// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019-2021 Linaro Ltd.
 */

#include <linux/log2.h>

#include "../gsi.h"
#include "../ipa_data.h"
#include "../ipa_endpoint.h"
#include "../ipa_mem.h"

/** enum ipa_resource_type - IPA resource types for an SoC having IPA v3.1 */
enum ipa_resource_type {
	/* Source resource types; first must have value 0 */
	IPA_RESOURCE_TYPE_SRC_PKT_CONTEXTS		= 0,
	IPA_RESOURCE_TYPE_SRC_HDR_SECTORS,
	IPA_RESOURCE_TYPE_SRC_HDRI1_BUFFER,
	IPA_RESOURCE_TYPE_SRC_DESCRIPTOR_LISTS,
	IPA_RESOURCE_TYPE_SRC_DESCRIPTOR_BUFF,
	IPA_RESOURCE_TYPE_SRC_HDRI2_BUFFERS,
	IPA_RESOURCE_TYPE_SRC_HPS_DMARS,
	IPA_RESOURCE_TYPE_SRC_ACK_ENTRIES,

	/* Destination resource types; first must have value 0 */
	IPA_RESOURCE_TYPE_DST_DATA_SECTORS		= 0,
	IPA_RESOURCE_TYPE_DST_DATA_SECTOR_LISTS,
	IPA_RESOURCE_TYPE_DST_DPS_DMARS,
};

/* Resource groups used for an SoC having IPA v3.1 */
enum ipa_rsrc_group_id {
	/* Source resource group identifiers */
	IPA_RSRC_GROUP_SRC_UL		= 0,
	IPA_RSRC_GROUP_SRC_DL,
	IPA_RSRC_GROUP_SRC_DIAG,
	IPA_RSRC_GROUP_SRC_DMA,
	IPA_RSRC_GROUP_SRC_UNUSED,
	IPA_RSRC_GROUP_SRC_UC_RX_Q,
	IPA_RSRC_GROUP_SRC_COUNT,	/* Last in set; not a source group */

	/* Destination resource group identifiers */
	IPA_RSRC_GROUP_DST_UL		= 0,
	IPA_RSRC_GROUP_DST_DL,
	IPA_RSRC_GROUP_DST_DIAG_DPL,
	IPA_RSRC_GROUP_DST_DMA,
	IPA_RSRC_GROUP_DST_Q6ZIP_GENERAL,
	IPA_RSRC_GROUP_DST_Q6ZIP_ENGINE,
	IPA_RSRC_GROUP_DST_COUNT,	/* Last; not a destination group */
};

/* QSB configuration data for an SoC having IPA v3.1 */
static const struct ipa_qsb_data ipa_qsb_data[] = {
	[IPA_QSB_MASTER_DDR] = {
		.max_writes	= 8,
		.max_reads	= 8,
	},
	[IPA_QSB_MASTER_PCIE] = {
		.max_writes	= 2,
		.max_reads	= 8,
	},
};

