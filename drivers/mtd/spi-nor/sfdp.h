/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2005, Intec Automation Inc.
 * Copyright (C) 2014, Freescale Semiconductor, Inc.
 */

#ifndef __LINUX_MTD_SFDP_H
#define __LINUX_MTD_SFDP_H

/* SFDP revisions */
#define SFDP_JESD216_MAJOR	1
#define SFDP_JESD216_MINOR	0
#define SFDP_JESD216A_MINOR	5
#define SFDP_JESD216B_MINOR	6

/* SFDP DWORDS are indexed from 1 but C arrays are indexed from 0. */
#define SFDP_DWORD(i)		((i) - 1)
#define SFDP_MASK_CHECK(dword, mask)		(((dword) & (mask)) == (mask))

/* Basic Flash Parameter Table */

/* JESD216 rev D defines a Basic Flash Parameter Table of 20 DWORDs. */
#define BFPT_DWORD_MAX		20

struct sfdp_bfpt {
	u32	dwords[BFPT_DWORD_MAX];
};

/* The first version of JESD216 defined only 9 DWORDs. */
#define BFPT_DWORD_MAX_JESD216			9
#define BFPT_DWORD_MAX_JESD216B			16

/* 1st DWORD. */
#define BFPT_DWORD1_FAST_READ_1_1_2		BIT(16)
#define BFPT_DWORD1_ADDRESS_BYTES_MASK		GENMASK(18, 17)
#define BFPT_DWORD1_ADDRESS_BYTES_3_ONLY	(0x0UL << 17)
#define BFPT_DWORD1_ADDRESS_BYTES_3_OR_4	(0x1UL << 17)
#define BFPT_DWORD1_ADDRESS_BYTES_4_ONLY	(0x2UL << 17)
#define BFPT_DWORD1_DTR				BIT(19)
#define BFPT_DWORD1_FAST_READ_1_2_2		BIT(20)
#define BFPT_DWORD1_FAST_READ_1_4_4		BIT(21)
#define BFPT_DWORD1_FAST_READ_1_1_4		BIT(22)

/* 5th DWORD. */
#define BFPT_DWORD5_FAST_READ_2_2_2		BIT(0)
#define BFPT_DWORD5_FAST_READ_4_4_4		BIT(4)

/* 11th DWORD. */
#define BFPT_DWORD11_PAGE_SIZE_SHIFT		4
#define BFPT_DWORD11_PAGE_SIZE_MASK		GENMASK(7, 4)

/* 15th DWORD. */

/*
 * (from JESD216 rev B)
 * Quad Enable Requirements (QER):
 * - 000b: Device does not have a QE bit. Device detects 1-1-4 and 1-4-4
 *         reads based on instruction. DQ3/HOLD# functions are hold during
 *         instruction phase.
 * - 001b: QE is bit 1 of status register 2. It is set via Write Status with
 *         two data bytes where bit 1 of the second byte is one.
 *         [...]
 *         Writing only one byte to the status register has the side-effect of
 *         clearing status register 2, including the QE bit. The 100b code is
 *         used if writing one byte to the status register does not modify
 *         status register 2.
 * - 010b: QE is bit 6 of status register 1. It is set via Write Status with
 *         one data byte where bit 6 is one.
 *         [...]
 * - 011b: QE is bit 7 of status register 2. It is set via Write status
 *         register 2 instruction 3Eh with one data byte where bit 7 is one.
 *         [...]
 *         The status register 2 is read using instruction 3Fh.
 * - 100b: QE is bit 1 of status register 2. It is set via Write Status with
 *         two data bytes where bit 1 of the second byte is one.
 *         [...]
 *         In contrast to the 001b code, writing one byte to the status
 *         register does not modify status register 2.
 * - 101b: QE is bit 1 of status register 2. Status register 1 is read using
 *         Read Status instruction 05h. Status register2 is read using
 *         instruction 35h. QE is set via Write Status instruction 01h with
 *         two data bytes where bit 1 of the second byte is one.
 *         [...]
 */
