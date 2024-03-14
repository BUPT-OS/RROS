// SPDX-License-Identifier: GPL-2.0-only
//
// Components shared between ASoC and HDA CS35L56 drivers
//
// Copyright (C) 2023 Cirrus Logic, Inc. and
//                    Cirrus Logic International Semiconductor Ltd.

#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>

#include "cs35l56.h"

static const struct reg_sequence cs35l56_patch[] = {
	/* These are not reset by a soft-reset, so patch to defaults. */
	{ CS35L56_MAIN_RENDER_USER_MUTE,	0x00000000 },
	{ CS35L56_MAIN_RENDER_USER_VOLUME,	0x00000000 },
	{ CS35L56_MAIN_POSTURE_NUMBER,		0x00000000 },
};

int cs35l56_set_patch(struct cs35l56_base *cs35l56_base)
{
	return regmap_register_patch(cs35l56_base->regmap, cs35l56_patch,
				     ARRAY_SIZE(cs35l56_patch));
}
EXPORT_SYMBOL_NS_GPL(cs35l56_set_patch, SND_SOC_CS35L56_SHARED);

static const struct reg_default cs35l56_reg_defaults[] = {
	{ CS35L56_ASP1_ENABLES1,		0x00000000 },
	{ CS35L56_ASP1_CONTROL1,		0x00000028 },
	{ CS35L56_ASP1_CONTROL2,		0x18180200 },
	{ CS35L56_ASP1_CONTROL3,		0x00000002 },
	{ CS35L56_ASP1_FRAME_CONTROL1,		0x03020100 },
	{ CS35L56_ASP1_FRAME_CONTROL5,		0x00020100 },
	{ CS35L56_ASP1_DATA_CONTROL1,		0x00000018 },
	{ CS35L56_ASP1_DATA_CONTROL5,		0x00000018 },
	{ CS35L56_ASP1TX1_INPUT,		0x00000018 },
	{ CS35L56_ASP1TX2_INPUT,		0x00000019 },
	{ CS35L56_ASP1TX3_INPUT,		0x00000020 },
	{ CS35L56_ASP1TX4_INPUT,		0x00000028 },
	{ CS35L56_SWIRE_DP3_CH1_INPUT,		0x00000018 },
	{ CS35L56_SWIRE_DP3_CH2_INPUT,		0x00000019 },
	{ CS35L56_SWIRE_DP3_CH3_INPUT,		0x00000029 },
	{ CS35L56_SWIRE_DP3_CH4_INPUT,		0x00000028 },
	{ CS35L56_IRQ1_CFG,			0x00000000 },
	{ CS35L56_IRQ1_MASK_1,			0x83ffffff },
	{ CS35L56_IRQ1_MASK_2,			0xffff7fff },
	{ CS35L56_IRQ1_MASK_4,			0xe0ffffff },
	{ CS35L56_IRQ1_MASK_8,			0xfc000fff },
	{ CS35L56_IRQ1_MASK_18,			0x1f7df0ff },
	{ CS35L56_IRQ1_MASK_20,			0x15c00000 },
	{ CS35L56_MAIN_RENDER_USER_MUTE,	0x00000000 },
	{ CS35L56_MAIN_RENDER_USER_VOLUME,	0x00000000 },
	{ CS35L56_MAIN_POSTURE_NUMBER,		0x00000000 },
};

static bool cs35l56_is_dsp_memory(unsigned int reg)
{
	switch (reg) {
	case CS35L56_DSP1_XMEM_PACKED_0 ... CS35L56_DSP1_XMEM_PACKED_6143:
	case CS35L56_DSP1_XMEM_UNPACKED32_0 ... CS35L56_DSP1_XMEM_UNPACKED32_4095:
	case CS35L56_DSP1_XMEM_UNPACKED24_0 ... CS35L56_DSP1_XMEM_UNPACKED24_8191:
	case CS35L56_DSP1_YMEM_PACKED_0 ... CS35L56_DSP1_YMEM_PACKED_4604:
	case CS35L56_DSP1_YMEM_UNPACKED32_0 ... CS35L56_DSP1_YMEM_UNPACKED32_3070:
	case CS35L56_DSP1_YMEM_UNPACKED24_0 ... CS35L56_DSP1_YMEM_UNPACKED24_6141:
	case CS35L56_DSP1_PMEM_0 ... CS35L56_DSP1_PMEM_5114:
		return true;
	default:
		return false;
	}
}

