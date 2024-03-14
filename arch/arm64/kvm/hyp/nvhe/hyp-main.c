// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 - Google Inc
 * Author: Andrew Scull <ascull@google.com>
 */

#include <hyp/adjust_pc.h>

#include <asm/pgtable-types.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_emulate.h>
#include <asm/kvm_host.h>
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>

#include <nvhe/ffa.h>
#include <nvhe/mem_protect.h>
#include <nvhe/mm.h>
#include <nvhe/pkvm.h>
#include <nvhe/trap_handler.h>

DEFINE_PER_CPU(struct kvm_nvhe_init_params, kvm_init_params);

void __kvm_hyp_host_forward_smc(struct kvm_cpu_context *host_ctxt);

static void flush_hyp_vcpu(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	struct kvm_vcpu *host_vcpu = hyp_vcpu->host_vcpu;

	hyp_vcpu->vcpu.arch.ctxt	= host_vcpu->arch.ctxt;

	hyp_vcpu->vcpu.arch.sve_state	= kern_hyp_va(host_vcpu->arch.sve_state);
	hyp_vcpu->vcpu.arch.sve_max_vl	= host_vcpu->arch.sve_max_vl;

	hyp_vcpu->vcpu.arch.hw_mmu	= host_vcpu->arch.hw_mmu;

	hyp_vcpu->vcpu.arch.hcr_el2	= host_vcpu->arch.hcr_el2;
	hyp_vcpu->vcpu.arch.mdcr_el2	= host_vcpu->arch.mdcr_el2;
	hyp_vcpu->vcpu.arch.cptr_el2	= host_vcpu->arch.cptr_el2;

	hyp_vcpu->vcpu.arch.iflags	= host_vcpu->arch.iflags;
	hyp_vcpu->vcpu.arch.fp_state	= host_vcpu->arch.fp_state;

	hyp_vcpu->vcpu.arch.debug_ptr	= kern_hyp_va(host_vcpu->arch.debug_ptr);
	hyp_vcpu->vcpu.arch.host_fpsimd_state = host_vcpu->arch.host_fpsimd_state;

	hyp_vcpu->vcpu.arch.vsesr_el2	= host_vcpu->arch.vsesr_el2;

	hyp_vcpu->vcpu.arch.vgic_cpu.vgic_v3 = host_vcpu->arch.vgic_cpu.vgic_v3;
}

static void sync_hyp_vcpu(struct pkvm_hyp_vcpu *hyp_vcpu)
{
	struct kvm_vcpu *host_vcpu = hyp_vcpu->host_vcpu;
	struct vgic_v3_cpu_if *hyp_cpu_if = &hyp_vcpu->vcpu.arch.vgic_cpu.vgic_v3;
	struct vgic_v3_cpu_if *host_cpu_if = &host_vcpu->arch.vgic_cpu.vgic_v3;
	unsigned int i;

	host_vcpu->arch.ctxt		= hyp_vcpu->vcpu.arch.ctxt;

	host_vcpu->arch.hcr_el2		= hyp_vcpu->vcpu.arch.hcr_el2;
	host_vcpu->arch.cptr_el2	= hyp_vcpu->vcpu.arch.cptr_el2;

	host_vcpu->arch.fault		= hyp_vcpu->vcpu.arch.fault;

	host_vcpu->arch.iflags		= hyp_vcpu->vcpu.arch.iflags;
	host_vcpu->arch.fp_state	= hyp_vcpu->vcpu.arch.fp_state;

	host_cpu_if->vgic_hcr		= hyp_cpu_if->vgic_hcr;
	for (i = 0; i < hyp_cpu_if->used_lrs; ++i)
		host_cpu_if->vgic_lr[i] = hyp_cpu_if->vgic_lr[i];
}

static void handle___kvm_vcpu_run(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(struct kvm_vcpu *, host_vcpu, host_ctxt, 1);
	int ret;

	host_vcpu = kern_hyp_va(host_vcpu);

	if (unlikely(is_protected_kvm_enabled())) {
		struct pkvm_hyp_vcpu *hyp_vcpu;
		struct kvm *host_kvm;

		host_kvm = kern_hyp_va(host_vcpu->kvm);
		hyp_vcpu = pkvm_load_hyp_vcpu(host_kvm->arch.pkvm.handle,
					      host_vcpu->vcpu_idx);
		if (!hyp_vcpu) {
			ret = -EINVAL;
			goto out;
		}

		flush_hyp_vcpu(hyp_vcpu);

		ret = __kvm_vcpu_run(&hyp_vcpu->vcpu);

		sync_hyp_vcpu(hyp_vcpu);
		pkvm_put_hyp_vcpu(hyp_vcpu);
	} else {
		/* The host is fully trusted, run its vCPU directly. */
		ret = __kvm_vcpu_run(host_vcpu);
	}

out:
	cpu_reg(host_ctxt, 1) =  ret;
}

