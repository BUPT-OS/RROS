/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SVM_H
#define __SVM_H

#include <uapi/asm/svm.h>
#include <uapi/asm/kvm.h>

#include <asm/hyperv-tlfs.h>

/*
 * 32-bit intercept words in the VMCB Control Area, starting
 * at Byte offset 000h.
 */

enum intercept_words {
	INTERCEPT_CR = 0,
	INTERCEPT_DR,
	INTERCEPT_EXCEPTION,
	INTERCEPT_WORD3,
	INTERCEPT_WORD4,
	INTERCEPT_WORD5,
	MAX_INTERCEPT,
};

enum {
	/* Byte offset 000h (word 0) */
	INTERCEPT_CR0_READ = 0,
	INTERCEPT_CR3_READ = 3,
	INTERCEPT_CR4_READ = 4,
	INTERCEPT_CR8_READ = 8,
	INTERCEPT_CR0_WRITE = 16,
	INTERCEPT_CR3_WRITE = 16 + 3,
	INTERCEPT_CR4_WRITE = 16 + 4,
	INTERCEPT_CR8_WRITE = 16 + 8,
	/* Byte offset 004h (word 1) */
	INTERCEPT_DR0_READ = 32,
	INTERCEPT_DR1_READ,
	INTERCEPT_DR2_READ,
	INTERCEPT_DR3_READ,
	INTERCEPT_DR4_READ,
	INTERCEPT_DR5_READ,
	INTERCEPT_DR6_READ,
	INTERCEPT_DR7_READ,
	INTERCEPT_DR0_WRITE = 48,
	INTERCEPT_DR1_WRITE,
	INTERCEPT_DR2_WRITE,
	INTERCEPT_DR3_WRITE,
	INTERCEPT_DR4_WRITE,
	INTERCEPT_DR5_WRITE,
	INTERCEPT_DR6_WRITE,
	INTERCEPT_DR7_WRITE,
	/* Byte offset 008h (word 2) */
	INTERCEPT_EXCEPTION_OFFSET = 64,
	/* Byte offset 00Ch (word 3) */
	INTERCEPT_INTR = 96,
	INTERCEPT_NMI,
	INTERCEPT_SMI,
	INTERCEPT_INIT,
	INTERCEPT_VINTR,
	INTERCEPT_SELECTIVE_CR0,
	INTERCEPT_STORE_IDTR,
	INTERCEPT_STORE_GDTR,
	INTERCEPT_STORE_LDTR,
	INTERCEPT_STORE_TR,
	INTERCEPT_LOAD_IDTR,
	INTERCEPT_LOAD_GDTR,
	INTERCEPT_LOAD_LDTR,
	INTERCEPT_LOAD_TR,
	INTERCEPT_RDTSC,
	INTERCEPT_RDPMC,
	INTERCEPT_PUSHF,
	INTERCEPT_POPF,
	INTERCEPT_CPUID,
	INTERCEPT_RSM,
	INTERCEPT_IRET,
	INTERCEPT_INTn,
	INTERCEPT_INVD,
	INTERCEPT_PAUSE,
	INTERCEPT_HLT,
	INTERCEPT_INVLPG,
	INTERCEPT_INVLPGA,
	INTERCEPT_IOIO_PROT,
	INTERCEPT_MSR_PROT,
	INTERCEPT_TASK_SWITCH,
	INTERCEPT_FERR_FREEZE,
	INTERCEPT_SHUTDOWN,
	/* Byte offset 010h (word 4) */
	INTERCEPT_VMRUN = 128,
	INTERCEPT_VMMCALL,
	INTERCEPT_VMLOAD,
	INTERCEPT_VMSAVE,
	INTERCEPT_STGI,
	INTERCEPT_CLGI,
	INTERCEPT_SKINIT,
	INTERCEPT_RDTSCP,
	INTERCEPT_ICEBP,
	INTERCEPT_WBINVD,
	INTERCEPT_MONITOR,
	INTERCEPT_MWAIT,
	INTERCEPT_MWAIT_COND,
	INTERCEPT_XSETBV,
	INTERCEPT_RDPRU,
	TRAP_EFER_WRITE,
	TRAP_CR0_WRITE,
	TRAP_CR1_WRITE,
	TRAP_CR2_WRITE,
	TRAP_CR3_WRITE,
	TRAP_CR4_WRITE,
	TRAP_CR5_WRITE,
	TRAP_CR6_WRITE,
	TRAP_CR7_WRITE,
	TRAP_CR8_WRITE,
	/* Byte offset 014h (word 5) */
	INTERCEPT_INVLPGB = 160,
	INTERCEPT_INVLPGB_ILLEGAL,
	INTERCEPT_INVPCID,
	INTERCEPT_MCOMMIT,
	INTERCEPT_TLBSYNC,
};


