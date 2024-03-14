// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DMA driver for Xilinx DMA/Bridge Subsystem
 *
 * Copyright (C) 2017-2020 Xilinx, Inc. All rights reserved.
 * Copyright (C) 2022, Advanced Micro Devices, Inc.
 */

/*
 * The DMA/Bridge Subsystem for PCI Express allows for the movement of data
 * between Host memory and the DMA subsystem. It does this by operating on
 * 'descriptors' that contain information about the source, destination and
 * amount of data to transfer. These direct memory transfers can be both in
 * the Host to Card (H2C) and Card to Host (C2H) transfers. The DMA can be
 * configured to have a single AXI4 Master interface shared by all channels
 * or one AXI4-Stream interface for each channel enabled. Memory transfers are
 * specified on a per-channel basis in descriptor linked lists, which the DMA
 * fetches from host memory and processes. Events such as descriptor completion
 * and errors are signaled using interrupts. The core also provides up to 16
 * user interrupt wires that generate interrupts to the host.
 */

#include <linux/mod_devicetable.h>
#include <linux/bitfield.h>
#include <linux/dmapool.h>
#include <linux/regmap.h>
#include <linux/dmaengine.h>
#include <linux/dma/amd_xdma.h>
#include <linux/platform_device.h>
#include <linux/platform_data/amd_xdma.h>
#include <linux/dma-mapping.h>
#include <linux/pci.h>
#include "../virt-dma.h"
#include "xdma-regs.h"

/* mmio regmap config for all XDMA registers */
static const struct regmap_config xdma_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = XDMA_REG_SPACE_LEN,
};

/**
 * struct xdma_desc_block - Descriptor block
 * @virt_addr: Virtual address of block start
 * @dma_addr: DMA address of block start
 */
struct xdma_desc_block {
	void		*virt_addr;
	dma_addr_t	dma_addr;
};

/**
 * struct xdma_chan - Driver specific DMA channel structure
 * @vchan: Virtual channel
 * @xdev_hdl: Pointer to DMA device structure
 * @base: Offset of channel registers
 * @desc_pool: Descriptor pool
 * @busy: Busy flag of the channel
 * @dir: Transferring direction of the channel
 * @cfg: Transferring config of the channel
 * @irq: IRQ assigned to the channel
 */
struct xdma_chan {
	struct virt_dma_chan		vchan;
	void				*xdev_hdl;
	u32				base;
	struct dma_pool			*desc_pool;
	bool				busy;
	enum dma_transfer_direction	dir;
	struct dma_slave_config		cfg;
	u32				irq;
};

/**
 * struct xdma_desc - DMA desc structure
 * @vdesc: Virtual DMA descriptor
 * @chan: DMA channel pointer
 * @dir: Transferring direction of the request
 * @dev_addr: Physical address on DMA device side
 * @desc_blocks: Hardware descriptor blocks
 * @dblk_num: Number of hardware descriptor blocks
 * @desc_num: Number of hardware descriptors
 * @completed_desc_num: Completed hardware descriptors
 */
struct xdma_desc {
	struct virt_dma_desc		vdesc;
	struct xdma_chan		*chan;
	enum dma_transfer_direction	dir;
	u64				dev_addr;
	struct xdma_desc_block		*desc_blocks;
	u32				dblk_num;
	u32				desc_num;
	u32				completed_desc_num;
};

#define XDMA_DEV_STATUS_REG_DMA		BIT(0)
#define XDMA_DEV_STATUS_INIT_MSIX	BIT(1)

/**
 * struct xdma_device - DMA device structure
 * @pdev: Platform device pointer
 * @dma_dev: DMA device structure
 * @rmap: MMIO regmap for DMA registers
 * @h2c_chans: Host to Card channels
 * @c2h_chans: Card to Host channels
 * @h2c_chan_num: Number of H2C channels
 * @c2h_chan_num: Number of C2H channels
 * @irq_start: Start IRQ assigned to device
 * @irq_num: Number of IRQ assigned to device
 * @status: Initialization status
 */
