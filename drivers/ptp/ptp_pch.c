// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTP 1588 clock using the EG20T PCH
 *
 * Copyright (C) 2010 OMICRON electronics GmbH
 * Copyright (C) 2011-2012 LAPIS SEMICONDUCTOR Co., LTD.
 *
 * This code was derived from the IXP46X driver.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/io-64-nonatomic-hi-lo.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/ptp_pch.h>
#include <linux/slab.h>

#define STATION_ADDR_LEN	20
#define PCI_DEVICE_ID_PCH_1588	0x8819
#define IO_MEM_BAR 1

#define DEFAULT_ADDEND 0xA0000000
#define TICKS_NS_SHIFT  5
#define N_EXT_TS	2

enum pch_status {
	PCH_SUCCESS,
	PCH_INVALIDPARAM,
	PCH_NOTIMESTAMP,
	PCH_INTERRUPTMODEINUSE,
	PCH_FAILED,
	PCH_UNSUPPORTED,
};

/*
 * struct pch_ts_regs - IEEE 1588 registers
 */
struct pch_ts_regs {
	u32 control;
	u32 event;
	u32 addend;
	u32 accum;
	u32 test;
	u32 ts_compare;
	u32 rsystime_lo;
	u32 rsystime_hi;
	u32 systime_lo;
	u32 systime_hi;
	u32 trgt_lo;
	u32 trgt_hi;
	u32 asms_lo;
	u32 asms_hi;
	u32 amms_lo;
	u32 amms_hi;
	u32 ch_control;
	u32 ch_event;
	u32 tx_snap_lo;
	u32 tx_snap_hi;
	u32 rx_snap_lo;
	u32 rx_snap_hi;
	u32 src_uuid_lo;
	u32 src_uuid_hi;
	u32 can_status;
	u32 can_snap_lo;
	u32 can_snap_hi;
	u32 ts_sel;
	u32 ts_st[6];
	u32 reserve1[14];
	u32 stl_max_set_en;
	u32 stl_max_set;
	u32 reserve2[13];
	u32 srst;
};

#define PCH_TSC_RESET		(1 << 0)
#define PCH_TSC_TTM_MASK	(1 << 1)
#define PCH_TSC_ASMS_MASK	(1 << 2)
#define PCH_TSC_AMMS_MASK	(1 << 3)
#define PCH_TSC_PPSM_MASK	(1 << 4)
#define PCH_TSE_TTIPEND		(1 << 1)
#define PCH_TSE_SNS		(1 << 2)
#define PCH_TSE_SNM		(1 << 3)
#define PCH_TSE_PPS		(1 << 4)
#define PCH_CC_MM		(1 << 0)
#define PCH_CC_TA		(1 << 1)

#define PCH_CC_MODE_SHIFT	16
#define PCH_CC_MODE_MASK	0x001F0000
#define PCH_CC_VERSION		(1 << 31)
#define PCH_CE_TXS		(1 << 0)
#define PCH_CE_RXS		(1 << 1)
#define PCH_CE_OVR		(1 << 0)
#define PCH_CE_VAL		(1 << 1)
#define PCH_ECS_ETH		(1 << 0)

#define PCH_ECS_CAN		(1 << 1)

#define PCH_IEEE1588_ETH	(1 << 0)
#define PCH_IEEE1588_CAN	(1 << 1)

/*
 * struct pch_dev - Driver private data
 */
struct pch_dev {
	struct pch_ts_regs __iomem *regs;
	struct ptp_clock *ptp_clock;
	struct ptp_clock_info caps;
	int exts0_enabled;
	int exts1_enabled;

	u32 irq;
	struct pci_dev *pdev;
	spinlock_t register_lock;
};

/*
 * struct pch_params - 1588 module parameter
 */
struct pch_params {
	u8 station[STATION_ADDR_LEN];
};

/* structure to hold the module parameters */
static struct pch_params pch_param = {
	"00:00:00:00:00:00"
};

/*
 * Register access functions
 */
static inline void pch_eth_enable_set(struct pch_dev *chip)
{
	u32 val;
	/* SET the eth_enable bit */
	val = ioread32(&chip->regs->ts_sel) | (PCH_ECS_ETH);
	iowrite32(val, (&chip->regs->ts_sel));
}

