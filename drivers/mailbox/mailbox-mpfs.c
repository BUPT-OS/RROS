// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip PolarFire SoC (MPFS) system controller/mailbox controller driver
 *
 * Copyright (c) 2020-2022 Microchip Corporation. All rights reserved.
 *
 * Author: Conor Dooley <conor.dooley@microchip.com>
 *
 */

#include <linux/io.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/mailbox_controller.h>
#include <soc/microchip/mpfs.h>

#define SERVICES_CR_OFFSET		0x50u
#define SERVICES_SR_OFFSET		0x54u
#define MAILBOX_REG_OFFSET		0x800u
#define MSS_SYS_MAILBOX_DATA_OFFSET	0u
#define SCB_MASK_WIDTH			16u

/* SCBCTRL service control register */

#define SCB_CTRL_REQ (0)
#define SCB_CTRL_REQ_MASK BIT(SCB_CTRL_REQ)

#define SCB_CTRL_BUSY (1)
#define SCB_CTRL_BUSY_MASK BIT(SCB_CTRL_BUSY)

#define SCB_CTRL_ABORT (2)
#define SCB_CTRL_ABORT_MASK BIT(SCB_CTRL_ABORT)

#define SCB_CTRL_NOTIFY (3)
#define SCB_CTRL_NOTIFY_MASK BIT(SCB_CTRL_NOTIFY)

#define SCB_CTRL_POS (16)
#define SCB_CTRL_MASK GENMASK(SCB_CTRL_POS + SCB_MASK_WIDTH - 1, SCB_CTRL_POS)

/* SCBCTRL service status register */

#define SCB_STATUS_REQ (0)
#define SCB_STATUS_REQ_MASK BIT(SCB_STATUS_REQ)

#define SCB_STATUS_BUSY (1)
#define SCB_STATUS_BUSY_MASK BIT(SCB_STATUS_BUSY)

#define SCB_STATUS_ABORT (2)
#define SCB_STATUS_ABORT_MASK BIT(SCB_STATUS_ABORT)

#define SCB_STATUS_NOTIFY (3)
#define SCB_STATUS_NOTIFY_MASK BIT(SCB_STATUS_NOTIFY)

#define SCB_STATUS_POS (16)
#define SCB_STATUS_MASK GENMASK(SCB_STATUS_POS + SCB_MASK_WIDTH - 1, SCB_STATUS_POS)

struct mpfs_mbox {
	struct mbox_controller controller;
	struct device *dev;
	int irq;
	void __iomem *ctrl_base;
	void __iomem *mbox_base;
	void __iomem *int_reg;
	struct mbox_chan chans[1];
	struct mpfs_mss_response *response;
	u16 resp_offset;
};

static bool mpfs_mbox_busy(struct mpfs_mbox *mbox)
{
	u32 status;

	status = readl_relaxed(mbox->ctrl_base + SERVICES_SR_OFFSET);

	return status & SCB_STATUS_BUSY_MASK;
}

static bool mpfs_mbox_last_tx_done(struct mbox_chan *chan)
{
	struct mpfs_mbox *mbox = (struct mpfs_mbox *)chan->con_priv;
	struct mpfs_mss_response *response = mbox->response;
	u32 val;

	if (mpfs_mbox_busy(mbox))
		return false;

	/*
	 * The service status is stored in bits 31:16 of the SERVICES_SR
	 * register & is only valid when the system controller is not busy.
	 * Failed services are intended to generated interrupts, but in reality
	 * this does not happen, so the status must be checked here.
	 */
	val = readl_relaxed(mbox->ctrl_base + SERVICES_SR_OFFSET);
	response->resp_status = (val & SCB_STATUS_MASK) >> SCB_STATUS_POS;

	return true;
}

static int mpfs_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct mpfs_mbox *mbox = (struct mpfs_mbox *)chan->con_priv;
	struct mpfs_mss_msg *msg = data;
	u32 tx_trigger;
	u16 opt_sel;
	u32 val = 0u;

	mbox->response = msg->response;
	mbox->resp_offset = msg->resp_offset;

	if (mpfs_mbox_busy(mbox))
		return -EBUSY;

	if (msg->cmd_data_size) {
		u32 index;
		u8 extra_bits = msg->cmd_data_size & 3;
		u32 *word_buf = (u32 *)msg->cmd_data;

		for (index = 0; index < (msg->cmd_data_size / 4); index++)
			writel_relaxed(word_buf[index],
				       mbox->mbox_base + msg->mbox_offset + index * 0x4);
		if (extra_bits) {
			u8 i;
			u8 byte_off = ALIGN_DOWN(msg->cmd_data_size, 4);
			u8 *byte_buf = msg->cmd_data + byte_off;

			val = readl_relaxed(mbox->mbox_base + msg->mbox_offset + index * 0x4);

			for (i = 0u; i < extra_bits; i++) {
				val &= ~(0xffu << (i * 8u));
				val |= (byte_buf[i] << (i * 8u));
			}

			writel_relaxed(val, mbox->mbox_base + msg->mbox_offset + index * 0x4);
		}
	}

	opt_sel = ((msg->mbox_offset << 7u) | (msg->cmd_opcode & 0x7fu));

	tx_trigger = (opt_sel << SCB_CTRL_POS) & SCB_CTRL_MASK;
	tx_trigger |= SCB_CTRL_REQ_MASK | SCB_STATUS_NOTIFY_MASK;
	writel_relaxed(tx_trigger, mbox->ctrl_base + SERVICES_CR_OFFSET);

	return 0;
}

