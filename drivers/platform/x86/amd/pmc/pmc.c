// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD SoC Power Management Controller Driver
 *
 * Copyright (c) 2020, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Shyam Sundar S K <Shyam-sundar.S-k@amd.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <asm/amd_nb.h>
#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/serio.h>
#include <linux/suspend.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "pmc.h"

/* SMU communication registers */
#define AMD_PMC_REGISTER_MESSAGE	0x538
#define AMD_PMC_REGISTER_RESPONSE	0x980
#define AMD_PMC_REGISTER_ARGUMENT	0x9BC

/* PMC Scratch Registers */
#define AMD_PMC_SCRATCH_REG_CZN		0x94
#define AMD_PMC_SCRATCH_REG_YC		0xD14

/* STB Registers */
#define AMD_PMC_STB_PMI_0		0x03E30600
#define AMD_PMC_STB_S2IDLE_PREPARE	0xC6000001
#define AMD_PMC_STB_S2IDLE_RESTORE	0xC6000002
#define AMD_PMC_STB_S2IDLE_CHECK	0xC6000003
#define AMD_PMC_STB_DUMMY_PC		0xC6000007

/* STB S2D(Spill to DRAM) has different message port offset */
#define AMD_S2D_REGISTER_MESSAGE	0xA20
#define AMD_S2D_REGISTER_RESPONSE	0xA80
#define AMD_S2D_REGISTER_ARGUMENT	0xA88

/* STB Spill to DRAM Parameters */
#define S2D_TELEMETRY_BYTES_MAX		0x100000
#define S2D_TELEMETRY_DRAMBYTES_MAX	0x1000000

/* Base address of SMU for mapping physical address to virtual address */
#define AMD_PMC_MAPPING_SIZE		0x01000
#define AMD_PMC_BASE_ADDR_OFFSET	0x10000
#define AMD_PMC_BASE_ADDR_LO		0x13B102E8
#define AMD_PMC_BASE_ADDR_HI		0x13B102EC
#define AMD_PMC_BASE_ADDR_LO_MASK	GENMASK(15, 0)
#define AMD_PMC_BASE_ADDR_HI_MASK	GENMASK(31, 20)

/* SMU Response Codes */
#define AMD_PMC_RESULT_OK                    0x01
#define AMD_PMC_RESULT_CMD_REJECT_BUSY       0xFC
#define AMD_PMC_RESULT_CMD_REJECT_PREREQ     0xFD
#define AMD_PMC_RESULT_CMD_UNKNOWN           0xFE
#define AMD_PMC_RESULT_FAILED                0xFF

/* FCH SSC Registers */
#define FCH_S0I3_ENTRY_TIME_L_OFFSET	0x30
#define FCH_S0I3_ENTRY_TIME_H_OFFSET	0x34
#define FCH_S0I3_EXIT_TIME_L_OFFSET	0x38
#define FCH_S0I3_EXIT_TIME_H_OFFSET	0x3C
#define FCH_SSC_MAPPING_SIZE		0x800
#define FCH_BASE_PHY_ADDR_LOW		0xFED81100
#define FCH_BASE_PHY_ADDR_HIGH		0x00000000

/* SMU Message Definations */
#define SMU_MSG_GETSMUVERSION		0x02
#define SMU_MSG_LOG_GETDRAM_ADDR_HI	0x04
#define SMU_MSG_LOG_GETDRAM_ADDR_LO	0x05
#define SMU_MSG_LOG_START		0x06
#define SMU_MSG_LOG_RESET		0x07
#define SMU_MSG_LOG_DUMP_DATA		0x08
#define SMU_MSG_GET_SUP_CONSTRAINTS	0x09
/* List of supported CPU ids */
#define AMD_CPU_ID_RV			0x15D0
#define AMD_CPU_ID_RN			0x1630
#define AMD_CPU_ID_PCO			AMD_CPU_ID_RV
#define AMD_CPU_ID_CZN			AMD_CPU_ID_RN
#define AMD_CPU_ID_YC			0x14B5
#define AMD_CPU_ID_CB			0x14D8
#define AMD_CPU_ID_PS			0x14E8
#define AMD_CPU_ID_SP			0x14A4
#define PCI_DEVICE_ID_AMD_1AH_M20H_ROOT 0x1507

#define PMC_MSG_DELAY_MIN_US		50
#define RESPONSE_REGISTER_LOOP_MAX	20000

#define DELAY_MIN_US		2000
#define DELAY_MAX_US		3000
#define FIFO_SIZE		4096

enum amd_pmc_def {
	MSG_TEST = 0x01,
	MSG_OS_HINT_PCO,
	MSG_OS_HINT_RN,
};

enum s2d_arg {
	S2D_TELEMETRY_SIZE = 0x01,
	S2D_PHYS_ADDR_LOW,
	S2D_PHYS_ADDR_HIGH,
	S2D_NUM_SAMPLES,
	S2D_DRAM_SIZE,
};

struct amd_pmc_bit_map {
	const char *name;
	u32 bit_mask;
};

