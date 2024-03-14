// SPDX-License-Identifier: GPL-2.0
/*
 * RISC-V performance counter support.
 *
 * Copyright (C) 2021 Western Digital Corporation or its affiliates.
 *
 * This implementation is based on old RISC-V perf and ARM perf event code
 * which are in turn based on sparc64 and x86 code.
 */

#include <linux/cpumask.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/perf/riscv_pmu.h>
#include <linux/printk.h>
#include <linux/smp.h>
#include <linux/sched_clock.h>

#include <asm/sbi.h>

static bool riscv_perf_user_access(struct perf_event *event)
{
	return ((event->attr.type == PERF_TYPE_HARDWARE) ||
		(event->attr.type == PERF_TYPE_HW_CACHE) ||
		(event->attr.type == PERF_TYPE_RAW)) &&
		!!(event->hw.flags & PERF_EVENT_FLAG_USER_READ_CNT);
}

void arch_perf_update_userpage(struct perf_event *event,
			       struct perf_event_mmap_page *userpg, u64 now)
{
	struct clock_read_data *rd;
	unsigned int seq;
	u64 ns;

	userpg->cap_user_time = 0;
	userpg->cap_user_time_zero = 0;
	userpg->cap_user_time_short = 0;
	userpg->cap_user_rdpmc = riscv_perf_user_access(event);

#ifdef CONFIG_RISCV_PMU
	/*
	 * The counters are 64-bit but the priv spec doesn't mandate all the
	 * bits to be implemented: that's why, counter width can vary based on
	 * the cpu vendor.
	 */
	if (userpg->cap_user_rdpmc)
		userpg->pmc_width = to_riscv_pmu(event->pmu)->ctr_get_width(event->hw.idx) + 1;
#endif

	do {
		rd = sched_clock_read_begin(&seq);

		userpg->time_mult = rd->mult;
		userpg->time_shift = rd->shift;
		userpg->time_zero = rd->epoch_ns;
		userpg->time_cycles = rd->epoch_cyc;
		userpg->time_mask = rd->sched_clock_mask;

		/*
		 * Subtract the cycle base, such that software that
		 * doesn't know about cap_user_time_short still 'works'
		 * assuming no wraps.
		 */
		ns = mul_u64_u32_shr(rd->epoch_cyc, rd->mult, rd->shift);
		userpg->time_zero -= ns;

	} while (sched_clock_read_retry(seq));

	userpg->time_offset = userpg->time_zero - now;

	/*
	 * time_shift is not expected to be greater than 31 due to
	 * the original published conversion algorithm shifting a
	 * 32-bit value (now specifies a 64-bit value) - refer
	 * perf_event_mmap_page documentation in perf_event.h.
	 */
	if (userpg->time_shift == 32) {
		userpg->time_shift = 31;
		userpg->time_mult >>= 1;
	}

	/*
	 * Internal timekeeping for enabled/running/stopped times
	 * is always computed with the sched_clock.
	 */
	userpg->cap_user_time = 1;
	userpg->cap_user_time_zero = 1;
	userpg->cap_user_time_short = 1;
}

static unsigned long csr_read_num(int csr_num)
{
#define switchcase_csr_read(__csr_num, __val)		{\
	case __csr_num:					\
		__val = csr_read(__csr_num);		\
		break; }
#define switchcase_csr_read_2(__csr_num, __val)		{\
	switchcase_csr_read(__csr_num + 0, __val)	 \
	switchcase_csr_read(__csr_num + 1, __val)}
#define switchcase_csr_read_4(__csr_num, __val)		{\
	switchcase_csr_read_2(__csr_num + 0, __val)	 \
	switchcase_csr_read_2(__csr_num + 2, __val)}
#define switchcase_csr_read_8(__csr_num, __val)		{\
	switchcase_csr_read_4(__csr_num + 0, __val)	 \
	switchcase_csr_read_4(__csr_num + 4, __val)}
#define switchcase_csr_read_16(__csr_num, __val)	{\
	switchcase_csr_read_8(__csr_num + 0, __val)	 \
	switchcase_csr_read_8(__csr_num + 8, __val)}
#define switchcase_csr_read_32(__csr_num, __val)	{\
	switchcase_csr_read_16(__csr_num + 0, __val)	 \
	switchcase_csr_read_16(__csr_num + 16, __val)}

	unsigned long ret = 0;

	switch (csr_num) {
	switchcase_csr_read_32(CSR_CYCLE, ret)
	switchcase_csr_read_32(CSR_CYCLEH, ret)
	default :
		break;
	}

	return ret;
#undef switchcase_csr_read_32
#undef switchcase_csr_read_16
#undef switchcase_csr_read_8
#undef switchcase_csr_read_4
#undef switchcase_csr_read_2
#undef switchcase_csr_read
}

