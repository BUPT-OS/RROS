// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Facebook
 * Copyright 2020 Google LLC.
 */

#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/rculist.h>
#include <linux/list.h>
#include <linux/hash.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/bpf.h>
#include <linux/bpf_local_storage.h>
#include <linux/filter.h>
#include <uapi/linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/fdtable.h>
#include <linux/rcupdate_trace.h>

DEFINE_BPF_STORAGE_CACHE(task_cache);

static DEFINE_PER_CPU(int, bpf_task_storage_busy);

static void bpf_task_storage_lock(void)
{
	migrate_disable();
	this_cpu_inc(bpf_task_storage_busy);
}

static void bpf_task_storage_unlock(void)
{
	this_cpu_dec(bpf_task_storage_busy);
	migrate_enable();
}

static bool bpf_task_storage_trylock(void)
{
	migrate_disable();
	if (unlikely(this_cpu_inc_return(bpf_task_storage_busy) != 1)) {
		this_cpu_dec(bpf_task_storage_busy);
		migrate_enable();
		return false;
	}
	return true;
}

static struct bpf_local_storage __rcu **task_storage_ptr(void *owner)
{
	struct task_struct *task = owner;

	return &task->bpf_storage;
}

static struct bpf_local_storage_data *
task_storage_lookup(struct task_struct *task, struct bpf_map *map,
		    bool cacheit_lockit)
{
	struct bpf_local_storage *task_storage;
	struct bpf_local_storage_map *smap;

	task_storage =
		rcu_dereference_check(task->bpf_storage, bpf_rcu_lock_held());
	if (!task_storage)
		return NULL;

	smap = (struct bpf_local_storage_map *)map;
	return bpf_local_storage_lookup(task_storage, smap, cacheit_lockit);
}

void bpf_task_storage_free(struct task_struct *task)
{
	struct bpf_local_storage *local_storage;

	rcu_read_lock();

	local_storage = rcu_dereference(task->bpf_storage);
	if (!local_storage) {
		rcu_read_unlock();
		return;
	}

	bpf_task_storage_lock();
	bpf_local_storage_destroy(local_storage);
	bpf_task_storage_unlock();
	rcu_read_unlock();
}

static void *bpf_pid_task_storage_lookup_elem(struct bpf_map *map, void *key)
{
	struct bpf_local_storage_data *sdata;
	struct task_struct *task;
	unsigned int f_flags;
	struct pid *pid;
	int fd, err;

	fd = *(int *)key;
	pid = pidfd_get_pid(fd, &f_flags);
	if (IS_ERR(pid))
		return ERR_CAST(pid);

	/* We should be in an RCU read side critical section, it should be safe
	 * to call pid_task.
	 */
	WARN_ON_ONCE(!rcu_read_lock_held());
	task = pid_task(pid, PIDTYPE_PID);
	if (!task) {
		err = -ENOENT;
		goto out;
	}

	bpf_task_storage_lock();
	sdata = task_storage_lookup(task, map, true);
	bpf_task_storage_unlock();
	put_pid(pid);
	return sdata ? sdata->data : NULL;
out:
	put_pid(pid);
	return ERR_PTR(err);
}

static long bpf_pid_task_storage_update_elem(struct bpf_map *map, void *key,
					     void *value, u64 map_flags)
{
	struct bpf_local_storage_data *sdata;
	struct task_struct *task;
	unsigned int f_flags;
	struct pid *pid;
	int fd, err;

	fd = *(int *)key;
	pid = pidfd_get_pid(fd, &f_flags);
	if (IS_ERR(pid))
		return PTR_ERR(pid);

	/* We should be in an RCU read side critical section, it should be safe
	 * to call pid_task.
	 */
	WARN_ON_ONCE(!rcu_read_lock_held());
	task = pid_task(pid, PIDTYPE_PID);
	if (!task) {
		err = -ENOENT;
		goto out;
	}

	bpf_task_storage_lock();
	sdata = bpf_local_storage_update(
		task, (struct bpf_local_storage_map *)map, value, map_flags,
		GFP_ATOMIC);
	bpf_task_storage_unlock();

	err = PTR_ERR_OR_ZERO(sdata);
out:
	put_pid(pid);
	return err;
}

