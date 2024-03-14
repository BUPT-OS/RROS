/* SPDX-License-Identifier: GPL-2.0 */

#ifndef BTRFS_DEFRAG_H
#define BTRFS_DEFRAG_H

int btrfs_defrag_file(struct inode *inode, struct file_ra_state *ra,
		      struct btrfs_ioctl_defrag_range_args *range,
		      u64 newer_than, unsigned long max_to_defrag);
int __init btrfs_auto_defrag_init(void);
void __cold btrfs_auto_defrag_exit(void);
int btrfs_add_inode_defrag(struct btrfs_trans_handle *trans,
			   struct btrfs_inode *inode, u32 extent_thresh);
int btrfs_run_defrag_inodes(struct btrfs_fs_info *fs_info);
void btrfs_cleanup_defrag_inodes(struct btrfs_fs_info *fs_info);
int btrfs_defrag_leaves(struct btrfs_trans_handle *trans, struct btrfs_root *root);

static inline int btrfs_defrag_cancelled(struct btrfs_fs_info *fs_info)
{
	return signal_pending(current);
}

#endif
