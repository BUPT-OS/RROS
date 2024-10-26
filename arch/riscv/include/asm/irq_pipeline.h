#ifndef _ASM_RISCV_IRQ_PIPELINE_H
#define _ASM_RISCV_IRQ_PIPELINE_H

#include <asm-generic/irq_pipeline.h>

#include <linux/compiler.h>
#include <asm/irqflags.h>

#ifdef CONFIG_IRQ_PIPELINE

#define RISCV_STATUS_SIE_BIT 1
// NOTE: RISCV_STATIS_SS_BIT should be unused bit.
#define RISCV_STATIS_SS_BIT 31
#define RISCV_STATUS_MIE_BIT 3

#ifdef CONFIG_RISCV_M_MODE
	#define RISCV_STATUS_IE_BIT RISCV_STATUS_MIE_BIT
#else
	#define RISCV_STATUS_IE_BIT RISCV_STATUS_SIE_BIT
#endif

/*
 * In order to cope with the limited number of SGIs available to us,
 * In-band IPI messages are multiplexed over SGI0, whereas out-of-band
 * IPIs are directly mapped to SGI1-3.
 */
#define OOB_NR_IPI 3
#define OOB_IPI_OFFSET 1 /* SGI1 */
#define TIMER_OOB_IPI (ipi_virq_base + OOB_IPI_OFFSET)
#define RESCHEDULE_OOB_IPI (TIMER_OOB_IPI + 1)
#define CALL_FUNCTION_OOB_IPI (RESCHEDULE_OOB_IPI + 1)

extern int ipi_virq_base;

static inline notrace unsigned long
arch_irqs_virtual_to_native_flags(int stalled)
{
	return (!stalled) << RISCV_STATUS_IE_BIT;
}

static inline notrace unsigned long
arch_irqs_native_to_virtual_flags(unsigned long flags)
{
	return (!!hard_irqs_disabled_flags(flags)) << RISCV_STATIS_SS_BIT;
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

static inline void arch_save_timer_regs(struct pt_regs *dst,
					struct pt_regs *src)
{
	dst->epc = src->epc;
	dst->ra = src->ra;
	dst->sp = src->sp;
	dst->gp = src->gp;
	dst->tp = src->tp;
	dst->t0 = src->t0;
	dst->t1 = src->t1;
	dst->t2 = src->t2;
	dst->s0 = src->s0;
	dst->s1 = src->s1;
	dst->a0 = src->a0;
	dst->a1 = src->a1;
	dst->a2 = src->a2;
	dst->a3 = src->a3;
	dst->a4 = src->a4;
	dst->a5 = src->a5;
	dst->a6 = src->a6;
	dst->a7 = src->a7;
	dst->s2 = src->s2;
	dst->s3 = src->s3;
	dst->s4 = src->s4;
	dst->s5 = src->s5;
	dst->s6 = src->s6;
	dst->s7 = src->s7;
	dst->s8 = src->s8;
	dst->s9 = src->s9;
	dst->s10 = src->s10;
	dst->s11 = src->s11;
	dst->t3 = src->t3;
	dst->t4 = src->t4;
	dst->t5 = src->t5;
	dst->status = src->status;
	dst->badaddr = src->badaddr;
	dst->cause = src->cause;
	dst->orig_a0 = src->orig_a0;
}

static inline bool arch_steal_pipelined_tick(struct pt_regs *regs)
{
	return !(regs->status & SR_IE);
}

static inline int arch_enable_oob_stage(void)
{
	return 0;
}

extern void (*handle_arch_irq)(struct pt_regs *) __ro_after_init;

static inline void arch_handle_irq_pipelined(struct pt_regs *regs)
{
	handle_arch_irq(regs);
}
#else /* !CONFIG_IRQ_PIPELINE */
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

#endif /* CONFIG_IRQ_PIPELINE */
static inline int arch_irqs_disabled(void)
{
	return arch_irqs_disabled_flags(arch_local_save_flags());
}

#endif /* _ASM_RISCV_IRQ_PIPELINE_H */