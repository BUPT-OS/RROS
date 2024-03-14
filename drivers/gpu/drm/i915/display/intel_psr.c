/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>

#include "i915_drv.h"
#include "i915_reg.h"
#include "intel_atomic.h"
#include "intel_crtc.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_dp_aux.h"
#include "intel_hdmi.h"
#include "intel_psr.h"
#include "intel_psr_regs.h"
#include "intel_snps_phy.h"
#include "skl_universal_plane.h"

/**
 * DOC: Panel Self Refresh (PSR/SRD)
 *
 * Since Haswell Display controller supports Panel Self-Refresh on display
 * panels witch have a remote frame buffer (RFB) implemented according to PSR
 * spec in eDP1.3. PSR feature allows the display to go to lower standby states
 * when system is idle but display is on as it eliminates display refresh
 * request to DDR memory completely as long as the frame buffer for that
 * display is unchanged.
 *
 * Panel Self Refresh must be supported by both Hardware (source) and
 * Panel (sink).
 *
 * PSR saves power by caching the framebuffer in the panel RFB, which allows us
 * to power down the link and memory controller. For DSI panels the same idea
 * is called "manual mode".
 *
 * The implementation uses the hardware-based PSR support which automatically
 * enters/exits self-refresh mode. The hardware takes care of sending the
 * required DP aux message and could even retrain the link (that part isn't
 * enabled yet though). The hardware also keeps track of any frontbuffer
 * changes to know when to exit self-refresh mode again. Unfortunately that
 * part doesn't work too well, hence why the i915 PSR support uses the
 * software frontbuffer tracking to make sure it doesn't miss a screen
 * update. For this integration intel_psr_invalidate() and intel_psr_flush()
 * get called by the frontbuffer tracking code. Note that because of locking
 * issues the self-refresh re-enable code is done from a work queue, which
 * must be correctly synchronized/cancelled when shutting down the pipe."
 *
 * DC3CO (DC3 clock off)
 *
 * On top of PSR2, GEN12 adds a intermediate power savings state that turns
 * clock off automatically during PSR2 idle state.
 * The smaller overhead of DC3co entry/exit vs. the overhead of PSR2 deep sleep
 * entry/exit allows the HW to enter a low-power state even when page flipping
 * periodically (for instance a 30fps video playback scenario).
 *
 * Every time a flips occurs PSR2 will get out of deep sleep state(if it was),
 * so DC3CO is enabled and tgl_dc3co_disable_work is schedule to run after 6
 * frames, if no other flip occurs and the function above is executed, DC3CO is
 * disabled and PSR2 is configured to enter deep sleep, resetting again in case
 * of another flip.
 * Front buffer modifications do not trigger DC3CO activation on purpose as it
 * would bring a lot of complexity and most of the moderns systems will only
 * use page flips.
 */

/*
 * Description of PSR mask bits:
 *
 * EDP_PSR_DEBUG[16]/EDP_PSR_DEBUG_MASK_DISP_REG_WRITE (hsw-skl):
 *
 *  When unmasked (nearly) all display register writes (eg. even
 *  SWF) trigger a PSR exit. Some registers are excluded from this
 *  and they have a more specific mask (described below). On icl+
 *  this bit no longer exists and is effectively always set.
 *
 * PIPE_MISC[21]/PIPE_MISC_PSR_MASK_PIPE_REG_WRITE (skl+):
 *
 *  When unmasked (nearly) all pipe/plane register writes
 *  trigger a PSR exit. Some plane registers are excluded from this
 *  and they have a more specific mask (described below).
 *
 * CHICKEN_PIPESL_1[11]/SKL_PSR_MASK_PLANE_FLIP (skl+):
 * PIPE_MISC[23]/PIPE_MISC_PSR_MASK_PRIMARY_FLIP (bdw):
 * EDP_PSR_DEBUG[23]/EDP_PSR_DEBUG_MASK_PRIMARY_FLIP (hsw):
 *
 *  When unmasked PRI_SURF/PLANE_SURF writes trigger a PSR exit.
 *  SPR_SURF/CURBASE are not included in this and instead are
 *  controlled by PIPE_MISC_PSR_MASK_PIPE_REG_WRITE (skl+) or
 *  EDP_PSR_DEBUG_MASK_DISP_REG_WRITE (hsw/bdw).
 *
 * PIPE_MISC[22]/PIPE_MISC_PSR_MASK_SPRITE_ENABLE (bdw):
 * EDP_PSR_DEBUG[21]/EDP_PSR_DEBUG_MASK_SPRITE_ENABLE (hsw):
 *
 *  When unmasked PSR is blocked as long as the sprite
 *  plane is enabled. skl+ with their universal planes no
 *  longer have a mask bit like this, and no plane being
 *  enabledb blocks PSR.
 *
 * PIPE_MISC[21]/PIPE_MISC_PSR_MASK_CURSOR_MOVE (bdw):
 * EDP_PSR_DEBUG[20]/EDP_PSR_DEBUG_MASK_CURSOR_MOVE (hsw):
 *
 *  When umasked CURPOS writes trigger a PSR exit. On skl+
 *  this doesn't exit but CURPOS is included in the
 *  PIPE_MISC_PSR_MASK_PIPE_REG_WRITE mask.
 *
 * PIPE_MISC[20]/PIPE_MISC_PSR_MASK_VBLANK_VSYNC_INT (bdw+):
 * EDP_PSR_DEBUG[19]/EDP_PSR_DEBUG_MASK_VBLANK_VSYNC_INT (hsw):
 *
 *  When unmasked PSR is blocked as long as vblank and/or vsync
 *  interrupt is unmasked in IMR *and* enabled in IER.
 *
 * CHICKEN_TRANS[30]/SKL_UNMASK_VBL_TO_PIPE_IN_SRD (skl+):
 * CHICKEN_PAR1_1[15]/HSW_MASK_VBL_TO_PIPE_IN_SRD (hsw/bdw):
 *
 *  Selectcs whether PSR exit generates an extra vblank before
 *  the first frame is transmitted. Also note the opposite polarity
 *  if the bit on hsw/bdw vs. skl+ (masked==generate the extra vblank,
 *  unmasked==do not generate the extra vblank).
 *
 *  With DC states enabled the extra vblank happens after link training,
 *  with DC states disabled it happens immediately upuon PSR exit trigger.
 *  No idea as of now why there is a difference. HSW/BDW (which don't
 *  even have DMC) always generate it after link training. Go figure.
 *
 *  Unfortunately CHICKEN_TRANS itself seems to be double buffered
 *  and thus won't latch until the first vblank. So with DC states
 *  enabled the register effctively uses the reset value during DC5
 *  exit+PSR exit sequence, and thus the bit does nothing until
 *  latched by the vblank that it was trying to prevent from being
 *  generated in the first place. So we should probably call this
 *  one a chicken/egg bit instead on skl+.
 *
 *  In standby mode (as opposed to link-off) this makes no difference
 *  as the timing generator keeps running the whole time generating
 *  normal periodic vblanks.
 *
 *  WaPsrDPAMaskVBlankInSRD asks us to set the bit on hsw/bdw,
 *  and doing so makes the behaviour match the skl+ reset value.
 *
 * CHICKEN_PIPESL_1[0]/BDW_UNMASK_VBL_TO_REGS_IN_SRD (bdw):
 * CHICKEN_PIPESL_1[15]/HSW_UNMASK_VBL_TO_REGS_IN_SRD (hsw):
 *
 *  On BDW without this bit is no vblanks whatsoever are
 *  generated after PSR exit. On HSW this has no apparant effect.
 *  WaPsrDPRSUnmaskVBlankInSRD says to set this.
 *
 * The rest of the bits are more self-explanatory and/or
 * irrelevant for normal operation.
 */

static bool psr_global_enabled(struct intel_dp *intel_dp)
{
	struct intel_connector *connector = intel_dp->attached_connector;
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);

	switch (intel_dp->psr.debug & I915_PSR_DEBUG_MODE_MASK) {
	case I915_PSR_DEBUG_DEFAULT:
		if (i915->params.enable_psr == -1)
			return connector->panel.vbt.psr.enable;
		return i915->params.enable_psr;
	case I915_PSR_DEBUG_DISABLE:
		return false;
	default:
		return true;
	}
}

static bool psr2_global_enabled(struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);

	switch (intel_dp->psr.debug & I915_PSR_DEBUG_MODE_MASK) {
	case I915_PSR_DEBUG_DISABLE:
	case I915_PSR_DEBUG_FORCE_PSR1:
		return false;
	default:
		if (i915->params.enable_psr == 1)
			return false;
		return true;
	}
}

static u32 psr_irq_psr_error_bit_get(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	return DISPLAY_VER(dev_priv) >= 12 ? TGL_PSR_ERROR :
		EDP_PSR_ERROR(intel_dp->psr.transcoder);
}

static u32 psr_irq_post_exit_bit_get(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	return DISPLAY_VER(dev_priv) >= 12 ? TGL_PSR_POST_EXIT :
		EDP_PSR_POST_EXIT(intel_dp->psr.transcoder);
}

static u32 psr_irq_pre_entry_bit_get(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	return DISPLAY_VER(dev_priv) >= 12 ? TGL_PSR_PRE_ENTRY :
		EDP_PSR_PRE_ENTRY(intel_dp->psr.transcoder);
}

static u32 psr_irq_mask_get(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	return DISPLAY_VER(dev_priv) >= 12 ? TGL_PSR_MASK :
		EDP_PSR_MASK(intel_dp->psr.transcoder);
}

static i915_reg_t psr_ctl_reg(struct drm_i915_private *dev_priv,
			      enum transcoder cpu_transcoder)
{
	if (DISPLAY_VER(dev_priv) >= 8)
		return EDP_PSR_CTL(cpu_transcoder);
	else
		return HSW_SRD_CTL;
}

static i915_reg_t psr_debug_reg(struct drm_i915_private *dev_priv,
				enum transcoder cpu_transcoder)
{
	if (DISPLAY_VER(dev_priv) >= 8)
		return EDP_PSR_DEBUG(cpu_transcoder);
	else
		return HSW_SRD_DEBUG;
}

static i915_reg_t psr_perf_cnt_reg(struct drm_i915_private *dev_priv,
				   enum transcoder cpu_transcoder)
{
	if (DISPLAY_VER(dev_priv) >= 8)
		return EDP_PSR_PERF_CNT(cpu_transcoder);
	else
		return HSW_SRD_PERF_CNT;
}

static i915_reg_t psr_status_reg(struct drm_i915_private *dev_priv,
				 enum transcoder cpu_transcoder)
{
	if (DISPLAY_VER(dev_priv) >= 8)
		return EDP_PSR_STATUS(cpu_transcoder);
	else
		return HSW_SRD_STATUS;
}

static i915_reg_t psr_imr_reg(struct drm_i915_private *dev_priv,
			      enum transcoder cpu_transcoder)
{
	if (DISPLAY_VER(dev_priv) >= 12)
		return TRANS_PSR_IMR(cpu_transcoder);
	else
		return EDP_PSR_IMR;
}

static i915_reg_t psr_iir_reg(struct drm_i915_private *dev_priv,
			      enum transcoder cpu_transcoder)
{
	if (DISPLAY_VER(dev_priv) >= 12)
		return TRANS_PSR_IIR(cpu_transcoder);
	else
		return EDP_PSR_IIR;
}

static i915_reg_t psr_aux_ctl_reg(struct drm_i915_private *dev_priv,
				  enum transcoder cpu_transcoder)
{
	if (DISPLAY_VER(dev_priv) >= 8)
		return EDP_PSR_AUX_CTL(cpu_transcoder);
	else
		return HSW_SRD_AUX_CTL;
}

static i915_reg_t psr_aux_data_reg(struct drm_i915_private *dev_priv,
				   enum transcoder cpu_transcoder, int i)
{
	if (DISPLAY_VER(dev_priv) >= 8)
		return EDP_PSR_AUX_DATA(cpu_transcoder, i);
	else
		return HSW_SRD_AUX_DATA(i);
}

static void psr_irq_control(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;
	u32 mask;

	mask = psr_irq_psr_error_bit_get(intel_dp);
	if (intel_dp->psr.debug & I915_PSR_DEBUG_IRQ)
		mask |= psr_irq_post_exit_bit_get(intel_dp) |
			psr_irq_pre_entry_bit_get(intel_dp);

	intel_de_rmw(dev_priv, psr_imr_reg(dev_priv, cpu_transcoder),
		     psr_irq_mask_get(intel_dp), ~mask);
}

static void psr_event_print(struct drm_i915_private *i915,
			    u32 val, bool psr2_enabled)
{
	drm_dbg_kms(&i915->drm, "PSR exit events: 0x%x\n", val);
	if (val & PSR_EVENT_PSR2_WD_TIMER_EXPIRE)
		drm_dbg_kms(&i915->drm, "\tPSR2 watchdog timer expired\n");
	if ((val & PSR_EVENT_PSR2_DISABLED) && psr2_enabled)
		drm_dbg_kms(&i915->drm, "\tPSR2 disabled\n");
	if (val & PSR_EVENT_SU_DIRTY_FIFO_UNDERRUN)
		drm_dbg_kms(&i915->drm, "\tSU dirty FIFO underrun\n");
	if (val & PSR_EVENT_SU_CRC_FIFO_UNDERRUN)
		drm_dbg_kms(&i915->drm, "\tSU CRC FIFO underrun\n");
	if (val & PSR_EVENT_GRAPHICS_RESET)
		drm_dbg_kms(&i915->drm, "\tGraphics reset\n");
	if (val & PSR_EVENT_PCH_INTERRUPT)
		drm_dbg_kms(&i915->drm, "\tPCH interrupt\n");
	if (val & PSR_EVENT_MEMORY_UP)
		drm_dbg_kms(&i915->drm, "\tMemory up\n");
	if (val & PSR_EVENT_FRONT_BUFFER_MODIFY)
		drm_dbg_kms(&i915->drm, "\tFront buffer modification\n");
	if (val & PSR_EVENT_WD_TIMER_EXPIRE)
		drm_dbg_kms(&i915->drm, "\tPSR watchdog timer expired\n");
	if (val & PSR_EVENT_PIPE_REGISTERS_UPDATE)
		drm_dbg_kms(&i915->drm, "\tPIPE registers updated\n");
	if (val & PSR_EVENT_REGISTER_UPDATE)
		drm_dbg_kms(&i915->drm, "\tRegister updated\n");
	if (val & PSR_EVENT_HDCP_ENABLE)
		drm_dbg_kms(&i915->drm, "\tHDCP enabled\n");
	if (val & PSR_EVENT_KVMR_SESSION_ENABLE)
		drm_dbg_kms(&i915->drm, "\tKVMR session enabled\n");
	if (val & PSR_EVENT_VBI_ENABLE)
		drm_dbg_kms(&i915->drm, "\tVBI enabled\n");
	if (val & PSR_EVENT_LPSP_MODE_EXIT)
		drm_dbg_kms(&i915->drm, "\tLPSP mode exited\n");
	if ((val & PSR_EVENT_PSR_DISABLE) && !psr2_enabled)
		drm_dbg_kms(&i915->drm, "\tPSR disabled\n");
}

