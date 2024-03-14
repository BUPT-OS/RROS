// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel-based Virtual Machine -- Performance Monitoring Unit support
 *
 * Copyright 2015 Red Hat, Inc. and/or its affiliates.
 *
 * Authors:
 *   Avi Kivity   <avi@redhat.com>
 *   Gleb Natapov <gleb@redhat.com>
 *   Wei Huang    <wei@redhat.com>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/types.h>
#include <linux/kvm_host.h>
#include <linux/perf_event.h>
#include <linux/bsearch.h>
#include <linux/sort.h>
#include <asm/perf_event.h>
#include <asm/cpu_device_id.h>
#include "x86.h"
#include "cpuid.h"
#include "lapic.h"
#include "pmu.h"

/* This is enough to filter the vast majority of currently defined events. */
#define KVM_PMU_EVENT_FILTER_MAX_EVENTS 300

struct x86_pmu_capability __read_mostly kvm_pmu_cap;
EXPORT_SYMBOL_GPL(kvm_pmu_cap);

/* Precise Distribution of Instructions Retired (PDIR) */
static const struct x86_cpu_id vmx_pebs_pdir_cpu[] = {
	X86_MATCH_INTEL_FAM6_MODEL(ICELAKE_D, NULL),
	X86_MATCH_INTEL_FAM6_MODEL(ICELAKE_X, NULL),
	/* Instruction-Accurate PDIR (PDIR++) */
	X86_MATCH_INTEL_FAM6_MODEL(SAPPHIRERAPIDS_X, NULL),
	{}
};

/* Precise Distribution (PDist) */
static const struct x86_cpu_id vmx_pebs_pdist_cpu[] = {
	X86_MATCH_INTEL_FAM6_MODEL(SAPPHIRERAPIDS_X, NULL),
	{}
};

/* NOTE:
 * - Each perf counter is defined as "struct kvm_pmc";
 * - There are two types of perf counters: general purpose (gp) and fixed.
 *   gp counters are stored in gp_counters[] and fixed counters are stored
 *   in fixed_counters[] respectively. Both of them are part of "struct
 *   kvm_pmu";
 * - pmu.c understands the difference between gp counters and fixed counters.
 *   However AMD doesn't support fixed-counters;
 * - There are three types of index to access perf counters (PMC):
 *     1. MSR (named msr): For example Intel has MSR_IA32_PERFCTRn and AMD
 *        has MSR_K7_PERFCTRn and, for families 15H and later,
 *        MSR_F15H_PERF_CTRn, where MSR_F15H_PERF_CTR[0-3] are
 *        aliased to MSR_K7_PERFCTRn.
 *     2. MSR Index (named idx): This normally is used by RDPMC instruction.
 *        For instance AMD RDPMC instruction uses 0000_0003h in ECX to access
 *        C001_0007h (MSR_K7_PERCTR3). Intel has a similar mechanism, except
 *        that it also supports fixed counters. idx can be used to as index to
 *        gp and fixed counters.
 *     3. Global PMC Index (named pmc): pmc is an index specific to PMU
 *        code. Each pmc, stored in kvm_pmc.idx field, is unique across
 *        all perf counters (both gp and fixed). The mapping relationship
 *        between pmc and perf counters is as the following:
 *        * Intel: [0 .. KVM_INTEL_PMC_MAX_GENERIC-1] <=> gp counters
 *                 [INTEL_PMC_IDX_FIXED .. INTEL_PMC_IDX_FIXED + 2] <=> fixed
 *        * AMD:   [0 .. AMD64_NUM_COUNTERS-1] and, for families 15H
 *          and later, [0 .. AMD64_NUM_COUNTERS_CORE-1] <=> gp counters
 */

static struct kvm_pmu_ops kvm_pmu_ops __read_mostly;

#define KVM_X86_PMU_OP(func)					     \
	DEFINE_STATIC_CALL_NULL(kvm_x86_pmu_##func,			     \
				*(((struct kvm_pmu_ops *)0)->func));
#define KVM_X86_PMU_OP_OPTIONAL KVM_X86_PMU_OP
#include <asm/kvm-x86-pmu-ops.h>

