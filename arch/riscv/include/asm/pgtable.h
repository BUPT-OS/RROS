/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Regents of the University of California
 */

#ifndef _ASM_RISCV_PGTABLE_H
#define _ASM_RISCV_PGTABLE_H

#include <linux/mmzone.h>
#include <linux/sizes.h>

#include <asm/pgtable-bits.h>

#ifndef CONFIG_MMU
#define KERNEL_LINK_ADDR	PAGE_OFFSET
#define KERN_VIRT_SIZE		(UL(-1))
#else

#define ADDRESS_SPACE_END	(UL(-1))

#ifdef CONFIG_64BIT
/* Leave 2GB for kernel and BPF at the end of the address space */
#define KERNEL_LINK_ADDR	(ADDRESS_SPACE_END - SZ_2G + 1)
#else
#define KERNEL_LINK_ADDR	PAGE_OFFSET
#endif

/* Number of entries in the page global directory */
#define PTRS_PER_PGD    (PAGE_SIZE / sizeof(pgd_t))
/* Number of entries in the page table */
#define PTRS_PER_PTE    (PAGE_SIZE / sizeof(pte_t))

/*
 * Half of the kernel address space (1/4 of the entries of the page global
 * directory) is for the direct mapping.
 */
#define KERN_VIRT_SIZE          ((PTRS_PER_PGD / 2 * PGDIR_SIZE) / 2)

#define VMALLOC_SIZE     (KERN_VIRT_SIZE >> 1)
#define VMALLOC_END      PAGE_OFFSET
#define VMALLOC_START    (PAGE_OFFSET - VMALLOC_SIZE)

#define BPF_JIT_REGION_SIZE	(SZ_128M)
#ifdef CONFIG_64BIT
#define BPF_JIT_REGION_START	(BPF_JIT_REGION_END - BPF_JIT_REGION_SIZE)
#define BPF_JIT_REGION_END	(MODULES_END)
#else
#define BPF_JIT_REGION_START	(PAGE_OFFSET - BPF_JIT_REGION_SIZE)
#define BPF_JIT_REGION_END	(VMALLOC_END)
#endif

/* Modules always live before the kernel */
#ifdef CONFIG_64BIT
/* This is used to define the end of the KASAN shadow region */
#define MODULES_LOWEST_VADDR	(KERNEL_LINK_ADDR - SZ_2G)
#define MODULES_VADDR		(PFN_ALIGN((unsigned long)&_end) - SZ_2G)
#define MODULES_END		(PFN_ALIGN((unsigned long)&_start))
#endif

/*
 * Roughly size the vmemmap space to be large enough to fit enough
 * struct pages to map half the virtual address space. Then
 * position vmemmap directly below the VMALLOC region.
 */
#define VA_BITS_SV32 32
#ifdef CONFIG_64BIT
#define VA_BITS_SV39 39
#define VA_BITS_SV48 48
#define VA_BITS_SV57 57

#define VA_BITS		(pgtable_l5_enabled ? \
				VA_BITS_SV57 : (pgtable_l4_enabled ? VA_BITS_SV48 : VA_BITS_SV39))
#else
#define VA_BITS		VA_BITS_SV32
#endif

#define VMEMMAP_SHIFT \
	(VA_BITS - PAGE_SHIFT - 1 + STRUCT_PAGE_MAX_SHIFT)
#define VMEMMAP_SIZE	BIT(VMEMMAP_SHIFT)
#define VMEMMAP_END	VMALLOC_START
#define VMEMMAP_START	(VMALLOC_START - VMEMMAP_SIZE)

/*
 * Define vmemmap for pfn_to_page & page_to_pfn calls. Needed if kernel
 * is configured with CONFIG_SPARSEMEM_VMEMMAP enabled.
 */
#define vmemmap		((struct page *)VMEMMAP_START)

#define PCI_IO_SIZE      SZ_16M
#define PCI_IO_END       VMEMMAP_START
#define PCI_IO_START     (PCI_IO_END - PCI_IO_SIZE)

#define FIXADDR_TOP      PCI_IO_START
#ifdef CONFIG_64BIT
#define MAX_FDT_SIZE	 PMD_SIZE
#define FIX_FDT_SIZE	 (MAX_FDT_SIZE + SZ_2M)
#define FIXADDR_SIZE     (PMD_SIZE + FIX_FDT_SIZE)
#else
#define MAX_FDT_SIZE	 PGDIR_SIZE
#define FIX_FDT_SIZE	 MAX_FDT_SIZE
#define FIXADDR_SIZE     (PGDIR_SIZE + FIX_FDT_SIZE)
#endif
#define FIXADDR_START    (FIXADDR_TOP - FIXADDR_SIZE)

