/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2018 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

#ifndef __SOUND_SOC_SOF_PRIV_H
#define __SOUND_SOC_SOF_PRIV_H

#include <linux/device.h>
#include <sound/hdaudio.h>
#include <sound/sof.h>
#include <sound/sof/info.h>
#include <sound/sof/pm.h>
#include <sound/sof/trace.h>
#include <uapi/sound/sof/fw.h>
#include <sound/sof/ext_manifest.h>

struct snd_sof_pcm_stream;

/* Flag definitions used in sof_core_debug (sof_debug module parameter) */
#define SOF_DBG_ENABLE_TRACE	BIT(0)
#define SOF_DBG_RETAIN_CTX	BIT(1)	/* prevent DSP D3 on FW exception */
#define SOF_DBG_VERIFY_TPLG	BIT(2) /* verify topology during load */
#define SOF_DBG_DYNAMIC_PIPELINES_OVERRIDE	BIT(3) /* 0: use topology token
							* 1: override topology
							*/
#define SOF_DBG_DYNAMIC_PIPELINES_ENABLE	BIT(4) /* 0: use static pipelines
							* 1: use dynamic pipelines
							*/
#define SOF_DBG_DISABLE_MULTICORE		BIT(5) /* schedule all pipelines/widgets
							* on primary core
							*/
#define SOF_DBG_PRINT_ALL_DUMPS		BIT(6) /* Print all ipc and dsp dumps */
#define SOF_DBG_IGNORE_D3_PERSISTENT		BIT(7) /* ignore the DSP D3 persistent capability
							* and always download firmware upon D3 exit
							*/
#define SOF_DBG_PRINT_DMA_POSITION_UPDATE_LOGS	BIT(8) /* print DMA position updates
							* in dmesg logs
							*/
#define SOF_DBG_PRINT_IPC_SUCCESS_LOGS		BIT(9) /* print IPC success
							* in dmesg logs
							*/
#define SOF_DBG_FORCE_NOCODEC			BIT(10) /* ignore all codec-related
							 * configurations
							 */
#define SOF_DBG_DUMP_IPC_MESSAGE_PAYLOAD	BIT(11) /* On top of the IPC message header
							 * dump the message payload also
							 */
#define SOF_DBG_DSPLESS_MODE			BIT(15) /* Do not initialize and use the DSP */

/* Flag definitions used for controlling the DSP dump behavior */
#define SOF_DBG_DUMP_REGS		BIT(0)
#define SOF_DBG_DUMP_MBOX		BIT(1)
#define SOF_DBG_DUMP_TEXT		BIT(2)
#define SOF_DBG_DUMP_PCI		BIT(3)
/* Output this dump (at the DEBUG level) only when SOF_DBG_PRINT_ALL_DUMPS is set */
#define SOF_DBG_DUMP_OPTIONAL		BIT(4)

/* global debug state set by SOF_DBG_ flags */
bool sof_debug_check_flag(int mask);

/* max BARs mmaped devices can use */
#define SND_SOF_BARS	8

/* time in ms for runtime suspend delay */
#define SND_SOF_SUSPEND_DELAY_MS	2000

/* DMA buffer size for trace */
#define DMA_BUF_SIZE_FOR_TRACE (PAGE_SIZE * 16)

#define SOF_IPC_DSP_REPLY		0
#define SOF_IPC_HOST_REPLY		1

/* convenience constructor for DAI driver streams */
#define SOF_DAI_STREAM(sname, scmin, scmax, srates, sfmt) \
	{.stream_name = sname, .channels_min = scmin, .channels_max = scmax, \
	 .rates = srates, .formats = sfmt}

#define SOF_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | \
	SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_FLOAT)

/* So far the primary core on all DSPs has ID 0 */
#define SOF_DSP_PRIMARY_CORE 0

/* max number of DSP cores */
#define SOF_MAX_DSP_NUM_CORES 8

struct sof_dsp_power_state {
	u32 state;
	u32 substate; /* platform-specific */
};

/* System suspend target state */
enum sof_system_suspend_state {
	SOF_SUSPEND_NONE = 0,
	SOF_SUSPEND_S0IX,
	SOF_SUSPEND_S3,
	SOF_SUSPEND_S4,
	SOF_SUSPEND_S5,
};

enum sof_dfsentry_type {
	SOF_DFSENTRY_TYPE_IOMEM = 0,
	SOF_DFSENTRY_TYPE_BUF,
};