void kvm_pmu_ops_update(const struct kvm_pmu_ops *pmu_ops)
{
	memcpy(&kvm_pmu_ops, pmu_ops, sizeof(kvm_pmu_ops));

#define __KVM_X86_PMU_OP(func) \
	static_call_update(kvm_x86_pmu_##func, kvm_pmu_ops.func);
#define KVM_X86_PMU_OP(func) \
	WARN_ON(!kvm_pmu_ops.func); __KVM_X86_PMU_OP(func)
#define KVM_X86_PMU_OP_OPTIONAL __KVM_X86_PMU_OP
#include <asm/kvm-x86-pmu-ops.h>
#undef __KVM_X86_PMU_OP
}

static void kvm_pmi_trigger_fn(struct irq_work *irq_work)
{
	struct kvm_pmu *pmu = container_of(irq_work, struct kvm_pmu, irq_work);
	struct kvm_vcpu *vcpu = pmu_to_vcpu(pmu);

	kvm_pmu_deliver_pmi(vcpu);
}

static inline void __kvm_perf_overflow(struct kvm_pmc *pmc, bool in_pmi)
{
	struct kvm_pmu *pmu = pmc_to_pmu(pmc);
	bool skip_pmi = false;

	if (pmc->perf_event && pmc->perf_event->attr.precise_ip) {
		if (!in_pmi) {
			/*
			 * TODO: KVM is currently _choosing_ to not generate records
			 * for emulated instructions, avoiding BUFFER_OVF PMI when
			 * there are no records. Strictly speaking, it should be done
			 * as well in the right context to improve sampling accuracy.
			 */
			skip_pmi = true;
		} else {
			/* Indicate PEBS overflow PMI to guest. */
			skip_pmi = __test_and_set_bit(GLOBAL_STATUS_BUFFER_OVF_BIT,
						      (unsigned long *)&pmu->global_status);
		}
	} else {
		__set_bit(pmc->idx, (unsigned long *)&pmu->global_status);
	}

	if (!pmc->intr || skip_pmi)
		return;

	/*
	 * Inject PMI. If vcpu was in a guest mode during NMI PMI
	 * can be ejected on a guest mode re-entry. Otherwise we can't
	 * be sure that vcpu wasn't executing hlt instruction at the
	 * time of vmexit and is not going to re-enter guest mode until
	 * woken up. So we should wake it, but this is impossible from
	 * NMI context. Do it from irq work instead.
	 */
	if (in_pmi && !kvm_handling_nmi_from_guest(pmc->vcpu))
		irq_work_queue(&pmc_to_pmu(pmc)->irq_work);
	else
		kvm_make_request(KVM_REQ_PMI, pmc->vcpu);
}

static void kvm_perf_overflow(struct perf_event *perf_event,
			      struct perf_sample_data *data,
			      struct pt_regs *regs)
{
	struct kvm_pmc *pmc = perf_event->overflow_handler_context;

	/*
	 * Ignore overflow events for counters that are scheduled to be
	 * reprogrammed, e.g. if a PMI for the previous event races with KVM's
	 * handling of a related guest WRMSR.
	 */
	if (test_and_set_bit(pmc->idx, pmc_to_pmu(pmc)->reprogram_pmi))
		return;

	__kvm_perf_overflow(pmc, true);

	kvm_make_request(KVM_REQ_PMU, pmc->vcpu);
}

static u64 pmc_get_pebs_precise_level(struct kvm_pmc *pmc)
{
	/*
	 * For some model specific pebs counters with special capabilities
	 * (PDIR, PDIR++, PDIST), KVM needs to raise the event precise
	 * level to the maximum value (currently 3, backwards compatible)
	 * so that the perf subsystem would assign specific hardware counter
	 * with that capability for vPMC.
	 */
	if ((pmc->idx == 0 && x86_match_cpu(vmx_pebs_pdist_cpu)) ||
	    (pmc->idx == 32 && x86_match_cpu(vmx_pebs_pdir_cpu)))
		return 3;

	/*
	 * The non-zero precision level of guest event makes the ordinary
	 * guest event becomes a guest PEBS event and triggers the host
	 * PEBS PMI handler to determine whether the PEBS overflow PMI
	 * comes from the host counters or the guest.
	 */
	return 1;
}

static int pmc_reprogram_counter(struct kvm_pmc *pmc, u32 type, u64 config,
				 bool exclude_user, bool exclude_kernel,
				 bool intr)
{
	struct kvm_pmu *pmu = pmc_to_pmu(pmc);
	struct perf_event *event;
	struct perf_event_attr attr = {
		.type = type,
		.size = sizeof(attr),
		.pinned = true,
		.exclude_idle = true,
		.exclude_host = 1,
		.exclude_user = exclude_user,
		.exclude_kernel = exclude_kernel,
		.config = config,
	};
	bool pebs = test_bit(pmc->idx, (unsigned long *)&pmu->pebs_enable);

	attr.sample_period = get_sample_period(pmc, pmc->counter);

	if ((attr.config & HSW_IN_TX_CHECKPOINTED) &&
	    guest_cpuid_is_intel(pmc->vcpu)) {
		/*
		 * HSW_IN_TX_CHECKPOINTED is not supported with nonzero
		 * period. Just clear the sample period so at least
		 * allocating the counter doesn't fail.
		 */
		attr.sample_period = 0;
	}
	if (pebs) {
		/*
		 * For most PEBS hardware events, the difference in the software
		 * precision levels of guest and host PEBS events will not affect
		 * the accuracy of the PEBS profiling result, because the "event IP"
		 * in the PEBS record is calibrated on the guest side.
		 */
		attr.precise_ip = pmc_get_pebs_precise_level(pmc);
	}

	event = perf_event_create_kernel_counter(&attr, -1, current,
						 kvm_perf_overflow, pmc);
	if (IS_ERR(event)) {
		pr_debug_ratelimited("kvm_pmu: event creation failed %ld for pmc->idx = %d\n",
			    PTR_ERR(event), pmc->idx);
		return PTR_ERR(event);
	}

	pmc->perf_event = event;
	pmc_to_pmu(pmc)->event_count++;
	pmc->is_paused = false;
	pmc->intr = intr || pebs;
	return 0;
}

static void pmc_pause_counter(struct kvm_pmc *pmc)
{
	u64 counter = pmc->counter;

	if (!pmc->perf_event || pmc->is_paused)
		return;

	/* update counter, reset event value to avoid redundant accumulation */
	counter += perf_event_pause(pmc->perf_event, true);
	pmc->counter = counter & pmc_bitmask(pmc);
	pmc->is_paused = true;
}

static bool pmc_resume_counter(struct kvm_pmc *pmc)
{
	if (!pmc->perf_event)
		return false;

	/* recalibrate sample period and check if it's accepted by perf core */
	if (is_sampling_event(pmc->perf_event) &&
	    perf_event_period(pmc->perf_event,
			      get_sample_period(pmc, pmc->counter)))
		return false;

	if (test_bit(pmc->idx, (unsigned long *)&pmc_to_pmu(pmc)->pebs_enable) !=
	    (!!pmc->perf_event->attr.precise_ip))
		return false;

	/* reuse perf_event to serve as pmc_reprogram_counter() does*/
	perf_event_enable(pmc->perf_event);
	pmc->is_paused = false;

	return true;
}

static int filter_cmp(const void *pa, const void *pb, u64 mask)
{
	u64 a = *(u64 *)pa & mask;
	u64 b = *(u64 *)pb & mask;

	return (a > b) - (a < b);
}


static int filter_sort_cmp(const void *pa, const void *pb)
{
	return filter_cmp(pa, pb, (KVM_PMU_MASKED_ENTRY_EVENT_SELECT |
				   KVM_PMU_MASKED_ENTRY_EXCLUDE));
}

/*
 * For the event filter, searching is done on the 'includes' list and
 * 'excludes' list separately rather than on the 'events' list (which
 * has both).  As a result the exclude bit can be ignored.
 */
static int filter_event_cmp(const void *pa, const void *pb)
{
	return filter_cmp(pa, pb, (KVM_PMU_MASKED_ENTRY_EVENT_SELECT));
}

static int find_filter_index(u64 *events, u64 nevents, u64 key)
{
	u64 *fe = bsearch(&key, events, nevents, sizeof(events[0]),
			  filter_event_cmp);

	if (!fe)
		return -1;

	return fe - events;
}

static bool is_filter_entry_match(u64 filter_event, u64 umask)
{
	u64 mask = filter_event >> (KVM_PMU_MASKED_ENTRY_UMASK_MASK_SHIFT - 8);
	u64 match = filter_event & KVM_PMU_MASKED_ENTRY_UMASK_MATCH;

	BUILD_BUG_ON((KVM_PMU_ENCODE_MASKED_ENTRY(0, 0xff, 0, false) >>
		     (KVM_PMU_MASKED_ENTRY_UMASK_MASK_SHIFT - 8)) !=
		     ARCH_PERFMON_EVENTSEL_UMASK);

	return (umask & mask) == match;
}

static bool filter_contains_match(u64 *events, u64 nevents, u64 eventsel)
{
	u64 event_select = eventsel & kvm_pmu_ops.EVENTSEL_EVENT;
	u64 umask = eventsel & ARCH_PERFMON_EVENTSEL_UMASK;
	int i, index;

	index = find_filter_index(events, nevents, event_select);
	if (index < 0)
		return false;

	/*
	 * Entries are sorted by the event select.  Walk the list in both
	 * directions to process all entries with the targeted event select.
	 */
	for (i = index; i < nevents; i++) {
		if (filter_event_cmp(&events[i], &event_select))
			break;

		if (is_filter_entry_match(events[i], umask))
			return true;
	}

	for (i = index - 1; i >= 0; i--) {
		if (filter_event_cmp(&events[i], &event_select))
			break;

		if (is_filter_entry_match(events[i], umask))
			return true;
	}

	return false;
}

static bool is_gp_event_allowed(struct kvm_x86_pmu_event_filter *f,
				u64 eventsel)
{
	if (filter_contains_match(f->includes, f->nr_includes, eventsel) &&
	    !filter_contains_match(f->excludes, f->nr_excludes, eventsel))
		return f->action == KVM_PMU_EVENT_ALLOW;

	return f->action == KVM_PMU_EVENT_DENY;
}

static bool is_fixed_event_allowed(struct kvm_x86_pmu_event_filter *filter,
				   int idx)
{
	int fixed_idx = idx - INTEL_PMC_IDX_FIXED;

	if (filter->action == KVM_PMU_EVENT_DENY &&
	    test_bit(fixed_idx, (ulong *)&filter->fixed_counter_bitmap))
		return false;
	if (filter->action == KVM_PMU_EVENT_ALLOW &&
	    !test_bit(fixed_idx, (ulong *)&filter->fixed_counter_bitmap))
		return false;

	return true;
}

static bool check_pmu_event_filter(struct kvm_pmc *pmc)
{
	struct kvm_x86_pmu_event_filter *filter;
	struct kvm *kvm = pmc->vcpu->kvm;

	filter = srcu_dereference(kvm->arch.pmu_event_filter, &kvm->srcu);
	if (!filter)
		return true;

	if (pmc_is_gp(pmc))
		return is_gp_event_allowed(filter, pmc->eventsel);

	return is_fixed_event_allowed(filter, pmc->idx);
}

static bool pmc_event_is_allowed(struct kvm_pmc *pmc)
{
	return pmc_is_globally_enabled(pmc) && pmc_speculative_in_use(pmc) &&
	       static_call(kvm_x86_pmu_hw_event_available)(pmc) &&
	       check_pmu_event_filter(pmc);
}

static void reprogram_counter(struct kvm_pmc *pmc)
{
	struct kvm_pmu *pmu = pmc_to_pmu(pmc);
	u64 eventsel = pmc->eventsel;
	u64 new_config = eventsel;
	u8 fixed_ctr_ctrl;

	pmc_pause_counter(pmc);

	if (!pmc_event_is_allowed(pmc))
		goto reprogram_complete;

	if (pmc->counter < pmc->prev_counter)
		__kvm_perf_overflow(pmc, false);

	if (eventsel & ARCH_PERFMON_EVENTSEL_PIN_CONTROL)
		printk_once("kvm pmu: pin control bit is ignored\n");

	if (pmc_is_fixed(pmc)) {
		fixed_ctr_ctrl = fixed_ctrl_field(pmu->fixed_ctr_ctrl,
						  pmc->idx - INTEL_PMC_IDX_FIXED);
		if (fixed_ctr_ctrl & 0x1)
			eventsel |= ARCH_PERFMON_EVENTSEL_OS;
		if (fixed_ctr_ctrl & 0x2)
			eventsel |= ARCH_PERFMON_EVENTSEL_USR;
		if (fixed_ctr_ctrl & 0x8)
			eventsel |= ARCH_PERFMON_EVENTSEL_INT;
		new_config = (u64)fixed_ctr_ctrl;
	}

	if (pmc->current_config == new_config && pmc_resume_counter(pmc))
		goto reprogram_complete;

	pmc_release_perf_event(pmc);

	pmc->current_config = new_config;

	/*
	 * If reprogramming fails, e.g. due to contention, leave the counter's
	 * regprogram bit set, i.e. opportunistically try again on the next PMU
	 * refresh.  Don't make a new request as doing so can stall the guest
	 * if reprogramming repeatedly fails.
	 */
	if (pmc_reprogram_counter(pmc, PERF_TYPE_RAW,
				  (eventsel & pmu->raw_event_mask),
				  !(eventsel & ARCH_PERFMON_EVENTSEL_USR),
				  !(eventsel & ARCH_PERFMON_EVENTSEL_OS),
				  eventsel & ARCH_PERFMON_EVENTSEL_INT))
		return;

reprogram_complete:
	clear_bit(pmc->idx, (unsigned long *)&pmc_to_pmu(pmc)->reprogram_pmi);
	pmc->prev_counter = 0;
}

void kvm_pmu_handle_event(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	int bit;

	for_each_set_bit(bit, pmu->reprogram_pmi, X86_PMC_IDX_MAX) {
		struct kvm_pmc *pmc = static_call(kvm_x86_pmu_pmc_idx_to_pmc)(pmu, bit);

		if (unlikely(!pmc)) {
			clear_bit(bit, pmu->reprogram_pmi);
			continue;
		}

		reprogram_counter(pmc);
	}

	/*
	 * Unused perf_events are only released if the corresponding MSRs
	 * weren't accessed during the last vCPU time slice. kvm_arch_sched_in
	 * triggers KVM_REQ_PMU if cleanup is needed.
	 */
	if (unlikely(pmu->need_cleanup))
		kvm_pmu_cleanup(vcpu);
}

/* check if idx is a valid index to access PMU */
bool kvm_pmu_is_valid_rdpmc_ecx(struct kvm_vcpu *vcpu, unsigned int idx)
{
	return static_call(kvm_x86_pmu_is_valid_rdpmc_ecx)(vcpu, idx);
}

bool is_vmware_backdoor_pmc(u32 pmc_idx)
{
	switch (pmc_idx) {
	case VMWARE_BACKDOOR_PMC_HOST_TSC:
	case VMWARE_BACKDOOR_PMC_REAL_TIME:
	case VMWARE_BACKDOOR_PMC_APPARENT_TIME:
		return true;
	}
	return false;
}

static int kvm_pmu_rdpmc_vmware(struct kvm_vcpu *vcpu, unsigned idx, u64 *data)
{
	u64 ctr_val;

	switch (idx) {
	case VMWARE_BACKDOOR_PMC_HOST_TSC:
		ctr_val = rdtsc();
		break;
	case VMWARE_BACKDOOR_PMC_REAL_TIME:
		ctr_val = ktime_get_boottime_ns();
		break;
	case VMWARE_BACKDOOR_PMC_APPARENT_TIME:
		ctr_val = ktime_get_boottime_ns() +
			vcpu->kvm->arch.kvmclock_offset;
		break;
	default:
		return 1;
	}

	*data = ctr_val;
	return 0;
}

int kvm_pmu_rdpmc(struct kvm_vcpu *vcpu, unsigned idx, u64 *data)
{
	bool fast_mode = idx & (1u << 31);
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct kvm_pmc *pmc;
	u64 mask = fast_mode ? ~0u : ~0ull;

	if (!pmu->version)
		return 1;

	if (is_vmware_backdoor_pmc(idx))
		return kvm_pmu_rdpmc_vmware(vcpu, idx, data);

	pmc = static_call(kvm_x86_pmu_rdpmc_ecx_to_pmc)(vcpu, idx, &mask);
	if (!pmc)
		return 1;

	if (!kvm_is_cr4_bit_set(vcpu, X86_CR4_PCE) &&
	    (static_call(kvm_x86_get_cpl)(vcpu) != 0) &&
	    kvm_is_cr0_bit_set(vcpu, X86_CR0_PE))
		return 1;

	*data = pmc_read_counter(pmc) & mask;
	return 0;
}

void kvm_pmu_deliver_pmi(struct kvm_vcpu *vcpu)
{
	if (lapic_in_kernel(vcpu)) {
		static_call_cond(kvm_x86_pmu_deliver_pmi)(vcpu);
		kvm_apic_local_deliver(vcpu->arch.apic, APIC_LVTPC);
	}
}

bool kvm_pmu_is_valid_msr(struct kvm_vcpu *vcpu, u32 msr)
{
	switch (msr) {
	case MSR_CORE_PERF_GLOBAL_STATUS:
	case MSR_CORE_PERF_GLOBAL_CTRL:
	case MSR_CORE_PERF_GLOBAL_OVF_CTRL:
		return kvm_pmu_has_perf_global_ctrl(vcpu_to_pmu(vcpu));
	default:
		break;
	}
	return static_call(kvm_x86_pmu_msr_idx_to_pmc)(vcpu, msr) ||
		static_call(kvm_x86_pmu_is_valid_msr)(vcpu, msr);
}

static void kvm_pmu_mark_pmc_in_use(struct kvm_vcpu *vcpu, u32 msr)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct kvm_pmc *pmc = static_call(kvm_x86_pmu_msr_idx_to_pmc)(vcpu, msr);

	if (pmc)
		__set_bit(pmc->idx, pmu->pmc_in_use);
}