struct xdma_device {
	struct platform_device	*pdev;
	struct dma_device	dma_dev;
	struct regmap		*rmap;
	struct xdma_chan	*h2c_chans;
	struct xdma_chan	*c2h_chans;
	u32			h2c_chan_num;
	u32			c2h_chan_num;
	u32			irq_start;
	u32			irq_num;
	u32			status;
};

#define xdma_err(xdev, fmt, args...)					\
	dev_err(&(xdev)->pdev->dev, fmt, ##args)
#define XDMA_CHAN_NUM(_xd) ({						\
	typeof(_xd) (xd) = (_xd);					\
	((xd)->h2c_chan_num + (xd)->c2h_chan_num); })

/* Get the last desc in a desc block */
static inline void *xdma_blk_last_desc(struct xdma_desc_block *block)
{
	return block->virt_addr + (XDMA_DESC_ADJACENT - 1) * XDMA_DESC_SIZE;
}

/**
 * xdma_link_desc_blocks - Link descriptor blocks for DMA transfer
 * @sw_desc: Tx descriptor pointer
 */
static void xdma_link_desc_blocks(struct xdma_desc *sw_desc)
{
	struct xdma_desc_block *block;
	u32 last_blk_desc, desc_control;
	struct xdma_hw_desc *desc;
	int i;

	desc_control = XDMA_DESC_CONTROL(XDMA_DESC_ADJACENT, 0);
	for (i = 1; i < sw_desc->dblk_num; i++) {
		block = &sw_desc->desc_blocks[i - 1];
		desc = xdma_blk_last_desc(block);

		if (!(i & XDMA_DESC_BLOCK_MASK)) {
			desc->control = cpu_to_le32(XDMA_DESC_CONTROL_LAST);
			continue;
		}
		desc->control = cpu_to_le32(desc_control);
		desc->next_desc = cpu_to_le64(block[1].dma_addr);
	}

	/* update the last block */
	last_blk_desc = (sw_desc->desc_num - 1) & XDMA_DESC_ADJACENT_MASK;
	if (((sw_desc->dblk_num - 1) & XDMA_DESC_BLOCK_MASK) > 0) {
		block = &sw_desc->desc_blocks[sw_desc->dblk_num - 2];
		desc = xdma_blk_last_desc(block);
		desc_control = XDMA_DESC_CONTROL(last_blk_desc + 1, 0);
		desc->control = cpu_to_le32(desc_control);
	}

	block = &sw_desc->desc_blocks[sw_desc->dblk_num - 1];
	desc = block->virt_addr + last_blk_desc * XDMA_DESC_SIZE;
	desc->control = cpu_to_le32(XDMA_DESC_CONTROL_LAST);
}

static inline struct xdma_chan *to_xdma_chan(struct dma_chan *chan)
{
	return container_of(chan, struct xdma_chan, vchan.chan);
}

static inline struct xdma_desc *to_xdma_desc(struct virt_dma_desc *vdesc)
{
	return container_of(vdesc, struct xdma_desc, vdesc);
}

/**
 * xdma_channel_init - Initialize DMA channel registers
 * @chan: DMA channel pointer
 */
static int xdma_channel_init(struct xdma_chan *chan)
{
	struct xdma_device *xdev = chan->xdev_hdl;
	int ret;

	ret = regmap_write(xdev->rmap, chan->base + XDMA_CHAN_CONTROL_W1C,
			   CHAN_CTRL_NON_INCR_ADDR);
	if (ret)
		return ret;

	ret = regmap_write(xdev->rmap, chan->base + XDMA_CHAN_INTR_ENABLE,
			   CHAN_IM_ALL);
	if (ret)
		return ret;

	return 0;
}

/**
 * xdma_free_desc - Free descriptor
 * @vdesc: Virtual DMA descriptor
 */
static void xdma_free_desc(struct virt_dma_desc *vdesc)
{
	struct xdma_desc *sw_desc;
	int i;

	sw_desc = to_xdma_desc(vdesc);
	for (i = 0; i < sw_desc->dblk_num; i++) {
		if (!sw_desc->desc_blocks[i].virt_addr)
			break;
		dma_pool_free(sw_desc->chan->desc_pool,
			      sw_desc->desc_blocks[i].virt_addr,
			      sw_desc->desc_blocks[i].dma_addr);
	}
	kfree(sw_desc->desc_blocks);
	kfree(sw_desc);
}

