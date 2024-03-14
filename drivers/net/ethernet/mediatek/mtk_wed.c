// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2021 Felix Fietkau <nbd@nbd.name> */

#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/skbuff.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/mfd/syscon.h>
#include <linux/debugfs.h>
#include <linux/soc/mediatek/mtk_wed.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>
#include "mtk_eth_soc.h"
#include "mtk_wed_regs.h"
#include "mtk_wed.h"
#include "mtk_ppe.h"
#include "mtk_wed_wo.h"

#define MTK_PCIE_BASE(n)		(0x1a143000 + (n) * 0x2000)

#define MTK_WED_PKT_SIZE		1900
#define MTK_WED_BUF_SIZE		2048
#define MTK_WED_BUF_PER_PAGE		(PAGE_SIZE / 2048)
#define MTK_WED_RX_RING_SIZE		1536

#define MTK_WED_TX_RING_SIZE		2048
#define MTK_WED_WDMA_RING_SIZE		1024
#define MTK_WED_MAX_GROUP_SIZE		0x100
#define MTK_WED_VLD_GROUP_SIZE		0x40
#define MTK_WED_PER_GROUP_PKT		128

#define MTK_WED_FBUF_SIZE		128
#define MTK_WED_MIOD_CNT		16
#define MTK_WED_FB_CMD_CNT		1024
#define MTK_WED_RRO_QUE_CNT		8192
#define MTK_WED_MIOD_ENTRY_CNT		128

static struct mtk_wed_hw *hw_list[2];
static DEFINE_MUTEX(hw_lock);

struct mtk_wed_flow_block_priv {
	struct mtk_wed_hw *hw;
	struct net_device *dev;
};

static void
wed_m32(struct mtk_wed_device *dev, u32 reg, u32 mask, u32 val)
{
	regmap_update_bits(dev->hw->regs, reg, mask | val, val);
}

static void
wed_set(struct mtk_wed_device *dev, u32 reg, u32 mask)
{
	return wed_m32(dev, reg, 0, mask);
}

static void
wed_clr(struct mtk_wed_device *dev, u32 reg, u32 mask)
{
	return wed_m32(dev, reg, mask, 0);
}

static void
wdma_m32(struct mtk_wed_device *dev, u32 reg, u32 mask, u32 val)
{
	wdma_w32(dev, reg, (wdma_r32(dev, reg) & ~mask) | val);
}

static void
wdma_set(struct mtk_wed_device *dev, u32 reg, u32 mask)
{
	wdma_m32(dev, reg, 0, mask);
}

static void
wdma_clr(struct mtk_wed_device *dev, u32 reg, u32 mask)
{
	wdma_m32(dev, reg, mask, 0);
}

static u32
wifi_r32(struct mtk_wed_device *dev, u32 reg)
{
	return readl(dev->wlan.base + reg);
}

static void
wifi_w32(struct mtk_wed_device *dev, u32 reg, u32 val)
{
	writel(val, dev->wlan.base + reg);
}

static u32
mtk_wed_read_reset(struct mtk_wed_device *dev)
{
	return wed_r32(dev, MTK_WED_RESET);
}

static u32
mtk_wdma_read_reset(struct mtk_wed_device *dev)
{
	return wdma_r32(dev, MTK_WDMA_GLO_CFG);
}

static int
mtk_wdma_rx_reset(struct mtk_wed_device *dev)
{
	u32 status, mask = MTK_WDMA_GLO_CFG_RX_DMA_BUSY;
	int i, ret;

	wdma_clr(dev, MTK_WDMA_GLO_CFG, MTK_WDMA_GLO_CFG_RX_DMA_EN);
	ret = readx_poll_timeout(mtk_wdma_read_reset, dev, status,
				 !(status & mask), 0, 10000);
	if (ret)
		dev_err(dev->hw->dev, "rx reset failed\n");

	wdma_w32(dev, MTK_WDMA_RESET_IDX, MTK_WDMA_RESET_IDX_RX);
	wdma_w32(dev, MTK_WDMA_RESET_IDX, 0);

	for (i = 0; i < ARRAY_SIZE(dev->rx_wdma); i++) {
		if (dev->rx_wdma[i].desc)
			continue;

		wdma_w32(dev,
			 MTK_WDMA_RING_RX(i) + MTK_WED_RING_OFS_CPU_IDX, 0);
	}

	return ret;
}

static void
mtk_wdma_tx_reset(struct mtk_wed_device *dev)
{
	u32 status, mask = MTK_WDMA_GLO_CFG_TX_DMA_BUSY;
	int i;

	wdma_clr(dev, MTK_WDMA_GLO_CFG, MTK_WDMA_GLO_CFG_TX_DMA_EN);
	if (readx_poll_timeout(mtk_wdma_read_reset, dev, status,
			       !(status & mask), 0, 10000))
		dev_err(dev->hw->dev, "tx reset failed\n");

	wdma_w32(dev, MTK_WDMA_RESET_IDX, MTK_WDMA_RESET_IDX_TX);
	wdma_w32(dev, MTK_WDMA_RESET_IDX, 0);

	for (i = 0; i < ARRAY_SIZE(dev->tx_wdma); i++)
		wdma_w32(dev,
			 MTK_WDMA_RING_TX(i) + MTK_WED_RING_OFS_CPU_IDX, 0);
}

static void
mtk_wed_reset(struct mtk_wed_device *dev, u32 mask)
{
	u32 status;

	wed_w32(dev, MTK_WED_RESET, mask);
	if (readx_poll_timeout(mtk_wed_read_reset, dev, status,
			       !(status & mask), 0, 1000))
		WARN_ON_ONCE(1);
}

static u32
mtk_wed_wo_read_status(struct mtk_wed_device *dev)
{
	return wed_r32(dev, MTK_WED_SCR0 + 4 * MTK_WED_DUMMY_CR_WO_STATUS);
}

static void
mtk_wed_wo_reset(struct mtk_wed_device *dev)
{
	struct mtk_wed_wo *wo = dev->hw->wed_wo;
	u8 state = MTK_WED_WO_STATE_DISABLE;
	void __iomem *reg;
	u32 val;

	mtk_wdma_tx_reset(dev);
	mtk_wed_reset(dev, MTK_WED_RESET_WED);

	if (mtk_wed_mcu_send_msg(wo, MTK_WED_MODULE_ID_WO,
				 MTK_WED_WO_CMD_CHANGE_STATE, &state,
				 sizeof(state), false))
		return;

	if (readx_poll_timeout(mtk_wed_wo_read_status, dev, val,
			       val == MTK_WED_WOIF_DISABLE_DONE,
			       100, MTK_WOCPU_TIMEOUT))
		dev_err(dev->hw->dev, "failed to disable wed-wo\n");

	reg = ioremap(MTK_WED_WO_CPU_MCUSYS_RESET_ADDR, 4);

	val = readl(reg);
	switch (dev->hw->index) {
	case 0:
		val |= MTK_WED_WO_CPU_WO0_MCUSYS_RESET_MASK;
		writel(val, reg);
		val &= ~MTK_WED_WO_CPU_WO0_MCUSYS_RESET_MASK;
		writel(val, reg);
		break;
	case 1:
		val |= MTK_WED_WO_CPU_WO1_MCUSYS_RESET_MASK;
		writel(val, reg);
		val &= ~MTK_WED_WO_CPU_WO1_MCUSYS_RESET_MASK;
		writel(val, reg);
		break;
	default:
		break;
	}
	iounmap(reg);
}

void mtk_wed_fe_reset(void)
{
	int i;

	mutex_lock(&hw_lock);

	for (i = 0; i < ARRAY_SIZE(hw_list); i++) {
		struct mtk_wed_hw *hw = hw_list[i];
		struct mtk_wed_device *dev;
		int err;

		if (!hw)
			break;

		dev = hw->wed_dev;
		if (!dev || !dev->wlan.reset)
			continue;

		/* reset callback blocks until WLAN reset is completed */
		err = dev->wlan.reset(dev);
		if (err)
			dev_err(dev->dev, "wlan reset failed: %d\n", err);
	}

	mutex_unlock(&hw_lock);
}

void mtk_wed_fe_reset_complete(void)
{
	int i;

	mutex_lock(&hw_lock);

	for (i = 0; i < ARRAY_SIZE(hw_list); i++) {
		struct mtk_wed_hw *hw = hw_list[i];
		struct mtk_wed_device *dev;

		if (!hw)
			break;

		dev = hw->wed_dev;
		if (!dev || !dev->wlan.reset_complete)
			continue;

		dev->wlan.reset_complete(dev);
	}

	mutex_unlock(&hw_lock);
}

static struct mtk_wed_hw *
mtk_wed_assign(struct mtk_wed_device *dev)
{
	struct mtk_wed_hw *hw;
	int i;

	if (dev->wlan.bus_type == MTK_WED_BUS_PCIE) {
		hw = hw_list[pci_domain_nr(dev->wlan.pci_dev->bus)];
		if (!hw)
			return NULL;

		if (!hw->wed_dev)
			goto out;

		if (hw->version == 1)
			return NULL;

		/* MT7986 WED devices do not have any pcie slot restrictions */
	}
	/* MT7986 PCIE or AXI */
	for (i = 0; i < ARRAY_SIZE(hw_list); i++) {
		hw = hw_list[i];
		if (hw && !hw->wed_dev)
			goto out;
	}

	return NULL;

out:
	hw->wed_dev = dev;
	return hw;
}

