#ifndef _ASM_RISCV_DOVETAIL_H
#define _ASM_RISCV_DOVETAIL_H

#include <linux/types.h>

#ifdef CONFIG_DOVETAIL

static inline void arch_dovetail_exec_prepare(void)
{
}

static inline void arch_dovetail_switch_prepare(bool leave_inband)
{
}

static inline void arch_dovetail_switch_finish(bool enter_inband)
{
}

/*
 * 172 is __NR_prctl from unistd in compat mode, without #inclusion
 * hell. At the end of the day, this number is written in stone to
 * honor the ABI stability promise anyway.
 */
#define arch_dovetail_is_syscall(__nr) \
	(is_compat_task() ? (__nr) == 172 : (__nr) == __NR_prctl)

#endif

/*
 * Pass the trap event to the companion core. Return true if running
 * in-band afterwards.
 */
#define mark_cond_trap_entry(__trapnr, __regs)             \
	({                                                 \
		bool __ret;                                \
		oob_trap_notify(__trapnr, __regs);         \
		__ret = running_inband();                  \
		if (!__ret)                                \
			oob_trap_unwind(__trapnr, __regs); \
		__ret;                                     \
	})

/*
 * Pass the trap event to the companion core. We expect the current
 * context to be running on the in-band stage upon return so that our
 * caller can tread on common kernel code.
 */
#define mark_trap_entry(__trapnr, __regs)                            \
	do {                                                         \
		bool __ret = mark_cond_trap_entry(__trapnr, __regs); \
		BUG_ON(dovetail_debug() && !__ret);                  \
	} while (0)

#define mark_trap_exit(__trapnr, __regs) oob_trap_unwind(__trapnr, __regs)

#endif /* _ASM_RISCV_DOVETAIL_H */