enum sof_debugfs_access_type {
	SOF_DEBUGFS_ACCESS_ALWAYS = 0,
	SOF_DEBUGFS_ACCESS_D0_ONLY,
};

struct sof_compr_stream {
	u64 copied_total;
	u32 sampling_rate;
	u16 channels;
	u16 sample_container_bytes;
	size_t posn_offset;
};

struct snd_sof_dev;
struct snd_sof_ipc_msg;
struct snd_sof_ipc;
struct snd_sof_debugfs_map;
struct snd_soc_tplg_ops;
struct snd_soc_component;
struct snd_sof_pdata;

/**
 * struct snd_sof_platform_stream_params - platform dependent stream parameters
 * @stream_tag:		Stream tag to use
 * @use_phy_addr:	Use the provided @phy_addr for configuration
 * @phy_addr:		Platform dependent address to be used, if  @use_phy_addr
 *			is true
 * @no_ipc_position:	Disable position update IPC from firmware
 */
struct snd_sof_platform_stream_params {
	u16 stream_tag;
	bool use_phy_address;
	u32 phy_addr;
	bool no_ipc_position;
	bool cont_update_posn;
};

/**
 * struct sof_firmware - Container struct for SOF firmware
 * @fw:			Pointer to the firmware
 * @payload_offset:	Offset of the data within the loaded firmware image to be
 *			loaded to the DSP (skipping for example ext_manifest section)
 */
struct sof_firmware {
	const struct firmware *fw;
	u32 payload_offset;
};

/*
 * SOF DSP HW abstraction operations.
 * Used to abstract DSP HW architecture and any IO busses between host CPU
 * and DSP device(s).
 */
struct snd_sof_dsp_ops {

	/* probe/remove/shutdown */
	int (*probe)(struct snd_sof_dev *sof_dev); /* mandatory */
	int (*remove)(struct snd_sof_dev *sof_dev); /* optional */
	int (*shutdown)(struct snd_sof_dev *sof_dev); /* optional */

	/* DSP core boot / reset */
	int (*run)(struct snd_sof_dev *sof_dev); /* mandatory */
	int (*stall)(struct snd_sof_dev *sof_dev, unsigned int core_mask); /* optional */
	int (*reset)(struct snd_sof_dev *sof_dev); /* optional */
	int (*core_get)(struct snd_sof_dev *sof_dev, int core); /* optional */
	int (*core_put)(struct snd_sof_dev *sof_dev, int core); /* optional */

	/*
	 * Register IO: only used by respective drivers themselves,
	 * TODO: consider removing these operations and calling respective
	 * implementations directly
	 */
	void (*write8)(struct snd_sof_dev *sof_dev, void __iomem *addr,
		       u8 value); /* optional */
	u8 (*read8)(struct snd_sof_dev *sof_dev,
		    void __iomem *addr); /* optional */
	void (*write)(struct snd_sof_dev *sof_dev, void __iomem *addr,
		      u32 value); /* optional */
	u32 (*read)(struct snd_sof_dev *sof_dev,
		    void __iomem *addr); /* optional */
	void (*write64)(struct snd_sof_dev *sof_dev, void __iomem *addr,
			u64 value); /* optional */
	u64 (*read64)(struct snd_sof_dev *sof_dev,
		      void __iomem *addr); /* optional */

	/* memcpy IO */
	int (*block_read)(struct snd_sof_dev *sof_dev,
			  enum snd_sof_fw_blk_type type, u32 offset,
			  void *dest, size_t size); /* mandatory */
	int (*block_write)(struct snd_sof_dev *sof_dev,
			   enum snd_sof_fw_blk_type type, u32 offset,
			   void *src, size_t size); /* mandatory */

	/* Mailbox IO */
	void (*mailbox_read)(struct snd_sof_dev *sof_dev,
			     u32 offset, void *dest,
			     size_t size); /* optional */
	void (*mailbox_write)(struct snd_sof_dev *sof_dev,
			      u32 offset, void *src,
			      size_t size); /* optional */

	/* doorbell */
	irqreturn_t (*irq_handler)(int irq, void *context); /* optional */
	irqreturn_t (*irq_thread)(int irq, void *context); /* optional */

	/* ipc */
	int (*send_msg)(struct snd_sof_dev *sof_dev,
			struct snd_sof_ipc_msg *msg); /* mandatory */

	/* FW loading */
	int (*load_firmware)(struct snd_sof_dev *sof_dev); /* mandatory */
	int (*load_module)(struct snd_sof_dev *sof_dev,
			   struct snd_sof_mod_hdr *hdr); /* optional */