static bool cs35l56_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS35L56_DEVID:
	case CS35L56_REVID:
	case CS35L56_RELID:
	case CS35L56_OTPID:
	case CS35L56_SFT_RESET:
	case CS35L56_GLOBAL_ENABLES:
	case CS35L56_BLOCK_ENABLES:
	case CS35L56_BLOCK_ENABLES2:
	case CS35L56_REFCLK_INPUT:
	case CS35L56_GLOBAL_SAMPLE_RATE:
	case CS35L56_ASP1_ENABLES1:
	case CS35L56_ASP1_CONTROL1:
	case CS35L56_ASP1_CONTROL2:
	case CS35L56_ASP1_CONTROL3:
	case CS35L56_ASP1_FRAME_CONTROL1:
	case CS35L56_ASP1_FRAME_CONTROL5:
	case CS35L56_ASP1_DATA_CONTROL1:
	case CS35L56_ASP1_DATA_CONTROL5:
	case CS35L56_DACPCM1_INPUT:
	case CS35L56_DACPCM2_INPUT:
	case CS35L56_ASP1TX1_INPUT:
	case CS35L56_ASP1TX2_INPUT:
	case CS35L56_ASP1TX3_INPUT:
	case CS35L56_ASP1TX4_INPUT:
	case CS35L56_DSP1RX1_INPUT:
	case CS35L56_DSP1RX2_INPUT:
	case CS35L56_SWIRE_DP3_CH1_INPUT:
	case CS35L56_SWIRE_DP3_CH2_INPUT:
	case CS35L56_SWIRE_DP3_CH3_INPUT:
	case CS35L56_SWIRE_DP3_CH4_INPUT:
	case CS35L56_IRQ1_CFG:
	case CS35L56_IRQ1_STATUS:
	case CS35L56_IRQ1_EINT_1 ... CS35L56_IRQ1_EINT_8:
	case CS35L56_IRQ1_EINT_18:
	case CS35L56_IRQ1_EINT_20:
	case CS35L56_IRQ1_MASK_1:
	case CS35L56_IRQ1_MASK_2:
	case CS35L56_IRQ1_MASK_4:
	case CS35L56_IRQ1_MASK_8:
	case CS35L56_IRQ1_MASK_18:
	case CS35L56_IRQ1_MASK_20:
	case CS35L56_DSP_VIRTUAL1_MBOX_1:
	case CS35L56_DSP_VIRTUAL1_MBOX_2:
	case CS35L56_DSP_VIRTUAL1_MBOX_3:
	case CS35L56_DSP_VIRTUAL1_MBOX_4:
	case CS35L56_DSP_VIRTUAL1_MBOX_5:
	case CS35L56_DSP_VIRTUAL1_MBOX_6:
	case CS35L56_DSP_VIRTUAL1_MBOX_7:
	case CS35L56_DSP_VIRTUAL1_MBOX_8:
	case CS35L56_DSP_RESTRICT_STS1:
	case CS35L56_DSP1_SYS_INFO_ID ... CS35L56_DSP1_SYS_INFO_END:
	case CS35L56_DSP1_AHBM_WINDOW_DEBUG_0:
	case CS35L56_DSP1_AHBM_WINDOW_DEBUG_1:
	case CS35L56_DSP1_SCRATCH1:
	case CS35L56_DSP1_SCRATCH2:
	case CS35L56_DSP1_SCRATCH3:
	case CS35L56_DSP1_SCRATCH4:
		return true;
	default:
		return cs35l56_is_dsp_memory(reg);
	}
}

static bool cs35l56_precious_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS35L56_DSP1_XMEM_PACKED_0 ... CS35L56_DSP1_XMEM_PACKED_6143:
	case CS35L56_DSP1_YMEM_PACKED_0 ... CS35L56_DSP1_YMEM_PACKED_4604:
	case CS35L56_DSP1_PMEM_0 ... CS35L56_DSP1_PMEM_5114:
		return true;
	default:
		return false;
	}
}

