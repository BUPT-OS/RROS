/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_X86_VMX_HYPERV_H
#define __KVM_X86_VMX_HYPERV_H

#include <linux/jump_label.h>

#include <asm/hyperv-tlfs.h>
#include <asm/mshyperv.h>
#include <asm/vmx.h>

#include "../hyperv.h"

#include "capabilities.h"
#include "vmcs.h"
#include "vmcs12.h"

struct vmcs_config;

#define current_evmcs ((struct hv_enlightened_vmcs *)this_cpu_read(current_vmcs))

#define KVM_EVMCS_VERSION 1

struct evmcs_field {
	u16 offset;
	u16 clean_field;
};

extern const struct evmcs_field vmcs_field_to_evmcs_1[];
extern const unsigned int nr_evmcs_1_fields;

static __always_inline int evmcs_field_offset(unsigned long field,
					      u16 *clean_field)
{
	unsigned int index = ROL16(field, 6);
	const struct evmcs_field *evmcs_field;

	if (unlikely(index >= nr_evmcs_1_fields))
		return -ENOENT;

	evmcs_field = &vmcs_field_to_evmcs_1[index];

	/*
	 * Use offset=0 to detect holes in eVMCS. This offset belongs to
	 * 'revision_id' but this field has no encoding and is supposed to
	 * be accessed directly.
	 */
	if (unlikely(!evmcs_field->offset))
		return -ENOENT;

	if (clean_field)
		*clean_field = evmcs_field->clean_field;

	return evmcs_field->offset;
}

static inline u64 evmcs_read_any(struct hv_enlightened_vmcs *evmcs,
				 unsigned long field, u16 offset)
{
	/*
	 * vmcs12_read_any() doesn't care whether the supplied structure
	 * is 'struct vmcs12' or 'struct hv_enlightened_vmcs' as it takes
	 * the exact offset of the required field, use it for convenience
	 * here.
	 */
	return vmcs12_read_any((void *)evmcs, field, offset);
}

#if IS_ENABLED(CONFIG_HYPERV)

DECLARE_STATIC_KEY_FALSE(__kvm_is_using_evmcs);

static __always_inline bool kvm_is_using_evmcs(void)
{
	return static_branch_unlikely(&__kvm_is_using_evmcs);
}

static __always_inline int get_evmcs_offset(unsigned long field,
					    u16 *clean_field)
{
	int offset = evmcs_field_offset(field, clean_field);

	WARN_ONCE(offset < 0, "accessing unsupported EVMCS field %lx\n", field);
	return offset;
}

static __always_inline void evmcs_write64(unsigned long field, u64 value)
{
	u16 clean_field;
	int offset = get_evmcs_offset(field, &clean_field);

	if (offset < 0)
		return;

	*(u64 *)((char *)current_evmcs + offset) = value;

	current_evmcs->hv_clean_fields &= ~clean_field;
}

static __always_inline void evmcs_write32(unsigned long field, u32 value)
{
	u16 clean_field;
	int offset = get_evmcs_offset(field, &clean_field);

	if (offset < 0)
		return;

	*(u32 *)((char *)current_evmcs + offset) = value;
	current_evmcs->hv_clean_fields &= ~clean_field;
}

static __always_inline void evmcs_write16(unsigned long field, u16 value)
{
	u16 clean_field;
	int offset = get_evmcs_offset(field, &clean_field);

	if (offset < 0)
		return;

	*(u16 *)((char *)current_evmcs + offset) = value;
	current_evmcs->hv_clean_fields &= ~clean_field;
}

static __always_inline u64 evmcs_read64(unsigned long field)
{
	int offset = get_evmcs_offset(field, NULL);

	if (offset < 0)
		return 0;

	return *(u64 *)((char *)current_evmcs + offset);
}

static __always_inline u32 evmcs_read32(unsigned long field)
{
	int offset = get_evmcs_offset(field, NULL);

	if (offset < 0)
		return 0;

	return *(u32 *)((char *)current_evmcs + offset);
}

static __always_inline u16 evmcs_read16(unsigned long field)
{
	int offset = get_evmcs_offset(field, NULL);

	if (offset < 0)
		return 0;

	return *(u16 *)((char *)current_evmcs + offset);
}

static inline void evmcs_load(u64 phys_addr)
{
	struct hv_vp_assist_page *vp_ap =
		hv_get_vp_assist_page(smp_processor_id());

	if (current_evmcs->hv_enlightenments_control.nested_flush_hypercall)
		vp_ap->nested_control.features.directhypercall = 1;
	vp_ap->current_nested_vmcs = phys_addr;
	vp_ap->enlighten_vmentry = 1;
}

void evmcs_sanitize_exec_ctrls(struct vmcs_config *vmcs_conf);
#else /* !IS_ENABLED(CONFIG_HYPERV) */
static __always_inline bool kvm_is_using_evmcs(void) { return false; }
static __always_inline void evmcs_write64(unsigned long field, u64 value) {}
static __always_inline void evmcs_write32(unsigned long field, u32 value) {}
static __always_inline void evmcs_write16(unsigned long field, u16 value) {}
static __always_inline u64 evmcs_read64(unsigned long field) { return 0; }
static __always_inline u32 evmcs_read32(unsigned long field) { return 0; }
static __always_inline u16 evmcs_read16(unsigned long field) { return 0; }
static inline void evmcs_load(u64 phys_addr) {}
#endif /* IS_ENABLED(CONFIG_HYPERV) */

#define EVMPTR_INVALID (-1ULL)
#define EVMPTR_MAP_PENDING (-2ULL)

static inline bool evmptr_is_valid(u64 evmptr)
{
	return evmptr != EVMPTR_INVALID && evmptr != EVMPTR_MAP_PENDING;
}

enum nested_evmptrld_status {
	EVMPTRLD_DISABLED,
	EVMPTRLD_SUCCEEDED,
	EVMPTRLD_VMFAIL,
	EVMPTRLD_ERROR,
};

u64 nested_get_evmptr(struct kvm_vcpu *vcpu);
uint16_t nested_get_evmcs_version(struct kvm_vcpu *vcpu);
int nested_enable_evmcs(struct kvm_vcpu *vcpu,
			uint16_t *vmcs_version);
void nested_evmcs_filter_control_msr(struct kvm_vcpu *vcpu, u32 msr_index, u64 *pdata);
int nested_evmcs_check_controls(struct vmcs12 *vmcs12);
bool nested_evmcs_l2_tlb_flush_enabled(struct kvm_vcpu *vcpu);
void vmx_hv_inject_synthetic_vmexit_post_tlb_flush(struct kvm_vcpu *vcpu);

#endif /* __KVM_X86_VMX_HYPERV_H */
