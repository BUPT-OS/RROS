// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2021 Felix Fietkau <nbd@nbd.name> */

#include <linux/seq_file.h>
#include <linux/soc/mediatek/mtk_wed.h>
#include "mtk_wed.h"
#include "mtk_wed_regs.h"

struct reg_dump {
	const char *name;
	u16 offset;
	u8 type;
	u8 base;
};

enum {
	DUMP_TYPE_STRING,
	DUMP_TYPE_WED,
	DUMP_TYPE_WDMA,
	DUMP_TYPE_WPDMA_TX,
	DUMP_TYPE_WPDMA_TXFREE,
	DUMP_TYPE_WPDMA_RX,
	DUMP_TYPE_WED_RRO,
};

#define DUMP_STR(_str) { _str, 0, DUMP_TYPE_STRING }
#define DUMP_REG(_reg, ...) { #_reg, MTK_##_reg, __VA_ARGS__ }
#define DUMP_RING(_prefix, _base, ...)				\
	{ _prefix " BASE", _base, __VA_ARGS__ },		\
	{ _prefix " CNT",  _base + 0x4, __VA_ARGS__ },	\
	{ _prefix " CIDX", _base + 0x8, __VA_ARGS__ },	\
	{ _prefix " DIDX", _base + 0xc, __VA_ARGS__ }

#define DUMP_WED(_reg) DUMP_REG(_reg, DUMP_TYPE_WED)
#define DUMP_WED_RING(_base) DUMP_RING(#_base, MTK_##_base, DUMP_TYPE_WED)

#define DUMP_WDMA(_reg) DUMP_REG(_reg, DUMP_TYPE_WDMA)
#define DUMP_WDMA_RING(_base) DUMP_RING(#_base, MTK_##_base, DUMP_TYPE_WDMA)

#define DUMP_WPDMA_TX_RING(_n) DUMP_RING("WPDMA_TX" #_n, 0, DUMP_TYPE_WPDMA_TX, _n)
#define DUMP_WPDMA_TXFREE_RING DUMP_RING("WPDMA_RX1", 0, DUMP_TYPE_WPDMA_TXFREE)
#define DUMP_WPDMA_RX_RING(_n)	DUMP_RING("WPDMA_RX" #_n, 0, DUMP_TYPE_WPDMA_RX, _n)
#define DUMP_WED_RRO_RING(_base)DUMP_RING("WED_RRO_MIOD", MTK_##_base, DUMP_TYPE_WED_RRO)
#define DUMP_WED_RRO_FDBK(_base)DUMP_RING("WED_RRO_FDBK", MTK_##_base, DUMP_TYPE_WED_RRO)

static void
print_reg_val(struct seq_file *s, const char *name, u32 val)
{
	seq_printf(s, "%-32s %08x\n", name, val);
}

static void
dump_wed_regs(struct seq_file *s, struct mtk_wed_device *dev,
	      const struct reg_dump *regs, int n_regs)
{
	const struct reg_dump *cur;
	u32 val;

	for (cur = regs; cur < &regs[n_regs]; cur++) {
		switch (cur->type) {
		case DUMP_TYPE_STRING:
			seq_printf(s, "%s======== %s:\n",
				   cur > regs ? "\n" : "",
				   cur->name);
			continue;
		case DUMP_TYPE_WED_RRO:
		case DUMP_TYPE_WED:
			val = wed_r32(dev, cur->offset);
			break;
		case DUMP_TYPE_WDMA:
			val = wdma_r32(dev, cur->offset);
			break;
		case DUMP_TYPE_WPDMA_TX:
			val = wpdma_tx_r32(dev, cur->base, cur->offset);
			break;
		case DUMP_TYPE_WPDMA_TXFREE:
			val = wpdma_txfree_r32(dev, cur->offset);
			break;
		case DUMP_TYPE_WPDMA_RX:
			val = wpdma_rx_r32(dev, cur->base, cur->offset);
			break;
		}
		print_reg_val(s, cur->name, val);
	}
}

