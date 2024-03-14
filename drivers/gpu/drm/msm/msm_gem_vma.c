// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2016 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include "msm_drv.h"
#include "msm_fence.h"
#include "msm_gem.h"
#include "msm_mmu.h"

static void
msm_gem_address_space_destroy(struct kref *kref)
{
	struct msm_gem_address_space *aspace = container_of(kref,
			struct msm_gem_address_space, kref);

	drm_mm_takedown(&aspace->mm);
	if (aspace->mmu)
		aspace->mmu->funcs->destroy(aspace->mmu);
	put_pid(aspace->pid);
	kfree(aspace);
}


void msm_gem_address_space_put(struct msm_gem_address_space *aspace)
{
	if (aspace)
		kref_put(&aspace->kref, msm_gem_address_space_destroy);
}

struct msm_gem_address_space *
msm_gem_address_space_get(struct msm_gem_address_space *aspace)
{
	if (!IS_ERR_OR_NULL(aspace))
		kref_get(&aspace->kref);

	return aspace;
}

/* Actually unmap memory for the vma */
void msm_gem_vma_purge(struct msm_gem_vma *vma)
{
	struct msm_gem_address_space *aspace = vma->aspace;
	unsigned size = vma->node.size;

	/* Don't do anything if the memory isn't mapped */
	if (!vma->mapped)
		return;

	aspace->mmu->funcs->unmap(aspace->mmu, vma->iova, size);

	vma->mapped = false;
}

/* Map and pin vma: */
int
msm_gem_vma_map(struct msm_gem_vma *vma, int prot,
		struct sg_table *sgt, int size)
{
	struct msm_gem_address_space *aspace = vma->aspace;
	int ret;

	if (GEM_WARN_ON(!vma->iova))
		return -EINVAL;

	if (vma->mapped)
		return 0;

	vma->mapped = true;

	if (!aspace)
		return 0;

	/*
	 * NOTE: iommu/io-pgtable can allocate pages, so we cannot hold
	 * a lock across map/unmap which is also used in the job_run()
	 * path, as this can cause deadlock in job_run() vs shrinker/
	 * reclaim.
	 *
	 * Revisit this if we can come up with a scheme to pre-alloc pages
	 * for the pgtable in map/unmap ops.
	 */
	ret = aspace->mmu->funcs->map(aspace->mmu, vma->iova, sgt, size, prot);

	if (ret) {
		vma->mapped = false;
	}

	return ret;
}

/* Close an iova.  Warn if it is still in use */
void msm_gem_vma_close(struct msm_gem_vma *vma)
{
	struct msm_gem_address_space *aspace = vma->aspace;

	GEM_WARN_ON(vma->mapped);

	spin_lock(&aspace->lock);
	if (vma->iova)
		drm_mm_remove_node(&vma->node);
	spin_unlock(&aspace->lock);

	vma->iova = 0;

	msm_gem_address_space_put(aspace);
}

struct msm_gem_vma *msm_gem_vma_new(struct msm_gem_address_space *aspace)
{
	struct msm_gem_vma *vma;

	vma = kzalloc(sizeof(*vma), GFP_KERNEL);
	if (!vma)
		return NULL;

	vma->aspace = aspace;

	return vma;
}

/* Initialize a new vma and allocate an iova for it */
int msm_gem_vma_init(struct msm_gem_vma *vma, int size,
		u64 range_start, u64 range_end)
{
	struct msm_gem_address_space *aspace = vma->aspace;
	int ret;

	if (GEM_WARN_ON(!aspace))
		return -EINVAL;

	if (GEM_WARN_ON(vma->iova))
		return -EBUSY;

	spin_lock(&aspace->lock);
	ret = drm_mm_insert_node_in_range(&aspace->mm, &vma->node,
					  size, PAGE_SIZE, 0,
					  range_start, range_end, 0);
	spin_unlock(&aspace->lock);

	if (ret)
		return ret;

	vma->iova = vma->node.start;
	vma->mapped = false;

	kref_get(&aspace->kref);

	return 0;
}

struct msm_gem_address_space *
msm_gem_address_space_create(struct msm_mmu *mmu, const char *name,
		u64 va_start, u64 size)
{
	struct msm_gem_address_space *aspace;

	if (IS_ERR(mmu))
		return ERR_CAST(mmu);

	aspace = kzalloc(sizeof(*aspace), GFP_KERNEL);
	if (!aspace)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&aspace->lock);
	aspace->name = name;
	aspace->mmu = mmu;
	aspace->va_start = va_start;
	aspace->va_size  = size;

	drm_mm_init(&aspace->mm, va_start, size);

	kref_init(&aspace->kref);

	return aspace;
}