	/* connect pcm substream to a host stream */
	int (*pcm_open)(struct snd_sof_dev *sdev,
			struct snd_pcm_substream *substream); /* optional */
	/* disconnect pcm substream to a host stream */
	int (*pcm_close)(struct snd_sof_dev *sdev,
			 struct snd_pcm_substream *substream); /* optional */

	/* host stream hw params */
	int (*pcm_hw_params)(struct snd_sof_dev *sdev,
			     struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_sof_platform_stream_params *platform_params); /* optional */

	/* host stream hw_free */
	int (*pcm_hw_free)(struct snd_sof_dev *sdev,
			   struct snd_pcm_substream *substream); /* optional */

	/* host stream trigger */
	int (*pcm_trigger)(struct snd_sof_dev *sdev,
			   struct snd_pcm_substream *substream,
			   int cmd); /* optional */

	/* host stream pointer */
	snd_pcm_uframes_t (*pcm_pointer)(struct snd_sof_dev *sdev,
					 struct snd_pcm_substream *substream); /* optional */

	/* pcm ack */
	int (*pcm_ack)(struct snd_sof_dev *sdev, struct snd_pcm_substream *substream); /* optional */

	/*
	 * optional callback to retrieve the link DMA position for the substream
	 * when the position is not reported in the shared SRAM windows but
	 * instead from a host-accessible hardware counter.
	 */
	u64 (*get_stream_position)(struct snd_sof_dev *sdev,
				   struct snd_soc_component *component,
				   struct snd_pcm_substream *substream); /* optional */

	/* host read DSP stream data */
	int (*ipc_msg_data)(struct snd_sof_dev *sdev,
			    struct snd_sof_pcm_stream *sps,
			    void *p, size_t sz); /* mandatory */

	/* host side configuration of the stream's data offset in stream mailbox area */
	int (*set_stream_data_offset)(struct snd_sof_dev *sdev,
				      struct snd_sof_pcm_stream *sps,
				      size_t posn_offset); /* optional */

	/* pre/post firmware run */
	int (*pre_fw_run)(struct snd_sof_dev *sof_dev); /* optional */
	int (*post_fw_run)(struct snd_sof_dev *sof_dev); /* optional */

	/* parse platform specific extended manifest, optional */
	int (*parse_platform_ext_manifest)(struct snd_sof_dev *sof_dev,
					   const struct sof_ext_man_elem_header *hdr);

	/* DSP PM */
	int (*suspend)(struct snd_sof_dev *sof_dev,
		       u32 target_state); /* optional */
	int (*resume)(struct snd_sof_dev *sof_dev); /* optional */
	int (*runtime_suspend)(struct snd_sof_dev *sof_dev); /* optional */
	int (*runtime_resume)(struct snd_sof_dev *sof_dev); /* optional */
	int (*runtime_idle)(struct snd_sof_dev *sof_dev); /* optional */
	int (*set_hw_params_upon_resume)(struct snd_sof_dev *sdev); /* optional */
	int (*set_power_state)(struct snd_sof_dev *sdev,
			       const struct sof_dsp_power_state *target_state); /* optional */

	/* DSP clocking */
	int (*set_clk)(struct snd_sof_dev *sof_dev, u32 freq); /* optional */

	/* debug */
	const struct snd_sof_debugfs_map *debug_map; /* optional */
	int debug_map_count; /* optional */
	void (*dbg_dump)(struct snd_sof_dev *sof_dev,
			 u32 flags); /* optional */
	void (*ipc_dump)(struct snd_sof_dev *sof_dev); /* optional */
	int (*debugfs_add_region_item)(struct snd_sof_dev *sdev,
				       enum snd_sof_fw_blk_type blk_type, u32 offset,
				       size_t size, const char *name,
				       enum sof_debugfs_access_type access_type); /* optional */

	/* host DMA trace (IPC3) */
	int (*trace_init)(struct snd_sof_dev *sdev,
			  struct snd_dma_buffer *dmatb,
			  struct sof_ipc_dma_trace_params_ext *dtrace_params); /* optional */
	int (*trace_release)(struct snd_sof_dev *sdev); /* optional */
	int (*trace_trigger)(struct snd_sof_dev *sdev,
			     int cmd); /* optional */

