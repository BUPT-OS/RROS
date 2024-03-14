// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include <linux/of_irq.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include <drm/drm_bridge_connector.h>
#include <drm/drm_of.h>

#include <sound/hdmi-codec.h>
#include "hdmi.h"

void msm_hdmi_set_mode(struct hdmi *hdmi, bool power_on)
{
	uint32_t ctrl = 0;
	unsigned long flags;

	spin_lock_irqsave(&hdmi->reg_lock, flags);
	if (power_on) {
		ctrl |= HDMI_CTRL_ENABLE;
		if (!hdmi->hdmi_mode) {
			ctrl |= HDMI_CTRL_HDMI;
			hdmi_write(hdmi, REG_HDMI_CTRL, ctrl);
			ctrl &= ~HDMI_CTRL_HDMI;
		} else {
			ctrl |= HDMI_CTRL_HDMI;
		}
	} else {
		ctrl = HDMI_CTRL_HDMI;
	}

	hdmi_write(hdmi, REG_HDMI_CTRL, ctrl);
	spin_unlock_irqrestore(&hdmi->reg_lock, flags);
	DBG("HDMI Core: %s, HDMI_CTRL=0x%08x",
			power_on ? "Enable" : "Disable", ctrl);
}

static irqreturn_t msm_hdmi_irq(int irq, void *dev_id)
{
	struct hdmi *hdmi = dev_id;

	/* Process HPD: */
	msm_hdmi_hpd_irq(hdmi->bridge);

	/* Process DDC: */
	msm_hdmi_i2c_irq(hdmi->i2c);

	/* Process HDCP: */
	if (hdmi->hdcp_ctrl)
		msm_hdmi_hdcp_irq(hdmi->hdcp_ctrl);

	/* TODO audio.. */

	return IRQ_HANDLED;
}

static void msm_hdmi_destroy(struct hdmi *hdmi)
{
	/*
	 * at this point, hpd has been disabled,
	 * after flush workq, it's safe to deinit hdcp
	 */
	if (hdmi->workq)
		destroy_workqueue(hdmi->workq);
	msm_hdmi_hdcp_destroy(hdmi);

	if (hdmi->i2c)
		msm_hdmi_i2c_destroy(hdmi->i2c);
}

static void msm_hdmi_put_phy(struct hdmi *hdmi)
{
	if (hdmi->phy_dev) {
		put_device(hdmi->phy_dev);
		hdmi->phy = NULL;
		hdmi->phy_dev = NULL;
	}
}

static int msm_hdmi_get_phy(struct hdmi *hdmi)
{
	struct platform_device *pdev = hdmi->pdev;
	struct platform_device *phy_pdev;
	struct device_node *phy_node;

	phy_node = of_parse_phandle(pdev->dev.of_node, "phys", 0);
	if (!phy_node) {
		DRM_DEV_ERROR(&pdev->dev, "cannot find phy device\n");
		return -ENXIO;
	}

	phy_pdev = of_find_device_by_node(phy_node);
	of_node_put(phy_node);

	if (!phy_pdev)
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER, "phy driver is not ready\n");

	hdmi->phy = platform_get_drvdata(phy_pdev);
	if (!hdmi->phy) {
		put_device(&phy_pdev->dev);
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER, "phy driver is not ready\n");
	}

	hdmi->phy_dev = &phy_pdev->dev;

	return 0;
}

/* construct hdmi at bind/probe time, grab all the resources.  If
 * we are to EPROBE_DEFER we want to do it here, rather than later
 * at modeset_init() time
 */
static int msm_hdmi_init(struct hdmi *hdmi)
{
	struct platform_device *pdev = hdmi->pdev;
	int ret;

	hdmi->workq = alloc_ordered_workqueue("msm_hdmi", 0);
	if (!hdmi->workq) {
		ret = -ENOMEM;
		goto fail;
	}

	hdmi->i2c = msm_hdmi_i2c_init(hdmi);
	if (IS_ERR(hdmi->i2c)) {
		ret = PTR_ERR(hdmi->i2c);
		DRM_DEV_ERROR(&pdev->dev, "failed to get i2c: %d\n", ret);
		hdmi->i2c = NULL;
		goto fail;
	}

	hdmi->hdcp_ctrl = msm_hdmi_hdcp_init(hdmi);
	if (IS_ERR(hdmi->hdcp_ctrl)) {
		dev_warn(&pdev->dev, "failed to init hdcp: disabled\n");
		hdmi->hdcp_ctrl = NULL;
	}

	return 0;

fail:
	msm_hdmi_destroy(hdmi);

	return ret;
}