struct __attribute__ ((__packed__)) vmcb_control_area {
	u32 intercepts[MAX_INTERCEPT];
	u32 reserved_1[15 - MAX_INTERCEPT];
	u16 pause_filter_thresh;
	u16 pause_filter_count;
	u64 iopm_base_pa;
	u64 msrpm_base_pa;
	u64 tsc_offset;
	u32 asid;
	u8 tlb_ctl;
	u8 reserved_2[3];
	u32 int_ctl;
	u32 int_vector;
	u32 int_state;
	u8 reserved_3[4];
	u32 exit_code;
	u32 exit_code_hi;
	u64 exit_info_1;
	u64 exit_info_2;
	u32 exit_int_info;
	u32 exit_int_info_err;
	u64 nested_ctl;
	u64 avic_vapic_bar;
	u64 ghcb_gpa;
	u32 event_inj;
	u32 event_inj_err;
	u64 nested_cr3;
	u64 virt_ext;
	u32 clean;
	u32 reserved_5;
	u64 next_rip;
	u8 insn_len;
	u8 insn_bytes[15];
	u64 avic_backing_page;	/* Offset 0xe0 */
	u8 reserved_6[8];	/* Offset 0xe8 */
	u64 avic_logical_id;	/* Offset 0xf0 */
	u64 avic_physical_id;	/* Offset 0xf8 */
	u8 reserved_7[8];
	u64 vmsa_pa;		/* Used for an SEV-ES guest */
	u8 reserved_8[720];
	/*
	 * Offset 0x3e0, 32 bytes reserved
	 * for use by hypervisor/software.
	 */
	union {
		struct hv_vmcb_enlightenments hv_enlightenments;
		u8 reserved_sw[32];
	};
};


#define TLB_CONTROL_DO_NOTHING 0
#define TLB_CONTROL_FLUSH_ALL_ASID 1
#define TLB_CONTROL_FLUSH_ASID 3
#define TLB_CONTROL_FLUSH_ASID_LOCAL 7

#define V_TPR_MASK 0x0f

#define V_IRQ_SHIFT 8
#define V_IRQ_MASK (1 << V_IRQ_SHIFT)

#define V_GIF_SHIFT 9
#define V_GIF_MASK (1 << V_GIF_SHIFT)

#define V_NMI_PENDING_SHIFT 11
#define V_NMI_PENDING_MASK (1 << V_NMI_PENDING_SHIFT)

#define V_NMI_BLOCKING_SHIFT 12
#define V_NMI_BLOCKING_MASK (1 << V_NMI_BLOCKING_SHIFT)

#define V_INTR_PRIO_SHIFT 16
#define V_INTR_PRIO_MASK (0x0f << V_INTR_PRIO_SHIFT)

#define V_IGN_TPR_SHIFT 20
#define V_IGN_TPR_MASK (1 << V_IGN_TPR_SHIFT)

#define V_IRQ_INJECTION_BITS_MASK (V_IRQ_MASK | V_INTR_PRIO_MASK | V_IGN_TPR_MASK)

#define V_INTR_MASKING_SHIFT 24
#define V_INTR_MASKING_MASK (1 << V_INTR_MASKING_SHIFT)

#define V_GIF_ENABLE_SHIFT 25
#define V_GIF_ENABLE_MASK (1 << V_GIF_ENABLE_SHIFT)

#define V_NMI_ENABLE_SHIFT 26
#define V_NMI_ENABLE_MASK (1 << V_NMI_ENABLE_SHIFT)

#define AVIC_ENABLE_SHIFT 31
#define AVIC_ENABLE_MASK (1 << AVIC_ENABLE_SHIFT)

#define X2APIC_MODE_SHIFT 30
#define X2APIC_MODE_MASK (1 << X2APIC_MODE_SHIFT)