int kvm_pmu_get_msr(struct kvm_vcpu *vcpu, struct msr_data *msr_info)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	u32 msr = msr_info->index;

	switch (msr) {
	case MSR_CORE_PERF_GLOBAL_STATUS:
	case MSR_AMD64_PERF_CNTR_GLOBAL_STATUS:
		msr_info->data = pmu->global_status;
		break;
	case MSR_AMD64_PERF_CNTR_GLOBAL_CTL:
	case MSR_CORE_PERF_GLOBAL_CTRL:
		msr_info->data = pmu->global_ctrl;
		break;
	case MSR_AMD64_PERF_CNTR_GLOBAL_STATUS_CLR:
	case MSR_CORE_PERF_GLOBAL_OVF_CTRL:
		msr_info->data = 0;
		break;
	default:
		return static_call(kvm_x86_pmu_get_msr)(vcpu, msr_info);
	}

	return 0;
}

int kvm_pmu_set_msr(struct kvm_vcpu *vcpu, struct msr_data *msr_info)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	u32 msr = msr_info->index;
	u64 data = msr_info->data;
	u64 diff;

	/*
	 * Note, AMD ignores writes to reserved bits and read-only PMU MSRs,
	 * whereas Intel generates #GP on attempts to write reserved/RO MSRs.
	 */
	switch (msr) {
	case MSR_CORE_PERF_GLOBAL_STATUS:
		if (!msr_info->host_initiated)
			return 1; /* RO MSR */
		fallthrough;
	case MSR_AMD64_PERF_CNTR_GLOBAL_STATUS:
		/* Per PPR, Read-only MSR. Writes are ignored. */
		if (!msr_info->host_initiated)
			break;

		if (data & pmu->global_status_mask)
			return 1;

		pmu->global_status = data;
		break;
	case MSR_AMD64_PERF_CNTR_GLOBAL_CTL:
		data &= ~pmu->global_ctrl_mask;
		fallthrough;
	case MSR_CORE_PERF_GLOBAL_CTRL:
		if (!kvm_valid_perf_global_ctrl(pmu, data))
			return 1;

		if (pmu->global_ctrl != data) {
			diff = pmu->global_ctrl ^ data;
			pmu->global_ctrl = data;
			reprogram_counters(pmu, diff);
		}
		break;
	case MSR_CORE_PERF_GLOBAL_OVF_CTRL:
		/*
		 * GLOBAL_OVF_CTRL, a.k.a. GLOBAL STATUS_RESET, clears bits in
		 * GLOBAL_STATUS, and so the set of reserved bits is the same.
		 */
		if (data & pmu->global_status_mask)
			return 1;
		fallthrough;
	case MSR_AMD64_PERF_CNTR_GLOBAL_STATUS_CLR:
		if (!msr_info->host_initiated)
			pmu->global_status &= ~data;
		break;
	default:
		kvm_pmu_mark_pmc_in_use(vcpu, msr_info->index);
		return static_call(kvm_x86_pmu_set_msr)(vcpu, msr_info);
	}

	return 0;
}