static const struct amd_pmc_bit_map soc15_ip_blk[] = {
	{"DISPLAY",	BIT(0)},
	{"CPU",		BIT(1)},
	{"GFX",		BIT(2)},
	{"VDD",		BIT(3)},
	{"ACP",		BIT(4)},
	{"VCN",		BIT(5)},
	{"ISP",		BIT(6)},
	{"NBIO",	BIT(7)},
	{"DF",		BIT(8)},
	{"USB3_0",	BIT(9)},
	{"USB3_1",	BIT(10)},
	{"LAPIC",	BIT(11)},
	{"USB3_2",	BIT(12)},
	{"USB3_3",	BIT(13)},
	{"USB3_4",	BIT(14)},
	{"USB4_0",	BIT(15)},
	{"USB4_1",	BIT(16)},
	{"MPM",		BIT(17)},
	{"JPEG",	BIT(18)},
	{"IPU",		BIT(19)},
	{"UMSCH",	BIT(20)},
	{}
};

static bool enable_stb;
module_param(enable_stb, bool, 0644);
MODULE_PARM_DESC(enable_stb, "Enable the STB debug mechanism");

static bool disable_workarounds;
module_param(disable_workarounds, bool, 0644);
MODULE_PARM_DESC(disable_workarounds, "Disable workarounds for platform bugs");

static struct amd_pmc_dev pmc;
static int amd_pmc_send_cmd(struct amd_pmc_dev *dev, u32 arg, u32 *data, u8 msg, bool ret);
static int amd_pmc_read_stb(struct amd_pmc_dev *dev, u32 *buf);
static int amd_pmc_write_stb(struct amd_pmc_dev *dev, u32 data);

static inline u32 amd_pmc_reg_read(struct amd_pmc_dev *dev, int reg_offset)
{
	return ioread32(dev->regbase + reg_offset);
}

static inline void amd_pmc_reg_write(struct amd_pmc_dev *dev, int reg_offset, u32 val)
{
	iowrite32(val, dev->regbase + reg_offset);
}

struct smu_metrics {
	u32 table_version;
	u32 hint_count;
	u32 s0i3_last_entry_status;
	u32 timein_s0i2;
	u64 timeentering_s0i3_lastcapture;
	u64 timeentering_s0i3_totaltime;
	u64 timeto_resume_to_os_lastcapture;
	u64 timeto_resume_to_os_totaltime;
	u64 timein_s0i3_lastcapture;
	u64 timein_s0i3_totaltime;
	u64 timein_swdrips_lastcapture;
	u64 timein_swdrips_totaltime;
	u64 timecondition_notmet_lastcapture[32];
	u64 timecondition_notmet_totaltime[32];
} __packed;

static int amd_pmc_stb_debugfs_open(struct inode *inode, struct file *filp)
{
	struct amd_pmc_dev *dev = filp->f_inode->i_private;
	u32 size = FIFO_SIZE * sizeof(u32);
	u32 *buf;
	int rc;

	buf = kzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	rc = amd_pmc_read_stb(dev, buf);
	if (rc) {
		kfree(buf);
		return rc;
	}

	filp->private_data = buf;
	return rc;
}

static ssize_t amd_pmc_stb_debugfs_read(struct file *filp, char __user *buf, size_t size,
					loff_t *pos)
{
	if (!filp->private_data)
		return -EINVAL;

	return simple_read_from_buffer(buf, size, pos, filp->private_data,
				       FIFO_SIZE * sizeof(u32));
}

static int amd_pmc_stb_debugfs_release(struct inode *inode, struct file *filp)
{
	kfree(filp->private_data);
	return 0;
}

static const struct file_operations amd_pmc_stb_debugfs_fops = {
	.owner = THIS_MODULE,
	.open = amd_pmc_stb_debugfs_open,
	.read = amd_pmc_stb_debugfs_read,
	.release = amd_pmc_stb_debugfs_release,
};

