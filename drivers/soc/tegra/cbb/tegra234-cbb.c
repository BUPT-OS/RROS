// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021-2022, NVIDIA CORPORATION. All rights reserved
 *
 * The driver handles Error's from Control Backbone(CBB) version 2.0.
 * generated due to illegal accesses. The driver prints debug information
 * about failed transaction on receiving interrupt from Error Notifier.
 * Error types supported by CBB2.0 are:
 *   UNSUPPORTED_ERR, PWRDOWN_ERR, TIMEOUT_ERR, FIREWALL_ERR, DECODE_ERR,
 *   SLAVE_ERR
 */

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/cpufeature.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <soc/tegra/fuse.h>
#include <soc/tegra/tegra-cbb.h>

#define FABRIC_EN_CFG_INTERRUPT_ENABLE_0_0	0x0
#define FABRIC_EN_CFG_STATUS_0_0		0x40
#define FABRIC_EN_CFG_ADDR_INDEX_0_0		0x60
#define FABRIC_EN_CFG_ADDR_LOW_0		0x80
#define FABRIC_EN_CFG_ADDR_HI_0			0x84

#define FABRIC_MN_MASTER_ERR_EN_0		0x200
#define FABRIC_MN_MASTER_ERR_FORCE_0		0x204
#define FABRIC_MN_MASTER_ERR_STATUS_0		0x208
#define FABRIC_MN_MASTER_ERR_OVERFLOW_STATUS_0	0x20c

#define FABRIC_MN_MASTER_LOG_ERR_STATUS_0	0x300
#define FABRIC_MN_MASTER_LOG_ADDR_LOW_0		0x304
#define FABRIC_MN_MASTER_LOG_ADDR_HIGH_0	0x308
#define FABRIC_MN_MASTER_LOG_ATTRIBUTES0_0	0x30c
#define FABRIC_MN_MASTER_LOG_ATTRIBUTES1_0	0x310
#define FABRIC_MN_MASTER_LOG_ATTRIBUTES2_0	0x314
#define FABRIC_MN_MASTER_LOG_USER_BITS0_0	0x318

#define AXI_SLV_TIMEOUT_STATUS_0_0		0x8
#define APB_BLOCK_TMO_STATUS_0			0xc00
#define APB_BLOCK_NUM_TMO_OFFSET		0x20

#define FAB_EM_EL_MSTRID		GENMASK(29, 24)
#define FAB_EM_EL_VQC			GENMASK(17, 16)
#define FAB_EM_EL_GRPSEC		GENMASK(14, 8)
#define FAB_EM_EL_FALCONSEC		GENMASK(1, 0)

#define FAB_EM_EL_FABID			GENMASK(20, 16)
#define FAB_EM_EL_SLAVEID		GENMASK(7, 0)

#define FAB_EM_EL_ACCESSID		GENMASK(7, 0)

#define FAB_EM_EL_AXCACHE		GENMASK(27, 24)
#define FAB_EM_EL_AXPROT		GENMASK(22, 20)
#define FAB_EM_EL_BURSTLENGTH		GENMASK(19, 12)
#define FAB_EM_EL_BURSTTYPE		GENMASK(9, 8)
#define FAB_EM_EL_BEATSIZE		GENMASK(6, 4)
#define FAB_EM_EL_ACCESSTYPE		GENMASK(0, 0)

#define USRBITS_MSTR_ID			GENMASK(29, 24)

#define REQ_SOCKET_ID			GENMASK(27, 24)

#define CCPLEX_MSTRID			0x1
#define FIREWALL_APERTURE_SZ		0x10000
/* Write firewall check enable */
#define WEN				0x20000

enum tegra234_cbb_fabric_ids {
	CBB_FAB_ID,
	SCE_FAB_ID,
	RCE_FAB_ID,
	DCE_FAB_ID,
	AON_FAB_ID,
	PSC_FAB_ID,
	BPMP_FAB_ID,
	FSI_FAB_ID,
	MAX_FAB_ID,
};

struct tegra234_slave_lookup {
	const char *name;
	unsigned int offset;
};

struct tegra234_cbb_fabric {
	const char *name;
	phys_addr_t off_mask_erd;
	phys_addr_t firewall_base;
	unsigned int firewall_ctl;
	unsigned int firewall_wr_ctl;
	const char * const *master_id;
	unsigned int notifier_offset;
	const struct tegra_cbb_error *errors;
	const int max_errors;
	const struct tegra234_slave_lookup *slave_map;
	const int max_slaves;
};

struct tegra234_cbb {
	struct tegra_cbb base;

	const struct tegra234_cbb_fabric *fabric;
	struct resource *res;
	void __iomem *regs;

	int num_intr;
	int sec_irq;

	/* record */
	void __iomem *mon;
	unsigned int type;
	u32 mask;
	u64 access;
	u32 mn_attr0;
	u32 mn_attr1;
	u32 mn_attr2;
	u32 mn_user_bits;
};

static inline struct tegra234_cbb *to_tegra234_cbb(struct tegra_cbb *cbb)
{
	return container_of(cbb, struct tegra234_cbb, base);
}

static LIST_HEAD(cbb_list);
static DEFINE_SPINLOCK(cbb_lock);

static bool
tegra234_cbb_write_access_allowed(struct platform_device *pdev, struct tegra234_cbb *cbb)
{
	u32 val;

	if (!cbb->fabric->firewall_base ||
	    !cbb->fabric->firewall_ctl ||
	    !cbb->fabric->firewall_wr_ctl) {
		dev_info(&pdev->dev, "SoC data missing for firewall\n");
		return false;
	}

	if ((cbb->fabric->firewall_ctl > FIREWALL_APERTURE_SZ) ||
	    (cbb->fabric->firewall_wr_ctl > FIREWALL_APERTURE_SZ)) {
		dev_err(&pdev->dev, "wrong firewall offset value\n");
		return false;
	}

	val = readl(cbb->regs + cbb->fabric->firewall_base + cbb->fabric->firewall_ctl);
	/*
	 * If the firewall check feature for allowing or blocking the
	 * write accesses through the firewall of a fabric is disabled
	 * then CCPLEX can write to the registers of that fabric.
	 */
	if (!(val & WEN))
		return true;

	/*
	 * If the firewall check is enabled then check whether CCPLEX
	 * has write access to the fabric's error notifier registers
	 */
	val = readl(cbb->regs + cbb->fabric->firewall_base + cbb->fabric->firewall_wr_ctl);
	if (val & (BIT(CCPLEX_MSTRID)))
		return true;

	return false;
}