/* Endpoint data for an SoC having IPA v3.1 */
static const struct ipa_gsi_endpoint_data ipa_gsi_endpoint_data[] = {
	[IPA_ENDPOINT_AP_COMMAND_TX] = {
		.ee_id		= GSI_EE_AP,
		.channel_id	= 6,
		.endpoint_id	= 22,
		.toward_ipa	= true,
		.channel = {
			.tre_count	= 256,
			.event_count	= 256,
			.tlv_count	= 18,
		},
		.endpoint = {
			.config = {
				.resource_group	= IPA_RSRC_GROUP_SRC_UL,
				.dma_mode	= true,
				.dma_endpoint	= IPA_ENDPOINT_AP_LAN_RX,
				.tx = {
					.seq_type = IPA_SEQ_DMA,
				},
			},
		},
	},
	[IPA_ENDPOINT_AP_LAN_RX] = {
		.ee_id		= GSI_EE_AP,
		.channel_id	= 7,
		.endpoint_id	= 15,
		.toward_ipa	= false,
		.channel = {
			.tre_count	= 256,
			.event_count	= 256,
			.tlv_count	= 8,
		},
		.endpoint = {
			.config = {
				.resource_group	= IPA_RSRC_GROUP_SRC_UL,
				.aggregation	= true,
				.status_enable	= true,
				.rx = {
					.buffer_size	= 8192,
					.pad_align	= ilog2(sizeof(u32)),
					.aggr_time_limit = 500,
				},
			},
		},
	},
	[IPA_ENDPOINT_AP_MODEM_TX] = {
		.ee_id		= GSI_EE_AP,
		.channel_id	= 5,
		.endpoint_id	= 3,
		.toward_ipa	= true,
		.channel = {
			.tre_count	= 512,
			.event_count	= 512,
			.tlv_count	= 16,
		},
		.endpoint = {
			.filter_support	= true,
			.config = {
				.resource_group	= IPA_RSRC_GROUP_SRC_UL,
				.checksum	= true,
				.qmap		= true,
				.status_enable	= true,
				.tx = {
					.seq_type = IPA_SEQ_2_PASS_SKIP_LAST_UC,
					.status_endpoint =
						IPA_ENDPOINT_MODEM_AP_RX,
				},
			},
		},
	},
	[IPA_ENDPOINT_AP_MODEM_RX] = {
		.ee_id		= GSI_EE_AP,
		.channel_id	= 8,
		.endpoint_id	= 16,
		.toward_ipa	= false,
		.channel = {
			.tre_count	= 256,
			.event_count	= 256,
			.tlv_count	= 8,
		},
		.endpoint = {
			.config = {
				.resource_group	= IPA_RSRC_GROUP_DST_DL,
				.checksum	= true,
				.qmap		= true,
				.aggregation	= true,
				.rx = {
					.buffer_size	= 8192,
					.aggr_time_limit = 500,
					.aggr_close_eof	= true,
				},
			},
		},
	},
	[IPA_ENDPOINT_MODEM_LAN_TX] = {
		.ee_id		= GSI_EE_MODEM,
		.channel_id	= 4,
		.endpoint_id	= 9,
		.toward_ipa	= true,
		.endpoint = {
			.filter_support	= true,
		},
	},
	[IPA_ENDPOINT_MODEM_AP_TX] = {
		.ee_id		= GSI_EE_MODEM,
		.channel_id	= 0,
		.endpoint_id	= 5,
		.toward_ipa	= true,
		.endpoint = {
			.filter_support	= true,
		},
	},
	[IPA_ENDPOINT_MODEM_AP_RX] = {
		.ee_id		= GSI_EE_MODEM,
		.channel_id	= 5,
		.endpoint_id	= 18,
		.toward_ipa	= false,
	},
};

