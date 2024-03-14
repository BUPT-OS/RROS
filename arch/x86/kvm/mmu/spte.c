// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel-based Virtual Machine driver for Linux
 *
 * Macros and functions to access KVM PTEs (also known as SPTEs)
 *
 * Copyright (C) 2006 Qumranet, Inc.
 * Copyright 2020 Red Hat, Inc. and/or its affiliates.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kvm_host.h>
#include "mmu.h"
#include "mmu_internal.h"
#include "x86.h"
#include "spte.h"

#include <asm/e820/api.h>
#include <asm/memtype.h>
#include <asm/vmx.h>

bool __read_mostly enable_mmio_caching = true;
static bool __ro_after_init allow_mmio_caching;
module_param_named(mmio_caching, enable_mmio_caching, bool, 0444);
EXPORT_SYMBOL_GPL(enable_mmio_caching);

u64 __read_mostly shadow_host_writable_mask;
u64 __read_mostly shadow_mmu_writable_mask;
u64 __read_mostly shadow_nx_mask;
u64 __read_mostly shadow_x_mask; /* mutual exclusive with nx_mask */
u64 __read_mostly shadow_user_mask;
u64 __read_mostly shadow_accessed_mask;
u64 __read_mostly shadow_dirty_mask;
u64 __read_mostly shadow_mmio_value;
u64 __read_mostly shadow_mmio_mask;
u64 __read_mostly shadow_mmio_access_mask;
u64 __read_mostly shadow_present_mask;
u64 __read_mostly shadow_memtype_mask;
u64 __read_mostly shadow_me_value;
u64 __read_mostly shadow_me_mask;
u64 __read_mostly shadow_acc_track_mask;

u64 __read_mostly shadow_nonpresent_or_rsvd_mask;
u64 __read_mostly shadow_nonpresent_or_rsvd_lower_gfn_mask;

u8 __read_mostly shadow_phys_bits;

void __init kvm_mmu_spte_module_init(void)
{
	/*
	 * Snapshot userspace's desire to allow MMIO caching.  Whether or not
	 * KVM can actually enable MMIO caching depends on vendor-specific
	 * hardware capabilities and other module params that can't be resolved
	 * until the vendor module is loaded, i.e. enable_mmio_caching can and
	 * will change when the vendor module is (re)loaded.
	 */
	allow_mmio_caching = enable_mmio_caching;
}

static u64 generation_mmio_spte_mask(u64 gen)
{
	u64 mask;

	WARN_ON_ONCE(gen & ~MMIO_SPTE_GEN_MASK);

	mask = (gen << MMIO_SPTE_GEN_LOW_SHIFT) & MMIO_SPTE_GEN_LOW_MASK;
	mask |= (gen << MMIO_SPTE_GEN_HIGH_SHIFT) & MMIO_SPTE_GEN_HIGH_MASK;
	return mask;
}

u64 make_mmio_spte(struct kvm_vcpu *vcpu, u64 gfn, unsigned int access)
{
	u64 gen = kvm_vcpu_memslots(vcpu)->generation & MMIO_SPTE_GEN_MASK;
	u64 spte = generation_mmio_spte_mask(gen);
	u64 gpa = gfn << PAGE_SHIFT;

	WARN_ON_ONCE(!shadow_mmio_value);

	access &= shadow_mmio_access_mask;
	spte |= shadow_mmio_value | access;
	spte |= gpa | shadow_nonpresent_or_rsvd_mask;
	spte |= (gpa & shadow_nonpresent_or_rsvd_mask)
		<< SHADOW_NONPRESENT_OR_RSVD_MASK_LEN;

	return spte;
}

static bool kvm_is_mmio_pfn(kvm_pfn_t pfn)
{
	if (pfn_valid(pfn))
		return !is_zero_pfn(pfn) && PageReserved(pfn_to_page(pfn)) &&
			/*
			 * Some reserved pages, such as those from NVDIMM
			 * DAX devices, are not for MMIO, and can be mapped
			 * with cached memory type for better performance.
			 * However, the above check misconceives those pages
			 * as MMIO, and results in KVM mapping them with UC
			 * memory type, which would hurt the performance.
			 * Therefore, we check the host memory type in addition
			 * and only treat UC/UC-/WC pages as MMIO.
			 */
			(!pat_enabled() || pat_pfn_immune_to_uc_mtrr(pfn));

	return !e820__mapped_raw_any(pfn_to_hpa(pfn),
				     pfn_to_hpa(pfn + 1) - 1,
				     E820_TYPE_RAM);
}

