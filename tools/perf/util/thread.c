// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <linux/kernel.h>
#include <linux/zalloc.h>
#include "dso.h"
#include "session.h"
#include "thread.h"
#include "thread-stack.h"
#include "debug.h"
#include "namespaces.h"
#include "comm.h"
#include "map.h"
#include "symbol.h"
#include "unwind.h"
#include "callchain.h"

#include <api/fs/fs.h>

int thread__init_maps(struct thread *thread, struct machine *machine)
{
	pid_t pid = thread__pid(thread);

	if (pid == thread__tid(thread) || pid == -1) {
		thread__set_maps(thread, maps__new(machine));
	} else {
		struct thread *leader = __machine__findnew_thread(machine, pid, pid);

		if (leader) {
			thread__set_maps(thread, maps__get(thread__maps(leader)));
			thread__put(leader);
		}
	}

	return thread__maps(thread) ? 0 : -1;
}

struct thread *thread__new(pid_t pid, pid_t tid)
{
	char *comm_str;
	struct comm *comm;
	RC_STRUCT(thread) *_thread = zalloc(sizeof(*_thread));
	struct thread *thread;

	if (ADD_RC_CHK(thread, _thread) != NULL) {
		thread__set_pid(thread, pid);
		thread__set_tid(thread, tid);
		thread__set_ppid(thread, -1);
		thread__set_cpu(thread, -1);
		thread__set_guest_cpu(thread, -1);
		thread__set_lbr_stitch_enable(thread, false);
		INIT_LIST_HEAD(thread__namespaces_list(thread));
		INIT_LIST_HEAD(thread__comm_list(thread));
		init_rwsem(thread__namespaces_lock(thread));
		init_rwsem(thread__comm_lock(thread));

		comm_str = malloc(32);
		if (!comm_str)
			goto err_thread;

		snprintf(comm_str, 32, ":%d", tid);
		comm = comm__new(comm_str, 0, false);
		free(comm_str);
		if (!comm)
			goto err_thread;

		list_add(&comm->list, thread__comm_list(thread));
		refcount_set(thread__refcnt(thread), 1);
		/* Thread holds first ref to nsdata. */
		RC_CHK_ACCESS(thread)->nsinfo = nsinfo__new(pid);
		srccode_state_init(thread__srccode_state(thread));
	}

	return thread;

err_thread:
	free(thread);
	return NULL;
}

static void (*thread__priv_destructor)(void *priv);

void thread__set_priv_destructor(void (*destructor)(void *priv))
{
	assert(thread__priv_destructor == NULL);

	thread__priv_destructor = destructor;
}

void thread__delete(struct thread *thread)
{
	struct namespaces *namespaces, *tmp_namespaces;
	struct comm *comm, *tmp_comm;

	thread_stack__free(thread);

	if (thread__maps(thread)) {
		maps__put(thread__maps(thread));
		thread__set_maps(thread, NULL);
	}
	down_write(thread__namespaces_lock(thread));
	list_for_each_entry_safe(namespaces, tmp_namespaces,
				 thread__namespaces_list(thread), list) {
		list_del_init(&namespaces->list);
		namespaces__free(namespaces);
	}
	up_write(thread__namespaces_lock(thread));

	down_write(thread__comm_lock(thread));
	list_for_each_entry_safe(comm, tmp_comm, thread__comm_list(thread), list) {
		list_del_init(&comm->list);
		comm__free(comm);
	}
	up_write(thread__comm_lock(thread));

	nsinfo__zput(RC_CHK_ACCESS(thread)->nsinfo);
	srccode_state_free(thread__srccode_state(thread));

	exit_rwsem(thread__namespaces_lock(thread));
	exit_rwsem(thread__comm_lock(thread));
	thread__free_stitch_list(thread);

	if (thread__priv_destructor)
		thread__priv_destructor(thread__priv(thread));

	RC_CHK_FREE(thread);
}

struct thread *thread__get(struct thread *thread)
{
	struct thread *result;

	if (RC_CHK_GET(result, thread))
		refcount_inc(thread__refcnt(thread));

	return result;
}

void thread__put(struct thread *thread)
{
	if (thread && refcount_dec_and_test(thread__refcnt(thread)))
		thread__delete(thread);
	else
		RC_CHK_PUT(thread);
}

static struct namespaces *__thread__namespaces(struct thread *thread)
{
	if (list_empty(thread__namespaces_list(thread)))
		return NULL;

	return list_first_entry(thread__namespaces_list(thread), struct namespaces, list);
}

struct namespaces *thread__namespaces(struct thread *thread)
{
	struct namespaces *ns;

