/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MFD core driver for the RT5033
 *
 * Copyright (C) 2014 Samsung Electronics
 * Author: Beomho Seo <beomho.seo@samsung.com>
 */

#ifndef __RT5033_H__
#define __RT5033_H__

#include <linux/regulator/consumer.h>
#include <linux/i2c.h>
#include <linux/regmap.h>

/* RT5033 regulator IDs */
enum rt5033_regulators {
	RT5033_BUCK = 0,
	RT5033_LDO,
	RT5033_SAFE_LDO,

	RT5033_REGULATOR_NUM,
};

struct rt5033_dev {
	struct device *dev;

	struct regmap *regmap;
	struct regmap_irq_chip_data *irq_data;
	int irq;
	bool wakeup;
};

#endif /* __RT5033_H__ */
