/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RZ/G2L Clock Pulse Generator
 *
 * Copyright (C) 2021 Renesas Electronics Corp.
 *
 */

#ifndef __RENESAS_RZG2L_CPG_H__
#define __RENESAS_RZG2L_CPG_H__

#define CPG_SIPLL5_STBY		(0x140)
#define CPG_SIPLL5_CLK1		(0x144)
#define CPG_SIPLL5_CLK3		(0x14C)
#define CPG_SIPLL5_CLK4		(0x150)
#define CPG_SIPLL5_CLK5		(0x154)
#define CPG_SIPLL5_MON		(0x15C)
#define CPG_PL1_DDIV		(0x200)
#define CPG_PL2_DDIV		(0x204)
#define CPG_PL3A_DDIV		(0x208)
#define CPG_PL6_DDIV		(0x210)
#define CPG_PL2SDHI_DSEL	(0x218)
#define CPG_CLKSTATUS		(0x280)
#define CPG_PL3_SSEL		(0x408)
#define CPG_PL6_SSEL		(0x414)
#define CPG_PL6_ETH_SSEL	(0x418)
#define CPG_PL5_SDIV		(0x420)
#define CPG_RST_MON		(0x680)
#define CPG_OTHERFUNC1_REG	(0xBE8)

#define CPG_SIPLL5_STBY_RESETB		BIT(0)
#define CPG_SIPLL5_STBY_RESETB_WEN	BIT(16)
#define CPG_SIPLL5_STBY_SSCG_EN_WEN	BIT(18)
#define CPG_SIPLL5_STBY_DOWNSPREAD_WEN	BIT(20)
#define CPG_SIPLL5_CLK4_RESV_LSB	(0xFF)
#define CPG_SIPLL5_MON_PLL5_LOCK	BIT(4)

#define CPG_OTHERFUNC1_REG_RES0_ON_WEN	BIT(16)

#define CPG_PL5_SDIV_DIV_DSI_A_WEN	BIT(16)
#define CPG_PL5_SDIV_DIV_DSI_B_WEN	BIT(24)

#define CPG_CLKSTATUS_SELSDHI0_STS	BIT(28)
#define CPG_CLKSTATUS_SELSDHI1_STS	BIT(29)

#define CPG_SDHI_CLK_SWITCH_STATUS_TIMEOUT_US	20000

/* n = 0/1/2 for PLL1/4/6 */
#define CPG_SAMPLL_CLK1(n)	(0x04 + (16 * n))
#define CPG_SAMPLL_CLK2(n)	(0x08 + (16 * n))

#define PLL146_CONF(n)	(CPG_SAMPLL_CLK1(n) << 22 | CPG_SAMPLL_CLK2(n) << 12)

#define DDIV_PACK(offset, bitpos, size) \
		(((offset) << 20) | ((bitpos) << 12) | ((size) << 8))
#define DIVPL1A		DDIV_PACK(CPG_PL1_DDIV, 0, 2)
#define DIVPL2A		DDIV_PACK(CPG_PL2_DDIV, 0, 3)
#define DIVDSILPCLK	DDIV_PACK(CPG_PL2_DDIV, 12, 2)
#define DIVPL3A		DDIV_PACK(CPG_PL3A_DDIV, 0, 3)
#define DIVPL3B		DDIV_PACK(CPG_PL3A_DDIV, 4, 3)
#define DIVPL3C		DDIV_PACK(CPG_PL3A_DDIV, 8, 3)
#define DIVGPU		DDIV_PACK(CPG_PL6_DDIV, 0, 2)

#define SEL_PLL_PACK(offset, bitpos, size) \
		(((offset) << 20) | ((bitpos) << 12) | ((size) << 8))

#define SEL_PLL3_3	SEL_PLL_PACK(CPG_PL3_SSEL, 8, 1)
#define SEL_PLL5_4	SEL_PLL_PACK(CPG_OTHERFUNC1_REG, 0, 1)
#define SEL_PLL6_2	SEL_PLL_PACK(CPG_PL6_ETH_SSEL, 0, 1)
#define SEL_GPU2	SEL_PLL_PACK(CPG_PL6_SSEL, 12, 1)

