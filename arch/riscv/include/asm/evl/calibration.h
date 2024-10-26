/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EVL_RISCV_ASM_CALIBRATION_H
#define _EVL_RISCV_ASM_CALIBRATION_H

#include <linux/kconfig.h>

static inline unsigned int evl_get_default_clock_gravity(void)
{
	return 3000;
}

#endif /* !_EVL_RISCV_ASM_CALIBRATION_H */
