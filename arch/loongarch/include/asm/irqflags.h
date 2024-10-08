/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */
#ifndef _ASM_IRQFLAGS_H
#define _ASM_IRQFLAGS_H

#ifndef __ASSEMBLY__

#include <linux/compiler.h>
#include <linux/stringify.h>
#include <asm/loongarch.h>
#include <asm/irq_pipeline.h>

static inline void native_irq_enable(void)
{
	u32 flags = CSR_CRMD_IE;
	__asm__ __volatile__(
		"csrxchg %[val], %[mask], %[reg]\n\t"
		: [val] "+r" (flags)
		: [mask] "r" (CSR_CRMD_IE), [reg] "i" (LOONGARCH_CSR_CRMD)
		: "memory");
}

static inline void native_irq_disable(void)
{
	u32 flags = 0;
	__asm__ __volatile__(
		"csrxchg %[val], %[mask], %[reg]\n\t"
		: [val] "+r" (flags)
		: [mask] "r" (CSR_CRMD_IE), [reg] "i" (LOONGARCH_CSR_CRMD)
		: "memory");
}

static inline void native_irq_sync(void)
{
	native_irq_enable();
	barrier();
	native_irq_disable();
}

static inline unsigned long native_irq_save(void)
{
	u32 flags = 0;
	__asm__ __volatile__(
		"csrxchg %[val], %[mask], %[reg]\n\t"
		: [val] "+r" (flags)
		: [mask] "r" (CSR_CRMD_IE), [reg] "i" (LOONGARCH_CSR_CRMD)
		: "memory");
	return flags;
}

static inline void native_irq_restore(unsigned long flags)
{
	__asm__ __volatile__(
		"csrxchg %[val], %[mask], %[reg]\n\t"
		: [val] "+r" (flags)
		: [mask] "r" (CSR_CRMD_IE), [reg] "i" (LOONGARCH_CSR_CRMD)
		: "memory");
}

static inline unsigned long native_save_flags(void)
{
	u32 flags;
	__asm__ __volatile__(
		"csrrd %[val], %[reg]\n\t"
		: [val] "=r" (flags)
		: [reg] "i" (LOONGARCH_CSR_CRMD)
		: "memory");
	return flags;
}


static inline int native_irqs_disabled(void)
{
	return native_irqs_disabled_flags(native_save_flags());
}

#endif /* #ifndef __ASSEMBLY__ */

/*
 * Do the CPU's IRQ-state tracing from assembly code.
 */
#ifdef CONFIG_TRACE_IRQFLAGS
/* Reload some registers clobbered by trace_hardirqs_on */
# define TRACE_IRQS_RELOAD_REGS \
	LONG_L  $r11, sp, PT_R11; \
	LONG_L  $r10, sp, PT_R10; \
	LONG_L  $r9, sp, PT_R9; \
	LONG_L  $r8, sp, PT_R8; \
	LONG_L  $r7, sp, PT_R7; \
	LONG_L  $r6, sp, PT_R6; \
	LONG_L  $r5, sp, PT_R5; \
	LONG_L  $r4, sp, PT_R4
# define TRACE_IRQS_ON \
	CLI;    /* make sure trace_hardirqs_on() is called in kernel level */ \
	la.abs  t0, trace_hardirqs_on_pipelined; \
	jirl    ra, t0, 0
# define TRACE_IRQS_ON_RELOAD \
	TRACE_IRQS_ON; \
	TRACE_IRQS_RELOAD_REGS
# define TRACE_IRQS_OFF \
	la.abs  t0, trace_hardirqs_off_pipelined; \
	jirl    ra, t0, 0
#else
# define TRACE_IRQS_ON
# define TRACE_IRQS_ON_RELOAD
# define TRACE_IRQS_OFF
#endif

#endif /* _ASM_IRQFLAGS_H */