#endif

#ifdef CONFIG_XIP_KERNEL
#define XIP_OFFSET		SZ_32M
#define XIP_OFFSET_MASK		(SZ_32M - 1)
#else
#define XIP_OFFSET		0
#endif

#ifndef __ASSEMBLY__

#include <asm/page.h>
#include <asm/tlbflush.h>
#include <linux/mm_types.h>
#include <asm/compat.h>

#define __page_val_to_pfn(_val)  (((_val) & _PAGE_PFN_MASK) >> _PAGE_PFN_SHIFT)

#ifdef CONFIG_64BIT
#include <asm/pgtable-64.h>

#define VA_USER_SV39 (UL(1) << (VA_BITS_SV39 - 1))
#define VA_USER_SV48 (UL(1) << (VA_BITS_SV48 - 1))
#define VA_USER_SV57 (UL(1) << (VA_BITS_SV57 - 1))

#ifdef CONFIG_COMPAT
#define MMAP_VA_BITS_64 ((VA_BITS >= VA_BITS_SV48) ? VA_BITS_SV48 : VA_BITS)
#define MMAP_MIN_VA_BITS_64 (VA_BITS_SV39)
#define MMAP_VA_BITS (is_compat_task() ? VA_BITS_SV32 : MMAP_VA_BITS_64)
#define MMAP_MIN_VA_BITS (is_compat_task() ? VA_BITS_SV32 : MMAP_MIN_VA_BITS_64)
#else
#define MMAP_VA_BITS ((VA_BITS >= VA_BITS_SV48) ? VA_BITS_SV48 : VA_BITS)
#define MMAP_MIN_VA_BITS (VA_BITS_SV39)
#endif /* CONFIG_COMPAT */

#else
#include <asm/pgtable-32.h>
#endif /* CONFIG_64BIT */

#include <linux/page_table_check.h>

#ifdef CONFIG_XIP_KERNEL
#define XIP_FIXUP(addr) ({							\
	uintptr_t __a = (uintptr_t)(addr);					\
	(__a >= CONFIG_XIP_PHYS_ADDR && \
	 __a < CONFIG_XIP_PHYS_ADDR + XIP_OFFSET * 2) ?	\
		__a - CONFIG_XIP_PHYS_ADDR + CONFIG_PHYS_RAM_BASE - XIP_OFFSET :\
		__a;								\
	})
#else
#define XIP_FIXUP(addr)		(addr)
#endif /* CONFIG_XIP_KERNEL */

struct pt_alloc_ops {
	pte_t *(*get_pte_virt)(phys_addr_t pa);
	phys_addr_t (*alloc_pte)(uintptr_t va);
#ifndef __PAGETABLE_PMD_FOLDED
	pmd_t *(*get_pmd_virt)(phys_addr_t pa);
	phys_addr_t (*alloc_pmd)(uintptr_t va);
	pud_t *(*get_pud_virt)(phys_addr_t pa);
	phys_addr_t (*alloc_pud)(uintptr_t va);
	p4d_t *(*get_p4d_virt)(phys_addr_t pa);
	phys_addr_t (*alloc_p4d)(uintptr_t va);
#endif
};

extern struct pt_alloc_ops pt_ops __initdata;

#ifdef CONFIG_MMU
/* Number of PGD entries that a user-mode program can use */
#define USER_PTRS_PER_PGD   (TASK_SIZE / PGDIR_SIZE)

/* Page protection bits */
#define _PAGE_BASE	(_PAGE_PRESENT | _PAGE_ACCESSED | _PAGE_USER)

#define PAGE_NONE		__pgprot(_PAGE_PROT_NONE | _PAGE_READ)
#define PAGE_READ		__pgprot(_PAGE_BASE | _PAGE_READ)
#define PAGE_WRITE		__pgprot(_PAGE_BASE | _PAGE_READ | _PAGE_WRITE)
#define PAGE_EXEC		__pgprot(_PAGE_BASE | _PAGE_EXEC)
#define PAGE_READ_EXEC		__pgprot(_PAGE_BASE | _PAGE_READ | _PAGE_EXEC)
#define PAGE_WRITE_EXEC		__pgprot(_PAGE_BASE | _PAGE_READ |	\
					 _PAGE_EXEC | _PAGE_WRITE)