/* Second part of initialization, the drm/kms level modeset_init,
 * constructs/initializes mode objects, etc, is called from master
 * driver (not hdmi sub-device's probe/bind!)
 *
 * Any resource (regulator/clk/etc) which could be missing at boot
 * should be handled in msm_hdmi_init() so that failure happens from
 * hdmi sub-device's probe.
 */
int msm_hdmi_modeset_init(struct hdmi *hdmi,
		struct drm_device *dev, struct drm_encoder *encoder)
{
	struct msm_drm_private *priv = dev->dev_private;
	int ret;

	if (priv->num_bridges == ARRAY_SIZE(priv->bridges)) {
		DRM_DEV_ERROR(dev->dev, "too many bridges\n");
		return -ENOSPC;
	}

	hdmi->dev = dev;
	hdmi->encoder = encoder;

	hdmi_audio_infoframe_init(&hdmi->audio.infoframe);

	hdmi->bridge = msm_hdmi_bridge_init(hdmi);
	if (IS_ERR(hdmi->bridge)) {
		ret = PTR_ERR(hdmi->bridge);
		DRM_DEV_ERROR(dev->dev, "failed to create HDMI bridge: %d\n", ret);
		hdmi->bridge = NULL;
		goto fail;
	}

	if (hdmi->next_bridge) {
		ret = drm_bridge_attach(hdmi->encoder, hdmi->next_bridge, hdmi->bridge,
					DRM_BRIDGE_ATTACH_NO_CONNECTOR);
		if (ret) {
			DRM_DEV_ERROR(dev->dev, "failed to attach next HDMI bridge: %d\n", ret);
			goto fail;
		}
	}

	hdmi->connector = drm_bridge_connector_init(hdmi->dev, encoder);
	if (IS_ERR(hdmi->connector)) {
		ret = PTR_ERR(hdmi->connector);
		DRM_DEV_ERROR(dev->dev, "failed to create HDMI connector: %d\n", ret);
		hdmi->connector = NULL;
		goto fail;
	}

	drm_connector_attach_encoder(hdmi->connector, hdmi->encoder);

	ret = devm_request_irq(dev->dev, hdmi->irq,
			msm_hdmi_irq, IRQF_TRIGGER_HIGH,
			"hdmi_isr", hdmi);
	if (ret < 0) {
		DRM_DEV_ERROR(dev->dev, "failed to request IRQ%u: %d\n",
				hdmi->irq, ret);
		goto fail;
	}

	ret = msm_hdmi_hpd_enable(hdmi->bridge);
	if (ret < 0) {
		DRM_DEV_ERROR(&hdmi->pdev->dev, "failed to enable HPD: %d\n", ret);
		goto fail;
	}

	priv->bridges[priv->num_bridges++]       = hdmi->bridge;

	return 0;

fail:
	/* bridge is normally destroyed by drm: */
	if (hdmi->bridge) {
		msm_hdmi_bridge_destroy(hdmi->bridge);
		hdmi->bridge = NULL;
	}
	if (hdmi->connector) {
		hdmi->connector->funcs->destroy(hdmi->connector);
		hdmi->connector = NULL;
	}

	return ret;
}

/*
 * The hdmi device:
 */

#define HDMI_CFG(item, entry) \
	.item ## _names = item ##_names_ ## entry, \
	.item ## _cnt   = ARRAY_SIZE(item ## _names_ ## entry)

static const char *hpd_reg_names_8960[] = {"core-vdda"};
static const char *hpd_clk_names_8960[] = {"core", "master_iface", "slave_iface"};

static const struct hdmi_platform_config hdmi_tx_8960_config = {
		HDMI_CFG(hpd_reg, 8960),
		HDMI_CFG(hpd_clk, 8960),
};

