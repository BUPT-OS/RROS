// SPDX-License-Identifier: GPL-2.0
/*
 * Implementation of s390 diagnose codes
 *
 * Copyright IBM Corp. 2007
 * Author(s): Michael Holzheu <holzheu@de.ibm.com>
 */

#include <linux/export.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/vmalloc.h>
#include <asm/asm-extable.h>
#include <asm/diag.h>
#include <asm/trace/diag.h>
#include <asm/sections.h>
#include "entry.h"

struct diag_stat {
	unsigned int counter[NR_DIAG_STAT];
};

static DEFINE_PER_CPU(struct diag_stat, diag_stat);

struct diag_desc {
	int code;
	char *name;
};

static const struct diag_desc diag_map[NR_DIAG_STAT] = {
	[DIAG_STAT_X008] = { .code = 0x008, .name = "Console Function" },
	[DIAG_STAT_X00C] = { .code = 0x00c, .name = "Pseudo Timer" },
	[DIAG_STAT_X010] = { .code = 0x010, .name = "Release Pages" },
	[DIAG_STAT_X014] = { .code = 0x014, .name = "Spool File Services" },
	[DIAG_STAT_X044] = { .code = 0x044, .name = "Voluntary Timeslice End" },
	[DIAG_STAT_X064] = { .code = 0x064, .name = "NSS Manipulation" },
	[DIAG_STAT_X08C] = { .code = 0x08c, .name = "Access 3270 Display Device Information" },
	[DIAG_STAT_X09C] = { .code = 0x09c, .name = "Relinquish Timeslice" },
	[DIAG_STAT_X0DC] = { .code = 0x0dc, .name = "Appldata Control" },
	[DIAG_STAT_X204] = { .code = 0x204, .name = "Logical-CPU Utilization" },
	[DIAG_STAT_X210] = { .code = 0x210, .name = "Device Information" },
	[DIAG_STAT_X224] = { .code = 0x224, .name = "EBCDIC-Name Table" },
	[DIAG_STAT_X250] = { .code = 0x250, .name = "Block I/O" },
	[DIAG_STAT_X258] = { .code = 0x258, .name = "Page-Reference Services" },
	[DIAG_STAT_X26C] = { .code = 0x26c, .name = "Certain System Information" },
	[DIAG_STAT_X288] = { .code = 0x288, .name = "Time Bomb" },
	[DIAG_STAT_X2C4] = { .code = 0x2c4, .name = "FTP Services" },
	[DIAG_STAT_X2FC] = { .code = 0x2fc, .name = "Guest Performance Data" },
	[DIAG_STAT_X304] = { .code = 0x304, .name = "Partition-Resource Service" },
	[DIAG_STAT_X308] = { .code = 0x308, .name = "List-Directed IPL" },
	[DIAG_STAT_X318] = { .code = 0x318, .name = "CP Name and Version Codes" },
	[DIAG_STAT_X320] = { .code = 0x320, .name = "Certificate Store" },
	[DIAG_STAT_X500] = { .code = 0x500, .name = "Virtio Service" },
};

struct diag_ops __amode31_ref diag_amode31_ops = {
	.diag210 = _diag210_amode31,
	.diag26c = _diag26c_amode31,
	.diag14 = _diag14_amode31,
	.diag0c = _diag0c_amode31,
	.diag8c = _diag8c_amode31,
	.diag308_reset = _diag308_reset_amode31
};

static struct diag210 _diag210_tmp_amode31 __section(".amode31.data");
struct diag210 __amode31_ref *__diag210_tmp_amode31 = &_diag210_tmp_amode31;

static struct diag8c _diag8c_tmp_amode31 __section(".amode31.data");
static struct diag8c __amode31_ref *__diag8c_tmp_amode31 = &_diag8c_tmp_amode31;

static int show_diag_stat(struct seq_file *m, void *v)
{
	struct diag_stat *stat;
	unsigned long n = (unsigned long) v - 1;
	int cpu, prec, tmp;

	cpus_read_lock();
	if (n == 0) {
		seq_puts(m, "         ");

		for_each_online_cpu(cpu) {
			prec = 10;
			for (tmp = 10; cpu >= tmp; tmp *= 10)
				prec--;
			seq_printf(m, "%*s%d", prec, "CPU", cpu);
		}
		seq_putc(m, '\n');
	} else if (n <= NR_DIAG_STAT) {
		seq_printf(m, "diag %03x:", diag_map[n-1].code);
		for_each_online_cpu(cpu) {
			stat = &per_cpu(diag_stat, cpu);
			seq_printf(m, " %10u", stat->counter[n-1]);
		}
		seq_printf(m, "    %s\n", diag_map[n-1].name);
	}
	cpus_read_unlock();
	return 0;
}

static void *show_diag_stat_start(struct seq_file *m, loff_t *pos)
{
	return *pos <= NR_DIAG_STAT ? (void *)((unsigned long) *pos + 1) : NULL;
}

static void *show_diag_stat_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return show_diag_stat_start(m, pos);
}

static void show_diag_stat_stop(struct seq_file *m, void *v)
{
}