#define SEL_SDHI0	DDIV_PACK(CPG_PL2SDHI_DSEL, 0, 2)
#define SEL_SDHI1	DDIV_PACK(CPG_PL2SDHI_DSEL, 4, 2)

#define EXTAL_FREQ_IN_MEGA_HZ	(24)

/**
 * Definitions of CPG Core Clocks
 *
 * These include:
 *   - Clock outputs exported to DT
 *   - External input clocks
 *   - Internal CPG clocks
 */
struct cpg_core_clk {
	const char *name;
	unsigned int id;
	unsigned int parent;
	unsigned int div;
	unsigned int mult;
	unsigned int type;
	unsigned int conf;
	const struct clk_div_table *dtable;
	const char * const *parent_names;
	int flag;
	int mux_flags;
	int num_parents;
};

enum clk_types {
	/* Generic */
	CLK_TYPE_IN,		/* External Clock Input */
	CLK_TYPE_FF,		/* Fixed Factor Clock */
	CLK_TYPE_SAM_PLL,

	/* Clock with divider */
	CLK_TYPE_DIV,

	/* Clock with clock source selector */
	CLK_TYPE_MUX,

	/* Clock with SD clock source selector */
	CLK_TYPE_SD_MUX,

	/* Clock for SIPLL5 */
	CLK_TYPE_SIPLL5,

	/* Clock for PLL5_4 clock source selector */
	CLK_TYPE_PLL5_4_MUX,

	/* Clock for DSI divider */
	CLK_TYPE_DSI_DIV,

};

#define DEF_TYPE(_name, _id, _type...) \
	{ .name = _name, .id = _id, .type = _type }
#define DEF_BASE(_name, _id, _type, _parent...) \
	DEF_TYPE(_name, _id, _type, .parent = _parent)
#define DEF_SAMPLL(_name, _id, _parent, _conf) \
	DEF_TYPE(_name, _id, CLK_TYPE_SAM_PLL, .parent = _parent, .conf = _conf)
#define DEF_INPUT(_name, _id) \
	DEF_TYPE(_name, _id, CLK_TYPE_IN)
#define DEF_FIXED(_name, _id, _parent, _mult, _div) \
	DEF_BASE(_name, _id, CLK_TYPE_FF, _parent, .div = _div, .mult = _mult)
#define DEF_DIV(_name, _id, _parent, _conf, _dtable) \
	DEF_TYPE(_name, _id, CLK_TYPE_DIV, .conf = _conf, \
		 .parent = _parent, .dtable = _dtable, \
		 .flag = CLK_DIVIDER_HIWORD_MASK)
#define DEF_DIV_RO(_name, _id, _parent, _conf, _dtable) \
	DEF_TYPE(_name, _id, CLK_TYPE_DIV, .conf = _conf, \
		 .parent = _parent, .dtable = _dtable, \
		 .flag = CLK_DIVIDER_READ_ONLY)
#define DEF_MUX(_name, _id, _conf, _parent_names) \
	DEF_TYPE(_name, _id, CLK_TYPE_MUX, .conf = _conf, \
		 .parent_names = _parent_names, \
		 .num_parents = ARRAY_SIZE(_parent_names), \
		 .mux_flags = CLK_MUX_HIWORD_MASK)
#define DEF_MUX_RO(_name, _id, _conf, _parent_names) \
	DEF_TYPE(_name, _id, CLK_TYPE_MUX, .conf = _conf, \
		 .parent_names = _parent_names, \
		 .num_parents = ARRAY_SIZE(_parent_names), \
		 .mux_flags = CLK_MUX_READ_ONLY)
#define DEF_SD_MUX(_name, _id, _conf, _parent_names) \
	DEF_TYPE(_name, _id, CLK_TYPE_SD_MUX, .conf = _conf, \
		 .parent_names = _parent_names, \
		 .num_parents = ARRAY_SIZE(_parent_names))
#define DEF_PLL5_FOUTPOSTDIV(_name, _id, _parent) \
	DEF_TYPE(_name, _id, CLK_TYPE_SIPLL5, .parent = _parent)
#define DEF_PLL5_4_MUX(_name, _id, _conf, _parent_names) \
	DEF_TYPE(_name, _id, CLK_TYPE_PLL5_4_MUX, .conf = _conf, \
		 .parent_names = _parent_names, \
		 .num_parents = ARRAY_SIZE(_parent_names))