#define PAGE_COPY		PAGE_READ
#define PAGE_COPY_EXEC		PAGE_READ_EXEC
#define PAGE_SHARED		PAGE_WRITE
#define PAGE_SHARED_EXEC	PAGE_WRITE_EXEC

#define _PAGE_KERNEL		(_PAGE_READ \
				| _PAGE_WRITE \
				| _PAGE_PRESENT \
				| _PAGE_ACCESSED \
				| _PAGE_DIRTY \
				| _PAGE_GLOBAL)

#define PAGE_KERNEL		__pgprot(_PAGE_KERNEL)
#define PAGE_KERNEL_READ	__pgprot(_PAGE_KERNEL & ~_PAGE_WRITE)
#define PAGE_KERNEL_EXEC	__pgprot(_PAGE_KERNEL | _PAGE_EXEC)
#define PAGE_KERNEL_READ_EXEC	__pgprot((_PAGE_KERNEL & ~_PAGE_WRITE) \
					 | _PAGE_EXEC)

#define PAGE_TABLE		__pgprot(_PAGE_TABLE)

#define _PAGE_IOREMAP	((_PAGE_KERNEL & ~_PAGE_MTMASK) | _PAGE_IO)
#define PAGE_KERNEL_IO		__pgprot(_PAGE_IOREMAP)

extern pgd_t swapper_pg_dir[];
extern pgd_t trampoline_pg_dir[];
extern pgd_t early_pg_dir[];

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static inline int pmd_present(pmd_t pmd)
{
	/*
	 * Checking for _PAGE_LEAF is needed too because:
	 * When splitting a THP, split_huge_page() will temporarily clear
	 * the present bit, in this situation, pmd_present() and
	 * pmd_trans_huge() still needs to return true.
	 */
	return (pmd_val(pmd) & (_PAGE_PRESENT | _PAGE_PROT_NONE | _PAGE_LEAF));
}
#else
static inline int pmd_present(pmd_t pmd)
{
	return (pmd_val(pmd) & (_PAGE_PRESENT | _PAGE_PROT_NONE));
}
#endif

static inline int pmd_none(pmd_t pmd)
{
	return (pmd_val(pmd) == 0);
}

static inline int pmd_bad(pmd_t pmd)
{
	return !pmd_present(pmd) || (pmd_val(pmd) & _PAGE_LEAF);
}

#define pmd_leaf	pmd_leaf
static inline int pmd_leaf(pmd_t pmd)
{
	return pmd_present(pmd) && (pmd_val(pmd) & _PAGE_LEAF);
}

static inline void set_pmd(pmd_t *pmdp, pmd_t pmd)
{
	*pmdp = pmd;
}

static inline void pmd_clear(pmd_t *pmdp)
{
	set_pmd(pmdp, __pmd(0));
}

static inline pgd_t pfn_pgd(unsigned long pfn, pgprot_t prot)
{
	unsigned long prot_val = pgprot_val(prot);

	ALT_THEAD_PMA(prot_val);

	return __pgd((pfn << _PAGE_PFN_SHIFT) | prot_val);
}

static inline unsigned long _pgd_pfn(pgd_t pgd)
{
	return __page_val_to_pfn(pgd_val(pgd));
}

static inline struct page *pmd_page(pmd_t pmd)
{
	return pfn_to_page(__page_val_to_pfn(pmd_val(pmd)));
}

static inline unsigned long pmd_page_vaddr(pmd_t pmd)
{
	return (unsigned long)pfn_to_virt(__page_val_to_pfn(pmd_val(pmd)));
}

static inline pte_t pmd_pte(pmd_t pmd)
{
	return __pte(pmd_val(pmd));
}

static inline pte_t pud_pte(pud_t pud)
{
	return __pte(pud_val(pud));
}

#ifdef CONFIG_RISCV_ISA_SVNAPOT

static __always_inline bool has_svnapot(void)
{
	return riscv_has_extension_likely(RISCV_ISA_EXT_SVNAPOT);
}

static inline unsigned long pte_napot(pte_t pte)
{
	return pte_val(pte) & _PAGE_NAPOT;
}

