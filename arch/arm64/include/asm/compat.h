/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_COMPAT_H
#define __ASM_COMPAT_H

#define compat_mode_t compat_mode_t
typedef u16		compat_mode_t;

#define __compat_uid_t	__compat_uid_t
typedef u16		__compat_uid_t;
typedef u16		__compat_gid_t;

#define compat_ipc_pid_t compat_ipc_pid_t
typedef u16		compat_ipc_pid_t;

#define compat_statfs	compat_statfs

#include <asm-generic/compat.h>

#ifdef CONFIG_COMPAT

/*
 * Architecture specific compatibility types
 */
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>

#ifdef __AARCH64EB__
#define COMPAT_UTS_MACHINE	"armv8b\0\0"
#else
#define COMPAT_UTS_MACHINE	"armv8l\0\0"
#endif

typedef u16		__compat_uid16_t;
typedef u16		__compat_gid16_t;
typedef s32		compat_nlink_t;

struct compat_stat {
#ifdef __AARCH64EB__
	short		st_dev;
	short		__pad1;
#else
	compat_dev_t	st_dev;
#endif
	compat_ino_t	st_ino;
	compat_mode_t	st_mode;
	compat_ushort_t	st_nlink;
	__compat_uid16_t	st_uid;
	__compat_gid16_t	st_gid;
#ifdef __AARCH64EB__
	short		st_rdev;
	short		__pad2;
#else
	compat_dev_t	st_rdev;
#endif
	compat_off_t	st_size;
	compat_off_t	st_blksize;
	compat_off_t	st_blocks;
	old_time32_t	st_atime;
	compat_ulong_t	st_atime_nsec;
	old_time32_t	st_mtime;
	compat_ulong_t	st_mtime_nsec;
	old_time32_t	st_ctime;
	compat_ulong_t	st_ctime_nsec;
	compat_ulong_t	__unused4[2];
};

struct compat_statfs {
	int		f_type;
	int		f_bsize;
	int		f_blocks;
	int		f_bfree;
	int		f_bavail;
	int		f_files;
	int		f_ffree;
	compat_fsid_t	f_fsid;
	int		f_namelen;	/* SunOS ignores this field. */
	int		f_frsize;
	int		f_flags;
	int		f_spare[4];
};

#define compat_user_stack_pointer() (user_stack_pointer(task_pt_regs(current)))
#define COMPAT_MINSIGSTKSZ	2048

static inline int is_compat_task(void)
{
	return test_thread_flag(TIF_32BIT);
}

static inline int is_compat_thread(struct thread_info *thread)
{
	return test_ti_thread_flag(thread, TIF_32BIT);
}

long compat_arm_syscall(struct pt_regs *regs, int scno);

#else /* !CONFIG_COMPAT */

static inline int is_compat_thread(struct thread_info *thread)
{
	return 0;
}

#endif /* CONFIG_COMPAT */
#endif /* __ASM_COMPAT_H */
