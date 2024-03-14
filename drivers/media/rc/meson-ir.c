// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Amlogic Meson IR remote receiver
 *
 * Copyright (C) 2014 Beniamino Galvani <b.galvani@gmail.com>
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/bitfield.h>
#include <linux/regmap.h>

#include <media/rc-core.h>

#define DRIVER_NAME		"meson-ir"

#define IR_DEC_LDR_ACTIVE	0x00
#define IR_DEC_LDR_IDLE		0x04
#define IR_DEC_LDR_REPEAT	0x08
#define IR_DEC_BIT_0		0x0c
#define IR_DEC_REG0		0x10
#define IR_DEC_REG0_BASE_TIME	GENMASK(11, 0)
#define IR_DEC_FRAME		0x14
#define IR_DEC_STATUS		0x18
#define IR_DEC_STATUS_PULSE	BIT(8)
#define IR_DEC_REG1		0x1c
#define IR_DEC_REG1_TIME_IV	GENMASK(28, 16)
#define IR_DEC_REG1_ENABLE	BIT(15)
#define IR_DEC_REG1_MODE	GENMASK(8, 7)
#define IR_DEC_REG1_IRQSEL	GENMASK(3, 2)
#define IR_DEC_REG1_RESET	BIT(0)
/* The following regs are only available on Meson 8b and newer */
#define IR_DEC_REG2		0x20
#define IR_DEC_REG2_MODE	GENMASK(3, 0)

#define DEC_MODE_NEC		0x0
#define DEC_MODE_RAW		0x2

#define IRQSEL_NEC_MODE		0
#define IRQSEL_RISE_FALL	1
#define IRQSEL_FALL		2
#define IRQSEL_RISE		3

#define MESON_RAW_TRATE		10	/* us */
#define MESON_HW_TRATE		20	/* us */

struct meson_ir {
	struct regmap	*reg;
	struct rc_dev	*rc;
	spinlock_t	lock;
};

static const struct regmap_config meson_ir_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static irqreturn_t meson_ir_irq(int irqno, void *dev_id)
{
	struct meson_ir *ir = dev_id;
	u32 duration, status;
	struct ir_raw_event rawir = {};

	spin_lock(&ir->lock);

	regmap_read(ir->reg, IR_DEC_REG1, &duration);
	duration = FIELD_GET(IR_DEC_REG1_TIME_IV, duration);
	rawir.duration = duration * MESON_RAW_TRATE;

	regmap_read(ir->reg, IR_DEC_STATUS, &status);
	rawir.pulse = !!(status & IR_DEC_STATUS_PULSE);

	ir_raw_event_store_with_timeout(ir->rc, &rawir);

	spin_unlock(&ir->lock);

	return IRQ_HANDLED;
}

