/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Switch a MMU context.
 *
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */
#ifndef _ASM_MMU_CONTEXT_H
#define _ASM_MMU_CONTEXT_H

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/mm_types.h>
#include <linux/smp.h>
#include <linux/slab.h>

#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm-generic/mm_hooks.h>

/*
 *  All unused by hardware upper bits will be considered
 *  as a software asid extension.
 */
static inline u64 asid_version_mask(unsigned int cpu)
{
	return ~(u64)(cpu_asid_mask(&cpu_data[cpu]));
}

static inline u64 asid_first_version(unsigned int cpu)
{
	return cpu_asid_mask(&cpu_data[cpu]) + 1;
}

#define cpu_context(cpu, mm)	((mm)->context.asid[cpu])
#define asid_cache(cpu)		(cpu_data[cpu].asid_cache)
#define cpu_asid(cpu, mm)	(cpu_context((cpu), (mm)) & cpu_asid_mask(&cpu_data[cpu]))

static inline int asid_valid(struct mm_struct *mm, unsigned int cpu)
{
	if ((cpu_context(cpu, mm) ^ asid_cache(cpu)) & asid_version_mask(cpu))
		return 0;

	return 1;
}

static inline void enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk)
{
}

/* Normal, classic get_new_mmu_context */
static inline void
get_new_mmu_context(struct mm_struct *mm, unsigned long cpu)
{
	u64 asid = asid_cache(cpu);

	if (!((++asid) & cpu_asid_mask(&cpu_data[cpu])))
		local_flush_tlb_user();	/* start new asid cycle */

	cpu_context(cpu, mm) = asid_cache(cpu) = asid;
}

/*
 * Initialize the context related info for a new mm_struct
 * instance.
 */
static inline int
init_new_context(struct task_struct *tsk, struct mm_struct *mm)
{
	int i;

	for_each_possible_cpu(i)
		cpu_context(i, mm) = 0;

	return 0;
}

static inline void switch_mm_irqs_off(struct mm_struct *prev, struct mm_struct *next,
				      struct task_struct *tsk)
{
	unsigned int cpu = smp_processor_id();

	/* Check if our ASID is of an older version and thus invalid */
	if (!asid_valid(next, cpu))
		get_new_mmu_context(next, cpu);

	write_csr_asid(cpu_asid(cpu, next));

	if (next != &init_mm)
		csr_write64((unsigned long)next->pgd, LOONGARCH_CSR_PGDL);
	else
		csr_write64((unsigned long)invalid_pg_dir, LOONGARCH_CSR_PGDL);

	/*
	 * Mark current->active_mm as not "active" anymore.
	 * We don't want to mislead possible IPI tlb flush routines.
	 */
	cpumask_set_cpu(cpu, mm_cpumask(next));
}

#define switch_mm_irqs_off switch_mm_irqs_off

static inline void switch_mm(struct mm_struct *prev, struct mm_struct *next,
			     struct task_struct *tsk)
{
	unsigned long flags;

	local_irq_save(flags);
	switch_mm_irqs_off(prev, next, tsk);
	local_irq_restore(flags);
}

/*
 * Destroy context related info for an mm_struct that is about
 * to be put to rest.
 */
static inline void destroy_context(struct mm_struct *mm)
{
}

#define activate_mm(prev, next)	switch_mm(prev, next, current)
#define deactivate_mm(task, mm)	do { } while (0)

/*
 * If mm is currently active, we can't really drop it.
 * Instead, we will get a new one for it.
 */
static inline void
drop_mmu_context(struct mm_struct *mm, unsigned int cpu)
{
	int asid;
	unsigned long flags;

	local_irq_save(flags);

	asid = read_csr_asid() & cpu_asid_mask(&current_cpu_data);

	if (asid == cpu_asid(cpu, mm)) {
		if (!current->mm || (current->mm == mm)) {
			get_new_mmu_context(mm, cpu);
			write_csr_asid(cpu_asid(cpu, mm));
			goto out;
		}
	}

	/* Will get a new context next time */
	cpu_context(cpu, mm) = 0;
	cpumask_clear_cpu(cpu, mm_cpumask(mm));
out:
	local_irq_restore(flags);
}

#endif /* _ASM_MMU_CONTEXT_H */
