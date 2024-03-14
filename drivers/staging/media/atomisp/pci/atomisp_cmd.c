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
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/pm_runtime.h>
#include <linux/timer.h>

#include <asm/iosf_mbi.h>

#include <media/v4l2-event.h>

#define CREATE_TRACE_POINTS
#include "atomisp_trace_event.h"

#include "atomisp_cmd.h"
#include "atomisp_common.h"
#include "atomisp_fops.h"
#include "atomisp_internal.h"
#include "atomisp_ioctl.h"
#include "atomisp-regs.h"
#include "atomisp_tables.h"
#include "atomisp_compat.h"
#include "atomisp_subdev.h"
#include "atomisp_dfs_tables.h"

#include <hmm/hmm.h>

#include "sh_css_hrt.h"
#include "sh_css_defs.h"
#include "system_global.h"
#include "sh_css_internal.h"
#include "sh_css_sp.h"
#include "gp_device.h"
#include "device_access.h"
#include "irq.h"

#include "ia_css_types.h"
#include "ia_css_stream.h"
#include "ia_css_debug.h"
#include "bits.h"

/* We should never need to run the flash for more than 2 frames.
 * At 15fps this means 133ms. We set the timeout a bit longer.
 * Each flash driver is supposed to set its own timeout, but
 * just in case someone else changed the timeout, we set it
 * here to make sure we don't damage the flash hardware. */
#define FLASH_TIMEOUT 800 /* ms */

union host {
	struct {
		void *kernel_ptr;
		void __user *user_ptr;
		int size;
	} scalar;
	struct {
		void *hmm_ptr;
	} ptr;
};

/*
 * get sensor:dis71430/ov2720 related info from v4l2_subdev->priv data field.
 * subdev->priv is set in mrst.c
 */
struct camera_mipi_info *atomisp_to_sensor_mipi_info(struct v4l2_subdev *sd)
{
	return (struct camera_mipi_info *)v4l2_get_subdev_hostdata(sd);
}

/*
 * get struct atomisp_video_pipe from v4l2 video_device
 */
struct atomisp_video_pipe *atomisp_to_video_pipe(struct video_device *dev)
{
	return (struct atomisp_video_pipe *)
	       container_of(dev, struct atomisp_video_pipe, vdev);
}

static unsigned short atomisp_get_sensor_fps(struct atomisp_sub_device *asd)
{
	struct v4l2_subdev_frame_interval fi = { 0 };
	struct atomisp_device *isp = asd->isp;

	unsigned short fps = 0;
	int ret;

	ret = v4l2_subdev_call(isp->inputs[asd->input_curr].camera,
			       video, g_frame_interval, &fi);

	if (!ret && fi.interval.numerator)
		fps = fi.interval.denominator / fi.interval.numerator;

	return fps;
}

/*
 * DFS progress is shown as follows:
 * 1. Target frequency is calculated according to FPS/Resolution/ISP running
 *    mode.
 * 2. Ratio is calculated using formula: 2 * HPLL / target frequency - 1
 *    with proper rounding.
 * 3. Set ratio to ISPFREQ40, 1 to FREQVALID and ISPFREQGUAR40
 *    to 200MHz in ISPSSPM1.
 * 4. Wait for FREQVALID to be cleared by P-Unit.
 * 5. Wait for field ISPFREQSTAT40 in ISPSSPM1 turn to ratio set in 3.
 */
static int write_target_freq_to_hw(struct atomisp_device *isp,
				   unsigned int new_freq)
{
	unsigned int ratio, timeout, guar_ratio;
	u32 isp_sspm1 = 0;
	int i;

	if (!isp->hpll_freq) {
		dev_err(isp->dev, "failed to get hpll_freq. no change to freq\n");
		return -EINVAL;
	}

	iosf_mbi_read(BT_MBI_UNIT_PMC, MBI_REG_READ, ISPSSPM1, &isp_sspm1);
	if (isp_sspm1 & ISP_FREQ_VALID_MASK) {
		dev_dbg(isp->dev, "clearing ISPSSPM1 valid bit.\n");
		iosf_mbi_write(BT_MBI_UNIT_PMC, MBI_REG_WRITE, ISPSSPM1,
			       isp_sspm1 & ~(1 << ISP_FREQ_VALID_OFFSET));
	}

	ratio = (2 * isp->hpll_freq + new_freq / 2) / new_freq - 1;
	guar_ratio = (2 * isp->hpll_freq + 200 / 2) / 200 - 1;

	iosf_mbi_read(BT_MBI_UNIT_PMC, MBI_REG_READ, ISPSSPM1, &isp_sspm1);
	isp_sspm1 &= ~(0x1F << ISP_REQ_FREQ_OFFSET);

	for (i = 0; i < ISP_DFS_TRY_TIMES; i++) {
		iosf_mbi_write(BT_MBI_UNIT_PMC, MBI_REG_WRITE, ISPSSPM1,
			       isp_sspm1
			       | ratio << ISP_REQ_FREQ_OFFSET
			       | 1 << ISP_FREQ_VALID_OFFSET
			       | guar_ratio << ISP_REQ_GUAR_FREQ_OFFSET);

		iosf_mbi_read(BT_MBI_UNIT_PMC, MBI_REG_READ, ISPSSPM1, &isp_sspm1);
		timeout = 20;
		while ((isp_sspm1 & ISP_FREQ_VALID_MASK) && timeout) {
			iosf_mbi_read(BT_MBI_UNIT_PMC, MBI_REG_READ, ISPSSPM1, &isp_sspm1);
			dev_dbg(isp->dev, "waiting for ISPSSPM1 valid bit to be 0.\n");
			udelay(100);
			timeout--;
		}

		if (timeout != 0)
			break;
	}

	if (timeout == 0) {
		dev_err(isp->dev, "DFS failed due to HW error.\n");
		return -EINVAL;
	}

	iosf_mbi_read(BT_MBI_UNIT_PMC, MBI_REG_READ, ISPSSPM1, &isp_sspm1);
	timeout = 10;
	while (((isp_sspm1 >> ISP_FREQ_STAT_OFFSET) != ratio) && timeout) {
		iosf_mbi_read(BT_MBI_UNIT_PMC, MBI_REG_READ, ISPSSPM1, &isp_sspm1);
		dev_dbg(isp->dev, "waiting for ISPSSPM1 status bit to be 0x%x.\n",
			new_freq);
		udelay(100);
		timeout--;
	}
	if (timeout == 0) {
		dev_err(isp->dev, "DFS target freq is rejected by HW.\n");
		return -EINVAL;
	}

	return 0;
}

int atomisp_freq_scaling(struct atomisp_device *isp,
			 enum atomisp_dfs_mode mode,
			 bool force)
{
	const struct atomisp_dfs_config *dfs;
	unsigned int new_freq;
	struct atomisp_freq_scaling_rule curr_rules;
	int i, ret;
	unsigned short fps = 0;

	dfs = isp->dfs;

	if (dfs->lowest_freq == 0 || dfs->max_freq_at_vmin == 0 ||
	    dfs->highest_freq == 0 || dfs->dfs_table_size == 0 ||
	    !dfs->dfs_table) {
		dev_err(isp->dev, "DFS configuration is invalid.\n");
		return -EINVAL;
	}

	if (mode == ATOMISP_DFS_MODE_LOW) {
		new_freq = dfs->lowest_freq;
		goto done;
	}

	if (mode == ATOMISP_DFS_MODE_MAX) {
		new_freq = dfs->highest_freq;
		goto done;
	}

	fps = atomisp_get_sensor_fps(&isp->asd);
	if (fps == 0) {
		dev_info(isp->dev,
			 "Sensor didn't report FPS. Using DFS max mode.\n");
		new_freq = dfs->highest_freq;
		goto done;
	}

	curr_rules.width = isp->asd.fmt[ATOMISP_SUBDEV_PAD_SOURCE].fmt.width;
	curr_rules.height = isp->asd.fmt[ATOMISP_SUBDEV_PAD_SOURCE].fmt.height;
	curr_rules.fps = fps;
	curr_rules.run_mode = isp->asd.run_mode->val;

	/* search for the target frequency by looping freq rules*/
	for (i = 0; i < dfs->dfs_table_size; i++) {
		if (curr_rules.width != dfs->dfs_table[i].width &&
		    dfs->dfs_table[i].width != ISP_FREQ_RULE_ANY)
			continue;
		if (curr_rules.height != dfs->dfs_table[i].height &&
		    dfs->dfs_table[i].height != ISP_FREQ_RULE_ANY)
			continue;
		if (curr_rules.fps != dfs->dfs_table[i].fps &&
		    dfs->dfs_table[i].fps != ISP_FREQ_RULE_ANY)
			continue;
		if (curr_rules.run_mode != dfs->dfs_table[i].run_mode &&
		    dfs->dfs_table[i].run_mode != ISP_FREQ_RULE_ANY)
			continue;
		break;
	}

	if (i == dfs->dfs_table_size)
		new_freq = dfs->max_freq_at_vmin;
	else
		new_freq = dfs->dfs_table[i].isp_freq;

done:
	dev_dbg(isp->dev, "DFS target frequency=%d.\n", new_freq);

	if ((new_freq == isp->running_freq) && !force)
		return 0;

	dev_dbg(isp->dev, "Programming DFS frequency to %d\n", new_freq);

	ret = write_target_freq_to_hw(isp, new_freq);
	if (!ret) {
		isp->running_freq = new_freq;
		trace_ipu_pstate(new_freq, -1);
	}
	return ret;
}

/*
 * reset and restore ISP
 */
int atomisp_reset(struct atomisp_device *isp)
{
	/* Reset ISP by power-cycling it */
	int ret = 0;

	dev_dbg(isp->dev, "%s\n", __func__);

	ret = atomisp_power_off(isp->dev);
	if (ret < 0)
		dev_err(isp->dev, "atomisp_power_off failed, %d\n", ret);

	ret = atomisp_power_on(isp->dev);
	if (ret < 0) {
		dev_err(isp->dev, "atomisp_power_on failed, %d\n", ret);
		isp->isp_fatal_error = true;
	}

	return ret;
}

/*
 * interrupt disable functions
 */
static void disable_isp_irq(enum hrt_isp_css_irq irq)
{
	irq_disable_channel(IRQ0_ID, irq);

	if (irq != hrt_isp_css_irq_sp)
		return;

	cnd_sp_irq_enable(SP0_ID, false);
}

/*
 * interrupt clean function
 */
static void clear_isp_irq(enum hrt_isp_css_irq irq)
{
	irq_clear_all(IRQ0_ID);
}

void atomisp_msi_irq_init(struct atomisp_device *isp)
{
	struct pci_dev *pdev = to_pci_dev(isp->dev);
	u32 msg32;
	u16 msg16;

	pci_read_config_dword(pdev, PCI_MSI_CAPID, &msg32);
	msg32 |= 1 << MSI_ENABLE_BIT;
	pci_write_config_dword(pdev, PCI_MSI_CAPID, msg32);

	msg32 = (1 << INTR_IER) | (1 << INTR_IIR);
	pci_write_config_dword(pdev, PCI_INTERRUPT_CTRL, msg32);

	pci_read_config_word(pdev, PCI_COMMAND, &msg16);
	msg16 |= (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |
		  PCI_COMMAND_INTX_DISABLE);
	pci_write_config_word(pdev, PCI_COMMAND, msg16);
}

void atomisp_msi_irq_uninit(struct atomisp_device *isp)
{
	struct pci_dev *pdev = to_pci_dev(isp->dev);
	u32 msg32;
	u16 msg16;

	pci_read_config_dword(pdev, PCI_MSI_CAPID, &msg32);
	msg32 &=  ~(1 << MSI_ENABLE_BIT);
	pci_write_config_dword(pdev, PCI_MSI_CAPID, msg32);

	msg32 = 0x0;
	pci_write_config_dword(pdev, PCI_INTERRUPT_CTRL, msg32);

	pci_read_config_word(pdev, PCI_COMMAND, &msg16);
	msg16 &= ~(PCI_COMMAND_MASTER);
	pci_write_config_word(pdev, PCI_COMMAND, msg16);
}

static void atomisp_sof_event(struct atomisp_sub_device *asd)
{
	struct v4l2_event event = {0};

	event.type = V4L2_EVENT_FRAME_SYNC;
	event.u.frame_sync.frame_sequence = atomic_read(&asd->sof_count);

	v4l2_event_queue(asd->subdev.devnode, &event);
}

void atomisp_eof_event(struct atomisp_sub_device *asd, uint8_t exp_id)
{
	struct v4l2_event event = {0};

	event.type = V4L2_EVENT_FRAME_END;
	event.u.frame_sync.frame_sequence = exp_id;

	v4l2_event_queue(asd->subdev.devnode, &event);
}

static void atomisp_3a_stats_ready_event(struct atomisp_sub_device *asd,
	uint8_t exp_id)
{
	struct v4l2_event event = {0};

	event.type = V4L2_EVENT_ATOMISP_3A_STATS_READY;
	event.u.frame_sync.frame_sequence = exp_id;

	v4l2_event_queue(asd->subdev.devnode, &event);
}

static void atomisp_metadata_ready_event(struct atomisp_sub_device *asd,
	enum atomisp_metadata_type md_type)
{
	struct v4l2_event event = {0};

	event.type = V4L2_EVENT_ATOMISP_METADATA_READY;
	event.u.data[0] = md_type;

	v4l2_event_queue(asd->subdev.devnode, &event);
}

static void atomisp_reset_event(struct atomisp_sub_device *asd)
{
	struct v4l2_event event = {0};

	event.type = V4L2_EVENT_ATOMISP_CSS_RESET;

	v4l2_event_queue(asd->subdev.devnode, &event);
}

static void print_csi_rx_errors(enum mipi_port_id port,
				struct atomisp_device *isp)
{
	u32 infos = 0;

	atomisp_css_rx_get_irq_info(port, &infos);

	dev_err(isp->dev, "CSI Receiver port %d errors:\n", port);
	if (infos & IA_CSS_RX_IRQ_INFO_BUFFER_OVERRUN)
		dev_err(isp->dev, "  buffer overrun");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_SOT)
		dev_err(isp->dev, "  start-of-transmission error");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_SOT_SYNC)
		dev_err(isp->dev, "  start-of-transmission sync error");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_CONTROL)
		dev_err(isp->dev, "  control error");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_ECC_DOUBLE)
		dev_err(isp->dev, "  2 or more ECC errors");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_CRC)
		dev_err(isp->dev, "  CRC mismatch");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_UNKNOWN_ID)
		dev_err(isp->dev, "  unknown error");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_FRAME_SYNC)
		dev_err(isp->dev, "  frame sync error");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_FRAME_DATA)
		dev_err(isp->dev, "  frame data error");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_DATA_TIMEOUT)
		dev_err(isp->dev, "  data timeout");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_UNKNOWN_ESC)
		dev_err(isp->dev, "  unknown escape command entry");
	if (infos & IA_CSS_RX_IRQ_INFO_ERR_LINE_SYNC)
		dev_err(isp->dev, "  line sync error");
}

/* Clear irq reg */
static void clear_irq_reg(struct atomisp_device *isp)
{
	struct pci_dev *pdev = to_pci_dev(isp->dev);
	u32 msg_ret;

	pci_read_config_dword(pdev, PCI_INTERRUPT_CTRL, &msg_ret);
	msg_ret |= 1 << INTR_IIR;
	pci_write_config_dword(pdev, PCI_INTERRUPT_CTRL, msg_ret);
}

/* interrupt handling function*/
irqreturn_t atomisp_isr(int irq, void *dev)
{
	struct atomisp_device *isp = (struct atomisp_device *)dev;
	struct atomisp_css_event eof_event;
	unsigned int irq_infos = 0;
	unsigned long flags;
	int err;

	spin_lock_irqsave(&isp->lock, flags);

	if (!isp->css_initialized) {
		spin_unlock_irqrestore(&isp->lock, flags);
		return IRQ_HANDLED;
	}
	err = atomisp_css_irq_translate(isp, &irq_infos);
	if (err) {
		spin_unlock_irqrestore(&isp->lock, flags);
		return IRQ_NONE;
	}

	clear_irq_reg(isp);

	if (!isp->asd.streaming)
		goto out_nowake;

	if (irq_infos & IA_CSS_IRQ_INFO_CSS_RECEIVER_SOF) {
		atomic_inc(&isp->asd.sof_count);
		atomisp_sof_event(&isp->asd);

		/*
		 * If sequence_temp and sequence are the same there where no frames
		 * lost so we can increase sequence_temp.
		 * If not then processing of frame is still in progress and driver
		 * needs to keep old sequence_temp value.
		 * NOTE: There is assumption here that ISP will not start processing
		 * next frame from sensor before old one is completely done.
		 */
		if (atomic_read(&isp->asd.sequence) == atomic_read(&isp->asd.sequence_temp))
			atomic_set(&isp->asd.sequence_temp, atomic_read(&isp->asd.sof_count));

		dev_dbg_ratelimited(isp->dev, "irq:0x%x (SOF)\n", irq_infos);
		irq_infos &= ~IA_CSS_IRQ_INFO_CSS_RECEIVER_SOF;
	}

	if (irq_infos & IA_CSS_IRQ_INFO_EVENTS_READY)
		atomic_set(&isp->asd.sequence, atomic_read(&isp->asd.sequence_temp));

	if ((irq_infos & IA_CSS_IRQ_INFO_INPUT_SYSTEM_ERROR) ||
	    (irq_infos & IA_CSS_IRQ_INFO_IF_ERROR)) {
		/* handle mipi receiver error */
		u32 rx_infos;
		enum mipi_port_id port;

		for (port = MIPI_PORT0_ID; port <= MIPI_PORT2_ID;
		     port++) {
			print_csi_rx_errors(port, isp);
			atomisp_css_rx_get_irq_info(port, &rx_infos);
			atomisp_css_rx_clear_irq_info(port, rx_infos);
		}
	}

	if (irq_infos & IA_CSS_IRQ_INFO_ISYS_EVENTS_READY) {
		while (ia_css_dequeue_isys_event(&eof_event.event) == 0) {
			atomisp_eof_event(&isp->asd, eof_event.event.exp_id);
			dev_dbg_ratelimited(isp->dev, "ISYS event: EOF exp_id %d\n",
					    eof_event.event.exp_id);
		}

		irq_infos &= ~IA_CSS_IRQ_INFO_ISYS_EVENTS_READY;
		if (irq_infos == 0)
			goto out_nowake;
	}

	spin_unlock_irqrestore(&isp->lock, flags);

	dev_dbg_ratelimited(isp->dev, "irq:0x%x (unhandled)\n", irq_infos);

	return IRQ_WAKE_THREAD;

out_nowake:
	spin_unlock_irqrestore(&isp->lock, flags);

	if (irq_infos)
		dev_dbg_ratelimited(isp->dev, "irq:0x%x (ignored, as not streaming anymore)\n",
				    irq_infos);

	return IRQ_HANDLED;
}

void atomisp_clear_css_buffer_counters(struct atomisp_sub_device *asd)
{
	int i;

	memset(asd->s3a_bufs_in_css, 0, sizeof(asd->s3a_bufs_in_css));
	for (i = 0; i < ATOMISP_INPUT_STREAM_NUM; i++)
		memset(asd->metadata_bufs_in_css[i], 0,
		       sizeof(asd->metadata_bufs_in_css[i]));
	asd->dis_bufs_in_css = 0;
}

/* 0x100000 is the start of dmem inside SP */
#define SP_DMEM_BASE	0x100000

void dump_sp_dmem(struct atomisp_device *isp, unsigned int addr,
		  unsigned int size)
{
	unsigned int data = 0;
	unsigned int size32 = DIV_ROUND_UP(size, sizeof(u32));

	dev_dbg(isp->dev, "atomisp mmio base: %p\n", isp->base);
	dev_dbg(isp->dev, "%s, addr:0x%x, size: %d, size32: %d\n", __func__,
		addr, size, size32);
	if (size32 * 4 + addr > 0x4000) {
		dev_err(isp->dev, "illegal size (%d) or addr (0x%x)\n",
			size32, addr);
		return;
	}
	addr += SP_DMEM_BASE;
	addr &= 0x003FFFFF;
	do {
		data = readl(isp->base + addr);
		dev_dbg(isp->dev, "%s, \t [0x%x]:0x%x\n", __func__, addr, data);
		addr += sizeof(u32);
	} while (--size32);
}

int atomisp_buffers_in_css(struct atomisp_video_pipe *pipe)
{
	unsigned long irqflags;
	struct list_head *pos;
	int buffers_in_css = 0;

	spin_lock_irqsave(&pipe->irq_lock, irqflags);

	list_for_each(pos, &pipe->buffers_in_css)
		buffers_in_css++;

	spin_unlock_irqrestore(&pipe->irq_lock, irqflags);

	return buffers_in_css;
}

void atomisp_buffer_done(struct ia_css_frame *frame, enum vb2_buffer_state state)
{
	struct atomisp_video_pipe *pipe = vb_to_pipe(&frame->vb.vb2_buf);

	lockdep_assert_held(&pipe->irq_lock);

	frame->vb.vb2_buf.timestamp = ktime_get_ns();
	frame->vb.field = pipe->pix.field;
	frame->vb.sequence = atomic_read(&pipe->asd->sequence);
	list_del(&frame->queue);
	if (state == VB2_BUF_STATE_DONE)
		vb2_set_plane_payload(&frame->vb.vb2_buf, 0, pipe->pix.sizeimage);
	vb2_buffer_done(&frame->vb.vb2_buf, state);
}