void intel_psr_irq_handler(struct intel_dp *intel_dp, u32 psr_iir)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;
	ktime_t time_ns =  ktime_get();

	if (psr_iir & psr_irq_pre_entry_bit_get(intel_dp)) {
		intel_dp->psr.last_entry_attempt = time_ns;
		drm_dbg_kms(&dev_priv->drm,
			    "[transcoder %s] PSR entry attempt in 2 vblanks\n",
			    transcoder_name(cpu_transcoder));
	}

	if (psr_iir & psr_irq_post_exit_bit_get(intel_dp)) {
		intel_dp->psr.last_exit = time_ns;
		drm_dbg_kms(&dev_priv->drm,
			    "[transcoder %s] PSR exit completed\n",
			    transcoder_name(cpu_transcoder));

		if (DISPLAY_VER(dev_priv) >= 9) {
			u32 val;

			val = intel_de_rmw(dev_priv, PSR_EVENT(cpu_transcoder), 0, 0);

			psr_event_print(dev_priv, val, intel_dp->psr.psr2_enabled);
		}
	}

	if (psr_iir & psr_irq_psr_error_bit_get(intel_dp)) {
		drm_warn(&dev_priv->drm, "[transcoder %s] PSR aux error\n",
			 transcoder_name(cpu_transcoder));

		intel_dp->psr.irq_aux_error = true;

		/*
		 * If this interruption is not masked it will keep
		 * interrupting so fast that it prevents the scheduled
		 * work to run.
		 * Also after a PSR error, we don't want to arm PSR
		 * again so we don't care about unmask the interruption
		 * or unset irq_aux_error.
		 */
		intel_de_rmw(dev_priv, psr_imr_reg(dev_priv, cpu_transcoder),
			     0, psr_irq_psr_error_bit_get(intel_dp));

		queue_work(dev_priv->unordered_wq, &intel_dp->psr.work);
	}
}

static bool intel_dp_get_alpm_status(struct intel_dp *intel_dp)
{
	u8 alpm_caps = 0;

	if (drm_dp_dpcd_readb(&intel_dp->aux, DP_RECEIVER_ALPM_CAP,
			      &alpm_caps) != 1)
		return false;
	return alpm_caps & DP_ALPM_CAP;
}

static u8 intel_dp_get_sink_sync_latency(struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);
	u8 val = 8; /* assume the worst if we can't read the value */

	if (drm_dp_dpcd_readb(&intel_dp->aux,
			      DP_SYNCHRONIZATION_LATENCY_IN_SINK, &val) == 1)
		val &= DP_MAX_RESYNC_FRAME_COUNT_MASK;
	else
		drm_dbg_kms(&i915->drm,
			    "Unable to get sink synchronization latency, assuming 8 frames\n");
	return val;
}

static void intel_dp_get_su_granularity(struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);
	ssize_t r;
	u16 w;
	u8 y;

	/* If sink don't have specific granularity requirements set legacy ones */
	if (!(intel_dp->psr_dpcd[1] & DP_PSR2_SU_GRANULARITY_REQUIRED)) {
		/* As PSR2 HW sends full lines, we do not care about x granularity */
		w = 4;
		y = 4;
		goto exit;
	}

	r = drm_dp_dpcd_read(&intel_dp->aux, DP_PSR2_SU_X_GRANULARITY, &w, 2);
	if (r != 2)
		drm_dbg_kms(&i915->drm,
			    "Unable to read DP_PSR2_SU_X_GRANULARITY\n");
	/*
	 * Spec says that if the value read is 0 the default granularity should
	 * be used instead.
	 */
	if (r != 2 || w == 0)
		w = 4;

	r = drm_dp_dpcd_read(&intel_dp->aux, DP_PSR2_SU_Y_GRANULARITY, &y, 1);
	if (r != 1) {
		drm_dbg_kms(&i915->drm,
			    "Unable to read DP_PSR2_SU_Y_GRANULARITY\n");
		y = 4;
	}
	if (y == 0)
		y = 1;

exit:
	intel_dp->psr.su_w_granularity = w;
	intel_dp->psr.su_y_granularity = y;
}

void intel_psr_init_dpcd(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv =
		to_i915(dp_to_dig_port(intel_dp)->base.base.dev);

	drm_dp_dpcd_read(&intel_dp->aux, DP_PSR_SUPPORT, intel_dp->psr_dpcd,
			 sizeof(intel_dp->psr_dpcd));

	if (!intel_dp->psr_dpcd[0])
		return;
	drm_dbg_kms(&dev_priv->drm, "eDP panel supports PSR version %x\n",
		    intel_dp->psr_dpcd[0]);

	if (drm_dp_has_quirk(&intel_dp->desc, DP_DPCD_QUIRK_NO_PSR)) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR support not currently available for this panel\n");
		return;
	}

	if (!(intel_dp->edp_dpcd[1] & DP_EDP_SET_POWER_CAP)) {
		drm_dbg_kms(&dev_priv->drm,
			    "Panel lacks power state control, PSR cannot be enabled\n");
		return;
	}

	intel_dp->psr.sink_support = true;
	intel_dp->psr.sink_sync_latency =
		intel_dp_get_sink_sync_latency(intel_dp);

	if (DISPLAY_VER(dev_priv) >= 9 &&
	    (intel_dp->psr_dpcd[0] == DP_PSR2_WITH_Y_COORD_IS_SUPPORTED)) {
		bool y_req = intel_dp->psr_dpcd[1] &
			     DP_PSR2_SU_Y_COORDINATE_REQUIRED;
		bool alpm = intel_dp_get_alpm_status(intel_dp);

		/*
		 * All panels that supports PSR version 03h (PSR2 +
		 * Y-coordinate) can handle Y-coordinates in VSC but we are
		 * only sure that it is going to be used when required by the
		 * panel. This way panel is capable to do selective update
		 * without a aux frame sync.
		 *
		 * To support PSR version 02h and PSR version 03h without
		 * Y-coordinate requirement panels we would need to enable
		 * GTC first.
		 */
		intel_dp->psr.sink_psr2_support = y_req && alpm;
		drm_dbg_kms(&dev_priv->drm, "PSR2 %ssupported\n",
			    intel_dp->psr.sink_psr2_support ? "" : "not ");

		if (intel_dp->psr.sink_psr2_support) {
			intel_dp->psr.colorimetry_support =
				intel_dp_get_colorimetry_status(intel_dp);
			intel_dp_get_su_granularity(intel_dp);
		}
	}
}

static void hsw_psr_setup_aux(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;
	u32 aux_clock_divider, aux_ctl;
	/* write DP_SET_POWER=D0 */
	static const u8 aux_msg[] = {
		[0] = (DP_AUX_NATIVE_WRITE << 4) | ((DP_SET_POWER >> 16) & 0xf),
		[1] = (DP_SET_POWER >> 8) & 0xff,
		[2] = DP_SET_POWER & 0xff,
		[3] = 1 - 1,
		[4] = DP_SET_POWER_D0,
	};
	int i;

	BUILD_BUG_ON(sizeof(aux_msg) > 20);
	for (i = 0; i < sizeof(aux_msg); i += 4)
		intel_de_write(dev_priv,
			       psr_aux_data_reg(dev_priv, cpu_transcoder, i >> 2),
			       intel_dp_aux_pack(&aux_msg[i], sizeof(aux_msg) - i));

	aux_clock_divider = intel_dp->get_aux_clock_divider(intel_dp, 0);

	/* Start with bits set for DDI_AUX_CTL register */
	aux_ctl = intel_dp->get_aux_send_ctl(intel_dp, sizeof(aux_msg),
					     aux_clock_divider);

	/* Select only valid bits for SRD_AUX_CTL */
	aux_ctl &= EDP_PSR_AUX_CTL_TIME_OUT_MASK |
		EDP_PSR_AUX_CTL_MESSAGE_SIZE_MASK |
		EDP_PSR_AUX_CTL_PRECHARGE_2US_MASK |
		EDP_PSR_AUX_CTL_BIT_CLOCK_2X_MASK;

	intel_de_write(dev_priv, psr_aux_ctl_reg(dev_priv, cpu_transcoder),
		       aux_ctl);
}

static void intel_psr_enable_sink(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	u8 dpcd_val = DP_PSR_ENABLE;

	/* Enable ALPM at sink for psr2 */
	if (intel_dp->psr.psr2_enabled) {
		drm_dp_dpcd_writeb(&intel_dp->aux, DP_RECEIVER_ALPM_CONFIG,
				   DP_ALPM_ENABLE |
				   DP_ALPM_LOCK_ERROR_IRQ_HPD_ENABLE);

		dpcd_val |= DP_PSR_ENABLE_PSR2 | DP_PSR_IRQ_HPD_WITH_CRC_ERRORS;
	} else {
		if (intel_dp->psr.link_standby)
			dpcd_val |= DP_PSR_MAIN_LINK_ACTIVE;

		if (DISPLAY_VER(dev_priv) >= 8)
			dpcd_val |= DP_PSR_CRC_VERIFICATION;
	}

	if (intel_dp->psr.req_psr2_sdp_prior_scanline)
		dpcd_val |= DP_PSR_SU_REGION_SCANLINE_CAPTURE;

	drm_dp_dpcd_writeb(&intel_dp->aux, DP_PSR_EN_CFG, dpcd_val);

	drm_dp_dpcd_writeb(&intel_dp->aux, DP_SET_POWER, DP_SET_POWER_D0);
}

static u32 intel_psr1_get_tp_time(struct intel_dp *intel_dp)
{
	struct intel_connector *connector = intel_dp->attached_connector;
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	u32 val = 0;

	if (DISPLAY_VER(dev_priv) >= 11)
		val |= EDP_PSR_TP4_TIME_0us;

	if (dev_priv->params.psr_safest_params) {
		val |= EDP_PSR_TP1_TIME_2500us;
		val |= EDP_PSR_TP2_TP3_TIME_2500us;
		goto check_tp3_sel;
	}

	if (connector->panel.vbt.psr.tp1_wakeup_time_us == 0)
		val |= EDP_PSR_TP1_TIME_0us;
	else if (connector->panel.vbt.psr.tp1_wakeup_time_us <= 100)
		val |= EDP_PSR_TP1_TIME_100us;
	else if (connector->panel.vbt.psr.tp1_wakeup_time_us <= 500)
		val |= EDP_PSR_TP1_TIME_500us;
	else
		val |= EDP_PSR_TP1_TIME_2500us;

	if (connector->panel.vbt.psr.tp2_tp3_wakeup_time_us == 0)
		val |= EDP_PSR_TP2_TP3_TIME_0us;
	else if (connector->panel.vbt.psr.tp2_tp3_wakeup_time_us <= 100)
		val |= EDP_PSR_TP2_TP3_TIME_100us;
	else if (connector->panel.vbt.psr.tp2_tp3_wakeup_time_us <= 500)
		val |= EDP_PSR_TP2_TP3_TIME_500us;
	else
		val |= EDP_PSR_TP2_TP3_TIME_2500us;

	/*
	 * WA 0479: hsw,bdw
	 * "Do not skip both TP1 and TP2/TP3"
	 */
	if (DISPLAY_VER(dev_priv) < 9 &&
	    connector->panel.vbt.psr.tp1_wakeup_time_us == 0 &&
	    connector->panel.vbt.psr.tp2_tp3_wakeup_time_us == 0)
		val |= EDP_PSR_TP2_TP3_TIME_100us;

check_tp3_sel:
	if (intel_dp_source_supports_tps3(dev_priv) &&
	    drm_dp_tps3_supported(intel_dp->dpcd))
		val |= EDP_PSR_TP_TP1_TP3;
	else
		val |= EDP_PSR_TP_TP1_TP2;

	return val;
}

static u8 psr_compute_idle_frames(struct intel_dp *intel_dp)
{
	struct intel_connector *connector = intel_dp->attached_connector;
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	int idle_frames;

	/* Let's use 6 as the minimum to cover all known cases including the
	 * off-by-one issue that HW has in some cases.
	 */
	idle_frames = max(6, connector->panel.vbt.psr.idle_frames);
	idle_frames = max(idle_frames, intel_dp->psr.sink_sync_latency + 1);

	if (drm_WARN_ON(&dev_priv->drm, idle_frames > 0xf))
		idle_frames = 0xf;

	return idle_frames;
}

static void hsw_activate_psr1(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;
	u32 max_sleep_time = 0x1f;
	u32 val = EDP_PSR_ENABLE;

	val |= EDP_PSR_IDLE_FRAMES(psr_compute_idle_frames(intel_dp));

	val |= EDP_PSR_MAX_SLEEP_TIME(max_sleep_time);
	if (IS_HASWELL(dev_priv))
		val |= EDP_PSR_MIN_LINK_ENTRY_TIME_8_LINES;

	if (intel_dp->psr.link_standby)
		val |= EDP_PSR_LINK_STANDBY;

	val |= intel_psr1_get_tp_time(intel_dp);

	if (DISPLAY_VER(dev_priv) >= 8)
		val |= EDP_PSR_CRC_ENABLE;

	intel_de_rmw(dev_priv, psr_ctl_reg(dev_priv, cpu_transcoder),
		     ~EDP_PSR_RESTORE_PSR_ACTIVE_CTX_MASK, val);
}

static u32 intel_psr2_get_tp_time(struct intel_dp *intel_dp)
{
	struct intel_connector *connector = intel_dp->attached_connector;
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	u32 val = 0;

	if (dev_priv->params.psr_safest_params)
		return EDP_PSR2_TP2_TIME_2500us;

	if (connector->panel.vbt.psr.psr2_tp2_tp3_wakeup_time_us >= 0 &&
	    connector->panel.vbt.psr.psr2_tp2_tp3_wakeup_time_us <= 50)
		val |= EDP_PSR2_TP2_TIME_50us;
	else if (connector->panel.vbt.psr.psr2_tp2_tp3_wakeup_time_us <= 100)
		val |= EDP_PSR2_TP2_TIME_100us;
	else if (connector->panel.vbt.psr.psr2_tp2_tp3_wakeup_time_us <= 500)
		val |= EDP_PSR2_TP2_TIME_500us;
	else
		val |= EDP_PSR2_TP2_TIME_2500us;

	return val;
}

static int psr2_block_count_lines(struct intel_dp *intel_dp)
{
	return intel_dp->psr.io_wake_lines < 9 &&
		intel_dp->psr.fast_wake_lines < 9 ? 8 : 12;
}

static int psr2_block_count(struct intel_dp *intel_dp)
{
	return psr2_block_count_lines(intel_dp) / 4;
}

