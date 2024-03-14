// SPDX-License-Identifier: GPL-2.0-only
//
// HDA audio driver for Cirrus Logic CS35L56 smart amp
//
// Copyright (C) 2023 Cirrus Logic, Inc. and
//                    Cirrus Logic International Semiconductor Ltd.
//

#include <linux/acpi.h>
#include <linux/debugfs.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/hda_codec.h>
#include <sound/tlv.h>
#include "cs35l56_hda.h"
#include "hda_component.h"
#include "hda_cs_dsp_ctl.h"
#include "hda_generic.h"

 /*
  * The cs35l56_hda_dai_config[] reg sequence configures the device as
  *  ASP1_BCLK_FREQ = 3.072 MHz
  *  ASP1_RX_WIDTH = 32 cycles per slot, ASP1_TX_WIDTH = 32 cycles per slot, ASP1_FMT = I2S
  *  ASP1_DOUT_HIZ_CONTROL = Hi-Z during unused timeslots
  *  ASP1_RX_WL = 24 bits per sample
  *  ASP1_TX_WL = 24 bits per sample
  *  ASP1_RXn_EN 1..3 and ASP1_TXn_EN 1..4 disabled
  */
static const struct reg_sequence cs35l56_hda_dai_config[] = {
	{ CS35L56_ASP1_CONTROL1,	0x00000021 },
	{ CS35L56_ASP1_CONTROL2,	0x20200200 },
	{ CS35L56_ASP1_CONTROL3,	0x00000003 },
	{ CS35L56_ASP1_DATA_CONTROL5,	0x00000018 },
	{ CS35L56_ASP1_DATA_CONTROL1,	0x00000018 },
	{ CS35L56_ASP1_ENABLES1,	0x00000000 },
};

static void cs35l56_hda_play(struct cs35l56_hda *cs35l56)
{
	unsigned int val;
	int ret;

	pm_runtime_get_sync(cs35l56->base.dev);
	ret = cs35l56_mbox_send(&cs35l56->base, CS35L56_MBOX_CMD_AUDIO_PLAY);
	if (ret == 0) {
		/* Wait for firmware to enter PS0 power state */
		ret = regmap_read_poll_timeout(cs35l56->base.regmap,
					       CS35L56_TRANSDUCER_ACTUAL_PS,
					       val, (val == CS35L56_PS0),
					       CS35L56_PS0_POLL_US,
					       CS35L56_PS0_TIMEOUT_US);
		if (ret)
			dev_warn(cs35l56->base.dev, "PS0 wait failed: %d\n", ret);
	}
	regmap_set_bits(cs35l56->base.regmap, CS35L56_ASP1_ENABLES1,
			BIT(CS35L56_ASP_RX1_EN_SHIFT) | BIT(CS35L56_ASP_RX2_EN_SHIFT) |
			cs35l56->asp_tx_mask);
	cs35l56->playing = true;
}

static void cs35l56_hda_pause(struct cs35l56_hda *cs35l56)
{
	cs35l56->playing = false;
	cs35l56_mbox_send(&cs35l56->base, CS35L56_MBOX_CMD_AUDIO_PAUSE);
	regmap_clear_bits(cs35l56->base.regmap, CS35L56_ASP1_ENABLES1,
			  BIT(CS35L56_ASP_RX1_EN_SHIFT) | BIT(CS35L56_ASP_RX2_EN_SHIFT) |
			  BIT(CS35L56_ASP_TX1_EN_SHIFT) | BIT(CS35L56_ASP_TX2_EN_SHIFT) |
			  BIT(CS35L56_ASP_TX3_EN_SHIFT) | BIT(CS35L56_ASP_TX4_EN_SHIFT));

	pm_runtime_mark_last_busy(cs35l56->base.dev);
	pm_runtime_put_autosuspend(cs35l56->base.dev);
}

static void cs35l56_hda_playback_hook(struct device *dev, int action)
{
	struct cs35l56_hda *cs35l56 = dev_get_drvdata(dev);

	dev_dbg(cs35l56->base.dev, "%s()%d: action: %d\n", __func__, __LINE__, action);

	switch (action) {
	case HDA_GEN_PCM_ACT_PREPARE:
		if (cs35l56->playing)
			break;

		/* If we're suspended: flag that resume should start playback */
		if (cs35l56->suspended) {
			cs35l56->playing = true;
			break;
		}

		cs35l56_hda_play(cs35l56);
		break;
	case HDA_GEN_PCM_ACT_CLEANUP:
		if (!cs35l56->playing)
			break;

		cs35l56_hda_pause(cs35l56);
		break;
	default:
		break;
	}
}

static int cs35l56_hda_runtime_suspend(struct device *dev)
{
	struct cs35l56_hda *cs35l56 = dev_get_drvdata(dev);

	if (cs35l56->cs_dsp.booted)
		cs_dsp_stop(&cs35l56->cs_dsp);

	return cs35l56_runtime_suspend_common(&cs35l56->base);
}

