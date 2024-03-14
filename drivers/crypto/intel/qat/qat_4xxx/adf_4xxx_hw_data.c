// SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0-only)
/* Copyright(c) 2020 - 2021 Intel Corporation */
#include <linux/iopoll.h>
#include <adf_accel_devices.h>
#include <adf_cfg.h>
#include <adf_clock.h>
#include <adf_common_drv.h>
#include <adf_gen4_dc.h>
#include <adf_gen4_hw_data.h>
#include <adf_gen4_pfvf.h>
#include <adf_gen4_pm.h>
#include <adf_gen4_timer.h>
#include "adf_4xxx_hw_data.h"
#include "icp_qat_hw.h"

enum adf_fw_objs {
	ADF_FW_SYM_OBJ,
	ADF_FW_ASYM_OBJ,
	ADF_FW_DC_OBJ,
	ADF_FW_ADMIN_OBJ,
};

static const char * const adf_4xxx_fw_objs[] = {
	[ADF_FW_SYM_OBJ] =  ADF_4XXX_SYM_OBJ,
	[ADF_FW_ASYM_OBJ] =  ADF_4XXX_ASYM_OBJ,
	[ADF_FW_DC_OBJ] =  ADF_4XXX_DC_OBJ,
	[ADF_FW_ADMIN_OBJ] = ADF_4XXX_ADMIN_OBJ,
};

static const char * const adf_402xx_fw_objs[] = {
	[ADF_FW_SYM_OBJ] =  ADF_402XX_SYM_OBJ,
	[ADF_FW_ASYM_OBJ] =  ADF_402XX_ASYM_OBJ,
	[ADF_FW_DC_OBJ] =  ADF_402XX_DC_OBJ,
	[ADF_FW_ADMIN_OBJ] = ADF_402XX_ADMIN_OBJ,
};

struct adf_fw_config {
	u32 ae_mask;
	enum adf_fw_objs obj;
};

static const struct adf_fw_config adf_fw_cy_config[] = {
	{0xF0, ADF_FW_SYM_OBJ},
	{0xF, ADF_FW_ASYM_OBJ},
	{0x100, ADF_FW_ADMIN_OBJ},
};

static const struct adf_fw_config adf_fw_dc_config[] = {
	{0xF0, ADF_FW_DC_OBJ},
	{0xF, ADF_FW_DC_OBJ},
	{0x100, ADF_FW_ADMIN_OBJ},
};

static const struct adf_fw_config adf_fw_sym_config[] = {
	{0xF0, ADF_FW_SYM_OBJ},
	{0xF, ADF_FW_SYM_OBJ},
	{0x100, ADF_FW_ADMIN_OBJ},
};

static const struct adf_fw_config adf_fw_asym_config[] = {
	{0xF0, ADF_FW_ASYM_OBJ},
	{0xF, ADF_FW_ASYM_OBJ},
	{0x100, ADF_FW_ADMIN_OBJ},
};

static const struct adf_fw_config adf_fw_asym_dc_config[] = {
	{0xF0, ADF_FW_ASYM_OBJ},
	{0xF, ADF_FW_DC_OBJ},
	{0x100, ADF_FW_ADMIN_OBJ},
};

static const struct adf_fw_config adf_fw_sym_dc_config[] = {
	{0xF0, ADF_FW_SYM_OBJ},
	{0xF, ADF_FW_DC_OBJ},
	{0x100, ADF_FW_ADMIN_OBJ},
};

static_assert(ARRAY_SIZE(adf_fw_cy_config) == ARRAY_SIZE(adf_fw_dc_config));
static_assert(ARRAY_SIZE(adf_fw_cy_config) == ARRAY_SIZE(adf_fw_sym_config));
static_assert(ARRAY_SIZE(adf_fw_cy_config) == ARRAY_SIZE(adf_fw_asym_config));
static_assert(ARRAY_SIZE(adf_fw_cy_config) == ARRAY_SIZE(adf_fw_asym_dc_config));
static_assert(ARRAY_SIZE(adf_fw_cy_config) == ARRAY_SIZE(adf_fw_sym_dc_config));

