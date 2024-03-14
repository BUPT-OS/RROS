// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2022 Meta Platforms, Inc. and affiliates. */
#include <linux/mm.h>
#include <linux/llist.h>
#include <linux/bpf.h>
#include <linux/irq_work.h>
#include <linux/bpf_mem_alloc.h>
#include <linux/memcontrol.h>
#include <asm/local.h>

/* Any context (including NMI) BPF specific memory allocator.
 *
 * Tracing BPF programs can attach to kprobe and fentry. Hence they
 * run in unknown context where calling plain kmalloc() might not be safe.
 *
 * Front-end kmalloc() with per-cpu per-bucket cache of free elements.
 * Refill this cache asynchronously from irq_work.
 *
 * CPU_0 buckets
 * 16 32 64 96 128 196 256 512 1024 2048 4096
 * ...
 * CPU_N buckets
 * 16 32 64 96 128 196 256 512 1024 2048 4096
 *
 * The buckets are prefilled at the start.
 * BPF programs always run with migration disabled.
 * It's safe to allocate from cache of the current cpu with irqs disabled.
 * Free-ing is always done into bucket of the current cpu as well.
 * irq_work trims extra free elements from buckets with kfree
 * and refills them with kmalloc, so global kmalloc logic takes care
 * of freeing objects allocated by one cpu and freed on another.
 *
 * Every allocated objected is padded with extra 8 bytes that contains
 * struct llist_node.
 */
#define LLIST_NODE_SZ sizeof(struct llist_node)

/* similar to kmalloc, but sizeof == 8 bucket is gone */
static u8 size_index[24] __ro_after_init = {
	3,	/* 8 */
	3,	/* 16 */
	4,	/* 24 */
	4,	/* 32 */
	5,	/* 40 */
	5,	/* 48 */
	5,	/* 56 */
	5,	/* 64 */
	1,	/* 72 */
	1,	/* 80 */
	1,	/* 88 */
	1,	/* 96 */
	6,	/* 104 */
	6,	/* 112 */
	6,	/* 120 */
	6,	/* 128 */
	2,	/* 136 */
	2,	/* 144 */
	2,	/* 152 */
	2,	/* 160 */
	2,	/* 168 */
	2,	/* 176 */
	2,	/* 184 */
	2	/* 192 */
};

static int bpf_mem_cache_idx(size_t size)
{
	if (!size || size > 4096)
		return -1;

	if (size <= 192)
		return size_index[(size - 1) / 8] - 1;

	return fls(size - 1) - 2;
}

#define NUM_CACHES 11

struct bpf_mem_cache {
	/* per-cpu list of free objects of size 'unit_size'.
	 * All accesses are done with interrupts disabled and 'active' counter
	 * protection with __llist_add() and __llist_del_first().
	 */
	struct llist_head free_llist;
	local_t active;

	/* Operations on the free_list from unit_alloc/unit_free/bpf_mem_refill
	 * are sequenced by per-cpu 'active' counter. But unit_free() cannot
	 * fail. When 'active' is busy the unit_free() will add an object to
	 * free_llist_extra.
	 */
	struct llist_head free_llist_extra;

	struct irq_work refill_work;
	struct obj_cgroup *objcg;
	int unit_size;
	/* count of objects in free_llist */
	int free_cnt;
	int low_watermark, high_watermark, batch;
	int percpu_size;
	bool draining;
	struct bpf_mem_cache *tgt;

	/* list of objects to be freed after RCU GP */
	struct llist_head free_by_rcu;
	struct llist_node *free_by_rcu_tail;
	struct llist_head waiting_for_gp;
	struct llist_node *waiting_for_gp_tail;
	struct rcu_head rcu;
	atomic_t call_rcu_in_progress;
	struct llist_head free_llist_extra_rcu;

	/* list of objects to be freed after RCU tasks trace GP */
	struct llist_head free_by_rcu_ttrace;
	struct llist_head waiting_for_gp_ttrace;
	struct rcu_head rcu_ttrace;
	atomic_t call_rcu_ttrace_in_progress;
};

struct bpf_mem_caches {
	struct bpf_mem_cache cache[NUM_CACHES];
};

static struct llist_node notrace *__llist_del_first(struct llist_head *head)
{
	struct llist_node *entry, *next;

	entry = head->first;
	if (!entry)
		return NULL;
	next = entry->next;
	head->first = next;
	return entry;
}

