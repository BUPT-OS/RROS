// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#include <linux/device.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>
#include <linux/sched/mm.h>
#include <linux/anon_inodes.h>

#include "cmd.h"

/* Device specification max LOAD size */
#define MAX_LOAD_SIZE (BIT_ULL(__mlx5_bit_sz(load_vhca_state_in, size)) - 1)

static struct mlx5vf_pci_core_device *mlx5vf_drvdata(struct pci_dev *pdev)
{
	struct vfio_pci_core_device *core_device = dev_get_drvdata(&pdev->dev);

	return container_of(core_device, struct mlx5vf_pci_core_device,
			    core_device);
}

struct page *
mlx5vf_get_migration_page(struct mlx5_vhca_data_buffer *buf,
			  unsigned long offset)
{
	unsigned long cur_offset = 0;
	struct scatterlist *sg;
	unsigned int i;

	/* All accesses are sequential */
	if (offset < buf->last_offset || !buf->last_offset_sg) {
		buf->last_offset = 0;
		buf->last_offset_sg = buf->table.sgt.sgl;
		buf->sg_last_entry = 0;
	}

	cur_offset = buf->last_offset;

	for_each_sg(buf->last_offset_sg, sg,
			buf->table.sgt.orig_nents - buf->sg_last_entry, i) {
		if (offset < sg->length + cur_offset) {
			buf->last_offset_sg = sg;
			buf->sg_last_entry += i;
			buf->last_offset = cur_offset;
			return nth_page(sg_page(sg),
					(offset - cur_offset) / PAGE_SIZE);
		}
		cur_offset += sg->length;
	}
	return NULL;
}

int mlx5vf_add_migration_pages(struct mlx5_vhca_data_buffer *buf,
			       unsigned int npages)
{
	unsigned int to_alloc = npages;
	struct page **page_list;
	unsigned long filled;
	unsigned int to_fill;
	int ret;

	to_fill = min_t(unsigned int, npages, PAGE_SIZE / sizeof(*page_list));
	page_list = kvzalloc(to_fill * sizeof(*page_list), GFP_KERNEL_ACCOUNT);
	if (!page_list)
		return -ENOMEM;

	do {
		filled = alloc_pages_bulk_array(GFP_KERNEL_ACCOUNT, to_fill,
						page_list);
		if (!filled) {
			ret = -ENOMEM;
			goto err;
		}
		to_alloc -= filled;
		ret = sg_alloc_append_table_from_pages(
			&buf->table, page_list, filled, 0,
			filled << PAGE_SHIFT, UINT_MAX, SG_MAX_SINGLE_ALLOC,
			GFP_KERNEL_ACCOUNT);

		if (ret)
			goto err;
		buf->allocated_length += filled * PAGE_SIZE;
		/* clean input for another bulk allocation */
		memset(page_list, 0, filled * sizeof(*page_list));
		to_fill = min_t(unsigned int, to_alloc,
				PAGE_SIZE / sizeof(*page_list));
	} while (to_alloc > 0);

	kvfree(page_list);
	return 0;

err:
	kvfree(page_list);
	return ret;
}

static void mlx5vf_disable_fd(struct mlx5_vf_migration_file *migf)
{
	mutex_lock(&migf->lock);
	migf->state = MLX5_MIGF_STATE_ERROR;
	migf->filp->f_pos = 0;
	mutex_unlock(&migf->lock);
}

static int mlx5vf_release_file(struct inode *inode, struct file *filp)
{
	struct mlx5_vf_migration_file *migf = filp->private_data;

	mlx5vf_disable_fd(migf);
	mutex_destroy(&migf->lock);
	kfree(migf);
	return 0;
}

static struct mlx5_vhca_data_buffer *
mlx5vf_get_data_buff_from_pos(struct mlx5_vf_migration_file *migf, loff_t pos,
			      bool *end_of_data)
{
	struct mlx5_vhca_data_buffer *buf;
	bool found = false;

	*end_of_data = false;
	spin_lock_irq(&migf->list_lock);
	if (list_empty(&migf->buf_list)) {
		*end_of_data = true;
		goto end;
	}

	buf = list_first_entry(&migf->buf_list, struct mlx5_vhca_data_buffer,
			       buf_elm);
	if (pos >= buf->start_pos &&
	    pos < buf->start_pos + buf->length) {
		found = true;
		goto end;
	}

	/*
	 * As we use a stream based FD we may expect having the data always
	 * on first chunk
	 */
	migf->state = MLX5_MIGF_STATE_ERROR;

end:
	spin_unlock_irq(&migf->list_lock);
	return found ? buf : NULL;
}

static ssize_t mlx5vf_buf_read(struct mlx5_vhca_data_buffer *vhca_buf,
			       char __user **buf, size_t *len, loff_t *pos)
{
	unsigned long offset;
	ssize_t done = 0;
	size_t copy_len;

	copy_len = min_t(size_t,
			 vhca_buf->start_pos + vhca_buf->length - *pos, *len);
	while (copy_len) {
		size_t page_offset;
		struct page *page;
		size_t page_len;
		u8 *from_buff;
		int ret;

		offset = *pos - vhca_buf->start_pos;
		page_offset = offset % PAGE_SIZE;
		offset -= page_offset;
		page = mlx5vf_get_migration_page(vhca_buf, offset);
		if (!page)
			return -EINVAL;
		page_len = min_t(size_t, copy_len, PAGE_SIZE - page_offset);
		from_buff = kmap_local_page(page);
		ret = copy_to_user(*buf, from_buff + page_offset, page_len);
		kunmap_local(from_buff);
		if (ret)
			return -EFAULT;
		*pos += page_len;
		*len -= page_len;
		*buf += page_len;
		done += page_len;
		copy_len -= page_len;
	}

	if (*pos >= vhca_buf->start_pos + vhca_buf->length) {
		spin_lock_irq(&vhca_buf->migf->list_lock);
		list_del_init(&vhca_buf->buf_elm);
		list_add_tail(&vhca_buf->buf_elm, &vhca_buf->migf->avail_list);
		spin_unlock_irq(&vhca_buf->migf->list_lock);
	}

	return done;
}

