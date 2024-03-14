// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2019 Facebook */

#include <linux/bpf.h>
#include <linux/bpf_verifier.h>
#include <linux/btf.h>
#include <linux/filter.h>
#include <linux/slab.h>
#include <linux/numa.h>
#include <linux/seq_file.h>
#include <linux/refcount.h>
#include <linux/mutex.h>
#include <linux/btf_ids.h>
#include <linux/rcupdate_wait.h>

enum bpf_struct_ops_state {
	BPF_STRUCT_OPS_STATE_INIT,
	BPF_STRUCT_OPS_STATE_INUSE,
	BPF_STRUCT_OPS_STATE_TOBEFREE,
	BPF_STRUCT_OPS_STATE_READY,
};

#define BPF_STRUCT_OPS_COMMON_VALUE			\
	refcount_t refcnt;				\
	enum bpf_struct_ops_state state

struct bpf_struct_ops_value {
	BPF_STRUCT_OPS_COMMON_VALUE;
	char data[] ____cacheline_aligned_in_smp;
};

struct bpf_struct_ops_map {
	struct bpf_map map;
	struct rcu_head rcu;
	const struct bpf_struct_ops *st_ops;
	/* protect map_update */
	struct mutex lock;
	/* link has all the bpf_links that is populated
	 * to the func ptr of the kernel's struct
	 * (in kvalue.data).
	 */
	struct bpf_link **links;
	/* image is a page that has all the trampolines
	 * that stores the func args before calling the bpf_prog.
	 * A PAGE_SIZE "image" is enough to store all trampoline for
	 * "links[]".
	 */
	void *image;
	/* uvalue->data stores the kernel struct
	 * (e.g. tcp_congestion_ops) that is more useful
	 * to userspace than the kvalue.  For example,
	 * the bpf_prog's id is stored instead of the kernel
	 * address of a func ptr.
	 */
	struct bpf_struct_ops_value *uvalue;
	/* kvalue.data stores the actual kernel's struct
	 * (e.g. tcp_congestion_ops) that will be
	 * registered to the kernel subsystem.
	 */
	struct bpf_struct_ops_value kvalue;
};

struct bpf_struct_ops_link {
	struct bpf_link link;
	struct bpf_map __rcu *map;
};

static DEFINE_MUTEX(update_mutex);

#define VALUE_PREFIX "bpf_struct_ops_"
#define VALUE_PREFIX_LEN (sizeof(VALUE_PREFIX) - 1)

/* bpf_struct_ops_##_name (e.g. bpf_struct_ops_tcp_congestion_ops) is
 * the map's value exposed to the userspace and its btf-type-id is
 * stored at the map->btf_vmlinux_value_type_id.
 *
 */
#define BPF_STRUCT_OPS_TYPE(_name)				\
extern struct bpf_struct_ops bpf_##_name;			\
								\
struct bpf_struct_ops_##_name {						\
	BPF_STRUCT_OPS_COMMON_VALUE;				\
	struct _name data ____cacheline_aligned_in_smp;		\
};
#include "bpf_struct_ops_types.h"
#undef BPF_STRUCT_OPS_TYPE

enum {
#define BPF_STRUCT_OPS_TYPE(_name) BPF_STRUCT_OPS_TYPE_##_name,
#include "bpf_struct_ops_types.h"
#undef BPF_STRUCT_OPS_TYPE
	__NR_BPF_STRUCT_OPS_TYPE,
};

