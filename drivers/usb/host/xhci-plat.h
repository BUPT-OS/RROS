/* SPDX-License-Identifier: GPL-2.0 */
/*
 * xhci-plat.h - xHCI host controller driver platform Bus Glue.
 *
 * Copyright (C) 2015 Renesas Electronics Corporation
 */

#ifndef _XHCI_PLAT_H
#define _XHCI_PLAT_H

#include "xhci.h"	/* for hcd_to_xhci() */

struct xhci_plat_priv {
	const char *firmware_name;
	unsigned long long quirks;
	void (*plat_start)(struct usb_hcd *);
	int (*init_quirk)(struct usb_hcd *);
	int (*suspend_quirk)(struct usb_hcd *);
	int (*resume_quirk)(struct usb_hcd *);
};

#define hcd_to_xhci_priv(h) ((struct xhci_plat_priv *)hcd_to_xhci(h)->priv)
#define xhci_to_priv(x) ((struct xhci_plat_priv *)(x)->priv)

int xhci_plat_probe(struct platform_device *pdev, struct device *sysdev,
		    const struct xhci_plat_priv *priv_match);

void xhci_plat_remove(struct platform_device *dev);
extern const struct dev_pm_ops xhci_plat_pm_ops;

#endif	/* _XHCI_PLAT_H */