static inline pte_t pte_mknapot(pte_t pte, unsigned int order)
{
	int pos = order - 1 + _PAGE_PFN_SHIFT;
	unsigned long napot_bit = BIT(pos);
	unsigned long napot_mask = ~GENMASK(pos, _PAGE_PFN_SHIFT);

	return __pte((pte_val(pte) & napot_mask) | napot_bit | _PAGE_NAPOT);
}

#else

static __always_inline bool has_svnapot(void) { return false; }

static inline unsigned long pte_napot(pte_t pte)
{
	return 0;
}

#endif /* CONFIG_RISCV_ISA_SVNAPOT */

/* Yields the page frame number (PFN) of a page table entry */
static inline unsigned long pte_pfn(pte_t pte)
{
	unsigned long res  = __page_val_to_pfn(pte_val(pte));

	if (has_svnapot() && pte_napot(pte))
		res = res & (res - 1UL);

	return res;
}

#define pte_page(x)     pfn_to_page(pte_pfn(x))

/* Constructs a page table entry */
static inline pte_t pfn_pte(unsigned long pfn, pgprot_t prot)
{
	unsigned long prot_val = pgprot_val(prot);

	ALT_THEAD_PMA(prot_val);

	return __pte((pfn << _PAGE_PFN_SHIFT) | prot_val);
}

#define mk_pte(page, prot)       pfn_pte(page_to_pfn(page), prot)

static inline int pte_present(pte_t pte)
{
	return (pte_val(pte) & (_PAGE_PRESENT | _PAGE_PROT_NONE));
}

static inline int pte_none(pte_t pte)
{
	return (pte_val(pte) == 0);
}

static inline int pte_write(pte_t pte)
{
	return pte_val(pte) & _PAGE_WRITE;
}

static inline int pte_exec(pte_t pte)
{
	return pte_val(pte) & _PAGE_EXEC;
}

static inline int pte_user(pte_t pte)
{
	return pte_val(pte) & _PAGE_USER;
}

static inline int pte_huge(pte_t pte)
{
	return pte_present(pte) && (pte_val(pte) & _PAGE_LEAF);
}

static inline int pte_dirty(pte_t pte)
{
	return pte_val(pte) & _PAGE_DIRTY;
}

static inline int pte_young(pte_t pte)
{
	return pte_val(pte) & _PAGE_ACCESSED;
}

static inline int pte_special(pte_t pte)
{
	return pte_val(pte) & _PAGE_SPECIAL;
}

/* static inline pte_t pte_rdprotect(pte_t pte) */

static inline pte_t pte_wrprotect(pte_t pte)
{
	return __pte(pte_val(pte) & ~(_PAGE_WRITE));
}

/* static inline pte_t pte_mkread(pte_t pte) */

static inline pte_t pte_mkwrite_novma(pte_t pte)
{
	return __pte(pte_val(pte) | _PAGE_WRITE);
}

/* static inline pte_t pte_mkexec(pte_t pte) */

static inline pte_t pte_mkdirty(pte_t pte)
{
	return __pte(pte_val(pte) | _PAGE_DIRTY);
}

static inline pte_t pte_mkclean(pte_t pte)
{
	return __pte(pte_val(pte) & ~(_PAGE_DIRTY));
}

static inline pte_t pte_mkyoung(pte_t pte)
{
	return __pte(pte_val(pte) | _PAGE_ACCESSED);
}

static inline pte_t pte_mkold(pte_t pte)
{
	return __pte(pte_val(pte) & ~(_PAGE_ACCESSED));
}

static inline pte_t pte_mkspecial(pte_t pte)
{
	return __pte(pte_val(pte) | _PAGE_SPECIAL);
}

static inline pte_t pte_mkhuge(pte_t pte)
{
	return pte;
}

#ifdef CONFIG_NUMA_BALANCING
/*
 * See the comment in include/asm-generic/pgtable.h
 */
static inline int pte_protnone(pte_t pte)
{
	return (pte_val(pte) & (_PAGE_PRESENT | _PAGE_PROT_NONE)) == _PAGE_PROT_NONE;
}

static inline int pmd_protnone(pmd_t pmd)
{
	return pte_protnone(pmd_pte(pmd));
}
#endif

/* Modify page protection bits */
static inline pte_t pte_modify(pte_t pte, pgprot_t newprot)
{
	unsigned long newprot_val = pgprot_val(newprot);

	ALT_THEAD_PMA(newprot_val);

	return __pte((pte_val(pte) & _PAGE_CHG_MASK) | newprot_val);
}

