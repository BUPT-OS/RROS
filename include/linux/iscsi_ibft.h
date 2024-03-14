/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  Copyright 2007 Red Hat, Inc.
 *  by Peter Jones <pjones@redhat.com>
 *  Copyright 2007 IBM, Inc.
 *  by Konrad Rzeszutek <konradr@linux.vnet.ibm.com>
 *  Copyright 2008
 *  by Konrad Rzeszutek <ketuzsezr@darnok.org>
 *
 * This code exposes the iSCSI Boot Format Table to userland via sysfs.
 */

#ifndef ISCSI_IBFT_H
#define ISCSI_IBFT_H

#include <linux/types.h>

/*
 * Physical location of iSCSI Boot Format Table.
 * If the value is 0 there is no iBFT on the machine.
 */
extern phys_addr_t ibft_phys_addr;

#ifdef CONFIG_ISCSI_IBFT_FIND

/*
 * Routine used to find and reserve the iSCSI Boot Format Table. The
 * physical address is set in the ibft_phys_addr variable.
 */
void reserve_ibft_region(void);

/*
 * Physical bounds to search for the iSCSI Boot Format Table.
 */
#define IBFT_START 0x80000 /* 512kB */
#define IBFT_END 0x100000 /* 1MB */

#else
static inline void reserve_ibft_region(void) {}
#endif

#endif /* ISCSI_IBFT_H */