/**
 * xdma_alloc_desc - Allocate descriptor
 * @chan: DMA channel pointer
 * @desc_num: Number of hardware descriptors
 */
static struct xdma_desc *
xdma_alloc_desc(struct xdma_chan *chan, u32 desc_num)
{
	struct xdma_desc *sw_desc;
	struct xdma_hw_desc *desc;
	dma_addr_t dma_addr;
	u32 dblk_num;
	void *addr;
	int i, j;

	sw_desc = kzalloc(sizeof(*sw_desc), GFP_NOWAIT);
	if (!sw_desc)
		return NULL;

	sw_desc->chan = chan;
	sw_desc->desc_num = desc_num;
	dblk_num = DIV_ROUND_UP(desc_num, XDMA_DESC_ADJACENT);
	sw_desc->desc_blocks = kcalloc(dblk_num, sizeof(*sw_desc->desc_blocks),
				       GFP_NOWAIT);
	if (!sw_desc->desc_blocks)
		goto failed;

	sw_desc->dblk_num = dblk_num;
	for (i = 0; i < sw_desc->dblk_num; i++) {
		addr = dma_pool_alloc(chan->desc_pool, GFP_NOWAIT, &dma_addr);
		if (!addr)
			goto failed;

		sw_desc->desc_blocks[i].virt_addr = addr;
		sw_desc->desc_blocks[i].dma_addr = dma_addr;
		for (j = 0, desc = addr; j < XDMA_DESC_ADJACENT; j++)
			desc[j].control = cpu_to_le32(XDMA_DESC_CONTROL(1, 0));
	}

	xdma_link_desc_blocks(sw_desc);

	return sw_desc;

failed:
	xdma_free_desc(&sw_desc->vdesc);
	return NULL;
}

/**
 * xdma_xfer_start - Start DMA transfer
 * @xchan: DMA channel pointer
 */
static int xdma_xfer_start(struct xdma_chan *xchan)
{
	struct virt_dma_desc *vd = vchan_next_desc(&xchan->vchan);
	struct xdma_device *xdev = xchan->xdev_hdl;
	struct xdma_desc_block *block;
	u32 val, completed_blocks;
	struct xdma_desc *desc;
	int ret;

	/*
	 * check if there is not any submitted descriptor or channel is busy.
	 * vchan lock should be held where this function is called.
	 */
	if (!vd || xchan->busy)
		return -EINVAL;

	/* clear run stop bit to get ready for transfer */
	ret = regmap_write(xdev->rmap, xchan->base + XDMA_CHAN_CONTROL_W1C,
			   CHAN_CTRL_RUN_STOP);
	if (ret)
		return ret;

	desc = to_xdma_desc(vd);
	if (desc->dir != xchan->dir) {
		xdma_err(xdev, "incorrect request direction");
		return -EINVAL;
	}

	/* set DMA engine to the first descriptor block */
	completed_blocks = desc->completed_desc_num / XDMA_DESC_ADJACENT;
	block = &desc->desc_blocks[completed_blocks];
	val = lower_32_bits(block->dma_addr);
	ret = regmap_write(xdev->rmap, xchan->base + XDMA_SGDMA_DESC_LO, val);
	if (ret)
		return ret;

	val = upper_32_bits(block->dma_addr);
	ret = regmap_write(xdev->rmap, xchan->base + XDMA_SGDMA_DESC_HI, val);
	if (ret)
		return ret;

	if (completed_blocks + 1 == desc->dblk_num)
		val = (desc->desc_num - 1) & XDMA_DESC_ADJACENT_MASK;
	else
		val = XDMA_DESC_ADJACENT - 1;
	ret = regmap_write(xdev->rmap, xchan->base + XDMA_SGDMA_DESC_ADJ, val);
	if (ret)
		return ret;

	/* kick off DMA transfer */
	ret = regmap_write(xdev->rmap, xchan->base + XDMA_CHAN_CONTROL,
			   CHAN_CTRL_START);
	if (ret)
		return ret;

	xchan->busy = true;
	return 0;
}

/**
 * xdma_alloc_channels - Detect and allocate DMA channels
 * @xdev: DMA device pointer
 * @dir: Channel direction
 */
