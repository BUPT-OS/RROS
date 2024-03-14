// SPDX-License-Identifier: GPL-2.0
/*
 * Support for Medifield PNW Camera Imaging ISP subsystem.
 *
 * Copyright (c) 2010 Intel Corporation. All Rights Reserved.
 *
 * Copyright (c) 2010 Silicon Hive www.siliconhive.com.
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

#include <linux/delay.h>
#include <linux/pci.h>

#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>

#include "atomisp_cmd.h"
#include "atomisp_common.h"
#include "atomisp_fops.h"
#include "atomisp_internal.h"
#include "atomisp_ioctl.h"
#include "atomisp-regs.h"
#include "atomisp_compat.h"

#include "sh_css_hrt.h"

#include "gp_device.h"
#include "device_access.h"
#include "irq.h"

static const char *DRIVER = "atomisp";	/* max size 15 */
static const char *CARD = "ATOM ISP";	/* max size 31 */

/*
 * FIXME: ISP should not know beforehand all CIDs supported by sensor.
 * Instead, it needs to propagate to sensor unkonwn CIDs.
 */
static struct v4l2_queryctrl ci_v4l2_controls[] = {
	{
		.id = V4L2_CID_AUTO_WHITE_BALANCE,
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.name = "Automatic White Balance",
		.minimum = 0,
		.maximum = 1,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_RED_BALANCE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Red Balance",
		.minimum = 0x00,
		.maximum = 0xff,
		.step = 1,
		.default_value = 0x00,
	},
	{
		.id = V4L2_CID_BLUE_BALANCE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Blue Balance",
		.minimum = 0x00,
		.maximum = 0xff,
		.step = 1,
		.default_value = 0x00,
	},
	{
		.id = V4L2_CID_GAMMA,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Gamma",
		.minimum = 0x00,
		.maximum = 0xff,
		.step = 1,
		.default_value = 0x00,
	},
	{
		.id = V4L2_CID_POWER_LINE_FREQUENCY,
		.type = V4L2_CTRL_TYPE_MENU,
		.name = "Light frequency filter",
		.minimum = 1,
		.maximum = 2,
		.step = 1,
		.default_value = 1,
	},
	{
		.id = V4L2_CID_COLORFX,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Image Color Effect",
		.minimum = 0,
		.maximum = 9,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_COLORFX_CBCR,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Image Color Effect CbCr",
		.minimum = 0,
		.maximum = 0xffff,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_ATOMISP_BAD_PIXEL_DETECTION,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Bad Pixel Correction",
		.minimum = 0,
		.maximum = 1,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_ATOMISP_POSTPROCESS_GDC_CAC,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "GDC/CAC",
		.minimum = 0,
		.maximum = 1,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_ATOMISP_VIDEO_STABLIZATION,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Video Stablization",
		.minimum = 0,
		.maximum = 1,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_ATOMISP_FIXED_PATTERN_NR,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Fixed Pattern Noise Reduction",
		.minimum = 0,
		.maximum = 1,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_ATOMISP_FALSE_COLOR_CORRECTION,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "False Color Correction",
		.minimum = 0,
		.maximum = 1,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_REQUEST_FLASH,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Request flash frames",
		.minimum = 0,
		.maximum = 10,
		.step = 1,
		.default_value = 1,
	},
	{
		.id = V4L2_CID_ATOMISP_LOW_LIGHT,
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.name = "Low light mode",
		.minimum = 0,
		.maximum = 1,
		.step = 1,
		.default_value = 1,
	},
	{
		.id = V4L2_CID_2A_STATUS,
		.type = V4L2_CTRL_TYPE_BITMASK,
		.name = "AE and AWB status",
		.minimum = 0,
		.maximum = V4L2_2A_STATUS_AE_READY | V4L2_2A_STATUS_AWB_READY,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_EXPOSURE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "exposure",
		.minimum = -4,
		.maximum = 4,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_EXPOSURE_ZONE_NUM,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "one-time exposure zone number",
		.minimum = 0x0,
		.maximum = 0xffff,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Exposure auto priority",
		.minimum = V4L2_EXPOSURE_AUTO,
		.maximum = V4L2_EXPOSURE_APERTURE_PRIORITY,
		.step = 1,
		.default_value = V4L2_EXPOSURE_AUTO,
	},
	{
		.id = V4L2_CID_SCENE_MODE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "scene mode",
		.minimum = 0,
		.maximum = 13,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_ISO_SENSITIVITY,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "iso",
		.minimum = -4,
		.maximum = 4,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_ISO_SENSITIVITY_AUTO,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "iso mode",
		.minimum = V4L2_ISO_SENSITIVITY_MANUAL,
		.maximum = V4L2_ISO_SENSITIVITY_AUTO,
		.step = 1,
		.default_value = V4L2_ISO_SENSITIVITY_AUTO,
	},
	{
		.id = V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "white balance",
		.minimum = 0,
		.maximum = 9,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_EXPOSURE_METERING,
		.type = V4L2_CTRL_TYPE_MENU,
		.name = "metering",
		.minimum = 0,
		.maximum = 3,
		.step = 1,
		.default_value = 1,
	},
	{
		.id = V4L2_CID_3A_LOCK,
		.type = V4L2_CTRL_TYPE_BITMASK,
		.name = "3a lock",
		.minimum = 0,
		.maximum = V4L2_LOCK_EXPOSURE | V4L2_LOCK_WHITE_BALANCE
		| V4L2_LOCK_FOCUS,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_TEST_PATTERN,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Test Pattern",
		.minimum = 0,
		.maximum = 0xffff,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_TEST_PATTERN_COLOR_R,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Test Pattern Solid Color R",
		.minimum = INT_MIN,
		.maximum = INT_MAX,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_TEST_PATTERN_COLOR_GR,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Test Pattern Solid Color GR",
		.minimum = INT_MIN,
		.maximum = INT_MAX,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_TEST_PATTERN_COLOR_GB,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Test Pattern Solid Color GB",
		.minimum = INT_MIN,
		.maximum = INT_MAX,
		.step = 1,
		.default_value = 0,
	},
	{
		.id = V4L2_CID_TEST_PATTERN_COLOR_B,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Test Pattern Solid Color B",
		.minimum = INT_MIN,
		.maximum = INT_MAX,
		.step = 1,
		.default_value = 0,
	},
};

static const u32 ctrls_num = ARRAY_SIZE(ci_v4l2_controls);

/*
 * supported V4L2 fmts and resolutions
 */