static u64 pch_systime_read(struct pch_ts_regs __iomem *regs)
{
	u64 ns;

	ns = ioread64_lo_hi(&regs->systime_lo);

	return ns << TICKS_NS_SHIFT;
}

static void pch_systime_write(struct pch_ts_regs __iomem *regs, u64 ns)
{
	iowrite64_lo_hi(ns >> TICKS_NS_SHIFT, &regs->systime_lo);
}

static inline void pch_block_reset(struct pch_dev *chip)
{
	u32 val;
	/* Reset Hardware Assist block */
	val = ioread32(&chip->regs->control) | PCH_TSC_RESET;
	iowrite32(val, (&chip->regs->control));
	val = val & ~PCH_TSC_RESET;
	iowrite32(val, (&chip->regs->control));
}

void pch_ch_control_write(struct pci_dev *pdev, u32 val)
{
	struct pch_dev *chip = pci_get_drvdata(pdev);

	iowrite32(val, (&chip->regs->ch_control));
}
EXPORT_SYMBOL(pch_ch_control_write);

u32 pch_ch_event_read(struct pci_dev *pdev)
{
	struct pch_dev *chip = pci_get_drvdata(pdev);
	u32 val;

	val = ioread32(&chip->regs->ch_event);

	return val;
}
EXPORT_SYMBOL(pch_ch_event_read);

void pch_ch_event_write(struct pci_dev *pdev, u32 val)
{
	struct pch_dev *chip = pci_get_drvdata(pdev);

	iowrite32(val, (&chip->regs->ch_event));
}
EXPORT_SYMBOL(pch_ch_event_write);

u32 pch_src_uuid_lo_read(struct pci_dev *pdev)
{
	struct pch_dev *chip = pci_get_drvdata(pdev);
	u32 val;

	val = ioread32(&chip->regs->src_uuid_lo);

	return val;
}
EXPORT_SYMBOL(pch_src_uuid_lo_read);

u32 pch_src_uuid_hi_read(struct pci_dev *pdev)
{
	struct pch_dev *chip = pci_get_drvdata(pdev);
	u32 val;

	val = ioread32(&chip->regs->src_uuid_hi);

	return val;
}
EXPORT_SYMBOL(pch_src_uuid_hi_read);

u64 pch_rx_snap_read(struct pci_dev *pdev)
{
	struct pch_dev *chip = pci_get_drvdata(pdev);
	u64 ns;

	ns = ioread64_lo_hi(&chip->regs->rx_snap_lo);

	return ns << TICKS_NS_SHIFT;
}
EXPORT_SYMBOL(pch_rx_snap_read);

u64 pch_tx_snap_read(struct pci_dev *pdev)
{
	struct pch_dev *chip = pci_get_drvdata(pdev);
	u64 ns;

	ns = ioread64_lo_hi(&chip->regs->tx_snap_lo);

	return ns << TICKS_NS_SHIFT;
}
EXPORT_SYMBOL(pch_tx_snap_read);

/* This function enables all 64 bits in system time registers [high & low].
This is a work-around for non continuous value in the SystemTime Register*/
static void pch_set_system_time_count(struct pch_dev *chip)
{
	iowrite32(0x01, &chip->regs->stl_max_set_en);
	iowrite32(0xFFFFFFFF, &chip->regs->stl_max_set);
	iowrite32(0x00, &chip->regs->stl_max_set_en);
}

static void pch_reset(struct pch_dev *chip)
{
	/* Reset Hardware Assist */
	pch_block_reset(chip);

	/* enable all 32 bits in system time registers */
	pch_set_system_time_count(chip);
}

/**
 * pch_set_station_address() - This API sets the station address used by
 *				    IEEE 1588 hardware when looking at PTP
 *				    traffic on the  ethernet interface
 * @addr:	dress which contain the column separated address to be used.
 * @pdev:	PCI device.
 */
int pch_set_station_address(u8 *addr, struct pci_dev *pdev)
{
	struct pch_dev *chip = pci_get_drvdata(pdev);
	bool valid;
	u64 mac;

	/* Verify the parameter */
	if ((chip->regs == NULL) || addr == (u8 *)NULL) {
		dev_err(&pdev->dev,
			"invalid params returning PCH_INVALIDPARAM\n");
		return PCH_INVALIDPARAM;
	}

	valid = mac_pton(addr, (u8 *)&mac);
	if (!valid) {
		dev_err(&pdev->dev, "invalid params returning PCH_INVALIDPARAM\n");
		return PCH_INVALIDPARAM;
	}

	dev_dbg(&pdev->dev, "invoking pch_station_set\n");
	iowrite64_lo_hi(mac, &chip->regs->ts_st);
	return 0;
}
EXPORT_SYMBOL(pch_set_station_address);