static int task_storage_delete(struct task_struct *task, struct bpf_map *map,
			       bool nobusy)
{
	struct bpf_local_storage_data *sdata;

	sdata = task_storage_lookup(task, map, false);
	if (!sdata)
		return -ENOENT;

	if (!nobusy)
		return -EBUSY;

	bpf_selem_unlink(SELEM(sdata), false);

	return 0;
}

static long bpf_pid_task_storage_delete_elem(struct bpf_map *map, void *key)
{
	struct task_struct *task;
	unsigned int f_flags;
	struct pid *pid;
	int fd, err;

	fd = *(int *)key;
	pid = pidfd_get_pid(fd, &f_flags);
	if (IS_ERR(pid))
		return PTR_ERR(pid);

	/* We should be in an RCU read side critical section, it should be safe
	 * to call pid_task.
	 */
	WARN_ON_ONCE(!rcu_read_lock_held());
	task = pid_task(pid, PIDTYPE_PID);
	if (!task) {
		err = -ENOENT;
		goto out;
	}

	bpf_task_storage_lock();
	err = task_storage_delete(task, map, true);
	bpf_task_storage_unlock();
out:
	put_pid(pid);
	return err;
}

/* Called by bpf_task_storage_get*() helpers */
static void *__bpf_task_storage_get(struct bpf_map *map,
				    struct task_struct *task, void *value,
				    u64 flags, gfp_t gfp_flags, bool nobusy)
{
	struct bpf_local_storage_data *sdata;

	sdata = task_storage_lookup(task, map, nobusy);
	if (sdata)
		return sdata->data;

	/* only allocate new storage, when the task is refcounted */
	if (refcount_read(&task->usage) &&
	    (flags & BPF_LOCAL_STORAGE_GET_F_CREATE) && nobusy) {
		sdata = bpf_local_storage_update(
			task, (struct bpf_local_storage_map *)map, value,
			BPF_NOEXIST, gfp_flags);
		return IS_ERR(sdata) ? NULL : sdata->data;
	}

	return NULL;
}

/* *gfp_flags* is a hidden argument provided by the verifier */
BPF_CALL_5(bpf_task_storage_get_recur, struct bpf_map *, map, struct task_struct *,
	   task, void *, value, u64, flags, gfp_t, gfp_flags)
{
	bool nobusy;
	void *data;

	WARN_ON_ONCE(!bpf_rcu_lock_held());
	if (flags & ~BPF_LOCAL_STORAGE_GET_F_CREATE || !task)
		return (unsigned long)NULL;

	nobusy = bpf_task_storage_trylock();
	data = __bpf_task_storage_get(map, task, value, flags,
				      gfp_flags, nobusy);
	if (nobusy)
		bpf_task_storage_unlock();
	return (unsigned long)data;
}

/* *gfp_flags* is a hidden argument provided by the verifier */
BPF_CALL_5(bpf_task_storage_get, struct bpf_map *, map, struct task_struct *,
	   task, void *, value, u64, flags, gfp_t, gfp_flags)
{
	void *data;

	WARN_ON_ONCE(!bpf_rcu_lock_held());
	if (flags & ~BPF_LOCAL_STORAGE_GET_F_CREATE || !task)
		return (unsigned long)NULL;

	bpf_task_storage_lock();
	data = __bpf_task_storage_get(map, task, value, flags,
				      gfp_flags, true);
	bpf_task_storage_unlock();
	return (unsigned long)data;
}

BPF_CALL_2(bpf_task_storage_delete_recur, struct bpf_map *, map, struct task_struct *,
	   task)
{
	bool nobusy;
	int ret;

	WARN_ON_ONCE(!bpf_rcu_lock_held());
	if (!task)
		return -EINVAL;

	nobusy = bpf_task_storage_trylock();
	/* This helper must only be called from places where the lifetime of the task
	 * is guaranteed. Either by being refcounted or by being protected
	 * by an RCU read-side critical section.
	 */
	ret = task_storage_delete(task, map, nobusy);
	if (nobusy)
		bpf_task_storage_unlock();
	return ret;
}

