/* SPDX-License-Identifier: GPL-2.0 */

#ifndef BTRFS_FILE_H
#define BTRFS_FILE_H

extern const struct file_operations btrfs_file_operations;

int btrfs_sync_file(struct file *file, loff_t start, loff_t end, int datasync);
int btrfs_drop_extents(struct btrfs_trans_handle *trans,
		       struct btrfs_root *root, struct btrfs_inode *inode,
		       struct btrfs_drop_extents_args *args);
int btrfs_replace_file_extents(struct btrfs_inode *inode,
			   struct btrfs_path *path, const u64 start,
			   const u64 end,
			   struct btrfs_replace_extent_info *extent_info,
			   struct btrfs_trans_handle **trans_out);
int btrfs_mark_extent_written(struct btrfs_trans_handle *trans,
			      struct btrfs_inode *inode, u64 start, u64 end);
ssize_t btrfs_do_write_iter(struct kiocb *iocb, struct iov_iter *from,
			    const struct btrfs_ioctl_encoded_io_args *encoded);
int btrfs_release_file(struct inode *inode, struct file *file);
int btrfs_dirty_pages(struct btrfs_inode *inode, struct page **pages,
		      size_t num_pages, loff_t pos, size_t write_bytes,
		      struct extent_state **cached, bool noreserve);
int btrfs_fdatawrite_range(struct inode *inode, loff_t start, loff_t end);
int btrfs_check_nocow_lock(struct btrfs_inode *inode, loff_t pos,
			   size_t *write_bytes, bool nowait);
void btrfs_check_nocow_unlock(struct btrfs_inode *inode);
bool btrfs_find_delalloc_in_range(struct btrfs_inode *inode, u64 start, u64 end,
				  struct extent_state **cached_state,
				  u64 *delalloc_start_ret, u64 *delalloc_end_ret);

#endif
