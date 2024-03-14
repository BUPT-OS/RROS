// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 *
 * Derived from MIPS:
 * Copyright (C) 1995 Linus Torvalds
 * Copyright (C) 1995 Waldorf Electronics
 * Copyright (C) 1994, 95, 96, 97, 98, 99, 2000, 01, 02, 03  Ralf Baechle
 * Copyright (C) 1996 Stoned Elipot
 * Copyright (C) 1999 Silicon Graphics, Inc.
 * Copyright (C) 2000, 2001, 2002, 2007	 Maciej W. Rozycki
 */
#include <linux/init.h>
#include <linux/acpi.h>
#include <linux/cpu.h>
#include <linux/dmi.h>
#include <linux/efi.h>
#include <linux/export.h>
#include <linux/screen_info.h>
#include <linux/memblock.h>
#include <linux/initrd.h>
#include <linux/ioport.h>
#include <linux/kexec.h>
#include <linux/crash_dump.h>
#include <linux/root_dev.h>
#include <linux/console.h>
#include <linux/pfn.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/device.h>
#include <linux/dma-map-ops.h>
#include <linux/libfdt.h>
#include <linux/of_fdt.h>
#include <linux/of_address.h>
#include <linux/suspend.h>
#include <linux/swiotlb.h>

#include <asm/addrspace.h>
#include <asm/alternative.h>
#include <asm/bootinfo.h>
#include <asm/cache.h>
#include <asm/cpu.h>
#include <asm/dma.h>
#include <asm/efi.h>
#include <asm/loongson.h>
#include <asm/numa.h>
#include <asm/pgalloc.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/time.h>

#define SMBIOS_BIOSSIZE_OFFSET		0x09
#define SMBIOS_BIOSEXTERN_OFFSET	0x13
#define SMBIOS_FREQLOW_OFFSET		0x16
#define SMBIOS_FREQHIGH_OFFSET		0x17
#define SMBIOS_FREQLOW_MASK		0xFF
#define SMBIOS_CORE_PACKAGE_OFFSET	0x23
#define LOONGSON_EFI_ENABLE		(1 << 3)

struct screen_info screen_info __section(".data");

unsigned long fw_arg0, fw_arg1, fw_arg2;
DEFINE_PER_CPU(unsigned long, kernelsp);
struct cpuinfo_loongarch cpu_data[NR_CPUS] __read_mostly;

EXPORT_SYMBOL(cpu_data);

struct loongson_board_info b_info;
static const char dmi_empty_string[] = "        ";

/*
 * Setup information
 *
 * These are initialized so they are in the .data section
 */
char init_command_line[COMMAND_LINE_SIZE] __initdata;

static int num_standard_resources;
static struct resource *standard_resources;

static struct resource code_resource = { .name = "Kernel code", };
static struct resource data_resource = { .name = "Kernel data", };
static struct resource bss_resource  = { .name = "Kernel bss", };

const char *get_system_type(void)
{
	return "generic-loongson-machine";
}

void __init arch_cpu_finalize_init(void)
{
	alternative_instructions();
}

static const char *dmi_string_parse(const struct dmi_header *dm, u8 s)
{
	const u8 *bp = ((u8 *) dm) + dm->length;

	if (s) {
		s--;
		while (s > 0 && *bp) {
			bp += strlen(bp) + 1;
			s--;
		}

		if (*bp != 0) {
			size_t len = strlen(bp)+1;
			size_t cmp_len = len > 8 ? 8 : len;

			if (!memcmp(bp, dmi_empty_string, cmp_len))
				return dmi_empty_string;

			return bp;
		}
	}

	return "";
}

static void __init parse_cpu_table(const struct dmi_header *dm)
{
	long freq_temp = 0;
	char *dmi_data = (char *)dm;

	freq_temp = ((*(dmi_data + SMBIOS_FREQHIGH_OFFSET) << 8) +
			((*(dmi_data + SMBIOS_FREQLOW_OFFSET)) & SMBIOS_FREQLOW_MASK));
	cpu_clock_freq = freq_temp * 1000000;

	loongson_sysconf.cpuname = (void *)dmi_string_parse(dm, dmi_data[16]);
	loongson_sysconf.cores_per_package = *(dmi_data + SMBIOS_CORE_PACKAGE_OFFSET);

	pr_info("CpuClock = %llu\n", cpu_clock_freq);
}

