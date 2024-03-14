/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * Copyright IBM Corporation, 2010
 * Author Aneesh Kumar K.V <aneesh.kumar@linux.vnet.ibm.com>
 */
#ifndef FS_9P_ACL_H
#define FS_9P_ACL_H

#ifdef CONFIG_9P_FS_POSIX_ACL
int v9fs_get_acl(struct inode *inode, struct p9_fid *fid);
struct posix_acl *v9fs_iop_get_inode_acl(struct inode *inode, int type,
				   bool rcu);
struct posix_acl *v9fs_iop_get_acl(struct mnt_idmap *idmap,
					  struct dentry *dentry, int type);
int v9fs_iop_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		     struct posix_acl *acl, int type);
int v9fs_acl_chmod(struct inode *inode, struct p9_fid *fid);
int v9fs_set_create_acl(struct inode *inode, struct p9_fid *fid,
			struct posix_acl *dacl, struct posix_acl *acl);
int v9fs_acl_mode(struct inode *dir, umode_t *modep,
		  struct posix_acl **dpacl, struct posix_acl **pacl);
void v9fs_put_acl(struct posix_acl *dacl, struct posix_acl *acl);
#else
#define v9fs_iop_get_inode_acl	NULL
#define v9fs_iop_get_acl NULL
#define v9fs_iop_set_acl NULL
static inline int v9fs_get_acl(struct inode *inode, struct p9_fid *fid)
{
	return 0;
}
static inline int v9fs_acl_chmod(struct inode *inode, struct p9_fid *fid)
{
	return 0;
}
static inline int v9fs_set_create_acl(struct inode *inode,
				      struct p9_fid *fid,
				      struct posix_acl *dacl,
				      struct posix_acl *acl)
{
	return 0;
}
static inline void v9fs_put_acl(struct posix_acl *dacl,
				struct posix_acl *acl)
{
}
static inline int v9fs_acl_mode(struct inode *dir, umode_t *modep,
				struct posix_acl **dpacl,
				struct posix_acl **pacl)
{
	return 0;
}

#endif
#endif /* FS_9P_XATTR_H */