static int
wed_txinfo_show(struct seq_file *s, void *data)
{
	static const struct reg_dump regs[] = {
		DUMP_STR("WED TX"),
		DUMP_WED(WED_TX_MIB(0)),
		DUMP_WED_RING(WED_RING_TX(0)),

		DUMP_WED(WED_TX_MIB(1)),
		DUMP_WED_RING(WED_RING_TX(1)),

		DUMP_STR("WPDMA TX"),
		DUMP_WED(WED_WPDMA_TX_MIB(0)),
		DUMP_WED_RING(WED_WPDMA_RING_TX(0)),
		DUMP_WED(WED_WPDMA_TX_COHERENT_MIB(0)),

		DUMP_WED(WED_WPDMA_TX_MIB(1)),
		DUMP_WED_RING(WED_WPDMA_RING_TX(1)),
		DUMP_WED(WED_WPDMA_TX_COHERENT_MIB(1)),

		DUMP_STR("WPDMA TX"),
		DUMP_WPDMA_TX_RING(0),
		DUMP_WPDMA_TX_RING(1),

		DUMP_STR("WED WDMA RX"),
		DUMP_WED(WED_WDMA_RX_MIB(0)),
		DUMP_WED_RING(WED_WDMA_RING_RX(0)),
		DUMP_WED(WED_WDMA_RX_THRES(0)),
		DUMP_WED(WED_WDMA_RX_RECYCLE_MIB(0)),
		DUMP_WED(WED_WDMA_RX_PROCESSED_MIB(0)),

		DUMP_WED(WED_WDMA_RX_MIB(1)),
		DUMP_WED_RING(WED_WDMA_RING_RX(1)),
		DUMP_WED(WED_WDMA_RX_THRES(1)),
		DUMP_WED(WED_WDMA_RX_RECYCLE_MIB(1)),
		DUMP_WED(WED_WDMA_RX_PROCESSED_MIB(1)),

		DUMP_STR("WDMA RX"),
		DUMP_WDMA(WDMA_GLO_CFG),
		DUMP_WDMA_RING(WDMA_RING_RX(0)),
		DUMP_WDMA_RING(WDMA_RING_RX(1)),

		DUMP_STR("WED TX FREE"),
		DUMP_WED(WED_RX_MIB(0)),
		DUMP_WED_RING(WED_RING_RX(0)),
		DUMP_WED(WED_WPDMA_RX_COHERENT_MIB(0)),
		DUMP_WED(WED_RX_MIB(1)),
		DUMP_WED_RING(WED_RING_RX(1)),
		DUMP_WED(WED_WPDMA_RX_COHERENT_MIB(1)),

		DUMP_STR("WED WPDMA TX FREE"),
		DUMP_WED_RING(WED_WPDMA_RING_RX(0)),
		DUMP_WED_RING(WED_WPDMA_RING_RX(1)),
	};
	struct mtk_wed_hw *hw = s->private;
	struct mtk_wed_device *dev = hw->wed_dev;

	if (dev)
		dump_wed_regs(s, dev, regs, ARRAY_SIZE(regs));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(wed_txinfo);

static int
wed_rxinfo_show(struct seq_file *s, void *data)
{
	static const struct reg_dump regs[] = {
		DUMP_STR("WPDMA RX"),
		DUMP_WPDMA_RX_RING(0),
		DUMP_WPDMA_RX_RING(1),

		DUMP_STR("WPDMA RX"),
		DUMP_WED(WED_WPDMA_RX_D_MIB(0)),
		DUMP_WED_RING(WED_WPDMA_RING_RX_DATA(0)),
		DUMP_WED(WED_WPDMA_RX_D_PROCESSED_MIB(0)),
		DUMP_WED(WED_WPDMA_RX_D_MIB(1)),
		DUMP_WED_RING(WED_WPDMA_RING_RX_DATA(1)),
		DUMP_WED(WED_WPDMA_RX_D_PROCESSED_MIB(1)),
		DUMP_WED(WED_WPDMA_RX_D_COHERENT_MIB),

		DUMP_STR("WED RX"),
		DUMP_WED_RING(WED_RING_RX_DATA(0)),
		DUMP_WED_RING(WED_RING_RX_DATA(1)),

		DUMP_STR("WED RRO"),
		DUMP_WED_RRO_RING(WED_RROQM_MIOD_CTRL0),
		DUMP_WED(WED_RROQM_MID_MIB),
		DUMP_WED(WED_RROQM_MOD_MIB),
		DUMP_WED(WED_RROQM_MOD_COHERENT_MIB),
		DUMP_WED_RRO_FDBK(WED_RROQM_FDBK_CTRL0),
		DUMP_WED(WED_RROQM_FDBK_IND_MIB),
		DUMP_WED(WED_RROQM_FDBK_ENQ_MIB),
		DUMP_WED(WED_RROQM_FDBK_ANC_MIB),
		DUMP_WED(WED_RROQM_FDBK_ANC2H_MIB),

		DUMP_STR("WED Route QM"),
		DUMP_WED(WED_RTQM_R2H_MIB(0)),
		DUMP_WED(WED_RTQM_R2Q_MIB(0)),
		DUMP_WED(WED_RTQM_Q2H_MIB(0)),
		DUMP_WED(WED_RTQM_R2H_MIB(1)),
		DUMP_WED(WED_RTQM_R2Q_MIB(1)),
		DUMP_WED(WED_RTQM_Q2H_MIB(1)),
		DUMP_WED(WED_RTQM_Q2N_MIB),
		DUMP_WED(WED_RTQM_Q2B_MIB),
		DUMP_WED(WED_RTQM_PFDBK_MIB),

		DUMP_STR("WED WDMA TX"),
		DUMP_WED(WED_WDMA_TX_MIB),
		DUMP_WED_RING(WED_WDMA_RING_TX),

		DUMP_STR("WDMA TX"),
		DUMP_WDMA(WDMA_GLO_CFG),
		DUMP_WDMA_RING(WDMA_RING_TX(0)),
		DUMP_WDMA_RING(WDMA_RING_TX(1)),

		DUMP_STR("WED RX BM"),
		DUMP_WED(WED_RX_BM_BASE),
		DUMP_WED(WED_RX_BM_RX_DMAD),
		DUMP_WED(WED_RX_BM_PTR),
		DUMP_WED(WED_RX_BM_TKID_MIB),
		DUMP_WED(WED_RX_BM_BLEN),
		DUMP_WED(WED_RX_BM_STS),
		DUMP_WED(WED_RX_BM_INTF2),
		DUMP_WED(WED_RX_BM_INTF),
		DUMP_WED(WED_RX_BM_ERR_STS),
	};
	struct mtk_wed_hw *hw = s->private;
	struct mtk_wed_device *dev = hw->wed_dev;

	if (dev)
		dump_wed_regs(s, dev, regs, ARRAY_SIZE(regs));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(wed_rxinfo);

static int
mtk_wed_reg_set(void *data, u64 val)
{
	struct mtk_wed_hw *hw = data;

	regmap_write(hw->regs, hw->debugfs_reg, val);

	return 0;
}

static int
mtk_wed_reg_get(void *data, u64 *val)
{
	struct mtk_wed_hw *hw = data;
	unsigned int regval;
	int ret;

	ret = regmap_read(hw->regs, hw->debugfs_reg, &regval);
	if (ret)
		return ret;

	*val = regval;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_regval, mtk_wed_reg_get, mtk_wed_reg_set,
             "0x%08llx\n");

void mtk_wed_hw_add_debugfs(struct mtk_wed_hw *hw)
{
	struct dentry *dir;

	snprintf(hw->dirname, sizeof(hw->dirname), "wed%d", hw->index);
	dir = debugfs_create_dir(hw->dirname, NULL);

	hw->debugfs_dir = dir;
	debugfs_create_u32("regidx", 0600, dir, &hw->debugfs_reg);
	debugfs_create_file_unsafe("regval", 0600, dir, hw, &fops_regval);
	debugfs_create_file_unsafe("txinfo", 0400, dir, hw, &wed_txinfo_fops);
	if (hw->version != 1)
		debugfs_create_file_unsafe("rxinfo", 0400, dir, hw,
					   &wed_rxinfo_fops);
}
