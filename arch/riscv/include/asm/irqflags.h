/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Regents of the University of California
 */


#ifndef _ASM_RISCV_IRQFLAGS_H
#define _ASM_RISCV_IRQFLAGS_H

#include <asm/processor.h>
#include <asm/csr.h>

/* read interrupt enabled status */
static inline unsigned long native_save_flags(void)
{
	return csr_read(CSR_STATUS);
}

/* unconditionally enable interrupts */
static inline void native_irq_enable(void)
{
	csr_set(CSR_STATUS, SR_IE);
}

/* unconditionally disable interrupts */
static inline void native_irq_disable(void)
{
	csr_clear(CSR_STATUS, SR_IE);
}

static inline void native_irq_sync()
{
	native_irq_enable();
	asm volatile ("fence" : : : "memory");
	native_irq_disable();
}

/* get status and disable interrupts */
static inline unsigned long native_irq_save(void)
{
	return csr_read_clear(CSR_STATUS, SR_IE);
}

/* test flags */
static inline int native_irqs_disabled_flags(unsigned long flags)
{
	return !(flags & SR_IE);
}

/* test hardware interrupt enable bit */
static inline int native_irqs_disabled(void)
{
	return native_irqs_disabled_flags(native_save_flags());
}

/* set interrupt enabled status */
static inline void native_irq_restore(unsigned long flags)
{
	csr_set(CSR_STATUS, flags & SR_IE);
}

#include <asm/irq_pipeline.h>

#endif /* _ASM_RISCV_IRQFLAGS_H */