static void tegra234_cbb_fault_enable(struct tegra_cbb *cbb)
{
	struct tegra234_cbb *priv = to_tegra234_cbb(cbb);
	void __iomem *addr;

	addr = priv->regs + priv->fabric->notifier_offset;
	writel(0x1ff, addr + FABRIC_EN_CFG_INTERRUPT_ENABLE_0_0);
	dsb(sy);
}

static void tegra234_cbb_error_clear(struct tegra_cbb *cbb)
{
	struct tegra234_cbb *priv = to_tegra234_cbb(cbb);

	writel(0x3f, priv->mon + FABRIC_MN_MASTER_ERR_STATUS_0);
	dsb(sy);
}

static u32 tegra234_cbb_get_status(struct tegra_cbb *cbb)
{
	struct tegra234_cbb *priv = to_tegra234_cbb(cbb);
	void __iomem *addr;
	u32 value;

	addr = priv->regs + priv->fabric->notifier_offset;
	value = readl(addr + FABRIC_EN_CFG_STATUS_0_0);
	dsb(sy);

	return value;
}

static void tegra234_cbb_mask_serror(struct tegra234_cbb *cbb)
{
	writel(0x1, cbb->regs + cbb->fabric->off_mask_erd);
	dsb(sy);
}

static u32 tegra234_cbb_get_tmo_slv(void __iomem *addr)
{
	u32 timeout;

	timeout = readl(addr);
	return timeout;
}

static void tegra234_cbb_tmo_slv(struct seq_file *file, const char *slave, void __iomem *addr,
				 u32 status)
{
	tegra_cbb_print_err(file, "\t  %s : %#x\n", slave, status);
}

static void tegra234_cbb_lookup_apbslv(struct seq_file *file, const char *slave,
				       void __iomem *base)
{
	unsigned int block = 0;
	void __iomem *addr;
	char name[64];
	u32 status;

	status = tegra234_cbb_get_tmo_slv(base);
	if (status)
		tegra_cbb_print_err(file, "\t  %s_BLOCK_TMO_STATUS : %#x\n", slave, status);

	while (status) {
		if (status & BIT(0)) {
			u32 timeout, clients, client = 0;

			addr = base + APB_BLOCK_NUM_TMO_OFFSET + (block * 4);
			timeout = tegra234_cbb_get_tmo_slv(addr);
			clients = timeout;

			while (timeout) {
				if (timeout & BIT(0)) {
					if (clients != 0xffffffff)
						clients &= BIT(client);

					sprintf(name, "%s_BLOCK%d_TMO", slave, block);

					tegra234_cbb_tmo_slv(file, name, addr, clients);
				}

				timeout >>= 1;
				client++;
			}
		}

		status >>= 1;
		block++;
	}
}

static void tegra234_lookup_slave_timeout(struct seq_file *file, struct tegra234_cbb *cbb,
					  u8 slave_id, u8 fab_id)
{
	const struct tegra234_slave_lookup *map = cbb->fabric->slave_map;
	void __iomem *addr;

	/*
	 * 1) Get slave node name and address mapping using slave_id.
	 * 2) Check if the timed out slave node is APB or AXI.
	 * 3) If AXI, then print timeout register and reset axi slave
	 *    using <FABRIC>_SN_<>_SLV_TIMEOUT_STATUS_0_0 register.
	 * 4) If APB, then perform an additional lookup to find the client
	 *    which timed out.
	 *	a) Get block number from the index of set bit in
	 *	   <FABRIC>_SN_AXI2APB_<>_BLOCK_TMO_STATUS_0 register.
	 *	b) Get address of register repective to block number i.e.
	 *	   <FABRIC>_SN_AXI2APB_<>_BLOCK<index-set-bit>_TMO_0.
	 *	c) Read the register in above step to get client_id which
	 *	   timed out as per the set bits.
	 *      d) Reset the timedout client and print details.
	 *	e) Goto step-a till all bits are set.
	 */

	addr = cbb->regs + map[slave_id].offset;

	if (strstr(map[slave_id].name, "AXI2APB")) {
		addr += APB_BLOCK_TMO_STATUS_0;

		tegra234_cbb_lookup_apbslv(file, map[slave_id].name, addr);
	} else {
		char name[64];
		u32 status;

		addr += AXI_SLV_TIMEOUT_STATUS_0_0;

		status = tegra234_cbb_get_tmo_slv(addr);
		if (status) {
			sprintf(name, "%s_SLV_TIMEOUT_STATUS", map[slave_id].name);
			tegra234_cbb_tmo_slv(file, name, addr, status);
		}
	}
}

static void tegra234_cbb_print_error(struct seq_file *file, struct tegra234_cbb *cbb, u32 status,
				     u32 overflow)
{
	unsigned int type = 0;

	if (status & (status - 1))
		tegra_cbb_print_err(file, "\t  Multiple type of errors reported\n");

	while (status) {
		if (type >= cbb->fabric->max_errors) {
			tegra_cbb_print_err(file, "\t  Wrong type index:%u, status:%u\n",
					    type, status);
			return;
		}

		if (status & 0x1)
			tegra_cbb_print_err(file, "\t  Error Code\t\t: %s\n",
					    cbb->fabric->errors[type].code);

		status >>= 1;
		type++;
	}

	type = 0;

	while (overflow) {
		if (type >= cbb->fabric->max_errors) {
			tegra_cbb_print_err(file, "\t  Wrong type index:%u, overflow:%u\n",
					    type, overflow);
			return;
		}

		if (overflow & 0x1)
			tegra_cbb_print_err(file, "\t  Overflow\t\t: Multiple %s\n",
					    cbb->fabric->errors[type].code);

		overflow >>= 1;
		type++;
	}
}