const struct atomisp_format_bridge atomisp_output_fmts[] = {
	{
		.pixelformat = V4L2_PIX_FMT_YUV420,
		.depth = 12,
		.mbus_code = V4L2_MBUS_FMT_CUSTOM_YUV420,
		.sh_fmt = IA_CSS_FRAME_FORMAT_YUV420,
		.description = "YUV420, planar",
		.planar = true
	}, {
		.pixelformat = V4L2_PIX_FMT_YVU420,
		.depth = 12,
		.mbus_code = V4L2_MBUS_FMT_CUSTOM_YVU420,
		.sh_fmt = IA_CSS_FRAME_FORMAT_YV12,
		.description = "YVU420, planar",
		.planar = true
	}, {
		.pixelformat = V4L2_PIX_FMT_YUV422P,
		.depth = 16,
		.mbus_code = V4L2_MBUS_FMT_CUSTOM_YUV422P,
		.sh_fmt = IA_CSS_FRAME_FORMAT_YUV422,
		.description = "YUV422, planar",
		.planar = true
	}, {
		.pixelformat = V4L2_PIX_FMT_YUV444,
		.depth = 24,
		.mbus_code = V4L2_MBUS_FMT_CUSTOM_YUV444,
		.sh_fmt = IA_CSS_FRAME_FORMAT_YUV444,
		.description = "YUV444"
	}, {
		.pixelformat = V4L2_PIX_FMT_NV12,
		.depth = 12,
		.mbus_code = V4L2_MBUS_FMT_CUSTOM_NV12,
		.sh_fmt = IA_CSS_FRAME_FORMAT_NV12,
		.description = "NV12, Y-plane, CbCr interleaved",
		.planar = true
	}, {
		.pixelformat = V4L2_PIX_FMT_NV21,
		.depth = 12,
		.mbus_code = V4L2_MBUS_FMT_CUSTOM_NV21,
		.sh_fmt = IA_CSS_FRAME_FORMAT_NV21,
		.description = "NV21, Y-plane, CbCr interleaved",
		.planar = true
	}, {
		.pixelformat = V4L2_PIX_FMT_NV16,
		.depth = 16,
		.mbus_code = V4L2_MBUS_FMT_CUSTOM_NV16,
		.sh_fmt = IA_CSS_FRAME_FORMAT_NV16,
		.description = "NV16, Y-plane, CbCr interleaved",
		.planar = true
	}, {
		.pixelformat = V4L2_PIX_FMT_YUYV,
		.depth = 16,
		.mbus_code = V4L2_MBUS_FMT_CUSTOM_YUYV,
		.sh_fmt = IA_CSS_FRAME_FORMAT_YUYV,
		.description = "YUYV, interleaved"
	}, {
		.pixelformat = V4L2_PIX_FMT_UYVY,
		.depth = 16,
		.mbus_code = MEDIA_BUS_FMT_UYVY8_1X16,
		.sh_fmt = IA_CSS_FRAME_FORMAT_UYVY,
		.description = "UYVY, interleaved"
	}, { /* This one is for parallel sensors! DO NOT USE! */
		.pixelformat = V4L2_PIX_FMT_UYVY,
		.depth = 16,
		.mbus_code = MEDIA_BUS_FMT_UYVY8_2X8,
		.sh_fmt = IA_CSS_FRAME_FORMAT_UYVY,
		.description = "UYVY, interleaved"
	}, {
		.pixelformat = V4L2_PIX_FMT_SBGGR16,
		.depth = 16,
		.mbus_code = V4L2_MBUS_FMT_CUSTOM_SBGGR16,
		.sh_fmt = IA_CSS_FRAME_FORMAT_RAW,
		.description = "Bayer 16"
	}, {
		.pixelformat = V4L2_PIX_FMT_SBGGR8,
		.depth = 8,
		.mbus_code = MEDIA_BUS_FMT_SBGGR8_1X8,
		.sh_fmt = IA_CSS_FRAME_FORMAT_RAW,
		.description = "Bayer 8"
	}, {
		.pixelformat = V4L2_PIX_FMT_SGBRG8,
		.depth = 8,
		.mbus_code = MEDIA_BUS_FMT_SGBRG8_1X8,
		.sh_fmt = IA_CSS_FRAME_FORMAT_RAW,
		.description = "Bayer 8"
	}, {
		.pixelformat = V4L2_PIX_FMT_SGRBG8,
		.depth = 8,
		.mbus_code = MEDIA_BUS_FMT_SGRBG8_1X8,
		.sh_fmt = IA_CSS_FRAME_FORMAT_RAW,
		.description = "Bayer 8"
	}, {
		.pixelformat = V4L2_PIX_FMT_SRGGB8,
		.depth = 8,
		.mbus_code = MEDIA_BUS_FMT_SRGGB8_1X8,
		.sh_fmt = IA_CSS_FRAME_FORMAT_RAW,
		.description = "Bayer 8"
	}, {
		.pixelformat = V4L2_PIX_FMT_SBGGR10,
		.depth = 16,
		.mbus_code = MEDIA_BUS_FMT_SBGGR10_1X10,
		.sh_fmt = IA_CSS_FRAME_FORMAT_RAW,
		.description = "Bayer 10"
	}, {
		.pixelformat = V4L2_PIX_FMT_SGBRG10,
		.depth = 16,
		.mbus_code = MEDIA_BUS_FMT_SGBRG10_1X10,
		.sh_fmt = IA_CSS_FRAME_FORMAT_RAW,
		.description = "Bayer 10"
	}, {
		.pixelformat = V4L2_PIX_FMT_SGRBG10,
		.depth = 16,
		.mbus_code = MEDIA_BUS_FMT_SGRBG10_1X10,
		.sh_fmt = IA_CSS_FRAME_FORMAT_RAW,
		.description = "Bayer 10"
	}, {
		.pixelformat = V4L2_PIX_FMT_SRGGB10,
		.depth = 16,
		.mbus_code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.sh_fmt = IA_CSS_FRAME_FORMAT_RAW,
		.description = "Bayer 10"
	}, {
		.pixelformat = V4L2_PIX_FMT_SBGGR12,
		.depth = 16,
		.mbus_code = MEDIA_BUS_FMT_SBGGR12_1X12,
		.sh_fmt = IA_CSS_FRAME_FORMAT_RAW,
		.description = "Bayer 12"
	}, {
		.pixelformat = V4L2_PIX_FMT_SGBRG12,
		.depth = 16,
		.mbus_code = MEDIA_BUS_FMT_SGBRG12_1X12,
		.sh_fmt = IA_CSS_FRAME_FORMAT_RAW,
		.description = "Bayer 12"
	}, {
		.pixelformat = V4L2_PIX_FMT_SGRBG12,
		.depth = 16,
		.mbus_code = MEDIA_BUS_FMT_SGRBG12_1X12,
		.sh_fmt = IA_CSS_FRAME_FORMAT_RAW,
		.description = "Bayer 12"
	}, {
		.pixelformat = V4L2_PIX_FMT_SRGGB12,
		.depth = 16,
		.mbus_code = MEDIA_BUS_FMT_SRGGB12_1X12,
		.sh_fmt = IA_CSS_FRAME_FORMAT_RAW,
		.description = "Bayer 12"
	}, {
		.pixelformat = V4L2_PIX_FMT_RGB32,
		.depth = 32,
		.mbus_code = V4L2_MBUS_FMT_CUSTOM_RGB32,
		.sh_fmt = IA_CSS_FRAME_FORMAT_RGBA888,
		.description = "32 RGB 8-8-8-8"
	}, {
		.pixelformat = V4L2_PIX_FMT_RGB565,
		.depth = 16,
		.mbus_code = MEDIA_BUS_FMT_BGR565_2X8_LE,
		.sh_fmt = IA_CSS_FRAME_FORMAT_RGB565,
		.description = "16 RGB 5-6-5"
#if 0
	}, {
		.pixelformat = V4L2_PIX_FMT_JPEG,
		.depth = 8,
		.mbus_code = MEDIA_BUS_FMT_JPEG_1X8,
		.sh_fmt = IA_CSS_FRAME_FORMAT_BINARY_8,
		.description = "JPEG"
	}, {
		/* This is a custom format being used by M10MO to send the RAW data */
		.pixelformat = V4L2_PIX_FMT_CUSTOM_M10MO_RAW,
		.depth = 8,
		.mbus_code = V4L2_MBUS_FMT_CUSTOM_M10MO_RAW,
		.sh_fmt = IA_CSS_FRAME_FORMAT_BINARY_8,
		.description = "Custom RAW for M10MO"
#endif
	},
};

const struct atomisp_format_bridge *
atomisp_get_format_bridge(unsigned int pixelformat)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(atomisp_output_fmts); i++) {
		if (atomisp_output_fmts[i].pixelformat == pixelformat)
			return &atomisp_output_fmts[i];
	}

	return NULL;
}

const struct atomisp_format_bridge *
atomisp_get_format_bridge_from_mbus(u32 mbus_code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(atomisp_output_fmts); i++) {
		if (mbus_code == atomisp_output_fmts[i].mbus_code)
			return &atomisp_output_fmts[i];
	}

	return NULL;
}

int atomisp_pipe_check(struct atomisp_video_pipe *pipe, bool settings_change)
{
	lockdep_assert_held(&pipe->isp->mutex);

	if (pipe->isp->isp_fatal_error)
		return -EIO;

	if (settings_change && vb2_is_busy(&pipe->vb_queue)) {
		dev_err(pipe->isp->dev, "Set fmt/input IOCTL while streaming\n");
		return -EBUSY;
	}

	return 0;
}

/*
 * v4l2 ioctls
 * return ISP capabilities
 */
static int atomisp_querycap(struct file *file, void *fh,
			    struct v4l2_capability *cap)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_device *isp = video_get_drvdata(vdev);

	strscpy(cap->driver, DRIVER, sizeof(cap->driver));
	strscpy(cap->card, CARD, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s", dev_name(isp->dev));

	return 0;
}