/* Worker thread to service arbiter mappings */
static const u32 default_thrd_to_arb_map[ADF_4XXX_MAX_ACCELENGINES] = {
	0x5555555, 0x5555555, 0x5555555, 0x5555555,
	0xAAAAAAA, 0xAAAAAAA, 0xAAAAAAA, 0xAAAAAAA,
	0x0
};

static const u32 thrd_to_arb_map_dc[ADF_4XXX_MAX_ACCELENGINES] = {
	0x000000FF, 0x000000FF, 0x000000FF, 0x000000FF,
	0x000000FF, 0x000000FF, 0x000000FF, 0x000000FF,
	0x0
};

static struct adf_hw_device_class adf_4xxx_class = {
	.name = ADF_4XXX_DEVICE_NAME,
	.type = DEV_4XXX,
	.instances = 0,
};

enum dev_services {
	SVC_CY = 0,
	SVC_CY2,
	SVC_DC,
	SVC_SYM,
	SVC_ASYM,
	SVC_DC_ASYM,
	SVC_ASYM_DC,
	SVC_DC_SYM,
	SVC_SYM_DC,
};

static const char *const dev_cfg_services[] = {
	[SVC_CY] = ADF_CFG_CY,
	[SVC_CY2] = ADF_CFG_ASYM_SYM,
	[SVC_DC] = ADF_CFG_DC,
	[SVC_SYM] = ADF_CFG_SYM,
	[SVC_ASYM] = ADF_CFG_ASYM,
	[SVC_DC_ASYM] = ADF_CFG_DC_ASYM,
	[SVC_ASYM_DC] = ADF_CFG_ASYM_DC,
	[SVC_DC_SYM] = ADF_CFG_DC_SYM,
	[SVC_SYM_DC] = ADF_CFG_SYM_DC,
};

static int get_service_enabled(struct adf_accel_dev *accel_dev)
{
	char services[ADF_CFG_MAX_VAL_LEN_IN_BYTES] = {0};
	int ret;

	ret = adf_cfg_get_param_value(accel_dev, ADF_GENERAL_SEC,
				      ADF_SERVICES_ENABLED, services);
	if (ret) {
		dev_err(&GET_DEV(accel_dev),
			ADF_SERVICES_ENABLED " param not found\n");
		return ret;
	}

	ret = match_string(dev_cfg_services, ARRAY_SIZE(dev_cfg_services),
			   services);
	if (ret < 0)
		dev_err(&GET_DEV(accel_dev),
			"Invalid value of " ADF_SERVICES_ENABLED " param: %s\n",
			services);

	return ret;
}

static u32 get_accel_mask(struct adf_hw_device_data *self)
{
	return ADF_4XXX_ACCELERATORS_MASK;
}

static u32 get_ae_mask(struct adf_hw_device_data *self)
{
	u32 me_disable = self->fuses;

	return ~me_disable & ADF_4XXX_ACCELENGINES_MASK;
}

static u32 get_num_accels(struct adf_hw_device_data *self)
{
	return ADF_4XXX_MAX_ACCELERATORS;
}

static u32 get_num_aes(struct adf_hw_device_data *self)
{
	if (!self || !self->ae_mask)
		return 0;

	return hweight32(self->ae_mask);
}

static u32 get_misc_bar_id(struct adf_hw_device_data *self)
{
	return ADF_4XXX_PMISC_BAR;
}

static u32 get_etr_bar_id(struct adf_hw_device_data *self)
{
	return ADF_4XXX_ETR_BAR;
}

static u32 get_sram_bar_id(struct adf_hw_device_data *self)
{
	return ADF_4XXX_SRAM_BAR;
}

