/* SPDX-License-Identifier: GPL-2.0 */

#ifndef BTRFS_DIR_ITEM_H
#define BTRFS_DIR_ITEM_H

int btrfs_check_dir_item_collision(struct btrfs_root *root, u64 dir,
			  const struct fscrypt_str *name);
int btrfs_insert_dir_item(struct btrfs_trans_handle *trans,
			  const struct fscrypt_str *name, struct btrfs_inode *dir,
			  struct btrfs_key *location, u8 type, u64 index);
struct btrfs_dir_item *btrfs_lookup_dir_item(struct btrfs_trans_handle *trans,
					     struct btrfs_root *root,
					     struct btrfs_path *path, u64 dir,
					     const struct fscrypt_str *name, int mod);
struct btrfs_dir_item *btrfs_lookup_dir_index_item(
			struct btrfs_trans_handle *trans,
			struct btrfs_root *root,
			struct btrfs_path *path, u64 dir,
			u64 index, const struct fscrypt_str *name, int mod);
struct btrfs_dir_item *btrfs_search_dir_index_item(struct btrfs_root *root,
			    struct btrfs_path *path, u64 dirid,
			    const struct fscrypt_str *name);
int btrfs_delete_one_dir_name(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root,
			      struct btrfs_path *path,
			      struct btrfs_dir_item *di);
int btrfs_insert_xattr_item(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root,
			    struct btrfs_path *path, u64 objectid,
			    const char *name, u16 name_len,
			    const void *data, u16 data_len);
struct btrfs_dir_item *btrfs_lookup_xattr(struct btrfs_trans_handle *trans,
					  struct btrfs_root *root,
					  struct btrfs_path *path, u64 dir,
					  const char *name, u16 name_len,
					  int mod);
struct btrfs_dir_item *btrfs_match_dir_item_name(struct btrfs_fs_info *fs_info,
						 struct btrfs_path *path,
						 const char *name,
						 int name_len);

#endif
