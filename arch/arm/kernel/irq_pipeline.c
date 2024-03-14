/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2016 Philippe Gerum  <rpm@xenomai.org>.
 */
#include <linux/irq.h>
#include <linux/irq_pipeline.h>

void arch_do_IRQ_pipelined(struct irq_desc *desc)
{
	struct pt_regs *regs = raw_cpu_ptr(&irq_pipeline.tick_regs);
	struct pt_regs *old_regs = set_irq_regs(regs);

	irq_enter();
	handle_irq_desc(desc);
	irq_exit();

	set_irq_regs(old_regs);
}

void __init arch_irq_pipeline_init(void)
{
	/* no per-arch init. */
}
