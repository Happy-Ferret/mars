/*
 * MARS Long Distance Replication Software
 *
 * Copyright (C) 2010-2014 Thomas Schoebel-Theuer
 * Copyright (C) 2011-2014 1&1 Internet AG
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define XIO_DEBUGGING

/* This MUST be updated whenever INCOMPATIBLE changes are made to the
 * symlink tree in /mars/ .
 *
 * Just adding a new symlink is usually not "incompatible", if
 * other tools like marsadm just ignore it.
 *
 * "incompatible" means that something may BREAK.
 */
#define SYMLINK_TREE_VERSION		"0.1"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include <linux/genhd.h>
#include <linux/blkdev.h>

#include "light_strategy.h"

#include <linux/wait.h>

#include "../xio_bricks/lib_mapfree.h"

/*  used brick types */
#include "../xio_bricks/xio_server.h"
#include "../xio_bricks/xio_client.h"
#include "../xio_bricks/xio_copy.h"
#include "../xio_bricks/xio_bio.h"
#include "../xio_bricks/xio_sio.h"
#include "../xio_bricks/xio_trans_logger.h"
#include "../xio_bricks/xio_if.h"
#include "mars_proc.h"

#define REPLAY_TOLERANCE		(PAGE_SIZE + OVERHEAD)