void atomisp_flush_video_pipe(struct atomisp_video_pipe *pipe, enum vb2_buffer_state state,
			      bool warn_on_css_frames)
{
	struct ia_css_frame *frame, *_frame;
	unsigned long irqflags;

	spin_lock_irqsave(&pipe->irq_lock, irqflags);

	list_for_each_entry_safe(frame, _frame, &pipe->buffers_in_css, queue) {
		if (warn_on_css_frames)
			dev_warn(pipe->isp->dev, "Warning: CSS frames queued on flush\n");
		atomisp_buffer_done(frame, state);
	}

	list_for_each_entry_safe(frame, _frame, &pipe->activeq, queue)
		atomisp_buffer_done(frame, state);

	list_for_each_entry_safe(frame, _frame, &pipe->buffers_waiting_for_param, queue) {
		pipe->frame_request_config_id[frame->vb.vb2_buf.index] = 0;
		atomisp_buffer_done(frame, state);
	}

	spin_unlock_irqrestore(&pipe->irq_lock, irqflags);
}

/* clean out the parameters that did not apply */
void atomisp_flush_params_queue(struct atomisp_video_pipe *pipe)
{
	struct atomisp_css_params_with_list *param;

	while (!list_empty(&pipe->per_frame_params)) {
		param = list_entry(pipe->per_frame_params.next,
				   struct atomisp_css_params_with_list, list);
		list_del(&param->list);
		atomisp_free_css_parameters(&param->params);
		kvfree(param);
	}
}

/* Re-queue per-frame parameters */
static void atomisp_recover_params_queue(struct atomisp_video_pipe *pipe)
{
	struct atomisp_css_params_with_list *param;
	int i;

	for (i = 0; i < VIDEO_MAX_FRAME; i++) {
		param = pipe->frame_params[i];
		if (param)
			list_add_tail(&param->list, &pipe->per_frame_params);
		pipe->frame_params[i] = NULL;
	}
	atomisp_handle_parameter_and_buffer(pipe);
}