/*
 * Returns true if the SPTE has bits that may be set without holding mmu_lock.
 * The caller is responsible for checking if the SPTE is shadow-present, and
 * for determining whether or not the caller cares about non-leaf SPTEs.
 */
bool spte_has_volatile_bits(u64 spte)
{
	/*
	 * Always atomically update spte if it can be updated
	 * out of mmu-lock, it can ensure dirty bit is not lost,
	 * also, it can help us to get a stable is_writable_pte()
	 * to ensure tlb flush is not missed.
	 */
	if (!is_writable_pte(spte) && is_mmu_writable_spte(spte))
		return true;

	if (is_access_track_spte(spte))
		return true;

	if (spte_ad_enabled(spte)) {
		if (!(spte & shadow_accessed_mask) ||
		    (is_writable_pte(spte) && !(spte & shadow_dirty_mask)))
			return true;
	}

	return false;
}

bool make_spte(struct kvm_vcpu *vcpu, struct kvm_mmu_page *sp,
	       const struct kvm_memory_slot *slot,
	       unsigned int pte_access, gfn_t gfn, kvm_pfn_t pfn,
	       u64 old_spte, bool prefetch, bool can_unsync,
	       bool host_writable, u64 *new_spte)
{
	int level = sp->role.level;
	u64 spte = SPTE_MMU_PRESENT_MASK;
	bool wrprot = false;

	WARN_ON_ONCE(!pte_access && !shadow_present_mask);

	if (sp->role.ad_disabled)
		spte |= SPTE_TDP_AD_DISABLED;
	else if (kvm_mmu_page_ad_need_write_protect(sp))
		spte |= SPTE_TDP_AD_WRPROT_ONLY;

	/*
	 * For the EPT case, shadow_present_mask is 0 if hardware
	 * supports exec-only page table entries.  In that case,
	 * ACC_USER_MASK and shadow_user_mask are used to represent
	 * read access.  See FNAME(gpte_access) in paging_tmpl.h.
	 */
	spte |= shadow_present_mask;
	if (!prefetch)
		spte |= spte_shadow_accessed_mask(spte);

	/*
	 * For simplicity, enforce the NX huge page mitigation even if not
	 * strictly necessary.  KVM could ignore the mitigation if paging is
	 * disabled in the guest, as the guest doesn't have any page tables to
	 * abuse.  But to safely ignore the mitigation, KVM would have to
	 * ensure a new MMU is loaded (or all shadow pages zapped) when CR0.PG
	 * is toggled on, and that's a net negative for performance when TDP is
	 * enabled.  When TDP is disabled, KVM will always switch to a new MMU
	 * when CR0.PG is toggled, but leveraging that to ignore the mitigation
	 * would tie make_spte() further to vCPU/MMU state, and add complexity
	 * just to optimize a mode that is anything but performance critical.
	 */
	if (level > PG_LEVEL_4K && (pte_access & ACC_EXEC_MASK) &&
	    is_nx_huge_page_enabled(vcpu->kvm)) {
		pte_access &= ~ACC_EXEC_MASK;
	}

	if (pte_access & ACC_EXEC_MASK)
		spte |= shadow_x_mask;
	else
		spte |= shadow_nx_mask;

	if (pte_access & ACC_USER_MASK)
		spte |= shadow_user_mask;

	if (level > PG_LEVEL_4K)
		spte |= PT_PAGE_SIZE_MASK;

	if (shadow_memtype_mask)
		spte |= static_call(kvm_x86_get_mt_mask)(vcpu, gfn,
							 kvm_is_mmio_pfn(pfn));
	if (host_writable)
		spte |= shadow_host_writable_mask;
	else
		pte_access &= ~ACC_WRITE_MASK;

	if (shadow_me_value && !kvm_is_mmio_pfn(pfn))
		spte |= shadow_me_value;

	spte |= (u64)pfn << PAGE_SHIFT;

	if (pte_access & ACC_WRITE_MASK) {
		spte |= PT_WRITABLE_MASK | shadow_mmu_writable_mask;

		/*
		 * Optimization: for pte sync, if spte was writable the hash
		 * lookup is unnecessary (and expensive). Write protection
		 * is responsibility of kvm_mmu_get_page / kvm_mmu_sync_roots.
		 * Same reasoning can be applied to dirty page accounting.
		 */
		if (is_writable_pte(old_spte))
			goto out;

		/*
		 * Unsync shadow pages that are reachable by the new, writable
		 * SPTE.  Write-protect the SPTE if the page can't be unsync'd,
		 * e.g. it's write-tracked (upper-level SPs) or has one or more
		 * shadow pages and unsync'ing pages is not allowed.
		 */
		if (mmu_try_to_unsync_pages(vcpu->kvm, slot, gfn, can_unsync, prefetch)) {
			wrprot = true;
			pte_access &= ~ACC_WRITE_MASK;
			spte &= ~(PT_WRITABLE_MASK | shadow_mmu_writable_mask);
		}
	}

	if (pte_access & ACC_WRITE_MASK)
		spte |= spte_shadow_dirty_mask(spte);

out:
	if (prefetch)
		spte = mark_spte_for_access_track(spte);

	WARN_ONCE(is_rsvd_spte(&vcpu->arch.mmu->shadow_zero_check, spte, level),
		  "spte = 0x%llx, level = %d, rsvd bits = 0x%llx", spte, level,
		  get_rsvd_bits(&vcpu->arch.mmu->shadow_zero_check, spte, level));

	if ((spte & PT_WRITABLE_MASK) && kvm_slot_dirty_track_enabled(slot)) {
		/* Enforced by kvm_mmu_hugepage_adjust. */
		WARN_ON_ONCE(level > PG_LEVEL_4K);
		mark_page_dirty_in_slot(vcpu->kvm, slot, gfn);
	}

	*new_spte = spte;
	return wrprot;
}