/* refresh PMU settings. This function generally is called when underlying
 * settings are changed (such as changes of PMU CPUID by guest VMs), which
 * should rarely happen.
 */
void kvm_pmu_refresh(struct kvm_vcpu *vcpu)
{
	if (KVM_BUG_ON(kvm_vcpu_has_run(vcpu), vcpu->kvm))
		return;

	bitmap_zero(vcpu_to_pmu(vcpu)->all_valid_pmc_idx, X86_PMC_IDX_MAX);
	static_call(kvm_x86_pmu_refresh)(vcpu);
}

void kvm_pmu_reset(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);

	irq_work_sync(&pmu->irq_work);
	static_call(kvm_x86_pmu_reset)(vcpu);
}

void kvm_pmu_init(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);

	memset(pmu, 0, sizeof(*pmu));
	static_call(kvm_x86_pmu_init)(vcpu);
	init_irq_work(&pmu->irq_work, kvm_pmi_trigger_fn);
	pmu->event_count = 0;
	pmu->need_cleanup = false;
	kvm_pmu_refresh(vcpu);
}

/* Release perf_events for vPMCs that have been unused for a full time slice.  */
void kvm_pmu_cleanup(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct kvm_pmc *pmc = NULL;
	DECLARE_BITMAP(bitmask, X86_PMC_IDX_MAX);
	int i;

	pmu->need_cleanup = false;

	bitmap_andnot(bitmask, pmu->all_valid_pmc_idx,
		      pmu->pmc_in_use, X86_PMC_IDX_MAX);

	for_each_set_bit(i, bitmask, X86_PMC_IDX_MAX) {
		pmc = static_call(kvm_x86_pmu_pmc_idx_to_pmc)(pmu, i);

		if (pmc && pmc->perf_event && !pmc_speculative_in_use(pmc))
			pmc_stop_counter(pmc);
	}

	static_call_cond(kvm_x86_pmu_cleanup)(vcpu);

	bitmap_zero(pmu->pmc_in_use, X86_PMC_IDX_MAX);
}