static int meson_ir_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	void __iomem *res_start;
	const char *map_name;
	struct meson_ir *ir;
	int irq, ret;

	ir = devm_kzalloc(dev, sizeof(struct meson_ir), GFP_KERNEL);
	if (!ir)
		return -ENOMEM;

	res_start = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(res_start))
		return PTR_ERR(res_start);

	ir->reg = devm_regmap_init_mmio(&pdev->dev, res_start,
					&meson_ir_regmap_config);
	if (IS_ERR(ir->reg))
		return PTR_ERR(ir->reg);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ir->rc = devm_rc_allocate_device(dev, RC_DRIVER_IR_RAW);
	if (!ir->rc) {
		dev_err(dev, "failed to allocate rc device\n");
		return -ENOMEM;
	}

	ir->rc->priv = ir;
	ir->rc->device_name = DRIVER_NAME;
	ir->rc->input_phys = DRIVER_NAME "/input0";
	ir->rc->input_id.bustype = BUS_HOST;
	map_name = of_get_property(node, "linux,rc-map-name", NULL);
	ir->rc->map_name = map_name ? map_name : RC_MAP_EMPTY;
	ir->rc->allowed_protocols = RC_PROTO_BIT_ALL_IR_DECODER;
	ir->rc->rx_resolution = MESON_RAW_TRATE;
	ir->rc->min_timeout = 1;
	ir->rc->timeout = IR_DEFAULT_TIMEOUT;
	ir->rc->max_timeout = 10 * IR_DEFAULT_TIMEOUT;
	ir->rc->driver_name = DRIVER_NAME;

	spin_lock_init(&ir->lock);
	platform_set_drvdata(pdev, ir);

	ret = devm_rc_register_device(dev, ir->rc);
	if (ret) {
		dev_err(dev, "failed to register rc device\n");
		return ret;
	}

	ret = devm_request_irq(dev, irq, meson_ir_irq, 0, NULL, ir);
	if (ret) {
		dev_err(dev, "failed to request irq\n");
		return ret;
	}

	/* Reset the decoder */
	regmap_update_bits(ir->reg, IR_DEC_REG1, IR_DEC_REG1_RESET,
			   IR_DEC_REG1_RESET);
	regmap_update_bits(ir->reg, IR_DEC_REG1, IR_DEC_REG1_RESET, 0);

	/* Set general operation mode (= raw/software decoding) */
	if (of_device_is_compatible(node, "amlogic,meson6-ir"))
		regmap_update_bits(ir->reg, IR_DEC_REG1, IR_DEC_REG1_MODE,
				   FIELD_PREP(IR_DEC_REG1_MODE, DEC_MODE_RAW));
	else
		regmap_update_bits(ir->reg, IR_DEC_REG2, IR_DEC_REG2_MODE,
				   FIELD_PREP(IR_DEC_REG2_MODE, DEC_MODE_RAW));

	/* Set rate */
	regmap_update_bits(ir->reg, IR_DEC_REG0, IR_DEC_REG0_BASE_TIME,
			   FIELD_PREP(IR_DEC_REG0_BASE_TIME,
				      MESON_RAW_TRATE - 1));
	/* IRQ on rising and falling edges */
	regmap_update_bits(ir->reg, IR_DEC_REG1, IR_DEC_REG1_IRQSEL,
			   FIELD_PREP(IR_DEC_REG1_IRQSEL, IRQSEL_RISE_FALL));
	/* Enable the decoder */
	regmap_update_bits(ir->reg, IR_DEC_REG1, IR_DEC_REG1_ENABLE,
			   IR_DEC_REG1_ENABLE);

	dev_info(dev, "receiver initialized\n");

	return 0;
}

static void meson_ir_remove(struct platform_device *pdev)
{
	struct meson_ir *ir = platform_get_drvdata(pdev);
	unsigned long flags;

	/* Disable the decoder */
	spin_lock_irqsave(&ir->lock, flags);
	regmap_update_bits(ir->reg, IR_DEC_REG1, IR_DEC_REG1_ENABLE, 0);
	spin_unlock_irqrestore(&ir->lock, flags);
}

static void meson_ir_shutdown(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct meson_ir *ir = platform_get_drvdata(pdev);
	unsigned long flags;

	spin_lock_irqsave(&ir->lock, flags);

	/*
	 * Set operation mode to NEC/hardware decoding to give
	 * bootloader a chance to power the system back on
	 */
	if (of_device_is_compatible(node, "amlogic,meson6-ir"))
		regmap_update_bits(ir->reg, IR_DEC_REG1, IR_DEC_REG1_MODE,
				   FIELD_PREP(IR_DEC_REG1_MODE, DEC_MODE_NEC));
	else
		regmap_update_bits(ir->reg, IR_DEC_REG2, IR_DEC_REG2_MODE,
				   FIELD_PREP(IR_DEC_REG2_MODE, DEC_MODE_NEC));

	/* Set rate to default value */
	regmap_update_bits(ir->reg, IR_DEC_REG0, IR_DEC_REG0_BASE_TIME,
			   FIELD_PREP(IR_DEC_REG0_BASE_TIME,
				      MESON_HW_TRATE - 1));

	spin_unlock_irqrestore(&ir->lock, flags);
}

static const struct of_device_id meson_ir_match[] = {
	{ .compatible = "amlogic,meson6-ir" },
	{ .compatible = "amlogic,meson8b-ir" },
	{ .compatible = "amlogic,meson-gxbb-ir" },
	{ },
};
MODULE_DEVICE_TABLE(of, meson_ir_match);

static struct platform_driver meson_ir_driver = {
	.probe		= meson_ir_probe,
	.remove_new	= meson_ir_remove,
	.shutdown	= meson_ir_shutdown,
	.driver = {
		.name		= DRIVER_NAME,
		.of_match_table	= meson_ir_match,
	},
};

module_platform_driver(meson_ir_driver);

MODULE_DESCRIPTION("Amlogic Meson IR remote receiver driver");
MODULE_AUTHOR("Beniamino Galvani <b.galvani@gmail.com>");
MODULE_LICENSE("GPL v2");