static int
mtk_wed_tx_buffer_alloc(struct mtk_wed_device *dev)
{
	struct mtk_wdma_desc *desc;
	dma_addr_t desc_phys;
	void **page_list;
	int token = dev->wlan.token_start;
	int ring_size;
	int n_pages;
	int i, page_idx;

	ring_size = dev->wlan.nbuf & ~(MTK_WED_BUF_PER_PAGE - 1);
	n_pages = ring_size / MTK_WED_BUF_PER_PAGE;

	page_list = kcalloc(n_pages, sizeof(*page_list), GFP_KERNEL);
	if (!page_list)
		return -ENOMEM;

	dev->tx_buf_ring.size = ring_size;
	dev->tx_buf_ring.pages = page_list;

	desc = dma_alloc_coherent(dev->hw->dev, ring_size * sizeof(*desc),
				  &desc_phys, GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	dev->tx_buf_ring.desc = desc;
	dev->tx_buf_ring.desc_phys = desc_phys;

	for (i = 0, page_idx = 0; i < ring_size; i += MTK_WED_BUF_PER_PAGE) {
		dma_addr_t page_phys, buf_phys;
		struct page *page;
		void *buf;
		int s;

		page = __dev_alloc_pages(GFP_KERNEL, 0);
		if (!page)
			return -ENOMEM;

		page_phys = dma_map_page(dev->hw->dev, page, 0, PAGE_SIZE,
					 DMA_BIDIRECTIONAL);
		if (dma_mapping_error(dev->hw->dev, page_phys)) {
			__free_page(page);
			return -ENOMEM;
		}

		page_list[page_idx++] = page;
		dma_sync_single_for_cpu(dev->hw->dev, page_phys, PAGE_SIZE,
					DMA_BIDIRECTIONAL);

		buf = page_to_virt(page);
		buf_phys = page_phys;

		for (s = 0; s < MTK_WED_BUF_PER_PAGE; s++) {
			u32 txd_size;
			u32 ctrl;

			txd_size = dev->wlan.init_buf(buf, buf_phys, token++);

			desc->buf0 = cpu_to_le32(buf_phys);
			desc->buf1 = cpu_to_le32(buf_phys + txd_size);

			if (dev->hw->version == 1)
				ctrl = FIELD_PREP(MTK_WDMA_DESC_CTRL_LEN0, txd_size) |
				       FIELD_PREP(MTK_WDMA_DESC_CTRL_LEN1,
						  MTK_WED_BUF_SIZE - txd_size) |
				       MTK_WDMA_DESC_CTRL_LAST_SEG1;
			else
				ctrl = FIELD_PREP(MTK_WDMA_DESC_CTRL_LEN0, txd_size) |
				       FIELD_PREP(MTK_WDMA_DESC_CTRL_LEN1_V2,
						  MTK_WED_BUF_SIZE - txd_size) |
				       MTK_WDMA_DESC_CTRL_LAST_SEG0;
			desc->ctrl = cpu_to_le32(ctrl);
			desc->info = 0;
			desc++;

			buf += MTK_WED_BUF_SIZE;
			buf_phys += MTK_WED_BUF_SIZE;
		}

		dma_sync_single_for_device(dev->hw->dev, page_phys, PAGE_SIZE,
					   DMA_BIDIRECTIONAL);
	}

	return 0;
}

static void
mtk_wed_free_tx_buffer(struct mtk_wed_device *dev)
{
	struct mtk_wdma_desc *desc = dev->tx_buf_ring.desc;
	void **page_list = dev->tx_buf_ring.pages;
	int page_idx;
	int i;

	if (!page_list)
		return;

	if (!desc)
		goto free_pagelist;

	for (i = 0, page_idx = 0; i < dev->tx_buf_ring.size;
	     i += MTK_WED_BUF_PER_PAGE) {
		void *page = page_list[page_idx++];
		dma_addr_t buf_addr;

		if (!page)
			break;

		buf_addr = le32_to_cpu(desc[i].buf0);
		dma_unmap_page(dev->hw->dev, buf_addr, PAGE_SIZE,
			       DMA_BIDIRECTIONAL);
		__free_page(page);
	}

	dma_free_coherent(dev->hw->dev, dev->tx_buf_ring.size * sizeof(*desc),
			  desc, dev->tx_buf_ring.desc_phys);

free_pagelist:
	kfree(page_list);
}

static int
mtk_wed_rx_buffer_alloc(struct mtk_wed_device *dev)
{
	struct mtk_rxbm_desc *desc;
	dma_addr_t desc_phys;

	dev->rx_buf_ring.size = dev->wlan.rx_nbuf;
	desc = dma_alloc_coherent(dev->hw->dev,
				  dev->wlan.rx_nbuf * sizeof(*desc),
				  &desc_phys, GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	dev->rx_buf_ring.desc = desc;
	dev->rx_buf_ring.desc_phys = desc_phys;
	dev->wlan.init_rx_buf(dev, dev->wlan.rx_npkt);

	return 0;
}

static void
mtk_wed_free_rx_buffer(struct mtk_wed_device *dev)
{
	struct mtk_rxbm_desc *desc = dev->rx_buf_ring.desc;

	if (!desc)
		return;

	dev->wlan.release_rx_buf(dev);
	dma_free_coherent(dev->hw->dev, dev->rx_buf_ring.size * sizeof(*desc),
			  desc, dev->rx_buf_ring.desc_phys);
}

static void
mtk_wed_rx_buffer_hw_init(struct mtk_wed_device *dev)
{
	wed_w32(dev, MTK_WED_RX_BM_RX_DMAD,
		FIELD_PREP(MTK_WED_RX_BM_RX_DMAD_SDL0, dev->wlan.rx_size));
	wed_w32(dev, MTK_WED_RX_BM_BASE, dev->rx_buf_ring.desc_phys);
	wed_w32(dev, MTK_WED_RX_BM_INIT_PTR, MTK_WED_RX_BM_INIT_SW_TAIL |
		FIELD_PREP(MTK_WED_RX_BM_SW_TAIL, dev->wlan.rx_npkt));
	wed_w32(dev, MTK_WED_RX_BM_DYN_ALLOC_TH,
		FIELD_PREP(MTK_WED_RX_BM_DYN_ALLOC_TH_H, 0xffff));
	wed_set(dev, MTK_WED_CTRL, MTK_WED_CTRL_WED_RX_BM_EN);
}

static void
mtk_wed_free_ring(struct mtk_wed_device *dev, struct mtk_wed_ring *ring)
{
	if (!ring->desc)
		return;

	dma_free_coherent(dev->hw->dev, ring->size * ring->desc_size,
			  ring->desc, ring->desc_phys);
}

static void
mtk_wed_free_rx_rings(struct mtk_wed_device *dev)
{
	mtk_wed_free_rx_buffer(dev);
	mtk_wed_free_ring(dev, &dev->rro.ring);
}

static void
mtk_wed_free_tx_rings(struct mtk_wed_device *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dev->tx_ring); i++)
		mtk_wed_free_ring(dev, &dev->tx_ring[i]);
	for (i = 0; i < ARRAY_SIZE(dev->rx_wdma); i++)
		mtk_wed_free_ring(dev, &dev->rx_wdma[i]);
}

static void
mtk_wed_set_ext_int(struct mtk_wed_device *dev, bool en)
{
	u32 mask = MTK_WED_EXT_INT_STATUS_ERROR_MASK;

	if (dev->hw->version == 1)
		mask |= MTK_WED_EXT_INT_STATUS_TX_DRV_R_RESP_ERR;
	else
		mask |= MTK_WED_EXT_INT_STATUS_RX_FBUF_LO_TH |
			MTK_WED_EXT_INT_STATUS_RX_FBUF_HI_TH |
			MTK_WED_EXT_INT_STATUS_RX_DRV_COHERENT |
			MTK_WED_EXT_INT_STATUS_TX_DMA_W_RESP_ERR;

	if (!dev->hw->num_flows)
		mask &= ~MTK_WED_EXT_INT_STATUS_TKID_WO_PYLD;

	wed_w32(dev, MTK_WED_EXT_INT_MASK, en ? mask : 0);
	wed_r32(dev, MTK_WED_EXT_INT_MASK);
}

static void
mtk_wed_set_512_support(struct mtk_wed_device *dev, bool enable)
{
	if (enable) {
		wed_w32(dev, MTK_WED_TXDP_CTRL, MTK_WED_TXDP_DW9_OVERWR);
		wed_w32(dev, MTK_WED_TXP_DW1,
			FIELD_PREP(MTK_WED_WPDMA_WRITE_TXP, 0x0103));
	} else {
		wed_w32(dev, MTK_WED_TXP_DW1,
			FIELD_PREP(MTK_WED_WPDMA_WRITE_TXP, 0x0100));
		wed_clr(dev, MTK_WED_TXDP_CTRL, MTK_WED_TXDP_DW9_OVERWR);
	}
}

#define MTK_WFMDA_RX_DMA_EN	BIT(2)
static void
mtk_wed_check_wfdma_rx_fill(struct mtk_wed_device *dev, int idx)
{
	u32 val;
	int i;

	if (!(dev->rx_ring[idx].flags & MTK_WED_RING_CONFIGURED))
		return; /* queue is not configured by mt76 */

	for (i = 0; i < 3; i++) {
		u32 cur_idx;

		cur_idx = wed_r32(dev,
				  MTK_WED_WPDMA_RING_RX_DATA(idx) +
				  MTK_WED_RING_OFS_CPU_IDX);
		if (cur_idx == MTK_WED_RX_RING_SIZE - 1)
			break;

		usleep_range(100000, 200000);
	}

	if (i == 3) {
		dev_err(dev->hw->dev, "rx dma enable failed\n");
		return;
	}

	val = wifi_r32(dev, dev->wlan.wpdma_rx_glo - dev->wlan.phy_base) |
	      MTK_WFMDA_RX_DMA_EN;
	wifi_w32(dev, dev->wlan.wpdma_rx_glo - dev->wlan.phy_base, val);
}

