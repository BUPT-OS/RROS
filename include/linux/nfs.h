/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NFS protocol definitions
 *
 * This file contains constants mostly for Version 2 of the protocol,
 * but also has a couple of NFSv3 bits in (notably the error codes).
 */
#ifndef _LINUX_NFS_H
#define _LINUX_NFS_H

#include <linux/sunrpc/msg_prot.h>
#include <linux/string.h>
#include <linux/crc32.h>
#include <uapi/linux/nfs.h>

/*
 * This is the kernel NFS client file handle representation
 */
#define NFS_MAXFHSIZE		128
struct nfs_fh {
	unsigned short		size;
	unsigned char		data[NFS_MAXFHSIZE];
};

/*
 * Returns a zero iff the size and data fields match.
 * Checks only "size" bytes in the data field.
 */
static inline int nfs_compare_fh(const struct nfs_fh *a, const struct nfs_fh *b)
{
	return a->size != b->size || memcmp(a->data, b->data, a->size) != 0;
}

static inline void nfs_copy_fh(struct nfs_fh *target, const struct nfs_fh *source)
{
	target->size = source->size;
	memcpy(target->data, source->data, source->size);
}

enum nfs3_stable_how {
	NFS_UNSTABLE = 0,
	NFS_DATA_SYNC = 1,
	NFS_FILE_SYNC = 2,

	/* used by direct.c to mark verf as invalid */
	NFS_INVALID_STABLE_HOW = -1
};

#ifdef CONFIG_CRC32
/**
 * nfs_fhandle_hash - calculate the crc32 hash for the filehandle
 * @fh - pointer to filehandle
 *
 * returns a crc32 hash for the filehandle that is compatible with
 * the one displayed by "wireshark".
 */
static inline u32 nfs_fhandle_hash(const struct nfs_fh *fh)
{
	return ~crc32_le(0xFFFFFFFF, &fh->data[0], fh->size);
}
#else /* CONFIG_CRC32 */
static inline u32 nfs_fhandle_hash(const struct nfs_fh *fh)
{
	return 0;
}
#endif /* CONFIG_CRC32 */
#endif /* _LINUX_NFS_H */