/*
 * Read the CSR of a corresponding counter.
 */
unsigned long riscv_pmu_ctr_read_csr(unsigned long csr)
{
	if (csr < CSR_CYCLE || csr > CSR_HPMCOUNTER31H ||
	   (csr > CSR_HPMCOUNTER31 && csr < CSR_CYCLEH)) {
		pr_err("Invalid performance counter csr %lx\n", csr);
		return -EINVAL;
	}

	return csr_read_num(csr);
}

u64 riscv_pmu_ctr_get_width_mask(struct perf_event *event)
{
	int cwidth;
	struct riscv_pmu *rvpmu = to_riscv_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	if (!rvpmu->ctr_get_width)
	/**
	 * If the pmu driver doesn't support counter width, set it to default
	 * maximum allowed by the specification.
	 */
		cwidth = 63;
	else {
		if (hwc->idx == -1)
			/* Handle init case where idx is not initialized yet */
			cwidth = rvpmu->ctr_get_width(0);
		else
			cwidth = rvpmu->ctr_get_width(hwc->idx);
	}

	return GENMASK_ULL(cwidth, 0);
}

u64 riscv_pmu_event_update(struct perf_event *event)
{
	struct riscv_pmu *rvpmu = to_riscv_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	u64 prev_raw_count, new_raw_count;
	unsigned long cmask;
	u64 oldval, delta;

	if (!rvpmu->ctr_read)
		return 0;

	cmask = riscv_pmu_ctr_get_width_mask(event);

	do {
		prev_raw_count = local64_read(&hwc->prev_count);
		new_raw_count = rvpmu->ctr_read(event);
		oldval = local64_cmpxchg(&hwc->prev_count, prev_raw_count,
					 new_raw_count);
	} while (oldval != prev_raw_count);

	delta = (new_raw_count - prev_raw_count) & cmask;
	local64_add(delta, &event->count);
	local64_sub(delta, &hwc->period_left);

	return delta;
}

void riscv_pmu_stop(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	struct riscv_pmu *rvpmu = to_riscv_pmu(event->pmu);

	WARN_ON_ONCE(hwc->state & PERF_HES_STOPPED);

	if (!(hwc->state & PERF_HES_STOPPED)) {
		if (rvpmu->ctr_stop) {
			rvpmu->ctr_stop(event, 0);
			hwc->state |= PERF_HES_STOPPED;
		}
		riscv_pmu_event_update(event);
		hwc->state |= PERF_HES_UPTODATE;
	}
}

int riscv_pmu_event_set_period(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	s64 left = local64_read(&hwc->period_left);
	s64 period = hwc->sample_period;
	int overflow = 0;
	uint64_t max_period = riscv_pmu_ctr_get_width_mask(event);

	if (unlikely(left <= -period)) {
		left = period;
		local64_set(&hwc->period_left, left);
		hwc->last_period = period;
		overflow = 1;
	}

	if (unlikely(left <= 0)) {
		left += period;
		local64_set(&hwc->period_left, left);
		hwc->last_period = period;
		overflow = 1;
	}

	/*
	 * Limit the maximum period to prevent the counter value
	 * from overtaking the one we are about to program. In
	 * effect we are reducing max_period to account for
	 * interrupt latency (and we are being very conservative).
	 */
	if (left > (max_period >> 1))
		left = (max_period >> 1);

	local64_set(&hwc->prev_count, (u64)-left);

	perf_event_update_userpage(event);

	return overflow;
}

void riscv_pmu_start(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	struct riscv_pmu *rvpmu = to_riscv_pmu(event->pmu);
	uint64_t max_period = riscv_pmu_ctr_get_width_mask(event);
	u64 init_val;

	if (flags & PERF_EF_RELOAD)
		WARN_ON_ONCE(!(event->hw.state & PERF_HES_UPTODATE));

	hwc->state = 0;
	riscv_pmu_event_set_period(event);
	init_val = local64_read(&hwc->prev_count) & max_period;
	rvpmu->ctr_start(event, init_val);
	perf_event_update_userpage(event);
}

static int riscv_pmu_add(struct perf_event *event, int flags)
{
	struct riscv_pmu *rvpmu = to_riscv_pmu(event->pmu);
	struct cpu_hw_events *cpuc = this_cpu_ptr(rvpmu->hw_events);
	struct hw_perf_event *hwc = &event->hw;
	int idx;

	idx = rvpmu->ctr_get_idx(event);
	if (idx < 0)
		return idx;

	hwc->idx = idx;
	cpuc->events[idx] = event;
	cpuc->n_events++;
	hwc->state = PERF_HES_UPTODATE | PERF_HES_STOPPED;
	if (flags & PERF_EF_START)
		riscv_pmu_start(event, PERF_EF_RELOAD);

	/* Propagate our changes to the userspace mapping. */
	perf_event_update_userpage(event);

	return 0;
}