static void __init parse_bios_table(const struct dmi_header *dm)
{
	char *dmi_data = (char *)dm;

	b_info.bios_size = (*(dmi_data + SMBIOS_BIOSSIZE_OFFSET) + 1) << 6;
}

static void __init find_tokens(const struct dmi_header *dm, void *dummy)
{
	switch (dm->type) {
	case 0x0: /* Extern BIOS */
		parse_bios_table(dm);
		break;
	case 0x4: /* Calling interface */
		parse_cpu_table(dm);
		break;
	}
}
static void __init smbios_parse(void)
{
	b_info.bios_vendor = (void *)dmi_get_system_info(DMI_BIOS_VENDOR);
	b_info.bios_version = (void *)dmi_get_system_info(DMI_BIOS_VERSION);
	b_info.bios_release_date = (void *)dmi_get_system_info(DMI_BIOS_DATE);
	b_info.board_vendor = (void *)dmi_get_system_info(DMI_BOARD_VENDOR);
	b_info.board_name = (void *)dmi_get_system_info(DMI_BOARD_NAME);
	dmi_walk(find_tokens, NULL);
}

#ifdef CONFIG_ARCH_WRITECOMBINE
pgprot_t pgprot_wc = PAGE_KERNEL_WUC;
#else
pgprot_t pgprot_wc = PAGE_KERNEL_SUC;
#endif

EXPORT_SYMBOL(pgprot_wc);

static int __init setup_writecombine(char *p)
{
	if (!strcmp(p, "on"))
		pgprot_wc = PAGE_KERNEL_WUC;
	else if (!strcmp(p, "off"))
		pgprot_wc = PAGE_KERNEL_SUC;
	else
		pr_warn("Unknown writecombine setting \"%s\".\n", p);

	return 0;
}
early_param("writecombine", setup_writecombine);

static int usermem __initdata;

static int __init early_parse_mem(char *p)
{
	phys_addr_t start, size;

	if (!p) {
		pr_err("mem parameter is empty, do nothing\n");
		return -EINVAL;
	}

	/*
	 * If a user specifies memory size, we
	 * blow away any automatically generated
	 * size.
	 */
	if (usermem == 0) {
		usermem = 1;
		memblock_remove(memblock_start_of_DRAM(),
			memblock_end_of_DRAM() - memblock_start_of_DRAM());
	}
	start = 0;
	size = memparse(p, &p);
	if (*p == '@')
		start = memparse(p + 1, &p);
	else {
		pr_err("Invalid format!\n");
		return -EINVAL;
	}

	if (!IS_ENABLED(CONFIG_NUMA))
		memblock_add(start, size);
	else
		memblock_add_node(start, size, pa_to_nid(start), MEMBLOCK_NONE);

	return 0;
}
early_param("mem", early_parse_mem);

static void __init arch_reserve_vmcore(void)
{
#ifdef CONFIG_PROC_VMCORE
	u64 i;
	phys_addr_t start, end;

	if (!is_kdump_kernel())
		return;

	if (!elfcorehdr_size) {
		for_each_mem_range(i, &start, &end) {
			if (elfcorehdr_addr >= start && elfcorehdr_addr < end) {
				/*
				 * Reserve from the elf core header to the end of
				 * the memory segment, that should all be kdump
				 * reserved memory.
				 */
				elfcorehdr_size = end - elfcorehdr_addr;
				break;
			}
		}
	}

	if (memblock_is_region_reserved(elfcorehdr_addr, elfcorehdr_size)) {
		pr_warn("elfcorehdr is overlapped\n");
		return;
	}

	memblock_reserve(elfcorehdr_addr, elfcorehdr_size);

	pr_info("Reserving %llu KiB of memory at 0x%llx for elfcorehdr\n",
		elfcorehdr_size >> 10, elfcorehdr_addr);
#endif
}