/*
 * enum input are used to check primary/secondary camera
 */
static int atomisp_enum_input(struct file *file, void *fh,
			      struct v4l2_input *input)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_device *isp = video_get_drvdata(vdev);
	int index = input->index;
	struct v4l2_subdev *motor;

	if (index >= isp->input_cnt)
		return -EINVAL;

	if (!isp->inputs[index].camera)
		return -EINVAL;

	memset(input, 0, sizeof(struct v4l2_input));
	strscpy(input->name, isp->inputs[index].camera->name,
		sizeof(input->name));

	/*
	 * HACK: append actuator's name to sensor's
	 * As currently userspace can't talk directly to subdev nodes, this
	 * ioctl is the only way to enum inputs + possible external actuators
	 * for 3A tuning purpose.
	 */
	if (!IS_ISP2401)
		motor = isp->inputs[index].motor;
	else
		motor = isp->motor;

	if (motor && strlen(motor->name) > 0) {
		const int cur_len = strlen(input->name);
		const int max_size = sizeof(input->name) - cur_len - 1;

		if (max_size > 1) {
			input->name[cur_len] = '+';
			strscpy(&input->name[cur_len + 1],
				motor->name, max_size);
		}
	}

	input->type = V4L2_INPUT_TYPE_CAMERA;
	input->index = index;
	input->reserved[0] = isp->inputs[index].type;
	input->reserved[1] = isp->inputs[index].port;

	return 0;
}

/*
 * get input are used to get current primary/secondary camera
 */
static int atomisp_g_input(struct file *file, void *fh, unsigned int *input)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;

	*input = asd->input_curr;
	return 0;
}

static int atomisp_s_fmt_cap(struct file *file, void *fh,
			     struct v4l2_format *f)
{
	struct video_device *vdev = video_devdata(file);

	return atomisp_set_fmt(vdev, f);
}

/*
 * set input are used to set current primary/secondary camera
 */
static int atomisp_s_input(struct file *file, void *fh, unsigned int input)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct atomisp_video_pipe *pipe = atomisp_to_video_pipe(vdev);
	struct atomisp_sub_device *asd = pipe->asd;
	struct v4l2_subdev *camera = NULL;
	struct v4l2_subdev *motor;
	int ret;

	ret = atomisp_pipe_check(pipe, true);
	if (ret)
		return ret;

	if (input >= ATOM_ISP_MAX_INPUTS || input >= isp->input_cnt) {
		dev_dbg(isp->dev, "input_cnt: %d\n", isp->input_cnt);
		return -EINVAL;
	}

	camera = isp->inputs[input].camera;
	if (!camera) {
		dev_err(isp->dev, "%s, no camera\n", __func__);
		return -EINVAL;
	}

	/* power off the current owned sensor, as it is not used this time */
	if (isp->inputs[asd->input_curr].asd == asd &&
	    asd->input_curr != input) {
		ret = v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
				       core, s_power, 0);
		if (ret && ret != -ENOIOCTLCMD)
			dev_warn(isp->dev,
				 "Failed to power-off sensor\n");
		/* clear the asd field to show this camera is not used */
		isp->inputs[asd->input_curr].asd = NULL;
	}

	/* powe on the new sensor */
	ret = v4l2_subdev_call(isp->inputs[input].camera, core, s_power, 1);
	if (ret && ret != -ENOIOCTLCMD) {
		dev_err(isp->dev, "Failed to power-on sensor\n");
		return ret;
	}
	/*
	 * Some sensor driver resets the run mode during power-on, thus force
	 * update the run mode to sensor after power-on.
	 */
	atomisp_update_run_mode(asd);

	/* select operating sensor */
	ret = v4l2_subdev_call(isp->inputs[input].camera, video, s_routing,
			       0, 0, 0);
	if (ret && (ret != -ENOIOCTLCMD)) {
		dev_err(isp->dev, "Failed to select sensor\n");
		return ret;
	}

	if (!IS_ISP2401) {
		motor = isp->inputs[input].motor;
	} else {
		motor = isp->motor;
		if (motor)
			ret = v4l2_subdev_call(motor, core, s_power, 1);
	}

	if (motor)
		ret = v4l2_subdev_call(motor, core, init, 1);

	asd->input_curr = input;
	/* mark this camera is used by the current stream */
	isp->inputs[input].asd = asd;

	return 0;
}

/*
 * With crop any framesize <= sensor-size can be made, give
 * userspace a list of sizes to choice from.
 */
static int atomisp_enum_framesizes_crop_inner(struct atomisp_device *isp,
					      struct v4l2_frmsizeenum *fsize,
					      const struct v4l2_rect *active,
					      const struct v4l2_rect *native,
					      int *valid_sizes)
{
	static const struct v4l2_frmsize_discrete frame_sizes[] = {
		{ 1600, 1200 },
		{ 1600, 1080 },
		{ 1600,  900 },
		{ 1440, 1080 },
		{ 1280,  960 },
		{ 1280,  720 },
		{  800,  600 },
		{  640,  480 },
	};
	u32 padding_w, padding_h;
	int i;

	for (i = 0; i < ARRAY_SIZE(frame_sizes); i++) {
		atomisp_get_padding(isp, frame_sizes[i].width, frame_sizes[i].height,
				    &padding_w, &padding_h);

		if ((frame_sizes[i].width + padding_w) > native->width ||
		    (frame_sizes[i].height + padding_h) > native->height)
			continue;

		/*
		 * Skip sizes where width and height are less then 2/3th of the
		 * sensor size to avoid sizes with a too small field of view.
		 */
		if (frame_sizes[i].width < (active->width * 2 / 3) &&
		    frame_sizes[i].height < (active->height * 2 / 3))
			continue;

		if (*valid_sizes == fsize->index) {
			fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
			fsize->discrete = frame_sizes[i];
			return 0;
		}

		(*valid_sizes)++;
	}

	return -EINVAL;
}

static int atomisp_enum_framesizes_crop(struct atomisp_device *isp,
					struct v4l2_frmsizeenum *fsize)
{
	struct atomisp_input_subdev *input = &isp->inputs[isp->asd.input_curr];
	struct v4l2_rect active = input->active_rect;
	struct v4l2_rect native = input->native_rect;
	int ret, valid_sizes = 0;

	ret = atomisp_enum_framesizes_crop_inner(isp, fsize, &active, &native, &valid_sizes);
	if (ret == 0)
		return 0;

	if (!input->binning_support)
		return -EINVAL;

	active.width /= 2;
	active.height /= 2;
	native.width /= 2;
	native.height /= 2;

	return atomisp_enum_framesizes_crop_inner(isp, fsize, &active, &native, &valid_sizes);
}

static int atomisp_enum_framesizes(struct file *file, void *priv,
				   struct v4l2_frmsizeenum *fsize)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	struct atomisp_input_subdev *input = &isp->inputs[asd->input_curr];
	struct v4l2_subdev_frame_size_enum fse = {
		.index = fsize->index,
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.code = input->code,
	};
	int ret;

	if (input->crop_support)
		return atomisp_enum_framesizes_crop(isp, fsize);

	ret = v4l2_subdev_call(input->camera, pad, enum_frame_size, NULL, &fse);
	if (ret)
		return ret;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = fse.max_width - pad_w;
	fsize->discrete.height = fse.max_height - pad_h;

	return 0;
}

static int atomisp_enum_frameintervals(struct file *file, void *priv,
				       struct v4l2_frmivalenum *fival)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	struct v4l2_subdev_frame_interval_enum fie = {
		.code	= atomisp_in_fmt_conv[0].code,
		.index = fival->index,
		.width = fival->width,
		.height = fival->height,
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	int ret;

	ret = v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
			       pad, enum_frame_interval, NULL,
			       &fie);
	if (ret)
		return ret;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete = fie.interval;

	return ret;
}

