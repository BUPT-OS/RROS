// SPDX-License-Identifier: GPL-2.0-only
/*
 * PRU-ICSS remoteproc driver for various TI SoCs
 *
 * Copyright (C) 2014-2022 Texas Instruments Incorporated - https://www.ti.com/
 *
 * Author(s):
 *	Suman Anna <s-anna@ti.com>
 *	Andrew F. Davis <afd@ti.com>
 *	Grzegorz Jaszczyk <grzegorz.jaszczyk@linaro.org> for Texas Instruments
 *	Puranjay Mohan <p-mohan@ti.com>
 *	Md Danish Anwar <danishanwar@ti.com>
 */

#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/remoteproc/pruss.h>
#include <linux/pruss_driver.h>
#include <linux/remoteproc.h>

#include "remoteproc_internal.h"
#include "remoteproc_elf_helpers.h"
#include "pru_rproc.h"

/* PRU_ICSS_PRU_CTRL registers */
#define PRU_CTRL_CTRL		0x0000
#define PRU_CTRL_STS		0x0004
#define PRU_CTRL_WAKEUP_EN	0x0008
#define PRU_CTRL_CYCLE		0x000C
#define PRU_CTRL_STALL		0x0010
#define PRU_CTRL_CTBIR0		0x0020
#define PRU_CTRL_CTBIR1		0x0024
#define PRU_CTRL_CTPPR0		0x0028
#define PRU_CTRL_CTPPR1		0x002C

/* CTRL register bit-fields */
#define CTRL_CTRL_SOFT_RST_N	BIT(0)
#define CTRL_CTRL_EN		BIT(1)
#define CTRL_CTRL_SLEEPING	BIT(2)
#define CTRL_CTRL_CTR_EN	BIT(3)
#define CTRL_CTRL_SINGLE_STEP	BIT(8)
#define CTRL_CTRL_RUNSTATE	BIT(15)

/* PRU_ICSS_PRU_DEBUG registers */
#define PRU_DEBUG_GPREG(x)	(0x0000 + (x) * 4)
#define PRU_DEBUG_CT_REG(x)	(0x0080 + (x) * 4)

/* PRU/RTU/Tx_PRU Core IRAM address masks */
#define PRU_IRAM_ADDR_MASK	0x3ffff
#define PRU0_IRAM_ADDR_MASK	0x34000
#define PRU1_IRAM_ADDR_MASK	0x38000
#define RTU0_IRAM_ADDR_MASK	0x4000
#define RTU1_IRAM_ADDR_MASK	0x6000
#define TX_PRU0_IRAM_ADDR_MASK	0xa000
#define TX_PRU1_IRAM_ADDR_MASK	0xc000

/* PRU device addresses for various type of PRU RAMs */
#define PRU_IRAM_DA	0	/* Instruction RAM */
#define PRU_PDRAM_DA	0	/* Primary Data RAM */
#define PRU_SDRAM_DA	0x2000	/* Secondary Data RAM */
#define PRU_SHRDRAM_DA	0x10000 /* Shared Data RAM */

#define MAX_PRU_SYS_EVENTS 160

/**
 * enum pru_iomem - PRU core memory/register range identifiers
 *
 * @PRU_IOMEM_IRAM: PRU Instruction RAM range
 * @PRU_IOMEM_CTRL: PRU Control register range
 * @PRU_IOMEM_DEBUG: PRU Debug register range
 * @PRU_IOMEM_MAX: just keep this one at the end
 */
enum pru_iomem {
	PRU_IOMEM_IRAM = 0,
	PRU_IOMEM_CTRL,
	PRU_IOMEM_DEBUG,
	PRU_IOMEM_MAX,
};

/**
 * struct pru_private_data - device data for a PRU core
 * @type: type of the PRU core (PRU, RTU, Tx_PRU)
 * @is_k3: flag used to identify the need for special load handling
 */
struct pru_private_data {
	enum pru_type type;
	unsigned int is_k3 : 1;
};

/**
 * struct pru_rproc - PRU remoteproc structure
 * @id: id of the PRU core within the PRUSS
 * @dev: PRU core device pointer
 * @pruss: back-reference to parent PRUSS structure
 * @rproc: remoteproc pointer for this PRU core
 * @data: PRU core specific data
 * @mem_regions: data for each of the PRU memory regions
 * @client_np: client device node
 * @lock: mutex to protect client usage
 * @fw_name: name of firmware image used during loading
 * @mapped_irq: virtual interrupt numbers of created fw specific mapping
 * @pru_interrupt_map: pointer to interrupt mapping description (firmware)
 * @pru_interrupt_map_sz: pru_interrupt_map size
 * @rmw_lock: lock for read, modify, write operations on registers
 * @dbg_single_step: debug state variable to set PRU into single step mode
 * @dbg_continuous: debug state variable to restore PRU execution mode
 * @evt_count: number of mapped events
 * @gpmux_save: saved value for gpmux config
 */