static void hsw_activate_psr2(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;
	u32 val = EDP_PSR2_ENABLE;

	val |= EDP_PSR2_IDLE_FRAMES(psr_compute_idle_frames(intel_dp));

	if (DISPLAY_VER(dev_priv) <= 13 && !IS_ALDERLAKE_P(dev_priv))
		val |= EDP_SU_TRACK_ENABLE;

	if (DISPLAY_VER(dev_priv) >= 10 && DISPLAY_VER(dev_priv) <= 12)
		val |= EDP_Y_COORDINATE_ENABLE;

	val |= EDP_PSR2_FRAME_BEFORE_SU(max_t(u8, intel_dp->psr.sink_sync_latency + 1, 2));
	val |= intel_psr2_get_tp_time(intel_dp);

	if (DISPLAY_VER(dev_priv) >= 12) {
		if (psr2_block_count(intel_dp) > 2)
			val |= TGL_EDP_PSR2_BLOCK_COUNT_NUM_3;
		else
			val |= TGL_EDP_PSR2_BLOCK_COUNT_NUM_2;
	}

	/* Wa_22012278275:adl-p */
	if (IS_ALDERLAKE_P(dev_priv) && IS_DISPLAY_STEP(dev_priv, STEP_A0, STEP_E0)) {
		static const u8 map[] = {
			2, /* 5 lines */
			1, /* 6 lines */
			0, /* 7 lines */
			3, /* 8 lines */
			6, /* 9 lines */
			5, /* 10 lines */
			4, /* 11 lines */
			7, /* 12 lines */
		};
		/*
		 * Still using the default IO_BUFFER_WAKE and FAST_WAKE, see
		 * comments bellow for more information
		 */
		int tmp;

		tmp = map[intel_dp->psr.io_wake_lines - TGL_EDP_PSR2_IO_BUFFER_WAKE_MIN_LINES];
		val |= TGL_EDP_PSR2_IO_BUFFER_WAKE(tmp + TGL_EDP_PSR2_IO_BUFFER_WAKE_MIN_LINES);

		tmp = map[intel_dp->psr.fast_wake_lines - TGL_EDP_PSR2_FAST_WAKE_MIN_LINES];
		val |= TGL_EDP_PSR2_FAST_WAKE(tmp + TGL_EDP_PSR2_FAST_WAKE_MIN_LINES);
	} else if (DISPLAY_VER(dev_priv) >= 12) {
		val |= TGL_EDP_PSR2_IO_BUFFER_WAKE(intel_dp->psr.io_wake_lines);
		val |= TGL_EDP_PSR2_FAST_WAKE(intel_dp->psr.fast_wake_lines);
	} else if (DISPLAY_VER(dev_priv) >= 9) {
		val |= EDP_PSR2_IO_BUFFER_WAKE(intel_dp->psr.io_wake_lines);
		val |= EDP_PSR2_FAST_WAKE(intel_dp->psr.fast_wake_lines);
	}

	if (intel_dp->psr.req_psr2_sdp_prior_scanline)
		val |= EDP_PSR2_SU_SDP_SCANLINE;

	if (intel_dp->psr.psr2_sel_fetch_enabled) {
		u32 tmp;

		tmp = intel_de_read(dev_priv, PSR2_MAN_TRK_CTL(cpu_transcoder));
		drm_WARN_ON(&dev_priv->drm, !(tmp & PSR2_MAN_TRK_CTL_ENABLE));
	} else if (HAS_PSR2_SEL_FETCH(dev_priv)) {
		intel_de_write(dev_priv, PSR2_MAN_TRK_CTL(cpu_transcoder), 0);
	}

	/*
	 * PSR2 HW is incorrectly using EDP_PSR_TP1_TP3_SEL and BSpec is
	 * recommending keep this bit unset while PSR2 is enabled.
	 */
	intel_de_write(dev_priv, psr_ctl_reg(dev_priv, cpu_transcoder), 0);

	intel_de_write(dev_priv, EDP_PSR2_CTL(cpu_transcoder), val);
}

static bool
transcoder_has_psr2(struct drm_i915_private *dev_priv, enum transcoder cpu_transcoder)
{
	if (IS_ALDERLAKE_P(dev_priv) || DISPLAY_VER(dev_priv) >= 14)
		return cpu_transcoder == TRANSCODER_A || cpu_transcoder == TRANSCODER_B;
	else if (DISPLAY_VER(dev_priv) >= 12)
		return cpu_transcoder == TRANSCODER_A;
	else if (DISPLAY_VER(dev_priv) >= 9)
		return cpu_transcoder == TRANSCODER_EDP;
	else
		return false;
}

static u32 intel_get_frame_time_us(const struct intel_crtc_state *cstate)
{
	if (!cstate || !cstate->hw.active)
		return 0;

	return DIV_ROUND_UP(1000 * 1000,
			    drm_mode_vrefresh(&cstate->hw.adjusted_mode));
}

static void psr2_program_idle_frames(struct intel_dp *intel_dp,
				     u32 idle_frames)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;

	intel_de_rmw(dev_priv, EDP_PSR2_CTL(cpu_transcoder),
		     EDP_PSR2_IDLE_FRAMES_MASK,
		     EDP_PSR2_IDLE_FRAMES(idle_frames));
}

static void tgl_psr2_enable_dc3co(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	psr2_program_idle_frames(intel_dp, 0);
	intel_display_power_set_target_dc_state(dev_priv, DC_STATE_EN_DC3CO);
}

static void tgl_psr2_disable_dc3co(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	intel_display_power_set_target_dc_state(dev_priv, DC_STATE_EN_UPTO_DC6);
	psr2_program_idle_frames(intel_dp, psr_compute_idle_frames(intel_dp));
}

static void tgl_dc3co_disable_work(struct work_struct *work)
{
	struct intel_dp *intel_dp =
		container_of(work, typeof(*intel_dp), psr.dc3co_work.work);

	mutex_lock(&intel_dp->psr.lock);
	/* If delayed work is pending, it is not idle */
	if (delayed_work_pending(&intel_dp->psr.dc3co_work))
		goto unlock;

	tgl_psr2_disable_dc3co(intel_dp);
unlock:
	mutex_unlock(&intel_dp->psr.lock);
}

static void tgl_disallow_dc3co_on_psr2_exit(struct intel_dp *intel_dp)
{
	if (!intel_dp->psr.dc3co_exitline)
		return;

	cancel_delayed_work(&intel_dp->psr.dc3co_work);
	/* Before PSR2 exit disallow dc3co*/
	tgl_psr2_disable_dc3co(intel_dp);
}

static bool
dc3co_is_pipe_port_compatible(struct intel_dp *intel_dp,
			      struct intel_crtc_state *crtc_state)
{
	struct intel_digital_port *dig_port = dp_to_dig_port(intel_dp);
	enum pipe pipe = to_intel_crtc(crtc_state->uapi.crtc)->pipe;
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum port port = dig_port->base.port;

	if (IS_ALDERLAKE_P(dev_priv) || DISPLAY_VER(dev_priv) >= 14)
		return pipe <= PIPE_B && port <= PORT_B;
	else
		return pipe == PIPE_A && port == PORT_A;
}

static void
tgl_dc3co_exitline_compute_config(struct intel_dp *intel_dp,
				  struct intel_crtc_state *crtc_state)
{
	const u32 crtc_vdisplay = crtc_state->uapi.adjusted_mode.crtc_vdisplay;
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	struct i915_power_domains *power_domains = &dev_priv->display.power.domains;
	u32 exit_scanlines;

	/*
	 * FIXME: Due to the changed sequence of activating/deactivating DC3CO,
	 * disable DC3CO until the changed dc3co activating/deactivating sequence
	 * is applied. B.Specs:49196
	 */
	return;

	/*
	 * DMC's DC3CO exit mechanism has an issue with Selective Fecth
	 * TODO: when the issue is addressed, this restriction should be removed.
	 */
	if (crtc_state->enable_psr2_sel_fetch)
		return;

	if (!(power_domains->allowed_dc_mask & DC_STATE_EN_DC3CO))
		return;

	if (!dc3co_is_pipe_port_compatible(intel_dp, crtc_state))
		return;

	/* Wa_16011303918:adl-p */
	if (IS_ALDERLAKE_P(dev_priv) && IS_DISPLAY_STEP(dev_priv, STEP_A0, STEP_B0))
		return;

	/*
	 * DC3CO Exit time 200us B.Spec 49196
	 * PSR2 transcoder Early Exit scanlines = ROUNDUP(200 / line time) + 1
	 */
	exit_scanlines =
		intel_usecs_to_scanlines(&crtc_state->uapi.adjusted_mode, 200) + 1;

	if (drm_WARN_ON(&dev_priv->drm, exit_scanlines > crtc_vdisplay))
		return;

	crtc_state->dc3co_exitline = crtc_vdisplay - exit_scanlines;
}

static bool intel_psr2_sel_fetch_config_valid(struct intel_dp *intel_dp,
					      struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	if (!dev_priv->params.enable_psr2_sel_fetch &&
	    intel_dp->psr.debug != I915_PSR_DEBUG_ENABLE_SEL_FETCH) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR2 sel fetch not enabled, disabled by parameter\n");
		return false;
	}

	if (crtc_state->uapi.async_flip) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR2 sel fetch not enabled, async flip enabled\n");
		return false;
	}

	return crtc_state->enable_psr2_sel_fetch = true;
}

static bool psr2_granularity_check(struct intel_dp *intel_dp,
				   struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	const struct drm_dsc_config *vdsc_cfg = &crtc_state->dsc.config;
	const int crtc_hdisplay = crtc_state->hw.adjusted_mode.crtc_hdisplay;
	const int crtc_vdisplay = crtc_state->hw.adjusted_mode.crtc_vdisplay;
	u16 y_granularity = 0;

	/* PSR2 HW only send full lines so we only need to validate the width */
	if (crtc_hdisplay % intel_dp->psr.su_w_granularity)
		return false;

	if (crtc_vdisplay % intel_dp->psr.su_y_granularity)
		return false;

	/* HW tracking is only aligned to 4 lines */
	if (!crtc_state->enable_psr2_sel_fetch)
		return intel_dp->psr.su_y_granularity == 4;

	/*
	 * adl_p and mtl platforms have 1 line granularity.
	 * For other platforms with SW tracking we can adjust the y coordinates
	 * to match sink requirement if multiple of 4.
	 */
	if (IS_ALDERLAKE_P(dev_priv) || DISPLAY_VER(dev_priv) >= 14)
		y_granularity = intel_dp->psr.su_y_granularity;
	else if (intel_dp->psr.su_y_granularity <= 2)
		y_granularity = 4;
	else if ((intel_dp->psr.su_y_granularity % 4) == 0)
		y_granularity = intel_dp->psr.su_y_granularity;

	if (y_granularity == 0 || crtc_vdisplay % y_granularity)
		return false;

	if (crtc_state->dsc.compression_enable &&
	    vdsc_cfg->slice_height % y_granularity)
		return false;

	crtc_state->su_y_granularity = y_granularity;
	return true;
}

static bool _compute_psr2_sdp_prior_scanline_indication(struct intel_dp *intel_dp,
							struct intel_crtc_state *crtc_state)
{
	const struct drm_display_mode *adjusted_mode = &crtc_state->uapi.adjusted_mode;
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	u32 hblank_total, hblank_ns, req_ns;

	hblank_total = adjusted_mode->crtc_hblank_end - adjusted_mode->crtc_hblank_start;
	hblank_ns = div_u64(1000000ULL * hblank_total, adjusted_mode->crtc_clock);

	/* From spec: ((60 / number of lanes) + 11) * 1000 / symbol clock frequency MHz */
	req_ns = ((60 / crtc_state->lane_count) + 11) * 1000 / (crtc_state->port_clock / 1000);

	if ((hblank_ns - req_ns) > 100)
		return true;

	/* Not supported <13 / Wa_22012279113:adl-p */
	if (DISPLAY_VER(dev_priv) <= 13 || intel_dp->edp_dpcd[0] < DP_EDP_14b)
		return false;

	crtc_state->req_psr2_sdp_prior_scanline = true;
	return true;
}

static bool _compute_psr2_wake_times(struct intel_dp *intel_dp,
				     struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);
	int io_wake_lines, io_wake_time, fast_wake_lines, fast_wake_time;
	u8 max_wake_lines;

	if (DISPLAY_VER(i915) >= 12) {
		io_wake_time = 42;
		/*
		 * According to Bspec it's 42us, but based on testing
		 * it is not enough -> use 45 us.
		 */
		fast_wake_time = 45;
		max_wake_lines = 12;
	} else {
		io_wake_time = 50;
		fast_wake_time = 32;
		max_wake_lines = 8;
	}

	io_wake_lines = intel_usecs_to_scanlines(
		&crtc_state->hw.adjusted_mode, io_wake_time);
	fast_wake_lines = intel_usecs_to_scanlines(
		&crtc_state->hw.adjusted_mode, fast_wake_time);

	if (io_wake_lines > max_wake_lines ||
	    fast_wake_lines > max_wake_lines)
		return false;

	if (i915->params.psr_safest_params)
		io_wake_lines = fast_wake_lines = max_wake_lines;

	/* According to Bspec lower limit should be set as 7 lines. */
	intel_dp->psr.io_wake_lines = max(io_wake_lines, 7);
	intel_dp->psr.fast_wake_lines = max(fast_wake_lines, 7);

	return true;
}