	/* misc */
	int (*get_bar_index)(struct snd_sof_dev *sdev,
			     u32 type); /* optional */
	int (*get_mailbox_offset)(struct snd_sof_dev *sdev);/* mandatory for common loader code */
	int (*get_window_offset)(struct snd_sof_dev *sdev,
				 u32 id);/* mandatory for common loader code */

	/* machine driver ops */
	int (*machine_register)(struct snd_sof_dev *sdev,
				void *pdata); /* optional */
	void (*machine_unregister)(struct snd_sof_dev *sdev,
				   void *pdata); /* optional */
	struct snd_soc_acpi_mach * (*machine_select)(struct snd_sof_dev *sdev); /* optional */
	void (*set_mach_params)(struct snd_soc_acpi_mach *mach,
				struct snd_sof_dev *sdev); /* optional */

	/* IPC client ops */
	int (*register_ipc_clients)(struct snd_sof_dev *sdev); /* optional */
	void (*unregister_ipc_clients)(struct snd_sof_dev *sdev); /* optional */

	/* DAI ops */
	struct snd_soc_dai_driver *drv;
	int num_drv;

	/* ALSA HW info flags, will be stored in snd_pcm_runtime.hw.info */
	u32 hw_info;

	const struct dsp_arch_ops *dsp_arch_ops;
};

/* DSP architecture specific callbacks for oops and stack dumps */
struct dsp_arch_ops {
	void (*dsp_oops)(struct snd_sof_dev *sdev, const char *level, void *oops);
	void (*dsp_stack)(struct snd_sof_dev *sdev, const char *level, void *oops,
			  u32 *stack, u32 stack_words);
};

#define sof_dsp_arch_ops(sdev) ((sdev)->pdata->desc->ops->dsp_arch_ops)

/* FS entry for debug files that can expose DSP memories, registers */
struct snd_sof_dfsentry {
	size_t size;
	size_t buf_data_size;  /* length of buffered data for file read operation */
	enum sof_dfsentry_type type;
	/*
	 * access_type specifies if the
	 * memory -> DSP resource (memory, register etc) is always accessible
	 * or if it is accessible only when the DSP is in D0.
	 */
	enum sof_debugfs_access_type access_type;
#if IS_ENABLED(CONFIG_SND_SOC_SOF_DEBUG_ENABLE_DEBUGFS_CACHE)
	char *cache_buf; /* buffer to cache the contents of debugfs memory */
#endif
	struct snd_sof_dev *sdev;
	struct list_head list;  /* list in sdev dfsentry list */
	union {
		void __iomem *io_mem;
		void *buf;
	};
};

/* Debug mapping for any DSP memory or registers that can used for debug */
struct snd_sof_debugfs_map {
	const char *name;
	u32 bar;
	u32 offset;
	u32 size;
	/*
	 * access_type specifies if the memory is always accessible
	 * or if it is accessible only when the DSP is in D0.
	 */
	enum sof_debugfs_access_type access_type;
};

/* mailbox descriptor, used for host <-> DSP IPC */
struct snd_sof_mailbox {
	u32 offset;
	size_t size;
};

/* IPC message descriptor for host <-> DSP IO */
struct snd_sof_ipc_msg {
	/* message data */
	void *msg_data;
	void *reply_data;
	size_t msg_size;
	size_t reply_size;
	int reply_error;

	/* notification, firmware initiated messages */
	void *rx_data;

	wait_queue_head_t waitq;
	bool ipc_complete;
};

/**
 * struct sof_ipc_fw_tracing_ops - IPC-specific firmware tracing ops
 * @init:	Function pointer for initialization of the tracing
 * @free:	Optional function pointer for freeing of the tracing
 * @fw_crashed:	Optional function pointer to notify the tracing of a firmware crash
 * @suspend:	Function pointer for system/runtime suspend
 * @resume:	Function pointer for system/runtime resume
 */
struct sof_ipc_fw_tracing_ops {
	int (*init)(struct snd_sof_dev *sdev);
	void (*free)(struct snd_sof_dev *sdev);
	void (*fw_crashed)(struct snd_sof_dev *sdev);
	void (*suspend)(struct snd_sof_dev *sdev, pm_message_t pm_state);
	int (*resume)(struct snd_sof_dev *sdev);
};

/**
 * struct sof_ipc_pm_ops - IPC-specific PM ops
 * @ctx_save:		Optional function pointer for context save
 * @ctx_restore:	Optional function pointer for context restore
 * @set_core_state:	Optional function pointer for turning on/off a DSP core
 * @set_pm_gate:	Optional function pointer for pm gate settings
 */
