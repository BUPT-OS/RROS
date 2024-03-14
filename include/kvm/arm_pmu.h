/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2015 Linaro Ltd.
 * Author: Shannon Zhao <shannon.zhao@linaro.org>
 */

#ifndef __ASM_ARM_KVM_PMU_H
#define __ASM_ARM_KVM_PMU_H

#include <linux/perf_event.h>
#include <linux/perf/arm_pmuv3.h>

#define ARMV8_PMU_CYCLE_IDX		(ARMV8_PMU_MAX_COUNTERS - 1)

#if IS_ENABLED(CONFIG_HW_PERF_EVENTS) && IS_ENABLED(CONFIG_KVM)

struct kvm_pmc {
	u8 idx;	/* index into the pmu->pmc array */
	struct perf_event *perf_event;
};

struct kvm_pmu_events {
	u32 events_host;
	u32 events_guest;
};

struct kvm_pmu {
	struct irq_work overflow_work;
	struct kvm_pmu_events events;
	struct kvm_pmc pmc[ARMV8_PMU_MAX_COUNTERS];
	int irq_num;
	bool created;
	bool irq_level;
};

struct arm_pmu_entry {
	struct list_head entry;
	struct arm_pmu *arm_pmu;
};

DECLARE_STATIC_KEY_FALSE(kvm_arm_pmu_available);

static __always_inline bool kvm_arm_support_pmu_v3(void)
{
	return static_branch_likely(&kvm_arm_pmu_available);
}

#define kvm_arm_pmu_irq_initialized(v)	((v)->arch.pmu.irq_num >= VGIC_NR_SGIS)
u64 kvm_pmu_get_counter_value(struct kvm_vcpu *vcpu, u64 select_idx);
void kvm_pmu_set_counter_value(struct kvm_vcpu *vcpu, u64 select_idx, u64 val);
u64 kvm_pmu_valid_counter_mask(struct kvm_vcpu *vcpu);
u64 kvm_pmu_get_pmceid(struct kvm_vcpu *vcpu, bool pmceid1);
void kvm_pmu_vcpu_init(struct kvm_vcpu *vcpu);
void kvm_pmu_vcpu_reset(struct kvm_vcpu *vcpu);
void kvm_pmu_vcpu_destroy(struct kvm_vcpu *vcpu);
void kvm_pmu_disable_counter_mask(struct kvm_vcpu *vcpu, u64 val);
void kvm_pmu_enable_counter_mask(struct kvm_vcpu *vcpu, u64 val);
void kvm_pmu_flush_hwstate(struct kvm_vcpu *vcpu);
void kvm_pmu_sync_hwstate(struct kvm_vcpu *vcpu);
bool kvm_pmu_should_notify_user(struct kvm_vcpu *vcpu);
void kvm_pmu_update_run(struct kvm_vcpu *vcpu);
void kvm_pmu_software_increment(struct kvm_vcpu *vcpu, u64 val);
void kvm_pmu_handle_pmcr(struct kvm_vcpu *vcpu, u64 val);
void kvm_pmu_set_counter_event_type(struct kvm_vcpu *vcpu, u64 data,
				    u64 select_idx);
int kvm_arm_pmu_v3_set_attr(struct kvm_vcpu *vcpu,
			    struct kvm_device_attr *attr);
int kvm_arm_pmu_v3_get_attr(struct kvm_vcpu *vcpu,
			    struct kvm_device_attr *attr);
int kvm_arm_pmu_v3_has_attr(struct kvm_vcpu *vcpu,
			    struct kvm_device_attr *attr);
int kvm_arm_pmu_v3_enable(struct kvm_vcpu *vcpu);

struct kvm_pmu_events *kvm_get_pmu_events(void);
void kvm_vcpu_pmu_restore_guest(struct kvm_vcpu *vcpu);
void kvm_vcpu_pmu_restore_host(struct kvm_vcpu *vcpu);
void kvm_vcpu_pmu_resync_el0(void);

#define kvm_vcpu_has_pmu(vcpu)					\
	(test_bit(KVM_ARM_VCPU_PMU_V3, (vcpu)->arch.features))

/*
 * Updates the vcpu's view of the pmu events for this cpu.
 * Must be called before every vcpu run after disabling interrupts, to ensure
 * that an interrupt cannot fire and update the structure.
 */