	down_read(thread__namespaces_lock(thread));
	ns = __thread__namespaces(thread);
	up_read(thread__namespaces_lock(thread));

	return ns;
}

static int __thread__set_namespaces(struct thread *thread, u64 timestamp,
				    struct perf_record_namespaces *event)
{
	struct namespaces *new, *curr = __thread__namespaces(thread);

	new = namespaces__new(event);
	if (!new)
		return -ENOMEM;

	list_add(&new->list, thread__namespaces_list(thread));

	if (timestamp && curr) {
		/*
		 * setns syscall must have changed few or all the namespaces
		 * of this thread. Update end time for the namespaces
		 * previously used.
		 */
		curr = list_next_entry(new, list);
		curr->end_time = timestamp;
	}

	return 0;
}

int thread__set_namespaces(struct thread *thread, u64 timestamp,
			   struct perf_record_namespaces *event)
{
	int ret;

	down_write(thread__namespaces_lock(thread));
	ret = __thread__set_namespaces(thread, timestamp, event);
	up_write(thread__namespaces_lock(thread));
	return ret;
}

struct comm *thread__comm(struct thread *thread)
{
	if (list_empty(thread__comm_list(thread)))
		return NULL;

	return list_first_entry(thread__comm_list(thread), struct comm, list);
}

struct comm *thread__exec_comm(struct thread *thread)
{
	struct comm *comm, *last = NULL, *second_last = NULL;

	list_for_each_entry(comm, thread__comm_list(thread), list) {
		if (comm->exec)
			return comm;
		second_last = last;
		last = comm;
	}

	/*
	 * 'last' with no start time might be the parent's comm of a synthesized
	 * thread (created by processing a synthesized fork event). For a main
	 * thread, that is very probably wrong. Prefer a later comm to avoid
	 * that case.
	 */
	if (second_last && !last->start && thread__pid(thread) == thread__tid(thread))
		return second_last;

	return last;
}

static int ____thread__set_comm(struct thread *thread, const char *str,
				u64 timestamp, bool exec)
{
	struct comm *new, *curr = thread__comm(thread);

	/* Override the default :tid entry */
	if (!thread__comm_set(thread)) {
		int err = comm__override(curr, str, timestamp, exec);
		if (err)
			return err;
	} else {
		new = comm__new(str, timestamp, exec);
		if (!new)
			return -ENOMEM;
		list_add(&new->list, thread__comm_list(thread));

		if (exec)
			unwind__flush_access(thread__maps(thread));
	}

	thread__set_comm_set(thread, true);

	return 0;
}

int __thread__set_comm(struct thread *thread, const char *str, u64 timestamp,
		       bool exec)
{
	int ret;

	down_write(thread__comm_lock(thread));
	ret = ____thread__set_comm(thread, str, timestamp, exec);
	up_write(thread__comm_lock(thread));
	return ret;
}

int thread__set_comm_from_proc(struct thread *thread)
{
	char path[64];
	char *comm = NULL;
	size_t sz;
	int err = -1;

	if (!(snprintf(path, sizeof(path), "%d/task/%d/comm",
		       thread__pid(thread), thread__tid(thread)) >= (int)sizeof(path)) &&
	    procfs__read_str(path, &comm, &sz) == 0) {
		comm[sz - 1] = '\0';
		err = thread__set_comm(thread, comm, 0);
	}

	return err;
}

static const char *__thread__comm_str(struct thread *thread)
{
	const struct comm *comm = thread__comm(thread);

	if (!comm)
		return NULL;

	return comm__str(comm);
}

const char *thread__comm_str(struct thread *thread)
{
	const char *str;

	down_read(thread__comm_lock(thread));
	str = __thread__comm_str(thread);
	up_read(thread__comm_lock(thread));

	return str;
}

static int __thread__comm_len(struct thread *thread, const char *comm)
{
	if (!comm)
		return 0;
	thread__set_comm_len(thread, strlen(comm));

	return thread__var_comm_len(thread);
}

/* CHECKME: it should probably better return the max comm len from its comm list */
int thread__comm_len(struct thread *thread)
{
	int comm_len = thread__var_comm_len(thread);

	if (!comm_len) {
		const char *comm;

		down_read(thread__comm_lock(thread));
		comm = __thread__comm_str(thread);
		comm_len = __thread__comm_len(thread, comm);
		up_read(thread__comm_lock(thread));
	}

	return comm_len;
}

size_t thread__fprintf(struct thread *thread, FILE *fp)
{
	return fprintf(fp, "Thread %d %s\n", thread__tid(thread), thread__comm_str(thread)) +
	       maps__fprintf(thread__maps(thread), fp);
}

