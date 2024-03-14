#ifndef _VC4_HDMI_H_
#define _VC4_HDMI_H_

#include <drm/drm_connector.h>
#include <media/cec.h>
#include <sound/dmaengine_pcm.h>
#include <sound/soc.h>

#include "vc4_drv.h"

struct vc4_hdmi;
struct vc4_hdmi_register;
struct vc4_hdmi_connector_state;

enum vc4_hdmi_phy_channel {
	PHY_LANE_0 = 0,
	PHY_LANE_1,
	PHY_LANE_2,
	PHY_LANE_CK,
};

struct vc4_hdmi_variant {
	/* Encoder Type for that controller */
	enum vc4_encoder_type encoder_type;

	/* ALSA card name */
	const char *card_name;

	/* Filename to expose the registers in debugfs */
	const char *debugfs_name;

	/* Maximum pixel clock supported by the controller (in Hz) */
	unsigned long long max_pixel_clock;

	/* List of the registers available on that variant */
	const struct vc4_hdmi_register *registers;

	/* Number of registers on that variant */
	unsigned int num_registers;

	/* BCM2711 Only.
	 * The variants don't map the lane in the same order in the
	 * PHY, so this is an array mapping the HDMI channel (index)
	 * to the PHY lane (value).
	 */
	enum vc4_hdmi_phy_channel phy_lane_mapping[4];

	/* The BCM2711 cannot deal with odd horizontal pixel timings */
	bool unsupported_odd_h_timings;

	/*
	 * The BCM2711 CEC/hotplug IRQ controller is shared between the
	 * two HDMI controllers, and we have a proper irqchip driver for
	 * it.
	 */
	bool external_irq_controller;

	/* Callback to get the resources (memory region, interrupts,
	 * clocks, etc) for that variant.
	 */
	int (*init_resources)(struct drm_device *drm,
			      struct vc4_hdmi *vc4_hdmi);

	/* Callback to reset the HDMI block */
	void (*reset)(struct vc4_hdmi *vc4_hdmi);

	/* Callback to enable / disable the CSC */
	void (*csc_setup)(struct vc4_hdmi *vc4_hdmi,
			  struct drm_connector_state *state,
			  const struct drm_display_mode *mode);

	/* Callback to configure the video timings in the HDMI block */
	void (*set_timings)(struct vc4_hdmi *vc4_hdmi,
			    struct drm_connector_state *state,
			    const struct drm_display_mode *mode);

	/* Callback to initialize the PHY according to the connector state */
	void (*phy_init)(struct vc4_hdmi *vc4_hdmi,
			 struct vc4_hdmi_connector_state *vc4_conn_state);

	/* Callback to disable the PHY */
	void (*phy_disable)(struct vc4_hdmi *vc4_hdmi);

	/* Callback to enable the RNG in the PHY */
	void (*phy_rng_enable)(struct vc4_hdmi *vc4_hdmi);

	/* Callback to disable the RNG in the PHY */
	void (*phy_rng_disable)(struct vc4_hdmi *vc4_hdmi);

	/* Callback to get channel map */
	u32 (*channel_map)(struct vc4_hdmi *vc4_hdmi, u32 channel_mask);

	/* Enables HDR metadata */
	bool supports_hdr;

	/* Callback for hardware specific hotplug detect */
	bool (*hp_detect)(struct vc4_hdmi *vc4_hdmi);
};

/* HDMI audio information */
struct vc4_hdmi_audio {
	struct snd_soc_card card;
	struct snd_soc_dai_link link;
	struct snd_soc_dai_link_component cpu;
	struct snd_soc_dai_link_component codec;
	struct snd_soc_dai_link_component platform;
	struct snd_dmaengine_dai_dma_data dma_data;
	struct hdmi_audio_infoframe infoframe;
	struct platform_device *codec_pdev;
	bool streaming;
};

enum vc4_hdmi_output_format {
	VC4_HDMI_OUTPUT_RGB,
	VC4_HDMI_OUTPUT_YUV422,
	VC4_HDMI_OUTPUT_YUV444,
	VC4_HDMI_OUTPUT_YUV420,
};

enum vc4_hdmi_broadcast_rgb {
	VC4_HDMI_BROADCAST_RGB_AUTO,
	VC4_HDMI_BROADCAST_RGB_FULL,
	VC4_HDMI_BROADCAST_RGB_LIMITED,
};

/* General HDMI hardware state. */
struct vc4_hdmi {
	struct vc4_hdmi_audio audio;

	struct platform_device *pdev;
	const struct vc4_hdmi_variant *variant;

	struct vc4_encoder encoder;
	struct drm_connector connector;

	struct delayed_work scrambling_work;

