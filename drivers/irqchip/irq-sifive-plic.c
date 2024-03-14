// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 SiFive
 * Copyright (C) 2018 Christoph Hellwig
 */
#define pr_fmt(fmt) "plic: " fmt
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/syscore_ops.h>
#include <asm/smp.h>

/*
 * This driver implements a version of the RISC-V PLIC with the actual layout
 * specified in chapter 8 of the SiFive U5 Coreplex Series Manual:
 *
 *     https://static.dev.sifive.com/U54-MC-RVCoreIP.pdf
 *
 * The largest number supported by devices marked as 'sifive,plic-1.0.0', is
 * 1024, of which device 0 is defined as non-existent by the RISC-V Privileged
 * Spec.
 */

#define MAX_DEVICES			1024
#define MAX_CONTEXTS			15872

/*
 * Each interrupt source has a priority register associated with it.
 * We always hardwire it to one in Linux.
 */
#define PRIORITY_BASE			0
#define     PRIORITY_PER_ID		4

/*
 * Each hart context has a vector of interrupt enable bits associated with it.
 * There's one bit for each interrupt source.
 */
#define CONTEXT_ENABLE_BASE		0x2000
#define     CONTEXT_ENABLE_SIZE		0x80

/*
 * Each hart context has a set of control registers associated with it.  Right
 * now there's only two: a source priority threshold over which the hart will
 * take an interrupt, and a register to claim interrupts.
 */
#define CONTEXT_BASE			0x200000
#define     CONTEXT_SIZE		0x1000
#define     CONTEXT_THRESHOLD		0x00
#define     CONTEXT_CLAIM		0x04

#define	PLIC_DISABLE_THRESHOLD		0x7
#define	PLIC_ENABLE_THRESHOLD		0

#define PLIC_QUIRK_EDGE_INTERRUPT	0

struct plic_priv {
	struct cpumask lmask;
	struct irq_domain *irqdomain;
	void __iomem *regs;
	unsigned long plic_quirks;
	unsigned int nr_irqs;
	unsigned long *prio_save;
};

struct plic_handler {
	bool			present;
	void __iomem		*hart_base;
	/*
	 * Protect mask operations on the registers given that we can't
	 * assume atomic memory operations work on them.
	 */
	raw_spinlock_t		enable_lock;
	void __iomem		*enable_base;
	u32			*enable_save;
	struct plic_priv	*priv;
};
static int plic_parent_irq __ro_after_init;
static bool plic_cpuhp_setup_done __ro_after_init;
static DEFINE_PER_CPU(struct plic_handler, plic_handlers);

static int plic_irq_set_type(struct irq_data *d, unsigned int type);

static void __plic_toggle(void __iomem *enable_base, int hwirq, int enable)
{
	u32 __iomem *reg = enable_base + (hwirq / 32) * sizeof(u32);
	u32 hwirq_mask = 1 << (hwirq % 32);

	if (enable)
		writel(readl(reg) | hwirq_mask, reg);
	else
		writel(readl(reg) & ~hwirq_mask, reg);
}

static void plic_toggle(struct plic_handler *handler, int hwirq, int enable)
{
	raw_spin_lock(&handler->enable_lock);
	__plic_toggle(handler->enable_base, hwirq, enable);
	raw_spin_unlock(&handler->enable_lock);
}

static inline void plic_irq_toggle(const struct cpumask *mask,
				   struct irq_data *d, int enable)
{
	int cpu;

	for_each_cpu(cpu, mask) {
		struct plic_handler *handler = per_cpu_ptr(&plic_handlers, cpu);

		plic_toggle(handler, d->hwirq, enable);
	}
}

static void plic_irq_enable(struct irq_data *d)
{
	plic_irq_toggle(irq_data_get_effective_affinity_mask(d), d, 1);
}

static void plic_irq_disable(struct irq_data *d)
{
	plic_irq_toggle(irq_data_get_effective_affinity_mask(d), d, 0);
}

static void plic_irq_unmask(struct irq_data *d)
{
	struct plic_priv *priv = irq_data_get_irq_chip_data(d);

	writel(1, priv->regs + PRIORITY_BASE + d->hwirq * PRIORITY_PER_ID);
}

static void plic_irq_mask(struct irq_data *d)
{
	struct plic_priv *priv = irq_data_get_irq_chip_data(d);

	writel(0, priv->regs + PRIORITY_BASE + d->hwirq * PRIORITY_PER_ID);
}

static void plic_irq_eoi(struct irq_data *d)
{
	struct plic_handler *handler = this_cpu_ptr(&plic_handlers);

	writel(d->hwirq, handler->hart_base + CONTEXT_CLAIM);
}

