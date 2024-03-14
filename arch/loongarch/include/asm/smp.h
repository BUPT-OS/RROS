/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Author: Huacai Chen <chenhuacai@loongson.cn>
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */
#ifndef __ASM_SMP_H
#define __ASM_SMP_H

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/linkage.h>
#include <linux/threads.h>
#include <linux/cpumask.h>

extern int smp_num_siblings;
extern int num_processors;
extern int disabled_cpus;
extern cpumask_t cpu_sibling_map[];
extern cpumask_t cpu_core_map[];
extern cpumask_t cpu_foreign_map[];

void loongson_smp_setup(void);
void loongson_prepare_cpus(unsigned int max_cpus);
void loongson_boot_secondary(int cpu, struct task_struct *idle);
void loongson_init_secondary(void);
void loongson_smp_finish(void);
void loongson_send_ipi_single(int cpu, unsigned int action);
void loongson_send_ipi_mask(const struct cpumask *mask, unsigned int action);
#ifdef CONFIG_HOTPLUG_CPU
int loongson_cpu_disable(void);
void loongson_cpu_die(unsigned int cpu);
#endif

static inline void plat_smp_setup(void)
{
	loongson_smp_setup();
}

static inline int raw_smp_processor_id(void)
{
#if defined(__VDSO__)
	extern int vdso_smp_processor_id(void)
		__compiletime_error("VDSO should not call smp_processor_id()");
	return vdso_smp_processor_id();
#else
	return current_thread_info()->cpu;
#endif
}
#define raw_smp_processor_id raw_smp_processor_id

/* Map from cpu id to sequential logical cpu number.  This will only
 * not be idempotent when cpus failed to come on-line.	*/
extern int __cpu_number_map[NR_CPUS];
#define cpu_number_map(cpu)  __cpu_number_map[cpu]

/* The reverse map from sequential logical cpu number to cpu id.  */
extern int __cpu_logical_map[NR_CPUS];
#define cpu_logical_map(cpu)  __cpu_logical_map[cpu]

#define cpu_physical_id(cpu)	cpu_logical_map(cpu)

#define SMP_BOOT_CPU		0x1
#define SMP_RESCHEDULE		0x2
#define SMP_CALL_FUNCTION	0x4

struct secondary_data {
	unsigned long stack;
	unsigned long thread_info;
};
extern struct secondary_data cpuboot_data;

extern asmlinkage void smpboot_entry(void);
extern asmlinkage void start_secondary(void);

extern void calculate_cpu_foreign_map(void);

/*
 * Generate IPI list text
 */
extern void show_ipi_list(struct seq_file *p, int prec);

static inline void arch_send_call_function_single_ipi(int cpu)
{
	loongson_send_ipi_single(cpu, SMP_CALL_FUNCTION);
}

static inline void arch_send_call_function_ipi_mask(const struct cpumask *mask)
{
	loongson_send_ipi_mask(mask, SMP_CALL_FUNCTION);
}

#ifdef CONFIG_HOTPLUG_CPU
static inline int __cpu_disable(void)
{
	return loongson_cpu_disable();
}

static inline void __cpu_die(unsigned int cpu)
{
	loongson_cpu_die(cpu);
}
#endif

#endif /* __ASM_SMP_H */
