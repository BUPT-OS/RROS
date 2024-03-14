/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_X86_VMX_H
#define __KVM_X86_VMX_H

#include <linux/kvm_host.h>

#include <asm/kvm.h>
#include <asm/intel_pt.h>
#include <asm/perf_event.h>

#include "capabilities.h"
#include "../kvm_cache_regs.h"
#include "posted_intr.h"
#include "vmcs.h"
#include "vmx_ops.h"
#include "../cpuid.h"
#include "run_flags.h"

#define MSR_TYPE_R	1
#define MSR_TYPE_W	2
#define MSR_TYPE_RW	3

#define X2APIC_MSR(r) (APIC_BASE_MSR + ((r) >> 4))

#ifdef CONFIG_X86_64
#define MAX_NR_USER_RETURN_MSRS	7
#else
#define MAX_NR_USER_RETURN_MSRS	4
#endif

#define MAX_NR_LOADSTORE_MSRS	8

struct vmx_msrs {
	unsigned int		nr;
	struct vmx_msr_entry	val[MAX_NR_LOADSTORE_MSRS];
};

struct vmx_uret_msr {
	bool load_into_hardware;
	u64 data;
	u64 mask;
};

enum segment_cache_field {
	SEG_FIELD_SEL = 0,
	SEG_FIELD_BASE = 1,
	SEG_FIELD_LIMIT = 2,
	SEG_FIELD_AR = 3,

	SEG_FIELD_NR = 4
};

#define RTIT_ADDR_RANGE		4

struct pt_ctx {
	u64 ctl;
	u64 status;
	u64 output_base;
	u64 output_mask;
	u64 cr3_match;
	u64 addr_a[RTIT_ADDR_RANGE];
	u64 addr_b[RTIT_ADDR_RANGE];
};

struct pt_desc {
	u64 ctl_bitmask;
	u32 num_address_ranges;
	u32 caps[PT_CPUID_REGS_NUM * PT_CPUID_LEAVES];
	struct pt_ctx host;
	struct pt_ctx guest;
};

union vmx_exit_reason {
	struct {
		u32	basic			: 16;
		u32	reserved16		: 1;
		u32	reserved17		: 1;
		u32	reserved18		: 1;
		u32	reserved19		: 1;
		u32	reserved20		: 1;
		u32	reserved21		: 1;
		u32	reserved22		: 1;
		u32	reserved23		: 1;
		u32	reserved24		: 1;
		u32	reserved25		: 1;
		u32	bus_lock_detected	: 1;
		u32	enclave_mode		: 1;
		u32	smi_pending_mtf		: 1;
		u32	smi_from_vmx_root	: 1;
		u32	reserved30		: 1;
		u32	failed_vmentry		: 1;
	};
	u32 full;
};

struct lbr_desc {
	/* Basic info about guest LBR records. */
	struct x86_pmu_lbr records;

	/*
	 * Emulate LBR feature via passthrough LBR registers when the
	 * per-vcpu guest LBR event is scheduled on the current pcpu.
	 *
	 * The records may be inaccurate if the host reclaims the LBR.
	 */
	struct perf_event *event;

	/* True if LBRs are marked as not intercepted in the MSR bitmap */
	bool msr_passthrough;
};

/*
 * The nested_vmx structure is part of vcpu_vmx, and holds information we need
 * for correct emulation of VMX (i.e., nested VMX) on this vcpu.
 */
struct nested_vmx {
	/* Has the level1 guest done vmxon? */
	bool vmxon;
	gpa_t vmxon_ptr;
	bool pml_full;

	/* The guest-physical address of the current VMCS L1 keeps for L2 */
	gpa_t current_vmptr;
	/*
	 * Cache of the guest's VMCS, existing outside of guest memory.
	 * Loaded from guest memory during VMPTRLD. Flushed to guest
	 * memory during VMCLEAR and VMPTRLD.
	 */
	struct vmcs12 *cached_vmcs12;
	/*
	 * Cache of the guest's shadow VMCS, existing outside of guest
	 * memory. Loaded from guest memory during VM entry. Flushed
	 * to guest memory during VM exit.
	 */
	struct vmcs12 *cached_shadow_vmcs12;