static int xdma_alloc_channels(struct xdma_device *xdev,
			       enum dma_transfer_direction dir)
{
	struct xdma_platdata *pdata = dev_get_platdata(&xdev->pdev->dev);
	struct xdma_chan **chans, *xchan;
	u32 base, identifier, target;
	u32 *chan_num;
	int i, j, ret;

	if (dir == DMA_MEM_TO_DEV) {
		base = XDMA_CHAN_H2C_OFFSET;
		target = XDMA_CHAN_H2C_TARGET;
		chans = &xdev->h2c_chans;
		chan_num = &xdev->h2c_chan_num;
	} else if (dir == DMA_DEV_TO_MEM) {
		base = XDMA_CHAN_C2H_OFFSET;
		target = XDMA_CHAN_C2H_TARGET;
		chans = &xdev->c2h_chans;
		chan_num = &xdev->c2h_chan_num;
	} else {
		xdma_err(xdev, "invalid direction specified");
		return -EINVAL;
	}

	/* detect number of available DMA channels */
	for (i = 0, *chan_num = 0; i < pdata->max_dma_channels; i++) {
		ret = regmap_read(xdev->rmap, base + i * XDMA_CHAN_STRIDE,
				  &identifier);
		if (ret)
			return ret;

		/* check if it is available DMA channel */
		if (XDMA_CHAN_CHECK_TARGET(identifier, target))
			(*chan_num)++;
	}

	if (!*chan_num) {
		xdma_err(xdev, "does not probe any channel");
		return -EINVAL;
	}

	*chans = devm_kcalloc(&xdev->pdev->dev, *chan_num, sizeof(**chans),
			      GFP_KERNEL);
	if (!*chans)
		return -ENOMEM;

	for (i = 0, j = 0; i < pdata->max_dma_channels; i++) {
		ret = regmap_read(xdev->rmap, base + i * XDMA_CHAN_STRIDE,
				  &identifier);
		if (ret)
			return ret;

		if (!XDMA_CHAN_CHECK_TARGET(identifier, target))
			continue;

		if (j == *chan_num) {
			xdma_err(xdev, "invalid channel number");
			return -EIO;
		}

		/* init channel structure and hardware */
		xchan = &(*chans)[j];
		xchan->xdev_hdl = xdev;
		xchan->base = base + i * XDMA_CHAN_STRIDE;
		xchan->dir = dir;

		ret = xdma_channel_init(xchan);
		if (ret)
			return ret;
		xchan->vchan.desc_free = xdma_free_desc;
		vchan_init(&xchan->vchan, &xdev->dma_dev);

		j++;
	}

	dev_info(&xdev->pdev->dev, "configured %d %s channels", j,
		 (dir == DMA_MEM_TO_DEV) ? "H2C" : "C2H");

	return 0;
}

/**
 * xdma_issue_pending - Issue pending transactions
 * @chan: DMA channel pointer
 */
static void xdma_issue_pending(struct dma_chan *chan)
{
	struct xdma_chan *xdma_chan = to_xdma_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&xdma_chan->vchan.lock, flags);
	if (vchan_issue_pending(&xdma_chan->vchan))
		xdma_xfer_start(xdma_chan);
	spin_unlock_irqrestore(&xdma_chan->vchan.lock, flags);
}

/**
 * xdma_prep_device_sg - prepare a descriptor for a DMA transaction
 * @chan: DMA channel pointer
 * @sgl: Transfer scatter gather list
 * @sg_len: Length of scatter gather list
 * @dir: Transfer direction
 * @flags: transfer ack flags
 * @context: APP words of the descriptor
 */