static void
mtk_wed_dma_disable(struct mtk_wed_device *dev)
{
	wed_clr(dev, MTK_WED_WPDMA_GLO_CFG,
		MTK_WED_WPDMA_GLO_CFG_TX_DRV_EN |
		MTK_WED_WPDMA_GLO_CFG_RX_DRV_EN);

	wed_clr(dev, MTK_WED_WDMA_GLO_CFG, MTK_WED_WDMA_GLO_CFG_RX_DRV_EN);

	wed_clr(dev, MTK_WED_GLO_CFG,
		MTK_WED_GLO_CFG_TX_DMA_EN |
		MTK_WED_GLO_CFG_RX_DMA_EN);

	wdma_clr(dev, MTK_WDMA_GLO_CFG,
		 MTK_WDMA_GLO_CFG_TX_DMA_EN |
		 MTK_WDMA_GLO_CFG_RX_INFO1_PRERES |
		 MTK_WDMA_GLO_CFG_RX_INFO2_PRERES);

	if (dev->hw->version == 1) {
		regmap_write(dev->hw->mirror, dev->hw->index * 4, 0);
		wdma_clr(dev, MTK_WDMA_GLO_CFG,
			 MTK_WDMA_GLO_CFG_RX_INFO3_PRERES);
	} else {
		wed_clr(dev, MTK_WED_WPDMA_GLO_CFG,
			MTK_WED_WPDMA_GLO_CFG_RX_DRV_R0_PKT_PROC |
			MTK_WED_WPDMA_GLO_CFG_RX_DRV_R0_CRX_SYNC);

		wed_clr(dev, MTK_WED_WPDMA_RX_D_GLO_CFG,
			MTK_WED_WPDMA_RX_D_RX_DRV_EN);
		wed_clr(dev, MTK_WED_WDMA_GLO_CFG,
			MTK_WED_WDMA_GLO_CFG_TX_DDONE_CHK);
	}

	mtk_wed_set_512_support(dev, false);
}

static void
mtk_wed_stop(struct mtk_wed_device *dev)
{
	mtk_wed_set_ext_int(dev, false);

	wed_w32(dev, MTK_WED_WPDMA_INT_TRIGGER, 0);
	wed_w32(dev, MTK_WED_WDMA_INT_TRIGGER, 0);
	wdma_w32(dev, MTK_WDMA_INT_MASK, 0);
	wdma_w32(dev, MTK_WDMA_INT_GRP2, 0);
	wed_w32(dev, MTK_WED_WPDMA_INT_MASK, 0);

	if (dev->hw->version == 1)
		return;

	wed_w32(dev, MTK_WED_EXT_INT_MASK1, 0);
	wed_w32(dev, MTK_WED_EXT_INT_MASK2, 0);
}

static void
mtk_wed_deinit(struct mtk_wed_device *dev)
{
	mtk_wed_stop(dev);
	mtk_wed_dma_disable(dev);

	wed_clr(dev, MTK_WED_CTRL,
		MTK_WED_CTRL_WDMA_INT_AGENT_EN |
		MTK_WED_CTRL_WPDMA_INT_AGENT_EN |
		MTK_WED_CTRL_WED_TX_BM_EN |
		MTK_WED_CTRL_WED_TX_FREE_AGENT_EN);

	if (dev->hw->version == 1)
		return;

	wed_clr(dev, MTK_WED_CTRL,
		MTK_WED_CTRL_RX_ROUTE_QM_EN |
		MTK_WED_CTRL_WED_RX_BM_EN |
		MTK_WED_CTRL_RX_RRO_QM_EN);
}

static void
__mtk_wed_detach(struct mtk_wed_device *dev)
{
	struct mtk_wed_hw *hw = dev->hw;

	mtk_wed_deinit(dev);

	mtk_wdma_rx_reset(dev);
	mtk_wed_reset(dev, MTK_WED_RESET_WED);
	mtk_wed_free_tx_buffer(dev);
	mtk_wed_free_tx_rings(dev);

	if (mtk_wed_get_rx_capa(dev)) {
		if (hw->wed_wo)
			mtk_wed_wo_reset(dev);
		mtk_wed_free_rx_rings(dev);
		if (hw->wed_wo)
			mtk_wed_wo_deinit(hw);
	}

	if (dev->wlan.bus_type == MTK_WED_BUS_PCIE) {
		struct device_node *wlan_node;

		wlan_node = dev->wlan.pci_dev->dev.of_node;
		if (of_dma_is_coherent(wlan_node) && hw->hifsys)
			regmap_update_bits(hw->hifsys, HIFSYS_DMA_AG_MAP,
					   BIT(hw->index), BIT(hw->index));
	}

	if ((!hw_list[!hw->index] || !hw_list[!hw->index]->wed_dev) &&
	    hw->eth->dma_dev != hw->eth->dev)
		mtk_eth_set_dma_device(hw->eth, hw->eth->dev);

	memset(dev, 0, sizeof(*dev));
	module_put(THIS_MODULE);

	hw->wed_dev = NULL;
}

static void
mtk_wed_detach(struct mtk_wed_device *dev)
{
	mutex_lock(&hw_lock);
	__mtk_wed_detach(dev);
	mutex_unlock(&hw_lock);
}

#define PCIE_BASE_ADDR0		0x11280000
static void
mtk_wed_bus_init(struct mtk_wed_device *dev)
{
	switch (dev->wlan.bus_type) {
	case MTK_WED_BUS_PCIE: {
		struct device_node *np = dev->hw->eth->dev->of_node;
		struct regmap *regs;

		regs = syscon_regmap_lookup_by_phandle(np,
						       "mediatek,wed-pcie");
		if (IS_ERR(regs))
			break;

		regmap_update_bits(regs, 0, BIT(0), BIT(0));

		wed_w32(dev, MTK_WED_PCIE_INT_CTRL,
			FIELD_PREP(MTK_WED_PCIE_INT_CTRL_POLL_EN, 2));

		/* pcie interrupt control: pola/source selection */
		wed_set(dev, MTK_WED_PCIE_INT_CTRL,
			MTK_WED_PCIE_INT_CTRL_MSK_EN_POLA |
			FIELD_PREP(MTK_WED_PCIE_INT_CTRL_SRC_SEL, 1));
		wed_r32(dev, MTK_WED_PCIE_INT_CTRL);

		wed_w32(dev, MTK_WED_PCIE_CFG_INTM, PCIE_BASE_ADDR0 | 0x180);
		wed_w32(dev, MTK_WED_PCIE_CFG_BASE, PCIE_BASE_ADDR0 | 0x184);

		/* pcie interrupt status trigger register */
		wed_w32(dev, MTK_WED_PCIE_INT_TRIGGER, BIT(24));
		wed_r32(dev, MTK_WED_PCIE_INT_TRIGGER);

		/* pola setting */
		wed_set(dev, MTK_WED_PCIE_INT_CTRL,
			MTK_WED_PCIE_INT_CTRL_MSK_EN_POLA);
		break;
	}
	case MTK_WED_BUS_AXI:
		wed_set(dev, MTK_WED_WPDMA_INT_CTRL,
			MTK_WED_WPDMA_INT_CTRL_SIG_SRC |
			FIELD_PREP(MTK_WED_WPDMA_INT_CTRL_SRC_SEL, 0));
		break;
	default:
		break;
	}
}

static void
mtk_wed_set_wpdma(struct mtk_wed_device *dev)
{
	if (dev->hw->version == 1) {
		wed_w32(dev, MTK_WED_WPDMA_CFG_BASE,  dev->wlan.wpdma_phys);
	} else {
		mtk_wed_bus_init(dev);

		wed_w32(dev, MTK_WED_WPDMA_CFG_BASE, dev->wlan.wpdma_int);
		wed_w32(dev, MTK_WED_WPDMA_CFG_INT_MASK, dev->wlan.wpdma_mask);
		wed_w32(dev, MTK_WED_WPDMA_CFG_TX, dev->wlan.wpdma_tx);
		wed_w32(dev, MTK_WED_WPDMA_CFG_TX_FREE, dev->wlan.wpdma_txfree);
		wed_w32(dev, MTK_WED_WPDMA_RX_GLO_CFG, dev->wlan.wpdma_rx_glo);
		wed_w32(dev, MTK_WED_WPDMA_RX_RING, dev->wlan.wpdma_rx);
	}
}

static void
mtk_wed_hw_init_early(struct mtk_wed_device *dev)
{
	u32 mask, set;

	mtk_wed_deinit(dev);
	mtk_wed_reset(dev, MTK_WED_RESET_WED);
	mtk_wed_set_wpdma(dev);

	mask = MTK_WED_WDMA_GLO_CFG_BT_SIZE |
	       MTK_WED_WDMA_GLO_CFG_DYNAMIC_DMAD_RECYCLE |
	       MTK_WED_WDMA_GLO_CFG_RX_DIS_FSM_AUTO_IDLE;
	set = FIELD_PREP(MTK_WED_WDMA_GLO_CFG_BT_SIZE, 2) |
	      MTK_WED_WDMA_GLO_CFG_DYNAMIC_SKIP_DMAD_PREP |
	      MTK_WED_WDMA_GLO_CFG_IDLE_DMAD_SUPPLY;
	wed_m32(dev, MTK_WED_WDMA_GLO_CFG, mask, set);

	if (dev->hw->version == 1) {
		u32 offset = dev->hw->index ? 0x04000400 : 0;

		wdma_set(dev, MTK_WDMA_GLO_CFG,
			 MTK_WDMA_GLO_CFG_RX_INFO1_PRERES |
			 MTK_WDMA_GLO_CFG_RX_INFO2_PRERES |
			 MTK_WDMA_GLO_CFG_RX_INFO3_PRERES);

		wed_w32(dev, MTK_WED_WDMA_OFFSET0, 0x2a042a20 + offset);
		wed_w32(dev, MTK_WED_WDMA_OFFSET1, 0x29002800 + offset);
		wed_w32(dev, MTK_WED_PCIE_CFG_BASE,
			MTK_PCIE_BASE(dev->hw->index));
	} else {
		wed_w32(dev, MTK_WED_WDMA_CFG_BASE, dev->hw->wdma_phy);
		wed_set(dev, MTK_WED_CTRL, MTK_WED_CTRL_ETH_DMAD_FMT);
		wed_w32(dev, MTK_WED_WDMA_OFFSET0,
			FIELD_PREP(MTK_WED_WDMA_OFST0_GLO_INTS,
				   MTK_WDMA_INT_STATUS) |
			FIELD_PREP(MTK_WED_WDMA_OFST0_GLO_CFG,
				   MTK_WDMA_GLO_CFG));

		wed_w32(dev, MTK_WED_WDMA_OFFSET1,
			FIELD_PREP(MTK_WED_WDMA_OFST1_TX_CTRL,
				   MTK_WDMA_RING_TX(0)) |
			FIELD_PREP(MTK_WED_WDMA_OFST1_RX_CTRL,
				   MTK_WDMA_RING_RX(0)));
	}
}

