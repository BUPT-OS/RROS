// SPDX-License-Identifier: GPL-2.0+
/*
 * Compaq Hot Plug Controller Driver
 *
 * Copyright (c) 1995,2001 Compaq Computer Corporation
 * Copyright (c) 2001,2003 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (c) 2001 IBM Corp.
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/pci.h>
#include "shpchp.h"


/* A few routines that create sysfs entries for the hot plug controller */

static ssize_t show_ctrl(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev;
	struct resource *res;
	struct pci_bus *bus;
	size_t len = 0;
	int busnr;

	pdev = to_pci_dev(dev);
	bus = pdev->subordinate;

	len += sysfs_emit_at(buf, len, "Free resources: memory\n");
	pci_bus_for_each_resource(bus, res) {
		if (res && (res->flags & IORESOURCE_MEM) &&
				!(res->flags & IORESOURCE_PREFETCH)) {
			len += sysfs_emit_at(buf, len,
					     "start = %8.8llx, length = %8.8llx\n",
					     (unsigned long long)res->start,
					     (unsigned long long)resource_size(res));
		}
	}
	len += sysfs_emit_at(buf, len, "Free resources: prefetchable memory\n");
	pci_bus_for_each_resource(bus, res) {
		if (res && (res->flags & IORESOURCE_MEM) &&
			       (res->flags & IORESOURCE_PREFETCH)) {
			len += sysfs_emit_at(buf, len,
					     "start = %8.8llx, length = %8.8llx\n",
					     (unsigned long long)res->start,
					     (unsigned long long)resource_size(res));
		}
	}
	len += sysfs_emit_at(buf, len, "Free resources: IO\n");
	pci_bus_for_each_resource(bus, res) {
		if (res && (res->flags & IORESOURCE_IO)) {
			len += sysfs_emit_at(buf, len,
					     "start = %8.8llx, length = %8.8llx\n",
					     (unsigned long long)res->start,
					     (unsigned long long)resource_size(res));
		}
	}
	len += sysfs_emit_at(buf, len, "Free resources: bus numbers\n");
	for (busnr = bus->busn_res.start; busnr <= bus->busn_res.end; busnr++) {
		if (!pci_find_bus(pci_domain_nr(bus), busnr))
			break;
	}
	if (busnr < bus->busn_res.end)
		len += sysfs_emit_at(buf, len,
				     "start = %8.8x, length = %8.8x\n",
				     busnr, (int)(bus->busn_res.end - busnr));

	return len;
}
static DEVICE_ATTR(ctrl, S_IRUGO, show_ctrl, NULL);

int shpchp_create_ctrl_files(struct controller *ctrl)
{
	return device_create_file(&ctrl->pci_dev->dev, &dev_attr_ctrl);
}

void shpchp_remove_ctrl_files(struct controller *ctrl)
{
	device_remove_file(&ctrl->pci_dev->dev, &dev_attr_ctrl);
}