int thread__insert_map(struct thread *thread, struct map *map)
{
	int ret;

	ret = unwind__prepare_access(thread__maps(thread), map, NULL);
	if (ret)
		return ret;

	maps__fixup_overlappings(thread__maps(thread), map, stderr);
	return maps__insert(thread__maps(thread), map);
}

static int __thread__prepare_access(struct thread *thread)
{
	bool initialized = false;
	int err = 0;
	struct maps *maps = thread__maps(thread);
	struct map_rb_node *rb_node;

	down_read(maps__lock(maps));

	maps__for_each_entry(maps, rb_node) {
		err = unwind__prepare_access(thread__maps(thread), rb_node->map, &initialized);
		if (err || initialized)
			break;
	}

	up_read(maps__lock(maps));

	return err;
}

static int thread__prepare_access(struct thread *thread)
{
	int err = 0;

	if (dwarf_callchain_users)
		err = __thread__prepare_access(thread);

	return err;
}

static int thread__clone_maps(struct thread *thread, struct thread *parent, bool do_maps_clone)
{
	/* This is new thread, we share map groups for process. */
	if (thread__pid(thread) == thread__pid(parent))
		return thread__prepare_access(thread);

	if (thread__maps(thread) == thread__maps(parent)) {
		pr_debug("broken map groups on thread %d/%d parent %d/%d\n",
			 thread__pid(thread), thread__tid(thread),
			 thread__pid(parent), thread__tid(parent));
		return 0;
	}
	/* But this one is new process, copy maps. */
	return do_maps_clone ? maps__clone(thread, thread__maps(parent)) : 0;
}

int thread__fork(struct thread *thread, struct thread *parent, u64 timestamp, bool do_maps_clone)
{
	if (thread__comm_set(parent)) {
		const char *comm = thread__comm_str(parent);
		int err;
		if (!comm)
			return -ENOMEM;
		err = thread__set_comm(thread, comm, timestamp);
		if (err)
			return err;
	}

	thread__set_ppid(thread, thread__tid(parent));
	return thread__clone_maps(thread, parent, do_maps_clone);
}

void thread__find_cpumode_addr_location(struct thread *thread, u64 addr,
					struct addr_location *al)
{
	size_t i;
	const u8 cpumodes[] = {
		PERF_RECORD_MISC_USER,
		PERF_RECORD_MISC_KERNEL,
		PERF_RECORD_MISC_GUEST_USER,
		PERF_RECORD_MISC_GUEST_KERNEL
	};

	for (i = 0; i < ARRAY_SIZE(cpumodes); i++) {
		thread__find_symbol(thread, cpumodes[i], addr, al);
		if (al->map)
			break;
	}
}

struct thread *thread__main_thread(struct machine *machine, struct thread *thread)
{
	if (thread__pid(thread) == thread__tid(thread))
		return thread__get(thread);

	if (thread__pid(thread) == -1)
		return NULL;

	return machine__find_thread(machine, thread__pid(thread), thread__pid(thread));
}

int thread__memcpy(struct thread *thread, struct machine *machine,
		   void *buf, u64 ip, int len, bool *is64bit)
{
	u8 cpumode = PERF_RECORD_MISC_USER;
	struct addr_location al;
	struct dso *dso;
	long offset;

	if (machine__kernel_ip(machine, ip))
		cpumode = PERF_RECORD_MISC_KERNEL;

	addr_location__init(&al);
	if (!thread__find_map(thread, cpumode, ip, &al)) {
		addr_location__exit(&al);
		return -1;
	}

	dso = map__dso(al.map);

	if (!dso || dso->data.status == DSO_DATA_STATUS_ERROR || map__load(al.map) < 0) {
		addr_location__exit(&al);
		return -1;
	}

	offset = map__map_ip(al.map, ip);
	if (is64bit)
		*is64bit = dso->is_64_bit;

	addr_location__exit(&al);

	return dso__data_read_offset(dso, machine, offset, buf, len);
}

void thread__free_stitch_list(struct thread *thread)
{
	struct lbr_stitch *lbr_stitch = thread__lbr_stitch(thread);
	struct stitch_list *pos, *tmp;

	if (!lbr_stitch)
		return;

	list_for_each_entry_safe(pos, tmp, &lbr_stitch->lists, node) {
		list_del_init(&pos->node);
		free(pos);
	}

	list_for_each_entry_safe(pos, tmp, &lbr_stitch->free_lists, node) {
		list_del_init(&pos->node);
		free(pos);
	}

	zfree(&lbr_stitch->prev_lbr_cursor);
	free(thread__lbr_stitch(thread));
	thread__set_lbr_stitch(thread, NULL);
}