static u64 make_spte_executable(u64 spte)
{
	bool is_access_track = is_access_track_spte(spte);

	if (is_access_track)
		spte = restore_acc_track_spte(spte);

	spte &= ~shadow_nx_mask;
	spte |= shadow_x_mask;

	if (is_access_track)
		spte = mark_spte_for_access_track(spte);

	return spte;
}

/*
 * Construct an SPTE that maps a sub-page of the given huge page SPTE where
 * `index` identifies which sub-page.
 *
 * This is used during huge page splitting to build the SPTEs that make up the
 * new page table.
 */
u64 make_huge_page_split_spte(struct kvm *kvm, u64 huge_spte, union kvm_mmu_page_role role,
			      int index)
{
	u64 child_spte;

	if (WARN_ON_ONCE(!is_shadow_present_pte(huge_spte)))
		return 0;

	if (WARN_ON_ONCE(!is_large_pte(huge_spte)))
		return 0;

	child_spte = huge_spte;

	/*
	 * The child_spte already has the base address of the huge page being
	 * split. So we just have to OR in the offset to the page at the next
	 * lower level for the given index.
	 */
	child_spte |= (index * KVM_PAGES_PER_HPAGE(role.level)) << PAGE_SHIFT;

	if (role.level == PG_LEVEL_4K) {
		child_spte &= ~PT_PAGE_SIZE_MASK;

		/*
		 * When splitting to a 4K page where execution is allowed, mark
		 * the page executable as the NX hugepage mitigation no longer
		 * applies.
		 */
		if ((role.access & ACC_EXEC_MASK) && is_nx_huge_page_enabled(kvm))
			child_spte = make_spte_executable(child_spte);
	}

	return child_spte;
}


u64 make_nonleaf_spte(u64 *child_pt, bool ad_disabled)
{
	u64 spte = SPTE_MMU_PRESENT_MASK;

	spte |= __pa(child_pt) | shadow_present_mask | PT_WRITABLE_MASK |
		shadow_user_mask | shadow_x_mask | shadow_me_value;

	if (ad_disabled)
		spte |= SPTE_TDP_AD_DISABLED;
	else
		spte |= shadow_accessed_mask;

	return spte;
}

u64 kvm_mmu_changed_pte_notifier_make_spte(u64 old_spte, kvm_pfn_t new_pfn)
{
	u64 new_spte;

	new_spte = old_spte & ~SPTE_BASE_ADDR_MASK;
	new_spte |= (u64)new_pfn << PAGE_SHIFT;

	new_spte &= ~PT_WRITABLE_MASK;
	new_spte &= ~shadow_host_writable_mask;
	new_spte &= ~shadow_mmu_writable_mask;

	new_spte = mark_spte_for_access_track(new_spte);

	return new_spte;
}

