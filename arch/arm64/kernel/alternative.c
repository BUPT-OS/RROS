// SPDX-License-Identifier: GPL-2.0-only
/*
 * alternative runtime patching
 * inspired by the x86 version
 *
 * Copyright (C) 2014 ARM Ltd.
 */

#define pr_fmt(fmt) "alternatives: " fmt

#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/elf.h>
#include <asm/cacheflush.h>
#include <asm/alternative.h>
#include <asm/cpufeature.h>
#include <asm/insn.h>
#include <asm/module.h>
#include <asm/sections.h>
#include <asm/vdso.h>
#include <linux/stop_machine.h>

#define __ALT_PTR(a, f)		((void *)&(a)->f + (a)->f)
#define ALT_ORIG_PTR(a)		__ALT_PTR(a, orig_offset)
#define ALT_REPL_PTR(a)		__ALT_PTR(a, alt_offset)

#define ALT_CAP(a)		((a)->cpucap & ~ARM64_CB_BIT)
#define ALT_HAS_CB(a)		((a)->cpucap & ARM64_CB_BIT)

/* Volatile, as we may be patching the guts of READ_ONCE() */
static volatile int all_alternatives_applied;

static DECLARE_BITMAP(applied_alternatives, ARM64_NCAPS);

struct alt_region {
	struct alt_instr *begin;
	struct alt_instr *end;
};

bool alternative_is_applied(u16 cpucap)
{
	if (WARN_ON(cpucap >= ARM64_NCAPS))
		return false;

	return test_bit(cpucap, applied_alternatives);
}

/*
 * Check if the target PC is within an alternative block.
 */
static __always_inline bool branch_insn_requires_update(struct alt_instr *alt, unsigned long pc)
{
	unsigned long replptr = (unsigned long)ALT_REPL_PTR(alt);
	return !(pc >= replptr && pc <= (replptr + alt->alt_len));
}

#define align_down(x, a)	((unsigned long)(x) & ~(((unsigned long)(a)) - 1))

static __always_inline u32 get_alt_insn(struct alt_instr *alt, __le32 *insnptr, __le32 *altinsnptr)
{
	u32 insn;

	insn = le32_to_cpu(*altinsnptr);

	if (aarch64_insn_is_branch_imm(insn)) {
		s32 offset = aarch64_get_branch_offset(insn);
		unsigned long target;

		target = (unsigned long)altinsnptr + offset;

		/*
		 * If we're branching inside the alternate sequence,
		 * do not rewrite the instruction, as it is already
		 * correct. Otherwise, generate the new instruction.
		 */
		if (branch_insn_requires_update(alt, target)) {
			offset = target - (unsigned long)insnptr;
			insn = aarch64_set_branch_offset(insn, offset);
		}
	} else if (aarch64_insn_is_adrp(insn)) {
		s32 orig_offset, new_offset;
		unsigned long target;

		/*
		 * If we're replacing an adrp instruction, which uses PC-relative
		 * immediate addressing, adjust the offset to reflect the new
		 * PC. adrp operates on 4K aligned addresses.
		 */
		orig_offset  = aarch64_insn_adrp_get_offset(insn);
		target = align_down(altinsnptr, SZ_4K) + orig_offset;
		new_offset = target - align_down(insnptr, SZ_4K);
		insn = aarch64_insn_adrp_set_offset(insn, new_offset);
	} else if (aarch64_insn_uses_literal(insn)) {
		/*
		 * Disallow patching unhandled instructions using PC relative
		 * literal addresses
		 */
		BUG();
	}

	return insn;
}

static noinstr void patch_alternative(struct alt_instr *alt,
			      __le32 *origptr, __le32 *updptr, int nr_inst)
{
	__le32 *replptr;
	int i;

	replptr = ALT_REPL_PTR(alt);
	for (i = 0; i < nr_inst; i++) {
		u32 insn;

		insn = get_alt_insn(alt, origptr + i, replptr + i);
		updptr[i] = cpu_to_le32(insn);
	}
}

/*
 * We provide our own, private D-cache cleaning function so that we don't
 * accidentally call into the cache.S code, which is patched by us at
 * runtime.
 */
static noinstr void clean_dcache_range_nopatch(u64 start, u64 end)
{
	u64 cur, d_size, ctr_el0;

	ctr_el0 = arm64_ftr_reg_ctrel0.sys_val;
	d_size = 4 << cpuid_feature_extract_unsigned_field(ctr_el0,
							   CTR_EL0_DminLine_SHIFT);
	cur = start & ~(d_size - 1);
	do {
		/*
		 * We must clean+invalidate to the PoC in order to avoid
		 * Cortex-A53 errata 826319, 827319, 824069 and 819472
		 * (this corresponds to ARM64_WORKAROUND_CLEAN_CACHE)
		 */
		asm volatile("dc civac, %0" : : "r" (cur) : "memory");
	} while (cur += d_size, cur < end);
}

