/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * VFIO API definition
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 */
#ifndef VFIO_H
#define VFIO_H


#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/workqueue.h>
#include <linux/poll.h>
#include <linux/cdev.h>
#include <uapi/linux/vfio.h>
#include <linux/iova_bitmap.h>

struct kvm;
struct iommufd_ctx;
struct iommufd_device;
struct iommufd_access;

/*
 * VFIO devices can be placed in a set, this allows all devices to share this
 * structure and the VFIO core will provide a lock that is held around
 * open_device()/close_device() for all devices in the set.
 */
struct vfio_device_set {
	void *set_id;
	struct mutex lock;
	struct list_head device_list;
	unsigned int device_count;
};

struct vfio_device {
	struct device *dev;
	const struct vfio_device_ops *ops;
	/*
	 * mig_ops/log_ops is a static property of the vfio_device which must
	 * be set prior to registering the vfio_device.
	 */
	const struct vfio_migration_ops *mig_ops;
	const struct vfio_log_ops *log_ops;
#if IS_ENABLED(CONFIG_VFIO_GROUP)
	struct vfio_group *group;
	struct list_head group_next;
	struct list_head iommu_entry;
#endif
	struct vfio_device_set *dev_set;
	struct list_head dev_set_list;
	unsigned int migration_flags;
	struct kvm *kvm;

	/* Members below here are private, not for driver use */
	unsigned int index;
	struct device device;	/* device.kref covers object life circle */
#if IS_ENABLED(CONFIG_VFIO_DEVICE_CDEV)
	struct cdev cdev;
#endif
	refcount_t refcount;	/* user count on registered device*/
	unsigned int open_count;
	struct completion comp;
	struct iommufd_access *iommufd_access;
	void (*put_kvm)(struct kvm *kvm);
#if IS_ENABLED(CONFIG_IOMMUFD)
	struct iommufd_device *iommufd_device;
	u8 iommufd_attached:1;
#endif
	u8 cdev_opened:1;
};

/**
 * struct vfio_device_ops - VFIO bus driver device callbacks
 *
 * @name: Name of the device driver.
 * @init: initialize private fields in device structure
 * @release: Reclaim private fields in device structure
 * @bind_iommufd: Called when binding the device to an iommufd
 * @unbind_iommufd: Opposite of bind_iommufd
 * @attach_ioas: Called when attaching device to an IOAS/HWPT managed by the
 *		 bound iommufd. Undo in unbind_iommufd if @detach_ioas is not
 *		 called.
 * @detach_ioas: Opposite of attach_ioas
 * @open_device: Called when the first file descriptor is opened for this device
 * @close_device: Opposite of open_device
 * @read: Perform read(2) on device file descriptor
 * @write: Perform write(2) on device file descriptor
 * @ioctl: Perform ioctl(2) on device file descriptor, supporting VFIO_DEVICE_*
 *         operations documented below
 * @mmap: Perform mmap(2) on a region of the device file descriptor
 * @request: Request for the bus driver to release the device
 * @match: Optional device name match callback (return: 0 for no-match, >0 for
 *         match, -errno for abort (ex. match with insufficient or incorrect
 *         additional args)
 * @dma_unmap: Called when userspace unmaps IOVA from the container
 *             this device is attached to.
 * @device_feature: Optional, fill in the VFIO_DEVICE_FEATURE ioctl
 */
struct vfio_device_ops {
	char	*name;
	int	(*init)(struct vfio_device *vdev);
	void	(*release)(struct vfio_device *vdev);
	int	(*bind_iommufd)(struct vfio_device *vdev,
				struct iommufd_ctx *ictx, u32 *out_device_id);
	void	(*unbind_iommufd)(struct vfio_device *vdev);
	int	(*attach_ioas)(struct vfio_device *vdev, u32 *pt_id);
	void	(*detach_ioas)(struct vfio_device *vdev);
	int	(*open_device)(struct vfio_device *vdev);
	void	(*close_device)(struct vfio_device *vdev);
	ssize_t	(*read)(struct vfio_device *vdev, char __user *buf,
			size_t count, loff_t *ppos);
	ssize_t	(*write)(struct vfio_device *vdev, const char __user *buf,
			 size_t count, loff_t *size);
	long	(*ioctl)(struct vfio_device *vdev, unsigned int cmd,
			 unsigned long arg);
	int	(*mmap)(struct vfio_device *vdev, struct vm_area_struct *vma);
	void	(*request)(struct vfio_device *vdev, unsigned int count);
	int	(*match)(struct vfio_device *vdev, char *buf);
	void	(*dma_unmap)(struct vfio_device *vdev, u64 iova, u64 length);
	int	(*device_feature)(struct vfio_device *device, u32 flags,
				  void __user *arg, size_t argsz);
};

#if IS_ENABLED(CONFIG_IOMMUFD)
struct iommufd_ctx *vfio_iommufd_device_ictx(struct vfio_device *vdev);
int vfio_iommufd_get_dev_id(struct vfio_device *vdev, struct iommufd_ctx *ictx);
int vfio_iommufd_physical_bind(struct vfio_device *vdev,
			       struct iommufd_ctx *ictx, u32 *out_device_id);