#define kvm_pmu_update_vcpu_events(vcpu)				\
	do {								\
		if (!has_vhe() && kvm_vcpu_has_pmu(vcpu))		\
			vcpu->arch.pmu.events = *kvm_get_pmu_events();	\
	} while (0)

/*
 * Evaluates as true when emulating PMUv3p5, and false otherwise.
 */
#define kvm_pmu_is_3p5(vcpu) ({						\
	u64 val = IDREG(vcpu->kvm, SYS_ID_AA64DFR0_EL1);		\
	u8 pmuver = SYS_FIELD_GET(ID_AA64DFR0_EL1, PMUVer, val);	\
									\
	pmuver >= ID_AA64DFR0_EL1_PMUVer_V3P5;				\
})

u8 kvm_arm_pmu_get_pmuver_limit(void);

#else
struct kvm_pmu {
};

static inline bool kvm_arm_support_pmu_v3(void)
{
	return false;
}

#define kvm_arm_pmu_irq_initialized(v)	(false)
static inline u64 kvm_pmu_get_counter_value(struct kvm_vcpu *vcpu,
					    u64 select_idx)
{
	return 0;
}
static inline void kvm_pmu_set_counter_value(struct kvm_vcpu *vcpu,
					     u64 select_idx, u64 val) {}
static inline u64 kvm_pmu_valid_counter_mask(struct kvm_vcpu *vcpu)
{
	return 0;
}
static inline void kvm_pmu_vcpu_init(struct kvm_vcpu *vcpu) {}
static inline void kvm_pmu_vcpu_reset(struct kvm_vcpu *vcpu) {}
static inline void kvm_pmu_vcpu_destroy(struct kvm_vcpu *vcpu) {}
static inline void kvm_pmu_disable_counter_mask(struct kvm_vcpu *vcpu, u64 val) {}
static inline void kvm_pmu_enable_counter_mask(struct kvm_vcpu *vcpu, u64 val) {}
static inline void kvm_pmu_flush_hwstate(struct kvm_vcpu *vcpu) {}
static inline void kvm_pmu_sync_hwstate(struct kvm_vcpu *vcpu) {}
static inline bool kvm_pmu_should_notify_user(struct kvm_vcpu *vcpu)
{
	return false;
}
static inline void kvm_pmu_update_run(struct kvm_vcpu *vcpu) {}
static inline void kvm_pmu_software_increment(struct kvm_vcpu *vcpu, u64 val) {}
static inline void kvm_pmu_handle_pmcr(struct kvm_vcpu *vcpu, u64 val) {}
static inline void kvm_pmu_set_counter_event_type(struct kvm_vcpu *vcpu,
						  u64 data, u64 select_idx) {}
static inline int kvm_arm_pmu_v3_set_attr(struct kvm_vcpu *vcpu,
					  struct kvm_device_attr *attr)
{
	return -ENXIO;
}
static inline int kvm_arm_pmu_v3_get_attr(struct kvm_vcpu *vcpu,
					  struct kvm_device_attr *attr)
{
	return -ENXIO;
}
static inline int kvm_arm_pmu_v3_has_attr(struct kvm_vcpu *vcpu,
					  struct kvm_device_attr *attr)
{
	return -ENXIO;
}
static inline int kvm_arm_pmu_v3_enable(struct kvm_vcpu *vcpu)
{
	return 0;
}
static inline u64 kvm_pmu_get_pmceid(struct kvm_vcpu *vcpu, bool pmceid1)
{
	return 0;
}

#define kvm_vcpu_has_pmu(vcpu)		({ false; })
#define kvm_pmu_is_3p5(vcpu)		({ false; })
static inline void kvm_pmu_update_vcpu_events(struct kvm_vcpu *vcpu) {}
static inline void kvm_vcpu_pmu_restore_guest(struct kvm_vcpu *vcpu) {}
static inline void kvm_vcpu_pmu_restore_host(struct kvm_vcpu *vcpu) {}
static inline u8 kvm_arm_pmu_get_pmuver_limit(void)
{
	return 0;
}
static inline void kvm_vcpu_pmu_resync_el0(void) {}

#endif

#endif