static struct dma_async_tx_descriptor *
xdma_prep_device_sg(struct dma_chan *chan, struct scatterlist *sgl,
		    unsigned int sg_len, enum dma_transfer_direction dir,
		    unsigned long flags, void *context)
{
	struct xdma_chan *xdma_chan = to_xdma_chan(chan);
	struct dma_async_tx_descriptor *tx_desc;
	u32 desc_num = 0, i, len, rest;
	struct xdma_desc_block *dblk;
	struct xdma_hw_desc *desc;
	struct xdma_desc *sw_desc;
	u64 dev_addr, *src, *dst;
	struct scatterlist *sg;
	u64 addr;

	for_each_sg(sgl, sg, sg_len, i)
		desc_num += DIV_ROUND_UP(sg_dma_len(sg), XDMA_DESC_BLEN_MAX);

	sw_desc = xdma_alloc_desc(xdma_chan, desc_num);
	if (!sw_desc)
		return NULL;
	sw_desc->dir = dir;

	if (dir == DMA_MEM_TO_DEV) {
		dev_addr = xdma_chan->cfg.dst_addr;
		src = &addr;
		dst = &dev_addr;
	} else {
		dev_addr = xdma_chan->cfg.src_addr;
		src = &dev_addr;
		dst = &addr;
	}

	dblk = sw_desc->desc_blocks;
	desc = dblk->virt_addr;
	desc_num = 1;
	for_each_sg(sgl, sg, sg_len, i) {
		addr = sg_dma_address(sg);
		rest = sg_dma_len(sg);

		do {
			len = min_t(u32, rest, XDMA_DESC_BLEN_MAX);
			/* set hardware descriptor */
			desc->bytes = cpu_to_le32(len);
			desc->src_addr = cpu_to_le64(*src);
			desc->dst_addr = cpu_to_le64(*dst);

			if (!(desc_num & XDMA_DESC_ADJACENT_MASK)) {
				dblk++;
				desc = dblk->virt_addr;
			} else {
				desc++;
			}

			desc_num++;
			dev_addr += len;
			addr += len;
			rest -= len;
		} while (rest);
	}

	tx_desc = vchan_tx_prep(&xdma_chan->vchan, &sw_desc->vdesc, flags);
	if (!tx_desc)
		goto failed;

	return tx_desc;

failed:
	xdma_free_desc(&sw_desc->vdesc);

	return NULL;
}

/**
 * xdma_device_config - Configure the DMA channel
 * @chan: DMA channel
 * @cfg: channel configuration
 */
static int xdma_device_config(struct dma_chan *chan,
			      struct dma_slave_config *cfg)
{
	struct xdma_chan *xdma_chan = to_xdma_chan(chan);

	memcpy(&xdma_chan->cfg, cfg, sizeof(*cfg));

	return 0;
}

/**
 * xdma_free_chan_resources - Free channel resources
 * @chan: DMA channel
 */
static void xdma_free_chan_resources(struct dma_chan *chan)
{
	struct xdma_chan *xdma_chan = to_xdma_chan(chan);

	vchan_free_chan_resources(&xdma_chan->vchan);
	dma_pool_destroy(xdma_chan->desc_pool);
	xdma_chan->desc_pool = NULL;
}

/**
 * xdma_alloc_chan_resources - Allocate channel resources
 * @chan: DMA channel
 */
static int xdma_alloc_chan_resources(struct dma_chan *chan)
{
	struct xdma_chan *xdma_chan = to_xdma_chan(chan);
	struct xdma_device *xdev = xdma_chan->xdev_hdl;
	struct device *dev = xdev->dma_dev.dev;

	while (dev && !dev_is_pci(dev))
		dev = dev->parent;
	if (!dev) {
		xdma_err(xdev, "unable to find pci device");
		return -EINVAL;
	}

	xdma_chan->desc_pool = dma_pool_create(dma_chan_name(chan),
					       dev, XDMA_DESC_BLOCK_SIZE,
					       XDMA_DESC_BLOCK_ALIGN, 0);
	if (!xdma_chan->desc_pool) {
		xdma_err(xdev, "unable to allocate descriptor pool");
		return -ENOMEM;
	}

	return 0;
}

/**
 * xdma_channel_isr - XDMA channel interrupt handler
 * @irq: IRQ number
 * @dev_id: Pointer to the DMA channel structure
 */
