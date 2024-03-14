// SPDX-License-Identifier: GPL-2.0-only
/*
 * This file contians vfs address (mmap) ops for 9P2000.
 *
 *  Copyright (C) 2005 by Eric Van Hensbergen <ericvh@gmail.com>
 *  Copyright (C) 2002 by Ron Minnich <rminnich@lanl.gov>
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/pagemap.h>
#include <linux/sched.h>
#include <linux/swap.h>
#include <linux/uio.h>
#include <linux/netfs.h>
#include <net/9p/9p.h>
#include <net/9p/client.h>

#include "v9fs.h"
#include "v9fs_vfs.h"
#include "cache.h"
#include "fid.h"

/**
 * v9fs_issue_read - Issue a read from 9P
 * @subreq: The read to make
 */
static void v9fs_issue_read(struct netfs_io_subrequest *subreq)
{
	struct netfs_io_request *rreq = subreq->rreq;
	struct p9_fid *fid = rreq->netfs_priv;
	struct iov_iter to;
	loff_t pos = subreq->start + subreq->transferred;
	size_t len = subreq->len   - subreq->transferred;
	int total, err;

	iov_iter_xarray(&to, ITER_DEST, &rreq->mapping->i_pages, pos, len);

	total = p9_client_read(fid, pos, &to, &err);

	/* if we just extended the file size, any portion not in
	 * cache won't be on server and is zeroes */
	__set_bit(NETFS_SREQ_CLEAR_TAIL, &subreq->flags);

	netfs_subreq_terminated(subreq, err ?: total, false);
}

/**
 * v9fs_init_request - Initialise a read request
 * @rreq: The read request
 * @file: The file being read from
 */
static int v9fs_init_request(struct netfs_io_request *rreq, struct file *file)
{
	struct p9_fid *fid = file->private_data;

	BUG_ON(!fid);

	/* we might need to read from a fid that was opened write-only
	 * for read-modify-write of page cache, use the writeback fid
	 * for that */
	WARN_ON(rreq->origin == NETFS_READ_FOR_WRITE &&
			!(fid->mode & P9_ORDWR));

	p9_fid_get(fid);
	rreq->netfs_priv = fid;
	return 0;
}

/**
 * v9fs_free_request - Cleanup request initialized by v9fs_init_rreq
 * @rreq: The I/O request to clean up
 */
static void v9fs_free_request(struct netfs_io_request *rreq)
{
	struct p9_fid *fid = rreq->netfs_priv;

	p9_fid_put(fid);
}

/**
 * v9fs_begin_cache_operation - Begin a cache operation for a read
 * @rreq: The read request
 */
static int v9fs_begin_cache_operation(struct netfs_io_request *rreq)
{
#ifdef CONFIG_9P_FSCACHE
	struct fscache_cookie *cookie = v9fs_inode_cookie(V9FS_I(rreq->inode));

	return fscache_begin_read_operation(&rreq->cache_resources, cookie);
#else
	return -ENOBUFS;
#endif
}

const struct netfs_request_ops v9fs_req_ops = {
	.init_request		= v9fs_init_request,
	.free_request		= v9fs_free_request,
	.begin_cache_operation	= v9fs_begin_cache_operation,
	.issue_read		= v9fs_issue_read,
};

/**
 * v9fs_release_folio - release the private state associated with a folio
 * @folio: The folio to be released
 * @gfp: The caller's allocation restrictions
 *
 * Returns true if the page can be released, false otherwise.
 */

static bool v9fs_release_folio(struct folio *folio, gfp_t gfp)
{
	if (folio_test_private(folio))
		return false;
#ifdef CONFIG_9P_FSCACHE
	if (folio_test_fscache(folio)) {
		if (current_is_kswapd() || !(gfp & __GFP_FS))
			return false;
		folio_wait_fscache(folio);
	}
	fscache_note_page_release(v9fs_inode_cookie(V9FS_I(folio_inode(folio))));
#endif
	return true;
}

static void v9fs_invalidate_folio(struct folio *folio, size_t offset,
				 size_t length)
{
	folio_wait_fscache(folio);
}

#ifdef CONFIG_9P_FSCACHE
static void v9fs_write_to_cache_done(void *priv, ssize_t transferred_or_error,
				     bool was_async)
{
	struct v9fs_inode *v9inode = priv;
	__le32 version;

	if (IS_ERR_VALUE(transferred_or_error) &&
	    transferred_or_error != -ENOBUFS) {
		version = cpu_to_le32(v9inode->qid.version);
		fscache_invalidate(v9fs_inode_cookie(v9inode), &version,
				   i_size_read(&v9inode->netfs.inode), 0);
	}
}
#endif

static int v9fs_vfs_write_folio_locked(struct folio *folio)
{
	struct inode *inode = folio_inode(folio);
	loff_t start = folio_pos(folio);
	loff_t i_size = i_size_read(inode);
	struct iov_iter from;
	size_t len = folio_size(folio);
	struct p9_fid *writeback_fid;
	int err;
	struct v9fs_inode __maybe_unused *v9inode = V9FS_I(inode);
	struct fscache_cookie __maybe_unused *cookie = v9fs_inode_cookie(v9inode);

	if (start >= i_size)
		return 0; /* Simultaneous truncation occurred */

	len = min_t(loff_t, i_size - start, len);

	iov_iter_xarray(&from, ITER_SOURCE, &folio_mapping(folio)->i_pages, start, len);

	writeback_fid = v9fs_fid_find_inode(inode, true, INVALID_UID, true);
	if (!writeback_fid) {
		WARN_ONCE(1, "folio expected an open fid inode->i_private=%p\n",
			inode->i_private);
		return -EINVAL;
	}

	folio_wait_fscache(folio);
	folio_start_writeback(folio);

	p9_client_write(writeback_fid, start, &from, &err);

#ifdef CONFIG_9P_FSCACHE
	if (err == 0 &&
		fscache_cookie_enabled(cookie) &&
		test_bit(FSCACHE_COOKIE_IS_CACHING, &cookie->flags)) {
		folio_start_fscache(folio);
		fscache_write_to_cache(v9fs_inode_cookie(v9inode),
					folio_mapping(folio), start, len, i_size,
					v9fs_write_to_cache_done, v9inode,
					true);
	}
#endif

	folio_end_writeback(folio);
	p9_fid_put(writeback_fid);

	return err;
}