static const char *pwr_reg_names_8x74[] = {"core-vdda", "core-vcc"};
static const char *pwr_clk_names_8x74[] = {"extp", "alt_iface"};
static const char *hpd_clk_names_8x74[] = {"iface", "core", "mdp_core"};
static unsigned long hpd_clk_freq_8x74[] = {0, 19200000, 0};

static const struct hdmi_platform_config hdmi_tx_8974_config = {
		HDMI_CFG(pwr_reg, 8x74),
		HDMI_CFG(pwr_clk, 8x74),
		HDMI_CFG(hpd_clk, 8x74),
		.hpd_freq      = hpd_clk_freq_8x74,
};

/*
 * HDMI audio codec callbacks
 */
static int msm_hdmi_audio_hw_params(struct device *dev, void *data,
				    struct hdmi_codec_daifmt *daifmt,
				    struct hdmi_codec_params *params)
{
	struct hdmi *hdmi = dev_get_drvdata(dev);
	unsigned int chan;
	unsigned int channel_allocation = 0;
	unsigned int rate;
	unsigned int level_shift  = 0; /* 0dB */
	bool down_mix = false;

	DRM_DEV_DEBUG(dev, "%u Hz, %d bit, %d channels\n", params->sample_rate,
		 params->sample_width, params->cea.channels);

	switch (params->cea.channels) {
	case 2:
		/* FR and FL speakers */
		channel_allocation  = 0;
		chan = MSM_HDMI_AUDIO_CHANNEL_2;
		break;
	case 4:
		/* FC, LFE, FR and FL speakers */
		channel_allocation  = 0x3;
		chan = MSM_HDMI_AUDIO_CHANNEL_4;
		break;
	case 6:
		/* RR, RL, FC, LFE, FR and FL speakers */
		channel_allocation  = 0x0B;
		chan = MSM_HDMI_AUDIO_CHANNEL_6;
		break;
	case 8:
		/* FRC, FLC, RR, RL, FC, LFE, FR and FL speakers */
		channel_allocation  = 0x1F;
		chan = MSM_HDMI_AUDIO_CHANNEL_8;
		break;
	default:
		return -EINVAL;
	}

	switch (params->sample_rate) {
	case 32000:
		rate = HDMI_SAMPLE_RATE_32KHZ;
		break;
	case 44100:
		rate = HDMI_SAMPLE_RATE_44_1KHZ;
		break;
	case 48000:
		rate = HDMI_SAMPLE_RATE_48KHZ;
		break;
	case 88200:
		rate = HDMI_SAMPLE_RATE_88_2KHZ;
		break;
	case 96000:
		rate = HDMI_SAMPLE_RATE_96KHZ;
		break;
	case 176400:
		rate = HDMI_SAMPLE_RATE_176_4KHZ;
		break;
	case 192000:
		rate = HDMI_SAMPLE_RATE_192KHZ;
		break;
	default:
		DRM_DEV_ERROR(dev, "rate[%d] not supported!\n",
			params->sample_rate);
		return -EINVAL;
	}

	msm_hdmi_audio_set_sample_rate(hdmi, rate);
	msm_hdmi_audio_info_setup(hdmi, 1, chan, channel_allocation,
			      level_shift, down_mix);

	return 0;
}

static void msm_hdmi_audio_shutdown(struct device *dev, void *data)
{
	struct hdmi *hdmi = dev_get_drvdata(dev);

	msm_hdmi_audio_info_setup(hdmi, 0, 0, 0, 0, 0);
}

static const struct hdmi_codec_ops msm_hdmi_audio_codec_ops = {
	.hw_params = msm_hdmi_audio_hw_params,
	.audio_shutdown = msm_hdmi_audio_shutdown,
};

static struct hdmi_codec_pdata codec_data = {
	.ops = &msm_hdmi_audio_codec_ops,
	.max_i2s_channels = 8,
	.i2s = 1,
};