static void handle___kvm_adjust_pc(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(struct kvm_vcpu *, vcpu, host_ctxt, 1);

	__kvm_adjust_pc(kern_hyp_va(vcpu));
}

static void handle___kvm_flush_vm_context(struct kvm_cpu_context *host_ctxt)
{
	__kvm_flush_vm_context();
}

static void handle___kvm_tlb_flush_vmid_ipa(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(struct kvm_s2_mmu *, mmu, host_ctxt, 1);
	DECLARE_REG(phys_addr_t, ipa, host_ctxt, 2);
	DECLARE_REG(int, level, host_ctxt, 3);

	__kvm_tlb_flush_vmid_ipa(kern_hyp_va(mmu), ipa, level);
}

static void handle___kvm_tlb_flush_vmid_ipa_nsh(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(struct kvm_s2_mmu *, mmu, host_ctxt, 1);
	DECLARE_REG(phys_addr_t, ipa, host_ctxt, 2);
	DECLARE_REG(int, level, host_ctxt, 3);

	__kvm_tlb_flush_vmid_ipa_nsh(kern_hyp_va(mmu), ipa, level);
}

static void
handle___kvm_tlb_flush_vmid_range(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(struct kvm_s2_mmu *, mmu, host_ctxt, 1);
	DECLARE_REG(phys_addr_t, start, host_ctxt, 2);
	DECLARE_REG(unsigned long, pages, host_ctxt, 3);

	__kvm_tlb_flush_vmid_range(kern_hyp_va(mmu), start, pages);
}

static void handle___kvm_tlb_flush_vmid(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(struct kvm_s2_mmu *, mmu, host_ctxt, 1);

	__kvm_tlb_flush_vmid(kern_hyp_va(mmu));
}

static void handle___kvm_flush_cpu_context(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(struct kvm_s2_mmu *, mmu, host_ctxt, 1);

	__kvm_flush_cpu_context(kern_hyp_va(mmu));
}

static void handle___kvm_timer_set_cntvoff(struct kvm_cpu_context *host_ctxt)
{
	__kvm_timer_set_cntvoff(cpu_reg(host_ctxt, 1));
}

static void handle___kvm_enable_ssbs(struct kvm_cpu_context *host_ctxt)
{
	u64 tmp;

	tmp = read_sysreg_el2(SYS_SCTLR);
	tmp |= SCTLR_ELx_DSSBS;
	write_sysreg_el2(tmp, SYS_SCTLR);
}

static void handle___vgic_v3_get_gic_config(struct kvm_cpu_context *host_ctxt)
{
	cpu_reg(host_ctxt, 1) = __vgic_v3_get_gic_config();
}

static void handle___vgic_v3_read_vmcr(struct kvm_cpu_context *host_ctxt)
{
	cpu_reg(host_ctxt, 1) = __vgic_v3_read_vmcr();
}

static void handle___vgic_v3_write_vmcr(struct kvm_cpu_context *host_ctxt)
{
	__vgic_v3_write_vmcr(cpu_reg(host_ctxt, 1));
}

static void handle___vgic_v3_init_lrs(struct kvm_cpu_context *host_ctxt)
{
	__vgic_v3_init_lrs();
}

static void handle___kvm_get_mdcr_el2(struct kvm_cpu_context *host_ctxt)
{
	cpu_reg(host_ctxt, 1) = __kvm_get_mdcr_el2();
}

static void handle___vgic_v3_save_aprs(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(struct vgic_v3_cpu_if *, cpu_if, host_ctxt, 1);

	__vgic_v3_save_aprs(kern_hyp_va(cpu_if));
}

static void handle___vgic_v3_restore_aprs(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(struct vgic_v3_cpu_if *, cpu_if, host_ctxt, 1);

	__vgic_v3_restore_aprs(kern_hyp_va(cpu_if));
}