static void print_errlog_err(struct seq_file *file, struct tegra234_cbb *cbb)
{
	u8 cache_type, prot_type, burst_length, mstr_id, grpsec, vqc, falconsec, beat_size;
	u8 access_type, access_id, requester_socket_id, local_socket_id, slave_id, fab_id;
	char fabric_name[20];
	bool is_numa = false;
	u8 burst_type;

	if (num_possible_nodes() > 1)
		is_numa = true;

	mstr_id = FIELD_GET(FAB_EM_EL_MSTRID, cbb->mn_user_bits);
	vqc = FIELD_GET(FAB_EM_EL_VQC, cbb->mn_user_bits);
	grpsec = FIELD_GET(FAB_EM_EL_GRPSEC, cbb->mn_user_bits);
	falconsec = FIELD_GET(FAB_EM_EL_FALCONSEC, cbb->mn_user_bits);

	/*
	 * For SOC with multiple NUMA nodes, print cross socket access
	 * errors only if initiator/master_id is CCPLEX, CPMU or GPU.
	 */
	if (is_numa) {
		local_socket_id = numa_node_id();
		requester_socket_id = FIELD_GET(REQ_SOCKET_ID, cbb->mn_attr2);

		if (requester_socket_id != local_socket_id) {
			if ((mstr_id != 0x1) && (mstr_id != 0x2) && (mstr_id != 0xB))
				return;
		}
	}

	fab_id = FIELD_GET(FAB_EM_EL_FABID, cbb->mn_attr2);
	slave_id = FIELD_GET(FAB_EM_EL_SLAVEID, cbb->mn_attr2);

	access_id = FIELD_GET(FAB_EM_EL_ACCESSID, cbb->mn_attr1);

	cache_type = FIELD_GET(FAB_EM_EL_AXCACHE, cbb->mn_attr0);
	prot_type = FIELD_GET(FAB_EM_EL_AXPROT, cbb->mn_attr0);
	burst_length = FIELD_GET(FAB_EM_EL_BURSTLENGTH, cbb->mn_attr0);
	burst_type = FIELD_GET(FAB_EM_EL_BURSTTYPE, cbb->mn_attr0);
	beat_size = FIELD_GET(FAB_EM_EL_BEATSIZE, cbb->mn_attr0);
	access_type = FIELD_GET(FAB_EM_EL_ACCESSTYPE, cbb->mn_attr0);

	tegra_cbb_print_err(file, "\n");
	if (cbb->type < cbb->fabric->max_errors)
		tegra_cbb_print_err(file, "\t  Error Code\t\t: %s\n",
				    cbb->fabric->errors[cbb->type].code);
	else
		tegra_cbb_print_err(file, "\t  Wrong type index:%u\n", cbb->type);

	tegra_cbb_print_err(file, "\t  MASTER_ID\t\t: %s\n", cbb->fabric->master_id[mstr_id]);
	tegra_cbb_print_err(file, "\t  Address\t\t: %#llx\n", cbb->access);

	tegra_cbb_print_cache(file, cache_type);
	tegra_cbb_print_prot(file, prot_type);

	tegra_cbb_print_err(file, "\t  Access_Type\t\t: %s", (access_type) ? "Write\n" : "Read\n");
	tegra_cbb_print_err(file, "\t  Access_ID\t\t: %#x", access_id);

	if (fab_id == PSC_FAB_ID)
		strcpy(fabric_name, "psc-fabric");
	else if (fab_id == FSI_FAB_ID)
		strcpy(fabric_name, "fsi-fabric");
	else
		strcpy(fabric_name, cbb->fabric->name);

	if (is_numa) {
		tegra_cbb_print_err(file, "\t  Requester_Socket_Id\t: %#x\n",
				    requester_socket_id);
		tegra_cbb_print_err(file, "\t  Local_Socket_Id\t: %#x\n",
				    local_socket_id);
		tegra_cbb_print_err(file, "\t  No. of NUMA_NODES\t: %#x\n",
				    num_possible_nodes());
	}

	tegra_cbb_print_err(file, "\t  Fabric\t\t: %s\n", fabric_name);
	tegra_cbb_print_err(file, "\t  Slave_Id\t\t: %#x\n", slave_id);
	tegra_cbb_print_err(file, "\t  Burst_length\t\t: %#x\n", burst_length);
	tegra_cbb_print_err(file, "\t  Burst_type\t\t: %#x\n", burst_type);
	tegra_cbb_print_err(file, "\t  Beat_size\t\t: %#x\n", beat_size);
	tegra_cbb_print_err(file, "\t  VQC\t\t\t: %#x\n", vqc);
	tegra_cbb_print_err(file, "\t  GRPSEC\t\t: %#x\n", grpsec);
	tegra_cbb_print_err(file, "\t  FALCONSEC\t\t: %#x\n", falconsec);

	if ((fab_id == PSC_FAB_ID) || (fab_id == FSI_FAB_ID))
		return;

	if (slave_id >= cbb->fabric->max_slaves) {
		tegra_cbb_print_err(file, "\t  Invalid slave_id:%d\n", slave_id);
		return;
	}

	if (!strcmp(cbb->fabric->errors[cbb->type].code, "TIMEOUT_ERR")) {
		tegra234_lookup_slave_timeout(file, cbb, slave_id, fab_id);
		return;
	}

	tegra_cbb_print_err(file, "\t  Slave\t\t\t: %s\n", cbb->fabric->slave_map[slave_id].name);
}

