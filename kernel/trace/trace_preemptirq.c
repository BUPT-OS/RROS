// SPDX-License-Identifier: GPL-2.0
/*
 * preemptoff and irqoff tracepoints
 *
 * Copyright (C) Joel Fernandes (Google) <joel@joelfernandes.org>
 */

#include <linux/kallsyms.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/ftrace.h>
#include <linux/kprobes.h>
#include "trace.h"

#define CREATE_TRACE_POINTS
#include <trace/events/preemptirq.h>

/*
 * Use regular trace points on architectures that implement noinstr
 * tooling: these calls will only happen with RCU enabled, which can
 * use a regular tracepoint.
 *
 * On older architectures, use the rcuidle tracing methods (which
 * aren't NMI-safe - so exclude NMI contexts):
 */
#ifdef CONFIG_ARCH_WANTS_NO_INSTR
#define trace(point)	trace_##point
#else
#define trace(point)	if (!in_nmi()) trace_##point##_rcuidle
#endif

#ifdef CONFIG_TRACE_IRQFLAGS
/* Per-cpu variable to prevent redundant calls when IRQs already off */
static DEFINE_PER_CPU(int, tracing_irq_cpu);

/*
 * Like trace_hardirqs_on() but without the lockdep invocation. This is
 * used in the low level entry code where the ordering vs. RCU is important
 * and lockdep uses a staged approach which splits the lockdep hardirq
 * tracking into a RCU on and a RCU off section.
 */
void trace_hardirqs_on_prepare(void)
{
	if (this_cpu_read(tracing_irq_cpu)) {
		trace(irq_enable)(CALLER_ADDR0, CALLER_ADDR1);
		tracer_hardirqs_on(CALLER_ADDR0, CALLER_ADDR1);
		this_cpu_write(tracing_irq_cpu, 0);
	}
}
EXPORT_SYMBOL(trace_hardirqs_on_prepare);
NOKPROBE_SYMBOL(trace_hardirqs_on_prepare);

void trace_hardirqs_on(void)
{
	if (this_cpu_read(tracing_irq_cpu)) {
		trace(irq_enable)(CALLER_ADDR0, CALLER_ADDR1);
		tracer_hardirqs_on(CALLER_ADDR0, CALLER_ADDR1);
		this_cpu_write(tracing_irq_cpu, 0);
	}

	lockdep_hardirqs_on_prepare();
	lockdep_hardirqs_on(CALLER_ADDR0);
}
EXPORT_SYMBOL(trace_hardirqs_on);
NOKPROBE_SYMBOL(trace_hardirqs_on);

/*
 * Like trace_hardirqs_off() but without the lockdep invocation. This is
 * used in the low level entry code where the ordering vs. RCU is important
 * and lockdep uses a staged approach which splits the lockdep hardirq
 * tracking into a RCU on and a RCU off section.
 */
void trace_hardirqs_off_finish(void)
{
	if (!this_cpu_read(tracing_irq_cpu)) {
		this_cpu_write(tracing_irq_cpu, 1);
		tracer_hardirqs_off(CALLER_ADDR0, CALLER_ADDR1);
		trace(irq_disable)(CALLER_ADDR0, CALLER_ADDR1);
	}

}
EXPORT_SYMBOL(trace_hardirqs_off_finish);
NOKPROBE_SYMBOL(trace_hardirqs_off_finish);

void trace_hardirqs_off(void)
{
	lockdep_hardirqs_off(CALLER_ADDR0);

	if (!this_cpu_read(tracing_irq_cpu)) {
		this_cpu_write(tracing_irq_cpu, 1);
		tracer_hardirqs_off(CALLER_ADDR0, CALLER_ADDR1);
		trace(irq_disable)(CALLER_ADDR0, CALLER_ADDR1);
	}
}
EXPORT_SYMBOL(trace_hardirqs_off);
NOKPROBE_SYMBOL(trace_hardirqs_off);
#endif /* CONFIG_TRACE_IRQFLAGS */

#ifdef CONFIG_TRACE_PREEMPT_TOGGLE

void trace_preempt_on(unsigned long a0, unsigned long a1)
{
	trace(preempt_enable)(a0, a1);
	tracer_preempt_on(a0, a1);
}

void trace_preempt_off(unsigned long a0, unsigned long a1)
{
	trace(preempt_disable)(a0, a1);
	tracer_preempt_off(a0, a1);
}
#endif
