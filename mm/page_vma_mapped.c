// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/hugetlb.h>
#include <linux/swap.h>
#include <linux/swapops.h>

#include "internal.h"

static inline bool not_found(struct page_vma_mapped_walk *pvmw)
{
	page_vma_mapped_walk_done(pvmw);
	return false;
}

static bool map_pte(struct page_vma_mapped_walk *pvmw, spinlock_t **ptlp)
{
	pte_t ptent;

	if (pvmw->flags & PVMW_SYNC) {
		/* Use the stricter lookup */
		pvmw->pte = pte_offset_map_lock(pvmw->vma->vm_mm, pvmw->pmd,
						pvmw->address, &pvmw->ptl);
		*ptlp = pvmw->ptl;
		return !!pvmw->pte;
	}

	/*
	 * It is important to return the ptl corresponding to pte,
	 * in case *pvmw->pmd changes underneath us; so we need to
	 * return it even when choosing not to lock, in case caller
	 * proceeds to loop over next ptes, and finds a match later.
	 * Though, in most cases, page lock already protects this.
	 */
	pvmw->pte = pte_offset_map_nolock(pvmw->vma->vm_mm, pvmw->pmd,
					  pvmw->address, ptlp);
	if (!pvmw->pte)
		return false;

	ptent = ptep_get(pvmw->pte);

	if (pvmw->flags & PVMW_MIGRATION) {
		if (!is_swap_pte(ptent))
			return false;
	} else if (is_swap_pte(ptent)) {
		swp_entry_t entry;
		/*
		 * Handle un-addressable ZONE_DEVICE memory.
		 *
		 * We get here when we are trying to unmap a private
		 * device page from the process address space. Such
		 * page is not CPU accessible and thus is mapped as
		 * a special swap entry, nonetheless it still does
		 * count as a valid regular mapping for the page
		 * (and is accounted as such in page maps count).
		 *
		 * So handle this special case as if it was a normal
		 * page mapping ie lock CPU page table and return true.
		 *
		 * For more details on device private memory see HMM
		 * (include/linux/hmm.h or mm/hmm.c).
		 */
		entry = pte_to_swp_entry(ptent);
		if (!is_device_private_entry(entry) &&
		    !is_device_exclusive_entry(entry))
			return false;
	} else if (!pte_present(ptent)) {
		return false;
	}
	pvmw->ptl = *ptlp;
	spin_lock(pvmw->ptl);
	return true;
}

/**
 * check_pte - check if [pvmw->pfn, @pvmw->pfn + @pvmw->nr_pages) is
 * mapped at the @pvmw->pte
 * @pvmw: page_vma_mapped_walk struct, includes a pair pte and pfn range
 * for checking
 *
 * page_vma_mapped_walk() found a place where pfn range is *potentially*
 * mapped. check_pte() has to validate this.
 *
 * pvmw->pte may point to empty PTE, swap PTE or PTE pointing to
 * arbitrary page.
 *
 * If PVMW_MIGRATION flag is set, returns true if @pvmw->pte contains migration
 * entry that points to [pvmw->pfn, @pvmw->pfn + @pvmw->nr_pages)
 *
 * If PVMW_MIGRATION flag is not set, returns true if pvmw->pte points to
 * [pvmw->pfn, @pvmw->pfn + @pvmw->nr_pages)
 *
 * Otherwise, return false.
 *
 */
static bool check_pte(struct page_vma_mapped_walk *pvmw)
{
	unsigned long pfn;
	pte_t ptent = ptep_get(pvmw->pte);

	if (pvmw->flags & PVMW_MIGRATION) {
		swp_entry_t entry;
		if (!is_swap_pte(ptent))
			return false;
		entry = pte_to_swp_entry(ptent);

		if (!is_migration_entry(entry) &&
		    !is_device_exclusive_entry(entry))
			return false;

		pfn = swp_offset_pfn(entry);
	} else if (is_swap_pte(ptent)) {
		swp_entry_t entry;

		/* Handle un-addressable ZONE_DEVICE memory */
		entry = pte_to_swp_entry(ptent);
		if (!is_device_private_entry(entry) &&
		    !is_device_exclusive_entry(entry))
			return false;

		pfn = swp_offset_pfn(entry);
	} else {
		if (!pte_present(ptent))
			return false;

		pfn = pte_pfn(ptent);
	}

	return (pfn - pvmw->pfn) < pvmw->nr_pages;
}