void atomisp_buf_done(struct atomisp_sub_device *asd, int error,
		      enum ia_css_buffer_type buf_type,
		      enum ia_css_pipe_id css_pipe_id,
		      bool q_buffers, enum atomisp_input_stream_id stream_id)
{
	struct atomisp_video_pipe *pipe = NULL;
	struct atomisp_css_buffer buffer;
	bool requeue = false;
	unsigned long irqflags;
	struct ia_css_frame *frame = NULL;
	struct atomisp_s3a_buf *s3a_buf = NULL, *_s3a_buf_tmp, *s3a_iter;
	struct atomisp_dis_buf *dis_buf = NULL, *_dis_buf_tmp, *dis_iter;
	struct atomisp_metadata_buf *md_buf = NULL, *_md_buf_tmp, *md_iter;
	enum atomisp_metadata_type md_type;
	struct atomisp_device *isp = asd->isp;
	struct v4l2_control ctrl;
	int i, err;

	lockdep_assert_held(&isp->mutex);

	if (
	    buf_type != IA_CSS_BUFFER_TYPE_METADATA &&
	    buf_type != IA_CSS_BUFFER_TYPE_3A_STATISTICS &&
	    buf_type != IA_CSS_BUFFER_TYPE_DIS_STATISTICS &&
	    buf_type != IA_CSS_BUFFER_TYPE_OUTPUT_FRAME &&
	    buf_type != IA_CSS_BUFFER_TYPE_SEC_OUTPUT_FRAME &&
	    buf_type != IA_CSS_BUFFER_TYPE_RAW_OUTPUT_FRAME &&
	    buf_type != IA_CSS_BUFFER_TYPE_SEC_VF_OUTPUT_FRAME &&
	    buf_type != IA_CSS_BUFFER_TYPE_VF_OUTPUT_FRAME) {
		dev_err(isp->dev, "%s, unsupported buffer type: %d\n",
			__func__, buf_type);
		return;
	}

	memset(&buffer, 0, sizeof(struct atomisp_css_buffer));
	buffer.css_buffer.type = buf_type;
	err = atomisp_css_dequeue_buffer(asd, stream_id, css_pipe_id,
					 buf_type, &buffer);
	if (err) {
		dev_err(isp->dev,
			"atomisp_css_dequeue_buffer failed: 0x%x\n", err);
		return;
	}

	switch (buf_type) {
	case IA_CSS_BUFFER_TYPE_3A_STATISTICS:
		list_for_each_entry_safe(s3a_iter, _s3a_buf_tmp,
					 &asd->s3a_stats_in_css, list) {
			if (s3a_iter->s3a_data ==
			    buffer.css_buffer.data.stats_3a) {
				list_del_init(&s3a_iter->list);
				list_add_tail(&s3a_iter->list,
					      &asd->s3a_stats_ready);
				s3a_buf = s3a_iter;
				break;
			}
		}

		asd->s3a_bufs_in_css[css_pipe_id]--;
		atomisp_3a_stats_ready_event(asd, buffer.css_buffer.exp_id);
		if (s3a_buf)
			dev_dbg(isp->dev, "%s: s3a stat with exp_id %d is ready\n",
				__func__, s3a_buf->s3a_data->exp_id);
		else
			dev_dbg(isp->dev, "%s: s3a stat is ready with no exp_id found\n",
				__func__);
		break;
	case IA_CSS_BUFFER_TYPE_METADATA:
		if (error)
			break;

		md_type = ATOMISP_MAIN_METADATA;
		list_for_each_entry_safe(md_iter, _md_buf_tmp,
					 &asd->metadata_in_css[md_type], list) {
			if (md_iter->metadata ==
			    buffer.css_buffer.data.metadata) {
				list_del_init(&md_iter->list);
				list_add_tail(&md_iter->list,
					      &asd->metadata_ready[md_type]);
				md_buf = md_iter;
				break;
			}
		}
		asd->metadata_bufs_in_css[stream_id][css_pipe_id]--;
		atomisp_metadata_ready_event(asd, md_type);
		if (md_buf)
			dev_dbg(isp->dev, "%s: metadata with exp_id %d is ready\n",
				__func__, md_buf->metadata->exp_id);
		else
			dev_dbg(isp->dev, "%s: metadata is ready with no exp_id found\n",
				__func__);
		break;
	case IA_CSS_BUFFER_TYPE_DIS_STATISTICS:
		list_for_each_entry_safe(dis_iter, _dis_buf_tmp,
					 &asd->dis_stats_in_css, list) {
			if (dis_iter->dis_data ==
			    buffer.css_buffer.data.stats_dvs) {
				spin_lock_irqsave(&asd->dis_stats_lock,
						  irqflags);
				list_del_init(&dis_iter->list);
				list_add(&dis_iter->list, &asd->dis_stats);
				asd->params.dis_proj_data_valid = true;
				spin_unlock_irqrestore(&asd->dis_stats_lock,
						       irqflags);
				dis_buf = dis_iter;
				break;
			}
		}
		asd->dis_bufs_in_css--;
		if (dis_buf)
			dev_dbg(isp->dev, "%s: dis stat with exp_id %d is ready\n",
				__func__, dis_buf->dis_data->exp_id);
		else
			dev_dbg(isp->dev, "%s: dis stat is ready with no exp_id found\n",
				__func__);
		break;
	case IA_CSS_BUFFER_TYPE_VF_OUTPUT_FRAME:
	case IA_CSS_BUFFER_TYPE_SEC_VF_OUTPUT_FRAME:
		frame = buffer.css_buffer.data.frame;
		if (!frame) {
			WARN_ON(1);
			break;
		}
		if (!frame->valid)
			error = true;

		pipe = vb_to_pipe(&frame->vb.vb2_buf);

		dev_dbg(isp->dev, "%s: vf frame with exp_id %d is ready\n",
			__func__, frame->exp_id);
		if (asd->params.flash_state == ATOMISP_FLASH_ONGOING) {
			if (frame->flash_state
			    == IA_CSS_FRAME_FLASH_STATE_PARTIAL)
				dev_dbg(isp->dev, "%s thumb partially flashed\n",
					__func__);
			else if (frame->flash_state
				 == IA_CSS_FRAME_FLASH_STATE_FULL)
				dev_dbg(isp->dev, "%s thumb completely flashed\n",
					__func__);
			else
				dev_dbg(isp->dev, "%s thumb no flash in this frame\n",
					__func__);
		}
		pipe->frame_config_id[frame->vb.vb2_buf.index] = frame->isp_config_id;
		break;
	case IA_CSS_BUFFER_TYPE_OUTPUT_FRAME:
	case IA_CSS_BUFFER_TYPE_SEC_OUTPUT_FRAME:
		frame = buffer.css_buffer.data.frame;
		if (!frame) {
			WARN_ON(1);
			break;
		}

		if (!frame->valid)
			error = true;

		pipe = vb_to_pipe(&frame->vb.vb2_buf);

		dev_dbg(isp->dev, "%s: main frame with exp_id %d is ready\n",
			__func__, frame->exp_id);

		i = frame->vb.vb2_buf.index;

		/* free the parameters */
		if (pipe->frame_params[i]) {
			if (asd->params.dvs_6axis == pipe->frame_params[i]->params.dvs_6axis)
				asd->params.dvs_6axis = NULL;
			atomisp_free_css_parameters(&pipe->frame_params[i]->params);
			kvfree(pipe->frame_params[i]);
			pipe->frame_params[i] = NULL;
		}

		pipe->frame_config_id[i] = frame->isp_config_id;
		ctrl.id = V4L2_CID_FLASH_MODE;
		if (asd->params.flash_state == ATOMISP_FLASH_ONGOING) {
			if (frame->flash_state == IA_CSS_FRAME_FLASH_STATE_PARTIAL) {
				asd->frame_status[i] = ATOMISP_FRAME_STATUS_FLASH_PARTIAL;
				dev_dbg(isp->dev, "%s partially flashed\n", __func__);
			} else if (frame->flash_state == IA_CSS_FRAME_FLASH_STATE_FULL) {
				asd->frame_status[i] = ATOMISP_FRAME_STATUS_FLASH_EXPOSED;
				asd->params.num_flash_frames--;
				dev_dbg(isp->dev, "%s completely flashed\n", __func__);
			} else {
				asd->frame_status[i] = ATOMISP_FRAME_STATUS_OK;
				dev_dbg(isp->dev, "%s no flash in this frame\n", __func__);
			}

			/* Check if flashing sequence is done */
			if (asd->frame_status[i] == ATOMISP_FRAME_STATUS_FLASH_EXPOSED)
				asd->params.flash_state = ATOMISP_FLASH_DONE;
		} else if (isp->flash) {
			if (v4l2_g_ctrl(isp->flash->ctrl_handler, &ctrl) == 0 &&
			    ctrl.value == ATOMISP_FLASH_MODE_TORCH) {
				ctrl.id = V4L2_CID_FLASH_TORCH_INTENSITY;
				if (v4l2_g_ctrl(isp->flash->ctrl_handler, &ctrl) == 0 &&
				    ctrl.value > 0)
					asd->frame_status[i] = ATOMISP_FRAME_STATUS_FLASH_EXPOSED;
				else
					asd->frame_status[i] = ATOMISP_FRAME_STATUS_OK;
			} else {
				asd->frame_status[i] = ATOMISP_FRAME_STATUS_OK;
			}
		} else {
			asd->frame_status[i] = ATOMISP_FRAME_STATUS_OK;
		}

		asd->params.last_frame_status = asd->frame_status[i];

		if (asd->params.css_update_params_needed) {
			atomisp_apply_css_parameters(asd,
						     &asd->params.css_param);
			if (asd->params.css_param.update_flag.dz_config)
				asd->params.config.dz_config = &asd->params.css_param.dz_config;
			/* New global dvs 6axis config should be blocked
			 * here if there's a buffer with per-frame parameters
			 * pending in CSS frame buffer queue.
			 * This is to aviod zooming vibration since global
			 * parameters take effect immediately while
			 * per-frame parameters are taken after previous
			 * buffers in CSS got processed.
			 */
			if (asd->params.dvs_6axis)
				atomisp_css_set_dvs_6axis(asd,
							  asd->params.dvs_6axis);
			else
				asd->params.css_update_params_needed = false;
			/* The update flag should not be cleaned here
			 * since it is still going to be used to make up
			 * following per-frame parameters.
			 * This will introduce more copy work since each
			 * time when updating global parameters, the whole
			 * parameter set are applied.
			 * FIXME: A new set of parameter copy functions can
			 * be added to make up per-frame parameters based on
			 * solid structures stored in asd->params.css_param
			 * instead of using shadow pointers in update flag.
			 */
			atomisp_css_update_isp_params(asd);
		}
		break;
	default:
		break;
	}
	if (frame) {
		spin_lock_irqsave(&pipe->irq_lock, irqflags);
		atomisp_buffer_done(frame, error ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
		spin_unlock_irqrestore(&pipe->irq_lock, irqflags);
	}

	/*
	 * Requeue should only be done for 3a and dis buffers.
	 * Queue/dequeue order will change if driver recycles image buffers.
	 */
	if (requeue) {
		err = atomisp_css_queue_buffer(asd,
					       stream_id, css_pipe_id,
					       buf_type, &buffer);
		if (err)
			dev_err(isp->dev, "%s, q to css fails: %d\n",
				__func__, err);
		return;
	}
	if (!error && q_buffers)
		atomisp_qbuffers_to_css(asd);
}

void atomisp_assert_recovery_work(struct work_struct *work)
{
	struct atomisp_device *isp = container_of(work, struct atomisp_device,
						  assert_recovery_work);
	struct pci_dev *pdev = to_pci_dev(isp->dev);
	unsigned long flags;
	int ret;

	mutex_lock(&isp->mutex);

	if (!isp->asd.streaming)
		goto out_unlock;

	atomisp_css_irq_enable(isp, IA_CSS_IRQ_INFO_CSS_RECEIVER_SOF, false);

	spin_lock_irqsave(&isp->lock, flags);
	isp->asd.streaming = false;
	spin_unlock_irqrestore(&isp->lock, flags);

	/* stream off sensor */
	ret = v4l2_subdev_call(isp->inputs[isp->asd.input_curr].camera, video, s_stream, 0);
	if (ret)
		dev_warn(isp->dev, "Stopping sensor stream failed: %d\n", ret);

	atomisp_clear_css_buffer_counters(&isp->asd);

	atomisp_css_stop(&isp->asd, true);

	isp->asd.preview_exp_id = 1;
	isp->asd.postview_exp_id = 1;
	/* notify HAL the CSS reset */
	dev_dbg(isp->dev, "send reset event to %s\n", isp->asd.subdev.devnode->name);
	atomisp_reset_event(&isp->asd);

	/* clear irq */
	disable_isp_irq(hrt_isp_css_irq_sp);
	clear_isp_irq(hrt_isp_css_irq_sp);

	/* Set the SRSE to 3 before resetting */
	pci_write_config_dword(pdev, PCI_I_CONTROL,
			       isp->saved_regs.i_control | MRFLD_PCI_I_CONTROL_SRSE_RESET_MASK);

	/* reset ISP and restore its state */
	atomisp_reset(isp);

	atomisp_css_input_set_mode(&isp->asd, IA_CSS_INPUT_MODE_BUFFERED_SENSOR);

	/* Recreate streams destroyed by atomisp_css_stop() */
	atomisp_create_pipes_stream(&isp->asd);

	/* Invalidate caches. FIXME: should flush only necessary buffers */
	wbinvd();

	if (atomisp_css_start(&isp->asd)) {
		dev_warn(isp->dev, "start SP failed, so do not set streaming to be enable!\n");
	} else {
		spin_lock_irqsave(&isp->lock, flags);
		isp->asd.streaming = true;
		spin_unlock_irqrestore(&isp->lock, flags);
	}

	atomisp_csi2_configure(&isp->asd);

	atomisp_css_irq_enable(isp, IA_CSS_IRQ_INFO_CSS_RECEIVER_SOF,
			       atomisp_css_valid_sof(isp));

	if (atomisp_freq_scaling(isp, ATOMISP_DFS_MODE_AUTO, true) < 0)
		dev_dbg(isp->dev, "DFS auto failed while recovering!\n");

	/* Dequeueing buffers is not needed, CSS will recycle buffers that it has */
	atomisp_flush_video_pipe(&isp->asd.video_out, VB2_BUF_STATE_ERROR, false);

	/* Requeue unprocessed per-frame parameters. */
	atomisp_recover_params_queue(&isp->asd.video_out);

	ret = v4l2_subdev_call(isp->inputs[isp->asd.input_curr].camera, video, s_stream, 1);
	if (ret)
		dev_err(isp->dev, "Starting sensor stream failed: %d\n", ret);

out_unlock:
	mutex_unlock(&isp->mutex);
}

void atomisp_setup_flash(struct atomisp_sub_device *asd)
{
	struct atomisp_device *isp = asd->isp;
	struct v4l2_control ctrl;

	if (!isp->flash)
		return;

	if (asd->params.flash_state != ATOMISP_FLASH_REQUESTED &&
	    asd->params.flash_state != ATOMISP_FLASH_DONE)
		return;

	if (asd->params.num_flash_frames) {
		/* make sure the timeout is set before setting flash mode */
		ctrl.id = V4L2_CID_FLASH_TIMEOUT;
		ctrl.value = FLASH_TIMEOUT;

		if (v4l2_s_ctrl(NULL, isp->flash->ctrl_handler, &ctrl)) {
			dev_err(isp->dev, "flash timeout configure failed\n");
			return;
		}

		ia_css_stream_request_flash(asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream);

		asd->params.flash_state = ATOMISP_FLASH_ONGOING;
	} else {
		asd->params.flash_state = ATOMISP_FLASH_IDLE;
	}
}

irqreturn_t atomisp_isr_thread(int irq, void *isp_ptr)
{
	struct atomisp_device *isp = isp_ptr;
	unsigned long flags;

	dev_dbg(isp->dev, ">%s\n", __func__);

	spin_lock_irqsave(&isp->lock, flags);

	if (!isp->asd.streaming) {
		spin_unlock_irqrestore(&isp->lock, flags);
		return IRQ_HANDLED;
	}

	spin_unlock_irqrestore(&isp->lock, flags);

	/*
	 * The standard CSS2.0 API tells the following calling sequence of
	 * dequeue ready buffers:
	 * while (ia_css_dequeue_psys_event(...)) {
	 *	switch (event.type) {
	 *	...
	 *	ia_css_pipe_dequeue_buffer()
	 *	}
	 * }
	 * That is, dequeue event and buffer are one after another.
	 *
	 * But the following implementation is to first deuque all the event
	 * to a FIFO, then process the event in the FIFO.
	 * This will not have issue in single stream mode, but it do have some
	 * issue in multiple stream case. The issue is that
	 * ia_css_pipe_dequeue_buffer() will not return the corrent buffer in
	 * a specific pipe.
	 *
	 * This is due to ia_css_pipe_dequeue_buffer() does not take the
	 * ia_css_pipe parameter.
	 *
	 * So:
	 * For CSS2.0: we change the way to not dequeue all the event at one
	 * time, instead, dequue one and process one, then another
	 */
	mutex_lock(&isp->mutex);
	if (atomisp_css_isr_thread(isp))
		goto out;

	if (isp->asd.streaming)
		atomisp_setup_flash(&isp->asd);
out:
	mutex_unlock(&isp->mutex);
	dev_dbg(isp->dev, "<%s\n", __func__);

	return IRQ_HANDLED;
}

/*
 * Get internal fmt according to V4L2 fmt
 */
static enum ia_css_frame_format
v4l2_fmt_to_sh_fmt(u32 fmt)
{
	switch (fmt) {
	case V4L2_PIX_FMT_YUV420:
				return IA_CSS_FRAME_FORMAT_YUV420;
	case V4L2_PIX_FMT_YVU420:
		return IA_CSS_FRAME_FORMAT_YV12;
	case V4L2_PIX_FMT_YUV422P:
		return IA_CSS_FRAME_FORMAT_YUV422;
	case V4L2_PIX_FMT_YUV444:
		return IA_CSS_FRAME_FORMAT_YUV444;
	case V4L2_PIX_FMT_NV12:
		return IA_CSS_FRAME_FORMAT_NV12;
	case V4L2_PIX_FMT_NV21:
		return IA_CSS_FRAME_FORMAT_NV21;
	case V4L2_PIX_FMT_NV16:
		return IA_CSS_FRAME_FORMAT_NV16;
	case V4L2_PIX_FMT_NV61:
		return IA_CSS_FRAME_FORMAT_NV61;
	case V4L2_PIX_FMT_UYVY:
		return IA_CSS_FRAME_FORMAT_UYVY;
	case V4L2_PIX_FMT_YUYV:
		return IA_CSS_FRAME_FORMAT_YUYV;
	case V4L2_PIX_FMT_RGB24:
		return IA_CSS_FRAME_FORMAT_PLANAR_RGB888;
	case V4L2_PIX_FMT_RGB32:
		return IA_CSS_FRAME_FORMAT_RGBA888;
	case V4L2_PIX_FMT_RGB565:
		return IA_CSS_FRAME_FORMAT_RGB565;
#if 0
	case V4L2_PIX_FMT_JPEG:
	case V4L2_PIX_FMT_CUSTOM_M10MO_RAW:
		return IA_CSS_FRAME_FORMAT_BINARY_8;
#endif
	case V4L2_PIX_FMT_SBGGR16:
	case V4L2_PIX_FMT_SBGGR10:
	case V4L2_PIX_FMT_SGBRG10:
	case V4L2_PIX_FMT_SGRBG10:
	case V4L2_PIX_FMT_SRGGB10:
	case V4L2_PIX_FMT_SBGGR12:
	case V4L2_PIX_FMT_SGBRG12:
	case V4L2_PIX_FMT_SGRBG12:
	case V4L2_PIX_FMT_SRGGB12:
	case V4L2_PIX_FMT_SBGGR8:
	case V4L2_PIX_FMT_SGBRG8:
	case V4L2_PIX_FMT_SGRBG8:
	case V4L2_PIX_FMT_SRGGB8:
		return IA_CSS_FRAME_FORMAT_RAW;
	default:
		return -EINVAL;
	}
}

/*
 * raw format match between SH format and V4L2 format
 */
static int raw_output_format_match_input(u32 input, u32 output)
{
	if ((input == ATOMISP_INPUT_FORMAT_RAW_12) &&
	    ((output == V4L2_PIX_FMT_SRGGB12) ||
	     (output == V4L2_PIX_FMT_SGRBG12) ||
	     (output == V4L2_PIX_FMT_SBGGR12) ||
	     (output == V4L2_PIX_FMT_SGBRG12)))
		return 0;

	if ((input == ATOMISP_INPUT_FORMAT_RAW_10) &&
	    ((output == V4L2_PIX_FMT_SRGGB10) ||
	     (output == V4L2_PIX_FMT_SGRBG10) ||
	     (output == V4L2_PIX_FMT_SBGGR10) ||
	     (output == V4L2_PIX_FMT_SGBRG10)))
		return 0;

	if ((input == ATOMISP_INPUT_FORMAT_RAW_8) &&
	    ((output == V4L2_PIX_FMT_SRGGB8) ||
	     (output == V4L2_PIX_FMT_SGRBG8) ||
	     (output == V4L2_PIX_FMT_SBGGR8) ||
	     (output == V4L2_PIX_FMT_SGBRG8)))
		return 0;

	if ((input == ATOMISP_INPUT_FORMAT_RAW_16) && (output == V4L2_PIX_FMT_SBGGR16))
		return 0;

	return -EINVAL;
}

u32 atomisp_get_pixel_depth(u32 pixelformat)
{
	switch (pixelformat) {
	case V4L2_PIX_FMT_YUV420:
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_YVU420:
		return 12;
	case V4L2_PIX_FMT_YUV422P:
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV61:
	case V4L2_PIX_FMT_RGB565:
	case V4L2_PIX_FMT_SBGGR16:
	case V4L2_PIX_FMT_SBGGR12:
	case V4L2_PIX_FMT_SGBRG12:
	case V4L2_PIX_FMT_SGRBG12:
	case V4L2_PIX_FMT_SRGGB12:
	case V4L2_PIX_FMT_SBGGR10:
	case V4L2_PIX_FMT_SGBRG10:
	case V4L2_PIX_FMT_SGRBG10:
	case V4L2_PIX_FMT_SRGGB10:
		return 16;
	case V4L2_PIX_FMT_RGB24:
	case V4L2_PIX_FMT_YUV444:
		return 24;
	case V4L2_PIX_FMT_RGB32:
		return 32;
	case V4L2_PIX_FMT_JPEG:
	case V4L2_PIX_FMT_CUSTOM_M10MO_RAW:
	case V4L2_PIX_FMT_SBGGR8:
	case V4L2_PIX_FMT_SGBRG8:
	case V4L2_PIX_FMT_SGRBG8:
	case V4L2_PIX_FMT_SRGGB8:
		return 8;
	default:
		return 8 * 2;	/* raw type now */
	}
}

bool atomisp_is_mbuscode_raw(uint32_t code)
{
	return code >= 0x3000 && code < 0x4000;
}

/*
 * ISP features control function
 */

/*
 * Set ISP capture mode based on current settings
 */
static void atomisp_update_capture_mode(struct atomisp_sub_device *asd)
{
	if (asd->params.gdc_cac_en)
		atomisp_css_capture_set_mode(asd, IA_CSS_CAPTURE_MODE_ADVANCED);
	else if (asd->params.low_light)
		atomisp_css_capture_set_mode(asd, IA_CSS_CAPTURE_MODE_LOW_LIGHT);
	else if (asd->video_out.sh_fmt == IA_CSS_FRAME_FORMAT_RAW)
		atomisp_css_capture_set_mode(asd, IA_CSS_CAPTURE_MODE_RAW);
	else
		atomisp_css_capture_set_mode(asd, IA_CSS_CAPTURE_MODE_PRIMARY);
}

/* ISP2401 */
int atomisp_set_sensor_runmode(struct atomisp_sub_device *asd,
			       struct atomisp_s_runmode *runmode)
{
	struct atomisp_device *isp = asd->isp;
	struct v4l2_ctrl *c;
	int ret = 0;

	if (!(runmode && (runmode->mode & RUNMODE_MASK)))
		return -EINVAL;

	mutex_lock(asd->ctrl_handler.lock);
	c = v4l2_ctrl_find(isp->inputs[asd->input_curr].camera->ctrl_handler,
			   V4L2_CID_RUN_MODE);

	if (c)
		ret = v4l2_ctrl_s_ctrl(c, runmode->mode);

	mutex_unlock(asd->ctrl_handler.lock);
	return ret;
}

/*
 * Function to enable/disable lens geometry distortion correction (GDC) and
 * chromatic aberration correction (CAC)
 */
int atomisp_gdc_cac(struct atomisp_sub_device *asd, int flag,
		    __s32 *value)
{
	if (flag == 0) {
		*value = asd->params.gdc_cac_en;
		return 0;
	}

	asd->params.gdc_cac_en = !!*value;
	if (asd->params.gdc_cac_en) {
		asd->params.config.morph_table = asd->params.css_param.morph_table;
	} else {
		asd->params.config.morph_table = NULL;
	}
	asd->params.css_update_params_needed = true;
	atomisp_update_capture_mode(asd);
	return 0;
}

/*
 * Function to enable/disable low light mode including ANR
 */
int atomisp_low_light(struct atomisp_sub_device *asd, int flag,
		      __s32 *value)
{
	if (flag == 0) {
		*value = asd->params.low_light;
		return 0;
	}

	asd->params.low_light = (*value != 0);
	atomisp_update_capture_mode(asd);
	return 0;
}

/*
 * Function to enable/disable extra noise reduction (XNR) in low light
 * condition
 */
int atomisp_xnr(struct atomisp_sub_device *asd, int flag,
		int *xnr_enable)
{
	if (flag == 0) {
		*xnr_enable = asd->params.xnr_en;
		return 0;
	}

	atomisp_css_capture_enable_xnr(asd, !!*xnr_enable);

	return 0;
}

/*
 * Function to configure bayer noise reduction
 */
int atomisp_nr(struct atomisp_sub_device *asd, int flag,
	       struct atomisp_nr_config *arg)
{
	if (flag == 0) {
		/* Get nr config from current setup */
		if (atomisp_css_get_nr_config(asd, arg))
			return -EINVAL;
	} else {
		/* Set nr config to isp parameters */
		memcpy(&asd->params.css_param.nr_config, arg,
		       sizeof(struct ia_css_nr_config));
		asd->params.config.nr_config = &asd->params.css_param.nr_config;
		asd->params.css_update_params_needed = true;
	}
	return 0;
}

/*
 * Function to configure temporal noise reduction (TNR)
 */
int atomisp_tnr(struct atomisp_sub_device *asd, int flag,
		struct atomisp_tnr_config *config)
{
	/* Get tnr config from current setup */
	if (flag == 0) {
		/* Get tnr config from current setup */
		if (atomisp_css_get_tnr_config(asd, config))
			return -EINVAL;
	} else {
		/* Set tnr config to isp parameters */
		memcpy(&asd->params.css_param.tnr_config, config,
		       sizeof(struct ia_css_tnr_config));
		asd->params.config.tnr_config = &asd->params.css_param.tnr_config;
		asd->params.css_update_params_needed = true;
	}

	return 0;
}

/*
 * Function to configure black level compensation
 */
int atomisp_black_level(struct atomisp_sub_device *asd, int flag,
			struct atomisp_ob_config *config)
{
	if (flag == 0) {
		/* Get ob config from current setup */
		if (atomisp_css_get_ob_config(asd, config))
			return -EINVAL;
	} else {
		/* Set ob config to isp parameters */
		memcpy(&asd->params.css_param.ob_config, config,
		       sizeof(struct ia_css_ob_config));
		asd->params.config.ob_config = &asd->params.css_param.ob_config;
		asd->params.css_update_params_needed = true;
	}

	return 0;
}

/*
 * Function to configure edge enhancement
 */
int atomisp_ee(struct atomisp_sub_device *asd, int flag,
	       struct atomisp_ee_config *config)
{
	if (flag == 0) {
		/* Get ee config from current setup */
		if (atomisp_css_get_ee_config(asd, config))
			return -EINVAL;
	} else {
		/* Set ee config to isp parameters */
		memcpy(&asd->params.css_param.ee_config, config,
		       sizeof(asd->params.css_param.ee_config));
		asd->params.config.ee_config = &asd->params.css_param.ee_config;
		asd->params.css_update_params_needed = true;
	}

	return 0;
}

/*
 * Function to update Gamma table for gamma, brightness and contrast config
 */
int atomisp_gamma(struct atomisp_sub_device *asd, int flag,
		  struct atomisp_gamma_table *config)
{
	if (flag == 0) {
		/* Get gamma table from current setup */
		if (atomisp_css_get_gamma_table(asd, config))
			return -EINVAL;
	} else {
		/* Set gamma table to isp parameters */
		memcpy(&asd->params.css_param.gamma_table, config,
		       sizeof(asd->params.css_param.gamma_table));
		asd->params.config.gamma_table = &asd->params.css_param.gamma_table;
	}

	return 0;
}

/*
 * Function to update Ctc table for Chroma Enhancement
 */
int atomisp_ctc(struct atomisp_sub_device *asd, int flag,
		struct atomisp_ctc_table *config)
{
	if (flag == 0) {
		/* Get ctc table from current setup */
		if (atomisp_css_get_ctc_table(asd, config))
			return -EINVAL;
	} else {
		/* Set ctc table to isp parameters */
		memcpy(&asd->params.css_param.ctc_table, config,
		       sizeof(asd->params.css_param.ctc_table));
		atomisp_css_set_ctc_table(asd, &asd->params.css_param.ctc_table);
	}

	return 0;
}

/*
 * Function to update gamma correction parameters
 */
int atomisp_gamma_correction(struct atomisp_sub_device *asd, int flag,
			     struct atomisp_gc_config *config)
{
	if (flag == 0) {
		/* Get gamma correction params from current setup */
		if (atomisp_css_get_gc_config(asd, config))
			return -EINVAL;
	} else {
		/* Set gamma correction params to isp parameters */
		memcpy(&asd->params.css_param.gc_config, config,
		       sizeof(asd->params.css_param.gc_config));
		asd->params.config.gc_config = &asd->params.css_param.gc_config;
		asd->params.css_update_params_needed = true;
	}

	return 0;
}

/*
 * Function to update narrow gamma flag
 */
int atomisp_formats(struct atomisp_sub_device *asd, int flag,
		    struct atomisp_formats_config *config)
{
	if (flag == 0) {
		/* Get narrow gamma flag from current setup */
		if (atomisp_css_get_formats_config(asd, config))
			return -EINVAL;
	} else {
		/* Set narrow gamma flag to isp parameters */
		memcpy(&asd->params.css_param.formats_config, config,
		       sizeof(asd->params.css_param.formats_config));
		asd->params.config.formats_config = &asd->params.css_param.formats_config;
	}

	return 0;
}

void atomisp_free_internal_buffers(struct atomisp_sub_device *asd)
{
	atomisp_free_css_parameters(&asd->params.css_param);
}

static void atomisp_update_grid_info(struct atomisp_sub_device *asd,
				     enum ia_css_pipe_id pipe_id)
{
	struct atomisp_device *isp = asd->isp;
	int err;

	if (atomisp_css_get_grid_info(asd, pipe_id))
		return;

	/* We must free all buffers because they no longer match
	   the grid size. */
	atomisp_css_free_stat_buffers(asd);

	err = atomisp_alloc_css_stat_bufs(asd, ATOMISP_INPUT_STREAM_GENERAL);
	if (err) {
		dev_err(isp->dev, "stat_buf allocate error\n");
		goto err;
	}

	if (atomisp_alloc_3a_output_buf(asd)) {
		/* Failure for 3A buffers does not influence DIS buffers */
		if (asd->params.s3a_output_bytes != 0) {
			/* For SOC sensor happens s3a_output_bytes == 0,
			 * using if condition to exclude false error log */
			dev_err(isp->dev, "Failed to allocate memory for 3A statistics\n");
		}
		goto err;
	}

	if (atomisp_alloc_dis_coef_buf(asd)) {
		dev_err(isp->dev,
			"Failed to allocate memory for DIS statistics\n");
		goto err;
	}

	if (atomisp_alloc_metadata_output_buf(asd)) {
		dev_err(isp->dev, "Failed to allocate memory for metadata\n");
		goto err;
	}

	return;

err:
	atomisp_css_free_stat_buffers(asd);
	return;
}

static void atomisp_curr_user_grid_info(struct atomisp_sub_device *asd,
					struct atomisp_grid_info *info)
{
	memcpy(info, &asd->params.curr_grid_info.s3a_grid,
	       sizeof(struct ia_css_3a_grid_info));
}

int atomisp_compare_grid(struct atomisp_sub_device *asd,
			 struct atomisp_grid_info *atomgrid)
{
	struct atomisp_grid_info tmp = {0};

	atomisp_curr_user_grid_info(asd, &tmp);
	return memcmp(atomgrid, &tmp, sizeof(tmp));
}

/*
 * Function to update Gdc table for gdc
 */
int atomisp_gdc_cac_table(struct atomisp_sub_device *asd, int flag,
			  struct atomisp_morph_table *config)
{
	int ret;
	int i;
	struct atomisp_device *isp = asd->isp;

	if (flag == 0) {
		/* Get gdc table from current setup */
		struct ia_css_morph_table tab = {0};

		atomisp_css_get_morph_table(asd, &tab);

		config->width = tab.width;
		config->height = tab.height;

		for (i = 0; i < IA_CSS_MORPH_TABLE_NUM_PLANES; i++) {
			ret = copy_to_user(config->coordinates_x[i],
					   tab.coordinates_x[i], tab.height *
					   tab.width * sizeof(*tab.coordinates_x[i]));
			if (ret) {
				dev_err(isp->dev,
					"Failed to copy to User for x\n");
				return -EFAULT;
			}
			ret = copy_to_user(config->coordinates_y[i],
					   tab.coordinates_y[i], tab.height *
					   tab.width * sizeof(*tab.coordinates_y[i]));
			if (ret) {
				dev_err(isp->dev,
					"Failed to copy to User for y\n");
				return -EFAULT;
			}
		}
	} else {
		struct ia_css_morph_table *tab =
			    asd->params.css_param.morph_table;

		/* free first if we have one */
		if (tab) {
			atomisp_css_morph_table_free(tab);
			asd->params.css_param.morph_table = NULL;
		}

		/* allocate new one */
		tab = atomisp_css_morph_table_allocate(config->width,
						       config->height);

		if (!tab) {
			dev_err(isp->dev, "out of memory\n");
			return -EINVAL;
		}

		for (i = 0; i < IA_CSS_MORPH_TABLE_NUM_PLANES; i++) {
			ret = copy_from_user(tab->coordinates_x[i],
					     config->coordinates_x[i],
					     config->height * config->width *
					     sizeof(*config->coordinates_x[i]));
			if (ret) {
				dev_err(isp->dev,
					"Failed to copy from User for x, ret %d\n",
					ret);
				atomisp_css_morph_table_free(tab);
				return -EFAULT;
			}
			ret = copy_from_user(tab->coordinates_y[i],
					     config->coordinates_y[i],
					     config->height * config->width *
					     sizeof(*config->coordinates_y[i]));
			if (ret) {
				dev_err(isp->dev,
					"Failed to copy from User for y, ret is %d\n",
					ret);
				atomisp_css_morph_table_free(tab);
				return -EFAULT;
			}
		}
		asd->params.css_param.morph_table = tab;
		if (asd->params.gdc_cac_en)
			asd->params.config.morph_table = tab;
	}

	return 0;
}

int atomisp_macc_table(struct atomisp_sub_device *asd, int flag,
		       struct atomisp_macc_config *config)
{
	struct ia_css_macc_table *macc_table;

	switch (config->color_effect) {
	case V4L2_COLORFX_NONE:
		macc_table = &asd->params.css_param.macc_table;
		break;
	case V4L2_COLORFX_SKY_BLUE:
		macc_table = &blue_macc_table;
		break;
	case V4L2_COLORFX_GRASS_GREEN:
		macc_table = &green_macc_table;
		break;
	case V4L2_COLORFX_SKIN_WHITEN_LOW:
		macc_table = &skin_low_macc_table;
		break;
	case V4L2_COLORFX_SKIN_WHITEN:
		macc_table = &skin_medium_macc_table;
		break;
	case V4L2_COLORFX_SKIN_WHITEN_HIGH:
		macc_table = &skin_high_macc_table;
		break;
	default:
		return -EINVAL;
	}

	if (flag == 0) {
		/* Get macc table from current setup */
		memcpy(&config->table, macc_table,
		       sizeof(struct ia_css_macc_table));
	} else {
		memcpy(macc_table, &config->table,
		       sizeof(struct ia_css_macc_table));
		if (config->color_effect == asd->params.color_effect)
			asd->params.config.macc_table = macc_table;
	}

	return 0;
}

int atomisp_set_dis_vector(struct atomisp_sub_device *asd,
			   struct atomisp_dis_vector *vector)
{
	atomisp_css_video_set_dis_vector(asd, vector);

	asd->params.dis_proj_data_valid = false;
	asd->params.css_update_params_needed = true;
	return 0;
}

/*
 * Function to set/get image stablization statistics
 */
int atomisp_get_dis_stat(struct atomisp_sub_device *asd,
			 struct atomisp_dis_statistics *stats)
{
	return atomisp_css_get_dis_stat(asd, stats);
}

/*
 * Function  set camrea_prefiles.xml current sensor pixel array size
 */
int atomisp_set_array_res(struct atomisp_sub_device *asd,
			  struct atomisp_resolution  *config)
{
	dev_dbg(asd->isp->dev, ">%s start\n", __func__);
	if (!config) {
		dev_err(asd->isp->dev, "Set sensor array size is not valid\n");
		return -EINVAL;
	}

	asd->sensor_array_res.width = config->width;
	asd->sensor_array_res.height = config->height;
	return 0;
}

/*
 * Function to get DVS2 BQ resolution settings
 */
int atomisp_get_dvs2_bq_resolutions(struct atomisp_sub_device *asd,
				    struct atomisp_dvs2_bq_resolutions *bq_res)
{
	struct ia_css_pipe_config *pipe_cfg = NULL;

	struct ia_css_stream *stream =
		    asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream;
	if (!stream) {
		dev_warn(asd->isp->dev, "stream is not created");
		return -EAGAIN;
	}

	pipe_cfg = &asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL]
		   .pipe_configs[IA_CSS_PIPE_ID_VIDEO];

	if (!bq_res)
		return -EINVAL;

	/* the GDC output resolution */
	bq_res->output_bq.width_bq = pipe_cfg->output_info[0].res.width / 2;
	bq_res->output_bq.height_bq = pipe_cfg->output_info[0].res.height / 2;

	bq_res->envelope_bq.width_bq = 0;
	bq_res->envelope_bq.height_bq = 0;
	/* the GDC input resolution */
	bq_res->source_bq.width_bq = bq_res->output_bq.width_bq +
				     pipe_cfg->dvs_envelope.width / 2;
	bq_res->source_bq.height_bq = bq_res->output_bq.height_bq +
				      pipe_cfg->dvs_envelope.height / 2;
	/*
	 * Bad pixels caused by spatial filter processing
	 * ISP filter resolution should be given by CSS/FW, but for now
	 * there is not such API to query, and it is fixed value, so
	 * hardcoded here.
	 */
	bq_res->ispfilter_bq.width_bq = 12 / 2;
	bq_res->ispfilter_bq.height_bq = 12 / 2;
	/* spatial filter shift, always 4 pixels */
	bq_res->gdc_shift_bq.width_bq = 4 / 2;
	bq_res->gdc_shift_bq.height_bq = 4 / 2;

	if (asd->params.video_dis_en) {
		bq_res->envelope_bq.width_bq = pipe_cfg->dvs_envelope.width / 2 -
					       bq_res->ispfilter_bq.width_bq;
		bq_res->envelope_bq.height_bq = pipe_cfg->dvs_envelope.height / 2 -
						bq_res->ispfilter_bq.height_bq;
	}

	dev_dbg(asd->isp->dev,
		"source_bq.width_bq %d, source_bq.height_bq %d,\nispfilter_bq.width_bq %d, ispfilter_bq.height_bq %d,\ngdc_shift_bq.width_bq %d, gdc_shift_bq.height_bq %d,\nenvelope_bq.width_bq %d, envelope_bq.height_bq %d,\noutput_bq.width_bq %d, output_bq.height_bq %d\n",
		bq_res->source_bq.width_bq, bq_res->source_bq.height_bq,
		bq_res->ispfilter_bq.width_bq, bq_res->ispfilter_bq.height_bq,
		bq_res->gdc_shift_bq.width_bq, bq_res->gdc_shift_bq.height_bq,
		bq_res->envelope_bq.width_bq, bq_res->envelope_bq.height_bq,
		bq_res->output_bq.width_bq, bq_res->output_bq.height_bq);

	return 0;
}

