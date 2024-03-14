// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Marvell
 *
 * Author: Thomas Petazzoni <thomas.petazzoni@bootlin.com>
 *
 * This file helps PCI controller drivers implement a fake root port
 * PCI bridge when the HW doesn't provide such a root port PCI
 * bridge.
 *
 * It emulates a PCI bridge by providing a fake PCI configuration
 * space (and optionally a PCIe capability configuration space) in
 * memory. By default the read/write operations simply read and update
 * this fake configuration space in memory. However, PCI controller
 * drivers can provide through the 'struct pci_sw_bridge_ops'
 * structure a set of operations to override or complement this
 * default behavior.
 */

#include <linux/pci.h>
#include "pci-bridge-emul.h"

#define PCI_BRIDGE_CONF_END	PCI_STD_HEADER_SIZEOF
#define PCI_CAP_SSID_SIZEOF	(PCI_SSVID_DEVICE_ID + 2)
#define PCI_CAP_PCIE_SIZEOF	(PCI_EXP_SLTSTA2 + 2)

/**
 * struct pci_bridge_reg_behavior - register bits behaviors
 * @ro:		Read-Only bits
 * @rw:		Read-Write bits
 * @w1c:	Write-1-to-Clear bits
 *
 * Reads and Writes will be filtered by specified behavior. All other bits not
 * declared are assumed 'Reserved' and will return 0 on reads, per PCIe 5.0:
 * "Reserved register fields must be read only and must return 0 (all 0's for
 * multi-bit fields) when read".
 */
struct pci_bridge_reg_behavior {
	/* Read-only bits */
	u32 ro;

	/* Read-write bits */
	u32 rw;

	/* Write-1-to-clear bits */
	u32 w1c;
};

static const
struct pci_bridge_reg_behavior pci_regs_behavior[PCI_STD_HEADER_SIZEOF / 4] = {
	[PCI_VENDOR_ID / 4] = { .ro = ~0 },
	[PCI_COMMAND / 4] = {
		.rw = (PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
		       PCI_COMMAND_MASTER | PCI_COMMAND_PARITY |
		       PCI_COMMAND_SERR),
		.ro = ((PCI_COMMAND_SPECIAL | PCI_COMMAND_INVALIDATE |
			PCI_COMMAND_VGA_PALETTE | PCI_COMMAND_WAIT |
			PCI_COMMAND_FAST_BACK) |
		       (PCI_STATUS_CAP_LIST | PCI_STATUS_66MHZ |
			PCI_STATUS_FAST_BACK | PCI_STATUS_DEVSEL_MASK) << 16),
		.w1c = PCI_STATUS_ERROR_BITS << 16,
	},
	[PCI_CLASS_REVISION / 4] = { .ro = ~0 },

	/*
	 * Cache Line Size register: implement as read-only, we do not
	 * pretend implementing "Memory Write and Invalidate"
	 * transactions"
	 *
	 * Latency Timer Register: implemented as read-only, as "A
	 * bridge that is not capable of a burst transfer of more than
	 * two data phases on its primary interface is permitted to
	 * hardwire the Latency Timer to a value of 16 or less"
	 *
	 * Header Type: always read-only
	 *
	 * BIST register: implemented as read-only, as "A bridge that
	 * does not support BIST must implement this register as a
	 * read-only register that returns 0 when read"
	 */
	[PCI_CACHE_LINE_SIZE / 4] = { .ro = ~0 },

	/*
	 * Base Address registers not used must be implemented as
	 * read-only registers that return 0 when read.
	 */
	[PCI_BASE_ADDRESS_0 / 4] = { .ro = ~0 },
	[PCI_BASE_ADDRESS_1 / 4] = { .ro = ~0 },

	[PCI_PRIMARY_BUS / 4] = {
		/* Primary, secondary and subordinate bus are RW */
		.rw = GENMASK(24, 0),
		/* Secondary latency is read-only */
		.ro = GENMASK(31, 24),
	},

	[PCI_IO_BASE / 4] = {
		/* The high four bits of I/O base/limit are RW */
		.rw = (GENMASK(15, 12) | GENMASK(7, 4)),

		/* The low four bits of I/O base/limit are RO */
		.ro = (((PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK |
			 PCI_STATUS_DEVSEL_MASK) << 16) |
		       GENMASK(11, 8) | GENMASK(3, 0)),

		.w1c = PCI_STATUS_ERROR_BITS << 16,
	},