static ssize_t mlx5vf_save_read(struct file *filp, char __user *buf, size_t len,
			       loff_t *pos)
{
	struct mlx5_vf_migration_file *migf = filp->private_data;
	struct mlx5_vhca_data_buffer *vhca_buf;
	bool first_loop_call = true;
	bool end_of_data;
	ssize_t done = 0;

	if (pos)
		return -ESPIPE;
	pos = &filp->f_pos;

	if (!(filp->f_flags & O_NONBLOCK)) {
		if (wait_event_interruptible(migf->poll_wait,
				!list_empty(&migf->buf_list) ||
				migf->state == MLX5_MIGF_STATE_ERROR ||
				migf->state == MLX5_MIGF_STATE_PRE_COPY_ERROR ||
				migf->state == MLX5_MIGF_STATE_PRE_COPY ||
				migf->state == MLX5_MIGF_STATE_COMPLETE))
			return -ERESTARTSYS;
	}

	mutex_lock(&migf->lock);
	if (migf->state == MLX5_MIGF_STATE_ERROR) {
		done = -ENODEV;
		goto out_unlock;
	}

	while (len) {
		ssize_t count;

		vhca_buf = mlx5vf_get_data_buff_from_pos(migf, *pos,
							 &end_of_data);
		if (first_loop_call) {
			first_loop_call = false;
			/* Temporary end of file as part of PRE_COPY */
			if (end_of_data && (migf->state == MLX5_MIGF_STATE_PRE_COPY ||
				migf->state == MLX5_MIGF_STATE_PRE_COPY_ERROR)) {
				done = -ENOMSG;
				goto out_unlock;
			}

			if (end_of_data && migf->state != MLX5_MIGF_STATE_COMPLETE) {
				if (filp->f_flags & O_NONBLOCK) {
					done = -EAGAIN;
					goto out_unlock;
				}
			}
		}

		if (end_of_data)
			goto out_unlock;

		if (!vhca_buf) {
			done = -EINVAL;
			goto out_unlock;
		}

		count = mlx5vf_buf_read(vhca_buf, &buf, &len, pos);
		if (count < 0) {
			done = count;
			goto out_unlock;
		}
		done += count;
	}

out_unlock:
	mutex_unlock(&migf->lock);
	return done;
}

static __poll_t mlx5vf_save_poll(struct file *filp,
				 struct poll_table_struct *wait)
{
	struct mlx5_vf_migration_file *migf = filp->private_data;
	__poll_t pollflags = 0;

	poll_wait(filp, &migf->poll_wait, wait);

	mutex_lock(&migf->lock);
	if (migf->state == MLX5_MIGF_STATE_ERROR)
		pollflags = EPOLLIN | EPOLLRDNORM | EPOLLRDHUP;
	else if (!list_empty(&migf->buf_list) ||
		 migf->state == MLX5_MIGF_STATE_COMPLETE)
		pollflags = EPOLLIN | EPOLLRDNORM;
	mutex_unlock(&migf->lock);

	return pollflags;
}

/*
 * FD is exposed and user can use it after receiving an error.
 * Mark migf in error, and wake the user.
 */
static void mlx5vf_mark_err(struct mlx5_vf_migration_file *migf)
{
	migf->state = MLX5_MIGF_STATE_ERROR;
	wake_up_interruptible(&migf->poll_wait);
}

static int mlx5vf_add_stop_copy_header(struct mlx5_vf_migration_file *migf)
{
	size_t size = sizeof(struct mlx5_vf_migration_header) +
		sizeof(struct mlx5_vf_migration_tag_stop_copy_data);
	struct mlx5_vf_migration_tag_stop_copy_data data = {};
	struct mlx5_vhca_data_buffer *header_buf = NULL;
	struct mlx5_vf_migration_header header = {};
	unsigned long flags;
	struct page *page;
	u8 *to_buff;
	int ret;

	header_buf = mlx5vf_get_data_buffer(migf, size, DMA_NONE);
	if (IS_ERR(header_buf))
		return PTR_ERR(header_buf);

	header.record_size = cpu_to_le64(sizeof(data));
	header.flags = cpu_to_le32(MLX5_MIGF_HEADER_FLAGS_TAG_OPTIONAL);
	header.tag = cpu_to_le32(MLX5_MIGF_HEADER_TAG_STOP_COPY_SIZE);
	page = mlx5vf_get_migration_page(header_buf, 0);
	if (!page) {
		ret = -EINVAL;
		goto err;
	}
	to_buff = kmap_local_page(page);
	memcpy(to_buff, &header, sizeof(header));
	header_buf->length = sizeof(header);
	data.stop_copy_size = cpu_to_le64(migf->buf->allocated_length);
	memcpy(to_buff + sizeof(header), &data, sizeof(data));
	header_buf->length += sizeof(data);
	kunmap_local(to_buff);
	header_buf->start_pos = header_buf->migf->max_pos;
	migf->max_pos += header_buf->length;
	spin_lock_irqsave(&migf->list_lock, flags);
	list_add_tail(&header_buf->buf_elm, &migf->buf_list);
	spin_unlock_irqrestore(&migf->list_lock, flags);
	migf->pre_copy_initial_bytes = size;
	return 0;
err:
	mlx5vf_put_data_buffer(header_buf);
	return ret;
}

static int mlx5vf_prep_stop_copy(struct mlx5_vf_migration_file *migf,
				 size_t state_size)
{
	struct mlx5_vhca_data_buffer *buf;
	size_t inc_state_size;
	int ret;

	/* let's be ready for stop_copy size that might grow by 10 percents */
	if (check_add_overflow(state_size, state_size / 10, &inc_state_size))
		inc_state_size = state_size;

	buf = mlx5vf_get_data_buffer(migf, inc_state_size, DMA_FROM_DEVICE);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	migf->buf = buf;
	buf = mlx5vf_get_data_buffer(migf,
			sizeof(struct mlx5_vf_migration_header), DMA_NONE);
	if (IS_ERR(buf)) {
		ret = PTR_ERR(buf);
		goto err;
	}

	migf->buf_header = buf;
	ret = mlx5vf_add_stop_copy_header(migf);
	if (ret)
		goto err_header;
	return 0;

err_header:
	mlx5vf_put_data_buffer(migf->buf_header);
	migf->buf_header = NULL;
err:
	mlx5vf_put_data_buffer(migf->buf);
	migf->buf = NULL;
	return ret;
}

