/*
 * SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
 *
 * Derived from Xenomai Cobalt, https://xenomai.org/
 * Copyright (C) 2006 Gilles Chanteperdrix <gilles.chanteperdrix@xenomai.org>.
 */
#ifndef _EVL_RISCV_ASM_UAPI_FPTEST_H
#define _EVL_RISCV_ASM_UAPI_FPTEST_H

#include <linux/types.h>

#define evl_riscv_fpsimd  0x1
#define evl_ricsv_sve     0x2

// TODO: 设置与检查riscv浮点寄存器
/*
 * CAUTION: keep this code strictly inlined in macros: we don't want
 * GCC to apply any callee-saved logic to fpsimd registers in
 * evl_set_fpregs() before evl_check_fpregs() can verify their
 * contents, but we still want GCC to know about the registers we have
 * clobbered.
 */

#define evl_set_fpregs(__features, __val)				\
	do {								\
	} while (0)

#define evl_check_fpregs(__features, __val, __bad)			\
	({								\
		unsigned int __result = (__val);			\
		__result;						\
	})

#endif /* !_EVL_ARM64_ASM_UAPI_FPTEST_H */
