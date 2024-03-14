// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/ext4/mballoc.h
 *
 *  Written by: Alex Tomas <alex@clusterfs.com>
 *
 */
#ifndef _EXT4_MBALLOC_H
#define _EXT4_MBALLOC_H

#include <linux/time.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/quotaops.h>
#include <linux/buffer_head.h>
#include <linux/module.h>
#include <linux/swap.h>
#include <linux/proc_fs.h>
#include <linux/pagemap.h>
#include <linux/seq_file.h>
#include <linux/blkdev.h>
#include <linux/mutex.h>
#include "ext4_jbd2.h"
#include "ext4.h"

/*
 * mb_debug() dynamic printk msgs could be used to debug mballoc code.
 */
#ifdef CONFIG_EXT4_DEBUG
#define mb_debug(sb, fmt, ...)						\
	pr_debug("[%s/%d] EXT4-fs (%s): (%s, %d): %s: " fmt,		\
		current->comm, task_pid_nr(current), sb->s_id,		\
	       __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#else
#define mb_debug(sb, fmt, ...)	no_printk(fmt, ##__VA_ARGS__)
#endif

#define EXT4_MB_HISTORY_ALLOC		1	/* allocation */
#define EXT4_MB_HISTORY_PREALLOC	2	/* preallocated blocks used */

/*
 * How long mballoc can look for a best extent (in found extents)
 */
#define MB_DEFAULT_MAX_TO_SCAN		200

/*
 * How long mballoc must look for a best extent
 */
#define MB_DEFAULT_MIN_TO_SCAN		10

/*
 * with 's_mb_stats' allocator will collect stats that will be
 * shown at umount. The collecting costs though!
 */
#define MB_DEFAULT_STATS		0

/*
 * files smaller than MB_DEFAULT_STREAM_THRESHOLD are served
 * by the stream allocator, which purpose is to pack requests
 * as close each to other as possible to produce smooth I/O traffic
 * We use locality group prealloc space for stream request.
 * We can tune the same via /proc/fs/ext4/<partition>/stream_req
 */
#define MB_DEFAULT_STREAM_THRESHOLD	16	/* 64K */

/*
 * for which requests use 2^N search using buddies
 */
#define MB_DEFAULT_ORDER2_REQS		2

/*
 * default group prealloc size 512 blocks
 */
#define MB_DEFAULT_GROUP_PREALLOC	512

/*
 * Number of groups to search linearly before performing group scanning
 * optimization.
 */
#define MB_DEFAULT_LINEAR_LIMIT		4

/*
 * Minimum number of groups that should be present in the file system to perform
 * group scanning optimizations.
 */
#define MB_DEFAULT_LINEAR_SCAN_THRESHOLD	16

/*
 * The maximum order upto which CR_BEST_AVAIL_LEN can trim a particular
 * allocation request. Example, if we have an order 7 request and max trim order
 * of 3, we can trim this request upto order 4.
 */
#define MB_DEFAULT_BEST_AVAIL_TRIM_ORDER	3

/*
 * Number of valid buddy orders
 */
#define MB_NUM_ORDERS(sb)		((sb)->s_blocksize_bits + 2)

struct ext4_free_data {
	/* this links the free block information from sb_info */
	struct list_head		efd_list;

	/* this links the free block information from group_info */
	struct rb_node			efd_node;

	/* group which free block extent belongs */
	ext4_group_t			efd_group;

	/* free block extent */
	ext4_grpblk_t			efd_start_cluster;
	ext4_grpblk_t			efd_count;

	/* transaction which freed this extent */
	tid_t				efd_tid;
};

struct ext4_prealloc_space {
	union {
		struct rb_node	inode_node;		/* for inode PA rbtree */
		struct list_head	lg_list;	/* for lg PAs */
	} pa_node;
	struct list_head	pa_group_list;
	union {
		struct list_head pa_tmp_list;
		struct rcu_head	pa_rcu;
	} u;
	spinlock_t		pa_lock;
	atomic_t		pa_count;
	unsigned		pa_deleted;
	ext4_fsblk_t		pa_pstart;	/* phys. block */
	ext4_lblk_t		pa_lstart;	/* log. block */
	ext4_grpblk_t		pa_len;		/* len of preallocated chunk */
	ext4_grpblk_t		pa_free;	/* how many blocks are free */
	unsigned short		pa_type;	/* pa type. inode or group */
	union {
		rwlock_t		*inode_lock;	/* locks the rbtree holding this PA */
		spinlock_t		*lg_lock;	/* locks the lg list holding this PA */
	} pa_node_lock;
	struct inode		*pa_inode;	/* used to get the inode during group discard */
};

enum {
	MB_INODE_PA = 0,
	MB_GROUP_PA = 1
};

struct ext4_free_extent {
	ext4_lblk_t fe_logical;
	ext4_grpblk_t fe_start;	/* In cluster units */
	ext4_group_t fe_group;
	ext4_grpblk_t fe_len;	/* In cluster units */
};

/*
 * Locality group:
 *   we try to group all related changes together
 *   so that writeback can flush/allocate them together as well
 *   Size of lg_prealloc_list hash is determined by MB_DEFAULT_GROUP_PREALLOC
 *   (512). We store prealloc space into the hash based on the pa_free blocks
 *   order value.ie, fls(pa_free)-1;
 */
#define PREALLOC_TB_SIZE 10
struct ext4_locality_group {
	/* for allocator */
	/* to serialize allocates */
	struct mutex		lg_mutex;
	/* list of preallocations */
	struct list_head	lg_prealloc_list[PREALLOC_TB_SIZE];
	spinlock_t		lg_prealloc_lock;
};

struct ext4_allocation_context {
	struct inode *ac_inode;
	struct super_block *ac_sb;

	/* original request */
	struct ext4_free_extent ac_o_ex;

	/* goal request (normalized ac_o_ex) */
	struct ext4_free_extent ac_g_ex;

	/* the best found extent */
	struct ext4_free_extent ac_b_ex;

	/* copy of the best found extent taken before preallocation efforts */
	struct ext4_free_extent ac_f_ex;

	/*
	 * goal len can change in CR1.5, so save the original len. This is
	 * used while adjusting the PA window and for accounting.
	 */
	ext4_grpblk_t	ac_orig_goal_len;

	__u32 ac_groups_considered;
	__u32 ac_flags;		/* allocation hints */
	__u16 ac_groups_scanned;
	__u16 ac_groups_linear_remaining;
	__u16 ac_found;
	__u16 ac_cX_found[EXT4_MB_NUM_CRS];
	__u16 ac_tail;
	__u16 ac_buddy;
	__u8 ac_status;
	__u8 ac_criteria;
	__u8 ac_2order;		/* if request is to allocate 2^N blocks and
				 * N > 0, the field stores N, otherwise 0 */
	__u8 ac_op;		/* operation, for history only */
	struct page *ac_bitmap_page;
	struct page *ac_buddy_page;
	struct ext4_prealloc_space *ac_pa;
	struct ext4_locality_group *ac_lg;
};

#define AC_STATUS_CONTINUE	1
#define AC_STATUS_FOUND		2
#define AC_STATUS_BREAK		3

struct ext4_buddy {
	struct page *bd_buddy_page;
	void *bd_buddy;
	struct page *bd_bitmap_page;
	void *bd_bitmap;
	struct ext4_group_info *bd_info;
	struct super_block *bd_sb;
	__u16 bd_blkbits;
	ext4_group_t bd_group;
};

static inline ext4_fsblk_t ext4_grp_offs_to_block(struct super_block *sb,
					struct ext4_free_extent *fex)
{
	return ext4_group_first_block_no(sb, fex->fe_group) +
		(fex->fe_start << EXT4_SB(sb)->s_cluster_bits);
}

static inline loff_t extent_logical_end(struct ext4_sb_info *sbi,
					struct ext4_free_extent *fex)
{
	/* Use loff_t to avoid end exceeding ext4_lblk_t max. */
	return (loff_t)fex->fe_logical + EXT4_C2B(sbi, fex->fe_len);
}

static inline loff_t pa_logical_end(struct ext4_sb_info *sbi,
				    struct ext4_prealloc_space *pa)
{
	/* Use loff_t to avoid end exceeding ext4_lblk_t max. */
	return (loff_t)pa->pa_lstart + EXT4_C2B(sbi, pa->pa_len);
}

typedef int (*ext4_mballoc_query_range_fn)(
	struct super_block		*sb,
	ext4_group_t			agno,
	ext4_grpblk_t			start,
	ext4_grpblk_t			len,
	void				*priv);

int
ext4_mballoc_query_range(
	struct super_block		*sb,
	ext4_group_t			agno,
	ext4_grpblk_t			start,
	ext4_grpblk_t			end,
	ext4_mballoc_query_range_fn	formatter,
	void				*priv);

#endif