static int
mtk_wed_rro_ring_alloc(struct mtk_wed_device *dev, struct mtk_wed_ring *ring,
		       int size)
{
	ring->desc = dma_alloc_coherent(dev->hw->dev,
					size * sizeof(*ring->desc),
					&ring->desc_phys, GFP_KERNEL);
	if (!ring->desc)
		return -ENOMEM;

	ring->desc_size = sizeof(*ring->desc);
	ring->size = size;

	return 0;
}

#define MTK_WED_MIOD_COUNT	(MTK_WED_MIOD_ENTRY_CNT * MTK_WED_MIOD_CNT)
static int
mtk_wed_rro_alloc(struct mtk_wed_device *dev)
{
	struct reserved_mem *rmem;
	struct device_node *np;
	int index;

	index = of_property_match_string(dev->hw->node, "memory-region-names",
					 "wo-dlm");
	if (index < 0)
		return index;

	np = of_parse_phandle(dev->hw->node, "memory-region", index);
	if (!np)
		return -ENODEV;

	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);

	if (!rmem)
		return -ENODEV;

	dev->rro.miod_phys = rmem->base;
	dev->rro.fdbk_phys = MTK_WED_MIOD_COUNT + dev->rro.miod_phys;

	return mtk_wed_rro_ring_alloc(dev, &dev->rro.ring,
				      MTK_WED_RRO_QUE_CNT);
}

static int
mtk_wed_rro_cfg(struct mtk_wed_device *dev)
{
	struct mtk_wed_wo *wo = dev->hw->wed_wo;
	struct {
		struct {
			__le32 base;
			__le32 cnt;
			__le32 unit;
		} ring[2];
		__le32 wed;
		u8 version;
	} req = {
		.ring[0] = {
			.base = cpu_to_le32(MTK_WED_WOCPU_VIEW_MIOD_BASE),
			.cnt = cpu_to_le32(MTK_WED_MIOD_CNT),
			.unit = cpu_to_le32(MTK_WED_MIOD_ENTRY_CNT),
		},
		.ring[1] = {
			.base = cpu_to_le32(MTK_WED_WOCPU_VIEW_MIOD_BASE +
					    MTK_WED_MIOD_COUNT),
			.cnt = cpu_to_le32(MTK_WED_FB_CMD_CNT),
			.unit = cpu_to_le32(4),
		},
	};

	return mtk_wed_mcu_send_msg(wo, MTK_WED_MODULE_ID_WO,
				    MTK_WED_WO_CMD_WED_CFG,
				    &req, sizeof(req), true);
}

static void
mtk_wed_rro_hw_init(struct mtk_wed_device *dev)
{
	wed_w32(dev, MTK_WED_RROQM_MIOD_CFG,
		FIELD_PREP(MTK_WED_RROQM_MIOD_MID_DW, 0x70 >> 2) |
		FIELD_PREP(MTK_WED_RROQM_MIOD_MOD_DW, 0x10 >> 2) |
		FIELD_PREP(MTK_WED_RROQM_MIOD_ENTRY_DW,
			   MTK_WED_MIOD_ENTRY_CNT >> 2));

	wed_w32(dev, MTK_WED_RROQM_MIOD_CTRL0, dev->rro.miod_phys);
	wed_w32(dev, MTK_WED_RROQM_MIOD_CTRL1,
		FIELD_PREP(MTK_WED_RROQM_MIOD_CNT, MTK_WED_MIOD_CNT));
	wed_w32(dev, MTK_WED_RROQM_FDBK_CTRL0, dev->rro.fdbk_phys);
	wed_w32(dev, MTK_WED_RROQM_FDBK_CTRL1,
		FIELD_PREP(MTK_WED_RROQM_FDBK_CNT, MTK_WED_FB_CMD_CNT));
	wed_w32(dev, MTK_WED_RROQM_FDBK_CTRL2, 0);
	wed_w32(dev, MTK_WED_RROQ_BASE_L, dev->rro.ring.desc_phys);

	wed_set(dev, MTK_WED_RROQM_RST_IDX,
		MTK_WED_RROQM_RST_IDX_MIOD |
		MTK_WED_RROQM_RST_IDX_FDBK);

	wed_w32(dev, MTK_WED_RROQM_RST_IDX, 0);
	wed_w32(dev, MTK_WED_RROQM_MIOD_CTRL2, MTK_WED_MIOD_CNT - 1);
	wed_set(dev, MTK_WED_CTRL, MTK_WED_CTRL_RX_RRO_QM_EN);
}

static void
mtk_wed_route_qm_hw_init(struct mtk_wed_device *dev)
{
	wed_w32(dev, MTK_WED_RESET, MTK_WED_RESET_RX_ROUTE_QM);

	for (;;) {
		usleep_range(100, 200);
		if (!(wed_r32(dev, MTK_WED_RESET) & MTK_WED_RESET_RX_ROUTE_QM))
			break;
	}

	/* configure RX_ROUTE_QM */
	wed_clr(dev, MTK_WED_RTQM_GLO_CFG, MTK_WED_RTQM_Q_RST);
	wed_clr(dev, MTK_WED_RTQM_GLO_CFG, MTK_WED_RTQM_TXDMAD_FPORT);
	wed_set(dev, MTK_WED_RTQM_GLO_CFG,
		FIELD_PREP(MTK_WED_RTQM_TXDMAD_FPORT, 0x3 + dev->hw->index));
	wed_clr(dev, MTK_WED_RTQM_GLO_CFG, MTK_WED_RTQM_Q_RST);
	/* enable RX_ROUTE_QM */
	wed_set(dev, MTK_WED_CTRL, MTK_WED_CTRL_RX_ROUTE_QM_EN);
}

static void
mtk_wed_hw_init(struct mtk_wed_device *dev)
{
	if (dev->init_done)
		return;

	dev->init_done = true;
	mtk_wed_set_ext_int(dev, false);
	wed_w32(dev, MTK_WED_TX_BM_CTRL,
		MTK_WED_TX_BM_CTRL_PAUSE |
		FIELD_PREP(MTK_WED_TX_BM_CTRL_VLD_GRP_NUM,
			   dev->tx_buf_ring.size / 128) |
		FIELD_PREP(MTK_WED_TX_BM_CTRL_RSV_GRP_NUM,
			   MTK_WED_TX_RING_SIZE / 256));

	wed_w32(dev, MTK_WED_TX_BM_BASE, dev->tx_buf_ring.desc_phys);

	wed_w32(dev, MTK_WED_TX_BM_BUF_LEN, MTK_WED_PKT_SIZE);

	if (dev->hw->version == 1) {
		wed_w32(dev, MTK_WED_TX_BM_TKID,
			FIELD_PREP(MTK_WED_TX_BM_TKID_START,
				   dev->wlan.token_start) |
			FIELD_PREP(MTK_WED_TX_BM_TKID_END,
				   dev->wlan.token_start +
				   dev->wlan.nbuf - 1));
		wed_w32(dev, MTK_WED_TX_BM_DYN_THR,
			FIELD_PREP(MTK_WED_TX_BM_DYN_THR_LO, 1) |
			MTK_WED_TX_BM_DYN_THR_HI);
	} else {
		wed_w32(dev, MTK_WED_TX_BM_TKID_V2,
			FIELD_PREP(MTK_WED_TX_BM_TKID_START,
				   dev->wlan.token_start) |
			FIELD_PREP(MTK_WED_TX_BM_TKID_END,
				   dev->wlan.token_start +
				   dev->wlan.nbuf - 1));
		wed_w32(dev, MTK_WED_TX_BM_DYN_THR,
			FIELD_PREP(MTK_WED_TX_BM_DYN_THR_LO_V2, 0) |
			MTK_WED_TX_BM_DYN_THR_HI_V2);
		wed_w32(dev, MTK_WED_TX_TKID_CTRL,
			MTK_WED_TX_TKID_CTRL_PAUSE |
			FIELD_PREP(MTK_WED_TX_TKID_CTRL_VLD_GRP_NUM,
				   dev->tx_buf_ring.size / 128) |
			FIELD_PREP(MTK_WED_TX_TKID_CTRL_RSV_GRP_NUM,
				   dev->tx_buf_ring.size / 128));
		wed_w32(dev, MTK_WED_TX_TKID_DYN_THR,
			FIELD_PREP(MTK_WED_TX_TKID_DYN_THR_LO, 0) |
			MTK_WED_TX_TKID_DYN_THR_HI);
	}

	mtk_wed_reset(dev, MTK_WED_RESET_TX_BM);

	if (dev->hw->version == 1) {
		wed_set(dev, MTK_WED_CTRL,
			MTK_WED_CTRL_WED_TX_BM_EN |
			MTK_WED_CTRL_WED_TX_FREE_AGENT_EN);
	} else {
		wed_clr(dev, MTK_WED_TX_TKID_CTRL, MTK_WED_TX_TKID_CTRL_PAUSE);
		/* rx hw init */
		wed_w32(dev, MTK_WED_WPDMA_RX_D_RST_IDX,
			MTK_WED_WPDMA_RX_D_RST_CRX_IDX |
			MTK_WED_WPDMA_RX_D_RST_DRV_IDX);
		wed_w32(dev, MTK_WED_WPDMA_RX_D_RST_IDX, 0);

		mtk_wed_rx_buffer_hw_init(dev);
		mtk_wed_rro_hw_init(dev);
		mtk_wed_route_qm_hw_init(dev);
	}

	wed_clr(dev, MTK_WED_TX_BM_CTRL, MTK_WED_TX_BM_CTRL_PAUSE);
}