	/*
	 * GPA to HVA cache for accessing vmcs12->vmcs_link_pointer
	 */
	struct gfn_to_hva_cache shadow_vmcs12_cache;

	/*
	 * GPA to HVA cache for VMCS12
	 */
	struct gfn_to_hva_cache vmcs12_cache;

	/*
	 * Indicates if the shadow vmcs or enlightened vmcs must be updated
	 * with the data held by struct vmcs12.
	 */
	bool need_vmcs12_to_shadow_sync;
	bool dirty_vmcs12;

	/*
	 * Indicates whether MSR bitmap for L2 needs to be rebuilt due to
	 * changes in MSR bitmap for L1 or switching to a different L2. Note,
	 * this flag can only be used reliably in conjunction with a paravirt L1
	 * which informs L0 whether any changes to MSR bitmap for L2 were done
	 * on its side.
	 */
	bool force_msr_bitmap_recalc;

	/*
	 * Indicates lazily loaded guest state has not yet been decached from
	 * vmcs02.
	 */
	bool need_sync_vmcs02_to_vmcs12_rare;

	/*
	 * vmcs02 has been initialized, i.e. state that is constant for
	 * vmcs02 has been written to the backing VMCS.  Initialization
	 * is delayed until L1 actually attempts to run a nested VM.
	 */
	bool vmcs02_initialized;

	bool change_vmcs01_virtual_apic_mode;
	bool reload_vmcs01_apic_access_page;
	bool update_vmcs01_cpu_dirty_logging;
	bool update_vmcs01_apicv_status;

	/*
	 * Enlightened VMCS has been enabled. It does not mean that L1 has to
	 * use it. However, VMX features available to L1 will be limited based
	 * on what the enlightened VMCS supports.
	 */
	bool enlightened_vmcs_enabled;

	/* L2 must run next, and mustn't decide to exit to L1. */
	bool nested_run_pending;

	/* Pending MTF VM-exit into L1.  */
	bool mtf_pending;

	struct loaded_vmcs vmcs02;

	/*
	 * Guest pages referred to in the vmcs02 with host-physical
	 * pointers, so we must keep them pinned while L2 runs.
	 */
	struct kvm_host_map apic_access_page_map;
	struct kvm_host_map virtual_apic_map;
	struct kvm_host_map pi_desc_map;

	struct kvm_host_map msr_bitmap_map;

	struct pi_desc *pi_desc;
	bool pi_pending;
	u16 posted_intr_nv;

	struct hrtimer preemption_timer;
	u64 preemption_timer_deadline;
	bool has_preemption_timer_deadline;
	bool preemption_timer_expired;

	/*
	 * Used to snapshot MSRs that are conditionally loaded on VM-Enter in
	 * order to propagate the guest's pre-VM-Enter value into vmcs02.  For
	 * emulation of VMLAUNCH/VMRESUME, the snapshot will be of L1's value.
	 * For KVM_SET_NESTED_STATE, the snapshot is of L2's value, _if_
	 * userspace restores MSRs before nested state.  If userspace restores
	 * MSRs after nested state, the snapshot holds garbage, but KVM can't
	 * detect that, and the garbage value in vmcs02 will be overwritten by
	 * MSR restoration in any case.
	 */
	u64 pre_vmenter_debugctl;
	u64 pre_vmenter_bndcfgs;

	/* to migrate it to L1 if L2 writes to L1's CR8 directly */
	int l1_tpr_threshold;

	u16 vpid02;
	u16 last_vpid;

	struct nested_vmx_msrs msrs;

	/* SMM related state */
	struct {
		/* in VMX operation on SMM entry? */
		bool vmxon;
		/* in guest mode on SMM entry? */
		bool guest_mode;
	} smm;

	gpa_t hv_evmcs_vmptr;
	struct kvm_host_map hv_evmcs_map;
	struct hv_enlightened_vmcs *hv_evmcs;
};

struct vcpu_vmx {
	struct kvm_vcpu       vcpu;
	u8                    fail;
	u8		      x2apic_msr_bitmap_mode;

	/*
	 * If true, host state has been stored in vmx->loaded_vmcs for
	 * the CPU registers that only need to be switched when transitioning
	 * to/from the kernel, and the registers have been loaded with guest
	 * values.  If false, host state is loaded in the CPU registers
	 * and vmx->loaded_vmcs->host_state is invalid.
	 */
	bool		      guest_state_loaded;

