// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple DART page table allocator.
 *
 * Copyright (C) 2022 The Asahi Linux Contributors
 *
 * Based on io-pgtable-arm.
 *
 * Copyright (C) 2014 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#define pr_fmt(fmt)	"dart io-pgtable: " fmt

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/io-pgtable.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <asm/barrier.h>

#define DART1_MAX_ADDR_BITS	36

#define DART_MAX_TABLES		4
#define DART_LEVELS		2

/* Struct accessors */
#define io_pgtable_to_data(x)						\
	container_of((x), struct dart_io_pgtable, iop)

#define io_pgtable_ops_to_data(x)					\
	io_pgtable_to_data(io_pgtable_ops_to_pgtable(x))

#define DART_GRANULE(d)						\
	(sizeof(dart_iopte) << (d)->bits_per_level)
#define DART_PTES_PER_TABLE(d)					\
	(DART_GRANULE(d) >> ilog2(sizeof(dart_iopte)))

#define APPLE_DART_PTE_SUBPAGE_START   GENMASK_ULL(63, 52)
#define APPLE_DART_PTE_SUBPAGE_END     GENMASK_ULL(51, 40)

#define APPLE_DART1_PADDR_MASK	GENMASK_ULL(35, 12)
#define APPLE_DART2_PADDR_MASK	GENMASK_ULL(37, 10)
#define APPLE_DART2_PADDR_SHIFT	(4)

/* Apple DART1 protection bits */
#define APPLE_DART1_PTE_PROT_NO_READ	BIT(8)
#define APPLE_DART1_PTE_PROT_NO_WRITE	BIT(7)
#define APPLE_DART1_PTE_PROT_SP_DIS	BIT(1)

/* Apple DART2 protection bits */
#define APPLE_DART2_PTE_PROT_NO_READ	BIT(3)
#define APPLE_DART2_PTE_PROT_NO_WRITE	BIT(2)
#define APPLE_DART2_PTE_PROT_NO_CACHE	BIT(1)

/* marks PTE as valid */
#define APPLE_DART_PTE_VALID		BIT(0)

/* IOPTE accessors */
#define iopte_deref(pte, d) __va(iopte_to_paddr(pte, d))

struct dart_io_pgtable {
	struct io_pgtable	iop;

	int			tbl_bits;
	int			bits_per_level;

	void			*pgd[DART_MAX_TABLES];
};

typedef u64 dart_iopte;


static dart_iopte paddr_to_iopte(phys_addr_t paddr,
				     struct dart_io_pgtable *data)
{
	dart_iopte pte;

	if (data->iop.fmt == APPLE_DART)
		return paddr & APPLE_DART1_PADDR_MASK;

	/* format is APPLE_DART2 */
	pte = paddr >> APPLE_DART2_PADDR_SHIFT;
	pte &= APPLE_DART2_PADDR_MASK;

	return pte;
}

static phys_addr_t iopte_to_paddr(dart_iopte pte,
				  struct dart_io_pgtable *data)
{
	u64 paddr;

	if (data->iop.fmt == APPLE_DART)
		return pte & APPLE_DART1_PADDR_MASK;

	/* format is APPLE_DART2 */
	paddr = pte & APPLE_DART2_PADDR_MASK;
	paddr <<= APPLE_DART2_PADDR_SHIFT;

	return paddr;
}

static void *__dart_alloc_pages(size_t size, gfp_t gfp,
				    struct io_pgtable_cfg *cfg)
{
	int order = get_order(size);
	struct page *p;

	VM_BUG_ON((gfp & __GFP_HIGHMEM));
	p = alloc_pages(gfp | __GFP_ZERO, order);
	if (!p)
		return NULL;

	return page_address(p);
}

static int dart_init_pte(struct dart_io_pgtable *data,
			     unsigned long iova, phys_addr_t paddr,
			     dart_iopte prot, int num_entries,
			     dart_iopte *ptep)
{
	int i;
	dart_iopte pte = prot;
	size_t sz = data->iop.cfg.pgsize_bitmap;

	for (i = 0; i < num_entries; i++)
		if (ptep[i] & APPLE_DART_PTE_VALID) {
			/* We require an unmap first */
			WARN_ON(ptep[i] & APPLE_DART_PTE_VALID);
			return -EEXIST;
		}

	/* subpage protection: always allow access to the entire page */
	pte |= FIELD_PREP(APPLE_DART_PTE_SUBPAGE_START, 0);
	pte |= FIELD_PREP(APPLE_DART_PTE_SUBPAGE_END, 0xfff);

	pte |= APPLE_DART1_PTE_PROT_SP_DIS;
	pte |= APPLE_DART_PTE_VALID;

	for (i = 0; i < num_entries; i++)
		ptep[i] = pte | paddr_to_iopte(paddr + i * sz, data);

	return 0;
}

static dart_iopte dart_install_table(dart_iopte *table,
					     dart_iopte *ptep,
					     dart_iopte curr,
					     struct dart_io_pgtable *data)
{
	dart_iopte old, new;