static void riscv_pmu_del(struct perf_event *event, int flags)
{
	struct riscv_pmu *rvpmu = to_riscv_pmu(event->pmu);
	struct cpu_hw_events *cpuc = this_cpu_ptr(rvpmu->hw_events);
	struct hw_perf_event *hwc = &event->hw;

	riscv_pmu_stop(event, PERF_EF_UPDATE);
	cpuc->events[hwc->idx] = NULL;
	/* The firmware need to reset the counter mapping */
	if (rvpmu->ctr_stop)
		rvpmu->ctr_stop(event, RISCV_PMU_STOP_FLAG_RESET);
	cpuc->n_events--;
	if (rvpmu->ctr_clear_idx)
		rvpmu->ctr_clear_idx(event);
	perf_event_update_userpage(event);
	hwc->idx = -1;
}

static void riscv_pmu_read(struct perf_event *event)
{
	riscv_pmu_event_update(event);
}

static int riscv_pmu_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct riscv_pmu *rvpmu = to_riscv_pmu(event->pmu);
	int mapped_event;
	u64 event_config = 0;
	uint64_t cmask;

	hwc->flags = 0;
	mapped_event = rvpmu->event_map(event, &event_config);
	if (mapped_event < 0) {
		pr_debug("event %x:%llx not supported\n", event->attr.type,
			 event->attr.config);
		return mapped_event;
	}

	/*
	 * idx is set to -1 because the index of a general event should not be
	 * decided until binding to some counter in pmu->add().
	 * config will contain the information about counter CSR
	 * the idx will contain the counter index
	 */
	hwc->config = event_config;
	hwc->idx = -1;
	hwc->event_base = mapped_event;

	if (rvpmu->event_init)
		rvpmu->event_init(event);

	if (!is_sampling_event(event)) {
		/*
		 * For non-sampling runs, limit the sample_period to half
		 * of the counter width. That way, the new counter value
		 * is far less likely to overtake the previous one unless
		 * you have some serious IRQ latency issues.
		 */
		cmask = riscv_pmu_ctr_get_width_mask(event);
		hwc->sample_period  =  cmask >> 1;
		hwc->last_period    = hwc->sample_period;
		local64_set(&hwc->period_left, hwc->sample_period);
	}

	return 0;
}

static int riscv_pmu_event_idx(struct perf_event *event)
{
	struct riscv_pmu *rvpmu = to_riscv_pmu(event->pmu);

	if (!(event->hw.flags & PERF_EVENT_FLAG_USER_READ_CNT))
		return 0;

	if (rvpmu->csr_index)
		return rvpmu->csr_index(event) + 1;

	return 0;
}

static void riscv_pmu_event_mapped(struct perf_event *event, struct mm_struct *mm)
{
	struct riscv_pmu *rvpmu = to_riscv_pmu(event->pmu);

	if (rvpmu->event_mapped) {
		rvpmu->event_mapped(event, mm);
		perf_event_update_userpage(event);
	}
}

static void riscv_pmu_event_unmapped(struct perf_event *event, struct mm_struct *mm)
{
	struct riscv_pmu *rvpmu = to_riscv_pmu(event->pmu);

	if (rvpmu->event_unmapped) {
		rvpmu->event_unmapped(event, mm);
		perf_event_update_userpage(event);
	}
}

struct riscv_pmu *riscv_pmu_alloc(void)
{
	struct riscv_pmu *pmu;
	int cpuid, i;
	struct cpu_hw_events *cpuc;

	pmu = kzalloc(sizeof(*pmu), GFP_KERNEL);
	if (!pmu)
		goto out;

	pmu->hw_events = alloc_percpu_gfp(struct cpu_hw_events, GFP_KERNEL);
	if (!pmu->hw_events) {
		pr_info("failed to allocate per-cpu PMU data.\n");
		goto out_free_pmu;
	}

	for_each_possible_cpu(cpuid) {
		cpuc = per_cpu_ptr(pmu->hw_events, cpuid);
		cpuc->n_events = 0;
		for (i = 0; i < RISCV_MAX_COUNTERS; i++)
			cpuc->events[i] = NULL;
	}
	pmu->pmu = (struct pmu) {
		.event_init	= riscv_pmu_event_init,
		.event_mapped	= riscv_pmu_event_mapped,
		.event_unmapped	= riscv_pmu_event_unmapped,
		.event_idx	= riscv_pmu_event_idx,
		.add		= riscv_pmu_add,
		.del		= riscv_pmu_del,
		.start		= riscv_pmu_start,
		.stop		= riscv_pmu_stop,
		.read		= riscv_pmu_read,
	};

	return pmu;

out_free_pmu:
	kfree(pmu);
out:
	return NULL;
}
