/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_PGTABLE_H
#define _ASM_POWERPC_PGTABLE_H

#ifndef __ASSEMBLY__
#include <linux/mmdebug.h>
#include <linux/mmzone.h>
#include <asm/processor.h>		/* For TASK_SIZE */
#include <asm/mmu.h>
#include <asm/page.h>
#include <asm/tlbflush.h>

struct mm_struct;

#endif /* !__ASSEMBLY__ */

#ifdef CONFIG_PPC_BOOK3S
#include <asm/book3s/pgtable.h>
#else
#include <asm/nohash/pgtable.h>
#endif /* !CONFIG_PPC_BOOK3S */

/*
 * Protection used for kernel text. We want the debuggers to be able to
 * set breakpoints anywhere, so don't write protect the kernel text
 * on platforms where such control is possible.
 */
#if defined(CONFIG_KGDB) || defined(CONFIG_XMON) || defined(CONFIG_BDI_SWITCH) || \
	defined(CONFIG_KPROBES) || defined(CONFIG_DYNAMIC_FTRACE)
#define PAGE_KERNEL_TEXT	PAGE_KERNEL_X
#else
#define PAGE_KERNEL_TEXT	PAGE_KERNEL_ROX
#endif

/* Make modules code happy. We don't set RO yet */
#define PAGE_KERNEL_EXEC	PAGE_KERNEL_X

/* Advertise special mapping type for AGP */
#define PAGE_AGP		(PAGE_KERNEL_NC)
#define HAVE_PAGE_AGP

#ifndef __ASSEMBLY__

void set_ptes(struct mm_struct *mm, unsigned long addr, pte_t *ptep,
		pte_t pte, unsigned int nr);
#define set_ptes set_ptes
#define update_mmu_cache(vma, addr, ptep) \
	update_mmu_cache_range(NULL, vma, addr, ptep, 1)

#ifndef MAX_PTRS_PER_PGD
#define MAX_PTRS_PER_PGD PTRS_PER_PGD
#endif

/* Keep these as a macros to avoid include dependency mess */
#define pte_page(x)		pfn_to_page(pte_pfn(x))
#define mk_pte(page, pgprot)	pfn_pte(page_to_pfn(page), (pgprot))

static inline unsigned long pte_pfn(pte_t pte)
{
	return (pte_val(pte) & PTE_RPN_MASK) >> PTE_RPN_SHIFT;
}

/*
 * Select all bits except the pfn
 */
static inline pgprot_t pte_pgprot(pte_t pte)
{
	unsigned long pte_flags;

	pte_flags = pte_val(pte) & ~PTE_RPN_MASK;
	return __pgprot(pte_flags);
}

#ifndef pmd_page_vaddr
static inline const void *pmd_page_vaddr(pmd_t pmd)
{
	return __va(pmd_val(pmd) & ~PMD_MASKED_BITS);
}
#define pmd_page_vaddr pmd_page_vaddr
#endif
/*
 * ZERO_PAGE is a global shared page that is always zero: used
 * for zero-mapped memory areas etc..
 */
extern unsigned long empty_zero_page[];
#define ZERO_PAGE(vaddr) (virt_to_page(empty_zero_page))

extern pgd_t swapper_pg_dir[];

extern void paging_init(void);
void poking_init(void);

extern unsigned long ioremap_bot;
extern const pgprot_t protection_map[16];

#ifndef CONFIG_TRANSPARENT_HUGEPAGE
#define pmd_large(pmd)		0
#endif

/* can we use this in kvm */
unsigned long vmalloc_to_phys(void *vmalloc_addr);

void pgtable_cache_add(unsigned int shift);

pte_t *early_pte_alloc_kernel(pmd_t *pmdp, unsigned long va);

#if defined(CONFIG_STRICT_KERNEL_RWX) || defined(CONFIG_PPC32)
void mark_initmem_nx(void);
#else
static inline void mark_initmem_nx(void) { }
#endif

/*
 * When used, PTE_FRAG_NR is defined in subarch pgtable.h
 * so we are sure it is included when arriving here.
 */
#ifdef PTE_FRAG_NR
static inline void *pte_frag_get(mm_context_t *ctx)
{
	return ctx->pte_frag;
}

static inline void pte_frag_set(mm_context_t *ctx, void *p)
{
	ctx->pte_frag = p;
}
#else
#define PTE_FRAG_NR		1
#define PTE_FRAG_SIZE_SHIFT	PAGE_SHIFT
#define PTE_FRAG_SIZE		(1UL << PTE_FRAG_SIZE_SHIFT)

static inline void *pte_frag_get(mm_context_t *ctx)
{
	return NULL;
}

static inline void pte_frag_set(mm_context_t *ctx, void *p)
{
}
#endif

#ifndef pmd_is_leaf
#define pmd_is_leaf pmd_is_leaf
static inline bool pmd_is_leaf(pmd_t pmd)
{
	return false;
}
#endif

#ifndef pud_is_leaf
#define pud_is_leaf pud_is_leaf
static inline bool pud_is_leaf(pud_t pud)
{
	return false;
}
#endif

#ifndef p4d_is_leaf
#define p4d_is_leaf p4d_is_leaf
static inline bool p4d_is_leaf(p4d_t p4d)
{
	return false;
}
#endif

#define pmd_pgtable pmd_pgtable
static inline pgtable_t pmd_pgtable(pmd_t pmd)
{
	return (pgtable_t)pmd_page_vaddr(pmd);
}

#ifdef CONFIG_PPC64
int __meminit vmemmap_populated(unsigned long vmemmap_addr, int vmemmap_map_size);
bool altmap_cross_boundary(struct vmem_altmap *altmap, unsigned long start,
			   unsigned long page_size);
/*
 * mm/memory_hotplug.c:mhp_supports_memmap_on_memory goes into details
 * some of the restrictions. We don't check for PMD_SIZE because our
 * vmemmap allocation code can fallback correctly. The pageblock
 * alignment requirement is met using altmap->reserve blocks.
 */
#define arch_supports_memmap_on_memory arch_supports_memmap_on_memory
static inline bool arch_supports_memmap_on_memory(unsigned long vmemmap_size)
{
	if (!radix_enabled())
		return false;
	/*
	 * With 4K page size and 2M PMD_SIZE, we can align
	 * things better with memory block size value
	 * starting from 128MB. Hence align things with PMD_SIZE.
	 */
	if (IS_ENABLED(CONFIG_PPC_4K_PAGES))
		return IS_ALIGNED(vmemmap_size, PMD_SIZE);
	return true;
}

#endif /* CONFIG_PPC64 */

#endif /* __ASSEMBLY__ */

#endif /* _ASM_POWERPC_PGTABLE_H */