static bool intel_psr2_config_valid(struct intel_dp *intel_dp,
				    struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	int crtc_hdisplay = crtc_state->hw.adjusted_mode.crtc_hdisplay;
	int crtc_vdisplay = crtc_state->hw.adjusted_mode.crtc_vdisplay;
	int psr_max_h = 0, psr_max_v = 0, max_bpp = 0;

	if (!intel_dp->psr.sink_psr2_support)
		return false;

	/* JSL and EHL only supports eDP 1.3 */
	if (IS_JASPERLAKE(dev_priv) || IS_ELKHARTLAKE(dev_priv)) {
		drm_dbg_kms(&dev_priv->drm, "PSR2 not supported by phy\n");
		return false;
	}

	/* Wa_16011181250 */
	if (IS_ROCKETLAKE(dev_priv) || IS_ALDERLAKE_S(dev_priv) ||
	    IS_DG2(dev_priv)) {
		drm_dbg_kms(&dev_priv->drm, "PSR2 is defeatured for this platform\n");
		return false;
	}

	if (IS_ALDERLAKE_P(dev_priv) && IS_DISPLAY_STEP(dev_priv, STEP_A0, STEP_B0)) {
		drm_dbg_kms(&dev_priv->drm, "PSR2 not completely functional in this stepping\n");
		return false;
	}

	if (!transcoder_has_psr2(dev_priv, crtc_state->cpu_transcoder)) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR2 not supported in transcoder %s\n",
			    transcoder_name(crtc_state->cpu_transcoder));
		return false;
	}

	if (!psr2_global_enabled(intel_dp)) {
		drm_dbg_kms(&dev_priv->drm, "PSR2 disabled by flag\n");
		return false;
	}

	/*
	 * DSC and PSR2 cannot be enabled simultaneously. If a requested
	 * resolution requires DSC to be enabled, priority is given to DSC
	 * over PSR2.
	 */
	if (crtc_state->dsc.compression_enable &&
	    (DISPLAY_VER(dev_priv) <= 13 && !IS_ALDERLAKE_P(dev_priv))) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR2 cannot be enabled since DSC is enabled\n");
		return false;
	}

	if (crtc_state->crc_enabled) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR2 not enabled because it would inhibit pipe CRC calculation\n");
		return false;
	}

	if (DISPLAY_VER(dev_priv) >= 12) {
		psr_max_h = 5120;
		psr_max_v = 3200;
		max_bpp = 30;
	} else if (DISPLAY_VER(dev_priv) >= 10) {
		psr_max_h = 4096;
		psr_max_v = 2304;
		max_bpp = 24;
	} else if (DISPLAY_VER(dev_priv) == 9) {
		psr_max_h = 3640;
		psr_max_v = 2304;
		max_bpp = 24;
	}

	if (crtc_state->pipe_bpp > max_bpp) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR2 not enabled, pipe bpp %d > max supported %d\n",
			    crtc_state->pipe_bpp, max_bpp);
		return false;
	}

	/* Wa_16011303918:adl-p */
	if (crtc_state->vrr.enable &&
	    IS_ALDERLAKE_P(dev_priv) && IS_DISPLAY_STEP(dev_priv, STEP_A0, STEP_B0)) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR2 not enabled, not compatible with HW stepping + VRR\n");
		return false;
	}

	if (!_compute_psr2_sdp_prior_scanline_indication(intel_dp, crtc_state)) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR2 not enabled, PSR2 SDP indication do not fit in hblank\n");
		return false;
	}

	if (!_compute_psr2_wake_times(intel_dp, crtc_state)) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR2 not enabled, Unable to use long enough wake times\n");
		return false;
	}

	/* Vblank >= PSR2_CTL Block Count Number maximum line count */
	if (crtc_state->hw.adjusted_mode.crtc_vblank_end -
	    crtc_state->hw.adjusted_mode.crtc_vblank_start <
	    psr2_block_count_lines(intel_dp)) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR2 not enabled, too short vblank time\n");
		return false;
	}

	if (HAS_PSR2_SEL_FETCH(dev_priv)) {
		if (!intel_psr2_sel_fetch_config_valid(intel_dp, crtc_state) &&
		    !HAS_PSR_HW_TRACKING(dev_priv)) {
			drm_dbg_kms(&dev_priv->drm,
				    "PSR2 not enabled, selective fetch not valid and no HW tracking available\n");
			return false;
		}
	}

	if (!psr2_granularity_check(intel_dp, crtc_state)) {
		drm_dbg_kms(&dev_priv->drm, "PSR2 not enabled, SU granularity not compatible\n");
		goto unsupported;
	}

	if (!crtc_state->enable_psr2_sel_fetch &&
	    (crtc_hdisplay > psr_max_h || crtc_vdisplay > psr_max_v)) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR2 not enabled, resolution %dx%d > max supported %dx%d\n",
			    crtc_hdisplay, crtc_vdisplay,
			    psr_max_h, psr_max_v);
		goto unsupported;
	}

	tgl_dc3co_exitline_compute_config(intel_dp, crtc_state);
	return true;

unsupported:
	crtc_state->enable_psr2_sel_fetch = false;
	return false;
}

void intel_psr_compute_config(struct intel_dp *intel_dp,
			      struct intel_crtc_state *crtc_state,
			      struct drm_connector_state *conn_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	const struct drm_display_mode *adjusted_mode =
		&crtc_state->hw.adjusted_mode;
	int psr_setup_time;

	/*
	 * Current PSR panels don't work reliably with VRR enabled
	 * So if VRR is enabled, do not enable PSR.
	 */
	if (crtc_state->vrr.enable)
		return;

	if (!CAN_PSR(intel_dp))
		return;

	if (!psr_global_enabled(intel_dp)) {
		drm_dbg_kms(&dev_priv->drm, "PSR disabled by flag\n");
		return;
	}

	if (intel_dp->psr.sink_not_reliable) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR sink implementation is not reliable\n");
		return;
	}

	if (adjusted_mode->flags & DRM_MODE_FLAG_INTERLACE) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR condition failed: Interlaced mode enabled\n");
		return;
	}

	psr_setup_time = drm_dp_psr_setup_time(intel_dp->psr_dpcd);
	if (psr_setup_time < 0) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR condition failed: Invalid PSR setup time (0x%02x)\n",
			    intel_dp->psr_dpcd[1]);
		return;
	}

	if (intel_usecs_to_scanlines(adjusted_mode, psr_setup_time) >
	    adjusted_mode->crtc_vtotal - adjusted_mode->crtc_vdisplay - 1) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR condition failed: PSR setup time (%d us) too long\n",
			    psr_setup_time);
		return;
	}

	crtc_state->has_psr = true;
	crtc_state->has_psr2 = intel_psr2_config_valid(intel_dp, crtc_state);

	crtc_state->infoframes.enable |= intel_hdmi_infoframe_enable(DP_SDP_VSC);
	intel_dp_compute_psr_vsc_sdp(intel_dp, crtc_state, conn_state,
				     &crtc_state->psr_vsc);
}

void intel_psr_get_config(struct intel_encoder *encoder,
			  struct intel_crtc_state *pipe_config)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_digital_port *dig_port = enc_to_dig_port(encoder);
	enum transcoder cpu_transcoder = pipe_config->cpu_transcoder;
	struct intel_dp *intel_dp;
	u32 val;

	if (!dig_port)
		return;

	intel_dp = &dig_port->dp;
	if (!CAN_PSR(intel_dp))
		return;

	mutex_lock(&intel_dp->psr.lock);
	if (!intel_dp->psr.enabled)
		goto unlock;

	/*
	 * Not possible to read EDP_PSR/PSR2_CTL registers as it is
	 * enabled/disabled because of frontbuffer tracking and others.
	 */
	pipe_config->has_psr = true;
	pipe_config->has_psr2 = intel_dp->psr.psr2_enabled;
	pipe_config->infoframes.enable |= intel_hdmi_infoframe_enable(DP_SDP_VSC);

	if (!intel_dp->psr.psr2_enabled)
		goto unlock;

	if (HAS_PSR2_SEL_FETCH(dev_priv)) {
		val = intel_de_read(dev_priv, PSR2_MAN_TRK_CTL(cpu_transcoder));
		if (val & PSR2_MAN_TRK_CTL_ENABLE)
			pipe_config->enable_psr2_sel_fetch = true;
	}

	if (DISPLAY_VER(dev_priv) >= 12) {
		val = intel_de_read(dev_priv, TRANS_EXITLINE(cpu_transcoder));
		pipe_config->dc3co_exitline = REG_FIELD_GET(EXITLINE_MASK, val);
	}
unlock:
	mutex_unlock(&intel_dp->psr.lock);
}

static void intel_psr_activate(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;

	drm_WARN_ON(&dev_priv->drm,
		    transcoder_has_psr2(dev_priv, cpu_transcoder) &&
		    intel_de_read(dev_priv, EDP_PSR2_CTL(cpu_transcoder)) & EDP_PSR2_ENABLE);

	drm_WARN_ON(&dev_priv->drm,
		    intel_de_read(dev_priv, psr_ctl_reg(dev_priv, cpu_transcoder)) & EDP_PSR_ENABLE);

	drm_WARN_ON(&dev_priv->drm, intel_dp->psr.active);

	lockdep_assert_held(&intel_dp->psr.lock);

	/* psr1 and psr2 are mutually exclusive.*/
	if (intel_dp->psr.psr2_enabled)
		hsw_activate_psr2(intel_dp);
	else
		hsw_activate_psr1(intel_dp);

	intel_dp->psr.active = true;
}

static u32 wa_16013835468_bit_get(struct intel_dp *intel_dp)
{
	switch (intel_dp->psr.pipe) {
	case PIPE_A:
		return LATENCY_REPORTING_REMOVED_PIPE_A;
	case PIPE_B:
		return LATENCY_REPORTING_REMOVED_PIPE_B;
	case PIPE_C:
		return LATENCY_REPORTING_REMOVED_PIPE_C;
	case PIPE_D:
		return LATENCY_REPORTING_REMOVED_PIPE_D;
	default:
		MISSING_CASE(intel_dp->psr.pipe);
		return 0;
	}
}

/*
 * Wa_16013835468
 * Wa_14015648006
 */
static void wm_optimization_wa(struct intel_dp *intel_dp,
			       const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	bool set_wa_bit = false;

	/* Wa_14015648006 */
	if (IS_MTL_DISPLAY_STEP(dev_priv, STEP_A0, STEP_B0) ||
	    IS_DISPLAY_VER(dev_priv, 11, 13))
		set_wa_bit |= crtc_state->wm_level_disabled;

	/* Wa_16013835468 */
	if (DISPLAY_VER(dev_priv) == 12)
		set_wa_bit |= crtc_state->hw.adjusted_mode.crtc_vblank_start !=
			crtc_state->hw.adjusted_mode.crtc_vdisplay;

	if (set_wa_bit)
		intel_de_rmw(dev_priv, GEN8_CHICKEN_DCPR_1,
			     0, wa_16013835468_bit_get(intel_dp));
	else
		intel_de_rmw(dev_priv, GEN8_CHICKEN_DCPR_1,
			     wa_16013835468_bit_get(intel_dp), 0);
}

static void intel_psr_enable_source(struct intel_dp *intel_dp,
				    const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;
	u32 mask;

	/*
	 * Only HSW and BDW have PSR AUX registers that need to be setup.
	 * SKL+ use hardcoded values PSR AUX transactions
	 */
	if (DISPLAY_VER(dev_priv) < 9)
		hsw_psr_setup_aux(intel_dp);

	/*
	 * Per Spec: Avoid continuous PSR exit by masking MEMUP and HPD also
	 * mask LPSP to avoid dependency on other drivers that might block
	 * runtime_pm besides preventing  other hw tracking issues now we
	 * can rely on frontbuffer tracking.
	 */
	mask = EDP_PSR_DEBUG_MASK_MEMUP |
	       EDP_PSR_DEBUG_MASK_HPD |
	       EDP_PSR_DEBUG_MASK_LPSP |
	       EDP_PSR_DEBUG_MASK_MAX_SLEEP;

	/*
	 * No separate pipe reg write mask on hsw/bdw, so have to unmask all
	 * registers in order to keep the CURSURFLIVE tricks working :(
	 */
	if (IS_DISPLAY_VER(dev_priv, 9, 10))
		mask |= EDP_PSR_DEBUG_MASK_DISP_REG_WRITE;

	/* allow PSR with sprite enabled */
	if (IS_HASWELL(dev_priv))
		mask |= EDP_PSR_DEBUG_MASK_SPRITE_ENABLE;

	intel_de_write(dev_priv, psr_debug_reg(dev_priv, cpu_transcoder), mask);

	psr_irq_control(intel_dp);

	/*
	 * TODO: if future platforms supports DC3CO in more than one
	 * transcoder, EXITLINE will need to be unset when disabling PSR
	 */
	if (intel_dp->psr.dc3co_exitline)
		intel_de_rmw(dev_priv, TRANS_EXITLINE(cpu_transcoder), EXITLINE_MASK,
			     intel_dp->psr.dc3co_exitline << EXITLINE_SHIFT | EXITLINE_ENABLE);

	if (HAS_PSR_HW_TRACKING(dev_priv) && HAS_PSR2_SEL_FETCH(dev_priv))
		intel_de_rmw(dev_priv, CHICKEN_PAR1_1, IGNORE_PSR2_HW_TRACKING,
			     intel_dp->psr.psr2_sel_fetch_enabled ?
			     IGNORE_PSR2_HW_TRACKING : 0);

	/*
	 * Wa_16013835468
	 * Wa_14015648006
	 */
	wm_optimization_wa(intel_dp, crtc_state);

	if (intel_dp->psr.psr2_enabled) {
		if (DISPLAY_VER(dev_priv) == 9)
			intel_de_rmw(dev_priv, CHICKEN_TRANS(cpu_transcoder), 0,
				     PSR2_VSC_ENABLE_PROG_HEADER |
				     PSR2_ADD_VERTICAL_LINE_COUNT);

		/*
		 * Wa_16014451276:adlp,mtl[a0,b0]
		 * All supported adlp panels have 1-based X granularity, this may
		 * cause issues if non-supported panels are used.
		 */
		if (IS_MTL_DISPLAY_STEP(dev_priv, STEP_A0, STEP_B0))
			intel_de_rmw(dev_priv, MTL_CHICKEN_TRANS(cpu_transcoder), 0,
				     ADLP_1_BASED_X_GRANULARITY);
		else if (IS_ALDERLAKE_P(dev_priv))
			intel_de_rmw(dev_priv, CHICKEN_TRANS(cpu_transcoder), 0,
				     ADLP_1_BASED_X_GRANULARITY);

		/* Wa_16012604467:adlp,mtl[a0,b0] */
		if (IS_MTL_DISPLAY_STEP(dev_priv, STEP_A0, STEP_B0))
			intel_de_rmw(dev_priv,
				     MTL_CLKGATE_DIS_TRANS(cpu_transcoder), 0,
				     MTL_CLKGATE_DIS_TRANS_DMASC_GATING_DIS);
		else if (IS_ALDERLAKE_P(dev_priv))
			intel_de_rmw(dev_priv, CLKGATE_DIS_MISC, 0,
				     CLKGATE_DIS_MISC_DMASC_GATING_DIS);
	}
}

static bool psr_interrupt_error_check(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;
	u32 val;

	/*
	 * If a PSR error happened and the driver is reloaded, the EDP_PSR_IIR
	 * will still keep the error set even after the reset done in the
	 * irq_preinstall and irq_uninstall hooks.
	 * And enabling in this situation cause the screen to freeze in the
	 * first time that PSR HW tries to activate so lets keep PSR disabled
	 * to avoid any rendering problems.
	 */
	val = intel_de_read(dev_priv, psr_iir_reg(dev_priv, cpu_transcoder));
	val &= psr_irq_psr_error_bit_get(intel_dp);
	if (val) {
		intel_dp->psr.sink_not_reliable = true;
		drm_dbg_kms(&dev_priv->drm,
			    "PSR interruption error set, not enabling PSR\n");
		return false;
	}

	return true;
}