#define LBR_CTL_ENABLE_MASK BIT_ULL(0)
#define VIRTUAL_VMLOAD_VMSAVE_ENABLE_MASK BIT_ULL(1)

#define SVM_INTERRUPT_SHADOW_MASK	BIT_ULL(0)
#define SVM_GUEST_INTERRUPT_MASK	BIT_ULL(1)

#define SVM_IOIO_STR_SHIFT 2
#define SVM_IOIO_REP_SHIFT 3
#define SVM_IOIO_SIZE_SHIFT 4
#define SVM_IOIO_ASIZE_SHIFT 7

#define SVM_IOIO_TYPE_MASK 1
#define SVM_IOIO_STR_MASK (1 << SVM_IOIO_STR_SHIFT)
#define SVM_IOIO_REP_MASK (1 << SVM_IOIO_REP_SHIFT)
#define SVM_IOIO_SIZE_MASK (7 << SVM_IOIO_SIZE_SHIFT)
#define SVM_IOIO_ASIZE_MASK (7 << SVM_IOIO_ASIZE_SHIFT)

#define SVM_VM_CR_VALID_MASK	0x001fULL
#define SVM_VM_CR_SVM_LOCK_MASK 0x0008ULL
#define SVM_VM_CR_SVM_DIS_MASK  0x0010ULL

#define SVM_NESTED_CTL_NP_ENABLE	BIT(0)
#define SVM_NESTED_CTL_SEV_ENABLE	BIT(1)
#define SVM_NESTED_CTL_SEV_ES_ENABLE	BIT(2)


#define SVM_TSC_RATIO_RSVD	0xffffff0000000000ULL
#define SVM_TSC_RATIO_MIN	0x0000000000000001ULL
#define SVM_TSC_RATIO_MAX	0x000000ffffffffffULL
#define SVM_TSC_RATIO_DEFAULT	0x0100000000ULL


/* AVIC */
#define AVIC_LOGICAL_ID_ENTRY_GUEST_PHYSICAL_ID_MASK	(0xFFULL)
#define AVIC_LOGICAL_ID_ENTRY_VALID_BIT			31
#define AVIC_LOGICAL_ID_ENTRY_VALID_MASK		(1 << 31)

#define AVIC_PHYSICAL_ID_ENTRY_HOST_PHYSICAL_ID_MASK	GENMASK_ULL(11, 0)
#define AVIC_PHYSICAL_ID_ENTRY_BACKING_PAGE_MASK	(0xFFFFFFFFFFULL << 12)
#define AVIC_PHYSICAL_ID_ENTRY_IS_RUNNING_MASK		(1ULL << 62)
#define AVIC_PHYSICAL_ID_ENTRY_VALID_MASK		(1ULL << 63)
#define AVIC_PHYSICAL_ID_TABLE_SIZE_MASK		(0xFFULL)

#define AVIC_DOORBELL_PHYSICAL_ID_MASK			GENMASK_ULL(11, 0)

#define VMCB_AVIC_APIC_BAR_MASK				0xFFFFFFFFFF000ULL

#define AVIC_UNACCEL_ACCESS_WRITE_MASK		1
#define AVIC_UNACCEL_ACCESS_OFFSET_MASK		0xFF0
#define AVIC_UNACCEL_ACCESS_VECTOR_MASK		0xFFFFFFFF

enum avic_ipi_failure_cause {
	AVIC_IPI_FAILURE_INVALID_INT_TYPE,
	AVIC_IPI_FAILURE_TARGET_NOT_RUNNING,
	AVIC_IPI_FAILURE_INVALID_TARGET,
	AVIC_IPI_FAILURE_INVALID_BACKING_PAGE,
};

#define AVIC_PHYSICAL_MAX_INDEX_MASK	GENMASK_ULL(8, 0)

/*
 * For AVIC, the max index allowed for physical APIC ID table is 0xfe (254), as
 * 0xff is a broadcast to all CPUs, i.e. can't be targeted individually.
 */
#define AVIC_MAX_PHYSICAL_ID		0XFEULL

/*
 * For x2AVIC, the max index allowed for physical APIC ID table is 0x1ff (511).
 */
#define X2AVIC_MAX_PHYSICAL_ID		0x1FFUL

