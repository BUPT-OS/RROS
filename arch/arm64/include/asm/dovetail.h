/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>.
 */
#ifndef _ASM_ARM64_DOVETAIL_H
#define _ASM_ARM64_DOVETAIL_H

#include <asm/fpsimd.h>

/* ARM64 traps */
#define ARM64_TRAP_ACCESS	0	/* Data or instruction access exception */
#define ARM64_TRAP_ALIGN	1	/* SP/PC alignment abort */
#define ARM64_TRAP_SEA		2	/* Synchronous external abort */
#define ARM64_TRAP_DEBUG	3	/* Debug trap */
#define ARM64_TRAP_UNDI		4	/* Undefined instruction */
#define ARM64_TRAP_UNDSE	5	/* Undefined synchronous exception */
#define ARM64_TRAP_FPE		6	/* FPSIMD exception */
#define ARM64_TRAP_SVE		7	/* SVE access trap */
#define ARM64_TRAP_BTI		8	/* Branch target identification */
#define ARM64_TRAP_SME		9	/* SME access trap */

#ifdef CONFIG_DOVETAIL

static inline void arch_dovetail_exec_prepare(void)
{ }

static inline void arch_dovetail_switch_prepare(bool leave_inband)
{ }

static inline void arch_dovetail_switch_finish(bool enter_inband)
{
	fpsimd_restore_current_oob();
}

/*
 * 172 is __NR_prctl from unistd32 in ARM32 mode, without #inclusion
 * hell. At the end of the day, this number is written in stone to
 * honor the ABI stability promise anyway.
 */
#define arch_dovetail_is_syscall(__nr)	\
	(is_compat_task() ? (__nr) == 172 : (__nr) == __NR_prctl)

#endif

/*
 * Pass the trap event to the companion core. Return true if running
 * in-band afterwards.
 */
#define mark_cond_trap_entry(__trapnr, __regs)			\
	({							\
	  	bool __ret;					\
		oob_trap_notify(__trapnr, __regs);		\
		__ret = running_inband();			\
		if (!__ret)					\
			oob_trap_unwind(__trapnr, __regs);	\
		__ret;						\
	})

/*
 * Pass the trap event to the companion core. We expect the current
 * context to be running on the in-band stage upon return so that our
 * caller can tread on common kernel code.
 */
#define mark_trap_entry(__trapnr, __regs)				\
	do {								\
		bool __ret = mark_cond_trap_entry(__trapnr, __regs);	\
		BUG_ON(dovetail_debug() && !__ret);			\
	} while (0)

#define mark_trap_exit(__trapnr, __regs)				\
	oob_trap_unwind(__trapnr, __regs)

#endif /* _ASM_ARM64_DOVETAIL_H */
