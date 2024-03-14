/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018 MediaTek Inc.
 * Author: Owen Chen <owen.chen@mediatek.com>
 */

#ifndef __DRV_CLK_MTK_MUX_H
#define __DRV_CLK_MTK_MUX_H

#include <linux/notifier.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct clk;
struct clk_hw_onecell_data;
struct clk_ops;
struct device;
struct device_node;

struct mtk_mux {
	int id;
	const char *name;
	const char * const *parent_names;
	unsigned int flags;

	u32 mux_ofs;
	u32 set_ofs;
	u32 clr_ofs;
	u32 upd_ofs;

	u8 mux_shift;
	u8 mux_width;
	u8 gate_shift;
	s8 upd_shift;

	const struct clk_ops *ops;
	signed char num_parents;
};

#define GATE_CLR_SET_UPD_FLAGS(_id, _name, _parents, _mux_ofs,		\
			_mux_set_ofs, _mux_clr_ofs, _shift, _width,	\
			_gate, _upd_ofs, _upd, _flags, _ops) {		\
		.id = _id,						\
		.name = _name,						\
		.mux_ofs = _mux_ofs,					\
		.set_ofs = _mux_set_ofs,				\
		.clr_ofs = _mux_clr_ofs,				\
		.upd_ofs = _upd_ofs,					\
		.mux_shift = _shift,					\
		.mux_width = _width,					\
		.gate_shift = _gate,					\
		.upd_shift = _upd,					\
		.parent_names = _parents,				\
		.num_parents = ARRAY_SIZE(_parents),			\
		.flags = _flags,					\
		.ops = &_ops,						\
	}

extern const struct clk_ops mtk_mux_clr_set_upd_ops;
extern const struct clk_ops mtk_mux_gate_clr_set_upd_ops;

#define MUX_GATE_CLR_SET_UPD_FLAGS(_id, _name, _parents, _mux_ofs,	\
			_mux_set_ofs, _mux_clr_ofs, _shift, _width,	\
			_gate, _upd_ofs, _upd, _flags)			\
		GATE_CLR_SET_UPD_FLAGS(_id, _name, _parents, _mux_ofs,	\
			_mux_set_ofs, _mux_clr_ofs, _shift, _width,	\
			_gate, _upd_ofs, _upd, _flags,			\
			mtk_mux_gate_clr_set_upd_ops)

#define MUX_GATE_CLR_SET_UPD(_id, _name, _parents, _mux_ofs,		\
			_mux_set_ofs, _mux_clr_ofs, _shift, _width,	\
			_gate, _upd_ofs, _upd)				\
		MUX_GATE_CLR_SET_UPD_FLAGS(_id, _name, _parents,	\
			_mux_ofs, _mux_set_ofs, _mux_clr_ofs, _shift,	\
			_width, _gate, _upd_ofs, _upd,			\
			CLK_SET_RATE_PARENT)

#define MUX_CLR_SET_UPD(_id, _name, _parents, _mux_ofs,			\
			_mux_set_ofs, _mux_clr_ofs, _shift, _width,	\
			_upd_ofs, _upd)					\
		GATE_CLR_SET_UPD_FLAGS(_id, _name, _parents, _mux_ofs,	\
			_mux_set_ofs, _mux_clr_ofs, _shift, _width,	\
			0, _upd_ofs, _upd, CLK_SET_RATE_PARENT,		\
			mtk_mux_clr_set_upd_ops)

int mtk_clk_register_muxes(struct device *dev,
			   const struct mtk_mux *muxes,
			   int num, struct device_node *node,
			   spinlock_t *lock,
			   struct clk_hw_onecell_data *clk_data);

void mtk_clk_unregister_muxes(const struct mtk_mux *muxes, int num,
			      struct clk_hw_onecell_data *clk_data);

struct mtk_mux_nb {
	struct notifier_block	nb;
	const struct clk_ops	*ops;

	u8	bypass_index;	/* Which parent to temporarily use */
	u8	original_index;	/* Set by notifier callback */
};

#define to_mtk_mux_nb(_nb)	container_of(_nb, struct mtk_mux_nb, nb)

int devm_mtk_clk_mux_notifier_register(struct device *dev, struct clk *clk,
				       struct mtk_mux_nb *mux_nb);

#endif /* __DRV_CLK_MTK_MUX_H */
