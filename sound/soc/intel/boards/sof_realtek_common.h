/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright(c) 2020 Intel Corporation.
 */

/*
 * This file defines data structures used in Machine Driver for Intel
 * platforms with Realtek Codecs.
 */
#ifndef __SOF_REALTEK_COMMON_H
#define __SOF_REALTEK_COMMON_H

#include <sound/soc.h>

#define RT1011_CODEC_DAI	"rt1011-aif"
#define RT1011_DEV0_NAME	"i2c-10EC1011:00"
#define RT1011_DEV1_NAME	"i2c-10EC1011:01"
#define RT1011_DEV2_NAME	"i2c-10EC1011:02"
#define RT1011_DEV3_NAME	"i2c-10EC1011:03"

void sof_rt1011_dai_link(struct snd_soc_dai_link *link);
void sof_rt1011_codec_conf(struct snd_soc_card *card);

#define RT1015P_CODEC_DAI	"HiFi"
#define RT1015P_DEV0_NAME	"RTL1015:00"
#define RT1015P_DEV1_NAME	"RTL1015:01"

void sof_rt1015p_dai_link(struct snd_soc_dai_link *link);
void sof_rt1015p_codec_conf(struct snd_soc_card *card);

#define RT1015_CODEC_DAI	"rt1015-aif"
#define RT1015_DEV0_NAME	"i2c-10EC1015:00"
#define RT1015_DEV1_NAME	"i2c-10EC1015:01"

void sof_rt1015_dai_link(struct snd_soc_dai_link *link);
void sof_rt1015_codec_conf(struct snd_soc_card *card);

#define RT1308_CODEC_DAI	"rt1308-aif"
#define RT1308_DEV0_NAME	"i2c-10EC1308:00"
void sof_rt1308_dai_link(struct snd_soc_dai_link *link);

#define RT1019P_CODEC_DAI	"HiFi"
#define RT1019P_DEV0_NAME	"RTL1019:00"

void sof_rt1019p_dai_link(struct snd_soc_dai_link *link);

#endif /* __SOF_REALTEK_COMMON_H */