u64 mark_spte_for_access_track(u64 spte)
{
	if (spte_ad_enabled(spte))
		return spte & ~shadow_accessed_mask;

	if (is_access_track_spte(spte))
		return spte;

	check_spte_writable_invariants(spte);

	WARN_ONCE(spte & (SHADOW_ACC_TRACK_SAVED_BITS_MASK <<
			  SHADOW_ACC_TRACK_SAVED_BITS_SHIFT),
		  "Access Tracking saved bit locations are not zero\n");

	spte |= (spte & SHADOW_ACC_TRACK_SAVED_BITS_MASK) <<
		SHADOW_ACC_TRACK_SAVED_BITS_SHIFT;
	spte &= ~shadow_acc_track_mask;

	return spte;
}

void kvm_mmu_set_mmio_spte_mask(u64 mmio_value, u64 mmio_mask, u64 access_mask)
{
	BUG_ON((u64)(unsigned)access_mask != access_mask);
	WARN_ON(mmio_value & shadow_nonpresent_or_rsvd_lower_gfn_mask);

	/*
	 * Reset to the original module param value to honor userspace's desire
	 * to (dis)allow MMIO caching.  Update the param itself so that
	 * userspace can see whether or not KVM is actually using MMIO caching.
	 */
	enable_mmio_caching = allow_mmio_caching;
	if (!enable_mmio_caching)
		mmio_value = 0;

	/*
	 * The mask must contain only bits that are carved out specifically for
	 * the MMIO SPTE mask, e.g. to ensure there's no overlap with the MMIO
	 * generation.
	 */
	if (WARN_ON(mmio_mask & ~SPTE_MMIO_ALLOWED_MASK))
		mmio_value = 0;

	/*
	 * Disable MMIO caching if the MMIO value collides with the bits that
	 * are used to hold the relocated GFN when the L1TF mitigation is
	 * enabled.  This should never fire as there is no known hardware that
	 * can trigger this condition, e.g. SME/SEV CPUs that require a custom
	 * MMIO value are not susceptible to L1TF.
	 */
	if (WARN_ON(mmio_value & (shadow_nonpresent_or_rsvd_mask <<
				  SHADOW_NONPRESENT_OR_RSVD_MASK_LEN)))
		mmio_value = 0;

	/*
	 * The masked MMIO value must obviously match itself and a removed SPTE
	 * must not get a false positive.  Removed SPTEs and MMIO SPTEs should
	 * never collide as MMIO must set some RWX bits, and removed SPTEs must
	 * not set any RWX bits.
	 */
	if (WARN_ON((mmio_value & mmio_mask) != mmio_value) ||
	    WARN_ON(mmio_value && (REMOVED_SPTE & mmio_mask) == mmio_value))
		mmio_value = 0;

	if (!mmio_value)
		enable_mmio_caching = false;

	shadow_mmio_value = mmio_value;
	shadow_mmio_mask  = mmio_mask;
	shadow_mmio_access_mask = access_mask;
}
EXPORT_SYMBOL_GPL(kvm_mmu_set_mmio_spte_mask);

void kvm_mmu_set_me_spte_mask(u64 me_value, u64 me_mask)
{
	/* shadow_me_value must be a subset of shadow_me_mask */
	if (WARN_ON(me_value & ~me_mask))
		me_value = me_mask = 0;

	shadow_me_value = me_value;
	shadow_me_mask = me_mask;
}
EXPORT_SYMBOL_GPL(kvm_mmu_set_me_spte_mask);