BPF_CALL_2(bpf_task_storage_delete, struct bpf_map *, map, struct task_struct *,
	   task)
{
	int ret;

	WARN_ON_ONCE(!bpf_rcu_lock_held());
	if (!task)
		return -EINVAL;

	bpf_task_storage_lock();
	/* This helper must only be called from places where the lifetime of the task
	 * is guaranteed. Either by being refcounted or by being protected
	 * by an RCU read-side critical section.
	 */
	ret = task_storage_delete(task, map, true);
	bpf_task_storage_unlock();
	return ret;
}

static int notsupp_get_next_key(struct bpf_map *map, void *key, void *next_key)
{
	return -ENOTSUPP;
}

static struct bpf_map *task_storage_map_alloc(union bpf_attr *attr)
{
	return bpf_local_storage_map_alloc(attr, &task_cache, true);
}

static void task_storage_map_free(struct bpf_map *map)
{
	bpf_local_storage_map_free(map, &task_cache, &bpf_task_storage_busy);
}

BTF_ID_LIST_GLOBAL_SINGLE(bpf_local_storage_map_btf_id, struct, bpf_local_storage_map)
const struct bpf_map_ops task_storage_map_ops = {
	.map_meta_equal = bpf_map_meta_equal,
	.map_alloc_check = bpf_local_storage_map_alloc_check,
	.map_alloc = task_storage_map_alloc,
	.map_free = task_storage_map_free,
	.map_get_next_key = notsupp_get_next_key,
	.map_lookup_elem = bpf_pid_task_storage_lookup_elem,
	.map_update_elem = bpf_pid_task_storage_update_elem,
	.map_delete_elem = bpf_pid_task_storage_delete_elem,
	.map_check_btf = bpf_local_storage_map_check_btf,
	.map_mem_usage = bpf_local_storage_map_mem_usage,
	.map_btf_id = &bpf_local_storage_map_btf_id[0],
	.map_owner_storage_ptr = task_storage_ptr,
};

const struct bpf_func_proto bpf_task_storage_get_recur_proto = {
	.func = bpf_task_storage_get_recur,
	.gpl_only = false,
	.ret_type = RET_PTR_TO_MAP_VALUE_OR_NULL,
	.arg1_type = ARG_CONST_MAP_PTR,
	.arg2_type = ARG_PTR_TO_BTF_ID_OR_NULL,
	.arg2_btf_id = &btf_tracing_ids[BTF_TRACING_TYPE_TASK],
	.arg3_type = ARG_PTR_TO_MAP_VALUE_OR_NULL,
	.arg4_type = ARG_ANYTHING,
};

const struct bpf_func_proto bpf_task_storage_get_proto = {
	.func = bpf_task_storage_get,
	.gpl_only = false,
	.ret_type = RET_PTR_TO_MAP_VALUE_OR_NULL,
	.arg1_type = ARG_CONST_MAP_PTR,
	.arg2_type = ARG_PTR_TO_BTF_ID_OR_NULL,
	.arg2_btf_id = &btf_tracing_ids[BTF_TRACING_TYPE_TASK],
	.arg3_type = ARG_PTR_TO_MAP_VALUE_OR_NULL,
	.arg4_type = ARG_ANYTHING,
};

const struct bpf_func_proto bpf_task_storage_delete_recur_proto = {
	.func = bpf_task_storage_delete_recur,
	.gpl_only = false,
	.ret_type = RET_INTEGER,
	.arg1_type = ARG_CONST_MAP_PTR,
	.arg2_type = ARG_PTR_TO_BTF_ID_OR_NULL,
	.arg2_btf_id = &btf_tracing_ids[BTF_TRACING_TYPE_TASK],
};

const struct bpf_func_proto bpf_task_storage_delete_proto = {
	.func = bpf_task_storage_delete,
	.gpl_only = false,
	.ret_type = RET_INTEGER,
	.arg1_type = ARG_CONST_MAP_PTR,
	.arg2_type = ARG_PTR_TO_BTF_ID_OR_NULL,
	.arg2_btf_id = &btf_tracing_ids[BTF_TRACING_TYPE_TASK],
};