static int print_errmonX_info(struct seq_file *file, struct tegra234_cbb *cbb)
{
	u32 overflow, status, error;

	status = readl(cbb->mon + FABRIC_MN_MASTER_ERR_STATUS_0);
	if (!status) {
		pr_err("Error Notifier received a spurious notification\n");
		return -ENODATA;
	}

	if (status == 0xffffffff) {
		pr_err("CBB registers returning all 1's which is invalid\n");
		return -EINVAL;
	}

	overflow = readl(cbb->mon + FABRIC_MN_MASTER_ERR_OVERFLOW_STATUS_0);

	tegra234_cbb_print_error(file, cbb, status, overflow);

	error = readl(cbb->mon + FABRIC_MN_MASTER_LOG_ERR_STATUS_0);
	if (!error) {
		pr_info("Error Monitor doesn't have Error Logger\n");
		return -EINVAL;
	}

	cbb->type = 0;

	while (error) {
		if (error & BIT(0)) {
			u32 hi, lo;

			hi = readl(cbb->mon + FABRIC_MN_MASTER_LOG_ADDR_HIGH_0);
			lo = readl(cbb->mon + FABRIC_MN_MASTER_LOG_ADDR_LOW_0);

			cbb->access = (u64)hi << 32 | lo;

			cbb->mn_attr0 = readl(cbb->mon + FABRIC_MN_MASTER_LOG_ATTRIBUTES0_0);
			cbb->mn_attr1 = readl(cbb->mon + FABRIC_MN_MASTER_LOG_ATTRIBUTES1_0);
			cbb->mn_attr2 = readl(cbb->mon + FABRIC_MN_MASTER_LOG_ATTRIBUTES2_0);
			cbb->mn_user_bits = readl(cbb->mon + FABRIC_MN_MASTER_LOG_USER_BITS0_0);

			print_errlog_err(file, cbb);
		}

		cbb->type++;
		error >>= 1;
	}

	return 0;
}

static int print_err_notifier(struct seq_file *file, struct tegra234_cbb *cbb, u32 status)
{
	unsigned int index = 0;
	int err;

	pr_crit("**************************************\n");
	pr_crit("CPU:%d, Error:%s, Errmon:%d\n", smp_processor_id(),
		cbb->fabric->name, status);

	while (status) {
		if (status & BIT(0)) {
			unsigned int notifier = cbb->fabric->notifier_offset;
			u32 hi, lo, mask = BIT(index);
			phys_addr_t addr;
			u64 offset;

			writel(mask, cbb->regs + notifier + FABRIC_EN_CFG_ADDR_INDEX_0_0);
			hi = readl(cbb->regs + notifier + FABRIC_EN_CFG_ADDR_HI_0);
			lo = readl(cbb->regs + notifier + FABRIC_EN_CFG_ADDR_LOW_0);

			addr = (u64)hi << 32 | lo;

			offset = addr - cbb->res->start;
			cbb->mon = cbb->regs + offset;
			cbb->mask = BIT(index);

			err = print_errmonX_info(file, cbb);
			tegra234_cbb_error_clear(&cbb->base);
			if (err)
				return err;
		}

		status >>= 1;
		index++;
	}

	tegra_cbb_print_err(file, "\t**************************************\n");
	return 0;
}

#ifdef CONFIG_DEBUG_FS
static DEFINE_MUTEX(cbb_debugfs_mutex);

static int tegra234_cbb_debugfs_show(struct tegra_cbb *cbb, struct seq_file *file, void *data)
{
	int err = 0;

	mutex_lock(&cbb_debugfs_mutex);

	list_for_each_entry(cbb, &cbb_list, node) {
		struct tegra234_cbb *priv = to_tegra234_cbb(cbb);
		u32 status;

		status = tegra_cbb_get_status(&priv->base);
		if (status) {
			err = print_err_notifier(file, priv, status);
			if (err)
				break;
		}
	}

	mutex_unlock(&cbb_debugfs_mutex);
	return err;
}
#endif

/*
 * Handler for CBB errors
 */
static irqreturn_t tegra234_cbb_isr(int irq, void *data)
{
	bool is_inband_err = false;
	struct tegra_cbb *cbb;
	unsigned long flags;
	u8 mstr_id;
	int err;

	spin_lock_irqsave(&cbb_lock, flags);

	list_for_each_entry(cbb, &cbb_list, node) {
		struct tegra234_cbb *priv = to_tegra234_cbb(cbb);
		u32 status = tegra_cbb_get_status(cbb);

		if (status && (irq == priv->sec_irq)) {
			tegra_cbb_print_err(NULL, "CPU:%d, Error: %s@0x%llx, irq=%d\n",
					    smp_processor_id(), priv->fabric->name,
					    priv->res->start, irq);

			err = print_err_notifier(NULL, priv, status);
			if (err)
				goto unlock;

			/*
			 * If illegal request is from CCPLEX(id:0x1) master then call WARN()
			 */
			if (priv->fabric->off_mask_erd) {
				mstr_id =  FIELD_GET(USRBITS_MSTR_ID, priv->mn_user_bits);
				if (mstr_id == CCPLEX_MSTRID)
					is_inband_err = 1;
			}
		}
	}

unlock:
	spin_unlock_irqrestore(&cbb_lock, flags);
	WARN_ON(is_inband_err);
	return IRQ_HANDLED;
}

/*
 * Register handler for CBB_SECURE interrupt for reporting errors
 */
static int tegra234_cbb_interrupt_enable(struct tegra_cbb *cbb)
{
	struct tegra234_cbb *priv = to_tegra234_cbb(cbb);

	if (priv->sec_irq) {
		int err = devm_request_irq(cbb->dev, priv->sec_irq, tegra234_cbb_isr, 0,
					   dev_name(cbb->dev), priv);
		if (err) {
			dev_err(cbb->dev, "failed to register interrupt %u: %d\n", priv->sec_irq,
				err);
			return err;
		}
	}

	return 0;
}