void kvm_mmu_set_ept_masks(bool has_ad_bits, bool has_exec_only)
{
	shadow_user_mask	= VMX_EPT_READABLE_MASK;
	shadow_accessed_mask	= has_ad_bits ? VMX_EPT_ACCESS_BIT : 0ull;
	shadow_dirty_mask	= has_ad_bits ? VMX_EPT_DIRTY_BIT : 0ull;
	shadow_nx_mask		= 0ull;
	shadow_x_mask		= VMX_EPT_EXECUTABLE_MASK;
	shadow_present_mask	= has_exec_only ? 0ull : VMX_EPT_READABLE_MASK;
	/*
	 * EPT overrides the host MTRRs, and so KVM must program the desired
	 * memtype directly into the SPTEs.  Note, this mask is just the mask
	 * of all bits that factor into the memtype, the actual memtype must be
	 * dynamically calculated, e.g. to ensure host MMIO is mapped UC.
	 */
	shadow_memtype_mask	= VMX_EPT_MT_MASK | VMX_EPT_IPAT_BIT;
	shadow_acc_track_mask	= VMX_EPT_RWX_MASK;
	shadow_host_writable_mask = EPT_SPTE_HOST_WRITABLE;
	shadow_mmu_writable_mask  = EPT_SPTE_MMU_WRITABLE;

	/*
	 * EPT Misconfigurations are generated if the value of bits 2:0
	 * of an EPT paging-structure entry is 110b (write/execute).
	 */
	kvm_mmu_set_mmio_spte_mask(VMX_EPT_MISCONFIG_WX_VALUE,
				   VMX_EPT_RWX_MASK, 0);
}
EXPORT_SYMBOL_GPL(kvm_mmu_set_ept_masks);

void kvm_mmu_reset_all_pte_masks(void)
{
	u8 low_phys_bits;
	u64 mask;

	shadow_phys_bits = kvm_get_shadow_phys_bits();

	/*
	 * If the CPU has 46 or less physical address bits, then set an
	 * appropriate mask to guard against L1TF attacks. Otherwise, it is
	 * assumed that the CPU is not vulnerable to L1TF.
	 *
	 * Some Intel CPUs address the L1 cache using more PA bits than are
	 * reported by CPUID. Use the PA width of the L1 cache when possible
	 * to achieve more effective mitigation, e.g. if system RAM overlaps
	 * the most significant bits of legal physical address space.
	 */
	shadow_nonpresent_or_rsvd_mask = 0;
	low_phys_bits = boot_cpu_data.x86_phys_bits;
	if (boot_cpu_has_bug(X86_BUG_L1TF) &&
	    !WARN_ON_ONCE(boot_cpu_data.x86_cache_bits >=
			  52 - SHADOW_NONPRESENT_OR_RSVD_MASK_LEN)) {
		low_phys_bits = boot_cpu_data.x86_cache_bits
			- SHADOW_NONPRESENT_OR_RSVD_MASK_LEN;
		shadow_nonpresent_or_rsvd_mask =
			rsvd_bits(low_phys_bits, boot_cpu_data.x86_cache_bits - 1);
	}

	shadow_nonpresent_or_rsvd_lower_gfn_mask =
		GENMASK_ULL(low_phys_bits - 1, PAGE_SHIFT);

	shadow_user_mask	= PT_USER_MASK;
	shadow_accessed_mask	= PT_ACCESSED_MASK;
	shadow_dirty_mask	= PT_DIRTY_MASK;
	shadow_nx_mask		= PT64_NX_MASK;
	shadow_x_mask		= 0;
	shadow_present_mask	= PT_PRESENT_MASK;

	/*
	 * For shadow paging and NPT, KVM uses PAT entry '0' to encode WB
	 * memtype in the SPTEs, i.e. relies on host MTRRs to provide the
	 * correct memtype (WB is the "weakest" memtype).
	 */
	shadow_memtype_mask	= 0;
	shadow_acc_track_mask	= 0;
	shadow_me_mask		= 0;
	shadow_me_value		= 0;

	shadow_host_writable_mask = DEFAULT_SPTE_HOST_WRITABLE;
	shadow_mmu_writable_mask  = DEFAULT_SPTE_MMU_WRITABLE;

	/*
	 * Set a reserved PA bit in MMIO SPTEs to generate page faults with
	 * PFEC.RSVD=1 on MMIO accesses.  64-bit PTEs (PAE, x86-64, and EPT
	 * paging) support a maximum of 52 bits of PA, i.e. if the CPU supports
	 * 52-bit physical addresses then there are no reserved PA bits in the
	 * PTEs and so the reserved PA approach must be disabled.
	 */
	if (shadow_phys_bits < 52)
		mask = BIT_ULL(51) | PT_PRESENT_MASK;
	else
		mask = 0;

	kvm_mmu_set_mmio_spte_mask(mask, mask, ACC_WRITE_MASK | ACC_USER_MASK);
}