/* 2MB alignment for crash kernel regions */
#define CRASH_ALIGN	SZ_2M
#define CRASH_ADDR_MAX	SZ_4G

static void __init arch_parse_crashkernel(void)
{
#ifdef CONFIG_KEXEC
	int ret;
	unsigned long long total_mem;
	unsigned long long crash_base, crash_size;

	total_mem = memblock_phys_mem_size();
	ret = parse_crashkernel(boot_command_line, total_mem, &crash_size, &crash_base);
	if (ret < 0 || crash_size <= 0)
		return;

	if (crash_base <= 0) {
		crash_base = memblock_phys_alloc_range(crash_size, CRASH_ALIGN, CRASH_ALIGN, CRASH_ADDR_MAX);
		if (!crash_base) {
			pr_warn("crashkernel reservation failed - No suitable area found.\n");
			return;
		}
	} else if (!memblock_phys_alloc_range(crash_size, CRASH_ALIGN, crash_base, crash_base + crash_size)) {
		pr_warn("Invalid memory region reserved for crash kernel\n");
		return;
	}

	crashk_res.start = crash_base;
	crashk_res.end	 = crash_base + crash_size - 1;
#endif
}

static void __init fdt_setup(void)
{
#ifdef CONFIG_OF_EARLY_FLATTREE
	void *fdt_pointer;

	/* ACPI-based systems do not require parsing fdt */
	if (acpi_os_get_root_pointer())
		return;

	/* Look for a device tree configuration table entry */
	fdt_pointer = efi_fdt_pointer();
	if (!fdt_pointer || fdt_check_header(fdt_pointer))
		return;

	early_init_dt_scan(fdt_pointer);
	early_init_fdt_reserve_self();

	max_low_pfn = PFN_PHYS(memblock_end_of_DRAM());
#endif
}