static struct bpf_struct_ops * const bpf_struct_ops[] = {
#define BPF_STRUCT_OPS_TYPE(_name)				\
	[BPF_STRUCT_OPS_TYPE_##_name] = &bpf_##_name,
#include "bpf_struct_ops_types.h"
#undef BPF_STRUCT_OPS_TYPE
};

const struct bpf_verifier_ops bpf_struct_ops_verifier_ops = {
};

const struct bpf_prog_ops bpf_struct_ops_prog_ops = {
#ifdef CONFIG_NET
	.test_run = bpf_struct_ops_test_run,
#endif
};

static const struct btf_type *module_type;

void bpf_struct_ops_init(struct btf *btf, struct bpf_verifier_log *log)
{
	s32 type_id, value_id, module_id;
	const struct btf_member *member;
	struct bpf_struct_ops *st_ops;
	const struct btf_type *t;
	char value_name[128];
	const char *mname;
	u32 i, j;

	/* Ensure BTF type is emitted for "struct bpf_struct_ops_##_name" */
#define BPF_STRUCT_OPS_TYPE(_name) BTF_TYPE_EMIT(struct bpf_struct_ops_##_name);
#include "bpf_struct_ops_types.h"
#undef BPF_STRUCT_OPS_TYPE

	module_id = btf_find_by_name_kind(btf, "module", BTF_KIND_STRUCT);
	if (module_id < 0) {
		pr_warn("Cannot find struct module in btf_vmlinux\n");
		return;
	}
	module_type = btf_type_by_id(btf, module_id);

	for (i = 0; i < ARRAY_SIZE(bpf_struct_ops); i++) {
		st_ops = bpf_struct_ops[i];

		if (strlen(st_ops->name) + VALUE_PREFIX_LEN >=
		    sizeof(value_name)) {
			pr_warn("struct_ops name %s is too long\n",
				st_ops->name);
			continue;
		}
		sprintf(value_name, "%s%s", VALUE_PREFIX, st_ops->name);

		value_id = btf_find_by_name_kind(btf, value_name,
						 BTF_KIND_STRUCT);
		if (value_id < 0) {
			pr_warn("Cannot find struct %s in btf_vmlinux\n",
				value_name);
			continue;
		}

		type_id = btf_find_by_name_kind(btf, st_ops->name,
						BTF_KIND_STRUCT);
		if (type_id < 0) {
			pr_warn("Cannot find struct %s in btf_vmlinux\n",
				st_ops->name);
			continue;
		}
		t = btf_type_by_id(btf, type_id);
		if (btf_type_vlen(t) > BPF_STRUCT_OPS_MAX_NR_MEMBERS) {
			pr_warn("Cannot support #%u members in struct %s\n",
				btf_type_vlen(t), st_ops->name);
			continue;
		}

		for_each_member(j, t, member) {
			const struct btf_type *func_proto;

			mname = btf_name_by_offset(btf, member->name_off);
			if (!*mname) {
				pr_warn("anon member in struct %s is not supported\n",
					st_ops->name);
				break;
			}

			if (__btf_member_bitfield_size(t, member)) {
				pr_warn("bit field member %s in struct %s is not supported\n",
					mname, st_ops->name);
				break;
			}

			func_proto = btf_type_resolve_func_ptr(btf,
							       member->type,
							       NULL);
			if (func_proto &&
			    btf_distill_func_proto(log, btf,
						   func_proto, mname,
						   &st_ops->func_models[j])) {
				pr_warn("Error in parsing func ptr %s in struct %s\n",
					mname, st_ops->name);
				break;
			}
		}

		if (j == btf_type_vlen(t)) {
			if (st_ops->init(btf)) {
				pr_warn("Error in init bpf_struct_ops %s\n",
					st_ops->name);
			} else {
				st_ops->type_id = type_id;
				st_ops->type = t;
				st_ops->value_id = value_id;
				st_ops->value_type = btf_type_by_id(btf,
								    value_id);
			}
		}
	}
}

extern struct btf *btf_vmlinux;

static const struct bpf_struct_ops *
bpf_struct_ops_find_value(u32 value_id)
{
	unsigned int i;

	if (!value_id || !btf_vmlinux)
		return NULL;

	for (i = 0; i < ARRAY_SIZE(bpf_struct_ops); i++) {
		if (bpf_struct_ops[i]->value_id == value_id)
			return bpf_struct_ops[i];
	}

	return NULL;
}

const struct bpf_struct_ops *bpf_struct_ops_find(u32 type_id)
{
	unsigned int i;

	if (!type_id || !btf_vmlinux)
		return NULL;

	for (i = 0; i < ARRAY_SIZE(bpf_struct_ops); i++) {
		if (bpf_struct_ops[i]->type_id == type_id)
			return bpf_struct_ops[i];
	}

	return NULL;
}

static int bpf_struct_ops_map_get_next_key(struct bpf_map *map, void *key,
					   void *next_key)
{
	if (key && *(u32 *)key == 0)
		return -ENOENT;

	*(u32 *)next_key = 0;
	return 0;
}

int bpf_struct_ops_map_sys_lookup_elem(struct bpf_map *map, void *key,
				       void *value)
{
	struct bpf_struct_ops_map *st_map = (struct bpf_struct_ops_map *)map;
	struct bpf_struct_ops_value *uvalue, *kvalue;
	enum bpf_struct_ops_state state;
	s64 refcnt;

	if (unlikely(*(u32 *)key != 0))
		return -ENOENT;

	kvalue = &st_map->kvalue;
	/* Pair with smp_store_release() during map_update */
	state = smp_load_acquire(&kvalue->state);
	if (state == BPF_STRUCT_OPS_STATE_INIT) {
		memset(value, 0, map->value_size);
		return 0;
	}

	/* No lock is needed.  state and refcnt do not need
	 * to be updated together under atomic context.
	 */
	uvalue = value;
	memcpy(uvalue, st_map->uvalue, map->value_size);
	uvalue->state = state;

	/* This value offers the user space a general estimate of how
	 * many sockets are still utilizing this struct_ops for TCP
	 * congestion control. The number might not be exact, but it
	 * should sufficiently meet our present goals.
	 */
	refcnt = atomic64_read(&map->refcnt) - atomic64_read(&map->usercnt);
	refcount_set(&uvalue->refcnt, max_t(s64, refcnt, 0));

	return 0;
}

static void *bpf_struct_ops_map_lookup_elem(struct bpf_map *map, void *key)
{
	return ERR_PTR(-EINVAL);
}

static void bpf_struct_ops_map_put_progs(struct bpf_struct_ops_map *st_map)
{
	const struct btf_type *t = st_map->st_ops->type;
	u32 i;

	for (i = 0; i < btf_type_vlen(t); i++) {
		if (st_map->links[i]) {
			bpf_link_put(st_map->links[i]);
			st_map->links[i] = NULL;
		}
	}
}

static int check_zero_holes(const struct btf_type *t, void *data)
{
	const struct btf_member *member;
	u32 i, moff, msize, prev_mend = 0;
	const struct btf_type *mtype;

	for_each_member(i, t, member) {
		moff = __btf_member_bit_offset(t, member) / 8;
		if (moff > prev_mend &&
		    memchr_inv(data + prev_mend, 0, moff - prev_mend))
			return -EINVAL;

		mtype = btf_type_by_id(btf_vmlinux, member->type);
		mtype = btf_resolve_size(btf_vmlinux, mtype, &msize);
		if (IS_ERR(mtype))
			return PTR_ERR(mtype);
		prev_mend = moff + msize;
	}

	if (t->size > prev_mend &&
	    memchr_inv(data + prev_mend, 0, t->size - prev_mend))
		return -EINVAL;

	return 0;
}

static void bpf_struct_ops_link_release(struct bpf_link *link)
{
}

static void bpf_struct_ops_link_dealloc(struct bpf_link *link)
{
	struct bpf_tramp_link *tlink = container_of(link, struct bpf_tramp_link, link);

	kfree(tlink);
}

const struct bpf_link_ops bpf_struct_ops_link_lops = {
	.release = bpf_struct_ops_link_release,
	.dealloc = bpf_struct_ops_link_dealloc,
};

int bpf_struct_ops_prepare_trampoline(struct bpf_tramp_links *tlinks,
				      struct bpf_tramp_link *link,
				      const struct btf_func_model *model,
				      void *image, void *image_end)
{
	u32 flags;

	tlinks[BPF_TRAMP_FENTRY].links[0] = link;
	tlinks[BPF_TRAMP_FENTRY].nr_links = 1;
	/* BPF_TRAMP_F_RET_FENTRY_RET is only used by bpf_struct_ops,
	 * and it must be used alone.
	 */
	flags = model->ret_size > 0 ? BPF_TRAMP_F_RET_FENTRY_RET : 0;
	return arch_prepare_bpf_trampoline(NULL, image, image_end,
					   model, flags, tlinks, NULL);
}

static long bpf_struct_ops_map_update_elem(struct bpf_map *map, void *key,
					   void *value, u64 flags)
{
	struct bpf_struct_ops_map *st_map = (struct bpf_struct_ops_map *)map;
	const struct bpf_struct_ops *st_ops = st_map->st_ops;
	struct bpf_struct_ops_value *uvalue, *kvalue;
	const struct btf_member *member;
	const struct btf_type *t = st_ops->type;
	struct bpf_tramp_links *tlinks;
	void *udata, *kdata;
	int prog_fd, err;
	void *image, *image_end;
	u32 i;

	if (flags)
		return -EINVAL;

	if (*(u32 *)key != 0)
		return -E2BIG;

	err = check_zero_holes(st_ops->value_type, value);
	if (err)
		return err;

	uvalue = value;
	err = check_zero_holes(t, uvalue->data);
	if (err)
		return err;

	if (uvalue->state || refcount_read(&uvalue->refcnt))
		return -EINVAL;

	tlinks = kcalloc(BPF_TRAMP_MAX, sizeof(*tlinks), GFP_KERNEL);
	if (!tlinks)
		return -ENOMEM;

	uvalue = (struct bpf_struct_ops_value *)st_map->uvalue;
	kvalue = (struct bpf_struct_ops_value *)&st_map->kvalue;

	mutex_lock(&st_map->lock);

	if (kvalue->state != BPF_STRUCT_OPS_STATE_INIT) {
		err = -EBUSY;
		goto unlock;
	}

	memcpy(uvalue, value, map->value_size);

	udata = &uvalue->data;
	kdata = &kvalue->data;
	image = st_map->image;
	image_end = st_map->image + PAGE_SIZE;

	for_each_member(i, t, member) {
		const struct btf_type *mtype, *ptype;
		struct bpf_prog *prog;
		struct bpf_tramp_link *link;
		u32 moff;

		moff = __btf_member_bit_offset(t, member) / 8;
		ptype = btf_type_resolve_ptr(btf_vmlinux, member->type, NULL);
		if (ptype == module_type) {
			if (*(void **)(udata + moff))
				goto reset_unlock;
			*(void **)(kdata + moff) = BPF_MODULE_OWNER;
			continue;
		}

		err = st_ops->init_member(t, member, kdata, udata);
		if (err < 0)
			goto reset_unlock;

		/* The ->init_member() has handled this member */
		if (err > 0)
			continue;

		/* If st_ops->init_member does not handle it,
		 * we will only handle func ptrs and zero-ed members
		 * here.  Reject everything else.
		 */

		/* All non func ptr member must be 0 */
		if (!ptype || !btf_type_is_func_proto(ptype)) {
			u32 msize;

			mtype = btf_type_by_id(btf_vmlinux, member->type);
			mtype = btf_resolve_size(btf_vmlinux, mtype, &msize);
			if (IS_ERR(mtype)) {
				err = PTR_ERR(mtype);
				goto reset_unlock;
			}

			if (memchr_inv(udata + moff, 0, msize)) {
				err = -EINVAL;
				goto reset_unlock;
			}

			continue;
		}

		prog_fd = (int)(*(unsigned long *)(udata + moff));
		/* Similar check as the attr->attach_prog_fd */
		if (!prog_fd)
			continue;

		prog = bpf_prog_get(prog_fd);
		if (IS_ERR(prog)) {
			err = PTR_ERR(prog);
			goto reset_unlock;
		}

		if (prog->type != BPF_PROG_TYPE_STRUCT_OPS ||
		    prog->aux->attach_btf_id != st_ops->type_id ||
		    prog->expected_attach_type != i) {
			bpf_prog_put(prog);
			err = -EINVAL;
			goto reset_unlock;
		}

		link = kzalloc(sizeof(*link), GFP_USER);
		if (!link) {
			bpf_prog_put(prog);
			err = -ENOMEM;
			goto reset_unlock;
		}
		bpf_link_init(&link->link, BPF_LINK_TYPE_STRUCT_OPS,
			      &bpf_struct_ops_link_lops, prog);
		st_map->links[i] = &link->link;

		err = bpf_struct_ops_prepare_trampoline(tlinks, link,
							&st_ops->func_models[i],
							image, image_end);
		if (err < 0)
			goto reset_unlock;

		*(void **)(kdata + moff) = image;
		image += err;

		/* put prog_id to udata */
		*(unsigned long *)(udata + moff) = prog->aux->id;
	}

	if (st_map->map.map_flags & BPF_F_LINK) {
		err = 0;
		if (st_ops->validate) {
			err = st_ops->validate(kdata);
			if (err)
				goto reset_unlock;
		}
		set_memory_rox((long)st_map->image, 1);
		/* Let bpf_link handle registration & unregistration.
		 *
		 * Pair with smp_load_acquire() during lookup_elem().
		 */
		smp_store_release(&kvalue->state, BPF_STRUCT_OPS_STATE_READY);
		goto unlock;
	}

	set_memory_rox((long)st_map->image, 1);
	err = st_ops->reg(kdata);
	if (likely(!err)) {
		/* This refcnt increment on the map here after
		 * 'st_ops->reg()' is secure since the state of the
		 * map must be set to INIT at this moment, and thus
		 * bpf_struct_ops_map_delete_elem() can't unregister
		 * or transition it to TOBEFREE concurrently.
		 */
		bpf_map_inc(map);
		/* Pair with smp_load_acquire() during lookup_elem().
		 * It ensures the above udata updates (e.g. prog->aux->id)
		 * can be seen once BPF_STRUCT_OPS_STATE_INUSE is set.
		 */
		smp_store_release(&kvalue->state, BPF_STRUCT_OPS_STATE_INUSE);
		goto unlock;
	}

	/* Error during st_ops->reg(). Can happen if this struct_ops needs to be
	 * verified as a whole, after all init_member() calls. Can also happen if
	 * there was a race in registering the struct_ops (under the same name) to
	 * a sub-system through different struct_ops's maps.
	 */
	set_memory_nx((long)st_map->image, 1);
	set_memory_rw((long)st_map->image, 1);

reset_unlock:
	bpf_struct_ops_map_put_progs(st_map);
	memset(uvalue, 0, map->value_size);
	memset(kvalue, 0, map->value_size);
unlock:
	kfree(tlinks);
	mutex_unlock(&st_map->lock);
	return err;
}

static long bpf_struct_ops_map_delete_elem(struct bpf_map *map, void *key)
{
	enum bpf_struct_ops_state prev_state;
	struct bpf_struct_ops_map *st_map;

	st_map = (struct bpf_struct_ops_map *)map;
	if (st_map->map.map_flags & BPF_F_LINK)
		return -EOPNOTSUPP;

	prev_state = cmpxchg(&st_map->kvalue.state,
			     BPF_STRUCT_OPS_STATE_INUSE,
			     BPF_STRUCT_OPS_STATE_TOBEFREE);
	switch (prev_state) {
	case BPF_STRUCT_OPS_STATE_INUSE:
		st_map->st_ops->unreg(&st_map->kvalue.data);
		bpf_map_put(map);
		return 0;
	case BPF_STRUCT_OPS_STATE_TOBEFREE:
		return -EINPROGRESS;
	case BPF_STRUCT_OPS_STATE_INIT:
		return -ENOENT;
	default:
		WARN_ON_ONCE(1);
		/* Should never happen.  Treat it as not found. */
		return -ENOENT;
	}
}

static void bpf_struct_ops_map_seq_show_elem(struct bpf_map *map, void *key,
					     struct seq_file *m)
{
	void *value;
	int err;