static void *__alloc(struct bpf_mem_cache *c, int node, gfp_t flags)
{
	if (c->percpu_size) {
		void **obj = kmalloc_node(c->percpu_size, flags, node);
		void *pptr = __alloc_percpu_gfp(c->unit_size, 8, flags);

		if (!obj || !pptr) {
			free_percpu(pptr);
			kfree(obj);
			return NULL;
		}
		obj[1] = pptr;
		return obj;
	}

	return kmalloc_node(c->unit_size, flags | __GFP_ZERO, node);
}

static struct mem_cgroup *get_memcg(const struct bpf_mem_cache *c)
{
#ifdef CONFIG_MEMCG_KMEM
	if (c->objcg)
		return get_mem_cgroup_from_objcg(c->objcg);
#endif

#ifdef CONFIG_MEMCG
	return root_mem_cgroup;
#else
	return NULL;
#endif
}

static void inc_active(struct bpf_mem_cache *c, unsigned long *flags)
{
	if (IS_ENABLED(CONFIG_PREEMPT_RT))
		/* In RT irq_work runs in per-cpu kthread, so disable
		 * interrupts to avoid preemption and interrupts and
		 * reduce the chance of bpf prog executing on this cpu
		 * when active counter is busy.
		 */
		local_irq_save(*flags);
	/* alloc_bulk runs from irq_work which will not preempt a bpf
	 * program that does unit_alloc/unit_free since IRQs are
	 * disabled there. There is no race to increment 'active'
	 * counter. It protects free_llist from corruption in case NMI
	 * bpf prog preempted this loop.
	 */
	WARN_ON_ONCE(local_inc_return(&c->active) != 1);
}

static void dec_active(struct bpf_mem_cache *c, unsigned long *flags)
{
	local_dec(&c->active);
	if (IS_ENABLED(CONFIG_PREEMPT_RT))
		local_irq_restore(*flags);
}

static void add_obj_to_free_list(struct bpf_mem_cache *c, void *obj)
{
	unsigned long flags;

	inc_active(c, &flags);
	__llist_add(obj, &c->free_llist);
	c->free_cnt++;
	dec_active(c, &flags);
}

/* Mostly runs from irq_work except __init phase. */
static void alloc_bulk(struct bpf_mem_cache *c, int cnt, int node, bool atomic)
{
	struct mem_cgroup *memcg = NULL, *old_memcg;
	gfp_t gfp;
	void *obj;
	int i;

	gfp = __GFP_NOWARN | __GFP_ACCOUNT;
	gfp |= atomic ? GFP_NOWAIT : GFP_KERNEL;

	for (i = 0; i < cnt; i++) {
		/*
		 * For every 'c' llist_del_first(&c->free_by_rcu_ttrace); is
		 * done only by one CPU == current CPU. Other CPUs might
		 * llist_add() and llist_del_all() in parallel.
		 */
		obj = llist_del_first(&c->free_by_rcu_ttrace);
		if (!obj)
			break;
		add_obj_to_free_list(c, obj);
	}
	if (i >= cnt)
		return;

	for (; i < cnt; i++) {
		obj = llist_del_first(&c->waiting_for_gp_ttrace);
		if (!obj)
			break;
		add_obj_to_free_list(c, obj);
	}
	if (i >= cnt)
		return;

	memcg = get_memcg(c);
	old_memcg = set_active_memcg(memcg);
	for (; i < cnt; i++) {
		/* Allocate, but don't deplete atomic reserves that typical
		 * GFP_ATOMIC would do. irq_work runs on this cpu and kmalloc
		 * will allocate from the current numa node which is what we
		 * want here.
		 */
		obj = __alloc(c, node, gfp);
		if (!obj)
			break;
		add_obj_to_free_list(c, obj);
	}
	set_active_memcg(old_memcg);
	mem_cgroup_put(memcg);
}

static void free_one(void *obj, bool percpu)
{
	if (percpu) {
		free_percpu(((void **)obj)[1]);
		kfree(obj);
		return;
	}

	kfree(obj);
}

static int free_all(struct llist_node *llnode, bool percpu)
{
	struct llist_node *pos, *t;
	int cnt = 0;

	llist_for_each_safe(pos, t, llnode) {
		free_one(pos, percpu);
		cnt++;
	}
	return cnt;
}

static void __free_rcu(struct rcu_head *head)
{
	struct bpf_mem_cache *c = container_of(head, struct bpf_mem_cache, rcu_ttrace);

	free_all(llist_del_all(&c->waiting_for_gp_ttrace), !!c->percpu_size);
	atomic_set(&c->call_rcu_ttrace_in_progress, 0);
}