static int v9fs_vfs_writepage(struct page *page, struct writeback_control *wbc)
{
	struct folio *folio = page_folio(page);
	int retval;

	p9_debug(P9_DEBUG_VFS, "folio %p\n", folio);

	retval = v9fs_vfs_write_folio_locked(folio);
	if (retval < 0) {
		if (retval == -EAGAIN) {
			folio_redirty_for_writepage(wbc, folio);
			retval = 0;
		} else {
			mapping_set_error(folio_mapping(folio), retval);
		}
	} else
		retval = 0;

	folio_unlock(folio);
	return retval;
}

static int v9fs_launder_folio(struct folio *folio)
{
	int retval;

	if (folio_clear_dirty_for_io(folio)) {
		retval = v9fs_vfs_write_folio_locked(folio);
		if (retval)
			return retval;
	}
	folio_wait_fscache(folio);
	return 0;
}

/**
 * v9fs_direct_IO - 9P address space operation for direct I/O
 * @iocb: target I/O control block
 * @iter: The data/buffer to use
 *
 * The presence of v9fs_direct_IO() in the address space ops vector
 * allowes open() O_DIRECT flags which would have failed otherwise.
 *
 * In the non-cached mode, we shunt off direct read and write requests before
 * the VFS gets them, so this method should never be called.
 *
 * Direct IO is not 'yet' supported in the cached mode. Hence when
 * this routine is called through generic_file_aio_read(), the read/write fails
 * with an error.
 *
 */
static ssize_t
v9fs_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	loff_t pos = iocb->ki_pos;
	ssize_t n;
	int err = 0;

	if (iov_iter_rw(iter) == WRITE) {
		n = p9_client_write(file->private_data, pos, iter, &err);
		if (n) {
			struct inode *inode = file_inode(file);
			loff_t i_size = i_size_read(inode);

			if (pos + n > i_size)
				inode_add_bytes(inode, pos + n - i_size);
		}
	} else {
		n = p9_client_read(file->private_data, pos, iter, &err);
	}
	return n ? n : err;
}

static int v9fs_write_begin(struct file *filp, struct address_space *mapping,
			    loff_t pos, unsigned int len,
			    struct page **subpagep, void **fsdata)
{
	int retval;
	struct folio *folio;
	struct v9fs_inode *v9inode = V9FS_I(mapping->host);

	p9_debug(P9_DEBUG_VFS, "filp %p, mapping %p\n", filp, mapping);

	/* Prefetch area to be written into the cache if we're caching this
	 * file.  We need to do this before we get a lock on the page in case
	 * there's more than one writer competing for the same cache block.
	 */
	retval = netfs_write_begin(&v9inode->netfs, filp, mapping, pos, len, &folio, fsdata);
	if (retval < 0)
		return retval;

	*subpagep = &folio->page;
	return retval;
}

static int v9fs_write_end(struct file *filp, struct address_space *mapping,
			  loff_t pos, unsigned int len, unsigned int copied,
			  struct page *subpage, void *fsdata)
{
	loff_t last_pos = pos + copied;
	struct folio *folio = page_folio(subpage);
	struct inode *inode = mapping->host;

	p9_debug(P9_DEBUG_VFS, "filp %p, mapping %p\n", filp, mapping);

	if (!folio_test_uptodate(folio)) {
		if (unlikely(copied < len)) {
			copied = 0;
			goto out;
		}

		folio_mark_uptodate(folio);
	}

	/*
	 * No need to use i_size_read() here, the i_size
	 * cannot change under us because we hold the i_mutex.
	 */
	if (last_pos > inode->i_size) {
		inode_add_bytes(inode, last_pos - inode->i_size);
		i_size_write(inode, last_pos);
#ifdef CONFIG_9P_FSCACHE
		fscache_update_cookie(v9fs_inode_cookie(V9FS_I(inode)), NULL,
			&last_pos);
#endif
	}
	folio_mark_dirty(folio);
out:
	folio_unlock(folio);
	folio_put(folio);

	return copied;
}

#ifdef CONFIG_9P_FSCACHE
/*
 * Mark a page as having been made dirty and thus needing writeback.  We also
 * need to pin the cache object to write back to.
 */
static bool v9fs_dirty_folio(struct address_space *mapping, struct folio *folio)
{
	struct v9fs_inode *v9inode = V9FS_I(mapping->host);

	return fscache_dirty_folio(mapping, folio, v9fs_inode_cookie(v9inode));
}
#else
#define v9fs_dirty_folio filemap_dirty_folio
#endif

const struct address_space_operations v9fs_addr_operations = {
	.read_folio = netfs_read_folio,
	.readahead = netfs_readahead,
	.dirty_folio = v9fs_dirty_folio,
	.writepage = v9fs_vfs_writepage,
	.write_begin = v9fs_write_begin,
	.write_end = v9fs_write_end,
	.release_folio = v9fs_release_folio,
	.invalidate_folio = v9fs_invalidate_folio,
	.launder_folio = v9fs_launder_folio,
	.direct_IO = v9fs_direct_IO,
};