static void intel_psr_enable_locked(struct intel_dp *intel_dp,
				    const struct intel_crtc_state *crtc_state)
{
	struct intel_digital_port *dig_port = dp_to_dig_port(intel_dp);
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum phy phy = intel_port_to_phy(dev_priv, dig_port->base.port);
	struct intel_encoder *encoder = &dig_port->base;
	u32 val;

	drm_WARN_ON(&dev_priv->drm, intel_dp->psr.enabled);

	intel_dp->psr.psr2_enabled = crtc_state->has_psr2;
	intel_dp->psr.busy_frontbuffer_bits = 0;
	intel_dp->psr.pipe = to_intel_crtc(crtc_state->uapi.crtc)->pipe;
	intel_dp->psr.transcoder = crtc_state->cpu_transcoder;
	/* DC5/DC6 requires at least 6 idle frames */
	val = usecs_to_jiffies(intel_get_frame_time_us(crtc_state) * 6);
	intel_dp->psr.dc3co_exit_delay = val;
	intel_dp->psr.dc3co_exitline = crtc_state->dc3co_exitline;
	intel_dp->psr.psr2_sel_fetch_enabled = crtc_state->enable_psr2_sel_fetch;
	intel_dp->psr.psr2_sel_fetch_cff_enabled = false;
	intel_dp->psr.req_psr2_sdp_prior_scanline =
		crtc_state->req_psr2_sdp_prior_scanline;

	if (!psr_interrupt_error_check(intel_dp))
		return;

	drm_dbg_kms(&dev_priv->drm, "Enabling PSR%s\n",
		    intel_dp->psr.psr2_enabled ? "2" : "1");
	intel_write_dp_vsc_sdp(encoder, crtc_state, &crtc_state->psr_vsc);
	intel_snps_phy_update_psr_power_state(dev_priv, phy, true);
	intel_psr_enable_sink(intel_dp);
	intel_psr_enable_source(intel_dp, crtc_state);
	intel_dp->psr.enabled = true;
	intel_dp->psr.paused = false;

	intel_psr_activate(intel_dp);
}

static void intel_psr_exit(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;
	u32 val;

	if (!intel_dp->psr.active) {
		if (transcoder_has_psr2(dev_priv, cpu_transcoder)) {
			val = intel_de_read(dev_priv, EDP_PSR2_CTL(cpu_transcoder));
			drm_WARN_ON(&dev_priv->drm, val & EDP_PSR2_ENABLE);
		}

		val = intel_de_read(dev_priv, psr_ctl_reg(dev_priv, cpu_transcoder));
		drm_WARN_ON(&dev_priv->drm, val & EDP_PSR_ENABLE);

		return;
	}

	if (intel_dp->psr.psr2_enabled) {
		tgl_disallow_dc3co_on_psr2_exit(intel_dp);

		val = intel_de_rmw(dev_priv, EDP_PSR2_CTL(cpu_transcoder),
				   EDP_PSR2_ENABLE, 0);

		drm_WARN_ON(&dev_priv->drm, !(val & EDP_PSR2_ENABLE));
	} else {
		val = intel_de_rmw(dev_priv, psr_ctl_reg(dev_priv, cpu_transcoder),
				   EDP_PSR_ENABLE, 0);

		drm_WARN_ON(&dev_priv->drm, !(val & EDP_PSR_ENABLE));
	}
	intel_dp->psr.active = false;
}

static void intel_psr_wait_exit_locked(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;
	i915_reg_t psr_status;
	u32 psr_status_mask;

	if (intel_dp->psr.psr2_enabled) {
		psr_status = EDP_PSR2_STATUS(cpu_transcoder);
		psr_status_mask = EDP_PSR2_STATUS_STATE_MASK;
	} else {
		psr_status = psr_status_reg(dev_priv, cpu_transcoder);
		psr_status_mask = EDP_PSR_STATUS_STATE_MASK;
	}

	/* Wait till PSR is idle */
	if (intel_de_wait_for_clear(dev_priv, psr_status,
				    psr_status_mask, 2000))
		drm_err(&dev_priv->drm, "Timed out waiting PSR idle state\n");
}

static void intel_psr_disable_locked(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;
	enum phy phy = intel_port_to_phy(dev_priv,
					 dp_to_dig_port(intel_dp)->base.port);

	lockdep_assert_held(&intel_dp->psr.lock);

	if (!intel_dp->psr.enabled)
		return;

	drm_dbg_kms(&dev_priv->drm, "Disabling PSR%s\n",
		    intel_dp->psr.psr2_enabled ? "2" : "1");

	intel_psr_exit(intel_dp);
	intel_psr_wait_exit_locked(intel_dp);

	/*
	 * Wa_16013835468
	 * Wa_14015648006
	 */
	if (DISPLAY_VER(dev_priv) >= 11)
		intel_de_rmw(dev_priv, GEN8_CHICKEN_DCPR_1,
			     wa_16013835468_bit_get(intel_dp), 0);

	if (intel_dp->psr.psr2_enabled) {
		/* Wa_16012604467:adlp,mtl[a0,b0] */
		if (IS_MTL_DISPLAY_STEP(dev_priv, STEP_A0, STEP_B0))
			intel_de_rmw(dev_priv,
				     MTL_CLKGATE_DIS_TRANS(cpu_transcoder),
				     MTL_CLKGATE_DIS_TRANS_DMASC_GATING_DIS, 0);
		else if (IS_ALDERLAKE_P(dev_priv))
			intel_de_rmw(dev_priv, CLKGATE_DIS_MISC,
				     CLKGATE_DIS_MISC_DMASC_GATING_DIS, 0);
	}

	intel_snps_phy_update_psr_power_state(dev_priv, phy, false);

	/* Disable PSR on Sink */
	drm_dp_dpcd_writeb(&intel_dp->aux, DP_PSR_EN_CFG, 0);

	if (intel_dp->psr.psr2_enabled)
		drm_dp_dpcd_writeb(&intel_dp->aux, DP_RECEIVER_ALPM_CONFIG, 0);

	intel_dp->psr.enabled = false;
	intel_dp->psr.psr2_enabled = false;
	intel_dp->psr.psr2_sel_fetch_enabled = false;
	intel_dp->psr.psr2_sel_fetch_cff_enabled = false;
}

/**
 * intel_psr_disable - Disable PSR
 * @intel_dp: Intel DP
 * @old_crtc_state: old CRTC state
 *
 * This function needs to be called before disabling pipe.
 */
void intel_psr_disable(struct intel_dp *intel_dp,
		       const struct intel_crtc_state *old_crtc_state)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	if (!old_crtc_state->has_psr)
		return;

	if (drm_WARN_ON(&dev_priv->drm, !CAN_PSR(intel_dp)))
		return;

	mutex_lock(&intel_dp->psr.lock);

	intel_psr_disable_locked(intel_dp);

	mutex_unlock(&intel_dp->psr.lock);
	cancel_work_sync(&intel_dp->psr.work);
	cancel_delayed_work_sync(&intel_dp->psr.dc3co_work);
}

/**
 * intel_psr_pause - Pause PSR
 * @intel_dp: Intel DP
 *
 * This function need to be called after enabling psr.
 */
void intel_psr_pause(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	struct intel_psr *psr = &intel_dp->psr;

	if (!CAN_PSR(intel_dp))
		return;

	mutex_lock(&psr->lock);

	if (!psr->enabled) {
		mutex_unlock(&psr->lock);
		return;
	}

	/* If we ever hit this, we will need to add refcount to pause/resume */
	drm_WARN_ON(&dev_priv->drm, psr->paused);

	intel_psr_exit(intel_dp);
	intel_psr_wait_exit_locked(intel_dp);
	psr->paused = true;

	mutex_unlock(&psr->lock);

	cancel_work_sync(&psr->work);
	cancel_delayed_work_sync(&psr->dc3co_work);
}

/**
 * intel_psr_resume - Resume PSR
 * @intel_dp: Intel DP
 *
 * This function need to be called after pausing psr.
 */
void intel_psr_resume(struct intel_dp *intel_dp)
{
	struct intel_psr *psr = &intel_dp->psr;

	if (!CAN_PSR(intel_dp))
		return;

	mutex_lock(&psr->lock);

	if (!psr->paused)
		goto unlock;

	psr->paused = false;
	intel_psr_activate(intel_dp);

unlock:
	mutex_unlock(&psr->lock);
}

static u32 man_trk_ctl_enable_bit_get(struct drm_i915_private *dev_priv)
{
	return IS_ALDERLAKE_P(dev_priv) || DISPLAY_VER(dev_priv) >= 14 ? 0 :
		PSR2_MAN_TRK_CTL_ENABLE;
}

static u32 man_trk_ctl_single_full_frame_bit_get(struct drm_i915_private *dev_priv)
{
	return IS_ALDERLAKE_P(dev_priv) || DISPLAY_VER(dev_priv) >= 14 ?
	       ADLP_PSR2_MAN_TRK_CTL_SF_SINGLE_FULL_FRAME :
	       PSR2_MAN_TRK_CTL_SF_SINGLE_FULL_FRAME;
}

static u32 man_trk_ctl_partial_frame_bit_get(struct drm_i915_private *dev_priv)
{
	return IS_ALDERLAKE_P(dev_priv) || DISPLAY_VER(dev_priv) >= 14 ?
	       ADLP_PSR2_MAN_TRK_CTL_SF_PARTIAL_FRAME_UPDATE :
	       PSR2_MAN_TRK_CTL_SF_PARTIAL_FRAME_UPDATE;
}

static u32 man_trk_ctl_continuos_full_frame(struct drm_i915_private *dev_priv)
{
	return IS_ALDERLAKE_P(dev_priv) || DISPLAY_VER(dev_priv) >= 14 ?
	       ADLP_PSR2_MAN_TRK_CTL_SF_CONTINUOS_FULL_FRAME :
	       PSR2_MAN_TRK_CTL_SF_CONTINUOS_FULL_FRAME;
}

static void psr_force_hw_tracking_exit(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;

	if (intel_dp->psr.psr2_sel_fetch_enabled)
		intel_de_write(dev_priv,
			       PSR2_MAN_TRK_CTL(cpu_transcoder),
			       man_trk_ctl_enable_bit_get(dev_priv) |
			       man_trk_ctl_partial_frame_bit_get(dev_priv) |
			       man_trk_ctl_single_full_frame_bit_get(dev_priv) |
			       man_trk_ctl_continuos_full_frame(dev_priv));

	/*
	 * Display WA #0884: skl+
	 * This documented WA for bxt can be safely applied
	 * broadly so we can force HW tracking to exit PSR
	 * instead of disabling and re-enabling.
	 * Workaround tells us to write 0 to CUR_SURFLIVE_A,
	 * but it makes more sense write to the current active
	 * pipe.
	 *
	 * This workaround do not exist for platforms with display 10 or newer
	 * but testing proved that it works for up display 13, for newer
	 * than that testing will be needed.
	 */
	intel_de_write(dev_priv, CURSURFLIVE(intel_dp->psr.pipe), 0);
}

void intel_psr2_disable_plane_sel_fetch_arm(struct intel_plane *plane,
					    const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(plane->base.dev);
	enum pipe pipe = plane->pipe;

	if (!crtc_state->enable_psr2_sel_fetch)
		return;

	intel_de_write_fw(dev_priv, PLANE_SEL_FETCH_CTL(pipe, plane->id), 0);
}

void intel_psr2_program_plane_sel_fetch_arm(struct intel_plane *plane,
					    const struct intel_crtc_state *crtc_state,
					    const struct intel_plane_state *plane_state)
{
	struct drm_i915_private *i915 = to_i915(plane->base.dev);
	enum pipe pipe = plane->pipe;

	if (!crtc_state->enable_psr2_sel_fetch)
		return;

	if (plane->id == PLANE_CURSOR)
		intel_de_write_fw(i915, PLANE_SEL_FETCH_CTL(pipe, plane->id),
				  plane_state->ctl);
	else
		intel_de_write_fw(i915, PLANE_SEL_FETCH_CTL(pipe, plane->id),
				  PLANE_SEL_FETCH_CTL_ENABLE);
}

void intel_psr2_program_plane_sel_fetch_noarm(struct intel_plane *plane,
					      const struct intel_crtc_state *crtc_state,
					      const struct intel_plane_state *plane_state,
					      int color_plane)
{
	struct drm_i915_private *dev_priv = to_i915(plane->base.dev);
	enum pipe pipe = plane->pipe;
	const struct drm_rect *clip;
	u32 val;
	int x, y;

	if (!crtc_state->enable_psr2_sel_fetch)
		return;

	if (plane->id == PLANE_CURSOR)
		return;

	clip = &plane_state->psr2_sel_fetch_area;

	val = (clip->y1 + plane_state->uapi.dst.y1) << 16;
	val |= plane_state->uapi.dst.x1;
	intel_de_write_fw(dev_priv, PLANE_SEL_FETCH_POS(pipe, plane->id), val);

	x = plane_state->view.color_plane[color_plane].x;

	/*
	 * From Bspec: UV surface Start Y Position = half of Y plane Y
	 * start position.
	 */
	if (!color_plane)
		y = plane_state->view.color_plane[color_plane].y + clip->y1;
	else
		y = plane_state->view.color_plane[color_plane].y + clip->y1 / 2;

	val = y << 16 | x;

	intel_de_write_fw(dev_priv, PLANE_SEL_FETCH_OFFSET(pipe, plane->id),
			  val);

	/* Sizes are 0 based */
	val = (drm_rect_height(clip) - 1) << 16;
	val |= (drm_rect_width(&plane_state->uapi.src) >> 16) - 1;
	intel_de_write_fw(dev_priv, PLANE_SEL_FETCH_SIZE(pipe, plane->id), val);
}

void intel_psr2_program_trans_man_trk_ctl(const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	struct intel_encoder *encoder;

	if (!crtc_state->enable_psr2_sel_fetch)
		return;

	for_each_intel_encoder_mask_with_psr(&dev_priv->drm, encoder,
					     crtc_state->uapi.encoder_mask) {
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

		lockdep_assert_held(&intel_dp->psr.lock);
		if (intel_dp->psr.psr2_sel_fetch_cff_enabled)
			return;
		break;
	}

	intel_de_write(dev_priv, PSR2_MAN_TRK_CTL(cpu_transcoder),
		       crtc_state->psr2_man_track_ctl);
}

static void psr2_man_trk_ctl_calc(struct intel_crtc_state *crtc_state,
				  struct drm_rect *clip, bool full_update)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	u32 val = man_trk_ctl_enable_bit_get(dev_priv);

	/* SF partial frame enable has to be set even on full update */
	val |= man_trk_ctl_partial_frame_bit_get(dev_priv);

	if (full_update) {
		val |= man_trk_ctl_single_full_frame_bit_get(dev_priv);
		val |= man_trk_ctl_continuos_full_frame(dev_priv);
		goto exit;
	}

	if (clip->y1 == -1)
		goto exit;

	if (IS_ALDERLAKE_P(dev_priv) || DISPLAY_VER(dev_priv) >= 14) {
		val |= ADLP_PSR2_MAN_TRK_CTL_SU_REGION_START_ADDR(clip->y1);
		val |= ADLP_PSR2_MAN_TRK_CTL_SU_REGION_END_ADDR(clip->y2 - 1);
	} else {
		drm_WARN_ON(crtc_state->uapi.crtc->dev, clip->y1 % 4 || clip->y2 % 4);

		val |= PSR2_MAN_TRK_CTL_SU_REGION_START_ADDR(clip->y1 / 4 + 1);
		val |= PSR2_MAN_TRK_CTL_SU_REGION_END_ADDR(clip->y2 / 4 + 1);
	}