static void __free_rcu_tasks_trace(struct rcu_head *head)
{
	/* If RCU Tasks Trace grace period implies RCU grace period,
	 * there is no need to invoke call_rcu().
	 */
	if (rcu_trace_implies_rcu_gp())
		__free_rcu(head);
	else
		call_rcu(head, __free_rcu);
}

static void enque_to_free(struct bpf_mem_cache *c, void *obj)
{
	struct llist_node *llnode = obj;

	/* bpf_mem_cache is a per-cpu object. Freeing happens in irq_work.
	 * Nothing races to add to free_by_rcu_ttrace list.
	 */
	llist_add(llnode, &c->free_by_rcu_ttrace);
}

static void do_call_rcu_ttrace(struct bpf_mem_cache *c)
{
	struct llist_node *llnode, *t;

	if (atomic_xchg(&c->call_rcu_ttrace_in_progress, 1)) {
		if (unlikely(READ_ONCE(c->draining))) {
			llnode = llist_del_all(&c->free_by_rcu_ttrace);
			free_all(llnode, !!c->percpu_size);
		}
		return;
	}

	WARN_ON_ONCE(!llist_empty(&c->waiting_for_gp_ttrace));
	llist_for_each_safe(llnode, t, llist_del_all(&c->free_by_rcu_ttrace))
		llist_add(llnode, &c->waiting_for_gp_ttrace);

	if (unlikely(READ_ONCE(c->draining))) {
		__free_rcu(&c->rcu_ttrace);
		return;
	}

	/* Use call_rcu_tasks_trace() to wait for sleepable progs to finish.
	 * If RCU Tasks Trace grace period implies RCU grace period, free
	 * these elements directly, else use call_rcu() to wait for normal
	 * progs to finish and finally do free_one() on each element.
	 */
	call_rcu_tasks_trace(&c->rcu_ttrace, __free_rcu_tasks_trace);
}

static void free_bulk(struct bpf_mem_cache *c)
{
	struct bpf_mem_cache *tgt = c->tgt;
	struct llist_node *llnode, *t;
	unsigned long flags;
	int cnt;

	WARN_ON_ONCE(tgt->unit_size != c->unit_size);

	do {
		inc_active(c, &flags);
		llnode = __llist_del_first(&c->free_llist);
		if (llnode)
			cnt = --c->free_cnt;
		else
			cnt = 0;
		dec_active(c, &flags);
		if (llnode)
			enque_to_free(tgt, llnode);
	} while (cnt > (c->high_watermark + c->low_watermark) / 2);

	/* and drain free_llist_extra */
	llist_for_each_safe(llnode, t, llist_del_all(&c->free_llist_extra))
		enque_to_free(tgt, llnode);
	do_call_rcu_ttrace(tgt);
}

static void __free_by_rcu(struct rcu_head *head)
{
	struct bpf_mem_cache *c = container_of(head, struct bpf_mem_cache, rcu);
	struct bpf_mem_cache *tgt = c->tgt;
	struct llist_node *llnode;

	llnode = llist_del_all(&c->waiting_for_gp);
	if (!llnode)
		goto out;

	llist_add_batch(llnode, c->waiting_for_gp_tail, &tgt->free_by_rcu_ttrace);

	/* Objects went through regular RCU GP. Send them to RCU tasks trace */
	do_call_rcu_ttrace(tgt);
out:
	atomic_set(&c->call_rcu_in_progress, 0);
}

