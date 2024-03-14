/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Support for Medifield PNW Camera Imaging ISP subsystem.
 *
 * Copyright (c) 2010 Intel Corporation. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *
 */
#ifndef __ATOMISP_CSI2_H__
#define __ATOMISP_CSI2_H__

#include <linux/gpio/consumer.h>
#include <linux/property.h>

#include <media/v4l2-subdev.h>
#include <media/v4l2-ctrls.h>

#include "../../include/linux/atomisp.h"

#define CSI2_PAD_SINK		0
#define CSI2_PAD_SOURCE		1
#define CSI2_PADS_NUM		2

#define CSI2_MAX_ACPI_GPIOS	2u

struct acpi_device;
struct v4l2_device;

struct atomisp_device;
struct atomisp_sub_device;

struct atomisp_csi2_acpi_gpio_map {
	struct acpi_gpio_params params[CSI2_MAX_ACPI_GPIOS];
	struct acpi_gpio_mapping mapping[CSI2_MAX_ACPI_GPIOS + 1];
};

struct atomisp_csi2_acpi_gpio_parsing_data {
	struct acpi_device *adev;
	struct atomisp_csi2_acpi_gpio_map *map;
	u32 settings[CSI2_MAX_ACPI_GPIOS];
	unsigned int settings_count;
	unsigned int res_count;
	unsigned int map_count;
};

struct atomisp_mipi_csi2_device {
	struct v4l2_subdev subdev;
	struct media_pad pads[CSI2_PADS_NUM];
	struct v4l2_mbus_framefmt formats[CSI2_PADS_NUM];

	struct v4l2_ctrl_handler ctrls;
	struct atomisp_device *isp;
};

int atomisp_csi2_set_ffmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  unsigned int which, uint16_t pad,
			  struct v4l2_mbus_framefmt *ffmt);
int atomisp_mipi_csi2_init(struct atomisp_device *isp);
void atomisp_mipi_csi2_cleanup(struct atomisp_device *isp);
void atomisp_mipi_csi2_unregister_entities(
    struct atomisp_mipi_csi2_device *csi2);
int atomisp_mipi_csi2_register_entities(struct atomisp_mipi_csi2_device *csi2,
					struct v4l2_device *vdev);
int atomisp_csi2_bridge_init(struct atomisp_device *isp);
int atomisp_csi2_bridge_parse_firmware(struct atomisp_device *isp);

void atomisp_csi2_configure(struct atomisp_sub_device *asd);

#endif /* __ATOMISP_CSI2_H__ */