static void
mtk_wed_ring_reset(struct mtk_wed_ring *ring, int size, bool tx)
{
	void *head = (void *)ring->desc;
	int i;

	for (i = 0; i < size; i++) {
		struct mtk_wdma_desc *desc;

		desc = (struct mtk_wdma_desc *)(head + i * ring->desc_size);
		desc->buf0 = 0;
		if (tx)
			desc->ctrl = cpu_to_le32(MTK_WDMA_DESC_CTRL_DMA_DONE);
		else
			desc->ctrl = cpu_to_le32(MTK_WFDMA_DESC_CTRL_TO_HOST);
		desc->buf1 = 0;
		desc->info = 0;
	}
}

static u32
mtk_wed_check_busy(struct mtk_wed_device *dev, u32 reg, u32 mask)
{
	return !!(wed_r32(dev, reg) & mask);
}

static int
mtk_wed_poll_busy(struct mtk_wed_device *dev, u32 reg, u32 mask)
{
	int sleep = 15000;
	int timeout = 100 * sleep;
	u32 val;

	return read_poll_timeout(mtk_wed_check_busy, val, !val, sleep,
				 timeout, false, dev, reg, mask);
}

static int
mtk_wed_rx_reset(struct mtk_wed_device *dev)
{
	struct mtk_wed_wo *wo = dev->hw->wed_wo;
	u8 val = MTK_WED_WO_STATE_SER_RESET;
	int i, ret;

	ret = mtk_wed_mcu_send_msg(wo, MTK_WED_MODULE_ID_WO,
				   MTK_WED_WO_CMD_CHANGE_STATE, &val,
				   sizeof(val), true);
	if (ret)
		return ret;

	wed_clr(dev, MTK_WED_WPDMA_RX_D_GLO_CFG, MTK_WED_WPDMA_RX_D_RX_DRV_EN);
	ret = mtk_wed_poll_busy(dev, MTK_WED_WPDMA_RX_D_GLO_CFG,
				MTK_WED_WPDMA_RX_D_RX_DRV_BUSY);
	if (ret) {
		mtk_wed_reset(dev, MTK_WED_RESET_WPDMA_INT_AGENT);
		mtk_wed_reset(dev, MTK_WED_RESET_WPDMA_RX_D_DRV);
	} else {
		wed_w32(dev, MTK_WED_WPDMA_RX_D_RST_IDX,
			MTK_WED_WPDMA_RX_D_RST_CRX_IDX |
			MTK_WED_WPDMA_RX_D_RST_DRV_IDX);

		wed_set(dev, MTK_WED_WPDMA_RX_D_GLO_CFG,
			MTK_WED_WPDMA_RX_D_RST_INIT_COMPLETE |
			MTK_WED_WPDMA_RX_D_FSM_RETURN_IDLE);
		wed_clr(dev, MTK_WED_WPDMA_RX_D_GLO_CFG,
			MTK_WED_WPDMA_RX_D_RST_INIT_COMPLETE |
			MTK_WED_WPDMA_RX_D_FSM_RETURN_IDLE);

		wed_w32(dev, MTK_WED_WPDMA_RX_D_RST_IDX, 0);
	}

	/* reset rro qm */
	wed_clr(dev, MTK_WED_CTRL, MTK_WED_CTRL_RX_RRO_QM_EN);
	ret = mtk_wed_poll_busy(dev, MTK_WED_CTRL,
				MTK_WED_CTRL_RX_RRO_QM_BUSY);
	if (ret) {
		mtk_wed_reset(dev, MTK_WED_RESET_RX_RRO_QM);
	} else {
		wed_set(dev, MTK_WED_RROQM_RST_IDX,
			MTK_WED_RROQM_RST_IDX_MIOD |
			MTK_WED_RROQM_RST_IDX_FDBK);
		wed_w32(dev, MTK_WED_RROQM_RST_IDX, 0);
	}

	/* reset route qm */
	wed_clr(dev, MTK_WED_CTRL, MTK_WED_CTRL_RX_ROUTE_QM_EN);
	ret = mtk_wed_poll_busy(dev, MTK_WED_CTRL,
				MTK_WED_CTRL_RX_ROUTE_QM_BUSY);
	if (ret)
		mtk_wed_reset(dev, MTK_WED_RESET_RX_ROUTE_QM);
	else
		wed_set(dev, MTK_WED_RTQM_GLO_CFG,
			MTK_WED_RTQM_Q_RST);

	/* reset tx wdma */
	mtk_wdma_tx_reset(dev);

	/* reset tx wdma drv */
	wed_clr(dev, MTK_WED_WDMA_GLO_CFG, MTK_WED_WDMA_GLO_CFG_TX_DRV_EN);
	mtk_wed_poll_busy(dev, MTK_WED_CTRL,
			  MTK_WED_CTRL_WDMA_INT_AGENT_BUSY);
	mtk_wed_reset(dev, MTK_WED_RESET_WDMA_TX_DRV);

	/* reset wed rx dma */
	ret = mtk_wed_poll_busy(dev, MTK_WED_GLO_CFG,
				MTK_WED_GLO_CFG_RX_DMA_BUSY);
	wed_clr(dev, MTK_WED_GLO_CFG, MTK_WED_GLO_CFG_RX_DMA_EN);
	if (ret) {
		mtk_wed_reset(dev, MTK_WED_RESET_WED_RX_DMA);
	} else {
		struct mtk_eth *eth = dev->hw->eth;

		if (mtk_is_netsys_v2_or_greater(eth))
			wed_set(dev, MTK_WED_RESET_IDX,
				MTK_WED_RESET_IDX_RX_V2);
		else
			wed_set(dev, MTK_WED_RESET_IDX, MTK_WED_RESET_IDX_RX);
		wed_w32(dev, MTK_WED_RESET_IDX, 0);
	}

	/* reset rx bm */
	wed_clr(dev, MTK_WED_CTRL, MTK_WED_CTRL_WED_RX_BM_EN);
	mtk_wed_poll_busy(dev, MTK_WED_CTRL,
			  MTK_WED_CTRL_WED_RX_BM_BUSY);
	mtk_wed_reset(dev, MTK_WED_RESET_RX_BM);

	/* wo change to enable state */
	val = MTK_WED_WO_STATE_ENABLE;
	ret = mtk_wed_mcu_send_msg(wo, MTK_WED_MODULE_ID_WO,
				   MTK_WED_WO_CMD_CHANGE_STATE, &val,
				   sizeof(val), true);
	if (ret)
		return ret;

	/* wed_rx_ring_reset */
	for (i = 0; i < ARRAY_SIZE(dev->rx_ring); i++) {
		if (!dev->rx_ring[i].desc)
			continue;

		mtk_wed_ring_reset(&dev->rx_ring[i], MTK_WED_RX_RING_SIZE,
				   false);
	}
	mtk_wed_free_rx_buffer(dev);

	return 0;
}

static void
mtk_wed_reset_dma(struct mtk_wed_device *dev)
{
	bool busy = false;
	u32 val;
	int i;

	for (i = 0; i < ARRAY_SIZE(dev->tx_ring); i++) {
		if (!dev->tx_ring[i].desc)
			continue;

		mtk_wed_ring_reset(&dev->tx_ring[i], MTK_WED_TX_RING_SIZE,
				   true);
	}

	/* 1. reset WED tx DMA */
	wed_clr(dev, MTK_WED_GLO_CFG, MTK_WED_GLO_CFG_TX_DMA_EN);
	busy = mtk_wed_poll_busy(dev, MTK_WED_GLO_CFG,
				 MTK_WED_GLO_CFG_TX_DMA_BUSY);
	if (busy) {
		mtk_wed_reset(dev, MTK_WED_RESET_WED_TX_DMA);
	} else {
		wed_w32(dev, MTK_WED_RESET_IDX, MTK_WED_RESET_IDX_TX);
		wed_w32(dev, MTK_WED_RESET_IDX, 0);
	}

	/* 2. reset WDMA rx DMA */
	busy = !!mtk_wdma_rx_reset(dev);
	wed_clr(dev, MTK_WED_WDMA_GLO_CFG, MTK_WED_WDMA_GLO_CFG_RX_DRV_EN);
	if (!busy)
		busy = mtk_wed_poll_busy(dev, MTK_WED_WDMA_GLO_CFG,
					 MTK_WED_WDMA_GLO_CFG_RX_DRV_BUSY);

	if (busy) {
		mtk_wed_reset(dev, MTK_WED_RESET_WDMA_INT_AGENT);
		mtk_wed_reset(dev, MTK_WED_RESET_WDMA_RX_DRV);
	} else {
		wed_w32(dev, MTK_WED_WDMA_RESET_IDX,
			MTK_WED_WDMA_RESET_IDX_RX | MTK_WED_WDMA_RESET_IDX_DRV);
		wed_w32(dev, MTK_WED_WDMA_RESET_IDX, 0);

		wed_set(dev, MTK_WED_WDMA_GLO_CFG,
			MTK_WED_WDMA_GLO_CFG_RST_INIT_COMPLETE);

		wed_clr(dev, MTK_WED_WDMA_GLO_CFG,
			MTK_WED_WDMA_GLO_CFG_RST_INIT_COMPLETE);
	}

	/* 3. reset WED WPDMA tx */
	wed_clr(dev, MTK_WED_CTRL, MTK_WED_CTRL_WED_TX_FREE_AGENT_EN);

	for (i = 0; i < 100; i++) {
		val = wed_r32(dev, MTK_WED_TX_BM_INTF);
		if (FIELD_GET(MTK_WED_TX_BM_INTF_TKFIFO_FDEP, val) == 0x40)
			break;
	}

	mtk_wed_reset(dev, MTK_WED_RESET_TX_FREE_AGENT);
	wed_clr(dev, MTK_WED_CTRL, MTK_WED_CTRL_WED_TX_BM_EN);
	mtk_wed_reset(dev, MTK_WED_RESET_TX_BM);

	/* 4. reset WED WPDMA tx */
	busy = mtk_wed_poll_busy(dev, MTK_WED_WPDMA_GLO_CFG,
				 MTK_WED_WPDMA_GLO_CFG_TX_DRV_BUSY);
	wed_clr(dev, MTK_WED_WPDMA_GLO_CFG,
		MTK_WED_WPDMA_GLO_CFG_TX_DRV_EN |
		MTK_WED_WPDMA_GLO_CFG_RX_DRV_EN);
	if (!busy)
		busy = mtk_wed_poll_busy(dev, MTK_WED_WPDMA_GLO_CFG,
					 MTK_WED_WPDMA_GLO_CFG_RX_DRV_BUSY);

	if (busy) {
		mtk_wed_reset(dev, MTK_WED_RESET_WPDMA_INT_AGENT);
		mtk_wed_reset(dev, MTK_WED_RESET_WPDMA_TX_DRV);
		mtk_wed_reset(dev, MTK_WED_RESET_WPDMA_RX_DRV);
	} else {
		wed_w32(dev, MTK_WED_WPDMA_RESET_IDX,
			MTK_WED_WPDMA_RESET_IDX_TX |
			MTK_WED_WPDMA_RESET_IDX_RX);
		wed_w32(dev, MTK_WED_WPDMA_RESET_IDX, 0);
	}

	dev->init_done = false;
	if (dev->hw->version == 1)
		return;

	if (!busy) {
		wed_w32(dev, MTK_WED_RESET_IDX, MTK_WED_RESET_WPDMA_IDX_RX);
		wed_w32(dev, MTK_WED_RESET_IDX, 0);
	}

	mtk_wed_rx_reset(dev);
}