	value = kmalloc(map->value_size, GFP_USER | __GFP_NOWARN);
	if (!value)
		return;

	err = bpf_struct_ops_map_sys_lookup_elem(map, key, value);
	if (!err) {
		btf_type_seq_show(btf_vmlinux, map->btf_vmlinux_value_type_id,
				  value, m);
		seq_puts(m, "\n");
	}

	kfree(value);
}

static void __bpf_struct_ops_map_free(struct bpf_map *map)
{
	struct bpf_struct_ops_map *st_map = (struct bpf_struct_ops_map *)map;

	if (st_map->links)
		bpf_struct_ops_map_put_progs(st_map);
	bpf_map_area_free(st_map->links);
	bpf_jit_free_exec(st_map->image);
	bpf_map_area_free(st_map->uvalue);
	bpf_map_area_free(st_map);
}

static void bpf_struct_ops_map_free(struct bpf_map *map)
{
	/* The struct_ops's function may switch to another struct_ops.
	 *
	 * For example, bpf_tcp_cc_x->init() may switch to
	 * another tcp_cc_y by calling
	 * setsockopt(TCP_CONGESTION, "tcp_cc_y").
	 * During the switch,  bpf_struct_ops_put(tcp_cc_x) is called
	 * and its refcount may reach 0 which then free its
	 * trampoline image while tcp_cc_x is still running.
	 *
	 * A vanilla rcu gp is to wait for all bpf-tcp-cc prog
	 * to finish. bpf-tcp-cc prog is non sleepable.
	 * A rcu_tasks gp is to wait for the last few insn
	 * in the tramopline image to finish before releasing
	 * the trampoline image.
	 */
	synchronize_rcu_mult(call_rcu, call_rcu_tasks);

	__bpf_struct_ops_map_free(map);
}

static int bpf_struct_ops_map_alloc_check(union bpf_attr *attr)
{
	if (attr->key_size != sizeof(unsigned int) || attr->max_entries != 1 ||
	    (attr->map_flags & ~BPF_F_LINK) || !attr->btf_vmlinux_value_type_id)
		return -EINVAL;
	return 0;
}

static struct bpf_map *bpf_struct_ops_map_alloc(union bpf_attr *attr)
{
	const struct bpf_struct_ops *st_ops;
	size_t st_map_size;
	struct bpf_struct_ops_map *st_map;
	const struct btf_type *t, *vt;
	struct bpf_map *map;