static void mpfs_mbox_rx_data(struct mbox_chan *chan)
{
	struct mpfs_mbox *mbox = (struct mpfs_mbox *)chan->con_priv;
	struct mpfs_mss_response *response = mbox->response;
	u16 num_words = ALIGN((response->resp_size), (4)) / 4U;
	u32 i;

	if (!response->resp_msg) {
		dev_err(mbox->dev, "failed to assign memory for response %d\n", -ENOMEM);
		return;
	}

	/*
	 * We should *never* get an interrupt while the controller is
	 * still in the busy state. If we do, something has gone badly
	 * wrong & the content of the mailbox would not be valid.
	 */
	if (mpfs_mbox_busy(mbox)) {
		dev_err(mbox->dev, "got an interrupt but system controller is busy\n");
		response->resp_status = 0xDEAD;
		return;
	}

	for (i = 0; i < num_words; i++) {
		response->resp_msg[i] =
			readl_relaxed(mbox->mbox_base
				      + mbox->resp_offset + i * 0x4);
	}

	mbox_chan_received_data(chan, response);
}

static irqreturn_t mpfs_mbox_inbox_isr(int irq, void *data)
{
	struct mbox_chan *chan = data;
	struct mpfs_mbox *mbox = (struct mpfs_mbox *)chan->con_priv;

	writel_relaxed(0, mbox->int_reg);

	mpfs_mbox_rx_data(chan);

	return IRQ_HANDLED;
}

static int mpfs_mbox_startup(struct mbox_chan *chan)
{
	struct mpfs_mbox *mbox = (struct mpfs_mbox *)chan->con_priv;
	int ret = 0;

	if (!mbox)
		return -EINVAL;

	ret = devm_request_irq(mbox->dev, mbox->irq, mpfs_mbox_inbox_isr, 0, "mpfs-mailbox", chan);
	if (ret)
		dev_err(mbox->dev, "failed to register mailbox interrupt:%d\n", ret);

	return ret;
}

static void mpfs_mbox_shutdown(struct mbox_chan *chan)
{
	struct mpfs_mbox *mbox = (struct mpfs_mbox *)chan->con_priv;

	devm_free_irq(mbox->dev, mbox->irq, chan);
}

static const struct mbox_chan_ops mpfs_mbox_ops = {
	.send_data = mpfs_mbox_send_data,
	.startup = mpfs_mbox_startup,
	.shutdown = mpfs_mbox_shutdown,
	.last_tx_done = mpfs_mbox_last_tx_done,
};

static int mpfs_mbox_probe(struct platform_device *pdev)
{
	struct mpfs_mbox *mbox;
	struct resource *regs;
	int ret;

	mbox = devm_kzalloc(&pdev->dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;

	mbox->ctrl_base = devm_platform_get_and_ioremap_resource(pdev, 0, &regs);
	if (IS_ERR(mbox->ctrl_base))
		return PTR_ERR(mbox->ctrl_base);

	mbox->int_reg = devm_platform_get_and_ioremap_resource(pdev, 1, &regs);
	if (IS_ERR(mbox->int_reg))
		return PTR_ERR(mbox->int_reg);

	mbox->mbox_base = devm_platform_get_and_ioremap_resource(pdev, 2, &regs);
	if (IS_ERR(mbox->mbox_base)) // account for the old dt-binding w/ 2 regs
		mbox->mbox_base = mbox->ctrl_base + MAILBOX_REG_OFFSET;

	mbox->irq = platform_get_irq(pdev, 0);
	if (mbox->irq < 0)
		return mbox->irq;

	mbox->dev = &pdev->dev;

	mbox->chans[0].con_priv = mbox;
	mbox->controller.dev = mbox->dev;
	mbox->controller.num_chans = 1;
	mbox->controller.chans = mbox->chans;
	mbox->controller.ops = &mpfs_mbox_ops;
	mbox->controller.txdone_poll = true;
	mbox->controller.txpoll_period = 10u;

	ret = devm_mbox_controller_register(&pdev->dev, &mbox->controller);
	if (ret) {
		dev_err(&pdev->dev, "Registering MPFS mailbox controller failed\n");
		return ret;
	}
	dev_info(&pdev->dev, "Registered MPFS mailbox controller driver\n");

	return 0;
}

static const struct of_device_id mpfs_mbox_of_match[] = {
	{.compatible = "microchip,mpfs-mailbox", },
	{},
};
MODULE_DEVICE_TABLE(of, mpfs_mbox_of_match);

static struct platform_driver mpfs_mbox_driver = {
	.driver = {
		.name = "mpfs-mailbox",
		.of_match_table = mpfs_mbox_of_match,
	},
	.probe = mpfs_mbox_probe,
};
module_platform_driver(mpfs_mbox_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Conor Dooley <conor.dooley@microchip.com>");
MODULE_DESCRIPTION("MPFS mailbox controller driver");