void kvm_pmu_destroy(struct kvm_vcpu *vcpu)
{
	kvm_pmu_reset(vcpu);
}

static void kvm_pmu_incr_counter(struct kvm_pmc *pmc)
{
	pmc->prev_counter = pmc->counter;
	pmc->counter = (pmc->counter + 1) & pmc_bitmask(pmc);
	kvm_pmu_request_counter_reprogram(pmc);
}

static inline bool eventsel_match_perf_hw_id(struct kvm_pmc *pmc,
	unsigned int perf_hw_id)
{
	return !((pmc->eventsel ^ perf_get_hw_event_config(perf_hw_id)) &
		AMD64_RAW_EVENT_MASK_NB);
}

static inline bool cpl_is_matched(struct kvm_pmc *pmc)
{
	bool select_os, select_user;
	u64 config;

	if (pmc_is_gp(pmc)) {
		config = pmc->eventsel;
		select_os = config & ARCH_PERFMON_EVENTSEL_OS;
		select_user = config & ARCH_PERFMON_EVENTSEL_USR;
	} else {
		config = fixed_ctrl_field(pmc_to_pmu(pmc)->fixed_ctr_ctrl,
					  pmc->idx - INTEL_PMC_IDX_FIXED);
		select_os = config & 0x1;
		select_user = config & 0x2;
	}

	return (static_call(kvm_x86_get_cpl)(pmc->vcpu) == 0) ? select_os : select_user;
}

