// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2005, Intec Automation Inc.
 * Copyright (C) 2014, Freescale Semiconductor, Inc.
 */

#include <linux/mtd/spi-nor.h>

#include "core.h"

static const struct flash_info esmt_nor_parts[] = {
	/* ESMT */
	{ "f25l32pa", INFO(0x8c2016, 0, 64 * 1024, 64)
		FLAGS(SPI_NOR_HAS_LOCK | SPI_NOR_SWP_IS_VOLATILE)
		NO_SFDP_FLAGS(SECT_4K) },
	{ "f25l32qa-2s", INFO(0x8c4116, 0, 64 * 1024, 64)
		FLAGS(SPI_NOR_HAS_LOCK)
		NO_SFDP_FLAGS(SECT_4K) },
	{ "f25l64qa", INFO(0x8c4117, 0, 64 * 1024, 128)
		FLAGS(SPI_NOR_HAS_LOCK)
		NO_SFDP_FLAGS(SECT_4K) },
};

const struct spi_nor_manufacturer spi_nor_esmt = {
	.name = "esmt",
	.parts = esmt_nor_parts,
	.nparts = ARRAY_SIZE(esmt_nor_parts),
};