static long mlx5vf_precopy_ioctl(struct file *filp, unsigned int cmd,
				 unsigned long arg)
{
	struct mlx5_vf_migration_file *migf = filp->private_data;
	struct mlx5vf_pci_core_device *mvdev = migf->mvdev;
	struct mlx5_vhca_data_buffer *buf;
	struct vfio_precopy_info info = {};
	loff_t *pos = &filp->f_pos;
	unsigned long minsz;
	size_t inc_length = 0;
	bool end_of_data = false;
	int ret;

	if (cmd != VFIO_MIG_GET_PRECOPY_INFO)
		return -ENOTTY;

	minsz = offsetofend(struct vfio_precopy_info, dirty_bytes);

	if (copy_from_user(&info, (void __user *)arg, minsz))
		return -EFAULT;

	if (info.argsz < minsz)
		return -EINVAL;

	mutex_lock(&mvdev->state_mutex);
	if (mvdev->mig_state != VFIO_DEVICE_STATE_PRE_COPY &&
	    mvdev->mig_state != VFIO_DEVICE_STATE_PRE_COPY_P2P) {
		ret = -EINVAL;
		goto err_state_unlock;
	}

	/*
	 * We can't issue a SAVE command when the device is suspended, so as
	 * part of VFIO_DEVICE_STATE_PRE_COPY_P2P no reason to query for extra
	 * bytes that can't be read.
	 */
	if (mvdev->mig_state == VFIO_DEVICE_STATE_PRE_COPY) {
		/*
		 * Once the query returns it's guaranteed that there is no
		 * active SAVE command.
		 * As so, the other code below is safe with the proper locks.
		 */
		ret = mlx5vf_cmd_query_vhca_migration_state(mvdev, &inc_length,
							    MLX5VF_QUERY_INC);
		if (ret)
			goto err_state_unlock;
	}

	mutex_lock(&migf->lock);
	if (migf->state == MLX5_MIGF_STATE_ERROR) {
		ret = -ENODEV;
		goto err_migf_unlock;
	}

	if (migf->pre_copy_initial_bytes > *pos) {
		info.initial_bytes = migf->pre_copy_initial_bytes - *pos;
	} else {
		info.dirty_bytes = migf->max_pos - *pos;
		if (!info.dirty_bytes)
			end_of_data = true;
		info.dirty_bytes += inc_length;
	}

	if (!end_of_data || !inc_length) {
		mutex_unlock(&migf->lock);
		goto done;
	}

	mutex_unlock(&migf->lock);
	/*
	 * We finished transferring the current state and the device has a
	 * dirty state, save a new state to be ready for.
	 */
	buf = mlx5vf_get_data_buffer(migf, inc_length, DMA_FROM_DEVICE);
	if (IS_ERR(buf)) {
		ret = PTR_ERR(buf);
		mlx5vf_mark_err(migf);
		goto err_state_unlock;
	}

	ret = mlx5vf_cmd_save_vhca_state(mvdev, migf, buf, true, true);
	if (ret) {
		mlx5vf_mark_err(migf);
		mlx5vf_put_data_buffer(buf);
		goto err_state_unlock;
	}

done:
	mlx5vf_state_mutex_unlock(mvdev);
	if (copy_to_user((void __user *)arg, &info, minsz))
		return -EFAULT;
	return 0;

err_migf_unlock:
	mutex_unlock(&migf->lock);
err_state_unlock:
	mlx5vf_state_mutex_unlock(mvdev);
	return ret;
}

static const struct file_operations mlx5vf_save_fops = {
	.owner = THIS_MODULE,
	.read = mlx5vf_save_read,
	.poll = mlx5vf_save_poll,
	.unlocked_ioctl = mlx5vf_precopy_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.release = mlx5vf_release_file,
	.llseek = no_llseek,
};

static int mlx5vf_pci_save_device_inc_data(struct mlx5vf_pci_core_device *mvdev)
{
	struct mlx5_vf_migration_file *migf = mvdev->saving_migf;
	struct mlx5_vhca_data_buffer *buf;
	size_t length;
	int ret;

	if (migf->state == MLX5_MIGF_STATE_ERROR)
		return -ENODEV;

	ret = mlx5vf_cmd_query_vhca_migration_state(mvdev, &length,
				MLX5VF_QUERY_INC | MLX5VF_QUERY_FINAL);
	if (ret)
		goto err;

	/* Checking whether we have a matching pre-allocated buffer that can fit */
	if (migf->buf && migf->buf->allocated_length >= length) {
		buf = migf->buf;
		migf->buf = NULL;
	} else {
		buf = mlx5vf_get_data_buffer(migf, length, DMA_FROM_DEVICE);
		if (IS_ERR(buf)) {
			ret = PTR_ERR(buf);
			goto err;
		}
	}

	ret = mlx5vf_cmd_save_vhca_state(mvdev, migf, buf, true, false);
	if (ret)
		goto err_save;

	return 0;

err_save:
	mlx5vf_put_data_buffer(buf);
err:
	mlx5vf_mark_err(migf);
	return ret;
}