	unsigned long         exit_qualification;
	u32                   exit_intr_info;
	u32                   idt_vectoring_info;
	ulong                 rflags;

	/*
	 * User return MSRs are always emulated when enabled in the guest, but
	 * only loaded into hardware when necessary, e.g. SYSCALL #UDs outside
	 * of 64-bit mode or if EFER.SCE=1, thus the SYSCALL MSRs don't need to
	 * be loaded into hardware if those conditions aren't met.
	 */
	struct vmx_uret_msr   guest_uret_msrs[MAX_NR_USER_RETURN_MSRS];
	bool                  guest_uret_msrs_loaded;
#ifdef CONFIG_X86_64
	u64		      msr_host_kernel_gs_base;
	u64		      msr_guest_kernel_gs_base;
#endif

	u64		      spec_ctrl;
	u32		      msr_ia32_umwait_control;

	/*
	 * loaded_vmcs points to the VMCS currently used in this vcpu. For a
	 * non-nested (L1) guest, it always points to vmcs01. For a nested
	 * guest (L2), it points to a different VMCS.
	 */
	struct loaded_vmcs    vmcs01;
	struct loaded_vmcs   *loaded_vmcs;

	struct msr_autoload {
		struct vmx_msrs guest;
		struct vmx_msrs host;
	} msr_autoload;

	struct msr_autostore {
		struct vmx_msrs guest;
	} msr_autostore;

	struct {
		int vm86_active;
		ulong save_rflags;
		struct kvm_segment segs[8];
	} rmode;
	struct {
		u32 bitmask; /* 4 bits per segment (1 bit per field) */
		struct kvm_save_segment {
			u16 selector;
			unsigned long base;
			u32 limit;
			u32 ar;
		} seg[8];
	} segment_cache;
	int vpid;
	bool emulation_required;

	union vmx_exit_reason exit_reason;

	/* Posted interrupt descriptor */
	struct pi_desc pi_desc;

	/* Used if this vCPU is waiting for PI notification wakeup. */
	struct list_head pi_wakeup_list;

	/* Support for a guest hypervisor (nested VMX) */
	struct nested_vmx nested;

	/* Dynamic PLE window. */
	unsigned int ple_window;
	bool ple_window_dirty;

	bool req_immediate_exit;

	/* Support for PML */
#define PML_ENTITY_NUM		512
	struct page *pml_pg;

	/* apic deadline value in host tsc */
	u64 hv_deadline_tsc;

	unsigned long host_debugctlmsr;

	/*
	 * Only bits masked by msr_ia32_feature_control_valid_bits can be set in
	 * msr_ia32_feature_control. FEAT_CTL_LOCKED is always included
	 * in msr_ia32_feature_control_valid_bits.
	 */
	u64 msr_ia32_feature_control;
	u64 msr_ia32_feature_control_valid_bits;
	/* SGX Launch Control public key hash */
	u64 msr_ia32_sgxlepubkeyhash[4];
	u64 msr_ia32_mcu_opt_ctrl;
	bool disable_fb_clear;

	struct pt_desc pt_desc;
	struct lbr_desc lbr_desc;

	/* Save desired MSR intercept (read: pass-through) state */
#define MAX_POSSIBLE_PASSTHROUGH_MSRS	16
	struct {
		DECLARE_BITMAP(read, MAX_POSSIBLE_PASSTHROUGH_MSRS);
		DECLARE_BITMAP(write, MAX_POSSIBLE_PASSTHROUGH_MSRS);
	} shadow_msr_intercept;
};

struct kvm_vmx {
	struct kvm kvm;

	unsigned int tss_addr;
	bool ept_identity_pagetable_done;
	gpa_t ept_identity_map_addr;
	/* Posted Interrupt Descriptor (PID) table for IPI virtualization */
	u64 *pid_table;
};

void vmx_vcpu_load_vmcs(struct kvm_vcpu *vcpu, int cpu,
			struct loaded_vmcs *buddy);