static bool cs35l56_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS35L56_DEVID:
	case CS35L56_REVID:
	case CS35L56_RELID:
	case CS35L56_OTPID:
	case CS35L56_SFT_RESET:
	case CS35L56_GLOBAL_ENABLES:		   /* owned by firmware */
	case CS35L56_BLOCK_ENABLES:		   /* owned by firmware */
	case CS35L56_BLOCK_ENABLES2:		   /* owned by firmware */
	case CS35L56_REFCLK_INPUT:		   /* owned by firmware */
	case CS35L56_GLOBAL_SAMPLE_RATE:	   /* owned by firmware */
	case CS35L56_DACPCM1_INPUT:		   /* owned by firmware */
	case CS35L56_DACPCM2_INPUT:		   /* owned by firmware */
	case CS35L56_DSP1RX1_INPUT:		   /* owned by firmware */
	case CS35L56_DSP1RX2_INPUT:		   /* owned by firmware */
	case CS35L56_IRQ1_STATUS:
	case CS35L56_IRQ1_EINT_1 ... CS35L56_IRQ1_EINT_8:
	case CS35L56_IRQ1_EINT_18:
	case CS35L56_IRQ1_EINT_20:
	case CS35L56_DSP_VIRTUAL1_MBOX_1:
	case CS35L56_DSP_VIRTUAL1_MBOX_2:
	case CS35L56_DSP_VIRTUAL1_MBOX_3:
	case CS35L56_DSP_VIRTUAL1_MBOX_4:
	case CS35L56_DSP_VIRTUAL1_MBOX_5:
	case CS35L56_DSP_VIRTUAL1_MBOX_6:
	case CS35L56_DSP_VIRTUAL1_MBOX_7:
	case CS35L56_DSP_VIRTUAL1_MBOX_8:
	case CS35L56_DSP_RESTRICT_STS1:
	case CS35L56_DSP1_SYS_INFO_ID ... CS35L56_DSP1_SYS_INFO_END:
	case CS35L56_DSP1_AHBM_WINDOW_DEBUG_0:
	case CS35L56_DSP1_AHBM_WINDOW_DEBUG_1:
	case CS35L56_DSP1_SCRATCH1:
	case CS35L56_DSP1_SCRATCH2:
	case CS35L56_DSP1_SCRATCH3:
	case CS35L56_DSP1_SCRATCH4:
		return true;
	case CS35L56_MAIN_RENDER_USER_MUTE:
	case CS35L56_MAIN_RENDER_USER_VOLUME:
	case CS35L56_MAIN_POSTURE_NUMBER:
		return false;
	default:
		return cs35l56_is_dsp_memory(reg);
	}
}