	struct drm_property *broadcast_rgb_property;

	struct i2c_adapter *ddc;
	void __iomem *hdmicore_regs;
	void __iomem *hd_regs;

	/* VC5 Only */
	void __iomem *cec_regs;
	/* VC5 Only */
	void __iomem *csc_regs;
	/* VC5 Only */
	void __iomem *dvp_regs;
	/* VC5 Only */
	void __iomem *phy_regs;
	/* VC5 Only */
	void __iomem *ram_regs;
	/* VC5 Only */
	void __iomem *rm_regs;

	struct gpio_desc *hpd_gpio;

	/*
	 * On some systems (like the RPi4), some modes are in the same
	 * frequency range than the WiFi channels (1440p@60Hz for
	 * example). Should we take evasive actions because that system
	 * has a wifi adapter?
	 */
	bool disable_wifi_frequencies;

	struct cec_adapter *cec_adap;
	struct cec_msg cec_rx_msg;
	bool cec_tx_ok;
	bool cec_irq_was_rx;

	struct clk *cec_clock;
	struct clk *pixel_clock;
	struct clk *hsm_clock;
	struct clk *audio_clock;
	struct clk *pixel_bvb_clock;

	struct reset_control *reset;

	struct debugfs_regset32 hdmi_regset;
	struct debugfs_regset32 hd_regset;

	/* VC5 only */
	struct debugfs_regset32 cec_regset;
	struct debugfs_regset32 csc_regset;
	struct debugfs_regset32 dvp_regset;
	struct debugfs_regset32 phy_regset;
	struct debugfs_regset32 ram_regset;
	struct debugfs_regset32 rm_regset;

	/**
	 * @hw_lock: Spinlock protecting device register access.
	 */
	spinlock_t hw_lock;

	/**
	 * @mutex: Mutex protecting the driver access across multiple
	 * frameworks (KMS, ALSA, CEC).
	 */
	struct mutex mutex;

	/**
	 * @saved_adjusted_mode: Copy of @drm_crtc_state.adjusted_mode
	 * for use by ALSA hooks and interrupt handlers. Protected by @mutex.
	 */
	struct drm_display_mode saved_adjusted_mode;

	/**
	 * @packet_ram_enabled: Is the HDMI controller packet RAM currently
	 * on? Protected by @mutex.
	 */
	bool packet_ram_enabled;

	/**
	 * @scdc_enabled: Is the HDMI controller currently running with
	 * the scrambler on? Protected by @mutex.
	 */
	bool scdc_enabled;

	/**
	 * @output_bpc: Copy of @vc4_connector_state.output_bpc for use
	 * outside of KMS hooks. Protected by @mutex.
	 */
	unsigned int output_bpc;

	/**
	 * @output_format: Copy of @vc4_connector_state.output_format
	 * for use outside of KMS hooks. Protected by @mutex.
	 */
	enum vc4_hdmi_output_format output_format;
};

#define connector_to_vc4_hdmi(_connector)				\
	container_of_const(_connector, struct vc4_hdmi, connector)

static inline struct vc4_hdmi *
encoder_to_vc4_hdmi(struct drm_encoder *encoder)
{
	struct vc4_encoder *_encoder = to_vc4_encoder(encoder);
	return container_of_const(_encoder, struct vc4_hdmi, encoder);
}

struct vc4_hdmi_connector_state {
	struct drm_connector_state	base;
	unsigned long long		tmds_char_rate;
	unsigned int 			output_bpc;
	enum vc4_hdmi_output_format	output_format;
	enum vc4_hdmi_broadcast_rgb	broadcast_rgb;
};

#define conn_state_to_vc4_hdmi_conn_state(_state)			\
	container_of_const(_state, struct vc4_hdmi_connector_state, base)

void vc4_hdmi_phy_init(struct vc4_hdmi *vc4_hdmi,
		       struct vc4_hdmi_connector_state *vc4_conn_state);
void vc4_hdmi_phy_disable(struct vc4_hdmi *vc4_hdmi);
void vc4_hdmi_phy_rng_enable(struct vc4_hdmi *vc4_hdmi);
void vc4_hdmi_phy_rng_disable(struct vc4_hdmi *vc4_hdmi);

void vc5_hdmi_phy_init(struct vc4_hdmi *vc4_hdmi,
		       struct vc4_hdmi_connector_state *vc4_conn_state);
void vc5_hdmi_phy_disable(struct vc4_hdmi *vc4_hdmi);
void vc5_hdmi_phy_rng_enable(struct vc4_hdmi *vc4_hdmi);
void vc5_hdmi_phy_rng_disable(struct vc4_hdmi *vc4_hdmi);

#endif /* _VC4_HDMI_H_ */
