/* SPDX-License-Identifier: GPL-2.0 */
#ifndef BOOT_BOOT_H
#define BOOT_BOOT_H

#include <linux/types.h>

#define IPL_START	0x200

#ifndef __ASSEMBLY__

#include <asm/physmem_info.h>

struct machine_info {
	unsigned char has_edat1 : 1;
	unsigned char has_edat2 : 1;
	unsigned char has_nx : 1;
};

struct vmlinux_info {
	unsigned long default_lma;
	unsigned long entry;
	unsigned long image_size;	/* does not include .bss */
	unsigned long bss_size;		/* uncompressed image .bss size */
	unsigned long bootdata_off;
	unsigned long bootdata_size;
	unsigned long bootdata_preserved_off;
	unsigned long bootdata_preserved_size;
	unsigned long dynsym_start;
	unsigned long rela_dyn_start;
	unsigned long rela_dyn_end;
	unsigned long amode31_size;
	unsigned long init_mm_off;
	unsigned long swapper_pg_dir_off;
	unsigned long invalid_pg_dir_off;
#ifdef CONFIG_KASAN
	unsigned long kasan_early_shadow_page_off;
	unsigned long kasan_early_shadow_pte_off;
	unsigned long kasan_early_shadow_pmd_off;
	unsigned long kasan_early_shadow_pud_off;
	unsigned long kasan_early_shadow_p4d_off;
#endif
};

void startup_kernel(void);
unsigned long detect_max_physmem_end(void);
void detect_physmem_online_ranges(unsigned long max_physmem_end);
void physmem_set_usable_limit(unsigned long limit);
void physmem_reserve(enum reserved_range_type type, unsigned long addr, unsigned long size);
void physmem_free(enum reserved_range_type type);
/* for continuous/multiple allocations per type */
unsigned long physmem_alloc_top_down(enum reserved_range_type type, unsigned long size,
				     unsigned long align);
/* for single allocations, 1 per type */
unsigned long physmem_alloc_range(enum reserved_range_type type, unsigned long size,
				  unsigned long align, unsigned long min, unsigned long max,
				  bool die_on_oom);
unsigned long get_physmem_alloc_pos(void);
bool ipl_report_certs_intersects(unsigned long addr, unsigned long size,
				 unsigned long *intersection_start);
bool is_ipl_block_dump(void);
void store_ipl_parmblock(void);
int read_ipl_report(void);
void save_ipl_cert_comp_list(void);
void setup_boot_command_line(void);
void parse_boot_command_line(void);
void verify_facilities(void);
void print_missing_facilities(void);
void sclp_early_setup_buffer(void);
void print_pgm_check_info(void);
unsigned long randomize_within_range(unsigned long size, unsigned long align,
				     unsigned long min, unsigned long max);
void setup_vmem(unsigned long asce_limit);
void __printf(1, 2) decompressor_printk(const char *fmt, ...);
void print_stacktrace(unsigned long sp);
void error(char *m);

extern struct machine_info machine;

/* Symbols defined by linker scripts */
extern const char kernel_version[];
extern unsigned long memory_limit;
extern unsigned long vmalloc_size;
extern int vmalloc_size_set;
extern char __boot_data_start[], __boot_data_end[];
extern char __boot_data_preserved_start[], __boot_data_preserved_end[];
extern char _decompressor_syms_start[], _decompressor_syms_end[];
extern char _stack_start[], _stack_end[];
extern char _end[], _decompressor_end[];
extern unsigned char _compressed_start[];
extern unsigned char _compressed_end[];
extern struct vmlinux_info _vmlinux_info;
#define vmlinux _vmlinux_info

#define __abs_lowcore_pa(x)	(((unsigned long)(x) - __abs_lowcore) % sizeof(struct lowcore))

static inline bool intersects(unsigned long addr0, unsigned long size0,
			      unsigned long addr1, unsigned long size1)
{
	return addr0 + size0 > addr1 && addr1 + size1 > addr0;
}
#endif /* __ASSEMBLY__ */
#endif /* BOOT_BOOT_H */