struct pru_rproc {
	int id;
	struct device *dev;
	struct pruss *pruss;
	struct rproc *rproc;
	const struct pru_private_data *data;
	struct pruss_mem_region mem_regions[PRU_IOMEM_MAX];
	struct device_node *client_np;
	struct mutex lock;
	const char *fw_name;
	unsigned int *mapped_irq;
	struct pru_irq_rsc *pru_interrupt_map;
	size_t pru_interrupt_map_sz;
	spinlock_t rmw_lock;
	u32 dbg_single_step;
	u32 dbg_continuous;
	u8 evt_count;
	u8 gpmux_save;
};

static inline u32 pru_control_read_reg(struct pru_rproc *pru, unsigned int reg)
{
	return readl_relaxed(pru->mem_regions[PRU_IOMEM_CTRL].va + reg);
}

static inline
void pru_control_write_reg(struct pru_rproc *pru, unsigned int reg, u32 val)
{
	writel_relaxed(val, pru->mem_regions[PRU_IOMEM_CTRL].va + reg);
}

static inline
void pru_control_set_reg(struct pru_rproc *pru, unsigned int reg,
			 u32 mask, u32 set)
{
	u32 val;
	unsigned long flags;

	spin_lock_irqsave(&pru->rmw_lock, flags);

	val = pru_control_read_reg(pru, reg);
	val &= ~mask;
	val |= (set & mask);
	pru_control_write_reg(pru, reg, val);

	spin_unlock_irqrestore(&pru->rmw_lock, flags);
}

/**
 * pru_rproc_set_firmware() - set firmware for a PRU core
 * @rproc: the rproc instance of the PRU
 * @fw_name: the new firmware name, or NULL if default is desired
 *
 * Return: 0 on success, or errno in error case.
 */
static int pru_rproc_set_firmware(struct rproc *rproc, const char *fw_name)
{
	struct pru_rproc *pru = rproc->priv;

	if (!fw_name)
		fw_name = pru->fw_name;

	return rproc_set_firmware(rproc, fw_name);
}

static struct rproc *__pru_rproc_get(struct device_node *np, int index)
{
	struct rproc *rproc;
	phandle rproc_phandle;
	int ret;

	ret = of_property_read_u32_index(np, "ti,prus", index, &rproc_phandle);
	if (ret)
		return ERR_PTR(ret);

	rproc = rproc_get_by_phandle(rproc_phandle);
	if (!rproc) {
		ret = -EPROBE_DEFER;
		return ERR_PTR(ret);
	}

	/* make sure it is PRU rproc */
	if (!is_pru_rproc(rproc->dev.parent)) {
		rproc_put(rproc);
		return ERR_PTR(-ENODEV);
	}

	return rproc;
}

/**
 * pru_rproc_get() - get the PRU rproc instance from a device node
 * @np: the user/client device node
 * @index: index to use for the ti,prus property
 * @pru_id: optional pointer to return the PRU remoteproc processor id
 *
 * This function looks through a client device node's "ti,prus" property at
 * index @index and returns the rproc handle for a valid PRU remote processor if
 * found. The function allows only one user to own the PRU rproc resource at a
 * time. Caller must call pru_rproc_put() when done with using the rproc, not
 * required if the function returns a failure.
 *
 * When optional @pru_id pointer is passed the PRU remoteproc processor id is
 * returned.
 *
 * Return: rproc handle on success, and an ERR_PTR on failure using one
 * of the following error values
 *    -ENODEV if device is not found
 *    -EBUSY if PRU is already acquired by anyone
 *    -EPROBE_DEFER is PRU device is not probed yet
 */
struct rproc *pru_rproc_get(struct device_node *np, int index,
			    enum pruss_pru_id *pru_id)
{
	struct rproc *rproc;
	struct pru_rproc *pru;
	struct device *dev;
	const char *fw_name;
	int ret;
	u32 mux;

	rproc = __pru_rproc_get(np, index);
	if (IS_ERR(rproc))
		return rproc;

	pru = rproc->priv;
	dev = &rproc->dev;

	mutex_lock(&pru->lock);

	if (pru->client_np) {
		mutex_unlock(&pru->lock);
		ret = -EBUSY;
		goto err_no_rproc_handle;
	}

	pru->client_np = np;
	rproc->sysfs_read_only = true;

	mutex_unlock(&pru->lock);

	if (pru_id)
		*pru_id = pru->id;

	ret = pruss_cfg_get_gpmux(pru->pruss, pru->id, &pru->gpmux_save);
	if (ret) {
		dev_err(dev, "failed to get cfg gpmux: %d\n", ret);
		goto err;
	}

	/* An error here is acceptable for backward compatibility */
	ret = of_property_read_u32_index(np, "ti,pruss-gp-mux-sel", index,
					 &mux);
	if (!ret) {
		ret = pruss_cfg_set_gpmux(pru->pruss, pru->id, mux);
		if (ret) {
			dev_err(dev, "failed to set cfg gpmux: %d\n", ret);
			goto err;
		}
	}

	ret = of_property_read_string_index(np, "firmware-name", index,
					    &fw_name);
	if (!ret) {
		ret = pru_rproc_set_firmware(rproc, fw_name);
		if (ret) {
			dev_err(dev, "failed to set firmware: %d\n", ret);
			goto err;
		}
	}

