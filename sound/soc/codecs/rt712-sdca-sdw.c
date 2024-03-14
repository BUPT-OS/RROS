// SPDX-License-Identifier: GPL-2.0-only
//
// rt712-sdca-sdw.c -- rt712 SDCA ALSA SoC audio driver
//
// Copyright(c) 2023 Realtek Semiconductor Corp.
//
//

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/soundwire/sdw_registers.h>
#include "rt712-sdca.h"
#include "rt712-sdca-sdw.h"

static bool rt712_sdca_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x201a ... 0x201f:
	case 0x2029 ... 0x202a:
	case 0x202d ... 0x2034:
	case 0x2230 ... 0x2232:
	case 0x2f01 ... 0x2f0a:
	case 0x2f35 ... 0x2f36:
	case 0x2f50:
	case 0x2f54:
	case 0x2f58 ... 0x2f5d:
	case 0x3201:
	case 0x320c:
	case 0x3301 ... 0x3303:
	case SDW_SDCA_CTL(FUNC_NUM_JACK_CODEC, RT712_SDCA_ENT_GE49, RT712_SDCA_CTL_SELECTED_MODE, 0):
	case SDW_SDCA_CTL(FUNC_NUM_JACK_CODEC, RT712_SDCA_ENT_GE49, RT712_SDCA_CTL_DETECTED_MODE, 0):
	case SDW_SDCA_CTL(FUNC_NUM_HID, RT712_SDCA_ENT_HID01, RT712_SDCA_CTL_HIDTX_CURRENT_OWNER, 0) ...
		SDW_SDCA_CTL(FUNC_NUM_HID, RT712_SDCA_ENT_HID01, RT712_SDCA_CTL_HIDTX_MESSAGE_LENGTH, 0):
	case RT712_BUF_ADDR_HID1 ... RT712_BUF_ADDR_HID2:
		return true;
	default:
		return false;
	}
}

static bool rt712_sdca_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x201b:
	case 0x201c:
	case 0x201d:
	case 0x201f:
	case 0x202d ... 0x202f:
	case 0x2230:
	case 0x2f01:
	case 0x2f35:
	case 0x320c:
	case SDW_SDCA_CTL(FUNC_NUM_JACK_CODEC, RT712_SDCA_ENT_GE49, RT712_SDCA_CTL_DETECTED_MODE, 0):
	case SDW_SDCA_CTL(FUNC_NUM_HID, RT712_SDCA_ENT_HID01, RT712_SDCA_CTL_HIDTX_CURRENT_OWNER, 0) ...
		SDW_SDCA_CTL(FUNC_NUM_HID, RT712_SDCA_ENT_HID01, RT712_SDCA_CTL_HIDTX_MESSAGE_LENGTH, 0):
	case RT712_BUF_ADDR_HID1 ... RT712_BUF_ADDR_HID2:
		return true;
	default:
		return false;
	}
}

static bool rt712_sdca_mbq_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x2000000 ... 0x200008e:
	case 0x5300000 ... 0x530000e:
	case 0x5400000 ... 0x540000e:
	case 0x5600000 ... 0x5600008:
	case 0x5700000 ... 0x570000d:
	case 0x5800000 ... 0x5800021:
	case 0x5900000 ... 0x5900028:
	case 0x5a00000 ... 0x5a00009:
	case 0x5b00000 ... 0x5b00051:
	case 0x5c00000 ... 0x5c0009a:
	case 0x5d00000 ... 0x5d00009:
	case 0x5f00000 ... 0x5f00030:
	case 0x6100000 ... 0x6100068:
	case SDW_SDCA_CTL(FUNC_NUM_JACK_CODEC, RT712_SDCA_ENT_USER_FU05, RT712_SDCA_CTL_FU_VOLUME, CH_L):
	case SDW_SDCA_CTL(FUNC_NUM_JACK_CODEC, RT712_SDCA_ENT_USER_FU05, RT712_SDCA_CTL_FU_VOLUME, CH_R):
	case SDW_SDCA_CTL(FUNC_NUM_JACK_CODEC, RT712_SDCA_ENT_USER_FU0F, RT712_SDCA_CTL_FU_VOLUME, CH_L):
	case SDW_SDCA_CTL(FUNC_NUM_JACK_CODEC, RT712_SDCA_ENT_USER_FU0F, RT712_SDCA_CTL_FU_VOLUME, CH_R):
	case SDW_SDCA_CTL(FUNC_NUM_AMP, RT712_SDCA_ENT_USER_FU06, RT712_SDCA_CTL_FU_VOLUME, CH_L):
	case SDW_SDCA_CTL(FUNC_NUM_AMP, RT712_SDCA_ENT_USER_FU06, RT712_SDCA_CTL_FU_VOLUME, CH_R):
		return true;
	default:
		return false;
	}
}