	st_ops = bpf_struct_ops_find_value(attr->btf_vmlinux_value_type_id);
	if (!st_ops)
		return ERR_PTR(-ENOTSUPP);

	vt = st_ops->value_type;
	if (attr->value_size != vt->size)
		return ERR_PTR(-EINVAL);

	t = st_ops->type;

	st_map_size = sizeof(*st_map) +
		/* kvalue stores the
		 * struct bpf_struct_ops_tcp_congestions_ops
		 */
		(vt->size - sizeof(struct bpf_struct_ops_value));

	st_map = bpf_map_area_alloc(st_map_size, NUMA_NO_NODE);
	if (!st_map)
		return ERR_PTR(-ENOMEM);

	st_map->st_ops = st_ops;
	map = &st_map->map;

	st_map->uvalue = bpf_map_area_alloc(vt->size, NUMA_NO_NODE);
	st_map->links =
		bpf_map_area_alloc(btf_type_vlen(t) * sizeof(struct bpf_links *),
				   NUMA_NO_NODE);
	st_map->image = bpf_jit_alloc_exec(PAGE_SIZE);
	if (!st_map->uvalue || !st_map->links || !st_map->image) {
		__bpf_struct_ops_map_free(map);
		return ERR_PTR(-ENOMEM);
	}