static int atomisp_enum_fmt_cap(struct file *file, void *fh,
				struct v4l2_fmtdesc *f)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	struct v4l2_subdev_mbus_code_enum code = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	const struct atomisp_format_bridge *format;
	struct v4l2_subdev *camera;
	unsigned int i, fi = 0;
	int rval;

	camera = isp->inputs[asd->input_curr].camera;
	if(!camera) {
		dev_err(isp->dev, "%s(): camera is NULL, device is %s\n",
			__func__, vdev->name);
		return -EINVAL;
	}

	rval = v4l2_subdev_call(camera, pad, enum_mbus_code, NULL, &code);
	if (rval == -ENOIOCTLCMD) {
		dev_warn(isp->dev,
			 "enum_mbus_code pad op not supported by %s. Please fix your sensor driver!\n",
			 camera->name);
	}

	if (rval)
		return rval;

	for (i = 0; i < ARRAY_SIZE(atomisp_output_fmts); i++) {
		format = &atomisp_output_fmts[i];

		/*
		 * Is the atomisp-supported format is valid for the
		 * sensor (configuration)? If not, skip it.
		 *
		 * FIXME: fix the pipeline to allow sensor format too.
		 */
		if (format->sh_fmt == IA_CSS_FRAME_FORMAT_RAW)
			continue;

		/* Found a match. Now let's pick f->index'th one. */
		if (fi < f->index) {
			fi++;
			continue;
		}

		strscpy(f->description, format->description,
			sizeof(f->description));
		f->pixelformat = format->pixelformat;
		return 0;
	}

	return -EINVAL;
}

/* This function looks up the closest available resolution. */
static int atomisp_try_fmt_cap(struct file *file, void *fh,
			       struct v4l2_format *f)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_device *isp = video_get_drvdata(vdev);

	return atomisp_try_fmt(isp, &f->fmt.pix, NULL, NULL);
}

static int atomisp_g_fmt_cap(struct file *file, void *fh,
			     struct v4l2_format *f)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_video_pipe *pipe;

	pipe = atomisp_to_video_pipe(vdev);

	f->fmt.pix = pipe->pix;

	/* If s_fmt was issued, just return whatever is was previouly set */
	if (f->fmt.pix.sizeimage)
		return 0;

	f->fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
	f->fmt.pix.width = 10000;
	f->fmt.pix.height = 10000;

	return atomisp_try_fmt_cap(file, fh, f);
}

int atomisp_alloc_css_stat_bufs(struct atomisp_sub_device *asd,
				uint16_t stream_id)
{
	struct atomisp_device *isp = asd->isp;
	struct atomisp_s3a_buf *s3a_buf = NULL, *_s3a_buf;
	struct atomisp_dis_buf *dis_buf = NULL, *_dis_buf;
	struct atomisp_metadata_buf *md_buf = NULL, *_md_buf;
	int count;
	struct ia_css_dvs_grid_info *dvs_grid_info =
	    atomisp_css_get_dvs_grid_info(&asd->params.curr_grid_info);
	unsigned int i;

	if (list_empty(&asd->s3a_stats) &&
	    asd->params.curr_grid_info.s3a_grid.enable) {
		count = ATOMISP_CSS_Q_DEPTH +
			ATOMISP_S3A_BUF_QUEUE_DEPTH_FOR_HAL;
		dev_dbg(isp->dev, "allocating %d 3a buffers\n", count);
		while (count--) {
			s3a_buf = kzalloc(sizeof(struct atomisp_s3a_buf), GFP_KERNEL);
			if (!s3a_buf)
				goto error;

			if (atomisp_css_allocate_stat_buffers(
				asd, stream_id, s3a_buf, NULL, NULL)) {
				kfree(s3a_buf);
				goto error;
			}

			list_add_tail(&s3a_buf->list, &asd->s3a_stats);
		}
	}

	if (list_empty(&asd->dis_stats) && dvs_grid_info &&
	    dvs_grid_info->enable) {
		count = ATOMISP_CSS_Q_DEPTH + 1;
		dev_dbg(isp->dev, "allocating %d dis buffers\n", count);
		while (count--) {
			dis_buf = kzalloc(sizeof(struct atomisp_dis_buf), GFP_KERNEL);
			if (!dis_buf)
				goto error;
			if (atomisp_css_allocate_stat_buffers(
				asd, stream_id, NULL, dis_buf, NULL)) {
				kfree(dis_buf);
				goto error;
			}

			list_add_tail(&dis_buf->list, &asd->dis_stats);
		}
	}

	for (i = 0; i < ATOMISP_METADATA_TYPE_NUM; i++) {
		if (list_empty(&asd->metadata[i]) &&
		    list_empty(&asd->metadata_ready[i]) &&
		    list_empty(&asd->metadata_in_css[i])) {
			count = ATOMISP_CSS_Q_DEPTH +
				ATOMISP_METADATA_QUEUE_DEPTH_FOR_HAL;
			dev_dbg(isp->dev, "allocating %d metadata buffers for type %d\n",
				count, i);
			while (count--) {
				md_buf = kzalloc(sizeof(struct atomisp_metadata_buf),
						 GFP_KERNEL);
				if (!md_buf)
					goto error;

				if (atomisp_css_allocate_stat_buffers(
					asd, stream_id, NULL, NULL, md_buf)) {
					kfree(md_buf);
					goto error;
				}
				list_add_tail(&md_buf->list, &asd->metadata[i]);
			}
		}
	}
	return 0;

error:
	dev_err(isp->dev, "failed to allocate statistics buffers\n");

	list_for_each_entry_safe(dis_buf, _dis_buf, &asd->dis_stats, list) {
		atomisp_css_free_dis_buffer(dis_buf);
		list_del(&dis_buf->list);
		kfree(dis_buf);
	}

	list_for_each_entry_safe(s3a_buf, _s3a_buf, &asd->s3a_stats, list) {
		atomisp_css_free_3a_buffer(s3a_buf);
		list_del(&s3a_buf->list);
		kfree(s3a_buf);
	}

	for (i = 0; i < ATOMISP_METADATA_TYPE_NUM; i++) {
		list_for_each_entry_safe(md_buf, _md_buf, &asd->metadata[i],
					 list) {
			atomisp_css_free_metadata_buffer(md_buf);
			list_del(&md_buf->list);
			kfree(md_buf);
		}
	}
	return -ENOMEM;
}

/*
 * FIXME the abuse of buf->reserved2 in the qbuf and dqbuf wrappers comes from
 * the original atomisp buffer handling and should be replaced with proper V4L2
 * per frame parameters use.
 *
 * Once this is fixed these wrappers can be removed, replacing them with direct
 * calls to vb2_ioctl_[d]qbuf().
 */
static int atomisp_qbuf_wrapper(struct file *file, void *fh, struct v4l2_buffer *buf)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct atomisp_video_pipe *pipe = atomisp_to_video_pipe(vdev);

	if (buf->index >= vdev->queue->num_buffers)
		return -EINVAL;

	if (buf->reserved2 & ATOMISP_BUFFER_HAS_PER_FRAME_SETTING) {
		/* this buffer will have a per-frame parameter */
		pipe->frame_request_config_id[buf->index] = buf->reserved2 &
			~ATOMISP_BUFFER_HAS_PER_FRAME_SETTING;
		dev_dbg(isp->dev,
			"This buffer requires per_frame setting which has isp_config_id %d\n",
			pipe->frame_request_config_id[buf->index]);
	} else {
		pipe->frame_request_config_id[buf->index] = 0;
	}

	return vb2_ioctl_qbuf(file, fh, buf);
}

static int atomisp_dqbuf_wrapper(struct file *file, void *fh, struct v4l2_buffer *buf)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_video_pipe *pipe = atomisp_to_video_pipe(vdev);
	struct atomisp_sub_device *asd = pipe->asd;
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct ia_css_frame *frame;
	struct vb2_buffer *vb;
	int ret;

	ret = vb2_ioctl_dqbuf(file, fh, buf);
	if (ret)
		return ret;

	vb = pipe->vb_queue.bufs[buf->index];
	frame = vb_to_frame(vb);

	buf->reserved = asd->frame_status[buf->index];

	/*
	 * Hack:
	 * Currently frame_status in the enum type which takes no more lower
	 * 8 bit.
	 * use bit[31:16] for exp_id as it is only in the range of 1~255
	 */
	buf->reserved &= 0x0000ffff;
	if (!(buf->flags & V4L2_BUF_FLAG_ERROR))
		buf->reserved |= frame->exp_id;
	buf->reserved2 = pipe->frame_config_id[buf->index];

	dev_dbg(isp->dev,
		"dqbuf buffer %d (%s) with exp_id %d, isp_config_id %d\n",
		buf->index, vdev->name, buf->reserved >> 16, buf->reserved2);
	return 0;
}