static int amd_pmc_stb_debugfs_open_v2(struct inode *inode, struct file *filp)
{
	struct amd_pmc_dev *dev = filp->f_inode->i_private;
	u32 *buf, fsize, num_samples, stb_rdptr_offset = 0;
	int ret;

	/* Write dummy postcode while reading the STB buffer */
	ret = amd_pmc_write_stb(dev, AMD_PMC_STB_DUMMY_PC);
	if (ret)
		dev_err(dev->dev, "error writing to STB: %d\n", ret);

	buf = kzalloc(S2D_TELEMETRY_BYTES_MAX, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* Spill to DRAM num_samples uses separate SMU message port */
	dev->msg_port = 1;

	/* Get the num_samples to calculate the last push location */
	ret = amd_pmc_send_cmd(dev, S2D_NUM_SAMPLES, &num_samples, dev->s2d_msg_id, true);
	/* Clear msg_port for other SMU operation */
	dev->msg_port = 0;
	if (ret) {
		dev_err(dev->dev, "error: S2D_NUM_SAMPLES not supported : %d\n", ret);
		kfree(buf);
		return ret;
	}

	/* Start capturing data from the last push location */
	if (num_samples > S2D_TELEMETRY_BYTES_MAX) {
		fsize  = S2D_TELEMETRY_BYTES_MAX;
		stb_rdptr_offset = num_samples - fsize;
	} else {
		fsize = num_samples;
		stb_rdptr_offset = 0;
	}

	memcpy_fromio(buf, dev->stb_virt_addr + stb_rdptr_offset, fsize);
	filp->private_data = buf;

	return 0;
}

static ssize_t amd_pmc_stb_debugfs_read_v2(struct file *filp, char __user *buf, size_t size,
					   loff_t *pos)
{
	if (!filp->private_data)
		return -EINVAL;

	return simple_read_from_buffer(buf, size, pos, filp->private_data,
					S2D_TELEMETRY_BYTES_MAX);
}

static int amd_pmc_stb_debugfs_release_v2(struct inode *inode, struct file *filp)
{
	kfree(filp->private_data);
	return 0;
}

static const struct file_operations amd_pmc_stb_debugfs_fops_v2 = {
	.owner = THIS_MODULE,
	.open = amd_pmc_stb_debugfs_open_v2,
	.read = amd_pmc_stb_debugfs_read_v2,
	.release = amd_pmc_stb_debugfs_release_v2,
};

static void amd_pmc_get_ip_info(struct amd_pmc_dev *dev)
{
	switch (dev->cpu_id) {
	case AMD_CPU_ID_PCO:
	case AMD_CPU_ID_RN:
	case AMD_CPU_ID_YC:
	case AMD_CPU_ID_CB:
		dev->num_ips = 12;
		dev->s2d_msg_id = 0xBE;
		break;
	case AMD_CPU_ID_PS:
		dev->num_ips = 21;
		dev->s2d_msg_id = 0x85;
		break;
	}
}

static int amd_pmc_setup_smu_logging(struct amd_pmc_dev *dev)
{
	if (dev->cpu_id == AMD_CPU_ID_PCO) {
		dev_warn_once(dev->dev, "SMU debugging info not supported on this platform\n");
		return -EINVAL;
	}

	/* Get Active devices list from SMU */
	if (!dev->active_ips)
		amd_pmc_send_cmd(dev, 0, &dev->active_ips, SMU_MSG_GET_SUP_CONSTRAINTS, true);

	/* Get dram address */
	if (!dev->smu_virt_addr) {
		u32 phys_addr_low, phys_addr_hi;
		u64 smu_phys_addr;

		amd_pmc_send_cmd(dev, 0, &phys_addr_low, SMU_MSG_LOG_GETDRAM_ADDR_LO, true);
		amd_pmc_send_cmd(dev, 0, &phys_addr_hi, SMU_MSG_LOG_GETDRAM_ADDR_HI, true);
		smu_phys_addr = ((u64)phys_addr_hi << 32 | phys_addr_low);

		dev->smu_virt_addr = devm_ioremap(dev->dev, smu_phys_addr,
						  sizeof(struct smu_metrics));
		if (!dev->smu_virt_addr)
			return -ENOMEM;
	}

	/* Start the logging */
	amd_pmc_send_cmd(dev, 0, NULL, SMU_MSG_LOG_RESET, false);
	amd_pmc_send_cmd(dev, 0, NULL, SMU_MSG_LOG_START, false);

	return 0;
}

static int get_metrics_table(struct amd_pmc_dev *pdev, struct smu_metrics *table)
{
	if (!pdev->smu_virt_addr) {
		int ret = amd_pmc_setup_smu_logging(pdev);

		if (ret)
			return ret;
	}

	if (pdev->cpu_id == AMD_CPU_ID_PCO)
		return -ENODEV;
	memcpy_fromio(table, pdev->smu_virt_addr, sizeof(struct smu_metrics));
	return 0;
}

static void amd_pmc_validate_deepest(struct amd_pmc_dev *pdev)
{
	struct smu_metrics table;

	if (get_metrics_table(pdev, &table))
		return;

	if (!table.s0i3_last_entry_status)
		dev_warn(pdev->dev, "Last suspend didn't reach deepest state\n");
	pm_report_hw_sleep_time(table.s0i3_last_entry_status ?
				table.timein_s0i3_lastcapture : 0);
}

static int amd_pmc_get_smu_version(struct amd_pmc_dev *dev)
{
	int rc;
	u32 val;

	if (dev->cpu_id == AMD_CPU_ID_PCO)
		return -ENODEV;

	rc = amd_pmc_send_cmd(dev, 0, &val, SMU_MSG_GETSMUVERSION, true);
	if (rc)
		return rc;

	dev->smu_program = (val >> 24) & GENMASK(7, 0);
	dev->major = (val >> 16) & GENMASK(7, 0);
	dev->minor = (val >> 8) & GENMASK(7, 0);
	dev->rev = (val >> 0) & GENMASK(7, 0);

	dev_dbg(dev->dev, "SMU program %u version is %u.%u.%u\n",
		dev->smu_program, dev->major, dev->minor, dev->rev);

	return 0;
}

static ssize_t smu_fw_version_show(struct device *d, struct device_attribute *attr,
				   char *buf)
{
	struct amd_pmc_dev *dev = dev_get_drvdata(d);

	if (!dev->major) {
		int rc = amd_pmc_get_smu_version(dev);

		if (rc)
			return rc;
	}
	return sysfs_emit(buf, "%u.%u.%u\n", dev->major, dev->minor, dev->rev);
}

static ssize_t smu_program_show(struct device *d, struct device_attribute *attr,
				   char *buf)
{
	struct amd_pmc_dev *dev = dev_get_drvdata(d);

	if (!dev->major) {
		int rc = amd_pmc_get_smu_version(dev);

		if (rc)
			return rc;
	}
	return sysfs_emit(buf, "%u\n", dev->smu_program);
}

static DEVICE_ATTR_RO(smu_fw_version);
static DEVICE_ATTR_RO(smu_program);

static umode_t pmc_attr_is_visible(struct kobject *kobj, struct attribute *attr, int idx)
{
	struct device *dev = kobj_to_dev(kobj);
	struct amd_pmc_dev *pdev = dev_get_drvdata(dev);

	if (pdev->cpu_id == AMD_CPU_ID_PCO)
		return 0;
	return 0444;
}

static struct attribute *pmc_attrs[] = {
	&dev_attr_smu_fw_version.attr,
	&dev_attr_smu_program.attr,
	NULL,
};

static struct attribute_group pmc_attr_group = {
	.attrs = pmc_attrs,
	.is_visible = pmc_attr_is_visible,
};

static const struct attribute_group *pmc_groups[] = {
	&pmc_attr_group,
	NULL,
};

static int smu_fw_info_show(struct seq_file *s, void *unused)
{
	struct amd_pmc_dev *dev = s->private;
	struct smu_metrics table;
	int idx;

	if (get_metrics_table(dev, &table))
		return -EINVAL;

	seq_puts(s, "\n=== SMU Statistics ===\n");
	seq_printf(s, "Table Version: %d\n", table.table_version);
	seq_printf(s, "Hint Count: %d\n", table.hint_count);
	seq_printf(s, "Last S0i3 Status: %s\n", table.s0i3_last_entry_status ? "Success" :
		   "Unknown/Fail");
	seq_printf(s, "Time (in us) to S0i3: %lld\n", table.timeentering_s0i3_lastcapture);
	seq_printf(s, "Time (in us) in S0i3: %lld\n", table.timein_s0i3_lastcapture);
	seq_printf(s, "Time (in us) to resume from S0i3: %lld\n",
		   table.timeto_resume_to_os_lastcapture);

	seq_puts(s, "\n=== Active time (in us) ===\n");
	for (idx = 0 ; idx < dev->num_ips ; idx++) {
		if (soc15_ip_blk[idx].bit_mask & dev->active_ips)
			seq_printf(s, "%-8s : %lld\n", soc15_ip_blk[idx].name,
				   table.timecondition_notmet_lastcapture[idx]);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(smu_fw_info);

static int s0ix_stats_show(struct seq_file *s, void *unused)
{
	struct amd_pmc_dev *dev = s->private;
	u64 entry_time, exit_time, residency;

	/* Use FCH registers to get the S0ix stats */
	if (!dev->fch_virt_addr) {
		u32 base_addr_lo = FCH_BASE_PHY_ADDR_LOW;
		u32 base_addr_hi = FCH_BASE_PHY_ADDR_HIGH;
		u64 fch_phys_addr = ((u64)base_addr_hi << 32 | base_addr_lo);

		dev->fch_virt_addr = devm_ioremap(dev->dev, fch_phys_addr, FCH_SSC_MAPPING_SIZE);
		if (!dev->fch_virt_addr)
			return -ENOMEM;
	}

	entry_time = ioread32(dev->fch_virt_addr + FCH_S0I3_ENTRY_TIME_H_OFFSET);
	entry_time = entry_time << 32 | ioread32(dev->fch_virt_addr + FCH_S0I3_ENTRY_TIME_L_OFFSET);

	exit_time = ioread32(dev->fch_virt_addr + FCH_S0I3_EXIT_TIME_H_OFFSET);
	exit_time = exit_time << 32 | ioread32(dev->fch_virt_addr + FCH_S0I3_EXIT_TIME_L_OFFSET);

	/* It's in 48MHz. We need to convert it */
	residency = exit_time - entry_time;
	do_div(residency, 48);

	seq_puts(s, "=== S0ix statistics ===\n");
	seq_printf(s, "S0ix Entry Time: %lld\n", entry_time);
	seq_printf(s, "S0ix Exit Time: %lld\n", exit_time);
	seq_printf(s, "Residency Time: %lld\n", residency);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(s0ix_stats);

static int amd_pmc_idlemask_read(struct amd_pmc_dev *pdev, struct device *dev,
				 struct seq_file *s)
{
	u32 val;
	int rc;

	switch (pdev->cpu_id) {
	case AMD_CPU_ID_CZN:
		/* we haven't yet read SMU version */
		if (!pdev->major) {
			rc = amd_pmc_get_smu_version(pdev);
			if (rc)
				return rc;
		}
		if (pdev->major > 56 || (pdev->major >= 55 && pdev->minor >= 37))
			val = amd_pmc_reg_read(pdev, AMD_PMC_SCRATCH_REG_CZN);
		else
			return -EINVAL;
		break;
	case AMD_CPU_ID_YC:
	case AMD_CPU_ID_CB:
	case AMD_CPU_ID_PS:
		val = amd_pmc_reg_read(pdev, AMD_PMC_SCRATCH_REG_YC);
		break;
	default:
		return -EINVAL;
	}

	if (dev)
		pm_pr_dbg("SMU idlemask s0i3: 0x%x\n", val);

	if (s)
		seq_printf(s, "SMU idlemask : 0x%x\n", val);

	return 0;
}

static int amd_pmc_idlemask_show(struct seq_file *s, void *unused)
{
	return amd_pmc_idlemask_read(s->private, NULL, s);
}
DEFINE_SHOW_ATTRIBUTE(amd_pmc_idlemask);

static void amd_pmc_dbgfs_unregister(struct amd_pmc_dev *dev)
{
	debugfs_remove_recursive(dev->dbgfs_dir);
}

static bool amd_pmc_is_stb_supported(struct amd_pmc_dev *dev)
{
	switch (dev->cpu_id) {
	case AMD_CPU_ID_YC:
	case AMD_CPU_ID_CB:
	case AMD_CPU_ID_PS:
		return true;
	default:
		return false;
	}
}

static void amd_pmc_dbgfs_register(struct amd_pmc_dev *dev)
{
	dev->dbgfs_dir = debugfs_create_dir("amd_pmc", NULL);
	debugfs_create_file("smu_fw_info", 0644, dev->dbgfs_dir, dev,
			    &smu_fw_info_fops);
	debugfs_create_file("s0ix_stats", 0644, dev->dbgfs_dir, dev,
			    &s0ix_stats_fops);
	debugfs_create_file("amd_pmc_idlemask", 0644, dev->dbgfs_dir, dev,
			    &amd_pmc_idlemask_fops);
	/* Enable STB only when the module_param is set */
	if (enable_stb) {
		if (amd_pmc_is_stb_supported(dev))
			debugfs_create_file("stb_read", 0644, dev->dbgfs_dir, dev,
					    &amd_pmc_stb_debugfs_fops_v2);
		else
			debugfs_create_file("stb_read", 0644, dev->dbgfs_dir, dev,
					    &amd_pmc_stb_debugfs_fops);
	}
}

static void amd_pmc_dump_registers(struct amd_pmc_dev *dev)
{
	u32 value, message, argument, response;

	if (dev->msg_port) {
		message = AMD_S2D_REGISTER_MESSAGE;
		argument = AMD_S2D_REGISTER_ARGUMENT;
		response = AMD_S2D_REGISTER_RESPONSE;
	} else {
		message = AMD_PMC_REGISTER_MESSAGE;
		argument = AMD_PMC_REGISTER_ARGUMENT;
		response = AMD_PMC_REGISTER_RESPONSE;
	}

	value = amd_pmc_reg_read(dev, response);
	dev_dbg(dev->dev, "AMD_%s_REGISTER_RESPONSE:%x\n", dev->msg_port ? "S2D" : "PMC", value);

	value = amd_pmc_reg_read(dev, argument);
	dev_dbg(dev->dev, "AMD_%s_REGISTER_ARGUMENT:%x\n", dev->msg_port ? "S2D" : "PMC", value);

	value = amd_pmc_reg_read(dev, message);
	dev_dbg(dev->dev, "AMD_%s_REGISTER_MESSAGE:%x\n", dev->msg_port ? "S2D" : "PMC", value);
}

static int amd_pmc_send_cmd(struct amd_pmc_dev *dev, u32 arg, u32 *data, u8 msg, bool ret)
{
	int rc;
	u32 val, message, argument, response;

	mutex_lock(&dev->lock);

	if (dev->msg_port) {
		message = AMD_S2D_REGISTER_MESSAGE;
		argument = AMD_S2D_REGISTER_ARGUMENT;
		response = AMD_S2D_REGISTER_RESPONSE;
	} else {
		message = AMD_PMC_REGISTER_MESSAGE;
		argument = AMD_PMC_REGISTER_ARGUMENT;
		response = AMD_PMC_REGISTER_RESPONSE;
	}

	/* Wait until we get a valid response */
	rc = readx_poll_timeout(ioread32, dev->regbase + response,
				val, val != 0, PMC_MSG_DELAY_MIN_US,
				PMC_MSG_DELAY_MIN_US * RESPONSE_REGISTER_LOOP_MAX);
	if (rc) {
		dev_err(dev->dev, "failed to talk to SMU\n");
		goto out_unlock;
	}

	/* Write zero to response register */
	amd_pmc_reg_write(dev, response, 0);

	/* Write argument into response register */
	amd_pmc_reg_write(dev, argument, arg);

	/* Write message ID to message ID register */
	amd_pmc_reg_write(dev, message, msg);

	/* Wait until we get a valid response */
	rc = readx_poll_timeout(ioread32, dev->regbase + response,
				val, val != 0, PMC_MSG_DELAY_MIN_US,
				PMC_MSG_DELAY_MIN_US * RESPONSE_REGISTER_LOOP_MAX);
	if (rc) {
		dev_err(dev->dev, "SMU response timed out\n");
		goto out_unlock;
	}

	switch (val) {
	case AMD_PMC_RESULT_OK:
		if (ret) {
			/* PMFW may take longer time to return back the data */
			usleep_range(DELAY_MIN_US, 10 * DELAY_MAX_US);
			*data = amd_pmc_reg_read(dev, argument);
		}
		break;
	case AMD_PMC_RESULT_CMD_REJECT_BUSY:
		dev_err(dev->dev, "SMU not ready. err: 0x%x\n", val);
		rc = -EBUSY;
		goto out_unlock;
	case AMD_PMC_RESULT_CMD_UNKNOWN:
		dev_err(dev->dev, "SMU cmd unknown. err: 0x%x\n", val);
		rc = -EINVAL;
		goto out_unlock;
	case AMD_PMC_RESULT_CMD_REJECT_PREREQ:
	case AMD_PMC_RESULT_FAILED:
	default:
		dev_err(dev->dev, "SMU cmd failed. err: 0x%x\n", val);
		rc = -EIO;
		goto out_unlock;
	}

out_unlock:
	mutex_unlock(&dev->lock);
	amd_pmc_dump_registers(dev);
	return rc;
}

static int amd_pmc_get_os_hint(struct amd_pmc_dev *dev)
{
	switch (dev->cpu_id) {
	case AMD_CPU_ID_PCO:
		return MSG_OS_HINT_PCO;
	case AMD_CPU_ID_RN:
	case AMD_CPU_ID_YC:
	case AMD_CPU_ID_CB:
	case AMD_CPU_ID_PS:
		return MSG_OS_HINT_RN;
	}
	return -EINVAL;
}

static int amd_pmc_czn_wa_irq1(struct amd_pmc_dev *pdev)
{
	struct device *d;
	int rc;

	if (!pdev->major) {
		rc = amd_pmc_get_smu_version(pdev);
		if (rc)
			return rc;
	}

	if (pdev->major > 64 || (pdev->major == 64 && pdev->minor > 65))
		return 0;

	d = bus_find_device_by_name(&serio_bus, NULL, "serio0");
	if (!d)
		return 0;
	if (device_may_wakeup(d)) {
		dev_info_once(d, "Disabling IRQ1 wakeup source to avoid platform firmware bug\n");
		disable_irq_wake(1);
		device_set_wakeup_enable(d, false);
	}
	put_device(d);

	return 0;
}

static int amd_pmc_verify_czn_rtc(struct amd_pmc_dev *pdev, u32 *arg)
{
	struct rtc_device *rtc_device;
	time64_t then, now, duration;
	struct rtc_wkalrm alarm;
	struct rtc_time tm;
	int rc;

	/* we haven't yet read SMU version */
	if (!pdev->major) {
		rc = amd_pmc_get_smu_version(pdev);
		if (rc)
			return rc;
	}

	if (pdev->major < 64 || (pdev->major == 64 && pdev->minor < 53))
		return 0;

	rtc_device = rtc_class_open("rtc0");
	if (!rtc_device)
		return 0;
	rc = rtc_read_alarm(rtc_device, &alarm);
	if (rc)
		return rc;
	if (!alarm.enabled) {
		dev_dbg(pdev->dev, "alarm not enabled\n");
		return 0;
	}
	rc = rtc_read_time(rtc_device, &tm);
	if (rc)
		return rc;
	then = rtc_tm_to_time64(&alarm.time);
	now = rtc_tm_to_time64(&tm);
	duration = then-now;

	/* in the past */
	if (then < now)
		return 0;

	/* will be stored in upper 16 bits of s0i3 hint argument,
	 * so timer wakeup from s0i3 is limited to ~18 hours or less
	 */
	if (duration <= 4 || duration > U16_MAX)
		return -EINVAL;

	*arg |= (duration << 16);
	rc = rtc_alarm_irq_enable(rtc_device, 0);
	pm_pr_dbg("wakeup timer programmed for %lld seconds\n", duration);

	return rc;
}

static void amd_pmc_s2idle_prepare(void)
{
	struct amd_pmc_dev *pdev = &pmc;
	int rc;
	u8 msg;
	u32 arg = 1;

	/* Reset and Start SMU logging - to monitor the s0i3 stats */
	amd_pmc_setup_smu_logging(pdev);

	/* Activate CZN specific platform bug workarounds */
	if (pdev->cpu_id == AMD_CPU_ID_CZN && !disable_workarounds) {
		rc = amd_pmc_verify_czn_rtc(pdev, &arg);
		if (rc) {
			dev_err(pdev->dev, "failed to set RTC: %d\n", rc);
			return;
		}
	}

	msg = amd_pmc_get_os_hint(pdev);
	rc = amd_pmc_send_cmd(pdev, arg, NULL, msg, false);
	if (rc) {
		dev_err(pdev->dev, "suspend failed: %d\n", rc);
		return;
	}

	rc = amd_pmc_write_stb(pdev, AMD_PMC_STB_S2IDLE_PREPARE);
	if (rc)
		dev_err(pdev->dev, "error writing to STB: %d\n", rc);
}

static void amd_pmc_s2idle_check(void)
{
	struct amd_pmc_dev *pdev = &pmc;
	struct smu_metrics table;
	int rc;

	/* CZN: Ensure that future s0i3 entry attempts at least 10ms passed */
	if (pdev->cpu_id == AMD_CPU_ID_CZN && !get_metrics_table(pdev, &table) &&
	    table.s0i3_last_entry_status)
		usleep_range(10000, 20000);

	/* Dump the IdleMask before we add to the STB */
	amd_pmc_idlemask_read(pdev, pdev->dev, NULL);

	rc = amd_pmc_write_stb(pdev, AMD_PMC_STB_S2IDLE_CHECK);
	if (rc)
		dev_err(pdev->dev, "error writing to STB: %d\n", rc);
}

static int amd_pmc_dump_data(struct amd_pmc_dev *pdev)
{
	if (pdev->cpu_id == AMD_CPU_ID_PCO)
		return -ENODEV;

	return amd_pmc_send_cmd(pdev, 0, NULL, SMU_MSG_LOG_DUMP_DATA, false);
}

static void amd_pmc_s2idle_restore(void)
{
	struct amd_pmc_dev *pdev = &pmc;
	int rc;
	u8 msg;

	msg = amd_pmc_get_os_hint(pdev);
	rc = amd_pmc_send_cmd(pdev, 0, NULL, msg, false);
	if (rc)
		dev_err(pdev->dev, "resume failed: %d\n", rc);

	/* Let SMU know that we are looking for stats */
	amd_pmc_dump_data(pdev);

	rc = amd_pmc_write_stb(pdev, AMD_PMC_STB_S2IDLE_RESTORE);
	if (rc)
		dev_err(pdev->dev, "error writing to STB: %d\n", rc);

	/* Notify on failed entry */
	amd_pmc_validate_deepest(pdev);

	amd_pmc_process_restore_quirks(pdev);
}

static struct acpi_s2idle_dev_ops amd_pmc_s2idle_dev_ops = {
	.prepare = amd_pmc_s2idle_prepare,
	.check = amd_pmc_s2idle_check,
	.restore = amd_pmc_s2idle_restore,
};

static int amd_pmc_suspend_handler(struct device *dev)
{
	struct amd_pmc_dev *pdev = dev_get_drvdata(dev);

	if (pdev->cpu_id == AMD_CPU_ID_CZN && !disable_workarounds) {
		int rc = amd_pmc_czn_wa_irq1(pdev);

		if (rc) {
			dev_err(pdev->dev, "failed to adjust keyboard wakeup: %d\n", rc);
			return rc;
		}
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(amd_pmc_pm, amd_pmc_suspend_handler, NULL);

static const struct pci_device_id pmc_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_PS) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_CB) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_YC) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_CZN) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_RN) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_PCO) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_RV) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, AMD_CPU_ID_SP) },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_1AH_M20H_ROOT) },
	{ }
};

