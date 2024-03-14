/* SPDX-License-Identifier: GPL-2.0 */
/*
 * OS info memory interface
 *
 * Copyright IBM Corp. 2012
 * Author(s): Michael Holzheu <holzheu@linux.vnet.ibm.com>
 */
#ifndef _ASM_S390_OS_INFO_H
#define _ASM_S390_OS_INFO_H

#include <linux/uio.h>

#define OS_INFO_VERSION_MAJOR	1
#define OS_INFO_VERSION_MINOR	1
#define OS_INFO_MAGIC		0x4f53494e464f535aULL /* OSINFOSZ */

#define OS_INFO_VMCOREINFO	0
#define OS_INFO_REIPL_BLOCK	1
#define OS_INFO_FLAGS_ENTRY	2

#define OS_INFO_FLAG_REIPL_CLEAR	(1UL << 0)

struct os_info_entry {
	u64	addr;
	u64	size;
	u32	csum;
} __packed;

struct os_info {
	u64	magic;
	u32	csum;
	u16	version_major;
	u16	version_minor;
	u64	crashkernel_addr;
	u64	crashkernel_size;
	struct os_info_entry entry[3];
	u8	reserved[4004];
} __packed;

void os_info_init(void);
void os_info_entry_add(int nr, void *ptr, u64 len);
void os_info_crashkernel_add(unsigned long base, unsigned long size);
u32 os_info_csum(struct os_info *os_info);

#ifdef CONFIG_CRASH_DUMP
void *os_info_old_entry(int nr, unsigned long *size);
#else
static inline void *os_info_old_entry(int nr, unsigned long *size)
{
	return NULL;
}
#endif

#endif /* _ASM_S390_OS_INFO_H */