static void check_free_by_rcu(struct bpf_mem_cache *c)
{
	struct llist_node *llnode, *t;
	unsigned long flags;

	/* drain free_llist_extra_rcu */
	if (unlikely(!llist_empty(&c->free_llist_extra_rcu))) {
		inc_active(c, &flags);
		llist_for_each_safe(llnode, t, llist_del_all(&c->free_llist_extra_rcu))
			if (__llist_add(llnode, &c->free_by_rcu))
				c->free_by_rcu_tail = llnode;
		dec_active(c, &flags);
	}

	if (llist_empty(&c->free_by_rcu))
		return;

	if (atomic_xchg(&c->call_rcu_in_progress, 1)) {
		/*
		 * Instead of kmalloc-ing new rcu_head and triggering 10k
		 * call_rcu() to hit rcutree.qhimark and force RCU to notice
		 * the overload just ask RCU to hurry up. There could be many
		 * objects in free_by_rcu list.
		 * This hint reduces memory consumption for an artificial
		 * benchmark from 2 Gbyte to 150 Mbyte.
		 */
		rcu_request_urgent_qs_task(current);
		return;
	}

	WARN_ON_ONCE(!llist_empty(&c->waiting_for_gp));

	inc_active(c, &flags);
	WRITE_ONCE(c->waiting_for_gp.first, __llist_del_all(&c->free_by_rcu));
	c->waiting_for_gp_tail = c->free_by_rcu_tail;
	dec_active(c, &flags);

	if (unlikely(READ_ONCE(c->draining))) {
		free_all(llist_del_all(&c->waiting_for_gp), !!c->percpu_size);
		atomic_set(&c->call_rcu_in_progress, 0);
	} else {
		call_rcu_hurry(&c->rcu, __free_by_rcu);
	}
}

static void bpf_mem_refill(struct irq_work *work)
{
	struct bpf_mem_cache *c = container_of(work, struct bpf_mem_cache, refill_work);
	int cnt;

	/* Racy access to free_cnt. It doesn't need to be 100% accurate */
	cnt = c->free_cnt;
	if (cnt < c->low_watermark)
		/* irq_work runs on this cpu and kmalloc will allocate
		 * from the current numa node which is what we want here.
		 */
		alloc_bulk(c, c->batch, NUMA_NO_NODE, true);
	else if (cnt > c->high_watermark)
		free_bulk(c);

	check_free_by_rcu(c);
}

static void notrace irq_work_raise(struct bpf_mem_cache *c)
{
	irq_work_queue(&c->refill_work);
}

/* For typical bpf map case that uses bpf_mem_cache_alloc and single bucket
 * the freelist cache will be elem_size * 64 (or less) on each cpu.
 *
 * For bpf programs that don't have statically known allocation sizes and
 * assuming (low_mark + high_mark) / 2 as an average number of elements per
 * bucket and all buckets are used the total amount of memory in freelists
 * on each cpu will be:
 * 64*16 + 64*32 + 64*64 + 64*96 + 64*128 + 64*196 + 64*256 + 32*512 + 16*1024 + 8*2048 + 4*4096
 * == ~ 116 Kbyte using below heuristic.
 * Initialized, but unused bpf allocator (not bpf map specific one) will
 * consume ~ 11 Kbyte per cpu.
 * Typical case will be between 11K and 116K closer to 11K.
 * bpf progs can and should share bpf_mem_cache when possible.
 */
static void init_refill_work(struct bpf_mem_cache *c)
{
	init_irq_work(&c->refill_work, bpf_mem_refill);
	if (c->unit_size <= 256) {
		c->low_watermark = 32;
		c->high_watermark = 96;
	} else {
		/* When page_size == 4k, order-0 cache will have low_mark == 2
		 * and high_mark == 6 with batch alloc of 3 individual pages at
		 * a time.
		 * 8k allocs and above low == 1, high == 3, batch == 1.
		 */
		c->low_watermark = max(32 * 256 / c->unit_size, 1);
		c->high_watermark = max(96 * 256 / c->unit_size, 3);
	}
	c->batch = max((c->high_watermark - c->low_watermark) / 4 * 3, 1);
}

static void prefill_mem_cache(struct bpf_mem_cache *c, int cpu)
{
	/* To avoid consuming memory assume that 1st run of bpf
	 * prog won't be doing more than 4 map_update_elem from
	 * irq disabled region
	 */
	alloc_bulk(c, c->unit_size <= 256 ? 4 : 1, cpu_to_node(cpu), false);
}

static int check_obj_size(struct bpf_mem_cache *c, unsigned int idx)
{
	struct llist_node *first;
	unsigned int obj_size;

	/* For per-cpu allocator, the size of free objects in free list doesn't
	 * match with unit_size and now there is no way to get the size of
	 * per-cpu pointer saved in free object, so just skip the checking.
	 */
	if (c->percpu_size)
		return 0;

	first = c->free_llist.first;
	if (!first)
		return 0;

	obj_size = ksize(first);
	if (obj_size != c->unit_size) {
		WARN_ONCE(1, "bpf_mem_cache[%u]: unexpected object size %u, expect %u\n",
			  idx, obj_size, c->unit_size);
		return -EINVAL;
	}
	return 0;
}