void kvm_pmu_trigger_event(struct kvm_vcpu *vcpu, u64 perf_hw_id)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct kvm_pmc *pmc;
	int i;

	for_each_set_bit(i, pmu->all_valid_pmc_idx, X86_PMC_IDX_MAX) {
		pmc = static_call(kvm_x86_pmu_pmc_idx_to_pmc)(pmu, i);

		if (!pmc || !pmc_event_is_allowed(pmc))
			continue;

		/* Ignore checks for edge detect, pin control, invert and CMASK bits */
		if (eventsel_match_perf_hw_id(pmc, perf_hw_id) && cpl_is_matched(pmc))
			kvm_pmu_incr_counter(pmc);
	}
}
EXPORT_SYMBOL_GPL(kvm_pmu_trigger_event);

static bool is_masked_filter_valid(const struct kvm_x86_pmu_event_filter *filter)
{
	u64 mask = kvm_pmu_ops.EVENTSEL_EVENT |
		   KVM_PMU_MASKED_ENTRY_UMASK_MASK |
		   KVM_PMU_MASKED_ENTRY_UMASK_MATCH |
		   KVM_PMU_MASKED_ENTRY_EXCLUDE;
	int i;

	for (i = 0; i < filter->nevents; i++) {
		if (filter->events[i] & ~mask)
			return false;
	}

	return true;
}