static int amd_pmc_get_dram_size(struct amd_pmc_dev *dev)
{
	int ret;

	switch (dev->cpu_id) {
	case AMD_CPU_ID_YC:
		if (!(dev->major > 90 || (dev->major == 90 && dev->minor > 39))) {
			ret = -EINVAL;
			goto err_dram_size;
		}
		break;
	default:
		ret = -EINVAL;
		goto err_dram_size;
	}

	ret = amd_pmc_send_cmd(dev, S2D_DRAM_SIZE, &dev->dram_size, dev->s2d_msg_id, true);
	if (ret || !dev->dram_size)
		goto err_dram_size;

	return 0;

err_dram_size:
	dev_err(dev->dev, "DRAM size command not supported for this platform\n");
	return ret;
}

static int amd_pmc_s2d_init(struct amd_pmc_dev *dev)
{
	u32 phys_addr_low, phys_addr_hi;
	u64 stb_phys_addr;
	u32 size = 0;
	int ret;

	/* Spill to DRAM feature uses separate SMU message port */
	dev->msg_port = 1;

	/* Get num of IP blocks within the SoC */
	amd_pmc_get_ip_info(dev);

	amd_pmc_send_cmd(dev, S2D_TELEMETRY_SIZE, &size, dev->s2d_msg_id, true);
	if (size != S2D_TELEMETRY_BYTES_MAX)
		return -EIO;

	/* Get DRAM size */
	ret = amd_pmc_get_dram_size(dev);
	if (ret)
		dev->dram_size = S2D_TELEMETRY_DRAMBYTES_MAX;

	/* Get STB DRAM address */
	amd_pmc_send_cmd(dev, S2D_PHYS_ADDR_LOW, &phys_addr_low, dev->s2d_msg_id, true);
	amd_pmc_send_cmd(dev, S2D_PHYS_ADDR_HIGH, &phys_addr_hi, dev->s2d_msg_id, true);

	stb_phys_addr = ((u64)phys_addr_hi << 32 | phys_addr_low);

	/* Clear msg_port for other SMU operation */
	dev->msg_port = 0;

	dev->stb_virt_addr = devm_ioremap(dev->dev, stb_phys_addr, dev->dram_size);
	if (!dev->stb_virt_addr)
		return -ENOMEM;

	return 0;
}