static int cs35l56_hda_runtime_resume(struct device *dev)
{
	struct cs35l56_hda *cs35l56 = dev_get_drvdata(dev);
	int ret;

	ret = cs35l56_runtime_resume_common(&cs35l56->base, false);
	if (ret < 0)
		return ret;

	if (cs35l56->cs_dsp.booted) {
		ret = cs_dsp_run(&cs35l56->cs_dsp);
		if (ret) {
			dev_dbg(cs35l56->base.dev, "%s: cs_dsp_run ret %d\n", __func__, ret);
			goto err;
		}
	}

	return 0;

err:
	cs35l56_mbox_send(&cs35l56->base, CS35L56_MBOX_CMD_ALLOW_AUTO_HIBERNATE);
	regmap_write(cs35l56->base.regmap, CS35L56_DSP_VIRTUAL1_MBOX_1,
		     CS35L56_MBOX_CMD_HIBERNATE_NOW);

	regcache_cache_only(cs35l56->base.regmap, true);

	return ret;
}

static int cs35l56_hda_mixer_info(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = CS35L56_NUM_INPUT_SRC;
	if (uinfo->value.enumerated.item >= CS35L56_NUM_INPUT_SRC)
		uinfo->value.enumerated.item = CS35L56_NUM_INPUT_SRC - 1;
	strscpy(uinfo->value.enumerated.name, cs35l56_tx_input_texts[uinfo->value.enumerated.item],
		sizeof(uinfo->value.enumerated.name));

	return 0;
}

static int cs35l56_hda_mixer_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct cs35l56_hda *cs35l56 = (struct cs35l56_hda *)kcontrol->private_data;
	unsigned int reg_val;
	int i;

	regmap_read(cs35l56->base.regmap, kcontrol->private_value, &reg_val);
	reg_val &= CS35L56_ASP_TXn_SRC_MASK;

	for (i = 0; i < CS35L56_NUM_INPUT_SRC; ++i) {
		if (cs35l56_tx_input_values[i] == reg_val) {
			ucontrol->value.enumerated.item[0] = i;
			break;
		}
	}

	return 0;
}

static int cs35l56_hda_mixer_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct cs35l56_hda *cs35l56 = (struct cs35l56_hda *)kcontrol->private_data;
	unsigned int item = ucontrol->value.enumerated.item[0];
	bool changed;

	if (item >= CS35L56_NUM_INPUT_SRC)
		return -EINVAL;

	regmap_update_bits_check(cs35l56->base.regmap, kcontrol->private_value,
				 CS35L56_INPUT_MASK, cs35l56_tx_input_values[item],
				 &changed);

	return changed;
}

static int cs35l56_hda_posture_info(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = CS35L56_MAIN_POSTURE_MIN;
	uinfo->value.integer.max = CS35L56_MAIN_POSTURE_MAX;
	return 0;
}

static int cs35l56_hda_posture_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct cs35l56_hda *cs35l56 = (struct cs35l56_hda *)kcontrol->private_data;
	unsigned int pos;
	int ret;

	ret = regmap_read(cs35l56->base.regmap, CS35L56_MAIN_POSTURE_NUMBER, &pos);
	if (ret)
		return ret;

	ucontrol->value.integer.value[0] = pos;

	return 0;
}

static int cs35l56_hda_posture_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct cs35l56_hda *cs35l56 = (struct cs35l56_hda *)kcontrol->private_data;
	unsigned long pos = ucontrol->value.integer.value[0];
	bool changed;
	int ret;

	if ((pos < CS35L56_MAIN_POSTURE_MIN) ||
	    (pos > CS35L56_MAIN_POSTURE_MAX))
		return -EINVAL;

	ret = regmap_update_bits_check(cs35l56->base.regmap,
				       CS35L56_MAIN_POSTURE_NUMBER,
				       CS35L56_MAIN_POSTURE_MASK,
				       pos, &changed);
	if (ret)
		return ret;

	return changed;
}

static const struct {
	const char *name;
	unsigned int reg;
} cs35l56_hda_mixer_controls[] = {
	{ "ASP1 TX1 Source", CS35L56_ASP1TX1_INPUT },
	{ "ASP1 TX2 Source", CS35L56_ASP1TX2_INPUT },
	{ "ASP1 TX3 Source", CS35L56_ASP1TX3_INPUT },
	{ "ASP1 TX4 Source", CS35L56_ASP1TX4_INPUT },
};

static const DECLARE_TLV_DB_SCALE(cs35l56_hda_vol_tlv, -10000, 25, 0);

static int cs35l56_hda_vol_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.step = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = CS35L56_MAIN_RENDER_USER_VOLUME_MAX -
				   CS35L56_MAIN_RENDER_USER_VOLUME_MIN;

	return 0;
}

