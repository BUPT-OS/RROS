
/*
 * Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */
/*********************************************************************/
//				USB4 DPIA BANDWIDTH ALLOCATION LOGIC
/*********************************************************************/
#include "link_dp_dpia_bw.h"
#include "link_dpcd.h"
#include "dc_dmub_srv.h"

#define DC_LOGGER \
	link->ctx->logger

#define Kbps_TO_Gbps (1000 * 1000)

// ------------------------------------------------------------------
//					PRIVATE FUNCTIONS
// ------------------------------------------------------------------
/*
 * Always Check the following:
 *  - Is it USB4 link?
 *  - Is HPD HIGH?
 *  - Is BW Allocation Support Mode enabled on DP-Tx?
 */
static bool get_bw_alloc_proceed_flag(struct dc_link *tmp)
{
	return (tmp && DISPLAY_ENDPOINT_USB4_DPIA == tmp->ep_type
			&& tmp->hpd_status
			&& tmp->dpia_bw_alloc_config.bw_alloc_enabled);
}
static void reset_bw_alloc_struct(struct dc_link *link)
{
	link->dpia_bw_alloc_config.bw_alloc_enabled = false;
	link->dpia_bw_alloc_config.sink_verified_bw = 0;
	link->dpia_bw_alloc_config.sink_max_bw = 0;
	link->dpia_bw_alloc_config.estimated_bw = 0;
	link->dpia_bw_alloc_config.bw_granularity = 0;
	link->dpia_bw_alloc_config.response_ready = false;
}
static uint8_t get_bw_granularity(struct dc_link *link)
{
	uint8_t bw_granularity = 0;

	core_link_read_dpcd(
			link,
			DP_BW_GRANULALITY,
			&bw_granularity,
			sizeof(uint8_t));

	switch (bw_granularity & 0x3) {
	case 0:
		bw_granularity = 4;
		break;
	case 1:
	default:
		bw_granularity = 2;
		break;
	}

	return bw_granularity;
}
static int get_estimated_bw(struct dc_link *link)
{
	uint8_t bw_estimated_bw = 0;

	core_link_read_dpcd(
			link,
			ESTIMATED_BW,
			&bw_estimated_bw,
			sizeof(uint8_t));

	return bw_estimated_bw * (Kbps_TO_Gbps / link->dpia_bw_alloc_config.bw_granularity);
}
static bool allocate_usb4_bw(int *stream_allocated_bw, int bw_needed, struct dc_link *link)
{
	if (bw_needed > 0)
		*stream_allocated_bw += bw_needed;

	return true;
}
static bool deallocate_usb4_bw(int *stream_allocated_bw, int bw_to_dealloc, struct dc_link *link)
{
	bool ret = false;

	if (*stream_allocated_bw > 0) {
		*stream_allocated_bw -= bw_to_dealloc;
		ret = true;
	} else {
		//Do nothing for now
		ret = true;
	}

	// Unplug so reset values
	if (!link->hpd_status)
		reset_bw_alloc_struct(link);

	return ret;
}
/*
 * Read all New BW alloc configuration ex: estimated_bw, allocated_bw,
 * granuality, Driver_ID, CM_Group, & populate the BW allocation structs
 * for host router and dpia
 */
static void init_usb4_bw_struct(struct dc_link *link)
{
	// Init the known values
	link->dpia_bw_alloc_config.bw_granularity = get_bw_granularity(link);
	link->dpia_bw_alloc_config.estimated_bw = get_estimated_bw(link);
}
static uint8_t get_lowest_dpia_index(struct dc_link *link)
{
	const struct dc *dc_struct = link->dc;
	uint8_t idx = 0xFF;
	int i;

	for (i = 0; i < MAX_PIPES * 2; ++i) {

		if (!dc_struct->links[i] ||
				dc_struct->links[i]->ep_type != DISPLAY_ENDPOINT_USB4_DPIA)
			continue;

		if (idx > dc_struct->links[i]->link_index)
			idx = dc_struct->links[i]->link_index;
	}

	return idx;
}
/*
 * Get the Max Available BW or Max Estimated BW for each Host Router
 *
 * @link: pointer to the dc_link struct instance
 * @type: ESTIMATD BW or MAX AVAILABLE BW
 *
 * return: response_ready flag from dc_link struct
 */