static void convert_to_masked_filter(struct kvm_x86_pmu_event_filter *filter)
{
	int i, j;

	for (i = 0, j = 0; i < filter->nevents; i++) {
		/*
		 * Skip events that are impossible to match against a guest
		 * event.  When filtering, only the event select + unit mask
		 * of the guest event is used.  To maintain backwards
		 * compatibility, impossible filters can't be rejected :-(
		 */
		if (filter->events[i] & ~(kvm_pmu_ops.EVENTSEL_EVENT |
					  ARCH_PERFMON_EVENTSEL_UMASK))
			continue;
		/*
		 * Convert userspace events to a common in-kernel event so
		 * only one code path is needed to support both events.  For
		 * the in-kernel events use masked events because they are
		 * flexible enough to handle both cases.  To convert to masked
		 * events all that's needed is to add an "all ones" umask_mask,
		 * (unmasked filter events don't support EXCLUDE).
		 */
		filter->events[j++] = filter->events[i] |
				      (0xFFULL << KVM_PMU_MASKED_ENTRY_UMASK_MASK_SHIFT);
	}

	filter->nevents = j;
}

static int prepare_filter_lists(struct kvm_x86_pmu_event_filter *filter)
{
	int i;

	if (!(filter->flags & KVM_PMU_EVENT_FLAG_MASKED_EVENTS))
		convert_to_masked_filter(filter);
	else if (!is_masked_filter_valid(filter))
		return -EINVAL;

	/*
	 * Sort entries by event select and includes vs. excludes so that all
	 * entries for a given event select can be processed efficiently during
	 * filtering.  The EXCLUDE flag uses a more significant bit than the
	 * event select, and so the sorted list is also effectively split into
	 * includes and excludes sub-lists.
	 */
	sort(&filter->events, filter->nevents, sizeof(filter->events[0]),
	     filter_sort_cmp, NULL);

	i = filter->nevents;
	/* Find the first EXCLUDE event (only supported for masked events). */
	if (filter->flags & KVM_PMU_EVENT_FLAG_MASKED_EVENTS) {
		for (i = 0; i < filter->nevents; i++) {
			if (filter->events[i] & KVM_PMU_MASKED_ENTRY_EXCLUDE)
				break;
		}
	}

	filter->nr_includes = i;
	filter->nr_excludes = filter->nevents - filter->nr_includes;
	filter->includes = filter->events;
	filter->excludes = filter->events + filter->nr_includes;

	return 0;
}