static void handle___pkvm_init(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(phys_addr_t, phys, host_ctxt, 1);
	DECLARE_REG(unsigned long, size, host_ctxt, 2);
	DECLARE_REG(unsigned long, nr_cpus, host_ctxt, 3);
	DECLARE_REG(unsigned long *, per_cpu_base, host_ctxt, 4);
	DECLARE_REG(u32, hyp_va_bits, host_ctxt, 5);

	/*
	 * __pkvm_init() will return only if an error occurred, otherwise it
	 * will tail-call in __pkvm_init_finalise() which will have to deal
	 * with the host context directly.
	 */
	cpu_reg(host_ctxt, 1) = __pkvm_init(phys, size, nr_cpus, per_cpu_base,
					    hyp_va_bits);
}

static void handle___pkvm_cpu_set_vector(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(enum arm64_hyp_spectre_vector, slot, host_ctxt, 1);

	cpu_reg(host_ctxt, 1) = pkvm_cpu_set_vector(slot);
}

static void handle___pkvm_host_share_hyp(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(u64, pfn, host_ctxt, 1);

	cpu_reg(host_ctxt, 1) = __pkvm_host_share_hyp(pfn);
}

static void handle___pkvm_host_unshare_hyp(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(u64, pfn, host_ctxt, 1);

	cpu_reg(host_ctxt, 1) = __pkvm_host_unshare_hyp(pfn);
}

static void handle___pkvm_create_private_mapping(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(phys_addr_t, phys, host_ctxt, 1);
	DECLARE_REG(size_t, size, host_ctxt, 2);
	DECLARE_REG(enum kvm_pgtable_prot, prot, host_ctxt, 3);

	/*
	 * __pkvm_create_private_mapping() populates a pointer with the
	 * hypervisor start address of the allocation.
	 *
	 * However, handle___pkvm_create_private_mapping() hypercall crosses the
	 * EL1/EL2 boundary so the pointer would not be valid in this context.
	 *
	 * Instead pass the allocation address as the return value (or return
	 * ERR_PTR() on failure).
	 */
	unsigned long haddr;
	int err = __pkvm_create_private_mapping(phys, size, prot, &haddr);

	if (err)
		haddr = (unsigned long)ERR_PTR(err);

	cpu_reg(host_ctxt, 1) = haddr;
}

static void handle___pkvm_prot_finalize(struct kvm_cpu_context *host_ctxt)
{
	cpu_reg(host_ctxt, 1) = __pkvm_prot_finalize();
}

static void handle___pkvm_vcpu_init_traps(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(struct kvm_vcpu *, vcpu, host_ctxt, 1);

	__pkvm_vcpu_init_traps(kern_hyp_va(vcpu));
}

static void handle___pkvm_init_vm(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(struct kvm *, host_kvm, host_ctxt, 1);
	DECLARE_REG(unsigned long, vm_hva, host_ctxt, 2);
	DECLARE_REG(unsigned long, pgd_hva, host_ctxt, 3);

	host_kvm = kern_hyp_va(host_kvm);
	cpu_reg(host_ctxt, 1) = __pkvm_init_vm(host_kvm, vm_hva, pgd_hva);
}

static void handle___pkvm_init_vcpu(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(pkvm_handle_t, handle, host_ctxt, 1);
	DECLARE_REG(struct kvm_vcpu *, host_vcpu, host_ctxt, 2);
	DECLARE_REG(unsigned long, vcpu_hva, host_ctxt, 3);

	host_vcpu = kern_hyp_va(host_vcpu);
	cpu_reg(host_ctxt, 1) = __pkvm_init_vcpu(handle, host_vcpu, vcpu_hva);
}

static void handle___pkvm_teardown_vm(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(pkvm_handle_t, handle, host_ctxt, 1);

	cpu_reg(host_ctxt, 1) = __pkvm_teardown_vm(handle);
}

typedef void (*hcall_t)(struct kvm_cpu_context *);

#define HANDLE_FUNC(x)	[__KVM_HOST_SMCCC_FUNC_##x] = (hcall_t)handle_##x