/* Input system HW workaround */
/* Input system address translation corrupts burst during */
/* invalidate. SW workaround for this is to set burst length */
/* manually to 128 in case of 13MPx snapshot and to 1 otherwise. */
static void atomisp_dma_burst_len_cfg(struct atomisp_sub_device *asd)
{
	struct v4l2_mbus_framefmt *sink;

	sink = atomisp_subdev_get_ffmt(&asd->subdev, NULL,
				       V4L2_SUBDEV_FORMAT_ACTIVE,
				       ATOMISP_SUBDEV_PAD_SINK);

	if (sink->width * sink->height >= 4096 * 3072)
		atomisp_css2_hw_store_32(DMA_BURST_SIZE_REG, 0x7F);
	else
		atomisp_css2_hw_store_32(DMA_BURST_SIZE_REG, 0x00);
}

int atomisp_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct atomisp_video_pipe *pipe = vq_to_pipe(vq);
	struct atomisp_sub_device *asd = pipe->asd;
	struct atomisp_device *isp = asd->isp;
	struct pci_dev *pdev = to_pci_dev(isp->dev);
	unsigned long irqflags;
	int ret;

	dev_dbg(isp->dev, "Start stream\n");

	mutex_lock(&isp->mutex);

	ret = atomisp_pipe_check(pipe, false);
	if (ret)
		goto out_unlock;

	/* Input system HW workaround */
	atomisp_dma_burst_len_cfg(asd);

	/* Invalidate caches. FIXME: should flush only necessary buffers */
	wbinvd();

	if (asd->params.css_update_params_needed) {
		atomisp_apply_css_parameters(asd, &asd->params.css_param);
		if (asd->params.css_param.update_flag.dz_config)
			asd->params.config.dz_config = &asd->params.css_param.dz_config;
		atomisp_css_update_isp_params(asd);
		asd->params.css_update_params_needed = false;
		memset(&asd->params.css_param.update_flag, 0,
		       sizeof(struct atomisp_parameters));
	}
	asd->params.dvs_6axis = NULL;

	ret = atomisp_css_start(asd);
	if (ret) {
		atomisp_flush_video_pipe(pipe, VB2_BUF_STATE_QUEUED, true);
		goto out_unlock;
	}

	spin_lock_irqsave(&isp->lock, irqflags);
	asd->streaming = true;
	spin_unlock_irqrestore(&isp->lock, irqflags);
	atomic_set(&asd->sof_count, -1);
	atomic_set(&asd->sequence, -1);
	atomic_set(&asd->sequence_temp, -1);

	asd->params.dis_proj_data_valid = false;
	asd->latest_preview_exp_id = 0;
	asd->postview_exp_id = 1;
	asd->preview_exp_id = 1;

	/* handle per_frame_setting parameter and buffers */
	atomisp_handle_parameter_and_buffer(pipe);

	atomisp_qbuffers_to_css(asd);

	if (isp->flash) {
		asd->params.num_flash_frames = 0;
		asd->params.flash_state = ATOMISP_FLASH_IDLE;
		atomisp_setup_flash(asd);
	}

	atomisp_css_irq_enable(isp, IA_CSS_IRQ_INFO_CSS_RECEIVER_SOF,
			       atomisp_css_valid_sof(isp));
	atomisp_csi2_configure(asd);

	if (atomisp_freq_scaling(isp, ATOMISP_DFS_MODE_AUTO, false) < 0)
		dev_dbg(isp->dev, "DFS auto mode failed!\n");

	/* Enable the CSI interface on ANN B0/K0 */
	if (isp->media_dev.hw_revision >= ((ATOMISP_HW_REVISION_ISP2401 <<
					    ATOMISP_HW_REVISION_SHIFT) | ATOMISP_HW_STEPPING_B0)) {
		pci_write_config_word(pdev, MRFLD_PCI_CSI_CONTROL,
				      isp->saved_regs.csi_control | MRFLD_PCI_CSI_CONTROL_CSI_READY);
	}

	/* stream on the sensor */
	ret = v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
			       video, s_stream, 1);
	if (ret) {
		dev_err(isp->dev, "Starting sensor stream failed: %d\n", ret);
		spin_lock_irqsave(&isp->lock, irqflags);
		asd->streaming = false;
		spin_unlock_irqrestore(&isp->lock, irqflags);
		ret = -EINVAL;
		goto out_unlock;
	}

out_unlock:
	mutex_unlock(&isp->mutex);
	return ret;
}

void atomisp_stop_streaming(struct vb2_queue *vq)
{
	struct atomisp_video_pipe *pipe = vq_to_pipe(vq);
	struct atomisp_sub_device *asd = pipe->asd;
	struct atomisp_device *isp = asd->isp;
	struct pci_dev *pdev = to_pci_dev(isp->dev);
	unsigned long flags;
	int ret;

	dev_dbg(isp->dev, "Stop stream\n");

	mutex_lock(&isp->mutex);
	/*
	 * There is no guarantee that the buffers queued to / owned by the ISP
	 * will properly be returned to the queue when stopping. Set a flag to
	 * avoid new buffers getting queued and then wait for all the current
	 * buffers to finish.
	 */
	pipe->stopping = true;
	mutex_unlock(&isp->mutex);
	/* wait max 1 second */
	ret = wait_event_timeout(pipe->vb_queue.done_wq,
				 atomisp_buffers_in_css(pipe) == 0, HZ);
	mutex_lock(&isp->mutex);
	pipe->stopping = false;
	if (ret == 0)
		dev_warn(isp->dev, "Warning timeout waiting for CSS to return buffers\n");

	spin_lock_irqsave(&isp->lock, flags);
	asd->streaming = false;
	spin_unlock_irqrestore(&isp->lock, flags);

	atomisp_clear_css_buffer_counters(asd);
	atomisp_css_irq_enable(isp, IA_CSS_IRQ_INFO_CSS_RECEIVER_SOF, false);

	atomisp_css_stop(asd, false);

	atomisp_flush_video_pipe(pipe, VB2_BUF_STATE_ERROR, true);

	atomisp_subdev_cleanup_pending_events(asd);

	ret = v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
			       video, s_stream, 0);
	if (ret)
		dev_warn(isp->dev, "Stopping sensor stream failed: %d\n", ret);

	if (isp->flash) {
		asd->params.num_flash_frames = 0;
		asd->params.flash_state = ATOMISP_FLASH_IDLE;
	}

	/* Disable the CSI interface on ANN B0/K0 */
	if (isp->media_dev.hw_revision >= ((ATOMISP_HW_REVISION_ISP2401 <<
					    ATOMISP_HW_REVISION_SHIFT) | ATOMISP_HW_STEPPING_B0)) {
		pci_write_config_word(pdev, MRFLD_PCI_CSI_CONTROL,
				      isp->saved_regs.csi_control & ~MRFLD_PCI_CSI_CONTROL_CSI_READY);
	}

	if (atomisp_freq_scaling(isp, ATOMISP_DFS_MODE_LOW, false))
		dev_warn(isp->dev, "DFS failed.\n");

	/*
	 * ISP work around, need to reset ISP to allow next stream on to work.
	 * Streams have already been destroyed by atomisp_css_stop().
	 * Disable PUNIT/ISP acknowlede/handshake - SRSE=3 and then reset.
	 */
	pci_write_config_dword(pdev, PCI_I_CONTROL,
			       isp->saved_regs.i_control | MRFLD_PCI_I_CONTROL_SRSE_RESET_MASK);
	atomisp_reset(isp);

	/* Streams were destroyed by atomisp_css_stop(), recreate them. */
	ret = atomisp_create_pipes_stream(&isp->asd);
	if (ret)
		dev_warn(isp->dev, "Recreating streams failed: %d\n", ret);

	mutex_unlock(&isp->mutex);
}

/*
 * To get the current value of a control.
 * applications initialize the id field of a struct v4l2_control and
 * call this ioctl with a pointer to this structure
 */