exit:
	crtc_state->psr2_man_track_ctl = val;
}

static void clip_area_update(struct drm_rect *overlap_damage_area,
			     struct drm_rect *damage_area,
			     struct drm_rect *pipe_src)
{
	if (!drm_rect_intersect(damage_area, pipe_src))
		return;

	if (overlap_damage_area->y1 == -1) {
		overlap_damage_area->y1 = damage_area->y1;
		overlap_damage_area->y2 = damage_area->y2;
		return;
	}

	if (damage_area->y1 < overlap_damage_area->y1)
		overlap_damage_area->y1 = damage_area->y1;

	if (damage_area->y2 > overlap_damage_area->y2)
		overlap_damage_area->y2 = damage_area->y2;
}

static void intel_psr2_sel_fetch_pipe_alignment(const struct intel_crtc_state *crtc_state,
						struct drm_rect *pipe_clip)
{
	struct drm_i915_private *dev_priv = to_i915(crtc_state->uapi.crtc->dev);
	const struct drm_dsc_config *vdsc_cfg = &crtc_state->dsc.config;
	u16 y_alignment;

	/* ADLP aligns the SU region to vdsc slice height in case dsc is enabled */
	if (crtc_state->dsc.compression_enable &&
	    (IS_ALDERLAKE_P(dev_priv) || DISPLAY_VER(dev_priv) >= 14))
		y_alignment = vdsc_cfg->slice_height;
	else
		y_alignment = crtc_state->su_y_granularity;

	pipe_clip->y1 -= pipe_clip->y1 % y_alignment;
	if (pipe_clip->y2 % y_alignment)
		pipe_clip->y2 = ((pipe_clip->y2 / y_alignment) + 1) * y_alignment;
}

/*
 * TODO: Not clear how to handle planes with negative position,
 * also planes are not updated if they have a negative X
 * position so for now doing a full update in this cases
 *
 * Plane scaling and rotation is not supported by selective fetch and both
 * properties can change without a modeset, so need to be check at every
 * atomic commit.
 */
static bool psr2_sel_fetch_plane_state_supported(const struct intel_plane_state *plane_state)
{
	if (plane_state->uapi.dst.y1 < 0 ||
	    plane_state->uapi.dst.x1 < 0 ||
	    plane_state->scaler_id >= 0 ||
	    plane_state->uapi.rotation != DRM_MODE_ROTATE_0)
		return false;

	return true;
}

/*
 * Check for pipe properties that is not supported by selective fetch.
 *
 * TODO: pipe scaling causes a modeset but skl_update_scaler_crtc() is executed
 * after intel_psr_compute_config(), so for now keeping PSR2 selective fetch
 * enabled and going to the full update path.
 */
static bool psr2_sel_fetch_pipe_state_supported(const struct intel_crtc_state *crtc_state)
{
	if (crtc_state->scaler_state.scaler_id >= 0)
		return false;

	return true;
}

int intel_psr2_sel_fetch_update(struct intel_atomic_state *state,
				struct intel_crtc *crtc)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_crtc_state *crtc_state = intel_atomic_get_new_crtc_state(state, crtc);
	struct drm_rect pipe_clip = { .x1 = 0, .y1 = -1, .x2 = INT_MAX, .y2 = -1 };
	struct intel_plane_state *new_plane_state, *old_plane_state;
	struct intel_plane *plane;
	bool full_update = false;
	int i, ret;

	if (!crtc_state->enable_psr2_sel_fetch)
		return 0;

	if (!psr2_sel_fetch_pipe_state_supported(crtc_state)) {
		full_update = true;
		goto skip_sel_fetch_set_loop;
	}

	/*
	 * Calculate minimal selective fetch area of each plane and calculate
	 * the pipe damaged area.
	 * In the next loop the plane selective fetch area will actually be set
	 * using whole pipe damaged area.
	 */
	for_each_oldnew_intel_plane_in_state(state, plane, old_plane_state,
					     new_plane_state, i) {
		struct drm_rect src, damaged_area = { .x1 = 0, .y1 = -1,
						      .x2 = INT_MAX };

		if (new_plane_state->uapi.crtc != crtc_state->uapi.crtc)
			continue;

		if (!new_plane_state->uapi.visible &&
		    !old_plane_state->uapi.visible)
			continue;

		if (!psr2_sel_fetch_plane_state_supported(new_plane_state)) {
			full_update = true;
			break;
		}

		/*
		 * If visibility or plane moved, mark the whole plane area as
		 * damaged as it needs to be complete redraw in the new and old
		 * position.
		 */
		if (new_plane_state->uapi.visible != old_plane_state->uapi.visible ||
		    !drm_rect_equals(&new_plane_state->uapi.dst,
				     &old_plane_state->uapi.dst)) {
			if (old_plane_state->uapi.visible) {
				damaged_area.y1 = old_plane_state->uapi.dst.y1;
				damaged_area.y2 = old_plane_state->uapi.dst.y2;
				clip_area_update(&pipe_clip, &damaged_area,
						 &crtc_state->pipe_src);
			}

			if (new_plane_state->uapi.visible) {
				damaged_area.y1 = new_plane_state->uapi.dst.y1;
				damaged_area.y2 = new_plane_state->uapi.dst.y2;
				clip_area_update(&pipe_clip, &damaged_area,
						 &crtc_state->pipe_src);
			}
			continue;
		} else if (new_plane_state->uapi.alpha != old_plane_state->uapi.alpha) {
			/* If alpha changed mark the whole plane area as damaged */
			damaged_area.y1 = new_plane_state->uapi.dst.y1;
			damaged_area.y2 = new_plane_state->uapi.dst.y2;
			clip_area_update(&pipe_clip, &damaged_area,
					 &crtc_state->pipe_src);
			continue;
		}

		src = drm_plane_state_src(&new_plane_state->uapi);
		drm_rect_fp_to_int(&src, &src);

		if (!drm_atomic_helper_damage_merged(&old_plane_state->uapi,
						     &new_plane_state->uapi, &damaged_area))
			continue;

		damaged_area.y1 += new_plane_state->uapi.dst.y1 - src.y1;
		damaged_area.y2 += new_plane_state->uapi.dst.y1 - src.y1;
		damaged_area.x1 += new_plane_state->uapi.dst.x1 - src.x1;
		damaged_area.x2 += new_plane_state->uapi.dst.x1 - src.x1;

		clip_area_update(&pipe_clip, &damaged_area, &crtc_state->pipe_src);
	}

	/*
	 * TODO: For now we are just using full update in case
	 * selective fetch area calculation fails. To optimize this we
	 * should identify cases where this happens and fix the area
	 * calculation for those.
	 */
	if (pipe_clip.y1 == -1) {
		drm_info_once(&dev_priv->drm,
			      "Selective fetch area calculation failed in pipe %c\n",
			      pipe_name(crtc->pipe));
		full_update = true;
	}

	if (full_update)
		goto skip_sel_fetch_set_loop;

	/* Wa_14014971492 */
	if ((IS_MTL_DISPLAY_STEP(dev_priv, STEP_A0, STEP_B0) ||
	     IS_ALDERLAKE_P(dev_priv) || IS_TIGERLAKE(dev_priv)) &&
	    crtc_state->splitter.enable)
		pipe_clip.y1 = 0;

	ret = drm_atomic_add_affected_planes(&state->base, &crtc->base);
	if (ret)
		return ret;

	intel_psr2_sel_fetch_pipe_alignment(crtc_state, &pipe_clip);

	/*
	 * Now that we have the pipe damaged area check if it intersect with
	 * every plane, if it does set the plane selective fetch area.
	 */
	for_each_oldnew_intel_plane_in_state(state, plane, old_plane_state,
					     new_plane_state, i) {
		struct drm_rect *sel_fetch_area, inter;
		struct intel_plane *linked = new_plane_state->planar_linked_plane;

		if (new_plane_state->uapi.crtc != crtc_state->uapi.crtc ||
		    !new_plane_state->uapi.visible)
			continue;

		inter = pipe_clip;
		if (!drm_rect_intersect(&inter, &new_plane_state->uapi.dst))
			continue;

		if (!psr2_sel_fetch_plane_state_supported(new_plane_state)) {
			full_update = true;
			break;
		}

		sel_fetch_area = &new_plane_state->psr2_sel_fetch_area;
		sel_fetch_area->y1 = inter.y1 - new_plane_state->uapi.dst.y1;
		sel_fetch_area->y2 = inter.y2 - new_plane_state->uapi.dst.y1;
		crtc_state->update_planes |= BIT(plane->id);

		/*
		 * Sel_fetch_area is calculated for UV plane. Use
		 * same area for Y plane as well.
		 */
		if (linked) {
			struct intel_plane_state *linked_new_plane_state;
			struct drm_rect *linked_sel_fetch_area;

			linked_new_plane_state = intel_atomic_get_plane_state(state, linked);
			if (IS_ERR(linked_new_plane_state))
				return PTR_ERR(linked_new_plane_state);

			linked_sel_fetch_area = &linked_new_plane_state->psr2_sel_fetch_area;
			linked_sel_fetch_area->y1 = sel_fetch_area->y1;
			linked_sel_fetch_area->y2 = sel_fetch_area->y2;
			crtc_state->update_planes |= BIT(linked->id);
		}
	}

skip_sel_fetch_set_loop:
	psr2_man_trk_ctl_calc(crtc_state, &pipe_clip, full_update);
	return 0;
}

void intel_psr_pre_plane_update(struct intel_atomic_state *state,
				struct intel_crtc *crtc)
{
	struct drm_i915_private *i915 = to_i915(state->base.dev);
	const struct intel_crtc_state *old_crtc_state =
		intel_atomic_get_old_crtc_state(state, crtc);
	const struct intel_crtc_state *new_crtc_state =
		intel_atomic_get_new_crtc_state(state, crtc);
	struct intel_encoder *encoder;

	if (!HAS_PSR(i915))
		return;

	for_each_intel_encoder_mask_with_psr(state->base.dev, encoder,
					     old_crtc_state->uapi.encoder_mask) {
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
		struct intel_psr *psr = &intel_dp->psr;
		bool needs_to_disable = false;

		mutex_lock(&psr->lock);

		/*
		 * Reasons to disable:
		 * - PSR disabled in new state
		 * - All planes will go inactive
		 * - Changing between PSR versions
		 * - Display WA #1136: skl, bxt
		 */
		needs_to_disable |= intel_crtc_needs_modeset(new_crtc_state);
		needs_to_disable |= !new_crtc_state->has_psr;
		needs_to_disable |= !new_crtc_state->active_planes;
		needs_to_disable |= new_crtc_state->has_psr2 != psr->psr2_enabled;
		needs_to_disable |= DISPLAY_VER(i915) < 11 &&
			new_crtc_state->wm_level_disabled;

		if (psr->enabled && needs_to_disable)
			intel_psr_disable_locked(intel_dp);
		else if (psr->enabled && new_crtc_state->wm_level_disabled)
			/* Wa_14015648006 */
			wm_optimization_wa(intel_dp, new_crtc_state);

		mutex_unlock(&psr->lock);
	}
}

static void _intel_psr_post_plane_update(const struct intel_atomic_state *state,
					 const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_encoder *encoder;

	if (!crtc_state->has_psr)
		return;

	for_each_intel_encoder_mask_with_psr(state->base.dev, encoder,
					     crtc_state->uapi.encoder_mask) {
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
		struct intel_psr *psr = &intel_dp->psr;
		bool keep_disabled = false;

		mutex_lock(&psr->lock);

		drm_WARN_ON(&dev_priv->drm, psr->enabled && !crtc_state->active_planes);

		keep_disabled |= psr->sink_not_reliable;
		keep_disabled |= !crtc_state->active_planes;

		/* Display WA #1136: skl, bxt */
		keep_disabled |= DISPLAY_VER(dev_priv) < 11 &&
			crtc_state->wm_level_disabled;

		if (!psr->enabled && !keep_disabled)
			intel_psr_enable_locked(intel_dp, crtc_state);
		else if (psr->enabled && !crtc_state->wm_level_disabled)
			/* Wa_14015648006 */
			wm_optimization_wa(intel_dp, crtc_state);

		/* Force a PSR exit when enabling CRC to avoid CRC timeouts */
		if (crtc_state->crc_enabled && psr->enabled)
			psr_force_hw_tracking_exit(intel_dp);

		mutex_unlock(&psr->lock);
	}
}

void intel_psr_post_plane_update(const struct intel_atomic_state *state)
{
	struct drm_i915_private *dev_priv = to_i915(state->base.dev);
	struct intel_crtc_state *crtc_state;
	struct intel_crtc *crtc;
	int i;

	if (!HAS_PSR(dev_priv))
		return;

	for_each_new_intel_crtc_in_state(state, crtc, crtc_state, i)
		_intel_psr_post_plane_update(state, crtc_state);
}

static int _psr2_ready_for_pipe_update_locked(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;

	/*
	 * Any state lower than EDP_PSR2_STATUS_STATE_DEEP_SLEEP is enough.
	 * As all higher states has bit 4 of PSR2 state set we can just wait for
	 * EDP_PSR2_STATUS_STATE_DEEP_SLEEP to be cleared.
	 */
	return intel_de_wait_for_clear(dev_priv,
				       EDP_PSR2_STATUS(cpu_transcoder),
				       EDP_PSR2_STATUS_STATE_DEEP_SLEEP, 50);
}

static int _psr1_ready_for_pipe_update_locked(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;

	/*
	 * From bspec: Panel Self Refresh (BDW+)
	 * Max. time for PSR to idle = Inverse of the refresh rate + 6 ms of
	 * exit training time + 1.5 ms of aux channel handshake. 50 ms is
	 * defensive enough to cover everything.
	 */
	return intel_de_wait_for_clear(dev_priv,
				       psr_status_reg(dev_priv, cpu_transcoder),
				       EDP_PSR_STATUS_STATE_MASK, 50);
}

/**
 * intel_psr_wait_for_idle_locked - wait for PSR be ready for a pipe update
 * @new_crtc_state: new CRTC state
 *
 * This function is expected to be called from pipe_update_start() where it is
 * not expected to race with PSR enable or disable.
 */
void intel_psr_wait_for_idle_locked(const struct intel_crtc_state *new_crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(new_crtc_state->uapi.crtc->dev);
	struct intel_encoder *encoder;

	if (!new_crtc_state->has_psr)
		return;

	for_each_intel_encoder_mask_with_psr(&dev_priv->drm, encoder,
					     new_crtc_state->uapi.encoder_mask) {
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
		int ret;

		lockdep_assert_held(&intel_dp->psr.lock);

		if (!intel_dp->psr.enabled)
			continue;

		if (intel_dp->psr.psr2_enabled)
			ret = _psr2_ready_for_pipe_update_locked(intel_dp);
		else
			ret = _psr1_ready_for_pipe_update_locked(intel_dp);

		if (ret)
			drm_err(&dev_priv->drm, "PSR wait timed out, atomic update may fail\n");
	}
}