struct sof_ipc_pm_ops {
	int (*ctx_save)(struct snd_sof_dev *sdev);
	int (*ctx_restore)(struct snd_sof_dev *sdev);
	int (*set_core_state)(struct snd_sof_dev *sdev, int core_idx, bool on);
	int (*set_pm_gate)(struct snd_sof_dev *sdev, u32 flags);
};

/**
 * struct sof_ipc_fw_loader_ops - IPC/FW-specific loader ops
 * @validate:		Function pointer for validating the firmware image
 * @parse_ext_manifest:	Function pointer for parsing the manifest of the firmware
 * @load_fw_to_dsp:	Optional function pointer for loading the firmware to the
 *			DSP.
 *			The function implements generic, hardware independent way
 *			of loading the initial firmware and its modules (if any).
 */
struct sof_ipc_fw_loader_ops {
	int (*validate)(struct snd_sof_dev *sdev);
	size_t (*parse_ext_manifest)(struct snd_sof_dev *sdev);
	int (*load_fw_to_dsp)(struct snd_sof_dev *sdev);
};

struct sof_ipc_tplg_ops;
struct sof_ipc_pcm_ops;

/**
 * struct sof_ipc_ops - IPC-specific ops
 * @tplg:	Pointer to IPC-specific topology ops
 * @pm:		Pointer to PM ops
 * @pcm:	Pointer to PCM ops
 * @fw_loader:	Pointer to Firmware Loader ops
 * @fw_tracing:	Optional pointer to Firmware tracing ops
 *
 * @init:	Optional pointer for IPC related initialization
 * @exit:	Optional pointer for IPC related cleanup
 * @post_fw_boot: Optional pointer to execute IPC related tasks after firmware
 *		boot.
 *
 * @tx_msg:	Function pointer for sending a 'short' IPC message
 * @set_get_data: Function pointer for set/get data ('large' IPC message). This
 *		function may split up the 'large' message and use the @tx_msg
 *		path to transfer individual chunks, or use other means to transfer
 *		the message.
 * @get_reply:	Function pointer for fetching the reply to
 *		sdev->ipc->msg.reply_data
 * @rx_msg:	Function pointer for handling a received message
 *
 * Note: both @tx_msg and @set_get_data considered as TX functions and they are
 * serialized for the duration of the instructed transfer. A large message sent
 * via @set_get_data is a single transfer even if at the hardware level it is
 * handled with multiple chunks.
 */
struct sof_ipc_ops {
	const struct sof_ipc_tplg_ops *tplg;
	const struct sof_ipc_pm_ops *pm;
	const struct sof_ipc_pcm_ops *pcm;
	const struct sof_ipc_fw_loader_ops *fw_loader;
	const struct sof_ipc_fw_tracing_ops *fw_tracing;

	int (*init)(struct snd_sof_dev *sdev);
	void (*exit)(struct snd_sof_dev *sdev);
	int (*post_fw_boot)(struct snd_sof_dev *sdev);

	int (*tx_msg)(struct snd_sof_dev *sdev, void *msg_data, size_t msg_bytes,
		      void *reply_data, size_t reply_bytes, bool no_pm);
	int (*set_get_data)(struct snd_sof_dev *sdev, void *data, size_t data_bytes,
			    bool set);
	int (*get_reply)(struct snd_sof_dev *sdev);
	void (*rx_msg)(struct snd_sof_dev *sdev);
};

/* SOF generic IPC data */
struct snd_sof_ipc {
	struct snd_sof_dev *sdev;

	/* protects messages and the disable flag */
	struct mutex tx_mutex;
	/* disables further sending of ipc's */
	bool disable_ipc_tx;

	/* Maximum allowed size of a single IPC message/reply */
	size_t max_payload_size;

	struct snd_sof_ipc_msg msg;

	/* IPC ops based on version */
	const struct sof_ipc_ops *ops;
};

/* Helper to retrieve the IPC ops */
#define sof_ipc_get_ops(sdev, ops_name)		\
		(((sdev)->ipc && (sdev)->ipc->ops) ? (sdev)->ipc->ops->ops_name : NULL)

/*
 * SOF Device Level.
 */
struct snd_sof_dev {
	struct device *dev;
	spinlock_t ipc_lock;	/* lock for IPC users */
	spinlock_t hw_lock;	/* lock for HW IO access */