static const struct seq_operations show_diag_stat_sops = {
	.start	= show_diag_stat_start,
	.next	= show_diag_stat_next,
	.stop	= show_diag_stat_stop,
	.show	= show_diag_stat,
};

DEFINE_SEQ_ATTRIBUTE(show_diag_stat);

static int __init show_diag_stat_init(void)
{
	debugfs_create_file("diag_stat", 0400, NULL, NULL,
			    &show_diag_stat_fops);
	return 0;
}

device_initcall(show_diag_stat_init);

void diag_stat_inc(enum diag_stat_enum nr)
{
	this_cpu_inc(diag_stat.counter[nr]);
	trace_s390_diagnose(diag_map[nr].code);
}
EXPORT_SYMBOL(diag_stat_inc);

void notrace diag_stat_inc_norecursion(enum diag_stat_enum nr)
{
	this_cpu_inc(diag_stat.counter[nr]);
	trace_s390_diagnose_norecursion(diag_map[nr].code);
}
EXPORT_SYMBOL(diag_stat_inc_norecursion);

/*
 * Diagnose 14: Input spool file manipulation
 */
int diag14(unsigned long rx, unsigned long ry1, unsigned long subcode)
{
	diag_stat_inc(DIAG_STAT_X014);
	return diag_amode31_ops.diag14(rx, ry1, subcode);
}
EXPORT_SYMBOL(diag14);

static inline int __diag204(unsigned long *subcode, unsigned long size, void *addr)
{
	union register_pair rp = { .even = *subcode, .odd = size };

	asm volatile(
		"	diag	%[addr],%[rp],0x204\n"
		"0:	nopr	%%r7\n"
		EX_TABLE(0b,0b)
		: [rp] "+&d" (rp.pair) : [addr] "d" (addr) : "memory");
	*subcode = rp.even;
	return rp.odd;
}

/**
 * diag204() - Issue diagnose 204 call.
 * @subcode: Subcode of diagnose 204 to be executed.
 * @size: Size of area in pages which @area points to, if given.
 * @addr: Vmalloc'ed memory area where the result is written to.
 *
 * Execute diagnose 204 with the given subcode and write the result to the
 * memory area specified with @addr. For subcodes which do not write a
 * result to memory both @size and @addr must be zero. If @addr is
 * specified it must be page aligned and must have been allocated with
 * vmalloc(). Conversion to real / physical addresses will be handled by
 * this function if required.
 */
int diag204(unsigned long subcode, unsigned long size, void *addr)
{
	if (addr) {
		if (WARN_ON_ONCE(!is_vmalloc_addr(addr)))
			return -1;
		if (WARN_ON_ONCE(!IS_ALIGNED((unsigned long)addr, PAGE_SIZE)))
			return -1;
	}
	if ((subcode & DIAG204_SUBCODE_MASK) == DIAG204_SUBC_STIB4)
		addr = (void *)pfn_to_phys(vmalloc_to_pfn(addr));
	diag_stat_inc(DIAG_STAT_X204);
	size = __diag204(&subcode, size, addr);
	if (subcode)
		return -1;
	return size;
}
EXPORT_SYMBOL(diag204);

/*
 * Diagnose 210: Get information about a virtual device
 */
int diag210(struct diag210 *addr)
{
	static DEFINE_SPINLOCK(diag210_lock);
	unsigned long flags;
	int ccode;

	spin_lock_irqsave(&diag210_lock, flags);
	*__diag210_tmp_amode31 = *addr;

	diag_stat_inc(DIAG_STAT_X210);
	ccode = diag_amode31_ops.diag210(__diag210_tmp_amode31);

	*addr = *__diag210_tmp_amode31;
	spin_unlock_irqrestore(&diag210_lock, flags);

	return ccode;
}
EXPORT_SYMBOL(diag210);

/*
 * Diagnose 8C: Access 3270 Display Device Information
 */
int diag8c(struct diag8c *addr, struct ccw_dev_id *devno)
{
	static DEFINE_SPINLOCK(diag8c_lock);
	unsigned long flags;
	int ccode;

	spin_lock_irqsave(&diag8c_lock, flags);

	diag_stat_inc(DIAG_STAT_X08C);
	ccode = diag_amode31_ops.diag8c(__diag8c_tmp_amode31, devno, sizeof(*addr));

	*addr = *__diag8c_tmp_amode31;
	spin_unlock_irqrestore(&diag8c_lock, flags);

	return ccode;
}
EXPORT_SYMBOL(diag8c);

int diag224(void *ptr)
{
	int rc = -EOPNOTSUPP;

	diag_stat_inc(DIAG_STAT_X224);
	asm volatile(
		"	diag	%1,%2,0x224\n"
		"0:	lhi	%0,0x0\n"
		"1:\n"
		EX_TABLE(0b,1b)
		: "+d" (rc) :"d" (0), "d" (ptr) : "memory");
	return rc;
}
EXPORT_SYMBOL(diag224);

/*
 * Diagnose 26C: Access Certain System Information
 */
int diag26c(void *req, void *resp, enum diag26c_sc subcode)
{
	diag_stat_inc(DIAG_STAT_X26C);
	return diag_amode31_ops.diag26c(req, resp, subcode);
}
EXPORT_SYMBOL(diag26c);