#ifdef CONFIG_SMP
static int plic_set_affinity(struct irq_data *d,
			     const struct cpumask *mask_val, bool force)
{
	unsigned int cpu;
	struct cpumask amask;
	struct plic_priv *priv = irq_data_get_irq_chip_data(d);

	cpumask_and(&amask, &priv->lmask, mask_val);

	if (force)
		cpu = cpumask_first(&amask);
	else
		cpu = cpumask_any_and(&amask, cpu_online_mask);

	if (cpu >= nr_cpu_ids)
		return -EINVAL;

	plic_irq_disable(d);

	irq_data_update_effective_affinity(d, cpumask_of(cpu));

	if (!irqd_irq_disabled(d))
		plic_irq_enable(d);

	return IRQ_SET_MASK_OK_DONE;
}
#endif

static struct irq_chip plic_edge_chip = {
	.name		= "SiFive PLIC",
	.irq_enable	= plic_irq_enable,
	.irq_disable	= plic_irq_disable,
	.irq_ack	= plic_irq_eoi,
	.irq_mask	= plic_irq_mask,
	.irq_unmask	= plic_irq_unmask,
#ifdef CONFIG_SMP
	.irq_set_affinity = plic_set_affinity,
#endif
	.irq_set_type	= plic_irq_set_type,
	.flags		= IRQCHIP_SKIP_SET_WAKE |
			  IRQCHIP_AFFINITY_PRE_STARTUP,
};

static struct irq_chip plic_chip = {
	.name		= "SiFive PLIC",
	.irq_enable	= plic_irq_enable,
	.irq_disable	= plic_irq_disable,
	.irq_mask	= plic_irq_mask,
	.irq_unmask	= plic_irq_unmask,
	.irq_eoi	= plic_irq_eoi,
#ifdef CONFIG_SMP
	.irq_set_affinity = plic_set_affinity,
#endif
	.irq_set_type	= plic_irq_set_type,
	.flags		= IRQCHIP_SKIP_SET_WAKE |
			  IRQCHIP_AFFINITY_PRE_STARTUP,
};

static int plic_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct plic_priv *priv = irq_data_get_irq_chip_data(d);

	if (!test_bit(PLIC_QUIRK_EDGE_INTERRUPT, &priv->plic_quirks))
		return IRQ_SET_MASK_OK_NOCOPY;

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		irq_set_chip_handler_name_locked(d, &plic_edge_chip,
						 handle_edge_irq, NULL);
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		irq_set_chip_handler_name_locked(d, &plic_chip,
						 handle_fasteoi_irq, NULL);
		break;
	default:
		return -EINVAL;
	}

	return IRQ_SET_MASK_OK;
}

static int plic_irq_suspend(void)
{
	unsigned int i, cpu;
	u32 __iomem *reg;
	struct plic_priv *priv;

	priv = per_cpu_ptr(&plic_handlers, smp_processor_id())->priv;

	for (i = 0; i < priv->nr_irqs; i++)
		if (readl(priv->regs + PRIORITY_BASE + i * PRIORITY_PER_ID))
			__set_bit(i, priv->prio_save);
		else
			__clear_bit(i, priv->prio_save);

	for_each_cpu(cpu, cpu_present_mask) {
		struct plic_handler *handler = per_cpu_ptr(&plic_handlers, cpu);

		if (!handler->present)
			continue;

		raw_spin_lock(&handler->enable_lock);
		for (i = 0; i < DIV_ROUND_UP(priv->nr_irqs, 32); i++) {
			reg = handler->enable_base + i * sizeof(u32);
			handler->enable_save[i] = readl(reg);
		}
		raw_spin_unlock(&handler->enable_lock);
	}

	return 0;
}

static void plic_irq_resume(void)
{
	unsigned int i, index, cpu;
	u32 __iomem *reg;
	struct plic_priv *priv;

	priv = per_cpu_ptr(&plic_handlers, smp_processor_id())->priv;

	for (i = 0; i < priv->nr_irqs; i++) {
		index = BIT_WORD(i);
		writel((priv->prio_save[index] & BIT_MASK(i)) ? 1 : 0,
		       priv->regs + PRIORITY_BASE + i * PRIORITY_PER_ID);
	}

	for_each_cpu(cpu, cpu_present_mask) {
		struct plic_handler *handler = per_cpu_ptr(&plic_handlers, cpu);

		if (!handler->present)
			continue;

		raw_spin_lock(&handler->enable_lock);
		for (i = 0; i < DIV_ROUND_UP(priv->nr_irqs, 32); i++) {
			reg = handler->enable_base + i * sizeof(u32);
			writel(handler->enable_save[i], reg);
		}
		raw_spin_unlock(&handler->enable_lock);
	}
}

static struct syscore_ops plic_irq_syscore_ops = {
	.suspend	= plic_irq_suspend,
	.resume		= plic_irq_resume,
};

