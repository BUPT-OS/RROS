/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license. When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2021, 2023 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Author: Ajit Kumar Pandey <AjitKumar.Pandey@amd.com>
 */

#ifndef __SOF_AMD_ACP_H
#define __SOF_AMD_ACP_H

#include <linux/dmi.h>

#include "../sof-priv.h"
#include "../sof-audio.h"

#define ACP_MAX_STREAM	8

#define ACP_DSP_BAR	0

#define ACP_HW_SEM_RETRY_COUNT			10000
#define ACP_REG_POLL_INTERVAL                   500
#define ACP_REG_POLL_TIMEOUT_US                 2000
#define ACP_DMA_COMPLETE_TIMEOUT_US		5000

#define ACP_PGFSM_CNTL_POWER_ON_MASK		0x01
#define ACP_PGFSM_STATUS_MASK			0x03
#define ACP_POWERED_ON				0x00
#define ACP_ASSERT_RESET			0x01
#define ACP_RELEASE_RESET			0x00
#define ACP_SOFT_RESET_DONE_MASK		0x00010001

#define ACP_DSP_INTR_EN_MASK			0x00000001
#define ACP3X_SRAM_PTE_OFFSET			0x02050000
#define ACP5X_SRAM_PTE_OFFSET			0x02050000
#define ACP6X_SRAM_PTE_OFFSET			0x03800000
#define PAGE_SIZE_4K_ENABLE			0x2
#define ACP_PAGE_SIZE				0x1000
#define ACP_DMA_CH_RUN				0x02
#define ACP_MAX_DESC_CNT			0x02
#define DSP_FW_RUN_ENABLE			0x01
#define ACP_SHA_RUN				0x01
#define ACP_SHA_RESET				0x02
#define ACP_SHA_HEADER				0x01
#define ACP_DMA_CH_RST				0x01
#define ACP_DMA_CH_GRACEFUL_RST_EN		0x10
#define ACP_ATU_CACHE_INVALID			0x01
#define ACP_MAX_DESC				128
#define ACPBUS_REG_BASE_OFFSET			ACP_DMA_CNTL_0

#define ACP_DEFAULT_DRAM_LENGTH			0x00080000
#define ACP3X_SCRATCH_MEMORY_ADDRESS		0x02050000
#define ACP_SYSTEM_MEMORY_WINDOW		0x4000000
#define ACP_IRAM_BASE_ADDRESS			0x000000
#define ACP_DATA_RAM_BASE_ADDRESS		0x01000000
#define ACP_DRAM_PAGE_COUNT			128

#define ACP_DSP_TO_HOST_IRQ			0x04

#define ACP_RN_PCI_ID				0x01
#define ACP_VANGOGH_PCI_ID			0x50
#define ACP_RMB_PCI_ID				0x6F

#define HOST_BRIDGE_CZN				0x1630
#define HOST_BRIDGE_VGH				0x1645
#define HOST_BRIDGE_RMB				0x14B5
#define ACP_SHA_STAT				0x8000
#define ACP_PSP_TIMEOUT_US			1000000
#define ACP_EXT_INTR_ERROR_STAT			0x20000000
#define MP0_C2PMSG_114_REG			0x3810AC8
#define MP0_C2PMSG_73_REG			0x3810A24
#define MBOX_ACP_SHA_DMA_COMMAND		0x70000
#define MBOX_DELAY_US				1000
#define MBOX_READY_MASK				0x80000000
#define MBOX_STATUS_MASK			0xFFFF

#define BOX_SIZE_512				0x200
#define BOX_SIZE_1024				0x400

#define EXCEPT_MAX_HDR_SIZE			0x400
#define AMD_STACK_DUMP_SIZE			32

#define SRAM1_SIZE				0x13A000
#define PROBE_STATUS_BIT			BIT(31)

#define ACP_FIRMWARE_SIGNATURE			0x100

enum clock_source {
	ACP_CLOCK_96M = 0,
	ACP_CLOCK_48M,
	ACP_CLOCK_24M,
	ACP_CLOCK_ACLK,
	ACP_CLOCK_MCLK,
};

struct  acp_atu_grp_pte {
	u32 low;
	u32 high;
};

union dma_tx_cnt {
	struct {
		unsigned int count : 19;
		unsigned int reserved : 12;
		unsigned ioc : 1;
	} bitfields, bits;
	unsigned int u32_all;
	signed int i32_all;
};