void vfio_iommufd_physical_unbind(struct vfio_device *vdev);
int vfio_iommufd_physical_attach_ioas(struct vfio_device *vdev, u32 *pt_id);
void vfio_iommufd_physical_detach_ioas(struct vfio_device *vdev);
int vfio_iommufd_emulated_bind(struct vfio_device *vdev,
			       struct iommufd_ctx *ictx, u32 *out_device_id);
void vfio_iommufd_emulated_unbind(struct vfio_device *vdev);
int vfio_iommufd_emulated_attach_ioas(struct vfio_device *vdev, u32 *pt_id);
void vfio_iommufd_emulated_detach_ioas(struct vfio_device *vdev);
#else
static inline struct iommufd_ctx *
vfio_iommufd_device_ictx(struct vfio_device *vdev)
{
	return NULL;
}

static inline int
vfio_iommufd_get_dev_id(struct vfio_device *vdev, struct iommufd_ctx *ictx)
{
	return VFIO_PCI_DEVID_NOT_OWNED;
}

#define vfio_iommufd_physical_bind                                      \
	((int (*)(struct vfio_device *vdev, struct iommufd_ctx *ictx,   \
		  u32 *out_device_id)) NULL)
#define vfio_iommufd_physical_unbind \
	((void (*)(struct vfio_device *vdev)) NULL)
#define vfio_iommufd_physical_attach_ioas \
	((int (*)(struct vfio_device *vdev, u32 *pt_id)) NULL)
#define vfio_iommufd_physical_detach_ioas \
	((void (*)(struct vfio_device *vdev)) NULL)
#define vfio_iommufd_emulated_bind                                      \
	((int (*)(struct vfio_device *vdev, struct iommufd_ctx *ictx,   \
		  u32 *out_device_id)) NULL)
#define vfio_iommufd_emulated_unbind \
	((void (*)(struct vfio_device *vdev)) NULL)
#define vfio_iommufd_emulated_attach_ioas \
	((int (*)(struct vfio_device *vdev, u32 *pt_id)) NULL)
#define vfio_iommufd_emulated_detach_ioas \
	((void (*)(struct vfio_device *vdev)) NULL)
#endif

static inline bool vfio_device_cdev_opened(struct vfio_device *device)
{
	return device->cdev_opened;
}

/**
 * struct vfio_migration_ops - VFIO bus device driver migration callbacks
 *
 * @migration_set_state: Optional callback to change the migration state for
 *         devices that support migration. It's mandatory for
 *         VFIO_DEVICE_FEATURE_MIGRATION migration support.
 *         The returned FD is used for data transfer according to the FSM
 *         definition. The driver is responsible to ensure that FD reaches end
 *         of stream or error whenever the migration FSM leaves a data transfer
 *         state or before close_device() returns.
 * @migration_get_state: Optional callback to get the migration state for
 *         devices that support migration. It's mandatory for
 *         VFIO_DEVICE_FEATURE_MIGRATION migration support.
 * @migration_get_data_size: Optional callback to get the estimated data
 *          length that will be required to complete stop copy. It's mandatory for
 *          VFIO_DEVICE_FEATURE_MIGRATION migration support.
 */
struct vfio_migration_ops {
	struct file *(*migration_set_state)(
		struct vfio_device *device,
		enum vfio_device_mig_state new_state);
	int (*migration_get_state)(struct vfio_device *device,
				   enum vfio_device_mig_state *curr_state);
	int (*migration_get_data_size)(struct vfio_device *device,
				       unsigned long *stop_copy_length);
};

/**
 * struct vfio_log_ops - VFIO bus device driver logging callbacks
 *
 * @log_start: Optional callback to ask the device start DMA logging.
 * @log_stop: Optional callback to ask the device stop DMA logging.
 * @log_read_and_clear: Optional callback to ask the device read
 *         and clear the dirty DMAs in some given range.
 *
 * The vfio core implementation of the DEVICE_FEATURE_DMA_LOGGING_ set
 * of features does not track logging state relative to the device,
 * therefore the device implementation of vfio_log_ops must handle
 * arbitrary user requests. This includes rejecting subsequent calls
 * to log_start without an intervening log_stop, as well as graceful
 * handling of log_stop and log_read_and_clear from invalid states.
 */
struct vfio_log_ops {
	int (*log_start)(struct vfio_device *device,
		struct rb_root_cached *ranges, u32 nnodes, u64 *page_size);
	int (*log_stop)(struct vfio_device *device);
	int (*log_read_and_clear)(struct vfio_device *device,
		unsigned long iova, unsigned long length,
		struct iova_bitmap *dirty);
};