static_assert((AVIC_MAX_PHYSICAL_ID & AVIC_PHYSICAL_MAX_INDEX_MASK) == AVIC_MAX_PHYSICAL_ID);
static_assert((X2AVIC_MAX_PHYSICAL_ID & AVIC_PHYSICAL_MAX_INDEX_MASK) == X2AVIC_MAX_PHYSICAL_ID);

#define AVIC_HPA_MASK	~((0xFFFULL << 52) | 0xFFF)

#define SVM_SEV_FEAT_DEBUG_SWAP                        BIT(5)

struct vmcb_seg {
	u16 selector;
	u16 attrib;
	u32 limit;
	u64 base;
} __packed;

/* Save area definition for legacy and SEV-MEM guests */
struct vmcb_save_area {
	struct vmcb_seg es;
	struct vmcb_seg cs;
	struct vmcb_seg ss;
	struct vmcb_seg ds;
	struct vmcb_seg fs;
	struct vmcb_seg gs;
	struct vmcb_seg gdtr;
	struct vmcb_seg ldtr;
	struct vmcb_seg idtr;
	struct vmcb_seg tr;
	/* Reserved fields are named following their struct offset */
	u8 reserved_0xa0[42];
	u8 vmpl;
	u8 cpl;
	u8 reserved_0xcc[4];
	u64 efer;
	u8 reserved_0xd8[112];
	u64 cr4;
	u64 cr3;
	u64 cr0;
	u64 dr7;
	u64 dr6;
	u64 rflags;
	u64 rip;
	u8 reserved_0x180[88];
	u64 rsp;
	u64 s_cet;
	u64 ssp;
	u64 isst_addr;
	u64 rax;
	u64 star;
	u64 lstar;
	u64 cstar;
	u64 sfmask;
	u64 kernel_gs_base;
	u64 sysenter_cs;
	u64 sysenter_esp;
	u64 sysenter_eip;
	u64 cr2;
	u8 reserved_0x248[32];
	u64 g_pat;
	u64 dbgctl;
	u64 br_from;
	u64 br_to;
	u64 last_excp_from;
	u64 last_excp_to;
	u8 reserved_0x298[72];
	u64 spec_ctrl;		/* Guest version of SPEC_CTRL at 0x2E0 */
} __packed;

/* Save area definition for SEV-ES and SEV-SNP guests */
struct sev_es_save_area {
	struct vmcb_seg es;
	struct vmcb_seg cs;
	struct vmcb_seg ss;
	struct vmcb_seg ds;
	struct vmcb_seg fs;
	struct vmcb_seg gs;
	struct vmcb_seg gdtr;
	struct vmcb_seg ldtr;
	struct vmcb_seg idtr;
	struct vmcb_seg tr;
	u64 vmpl0_ssp;
	u64 vmpl1_ssp;
	u64 vmpl2_ssp;
	u64 vmpl3_ssp;
	u64 u_cet;
	u8 reserved_0xc8[2];
	u8 vmpl;
	u8 cpl;
	u8 reserved_0xcc[4];
	u64 efer;
	u8 reserved_0xd8[104];
	u64 xss;
	u64 cr4;
	u64 cr3;
	u64 cr0;
	u64 dr7;
	u64 dr6;
	u64 rflags;
	u64 rip;
	u64 dr0;
	u64 dr1;
	u64 dr2;
	u64 dr3;
	u64 dr0_addr_mask;
	u64 dr1_addr_mask;
	u64 dr2_addr_mask;
	u64 dr3_addr_mask;
	u8 reserved_0x1c0[24];
	u64 rsp;
	u64 s_cet;
	u64 ssp;
	u64 isst_addr;
	u64 rax;
	u64 star;
	u64 lstar;
	u64 cstar;
	u64 sfmask;
	u64 kernel_gs_base;
	u64 sysenter_cs;
	u64 sysenter_esp;
	u64 sysenter_eip;
	u64 cr2;
	u8 reserved_0x248[32];
	u64 g_pat;
	u64 dbgctl;
	u64 br_from;
	u64 br_to;
	u64 last_excp_from;
	u64 last_excp_to;
	u8 reserved_0x298[80];
	u32 pkru;
	u32 tsc_aux;
	u8 reserved_0x2f0[24];
	u64 rcx;
	u64 rdx;
	u64 rbx;
	u64 reserved_0x320;	/* rsp already available at 0x01d8 */
	u64 rbp;
	u64 rsi;
	u64 rdi;
	u64 r8;
	u64 r9;
	u64 r10;
	u64 r11;
	u64 r12;
	u64 r13;
	u64 r14;
	u64 r15;
	u8 reserved_0x380[16];
	u64 guest_exit_info_1;
	u64 guest_exit_info_2;
	u64 guest_exit_int_info;
	u64 guest_nrip;
	u64 sev_features;
	u64 vintr_ctrl;
	u64 guest_exit_code;
	u64 virtual_tom;
	u64 tlb_id;
	u64 pcpu_id;
	u64 event_inj;
	u64 xcr0;
	u8 reserved_0x3f0[16];

