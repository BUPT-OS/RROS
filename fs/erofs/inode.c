// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017-2018 HUAWEI, Inc.
 *             https://www.huawei.com/
 * Copyright (C) 2021, Alibaba Cloud
 */
#include "xattr.h"

#include <trace/events/erofs.h>

static void *erofs_read_inode(struct erofs_buf *buf,
			      struct inode *inode, unsigned int *ofs)
{
	struct super_block *sb = inode->i_sb;
	struct erofs_sb_info *sbi = EROFS_SB(sb);
	struct erofs_inode *vi = EROFS_I(inode);
	const erofs_off_t inode_loc = erofs_iloc(inode);

	erofs_blk_t blkaddr, nblks = 0;
	void *kaddr;
	struct erofs_inode_compact *dic;
	struct erofs_inode_extended *die, *copied = NULL;
	unsigned int ifmt;
	int err;

	blkaddr = erofs_blknr(sb, inode_loc);
	*ofs = erofs_blkoff(sb, inode_loc);

	kaddr = erofs_read_metabuf(buf, sb, blkaddr, EROFS_KMAP);
	if (IS_ERR(kaddr)) {
		erofs_err(sb, "failed to get inode (nid: %llu) page, err %ld",
			  vi->nid, PTR_ERR(kaddr));
		return kaddr;
	}

	dic = kaddr + *ofs;
	ifmt = le16_to_cpu(dic->i_format);

	if (ifmt & ~EROFS_I_ALL) {
		erofs_err(inode->i_sb, "unsupported i_format %u of nid %llu",
			  ifmt, vi->nid);
		err = -EOPNOTSUPP;
		goto err_out;
	}

	vi->datalayout = erofs_inode_datalayout(ifmt);
	if (vi->datalayout >= EROFS_INODE_DATALAYOUT_MAX) {
		erofs_err(inode->i_sb, "unsupported datalayout %u of nid %llu",
			  vi->datalayout, vi->nid);
		err = -EOPNOTSUPP;
		goto err_out;
	}

	switch (erofs_inode_version(ifmt)) {
	case EROFS_INODE_LAYOUT_EXTENDED:
		vi->inode_isize = sizeof(struct erofs_inode_extended);
		/* check if the extended inode acrosses block boundary */
		if (*ofs + vi->inode_isize <= sb->s_blocksize) {
			*ofs += vi->inode_isize;
			die = (struct erofs_inode_extended *)dic;
		} else {
			const unsigned int gotten = sb->s_blocksize - *ofs;

			copied = kmalloc(vi->inode_isize, GFP_NOFS);
			if (!copied) {
				err = -ENOMEM;
				goto err_out;
			}
			memcpy(copied, dic, gotten);
			kaddr = erofs_read_metabuf(buf, sb, blkaddr + 1,
						   EROFS_KMAP);
			if (IS_ERR(kaddr)) {
				erofs_err(sb, "failed to get inode payload block (nid: %llu), err %ld",
					  vi->nid, PTR_ERR(kaddr));
				kfree(copied);
				return kaddr;
			}
			*ofs = vi->inode_isize - gotten;
			memcpy((u8 *)copied + gotten, kaddr, *ofs);
			die = copied;
		}
		vi->xattr_isize = erofs_xattr_ibody_size(die->i_xattr_icount);

		inode->i_mode = le16_to_cpu(die->i_mode);
		switch (inode->i_mode & S_IFMT) {
		case S_IFREG:
		case S_IFDIR:
		case S_IFLNK:
			vi->raw_blkaddr = le32_to_cpu(die->i_u.raw_blkaddr);
			break;
		case S_IFCHR:
		case S_IFBLK:
			inode->i_rdev =
				new_decode_dev(le32_to_cpu(die->i_u.rdev));
			break;
		case S_IFIFO:
		case S_IFSOCK:
			inode->i_rdev = 0;
			break;
		default:
			goto bogusimode;
		}
		i_uid_write(inode, le32_to_cpu(die->i_uid));
		i_gid_write(inode, le32_to_cpu(die->i_gid));
		set_nlink(inode, le32_to_cpu(die->i_nlink));

		/* extended inode has its own timestamp */
		inode_set_ctime(inode, le64_to_cpu(die->i_mtime),
				le32_to_cpu(die->i_mtime_nsec));

		inode->i_size = le64_to_cpu(die->i_size);

		/* total blocks for compressed files */
		if (erofs_inode_is_data_compressed(vi->datalayout))
			nblks = le32_to_cpu(die->i_u.compressed_blocks);
		else if (vi->datalayout == EROFS_INODE_CHUNK_BASED)
			/* fill chunked inode summary info */
			vi->chunkformat = le16_to_cpu(die->i_u.c.format);
		kfree(copied);
		copied = NULL;
		break;
	case EROFS_INODE_LAYOUT_COMPACT:
		vi->inode_isize = sizeof(struct erofs_inode_compact);
		*ofs += vi->inode_isize;
		vi->xattr_isize = erofs_xattr_ibody_size(dic->i_xattr_icount);

		inode->i_mode = le16_to_cpu(dic->i_mode);
		switch (inode->i_mode & S_IFMT) {
		case S_IFREG:
		case S_IFDIR:
		case S_IFLNK:
			vi->raw_blkaddr = le32_to_cpu(dic->i_u.raw_blkaddr);
			break;
		case S_IFCHR:
		case S_IFBLK:
			inode->i_rdev =
				new_decode_dev(le32_to_cpu(dic->i_u.rdev));
			break;
		case S_IFIFO:
		case S_IFSOCK:
			inode->i_rdev = 0;
			break;
		default:
			goto bogusimode;
		}
		i_uid_write(inode, le16_to_cpu(dic->i_uid));
		i_gid_write(inode, le16_to_cpu(dic->i_gid));
		set_nlink(inode, le16_to_cpu(dic->i_nlink));

		/* use build time for compact inodes */
		inode_set_ctime(inode, sbi->build_time, sbi->build_time_nsec);

		inode->i_size = le32_to_cpu(dic->i_size);
		if (erofs_inode_is_data_compressed(vi->datalayout))
			nblks = le32_to_cpu(dic->i_u.compressed_blocks);
		else if (vi->datalayout == EROFS_INODE_CHUNK_BASED)
			vi->chunkformat = le16_to_cpu(dic->i_u.c.format);
		break;
	default:
		erofs_err(inode->i_sb,
			  "unsupported on-disk inode version %u of nid %llu",
			  erofs_inode_version(ifmt), vi->nid);
		err = -EOPNOTSUPP;
		goto err_out;
	}