int allocate_vpid(void);
void free_vpid(int vpid);
void vmx_set_constant_host_state(struct vcpu_vmx *vmx);
void vmx_prepare_switch_to_guest(struct kvm_vcpu *vcpu);
void vmx_set_host_fs_gs(struct vmcs_host_state *host, u16 fs_sel, u16 gs_sel,
			unsigned long fs_base, unsigned long gs_base);
int vmx_get_cpl(struct kvm_vcpu *vcpu);
bool vmx_emulation_required(struct kvm_vcpu *vcpu);
unsigned long vmx_get_rflags(struct kvm_vcpu *vcpu);
void vmx_set_rflags(struct kvm_vcpu *vcpu, unsigned long rflags);
u32 vmx_get_interrupt_shadow(struct kvm_vcpu *vcpu);
void vmx_set_interrupt_shadow(struct kvm_vcpu *vcpu, int mask);
int vmx_set_efer(struct kvm_vcpu *vcpu, u64 efer);
void vmx_set_cr0(struct kvm_vcpu *vcpu, unsigned long cr0);
void vmx_set_cr4(struct kvm_vcpu *vcpu, unsigned long cr4);
void set_cr4_guest_host_mask(struct vcpu_vmx *vmx);
void ept_save_pdptrs(struct kvm_vcpu *vcpu);
void vmx_get_segment(struct kvm_vcpu *vcpu, struct kvm_segment *var, int seg);
void __vmx_set_segment(struct kvm_vcpu *vcpu, struct kvm_segment *var, int seg);
u64 construct_eptp(struct kvm_vcpu *vcpu, hpa_t root_hpa, int root_level);

bool vmx_guest_inject_ac(struct kvm_vcpu *vcpu);
void vmx_update_exception_bitmap(struct kvm_vcpu *vcpu);
bool vmx_nmi_blocked(struct kvm_vcpu *vcpu);
bool vmx_interrupt_blocked(struct kvm_vcpu *vcpu);
bool vmx_get_nmi_mask(struct kvm_vcpu *vcpu);
void vmx_set_nmi_mask(struct kvm_vcpu *vcpu, bool masked);
void vmx_set_virtual_apic_mode(struct kvm_vcpu *vcpu);
struct vmx_uret_msr *vmx_find_uret_msr(struct vcpu_vmx *vmx, u32 msr);
void pt_update_intercept_for_msr(struct kvm_vcpu *vcpu);
void vmx_update_host_rsp(struct vcpu_vmx *vmx, unsigned long host_rsp);
void vmx_spec_ctrl_restore_host(struct vcpu_vmx *vmx, unsigned int flags);
unsigned int __vmx_vcpu_run_flags(struct vcpu_vmx *vmx);
bool __vmx_vcpu_run(struct vcpu_vmx *vmx, unsigned long *regs,
		    unsigned int flags);
int vmx_find_loadstore_msr_slot(struct vmx_msrs *m, u32 msr);
void vmx_ept_load_pdptrs(struct kvm_vcpu *vcpu);

void vmx_disable_intercept_for_msr(struct kvm_vcpu *vcpu, u32 msr, int type);
void vmx_enable_intercept_for_msr(struct kvm_vcpu *vcpu, u32 msr, int type);

u64 vmx_get_l2_tsc_offset(struct kvm_vcpu *vcpu);
u64 vmx_get_l2_tsc_multiplier(struct kvm_vcpu *vcpu);

static inline void vmx_set_intercept_for_msr(struct kvm_vcpu *vcpu, u32 msr,
					     int type, bool value)
{
	if (value)
		vmx_enable_intercept_for_msr(vcpu, msr, type);
	else
		vmx_disable_intercept_for_msr(vcpu, msr, type);
}

void vmx_update_cpu_dirty_logging(struct kvm_vcpu *vcpu);

/*
 * Note, early Intel manuals have the write-low and read-high bitmap offsets
 * the wrong way round.  The bitmaps control MSRs 0x00000000-0x00001fff and
 * 0xc0000000-0xc0001fff.  The former (low) uses bytes 0-0x3ff for reads and
 * 0x800-0xbff for writes.  The latter (high) uses 0x400-0x7ff for reads and
 * 0xc00-0xfff for writes.  MSRs not covered by either of the ranges always
 * VM-Exit.
 */
