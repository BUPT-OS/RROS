/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2014-2015 ARM Ltd.
 */
#ifndef __DMA_IOMMU_H
#define __DMA_IOMMU_H

#include <linux/iommu.h>

#ifdef CONFIG_IOMMU_DMA

int iommu_get_dma_cookie(struct iommu_domain *domain);
void iommu_put_dma_cookie(struct iommu_domain *domain);

int iommu_dma_init_fq(struct iommu_domain *domain);

void iommu_dma_get_resv_regions(struct device *dev, struct list_head *list);

extern bool iommu_dma_forcedac;
static inline void iommu_dma_set_pci_32bit_workaround(struct device *dev)
{
	dev->iommu->pci_32bit_workaround = !iommu_dma_forcedac;
}

#else /* CONFIG_IOMMU_DMA */

static inline int iommu_dma_init_fq(struct iommu_domain *domain)
{
	return -EINVAL;
}

static inline int iommu_get_dma_cookie(struct iommu_domain *domain)
{
	return -ENODEV;
}

static inline void iommu_put_dma_cookie(struct iommu_domain *domain)
{
}

static inline void iommu_dma_get_resv_regions(struct device *dev, struct list_head *list)
{
}

static inline void iommu_dma_set_pci_32bit_workaround(struct device *dev)
{
}

#endif	/* CONFIG_IOMMU_DMA */
#endif	/* __DMA_IOMMU_H */