/*
 * The vector routing table is used to select the MSI-X entry to use for each
 * interrupt source.
 * The first ADF_4XXX_ETR_MAX_BANKS entries correspond to ring interrupts.
 * The final entry corresponds to VF2PF or error interrupts.
 * This vector table could be used to configure one MSI-X entry to be shared
 * between multiple interrupt sources.
 *
 * The default routing is set to have a one to one correspondence between the
 * interrupt source and the MSI-X entry used.
 */
static void set_msix_default_rttable(struct adf_accel_dev *accel_dev)
{
	void __iomem *csr;
	int i;

	csr = (&GET_BARS(accel_dev)[ADF_4XXX_PMISC_BAR])->virt_addr;
	for (i = 0; i <= ADF_4XXX_ETR_MAX_BANKS; i++)
		ADF_CSR_WR(csr, ADF_4XXX_MSIX_RTTABLE_OFFSET(i), i);
}

static u32 get_accel_cap(struct adf_accel_dev *accel_dev)
{
	struct pci_dev *pdev = accel_dev->accel_pci_dev.pci_dev;
	u32 capabilities_sym, capabilities_asym, capabilities_dc;
	u32 fusectl1;

	/* Read accelerator capabilities mask */
	pci_read_config_dword(pdev, ADF_4XXX_FUSECTL1_OFFSET, &fusectl1);

	capabilities_sym = ICP_ACCEL_CAPABILITIES_CRYPTO_SYMMETRIC |
			  ICP_ACCEL_CAPABILITIES_CIPHER |
			  ICP_ACCEL_CAPABILITIES_AUTHENTICATION |
			  ICP_ACCEL_CAPABILITIES_SHA3 |
			  ICP_ACCEL_CAPABILITIES_SHA3_EXT |
			  ICP_ACCEL_CAPABILITIES_HKDF |
			  ICP_ACCEL_CAPABILITIES_CHACHA_POLY |
			  ICP_ACCEL_CAPABILITIES_AESGCM_SPC |
			  ICP_ACCEL_CAPABILITIES_SM3 |
			  ICP_ACCEL_CAPABILITIES_SM4 |
			  ICP_ACCEL_CAPABILITIES_AES_V2;

	/* A set bit in fusectl1 means the feature is OFF in this SKU */
	if (fusectl1 & ICP_ACCEL_4XXX_MASK_CIPHER_SLICE) {
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_CRYPTO_SYMMETRIC;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_HKDF;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_CIPHER;
	}

	if (fusectl1 & ICP_ACCEL_4XXX_MASK_UCS_SLICE) {
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_CHACHA_POLY;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_AESGCM_SPC;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_AES_V2;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_CIPHER;
	}

	if (fusectl1 & ICP_ACCEL_4XXX_MASK_AUTH_SLICE) {
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_AUTHENTICATION;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_SHA3;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_SHA3_EXT;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_CIPHER;
	}

	if (fusectl1 & ICP_ACCEL_4XXX_MASK_SMX_SLICE) {
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_SM3;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_SM4;
	}

	capabilities_asym = ICP_ACCEL_CAPABILITIES_CRYPTO_ASYMMETRIC |
			  ICP_ACCEL_CAPABILITIES_CIPHER |
			  ICP_ACCEL_CAPABILITIES_SM2 |
			  ICP_ACCEL_CAPABILITIES_ECEDMONT;

	if (fusectl1 & ICP_ACCEL_4XXX_MASK_PKE_SLICE) {
		capabilities_asym &= ~ICP_ACCEL_CAPABILITIES_CRYPTO_ASYMMETRIC;
		capabilities_asym &= ~ICP_ACCEL_CAPABILITIES_SM2;
		capabilities_asym &= ~ICP_ACCEL_CAPABILITIES_ECEDMONT;
	}

	capabilities_dc = ICP_ACCEL_CAPABILITIES_COMPRESSION |
			  ICP_ACCEL_CAPABILITIES_LZ4_COMPRESSION |
			  ICP_ACCEL_CAPABILITIES_LZ4S_COMPRESSION |
			  ICP_ACCEL_CAPABILITIES_CNV_INTEGRITY64;

	if (fusectl1 & ICP_ACCEL_4XXX_MASK_COMPRESS_SLICE) {
		capabilities_dc &= ~ICP_ACCEL_CAPABILITIES_COMPRESSION;
		capabilities_dc &= ~ICP_ACCEL_CAPABILITIES_LZ4_COMPRESSION;
		capabilities_dc &= ~ICP_ACCEL_CAPABILITIES_LZ4S_COMPRESSION;
		capabilities_dc &= ~ICP_ACCEL_CAPABILITIES_CNV_INTEGRITY64;
	}

	switch (get_service_enabled(accel_dev)) {
	case SVC_CY:
	case SVC_CY2:
		return capabilities_sym | capabilities_asym;
	case SVC_DC:
		return capabilities_dc;
	case SVC_SYM:
		return capabilities_sym;
	case SVC_ASYM:
		return capabilities_asym;
	case SVC_ASYM_DC:
	case SVC_DC_ASYM:
		return capabilities_asym | capabilities_dc;
	case SVC_SYM_DC:
	case SVC_DC_SYM:
		return capabilities_sym | capabilities_dc;
	default:
		return 0;
	}
}