#define __BUILD_VMX_MSR_BITMAP_HELPER(rtype, action, bitop, access, base)      \
static inline rtype vmx_##action##_msr_bitmap_##access(unsigned long *bitmap,  \
						       u32 msr)		       \
{									       \
	int f = sizeof(unsigned long);					       \
									       \
	if (msr <= 0x1fff)						       \
		return bitop##_bit(msr, bitmap + base / f);		       \
	else if ((msr >= 0xc0000000) && (msr <= 0xc0001fff))		       \
		return bitop##_bit(msr & 0x1fff, bitmap + (base + 0x400) / f); \
	return (rtype)true;						       \
}
#define BUILD_VMX_MSR_BITMAP_HELPERS(ret_type, action, bitop)		       \
	__BUILD_VMX_MSR_BITMAP_HELPER(ret_type, action, bitop, read,  0x0)     \
	__BUILD_VMX_MSR_BITMAP_HELPER(ret_type, action, bitop, write, 0x800)

BUILD_VMX_MSR_BITMAP_HELPERS(bool, test, test)
BUILD_VMX_MSR_BITMAP_HELPERS(void, clear, __clear)
BUILD_VMX_MSR_BITMAP_HELPERS(void, set, __set)

static inline u8 vmx_get_rvi(void)
{
	return vmcs_read16(GUEST_INTR_STATUS) & 0xff;
}

#define __KVM_REQUIRED_VMX_VM_ENTRY_CONTROLS				\
	(VM_ENTRY_LOAD_DEBUG_CONTROLS)
#ifdef CONFIG_X86_64
	#define KVM_REQUIRED_VMX_VM_ENTRY_CONTROLS			\
		(__KVM_REQUIRED_VMX_VM_ENTRY_CONTROLS |			\
		 VM_ENTRY_IA32E_MODE)
#else
	#define KVM_REQUIRED_VMX_VM_ENTRY_CONTROLS			\
		__KVM_REQUIRED_VMX_VM_ENTRY_CONTROLS
#endif
#define KVM_OPTIONAL_VMX_VM_ENTRY_CONTROLS				\
	(VM_ENTRY_LOAD_IA32_PERF_GLOBAL_CTRL |				\
	 VM_ENTRY_LOAD_IA32_PAT |					\
	 VM_ENTRY_LOAD_IA32_EFER |					\
	 VM_ENTRY_LOAD_BNDCFGS |					\
	 VM_ENTRY_PT_CONCEAL_PIP |					\
	 VM_ENTRY_LOAD_IA32_RTIT_CTL)

#define __KVM_REQUIRED_VMX_VM_EXIT_CONTROLS				\
	(VM_EXIT_SAVE_DEBUG_CONTROLS |					\
	 VM_EXIT_ACK_INTR_ON_EXIT)
#ifdef CONFIG_X86_64
	#define KVM_REQUIRED_VMX_VM_EXIT_CONTROLS			\
		(__KVM_REQUIRED_VMX_VM_EXIT_CONTROLS |			\
		 VM_EXIT_HOST_ADDR_SPACE_SIZE)
#else
	#define KVM_REQUIRED_VMX_VM_EXIT_CONTROLS			\
		__KVM_REQUIRED_VMX_VM_EXIT_CONTROLS
#endif
#define KVM_OPTIONAL_VMX_VM_EXIT_CONTROLS				\
	      (VM_EXIT_LOAD_IA32_PERF_GLOBAL_CTRL |			\
	       VM_EXIT_SAVE_IA32_PAT |					\
	       VM_EXIT_LOAD_IA32_PAT |					\
	       VM_EXIT_SAVE_IA32_EFER |					\
	       VM_EXIT_SAVE_VMX_PREEMPTION_TIMER |			\
	       VM_EXIT_LOAD_IA32_EFER |					\
	       VM_EXIT_CLEAR_BNDCFGS |					\
	       VM_EXIT_PT_CONCEAL_PIP |					\
	       VM_EXIT_CLEAR_IA32_RTIT_CTL)

#define KVM_REQUIRED_VMX_PIN_BASED_VM_EXEC_CONTROL			\
	(PIN_BASED_EXT_INTR_MASK |					\
	 PIN_BASED_NMI_EXITING)
#define KVM_OPTIONAL_VMX_PIN_BASED_VM_EXEC_CONTROL			\
	(PIN_BASED_VIRTUAL_NMIS |					\
	 PIN_BASED_POSTED_INTR |					\
	 PIN_BASED_VMX_PREEMPTION_TIMER)

