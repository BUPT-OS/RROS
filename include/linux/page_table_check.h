/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2021, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#ifndef __LINUX_PAGE_TABLE_CHECK_H
#define __LINUX_PAGE_TABLE_CHECK_H

#ifdef CONFIG_PAGE_TABLE_CHECK
#include <linux/jump_label.h>

extern struct static_key_true page_table_check_disabled;
extern struct page_ext_operations page_table_check_ops;

void __page_table_check_zero(struct page *page, unsigned int order);
void __page_table_check_pte_clear(struct mm_struct *mm, pte_t pte);
void __page_table_check_pmd_clear(struct mm_struct *mm, pmd_t pmd);
void __page_table_check_pud_clear(struct mm_struct *mm, pud_t pud);
void __page_table_check_ptes_set(struct mm_struct *mm, pte_t *ptep, pte_t pte,
		unsigned int nr);
void __page_table_check_pmd_set(struct mm_struct *mm, pmd_t *pmdp, pmd_t pmd);
void __page_table_check_pud_set(struct mm_struct *mm, pud_t *pudp, pud_t pud);
void __page_table_check_pte_clear_range(struct mm_struct *mm,
					unsigned long addr,
					pmd_t pmd);

static inline void page_table_check_alloc(struct page *page, unsigned int order)
{
	if (static_branch_likely(&page_table_check_disabled))
		return;

	__page_table_check_zero(page, order);
}

static inline void page_table_check_free(struct page *page, unsigned int order)
{
	if (static_branch_likely(&page_table_check_disabled))
		return;

	__page_table_check_zero(page, order);
}

static inline void page_table_check_pte_clear(struct mm_struct *mm, pte_t pte)
{
	if (static_branch_likely(&page_table_check_disabled))
		return;

	__page_table_check_pte_clear(mm, pte);
}

static inline void page_table_check_pmd_clear(struct mm_struct *mm, pmd_t pmd)
{
	if (static_branch_likely(&page_table_check_disabled))
		return;

	__page_table_check_pmd_clear(mm, pmd);
}

static inline void page_table_check_pud_clear(struct mm_struct *mm, pud_t pud)
{
	if (static_branch_likely(&page_table_check_disabled))
		return;

	__page_table_check_pud_clear(mm, pud);
}

static inline void page_table_check_ptes_set(struct mm_struct *mm,
		pte_t *ptep, pte_t pte, unsigned int nr)
{
	if (static_branch_likely(&page_table_check_disabled))
		return;

	__page_table_check_ptes_set(mm, ptep, pte, nr);
}

static inline void page_table_check_pmd_set(struct mm_struct *mm, pmd_t *pmdp,
					    pmd_t pmd)
{
	if (static_branch_likely(&page_table_check_disabled))
		return;

	__page_table_check_pmd_set(mm, pmdp, pmd);
}

static inline void page_table_check_pud_set(struct mm_struct *mm, pud_t *pudp,
					    pud_t pud)
{
	if (static_branch_likely(&page_table_check_disabled))
		return;

	__page_table_check_pud_set(mm, pudp, pud);
}

static inline void page_table_check_pte_clear_range(struct mm_struct *mm,
						    unsigned long addr,
						    pmd_t pmd)
{
	if (static_branch_likely(&page_table_check_disabled))
		return;

	__page_table_check_pte_clear_range(mm, addr, pmd);
}

#else

static inline void page_table_check_alloc(struct page *page, unsigned int order)
{
}

static inline void page_table_check_free(struct page *page, unsigned int order)
{
}

static inline void page_table_check_pte_clear(struct mm_struct *mm, pte_t pte)
{
}

static inline void page_table_check_pmd_clear(struct mm_struct *mm, pmd_t pmd)
{
}

static inline void page_table_check_pud_clear(struct mm_struct *mm, pud_t pud)
{
}

static inline void page_table_check_ptes_set(struct mm_struct *mm,
		pte_t *ptep, pte_t pte, unsigned int nr)
{
}

static inline void page_table_check_pmd_set(struct mm_struct *mm, pmd_t *pmdp,
					    pmd_t pmd)
{
}

static inline void page_table_check_pud_set(struct mm_struct *mm, pud_t *pudp,
					    pud_t pud)
{
}

static inline void page_table_check_pte_clear_range(struct mm_struct *mm,
						    unsigned long addr,
						    pmd_t pmd)
{
}

#endif /* CONFIG_PAGE_TABLE_CHECK */
#endif /* __LINUX_PAGE_TABLE_CHECK_H */