static int msm_hdmi_register_audio_driver(struct hdmi *hdmi, struct device *dev)
{
	hdmi->audio_pdev = platform_device_register_data(dev,
							 HDMI_CODEC_DRV_NAME,
							 PLATFORM_DEVID_AUTO,
							 &codec_data,
							 sizeof(codec_data));
	return PTR_ERR_OR_ZERO(hdmi->audio_pdev);
}

static int msm_hdmi_bind(struct device *dev, struct device *master, void *data)
{
	struct msm_drm_private *priv = dev_get_drvdata(master);
	struct hdmi *hdmi = dev_get_drvdata(dev);
	int err;

	err = msm_hdmi_init(hdmi);
	if (err)
		return err;
	priv->hdmi = hdmi;

	err = msm_hdmi_register_audio_driver(hdmi, dev);
	if (err) {
		DRM_ERROR("Failed to attach an audio codec %d\n", err);
		hdmi->audio_pdev = NULL;
	}

	return 0;
}

static void msm_hdmi_unbind(struct device *dev, struct device *master,
		void *data)
{
	struct msm_drm_private *priv = dev_get_drvdata(master);

	if (priv->hdmi) {
		if (priv->hdmi->audio_pdev)
			platform_device_unregister(priv->hdmi->audio_pdev);

		msm_hdmi_destroy(priv->hdmi);
		priv->hdmi = NULL;
	}
}

static const struct component_ops msm_hdmi_ops = {
		.bind   = msm_hdmi_bind,
		.unbind = msm_hdmi_unbind,
};

static int msm_hdmi_dev_probe(struct platform_device *pdev)
{
	const struct hdmi_platform_config *config;
	struct device *dev = &pdev->dev;
	struct hdmi *hdmi;
	struct resource *res;
	int i, ret;

	config = of_device_get_match_data(dev);
	if (!config)
		return -EINVAL;

	hdmi = devm_kzalloc(&pdev->dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;

	hdmi->pdev = pdev;
	hdmi->config = config;
	spin_lock_init(&hdmi->reg_lock);

	ret = drm_of_find_panel_or_bridge(pdev->dev.of_node, 1, 0, NULL, &hdmi->next_bridge);
	if (ret && ret != -ENODEV)
		return ret;

	hdmi->mmio = msm_ioremap(pdev, "core_physical");
	if (IS_ERR(hdmi->mmio))
		return PTR_ERR(hdmi->mmio);

	/* HDCP needs physical address of hdmi register */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
		"core_physical");
	if (!res)
		return -EINVAL;
	hdmi->mmio_phy_addr = res->start;

	hdmi->qfprom_mmio = msm_ioremap(pdev, "qfprom_physical");
	if (IS_ERR(hdmi->qfprom_mmio)) {
		DRM_DEV_INFO(&pdev->dev, "can't find qfprom resource\n");
		hdmi->qfprom_mmio = NULL;
	}

	hdmi->irq = platform_get_irq(pdev, 0);
	if (hdmi->irq < 0)
		return hdmi->irq;

	hdmi->hpd_regs = devm_kcalloc(&pdev->dev,
				      config->hpd_reg_cnt,
				      sizeof(hdmi->hpd_regs[0]),
				      GFP_KERNEL);
	if (!hdmi->hpd_regs)
		return -ENOMEM;

	for (i = 0; i < config->hpd_reg_cnt; i++)
		hdmi->hpd_regs[i].supply = config->hpd_reg_names[i];

	ret = devm_regulator_bulk_get(&pdev->dev, config->hpd_reg_cnt, hdmi->hpd_regs);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get hpd regulators\n");

	hdmi->pwr_regs = devm_kcalloc(&pdev->dev,
				      config->pwr_reg_cnt,
				      sizeof(hdmi->pwr_regs[0]),
				      GFP_KERNEL);
	if (!hdmi->pwr_regs)
		return -ENOMEM;

	for (i = 0; i < config->pwr_reg_cnt; i++)
		hdmi->pwr_regs[i].supply = config->pwr_reg_names[i];

	ret = devm_regulator_bulk_get(&pdev->dev, config->pwr_reg_cnt, hdmi->pwr_regs);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get pwr regulators\n");

	hdmi->hpd_clks = devm_kcalloc(&pdev->dev,
				      config->hpd_clk_cnt,
				      sizeof(hdmi->hpd_clks[0]),
				      GFP_KERNEL);
	if (!hdmi->hpd_clks)
		return -ENOMEM;

	for (i = 0; i < config->hpd_clk_cnt; i++) {
		struct clk *clk;

		clk = msm_clk_get(pdev, config->hpd_clk_names[i]);
		if (IS_ERR(clk))
			return dev_err_probe(dev, PTR_ERR(clk),
					     "failed to get hpd clk: %s\n",
					     config->hpd_clk_names[i]);

		hdmi->hpd_clks[i] = clk;
	}

	hdmi->pwr_clks = devm_kcalloc(&pdev->dev,
				      config->pwr_clk_cnt,
				      sizeof(hdmi->pwr_clks[0]),
				      GFP_KERNEL);
	if (!hdmi->pwr_clks)
		return -ENOMEM;

	for (i = 0; i < config->pwr_clk_cnt; i++) {
		struct clk *clk;

		clk = msm_clk_get(pdev, config->pwr_clk_names[i]);
		if (IS_ERR(clk))
			return dev_err_probe(dev, PTR_ERR(clk),
					     "failed to get pwr clk: %s\n",
					     config->pwr_clk_names[i]);

		hdmi->pwr_clks[i] = clk;
	}

	hdmi->hpd_gpiod = devm_gpiod_get_optional(&pdev->dev, "hpd", GPIOD_IN);
	/* This will catch e.g. -EPROBE_DEFER */
	if (IS_ERR(hdmi->hpd_gpiod))
		return dev_err_probe(dev, PTR_ERR(hdmi->hpd_gpiod),
				     "failed to get hpd gpio\n");

	if (!hdmi->hpd_gpiod)
		DBG("failed to get HPD gpio");

	if (hdmi->hpd_gpiod)
		gpiod_set_consumer_name(hdmi->hpd_gpiod, "HDMI_HPD");

	ret = msm_hdmi_get_phy(hdmi);
	if (ret) {
		DRM_DEV_ERROR(&pdev->dev, "failed to get phy\n");
		return ret;
	}

	ret = devm_pm_runtime_enable(&pdev->dev);
	if (ret)
		goto err_put_phy;

	platform_set_drvdata(pdev, hdmi);

	ret = component_add(&pdev->dev, &msm_hdmi_ops);
	if (ret)
		goto err_put_phy;

	return 0;

err_put_phy:
	msm_hdmi_put_phy(hdmi);
	return ret;
}