static bool rt712_sdca_mbq_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x2000000:
	case 0x200001a:
	case 0x2000024:
	case 0x2000046:
	case 0x200008a:
	case 0x5800000:
	case 0x5800001:
	case 0x6100008:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config rt712_sdca_regmap = {
	.reg_bits = 32,
	.val_bits = 8,
	.readable_reg = rt712_sdca_readable_register,
	.volatile_reg = rt712_sdca_volatile_register,
	.max_register = 0x44ffffff,
	.reg_defaults = rt712_sdca_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(rt712_sdca_reg_defaults),
	.cache_type = REGCACHE_MAPLE,
	.use_single_read = true,
	.use_single_write = true,
};

static const struct regmap_config rt712_sdca_mbq_regmap = {
	.name = "sdw-mbq",
	.reg_bits = 32,
	.val_bits = 16,
	.readable_reg = rt712_sdca_mbq_readable_register,
	.volatile_reg = rt712_sdca_mbq_volatile_register,
	.max_register = 0x41000312,
	.reg_defaults = rt712_sdca_mbq_defaults,
	.num_reg_defaults = ARRAY_SIZE(rt712_sdca_mbq_defaults),
	.cache_type = REGCACHE_MAPLE,
	.use_single_read = true,
	.use_single_write = true,
};

static int rt712_sdca_update_status(struct sdw_slave *slave,
				enum sdw_slave_status status)
{
	struct rt712_sdca_priv *rt712 = dev_get_drvdata(&slave->dev);

	if (status == SDW_SLAVE_UNATTACHED)
		rt712->hw_init = false;

	if (status == SDW_SLAVE_ATTACHED) {
		if (rt712->hs_jack) {
			/*
			 * Due to the SCP_SDCA_INTMASK will be cleared by any reset, and then
			 * if the device attached again, we will need to set the setting back.
			 * It could avoid losing the jack detection interrupt.
			 * This also could sync with the cache value as the rt712_sdca_jack_init set.
			 */
			sdw_write_no_pm(rt712->slave, SDW_SCP_SDCA_INTMASK1,
				SDW_SCP_SDCA_INTMASK_SDCA_0);
			sdw_write_no_pm(rt712->slave, SDW_SCP_SDCA_INTMASK2,
				SDW_SCP_SDCA_INTMASK_SDCA_8);
		}
	}

	/*
	 * Perform initialization only if slave status is present and
	 * hw_init flag is false
	 */
	if (rt712->hw_init || status != SDW_SLAVE_ATTACHED)
		return 0;

	/* perform I/O transfers required for Slave initialization */
	return rt712_sdca_io_init(&slave->dev, slave);
}

