/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>.
 * Copyright (C) 2023 Hao Miao <haomiao19@mails.ucas.ac.cn>.
 * Copyright (C) 2024 Zhang Zheng <lz5333885@gmail.com>.
 */
#ifndef _ASM_LOONGARCH64_IRQ_PIPELINE_H
#define _ASM_LOONGARCH64_IRQ_PIPELINE_H

#include <asm-generic/irq_pipeline.h>

#include <asm/loongarch.h>
#include <asm/ptrace.h>


static inline int arch_irqs_disabled_flags(unsigned long flags);

#ifdef CONFIG_IRQ_PIPELINE

#define OOB_NR_IPI		2
#define OOB_IPI_OFFSET		1 /* SGI1 */
#define TIMER_OOB_IPI		(ipi_irq_base + OOB_IPI_OFFSET)
#define RESCHEDULE_OOB_IPI	(TIMER_OOB_IPI + 1)
#define CALL_FUNCTION_OOB_IPI	(RESCHEDULE_OOB_IPI + 1)

extern int ipi_irq_base;

#define CSR_CRME_EMPTY		10

static inline notrace
unsigned long arch_irqs_virtual_to_native_flags(int stalled)
{
	return (!stalled) << CSR_CRMD_IE_SHIFT;
}

static inline int native_irqs_disabled_flags(unsigned long flags)
{
	return !(flags & CSR_CRMD_IE);
}

static inline notrace
unsigned long arch_irqs_native_to_virtual_flags(unsigned long flags)
{
	return (!!hard_irqs_disabled_flags(flags)) << CSR_CRME_EMPTY;
}

static inline notrace unsigned long arch_local_irq_save(void)
{
	int stalled = inband_irq_save();
	barrier();
	return arch_irqs_virtual_to_native_flags(stalled);
}

static inline notrace void arch_local_irq_enable(void)
{
	barrier();
	inband_irq_enable();
}

static inline notrace void arch_local_irq_disable(void)
{
	inband_irq_disable();
	barrier();
}

static inline notrace unsigned long arch_local_save_flags(void)
{
	int stalled = inband_irqs_disabled();
	barrier();
	return arch_irqs_virtual_to_native_flags(stalled);
}


static inline int arch_irqs_disabled_flags(unsigned long flags)
{
	return native_irqs_disabled_flags(flags);
}

static inline notrace void arch_local_irq_restore(unsigned long flags)
{
	inband_irq_restore(arch_irqs_disabled_flags(flags));
	barrier();
}

static inline
void arch_save_timer_regs(struct pt_regs *dst, struct pt_regs *src)
{
	dst->csr_crmd = src->csr_crmd;
	dst->csr_prmd = src->csr_prmd;
	dst->orig_a0 = src->orig_a0;
}

static inline bool arch_steal_pipelined_tick(struct pt_regs *regs)
{
	return !(regs->csr_crmd & CSR_CRMD_IE);
}

static inline int arch_enable_oob_stage(void)
{
	return 0;
}


inline void arch_handle_irq_pipelined(struct pt_regs *regs);

#else  /* !CONFIG_IRQ_PIPELINE */

static inline unsigned long arch_local_irq_save(void)
{
	return native_irq_save();
}

static inline void arch_local_irq_enable(void)
{
	native_irq_enable();
}

static inline void arch_local_irq_disable(void)
{
	native_irq_disable();
}

static inline unsigned long arch_local_save_flags(void)
{
	return native_save_flags();
}

static inline void arch_local_irq_restore(unsigned long flags)
{
	native_irq_restore(flags);
}

static inline int arch_irqs_disabled_flags(unsigned long flags)
{
	return native_irqs_disabled_flags(flags);
}

#endif /* !CONFIG_IRQ_PIPELINE */

static inline int arch_irqs_disabled(void)
{
	return arch_irqs_disabled_flags(arch_local_save_flags());
}

#endif /* _ASM_LOONGARCH64_IRQ_PIPELINE_H */