	[PCI_MEMORY_BASE / 4] = {
		/* The high 12-bits of mem base/limit are RW */
		.rw = GENMASK(31, 20) | GENMASK(15, 4),

		/* The low four bits of mem base/limit are RO */
		.ro = GENMASK(19, 16) | GENMASK(3, 0),
	},

	[PCI_PREF_MEMORY_BASE / 4] = {
		/* The high 12-bits of pref mem base/limit are RW */
		.rw = GENMASK(31, 20) | GENMASK(15, 4),

		/* The low four bits of pref mem base/limit are RO */
		.ro = GENMASK(19, 16) | GENMASK(3, 0),
	},

	[PCI_PREF_BASE_UPPER32 / 4] = {
		.rw = ~0,
	},

	[PCI_PREF_LIMIT_UPPER32 / 4] = {
		.rw = ~0,
	},

	[PCI_IO_BASE_UPPER16 / 4] = {
		.rw = ~0,
	},

	[PCI_CAPABILITY_LIST / 4] = {
		.ro = GENMASK(7, 0),
	},

	/*
	 * If expansion ROM is unsupported then ROM Base Address register must
	 * be implemented as read-only register that return 0 when read, same
	 * as for unused Base Address registers.
	 */
	[PCI_ROM_ADDRESS1 / 4] = {
		.ro = ~0,
	},

	/*
	 * Interrupt line (bits 7:0) are RW, interrupt pin (bits 15:8)
	 * are RO, and bridge control (31:16) are a mix of RW, RO,
	 * reserved and W1C bits
	 */
	[PCI_INTERRUPT_LINE / 4] = {
		/* Interrupt line is RW */
		.rw = (GENMASK(7, 0) |
		       ((PCI_BRIDGE_CTL_PARITY |
			 PCI_BRIDGE_CTL_SERR |
			 PCI_BRIDGE_CTL_ISA |
			 PCI_BRIDGE_CTL_VGA |
			 PCI_BRIDGE_CTL_MASTER_ABORT |
			 PCI_BRIDGE_CTL_BUS_RESET |
			 BIT(8) | BIT(9) | BIT(11)) << 16)),

		/* Interrupt pin is RO */
		.ro = (GENMASK(15, 8) | ((PCI_BRIDGE_CTL_FAST_BACK) << 16)),

		.w1c = BIT(10) << 16,
	},
};

static const
struct pci_bridge_reg_behavior pcie_cap_regs_behavior[PCI_CAP_PCIE_SIZEOF / 4] = {
	[PCI_CAP_LIST_ID / 4] = {
		/*
		 * Capability ID, Next Capability Pointer and
		 * bits [14:0] of Capabilities register are all read-only.
		 * Bit 15 of Capabilities register is reserved.
		 */
		.ro = GENMASK(30, 0),
	},

	[PCI_EXP_DEVCAP / 4] = {
		/*
		 * Bits [31:29] and [17:16] are reserved.
		 * Bits [27:18] are reserved for non-upstream ports.
		 * Bits 28 and [14:6] are reserved for non-endpoint devices.
		 * Other bits are read-only.
		 */
		.ro = BIT(15) | GENMASK(5, 0),
	},

	[PCI_EXP_DEVCTL / 4] = {
		/*
		 * Device control register is RW, except bit 15 which is
		 * reserved for non-endpoints or non-PCIe-to-PCI/X bridges.
		 */
		.rw = GENMASK(14, 0),

		/*
		 * Device status register has bits 6 and [3:0] W1C, [5:4] RO,
		 * the rest is reserved. Also bit 6 is reserved for non-upstream
		 * ports.
		 */
		.w1c = GENMASK(3, 0) << 16,
		.ro = GENMASK(5, 4) << 16,
	},

	[PCI_EXP_LNKCAP / 4] = {
		/*
		 * All bits are RO, except bit 23 which is reserved and
		 * bit 18 which is reserved for non-upstream ports.
		 */
		.ro = lower_32_bits(~(BIT(23) | PCI_EXP_LNKCAP_CLKPM)),
	},