static int amd_pmc_write_stb(struct amd_pmc_dev *dev, u32 data)
{
	int err;

	err = amd_smn_write(0, AMD_PMC_STB_PMI_0, data);
	if (err) {
		dev_err(dev->dev, "failed to write data in stb: 0x%X\n", AMD_PMC_STB_PMI_0);
		return pcibios_err_to_errno(err);
	}

	return 0;
}

static int amd_pmc_read_stb(struct amd_pmc_dev *dev, u32 *buf)
{
	int i, err;

	for (i = 0; i < FIFO_SIZE; i++) {
		err = amd_smn_read(0, AMD_PMC_STB_PMI_0, buf++);
		if (err) {
			dev_err(dev->dev, "error reading data from stb: 0x%X\n", AMD_PMC_STB_PMI_0);
			return pcibios_err_to_errno(err);
		}
	}

	return 0;
}

static int amd_pmc_probe(struct platform_device *pdev)
{
	struct amd_pmc_dev *dev = &pmc;
	struct pci_dev *rdev;
	u32 base_addr_lo, base_addr_hi;
	u64 base_addr;
	int err;
	u32 val;

	dev->dev = &pdev->dev;

	rdev = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0));
	if (!rdev || !pci_match_id(pmc_pci_ids, rdev)) {
		err = -ENODEV;
		goto err_pci_dev_put;
	}

	dev->cpu_id = rdev->device;

	if (dev->cpu_id == AMD_CPU_ID_SP) {
		dev_warn_once(dev->dev, "S0i3 is not supported on this hardware\n");
		err = -ENODEV;
		goto err_pci_dev_put;
	}

	dev->rdev = rdev;
	err = amd_smn_read(0, AMD_PMC_BASE_ADDR_LO, &val);
	if (err) {
		dev_err(dev->dev, "error reading 0x%x\n", AMD_PMC_BASE_ADDR_LO);
		err = pcibios_err_to_errno(err);
		goto err_pci_dev_put;
	}

	base_addr_lo = val & AMD_PMC_BASE_ADDR_HI_MASK;

	err = amd_smn_read(0, AMD_PMC_BASE_ADDR_HI, &val);
	if (err) {
		dev_err(dev->dev, "error reading 0x%x\n", AMD_PMC_BASE_ADDR_HI);
		err = pcibios_err_to_errno(err);
		goto err_pci_dev_put;
	}

	base_addr_hi = val & AMD_PMC_BASE_ADDR_LO_MASK;
	base_addr = ((u64)base_addr_hi << 32 | base_addr_lo);

	dev->regbase = devm_ioremap(dev->dev, base_addr + AMD_PMC_BASE_ADDR_OFFSET,
				    AMD_PMC_MAPPING_SIZE);
	if (!dev->regbase) {
		err = -ENOMEM;
		goto err_pci_dev_put;
	}

	mutex_init(&dev->lock);

	if (enable_stb && amd_pmc_is_stb_supported(dev)) {
		err = amd_pmc_s2d_init(dev);
		if (err)
			goto err_pci_dev_put;
	}

	platform_set_drvdata(pdev, dev);
	if (IS_ENABLED(CONFIG_SUSPEND)) {
		err = acpi_register_lps0_dev(&amd_pmc_s2idle_dev_ops);
		if (err)
			dev_warn(dev->dev, "failed to register LPS0 sleep handler, expect increased power consumption\n");
		if (!disable_workarounds)
			amd_pmc_quirks_init(dev);
	}

	amd_pmc_dbgfs_register(dev);
	pm_report_max_hw_sleep(U64_MAX);
	return 0;