static void __apply_alternatives(const struct alt_region *region,
				 bool is_module,
				 unsigned long *cpucap_mask)
{
	struct alt_instr *alt;
	__le32 *origptr, *updptr;
	alternative_cb_t alt_cb;

	for (alt = region->begin; alt < region->end; alt++) {
		int nr_inst;
		int cap = ALT_CAP(alt);

		if (!test_bit(cap, cpucap_mask))
			continue;

		if (!cpus_have_cap(cap))
			continue;

		if (ALT_HAS_CB(alt))
			BUG_ON(alt->alt_len != 0);
		else
			BUG_ON(alt->alt_len != alt->orig_len);

		origptr = ALT_ORIG_PTR(alt);
		updptr = is_module ? origptr : lm_alias(origptr);
		nr_inst = alt->orig_len / AARCH64_INSN_SIZE;

		if (ALT_HAS_CB(alt))
			alt_cb  = ALT_REPL_PTR(alt);
		else
			alt_cb = patch_alternative;

		alt_cb(alt, origptr, updptr, nr_inst);

		if (!is_module) {
			clean_dcache_range_nopatch((u64)origptr,
						   (u64)(origptr + nr_inst));
		}
	}

	/*
	 * The core module code takes care of cache maintenance in
	 * flush_module_icache().
	 */
	if (!is_module) {
		dsb(ish);
		icache_inval_all_pou();
		isb();

		bitmap_or(applied_alternatives, applied_alternatives,
			  cpucap_mask, ARM64_NCAPS);
		bitmap_and(applied_alternatives, applied_alternatives,
			   system_cpucaps, ARM64_NCAPS);
	}
}

static void __init apply_alternatives_vdso(void)
{
	struct alt_region region;
	const struct elf64_hdr *hdr;
	const struct elf64_shdr *shdr;
	const struct elf64_shdr *alt;
	DECLARE_BITMAP(all_capabilities, ARM64_NCAPS);

	bitmap_fill(all_capabilities, ARM64_NCAPS);

	hdr = (struct elf64_hdr *)vdso_start;
	shdr = (void *)hdr + hdr->e_shoff;
	alt = find_section(hdr, shdr, ".altinstructions");
	if (!alt)
		return;

	region = (struct alt_region){
		.begin	= (void *)hdr + alt->sh_offset,
		.end	= (void *)hdr + alt->sh_offset + alt->sh_size,
	};

	__apply_alternatives(&region, false, &all_capabilities[0]);
}

static const struct alt_region kernel_alternatives __initconst = {
	.begin	= (struct alt_instr *)__alt_instructions,
	.end	= (struct alt_instr *)__alt_instructions_end,
};

/*
 * We might be patching the stop_machine state machine, so implement a
 * really simple polling protocol here.
 */
static int __init __apply_alternatives_multi_stop(void *unused)
{
	/* We always have a CPU 0 at this point (__init) */
	if (smp_processor_id()) {
		while (!all_alternatives_applied)
			cpu_relax();
		isb();
	} else {
		DECLARE_BITMAP(remaining_capabilities, ARM64_NCAPS);

		bitmap_complement(remaining_capabilities, boot_cpucaps,
				  ARM64_NCAPS);

		BUG_ON(all_alternatives_applied);
		__apply_alternatives(&kernel_alternatives, false,
				     remaining_capabilities);
		/* Barriers provided by the cache flushing */
		all_alternatives_applied = 1;
	}

	return 0;
}

void __init apply_alternatives_all(void)
{
	pr_info("applying system-wide alternatives\n");

	apply_alternatives_vdso();
	/* better not try code patching on a live SMP system */
	stop_machine(__apply_alternatives_multi_stop, NULL, cpu_online_mask);
}

/*
 * This is called very early in the boot process (directly after we run
 * a feature detect on the boot CPU). No need to worry about other CPUs
 * here.
 */
void __init apply_boot_alternatives(void)
{
	/* If called on non-boot cpu things could go wrong */
	WARN_ON(smp_processor_id() != 0);

	pr_info("applying boot alternatives\n");

	__apply_alternatives(&kernel_alternatives, false,
			     &boot_cpucaps[0]);
}

#ifdef CONFIG_MODULES
void apply_alternatives_module(void *start, size_t length)
{
	struct alt_region region = {
		.begin	= start,
		.end	= start + length,
	};
	DECLARE_BITMAP(all_capabilities, ARM64_NCAPS);

	bitmap_fill(all_capabilities, ARM64_NCAPS);

	__apply_alternatives(&region, true, &all_capabilities[0]);
}
#endif

noinstr void alt_cb_patch_nops(struct alt_instr *alt, __le32 *origptr,
			       __le32 *updptr, int nr_inst)
{
	for (int i = 0; i < nr_inst; i++)
		updptr[i] = cpu_to_le32(aarch64_insn_gen_nop());
}
EXPORT_SYMBOL(alt_cb_patch_nops);