#define pgd_ERROR(e) \
	pr_err("%s:%d: bad pgd " PTE_FMT ".\n", __FILE__, __LINE__, pgd_val(e))


/* Commit new configuration to MMU hardware */
static inline void update_mmu_cache_range(struct vm_fault *vmf,
		struct vm_area_struct *vma, unsigned long address,
		pte_t *ptep, unsigned int nr)
{
	/*
	 * The kernel assumes that TLBs don't cache invalid entries, but
	 * in RISC-V, SFENCE.VMA specifies an ordering constraint, not a
	 * cache flush; it is necessary even after writing invalid entries.
	 * Relying on flush_tlb_fix_spurious_fault would suffice, but
	 * the extra traps reduce performance.  So, eagerly SFENCE.VMA.
	 */
	while (nr--)
		local_flush_tlb_page(address + nr * PAGE_SIZE);
}
#define update_mmu_cache(vma, addr, ptep) \
	update_mmu_cache_range(NULL, vma, addr, ptep, 1)

#define __HAVE_ARCH_UPDATE_MMU_TLB
#define update_mmu_tlb update_mmu_cache

static inline void update_mmu_cache_pmd(struct vm_area_struct *vma,
		unsigned long address, pmd_t *pmdp)
{
	pte_t *ptep = (pte_t *)pmdp;

	update_mmu_cache(vma, address, ptep);
}

#define __HAVE_ARCH_PTE_SAME
static inline int pte_same(pte_t pte_a, pte_t pte_b)
{
	return pte_val(pte_a) == pte_val(pte_b);
}

/*
 * Certain architectures need to do special things when PTEs within
 * a page table are directly modified.  Thus, the following hook is
 * made available.
 */
static inline void set_pte(pte_t *ptep, pte_t pteval)
{
	*ptep = pteval;
}

void flush_icache_pte(pte_t pte);

static inline void __set_pte_at(pte_t *ptep, pte_t pteval)
{
	if (pte_present(pteval) && pte_exec(pteval))
		flush_icache_pte(pteval);

	set_pte(ptep, pteval);
}

static inline void set_ptes(struct mm_struct *mm, unsigned long addr,
		pte_t *ptep, pte_t pteval, unsigned int nr)
{
	page_table_check_ptes_set(mm, ptep, pteval, nr);

	for (;;) {
		__set_pte_at(ptep, pteval);
		if (--nr == 0)
			break;
		ptep++;
		pte_val(pteval) += 1 << _PAGE_PFN_SHIFT;
	}
}
#define set_ptes set_ptes

static inline void pte_clear(struct mm_struct *mm,
	unsigned long addr, pte_t *ptep)
{
	__set_pte_at(ptep, __pte(0));
}

#define __HAVE_ARCH_PTEP_SET_ACCESS_FLAGS
static inline int ptep_set_access_flags(struct vm_area_struct *vma,
					unsigned long address, pte_t *ptep,
					pte_t entry, int dirty)
{
	if (!pte_same(*ptep, entry))
		__set_pte_at(ptep, entry);
	/*
	 * update_mmu_cache will unconditionally execute, handling both
	 * the case that the PTE changed and the spurious fault case.
	 */
	return true;
}

#define __HAVE_ARCH_PTEP_GET_AND_CLEAR
static inline pte_t ptep_get_and_clear(struct mm_struct *mm,
				       unsigned long address, pte_t *ptep)
{
	pte_t pte = __pte(atomic_long_xchg((atomic_long_t *)ptep, 0));

	page_table_check_pte_clear(mm, pte);

	return pte;
}

#define __HAVE_ARCH_PTEP_TEST_AND_CLEAR_YOUNG
static inline int ptep_test_and_clear_young(struct vm_area_struct *vma,
					    unsigned long address,
					    pte_t *ptep)
{
	if (!pte_young(*ptep))
		return 0;
	return test_and_clear_bit(_PAGE_ACCESSED_OFFSET, &pte_val(*ptep));
}

#define __HAVE_ARCH_PTEP_SET_WRPROTECT
static inline void ptep_set_wrprotect(struct mm_struct *mm,
				      unsigned long address, pte_t *ptep)
{
	atomic_long_and(~(unsigned long)_PAGE_WRITE, (atomic_long_t *)ptep);
}