	/* Floating point area */
	u64 x87_dp;
	u32 mxcsr;
	u16 x87_ftw;
	u16 x87_fsw;
	u16 x87_fcw;
	u16 x87_fop;
	u16 x87_ds;
	u16 x87_cs;
	u64 x87_rip;
	u8 fpreg_x87[80];
	u8 fpreg_xmm[256];
	u8 fpreg_ymm[256];
} __packed;

struct ghcb_save_area {
	u8 reserved_0x0[203];
	u8 cpl;
	u8 reserved_0xcc[116];
	u64 xss;
	u8 reserved_0x148[24];
	u64 dr7;
	u8 reserved_0x168[16];
	u64 rip;
	u8 reserved_0x180[88];
	u64 rsp;
	u8 reserved_0x1e0[24];
	u64 rax;
	u8 reserved_0x200[264];
	u64 rcx;
	u64 rdx;
	u64 rbx;
	u8 reserved_0x320[8];
	u64 rbp;
	u64 rsi;
	u64 rdi;
	u64 r8;
	u64 r9;
	u64 r10;
	u64 r11;
	u64 r12;
	u64 r13;
	u64 r14;
	u64 r15;
	u8 reserved_0x380[16];
	u64 sw_exit_code;
	u64 sw_exit_info_1;
	u64 sw_exit_info_2;
	u64 sw_scratch;
	u8 reserved_0x3b0[56];
	u64 xcr0;
	u8 valid_bitmap[16];
	u64 x87_state_gpa;
} __packed;

#define GHCB_SHARED_BUF_SIZE	2032

struct ghcb {
	struct ghcb_save_area save;
	u8 reserved_save[2048 - sizeof(struct ghcb_save_area)];

	u8 shared_buffer[GHCB_SHARED_BUF_SIZE];

	u8 reserved_0xff0[10];
	u16 protocol_version;	/* negotiated SEV-ES/GHCB protocol version */
	u32 ghcb_usage;
} __packed;


#define EXPECTED_VMCB_SAVE_AREA_SIZE		744
#define EXPECTED_GHCB_SAVE_AREA_SIZE		1032
#define EXPECTED_SEV_ES_SAVE_AREA_SIZE		1648
#define EXPECTED_VMCB_CONTROL_AREA_SIZE		1024
#define EXPECTED_GHCB_SIZE			PAGE_SIZE