static int rt712_sdca_read_prop(struct sdw_slave *slave)
{
	struct sdw_slave_prop *prop = &slave->prop;
	int nval;
	int i, j;
	u32 bit;
	unsigned long addr;
	struct sdw_dpn_prop *dpn;

	prop->scp_int1_mask = SDW_SCP_INT1_BUS_CLASH | SDW_SCP_INT1_PARITY;
	prop->quirks = SDW_SLAVE_QUIRKS_INVALID_INITIAL_PARITY;

	prop->paging_support = true;

	/* first we need to allocate memory for set bits in port lists */
	prop->source_ports = BIT(4); /* BITMAP: 00010000 */
	prop->sink_ports = BIT(3) | BIT(1); /* BITMAP:  00001010 */

	nval = hweight32(prop->source_ports);
	prop->src_dpn_prop = devm_kcalloc(&slave->dev, nval,
		sizeof(*prop->src_dpn_prop), GFP_KERNEL);
	if (!prop->src_dpn_prop)
		return -ENOMEM;

	i = 0;
	dpn = prop->src_dpn_prop;
	addr = prop->source_ports;
	for_each_set_bit(bit, &addr, 32) {
		dpn[i].num = bit;
		dpn[i].type = SDW_DPN_FULL;
		dpn[i].simple_ch_prep_sm = true;
		dpn[i].ch_prep_timeout = 10;
		i++;
	}

	/* do this again for sink now */
	nval = hweight32(prop->sink_ports);
	prop->sink_dpn_prop = devm_kcalloc(&slave->dev, nval,
		sizeof(*prop->sink_dpn_prop), GFP_KERNEL);
	if (!prop->sink_dpn_prop)
		return -ENOMEM;

	j = 0;
	dpn = prop->sink_dpn_prop;
	addr = prop->sink_ports;
	for_each_set_bit(bit, &addr, 32) {
		dpn[j].num = bit;
		dpn[j].type = SDW_DPN_FULL;
		dpn[j].simple_ch_prep_sm = true;
		dpn[j].ch_prep_timeout = 10;
		j++;
	}

	/* set the timeout values */
	prop->clk_stop_timeout = 1380;

	/* wake-up event */
	prop->wake_capable = 1;

	return 0;
}

static int rt712_sdca_interrupt_callback(struct sdw_slave *slave,
					struct sdw_slave_intr_status *status)
{
	struct rt712_sdca_priv *rt712 = dev_get_drvdata(&slave->dev);
	int ret, stat;
	int count = 0, retry = 3;
	unsigned int sdca_cascade, scp_sdca_stat1, scp_sdca_stat2 = 0;

	dev_dbg(&slave->dev,
		"%s control_port_stat=%x, sdca_cascade=%x", __func__,
		status->control_port, status->sdca_cascade);

	if (cancel_delayed_work_sync(&rt712->jack_detect_work)) {
		dev_warn(&slave->dev, "%s the pending delayed_work was cancelled", __func__);
		/* avoid the HID owner doesn't change to device */
		if (rt712->scp_sdca_stat2)
			scp_sdca_stat2 = rt712->scp_sdca_stat2;
	}

	/*
	 * The critical section below intentionally protects a rather large piece of code.
	 * We don't want to allow the system suspend to disable an interrupt while we are
	 * processing it, which could be problematic given the quirky SoundWire interrupt
	 * scheme. We do want however to prevent new workqueues from being scheduled if
	 * the disable_irq flag was set during system suspend.
	 */
	mutex_lock(&rt712->disable_irq_lock);

	ret = sdw_read_no_pm(rt712->slave, SDW_SCP_SDCA_INT1);
	if (ret < 0)
		goto io_error;
	rt712->scp_sdca_stat1 = ret;
	ret = sdw_read_no_pm(rt712->slave, SDW_SCP_SDCA_INT2);
	if (ret < 0)
		goto io_error;
	rt712->scp_sdca_stat2 = ret;
	if (scp_sdca_stat2)
		rt712->scp_sdca_stat2 |= scp_sdca_stat2;