static void tegra234_cbb_error_enable(struct tegra_cbb *cbb)
{
	tegra_cbb_fault_enable(cbb);
}

static const struct tegra_cbb_ops tegra234_cbb_ops = {
	.get_status = tegra234_cbb_get_status,
	.error_clear = tegra234_cbb_error_clear,
	.fault_enable = tegra234_cbb_fault_enable,
	.error_enable = tegra234_cbb_error_enable,
	.interrupt_enable = tegra234_cbb_interrupt_enable,
#ifdef CONFIG_DEBUG_FS
	.debugfs_show = tegra234_cbb_debugfs_show,
#endif
};

static const char * const tegra234_master_id[] = {
	[0x00] = "TZ",
	[0x01] = "CCPLEX",
	[0x02] = "CCPMU",
	[0x03] = "BPMP_FW",
	[0x04] = "AON",
	[0x05] = "SCE",
	[0x06] = "GPCDMA_P",
	[0x07] = "TSECA_NONSECURE",
	[0x08] = "TSECA_LIGHTSECURE",
	[0x09] = "TSECA_HEAVYSECURE",
	[0x0a] = "CORESIGHT",
	[0x0b] = "APE",
	[0x0c] = "PEATRANS",
	[0x0d] = "JTAGM_DFT",
	[0x0e] = "RCE",
	[0x0f] = "DCE",
	[0x10] = "PSC_FW_USER",
	[0x11] = "PSC_FW_SUPERVISOR",
	[0x12] = "PSC_FW_MACHINE",
	[0x13] = "PSC_BOOT",
	[0x14] = "BPMP_BOOT",
	[0x15] = "NVDEC_NONSECURE",
	[0x16] = "NVDEC_LIGHTSECURE",
	[0x17] = "NVDEC_HEAVYSECURE",
	[0x18] = "CBB_INTERNAL",
	[0x19] = "RSVD"
};

static const struct tegra_cbb_error tegra234_cbb_errors[] = {
	{
		.code = "SLAVE_ERR",
		.desc = "Slave being accessed responded with an error"
	}, {
		.code = "DECODE_ERR",
		.desc = "Attempt to access an address hole"
	}, {
		.code = "FIREWALL_ERR",
		.desc = "Attempt to access a region which is firewall protected"
	}, {
		.code = "TIMEOUT_ERR",
		.desc = "No response returned by slave"
	}, {
		.code = "PWRDOWN_ERR",
		.desc = "Attempt to access a portion of fabric that is powered down"
	}, {
		.code = "UNSUPPORTED_ERR",
		.desc = "Attempt to access a slave through an unsupported access"
	}
};

static const struct tegra234_slave_lookup tegra234_aon_slave_map[] = {
	{ "AXI2APB", 0x00000 },
	{ "AST",     0x14000 },
	{ "CBB",     0x15000 },
	{ "CPU",     0x16000 },
};

static const struct tegra234_cbb_fabric tegra234_aon_fabric = {
	.name = "aon-fabric",
	.master_id = tegra234_master_id,
	.slave_map = tegra234_aon_slave_map,
	.max_slaves = ARRAY_SIZE(tegra234_aon_slave_map),
	.errors = tegra234_cbb_errors,
	.max_errors = ARRAY_SIZE(tegra234_cbb_errors),
	.notifier_offset = 0x17000,
	.firewall_base = 0x30000,
	.firewall_ctl = 0x8d0,
	.firewall_wr_ctl = 0x8c8,
};

static const struct tegra234_slave_lookup tegra234_bpmp_slave_map[] = {
	{ "AXI2APB", 0x00000 },
	{ "AST0",    0x15000 },
	{ "AST1",    0x16000 },
	{ "CBB",     0x17000 },
	{ "CPU",     0x18000 },
};

static const struct tegra234_cbb_fabric tegra234_bpmp_fabric = {
	.name = "bpmp-fabric",
	.master_id = tegra234_master_id,
	.slave_map = tegra234_bpmp_slave_map,
	.max_slaves = ARRAY_SIZE(tegra234_bpmp_slave_map),
	.errors = tegra234_cbb_errors,
	.max_errors = ARRAY_SIZE(tegra234_cbb_errors),
	.notifier_offset = 0x19000,
	.firewall_base = 0x30000,
	.firewall_ctl = 0x8f0,
	.firewall_wr_ctl = 0x8e8,
};

static const struct tegra234_slave_lookup tegra234_cbb_slave_map[] = {
	{ "AON",        0x40000 },
	{ "BPMP",       0x41000 },
	{ "CBB",        0x42000 },
	{ "HOST1X",     0x43000 },
	{ "STM",        0x44000 },
	{ "FSI",        0x45000 },
	{ "PSC",        0x46000 },
	{ "PCIE_C1",    0x47000 },
	{ "PCIE_C2",    0x48000 },
	{ "PCIE_C3",    0x49000 },
	{ "PCIE_C0",    0x4a000 },
	{ "PCIE_C4",    0x4b000 },
	{ "GPU",        0x4c000 },
	{ "SMMU0",      0x4d000 },
	{ "SMMU1",      0x4e000 },
	{ "SMMU2",      0x4f000 },
	{ "SMMU3",      0x50000 },
	{ "SMMU4",      0x51000 },
	{ "PCIE_C10",   0x52000 },
	{ "PCIE_C7",    0x53000 },
	{ "PCIE_C8",    0x54000 },
	{ "PCIE_C9",    0x55000 },
	{ "PCIE_C5",    0x56000 },
	{ "PCIE_C6",    0x57000 },
	{ "DCE",        0x58000 },
	{ "RCE",        0x59000 },
	{ "SCE",        0x5a000 },
	{ "AXI2APB_1",  0x70000 },
	{ "AXI2APB_10", 0x71000 },
	{ "AXI2APB_11", 0x72000 },
	{ "AXI2APB_12", 0x73000 },
	{ "AXI2APB_13", 0x74000 },
	{ "AXI2APB_14", 0x75000 },
	{ "AXI2APB_15", 0x76000 },
	{ "AXI2APB_16", 0x77000 },
	{ "AXI2APB_17", 0x78000 },
	{ "AXI2APB_18", 0x79000 },
	{ "AXI2APB_19", 0x7a000 },
	{ "AXI2APB_2",  0x7b000 },
	{ "AXI2APB_20", 0x7c000 },
	{ "AXI2APB_21", 0x7d000 },
	{ "AXI2APB_22", 0x7e000 },
	{ "AXI2APB_23", 0x7f000 },
	{ "AXI2APB_25", 0x80000 },
	{ "AXI2APB_26", 0x81000 },
	{ "AXI2APB_27", 0x82000 },
	{ "AXI2APB_28", 0x83000 },
	{ "AXI2APB_29", 0x84000 },
	{ "AXI2APB_30", 0x85000 },
	{ "AXI2APB_31", 0x86000 },
	{ "AXI2APB_32", 0x87000 },
	{ "AXI2APB_33", 0x88000 },
	{ "AXI2APB_34", 0x89000 },
	{ "AXI2APB_35", 0x92000 },
	{ "AXI2APB_4",  0x8b000 },
	{ "AXI2APB_5",  0x8c000 },
	{ "AXI2APB_6",  0x8d000 },
	{ "AXI2APB_7",  0x8e000 },
	{ "AXI2APB_8",  0x8f000 },
	{ "AXI2APB_9",  0x90000 },
	{ "AXI2APB_3",  0x91000 },
};