/* When size != 0 bpf_mem_cache for each cpu.
 * This is typical bpf hash map use case when all elements have equal size.
 *
 * When size == 0 allocate 11 bpf_mem_cache-s for each cpu, then rely on
 * kmalloc/kfree. Max allocation size is 4096 in this case.
 * This is bpf_dynptr and bpf_kptr use case.
 */
int bpf_mem_alloc_init(struct bpf_mem_alloc *ma, int size, bool percpu)
{
	static u16 sizes[NUM_CACHES] = {96, 192, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
	int cpu, i, err, unit_size, percpu_size = 0;
	struct bpf_mem_caches *cc, __percpu *pcc;
	struct bpf_mem_cache *c, __percpu *pc;
	struct obj_cgroup *objcg = NULL;

	if (size) {
		pc = __alloc_percpu_gfp(sizeof(*pc), 8, GFP_KERNEL);
		if (!pc)
			return -ENOMEM;

		if (percpu)
			/* room for llist_node and per-cpu pointer */
			percpu_size = LLIST_NODE_SZ + sizeof(void *);
		else
			size += LLIST_NODE_SZ; /* room for llist_node */
		unit_size = size;

#ifdef CONFIG_MEMCG_KMEM
		if (memcg_bpf_enabled())
			objcg = get_obj_cgroup_from_current();
#endif
		for_each_possible_cpu(cpu) {
			c = per_cpu_ptr(pc, cpu);
			c->unit_size = unit_size;
			c->objcg = objcg;
			c->percpu_size = percpu_size;
			c->tgt = c;
			init_refill_work(c);
			prefill_mem_cache(c, cpu);
		}
		ma->cache = pc;
		return 0;
	}

	/* size == 0 && percpu is an invalid combination */
	if (WARN_ON_ONCE(percpu))
		return -EINVAL;

	pcc = __alloc_percpu_gfp(sizeof(*cc), 8, GFP_KERNEL);
	if (!pcc)
		return -ENOMEM;
	err = 0;
#ifdef CONFIG_MEMCG_KMEM
	objcg = get_obj_cgroup_from_current();
#endif
	for_each_possible_cpu(cpu) {
		cc = per_cpu_ptr(pcc, cpu);
		for (i = 0; i < NUM_CACHES; i++) {
			c = &cc->cache[i];
			c->unit_size = sizes[i];
			c->objcg = objcg;
			c->tgt = c;

			init_refill_work(c);
			/* Another bpf_mem_cache will be used when allocating
			 * c->unit_size in bpf_mem_alloc(), so doesn't prefill
			 * for the bpf_mem_cache because these free objects will
			 * never be used.
			 */
			if (i != bpf_mem_cache_idx(c->unit_size))
				continue;
			prefill_mem_cache(c, cpu);
			err = check_obj_size(c, i);
			if (err)
				goto out;
		}
	}

out:
	ma->caches = pcc;
	/* refill_work is either zeroed or initialized, so it is safe to
	 * call irq_work_sync().
	 */
	if (err)
		bpf_mem_alloc_destroy(ma);
	return err;
}

static void drain_mem_cache(struct bpf_mem_cache *c)
{
	bool percpu = !!c->percpu_size;

	/* No progs are using this bpf_mem_cache, but htab_map_free() called
	 * bpf_mem_cache_free() for all remaining elements and they can be in
	 * free_by_rcu_ttrace or in waiting_for_gp_ttrace lists, so drain those lists now.
	 *
	 * Except for waiting_for_gp_ttrace list, there are no concurrent operations
	 * on these lists, so it is safe to use __llist_del_all().
	 */
	free_all(llist_del_all(&c->free_by_rcu_ttrace), percpu);
	free_all(llist_del_all(&c->waiting_for_gp_ttrace), percpu);
	free_all(__llist_del_all(&c->free_llist), percpu);
	free_all(__llist_del_all(&c->free_llist_extra), percpu);
	free_all(__llist_del_all(&c->free_by_rcu), percpu);
	free_all(__llist_del_all(&c->free_llist_extra_rcu), percpu);
	free_all(llist_del_all(&c->waiting_for_gp), percpu);
}

static void check_mem_cache(struct bpf_mem_cache *c)
{
	WARN_ON_ONCE(!llist_empty(&c->free_by_rcu_ttrace));
	WARN_ON_ONCE(!llist_empty(&c->waiting_for_gp_ttrace));
	WARN_ON_ONCE(!llist_empty(&c->free_llist));
	WARN_ON_ONCE(!llist_empty(&c->free_llist_extra));
	WARN_ON_ONCE(!llist_empty(&c->free_by_rcu));
	WARN_ON_ONCE(!llist_empty(&c->free_llist_extra_rcu));
	WARN_ON_ONCE(!llist_empty(&c->waiting_for_gp));
}

static void check_leaked_objs(struct bpf_mem_alloc *ma)
{
	struct bpf_mem_caches *cc;
	struct bpf_mem_cache *c;
	int cpu, i;

	if (ma->cache) {
		for_each_possible_cpu(cpu) {
			c = per_cpu_ptr(ma->cache, cpu);
			check_mem_cache(c);
		}
	}
	if (ma->caches) {
		for_each_possible_cpu(cpu) {
			cc = per_cpu_ptr(ma->caches, cpu);
			for (i = 0; i < NUM_CACHES; i++) {
				c = &cc->cache[i];
				check_mem_cache(c);
			}
		}
	}
}

static void free_mem_alloc_no_barrier(struct bpf_mem_alloc *ma)
{
	check_leaked_objs(ma);
	free_percpu(ma->cache);
	free_percpu(ma->caches);
	ma->cache = NULL;
	ma->caches = NULL;
}

static void free_mem_alloc(struct bpf_mem_alloc *ma)
{
	/* waiting_for_gp[_ttrace] lists were drained, but RCU callbacks
	 * might still execute. Wait for them.
	 *
	 * rcu_barrier_tasks_trace() doesn't imply synchronize_rcu_tasks_trace(),
	 * but rcu_barrier_tasks_trace() and rcu_barrier() below are only used
	 * to wait for the pending __free_rcu_tasks_trace() and __free_rcu(),
	 * so if call_rcu(head, __free_rcu) is skipped due to
	 * rcu_trace_implies_rcu_gp(), it will be OK to skip rcu_barrier() by
	 * using rcu_trace_implies_rcu_gp() as well.
	 */
	rcu_barrier(); /* wait for __free_by_rcu */
	rcu_barrier_tasks_trace(); /* wait for __free_rcu */
	if (!rcu_trace_implies_rcu_gp())
		rcu_barrier();
	free_mem_alloc_no_barrier(ma);
}

static void free_mem_alloc_deferred(struct work_struct *work)
{
	struct bpf_mem_alloc *ma = container_of(work, struct bpf_mem_alloc, work);

	free_mem_alloc(ma);
	kfree(ma);
}

static void destroy_mem_alloc(struct bpf_mem_alloc *ma, int rcu_in_progress)
{
	struct bpf_mem_alloc *copy;

	if (!rcu_in_progress) {
		/* Fast path. No callbacks are pending, hence no need to do
		 * rcu_barrier-s.
		 */
		free_mem_alloc_no_barrier(ma);
		return;
	}

	copy = kmemdup(ma, sizeof(*ma), GFP_KERNEL);
	if (!copy) {
		/* Slow path with inline barrier-s */
		free_mem_alloc(ma);
		return;
	}

	/* Defer barriers into worker to let the rest of map memory to be freed */
	memset(ma, 0, sizeof(*ma));
	INIT_WORK(&copy->work, free_mem_alloc_deferred);
	queue_work(system_unbound_wq, &copy->work);
}

void bpf_mem_alloc_destroy(struct bpf_mem_alloc *ma)
{
	struct bpf_mem_caches *cc;
	struct bpf_mem_cache *c;
	int cpu, i, rcu_in_progress;

	if (ma->cache) {
		rcu_in_progress = 0;
		for_each_possible_cpu(cpu) {
			c = per_cpu_ptr(ma->cache, cpu);
			WRITE_ONCE(c->draining, true);
			irq_work_sync(&c->refill_work);
			drain_mem_cache(c);
			rcu_in_progress += atomic_read(&c->call_rcu_ttrace_in_progress);
			rcu_in_progress += atomic_read(&c->call_rcu_in_progress);
		}
		/* objcg is the same across cpus */
		if (c->objcg)
			obj_cgroup_put(c->objcg);
		destroy_mem_alloc(ma, rcu_in_progress);
	}
	if (ma->caches) {
		rcu_in_progress = 0;
		for_each_possible_cpu(cpu) {
			cc = per_cpu_ptr(ma->caches, cpu);
			for (i = 0; i < NUM_CACHES; i++) {
				c = &cc->cache[i];
				WRITE_ONCE(c->draining, true);
				irq_work_sync(&c->refill_work);
				drain_mem_cache(c);
				rcu_in_progress += atomic_read(&c->call_rcu_ttrace_in_progress);
				rcu_in_progress += atomic_read(&c->call_rcu_in_progress);
			}
		}
		if (c->objcg)
			obj_cgroup_put(c->objcg);
		destroy_mem_alloc(ma, rcu_in_progress);
	}
}

/* notrace is necessary here and in other functions to make sure
 * bpf programs cannot attach to them and cause llist corruptions.
 */
static void notrace *unit_alloc(struct bpf_mem_cache *c)
{
	struct llist_node *llnode = NULL;
	unsigned long flags;
	int cnt = 0;

	/* Disable irqs to prevent the following race for majority of prog types:
	 * prog_A
	 *   bpf_mem_alloc
	 *      preemption or irq -> prog_B
	 *        bpf_mem_alloc
	 *
	 * but prog_B could be a perf_event NMI prog.
	 * Use per-cpu 'active' counter to order free_list access between
	 * unit_alloc/unit_free/bpf_mem_refill.
	 */
	local_irq_save(flags);
	if (local_inc_return(&c->active) == 1) {
		llnode = __llist_del_first(&c->free_llist);
		if (llnode) {
			cnt = --c->free_cnt;
			*(struct bpf_mem_cache **)llnode = c;
		}
	}
	local_dec(&c->active);
	local_irq_restore(flags);

	WARN_ON(cnt < 0);

	if (cnt < c->low_watermark)
		irq_work_raise(c);
	return llnode;
}

/* Though 'ptr' object could have been allocated on a different cpu
 * add it to the free_llist of the current cpu.
 * Let kfree() logic deal with it when it's later called from irq_work.
 */
static void notrace unit_free(struct bpf_mem_cache *c, void *ptr)
{
	struct llist_node *llnode = ptr - LLIST_NODE_SZ;
	unsigned long flags;
	int cnt = 0;

	BUILD_BUG_ON(LLIST_NODE_SZ > 8);

	/*
	 * Remember bpf_mem_cache that allocated this object.
	 * The hint is not accurate.
	 */
	c->tgt = *(struct bpf_mem_cache **)llnode;

	local_irq_save(flags);
	if (local_inc_return(&c->active) == 1) {
		__llist_add(llnode, &c->free_llist);
		cnt = ++c->free_cnt;
	} else {
		/* unit_free() cannot fail. Therefore add an object to atomic
		 * llist. free_bulk() will drain it. Though free_llist_extra is
		 * a per-cpu list we have to use atomic llist_add here, since
		 * it also can be interrupted by bpf nmi prog that does another
		 * unit_free() into the same free_llist_extra.
		 */
		llist_add(llnode, &c->free_llist_extra);
	}
	local_dec(&c->active);
	local_irq_restore(flags);

	if (cnt > c->high_watermark)
		/* free few objects from current cpu into global kmalloc pool */
		irq_work_raise(c);
}

static void notrace unit_free_rcu(struct bpf_mem_cache *c, void *ptr)
{
	struct llist_node *llnode = ptr - LLIST_NODE_SZ;
	unsigned long flags;

	c->tgt = *(struct bpf_mem_cache **)llnode;

	local_irq_save(flags);
	if (local_inc_return(&c->active) == 1) {
		if (__llist_add(llnode, &c->free_by_rcu))
			c->free_by_rcu_tail = llnode;
	} else {
		llist_add(llnode, &c->free_llist_extra_rcu);
	}
	local_dec(&c->active);
	local_irq_restore(flags);

	if (!atomic_read(&c->call_rcu_in_progress))
		irq_work_raise(c);
}

/* Called from BPF program or from sys_bpf syscall.
 * In both cases migration is disabled.
 */
void notrace *bpf_mem_alloc(struct bpf_mem_alloc *ma, size_t size)
{
	int idx;
	void *ret;

	if (!size)
		return ZERO_SIZE_PTR;

	idx = bpf_mem_cache_idx(size + LLIST_NODE_SZ);
	if (idx < 0)
		return NULL;

	ret = unit_alloc(this_cpu_ptr(ma->caches)->cache + idx);
	return !ret ? NULL : ret + LLIST_NODE_SZ;
}

void notrace bpf_mem_free(struct bpf_mem_alloc *ma, void *ptr)
{
	int idx;

	if (!ptr)
		return;

	idx = bpf_mem_cache_idx(ksize(ptr - LLIST_NODE_SZ));
	if (idx < 0)
		return;

	unit_free(this_cpu_ptr(ma->caches)->cache + idx, ptr);
}

void notrace bpf_mem_free_rcu(struct bpf_mem_alloc *ma, void *ptr)
{
	int idx;

	if (!ptr)
		return;

	idx = bpf_mem_cache_idx(ksize(ptr - LLIST_NODE_SZ));
	if (idx < 0)
		return;

	unit_free_rcu(this_cpu_ptr(ma->caches)->cache + idx, ptr);
}

void notrace *bpf_mem_cache_alloc(struct bpf_mem_alloc *ma)
{
	void *ret;

	ret = unit_alloc(this_cpu_ptr(ma->cache));
	return !ret ? NULL : ret + LLIST_NODE_SZ;
}

void notrace bpf_mem_cache_free(struct bpf_mem_alloc *ma, void *ptr)
{
	if (!ptr)
		return;

	unit_free(this_cpu_ptr(ma->cache), ptr);
}

void notrace bpf_mem_cache_free_rcu(struct bpf_mem_alloc *ma, void *ptr)
{
	if (!ptr)
		return;

	unit_free_rcu(this_cpu_ptr(ma->cache), ptr);
}

/* Directly does a kfree() without putting 'ptr' back to the free_llist
 * for reuse and without waiting for a rcu_tasks_trace gp.
 * The caller must first go through the rcu_tasks_trace gp for 'ptr'
 * before calling bpf_mem_cache_raw_free().
 * It could be used when the rcu_tasks_trace callback does not have
 * a hold on the original bpf_mem_alloc object that allocated the
 * 'ptr'. This should only be used in the uncommon code path.
 * Otherwise, the bpf_mem_alloc's free_llist cannot be refilled
 * and may affect performance.
 */
void bpf_mem_cache_raw_free(void *ptr)
{
	if (!ptr)
		return;

	kfree(ptr - LLIST_NODE_SZ);
}

/* When flags == GFP_KERNEL, it signals that the caller will not cause
 * deadlock when using kmalloc. bpf_mem_cache_alloc_flags() will use
 * kmalloc if the free_llist is empty.
 */
void notrace *bpf_mem_cache_alloc_flags(struct bpf_mem_alloc *ma, gfp_t flags)
{
	struct bpf_mem_cache *c;
	void *ret;

	c = this_cpu_ptr(ma->cache);

	ret = unit_alloc(c);
	if (!ret && flags == GFP_KERNEL) {
		struct mem_cgroup *memcg, *old_memcg;

		memcg = get_memcg(c);
		old_memcg = set_active_memcg(memcg);
		ret = __alloc(c, NUMA_NO_NODE, GFP_KERNEL | __GFP_NOWARN | __GFP_ACCOUNT);
		set_active_memcg(old_memcg);
		mem_cgroup_put(memcg);
	}

	return !ret ? NULL : ret + LLIST_NODE_SZ;
}

/* Most of the logic is taken from setup_kmalloc_cache_index_table() */
static __init int bpf_mem_cache_adjust_size(void)
{
	unsigned int size, index;

	/* Normally KMALLOC_MIN_SIZE is 8-bytes, but it can be
	 * up-to 256-bytes.
	 */
	size = KMALLOC_MIN_SIZE;
	if (size <= 192)
		index = size_index[(size - 1) / 8];
	else
		index = fls(size - 1) - 1;
	for (size = 8; size < KMALLOC_MIN_SIZE && size <= 192; size += 8)
		size_index[(size - 1) / 8] = index;

	/* The minimal alignment is 64-bytes, so disable 96-bytes cache and
	 * use 128-bytes cache instead.
	 */
	if (KMALLOC_MIN_SIZE >= 64) {
		index = size_index[(128 - 1) / 8];
		for (size = 64 + 8; size <= 96; size += 8)
			size_index[(size - 1) / 8] = index;
	}

	/* The minimal alignment is 128-bytes, so disable 192-bytes cache and
	 * use 256-bytes cache instead.
	 */
	if (KMALLOC_MIN_SIZE >= 128) {
		index = fls(256 - 1) - 1;
		for (size = 128 + 8; size <= 192; size += 8)
			size_index[(size - 1) / 8] = index;
	}

	return 0;
}
subsys_initcall(bpf_mem_cache_adjust_size);