	new = paddr_to_iopte(__pa(table), data) | APPLE_DART_PTE_VALID;

	/*
	 * Ensure the table itself is visible before its PTE can be.
	 * Whilst we could get away with cmpxchg64_release below, this
	 * doesn't have any ordering semantics when !CONFIG_SMP.
	 */
	dma_wmb();

	old = cmpxchg64_relaxed(ptep, curr, new);

	return old;
}

static int dart_get_table(struct dart_io_pgtable *data, unsigned long iova)
{
	return (iova >> (3 * data->bits_per_level + ilog2(sizeof(dart_iopte)))) &
		((1 << data->tbl_bits) - 1);
}

static int dart_get_l1_index(struct dart_io_pgtable *data, unsigned long iova)
{

	return (iova >> (2 * data->bits_per_level + ilog2(sizeof(dart_iopte)))) &
		 ((1 << data->bits_per_level) - 1);
}

static int dart_get_l2_index(struct dart_io_pgtable *data, unsigned long iova)
{

	return (iova >> (data->bits_per_level + ilog2(sizeof(dart_iopte)))) &
		 ((1 << data->bits_per_level) - 1);
}

static  dart_iopte *dart_get_l2(struct dart_io_pgtable *data, unsigned long iova)
{
	dart_iopte pte, *ptep;
	int tbl = dart_get_table(data, iova);

	ptep = data->pgd[tbl];
	if (!ptep)
		return NULL;

	ptep += dart_get_l1_index(data, iova);
	pte = READ_ONCE(*ptep);

	/* Valid entry? */
	if (!pte)
		return NULL;

	/* Deref to get level 2 table */
	return iopte_deref(pte, data);
}

static dart_iopte dart_prot_to_pte(struct dart_io_pgtable *data,
					   int prot)
{
	dart_iopte pte = 0;

	if (data->iop.fmt == APPLE_DART) {
		if (!(prot & IOMMU_WRITE))
			pte |= APPLE_DART1_PTE_PROT_NO_WRITE;
		if (!(prot & IOMMU_READ))
			pte |= APPLE_DART1_PTE_PROT_NO_READ;
	}
	if (data->iop.fmt == APPLE_DART2) {
		if (!(prot & IOMMU_WRITE))
			pte |= APPLE_DART2_PTE_PROT_NO_WRITE;
		if (!(prot & IOMMU_READ))
			pte |= APPLE_DART2_PTE_PROT_NO_READ;
		if (!(prot & IOMMU_CACHE))
			pte |= APPLE_DART2_PTE_PROT_NO_CACHE;
	}

	return pte;
}

static int dart_map_pages(struct io_pgtable_ops *ops, unsigned long iova,
			      phys_addr_t paddr, size_t pgsize, size_t pgcount,
			      int iommu_prot, gfp_t gfp, size_t *mapped)
{
	struct dart_io_pgtable *data = io_pgtable_ops_to_data(ops);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	size_t tblsz = DART_GRANULE(data);
	int ret = 0, tbl, num_entries, max_entries, map_idx_start;
	dart_iopte pte, *cptep, *ptep;
	dart_iopte prot;

	if (WARN_ON(pgsize != cfg->pgsize_bitmap))
		return -EINVAL;

	if (WARN_ON(paddr >> cfg->oas))
		return -ERANGE;

	/* If no access, then nothing to do */
	if (!(iommu_prot & (IOMMU_READ | IOMMU_WRITE)))
		return 0;

	tbl = dart_get_table(data, iova);

	ptep = data->pgd[tbl];
	ptep += dart_get_l1_index(data, iova);
	pte = READ_ONCE(*ptep);

	/* no L2 table present */
	if (!pte) {
		cptep = __dart_alloc_pages(tblsz, gfp, cfg);
		if (!cptep)
			return -ENOMEM;

		pte = dart_install_table(cptep, ptep, 0, data);
		if (pte)
			free_pages((unsigned long)cptep, get_order(tblsz));

		/* L2 table is present (now) */
		pte = READ_ONCE(*ptep);
	}

	ptep = iopte_deref(pte, data);

	/* install a leaf entries into L2 table */
	prot = dart_prot_to_pte(data, iommu_prot);
	map_idx_start = dart_get_l2_index(data, iova);
	max_entries = DART_PTES_PER_TABLE(data) - map_idx_start;
	num_entries = min_t(int, pgcount, max_entries);
	ptep += map_idx_start;
	ret = dart_init_pte(data, iova, paddr, prot, num_entries, ptep);
	if (!ret && mapped)
		*mapped += num_entries * pgsize;

	/*
	 * Synchronise all PTE updates for the new mapping before there's
	 * a chance for anything to kick off a table walk for the new iova.
	 */
	wmb();

	return ret;
}

static size_t dart_unmap_pages(struct io_pgtable_ops *ops, unsigned long iova,
				   size_t pgsize, size_t pgcount,
				   struct iommu_iotlb_gather *gather)
{
	struct dart_io_pgtable *data = io_pgtable_ops_to_data(ops);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	int i = 0, num_entries, max_entries, unmap_idx_start;
	dart_iopte pte, *ptep;