	[PCI_EXP_LNKCTL / 4] = {
		/*
		 * Link control has bits [15:14], [11:3] and [1:0] RW, the
		 * rest is reserved. Bit 8 is reserved for non-upstream ports.
		 *
		 * Link status has bits [13:0] RO, and bits [15:14]
		 * W1C.
		 */
		.rw = GENMASK(15, 14) | GENMASK(11, 9) | GENMASK(7, 3) | GENMASK(1, 0),
		.ro = GENMASK(13, 0) << 16,
		.w1c = GENMASK(15, 14) << 16,
	},

	[PCI_EXP_SLTCAP / 4] = {
		.ro = ~0,
	},

	[PCI_EXP_SLTCTL / 4] = {
		/*
		 * Slot control has bits [14:0] RW, the rest is
		 * reserved.
		 *
		 * Slot status has bits 8 and [4:0] W1C, bits [7:5] RO, the
		 * rest is reserved.
		 */
		.rw = GENMASK(14, 0),
		.w1c = (PCI_EXP_SLTSTA_ABP | PCI_EXP_SLTSTA_PFD |
			PCI_EXP_SLTSTA_MRLSC | PCI_EXP_SLTSTA_PDC |
			PCI_EXP_SLTSTA_CC | PCI_EXP_SLTSTA_DLLSC) << 16,
		.ro = (PCI_EXP_SLTSTA_MRLSS | PCI_EXP_SLTSTA_PDS |
		       PCI_EXP_SLTSTA_EIS) << 16,
	},

	[PCI_EXP_RTCTL / 4] = {
		/*
		 * Root control has bits [4:0] RW, the rest is
		 * reserved.
		 *
		 * Root capabilities has bit 0 RO, the rest is reserved.
		 */
		.rw = (PCI_EXP_RTCTL_SECEE | PCI_EXP_RTCTL_SENFEE |
		       PCI_EXP_RTCTL_SEFEE | PCI_EXP_RTCTL_PMEIE |
		       PCI_EXP_RTCTL_CRSSVE),
		.ro = PCI_EXP_RTCAP_CRSVIS << 16,
	},

	[PCI_EXP_RTSTA / 4] = {
		/*
		 * Root status has bits 17 and [15:0] RO, bit 16 W1C, the rest
		 * is reserved.
		 */
		.ro = GENMASK(15, 0) | PCI_EXP_RTSTA_PENDING,
		.w1c = PCI_EXP_RTSTA_PME,
	},

	[PCI_EXP_DEVCAP2 / 4] = {
		/*
		 * Device capabilities 2 register has reserved bits [30:27].
		 * Also bits [26:24] are reserved for non-upstream ports.
		 */
		.ro = BIT(31) | GENMASK(23, 0),
	},

	[PCI_EXP_DEVCTL2 / 4] = {
		/*
		 * Device control 2 register is RW. Bit 11 is reserved for
		 * non-upstream ports.
		 *
		 * Device status 2 register is reserved.
		 */
		.rw = GENMASK(15, 12) | GENMASK(10, 0),
	},

	[PCI_EXP_LNKCAP2 / 4] = {
		/* Link capabilities 2 register has reserved bits [30:25] and 0. */
		.ro = BIT(31) | GENMASK(24, 1),
	},

	[PCI_EXP_LNKCTL2 / 4] = {
		/*
		 * Link control 2 register is RW.
		 *
		 * Link status 2 register has bits 5, 15 W1C;
		 * bits 10, 11 reserved and others are RO.
		 */
		.rw = GENMASK(15, 0),
		.w1c = (BIT(15) | BIT(5)) << 16,
		.ro = (GENMASK(14, 12) | GENMASK(9, 6) | GENMASK(4, 0)) << 16,
	},

	[PCI_EXP_SLTCAP2 / 4] = {
		/* Slot capabilities 2 register is reserved. */
	},

	[PCI_EXP_SLTCTL2 / 4] = {
		/* Both Slot control 2 and Slot status 2 registers are reserved. */
	},
};

static pci_bridge_emul_read_status_t
pci_bridge_emul_read_ssid(struct pci_bridge_emul *bridge, int reg, u32 *value)
{
	switch (reg) {
	case PCI_CAP_LIST_ID:
		*value = PCI_CAP_ID_SSVID |
			((bridge->pcie_start > bridge->ssid_start) ? (bridge->pcie_start << 8) : 0);
		return PCI_BRIDGE_EMUL_HANDLED;

	case PCI_SSVID_VENDOR_ID:
		*value = bridge->subsystem_vendor_id |
			(bridge->subsystem_id << 16);
		return PCI_BRIDGE_EMUL_HANDLED;

	default:
		return PCI_BRIDGE_EMUL_NOT_HANDLED;
	}
}