int atomisp_set_dis_coefs(struct atomisp_sub_device *asd,
			  struct atomisp_dis_coefficients *coefs)
{
	return atomisp_css_set_dis_coefs(asd, coefs);
}

/*
 * Function to set/get 3A stat from isp
 */
int atomisp_3a_stat(struct atomisp_sub_device *asd, int flag,
		    struct atomisp_3a_statistics *config)
{
	struct atomisp_device *isp = asd->isp;
	struct atomisp_s3a_buf *s3a_buf;
	unsigned long ret;

	if (flag != 0)
		return -EINVAL;

	/* sanity check to avoid writing into unallocated memory. */
	if (asd->params.s3a_output_bytes == 0)
		return -EINVAL;

	if (atomisp_compare_grid(asd, &config->grid_info) != 0) {
		/* If the grid info in the argument differs from the current
		   grid info, we tell the caller to reset the grid size and
		   try again. */
		return -EAGAIN;
	}

	if (list_empty(&asd->s3a_stats_ready)) {
		dev_err(isp->dev, "3a statistics is not valid.\n");
		return -EAGAIN;
	}

	s3a_buf = list_entry(asd->s3a_stats_ready.next,
			     struct atomisp_s3a_buf, list);
	if (s3a_buf->s3a_map)
		ia_css_translate_3a_statistics(
		    asd->params.s3a_user_stat, s3a_buf->s3a_map);
	else
		ia_css_get_3a_statistics(asd->params.s3a_user_stat,
					 s3a_buf->s3a_data);

	config->exp_id = s3a_buf->s3a_data->exp_id;
	config->isp_config_id = s3a_buf->s3a_data->isp_config_id;

	ret = copy_to_user(config->data, asd->params.s3a_user_stat->data,
			   asd->params.s3a_output_bytes);
	if (ret) {
		dev_err(isp->dev, "copy to user failed: copied %lu bytes\n",
			ret);
		return -EFAULT;
	}

	/* Move to free buffer list */
	list_del_init(&s3a_buf->list);
	list_add_tail(&s3a_buf->list, &asd->s3a_stats);
	dev_dbg(isp->dev, "%s: finish getting exp_id %d 3a stat, isp_config_id %d\n",
		__func__,
		config->exp_id, config->isp_config_id);
	return 0;
}

/*
 * Function to calculate real zoom region for every pipe
 */
int atomisp_calculate_real_zoom_region(struct atomisp_sub_device *asd,
				       struct ia_css_dz_config   *dz_config,
				       enum ia_css_pipe_id css_pipe_id)

{
	struct atomisp_stream_env *stream_env =
		    &asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL];
	struct atomisp_resolution  eff_res, out_res;
	int w_offset, h_offset;

	memset(&eff_res, 0, sizeof(eff_res));
	memset(&out_res, 0, sizeof(out_res));

	if (dz_config->dx || dz_config->dy)
		return 0;

	if (css_pipe_id != IA_CSS_PIPE_ID_PREVIEW
	    && css_pipe_id != IA_CSS_PIPE_ID_CAPTURE) {
		dev_err(asd->isp->dev, "%s the set pipe no support crop region"
			, __func__);
		return -EINVAL;
	}

	eff_res.width =
	    stream_env->stream_config.input_config.effective_res.width;
	eff_res.height =
	    stream_env->stream_config.input_config.effective_res.height;
	if (eff_res.width == 0 || eff_res.height == 0) {
		dev_err(asd->isp->dev, "%s err effective resolution"
			, __func__);
		return -EINVAL;
	}

	if (dz_config->zoom_region.resolution.width
	    == asd->sensor_array_res.width
	    || dz_config->zoom_region.resolution.height
	    == asd->sensor_array_res.height) {
		/*no need crop region*/
		dz_config->zoom_region.origin.x = 0;
		dz_config->zoom_region.origin.y = 0;
		dz_config->zoom_region.resolution.width = eff_res.width;
		dz_config->zoom_region.resolution.height = eff_res.height;
		return 0;
	}

	/* FIXME:
	 * This is not the correct implementation with Google's definition, due
	 * to firmware limitation.
	 * map real crop region base on above calculating base max crop region.
	 */

	if (!IS_ISP2401) {
		dz_config->zoom_region.origin.x = dz_config->zoom_region.origin.x
						  * eff_res.width
						  / asd->sensor_array_res.width;
		dz_config->zoom_region.origin.y = dz_config->zoom_region.origin.y
						  * eff_res.height
						  / asd->sensor_array_res.height;
		dz_config->zoom_region.resolution.width = dz_config->zoom_region.resolution.width
							  * eff_res.width
							  / asd->sensor_array_res.width;
		dz_config->zoom_region.resolution.height = dz_config->zoom_region.resolution.height
							  * eff_res.height
							  / asd->sensor_array_res.height;
		/*
		 * Set same ratio of crop region resolution and current pipe output
		 * resolution
		 */
		out_res.width = stream_env->pipe_configs[css_pipe_id].output_info[0].res.width;
		out_res.height = stream_env->pipe_configs[css_pipe_id].output_info[0].res.height;
		if (out_res.width == 0 || out_res.height == 0) {
			dev_err(asd->isp->dev, "%s err current pipe output resolution"
				, __func__);
			return -EINVAL;
		}
	} else {
		out_res.width = stream_env->pipe_configs[css_pipe_id].output_info[0].res.width;
		out_res.height = stream_env->pipe_configs[css_pipe_id].output_info[0].res.height;
		if (out_res.width == 0 || out_res.height == 0) {
			dev_err(asd->isp->dev, "%s err current pipe output resolution"
				, __func__);
			return -EINVAL;
		}

		if (asd->sensor_array_res.width * out_res.height
		    < out_res.width * asd->sensor_array_res.height) {
			h_offset = asd->sensor_array_res.height
				   - asd->sensor_array_res.width
				   * out_res.height / out_res.width;
			h_offset = h_offset / 2;
			if (dz_config->zoom_region.origin.y < h_offset)
				dz_config->zoom_region.origin.y = 0;
			else
				dz_config->zoom_region.origin.y = dz_config->zoom_region.origin.y - h_offset;
			w_offset = 0;
		} else {
			w_offset = asd->sensor_array_res.width
				   - asd->sensor_array_res.height
				   * out_res.width / out_res.height;
			w_offset = w_offset / 2;
			if (dz_config->zoom_region.origin.x < w_offset)
				dz_config->zoom_region.origin.x = 0;
			else
				dz_config->zoom_region.origin.x = dz_config->zoom_region.origin.x - w_offset;
			h_offset = 0;
		}
		dz_config->zoom_region.origin.x = dz_config->zoom_region.origin.x
						  * eff_res.width
						  / (asd->sensor_array_res.width - 2 * w_offset);
		dz_config->zoom_region.origin.y = dz_config->zoom_region.origin.y
						  * eff_res.height
						  / (asd->sensor_array_res.height - 2 * h_offset);
		dz_config->zoom_region.resolution.width = dz_config->zoom_region.resolution.width
						  * eff_res.width
						  / (asd->sensor_array_res.width - 2 * w_offset);
		dz_config->zoom_region.resolution.height = dz_config->zoom_region.resolution.height
						  * eff_res.height
						  / (asd->sensor_array_res.height - 2 * h_offset);
	}

	if (out_res.width * dz_config->zoom_region.resolution.height
	    > dz_config->zoom_region.resolution.width * out_res.height) {
		dz_config->zoom_region.resolution.height =
		    dz_config->zoom_region.resolution.width
		    * out_res.height / out_res.width;
	} else {
		dz_config->zoom_region.resolution.width =
		    dz_config->zoom_region.resolution.height
		    * out_res.width / out_res.height;
	}
	dev_dbg(asd->isp->dev,
		"%s crop region:(%d,%d),(%d,%d) eff_res(%d, %d) array_size(%d,%d) out_res(%d, %d)\n",
		__func__, dz_config->zoom_region.origin.x,
		dz_config->zoom_region.origin.y,
		dz_config->zoom_region.resolution.width,
		dz_config->zoom_region.resolution.height,
		eff_res.width, eff_res.height,
		asd->sensor_array_res.width,
		asd->sensor_array_res.height,
		out_res.width, out_res.height);

	if ((dz_config->zoom_region.origin.x +
	     dz_config->zoom_region.resolution.width
	     > eff_res.width) ||
	    (dz_config->zoom_region.origin.y +
	     dz_config->zoom_region.resolution.height
	     > eff_res.height))
		return -EINVAL;

	return 0;
}

/*
 * Function to check the zoom region whether is effective
 */
static bool atomisp_check_zoom_region(
    struct atomisp_sub_device *asd,
    struct ia_css_dz_config *dz_config)
{
	struct atomisp_resolution  config;
	bool flag = false;
	unsigned int w, h;

	memset(&config, 0, sizeof(struct atomisp_resolution));

	if (dz_config->dx && dz_config->dy)
		return true;

	config.width = asd->sensor_array_res.width;
	config.height = asd->sensor_array_res.height;
	w = dz_config->zoom_region.origin.x +
	    dz_config->zoom_region.resolution.width;
	h = dz_config->zoom_region.origin.y +
	    dz_config->zoom_region.resolution.height;

	if ((w <= config.width) && (h <= config.height) && w > 0 && h > 0)
		flag = true;
	else
		/* setting error zoom region */
		dev_err(asd->isp->dev,
			"%s zoom region ERROR:dz_config:(%d,%d),(%d,%d)array_res(%d, %d)\n",
			__func__, dz_config->zoom_region.origin.x,
			dz_config->zoom_region.origin.y,
			dz_config->zoom_region.resolution.width,
			dz_config->zoom_region.resolution.height,
			config.width, config.height);

	return flag;
}

void atomisp_apply_css_parameters(
    struct atomisp_sub_device *asd,
    struct atomisp_css_params *css_param)
{
	if (css_param->update_flag.wb_config)
		asd->params.config.wb_config = &css_param->wb_config;

	if (css_param->update_flag.ob_config)
		asd->params.config.ob_config = &css_param->ob_config;

	if (css_param->update_flag.dp_config)
		asd->params.config.dp_config = &css_param->dp_config;

	if (css_param->update_flag.nr_config)
		asd->params.config.nr_config = &css_param->nr_config;

	if (css_param->update_flag.ee_config)
		asd->params.config.ee_config = &css_param->ee_config;

	if (css_param->update_flag.tnr_config)
		asd->params.config.tnr_config = &css_param->tnr_config;

	if (css_param->update_flag.a3a_config)
		asd->params.config.s3a_config = &css_param->s3a_config;

	if (css_param->update_flag.ctc_config)
		asd->params.config.ctc_config = &css_param->ctc_config;

	if (css_param->update_flag.cnr_config)
		asd->params.config.cnr_config = &css_param->cnr_config;

	if (css_param->update_flag.ecd_config)
		asd->params.config.ecd_config = &css_param->ecd_config;

	if (css_param->update_flag.ynr_config)
		asd->params.config.ynr_config = &css_param->ynr_config;

	if (css_param->update_flag.fc_config)
		asd->params.config.fc_config = &css_param->fc_config;

	if (css_param->update_flag.macc_config)
		asd->params.config.macc_config = &css_param->macc_config;

	if (css_param->update_flag.aa_config)
		asd->params.config.aa_config = &css_param->aa_config;

	if (css_param->update_flag.anr_config)
		asd->params.config.anr_config = &css_param->anr_config;

	if (css_param->update_flag.xnr_config)
		asd->params.config.xnr_config = &css_param->xnr_config;

	if (css_param->update_flag.yuv2rgb_cc_config)
		asd->params.config.yuv2rgb_cc_config = &css_param->yuv2rgb_cc_config;

	if (css_param->update_flag.rgb2yuv_cc_config)
		asd->params.config.rgb2yuv_cc_config = &css_param->rgb2yuv_cc_config;

	if (css_param->update_flag.macc_table)
		asd->params.config.macc_table = &css_param->macc_table;

	if (css_param->update_flag.xnr_table)
		asd->params.config.xnr_table = &css_param->xnr_table;

	if (css_param->update_flag.r_gamma_table)
		asd->params.config.r_gamma_table = &css_param->r_gamma_table;

	if (css_param->update_flag.g_gamma_table)
		asd->params.config.g_gamma_table = &css_param->g_gamma_table;

	if (css_param->update_flag.b_gamma_table)
		asd->params.config.b_gamma_table = &css_param->b_gamma_table;

	if (css_param->update_flag.anr_thres)
		atomisp_css_set_anr_thres(asd, &css_param->anr_thres);

	if (css_param->update_flag.shading_table)
		asd->params.config.shading_table = css_param->shading_table;

	if (css_param->update_flag.morph_table && asd->params.gdc_cac_en)
		asd->params.config.morph_table = css_param->morph_table;

	if (css_param->update_flag.dvs2_coefs) {
		struct ia_css_dvs_grid_info *dvs_grid_info =
		    atomisp_css_get_dvs_grid_info(
			&asd->params.curr_grid_info);

		if (dvs_grid_info && dvs_grid_info->enable)
			atomisp_css_set_dvs2_coefs(asd, css_param->dvs2_coeff);
	}

	if (css_param->update_flag.dvs_6axis_config)
		atomisp_css_set_dvs_6axis(asd, css_param->dvs_6axis);

	atomisp_css_set_isp_config_id(asd, css_param->isp_config_id);
	/*
	 * These configurations are on used by ISP1.x, not for ISP2.x,
	 * so do not handle them. see comments of ia_css_isp_config.
	 * 1 cc_config
	 * 2 ce_config
	 * 3 de_config
	 * 4 gc_config
	 * 5 gamma_table
	 * 6 ctc_table
	 * 7 dvs_coefs
	 */
}

static unsigned int long copy_from_compatible(void *to, const void *from,
	unsigned long n, bool from_user)
{
	if (from_user)
		return copy_from_user(to, (void __user *)from, n);
	else
		memcpy(to, from, n);
	return 0;
}