#define __HAVE_ARCH_PTEP_CLEAR_YOUNG_FLUSH
static inline int ptep_clear_flush_young(struct vm_area_struct *vma,
					 unsigned long address, pte_t *ptep)
{
	/*
	 * This comment is borrowed from x86, but applies equally to RISC-V:
	 *
	 * Clearing the accessed bit without a TLB flush
	 * doesn't cause data corruption. [ It could cause incorrect
	 * page aging and the (mistaken) reclaim of hot pages, but the
	 * chance of that should be relatively low. ]
	 *
	 * So as a performance optimization don't flush the TLB when
	 * clearing the accessed bit, it will eventually be flushed by
	 * a context switch or a VM operation anyway. [ In the rare
	 * event of it not getting flushed for a long time the delay
	 * shouldn't really matter because there's no real memory
	 * pressure for swapout to react to. ]
	 */
	return ptep_test_and_clear_young(vma, address, ptep);
}

#define pgprot_noncached pgprot_noncached
static inline pgprot_t pgprot_noncached(pgprot_t _prot)
{
	unsigned long prot = pgprot_val(_prot);

	prot &= ~_PAGE_MTMASK;
	prot |= _PAGE_IO;

	return __pgprot(prot);
}

#define pgprot_writecombine pgprot_writecombine
static inline pgprot_t pgprot_writecombine(pgprot_t _prot)
{
	unsigned long prot = pgprot_val(_prot);

	prot &= ~_PAGE_MTMASK;
	prot |= _PAGE_NOCACHE;

	return __pgprot(prot);
}

/*
 * THP functions
 */
static inline pmd_t pte_pmd(pte_t pte)
{
	return __pmd(pte_val(pte));
}

static inline pmd_t pmd_mkhuge(pmd_t pmd)
{
	return pmd;
}

static inline pmd_t pmd_mkinvalid(pmd_t pmd)
{
	return __pmd(pmd_val(pmd) & ~(_PAGE_PRESENT|_PAGE_PROT_NONE));
}

#define __pmd_to_phys(pmd)  (__page_val_to_pfn(pmd_val(pmd)) << PAGE_SHIFT)

static inline unsigned long pmd_pfn(pmd_t pmd)
{
	return ((__pmd_to_phys(pmd) & PMD_MASK) >> PAGE_SHIFT);
}

#define __pud_to_phys(pud)  (__page_val_to_pfn(pud_val(pud)) << PAGE_SHIFT)

static inline unsigned long pud_pfn(pud_t pud)
{
	return ((__pud_to_phys(pud) & PUD_MASK) >> PAGE_SHIFT);
}

static inline pmd_t pmd_modify(pmd_t pmd, pgprot_t newprot)
{
	return pte_pmd(pte_modify(pmd_pte(pmd), newprot));
}

#define pmd_write pmd_write
static inline int pmd_write(pmd_t pmd)
{
	return pte_write(pmd_pte(pmd));
}

static inline int pmd_dirty(pmd_t pmd)
{
	return pte_dirty(pmd_pte(pmd));
}

#define pmd_young pmd_young
static inline int pmd_young(pmd_t pmd)
{
	return pte_young(pmd_pte(pmd));
}

static inline int pmd_user(pmd_t pmd)
{
	return pte_user(pmd_pte(pmd));
}

static inline pmd_t pmd_mkold(pmd_t pmd)
{
	return pte_pmd(pte_mkold(pmd_pte(pmd)));
}

static inline pmd_t pmd_mkyoung(pmd_t pmd)
{
	return pte_pmd(pte_mkyoung(pmd_pte(pmd)));
}

static inline pmd_t pmd_mkwrite_novma(pmd_t pmd)
{
	return pte_pmd(pte_mkwrite_novma(pmd_pte(pmd)));
}

static inline pmd_t pmd_wrprotect(pmd_t pmd)
{
	return pte_pmd(pte_wrprotect(pmd_pte(pmd)));
}

static inline pmd_t pmd_mkclean(pmd_t pmd)
{
	return pte_pmd(pte_mkclean(pmd_pte(pmd)));
}

static inline pmd_t pmd_mkdirty(pmd_t pmd)
{
	return pte_pmd(pte_mkdirty(pmd_pte(pmd)));
}

static inline void set_pmd_at(struct mm_struct *mm, unsigned long addr,
				pmd_t *pmdp, pmd_t pmd)
{
	page_table_check_pmd_set(mm, pmdp, pmd);
	return __set_pte_at((pte_t *)pmdp, pmd_pte(pmd));
}

