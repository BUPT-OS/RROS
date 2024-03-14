// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel-based Virtual Machine driver for Linux
 * cpuid support routines
 *
 * derived from arch/x86/kvm/x86.c
 *
 * Copyright 2011 Red Hat, Inc. and/or its affiliates.
 * Copyright IBM Corporation, 2008
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kvm_host.h>
#include "linux/lockdep.h"
#include <linux/export.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/sched/stat.h>

#include <asm/processor.h>
#include <asm/user.h>
#include <asm/fpu/xstate.h>
#include <asm/sgx.h>
#include <asm/cpuid.h>
#include "cpuid.h"
#include "lapic.h"
#include "mmu.h"
#include "trace.h"
#include "pmu.h"
#include "xen.h"

/*
 * Unlike "struct cpuinfo_x86.x86_capability", kvm_cpu_caps doesn't need to be
 * aligned to sizeof(unsigned long) because it's not accessed via bitops.
 */
u32 kvm_cpu_caps[NR_KVM_CPU_CAPS] __read_mostly;
EXPORT_SYMBOL_GPL(kvm_cpu_caps);

u32 xstate_required_size(u64 xstate_bv, bool compacted)
{
	int feature_bit = 0;
	u32 ret = XSAVE_HDR_SIZE + XSAVE_HDR_OFFSET;

	xstate_bv &= XFEATURE_MASK_EXTEND;
	while (xstate_bv) {
		if (xstate_bv & 0x1) {
		        u32 eax, ebx, ecx, edx, offset;
		        cpuid_count(0xD, feature_bit, &eax, &ebx, &ecx, &edx);
			/* ECX[1]: 64B alignment in compacted form */
			if (compacted)
				offset = (ecx & 0x2) ? ALIGN(ret, 64) : ret;
			else
				offset = ebx;
			ret = max(ret, offset + eax);
		}

		xstate_bv >>= 1;
		feature_bit++;
	}

	return ret;
}

#define F feature_bit