static int cs35l56_hda_vol_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct cs35l56_hda *cs35l56 = (struct cs35l56_hda *)kcontrol->private_data;
	unsigned int raw_vol;
	int vol;
	int ret;

	ret = regmap_read(cs35l56->base.regmap, CS35L56_MAIN_RENDER_USER_VOLUME, &raw_vol);

	if (ret)
		return ret;

	vol = (s16)(raw_vol & 0xFFFF);
	vol >>= CS35L56_MAIN_RENDER_USER_VOLUME_SHIFT;

	if (vol & BIT(CS35L56_MAIN_RENDER_USER_VOLUME_SIGNBIT))
		vol |= ~((int)(BIT(CS35L56_MAIN_RENDER_USER_VOLUME_SIGNBIT) - 1));

	ucontrol->value.integer.value[0] = vol - CS35L56_MAIN_RENDER_USER_VOLUME_MIN;

	return 0;
}

static int cs35l56_hda_vol_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct cs35l56_hda *cs35l56 = (struct cs35l56_hda *)kcontrol->private_data;
	long vol = ucontrol->value.integer.value[0];
	unsigned int raw_vol;
	bool changed;
	int ret;

	if ((vol < 0) || (vol > (CS35L56_MAIN_RENDER_USER_VOLUME_MAX -
				 CS35L56_MAIN_RENDER_USER_VOLUME_MIN)))
		return -EINVAL;

	raw_vol = (vol + CS35L56_MAIN_RENDER_USER_VOLUME_MIN) <<
		  CS35L56_MAIN_RENDER_USER_VOLUME_SHIFT;

	ret = regmap_update_bits_check(cs35l56->base.regmap,
				       CS35L56_MAIN_RENDER_USER_VOLUME,
				       CS35L56_MAIN_RENDER_USER_VOLUME_MASK,
				       raw_vol, &changed);
	if (ret)
		return ret;

	return changed;
}

static void cs35l56_hda_create_controls(struct cs35l56_hda *cs35l56)
{
	struct snd_kcontrol_new ctl_template = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = cs35l56_hda_posture_info,
		.get = cs35l56_hda_posture_get,
		.put = cs35l56_hda_posture_put,
	};
	char name[64];
	int i;

	snprintf(name, sizeof(name), "%s Posture Number", cs35l56->amp_name);
	ctl_template.name = name;
	cs35l56->posture_ctl = snd_ctl_new1(&ctl_template, cs35l56);
	if (snd_ctl_add(cs35l56->codec->card, cs35l56->posture_ctl))
		dev_err(cs35l56->base.dev, "Failed to add KControl: %s\n", ctl_template.name);

	/* Mixer controls */
	ctl_template.info = cs35l56_hda_mixer_info;
	ctl_template.get = cs35l56_hda_mixer_get;
	ctl_template.put = cs35l56_hda_mixer_put;

	BUILD_BUG_ON(ARRAY_SIZE(cs35l56->mixer_ctl) != ARRAY_SIZE(cs35l56_hda_mixer_controls));

	for (i = 0; i < ARRAY_SIZE(cs35l56_hda_mixer_controls); ++i) {
		snprintf(name, sizeof(name), "%s %s", cs35l56->amp_name,
			 cs35l56_hda_mixer_controls[i].name);
		ctl_template.private_value = cs35l56_hda_mixer_controls[i].reg;
		cs35l56->mixer_ctl[i] = snd_ctl_new1(&ctl_template, cs35l56);
		if (snd_ctl_add(cs35l56->codec->card, cs35l56->mixer_ctl[i])) {
			dev_err(cs35l56->base.dev, "Failed to add KControl: %s\n",
				ctl_template.name);
		}
	}

	ctl_template.info = cs35l56_hda_vol_info;
	ctl_template.get = cs35l56_hda_vol_get;
	ctl_template.put = cs35l56_hda_vol_put;
	ctl_template.access = (SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ);
	ctl_template.tlv.p = cs35l56_hda_vol_tlv;
	snprintf(name, sizeof(name), "%s Speaker Playback Volume", cs35l56->amp_name);
	ctl_template.name = name;
	cs35l56->volume_ctl = snd_ctl_new1(&ctl_template, cs35l56);
	if (snd_ctl_add(cs35l56->codec->card, cs35l56->volume_ctl))
		dev_err(cs35l56->base.dev, "Failed to add KControl: %s\n", ctl_template.name);
}

static void cs35l56_hda_remove_controls(struct cs35l56_hda *cs35l56)
{
	int i;

	for (i = ARRAY_SIZE(cs35l56->mixer_ctl) - 1; i >= 0; i--)
		snd_ctl_remove(cs35l56->codec->card, cs35l56->mixer_ctl[i]);

	snd_ctl_remove(cs35l56->codec->card, cs35l56->posture_ctl);
	snd_ctl_remove(cs35l56->codec->card, cs35l56->volume_ctl);
}