	/*
	 * When true the DSP is not used.
	 * It is set under the following condition:
	 * User sets the SOF_DBG_DSPLESS_MODE flag in sof_debug module parameter
	 * and
	 * the platform advertises that it can support such mode
	 * pdata->desc->dspless_mode_supported is true.
	 */
	bool dspless_mode_selected;

	/* Main, Base firmware image */
	struct sof_firmware basefw;

	/*
	 * ASoC components. plat_drv fields are set dynamically so
	 * can't use const
	 */
	struct snd_soc_component_driver plat_drv;

	/* current DSP power state */
	struct sof_dsp_power_state dsp_power_state;
	/* mutex to protect the dsp_power_state access */
	struct mutex power_state_access;

	/* Intended power target of system suspend */
	enum sof_system_suspend_state system_suspend_target;

	/* DSP firmware boot */
	wait_queue_head_t boot_wait;
	enum sof_fw_state fw_state;
	bool first_boot;

	/* work queue in case the probe is implemented in two steps */
	struct work_struct probe_work;
	bool probe_completed;

	/* DSP HW differentiation */
	struct snd_sof_pdata *pdata;

	/* IPC */
	struct snd_sof_ipc *ipc;
	struct snd_sof_mailbox fw_info_box;	/* FW shared memory */
	struct snd_sof_mailbox dsp_box;		/* DSP initiated IPC */
	struct snd_sof_mailbox host_box;	/* Host initiated IPC */
	struct snd_sof_mailbox stream_box;	/* Stream position update */
	struct snd_sof_mailbox debug_box;	/* Debug info updates */
	struct snd_sof_ipc_msg *msg;
	int ipc_irq;
	u32 next_comp_id; /* monotonic - reset during S3 */

	/* memory bases for mmaped DSPs - set by dsp_init() */
	void __iomem *bar[SND_SOF_BARS];	/* DSP base address */
	int mmio_bar;
	int mailbox_bar;
	size_t dsp_oops_offset;

	/* debug */
	struct dentry *debugfs_root;
	struct list_head dfsentry_list;
	bool dbg_dump_printed;
	bool ipc_dump_printed;

	/* firmware loader */
	struct sof_ipc_fw_ready fw_ready;
	struct sof_ipc_fw_version fw_version;
	struct sof_ipc_cc_version *cc_version;

	/* topology */
	struct snd_soc_tplg_ops *tplg_ops;
	struct list_head pcm_list;
	struct list_head kcontrol_list;
	struct list_head widget_list;
	struct list_head pipeline_list;
	struct list_head dai_list;
	struct list_head dai_link_list;
	struct list_head route_list;
	struct snd_soc_component *component;
	u32 enabled_cores_mask; /* keep track of enabled cores */
	bool led_present;

	/* FW configuration */
	struct sof_ipc_window *info_window;

	/* IPC timeouts in ms */
	int ipc_timeout;
	int boot_timeout;

	/* firmwre tracing */
	bool fw_trace_is_supported; /* set with Kconfig or module parameter */
	void *fw_trace_data; /* private data used by firmware tracing implementation */

	bool msi_enabled;

	/* DSP core context */
	u32 num_cores;

	/*
	 * ref count per core that will be modified during system suspend/resume and during pcm
	 * hw_params/hw_free. This doesn't need to be protected with a mutex because pcm
	 * hw_params/hw_free are already protected by the PCM mutex in the ALSA framework in
	 * sound/core/ when streams are active and during system suspend/resume, streams are
	 * already suspended.
	 */
	int dsp_core_ref_count[SOF_MAX_DSP_NUM_CORES];

	/*
	 * Used to keep track of registered IPC client devices so that they can
	 * be removed when the parent SOF module is removed.
	 */
	struct list_head ipc_client_list;

	/* mutex to protect client list */
	struct mutex ipc_client_mutex;

	/*
	 * Used for tracking the IPC client's RX registration for DSP initiated
	 * message handling.
	 */
	struct list_head ipc_rx_handler_list;

	/*
	 * Used for tracking the IPC client's registration for DSP state change
	 * notification
	 */
	struct list_head fw_state_handler_list;

	/* to protect the ipc_rx_handler_list  and  dsp_state_handler_list list */
	struct mutex client_event_handler_mutex;

	/* quirks to override topology values */
	bool mclk_id_override;
	u16  mclk_id_quirk; /* same size as in IPC3 definitions */

	void *private;			/* core does not touch this */
};

/*
 * Device Level.
 */

