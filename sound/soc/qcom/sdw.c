// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018, Linaro Limited.
// Copyright (c) 2018, The Linux Foundation. All rights reserved.

#include <linux/module.h>
#include <sound/soc.h>
#include "qdsp6/q6afe.h"
#include "sdw.h"

int qcom_snd_sdw_prepare(struct snd_pcm_substream *substream,
			 struct sdw_stream_runtime *sruntime,
			 bool *stream_prepared)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = asoc_rtd_to_cpu(rtd, 0);
	int ret;

	if (!sruntime)
		return 0;

	switch (cpu_dai->id) {
	case WSA_CODEC_DMA_RX_0:
	case WSA_CODEC_DMA_RX_1:
	case RX_CODEC_DMA_RX_0:
	case RX_CODEC_DMA_RX_1:
	case TX_CODEC_DMA_TX_0:
	case TX_CODEC_DMA_TX_1:
	case TX_CODEC_DMA_TX_2:
	case TX_CODEC_DMA_TX_3:
		break;
	default:
		return 0;
	}

	if (*stream_prepared)
		return 0;

	ret = sdw_prepare_stream(sruntime);
	if (ret)
		return ret;

	/**
	 * NOTE: there is a strict hw requirement about the ordering of port
	 * enables and actual WSA881x PA enable. PA enable should only happen
	 * after soundwire ports are enabled if not DC on the line is
	 * accumulated resulting in Click/Pop Noise
	 * PA enable/mute are handled as part of codec DAPM and digital mute.
	 */

	ret = sdw_enable_stream(sruntime);
	if (ret) {
		sdw_deprepare_stream(sruntime);
		return ret;
	}
	*stream_prepared  = true;

	return ret;
}
EXPORT_SYMBOL_GPL(qcom_snd_sdw_prepare);

int qcom_snd_sdw_hw_params(struct snd_pcm_substream *substream,
			   struct snd_pcm_hw_params *params,
			   struct sdw_stream_runtime **psruntime)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai;
	struct snd_soc_dai *cpu_dai = asoc_rtd_to_cpu(rtd, 0);
	struct sdw_stream_runtime *sruntime;
	int i;

	switch (cpu_dai->id) {
	case WSA_CODEC_DMA_RX_0:
	case RX_CODEC_DMA_RX_0:
	case RX_CODEC_DMA_RX_1:
	case TX_CODEC_DMA_TX_0:
	case TX_CODEC_DMA_TX_1:
	case TX_CODEC_DMA_TX_2:
	case TX_CODEC_DMA_TX_3:
		for_each_rtd_codec_dais(rtd, i, codec_dai) {
			sruntime = snd_soc_dai_get_stream(codec_dai, substream->stream);
			if (sruntime != ERR_PTR(-ENOTSUPP))
				*psruntime = sruntime;
		}
		break;
	}

	return 0;

}
EXPORT_SYMBOL_GPL(qcom_snd_sdw_hw_params);

int qcom_snd_sdw_hw_free(struct snd_pcm_substream *substream,
			 struct sdw_stream_runtime *sruntime, bool *stream_prepared)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = asoc_rtd_to_cpu(rtd, 0);

	switch (cpu_dai->id) {
	case WSA_CODEC_DMA_RX_0:
	case WSA_CODEC_DMA_RX_1:
	case RX_CODEC_DMA_RX_0:
	case RX_CODEC_DMA_RX_1:
	case TX_CODEC_DMA_TX_0:
	case TX_CODEC_DMA_TX_1:
	case TX_CODEC_DMA_TX_2:
	case TX_CODEC_DMA_TX_3:
		if (sruntime && *stream_prepared) {
			sdw_disable_stream(sruntime);
			sdw_deprepare_stream(sruntime);
			*stream_prepared = false;
		}
		break;
	default:
		break;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(qcom_snd_sdw_hw_free);
MODULE_LICENSE("GPL v2");