/*
 * Interrupt service routine
 */
static irqreturn_t isr(int irq, void *priv)
{
	struct pch_dev *pch_dev = priv;
	struct pch_ts_regs __iomem *regs = pch_dev->regs;
	struct ptp_clock_event event;
	u32 ack = 0, val;

	val = ioread32(&regs->event);

	if (val & PCH_TSE_SNS) {
		ack |= PCH_TSE_SNS;
		if (pch_dev->exts0_enabled) {
			event.type = PTP_CLOCK_EXTTS;
			event.index = 0;
			event.timestamp = ioread64_hi_lo(&regs->asms_hi);
			event.timestamp <<= TICKS_NS_SHIFT;
			ptp_clock_event(pch_dev->ptp_clock, &event);
		}
	}

	if (val & PCH_TSE_SNM) {
		ack |= PCH_TSE_SNM;
		if (pch_dev->exts1_enabled) {
			event.type = PTP_CLOCK_EXTTS;
			event.index = 1;
			event.timestamp = ioread64_hi_lo(&regs->asms_hi);
			event.timestamp <<= TICKS_NS_SHIFT;
			ptp_clock_event(pch_dev->ptp_clock, &event);
		}
	}

	if (val & PCH_TSE_TTIPEND)
		ack |= PCH_TSE_TTIPEND; /* this bit seems to be always set */

	if (ack) {
		iowrite32(ack, &regs->event);
		return IRQ_HANDLED;
	} else
		return IRQ_NONE;
}

/*
 * PTP clock operations
 */

static int ptp_pch_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	u32 addend;
	struct pch_dev *pch_dev = container_of(ptp, struct pch_dev, caps);
	struct pch_ts_regs __iomem *regs = pch_dev->regs;

	addend = adjust_by_scaled_ppm(DEFAULT_ADDEND, scaled_ppm);

	iowrite32(addend, &regs->addend);

	return 0;
}

static int ptp_pch_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	s64 now;
	unsigned long flags;
	struct pch_dev *pch_dev = container_of(ptp, struct pch_dev, caps);
	struct pch_ts_regs __iomem *regs = pch_dev->regs;

	spin_lock_irqsave(&pch_dev->register_lock, flags);
	now = pch_systime_read(regs);
	now += delta;
	pch_systime_write(regs, now);
	spin_unlock_irqrestore(&pch_dev->register_lock, flags);

	return 0;
}

static int ptp_pch_gettime(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	u64 ns;
	unsigned long flags;
	struct pch_dev *pch_dev = container_of(ptp, struct pch_dev, caps);
	struct pch_ts_regs __iomem *regs = pch_dev->regs;

	spin_lock_irqsave(&pch_dev->register_lock, flags);
	ns = pch_systime_read(regs);
	spin_unlock_irqrestore(&pch_dev->register_lock, flags);

	*ts = ns_to_timespec64(ns);
	return 0;
}

static int ptp_pch_settime(struct ptp_clock_info *ptp,
			   const struct timespec64 *ts)
{
	u64 ns;
	unsigned long flags;
	struct pch_dev *pch_dev = container_of(ptp, struct pch_dev, caps);
	struct pch_ts_regs __iomem *regs = pch_dev->regs;

	ns = timespec64_to_ns(ts);

	spin_lock_irqsave(&pch_dev->register_lock, flags);
	pch_systime_write(regs, ns);
	spin_unlock_irqrestore(&pch_dev->register_lock, flags);

	return 0;
}

static int ptp_pch_enable(struct ptp_clock_info *ptp,
			  struct ptp_clock_request *rq, int on)
{
	struct pch_dev *pch_dev = container_of(ptp, struct pch_dev, caps);