struct dma_descriptor {
	unsigned int src_addr;
	unsigned int dest_addr;
	union dma_tx_cnt tx_cnt;
	unsigned int reserved;
};

/* Scratch memory structure for communication b/w host and dsp */
struct  scratch_ipc_conf {
	/* Debug memory */
	u8 sof_debug_box[1024];
	/* Exception memory*/
	u8 sof_except_box[1024];
	/* Stream buffer */
	u8 sof_stream_box[1024];
	/* Trace buffer */
	u8 sof_trace_box[1024];
	/* Host msg flag */
	u32 sof_host_msg_write;
	/* Host ack flag*/
	u32 sof_host_ack_write;
	/* DSP msg flag */
	u32 sof_dsp_msg_write;
	/* Dsp ack flag */
	u32 sof_dsp_ack_write;
};

struct  scratch_reg_conf {
	struct scratch_ipc_conf info;
	struct acp_atu_grp_pte grp1_pte[16];
	struct acp_atu_grp_pte grp2_pte[16];
	struct acp_atu_grp_pte grp3_pte[16];
	struct acp_atu_grp_pte grp4_pte[16];
	struct acp_atu_grp_pte grp5_pte[16];
	struct acp_atu_grp_pte grp6_pte[16];
	struct acp_atu_grp_pte grp7_pte[16];
	struct acp_atu_grp_pte grp8_pte[16];
	struct dma_descriptor dma_desc[64];
	unsigned int reg_offset[8];
	unsigned int buf_size[8];
	u8 acp_tx_fifo_buf[256];
	u8 acp_rx_fifo_buf[256];
	unsigned int    reserve[];
};

struct acp_dsp_stream {
	struct list_head list;
	struct snd_sof_dev *sdev;
	struct snd_pcm_substream *substream;
	struct snd_dma_buffer *dmab;
	int num_pages;
	int stream_tag;
	int active;
	unsigned int reg_offset;
	size_t posn_offset;
	struct snd_compr_stream *cstream;
	u64 cstream_posn;
};

struct sof_amd_acp_desc {
	unsigned int rev;
	const char *name;
	unsigned int host_bridge_id;
	u32 pgfsm_base;
	u32 ext_intr_enb;
	u32 ext_intr_stat;
	u32 dsp_intr_base;
	u32 sram_pte_offset;
	u32 hw_semaphore_offset;
	u32 acp_clkmux_sel;
	u32 fusion_dsp_offset;
	u32 probe_reg_offset;
};

/* Common device data struct for ACP devices */
struct acp_dev_data {
	struct snd_sof_dev  *dev;
	const struct firmware *fw_dbin;
	/* DMIC device */
	struct platform_device *dmic_dev;
	unsigned int fw_bin_size;
	unsigned int fw_data_bin_size;
	const char *fw_code_bin;
	const char *fw_data_bin;
	u32 fw_bin_page_count;
	dma_addr_t sha_dma_addr;
	u8 *bin_buf;
	dma_addr_t dma_addr;
	u8 *data_buf;
	bool signed_fw_image;
	struct dma_descriptor dscr_info[ACP_MAX_DESC];
	struct acp_dsp_stream stream_buf[ACP_MAX_STREAM];
	struct acp_dsp_stream *dtrace_stream;
	struct pci_dev *smn_dev;
	struct acp_dsp_stream *probe_stream;
	bool enable_fw_debug;
};

void memcpy_to_scratch(struct snd_sof_dev *sdev, u32 offset, unsigned int *src, size_t bytes);
void memcpy_from_scratch(struct snd_sof_dev *sdev, u32 offset, unsigned int *dst, size_t bytes);

int acp_dma_status(struct acp_dev_data *adata, unsigned char ch);
int configure_and_run_dma(struct acp_dev_data *adata, unsigned int src_addr,
			  unsigned int dest_addr, int dsp_data_size);
int configure_and_run_sha_dma(struct acp_dev_data *adata, void *image_addr,
			      unsigned int start_addr, unsigned int dest_addr,
			      unsigned int image_length);

/* ACP device probe/remove */
int amd_sof_acp_probe(struct snd_sof_dev *sdev);
int amd_sof_acp_remove(struct snd_sof_dev *sdev);

/* DSP Loader callbacks */
int acp_sof_dsp_run(struct snd_sof_dev *sdev);
int acp_dsp_pre_fw_run(struct snd_sof_dev *sdev);
int acp_sof_load_signed_firmware(struct snd_sof_dev *sdev);
int acp_get_bar_index(struct snd_sof_dev *sdev, u32 type);

