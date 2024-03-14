/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2006 Intel Corp.
 *     Tom Long Nguyen (tom.l.nguyen@intel.com)
 *     Zhang Yanmin (yanmin.zhang@intel.com)
 */

#ifndef _AER_H_
#define _AER_H_

#include <linux/errno.h>
#include <linux/types.h>

#define AER_NONFATAL			0
#define AER_FATAL			1
#define AER_CORRECTABLE			2
#define DPC_FATAL			3

struct pci_dev;

struct aer_header_log_regs {
	unsigned int dw0;
	unsigned int dw1;
	unsigned int dw2;
	unsigned int dw3;
};

struct aer_capability_regs {
	u32 header;
	u32 uncor_status;
	u32 uncor_mask;
	u32 uncor_severity;
	u32 cor_status;
	u32 cor_mask;
	u32 cap_control;
	struct aer_header_log_regs header_log;
	u32 root_command;
	u32 root_status;
	u16 cor_err_source;
	u16 uncor_err_source;
};

#if defined(CONFIG_PCIEAER)
int pci_aer_clear_nonfatal_status(struct pci_dev *dev);
int pcie_aer_is_native(struct pci_dev *dev);
#else
static inline int pci_aer_clear_nonfatal_status(struct pci_dev *dev)
{
	return -EINVAL;
}
static inline int pcie_aer_is_native(struct pci_dev *dev) { return 0; }
#endif

void cper_print_aer(struct pci_dev *dev, int aer_severity,
		    struct aer_capability_regs *aer);
int cper_severity_to_aer(int cper_severity);
void aer_recover_queue(int domain, unsigned int bus, unsigned int devfn,
		       int severity, struct aer_capability_regs *aer_regs);
#endif //_AER_H_

