/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2016 Philippe Gerum.
 */
#ifndef _ASM_ARM_DOVETAIL_H
#define _ASM_ARM_DOVETAIL_H

/* ARM traps */
#define ARM_TRAP_ACCESS		0	/* Data or instruction access exception */
#define ARM_TRAP_SECTION	1	/* Section fault */
#define ARM_TRAP_DABT		2	/* Generic data abort */
#define ARM_TRAP_PABT		3	/* Prefetch abort */
#define ARM_TRAP_BREAK		4	/* Instruction breakpoint */
#define ARM_TRAP_FPU		5	/* Floating point exception */
#define ARM_TRAP_VFP		6	/* VFP floating point exception */
#define ARM_TRAP_UNDEFINSTR	7	/* Undefined instruction */
#define ARM_TRAP_ALIGNMENT	8	/* Unaligned access exception */

#if !defined(__ASSEMBLY__)

#ifdef CONFIG_DOVETAIL

static inline void arch_dovetail_exec_prepare(void)
{ }

static inline void arch_dovetail_switch_prepare(bool leave_inband)
{ }

static inline void arch_dovetail_switch_finish(bool enter_inband)
{ }

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

#endif	/* !__ASSEMBLY__ */

#endif /* _ASM_ARM_DOVETAIL_H */
