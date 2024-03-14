// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
/* Data structures shared between BPF and tools. */
#ifndef UTIL_BPF_SKEL_LOCK_DATA_H
#define UTIL_BPF_SKEL_LOCK_DATA_H

struct contention_key {
	u32 stack_id;
	u32 pid;
	u64 lock_addr;
};

#define TASK_COMM_LEN  16

struct contention_task_data {
	char comm[TASK_COMM_LEN];
};

/* default buffer size */
#define MAX_ENTRIES  16384

/*
 * Upper bits of the flags in the contention_data are used to identify
 * some well-known locks which do not have symbols (non-global locks).
 */
#define LCD_F_MMAP_LOCK		(1U << 31)
#define LCD_F_SIGHAND_LOCK	(1U << 30)

#define LCB_F_MAX_FLAGS		(1U << 7)

struct contention_data {
	u64 total_time;
	u64 min_time;
	u64 max_time;
	u32 count;
	u32 flags;
};

enum lock_aggr_mode {
	LOCK_AGGR_ADDR = 0,
	LOCK_AGGR_TASK,
	LOCK_AGGR_CALLER,
};

enum lock_class_sym {
	LOCK_CLASS_NONE,
	LOCK_CLASS_RQLOCK,
};

#endif /* UTIL_BPF_SKEL_LOCK_DATA_H */
