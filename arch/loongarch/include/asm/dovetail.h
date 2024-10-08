/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>.
 * Copyright (C) 2023 Hao Miao <haomiao19@mails.ucas.ac.cn>.
 * Copyright (C) 2024 Zhang Zheng <lz5333885@gmail.com>.
 */
#ifndef _ASM_LOONGARCH64_DOVETAIL_H
#define _ASM_LOONGARCH64_DOVETAIL_H

/* LOONGARCH64 traps */
#define LOONGARCH64_TRAP_FPE 0        /* Delayed fp exception */
#define LOONGARCH64_TRAP_BP 1         /* Break point */
#define LOONGARCH64_TRAP_WATCH 2      /* Watch point */
#define LOONGARCH64_TRAP_RI 3         /* Reserved instruction */
#define LOONGARCH64_TRAP_FPU 4        /* FPU access */
#define LOONGARCH64_TRAP_LSX 5        /* LAX access */
#define LOONGARCH64_TRAP_LASX 6       /* LASX access */
#define LOONGARCH64_TRAP_LBT 7        /* LBT access */
#define LOONGARCH64_TRAP_RESERVED 8   /* Reserved exception */
#define LOONGARCH64_TRAP_ADE 9        /* Wrong memeory address access */
#define LOONGARCH64_TRAP_ALE 10       /* Unaligned memeory address access */
#define LOONGARCH64_TRAP_PAGEFAULT 11 /* Page fault */

#ifdef CONFIG_DOVETAIL

static inline void arch_dovetail_exec_prepare(void) {}

static inline void arch_dovetail_switch_prepare(bool leave_inband) {}

static inline void arch_dovetail_switch_finish(bool enter_inband) {
  extern void restore_fp_current_oob(void);
  restore_fp_current_oob();
}

#define arch_dovetail_is_syscall(__nr) ((__nr) == __NR_prctl)

#endif

#endif /* _ASM_LOONGARCH64_DOVETAIL_H */