	mutex_init(&st_map->lock);
	set_vm_flush_reset_perms(st_map->image);
	bpf_map_init_from_attr(map, attr);

	return map;
}

static u64 bpf_struct_ops_map_mem_usage(const struct bpf_map *map)
{
	struct bpf_struct_ops_map *st_map = (struct bpf_struct_ops_map *)map;
	const struct bpf_struct_ops *st_ops = st_map->st_ops;
	const struct btf_type *vt = st_ops->value_type;
	u64 usage;

	usage = sizeof(*st_map) +
			vt->size - sizeof(struct bpf_struct_ops_value);
	usage += vt->size;
	usage += btf_type_vlen(vt) * sizeof(struct bpf_links *);
	usage += PAGE_SIZE;
	return usage;
}

BTF_ID_LIST_SINGLE(bpf_struct_ops_map_btf_ids, struct, bpf_struct_ops_map)
const struct bpf_map_ops bpf_struct_ops_map_ops = {
	.map_alloc_check = bpf_struct_ops_map_alloc_check,
	.map_alloc = bpf_struct_ops_map_alloc,
	.map_free = bpf_struct_ops_map_free,
	.map_get_next_key = bpf_struct_ops_map_get_next_key,
	.map_lookup_elem = bpf_struct_ops_map_lookup_elem,
	.map_delete_elem = bpf_struct_ops_map_delete_elem,
	.map_update_elem = bpf_struct_ops_map_update_elem,
	.map_seq_show_elem = bpf_struct_ops_map_seq_show_elem,
	.map_mem_usage = bpf_struct_ops_map_mem_usage,
	.map_btf_id = &bpf_struct_ops_map_btf_ids[0],
};

/* "const void *" because some subsystem is
 * passing a const (e.g. const struct tcp_congestion_ops *)
 */
bool bpf_struct_ops_get(const void *kdata)
{
	struct bpf_struct_ops_value *kvalue;
	struct bpf_struct_ops_map *st_map;
	struct bpf_map *map;

	kvalue = container_of(kdata, struct bpf_struct_ops_value, data);
	st_map = container_of(kvalue, struct bpf_struct_ops_map, kvalue);

	map = __bpf_map_inc_not_zero(&st_map->map, false);
	return !IS_ERR(map);
}

void bpf_struct_ops_put(const void *kdata)
{
	struct bpf_struct_ops_value *kvalue;
	struct bpf_struct_ops_map *st_map;

	kvalue = container_of(kdata, struct bpf_struct_ops_value, data);
	st_map = container_of(kvalue, struct bpf_struct_ops_map, kvalue);

	bpf_map_put(&st_map->map);
}

static bool bpf_struct_ops_valid_to_reg(struct bpf_map *map)
{
	struct bpf_struct_ops_map *st_map = (struct bpf_struct_ops_map *)map;

	return map->map_type == BPF_MAP_TYPE_STRUCT_OPS &&
		map->map_flags & BPF_F_LINK &&
		/* Pair with smp_store_release() during map_update */
		smp_load_acquire(&st_map->kvalue.state) == BPF_STRUCT_OPS_STATE_READY;
}

static void bpf_struct_ops_map_link_dealloc(struct bpf_link *link)
{
	struct bpf_struct_ops_link *st_link;
	struct bpf_struct_ops_map *st_map;

	st_link = container_of(link, struct bpf_struct_ops_link, link);
	st_map = (struct bpf_struct_ops_map *)
		rcu_dereference_protected(st_link->map, true);
	if (st_map) {
		/* st_link->map can be NULL if
		 * bpf_struct_ops_link_create() fails to register.
		 */
		st_map->st_ops->unreg(&st_map->kvalue.data);
		bpf_map_put(&st_map->map);
	}
	kfree(st_link);
}

static void bpf_struct_ops_map_link_show_fdinfo(const struct bpf_link *link,
					    struct seq_file *seq)
{
	struct bpf_struct_ops_link *st_link;
	struct bpf_map *map;