int atomisp_cp_general_isp_parameters(struct atomisp_sub_device *asd,
				      struct atomisp_parameters *arg,
				      struct atomisp_css_params *css_param,
				      bool from_user)
{
	struct atomisp_parameters *cur_config = &css_param->update_flag;

	if (!arg || !asd || !css_param)
		return -EINVAL;

	if (arg->wb_config && (from_user || !cur_config->wb_config)) {
		if (copy_from_compatible(&css_param->wb_config, arg->wb_config,
					 sizeof(struct ia_css_wb_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.wb_config =
		    (struct atomisp_wb_config *)&css_param->wb_config;
	}

	if (arg->ob_config && (from_user || !cur_config->ob_config)) {
		if (copy_from_compatible(&css_param->ob_config, arg->ob_config,
					 sizeof(struct ia_css_ob_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.ob_config =
		    (struct atomisp_ob_config *)&css_param->ob_config;
	}

	if (arg->dp_config && (from_user || !cur_config->dp_config)) {
		if (copy_from_compatible(&css_param->dp_config, arg->dp_config,
					 sizeof(struct ia_css_dp_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.dp_config =
		    (struct atomisp_dp_config *)&css_param->dp_config;
	}

	if (asd->run_mode->val != ATOMISP_RUN_MODE_VIDEO) {
		if (arg->dz_config && (from_user || !cur_config->dz_config)) {
			if (copy_from_compatible(&css_param->dz_config,
						 arg->dz_config,
						 sizeof(struct ia_css_dz_config),
						 from_user))
				return -EFAULT;
			if (!atomisp_check_zoom_region(asd,
						       &css_param->dz_config)) {
				dev_err(asd->isp->dev, "crop region error!");
				return -EINVAL;
			}
			css_param->update_flag.dz_config =
			    (struct atomisp_dz_config *)
			    &css_param->dz_config;
		}
	}

	if (arg->nr_config && (from_user || !cur_config->nr_config)) {
		if (copy_from_compatible(&css_param->nr_config, arg->nr_config,
					 sizeof(struct ia_css_nr_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.nr_config =
		    (struct atomisp_nr_config *)&css_param->nr_config;
	}

	if (arg->ee_config && (from_user || !cur_config->ee_config)) {
		if (copy_from_compatible(&css_param->ee_config, arg->ee_config,
					 sizeof(struct ia_css_ee_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.ee_config =
		    (struct atomisp_ee_config *)&css_param->ee_config;
	}

	if (arg->tnr_config && (from_user || !cur_config->tnr_config)) {
		if (copy_from_compatible(&css_param->tnr_config,
					 arg->tnr_config,
					 sizeof(struct ia_css_tnr_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.tnr_config =
		    (struct atomisp_tnr_config *)
		    &css_param->tnr_config;
	}

	if (arg->a3a_config && (from_user || !cur_config->a3a_config)) {
		if (copy_from_compatible(&css_param->s3a_config,
					 arg->a3a_config,
					 sizeof(struct ia_css_3a_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.a3a_config =
		    (struct atomisp_3a_config *)&css_param->s3a_config;
	}

	if (arg->ctc_config && (from_user || !cur_config->ctc_config)) {
		if (copy_from_compatible(&css_param->ctc_config,
					 arg->ctc_config,
					 sizeof(struct ia_css_ctc_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.ctc_config =
		    (struct atomisp_ctc_config *)
		    &css_param->ctc_config;
	}

	if (arg->cnr_config && (from_user || !cur_config->cnr_config)) {
		if (copy_from_compatible(&css_param->cnr_config,
					 arg->cnr_config,
					 sizeof(struct ia_css_cnr_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.cnr_config =
		    (struct atomisp_cnr_config *)
		    &css_param->cnr_config;
	}

	if (arg->ecd_config && (from_user || !cur_config->ecd_config)) {
		if (copy_from_compatible(&css_param->ecd_config,
					 arg->ecd_config,
					 sizeof(struct ia_css_ecd_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.ecd_config =
		    (struct atomisp_ecd_config *)
		    &css_param->ecd_config;
	}

	if (arg->ynr_config && (from_user || !cur_config->ynr_config)) {
		if (copy_from_compatible(&css_param->ynr_config,
					 arg->ynr_config,
					 sizeof(struct ia_css_ynr_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.ynr_config =
		    (struct atomisp_ynr_config *)
		    &css_param->ynr_config;
	}

	if (arg->fc_config && (from_user || !cur_config->fc_config)) {
		if (copy_from_compatible(&css_param->fc_config,
					 arg->fc_config,
					 sizeof(struct ia_css_fc_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.fc_config =
		    (struct atomisp_fc_config *)&css_param->fc_config;
	}

	if (arg->macc_config && (from_user || !cur_config->macc_config)) {
		if (copy_from_compatible(&css_param->macc_config,
					 arg->macc_config,
					 sizeof(struct ia_css_macc_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.macc_config =
		    (struct atomisp_macc_config *)
		    &css_param->macc_config;
	}

	if (arg->aa_config && (from_user || !cur_config->aa_config)) {
		if (copy_from_compatible(&css_param->aa_config, arg->aa_config,
					 sizeof(struct ia_css_aa_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.aa_config =
		    (struct atomisp_aa_config *)&css_param->aa_config;
	}

	if (arg->anr_config && (from_user || !cur_config->anr_config)) {
		if (copy_from_compatible(&css_param->anr_config,
					 arg->anr_config,
					 sizeof(struct ia_css_anr_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.anr_config =
		    (struct atomisp_anr_config *)
		    &css_param->anr_config;
	}

	if (arg->xnr_config && (from_user || !cur_config->xnr_config)) {
		if (copy_from_compatible(&css_param->xnr_config,
					 arg->xnr_config,
					 sizeof(struct ia_css_xnr_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.xnr_config =
		    (struct atomisp_xnr_config *)
		    &css_param->xnr_config;
	}

	if (arg->yuv2rgb_cc_config &&
	    (from_user || !cur_config->yuv2rgb_cc_config)) {
		if (copy_from_compatible(&css_param->yuv2rgb_cc_config,
					 arg->yuv2rgb_cc_config,
					 sizeof(struct ia_css_cc_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.yuv2rgb_cc_config =
		    (struct atomisp_cc_config *)
		    &css_param->yuv2rgb_cc_config;
	}

	if (arg->rgb2yuv_cc_config &&
	    (from_user || !cur_config->rgb2yuv_cc_config)) {
		if (copy_from_compatible(&css_param->rgb2yuv_cc_config,
					 arg->rgb2yuv_cc_config,
					 sizeof(struct ia_css_cc_config),
					 from_user))
			return -EFAULT;
		css_param->update_flag.rgb2yuv_cc_config =
		    (struct atomisp_cc_config *)
		    &css_param->rgb2yuv_cc_config;
	}

	if (arg->macc_table && (from_user || !cur_config->macc_table)) {
		if (copy_from_compatible(&css_param->macc_table,
					 arg->macc_table,
					 sizeof(struct ia_css_macc_table),
					 from_user))
			return -EFAULT;
		css_param->update_flag.macc_table =
		    (struct atomisp_macc_table *)
		    &css_param->macc_table;
	}

	if (arg->xnr_table && (from_user || !cur_config->xnr_table)) {
		if (copy_from_compatible(&css_param->xnr_table,
					 arg->xnr_table,
					 sizeof(struct ia_css_xnr_table),
					 from_user))
			return -EFAULT;
		css_param->update_flag.xnr_table =
		    (struct atomisp_xnr_table *)&css_param->xnr_table;
	}

	if (arg->r_gamma_table && (from_user || !cur_config->r_gamma_table)) {
		if (copy_from_compatible(&css_param->r_gamma_table,
					 arg->r_gamma_table,
					 sizeof(struct ia_css_rgb_gamma_table),
					 from_user))
			return -EFAULT;
		css_param->update_flag.r_gamma_table =
		    (struct atomisp_rgb_gamma_table *)
		    &css_param->r_gamma_table;
	}

	if (arg->g_gamma_table && (from_user || !cur_config->g_gamma_table)) {
		if (copy_from_compatible(&css_param->g_gamma_table,
					 arg->g_gamma_table,
					 sizeof(struct ia_css_rgb_gamma_table),
					 from_user))
			return -EFAULT;
		css_param->update_flag.g_gamma_table =
		    (struct atomisp_rgb_gamma_table *)
		    &css_param->g_gamma_table;
	}

	if (arg->b_gamma_table && (from_user || !cur_config->b_gamma_table)) {
		if (copy_from_compatible(&css_param->b_gamma_table,
					 arg->b_gamma_table,
					 sizeof(struct ia_css_rgb_gamma_table),
					 from_user))
			return -EFAULT;
		css_param->update_flag.b_gamma_table =
		    (struct atomisp_rgb_gamma_table *)
		    &css_param->b_gamma_table;
	}

	if (arg->anr_thres && (from_user || !cur_config->anr_thres)) {
		if (copy_from_compatible(&css_param->anr_thres, arg->anr_thres,
					 sizeof(struct ia_css_anr_thres),
					 from_user))
			return -EFAULT;
		css_param->update_flag.anr_thres =
		    (struct atomisp_anr_thres *)&css_param->anr_thres;
	}

	if (from_user)
		css_param->isp_config_id = arg->isp_config_id;
	/*
	 * These configurations are on used by ISP1.x, not for ISP2.x,
	 * so do not handle them. see comments of ia_css_isp_config.
	 * 1 cc_config
	 * 2 ce_config
	 * 3 de_config
	 * 4 gc_config
	 * 5 gamma_table
	 * 6 ctc_table
	 * 7 dvs_coefs
	 */
	return 0;
}

int atomisp_cp_lsc_table(struct atomisp_sub_device *asd,
			 struct atomisp_shading_table *source_st,
			 struct atomisp_css_params *css_param,
			 bool from_user)
{
	unsigned int i;
	unsigned int len_table;
	struct ia_css_shading_table *shading_table;
	struct ia_css_shading_table *old_table;
	struct atomisp_shading_table *st, dest_st;

	if (!source_st)
		return 0;

	if (!css_param)
		return -EINVAL;

	if (!from_user && css_param->update_flag.shading_table)
		return 0;

	if (IS_ISP2401) {
		if (copy_from_compatible(&dest_st, source_st,
					sizeof(struct atomisp_shading_table),
					from_user)) {
			dev_err(asd->isp->dev, "copy shading table failed!");
			return -EFAULT;
		}
		st = &dest_st;
	} else {
		st = source_st;
	}

	old_table = css_param->shading_table;

	/* user config is to disable the shading table. */
	if (!st->enable) {
		/* Generate a minimum table with enable = 0. */
		shading_table = atomisp_css_shading_table_alloc(1, 1);
		if (!shading_table)
			return -ENOMEM;
		shading_table->enable = 0;
		goto set_lsc;
	}

	/* Setting a new table. Validate first - all tables must be set */
	for (i = 0; i < ATOMISP_NUM_SC_COLORS; i++) {
		if (!st->data[i]) {
			dev_err(asd->isp->dev, "shading table validate failed");
			return -EINVAL;
		}
	}

	/* Shading table size per color */
	if (st->width > SH_CSS_MAX_SCTBL_WIDTH_PER_COLOR ||
	    st->height > SH_CSS_MAX_SCTBL_HEIGHT_PER_COLOR) {
		dev_err(asd->isp->dev, "shading table w/h validate failed!");
		return -EINVAL;
	}

	shading_table = atomisp_css_shading_table_alloc(st->width, st->height);
	if (!shading_table)
		return -ENOMEM;

	len_table = st->width * st->height * ATOMISP_SC_TYPE_SIZE;
	for (i = 0; i < ATOMISP_NUM_SC_COLORS; i++) {
		if (copy_from_compatible(shading_table->data[i],
					 st->data[i], len_table, from_user)) {
			atomisp_css_shading_table_free(shading_table);
			return -EFAULT;
		}
	}
	shading_table->sensor_width = st->sensor_width;
	shading_table->sensor_height = st->sensor_height;
	shading_table->fraction_bits = st->fraction_bits;
	shading_table->enable = st->enable;

	/* No need to update shading table if it is the same */
	if (old_table &&
	    old_table->sensor_width == shading_table->sensor_width &&
	    old_table->sensor_height == shading_table->sensor_height &&
	    old_table->width == shading_table->width &&
	    old_table->height == shading_table->height &&
	    old_table->fraction_bits == shading_table->fraction_bits &&
	    old_table->enable == shading_table->enable) {
		bool data_is_same = true;

		for (i = 0; i < ATOMISP_NUM_SC_COLORS; i++) {
			if (memcmp(shading_table->data[i], old_table->data[i],
				   len_table) != 0) {
				data_is_same = false;
				break;
			}
		}

		if (data_is_same) {
			atomisp_css_shading_table_free(shading_table);
			return 0;
		}
	}

set_lsc:
	/* set LSC to CSS */
	css_param->shading_table = shading_table;
	css_param->update_flag.shading_table = (struct atomisp_shading_table *)shading_table;
	asd->params.sc_en = shading_table;

	if (old_table)
		atomisp_css_shading_table_free(old_table);

	return 0;
}

int atomisp_css_cp_dvs2_coefs(struct atomisp_sub_device *asd,
			      struct ia_css_dvs2_coefficients *coefs,
			      struct atomisp_css_params *css_param,
			      bool from_user)
{
	struct ia_css_dvs_grid_info *cur =
	    atomisp_css_get_dvs_grid_info(&asd->params.curr_grid_info);
	int dvs_hor_coef_bytes, dvs_ver_coef_bytes;
	struct ia_css_dvs2_coefficients dvs2_coefs;

	if (!coefs || !cur)
		return 0;

	if (!from_user && css_param->update_flag.dvs2_coefs)
		return 0;

	if (!IS_ISP2401) {
		if (sizeof(*cur) != sizeof(coefs->grid) ||
		    memcmp(&coefs->grid, cur, sizeof(coefs->grid))) {
			dev_err(asd->isp->dev, "dvs grid mismatch!\n");
			/* If the grid info in the argument differs from the current
			grid info, we tell the caller to reset the grid size and
			try again. */
			return -EAGAIN;
		}

		if (!coefs->hor_coefs.odd_real ||
		    !coefs->hor_coefs.odd_imag ||
		    !coefs->hor_coefs.even_real ||
		    !coefs->hor_coefs.even_imag ||
		    !coefs->ver_coefs.odd_real ||
		    !coefs->ver_coefs.odd_imag ||
		    !coefs->ver_coefs.even_real ||
		    !coefs->ver_coefs.even_imag)
			return -EINVAL;

		if (!css_param->dvs2_coeff) {
			/* DIS coefficients. */
			css_param->dvs2_coeff = ia_css_dvs2_coefficients_allocate(cur);
			if (!css_param->dvs2_coeff)
				return -ENOMEM;
		}

		dvs_hor_coef_bytes = asd->params.dvs_hor_coef_bytes;
		dvs_ver_coef_bytes = asd->params.dvs_ver_coef_bytes;
		if (copy_from_compatible(css_param->dvs2_coeff->hor_coefs.odd_real,
					coefs->hor_coefs.odd_real, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->hor_coefs.odd_imag,
					coefs->hor_coefs.odd_imag, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->hor_coefs.even_real,
					coefs->hor_coefs.even_real, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->hor_coefs.even_imag,
					coefs->hor_coefs.even_imag, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.odd_real,
					coefs->ver_coefs.odd_real, dvs_ver_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.odd_imag,
					coefs->ver_coefs.odd_imag, dvs_ver_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.even_real,
					coefs->ver_coefs.even_real, dvs_ver_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.even_imag,
					coefs->ver_coefs.even_imag, dvs_ver_coef_bytes, from_user)) {
			ia_css_dvs2_coefficients_free(css_param->dvs2_coeff);
			css_param->dvs2_coeff = NULL;
			return -EFAULT;
		}
	} else {
		if (copy_from_compatible(&dvs2_coefs, coefs,
					sizeof(struct ia_css_dvs2_coefficients),
					from_user)) {
			dev_err(asd->isp->dev, "copy dvs2 coef failed");
			return -EFAULT;
		}

		if (sizeof(*cur) != sizeof(dvs2_coefs.grid) ||
		    memcmp(&dvs2_coefs.grid, cur, sizeof(dvs2_coefs.grid))) {
			dev_err(asd->isp->dev, "dvs grid mismatch!\n");
			/* If the grid info in the argument differs from the current
			grid info, we tell the caller to reset the grid size and
			try again. */
			return -EAGAIN;
		}

		if (!dvs2_coefs.hor_coefs.odd_real ||
		    !dvs2_coefs.hor_coefs.odd_imag ||
		    !dvs2_coefs.hor_coefs.even_real ||
		    !dvs2_coefs.hor_coefs.even_imag ||
		    !dvs2_coefs.ver_coefs.odd_real ||
		    !dvs2_coefs.ver_coefs.odd_imag ||
		    !dvs2_coefs.ver_coefs.even_real ||
		    !dvs2_coefs.ver_coefs.even_imag)
			return -EINVAL;

		if (!css_param->dvs2_coeff) {
			/* DIS coefficients. */
			css_param->dvs2_coeff = ia_css_dvs2_coefficients_allocate(cur);
			if (!css_param->dvs2_coeff)
				return -ENOMEM;
		}

		dvs_hor_coef_bytes = asd->params.dvs_hor_coef_bytes;
		dvs_ver_coef_bytes = asd->params.dvs_ver_coef_bytes;
		if (copy_from_compatible(css_param->dvs2_coeff->hor_coefs.odd_real,
					dvs2_coefs.hor_coefs.odd_real, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->hor_coefs.odd_imag,
					dvs2_coefs.hor_coefs.odd_imag, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->hor_coefs.even_real,
					dvs2_coefs.hor_coefs.even_real, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->hor_coefs.even_imag,
					dvs2_coefs.hor_coefs.even_imag, dvs_hor_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.odd_real,
					dvs2_coefs.ver_coefs.odd_real, dvs_ver_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.odd_imag,
					dvs2_coefs.ver_coefs.odd_imag, dvs_ver_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.even_real,
					dvs2_coefs.ver_coefs.even_real, dvs_ver_coef_bytes, from_user) ||
		    copy_from_compatible(css_param->dvs2_coeff->ver_coefs.even_imag,
					dvs2_coefs.ver_coefs.even_imag, dvs_ver_coef_bytes, from_user)) {
			ia_css_dvs2_coefficients_free(css_param->dvs2_coeff);
			css_param->dvs2_coeff = NULL;
			return -EFAULT;
		}
	}

	css_param->update_flag.dvs2_coefs =
	    (struct atomisp_dis_coefficients *)css_param->dvs2_coeff;
	return 0;
}

int atomisp_cp_dvs_6axis_config(struct atomisp_sub_device *asd,
				struct atomisp_dvs_6axis_config *source_6axis_config,
				struct atomisp_css_params *css_param,
				bool from_user)
{
	struct ia_css_dvs_6axis_config *dvs_6axis_config;
	struct ia_css_dvs_6axis_config *old_6axis_config;
	struct ia_css_stream *stream =
		    asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream;
	struct ia_css_dvs_grid_info *dvs_grid_info =
	    atomisp_css_get_dvs_grid_info(&asd->params.curr_grid_info);
	int ret = -EFAULT;

	if (!stream) {
		dev_err(asd->isp->dev, "%s: internal error!", __func__);
		return -EINVAL;
	}

	if (!source_6axis_config || !dvs_grid_info)
		return 0;

	if (!dvs_grid_info->enable)
		return 0;

	if (!from_user && css_param->update_flag.dvs_6axis_config)
		return 0;

	/* check whether need to reallocate for 6 axis config */
	old_6axis_config = css_param->dvs_6axis;
	dvs_6axis_config = old_6axis_config;

	if (IS_ISP2401) {
		struct ia_css_dvs_6axis_config t_6axis_config;

		if (copy_from_compatible(&t_6axis_config, source_6axis_config,
					sizeof(struct atomisp_dvs_6axis_config),
					from_user)) {
			dev_err(asd->isp->dev, "copy morph table failed!");
			return -EFAULT;
		}

		if (old_6axis_config &&
		    (old_6axis_config->width_y != t_6axis_config.width_y ||
		    old_6axis_config->height_y != t_6axis_config.height_y ||
		    old_6axis_config->width_uv != t_6axis_config.width_uv ||
		    old_6axis_config->height_uv != t_6axis_config.height_uv)) {
			ia_css_dvs2_6axis_config_free(css_param->dvs_6axis);
			css_param->dvs_6axis = NULL;

			dvs_6axis_config = ia_css_dvs2_6axis_config_allocate(stream);
			if (!dvs_6axis_config)
				return -ENOMEM;
		} else if (!dvs_6axis_config) {
			dvs_6axis_config = ia_css_dvs2_6axis_config_allocate(stream);
			if (!dvs_6axis_config)
				return -ENOMEM;
		}

		dvs_6axis_config->exp_id = t_6axis_config.exp_id;

		if (copy_from_compatible(dvs_6axis_config->xcoords_y,
					t_6axis_config.xcoords_y,
					t_6axis_config.width_y *
					t_6axis_config.height_y *
					sizeof(*dvs_6axis_config->xcoords_y),
					from_user))
			goto error;
		if (copy_from_compatible(dvs_6axis_config->ycoords_y,
					t_6axis_config.ycoords_y,
					t_6axis_config.width_y *
					t_6axis_config.height_y *
					sizeof(*dvs_6axis_config->ycoords_y),
					from_user))
			goto error;
		if (copy_from_compatible(dvs_6axis_config->xcoords_uv,
					t_6axis_config.xcoords_uv,
					t_6axis_config.width_uv *
					t_6axis_config.height_uv *
					sizeof(*dvs_6axis_config->xcoords_uv),
					from_user))
			goto error;
		if (copy_from_compatible(dvs_6axis_config->ycoords_uv,
					t_6axis_config.ycoords_uv,
					t_6axis_config.width_uv *
					t_6axis_config.height_uv *
					sizeof(*dvs_6axis_config->ycoords_uv),
					from_user))
			goto error;
	} else {
		if (old_6axis_config &&
		    (old_6axis_config->width_y != source_6axis_config->width_y ||
		    old_6axis_config->height_y != source_6axis_config->height_y ||
		    old_6axis_config->width_uv != source_6axis_config->width_uv ||
		    old_6axis_config->height_uv != source_6axis_config->height_uv)) {
			ia_css_dvs2_6axis_config_free(css_param->dvs_6axis);
			css_param->dvs_6axis = NULL;

			dvs_6axis_config = ia_css_dvs2_6axis_config_allocate(stream);
			if (!dvs_6axis_config)
				return -ENOMEM;
		} else if (!dvs_6axis_config) {
			dvs_6axis_config = ia_css_dvs2_6axis_config_allocate(stream);
			if (!dvs_6axis_config)
				return -ENOMEM;
		}

		dvs_6axis_config->exp_id = source_6axis_config->exp_id;

		if (copy_from_compatible(dvs_6axis_config->xcoords_y,
					source_6axis_config->xcoords_y,
					source_6axis_config->width_y *
					source_6axis_config->height_y *
					sizeof(*source_6axis_config->xcoords_y),
					from_user))
			goto error;
		if (copy_from_compatible(dvs_6axis_config->ycoords_y,
					source_6axis_config->ycoords_y,
					source_6axis_config->width_y *
					source_6axis_config->height_y *
					sizeof(*source_6axis_config->ycoords_y),
					from_user))
			goto error;
		if (copy_from_compatible(dvs_6axis_config->xcoords_uv,
					source_6axis_config->xcoords_uv,
					source_6axis_config->width_uv *
					source_6axis_config->height_uv *
					sizeof(*source_6axis_config->xcoords_uv),
					from_user))
			goto error;
		if (copy_from_compatible(dvs_6axis_config->ycoords_uv,
					source_6axis_config->ycoords_uv,
					source_6axis_config->width_uv *
					source_6axis_config->height_uv *
					sizeof(*source_6axis_config->ycoords_uv),
					from_user))
			goto error;
	}
	css_param->dvs_6axis = dvs_6axis_config;
	css_param->update_flag.dvs_6axis_config =
	    (struct atomisp_dvs_6axis_config *)dvs_6axis_config;
	return 0;

error:
	if (dvs_6axis_config)
		ia_css_dvs2_6axis_config_free(dvs_6axis_config);
	return ret;
}

int atomisp_cp_morph_table(struct atomisp_sub_device *asd,
			   struct atomisp_morph_table *source_morph_table,
			   struct atomisp_css_params *css_param,
			   bool from_user)
{
	int ret = -EFAULT;
	unsigned int i;
	struct ia_css_morph_table *morph_table;
	struct ia_css_morph_table *old_morph_table;

	if (!source_morph_table)
		return 0;

	if (!from_user && css_param->update_flag.morph_table)
		return 0;

	old_morph_table = css_param->morph_table;

	if (IS_ISP2401) {
		struct ia_css_morph_table mtbl;

		if (copy_from_compatible(&mtbl, source_morph_table,
				sizeof(struct atomisp_morph_table),
				from_user)) {
			dev_err(asd->isp->dev, "copy morph table failed!");
			return -EFAULT;
		}

		morph_table = atomisp_css_morph_table_allocate(
				mtbl.width,
				mtbl.height);
		if (!morph_table)
			return -ENOMEM;

		for (i = 0; i < IA_CSS_MORPH_TABLE_NUM_PLANES; i++) {
			if (copy_from_compatible(morph_table->coordinates_x[i],
						(__force void *)source_morph_table->coordinates_x[i],
						mtbl.height * mtbl.width *
						sizeof(*morph_table->coordinates_x[i]),
						from_user))
				goto error;

			if (copy_from_compatible(morph_table->coordinates_y[i],
						(__force void *)source_morph_table->coordinates_y[i],
						mtbl.height * mtbl.width *
						sizeof(*morph_table->coordinates_y[i]),
						from_user))
				goto error;
		}
	} else {
		morph_table = atomisp_css_morph_table_allocate(
				source_morph_table->width,
				source_morph_table->height);
		if (!morph_table)
			return -ENOMEM;

		for (i = 0; i < IA_CSS_MORPH_TABLE_NUM_PLANES; i++) {
			if (copy_from_compatible(morph_table->coordinates_x[i],
						(__force void *)source_morph_table->coordinates_x[i],
						source_morph_table->height * source_morph_table->width *
						sizeof(*source_morph_table->coordinates_x[i]),
						from_user))
				goto error;

			if (copy_from_compatible(morph_table->coordinates_y[i],
						(__force void *)source_morph_table->coordinates_y[i],
						source_morph_table->height * source_morph_table->width *
						sizeof(*source_morph_table->coordinates_y[i]),
						from_user))
				goto error;
		}
	}

	css_param->morph_table = morph_table;
	if (old_morph_table)
		atomisp_css_morph_table_free(old_morph_table);
	css_param->update_flag.morph_table =
	    (struct atomisp_morph_table *)morph_table;
	return 0;

error:
	if (morph_table)
		atomisp_css_morph_table_free(morph_table);
	return ret;
}

int atomisp_makeup_css_parameters(struct atomisp_sub_device *asd,
				  struct atomisp_parameters *arg,
				  struct atomisp_css_params *css_param)
{
	int ret;

	ret = atomisp_cp_general_isp_parameters(asd, arg, css_param, false);
	if (ret)
		return ret;
	ret = atomisp_cp_lsc_table(asd, arg->shading_table, css_param, false);
	if (ret)
		return ret;
	ret = atomisp_cp_morph_table(asd, arg->morph_table, css_param, false);
	if (ret)
		return ret;
	ret = atomisp_css_cp_dvs2_coefs(asd,
					(struct ia_css_dvs2_coefficients *)arg->dvs2_coefs,
					css_param, false);
	if (ret)
		return ret;
	ret = atomisp_cp_dvs_6axis_config(asd, arg->dvs_6axis_config,
					  css_param, false);
	return ret;
}

void atomisp_free_css_parameters(struct atomisp_css_params *css_param)
{
	if (css_param->dvs_6axis) {
		ia_css_dvs2_6axis_config_free(css_param->dvs_6axis);
		css_param->dvs_6axis = NULL;
	}
	if (css_param->dvs2_coeff) {
		ia_css_dvs2_coefficients_free(css_param->dvs2_coeff);
		css_param->dvs2_coeff = NULL;
	}
	if (css_param->shading_table) {
		ia_css_shading_table_free(css_param->shading_table);
		css_param->shading_table = NULL;
	}
	if (css_param->morph_table) {
		ia_css_morph_table_free(css_param->morph_table);
		css_param->morph_table = NULL;
	}
}

static void atomisp_move_frame_to_activeq(struct ia_css_frame *frame,
					  struct atomisp_css_params_with_list *param)
{
	struct atomisp_video_pipe *pipe = vb_to_pipe(&frame->vb.vb2_buf);
	unsigned long irqflags;

	pipe->frame_params[frame->vb.vb2_buf.index] = param;
	spin_lock_irqsave(&pipe->irq_lock, irqflags);
	list_move_tail(&frame->queue, &pipe->activeq);
	spin_unlock_irqrestore(&pipe->irq_lock, irqflags);
}

/*
 * Check parameter queue list and buffer queue list to find out if matched items
 * and then set parameter to CSS and enqueue buffer to CSS.
 * Of course, if the buffer in buffer waiting list is not bound to a per-frame
 * parameter, it will be enqueued into CSS as long as the per-frame setting
 * buffers before it get enqueued.
 */
void atomisp_handle_parameter_and_buffer(struct atomisp_video_pipe *pipe)
{
	struct atomisp_sub_device *asd = pipe->asd;
	struct ia_css_frame *frame = NULL, *frame_tmp;
	struct atomisp_css_params_with_list *param = NULL, *param_tmp;
	bool need_to_enqueue_buffer = false;
	int i;

	lockdep_assert_held(&asd->isp->mutex);

	/*
	 * CSS/FW requires set parameter and enqueue buffer happen after ISP
	 * is streamon.
	 */
	if (!asd->streaming)
		return;

	if (list_empty(&pipe->per_frame_params) ||
	    list_empty(&pipe->buffers_waiting_for_param))
		return;

	list_for_each_entry_safe(frame, frame_tmp,
				 &pipe->buffers_waiting_for_param, queue) {
		i = frame->vb.vb2_buf.index;
		if (pipe->frame_request_config_id[i]) {
			list_for_each_entry_safe(param, param_tmp,
						 &pipe->per_frame_params, list) {
				if (pipe->frame_request_config_id[i] != param->params.isp_config_id)
					continue;

				list_del(&param->list);

				/*
				 * clear the request config id as the buffer
				 * will be handled and enqueued into CSS soon
				 */
				pipe->frame_request_config_id[i] = 0;
				atomisp_move_frame_to_activeq(frame, param);
				need_to_enqueue_buffer = true;
				break;
			}

			/* If this is the end, stop further loop */
			if (list_entry_is_head(param, &pipe->per_frame_params, list))
				break;
		} else {
			atomisp_move_frame_to_activeq(frame, NULL);
			need_to_enqueue_buffer = true;
		}
	}

	if (!need_to_enqueue_buffer)
		return;

	atomisp_qbuffers_to_css(asd);
}

/*
* Function to configure ISP parameters
*/
int atomisp_set_parameters(struct video_device *vdev,
			   struct atomisp_parameters *arg)
{
	struct atomisp_video_pipe *pipe = atomisp_to_video_pipe(vdev);
	struct atomisp_sub_device *asd = pipe->asd;
	struct atomisp_css_params_with_list *param = NULL;
	struct atomisp_css_params *css_param = &asd->params.css_param;
	int ret;

	lockdep_assert_held(&asd->isp->mutex);

	if (!asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream) {
		dev_err(asd->isp->dev, "%s: internal error!\n", __func__);
		return -EINVAL;
	}

	dev_dbg(asd->isp->dev, "set parameter(per_frame_setting %d) isp_config_id %d of %s\n",
		arg->per_frame_setting, arg->isp_config_id, vdev->name);

	if (arg->per_frame_setting) {
		/*
		 * Per-frame setting enabled, we allocate a new parameter
		 * buffer to cache the parameters and only when frame buffers
		 * are ready, the parameters will be set to CSS.
		 * per-frame setting only works for the main output frame.
		 */
		param = kvzalloc(sizeof(*param), GFP_KERNEL);
		if (!param) {
			dev_err(asd->isp->dev, "%s: failed to alloc params buffer\n",
				__func__);
			return -ENOMEM;
		}
		css_param = &param->params;
	}

	ret = atomisp_cp_general_isp_parameters(asd, arg, css_param, true);
	if (ret)
		goto apply_parameter_failed;

	ret = atomisp_cp_lsc_table(asd, arg->shading_table, css_param, true);
	if (ret)
		goto apply_parameter_failed;

	ret = atomisp_cp_morph_table(asd, arg->morph_table, css_param, true);
	if (ret)
		goto apply_parameter_failed;

	ret = atomisp_css_cp_dvs2_coefs(asd,
					(struct ia_css_dvs2_coefficients *)arg->dvs2_coefs,
					css_param, true);
	if (ret)
		goto apply_parameter_failed;

	ret = atomisp_cp_dvs_6axis_config(asd, arg->dvs_6axis_config,
					  css_param, true);
	if (ret)
		goto apply_parameter_failed;

	if (!arg->per_frame_setting) {
		/* indicate to CSS that we have parameters to be updated */
		asd->params.css_update_params_needed = true;
	} else {
		list_add_tail(&param->list, &pipe->per_frame_params);
		atomisp_handle_parameter_and_buffer(pipe);
	}

	return 0;

apply_parameter_failed:
	if (css_param)
		atomisp_free_css_parameters(css_param);
	kvfree(param);

	return ret;
}

/*
 * Function to set/get isp parameters to isp
 */
int atomisp_param(struct atomisp_sub_device *asd, int flag,
		  struct atomisp_parm *config)
{
	struct ia_css_pipe_config *vp_cfg =
		    &asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].
		    pipe_configs[IA_CSS_PIPE_ID_VIDEO];

	/* Read parameter for 3A binary info */
	if (flag == 0) {
		struct ia_css_dvs_grid_info *dvs_grid_info =
		    atomisp_css_get_dvs_grid_info(
			&asd->params.curr_grid_info);

		atomisp_curr_user_grid_info(asd, &config->info);

		/* We always return the resolution and stride even if there is
		 * no valid metadata. This allows the caller to get the
		 * information needed to allocate user-space buffers. */
		config->metadata_config.metadata_height = asd->
			stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream_info.
			metadata_info.resolution.height;
		config->metadata_config.metadata_stride = asd->
			stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream_info.
			metadata_info.stride;

		/* update dvs grid info */
		if (dvs_grid_info)
			memcpy(&config->dvs_grid,
			       dvs_grid_info,
			       sizeof(struct ia_css_dvs_grid_info));

		if (asd->run_mode->val != ATOMISP_RUN_MODE_VIDEO) {
			config->dvs_envelop.width = 0;
			config->dvs_envelop.height = 0;
			return 0;
		}

		/* update dvs envelop info */
		config->dvs_envelop.width = vp_cfg->dvs_envelope.width;
		config->dvs_envelop.height = vp_cfg->dvs_envelope.height;
		return 0;
	}

	memcpy(&asd->params.css_param.wb_config, &config->wb_config,
	       sizeof(struct ia_css_wb_config));
	memcpy(&asd->params.css_param.ob_config, &config->ob_config,
	       sizeof(struct ia_css_ob_config));
	memcpy(&asd->params.css_param.dp_config, &config->dp_config,
	       sizeof(struct ia_css_dp_config));
	memcpy(&asd->params.css_param.de_config, &config->de_config,
	       sizeof(struct ia_css_de_config));
	memcpy(&asd->params.css_param.dz_config, &config->dz_config,
	       sizeof(struct ia_css_dz_config));
	memcpy(&asd->params.css_param.ce_config, &config->ce_config,
	       sizeof(struct ia_css_ce_config));
	memcpy(&asd->params.css_param.nr_config, &config->nr_config,
	       sizeof(struct ia_css_nr_config));
	memcpy(&asd->params.css_param.ee_config, &config->ee_config,
	       sizeof(struct ia_css_ee_config));
	memcpy(&asd->params.css_param.tnr_config, &config->tnr_config,
	       sizeof(struct ia_css_tnr_config));

	if (asd->params.color_effect == V4L2_COLORFX_NEGATIVE) {
		asd->params.css_param.cc_config.matrix[3] = -config->cc_config.matrix[3];
		asd->params.css_param.cc_config.matrix[4] = -config->cc_config.matrix[4];
		asd->params.css_param.cc_config.matrix[5] = -config->cc_config.matrix[5];
		asd->params.css_param.cc_config.matrix[6] = -config->cc_config.matrix[6];
		asd->params.css_param.cc_config.matrix[7] = -config->cc_config.matrix[7];
		asd->params.css_param.cc_config.matrix[8] = -config->cc_config.matrix[8];
	}

	if (asd->params.color_effect != V4L2_COLORFX_SEPIA &&
	    asd->params.color_effect != V4L2_COLORFX_BW) {
		memcpy(&asd->params.css_param.cc_config, &config->cc_config,
		       sizeof(struct ia_css_cc_config));
		asd->params.config.cc_config = &asd->params.css_param.cc_config;
	}

	asd->params.config.wb_config = &asd->params.css_param.wb_config;
	asd->params.config.ob_config = &asd->params.css_param.ob_config;
	asd->params.config.de_config = &asd->params.css_param.de_config;
	asd->params.config.dz_config = &asd->params.css_param.dz_config;
	asd->params.config.ce_config = &asd->params.css_param.ce_config;
	asd->params.config.dp_config = &asd->params.css_param.dp_config;
	asd->params.config.nr_config = &asd->params.css_param.nr_config;
	asd->params.config.ee_config = &asd->params.css_param.ee_config;
	asd->params.config.tnr_config = &asd->params.css_param.tnr_config;
	asd->params.css_update_params_needed = true;

	return 0;
}

/*
 * Function to configure color effect of the image
 */
int atomisp_color_effect(struct atomisp_sub_device *asd, int flag,
			 __s32 *effect)
{
	struct ia_css_cc_config *cc_config = NULL;
	struct ia_css_macc_table *macc_table = NULL;
	struct ia_css_ctc_table *ctc_table = NULL;
	int ret = 0;
	struct v4l2_control control;
	struct atomisp_device *isp = asd->isp;

	if (flag == 0) {
		*effect = asd->params.color_effect;
		return 0;
	}

	control.id = V4L2_CID_COLORFX;
	control.value = *effect;
	ret =
	    v4l2_s_ctrl(NULL, isp->inputs[asd->input_curr].camera->ctrl_handler,
			&control);
	/*
	 * if set color effect to sensor successfully, return
	 * 0 directly.
	 */
	if (!ret) {
		asd->params.color_effect = (u32)*effect;
		return 0;
	}

	if (*effect == asd->params.color_effect)
		return 0;

	/*
	 * isp_subdev->params.macc_en should be set to false.
	 */
	asd->params.macc_en = false;

	switch (*effect) {
	case V4L2_COLORFX_NONE:
		macc_table = &asd->params.css_param.macc_table;
		asd->params.macc_en = true;
		break;
	case V4L2_COLORFX_SEPIA:
		cc_config = &sepia_cc_config;
		break;
	case V4L2_COLORFX_NEGATIVE:
		cc_config = &nega_cc_config;
		break;
	case V4L2_COLORFX_BW:
		cc_config = &mono_cc_config;
		break;
	case V4L2_COLORFX_SKY_BLUE:
		macc_table = &blue_macc_table;
		asd->params.macc_en = true;
		break;
	case V4L2_COLORFX_GRASS_GREEN:
		macc_table = &green_macc_table;
		asd->params.macc_en = true;
		break;
	case V4L2_COLORFX_SKIN_WHITEN_LOW:
		macc_table = &skin_low_macc_table;
		asd->params.macc_en = true;
		break;
	case V4L2_COLORFX_SKIN_WHITEN:
		macc_table = &skin_medium_macc_table;
		asd->params.macc_en = true;
		break;
	case V4L2_COLORFX_SKIN_WHITEN_HIGH:
		macc_table = &skin_high_macc_table;
		asd->params.macc_en = true;
		break;
	case V4L2_COLORFX_VIVID:
		ctc_table = &vivid_ctc_table;
		break;
	default:
		return -EINVAL;
	}
	atomisp_update_capture_mode(asd);

	if (cc_config)
		asd->params.config.cc_config = cc_config;
	if (macc_table)
		asd->params.config.macc_table = macc_table;
	if (ctc_table)
		atomisp_css_set_ctc_table(asd, ctc_table);
	asd->params.color_effect = (u32)*effect;
	asd->params.css_update_params_needed = true;
	return 0;
}

/*
 * Function to configure bad pixel correction
 */
int atomisp_bad_pixel(struct atomisp_sub_device *asd, int flag,
		      __s32 *value)
{
	if (flag == 0) {
		*value = asd->params.bad_pixel_en;
		return 0;
	}
	asd->params.bad_pixel_en = !!*value;

	return 0;
}

/*
 * Function to configure bad pixel correction params
 */
int atomisp_bad_pixel_param(struct atomisp_sub_device *asd, int flag,
			    struct atomisp_dp_config *config)
{
	if (flag == 0) {
		/* Get bad pixel from current setup */
		if (atomisp_css_get_dp_config(asd, config))
			return -EINVAL;
	} else {
		/* Set bad pixel to isp parameters */
		memcpy(&asd->params.css_param.dp_config, config,
		       sizeof(asd->params.css_param.dp_config));
		asd->params.config.dp_config = &asd->params.css_param.dp_config;
		asd->params.css_update_params_needed = true;
	}

	return 0;
}

/*
 * Function to enable/disable video image stablization
 */
int atomisp_video_stable(struct atomisp_sub_device *asd, int flag,
			 __s32 *value)
{
	if (flag == 0)
		*value = asd->params.video_dis_en;
	else
		asd->params.video_dis_en = !!*value;

	return 0;
}

/*
 * Function to configure fixed pattern noise
 */
int atomisp_fixed_pattern(struct atomisp_sub_device *asd, int flag,
			  __s32 *value)
{
	if (flag == 0) {
		*value = asd->params.fpn_en;
		return 0;
	}

	if (*value == 0) {
		asd->params.fpn_en = false;
		return 0;
	}

	/* Add function to get black from from sensor with shutter off */
	return 0;
}

static unsigned int
atomisp_bytesperline_to_padded_width(unsigned int bytesperline,
				     enum ia_css_frame_format format)
{
	switch (format) {
	case IA_CSS_FRAME_FORMAT_UYVY:
	case IA_CSS_FRAME_FORMAT_YUYV:
	case IA_CSS_FRAME_FORMAT_RAW:
	case IA_CSS_FRAME_FORMAT_RGB565:
		return bytesperline / 2;
	case IA_CSS_FRAME_FORMAT_RGBA888:
		return bytesperline / 4;
	/* The following cases could be removed, but we leave them
	   in to document the formats that are included. */
	case IA_CSS_FRAME_FORMAT_NV11:
	case IA_CSS_FRAME_FORMAT_NV12:
	case IA_CSS_FRAME_FORMAT_NV16:
	case IA_CSS_FRAME_FORMAT_NV21:
	case IA_CSS_FRAME_FORMAT_NV61:
	case IA_CSS_FRAME_FORMAT_YV12:
	case IA_CSS_FRAME_FORMAT_YV16:
	case IA_CSS_FRAME_FORMAT_YUV420:
	case IA_CSS_FRAME_FORMAT_YUV420_16:
	case IA_CSS_FRAME_FORMAT_YUV422:
	case IA_CSS_FRAME_FORMAT_YUV422_16:
	case IA_CSS_FRAME_FORMAT_YUV444:
	case IA_CSS_FRAME_FORMAT_YUV_LINE:
	case IA_CSS_FRAME_FORMAT_PLANAR_RGB888:
	case IA_CSS_FRAME_FORMAT_QPLANE6:
	case IA_CSS_FRAME_FORMAT_BINARY_8:
	default:
		return bytesperline;
	}
}

static int
atomisp_v4l2_framebuffer_to_css_frame(const struct v4l2_framebuffer *arg,
				      struct ia_css_frame **result)
{
	struct ia_css_frame *res = NULL;
	unsigned int padded_width;
	enum ia_css_frame_format sh_format;
	char *tmp_buf = NULL;
	int ret = 0;

	sh_format = v4l2_fmt_to_sh_fmt(arg->fmt.pixelformat);
	padded_width = atomisp_bytesperline_to_padded_width(
			   arg->fmt.bytesperline, sh_format);

	/* Note: the padded width on an ia_css_frame is in elements, not in
	   bytes. The RAW frame we use here should always be a 16bit RAW
	   frame. This is why we bytesperline/2 is equal to the padded with */
	if (ia_css_frame_allocate(&res, arg->fmt.width, arg->fmt.height,
				       sh_format, padded_width, 0)) {
		ret = -ENOMEM;
		goto err;
	}

	tmp_buf = vmalloc(arg->fmt.sizeimage);
	if (!tmp_buf) {
		ret = -ENOMEM;
		goto err;
	}
	if (copy_from_user(tmp_buf, (void __user __force *)arg->base,
			   arg->fmt.sizeimage)) {
		ret = -EFAULT;
		goto err;
	}

	if (hmm_store(res->data, tmp_buf, arg->fmt.sizeimage)) {
		ret = -EINVAL;
		goto err;
	}

err:
	if (ret && res)
		ia_css_frame_free(res);
	vfree(tmp_buf);
	if (ret == 0)
		*result = res;
	return ret;
}

/*
 * Function to configure fixed pattern noise table
 */
int atomisp_fixed_pattern_table(struct atomisp_sub_device *asd,
				struct v4l2_framebuffer *arg)
{
	struct ia_css_frame *raw_black_frame = NULL;
	int ret;

	if (!arg)
		return -EINVAL;

	ret = atomisp_v4l2_framebuffer_to_css_frame(arg, &raw_black_frame);
	if (ret)
		return ret;

	if (sh_css_set_black_frame(asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].stream,
				   raw_black_frame) != 0)
		return -ENOMEM;

	ia_css_frame_free(raw_black_frame);
	return ret;
}

/*
 * Function to configure false color correction
 */
int atomisp_false_color(struct atomisp_sub_device *asd, int flag,
			__s32 *value)
{
	/* Get nr config from current setup */
	if (flag == 0) {
		*value = asd->params.false_color;
		return 0;
	}

	/* Set nr config to isp parameters */
	if (*value) {
		asd->params.config.de_config = NULL;
	} else {
		asd->params.css_param.de_config.pixelnoise = 0;
		asd->params.config.de_config = &asd->params.css_param.de_config;
	}
	asd->params.css_update_params_needed = true;
	asd->params.false_color = *value;
	return 0;
}

/*
 * Function to configure bad pixel correction params
 */
int atomisp_false_color_param(struct atomisp_sub_device *asd, int flag,
			      struct atomisp_de_config *config)
{
	if (flag == 0) {
		/* Get false color from current setup */
		if (atomisp_css_get_de_config(asd, config))
			return -EINVAL;
	} else {
		/* Set false color to isp parameters */
		memcpy(&asd->params.css_param.de_config, config,
		       sizeof(asd->params.css_param.de_config));
		asd->params.config.de_config = &asd->params.css_param.de_config;
		asd->params.css_update_params_needed = true;
	}

	return 0;
}

/*
 * Function to configure white balance params
 */
int atomisp_white_balance_param(struct atomisp_sub_device *asd, int flag,
				struct atomisp_wb_config *config)
{
	if (flag == 0) {
		/* Get white balance from current setup */
		if (atomisp_css_get_wb_config(asd, config))
			return -EINVAL;
	} else {
		/* Set white balance to isp parameters */
		memcpy(&asd->params.css_param.wb_config, config,
		       sizeof(asd->params.css_param.wb_config));
		asd->params.config.wb_config = &asd->params.css_param.wb_config;
		asd->params.css_update_params_needed = true;
	}

	return 0;
}

int atomisp_3a_config_param(struct atomisp_sub_device *asd, int flag,
			    struct atomisp_3a_config *config)
{
	struct atomisp_device *isp = asd->isp;

	dev_dbg(isp->dev, ">%s %d\n", __func__, flag);

	if (flag == 0) {
		/* Get white balance from current setup */
		if (atomisp_css_get_3a_config(asd, config))
			return -EINVAL;
	} else {
		/* Set white balance to isp parameters */
		memcpy(&asd->params.css_param.s3a_config, config,
		       sizeof(asd->params.css_param.s3a_config));
		asd->params.config.s3a_config = &asd->params.css_param.s3a_config;
		asd->params.css_update_params_needed = true;
	}

	dev_dbg(isp->dev, "<%s %d\n", __func__, flag);
	return 0;
}

/*
 * Function to setup digital zoom
 */
int atomisp_digital_zoom(struct atomisp_sub_device *asd, int flag,
			 __s32 *value)
{
	u32 zoom;
	struct atomisp_device *isp = asd->isp;

	unsigned int max_zoom = MRFLD_MAX_ZOOM_FACTOR;

	if (flag == 0) {
		atomisp_css_get_zoom_factor(asd, &zoom);
		*value = max_zoom - zoom;
	} else {
		if (*value < 0)
			return -EINVAL;

		zoom = max_zoom - min_t(u32, max_zoom - 1, *value);
		atomisp_css_set_zoom_factor(asd, zoom);

		dev_dbg(isp->dev, "%s, zoom: %d\n", __func__, zoom);
		asd->params.css_update_params_needed = true;
	}

	return 0;
}

static void __atomisp_update_stream_env(struct atomisp_sub_device *asd,
					u16 stream_index, struct atomisp_input_stream_info *stream_info)
{
	int i;

	/* assign virtual channel id return from sensor driver query */
	asd->stream_env[stream_index].ch_id = stream_info->ch_id;
	asd->stream_env[stream_index].isys_configs = stream_info->isys_configs;
	for (i = 0; i < stream_info->isys_configs; i++) {
		asd->stream_env[stream_index].isys_info[i].input_format =
		    stream_info->isys_info[i].input_format;
		asd->stream_env[stream_index].isys_info[i].width =
		    stream_info->isys_info[i].width;
		asd->stream_env[stream_index].isys_info[i].height =
		    stream_info->isys_info[i].height;
	}
}

static void __atomisp_init_stream_info(u16 stream_index,
				       struct atomisp_input_stream_info *stream_info)
{
	int i;

	stream_info->enable = 1;
	stream_info->stream = stream_index;
	stream_info->ch_id = 0;
	stream_info->isys_configs = 0;
	for (i = 0; i < MAX_STREAMS_PER_CHANNEL; i++) {
		stream_info->isys_info[i].input_format = 0;
		stream_info->isys_info[i].width = 0;
		stream_info->isys_info[i].height = 0;
	}
}

static void atomisp_fill_pix_format(struct v4l2_pix_format *f,
				    u32 width, u32 height,
				    const struct atomisp_format_bridge *br_fmt)
{
	u32 bytes;

	f->width = width;
	f->height = height;
	f->pixelformat = br_fmt->pixelformat;

	/* Adding padding to width for bytesperline calculation */
	width = ia_css_frame_pad_width(width, br_fmt->sh_fmt);
	bytes = BITS_TO_BYTES(br_fmt->depth * width);

	if (br_fmt->planar)
		f->bytesperline = width;
	else
		f->bytesperline = bytes;

	f->sizeimage = PAGE_ALIGN(height * bytes);

	if (f->field == V4L2_FIELD_ANY)
		f->field = V4L2_FIELD_NONE;

	/*
	 * FIXME: do we need to set this up differently, depending on the
	 * sensor or the pipeline?
	 */
	f->colorspace = V4L2_COLORSPACE_REC709;
	f->ycbcr_enc = V4L2_YCBCR_ENC_709;
	f->xfer_func = V4L2_XFER_FUNC_709;
}

/* Get sensor padding values for the non padded width x height resolution */
void atomisp_get_padding(struct atomisp_device *isp, u32 width, u32 height,
			 u32 *padding_w, u32 *padding_h)
{
	struct atomisp_input_subdev *input = &isp->inputs[isp->asd.input_curr];
	struct v4l2_rect native_rect = input->native_rect;
	const struct atomisp_in_fmt_conv *fc = NULL;
	u32 min_pad_w = ISP2400_MIN_PAD_W;
	u32 min_pad_h = ISP2400_MIN_PAD_H;
	struct v4l2_mbus_framefmt *sink;

	if (!input->crop_support) {
		*padding_w = pad_w;
		*padding_h = pad_h;
		return;
	}

	width = min(width, input->active_rect.width);
	height = min(height, input->active_rect.height);

	if (input->binning_support && width <= (input->active_rect.width / 2) &&
				      height <= (input->active_rect.height / 2)) {
		native_rect.width /= 2;
		native_rect.height /= 2;
	}

	*padding_w = min_t(u32, (native_rect.width - width) & ~1, pad_w);
	*padding_h = min_t(u32, (native_rect.height - height) & ~1, pad_h);

	/* The below minimum padding requirements are for BYT / ISP2400 only */
	if (IS_ISP2401)
		return;

	sink = atomisp_subdev_get_ffmt(&isp->asd.subdev, NULL, V4L2_SUBDEV_FORMAT_ACTIVE,
				       ATOMISP_SUBDEV_PAD_SINK);
	if (sink)
		fc = atomisp_find_in_fmt_conv(sink->code);
	if (!fc) {
		dev_warn(isp->dev, "%s: Could not get sensor format\n", __func__);
		goto apply_min_padding;
	}

	/*
	 * The ISP only supports GRBG for other bayer-orders additional padding
	 * is used so that the raw sensor data can be cropped to fix the order.
	 */
	if (fc->bayer_order == IA_CSS_BAYER_ORDER_RGGB ||
	    fc->bayer_order == IA_CSS_BAYER_ORDER_GBRG)
		min_pad_w += 2;

	if (fc->bayer_order == IA_CSS_BAYER_ORDER_BGGR ||
	    fc->bayer_order == IA_CSS_BAYER_ORDER_GBRG)
		min_pad_h += 2;

apply_min_padding:
	*padding_w = max_t(u32, *padding_w, min_pad_w);
	*padding_h = max_t(u32, *padding_h, min_pad_h);
}

static int atomisp_set_crop(struct atomisp_device *isp,
			    const struct v4l2_mbus_framefmt *format,
			    int which)
{
	struct atomisp_input_subdev *input = &isp->inputs[isp->asd.input_curr];
	struct v4l2_subdev_state pad_state = {
		.pads = &input->pad_cfg,
	};
	struct v4l2_subdev_selection sel = {
		.which = which,
		.target = V4L2_SEL_TGT_CROP,
		.r.width = format->width,
		.r.height = format->height,
	};
	int ret;

	if (!input->crop_support)
		return 0;

	/* Cropping is done before binning, when binning double the crop rect */
	if (input->binning_support && sel.r.width <= (input->native_rect.width / 2) &&
				      sel.r.height <= (input->native_rect.height / 2)) {
		sel.r.width *= 2;
		sel.r.height *= 2;
	}

	/* Clamp to avoid top/left calculations overflowing */
	sel.r.width = min(sel.r.width, input->native_rect.width);
	sel.r.height = min(sel.r.height, input->native_rect.height);

	sel.r.left = ((input->native_rect.width - sel.r.width) / 2) & ~1;
	sel.r.top = ((input->native_rect.height - sel.r.height) / 2) & ~1;

	ret = v4l2_subdev_call(input->camera, pad, set_selection, &pad_state, &sel);
	if (ret)
		dev_err(isp->dev, "Error setting crop to %ux%u @%ux%u: %d\n",
			sel.r.width, sel.r.height, sel.r.left, sel.r.top, ret);

	return ret;
}

/* This function looks up the closest available resolution. */
int atomisp_try_fmt(struct atomisp_device *isp, struct v4l2_pix_format *f,
		    const struct atomisp_format_bridge **fmt_ret,
		    const struct atomisp_format_bridge **snr_fmt_ret)
{
	const struct atomisp_format_bridge *fmt, *snr_fmt;
	struct atomisp_sub_device *asd = &isp->asd;
	struct atomisp_input_subdev *input = &isp->inputs[asd->input_curr];
	struct v4l2_subdev_state pad_state = {
		.pads = &input->pad_cfg,
	};
	struct v4l2_subdev_format format = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
	};
	u32 padding_w, padding_h;
	int ret;

	if (!input->camera)
		return -EINVAL;

	fmt = atomisp_get_format_bridge(f->pixelformat);
	/* Currently, raw formats are broken!!! */
	if (!fmt || fmt->sh_fmt == IA_CSS_FRAME_FORMAT_RAW) {
		f->pixelformat = V4L2_PIX_FMT_YUV420;

		fmt = atomisp_get_format_bridge(f->pixelformat);
		if (!fmt)
			return -EINVAL;
	}

	/*
	 * atomisp_set_fmt() will set the sensor resolution to the requested
	 * resolution + padding. Add padding here and remove it again after
	 * the set_fmt call, like atomisp_set_fmt_to_snr() does.
	 */
	atomisp_get_padding(isp, f->width, f->height, &padding_w, &padding_h);
	v4l2_fill_mbus_format(&format.format, f, fmt->mbus_code);
	format.format.width += padding_w;
	format.format.height += padding_h;

	dev_dbg(isp->dev, "try_mbus_fmt: asking for %ux%u\n",
		format.format.width, format.format.height);

	ret = atomisp_set_crop(isp, &format.format, V4L2_SUBDEV_FORMAT_TRY);
	if (ret)
		return ret;

	ret = v4l2_subdev_call(input->camera, pad, set_fmt, &pad_state, &format);
	if (ret)
		return ret;

	dev_dbg(isp->dev, "try_mbus_fmt: got %ux%u\n",
		format.format.width, format.format.height);

	snr_fmt = atomisp_get_format_bridge_from_mbus(format.format.code);
	if (!snr_fmt) {
		dev_err(isp->dev, "unknown sensor format 0x%8.8x\n",
			format.format.code);
		return -EINVAL;
	}

	f->width = format.format.width - padding_w;
	f->height = format.format.height - padding_h;

	/*
	 * If the format is jpeg or custom RAW, then the width and height will
	 * not satisfy the normal atomisp requirements and no need to check
	 * the below conditions. So just assign to what is being returned from
	 * the sensor driver.
	 */
	if (f->pixelformat == V4L2_PIX_FMT_JPEG ||
	    f->pixelformat == V4L2_PIX_FMT_CUSTOM_M10MO_RAW)
		goto out_fill_pix_format;

	/* app vs isp */
	f->width = rounddown(clamp_t(u32, f->width, ATOM_ISP_MIN_WIDTH,
				     ATOM_ISP_MAX_WIDTH), ATOM_ISP_STEP_WIDTH);
	f->height = rounddown(clamp_t(u32, f->height, ATOM_ISP_MIN_HEIGHT,
				      ATOM_ISP_MAX_HEIGHT), ATOM_ISP_STEP_HEIGHT);

out_fill_pix_format:
	atomisp_fill_pix_format(f, f->width, f->height, fmt);

	if (fmt_ret)
		*fmt_ret = fmt;

	if (snr_fmt_ret)
		*snr_fmt_ret = snr_fmt;

	return 0;
}

enum mipi_port_id atomisp_port_to_mipi_port(struct atomisp_device *isp,
					    enum atomisp_camera_port port)
{
	switch (port) {
	case ATOMISP_CAMERA_PORT_PRIMARY:
		return MIPI_PORT0_ID;
	case ATOMISP_CAMERA_PORT_SECONDARY:
		return MIPI_PORT1_ID;
	case ATOMISP_CAMERA_PORT_TERTIARY:
		return MIPI_PORT2_ID;
	default:
		dev_err(isp->dev, "unsupported port: %d\n", port);
		return MIPI_PORT0_ID;
	}
}

static inline int atomisp_set_sensor_mipi_to_isp(
    struct atomisp_sub_device *asd,
    enum atomisp_input_stream_id stream_id,
    struct camera_mipi_info *mipi_info)
{
	struct v4l2_control ctrl;
	struct atomisp_device *isp = asd->isp;
	struct atomisp_input_subdev *input = &isp->inputs[asd->input_curr];
	const struct atomisp_in_fmt_conv *fc;
	int mipi_freq = 0;
	unsigned int input_format, bayer_order;
	enum atomisp_input_format metadata_format = ATOMISP_INPUT_FORMAT_EMBEDDED;
	u32 mipi_port, metadata_width = 0, metadata_height = 0;

	ctrl.id = V4L2_CID_LINK_FREQ;
	if (v4l2_g_ctrl(input->camera->ctrl_handler, &ctrl) == 0)
		mipi_freq = ctrl.value;

	if (asd->stream_env[stream_id].isys_configs == 1) {
		input_format =
		    asd->stream_env[stream_id].isys_info[0].input_format;
		atomisp_css_isys_set_format(asd, stream_id,
					    input_format, IA_CSS_STREAM_DEFAULT_ISYS_STREAM_IDX);
	} else if (asd->stream_env[stream_id].isys_configs == 2) {
		atomisp_css_isys_two_stream_cfg_update_stream1(
		    asd, stream_id,
		    asd->stream_env[stream_id].isys_info[0].input_format,
		    asd->stream_env[stream_id].isys_info[0].width,
		    asd->stream_env[stream_id].isys_info[0].height);

		atomisp_css_isys_two_stream_cfg_update_stream2(
		    asd, stream_id,
		    asd->stream_env[stream_id].isys_info[1].input_format,
		    asd->stream_env[stream_id].isys_info[1].width,
		    asd->stream_env[stream_id].isys_info[1].height);
	}

	/* Compatibility for sensors which provide no media bus code
	 * in s_mbus_framefmt() nor support pad formats. */
	if (mipi_info && mipi_info->input_format != -1) {
		bayer_order = mipi_info->raw_bayer_order;

		/* Input stream config is still needs configured */
		/* TODO: Check if this is necessary */
		fc = atomisp_find_in_fmt_conv_by_atomisp_in_fmt(
			 mipi_info->input_format);
		if (!fc)
			return -EINVAL;
		input_format = fc->atomisp_in_fmt;
		metadata_format = mipi_info->metadata_format;
		metadata_width = mipi_info->metadata_width;
		metadata_height = mipi_info->metadata_height;
	} else {
		struct v4l2_mbus_framefmt *sink;

		sink = atomisp_subdev_get_ffmt(&asd->subdev, NULL,
					       V4L2_SUBDEV_FORMAT_ACTIVE,
					       ATOMISP_SUBDEV_PAD_SINK);
		fc = atomisp_find_in_fmt_conv(sink->code);
		if (!fc)
			return -EINVAL;
		input_format = fc->atomisp_in_fmt;
		bayer_order = fc->bayer_order;
	}

	atomisp_css_input_set_format(asd, stream_id, input_format);
	atomisp_css_input_set_bayer_order(asd, stream_id, bayer_order);

	fc = atomisp_find_in_fmt_conv_by_atomisp_in_fmt(metadata_format);
	if (!fc)
		return -EINVAL;

	input_format = fc->atomisp_in_fmt;
	mipi_port = atomisp_port_to_mipi_port(isp, input->port);
	atomisp_css_input_configure_port(asd, mipi_port,
					 isp->sensor_lanes[mipi_port],
					 0xffff4, mipi_freq,
					 input_format,
					 metadata_width, metadata_height);
	return 0;
}

static int configure_pp_input_nop(struct atomisp_sub_device *asd,
				  unsigned int width, unsigned int height)
{
	return 0;
}

static int configure_output_nop(struct atomisp_sub_device *asd,
				unsigned int width, unsigned int height,
				unsigned int min_width,
				enum ia_css_frame_format sh_fmt)
{
	return 0;
}

static int get_frame_info_nop(struct atomisp_sub_device *asd,
			      struct ia_css_frame_info *finfo)
{
	return 0;
}

/*
 * Resets CSS parameters that depend on input resolution.
 *
 * Update params like CSS RAW binning, 2ppc mode and pp_input
 * which depend on input size, but are not automatically
 * handled in CSS when the input resolution is changed.
 */
static int css_input_resolution_changed(struct atomisp_sub_device *asd,
					struct v4l2_mbus_framefmt *ffmt)
{
	struct atomisp_metadata_buf *md_buf = NULL, *_md_buf;
	unsigned int i;

	dev_dbg(asd->isp->dev, "css_input_resolution_changed to %ux%u\n",
		ffmt->width, ffmt->height);

	if (IS_ISP2401)
		atomisp_css_input_set_two_pixels_per_clock(asd, false);
	else
		atomisp_css_input_set_two_pixels_per_clock(asd, true);

	/*
	 * If sensor input changed, which means metadata resolution changed
	 * together. Release all metadata buffers here to let it re-allocated
	 * next time in reqbufs.
	 */
	for (i = 0; i < ATOMISP_METADATA_TYPE_NUM; i++) {
		list_for_each_entry_safe(md_buf, _md_buf, &asd->metadata[i],
					 list) {
			atomisp_css_free_metadata_buffer(md_buf);
			list_del(&md_buf->list);
			kfree(md_buf);
		}
	}
	return 0;

	/*
	 * TODO: atomisp_css_preview_configure_pp_input() not
	 *       reset due to CSS bug tracked as PSI BZ 115124
	 */
}

static int atomisp_set_fmt_to_isp(struct video_device *vdev,
				  struct ia_css_frame_info *output_info,
				  const struct v4l2_pix_format *pix)
{
	struct camera_mipi_info *mipi_info;
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct atomisp_sub_device *asd = atomisp_to_video_pipe(vdev)->asd;
	struct atomisp_input_subdev *input = &isp->inputs[asd->input_curr];
	const struct atomisp_format_bridge *format;
	struct v4l2_rect *isp_sink_crop;
	enum ia_css_pipe_id pipe_id;
	int (*configure_output)(struct atomisp_sub_device *asd,
				unsigned int width, unsigned int height,
				unsigned int min_width,
				enum ia_css_frame_format sh_fmt) =
				    configure_output_nop;
	int (*get_frame_info)(struct atomisp_sub_device *asd,
			      struct ia_css_frame_info *finfo) =
				  get_frame_info_nop;
	int (*configure_pp_input)(struct atomisp_sub_device *asd,
				  unsigned int width, unsigned int height) =
				      configure_pp_input_nop;
	const struct atomisp_in_fmt_conv *fc = NULL;
	int ret, i;

	isp_sink_crop = atomisp_subdev_get_rect(
			    &asd->subdev, NULL, V4L2_SUBDEV_FORMAT_ACTIVE,
			    ATOMISP_SUBDEV_PAD_SINK, V4L2_SEL_TGT_CROP);

	format = atomisp_get_format_bridge(pix->pixelformat);
	if (!format)
		return -EINVAL;

	if (input->type != TEST_PATTERN) {
		mipi_info = atomisp_to_sensor_mipi_info(input->camera);

		if (atomisp_set_sensor_mipi_to_isp(asd, ATOMISP_INPUT_STREAM_GENERAL,
						   mipi_info))
			return -EINVAL;

		if (mipi_info)
			fc = atomisp_find_in_fmt_conv_by_atomisp_in_fmt(mipi_info->input_format);

		if (!fc)
			fc = atomisp_find_in_fmt_conv(
				 atomisp_subdev_get_ffmt(&asd->subdev,
							 NULL, V4L2_SUBDEV_FORMAT_ACTIVE,
							 ATOMISP_SUBDEV_PAD_SINK)->code);
		if (!fc)
			return -EINVAL;
		if (format->sh_fmt == IA_CSS_FRAME_FORMAT_RAW &&
		    raw_output_format_match_input(fc->atomisp_in_fmt,
						  pix->pixelformat))
			return -EINVAL;
	}

	/*
	 * Configure viewfinder also when vfpp is disabled: the
	 * CSS still requires viewfinder configuration.
	 */
	{
		u32 width, height;

		if (pix->width < 640 || pix->height < 480) {
			width = pix->width;
			height = pix->height;
		} else {
			width = 640;
			height = 480;
		}

		if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO ||
		    asd->vfpp->val == ATOMISP_VFPP_DISABLE_SCALER) {
			atomisp_css_video_configure_viewfinder(asd, width, height, 0,
							       IA_CSS_FRAME_FORMAT_NV12);
		} else if (asd->run_mode->val == ATOMISP_RUN_MODE_STILL_CAPTURE ||
			   asd->vfpp->val == ATOMISP_VFPP_DISABLE_LOWLAT) {
			atomisp_css_capture_configure_viewfinder(asd, width, height, 0,
								 IA_CSS_FRAME_FORMAT_NV12);
		}
	}

	atomisp_css_input_set_mode(asd, IA_CSS_INPUT_MODE_BUFFERED_SENSOR);

	for (i = 0; i < IA_CSS_PIPE_ID_NUM; i++)
		asd->stream_env[ATOMISP_INPUT_STREAM_GENERAL].pipe_extra_configs[i].disable_vf_pp = asd->vfpp->val != ATOMISP_VFPP_ENABLE;

	/* ISP2401 new input system need to use copy pipe */
	if (asd->copy_mode) {
		pipe_id = IA_CSS_PIPE_ID_COPY;
		atomisp_css_capture_enable_online(asd, ATOMISP_INPUT_STREAM_GENERAL, false);
	} else if (asd->vfpp->val == ATOMISP_VFPP_DISABLE_SCALER) {
		/* video same in continuouscapture and online modes */
		configure_output = atomisp_css_video_configure_output;
		get_frame_info = atomisp_css_video_get_output_frame_info;
		pipe_id = IA_CSS_PIPE_ID_VIDEO;
	} else if (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO) {
		configure_output = atomisp_css_video_configure_output;
		get_frame_info = atomisp_css_video_get_output_frame_info;
		pipe_id = IA_CSS_PIPE_ID_VIDEO;
	} else if (asd->run_mode->val == ATOMISP_RUN_MODE_PREVIEW) {
		configure_output = atomisp_css_preview_configure_output;
		get_frame_info = atomisp_css_preview_get_output_frame_info;
		configure_pp_input = atomisp_css_preview_configure_pp_input;
		pipe_id = IA_CSS_PIPE_ID_PREVIEW;
	} else {
		if (format->sh_fmt == IA_CSS_FRAME_FORMAT_RAW) {
			atomisp_css_capture_set_mode(asd, IA_CSS_CAPTURE_MODE_RAW);
			atomisp_css_enable_dz(asd, false);
		} else {
			atomisp_update_capture_mode(asd);
		}

		/* in case of ANR, force capture pipe to offline mode */
		atomisp_css_capture_enable_online(asd, ATOMISP_INPUT_STREAM_GENERAL,
						  !asd->params.low_light);

		configure_output = atomisp_css_capture_configure_output;
		get_frame_info = atomisp_css_capture_get_output_frame_info;
		configure_pp_input = atomisp_css_capture_configure_pp_input;
		pipe_id = IA_CSS_PIPE_ID_CAPTURE;

		if (asd->run_mode->val != ATOMISP_RUN_MODE_STILL_CAPTURE) {
			dev_err(isp->dev,
				"Need to set the running mode first\n");
			asd->run_mode->val = ATOMISP_RUN_MODE_STILL_CAPTURE;
		}
	}

	if (asd->copy_mode)
		ret = atomisp_css_copy_configure_output(asd, ATOMISP_INPUT_STREAM_GENERAL,
							pix->width, pix->height,
							format->planar ? pix->bytesperline :
							pix->bytesperline * 8 / format->depth,
							format->sh_fmt);
	else
		ret = configure_output(asd, pix->width, pix->height,
				       format->planar ? pix->bytesperline :
				       pix->bytesperline * 8 / format->depth,
				       format->sh_fmt);
	if (ret) {
		dev_err(isp->dev, "configure_output %ux%u, format %8.8x\n",
			pix->width, pix->height, format->sh_fmt);
		return -EINVAL;
	}

	ret = configure_pp_input(asd, isp_sink_crop->width, isp_sink_crop->height);
	if (ret) {
		dev_err(isp->dev, "configure_pp_input %ux%u\n",
			isp_sink_crop->width,
			isp_sink_crop->height);
		return -EINVAL;
	}
	if (asd->copy_mode)
		ret = atomisp_css_copy_get_output_frame_info(asd,
							     ATOMISP_INPUT_STREAM_GENERAL,
							     output_info);
	else
		ret = get_frame_info(asd, output_info);
	if (ret) {
		dev_err(isp->dev, "__get_frame_info %ux%u (padded to %u) returned %d\n",
			pix->width, pix->height, pix->bytesperline, ret);
		return ret;
	}

	atomisp_update_grid_info(asd, pipe_id);
	return 0;
}

static void atomisp_get_dis_envelop(struct atomisp_sub_device *asd,
				    unsigned int width, unsigned int height,
				    unsigned int *dvs_env_w, unsigned int *dvs_env_h)
{
	if (asd->params.video_dis_en &&
	    asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO) {
		/* envelope is 20% of the output resolution */
		/*
		 * dvs envelope cannot be round up.
		 * it would cause ISP timeout and color switch issue
		 */
		*dvs_env_w = rounddown(width / 5, ATOM_ISP_STEP_WIDTH);
		*dvs_env_h = rounddown(height / 5, ATOM_ISP_STEP_HEIGHT);
	}

	asd->params.dis_proj_data_valid = false;
	asd->params.css_update_params_needed = true;
}

static void atomisp_check_copy_mode(struct atomisp_sub_device *asd,
				    const struct v4l2_pix_format *f)
{
	struct v4l2_mbus_framefmt *sink, *src;

	if (!IS_ISP2401) {
		/* Only used for the new input system */
		asd->copy_mode = false;
		return;
	}

	sink = atomisp_subdev_get_ffmt(&asd->subdev, NULL,
				       V4L2_SUBDEV_FORMAT_ACTIVE, ATOMISP_SUBDEV_PAD_SINK);
	src = atomisp_subdev_get_ffmt(&asd->subdev, NULL,
				      V4L2_SUBDEV_FORMAT_ACTIVE, ATOMISP_SUBDEV_PAD_SOURCE);

	if (sink->code == src->code && sink->width == f->width && sink->height == f->height)
		asd->copy_mode = true;
	else
		asd->copy_mode = false;

	dev_dbg(asd->isp->dev, "copy_mode: %d\n", asd->copy_mode);
}

static int atomisp_set_fmt_to_snr(struct video_device *vdev, const struct v4l2_pix_format *f,
				  unsigned int dvs_env_w, unsigned int dvs_env_h)
{
	struct atomisp_video_pipe *pipe = atomisp_to_video_pipe(vdev);
	struct atomisp_sub_device *asd = pipe->asd;
	struct atomisp_device *isp = asd->isp;
	struct atomisp_input_subdev *input = &isp->inputs[asd->input_curr];
	const struct atomisp_format_bridge *format;
	struct v4l2_subdev_state pad_state = {
		.pads = &input->pad_cfg,
	};
	struct v4l2_subdev_format vformat = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
	};
	struct v4l2_mbus_framefmt *ffmt = &vformat.format;
	struct v4l2_mbus_framefmt *req_ffmt;
	struct atomisp_input_stream_info *stream_info =
	    (struct atomisp_input_stream_info *)ffmt->reserved;
	int ret;

	format = atomisp_get_format_bridge(f->pixelformat);
	if (!format)
		return -EINVAL;

	v4l2_fill_mbus_format(ffmt, f, format->mbus_code);
	ffmt->height += asd->sink_pad_padding_h + dvs_env_h;
	ffmt->width += asd->sink_pad_padding_w + dvs_env_w;

	dev_dbg(isp->dev, "s_mbus_fmt: ask %ux%u (padding %ux%u, dvs %ux%u)\n",
		ffmt->width, ffmt->height, asd->sink_pad_padding_w, asd->sink_pad_padding_h,
		dvs_env_w, dvs_env_h);

	__atomisp_init_stream_info(ATOMISP_INPUT_STREAM_GENERAL, stream_info);

	req_ffmt = ffmt;

	/* Disable dvs if resolution can't be supported by sensor */
	if (asd->params.video_dis_en && asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO) {
		ret = atomisp_set_crop(isp, &vformat.format, V4L2_SUBDEV_FORMAT_TRY);
		if (ret)
			return ret;

		vformat.which = V4L2_SUBDEV_FORMAT_TRY;
		ret = v4l2_subdev_call(input->camera, pad, set_fmt, &pad_state, &vformat);
		if (ret)
			return ret;

		dev_dbg(isp->dev, "video dis: sensor width: %d, height: %d\n",
			ffmt->width, ffmt->height);

		if (ffmt->width < req_ffmt->width ||
		    ffmt->height < req_ffmt->height) {
			req_ffmt->height -= dvs_env_h;
			req_ffmt->width -= dvs_env_w;
			ffmt = req_ffmt;
			dev_warn(isp->dev,
				 "can not enable video dis due to sensor limitation.");
			asd->params.video_dis_en = false;
		}
	}

	ret = atomisp_set_crop(isp, &vformat.format, V4L2_SUBDEV_FORMAT_ACTIVE);
	if (ret)
		return ret;

	vformat.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	ret = v4l2_subdev_call(input->camera, pad, set_fmt, NULL, &vformat);
	if (ret)
		return ret;

	__atomisp_update_stream_env(asd, ATOMISP_INPUT_STREAM_GENERAL, stream_info);

	dev_dbg(isp->dev, "sensor width: %d, height: %d\n",
		ffmt->width, ffmt->height);

	if (ffmt->width < ATOM_ISP_STEP_WIDTH ||
	    ffmt->height < ATOM_ISP_STEP_HEIGHT)
		return -EINVAL;

	if (asd->params.video_dis_en && asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO &&
	    (ffmt->width < req_ffmt->width || ffmt->height < req_ffmt->height)) {
		dev_warn(isp->dev,
			 "can not enable video dis due to sensor limitation.");
		asd->params.video_dis_en = false;
	}

	atomisp_subdev_set_ffmt(&asd->subdev, NULL,
				V4L2_SUBDEV_FORMAT_ACTIVE,
				ATOMISP_SUBDEV_PAD_SINK, ffmt);

	return css_input_resolution_changed(asd, ffmt);
}

int atomisp_set_fmt(struct video_device *vdev, struct v4l2_format *f)
{
	struct atomisp_device *isp = video_get_drvdata(vdev);
	struct atomisp_video_pipe *pipe = atomisp_to_video_pipe(vdev);
	struct atomisp_sub_device *asd = pipe->asd;
	const struct atomisp_format_bridge *format_bridge;
	const struct atomisp_format_bridge *snr_format_bridge;
	struct ia_css_frame_info output_info;
	unsigned int dvs_env_w = 0, dvs_env_h = 0;
	struct v4l2_mbus_framefmt isp_source_fmt = {0};
	struct v4l2_rect isp_sink_crop;
	int ret;

	ret = atomisp_pipe_check(pipe, true);
	if (ret)
		return ret;

	dev_dbg(isp->dev,
		"setting resolution %ux%u bytesperline %u\n",
		f->fmt.pix.width, f->fmt.pix.height, f->fmt.pix.bytesperline);

	/* Ensure that the resolution is equal or below the maximum supported */
	ret = atomisp_try_fmt(isp, &f->fmt.pix, &format_bridge, &snr_format_bridge);
	if (ret)
		return ret;

	pipe->sh_fmt = format_bridge->sh_fmt;
	pipe->pix.pixelformat = format_bridge->pixelformat;

	atomisp_subdev_get_ffmt(&asd->subdev, NULL,
				V4L2_SUBDEV_FORMAT_ACTIVE,
				ATOMISP_SUBDEV_PAD_SINK)->code =
				    snr_format_bridge->mbus_code;

	isp_source_fmt.code = format_bridge->mbus_code;
	atomisp_subdev_set_ffmt(&asd->subdev, NULL,
				V4L2_SUBDEV_FORMAT_ACTIVE,
				ATOMISP_SUBDEV_PAD_SOURCE, &isp_source_fmt);

	if (atomisp_subdev_format_conversion(asd)) {
		atomisp_get_padding(isp, f->fmt.pix.width, f->fmt.pix.height,
				    &asd->sink_pad_padding_w, &asd->sink_pad_padding_h);
	} else {
		asd->sink_pad_padding_w = 0;
		asd->sink_pad_padding_h = 0;
	}

	atomisp_get_dis_envelop(asd, f->fmt.pix.width, f->fmt.pix.height,
				&dvs_env_w, &dvs_env_h);

	ret = atomisp_set_fmt_to_snr(vdev, &f->fmt.pix, dvs_env_w, dvs_env_h);
	if (ret) {
		dev_warn(isp->dev,
			 "Set format to sensor failed with %d\n", ret);
		return -EINVAL;
	}

	atomisp_csi_lane_config(isp);

	atomisp_check_copy_mode(asd, &f->fmt.pix);

	isp_sink_crop = *atomisp_subdev_get_rect(&asd->subdev, NULL,
			V4L2_SUBDEV_FORMAT_ACTIVE,
			ATOMISP_SUBDEV_PAD_SINK,
			V4L2_SEL_TGT_CROP);

	/* Try to enable YUV downscaling if ISP input is 10 % (either
	 * width or height) bigger than the desired result. */
	if (!IS_MOFD ||
	    isp_sink_crop.width * 9 / 10 < f->fmt.pix.width ||
	    isp_sink_crop.height * 9 / 10 < f->fmt.pix.height ||
	    (atomisp_subdev_format_conversion(asd) &&
	     (asd->run_mode->val == ATOMISP_RUN_MODE_VIDEO ||
	      asd->vfpp->val == ATOMISP_VFPP_DISABLE_SCALER))) {
		isp_sink_crop.width = f->fmt.pix.width;
		isp_sink_crop.height = f->fmt.pix.height;

		atomisp_subdev_set_selection(&asd->subdev, NULL,
					     V4L2_SUBDEV_FORMAT_ACTIVE,
					     ATOMISP_SUBDEV_PAD_SOURCE, V4L2_SEL_TGT_COMPOSE,
					     0, &isp_sink_crop);
	} else {
		struct v4l2_rect main_compose = {0};

		main_compose.width = isp_sink_crop.width;
		main_compose.height =
		    DIV_ROUND_UP(main_compose.width * f->fmt.pix.height,
				 f->fmt.pix.width);
		if (main_compose.height > isp_sink_crop.height) {
			main_compose.height = isp_sink_crop.height;
			main_compose.width =
			    DIV_ROUND_UP(main_compose.height *
					 f->fmt.pix.width,
					 f->fmt.pix.height);
		}

		atomisp_subdev_set_selection(&asd->subdev, NULL,
					     V4L2_SUBDEV_FORMAT_ACTIVE,
					     ATOMISP_SUBDEV_PAD_SOURCE,
					     V4L2_SEL_TGT_COMPOSE, 0,
					     &main_compose);
	}

	ret = atomisp_set_fmt_to_isp(vdev, &output_info, &f->fmt.pix);
	if (ret) {
		dev_warn(isp->dev, "Can't set format on ISP. Error %d\n", ret);
		return -EINVAL;
	}

	atomisp_fill_pix_format(&pipe->pix, f->fmt.pix.width, f->fmt.pix.height, format_bridge);

	f->fmt.pix = pipe->pix;
	f->fmt.pix.priv = PAGE_ALIGN(pipe->pix.width *
				     pipe->pix.height * 2);

	dev_dbg(isp->dev, "%s: %dx%d, image size: %d, %d bytes per line\n",
		__func__,
		f->fmt.pix.width, f->fmt.pix.height,
		f->fmt.pix.sizeimage, f->fmt.pix.bytesperline);

	return 0;
}

int atomisp_set_shading_table(struct atomisp_sub_device *asd,
			      struct atomisp_shading_table *user_shading_table)
{
	struct ia_css_shading_table *shading_table;
	struct ia_css_shading_table *free_table;
	unsigned int len_table;
	int i;
	int ret = 0;

	if (!user_shading_table)
		return -EINVAL;

	if (!user_shading_table->enable) {
		asd->params.config.shading_table = NULL;
		asd->params.sc_en = false;
		return 0;
	}

	/* If enabling, all tables must be set */
	for (i = 0; i < ATOMISP_NUM_SC_COLORS; i++) {
		if (!user_shading_table->data[i])
			return -EINVAL;
	}

	/* Shading table size per color */
	if (user_shading_table->width > SH_CSS_MAX_SCTBL_WIDTH_PER_COLOR ||
	    user_shading_table->height > SH_CSS_MAX_SCTBL_HEIGHT_PER_COLOR)
		return -EINVAL;

	shading_table = atomisp_css_shading_table_alloc(
			    user_shading_table->width, user_shading_table->height);
	if (!shading_table)
		return -ENOMEM;

	len_table = user_shading_table->width * user_shading_table->height *
		    ATOMISP_SC_TYPE_SIZE;
	for (i = 0; i < ATOMISP_NUM_SC_COLORS; i++) {
		ret = copy_from_user(shading_table->data[i],
				     (void __user *)user_shading_table->data[i],
				     len_table);
		if (ret) {
			free_table = shading_table;
			ret = -EFAULT;
			goto out;
		}
	}
	shading_table->sensor_width = user_shading_table->sensor_width;
	shading_table->sensor_height = user_shading_table->sensor_height;
	shading_table->fraction_bits = user_shading_table->fraction_bits;

	free_table = asd->params.css_param.shading_table;
	asd->params.css_param.shading_table = shading_table;
	asd->params.config.shading_table = shading_table;
	asd->params.sc_en = true;

out:
	if (free_table)
		atomisp_css_shading_table_free(free_table);

	return ret;
}

int atomisp_flash_enable(struct atomisp_sub_device *asd, int num_frames)
{
	struct atomisp_device *isp = asd->isp;

	if (num_frames < 0) {
		dev_dbg(isp->dev, "%s ERROR: num_frames: %d\n", __func__,
			num_frames);
		return -EINVAL;
	}
	/* a requested flash is still in progress. */
	if (num_frames && asd->params.flash_state != ATOMISP_FLASH_IDLE) {
		dev_dbg(isp->dev, "%s flash busy: %d frames left: %d\n",
			__func__, asd->params.flash_state,
			asd->params.num_flash_frames);
		return -EBUSY;
	}

	asd->params.num_flash_frames = num_frames;
	asd->params.flash_state = ATOMISP_FLASH_REQUESTED;
	return 0;
}

static int __checking_exp_id(struct atomisp_sub_device *asd, int exp_id)
{
	struct atomisp_device *isp = asd->isp;

	if (!asd->enable_raw_buffer_lock->val) {
		dev_warn(isp->dev, "%s Raw Buffer Lock is disable.\n", __func__);
		return -EINVAL;
	}
	if (!asd->streaming) {
		dev_err(isp->dev, "%s streaming %d invalid exp_id %d.\n",
			__func__, exp_id, asd->streaming);
		return -EINVAL;
	}
	if ((exp_id > ATOMISP_MAX_EXP_ID) || (exp_id <= 0)) {
		dev_err(isp->dev, "%s exp_id %d invalid.\n", __func__, exp_id);
		return -EINVAL;
	}
	return 0;
}

void atomisp_init_raw_buffer_bitmap(struct atomisp_sub_device *asd)
{
	unsigned long flags;

	spin_lock_irqsave(&asd->raw_buffer_bitmap_lock, flags);
	memset(asd->raw_buffer_bitmap, 0, sizeof(asd->raw_buffer_bitmap));
	asd->raw_buffer_locked_count = 0;
	spin_unlock_irqrestore(&asd->raw_buffer_bitmap_lock, flags);
}

static int __is_raw_buffer_locked(struct atomisp_sub_device *asd, int exp_id)
{
	int *bitmap, bit;
	unsigned long flags;
	int ret;

	if (__checking_exp_id(asd, exp_id))
		return -EINVAL;

	bitmap = asd->raw_buffer_bitmap + exp_id / 32;
	bit = exp_id % 32;
	spin_lock_irqsave(&asd->raw_buffer_bitmap_lock, flags);
	ret = ((*bitmap) & (1 << bit));
	spin_unlock_irqrestore(&asd->raw_buffer_bitmap_lock, flags);
	return !ret;
}

static int __clear_raw_buffer_bitmap(struct atomisp_sub_device *asd, int exp_id)
{
	int *bitmap, bit;
	unsigned long flags;

	if (__is_raw_buffer_locked(asd, exp_id))
		return -EINVAL;

	bitmap = asd->raw_buffer_bitmap + exp_id / 32;
	bit = exp_id % 32;
	spin_lock_irqsave(&asd->raw_buffer_bitmap_lock, flags);
	(*bitmap) &= ~(1 << bit);
	asd->raw_buffer_locked_count--;
	spin_unlock_irqrestore(&asd->raw_buffer_bitmap_lock, flags);

	dev_dbg(asd->isp->dev, "%s: exp_id %d,  raw_buffer_locked_count %d\n",
		__func__, exp_id, asd->raw_buffer_locked_count);
	return 0;
}

int atomisp_exp_id_capture(struct atomisp_sub_device *asd, int *exp_id)
{
	struct atomisp_device *isp = asd->isp;
	int value = *exp_id;
	int ret;

	lockdep_assert_held(&isp->mutex);

	ret = __is_raw_buffer_locked(asd, value);
	if (ret) {
		dev_err(isp->dev, "%s exp_id %d invalid %d.\n", __func__, value, ret);
		return -EINVAL;
	}

	dev_dbg(isp->dev, "%s exp_id %d\n", __func__, value);
	ret = atomisp_css_exp_id_capture(asd, value);
	if (ret) {
		dev_err(isp->dev, "%s exp_id %d failed.\n", __func__, value);
		return -EIO;
	}
	return 0;
}

int atomisp_exp_id_unlock(struct atomisp_sub_device *asd, int *exp_id)
{
	struct atomisp_device *isp = asd->isp;
	int value = *exp_id;
	int ret;

	lockdep_assert_held(&isp->mutex);

	ret = __clear_raw_buffer_bitmap(asd, value);
	if (ret) {
		dev_err(isp->dev, "%s exp_id %d invalid %d.\n", __func__, value, ret);
		return -EINVAL;
	}

	dev_dbg(isp->dev, "%s exp_id %d\n", __func__, value);
	ret = atomisp_css_exp_id_unlock(asd, value);
	if (ret)
		dev_err(isp->dev, "%s exp_id %d failed, err %d.\n",
			__func__, value, ret);

	return ret;
}

int atomisp_enable_dz_capt_pipe(struct atomisp_sub_device *asd,
				unsigned int *enable)
{
	bool value;

	if (!enable)
		return -EINVAL;

	value = *enable > 0;

	atomisp_en_dz_capt_pipe(asd, value);

	return 0;
}

int atomisp_inject_a_fake_event(struct atomisp_sub_device *asd, int *event)
{
	if (!event || !asd->streaming)
		return -EINVAL;

	lockdep_assert_held(&asd->isp->mutex);

	dev_dbg(asd->isp->dev, "%s: trying to inject a fake event 0x%x\n",
		__func__, *event);

	switch (*event) {
	case V4L2_EVENT_FRAME_SYNC:
		atomisp_sof_event(asd);
		break;
	case V4L2_EVENT_FRAME_END:
		atomisp_eof_event(asd, 0);
		break;
	case V4L2_EVENT_ATOMISP_3A_STATS_READY:
		atomisp_3a_stats_ready_event(asd, 0);
		break;
	case V4L2_EVENT_ATOMISP_METADATA_READY:
		atomisp_metadata_ready_event(asd, 0);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}
