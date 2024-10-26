/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EVL_RISCV_ASM_THREAD_H
#define _EVL_RISCV_ASM_THREAD_H


static inline bool evl_is_breakpoint(int trapnr)
{
    // TODO: Check if the trap number is a breakpoint trap
	return trapnr == 0 || trapnr == 1;
}

#endif /* !_EVL_X86_ASM_THREAD_H */
