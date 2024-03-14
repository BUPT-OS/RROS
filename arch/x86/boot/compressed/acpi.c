// SPDX-License-Identifier: GPL-2.0
#define BOOT_CTYPE_H
#include "misc.h"
#include "error.h"
#include "../string.h"
#include "efi.h"

#include <linux/numa.h>

/*
 * Longest parameter of 'acpi=' is 'copy_dsdt', plus an extra '\0'
 * for termination.
 */
#define MAX_ACPI_ARG_LENGTH 10

/*
 * Immovable memory regions representation. Max amount of memory regions is
 * MAX_NUMNODES*2.
 */
struct mem_vector immovable_mem[MAX_NUMNODES*2];

static acpi_physical_address
__efi_get_rsdp_addr(unsigned long cfg_tbl_pa, unsigned int cfg_tbl_len)
{
#ifdef CONFIG_EFI
	unsigned long rsdp_addr;
	int ret;

	/*
	 * Search EFI system tables for RSDP. Preferred is ACPI_20_TABLE_GUID to
	 * ACPI_TABLE_GUID because it has more features.
	 */
	rsdp_addr = efi_find_vendor_table(boot_params, cfg_tbl_pa, cfg_tbl_len,
					  ACPI_20_TABLE_GUID);
	if (rsdp_addr)
		return (acpi_physical_address)rsdp_addr;

	/* No ACPI_20_TABLE_GUID found, fallback to ACPI_TABLE_GUID. */
	rsdp_addr = efi_find_vendor_table(boot_params, cfg_tbl_pa, cfg_tbl_len,
					  ACPI_TABLE_GUID);
	if (rsdp_addr)
		return (acpi_physical_address)rsdp_addr;

	debug_putstr("Error getting RSDP address.\n");
#endif
	return 0;
}

static acpi_physical_address efi_get_rsdp_addr(void)
{
#ifdef CONFIG_EFI
	unsigned long cfg_tbl_pa = 0;
	unsigned int cfg_tbl_len;
	unsigned long systab_pa;
	unsigned int nr_tables;
	enum efi_type et;
	int ret;

	et = efi_get_type(boot_params);
	if (et == EFI_TYPE_NONE)
		return 0;

	systab_pa = efi_get_system_table(boot_params);
	if (!systab_pa)
		error("EFI support advertised, but unable to locate system table.");

	ret = efi_get_conf_table(boot_params, &cfg_tbl_pa, &cfg_tbl_len);
	if (ret || !cfg_tbl_pa)
		error("EFI config table not found.");

	return __efi_get_rsdp_addr(cfg_tbl_pa, cfg_tbl_len);
#else
	return 0;
#endif
}

static u8 compute_checksum(u8 *buffer, u32 length)
{
	u8 *end = buffer + length;
	u8 sum = 0;

	while (buffer < end)
		sum += *(buffer++);

	return sum;
}

/* Search a block of memory for the RSDP signature. */
static u8 *scan_mem_for_rsdp(u8 *start, u32 length)
{
	struct acpi_table_rsdp *rsdp;
	u8 *address, *end;

	end = start + length;

	/* Search from given start address for the requested length */
	for (address = start; address < end; address += ACPI_RSDP_SCAN_STEP) {
		/*
		 * Both RSDP signature and checksum must be correct.
		 * Note: Sometimes there exists more than one RSDP in memory;
		 * the valid RSDP has a valid checksum, all others have an
		 * invalid checksum.
		 */
		rsdp = (struct acpi_table_rsdp *)address;

		/* BAD Signature */
		if (!ACPI_VALIDATE_RSDP_SIG(rsdp->signature))
			continue;

		/* Check the standard checksum */
		if (compute_checksum((u8 *)rsdp, ACPI_RSDP_CHECKSUM_LENGTH))
			continue;

		/* Check extended checksum if table version >= 2 */
		if ((rsdp->revision >= 2) &&
		    (compute_checksum((u8 *)rsdp, ACPI_RSDP_XCHECKSUM_LENGTH)))
			continue;

		/* Signature and checksum valid, we have found a real RSDP */
		return address;
	}
	return NULL;
}

/* Search RSDP address in EBDA. */
static acpi_physical_address bios_get_rsdp_addr(void)
{
	unsigned long address;
	u8 *rsdp;

	/* Get the location of the Extended BIOS Data Area (EBDA) */
	address = *(u16 *)ACPI_EBDA_PTR_LOCATION;
	address <<= 4;

	/*
	 * Search EBDA paragraphs (EBDA is required to be a minimum of
	 * 1K length)
	 */
	if (address > 0x400) {
		rsdp = scan_mem_for_rsdp((u8 *)address, ACPI_EBDA_WINDOW_SIZE);
		if (rsdp)
			return (acpi_physical_address)(unsigned long)rsdp;
	}

	/* Search upper memory: 16-byte boundaries in E0000h-FFFFFh */
	rsdp = scan_mem_for_rsdp((u8 *) ACPI_HI_RSDP_WINDOW_BASE,
					ACPI_HI_RSDP_WINDOW_SIZE);
	if (rsdp)
		return (acpi_physical_address)(unsigned long)rsdp;

	return 0;
}