/**
 * vfio_check_feature - Validate user input for the VFIO_DEVICE_FEATURE ioctl
 * @flags: Arg from the device_feature op
 * @argsz: Arg from the device_feature op
 * @supported_ops: Combination of VFIO_DEVICE_FEATURE_GET and SET the driver
 *                 supports
 * @minsz: Minimum data size the driver accepts
 *
 * For use in a driver's device_feature op. Checks that the inputs to the
 * VFIO_DEVICE_FEATURE ioctl are correct for the driver's feature. Returns 1 if
 * the driver should execute the get or set, otherwise the relevant
 * value should be returned.
 */
static inline int vfio_check_feature(u32 flags, size_t argsz, u32 supported_ops,
				    size_t minsz)
{
	if ((flags & (VFIO_DEVICE_FEATURE_GET | VFIO_DEVICE_FEATURE_SET)) &
	    ~supported_ops)
		return -EINVAL;
	if (flags & VFIO_DEVICE_FEATURE_PROBE)
		return 0;
	/* Without PROBE one of GET or SET must be requested */
	if (!(flags & (VFIO_DEVICE_FEATURE_GET | VFIO_DEVICE_FEATURE_SET)))
		return -EINVAL;
	if (argsz < minsz)
		return -EINVAL;
	return 1;
}

struct vfio_device *_vfio_alloc_device(size_t size, struct device *dev,
				       const struct vfio_device_ops *ops);
#define vfio_alloc_device(dev_struct, member, dev, ops)				\
	container_of(_vfio_alloc_device(sizeof(struct dev_struct) +		\
					BUILD_BUG_ON_ZERO(offsetof(		\
						struct dev_struct, member)),	\
					dev, ops),				\
		     struct dev_struct, member)

static inline void vfio_put_device(struct vfio_device *device)
{
	put_device(&device->device);
}

int vfio_register_group_dev(struct vfio_device *device);
int vfio_register_emulated_iommu_dev(struct vfio_device *device);
void vfio_unregister_group_dev(struct vfio_device *device);

int vfio_assign_device_set(struct vfio_device *device, void *set_id);
unsigned int vfio_device_set_open_count(struct vfio_device_set *dev_set);
struct vfio_device *
vfio_find_device_in_devset(struct vfio_device_set *dev_set,
			   struct device *dev);

int vfio_mig_get_next_state(struct vfio_device *device,
			    enum vfio_device_mig_state cur_fsm,
			    enum vfio_device_mig_state new_fsm,
			    enum vfio_device_mig_state *next_fsm);

void vfio_combine_iova_ranges(struct rb_root_cached *root, u32 cur_nodes,
			      u32 req_nodes);

/*
 * External user API
 */
#if IS_ENABLED(CONFIG_VFIO_GROUP)
struct iommu_group *vfio_file_iommu_group(struct file *file);
bool vfio_file_is_group(struct file *file);
bool vfio_file_has_dev(struct file *file, struct vfio_device *device);
#else
static inline struct iommu_group *vfio_file_iommu_group(struct file *file)
{
	return NULL;
}

static inline bool vfio_file_is_group(struct file *file)
{
	return false;
}

static inline bool vfio_file_has_dev(struct file *file, struct vfio_device *device)
{
	return false;
}
#endif
bool vfio_file_is_valid(struct file *file);
bool vfio_file_enforced_coherent(struct file *file);
void vfio_file_set_kvm(struct file *file, struct kvm *kvm);

#define VFIO_PIN_PAGES_MAX_ENTRIES	(PAGE_SIZE/sizeof(unsigned long))

int vfio_pin_pages(struct vfio_device *device, dma_addr_t iova,
		   int npage, int prot, struct page **pages);
void vfio_unpin_pages(struct vfio_device *device, dma_addr_t iova, int npage);
int vfio_dma_rw(struct vfio_device *device, dma_addr_t iova,
		void *data, size_t len, bool write);

/*
 * Sub-module helpers
 */
struct vfio_info_cap {
	struct vfio_info_cap_header *buf;
	size_t size;
};
struct vfio_info_cap_header *vfio_info_cap_add(struct vfio_info_cap *caps,
					       size_t size, u16 id,
					       u16 version);
void vfio_info_cap_shift(struct vfio_info_cap *caps, size_t offset);

int vfio_info_add_capability(struct vfio_info_cap *caps,
			     struct vfio_info_cap_header *cap, size_t size);

int vfio_set_irqs_validate_and_prepare(struct vfio_irq_set *hdr,
				       int num_irqs, int max_irq_type,
				       size_t *data_size);

/*
 * IRQfd - generic
 */
struct virqfd {
	void			*opaque;
	struct eventfd_ctx	*eventfd;
	int			(*handler)(void *, void *);
	void			(*thread)(void *, void *);
	void			*data;
	struct work_struct	inject;
	wait_queue_entry_t		wait;
	poll_table		pt;
	struct work_struct	shutdown;
	struct virqfd		**pvirqfd;
};

int vfio_virqfd_enable(void *opaque, int (*handler)(void *, void *),
		       void (*thread)(void *, void *), void *data,
		       struct virqfd **pvirqfd, int fd);
void vfio_virqfd_disable(struct virqfd **pvirqfd);

#endif /* VFIO_H */