int snd_sof_device_probe(struct device *dev, struct snd_sof_pdata *plat_data);
int snd_sof_device_remove(struct device *dev);
int snd_sof_device_shutdown(struct device *dev);
bool snd_sof_device_probe_completed(struct device *dev);

int snd_sof_runtime_suspend(struct device *dev);
int snd_sof_runtime_resume(struct device *dev);
int snd_sof_runtime_idle(struct device *dev);
int snd_sof_resume(struct device *dev);
int snd_sof_suspend(struct device *dev);
int snd_sof_dsp_power_down_notify(struct snd_sof_dev *sdev);
int snd_sof_prepare(struct device *dev);
void snd_sof_complete(struct device *dev);

void snd_sof_new_platform_drv(struct snd_sof_dev *sdev);

/*
 * Compress support
 */
extern struct snd_compress_ops sof_compressed_ops;

/*
 * Firmware loading.
 */
int snd_sof_load_firmware_raw(struct snd_sof_dev *sdev);
int snd_sof_load_firmware_memcpy(struct snd_sof_dev *sdev);
int snd_sof_run_firmware(struct snd_sof_dev *sdev);
void snd_sof_fw_unload(struct snd_sof_dev *sdev);

/*
 * IPC low level APIs.
 */
struct snd_sof_ipc *snd_sof_ipc_init(struct snd_sof_dev *sdev);
void snd_sof_ipc_free(struct snd_sof_dev *sdev);
void snd_sof_ipc_get_reply(struct snd_sof_dev *sdev);
void snd_sof_ipc_reply(struct snd_sof_dev *sdev, u32 msg_id);
static inline void snd_sof_ipc_msgs_rx(struct snd_sof_dev *sdev)
{
	sdev->ipc->ops->rx_msg(sdev);
}
int sof_ipc_tx_message(struct snd_sof_ipc *ipc, void *msg_data, size_t msg_bytes,
		       void *reply_data, size_t reply_bytes);
static inline int sof_ipc_tx_message_no_reply(struct snd_sof_ipc *ipc, void *msg_data,
					      size_t msg_bytes)
{
	return sof_ipc_tx_message(ipc, msg_data, msg_bytes, NULL, 0);
}
int sof_ipc_set_get_data(struct snd_sof_ipc *ipc, void *msg_data,
			 size_t msg_bytes, bool set);
int sof_ipc_tx_message_no_pm(struct snd_sof_ipc *ipc, void *msg_data, size_t msg_bytes,
			     void *reply_data, size_t reply_bytes);
static inline int sof_ipc_tx_message_no_pm_no_reply(struct snd_sof_ipc *ipc, void *msg_data,
						    size_t msg_bytes)
{
	return sof_ipc_tx_message_no_pm(ipc, msg_data, msg_bytes, NULL, 0);
}
int sof_ipc_send_msg(struct snd_sof_dev *sdev, void *msg_data, size_t msg_bytes,
		     size_t reply_bytes);

static inline void snd_sof_ipc_process_reply(struct snd_sof_dev *sdev, u32 msg_id)
{
	snd_sof_ipc_get_reply(sdev);
	snd_sof_ipc_reply(sdev, msg_id);
}

/*
 * Trace/debug
 */
int snd_sof_dbg_init(struct snd_sof_dev *sdev);
void snd_sof_free_debug(struct snd_sof_dev *sdev);
int snd_sof_debugfs_buf_item(struct snd_sof_dev *sdev,
			     void *base, size_t size,
			     const char *name, mode_t mode);
void sof_print_oops_and_stack(struct snd_sof_dev *sdev, const char *level,
			      u32 panic_code, u32 tracep_code, void *oops,
			      struct sof_ipc_panic_info *panic_info,
			      void *stack, size_t stack_words);
void snd_sof_handle_fw_exception(struct snd_sof_dev *sdev, const char *msg);
int snd_sof_dbg_memory_info_init(struct snd_sof_dev *sdev);
int snd_sof_debugfs_add_region_item_iomem(struct snd_sof_dev *sdev,
		enum snd_sof_fw_blk_type blk_type, u32 offset, size_t size,
		const char *name, enum sof_debugfs_access_type access_type);
/* Firmware tracing */
int sof_fw_trace_init(struct snd_sof_dev *sdev);
void sof_fw_trace_free(struct snd_sof_dev *sdev);
void sof_fw_trace_fw_crashed(struct snd_sof_dev *sdev);
void sof_fw_trace_suspend(struct snd_sof_dev *sdev, pm_message_t pm_state);
int sof_fw_trace_resume(struct snd_sof_dev *sdev);