static struct mlx5_vf_migration_file *
mlx5vf_pci_save_device_data(struct mlx5vf_pci_core_device *mvdev, bool track)
{
	struct mlx5_vf_migration_file *migf;
	struct mlx5_vhca_data_buffer *buf;
	size_t length;
	int ret;

	migf = kzalloc(sizeof(*migf), GFP_KERNEL_ACCOUNT);
	if (!migf)
		return ERR_PTR(-ENOMEM);

	migf->filp = anon_inode_getfile("mlx5vf_mig", &mlx5vf_save_fops, migf,
					O_RDONLY);
	if (IS_ERR(migf->filp)) {
		ret = PTR_ERR(migf->filp);
		goto end;
	}

	migf->mvdev = mvdev;
	ret = mlx5vf_cmd_alloc_pd(migf);
	if (ret)
		goto out_free;

	stream_open(migf->filp->f_inode, migf->filp);
	mutex_init(&migf->lock);
	init_waitqueue_head(&migf->poll_wait);
	init_completion(&migf->save_comp);
	/*
	 * save_comp is being used as a binary semaphore built from
	 * a completion. A normal mutex cannot be used because the lock is
	 * passed between kernel threads and lockdep can't model this.
	 */
	complete(&migf->save_comp);
	mlx5_cmd_init_async_ctx(mvdev->mdev, &migf->async_ctx);
	INIT_WORK(&migf->async_data.work, mlx5vf_mig_file_cleanup_cb);
	INIT_LIST_HEAD(&migf->buf_list);
	INIT_LIST_HEAD(&migf->avail_list);
	spin_lock_init(&migf->list_lock);
	ret = mlx5vf_cmd_query_vhca_migration_state(mvdev, &length, 0);
	if (ret)
		goto out_pd;

	if (track) {
		ret = mlx5vf_prep_stop_copy(migf, length);
		if (ret)
			goto out_pd;
	}

	buf = mlx5vf_alloc_data_buffer(migf, length, DMA_FROM_DEVICE);
	if (IS_ERR(buf)) {
		ret = PTR_ERR(buf);
		goto out_pd;
	}

	ret = mlx5vf_cmd_save_vhca_state(mvdev, migf, buf, false, track);
	if (ret)
		goto out_save;
	return migf;
out_save:
	mlx5vf_free_data_buffer(buf);
out_pd:
	mlx5fv_cmd_clean_migf_resources(migf);
out_free:
	fput(migf->filp);
end:
	kfree(migf);
	return ERR_PTR(ret);
}

static int
mlx5vf_append_page_to_mig_buf(struct mlx5_vhca_data_buffer *vhca_buf,
			      const char __user **buf, size_t *len,
			      loff_t *pos, ssize_t *done)
{
	unsigned long offset;
	size_t page_offset;
	struct page *page;
	size_t page_len;
	u8 *to_buff;
	int ret;

	offset = *pos - vhca_buf->start_pos;
	page_offset = offset % PAGE_SIZE;

	page = mlx5vf_get_migration_page(vhca_buf, offset - page_offset);
	if (!page)
		return -EINVAL;
	page_len = min_t(size_t, *len, PAGE_SIZE - page_offset);
	to_buff = kmap_local_page(page);
	ret = copy_from_user(to_buff + page_offset, *buf, page_len);
	kunmap_local(to_buff);
	if (ret)
		return -EFAULT;

	*pos += page_len;
	*done += page_len;
	*buf += page_len;
	*len -= page_len;
	vhca_buf->length += page_len;
	return 0;
}

static int
mlx5vf_resume_read_image_no_header(struct mlx5_vhca_data_buffer *vhca_buf,
				   loff_t requested_length,
				   const char __user **buf, size_t *len,
				   loff_t *pos, ssize_t *done)
{
	int ret;

	if (requested_length > MAX_LOAD_SIZE)
		return -ENOMEM;

	if (vhca_buf->allocated_length < requested_length) {
		ret = mlx5vf_add_migration_pages(
			vhca_buf,
			DIV_ROUND_UP(requested_length - vhca_buf->allocated_length,
				     PAGE_SIZE));
		if (ret)
			return ret;
	}

	while (*len) {
		ret = mlx5vf_append_page_to_mig_buf(vhca_buf, buf, len, pos,
						    done);
		if (ret)
			return ret;
	}

	return 0;
}

static ssize_t
mlx5vf_resume_read_image(struct mlx5_vf_migration_file *migf,
			 struct mlx5_vhca_data_buffer *vhca_buf,
			 size_t image_size, const char __user **buf,
			 size_t *len, loff_t *pos, ssize_t *done,
			 bool *has_work)
{
	size_t copy_len, to_copy;
	int ret;

	to_copy = min_t(size_t, *len, image_size - vhca_buf->length);
	copy_len = to_copy;
	while (to_copy) {
		ret = mlx5vf_append_page_to_mig_buf(vhca_buf, buf, &to_copy, pos,
						    done);
		if (ret)
			return ret;
	}

	*len -= copy_len;
	if (vhca_buf->length == image_size) {
		migf->load_state = MLX5_VF_LOAD_STATE_LOAD_IMAGE;
		migf->max_pos += image_size;
		*has_work = true;
	}

	return 0;
}

static int
mlx5vf_resume_read_header_data(struct mlx5_vf_migration_file *migf,
			       struct mlx5_vhca_data_buffer *vhca_buf,
			       const char __user **buf, size_t *len,
			       loff_t *pos, ssize_t *done)
{
	size_t copy_len, to_copy;
	size_t required_data;
	u8 *to_buff;
	int ret;

	required_data = migf->record_size - vhca_buf->length;
	to_copy = min_t(size_t, *len, required_data);
	copy_len = to_copy;
	while (to_copy) {
		ret = mlx5vf_append_page_to_mig_buf(vhca_buf, buf, &to_copy, pos,
						    done);
		if (ret)
			return ret;
	}

	*len -= copy_len;
	if (vhca_buf->length == migf->record_size) {
		switch (migf->record_tag) {
		case MLX5_MIGF_HEADER_TAG_STOP_COPY_SIZE:
		{
			struct page *page;

			page = mlx5vf_get_migration_page(vhca_buf, 0);
			if (!page)
				return -EINVAL;
			to_buff = kmap_local_page(page);
			migf->stop_copy_prep_size = min_t(u64,
				le64_to_cpup((__le64 *)to_buff), MAX_LOAD_SIZE);
			kunmap_local(to_buff);
			break;
		}
		default:
			/* Optional tag */
			break;
		}

		migf->load_state = MLX5_VF_LOAD_STATE_READ_HEADER;
		migf->max_pos += migf->record_size;
		vhca_buf->length = 0;
	}

	return 0;
}

