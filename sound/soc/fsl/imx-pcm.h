/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2009 Sascha Hauer <s.hauer@pengutronix.de>
 *
 * This code is based on code copyrighted by Freescale,
 * Liam Girdwood, Javier Martin and probably others.
 */

#ifndef _IMX_PCM_H
#define _IMX_PCM_H

#include <linux/dma/imx-dma.h>

/*
 * Do not change this as the FIQ handler depends on this size
 */
#define IMX_SSI_DMABUF_SIZE	(64 * 1024)

#define IMX_DEFAULT_DMABUF_SIZE	(64 * 1024)

struct imx_pcm_fiq_params {
	int irq;
	void __iomem *base;

	/* Pointer to original ssi driver to setup tx rx sizes */
	struct snd_dmaengine_dai_dma_data *dma_params_rx;
	struct snd_dmaengine_dai_dma_data *dma_params_tx;
};

#if IS_ENABLED(CONFIG_SND_SOC_IMX_PCM_DMA)
int imx_pcm_dma_init(struct platform_device *pdev);
#else
static inline int imx_pcm_dma_init(struct platform_device *pdev)
{
	return -ENODEV;
}
#endif

#if IS_ENABLED(CONFIG_SND_SOC_IMX_PCM_FIQ)
int imx_pcm_fiq_init(struct platform_device *pdev,
		struct imx_pcm_fiq_params *params);
void imx_pcm_fiq_exit(struct platform_device *pdev);
#else
static inline int imx_pcm_fiq_init(struct platform_device *pdev,
		struct imx_pcm_fiq_params *params)
{
	return -ENODEV;
}

static inline void imx_pcm_fiq_exit(struct platform_device *pdev)
{
}
#endif

#endif /* _IMX_PCM_H */