static int msm_hdmi_dev_remove(struct platform_device *pdev)
{
	struct hdmi *hdmi = dev_get_drvdata(&pdev->dev);

	component_del(&pdev->dev, &msm_hdmi_ops);

	msm_hdmi_put_phy(hdmi);

	return 0;
}

static const struct of_device_id msm_hdmi_dt_match[] = {
	{ .compatible = "qcom,hdmi-tx-8996", .data = &hdmi_tx_8974_config },
	{ .compatible = "qcom,hdmi-tx-8994", .data = &hdmi_tx_8974_config },
	{ .compatible = "qcom,hdmi-tx-8084", .data = &hdmi_tx_8974_config },
	{ .compatible = "qcom,hdmi-tx-8974", .data = &hdmi_tx_8974_config },
	{ .compatible = "qcom,hdmi-tx-8960", .data = &hdmi_tx_8960_config },
	{ .compatible = "qcom,hdmi-tx-8660", .data = &hdmi_tx_8960_config },
	{}
};

static struct platform_driver msm_hdmi_driver = {
	.probe = msm_hdmi_dev_probe,
	.remove = msm_hdmi_dev_remove,
	.driver = {
		.name = "hdmi_msm",
		.of_match_table = msm_hdmi_dt_match,
	},
};

void __init msm_hdmi_register(void)
{
	msm_hdmi_phy_driver_register();
	platform_driver_register(&msm_hdmi_driver);
}

void __exit msm_hdmi_unregister(void)
{
	platform_driver_unregister(&msm_hdmi_driver);
	msm_hdmi_phy_driver_unregister();
}
