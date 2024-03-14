/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/include/asm/traps.h
 *
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_TRAP_H
#define __ASM_TRAP_H

#include <linux/list.h>
#include <asm/esr.h>
#include <asm/sections.h>

struct pt_regs;

#ifdef CONFIG_ARMV8_DEPRECATED
bool try_emulate_armv8_deprecated(struct pt_regs *regs, u32 insn);
#else
static inline bool
try_emulate_armv8_deprecated(struct pt_regs *regs, u32 insn)
{
	return false;
}
#endif /* CONFIG_ARMV8_DEPRECATED */

void force_signal_inject(int signal, int code, unsigned long address, unsigned long err);
void arm64_notify_segfault(unsigned long addr);
void arm64_force_sig_fault(int signo, int code, unsigned long far, const char *str);
void arm64_force_sig_mceerr(int code, unsigned long far, short lsb, const char *str);
void arm64_force_sig_ptrace_errno_trap(int errno, unsigned long far, const char *str);

int early_brk64(unsigned long addr, unsigned long esr, struct pt_regs *regs);

/*
 * Move regs->pc to next instruction and do necessary setup before it
 * is executed.
 */
void arm64_skip_faulting_instruction(struct pt_regs *regs, unsigned long size);

static inline int __in_irqentry_text(unsigned long ptr)
{
	return ptr >= (unsigned long)&__irqentry_text_start &&
	       ptr < (unsigned long)&__irqentry_text_end;
}

static inline int in_entry_text(unsigned long ptr)
{
	return ptr >= (unsigned long)&__entry_text_start &&
	       ptr < (unsigned long)&__entry_text_end;
}

/*
 * CPUs with the RAS extensions have an Implementation-Defined-Syndrome bit
 * to indicate whether this ESR has a RAS encoding. CPUs without this feature
 * have a ISS-Valid bit in the same position.
 * If this bit is set, we know its not a RAS SError.
 * If its clear, we need to know if the CPU supports RAS. Uncategorized RAS
 * errors share the same encoding as an all-zeros encoding from a CPU that
 * doesn't support RAS.
 */
static inline bool arm64_is_ras_serror(unsigned long esr)
{
	WARN_ON(preemptible());

	if (esr & ESR_ELx_IDS)
		return false;

	if (this_cpu_has_cap(ARM64_HAS_RAS_EXTN))
		return true;
	else
		return false;
}

/*
 * Return the AET bits from a RAS SError's ESR.
 *
 * It is implementation defined whether Uncategorized errors are containable.
 * We treat them as Uncontainable.
 * Non-RAS SError's are reported as Uncontained/Uncategorized.
 */
static inline unsigned long arm64_ras_serror_get_severity(unsigned long esr)
{
	unsigned long aet = esr & ESR_ELx_AET;

	if (!arm64_is_ras_serror(esr)) {
		/* Not a RAS error, we can't interpret the ESR. */
		return ESR_ELx_AET_UC;
	}

	/*
	 * AET is RES0 if 'the value returned in the DFSC field is not
	 * [ESR_ELx_FSC_SERROR]'
	 */
	if ((esr & ESR_ELx_FSC) != ESR_ELx_FSC_SERROR) {
		/* No severity information : Uncategorized */
		return ESR_ELx_AET_UC;
	}

	return aet;
}

bool arm64_is_fatal_ras_serror(struct pt_regs *regs, unsigned long esr);
void __noreturn arm64_serror_panic(struct pt_regs *regs, unsigned long esr);
#endif