static enum dev_sku_info get_sku(struct adf_hw_device_data *self)
{
	return DEV_SKU_1;
}

static const u32 *adf_get_arbiter_mapping(struct adf_accel_dev *accel_dev)
{
	switch (get_service_enabled(accel_dev)) {
	case SVC_DC:
		return thrd_to_arb_map_dc;
	default:
		return default_thrd_to_arb_map;
	}
}

static void get_arb_info(struct arb_info *arb_info)
{
	arb_info->arb_cfg = ADF_4XXX_ARB_CONFIG;
	arb_info->arb_offset = ADF_4XXX_ARB_OFFSET;
	arb_info->wt2sam_offset = ADF_4XXX_ARB_WRK_2_SER_MAP_OFFSET;
}

static void get_admin_info(struct admin_info *admin_csrs_info)
{
	admin_csrs_info->mailbox_offset = ADF_4XXX_MAILBOX_BASE_OFFSET;
	admin_csrs_info->admin_msg_ur = ADF_4XXX_ADMINMSGUR_OFFSET;
	admin_csrs_info->admin_msg_lr = ADF_4XXX_ADMINMSGLR_OFFSET;
}

static u32 get_heartbeat_clock(struct adf_hw_device_data *self)
{
	/*
	 * 4XXX uses KPT counter for HB
	 */
	return ADF_4XXX_KPT_COUNTER_FREQ;
}

static void adf_enable_error_correction(struct adf_accel_dev *accel_dev)
{
	struct adf_bar *misc_bar = &GET_BARS(accel_dev)[ADF_4XXX_PMISC_BAR];
	void __iomem *csr = misc_bar->virt_addr;

	/* Enable all in errsou3 except VFLR notification on host */
	ADF_CSR_WR(csr, ADF_GEN4_ERRMSK3, ADF_GEN4_VFLNOTIFY);
}

static void adf_enable_ints(struct adf_accel_dev *accel_dev)
{
	void __iomem *addr;

	addr = (&GET_BARS(accel_dev)[ADF_4XXX_PMISC_BAR])->virt_addr;

	/* Enable bundle interrupts */
	ADF_CSR_WR(addr, ADF_4XXX_SMIAPF_RP_X0_MASK_OFFSET, 0);
	ADF_CSR_WR(addr, ADF_4XXX_SMIAPF_RP_X1_MASK_OFFSET, 0);

	/* Enable misc interrupts */
	ADF_CSR_WR(addr, ADF_4XXX_SMIAPF_MASK_OFFSET, 0);
}