/* Return RSDP address on success, otherwise 0. */
acpi_physical_address get_rsdp_addr(void)
{
	acpi_physical_address pa;

	pa = boot_params->acpi_rsdp_addr;

	if (!pa)
		pa = efi_get_rsdp_addr();

	if (!pa)
		pa = bios_get_rsdp_addr();

	return pa;
}

#if defined(CONFIG_RANDOMIZE_BASE) && defined(CONFIG_MEMORY_HOTREMOVE)
/*
 * Max length of 64-bit hex address string is 19, prefix "0x" + 16 hex
 * digits, and '\0' for termination.
 */
#define MAX_ADDR_LEN 19

static unsigned long get_cmdline_acpi_rsdp(void)
{
	unsigned long addr = 0;

#ifdef CONFIG_KEXEC
	char val[MAX_ADDR_LEN] = { };
	int ret;

	ret = cmdline_find_option("acpi_rsdp", val, MAX_ADDR_LEN);
	if (ret < 0)
		return 0;

	if (boot_kstrtoul(val, 16, &addr))
		return 0;
#endif
	return addr;
}

/* Compute SRAT address from RSDP. */
static unsigned long get_acpi_srat_table(void)
{
	unsigned long root_table, acpi_table;
	struct acpi_table_header *header;
	struct acpi_table_rsdp *rsdp;
	u32 num_entries, size, len;
	char arg[10];
	u8 *entry;

	/*
	 * Check whether we were given an RSDP on the command line. We don't
	 * stash this in boot params because the kernel itself may have
	 * different ideas about whether to trust a command-line parameter.
	 */
	rsdp = (struct acpi_table_rsdp *)get_cmdline_acpi_rsdp();
	if (!rsdp)
		rsdp = (struct acpi_table_rsdp *)(long)
			boot_params->acpi_rsdp_addr;

	if (!rsdp)
		return 0;

	/* Get ACPI root table from RSDP.*/
	if (!(cmdline_find_option("acpi", arg, sizeof(arg)) == 4 &&
	    !strncmp(arg, "rsdt", 4)) &&
	    rsdp->xsdt_physical_address &&
	    rsdp->revision > 1) {
		root_table = rsdp->xsdt_physical_address;
		size = ACPI_XSDT_ENTRY_SIZE;
	} else {
		root_table = rsdp->rsdt_physical_address;
		size = ACPI_RSDT_ENTRY_SIZE;
	}

	if (!root_table)
		return 0;

	header = (struct acpi_table_header *)root_table;
	len = header->length;
	if (len < sizeof(struct acpi_table_header) + size)
		return 0;

	num_entries = (len - sizeof(struct acpi_table_header)) / size;
	entry = (u8 *)(root_table + sizeof(struct acpi_table_header));

	while (num_entries--) {
		if (size == ACPI_RSDT_ENTRY_SIZE)
			acpi_table = *(u32 *)entry;
		else
			acpi_table = *(u64 *)entry;

		if (acpi_table) {
			header = (struct acpi_table_header *)acpi_table;

			if (ACPI_COMPARE_NAMESEG(header->signature, ACPI_SIG_SRAT))
				return acpi_table;
		}
		entry += size;
	}
	return 0;
}

/**
 * count_immovable_mem_regions - Parse SRAT and cache the immovable
 * memory regions into the immovable_mem array.
 *
 * Return the number of immovable memory regions on success, 0 on failure:
 *
 * - Too many immovable memory regions
 * - ACPI off or no SRAT found
 * - No immovable memory region found.
 */
int count_immovable_mem_regions(void)
{
	unsigned long table_addr, table_end, table;
	struct acpi_subtable_header *sub_table;
	struct acpi_table_header *table_header;
	char arg[MAX_ACPI_ARG_LENGTH];
	int num = 0;

	if (cmdline_find_option("acpi", arg, sizeof(arg)) == 3 &&
	    !strncmp(arg, "off", 3))
		return 0;

	table_addr = get_acpi_srat_table();
	if (!table_addr)
		return 0;

	table_header = (struct acpi_table_header *)table_addr;
	table_end = table_addr + table_header->length;
	table = table_addr + sizeof(struct acpi_table_srat);

	while (table + sizeof(struct acpi_subtable_header) < table_end) {

		sub_table = (struct acpi_subtable_header *)table;
		if (!sub_table->length) {
			debug_putstr("Invalid zero length SRAT subtable.\n");
			return 0;
		}

		if (sub_table->type == ACPI_SRAT_TYPE_MEMORY_AFFINITY) {
			struct acpi_srat_mem_affinity *ma;

			ma = (struct acpi_srat_mem_affinity *)sub_table;
			if (!(ma->flags & ACPI_SRAT_MEM_HOT_PLUGGABLE) && ma->length) {
				immovable_mem[num].start = ma->base_address;
				immovable_mem[num].size = ma->length;
				num++;
			}

			if (num >= MAX_NUMNODES*2) {
				debug_putstr("Too many immovable memory regions, aborting.\n");
				return 0;
			}
		}
		table += sub_table->length;
	}
	return num;
}
#endif /* CONFIG_RANDOMIZE_BASE && CONFIG_MEMORY_HOTREMOVE */
