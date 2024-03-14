// SPDX-License-Identifier: GPL-2.0
/*
 * 8250 PCI library.
 *
 * Copyright (C) 2001 Russell King, All Rights Reserved.
 */
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/types.h>

#include "8250.h"
#include "8250_pcilib.h"

int serial8250_pci_setup_port(struct pci_dev *dev, struct uart_8250_port *port,
		   u8 bar, unsigned int offset, int regshift)
{
	if (bar >= PCI_STD_NUM_BARS)
		return -EINVAL;

	if (pci_resource_flags(dev, bar) & IORESOURCE_MEM) {
		if (!pcim_iomap(dev, bar, 0) && !pcim_iomap_table(dev))
			return -ENOMEM;

		port->port.iotype = UPIO_MEM;
		port->port.iobase = 0;
		port->port.mapbase = pci_resource_start(dev, bar) + offset;
		port->port.membase = pcim_iomap_table(dev)[bar] + offset;
		port->port.regshift = regshift;
	} else {
		port->port.iotype = UPIO_PORT;
		port->port.iobase = pci_resource_start(dev, bar) + offset;
		port->port.mapbase = 0;
		port->port.membase = NULL;
		port->port.regshift = 0;
	}
	return 0;
}
EXPORT_SYMBOL_NS_GPL(serial8250_pci_setup_port, SERIAL_8250_PCI);
MODULE_LICENSE("GPL");