static const struct cs_dsp_client_ops cs35l56_hda_client_ops = {
	.control_remove = hda_cs_dsp_control_remove,
};

static int cs35l56_hda_request_firmware_file(struct cs35l56_hda *cs35l56,
					     const struct firmware **firmware, char **filename,
					     const char *dir, const char *system_name,
					     const char *amp_name,
					     const char *filetype)
{
	char *s, c;
	int ret = 0;

	if (system_name && amp_name)
		*filename = kasprintf(GFP_KERNEL, "%scs35l56%s-%02x-dsp1-misc-%s-%s.%s", dir,
				      cs35l56->base.secured ? "s" : "", cs35l56->base.rev,
				      system_name, amp_name, filetype);
	else if (system_name)
		*filename = kasprintf(GFP_KERNEL, "%scs35l56%s-%02x-dsp1-misc-%s.%s", dir,
				      cs35l56->base.secured ? "s" : "", cs35l56->base.rev,
				      system_name, filetype);
	else
		*filename = kasprintf(GFP_KERNEL, "%scs35l56%s-%02x-dsp1-misc.%s", dir,
				      cs35l56->base.secured ? "s" : "", cs35l56->base.rev,
				      filetype);

	if (!*filename)
		return -ENOMEM;

	/*
	 * Make sure that filename is lower-case and any non alpha-numeric
	 * characters except full stop and forward slash are replaced with
	 * hyphens.
	 */
	s = *filename;
	while (*s) {
		c = *s;
		if (isalnum(c))
			*s = tolower(c);
		else if (c != '.' && c != '/')
			*s = '-';
		s++;
	}

	ret = firmware_request_nowarn(firmware, *filename, cs35l56->base.dev);
	if (ret) {
		dev_dbg(cs35l56->base.dev, "Failed to request '%s'\n", *filename);
		kfree(*filename);
		*filename = NULL;
		return ret;
	}

	dev_dbg(cs35l56->base.dev, "Found '%s'\n", *filename);

	return 0;
}

static const char cirrus_dir[] = "cirrus/";
static void cs35l56_hda_request_firmware_files(struct cs35l56_hda *cs35l56,
					       const struct firmware **wmfw_firmware,
					       char **wmfw_filename,
					       const struct firmware **coeff_firmware,
					       char **coeff_filename)
{
	const char *system_name = cs35l56->system_name;
	const char *amp_name = cs35l56->amp_name;
	int ret;

	if (system_name && amp_name) {
		if (!cs35l56_hda_request_firmware_file(cs35l56, wmfw_firmware, wmfw_filename,
						       cirrus_dir, system_name, amp_name, "wmfw")) {
			cs35l56_hda_request_firmware_file(cs35l56, coeff_firmware, coeff_filename,
							  cirrus_dir, system_name, amp_name, "bin");
			return;
		}
	}

	if (system_name) {
		if (!cs35l56_hda_request_firmware_file(cs35l56, wmfw_firmware, wmfw_filename,
						       cirrus_dir, system_name, NULL, "wmfw")) {
			if (amp_name)
				cs35l56_hda_request_firmware_file(cs35l56,
								  coeff_firmware, coeff_filename,
								  cirrus_dir, system_name,
								  amp_name, "bin");
			if (!*coeff_firmware)
				cs35l56_hda_request_firmware_file(cs35l56,
								  coeff_firmware, coeff_filename,
								  cirrus_dir, system_name,
								  NULL, "bin");
			return;
		}
	}

	ret = cs35l56_hda_request_firmware_file(cs35l56, wmfw_firmware, wmfw_filename,
						cirrus_dir, NULL, NULL, "wmfw");
	if (!ret) {
		cs35l56_hda_request_firmware_file(cs35l56, coeff_firmware, coeff_filename,
						  cirrus_dir, NULL, NULL, "bin");
		return;
	}

	/* When a firmware file is not found must still search for the coeff files */
	if (system_name) {
		if (amp_name)
			cs35l56_hda_request_firmware_file(cs35l56, coeff_firmware, coeff_filename,
							  cirrus_dir, system_name, amp_name, "bin");
		if (!*coeff_firmware)
			cs35l56_hda_request_firmware_file(cs35l56, coeff_firmware, coeff_filename,
							  cirrus_dir, system_name, NULL, "bin");
	}

	if (!*coeff_firmware)
		cs35l56_hda_request_firmware_file(cs35l56, coeff_firmware, coeff_filename,
						  cirrus_dir, NULL, NULL, "bin");
}

static void cs35l56_hda_release_firmware_files(const struct firmware *wmfw_firmware,
					       char *wmfw_filename,
					       const struct firmware *coeff_firmware,
					       char *coeff_filename)
{
	if (wmfw_firmware)
		release_firmware(wmfw_firmware);
	kfree(wmfw_filename);

	if (coeff_firmware)
		release_firmware(coeff_firmware);
	kfree(coeff_filename);
}

