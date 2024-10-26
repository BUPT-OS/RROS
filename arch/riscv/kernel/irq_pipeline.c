/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>.
 */
#include <linux/irq.h>
#include <linux/irq_pipeline.h>
#include <linux/entry-common.h>

static irqentry_state_t pipeline_enter_rcu(void)
{
	irqentry_state_t state = {
		.exit_rcu = false,
		.stage_info = IRQENTRY_INBAND_UNSTALLED,
	};

	if (!IS_ENABLED(CONFIG_TINY_RCU) && is_idle_task(current)) {
		ct_irq_enter();
		state.exit_rcu = true;
	} else {
		rcu_irq_enter_check_tick();
	}

	return state;
}

static void pipeline_exit_rcu(irqentry_state_t state)
{
	if (state.exit_rcu)
		ct_irq_exit();
}

void arch_do_IRQ_pipelined(struct irq_desc *desc)
{
	struct pt_regs *regs = raw_cpu_ptr(&irq_pipeline.tick_regs);

	irqentry_state_t state = pipeline_enter_rcu();

	struct pt_regs *old_regs = set_irq_regs(regs);
	handle_irq_desc(desc);
	set_irq_regs(old_regs);

	pipeline_exit_rcu(state);
}

void __init arch_irq_pipeline_init(void)
{
	/* no per-arch init. */
}