static int
mtk_wed_ring_alloc(struct mtk_wed_device *dev, struct mtk_wed_ring *ring,
		   int size, u32 desc_size, bool tx)
{
	ring->desc = dma_alloc_coherent(dev->hw->dev, size * desc_size,
					&ring->desc_phys, GFP_KERNEL);
	if (!ring->desc)
		return -ENOMEM;

	ring->desc_size = desc_size;
	ring->size = size;
	mtk_wed_ring_reset(ring, size, tx);

	return 0;
}

static int
mtk_wed_wdma_rx_ring_setup(struct mtk_wed_device *dev, int idx, int size,
			   bool reset)
{
	u32 desc_size = sizeof(struct mtk_wdma_desc) * dev->hw->version;
	struct mtk_wed_ring *wdma;

	if (idx >= ARRAY_SIZE(dev->rx_wdma))
		return -EINVAL;

	wdma = &dev->rx_wdma[idx];
	if (!reset && mtk_wed_ring_alloc(dev, wdma, MTK_WED_WDMA_RING_SIZE,
					 desc_size, true))
		return -ENOMEM;

	wdma_w32(dev, MTK_WDMA_RING_RX(idx) + MTK_WED_RING_OFS_BASE,
		 wdma->desc_phys);
	wdma_w32(dev, MTK_WDMA_RING_RX(idx) + MTK_WED_RING_OFS_COUNT,
		 size);
	wdma_w32(dev, MTK_WDMA_RING_RX(idx) + MTK_WED_RING_OFS_CPU_IDX, 0);

	wed_w32(dev, MTK_WED_WDMA_RING_RX(idx) + MTK_WED_RING_OFS_BASE,
		wdma->desc_phys);
	wed_w32(dev, MTK_WED_WDMA_RING_RX(idx) + MTK_WED_RING_OFS_COUNT,
		size);

	return 0;
}

static int
mtk_wed_wdma_tx_ring_setup(struct mtk_wed_device *dev, int idx, int size,
			   bool reset)
{
	u32 desc_size = sizeof(struct mtk_wdma_desc) * dev->hw->version;
	struct mtk_wed_ring *wdma;

	if (idx >= ARRAY_SIZE(dev->tx_wdma))
		return -EINVAL;

	wdma = &dev->tx_wdma[idx];
	if (!reset && mtk_wed_ring_alloc(dev, wdma, MTK_WED_WDMA_RING_SIZE,
					 desc_size, true))
		return -ENOMEM;

	wdma_w32(dev, MTK_WDMA_RING_TX(idx) + MTK_WED_RING_OFS_BASE,
		 wdma->desc_phys);
	wdma_w32(dev, MTK_WDMA_RING_TX(idx) + MTK_WED_RING_OFS_COUNT,
		 size);
	wdma_w32(dev, MTK_WDMA_RING_TX(idx) + MTK_WED_RING_OFS_CPU_IDX, 0);
	wdma_w32(dev, MTK_WDMA_RING_TX(idx) + MTK_WED_RING_OFS_DMA_IDX, 0);

	if (reset)
		mtk_wed_ring_reset(wdma, MTK_WED_WDMA_RING_SIZE, true);

	if (!idx)  {
		wed_w32(dev, MTK_WED_WDMA_RING_TX + MTK_WED_RING_OFS_BASE,
			wdma->desc_phys);
		wed_w32(dev, MTK_WED_WDMA_RING_TX + MTK_WED_RING_OFS_COUNT,
			size);
		wed_w32(dev, MTK_WED_WDMA_RING_TX + MTK_WED_RING_OFS_CPU_IDX,
			0);
		wed_w32(dev, MTK_WED_WDMA_RING_TX + MTK_WED_RING_OFS_DMA_IDX,
			0);
	}

	return 0;
}

static void
mtk_wed_ppe_check(struct mtk_wed_device *dev, struct sk_buff *skb,
		  u32 reason, u32 hash)
{
	struct mtk_eth *eth = dev->hw->eth;
	struct ethhdr *eh;

	if (!skb)
		return;

	if (reason != MTK_PPE_CPU_REASON_HIT_UNBIND_RATE_REACHED)
		return;

	skb_set_mac_header(skb, 0);
	eh = eth_hdr(skb);
	skb->protocol = eh->h_proto;
	mtk_ppe_check_skb(eth->ppe[dev->hw->index], skb, hash);
}

static void
mtk_wed_configure_irq(struct mtk_wed_device *dev, u32 irq_mask)
{
	u32 wdma_mask = FIELD_PREP(MTK_WDMA_INT_MASK_RX_DONE, GENMASK(1, 0));

	/* wed control cr set */
	wed_set(dev, MTK_WED_CTRL,
		MTK_WED_CTRL_WDMA_INT_AGENT_EN |
		MTK_WED_CTRL_WPDMA_INT_AGENT_EN |
		MTK_WED_CTRL_WED_TX_BM_EN |
		MTK_WED_CTRL_WED_TX_FREE_AGENT_EN);

	if (dev->hw->version == 1) {
		wed_w32(dev, MTK_WED_PCIE_INT_TRIGGER,
			MTK_WED_PCIE_INT_TRIGGER_STATUS);

		wed_w32(dev, MTK_WED_WPDMA_INT_TRIGGER,
			MTK_WED_WPDMA_INT_TRIGGER_RX_DONE |
			MTK_WED_WPDMA_INT_TRIGGER_TX_DONE);

		wed_clr(dev, MTK_WED_WDMA_INT_CTRL, wdma_mask);
	} else {
		wdma_mask |= FIELD_PREP(MTK_WDMA_INT_MASK_TX_DONE,
					GENMASK(1, 0));
		/* initail tx interrupt trigger */
		wed_w32(dev, MTK_WED_WPDMA_INT_CTRL_TX,
			MTK_WED_WPDMA_INT_CTRL_TX0_DONE_EN |
			MTK_WED_WPDMA_INT_CTRL_TX0_DONE_CLR |
			MTK_WED_WPDMA_INT_CTRL_TX1_DONE_EN |
			MTK_WED_WPDMA_INT_CTRL_TX1_DONE_CLR |
			FIELD_PREP(MTK_WED_WPDMA_INT_CTRL_TX0_DONE_TRIG,
				   dev->wlan.tx_tbit[0]) |
			FIELD_PREP(MTK_WED_WPDMA_INT_CTRL_TX1_DONE_TRIG,
				   dev->wlan.tx_tbit[1]));

		/* initail txfree interrupt trigger */
		wed_w32(dev, MTK_WED_WPDMA_INT_CTRL_TX_FREE,
			MTK_WED_WPDMA_INT_CTRL_TX_FREE_DONE_EN |
			MTK_WED_WPDMA_INT_CTRL_TX_FREE_DONE_CLR |
			FIELD_PREP(MTK_WED_WPDMA_INT_CTRL_TX_FREE_DONE_TRIG,
				   dev->wlan.txfree_tbit));

		wed_w32(dev, MTK_WED_WPDMA_INT_CTRL_RX,
			MTK_WED_WPDMA_INT_CTRL_RX0_EN |
			MTK_WED_WPDMA_INT_CTRL_RX0_CLR |
			MTK_WED_WPDMA_INT_CTRL_RX1_EN |
			MTK_WED_WPDMA_INT_CTRL_RX1_CLR |
			FIELD_PREP(MTK_WED_WPDMA_INT_CTRL_RX0_DONE_TRIG,
				   dev->wlan.rx_tbit[0]) |
			FIELD_PREP(MTK_WED_WPDMA_INT_CTRL_RX1_DONE_TRIG,
				   dev->wlan.rx_tbit[1]));

		wed_w32(dev, MTK_WED_WDMA_INT_CLR, wdma_mask);
		wed_set(dev, MTK_WED_WDMA_INT_CTRL,
			FIELD_PREP(MTK_WED_WDMA_INT_CTRL_POLL_SRC_SEL,
				   dev->wdma_idx));
	}

	wed_w32(dev, MTK_WED_WDMA_INT_TRIGGER, wdma_mask);

	wdma_w32(dev, MTK_WDMA_INT_MASK, wdma_mask);
	wdma_w32(dev, MTK_WDMA_INT_GRP2, wdma_mask);
	wed_w32(dev, MTK_WED_WPDMA_INT_MASK, irq_mask);
	wed_w32(dev, MTK_WED_INT_MASK, irq_mask);
}