/*
 * Initialize a pci_bridge_emul structure to represent a fake PCI
 * bridge configuration space. The caller needs to have initialized
 * the PCI configuration space with whatever values make sense
 * (typically at least vendor, device, revision), the ->ops pointer,
 * and optionally ->data and ->has_pcie.
 */
int pci_bridge_emul_init(struct pci_bridge_emul *bridge,
			 unsigned int flags)
{
	BUILD_BUG_ON(sizeof(bridge->conf) != PCI_BRIDGE_CONF_END);

	/*
	 * class_revision: Class is high 24 bits and revision is low 8 bit
	 * of this member, while class for PCI Bridge Normal Decode has the
	 * 24-bit value: PCI_CLASS_BRIDGE_PCI_NORMAL
	 */
	bridge->conf.class_revision |=
		cpu_to_le32(PCI_CLASS_BRIDGE_PCI_NORMAL << 8);
	bridge->conf.header_type = PCI_HEADER_TYPE_BRIDGE;
	bridge->conf.cache_line_size = 0x10;
	bridge->conf.status = cpu_to_le16(PCI_STATUS_CAP_LIST);
	bridge->pci_regs_behavior = kmemdup(pci_regs_behavior,
					    sizeof(pci_regs_behavior),
					    GFP_KERNEL);
	if (!bridge->pci_regs_behavior)
		return -ENOMEM;

	/* If ssid_start and pcie_start were not specified then choose the lowest possible value. */
	if (!bridge->ssid_start && !bridge->pcie_start) {
		if (bridge->subsystem_vendor_id)
			bridge->ssid_start = PCI_BRIDGE_CONF_END;
		if (bridge->has_pcie)
			bridge->pcie_start = bridge->ssid_start + PCI_CAP_SSID_SIZEOF;
	} else if (!bridge->ssid_start && bridge->subsystem_vendor_id) {
		if (bridge->pcie_start - PCI_BRIDGE_CONF_END >= PCI_CAP_SSID_SIZEOF)
			bridge->ssid_start = PCI_BRIDGE_CONF_END;
		else
			bridge->ssid_start = bridge->pcie_start + PCI_CAP_PCIE_SIZEOF;
	} else if (!bridge->pcie_start && bridge->has_pcie) {
		if (bridge->ssid_start - PCI_BRIDGE_CONF_END >= PCI_CAP_PCIE_SIZEOF)
			bridge->pcie_start = PCI_BRIDGE_CONF_END;
		else
			bridge->pcie_start = bridge->ssid_start + PCI_CAP_SSID_SIZEOF;
	}

	bridge->conf.capabilities_pointer = min(bridge->ssid_start, bridge->pcie_start);

	if (bridge->conf.capabilities_pointer)
		bridge->conf.status |= cpu_to_le16(PCI_STATUS_CAP_LIST);

	if (bridge->has_pcie) {
		bridge->pcie_conf.cap_id = PCI_CAP_ID_EXP;
		bridge->pcie_conf.next = (bridge->ssid_start > bridge->pcie_start) ?
					 bridge->ssid_start : 0;
		bridge->pcie_conf.cap |= cpu_to_le16(PCI_EXP_TYPE_ROOT_PORT << 4);
		bridge->pcie_cap_regs_behavior =
			kmemdup(pcie_cap_regs_behavior,
				sizeof(pcie_cap_regs_behavior),
				GFP_KERNEL);
		if (!bridge->pcie_cap_regs_behavior) {
			kfree(bridge->pci_regs_behavior);
			return -ENOMEM;
		}
		/* These bits are applicable only for PCI and reserved on PCIe */
		bridge->pci_regs_behavior[PCI_CACHE_LINE_SIZE / 4].ro &=
			~GENMASK(15, 8);
		bridge->pci_regs_behavior[PCI_COMMAND / 4].ro &=
			~((PCI_COMMAND_SPECIAL | PCI_COMMAND_INVALIDATE |
			   PCI_COMMAND_VGA_PALETTE | PCI_COMMAND_WAIT |
			   PCI_COMMAND_FAST_BACK) |
			  (PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK |
			   PCI_STATUS_DEVSEL_MASK) << 16);
		bridge->pci_regs_behavior[PCI_PRIMARY_BUS / 4].ro &=
			~GENMASK(31, 24);
		bridge->pci_regs_behavior[PCI_IO_BASE / 4].ro &=
			~((PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK |
			   PCI_STATUS_DEVSEL_MASK) << 16);
		bridge->pci_regs_behavior[PCI_INTERRUPT_LINE / 4].rw &=
			~((PCI_BRIDGE_CTL_MASTER_ABORT |
			   BIT(8) | BIT(9) | BIT(11)) << 16);
		bridge->pci_regs_behavior[PCI_INTERRUPT_LINE / 4].ro &=
			~((PCI_BRIDGE_CTL_FAST_BACK) << 16);
		bridge->pci_regs_behavior[PCI_INTERRUPT_LINE / 4].w1c &=
			~(BIT(10) << 16);
	}

	if (flags & PCI_BRIDGE_EMUL_NO_PREFMEM_FORWARD) {
		bridge->pci_regs_behavior[PCI_PREF_MEMORY_BASE / 4].ro = ~0;
		bridge->pci_regs_behavior[PCI_PREF_MEMORY_BASE / 4].rw = 0;
	}

	if (flags & PCI_BRIDGE_EMUL_NO_IO_FORWARD) {
		bridge->pci_regs_behavior[PCI_COMMAND / 4].ro |= PCI_COMMAND_IO;
		bridge->pci_regs_behavior[PCI_COMMAND / 4].rw &= ~PCI_COMMAND_IO;
		bridge->pci_regs_behavior[PCI_IO_BASE / 4].ro |= GENMASK(15, 0);
		bridge->pci_regs_behavior[PCI_IO_BASE / 4].rw &= ~GENMASK(15, 0);
		bridge->pci_regs_behavior[PCI_IO_BASE_UPPER16 / 4].ro = ~0;
		bridge->pci_regs_behavior[PCI_IO_BASE_UPPER16 / 4].rw = 0;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_init);

/*
 * Cleanup a pci_bridge_emul structure that was previously initialized
 * using pci_bridge_emul_init().
 */
void pci_bridge_emul_cleanup(struct pci_bridge_emul *bridge)
{
	if (bridge->has_pcie)
		kfree(bridge->pcie_cap_regs_behavior);
	kfree(bridge->pci_regs_behavior);
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_cleanup);

/*
 * Should be called by the PCI controller driver when reading the PCI
 * configuration space of the fake bridge. It will call back the
 * ->ops->read_base or ->ops->read_pcie operations.
 */
int pci_bridge_emul_conf_read(struct pci_bridge_emul *bridge, int where,
			      int size, u32 *value)
{
	int ret;
	int reg = where & ~3;
	pci_bridge_emul_read_status_t (*read_op)(struct pci_bridge_emul *bridge,
						 int reg, u32 *value);
	__le32 *cfgspace;
	const struct pci_bridge_reg_behavior *behavior;

	if (reg < PCI_BRIDGE_CONF_END) {
		/* Emulated PCI space */
		read_op = bridge->ops->read_base;
		cfgspace = (__le32 *) &bridge->conf;
		behavior = bridge->pci_regs_behavior;
	} else if (reg >= bridge->ssid_start && reg < bridge->ssid_start + PCI_CAP_SSID_SIZEOF &&
		   bridge->subsystem_vendor_id) {
		/* Emulated PCI Bridge Subsystem Vendor ID capability */
		reg -= bridge->ssid_start;
		read_op = pci_bridge_emul_read_ssid;
		cfgspace = NULL;
		behavior = NULL;
	} else if (reg >= bridge->pcie_start && reg < bridge->pcie_start + PCI_CAP_PCIE_SIZEOF &&
		   bridge->has_pcie) {
		/* Our emulated PCIe capability */
		reg -= bridge->pcie_start;
		read_op = bridge->ops->read_pcie;
		cfgspace = (__le32 *) &bridge->pcie_conf;
		behavior = bridge->pcie_cap_regs_behavior;
	} else if (reg >= PCI_CFG_SPACE_SIZE && bridge->has_pcie) {
		/* PCIe extended capability space */
		reg -= PCI_CFG_SPACE_SIZE;
		read_op = bridge->ops->read_ext;
		cfgspace = NULL;
		behavior = NULL;
	} else {
		/* Not implemented */
		*value = 0;
		return PCIBIOS_SUCCESSFUL;
	}

	if (read_op)
		ret = read_op(bridge, reg, value);
	else
		ret = PCI_BRIDGE_EMUL_NOT_HANDLED;

	if (ret == PCI_BRIDGE_EMUL_NOT_HANDLED) {
		if (cfgspace)
			*value = le32_to_cpu(cfgspace[reg / 4]);
		else
			*value = 0;
	}

	/*
	 * Make sure we never return any reserved bit with a value
	 * different from 0.
	 */
	if (behavior)
		*value &= behavior[reg / 4].ro | behavior[reg / 4].rw |
			  behavior[reg / 4].w1c;

	if (size == 1)
		*value = (*value >> (8 * (where & 3))) & 0xff;
	else if (size == 2)
		*value = (*value >> (8 * (where & 3))) & 0xffff;
	else if (size != 4)
		return PCIBIOS_BAD_REGISTER_NUMBER;

	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_conf_read);

/*
 * Should be called by the PCI controller driver when writing the PCI
 * configuration space of the fake bridge. It will call back the
 * ->ops->write_base or ->ops->write_pcie operations.
 */
int pci_bridge_emul_conf_write(struct pci_bridge_emul *bridge, int where,
			       int size, u32 value)
{
	int reg = where & ~3;
	int mask, ret, old, new, shift;
	void (*write_op)(struct pci_bridge_emul *bridge, int reg,
			 u32 old, u32 new, u32 mask);
	__le32 *cfgspace;
	const struct pci_bridge_reg_behavior *behavior;

	ret = pci_bridge_emul_conf_read(bridge, reg, 4, &old);
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;

	if (reg < PCI_BRIDGE_CONF_END) {
		/* Emulated PCI space */
		write_op = bridge->ops->write_base;
		cfgspace = (__le32 *) &bridge->conf;
		behavior = bridge->pci_regs_behavior;
	} else if (reg >= bridge->pcie_start && reg < bridge->pcie_start + PCI_CAP_PCIE_SIZEOF &&
		   bridge->has_pcie) {
		/* Our emulated PCIe capability */
		reg -= bridge->pcie_start;
		write_op = bridge->ops->write_pcie;
		cfgspace = (__le32 *) &bridge->pcie_conf;
		behavior = bridge->pcie_cap_regs_behavior;
	} else if (reg >= PCI_CFG_SPACE_SIZE && bridge->has_pcie) {
		/* PCIe extended capability space */
		reg -= PCI_CFG_SPACE_SIZE;
		write_op = bridge->ops->write_ext;
		cfgspace = NULL;
		behavior = NULL;
	} else {
		/* Not implemented */
		return PCIBIOS_SUCCESSFUL;
	}

	shift = (where & 0x3) * 8;

	if (size == 4)
		mask = 0xffffffff;
	else if (size == 2)
		mask = 0xffff << shift;
	else if (size == 1)
		mask = 0xff << shift;
	else
		return PCIBIOS_BAD_REGISTER_NUMBER;

	if (behavior) {
		/* Keep all bits, except the RW bits */
		new = old & (~mask | ~behavior[reg / 4].rw);

		/* Update the value of the RW bits */
		new |= (value << shift) & (behavior[reg / 4].rw & mask);

		/* Clear the W1C bits */
		new &= ~((value << shift) & (behavior[reg / 4].w1c & mask));
	} else {
		new = old & ~mask;
		new |= (value << shift) & mask;
	}

	if (cfgspace) {
		/* Save the new value with the cleared W1C bits into the cfgspace */
		cfgspace[reg / 4] = cpu_to_le32(new);
	}

	if (behavior) {
		/*
		 * Clear the W1C bits not specified by the write mask, so that the
		 * write_op() does not clear them.
		 */
		new &= ~(behavior[reg / 4].w1c & ~mask);

		/*
		 * Set the W1C bits specified by the write mask, so that write_op()
		 * knows about that they are to be cleared.
		 */
		new |= (value << shift) & (behavior[reg / 4].w1c & mask);
	}

	if (write_op)
		write_op(bridge, reg, old, new, mask);

	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL_GPL(pci_bridge_emul_conf_write);