static int
mlx5vf_resume_read_header(struct mlx5_vf_migration_file *migf,
			  struct mlx5_vhca_data_buffer *vhca_buf,
			  const char __user **buf,
			  size_t *len, loff_t *pos,
			  ssize_t *done, bool *has_work)
{
	struct page *page;
	size_t copy_len;
	u8 *to_buff;
	int ret;

	copy_len = min_t(size_t, *len,
		sizeof(struct mlx5_vf_migration_header) - vhca_buf->length);
	page = mlx5vf_get_migration_page(vhca_buf, 0);
	if (!page)
		return -EINVAL;
	to_buff = kmap_local_page(page);
	ret = copy_from_user(to_buff + vhca_buf->length, *buf, copy_len);
	if (ret) {
		ret = -EFAULT;
		goto end;
	}

	*buf += copy_len;
	*pos += copy_len;
	*done += copy_len;
	*len -= copy_len;
	vhca_buf->length += copy_len;
	if (vhca_buf->length == sizeof(struct mlx5_vf_migration_header)) {
		u64 record_size;
		u32 flags;

		record_size = le64_to_cpup((__le64 *)to_buff);
		if (record_size > MAX_LOAD_SIZE) {
			ret = -ENOMEM;
			goto end;
		}

		migf->record_size = record_size;
		flags = le32_to_cpup((__le32 *)(to_buff +
			    offsetof(struct mlx5_vf_migration_header, flags)));
		migf->record_tag = le32_to_cpup((__le32 *)(to_buff +
			    offsetof(struct mlx5_vf_migration_header, tag)));
		switch (migf->record_tag) {
		case MLX5_MIGF_HEADER_TAG_FW_DATA:
			migf->load_state = MLX5_VF_LOAD_STATE_PREP_IMAGE;
			break;
		case MLX5_MIGF_HEADER_TAG_STOP_COPY_SIZE:
			migf->load_state = MLX5_VF_LOAD_STATE_PREP_HEADER_DATA;
			break;
		default:
			if (!(flags & MLX5_MIGF_HEADER_FLAGS_TAG_OPTIONAL)) {
				ret = -EOPNOTSUPP;
				goto end;
			}
			/* We may read and skip this optional record data */
			migf->load_state = MLX5_VF_LOAD_STATE_PREP_HEADER_DATA;
		}

		migf->max_pos += vhca_buf->length;
		vhca_buf->length = 0;
		*has_work = true;
	}
end:
	kunmap_local(to_buff);
	return ret;
}

static ssize_t mlx5vf_resume_write(struct file *filp, const char __user *buf,
				   size_t len, loff_t *pos)
{
	struct mlx5_vf_migration_file *migf = filp->private_data;
	struct mlx5_vhca_data_buffer *vhca_buf = migf->buf;
	struct mlx5_vhca_data_buffer *vhca_buf_header = migf->buf_header;
	loff_t requested_length;
	bool has_work = false;
	ssize_t done = 0;
	int ret = 0;

	if (pos)
		return -ESPIPE;
	pos = &filp->f_pos;

	if (*pos < 0 ||
	    check_add_overflow((loff_t)len, *pos, &requested_length))
		return -EINVAL;

	mutex_lock(&migf->mvdev->state_mutex);
	mutex_lock(&migf->lock);
	if (migf->state == MLX5_MIGF_STATE_ERROR) {
		ret = -ENODEV;
		goto out_unlock;
	}

	while (len || has_work) {
		has_work = false;
		switch (migf->load_state) {
		case MLX5_VF_LOAD_STATE_READ_HEADER:
			ret = mlx5vf_resume_read_header(migf, vhca_buf_header,
							&buf, &len, pos,
							&done, &has_work);
			if (ret)
				goto out_unlock;
			break;
		case MLX5_VF_LOAD_STATE_PREP_HEADER_DATA:
			if (vhca_buf_header->allocated_length < migf->record_size) {
				mlx5vf_free_data_buffer(vhca_buf_header);

				migf->buf_header = mlx5vf_alloc_data_buffer(migf,
						migf->record_size, DMA_NONE);
				if (IS_ERR(migf->buf_header)) {
					ret = PTR_ERR(migf->buf_header);
					migf->buf_header = NULL;
					goto out_unlock;
				}

				vhca_buf_header = migf->buf_header;
			}

			vhca_buf_header->start_pos = migf->max_pos;
			migf->load_state = MLX5_VF_LOAD_STATE_READ_HEADER_DATA;
			break;
		case MLX5_VF_LOAD_STATE_READ_HEADER_DATA:
			ret = mlx5vf_resume_read_header_data(migf, vhca_buf_header,
							&buf, &len, pos, &done);
			if (ret)
				goto out_unlock;
			break;
		case MLX5_VF_LOAD_STATE_PREP_IMAGE:
		{
			u64 size = max(migf->record_size,
				       migf->stop_copy_prep_size);

			if (vhca_buf->allocated_length < size) {
				mlx5vf_free_data_buffer(vhca_buf);

				migf->buf = mlx5vf_alloc_data_buffer(migf,
							size, DMA_TO_DEVICE);
				if (IS_ERR(migf->buf)) {
					ret = PTR_ERR(migf->buf);
					migf->buf = NULL;
					goto out_unlock;
				}

				vhca_buf = migf->buf;
			}

			vhca_buf->start_pos = migf->max_pos;
			migf->load_state = MLX5_VF_LOAD_STATE_READ_IMAGE;
			break;
		}
		case MLX5_VF_LOAD_STATE_READ_IMAGE_NO_HEADER:
			ret = mlx5vf_resume_read_image_no_header(vhca_buf,
						requested_length,
						&buf, &len, pos, &done);
			if (ret)
				goto out_unlock;
			break;
		case MLX5_VF_LOAD_STATE_READ_IMAGE:
			ret = mlx5vf_resume_read_image(migf, vhca_buf,
						migf->record_size,
						&buf, &len, pos, &done, &has_work);
			if (ret)
				goto out_unlock;
			break;
		case MLX5_VF_LOAD_STATE_LOAD_IMAGE:
			ret = mlx5vf_cmd_load_vhca_state(migf->mvdev, migf, vhca_buf);
			if (ret)
				goto out_unlock;
			migf->load_state = MLX5_VF_LOAD_STATE_READ_HEADER;

			/* prep header buf for next image */
			vhca_buf_header->length = 0;
			/* prep data buf for next image */
			vhca_buf->length = 0;

			break;
		default:
			break;
		}
	}

out_unlock:
	if (ret)
		migf->state = MLX5_MIGF_STATE_ERROR;
	mutex_unlock(&migf->lock);
	mlx5vf_state_mutex_unlock(migf->mvdev);
	return ret ? ret : done;
}