static void
mtk_wed_dma_enable(struct mtk_wed_device *dev)
{
	wed_set(dev, MTK_WED_WPDMA_INT_CTRL, MTK_WED_WPDMA_INT_CTRL_SUBRT_ADV);

	wed_set(dev, MTK_WED_GLO_CFG,
		MTK_WED_GLO_CFG_TX_DMA_EN |
		MTK_WED_GLO_CFG_RX_DMA_EN);
	wed_set(dev, MTK_WED_WPDMA_GLO_CFG,
		MTK_WED_WPDMA_GLO_CFG_TX_DRV_EN |
		MTK_WED_WPDMA_GLO_CFG_RX_DRV_EN);
	wed_set(dev, MTK_WED_WDMA_GLO_CFG,
		MTK_WED_WDMA_GLO_CFG_RX_DRV_EN);

	wdma_set(dev, MTK_WDMA_GLO_CFG,
		 MTK_WDMA_GLO_CFG_TX_DMA_EN |
		 MTK_WDMA_GLO_CFG_RX_INFO1_PRERES |
		 MTK_WDMA_GLO_CFG_RX_INFO2_PRERES);

	if (dev->hw->version == 1) {
		wdma_set(dev, MTK_WDMA_GLO_CFG,
			 MTK_WDMA_GLO_CFG_RX_INFO3_PRERES);
	} else {
		int i;

		wed_set(dev, MTK_WED_WPDMA_CTRL,
			MTK_WED_WPDMA_CTRL_SDL1_FIXED);

		wed_set(dev, MTK_WED_WDMA_GLO_CFG,
			MTK_WED_WDMA_GLO_CFG_TX_DRV_EN |
			MTK_WED_WDMA_GLO_CFG_TX_DDONE_CHK);

		wed_set(dev, MTK_WED_WPDMA_GLO_CFG,
			MTK_WED_WPDMA_GLO_CFG_RX_DRV_R0_PKT_PROC |
			MTK_WED_WPDMA_GLO_CFG_RX_DRV_R0_CRX_SYNC);

		wed_clr(dev, MTK_WED_WPDMA_GLO_CFG,
			MTK_WED_WPDMA_GLO_CFG_TX_TKID_KEEP |
			MTK_WED_WPDMA_GLO_CFG_TX_DMAD_DW3_PREV);

		wed_set(dev, MTK_WED_WPDMA_RX_D_GLO_CFG,
			MTK_WED_WPDMA_RX_D_RX_DRV_EN |
			FIELD_PREP(MTK_WED_WPDMA_RX_D_RXD_READ_LEN, 0x18) |
			FIELD_PREP(MTK_WED_WPDMA_RX_D_INIT_PHASE_RXEN_SEL,
				   0x2));

		for (i = 0; i < MTK_WED_RX_QUEUES; i++)
			mtk_wed_check_wfdma_rx_fill(dev, i);
	}
}

static void
mtk_wed_start(struct mtk_wed_device *dev, u32 irq_mask)
{
	int i;

	if (mtk_wed_get_rx_capa(dev) && mtk_wed_rx_buffer_alloc(dev))
		return;

	for (i = 0; i < ARRAY_SIZE(dev->rx_wdma); i++)
		if (!dev->rx_wdma[i].desc)
			mtk_wed_wdma_rx_ring_setup(dev, i, 16, false);

	mtk_wed_hw_init(dev);
	mtk_wed_configure_irq(dev, irq_mask);

	mtk_wed_set_ext_int(dev, true);

	if (dev->hw->version == 1) {
		u32 val = dev->wlan.wpdma_phys | MTK_PCIE_MIRROR_MAP_EN |
			  FIELD_PREP(MTK_PCIE_MIRROR_MAP_WED_ID,
				     dev->hw->index);

		val |= BIT(0) | (BIT(1) * !!dev->hw->index);
		regmap_write(dev->hw->mirror, dev->hw->index * 4, val);
	} else {
		/* driver set mid ready and only once */
		wed_w32(dev, MTK_WED_EXT_INT_MASK1,
			MTK_WED_EXT_INT_STATUS_WPDMA_MID_RDY);
		wed_w32(dev, MTK_WED_EXT_INT_MASK2,
			MTK_WED_EXT_INT_STATUS_WPDMA_MID_RDY);

		wed_r32(dev, MTK_WED_EXT_INT_MASK1);
		wed_r32(dev, MTK_WED_EXT_INT_MASK2);

		if (mtk_wed_rro_cfg(dev))
			return;

	}

	mtk_wed_set_512_support(dev, dev->wlan.wcid_512);

	mtk_wed_dma_enable(dev);
	dev->running = true;
}

static int
mtk_wed_attach(struct mtk_wed_device *dev)
	__releases(RCU)
{
	struct mtk_wed_hw *hw;
	struct device *device;
	int ret = 0;

	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			 "mtk_wed_attach without holding the RCU read lock");

	if ((dev->wlan.bus_type == MTK_WED_BUS_PCIE &&
	     pci_domain_nr(dev->wlan.pci_dev->bus) > 1) ||
	    !try_module_get(THIS_MODULE))
		ret = -ENODEV;

	rcu_read_unlock();

	if (ret)
		return ret;

	mutex_lock(&hw_lock);

	hw = mtk_wed_assign(dev);
	if (!hw) {
		module_put(THIS_MODULE);
		ret = -ENODEV;
		goto unlock;
	}

	device = dev->wlan.bus_type == MTK_WED_BUS_PCIE
		? &dev->wlan.pci_dev->dev
		: &dev->wlan.platform_dev->dev;
	dev_info(device, "attaching wed device %d version %d\n",
		 hw->index, hw->version);

	dev->hw = hw;
	dev->dev = hw->dev;
	dev->irq = hw->irq;
	dev->wdma_idx = hw->index;
	dev->version = hw->version;

	if (hw->eth->dma_dev == hw->eth->dev &&
	    of_dma_is_coherent(hw->eth->dev->of_node))
		mtk_eth_set_dma_device(hw->eth, hw->dev);

	ret = mtk_wed_tx_buffer_alloc(dev);
	if (ret)
		goto out;

	if (mtk_wed_get_rx_capa(dev)) {
		ret = mtk_wed_rro_alloc(dev);
		if (ret)
			goto out;
	}

	mtk_wed_hw_init_early(dev);
	if (hw->version == 1) {
		regmap_update_bits(hw->hifsys, HIFSYS_DMA_AG_MAP,
				   BIT(hw->index), 0);
	} else {
		dev->rev_id = wed_r32(dev, MTK_WED_REV_ID);
		ret = mtk_wed_wo_init(hw);
	}
out:
	if (ret) {
		dev_err(dev->hw->dev, "failed to attach wed device\n");
		__mtk_wed_detach(dev);
	}
unlock:
	mutex_unlock(&hw_lock);

	return ret;
}

static int
mtk_wed_tx_ring_setup(struct mtk_wed_device *dev, int idx, void __iomem *regs,
		      bool reset)
{
	struct mtk_wed_ring *ring = &dev->tx_ring[idx];

	/*
	 * Tx ring redirection:
	 * Instead of configuring the WLAN PDMA TX ring directly, the WLAN
	 * driver allocated DMA ring gets configured into WED MTK_WED_RING_TX(n)
	 * registers.
	 *
	 * WED driver posts its own DMA ring as WLAN PDMA TX and configures it
	 * into MTK_WED_WPDMA_RING_TX(n) registers.
	 * It gets filled with packets picked up from WED TX ring and from
	 * WDMA RX.
	 */

	if (WARN_ON(idx >= ARRAY_SIZE(dev->tx_ring)))
		return -EINVAL;

	if (!reset && mtk_wed_ring_alloc(dev, ring, MTK_WED_TX_RING_SIZE,
					 sizeof(*ring->desc), true))
		return -ENOMEM;

	if (mtk_wed_wdma_rx_ring_setup(dev, idx, MTK_WED_WDMA_RING_SIZE,
				       reset))
		return -ENOMEM;

	ring->reg_base = MTK_WED_RING_TX(idx);
	ring->wpdma = regs;

	/* WED -> WPDMA */
	wpdma_tx_w32(dev, idx, MTK_WED_RING_OFS_BASE, ring->desc_phys);
	wpdma_tx_w32(dev, idx, MTK_WED_RING_OFS_COUNT, MTK_WED_TX_RING_SIZE);
	wpdma_tx_w32(dev, idx, MTK_WED_RING_OFS_CPU_IDX, 0);

	wed_w32(dev, MTK_WED_WPDMA_RING_TX(idx) + MTK_WED_RING_OFS_BASE,
		ring->desc_phys);
	wed_w32(dev, MTK_WED_WPDMA_RING_TX(idx) + MTK_WED_RING_OFS_COUNT,
		MTK_WED_TX_RING_SIZE);
	wed_w32(dev, MTK_WED_WPDMA_RING_TX(idx) + MTK_WED_RING_OFS_CPU_IDX, 0);

	return 0;
}

static int
mtk_wed_txfree_ring_setup(struct mtk_wed_device *dev, void __iomem *regs)
{
	struct mtk_wed_ring *ring = &dev->txfree_ring;
	int i, index = dev->hw->version == 1;

	/*
	 * For txfree event handling, the same DMA ring is shared between WED
	 * and WLAN. The WLAN driver accesses the ring index registers through
	 * WED
	 */
	ring->reg_base = MTK_WED_RING_RX(index);
	ring->wpdma = regs;

	for (i = 0; i < 12; i += 4) {
		u32 val = readl(regs + i);

		wed_w32(dev, MTK_WED_RING_RX(index) + i, val);
		wed_w32(dev, MTK_WED_WPDMA_RING_RX(index) + i, val);
	}

	return 0;
}