static int get_host_router_total_bw(struct dc_link *link, uint8_t type)
{
	const struct dc *dc_struct = link->dc;
	uint8_t lowest_dpia_index = get_lowest_dpia_index(link);
	uint8_t idx = (link->link_index - lowest_dpia_index) / 2, idx_temp = 0;
	struct dc_link *link_temp;
	int total_bw = 0;
	int i;

	for (i = 0; i < MAX_PIPES * 2; ++i) {

		if (!dc_struct->links[i] || dc_struct->links[i]->ep_type != DISPLAY_ENDPOINT_USB4_DPIA)
			continue;

		link_temp = dc_struct->links[i];
		if (!link_temp || !link_temp->hpd_status)
			continue;

		idx_temp = (link_temp->link_index - lowest_dpia_index) / 2;

		if (idx_temp == idx) {

			if (type == HOST_ROUTER_BW_ESTIMATED)
				total_bw += link_temp->dpia_bw_alloc_config.estimated_bw;
			else if (type == HOST_ROUTER_BW_ALLOCATED)
				total_bw += link_temp->dpia_bw_alloc_config.sink_allocated_bw;
		}
	}

	return total_bw;
}
/*
 * Cleanup function for when the dpia is unplugged to reset struct
 * and perform any required clean up
 *
 * @link: pointer to the dc_link struct instance
 *
 * return: none
 */
static bool dpia_bw_alloc_unplug(struct dc_link *link)
{
	if (!link)
		return true;

	return deallocate_usb4_bw(&link->dpia_bw_alloc_config.sink_allocated_bw,
			link->dpia_bw_alloc_config.sink_allocated_bw, link);
}
static void set_usb4_req_bw_req(struct dc_link *link, int req_bw)
{
	uint8_t requested_bw;
	uint32_t temp;

	// 1. Add check for this corner case #1
	if (req_bw > link->dpia_bw_alloc_config.estimated_bw)
		req_bw = link->dpia_bw_alloc_config.estimated_bw;

	temp = req_bw * link->dpia_bw_alloc_config.bw_granularity;
	requested_bw = temp / Kbps_TO_Gbps;

	// Always make sure to add more to account for floating points
	if (temp % Kbps_TO_Gbps)
		++requested_bw;

	// 2. Add check for this corner case #2
	req_bw = requested_bw * (Kbps_TO_Gbps / link->dpia_bw_alloc_config.bw_granularity);
	if (req_bw == link->dpia_bw_alloc_config.sink_allocated_bw)
		return;

	if (core_link_write_dpcd(
		link,
		REQUESTED_BW,
		&requested_bw,
		sizeof(uint8_t)) == DC_OK)
		link->dpia_bw_alloc_config.response_ready = false; // Reset flag
}
/*
 * Return the response_ready flag from dc_link struct
 *
 * @link: pointer to the dc_link struct instance
 *
 * return: response_ready flag from dc_link struct
 */