static int atomisp_g_ctrl(struct file *file, void *fh,
			  struct v4l2_control *control)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	struct atomisp_device *isp = video_get_drvdata(vdev);
	int i, ret = -EINVAL;

	for (i = 0; i < ctrls_num; i++) {
		if (ci_v4l2_controls[i].id == control->id) {
			ret = 0;
			break;
		}
	}

	if (ret)
		return ret;

	switch (control->id) {
	case V4L2_CID_IRIS_ABSOLUTE:
	case V4L2_CID_EXPOSURE_ABSOLUTE:
	case V4L2_CID_2A_STATUS:
	case V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE:
	case V4L2_CID_EXPOSURE:
	case V4L2_CID_EXPOSURE_AUTO:
	case V4L2_CID_SCENE_MODE:
	case V4L2_CID_ISO_SENSITIVITY:
	case V4L2_CID_ISO_SENSITIVITY_AUTO:
	case V4L2_CID_CONTRAST:
	case V4L2_CID_SATURATION:
	case V4L2_CID_SHARPNESS:
	case V4L2_CID_3A_LOCK:
	case V4L2_CID_EXPOSURE_ZONE_NUM:
	case V4L2_CID_TEST_PATTERN:
	case V4L2_CID_TEST_PATTERN_COLOR_R:
	case V4L2_CID_TEST_PATTERN_COLOR_GR:
	case V4L2_CID_TEST_PATTERN_COLOR_GB:
	case V4L2_CID_TEST_PATTERN_COLOR_B:
		return v4l2_g_ctrl(isp->inputs[asd->input_curr].camera->
				   ctrl_handler, control);
	case V4L2_CID_COLORFX:
		ret = atomisp_color_effect(asd, 0, &control->value);
		break;
	case V4L2_CID_ATOMISP_BAD_PIXEL_DETECTION:
		ret = atomisp_bad_pixel(asd, 0, &control->value);
		break;
	case V4L2_CID_ATOMISP_POSTPROCESS_GDC_CAC:
		ret = atomisp_gdc_cac(asd, 0, &control->value);
		break;
	case V4L2_CID_ATOMISP_VIDEO_STABLIZATION:
		ret = atomisp_video_stable(asd, 0, &control->value);
		break;
	case V4L2_CID_ATOMISP_FIXED_PATTERN_NR:
		ret = atomisp_fixed_pattern(asd, 0, &control->value);
		break;
	case V4L2_CID_ATOMISP_FALSE_COLOR_CORRECTION:
		ret = atomisp_false_color(asd, 0, &control->value);
		break;
	case V4L2_CID_ATOMISP_LOW_LIGHT:
		ret = atomisp_low_light(asd, 0, &control->value);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

/*
 * To change the value of a control.
 * applications initialize the id and value fields of a struct v4l2_control
 * and call this ioctl.
 */
static int atomisp_s_ctrl(struct file *file, void *fh,
			  struct v4l2_control *control)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	struct atomisp_device *isp = video_get_drvdata(vdev);
	int i, ret = -EINVAL;

	for (i = 0; i < ctrls_num; i++) {
		if (ci_v4l2_controls[i].id == control->id) {
			ret = 0;
			break;
		}
	}

	if (ret)
		return ret;

	switch (control->id) {
	case V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE:
	case V4L2_CID_EXPOSURE:
	case V4L2_CID_EXPOSURE_AUTO:
	case V4L2_CID_EXPOSURE_AUTO_PRIORITY:
	case V4L2_CID_SCENE_MODE:
	case V4L2_CID_ISO_SENSITIVITY:
	case V4L2_CID_ISO_SENSITIVITY_AUTO:
	case V4L2_CID_POWER_LINE_FREQUENCY:
	case V4L2_CID_EXPOSURE_METERING:
	case V4L2_CID_CONTRAST:
	case V4L2_CID_SATURATION:
	case V4L2_CID_SHARPNESS:
	case V4L2_CID_3A_LOCK:
	case V4L2_CID_COLORFX_CBCR:
	case V4L2_CID_TEST_PATTERN:
	case V4L2_CID_TEST_PATTERN_COLOR_R:
	case V4L2_CID_TEST_PATTERN_COLOR_GR:
	case V4L2_CID_TEST_PATTERN_COLOR_GB:
	case V4L2_CID_TEST_PATTERN_COLOR_B:
		return v4l2_s_ctrl(NULL,
				   isp->inputs[asd->input_curr].camera->
				   ctrl_handler, control);
	case V4L2_CID_COLORFX:
		ret = atomisp_color_effect(asd, 1, &control->value);
		break;
	case V4L2_CID_ATOMISP_BAD_PIXEL_DETECTION:
		ret = atomisp_bad_pixel(asd, 1, &control->value);
		break;
	case V4L2_CID_ATOMISP_POSTPROCESS_GDC_CAC:
		ret = atomisp_gdc_cac(asd, 1, &control->value);
		break;
	case V4L2_CID_ATOMISP_VIDEO_STABLIZATION:
		ret = atomisp_video_stable(asd, 1, &control->value);
		break;
	case V4L2_CID_ATOMISP_FIXED_PATTERN_NR:
		ret = atomisp_fixed_pattern(asd, 1, &control->value);
		break;
	case V4L2_CID_ATOMISP_FALSE_COLOR_CORRECTION:
		ret = atomisp_false_color(asd, 1, &control->value);
		break;
	case V4L2_CID_REQUEST_FLASH:
		ret = atomisp_flash_enable(asd, control->value);
		break;
	case V4L2_CID_ATOMISP_LOW_LIGHT:
		ret = atomisp_low_light(asd, 1, &control->value);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

/*
 * To query the attributes of a control.
 * applications set the id field of a struct v4l2_queryctrl and call the
 * this ioctl with a pointer to this structure. The driver fills
 * the rest of the structure.
 */
static int atomisp_queryctl(struct file *file, void *fh,
			    struct v4l2_queryctrl *qc)
{
	int i, ret = -EINVAL;
	struct video_device *vdev = video_devdata(file);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	struct atomisp_device *isp = video_get_drvdata(vdev);

	switch (qc->id) {
	case V4L2_CID_FOCUS_ABSOLUTE:
	case V4L2_CID_FOCUS_RELATIVE:
	case V4L2_CID_FOCUS_STATUS:
		if (!IS_ISP2401) {
			return v4l2_queryctrl(isp->inputs[asd->input_curr].camera->
					    ctrl_handler, qc);
		}
		/* ISP2401 */
		if (isp->motor)
			return v4l2_queryctrl(isp->motor->ctrl_handler, qc);
		else
			return v4l2_queryctrl(isp->inputs[asd->input_curr].
					      camera->ctrl_handler, qc);
	}

	if (qc->id & V4L2_CTRL_FLAG_NEXT_CTRL)
		return ret;

	for (i = 0; i < ctrls_num; i++) {
		if (ci_v4l2_controls[i].id == qc->id) {
			memcpy(qc, &ci_v4l2_controls[i],
			       sizeof(struct v4l2_queryctrl));
			qc->reserved[0] = 0;
			ret = 0;
			break;
		}
	}
	if (ret != 0)
		qc->flags = V4L2_CTRL_FLAG_DISABLED;

	return ret;
}

static int atomisp_camera_g_ext_ctrls(struct file *file, void *fh,
				      struct v4l2_ext_controls *c)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct v4l2_subdev *motor;
	struct v4l2_control ctrl;
	int i;
	int ret = 0;

	if (!IS_ISP2401)
		motor = isp->inputs[asd->input_curr].motor;
	else
		motor = isp->motor;

	for (i = 0; i < c->count; i++) {
		ctrl.id = c->controls[i].id;
		ctrl.value = c->controls[i].value;
		switch (ctrl.id) {
		case V4L2_CID_EXPOSURE_ABSOLUTE:
		case V4L2_CID_EXPOSURE_AUTO:
		case V4L2_CID_IRIS_ABSOLUTE:
		case V4L2_CID_3A_LOCK:
		case V4L2_CID_TEST_PATTERN:
		case V4L2_CID_TEST_PATTERN_COLOR_R:
		case V4L2_CID_TEST_PATTERN_COLOR_GR:
		case V4L2_CID_TEST_PATTERN_COLOR_GB:
		case V4L2_CID_TEST_PATTERN_COLOR_B:
			/*
			 * Exposure related control will be handled by sensor
			 * driver
			 */
			ret =
			    v4l2_g_ctrl(isp->inputs[asd->input_curr].camera->
					ctrl_handler, &ctrl);
			break;
		case V4L2_CID_FOCUS_ABSOLUTE:
		case V4L2_CID_FOCUS_RELATIVE:
		case V4L2_CID_FOCUS_STATUS:
		case V4L2_CID_FOCUS_AUTO:
			if (motor)
				ret = v4l2_g_ctrl(motor->ctrl_handler, &ctrl);
			break;
		case V4L2_CID_FLASH_STATUS:
		case V4L2_CID_FLASH_INTENSITY:
		case V4L2_CID_FLASH_TORCH_INTENSITY:
		case V4L2_CID_FLASH_INDICATOR_INTENSITY:
		case V4L2_CID_FLASH_TIMEOUT:
		case V4L2_CID_FLASH_STROBE:
		case V4L2_CID_FLASH_MODE:
		case V4L2_CID_FLASH_STATUS_REGISTER:
			if (isp->flash)
				ret =
				    v4l2_g_ctrl(isp->flash->ctrl_handler,
						&ctrl);
			break;
		case V4L2_CID_ZOOM_ABSOLUTE:
			ret = atomisp_digital_zoom(asd, 0, &ctrl.value);
			break;
		case V4L2_CID_G_SKIP_FRAMES:
			ret = v4l2_subdev_call(
				  isp->inputs[asd->input_curr].camera,
				  sensor, g_skip_frames, (u32 *)&ctrl.value);
			break;
		default:
			ret = -EINVAL;
		}

		if (ret) {
			c->error_idx = i;
			break;
		}
		c->controls[i].value = ctrl.value;
	}
	return ret;
}

/* This ioctl allows the application to get multiple controls by class */
static int atomisp_g_ext_ctrls(struct file *file, void *fh,
			       struct v4l2_ext_controls *c)
{
	struct v4l2_control ctrl;
	int i, ret = 0;

	/*
	 * input_lock is not need for the Camera related IOCTLs
	 * The input_lock downgrade the FPS of 3A
	 */
	ret = atomisp_camera_g_ext_ctrls(file, fh, c);
	if (ret != -EINVAL)
		return ret;

	for (i = 0; i < c->count; i++) {
		ctrl.id = c->controls[i].id;
		ctrl.value = c->controls[i].value;
		ret = atomisp_g_ctrl(file, fh, &ctrl);
		c->controls[i].value = ctrl.value;
		if (ret) {
			c->error_idx = i;
			break;
		}
	}
	return ret;
}

static int atomisp_camera_s_ext_ctrls(struct file *file, void *fh,
				      struct v4l2_ext_controls *c)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct v4l2_subdev *motor;
	struct v4l2_control ctrl;
	int i;
	int ret = 0;

	if (!IS_ISP2401)
		motor = isp->inputs[asd->input_curr].motor;
	else
		motor = isp->motor;

	for (i = 0; i < c->count; i++) {
		struct v4l2_ctrl *ctr;

		ctrl.id = c->controls[i].id;
		ctrl.value = c->controls[i].value;
		switch (ctrl.id) {
		case V4L2_CID_EXPOSURE_ABSOLUTE:
		case V4L2_CID_EXPOSURE_AUTO:
		case V4L2_CID_EXPOSURE_METERING:
		case V4L2_CID_IRIS_ABSOLUTE:
		case V4L2_CID_VCM_TIMING:
		case V4L2_CID_VCM_SLEW:
		case V4L2_CID_3A_LOCK:
		case V4L2_CID_TEST_PATTERN:
		case V4L2_CID_TEST_PATTERN_COLOR_R:
		case V4L2_CID_TEST_PATTERN_COLOR_GR:
		case V4L2_CID_TEST_PATTERN_COLOR_GB:
		case V4L2_CID_TEST_PATTERN_COLOR_B:
			ret = v4l2_s_ctrl(NULL,
					  isp->inputs[asd->input_curr].camera->
					  ctrl_handler, &ctrl);
			break;
		case V4L2_CID_FOCUS_ABSOLUTE:
		case V4L2_CID_FOCUS_RELATIVE:
		case V4L2_CID_FOCUS_STATUS:
		case V4L2_CID_FOCUS_AUTO:
			if (motor)
				ret = v4l2_s_ctrl(NULL, motor->ctrl_handler,
						  &ctrl);
			else
				ret = v4l2_s_ctrl(NULL,
						  isp->inputs[asd->input_curr].
						  camera->ctrl_handler, &ctrl);
			break;
		case V4L2_CID_FLASH_STATUS:
		case V4L2_CID_FLASH_INTENSITY:
		case V4L2_CID_FLASH_TORCH_INTENSITY:
		case V4L2_CID_FLASH_INDICATOR_INTENSITY:
		case V4L2_CID_FLASH_TIMEOUT:
		case V4L2_CID_FLASH_STROBE:
		case V4L2_CID_FLASH_MODE:
		case V4L2_CID_FLASH_STATUS_REGISTER:
			if (isp->flash) {
				ret =
				    v4l2_s_ctrl(NULL, isp->flash->ctrl_handler,
						&ctrl);
				/*
				 * When flash mode is changed we need to reset
				 * flash state
				 */
				if (ctrl.id == V4L2_CID_FLASH_MODE) {
					asd->params.flash_state =
					    ATOMISP_FLASH_IDLE;
					asd->params.num_flash_frames = 0;
				}
			}
			break;
		case V4L2_CID_ZOOM_ABSOLUTE:
			ret = atomisp_digital_zoom(asd, 1, &ctrl.value);
			break;
		default:
			ctr = v4l2_ctrl_find(&asd->ctrl_handler, ctrl.id);
			if (ctr)
				ret = v4l2_ctrl_s_ctrl(ctr, ctrl.value);
			else
				ret = -EINVAL;
		}

		if (ret) {
			c->error_idx = i;
			break;
		}
		c->controls[i].value = ctrl.value;
	}
	return ret;
}