static bool __psr_wait_for_idle_locked(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;
	i915_reg_t reg;
	u32 mask;
	int err;

	if (!intel_dp->psr.enabled)
		return false;

	if (intel_dp->psr.psr2_enabled) {
		reg = EDP_PSR2_STATUS(cpu_transcoder);
		mask = EDP_PSR2_STATUS_STATE_MASK;
	} else {
		reg = psr_status_reg(dev_priv, cpu_transcoder);
		mask = EDP_PSR_STATUS_STATE_MASK;
	}

	mutex_unlock(&intel_dp->psr.lock);

	err = intel_de_wait_for_clear(dev_priv, reg, mask, 50);
	if (err)
		drm_err(&dev_priv->drm,
			"Timed out waiting for PSR Idle for re-enable\n");

	/* After the unlocked wait, verify that PSR is still wanted! */
	mutex_lock(&intel_dp->psr.lock);
	return err == 0 && intel_dp->psr.enabled;
}

static int intel_psr_fastset_force(struct drm_i915_private *dev_priv)
{
	struct drm_connector_list_iter conn_iter;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	struct drm_connector *conn;
	int err = 0;

	state = drm_atomic_state_alloc(&dev_priv->drm);
	if (!state)
		return -ENOMEM;

	drm_modeset_acquire_init(&ctx, DRM_MODESET_ACQUIRE_INTERRUPTIBLE);

	state->acquire_ctx = &ctx;
	to_intel_atomic_state(state)->internal = true;

retry:
	drm_connector_list_iter_begin(&dev_priv->drm, &conn_iter);
	drm_for_each_connector_iter(conn, &conn_iter) {
		struct drm_connector_state *conn_state;
		struct drm_crtc_state *crtc_state;

		if (conn->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		conn_state = drm_atomic_get_connector_state(state, conn);
		if (IS_ERR(conn_state)) {
			err = PTR_ERR(conn_state);
			break;
		}

		if (!conn_state->crtc)
			continue;

		crtc_state = drm_atomic_get_crtc_state(state, conn_state->crtc);
		if (IS_ERR(crtc_state)) {
			err = PTR_ERR(crtc_state);
			break;
		}

		/* Mark mode as changed to trigger a pipe->update() */
		crtc_state->mode_changed = true;
	}
	drm_connector_list_iter_end(&conn_iter);

	if (err == 0)
		err = drm_atomic_commit(state);

	if (err == -EDEADLK) {
		drm_atomic_state_clear(state);
		err = drm_modeset_backoff(&ctx);
		if (!err)
			goto retry;
	}

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	drm_atomic_state_put(state);

	return err;
}

int intel_psr_debug_set(struct intel_dp *intel_dp, u64 val)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	const u32 mode = val & I915_PSR_DEBUG_MODE_MASK;
	u32 old_mode;
	int ret;

	if (val & ~(I915_PSR_DEBUG_IRQ | I915_PSR_DEBUG_MODE_MASK) ||
	    mode > I915_PSR_DEBUG_ENABLE_SEL_FETCH) {
		drm_dbg_kms(&dev_priv->drm, "Invalid debug mask %llx\n", val);
		return -EINVAL;
	}

	ret = mutex_lock_interruptible(&intel_dp->psr.lock);
	if (ret)
		return ret;

	old_mode = intel_dp->psr.debug & I915_PSR_DEBUG_MODE_MASK;
	intel_dp->psr.debug = val;

	/*
	 * Do it right away if it's already enabled, otherwise it will be done
	 * when enabling the source.
	 */
	if (intel_dp->psr.enabled)
		psr_irq_control(intel_dp);

	mutex_unlock(&intel_dp->psr.lock);

	if (old_mode != mode)
		ret = intel_psr_fastset_force(dev_priv);

	return ret;
}

static void intel_psr_handle_irq(struct intel_dp *intel_dp)
{
	struct intel_psr *psr = &intel_dp->psr;

	intel_psr_disable_locked(intel_dp);
	psr->sink_not_reliable = true;
	/* let's make sure that sink is awaken */
	drm_dp_dpcd_writeb(&intel_dp->aux, DP_SET_POWER, DP_SET_POWER_D0);
}

static void intel_psr_work(struct work_struct *work)
{
	struct intel_dp *intel_dp =
		container_of(work, typeof(*intel_dp), psr.work);

	mutex_lock(&intel_dp->psr.lock);

	if (!intel_dp->psr.enabled)
		goto unlock;

	if (READ_ONCE(intel_dp->psr.irq_aux_error))
		intel_psr_handle_irq(intel_dp);

	/*
	 * We have to make sure PSR is ready for re-enable
	 * otherwise it keeps disabled until next full enable/disable cycle.
	 * PSR might take some time to get fully disabled
	 * and be ready for re-enable.
	 */
	if (!__psr_wait_for_idle_locked(intel_dp))
		goto unlock;

	/*
	 * The delayed work can race with an invalidate hence we need to
	 * recheck. Since psr_flush first clears this and then reschedules we
	 * won't ever miss a flush when bailing out here.
	 */
	if (intel_dp->psr.busy_frontbuffer_bits || intel_dp->psr.active)
		goto unlock;

	intel_psr_activate(intel_dp);
unlock:
	mutex_unlock(&intel_dp->psr.lock);
}

static void _psr_invalidate_handle(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;

	if (intel_dp->psr.psr2_sel_fetch_enabled) {
		u32 val;

		if (intel_dp->psr.psr2_sel_fetch_cff_enabled) {
			/* Send one update otherwise lag is observed in screen */
			intel_de_write(dev_priv, CURSURFLIVE(intel_dp->psr.pipe), 0);
			return;
		}

		val = man_trk_ctl_enable_bit_get(dev_priv) |
		      man_trk_ctl_partial_frame_bit_get(dev_priv) |
		      man_trk_ctl_continuos_full_frame(dev_priv);
		intel_de_write(dev_priv, PSR2_MAN_TRK_CTL(cpu_transcoder), val);
		intel_de_write(dev_priv, CURSURFLIVE(intel_dp->psr.pipe), 0);
		intel_dp->psr.psr2_sel_fetch_cff_enabled = true;
	} else {
		intel_psr_exit(intel_dp);
	}
}

/**
 * intel_psr_invalidate - Invalidate PSR
 * @dev_priv: i915 device
 * @frontbuffer_bits: frontbuffer plane tracking bits
 * @origin: which operation caused the invalidate
 *
 * Since the hardware frontbuffer tracking has gaps we need to integrate
 * with the software frontbuffer tracking. This function gets called every
 * time frontbuffer rendering starts and a buffer gets dirtied. PSR must be
 * disabled if the frontbuffer mask contains a buffer relevant to PSR.
 *
 * Dirty frontbuffers relevant to PSR are tracked in busy_frontbuffer_bits."
 */
void intel_psr_invalidate(struct drm_i915_private *dev_priv,
			  unsigned frontbuffer_bits, enum fb_op_origin origin)
{
	struct intel_encoder *encoder;

	if (origin == ORIGIN_FLIP)
		return;

	for_each_intel_encoder_with_psr(&dev_priv->drm, encoder) {
		unsigned int pipe_frontbuffer_bits = frontbuffer_bits;
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

		mutex_lock(&intel_dp->psr.lock);
		if (!intel_dp->psr.enabled) {
			mutex_unlock(&intel_dp->psr.lock);
			continue;
		}

		pipe_frontbuffer_bits &=
			INTEL_FRONTBUFFER_ALL_MASK(intel_dp->psr.pipe);
		intel_dp->psr.busy_frontbuffer_bits |= pipe_frontbuffer_bits;

		if (pipe_frontbuffer_bits)
			_psr_invalidate_handle(intel_dp);

		mutex_unlock(&intel_dp->psr.lock);
	}
}
/*
 * When we will be completely rely on PSR2 S/W tracking in future,
 * intel_psr_flush() will invalidate and flush the PSR for ORIGIN_FLIP
 * event also therefore tgl_dc3co_flush_locked() require to be changed
 * accordingly in future.
 */
static void
tgl_dc3co_flush_locked(struct intel_dp *intel_dp, unsigned int frontbuffer_bits,
		       enum fb_op_origin origin)
{
	struct drm_i915_private *i915 = dp_to_i915(intel_dp);

	if (!intel_dp->psr.dc3co_exitline || !intel_dp->psr.psr2_enabled ||
	    !intel_dp->psr.active)
		return;

	/*
	 * At every frontbuffer flush flip event modified delay of delayed work,
	 * when delayed work schedules that means display has been idle.
	 */
	if (!(frontbuffer_bits &
	    INTEL_FRONTBUFFER_ALL_MASK(intel_dp->psr.pipe)))
		return;

	tgl_psr2_enable_dc3co(intel_dp);
	mod_delayed_work(i915->unordered_wq, &intel_dp->psr.dc3co_work,
			 intel_dp->psr.dc3co_exit_delay);
}

static void _psr_flush_handle(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;

	if (intel_dp->psr.psr2_sel_fetch_enabled) {
		if (intel_dp->psr.psr2_sel_fetch_cff_enabled) {
			/* can we turn CFF off? */
			if (intel_dp->psr.busy_frontbuffer_bits == 0) {
				u32 val = man_trk_ctl_enable_bit_get(dev_priv) |
					man_trk_ctl_partial_frame_bit_get(dev_priv) |
					man_trk_ctl_single_full_frame_bit_get(dev_priv) |
					man_trk_ctl_continuos_full_frame(dev_priv);

				/*
				 * Set psr2_sel_fetch_cff_enabled as false to allow selective
				 * updates. Still keep cff bit enabled as we don't have proper
				 * SU configuration in case update is sent for any reason after
				 * sff bit gets cleared by the HW on next vblank.
				 */
				intel_de_write(dev_priv, PSR2_MAN_TRK_CTL(cpu_transcoder),
					       val);
				intel_de_write(dev_priv, CURSURFLIVE(intel_dp->psr.pipe), 0);
				intel_dp->psr.psr2_sel_fetch_cff_enabled = false;
			}
		} else {
			/*
			 * continuous full frame is disabled, only a single full
			 * frame is required
			 */
			psr_force_hw_tracking_exit(intel_dp);
		}
	} else {
		psr_force_hw_tracking_exit(intel_dp);

		if (!intel_dp->psr.active && !intel_dp->psr.busy_frontbuffer_bits)
			queue_work(dev_priv->unordered_wq, &intel_dp->psr.work);
	}
}

/**
 * intel_psr_flush - Flush PSR
 * @dev_priv: i915 device
 * @frontbuffer_bits: frontbuffer plane tracking bits
 * @origin: which operation caused the flush
 *
 * Since the hardware frontbuffer tracking has gaps we need to integrate
 * with the software frontbuffer tracking. This function gets called every
 * time frontbuffer rendering has completed and flushed out to memory. PSR
 * can be enabled again if no other frontbuffer relevant to PSR is dirty.
 *
 * Dirty frontbuffers relevant to PSR are tracked in busy_frontbuffer_bits.
 */
void intel_psr_flush(struct drm_i915_private *dev_priv,
		     unsigned frontbuffer_bits, enum fb_op_origin origin)
{
	struct intel_encoder *encoder;

	for_each_intel_encoder_with_psr(&dev_priv->drm, encoder) {
		unsigned int pipe_frontbuffer_bits = frontbuffer_bits;
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

		mutex_lock(&intel_dp->psr.lock);
		if (!intel_dp->psr.enabled) {
			mutex_unlock(&intel_dp->psr.lock);
			continue;
		}

		pipe_frontbuffer_bits &=
			INTEL_FRONTBUFFER_ALL_MASK(intel_dp->psr.pipe);
		intel_dp->psr.busy_frontbuffer_bits &= ~pipe_frontbuffer_bits;

		/*
		 * If the PSR is paused by an explicit intel_psr_paused() call,
		 * we have to ensure that the PSR is not activated until
		 * intel_psr_resume() is called.
		 */
		if (intel_dp->psr.paused)
			goto unlock;

		if (origin == ORIGIN_FLIP ||
		    (origin == ORIGIN_CURSOR_UPDATE &&
		     !intel_dp->psr.psr2_sel_fetch_enabled)) {
			tgl_dc3co_flush_locked(intel_dp, frontbuffer_bits, origin);
			goto unlock;
		}

		if (pipe_frontbuffer_bits == 0)
			goto unlock;

		/* By definition flush = invalidate + flush */
		_psr_flush_handle(intel_dp);
unlock:
		mutex_unlock(&intel_dp->psr.lock);
	}
}

/**
 * intel_psr_init - Init basic PSR work and mutex.
 * @intel_dp: Intel DP
 *
 * This function is called after the initializing connector.
 * (the initializing of connector treats the handling of connector capabilities)
 * And it initializes basic PSR stuff for each DP Encoder.
 */
void intel_psr_init(struct intel_dp *intel_dp)
{
	struct intel_connector *connector = intel_dp->attached_connector;
	struct intel_digital_port *dig_port = dp_to_dig_port(intel_dp);
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);

	if (!HAS_PSR(dev_priv))
		return;

	/*
	 * HSW spec explicitly says PSR is tied to port A.
	 * BDW+ platforms have a instance of PSR registers per transcoder but
	 * BDW, GEN9 and GEN11 are not validated by HW team in other transcoder
	 * than eDP one.
	 * For now it only supports one instance of PSR for BDW, GEN9 and GEN11.
	 * So lets keep it hardcoded to PORT_A for BDW, GEN9 and GEN11.
	 * But GEN12 supports a instance of PSR registers per transcoder.
	 */
	if (DISPLAY_VER(dev_priv) < 12 && dig_port->base.port != PORT_A) {
		drm_dbg_kms(&dev_priv->drm,
			    "PSR condition failed: Port not supported\n");
		return;
	}

	intel_dp->psr.source_support = true;

	/* Set link_standby x link_off defaults */
	if (DISPLAY_VER(dev_priv) < 12)
		/* For new platforms up to TGL let's respect VBT back again */
		intel_dp->psr.link_standby = connector->panel.vbt.psr.full_link;

	INIT_WORK(&intel_dp->psr.work, intel_psr_work);
	INIT_DELAYED_WORK(&intel_dp->psr.dc3co_work, tgl_dc3co_disable_work);
	mutex_init(&intel_dp->psr.lock);
}

static int psr_get_status_and_error_status(struct intel_dp *intel_dp,
					   u8 *status, u8 *error_status)
{
	struct drm_dp_aux *aux = &intel_dp->aux;
	int ret;

	ret = drm_dp_dpcd_readb(aux, DP_PSR_STATUS, status);
	if (ret != 1)
		return ret;

	ret = drm_dp_dpcd_readb(aux, DP_PSR_ERROR_STATUS, error_status);
	if (ret != 1)
		return ret;

	*status = *status & DP_PSR_SINK_STATE_MASK;

	return 0;
}