	st_link = container_of(link, struct bpf_struct_ops_link, link);
	rcu_read_lock();
	map = rcu_dereference(st_link->map);
	seq_printf(seq, "map_id:\t%d\n", map->id);
	rcu_read_unlock();
}

static int bpf_struct_ops_map_link_fill_link_info(const struct bpf_link *link,
					       struct bpf_link_info *info)
{
	struct bpf_struct_ops_link *st_link;
	struct bpf_map *map;

	st_link = container_of(link, struct bpf_struct_ops_link, link);
	rcu_read_lock();
	map = rcu_dereference(st_link->map);
	info->struct_ops.map_id = map->id;
	rcu_read_unlock();
	return 0;
}

static int bpf_struct_ops_map_link_update(struct bpf_link *link, struct bpf_map *new_map,
					  struct bpf_map *expected_old_map)
{
	struct bpf_struct_ops_map *st_map, *old_st_map;
	struct bpf_map *old_map;
	struct bpf_struct_ops_link *st_link;
	int err;

	st_link = container_of(link, struct bpf_struct_ops_link, link);
	st_map = container_of(new_map, struct bpf_struct_ops_map, map);

	if (!bpf_struct_ops_valid_to_reg(new_map))
		return -EINVAL;

	if (!st_map->st_ops->update)
		return -EOPNOTSUPP;