/* Returns true if the two ranges overlap.  Careful to not overflow. */
static bool check_pmd(unsigned long pfn, struct page_vma_mapped_walk *pvmw)
{
	if ((pfn + HPAGE_PMD_NR - 1) < pvmw->pfn)
		return false;
	if (pfn > pvmw->pfn + pvmw->nr_pages - 1)
		return false;
	return true;
}

static void step_forward(struct page_vma_mapped_walk *pvmw, unsigned long size)
{
	pvmw->address = (pvmw->address + size) & ~(size - 1);
	if (!pvmw->address)
		pvmw->address = ULONG_MAX;
}

/**
 * page_vma_mapped_walk - check if @pvmw->pfn is mapped in @pvmw->vma at
 * @pvmw->address
 * @pvmw: pointer to struct page_vma_mapped_walk. page, vma, address and flags
 * must be set. pmd, pte and ptl must be NULL.
 *
 * Returns true if the page is mapped in the vma. @pvmw->pmd and @pvmw->pte point
 * to relevant page table entries. @pvmw->ptl is locked. @pvmw->address is
 * adjusted if needed (for PTE-mapped THPs).
 *
 * If @pvmw->pmd is set but @pvmw->pte is not, you have found PMD-mapped page
 * (usually THP). For PTE-mapped THP, you should run page_vma_mapped_walk() in
 * a loop to find all PTEs that map the THP.
 *
 * For HugeTLB pages, @pvmw->pte is set to the relevant page table entry
 * regardless of which page table level the page is mapped at. @pvmw->pmd is
 * NULL.
 *
 * Returns false if there are no more page table entries for the page in
 * the vma. @pvmw->ptl is unlocked and @pvmw->pte is unmapped.
 *
 * If you need to stop the walk before page_vma_mapped_walk() returned false,
 * use page_vma_mapped_walk_done(). It will do the housekeeping.
 */