static int
mtk_wed_rx_ring_setup(struct mtk_wed_device *dev, int idx, void __iomem *regs,
		      bool reset)
{
	struct mtk_wed_ring *ring = &dev->rx_ring[idx];

	if (WARN_ON(idx >= ARRAY_SIZE(dev->rx_ring)))
		return -EINVAL;

	if (!reset && mtk_wed_ring_alloc(dev, ring, MTK_WED_RX_RING_SIZE,
					 sizeof(*ring->desc), false))
		return -ENOMEM;

	if (mtk_wed_wdma_tx_ring_setup(dev, idx, MTK_WED_WDMA_RING_SIZE,
				       reset))
		return -ENOMEM;

	ring->reg_base = MTK_WED_RING_RX_DATA(idx);
	ring->wpdma = regs;
	ring->flags |= MTK_WED_RING_CONFIGURED;

	/* WPDMA ->  WED */
	wpdma_rx_w32(dev, idx, MTK_WED_RING_OFS_BASE, ring->desc_phys);
	wpdma_rx_w32(dev, idx, MTK_WED_RING_OFS_COUNT, MTK_WED_RX_RING_SIZE);

	wed_w32(dev, MTK_WED_WPDMA_RING_RX_DATA(idx) + MTK_WED_RING_OFS_BASE,
		ring->desc_phys);
	wed_w32(dev, MTK_WED_WPDMA_RING_RX_DATA(idx) + MTK_WED_RING_OFS_COUNT,
		MTK_WED_RX_RING_SIZE);

	return 0;
}

static u32
mtk_wed_irq_get(struct mtk_wed_device *dev, u32 mask)
{
	u32 val, ext_mask = MTK_WED_EXT_INT_STATUS_ERROR_MASK;

	if (dev->hw->version == 1)
		ext_mask |= MTK_WED_EXT_INT_STATUS_TX_DRV_R_RESP_ERR;
	else
		ext_mask |= MTK_WED_EXT_INT_STATUS_RX_FBUF_LO_TH |
			    MTK_WED_EXT_INT_STATUS_RX_FBUF_HI_TH |
			    MTK_WED_EXT_INT_STATUS_RX_DRV_COHERENT |
			    MTK_WED_EXT_INT_STATUS_TX_DMA_W_RESP_ERR;

	val = wed_r32(dev, MTK_WED_EXT_INT_STATUS);
	wed_w32(dev, MTK_WED_EXT_INT_STATUS, val);
	val &= ext_mask;
	if (!dev->hw->num_flows)
		val &= ~MTK_WED_EXT_INT_STATUS_TKID_WO_PYLD;
	if (val && net_ratelimit())
		pr_err("mtk_wed%d: error status=%08x\n", dev->hw->index, val);

	val = wed_r32(dev, MTK_WED_INT_STATUS);
	val &= mask;
	wed_w32(dev, MTK_WED_INT_STATUS, val); /* ACK */

	return val;
}

static void
mtk_wed_irq_set_mask(struct mtk_wed_device *dev, u32 mask)
{
	if (!dev->running)
		return;

	mtk_wed_set_ext_int(dev, !!mask);
	wed_w32(dev, MTK_WED_INT_MASK, mask);
}

int mtk_wed_flow_add(int index)
{
	struct mtk_wed_hw *hw = hw_list[index];
	int ret;

	if (!hw || !hw->wed_dev)
		return -ENODEV;

	if (hw->num_flows) {
		hw->num_flows++;
		return 0;
	}

	mutex_lock(&hw_lock);
	if (!hw->wed_dev) {
		ret = -ENODEV;
		goto out;
	}

	ret = hw->wed_dev->wlan.offload_enable(hw->wed_dev);
	if (!ret)
		hw->num_flows++;
	mtk_wed_set_ext_int(hw->wed_dev, true);

out:
	mutex_unlock(&hw_lock);

	return ret;
}

void mtk_wed_flow_remove(int index)
{
	struct mtk_wed_hw *hw = hw_list[index];

	if (!hw)
		return;

	if (--hw->num_flows)
		return;

	mutex_lock(&hw_lock);
	if (!hw->wed_dev)
		goto out;

	hw->wed_dev->wlan.offload_disable(hw->wed_dev);
	mtk_wed_set_ext_int(hw->wed_dev, true);

out:
	mutex_unlock(&hw_lock);
}

static int
mtk_wed_setup_tc_block_cb(enum tc_setup_type type, void *type_data, void *cb_priv)
{
	struct mtk_wed_flow_block_priv *priv = cb_priv;
	struct flow_cls_offload *cls = type_data;
	struct mtk_wed_hw *hw = priv->hw;

	if (!tc_can_offload(priv->dev))
		return -EOPNOTSUPP;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	return mtk_flow_offload_cmd(hw->eth, cls, hw->index);
}

static int
mtk_wed_setup_tc_block(struct mtk_wed_hw *hw, struct net_device *dev,
		       struct flow_block_offload *f)
{
	struct mtk_wed_flow_block_priv *priv;
	static LIST_HEAD(block_cb_list);
	struct flow_block_cb *block_cb;
	struct mtk_eth *eth = hw->eth;
	flow_setup_cb_t *cb;

	if (!eth->soc->offload_version)
		return -EOPNOTSUPP;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	cb = mtk_wed_setup_tc_block_cb;
	f->driver_block_list = &block_cb_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block, cb, dev);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}

		priv = kzalloc(sizeof(*priv), GFP_KERNEL);
		if (!priv)
			return -ENOMEM;

		priv->hw = hw;
		priv->dev = dev;
		block_cb = flow_block_cb_alloc(cb, dev, priv, NULL);
		if (IS_ERR(block_cb)) {
			kfree(priv);
			return PTR_ERR(block_cb);
		}

		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &block_cb_list);
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, cb, dev);
		if (!block_cb)
			return -ENOENT;

		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
			kfree(block_cb->cb_priv);
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int
mtk_wed_setup_tc(struct mtk_wed_device *wed, struct net_device *dev,
		 enum tc_setup_type type, void *type_data)
{
	struct mtk_wed_hw *hw = wed->hw;

	if (hw->version < 2)
		return -EOPNOTSUPP;

	switch (type) {
	case TC_SETUP_BLOCK:
	case TC_SETUP_FT:
		return mtk_wed_setup_tc_block(hw, dev, type_data);
	default:
		return -EOPNOTSUPP;
	}
}

void mtk_wed_add_hw(struct device_node *np, struct mtk_eth *eth,
		    void __iomem *wdma, phys_addr_t wdma_phy,
		    int index)
{
	static const struct mtk_wed_ops wed_ops = {
		.attach = mtk_wed_attach,
		.tx_ring_setup = mtk_wed_tx_ring_setup,
		.rx_ring_setup = mtk_wed_rx_ring_setup,
		.txfree_ring_setup = mtk_wed_txfree_ring_setup,
		.msg_update = mtk_wed_mcu_msg_update,
		.start = mtk_wed_start,
		.stop = mtk_wed_stop,
		.reset_dma = mtk_wed_reset_dma,
		.reg_read = wed_r32,
		.reg_write = wed_w32,
		.irq_get = mtk_wed_irq_get,
		.irq_set_mask = mtk_wed_irq_set_mask,
		.detach = mtk_wed_detach,
		.ppe_check = mtk_wed_ppe_check,
		.setup_tc = mtk_wed_setup_tc,
	};
	struct device_node *eth_np = eth->dev->of_node;
	struct platform_device *pdev;
	struct mtk_wed_hw *hw;
	struct regmap *regs;
	int irq;

	if (!np)
		return;

	pdev = of_find_device_by_node(np);
	if (!pdev)
		goto err_of_node_put;

	get_device(&pdev->dev);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		goto err_put_device;

	regs = syscon_regmap_lookup_by_phandle(np, NULL);
	if (IS_ERR(regs))
		goto err_put_device;

	rcu_assign_pointer(mtk_soc_wed_ops, &wed_ops);

	mutex_lock(&hw_lock);

	if (WARN_ON(hw_list[index]))
		goto unlock;

	hw = kzalloc(sizeof(*hw), GFP_KERNEL);
	if (!hw)
		goto unlock;

	hw->node = np;
	hw->regs = regs;
	hw->eth = eth;
	hw->dev = &pdev->dev;
	hw->wdma_phy = wdma_phy;
	hw->wdma = wdma;
	hw->index = index;
	hw->irq = irq;
	hw->version = mtk_is_netsys_v1(eth) ? 1 : 2;

	if (hw->version == 1) {
		hw->mirror = syscon_regmap_lookup_by_phandle(eth_np,
				"mediatek,pcie-mirror");
		hw->hifsys = syscon_regmap_lookup_by_phandle(eth_np,
				"mediatek,hifsys");
		if (IS_ERR(hw->mirror) || IS_ERR(hw->hifsys)) {
			kfree(hw);
			goto unlock;
		}

		if (!index) {
			regmap_write(hw->mirror, 0, 0);
			regmap_write(hw->mirror, 4, 0);
		}
	}

	mtk_wed_hw_add_debugfs(hw);

	hw_list[index] = hw;

	mutex_unlock(&hw_lock);

	return;

unlock:
	mutex_unlock(&hw_lock);
err_put_device:
	put_device(&pdev->dev);
err_of_node_put:
	of_node_put(np);
}

void mtk_wed_exit(void)
{
	int i;

	rcu_assign_pointer(mtk_soc_wed_ops, NULL);

	synchronize_rcu();

	for (i = 0; i < ARRAY_SIZE(hw_list); i++) {
		struct mtk_wed_hw *hw;

		hw = hw_list[i];
		if (!hw)
			continue;

		hw_list[i] = NULL;
		debugfs_remove(hw->debugfs_dir);
		put_device(hw->dev);
		of_node_put(hw->node);
		kfree(hw);
	}
}