	do {
		/* clear flag */
		ret = sdw_read_no_pm(rt712->slave, SDW_SCP_SDCA_INT1);
		if (ret < 0)
			goto io_error;
		if (ret & SDW_SCP_SDCA_INTMASK_SDCA_0) {
			ret = sdw_write_no_pm(rt712->slave, SDW_SCP_SDCA_INT1,
						SDW_SCP_SDCA_INTMASK_SDCA_0);
			if (ret < 0)
				goto io_error;
		}
		ret = sdw_read_no_pm(rt712->slave, SDW_SCP_SDCA_INT2);
		if (ret < 0)
			goto io_error;
		if (ret & SDW_SCP_SDCA_INTMASK_SDCA_8) {
			ret = sdw_write_no_pm(rt712->slave, SDW_SCP_SDCA_INT2,
						SDW_SCP_SDCA_INTMASK_SDCA_8);
			if (ret < 0)
				goto io_error;
		}

		/* check if flag clear or not */
		ret = sdw_read_no_pm(rt712->slave, SDW_DP0_INT);
		if (ret < 0)
			goto io_error;
		sdca_cascade = ret & SDW_DP0_SDCA_CASCADE;

		ret = sdw_read_no_pm(rt712->slave, SDW_SCP_SDCA_INT1);
		if (ret < 0)
			goto io_error;
		scp_sdca_stat1 = ret & SDW_SCP_SDCA_INTMASK_SDCA_0;

		ret = sdw_read_no_pm(rt712->slave, SDW_SCP_SDCA_INT2);
		if (ret < 0)
			goto io_error;
		scp_sdca_stat2 = ret & SDW_SCP_SDCA_INTMASK_SDCA_8;

		stat = scp_sdca_stat1 || scp_sdca_stat2 || sdca_cascade;

		count++;
	} while (stat != 0 && count < retry);

	if (stat)
		dev_warn(&slave->dev,
			"%s scp_sdca_stat1=0x%x, scp_sdca_stat2=0x%x\n", __func__,
			rt712->scp_sdca_stat1, rt712->scp_sdca_stat2);

	if (status->sdca_cascade && !rt712->disable_irq)
		mod_delayed_work(system_power_efficient_wq,
			&rt712->jack_detect_work, msecs_to_jiffies(30));

	mutex_unlock(&rt712->disable_irq_lock);

	return 0;

io_error:
	mutex_unlock(&rt712->disable_irq_lock);
	pr_err_ratelimited("IO error in %s, ret %d\n", __func__, ret);
	return ret;
}

static struct sdw_slave_ops rt712_sdca_slave_ops = {
	.read_prop = rt712_sdca_read_prop,
	.interrupt_callback = rt712_sdca_interrupt_callback,
	.update_status = rt712_sdca_update_status,
};

static int rt712_sdca_sdw_probe(struct sdw_slave *slave,
				const struct sdw_device_id *id)
{
	struct regmap *regmap, *mbq_regmap;

	/* Regmap Initialization */
	mbq_regmap = devm_regmap_init_sdw_mbq(slave, &rt712_sdca_mbq_regmap);
	if (IS_ERR(mbq_regmap))
		return PTR_ERR(mbq_regmap);

	regmap = devm_regmap_init_sdw(slave, &rt712_sdca_regmap);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return rt712_sdca_init(&slave->dev, regmap, mbq_regmap, slave);
}

static int rt712_sdca_sdw_remove(struct sdw_slave *slave)
{
	struct rt712_sdca_priv *rt712 = dev_get_drvdata(&slave->dev);

	if (rt712->hw_init) {
		cancel_delayed_work_sync(&rt712->jack_detect_work);
		cancel_delayed_work_sync(&rt712->jack_btn_check_work);
	}

	pm_runtime_disable(&slave->dev);

	mutex_destroy(&rt712->calibrate_mutex);
	mutex_destroy(&rt712->disable_irq_lock);

	return 0;
}

static const struct sdw_device_id rt712_sdca_id[] = {
	SDW_SLAVE_ENTRY_EXT(0x025d, 0x712, 0x3, 0x1, 0),
	SDW_SLAVE_ENTRY_EXT(0x025d, 0x713, 0x3, 0x1, 0),
	SDW_SLAVE_ENTRY_EXT(0x025d, 0x716, 0x3, 0x1, 0),
	SDW_SLAVE_ENTRY_EXT(0x025d, 0x717, 0x3, 0x1, 0),
	{},
};
MODULE_DEVICE_TABLE(sdw, rt712_sdca_id);

static int __maybe_unused rt712_sdca_dev_suspend(struct device *dev)
{
	struct rt712_sdca_priv *rt712 = dev_get_drvdata(dev);

	if (!rt712->hw_init)
		return 0;

	cancel_delayed_work_sync(&rt712->jack_detect_work);
	cancel_delayed_work_sync(&rt712->jack_btn_check_work);

	regcache_cache_only(rt712->regmap, true);
	regcache_cache_only(rt712->mbq_regmap, true);

	return 0;
}