	if (vi->datalayout == EROFS_INODE_CHUNK_BASED) {
		if (vi->chunkformat & ~EROFS_CHUNK_FORMAT_ALL) {
			erofs_err(inode->i_sb,
				  "unsupported chunk format %x of nid %llu",
				  vi->chunkformat, vi->nid);
			err = -EOPNOTSUPP;
			goto err_out;
		}
		vi->chunkbits = sb->s_blocksize_bits +
			(vi->chunkformat & EROFS_CHUNK_FORMAT_BLKBITS_MASK);
	}
	inode->i_mtime = inode->i_atime = inode_get_ctime(inode);

	inode->i_flags &= ~S_DAX;
	if (test_opt(&sbi->opt, DAX_ALWAYS) && S_ISREG(inode->i_mode) &&
	    (vi->datalayout == EROFS_INODE_FLAT_PLAIN ||
	     vi->datalayout == EROFS_INODE_CHUNK_BASED))
		inode->i_flags |= S_DAX;

	if (!nblks)
		/* measure inode.i_blocks as generic filesystems */
		inode->i_blocks = round_up(inode->i_size, sb->s_blocksize) >> 9;
	else
		inode->i_blocks = nblks << (sb->s_blocksize_bits - 9);
	return kaddr;

bogusimode:
	erofs_err(inode->i_sb, "bogus i_mode (%o) @ nid %llu",
		  inode->i_mode, vi->nid);
	err = -EFSCORRUPTED;
err_out:
	DBG_BUGON(1);
	kfree(copied);
	erofs_put_metabuf(buf);
	return ERR_PTR(err);
}

static int erofs_fill_symlink(struct inode *inode, void *kaddr,
			      unsigned int m_pofs)
{
	struct erofs_inode *vi = EROFS_I(inode);
	unsigned int bsz = i_blocksize(inode);
	char *lnk;

	/* if it cannot be handled with fast symlink scheme */
	if (vi->datalayout != EROFS_INODE_FLAT_INLINE ||
	    inode->i_size >= bsz || inode->i_size < 0) {
		inode->i_op = &erofs_symlink_iops;
		return 0;
	}

	lnk = kmalloc(inode->i_size + 1, GFP_KERNEL);
	if (!lnk)
		return -ENOMEM;

	m_pofs += vi->xattr_isize;
	/* inline symlink data shouldn't cross block boundary */
	if (m_pofs + inode->i_size > bsz) {
		kfree(lnk);
		erofs_err(inode->i_sb,
			  "inline data cross block boundary @ nid %llu",
			  vi->nid);
		DBG_BUGON(1);
		return -EFSCORRUPTED;
	}
	memcpy(lnk, kaddr + m_pofs, inode->i_size);
	lnk[inode->i_size] = '\0';

	inode->i_link = lnk;
	inode->i_op = &erofs_fast_symlink_iops;
	return 0;
}