static void __init bootcmdline_init(char **cmdline_p)
{
	/*
	 * If CONFIG_CMDLINE_FORCE is enabled then initializing the command line
	 * is trivial - we simply use the built-in command line unconditionally &
	 * unmodified.
	 */
	if (IS_ENABLED(CONFIG_CMDLINE_FORCE)) {
		strscpy(boot_command_line, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
		goto out;
	}

#ifdef CONFIG_OF_FLATTREE
	/*
	 * If CONFIG_CMDLINE_BOOTLOADER is enabled and we are in FDT-based system,
	 * the boot_command_line will be overwritten by early_init_dt_scan_chosen().
	 * So we need to append init_command_line (the original copy of boot_command_line)
	 * to boot_command_line.
	 */
	if (initial_boot_params) {
		if (boot_command_line[0])
			strlcat(boot_command_line, " ", COMMAND_LINE_SIZE);

		strlcat(boot_command_line, init_command_line, COMMAND_LINE_SIZE);
		goto out;
	}
#endif

	/*
	 * Append built-in command line to the bootloader command line if
	 * CONFIG_CMDLINE_EXTEND is enabled.
	 */
	if (IS_ENABLED(CONFIG_CMDLINE_EXTEND) && CONFIG_CMDLINE[0]) {
		strlcat(boot_command_line, " ", COMMAND_LINE_SIZE);
		strlcat(boot_command_line, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
	}

	/*
	 * Use built-in command line if the bootloader command line is empty.
	 */
	if (IS_ENABLED(CONFIG_CMDLINE_BOOTLOADER) && !boot_command_line[0])
		strscpy(boot_command_line, CONFIG_CMDLINE, COMMAND_LINE_SIZE);

out:
	*cmdline_p = boot_command_line;
}

void __init platform_init(void)
{
	arch_reserve_vmcore();
	arch_parse_crashkernel();

#ifdef CONFIG_ACPI_TABLE_UPGRADE
	acpi_table_upgrade();
#endif
#ifdef CONFIG_ACPI
	acpi_gbl_use_default_register_widths = false;
	acpi_boot_table_init();
#endif
	unflatten_and_copy_device_tree();

#ifdef CONFIG_NUMA
	init_numa_memory();
#endif
	dmi_setup();
	smbios_parse();
	pr_info("The BIOS Version: %s\n", b_info.bios_version);

	efi_runtime_init();
}

static void __init check_kernel_sections_mem(void)
{
	phys_addr_t start = __pa_symbol(&_text);
	phys_addr_t size = __pa_symbol(&_end) - start;

	if (!memblock_is_region_memory(start, size)) {
		pr_info("Kernel sections are not in the memory maps\n");
		memblock_add(start, size);
	}
}

/*
 * arch_mem_init - initialize memory management subsystem
 */
static void __init arch_mem_init(char **cmdline_p)
{
	if (usermem)
		pr_info("User-defined physical RAM map overwrite\n");

	check_kernel_sections_mem();

	early_init_fdt_scan_reserved_mem();

	/*
	 * In order to reduce the possibility of kernel panic when failed to
	 * get IO TLB memory under CONFIG_SWIOTLB, it is better to allocate
	 * low memory as small as possible before swiotlb_init(), so make
	 * sparse_init() using top-down allocation.
	 */
	memblock_set_bottom_up(false);
	sparse_init();
	memblock_set_bottom_up(true);

	swiotlb_init(true, SWIOTLB_VERBOSE);

	dma_contiguous_reserve(PFN_PHYS(max_low_pfn));

	/* Reserve for hibernation. */
	register_nosave_region(PFN_DOWN(__pa_symbol(&__nosave_begin)),
				   PFN_UP(__pa_symbol(&__nosave_end)));

	memblock_dump_all();

	early_memtest(PFN_PHYS(ARCH_PFN_OFFSET), PFN_PHYS(max_low_pfn));
}

static void __init resource_init(void)
{
	long i = 0;
	size_t res_size;
	struct resource *res;
	struct memblock_region *region;

	code_resource.start = __pa_symbol(&_text);
	code_resource.end = __pa_symbol(&_etext) - 1;
	data_resource.start = __pa_symbol(&_etext);
	data_resource.end = __pa_symbol(&_edata) - 1;
	bss_resource.start = __pa_symbol(&__bss_start);
	bss_resource.end = __pa_symbol(&__bss_stop) - 1;

	num_standard_resources = memblock.memory.cnt;
	res_size = num_standard_resources * sizeof(*standard_resources);
	standard_resources = memblock_alloc(res_size, SMP_CACHE_BYTES);

	for_each_mem_region(region) {
		res = &standard_resources[i++];
		if (!memblock_is_nomap(region)) {
			res->name  = "System RAM";
			res->flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;
			res->start = __pfn_to_phys(memblock_region_memory_base_pfn(region));
			res->end = __pfn_to_phys(memblock_region_memory_end_pfn(region)) - 1;
		} else {
			res->name  = "Reserved";
			res->flags = IORESOURCE_MEM;
			res->start = __pfn_to_phys(memblock_region_reserved_base_pfn(region));
			res->end = __pfn_to_phys(memblock_region_reserved_end_pfn(region)) - 1;
		}

		request_resource(&iomem_resource, res);

		/*
		 *  We don't know which RAM region contains kernel data,
		 *  so we try it repeatedly and let the resource manager
		 *  test it.
		 */
		request_resource(res, &code_resource);
		request_resource(res, &data_resource);
		request_resource(res, &bss_resource);
	}

#ifdef CONFIG_KEXEC
	if (crashk_res.start < crashk_res.end) {
		insert_resource(&iomem_resource, &crashk_res);
		pr_info("Reserving %ldMB of memory at %ldMB for crashkernel\n",
			(unsigned long)((crashk_res.end - crashk_res.start + 1) >> 20),
			(unsigned long)(crashk_res.start  >> 20));
	}
#endif
}

static int __init add_legacy_isa_io(struct fwnode_handle *fwnode,
				resource_size_t hw_start, resource_size_t size)
{
	int ret = 0;
	unsigned long vaddr;
	struct logic_pio_hwaddr *range;

	range = kzalloc(sizeof(*range), GFP_ATOMIC);
	if (!range)
		return -ENOMEM;

	range->fwnode = fwnode;
	range->size = size = round_up(size, PAGE_SIZE);
	range->hw_start = hw_start;
	range->flags = LOGIC_PIO_CPU_MMIO;

	ret = logic_pio_register_range(range);
	if (ret) {
		kfree(range);
		return ret;
	}

	/* Legacy ISA must placed at the start of PCI_IOBASE */
	if (range->io_start != 0) {
		logic_pio_unregister_range(range);
		kfree(range);
		return -EINVAL;
	}

	vaddr = (unsigned long)(PCI_IOBASE + range->io_start);
	ioremap_page_range(vaddr, vaddr + size, hw_start, pgprot_device(PAGE_KERNEL));

	return 0;
}

static __init int arch_reserve_pio_range(void)
{
	struct device_node *np;

	for_each_node_by_name(np, "isa") {
		struct of_range range;
		struct of_range_parser parser;

		pr_info("ISA Bridge: %pOF\n", np);

		if (of_range_parser_init(&parser, np)) {
			pr_info("Failed to parse resources.\n");
			of_node_put(np);
			break;
		}

		for_each_of_range(&parser, &range) {
			switch (range.flags & IORESOURCE_TYPE_BITS) {
			case IORESOURCE_IO:
				pr_info(" IO 0x%016llx..0x%016llx  ->  0x%016llx\n",
					range.cpu_addr,
					range.cpu_addr + range.size - 1,
					range.bus_addr);
				if (add_legacy_isa_io(&np->fwnode, range.cpu_addr, range.size))
					pr_warn("Failed to reserve legacy IO in Logic PIO\n");
				break;
			case IORESOURCE_MEM:
				pr_info(" MEM 0x%016llx..0x%016llx  ->  0x%016llx\n",
					range.cpu_addr,
					range.cpu_addr + range.size - 1,
					range.bus_addr);
				break;
			}
		}
	}

	return 0;
}
arch_initcall(arch_reserve_pio_range);

static int __init reserve_memblock_reserved_regions(void)
{
	u64 i, j;

	for (i = 0; i < num_standard_resources; ++i) {
		struct resource *mem = &standard_resources[i];
		phys_addr_t r_start, r_end, mem_size = resource_size(mem);

		if (!memblock_is_region_reserved(mem->start, mem_size))
			continue;

		for_each_reserved_mem_range(j, &r_start, &r_end) {
			resource_size_t start, end;

			start = max(PFN_PHYS(PFN_DOWN(r_start)), mem->start);
			end = min(PFN_PHYS(PFN_UP(r_end)) - 1, mem->end);

			if (start > mem->end || end < mem->start)
				continue;

			reserve_region_with_split(mem, start, end, "Reserved");
		}
	}

	return 0;
}
arch_initcall(reserve_memblock_reserved_regions);

#ifdef CONFIG_SMP
static void __init prefill_possible_map(void)
{
	int i, possible;

	possible = num_processors + disabled_cpus;
	if (possible > nr_cpu_ids)
		possible = nr_cpu_ids;

	pr_info("SMP: Allowing %d CPUs, %d hotplug CPUs\n",
			possible, max((possible - num_processors), 0));

	for (i = 0; i < possible; i++)
		set_cpu_possible(i, true);
	for (; i < NR_CPUS; i++)
		set_cpu_possible(i, false);

	set_nr_cpu_ids(possible);
}
#endif

void __init setup_arch(char **cmdline_p)
{
	cpu_probe();

	init_environ();
	efi_init();
	fdt_setup();
	memblock_init();
	pagetable_init();
	bootcmdline_init(cmdline_p);
	parse_early_param();
	reserve_initrd_mem();

	platform_init();
	arch_mem_init(cmdline_p);

	resource_init();
#ifdef CONFIG_SMP
	plat_smp_setup();
	prefill_possible_map();
#endif

	paging_init();

#ifdef CONFIG_KASAN
	kasan_init();
#endif
}
