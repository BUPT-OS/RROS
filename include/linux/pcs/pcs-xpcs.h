/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare XPCS helpers
 */

#ifndef __LINUX_PCS_XPCS_H
#define __LINUX_PCS_XPCS_H

#include <linux/phy.h>
#include <linux/phylink.h>

#define NXP_SJA1105_XPCS_ID		0x00000010
#define NXP_SJA1110_XPCS_ID		0x00000020

/* AN mode */
#define DW_AN_C73			1
#define DW_AN_C37_SGMII			2
#define DW_2500BASEX			3
#define DW_AN_C37_1000BASEX		4
#define DW_10GBASER			5

/* device vendor OUI */
#define DW_OUI_WX			0x0018fc80

/* dev_flag */
#define DW_DEV_TXGBE			BIT(0)

struct xpcs_id;

struct dw_xpcs {
	struct mdio_device *mdiodev;
	const struct xpcs_id *id;
	struct phylink_pcs pcs;
	phy_interface_t interface;
	int dev_flag;
};

int xpcs_get_an_mode(struct dw_xpcs *xpcs, phy_interface_t interface);
void xpcs_link_up(struct phylink_pcs *pcs, unsigned int neg_mode,
		  phy_interface_t interface, int speed, int duplex);
int xpcs_do_config(struct dw_xpcs *xpcs, phy_interface_t interface,
		   const unsigned long *advertising, unsigned int neg_mode);
void xpcs_get_interfaces(struct dw_xpcs *xpcs, unsigned long *interfaces);
int xpcs_config_eee(struct dw_xpcs *xpcs, int mult_fact_100ns,
		    int enable);
struct dw_xpcs *xpcs_create_mdiodev(struct mii_bus *bus, int addr,
				    phy_interface_t interface);
void xpcs_destroy(struct dw_xpcs *xpcs);

#endif /* __LINUX_PCS_XPCS_H */