static inline void set_pud_at(struct mm_struct *mm, unsigned long addr,
				pud_t *pudp, pud_t pud)
{
	page_table_check_pud_set(mm, pudp, pud);
	return __set_pte_at((pte_t *)pudp, pud_pte(pud));
}

#ifdef CONFIG_PAGE_TABLE_CHECK
static inline bool pte_user_accessible_page(pte_t pte)
{
	return pte_present(pte) && pte_user(pte);
}

static inline bool pmd_user_accessible_page(pmd_t pmd)
{
	return pmd_leaf(pmd) && pmd_user(pmd);
}

static inline bool pud_user_accessible_page(pud_t pud)
{
	return pud_leaf(pud) && pud_user(pud);
}
#endif

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static inline int pmd_trans_huge(pmd_t pmd)
{
	return pmd_leaf(pmd);
}

#define __HAVE_ARCH_PMDP_SET_ACCESS_FLAGS
static inline int pmdp_set_access_flags(struct vm_area_struct *vma,
					unsigned long address, pmd_t *pmdp,
					pmd_t entry, int dirty)
{
	return ptep_set_access_flags(vma, address, (pte_t *)pmdp, pmd_pte(entry), dirty);
}

#define __HAVE_ARCH_PMDP_TEST_AND_CLEAR_YOUNG
static inline int pmdp_test_and_clear_young(struct vm_area_struct *vma,
					unsigned long address, pmd_t *pmdp)
{
	return ptep_test_and_clear_young(vma, address, (pte_t *)pmdp);
}

#define __HAVE_ARCH_PMDP_HUGE_GET_AND_CLEAR
static inline pmd_t pmdp_huge_get_and_clear(struct mm_struct *mm,
					unsigned long address, pmd_t *pmdp)
{
	pmd_t pmd = __pmd(atomic_long_xchg((atomic_long_t *)pmdp, 0));

	page_table_check_pmd_clear(mm, pmd);

	return pmd;
}

#define __HAVE_ARCH_PMDP_SET_WRPROTECT
static inline void pmdp_set_wrprotect(struct mm_struct *mm,
					unsigned long address, pmd_t *pmdp)
{
	ptep_set_wrprotect(mm, address, (pte_t *)pmdp);
}

#define pmdp_establish pmdp_establish
static inline pmd_t pmdp_establish(struct vm_area_struct *vma,
				unsigned long address, pmd_t *pmdp, pmd_t pmd)
{
	page_table_check_pmd_set(vma->vm_mm, pmdp, pmd);
	return __pmd(atomic_long_xchg((atomic_long_t *)pmdp, pmd_val(pmd)));
}

#define pmdp_collapse_flush pmdp_collapse_flush
extern pmd_t pmdp_collapse_flush(struct vm_area_struct *vma,
				 unsigned long address, pmd_t *pmdp);
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

/*
 * Encode/decode swap entries and swap PTEs. Swap PTEs are all PTEs that
 * are !pte_none() && !pte_present().
 *
 * Format of swap PTE:
 *	bit            0:	_PAGE_PRESENT (zero)
 *	bit       1 to 3:       _PAGE_LEAF (zero)
 *	bit            5:	_PAGE_PROT_NONE (zero)
 *	bit            6:	exclusive marker
 *	bits      7 to 11:	swap type
 *	bits 11 to XLEN-1:	swap offset
 */
#define __SWP_TYPE_SHIFT	7
#define __SWP_TYPE_BITS		5
#define __SWP_TYPE_MASK		((1UL << __SWP_TYPE_BITS) - 1)
#define __SWP_OFFSET_SHIFT	(__SWP_TYPE_BITS + __SWP_TYPE_SHIFT)

#define MAX_SWAPFILES_CHECK()	\
	BUILD_BUG_ON(MAX_SWAPFILES_SHIFT > __SWP_TYPE_BITS)

#define __swp_type(x)	(((x).val >> __SWP_TYPE_SHIFT) & __SWP_TYPE_MASK)
#define __swp_offset(x)	((x).val >> __SWP_OFFSET_SHIFT)
#define __swp_entry(type, offset) ((swp_entry_t) \
	{ (((type) & __SWP_TYPE_MASK) << __SWP_TYPE_SHIFT) | \
	  ((offset) << __SWP_OFFSET_SHIFT) })