static const hcall_t host_hcall[] = {
	/* ___kvm_hyp_init */
	HANDLE_FUNC(__kvm_get_mdcr_el2),
	HANDLE_FUNC(__pkvm_init),
	HANDLE_FUNC(__pkvm_create_private_mapping),
	HANDLE_FUNC(__pkvm_cpu_set_vector),
	HANDLE_FUNC(__kvm_enable_ssbs),
	HANDLE_FUNC(__vgic_v3_init_lrs),
	HANDLE_FUNC(__vgic_v3_get_gic_config),
	HANDLE_FUNC(__pkvm_prot_finalize),

	HANDLE_FUNC(__pkvm_host_share_hyp),
	HANDLE_FUNC(__pkvm_host_unshare_hyp),
	HANDLE_FUNC(__kvm_adjust_pc),
	HANDLE_FUNC(__kvm_vcpu_run),
	HANDLE_FUNC(__kvm_flush_vm_context),
	HANDLE_FUNC(__kvm_tlb_flush_vmid_ipa),
	HANDLE_FUNC(__kvm_tlb_flush_vmid_ipa_nsh),
	HANDLE_FUNC(__kvm_tlb_flush_vmid),
	HANDLE_FUNC(__kvm_tlb_flush_vmid_range),
	HANDLE_FUNC(__kvm_flush_cpu_context),
	HANDLE_FUNC(__kvm_timer_set_cntvoff),
	HANDLE_FUNC(__vgic_v3_read_vmcr),
	HANDLE_FUNC(__vgic_v3_write_vmcr),
	HANDLE_FUNC(__vgic_v3_save_aprs),
	HANDLE_FUNC(__vgic_v3_restore_aprs),
	HANDLE_FUNC(__pkvm_vcpu_init_traps),
	HANDLE_FUNC(__pkvm_init_vm),
	HANDLE_FUNC(__pkvm_init_vcpu),
	HANDLE_FUNC(__pkvm_teardown_vm),
};

static void handle_host_hcall(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(unsigned long, id, host_ctxt, 0);
	unsigned long hcall_min = 0;
	hcall_t hfn;

	/*
	 * If pKVM has been initialised then reject any calls to the
	 * early "privileged" hypercalls. Note that we cannot reject
	 * calls to __pkvm_prot_finalize for two reasons: (1) The static
	 * key used to determine initialisation must be toggled prior to
	 * finalisation and (2) finalisation is performed on a per-CPU
	 * basis. This is all fine, however, since __pkvm_prot_finalize
	 * returns -EPERM after the first call for a given CPU.
	 */
	if (static_branch_unlikely(&kvm_protected_mode_initialized))
		hcall_min = __KVM_HOST_SMCCC_FUNC___pkvm_prot_finalize;

	id &= ~ARM_SMCCC_CALL_HINTS;
	id -= KVM_HOST_SMCCC_ID(0);

	if (unlikely(id < hcall_min || id >= ARRAY_SIZE(host_hcall)))
		goto inval;

	hfn = host_hcall[id];
	if (unlikely(!hfn))
		goto inval;

	cpu_reg(host_ctxt, 0) = SMCCC_RET_SUCCESS;
	hfn(host_ctxt);

	return;
inval:
	cpu_reg(host_ctxt, 0) = SMCCC_RET_NOT_SUPPORTED;
}

static void default_host_smc_handler(struct kvm_cpu_context *host_ctxt)
{
	__kvm_hyp_host_forward_smc(host_ctxt);
}

static void handle_host_smc(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(u64, func_id, host_ctxt, 0);
	bool handled;

	func_id &= ~ARM_SMCCC_CALL_HINTS;

	handled = kvm_host_psci_handler(host_ctxt, func_id);
	if (!handled)
		handled = kvm_host_ffa_handler(host_ctxt, func_id);
	if (!handled)
		default_host_smc_handler(host_ctxt);

	/* SMC was trapped, move ELR past the current PC. */
	kvm_skip_host_instr();
}

void handle_trap(struct kvm_cpu_context *host_ctxt)
{
	u64 esr = read_sysreg_el2(SYS_ESR);

	switch (ESR_ELx_EC(esr)) {
	case ESR_ELx_EC_HVC64:
		handle_host_hcall(host_ctxt);
		break;
	case ESR_ELx_EC_SMC64:
		handle_host_smc(host_ctxt);
		break;
	case ESR_ELx_EC_SVE:
		if (has_hvhe())
			sysreg_clear_set(cpacr_el1, 0, (CPACR_EL1_ZEN_EL1EN |
							CPACR_EL1_ZEN_EL0EN));
		else
			sysreg_clear_set(cptr_el2, CPTR_EL2_TZ, 0);
		isb();
		sve_cond_update_zcr_vq(ZCR_ELx_LEN_MASK, SYS_ZCR_EL2);
		break;
	case ESR_ELx_EC_IABT_LOW:
	case ESR_ELx_EC_DABT_LOW:
		handle_host_mem_abort(host_ctxt);
		break;
	default:
		BUG();
	}
}
