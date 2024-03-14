// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas RZ/G2L WDT Watchdog Driver
 *
 * Copyright (C) 2021 Renesas Electronics Corporation
 */
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/units.h>
#include <linux/watchdog.h>

#define WDTCNT		0x00
#define WDTSET		0x04
#define WDTTIM		0x08
#define WDTINT		0x0C
#define PECR		0x10
#define PEEN		0x14
#define WDTCNT_WDTEN	BIT(0)
#define WDTINT_INTDISP	BIT(0)
#define PEEN_FORCE	BIT(0)

#define WDT_DEFAULT_TIMEOUT		60U

/* Setting period time register only 12 bit set in WDTSET[31:20] */
#define WDTSET_COUNTER_MASK		(0xFFF00000)
#define WDTSET_COUNTER_VAL(f)		((f) << 20)

#define F2CYCLE_NSEC(f)			(1000000000 / (f))

#define RZV2M_A_NSEC			730

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
				__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

enum rz_wdt_type {
	WDT_RZG2L,
	WDT_RZV2M,
};

struct rzg2l_wdt_priv {
	void __iomem *base;
	struct watchdog_device wdev;
	struct reset_control *rstc;
	unsigned long osc_clk_rate;
	unsigned long delay;
	unsigned long minimum_assertion_period;
	struct clk *pclk;
	struct clk *osc_clk;
	enum rz_wdt_type devtype;
};

static int rzg2l_wdt_reset(struct rzg2l_wdt_priv *priv)
{
	int err, status;

	if (priv->devtype == WDT_RZV2M) {
		/* WDT needs TYPE-B reset control */
		err = reset_control_assert(priv->rstc);
		if (err)
			return err;
		ndelay(priv->minimum_assertion_period);
		err = reset_control_deassert(priv->rstc);
		if (err)
			return err;
		err = read_poll_timeout(reset_control_status, status,
					status != 1, 0, 1000, false,
					priv->rstc);
	} else {
		err = reset_control_reset(priv->rstc);
	}

	return err;
}

static void rzg2l_wdt_wait_delay(struct rzg2l_wdt_priv *priv)
{
	/* delay timer when change the setting register */
	ndelay(priv->delay);
}

static u32 rzg2l_wdt_get_cycle_usec(unsigned long cycle, u32 wdttime)
{
	u64 timer_cycle_us = 1024 * 1024ULL * (wdttime + 1) * MICRO;

	return div64_ul(timer_cycle_us, cycle);
}

static void rzg2l_wdt_write(struct rzg2l_wdt_priv *priv, u32 val, unsigned int reg)
{
	if (reg == WDTSET)
		val &= WDTSET_COUNTER_MASK;

	writel_relaxed(val, priv->base + reg);
	/* Registers other than the WDTINT is always synchronized with WDT_CLK */
	if (reg != WDTINT)
		rzg2l_wdt_wait_delay(priv);
}

static void rzg2l_wdt_init_timeout(struct watchdog_device *wdev)
{
	struct rzg2l_wdt_priv *priv = watchdog_get_drvdata(wdev);
	u32 time_out;

	/* Clear Lapsed Time Register and clear Interrupt */
	rzg2l_wdt_write(priv, WDTINT_INTDISP, WDTINT);
	/* 2 consecutive overflow cycle needed to trigger reset */
	time_out = (wdev->timeout * (MICRO / 2)) /
		   rzg2l_wdt_get_cycle_usec(priv->osc_clk_rate, 0);
	rzg2l_wdt_write(priv, WDTSET_COUNTER_VAL(time_out), WDTSET);
}

static int rzg2l_wdt_start(struct watchdog_device *wdev)
{
	struct rzg2l_wdt_priv *priv = watchdog_get_drvdata(wdev);

	pm_runtime_get_sync(wdev->parent);

	/* Initialize time out */
	rzg2l_wdt_init_timeout(wdev);

	/* Initialize watchdog counter register */
	rzg2l_wdt_write(priv, 0, WDTTIM);

	/* Enable watchdog timer*/
	rzg2l_wdt_write(priv, WDTCNT_WDTEN, WDTCNT);

	return 0;
}

static int rzg2l_wdt_stop(struct watchdog_device *wdev)
{
	struct rzg2l_wdt_priv *priv = watchdog_get_drvdata(wdev);

	rzg2l_wdt_reset(priv);
	pm_runtime_put(wdev->parent);

	return 0;
}

static int rzg2l_wdt_set_timeout(struct watchdog_device *wdev, unsigned int timeout)
{
	wdev->timeout = timeout;

	/*
	 * If the watchdog is active, reset the module for updating the WDTSET
	 * register by calling rzg2l_wdt_stop() (which internally calls reset_control_reset()
	 * to reset the module) so that it is updated with new timeout values.
	 */
	if (watchdog_active(wdev)) {
		rzg2l_wdt_stop(wdev);
		rzg2l_wdt_start(wdev);
	}

	return 0;
}