int cs35l56_mbox_send(struct cs35l56_base *cs35l56_base, unsigned int command)
{
	unsigned int val;
	int ret;

	regmap_write(cs35l56_base->regmap, CS35L56_DSP_VIRTUAL1_MBOX_1, command);
	ret = regmap_read_poll_timeout(cs35l56_base->regmap, CS35L56_DSP_VIRTUAL1_MBOX_1,
				       val, (val == 0),
				       CS35L56_MBOX_POLL_US, CS35L56_MBOX_TIMEOUT_US);
	if (ret) {
		dev_warn(cs35l56_base->dev, "MBOX command %#x failed: %d\n", command, ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_NS_GPL(cs35l56_mbox_send, SND_SOC_CS35L56_SHARED);

int cs35l56_firmware_shutdown(struct cs35l56_base *cs35l56_base)
{
	int ret;
	unsigned int reg;
	unsigned int val;

	ret = cs35l56_mbox_send(cs35l56_base, CS35L56_MBOX_CMD_SHUTDOWN);
	if (ret)
		return ret;

	if (cs35l56_base->rev < CS35L56_REVID_B0)
		reg = CS35L56_DSP1_PM_CUR_STATE_A1;
	else
		reg = CS35L56_DSP1_PM_CUR_STATE;

	ret = regmap_read_poll_timeout(cs35l56_base->regmap,  reg,
				       val, (val == CS35L56_HALO_STATE_SHUTDOWN),
				       CS35L56_HALO_STATE_POLL_US,
				       CS35L56_HALO_STATE_TIMEOUT_US);
	if (ret < 0)
		dev_err(cs35l56_base->dev, "Failed to poll PM_CUR_STATE to 1 is %d (ret %d)\n",
			val, ret);
	return ret;
}
EXPORT_SYMBOL_NS_GPL(cs35l56_firmware_shutdown, SND_SOC_CS35L56_SHARED);

int cs35l56_wait_for_firmware_boot(struct cs35l56_base *cs35l56_base)
{
	unsigned int reg;
	unsigned int val;
	int read_ret, poll_ret;

	if (cs35l56_base->rev < CS35L56_REVID_B0)
		reg = CS35L56_DSP1_HALO_STATE_A1;
	else
		reg = CS35L56_DSP1_HALO_STATE;

	/*
	 * This can't be a regmap_read_poll_timeout() because cs35l56 will NAK
	 * I2C until it has booted which would terminate the poll
	 */
	poll_ret = read_poll_timeout(regmap_read, read_ret,
				     (val < 0xFFFF) && (val >= CS35L56_HALO_STATE_BOOT_DONE),
				     CS35L56_HALO_STATE_POLL_US,
				     CS35L56_HALO_STATE_TIMEOUT_US,
				     false,
				     cs35l56_base->regmap, reg, &val);

	if (poll_ret) {
		dev_err(cs35l56_base->dev, "Firmware boot timed out(%d): HALO_STATE=%#x\n",
			read_ret, val);
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL_NS_GPL(cs35l56_wait_for_firmware_boot, SND_SOC_CS35L56_SHARED);

void cs35l56_wait_control_port_ready(void)
{
	/* Wait for control port to be ready (datasheet tIRS). */
	usleep_range(CS35L56_CONTROL_PORT_READY_US, 2 * CS35L56_CONTROL_PORT_READY_US);
}
EXPORT_SYMBOL_NS_GPL(cs35l56_wait_control_port_ready, SND_SOC_CS35L56_SHARED);

void cs35l56_wait_min_reset_pulse(void)
{
	/* Satisfy minimum reset pulse width spec */
	usleep_range(CS35L56_RESET_PULSE_MIN_US, 2 * CS35L56_RESET_PULSE_MIN_US);
}
EXPORT_SYMBOL_NS_GPL(cs35l56_wait_min_reset_pulse, SND_SOC_CS35L56_SHARED);

static const struct reg_sequence cs35l56_system_reset_seq[] = {
	REG_SEQ0(CS35L56_DSP_VIRTUAL1_MBOX_1, CS35L56_MBOX_CMD_SYSTEM_RESET),
};

void cs35l56_system_reset(struct cs35l56_base *cs35l56_base, bool is_soundwire)
{
	/*
	 * Must enter cache-only first so there can't be any more register
	 * accesses other than the controlled system reset sequence below.
	 */
	regcache_cache_only(cs35l56_base->regmap, true);
	regmap_multi_reg_write_bypassed(cs35l56_base->regmap,
					cs35l56_system_reset_seq,
					ARRAY_SIZE(cs35l56_system_reset_seq));

	/* On SoundWire the registers won't be accessible until it re-enumerates. */
	if (is_soundwire)
		return;

	cs35l56_wait_control_port_ready();
	regcache_cache_only(cs35l56_base->regmap, false);
}
EXPORT_SYMBOL_NS_GPL(cs35l56_system_reset, SND_SOC_CS35L56_SHARED);

int cs35l56_irq_request(struct cs35l56_base *cs35l56_base, int irq)
{
	int ret;

	if (!irq)
		return 0;

	ret = devm_request_threaded_irq(cs35l56_base->dev, irq, NULL, cs35l56_irq,
					IRQF_ONESHOT | IRQF_SHARED | IRQF_TRIGGER_LOW,
					"cs35l56", cs35l56_base);
	if (!ret)
		cs35l56_base->irq = irq;
	else
		dev_err(cs35l56_base->dev, "Failed to get IRQ: %d\n", ret);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(cs35l56_irq_request, SND_SOC_CS35L56_SHARED);

irqreturn_t cs35l56_irq(int irq, void *data)
{
	struct cs35l56_base *cs35l56_base = data;
	unsigned int status1 = 0, status8 = 0, status20 = 0;
	unsigned int mask1, mask8, mask20;
	unsigned int val;
	int rv;

	irqreturn_t ret = IRQ_NONE;

	if (!cs35l56_base->init_done)
		return IRQ_NONE;

	mutex_lock(&cs35l56_base->irq_lock);

	rv = pm_runtime_resume_and_get(cs35l56_base->dev);
	if (rv < 0) {
		dev_err(cs35l56_base->dev, "irq: failed to get pm_runtime: %d\n", rv);
		goto err_unlock;
	}

	regmap_read(cs35l56_base->regmap, CS35L56_IRQ1_STATUS, &val);
	if ((val & CS35L56_IRQ1_STS_MASK) == 0) {
		dev_dbg(cs35l56_base->dev, "Spurious IRQ: no pending interrupt\n");
		goto err;
	}

	/* Ack interrupts */
	regmap_read(cs35l56_base->regmap, CS35L56_IRQ1_EINT_1, &status1);
	regmap_read(cs35l56_base->regmap, CS35L56_IRQ1_MASK_1, &mask1);
	status1 &= ~mask1;
	regmap_write(cs35l56_base->regmap, CS35L56_IRQ1_EINT_1, status1);

	regmap_read(cs35l56_base->regmap, CS35L56_IRQ1_EINT_8, &status8);
	regmap_read(cs35l56_base->regmap, CS35L56_IRQ1_MASK_8, &mask8);
	status8 &= ~mask8;
	regmap_write(cs35l56_base->regmap, CS35L56_IRQ1_EINT_8, status8);

	regmap_read(cs35l56_base->regmap, CS35L56_IRQ1_EINT_20, &status20);
	regmap_read(cs35l56_base->regmap, CS35L56_IRQ1_MASK_20, &mask20);
	status20 &= ~mask20;
	/* We don't want EINT20 but they default to unmasked: force mask */
	regmap_write(cs35l56_base->regmap, CS35L56_IRQ1_MASK_20, 0xffffffff);

	dev_dbg(cs35l56_base->dev, "%s: %#x %#x\n", __func__, status1, status8);

	/* Check to see if unmasked bits are active */
	if (!status1 && !status8 && !status20)
		goto err;

	if (status1 & CS35L56_AMP_SHORT_ERR_EINT1_MASK)
		dev_crit(cs35l56_base->dev, "Amp short error\n");

	if (status8 & CS35L56_TEMP_ERR_EINT1_MASK)
		dev_crit(cs35l56_base->dev, "Overtemp error\n");

	ret = IRQ_HANDLED;

err:
	pm_runtime_put(cs35l56_base->dev);
err_unlock:
	mutex_unlock(&cs35l56_base->irq_lock);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(cs35l56_irq, SND_SOC_CS35L56_SHARED);

int cs35l56_is_fw_reload_needed(struct cs35l56_base *cs35l56_base)
{
	unsigned int val;
	int ret;

	/* Nothing to re-patch if we haven't done any patching yet. */
	if (!cs35l56_base->fw_patched)
		return false;

	/*
	 * If we have control of RESET we will have asserted it so the firmware
	 * will need re-patching.
	 */
	if (cs35l56_base->reset_gpio)
		return true;

	/*
	 * In secure mode FIRMWARE_MISSING is cleared by the BIOS loader so
	 * can't be used here to test for memory retention.
	 * Assume that tuning must be re-loaded.
	 */
	if (cs35l56_base->secured)
		return true;

	ret = pm_runtime_resume_and_get(cs35l56_base->dev);
	if (ret) {
		dev_err(cs35l56_base->dev, "Failed to runtime_get: %d\n", ret);
		return ret;
	}

	ret = regmap_read(cs35l56_base->regmap, CS35L56_PROTECTION_STATUS, &val);
	if (ret)
		dev_err(cs35l56_base->dev, "Failed to read PROTECTION_STATUS: %d\n", ret);
	else
		ret = !!(val & CS35L56_FIRMWARE_MISSING);

	pm_runtime_put_autosuspend(cs35l56_base->dev);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(cs35l56_is_fw_reload_needed, SND_SOC_CS35L56_SHARED);

static const struct reg_sequence cs35l56_hibernate_seq[] = {
	/* This must be the last register access */
	REG_SEQ0(CS35L56_DSP_VIRTUAL1_MBOX_1, CS35L56_MBOX_CMD_HIBERNATE_NOW),
};

static const struct reg_sequence cs35l56_hibernate_wake_seq[] = {
	REG_SEQ0(CS35L56_DSP_VIRTUAL1_MBOX_1, CS35L56_MBOX_CMD_WAKEUP),
};

int cs35l56_runtime_suspend_common(struct cs35l56_base *cs35l56_base)
{
	unsigned int val;
	int ret;

	if (!cs35l56_base->init_done)
		return 0;

	/* Firmware must have entered a power-save state */
	ret = regmap_read_poll_timeout(cs35l56_base->regmap,
				       CS35L56_TRANSDUCER_ACTUAL_PS,
				       val, (val >= CS35L56_PS3),
				       CS35L56_PS3_POLL_US,
				       CS35L56_PS3_TIMEOUT_US);
	if (ret)
		dev_warn(cs35l56_base->dev, "PS3 wait failed: %d\n", ret);

	/* Clear BOOT_DONE so it can be used to detect a reboot */
	regmap_write(cs35l56_base->regmap, CS35L56_IRQ1_EINT_4, CS35L56_OTP_BOOT_DONE_MASK);

	if (!cs35l56_base->can_hibernate) {
		regcache_cache_only(cs35l56_base->regmap, true);
		dev_dbg(cs35l56_base->dev, "Suspended: no hibernate");

		return 0;
	}

	/*
	 * Enable auto-hibernate. If it is woken by some other wake source
	 * it will automatically return to hibernate.
	 */
	cs35l56_mbox_send(cs35l56_base, CS35L56_MBOX_CMD_ALLOW_AUTO_HIBERNATE);

	/*
	 * Must enter cache-only first so there can't be any more register
	 * accesses other than the controlled hibernate sequence below.
	 */
	regcache_cache_only(cs35l56_base->regmap, true);

	regmap_multi_reg_write_bypassed(cs35l56_base->regmap,
					cs35l56_hibernate_seq,
					ARRAY_SIZE(cs35l56_hibernate_seq));

	dev_dbg(cs35l56_base->dev, "Suspended: hibernate");

	return 0;
}
EXPORT_SYMBOL_NS_GPL(cs35l56_runtime_suspend_common, SND_SOC_CS35L56_SHARED);

int cs35l56_runtime_resume_common(struct cs35l56_base *cs35l56_base, bool is_soundwire)
{
	unsigned int val;
	int ret;

	if (!cs35l56_base->init_done)
		return 0;

	if (!cs35l56_base->can_hibernate)
		goto out_sync;

	if (!is_soundwire) {
		/*
		 * Dummy transaction to trigger I2C/SPI auto-wake. This will NAK on I2C.
		 * Must be done before releasing cache-only.
		 */
		regmap_multi_reg_write_bypassed(cs35l56_base->regmap,
						cs35l56_hibernate_wake_seq,
						ARRAY_SIZE(cs35l56_hibernate_wake_seq));

		cs35l56_wait_control_port_ready();
	}

out_sync:
	regcache_cache_only(cs35l56_base->regmap, false);

	ret = cs35l56_wait_for_firmware_boot(cs35l56_base);
	if (ret) {
		dev_err(cs35l56_base->dev, "Hibernate wake failed: %d\n", ret);
		goto err;
	}

	ret = cs35l56_mbox_send(cs35l56_base, CS35L56_MBOX_CMD_PREVENT_AUTO_HIBERNATE);
	if (ret)
		goto err;

	/* BOOT_DONE will be 1 if the amp reset */
	regmap_read(cs35l56_base->regmap, CS35L56_IRQ1_EINT_4, &val);
	if (val & CS35L56_OTP_BOOT_DONE_MASK) {
		dev_dbg(cs35l56_base->dev, "Registers reset in suspend\n");
		regcache_mark_dirty(cs35l56_base->regmap);
	}

	regcache_sync(cs35l56_base->regmap);

	dev_dbg(cs35l56_base->dev, "Resumed");

	return 0;

err:
	regmap_write(cs35l56_base->regmap, CS35L56_DSP_VIRTUAL1_MBOX_1,
		     CS35L56_MBOX_CMD_HIBERNATE_NOW);

	regcache_cache_only(cs35l56_base->regmap, true);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(cs35l56_runtime_resume_common, SND_SOC_CS35L56_SHARED);

static const struct cs_dsp_region cs35l56_dsp1_regions[] = {
	{ .type = WMFW_HALO_PM_PACKED,	.base = CS35L56_DSP1_PMEM_0 },
	{ .type = WMFW_HALO_XM_PACKED,	.base = CS35L56_DSP1_XMEM_PACKED_0 },
	{ .type = WMFW_HALO_YM_PACKED,	.base = CS35L56_DSP1_YMEM_PACKED_0 },
	{ .type = WMFW_ADSP2_XM,	.base = CS35L56_DSP1_XMEM_UNPACKED24_0 },
	{ .type = WMFW_ADSP2_YM,	.base = CS35L56_DSP1_YMEM_UNPACKED24_0 },
};

void cs35l56_init_cs_dsp(struct cs35l56_base *cs35l56_base, struct cs_dsp *cs_dsp)
{
	cs_dsp->num = 1;
	cs_dsp->type = WMFW_HALO;
	cs_dsp->rev = 0;
	cs_dsp->dev = cs35l56_base->dev;
	cs_dsp->regmap = cs35l56_base->regmap;
	cs_dsp->base = CS35L56_DSP1_CORE_BASE;
	cs_dsp->base_sysinfo = CS35L56_DSP1_SYS_INFO_ID;
	cs_dsp->mem = cs35l56_dsp1_regions;
	cs_dsp->num_mems = ARRAY_SIZE(cs35l56_dsp1_regions);
	cs_dsp->no_core_startstop = true;
}
EXPORT_SYMBOL_NS_GPL(cs35l56_init_cs_dsp, SND_SOC_CS35L56_SHARED);

int cs35l56_hw_init(struct cs35l56_base *cs35l56_base)
{
	int ret;
	unsigned int devid, revid, otpid, secured;

	/*
	 * If the system is not using a reset_gpio then issue a
	 * dummy read to force a wakeup.
	 */
	if (!cs35l56_base->reset_gpio)
		regmap_read(cs35l56_base->regmap, CS35L56_DSP_VIRTUAL1_MBOX_1, &devid);

	cs35l56_wait_control_port_ready();

	/*
	 * The HALO_STATE register is in different locations on Ax and B0
	 * devices so the REVID needs to be determined before waiting for the
	 * firmware to boot.
	 */
	ret = regmap_read(cs35l56_base->regmap, CS35L56_REVID, &revid);
	if (ret < 0) {
		dev_err(cs35l56_base->dev, "Get Revision ID failed\n");
		return ret;
	}
	cs35l56_base->rev = revid & (CS35L56_AREVID_MASK | CS35L56_MTLREVID_MASK);

	ret = cs35l56_wait_for_firmware_boot(cs35l56_base);
	if (ret)
		return ret;

	ret = regmap_read(cs35l56_base->regmap, CS35L56_DEVID, &devid);
	if (ret < 0) {
		dev_err(cs35l56_base->dev, "Get Device ID failed\n");
		return ret;
	}
	devid &= CS35L56_DEVID_MASK;

	switch (devid) {
	case 0x35A56:
		break;
	default:
		dev_err(cs35l56_base->dev, "Unknown device %x\n", devid);
		return ret;
	}

	ret = regmap_read(cs35l56_base->regmap, CS35L56_DSP_RESTRICT_STS1, &secured);
	if (ret) {
		dev_err(cs35l56_base->dev, "Get Secure status failed\n");
		return ret;
	}

	/* When any bus is restricted treat the device as secured */
	if (secured & CS35L56_RESTRICTED_MASK)
		cs35l56_base->secured = true;

	ret = regmap_read(cs35l56_base->regmap, CS35L56_OTPID, &otpid);
	if (ret < 0) {
		dev_err(cs35l56_base->dev, "Get OTP ID failed\n");
		return ret;
	}

	dev_info(cs35l56_base->dev, "Cirrus Logic CS35L56%s Rev %02X OTP%d\n",
		 cs35l56_base->secured ? "s" : "", cs35l56_base->rev, otpid);

	/* Wake source and *_BLOCKED interrupts default to unmasked, so mask them */
	regmap_write(cs35l56_base->regmap, CS35L56_IRQ1_MASK_20, 0xffffffff);
	regmap_update_bits(cs35l56_base->regmap, CS35L56_IRQ1_MASK_1,
			   CS35L56_AMP_SHORT_ERR_EINT1_MASK,
			   0);
	regmap_update_bits(cs35l56_base->regmap, CS35L56_IRQ1_MASK_8,
			   CS35L56_TEMP_ERR_EINT1_MASK,
			   0);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(cs35l56_hw_init, SND_SOC_CS35L56_SHARED);

static const u32 cs35l56_bclk_valid_for_pll_freq_table[] = {
	[0x0C] = 128000,
	[0x0F] = 256000,
	[0x11] = 384000,
	[0x12] = 512000,
	[0x15] = 768000,
	[0x17] = 1024000,
	[0x1A] = 1500000,
	[0x1B] = 1536000,
	[0x1C] = 2000000,
	[0x1D] = 2048000,
	[0x1E] = 2400000,
	[0x20] = 3000000,
	[0x21] = 3072000,
	[0x23] = 4000000,
	[0x24] = 4096000,
	[0x25] = 4800000,
	[0x27] = 6000000,
	[0x28] = 6144000,
	[0x29] = 6250000,
	[0x2A] = 6400000,
	[0x2E] = 8000000,
	[0x2F] = 8192000,
	[0x30] = 9600000,
	[0x32] = 12000000,
	[0x33] = 12288000,
	[0x37] = 13500000,
	[0x38] = 19200000,
	[0x39] = 22579200,
	[0x3B] = 24576000,
};

int cs35l56_get_bclk_freq_id(unsigned int freq)
{
	int i;

	if (freq == 0)
		return -EINVAL;

	/* The BCLK frequency must be a valid PLL REFCLK */
	for (i = 0; i < ARRAY_SIZE(cs35l56_bclk_valid_for_pll_freq_table); ++i) {
		if (cs35l56_bclk_valid_for_pll_freq_table[i] == freq)
			return i;
	}

	return -EINVAL;
}
EXPORT_SYMBOL_NS_GPL(cs35l56_get_bclk_freq_id, SND_SOC_CS35L56_SHARED);

static const char * const cs35l56_supplies[/* auto-sized */] = {
	"VDD_P",
	"VDD_IO",
	"VDD_A",
};

void cs35l56_fill_supply_names(struct regulator_bulk_data *data)
{
	int i;

	BUILD_BUG_ON(ARRAY_SIZE(cs35l56_supplies) != CS35L56_NUM_BULK_SUPPLIES);
	for (i = 0; i < ARRAY_SIZE(cs35l56_supplies); i++)
		data[i].supply = cs35l56_supplies[i];
}
EXPORT_SYMBOL_NS_GPL(cs35l56_fill_supply_names, SND_SOC_CS35L56_SHARED);

const char * const cs35l56_tx_input_texts[] = {
	"None", "ASP1RX1", "ASP1RX2", "VMON", "IMON", "ERRVOL", "CLASSH",
	"VDDBMON", "VBSTMON", "DSP1TX1", "DSP1TX2", "DSP1TX3", "DSP1TX4",
	"DSP1TX5", "DSP1TX6", "DSP1TX7", "DSP1TX8", "TEMPMON",
	"INTERPOLATOR", "SDW1RX1", "SDW1RX2",
};
EXPORT_SYMBOL_NS_GPL(cs35l56_tx_input_texts, SND_SOC_CS35L56_SHARED);

const unsigned int cs35l56_tx_input_values[] = {
	CS35L56_INPUT_SRC_NONE,
	CS35L56_INPUT_SRC_ASP1RX1,
	CS35L56_INPUT_SRC_ASP1RX2,
	CS35L56_INPUT_SRC_VMON,
	CS35L56_INPUT_SRC_IMON,
	CS35L56_INPUT_SRC_ERR_VOL,
	CS35L56_INPUT_SRC_CLASSH,
	CS35L56_INPUT_SRC_VDDBMON,
	CS35L56_INPUT_SRC_VBSTMON,
	CS35L56_INPUT_SRC_DSP1TX1,
	CS35L56_INPUT_SRC_DSP1TX2,
	CS35L56_INPUT_SRC_DSP1TX3,
	CS35L56_INPUT_SRC_DSP1TX4,
	CS35L56_INPUT_SRC_DSP1TX5,
	CS35L56_INPUT_SRC_DSP1TX6,
	CS35L56_INPUT_SRC_DSP1TX7,
	CS35L56_INPUT_SRC_DSP1TX8,
	CS35L56_INPUT_SRC_TEMPMON,
	CS35L56_INPUT_SRC_INTERPOLATOR,
	CS35L56_INPUT_SRC_SWIRE_DP1_CHANNEL1,
	CS35L56_INPUT_SRC_SWIRE_DP1_CHANNEL2,
};
EXPORT_SYMBOL_NS_GPL(cs35l56_tx_input_values, SND_SOC_CS35L56_SHARED);

struct regmap_config cs35l56_regmap_i2c = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.max_register = CS35L56_DSP1_PMEM_5114,
	.reg_defaults = cs35l56_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(cs35l56_reg_defaults),
	.volatile_reg = cs35l56_volatile_reg,
	.readable_reg = cs35l56_readable_reg,
	.precious_reg = cs35l56_precious_reg,
	.cache_type = REGCACHE_MAPLE,
};
EXPORT_SYMBOL_NS_GPL(cs35l56_regmap_i2c, SND_SOC_CS35L56_SHARED);

struct regmap_config cs35l56_regmap_spi = {
	.reg_bits = 32,
	.val_bits = 32,
	.pad_bits = 16,
	.reg_stride = 4,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.max_register = CS35L56_DSP1_PMEM_5114,
	.reg_defaults = cs35l56_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(cs35l56_reg_defaults),
	.volatile_reg = cs35l56_volatile_reg,
	.readable_reg = cs35l56_readable_reg,
	.precious_reg = cs35l56_precious_reg,
	.cache_type = REGCACHE_MAPLE,
};
EXPORT_SYMBOL_NS_GPL(cs35l56_regmap_spi, SND_SOC_CS35L56_SHARED);

struct regmap_config cs35l56_regmap_sdw = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.max_register = CS35L56_DSP1_PMEM_5114,
	.reg_defaults = cs35l56_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(cs35l56_reg_defaults),
	.volatile_reg = cs35l56_volatile_reg,
	.readable_reg = cs35l56_readable_reg,
	.precious_reg = cs35l56_precious_reg,
	.cache_type = REGCACHE_MAPLE,
};
EXPORT_SYMBOL_NS_GPL(cs35l56_regmap_sdw, SND_SOC_CS35L56_SHARED);

MODULE_DESCRIPTION("ASoC CS35L56 Shared");
MODULE_AUTHOR("Richard Fitzgerald <rf@opensource.cirrus.com>");
MODULE_AUTHOR("Simon Trimmer <simont@opensource.cirrus.com>");
MODULE_LICENSE("GPL");