/* This ioctl allows the application to set multiple controls by class */
static int atomisp_s_ext_ctrls(struct file *file, void *fh,
			       struct v4l2_ext_controls *c)
{
	struct v4l2_control ctrl;
	int i, ret = 0;

	/*
	 * input_lock is not need for the Camera related IOCTLs
	 * The input_lock downgrade the FPS of 3A
	 */
	ret = atomisp_camera_s_ext_ctrls(file, fh, c);
	if (ret != -EINVAL)
		return ret;

	for (i = 0; i < c->count; i++) {
		ctrl.id = c->controls[i].id;
		ctrl.value = c->controls[i].value;
		ret = atomisp_s_ctrl(file, fh, &ctrl);
		c->controls[i].value = ctrl.value;
		if (ret) {
			c->error_idx = i;
			break;
		}
	}
	return ret;
}

/*
 * vidioc_g/s_param are used to switch isp running mode
 */
static int atomisp_g_parm(struct file *file, void *fh,
			  struct v4l2_streamparm *parm)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	struct atomisp_device *isp = video_get_drvdata(vdev);

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		dev_err(isp->dev, "unsupported v4l2 buf type\n");
		return -EINVAL;
	}

	parm->parm.capture.capturemode = asd->run_mode->val;

	return 0;
}

static int atomisp_s_parm(struct file *file, void *fh,
			  struct v4l2_streamparm *parm)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	int mode;
	int rval;
	int fps;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		dev_err(isp->dev, "unsupported v4l2 buf type\n");
		return -EINVAL;
	}

	asd->high_speed_mode = false;
	switch (parm->parm.capture.capturemode) {
	case CI_MODE_NONE: {
		struct v4l2_subdev_frame_interval fi = {0};

		fi.interval = parm->parm.capture.timeperframe;

		rval = v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
					video, s_frame_interval, &fi);
		if (!rval)
			parm->parm.capture.timeperframe = fi.interval;

		if (fi.interval.numerator != 0) {
			fps = fi.interval.denominator / fi.interval.numerator;
			if (fps > 30)
				asd->high_speed_mode = true;
		}

		return rval == -ENOIOCTLCMD ? 0 : rval;
	}
	case CI_MODE_VIDEO:
		mode = ATOMISP_RUN_MODE_VIDEO;
		break;
	case CI_MODE_STILL_CAPTURE:
		mode = ATOMISP_RUN_MODE_STILL_CAPTURE;
		break;
	case CI_MODE_PREVIEW:
		mode = ATOMISP_RUN_MODE_PREVIEW;
		break;
	default:
		return -EINVAL;
	}

	rval = v4l2_ctrl_s_ctrl(asd->run_mode, mode);

	return rval == -ENOIOCTLCMD ? 0 : rval;
}