#define __pte_to_swp_entry(pte)	((swp_entry_t) { pte_val(pte) })
#define __swp_entry_to_pte(x)	((pte_t) { (x).val })

static inline int pte_swp_exclusive(pte_t pte)
{
	return pte_val(pte) & _PAGE_SWP_EXCLUSIVE;
}

static inline pte_t pte_swp_mkexclusive(pte_t pte)
{
	return __pte(pte_val(pte) | _PAGE_SWP_EXCLUSIVE);
}

static inline pte_t pte_swp_clear_exclusive(pte_t pte)
{
	return __pte(pte_val(pte) & ~_PAGE_SWP_EXCLUSIVE);
}

#ifdef CONFIG_ARCH_ENABLE_THP_MIGRATION
#define __pmd_to_swp_entry(pmd) ((swp_entry_t) { pmd_val(pmd) })
#define __swp_entry_to_pmd(swp) __pmd((swp).val)
#endif /* CONFIG_ARCH_ENABLE_THP_MIGRATION */

/*
 * In the RV64 Linux scheme, we give the user half of the virtual-address space
 * and give the kernel the other (upper) half.
 */
#ifdef CONFIG_64BIT
#define KERN_VIRT_START	(-(BIT(VA_BITS)) + TASK_SIZE)
#else
#define KERN_VIRT_START	FIXADDR_START
#endif

/*
 * Task size is 0x4000000000 for RV64 or 0x9fc00000 for RV32.
 * Note that PGDIR_SIZE must evenly divide TASK_SIZE.
 * Task size is:
 * -        0x9fc00000	(~2.5GB) for RV32.
 * -      0x4000000000	( 256GB) for RV64 using SV39 mmu
 * -    0x800000000000	( 128TB) for RV64 using SV48 mmu
 * - 0x100000000000000	(  64PB) for RV64 using SV57 mmu
 *
 * Note that PGDIR_SIZE must evenly divide TASK_SIZE since "RISC-V
 * Instruction Set Manual Volume II: Privileged Architecture" states that
 * "load and store effective addresses, which are 64bits, must have bits
 * 63–48 all equal to bit 47, or else a page-fault exception will occur."
 * Similarly for SV57, bits 63–57 must be equal to bit 56.
 */
#ifdef CONFIG_64BIT
#define TASK_SIZE_64	(PGDIR_SIZE * PTRS_PER_PGD / 2)
#define TASK_SIZE_MIN	(PGDIR_SIZE_L3 * PTRS_PER_PGD / 2)

#ifdef CONFIG_COMPAT
#define TASK_SIZE_32	(_AC(0x80000000, UL) - PAGE_SIZE)
#define TASK_SIZE	(test_thread_flag(TIF_32BIT) ? \
			 TASK_SIZE_32 : TASK_SIZE_64)
#else
#define TASK_SIZE	TASK_SIZE_64
#endif

#else
#define TASK_SIZE	FIXADDR_START
#define TASK_SIZE_MIN	TASK_SIZE
#endif

#else /* CONFIG_MMU */

#define PAGE_SHARED		__pgprot(0)
#define PAGE_KERNEL		__pgprot(0)
#define swapper_pg_dir		NULL
#define TASK_SIZE		0xffffffffUL
#define VMALLOC_START		0
#define VMALLOC_END		TASK_SIZE

#endif /* !CONFIG_MMU */

extern char _start[];
extern void *_dtb_early_va;
extern uintptr_t _dtb_early_pa;
#if defined(CONFIG_XIP_KERNEL) && defined(CONFIG_MMU)
#define dtb_early_va	(*(void **)XIP_FIXUP(&_dtb_early_va))
#define dtb_early_pa	(*(uintptr_t *)XIP_FIXUP(&_dtb_early_pa))
#else
#define dtb_early_va	_dtb_early_va
#define dtb_early_pa	_dtb_early_pa
#endif /* CONFIG_XIP_KERNEL */
extern u64 satp_mode;
extern bool pgtable_l4_enabled;

void paging_init(void);
void misc_mem_init(void);

/*
 * ZERO_PAGE is a global shared page that is always zero,
 * used for zero-mapped memory areas, etc.
 */
extern unsigned long empty_zero_page[PAGE_SIZE / sizeof(unsigned long)];
#define ZERO_PAGE(vaddr) (virt_to_page(empty_zero_page))

#endif /* !__ASSEMBLY__ */

#endif /* _ASM_RISCV_PGTABLE_H */