#define BFPT_DWORD15_QER_MASK			GENMASK(22, 20)
#define BFPT_DWORD15_QER_NONE			(0x0UL << 20) /* Micron */
#define BFPT_DWORD15_QER_SR2_BIT1_BUGGY		(0x1UL << 20)
#define BFPT_DWORD15_QER_SR1_BIT6		(0x2UL << 20) /* Macronix */
#define BFPT_DWORD15_QER_SR2_BIT7		(0x3UL << 20)
#define BFPT_DWORD15_QER_SR2_BIT1_NO_RD		(0x4UL << 20)
#define BFPT_DWORD15_QER_SR2_BIT1		(0x5UL << 20) /* Spansion */

#define BFPT_DWORD16_EN4B_MASK			GENMASK(31, 24)
#define BFPT_DWORD16_EN4B_ALWAYS_4B		BIT(30)
#define BFPT_DWORD16_EN4B_4B_OPCODES		BIT(29)
#define BFPT_DWORD16_EN4B_16BIT_NV_CR		BIT(28)
#define BFPT_DWORD16_EN4B_BRWR			BIT(27)
#define BFPT_DWORD16_EN4B_WREAR			BIT(26)
#define BFPT_DWORD16_EN4B_WREN_EN4B		BIT(25)
#define BFPT_DWORD16_EN4B_EN4B			BIT(24)
#define BFPT_DWORD16_EX4B_MASK			GENMASK(18, 14)
#define BFPT_DWORD16_EX4B_16BIT_NV_CR		BIT(18)
#define BFPT_DWORD16_EX4B_BRWR			BIT(17)
#define BFPT_DWORD16_EX4B_WREAR			BIT(16)
#define BFPT_DWORD16_EX4B_WREN_EX4B		BIT(15)
#define BFPT_DWORD16_EX4B_EX4B			BIT(14)
#define BFPT_DWORD16_4B_ADDR_MODE_MASK			\
	(BFPT_DWORD16_EN4B_MASK | BFPT_DWORD16_EX4B_MASK)
#define BFPT_DWORD16_4B_ADDR_MODE_16BIT_NV_CR		\
	(BFPT_DWORD16_EN4B_16BIT_NV_CR | BFPT_DWORD16_EX4B_16BIT_NV_CR)
#define BFPT_DWORD16_4B_ADDR_MODE_BRWR			\
	(BFPT_DWORD16_EN4B_BRWR | BFPT_DWORD16_EX4B_BRWR)
#define BFPT_DWORD16_4B_ADDR_MODE_WREAR			\
	(BFPT_DWORD16_EN4B_WREAR | BFPT_DWORD16_EX4B_WREAR)
#define BFPT_DWORD16_4B_ADDR_MODE_WREN_EN4B_EX4B	\
	(BFPT_DWORD16_EN4B_WREN_EN4B | BFPT_DWORD16_EX4B_WREN_EX4B)
#define BFPT_DWORD16_4B_ADDR_MODE_EN4B_EX4B		\
	(BFPT_DWORD16_EN4B_EN4B | BFPT_DWORD16_EX4B_EX4B)
#define BFPT_DWORD16_SWRST_EN_RST		BIT(12)

#define BFPT_DWORD18_CMD_EXT_MASK		GENMASK(30, 29)
#define BFPT_DWORD18_CMD_EXT_REP		(0x0UL << 29) /* Repeat */
#define BFPT_DWORD18_CMD_EXT_INV		(0x1UL << 29) /* Invert */
#define BFPT_DWORD18_CMD_EXT_RES		(0x2UL << 29) /* Reserved */
#define BFPT_DWORD18_CMD_EXT_16B		(0x3UL << 29) /* 16-bit opcode */

struct sfdp_parameter_header {
	u8		id_lsb;
	u8		minor;
	u8		major;
	u8		length; /* in double words */
	u8		parameter_table_pointer[3]; /* byte address */
	u8		id_msb;
};

#endif /* __LINUX_MTD_SFDP_H */