static bool get_cm_response_ready_flag(struct dc_link *link)
{
	return link->dpia_bw_alloc_config.response_ready;
}
// ------------------------------------------------------------------
//					PUBLIC FUNCTIONS
// ------------------------------------------------------------------
bool link_dp_dpia_set_dptx_usb4_bw_alloc_support(struct dc_link *link)
{
	bool ret = false;
	uint8_t response = 0,
			bw_support_dpia = 0,
			bw_support_cm = 0;

	if (!(link->ep_type == DISPLAY_ENDPOINT_USB4_DPIA && link->hpd_status))
		goto out;

	if (core_link_read_dpcd(
			link,
			DP_TUNNELING_CAPABILITIES,
			&response,
			sizeof(uint8_t)) == DC_OK)
		bw_support_dpia = (response >> 7) & 1;

	if (core_link_read_dpcd(
		link,
		USB4_DRIVER_BW_CAPABILITY,
		&response,
		sizeof(uint8_t)) == DC_OK)
		bw_support_cm = (response >> 7) & 1;

	/* Send request acknowledgment to Turn ON DPTX support */
	if (bw_support_cm && bw_support_dpia) {

		response = 0x80;
		if (core_link_write_dpcd(
				link,
				DPTX_BW_ALLOCATION_MODE_CONTROL,
				&response,
				sizeof(uint8_t)) != DC_OK) {
			DC_LOG_DEBUG("%s: **** FAILURE Enabling DPtx BW Allocation Mode Support ***\n",
					__func__);
		} else {
			// SUCCESS Enabled DPtx BW Allocation Mode Support
			link->dpia_bw_alloc_config.bw_alloc_enabled = true;
			DC_LOG_DEBUG("%s: **** SUCCESS Enabling DPtx BW Allocation Mode Support ***\n",
					__func__);

			ret = true;
			init_usb4_bw_struct(link);
		}
	}

out:
	return ret;
}
void dpia_handle_bw_alloc_response(struct dc_link *link, uint8_t bw, uint8_t result)
{
	int bw_needed = 0;
	int estimated = 0;
	int host_router_total_estimated_bw = 0;

	if (!get_bw_alloc_proceed_flag((link)))
		return;

	switch (result) {

	case DPIA_BW_REQ_FAILED:

		DC_LOG_DEBUG("%s: *** *** BW REQ FAILURE for DP-TX Request *** ***\n", __func__);

		// Update the new Estimated BW value updated by CM
		link->dpia_bw_alloc_config.estimated_bw =
				bw * (Kbps_TO_Gbps / link->dpia_bw_alloc_config.bw_granularity);

		set_usb4_req_bw_req(link, link->dpia_bw_alloc_config.estimated_bw);
		link->dpia_bw_alloc_config.response_ready = false;

		/*
		 * If FAIL then it is either:
		 * 1. Due to DP-Tx trying to allocate more than available i.e. it failed locally
		 *    => get estimated and allocate that
		 * 2. Due to the fact that DP-Tx tried to allocated ESTIMATED BW and failed then
		 *    CM will have to update 0xE0023 with new ESTIMATED BW value.
		 */
		break;

	case DPIA_BW_REQ_SUCCESS:

		DC_LOG_DEBUG("%s: *** BW REQ SUCCESS for DP-TX Request ***\n", __func__);

		// 1. SUCCESS 1st time before any Pruning is done
		// 2. SUCCESS after prev. FAIL before any Pruning is done
		// 3. SUCCESS after Pruning is done but before enabling link

		bw_needed = bw * (Kbps_TO_Gbps / link->dpia_bw_alloc_config.bw_granularity);

		// 1.
		if (!link->dpia_bw_alloc_config.sink_allocated_bw) {

			allocate_usb4_bw(&link->dpia_bw_alloc_config.sink_allocated_bw, bw_needed, link);
			link->dpia_bw_alloc_config.sink_verified_bw =
					link->dpia_bw_alloc_config.sink_allocated_bw;

			// SUCCESS from first attempt
			if (link->dpia_bw_alloc_config.sink_allocated_bw >
			link->dpia_bw_alloc_config.sink_max_bw)
				link->dpia_bw_alloc_config.sink_verified_bw =
						link->dpia_bw_alloc_config.sink_max_bw;
		}
		// 3.
		else if (link->dpia_bw_alloc_config.sink_allocated_bw) {

			// Find out how much do we need to de-alloc
			if (link->dpia_bw_alloc_config.sink_allocated_bw > bw_needed)
				deallocate_usb4_bw(&link->dpia_bw_alloc_config.sink_allocated_bw,
						link->dpia_bw_alloc_config.sink_allocated_bw - bw_needed, link);
			else
				allocate_usb4_bw(&link->dpia_bw_alloc_config.sink_allocated_bw,
						bw_needed - link->dpia_bw_alloc_config.sink_allocated_bw, link);
		}

		// 4. If this is the 2nd sink then any unused bw will be reallocated to master DPIA
		// => check if estimated_bw changed

		link->dpia_bw_alloc_config.response_ready = true;
		break;

	case DPIA_EST_BW_CHANGED:

		DC_LOG_DEBUG("%s: *** ESTIMATED BW CHANGED for DP-TX Request ***\n", __func__);

		estimated = bw * (Kbps_TO_Gbps / link->dpia_bw_alloc_config.bw_granularity);
		host_router_total_estimated_bw = get_host_router_total_bw(link, HOST_ROUTER_BW_ESTIMATED);

		// 1. If due to unplug of other sink
		if (estimated == host_router_total_estimated_bw) {
			// First update the estimated & max_bw fields
			if (link->dpia_bw_alloc_config.estimated_bw < estimated)
				link->dpia_bw_alloc_config.estimated_bw = estimated;
		}
		// 2. If due to realloc bw btw 2 dpia due to plug OR realloc unused Bw
		else {
			// We lost estimated bw usually due to plug event of other dpia
			link->dpia_bw_alloc_config.estimated_bw = estimated;
		}
		break;

	case DPIA_BW_ALLOC_CAPS_CHANGED:

		DC_LOG_DEBUG("%s: *** BW ALLOC CAPABILITY CHANGED for DP-TX Request ***\n", __func__);
		link->dpia_bw_alloc_config.bw_alloc_enabled = false;
		break;
	}
}
int dpia_handle_usb4_bandwidth_allocation_for_link(struct dc_link *link, int peak_bw)
{
	int ret = 0;
	uint8_t timeout = 10;

	if (!(link && DISPLAY_ENDPOINT_USB4_DPIA == link->ep_type
			&& link->dpia_bw_alloc_config.bw_alloc_enabled))
		goto out;

	//1. Hot Plug
	if (link->hpd_status && peak_bw > 0) {

		// If DP over USB4 then we need to check BW allocation
		link->dpia_bw_alloc_config.sink_max_bw = peak_bw;
		set_usb4_req_bw_req(link, link->dpia_bw_alloc_config.sink_max_bw);

		do {
			if (!(timeout > 0))
				timeout--;
			else
				break;
			fsleep(10 * 1000);
		} while (!get_cm_response_ready_flag(link));

		if (!timeout)
			ret = 0;// ERROR TIMEOUT waiting for response for allocating bw
		else if (link->dpia_bw_alloc_config.sink_allocated_bw > 0)
			ret = get_host_router_total_bw(link, HOST_ROUTER_BW_ALLOCATED);
	}
	//2. Cold Unplug
	else if (!link->hpd_status)
		dpia_bw_alloc_unplug(link);

out:
	return ret;
}
int link_dp_dpia_allocate_usb4_bandwidth_for_stream(struct dc_link *link, int req_bw)
{
	int ret = 0;
	uint8_t timeout = 10;

	if (!get_bw_alloc_proceed_flag(link))
		goto out;

	/*
	 * Sometimes stream uses same timing parameters as the already
	 * allocated max sink bw so no need to re-alloc
	 */
	if (req_bw != link->dpia_bw_alloc_config.sink_allocated_bw) {
		set_usb4_req_bw_req(link, req_bw);
		do {
			if (!(timeout > 0))
				timeout--;
			else
				break;
			udelay(10 * 1000);
		} while (!get_cm_response_ready_flag(link));

		if (!timeout)
			ret = 0;// ERROR TIMEOUT waiting for response for allocating bw
		else if (link->dpia_bw_alloc_config.sink_allocated_bw > 0)
			ret = get_host_router_total_bw(link, HOST_ROUTER_BW_ALLOCATED);
	}

out:
	return ret;
}
bool dpia_validate_usb4_bw(struct dc_link **link, int *bw_needed_per_dpia, const unsigned int num_dpias)
{
	bool ret = true;
	int bw_needed_per_hr[MAX_HR_NUM] = { 0, 0 };
	uint8_t lowest_dpia_index = 0, dpia_index = 0;
	uint8_t i;

	if (!num_dpias || num_dpias > MAX_DPIA_NUM)
		return ret;

	//Get total Host Router BW & Validate against each Host Router max BW
	for (i = 0; i < num_dpias; ++i) {

		if (!link[i]->dpia_bw_alloc_config.bw_alloc_enabled)
			continue;

		lowest_dpia_index = get_lowest_dpia_index(link[i]);
		if (link[i]->link_index < lowest_dpia_index)
			continue;

		dpia_index = (link[i]->link_index - lowest_dpia_index) / 2;
		bw_needed_per_hr[dpia_index] += bw_needed_per_dpia[i];
		if (bw_needed_per_hr[dpia_index] > get_host_router_total_bw(link[i], HOST_ROUTER_BW_ALLOCATED)) {

			ret = false;
			break;
		}
	}

	return ret;
}