static void cs35l56_hda_add_dsp_controls(struct cs35l56_hda *cs35l56)
{
	struct hda_cs_dsp_ctl_info info;

	info.device_name = cs35l56->amp_name;
	info.fw_type = HDA_CS_DSP_FW_MISC;
	info.card = cs35l56->codec->card;

	hda_cs_dsp_add_controls(&cs35l56->cs_dsp, &info);
}

static int cs35l56_hda_fw_load(struct cs35l56_hda *cs35l56)
{
	const struct firmware *coeff_firmware = NULL;
	const struct firmware *wmfw_firmware = NULL;
	char *coeff_filename = NULL;
	char *wmfw_filename = NULL;
	unsigned int firmware_missing;
	int ret = 0;

	/* Prepare for a new DSP power-up */
	if (cs35l56->base.fw_patched)
		cs_dsp_power_down(&cs35l56->cs_dsp);

	cs35l56->base.fw_patched = false;

	pm_runtime_get_sync(cs35l56->base.dev);

	ret = regmap_read(cs35l56->base.regmap, CS35L56_PROTECTION_STATUS, &firmware_missing);
	if (ret) {
		dev_err(cs35l56->base.dev, "Failed to read PROTECTION_STATUS: %d\n", ret);
		goto err_pm_put;
	}

	firmware_missing &= CS35L56_FIRMWARE_MISSING;

	/*
	 * Firmware can only be downloaded if the CS35L56 is secured or is
	 * running from the built-in ROM. If it is secured the BIOS will have
	 * downloaded firmware, and the wmfw/bin files will only contain
	 * tunings that are safe to download with the firmware running.
	 */
	if (cs35l56->base.secured || firmware_missing) {
		cs35l56_hda_request_firmware_files(cs35l56, &wmfw_firmware, &wmfw_filename,
						   &coeff_firmware, &coeff_filename);
	}

	/*
	 * If the BIOS didn't patch the firmware a bin file is mandatory to
	 * enable the ASP·
	 */
	if (!coeff_firmware && firmware_missing) {
		dev_err(cs35l56->base.dev, ".bin file required but not found\n");
		ret = -ENOENT;
		goto err_fw_release;
	}

	mutex_lock(&cs35l56->base.irq_lock);

	/*
	 * When the device is running in secure mode the firmware files can
	 * only contain insecure tunings and therefore we do not need to
	 * shutdown the firmware to apply them and can use the lower cost
	 * reinit sequence instead.
	 */
	if (!cs35l56->base.secured && (wmfw_firmware || coeff_firmware)) {
		ret = cs35l56_firmware_shutdown(&cs35l56->base);
		if (ret)
			goto err;
	}

	ret = cs_dsp_power_up(&cs35l56->cs_dsp, wmfw_firmware, wmfw_filename,
			      coeff_firmware, coeff_filename, "misc");
	if (ret) {
		dev_dbg(cs35l56->base.dev, "%s: cs_dsp_power_up ret %d\n", __func__, ret);
		goto err;
	}

	if (wmfw_filename)
		dev_dbg(cs35l56->base.dev, "Loaded WMFW Firmware: %s\n", wmfw_filename);

	if (coeff_filename)
		dev_dbg(cs35l56->base.dev, "Loaded Coefficients: %s\n", coeff_filename);

	if (cs35l56->base.secured) {
		ret = cs35l56_mbox_send(&cs35l56->base, CS35L56_MBOX_CMD_AUDIO_REINIT);
		if (ret)
			goto err_powered_up;
	} else if (wmfw_firmware || coeff_firmware) {
		/* If we downloaded firmware, reset the device and wait for it to boot */
		cs35l56_system_reset(&cs35l56->base, false);
		regcache_mark_dirty(cs35l56->base.regmap);
		ret = cs35l56_wait_for_firmware_boot(&cs35l56->base);
		if (ret)
			goto err_powered_up;
	}

	/* Disable auto-hibernate so that runtime_pm has control */
	ret = cs35l56_mbox_send(&cs35l56->base, CS35L56_MBOX_CMD_PREVENT_AUTO_HIBERNATE);
	if (ret)
		goto err_powered_up;

	regcache_sync(cs35l56->base.regmap);

	regmap_clear_bits(cs35l56->base.regmap, CS35L56_PROTECTION_STATUS,
			  CS35L56_FIRMWARE_MISSING);
	cs35l56->base.fw_patched = true;

	ret = cs_dsp_run(&cs35l56->cs_dsp);
	if (ret)
		dev_dbg(cs35l56->base.dev, "%s: cs_dsp_run ret %d\n", __func__, ret);

err_powered_up:
	if (!cs35l56->base.fw_patched)
		cs_dsp_power_down(&cs35l56->cs_dsp);
err:
	mutex_unlock(&cs35l56->base.irq_lock);
err_fw_release:
	cs35l56_hda_release_firmware_files(wmfw_firmware, wmfw_filename,
					   coeff_firmware, coeff_filename);
err_pm_put:
	pm_runtime_put(cs35l56->base.dev);

	return ret;
}