	mutex_lock(&update_mutex);

	old_map = rcu_dereference_protected(st_link->map, lockdep_is_held(&update_mutex));
	if (expected_old_map && old_map != expected_old_map) {
		err = -EPERM;
		goto err_out;
	}

	old_st_map = container_of(old_map, struct bpf_struct_ops_map, map);
	/* The new and old struct_ops must be the same type. */
	if (st_map->st_ops != old_st_map->st_ops) {
		err = -EINVAL;
		goto err_out;
	}

	err = st_map->st_ops->update(st_map->kvalue.data, old_st_map->kvalue.data);
	if (err)
		goto err_out;

	bpf_map_inc(new_map);
	rcu_assign_pointer(st_link->map, new_map);
	bpf_map_put(old_map);

err_out:
	mutex_unlock(&update_mutex);

	return err;
}

static const struct bpf_link_ops bpf_struct_ops_map_lops = {
	.dealloc = bpf_struct_ops_map_link_dealloc,
	.show_fdinfo = bpf_struct_ops_map_link_show_fdinfo,
	.fill_link_info = bpf_struct_ops_map_link_fill_link_info,
	.update_map = bpf_struct_ops_map_link_update,
};

int bpf_struct_ops_link_create(union bpf_attr *attr)
{
	struct bpf_struct_ops_link *link = NULL;
	struct bpf_link_primer link_primer;
	struct bpf_struct_ops_map *st_map;
	struct bpf_map *map;
	int err;

	map = bpf_map_get(attr->link_create.map_fd);
	if (IS_ERR(map))
		return PTR_ERR(map);

	st_map = (struct bpf_struct_ops_map *)map;

	if (!bpf_struct_ops_valid_to_reg(map)) {
		err = -EINVAL;
		goto err_out;
	}

	link = kzalloc(sizeof(*link), GFP_USER);
	if (!link) {
		err = -ENOMEM;
		goto err_out;
	}
	bpf_link_init(&link->link, BPF_LINK_TYPE_STRUCT_OPS, &bpf_struct_ops_map_lops, NULL);

	err = bpf_link_prime(&link->link, &link_primer);
	if (err)
		goto err_out;

	err = st_map->st_ops->reg(st_map->kvalue.data);
	if (err) {
		bpf_link_cleanup(&link_primer);
		link = NULL;
		goto err_out;
	}
	RCU_INIT_POINTER(link->map, map);

	return bpf_link_settle(&link_primer);

err_out:
	bpf_map_put(map);
	kfree(link);
	return err;
}

