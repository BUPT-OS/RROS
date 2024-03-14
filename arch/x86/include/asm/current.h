/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CURRENT_H
#define _ASM_X86_CURRENT_H

#include <linux/compiler.h>

#ifndef __ASSEMBLY__

#include <linux/cache.h>
#include <asm/percpu.h>

struct task_struct;

struct pcpu_hot {
	union {
		struct {
			struct task_struct	*current_task;
			int			preempt_count;
			int			cpu_number;
#ifdef CONFIG_CALL_DEPTH_TRACKING
			u64			call_depth;
#endif
			unsigned long		top_of_stack;
			void			*hardirq_stack_ptr;
			u16			softirq_pending;
#ifdef CONFIG_X86_64
			bool			hardirq_stack_inuse;
#else
			void			*softirq_stack_ptr;
#endif
		};
		u8	pad[64];
	};
};
static_assert(sizeof(struct pcpu_hot) == 64);

DECLARE_PER_CPU_ALIGNED(struct pcpu_hot, pcpu_hot);

static __always_inline struct task_struct *get_current(void)
{
	return this_cpu_read_stable(pcpu_hot.current_task);
}

#define current get_current()

#endif /* __ASSEMBLY__ */

#endif /* _ASM_X86_CURRENT_H */