bool page_vma_mapped_walk(struct page_vma_mapped_walk *pvmw)
{
	struct vm_area_struct *vma = pvmw->vma;
	struct mm_struct *mm = vma->vm_mm;
	unsigned long end;
	spinlock_t *ptl;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t pmde;

	/* The only possible pmd mapping has been handled on last iteration */
	if (pvmw->pmd && !pvmw->pte)
		return not_found(pvmw);

	if (unlikely(is_vm_hugetlb_page(vma))) {
		struct hstate *hstate = hstate_vma(vma);
		unsigned long size = huge_page_size(hstate);
		/* The only possible mapping was handled on last iteration */
		if (pvmw->pte)
			return not_found(pvmw);
		/*
		 * All callers that get here will already hold the
		 * i_mmap_rwsem.  Therefore, no additional locks need to be
		 * taken before calling hugetlb_walk().
		 */
		pvmw->pte = hugetlb_walk(vma, pvmw->address, size);
		if (!pvmw->pte)
			return false;

		pvmw->ptl = huge_pte_lock(hstate, mm, pvmw->pte);
		if (!check_pte(pvmw))
			return not_found(pvmw);
		return true;
	}

	end = vma_address_end(pvmw);
	if (pvmw->pte)
		goto next_pte;
restart:
	do {
		pgd = pgd_offset(mm, pvmw->address);
		if (!pgd_present(*pgd)) {
			step_forward(pvmw, PGDIR_SIZE);
			continue;
		}
		p4d = p4d_offset(pgd, pvmw->address);
		if (!p4d_present(*p4d)) {
			step_forward(pvmw, P4D_SIZE);
			continue;
		}
		pud = pud_offset(p4d, pvmw->address);
		if (!pud_present(*pud)) {
			step_forward(pvmw, PUD_SIZE);
			continue;
		}

		pvmw->pmd = pmd_offset(pud, pvmw->address);
		/*
		 * Make sure the pmd value isn't cached in a register by the
		 * compiler and used as a stale value after we've observed a
		 * subsequent update.
		 */
		pmde = pmdp_get_lockless(pvmw->pmd);

		if (pmd_trans_huge(pmde) || is_pmd_migration_entry(pmde) ||
		    (pmd_present(pmde) && pmd_devmap(pmde))) {
			pvmw->ptl = pmd_lock(mm, pvmw->pmd);
			pmde = *pvmw->pmd;
			if (!pmd_present(pmde)) {
				swp_entry_t entry;

				if (!thp_migration_supported() ||
				    !(pvmw->flags & PVMW_MIGRATION))
					return not_found(pvmw);
				entry = pmd_to_swp_entry(pmde);
				if (!is_migration_entry(entry) ||
				    !check_pmd(swp_offset_pfn(entry), pvmw))
					return not_found(pvmw);
				return true;
			}
			if (likely(pmd_trans_huge(pmde) || pmd_devmap(pmde))) {
				if (pvmw->flags & PVMW_MIGRATION)
					return not_found(pvmw);
				if (!check_pmd(pmd_pfn(pmde), pvmw))
					return not_found(pvmw);
				return true;
			}
			/* THP pmd was split under us: handle on pte level */
			spin_unlock(pvmw->ptl);
			pvmw->ptl = NULL;
		} else if (!pmd_present(pmde)) {
			/*
			 * If PVMW_SYNC, take and drop THP pmd lock so that we
			 * cannot return prematurely, while zap_huge_pmd() has
			 * cleared *pmd but not decremented compound_mapcount().
			 */
			if ((pvmw->flags & PVMW_SYNC) &&
			    transhuge_vma_suitable(vma, pvmw->address) &&
			    (pvmw->nr_pages >= HPAGE_PMD_NR)) {
				spinlock_t *ptl = pmd_lock(mm, pvmw->pmd);

				spin_unlock(ptl);
			}
			step_forward(pvmw, PMD_SIZE);
			continue;
		}
		if (!map_pte(pvmw, &ptl)) {
			if (!pvmw->pte)
				goto restart;
			goto next_pte;
		}
this_pte:
		if (check_pte(pvmw))
			return true;
next_pte:
		do {
			pvmw->address += PAGE_SIZE;
			if (pvmw->address >= end)
				return not_found(pvmw);
			/* Did we cross page table boundary? */
			if ((pvmw->address & (PMD_SIZE - PAGE_SIZE)) == 0) {
				if (pvmw->ptl) {
					spin_unlock(pvmw->ptl);
					pvmw->ptl = NULL;
				}
				pte_unmap(pvmw->pte);
				pvmw->pte = NULL;
				goto restart;
			}
			pvmw->pte++;
		} while (pte_none(ptep_get(pvmw->pte)));

		if (!pvmw->ptl) {
			pvmw->ptl = ptl;
			spin_lock(pvmw->ptl);
		}
		goto this_pte;
	} while (pvmw->address < end);

	return false;
}

/**
 * page_mapped_in_vma - check whether a page is really mapped in a VMA
 * @page: the page to test
 * @vma: the VMA to test
 *
 * Returns 1 if the page is mapped into the page tables of the VMA, 0
 * if the page is not mapped into the page tables of this VMA.  Only
 * valid for normal file or anonymous VMAs.
 */
int page_mapped_in_vma(struct page *page, struct vm_area_struct *vma)
{
	struct page_vma_mapped_walk pvmw = {
		.pfn = page_to_pfn(page),
		.nr_pages = 1,
		.vma = vma,
		.flags = PVMW_SYNC,
	};

	pvmw.address = vma_address(page, vma);
	if (pvmw.address == -EFAULT)
		return 0;
	if (!page_vma_mapped_walk(&pvmw))
		return 0;
	page_vma_mapped_walk_done(&pvmw);
	return 1;
}