static int rzg2l_wdt_restart(struct watchdog_device *wdev,
			     unsigned long action, void *data)
{
	struct rzg2l_wdt_priv *priv = watchdog_get_drvdata(wdev);

	clk_prepare_enable(priv->pclk);
	clk_prepare_enable(priv->osc_clk);

	if (priv->devtype == WDT_RZG2L) {
		/* Generate Reset (WDTRSTB) Signal on parity error */
		rzg2l_wdt_write(priv, 0, PECR);

		/* Force parity error */
		rzg2l_wdt_write(priv, PEEN_FORCE, PEEN);
	} else {
		/* RZ/V2M doesn't have parity error registers */
		rzg2l_wdt_reset(priv);

		wdev->timeout = 0;

		/* Initialize time out */
		rzg2l_wdt_init_timeout(wdev);

		/* Initialize watchdog counter register */
		rzg2l_wdt_write(priv, 0, WDTTIM);

		/* Enable watchdog timer*/
		rzg2l_wdt_write(priv, WDTCNT_WDTEN, WDTCNT);

		/* Wait 2 consecutive overflow cycles for reset */
		mdelay(DIV_ROUND_UP(2 * 0xFFFFF * 1000, priv->osc_clk_rate));
	}

	return 0;
}

static const struct watchdog_info rzg2l_wdt_ident = {
	.options = WDIOF_MAGICCLOSE | WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT,
	.identity = "Renesas RZ/G2L WDT Watchdog",
};

static int rzg2l_wdt_ping(struct watchdog_device *wdev)
{
	struct rzg2l_wdt_priv *priv = watchdog_get_drvdata(wdev);

	rzg2l_wdt_write(priv, WDTINT_INTDISP, WDTINT);

	return 0;
}

static const struct watchdog_ops rzg2l_wdt_ops = {
	.owner = THIS_MODULE,
	.start = rzg2l_wdt_start,
	.stop = rzg2l_wdt_stop,
	.ping = rzg2l_wdt_ping,
	.set_timeout = rzg2l_wdt_set_timeout,
	.restart = rzg2l_wdt_restart,
};

static void rzg2l_wdt_reset_assert_pm_disable(void *data)
{
	struct watchdog_device *wdev = data;
	struct rzg2l_wdt_priv *priv = watchdog_get_drvdata(wdev);

	pm_runtime_disable(wdev->parent);
	reset_control_assert(priv->rstc);
}

static int rzg2l_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rzg2l_wdt_priv *priv;
	unsigned long pclk_rate;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	/* Get watchdog main clock */
	priv->osc_clk = devm_clk_get(&pdev->dev, "oscclk");
	if (IS_ERR(priv->osc_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->osc_clk), "no oscclk");

	priv->osc_clk_rate = clk_get_rate(priv->osc_clk);
	if (!priv->osc_clk_rate)
		return dev_err_probe(&pdev->dev, -EINVAL, "oscclk rate is 0");

	/* Get Peripheral clock */
	priv->pclk = devm_clk_get(&pdev->dev, "pclk");
	if (IS_ERR(priv->pclk))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->pclk), "no pclk");

	pclk_rate = clk_get_rate(priv->pclk);
	if (!pclk_rate)
		return dev_err_probe(&pdev->dev, -EINVAL, "pclk rate is 0");

	priv->delay = F2CYCLE_NSEC(priv->osc_clk_rate) * 6 + F2CYCLE_NSEC(pclk_rate) * 9;

	priv->rstc = devm_reset_control_get_exclusive(&pdev->dev, NULL);
	if (IS_ERR(priv->rstc))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->rstc),
				     "failed to get cpg reset");

	ret = reset_control_deassert(priv->rstc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to deassert");

	priv->devtype = (uintptr_t)of_device_get_match_data(dev);

	if (priv->devtype == WDT_RZV2M) {
		priv->minimum_assertion_period = RZV2M_A_NSEC +
			3 * F2CYCLE_NSEC(pclk_rate) + 5 *
			max(F2CYCLE_NSEC(priv->osc_clk_rate),
			    F2CYCLE_NSEC(pclk_rate));
	}

	pm_runtime_enable(&pdev->dev);

	priv->wdev.info = &rzg2l_wdt_ident;
	priv->wdev.ops = &rzg2l_wdt_ops;
	priv->wdev.parent = dev;
	priv->wdev.min_timeout = 1;
	priv->wdev.max_timeout = rzg2l_wdt_get_cycle_usec(priv->osc_clk_rate, 0xfff) /
				 USEC_PER_SEC;
	priv->wdev.timeout = WDT_DEFAULT_TIMEOUT;

	watchdog_set_drvdata(&priv->wdev, priv);
	ret = devm_add_action_or_reset(&pdev->dev,
				       rzg2l_wdt_reset_assert_pm_disable,
				       &priv->wdev);
	if (ret < 0)
		return ret;

	watchdog_set_nowayout(&priv->wdev, nowayout);
	watchdog_stop_on_unregister(&priv->wdev);

	ret = watchdog_init_timeout(&priv->wdev, 0, dev);
	if (ret)
		dev_warn(dev, "Specified timeout invalid, using default");

	return devm_watchdog_register_device(&pdev->dev, &priv->wdev);
}

static const struct of_device_id rzg2l_wdt_ids[] = {
	{ .compatible = "renesas,rzg2l-wdt", .data = (void *)WDT_RZG2L },
	{ .compatible = "renesas,rzv2m-wdt", .data = (void *)WDT_RZV2M },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rzg2l_wdt_ids);

static struct platform_driver rzg2l_wdt_driver = {
	.driver = {
		.name = "rzg2l_wdt",
		.of_match_table = rzg2l_wdt_ids,
	},
	.probe = rzg2l_wdt_probe,
};
module_platform_driver(rzg2l_wdt_driver);

MODULE_DESCRIPTION("Renesas RZ/G2L WDT Watchdog Driver");
MODULE_AUTHOR("Biju Das <biju.das.jz@bp.renesas.com>");
MODULE_LICENSE("GPL v2");
