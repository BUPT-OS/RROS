/* SPDX-License-Identifier: GPL-2.0 */
#include <stdbool.h>
#include <stdlib.h>

#include "../kselftest.h"

#define PAGE_SIZE 4096

#define MB(x) (x << 20)

#define USEC_PER_SEC	1000000L
#define NSEC_PER_SEC	1000000000L

#define TEST_UID	65534 /* usually nobody, any !root is fine */

/*
 * Checks if two given values differ by less than err% of their sum.
 */
static inline int values_close(long a, long b, int err)
{
	return abs(a - b) <= (a + b) / 100 * err;
}

extern int cg_find_unified_root(char *root, size_t len);
extern char *cg_name(const char *root, const char *name);
extern char *cg_name_indexed(const char *root, const char *name, int index);
extern char *cg_control(const char *cgroup, const char *control);
extern int cg_create(const char *cgroup);
extern int cg_destroy(const char *cgroup);
extern int cg_read(const char *cgroup, const char *control,
		   char *buf, size_t len);
extern int cg_read_strcmp(const char *cgroup, const char *control,
			  const char *expected);
extern int cg_read_strstr(const char *cgroup, const char *control,
			  const char *needle);
extern long cg_read_long(const char *cgroup, const char *control);
long cg_read_key_long(const char *cgroup, const char *control, const char *key);
extern long cg_read_lc(const char *cgroup, const char *control);
extern int cg_write(const char *cgroup, const char *control, char *buf);
int cg_write_numeric(const char *cgroup, const char *control, long value);
extern int cg_run(const char *cgroup,
		  int (*fn)(const char *cgroup, void *arg),
		  void *arg);
extern int cg_enter(const char *cgroup, int pid);
extern int cg_enter_current(const char *cgroup);
extern int cg_enter_current_thread(const char *cgroup);
extern int cg_run_nowait(const char *cgroup,
			 int (*fn)(const char *cgroup, void *arg),
			 void *arg);
extern int get_temp_fd(void);
extern int alloc_pagecache(int fd, size_t size);
extern int alloc_anon(const char *cgroup, void *arg);
extern int is_swap_enabled(void);
extern int set_oom_adj_score(int pid, int score);
extern int cg_wait_for_proc_count(const char *cgroup, int count);
extern int cg_killall(const char *cgroup);
int proc_mount_contains(const char *option);
extern ssize_t proc_read_text(int pid, bool thread, const char *item, char *buf, size_t size);
extern int proc_read_strstr(int pid, bool thread, const char *item, const char *needle);
extern pid_t clone_into_cgroup(int cgroup_fd);
extern int clone_reap(pid_t pid, int options);
extern int clone_into_cgroup_run_wait(const char *cgroup);
extern int dirfd_open_opath(const char *dir);
extern int cg_prepare_for_wait(const char *cgroup);
extern int memcg_prepare_for_wait(const char *cgroup);
extern int cg_wait_for(int fd);