err_pci_dev_put:
	pci_dev_put(rdev);
	return err;
}

static void amd_pmc_remove(struct platform_device *pdev)
{
	struct amd_pmc_dev *dev = platform_get_drvdata(pdev);

	if (IS_ENABLED(CONFIG_SUSPEND))
		acpi_unregister_lps0_dev(&amd_pmc_s2idle_dev_ops);
	amd_pmc_dbgfs_unregister(dev);
	pci_dev_put(dev->rdev);
	mutex_destroy(&dev->lock);
}

static const struct acpi_device_id amd_pmc_acpi_ids[] = {
	{"AMDI0005", 0},
	{"AMDI0006", 0},
	{"AMDI0007", 0},
	{"AMDI0008", 0},
	{"AMDI0009", 0},
	{"AMDI000A", 0},
	{"AMD0004", 0},
	{"AMD0005", 0},
	{ }
};
MODULE_DEVICE_TABLE(acpi, amd_pmc_acpi_ids);

static struct platform_driver amd_pmc_driver = {
	.driver = {
		.name = "amd_pmc",
		.acpi_match_table = amd_pmc_acpi_ids,
		.dev_groups = pmc_groups,
		.pm = pm_sleep_ptr(&amd_pmc_pm),
	},
	.probe = amd_pmc_probe,
	.remove_new = amd_pmc_remove,
};
module_platform_driver(amd_pmc_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("AMD PMC Driver");
