// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2017 SiFive
 * Copyright (C) 2018 Christoph Hellwig
 */

#include <linux/interrupt.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <asm/sbi.h>
#include <asm/smp.h>
#include <asm/softirq_stack.h>
#include <asm/stacktrace.h>

static struct fwnode_handle *(*__get_intc_node)(void);

void riscv_set_intc_hwnode_fn(struct fwnode_handle *(*fn)(void))
{
	__get_intc_node = fn;
}

struct fwnode_handle *riscv_get_intc_hwnode(void)
{
	if (__get_intc_node)
		return __get_intc_node();

	return NULL;
}
EXPORT_SYMBOL_GPL(riscv_get_intc_hwnode);

#ifdef CONFIG_IRQ_STACKS
#include <asm/irq_stack.h>

DEFINE_PER_CPU(ulong *, irq_stack_ptr);

#ifdef CONFIG_VMAP_STACK
static void init_irq_stacks(void)
{
	int cpu;
	ulong *p;

	for_each_possible_cpu(cpu) {
		p = arch_alloc_vmap_stack(IRQ_STACK_SIZE, cpu_to_node(cpu));
		per_cpu(irq_stack_ptr, cpu) = p;
	}
}
#else
/* irq stack only needs to be 16 byte aligned - not IRQ_STACK_SIZE aligned. */
DEFINE_PER_CPU_ALIGNED(ulong [IRQ_STACK_SIZE/sizeof(ulong)], irq_stack);

static void init_irq_stacks(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		per_cpu(irq_stack_ptr, cpu) = per_cpu(irq_stack, cpu);
}
#endif /* CONFIG_VMAP_STACK */

#ifdef CONFIG_HAVE_SOFTIRQ_ON_OWN_STACK
void do_softirq_own_stack(void)
{
#ifdef CONFIG_IRQ_STACKS
	if (on_thread_stack()) {
		ulong *sp = per_cpu(irq_stack_ptr, smp_processor_id())
					+ IRQ_STACK_SIZE/sizeof(ulong);
		__asm__ __volatile(
		"addi	sp, sp, -"RISCV_SZPTR  "\n"
		REG_S"  ra, (sp)		\n"
		"addi	sp, sp, -"RISCV_SZPTR  "\n"
		REG_S"  s0, (sp)		\n"
		"addi	s0, sp, 2*"RISCV_SZPTR "\n"
		"move	sp, %[sp]		\n"
		"call	__do_softirq		\n"
		"addi	sp, s0, -2*"RISCV_SZPTR"\n"
		REG_L"  s0, (sp)		\n"
		"addi	sp, sp, "RISCV_SZPTR   "\n"
		REG_L"  ra, (sp)		\n"
		"addi	sp, sp, "RISCV_SZPTR   "\n"
		:
		: [sp] "r" (sp)
		: "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
		  "t0", "t1", "t2", "t3", "t4", "t5", "t6",
#ifndef CONFIG_FRAME_POINTER
		  "s0",
#endif
		  "memory");
	} else
#endif
		__do_softirq();
}
#endif /* CONFIG_HAVE_SOFTIRQ_ON_OWN_STACK */

#else
static void init_irq_stacks(void) {}
#endif /* CONFIG_IRQ_STACKS */

int arch_show_interrupts(struct seq_file *p, int prec)
{
	show_ipi_stats(p, prec);
	return 0;
}

void __init init_IRQ(void)
{
	init_irq_stacks();
	irqchip_init();
	if (!handle_arch_irq)
		panic("No interrupt controller found.");
	sbi_ipi_init();
}