static const struct tegra234_cbb_fabric tegra234_cbb_fabric = {
	.name = "cbb-fabric",
	.master_id = tegra234_master_id,
	.slave_map = tegra234_cbb_slave_map,
	.max_slaves = ARRAY_SIZE(tegra234_cbb_slave_map),
	.errors = tegra234_cbb_errors,
	.max_errors = ARRAY_SIZE(tegra234_cbb_errors),
	.notifier_offset = 0x60000,
	.off_mask_erd = 0x3a004,
	.firewall_base = 0x10000,
	.firewall_ctl = 0x23f0,
	.firewall_wr_ctl = 0x23e8,
};

static const struct tegra234_slave_lookup tegra234_common_slave_map[] = {
	{ "AXI2APB", 0x00000 },
	{ "AST0",    0x15000 },
	{ "AST1",    0x16000 },
	{ "CBB",     0x17000 },
	{ "RSVD",    0x00000 },
	{ "CPU",     0x18000 },
};

static const struct tegra234_cbb_fabric tegra234_dce_fabric = {
	.name = "dce-fabric",
	.master_id = tegra234_master_id,
	.slave_map = tegra234_common_slave_map,
	.max_slaves = ARRAY_SIZE(tegra234_common_slave_map),
	.errors = tegra234_cbb_errors,
	.max_errors = ARRAY_SIZE(tegra234_cbb_errors),
	.notifier_offset = 0x19000,
	.firewall_base = 0x30000,
	.firewall_ctl = 0x290,
	.firewall_wr_ctl = 0x288,
};

static const struct tegra234_cbb_fabric tegra234_rce_fabric = {
	.name = "rce-fabric",
	.master_id = tegra234_master_id,
	.slave_map = tegra234_common_slave_map,
	.max_slaves = ARRAY_SIZE(tegra234_common_slave_map),
	.errors = tegra234_cbb_errors,
	.max_errors = ARRAY_SIZE(tegra234_cbb_errors),
	.notifier_offset = 0x19000,
	.firewall_base = 0x30000,
	.firewall_ctl = 0x290,
	.firewall_wr_ctl = 0x288,
};

static const struct tegra234_cbb_fabric tegra234_sce_fabric = {
	.name = "sce-fabric",
	.master_id = tegra234_master_id,
	.slave_map = tegra234_common_slave_map,
	.max_slaves = ARRAY_SIZE(tegra234_common_slave_map),
	.errors = tegra234_cbb_errors,
	.max_errors = ARRAY_SIZE(tegra234_cbb_errors),
	.notifier_offset = 0x19000,
	.firewall_base = 0x30000,
	.firewall_ctl = 0x290,
	.firewall_wr_ctl = 0x288,
};

static const char * const tegra241_master_id[] = {
	[0x0] = "TZ",
	[0x1] = "CCPLEX",
	[0x2] = "CCPMU",
	[0x3] = "BPMP_FW",
	[0x4] = "PSC_FW_USER",
	[0x5] = "PSC_FW_SUPERVISOR",
	[0x6] = "PSC_FW_MACHINE",
	[0x7] = "PSC_BOOT",
	[0x8] = "BPMP_BOOT",
	[0x9] = "JTAGM_DFT",
	[0xa] = "CORESIGHT",
	[0xb] = "GPU",
	[0xc] = "PEATRANS",
	[0xd ... 0x3f] = "RSVD"
};

/*
 * Possible causes for Slave and Timeout errors.
 * SLAVE_ERR:
 * Slave being accessed responded with an error. Slave could return
 * an error for various cases :
 *   Unsupported access, clamp setting when power gated, register
 *   level firewall(SCR), address hole within the slave, etc
 *
 * TIMEOUT_ERR:
 * No response returned by slave. Can be due to slave being clock
 * gated, under reset, powered down or slave inability to respond
 * for an internal slave issue
 */