static int cs35l56_hda_bind(struct device *dev, struct device *master, void *master_data)
{
	struct cs35l56_hda *cs35l56 = dev_get_drvdata(dev);
	struct hda_component *comps = master_data;
	int ret;

	if (!comps || cs35l56->index < 0 || cs35l56->index >= HDA_MAX_COMPONENTS)
		return -EINVAL;

	comps = &comps[cs35l56->index];
	if (comps->dev)
		return -EBUSY;

	comps->dev = dev;
	cs35l56->codec = comps->codec;
	strscpy(comps->name, dev_name(dev), sizeof(comps->name));
	comps->playback_hook = cs35l56_hda_playback_hook;

	ret = cs35l56_hda_fw_load(cs35l56);
	if (ret)
		return ret;

	cs35l56_hda_create_controls(cs35l56);
	cs35l56_hda_add_dsp_controls(cs35l56);

#if IS_ENABLED(CONFIG_SND_DEBUG)
	cs35l56->debugfs_root = debugfs_create_dir(dev_name(cs35l56->base.dev), sound_debugfs_root);
	cs_dsp_init_debugfs(&cs35l56->cs_dsp, cs35l56->debugfs_root);
#endif

	dev_dbg(cs35l56->base.dev, "Bound\n");

	return 0;
}

static void cs35l56_hda_unbind(struct device *dev, struct device *master, void *master_data)
{
	struct cs35l56_hda *cs35l56 = dev_get_drvdata(dev);
	struct hda_component *comps = master_data;

	cs35l56_hda_remove_controls(cs35l56);

#if IS_ENABLED(CONFIG_SND_DEBUG)
	cs_dsp_cleanup_debugfs(&cs35l56->cs_dsp);
	debugfs_remove_recursive(cs35l56->debugfs_root);
#endif

	if (cs35l56->base.fw_patched)
		cs_dsp_power_down(&cs35l56->cs_dsp);

	cs_dsp_remove(&cs35l56->cs_dsp);

	if (comps[cs35l56->index].dev == dev)
		memset(&comps[cs35l56->index], 0, sizeof(*comps));

	dev_dbg(cs35l56->base.dev, "Unbound\n");
}

static const struct component_ops cs35l56_hda_comp_ops = {
	.bind = cs35l56_hda_bind,
	.unbind = cs35l56_hda_unbind,
};

static int cs35l56_hda_system_suspend(struct device *dev)
{
	struct cs35l56_hda *cs35l56 = dev_get_drvdata(dev);

	if (cs35l56->playing)
		cs35l56_hda_pause(cs35l56);

	cs35l56->suspended = true;

	/*
	 * The interrupt line is normally shared, but after we start suspending
	 * we can't check if our device is the source of an interrupt, and can't
	 * clear it. Prevent this race by temporarily disabling the parent irq
	 * until we reach _no_irq.
	 */
	if (cs35l56->base.irq)
		disable_irq(cs35l56->base.irq);

	return pm_runtime_force_suspend(dev);
}

static int cs35l56_hda_system_suspend_late(struct device *dev)
{
	struct cs35l56_hda *cs35l56 = dev_get_drvdata(dev);

	/*
	 * RESET is usually shared by all amps so it must not be asserted until
	 * all driver instances have done their suspend() stage.
	 */
	if (cs35l56->base.reset_gpio) {
		gpiod_set_value_cansleep(cs35l56->base.reset_gpio, 0);
		cs35l56_wait_min_reset_pulse();
	}

	return 0;
}

static int cs35l56_hda_system_suspend_no_irq(struct device *dev)
{
	struct cs35l56_hda *cs35l56 = dev_get_drvdata(dev);

	/* Handlers are now disabled so the parent IRQ can safely be re-enabled. */
	if (cs35l56->base.irq)
		enable_irq(cs35l56->base.irq);

	return 0;
}

static int cs35l56_hda_system_resume_no_irq(struct device *dev)
{
	struct cs35l56_hda *cs35l56 = dev_get_drvdata(dev);

	/*
	 * WAKE interrupts unmask if the CS35L56 hibernates, which can cause
	 * spurious interrupts, and the interrupt line is normally shared.
	 * We can't check if our device is the source of an interrupt, and can't
	 * clear it, until it has fully resumed. Prevent this race by temporarily
	 * disabling the parent irq until we complete resume().
	 */
	if (cs35l56->base.irq)
		disable_irq(cs35l56->base.irq);

	return 0;
}

static int cs35l56_hda_system_resume_early(struct device *dev)
{
	struct cs35l56_hda *cs35l56 = dev_get_drvdata(dev);

	/* Ensure a spec-compliant RESET pulse. */
	if (cs35l56->base.reset_gpio) {
		gpiod_set_value_cansleep(cs35l56->base.reset_gpio, 0);
		cs35l56_wait_min_reset_pulse();

		/* Release shared RESET before drivers start resume(). */
		gpiod_set_value_cansleep(cs35l56->base.reset_gpio, 1);
		cs35l56_wait_control_port_ready();
	}

	return 0;
}