static int plic_irqdomain_map(struct irq_domain *d, unsigned int irq,
			      irq_hw_number_t hwirq)
{
	struct plic_priv *priv = d->host_data;

	irq_domain_set_info(d, irq, hwirq, &plic_chip, d->host_data,
			    handle_fasteoi_irq, NULL, NULL);
	irq_set_noprobe(irq);
	irq_set_affinity(irq, &priv->lmask);
	return 0;
}

static int plic_irq_domain_translate(struct irq_domain *d,
				     struct irq_fwspec *fwspec,
				     unsigned long *hwirq,
				     unsigned int *type)
{
	struct plic_priv *priv = d->host_data;

	if (test_bit(PLIC_QUIRK_EDGE_INTERRUPT, &priv->plic_quirks))
		return irq_domain_translate_twocell(d, fwspec, hwirq, type);

	return irq_domain_translate_onecell(d, fwspec, hwirq, type);
}

static int plic_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				 unsigned int nr_irqs, void *arg)
{
	int i, ret;
	irq_hw_number_t hwirq;
	unsigned int type;
	struct irq_fwspec *fwspec = arg;

	ret = plic_irq_domain_translate(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	for (i = 0; i < nr_irqs; i++) {
		ret = plic_irqdomain_map(domain, virq + i, hwirq + i);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct irq_domain_ops plic_irqdomain_ops = {
	.translate	= plic_irq_domain_translate,
	.alloc		= plic_irq_domain_alloc,
	.free		= irq_domain_free_irqs_top,
};

/*
 * Handling an interrupt is a two-step process: first you claim the interrupt
 * by reading the claim register, then you complete the interrupt by writing
 * that source ID back to the same claim register.  This automatically enables
 * and disables the interrupt, so there's nothing else to do.
 */
static void plic_handle_irq(struct irq_desc *desc)
{
	struct plic_handler *handler = this_cpu_ptr(&plic_handlers);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	void __iomem *claim = handler->hart_base + CONTEXT_CLAIM;
	irq_hw_number_t hwirq;

	WARN_ON_ONCE(!handler->present);

	chained_irq_enter(chip, desc);

	while ((hwirq = readl(claim))) {
		int err = generic_handle_domain_irq(handler->priv->irqdomain,
						    hwirq);
		if (unlikely(err))
			pr_warn_ratelimited("can't find mapping for hwirq %lu\n",
					hwirq);
	}

	chained_irq_exit(chip, desc);
}

static void plic_set_threshold(struct plic_handler *handler, u32 threshold)
{
	/* priority must be > threshold to trigger an interrupt */
	writel(threshold, handler->hart_base + CONTEXT_THRESHOLD);
}

static int plic_dying_cpu(unsigned int cpu)
{
	if (plic_parent_irq)
		disable_percpu_irq(plic_parent_irq);

	return 0;
}

static int plic_starting_cpu(unsigned int cpu)
{
	struct plic_handler *handler = this_cpu_ptr(&plic_handlers);

	if (plic_parent_irq)
		enable_percpu_irq(plic_parent_irq,
				  irq_get_trigger_type(plic_parent_irq));
	else
		pr_warn("cpu%d: parent irq not available\n", cpu);
	plic_set_threshold(handler, PLIC_ENABLE_THRESHOLD);

	return 0;
}

static int __init __plic_init(struct device_node *node,
			      struct device_node *parent,
			      unsigned long plic_quirks)
{
	int error = 0, nr_contexts, nr_handlers = 0, i;
	u32 nr_irqs;
	struct plic_priv *priv;
	struct plic_handler *handler;
	unsigned int cpu;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->plic_quirks = plic_quirks;

	priv->regs = of_iomap(node, 0);
	if (WARN_ON(!priv->regs)) {
		error = -EIO;
		goto out_free_priv;
	}

	error = -EINVAL;
	of_property_read_u32(node, "riscv,ndev", &nr_irqs);
	if (WARN_ON(!nr_irqs))
		goto out_iounmap;

	priv->nr_irqs = nr_irqs;

	priv->prio_save = bitmap_alloc(nr_irqs, GFP_KERNEL);
	if (!priv->prio_save)
		goto out_free_priority_reg;

	nr_contexts = of_irq_count(node);
	if (WARN_ON(!nr_contexts))
		goto out_free_priority_reg;

	error = -ENOMEM;
	priv->irqdomain = irq_domain_add_linear(node, nr_irqs + 1,
			&plic_irqdomain_ops, priv);
	if (WARN_ON(!priv->irqdomain))
		goto out_free_priority_reg;

	for (i = 0; i < nr_contexts; i++) {
		struct of_phandle_args parent;
		irq_hw_number_t hwirq;
		int cpu;
		unsigned long hartid;

		if (of_irq_parse_one(node, i, &parent)) {
			pr_err("failed to parse parent for context %d.\n", i);
			continue;
		}

		/*
		 * Skip contexts other than external interrupts for our
		 * privilege level.
		 */
		if (parent.args[0] != RV_IRQ_EXT) {
			/* Disable S-mode enable bits if running in M-mode. */
			if (IS_ENABLED(CONFIG_RISCV_M_MODE)) {
				void __iomem *enable_base = priv->regs +
					CONTEXT_ENABLE_BASE +
					i * CONTEXT_ENABLE_SIZE;

				for (hwirq = 1; hwirq <= nr_irqs; hwirq++)
					__plic_toggle(enable_base, hwirq, 0);
			}
			continue;
		}

		error = riscv_of_parent_hartid(parent.np, &hartid);
		if (error < 0) {
			pr_warn("failed to parse hart ID for context %d.\n", i);
			continue;
		}

		cpu = riscv_hartid_to_cpuid(hartid);
		if (cpu < 0) {
			pr_warn("Invalid cpuid for context %d\n", i);
			continue;
		}

		/* Find parent domain and register chained handler */
		if (!plic_parent_irq && irq_find_host(parent.np)) {
			plic_parent_irq = irq_of_parse_and_map(node, i);
			if (plic_parent_irq)
				irq_set_chained_handler(plic_parent_irq,
							plic_handle_irq);
		}

		/*
		 * When running in M-mode we need to ignore the S-mode handler.
		 * Here we assume it always comes later, but that might be a
		 * little fragile.
		 */
		handler = per_cpu_ptr(&plic_handlers, cpu);
		if (handler->present) {
			pr_warn("handler already present for context %d.\n", i);
			plic_set_threshold(handler, PLIC_DISABLE_THRESHOLD);
			goto done;
		}

		cpumask_set_cpu(cpu, &priv->lmask);
		handler->present = true;
		handler->hart_base = priv->regs + CONTEXT_BASE +
			i * CONTEXT_SIZE;
		raw_spin_lock_init(&handler->enable_lock);
		handler->enable_base = priv->regs + CONTEXT_ENABLE_BASE +
			i * CONTEXT_ENABLE_SIZE;
		handler->priv = priv;

		handler->enable_save =  kcalloc(DIV_ROUND_UP(nr_irqs, 32),
						sizeof(*handler->enable_save), GFP_KERNEL);
		if (!handler->enable_save)
			goto out_free_enable_reg;
done:
		for (hwirq = 1; hwirq <= nr_irqs; hwirq++) {
			plic_toggle(handler, hwirq, 0);
			writel(1, priv->regs + PRIORITY_BASE +
				  hwirq * PRIORITY_PER_ID);
		}
		nr_handlers++;
	}

	/*
	 * We can have multiple PLIC instances so setup cpuhp state only
	 * when context handler for current/boot CPU is present.
	 */
	handler = this_cpu_ptr(&plic_handlers);
	if (handler->present && !plic_cpuhp_setup_done) {
		cpuhp_setup_state(CPUHP_AP_IRQ_SIFIVE_PLIC_STARTING,
				  "irqchip/sifive/plic:starting",
				  plic_starting_cpu, plic_dying_cpu);
		plic_cpuhp_setup_done = true;
	}
	register_syscore_ops(&plic_irq_syscore_ops);

	pr_info("%pOFP: mapped %d interrupts with %d handlers for"
		" %d contexts.\n", node, nr_irqs, nr_handlers, nr_contexts);
	return 0;

out_free_enable_reg:
	for_each_cpu(cpu, cpu_present_mask) {
		handler = per_cpu_ptr(&plic_handlers, cpu);
		kfree(handler->enable_save);
	}
out_free_priority_reg:
	kfree(priv->prio_save);
out_iounmap:
	iounmap(priv->regs);
out_free_priv:
	kfree(priv);
	return error;
}

static int __init plic_init(struct device_node *node,
			    struct device_node *parent)
{
	return __plic_init(node, parent, 0);
}

IRQCHIP_DECLARE(sifive_plic, "sifive,plic-1.0.0", plic_init);
IRQCHIP_DECLARE(riscv_plic0, "riscv,plic0", plic_init); /* for legacy systems */

static int __init plic_edge_init(struct device_node *node,
				 struct device_node *parent)
{
	return __plic_init(node, parent, BIT(PLIC_QUIRK_EDGE_INTERRUPT));
}

IRQCHIP_DECLARE(andestech_nceplic100, "andestech,nceplic100", plic_edge_init);
IRQCHIP_DECLARE(thead_c900_plic, "thead,c900-plic", plic_edge_init);
