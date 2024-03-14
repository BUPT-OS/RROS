/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SETUP_H
#define _ASM_X86_SETUP_H

#include <uapi/asm/setup.h>

#define COMMAND_LINE_SIZE 2048

#include <linux/linkage.h>
#include <asm/page_types.h>
#include <asm/ibt.h>

#ifdef __i386__

#include <linux/pfn.h>
/*
 * Reserved space for vmalloc and iomap - defined in asm/page.h
 */
#define MAXMEM_PFN	PFN_DOWN(MAXMEM)
#define MAX_NONPAE_PFN	(1 << 20)

#endif /* __i386__ */

#define PARAM_SIZE 4096		/* sizeof(struct boot_params) */

#define OLD_CL_MAGIC		0xA33F
#define OLD_CL_ADDRESS		0x020	/* Relative to real mode data */
#define NEW_CL_POINTER		0x228	/* Relative to real mode data */

#ifndef __ASSEMBLY__
#include <asm/bootparam.h>
#include <asm/x86_init.h>

extern u64 relocated_ramdisk;

/* Interrupt control for vSMPowered x86_64 systems */
#ifdef CONFIG_X86_64
void vsmp_init(void);
#else
static inline void vsmp_init(void) { }
#endif

struct pt_regs;

void setup_bios_corruption_check(void);
void early_platform_quirks(void);

extern unsigned long saved_video_mode;

extern void reserve_standard_io_resources(void);
extern void i386_reserve_resources(void);
extern unsigned long __startup_64(unsigned long physaddr, struct boot_params *bp);
extern void startup_64_setup_env(unsigned long physbase);
extern void early_setup_idt(void);
extern void __init do_early_exception(struct pt_regs *regs, int trapnr);

#ifdef CONFIG_X86_INTEL_MID
extern void x86_intel_mid_early_setup(void);
#else
static inline void x86_intel_mid_early_setup(void) { }
#endif

#ifdef CONFIG_X86_INTEL_CE
extern void x86_ce4100_early_setup(void);
#else
static inline void x86_ce4100_early_setup(void) { }
#endif

#ifndef _SETUP

#include <asm/espfix.h>
#include <linux/kernel.h>

/*
 * This is set up by the setup-routine at boot-time
 */
extern struct boot_params boot_params;
extern char _text[];

static inline bool kaslr_enabled(void)
{
	return IS_ENABLED(CONFIG_RANDOMIZE_MEMORY) &&
		!!(boot_params.hdr.loadflags & KASLR_FLAG);
}

/*
 * Apply no randomization if KASLR was disabled at boot or if KASAN
 * is enabled. KASAN shadow mappings rely on regions being PGD aligned.
 */
static inline bool kaslr_memory_enabled(void)
{
	return kaslr_enabled() && !IS_ENABLED(CONFIG_KASAN);
}

static inline unsigned long kaslr_offset(void)
{
	return (unsigned long)&_text - __START_KERNEL;
}

/*
 * Do NOT EVER look at the BIOS memory size location.
 * It does not work on many machines.
 */
#define LOWMEMSIZE()	(0x9f000)

/* exceedingly early brk-like allocator */
extern unsigned long _brk_end;
void *extend_brk(size_t size, size_t align);

/*
 * Reserve space in the .brk section, which is a block of memory from which the
 * caller is allowed to allocate very early (before even memblock is available)
 * by calling extend_brk().  All allocated memory will be eventually converted
 * to memblock.  Any leftover unallocated memory will be freed.
 *
 * The size is in bytes.
 */
#define RESERVE_BRK(name, size)					\
	__section(".bss..brk") __aligned(1) __used	\
	static char __brk_##name[size]

extern void probe_roms(void);

void clear_bss(void);

#ifdef __i386__

asmlinkage void __init __noreturn i386_start_kernel(void);

#else
asmlinkage void __init __noreturn x86_64_start_kernel(char *real_mode);
asmlinkage void __init __noreturn x86_64_start_reservations(char *real_mode_data);

#endif /* __i386__ */
#endif /* _SETUP */

#else  /* __ASSEMBLY */

.macro __RESERVE_BRK name, size
	.pushsection .bss..brk, "aw"
SYM_DATA_START(__brk_\name)
	.skip \size
SYM_DATA_END(__brk_\name)
	.popsection
.endm

#define RESERVE_BRK(name, size) __RESERVE_BRK name, size

#endif /* __ASSEMBLY__ */

#endif /* _ASM_X86_SETUP_H */