static const struct file_operations mlx5vf_resume_fops = {
	.owner = THIS_MODULE,
	.write = mlx5vf_resume_write,
	.release = mlx5vf_release_file,
	.llseek = no_llseek,
};

static struct mlx5_vf_migration_file *
mlx5vf_pci_resume_device_data(struct mlx5vf_pci_core_device *mvdev)
{
	struct mlx5_vf_migration_file *migf;
	struct mlx5_vhca_data_buffer *buf;
	int ret;

	migf = kzalloc(sizeof(*migf), GFP_KERNEL_ACCOUNT);
	if (!migf)
		return ERR_PTR(-ENOMEM);

	migf->filp = anon_inode_getfile("mlx5vf_mig", &mlx5vf_resume_fops, migf,
					O_WRONLY);
	if (IS_ERR(migf->filp)) {
		ret = PTR_ERR(migf->filp);
		goto end;
	}

	migf->mvdev = mvdev;
	ret = mlx5vf_cmd_alloc_pd(migf);
	if (ret)
		goto out_free;

	buf = mlx5vf_alloc_data_buffer(migf, 0, DMA_TO_DEVICE);
	if (IS_ERR(buf)) {
		ret = PTR_ERR(buf);
		goto out_pd;
	}

	migf->buf = buf;
	if (MLX5VF_PRE_COPY_SUPP(mvdev)) {
		buf = mlx5vf_alloc_data_buffer(migf,
			sizeof(struct mlx5_vf_migration_header), DMA_NONE);
		if (IS_ERR(buf)) {
			ret = PTR_ERR(buf);
			goto out_buf;
		}

		migf->buf_header = buf;
		migf->load_state = MLX5_VF_LOAD_STATE_READ_HEADER;
	} else {
		/* Initial state will be to read the image */
		migf->load_state = MLX5_VF_LOAD_STATE_READ_IMAGE_NO_HEADER;
	}

	stream_open(migf->filp->f_inode, migf->filp);
	mutex_init(&migf->lock);
	INIT_LIST_HEAD(&migf->buf_list);
	INIT_LIST_HEAD(&migf->avail_list);
	spin_lock_init(&migf->list_lock);
	return migf;
out_buf:
	mlx5vf_free_data_buffer(migf->buf);
out_pd:
	mlx5vf_cmd_dealloc_pd(migf);
out_free:
	fput(migf->filp);
end:
	kfree(migf);
	return ERR_PTR(ret);
}

void mlx5vf_disable_fds(struct mlx5vf_pci_core_device *mvdev)
{
	if (mvdev->resuming_migf) {
		mlx5vf_disable_fd(mvdev->resuming_migf);
		mlx5fv_cmd_clean_migf_resources(mvdev->resuming_migf);
		fput(mvdev->resuming_migf->filp);
		mvdev->resuming_migf = NULL;
	}
	if (mvdev->saving_migf) {
		mlx5_cmd_cleanup_async_ctx(&mvdev->saving_migf->async_ctx);
		cancel_work_sync(&mvdev->saving_migf->async_data.work);
		mlx5vf_disable_fd(mvdev->saving_migf);
		mlx5fv_cmd_clean_migf_resources(mvdev->saving_migf);
		fput(mvdev->saving_migf->filp);
		mvdev->saving_migf = NULL;
	}
}