int kvm_vm_ioctl_set_pmu_event_filter(struct kvm *kvm, void __user *argp)
{
	struct kvm_pmu_event_filter __user *user_filter = argp;
	struct kvm_x86_pmu_event_filter *filter;
	struct kvm_pmu_event_filter tmp;
	struct kvm_vcpu *vcpu;
	unsigned long i;
	size_t size;
	int r;

	if (copy_from_user(&tmp, user_filter, sizeof(tmp)))
		return -EFAULT;

	if (tmp.action != KVM_PMU_EVENT_ALLOW &&
	    tmp.action != KVM_PMU_EVENT_DENY)
		return -EINVAL;

	if (tmp.flags & ~KVM_PMU_EVENT_FLAGS_VALID_MASK)
		return -EINVAL;

	if (tmp.nevents > KVM_PMU_EVENT_FILTER_MAX_EVENTS)
		return -E2BIG;

	size = struct_size(filter, events, tmp.nevents);
	filter = kzalloc(size, GFP_KERNEL_ACCOUNT);
	if (!filter)
		return -ENOMEM;

	filter->action = tmp.action;
	filter->nevents = tmp.nevents;
	filter->fixed_counter_bitmap = tmp.fixed_counter_bitmap;
	filter->flags = tmp.flags;

	r = -EFAULT;
	if (copy_from_user(filter->events, user_filter->events,
			   sizeof(filter->events[0]) * filter->nevents))
		goto cleanup;

	r = prepare_filter_lists(filter);
	if (r)
		goto cleanup;

	mutex_lock(&kvm->lock);
	filter = rcu_replace_pointer(kvm->arch.pmu_event_filter, filter,
				     mutex_is_locked(&kvm->lock));
	mutex_unlock(&kvm->lock);
	synchronize_srcu_expedited(&kvm->srcu);

	BUILD_BUG_ON(sizeof(((struct kvm_pmu *)0)->reprogram_pmi) >
		     sizeof(((struct kvm_pmu *)0)->__reprogram_pmi));

	kvm_for_each_vcpu(i, vcpu, kvm)
		atomic64_set(&vcpu_to_pmu(vcpu)->__reprogram_pmi, -1ull);

	kvm_make_all_cpus_request(kvm, KVM_REQ_PMU);

	r = 0;
cleanup:
	kfree(filter);
	return r;
}