/*  TODO: add human-readable timestamps */
#define XIO_INF_TO(channel, fmt, args...)				\
	({								\
		say_to(channel, SAY_INFO, "%s: " fmt, say_class[SAY_INFO], ##args);\
		XIO_INF(fmt, ##args);					\
	})

#define XIO_WRN_TO(channel, fmt, args...)				\
	({								\
		say_to(channel, SAY_WARN, "%s: " fmt, say_class[SAY_WARN], ##args);\
		XIO_WRN(fmt, ##args);					\
	})

#define XIO_ERR_TO(channel, fmt, args...)				\
	({								\
		say_to(channel, SAY_ERROR, "%s: " fmt, say_class[SAY_ERROR], ##args);\
		XIO_ERR(fmt, ##args);					\
	})

loff_t raw_total_space;
loff_t global_total_space;

loff_t raw_remaining_space;
loff_t global_remaining_space;

int global_logrot_auto = 32;

module_param_named(logrot_auto, global_logrot_auto, int, 0);

int global_free_space_0 = CONFIG_MARS_MIN_SPACE_0;

int global_free_space_1 = CONFIG_MARS_MIN_SPACE_1;

int global_free_space_2 = CONFIG_MARS_MIN_SPACE_2;

int global_free_space_3 = CONFIG_MARS_MIN_SPACE_3;

int global_free_space_4 = CONFIG_MARS_MIN_SPACE_4;

int _global_sync_want;
int global_sync_want;

int global_sync_nr;

int global_sync_limit;

int mars_rollover_interval = 3;

module_param_named(mars_rollover_interval, mars_rollover_interval, int, 0);

int mars_scan_interval = 5;

module_param_named(mars_scan_interval, mars_scan_interval, int, 0);

int mars_propagate_interval = 5;

module_param_named(mars_propagate_interval, mars_propagate_interval, int, 0);

int mars_sync_flip_interval = 60;

module_param_named(mars_sync_flip_interval, mars_sync_flip_interval, int, 0);

int mars_peer_abort = 7;

int mars_fast_fullsync = 1;

module_param_named(mars_fast_fullsync, mars_fast_fullsync, int, 0);

int xio_throttle_start = 60;

int xio_throttle_end = 90;

int mars_emergency_mode;

int mars_reset_emergency = 1;

int mars_keep_msg = 10;

#define MARS_SYMLINK_MAX		1023

struct key_value_pair {
	const char *key;
	char *val;
	char *old_val;
	unsigned long last_jiffies;
	struct timespec system_stamp;
	struct timespec lamport_stamp;
};

static inline
void clear_vals(struct key_value_pair *start)
{
	while (start->key) {
		brick_string_free(start->val);
		start->val = NULL;
		brick_string_free(start->old_val);
		start->old_val = NULL;
		start++;
	}
}

static
void show_vals(struct key_value_pair *start, const char *path, const char *add)
{
	while (start->key) {
		char *dst = path_make("%s/actual-%s/msg-%s%s", path, my_id(), add, start->key);

		/*  show the old message for some keep_time if no new one is available */
		if (!start->val && start->old_val &&
		    (long long)start->last_jiffies  + mars_keep_msg * HZ <= (long long)jiffies) {
			start->val = start->old_val;
			start->old_val = NULL;
		}
		if (start->val) {
			char *src = path_make("%ld.%09ld %ld.%09ld %s",
					      start->system_stamp.tv_sec, start->system_stamp.tv_nsec,
					      start->lamport_stamp.tv_sec, start->lamport_stamp.tv_nsec,
					      start->val);
			mars_symlink(src, dst, NULL, 0);
			brick_string_free(src);
			brick_string_free(start->old_val);
			start->old_val = start->val;
			start->val = NULL;
		} else {
			mars_symlink("OK", dst, NULL, 0);
			memset(&start->system_stamp, 0, sizeof(start->system_stamp));
			memset(&start->lamport_stamp, 0, sizeof(start->lamport_stamp));
			brick_string_free(start->old_val);
			start->old_val = NULL;
		}
		brick_string_free(dst);
		start++;
	}
}

static inline
void assign_keys(struct key_value_pair *start, const char **keys)
{
	while (*keys) {
		start->key = *keys;
		start++;
		keys++;
	}
}

static inline
struct key_value_pair *find_key(struct key_value_pair *start, const char *key)
{
	while (start->key) {
		if (!strcmp(start->key, key))
			return start;
		start++;
	}
	XIO_ERR("cannot find key '%s'\n", key);
	return NULL;
}

static
void _make_msg(int line, struct key_value_pair *pair, const char *fmt, ...)  __printf(3, 4);

static
void _make_msg(int line, struct key_value_pair *pair, const char *fmt, ...)
{
	int len;
	va_list args;

	if (unlikely(!pair || !pair->key)) {
		XIO_ERR("bad pointer %p at line %d\n", pair, line);
		goto out_return;
	}
	pair->last_jiffies = jiffies;
	if (!pair->val) {
		pair->val = brick_string_alloc(MARS_SYMLINK_MAX + 1);
		len = 0;
		if (!pair->system_stamp.tv_sec) {
			pair->system_stamp = CURRENT_TIME;
			get_lamport(&pair->lamport_stamp);
		}
	} else {
		len = strnlen(pair->val, MARS_SYMLINK_MAX);
		if (unlikely(len >= MARS_SYMLINK_MAX - 48))
			goto out_return;
		pair->val[len++] = ',';
	}

	va_start(args, fmt);
	vsnprintf(pair->val + len, MARS_SYMLINK_MAX - 1 - len, fmt, args);
	va_end(args);
out_return:;
}

#define make_msg(pair, fmt, args...)					\
	_make_msg(__LINE__, pair, fmt, ##args)

static
struct key_value_pair gbl_pairs[] = {
	{ NULL }
};

#define make_gbl_msg(key, fmt, args...)					\
	make_msg(find_key(gbl_pairs, key), fmt, ##args)

static
const char *rot_keys[] = {
	/*  from _update_version_link() */
	"err-versionlink-skip",
	/*  from _update_info() */
	"err-sequence-trash",
	/*  from _is_switchover_possible() */
	"inf-versionlink-not-yet-exist",
	"inf-versionlink-not-equal",
	"inf-replay-not-yet-finished",
	"err-bad-log-name",
	"err-log-not-contiguous",
	"err-versionlink-not-readable",
	"err-replaylink-not-readable",
	"err-splitbrain-detected",
	/*  from _update_file() */
	"inf-fetch",
	/*  from make_sync() */
	"inf-sync",
	/*  from make_log_step() */
	"wrn-log-consecutive",
	/*  from make_log_finalize() */
	"inf-replay-start",
	"wrn-space-low",
	"err-space-low",
	"err-emergency",
	"err-replay-stop",
	/*  from _check_logging_status() */
	"inf-replay-tolerance",
	"err-replay-size",
	NULL,
};

#define make_rot_msg(rot, key, fmt, args...)				\
	make_msg(find_key(&(rot)->msgs[0], key), fmt, ##args)

#define IS_EXHAUSTED()		   (mars_emergency_mode > 0)
#define IS_EMERGENCY_SECONDARY()   (mars_emergency_mode > 1)
#define IS_EMERGENCY_PRIMARY()	   (mars_emergency_mode > 2)
#define IS_JAMMED()		   (mars_emergency_mode > 3)

static
void _make_alivelink_str(const char *name, const char *src)
{
	char *dst = path_make("/mars/%s-%s", name, my_id());

	if (!src || !dst) {
		XIO_ERR("cannot make alivelink paths\n");
		goto err;
	}
	XIO_DBG("'%s' -> '%s'\n", src, dst);
	mars_symlink(src, dst, NULL, 0);
err:
	brick_string_free(dst);
}

static
void _make_alivelink(const char *name, loff_t val)
{
	char *src = path_make("%lld", val);

	_make_alivelink_str(name, src);
	brick_string_free(src);
}

static
int compute_emergency_mode(void)
{
	loff_t rest;
	loff_t present;
	loff_t limit = 0;
	int mode = 4;
	int this_mode = 0;

	mars_remaining_space("/mars", &raw_total_space, &raw_remaining_space);
	rest = raw_remaining_space;

#define CHECK_LIMIT(LIMIT_VAR)						\
do {									\
	if (LIMIT_VAR > 0)						\
		limit += (loff_t)LIMIT_VAR * 1024 * 1024;		\
	if (rest < limit && !this_mode) {				\
		this_mode = mode;					\
	}								\
	mode--;								\
} while (0)

	CHECK_LIMIT(global_free_space_4);
	CHECK_LIMIT(global_free_space_3);
	CHECK_LIMIT(global_free_space_2);
	CHECK_LIMIT(global_free_space_1);

	/* Decrease the emergeny mode only in single steps.
	 */
	if (mars_reset_emergency && mars_emergency_mode > 0 && mars_emergency_mode > this_mode)
		mars_emergency_mode--;
	else
		mars_emergency_mode = this_mode;
	_make_alivelink("emergency", mars_emergency_mode);

	rest -= limit;
	if (rest < 0)
		rest = 0;
	global_remaining_space = rest;
	_make_alivelink("rest-space", rest / (1024 * 1024));

	present = raw_total_space - limit;
	global_total_space = present;

	if (xio_throttle_start > 0 &&
	    xio_throttle_end > xio_throttle_start &&
	    present > 0) {
		loff_t percent_used = 100 - (rest * 100 / present);

		if (percent_used < xio_throttle_start)
			if_throttle_start_size = 0;
		else if (percent_used >= xio_throttle_end)
			if_throttle_start_size = 1;
		else
			if_throttle_start_size = (xio_throttle_end - percent_used) * 1024 / (xio_throttle_end - xio_throttle_start) + 1;
	}

	if (unlikely(present < global_free_space_0))
		return -ENOSPC;
	return 0;
}

/*****************************************************************/

static struct task_struct *main_thread;

typedef int (*light_worker_fn)(void *buf, struct mars_dent *dent);

struct light_class {
	char *cl_name;
	int    cl_len;
	char   cl_type;
	bool   cl_hostcontext;
	bool   cl_serial;
	bool   cl_use_channel;
	int    cl_father;

	light_worker_fn cl_prepare;
	light_worker_fn cl_forward;
	light_worker_fn cl_backward;
};

/*  the order is important! */
enum {
	/*  root element: this must have index 0 */
	CL_ROOT,
	/*  global ID */
	CL_UUID,
	/*  global userspace */
	CL_GLOBAL_USERSPACE,
	CL_GLOBAL_USERSPACE_ITEMS,
	/*  global todos */
	CL_GLOBAL_TODO,
	CL_GLOBAL_TODO_DELETE,
	CL_GLOBAL_TODO_DELETED,
	CL_DEFAULTS0,
	CL_DEFAULTS,
	CL_DEFAULTS_ITEMS0,
	CL_DEFAULTS_ITEMS,
	/*  replacement for DNS in kernelspace */
	CL_IPS,
	CL_PEERS,
	CL_GBL_ACTUAL,
	CL_GBL_ACTUAL_ITEMS,
	CL_ALIVE,
	CL_TIME,
	CL_TREE,
	CL_EMERGENCY,
	CL_REST_SPACE,
	/*  resource definitions */
	CL_RESOURCE,
	CL_RESOURCE_USERSPACE,
	CL_RESOURCE_USERSPACE_ITEMS,
	CL_RES_DEFAULTS0,
	CL_RES_DEFAULTS,
	CL_RES_DEFAULTS_ITEMS0,
	CL_RES_DEFAULTS_ITEMS,
	CL_TODO,
	CL_TODO_ITEMS,
	CL_ACTUAL,
	CL_ACTUAL_ITEMS,
	CL_DATA,
	CL_SIZE,
	CL_ACTSIZE,
	CL_PRIMARY,
	CL_CONNECT,
	CL_TRANSFER,
	CL_SYNC,
	CL_VERIF,
	CL_SYNCPOS,
	CL_VERSION,
	CL_LOG,
	CL_REPLAYSTATUS,
	CL_DEVICE,
	CL_MAXNR,
};

/*********************************************************************/

/*  needed for logfile rotation */

#define MAX_INFOS			4

struct mars_rotate {
	struct list_head rot_head;
	struct mars_global *global;
	struct copy_brick *sync_brick;
	struct mars_dent *replay_link;
	struct xio_brick *bio_brick;
	struct mars_dent *aio_dent;
	struct xio_brick *aio_brick;
	struct xio_info aio_info;
	struct trans_logger_brick *trans_brick;
	struct mars_dent *first_log;
	struct mars_dent *relevant_log;
	struct xio_brick *relevant_brick;
	struct mars_dent *next_relevant_log;
	struct xio_brick *next_relevant_brick;
	struct mars_dent *prev_log;
	struct mars_dent *next_log;
	struct mars_dent *syncstatus_dent;
	struct timespec sync_finish_stamp;
	struct if_brick *if_brick;
	const char *fetch_path;
	const char *fetch_peer;
	const char *preferred_peer;
	const char *parent_path;
	const char *parent_rest;
	const char *fetch_next_origin;
	struct say_channel *log_say;
	struct copy_brick *fetch_brick;
	struct rate_limiter replay_limiter;
	struct rate_limiter sync_limiter;
	struct rate_limiter fetch_limiter;
	int inf_prev_sequence;
	long long flip_start;
	loff_t dev_size;
	loff_t start_pos;
	loff_t end_pos;
	int max_sequence;
	int fetch_round;
	int fetch_serial;
	int fetch_next_serial;
	int split_brain_serial;
	int split_brain_round;
	int fetch_next_is_available;
	int relevant_serial;
	bool has_symlinks;
	bool res_shutdown;
	bool has_error;
	bool has_double_logfile;
	bool has_hole_logfile;
	bool allow_update;
	bool forbid_replay;
	bool replay_mode;
	bool todo_primary;
	bool is_primary;
	bool old_is_primary;
	bool created_hole;
	bool is_log_damaged;
	bool has_emergency;
	bool wants_sync;
	bool gets_sync;
	spinlock_t inf_lock;
	bool infs_is_dirty[MAX_INFOS];
	struct trans_logger_info infs[MAX_INFOS];
	struct key_value_pair msgs[sizeof(rot_keys) / sizeof(char *)];
};

static LIST_HEAD(rot_anchor);

/*********************************************************************/

/*  TUNING */

int mars_mem_percent = 20;

#define CONF_TRANS_SHADOW_LIMIT		(1024 * 128) /*  don't fill the hashtable too much */

#define CONF_TRANS_BATCHLEN		64
#define CONF_TRANS_PRIO			XIO_PRIO_HIGH
#define CONF_TRANS_LOG_READS		false

#define CONF_ALL_BATCHLEN		1
#define CONF_ALL_PRIO			XIO_PRIO_NORMAL

#define IF_SKIP_SYNC			true

#define IF_MAX_PLUGGED			10000
#define IF_READAHEAD			0

#define BIO_READAHEAD			0
#define BIO_NOIDLE			true
#define BIO_SYNC			true
#define BIO_UNPLUG			true

#define COPY_APPEND_MODE		0
#define COPY_PRIO			XIO_PRIO_LOW

static
int _set_trans_params(struct xio_brick *_brick, void *private)
{
	struct trans_logger_brick *trans_brick = (void *)_brick;

	if (_brick->type != (void *)&trans_logger_brick_type) {
		XIO_ERR("bad brick type\n");
		return -EINVAL;
	}
	if (!trans_brick->q_phase[1].q_ordering) {
		trans_brick->q_phase[0].q_batchlen = CONF_TRANS_BATCHLEN;
		trans_brick->q_phase[1].q_batchlen = CONF_ALL_BATCHLEN;
		trans_brick->q_phase[2].q_batchlen = CONF_ALL_BATCHLEN;
		trans_brick->q_phase[3].q_batchlen = CONF_ALL_BATCHLEN;

		trans_brick->q_phase[0].q_io_prio = CONF_TRANS_PRIO;
		trans_brick->q_phase[1].q_io_prio = CONF_ALL_PRIO;
		trans_brick->q_phase[2].q_io_prio = CONF_ALL_PRIO;
		trans_brick->q_phase[3].q_io_prio = CONF_ALL_PRIO;

		trans_brick->q_phase[1].q_ordering = true;
		trans_brick->q_phase[3].q_ordering = true;

		trans_brick->shadow_mem_limit = CONF_TRANS_SHADOW_LIMIT;
		trans_brick->log_reads = CONF_TRANS_LOG_READS;
	}
	XIO_INF("name = '%s' path = '%s'\n", _brick->brick_name, _brick->brick_path);
	return 1;
}

struct client_cookie {
	bool limit_mode;
	bool create_mode;
};

static
int _set_client_params(struct xio_brick *_brick, void *private)
{
	struct client_brick *client_brick = (void *)_brick;
	struct client_cookie *clc = private;

	client_brick->limit_mode = clc ? clc->limit_mode : false;
	client_brick->killme = true;
	XIO_INF("name = '%s' path = '%s'\n", _brick->brick_name, _brick->brick_path);
	return 1;
}

static
int _set_sio_params(struct xio_brick *_brick, void *private)
{
	struct sio_brick *sio_brick = (void *)_brick;

	if (_brick->type == (void *)&client_brick_type)
		return _set_client_params(_brick, private);
	if (_brick->type != (void *)&sio_brick_type) {
		XIO_ERR("bad brick type\n");
		return -EINVAL;
	}
	sio_brick->o_direct = false; /*  important! */
	sio_brick->o_fdsync = true;
	sio_brick->killme = true;
	XIO_INF("name = '%s' path = '%s'\n", _brick->brick_name, _brick->brick_path);
	return 1;
}

static
int _set_bio_params(struct xio_brick *_brick, void *private)
{
	struct bio_brick *bio_brick;

	if (_brick->type == (void *)&client_brick_type)
		return _set_client_params(_brick, private);
	if (_brick->type == (void *)&sio_brick_type)
		return _set_sio_params(_brick, private);
	if (_brick->type != (void *)&bio_brick_type) {
		XIO_ERR("bad brick type\n");
		return -EINVAL;
	}
	bio_brick = (void *)_brick;
	bio_brick->ra_pages = BIO_READAHEAD;
	bio_brick->do_noidle = BIO_NOIDLE;
	bio_brick->do_sync = BIO_SYNC;
	bio_brick->do_unplug = BIO_UNPLUG;
	bio_brick->killme = true;
	XIO_INF("name = '%s' path = '%s'\n", _brick->brick_name, _brick->brick_path);
	return 1;
}

static
int _set_if_params(struct xio_brick *_brick, void *private)
{
	struct if_brick *if_brick = (void *)_brick;
	struct mars_rotate *rot = private;

	if (_brick->type != (void *)&if_brick_type) {
		XIO_ERR("bad brick type\n");
		return -EINVAL;
	}
	if (!rot) {
		XIO_ERR("too early\n");
		return -EINVAL;
	}
	if (rot->dev_size <= 0) {
		XIO_ERR("dev_size = %lld\n", rot->dev_size);
		return -EINVAL;
	}
	if (if_brick->dev_size > 0 && rot->dev_size < if_brick->dev_size) {
		XIO_ERR("new dev size = %lld < old dev_size = %lld\n", rot->dev_size, if_brick->dev_size);
		return -EINVAL;
	}
	if_brick->dev_size = rot->dev_size;
	if_brick->max_plugged = IF_MAX_PLUGGED;
	if_brick->readahead = IF_READAHEAD;
	if_brick->skip_sync = IF_SKIP_SYNC;
	XIO_INF("name = '%s' path = '%s' size = %lld\n", _brick->brick_name, _brick->brick_path, if_brick->dev_size);
	return 1;
}

struct copy_cookie {
	const char *argv[2];
	const char *copy_path;
	loff_t start_pos;
	loff_t end_pos;
	bool keep_running;
	bool verify_mode;

	const char *fullpath[2];
	struct xio_output *output[2];
	struct xio_info info[2];
};

static
int _set_copy_params(struct xio_brick *_brick, void *private)
{
	struct copy_brick *copy_brick = (void *)_brick;
	struct copy_cookie *cc = private;
	int status = 1;

	if (_brick->type != (void *)&copy_brick_type) {
		XIO_ERR("bad brick type\n");
		status = -EINVAL;
		goto done;
	}
	copy_brick->append_mode = COPY_APPEND_MODE;
	copy_brick->io_prio = COPY_PRIO;
	copy_brick->verify_mode = cc->verify_mode;
	copy_brick->repair_mode = true;
	copy_brick->killme = true;
	XIO_INF("name = '%s' path = '%s'\n", _brick->brick_name, _brick->brick_path);

	/* Determine the copy area, switch on/off when necessary
	 */
	if (!copy_brick->power.button && copy_brick->power.off_led) {
		int i;

		copy_brick->copy_last = 0;
		for (i = 0; i < 2; i++) {
			status = cc->output[i]->ops->xio_get_info(cc->output[i], &cc->info[i]);
			if (status < 0) {
				XIO_WRN("cannot determine current size of '%s'\n", cc->argv[i]);
				goto done;
			}
			XIO_DBG("%d '%s' current_size = %lld\n", i, cc->fullpath[i], cc->info[i].current_size);
		}
		copy_brick->copy_start = cc->info[1].current_size;
		if (cc->start_pos != -1) {
			copy_brick->copy_start = cc->start_pos;
			if (unlikely(cc->start_pos > cc->info[0].current_size)) {
				XIO_ERR("bad start position %lld is larger than actual size %lld on '%s'\n",
					cc->start_pos,
					cc->info[0].current_size,
					cc->copy_path);
				status = -EINVAL;
				goto done;
			}
		}
		XIO_DBG("copy_start = %lld\n", copy_brick->copy_start);
		copy_brick->copy_end = cc->info[0].current_size;
		if (cc->end_pos != -1) {
			if (unlikely(cc->end_pos > copy_brick->copy_end)) {
				XIO_ERR("target size %lld is larger than actual size %lld on source\n",
					cc->end_pos,
					copy_brick->copy_end);
				status = -EINVAL;
				goto done;
			}
			copy_brick->copy_end = cc->end_pos;
			if (unlikely(cc->end_pos > cc->info[1].current_size)) {
				XIO_ERR("bad end position %lld is larger than actual size %lld on target\n",
					cc->end_pos,
					cc->info[1].current_size);
				status = -EINVAL;
				goto done;
			}
		}
		XIO_DBG("copy_end = %lld\n", copy_brick->copy_end);
		if (copy_brick->copy_start < copy_brick->copy_end) {
			status = 1;
			XIO_DBG("copy switch on\n");
		}
	} else if (copy_brick->power.button && copy_brick->power.on_led &&
		   !cc->keep_running &&
		   copy_brick->copy_last == copy_brick->copy_end && copy_brick->copy_end > 0) {
		status = 0;
		XIO_DBG("copy switch off\n");
	}

done:
	return status;
}

/*********************************************************************/

/*  internal helpers */

#define MARS_DELIM			','

static int _parse_args(struct mars_dent *dent, char *str, int count)
{
	int i;
	int status = -EINVAL;

	if (!str)
		goto done;
	if (!dent->d_args)
		dent->d_args = brick_strdup(str);
	for (i = 0; i < count; i++) {
		char *tmp;
		int len;

		if (!*str)
			goto done;
		if (i == count-1) {
			len = strlen(str);
		} else {
			char *tmp = strchr(str, MARS_DELIM);

			if (!tmp)
				goto done;
			len = (tmp - str);
		}
		brick_string_free(dent->d_argv[i]);
		tmp = brick_string_alloc(len + 1);
		dent->d_argv[i] = tmp;
		strncpy(dent->d_argv[i], str, len);
		dent->d_argv[i][len] = '\0';

		str += len;
		if (i != count-1)
			str++;
	}
	status = 0;
done:
	if (status < 0) {
		XIO_ERR("bad syntax '%s' (should have %d args), status = %d\n",
			dent->d_args ? dent->d_args : "",
			count,
			status);
	}
	return status;
}

static
int _check_switch(struct mars_global *global, const char *path)
{
	int status;
	int res = 0;
	struct mars_dent *allow_dent;

	/* Upon shutdown, treat all switches as "off"
	 */
	if (!global->global_power.button)
		goto done;

	allow_dent = mars_find_dent(global, path);
	if (!allow_dent || !allow_dent->link_val)
		goto done;
	status = kstrtoint(allow_dent->link_val, 10, &res);
	(void)status; /*  treat errors as if the switch were set to 0 */
	XIO_DBG("'%s' -> %d\n", path, res);

done:
	return res;
}

static
int _check_allow(struct mars_global *global, struct mars_dent *parent, const char *name)
{
	int res = 0;
	char *path = path_make("%s/todo-%s/%s", parent->d_path, my_id(), name);

	if (!path)
		goto done;

	res = _check_switch(global, path);

done:
	brick_string_free(path);
	return res;
}

#define skip_part(s) _skip_part(s, ',', ':')
#define skip_sect(s) _skip_part(s, ':', 0)
static inline
int _skip_part(const char *str, const char del1, const char del2)
{
	int len = 0;

	while (str[len] && str[len] != del1 && (!del2 || str[len] != del2))
		len++;
	return len;
}

static inline
int skip_dir(const char *str)
{
	int len = 0;
	int res = 0;

	for (len = 0; str[len]; len++)
		if (str[len] == '/')
			res = len + 1;
	return res;
}

static
int parse_logfile_name(const char *str, int *seq, const char **host)
{
	char *_host;
	int count;
	int len = 0;
	int len_host;

	*seq = 0;
	*host = NULL;

	count = sscanf(str, "log-%d-%n", seq, &len);
	if (unlikely(count != 1)) {
		XIO_ERR("bad logfile name '%s', count=%d, len=%d\n", str, count, len);
		return 0;
	}

	_host = brick_strdup(str + len);

	len_host = skip_part(_host);
	_host[len_host] = '\0';
	*host = _host;
	len += len_host;

	return len;
}

static
int compare_replaylinks(struct mars_rotate *rot, const char *hosta, const char *hostb)
{
	const char *linka = path_make("%s/replay-%s", rot->parent_path, hosta);
	const char *linkb = path_make("%s/replay-%s", rot->parent_path, hostb);
	const char *a = NULL;
	const char *b = NULL;
	int seqa;
	int seqb;
	int posa;
	int posb;
	loff_t offa = 0;
	loff_t offb = -1;
	loff_t taila = 0;
	loff_t tailb = -1;
	int count;
	int res = -2;

	if (unlikely(!linka || !linkb)) {
		XIO_ERR("nen MEM");
		goto done;
	}

	a = mars_readlink(linka);
	if (unlikely(!a || !a[0])) {
		XIO_ERR_TO(rot->log_say, "cannot read replaylink '%s'\n", linka);
		goto done;
	}
	b = mars_readlink(linkb);
	if (unlikely(!b || !b[0])) {
		XIO_ERR_TO(rot->log_say, "cannot read replaylink '%s'\n", linkb);
		goto done;
	}

	count = sscanf(a, "log-%d-%n", &seqa, &posa);
	if (unlikely(count != 1))
		XIO_ERR_TO(rot->log_say, "replay link '%s' -> '%s' is malformed\n", linka, a);
	count = sscanf(b, "log-%d-%n", &seqb, &posb);
	if (unlikely(count != 1))
		XIO_ERR_TO(rot->log_say, "replay link '%s' -> '%s' is malformed\n", linkb, b);

	if (seqa < seqb) {
		res = -1;
		goto done;
	} else if (seqa > seqb) {
		res = 1;
		goto done;
	}

	posa += skip_part(a + posa);
	posb += skip_part(b + posb);
	if (unlikely(!a[posa++]))
		XIO_ERR_TO(rot->log_say, "replay link '%s' -> '%s' is malformed\n", linka, a);
	if (unlikely(!b[posb++]))
		XIO_ERR_TO(rot->log_say, "replay link '%s' -> '%s' is malformed\n", linkb, b);

	count = sscanf(a + posa, "%lld,%lld", &offa, &taila);
	if (unlikely(count != 2))
		XIO_ERR_TO(rot->log_say, "replay link '%s' -> '%s' is malformed\n", linka, a);
	count = sscanf(b + posb, "%lld,%lld", &offb, &tailb);
	if (unlikely(count != 2))
		XIO_ERR_TO(rot->log_say, "replay link '%s' -> '%s' is malformed\n", linkb, b);

	if (posa < posb)
		res = -1;
	else if (posa > posb)
		res = 1;
	else
		res = 0;

done:
	brick_string_free(a);
	brick_string_free(b);
	brick_string_free(linka);
	brick_string_free(linkb);
	return res;
}

/*********************************************************************/

/*  status display */

static
int _update_link_when_necessary(struct mars_rotate *rot, const char *type, const char *old, const char *new)
{
	char *check = NULL;
	int status = -EINVAL;
	bool res = false;

	if (unlikely(!old || !new))
		goto out;

	/* Check whether something really has changed (avoid
	 * useless/disturbing timestamp updates)
	 */
	check = mars_readlink(new);
	if (check && !strcmp(check, old)) {
		XIO_DBG("%s symlink '%s' -> '%s' has not changed\n", type, old, new);
		res = 0;
		goto out;
	}

	status = mars_symlink(old, new, NULL, 0);
	if (unlikely(status < 0)) {
		XIO_ERR_TO(rot->log_say,
			"cannot create %s symlink '%s' -> '%s' status = %d\n",
			type,
			old,
			new,
			status);
	} else {
		res = 1;
		XIO_DBG("made %s symlink '%s' -> '%s' status = %d\n", type, old, new, status);
	}

out:
	brick_string_free(check);
	return res;
}

static
int _update_replay_link(struct mars_rotate *rot, struct trans_logger_info *inf)
{
	char *old = NULL;
	char *new = NULL;
	int res = 0;

	old = path_make("log-%09d-%s,%lld,%lld",
		inf->inf_sequence,
		inf->inf_host,
		inf->inf_min_pos,
		inf->inf_max_pos - inf->inf_min_pos);
	if (!old)
		goto out;
	new = path_make("%s/replay-%s", rot->parent_path, my_id());
	if (!new)
		goto out;

	res = _update_link_when_necessary(rot, "replay", old, new);

out:
	brick_string_free(new);
	brick_string_free(old);
	return res;
}

static
int _update_version_link(struct mars_rotate *rot, struct trans_logger_info *inf)
{
	char *data = brick_string_alloc(0);
	char *old = brick_string_alloc(0);
	char *new = NULL;
	unsigned char *digest = brick_string_alloc(0);
	char *prev = NULL;
	char *prev_link = NULL;
	char *prev_digest = NULL;
	int len;
	int i;
	int res = 0;

	if (likely(inf->inf_sequence > 1)) {
		if (unlikely((inf->inf_sequence < rot->inf_prev_sequence ||
			      inf->inf_sequence > rot->inf_prev_sequence + 1) &&
			     rot->inf_prev_sequence != 0)) {
			char *skip_path = path_make("%s/skip-check-%s", rot->parent_path, my_id());
			char *skip_link = mars_readlink(skip_path);
			char *msg = "";
			int skip_nr = -1;
			int nr_char = 0;

			if (likely(skip_link && skip_link[0])) {
				int status = sscanf(skip_link, "%d%n", &skip_nr, &nr_char);

				(void)status; /* keep msg empty in case of errors */
				msg = skip_link + nr_char;
			}
			brick_string_free(skip_path);
			if (likely(skip_nr != inf->inf_sequence)) {
				XIO_ERR_TO(rot->log_say,
					"SKIP in sequence numbers detected: %d != %d + 1\n",
					inf->inf_sequence,
					rot->inf_prev_sequence);
				make_rot_msg(rot,
					"err-versionlink-skip",
					"SKIP in sequence numbers detected: %d != %d + 1",
					inf->inf_sequence,
					rot->inf_prev_sequence);
				brick_string_free(skip_link);
				goto out;
			}
			XIO_WRN_TO(rot->log_say,
				    "you explicitly requested to SKIP sequence numbers from %d to %d%s\n",
				    rot->inf_prev_sequence, inf->inf_sequence, msg);
			brick_string_free(skip_link);
		}
		prev = path_make("%s/version-%09d-%s", rot->parent_path, inf->inf_sequence - 1, my_id());
		if (unlikely(!prev)) {
			XIO_ERR("no MEM\n");
			goto out;
		}
		prev_link = mars_readlink(prev);
		rot->inf_prev_sequence = inf->inf_sequence;
	}

	len = sprintf(data,
		"%d,%s,%lld:%s",
		inf->inf_sequence,
		inf->inf_host,
		inf->inf_log_pos,
		prev_link ? prev_link : "");

	XIO_DBG("data = '%s' len = %d\n", data, len);

	xio_digest(digest, data, len);

	len = 0;
	for (i = 0; i < xio_digest_size; i++)
		len += sprintf(old + len, "%02x", digest[i]);

	if (likely(prev_link && prev_link[0])) {
		char *tmp;

		prev_digest = brick_strdup(prev_link);
		/*  take the part before ':' */
		for (tmp = prev_digest; *tmp; tmp++)
			if (*tmp == ':')
				break;
		*tmp = '\0';
	}

	len += sprintf(old + len,
		",log-%09d-%s,%lld:%s",
		inf->inf_sequence,
		inf->inf_host,
		inf->inf_log_pos,
		prev_digest ? prev_digest : "");

	new = path_make("%s/version-%09d-%s", rot->parent_path, inf->inf_sequence, my_id());
	if (!new) {
		XIO_ERR("no MEM\n");
		goto out;
	}

	res = _update_link_when_necessary(rot, "version", old, new);

out:
	brick_string_free(new);
	brick_string_free(prev);
	brick_string_free(data);
	brick_string_free(digest);
	brick_string_free(old);
	brick_string_free(prev_link);
	brick_string_free(prev_digest);
	return res;
}

static
void _update_info(struct trans_logger_info *inf)
{
	struct mars_rotate *rot = inf->inf_private;
	int hash;
	unsigned long flags;

	if (unlikely(!rot)) {
		XIO_ERR("rot is NULL\n");
		goto done;
	}

	XIO_DBG("inf = %p '%s' seq = %d min_pos = %lld max_pos = %lld log_pos = %lld is_replaying = %d is_logging = %d\n",
		 inf,
		 inf->inf_host,
		 inf->inf_sequence,
		 inf->inf_min_pos,
		 inf->inf_max_pos,
		 inf->inf_log_pos,
		 inf->inf_is_replaying,
		 inf->inf_is_logging);

	hash = inf->inf_sequence % MAX_INFOS;
	if (unlikely(rot->infs_is_dirty[hash])) {
		if (unlikely(rot->infs[hash].inf_sequence != inf->inf_sequence)) {
			XIO_ERR_TO(rot->log_say,
				"buffer %d: sequence trash %d -> %d. is the mar_light thread hanging?\n",
				hash,
				rot->infs[hash].inf_sequence,
				inf->inf_sequence);
			make_rot_msg(rot,
				"err-sequence-trash",
				"buffer %d: sequence trash %d -> %d",
				hash,
				rot->infs[hash].inf_sequence,
				inf->inf_sequence);
		} else {
			XIO_DBG("buffer %d is overwritten (sequence=%d)\n", hash, inf->inf_sequence);
		}
	}

	spin_lock_irqsave(&rot->inf_lock, flags);
	memcpy(&rot->infs[hash], inf, sizeof(struct trans_logger_info));
	rot->infs_is_dirty[hash] = true;
	spin_unlock_irqrestore(&rot->inf_lock, flags);

	local_trigger();
done:;
}

static
void write_info_links(struct mars_rotate *rot)
{
	struct trans_logger_info inf;
	int count = 0;

	for (;;) {
		unsigned long flags;
		int hash = -1;
		int min = 0;
		int i;

		spin_lock_irqsave(&rot->inf_lock, flags);
		for (i = 0; i < MAX_INFOS; i++) {
			if (!rot->infs_is_dirty[i])
				continue;
			if (!min || min > rot->infs[i].inf_sequence) {
				min = rot->infs[i].inf_sequence;
				hash = i;
			}
		}

		if (hash < 0) {
			spin_unlock_irqrestore(&rot->inf_lock, flags);
			break;
		}

		rot->infs_is_dirty[hash] = false;
		memcpy(&inf, &rot->infs[hash], sizeof(struct trans_logger_info));
		spin_unlock_irqrestore(&rot->inf_lock, flags);

		XIO_DBG("seq = %d min_pos = %lld max_pos = %lld log_pos = %lld is_replaying = %d is_logging = %d\n",
			 inf.inf_sequence,
			 inf.inf_min_pos,
			 inf.inf_max_pos,
			 inf.inf_log_pos,
			 inf.inf_is_replaying,
			 inf.inf_is_logging);

		if (inf.inf_is_logging || inf.inf_is_replaying)
			count += _update_replay_link(rot, &inf);
		if (inf.inf_is_logging || inf.inf_is_replaying)
			count += _update_version_link(rot, &inf);
	}
	if (count) {
		if (inf.inf_min_pos == inf.inf_max_pos)
			local_trigger();
		remote_trigger();
	}
}

static
void _make_new_replaylink(struct mars_rotate *rot, char *new_host, int new_sequence, loff_t end_pos)
{
	struct trans_logger_info inf = {
		.inf_private = rot,
		.inf_sequence = new_sequence,
		.inf_min_pos = 0,
		.inf_max_pos = 0,
		.inf_log_pos = end_pos,
		.inf_is_replaying = true,
	};
	strncpy(inf.inf_host, new_host, sizeof(inf.inf_host));

	XIO_DBG("new_host = '%s' new_sequence = %d end_pos = %lld\n", new_host, new_sequence, end_pos);

	_update_replay_link(rot, &inf);
	_update_version_link(rot, &inf);

	local_trigger();
	remote_trigger();
}

static
int __show_actual(const char *path, const char *name, int val)
{
	char *src;
	char *dst = NULL;
	int status = -EINVAL;

	src = path_make("%d", val);
	dst = path_make("%s/actual-%s/%s", path, my_id(), name);
	status = -ENOMEM;
	if (!dst)
		goto done;

	XIO_DBG("symlink '%s' -> '%s'\n", dst, src);
	status = mars_symlink(src, dst, NULL, 0);

done:
	brick_string_free(src);
	brick_string_free(dst);
	return status;
}

static inline
int _show_actual(const char *path, const char *name, bool val)
{
	return __show_actual(path, name, val ? 1 : 0);
}

static
void _show_primary(struct mars_rotate *rot, struct mars_dent *parent)
{
	int status;

	if (!rot || !parent)
		goto out_return;
	status = _show_actual(parent->d_path, "is-primary", rot->is_primary);
	if (rot->is_primary != rot->old_is_primary) {
		rot->old_is_primary = rot->is_primary;
		remote_trigger();
	}
out_return:;
}

static
void _show_brick_status(struct xio_brick *test, bool shutdown)
{
	const char *path;
	char *src;
	char *dst;
	int status;

	path = test->brick_path;
	if (!path) {
		XIO_WRN("bad path\n");
		goto out_return;
	}
	if (*path != '/') {
		XIO_WRN("bogus path '%s'\n", path);
		goto out_return;
	}

	src = (test->power.on_led && !shutdown) ? "1" : "0";
	dst = backskip_replace(path, '/', true, "/actual-%s/", my_id());
	if (!dst)
		goto out_return;

	status = mars_symlink(src, dst, NULL, 0);
	XIO_DBG("status symlink '%s' -> '%s' status = %d\n", dst, src, status);
	brick_string_free(dst);
out_return:;
}

static
void _show_status_all(struct mars_global *global)
{
	struct list_head *tmp;

	down_read(&global->brick_mutex);
	for (tmp = global->brick_anchor.next; tmp != &global->brick_anchor; tmp = tmp->next) {
		struct xio_brick *test;

		test = container_of(tmp, struct xio_brick, global_brick_link);
		if (!test->show_status)
			continue;
		_show_brick_status(test, false);
	}
	up_read(&global->brick_mutex);
}

static
void _show_rate(struct mars_rotate *rot, struct rate_limiter *limiter, const char *name)
{
	rate_limit(limiter, 0);
	__show_actual(rot->parent_path, name, limiter->lim_rate);
}

/*********************************************************************/

static
int __make_copy(
		struct mars_global *global,
		struct mars_dent *belongs,
		const char *switch_path,
		const char *copy_path,
		const char *parent,
		const char *argv[],
		struct key_value_pair *msg_pair,
		loff_t start_pos, /*  -1 means at EOF of source */
		loff_t end_pos,   /*  -1 means at EOF of target */
		bool keep_running,
		bool verify_mode,
		bool limit_mode,
		bool space_using_mode,
		struct copy_brick **__copy)
{
	struct xio_brick *copy;
	struct copy_cookie cc = {};

	struct client_cookie clc[2] = {
		{
			.limit_mode = limit_mode,
		},
		{
			.limit_mode = limit_mode,
			.create_mode = true,
		},
	};
	int i;
	bool switch_copy;
	int status = -EINVAL;

	if (!switch_path || !global)
		goto done;

	/*  don't generate empty aio files if copy does not yet exist */
	switch_copy = _check_switch(global, switch_path);
	copy = mars_find_brick(global, &copy_brick_type, copy_path);
	if (!copy && !switch_copy)
		goto done;

	/*  create/find predecessor aio bricks */
	for (i = 0; i < 2; i++) {
		struct xio_brick *aio;

		cc.argv[i] = argv[i];
		if (parent) {
			cc.fullpath[i] = path_make("%s/%s", parent, argv[i]);
			if (!cc.fullpath[i]) {
				XIO_ERR("cannot make path '%s/%s'\n", parent, argv[i]);
				goto done;
			}
		} else {
			cc.fullpath[i] = argv[i];
		}

		aio =
			make_brick_all(global,
				       NULL,
				       _set_bio_params,
				       &clc[i],
				       NULL,
				       (const struct generic_brick_type *)&bio_brick_type,
				       (const struct generic_brick_type*[]){},
				       switch_copy || (copy && !copy->power.off_led) ? 2 : -1,
				       cc.fullpath[i],
				       (const char *[]){},
				       0);
		if (!aio) {
			XIO_DBG("cannot instantiate '%s'\n", cc.fullpath[i]);
			make_msg(msg_pair, "cannot instantiate '%s'", cc.fullpath[i]);
			goto done;
		}
		cc.output[i] = aio->outputs[0];
		/* When switching off, use a short timeout for aborting.
		 * Important on very slow networks (since a large number
		 * of requests may be pending).
		 */
		aio->power.io_timeout = switch_copy ? 0 : 1;
	}

	cc.copy_path = copy_path;
	cc.start_pos = start_pos;
	cc.end_pos = end_pos;
	cc.keep_running = keep_running;
	cc.verify_mode = verify_mode;

	copy =
		make_brick_all(global,
			       belongs,
			       _set_copy_params,
			       &cc,
			       cc.fullpath[1],
			       (const struct generic_brick_type *)&copy_brick_type,
			       (const struct generic_brick_type*[]){NULL, NULL, NULL, NULL},
			       (!switch_copy || (IS_EMERGENCY_PRIMARY() && !space_using_mode)) ? -1 : 2,
			       "%s",
			       (const char *[]){"%s", "%s", "%s", "%s"},
			       4,
			       copy_path,
			       cc.fullpath[0],
			       cc.fullpath[0],
			       cc.fullpath[1],
			       cc.fullpath[1]);
	if (copy) {
		struct copy_brick *_copy = (void *)copy;

		copy->show_status = _show_brick_status;
		make_msg(msg_pair,
			 "from = '%s' to = '%s' on = %d start_pos = %lld end_pos = %lld actual_pos = %lld actual_stamp = %ld.%09ld rate = %d read_fly = %d write_fly = %d error_code = %d nr_errors = %d",
			 argv[0],
			 argv[1],
			 _copy->power.on_led,
			 _copy->copy_start,
			 _copy->copy_end,
			 _copy->copy_last,
			 _copy->copy_last_stamp.tv_sec, _copy->copy_last_stamp.tv_nsec,
			 _copy->copy_limiter ? _copy->copy_limiter->lim_rate : 0,
			 atomic_read(&_copy->copy_read_flight),
			 atomic_read(&_copy->copy_write_flight),
			 _copy->copy_error,
			 _copy->copy_error_count);
	}
	if (__copy)
		*__copy = (void *)copy;

	status = 0;

done:
	XIO_DBG("status = %d\n", status);
	for (i = 0; i < 2; i++) {
		if (cc.fullpath[i] && cc.fullpath[i] != argv[i])
			brick_string_free(cc.fullpath[i]);
	}
	return status;
}

/*********************************************************************/

/*  remote workers */

static
rwlock_t peer_lock = __RW_LOCK_UNLOCKED(&peer_lock);

static
struct list_head peer_anchor = LIST_HEAD_INIT(peer_anchor);

struct mars_peerinfo {
	struct mars_global *global;
	char *peer;
	char *path;
	struct xio_socket socket;
	struct task_struct *peer_thread;
	spinlock_t lock;
	struct list_head peer_head;
	struct list_head remote_dent_list;
	unsigned long last_remote_jiffies;
	int maxdepth;
	bool to_remote_trigger;
	bool from_remote_trigger;
};

static
struct mars_peerinfo *find_peer(const char *peer_name)
{
	struct list_head *tmp;
	struct mars_peerinfo *res = NULL;
	unsigned long flags;

	read_lock_irqsave(&peer_lock, flags);
	for (tmp = peer_anchor.next; tmp != &peer_anchor; tmp = tmp->next) {
		struct mars_peerinfo *peer = container_of(tmp, struct mars_peerinfo, peer_head);

		if (!strcmp(peer->peer, peer_name)) {
			res = peer;
			break;
		}
	}
	read_unlock_irqrestore(&peer_lock, flags);

	return res;
}

static
bool _is_usable_dir(const char *name)
{
	if (!strncmp(name, "resource-", 9)
	   || !strncmp(name, "todo-", 5)
	   || !strncmp(name, "actual-", 7)
	   || !strncmp(name, "defaults", 8)
	   ) {
		return true;
	}
	return false;
}

static
bool _is_peer_logfile(const char *name, const char *id)
{
	int len = strlen(name);
	int idlen = id ? strlen(id) : 4 + 9 + 1;

	if (len <= idlen ||
	   strncmp(name, "log-", 4) != 0) {
		XIO_DBG("not a logfile at all: '%s'\n", name);
		return false;
	}
	if (id &&
	   name[len - idlen - 1] == '-' &&
	   strncmp(name + len - idlen, id, idlen) == 0) {
		XIO_DBG("not a peer logfile: '%s'\n", name);
		return false;
	}
	XIO_DBG("found peer logfile: '%s'\n", name);
	return true;
}

static
int _update_file(struct mars_dent *parent,
	const char *switch_path,
	const char *copy_path,
	const char *file,
	const char *peer,
	loff_t end_pos)
{
	struct mars_rotate *rot = parent->d_private;
	struct mars_global *global = rot->global;
	const char *tmp = path_make("%s@%s:%d", file, peer, xio_net_default_port + 1);
	const char *argv[2] = { tmp, file };
	struct copy_brick *copy = NULL;
	struct key_value_pair *msg_pair = find_key(rot->msgs, "inf-fetch");
	bool do_start = true;
	int status = -ENOMEM;

	if (unlikely(!tmp || !global))
		goto done;

	rot->fetch_round = 0;

	if (rot->todo_primary | rot->is_primary) {
		XIO_DBG("disallowing fetch, todo_primary=%d is_primary=%d\n", rot->todo_primary, rot->is_primary);
		make_msg(msg_pair,
			"disallowing fetch (todo_primary=%d is_primary=%d)",
			rot->todo_primary,
			rot->is_primary);
		do_start = false;
	}
	if (do_start && !strcmp(peer, "(none)")) {
		XIO_DBG("disabling fetch from unspecified peer / no primary designated\n");
		make_msg(msg_pair, "disabling fetch from unspecified peer / no primary designated");
		do_start = false;
	}
	if (do_start && !global->global_power.button) {
		XIO_DBG("disabling fetch due to rmmod\n");
		make_msg(msg_pair, "disabling fetch due to rmmod");
		do_start = false;
	}
#if 0
	/* Disabled for now. Re-enable this code after a new feature has been
	 * implemented: when pause-replay is given, /dev/mars/mydata should
	 * appear in _readonly_ form.
	 * The idea is to _not_ disable the fetch during this!
	 * You may draw a backup from the readonly device without losing your
	 * redundancy, because the transactions logs will contiue to be updated.
	 * Until the new feature is implemented, use
	 * "marsadm pause-replay $res; marsadm detach $res; mount -o ro /dev/lv/$res"
	 * as a workaround.
	 */
	if (do_start && !_check_allow(global, parent, "attach")) {
		XIO_DBG("disabling fetch due to detach\n");
		make_msg(msg_pair, "disabling fetch due to detach");
		do_start = false;
	}
#endif
	if (do_start && !_check_allow(global, parent, "connect")) {
		XIO_DBG("disabling fetch due to disconnect\n");
		make_msg(msg_pair, "disabling fetch due to disconnect");
		do_start = false;
	}

	XIO_DBG("src = '%s' dst = '%s'\n", tmp, file);
	status = __make_copy(global,
		NULL,
		do_start ? switch_path : "",
		copy_path,
		NULL,
		argv,
		msg_pair,
		-1,
		-1,
		false,
		false,
		false,
		true,
		&copy);
	if (status >= 0 && copy) {
		copy->copy_limiter = &rot->fetch_limiter;
		/*  FIXME: code is dead */
		if (copy->append_mode && copy->power.on_led &&
		    end_pos > copy->copy_end) {
			XIO_DBG("appending to '%s' %lld => %lld\n", copy_path, copy->copy_end, end_pos);
			/*  FIXME: use corrected length from xio_get_info() / see _set_copy_params() */
			copy->copy_end = end_pos;
		}
	}

done:
	brick_string_free(tmp);
	return status;
}

static
int check_logfile(const char *peer,
	struct mars_dent *remote_dent,
	struct mars_dent *local_dent,
	struct mars_dent *parent,
	loff_t dst_size)
{
	loff_t src_size = remote_dent->stat_val.size;
	struct mars_rotate *rot;
	const char *switch_path = NULL;
	struct copy_brick *fetch_brick;
	int status = 0;

	/*  correct the remote size when necessary */
	if (remote_dent->d_corr_B > 0 && remote_dent->d_corr_B < src_size) {
		XIO_DBG("logfile '%s' correcting src_size from %lld to %lld\n",
			remote_dent->d_path,
			src_size,
			remote_dent->d_corr_B);
		src_size = remote_dent->d_corr_B;
	}

	/*  plausibility checks */
	if (unlikely(dst_size > src_size)) {
		XIO_WRN("my local copy is larger than the remote one, ignoring\n");
		status = -EINVAL;
		goto done;
	}

	/*  check whether we are participating in that resource */
	rot = parent->d_private;
	if (!rot) {
		XIO_WRN("parent has no rot info\n");
		status = -EINVAL;
		goto done;
	}
	if (!rot->fetch_path) {
		XIO_WRN("parent has no fetch_path\n");
		status = -EINVAL;
		goto done;
	}

	/*  bookkeeping for serialization of logfile updates */
	if (remote_dent->d_serial > rot->fetch_serial) {
		rot->fetch_next_is_available++;
		if (!rot->fetch_next_serial || !rot->fetch_next_origin) {
			rot->fetch_next_serial = remote_dent->d_serial;
			rot->fetch_next_origin = brick_strdup(remote_dent->d_rest);
		} else if (rot->fetch_next_serial == remote_dent->d_serial && strcmp(rot->fetch_next_origin,
			remote_dent->d_rest)) {
			rot->split_brain_round = 0;
			rot->split_brain_serial = remote_dent->d_serial;
			XIO_WRN("SPLIT BRAIN (logfiles from '%s' and '%s' with same serial number %d) detected!\n",
				 rot->fetch_next_origin, remote_dent->d_rest, rot->split_brain_serial);
		}
	}

	/*  check whether connection is allowed */
	switch_path = path_make("%s/todo-%s/connect", parent->d_path, my_id());

	/*  check whether copy is necessary */
	fetch_brick = rot->fetch_brick;
	XIO_DBG("fetch_brick = %p (remote '%s' %d) fetch_serial = %d\n",
		fetch_brick,
		remote_dent->d_path,
		remote_dent->d_serial,
		rot->fetch_serial);
	if (fetch_brick) {
		if (remote_dent->d_serial == rot->fetch_serial && rot->fetch_peer && !strcmp(peer, rot->fetch_peer)) {
			/*  treat copy brick instance underway */
			status = _update_file(parent,
				switch_path,
				rot->fetch_path,
				remote_dent->d_path,
				peer,
				src_size);
			XIO_DBG("re-update '%s' from peer '%s' status = %d\n", remote_dent->d_path, peer, status);
		}
	} else if (!rot->fetch_serial && rot->allow_update &&
		   !rot->is_primary && !rot->old_is_primary &&
		   (!rot->preferred_peer || !strcmp(rot->preferred_peer, peer)) &&
		   (!rot->split_brain_serial || remote_dent->d_serial < rot->split_brain_serial) &&
		   (dst_size < src_size || !local_dent)) {
		/*  start copy brick instance */
		status = _update_file(parent, switch_path, rot->fetch_path, remote_dent->d_path, peer, src_size);
		XIO_DBG("update '%s' from peer '%s' status = %d\n", remote_dent->d_path, peer, status);
		if (likely(status >= 0)) {
			rot->fetch_serial = remote_dent->d_serial;
			rot->fetch_next_is_available = 0;
			brick_string_free(rot->fetch_peer);
			rot->fetch_peer = brick_strdup(peer);
		}
	} else {
		XIO_DBG("allow_update = %d src_size = %lld dst_size = %lld local_dent = %p\n",
			rot->allow_update,
			src_size,
			dst_size,
			local_dent);
	}

done:
	brick_string_free(switch_path);
	return status;
}

static
int run_bone(struct mars_peerinfo *peer, struct mars_dent *remote_dent)
{
	int status = 0;
	struct kstat local_stat = {};
	const char *marker_path = NULL;
	bool stat_ok;
	bool update_mtime = true;
	bool update_ctime = true;
	bool run_trigger = false;

	if (!strncmp(remote_dent->d_name, ".tmp", 4))
		goto done;
	if (!strncmp(remote_dent->d_name, ".deleted-", 9))
		goto done;
	if (!strncmp(remote_dent->d_name, "ignore", 6))
		goto done;

	/*  create / check markers (prevent concurrent updates) */
	if (remote_dent->link_val && !strncmp(remote_dent->d_path, "/mars/todo-global/delete-", 25)) {
		marker_path = backskip_replace(remote_dent->link_val, '/', true, "/.deleted-");
		if (mars_stat(marker_path, &local_stat, true) < 0 ||
		    timespec_compare(&remote_dent->stat_val.mtime, &local_stat.mtime) > 0) {
			XIO_DBG("creating / updating marker '%s' mtime=%lu.%09lu\n",
				 marker_path, remote_dent->stat_val.mtime.tv_sec, remote_dent->stat_val.mtime.tv_nsec);
			mars_symlink("1", marker_path, &remote_dent->stat_val.mtime, 0);
		}
		if (remote_dent->d_serial < peer->global->deleted_my_border) {
			XIO_DBG("ignoring deletion '%s' at border %d\n",
				remote_dent->d_path,
				peer->global->deleted_my_border);
			goto done;
		}
	} else {
		/*  check marker preventing concurrent updates from remote hosts when deletes are in progress */
		marker_path = backskip_replace(remote_dent->d_path, '/', true, "/.deleted-");
		if (mars_stat(marker_path, &local_stat, true) >= 0) {
			if (timespec_compare(&remote_dent->stat_val.mtime, &local_stat.mtime) <= 0) {
				XIO_DBG("marker '%s' exists, ignoring '%s' (new mtime=%lu.%09lu, marker mtime=%lu.%09lu)\n",
					 marker_path, remote_dent->d_path,
					 remote_dent->stat_val.mtime.tv_sec, remote_dent->stat_val.mtime.tv_nsec,
					 local_stat.mtime.tv_sec, local_stat.mtime.tv_nsec);
				goto done;
			} else {
				XIO_DBG("marker '%s' exists, overwriting '%s' (new mtime=%lu.%09lu, marker mtime=%lu.%09lu)\n",
					 marker_path, remote_dent->d_path,
					 remote_dent->stat_val.mtime.tv_sec, remote_dent->stat_val.mtime.tv_nsec,
					 local_stat.mtime.tv_sec, local_stat.mtime.tv_nsec);
			}
		}
	}

	status = mars_stat(remote_dent->d_path, &local_stat, true);
	stat_ok = (status >= 0);

	if (stat_ok) {
		update_mtime = timespec_compare(&remote_dent->stat_val.mtime, &local_stat.mtime) > 0;
		update_ctime = timespec_compare(&remote_dent->stat_val.ctime, &local_stat.ctime) > 0;

	}

	if (S_ISDIR(remote_dent->stat_val.mode)) {
		if (!_is_usable_dir(remote_dent->d_name)) {
			XIO_DBG("ignoring directory '%s'\n", remote_dent->d_path);
			goto done;
		}
		if (!stat_ok) {
			status = mars_mkdir(remote_dent->d_path);
			XIO_DBG("create directory '%s' status = %d\n", remote_dent->d_path, status);
		}
	} else if (S_ISLNK(remote_dent->stat_val.mode) && remote_dent->link_val) {
		if (!stat_ok || update_mtime) {
			status = mars_symlink(remote_dent->link_val,
				remote_dent->d_path,
				&remote_dent->stat_val.mtime,
				__kuid_val(remote_dent->stat_val.uid));
			XIO_DBG("create symlink '%s' -> '%s' status = %d\n",
				remote_dent->d_path,
				remote_dent->link_val,
				status);
			run_trigger = true;
		}
	} else if (S_ISREG(remote_dent->stat_val.mode) && _is_peer_logfile(remote_dent->d_name, my_id())) {
		const char *parent_path = backskip_replace(remote_dent->d_path, '/', false, "");

		if (likely(parent_path)) {
			struct mars_dent *parent = mars_find_dent(peer->global, parent_path);

			if (unlikely(!parent)) {
				XIO_DBG("ignoring non-existing local resource '%s'\n", parent_path);
			/*  don't copy old / outdated logfiles */
			} else {
				struct mars_rotate *rot;

				rot = parent->d_private;
				if (rot && rot->relevant_serial > remote_dent->d_serial) {
					XIO_DBG("ignoring outdated remote logfile '%s' (behind %d)\n",
						 remote_dent->d_path, rot->relevant_serial);
				} else {
					struct mars_dent *local_dent;

					local_dent = mars_find_dent(peer->global, remote_dent->d_path);
					status = check_logfile(peer->peer,
						remote_dent,
						local_dent,
						parent,
						local_stat.size);
				}
			}
			brick_string_free(parent_path);
		}
	} else {
		XIO_DBG("ignoring '%s'\n", remote_dent->d_path);
	}

done:
	brick_string_free(marker_path);
	if (status >= 0)
		status = run_trigger ? 1 : 0;
	return status;
}

static
int run_bones(struct mars_peerinfo *peer)
{
	LIST_HEAD(tmp_list);
	struct list_head *tmp;
	unsigned long flags;
	bool run_trigger = false;
	int status = 0;

	spin_lock_irqsave(&peer->lock, flags);
	list_replace_init(&peer->remote_dent_list, &tmp_list);
	spin_unlock_irqrestore(&peer->lock, flags);

	XIO_DBG("remote_dent_list list_empty = %d\n", list_empty(&tmp_list));

	for (tmp = tmp_list.next; tmp != &tmp_list; tmp = tmp->next) {
		struct mars_dent *remote_dent = container_of(tmp, struct mars_dent, dent_link);

		if (!remote_dent->d_path || !remote_dent->d_name) {
			XIO_DBG("NULL\n");
			continue;
		}
		status = run_bone(peer, remote_dent);
		if (status > 0)
			run_trigger = true;
		/* XIO_DBG("path = '%s' worker status = %d\n", remote_dent->d_path, status); */
	}

	xio_free_dent_all(NULL, &tmp_list);

	if (run_trigger)
		local_trigger();
	return status;
}

/*********************************************************************/

/*  remote working infrastructure */

static
void _peer_cleanup(struct mars_peerinfo *peer)
{
	XIO_DBG("cleanup\n");
	if (xio_socket_is_alive(&peer->socket)) {
		XIO_DBG("really shutdown socket\n");
		xio_shutdown_socket(&peer->socket);
	}
	xio_put_socket(&peer->socket);
}

static DECLARE_WAIT_QUEUE_HEAD(remote_event);

static
int peer_thread(void *data)
{
	struct mars_peerinfo *peer = data;
	char *real_peer;
	struct sockaddr_storage src_sockaddr;
	struct sockaddr_storage dst_sockaddr;

	struct key_value_pair peer_pairs[] = {
		{ peer->peer },
		{ NULL }
	};
	int pause_time = 0;
	bool do_kill = false;
	int status;

	if (!peer)
		return -1;

	real_peer = xio_translate_hostname(peer->peer);
	XIO_INF("-------- peer thread starting on peer '%s' (%s)\n", peer->peer, real_peer);

	status = xio_create_sockaddr(&src_sockaddr, my_id());
	if (unlikely(status < 0)) {
		XIO_ERR("unusable local address '%s' (%s)\n", real_peer, peer->peer);
		goto done;
	}

	status = xio_create_sockaddr(&dst_sockaddr, real_peer);
	if (unlikely(status < 0)) {
		XIO_ERR("unusable remote address '%s' (%s)\n", real_peer, peer->peer);
		goto done;
	}

	while (!brick_thread_should_stop()) {
		struct mars_global tmp_global = {
			.dent_anchor = LIST_HEAD_INIT(tmp_global.dent_anchor),
			.brick_anchor = LIST_HEAD_INIT(tmp_global.brick_anchor),
			.global_power = {
				.button = true,
			},
			.main_event = __WAIT_QUEUE_HEAD_INITIALIZER(tmp_global.main_event),
		};
		LIST_HEAD(old_list);
		unsigned long flags;

		struct xio_cmd cmd = {
			.cmd_str1 = peer->path,
			.cmd_int1 = peer->maxdepth,
		};

		init_rwsem(&tmp_global.dent_mutex);
		init_rwsem(&tmp_global.brick_mutex);

		show_vals(peer_pairs, "/mars", "connection-from-");

		if (!xio_socket_is_alive(&peer->socket)) {
			make_msg(peer_pairs, "connection to '%s' (%s) is dead", peer->peer, real_peer);
			brick_string_free(real_peer);
			real_peer = xio_translate_hostname(peer->peer);
			status = xio_create_sockaddr(&dst_sockaddr, real_peer);
			if (unlikely(status < 0)) {
				XIO_ERR("unusable remote address '%s' (%s)\n", real_peer, peer->peer);
				make_msg(peer_pairs, "unusable remote address '%s' (%s)\n", real_peer, peer->peer);
				brick_msleep(1000);
				continue;
			}
			if (do_kill) {
				do_kill = false;
				_peer_cleanup(peer);
				brick_msleep(1000);
				continue;
			}
			if (!xio_net_is_alive) {
				brick_msleep(1000);
				continue;
			}

			status = xio_create_socket(&peer->socket, &src_sockaddr, &dst_sockaddr);
			if (unlikely(status < 0)) {
				XIO_INF("no connection to mars module on '%s' (%s) status = %d\n",
					peer->peer,
					real_peer,
					status);
				make_msg(peer_pairs,
					"connection to '%s' (%s) could not be established: status = %d",
					peer->peer,
					real_peer,
					status);
				brick_msleep(2000);
				continue;
			}
			do_kill = true;
			peer->socket.s_shutdown_on_err = true;
			peer->socket.s_send_abort = mars_peer_abort;
			peer->socket.s_recv_abort = mars_peer_abort;
			XIO_DBG("successfully opened socket to '%s'\n", real_peer);
			brick_msleep(100);
			continue;
		}

		if (peer->from_remote_trigger) {
			pause_time = 0;
			peer->from_remote_trigger = false;
			XIO_DBG("got notify from peer.\n");
		}

		status = 0;
		if (peer->to_remote_trigger) {
			pause_time = 0;
			peer->to_remote_trigger = false;
			XIO_DBG("sending notify to peer...\n");
			cmd.cmd_code = CMD_NOTIFY;
			status = xio_send_struct(&peer->socket, &cmd, xio_cmd_meta);
		}

		if (likely(status >= 0)) {
			cmd.cmd_code = CMD_GETENTS;
			status = xio_send_struct(&peer->socket, &cmd, xio_cmd_meta);
		}
		if (unlikely(status < 0)) {
			XIO_WRN("communication error on send, status = %d\n", status);
			if (do_kill) {
				do_kill = false;
				_peer_cleanup(peer);
			}
			brick_msleep(1000);
			continue;
		}

		XIO_DBG("fetching remote dentry list\n");
		status = xio_recv_dent_list(&peer->socket, &tmp_global.dent_anchor);
		if (unlikely(status < 0)) {
			XIO_WRN("communication error on receive, status = %d\n", status);
			if (do_kill) {
				do_kill = false;
				_peer_cleanup(peer);
			}
			goto free_and_restart;
			xio_free_dent_all(NULL, &tmp_global.dent_anchor);
			brick_msleep(2000);
			continue;
		}

		if (likely(!list_empty(&tmp_global.dent_anchor))) {
			struct mars_dent *peer_uuid;
			struct mars_dent *my_uuid;

			XIO_DBG("got remote denties\n");

			peer_uuid = mars_find_dent(&tmp_global, "/mars/uuid");
			if (unlikely(!peer_uuid || !peer_uuid->link_val)) {
				XIO_ERR("peer %s has no uuid\n", peer->peer);
				make_msg(peer_pairs, "peer has no UUID");
				goto free_and_restart;
			}
			my_uuid = mars_find_dent(mars_global, "/mars/uuid");
			if (unlikely(!my_uuid || !my_uuid->link_val)) {
				XIO_ERR("cannot determine my own uuid for peer %s\n", peer->peer);
				make_msg(peer_pairs, "cannot determine my own uuid");
				goto free_and_restart;
			}
			if (unlikely(strcmp(peer_uuid->link_val, my_uuid->link_val))) {
				XIO_ERR("UUID mismatch for peer %s, you are trying to communicate with a foreign cluster!\n",
					peer->peer);
				make_msg(peer_pairs,
					"UUID mismatch, own cluster '%s' is trying to communicate with a foreign cluster '%s'",
					 my_uuid->link_val, peer_uuid->link_val);
				goto free_and_restart;
			}

			make_msg(peer_pairs, "CONNECTED %s(%s)", peer->peer, real_peer);

			spin_lock_irqsave(&peer->lock, flags);

			list_replace_init(&peer->remote_dent_list, &old_list);
			list_replace_init(&tmp_global.dent_anchor, &peer->remote_dent_list);

			spin_unlock_irqrestore(&peer->lock, flags);

			peer->last_remote_jiffies = jiffies;

			local_trigger();

			xio_free_dent_all(NULL, &old_list);
		}

		brick_msleep(100);
		if (!brick_thread_should_stop()) {
			if (pause_time < mars_propagate_interval)
				pause_time++;
			wait_event_interruptible_timeout(remote_event,
							 (peer->to_remote_trigger | peer->from_remote_trigger) ||
							 (mars_global && mars_global->main_trigger),
							 pause_time * HZ);
		}
		continue;

free_and_restart:
		xio_free_dent_all(NULL, &tmp_global.dent_anchor);
		brick_msleep(2000);
	}

	XIO_INF("-------- peer thread terminating\n");

	make_msg(peer_pairs, "NOT connected %s(%s)", peer->peer, real_peer);
	show_vals(peer_pairs, "/mars", "connection-from-");

	if (do_kill)
		_peer_cleanup(peer);

done:
	clear_vals(peer_pairs);
	brick_string_free(real_peer);
	return 0;
}

static
void _make_alive(void)
{
	struct timespec now;
	char *tmp;

	get_lamport(&now);
	tmp = path_make("%ld.%09ld", now.tv_sec, now.tv_nsec);
	if (likely(tmp)) {
		_make_alivelink_str("time", tmp);
		brick_string_free(tmp);
	}
	_make_alivelink("alive", mars_global && mars_global->global_power.button ? 1 : 0);
	_make_alivelink_str("tree", SYMLINK_TREE_VERSION);
}

void from_remote_trigger(void)
{
	struct list_head *tmp;
	int count = 0;
	unsigned long flags;

	_make_alive();

	read_lock_irqsave(&peer_lock, flags);
	for (tmp = peer_anchor.next; tmp != &peer_anchor; tmp = tmp->next) {
		struct mars_peerinfo *peer = container_of(tmp, struct mars_peerinfo, peer_head);

		peer->from_remote_trigger = true;
		count++;
	}
	read_unlock_irqrestore(&peer_lock, flags);

	XIO_DBG("got trigger for %d peers\n", count);
	wake_up_interruptible_all(&remote_event);
}

static
void __remote_trigger(void)
{
	struct list_head *tmp;
	int count = 0;
	unsigned long flags;

	read_lock_irqsave(&peer_lock, flags);
	for (tmp = peer_anchor.next; tmp != &peer_anchor; tmp = tmp->next) {
		struct mars_peerinfo *peer = container_of(tmp, struct mars_peerinfo, peer_head);

		peer->to_remote_trigger = true;
		count++;
	}
	read_unlock_irqrestore(&peer_lock, flags);

	XIO_DBG("triggered %d peers\n", count);
	wake_up_interruptible_all(&remote_event);
}

static
bool is_shutdown(void)
{
	bool res = false;
	int used = atomic_read(&global_mshadow_count);

	if (used > 0) {
		XIO_INF("global shutdown delayed: there are %d buffers in use, occupying %ld bytes\n",
			used,
			atomic64_read(&global_mshadow_used));
	} else {
		int rounds = 3;

		while ((used = atomic_read(&xio_global_io_flying)) <= 0) {
			if (--rounds <= 0) {
				res = true;
				break;
			}
			brick_msleep(30);
		}
		if (!res)
			XIO_INF("global shutdown delayed: there are %d IO requests flying\n", used);
	}
	return res;
}

/*********************************************************************/

/*  helpers for worker functions */

static int _kill_peer(struct mars_global *global, struct mars_peerinfo *peer)
{
	LIST_HEAD(tmp_list);
	unsigned long flags;

	if (!peer)
		return 0;

	write_lock_irqsave(&peer_lock, flags);
	list_del_init(&peer->peer_head);
	write_unlock_irqrestore(&peer_lock, flags);

	XIO_INF("stopping peer thread...\n");
	if (peer->peer_thread) {
		brick_thread_stop(peer->peer_thread);
		peer->peer_thread = NULL;
	}
	spin_lock_irqsave(&peer->lock, flags);
	list_replace_init(&peer->remote_dent_list, &tmp_list);
	spin_unlock_irqrestore(&peer->lock, flags);
	xio_free_dent_all(NULL, &tmp_list);
	brick_string_free(peer->peer);
	brick_string_free(peer->path);
	return 0;
}

static
void peer_destruct(void *_peer)
{
	struct mars_peerinfo *peer = _peer;

	if (likely(peer))
		_kill_peer(peer->global, peer);
}

static int _make_peer(struct mars_global *global, struct mars_dent *dent, char *path)
{
	static int serial;
	struct mars_peerinfo *peer;
	char *mypeer;
	char *parent_path;
	int status = 0;

	if (unlikely(!global ||
		     !dent || !dent->link_val || !dent->d_parent)) {
		XIO_DBG("cannot work\n");
		return 0;
	}
	parent_path = dent->d_parent->d_path;
	if (unlikely(!parent_path)) {
		XIO_DBG("cannot work\n");
		return 0;
	}
	mypeer = dent->d_rest;
	if (!mypeer) {
		status = _parse_args(dent, dent->link_val, 1);
		if (status < 0)
			goto done;
		mypeer = dent->d_argv[0];
	}

	XIO_DBG("peer '%s'\n", mypeer);
	if (!dent->d_private) {
		unsigned long flags;

		dent->d_private = brick_zmem_alloc(sizeof(struct mars_peerinfo));
		dent->d_private_destruct = peer_destruct;
		peer = dent->d_private;
		peer->global = global;
		peer->peer = brick_strdup(mypeer);
		peer->path = brick_strdup(path);
		peer->maxdepth = 2;
		spin_lock_init(&peer->lock);
		INIT_LIST_HEAD(&peer->peer_head);
		INIT_LIST_HEAD(&peer->remote_dent_list);

		write_lock_irqsave(&peer_lock, flags);
		list_add_tail(&peer->peer_head, &peer_anchor);
		write_unlock_irqrestore(&peer_lock, flags);
	}

	peer = dent->d_private;
	if (!peer->peer_thread) {
		peer->peer_thread = brick_thread_create(peer_thread, peer, "mars_peer%d", serial++);
		if (unlikely(!peer->peer_thread)) {
			XIO_ERR("cannot start peer thread\n");
			return -1;
		}
		XIO_DBG("started peer thread\n");
	}

	/* This must be called by the main thread in order to
	 * avoid nasty races.
	 * The peer thread does nothing but fetching the dent list.
	 */
	status = run_bones(peer);

done:
	return status;
}

static int kill_scan(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_peerinfo *peer = dent->d_private;
	int res;

	if (!global || global->global_power.button || !peer)
		return 0;
	dent->d_private = NULL;
	res = _kill_peer(global, peer);
	brick_mem_free(peer);
	return res;
}

static int make_scan(void *buf, struct mars_dent *dent)
{
	XIO_DBG("path = '%s' peer = '%s'\n", dent->d_path, dent->d_rest);
	/*  don't connect to myself */
	if (!strcmp(dent->d_rest, my_id()))
		return 0;
	return _make_peer(buf, dent, "/mars");
}

static
int kill_any(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct list_head *tmp;

	if (global->global_power.button || !is_shutdown())
		return 0;

	for (tmp = dent->brick_list.next; tmp != &dent->brick_list; tmp = tmp->next) {
		struct xio_brick *brick = container_of(tmp, struct xio_brick, dent_brick_link);

		if (brick->nr_outputs > 0 && brick->outputs[0] && brick->outputs[0]->nr_connected) {
			XIO_DBG("cannot kill dent '%s' because brick '%s' is wired\n",
				dent->d_path,
				brick->brick_path);
			return 0;
		}
	}

	XIO_DBG("killing dent = '%s'\n", dent->d_path);
	xio_kill_dent(dent);
	return 1;
}

/*********************************************************************/

/*  handlers / helpers for logfile rotation */

static
void _create_new_logfile(const char *path)
{
	struct file *f;
	const int flags = O_RDWR | O_CREAT | O_EXCL;
	const int prot = 0600;

	mm_segment_t oldfs;

	oldfs = get_fs();
	set_fs(get_ds());
	f = filp_open(path, flags, prot);
	set_fs(oldfs);
	if (IS_ERR(f)) {
		int err = PTR_ERR(f);

		if (err == -EEXIST)
			XIO_INF("logfile '%s' already exists\n", path);
		else
			XIO_ERR("could not create logfile '%s' status = %d\n", path, err);
	} else {
		XIO_DBG("created empty logfile '%s'\n", path);
		filp_close(f, NULL);
		local_trigger();
	}
}

static
const char *get_replaylink(const char *parent_path, const char *host, const char **linkpath)
{
	const char *_linkpath = path_make("%s/replay-%s", parent_path, host);

	*linkpath = _linkpath;
	if (unlikely(!_linkpath)) {
		XIO_ERR("no MEM\n");
		return NULL;
	}
	return mars_readlink(_linkpath);
}

static
const char *get_versionlink(const char *parent_path, int seq, const char *host, const char **linkpath)
{
	const char *_linkpath = path_make("%s/version-%09d-%s", parent_path, seq, host);

	*linkpath = _linkpath;
	if (unlikely(!_linkpath)) {
		XIO_ERR("no MEM\n");
		return NULL;
	}
	return mars_readlink(_linkpath);
}

static inline
int _get_tolerance(struct mars_rotate *rot)
{
	if (rot->is_log_damaged)
		return REPLAY_TOLERANCE;
	return 0;
}

static
bool is_switchover_possible(struct mars_rotate *rot,
	const char *old_log_path,
	const char *new_log_path,
	int replay_tolerance,
	bool skip_new)
{
	const char *old_log_name = old_log_path + skip_dir(old_log_path);
	const char *new_log_name = new_log_path + skip_dir(new_log_path);
	const char *old_host = NULL;
	const char *new_host = NULL;
	const char *own_versionlink_path = NULL;
	const char *old_versionlink_path = NULL;
	const char *new_versionlink_path = NULL;
	const char *own_versionlink = NULL;
	const char *old_versionlink = NULL;
	const char *new_versionlink = NULL;
	const char *own_replaylink_path = NULL;
	const char *own_replaylink = NULL;
	loff_t own_r_val;
	loff_t own_v_val;
	loff_t own_r_tail;
	int old_log_seq;
	int new_log_seq;
	int own_r_offset;
	int own_v_offset;
	int own_r_len;
	int own_v_len;
	int len1;
	int len2;
	int offs2;
	char dummy = 0;

	bool res = false;

	XIO_DBG("old_log = '%s' new_log = '%s' toler = %d skip_new = %d\n",
		 old_log_path, new_log_path, replay_tolerance, skip_new);

	/*  check precondition: is split brain already for sure? */
	if (unlikely(rot->has_double_logfile)) {
		XIO_WRN_TO(rot->log_say,
			"SPLIT BRAIN detected: multiple logfiles with sequence number %d exist\n",
			rot->next_relevant_log->d_serial);
		make_rot_msg(rot,
			"err-splitbrain-detected",
			"SPLIT BRAIN detected: multiple logfiles with sequence number %d exist\n",
			rot->next_relevant_log->d_serial);
		goto done;
	}

	/*  parse the names */
	if (unlikely(!parse_logfile_name(old_log_name, &old_log_seq, &old_host))) {
		make_rot_msg(rot, "err-bad-log-name", "logfile name '%s' cannot be parsed", old_log_name);
		goto done;
	}
	if (unlikely(!parse_logfile_name(new_log_name, &new_log_seq, &new_host))) {
		make_rot_msg(rot, "err-bad-log-name", "logfile name '%s' cannot be parsed", new_log_name);
		goto done;
	}

	/*  check: are the sequence numbers contiguous? */
	if (unlikely(new_log_seq != old_log_seq + 1)) {
		XIO_ERR_TO(rot->log_say,
			"logfile sequence numbers are not contiguous (%d != %d + 1), old_log_path='%s' new_log_path='%s'\n",
			new_log_seq,
			old_log_seq,
			old_log_path,
			new_log_path);
		make_rot_msg(rot,
			"err-log-not-contiguous",
			"logfile sequence numbers are not contiguous (%d != %d + 1) old_log_path='%s' new_log_path='%s'",
			new_log_seq,
			old_log_seq,
			old_log_path,
			new_log_path);
		goto done;
	}

	/*  fetch all the versionlinks and test for their existence. */
	own_versionlink = get_versionlink(rot->parent_path, old_log_seq, my_id(), &own_versionlink_path);
	if (unlikely(!own_versionlink || !own_versionlink[0])) {
		XIO_ERR_TO(rot->log_say, "cannot read my own versionlink '%s'\n", own_versionlink_path);
		make_rot_msg(rot,
			"err-versionlink-not-readable",
			"cannot read my own versionlink '%s'",
			own_versionlink_path);
		goto done;
	}
	old_versionlink = get_versionlink(rot->parent_path, old_log_seq, old_host, &old_versionlink_path);
	if (unlikely(!old_versionlink || !old_versionlink[0])) {
		XIO_ERR_TO(rot->log_say, "cannot read old versionlink '%s'\n", old_versionlink_path);
		make_rot_msg(rot,
			"err-versionlink-not-readable",
			"cannot read old versionlink '%s'",
			old_versionlink_path);
		goto done;
	}
	if (!skip_new) {
		new_versionlink = get_versionlink(rot->parent_path, new_log_seq, new_host, &new_versionlink_path);
		if (unlikely(!new_versionlink || !new_versionlink[0])) {
			XIO_INF_TO(rot->log_say,
				"new versionlink '%s' does not yet exist, we must wait for it.\n",
				new_versionlink_path);
			make_rot_msg(rot,
				"inf-versionlink-not-yet-exist",
				"we must wait for new versionlink '%s'",
				new_versionlink_path);
			goto done;
		}
	}

	/*  check: are the versionlinks correct? */
	if (unlikely(strcmp(own_versionlink, old_versionlink))) {
		XIO_INF_TO(rot->log_say,
			"old logfile is not yet completeley transferred, own_versionlink '%s' -> '%s' != old_versionlink '%s' -> '%s'\n",
			own_versionlink_path,
			own_versionlink,
			old_versionlink_path,
			old_versionlink);
		make_rot_msg(rot,
			"inf-versionlink-not-equal",
			"old logfile is not yet completeley transferred (own_versionlink '%s' -> '%s' != old_versionlink '%s' -> '%s')",
			own_versionlink_path,
			own_versionlink,
			old_versionlink_path,
			old_versionlink);
		goto done;
	}

	/*  check: did I fully replay my old logfile data? */
	own_replaylink = get_replaylink(rot->parent_path, my_id(), &own_replaylink_path);
	if (unlikely(!own_replaylink || !own_replaylink[0])) {
		XIO_ERR_TO(rot->log_say, "cannot read my own replaylink '%s'\n", own_replaylink_path);
		goto done;
	}
	own_r_len = skip_part(own_replaylink);
	own_v_offset = skip_part(own_versionlink);
	if (unlikely(!own_versionlink[own_v_offset++])) {
		XIO_ERR_TO(rot->log_say,
			"own version link '%s' -> '%s' is malformed\n",
			own_versionlink_path,
			own_versionlink);
		make_rot_msg(rot,
			"err-replaylink-not-readable",
			"own version link '%s' -> '%s' is malformed",
			own_versionlink_path,
			own_versionlink);
		goto done;
	}
	own_v_len = skip_part(own_versionlink + own_v_offset);
	if (unlikely(own_r_len != own_v_len ||
		     strncmp(own_replaylink, own_versionlink + own_v_offset, own_r_len))) {
		XIO_ERR_TO(rot->log_say,
			"internal problem: logfile name mismatch between '%s' and '%s'\n",
			own_replaylink,
			own_versionlink);
		make_rot_msg(rot,
			"err-bad-log-name",
			"internal problem: logfile name mismatch between '%s' and '%s'",
			own_replaylink,
			own_versionlink);
		goto done;
	}
	if (unlikely(!own_replaylink[own_r_len])) {
		XIO_ERR_TO(rot->log_say,
			"own replay link '%s' -> '%s' is malformed\n",
			own_replaylink_path,
			own_replaylink);
		make_rot_msg(rot,
			"err-replaylink-not-readable",
			"own replay link '%s' -> '%s' is malformed",
			own_replaylink_path,
			own_replaylink);
		goto done;
	}
	own_r_offset = own_r_len + 1;
	if (unlikely(!own_versionlink[own_v_len])) {
		XIO_ERR_TO(rot->log_say,
			"own version link '%s' -> '%s' is malformed\n",
			own_versionlink_path,
			own_versionlink);
		make_rot_msg(rot,
			"err-versionlink-not-readable",
			"own version link '%s' -> '%s' is malformed",
			own_versionlink_path,
			own_versionlink);
		goto done;
	}
	own_v_offset += own_r_len + 1;
	own_r_len = skip_part(own_replaylink  + own_r_offset);
	own_v_len = skip_part(own_versionlink + own_v_offset);
	own_r_val = own_v_val = 0;
	own_r_tail = 0;
	if (sscanf(own_replaylink + own_r_offset, "%lld,%lld", &own_r_val, &own_r_tail) != 2) {
		XIO_ERR_TO(rot->log_say,
			"own replay link '%s' -> '%s' is malformed\n",
			own_replaylink_path,
			own_replaylink);
		make_rot_msg(rot,
			"err-replaylink-not-readable",
			"own replay link '%s' -> '%s' is malformed",
			own_replaylink_path,
			own_replaylink);
		goto done;
	}
	/* SSCANF_TO_KSTRTO: kstros64 does not work because of the next char */
	if (sscanf(own_versionlink + own_v_offset, "%lld%c", &own_v_val, &dummy) != 2) {
		XIO_ERR_TO(rot->log_say,
			"own version link '%s' -> '%s' is malformed\n",
			own_versionlink_path,
			own_versionlink);
		make_rot_msg(rot,
			"err-versionlink-not-readable",
			"own version link '%s' -> '%s' is malformed",
			own_versionlink_path,
			own_versionlink);
		goto done;
	}
	if (unlikely(own_r_len > own_v_len || own_r_len + replay_tolerance < own_v_len)) {
		XIO_INF_TO(rot->log_say,
			"log replay is not yet finished: '%s' and '%s' are reporting different positions.\n",
			own_replaylink,
			own_versionlink);
		make_rot_msg(rot,
			"inf-replay-not-yet-finished",
			"log replay is not yet finished: '%s' and '%s' are reporting different positions",
			own_replaylink,
			own_versionlink);
		goto done;
	}

	/*  last check: is the new versionlink based on the old one? */
	if (!skip_new) {
		len1 = skip_sect(own_versionlink);
		offs2 = skip_sect(new_versionlink);
		if (unlikely(!new_versionlink[offs2++])) {
			XIO_ERR_TO(rot->log_say,
				"new version link '%s' -> '%s' is malformed\n",
				new_versionlink_path,
				new_versionlink);
			make_rot_msg(rot,
				"err-versionlink-not-readable",
				"new version link '%s' -> '%s' is malformed",
				new_versionlink_path,
				new_versionlink);
			goto done;
		}
		len2 = skip_sect(new_versionlink + offs2);
		if (unlikely(len1 != len2 ||
			     strncmp(own_versionlink, new_versionlink + offs2, len1))) {
			XIO_WRN_TO(rot->log_say,
				"VERSION MISMATCH old '%s' -> '%s' new '%s' -> '%s' ==(%d,%d) ===> check for SPLIT BRAIN!\n",
				own_versionlink_path,
				own_versionlink,
				new_versionlink_path,
				new_versionlink,
				len1,
				len2);
			make_rot_msg(rot,
				"err-splitbrain-detected",
				"VERSION MISMATCH old '%s' -> '%s' new '%s' -> '%s' ==(%d,%d) ===> check for SPLIT BRAIN",
				own_versionlink_path,
				own_versionlink,
				new_versionlink_path,
				new_versionlink,
				len1,
				len2);
			goto done;
		}
	}

	/*  report success */
	res = true;
	XIO_DBG("VERSION OK '%s' -> '%s'\n", own_versionlink_path, own_versionlink);

done:
	brick_string_free(old_host);
	brick_string_free(new_host);
	brick_string_free(own_versionlink_path);
	brick_string_free(old_versionlink_path);
	brick_string_free(new_versionlink_path);
	brick_string_free(own_versionlink);
	brick_string_free(old_versionlink);
	brick_string_free(new_versionlink);
	brick_string_free(own_replaylink_path);
	brick_string_free(own_replaylink);
	return res;
}

static
void rot_destruct(void *_rot)
{
	struct mars_rotate *rot = _rot;

	if (likely(rot)) {
		list_del_init(&rot->rot_head);
		write_info_links(rot);
		del_channel(rot->log_say);
		rot->log_say = NULL;
		brick_string_free(rot->fetch_path);
		brick_string_free(rot->fetch_peer);
		brick_string_free(rot->preferred_peer);
		brick_string_free(rot->parent_path);
		brick_string_free(rot->parent_rest);
		brick_string_free(rot->fetch_next_origin);
		rot->fetch_path = NULL;
		rot->fetch_peer = NULL;
		rot->preferred_peer = NULL;
		rot->parent_path = NULL;
		rot->parent_rest = NULL;
		rot->fetch_next_origin = NULL;
		clear_vals(rot->msgs);
	}
}

/* This must be called once at every round of logfile checking.
 */
static
int make_log_init(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_dent *parent = dent->d_parent;
	struct xio_brick *bio_brick;
	struct xio_brick *aio_brick;
	struct xio_brick *trans_brick;
	struct mars_rotate *rot = parent->d_private;
	struct mars_dent *replay_link;
	struct mars_dent *aio_dent;
	struct xio_output *output;
	const char *parent_path;
	const char *replay_path = NULL;
	const char *aio_path = NULL;
	bool switch_on;
	int status = 0;

	if (!global->global_power.button)
		goto done;
	status = -EINVAL;
	CHECK_PTR(parent, done);
	parent_path = parent->d_path;
	CHECK_PTR(parent_path, done);

	if (!rot) {
		const char *fetch_path;

		rot = brick_zmem_alloc(sizeof(struct mars_rotate));
		spin_lock_init(&rot->inf_lock);
		fetch_path = path_make("%s/logfile-update", parent_path);
		if (unlikely(!fetch_path)) {
			XIO_ERR("cannot create fetch_path\n");
			brick_mem_free(rot);
			status = -ENOMEM;
			goto done;
		}
		rot->fetch_path = fetch_path;
		rot->global = global;
		parent->d_private = rot;
		parent->d_private_destruct = rot_destruct;
		list_add_tail(&rot->rot_head, &rot_anchor);
		assign_keys(rot->msgs, rot_keys);
	}

	rot->replay_link = NULL;
	rot->aio_dent = NULL;
	rot->aio_brick = NULL;
	rot->first_log = NULL;
	rot->relevant_log = NULL;
	rot->relevant_serial = 0;
	rot->relevant_brick = NULL;
	rot->next_relevant_log = NULL;
	rot->prev_log = NULL;
	rot->next_log = NULL;
	brick_string_free(rot->fetch_next_origin);
	rot->fetch_next_origin = NULL;
	rot->max_sequence = 0;
	/*  reset the split brain detector only when conflicts have gone for a number of rounds */
	if (rot->split_brain_serial && rot->split_brain_round++ > 3)
		rot->split_brain_serial = 0;
	rot->fetch_next_serial = 0;
	rot->has_error = false;
	rot->wants_sync = false;
	rot->has_symlinks = true;
	brick_string_free(rot->preferred_peer);
	rot->preferred_peer = NULL;

	if (dent->link_val) {
		int status = kstrtos64(dent->link_val, 10, &rot->dev_size);

		(void)status; /* leave as before in case of errors */
	}
	if (!rot->parent_path) {
		rot->parent_path = brick_strdup(parent_path);
		rot->parent_rest = brick_strdup(parent->d_rest);
	}

	if (unlikely(!rot->log_say)) {
		char *name = path_make("%s/logstatus-%s", parent_path, my_id());

		if (likely(name)) {
			rot->log_say = make_channel(name, false);
			brick_string_free(name);
		}
	}

	write_info_links(rot);

	/* Fetch the replay status symlink.
	 * It must exist, and its value will control everything.
	 */
	replay_path = path_make("%s/replay-%s", parent_path, my_id());
	if (unlikely(!replay_path)) {
		XIO_ERR("cannot make path\n");
		status = -ENOMEM;
		goto done;
	}

	replay_link = (void *)mars_find_dent(global, replay_path);
	if (unlikely(!replay_link || !replay_link->link_val)) {
		XIO_DBG("replay status symlink '%s' does not exist (%p)\n", replay_path, replay_link);
		rot->allow_update = false;
		status = -ENOENT;
		goto done;
	}

	status = _parse_args(replay_link, replay_link->link_val, 3);
	if (unlikely(status < 0))
		goto done;
	rot->replay_link = replay_link;

	/* Fetch AIO dentry of the logfile.
	 */
	if (rot->trans_brick) {
		struct trans_logger_input *trans_input = rot->trans_brick->inputs[rot->trans_brick->old_input_nr];

		if (trans_input && trans_input->is_operating) {
			aio_path = path_make("%s/log-%09d-%s",
				parent_path,
				trans_input->inf.inf_sequence,
				trans_input->inf.inf_host);
			XIO_DBG("using logfile '%s' from trans_input %d (new=%d)\n",
				aio_path,
				rot->trans_brick->old_input_nr,
				rot->trans_brick->log_input_nr);
		}
	}
	if (!aio_path) {
		aio_path = path_make("%s/%s", parent_path, replay_link->d_argv[0]);
		XIO_DBG("using logfile '%s' from replay symlink\n", aio_path);
	}
	if (unlikely(!aio_path)) {
		XIO_ERR("cannot make path\n");
		status = -ENOMEM;
		goto done;
	}

	aio_dent = (void *)mars_find_dent(global, aio_path);
	if (unlikely(!aio_dent)) {
		XIO_DBG("logfile '%s' does not exist\n", aio_path);
		status = -ENOENT;
		if (rot->todo_primary && !rot->is_primary && !rot->old_is_primary) {
			int offset = strlen(aio_path) - strlen(my_id());

			if (offset > 0 && aio_path[offset-1] == '-' && !strcmp(aio_path + offset, my_id())) {
				/*  try to create an empty logfile */
				_create_new_logfile(aio_path);
			}
		}
		goto done;
	}
	rot->aio_dent = aio_dent;

	/*  check whether attach is allowed */
	switch_on = _check_allow(global, parent, "attach");
	if (switch_on && rot->res_shutdown) {
		XIO_ERR("cannot start transaction logger: resource shutdown mode is currently active\n");
		switch_on = false;
	}

	/* Fetch / make the AIO brick instance
	 */
	aio_brick =
		make_brick_all(global,
			       aio_dent,
			       _set_sio_params,
			       NULL,
			       aio_path,
			       (const struct generic_brick_type *)&sio_brick_type,
			       (const struct generic_brick_type*[]){},
/**/			       rot->trans_brick || switch_on ? 2 : -1,
			       "%s",
			       (const char *[]){},
			       0,
			       aio_path);
	rot->aio_brick = aio_brick;
	status = 0;
	if (unlikely(!aio_brick || !aio_brick->power.on_led))
		goto done; /*  this may happen in case of detach */
	bio_brick = rot->bio_brick;
	if (unlikely(!bio_brick || !bio_brick->power.on_led))
		goto done; /*  this may happen in case of detach */

	/* Fetch the actual logfile size
	 */
	output = aio_brick->outputs[0];
	status = output->ops->xio_get_info(output, &rot->aio_info);
	if (status < 0) {
		XIO_ERR("cannot get info on '%s'\n", aio_path);
		goto done;
	}
	XIO_DBG("logfile '%s' size = %lld\n", aio_path, rot->aio_info.current_size);

	if (rot->is_primary &&
	    global_logrot_auto > 0 &&
	    unlikely(rot->aio_info.current_size >= (loff_t)global_logrot_auto * 1024 * 1024 * 1024)) {
		char *new_path = path_make("%s/log-%09d-%s", parent_path, aio_dent->d_serial + 1, my_id());

		if (likely(new_path && !mars_find_dent(global, new_path))) {
			XIO_INF("old logfile size = %lld, creating new logfile '%s'\n",
				rot->aio_info.current_size,
				new_path);
			_create_new_logfile(new_path);
		}
		brick_string_free(new_path);
	}

	/* Fetch / make the transaction logger.
	 * We deliberately "forget" to connect the log input here.
	 * Will be carried out later in make_log_step().
	 * The final switch-on will be started in make_log_finalize().
	 */
	trans_brick =
		make_brick_all(global,
			       replay_link,
			       _set_trans_params,
			       NULL,
			       aio_path,
			       (const struct generic_brick_type *)&trans_logger_brick_type,
			       (const struct generic_brick_type *[]){NULL},
			       1, /*  create when necessary, but leave in current state otherwise */
			       "%s/replay-%s",
			       (const char *[]){"%s/data-%s"},
			       1,
			       parent_path,
			       my_id(),
			       parent_path,
			       my_id());
	rot->trans_brick = (void *)trans_brick;
	status = -ENOENT;
	if (!trans_brick)
		goto done;
	rot->trans_brick->kill_ptr = (void **)&rot->trans_brick;
	rot->trans_brick->replay_limiter = &rot->replay_limiter;
	/* For safety, default is to try an (unnecessary) replay in case
	 * something goes wrong later.
	 */
	rot->replay_mode = true;

	status = 0;

done:
	brick_string_free(aio_path);
	brick_string_free(replay_path);
	return status;
}

static
bool _next_is_acceptable(struct mars_rotate *rot, struct mars_dent *old_dent, struct mars_dent *new_dent)
{
	/* Primaries are never allowed to consider logfiles not belonging to them.
	 * Secondaries need this for replay, unfortunately.
	 */
	if ((rot->is_primary | rot->old_is_primary) ||
	    (rot->trans_brick && rot->trans_brick->power.on_led && !rot->trans_brick->replay_mode)) {
		if (new_dent->stat_val.size) {
			XIO_WRN("logrotate impossible, '%s' size = %lld\n",
				new_dent->d_rest,
				new_dent->stat_val.size);
			return false;
		}
		if (strcmp(new_dent->d_rest, my_id())) {
			XIO_WRN("logrotate impossible, '%s'\n", new_dent->d_rest);
			return false;
		}
	} else {
		/* Only secondaries should check for contiguity,
		 * primaries sometimes need holes for emergency mode.
		 */
		if (new_dent->d_serial != old_dent->d_serial + 1)
			return false;
	}
	return true;
}

/* Note: this is strictly called in d_serial order.
 * This is important!
 */
static
int make_log_step(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_dent *parent = dent->d_parent;
	struct mars_rotate *rot;
	struct trans_logger_brick *trans_brick;
	struct mars_dent *prev_log;
	int replay_log_nr = 0;
	int status = -EINVAL;

	CHECK_PTR(parent, err);
	rot = parent->d_private;
	if (!rot)
		goto err;
	CHECK_PTR(rot, err);

	status = 0;
	trans_brick = rot->trans_brick;
	if (!global->global_power.button || !dent->d_parent || !trans_brick || rot->has_error) {
		XIO_DBG("nothing to do rot_error = %d\n", rot->has_error);
		goto done;
	}

	/* Check for consecutiveness of logfiles
	 */
	prev_log = rot->next_log;
	if (prev_log && prev_log->d_serial + 1 != dent->d_serial &&
	    (!rot->replay_link || !rot->replay_link->d_argv[0] ||
	     sscanf(rot->replay_link->d_argv[0], "log-%d", &replay_log_nr) != 1 ||
	     dent->d_serial > replay_log_nr)) {
		XIO_WRN_TO(rot->log_say,
			"transaction logs are not consecutive at '%s' (%d ~> %d)\n",
			dent->d_path,
			prev_log->d_serial,
			dent->d_serial);
		make_rot_msg(rot,
			"wrn-log-consecutive",
			"transaction logs are not consecutive at '%s' (%d ~> %d)\n",
			dent->d_path,
			prev_log->d_serial,
			dent->d_serial);
	}

	if (dent->d_serial > rot->max_sequence)
		rot->max_sequence = dent->d_serial;

	if (!rot->first_log)
		rot->first_log = dent;

	/* Skip any logfiles after the relevant one.
	 * This should happen only when replaying multiple logfiles
	 * in sequence, or when starting a new logfile for writing.
	 */
	status = 0;
	if (rot->relevant_log) {
		if (!rot->next_relevant_log) {
			if (unlikely(dent->d_serial == rot->relevant_log->d_serial)) {
				/*  always prefer the one created by myself */
				if (!strcmp(rot->relevant_log->d_rest, my_id())) {
					XIO_WRN("PREFER LOGFILE '%s' in front of '%s'\n",
						 rot->relevant_log->d_path, dent->d_path);
				} else if (!strcmp(dent->d_rest, my_id())) {
					XIO_WRN("PREFER LOGFILE '%s' in front of '%s'\n",
						 dent->d_path, rot->relevant_log->d_path);
					rot->relevant_log = dent;
				} else {
					rot->has_double_logfile = true;
					XIO_ERR("DOUBLE LOGFILES '%s' '%s'\n",
						 dent->d_path, rot->relevant_log->d_path);
				}
			} else if (_next_is_acceptable(rot, rot->relevant_log, dent)) {
				rot->next_relevant_log = dent;
			} else if (dent->d_serial > rot->relevant_log->d_serial + 5) {
				rot->has_hole_logfile = true;
			}
		} else { /*  check for double logfiles = > split brain */
			if (unlikely(dent->d_serial == rot->next_relevant_log->d_serial)) {
				/*  always prefer the one created by myself */
				if (!strcmp(rot->next_relevant_log->d_rest, my_id())) {
					XIO_WRN("PREFER LOGFILE '%s' in front of '%s'\n",
						rot->next_relevant_log->d_path,
						dent->d_path);
				} else if (!strcmp(dent->d_rest, my_id())) {
					XIO_WRN("PREFER LOGFILE '%s' in front of '%s'\n",
						dent->d_path,
						rot->next_relevant_log->d_path);
					rot->next_relevant_log = dent;
				} else {
					rot->has_double_logfile = true;
					XIO_ERR("DOUBLE LOGFILES '%s' '%s'\n",
						dent->d_path,
						rot->next_relevant_log->d_path);
				}
			} else if (dent->d_serial > rot->next_relevant_log->d_serial + 5) {
				rot->has_hole_logfile = true;
			}

		}
		XIO_DBG("next_relevant_log = %p\n", rot->next_relevant_log);
		goto ok;
	}

	/* Preconditions
	 */
	if (!rot->replay_link || !rot->aio_dent || !rot->aio_brick) {
		XIO_DBG("nothing to do on '%s'\n", dent->d_path);
		goto ok;
	}

	/* Remember the relevant log.
	 */
	if (!rot->relevant_log && rot->aio_dent->d_serial == dent->d_serial) {
		rot->relevant_serial = dent->d_serial;
		rot->relevant_log = dent;
		rot->has_double_logfile = false;
		rot->has_hole_logfile = false;
	}

ok:
	/* All ok: switch over the indicators.
	 */
	XIO_DBG("next_log = '%s'\n", dent->d_path);
	rot->prev_log = rot->next_log;
	rot->next_log = dent;

done:
	if (status < 0) {
		XIO_DBG("rot_error status = %d\n", status);
		rot->has_error = true;
	}
err:
	return status;
}

/* Internal helper. Return codes:
 * ret < 0 : error
 * ret == 0 : not relevant
 * ret == 1 : relevant, no transaction replay, switch to the next
 * ret == 2 : relevant for transaction replay
 * ret == 3 : relevant for appending
 */
static
int _check_logging_status(struct mars_rotate *rot,
	int *log_nr,
	long long *oldpos_start,
	long long *oldpos_end,
	long long *newpos)
{
	struct mars_dent *dent = rot->relevant_log;
	struct mars_dent *parent;
	struct mars_global *global = NULL;
	int status = 0;

	if (!dent)
		goto done;

	status = -EINVAL;
	parent = dent->d_parent;
	CHECK_PTR(parent, done);
	global = rot->global;
	CHECK_PTR_NULL(global, done);
	CHECK_PTR(rot->replay_link, done);
	CHECK_PTR(rot->aio_brick, done);
	CHECK_PTR(rot->aio_dent, done);

	XIO_DBG("    dent = '%s'\n", dent->d_path);
	XIO_DBG("aio_dent = '%s'\n", rot->aio_dent->d_path);
	if (unlikely(strcmp(dent->d_path, rot->aio_dent->d_path)))
		goto done;

	if (sscanf(rot->replay_link->d_argv[0], "log-%d", log_nr) != 1) {
		XIO_ERR_TO(rot->log_say,
			"replay link has malformed logfile number '%s'\n",
			rot->replay_link->d_argv[0]);
		goto done;
	}
	if (kstrtos64(rot->replay_link->d_argv[1], 10, oldpos_start)) {
		XIO_ERR_TO(rot->log_say,
			"replay link has bad start position argument '%s'\n",
			rot->replay_link->d_argv[1]);
		goto done;
	}
	if (kstrtos64(rot->replay_link->d_argv[2], 10, oldpos_end)) {
		XIO_ERR_TO(rot->log_say,
			"replay link has bad end position argument '%s'\n",
			rot->replay_link->d_argv[2]);
		goto done;
	}
	*oldpos_end += *oldpos_start;
	if (unlikely(*oldpos_end < *oldpos_start)) {
		XIO_ERR_TO(rot->log_say, "replay link end_pos %lld < start_pos %lld\n", *oldpos_end, *oldpos_start);
		/*  safety: use the smaller value, it does not hurt */
		*oldpos_start = *oldpos_end;
		if (unlikely(*oldpos_start < 0))
			*oldpos_start = 0;
	}

	*newpos = rot->aio_info.current_size;

	if (unlikely(rot->aio_info.current_size < *oldpos_start)) {
		XIO_ERR_TO(rot->log_say,
			"oops, bad replay position attempted at logfile '%s' (file length %lld should never be smaller than requested position %lld, is your filesystem corrupted?) => please repair this by hand\n",
			rot->aio_dent->d_path,
			rot->aio_info.current_size,
			*oldpos_start);
		make_rot_msg(rot,
			"err-replay-size",
			"oops, bad replay position attempted at logfile '%s' (file length %lld should never be smaller than requested position %lld, is your filesystem corrupted?) => please repair this by hand",
			rot->aio_dent->d_path,
			rot->aio_info.current_size,
			*oldpos_start);
		status = -EBADF;
		goto done;
	}

	status = 0;
	if (rot->aio_info.current_size > *oldpos_start) {
		if (rot->aio_info.current_size - *oldpos_start < REPLAY_TOLERANCE &&
		    (rot->todo_primary ||
			(rot->relevant_log &&
			 rot->next_relevant_log &&
			 is_switchover_possible(rot,
				 rot->relevant_log->d_path,
				 rot->next_relevant_log->d_path,
				 _get_tolerance(rot),
				 false)))) {
			XIO_INF_TO(rot->log_say,
				"TOLERANCE: transaction log '%s' is treated as fully applied\n",
				rot->aio_dent->d_path);
			make_rot_msg(rot,
				"inf-replay-tolerance",
				"TOLERANCE: transaction log '%s' is treated as fully applied",
				rot->aio_dent->d_path);
			status = 1;
		} else {
			XIO_INF_TO(rot->log_say,
				"transaction log replay is necessary on '%s' from %lld to %lld (dirty region ends at %lld)\n",
				rot->aio_dent->d_path,
				*oldpos_start,
				rot->aio_info.current_size,
				*oldpos_end);
			status = 2;
		}
	} else if (rot->next_relevant_log) {
		XIO_INF_TO(rot->log_say,
			"transaction log '%s' is already applied, and the next one is available for switching\n",
			rot->aio_dent->d_path);
		status = 1;
	} else if (rot->todo_primary) {
		if (rot->aio_info.current_size > 0 || strcmp(dent->d_rest, my_id()) != 0) {
			XIO_INF_TO(rot->log_say,
				"transaction log '%s' is already applied (would be usable for appending at position %lld, but a fresh logfile will be used for safety reasons)\n",
				rot->aio_dent->d_path,
				*oldpos_end);
			status = 1;
		} else {
			XIO_INF_TO(rot->log_say,
				"empty transaction log '%s' is usable for me as a primary node\n",
				rot->aio_dent->d_path);
			status = 3;
		}
	} else {
		XIO_DBG("transaction log '%s' is the last one, currently fully applied\n", rot->aio_dent->d_path);
		status = 0;
	}

done:
	return status;
}

static
int _make_logging_status(struct mars_rotate *rot)
{
	struct mars_dent *dent = rot->relevant_log;
	struct mars_dent *parent;
	struct mars_global *global = NULL;
	struct trans_logger_brick *trans_brick;
	int log_nr = 0;
	loff_t start_pos = 0;
	loff_t dirty_pos = 0;
	loff_t end_pos = 0;
	int status = 0;

	if (!dent)
		goto done;

	status = -EINVAL;
	parent = dent->d_parent;
	CHECK_PTR(parent, done);
	global = rot->global;
	CHECK_PTR_NULL(global, done);

	status = 0;
	trans_brick = rot->trans_brick;
	if (!global->global_power.button || !trans_brick || rot->has_error) {
		XIO_DBG("nothing to do rot_error = %d\n", rot->has_error);
		goto done;
	}

	/* Find current logging status.
	 */
	status = _check_logging_status(rot, &log_nr, &start_pos, &dirty_pos, &end_pos);
	XIO_DBG("case = %d (todo_primary=%d is_primary=%d old_is_primary=%d)\n",
		status,
		rot->todo_primary,
		rot->is_primary,
		rot->old_is_primary);
	if (status < 0)
		goto done;
	if (unlikely(start_pos < 0 || dirty_pos < start_pos || end_pos < dirty_pos)) {
		XIO_ERR_TO(rot->log_say,
			"replay symlink has implausible values: start_pos = %lld dirty_pos = %lld end_pos = %lld\n",
			start_pos,
			dirty_pos,
			end_pos);
	}
	/* Relevant or not?
	 */
	switch (status) {
	case 0: /*  not relevant */
		goto ok;
	case 1: /* Relevant, and transaction replay already finished.
		 * Allow switching over to a new logfile.
		 */
		if (!trans_brick->power.button && !trans_brick->power.on_led && trans_brick->power.off_led) {
			if (rot->next_relevant_log) {
				int replay_tolerance = _get_tolerance(rot);
				bool skip_new = !!rot->todo_primary;

				XIO_DBG("check switchover from '%s' to '%s' (size = %lld, skip_new = %d, replay_tolerance = %d)\n",
					dent->d_path,
					rot->next_relevant_log->d_path,
					rot->next_relevant_log->stat_val.size,
					skip_new,
					replay_tolerance);
				if (is_switchover_possible(rot,
					dent->d_path,
					rot->next_relevant_log->d_path,
					replay_tolerance,
					skip_new) ||
				    (skip_new && !_check_allow(global, parent, "connect"))) {
					XIO_INF_TO(rot->log_say,
						"start switchover from transaction log '%s' to '%s'\n",
						dent->d_path,
						rot->next_relevant_log->d_path);
					_make_new_replaylink(rot,
						rot->next_relevant_log->d_rest,
						rot->next_relevant_log->d_serial,
						rot->next_relevant_log->stat_val.size);
				} else if (!_check_allow(global, parent, "connect")) {
					char *new_path = path_make("%s/log-%09d-%s",

						parent->d_path,
						log_nr + 1,
						my_id());
					if (strcmp(new_path, rot->next_relevant_log->d_path)) {
						XIO_WRN("FORCING PRIMARY LOGFILE '%s'\n", new_path);
						_create_new_logfile(new_path);
					}
					brick_string_free(new_path);
				}
			} else if (rot->todo_primary) {
				if (dent->d_serial > log_nr)
					log_nr = dent->d_serial;
				XIO_INF_TO(rot->log_say,
					"preparing new transaction log, number moves from %d to %d\n",
					dent->d_serial,
					log_nr + 1);
				_make_new_replaylink(rot, my_id(), log_nr + 1, 0);
			} else {
				XIO_DBG("nothing to do on last transaction log '%s'\n", dent->d_path);
			}
		}
		status = -EAGAIN;
		goto done;
	case 2: /*  relevant for transaction replay */
		XIO_INF_TO(rot->log_say,
			"replaying transaction log '%s' from position %lld to %lld\n",
			dent->d_path,
			start_pos,
			end_pos);
		rot->replay_mode = true;
		rot->start_pos = start_pos;
		rot->end_pos = end_pos;
		break;
	case 3: /*  relevant for appending */
		XIO_INF_TO(rot->log_say, "appending to transaction log '%s'\n", dent->d_path);
		rot->replay_mode = false;
		rot->start_pos = 0;
		rot->end_pos = 0;
		break;
	default:
		XIO_ERR_TO(rot->log_say, "bad internal status %d\n", status);
		status = -EINVAL;
		goto done;
	}

ok:
	/* All ok: switch over the indicators.
	 */
	rot->prev_log = rot->next_log;
	rot->next_log = dent;

done:
	if (status < 0) {
		XIO_DBG("rot_error status = %d\n", status);
		rot->has_error = true;
	}
	return status;
}

static
void _init_trans_input(struct trans_logger_input *trans_input, struct mars_dent *log_dent, struct mars_rotate *rot)
{
	if (unlikely(trans_input->connect || trans_input->is_operating)) {
		XIO_ERR("this should not happen\n");
		goto out_return;
	}

	memset(&trans_input->inf, 0, sizeof(trans_input->inf));

	strncpy(trans_input->inf.inf_host, log_dent->d_rest, sizeof(trans_input->inf.inf_host));
	trans_input->inf.inf_sequence = log_dent->d_serial;
	trans_input->inf.inf_private = rot;
	trans_input->inf.inf_callback = _update_info;
	XIO_DBG("initialized '%s' %d\n", trans_input->inf.inf_host, trans_input->inf.inf_sequence);
out_return:;
}

static
int _get_free_input(struct trans_logger_brick *trans_brick)
{
	int nr = (((trans_brick->log_input_nr - TL_INPUT_LOG1) + 1) % 2) + TL_INPUT_LOG1;
	struct trans_logger_input *candidate;

	candidate = trans_brick->inputs[nr];
	if (unlikely(!candidate)) {
		XIO_ERR("input nr = %d is corrupted!\n", nr);
		return -EEXIST;
	}
	if (unlikely(candidate->is_operating || candidate->connect)) {
		XIO_DBG("nr = %d unusable! is_operating = %d connect = %p\n",
			nr,
			candidate->is_operating,
			candidate->connect);
		return -EEXIST;
	}
	XIO_DBG("got nr = %d\n", nr);
	return nr;
}

static
void _rotate_trans(struct mars_rotate *rot)
{
	struct trans_logger_brick *trans_brick = rot->trans_brick;
	int old_nr = trans_brick->old_input_nr;
	int log_nr = trans_brick->log_input_nr;
	int next_nr;

	XIO_DBG("log_input_nr = %d old_input_nr = %d next_relevant_log = %p\n",
		log_nr,
		old_nr,
		rot->next_relevant_log);

	/*  try to cleanup old log */
	if (log_nr != old_nr) {
		struct trans_logger_input *trans_input = trans_brick->inputs[old_nr];
		struct trans_logger_input *new_input = trans_brick->inputs[log_nr];

		if (!trans_input->connect) {
			XIO_DBG("ignoring unused old input %d\n", old_nr);
		} else if (!new_input->is_operating) {
			XIO_DBG("ignoring uninitialized new input %d\n", log_nr);
		} else if (trans_input->is_operating &&
			   trans_input->inf.inf_min_pos == trans_input->inf.inf_max_pos &&
			   list_empty(&trans_input->pos_list) &&
			   atomic_read(&trans_input->log_obj_count) <= 0) {
			int status;

			XIO_INF("cleanup old transaction log (%d -> %d)\n", old_nr, log_nr);
			status = generic_disconnect((void *)trans_input);
			if (unlikely(status < 0))
				XIO_ERR("disconnect failed\n");
			else
				remote_trigger();
		} else {
			XIO_DBG("old transaction replay not yet finished: is_operating = %d pos %lld != %lld\n",
				 trans_input->is_operating,
				 trans_input->inf.inf_min_pos,
				 trans_input->inf.inf_max_pos);
		}
	} else
	/*  try to setup new log */
	if (log_nr == trans_brick->new_input_nr &&
	    rot->next_relevant_log &&
	    (rot->next_relevant_log->d_serial == trans_brick->inputs[log_nr]->inf.inf_sequence + 1 ||
	     trans_brick->cease_logging)) {
		struct trans_logger_input *trans_input;
		int status;

		next_nr = _get_free_input(trans_brick);
		if (unlikely(next_nr < 0)) {
			XIO_ERR_TO(rot->log_say, "no free input\n");
			goto done;
		}

		XIO_DBG("start switchover %d -> %d\n", old_nr, next_nr);

		rot->next_relevant_brick =
			make_brick_all(rot->global,
				       rot->next_relevant_log,
				       _set_sio_params,
				       NULL,
				       rot->next_relevant_log->d_path,
				       (const struct generic_brick_type *)&sio_brick_type,
				       (const struct generic_brick_type *[]){},
				       2, /*  create + activate */
				       rot->next_relevant_log->d_path,
				       (const char *[]){},
				       0);
		if (unlikely(!rot->next_relevant_brick)) {
			XIO_ERR_TO(rot->log_say,
				"could not open next transaction log '%s'\n",
				rot->next_relevant_log->d_path);
			goto done;
		}
		trans_input = trans_brick->inputs[next_nr];
		if (unlikely(!trans_input)) {
			XIO_ERR_TO(rot->log_say, "internal log input does not exist\n");
			goto done;
		}

		_init_trans_input(trans_input, rot->next_relevant_log, rot);

		status = generic_connect((void *)trans_input, (void *)rot->next_relevant_brick->outputs[0]);
		if (unlikely(status < 0)) {
			XIO_ERR_TO(rot->log_say, "internal connect failed\n");
			goto done;
		}
		trans_brick->new_input_nr = next_nr;
		XIO_INF_TO(rot->log_say,
			"started logrotate switchover from '%s' to '%s'\n",
			rot->relevant_log->d_path,
			rot->next_relevant_log->d_path);
	}
done:;
}

static
void _change_trans(struct mars_rotate *rot)
{
	struct trans_logger_brick *trans_brick = rot->trans_brick;

	XIO_DBG("replay_mode = %d start_pos = %lld end_pos = %lld\n",
		trans_brick->replay_mode,
		rot->start_pos,
		rot->end_pos);

	if (trans_brick->replay_mode) {
		trans_brick->replay_start_pos = rot->start_pos;
		trans_brick->replay_end_pos = rot->end_pos;
	} else {
		_rotate_trans(rot);
	}
}

static
int _start_trans(struct mars_rotate *rot)
{
	struct trans_logger_brick *trans_brick;
	struct trans_logger_input *trans_input;
	int nr;
	int status;

	/* Internal safety checks
	 */
	status = -EINVAL;
	if (unlikely(!rot)) {
		XIO_ERR("rot is NULL\n");
		goto done;
	}
	if (unlikely(!rot->aio_brick || !rot->relevant_log)) {
		XIO_ERR("aio %p or relevant log %p is missing, this should not happen\n",
			rot->aio_brick,
			rot->relevant_log);
		goto done;
	}
	trans_brick = rot->trans_brick;
	if (unlikely(!trans_brick)) {
		XIO_ERR("logger instance does not exist\n");
		goto done;
	}

	/* Update status when already working
	 */
	if (trans_brick->power.button || !trans_brick->power.off_led) {
		_change_trans(rot);
		status = 0;
		goto done;
	}

	/* Further safety checks.
	 */
	if (unlikely(rot->relevant_brick)) {
		XIO_ERR("log aio brick already present, this should not happen\n");
		goto done;
	}
	if (unlikely(trans_brick->inputs[TL_INPUT_LOG1]->is_operating || trans_brick->inputs[TL_INPUT_LOG2]->is_operating)) {
		XIO_ERR("some input is operating, this should not happen\n");
		goto done;
	}

	/* Allocate new input slot
	 */
	nr = _get_free_input(trans_brick);
	if (unlikely(nr < TL_INPUT_LOG1 || nr > TL_INPUT_LOG2)) {
		XIO_ERR("bad new_input_nr = %d\n", nr);
		goto done;
	}
	trans_brick->new_input_nr = nr;
	trans_brick->old_input_nr = nr;
	trans_brick->log_input_nr = nr;
	trans_input = trans_brick->inputs[nr];
	if (unlikely(!trans_input)) {
		XIO_ERR("log input %d does not exist\n", nr);
		goto done;
	}

	/* Open new transaction log
	 */
	rot->relevant_brick =
		make_brick_all(rot->global,
			       rot->relevant_log,
			       _set_sio_params,
			       NULL,
			       rot->relevant_log->d_path,
			       (const struct generic_brick_type *)&sio_brick_type,
			       (const struct generic_brick_type *[]){},
			       2, /*  start always */
			       rot->relevant_log->d_path,
			       (const char *[]){},
			       0);
	if (unlikely(!rot->relevant_brick)) {
		XIO_ERR("log aio brick '%s' not open\n", rot->relevant_log->d_path);
		goto done;
	}

	/* Supply all relevant parameters
	 */
	trans_brick->replay_mode = rot->replay_mode;
	trans_brick->replay_tolerance = REPLAY_TOLERANCE;
	_init_trans_input(trans_input, rot->relevant_log, rot);

	/* Connect to new transaction log
	 */
	status = generic_connect((void *)trans_input, (void *)rot->relevant_brick->outputs[0]);
	if (unlikely(status < 0)) {
		XIO_ERR("initial connect failed\n");
		goto done;
	}

	_change_trans(rot);

	/* Switch on....
	 */
	status = mars_power_button((void *)trans_brick, true, false);
	XIO_DBG("status = %d\n", status);

done:
	return status;
}

static
int _stop_trans(struct mars_rotate *rot, const char *parent_path)
{
	struct trans_logger_brick *trans_brick = rot->trans_brick;
	int status = 0;

	if (!trans_brick)
		goto done;

	/* Switch off temporarily....
	 */
	status = mars_power_button((void *)trans_brick, false, false);
	XIO_DBG("status = %d\n", status);
	if (status < 0)
		goto done;

	/* Disconnect old connection(s)
	 */
	if (trans_brick->power.off_led) {
		int i;

		for (i = TL_INPUT_LOG1; i <= TL_INPUT_LOG2; i++) {
			struct trans_logger_input *trans_input;

			trans_input = trans_brick->inputs[i];
			if (trans_input && !trans_input->is_operating) {
				if (trans_input->connect)
					(void)generic_disconnect((void *)trans_input);
			}
		}
	}

done:
	return status;
}

static
int make_log_finalize(struct mars_global *global, struct mars_dent *dent)
{
	struct mars_dent *parent = dent->d_parent;
	struct mars_rotate *rot;
	struct trans_logger_brick *trans_brick;
	struct copy_brick *fetch_brick;
	bool is_attached;
	bool is_stopped;
	int status = -EINVAL;

	CHECK_PTR(parent, err);
	rot = parent->d_private;
	if (!rot)
		goto err;
	CHECK_PTR(rot, err);
	rot->has_symlinks = true;
	trans_brick = rot->trans_brick;
	status = 0;
	if (!trans_brick) {
		XIO_DBG("nothing to do\n");
		goto done;
	}

	/* Handle jamming (a very exceptional state)
	 */
	if (IS_JAMMED()) {
#ifndef CONFIG_MARS_DEBUG
		brick_say_logging = 0;
#endif
		rot->has_emergency = true;
		/* Report remote errors to clients when they
		 * try to sync during emergency mode.
		 */
		if (rot->bio_brick && rot->bio_brick->mode_ptr)
			*rot->bio_brick->mode_ptr = -EMEDIUMTYPE;
		XIO_ERR_TO(rot->log_say, "DISK SPACE IS EXTREMELY LOW on %s\n", rot->parent_path);
		make_rot_msg(rot, "err-space-low", "DISK SPACE IS EXTREMELY LOW");
	} else if (IS_EXHAUSTED() && rot->has_emergency) {
		XIO_ERR_TO(rot->log_say,
			"EMEGENCY MODE HYSTERESIS on %s: you need to free more space for recovery.\n",
			rot->parent_path);
		make_rot_msg(rot,
			"err-space-low",
			"EMEGENCY MODE HYSTERESIS: you need to free more space for recovery.");
	} else {
		int limit = _check_allow(global, parent, "emergency-limit");

		rot->has_emergency = (limit > 0 && global_remaining_space * 100 / global_total_space < limit);
		XIO_DBG("has_emergency=%d limit=%d remaining_space=%lld total_space=%lld\n",
			 rot->has_emergency, limit, global_remaining_space, global_total_space);
		if (!rot->has_emergency && rot->bio_brick && rot->bio_brick->mode_ptr)
			*rot->bio_brick->mode_ptr = 0;
	}
	_show_actual(parent->d_path, "has-emergency", rot->has_emergency);
	if (rot->has_emergency) {
		if (rot->todo_primary || rot->is_primary) {
			trans_brick->cease_logging = true;
			rot->inf_prev_sequence = 0; /*	disable checking */
		}
	} else {
		if (!trans_logger_resume) {
			XIO_INF_TO(rot->log_say,
				"emergency mode on %s could be turned off now, but /proc/sys/mars/logger_resume inhibits it.\n",
				rot->parent_path);
		} else {
			trans_brick->cease_logging = false;
			XIO_INF_TO(rot->log_say, "emergency mode on %s will be turned off again\n", rot->parent_path);
		}
	}
	is_stopped = trans_brick->cease_logging | trans_brick->stopped_logging;
	_show_actual(parent->d_path, "is-emergency", is_stopped);
	if (is_stopped) {
		XIO_ERR_TO(rot->log_say,
			"EMERGENCY MODE on %s: stopped transaction logging, and created a hole in the logfile sequence nubers.\n",
			rot->parent_path);
		make_rot_msg(rot,
			"err-emergency",
			"EMERGENCY MODE on %s: stopped transaction logging, and created a hole in the logfile sequence nubers.\n",
			rot->parent_path);
		/* Create a hole in the sequence of logfile numbers.
		 * The secondaries will later stumble over it.
		 */
		if (!rot->created_hole) {
			int new_sequence = rot->max_sequence + 10;
			char *new_vers = path_make("%s/version-%09d-%s", rot->parent_path, new_sequence, my_id());

			char *new_vval = path_make("00000000000000000000000000000000,log-%09d-%s,0:",

				new_sequence,
				my_id());
			char *new_path = path_make("%s/log-%09d-%s", rot->parent_path, new_sequence + 1, my_id());

			if (likely(new_vers && new_vval && new_path &&
				   !mars_find_dent(global, new_path))) {
				XIO_INF_TO(rot->log_say, "EMERGENCY: creating new logfile '%s'\n", new_path);
				mars_symlink(new_vval, new_vers, NULL, 0);
				_create_new_logfile(new_path);
				rot->created_hole = true;
			}
			brick_string_free(new_vers);
			brick_string_free(new_vval);
			brick_string_free(new_path);
		}
	} else {
		rot->created_hole = false;
	}

	if (IS_EMERGENCY_SECONDARY()) {
		if (!rot->todo_primary && rot->first_log && rot->first_log != rot->relevant_log) {
			XIO_WRN_TO(rot->log_say,
				"EMERGENCY: ruthlessly freeing old logfile '%s', don't cry on any ramifications.\n",
				rot->first_log->d_path);
			make_rot_msg(rot,
				"wrn-space-low",
				"EMERGENCY: ruthlessly freeing old logfile '%s'",
				rot->first_log->d_path);
			mars_unlink(rot->first_log->d_path);
			rot->first_log->d_killme = true;
			/*  give it a chance to cease deleting next time */
			compute_emergency_mode();
		} else if (IS_EMERGENCY_PRIMARY()) {
			XIO_WRN_TO(rot->log_say, "EMERGENCY: the space on /mars/ is VERY low.\n");
			make_rot_msg(rot, "wrn-space-low", "EMERGENCY: the space on /mars/ is VERY low.");
		} else {
			XIO_WRN_TO(rot->log_say, "EMERGENCY: the space on /mars/ is low.\n");
			make_rot_msg(rot, "wrn-space-low", "EMERGENCY: the space on /mars/ is low.");
		}
	} else if (IS_EXHAUSTED()) {
		XIO_WRN_TO(rot->log_say, "EMERGENCY: the space on /mars/ is becoming low.\n");
		make_rot_msg(rot, "wrn-space-low", "EMERGENCY: the space on /mars/ is becoming low.");
	}

	if (trans_brick->replay_mode) {
		if (trans_brick->replay_code > 0) {
			XIO_INF_TO(rot->log_say,
				"logfile replay ended successfully at position %lld\n",
				trans_brick->replay_current_pos);
		} else if (trans_brick->replay_code == -EAGAIN ||
			   trans_brick->replay_end_pos - trans_brick->replay_current_pos < trans_brick->replay_tolerance) {
			XIO_INF_TO(rot->log_say,
				"logfile replay stopped intermediately at position %lld\n",
				trans_brick->replay_current_pos);
		} else if (trans_brick->replay_code < 0) {
			XIO_ERR_TO(rot->log_say,
				"logfile replay stopped with error = %d at position %lld\n",
				trans_brick->replay_code,
				trans_brick->replay_current_pos);
			make_rot_msg(rot,
				"err-replay-stop",
				"logfile replay stopped with error = %d at position %lld",
				trans_brick->replay_code,
				trans_brick->replay_current_pos);
		}
	}

	/* Stopping is also possible in case of errors
	 */
	if (trans_brick->power.button && trans_brick->power.on_led && !trans_brick->power.off_led) {
		bool do_stop = true;

		if (trans_brick->replay_mode) {
			rot->is_log_damaged =
				trans_brick->replay_code == -EAGAIN &&
				trans_brick->replay_end_pos - trans_brick->replay_current_pos < trans_brick->replay_tolerance;
			do_stop = trans_brick->replay_code != 0 ||
				!global->global_power.button ||
				!_check_allow(global, parent, "allow-replay") ||
				!_check_allow(global, parent, "attach");
		} else {
			do_stop =
				!rot->if_brick &&
				!rot->is_primary &&
				(!rot->todo_primary ||
				 !_check_allow(global, parent, "attach"));
		}

		XIO_DBG("replay_mode = %d replay_code = %d is_primary = %d do_stop = %d\n",
			trans_brick->replay_mode,
			trans_brick->replay_code,
			rot->is_primary,
			(int)do_stop);

		if (do_stop)
			status = _stop_trans(rot, parent->d_path);
		else
			_change_trans(rot);
		goto done;
	}

	/* Starting is only possible when no error occurred.
	 */
	if (!rot->relevant_log || rot->has_error) {
		XIO_DBG("nothing to do\n");
		goto done;
	}

	/* Start when necessary
	 */
	if (!trans_brick->power.button && !trans_brick->power.on_led && trans_brick->power.off_led) {
		bool do_start;

		status = _make_logging_status(rot);
		if (status <= 0)
			goto done;

		rot->is_log_damaged = false;

		do_start = (!rot->replay_mode ||
			    (rot->start_pos != rot->end_pos &&
			     _check_allow(global, parent, "allow-replay")));

		if (do_start && rot->forbid_replay) {
			XIO_INF("cannot start replay because sync wants to start\n");
			make_rot_msg(rot, "inf-replay-start", "cannot start replay because sync wants to star");
			do_start = false;
		}

		if (do_start && rot->sync_brick && !rot->sync_brick->power.off_led) {
			XIO_INF("cannot start replay because sync is running\n");
			make_rot_msg(rot, "inf-replay-start", "cannot start replay because sync is running");
			do_start = false;
		}

		XIO_DBG("rot->replay_mode = %d rot->start_pos = %lld rot->end_pos = %lld | do_start = %d\n",
			rot->replay_mode,
			rot->start_pos,
			rot->end_pos,
			do_start);

		if (do_start)
			status = _start_trans(rot);
	}

done:
	/*  check whether some copy has finished */
	fetch_brick = (struct copy_brick *)mars_find_brick(global, &copy_brick_type, rot->fetch_path);
	XIO_DBG("fetch_path = '%s' fetch_brick = %p\n", rot->fetch_path, fetch_brick);
	if (fetch_brick &&
	    (fetch_brick->power.off_led ||
	     !global->global_power.button ||
	     !_check_allow(global, parent, "connect") ||
	     !_check_allow(global, parent, "attach") ||
	     (fetch_brick->copy_last == fetch_brick->copy_end &&
	      (rot->fetch_next_is_available > 0 ||
	       rot->fetch_round++ > 3)))) {
		int i;

		for (i = 0; i < 4; i++) {
			if (fetch_brick->inputs[i] && fetch_brick->inputs[i]->brick)
				fetch_brick->inputs[i]->brick->power.io_timeout = 1;
		}
		status = xio_kill_brick((void *)fetch_brick);
		if (status < 0)
			XIO_ERR("could not kill fetch_brick, status = %d\n", status);
		else
			fetch_brick = NULL;
		local_trigger();
	}
	rot->fetch_next_is_available = 0;
	rot->fetch_brick = fetch_brick;
	if (fetch_brick)
		fetch_brick->kill_ptr = (void **)&rot->fetch_brick;
	else
		rot->fetch_serial = 0;
	/*  remove trans_logger (when possible) upon detach */
	is_attached = !!rot->trans_brick;
	_show_actual(rot->parent_path, "is-attached", is_attached);

	if (rot->trans_brick && rot->trans_brick->power.off_led && !rot->trans_brick->outputs[0]->nr_connected) {
		bool do_attach = _check_allow(global, parent, "attach");

		XIO_DBG("do_attach = %d\n", do_attach);
		if (!do_attach) {
			rot->trans_brick->killme = true;
			rot->trans_brick = NULL;
		}
	}

	_show_actual(rot->parent_path,
		"is-replaying",
		rot->trans_brick && rot->trans_brick->replay_mode && !rot->trans_brick->power.off_led);
	_show_rate(rot, &rot->replay_limiter, "replay_rate");
	_show_actual(rot->parent_path, "is-copying", rot->fetch_brick && !rot->fetch_brick->power.off_led);
	_show_rate(rot, &rot->fetch_limiter, "file_rate");
	_show_actual(rot->parent_path, "is-syncing", rot->sync_brick && !rot->sync_brick->power.off_led);
	_show_rate(rot, &rot->sync_limiter, "sync_rate");
err:
	return status;
}

/*********************************************************************/

/*  specific handlers */

static
int make_primary(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_dent *parent;
	struct mars_rotate *rot;
	int status = -EINVAL;

	parent = dent->d_parent;
	CHECK_PTR(parent, done);
	rot = parent->d_private;
	if (!rot)
		goto done;
	CHECK_PTR(rot, done);

	rot->has_symlinks = true;

	rot->todo_primary =
		global->global_power.button && dent->link_val && !strcmp(dent->link_val, my_id());
	XIO_DBG("todo_primary = %d is_primary = %d\n", rot->todo_primary, rot->is_primary);
	status = 0;

done:
	return status;
}

static
int make_bio(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_rotate *rot;
	struct xio_brick *brick;
	bool switch_on;
	int status = 0;

	if (!global || !dent->d_parent)
		goto done;
	rot = dent->d_parent->d_private;
	if (!rot)
		goto done;

	rot->has_symlinks = true;

	switch_on = _check_allow(global, dent->d_parent, "attach");
	if (switch_on && rot->res_shutdown) {
		XIO_ERR("cannot access disk: resource shutdown mode is currently active\n");
		switch_on = false;
	}

	brick =
		make_brick_all(global,
			       dent,
			       _set_bio_params,
			       NULL,
			       dent->d_path,
			       (const struct generic_brick_type *)&bio_brick_type,
			       (const struct generic_brick_type *[]){},
			       switch_on ? 2 : -1,
			       dent->d_path,
			       (const char *[]){},
			       0);
	rot->bio_brick = brick;
	if (unlikely(!brick)) {
		status = -ENXIO;
		goto done;
	}

	/* Report the actual size of the device.
	 * It may be larger than the global size.
	 */
	if (brick && brick->power.on_led) {
		struct xio_info info = {};
		struct xio_output *output;
		char *src = NULL;
		char *dst = NULL;

		output = brick->outputs[0];
		status = output->ops->xio_get_info(output, &info);
		if (status < 0) {
			XIO_ERR("cannot get info on '%s'\n", dent->d_path);
			goto done;
		}
		src = path_make("%lld", info.current_size);
		dst = path_make("%s/actsize-%s", dent->d_parent->d_path, my_id());
		if (src && dst)
			(void)mars_symlink(src, dst, NULL, 0);
		brick_string_free(src);
		brick_string_free(dst);
	}

done:
	return status;
}

static int make_replay(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_dent *parent = dent->d_parent;
	int status = 0;

	if (!global->global_power.button || !parent || !dent->link_val) {
		XIO_DBG("nothing to do\n");
		goto done;
	}

	status = make_log_finalize(global, dent);
	if (status < 0) {
		XIO_DBG("logger not initialized\n");
		goto done;
	}

done:
	return status;
}

static
int make_dev(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_dent *parent = dent->d_parent;
	struct mars_rotate *rot = NULL;
	struct xio_brick *dev_brick;
	struct if_brick *_dev_brick;
	char *dev_name = NULL;
	bool switch_on;
	int open_count = 0;
	int status = 0;

	if (!parent || !dent->link_val) {
		XIO_ERR("nothing to do\n");
		return -EINVAL;
	}
	rot = parent->d_private;
	if (!rot || !rot->parent_path) {
		XIO_DBG("nothing to do\n");
		goto err;
	}
	rot->has_symlinks = true;
	if (!rot->trans_brick) {
		XIO_DBG("transaction logger does not exist\n");
		goto done;
	}
	if (rot->dev_size <= 0) {
		XIO_WRN("trying to create device '%s' with zero size\n", dent->d_path);
		goto done;
	}

	status = _parse_args(dent, dent->link_val, 1);
	if (status < 0) {
		XIO_DBG("fail\n");
		goto done;
	}

	dev_name = path_make("mars/%s", dent->d_argv[0]);

	switch_on =
		(rot->if_brick && atomic_read(&rot->if_brick->open_count) > 0) ||
		(rot->todo_primary &&
		 !rot->trans_brick->replay_mode &&
		 rot->trans_brick->power.on_led &&
		 _check_allow(global, dent->d_parent, "attach"));
	if (!global->global_power.button)
		switch_on = false;
	if (switch_on && rot->res_shutdown) {
		XIO_ERR("cannot create device: resource shutdown mode is currently active\n");
		switch_on = false;
	}

	dev_brick =
		make_brick_all(global,
			       dent,
			       _set_if_params,
			       rot,
			       dev_name,
			       (const struct generic_brick_type *)&if_brick_type,
			       (const struct generic_brick_type *[]){(const struct generic_brick_type *)&trans_logger_brick_type},
			       switch_on ? 2 : -1,
			       "%s/device-%s",
			       (const char *[]){"%s/replay-%s"},
			       1,
			       parent->d_path,
			       my_id(),
			       parent->d_path,
			       my_id());
	rot->if_brick = (void *)dev_brick;
	if (!dev_brick) {
		XIO_DBG("device not shown\n");
		goto done;
	}
	if (!switch_on) {
		XIO_DBG("setting killme on if_brick\n");
		dev_brick->killme = true;
	}
	dev_brick->kill_ptr = (void **)&rot->if_brick;
	dev_brick->show_status = _show_brick_status;
	_dev_brick = (void *)dev_brick;
	open_count = atomic_read(&_dev_brick->open_count);

done:
	__show_actual(rot->parent_path, "open-count", open_count);
	rot->is_primary =
		rot->if_brick && !rot->if_brick->power.off_led;
	_show_primary(rot, parent);

err:
	brick_string_free(dev_name);
	return status;
}

static
int kill_dev(void *buf, struct mars_dent *dent)
{
	struct mars_dent *parent = dent->d_parent;
	int status = kill_any(buf, dent);

	if (status > 0 && parent) {
		struct mars_rotate *rot = parent->d_private;

		if (rot)
			rot->if_brick = NULL;
	}
	return status;
}

static
int _update_syncstatus(struct mars_rotate *rot, struct copy_brick *copy, char *peer)
{
	const char *src = NULL;
	const char *dst = NULL;
	const char *syncpos_path = NULL;
	const char *peer_replay_path = NULL;
	const char *peer_replay_link = NULL;
	const char *peer_time_path = NULL;
	int status = -EINVAL;

	/* create syncpos symlink when necessary */
	if (copy->copy_last == copy->copy_end && !rot->sync_finish_stamp.tv_sec) {
		get_lamport(&rot->sync_finish_stamp);
		XIO_DBG("sync finished at timestamp %lu\n",
			 rot->sync_finish_stamp.tv_sec);
		/* Give the remote replay position a chance to become
		 * recent enough.
		 */
		remote_trigger();
		status = -EAGAIN;
		goto done;
	}
	if (rot->sync_finish_stamp.tv_sec) {
		struct kstat peer_time_stat = {};

		peer_time_path = path_make("/mars/tree-%s", peer);
		status = mars_stat(peer_time_path, &peer_time_stat, true);
		if (unlikely(status < 0)) {
			XIO_ERR("cannot stat '%s'\n", peer_time_path);
			goto done;
		}

		/* The syncpos tells us the replay position at the primary
		 * which was effective at the moment when the local sync was done.
		 * It is used to guarantee consistency:
		 * before our underlying disk is _really_ consistent, not only
		 * the sync must have finished, but additionally the local
		 * replay must have grown (at least) until the same position
		 * at which the primary was at that moment.
		 * Therefore, we have to remember the replay position of
		 * the primary at that moment.
		 * And because of the network delays we must ensure
		 * to get a recent enough remote version.
		 */
		syncpos_path = path_make("%s/syncpos-%s", rot->parent_path, my_id());
		peer_replay_path = path_make("%s/replay-%s", rot->parent_path, peer);
		peer_replay_link = mars_readlink(peer_replay_path);
		if (unlikely(!peer_replay_link || !peer_replay_link[0])) {
			XIO_ERR("cannot read peer replay link '%s'\n", peer_replay_path);
			goto done;
		}

		status = _update_link_when_necessary(rot, "syncpos", peer_replay_link, syncpos_path);
		/* Sync is only marked as finished when the syncpos
		 * production was successful and timestamps are recent enough.
		 */
		if (unlikely(status < 0))
			goto done;
		if (timespec_compare(&peer_time_stat.mtime, &rot->sync_finish_stamp) < 0) {
			XIO_INF("peer replay link '%s' is not recent enough (%lu < %lu)\n",
				 peer_replay_path,
				 peer_time_stat.mtime.tv_sec,
				 rot->sync_finish_stamp.tv_sec);
			remote_trigger();
			status = -EAGAIN;
			goto done;
		}
	}

	src = path_make("%lld", copy->copy_last);
	dst = path_make("%s/syncstatus-%s", rot->parent_path, my_id());

	status = _update_link_when_necessary(rot, "syncstatus", src, dst);

	brick_string_free(src);
	brick_string_free(dst);
	src = path_make("%lld,%lld", copy->verify_ok_count, copy->verify_error_count);
	dst = path_make("%s/verifystatus-%s", rot->parent_path, my_id());

	(void)_update_link_when_necessary(rot, "verifystatus", src, dst);

	memset(&rot->sync_finish_stamp, 0, sizeof(rot->sync_finish_stamp));
done:
	brick_string_free(src);
	brick_string_free(dst);
	brick_string_free(peer_replay_link);
	brick_string_free(peer_replay_path);
	brick_string_free(syncpos_path);
	brick_string_free(peer_time_path);
	return status;
}

static int make_sync(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_rotate *rot;
	loff_t start_pos = 0;
	loff_t end_pos = 0;
	struct mars_dent *size_dent;
	struct mars_dent *primary_dent;
	struct mars_dent *syncfrom_dent;
	char *peer;
	struct copy_brick *copy = NULL;
	char *tmp = NULL;
	const char *switch_path = NULL;
	const char *copy_path = NULL;
	const char *src = NULL;
	const char *dst = NULL;
	bool do_start;
	int status;

	if (!dent->d_parent || !dent->link_val)
		return 0;

	/* Determine peer
	 */
	tmp = path_make("%s/primary", dent->d_parent->d_path);
	primary_dent = (void *)mars_find_dent(global, tmp);
	if (!primary_dent || !primary_dent->link_val) {
		XIO_ERR("cannot determine primary, symlink '%s'\n", tmp);
		status = 0;
		goto done;
	}
	peer = primary_dent->link_val;

	do_start = _check_allow(global, dent->d_parent, "attach");

	/* Analyze replay position
	 */
	status = kstrtos64(dent->link_val, 10, &start_pos);
	if (unlikely(status)) {
		XIO_ERR("bad syncstatus symlink syntax '%s' (%s)\n", dent->link_val, dent->d_path);
		status = -EINVAL;
		goto done;
	}

	rot = dent->d_parent->d_private;
	status = -ENOENT;
	CHECK_PTR(rot, done);

	rot->forbid_replay = false;
	rot->has_symlinks = true;
	rot->allow_update = true;
	rot->syncstatus_dent = dent;

	/* Sync necessary?
	 */
	brick_string_free(tmp);
	tmp = path_make("%s/size", dent->d_parent->d_path);
	status = -ENOMEM;
	if (unlikely(!tmp))
		goto done;
	size_dent = (void *)mars_find_dent(global, tmp);
	if (!size_dent || !size_dent->link_val) {
		XIO_ERR("cannot determine size '%s'\n", tmp);
		status = -ENOENT;
		goto done;
	}
	status = kstrtos64(size_dent->link_val, 10, &end_pos);
	if (unlikely(status)) {
		XIO_ERR("bad size symlink syntax '%s' (%s)\n", size_dent->link_val, tmp);
		status = -EINVAL;
		goto done;
	}

	/* Is sync necessary at all?
	 */
	if (start_pos >= end_pos) {
		XIO_DBG("no data sync necessary, size = %lld\n", start_pos);
		do_start = false;
	}

	/* Handle final waiting step when finished
	 */
	if (rot->sync_finish_stamp.tv_sec && do_start)
		goto shortcut;

	/* Don't sync when logfiles are discontiguous
	 */
	if (do_start && (rot->has_double_logfile | rot->has_hole_logfile)) {
		XIO_WRN("no sync possible due to discontiguous logfiles (%d|%d)\n",
			 rot->has_double_logfile, rot->has_hole_logfile);
		if (do_start)
			start_pos = 0;
		do_start = false;
	}

	/* stop sync when primary is unknown
	 */
	if (!strcmp(peer, "(none)")) {
		XIO_INF("cannot start sync, no primary is designated\n");
		if (do_start)
			start_pos = 0;
		do_start = false;
	}

	/* Check syncfrom link (when existing)
	 */
	brick_string_free(tmp);
	tmp = path_make("%s/syncfrom-%s", dent->d_parent->d_path, my_id());
	syncfrom_dent = (void *)mars_find_dent(global, tmp);
	if (do_start && syncfrom_dent && syncfrom_dent->link_val &&
	    strcmp(syncfrom_dent->link_val, peer)) {
		XIO_WRN("cannot start sync, primary has changed: '%s' != '%s'\n",
			 syncfrom_dent->link_val, peer);
		if (do_start)
			start_pos = 0;
		do_start = false;
	}

	/* Disallow contemporary sync & logfile_replay
	 */
	if (do_start &&
	    rot->trans_brick &&
	    !rot->trans_brick->power.off_led) {
		XIO_INF("cannot start sync because logger is working\n");
		do_start = false;
	}

	/* Disallow overwrite of newer data
	 */
	if (do_start && compare_replaylinks(rot, peer, my_id()) < 0) {
		XIO_INF("cannot start sync because my data is newer than the remote one at '%s'!\n", peer);
		do_start = false;
		rot->forbid_replay = true;
	}

	/* Flip between replay and sync
	 */
	if (do_start && rot->replay_mode && rot->end_pos > rot->start_pos &&
	    mars_sync_flip_interval >= 8) {
		if (!rot->flip_start) {
			rot->flip_start = jiffies;
		} else if ((long long)jiffies - rot->flip_start > mars_sync_flip_interval * HZ) {
			do_start = false;
			rot->flip_start = jiffies + mars_sync_flip_interval * HZ;
		}
	} else {
		rot->flip_start = 0;
	}

	XIO_DBG("initial sync '%s' => '%s' do_start = %d\n", src, dst, do_start);
	/* Obey global sync limit
	 */
	rot->wants_sync = (do_start != 0);
	if (rot->wants_sync && global_sync_limit > 0) {
		do_start = rot->gets_sync;
		if (!rot->gets_sync) {
			XIO_INF_TO(rot->log_say,
				"won't start sync because of parallelism limit %d\n",
				global_sync_limit);
		}
	}

shortcut:
	/* Start copy
	 */
	src = path_make("data-%s@%s:%d", peer, peer, xio_net_default_port + 2);
	dst = path_make("data-%s", my_id());
	copy_path = backskip_replace(dent->d_path, '/', true, "/copy-");

	/*  check whether connection is allowed */
	switch_path = path_make("%s/todo-%s/sync", dent->d_parent->d_path, my_id());

	status = -ENOMEM;
	if (unlikely(!src || !dst || !copy_path || !switch_path))
		goto done;

	/* Informational
	 */
	XIO_DBG("start_pos = %lld end_pos = %lld sync_finish_stamp=%lu do_start=%d\n",
		 start_pos, end_pos, rot->sync_finish_stamp.tv_sec, do_start);

	if (!do_start)
		memset(&rot->sync_finish_stamp, 0, sizeof(rot->sync_finish_stamp));

	/* Now do it....
	 */
	{
		const char *argv[2] = { src, dst };

		status = __make_copy(global, dent,
				     do_start ? switch_path : "",
				     copy_path, dent->d_parent->d_path, argv, find_key(rot->msgs, "inf-sync"),
				     start_pos, end_pos,
				     rot->sync_finish_stamp.tv_sec != 0,
				     mars_fast_fullsync > 0,
				     true, false, &copy);
		if (copy) {
			copy->kill_ptr = (void **)&rot->sync_brick;
			copy->copy_limiter = &rot->sync_limiter;
		}
		rot->sync_brick = copy;
	}

	/* Update syncstatus symlink
	 */
	if (status >= 0 && copy &&
	    ((copy->power.button && copy->power.on_led) ||
	     !copy->copy_start ||
	     (copy->copy_last == copy->copy_end && copy->copy_end > 0))) {
		status = _update_syncstatus(rot, copy, peer);
	}

done:
	XIO_DBG("status = %d\n", status);
	brick_string_free(tmp);
	brick_string_free(src);
	brick_string_free(dst);
	brick_string_free(copy_path);
	brick_string_free(switch_path);
	return status;
}

static
bool remember_peer(struct mars_rotate *rot, struct mars_peerinfo *peer)
{
	if (!peer || !rot || rot->preferred_peer)
		return false;

	if ((long long)peer->last_remote_jiffies + mars_scan_interval * HZ * 2 < (long long)jiffies)
		return false;

	rot->preferred_peer = brick_strdup(peer->peer);
	return true;
}

static
int make_connect(void *buf, struct mars_dent *dent)
{
	struct mars_rotate *rot;
	struct mars_peerinfo *peer;
	char *names;
	char *this_name;
	char *tmp;

	if (unlikely(!dent->d_parent || !dent->link_val))
		goto done;
	rot = dent->d_parent->d_private;
	if (unlikely(!rot))
		goto done;

	names = brick_strdup(dent->link_val);
	for (tmp = this_name = names; *tmp; tmp++) {
		if (*tmp == MARS_DELIM) {
			*tmp = '\0';
			peer = find_peer(this_name);
			if (remember_peer(rot, peer))
				goto found;
			this_name = tmp + 1;
		}
	}
	peer = find_peer(this_name);
	remember_peer(rot, peer);

found:
	brick_string_free(names);
done:
	return 0;
}

static int prepare_delete(void *buf, struct mars_dent *dent)
{
	struct kstat stat;
	struct kstat *to_delete = NULL;
	struct mars_global *global = buf;
	struct mars_dent *target;
	struct mars_dent *response;
	const char *marker_path = NULL;
	const char *response_path = NULL;
	struct xio_brick *brick;
	int max_serial = 0;
	int status;

	if (!global || !dent || !dent->link_val || !dent->d_path)
		goto err;

	/*  create a marker which prevents concurrent updates from remote hosts */
	marker_path = backskip_replace(dent->link_val, '/', true, "/.deleted-");
	if (mars_stat(marker_path, &stat, true) < 0 ||
	    timespec_compare(&dent->stat_val.mtime, &stat.mtime) > 0) {
		XIO_DBG("creating / updating marker '%s' mtime=%lu.%09lu\n",
			 marker_path, dent->stat_val.mtime.tv_sec, dent->stat_val.mtime.tv_nsec);
		mars_symlink("1", marker_path, &dent->stat_val.mtime, 0);
	}

	brick = mars_find_brick(global, NULL, dent->link_val);
	if (brick &&
	    unlikely((brick->nr_outputs > 0 && brick->outputs[0] && brick->outputs[0]->nr_connected) ||
		     (brick->type == (void *)&if_brick_type && !brick->power.off_led))) {
		XIO_WRN("target '%s' cannot be deleted, its brick '%s' in use\n", dent->link_val, brick->brick_name);
		goto done;
	}

	status = 0;
	target = mars_find_dent(global, dent->link_val);
	if (target) {
		if (timespec_compare(&target->stat_val.mtime, &dent->stat_val.mtime) > 0) {
			XIO_WRN("target '%s' has newer timestamp than deletion link, ignoring\n", dent->link_val);
			status = -EAGAIN;
			goto ok;
		}
		if (target->d_child_count) {
			XIO_WRN("target '%s' has %d children, cannot kill\n", dent->link_val, target->d_child_count);
			goto done;
		}
		target->d_killme = true;
		XIO_DBG("target '%s' marked for removal\n", dent->link_val);
		to_delete = &target->stat_val;
	} else if (mars_stat(dent->link_val, &stat, true) >= 0) {
		if (timespec_compare(&stat.mtime, &dent->stat_val.mtime) > 0) {
			XIO_WRN("target '%s' has newer timestamp than deletion link, ignoring\n", dent->link_val);
			status = -EAGAIN;
			goto ok;
		}
		to_delete = &stat;
	} else {
		status = -EAGAIN;
		XIO_DBG("target '%s' does no longer exist\n", dent->link_val);
	}
	if (to_delete) {
		status = mars_unlink(dent->link_val);
		XIO_DBG("unlink '%s', status = %d\n", dent->link_val, status);
	}

ok:
	if (status < 0) {
		XIO_DBG("deletion '%s' to target '%s' is accomplished\n",
			 dent->d_path, dent->link_val);
		if (dent->d_serial <= global->deleted_border) {
			XIO_DBG("removing deletion symlink '%s'\n", dent->d_path);
			dent->d_killme = true;
			mars_unlink(dent->d_path);
			XIO_DBG("removing marker '%s'\n", marker_path);
			mars_unlink(marker_path);
		}
	}

done:
	/*  tell the world that we have seen this deletion... (even when not yet accomplished) */
	response_path = path_make("/mars/todo-global/deleted-%s", my_id());
	response = mars_find_dent(global, response_path);
	if (response && response->link_val) {
		int status = kstrtoint(response->link_val, 10, &max_serial);

		(void)status; /* leave untouched in case of errors */
	}
	if (dent->d_serial > max_serial) {
		char response_val[16];

		max_serial = dent->d_serial;
		global->deleted_my_border = max_serial;
		snprintf(response_val, sizeof(response_val), "%09d", max_serial);
		mars_symlink(response_val, response_path, NULL, 0);
	}

err:
	brick_string_free(marker_path);
	brick_string_free(response_path);
	return 0;
}

static int check_deleted(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	int serial = 0;
	int status;

	if (!global || !dent || !dent->link_val)
		goto done;

	status = kstrtoint(dent->link_val, 10, &serial);
	if (unlikely(status || serial <= 0)) {
		XIO_WRN("cannot parse symlink '%s' -> '%s'\n", dent->d_path, dent->link_val);
		goto done;
	}

	if (!strcmp(dent->d_rest, my_id()))
		global->deleted_my_border = serial;

	/* Compute the minimum of the deletion progress among
	 * the resource members.
	 */
	if (serial < global->deleted_min || !global->deleted_min)
		global->deleted_min = serial;

done:
	return 0;
}

static
int make_res(void *buf, struct mars_dent *dent)
{
	struct mars_rotate *rot = dent->d_private;

	if (!rot) {
		XIO_DBG("nothing to do\n");
		goto done;
	}

	rot->has_symlinks = false;

done:
	return 0;
}

static
int kill_res(void *buf, struct mars_dent *dent)
{
	struct mars_rotate *rot = dent->d_private;

	if (unlikely(!rot || !rot->parent_path)) {
		XIO_DBG("nothing to do\n");
		goto done;
	}

	show_vals(rot->msgs, rot->parent_path, "");

	if (unlikely(!rot->global)) {
		XIO_DBG("nothing to do\n");
		goto done;
	}
	if (rot->has_symlinks) {
		XIO_DBG("symlinks were present, nothing to kill.\n");
		goto done;
	}

	/*  this code is only executed in case of forced deletion of symlinks */
	if (rot->if_brick || rot->sync_brick || rot->fetch_brick || rot->trans_brick) {
		rot->res_shutdown = true;
		XIO_WRN("resource '%s' has no symlinks, shutting down.\n", rot->parent_path);
	}
	if (rot->if_brick) {
		if (atomic_read(&rot->if_brick->open_count) > 0) {
			XIO_ERR("cannot destroy resource '%s': device is is use!\n", rot->parent_path);
			goto done;
		}
		rot->if_brick->killme = true;
		if (!rot->if_brick->power.off_led) {
			int status = mars_power_button((void *)rot->if_brick, false, false);

			XIO_INF("switching off resource '%s', device status = %d\n", rot->parent_path, status);
		} else {
			xio_kill_brick((void *)rot->if_brick);
			rot->if_brick = NULL;
		}
	}
	if (rot->sync_brick) {
		rot->sync_brick->killme = true;
		if (!rot->sync_brick->power.off_led) {
			int status = mars_power_button((void *)rot->sync_brick, false, false);

			XIO_INF("switching off resource '%s', sync status = %d\n", rot->parent_path, status);
		}
	}
	if (rot->fetch_brick) {
		rot->fetch_brick->killme = true;
		if (!rot->fetch_brick->power.off_led) {
			int status = mars_power_button((void *)rot->fetch_brick, false, false);

			XIO_INF("switching off resource '%s', fetch status = %d\n", rot->parent_path, status);
		}
	}
	if (rot->trans_brick) {
		struct trans_logger_output *output = rot->trans_brick->outputs[0];

		if (!output || output->nr_connected) {
			XIO_ERR("cannot destroy resource '%s': trans_logger is is use!\n", rot->parent_path);
			goto done;
		}
		rot->trans_brick->killme = true;
		if (!rot->trans_brick->power.off_led) {
			int status = mars_power_button((void *)rot->trans_brick, false, false);

			XIO_INF("switching off resource '%s', logger status = %d\n", rot->parent_path, status);
		}
	}
	if (!rot->if_brick && !rot->sync_brick && !rot->fetch_brick && !rot->trans_brick)
		rot->res_shutdown = false;

done:
	return 0;
}

static
int make_defaults(void *buf, struct mars_dent *dent)
{
	if (!dent->link_val)
		goto done;

	XIO_DBG("name = '%s' value = '%s'\n", dent->d_name, dent->link_val);

	if (!strcmp(dent->d_name, "sync-limit")) {
		int status = kstrtoint(dent->link_val, 10, &global_sync_limit);

		(void)status; /* leave untouched in case of errors */
	} else if (!strcmp(dent->d_name, "sync-pref-list")) {
		const char *start;
		struct list_head *tmp;
		int len;
		int want_count = 0;
		int get_count = 0;

		for (tmp = rot_anchor.next; tmp != &rot_anchor; tmp = tmp->next) {
			struct mars_rotate *rot = container_of(tmp, struct mars_rotate, rot_head);

			if (rot->wants_sync)
				want_count++;
			else
				rot->gets_sync = false;
			if (rot->sync_brick && rot->sync_brick->power.on_led)
				get_count++;
		}
		global_sync_want = want_count;
		global_sync_nr = get_count;

		/*  prefer mentioned resources in the right order */
		for (start = dent->link_val; *start && get_count < global_sync_limit; start += len) {
			len = 1;
			while (start[len] && start[len] != ',')
				len++;
			for (tmp = rot_anchor.next; tmp != &rot_anchor; tmp = tmp->next) {
				struct mars_rotate *rot = container_of(tmp, struct mars_rotate, rot_head);

				if (rot->wants_sync && rot->parent_rest && !strncmp(start, rot->parent_rest, len)) {
					rot->gets_sync = true;
					get_count++;
					XIO_DBG("new get_count = %d res = '%s' wants_sync = %d gets_sync = %d\n",
						 get_count, rot->parent_rest, rot->wants_sync, rot->gets_sync);
					break;
				}
			}
			if (start[len])
				len++;
		}
		/*  fill up with unmentioned resources */
		for (tmp = rot_anchor.next; tmp != &rot_anchor && get_count < global_sync_limit; tmp = tmp->next) {
			struct mars_rotate *rot = container_of(tmp, struct mars_rotate, rot_head);

			if (rot->wants_sync && !rot->gets_sync) {
				rot->gets_sync = true;
				get_count++;
			}
			XIO_DBG("new get_count = %d res = '%s' wants_sync = %d gets_sync = %d\n",
				 get_count, rot->parent_rest, rot->wants_sync, rot->gets_sync);
		}
		XIO_DBG("final want_count = %d get_count = %d\n", want_count, get_count);
	} else {
		XIO_DBG("unimplemented default '%s'\n", dent->d_name);
	}
done:
	return 0;
}

/*********************************************************************/

/* Please keep the order the same as in the enum.
 */
static const struct light_class light_classes[] = {
	/* Placeholder for root node /mars/
	 */
	[CL_ROOT] = {
	},

	/* UUID, indentifying the whole cluster.
	 */
	[CL_UUID] = {
		.cl_name = "uuid",
		.cl_len = 4,
		.cl_type = 'l',
		.cl_father = CL_ROOT,
	},

	/* Subdirectory for global userspace items...
	 */
	[CL_GLOBAL_USERSPACE] = {
		.cl_name = "userspace",
		.cl_len = 9,
		.cl_type = 'd',
		.cl_hostcontext = false,
		.cl_father = CL_ROOT,
	},
	[CL_GLOBAL_USERSPACE_ITEMS] = {
		.cl_name = "",
		.cl_len = 0, /*  catch any */
		.cl_type = 'l',
		.cl_father = CL_GLOBAL_USERSPACE,
	},

	/* Subdirectory for defaults...
	 */
	[CL_DEFAULTS0] = {
		.cl_name = "defaults",
		.cl_len = 8,
		.cl_type = 'd',
		.cl_hostcontext = false,
		.cl_father = CL_ROOT,
	},
	[CL_DEFAULTS] = {
		.cl_name = "defaults-",
		.cl_len = 9,
		.cl_type = 'd',
		.cl_hostcontext = true,
		.cl_father = CL_ROOT,
	},
	/* ... and its contents
	 */
	[CL_DEFAULTS_ITEMS0] = {
		.cl_name = "",
		.cl_len = 0, /*  catch any */
		.cl_type = 'l',
		.cl_father = CL_DEFAULTS0,
	},
	[CL_DEFAULTS_ITEMS] = {
		.cl_name = "",
		.cl_len = 0, /*  catch any */
		.cl_type = 'l',
		.cl_father = CL_DEFAULTS,
		.cl_forward = make_defaults,
	},

	/* Subdirectory for global controlling items...
	 */
	[CL_GLOBAL_TODO] = {
		.cl_name = "todo-global",
		.cl_len = 11,
		.cl_type = 'd',
		.cl_hostcontext = false,
		.cl_father = CL_ROOT,
	},
	/* ... and its contents
	 */
	[CL_GLOBAL_TODO_DELETE] = {
		.cl_name = "delete-",
		.cl_len = 7,
		.cl_type = 'l',
		.cl_serial = true,
		.cl_hostcontext = false, /*  ignore context, although present */
		.cl_father = CL_GLOBAL_TODO,
		.cl_prepare = prepare_delete,
	},
	[CL_GLOBAL_TODO_DELETED] = {
		.cl_name = "deleted-",
		.cl_len = 8,
		.cl_type = 'l',
		.cl_father = CL_GLOBAL_TODO,
		.cl_prepare = check_deleted,
	},

	/* Directory containing the addresses of all peers
	 */
	[CL_IPS] = {
		.cl_name = "ips",
		.cl_len = 3,
		.cl_type = 'd',
		.cl_father = CL_ROOT,
	},
	/* Anyone participating in a MARS cluster must
	 * be named here (symlink pointing to the IP address).
	 * We have no DNS in kernel space.
	 */
	[CL_PEERS] = {
		.cl_name = "ip-",
		.cl_len = 3,
		.cl_type = 'l',
		.cl_father = CL_IPS,
		.cl_forward = make_scan,
		.cl_backward = kill_scan,
	},
	/* Subdirectory for actual state
	 */
	[CL_GBL_ACTUAL] = {
		.cl_name = "actual-",
		.cl_len = 7,
		.cl_type = 'd',
		.cl_hostcontext = false,
		.cl_father = CL_ROOT,
	},
	/* ... and its contents
	 */
	[CL_GBL_ACTUAL_ITEMS] = {
		.cl_name = "",
		.cl_len = 0, /*  catch any */
		.cl_type = 'l',
		.cl_father = CL_GBL_ACTUAL,
	},
	/* Indicate aliveness of all cluster paritcipants
	 * by the timestamp of this link.
	 */
	[CL_ALIVE] = {
		.cl_name = "alive-",
		.cl_len = 6,
		.cl_type = 'l',
		.cl_father = CL_ROOT,
	},
	[CL_TIME] = {
		.cl_name = "time-",
		.cl_len = 5,
		.cl_type = 'l',
		.cl_father = CL_ROOT,
	},
	/* Show version indication for symlink tree.
	 */
	[CL_TREE] = {
		.cl_name = "tree-",
		.cl_len = 5,
		.cl_type = 'l',
		.cl_father = CL_ROOT,
	},
	/* Indicate whether filesystem is full
	 */
	[CL_EMERGENCY] = {
		.cl_name = "emergency-",
		.cl_len = 10,
		.cl_type = 'l',
		.cl_father = CL_ROOT,
	},
	/* dto as percentage
	 */
	[CL_REST_SPACE] = {
		.cl_name = "rest-space-",
		.cl_len = 11,
		.cl_type = 'l',
		.cl_father = CL_ROOT,
	},

	/* Directory containing all items of a resource
	 */
	[CL_RESOURCE] = {
		.cl_name = "resource-",
		.cl_len = 9,
		.cl_type = 'd',
		.cl_use_channel = true,
		.cl_father = CL_ROOT,
		.cl_forward = make_res,
		.cl_backward = kill_res,
	},

	/* Subdirectory for resource-specific userspace items...
	 */
	[CL_RESOURCE_USERSPACE] = {
		.cl_name = "userspace",
		.cl_len = 9,
		.cl_type = 'd',
		.cl_hostcontext = false,
		.cl_father = CL_RESOURCE,
	},
	[CL_RESOURCE_USERSPACE_ITEMS] = {
		.cl_name = "",
		.cl_len = 0, /*  catch any */
		.cl_type = 'l',
		.cl_father = CL_RESOURCE_USERSPACE,
	},

	/* Subdirectory for defaults...
	 */
	[CL_RES_DEFAULTS0] = {
		.cl_name = "defaults",
		.cl_len = 8,
		.cl_type = 'd',
		.cl_hostcontext = false,
		.cl_father = CL_RESOURCE,
	},
	[CL_RES_DEFAULTS] = {
		.cl_name = "defaults-",
		.cl_len = 9,
		.cl_type = 'd',
		.cl_hostcontext = false,
		.cl_father = CL_RESOURCE,
	},
	/* ... and its contents
	 */
	[CL_RES_DEFAULTS_ITEMS0] = {
		.cl_name = "",
		.cl_len = 0, /*  catch any */
		.cl_type = 'l',
		.cl_father = CL_RES_DEFAULTS0,
	},
	[CL_RES_DEFAULTS_ITEMS] = {
		.cl_name = "",
		.cl_len = 0, /*  catch any */
		.cl_type = 'l',
		.cl_father = CL_RES_DEFAULTS,
	},

	/* Subdirectory for controlling items...
	 */
	[CL_TODO] = {
		.cl_name = "todo-",
		.cl_len = 5,
		.cl_type = 'd',
		.cl_hostcontext = false,
		.cl_father = CL_RESOURCE,
	},
	/* ... and its contents
	 */
	[CL_TODO_ITEMS] = {
		.cl_name = "",
		.cl_len = 0, /*  catch any */
		.cl_type = 'l',
		.cl_father = CL_TODO,
	},

	/* Subdirectory for actual state
	 */
	[CL_ACTUAL] = {
		.cl_name = "actual-",
		.cl_len = 7,
		.cl_type = 'd',
		.cl_hostcontext = false,
		.cl_father = CL_RESOURCE,
	},
	/* ... and its contents
	 */
	[CL_ACTUAL_ITEMS] = {
		.cl_name = "",
		.cl_len = 0, /*  catch any */
		.cl_type = 'l',
		.cl_father = CL_ACTUAL,
	},

	/* File or symlink to the real device / real (sparse) file
	 * when hostcontext is missing, the corresponding peer will
	 * not participate in that resource.
	 */
	[CL_DATA] = {
		.cl_name = "data-",
		.cl_len = 5,
		.cl_type = 'F',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_bio,
		.cl_backward = kill_any,
	},
	/* Symlink indicating the (common) size of the resource
	 */
	[CL_SIZE] = {
		.cl_name = "size",
		.cl_len = 4,
		.cl_type = 'l',
		.cl_hostcontext = false,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_log_init,
		.cl_backward = kill_any,
	},
	/* Dito for each individual size
	 */
	[CL_ACTSIZE] = {
		.cl_name = "actsize-",
		.cl_len = 8,
		.cl_type = 'l',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
	},
	/* Symlink pointing to the name of the primary node
	 */
	[CL_PRIMARY] = {
		.cl_name = "primary",
		.cl_len = 7,
		.cl_type = 'l',
		.cl_hostcontext = false,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_primary,
		.cl_backward = NULL,
	},
	/* Symlink for connection preferences
	 */
	[CL_CONNECT] = {
		.cl_name = "connect-",
		.cl_len = 8,
		.cl_type = 'l',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_connect,
	},
	/* informational symlink indicating the current
	 * status / start / pos / end of logfile transfers.
	 */
	[CL_TRANSFER] = {
		.cl_name = "transferstatus-",
		.cl_len = 15,
		.cl_type = 'l',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
	},
	/* symlink indicating the current status / end
	 * of initial data sync.
	 */
	[CL_SYNC] = {
		.cl_name = "syncstatus-",
		.cl_len = 11,
		.cl_type = 'l',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_sync,
		.cl_backward = kill_any,
	},
	/* informational symlink for verify status
	 * of initial data sync.
	 */
	[CL_VERIF] = {
		.cl_name = "verifystatus-",
		.cl_len = 13,
		.cl_type = 'l',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
	},
	/* informational symlink: after sync has finished,
	 * keep a copy of the replay symlink from the primary.
	 * when comparing the own replay symlink against this,
	 * we can determine whether we are consistent.
	 */
	[CL_SYNCPOS] = {
		.cl_name = "syncpos-",
		.cl_len = 8,
		.cl_type = 'l',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
	},
	/* Passive symlink indicating the split-brain crypto hash
	 */
	[CL_VERSION] = {
		.cl_name = "version-",
		.cl_len = 8,
		.cl_type = 'l',
		.cl_serial = true,
		.cl_hostcontext = false,
		.cl_father = CL_RESOURCE,
	},
	/* Logfiles for transaction logger
	 */
	[CL_LOG] = {
		.cl_name = "log-",
		.cl_len = 4,
		.cl_type = 'F',
		.cl_serial = true,
		.cl_hostcontext = false,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_log_step,
		.cl_backward = kill_any,
	},
	/* Symlink indicating the last state of
	 * transaction log replay.
	 */
	[CL_REPLAYSTATUS] = {
		.cl_name = "replay-",
		.cl_len = 7,
		.cl_type = 'l',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_replay,
		.cl_backward = kill_any,
	},

	/* Name of the device appearing at the primary
	 */
	[CL_DEVICE] = {
		.cl_name = "device-",
		.cl_len = 7,
		.cl_type = 'l',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_dev,
		.cl_backward = kill_dev,
	},

	/* Quirk: when dead resources are recreated during a network partition,
	 * this is used to void version number clashes in the
	 * partitioned cluster.
	 */
	[CL_MAXNR] = {
		.cl_name = "maxnr",
		.cl_len = 5,
		.cl_type = 'l',
		.cl_father = CL_RESOURCE,
	},
	{}
};

/* Helper routine to pre-determine the relevance of a name from the filesystem.
 */
int light_checker(struct mars_dent *parent,
	const char *_name,
	int namlen,
	unsigned int d_type,
	int *prefix,
	int *serial,
	bool *use_channel)
{
	int class;
	int status = -2;

#ifdef XIO_DEBUGGING
	const char *name = brick_strndup(_name, namlen);

#else
	const char *name = _name;

#endif

	/* XIO_DBG("trying '%s' '%s'\n", path, name); */
	for (class = CL_ROOT + 1; ; class++) {
		const struct light_class *test = &light_classes[class];
		int len = test->cl_len;

		if (!test->cl_name) { /*  end of table */
			break;
		}

		/* XIO_DBG("   testing class '%s'\n", test->cl_name); */

#ifdef XIO_DEBUGGING
		if (len != strlen(test->cl_name)) {
			XIO_ERR("internal table '%s' mismatch: %d != %d\n",
				test->cl_name,
				len,
				(int)strlen(test->cl_name));
			len = strlen(test->cl_name);
		}
#endif

		if (test->cl_father &&
		   (!parent || parent->d_class != test->cl_father)) {
			continue;
		}

		if (len > 0 &&
		   (namlen < len || memcmp(name, test->cl_name, len))) {
			continue;
		}

		/* XIO_DBG("path '%s/%s' matches class %d '%s'\n", path, name, class, test->cl_name); */

		/*  check special contexts */
		if (test->cl_serial) {
			int plus = 0;
			int count;

			count = sscanf(name+len, "%d%n", serial, &plus);
			if (count < 1) {
				/* XIO_DBG("'%s' serial number mismatch at '%s'\n", name, name+len); */
				continue;
			}
			/* XIO_DBG("'%s' serial number = %d\n", name, *serial); */
			len += plus;
			if (name[len] == '-')
				len++;
		}
		if (prefix)
			*prefix = len;
		if (test->cl_hostcontext) {
			if (memcmp(name+len, my_id(), namlen-len)) {
				/* XIO_DBG("context mismatch '%s' at '%s'\n", name, name+len); */
				continue;
			}
		}

		/*  all ok */
		status = class;
		*use_channel = test->cl_use_channel;
	}

#ifdef XIO_DEBUGGING
	brick_string_free(name);
#endif
	return status;
}

/* Do some syntactic checks, then delegate work to the real worker functions
 * from the light_classes[] table.
 */
static int light_worker(struct mars_global *global, struct mars_dent *dent, bool prepare, bool direction)
{
	light_worker_fn worker;
	int class = dent->d_class;

	if (class < 0 || class >= sizeof(light_classes)/sizeof(struct light_class)) {
		XIO_ERR("bad internal class %d of '%s'\n", class, dent->d_path);
		return -EINVAL;
	}
	switch (light_classes[class].cl_type) {
	case 'd':
		if (!S_ISDIR(dent->stat_val.mode)) {
			XIO_ERR("'%s' should be a directory, but is something else\n", dent->d_path);
			return -EINVAL;
		}
		break;
	case 'f':
		if (!S_ISREG(dent->stat_val.mode)) {
			XIO_ERR("'%s' should be a regular file, but is something else\n", dent->d_path);
			return -EINVAL;
		}
		break;
	case 'F':
		if (!S_ISREG(dent->stat_val.mode) && !S_ISLNK(dent->stat_val.mode)) {
			XIO_ERR("'%s' should be a regular file or a symlink, but is something else\n", dent->d_path);
			return -EINVAL;
		}
		break;
	case 'l':
		if (!S_ISLNK(dent->stat_val.mode)) {
			XIO_ERR("'%s' should be a symlink, but is something else\n", dent->d_path);
			return -EINVAL;
		}
		break;
	}
	if (likely(class > CL_ROOT)) {
		int father = light_classes[class].cl_father;

		if (father == CL_ROOT) {
			if (unlikely(dent->d_parent)) {
				XIO_ERR("'%s' class %d is not at the root of the hierarchy\n", dent->d_path, class);
				return -EINVAL;
			}
		} else if (unlikely(!dent->d_parent || dent->d_parent->d_class != father)) {
			XIO_ERR("last component '%s' from '%s' is at the wrong position in the hierarchy (class = %d, parent_class = %d, parent = '%s')\n",
				dent->d_name,
				dent->d_path,
				father,
				dent->d_parent ? dent->d_parent->d_class : -9999,
				dent->d_parent ? dent->d_parent->d_path : "");
			return -EINVAL;
		}
	}
	if (prepare)
		worker = light_classes[class].cl_prepare;
	else if (direction)
		worker = light_classes[class].cl_backward;
	else
		worker = light_classes[class].cl_forward;
	if (worker) {
		int status;

		if (!direction)
			XIO_DBG("--- start working %s on '%s' rest='%s'\n",
				direction ? "backward" : "forward",
				dent->d_path,
				dent->d_rest);
		status = worker(global, (void *)dent);
		XIO_DBG("--- done, worked %s on '%s', status = %d\n",
			direction ? "backward" : "forward",
			dent->d_path,
			status);
		return status;
	}
	return 0;
}

static struct mars_global _global = {
	.dent_anchor = LIST_HEAD_INIT(_global.dent_anchor),
	.brick_anchor = LIST_HEAD_INIT(_global.brick_anchor),
	.global_power = {
		.button = true,
	},
	.main_event = __WAIT_QUEUE_HEAD_INITIALIZER(_global.main_event),
};

static int light_thread(void *data)
{
	long long last_rollover = jiffies;
	char *id = my_id();
	int status = 0;

	init_rwsem(&_global.dent_mutex);
	init_rwsem(&_global.brick_mutex);

	mars_global = &_global;

	if (!id || strlen(id) < 2) {
		XIO_ERR("invalid hostname\n");
		status = -EFAULT;
		goto done;
	}

	XIO_INF("-------- starting as host '%s' ----------\n", id);

	while (_global.global_power.button || !list_empty(&_global.brick_anchor)) {
		int status;

		XIO_DBG("-------- NEW ROUND ---------\n");

		if (mars_mem_percent < 0)
			mars_mem_percent = 0;
		if (mars_mem_percent > 70)
			mars_mem_percent = 70;
		brick_global_memlimit = (long long)brick_global_memavail * mars_mem_percent / 100;

		brick_msleep(100);

		if (brick_thread_should_stop()) {
			_global.global_power.button = false;
			xio_net_is_alive = false;
		}

		_make_alive();

		compute_emergency_mode();

		XIO_DBG("-------- start worker ---------\n");
		_global.deleted_min = 0;
		status = mars_dent_work(&_global,
			"/mars",
			sizeof(struct mars_dent),
			light_checker,
			light_worker,
			&_global,
			3);
		_global.deleted_border = _global.deleted_min;
		XIO_DBG("-------- worker deleted_min = %d status = %d\n", _global.deleted_min, status);

		if (!_global.global_power.button) {
			status = xio_kill_brick_when_possible(&_global,
				&_global.brick_anchor,
				false,
				(void *)&copy_brick_type,
				true);
			XIO_DBG("kill copy bricks (when possible) = %d\n", status);
		}

		status = xio_kill_brick_when_possible(&_global, &_global.brick_anchor, false, NULL, false);
		XIO_DBG("kill main bricks (when possible) = %d\n", status);

		status = xio_kill_brick_when_possible(&_global,
			&_global.brick_anchor,
			false,
			(void *)&client_brick_type,
			true);
		XIO_DBG("kill client bricks (when possible) = %d\n", status);
		status = xio_kill_brick_when_possible(&_global,
			&_global.brick_anchor,
			false,
			(void *)&sio_brick_type,
			true);
		XIO_DBG("kill sio    bricks (when possible) = %d\n", status);
		status = xio_kill_brick_when_possible(&_global,
			&_global.brick_anchor,
			false,
			(void *)&bio_brick_type,
			true);
		XIO_DBG("kill bio    bricks (when possible) = %d\n", status);

		if ((long long)jiffies + mars_rollover_interval * HZ >= last_rollover) {
			last_rollover = jiffies;
			rollover_all();
		}

		_show_status_all(&_global);
		show_vals(gbl_pairs, "/mars", "");
		show_statistics(&_global, "main");

		XIO_DBG("ban_count = %d ban_renew_count = %d\n",
			xio_global_ban.ban_count,
			xio_global_ban.ban_renew_count);

		brick_msleep(500);

		wait_event_interruptible_timeout(_global.main_event, _global.main_trigger, mars_scan_interval * HZ);

		_global.main_trigger = false;
	}

done:
	XIO_INF("-------- cleaning up ----------\n");
	remote_trigger();
	brick_msleep(1000);

	xio_free_dent_all(&_global, &_global.dent_anchor);
	xio_kill_brick_all(&_global, &_global.brick_anchor, false);

	_show_status_all(&_global);
	show_vals(gbl_pairs, "/mars", "");
	show_statistics(&_global, "main");

	mars_global = NULL;

	XIO_INF("-------- done status = %d ----------\n", status);
	/* cleanup_mm(); */
	return status;
}

static
char *_xio_info(void)
{
	int max = PAGE_SIZE - 64;
	char *txt;
	struct list_head *tmp;
	int dent_count = 0;
	int brick_count = 0;
	int pos = 0;

	if (unlikely(!mars_global))
		return NULL;

	txt = brick_string_alloc(max);

	txt[--max] = '\0'; /*  safeguard */

	down_read(&mars_global->brick_mutex);
	for (tmp = mars_global->brick_anchor.next; tmp != &mars_global->brick_anchor; tmp = tmp->next) {
		struct xio_brick *test;

		brick_count++;
		test = container_of(tmp, struct xio_brick, global_brick_link);
		pos += scnprintf(
			txt + pos, max - pos,
			"brick button=%d off=%d on=%d path='%s'\n",
			test->power.button,
			test->power.off_led,
			test->power.on_led,
			test->brick_path
			);
	}
	up_read(&mars_global->brick_mutex);

	pos += scnprintf(
		txt + pos, max - pos,
		"SUMMARY: brick_count=%d dent_count=%d\n",
		brick_count,
		dent_count
		);

	return txt;
}

#define INIT_MAX			32
static char *exit_names[INIT_MAX];
static void (*exit_fn[INIT_MAX])(void);
static int exit_fn_nr;

#define DO_INIT(name)							\
	do {								\
		XIO_DBG("=== starting module " #name "...\n");		\
		status = init_##name();					\
		if (status < 0)						\
			goto done;					\
		exit_names[exit_fn_nr] = #name;				\
		exit_fn[exit_fn_nr++] = exit_##name;			\
	} while (0)

void (*_remote_trigger)(void);

static void exit_light(void)
{
	XIO_DBG("====================== stopping everything...\n");
	/*  TODO: make this thread-safe. */
	if (main_thread) {
		XIO_DBG("=== stopping light thread...\n");
		local_trigger();
		XIO_INF("stopping main thread...\n");
		brick_thread_stop(main_thread);
	}

	xio_info = NULL;
	_remote_trigger = NULL;

	while (exit_fn_nr > 0) {
		XIO_DBG("=== stopping module %s ...\n", exit_names[exit_fn_nr - 1]);
		exit_fn[--exit_fn_nr]();
	}
	XIO_DBG("====================== stopped everything.\n");
	exit_say();
	printk(KERN_INFO "stopped MARS\n");
	/* Workaround for nasty race: some kernel threads have not yet
	 * really finished even _after_ kthread_stop() and may execute
	 * some code which will disappear right after return from this
	 * function.
	 * A correct solution would probably need the help of the kernel
	 * scheduler.
	 */
	brick_msleep(1000);
}

static int __init init_light(void)
{
	struct kstat dummy;
	int status = mars_stat("/mars/uuid", &dummy, true);

	if (unlikely(status < 0)) {
		printk(KERN_ERR "cannot load MARS: cluster UUID is missing. Mount /mars/, and/or use {create,join}-cluster first.\n");
		return -ENOENT;
	}

	printk(KERN_INFO "loading MARS, tree_version=%s\n", SYMLINK_TREE_VERSION);

	init_say(); /*	this must come first */

	/* be careful: order is important!
	 */
	DO_INIT(brick_mem);
	DO_INIT(brick);
	DO_INIT(xio);
	DO_INIT(xio_mapfree);
	DO_INIT(xio_net);
	DO_INIT(xio_client);
	DO_INIT(xio_sio);
	DO_INIT(xio_bio);
	DO_INIT(xio_copy);
	DO_INIT(log_format);
	DO_INIT(xio_trans_logger);
	DO_INIT(xio_if);

	DO_INIT(sy);
	DO_INIT(sy_net);
	DO_INIT(xio_proc);

#ifdef CONFIG_MARS_MEM_PREALLOC
	brick_pre_reserve[5] = 64;
	brick_mem_reserve();
#endif

	DO_INIT(xio_server);

	status = compute_emergency_mode();
	if (unlikely(status < 0)) {
		XIO_ERR("Sorry, your /mars/ filesystem is too small!\n");
		goto done;
	}

	main_thread = brick_thread_create(light_thread, NULL, "mars_light");
	if (unlikely(!main_thread)) {
		status = -ENOENT;
		goto done;
	}

done:
	if (status < 0) {
		XIO_ERR("module init failed with status = %d, exiting.\n", status);
		exit_light();
	}
	_remote_trigger = __remote_trigger;
	xio_info = _xio_info;
	return status;
}

/*  force module loading */
const void *dummy1 = &client_brick_type;
const void *dummy2 = &server_brick_type;

MODULE_DESCRIPTION("MARS Light");
MODULE_AUTHOR("Thomas Schoebel-Theuer <tst@{schoebel-theuer,1und1}.de>");
MODULE_VERSION(SYMLINK_TREE_VERSION);
MODULE_LICENSE("GPL");

#ifndef CONFIG_MARS_DEBUG
MODULE_INFO(debug, "production");
#else
MODULE_INFO(debug, "DEBUG");
#endif
#ifdef CONFIG_MARS_DEBUG_MEM
MODULE_INFO(io, "BAD_PERFORMANCE");
#endif
#ifdef CONFIG_MARS_DEBUG_ORDER0
MODULE_INFO(memory, "EVIL_PERFORMANCE");
#endif

module_init(init_light);
module_exit(exit_light);