/* Block IO callbacks */
int acp_dsp_block_write(struct snd_sof_dev *sdev, enum snd_sof_fw_blk_type blk_type,
			u32 offset, void *src, size_t size);
int acp_dsp_block_read(struct snd_sof_dev *sdev, enum snd_sof_fw_blk_type blk_type,
		       u32 offset, void *dest, size_t size);

/* IPC callbacks */
irqreturn_t acp_sof_ipc_irq_thread(int irq, void *context);
int acp_sof_ipc_msg_data(struct snd_sof_dev *sdev, struct snd_sof_pcm_stream *sps,
			 void *p, size_t sz);
int acp_set_stream_data_offset(struct snd_sof_dev *sdev,
			       struct snd_sof_pcm_stream *sps,
			       size_t posn_offset);
int acp_sof_ipc_send_msg(struct snd_sof_dev *sdev,
			 struct snd_sof_ipc_msg *msg);
int acp_sof_ipc_get_mailbox_offset(struct snd_sof_dev *sdev);
int acp_sof_ipc_get_window_offset(struct snd_sof_dev *sdev, u32 id);
void acp_mailbox_write(struct snd_sof_dev *sdev, u32 offset, void *message, size_t bytes);
void acp_mailbox_read(struct snd_sof_dev *sdev, u32 offset, void *message, size_t bytes);

/* ACP - DSP  stream callbacks */
int acp_dsp_stream_config(struct snd_sof_dev *sdev, struct acp_dsp_stream *stream);
int acp_dsp_stream_init(struct snd_sof_dev *sdev);
struct acp_dsp_stream *acp_dsp_stream_get(struct snd_sof_dev *sdev, int tag);
int acp_dsp_stream_put(struct snd_sof_dev *sdev, struct acp_dsp_stream *acp_stream);

/*
 * DSP PCM Operations.
 */
int acp_pcm_open(struct snd_sof_dev *sdev, struct snd_pcm_substream *substream);
int acp_pcm_close(struct snd_sof_dev *sdev, struct snd_pcm_substream *substream);
int acp_pcm_hw_params(struct snd_sof_dev *sdev, struct snd_pcm_substream *substream,
		      struct snd_pcm_hw_params *params,
		      struct snd_sof_platform_stream_params *platform_params);
snd_pcm_uframes_t acp_pcm_pointer(struct snd_sof_dev *sdev,
				  struct snd_pcm_substream *substream);

extern struct snd_sof_dsp_ops sof_acp_common_ops;

extern struct snd_sof_dsp_ops sof_renoir_ops;
int sof_renoir_ops_init(struct snd_sof_dev *sdev);
extern struct snd_sof_dsp_ops sof_vangogh_ops;
int sof_vangogh_ops_init(struct snd_sof_dev *sdev);
extern struct snd_sof_dsp_ops sof_rembrandt_ops;
int sof_rembrandt_ops_init(struct snd_sof_dev *sdev);

struct snd_soc_acpi_mach *amd_sof_machine_select(struct snd_sof_dev *sdev);
/* Machine configuration */
int snd_amd_acp_find_config(struct pci_dev *pci);

/* Trace */
int acp_sof_trace_init(struct snd_sof_dev *sdev, struct snd_dma_buffer *dmab,
		       struct sof_ipc_dma_trace_params_ext *dtrace_params);
int acp_sof_trace_release(struct snd_sof_dev *sdev);

/* PM Callbacks */
int amd_sof_acp_suspend(struct snd_sof_dev *sdev, u32 target_state);
int amd_sof_acp_resume(struct snd_sof_dev *sdev);

void amd_sof_ipc_dump(struct snd_sof_dev *sdev);
void amd_sof_dump(struct snd_sof_dev *sdev, u32 flags);

static inline const struct sof_amd_acp_desc *get_chip_info(struct snd_sof_pdata *pdata)
{
	const struct sof_dev_desc *desc = pdata->desc;

	return desc->chip_info;
}

int acp_probes_register(struct snd_sof_dev *sdev);
void acp_probes_unregister(struct snd_sof_dev *sdev);

extern struct snd_soc_acpi_mach snd_soc_acpi_amd_vangogh_sof_machines[];
extern const struct dmi_system_id acp_sof_quirk_table[];
#endif