#define __KVM_REQUIRED_VMX_CPU_BASED_VM_EXEC_CONTROL			\
	(CPU_BASED_HLT_EXITING |					\
	 CPU_BASED_CR3_LOAD_EXITING |					\
	 CPU_BASED_CR3_STORE_EXITING |					\
	 CPU_BASED_UNCOND_IO_EXITING |					\
	 CPU_BASED_MOV_DR_EXITING |					\
	 CPU_BASED_USE_TSC_OFFSETTING |					\
	 CPU_BASED_MWAIT_EXITING |					\
	 CPU_BASED_MONITOR_EXITING |					\
	 CPU_BASED_INVLPG_EXITING |					\
	 CPU_BASED_RDPMC_EXITING |					\
	 CPU_BASED_INTR_WINDOW_EXITING)

#ifdef CONFIG_X86_64
	#define KVM_REQUIRED_VMX_CPU_BASED_VM_EXEC_CONTROL		\
		(__KVM_REQUIRED_VMX_CPU_BASED_VM_EXEC_CONTROL |		\
		 CPU_BASED_CR8_LOAD_EXITING |				\
		 CPU_BASED_CR8_STORE_EXITING)
#else
	#define KVM_REQUIRED_VMX_CPU_BASED_VM_EXEC_CONTROL		\
		__KVM_REQUIRED_VMX_CPU_BASED_VM_EXEC_CONTROL
#endif

#define KVM_OPTIONAL_VMX_CPU_BASED_VM_EXEC_CONTROL			\
	(CPU_BASED_RDTSC_EXITING |					\
	 CPU_BASED_TPR_SHADOW |						\
	 CPU_BASED_USE_IO_BITMAPS |					\
	 CPU_BASED_MONITOR_TRAP_FLAG |					\
	 CPU_BASED_USE_MSR_BITMAPS |					\
	 CPU_BASED_NMI_WINDOW_EXITING |					\
	 CPU_BASED_PAUSE_EXITING |					\
	 CPU_BASED_ACTIVATE_SECONDARY_CONTROLS |			\
	 CPU_BASED_ACTIVATE_TERTIARY_CONTROLS)

#define KVM_REQUIRED_VMX_SECONDARY_VM_EXEC_CONTROL 0
#define KVM_OPTIONAL_VMX_SECONDARY_VM_EXEC_CONTROL			\
	(SECONDARY_EXEC_VIRTUALIZE_APIC_ACCESSES |			\
	 SECONDARY_EXEC_VIRTUALIZE_X2APIC_MODE |			\
	 SECONDARY_EXEC_WBINVD_EXITING |				\
	 SECONDARY_EXEC_ENABLE_VPID |					\
	 SECONDARY_EXEC_ENABLE_EPT |					\
	 SECONDARY_EXEC_UNRESTRICTED_GUEST |				\
	 SECONDARY_EXEC_PAUSE_LOOP_EXITING |				\
	 SECONDARY_EXEC_DESC |						\
	 SECONDARY_EXEC_ENABLE_RDTSCP |					\
	 SECONDARY_EXEC_ENABLE_INVPCID |				\
	 SECONDARY_EXEC_APIC_REGISTER_VIRT |				\
	 SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY |				\
	 SECONDARY_EXEC_SHADOW_VMCS |					\
	 SECONDARY_EXEC_ENABLE_XSAVES |					\
	 SECONDARY_EXEC_RDSEED_EXITING |				\
	 SECONDARY_EXEC_RDRAND_EXITING |				\
	 SECONDARY_EXEC_ENABLE_PML |					\
	 SECONDARY_EXEC_TSC_SCALING |					\
	 SECONDARY_EXEC_ENABLE_USR_WAIT_PAUSE |				\
	 SECONDARY_EXEC_PT_USE_GPA |					\
	 SECONDARY_EXEC_PT_CONCEAL_VMX |				\
	 SECONDARY_EXEC_ENABLE_VMFUNC |					\
	 SECONDARY_EXEC_BUS_LOCK_DETECTION |				\
	 SECONDARY_EXEC_NOTIFY_VM_EXITING |				\
	 SECONDARY_EXEC_ENCLS_EXITING)