static int cs35l56_hda_system_resume(struct device *dev)
{
	struct cs35l56_hda *cs35l56 = dev_get_drvdata(dev);
	int ret;

	/* Undo pm_runtime_force_suspend() before re-enabling the irq */
	ret = pm_runtime_force_resume(dev);
	if (cs35l56->base.irq)
		enable_irq(cs35l56->base.irq);

	if (ret)
		return ret;

	cs35l56->suspended = false;

	ret = cs35l56_is_fw_reload_needed(&cs35l56->base);
	dev_dbg(cs35l56->base.dev, "fw_reload_needed: %d\n", ret);
	if (ret > 0) {
		ret = cs35l56_hda_fw_load(cs35l56);
		if (ret)
			return ret;
	}

	if (cs35l56->playing)
		cs35l56_hda_play(cs35l56);

	return 0;
}

static int cs35l56_hda_read_acpi(struct cs35l56_hda *cs35l56, int id)
{
	u32 values[HDA_MAX_COMPONENTS];
	struct acpi_device *adev;
	const char *property, *sub;
	size_t nval;
	int i, ret;

	/*
	 * ACPI_COMPANION isn't available when this driver was instantiated by
	 * the serial-multi-instantiate driver, so lookup the node by HID
	 */
	if (!ACPI_COMPANION(cs35l56->base.dev)) {
		adev = acpi_dev_get_first_match_dev("CSC3556", NULL, -1);
		if (!adev) {
			dev_err(cs35l56->base.dev, "Failed to find an ACPI device for %s\n",
				dev_name(cs35l56->base.dev));
			return -ENODEV;
		}
		ACPI_COMPANION_SET(cs35l56->base.dev, adev);
	}

	property = "cirrus,dev-index";
	ret = device_property_count_u32(cs35l56->base.dev, property);
	if (ret <= 0)
		goto err;

	if (ret > ARRAY_SIZE(values)) {
		ret = -EINVAL;
		goto err;
	}
	nval = ret;

	ret = device_property_read_u32_array(cs35l56->base.dev, property, values, nval);
	if (ret)
		goto err;

	cs35l56->index = -1;
	for (i = 0; i < nval; i++) {
		if (values[i] == id) {
			cs35l56->index = i;
			break;
		}
	}
	/*
	 * It's not an error for the ID to be missing: for I2C there can be
	 * an alias address that is not a real device. So reject silently.
	 */
	if (cs35l56->index == -1) {
		dev_dbg(cs35l56->base.dev, "No index found in %s\n", property);
		ret = -ENODEV;
		goto err;
	}

	sub = acpi_get_subsystem_id(ACPI_HANDLE(cs35l56->base.dev));

	if (IS_ERR(sub)) {
		dev_info(cs35l56->base.dev,
			 "Read ACPI _SUB failed(%ld): fallback to generic firmware\n",
			 PTR_ERR(sub));
	} else {
		cs35l56->system_name = sub;
	}

	cs35l56->base.reset_gpio = devm_gpiod_get_index_optional(cs35l56->base.dev,
								 "reset",
								 cs35l56->index,
								 GPIOD_OUT_LOW);
	if (IS_ERR(cs35l56->base.reset_gpio)) {
		ret = PTR_ERR(cs35l56->base.reset_gpio);

		/*
		 * If RESET is shared the first amp to probe will grab the reset
		 * line and reset all the amps
		 */
		if (ret != -EBUSY)
			return dev_err_probe(cs35l56->base.dev, ret, "Failed to get reset GPIO\n");

		dev_info(cs35l56->base.dev, "Reset GPIO busy, assume shared reset\n");
		cs35l56->base.reset_gpio = NULL;
	}

	return 0;

err:
	if (ret != -ENODEV)
		dev_err(cs35l56->base.dev, "Failed property %s: %d\n", property, ret);

	return ret;
}

