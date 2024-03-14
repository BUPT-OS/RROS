// SPDX-License-Identifier: GPL-2.0
//
// DFL bus driver for Altera SPI Master
//
// Copyright (C) 2020 Intel Corporation, Inc.
//
// Authors:
//   Matthew Gerlach <matthew.gerlach@linux.intel.com>
//

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/stddef.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/bitfield.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/spi/altera.h>
#include <linux/dfl.h>

#define FME_FEATURE_ID_MAX10_SPI	0xe
#define FME_FEATURE_REV_MAX10_SPI_N5010	0x1

#define SPI_CORE_PARAMETER      0x8
#define SHIFT_MODE              BIT_ULL(1)
#define SHIFT_MODE_MSB          0
#define SHIFT_MODE_LSB          1
#define DATA_WIDTH              GENMASK_ULL(7, 2)
#define NUM_CHIPSELECT          GENMASK_ULL(13, 8)
#define CLK_POLARITY            BIT_ULL(14)
#define CLK_PHASE               BIT_ULL(15)
#define PERIPHERAL_ID           GENMASK_ULL(47, 32)
#define SPI_CLK                 GENMASK_ULL(31, 22)
#define SPI_INDIRECT_ACC_OFST   0x10

#define INDIRECT_ADDR           (SPI_INDIRECT_ACC_OFST+0x0)
#define INDIRECT_WR             BIT_ULL(8)
#define INDIRECT_RD             BIT_ULL(9)
#define INDIRECT_RD_DATA        (SPI_INDIRECT_ACC_OFST+0x8)
#define INDIRECT_DATA_MASK      GENMASK_ULL(31, 0)
#define INDIRECT_DEBUG          BIT_ULL(32)
#define INDIRECT_WR_DATA        (SPI_INDIRECT_ACC_OFST+0x10)
#define INDIRECT_TIMEOUT        10000

static int indirect_bus_reg_read(void *context, unsigned int reg,
				 unsigned int *val)
{
	void __iomem *base = context;
	int loops;
	u64 v;

	writeq((reg >> 2) | INDIRECT_RD, base + INDIRECT_ADDR);

	loops = 0;
	while ((readq(base + INDIRECT_ADDR) & INDIRECT_RD) &&
	       (loops++ < INDIRECT_TIMEOUT))
		cpu_relax();

	if (loops >= INDIRECT_TIMEOUT) {
		pr_err("%s timed out %d\n", __func__, loops);
		return -ETIME;
	}

	v = readq(base + INDIRECT_RD_DATA);

	*val = v & INDIRECT_DATA_MASK;

	return 0;
}

static int indirect_bus_reg_write(void *context, unsigned int reg,
				  unsigned int val)
{
	void __iomem *base = context;
	int loops;

	writeq(val, base + INDIRECT_WR_DATA);
	writeq((reg >> 2) | INDIRECT_WR, base + INDIRECT_ADDR);

	loops = 0;
	while ((readq(base + INDIRECT_ADDR) & INDIRECT_WR) &&
	       (loops++ < INDIRECT_TIMEOUT))
		cpu_relax();

	if (loops >= INDIRECT_TIMEOUT) {
		pr_err("%s timed out %d\n", __func__, loops);
		return -ETIME;
	}
	return 0;
}

static const struct regmap_config indirect_regbus_cfg = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.fast_io = true,
	.max_register = 24,

	.reg_write = indirect_bus_reg_write,
	.reg_read = indirect_bus_reg_read,
};

static void config_spi_host(void __iomem *base, struct spi_controller *host)
{
	u64 v;

	v = readq(base + SPI_CORE_PARAMETER);

	host->mode_bits = SPI_CS_HIGH;
	if (FIELD_GET(CLK_POLARITY, v))
		host->mode_bits |= SPI_CPOL;
	if (FIELD_GET(CLK_PHASE, v))
		host->mode_bits |= SPI_CPHA;

	host->num_chipselect = FIELD_GET(NUM_CHIPSELECT, v);
	host->bits_per_word_mask =
		SPI_BPW_RANGE_MASK(1, FIELD_GET(DATA_WIDTH, v));
}

static int dfl_spi_altera_probe(struct dfl_device *dfl_dev)
{
	struct spi_board_info board_info = { 0 };
	struct device *dev = &dfl_dev->dev;
	struct spi_controller *host;
	struct altera_spi *hw;
	void __iomem *base;
	int err;

	host = devm_spi_alloc_host(dev, sizeof(struct altera_spi));
	if (!host)
		return -ENOMEM;

	host->bus_num = -1;

	hw = spi_controller_get_devdata(host);

	hw->dev = dev;

	base = devm_ioremap_resource(dev, &dfl_dev->mmio_res);

	if (IS_ERR(base))
		return PTR_ERR(base);

	config_spi_host(base, host);
	dev_dbg(dev, "%s cs %u bpm 0x%x mode 0x%x\n", __func__,
		host->num_chipselect, host->bits_per_word_mask,
		host->mode_bits);

	hw->regmap = devm_regmap_init(dev, NULL, base, &indirect_regbus_cfg);
	if (IS_ERR(hw->regmap))
		return PTR_ERR(hw->regmap);

	hw->irq = -EINVAL;

	altera_spi_init_host(host);

	err = devm_spi_register_controller(dev, host);
	if (err)
		return dev_err_probe(dev, err, "%s failed to register spi host\n",
				     __func__);

	if (dfl_dev->revision == FME_FEATURE_REV_MAX10_SPI_N5010)
		strscpy(board_info.modalias, "m10-n5010", SPI_NAME_SIZE);
	else
		strscpy(board_info.modalias, "m10-d5005", SPI_NAME_SIZE);

	board_info.max_speed_hz = 12500000;
	board_info.bus_num = 0;
	board_info.chip_select = 0;

	if (!spi_new_device(host, &board_info)) {
		dev_err(dev, "%s failed to create SPI device: %s\n",
			__func__, board_info.modalias);
	}

	return 0;
}

static const struct dfl_device_id dfl_spi_altera_ids[] = {
	{ FME_ID, FME_FEATURE_ID_MAX10_SPI },
	{ }
};

static struct dfl_driver dfl_spi_altera_driver = {
	.drv	= {
		.name       = "dfl-spi-altera",
	},
	.id_table = dfl_spi_altera_ids,
	.probe   = dfl_spi_altera_probe,
};

module_dfl_driver(dfl_spi_altera_driver);

MODULE_DEVICE_TABLE(dfl, dfl_spi_altera_ids);
MODULE_DESCRIPTION("DFL spi altera driver");
MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