/*
 * DSP Architectures.
 */
static inline void sof_stack(struct snd_sof_dev *sdev, const char *level,
			     void *oops, u32 *stack, u32 stack_words)
{
		sof_dsp_arch_ops(sdev)->dsp_stack(sdev, level,  oops, stack,
						  stack_words);
}

static inline void sof_oops(struct snd_sof_dev *sdev, const char *level, void *oops)
{
	if (sof_dsp_arch_ops(sdev)->dsp_oops)
		sof_dsp_arch_ops(sdev)->dsp_oops(sdev, level, oops);
}

extern const struct dsp_arch_ops sof_xtensa_arch_ops;

/*
 * Firmware state tracking
 */
void sof_set_fw_state(struct snd_sof_dev *sdev, enum sof_fw_state new_state);

/*
 * Utilities
 */
void sof_io_write(struct snd_sof_dev *sdev, void __iomem *addr, u32 value);
void sof_io_write64(struct snd_sof_dev *sdev, void __iomem *addr, u64 value);
u32 sof_io_read(struct snd_sof_dev *sdev, void __iomem *addr);
u64 sof_io_read64(struct snd_sof_dev *sdev, void __iomem *addr);
void sof_mailbox_write(struct snd_sof_dev *sdev, u32 offset,
		       void *message, size_t bytes);
void sof_mailbox_read(struct snd_sof_dev *sdev, u32 offset,
		      void *message, size_t bytes);
int sof_block_write(struct snd_sof_dev *sdev, enum snd_sof_fw_blk_type blk_type,
		    u32 offset, void *src, size_t size);
int sof_block_read(struct snd_sof_dev *sdev, enum snd_sof_fw_blk_type blk_type,
		   u32 offset, void *dest, size_t size);

int sof_ipc_msg_data(struct snd_sof_dev *sdev,
		     struct snd_sof_pcm_stream *sps,
		     void *p, size_t sz);
int sof_set_stream_data_offset(struct snd_sof_dev *sdev,
			       struct snd_sof_pcm_stream *sps,
			       size_t posn_offset);

int sof_stream_pcm_open(struct snd_sof_dev *sdev,
			struct snd_pcm_substream *substream);
int sof_stream_pcm_close(struct snd_sof_dev *sdev,
			 struct snd_pcm_substream *substream);

int sof_machine_check(struct snd_sof_dev *sdev);

/* SOF client support */
#if IS_ENABLED(CONFIG_SND_SOC_SOF_CLIENT)
int sof_client_dev_register(struct snd_sof_dev *sdev, const char *name, u32 id,
			    const void *data, size_t size);
void sof_client_dev_unregister(struct snd_sof_dev *sdev, const char *name, u32 id);
int sof_register_clients(struct snd_sof_dev *sdev);
void sof_unregister_clients(struct snd_sof_dev *sdev);
void sof_client_ipc_rx_dispatcher(struct snd_sof_dev *sdev, void *msg_buf);
void sof_client_fw_state_dispatcher(struct snd_sof_dev *sdev);
int sof_suspend_clients(struct snd_sof_dev *sdev, pm_message_t state);
int sof_resume_clients(struct snd_sof_dev *sdev);
#else /* CONFIG_SND_SOC_SOF_CLIENT */
static inline int sof_client_dev_register(struct snd_sof_dev *sdev, const char *name,
					  u32 id, const void *data, size_t size)
{
	return 0;
}

static inline void sof_client_dev_unregister(struct snd_sof_dev *sdev,
					     const char *name, u32 id)
{
}

static inline int sof_register_clients(struct snd_sof_dev *sdev)
{
	return 0;
}

static inline  void sof_unregister_clients(struct snd_sof_dev *sdev)
{
}

static inline void sof_client_ipc_rx_dispatcher(struct snd_sof_dev *sdev, void *msg_buf)
{
}

static inline void sof_client_fw_state_dispatcher(struct snd_sof_dev *sdev)
{
}

static inline int sof_suspend_clients(struct snd_sof_dev *sdev, pm_message_t state)
{
	return 0;
}

static inline int sof_resume_clients(struct snd_sof_dev *sdev)
{
	return 0;
}
#endif /* CONFIG_SND_SOC_SOF_CLIENT */

/* Main ops for IPC implementations */
extern const struct sof_ipc_ops ipc3_ops;
extern const struct sof_ipc_ops ipc4_ops;

#endif