static int __maybe_unused rt712_sdca_dev_system_suspend(struct device *dev)
{
	struct rt712_sdca_priv *rt712_sdca = dev_get_drvdata(dev);
	struct sdw_slave *slave = dev_to_sdw_dev(dev);
	int ret1, ret2;

	if (!rt712_sdca->hw_init)
		return 0;

	/*
	 * prevent new interrupts from being handled after the
	 * deferred work completes and before the parent disables
	 * interrupts on the link
	 */
	mutex_lock(&rt712_sdca->disable_irq_lock);
	rt712_sdca->disable_irq = true;
	ret1 = sdw_update_no_pm(slave, SDW_SCP_SDCA_INTMASK1,
				SDW_SCP_SDCA_INTMASK_SDCA_0, 0);
	ret2 = sdw_update_no_pm(slave, SDW_SCP_SDCA_INTMASK2,
				SDW_SCP_SDCA_INTMASK_SDCA_8, 0);
	mutex_unlock(&rt712_sdca->disable_irq_lock);

	if (ret1 < 0 || ret2 < 0) {
		/* log but don't prevent suspend from happening */
		dev_dbg(&slave->dev, "%s: could not disable SDCA interrupts\n:", __func__);
	}

	return rt712_sdca_dev_suspend(dev);
}

#define RT712_PROBE_TIMEOUT 5000

static int __maybe_unused rt712_sdca_dev_resume(struct device *dev)
{
	struct sdw_slave *slave = dev_to_sdw_dev(dev);
	struct rt712_sdca_priv *rt712 = dev_get_drvdata(dev);
	unsigned long time;

	if (!rt712->first_hw_init)
		return 0;

	if (!slave->unattach_request) {
		if (rt712->disable_irq == true) {
			mutex_lock(&rt712->disable_irq_lock);
			sdw_write_no_pm(slave, SDW_SCP_SDCA_INTMASK1, SDW_SCP_SDCA_INTMASK_SDCA_0);
			sdw_write_no_pm(slave, SDW_SCP_SDCA_INTMASK2, SDW_SCP_SDCA_INTMASK_SDCA_8);
			rt712->disable_irq = false;
			mutex_unlock(&rt712->disable_irq_lock);
		}
		goto regmap_sync;
	}

	time = wait_for_completion_timeout(&slave->initialization_complete,
				msecs_to_jiffies(RT712_PROBE_TIMEOUT));
	if (!time) {
		dev_err(&slave->dev, "Initialization not complete, timed out\n");
		sdw_show_ping_status(slave->bus, true);

		return -ETIMEDOUT;
	}

regmap_sync:
	slave->unattach_request = 0;
	regcache_cache_only(rt712->regmap, false);
	regcache_sync(rt712->regmap);
	regcache_cache_only(rt712->mbq_regmap, false);
	regcache_sync(rt712->mbq_regmap);
	return 0;
}

static const struct dev_pm_ops rt712_sdca_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(rt712_sdca_dev_system_suspend, rt712_sdca_dev_resume)
	SET_RUNTIME_PM_OPS(rt712_sdca_dev_suspend, rt712_sdca_dev_resume, NULL)
};

static struct sdw_driver rt712_sdca_sdw_driver = {
	.driver = {
		.name = "rt712-sdca",
		.owner = THIS_MODULE,
		.pm = &rt712_sdca_pm,
	},
	.probe = rt712_sdca_sdw_probe,
	.remove = rt712_sdca_sdw_remove,
	.ops = &rt712_sdca_slave_ops,
	.id_table = rt712_sdca_id,
};
module_sdw_driver(rt712_sdca_sdw_driver);

MODULE_DESCRIPTION("ASoC RT712 SDCA SDW driver");
MODULE_AUTHOR("Shuming Fan <shumingf@realtek.com>");
MODULE_LICENSE("GPL");