/* Source resource configuration data for an SoC having IPA v3.1 */
static const struct ipa_resource ipa_resource_src[] = {
	[IPA_RESOURCE_TYPE_SRC_PKT_CONTEXTS] = {
		.limits[IPA_RSRC_GROUP_SRC_UL] = {
			.min = 3,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_DL] = {
			.min = 3,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_DIAG] = {
			.min = 1,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_DMA] = {
			.min = 1,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_UC_RX_Q] = {
			.min = 2,	.max = 255,
		},
	},
	[IPA_RESOURCE_TYPE_SRC_HDR_SECTORS] = {
		.limits[IPA_RSRC_GROUP_SRC_UL] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_DL] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_DIAG] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_DMA] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_UC_RX_Q] = {
			.min = 0,	.max = 255,
		},
	},
	[IPA_RESOURCE_TYPE_SRC_HDRI1_BUFFER] = {
		.limits[IPA_RSRC_GROUP_SRC_UL] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_DL] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_DIAG] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_DMA] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_UC_RX_Q] = {
			.min = 0,	.max = 255,
		},
	},
	[IPA_RESOURCE_TYPE_SRC_DESCRIPTOR_LISTS] = {
		.limits[IPA_RSRC_GROUP_SRC_UL] = {
			.min = 14,	.max = 14,
		},
		.limits[IPA_RSRC_GROUP_SRC_DL] = {
			.min = 16,	.max = 16,
		},
		.limits[IPA_RSRC_GROUP_SRC_DIAG] = {
			.min = 5,	.max = 5,
		},
		.limits[IPA_RSRC_GROUP_SRC_DMA] = {
			.min = 5,	.max = 5,
		},
		.limits[IPA_RSRC_GROUP_SRC_UC_RX_Q] = {
			.min = 8,	.max = 8,
		},
	},
	[IPA_RESOURCE_TYPE_SRC_DESCRIPTOR_BUFF] = {
		.limits[IPA_RSRC_GROUP_SRC_UL] = {
			.min = 19,	.max = 19,
		},
		.limits[IPA_RSRC_GROUP_SRC_DL] = {
			.min = 26,	.max = 26,
		},
		.limits[IPA_RSRC_GROUP_SRC_DIAG] = {
			.min = 5,	.max = 5,	/* 3 downstream */
		},
		.limits[IPA_RSRC_GROUP_SRC_DMA] = {
			.min = 5,	.max = 5,	/* 7 downstream */
		},
		.limits[IPA_RSRC_GROUP_SRC_UC_RX_Q] = {
			.min = 8,	.max = 8,
		},
	},
	[IPA_RESOURCE_TYPE_SRC_HDRI2_BUFFERS] = {
		.limits[IPA_RSRC_GROUP_SRC_UL] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_DL] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_DIAG] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_DMA] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_UC_RX_Q] = {
			.min = 0,	.max = 255,
		},
	},
	[IPA_RESOURCE_TYPE_SRC_HPS_DMARS] = {
		.limits[IPA_RSRC_GROUP_SRC_UL] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_DL] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_DIAG] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_DMA] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_SRC_UC_RX_Q] = {
			.min = 0,	.max = 255,
		},
	},
	[IPA_RESOURCE_TYPE_SRC_ACK_ENTRIES] = {
		.limits[IPA_RSRC_GROUP_SRC_UL] = {
			.min = 19,	.max = 19,
		},
		.limits[IPA_RSRC_GROUP_SRC_DL] = {
			.min = 26,	.max = 26,
		},
		.limits[IPA_RSRC_GROUP_SRC_DIAG] = {
			.min = 5,	.max = 5,
		},
		.limits[IPA_RSRC_GROUP_SRC_DMA] = {
			.min = 5,	.max = 5,
		},
		.limits[IPA_RSRC_GROUP_SRC_UC_RX_Q] = {
			.min = 8,	.max = 8,
		},
	},
};

/* Destination resource configuration data for an SoC having IPA v3.1 */
static const struct ipa_resource ipa_resource_dst[] = {
	[IPA_RESOURCE_TYPE_DST_DATA_SECTORS] = {
		.limits[IPA_RSRC_GROUP_DST_UL] = {
			.min = 3,	.max = 3,	/* 2 downstream */
		},
		.limits[IPA_RSRC_GROUP_DST_DL] = {
			.min = 3,	.max = 3,
		},
		.limits[IPA_RSRC_GROUP_DST_DIAG_DPL] = {
			.min = 1,	.max = 1,	/* 0 downstream */
		},
		/* IPA_RSRC_GROUP_DST_DMA uses 2 downstream */
		.limits[IPA_RSRC_GROUP_DST_Q6ZIP_GENERAL] = {
			.min = 3,	.max = 3,
		},
		.limits[IPA_RSRC_GROUP_DST_Q6ZIP_ENGINE] = {
			.min = 3,	.max = 3,
		},
	},
	[IPA_RESOURCE_TYPE_DST_DATA_SECTOR_LISTS] = {
		.limits[IPA_RSRC_GROUP_DST_UL] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_DST_DL] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_DST_DIAG_DPL] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_DST_DMA] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_DST_Q6ZIP_GENERAL] = {
			.min = 0,	.max = 255,
		},
		.limits[IPA_RSRC_GROUP_DST_Q6ZIP_ENGINE] = {
			.min = 0,	.max = 255,
		},
	},
	[IPA_RESOURCE_TYPE_DST_DPS_DMARS] = {
		.limits[IPA_RSRC_GROUP_DST_UL] = {
			.min = 1,	.max = 1,
		},
		.limits[IPA_RSRC_GROUP_DST_DL] = {
			.min = 1,	.max = 1,
		},
		.limits[IPA_RSRC_GROUP_DST_DIAG_DPL] = {
			.min = 1,	.max = 1,
		},
		.limits[IPA_RSRC_GROUP_DST_DMA] = {
			.min = 1,	.max = 1,
		},
		.limits[IPA_RSRC_GROUP_DST_Q6ZIP_GENERAL] = {
			.min = 1,	.max = 1,
		},
	},
};