static struct file *
mlx5vf_pci_step_device_state_locked(struct mlx5vf_pci_core_device *mvdev,
				    u32 new)
{
	u32 cur = mvdev->mig_state;
	int ret;

	if (cur == VFIO_DEVICE_STATE_RUNNING_P2P && new == VFIO_DEVICE_STATE_STOP) {
		ret = mlx5vf_cmd_suspend_vhca(mvdev,
			MLX5_SUSPEND_VHCA_IN_OP_MOD_SUSPEND_RESPONDER);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_RUNNING_P2P) {
		ret = mlx5vf_cmd_resume_vhca(mvdev,
			MLX5_RESUME_VHCA_IN_OP_MOD_RESUME_RESPONDER);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	if ((cur == VFIO_DEVICE_STATE_RUNNING && new == VFIO_DEVICE_STATE_RUNNING_P2P) ||
	    (cur == VFIO_DEVICE_STATE_PRE_COPY && new == VFIO_DEVICE_STATE_PRE_COPY_P2P)) {
		ret = mlx5vf_cmd_suspend_vhca(mvdev,
			MLX5_SUSPEND_VHCA_IN_OP_MOD_SUSPEND_INITIATOR);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	if ((cur == VFIO_DEVICE_STATE_RUNNING_P2P && new == VFIO_DEVICE_STATE_RUNNING) ||
	    (cur == VFIO_DEVICE_STATE_PRE_COPY_P2P && new == VFIO_DEVICE_STATE_PRE_COPY)) {
		ret = mlx5vf_cmd_resume_vhca(mvdev,
			MLX5_RESUME_VHCA_IN_OP_MOD_RESUME_INITIATOR);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_STOP_COPY) {
		struct mlx5_vf_migration_file *migf;

		migf = mlx5vf_pci_save_device_data(mvdev, false);
		if (IS_ERR(migf))
			return ERR_CAST(migf);
		get_file(migf->filp);
		mvdev->saving_migf = migf;
		return migf->filp;
	}

	if ((cur == VFIO_DEVICE_STATE_STOP_COPY && new == VFIO_DEVICE_STATE_STOP) ||
	    (cur == VFIO_DEVICE_STATE_PRE_COPY && new == VFIO_DEVICE_STATE_RUNNING) ||
	    (cur == VFIO_DEVICE_STATE_PRE_COPY_P2P &&
	     new == VFIO_DEVICE_STATE_RUNNING_P2P)) {
		mlx5vf_disable_fds(mvdev);
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_RESUMING) {
		struct mlx5_vf_migration_file *migf;

		migf = mlx5vf_pci_resume_device_data(mvdev);
		if (IS_ERR(migf))
			return ERR_CAST(migf);
		get_file(migf->filp);
		mvdev->resuming_migf = migf;
		return migf->filp;
	}

	if (cur == VFIO_DEVICE_STATE_RESUMING && new == VFIO_DEVICE_STATE_STOP) {
		if (!MLX5VF_PRE_COPY_SUPP(mvdev)) {
			ret = mlx5vf_cmd_load_vhca_state(mvdev,
							 mvdev->resuming_migf,
							 mvdev->resuming_migf->buf);
			if (ret)
				return ERR_PTR(ret);
		}
		mlx5vf_disable_fds(mvdev);
		return NULL;
	}

	if ((cur == VFIO_DEVICE_STATE_RUNNING && new == VFIO_DEVICE_STATE_PRE_COPY) ||
	    (cur == VFIO_DEVICE_STATE_RUNNING_P2P &&
	     new == VFIO_DEVICE_STATE_PRE_COPY_P2P)) {
		struct mlx5_vf_migration_file *migf;

		migf = mlx5vf_pci_save_device_data(mvdev, true);
		if (IS_ERR(migf))
			return ERR_CAST(migf);
		get_file(migf->filp);
		mvdev->saving_migf = migf;
		return migf->filp;
	}

	if (cur == VFIO_DEVICE_STATE_PRE_COPY_P2P && new == VFIO_DEVICE_STATE_STOP_COPY) {
		ret = mlx5vf_cmd_suspend_vhca(mvdev,
			MLX5_SUSPEND_VHCA_IN_OP_MOD_SUSPEND_RESPONDER);
		if (ret)
			return ERR_PTR(ret);
		ret = mlx5vf_pci_save_device_inc_data(mvdev);
		return ret ? ERR_PTR(ret) : NULL;
	}

	/*
	 * vfio_mig_get_next_state() does not use arcs other than the above
	 */
	WARN_ON(true);
	return ERR_PTR(-EINVAL);
}

/*
 * This function is called in all state_mutex unlock cases to
 * handle a 'deferred_reset' if exists.
 */
void mlx5vf_state_mutex_unlock(struct mlx5vf_pci_core_device *mvdev)
{
again:
	spin_lock(&mvdev->reset_lock);
	if (mvdev->deferred_reset) {
		mvdev->deferred_reset = false;
		spin_unlock(&mvdev->reset_lock);
		mvdev->mig_state = VFIO_DEVICE_STATE_RUNNING;
		mlx5vf_disable_fds(mvdev);
		goto again;
	}
	mutex_unlock(&mvdev->state_mutex);
	spin_unlock(&mvdev->reset_lock);
}

static struct file *
mlx5vf_pci_set_device_state(struct vfio_device *vdev,
			    enum vfio_device_mig_state new_state)
{
	struct mlx5vf_pci_core_device *mvdev = container_of(
		vdev, struct mlx5vf_pci_core_device, core_device.vdev);
	enum vfio_device_mig_state next_state;
	struct file *res = NULL;
	int ret;

	mutex_lock(&mvdev->state_mutex);
	while (new_state != mvdev->mig_state) {
		ret = vfio_mig_get_next_state(vdev, mvdev->mig_state,
					      new_state, &next_state);
		if (ret) {
			res = ERR_PTR(ret);
			break;
		}
		res = mlx5vf_pci_step_device_state_locked(mvdev, next_state);
		if (IS_ERR(res))
			break;
		mvdev->mig_state = next_state;
		if (WARN_ON(res && new_state != mvdev->mig_state)) {
			fput(res);
			res = ERR_PTR(-EINVAL);
			break;
		}
	}
	mlx5vf_state_mutex_unlock(mvdev);
	return res;
}

static int mlx5vf_pci_get_data_size(struct vfio_device *vdev,
				    unsigned long *stop_copy_length)
{
	struct mlx5vf_pci_core_device *mvdev = container_of(
		vdev, struct mlx5vf_pci_core_device, core_device.vdev);
	size_t state_size;
	int ret;

	mutex_lock(&mvdev->state_mutex);
	ret = mlx5vf_cmd_query_vhca_migration_state(mvdev,
						    &state_size, 0);
	if (!ret)
		*stop_copy_length = state_size;
	mlx5vf_state_mutex_unlock(mvdev);
	return ret;
}

static int mlx5vf_pci_get_device_state(struct vfio_device *vdev,
				       enum vfio_device_mig_state *curr_state)
{
	struct mlx5vf_pci_core_device *mvdev = container_of(
		vdev, struct mlx5vf_pci_core_device, core_device.vdev);

	mutex_lock(&mvdev->state_mutex);
	*curr_state = mvdev->mig_state;
	mlx5vf_state_mutex_unlock(mvdev);
	return 0;
}

static void mlx5vf_pci_aer_reset_done(struct pci_dev *pdev)
{
	struct mlx5vf_pci_core_device *mvdev = mlx5vf_drvdata(pdev);

	if (!mvdev->migrate_cap)
		return;

	/*
	 * As the higher VFIO layers are holding locks across reset and using
	 * those same locks with the mm_lock we need to prevent ABBA deadlock
	 * with the state_mutex and mm_lock.
	 * In case the state_mutex was taken already we defer the cleanup work
	 * to the unlock flow of the other running context.
	 */
	spin_lock(&mvdev->reset_lock);
	mvdev->deferred_reset = true;
	if (!mutex_trylock(&mvdev->state_mutex)) {
		spin_unlock(&mvdev->reset_lock);
		return;
	}
	spin_unlock(&mvdev->reset_lock);
	mlx5vf_state_mutex_unlock(mvdev);
}

static int mlx5vf_pci_open_device(struct vfio_device *core_vdev)
{
	struct mlx5vf_pci_core_device *mvdev = container_of(
		core_vdev, struct mlx5vf_pci_core_device, core_device.vdev);
	struct vfio_pci_core_device *vdev = &mvdev->core_device;
	int ret;

	ret = vfio_pci_core_enable(vdev);
	if (ret)
		return ret;

	if (mvdev->migrate_cap)
		mvdev->mig_state = VFIO_DEVICE_STATE_RUNNING;
	vfio_pci_core_finish_enable(vdev);
	return 0;
}

static void mlx5vf_pci_close_device(struct vfio_device *core_vdev)
{
	struct mlx5vf_pci_core_device *mvdev = container_of(
		core_vdev, struct mlx5vf_pci_core_device, core_device.vdev);

	mlx5vf_cmd_close_migratable(mvdev);
	vfio_pci_core_close_device(core_vdev);
}

static const struct vfio_migration_ops mlx5vf_pci_mig_ops = {
	.migration_set_state = mlx5vf_pci_set_device_state,
	.migration_get_state = mlx5vf_pci_get_device_state,
	.migration_get_data_size = mlx5vf_pci_get_data_size,
};

static const struct vfio_log_ops mlx5vf_pci_log_ops = {
	.log_start = mlx5vf_start_page_tracker,
	.log_stop = mlx5vf_stop_page_tracker,
	.log_read_and_clear = mlx5vf_tracker_read_and_clear,
};

static int mlx5vf_pci_init_dev(struct vfio_device *core_vdev)
{
	struct mlx5vf_pci_core_device *mvdev = container_of(core_vdev,
			struct mlx5vf_pci_core_device, core_device.vdev);
	int ret;

	ret = vfio_pci_core_init_dev(core_vdev);
	if (ret)
		return ret;

	mlx5vf_cmd_set_migratable(mvdev, &mlx5vf_pci_mig_ops,
				  &mlx5vf_pci_log_ops);

	return 0;
}

static void mlx5vf_pci_release_dev(struct vfio_device *core_vdev)
{
	struct mlx5vf_pci_core_device *mvdev = container_of(core_vdev,
			struct mlx5vf_pci_core_device, core_device.vdev);

	mlx5vf_cmd_remove_migratable(mvdev);
	vfio_pci_core_release_dev(core_vdev);
}

static const struct vfio_device_ops mlx5vf_pci_ops = {
	.name = "mlx5-vfio-pci",
	.init = mlx5vf_pci_init_dev,
	.release = mlx5vf_pci_release_dev,
	.open_device = mlx5vf_pci_open_device,
	.close_device = mlx5vf_pci_close_device,
	.ioctl = vfio_pci_core_ioctl,
	.device_feature = vfio_pci_core_ioctl_feature,
	.read = vfio_pci_core_read,
	.write = vfio_pci_core_write,
	.mmap = vfio_pci_core_mmap,
	.request = vfio_pci_core_request,
	.match = vfio_pci_core_match,
	.bind_iommufd = vfio_iommufd_physical_bind,
	.unbind_iommufd = vfio_iommufd_physical_unbind,
	.attach_ioas = vfio_iommufd_physical_attach_ioas,
	.detach_ioas = vfio_iommufd_physical_detach_ioas,
};

static int mlx5vf_pci_probe(struct pci_dev *pdev,
			    const struct pci_device_id *id)
{
	struct mlx5vf_pci_core_device *mvdev;
	int ret;

	mvdev = vfio_alloc_device(mlx5vf_pci_core_device, core_device.vdev,
				  &pdev->dev, &mlx5vf_pci_ops);
	if (IS_ERR(mvdev))
		return PTR_ERR(mvdev);

	dev_set_drvdata(&pdev->dev, &mvdev->core_device);
	ret = vfio_pci_core_register_device(&mvdev->core_device);
	if (ret)
		goto out_put_vdev;
	return 0;

out_put_vdev:
	vfio_put_device(&mvdev->core_device.vdev);
	return ret;
}

static void mlx5vf_pci_remove(struct pci_dev *pdev)
{
	struct mlx5vf_pci_core_device *mvdev = mlx5vf_drvdata(pdev);

	vfio_pci_core_unregister_device(&mvdev->core_device);
	vfio_put_device(&mvdev->core_device.vdev);
}

static const struct pci_device_id mlx5vf_pci_table[] = {
	{ PCI_DRIVER_OVERRIDE_DEVICE_VFIO(PCI_VENDOR_ID_MELLANOX, 0x101e) }, /* ConnectX Family mlx5Gen Virtual Function */
	{}
};

MODULE_DEVICE_TABLE(pci, mlx5vf_pci_table);

static const struct pci_error_handlers mlx5vf_err_handlers = {
	.reset_done = mlx5vf_pci_aer_reset_done,
	.error_detected = vfio_pci_core_aer_err_detected,
};

static struct pci_driver mlx5vf_pci_driver = {
	.name = KBUILD_MODNAME,
	.id_table = mlx5vf_pci_table,
	.probe = mlx5vf_pci_probe,
	.remove = mlx5vf_pci_remove,
	.err_handler = &mlx5vf_err_handlers,
	.driver_managed_dma = true,
};

module_pci_driver(mlx5vf_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Max Gurtovoy <mgurtovoy@nvidia.com>");
MODULE_AUTHOR("Yishai Hadas <yishaih@nvidia.com>");
MODULE_DESCRIPTION(
	"MLX5 VFIO PCI - User Level meta-driver for MLX5 device family");