static int erofs_fill_inode(struct inode *inode)
{
	struct erofs_inode *vi = EROFS_I(inode);
	struct erofs_buf buf = __EROFS_BUF_INITIALIZER;
	void *kaddr;
	unsigned int ofs;
	int err = 0;

	trace_erofs_fill_inode(inode);

	/* read inode base data from disk */
	kaddr = erofs_read_inode(&buf, inode, &ofs);
	if (IS_ERR(kaddr))
		return PTR_ERR(kaddr);

	/* setup the new inode */
	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
		inode->i_op = &erofs_generic_iops;
		if (erofs_inode_is_data_compressed(vi->datalayout))
			inode->i_fop = &generic_ro_fops;
		else
			inode->i_fop = &erofs_file_fops;
		break;
	case S_IFDIR:
		inode->i_op = &erofs_dir_iops;
		inode->i_fop = &erofs_dir_fops;
		inode_nohighmem(inode);
		break;
	case S_IFLNK:
		err = erofs_fill_symlink(inode, kaddr, ofs);
		if (err)
			goto out_unlock;
		inode_nohighmem(inode);
		break;
	case S_IFCHR:
	case S_IFBLK:
	case S_IFIFO:
	case S_IFSOCK:
		inode->i_op = &erofs_generic_iops;
		init_special_inode(inode, inode->i_mode, inode->i_rdev);
		goto out_unlock;
	default:
		err = -EFSCORRUPTED;
		goto out_unlock;
	}

	if (erofs_inode_is_data_compressed(vi->datalayout)) {
#ifdef CONFIG_EROFS_FS_ZIP
		if (!erofs_is_fscache_mode(inode->i_sb) &&
		    inode->i_sb->s_blocksize_bits == PAGE_SHIFT) {
			inode->i_mapping->a_ops = &z_erofs_aops;
			err = 0;
			goto out_unlock;
		}
#endif
		err = -EOPNOTSUPP;
		goto out_unlock;
	}
	inode->i_mapping->a_ops = &erofs_raw_access_aops;
	mapping_set_large_folios(inode->i_mapping);
#ifdef CONFIG_EROFS_FS_ONDEMAND
	if (erofs_is_fscache_mode(inode->i_sb))
		inode->i_mapping->a_ops = &erofs_fscache_access_aops;
#endif

out_unlock:
	erofs_put_metabuf(&buf);
	return err;
}

/*
 * ino_t is 32-bits on 32-bit arch. We have to squash the 64-bit value down
 * so that it will fit.
 */
static ino_t erofs_squash_ino(erofs_nid_t nid)
{
	ino_t ino = (ino_t)nid;

	if (sizeof(ino_t) < sizeof(erofs_nid_t))
		ino ^= nid >> (sizeof(erofs_nid_t) - sizeof(ino_t)) * 8;
	return ino;
}

static int erofs_iget5_eq(struct inode *inode, void *opaque)
{
	return EROFS_I(inode)->nid == *(erofs_nid_t *)opaque;
}

static int erofs_iget5_set(struct inode *inode, void *opaque)
{
	const erofs_nid_t nid = *(erofs_nid_t *)opaque;

	inode->i_ino = erofs_squash_ino(nid);
	EROFS_I(inode)->nid = nid;
	return 0;
}

struct inode *erofs_iget(struct super_block *sb, erofs_nid_t nid)
{
	struct inode *inode;

	inode = iget5_locked(sb, erofs_squash_ino(nid), erofs_iget5_eq,
			     erofs_iget5_set, &nid);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	if (inode->i_state & I_NEW) {
		int err = erofs_fill_inode(inode);

		if (err) {
			iget_failed(inode);
			return ERR_PTR(err);
		}
		unlock_new_inode(inode);
	}
	return inode;
}

int erofs_getattr(struct mnt_idmap *idmap, const struct path *path,
		  struct kstat *stat, u32 request_mask,
		  unsigned int query_flags)
{
	struct inode *const inode = d_inode(path->dentry);

	if (erofs_inode_is_data_compressed(EROFS_I(inode)->datalayout))
		stat->attributes |= STATX_ATTR_COMPRESSED;

	stat->attributes |= STATX_ATTR_IMMUTABLE;
	stat->attributes_mask |= (STATX_ATTR_COMPRESSED |
				  STATX_ATTR_IMMUTABLE);

	generic_fillattr(idmap, request_mask, inode, stat);
	return 0;
}

const struct inode_operations erofs_generic_iops = {
	.getattr = erofs_getattr,
	.listxattr = erofs_listxattr,
	.get_inode_acl = erofs_get_acl,
	.fiemap = erofs_fiemap,
};

const struct inode_operations erofs_symlink_iops = {
	.get_link = page_get_link,
	.getattr = erofs_getattr,
	.listxattr = erofs_listxattr,
	.get_inode_acl = erofs_get_acl,
};

const struct inode_operations erofs_fast_symlink_iops = {
	.get_link = simple_get_link,
	.getattr = erofs_getattr,
	.listxattr = erofs_listxattr,
	.get_inode_acl = erofs_get_acl,
};