static void psr_alpm_check(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	struct drm_dp_aux *aux = &intel_dp->aux;
	struct intel_psr *psr = &intel_dp->psr;
	u8 val;
	int r;

	if (!psr->psr2_enabled)
		return;

	r = drm_dp_dpcd_readb(aux, DP_RECEIVER_ALPM_STATUS, &val);
	if (r != 1) {
		drm_err(&dev_priv->drm, "Error reading ALPM status\n");
		return;
	}

	if (val & DP_ALPM_LOCK_TIMEOUT_ERROR) {
		intel_psr_disable_locked(intel_dp);
		psr->sink_not_reliable = true;
		drm_dbg_kms(&dev_priv->drm,
			    "ALPM lock timeout error, disabling PSR\n");

		/* Clearing error */
		drm_dp_dpcd_writeb(aux, DP_RECEIVER_ALPM_STATUS, val);
	}
}

static void psr_capability_changed_check(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	struct intel_psr *psr = &intel_dp->psr;
	u8 val;
	int r;

	r = drm_dp_dpcd_readb(&intel_dp->aux, DP_PSR_ESI, &val);
	if (r != 1) {
		drm_err(&dev_priv->drm, "Error reading DP_PSR_ESI\n");
		return;
	}

	if (val & DP_PSR_CAPS_CHANGE) {
		intel_psr_disable_locked(intel_dp);
		psr->sink_not_reliable = true;
		drm_dbg_kms(&dev_priv->drm,
			    "Sink PSR capability changed, disabling PSR\n");

		/* Clearing it */
		drm_dp_dpcd_writeb(&intel_dp->aux, DP_PSR_ESI, val);
	}
}

void intel_psr_short_pulse(struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	struct intel_psr *psr = &intel_dp->psr;
	u8 status, error_status;
	const u8 errors = DP_PSR_RFB_STORAGE_ERROR |
			  DP_PSR_VSC_SDP_UNCORRECTABLE_ERROR |
			  DP_PSR_LINK_CRC_ERROR;

	if (!CAN_PSR(intel_dp))
		return;

	mutex_lock(&psr->lock);

	if (!psr->enabled)
		goto exit;

	if (psr_get_status_and_error_status(intel_dp, &status, &error_status)) {
		drm_err(&dev_priv->drm,
			"Error reading PSR status or error status\n");
		goto exit;
	}

	if (status == DP_PSR_SINK_INTERNAL_ERROR || (error_status & errors)) {
		intel_psr_disable_locked(intel_dp);
		psr->sink_not_reliable = true;
	}

	if (status == DP_PSR_SINK_INTERNAL_ERROR && !error_status)
		drm_dbg_kms(&dev_priv->drm,
			    "PSR sink internal error, disabling PSR\n");
	if (error_status & DP_PSR_RFB_STORAGE_ERROR)
		drm_dbg_kms(&dev_priv->drm,
			    "PSR RFB storage error, disabling PSR\n");
	if (error_status & DP_PSR_VSC_SDP_UNCORRECTABLE_ERROR)
		drm_dbg_kms(&dev_priv->drm,
			    "PSR VSC SDP uncorrectable error, disabling PSR\n");
	if (error_status & DP_PSR_LINK_CRC_ERROR)
		drm_dbg_kms(&dev_priv->drm,
			    "PSR Link CRC error, disabling PSR\n");

	if (error_status & ~errors)
		drm_err(&dev_priv->drm,
			"PSR_ERROR_STATUS unhandled errors %x\n",
			error_status & ~errors);
	/* clear status register */
	drm_dp_dpcd_writeb(&intel_dp->aux, DP_PSR_ERROR_STATUS, error_status);

	psr_alpm_check(intel_dp);
	psr_capability_changed_check(intel_dp);

exit:
	mutex_unlock(&psr->lock);
}

bool intel_psr_enabled(struct intel_dp *intel_dp)
{
	bool ret;

	if (!CAN_PSR(intel_dp))
		return false;

	mutex_lock(&intel_dp->psr.lock);
	ret = intel_dp->psr.enabled;
	mutex_unlock(&intel_dp->psr.lock);

	return ret;
}

/**
 * intel_psr_lock - grab PSR lock
 * @crtc_state: the crtc state
 *
 * This is initially meant to be used by around CRTC update, when
 * vblank sensitive registers are updated and we need grab the lock
 * before it to avoid vblank evasion.
 */
void intel_psr_lock(const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915 = to_i915(crtc_state->uapi.crtc->dev);
	struct intel_encoder *encoder;

	if (!crtc_state->has_psr)
		return;

	for_each_intel_encoder_mask_with_psr(&i915->drm, encoder,
					     crtc_state->uapi.encoder_mask) {
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

		mutex_lock(&intel_dp->psr.lock);
		break;
	}
}

/**
 * intel_psr_unlock - release PSR lock
 * @crtc_state: the crtc state
 *
 * Release the PSR lock that was held during pipe update.
 */
void intel_psr_unlock(const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915 = to_i915(crtc_state->uapi.crtc->dev);
	struct intel_encoder *encoder;

	if (!crtc_state->has_psr)
		return;

	for_each_intel_encoder_mask_with_psr(&i915->drm, encoder,
					     crtc_state->uapi.encoder_mask) {
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

		mutex_unlock(&intel_dp->psr.lock);
		break;
	}
}

static void
psr_source_status(struct intel_dp *intel_dp, struct seq_file *m)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;
	const char *status = "unknown";
	u32 val, status_val;

	if (intel_dp->psr.psr2_enabled) {
		static const char * const live_status[] = {
			"IDLE",
			"CAPTURE",
			"CAPTURE_FS",
			"SLEEP",
			"BUFON_FW",
			"ML_UP",
			"SU_STANDBY",
			"FAST_SLEEP",
			"DEEP_SLEEP",
			"BUF_ON",
			"TG_ON"
		};
		val = intel_de_read(dev_priv, EDP_PSR2_STATUS(cpu_transcoder));
		status_val = REG_FIELD_GET(EDP_PSR2_STATUS_STATE_MASK, val);
		if (status_val < ARRAY_SIZE(live_status))
			status = live_status[status_val];
	} else {
		static const char * const live_status[] = {
			"IDLE",
			"SRDONACK",
			"SRDENT",
			"BUFOFF",
			"BUFON",
			"AUXACK",
			"SRDOFFACK",
			"SRDENT_ON",
		};
		val = intel_de_read(dev_priv, psr_status_reg(dev_priv, cpu_transcoder));
		status_val = REG_FIELD_GET(EDP_PSR_STATUS_STATE_MASK, val);
		if (status_val < ARRAY_SIZE(live_status))
			status = live_status[status_val];
	}

	seq_printf(m, "Source PSR status: %s [0x%08x]\n", status, val);
}

static int intel_psr_status(struct seq_file *m, struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv = dp_to_i915(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->psr.transcoder;
	struct intel_psr *psr = &intel_dp->psr;
	intel_wakeref_t wakeref;
	const char *status;
	bool enabled;
	u32 val;

	seq_printf(m, "Sink support: %s", str_yes_no(psr->sink_support));
	if (psr->sink_support)
		seq_printf(m, " [0x%02x]", intel_dp->psr_dpcd[0]);
	seq_puts(m, "\n");

	if (!psr->sink_support)
		return 0;

	wakeref = intel_runtime_pm_get(&dev_priv->runtime_pm);
	mutex_lock(&psr->lock);

	if (psr->enabled)
		status = psr->psr2_enabled ? "PSR2 enabled" : "PSR1 enabled";
	else
		status = "disabled";
	seq_printf(m, "PSR mode: %s\n", status);

	if (!psr->enabled) {
		seq_printf(m, "PSR sink not reliable: %s\n",
			   str_yes_no(psr->sink_not_reliable));

		goto unlock;
	}

	if (psr->psr2_enabled) {
		val = intel_de_read(dev_priv, EDP_PSR2_CTL(cpu_transcoder));
		enabled = val & EDP_PSR2_ENABLE;
	} else {
		val = intel_de_read(dev_priv, psr_ctl_reg(dev_priv, cpu_transcoder));
		enabled = val & EDP_PSR_ENABLE;
	}
	seq_printf(m, "Source PSR ctl: %s [0x%08x]\n",
		   str_enabled_disabled(enabled), val);
	psr_source_status(intel_dp, m);
	seq_printf(m, "Busy frontbuffer bits: 0x%08x\n",
		   psr->busy_frontbuffer_bits);

	/*
	 * SKL+ Perf counter is reset to 0 everytime DC state is entered
	 */
	val = intel_de_read(dev_priv, psr_perf_cnt_reg(dev_priv, cpu_transcoder));
	seq_printf(m, "Performance counter: %u\n",
		   REG_FIELD_GET(EDP_PSR_PERF_CNT_MASK, val));

	if (psr->debug & I915_PSR_DEBUG_IRQ) {
		seq_printf(m, "Last attempted entry at: %lld\n",
			   psr->last_entry_attempt);
		seq_printf(m, "Last exit at: %lld\n", psr->last_exit);
	}

	if (psr->psr2_enabled) {
		u32 su_frames_val[3];
		int frame;

		/*
		 * Reading all 3 registers before hand to minimize crossing a
		 * frame boundary between register reads
		 */
		for (frame = 0; frame < PSR2_SU_STATUS_FRAMES; frame += 3) {
			val = intel_de_read(dev_priv, PSR2_SU_STATUS(cpu_transcoder, frame));
			su_frames_val[frame / 3] = val;
		}

		seq_puts(m, "Frame:\tPSR2 SU blocks:\n");

		for (frame = 0; frame < PSR2_SU_STATUS_FRAMES; frame++) {
			u32 su_blocks;

			su_blocks = su_frames_val[frame / 3] &
				    PSR2_SU_STATUS_MASK(frame);
			su_blocks = su_blocks >> PSR2_SU_STATUS_SHIFT(frame);
			seq_printf(m, "%d\t%d\n", frame, su_blocks);
		}

		seq_printf(m, "PSR2 selective fetch: %s\n",
			   str_enabled_disabled(psr->psr2_sel_fetch_enabled));
	}

unlock:
	mutex_unlock(&psr->lock);
	intel_runtime_pm_put(&dev_priv->runtime_pm, wakeref);

	return 0;
}

static int i915_edp_psr_status_show(struct seq_file *m, void *data)
{
	struct drm_i915_private *dev_priv = m->private;
	struct intel_dp *intel_dp = NULL;
	struct intel_encoder *encoder;

	if (!HAS_PSR(dev_priv))
		return -ENODEV;

	/* Find the first EDP which supports PSR */
	for_each_intel_encoder_with_psr(&dev_priv->drm, encoder) {
		intel_dp = enc_to_intel_dp(encoder);
		break;
	}

	if (!intel_dp)
		return -ENODEV;

	return intel_psr_status(m, intel_dp);
}
DEFINE_SHOW_ATTRIBUTE(i915_edp_psr_status);

static int
i915_edp_psr_debug_set(void *data, u64 val)
{
	struct drm_i915_private *dev_priv = data;
	struct intel_encoder *encoder;
	intel_wakeref_t wakeref;
	int ret = -ENODEV;

	if (!HAS_PSR(dev_priv))
		return ret;

	for_each_intel_encoder_with_psr(&dev_priv->drm, encoder) {
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

		drm_dbg_kms(&dev_priv->drm, "Setting PSR debug to %llx\n", val);

		wakeref = intel_runtime_pm_get(&dev_priv->runtime_pm);

		// TODO: split to each transcoder's PSR debug state
		ret = intel_psr_debug_set(intel_dp, val);

		intel_runtime_pm_put(&dev_priv->runtime_pm, wakeref);
	}

	return ret;
}

static int
i915_edp_psr_debug_get(void *data, u64 *val)
{
	struct drm_i915_private *dev_priv = data;
	struct intel_encoder *encoder;

	if (!HAS_PSR(dev_priv))
		return -ENODEV;

	for_each_intel_encoder_with_psr(&dev_priv->drm, encoder) {
		struct intel_dp *intel_dp = enc_to_intel_dp(encoder);

		// TODO: split to each transcoder's PSR debug state
		*val = READ_ONCE(intel_dp->psr.debug);
		return 0;
	}

	return -ENODEV;
}

DEFINE_SIMPLE_ATTRIBUTE(i915_edp_psr_debug_fops,
			i915_edp_psr_debug_get, i915_edp_psr_debug_set,
			"%llu\n");

void intel_psr_debugfs_register(struct drm_i915_private *i915)
{
	struct drm_minor *minor = i915->drm.primary;

	debugfs_create_file("i915_edp_psr_debug", 0644, minor->debugfs_root,
			    i915, &i915_edp_psr_debug_fops);

	debugfs_create_file("i915_edp_psr_status", 0444, minor->debugfs_root,
			    i915, &i915_edp_psr_status_fops);
}

static int i915_psr_sink_status_show(struct seq_file *m, void *data)
{
	struct intel_connector *connector = m->private;
	struct intel_dp *intel_dp = intel_attached_dp(connector);
	static const char * const sink_status[] = {
		"inactive",
		"transition to active, capture and display",
		"active, display from RFB",
		"active, capture and display on sink device timings",
		"transition to inactive, capture and display, timing re-sync",
		"reserved",
		"reserved",
		"sink internal error",
	};
	const char *str;
	int ret;
	u8 val;

	if (!CAN_PSR(intel_dp)) {
		seq_puts(m, "PSR Unsupported\n");
		return -ENODEV;
	}

	if (connector->base.status != connector_status_connected)
		return -ENODEV;

	ret = drm_dp_dpcd_readb(&intel_dp->aux, DP_PSR_STATUS, &val);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;

	val &= DP_PSR_SINK_STATE_MASK;
	if (val < ARRAY_SIZE(sink_status))
		str = sink_status[val];
	else
		str = "unknown";

	seq_printf(m, "Sink PSR status: 0x%x [%s]\n", val, str);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(i915_psr_sink_status);

static int i915_psr_status_show(struct seq_file *m, void *data)
{
	struct intel_connector *connector = m->private;
	struct intel_dp *intel_dp = intel_attached_dp(connector);

	return intel_psr_status(m, intel_dp);
}
DEFINE_SHOW_ATTRIBUTE(i915_psr_status);

void intel_psr_connector_debugfs_add(struct intel_connector *connector)
{
	struct drm_i915_private *i915 = to_i915(connector->base.dev);
	struct dentry *root = connector->base.debugfs_entry;

	if (connector->base.connector_type != DRM_MODE_CONNECTOR_eDP)
		return;

	debugfs_create_file("i915_psr_sink_status", 0444, root,
			    connector, &i915_psr_sink_status_fops);

	if (HAS_PSR(i915))
		debugfs_create_file("i915_psr_status", 0444, root,
				    connector, &i915_psr_status_fops);
}