static long atomisp_vidioc_default(struct file *file, void *fh,
				   bool valid_prio, unsigned int cmd, void *arg)
{
	struct video_device *vdev = video_devdata(file);
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	int err;

	switch (cmd) {
	case ATOMISP_IOC_S_SENSOR_RUNMODE:
		if (IS_ISP2401)
			err = atomisp_set_sensor_runmode(asd, arg);
		else
			err = -EINVAL;
		break;

	case ATOMISP_IOC_G_XNR:
		err = atomisp_xnr(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_XNR:
		err = atomisp_xnr(asd, 1, arg);
		break;

	case ATOMISP_IOC_G_NR:
		err = atomisp_nr(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_NR:
		err = atomisp_nr(asd, 1, arg);
		break;

	case ATOMISP_IOC_G_TNR:
		err = atomisp_tnr(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_TNR:
		err = atomisp_tnr(asd, 1, arg);
		break;

	case ATOMISP_IOC_G_BLACK_LEVEL_COMP:
		err = atomisp_black_level(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_BLACK_LEVEL_COMP:
		err = atomisp_black_level(asd, 1, arg);
		break;

	case ATOMISP_IOC_G_EE:
		err = atomisp_ee(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_EE:
		err = atomisp_ee(asd, 1, arg);
		break;

	case ATOMISP_IOC_G_DIS_STAT:
		err = atomisp_get_dis_stat(asd, arg);
		break;

	case ATOMISP_IOC_G_DVS2_BQ_RESOLUTIONS:
		err = atomisp_get_dvs2_bq_resolutions(asd, arg);
		break;

	case ATOMISP_IOC_S_DIS_COEFS:
		err = atomisp_css_cp_dvs2_coefs(asd, arg,
						&asd->params.css_param, true);
		if (!err && arg)
			asd->params.css_update_params_needed = true;
		break;

	case ATOMISP_IOC_S_DIS_VECTOR:
		err = atomisp_cp_dvs_6axis_config(asd, arg,
						  &asd->params.css_param, true);
		if (!err && arg)
			asd->params.css_update_params_needed = true;
		break;

	case ATOMISP_IOC_G_ISP_PARM:
		err = atomisp_param(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_ISP_PARM:
		err = atomisp_param(asd, 1, arg);
		break;

	case ATOMISP_IOC_G_3A_STAT:
		err = atomisp_3a_stat(asd, 0, arg);
		break;

	case ATOMISP_IOC_G_ISP_GAMMA:
		err = atomisp_gamma(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_ISP_GAMMA:
		err = atomisp_gamma(asd, 1, arg);
		break;

	case ATOMISP_IOC_G_ISP_GDC_TAB:
		err = atomisp_gdc_cac_table(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_ISP_GDC_TAB:
		err = atomisp_gdc_cac_table(asd, 1, arg);
		break;

	case ATOMISP_IOC_G_ISP_MACC:
		err = atomisp_macc_table(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_ISP_MACC:
		err = atomisp_macc_table(asd, 1, arg);
		break;

	case ATOMISP_IOC_G_ISP_BAD_PIXEL_DETECTION:
		err = atomisp_bad_pixel_param(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_ISP_BAD_PIXEL_DETECTION:
		err = atomisp_bad_pixel_param(asd, 1, arg);
		break;

	case ATOMISP_IOC_G_ISP_FALSE_COLOR_CORRECTION:
		err = atomisp_false_color_param(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_ISP_FALSE_COLOR_CORRECTION:
		err = atomisp_false_color_param(asd, 1, arg);
		break;

	case ATOMISP_IOC_G_ISP_CTC:
		err = atomisp_ctc(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_ISP_CTC:
		err = atomisp_ctc(asd, 1, arg);
		break;

	case ATOMISP_IOC_G_ISP_WHITE_BALANCE:
		err = atomisp_white_balance_param(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_ISP_WHITE_BALANCE:
		err = atomisp_white_balance_param(asd, 1, arg);
		break;

	case ATOMISP_IOC_G_3A_CONFIG:
		err = atomisp_3a_config_param(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_3A_CONFIG:
		err = atomisp_3a_config_param(asd, 1, arg);
		break;

	case ATOMISP_IOC_S_ISP_FPN_TABLE:
		err = atomisp_fixed_pattern_table(asd, arg);
		break;

	case ATOMISP_IOC_S_EXPOSURE:
		err = v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
				       core, ioctl, cmd, arg);
		break;

	case ATOMISP_IOC_S_ISP_SHD_TAB:
		err = atomisp_set_shading_table(asd, arg);
		break;

	case ATOMISP_IOC_G_ISP_GAMMA_CORRECTION:
		err = atomisp_gamma_correction(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_ISP_GAMMA_CORRECTION:
		err = atomisp_gamma_correction(asd, 1, arg);
		break;

	case ATOMISP_IOC_S_PARAMETERS:
		err = atomisp_set_parameters(vdev, arg);
		break;

	case ATOMISP_IOC_EXT_ISP_CTRL:
		err = v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
				       core, ioctl, cmd, arg);
		break;
	case ATOMISP_IOC_EXP_ID_UNLOCK:
		err = atomisp_exp_id_unlock(asd, arg);
		break;
	case ATOMISP_IOC_EXP_ID_CAPTURE:
		err = atomisp_exp_id_capture(asd, arg);
		break;
	case ATOMISP_IOC_S_ENABLE_DZ_CAPT_PIPE:
		err = atomisp_enable_dz_capt_pipe(asd, arg);
		break;
	case ATOMISP_IOC_G_FORMATS_CONFIG:
		err = atomisp_formats(asd, 0, arg);
		break;

	case ATOMISP_IOC_S_FORMATS_CONFIG:
		err = atomisp_formats(asd, 1, arg);
		break;
	case ATOMISP_IOC_INJECT_A_FAKE_EVENT:
		err = atomisp_inject_a_fake_event(asd, arg);
		break;
	case ATOMISP_IOC_S_ARRAY_RESOLUTION:
		err = atomisp_set_array_res(asd, arg);
		break;
	default:
		err = -EINVAL;
		break;
	}

	return err;
}

const struct v4l2_ioctl_ops atomisp_ioctl_ops = {
	.vidioc_querycap = atomisp_querycap,
	.vidioc_enum_input = atomisp_enum_input,
	.vidioc_g_input = atomisp_g_input,
	.vidioc_s_input = atomisp_s_input,
	.vidioc_queryctrl = atomisp_queryctl,
	.vidioc_s_ctrl = atomisp_s_ctrl,
	.vidioc_g_ctrl = atomisp_g_ctrl,
	.vidioc_s_ext_ctrls = atomisp_s_ext_ctrls,
	.vidioc_g_ext_ctrls = atomisp_g_ext_ctrls,
	.vidioc_enum_framesizes   = atomisp_enum_framesizes,
	.vidioc_enum_frameintervals = atomisp_enum_frameintervals,
	.vidioc_enum_fmt_vid_cap = atomisp_enum_fmt_cap,
	.vidioc_try_fmt_vid_cap = atomisp_try_fmt_cap,
	.vidioc_g_fmt_vid_cap = atomisp_g_fmt_cap,
	.vidioc_s_fmt_vid_cap = atomisp_s_fmt_cap,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = atomisp_qbuf_wrapper,
	.vidioc_dqbuf = atomisp_dqbuf_wrapper,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_default = atomisp_vidioc_default,
	.vidioc_s_parm = atomisp_s_parm,
	.vidioc_g_parm = atomisp_g_parm,
};