/* Scattered Flag - For features that are scattered by cpufeatures.h. */
#define SF(name)						\
({								\
	BUILD_BUG_ON(X86_FEATURE_##name >= MAX_CPU_FEATURES);	\
	(boot_cpu_has(X86_FEATURE_##name) ? F(name) : 0);	\
})

/*
 * Magic value used by KVM when querying userspace-provided CPUID entries and
 * doesn't care about the CPIUD index because the index of the function in
 * question is not significant.  Note, this magic value must have at least one
 * bit set in bits[63:32] and must be consumed as a u64 by cpuid_entry2_find()
 * to avoid false positives when processing guest CPUID input.
 */
#define KVM_CPUID_INDEX_NOT_SIGNIFICANT -1ull

static inline struct kvm_cpuid_entry2 *cpuid_entry2_find(
	struct kvm_cpuid_entry2 *entries, int nent, u32 function, u64 index)
{
	struct kvm_cpuid_entry2 *e;
	int i;

	/*
	 * KVM has a semi-arbitrary rule that querying the guest's CPUID model
	 * with IRQs disabled is disallowed.  The CPUID model can legitimately
	 * have over one hundred entries, i.e. the lookup is slow, and IRQs are
	 * typically disabled in KVM only when KVM is in a performance critical
	 * path, e.g. the core VM-Enter/VM-Exit run loop.  Nothing will break
	 * if this rule is violated, this assertion is purely to flag potential
	 * performance issues.  If this fires, consider moving the lookup out
	 * of the hotpath, e.g. by caching information during CPUID updates.
	 */
	lockdep_assert_irqs_enabled();

	for (i = 0; i < nent; i++) {
		e = &entries[i];

		if (e->function != function)
			continue;

		/*
		 * If the index isn't significant, use the first entry with a
		 * matching function.  It's userspace's responsibilty to not
		 * provide "duplicate" entries in all cases.
		 */
		if (!(e->flags & KVM_CPUID_FLAG_SIGNIFCANT_INDEX) || e->index == index)
			return e;


		/*
		 * Similarly, use the first matching entry if KVM is doing a
		 * lookup (as opposed to emulating CPUID) for a function that's
		 * architecturally defined as not having a significant index.
		 */
		if (index == KVM_CPUID_INDEX_NOT_SIGNIFICANT) {
			/*
			 * Direct lookups from KVM should not diverge from what
			 * KVM defines internally (the architectural behavior).
			 */
			WARN_ON_ONCE(cpuid_function_is_indexed(function));
			return e;
		}
	}

	return NULL;
}

static int kvm_check_cpuid(struct kvm_vcpu *vcpu,
			   struct kvm_cpuid_entry2 *entries,
			   int nent)
{
	struct kvm_cpuid_entry2 *best;
	u64 xfeatures;

	/*
	 * The existing code assumes virtual address is 48-bit or 57-bit in the
	 * canonical address checks; exit if it is ever changed.
	 */
	best = cpuid_entry2_find(entries, nent, 0x80000008,
				 KVM_CPUID_INDEX_NOT_SIGNIFICANT);
	if (best) {
		int vaddr_bits = (best->eax & 0xff00) >> 8;

		if (vaddr_bits != 48 && vaddr_bits != 57 && vaddr_bits != 0)
			return -EINVAL;
	}

	/*
	 * Exposing dynamic xfeatures to the guest requires additional
	 * enabling in the FPU, e.g. to expand the guest XSAVE state size.
	 */
	best = cpuid_entry2_find(entries, nent, 0xd, 0);
	if (!best)
		return 0;

	xfeatures = best->eax | ((u64)best->edx << 32);
	xfeatures &= XFEATURE_MASK_USER_DYNAMIC;
	if (!xfeatures)
		return 0;

	return fpu_enable_guest_xfd_features(&vcpu->arch.guest_fpu, xfeatures);
}

/* Check whether the supplied CPUID data is equal to what is already set for the vCPU. */
static int kvm_cpuid_check_equal(struct kvm_vcpu *vcpu, struct kvm_cpuid_entry2 *e2,
				 int nent)
{
	struct kvm_cpuid_entry2 *orig;
	int i;

	if (nent != vcpu->arch.cpuid_nent)
		return -EINVAL;

	for (i = 0; i < nent; i++) {
		orig = &vcpu->arch.cpuid_entries[i];
		if (e2[i].function != orig->function ||
		    e2[i].index != orig->index ||
		    e2[i].flags != orig->flags ||
		    e2[i].eax != orig->eax || e2[i].ebx != orig->ebx ||
		    e2[i].ecx != orig->ecx || e2[i].edx != orig->edx)
			return -EINVAL;
	}

	return 0;
}

static struct kvm_hypervisor_cpuid kvm_get_hypervisor_cpuid(struct kvm_vcpu *vcpu,
							    const char *sig)
{
	struct kvm_hypervisor_cpuid cpuid = {};
	struct kvm_cpuid_entry2 *entry;
	u32 base;

	for_each_possible_hypervisor_cpuid_base(base) {
		entry = kvm_find_cpuid_entry(vcpu, base);

		if (entry) {
			u32 signature[3];

			signature[0] = entry->ebx;
			signature[1] = entry->ecx;
			signature[2] = entry->edx;

			if (!memcmp(signature, sig, sizeof(signature))) {
				cpuid.base = base;
				cpuid.limit = entry->eax;
				break;
			}
		}
	}

	return cpuid;
}

static struct kvm_cpuid_entry2 *__kvm_find_kvm_cpuid_features(struct kvm_vcpu *vcpu,
					      struct kvm_cpuid_entry2 *entries, int nent)
{
	u32 base = vcpu->arch.kvm_cpuid.base;

	if (!base)
		return NULL;

	return cpuid_entry2_find(entries, nent, base | KVM_CPUID_FEATURES,
				 KVM_CPUID_INDEX_NOT_SIGNIFICANT);
}

static struct kvm_cpuid_entry2 *kvm_find_kvm_cpuid_features(struct kvm_vcpu *vcpu)
{
	return __kvm_find_kvm_cpuid_features(vcpu, vcpu->arch.cpuid_entries,
					     vcpu->arch.cpuid_nent);
}

void kvm_update_pv_runtime(struct kvm_vcpu *vcpu)
{
	struct kvm_cpuid_entry2 *best = kvm_find_kvm_cpuid_features(vcpu);

	/*
	 * save the feature bitmap to avoid cpuid lookup for every PV
	 * operation
	 */
	if (best)
		vcpu->arch.pv_cpuid.features = best->eax;
}

/*
 * Calculate guest's supported XCR0 taking into account guest CPUID data and
 * KVM's supported XCR0 (comprised of host's XCR0 and KVM_SUPPORTED_XCR0).
 */
static u64 cpuid_get_supported_xcr0(struct kvm_cpuid_entry2 *entries, int nent)
{
	struct kvm_cpuid_entry2 *best;

	best = cpuid_entry2_find(entries, nent, 0xd, 0);
	if (!best)
		return 0;

	return (best->eax | ((u64)best->edx << 32)) & kvm_caps.supported_xcr0;
}

static void __kvm_update_cpuid_runtime(struct kvm_vcpu *vcpu, struct kvm_cpuid_entry2 *entries,
				       int nent)
{
	struct kvm_cpuid_entry2 *best;

	best = cpuid_entry2_find(entries, nent, 1, KVM_CPUID_INDEX_NOT_SIGNIFICANT);
	if (best) {
		/* Update OSXSAVE bit */
		if (boot_cpu_has(X86_FEATURE_XSAVE))
			cpuid_entry_change(best, X86_FEATURE_OSXSAVE,
					   kvm_is_cr4_bit_set(vcpu, X86_CR4_OSXSAVE));

		cpuid_entry_change(best, X86_FEATURE_APIC,
			   vcpu->arch.apic_base & MSR_IA32_APICBASE_ENABLE);
	}

	best = cpuid_entry2_find(entries, nent, 7, 0);
	if (best && boot_cpu_has(X86_FEATURE_PKU) && best->function == 0x7)
		cpuid_entry_change(best, X86_FEATURE_OSPKE,
				   kvm_is_cr4_bit_set(vcpu, X86_CR4_PKE));

	best = cpuid_entry2_find(entries, nent, 0xD, 0);
	if (best)
		best->ebx = xstate_required_size(vcpu->arch.xcr0, false);

	best = cpuid_entry2_find(entries, nent, 0xD, 1);
	if (best && (cpuid_entry_has(best, X86_FEATURE_XSAVES) ||
		     cpuid_entry_has(best, X86_FEATURE_XSAVEC)))
		best->ebx = xstate_required_size(vcpu->arch.xcr0, true);

	best = __kvm_find_kvm_cpuid_features(vcpu, entries, nent);
	if (kvm_hlt_in_guest(vcpu->kvm) && best &&
		(best->eax & (1 << KVM_FEATURE_PV_UNHALT)))
		best->eax &= ~(1 << KVM_FEATURE_PV_UNHALT);

	if (!kvm_check_has_quirk(vcpu->kvm, KVM_X86_QUIRK_MISC_ENABLE_NO_MWAIT)) {
		best = cpuid_entry2_find(entries, nent, 0x1, KVM_CPUID_INDEX_NOT_SIGNIFICANT);
		if (best)
			cpuid_entry_change(best, X86_FEATURE_MWAIT,
					   vcpu->arch.ia32_misc_enable_msr &
					   MSR_IA32_MISC_ENABLE_MWAIT);
	}
}

void kvm_update_cpuid_runtime(struct kvm_vcpu *vcpu)
{
	__kvm_update_cpuid_runtime(vcpu, vcpu->arch.cpuid_entries, vcpu->arch.cpuid_nent);
}
EXPORT_SYMBOL_GPL(kvm_update_cpuid_runtime);

static bool kvm_cpuid_has_hyperv(struct kvm_cpuid_entry2 *entries, int nent)
{
	struct kvm_cpuid_entry2 *entry;

	entry = cpuid_entry2_find(entries, nent, HYPERV_CPUID_INTERFACE,
				  KVM_CPUID_INDEX_NOT_SIGNIFICANT);
	return entry && entry->eax == HYPERV_CPUID_SIGNATURE_EAX;
}

static void kvm_vcpu_after_set_cpuid(struct kvm_vcpu *vcpu)
{
	struct kvm_lapic *apic = vcpu->arch.apic;
	struct kvm_cpuid_entry2 *best;
	bool allow_gbpages;

	BUILD_BUG_ON(KVM_NR_GOVERNED_FEATURES > KVM_MAX_NR_GOVERNED_FEATURES);
	bitmap_zero(vcpu->arch.governed_features.enabled,
		    KVM_MAX_NR_GOVERNED_FEATURES);

	/*
	 * If TDP is enabled, let the guest use GBPAGES if they're supported in
	 * hardware.  The hardware page walker doesn't let KVM disable GBPAGES,
	 * i.e. won't treat them as reserved, and KVM doesn't redo the GVA->GPA
	 * walk for performance and complexity reasons.  Not to mention KVM
	 * _can't_ solve the problem because GVA->GPA walks aren't visible to
	 * KVM once a TDP translation is installed.  Mimic hardware behavior so
	 * that KVM's is at least consistent, i.e. doesn't randomly inject #PF.
	 * If TDP is disabled, honor *only* guest CPUID as KVM has full control
	 * and can install smaller shadow pages if the host lacks 1GiB support.
	 */
	allow_gbpages = tdp_enabled ? boot_cpu_has(X86_FEATURE_GBPAGES) :
				      guest_cpuid_has(vcpu, X86_FEATURE_GBPAGES);
	if (allow_gbpages)
		kvm_governed_feature_set(vcpu, X86_FEATURE_GBPAGES);

	best = kvm_find_cpuid_entry(vcpu, 1);
	if (best && apic) {
		if (cpuid_entry_has(best, X86_FEATURE_TSC_DEADLINE_TIMER))
			apic->lapic_timer.timer_mode_mask = 3 << 17;
		else
			apic->lapic_timer.timer_mode_mask = 1 << 17;

		kvm_apic_set_version(vcpu);
	}

	vcpu->arch.guest_supported_xcr0 =
		cpuid_get_supported_xcr0(vcpu->arch.cpuid_entries, vcpu->arch.cpuid_nent);

	/*
	 * FP+SSE can always be saved/restored via KVM_{G,S}ET_XSAVE, even if
	 * XSAVE/XCRO are not exposed to the guest, and even if XSAVE isn't
	 * supported by the host.
	 */
	vcpu->arch.guest_fpu.fpstate->user_xfeatures = vcpu->arch.guest_supported_xcr0 |
						       XFEATURE_MASK_FPSSE;

	kvm_update_pv_runtime(vcpu);

	vcpu->arch.maxphyaddr = cpuid_query_maxphyaddr(vcpu);
	vcpu->arch.reserved_gpa_bits = kvm_vcpu_reserved_gpa_bits_raw(vcpu);

	kvm_pmu_refresh(vcpu);
	vcpu->arch.cr4_guest_rsvd_bits =
	    __cr4_reserved_bits(guest_cpuid_has, vcpu);

	kvm_hv_set_cpuid(vcpu, kvm_cpuid_has_hyperv(vcpu->arch.cpuid_entries,
						    vcpu->arch.cpuid_nent));

	/* Invoke the vendor callback only after the above state is updated. */
	static_call(kvm_x86_vcpu_after_set_cpuid)(vcpu);

	/*
	 * Except for the MMU, which needs to do its thing any vendor specific
	 * adjustments to the reserved GPA bits.
	 */
	kvm_mmu_after_set_cpuid(vcpu);
}

int cpuid_query_maxphyaddr(struct kvm_vcpu *vcpu)
{
	struct kvm_cpuid_entry2 *best;

	best = kvm_find_cpuid_entry(vcpu, 0x80000000);
	if (!best || best->eax < 0x80000008)
		goto not_found;
	best = kvm_find_cpuid_entry(vcpu, 0x80000008);
	if (best)
		return best->eax & 0xff;
not_found:
	return 36;
}

/*
 * This "raw" version returns the reserved GPA bits without any adjustments for
 * encryption technologies that usurp bits.  The raw mask should be used if and
 * only if hardware does _not_ strip the usurped bits, e.g. in virtual MTRRs.
 */
u64 kvm_vcpu_reserved_gpa_bits_raw(struct kvm_vcpu *vcpu)
{
	return rsvd_bits(cpuid_maxphyaddr(vcpu), 63);
}

static int kvm_set_cpuid(struct kvm_vcpu *vcpu, struct kvm_cpuid_entry2 *e2,
                        int nent)
{
	int r;

	__kvm_update_cpuid_runtime(vcpu, e2, nent);

	/*
	 * KVM does not correctly handle changing guest CPUID after KVM_RUN, as
	 * MAXPHYADDR, GBPAGES support, AMD reserved bit behavior, etc.. aren't
	 * tracked in kvm_mmu_page_role.  As a result, KVM may miss guest page
	 * faults due to reusing SPs/SPTEs. In practice no sane VMM mucks with
	 * the core vCPU model on the fly. It would've been better to forbid any
	 * KVM_SET_CPUID{,2} calls after KVM_RUN altogether but unfortunately
	 * some VMMs (e.g. QEMU) reuse vCPU fds for CPU hotplug/unplug and do
	 * KVM_SET_CPUID{,2} again. To support this legacy behavior, check
	 * whether the supplied CPUID data is equal to what's already set.
	 */
	if (kvm_vcpu_has_run(vcpu)) {
		r = kvm_cpuid_check_equal(vcpu, e2, nent);
		if (r)
			return r;

		kvfree(e2);
		return 0;
	}

	if (kvm_cpuid_has_hyperv(e2, nent)) {
		r = kvm_hv_vcpu_init(vcpu);
		if (r)
			return r;
	}

	r = kvm_check_cpuid(vcpu, e2, nent);
	if (r)
		return r;

	kvfree(vcpu->arch.cpuid_entries);
	vcpu->arch.cpuid_entries = e2;
	vcpu->arch.cpuid_nent = nent;

	vcpu->arch.kvm_cpuid = kvm_get_hypervisor_cpuid(vcpu, KVM_SIGNATURE);
	vcpu->arch.xen.cpuid = kvm_get_hypervisor_cpuid(vcpu, XEN_SIGNATURE);
	kvm_vcpu_after_set_cpuid(vcpu);

	return 0;
}

/* when an old userspace process fills a new kernel module */
int kvm_vcpu_ioctl_set_cpuid(struct kvm_vcpu *vcpu,
			     struct kvm_cpuid *cpuid,
			     struct kvm_cpuid_entry __user *entries)
{
	int r, i;
	struct kvm_cpuid_entry *e = NULL;
	struct kvm_cpuid_entry2 *e2 = NULL;

	if (cpuid->nent > KVM_MAX_CPUID_ENTRIES)
		return -E2BIG;

	if (cpuid->nent) {
		e = vmemdup_user(entries, array_size(sizeof(*e), cpuid->nent));
		if (IS_ERR(e))
			return PTR_ERR(e);

		e2 = kvmalloc_array(cpuid->nent, sizeof(*e2), GFP_KERNEL_ACCOUNT);
		if (!e2) {
			r = -ENOMEM;
			goto out_free_cpuid;
		}
	}
	for (i = 0; i < cpuid->nent; i++) {
		e2[i].function = e[i].function;
		e2[i].eax = e[i].eax;
		e2[i].ebx = e[i].ebx;
		e2[i].ecx = e[i].ecx;
		e2[i].edx = e[i].edx;
		e2[i].index = 0;
		e2[i].flags = 0;
		e2[i].padding[0] = 0;
		e2[i].padding[1] = 0;
		e2[i].padding[2] = 0;
	}

	r = kvm_set_cpuid(vcpu, e2, cpuid->nent);
	if (r)
		kvfree(e2);

out_free_cpuid:
	kvfree(e);

	return r;
}

int kvm_vcpu_ioctl_set_cpuid2(struct kvm_vcpu *vcpu,
			      struct kvm_cpuid2 *cpuid,
			      struct kvm_cpuid_entry2 __user *entries)
{
	struct kvm_cpuid_entry2 *e2 = NULL;
	int r;

	if (cpuid->nent > KVM_MAX_CPUID_ENTRIES)
		return -E2BIG;

	if (cpuid->nent) {
		e2 = vmemdup_user(entries, array_size(sizeof(*e2), cpuid->nent));
		if (IS_ERR(e2))
			return PTR_ERR(e2);
	}

	r = kvm_set_cpuid(vcpu, e2, cpuid->nent);
	if (r)
		kvfree(e2);

	return r;
}

int kvm_vcpu_ioctl_get_cpuid2(struct kvm_vcpu *vcpu,
			      struct kvm_cpuid2 *cpuid,
			      struct kvm_cpuid_entry2 __user *entries)
{
	if (cpuid->nent < vcpu->arch.cpuid_nent)
		return -E2BIG;

	if (copy_to_user(entries, vcpu->arch.cpuid_entries,
			 vcpu->arch.cpuid_nent * sizeof(struct kvm_cpuid_entry2)))
		return -EFAULT;

	cpuid->nent = vcpu->arch.cpuid_nent;
	return 0;
}

/* Mask kvm_cpu_caps for @leaf with the raw CPUID capabilities of this CPU. */
static __always_inline void __kvm_cpu_cap_mask(unsigned int leaf)
{
	const struct cpuid_reg cpuid = x86_feature_cpuid(leaf * 32);
	struct kvm_cpuid_entry2 entry;

	reverse_cpuid_check(leaf);

	cpuid_count(cpuid.function, cpuid.index,
		    &entry.eax, &entry.ebx, &entry.ecx, &entry.edx);

	kvm_cpu_caps[leaf] &= *__cpuid_entry_get_reg(&entry, cpuid.reg);
}

static __always_inline
void kvm_cpu_cap_init_kvm_defined(enum kvm_only_cpuid_leafs leaf, u32 mask)
{
	/* Use kvm_cpu_cap_mask for leafs that aren't KVM-only. */
	BUILD_BUG_ON(leaf < NCAPINTS);

	kvm_cpu_caps[leaf] = mask;

	__kvm_cpu_cap_mask(leaf);
}

static __always_inline void kvm_cpu_cap_mask(enum cpuid_leafs leaf, u32 mask)
{
	/* Use kvm_cpu_cap_init_kvm_defined for KVM-only leafs. */
	BUILD_BUG_ON(leaf >= NCAPINTS);

	kvm_cpu_caps[leaf] &= mask;

	__kvm_cpu_cap_mask(leaf);
}

void kvm_set_cpu_caps(void)
{
#ifdef CONFIG_X86_64
	unsigned int f_gbpages = F(GBPAGES);
	unsigned int f_lm = F(LM);
	unsigned int f_xfd = F(XFD);
#else
	unsigned int f_gbpages = 0;
	unsigned int f_lm = 0;
	unsigned int f_xfd = 0;
#endif
	memset(kvm_cpu_caps, 0, sizeof(kvm_cpu_caps));

	BUILD_BUG_ON(sizeof(kvm_cpu_caps) - (NKVMCAPINTS * sizeof(*kvm_cpu_caps)) >
		     sizeof(boot_cpu_data.x86_capability));

	memcpy(&kvm_cpu_caps, &boot_cpu_data.x86_capability,
	       sizeof(kvm_cpu_caps) - (NKVMCAPINTS * sizeof(*kvm_cpu_caps)));

	kvm_cpu_cap_mask(CPUID_1_ECX,
		/*
		 * NOTE: MONITOR (and MWAIT) are emulated as NOP, but *not*
		 * advertised to guests via CPUID!
		 */
		F(XMM3) | F(PCLMULQDQ) | 0 /* DTES64, MONITOR */ |
		0 /* DS-CPL, VMX, SMX, EST */ |
		0 /* TM2 */ | F(SSSE3) | 0 /* CNXT-ID */ | 0 /* Reserved */ |
		F(FMA) | F(CX16) | 0 /* xTPR Update */ | F(PDCM) |
		F(PCID) | 0 /* Reserved, DCA */ | F(XMM4_1) |
		F(XMM4_2) | F(X2APIC) | F(MOVBE) | F(POPCNT) |
		0 /* Reserved*/ | F(AES) | F(XSAVE) | 0 /* OSXSAVE */ | F(AVX) |
		F(F16C) | F(RDRAND)
	);
	/* KVM emulates x2apic in software irrespective of host support. */
	kvm_cpu_cap_set(X86_FEATURE_X2APIC);

	kvm_cpu_cap_mask(CPUID_1_EDX,
		F(FPU) | F(VME) | F(DE) | F(PSE) |
		F(TSC) | F(MSR) | F(PAE) | F(MCE) |
		F(CX8) | F(APIC) | 0 /* Reserved */ | F(SEP) |
		F(MTRR) | F(PGE) | F(MCA) | F(CMOV) |
		F(PAT) | F(PSE36) | 0 /* PSN */ | F(CLFLUSH) |
		0 /* Reserved, DS, ACPI */ | F(MMX) |
		F(FXSR) | F(XMM) | F(XMM2) | F(SELFSNOOP) |
		0 /* HTT, TM, Reserved, PBE */
	);

	kvm_cpu_cap_mask(CPUID_7_0_EBX,
		F(FSGSBASE) | F(SGX) | F(BMI1) | F(HLE) | F(AVX2) |
		F(FDP_EXCPTN_ONLY) | F(SMEP) | F(BMI2) | F(ERMS) | F(INVPCID) |
		F(RTM) | F(ZERO_FCS_FDS) | 0 /*MPX*/ | F(AVX512F) |
		F(AVX512DQ) | F(RDSEED) | F(ADX) | F(SMAP) | F(AVX512IFMA) |
		F(CLFLUSHOPT) | F(CLWB) | 0 /*INTEL_PT*/ | F(AVX512PF) |
		F(AVX512ER) | F(AVX512CD) | F(SHA_NI) | F(AVX512BW) |
		F(AVX512VL));

	kvm_cpu_cap_mask(CPUID_7_ECX,
		F(AVX512VBMI) | F(LA57) | F(PKU) | 0 /*OSPKE*/ | F(RDPID) |
		F(AVX512_VPOPCNTDQ) | F(UMIP) | F(AVX512_VBMI2) | F(GFNI) |
		F(VAES) | F(VPCLMULQDQ) | F(AVX512_VNNI) | F(AVX512_BITALG) |
		F(CLDEMOTE) | F(MOVDIRI) | F(MOVDIR64B) | 0 /*WAITPKG*/ |
		F(SGX_LC) | F(BUS_LOCK_DETECT)
	);
	/* Set LA57 based on hardware capability. */
	if (cpuid_ecx(7) & F(LA57))
		kvm_cpu_cap_set(X86_FEATURE_LA57);

	/*
	 * PKU not yet implemented for shadow paging and requires OSPKE
	 * to be set on the host. Clear it if that is not the case
	 */
	if (!tdp_enabled || !boot_cpu_has(X86_FEATURE_OSPKE))
		kvm_cpu_cap_clear(X86_FEATURE_PKU);

	kvm_cpu_cap_mask(CPUID_7_EDX,
		F(AVX512_4VNNIW) | F(AVX512_4FMAPS) | F(SPEC_CTRL) |
		F(SPEC_CTRL_SSBD) | F(ARCH_CAPABILITIES) | F(INTEL_STIBP) |
		F(MD_CLEAR) | F(AVX512_VP2INTERSECT) | F(FSRM) |
		F(SERIALIZE) | F(TSXLDTRK) | F(AVX512_FP16) |
		F(AMX_TILE) | F(AMX_INT8) | F(AMX_BF16) | F(FLUSH_L1D)
	);

	/* TSC_ADJUST and ARCH_CAPABILITIES are emulated in software. */
	kvm_cpu_cap_set(X86_FEATURE_TSC_ADJUST);
	kvm_cpu_cap_set(X86_FEATURE_ARCH_CAPABILITIES);

	if (boot_cpu_has(X86_FEATURE_IBPB) && boot_cpu_has(X86_FEATURE_IBRS))
		kvm_cpu_cap_set(X86_FEATURE_SPEC_CTRL);
	if (boot_cpu_has(X86_FEATURE_STIBP))
		kvm_cpu_cap_set(X86_FEATURE_INTEL_STIBP);
	if (boot_cpu_has(X86_FEATURE_AMD_SSBD))
		kvm_cpu_cap_set(X86_FEATURE_SPEC_CTRL_SSBD);

	kvm_cpu_cap_mask(CPUID_7_1_EAX,
		F(AVX_VNNI) | F(AVX512_BF16) | F(CMPCCXADD) |
		F(FZRM) | F(FSRS) | F(FSRC) |
		F(AMX_FP16) | F(AVX_IFMA)
	);

	kvm_cpu_cap_init_kvm_defined(CPUID_7_1_EDX,
		F(AVX_VNNI_INT8) | F(AVX_NE_CONVERT) | F(PREFETCHITI) |
		F(AMX_COMPLEX)
	);

	kvm_cpu_cap_mask(CPUID_D_1_EAX,
		F(XSAVEOPT) | F(XSAVEC) | F(XGETBV1) | F(XSAVES) | f_xfd
	);

	kvm_cpu_cap_init_kvm_defined(CPUID_12_EAX,
		SF(SGX1) | SF(SGX2) | SF(SGX_EDECCSSA)
	);

	kvm_cpu_cap_mask(CPUID_8000_0001_ECX,
		F(LAHF_LM) | F(CMP_LEGACY) | 0 /*SVM*/ | 0 /* ExtApicSpace */ |
		F(CR8_LEGACY) | F(ABM) | F(SSE4A) | F(MISALIGNSSE) |
		F(3DNOWPREFETCH) | F(OSVW) | 0 /* IBS */ | F(XOP) |
		0 /* SKINIT, WDT, LWP */ | F(FMA4) | F(TBM) |
		F(TOPOEXT) | 0 /* PERFCTR_CORE */
	);

	kvm_cpu_cap_mask(CPUID_8000_0001_EDX,
		F(FPU) | F(VME) | F(DE) | F(PSE) |
		F(TSC) | F(MSR) | F(PAE) | F(MCE) |
		F(CX8) | F(APIC) | 0 /* Reserved */ | F(SYSCALL) |
		F(MTRR) | F(PGE) | F(MCA) | F(CMOV) |
		F(PAT) | F(PSE36) | 0 /* Reserved */ |
		F(NX) | 0 /* Reserved */ | F(MMXEXT) | F(MMX) |
		F(FXSR) | F(FXSR_OPT) | f_gbpages | F(RDTSCP) |
		0 /* Reserved */ | f_lm | F(3DNOWEXT) | F(3DNOW)
	);

	if (!tdp_enabled && IS_ENABLED(CONFIG_X86_64))
		kvm_cpu_cap_set(X86_FEATURE_GBPAGES);

	kvm_cpu_cap_init_kvm_defined(CPUID_8000_0007_EDX,
		SF(CONSTANT_TSC)
	);

	kvm_cpu_cap_mask(CPUID_8000_0008_EBX,
		F(CLZERO) | F(XSAVEERPTR) |
		F(WBNOINVD) | F(AMD_IBPB) | F(AMD_IBRS) | F(AMD_SSBD) | F(VIRT_SSBD) |
		F(AMD_SSB_NO) | F(AMD_STIBP) | F(AMD_STIBP_ALWAYS_ON) |
		F(AMD_PSFD)
	);

	/*
	 * AMD has separate bits for each SPEC_CTRL bit.
	 * arch/x86/kernel/cpu/bugs.c is kind enough to
	 * record that in cpufeatures so use them.
	 */
	if (boot_cpu_has(X86_FEATURE_IBPB))
		kvm_cpu_cap_set(X86_FEATURE_AMD_IBPB);
	if (boot_cpu_has(X86_FEATURE_IBRS))
		kvm_cpu_cap_set(X86_FEATURE_AMD_IBRS);
	if (boot_cpu_has(X86_FEATURE_STIBP))
		kvm_cpu_cap_set(X86_FEATURE_AMD_STIBP);
	if (boot_cpu_has(X86_FEATURE_SPEC_CTRL_SSBD))
		kvm_cpu_cap_set(X86_FEATURE_AMD_SSBD);
	if (!boot_cpu_has_bug(X86_BUG_SPEC_STORE_BYPASS))
		kvm_cpu_cap_set(X86_FEATURE_AMD_SSB_NO);
	/*
	 * The preference is to use SPEC CTRL MSR instead of the
	 * VIRT_SPEC MSR.
	 */
	if (boot_cpu_has(X86_FEATURE_LS_CFG_SSBD) &&
	    !boot_cpu_has(X86_FEATURE_AMD_SSBD))
		kvm_cpu_cap_set(X86_FEATURE_VIRT_SSBD);

	/*
	 * Hide all SVM features by default, SVM will set the cap bits for
	 * features it emulates and/or exposes for L1.
	 */
	kvm_cpu_cap_mask(CPUID_8000_000A_EDX, 0);

	kvm_cpu_cap_mask(CPUID_8000_001F_EAX,
		0 /* SME */ | F(SEV) | 0 /* VM_PAGE_FLUSH */ | F(SEV_ES) |
		F(SME_COHERENT));

	kvm_cpu_cap_mask(CPUID_8000_0021_EAX,
		F(NO_NESTED_DATA_BP) | F(LFENCE_RDTSC) | 0 /* SmmPgCfgLock */ |
		F(NULL_SEL_CLR_BASE) | F(AUTOIBRS) | 0 /* PrefetchCtlMsr */
	);

	if (cpu_feature_enabled(X86_FEATURE_SRSO_NO))
		kvm_cpu_cap_set(X86_FEATURE_SRSO_NO);

	kvm_cpu_cap_init_kvm_defined(CPUID_8000_0022_EAX,
		F(PERFMON_V2)
	);

	/*
	 * Synthesize "LFENCE is serializing" into the AMD-defined entry in
	 * KVM's supported CPUID if the feature is reported as supported by the
	 * kernel.  LFENCE_RDTSC was a Linux-defined synthetic feature long
	 * before AMD joined the bandwagon, e.g. LFENCE is serializing on most
	 * CPUs that support SSE2.  On CPUs that don't support AMD's leaf,
	 * kvm_cpu_cap_mask() will unfortunately drop the flag due to ANDing
	 * the mask with the raw host CPUID, and reporting support in AMD's
	 * leaf can make it easier for userspace to detect the feature.
	 */
	if (cpu_feature_enabled(X86_FEATURE_LFENCE_RDTSC))
		kvm_cpu_cap_set(X86_FEATURE_LFENCE_RDTSC);
	if (!static_cpu_has_bug(X86_BUG_NULL_SEG))
		kvm_cpu_cap_set(X86_FEATURE_NULL_SEL_CLR_BASE);
	kvm_cpu_cap_set(X86_FEATURE_NO_SMM_CTL_MSR);

	kvm_cpu_cap_mask(CPUID_C000_0001_EDX,
		F(XSTORE) | F(XSTORE_EN) | F(XCRYPT) | F(XCRYPT_EN) |
		F(ACE2) | F(ACE2_EN) | F(PHE) | F(PHE_EN) |
		F(PMM) | F(PMM_EN)
	);

	/*
	 * Hide RDTSCP and RDPID if either feature is reported as supported but
	 * probing MSR_TSC_AUX failed.  This is purely a sanity check and
	 * should never happen, but the guest will likely crash if RDTSCP or
	 * RDPID is misreported, and KVM has botched MSR_TSC_AUX emulation in
	 * the past.  For example, the sanity check may fire if this instance of
	 * KVM is running as L1 on top of an older, broken KVM.
	 */
	if (WARN_ON((kvm_cpu_cap_has(X86_FEATURE_RDTSCP) ||
		     kvm_cpu_cap_has(X86_FEATURE_RDPID)) &&
		     !kvm_is_supported_user_return_msr(MSR_TSC_AUX))) {
		kvm_cpu_cap_clear(X86_FEATURE_RDTSCP);
		kvm_cpu_cap_clear(X86_FEATURE_RDPID);
	}
}
EXPORT_SYMBOL_GPL(kvm_set_cpu_caps);

struct kvm_cpuid_array {
	struct kvm_cpuid_entry2 *entries;
	int maxnent;
	int nent;
};

static struct kvm_cpuid_entry2 *get_next_cpuid(struct kvm_cpuid_array *array)
{
	if (array->nent >= array->maxnent)
		return NULL;

	return &array->entries[array->nent++];
}

static struct kvm_cpuid_entry2 *do_host_cpuid(struct kvm_cpuid_array *array,
					      u32 function, u32 index)
{
	struct kvm_cpuid_entry2 *entry = get_next_cpuid(array);

	if (!entry)
		return NULL;

	memset(entry, 0, sizeof(*entry));
	entry->function = function;
	entry->index = index;
	switch (function & 0xC0000000) {
	case 0x40000000:
		/* Hypervisor leaves are always synthesized by __do_cpuid_func.  */
		return entry;

	case 0x80000000:
		/*
		 * 0x80000021 is sometimes synthesized by __do_cpuid_func, which
		 * would result in out-of-bounds calls to do_host_cpuid.
		 */
		{
			static int max_cpuid_80000000;
			if (!READ_ONCE(max_cpuid_80000000))
				WRITE_ONCE(max_cpuid_80000000, cpuid_eax(0x80000000));
			if (function > READ_ONCE(max_cpuid_80000000))
				return entry;
		}
		break;

	default:
		break;
	}

	cpuid_count(entry->function, entry->index,
		    &entry->eax, &entry->ebx, &entry->ecx, &entry->edx);

	if (cpuid_function_is_indexed(function))
		entry->flags |= KVM_CPUID_FLAG_SIGNIFCANT_INDEX;

	return entry;
}

static int __do_cpuid_func_emulated(struct kvm_cpuid_array *array, u32 func)
{
	struct kvm_cpuid_entry2 *entry;

	if (array->nent >= array->maxnent)
		return -E2BIG;

	entry = &array->entries[array->nent];
	entry->function = func;
	entry->index = 0;
	entry->flags = 0;

	switch (func) {
	case 0:
		entry->eax = 7;
		++array->nent;
		break;
	case 1:
		entry->ecx = F(MOVBE);
		++array->nent;
		break;
	case 7:
		entry->flags |= KVM_CPUID_FLAG_SIGNIFCANT_INDEX;
		entry->eax = 0;
		if (kvm_cpu_cap_has(X86_FEATURE_RDTSCP))
			entry->ecx = F(RDPID);
		++array->nent;
		break;
	default:
		break;
	}

	return 0;
}

static inline int __do_cpuid_func(struct kvm_cpuid_array *array, u32 function)
{
	struct kvm_cpuid_entry2 *entry;
	int r, i, max_idx;

	/* all calls to cpuid_count() should be made on the same cpu */
	get_cpu();

	r = -E2BIG;

	entry = do_host_cpuid(array, function, 0);
	if (!entry)
		goto out;

	switch (function) {
	case 0:
		/* Limited to the highest leaf implemented in KVM. */
		entry->eax = min(entry->eax, 0x1fU);
		break;
	case 1:
		cpuid_entry_override(entry, CPUID_1_EDX);
		cpuid_entry_override(entry, CPUID_1_ECX);
		break;
	case 2:
		/*
		 * On ancient CPUs, function 2 entries are STATEFUL.  That is,
		 * CPUID(function=2, index=0) may return different results each
		 * time, with the least-significant byte in EAX enumerating the
		 * number of times software should do CPUID(2, 0).
		 *
		 * Modern CPUs, i.e. every CPU KVM has *ever* run on are less
		 * idiotic.  Intel's SDM states that EAX & 0xff "will always
		 * return 01H. Software should ignore this value and not
		 * interpret it as an informational descriptor", while AMD's
		 * APM states that CPUID(2) is reserved.
		 *
		 * WARN if a frankenstein CPU that supports virtualization and
		 * a stateful CPUID.0x2 is encountered.
		 */
		WARN_ON_ONCE((entry->eax & 0xff) > 1);
		break;
	/* functions 4 and 0x8000001d have additional index. */
	case 4:
	case 0x8000001d:
		/*
		 * Read entries until the cache type in the previous entry is
		 * zero, i.e. indicates an invalid entry.
		 */
		for (i = 1; entry->eax & 0x1f; ++i) {
			entry = do_host_cpuid(array, function, i);
			if (!entry)
				goto out;
		}
		break;
	case 6: /* Thermal management */
		entry->eax = 0x4; /* allow ARAT */
		entry->ebx = 0;
		entry->ecx = 0;
		entry->edx = 0;
		break;
	/* function 7 has additional index. */
	case 7:
		entry->eax = min(entry->eax, 1u);
		cpuid_entry_override(entry, CPUID_7_0_EBX);
		cpuid_entry_override(entry, CPUID_7_ECX);
		cpuid_entry_override(entry, CPUID_7_EDX);

		/* KVM only supports 0x7.0 and 0x7.1, capped above via min(). */
		if (entry->eax == 1) {
			entry = do_host_cpuid(array, function, 1);
			if (!entry)
				goto out;

			cpuid_entry_override(entry, CPUID_7_1_EAX);
			cpuid_entry_override(entry, CPUID_7_1_EDX);
			entry->ebx = 0;
			entry->ecx = 0;
		}
		break;
	case 0xa: { /* Architectural Performance Monitoring */
		union cpuid10_eax eax;
		union cpuid10_edx edx;

		if (!enable_pmu || !static_cpu_has(X86_FEATURE_ARCH_PERFMON)) {
			entry->eax = entry->ebx = entry->ecx = entry->edx = 0;
			break;
		}

		eax.split.version_id = kvm_pmu_cap.version;
		eax.split.num_counters = kvm_pmu_cap.num_counters_gp;
		eax.split.bit_width = kvm_pmu_cap.bit_width_gp;
		eax.split.mask_length = kvm_pmu_cap.events_mask_len;
		edx.split.num_counters_fixed = kvm_pmu_cap.num_counters_fixed;
		edx.split.bit_width_fixed = kvm_pmu_cap.bit_width_fixed;

		if (kvm_pmu_cap.version)
			edx.split.anythread_deprecated = 1;
		edx.split.reserved1 = 0;
		edx.split.reserved2 = 0;

		entry->eax = eax.full;
		entry->ebx = kvm_pmu_cap.events_mask;
		entry->ecx = 0;
		entry->edx = edx.full;
		break;
	}
	case 0x1f:
	case 0xb:
		/*
		 * No topology; a valid topology is indicated by the presence
		 * of subleaf 1.
		 */
		entry->eax = entry->ebx = entry->ecx = 0;
		break;
	case 0xd: {
		u64 permitted_xcr0 = kvm_get_filtered_xcr0();
		u64 permitted_xss = kvm_caps.supported_xss;

		entry->eax &= permitted_xcr0;
		entry->ebx = xstate_required_size(permitted_xcr0, false);
		entry->ecx = entry->ebx;
		entry->edx &= permitted_xcr0 >> 32;
		if (!permitted_xcr0)
			break;

		entry = do_host_cpuid(array, function, 1);
		if (!entry)
			goto out;

		cpuid_entry_override(entry, CPUID_D_1_EAX);
		if (entry->eax & (F(XSAVES)|F(XSAVEC)))
			entry->ebx = xstate_required_size(permitted_xcr0 | permitted_xss,
							  true);
		else {
			WARN_ON_ONCE(permitted_xss != 0);
			entry->ebx = 0;
		}
		entry->ecx &= permitted_xss;
		entry->edx &= permitted_xss >> 32;

		for (i = 2; i < 64; ++i) {
			bool s_state;
			if (permitted_xcr0 & BIT_ULL(i))
				s_state = false;
			else if (permitted_xss & BIT_ULL(i))
				s_state = true;
			else
				continue;

			entry = do_host_cpuid(array, function, i);
			if (!entry)
				goto out;

			/*
			 * The supported check above should have filtered out
			 * invalid sub-leafs.  Only valid sub-leafs should
			 * reach this point, and they should have a non-zero
			 * save state size.  Furthermore, check whether the
			 * processor agrees with permitted_xcr0/permitted_xss
			 * on whether this is an XCR0- or IA32_XSS-managed area.
			 */
			if (WARN_ON_ONCE(!entry->eax || (entry->ecx & 0x1) != s_state)) {
				--array->nent;
				continue;
			}

			if (!kvm_cpu_cap_has(X86_FEATURE_XFD))
				entry->ecx &= ~BIT_ULL(2);
			entry->edx = 0;
		}
		break;
	}
	case 0x12:
		/* Intel SGX */
		if (!kvm_cpu_cap_has(X86_FEATURE_SGX)) {
			entry->eax = entry->ebx = entry->ecx = entry->edx = 0;
			break;
		}

		/*
		 * Index 0: Sub-features, MISCSELECT (a.k.a extended features)
		 * and max enclave sizes.   The SGX sub-features and MISCSELECT
		 * are restricted by kernel and KVM capabilities (like most
		 * feature flags), while enclave size is unrestricted.
		 */
		cpuid_entry_override(entry, CPUID_12_EAX);
		entry->ebx &= SGX_MISC_EXINFO;

		entry = do_host_cpuid(array, function, 1);
		if (!entry)
			goto out;

		/*
		 * Index 1: SECS.ATTRIBUTES.  ATTRIBUTES are restricted a la
		 * feature flags.  Advertise all supported flags, including
		 * privileged attributes that require explicit opt-in from
		 * userspace.  ATTRIBUTES.XFRM is not adjusted as userspace is
		 * expected to derive it from supported XCR0.
		 */
		entry->eax &= SGX_ATTR_PRIV_MASK | SGX_ATTR_UNPRIV_MASK;
		entry->ebx &= 0;
		break;
	/* Intel PT */
	case 0x14:
		if (!kvm_cpu_cap_has(X86_FEATURE_INTEL_PT)) {
			entry->eax = entry->ebx = entry->ecx = entry->edx = 0;
			break;
		}

		for (i = 1, max_idx = entry->eax; i <= max_idx; ++i) {
			if (!do_host_cpuid(array, function, i))
				goto out;
		}
		break;
	/* Intel AMX TILE */
	case 0x1d:
		if (!kvm_cpu_cap_has(X86_FEATURE_AMX_TILE)) {
			entry->eax = entry->ebx = entry->ecx = entry->edx = 0;
			break;
		}

		for (i = 1, max_idx = entry->eax; i <= max_idx; ++i) {
			if (!do_host_cpuid(array, function, i))
				goto out;
		}
		break;
	case 0x1e: /* TMUL information */
		if (!kvm_cpu_cap_has(X86_FEATURE_AMX_TILE)) {
			entry->eax = entry->ebx = entry->ecx = entry->edx = 0;
			break;
		}
		break;
	case KVM_CPUID_SIGNATURE: {
		const u32 *sigptr = (const u32 *)KVM_SIGNATURE;
		entry->eax = KVM_CPUID_FEATURES;
		entry->ebx = sigptr[0];
		entry->ecx = sigptr[1];
		entry->edx = sigptr[2];
		break;
	}
	case KVM_CPUID_FEATURES:
		entry->eax = (1 << KVM_FEATURE_CLOCKSOURCE) |
			     (1 << KVM_FEATURE_NOP_IO_DELAY) |
			     (1 << KVM_FEATURE_CLOCKSOURCE2) |
			     (1 << KVM_FEATURE_ASYNC_PF) |
			     (1 << KVM_FEATURE_PV_EOI) |
			     (1 << KVM_FEATURE_CLOCKSOURCE_STABLE_BIT) |
			     (1 << KVM_FEATURE_PV_UNHALT) |
			     (1 << KVM_FEATURE_PV_TLB_FLUSH) |
			     (1 << KVM_FEATURE_ASYNC_PF_VMEXIT) |
			     (1 << KVM_FEATURE_PV_SEND_IPI) |
			     (1 << KVM_FEATURE_POLL_CONTROL) |
			     (1 << KVM_FEATURE_PV_SCHED_YIELD) |
			     (1 << KVM_FEATURE_ASYNC_PF_INT);

		if (sched_info_on())
			entry->eax |= (1 << KVM_FEATURE_STEAL_TIME);

		entry->ebx = 0;
		entry->ecx = 0;
		entry->edx = 0;
		break;
	case 0x80000000:
		entry->eax = min(entry->eax, 0x80000022);
		/*
		 * Serializing LFENCE is reported in a multitude of ways, and
		 * NullSegClearsBase is not reported in CPUID on Zen2; help
		 * userspace by providing the CPUID leaf ourselves.
		 *
		 * However, only do it if the host has CPUID leaf 0x8000001d.
		 * QEMU thinks that it can query the host blindly for that
		 * CPUID leaf if KVM reports that it supports 0x8000001d or
		 * above.  The processor merrily returns values from the
		 * highest Intel leaf which QEMU tries to use as the guest's
		 * 0x8000001d.  Even worse, this can result in an infinite
		 * loop if said highest leaf has no subleaves indexed by ECX.
		 */
		if (entry->eax >= 0x8000001d &&
		    (static_cpu_has(X86_FEATURE_LFENCE_RDTSC)
		     || !static_cpu_has_bug(X86_BUG_NULL_SEG)))
			entry->eax = max(entry->eax, 0x80000021);
		break;
	case 0x80000001:
		entry->ebx &= ~GENMASK(27, 16);
		cpuid_entry_override(entry, CPUID_8000_0001_EDX);
		cpuid_entry_override(entry, CPUID_8000_0001_ECX);
		break;
	case 0x80000005:
		/*  Pass host L1 cache and TLB info. */
		break;
	case 0x80000006:
		/* Drop reserved bits, pass host L2 cache and TLB info. */
		entry->edx &= ~GENMASK(17, 16);
		break;
	case 0x80000007: /* Advanced power management */
		cpuid_entry_override(entry, CPUID_8000_0007_EDX);

		/* mask against host */
		entry->edx &= boot_cpu_data.x86_power;
		entry->eax = entry->ebx = entry->ecx = 0;
		break;
	case 0x80000008: {
		unsigned g_phys_as = (entry->eax >> 16) & 0xff;
		unsigned virt_as = max((entry->eax >> 8) & 0xff, 48U);
		unsigned phys_as = entry->eax & 0xff;

		/*
		 * If TDP (NPT) is disabled use the adjusted host MAXPHYADDR as
		 * the guest operates in the same PA space as the host, i.e.
		 * reductions in MAXPHYADDR for memory encryption affect shadow
		 * paging, too.
		 *
		 * If TDP is enabled but an explicit guest MAXPHYADDR is not
		 * provided, use the raw bare metal MAXPHYADDR as reductions to
		 * the HPAs do not affect GPAs.
		 */
		if (!tdp_enabled)
			g_phys_as = boot_cpu_data.x86_phys_bits;
		else if (!g_phys_as)
			g_phys_as = phys_as;

		entry->eax = g_phys_as | (virt_as << 8);
		entry->ecx &= ~(GENMASK(31, 16) | GENMASK(11, 8));
		entry->edx = 0;
		cpuid_entry_override(entry, CPUID_8000_0008_EBX);
		break;
	}
	case 0x8000000A:
		if (!kvm_cpu_cap_has(X86_FEATURE_SVM)) {
			entry->eax = entry->ebx = entry->ecx = entry->edx = 0;
			break;
		}
		entry->eax = 1; /* SVM revision 1 */
		entry->ebx = 8; /* Lets support 8 ASIDs in case we add proper
				   ASID emulation to nested SVM */
		entry->ecx = 0; /* Reserved */
		cpuid_entry_override(entry, CPUID_8000_000A_EDX);
		break;
	case 0x80000019:
		entry->ecx = entry->edx = 0;
		break;
	case 0x8000001a:
		entry->eax &= GENMASK(2, 0);
		entry->ebx = entry->ecx = entry->edx = 0;
		break;
	case 0x8000001e:
		/* Do not return host topology information.  */
		entry->eax = entry->ebx = entry->ecx = 0;
		entry->edx = 0; /* reserved */
		break;
	case 0x8000001F:
		if (!kvm_cpu_cap_has(X86_FEATURE_SEV)) {
			entry->eax = entry->ebx = entry->ecx = entry->edx = 0;
		} else {
			cpuid_entry_override(entry, CPUID_8000_001F_EAX);
			/* Clear NumVMPL since KVM does not support VMPL.  */
			entry->ebx &= ~GENMASK(31, 12);
			/*
			 * Enumerate '0' for "PA bits reduction", the adjusted
			 * MAXPHYADDR is enumerated directly (see 0x80000008).
			 */
			entry->ebx &= ~GENMASK(11, 6);
		}
		break;
	case 0x80000020:
		entry->eax = entry->ebx = entry->ecx = entry->edx = 0;
		break;
	case 0x80000021:
		entry->ebx = entry->ecx = entry->edx = 0;
		cpuid_entry_override(entry, CPUID_8000_0021_EAX);
		break;
	/* AMD Extended Performance Monitoring and Debug */
	case 0x80000022: {
		union cpuid_0x80000022_ebx ebx;

		entry->ecx = entry->edx = 0;
		if (!enable_pmu || !kvm_cpu_cap_has(X86_FEATURE_PERFMON_V2)) {
			entry->eax = entry->ebx;
			break;
		}

		cpuid_entry_override(entry, CPUID_8000_0022_EAX);

		if (kvm_cpu_cap_has(X86_FEATURE_PERFMON_V2))
			ebx.split.num_core_pmc = kvm_pmu_cap.num_counters_gp;
		else if (kvm_cpu_cap_has(X86_FEATURE_PERFCTR_CORE))
			ebx.split.num_core_pmc = AMD64_NUM_COUNTERS_CORE;
		else
			ebx.split.num_core_pmc = AMD64_NUM_COUNTERS;

		entry->ebx = ebx.full;
		break;
	}
	/*Add support for Centaur's CPUID instruction*/
	case 0xC0000000:
		/*Just support up to 0xC0000004 now*/
		entry->eax = min(entry->eax, 0xC0000004);
		break;
	case 0xC0000001:
		cpuid_entry_override(entry, CPUID_C000_0001_EDX);
		break;
	case 3: /* Processor serial number */
	case 5: /* MONITOR/MWAIT */
	case 0xC0000002:
	case 0xC0000003:
	case 0xC0000004:
	default:
		entry->eax = entry->ebx = entry->ecx = entry->edx = 0;
		break;
	}

	r = 0;

out:
	put_cpu();

	return r;
}

static int do_cpuid_func(struct kvm_cpuid_array *array, u32 func,
			 unsigned int type)
{
	if (type == KVM_GET_EMULATED_CPUID)
		return __do_cpuid_func_emulated(array, func);

	return __do_cpuid_func(array, func);
}

#define CENTAUR_CPUID_SIGNATURE 0xC0000000

static int get_cpuid_func(struct kvm_cpuid_array *array, u32 func,
			  unsigned int type)
{
	u32 limit;
	int r;

	if (func == CENTAUR_CPUID_SIGNATURE &&
	    boot_cpu_data.x86_vendor != X86_VENDOR_CENTAUR)
		return 0;

	r = do_cpuid_func(array, func, type);
	if (r)
		return r;

	limit = array->entries[array->nent - 1].eax;
	for (func = func + 1; func <= limit; ++func) {
		r = do_cpuid_func(array, func, type);
		if (r)
			break;
	}

	return r;
}

static bool sanity_check_entries(struct kvm_cpuid_entry2 __user *entries,
				 __u32 num_entries, unsigned int ioctl_type)
{
	int i;
	__u32 pad[3];

	if (ioctl_type != KVM_GET_EMULATED_CPUID)
		return false;

	/*
	 * We want to make sure that ->padding is being passed clean from
	 * userspace in case we want to use it for something in the future.
	 *
	 * Sadly, this wasn't enforced for KVM_GET_SUPPORTED_CPUID and so we
	 * have to give ourselves satisfied only with the emulated side. /me
	 * sheds a tear.
	 */
	for (i = 0; i < num_entries; i++) {
		if (copy_from_user(pad, entries[i].padding, sizeof(pad)))
			return true;

		if (pad[0] || pad[1] || pad[2])
			return true;
	}
	return false;
}

int kvm_dev_ioctl_get_cpuid(struct kvm_cpuid2 *cpuid,
			    struct kvm_cpuid_entry2 __user *entries,
			    unsigned int type)
{
	static const u32 funcs[] = {
		0, 0x80000000, CENTAUR_CPUID_SIGNATURE, KVM_CPUID_SIGNATURE,
	};

	struct kvm_cpuid_array array = {
		.nent = 0,
	};
	int r, i;

	if (cpuid->nent < 1)
		return -E2BIG;
	if (cpuid->nent > KVM_MAX_CPUID_ENTRIES)
		cpuid->nent = KVM_MAX_CPUID_ENTRIES;

	if (sanity_check_entries(entries, cpuid->nent, type))
		return -EINVAL;

	array.entries = kvcalloc(cpuid->nent, sizeof(struct kvm_cpuid_entry2), GFP_KERNEL);
	if (!array.entries)
		return -ENOMEM;

	array.maxnent = cpuid->nent;

	for (i = 0; i < ARRAY_SIZE(funcs); i++) {
		r = get_cpuid_func(&array, funcs[i], type);
		if (r)
			goto out_free;
	}
	cpuid->nent = array.nent;

	if (copy_to_user(entries, array.entries,
			 array.nent * sizeof(struct kvm_cpuid_entry2)))
		r = -EFAULT;

out_free:
	kvfree(array.entries);
	return r;
}

struct kvm_cpuid_entry2 *kvm_find_cpuid_entry_index(struct kvm_vcpu *vcpu,
						    u32 function, u32 index)
{
	return cpuid_entry2_find(vcpu->arch.cpuid_entries, vcpu->arch.cpuid_nent,
				 function, index);
}
EXPORT_SYMBOL_GPL(kvm_find_cpuid_entry_index);

struct kvm_cpuid_entry2 *kvm_find_cpuid_entry(struct kvm_vcpu *vcpu,
					      u32 function)
{
	return cpuid_entry2_find(vcpu->arch.cpuid_entries, vcpu->arch.cpuid_nent,
				 function, KVM_CPUID_INDEX_NOT_SIGNIFICANT);
}
EXPORT_SYMBOL_GPL(kvm_find_cpuid_entry);

/*
 * Intel CPUID semantics treats any query for an out-of-range leaf as if the
 * highest basic leaf (i.e. CPUID.0H:EAX) were requested.  AMD CPUID semantics
 * returns all zeroes for any undefined leaf, whether or not the leaf is in
 * range.  Centaur/VIA follows Intel semantics.
 *
 * A leaf is considered out-of-range if its function is higher than the maximum
 * supported leaf of its associated class or if its associated class does not
 * exist.
 *
 * There are three primary classes to be considered, with their respective
 * ranges described as "<base> - <top>[,<base2> - <top2>] inclusive.  A primary
 * class exists if a guest CPUID entry for its <base> leaf exists.  For a given
 * class, CPUID.<base>.EAX contains the max supported leaf for the class.
 *
 *  - Basic:      0x00000000 - 0x3fffffff, 0x50000000 - 0x7fffffff
 *  - Hypervisor: 0x40000000 - 0x4fffffff
 *  - Extended:   0x80000000 - 0xbfffffff
 *  - Centaur:    0xc0000000 - 0xcfffffff
 *
 * The Hypervisor class is further subdivided into sub-classes that each act as
 * their own independent class associated with a 0x100 byte range.  E.g. if Qemu
 * is advertising support for both HyperV and KVM, the resulting Hypervisor
 * CPUID sub-classes are:
 *
 *  - HyperV:     0x40000000 - 0x400000ff
 *  - KVM:        0x40000100 - 0x400001ff
 */
static struct kvm_cpuid_entry2 *
get_out_of_range_cpuid_entry(struct kvm_vcpu *vcpu, u32 *fn_ptr, u32 index)
{
	struct kvm_cpuid_entry2 *basic, *class;
	u32 function = *fn_ptr;

	basic = kvm_find_cpuid_entry(vcpu, 0);
	if (!basic)
		return NULL;

	if (is_guest_vendor_amd(basic->ebx, basic->ecx, basic->edx) ||
	    is_guest_vendor_hygon(basic->ebx, basic->ecx, basic->edx))
		return NULL;

	if (function >= 0x40000000 && function <= 0x4fffffff)
		class = kvm_find_cpuid_entry(vcpu, function & 0xffffff00);
	else if (function >= 0xc0000000)
		class = kvm_find_cpuid_entry(vcpu, 0xc0000000);
	else
		class = kvm_find_cpuid_entry(vcpu, function & 0x80000000);

	if (class && function <= class->eax)
		return NULL;

	/*
	 * Leaf specific adjustments are also applied when redirecting to the
	 * max basic entry, e.g. if the max basic leaf is 0xb but there is no
	 * entry for CPUID.0xb.index (see below), then the output value for EDX
	 * needs to be pulled from CPUID.0xb.1.
	 */
	*fn_ptr = basic->eax;

	/*
	 * The class does not exist or the requested function is out of range;
	 * the effective CPUID entry is the max basic leaf.  Note, the index of
	 * the original requested leaf is observed!
	 */
	return kvm_find_cpuid_entry_index(vcpu, basic->eax, index);
}

bool kvm_cpuid(struct kvm_vcpu *vcpu, u32 *eax, u32 *ebx,
	       u32 *ecx, u32 *edx, bool exact_only)
{
	u32 orig_function = *eax, function = *eax, index = *ecx;
	struct kvm_cpuid_entry2 *entry;
	bool exact, used_max_basic = false;

	entry = kvm_find_cpuid_entry_index(vcpu, function, index);
	exact = !!entry;

	if (!entry && !exact_only) {
		entry = get_out_of_range_cpuid_entry(vcpu, &function, index);
		used_max_basic = !!entry;
	}

	if (entry) {
		*eax = entry->eax;
		*ebx = entry->ebx;
		*ecx = entry->ecx;
		*edx = entry->edx;
		if (function == 7 && index == 0) {
			u64 data;
		        if (!__kvm_get_msr(vcpu, MSR_IA32_TSX_CTRL, &data, true) &&
			    (data & TSX_CTRL_CPUID_CLEAR))
				*ebx &= ~(F(RTM) | F(HLE));
		} else if (function == 0x80000007) {
			if (kvm_hv_invtsc_suppressed(vcpu))
				*edx &= ~SF(CONSTANT_TSC);
		}
	} else {
		*eax = *ebx = *ecx = *edx = 0;
		/*
		 * When leaf 0BH or 1FH is defined, CL is pass-through
		 * and EDX is always the x2APIC ID, even for undefined
		 * subleaves. Index 1 will exist iff the leaf is
		 * implemented, so we pass through CL iff leaf 1
		 * exists. EDX can be copied from any existing index.
		 */
		if (function == 0xb || function == 0x1f) {
			entry = kvm_find_cpuid_entry_index(vcpu, function, 1);
			if (entry) {
				*ecx = index & 0xff;
				*edx = entry->edx;
			}
		}
	}
	trace_kvm_cpuid(orig_function, index, *eax, *ebx, *ecx, *edx, exact,
			used_max_basic);
	return exact;
}
EXPORT_SYMBOL_GPL(kvm_cpuid);

int kvm_emulate_cpuid(struct kvm_vcpu *vcpu)
{
	u32 eax, ebx, ecx, edx;

	if (cpuid_fault_enabled(vcpu) && !kvm_require_cpl(vcpu, 0))
		return 1;

	eax = kvm_rax_read(vcpu);
	ecx = kvm_rcx_read(vcpu);
	kvm_cpuid(vcpu, &eax, &ebx, &ecx, &edx, false);
	kvm_rax_write(vcpu, eax);
	kvm_rbx_write(vcpu, ebx);
	kvm_rcx_write(vcpu, ecx);
	kvm_rdx_write(vcpu, edx);
	return kvm_skip_emulated_instruction(vcpu);
}
EXPORT_SYMBOL_GPL(kvm_emulate_cpuid);
