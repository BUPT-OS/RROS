/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SVA library for IOMMU drivers
 */
#ifndef _IOMMU_SVA_H
#define _IOMMU_SVA_H

#include <linux/mm_types.h>

/* I/O Page fault */
struct device;
struct iommu_fault;
struct iopf_queue;

#ifdef CONFIG_IOMMU_SVA
int iommu_queue_iopf(struct iommu_fault *fault, void *cookie);

int iopf_queue_add_device(struct iopf_queue *queue, struct device *dev);
int iopf_queue_remove_device(struct iopf_queue *queue,
			     struct device *dev);
int iopf_queue_flush_dev(struct device *dev);
struct iopf_queue *iopf_queue_alloc(const char *name);
void iopf_queue_free(struct iopf_queue *queue);
int iopf_queue_discard_partial(struct iopf_queue *queue);
enum iommu_page_response_code
iommu_sva_handle_iopf(struct iommu_fault *fault, void *data);

#else /* CONFIG_IOMMU_SVA */
static inline int iommu_queue_iopf(struct iommu_fault *fault, void *cookie)
{
	return -ENODEV;
}

static inline int iopf_queue_add_device(struct iopf_queue *queue,
					struct device *dev)
{
	return -ENODEV;
}

static inline int iopf_queue_remove_device(struct iopf_queue *queue,
					   struct device *dev)
{
	return -ENODEV;
}

static inline int iopf_queue_flush_dev(struct device *dev)
{
	return -ENODEV;
}

static inline struct iopf_queue *iopf_queue_alloc(const char *name)
{
	return NULL;
}

static inline void iopf_queue_free(struct iopf_queue *queue)
{
}

static inline int iopf_queue_discard_partial(struct iopf_queue *queue)
{
	return -ENODEV;
}

static inline enum iommu_page_response_code
iommu_sva_handle_iopf(struct iommu_fault *fault, void *data)
{
	return IOMMU_PAGE_RESP_INVALID;
}
#endif /* CONFIG_IOMMU_SVA */
#endif /* _IOMMU_SVA_H */