/* Resource configuration data for an SoC having IPA v3.1 */
static const struct ipa_resource_data ipa_resource_data = {
	.rsrc_group_src_count	= IPA_RSRC_GROUP_SRC_COUNT,
	.rsrc_group_dst_count	= IPA_RSRC_GROUP_DST_COUNT,
	.resource_src_count	= ARRAY_SIZE(ipa_resource_src),
	.resource_src		= ipa_resource_src,
	.resource_dst_count	= ARRAY_SIZE(ipa_resource_dst),
	.resource_dst		= ipa_resource_dst,
};

/* IPA-resident memory region data for an SoC having IPA v3.1 */
static const struct ipa_mem ipa_mem_local_data[] = {
	{
		.id		= IPA_MEM_UC_SHARED,
		.offset		= 0x0000,
		.size		= 0x0080,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_UC_INFO,
		.offset		= 0x0080,
		.size		= 0x0200,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_V4_FILTER_HASHED,
		.offset		= 0x0288,
		.size		= 0x0078,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V4_FILTER,
		.offset		= 0x0308,
		.size		= 0x0078,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V6_FILTER_HASHED,
		.offset		= 0x0388,
		.size		= 0x0078,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V6_FILTER,
		.offset		= 0x0408,
		.size		= 0x0078,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V4_ROUTE_HASHED,
		.offset		= 0x0488,
		.size		= 0x0078,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V4_ROUTE,
		.offset		= 0x0508,
		.size		= 0x0078,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V6_ROUTE_HASHED,
		.offset		= 0x0588,
		.size		= 0x0078,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V6_ROUTE,
		.offset		= 0x0608,
		.size		= 0x0078,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_MODEM_HEADER,
		.offset		= 0x0688,
		.size		= 0x0140,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_MODEM_PROC_CTX,
		.offset		= 0x07d0,
		.size		= 0x0200,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_AP_PROC_CTX,
		.offset		= 0x09d0,
		.size		= 0x0200,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_MODEM,
		.offset		= 0x0bd8,
		.size		= 0x1424,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_END_MARKER,
		.offset		= 0x2000,
		.size		= 0,
		.canary_count	= 1,
	},
};

/* Memory configuration data for an SoC having IPA v3.1 */
static const struct ipa_mem_data ipa_mem_data = {
	.local_count	= ARRAY_SIZE(ipa_mem_local_data),
	.local		= ipa_mem_local_data,
	.imem_addr	= 0x146bd000,
	.imem_size	= 0x00002000,
	.smem_id	= 497,
	.smem_size	= 0x00002000,
};

/* Interconnect bandwidths are in 1000 byte/second units */
static const struct ipa_interconnect_data ipa_interconnect_data[] = {
	{
		.name			= "memory",
		.peak_bandwidth		= 640000,	/* 640 MBps */
		.average_bandwidth	= 80000,	/* 80 MBps */
	},
	{
		.name			= "imem",
		.peak_bandwidth		= 640000,	/* 640 MBps */
		.average_bandwidth	= 80000,	/* 80 MBps */
	},
	/* Average bandwidth is unused for the next interconnect */
	{
		.name			= "config",
		.peak_bandwidth		= 80000,	/* 80 MBps */
		.average_bandwidth	= 0,		/* unused */
	},
};

/* Clock and interconnect configuration data for an SoC having IPA v3.1 */
static const struct ipa_power_data ipa_power_data = {
	.core_clock_rate	= 16 * 1000 * 1000,	/* Hz */
	.interconnect_count	= ARRAY_SIZE(ipa_interconnect_data),
	.interconnect_data	= ipa_interconnect_data,
};

/* Configuration data for an SoC having IPA v3.1 */
const struct ipa_data ipa_data_v3_1 = {
	.version		= IPA_VERSION_3_1,
	.backward_compat	= BIT(BCR_CMDQ_L_LACK_ONE_ENTRY),
	.qsb_count		= ARRAY_SIZE(ipa_qsb_data),
	.qsb_data		= ipa_qsb_data,
	.modem_route_count      = 8,
	.endpoint_count		= ARRAY_SIZE(ipa_gsi_endpoint_data),
	.endpoint_data		= ipa_gsi_endpoint_data,
	.resource_data		= &ipa_resource_data,
	.mem_data		= &ipa_mem_data,
	.power_data		= &ipa_power_data,
};