static const struct tegra_cbb_error tegra241_cbb_errors[] = {
	{
		.code = "SLAVE_ERR",
		.desc = "Slave being accessed responded with an error."
	}, {
		.code = "DECODE_ERR",
		.desc = "Attempt to access an address hole or Reserved region of memory."
	}, {
		.code = "FIREWALL_ERR",
		.desc = "Attempt to access a region which is firewalled."
	}, {
		.code = "TIMEOUT_ERR",
		.desc = "No response returned by slave."
	}, {
		.code = "PWRDOWN_ERR",
		.desc = "Attempt to access a portion of the fabric that is powered down."
	}, {
		.code = "UNSUPPORTED_ERR",
		.desc = "Attempt to access a slave through an unsupported access."
	}, {
		.code = "POISON_ERR",
		.desc = "Slave responds with poison error to indicate error in data."
	}, {
		.code = "RSVD"
	}, {
		.code = "RSVD"
	}, {
		.code = "RSVD"
	}, {
		.code = "RSVD"
	}, {
		.code = "RSVD"
	}, {
		.code = "RSVD"
	}, {
		.code = "RSVD"
	}, {
		.code = "RSVD"
	}, {
		.code = "RSVD"
	}, {
		.code = "NO_SUCH_ADDRESS_ERR",
		.desc = "The address belongs to the pri_target range but there is no register "
			"implemented at the address."
	}, {
		.code = "TASK_ERR",
		.desc = "Attempt to update a PRI task when the current task has still not "
			"completed."
	}, {
		.code = "EXTERNAL_ERR",
		.desc = "Indicates that an external PRI register access met with an error due to "
			"any issue in the unit."
	}, {
		.code = "INDEX_ERR",
		.desc = "Applicable to PRI index aperture pair, when the programmed index is "
			"outside the range defined in the manual."
	}, {
		.code = "RESET_ERR",
		.desc = "Target in Reset Error: Attempt to access a SubPri or external PRI "
			"register but they are in reset."
	}, {
		.code = "REGISTER_RST_ERR",
		.desc = "Attempt to access a PRI register but the register is partial or "
			"completely in reset."
	}, {
		.code = "POWER_GATED_ERR",
		.desc = "Returned by external PRI client when the external access goes to a power "
			"gated domain."
	}, {
		.code = "SUBPRI_FS_ERR",
		.desc = "Subpri is floorswept: Attempt to access a subpri through the main pri "
			"target but subPri logic is floorswept."
	}, {
		.code = "SUBPRI_CLK_OFF_ERR",
		.desc = "Subpri clock is off: Attempt to access a subpri through the main pri "
			"target but subPris clock is gated/off."
	},
};

static const struct tegra234_slave_lookup tegra241_cbb_slave_map[] = {
	{ "RSVD",       0x00000 },
	{ "PCIE_C8",    0x51000 },
	{ "PCIE_C9",    0x52000 },
	{ "RSVD",       0x00000 },
	{ "RSVD",       0x00000 },
	{ "RSVD",       0x00000 },
	{ "RSVD",       0x00000 },
	{ "RSVD",       0x00000 },
	{ "RSVD",       0x00000 },
	{ "RSVD",       0x00000 },
	{ "RSVD",       0x00000 },
	{ "AON",        0x5b000 },
	{ "BPMP",       0x5c000 },
	{ "RSVD",       0x00000 },
	{ "RSVD",       0x00000 },
	{ "PSC",        0x5d000 },
	{ "STM",        0x5e000 },
	{ "AXI2APB_1",  0x70000 },
	{ "AXI2APB_10", 0x71000 },
	{ "AXI2APB_11", 0x72000 },
	{ "AXI2APB_12", 0x73000 },
	{ "AXI2APB_13", 0x74000 },
	{ "AXI2APB_14", 0x75000 },
	{ "AXI2APB_15", 0x76000 },
	{ "AXI2APB_16", 0x77000 },
	{ "AXI2APB_17", 0x78000 },
	{ "AXI2APB_18", 0x79000 },
	{ "AXI2APB_19", 0x7a000 },
	{ "AXI2APB_2",  0x7b000 },
	{ "AXI2APB_20", 0x7c000 },
	{ "AXI2APB_4",  0x87000 },
	{ "AXI2APB_5",  0x88000 },
	{ "AXI2APB_6",  0x89000 },
	{ "AXI2APB_7",  0x8a000 },
	{ "AXI2APB_8",  0x8b000 },
	{ "AXI2APB_9",  0x8c000 },
	{ "AXI2APB_3",  0x8d000 },
	{ "AXI2APB_21", 0x7d000 },
	{ "AXI2APB_22", 0x7e000 },
	{ "AXI2APB_23", 0x7f000 },
	{ "AXI2APB_24", 0x80000 },
	{ "AXI2APB_25", 0x81000 },
	{ "AXI2APB_26", 0x82000 },
	{ "AXI2APB_27", 0x83000 },
	{ "AXI2APB_28", 0x84000 },
	{ "PCIE_C4",    0x53000 },
	{ "PCIE_C5",    0x54000 },
	{ "PCIE_C6",    0x55000 },
	{ "PCIE_C7",    0x56000 },
	{ "PCIE_C2",    0x57000 },
	{ "PCIE_C3",    0x58000 },
	{ "PCIE_C0",    0x59000 },
	{ "PCIE_C1",    0x5a000 },
	{ "CCPLEX",     0x50000 },
	{ "AXI2APB_29", 0x85000 },
	{ "AXI2APB_30", 0x86000 },
	{ "CBB_CENTRAL", 0x00000 },
	{ "AXI2APB_31", 0x8E000 },
	{ "AXI2APB_32", 0x8F000 },
};

static const struct tegra234_cbb_fabric tegra241_cbb_fabric = {
	.name = "cbb-fabric",
	.master_id = tegra241_master_id,
	.slave_map = tegra241_cbb_slave_map,
	.max_slaves = ARRAY_SIZE(tegra241_cbb_slave_map),
	.errors = tegra241_cbb_errors,
	.max_errors = ARRAY_SIZE(tegra241_cbb_errors),
	.notifier_offset = 0x60000,
	.off_mask_erd = 0x40004,
	.firewall_base = 0x20000,
	.firewall_ctl = 0x2370,
	.firewall_wr_ctl = 0x2368,
};