	return rproc;

err_no_rproc_handle:
	rproc_put(rproc);
	return ERR_PTR(ret);

err:
	pru_rproc_put(rproc);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(pru_rproc_get);

/**
 * pru_rproc_put() - release the PRU rproc resource
 * @rproc: the rproc resource to release
 *
 * Releases the PRU rproc resource and makes it available to other
 * users.
 */
void pru_rproc_put(struct rproc *rproc)
{
	struct pru_rproc *pru;

	if (IS_ERR_OR_NULL(rproc) || !is_pru_rproc(rproc->dev.parent))
		return;

	pru = rproc->priv;

	pruss_cfg_set_gpmux(pru->pruss, pru->id, pru->gpmux_save);

	pru_rproc_set_firmware(rproc, NULL);

	mutex_lock(&pru->lock);

	if (!pru->client_np) {
		mutex_unlock(&pru->lock);
		return;
	}

	pru->client_np = NULL;
	rproc->sysfs_read_only = false;
	mutex_unlock(&pru->lock);

	rproc_put(rproc);
}
EXPORT_SYMBOL_GPL(pru_rproc_put);

/**
 * pru_rproc_set_ctable() - set the constant table index for the PRU
 * @rproc: the rproc instance of the PRU
 * @c: constant table index to set
 * @addr: physical address to set it to
 *
 * Return: 0 on success, or errno in error case.
 */
int pru_rproc_set_ctable(struct rproc *rproc, enum pru_ctable_idx c, u32 addr)
{
	struct pru_rproc *pru = rproc->priv;
	unsigned int reg;
	u32 mask, set;
	u16 idx;
	u16 idx_mask;

	if (IS_ERR_OR_NULL(rproc))
		return -EINVAL;

	if (!rproc->dev.parent || !is_pru_rproc(rproc->dev.parent))
		return -ENODEV;

	/* pointer is 16 bit and index is 8-bit so mask out the rest */
	idx_mask = (c >= PRU_C28) ? 0xFFFF : 0xFF;

	/* ctable uses bit 8 and upwards only */
	idx = (addr >> 8) & idx_mask;

	/* configurable ctable (i.e. C24) starts at PRU_CTRL_CTBIR0 */
	reg = PRU_CTRL_CTBIR0 + 4 * (c >> 1);
	mask = idx_mask << (16 * (c & 1));
	set = idx << (16 * (c & 1));

	pru_control_set_reg(pru, reg, mask, set);

	return 0;
}
EXPORT_SYMBOL_GPL(pru_rproc_set_ctable);

static inline u32 pru_debug_read_reg(struct pru_rproc *pru, unsigned int reg)
{
	return readl_relaxed(pru->mem_regions[PRU_IOMEM_DEBUG].va + reg);
}

static int regs_show(struct seq_file *s, void *data)
{
	struct rproc *rproc = s->private;
	struct pru_rproc *pru = rproc->priv;
	int i, nregs = 32;
	u32 pru_sts;
	int pru_is_running;

	seq_puts(s, "============== Control Registers ==============\n");
	seq_printf(s, "CTRL      := 0x%08x\n",
		   pru_control_read_reg(pru, PRU_CTRL_CTRL));
	pru_sts = pru_control_read_reg(pru, PRU_CTRL_STS);
	seq_printf(s, "STS (PC)  := 0x%08x (0x%08x)\n", pru_sts, pru_sts << 2);
	seq_printf(s, "WAKEUP_EN := 0x%08x\n",
		   pru_control_read_reg(pru, PRU_CTRL_WAKEUP_EN));
	seq_printf(s, "CYCLE     := 0x%08x\n",
		   pru_control_read_reg(pru, PRU_CTRL_CYCLE));
	seq_printf(s, "STALL     := 0x%08x\n",
		   pru_control_read_reg(pru, PRU_CTRL_STALL));
	seq_printf(s, "CTBIR0    := 0x%08x\n",
		   pru_control_read_reg(pru, PRU_CTRL_CTBIR0));
	seq_printf(s, "CTBIR1    := 0x%08x\n",
		   pru_control_read_reg(pru, PRU_CTRL_CTBIR1));
	seq_printf(s, "CTPPR0    := 0x%08x\n",
		   pru_control_read_reg(pru, PRU_CTRL_CTPPR0));
	seq_printf(s, "CTPPR1    := 0x%08x\n",
		   pru_control_read_reg(pru, PRU_CTRL_CTPPR1));

	seq_puts(s, "=============== Debug Registers ===============\n");
	pru_is_running = pru_control_read_reg(pru, PRU_CTRL_CTRL) &
				CTRL_CTRL_RUNSTATE;
	if (pru_is_running) {
		seq_puts(s, "PRU is executing, cannot print/access debug registers.\n");
		return 0;
	}

	for (i = 0; i < nregs; i++) {
		seq_printf(s, "GPREG%-2d := 0x%08x\tCT_REG%-2d := 0x%08x\n",
			   i, pru_debug_read_reg(pru, PRU_DEBUG_GPREG(i)),
			   i, pru_debug_read_reg(pru, PRU_DEBUG_CT_REG(i)));
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(regs);

/*
 * Control PRU single-step mode
 *
 * This is a debug helper function used for controlling the single-step
 * mode of the PRU. The PRU Debug registers are not accessible when the
 * PRU is in RUNNING state.
 *
 * Writing a non-zero value sets the PRU into single-step mode irrespective
 * of its previous state. The PRU mode is saved only on the first set into
 * a single-step mode. Writing a zero value will restore the PRU into its
 * original mode.
 */
static int pru_rproc_debug_ss_set(void *data, u64 val)
{
	struct rproc *rproc = data;
	struct pru_rproc *pru = rproc->priv;
	u32 reg_val;

	val = val ? 1 : 0;
	if (!val && !pru->dbg_single_step)
		return 0;

	reg_val = pru_control_read_reg(pru, PRU_CTRL_CTRL);

	if (val && !pru->dbg_single_step)
		pru->dbg_continuous = reg_val;

	if (val)
		reg_val |= CTRL_CTRL_SINGLE_STEP | CTRL_CTRL_EN;
	else
		reg_val = pru->dbg_continuous;

	pru->dbg_single_step = val;
	pru_control_write_reg(pru, PRU_CTRL_CTRL, reg_val);

	return 0;
}

static int pru_rproc_debug_ss_get(void *data, u64 *val)
{
	struct rproc *rproc = data;
	struct pru_rproc *pru = rproc->priv;

	*val = pru->dbg_single_step;

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(pru_rproc_debug_ss_fops, pru_rproc_debug_ss_get,
			 pru_rproc_debug_ss_set, "%llu\n");

/*
 * Create PRU-specific debugfs entries
 *
 * The entries are created only if the parent remoteproc debugfs directory
 * exists, and will be cleaned up by the remoteproc core.
 */
static void pru_rproc_create_debug_entries(struct rproc *rproc)
{
	if (!rproc->dbg_dir)
		return;

	debugfs_create_file("regs", 0400, rproc->dbg_dir,
			    rproc, &regs_fops);
	debugfs_create_file("single_step", 0600, rproc->dbg_dir,
			    rproc, &pru_rproc_debug_ss_fops);
}

static void pru_dispose_irq_mapping(struct pru_rproc *pru)
{
	if (!pru->mapped_irq)
		return;

	while (pru->evt_count) {
		pru->evt_count--;
		if (pru->mapped_irq[pru->evt_count] > 0)
			irq_dispose_mapping(pru->mapped_irq[pru->evt_count]);
	}

	kfree(pru->mapped_irq);
	pru->mapped_irq = NULL;
}

/*
 * Parse the custom PRU interrupt map resource and configure the INTC
 * appropriately.
 */
static int pru_handle_intrmap(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct pru_rproc *pru = rproc->priv;
	struct pru_irq_rsc *rsc = pru->pru_interrupt_map;
	struct irq_fwspec fwspec;
	struct device_node *parent, *irq_parent;
	int i, ret = 0;

	/* not having pru_interrupt_map is not an error */
	if (!rsc)
		return 0;

	/* currently supporting only type 0 */
	if (rsc->type != 0) {
		dev_err(dev, "unsupported rsc type: %d\n", rsc->type);
		return -EINVAL;
	}

	if (rsc->num_evts > MAX_PRU_SYS_EVENTS)
		return -EINVAL;

	if (sizeof(*rsc) + rsc->num_evts * sizeof(struct pruss_int_map) !=
	    pru->pru_interrupt_map_sz)
		return -EINVAL;

	pru->evt_count = rsc->num_evts;
	pru->mapped_irq = kcalloc(pru->evt_count, sizeof(unsigned int),
				  GFP_KERNEL);
	if (!pru->mapped_irq) {
		pru->evt_count = 0;
		return -ENOMEM;
	}

	/*
	 * parse and fill in system event to interrupt channel and
	 * channel-to-host mapping. The interrupt controller to be used
	 * for these mappings for a given PRU remoteproc is always its
	 * corresponding sibling PRUSS INTC node.
	 */
	parent = of_get_parent(dev_of_node(pru->dev));
	if (!parent) {
		kfree(pru->mapped_irq);
		pru->mapped_irq = NULL;
		pru->evt_count = 0;
		return -ENODEV;
	}

	irq_parent = of_get_child_by_name(parent, "interrupt-controller");
	of_node_put(parent);
	if (!irq_parent) {
		kfree(pru->mapped_irq);
		pru->mapped_irq = NULL;
		pru->evt_count = 0;
		return -ENODEV;
	}

	fwspec.fwnode = of_node_to_fwnode(irq_parent);
	fwspec.param_count = 3;
	for (i = 0; i < pru->evt_count; i++) {
		fwspec.param[0] = rsc->pru_intc_map[i].event;
		fwspec.param[1] = rsc->pru_intc_map[i].chnl;
		fwspec.param[2] = rsc->pru_intc_map[i].host;

		dev_dbg(dev, "mapping%d: event %d, chnl %d, host %d\n",
			i, fwspec.param[0], fwspec.param[1], fwspec.param[2]);

		pru->mapped_irq[i] = irq_create_fwspec_mapping(&fwspec);
		if (!pru->mapped_irq[i]) {
			dev_err(dev, "failed to get virq for fw mapping %d: event %d chnl %d host %d\n",
				i, fwspec.param[0], fwspec.param[1],
				fwspec.param[2]);
			ret = -EINVAL;
			goto map_fail;
		}
	}
	of_node_put(irq_parent);

	return ret;

map_fail:
	pru_dispose_irq_mapping(pru);
	of_node_put(irq_parent);

	return ret;
}

static int pru_rproc_start(struct rproc *rproc)
{
	struct device *dev = &rproc->dev;
	struct pru_rproc *pru = rproc->priv;
	const char *names[PRU_TYPE_MAX] = { "PRU", "RTU", "Tx_PRU" };
	u32 val;
	int ret;

	dev_dbg(dev, "starting %s%d: entry-point = 0x%llx\n",
		names[pru->data->type], pru->id, (rproc->bootaddr >> 2));

	ret = pru_handle_intrmap(rproc);
	/*
	 * reset references to pru interrupt map - they will stop being valid
	 * after rproc_start returns
	 */
	pru->pru_interrupt_map = NULL;
	pru->pru_interrupt_map_sz = 0;
	if (ret)
		return ret;

	val = CTRL_CTRL_EN | ((rproc->bootaddr >> 2) << 16);
	pru_control_write_reg(pru, PRU_CTRL_CTRL, val);

	return 0;
}

static int pru_rproc_stop(struct rproc *rproc)
{
	struct device *dev = &rproc->dev;
	struct pru_rproc *pru = rproc->priv;
	const char *names[PRU_TYPE_MAX] = { "PRU", "RTU", "Tx_PRU" };
	u32 val;

	dev_dbg(dev, "stopping %s%d\n", names[pru->data->type], pru->id);

	val = pru_control_read_reg(pru, PRU_CTRL_CTRL);
	val &= ~CTRL_CTRL_EN;
	pru_control_write_reg(pru, PRU_CTRL_CTRL, val);

	/* dispose irq mapping - new firmware can provide new mapping */
	pru_dispose_irq_mapping(pru);

	return 0;
}

/*
 * Convert PRU device address (data spaces only) to kernel virtual address.
 *
 * Each PRU has access to all data memories within the PRUSS, accessible at
 * different ranges. So, look through both its primary and secondary Data
 * RAMs as well as any shared Data RAM to convert a PRU device address to
 * kernel virtual address. Data RAM0 is primary Data RAM for PRU0 and Data
 * RAM1 is primary Data RAM for PRU1.
 */
static void *pru_d_da_to_va(struct pru_rproc *pru, u32 da, size_t len)
{
	struct pruss_mem_region dram0, dram1, shrd_ram;
	struct pruss *pruss = pru->pruss;
	u32 offset;
	void *va = NULL;

	if (len == 0)
		return NULL;

	dram0 = pruss->mem_regions[PRUSS_MEM_DRAM0];
	dram1 = pruss->mem_regions[PRUSS_MEM_DRAM1];
	/* PRU1 has its local RAM addresses reversed */
	if (pru->id == PRUSS_PRU1)
		swap(dram0, dram1);
	shrd_ram = pruss->mem_regions[PRUSS_MEM_SHRD_RAM2];

	if (da + len <= PRU_PDRAM_DA + dram0.size) {
		offset = da - PRU_PDRAM_DA;
		va = (__force void *)(dram0.va + offset);
	} else if (da >= PRU_SDRAM_DA &&
		   da + len <= PRU_SDRAM_DA + dram1.size) {
		offset = da - PRU_SDRAM_DA;
		va = (__force void *)(dram1.va + offset);
	} else if (da >= PRU_SHRDRAM_DA &&
		   da + len <= PRU_SHRDRAM_DA + shrd_ram.size) {
		offset = da - PRU_SHRDRAM_DA;
		va = (__force void *)(shrd_ram.va + offset);
	}

	return va;
}

/*
 * Convert PRU device address (instruction space) to kernel virtual address.
 *
 * A PRU does not have an unified address space. Each PRU has its very own
 * private Instruction RAM, and its device address is identical to that of
 * its primary Data RAM device address.
 */
static void *pru_i_da_to_va(struct pru_rproc *pru, u32 da, size_t len)
{
	u32 offset;
	void *va = NULL;

	if (len == 0)
		return NULL;

	/*
	 * GNU binutils do not support multiple address spaces. The GNU
	 * linker's default linker script places IRAM at an arbitrary high
	 * offset, in order to differentiate it from DRAM. Hence we need to
	 * strip the artificial offset in the IRAM addresses coming from the
	 * ELF file.
	 *
	 * The TI proprietary linker would never set those higher IRAM address
	 * bits anyway. PRU architecture limits the program counter to 16-bit
	 * word-address range. This in turn corresponds to 18-bit IRAM
	 * byte-address range for ELF.
	 *
	 * Two more bits are added just in case to make the final 20-bit mask.
	 * Idea is to have a safeguard in case TI decides to add banking
	 * in future SoCs.
	 */
	da &= 0xfffff;

	if (da + len <= PRU_IRAM_DA + pru->mem_regions[PRU_IOMEM_IRAM].size) {
		offset = da - PRU_IRAM_DA;
		va = (__force void *)(pru->mem_regions[PRU_IOMEM_IRAM].va +
				      offset);
	}

	return va;
}

/*
 * Provide address translations for only PRU Data RAMs through the remoteproc
 * core for any PRU client drivers. The PRU Instruction RAM access is restricted
 * only to the PRU loader code.
 */
static void *pru_rproc_da_to_va(struct rproc *rproc, u64 da, size_t len, bool *is_iomem)
{
	struct pru_rproc *pru = rproc->priv;

	return pru_d_da_to_va(pru, da, len);
}

/* PRU-specific address translator used by PRU loader. */
static void *pru_da_to_va(struct rproc *rproc, u64 da, size_t len, bool is_iram)
{
	struct pru_rproc *pru = rproc->priv;
	void *va;

	if (is_iram)
		va = pru_i_da_to_va(pru, da, len);
	else
		va = pru_d_da_to_va(pru, da, len);

	return va;
}

static struct rproc_ops pru_rproc_ops = {
	.start		= pru_rproc_start,
	.stop		= pru_rproc_stop,
	.da_to_va	= pru_rproc_da_to_va,
};

/*
 * Custom memory copy implementation for ICSSG PRU/RTU/Tx_PRU Cores
 *
 * The ICSSG PRU/RTU/Tx_PRU cores have a memory copying issue with IRAM
 * memories, that is not seen on previous generation SoCs. The data is reflected
 * properly in the IRAM memories only for integer (4-byte) copies. Any unaligned
 * copies result in all the other pre-existing bytes zeroed out within that
 * 4-byte boundary, thereby resulting in wrong text/code in the IRAMs. Also, the
 * IRAM memory port interface does not allow any 8-byte copies (as commonly used
 * by ARM64 memcpy implementation) and throws an exception. The DRAM memory
 * ports do not show this behavior.
 */
static int pru_rproc_memcpy(void *dest, const void *src, size_t count)
{
	const u32 *s = src;
	u32 *d = dest;
	size_t size = count / 4;
	u32 *tmp_src = NULL;

	/*
	 * TODO: relax limitation of 4-byte aligned dest addresses and copy
	 * sizes
	 */
	if ((long)dest % 4 || count % 4)
		return -EINVAL;

	/* src offsets in ELF firmware image can be non-aligned */
	if ((long)src % 4) {
		tmp_src = kmemdup(src, count, GFP_KERNEL);
		if (!tmp_src)
			return -ENOMEM;
		s = tmp_src;
	}

	while (size--)
		*d++ = *s++;

	kfree(tmp_src);

	return 0;
}

static int
pru_rproc_load_elf_segments(struct rproc *rproc, const struct firmware *fw)
{
	struct pru_rproc *pru = rproc->priv;
	struct device *dev = &rproc->dev;
	struct elf32_hdr *ehdr;
	struct elf32_phdr *phdr;
	int i, ret = 0;
	const u8 *elf_data = fw->data;

	ehdr = (struct elf32_hdr *)elf_data;
	phdr = (struct elf32_phdr *)(elf_data + ehdr->e_phoff);

	/* go through the available ELF segments */
	for (i = 0; i < ehdr->e_phnum; i++, phdr++) {
		u32 da = phdr->p_paddr;
		u32 memsz = phdr->p_memsz;
		u32 filesz = phdr->p_filesz;
		u32 offset = phdr->p_offset;
		bool is_iram;
		void *ptr;

		if (phdr->p_type != PT_LOAD || !filesz)
			continue;

		dev_dbg(dev, "phdr: type %d da 0x%x memsz 0x%x filesz 0x%x\n",
			phdr->p_type, da, memsz, filesz);

		if (filesz > memsz) {
			dev_err(dev, "bad phdr filesz 0x%x memsz 0x%x\n",
				filesz, memsz);
			ret = -EINVAL;
			break;
		}

		if (offset + filesz > fw->size) {
			dev_err(dev, "truncated fw: need 0x%x avail 0x%zx\n",
				offset + filesz, fw->size);
			ret = -EINVAL;
			break;
		}

		/* grab the kernel address for this device address */
		is_iram = phdr->p_flags & PF_X;
		ptr = pru_da_to_va(rproc, da, memsz, is_iram);
		if (!ptr) {
			dev_err(dev, "bad phdr da 0x%x mem 0x%x\n", da, memsz);
			ret = -EINVAL;
			break;
		}

		if (pru->data->is_k3) {
			ret = pru_rproc_memcpy(ptr, elf_data + phdr->p_offset,
					       filesz);
			if (ret) {
				dev_err(dev, "PRU memory copy failed for da 0x%x memsz 0x%x\n",
					da, memsz);
				break;
			}
		} else {
			memcpy(ptr, elf_data + phdr->p_offset, filesz);
		}

		/* skip the memzero logic performed by remoteproc ELF loader */
	}

	return ret;
}

static const void *
pru_rproc_find_interrupt_map(struct device *dev, const struct firmware *fw)
{
	struct elf32_shdr *shdr, *name_table_shdr;
	const char *name_table;
	const u8 *elf_data = fw->data;
	struct elf32_hdr *ehdr = (struct elf32_hdr *)elf_data;
	u16 shnum = ehdr->e_shnum;
	u16 shstrndx = ehdr->e_shstrndx;
	int i;

	/* first, get the section header */
	shdr = (struct elf32_shdr *)(elf_data + ehdr->e_shoff);
	/* compute name table section header entry in shdr array */
	name_table_shdr = shdr + shstrndx;
	/* finally, compute the name table section address in elf */
	name_table = elf_data + name_table_shdr->sh_offset;

	for (i = 0; i < shnum; i++, shdr++) {
		u32 size = shdr->sh_size;
		u32 offset = shdr->sh_offset;
		u32 name = shdr->sh_name;

		if (strcmp(name_table + name, ".pru_irq_map"))
			continue;

		/* make sure we have the entire irq map */
		if (offset + size > fw->size || offset + size < size) {
			dev_err(dev, ".pru_irq_map section truncated\n");
			return ERR_PTR(-EINVAL);
		}

		/* make sure irq map has at least the header */
		if (sizeof(struct pru_irq_rsc) > size) {
			dev_err(dev, "header-less .pru_irq_map section\n");
			return ERR_PTR(-EINVAL);
		}

		return shdr;
	}

	dev_dbg(dev, "no .pru_irq_map section found for this fw\n");

	return NULL;
}

/*
 * Use a custom parse_fw callback function for dealing with PRU firmware
 * specific sections.
 *
 * The firmware blob can contain optional ELF sections: .resource_table section
 * and .pru_irq_map one. The second one contains the PRUSS interrupt mapping
 * description, which needs to be setup before powering on the PRU core. To
 * avoid RAM wastage this ELF section is not mapped to any ELF segment (by the
 * firmware linker) and therefore is not loaded to PRU memory.
 */
static int pru_rproc_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	struct device *dev = &rproc->dev;
	struct pru_rproc *pru = rproc->priv;
	const u8 *elf_data = fw->data;
	const void *shdr;
	u8 class = fw_elf_get_class(fw);
	u64 sh_offset;
	int ret;

	/* load optional rsc table */
	ret = rproc_elf_load_rsc_table(rproc, fw);
	if (ret == -EINVAL)
		dev_dbg(&rproc->dev, "no resource table found for this fw\n");
	else if (ret)
		return ret;

	/* find .pru_interrupt_map section, not having it is not an error */
	shdr = pru_rproc_find_interrupt_map(dev, fw);
	if (IS_ERR(shdr))
		return PTR_ERR(shdr);

	if (!shdr)
		return 0;

	/* preserve pointer to PRU interrupt map together with it size */
	sh_offset = elf_shdr_get_sh_offset(class, shdr);
	pru->pru_interrupt_map = (struct pru_irq_rsc *)(elf_data + sh_offset);
	pru->pru_interrupt_map_sz = elf_shdr_get_sh_size(class, shdr);

	return 0;
}

/*
 * Compute PRU id based on the IRAM addresses. The PRU IRAMs are
 * always at a particular offset within the PRUSS address space.
 */
static int pru_rproc_set_id(struct pru_rproc *pru)
{
	int ret = 0;

	switch (pru->mem_regions[PRU_IOMEM_IRAM].pa & PRU_IRAM_ADDR_MASK) {
	case TX_PRU0_IRAM_ADDR_MASK:
		fallthrough;
	case RTU0_IRAM_ADDR_MASK:
		fallthrough;
	case PRU0_IRAM_ADDR_MASK:
		pru->id = PRUSS_PRU0;
		break;
	case TX_PRU1_IRAM_ADDR_MASK:
		fallthrough;
	case RTU1_IRAM_ADDR_MASK:
		fallthrough;
	case PRU1_IRAM_ADDR_MASK:
		pru->id = PRUSS_PRU1;
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int pru_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct platform_device *ppdev = to_platform_device(dev->parent);
	struct pru_rproc *pru;
	const char *fw_name;
	struct rproc *rproc = NULL;
	struct resource *res;
	int i, ret;
	const struct pru_private_data *data;
	const char *mem_names[PRU_IOMEM_MAX] = { "iram", "control", "debug" };

	data = of_device_get_match_data(&pdev->dev);
	if (!data)
		return -ENODEV;

	ret = of_property_read_string(np, "firmware-name", &fw_name);
	if (ret) {
		dev_err(dev, "unable to retrieve firmware-name %d\n", ret);
		return ret;
	}

	rproc = devm_rproc_alloc(dev, pdev->name, &pru_rproc_ops, fw_name,
				 sizeof(*pru));
	if (!rproc) {
		dev_err(dev, "rproc_alloc failed\n");
		return -ENOMEM;
	}
	/* use a custom load function to deal with PRU-specific quirks */
	rproc->ops->load = pru_rproc_load_elf_segments;

	/* use a custom parse function to deal with PRU-specific resources */
	rproc->ops->parse_fw = pru_rproc_parse_fw;

	/* error recovery is not supported for PRUs */
	rproc->recovery_disabled = true;

	/*
	 * rproc_add will auto-boot the processor normally, but this is not
	 * desired with PRU client driven boot-flow methodology. A PRU
	 * application/client driver will boot the corresponding PRU
	 * remote-processor as part of its state machine either through the
	 * remoteproc sysfs interface or through the equivalent kernel API.
	 */
	rproc->auto_boot = false;

	pru = rproc->priv;
	pru->dev = dev;
	pru->data = data;
	pru->pruss = platform_get_drvdata(ppdev);
	pru->rproc = rproc;
	pru->fw_name = fw_name;
	pru->client_np = NULL;
	spin_lock_init(&pru->rmw_lock);
	mutex_init(&pru->lock);

	for (i = 0; i < ARRAY_SIZE(mem_names); i++) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   mem_names[i]);
		pru->mem_regions[i].va = devm_ioremap_resource(dev, res);
		if (IS_ERR(pru->mem_regions[i].va)) {
			dev_err(dev, "failed to parse and map memory resource %d %s\n",
				i, mem_names[i]);
			ret = PTR_ERR(pru->mem_regions[i].va);
			return ret;
		}
		pru->mem_regions[i].pa = res->start;
		pru->mem_regions[i].size = resource_size(res);

		dev_dbg(dev, "memory %8s: pa %pa size 0x%zx va %pK\n",
			mem_names[i], &pru->mem_regions[i].pa,
			pru->mem_regions[i].size, pru->mem_regions[i].va);
	}

	ret = pru_rproc_set_id(pru);
	if (ret < 0)
		return ret;

	platform_set_drvdata(pdev, rproc);

	ret = devm_rproc_add(dev, pru->rproc);
	if (ret) {
		dev_err(dev, "rproc_add failed: %d\n", ret);
		return ret;
	}

	pru_rproc_create_debug_entries(rproc);

	dev_dbg(dev, "PRU rproc node %pOF probed successfully\n", np);

	return 0;
}

static void pru_rproc_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rproc *rproc = platform_get_drvdata(pdev);

	dev_dbg(dev, "%s: removing rproc %s\n", __func__, rproc->name);
}

static const struct pru_private_data pru_data = {
	.type = PRU_TYPE_PRU,
};

static const struct pru_private_data k3_pru_data = {
	.type = PRU_TYPE_PRU,
	.is_k3 = 1,
};

static const struct pru_private_data k3_rtu_data = {
	.type = PRU_TYPE_RTU,
	.is_k3 = 1,
};

static const struct pru_private_data k3_tx_pru_data = {
	.type = PRU_TYPE_TX_PRU,
	.is_k3 = 1,
};

static const struct of_device_id pru_rproc_match[] = {
	{ .compatible = "ti,am3356-pru",	.data = &pru_data },
	{ .compatible = "ti,am4376-pru",	.data = &pru_data },
	{ .compatible = "ti,am5728-pru",	.data = &pru_data },
	{ .compatible = "ti,am642-pru",		.data = &k3_pru_data },
	{ .compatible = "ti,am642-rtu",		.data = &k3_rtu_data },
	{ .compatible = "ti,am642-tx-pru",	.data = &k3_tx_pru_data },
	{ .compatible = "ti,k2g-pru",		.data = &pru_data },
	{ .compatible = "ti,am654-pru",		.data = &k3_pru_data },
	{ .compatible = "ti,am654-rtu",		.data = &k3_rtu_data },
	{ .compatible = "ti,am654-tx-pru",	.data = &k3_tx_pru_data },
	{ .compatible = "ti,j721e-pru",		.data = &k3_pru_data },
	{ .compatible = "ti,j721e-rtu",		.data = &k3_rtu_data },
	{ .compatible = "ti,j721e-tx-pru",	.data = &k3_tx_pru_data },
	{ .compatible = "ti,am625-pru",		.data = &k3_pru_data },
	{},
};
MODULE_DEVICE_TABLE(of, pru_rproc_match);

static struct platform_driver pru_rproc_driver = {
	.driver = {
		.name   = PRU_RPROC_DRVNAME,
		.of_match_table = pru_rproc_match,
		.suppress_bind_attrs = true,
	},
	.probe  = pru_rproc_probe,
	.remove_new = pru_rproc_remove,
};
module_platform_driver(pru_rproc_driver);

MODULE_AUTHOR("Suman Anna <s-anna@ti.com>");
MODULE_AUTHOR("Andrew F. Davis <afd@ti.com>");
MODULE_AUTHOR("Grzegorz Jaszczyk <grzegorz.jaszczyk@linaro.org>");
MODULE_AUTHOR("Puranjay Mohan <p-mohan@ti.com>");
MODULE_AUTHOR("Md Danish Anwar <danishanwar@ti.com>");
MODULE_DESCRIPTION("PRU-ICSS Remote Processor Driver");
MODULE_LICENSE("GPL v2");