#define KVM_REQUIRED_VMX_TERTIARY_VM_EXEC_CONTROL 0
#define KVM_OPTIONAL_VMX_TERTIARY_VM_EXEC_CONTROL			\
	(TERTIARY_EXEC_IPI_VIRT)

#define BUILD_CONTROLS_SHADOW(lname, uname, bits)						\
static inline void lname##_controls_set(struct vcpu_vmx *vmx, u##bits val)			\
{												\
	if (vmx->loaded_vmcs->controls_shadow.lname != val) {					\
		vmcs_write##bits(uname, val);							\
		vmx->loaded_vmcs->controls_shadow.lname = val;					\
	}											\
}												\
static inline u##bits __##lname##_controls_get(struct loaded_vmcs *vmcs)			\
{												\
	return vmcs->controls_shadow.lname;							\
}												\
static inline u##bits lname##_controls_get(struct vcpu_vmx *vmx)				\
{												\
	return __##lname##_controls_get(vmx->loaded_vmcs);					\
}												\
static __always_inline void lname##_controls_setbit(struct vcpu_vmx *vmx, u##bits val)		\
{												\
	BUILD_BUG_ON(!(val & (KVM_REQUIRED_VMX_##uname | KVM_OPTIONAL_VMX_##uname)));		\
	lname##_controls_set(vmx, lname##_controls_get(vmx) | val);				\
}												\
static __always_inline void lname##_controls_clearbit(struct vcpu_vmx *vmx, u##bits val)	\
{												\
	BUILD_BUG_ON(!(val & (KVM_REQUIRED_VMX_##uname | KVM_OPTIONAL_VMX_##uname)));		\
	lname##_controls_set(vmx, lname##_controls_get(vmx) & ~val);				\
}
BUILD_CONTROLS_SHADOW(vm_entry, VM_ENTRY_CONTROLS, 32)
BUILD_CONTROLS_SHADOW(vm_exit, VM_EXIT_CONTROLS, 32)
BUILD_CONTROLS_SHADOW(pin, PIN_BASED_VM_EXEC_CONTROL, 32)
BUILD_CONTROLS_SHADOW(exec, CPU_BASED_VM_EXEC_CONTROL, 32)
BUILD_CONTROLS_SHADOW(secondary_exec, SECONDARY_VM_EXEC_CONTROL, 32)
BUILD_CONTROLS_SHADOW(tertiary_exec, TERTIARY_VM_EXEC_CONTROL, 64)

/*
 * VMX_REGS_LAZY_LOAD_SET - The set of registers that will be updated in the
 * cache on demand.  Other registers not listed here are synced to
 * the cache immediately after VM-Exit.
 */
#define VMX_REGS_LAZY_LOAD_SET	((1 << VCPU_REGS_RIP) |         \
				(1 << VCPU_REGS_RSP) |          \
				(1 << VCPU_EXREG_RFLAGS) |      \
				(1 << VCPU_EXREG_PDPTR) |       \
				(1 << VCPU_EXREG_SEGMENTS) |    \
				(1 << VCPU_EXREG_CR0) |         \
				(1 << VCPU_EXREG_CR3) |         \
				(1 << VCPU_EXREG_CR4) |         \
				(1 << VCPU_EXREG_EXIT_INFO_1) | \
				(1 << VCPU_EXREG_EXIT_INFO_2))

static inline unsigned long vmx_l1_guest_owned_cr0_bits(void)
{
	unsigned long bits = KVM_POSSIBLE_CR0_GUEST_BITS;

	/*
	 * CR0.WP needs to be intercepted when KVM is shadowing legacy paging
	 * in order to construct shadow PTEs with the correct protections.
	 * Note!  CR0.WP technically can be passed through to the guest if
	 * paging is disabled, but checking CR0.PG would generate a cyclical
	 * dependency of sorts due to forcing the caller to ensure CR0 holds
	 * the correct value prior to determining which CR0 bits can be owned
	 * by L1.  Keep it simple and limit the optimization to EPT.
	 */
	if (!enable_ept)
		bits &= ~X86_CR0_WP;
	return bits;
}

static __always_inline struct kvm_vmx *to_kvm_vmx(struct kvm *kvm)
{
	return container_of(kvm, struct kvm_vmx, kvm);
}

static __always_inline struct vcpu_vmx *to_vmx(struct kvm_vcpu *vcpu)
{
	return container_of(vcpu, struct vcpu_vmx, vcpu);
}

static inline struct lbr_desc *vcpu_to_lbr_desc(struct kvm_vcpu *vcpu)
{
	return &to_vmx(vcpu)->lbr_desc;
}

static inline struct x86_pmu_lbr *vcpu_to_lbr_records(struct kvm_vcpu *vcpu)
{
	return &vcpu_to_lbr_desc(vcpu)->records;
}

static inline bool intel_pmu_lbr_is_enabled(struct kvm_vcpu *vcpu)
{
	return !!vcpu_to_lbr_records(vcpu)->nr;
}

void intel_pmu_cross_mapped_check(struct kvm_pmu *pmu);
int intel_pmu_create_guest_lbr_event(struct kvm_vcpu *vcpu);
void vmx_passthrough_lbr_msrs(struct kvm_vcpu *vcpu);

static __always_inline unsigned long vmx_get_exit_qual(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	if (!kvm_register_test_and_mark_available(vcpu, VCPU_EXREG_EXIT_INFO_1))
		vmx->exit_qualification = vmcs_readl(EXIT_QUALIFICATION);

	return vmx->exit_qualification;
}

static __always_inline u32 vmx_get_intr_info(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	if (!kvm_register_test_and_mark_available(vcpu, VCPU_EXREG_EXIT_INFO_2))
		vmx->exit_intr_info = vmcs_read32(VM_EXIT_INTR_INFO);

	return vmx->exit_intr_info;
}

struct vmcs *alloc_vmcs_cpu(bool shadow, int cpu, gfp_t flags);
void free_vmcs(struct vmcs *vmcs);
int alloc_loaded_vmcs(struct loaded_vmcs *loaded_vmcs);
void free_loaded_vmcs(struct loaded_vmcs *loaded_vmcs);
void loaded_vmcs_clear(struct loaded_vmcs *loaded_vmcs);

static inline struct vmcs *alloc_vmcs(bool shadow)
{
	return alloc_vmcs_cpu(shadow, raw_smp_processor_id(),
			      GFP_KERNEL_ACCOUNT);
}

static inline bool vmx_has_waitpkg(struct vcpu_vmx *vmx)
{
	return secondary_exec_controls_get(vmx) &
		SECONDARY_EXEC_ENABLE_USR_WAIT_PAUSE;
}

static inline bool vmx_need_pf_intercept(struct kvm_vcpu *vcpu)
{
	if (!enable_ept)
		return true;

	return allow_smaller_maxphyaddr && cpuid_maxphyaddr(vcpu) < boot_cpu_data.x86_phys_bits;
}

static inline bool is_unrestricted_guest(struct kvm_vcpu *vcpu)
{
	return enable_unrestricted_guest && (!is_guest_mode(vcpu) ||
	    (secondary_exec_controls_get(to_vmx(vcpu)) &
	    SECONDARY_EXEC_UNRESTRICTED_GUEST));
}

bool __vmx_guest_state_valid(struct kvm_vcpu *vcpu);
static inline bool vmx_guest_state_valid(struct kvm_vcpu *vcpu)
{
	return is_unrestricted_guest(vcpu) || __vmx_guest_state_valid(vcpu);
}

void dump_vmcs(struct kvm_vcpu *vcpu);

static inline int vmx_get_instr_info_reg2(u32 vmx_instr_info)
{
	return (vmx_instr_info >> 28) & 0xf;
}

static inline bool vmx_can_use_ipiv(struct kvm_vcpu *vcpu)
{
	return  lapic_in_kernel(vcpu) && enable_ipiv;
}

static inline bool guest_cpuid_has_evmcs(struct kvm_vcpu *vcpu)
{
	/*
	 * eVMCS is exposed to the guest if Hyper-V is enabled in CPUID and
	 * eVMCS has been explicitly enabled by userspace.
	 */
	return vcpu->arch.hyperv_enabled &&
	       to_vmx(vcpu)->nested.enlightened_vmcs_enabled;
}

#endif /* __KVM_X86_VMX_H */