static const struct tegra234_slave_lookup tegra241_bpmp_slave_map[] = {
	{ "RSVD",    0x00000 },
	{ "RSVD",    0x00000 },
	{ "RSVD",    0x00000 },
	{ "CBB",     0x15000 },
	{ "CPU",     0x16000 },
	{ "AXI2APB", 0x00000 },
	{ "DBB0",    0x17000 },
	{ "DBB1",    0x18000 },
};

static const struct tegra234_cbb_fabric tegra241_bpmp_fabric = {
	.name = "bpmp-fabric",
	.master_id = tegra241_master_id,
	.slave_map = tegra241_bpmp_slave_map,
	.max_slaves = ARRAY_SIZE(tegra241_bpmp_slave_map),
	.errors = tegra241_cbb_errors,
	.max_errors = ARRAY_SIZE(tegra241_cbb_errors),
	.notifier_offset = 0x19000,
	.firewall_base = 0x30000,
	.firewall_ctl = 0x8f0,
	.firewall_wr_ctl = 0x8e8,
};

static const struct of_device_id tegra234_cbb_dt_ids[] = {
	{ .compatible = "nvidia,tegra234-cbb-fabric", .data = &tegra234_cbb_fabric },
	{ .compatible = "nvidia,tegra234-aon-fabric", .data = &tegra234_aon_fabric },
	{ .compatible = "nvidia,tegra234-bpmp-fabric", .data = &tegra234_bpmp_fabric },
	{ .compatible = "nvidia,tegra234-dce-fabric", .data = &tegra234_dce_fabric },
	{ .compatible = "nvidia,tegra234-rce-fabric", .data = &tegra234_rce_fabric },
	{ .compatible = "nvidia,tegra234-sce-fabric", .data = &tegra234_sce_fabric },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, tegra234_cbb_dt_ids);

struct tegra234_cbb_acpi_uid {
	const char *hid;
	const char *uid;
	const struct tegra234_cbb_fabric *fabric;
};

static const struct tegra234_cbb_acpi_uid tegra234_cbb_acpi_uids[] = {
	{ "NVDA1070", "1", &tegra241_cbb_fabric },
	{ "NVDA1070", "2", &tegra241_bpmp_fabric },
	{ },
};

static const struct
tegra234_cbb_fabric *tegra234_cbb_acpi_get_fabric(struct acpi_device *adev)
{
	const struct tegra234_cbb_acpi_uid *entry;

	for (entry = tegra234_cbb_acpi_uids; entry->hid; entry++) {
		if (acpi_dev_hid_uid_match(adev, entry->hid, entry->uid))
			return entry->fabric;
	}

	return NULL;
}

static const struct acpi_device_id tegra241_cbb_acpi_ids[] = {
	{ "NVDA1070" },
	{ },
};
MODULE_DEVICE_TABLE(acpi, tegra241_cbb_acpi_ids);

static int tegra234_cbb_probe(struct platform_device *pdev)
{
	const struct tegra234_cbb_fabric *fabric;
	struct tegra234_cbb *cbb;
	unsigned long flags = 0;
	int err;

	if (pdev->dev.of_node) {
		fabric = of_device_get_match_data(&pdev->dev);
	} else {
		struct acpi_device *device = ACPI_COMPANION(&pdev->dev);
		if (!device)
			return -ENODEV;

		fabric = tegra234_cbb_acpi_get_fabric(device);
		if (!fabric) {
			dev_err(&pdev->dev, "no device match found\n");
			return -ENODEV;
		}
	}

	cbb = devm_kzalloc(&pdev->dev, sizeof(*cbb), GFP_KERNEL);
	if (!cbb)
		return -ENOMEM;

	INIT_LIST_HEAD(&cbb->base.node);
	cbb->base.ops = &tegra234_cbb_ops;
	cbb->base.dev = &pdev->dev;
	cbb->fabric = fabric;

	cbb->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &cbb->res);
	if (IS_ERR(cbb->regs))
		return PTR_ERR(cbb->regs);

	err = tegra_cbb_get_irq(pdev, NULL, &cbb->sec_irq);
	if (err)
		return err;

	platform_set_drvdata(pdev, cbb);

	/*
	 * Don't enable error reporting for a Fabric if write to it's registers
	 * is blocked by CBB firewall.
	 */
	if (!tegra234_cbb_write_access_allowed(pdev, cbb)) {
		dev_info(&pdev->dev, "error reporting not enabled due to firewall\n");
		return 0;
	}

	spin_lock_irqsave(&cbb_lock, flags);
	list_add(&cbb->base.node, &cbb_list);
	spin_unlock_irqrestore(&cbb_lock, flags);

	/* set ERD bit to mask SError and generate interrupt to report error */
	if (cbb->fabric->off_mask_erd)
		tegra234_cbb_mask_serror(cbb);

	return tegra_cbb_register(&cbb->base);
}

static int __maybe_unused tegra234_cbb_resume_noirq(struct device *dev)
{
	struct tegra234_cbb *cbb = dev_get_drvdata(dev);

	tegra234_cbb_error_enable(&cbb->base);

	dev_dbg(dev, "%s resumed\n", cbb->fabric->name);

	return 0;
}

static const struct dev_pm_ops tegra234_cbb_pm = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(NULL, tegra234_cbb_resume_noirq)
};

static struct platform_driver tegra234_cbb_driver = {
	.probe = tegra234_cbb_probe,
	.driver = {
		.name = "tegra234-cbb",
		.of_match_table = tegra234_cbb_dt_ids,
		.acpi_match_table = tegra241_cbb_acpi_ids,
		.pm = &tegra234_cbb_pm,
	},
};

static int __init tegra234_cbb_init(void)
{
	return platform_driver_register(&tegra234_cbb_driver);
}
pure_initcall(tegra234_cbb_init);

static void __exit tegra234_cbb_exit(void)
{
	platform_driver_unregister(&tegra234_cbb_driver);
}
module_exit(tegra234_cbb_exit);

MODULE_DESCRIPTION("Control Backbone 2.0 error handling driver for Tegra234");