static irqreturn_t xdma_channel_isr(int irq, void *dev_id)
{
	struct xdma_chan *xchan = dev_id;
	u32 complete_desc_num = 0;
	struct xdma_device *xdev;
	struct virt_dma_desc *vd;
	struct xdma_desc *desc;
	int ret;

	spin_lock(&xchan->vchan.lock);

	/* get submitted request */
	vd = vchan_next_desc(&xchan->vchan);
	if (!vd)
		goto out;

	xchan->busy = false;
	desc = to_xdma_desc(vd);
	xdev = xchan->xdev_hdl;

	ret = regmap_read(xdev->rmap, xchan->base + XDMA_CHAN_COMPLETED_DESC,
			  &complete_desc_num);
	if (ret)
		goto out;

	desc->completed_desc_num += complete_desc_num;
	/*
	 * if all data blocks are transferred, remove and complete the request
	 */
	if (desc->completed_desc_num == desc->desc_num) {
		list_del(&vd->node);
		vchan_cookie_complete(vd);
		goto out;
	}

	if (desc->completed_desc_num > desc->desc_num ||
	    complete_desc_num != XDMA_DESC_BLOCK_NUM * XDMA_DESC_ADJACENT)
		goto out;

	/* transfer the rest of data */
	xdma_xfer_start(xchan);

out:
	spin_unlock(&xchan->vchan.lock);
	return IRQ_HANDLED;
}

/**
 * xdma_irq_fini - Uninitialize IRQ
 * @xdev: DMA device pointer
 */
static void xdma_irq_fini(struct xdma_device *xdev)
{
	int i;

	/* disable interrupt */
	regmap_write(xdev->rmap, XDMA_IRQ_CHAN_INT_EN_W1C, ~0);

	/* free irq handler */
	for (i = 0; i < xdev->h2c_chan_num; i++)
		free_irq(xdev->h2c_chans[i].irq, &xdev->h2c_chans[i]);

	for (i = 0; i < xdev->c2h_chan_num; i++)
		free_irq(xdev->c2h_chans[i].irq, &xdev->c2h_chans[i]);
}

/**
 * xdma_set_vector_reg - configure hardware IRQ registers
 * @xdev: DMA device pointer
 * @vec_tbl_start: Start of IRQ registers
 * @irq_start: Start of IRQ
 * @irq_num: Number of IRQ
 */
static int xdma_set_vector_reg(struct xdma_device *xdev, u32 vec_tbl_start,
			       u32 irq_start, u32 irq_num)
{
	u32 shift, i, val = 0;
	int ret;

	/* Each IRQ register is 32 bit and contains 4 IRQs */
	while (irq_num > 0) {
		for (i = 0; i < 4; i++) {
			shift = XDMA_IRQ_VEC_SHIFT * i;
			val |= irq_start << shift;
			irq_start++;
			irq_num--;
			if (!irq_num)
				break;
		}

		/* write IRQ register */
		ret = regmap_write(xdev->rmap, vec_tbl_start, val);
		if (ret)
			return ret;
		vec_tbl_start += sizeof(u32);
		val = 0;
	}

	return 0;
}

/**
 * xdma_irq_init - initialize IRQs
 * @xdev: DMA device pointer
 */
static int xdma_irq_init(struct xdma_device *xdev)
{
	u32 irq = xdev->irq_start;
	u32 user_irq_start;
	int i, j, ret;

	/* return failure if there are not enough IRQs */
	if (xdev->irq_num < XDMA_CHAN_NUM(xdev)) {
		xdma_err(xdev, "not enough irq");
		return -EINVAL;
	}

	/* setup H2C interrupt handler */
	for (i = 0; i < xdev->h2c_chan_num; i++) {
		ret = request_irq(irq, xdma_channel_isr, 0,
				  "xdma-h2c-channel", &xdev->h2c_chans[i]);
		if (ret) {
			xdma_err(xdev, "H2C channel%d request irq%d failed: %d",
				 i, irq, ret);
			goto failed_init_h2c;
		}
		xdev->h2c_chans[i].irq = irq;
		irq++;
	}

	/* setup C2H interrupt handler */
	for (j = 0; j < xdev->c2h_chan_num; j++) {
		ret = request_irq(irq, xdma_channel_isr, 0,
				  "xdma-c2h-channel", &xdev->c2h_chans[j]);
		if (ret) {
			xdma_err(xdev, "C2H channel%d request irq%d failed: %d",
				 j, irq, ret);
			goto failed_init_c2h;
		}
		xdev->c2h_chans[j].irq = irq;
		irq++;
	}

	/* config hardware IRQ registers */
	ret = xdma_set_vector_reg(xdev, XDMA_IRQ_CHAN_VEC_NUM, 0,
				  XDMA_CHAN_NUM(xdev));
	if (ret) {
		xdma_err(xdev, "failed to set channel vectors: %d", ret);
		goto failed_init_c2h;
	}

	/* config user IRQ registers if needed */
	user_irq_start = XDMA_CHAN_NUM(xdev);
	if (xdev->irq_num > user_irq_start) {
		ret = xdma_set_vector_reg(xdev, XDMA_IRQ_USER_VEC_NUM,
					  user_irq_start,
					  xdev->irq_num - user_irq_start);
		if (ret) {
			xdma_err(xdev, "failed to set user vectors: %d", ret);
			goto failed_init_c2h;
		}
	}

	/* enable interrupt */
	ret = regmap_write(xdev->rmap, XDMA_IRQ_CHAN_INT_EN_W1S, ~0);
	if (ret)
		goto failed_init_c2h;

	return 0;

failed_init_c2h:
	while (j--)
		free_irq(xdev->c2h_chans[j].irq, &xdev->c2h_chans[j]);
failed_init_h2c:
	while (i--)
		free_irq(xdev->h2c_chans[i].irq, &xdev->h2c_chans[i]);

	return ret;
}