#define DEF_DSI_DIV(_name, _id, _parent, _flag) \
	DEF_TYPE(_name, _id, CLK_TYPE_DSI_DIV, .parent = _parent, .flag = _flag)

/**
 * struct rzg2l_mod_clk - Module Clocks definitions
 *
 * @name: handle between common and hardware-specific interfaces
 * @id: clock index in array containing all Core and Module Clocks
 * @parent: id of parent clock
 * @off: register offset
 * @bit: ON/MON bit
 * @is_coupled: flag to indicate coupled clock
 */
struct rzg2l_mod_clk {
	const char *name;
	unsigned int id;
	unsigned int parent;
	u16 off;
	u8 bit;
	bool is_coupled;
};

#define DEF_MOD_BASE(_name, _id, _parent, _off, _bit, _is_coupled)	\
	{ \
		.name = _name, \
		.id = MOD_CLK_BASE + (_id), \
		.parent = (_parent), \
		.off = (_off), \
		.bit = (_bit), \
		.is_coupled = (_is_coupled), \
	}

#define DEF_MOD(_name, _id, _parent, _off, _bit)	\
	DEF_MOD_BASE(_name, _id, _parent, _off, _bit, false)

#define DEF_COUPLED(_name, _id, _parent, _off, _bit)	\
	DEF_MOD_BASE(_name, _id, _parent, _off, _bit, true)

/**
 * struct rzg2l_reset - Reset definitions
 *
 * @off: register offset
 * @bit: reset bit
 * @monbit: monitor bit in CPG_RST_MON register, -1 if none
 */
struct rzg2l_reset {
	u16 off;
	u8 bit;
	s8 monbit;
};

#define DEF_RST_MON(_id, _off, _bit, _monbit)	\
	[_id] = { \
		.off = (_off), \
		.bit = (_bit), \
		.monbit = (_monbit) \
	}
#define DEF_RST(_id, _off, _bit)	\
	DEF_RST_MON(_id, _off, _bit, -1)

/**
 * struct rzg2l_cpg_info - SoC-specific CPG Description
 *
 * @core_clks: Array of Core Clock definitions
 * @num_core_clks: Number of entries in core_clks[]
 * @last_dt_core_clk: ID of the last Core Clock exported to DT
 * @num_total_core_clks: Total number of Core Clocks (exported + internal)
 *
 * @mod_clks: Array of Module Clock definitions
 * @num_mod_clks: Number of entries in mod_clks[]
 * @num_hw_mod_clks: Number of Module Clocks supported by the hardware
 *
 * @resets: Array of Module Reset definitions
 * @num_resets: Number of entries in resets[]
 *
 * @crit_mod_clks: Array with Module Clock IDs of critical clocks that
 *                 should not be disabled without a knowledgeable driver
 * @num_crit_mod_clks: Number of entries in crit_mod_clks[]
 * @has_clk_mon_regs: Flag indicating whether the SoC has CLK_MON registers
 */
struct rzg2l_cpg_info {
	/* Core Clocks */
	const struct cpg_core_clk *core_clks;
	unsigned int num_core_clks;
	unsigned int last_dt_core_clk;
	unsigned int num_total_core_clks;

	/* Module Clocks */
	const struct rzg2l_mod_clk *mod_clks;
	unsigned int num_mod_clks;
	unsigned int num_hw_mod_clks;

	/* No PM Module Clocks */
	const unsigned int *no_pm_mod_clks;
	unsigned int num_no_pm_mod_clks;

	/* Resets */
	const struct rzg2l_reset *resets;
	unsigned int num_resets;

	/* Critical Module Clocks that should not be disabled */
	const unsigned int *crit_mod_clks;
	unsigned int num_crit_mod_clks;

	bool has_clk_mon_regs;
};

extern const struct rzg2l_cpg_info r9a07g043_cpg_info;
extern const struct rzg2l_cpg_info r9a07g044_cpg_info;
extern const struct rzg2l_cpg_info r9a07g054_cpg_info;
extern const struct rzg2l_cpg_info r9a09g011_cpg_info;

#endif