	if (WARN_ON(pgsize != cfg->pgsize_bitmap || !pgcount))
		return 0;

	ptep = dart_get_l2(data, iova);

	/* Valid L2 IOPTE pointer? */
	if (WARN_ON(!ptep))
		return 0;

	unmap_idx_start = dart_get_l2_index(data, iova);
	ptep += unmap_idx_start;

	max_entries = DART_PTES_PER_TABLE(data) - unmap_idx_start;
	num_entries = min_t(int, pgcount, max_entries);

	while (i < num_entries) {
		pte = READ_ONCE(*ptep);
		if (WARN_ON(!pte))
			break;

		/* clear pte */
		*ptep = 0;

		if (!iommu_iotlb_gather_queued(gather))
			io_pgtable_tlb_add_page(&data->iop, gather,
						iova + i * pgsize, pgsize);

		ptep++;
		i++;
	}

	return i * pgsize;
}

static phys_addr_t dart_iova_to_phys(struct io_pgtable_ops *ops,
					 unsigned long iova)
{
	struct dart_io_pgtable *data = io_pgtable_ops_to_data(ops);
	dart_iopte pte, *ptep;

	ptep = dart_get_l2(data, iova);

	/* Valid L2 IOPTE pointer? */
	if (!ptep)
		return 0;

	ptep += dart_get_l2_index(data, iova);

	pte = READ_ONCE(*ptep);
	/* Found translation */
	if (pte) {
		iova &= (data->iop.cfg.pgsize_bitmap - 1);
		return iopte_to_paddr(pte, data) | iova;
	}

	/* Ran out of page tables to walk */
	return 0;
}

static struct dart_io_pgtable *
dart_alloc_pgtable(struct io_pgtable_cfg *cfg)
{
	struct dart_io_pgtable *data;
	int tbl_bits, bits_per_level, va_bits, pg_shift;

	pg_shift = __ffs(cfg->pgsize_bitmap);
	bits_per_level = pg_shift - ilog2(sizeof(dart_iopte));

	va_bits = cfg->ias - pg_shift;

	tbl_bits = max_t(int, 0, va_bits - (bits_per_level * DART_LEVELS));
	if ((1 << tbl_bits) > DART_MAX_TABLES)
		return NULL;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return NULL;

	data->tbl_bits = tbl_bits;
	data->bits_per_level = bits_per_level;

	data->iop.ops = (struct io_pgtable_ops) {
		.map_pages	= dart_map_pages,
		.unmap_pages	= dart_unmap_pages,
		.iova_to_phys	= dart_iova_to_phys,
	};

	return data;
}

static struct io_pgtable *
apple_dart_alloc_pgtable(struct io_pgtable_cfg *cfg, void *cookie)
{
	struct dart_io_pgtable *data;
	int i;

	if (!cfg->coherent_walk)
		return NULL;

	if (cfg->oas != 36 && cfg->oas != 42)
		return NULL;

	if (cfg->ias > cfg->oas)
		return NULL;

	if (!(cfg->pgsize_bitmap == SZ_4K || cfg->pgsize_bitmap == SZ_16K))
		return NULL;

	data = dart_alloc_pgtable(cfg);
	if (!data)
		return NULL;

	cfg->apple_dart_cfg.n_ttbrs = 1 << data->tbl_bits;

	for (i = 0; i < cfg->apple_dart_cfg.n_ttbrs; ++i) {
		data->pgd[i] = __dart_alloc_pages(DART_GRANULE(data), GFP_KERNEL,
					   cfg);
		if (!data->pgd[i])
			goto out_free_data;
		cfg->apple_dart_cfg.ttbr[i] = virt_to_phys(data->pgd[i]);
	}

	return &data->iop;

out_free_data:
	while (--i >= 0)
		free_pages((unsigned long)data->pgd[i],
			   get_order(DART_GRANULE(data)));
	kfree(data);
	return NULL;
}

static void apple_dart_free_pgtable(struct io_pgtable *iop)
{
	struct dart_io_pgtable *data = io_pgtable_to_data(iop);
	dart_iopte *ptep, *end;
	int i;

	for (i = 0; i < (1 << data->tbl_bits) && data->pgd[i]; ++i) {
		ptep = data->pgd[i];
		end = (void *)ptep + DART_GRANULE(data);

		while (ptep != end) {
			dart_iopte pte = *ptep++;

			if (pte) {
				unsigned long page =
					(unsigned long)iopte_deref(pte, data);

				free_pages(page, get_order(DART_GRANULE(data)));
			}
		}
		free_pages((unsigned long)data->pgd[i],
			   get_order(DART_GRANULE(data)));
	}

	kfree(data);
}

struct io_pgtable_init_fns io_pgtable_apple_dart_init_fns = {
	.alloc	= apple_dart_alloc_pgtable,
	.free	= apple_dart_free_pgtable,
};