static int adf_init_device(struct adf_accel_dev *accel_dev)
{
	void __iomem *addr;
	u32 status;
	u32 csr;
	int ret;

	addr = (&GET_BARS(accel_dev)[ADF_4XXX_PMISC_BAR])->virt_addr;

	/* Temporarily mask PM interrupt */
	csr = ADF_CSR_RD(addr, ADF_GEN4_ERRMSK2);
	csr |= ADF_GEN4_PM_SOU;
	ADF_CSR_WR(addr, ADF_GEN4_ERRMSK2, csr);

	/* Set DRV_ACTIVE bit to power up the device */
	ADF_CSR_WR(addr, ADF_GEN4_PM_INTERRUPT, ADF_GEN4_PM_DRV_ACTIVE);

	/* Poll status register to make sure the device is powered up */
	ret = read_poll_timeout(ADF_CSR_RD, status,
				status & ADF_GEN4_PM_INIT_STATE,
				ADF_GEN4_PM_POLL_DELAY_US,
				ADF_GEN4_PM_POLL_TIMEOUT_US, true, addr,
				ADF_GEN4_PM_STATUS);
	if (ret)
		dev_err(&GET_DEV(accel_dev), "Failed to power up the device\n");

	return ret;
}

static u32 uof_get_num_objs(void)
{
	return ARRAY_SIZE(adf_fw_cy_config);
}

static const char *uof_get_name(struct adf_accel_dev *accel_dev, u32 obj_num,
				const char * const fw_objs[], int num_objs)
{
	int id;

	switch (get_service_enabled(accel_dev)) {
	case SVC_CY:
	case SVC_CY2:
		id = adf_fw_cy_config[obj_num].obj;
		break;
	case SVC_DC:
		id = adf_fw_dc_config[obj_num].obj;
		break;
	case SVC_SYM:
		id = adf_fw_sym_config[obj_num].obj;
		break;
	case SVC_ASYM:
		id =  adf_fw_asym_config[obj_num].obj;
		break;
	case SVC_ASYM_DC:
	case SVC_DC_ASYM:
		id = adf_fw_asym_dc_config[obj_num].obj;
		break;
	case SVC_SYM_DC:
	case SVC_DC_SYM:
		id = adf_fw_sym_dc_config[obj_num].obj;
		break;
	default:
		id = -EINVAL;
		break;
	}

	if (id < 0 || id > num_objs)
		return NULL;

	return fw_objs[id];
}

static const char *uof_get_name_4xxx(struct adf_accel_dev *accel_dev, u32 obj_num)
{
	int num_fw_objs = ARRAY_SIZE(adf_4xxx_fw_objs);

	return uof_get_name(accel_dev, obj_num, adf_4xxx_fw_objs, num_fw_objs);
}

static const char *uof_get_name_402xx(struct adf_accel_dev *accel_dev, u32 obj_num)
{
	int num_fw_objs = ARRAY_SIZE(adf_402xx_fw_objs);

	return uof_get_name(accel_dev, obj_num, adf_402xx_fw_objs, num_fw_objs);
}

static u32 uof_get_ae_mask(struct adf_accel_dev *accel_dev, u32 obj_num)
{
	switch (get_service_enabled(accel_dev)) {
	case SVC_CY:
		return adf_fw_cy_config[obj_num].ae_mask;
	case SVC_DC:
		return adf_fw_dc_config[obj_num].ae_mask;
	case SVC_CY2:
		return adf_fw_cy_config[obj_num].ae_mask;
	case SVC_SYM:
		return adf_fw_sym_config[obj_num].ae_mask;
	case SVC_ASYM:
		return adf_fw_asym_config[obj_num].ae_mask;
	case SVC_ASYM_DC:
	case SVC_DC_ASYM:
		return adf_fw_asym_dc_config[obj_num].ae_mask;
	case SVC_SYM_DC:
	case SVC_DC_SYM:
		return adf_fw_sym_dc_config[obj_num].ae_mask;
	default:
		return 0;
	}
}