	switch (rq->type) {
	case PTP_CLK_REQ_EXTTS:
		switch (rq->extts.index) {
		case 0:
			pch_dev->exts0_enabled = on ? 1 : 0;
			break;
		case 1:
			pch_dev->exts1_enabled = on ? 1 : 0;
			break;
		default:
			return -EINVAL;
		}
		return 0;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static const struct ptp_clock_info ptp_pch_caps = {
	.owner		= THIS_MODULE,
	.name		= "PCH timer",
	.max_adj	= 50000000,
	.n_ext_ts	= N_EXT_TS,
	.n_pins		= 0,
	.pps		= 0,
	.adjfine	= ptp_pch_adjfine,
	.adjtime	= ptp_pch_adjtime,
	.gettime64	= ptp_pch_gettime,
	.settime64	= ptp_pch_settime,
	.enable		= ptp_pch_enable,
};

static void pch_remove(struct pci_dev *pdev)
{
	struct pch_dev *chip = pci_get_drvdata(pdev);

	free_irq(pdev->irq, chip);
	ptp_clock_unregister(chip->ptp_clock);
}

static s32
pch_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	s32 ret;
	unsigned long flags;
	struct pch_dev *chip;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (chip == NULL)
		return -ENOMEM;

	/* enable the 1588 pci device */
	ret = pcim_enable_device(pdev);
	if (ret != 0) {
		dev_err(&pdev->dev, "could not enable the pci device\n");
		return ret;
	}

	ret = pcim_iomap_regions(pdev, BIT(IO_MEM_BAR), "1588_regs");
	if (ret) {
		dev_err(&pdev->dev, "could not locate IO memory address\n");
		return ret;
	}

	/* get the virtual address to the 1588 registers */
	chip->regs = pcim_iomap_table(pdev)[IO_MEM_BAR];
	chip->caps = ptp_pch_caps;
	chip->ptp_clock = ptp_clock_register(&chip->caps, &pdev->dev);
	if (IS_ERR(chip->ptp_clock))
		return PTR_ERR(chip->ptp_clock);

	spin_lock_init(&chip->register_lock);

	ret = request_irq(pdev->irq, &isr, IRQF_SHARED, KBUILD_MODNAME, chip);
	if (ret != 0) {
		dev_err(&pdev->dev, "failed to get irq %d\n", pdev->irq);
		goto err_req_irq;
	}

	/* indicate success */
	chip->irq = pdev->irq;
	chip->pdev = pdev;
	pci_set_drvdata(pdev, chip);

	spin_lock_irqsave(&chip->register_lock, flags);
	/* reset the ieee1588 h/w */
	pch_reset(chip);

	iowrite32(DEFAULT_ADDEND, &chip->regs->addend);
	iowrite64_lo_hi(1, &chip->regs->trgt_lo);
	iowrite32(PCH_TSE_TTIPEND, &chip->regs->event);

	pch_eth_enable_set(chip);

	if (strcmp(pch_param.station, "00:00:00:00:00:00") != 0) {
		if (pch_set_station_address(pch_param.station, pdev) != 0) {
			dev_err(&pdev->dev,
			"Invalid station address parameter\n"
			"Module loaded but station address not set correctly\n"
			);
		}
	}
	spin_unlock_irqrestore(&chip->register_lock, flags);
	return 0;

err_req_irq:
	ptp_clock_unregister(chip->ptp_clock);

	dev_err(&pdev->dev, "probe failed(ret=0x%x)\n", ret);

	return ret;
}

static const struct pci_device_id pch_ieee1588_pcidev_id[] = {
	{
	  .vendor = PCI_VENDOR_ID_INTEL,
	  .device = PCI_DEVICE_ID_PCH_1588
	 },
	{0}
};
MODULE_DEVICE_TABLE(pci, pch_ieee1588_pcidev_id);

static struct pci_driver pch_driver = {
	.name = KBUILD_MODNAME,
	.id_table = pch_ieee1588_pcidev_id,
	.probe = pch_probe,
	.remove = pch_remove,
};
module_pci_driver(pch_driver);

module_param_string(station,
		    pch_param.station, sizeof(pch_param.station), 0444);
MODULE_PARM_DESC(station,
	 "IEEE 1588 station address to use - colon separated hex values");

MODULE_AUTHOR("LAPIS SEMICONDUCTOR, <tshimizu818@gmail.com>");
MODULE_DESCRIPTION("PTP clock using the EG20T timer");
MODULE_LICENSE("GPL");