static bool xdma_filter_fn(struct dma_chan *chan, void *param)
{
	struct xdma_chan *xdma_chan = to_xdma_chan(chan);
	struct xdma_chan_info *chan_info = param;

	return chan_info->dir == xdma_chan->dir;
}

/**
 * xdma_disable_user_irq - Disable user interrupt
 * @pdev: Pointer to the platform_device structure
 * @irq_num: System IRQ number
 */
void xdma_disable_user_irq(struct platform_device *pdev, u32 irq_num)
{
	struct xdma_device *xdev = platform_get_drvdata(pdev);
	u32 index;

	index = irq_num - xdev->irq_start;
	if (index < XDMA_CHAN_NUM(xdev) || index >= xdev->irq_num) {
		xdma_err(xdev, "invalid user irq number");
		return;
	}
	index -= XDMA_CHAN_NUM(xdev);

	regmap_write(xdev->rmap, XDMA_IRQ_USER_INT_EN_W1C, 1 << index);
}
EXPORT_SYMBOL(xdma_disable_user_irq);

/**
 * xdma_enable_user_irq - Enable user logic interrupt
 * @pdev: Pointer to the platform_device structure
 * @irq_num: System IRQ number
 */
int xdma_enable_user_irq(struct platform_device *pdev, u32 irq_num)
{
	struct xdma_device *xdev = platform_get_drvdata(pdev);
	u32 index;
	int ret;

	index = irq_num - xdev->irq_start;
	if (index < XDMA_CHAN_NUM(xdev) || index >= xdev->irq_num) {
		xdma_err(xdev, "invalid user irq number");
		return -EINVAL;
	}
	index -= XDMA_CHAN_NUM(xdev);

	ret = regmap_write(xdev->rmap, XDMA_IRQ_USER_INT_EN_W1S, 1 << index);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL(xdma_enable_user_irq);

/**
 * xdma_get_user_irq - Get system IRQ number
 * @pdev: Pointer to the platform_device structure
 * @user_irq_index: User logic IRQ wire index
 *
 * Return: The system IRQ number allocated for the given wire index.
 */
int xdma_get_user_irq(struct platform_device *pdev, u32 user_irq_index)
{
	struct xdma_device *xdev = platform_get_drvdata(pdev);

	if (XDMA_CHAN_NUM(xdev) + user_irq_index >= xdev->irq_num) {
		xdma_err(xdev, "invalid user irq index");
		return -EINVAL;
	}

	return xdev->irq_start + XDMA_CHAN_NUM(xdev) + user_irq_index;
}
EXPORT_SYMBOL(xdma_get_user_irq);

/**
 * xdma_remove - Driver remove function
 * @pdev: Pointer to the platform_device structure
 */
static int xdma_remove(struct platform_device *pdev)
{
	struct xdma_device *xdev = platform_get_drvdata(pdev);

	if (xdev->status & XDMA_DEV_STATUS_INIT_MSIX)
		xdma_irq_fini(xdev);

	if (xdev->status & XDMA_DEV_STATUS_REG_DMA)
		dma_async_device_unregister(&xdev->dma_dev);

	return 0;
}

/**
 * xdma_probe - Driver probe function
 * @pdev: Pointer to the platform_device structure
 */
static int xdma_probe(struct platform_device *pdev)
{
	struct xdma_platdata *pdata = dev_get_platdata(&pdev->dev);
	struct xdma_device *xdev;
	void __iomem *reg_base;
	struct resource *res;
	int ret = -ENODEV;

	if (pdata->max_dma_channels > XDMA_MAX_CHANNELS) {
		dev_err(&pdev->dev, "invalid max dma channels %d",
			pdata->max_dma_channels);
		return -EINVAL;
	}

	xdev = devm_kzalloc(&pdev->dev, sizeof(*xdev), GFP_KERNEL);
	if (!xdev)
		return -ENOMEM;

	platform_set_drvdata(pdev, xdev);
	xdev->pdev = pdev;

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res) {
		xdma_err(xdev, "failed to get irq resource");
		goto failed;
	}
	xdev->irq_start = res->start;
	xdev->irq_num = res->end - res->start + 1;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		xdma_err(xdev, "failed to get io resource");
		goto failed;
	}

	reg_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(reg_base)) {
		xdma_err(xdev, "ioremap failed");
		goto failed;
	}

	xdev->rmap = devm_regmap_init_mmio(&pdev->dev, reg_base,
					   &xdma_regmap_config);
	if (!xdev->rmap) {
		xdma_err(xdev, "config regmap failed: %d", ret);
		goto failed;
	}
	INIT_LIST_HEAD(&xdev->dma_dev.channels);

	ret = xdma_alloc_channels(xdev, DMA_MEM_TO_DEV);
	if (ret) {
		xdma_err(xdev, "config H2C channels failed: %d", ret);
		goto failed;
	}

	ret = xdma_alloc_channels(xdev, DMA_DEV_TO_MEM);
	if (ret) {
		xdma_err(xdev, "config C2H channels failed: %d", ret);
		goto failed;
	}

	dma_cap_set(DMA_SLAVE, xdev->dma_dev.cap_mask);
	dma_cap_set(DMA_PRIVATE, xdev->dma_dev.cap_mask);

	xdev->dma_dev.dev = &pdev->dev;
	xdev->dma_dev.device_free_chan_resources = xdma_free_chan_resources;
	xdev->dma_dev.device_alloc_chan_resources = xdma_alloc_chan_resources;
	xdev->dma_dev.device_tx_status = dma_cookie_status;
	xdev->dma_dev.device_prep_slave_sg = xdma_prep_device_sg;
	xdev->dma_dev.device_config = xdma_device_config;
	xdev->dma_dev.device_issue_pending = xdma_issue_pending;
	xdev->dma_dev.filter.map = pdata->device_map;
	xdev->dma_dev.filter.mapcnt = pdata->device_map_cnt;
	xdev->dma_dev.filter.fn = xdma_filter_fn;

	ret = dma_async_device_register(&xdev->dma_dev);
	if (ret) {
		xdma_err(xdev, "failed to register Xilinx XDMA: %d", ret);
		goto failed;
	}
	xdev->status |= XDMA_DEV_STATUS_REG_DMA;

	ret = xdma_irq_init(xdev);
	if (ret) {
		xdma_err(xdev, "failed to init msix: %d", ret);
		goto failed;
	}
	xdev->status |= XDMA_DEV_STATUS_INIT_MSIX;

	return 0;

failed:
	xdma_remove(pdev);

	return ret;
}

static const struct platform_device_id xdma_id_table[] = {
	{ "xdma", 0},
	{ },
};

static struct platform_driver xdma_driver = {
	.driver		= {
		.name = "xdma",
	},
	.id_table	= xdma_id_table,
	.probe		= xdma_probe,
	.remove		= xdma_remove,
};

module_platform_driver(xdma_driver);

MODULE_DESCRIPTION("AMD XDMA driver");
MODULE_AUTHOR("XRT Team <runtimeca39d@amd.com>");
MODULE_LICENSE("GPL");
