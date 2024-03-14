/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common Primitives for Data Access Monitoring
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

#include <linux/damon.h>

struct folio *damon_get_folio(unsigned long pfn);

void damon_ptep_mkold(pte_t *pte, struct vm_area_struct *vma, unsigned long addr);
void damon_pmdp_mkold(pmd_t *pmd, struct vm_area_struct *vma, unsigned long addr);

int damon_cold_score(struct damon_ctx *c, struct damon_region *r,
			struct damos *s);
int damon_hot_score(struct damon_ctx *c, struct damon_region *r,
			struct damos *s);
