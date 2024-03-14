/* SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0-only) */
/* Copyright(c) 2014 - 2020 Intel Corporation */
#ifndef ADF_C62X_HW_DATA_H_
#define ADF_C62X_HW_DATA_H_

#include <linux/units.h>

/* PCIe configuration space */
#define ADF_C62X_SRAM_BAR 0
#define ADF_C62X_PMISC_BAR 1
#define ADF_C62X_ETR_BAR 2
#define ADF_C62X_MAX_ACCELERATORS 5
#define ADF_C62X_MAX_ACCELENGINES 10
#define ADF_C62X_ACCELERATORS_REG_OFFSET 16
#define ADF_C62X_ACCELERATORS_MASK 0x1F
#define ADF_C62X_ACCELENGINES_MASK 0x3FF
#define ADF_C62X_ETR_MAX_BANKS 16
#define ADF_C62X_SOFTSTRAP_CSR_OFFSET 0x2EC

/* AE to function mapping */
#define ADF_C62X_AE2FUNC_MAP_GRP_A_NUM_REGS 80
#define ADF_C62X_AE2FUNC_MAP_GRP_B_NUM_REGS 10

/* Clocks frequency */
#define ADF_C62X_AE_FREQ (685 * HZ_PER_MHZ)
#define ADF_C62X_MIN_AE_FREQ (533 * HZ_PER_MHZ)
#define ADF_C62X_MAX_AE_FREQ (800 * HZ_PER_MHZ)

/* Firmware Binary */
#define ADF_C62X_FW "qat_c62x.bin"
#define ADF_C62X_MMP "qat_c62x_mmp.bin"

void adf_init_hw_data_c62x(struct adf_hw_device_data *hw_data);
void adf_clean_hw_data_c62x(struct adf_hw_device_data *hw_data);
#endif