#define BUILD_BUG_RESERVED_OFFSET(x, y) \
	ASSERT_STRUCT_OFFSET(struct x, reserved ## _ ## y, y)

static inline void __unused_size_checks(void)
{
	BUILD_BUG_ON(sizeof(struct vmcb_save_area)	!= EXPECTED_VMCB_SAVE_AREA_SIZE);
	BUILD_BUG_ON(sizeof(struct ghcb_save_area)	!= EXPECTED_GHCB_SAVE_AREA_SIZE);
	BUILD_BUG_ON(sizeof(struct sev_es_save_area)	!= EXPECTED_SEV_ES_SAVE_AREA_SIZE);
	BUILD_BUG_ON(sizeof(struct vmcb_control_area)	!= EXPECTED_VMCB_CONTROL_AREA_SIZE);
	BUILD_BUG_ON(sizeof(struct ghcb)		!= EXPECTED_GHCB_SIZE);

	/* Check offsets of reserved fields */

	BUILD_BUG_RESERVED_OFFSET(vmcb_save_area, 0xa0);
	BUILD_BUG_RESERVED_OFFSET(vmcb_save_area, 0xcc);
	BUILD_BUG_RESERVED_OFFSET(vmcb_save_area, 0xd8);
	BUILD_BUG_RESERVED_OFFSET(vmcb_save_area, 0x180);
	BUILD_BUG_RESERVED_OFFSET(vmcb_save_area, 0x248);
	BUILD_BUG_RESERVED_OFFSET(vmcb_save_area, 0x298);

	BUILD_BUG_RESERVED_OFFSET(sev_es_save_area, 0xc8);
	BUILD_BUG_RESERVED_OFFSET(sev_es_save_area, 0xcc);
	BUILD_BUG_RESERVED_OFFSET(sev_es_save_area, 0xd8);
	BUILD_BUG_RESERVED_OFFSET(sev_es_save_area, 0x1c0);
	BUILD_BUG_RESERVED_OFFSET(sev_es_save_area, 0x248);
	BUILD_BUG_RESERVED_OFFSET(sev_es_save_area, 0x298);
	BUILD_BUG_RESERVED_OFFSET(sev_es_save_area, 0x2f0);
	BUILD_BUG_RESERVED_OFFSET(sev_es_save_area, 0x320);
	BUILD_BUG_RESERVED_OFFSET(sev_es_save_area, 0x380);
	BUILD_BUG_RESERVED_OFFSET(sev_es_save_area, 0x3f0);

	BUILD_BUG_RESERVED_OFFSET(ghcb_save_area, 0x0);
	BUILD_BUG_RESERVED_OFFSET(ghcb_save_area, 0xcc);
	BUILD_BUG_RESERVED_OFFSET(ghcb_save_area, 0x148);
	BUILD_BUG_RESERVED_OFFSET(ghcb_save_area, 0x168);
	BUILD_BUG_RESERVED_OFFSET(ghcb_save_area, 0x180);
	BUILD_BUG_RESERVED_OFFSET(ghcb_save_area, 0x1e0);
	BUILD_BUG_RESERVED_OFFSET(ghcb_save_area, 0x200);
	BUILD_BUG_RESERVED_OFFSET(ghcb_save_area, 0x320);
	BUILD_BUG_RESERVED_OFFSET(ghcb_save_area, 0x380);
	BUILD_BUG_RESERVED_OFFSET(ghcb_save_area, 0x3b0);

	BUILD_BUG_RESERVED_OFFSET(ghcb, 0xff0);
}

struct vmcb {
	struct vmcb_control_area control;
	struct vmcb_save_area save;
} __packed;

#define SVM_CPUID_FUNC 0x8000000a

#define SVM_VM_CR_SVM_DISABLE 4

#define SVM_SELECTOR_S_SHIFT 4
#define SVM_SELECTOR_DPL_SHIFT 5
#define SVM_SELECTOR_P_SHIFT 7
#define SVM_SELECTOR_AVL_SHIFT 8
#define SVM_SELECTOR_L_SHIFT 9
#define SVM_SELECTOR_DB_SHIFT 10
#define SVM_SELECTOR_G_SHIFT 11

#define SVM_SELECTOR_TYPE_MASK (0xf)
#define SVM_SELECTOR_S_MASK (1 << SVM_SELECTOR_S_SHIFT)
#define SVM_SELECTOR_DPL_MASK (3 << SVM_SELECTOR_DPL_SHIFT)
#define SVM_SELECTOR_P_MASK (1 << SVM_SELECTOR_P_SHIFT)
#define SVM_SELECTOR_AVL_MASK (1 << SVM_SELECTOR_AVL_SHIFT)
#define SVM_SELECTOR_L_MASK (1 << SVM_SELECTOR_L_SHIFT)
#define SVM_SELECTOR_DB_MASK (1 << SVM_SELECTOR_DB_SHIFT)
#define SVM_SELECTOR_G_MASK (1 << SVM_SELECTOR_G_SHIFT)

#define SVM_SELECTOR_WRITE_MASK (1 << 1)
#define SVM_SELECTOR_READ_MASK SVM_SELECTOR_WRITE_MASK
#define SVM_SELECTOR_CODE_MASK (1 << 3)

#define SVM_EVTINJ_VEC_MASK 0xff

#define SVM_EVTINJ_TYPE_SHIFT 8
#define SVM_EVTINJ_TYPE_MASK (7 << SVM_EVTINJ_TYPE_SHIFT)

#define SVM_EVTINJ_TYPE_INTR (0 << SVM_EVTINJ_TYPE_SHIFT)
#define SVM_EVTINJ_TYPE_NMI (2 << SVM_EVTINJ_TYPE_SHIFT)
#define SVM_EVTINJ_TYPE_EXEPT (3 << SVM_EVTINJ_TYPE_SHIFT)
#define SVM_EVTINJ_TYPE_SOFT (4 << SVM_EVTINJ_TYPE_SHIFT)

#define SVM_EVTINJ_VALID (1 << 31)
#define SVM_EVTINJ_VALID_ERR (1 << 11)

#define SVM_EXITINTINFO_VEC_MASK SVM_EVTINJ_VEC_MASK
#define SVM_EXITINTINFO_TYPE_MASK SVM_EVTINJ_TYPE_MASK

#define	SVM_EXITINTINFO_TYPE_INTR SVM_EVTINJ_TYPE_INTR
#define	SVM_EXITINTINFO_TYPE_NMI SVM_EVTINJ_TYPE_NMI
#define	SVM_EXITINTINFO_TYPE_EXEPT SVM_EVTINJ_TYPE_EXEPT
#define	SVM_EXITINTINFO_TYPE_SOFT SVM_EVTINJ_TYPE_SOFT

#define SVM_EXITINTINFO_VALID SVM_EVTINJ_VALID
#define SVM_EXITINTINFO_VALID_ERR SVM_EVTINJ_VALID_ERR

#define SVM_EXITINFOSHIFT_TS_REASON_IRET 36
#define SVM_EXITINFOSHIFT_TS_REASON_JMP 38
#define SVM_EXITINFOSHIFT_TS_HAS_ERROR_CODE 44

#define SVM_EXITINFO_REG_MASK 0x0F

#define SVM_CR0_SELECTIVE_MASK (X86_CR0_TS | X86_CR0_MP)

/* GHCB Accessor functions */

#define GHCB_BITMAP_IDX(field)							\
	(offsetof(struct ghcb_save_area, field) / sizeof(u64))

#define DEFINE_GHCB_ACCESSORS(field)						\
	static __always_inline bool ghcb_##field##_is_valid(const struct ghcb *ghcb) \
	{									\
		return test_bit(GHCB_BITMAP_IDX(field),				\
				(unsigned long *)&ghcb->save.valid_bitmap);	\
	}									\
										\
	static __always_inline u64 ghcb_get_##field(struct ghcb *ghcb)		\
	{									\
		return ghcb->save.field;					\
	}									\
										\
	static __always_inline u64 ghcb_get_##field##_if_valid(struct ghcb *ghcb) \
	{									\
		return ghcb_##field##_is_valid(ghcb) ? ghcb->save.field : 0;	\
	}									\
										\
	static __always_inline void ghcb_set_##field(struct ghcb *ghcb, u64 value) \
	{									\
		__set_bit(GHCB_BITMAP_IDX(field),				\
			  (unsigned long *)&ghcb->save.valid_bitmap);		\
		ghcb->save.field = value;					\
	}

DEFINE_GHCB_ACCESSORS(cpl)
DEFINE_GHCB_ACCESSORS(rip)
DEFINE_GHCB_ACCESSORS(rsp)
DEFINE_GHCB_ACCESSORS(rax)
DEFINE_GHCB_ACCESSORS(rcx)
DEFINE_GHCB_ACCESSORS(rdx)
DEFINE_GHCB_ACCESSORS(rbx)
DEFINE_GHCB_ACCESSORS(rbp)
DEFINE_GHCB_ACCESSORS(rsi)
DEFINE_GHCB_ACCESSORS(rdi)
DEFINE_GHCB_ACCESSORS(r8)
DEFINE_GHCB_ACCESSORS(r9)
DEFINE_GHCB_ACCESSORS(r10)
DEFINE_GHCB_ACCESSORS(r11)
DEFINE_GHCB_ACCESSORS(r12)
DEFINE_GHCB_ACCESSORS(r13)
DEFINE_GHCB_ACCESSORS(r14)
DEFINE_GHCB_ACCESSORS(r15)
DEFINE_GHCB_ACCESSORS(sw_exit_code)
DEFINE_GHCB_ACCESSORS(sw_exit_info_1)
DEFINE_GHCB_ACCESSORS(sw_exit_info_2)
DEFINE_GHCB_ACCESSORS(sw_scratch)
DEFINE_GHCB_ACCESSORS(xcr0)

#endif