int cs35l56_hda_common_probe(struct cs35l56_hda *cs35l56, int id)
{
	int ret;

	mutex_init(&cs35l56->base.irq_lock);
	dev_set_drvdata(cs35l56->base.dev, cs35l56);

	ret = cs35l56_hda_read_acpi(cs35l56, id);
	if (ret)
		goto err;

	cs35l56->amp_name = devm_kasprintf(cs35l56->base.dev, GFP_KERNEL, "AMP%d",
					   cs35l56->index + 1);
	if (!cs35l56->amp_name) {
		ret = -ENOMEM;
		goto err;
	}

	cs35l56_init_cs_dsp(&cs35l56->base, &cs35l56->cs_dsp);
	cs35l56->cs_dsp.client_ops = &cs35l56_hda_client_ops;

	if (cs35l56->base.reset_gpio) {
		dev_dbg(cs35l56->base.dev, "Hard reset\n");

		/*
		 * The GPIOD_OUT_LOW to *_gpiod_get_*() will be ignored if the
		 * ACPI defines a different default state. So explicitly set low.
		 */
		gpiod_set_value_cansleep(cs35l56->base.reset_gpio, 0);
		cs35l56_wait_min_reset_pulse();
		gpiod_set_value_cansleep(cs35l56->base.reset_gpio, 1);
	}

	ret = cs35l56_hw_init(&cs35l56->base);
	if (ret < 0)
		goto err;

	/* Reset the device and wait for it to boot */
	cs35l56_system_reset(&cs35l56->base, false);
	ret = cs35l56_wait_for_firmware_boot(&cs35l56->base);
	if (ret)
		goto err;

	ret = cs35l56_set_patch(&cs35l56->base);
	if (ret)
		goto err;

	regcache_mark_dirty(cs35l56->base.regmap);
	regcache_sync(cs35l56->base.regmap);

	/* Disable auto-hibernate so that runtime_pm has control */
	ret = cs35l56_mbox_send(&cs35l56->base, CS35L56_MBOX_CMD_PREVENT_AUTO_HIBERNATE);
	if (ret)
		goto err;

	ret = cs_dsp_halo_init(&cs35l56->cs_dsp);
	if (ret) {
		dev_err_probe(cs35l56->base.dev, ret, "cs_dsp_halo_init failed\n");
		goto err;
	}

	dev_dbg(cs35l56->base.dev, "DSP system name: '%s', amp name: '%s'\n",
		cs35l56->system_name, cs35l56->amp_name);

	regmap_multi_reg_write(cs35l56->base.regmap, cs35l56_hda_dai_config,
			       ARRAY_SIZE(cs35l56_hda_dai_config));

	/*
	 * By default only enable one ASP1TXn, where n=amplifier index,
	 * This prevents multiple amps trying to drive the same slot.
	 */
	cs35l56->asp_tx_mask = BIT(cs35l56->index);

	pm_runtime_set_autosuspend_delay(cs35l56->base.dev, 3000);
	pm_runtime_use_autosuspend(cs35l56->base.dev);
	pm_runtime_set_active(cs35l56->base.dev);
	pm_runtime_mark_last_busy(cs35l56->base.dev);
	pm_runtime_enable(cs35l56->base.dev);

	ret = component_add(cs35l56->base.dev, &cs35l56_hda_comp_ops);
	if (ret) {
		dev_err(cs35l56->base.dev, "Register component failed: %d\n", ret);
		goto pm_err;
	}

	cs35l56->base.init_done = true;

	return 0;

pm_err:
	pm_runtime_disable(cs35l56->base.dev);
err:
	gpiod_set_value_cansleep(cs35l56->base.reset_gpio, 0);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(cs35l56_hda_common_probe, SND_HDA_SCODEC_CS35L56);

void cs35l56_hda_remove(struct device *dev)
{
	struct cs35l56_hda *cs35l56 = dev_get_drvdata(dev);

	pm_runtime_dont_use_autosuspend(cs35l56->base.dev);
	pm_runtime_get_sync(cs35l56->base.dev);
	pm_runtime_disable(cs35l56->base.dev);

	component_del(cs35l56->base.dev, &cs35l56_hda_comp_ops);

	kfree(cs35l56->system_name);
	pm_runtime_put_noidle(cs35l56->base.dev);

	gpiod_set_value_cansleep(cs35l56->base.reset_gpio, 0);
}
EXPORT_SYMBOL_NS_GPL(cs35l56_hda_remove, SND_HDA_SCODEC_CS35L56);

const struct dev_pm_ops cs35l56_hda_pm_ops = {
	RUNTIME_PM_OPS(cs35l56_hda_runtime_suspend, cs35l56_hda_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(cs35l56_hda_system_suspend, cs35l56_hda_system_resume)
	LATE_SYSTEM_SLEEP_PM_OPS(cs35l56_hda_system_suspend_late,
				 cs35l56_hda_system_resume_early)
	NOIRQ_SYSTEM_SLEEP_PM_OPS(cs35l56_hda_system_suspend_no_irq,
				  cs35l56_hda_system_resume_no_irq)
};
EXPORT_SYMBOL_NS_GPL(cs35l56_hda_pm_ops, SND_HDA_SCODEC_CS35L56);

MODULE_DESCRIPTION("CS35L56 HDA Driver");
MODULE_IMPORT_NS(SND_HDA_CS_DSP_CONTROLS);
MODULE_IMPORT_NS(SND_SOC_CS35L56_SHARED);
MODULE_AUTHOR("Richard Fitzgerald <rf@opensource.cirrus.com>");
MODULE_AUTHOR("Simon Trimmer <simont@opensource.cirrus.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(FW_CS_DSP);