void adf_init_hw_data_4xxx(struct adf_hw_device_data *hw_data, u32 dev_id)
{
	hw_data->dev_class = &adf_4xxx_class;
	hw_data->instance_id = adf_4xxx_class.instances++;
	hw_data->num_banks = ADF_4XXX_ETR_MAX_BANKS;
	hw_data->num_banks_per_vf = ADF_4XXX_NUM_BANKS_PER_VF;
	hw_data->num_rings_per_bank = ADF_4XXX_NUM_RINGS_PER_BANK;
	hw_data->num_accel = ADF_4XXX_MAX_ACCELERATORS;
	hw_data->num_engines = ADF_4XXX_MAX_ACCELENGINES;
	hw_data->num_logical_accel = 1;
	hw_data->tx_rx_gap = ADF_4XXX_RX_RINGS_OFFSET;
	hw_data->tx_rings_mask = ADF_4XXX_TX_RINGS_MASK;
	hw_data->ring_to_svc_map = ADF_GEN4_DEFAULT_RING_TO_SRV_MAP;
	hw_data->alloc_irq = adf_isr_resource_alloc;
	hw_data->free_irq = adf_isr_resource_free;
	hw_data->enable_error_correction = adf_enable_error_correction;
	hw_data->get_accel_mask = get_accel_mask;
	hw_data->get_ae_mask = get_ae_mask;
	hw_data->get_num_accels = get_num_accels;
	hw_data->get_num_aes = get_num_aes;
	hw_data->get_sram_bar_id = get_sram_bar_id;
	hw_data->get_etr_bar_id = get_etr_bar_id;
	hw_data->get_misc_bar_id = get_misc_bar_id;
	hw_data->get_arb_info = get_arb_info;
	hw_data->get_admin_info = get_admin_info;
	hw_data->get_accel_cap = get_accel_cap;
	hw_data->get_sku = get_sku;
	hw_data->init_admin_comms = adf_init_admin_comms;
	hw_data->exit_admin_comms = adf_exit_admin_comms;
	hw_data->send_admin_init = adf_send_admin_init;
	hw_data->init_arb = adf_init_arb;
	hw_data->exit_arb = adf_exit_arb;
	hw_data->get_arb_mapping = adf_get_arbiter_mapping;
	hw_data->enable_ints = adf_enable_ints;
	hw_data->init_device = adf_init_device;
	hw_data->reset_device = adf_reset_flr;
	hw_data->admin_ae_mask = ADF_4XXX_ADMIN_AE_MASK;
	switch (dev_id) {
	case ADF_402XX_PCI_DEVICE_ID:
		hw_data->fw_name = ADF_402XX_FW;
		hw_data->fw_mmp_name = ADF_402XX_MMP;
		hw_data->uof_get_name = uof_get_name_402xx;
		break;

	default:
		hw_data->fw_name = ADF_4XXX_FW;
		hw_data->fw_mmp_name = ADF_4XXX_MMP;
		hw_data->uof_get_name = uof_get_name_4xxx;
	}
	hw_data->uof_get_num_objs = uof_get_num_objs;
	hw_data->uof_get_ae_mask = uof_get_ae_mask;
	hw_data->set_msix_rttable = set_msix_default_rttable;
	hw_data->set_ssm_wdtimer = adf_gen4_set_ssm_wdtimer;
	hw_data->disable_iov = adf_disable_sriov;
	hw_data->ring_pair_reset = adf_gen4_ring_pair_reset;
	hw_data->enable_pm = adf_gen4_enable_pm;
	hw_data->handle_pm_interrupt = adf_gen4_handle_pm_interrupt;
	hw_data->dev_config = adf_gen4_dev_config;
	hw_data->start_timer = adf_gen4_timer_start;
	hw_data->stop_timer = adf_gen4_timer_stop;
	hw_data->get_hb_clock = get_heartbeat_clock;
	hw_data->num_hb_ctrs = ADF_NUM_HB_CNT_PER_AE;

	adf_gen4_init_hw_csr_ops(&hw_data->csr_ops);
	adf_gen4_init_pf_pfvf_ops(&hw_data->pfvf_ops);
	adf_gen4_init_dc_ops(&hw_data->dc_ops);
}

void adf_clean_hw_data_4xxx(struct adf_hw_device_data *hw_data)
{
	hw_data->dev_class->instances--;
}
