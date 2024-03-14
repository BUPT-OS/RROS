/* SPDX-License-Identifier: GPL-2.0-only */
/****************************************************************************
 * Driver for Solarflare network controllers and boards
 * Copyright 2018 Solarflare Communications Inc.
 * Copyright 2019-2020 Xilinx Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 */
#ifndef EFX_EF100_NIC_H
#define EFX_EF100_NIC_H

#include "net_driver.h"
#include "nic_common.h"

extern const struct efx_nic_type ef100_pf_nic_type;
extern const struct efx_nic_type ef100_vf_nic_type;

int ef100_probe_netdev_pf(struct efx_nic *efx);
int ef100_probe_vf(struct efx_nic *efx);
void ef100_remove(struct efx_nic *efx);

enum {
	EF100_STAT_port_tx_bytes = GENERIC_STAT_COUNT,
	EF100_STAT_port_tx_packets,
	EF100_STAT_port_tx_pause,
	EF100_STAT_port_tx_unicast,
	EF100_STAT_port_tx_multicast,
	EF100_STAT_port_tx_broadcast,
	EF100_STAT_port_tx_lt64,
	EF100_STAT_port_tx_64,
	EF100_STAT_port_tx_65_to_127,
	EF100_STAT_port_tx_128_to_255,
	EF100_STAT_port_tx_256_to_511,
	EF100_STAT_port_tx_512_to_1023,
	EF100_STAT_port_tx_1024_to_15xx,
	EF100_STAT_port_tx_15xx_to_jumbo,
	EF100_STAT_port_rx_bytes,
	EF100_STAT_port_rx_packets,
	EF100_STAT_port_rx_good,
	EF100_STAT_port_rx_bad,
	EF100_STAT_port_rx_pause,
	EF100_STAT_port_rx_unicast,
	EF100_STAT_port_rx_multicast,
	EF100_STAT_port_rx_broadcast,
	EF100_STAT_port_rx_lt64,
	EF100_STAT_port_rx_64,
	EF100_STAT_port_rx_65_to_127,
	EF100_STAT_port_rx_128_to_255,
	EF100_STAT_port_rx_256_to_511,
	EF100_STAT_port_rx_512_to_1023,
	EF100_STAT_port_rx_1024_to_15xx,
	EF100_STAT_port_rx_15xx_to_jumbo,
	EF100_STAT_port_rx_gtjumbo,
	EF100_STAT_port_rx_bad_gtjumbo,
	EF100_STAT_port_rx_align_error,
	EF100_STAT_port_rx_length_error,
	EF100_STAT_port_rx_overflow,
	EF100_STAT_port_rx_nodesc_drops,
	EF100_STAT_COUNT
};

struct ef100_nic_data {
	struct efx_nic *efx;
	struct efx_buffer mcdi_buf;
	u32 datapath_caps;
	u32 datapath_caps2;
	u32 datapath_caps3;
	unsigned int pf_index;
	u16 warm_boot_count;
	u8 port_id[ETH_ALEN];
	DECLARE_BITMAP(evq_phases, EFX_MAX_CHANNELS);
	u64 stats[EF100_STAT_COUNT];
	u32 base_mport;
	bool have_mport; /* base_mport was populated successfully */
	u32 own_mport;
	u32 local_mae_intf; /* interface_idx that corresponds to us, in mport enumerate */
	bool have_own_mport; /* own_mport was populated successfully */
	bool have_local_intf; /* local_mae_intf was populated successfully */
	bool grp_mae; /* MAE Privilege */
	u16 tso_max_hdr_len;
	u16 tso_max_payload_num_segs;
	u16 tso_max_frames;
	unsigned int tso_max_payload_len;
};

#define efx_ef100_has_cap(caps, flag) \
	(!!((caps) & BIT_ULL(MC_CMD_GET_CAPABILITIES_V4_OUT_ ## flag ## _LBN)))

int efx_ef100_init_datapath_caps(struct efx_nic *efx);
int ef100_phy_probe(struct efx_nic *efx);
int ef100_filter_table_probe(struct efx_nic *efx);

int ef100_get_mac_address(struct efx_nic *efx, u8 *mac_address,
			  int client_handle, bool empty_ok);
int efx_ef100_lookup_client_id(struct efx_nic *efx, efx_qword_t pciefn, u32 *id);
#endif	/* EFX_EF100_NIC_H */
