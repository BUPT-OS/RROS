// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2003-2006, Cluster File Systems, Inc, info@clusterfs.com
 * Written by Alex Tomas <alex@clusterfs.com>
 */


/*
 * mballoc.c contains the multiblocks allocation routines
 */

#include "ext4_jbd2.h"
#include "mballoc.h"
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/nospec.h>
#include <linux/backing-dev.h>
#include <linux/freezer.h>
#include <trace/events/ext4.h>

/*
 * MUSTDO:
 *   - test ext4_ext_search_left() and ext4_ext_search_right()
 *   - search for metadata in few groups
 *
 * TODO v4:
 *   - normalization should take into account whether file is still open
 *   - discard preallocations if no free space left (policy?)
 *   - don't normalize tails
 *   - quota
 *   - reservation for superuser
 *
 * TODO v3:
 *   - bitmap read-ahead (proposed by Oleg Drokin aka green)
 *   - track min/max extents in each group for better group selection
 *   - mb_mark_used() may allocate chunk right after splitting buddy
 *   - tree of groups sorted by number of free blocks
 *   - error handling
 */

/*
 * The allocation request involve request for multiple number of blocks
 * near to the goal(block) value specified.
 *
 * During initialization phase of the allocator we decide to use the
 * group preallocation or inode preallocation depending on the size of
 * the file. The size of the file could be the resulting file size we
 * would have after allocation, or the current file size, which ever
 * is larger. If the size is less than sbi->s_mb_stream_request we
 * select to use the group preallocation. The default value of
 * s_mb_stream_request is 16 blocks. This can also be tuned via
 * /sys/fs/ext4/<partition>/mb_stream_req. The value is represented in
 * terms of number of blocks.
 *
 * The main motivation for having small file use group preallocation is to
 * ensure that we have small files closer together on the disk.
 *
 * First stage the allocator looks at the inode prealloc list,
 * ext4_inode_info->i_prealloc_list, which contains list of prealloc
 * spaces for this particular inode. The inode prealloc space is
 * represented as:
 *
 * pa_lstart -> the logical start block for this prealloc space
 * pa_pstart -> the physical start block for this prealloc space
 * pa_len    -> length for this prealloc space (in clusters)
 * pa_free   ->  free space available in this prealloc space (in clusters)
 *
 * The inode preallocation space is used looking at the _logical_ start
 * block. If only the logical file block falls within the range of prealloc
 * space we will consume the particular prealloc space. This makes sure that
 * we have contiguous physical blocks representing the file blocks
 *
 * The important thing to be noted in case of inode prealloc space is that
 * we don't modify the values associated to inode prealloc space except
 * pa_free.
 *
 * If we are not able to find blocks in the inode prealloc space and if we
 * have the group allocation flag set then we look at the locality group
 * prealloc space. These are per CPU prealloc list represented as
 *
 * ext4_sb_info.s_locality_groups[smp_processor_id()]
 *
 * The reason for having a per cpu locality group is to reduce the contention
 * between CPUs. It is possible to get scheduled at this point.
 *
 * The locality group prealloc space is used looking at whether we have
 * enough free space (pa_free) within the prealloc space.
 *
 * If we can't allocate blocks via inode prealloc or/and locality group
 * prealloc then we look at the buddy cache. The buddy cache is represented
 * by ext4_sb_info.s_buddy_cache (struct inode) whose file offset gets
 * mapped to the buddy and bitmap information regarding different
 * groups. The buddy information is attached to buddy cache inode so that
 * we can access them through the page cache. The information regarding
 * each group is loaded via ext4_mb_load_buddy.  The information involve
 * block bitmap and buddy information. The information are stored in the
 * inode as:
 *
 *  {                        page                        }
 *  [ group 0 bitmap][ group 0 buddy] [group 1][ group 1]...
 *
 *
 * one block each for bitmap and buddy information.  So for each group we
 * take up 2 blocks. A page can contain blocks_per_page (PAGE_SIZE /
 * blocksize) blocks.  So it can have information regarding groups_per_page
 * which is blocks_per_page/2
 *
 * The buddy cache inode is not stored on disk. The inode is thrown
 * away when the filesystem is unmounted.
 *
 * We look for count number of blocks in the buddy cache. If we were able
 * to locate that many free blocks we return with additional information
 * regarding rest of the contiguous physical block available
 *
 * Before allocating blocks via buddy cache we normalize the request
 * blocks. This ensure we ask for more blocks that we needed. The extra
 * blocks that we get after allocation is added to the respective prealloc
 * list. In case of inode preallocation we follow a list of heuristics
 * based on file size. This can be found in ext4_mb_normalize_request. If
 * we are doing a group prealloc we try to normalize the request to
 * sbi->s_mb_group_prealloc.  The default value of s_mb_group_prealloc is
 * dependent on the cluster size; for non-bigalloc file systems, it is
 * 512 blocks. This can be tuned via
 * /sys/fs/ext4/<partition>/mb_group_prealloc. The value is represented in
 * terms of number of blocks. If we have mounted the file system with -O
 * stripe=<value> option the group prealloc request is normalized to the
 * smallest multiple of the stripe value (sbi->s_stripe) which is
 * greater than the default mb_group_prealloc.
 *
 * If "mb_optimize_scan" mount option is set, we maintain in memory group info
 * structures in two data structures:
 *
 * 1) Array of largest free order lists (sbi->s_mb_largest_free_orders)
 *
 *    Locking: sbi->s_mb_largest_free_orders_locks(array of rw locks)
 *
 *    This is an array of lists where the index in the array represents the
 *    largest free order in the buddy bitmap of the participating group infos of
 *    that list. So, there are exactly MB_NUM_ORDERS(sb) (which means total
 *    number of buddy bitmap orders possible) number of lists. Group-infos are
 *    placed in appropriate lists.
 *
 * 2) Average fragment size lists (sbi->s_mb_avg_fragment_size)
 *
 *    Locking: sbi->s_mb_avg_fragment_size_locks(array of rw locks)
 *
 *    This is an array of lists where in the i-th list there are groups with
 *    average fragment size >= 2^i and < 2^(i+1). The average fragment size
 *    is computed as ext4_group_info->bb_free / ext4_group_info->bb_fragments.
 *    Note that we don't bother with a special list for completely empty groups
 *    so we only have MB_NUM_ORDERS(sb) lists.
 *
 * When "mb_optimize_scan" mount option is set, mballoc consults the above data
 * structures to decide the order in which groups are to be traversed for
 * fulfilling an allocation request.
 *
 * At CR_POWER2_ALIGNED , we look for groups which have the largest_free_order
 * >= the order of the request. We directly look at the largest free order list
 * in the data structure (1) above where largest_free_order = order of the
 * request. If that list is empty, we look at remaining list in the increasing
 * order of largest_free_order. This allows us to perform CR_POWER2_ALIGNED
 * lookup in O(1) time.
 *
 * At CR_GOAL_LEN_FAST, we only consider groups where
 * average fragment size > request size. So, we lookup a group which has average
 * fragment size just above or equal to request size using our average fragment
 * size group lists (data structure 2) in O(1) time.
 *
 * At CR_BEST_AVAIL_LEN, we aim to optimize allocations which can't be satisfied
 * in CR_GOAL_LEN_FAST. The fact that we couldn't find a group in
 * CR_GOAL_LEN_FAST suggests that there is no BG that has avg
 * fragment size > goal length. So before falling to the slower
 * CR_GOAL_LEN_SLOW, in CR_BEST_AVAIL_LEN we proactively trim goal length and
 * then use the same fragment lists as CR_GOAL_LEN_FAST to find a BG with a big
 * enough average fragment size. This increases the chances of finding a
 * suitable block group in O(1) time and results in faster allocation at the
 * cost of reduced size of allocation.
 *
 * If "mb_optimize_scan" mount option is not set, mballoc traverses groups in
 * linear order which requires O(N) search time for each CR_POWER2_ALIGNED and
 * CR_GOAL_LEN_FAST phase.
 *
 * The regular allocator (using the buddy cache) supports a few tunables.
 *
 * /sys/fs/ext4/<partition>/mb_min_to_scan
 * /sys/fs/ext4/<partition>/mb_max_to_scan
 * /sys/fs/ext4/<partition>/mb_order2_req
 * /sys/fs/ext4/<partition>/mb_linear_limit
 *
 * The regular allocator uses buddy scan only if the request len is power of
 * 2 blocks and the order of allocation is >= sbi->s_mb_order2_reqs. The
 * value of s_mb_order2_reqs can be tuned via
 * /sys/fs/ext4/<partition>/mb_order2_req.  If the request len is equal to
 * stripe size (sbi->s_stripe), we try to search for contiguous block in
 * stripe size. This should result in better allocation on RAID setups. If
 * not, we search in the specific group using bitmap for best extents. The
 * tunable min_to_scan and max_to_scan control the behaviour here.
 * min_to_scan indicate how long the mballoc __must__ look for a best
 * extent and max_to_scan indicates how long the mballoc __can__ look for a
 * best extent in the found extents. Searching for the blocks starts with
 * the group specified as the goal value in allocation context via
 * ac_g_ex. Each group is first checked based on the criteria whether it
 * can be used for allocation. ext4_mb_good_group explains how the groups are
 * checked.
 *
 * When "mb_optimize_scan" is turned on, as mentioned above, the groups may not
 * get traversed linearly. That may result in subsequent allocations being not
 * close to each other. And so, the underlying device may get filled up in a
 * non-linear fashion. While that may not matter on non-rotational devices, for
 * rotational devices that may result in higher seek times. "mb_linear_limit"
 * tells mballoc how many groups mballoc should search linearly before
 * performing consulting above data structures for more efficient lookups. For
 * non rotational devices, this value defaults to 0 and for rotational devices
 * this is set to MB_DEFAULT_LINEAR_LIMIT.
 *
 * Both the prealloc space are getting populated as above. So for the first
 * request we will hit the buddy cache which will result in this prealloc
 * space getting filled. The prealloc space is then later used for the
 * subsequent request.
 */

/*
 * mballoc operates on the following data:
 *  - on-disk bitmap
 *  - in-core buddy (actually includes buddy and bitmap)
 *  - preallocation descriptors (PAs)
 *
 * there are two types of preallocations:
 *  - inode
 *    assiged to specific inode and can be used for this inode only.
 *    it describes part of inode's space preallocated to specific
 *    physical blocks. any block from that preallocated can be used
 *    independent. the descriptor just tracks number of blocks left
 *    unused. so, before taking some block from descriptor, one must
 *    make sure corresponded logical block isn't allocated yet. this
 *    also means that freeing any block within descriptor's range
 *    must discard all preallocated blocks.
 *  - locality group
 *    assigned to specific locality group which does not translate to
 *    permanent set of inodes: inode can join and leave group. space
 *    from this type of preallocation can be used for any inode. thus
 *    it's consumed from the beginning to the end.
 *
 * relation between them can be expressed as:
 *    in-core buddy = on-disk bitmap + preallocation descriptors
 *
 * this mean blocks mballoc considers used are:
 *  - allocated blocks (persistent)
 *  - preallocated blocks (non-persistent)
 *
 * consistency in mballoc world means that at any time a block is either
 * free or used in ALL structures. notice: "any time" should not be read
 * literally -- time is discrete and delimited by locks.
 *
 *  to keep it simple, we don't use block numbers, instead we count number of
 *  blocks: how many blocks marked used/free in on-disk bitmap, buddy and PA.
 *
 * all operations can be expressed as:
 *  - init buddy:			buddy = on-disk + PAs
 *  - new PA:				buddy += N; PA = N
 *  - use inode PA:			on-disk += N; PA -= N
 *  - discard inode PA			buddy -= on-disk - PA; PA = 0
 *  - use locality group PA		on-disk += N; PA -= N
 *  - discard locality group PA		buddy -= PA; PA = 0
 *  note: 'buddy -= on-disk - PA' is used to show that on-disk bitmap
 *        is used in real operation because we can't know actual used
 *        bits from PA, only from on-disk bitmap
 *
 * if we follow this strict logic, then all operations above should be atomic.
 * given some of them can block, we'd have to use something like semaphores
 * killing performance on high-end SMP hardware. let's try to relax it using
 * the following knowledge:
 *  1) if buddy is referenced, it's already initialized
 *  2) while block is used in buddy and the buddy is referenced,
 *     nobody can re-allocate that block
 *  3) we work on bitmaps and '+' actually means 'set bits'. if on-disk has
 *     bit set and PA claims same block, it's OK. IOW, one can set bit in
 *     on-disk bitmap if buddy has same bit set or/and PA covers corresponded
 *     block
 *
 * so, now we're building a concurrency table:
 *  - init buddy vs.
 *    - new PA
 *      blocks for PA are allocated in the buddy, buddy must be referenced
 *      until PA is linked to allocation group to avoid concurrent buddy init
 *    - use inode PA
 *      we need to make sure that either on-disk bitmap or PA has uptodate data
 *      given (3) we care that PA-=N operation doesn't interfere with init
 *    - discard inode PA
 *      the simplest way would be to have buddy initialized by the discard
 *    - use locality group PA
 *      again PA-=N must be serialized with init
 *    - discard locality group PA
 *      the simplest way would be to have buddy initialized by the discard
 *  - new PA vs.
 *    - use inode PA
 *      i_data_sem serializes them
 *    - discard inode PA
 *      discard process must wait until PA isn't used by another process
 *    - use locality group PA
 *      some mutex should serialize them
 *    - discard locality group PA
 *      discard process must wait until PA isn't used by another process
 *  - use inode PA
 *    - use inode PA
 *      i_data_sem or another mutex should serializes them
 *    - discard inode PA
 *      discard process must wait until PA isn't used by another process
 *    - use locality group PA
 *      nothing wrong here -- they're different PAs covering different blocks
 *    - discard locality group PA
 *      discard process must wait until PA isn't used by another process
 *
 * now we're ready to make few consequences:
 *  - PA is referenced and while it is no discard is possible
 *  - PA is referenced until block isn't marked in on-disk bitmap
 *  - PA changes only after on-disk bitmap
 *  - discard must not compete with init. either init is done before
 *    any discard or they're serialized somehow
 *  - buddy init as sum of on-disk bitmap and PAs is done atomically
 *
 * a special case when we've used PA to emptiness. no need to modify buddy
 * in this case, but we should care about concurrent init
 *
 */

 /*
 * Logic in few words:
 *
 *  - allocation:
 *    load group
 *    find blocks
 *    mark bits in on-disk bitmap
 *    release group
 *
 *  - use preallocation:
 *    find proper PA (per-inode or group)
 *    load group
 *    mark bits in on-disk bitmap
 *    release group
 *    release PA
 *
 *  - free:
 *    load group
 *    mark bits in on-disk bitmap
 *    release group
 *
 *  - discard preallocations in group:
 *    mark PAs deleted
 *    move them onto local list
 *    load on-disk bitmap
 *    load group
 *    remove PA from object (inode or locality group)
 *    mark free blocks in-core
 *
 *  - discard inode's preallocations:
 */

/*
 * Locking rules
 *
 * Locks:
 *  - bitlock on a group	(group)
 *  - object (inode/locality)	(object)
 *  - per-pa lock		(pa)
 *  - cr_power2_aligned lists lock	(cr_power2_aligned)
 *  - cr_goal_len_fast lists lock	(cr_goal_len_fast)
 *
 * Paths:
 *  - new pa
 *    object
 *    group
 *
 *  - find and use pa:
 *    pa
 *
 *  - release consumed pa:
 *    pa
 *    group
 *    object
 *
 *  - generate in-core bitmap:
 *    group
 *        pa
 *
 *  - discard all for given object (inode, locality group):
 *    object
 *        pa
 *    group
 *
 *  - discard all for given group:
 *    group
 *        pa
 *    group
 *        object
 *
 *  - allocation path (ext4_mb_regular_allocator)
 *    group
 *    cr_power2_aligned/cr_goal_len_fast
 */
static struct kmem_cache *ext4_pspace_cachep;
static struct kmem_cache *ext4_ac_cachep;
static struct kmem_cache *ext4_free_data_cachep;

/* We create slab caches for groupinfo data structures based on the
 * superblock block size.  There will be one per mounted filesystem for
 * each unique s_blocksize_bits */
#define NR_GRPINFO_CACHES 8
static struct kmem_cache *ext4_groupinfo_caches[NR_GRPINFO_CACHES];

static const char * const ext4_groupinfo_slab_names[NR_GRPINFO_CACHES] = {
	"ext4_groupinfo_1k", "ext4_groupinfo_2k", "ext4_groupinfo_4k",
	"ext4_groupinfo_8k", "ext4_groupinfo_16k", "ext4_groupinfo_32k",
	"ext4_groupinfo_64k", "ext4_groupinfo_128k"
};

static void ext4_mb_generate_from_pa(struct super_block *sb, void *bitmap,
					ext4_group_t group);
static void ext4_mb_generate_from_freelist(struct super_block *sb, void *bitmap,
						ext4_group_t group);
static void ext4_mb_new_preallocation(struct ext4_allocation_context *ac);

static bool ext4_mb_good_group(struct ext4_allocation_context *ac,
			       ext4_group_t group, enum criteria cr);

static int ext4_try_to_trim_range(struct super_block *sb,
		struct ext4_buddy *e4b, ext4_grpblk_t start,
		ext4_grpblk_t max, ext4_grpblk_t minblocks);

/*
 * The algorithm using this percpu seq counter goes below:
 * 1. We sample the percpu discard_pa_seq counter before trying for block
 *    allocation in ext4_mb_new_blocks().
 * 2. We increment this percpu discard_pa_seq counter when we either allocate
 *    or free these blocks i.e. while marking those blocks as used/free in
 *    mb_mark_used()/mb_free_blocks().
 * 3. We also increment this percpu seq counter when we successfully identify
 *    that the bb_prealloc_list is not empty and hence proceed for discarding
 *    of those PAs inside ext4_mb_discard_group_preallocations().
 *
 * Now to make sure that the regular fast path of block allocation is not
 * affected, as a small optimization we only sample the percpu seq counter
 * on that cpu. Only when the block allocation fails and when freed blocks
 * found were 0, that is when we sample percpu seq counter for all cpus using
 * below function ext4_get_discard_pa_seq_sum(). This happens after making
 * sure that all the PAs on grp->bb_prealloc_list got freed or if it's empty.
 */
static DEFINE_PER_CPU(u64, discard_pa_seq);
static inline u64 ext4_get_discard_pa_seq_sum(void)
{
	int __cpu;
	u64 __seq = 0;

	for_each_possible_cpu(__cpu)
		__seq += per_cpu(discard_pa_seq, __cpu);
	return __seq;
}

static inline void *mb_correct_addr_and_bit(int *bit, void *addr)
{
#if BITS_PER_LONG == 64
	*bit += ((unsigned long) addr & 7UL) << 3;
	addr = (void *) ((unsigned long) addr & ~7UL);
#elif BITS_PER_LONG == 32
	*bit += ((unsigned long) addr & 3UL) << 3;
	addr = (void *) ((unsigned long) addr & ~3UL);
#else
#error "how many bits you are?!"
#endif
	return addr;
}

static inline int mb_test_bit(int bit, void *addr)
{
	/*
	 * ext4_test_bit on architecture like powerpc
	 * needs unsigned long aligned address
	 */
	addr = mb_correct_addr_and_bit(&bit, addr);
	return ext4_test_bit(bit, addr);
}

static inline void mb_set_bit(int bit, void *addr)
{
	addr = mb_correct_addr_and_bit(&bit, addr);
	ext4_set_bit(bit, addr);
}

static inline void mb_clear_bit(int bit, void *addr)
{
	addr = mb_correct_addr_and_bit(&bit, addr);
	ext4_clear_bit(bit, addr);
}

static inline int mb_test_and_clear_bit(int bit, void *addr)
{
	addr = mb_correct_addr_and_bit(&bit, addr);
	return ext4_test_and_clear_bit(bit, addr);
}

static inline int mb_find_next_zero_bit(void *addr, int max, int start)
{
	int fix = 0, ret, tmpmax;
	addr = mb_correct_addr_and_bit(&fix, addr);
	tmpmax = max + fix;
	start += fix;

	ret = ext4_find_next_zero_bit(addr, tmpmax, start) - fix;
	if (ret > max)
		return max;
	return ret;
}

static inline int mb_find_next_bit(void *addr, int max, int start)
{
	int fix = 0, ret, tmpmax;
	addr = mb_correct_addr_and_bit(&fix, addr);
	tmpmax = max + fix;
	start += fix;

	ret = ext4_find_next_bit(addr, tmpmax, start) - fix;
	if (ret > max)
		return max;
	return ret;
}

static void *mb_find_buddy(struct ext4_buddy *e4b, int order, int *max)
{
	char *bb;

	BUG_ON(e4b->bd_bitmap == e4b->bd_buddy);
	BUG_ON(max == NULL);

	if (order > e4b->bd_blkbits + 1) {
		*max = 0;
		return NULL;
	}

	/* at order 0 we see each particular block */
	if (order == 0) {
		*max = 1 << (e4b->bd_blkbits + 3);
		return e4b->bd_bitmap;
	}

	bb = e4b->bd_buddy + EXT4_SB(e4b->bd_sb)->s_mb_offsets[order];
	*max = EXT4_SB(e4b->bd_sb)->s_mb_maxs[order];

	return bb;
}

#ifdef DOUBLE_CHECK
static void mb_free_blocks_double(struct inode *inode, struct ext4_buddy *e4b,
			   int first, int count)
{
	int i;
	struct super_block *sb = e4b->bd_sb;

	if (unlikely(e4b->bd_info->bb_bitmap == NULL))
		return;
	assert_spin_locked(ext4_group_lock_ptr(sb, e4b->bd_group));
	for (i = 0; i < count; i++) {
		if (!mb_test_bit(first + i, e4b->bd_info->bb_bitmap)) {
			ext4_fsblk_t blocknr;

			blocknr = ext4_group_first_block_no(sb, e4b->bd_group);
			blocknr += EXT4_C2B(EXT4_SB(sb), first + i);
			ext4_grp_locked_error(sb, e4b->bd_group,
					      inode ? inode->i_ino : 0,
					      blocknr,
					      "freeing block already freed "
					      "(bit %u)",
					      first + i);
			ext4_mark_group_bitmap_corrupted(sb, e4b->bd_group,
					EXT4_GROUP_INFO_BBITMAP_CORRUPT);
		}
		mb_clear_bit(first + i, e4b->bd_info->bb_bitmap);
	}
}

static void mb_mark_used_double(struct ext4_buddy *e4b, int first, int count)
{
	int i;

	if (unlikely(e4b->bd_info->bb_bitmap == NULL))
		return;
	assert_spin_locked(ext4_group_lock_ptr(e4b->bd_sb, e4b->bd_group));
	for (i = 0; i < count; i++) {
		BUG_ON(mb_test_bit(first + i, e4b->bd_info->bb_bitmap));
		mb_set_bit(first + i, e4b->bd_info->bb_bitmap);
	}
}

static void mb_cmp_bitmaps(struct ext4_buddy *e4b, void *bitmap)
{
	if (unlikely(e4b->bd_info->bb_bitmap == NULL))
		return;
	if (memcmp(e4b->bd_info->bb_bitmap, bitmap, e4b->bd_sb->s_blocksize)) {
		unsigned char *b1, *b2;
		int i;
		b1 = (unsigned char *) e4b->bd_info->bb_bitmap;
		b2 = (unsigned char *) bitmap;
		for (i = 0; i < e4b->bd_sb->s_blocksize; i++) {
			if (b1[i] != b2[i]) {
				ext4_msg(e4b->bd_sb, KERN_ERR,
					 "corruption in group %u "
					 "at byte %u(%u): %x in copy != %x "
					 "on disk/prealloc",
					 e4b->bd_group, i, i * 8, b1[i], b2[i]);
				BUG();
			}
		}
	}
}

static void mb_group_bb_bitmap_alloc(struct super_block *sb,
			struct ext4_group_info *grp, ext4_group_t group)
{
	struct buffer_head *bh;

	grp->bb_bitmap = kmalloc(sb->s_blocksize, GFP_NOFS);
	if (!grp->bb_bitmap)
		return;

	bh = ext4_read_block_bitmap(sb, group);
	if (IS_ERR_OR_NULL(bh)) {
		kfree(grp->bb_bitmap);
		grp->bb_bitmap = NULL;
		return;
	}

	memcpy(grp->bb_bitmap, bh->b_data, sb->s_blocksize);
	put_bh(bh);
}

static void mb_group_bb_bitmap_free(struct ext4_group_info *grp)
{
	kfree(grp->bb_bitmap);
}

#else
static inline void mb_free_blocks_double(struct inode *inode,
				struct ext4_buddy *e4b, int first, int count)
{
	return;
}
static inline void mb_mark_used_double(struct ext4_buddy *e4b,
						int first, int count)
{
	return;
}
static inline void mb_cmp_bitmaps(struct ext4_buddy *e4b, void *bitmap)
{
	return;
}

static inline void mb_group_bb_bitmap_alloc(struct super_block *sb,
			struct ext4_group_info *grp, ext4_group_t group)
{
	return;
}

static inline void mb_group_bb_bitmap_free(struct ext4_group_info *grp)
{
	return;
}
#endif

#ifdef AGGRESSIVE_CHECK

#define MB_CHECK_ASSERT(assert)						\
do {									\
	if (!(assert)) {						\
		printk(KERN_EMERG					\
			"Assertion failure in %s() at %s:%d: \"%s\"\n",	\
			function, file, line, # assert);		\
		BUG();							\
	}								\
} while (0)

static int __mb_check_buddy(struct ext4_buddy *e4b, char *file,
				const char *function, int line)
{
	struct super_block *sb = e4b->bd_sb;
	int order = e4b->bd_blkbits + 1;
	int max;
	int max2;
	int i;
	int j;
	int k;
	int count;
	struct ext4_group_info *grp;
	int fragments = 0;
	int fstart;
	struct list_head *cur;
	void *buddy;
	void *buddy2;

	if (e4b->bd_info->bb_check_counter++ % 10)
		return 0;

	while (order > 1) {
		buddy = mb_find_buddy(e4b, order, &max);
		MB_CHECK_ASSERT(buddy);
		buddy2 = mb_find_buddy(e4b, order - 1, &max2);
		MB_CHECK_ASSERT(buddy2);
		MB_CHECK_ASSERT(buddy != buddy2);
		MB_CHECK_ASSERT(max * 2 == max2);

		count = 0;
		for (i = 0; i < max; i++) {

			if (mb_test_bit(i, buddy)) {
				/* only single bit in buddy2 may be 0 */
				if (!mb_test_bit(i << 1, buddy2)) {
					MB_CHECK_ASSERT(
						mb_test_bit((i<<1)+1, buddy2));
				}
				continue;
			}

			/* both bits in buddy2 must be 1 */
			MB_CHECK_ASSERT(mb_test_bit(i << 1, buddy2));
			MB_CHECK_ASSERT(mb_test_bit((i << 1) + 1, buddy2));

			for (j = 0; j < (1 << order); j++) {
				k = (i * (1 << order)) + j;
				MB_CHECK_ASSERT(
					!mb_test_bit(k, e4b->bd_bitmap));
			}
			count++;
		}
		MB_CHECK_ASSERT(e4b->bd_info->bb_counters[order] == count);
		order--;
	}

	fstart = -1;
	buddy = mb_find_buddy(e4b, 0, &max);
	for (i = 0; i < max; i++) {
		if (!mb_test_bit(i, buddy)) {
			MB_CHECK_ASSERT(i >= e4b->bd_info->bb_first_free);
			if (fstart == -1) {
				fragments++;
				fstart = i;
			}
			continue;
		}
		fstart = -1;
		/* check used bits only */
		for (j = 0; j < e4b->bd_blkbits + 1; j++) {
			buddy2 = mb_find_buddy(e4b, j, &max2);
			k = i >> j;
			MB_CHECK_ASSERT(k < max2);
			MB_CHECK_ASSERT(mb_test_bit(k, buddy2));
		}
	}
	MB_CHECK_ASSERT(!EXT4_MB_GRP_NEED_INIT(e4b->bd_info));
	MB_CHECK_ASSERT(e4b->bd_info->bb_fragments == fragments);

	grp = ext4_get_group_info(sb, e4b->bd_group);
	if (!grp)
		return NULL;
	list_for_each(cur, &grp->bb_prealloc_list) {
		ext4_group_t groupnr;
		struct ext4_prealloc_space *pa;
		pa = list_entry(cur, struct ext4_prealloc_space, pa_group_list);
		ext4_get_group_no_and_offset(sb, pa->pa_pstart, &groupnr, &k);
		MB_CHECK_ASSERT(groupnr == e4b->bd_group);
		for (i = 0; i < pa->pa_len; i++)
			MB_CHECK_ASSERT(mb_test_bit(k + i, buddy));
	}
	return 0;
}
#undef MB_CHECK_ASSERT
#define mb_check_buddy(e4b) __mb_check_buddy(e4b,	\
					__FILE__, __func__, __LINE__)
#else
#define mb_check_buddy(e4b)
#endif

/*
 * Divide blocks started from @first with length @len into
 * smaller chunks with power of 2 blocks.
 * Clear the bits in bitmap which the blocks of the chunk(s) covered,
 * then increase bb_counters[] for corresponded chunk size.
 */
static void ext4_mb_mark_free_simple(struct super_block *sb,
				void *buddy, ext4_grpblk_t first, ext4_grpblk_t len,
					struct ext4_group_info *grp)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_grpblk_t min;
	ext4_grpblk_t max;
	ext4_grpblk_t chunk;
	unsigned int border;

	BUG_ON(len > EXT4_CLUSTERS_PER_GROUP(sb));

	border = 2 << sb->s_blocksize_bits;

	while (len > 0) {
		/* find how many blocks can be covered since this position */
		max = ffs(first | border) - 1;

		/* find how many blocks of power 2 we need to mark */
		min = fls(len) - 1;

		if (max < min)
			min = max;
		chunk = 1 << min;

		/* mark multiblock chunks only */
		grp->bb_counters[min]++;
		if (min > 0)
			mb_clear_bit(first >> min,
				     buddy + sbi->s_mb_offsets[min]);

		len -= chunk;
		first += chunk;
	}
}

static int mb_avg_fragment_size_order(struct super_block *sb, ext4_grpblk_t len)
{
	int order;

	/*
	 * We don't bother with a special lists groups with only 1 block free
	 * extents and for completely empty groups.
	 */
	order = fls(len) - 2;
	if (order < 0)
		return 0;
	if (order == MB_NUM_ORDERS(sb))
		order--;
	return order;
}

/* Move group to appropriate avg_fragment_size list */
static void
mb_update_avg_fragment_size(struct super_block *sb, struct ext4_group_info *grp)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	int new_order;

	if (!test_opt2(sb, MB_OPTIMIZE_SCAN) || grp->bb_free == 0)
		return;

	new_order = mb_avg_fragment_size_order(sb,
					grp->bb_free / grp->bb_fragments);
	if (new_order == grp->bb_avg_fragment_size_order)
		return;

	if (grp->bb_avg_fragment_size_order != -1) {
		write_lock(&sbi->s_mb_avg_fragment_size_locks[
					grp->bb_avg_fragment_size_order]);
		list_del(&grp->bb_avg_fragment_size_node);
		write_unlock(&sbi->s_mb_avg_fragment_size_locks[
					grp->bb_avg_fragment_size_order]);
	}
	grp->bb_avg_fragment_size_order = new_order;
	write_lock(&sbi->s_mb_avg_fragment_size_locks[
					grp->bb_avg_fragment_size_order]);
	list_add_tail(&grp->bb_avg_fragment_size_node,
		&sbi->s_mb_avg_fragment_size[grp->bb_avg_fragment_size_order]);
	write_unlock(&sbi->s_mb_avg_fragment_size_locks[
					grp->bb_avg_fragment_size_order]);
}

/*
 * Choose next group by traversing largest_free_order lists. Updates *new_cr if
 * cr level needs an update.
 */
static void ext4_mb_choose_next_group_p2_aligned(struct ext4_allocation_context *ac,
			enum criteria *new_cr, ext4_group_t *group, ext4_group_t ngroups)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct ext4_group_info *iter;
	int i;

	if (ac->ac_status == AC_STATUS_FOUND)
		return;

	if (unlikely(sbi->s_mb_stats && ac->ac_flags & EXT4_MB_CR_POWER2_ALIGNED_OPTIMIZED))
		atomic_inc(&sbi->s_bal_p2_aligned_bad_suggestions);

	for (i = ac->ac_2order; i < MB_NUM_ORDERS(ac->ac_sb); i++) {
		if (list_empty(&sbi->s_mb_largest_free_orders[i]))
			continue;
		read_lock(&sbi->s_mb_largest_free_orders_locks[i]);
		if (list_empty(&sbi->s_mb_largest_free_orders[i])) {
			read_unlock(&sbi->s_mb_largest_free_orders_locks[i]);
			continue;
		}
		list_for_each_entry(iter, &sbi->s_mb_largest_free_orders[i],
				    bb_largest_free_order_node) {
			if (sbi->s_mb_stats)
				atomic64_inc(&sbi->s_bal_cX_groups_considered[CR_POWER2_ALIGNED]);
			if (likely(ext4_mb_good_group(ac, iter->bb_group, CR_POWER2_ALIGNED))) {
				*group = iter->bb_group;
				ac->ac_flags |= EXT4_MB_CR_POWER2_ALIGNED_OPTIMIZED;
				read_unlock(&sbi->s_mb_largest_free_orders_locks[i]);
				return;
			}
		}
		read_unlock(&sbi->s_mb_largest_free_orders_locks[i]);
	}

	/* Increment cr and search again if no group is found */
	*new_cr = CR_GOAL_LEN_FAST;
}

/*
 * Find a suitable group of given order from the average fragments list.
 */
static struct ext4_group_info *
ext4_mb_find_good_group_avg_frag_lists(struct ext4_allocation_context *ac, int order)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct list_head *frag_list = &sbi->s_mb_avg_fragment_size[order];
	rwlock_t *frag_list_lock = &sbi->s_mb_avg_fragment_size_locks[order];
	struct ext4_group_info *grp = NULL, *iter;
	enum criteria cr = ac->ac_criteria;

	if (list_empty(frag_list))
		return NULL;
	read_lock(frag_list_lock);
	if (list_empty(frag_list)) {
		read_unlock(frag_list_lock);
		return NULL;
	}
	list_for_each_entry(iter, frag_list, bb_avg_fragment_size_node) {
		if (sbi->s_mb_stats)
			atomic64_inc(&sbi->s_bal_cX_groups_considered[cr]);
		if (likely(ext4_mb_good_group(ac, iter->bb_group, cr))) {
			grp = iter;
			break;
		}
	}
	read_unlock(frag_list_lock);
	return grp;
}

/*
 * Choose next group by traversing average fragment size list of suitable
 * order. Updates *new_cr if cr level needs an update.
 */
static void ext4_mb_choose_next_group_goal_fast(struct ext4_allocation_context *ac,
		enum criteria *new_cr, ext4_group_t *group, ext4_group_t ngroups)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct ext4_group_info *grp = NULL;
	int i;

	if (unlikely(ac->ac_flags & EXT4_MB_CR_GOAL_LEN_FAST_OPTIMIZED)) {
		if (sbi->s_mb_stats)
			atomic_inc(&sbi->s_bal_goal_fast_bad_suggestions);
	}

	for (i = mb_avg_fragment_size_order(ac->ac_sb, ac->ac_g_ex.fe_len);
	     i < MB_NUM_ORDERS(ac->ac_sb); i++) {
		grp = ext4_mb_find_good_group_avg_frag_lists(ac, i);
		if (grp) {
			*group = grp->bb_group;
			ac->ac_flags |= EXT4_MB_CR_GOAL_LEN_FAST_OPTIMIZED;
			return;
		}
	}

	/*
	 * CR_BEST_AVAIL_LEN works based on the concept that we have
	 * a larger normalized goal len request which can be trimmed to
	 * a smaller goal len such that it can still satisfy original
	 * request len. However, allocation request for non-regular
	 * files never gets normalized.
	 * See function ext4_mb_normalize_request() (EXT4_MB_HINT_DATA).
	 */
	if (ac->ac_flags & EXT4_MB_HINT_DATA)
		*new_cr = CR_BEST_AVAIL_LEN;
	else
		*new_cr = CR_GOAL_LEN_SLOW;
}

/*
 * We couldn't find a group in CR_GOAL_LEN_FAST so try to find the highest free fragment
 * order we have and proactively trim the goal request length to that order to
 * find a suitable group faster.
 *
 * This optimizes allocation speed at the cost of slightly reduced
 * preallocations. However, we make sure that we don't trim the request too
 * much and fall to CR_GOAL_LEN_SLOW in that case.
 */
static void ext4_mb_choose_next_group_best_avail(struct ext4_allocation_context *ac,
		enum criteria *new_cr, ext4_group_t *group, ext4_group_t ngroups)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct ext4_group_info *grp = NULL;
	int i, order, min_order;
	unsigned long num_stripe_clusters = 0;

	if (unlikely(ac->ac_flags & EXT4_MB_CR_BEST_AVAIL_LEN_OPTIMIZED)) {
		if (sbi->s_mb_stats)
			atomic_inc(&sbi->s_bal_best_avail_bad_suggestions);
	}

	/*
	 * mb_avg_fragment_size_order() returns order in a way that makes
	 * retrieving back the length using (1 << order) inaccurate. Hence, use
	 * fls() instead since we need to know the actual length while modifying
	 * goal length.
	 */
	order = fls(ac->ac_g_ex.fe_len) - 1;
	min_order = order - sbi->s_mb_best_avail_max_trim_order;
	if (min_order < 0)
		min_order = 0;

	if (sbi->s_stripe > 0) {
		/*
		 * We are assuming that stripe size is always a multiple of
		 * cluster ratio otherwise __ext4_fill_super exists early.
		 */
		num_stripe_clusters = EXT4_NUM_B2C(sbi, sbi->s_stripe);
		if (1 << min_order < num_stripe_clusters)
			/*
			 * We consider 1 order less because later we round
			 * up the goal len to num_stripe_clusters
			 */
			min_order = fls(num_stripe_clusters) - 1;
	}

	if (1 << min_order < ac->ac_o_ex.fe_len)
		min_order = fls(ac->ac_o_ex.fe_len);

	for (i = order; i >= min_order; i--) {
		int frag_order;
		/*
		 * Scale down goal len to make sure we find something
		 * in the free fragments list. Basically, reduce
		 * preallocations.
		 */
		ac->ac_g_ex.fe_len = 1 << i;

		if (num_stripe_clusters > 0) {
			/*
			 * Try to round up the adjusted goal length to
			 * stripe size (in cluster units) multiple for
			 * efficiency.
			 */
			ac->ac_g_ex.fe_len = roundup(ac->ac_g_ex.fe_len,
						     num_stripe_clusters);
		}

		frag_order = mb_avg_fragment_size_order(ac->ac_sb,
							ac->ac_g_ex.fe_len);

		grp = ext4_mb_find_good_group_avg_frag_lists(ac, frag_order);
		if (grp) {
			*group = grp->bb_group;
			ac->ac_flags |= EXT4_MB_CR_BEST_AVAIL_LEN_OPTIMIZED;
			return;
		}
	}

	/* Reset goal length to original goal length before falling into CR_GOAL_LEN_SLOW */
	ac->ac_g_ex.fe_len = ac->ac_orig_goal_len;
	*new_cr = CR_GOAL_LEN_SLOW;
}

static inline int should_optimize_scan(struct ext4_allocation_context *ac)
{
	if (unlikely(!test_opt2(ac->ac_sb, MB_OPTIMIZE_SCAN)))
		return 0;
	if (ac->ac_criteria >= CR_GOAL_LEN_SLOW)
		return 0;
	if (!ext4_test_inode_flag(ac->ac_inode, EXT4_INODE_EXTENTS))
		return 0;
	return 1;
}

/*
 * Return next linear group for allocation. If linear traversal should not be
 * performed, this function just returns the same group
 */
static ext4_group_t
next_linear_group(struct ext4_allocation_context *ac, ext4_group_t group,
		  ext4_group_t ngroups)
{
	if (!should_optimize_scan(ac))
		goto inc_and_return;

	if (ac->ac_groups_linear_remaining) {
		ac->ac_groups_linear_remaining--;
		goto inc_and_return;
	}

	return group;
inc_and_return:
	/*
	 * Artificially restricted ngroups for non-extent
	 * files makes group > ngroups possible on first loop.
	 */
	return group + 1 >= ngroups ? 0 : group + 1;
}

/*
 * ext4_mb_choose_next_group: choose next group for allocation.
 *
 * @ac        Allocation Context
 * @new_cr    This is an output parameter. If the there is no good group
 *            available at current CR level, this field is updated to indicate
 *            the new cr level that should be used.
 * @group     This is an input / output parameter. As an input it indicates the
 *            next group that the allocator intends to use for allocation. As
 *            output, this field indicates the next group that should be used as
 *            determined by the optimization functions.
 * @ngroups   Total number of groups
 */
static void ext4_mb_choose_next_group(struct ext4_allocation_context *ac,
		enum criteria *new_cr, ext4_group_t *group, ext4_group_t ngroups)
{
	*new_cr = ac->ac_criteria;

	if (!should_optimize_scan(ac) || ac->ac_groups_linear_remaining) {
		*group = next_linear_group(ac, *group, ngroups);
		return;
	}

	if (*new_cr == CR_POWER2_ALIGNED) {
		ext4_mb_choose_next_group_p2_aligned(ac, new_cr, group, ngroups);
	} else if (*new_cr == CR_GOAL_LEN_FAST) {
		ext4_mb_choose_next_group_goal_fast(ac, new_cr, group, ngroups);
	} else if (*new_cr == CR_BEST_AVAIL_LEN) {
		ext4_mb_choose_next_group_best_avail(ac, new_cr, group, ngroups);
	} else {
		/*
		 * TODO: For CR=2, we can arrange groups in an rb tree sorted by
		 * bb_free. But until that happens, we should never come here.
		 */
		WARN_ON(1);
	}
}

/*
 * Cache the order of the largest free extent we have available in this block
 * group.
 */
static void
mb_set_largest_free_order(struct super_block *sb, struct ext4_group_info *grp)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	int i;

	for (i = MB_NUM_ORDERS(sb) - 1; i >= 0; i--)
		if (grp->bb_counters[i] > 0)
			break;
	/* No need to move between order lists? */
	if (!test_opt2(sb, MB_OPTIMIZE_SCAN) ||
	    i == grp->bb_largest_free_order) {
		grp->bb_largest_free_order = i;
		return;
	}

	if (grp->bb_largest_free_order >= 0) {
		write_lock(&sbi->s_mb_largest_free_orders_locks[
					      grp->bb_largest_free_order]);
		list_del_init(&grp->bb_largest_free_order_node);
		write_unlock(&sbi->s_mb_largest_free_orders_locks[
					      grp->bb_largest_free_order]);
	}
	grp->bb_largest_free_order = i;
	if (grp->bb_largest_free_order >= 0 && grp->bb_free) {
		write_lock(&sbi->s_mb_largest_free_orders_locks[
					      grp->bb_largest_free_order]);
		list_add_tail(&grp->bb_largest_free_order_node,
		      &sbi->s_mb_largest_free_orders[grp->bb_largest_free_order]);
		write_unlock(&sbi->s_mb_largest_free_orders_locks[
					      grp->bb_largest_free_order]);
	}
}

static noinline_for_stack
void ext4_mb_generate_buddy(struct super_block *sb,
			    void *buddy, void *bitmap, ext4_group_t group,
			    struct ext4_group_info *grp)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_grpblk_t max = EXT4_CLUSTERS_PER_GROUP(sb);
	ext4_grpblk_t i = 0;
	ext4_grpblk_t first;
	ext4_grpblk_t len;
	unsigned free = 0;
	unsigned fragments = 0;
	unsigned long long period = get_cycles();

	/* initialize buddy from bitmap which is aggregation
	 * of on-disk bitmap and preallocations */
	i = mb_find_next_zero_bit(bitmap, max, 0);
	grp->bb_first_free = i;
	while (i < max) {
		fragments++;
		first = i;
		i = mb_find_next_bit(bitmap, max, i);
		len = i - first;
		free += len;
		if (len > 1)
			ext4_mb_mark_free_simple(sb, buddy, first, len, grp);
		else
			grp->bb_counters[0]++;
		if (i < max)
			i = mb_find_next_zero_bit(bitmap, max, i);
	}
	grp->bb_fragments = fragments;

	if (free != grp->bb_free) {
		ext4_grp_locked_error(sb, group, 0, 0,
				      "block bitmap and bg descriptor "
				      "inconsistent: %u vs %u free clusters",
				      free, grp->bb_free);
		/*
		 * If we intend to continue, we consider group descriptor
		 * corrupt and update bb_free using bitmap value
		 */
		grp->bb_free = free;
		ext4_mark_group_bitmap_corrupted(sb, group,
					EXT4_GROUP_INFO_BBITMAP_CORRUPT);
	}
	mb_set_largest_free_order(sb, grp);
	mb_update_avg_fragment_size(sb, grp);

	clear_bit(EXT4_GROUP_INFO_NEED_INIT_BIT, &(grp->bb_state));

	period = get_cycles() - period;
	atomic_inc(&sbi->s_mb_buddies_generated);
	atomic64_add(period, &sbi->s_mb_generation_time);
}

/* The buddy information is attached the buddy cache inode
 * for convenience. The information regarding each group
 * is loaded via ext4_mb_load_buddy. The information involve
 * block bitmap and buddy information. The information are
 * stored in the inode as
 *
 * {                        page                        }
 * [ group 0 bitmap][ group 0 buddy] [group 1][ group 1]...
 *
 *
 * one block each for bitmap and buddy information.
 * So for each group we take up 2 blocks. A page can
 * contain blocks_per_page (PAGE_SIZE / blocksize)  blocks.
 * So it can have information regarding groups_per_page which
 * is blocks_per_page/2
 *
 * Locking note:  This routine takes the block group lock of all groups
 * for this page; do not hold this lock when calling this routine!
 */

static int ext4_mb_init_cache(struct page *page, char *incore, gfp_t gfp)
{
	ext4_group_t ngroups;
	unsigned int blocksize;
	int blocks_per_page;
	int groups_per_page;
	int err = 0;
	int i;
	ext4_group_t first_group, group;
	int first_block;
	struct super_block *sb;
	struct buffer_head *bhs;
	struct buffer_head **bh = NULL;
	struct inode *inode;
	char *data;
	char *bitmap;
	struct ext4_group_info *grinfo;

	inode = page->mapping->host;
	sb = inode->i_sb;
	ngroups = ext4_get_groups_count(sb);
	blocksize = i_blocksize(inode);
	blocks_per_page = PAGE_SIZE / blocksize;

	mb_debug(sb, "init page %lu\n", page->index);

	groups_per_page = blocks_per_page >> 1;
	if (groups_per_page == 0)
		groups_per_page = 1;

	/* allocate buffer_heads to read bitmaps */
	if (groups_per_page > 1) {
		i = sizeof(struct buffer_head *) * groups_per_page;
		bh = kzalloc(i, gfp);
		if (bh == NULL)
			return -ENOMEM;
	} else
		bh = &bhs;

	first_group = page->index * blocks_per_page / 2;

	/* read all groups the page covers into the cache */
	for (i = 0, group = first_group; i < groups_per_page; i++, group++) {
		if (group >= ngroups)
			break;

		grinfo = ext4_get_group_info(sb, group);
		if (!grinfo)
			continue;
		/*
		 * If page is uptodate then we came here after online resize
		 * which added some new uninitialized group info structs, so
		 * we must skip all initialized uptodate buddies on the page,
		 * which may be currently in use by an allocating task.
		 */
		if (PageUptodate(page) && !EXT4_MB_GRP_NEED_INIT(grinfo)) {
			bh[i] = NULL;
			continue;
		}
		bh[i] = ext4_read_block_bitmap_nowait(sb, group, false);
		if (IS_ERR(bh[i])) {
			err = PTR_ERR(bh[i]);
			bh[i] = NULL;
			goto out;
		}
		mb_debug(sb, "read bitmap for group %u\n", group);
	}

	/* wait for I/O completion */
	for (i = 0, group = first_group; i < groups_per_page; i++, group++) {
		int err2;

		if (!bh[i])
			continue;
		err2 = ext4_wait_block_bitmap(sb, group, bh[i]);
		if (!err)
			err = err2;
	}

	first_block = page->index * blocks_per_page;
	for (i = 0; i < blocks_per_page; i++) {
		group = (first_block + i) >> 1;
		if (group >= ngroups)
			break;

		if (!bh[group - first_group])
			/* skip initialized uptodate buddy */
			continue;

		if (!buffer_verified(bh[group - first_group]))
			/* Skip faulty bitmaps */
			continue;
		err = 0;

		/*
		 * data carry information regarding this
		 * particular group in the format specified
		 * above
		 *
		 */
		data = page_address(page) + (i * blocksize);
		bitmap = bh[group - first_group]->b_data;

		/*
		 * We place the buddy block and bitmap block
		 * close together
		 */
		if ((first_block + i) & 1) {
			/* this is block of buddy */
			BUG_ON(incore == NULL);
			mb_debug(sb, "put buddy for group %u in page %lu/%x\n",
				group, page->index, i * blocksize);
			trace_ext4_mb_buddy_bitmap_load(sb, group);
			grinfo = ext4_get_group_info(sb, group);
			if (!grinfo) {
				err = -EFSCORRUPTED;
				goto out;
			}
			grinfo->bb_fragments = 0;
			memset(grinfo->bb_counters, 0,
			       sizeof(*grinfo->bb_counters) *
			       (MB_NUM_ORDERS(sb)));
			/*
			 * incore got set to the group block bitmap below
			 */
			ext4_lock_group(sb, group);
			/* init the buddy */
			memset(data, 0xff, blocksize);
			ext4_mb_generate_buddy(sb, data, incore, group, grinfo);
			ext4_unlock_group(sb, group);
			incore = NULL;
		} else {
			/* this is block of bitmap */
			BUG_ON(incore != NULL);
			mb_debug(sb, "put bitmap for group %u in page %lu/%x\n",
				group, page->index, i * blocksize);
			trace_ext4_mb_bitmap_load(sb, group);

			/* see comments in ext4_mb_put_pa() */
			ext4_lock_group(sb, group);
			memcpy(data, bitmap, blocksize);

			/* mark all preallocated blks used in in-core bitmap */
			ext4_mb_generate_from_pa(sb, data, group);
			ext4_mb_generate_from_freelist(sb, data, group);
			ext4_unlock_group(sb, group);

			/* set incore so that the buddy information can be
			 * generated using this
			 */
			incore = data;
		}
	}
	SetPageUptodate(page);

out:
	if (bh) {
		for (i = 0; i < groups_per_page; i++)
			brelse(bh[i]);
		if (bh != &bhs)
			kfree(bh);
	}
	return err;
}

/*
 * Lock the buddy and bitmap pages. This make sure other parallel init_group
 * on the same buddy page doesn't happen whild holding the buddy page lock.
 * Return locked buddy and bitmap pages on e4b struct. If buddy and bitmap
 * are on the same page e4b->bd_buddy_page is NULL and return value is 0.
 */
static int ext4_mb_get_buddy_page_lock(struct super_block *sb,
		ext4_group_t group, struct ext4_buddy *e4b, gfp_t gfp)
{
	struct inode *inode = EXT4_SB(sb)->s_buddy_cache;
	int block, pnum, poff;
	int blocks_per_page;
	struct page *page;

	e4b->bd_buddy_page = NULL;
	e4b->bd_bitmap_page = NULL;

	blocks_per_page = PAGE_SIZE / sb->s_blocksize;
	/*
	 * the buddy cache inode stores the block bitmap
	 * and buddy information in consecutive blocks.
	 * So for each group we need two blocks.
	 */
	block = group * 2;
	pnum = block / blocks_per_page;
	poff = block % blocks_per_page;
	page = find_or_create_page(inode->i_mapping, pnum, gfp);
	if (!page)
		return -ENOMEM;
	BUG_ON(page->mapping != inode->i_mapping);
	e4b->bd_bitmap_page = page;
	e4b->bd_bitmap = page_address(page) + (poff * sb->s_blocksize);

	if (blocks_per_page >= 2) {
		/* buddy and bitmap are on the same page */
		return 0;
	}

	block++;
	pnum = block / blocks_per_page;
	page = find_or_create_page(inode->i_mapping, pnum, gfp);
	if (!page)
		return -ENOMEM;
	BUG_ON(page->mapping != inode->i_mapping);
	e4b->bd_buddy_page = page;
	return 0;
}

static void ext4_mb_put_buddy_page_lock(struct ext4_buddy *e4b)
{
	if (e4b->bd_bitmap_page) {
		unlock_page(e4b->bd_bitmap_page);
		put_page(e4b->bd_bitmap_page);
	}
	if (e4b->bd_buddy_page) {
		unlock_page(e4b->bd_buddy_page);
		put_page(e4b->bd_buddy_page);
	}
}

/*
 * Locking note:  This routine calls ext4_mb_init_cache(), which takes the
 * block group lock of all groups for this page; do not hold the BG lock when
 * calling this routine!
 */
static noinline_for_stack
int ext4_mb_init_group(struct super_block *sb, ext4_group_t group, gfp_t gfp)
{

	struct ext4_group_info *this_grp;
	struct ext4_buddy e4b;
	struct page *page;
	int ret = 0;

	might_sleep();
	mb_debug(sb, "init group %u\n", group);
	this_grp = ext4_get_group_info(sb, group);
	if (!this_grp)
		return -EFSCORRUPTED;

	/*
	 * This ensures that we don't reinit the buddy cache
	 * page which map to the group from which we are already
	 * allocating. If we are looking at the buddy cache we would
	 * have taken a reference using ext4_mb_load_buddy and that
	 * would have pinned buddy page to page cache.
	 * The call to ext4_mb_get_buddy_page_lock will mark the
	 * page accessed.
	 */
	ret = ext4_mb_get_buddy_page_lock(sb, group, &e4b, gfp);
	if (ret || !EXT4_MB_GRP_NEED_INIT(this_grp)) {
		/*
		 * somebody initialized the group
		 * return without doing anything
		 */
		goto err;
	}

	page = e4b.bd_bitmap_page;
	ret = ext4_mb_init_cache(page, NULL, gfp);
	if (ret)
		goto err;
	if (!PageUptodate(page)) {
		ret = -EIO;
		goto err;
	}

	if (e4b.bd_buddy_page == NULL) {
		/*
		 * If both the bitmap and buddy are in
		 * the same page we don't need to force
		 * init the buddy
		 */
		ret = 0;
		goto err;
	}
	/* init buddy cache */
	page = e4b.bd_buddy_page;
	ret = ext4_mb_init_cache(page, e4b.bd_bitmap, gfp);
	if (ret)
		goto err;
	if (!PageUptodate(page)) {
		ret = -EIO;
		goto err;
	}
err:
	ext4_mb_put_buddy_page_lock(&e4b);
	return ret;
}

/*
 * Locking note:  This routine calls ext4_mb_init_cache(), which takes the
 * block group lock of all groups for this page; do not hold the BG lock when
 * calling this routine!
 */
static noinline_for_stack int
ext4_mb_load_buddy_gfp(struct super_block *sb, ext4_group_t group,
		       struct ext4_buddy *e4b, gfp_t gfp)
{
	int blocks_per_page;
	int block;
	int pnum;
	int poff;
	struct page *page;
	int ret;
	struct ext4_group_info *grp;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct inode *inode = sbi->s_buddy_cache;

	might_sleep();
	mb_debug(sb, "load group %u\n", group);

	blocks_per_page = PAGE_SIZE / sb->s_blocksize;
	grp = ext4_get_group_info(sb, group);
	if (!grp)
		return -EFSCORRUPTED;

	e4b->bd_blkbits = sb->s_blocksize_bits;
	e4b->bd_info = grp;
	e4b->bd_sb = sb;
	e4b->bd_group = group;
	e4b->bd_buddy_page = NULL;
	e4b->bd_bitmap_page = NULL;

	if (unlikely(EXT4_MB_GRP_NEED_INIT(grp))) {
		/*
		 * we need full data about the group
		 * to make a good selection
		 */
		ret = ext4_mb_init_group(sb, group, gfp);
		if (ret)
			return ret;
	}

	/*
	 * the buddy cache inode stores the block bitmap
	 * and buddy information in consecutive blocks.
	 * So for each group we need two blocks.
	 */
	block = group * 2;
	pnum = block / blocks_per_page;
	poff = block % blocks_per_page;

	/* we could use find_or_create_page(), but it locks page
	 * what we'd like to avoid in fast path ... */
	page = find_get_page_flags(inode->i_mapping, pnum, FGP_ACCESSED);
	if (page == NULL || !PageUptodate(page)) {
		if (page)
			/*
			 * drop the page reference and try
			 * to get the page with lock. If we
			 * are not uptodate that implies
			 * somebody just created the page but
			 * is yet to initialize the same. So
			 * wait for it to initialize.
			 */
			put_page(page);
		page = find_or_create_page(inode->i_mapping, pnum, gfp);
		if (page) {
			if (WARN_RATELIMIT(page->mapping != inode->i_mapping,
	"ext4: bitmap's paging->mapping != inode->i_mapping\n")) {
				/* should never happen */
				unlock_page(page);
				ret = -EINVAL;
				goto err;
			}
			if (!PageUptodate(page)) {
				ret = ext4_mb_init_cache(page, NULL, gfp);
				if (ret) {
					unlock_page(page);
					goto err;
				}
				mb_cmp_bitmaps(e4b, page_address(page) +
					       (poff * sb->s_blocksize));
			}
			unlock_page(page);
		}
	}
	if (page == NULL) {
		ret = -ENOMEM;
		goto err;
	}
	if (!PageUptodate(page)) {
		ret = -EIO;
		goto err;
	}

	/* Pages marked accessed already */
	e4b->bd_bitmap_page = page;
	e4b->bd_bitmap = page_address(page) + (poff * sb->s_blocksize);

	block++;
	pnum = block / blocks_per_page;
	poff = block % blocks_per_page;

	page = find_get_page_flags(inode->i_mapping, pnum, FGP_ACCESSED);
	if (page == NULL || !PageUptodate(page)) {
		if (page)
			put_page(page);
		page = find_or_create_page(inode->i_mapping, pnum, gfp);
		if (page) {
			if (WARN_RATELIMIT(page->mapping != inode->i_mapping,
	"ext4: buddy bitmap's page->mapping != inode->i_mapping\n")) {
				/* should never happen */
				unlock_page(page);
				ret = -EINVAL;
				goto err;
			}
			if (!PageUptodate(page)) {
				ret = ext4_mb_init_cache(page, e4b->bd_bitmap,
							 gfp);
				if (ret) {
					unlock_page(page);
					goto err;
				}
			}
			unlock_page(page);
		}
	}
	if (page == NULL) {
		ret = -ENOMEM;
		goto err;
	}
	if (!PageUptodate(page)) {
		ret = -EIO;
		goto err;
	}

	/* Pages marked accessed already */
	e4b->bd_buddy_page = page;
	e4b->bd_buddy = page_address(page) + (poff * sb->s_blocksize);

	return 0;

err:
	if (page)
		put_page(page);
	if (e4b->bd_bitmap_page)
		put_page(e4b->bd_bitmap_page);

	e4b->bd_buddy = NULL;
	e4b->bd_bitmap = NULL;
	return ret;
}

static int ext4_mb_load_buddy(struct super_block *sb, ext4_group_t group,
			      struct ext4_buddy *e4b)
{
	return ext4_mb_load_buddy_gfp(sb, group, e4b, GFP_NOFS);
}

static void ext4_mb_unload_buddy(struct ext4_buddy *e4b)
{
	if (e4b->bd_bitmap_page)
		put_page(e4b->bd_bitmap_page);
	if (e4b->bd_buddy_page)
		put_page(e4b->bd_buddy_page);
}


static int mb_find_order_for_block(struct ext4_buddy *e4b, int block)
{
	int order = 1, max;
	void *bb;

	BUG_ON(e4b->bd_bitmap == e4b->bd_buddy);
	BUG_ON(block >= (1 << (e4b->bd_blkbits + 3)));

	while (order <= e4b->bd_blkbits + 1) {
		bb = mb_find_buddy(e4b, order, &max);
		if (!mb_test_bit(block >> order, bb)) {
			/* this block is part of buddy of order 'order' */
			return order;
		}
		order++;
	}
	return 0;
}

static void mb_clear_bits(void *bm, int cur, int len)
{
	__u32 *addr;

	len = cur + len;
	while (cur < len) {
		if ((cur & 31) == 0 && (len - cur) >= 32) {
			/* fast path: clear whole word at once */
			addr = bm + (cur >> 3);
			*addr = 0;
			cur += 32;
			continue;
		}
		mb_clear_bit(cur, bm);
		cur++;
	}
}

/* clear bits in given range
 * will return first found zero bit if any, -1 otherwise
 */
static int mb_test_and_clear_bits(void *bm, int cur, int len)
{
	__u32 *addr;
	int zero_bit = -1;

	len = cur + len;
	while (cur < len) {
		if ((cur & 31) == 0 && (len - cur) >= 32) {
			/* fast path: clear whole word at once */
			addr = bm + (cur >> 3);
			if (*addr != (__u32)(-1) && zero_bit == -1)
				zero_bit = cur + mb_find_next_zero_bit(addr, 32, 0);
			*addr = 0;
			cur += 32;
			continue;
		}
		if (!mb_test_and_clear_bit(cur, bm) && zero_bit == -1)
			zero_bit = cur;
		cur++;
	}

	return zero_bit;
}

void mb_set_bits(void *bm, int cur, int len)
{
	__u32 *addr;

	len = cur + len;
	while (cur < len) {
		if ((cur & 31) == 0 && (len - cur) >= 32) {
			/* fast path: set whole word at once */
			addr = bm + (cur >> 3);
			*addr = 0xffffffff;
			cur += 32;
			continue;
		}
		mb_set_bit(cur, bm);
		cur++;
	}
}

static inline int mb_buddy_adjust_border(int* bit, void* bitmap, int side)
{
	if (mb_test_bit(*bit + side, bitmap)) {
		mb_clear_bit(*bit, bitmap);
		(*bit) -= side;
		return 1;
	}
	else {
		(*bit) += side;
		mb_set_bit(*bit, bitmap);
		return -1;
	}
}

static void mb_buddy_mark_free(struct ext4_buddy *e4b, int first, int last)
{
	int max;
	int order = 1;
	void *buddy = mb_find_buddy(e4b, order, &max);

	while (buddy) {
		void *buddy2;

		/* Bits in range [first; last] are known to be set since
		 * corresponding blocks were allocated. Bits in range
		 * (first; last) will stay set because they form buddies on
		 * upper layer. We just deal with borders if they don't
		 * align with upper layer and then go up.
		 * Releasing entire group is all about clearing
		 * single bit of highest order buddy.
		 */

		/* Example:
		 * ---------------------------------
		 * |   1   |   1   |   1   |   1   |
		 * ---------------------------------
		 * | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
		 * ---------------------------------
		 *   0   1   2   3   4   5   6   7
		 *      \_____________________/
		 *
		 * Neither [1] nor [6] is aligned to above layer.
		 * Left neighbour [0] is free, so mark it busy,
		 * decrease bb_counters and extend range to
		 * [0; 6]
		 * Right neighbour [7] is busy. It can't be coaleasced with [6], so
		 * mark [6] free, increase bb_counters and shrink range to
		 * [0; 5].
		 * Then shift range to [0; 2], go up and do the same.
		 */


		if (first & 1)
			e4b->bd_info->bb_counters[order] += mb_buddy_adjust_border(&first, buddy, -1);
		if (!(last & 1))
			e4b->bd_info->bb_counters[order] += mb_buddy_adjust_border(&last, buddy, 1);
		if (first > last)
			break;
		order++;

		buddy2 = mb_find_buddy(e4b, order, &max);
		if (!buddy2) {
			mb_clear_bits(buddy, first, last - first + 1);
			e4b->bd_info->bb_counters[order - 1] += last - first + 1;
			break;
		}
		first >>= 1;
		last >>= 1;
		buddy = buddy2;
	}
}

static void mb_free_blocks(struct inode *inode, struct ext4_buddy *e4b,
			   int first, int count)
{
	int left_is_free = 0;
	int right_is_free = 0;
	int block;
	int last = first + count - 1;
	struct super_block *sb = e4b->bd_sb;

	if (WARN_ON(count == 0))
		return;
	BUG_ON(last >= (sb->s_blocksize << 3));
	assert_spin_locked(ext4_group_lock_ptr(sb, e4b->bd_group));
	/* Don't bother if the block group is corrupt. */
	if (unlikely(EXT4_MB_GRP_BBITMAP_CORRUPT(e4b->bd_info)))
		return;

	mb_check_buddy(e4b);
	mb_free_blocks_double(inode, e4b, first, count);

	this_cpu_inc(discard_pa_seq);
	e4b->bd_info->bb_free += count;
	if (first < e4b->bd_info->bb_first_free)
		e4b->bd_info->bb_first_free = first;

	/* access memory sequentially: check left neighbour,
	 * clear range and then check right neighbour
	 */
	if (first != 0)
		left_is_free = !mb_test_bit(first - 1, e4b->bd_bitmap);
	block = mb_test_and_clear_bits(e4b->bd_bitmap, first, count);
	if (last + 1 < EXT4_SB(sb)->s_mb_maxs[0])
		right_is_free = !mb_test_bit(last + 1, e4b->bd_bitmap);

	if (unlikely(block != -1)) {
		struct ext4_sb_info *sbi = EXT4_SB(sb);
		ext4_fsblk_t blocknr;

		blocknr = ext4_group_first_block_no(sb, e4b->bd_group);
		blocknr += EXT4_C2B(sbi, block);
		if (!(sbi->s_mount_state & EXT4_FC_REPLAY)) {
			ext4_grp_locked_error(sb, e4b->bd_group,
					      inode ? inode->i_ino : 0,
					      blocknr,
					      "freeing already freed block (bit %u); block bitmap corrupt.",
					      block);
			ext4_mark_group_bitmap_corrupted(
				sb, e4b->bd_group,
				EXT4_GROUP_INFO_BBITMAP_CORRUPT);
		}
		goto done;
	}

	/* let's maintain fragments counter */
	if (left_is_free && right_is_free)
		e4b->bd_info->bb_fragments--;
	else if (!left_is_free && !right_is_free)
		e4b->bd_info->bb_fragments++;

	/* buddy[0] == bd_bitmap is a special case, so handle
	 * it right away and let mb_buddy_mark_free stay free of
	 * zero order checks.
	 * Check if neighbours are to be coaleasced,
	 * adjust bitmap bb_counters and borders appropriately.
	 */
	if (first & 1) {
		first += !left_is_free;
		e4b->bd_info->bb_counters[0] += left_is_free ? -1 : 1;
	}
	if (!(last & 1)) {
		last -= !right_is_free;
		e4b->bd_info->bb_counters[0] += right_is_free ? -1 : 1;
	}

	if (first <= last)
		mb_buddy_mark_free(e4b, first >> 1, last >> 1);

done:
	mb_set_largest_free_order(sb, e4b->bd_info);
	mb_update_avg_fragment_size(sb, e4b->bd_info);
	mb_check_buddy(e4b);
}

static int mb_find_extent(struct ext4_buddy *e4b, int block,
				int needed, struct ext4_free_extent *ex)
{
	int next = block;
	int max, order;
	void *buddy;

	assert_spin_locked(ext4_group_lock_ptr(e4b->bd_sb, e4b->bd_group));
	BUG_ON(ex == NULL);

	buddy = mb_find_buddy(e4b, 0, &max);
	BUG_ON(buddy == NULL);
	BUG_ON(block >= max);
	if (mb_test_bit(block, buddy)) {
		ex->fe_len = 0;
		ex->fe_start = 0;
		ex->fe_group = 0;
		return 0;
	}

	/* find actual order */
	order = mb_find_order_for_block(e4b, block);
	block = block >> order;

	ex->fe_len = 1 << order;
	ex->fe_start = block << order;
	ex->fe_group = e4b->bd_group;

	/* calc difference from given start */
	next = next - ex->fe_start;
	ex->fe_len -= next;
	ex->fe_start += next;

	while (needed > ex->fe_len &&
	       mb_find_buddy(e4b, order, &max)) {

		if (block + 1 >= max)
			break;

		next = (block + 1) * (1 << order);
		if (mb_test_bit(next, e4b->bd_bitmap))
			break;

		order = mb_find_order_for_block(e4b, next);

		block = next >> order;
		ex->fe_len += 1 << order;
	}

	if (ex->fe_start + ex->fe_len > EXT4_CLUSTERS_PER_GROUP(e4b->bd_sb)) {
		/* Should never happen! (but apparently sometimes does?!?) */
		WARN_ON(1);
		ext4_grp_locked_error(e4b->bd_sb, e4b->bd_group, 0, 0,
			"corruption or bug in mb_find_extent "
			"block=%d, order=%d needed=%d ex=%u/%d/%d@%u",
			block, order, needed, ex->fe_group, ex->fe_start,
			ex->fe_len, ex->fe_logical);
		ex->fe_len = 0;
		ex->fe_start = 0;
		ex->fe_group = 0;
	}
	return ex->fe_len;
}

static int mb_mark_used(struct ext4_buddy *e4b, struct ext4_free_extent *ex)
{
	int ord;
	int mlen = 0;
	int max = 0;
	int cur;
	int start = ex->fe_start;
	int len = ex->fe_len;
	unsigned ret = 0;
	int len0 = len;
	void *buddy;
	bool split = false;

	BUG_ON(start + len > (e4b->bd_sb->s_blocksize << 3));
	BUG_ON(e4b->bd_group != ex->fe_group);
	assert_spin_locked(ext4_group_lock_ptr(e4b->bd_sb, e4b->bd_group));
	mb_check_buddy(e4b);
	mb_mark_used_double(e4b, start, len);

	this_cpu_inc(discard_pa_seq);
	e4b->bd_info->bb_free -= len;
	if (e4b->bd_info->bb_first_free == start)
		e4b->bd_info->bb_first_free += len;

	/* let's maintain fragments counter */
	if (start != 0)
		mlen = !mb_test_bit(start - 1, e4b->bd_bitmap);
	if (start + len < EXT4_SB(e4b->bd_sb)->s_mb_maxs[0])
		max = !mb_test_bit(start + len, e4b->bd_bitmap);
	if (mlen && max)
		e4b->bd_info->bb_fragments++;
	else if (!mlen && !max)
		e4b->bd_info->bb_fragments--;

	/* let's maintain buddy itself */
	while (len) {
		if (!split)
			ord = mb_find_order_for_block(e4b, start);

		if (((start >> ord) << ord) == start && len >= (1 << ord)) {
			/* the whole chunk may be allocated at once! */
			mlen = 1 << ord;
			if (!split)
				buddy = mb_find_buddy(e4b, ord, &max);
			else
				split = false;
			BUG_ON((start >> ord) >= max);
			mb_set_bit(start >> ord, buddy);
			e4b->bd_info->bb_counters[ord]--;
			start += mlen;
			len -= mlen;
			BUG_ON(len < 0);
			continue;
		}

		/* store for history */
		if (ret == 0)
			ret = len | (ord << 16);

		/* we have to split large buddy */
		BUG_ON(ord <= 0);
		buddy = mb_find_buddy(e4b, ord, &max);
		mb_set_bit(start >> ord, buddy);
		e4b->bd_info->bb_counters[ord]--;

		ord--;
		cur = (start >> ord) & ~1U;
		buddy = mb_find_buddy(e4b, ord, &max);
		mb_clear_bit(cur, buddy);
		mb_clear_bit(cur + 1, buddy);
		e4b->bd_info->bb_counters[ord]++;
		e4b->bd_info->bb_counters[ord]++;
		split = true;
	}
	mb_set_largest_free_order(e4b->bd_sb, e4b->bd_info);

	mb_update_avg_fragment_size(e4b->bd_sb, e4b->bd_info);
	mb_set_bits(e4b->bd_bitmap, ex->fe_start, len0);
	mb_check_buddy(e4b);

	return ret;
}

/*
 * Must be called under group lock!
 */
static void ext4_mb_use_best_found(struct ext4_allocation_context *ac,
					struct ext4_buddy *e4b)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	int ret;

	BUG_ON(ac->ac_b_ex.fe_group != e4b->bd_group);
	BUG_ON(ac->ac_status == AC_STATUS_FOUND);

	ac->ac_b_ex.fe_len = min(ac->ac_b_ex.fe_len, ac->ac_g_ex.fe_len);
	ac->ac_b_ex.fe_logical = ac->ac_g_ex.fe_logical;
	ret = mb_mark_used(e4b, &ac->ac_b_ex);

	/* preallocation can change ac_b_ex, thus we store actually
	 * allocated blocks for history */
	ac->ac_f_ex = ac->ac_b_ex;

	ac->ac_status = AC_STATUS_FOUND;
	ac->ac_tail = ret & 0xffff;
	ac->ac_buddy = ret >> 16;

	/*
	 * take the page reference. We want the page to be pinned
	 * so that we don't get a ext4_mb_init_cache_call for this
	 * group until we update the bitmap. That would mean we
	 * double allocate blocks. The reference is dropped
	 * in ext4_mb_release_context
	 */
	ac->ac_bitmap_page = e4b->bd_bitmap_page;
	get_page(ac->ac_bitmap_page);
	ac->ac_buddy_page = e4b->bd_buddy_page;
	get_page(ac->ac_buddy_page);
	/* store last allocated for subsequent stream allocation */
	if (ac->ac_flags & EXT4_MB_STREAM_ALLOC) {
		spin_lock(&sbi->s_md_lock);
		sbi->s_mb_last_group = ac->ac_f_ex.fe_group;
		sbi->s_mb_last_start = ac->ac_f_ex.fe_start;
		spin_unlock(&sbi->s_md_lock);
	}
	/*
	 * As we've just preallocated more space than
	 * user requested originally, we store allocated
	 * space in a special descriptor.
	 */
	if (ac->ac_o_ex.fe_len < ac->ac_b_ex.fe_len)
		ext4_mb_new_preallocation(ac);

}

static void ext4_mb_check_limits(struct ext4_allocation_context *ac,
					struct ext4_buddy *e4b,
					int finish_group)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct ext4_free_extent *bex = &ac->ac_b_ex;
	struct ext4_free_extent *gex = &ac->ac_g_ex;

	if (ac->ac_status == AC_STATUS_FOUND)
		return;
	/*
	 * We don't want to scan for a whole year
	 */
	if (ac->ac_found > sbi->s_mb_max_to_scan &&
			!(ac->ac_flags & EXT4_MB_HINT_FIRST)) {
		ac->ac_status = AC_STATUS_BREAK;
		return;
	}

	/*
	 * Haven't found good chunk so far, let's continue
	 */
	if (bex->fe_len < gex->fe_len)
		return;

	if (finish_group || ac->ac_found > sbi->s_mb_min_to_scan)
		ext4_mb_use_best_found(ac, e4b);
}

/*
 * The routine checks whether found extent is good enough. If it is,
 * then the extent gets marked used and flag is set to the context
 * to stop scanning. Otherwise, the extent is compared with the
 * previous found extent and if new one is better, then it's stored
 * in the context. Later, the best found extent will be used, if
 * mballoc can't find good enough extent.
 *
 * The algorithm used is roughly as follows:
 *
 * * If free extent found is exactly as big as goal, then
 *   stop the scan and use it immediately
 *
 * * If free extent found is smaller than goal, then keep retrying
 *   upto a max of sbi->s_mb_max_to_scan times (default 200). After
 *   that stop scanning and use whatever we have.
 *
 * * If free extent found is bigger than goal, then keep retrying
 *   upto a max of sbi->s_mb_min_to_scan times (default 10) before
 *   stopping the scan and using the extent.
 *
 *
 * FIXME: real allocation policy is to be designed yet!
 */
static void ext4_mb_measure_extent(struct ext4_allocation_context *ac,
					struct ext4_free_extent *ex,
					struct ext4_buddy *e4b)
{
	struct ext4_free_extent *bex = &ac->ac_b_ex;
	struct ext4_free_extent *gex = &ac->ac_g_ex;

	BUG_ON(ex->fe_len <= 0);
	BUG_ON(ex->fe_len > EXT4_CLUSTERS_PER_GROUP(ac->ac_sb));
	BUG_ON(ex->fe_start >= EXT4_CLUSTERS_PER_GROUP(ac->ac_sb));
	BUG_ON(ac->ac_status != AC_STATUS_CONTINUE);

	ac->ac_found++;
	ac->ac_cX_found[ac->ac_criteria]++;

	/*
	 * The special case - take what you catch first
	 */
	if (unlikely(ac->ac_flags & EXT4_MB_HINT_FIRST)) {
		*bex = *ex;
		ext4_mb_use_best_found(ac, e4b);
		return;
	}

	/*
	 * Let's check whether the chuck is good enough
	 */
	if (ex->fe_len == gex->fe_len) {
		*bex = *ex;
		ext4_mb_use_best_found(ac, e4b);
		return;
	}

	/*
	 * If this is first found extent, just store it in the context
	 */
	if (bex->fe_len == 0) {
		*bex = *ex;
		return;
	}

	/*
	 * If new found extent is better, store it in the context
	 */
	if (bex->fe_len < gex->fe_len) {
		/* if the request isn't satisfied, any found extent
		 * larger than previous best one is better */
		if (ex->fe_len > bex->fe_len)
			*bex = *ex;
	} else if (ex->fe_len > gex->fe_len) {
		/* if the request is satisfied, then we try to find
		 * an extent that still satisfy the request, but is
		 * smaller than previous one */
		if (ex->fe_len < bex->fe_len)
			*bex = *ex;
	}

	ext4_mb_check_limits(ac, e4b, 0);
}

static noinline_for_stack
void ext4_mb_try_best_found(struct ext4_allocation_context *ac,
					struct ext4_buddy *e4b)
{
	struct ext4_free_extent ex = ac->ac_b_ex;
	ext4_group_t group = ex.fe_group;
	int max;
	int err;

	BUG_ON(ex.fe_len <= 0);
	err = ext4_mb_load_buddy(ac->ac_sb, group, e4b);
	if (err)
		return;

	ext4_lock_group(ac->ac_sb, group);
	max = mb_find_extent(e4b, ex.fe_start, ex.fe_len, &ex);

	if (max > 0) {
		ac->ac_b_ex = ex;
		ext4_mb_use_best_found(ac, e4b);
	}

	ext4_unlock_group(ac->ac_sb, group);
	ext4_mb_unload_buddy(e4b);
}

static noinline_for_stack
int ext4_mb_find_by_goal(struct ext4_allocation_context *ac,
				struct ext4_buddy *e4b)
{
	ext4_group_t group = ac->ac_g_ex.fe_group;
	int max;
	int err;
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct ext4_group_info *grp = ext4_get_group_info(ac->ac_sb, group);
	struct ext4_free_extent ex;

	if (!grp)
		return -EFSCORRUPTED;
	if (!(ac->ac_flags & (EXT4_MB_HINT_TRY_GOAL | EXT4_MB_HINT_GOAL_ONLY)))
		return 0;
	if (grp->bb_free == 0)
		return 0;

	err = ext4_mb_load_buddy(ac->ac_sb, group, e4b);
	if (err)
		return err;

	if (unlikely(EXT4_MB_GRP_BBITMAP_CORRUPT(e4b->bd_info))) {
		ext4_mb_unload_buddy(e4b);
		return 0;
	}

	ext4_lock_group(ac->ac_sb, group);
	max = mb_find_extent(e4b, ac->ac_g_ex.fe_start,
			     ac->ac_g_ex.fe_len, &ex);
	ex.fe_logical = 0xDEADFA11; /* debug value */

	if (max >= ac->ac_g_ex.fe_len &&
	    ac->ac_g_ex.fe_len == EXT4_B2C(sbi, sbi->s_stripe)) {
		ext4_fsblk_t start;

		start = ext4_grp_offs_to_block(ac->ac_sb, &ex);
		/* use do_div to get remainder (would be 64-bit modulo) */
		if (do_div(start, sbi->s_stripe) == 0) {
			ac->ac_found++;
			ac->ac_b_ex = ex;
			ext4_mb_use_best_found(ac, e4b);
		}
	} else if (max >= ac->ac_g_ex.fe_len) {
		BUG_ON(ex.fe_len <= 0);
		BUG_ON(ex.fe_group != ac->ac_g_ex.fe_group);
		BUG_ON(ex.fe_start != ac->ac_g_ex.fe_start);
		ac->ac_found++;
		ac->ac_b_ex = ex;
		ext4_mb_use_best_found(ac, e4b);
	} else if (max > 0 && (ac->ac_flags & EXT4_MB_HINT_MERGE)) {
		/* Sometimes, caller may want to merge even small
		 * number of blocks to an existing extent */
		BUG_ON(ex.fe_len <= 0);
		BUG_ON(ex.fe_group != ac->ac_g_ex.fe_group);
		BUG_ON(ex.fe_start != ac->ac_g_ex.fe_start);
		ac->ac_found++;
		ac->ac_b_ex = ex;
		ext4_mb_use_best_found(ac, e4b);
	}
	ext4_unlock_group(ac->ac_sb, group);
	ext4_mb_unload_buddy(e4b);

	return 0;
}

/*
 * The routine scans buddy structures (not bitmap!) from given order
 * to max order and tries to find big enough chunk to satisfy the req
 */
static noinline_for_stack
void ext4_mb_simple_scan_group(struct ext4_allocation_context *ac,
					struct ext4_buddy *e4b)
{
	struct super_block *sb = ac->ac_sb;
	struct ext4_group_info *grp = e4b->bd_info;
	void *buddy;
	int i;
	int k;
	int max;

	BUG_ON(ac->ac_2order <= 0);
	for (i = ac->ac_2order; i < MB_NUM_ORDERS(sb); i++) {
		if (grp->bb_counters[i] == 0)
			continue;

		buddy = mb_find_buddy(e4b, i, &max);
		if (WARN_RATELIMIT(buddy == NULL,
			 "ext4: mb_simple_scan_group: mb_find_buddy failed, (%d)\n", i))
			continue;

		k = mb_find_next_zero_bit(buddy, max, 0);
		if (k >= max) {
			ext4_grp_locked_error(ac->ac_sb, e4b->bd_group, 0, 0,
				"%d free clusters of order %d. But found 0",
				grp->bb_counters[i], i);
			ext4_mark_group_bitmap_corrupted(ac->ac_sb,
					 e4b->bd_group,
					EXT4_GROUP_INFO_BBITMAP_CORRUPT);
			break;
		}
		ac->ac_found++;
		ac->ac_cX_found[ac->ac_criteria]++;

		ac->ac_b_ex.fe_len = 1 << i;
		ac->ac_b_ex.fe_start = k << i;
		ac->ac_b_ex.fe_group = e4b->bd_group;

		ext4_mb_use_best_found(ac, e4b);

		BUG_ON(ac->ac_f_ex.fe_len != ac->ac_g_ex.fe_len);

		if (EXT4_SB(sb)->s_mb_stats)
			atomic_inc(&EXT4_SB(sb)->s_bal_2orders);

		break;
	}
}

/*
 * The routine scans the group and measures all found extents.
 * In order to optimize scanning, caller must pass number of
 * free blocks in the group, so the routine can know upper limit.
 */
static noinline_for_stack
void ext4_mb_complex_scan_group(struct ext4_allocation_context *ac,
					struct ext4_buddy *e4b)
{
	struct super_block *sb = ac->ac_sb;
	void *bitmap = e4b->bd_bitmap;
	struct ext4_free_extent ex;
	int i, j, freelen;
	int free;

	free = e4b->bd_info->bb_free;
	if (WARN_ON(free <= 0))
		return;

	i = e4b->bd_info->bb_first_free;

	while (free && ac->ac_status == AC_STATUS_CONTINUE) {
		i = mb_find_next_zero_bit(bitmap,
						EXT4_CLUSTERS_PER_GROUP(sb), i);
		if (i >= EXT4_CLUSTERS_PER_GROUP(sb)) {
			/*
			 * IF we have corrupt bitmap, we won't find any
			 * free blocks even though group info says we
			 * have free blocks
			 */
			ext4_grp_locked_error(sb, e4b->bd_group, 0, 0,
					"%d free clusters as per "
					"group info. But bitmap says 0",
					free);
			ext4_mark_group_bitmap_corrupted(sb, e4b->bd_group,
					EXT4_GROUP_INFO_BBITMAP_CORRUPT);
			break;
		}

		if (!ext4_mb_cr_expensive(ac->ac_criteria)) {
			/*
			 * In CR_GOAL_LEN_FAST and CR_BEST_AVAIL_LEN, we are
			 * sure that this group will have a large enough
			 * continuous free extent, so skip over the smaller free
			 * extents
			 */
			j = mb_find_next_bit(bitmap,
						EXT4_CLUSTERS_PER_GROUP(sb), i);
			freelen = j - i;

			if (freelen < ac->ac_g_ex.fe_len) {
				i = j;
				free -= freelen;
				continue;
			}
		}

		mb_find_extent(e4b, i, ac->ac_g_ex.fe_len, &ex);
		if (WARN_ON(ex.fe_len <= 0))
			break;
		if (free < ex.fe_len) {
			ext4_grp_locked_error(sb, e4b->bd_group, 0, 0,
					"%d free clusters as per "
					"group info. But got %d blocks",
					free, ex.fe_len);
			ext4_mark_group_bitmap_corrupted(sb, e4b->bd_group,
					EXT4_GROUP_INFO_BBITMAP_CORRUPT);
			/*
			 * The number of free blocks differs. This mostly
			 * indicate that the bitmap is corrupt. So exit
			 * without claiming the space.
			 */
			break;
		}
		ex.fe_logical = 0xDEADC0DE; /* debug value */
		ext4_mb_measure_extent(ac, &ex, e4b);

		i += ex.fe_len;
		free -= ex.fe_len;
	}

	ext4_mb_check_limits(ac, e4b, 1);
}

/*
 * This is a special case for storages like raid5
 * we try to find stripe-aligned chunks for stripe-size-multiple requests
 */
static noinline_for_stack
void ext4_mb_scan_aligned(struct ext4_allocation_context *ac,
				 struct ext4_buddy *e4b)
{
	struct super_block *sb = ac->ac_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	void *bitmap = e4b->bd_bitmap;
	struct ext4_free_extent ex;
	ext4_fsblk_t first_group_block;
	ext4_fsblk_t a;
	ext4_grpblk_t i, stripe;
	int max;

	BUG_ON(sbi->s_stripe == 0);

	/* find first stripe-aligned block in group */
	first_group_block = ext4_group_first_block_no(sb, e4b->bd_group);

	a = first_group_block + sbi->s_stripe - 1;
	do_div(a, sbi->s_stripe);
	i = (a * sbi->s_stripe) - first_group_block;

	stripe = EXT4_B2C(sbi, sbi->s_stripe);
	i = EXT4_B2C(sbi, i);
	while (i < EXT4_CLUSTERS_PER_GROUP(sb)) {
		if (!mb_test_bit(i, bitmap)) {
			max = mb_find_extent(e4b, i, stripe, &ex);
			if (max >= stripe) {
				ac->ac_found++;
				ac->ac_cX_found[ac->ac_criteria]++;
				ex.fe_logical = 0xDEADF00D; /* debug value */
				ac->ac_b_ex = ex;
				ext4_mb_use_best_found(ac, e4b);
				break;
			}
		}
		i += stripe;
	}
}

/*
 * This is also called BEFORE we load the buddy bitmap.
 * Returns either 1 or 0 indicating that the group is either suitable
 * for the allocation or not.
 */
static bool ext4_mb_good_group(struct ext4_allocation_context *ac,
				ext4_group_t group, enum criteria cr)
{
	ext4_grpblk_t free, fragments;
	int flex_size = ext4_flex_bg_size(EXT4_SB(ac->ac_sb));
	struct ext4_group_info *grp = ext4_get_group_info(ac->ac_sb, group);

	BUG_ON(cr < CR_POWER2_ALIGNED || cr >= EXT4_MB_NUM_CRS);

	if (unlikely(!grp || EXT4_MB_GRP_BBITMAP_CORRUPT(grp)))
		return false;

	free = grp->bb_free;
	if (free == 0)
		return false;

	fragments = grp->bb_fragments;
	if (fragments == 0)
		return false;

	switch (cr) {
	case CR_POWER2_ALIGNED:
		BUG_ON(ac->ac_2order == 0);

		/* Avoid using the first bg of a flexgroup for data files */
		if ((ac->ac_flags & EXT4_MB_HINT_DATA) &&
		    (flex_size >= EXT4_FLEX_SIZE_DIR_ALLOC_SCHEME) &&
		    ((group % flex_size) == 0))
			return false;

		if (free < ac->ac_g_ex.fe_len)
			return false;

		if (ac->ac_2order >= MB_NUM_ORDERS(ac->ac_sb))
			return true;

		if (grp->bb_largest_free_order < ac->ac_2order)
			return false;

		return true;
	case CR_GOAL_LEN_FAST:
	case CR_BEST_AVAIL_LEN:
		if ((free / fragments) >= ac->ac_g_ex.fe_len)
			return true;
		break;
	case CR_GOAL_LEN_SLOW:
		if (free >= ac->ac_g_ex.fe_len)
			return true;
		break;
	case CR_ANY_FREE:
		return true;
	default:
		BUG();
	}

	return false;
}

/*
 * This could return negative error code if something goes wrong
 * during ext4_mb_init_group(). This should not be called with
 * ext4_lock_group() held.
 *
 * Note: because we are conditionally operating with the group lock in
 * the EXT4_MB_STRICT_CHECK case, we need to fake out sparse in this
 * function using __acquire and __release.  This means we need to be
 * super careful before messing with the error path handling via "goto
 * out"!
 */
static int ext4_mb_good_group_nolock(struct ext4_allocation_context *ac,
				     ext4_group_t group, enum criteria cr)
{
	struct ext4_group_info *grp = ext4_get_group_info(ac->ac_sb, group);
	struct super_block *sb = ac->ac_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	bool should_lock = ac->ac_flags & EXT4_MB_STRICT_CHECK;
	ext4_grpblk_t free;
	int ret = 0;

	if (!grp)
		return -EFSCORRUPTED;
	if (sbi->s_mb_stats)
		atomic64_inc(&sbi->s_bal_cX_groups_considered[ac->ac_criteria]);
	if (should_lock) {
		ext4_lock_group(sb, group);
		__release(ext4_group_lock_ptr(sb, group));
	}
	free = grp->bb_free;
	if (free == 0)
		goto out;
	/*
	 * In all criterias except CR_ANY_FREE we try to avoid groups that
	 * can't possibly satisfy the full goal request due to insufficient
	 * free blocks.
	 */
	if (cr < CR_ANY_FREE && free < ac->ac_g_ex.fe_len)
		goto out;
	if (unlikely(EXT4_MB_GRP_BBITMAP_CORRUPT(grp)))
		goto out;
	if (should_lock) {
		__acquire(ext4_group_lock_ptr(sb, group));
		ext4_unlock_group(sb, group);
	}

	/* We only do this if the grp has never been initialized */
	if (unlikely(EXT4_MB_GRP_NEED_INIT(grp))) {
		struct ext4_group_desc *gdp =
			ext4_get_group_desc(sb, group, NULL);
		int ret;

		/*
		 * cr=CR_POWER2_ALIGNED/CR_GOAL_LEN_FAST is a very optimistic
		 * search to find large good chunks almost for free. If buddy
		 * data is not ready, then this optimization makes no sense. But
		 * we never skip the first block group in a flex_bg, since this
		 * gets used for metadata block allocation, and we want to make
		 * sure we locate metadata blocks in the first block group in
		 * the flex_bg if possible.
		 */
		if (!ext4_mb_cr_expensive(cr) &&
		    (!sbi->s_log_groups_per_flex ||
		     ((group & ((1 << sbi->s_log_groups_per_flex) - 1)) != 0)) &&
		    !(ext4_has_group_desc_csum(sb) &&
		      (gdp->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT))))
			return 0;
		ret = ext4_mb_init_group(sb, group, GFP_NOFS);
		if (ret)
			return ret;
	}

	if (should_lock) {
		ext4_lock_group(sb, group);
		__release(ext4_group_lock_ptr(sb, group));
	}
	ret = ext4_mb_good_group(ac, group, cr);
out:
	if (should_lock) {
		__acquire(ext4_group_lock_ptr(sb, group));
		ext4_unlock_group(sb, group);
	}
	return ret;
}

/*
 * Start prefetching @nr block bitmaps starting at @group.
 * Return the next group which needs to be prefetched.
 */
ext4_group_t ext4_mb_prefetch(struct super_block *sb, ext4_group_t group,
			      unsigned int nr, int *cnt)
{
	ext4_group_t ngroups = ext4_get_groups_count(sb);
	struct buffer_head *bh;
	struct blk_plug plug;

	blk_start_plug(&plug);
	while (nr-- > 0) {
		struct ext4_group_desc *gdp = ext4_get_group_desc(sb, group,
								  NULL);
		struct ext4_group_info *grp = ext4_get_group_info(sb, group);

		/*
		 * Prefetch block groups with free blocks; but don't
		 * bother if it is marked uninitialized on disk, since
		 * it won't require I/O to read.  Also only try to
		 * prefetch once, so we avoid getblk() call, which can
		 * be expensive.
		 */
		if (gdp && grp && !EXT4_MB_GRP_TEST_AND_SET_READ(grp) &&
		    EXT4_MB_GRP_NEED_INIT(grp) &&
		    ext4_free_group_clusters(sb, gdp) > 0 ) {
			bh = ext4_read_block_bitmap_nowait(sb, group, true);
			if (bh && !IS_ERR(bh)) {
				if (!buffer_uptodate(bh) && cnt)
					(*cnt)++;
				brelse(bh);
			}
		}
		if (++group >= ngroups)
			group = 0;
	}
	blk_finish_plug(&plug);
	return group;
}

/*
 * Prefetching reads the block bitmap into the buffer cache; but we
 * need to make sure that the buddy bitmap in the page cache has been
 * initialized.  Note that ext4_mb_init_group() will block if the I/O
 * is not yet completed, or indeed if it was not initiated by
 * ext4_mb_prefetch did not start the I/O.
 *
 * TODO: We should actually kick off the buddy bitmap setup in a work
 * queue when the buffer I/O is completed, so that we don't block
 * waiting for the block allocation bitmap read to finish when
 * ext4_mb_prefetch_fini is called from ext4_mb_regular_allocator().
 */
void ext4_mb_prefetch_fini(struct super_block *sb, ext4_group_t group,
			   unsigned int nr)
{
	struct ext4_group_desc *gdp;
	struct ext4_group_info *grp;

	while (nr-- > 0) {
		if (!group)
			group = ext4_get_groups_count(sb);
		group--;
		gdp = ext4_get_group_desc(sb, group, NULL);
		grp = ext4_get_group_info(sb, group);

		if (grp && gdp && EXT4_MB_GRP_NEED_INIT(grp) &&
		    ext4_free_group_clusters(sb, gdp) > 0) {
			if (ext4_mb_init_group(sb, group, GFP_NOFS))
				break;
		}
	}
}

static noinline_for_stack int
ext4_mb_regular_allocator(struct ext4_allocation_context *ac)
{
	ext4_group_t prefetch_grp = 0, ngroups, group, i;
	enum criteria new_cr, cr = CR_GOAL_LEN_FAST;
	int err = 0, first_err = 0;
	unsigned int nr = 0, prefetch_ios = 0;
	struct ext4_sb_info *sbi;
	struct super_block *sb;
	struct ext4_buddy e4b;
	int lost;

	sb = ac->ac_sb;
	sbi = EXT4_SB(sb);
	ngroups = ext4_get_groups_count(sb);
	/* non-extent files are limited to low blocks/groups */
	if (!(ext4_test_inode_flag(ac->ac_inode, EXT4_INODE_EXTENTS)))
		ngroups = sbi->s_blockfile_groups;

	BUG_ON(ac->ac_status == AC_STATUS_FOUND);

	/* first, try the goal */
	err = ext4_mb_find_by_goal(ac, &e4b);
	if (err || ac->ac_status == AC_STATUS_FOUND)
		goto out;

	if (unlikely(ac->ac_flags & EXT4_MB_HINT_GOAL_ONLY))
		goto out;

	/*
	 * ac->ac_2order is set only if the fe_len is a power of 2
	 * if ac->ac_2order is set we also set criteria to CR_POWER2_ALIGNED
	 * so that we try exact allocation using buddy.
	 */
	i = fls(ac->ac_g_ex.fe_len);
	ac->ac_2order = 0;
	/*
	 * We search using buddy data only if the order of the request
	 * is greater than equal to the sbi_s_mb_order2_reqs
	 * You can tune it via /sys/fs/ext4/<partition>/mb_order2_req
	 * We also support searching for power-of-two requests only for
	 * requests upto maximum buddy size we have constructed.
	 */
	if (i >= sbi->s_mb_order2_reqs && i <= MB_NUM_ORDERS(sb)) {
		if (is_power_of_2(ac->ac_g_ex.fe_len))
			ac->ac_2order = array_index_nospec(i - 1,
							   MB_NUM_ORDERS(sb));
	}

	/* if stream allocation is enabled, use global goal */
	if (ac->ac_flags & EXT4_MB_STREAM_ALLOC) {
		/* TBD: may be hot point */
		spin_lock(&sbi->s_md_lock);
		ac->ac_g_ex.fe_group = sbi->s_mb_last_group;
		ac->ac_g_ex.fe_start = sbi->s_mb_last_start;
		spin_unlock(&sbi->s_md_lock);
	}

	/*
	 * Let's just scan groups to find more-less suitable blocks We
	 * start with CR_GOAL_LEN_FAST, unless it is power of 2
	 * aligned, in which case let's do that faster approach first.
	 */
	if (ac->ac_2order)
		cr = CR_POWER2_ALIGNED;
repeat:
	for (; cr < EXT4_MB_NUM_CRS && ac->ac_status == AC_STATUS_CONTINUE; cr++) {
		ac->ac_criteria = cr;
		/*
		 * searching for the right group start
		 * from the goal value specified
		 */
		group = ac->ac_g_ex.fe_group;
		ac->ac_groups_linear_remaining = sbi->s_mb_max_linear_groups;
		prefetch_grp = group;

		for (i = 0, new_cr = cr; i < ngroups; i++,
		     ext4_mb_choose_next_group(ac, &new_cr, &group, ngroups)) {
			int ret = 0;

			cond_resched();
			if (new_cr != cr) {
				cr = new_cr;
				goto repeat;
			}

			/*
			 * Batch reads of the block allocation bitmaps
			 * to get multiple READs in flight; limit
			 * prefetching at inexpensive CR, otherwise mballoc
			 * can spend a lot of time loading imperfect groups
			 */
			if ((prefetch_grp == group) &&
			    (ext4_mb_cr_expensive(cr) ||
			     prefetch_ios < sbi->s_mb_prefetch_limit)) {
				nr = sbi->s_mb_prefetch;
				if (ext4_has_feature_flex_bg(sb)) {
					nr = 1 << sbi->s_log_groups_per_flex;
					nr -= group & (nr - 1);
					nr = min(nr, sbi->s_mb_prefetch);
				}
				prefetch_grp = ext4_mb_prefetch(sb, group,
							nr, &prefetch_ios);
			}

			/* This now checks without needing the buddy page */
			ret = ext4_mb_good_group_nolock(ac, group, cr);
			if (ret <= 0) {
				if (!first_err)
					first_err = ret;
				continue;
			}

			err = ext4_mb_load_buddy(sb, group, &e4b);
			if (err)
				goto out;

			ext4_lock_group(sb, group);

			/*
			 * We need to check again after locking the
			 * block group
			 */
			ret = ext4_mb_good_group(ac, group, cr);
			if (ret == 0) {
				ext4_unlock_group(sb, group);
				ext4_mb_unload_buddy(&e4b);
				continue;
			}

			ac->ac_groups_scanned++;
			if (cr == CR_POWER2_ALIGNED)
				ext4_mb_simple_scan_group(ac, &e4b);
			else if ((cr == CR_GOAL_LEN_FAST ||
				 cr == CR_BEST_AVAIL_LEN) &&
				 sbi->s_stripe &&
				 !(ac->ac_g_ex.fe_len %
				 EXT4_B2C(sbi, sbi->s_stripe)))
				ext4_mb_scan_aligned(ac, &e4b);
			else
				ext4_mb_complex_scan_group(ac, &e4b);

			ext4_unlock_group(sb, group);
			ext4_mb_unload_buddy(&e4b);

			if (ac->ac_status != AC_STATUS_CONTINUE)
				break;
		}
		/* Processed all groups and haven't found blocks */
		if (sbi->s_mb_stats && i == ngroups)
			atomic64_inc(&sbi->s_bal_cX_failed[cr]);

		if (i == ngroups && ac->ac_criteria == CR_BEST_AVAIL_LEN)
			/* Reset goal length to original goal length before
			 * falling into CR_GOAL_LEN_SLOW */
			ac->ac_g_ex.fe_len = ac->ac_orig_goal_len;
	}

	if (ac->ac_b_ex.fe_len > 0 && ac->ac_status != AC_STATUS_FOUND &&
	    !(ac->ac_flags & EXT4_MB_HINT_FIRST)) {
		/*
		 * We've been searching too long. Let's try to allocate
		 * the best chunk we've found so far
		 */
		ext4_mb_try_best_found(ac, &e4b);
		if (ac->ac_status != AC_STATUS_FOUND) {
			/*
			 * Someone more lucky has already allocated it.
			 * The only thing we can do is just take first
			 * found block(s)
			 */
			lost = atomic_inc_return(&sbi->s_mb_lost_chunks);
			mb_debug(sb, "lost chunk, group: %u, start: %d, len: %d, lost: %d\n",
				 ac->ac_b_ex.fe_group, ac->ac_b_ex.fe_start,
				 ac->ac_b_ex.fe_len, lost);

			ac->ac_b_ex.fe_group = 0;
			ac->ac_b_ex.fe_start = 0;
			ac->ac_b_ex.fe_len = 0;
			ac->ac_status = AC_STATUS_CONTINUE;
			ac->ac_flags |= EXT4_MB_HINT_FIRST;
			cr = CR_ANY_FREE;
			goto repeat;
		}
	}

	if (sbi->s_mb_stats && ac->ac_status == AC_STATUS_FOUND)
		atomic64_inc(&sbi->s_bal_cX_hits[ac->ac_criteria]);
out:
	if (!err && ac->ac_status != AC_STATUS_FOUND && first_err)
		err = first_err;

	mb_debug(sb, "Best len %d, origin len %d, ac_status %u, ac_flags 0x%x, cr %d ret %d\n",
		 ac->ac_b_ex.fe_len, ac->ac_o_ex.fe_len, ac->ac_status,
		 ac->ac_flags, cr, err);

	if (nr)
		ext4_mb_prefetch_fini(sb, prefetch_grp, nr);

	return err;
}

static void *ext4_mb_seq_groups_start(struct seq_file *seq, loff_t *pos)
{
	struct super_block *sb = pde_data(file_inode(seq->file));
	ext4_group_t group;

	if (*pos < 0 || *pos >= ext4_get_groups_count(sb))
		return NULL;
	group = *pos + 1;
	return (void *) ((unsigned long) group);
}

static void *ext4_mb_seq_groups_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct super_block *sb = pde_data(file_inode(seq->file));
	ext4_group_t group;

	++*pos;
	if (*pos < 0 || *pos >= ext4_get_groups_count(sb))
		return NULL;
	group = *pos + 1;
	return (void *) ((unsigned long) group);
}

static int ext4_mb_seq_groups_show(struct seq_file *seq, void *v)
{
	struct super_block *sb = pde_data(file_inode(seq->file));
	ext4_group_t group = (ext4_group_t) ((unsigned long) v);
	int i;
	int err, buddy_loaded = 0;
	struct ext4_buddy e4b;
	struct ext4_group_info *grinfo;
	unsigned char blocksize_bits = min_t(unsigned char,
					     sb->s_blocksize_bits,
					     EXT4_MAX_BLOCK_LOG_SIZE);
	struct sg {
		struct ext4_group_info info;
		ext4_grpblk_t counters[EXT4_MAX_BLOCK_LOG_SIZE + 2];
	} sg;

	group--;
	if (group == 0)
		seq_puts(seq, "#group: free  frags first ["
			      " 2^0   2^1   2^2   2^3   2^4   2^5   2^6  "
			      " 2^7   2^8   2^9   2^10  2^11  2^12  2^13  ]\n");

	i = (blocksize_bits + 2) * sizeof(sg.info.bb_counters[0]) +
		sizeof(struct ext4_group_info);

	grinfo = ext4_get_group_info(sb, group);
	if (!grinfo)
		return 0;
	/* Load the group info in memory only if not already loaded. */
	if (unlikely(EXT4_MB_GRP_NEED_INIT(grinfo))) {
		err = ext4_mb_load_buddy(sb, group, &e4b);
		if (err) {
			seq_printf(seq, "#%-5u: I/O error\n", group);
			return 0;
		}
		buddy_loaded = 1;
	}

	memcpy(&sg, grinfo, i);

	if (buddy_loaded)
		ext4_mb_unload_buddy(&e4b);

	seq_printf(seq, "#%-5u: %-5u %-5u %-5u [", group, sg.info.bb_free,
			sg.info.bb_fragments, sg.info.bb_first_free);
	for (i = 0; i <= 13; i++)
		seq_printf(seq, " %-5u", i <= blocksize_bits + 1 ?
				sg.info.bb_counters[i] : 0);
	seq_puts(seq, " ]\n");

	return 0;
}

static void ext4_mb_seq_groups_stop(struct seq_file *seq, void *v)
{
}

const struct seq_operations ext4_mb_seq_groups_ops = {
	.start  = ext4_mb_seq_groups_start,
	.next   = ext4_mb_seq_groups_next,
	.stop   = ext4_mb_seq_groups_stop,
	.show   = ext4_mb_seq_groups_show,
};

int ext4_seq_mb_stats_show(struct seq_file *seq, void *offset)
{
	struct super_block *sb = seq->private;
	struct ext4_sb_info *sbi = EXT4_SB(sb);

	seq_puts(seq, "mballoc:\n");
	if (!sbi->s_mb_stats) {
		seq_puts(seq, "\tmb stats collection turned off.\n");
		seq_puts(
			seq,
			"\tTo enable, please write \"1\" to sysfs file mb_stats.\n");
		return 0;
	}
	seq_printf(seq, "\treqs: %u\n", atomic_read(&sbi->s_bal_reqs));
	seq_printf(seq, "\tsuccess: %u\n", atomic_read(&sbi->s_bal_success));

	seq_printf(seq, "\tgroups_scanned: %u\n",
		   atomic_read(&sbi->s_bal_groups_scanned));

	/* CR_POWER2_ALIGNED stats */
	seq_puts(seq, "\tcr_p2_aligned_stats:\n");
	seq_printf(seq, "\t\thits: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_hits[CR_POWER2_ALIGNED]));
	seq_printf(
		seq, "\t\tgroups_considered: %llu\n",
		atomic64_read(
			&sbi->s_bal_cX_groups_considered[CR_POWER2_ALIGNED]));
	seq_printf(seq, "\t\textents_scanned: %u\n",
		   atomic_read(&sbi->s_bal_cX_ex_scanned[CR_POWER2_ALIGNED]));
	seq_printf(seq, "\t\tuseless_loops: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_failed[CR_POWER2_ALIGNED]));
	seq_printf(seq, "\t\tbad_suggestions: %u\n",
		   atomic_read(&sbi->s_bal_p2_aligned_bad_suggestions));

	/* CR_GOAL_LEN_FAST stats */
	seq_puts(seq, "\tcr_goal_fast_stats:\n");
	seq_printf(seq, "\t\thits: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_hits[CR_GOAL_LEN_FAST]));
	seq_printf(seq, "\t\tgroups_considered: %llu\n",
		   atomic64_read(
			   &sbi->s_bal_cX_groups_considered[CR_GOAL_LEN_FAST]));
	seq_printf(seq, "\t\textents_scanned: %u\n",
		   atomic_read(&sbi->s_bal_cX_ex_scanned[CR_GOAL_LEN_FAST]));
	seq_printf(seq, "\t\tuseless_loops: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_failed[CR_GOAL_LEN_FAST]));
	seq_printf(seq, "\t\tbad_suggestions: %u\n",
		   atomic_read(&sbi->s_bal_goal_fast_bad_suggestions));

	/* CR_BEST_AVAIL_LEN stats */
	seq_puts(seq, "\tcr_best_avail_stats:\n");
	seq_printf(seq, "\t\thits: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_hits[CR_BEST_AVAIL_LEN]));
	seq_printf(
		seq, "\t\tgroups_considered: %llu\n",
		atomic64_read(
			&sbi->s_bal_cX_groups_considered[CR_BEST_AVAIL_LEN]));
	seq_printf(seq, "\t\textents_scanned: %u\n",
		   atomic_read(&sbi->s_bal_cX_ex_scanned[CR_BEST_AVAIL_LEN]));
	seq_printf(seq, "\t\tuseless_loops: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_failed[CR_BEST_AVAIL_LEN]));
	seq_printf(seq, "\t\tbad_suggestions: %u\n",
		   atomic_read(&sbi->s_bal_best_avail_bad_suggestions));

	/* CR_GOAL_LEN_SLOW stats */
	seq_puts(seq, "\tcr_goal_slow_stats:\n");
	seq_printf(seq, "\t\thits: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_hits[CR_GOAL_LEN_SLOW]));
	seq_printf(seq, "\t\tgroups_considered: %llu\n",
		   atomic64_read(
			   &sbi->s_bal_cX_groups_considered[CR_GOAL_LEN_SLOW]));
	seq_printf(seq, "\t\textents_scanned: %u\n",
		   atomic_read(&sbi->s_bal_cX_ex_scanned[CR_GOAL_LEN_SLOW]));
	seq_printf(seq, "\t\tuseless_loops: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_failed[CR_GOAL_LEN_SLOW]));

	/* CR_ANY_FREE stats */
	seq_puts(seq, "\tcr_any_free_stats:\n");
	seq_printf(seq, "\t\thits: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_hits[CR_ANY_FREE]));
	seq_printf(
		seq, "\t\tgroups_considered: %llu\n",
		atomic64_read(&sbi->s_bal_cX_groups_considered[CR_ANY_FREE]));
	seq_printf(seq, "\t\textents_scanned: %u\n",
		   atomic_read(&sbi->s_bal_cX_ex_scanned[CR_ANY_FREE]));
	seq_printf(seq, "\t\tuseless_loops: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_failed[CR_ANY_FREE]));

	/* Aggregates */
	seq_printf(seq, "\textents_scanned: %u\n",
		   atomic_read(&sbi->s_bal_ex_scanned));
	seq_printf(seq, "\t\tgoal_hits: %u\n", atomic_read(&sbi->s_bal_goals));
	seq_printf(seq, "\t\tlen_goal_hits: %u\n",
		   atomic_read(&sbi->s_bal_len_goals));
	seq_printf(seq, "\t\t2^n_hits: %u\n", atomic_read(&sbi->s_bal_2orders));
	seq_printf(seq, "\t\tbreaks: %u\n", atomic_read(&sbi->s_bal_breaks));
	seq_printf(seq, "\t\tlost: %u\n", atomic_read(&sbi->s_mb_lost_chunks));
	seq_printf(seq, "\tbuddies_generated: %u/%u\n",
		   atomic_read(&sbi->s_mb_buddies_generated),
		   ext4_get_groups_count(sb));
	seq_printf(seq, "\tbuddies_time_used: %llu\n",
		   atomic64_read(&sbi->s_mb_generation_time));
	seq_printf(seq, "\tpreallocated: %u\n",
		   atomic_read(&sbi->s_mb_preallocated));
	seq_printf(seq, "\tdiscarded: %u\n", atomic_read(&sbi->s_mb_discarded));
	return 0;
}

static void *ext4_mb_seq_structs_summary_start(struct seq_file *seq, loff_t *pos)
__acquires(&EXT4_SB(sb)->s_mb_rb_lock)
{
	struct super_block *sb = pde_data(file_inode(seq->file));
	unsigned long position;

	if (*pos < 0 || *pos >= 2*MB_NUM_ORDERS(sb))
		return NULL;
	position = *pos + 1;
	return (void *) ((unsigned long) position);
}

static void *ext4_mb_seq_structs_summary_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct super_block *sb = pde_data(file_inode(seq->file));
	unsigned long position;

	++*pos;
	if (*pos < 0 || *pos >= 2*MB_NUM_ORDERS(sb))
		return NULL;
	position = *pos + 1;
	return (void *) ((unsigned long) position);
}

static int ext4_mb_seq_structs_summary_show(struct seq_file *seq, void *v)
{
	struct super_block *sb = pde_data(file_inode(seq->file));
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	unsigned long position = ((unsigned long) v);
	struct ext4_group_info *grp;
	unsigned int count;

	position--;
	if (position >= MB_NUM_ORDERS(sb)) {
		position -= MB_NUM_ORDERS(sb);
		if (position == 0)
			seq_puts(seq, "avg_fragment_size_lists:\n");

		count = 0;
		read_lock(&sbi->s_mb_avg_fragment_size_locks[position]);
		list_for_each_entry(grp, &sbi->s_mb_avg_fragment_size[position],
				    bb_avg_fragment_size_node)
			count++;
		read_unlock(&sbi->s_mb_avg_fragment_size_locks[position]);
		seq_printf(seq, "\tlist_order_%u_groups: %u\n",
					(unsigned int)position, count);
		return 0;
	}

	if (position == 0) {
		seq_printf(seq, "optimize_scan: %d\n",
			   test_opt2(sb, MB_OPTIMIZE_SCAN) ? 1 : 0);
		seq_puts(seq, "max_free_order_lists:\n");
	}
	count = 0;
	read_lock(&sbi->s_mb_largest_free_orders_locks[position]);
	list_for_each_entry(grp, &sbi->s_mb_largest_free_orders[position],
			    bb_largest_free_order_node)
		count++;
	read_unlock(&sbi->s_mb_largest_free_orders_locks[position]);
	seq_printf(seq, "\tlist_order_%u_groups: %u\n",
		   (unsigned int)position, count);

	return 0;
}

static void ext4_mb_seq_structs_summary_stop(struct seq_file *seq, void *v)
{
}

const struct seq_operations ext4_mb_seq_structs_summary_ops = {
	.start  = ext4_mb_seq_structs_summary_start,
	.next   = ext4_mb_seq_structs_summary_next,
	.stop   = ext4_mb_seq_structs_summary_stop,
	.show   = ext4_mb_seq_structs_summary_show,
};

static struct kmem_cache *get_groupinfo_cache(int blocksize_bits)
{
	int cache_index = blocksize_bits - EXT4_MIN_BLOCK_LOG_SIZE;
	struct kmem_cache *cachep = ext4_groupinfo_caches[cache_index];

	BUG_ON(!cachep);
	return cachep;
}

/*
 * Allocate the top-level s_group_info array for the specified number
 * of groups
 */
int ext4_mb_alloc_groupinfo(struct super_block *sb, ext4_group_t ngroups)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	unsigned size;
	struct ext4_group_info ***old_groupinfo, ***new_groupinfo;

	size = (ngroups + EXT4_DESC_PER_BLOCK(sb) - 1) >>
		EXT4_DESC_PER_BLOCK_BITS(sb);
	if (size <= sbi->s_group_info_size)
		return 0;

	size = roundup_pow_of_two(sizeof(*sbi->s_group_info) * size);
	new_groupinfo = kvzalloc(size, GFP_KERNEL);
	if (!new_groupinfo) {
		ext4_msg(sb, KERN_ERR, "can't allocate buddy meta group");
		return -ENOMEM;
	}
	rcu_read_lock();
	old_groupinfo = rcu_dereference(sbi->s_group_info);
	if (old_groupinfo)
		memcpy(new_groupinfo, old_groupinfo,
		       sbi->s_group_info_size * sizeof(*sbi->s_group_info));
	rcu_read_unlock();
	rcu_assign_pointer(sbi->s_group_info, new_groupinfo);
	sbi->s_group_info_size = size / sizeof(*sbi->s_group_info);
	if (old_groupinfo)
		ext4_kvfree_array_rcu(old_groupinfo);
	ext4_debug("allocated s_groupinfo array for %d meta_bg's\n",
		   sbi->s_group_info_size);
	return 0;
}

/* Create and initialize ext4_group_info data for the given group. */
int ext4_mb_add_groupinfo(struct super_block *sb, ext4_group_t group,
			  struct ext4_group_desc *desc)
{
	int i;
	int metalen = 0;
	int idx = group >> EXT4_DESC_PER_BLOCK_BITS(sb);
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_group_info **meta_group_info;
	struct kmem_cache *cachep = get_groupinfo_cache(sb->s_blocksize_bits);

	/*
	 * First check if this group is the first of a reserved block.
	 * If it's true, we have to allocate a new table of pointers
	 * to ext4_group_info structures
	 */
	if (group % EXT4_DESC_PER_BLOCK(sb) == 0) {
		metalen = sizeof(*meta_group_info) <<
			EXT4_DESC_PER_BLOCK_BITS(sb);
		meta_group_info = kmalloc(metalen, GFP_NOFS);
		if (meta_group_info == NULL) {
			ext4_msg(sb, KERN_ERR, "can't allocate mem "
				 "for a buddy group");
			return -ENOMEM;
		}
		rcu_read_lock();
		rcu_dereference(sbi->s_group_info)[idx] = meta_group_info;
		rcu_read_unlock();
	}

	meta_group_info = sbi_array_rcu_deref(sbi, s_group_info, idx);
	i = group & (EXT4_DESC_PER_BLOCK(sb) - 1);

	meta_group_info[i] = kmem_cache_zalloc(cachep, GFP_NOFS);
	if (meta_group_info[i] == NULL) {
		ext4_msg(sb, KERN_ERR, "can't allocate buddy mem");
		goto exit_group_info;
	}
	set_bit(EXT4_GROUP_INFO_NEED_INIT_BIT,
		&(meta_group_info[i]->bb_state));

	/*
	 * initialize bb_free to be able to skip
	 * empty groups without initialization
	 */
	if (ext4_has_group_desc_csum(sb) &&
	    (desc->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT))) {
		meta_group_info[i]->bb_free =
			ext4_free_clusters_after_init(sb, group, desc);
	} else {
		meta_group_info[i]->bb_free =
			ext4_free_group_clusters(sb, desc);
	}

	INIT_LIST_HEAD(&meta_group_info[i]->bb_prealloc_list);
	init_rwsem(&meta_group_info[i]->alloc_sem);
	meta_group_info[i]->bb_free_root = RB_ROOT;
	INIT_LIST_HEAD(&meta_group_info[i]->bb_largest_free_order_node);
	INIT_LIST_HEAD(&meta_group_info[i]->bb_avg_fragment_size_node);
	meta_group_info[i]->bb_largest_free_order = -1;  /* uninit */
	meta_group_info[i]->bb_avg_fragment_size_order = -1;  /* uninit */
	meta_group_info[i]->bb_group = group;

	mb_group_bb_bitmap_alloc(sb, meta_group_info[i], group);
	return 0;

exit_group_info:
	/* If a meta_group_info table has been allocated, release it now */
	if (group % EXT4_DESC_PER_BLOCK(sb) == 0) {
		struct ext4_group_info ***group_info;

		rcu_read_lock();
		group_info = rcu_dereference(sbi->s_group_info);
		kfree(group_info[idx]);
		group_info[idx] = NULL;
		rcu_read_unlock();
	}
	return -ENOMEM;
} /* ext4_mb_add_groupinfo */

static int ext4_mb_init_backend(struct super_block *sb)
{
	ext4_group_t ngroups = ext4_get_groups_count(sb);
	ext4_group_t i;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	int err;
	struct ext4_group_desc *desc;
	struct ext4_group_info ***group_info;
	struct kmem_cache *cachep;

	err = ext4_mb_alloc_groupinfo(sb, ngroups);
	if (err)
		return err;

	sbi->s_buddy_cache = new_inode(sb);
	if (sbi->s_buddy_cache == NULL) {
		ext4_msg(sb, KERN_ERR, "can't get new inode");
		goto err_freesgi;
	}
	/* To avoid potentially colliding with an valid on-disk inode number,
	 * use EXT4_BAD_INO for the buddy cache inode number.  This inode is
	 * not in the inode hash, so it should never be found by iget(), but
	 * this will avoid confusion if it ever shows up during debugging. */
	sbi->s_buddy_cache->i_ino = EXT4_BAD_INO;
	EXT4_I(sbi->s_buddy_cache)->i_disksize = 0;
	for (i = 0; i < ngroups; i++) {
		cond_resched();
		desc = ext4_get_group_desc(sb, i, NULL);
		if (desc == NULL) {
			ext4_msg(sb, KERN_ERR, "can't read descriptor %u", i);
			goto err_freebuddy;
		}
		if (ext4_mb_add_groupinfo(sb, i, desc) != 0)
			goto err_freebuddy;
	}

	if (ext4_has_feature_flex_bg(sb)) {
		/* a single flex group is supposed to be read by a single IO.
		 * 2 ^ s_log_groups_per_flex != UINT_MAX as s_mb_prefetch is
		 * unsigned integer, so the maximum shift is 32.
		 */
		if (sbi->s_es->s_log_groups_per_flex >= 32) {
			ext4_msg(sb, KERN_ERR, "too many log groups per flexible block group");
			goto err_freebuddy;
		}
		sbi->s_mb_prefetch = min_t(uint, 1 << sbi->s_es->s_log_groups_per_flex,
			BLK_MAX_SEGMENT_SIZE >> (sb->s_blocksize_bits - 9));
		sbi->s_mb_prefetch *= 8; /* 8 prefetch IOs in flight at most */
	} else {
		sbi->s_mb_prefetch = 32;
	}
	if (sbi->s_mb_prefetch > ext4_get_groups_count(sb))
		sbi->s_mb_prefetch = ext4_get_groups_count(sb);
	/* now many real IOs to prefetch within a single allocation at cr=0
	 * given cr=0 is an CPU-related optimization we shouldn't try to
	 * load too many groups, at some point we should start to use what
	 * we've got in memory.
	 * with an average random access time 5ms, it'd take a second to get
	 * 200 groups (* N with flex_bg), so let's make this limit 4
	 */
	sbi->s_mb_prefetch_limit = sbi->s_mb_prefetch * 4;
	if (sbi->s_mb_prefetch_limit > ext4_get_groups_count(sb))
		sbi->s_mb_prefetch_limit = ext4_get_groups_count(sb);

	return 0;

err_freebuddy:
	cachep = get_groupinfo_cache(sb->s_blocksize_bits);
	while (i-- > 0) {
		struct ext4_group_info *grp = ext4_get_group_info(sb, i);

		if (grp)
			kmem_cache_free(cachep, grp);
	}
	i = sbi->s_group_info_size;
	rcu_read_lock();
	group_info = rcu_dereference(sbi->s_group_info);
	while (i-- > 0)
		kfree(group_info[i]);
	rcu_read_unlock();
	iput(sbi->s_buddy_cache);
err_freesgi:
	rcu_read_lock();
	kvfree(rcu_dereference(sbi->s_group_info));
	rcu_read_unlock();
	return -ENOMEM;
}

static void ext4_groupinfo_destroy_slabs(void)
{
	int i;

	for (i = 0; i < NR_GRPINFO_CACHES; i++) {
		kmem_cache_destroy(ext4_groupinfo_caches[i]);
		ext4_groupinfo_caches[i] = NULL;
	}
}

static int ext4_groupinfo_create_slab(size_t size)
{
	static DEFINE_MUTEX(ext4_grpinfo_slab_create_mutex);
	int slab_size;
	int blocksize_bits = order_base_2(size);
	int cache_index = blocksize_bits - EXT4_MIN_BLOCK_LOG_SIZE;
	struct kmem_cache *cachep;

	if (cache_index >= NR_GRPINFO_CACHES)
		return -EINVAL;

	if (unlikely(cache_index < 0))
		cache_index = 0;

	mutex_lock(&ext4_grpinfo_slab_create_mutex);
	if (ext4_groupinfo_caches[cache_index]) {
		mutex_unlock(&ext4_grpinfo_slab_create_mutex);
		return 0;	/* Already created */
	}

	slab_size = offsetof(struct ext4_group_info,
				bb_counters[blocksize_bits + 2]);

	cachep = kmem_cache_create(ext4_groupinfo_slab_names[cache_index],
					slab_size, 0, SLAB_RECLAIM_ACCOUNT,
					NULL);

	ext4_groupinfo_caches[cache_index] = cachep;

	mutex_unlock(&ext4_grpinfo_slab_create_mutex);
	if (!cachep) {
		printk(KERN_EMERG
		       "EXT4-fs: no memory for groupinfo slab cache\n");
		return -ENOMEM;
	}

	return 0;
}

static void ext4_discard_work(struct work_struct *work)
{
	struct ext4_sb_info *sbi = container_of(work,
			struct ext4_sb_info, s_discard_work);
	struct super_block *sb = sbi->s_sb;
	struct ext4_free_data *fd, *nfd;
	struct ext4_buddy e4b;
	LIST_HEAD(discard_list);
	ext4_group_t grp, load_grp;
	int err = 0;

	spin_lock(&sbi->s_md_lock);
	list_splice_init(&sbi->s_discard_list, &discard_list);
	spin_unlock(&sbi->s_md_lock);

	load_grp = UINT_MAX;
	list_for_each_entry_safe(fd, nfd, &discard_list, efd_list) {
		/*
		 * If filesystem is umounting or no memory or suffering
		 * from no space, give up the discard
		 */
		if ((sb->s_flags & SB_ACTIVE) && !err &&
		    !atomic_read(&sbi->s_retry_alloc_pending)) {
			grp = fd->efd_group;
			if (grp != load_grp) {
				if (load_grp != UINT_MAX)
					ext4_mb_unload_buddy(&e4b);

				err = ext4_mb_load_buddy(sb, grp, &e4b);
				if (err) {
					kmem_cache_free(ext4_free_data_cachep, fd);
					load_grp = UINT_MAX;
					continue;
				} else {
					load_grp = grp;
				}
			}

			ext4_lock_group(sb, grp);
			ext4_try_to_trim_range(sb, &e4b, fd->efd_start_cluster,
						fd->efd_start_cluster + fd->efd_count - 1, 1);
			ext4_unlock_group(sb, grp);
		}
		kmem_cache_free(ext4_free_data_cachep, fd);
	}

	if (load_grp != UINT_MAX)
		ext4_mb_unload_buddy(&e4b);
}

int ext4_mb_init(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	unsigned i, j;
	unsigned offset, offset_incr;
	unsigned max;
	int ret;

	i = MB_NUM_ORDERS(sb) * sizeof(*sbi->s_mb_offsets);

	sbi->s_mb_offsets = kmalloc(i, GFP_KERNEL);
	if (sbi->s_mb_offsets == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	i = MB_NUM_ORDERS(sb) * sizeof(*sbi->s_mb_maxs);
	sbi->s_mb_maxs = kmalloc(i, GFP_KERNEL);
	if (sbi->s_mb_maxs == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	ret = ext4_groupinfo_create_slab(sb->s_blocksize);
	if (ret < 0)
		goto out;

	/* order 0 is regular bitmap */
	sbi->s_mb_maxs[0] = sb->s_blocksize << 3;
	sbi->s_mb_offsets[0] = 0;

	i = 1;
	offset = 0;
	offset_incr = 1 << (sb->s_blocksize_bits - 1);
	max = sb->s_blocksize << 2;
	do {
		sbi->s_mb_offsets[i] = offset;
		sbi->s_mb_maxs[i] = max;
		offset += offset_incr;
		offset_incr = offset_incr >> 1;
		max = max >> 1;
		i++;
	} while (i < MB_NUM_ORDERS(sb));

	sbi->s_mb_avg_fragment_size =
		kmalloc_array(MB_NUM_ORDERS(sb), sizeof(struct list_head),
			GFP_KERNEL);
	if (!sbi->s_mb_avg_fragment_size) {
		ret = -ENOMEM;
		goto out;
	}
	sbi->s_mb_avg_fragment_size_locks =
		kmalloc_array(MB_NUM_ORDERS(sb), sizeof(rwlock_t),
			GFP_KERNEL);
	if (!sbi->s_mb_avg_fragment_size_locks) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < MB_NUM_ORDERS(sb); i++) {
		INIT_LIST_HEAD(&sbi->s_mb_avg_fragment_size[i]);
		rwlock_init(&sbi->s_mb_avg_fragment_size_locks[i]);
	}
	sbi->s_mb_largest_free_orders =
		kmalloc_array(MB_NUM_ORDERS(sb), sizeof(struct list_head),
			GFP_KERNEL);
	if (!sbi->s_mb_largest_free_orders) {
		ret = -ENOMEM;
		goto out;
	}
	sbi->s_mb_largest_free_orders_locks =
		kmalloc_array(MB_NUM_ORDERS(sb), sizeof(rwlock_t),
			GFP_KERNEL);
	if (!sbi->s_mb_largest_free_orders_locks) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < MB_NUM_ORDERS(sb); i++) {
		INIT_LIST_HEAD(&sbi->s_mb_largest_free_orders[i]);
		rwlock_init(&sbi->s_mb_largest_free_orders_locks[i]);
	}

	spin_lock_init(&sbi->s_md_lock);
	sbi->s_mb_free_pending = 0;
	INIT_LIST_HEAD(&sbi->s_freed_data_list);
	INIT_LIST_HEAD(&sbi->s_discard_list);
	INIT_WORK(&sbi->s_discard_work, ext4_discard_work);
	atomic_set(&sbi->s_retry_alloc_pending, 0);

	sbi->s_mb_max_to_scan = MB_DEFAULT_MAX_TO_SCAN;
	sbi->s_mb_min_to_scan = MB_DEFAULT_MIN_TO_SCAN;
	sbi->s_mb_stats = MB_DEFAULT_STATS;
	sbi->s_mb_stream_request = MB_DEFAULT_STREAM_THRESHOLD;
	sbi->s_mb_order2_reqs = MB_DEFAULT_ORDER2_REQS;
	sbi->s_mb_best_avail_max_trim_order = MB_DEFAULT_BEST_AVAIL_TRIM_ORDER;

	/*
	 * The default group preallocation is 512, which for 4k block
	 * sizes translates to 2 megabytes.  However for bigalloc file
	 * systems, this is probably too big (i.e, if the cluster size
	 * is 1 megabyte, then group preallocation size becomes half a
	 * gigabyte!).  As a default, we will keep a two megabyte
	 * group pralloc size for cluster sizes up to 64k, and after
	 * that, we will force a minimum group preallocation size of
	 * 32 clusters.  This translates to 8 megs when the cluster
	 * size is 256k, and 32 megs when the cluster size is 1 meg,
	 * which seems reasonable as a default.
	 */
	sbi->s_mb_group_prealloc = max(MB_DEFAULT_GROUP_PREALLOC >>
				       sbi->s_cluster_bits, 32);
	/*
	 * If there is a s_stripe > 1, then we set the s_mb_group_prealloc
	 * to the lowest multiple of s_stripe which is bigger than
	 * the s_mb_group_prealloc as determined above. We want
	 * the preallocation size to be an exact multiple of the
	 * RAID stripe size so that preallocations don't fragment
	 * the stripes.
	 */
	if (sbi->s_stripe > 1) {
		sbi->s_mb_group_prealloc = roundup(
			sbi->s_mb_group_prealloc, EXT4_B2C(sbi, sbi->s_stripe));
	}

	sbi->s_locality_groups = alloc_percpu(struct ext4_locality_group);
	if (sbi->s_locality_groups == NULL) {
		ret = -ENOMEM;
		goto out;
	}
	for_each_possible_cpu(i) {
		struct ext4_locality_group *lg;
		lg = per_cpu_ptr(sbi->s_locality_groups, i);
		mutex_init(&lg->lg_mutex);
		for (j = 0; j < PREALLOC_TB_SIZE; j++)
			INIT_LIST_HEAD(&lg->lg_prealloc_list[j]);
		spin_lock_init(&lg->lg_prealloc_lock);
	}

	if (bdev_nonrot(sb->s_bdev))
		sbi->s_mb_max_linear_groups = 0;
	else
		sbi->s_mb_max_linear_groups = MB_DEFAULT_LINEAR_LIMIT;
	/* init file for buddy data */
	ret = ext4_mb_init_backend(sb);
	if (ret != 0)
		goto out_free_locality_groups;

	return 0;

out_free_locality_groups:
	free_percpu(sbi->s_locality_groups);
	sbi->s_locality_groups = NULL;
out:
	kfree(sbi->s_mb_avg_fragment_size);
	kfree(sbi->s_mb_avg_fragment_size_locks);
	kfree(sbi->s_mb_largest_free_orders);
	kfree(sbi->s_mb_largest_free_orders_locks);
	kfree(sbi->s_mb_offsets);
	sbi->s_mb_offsets = NULL;
	kfree(sbi->s_mb_maxs);
	sbi->s_mb_maxs = NULL;
	return ret;
}

/* need to called with the ext4 group lock held */
static int ext4_mb_cleanup_pa(struct ext4_group_info *grp)
{
	struct ext4_prealloc_space *pa;
	struct list_head *cur, *tmp;
	int count = 0;

	list_for_each_safe(cur, tmp, &grp->bb_prealloc_list) {
		pa = list_entry(cur, struct ext4_prealloc_space, pa_group_list);
		list_del(&pa->pa_group_list);
		count++;
		kmem_cache_free(ext4_pspace_cachep, pa);
	}
	return count;
}

int ext4_mb_release(struct super_block *sb)
{
	ext4_group_t ngroups = ext4_get_groups_count(sb);
	ext4_group_t i;
	int num_meta_group_infos;
	struct ext4_group_info *grinfo, ***group_info;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct kmem_cache *cachep = get_groupinfo_cache(sb->s_blocksize_bits);
	int count;

	if (test_opt(sb, DISCARD)) {
		/*
		 * wait the discard work to drain all of ext4_free_data
		 */
		flush_work(&sbi->s_discard_work);
		WARN_ON_ONCE(!list_empty(&sbi->s_discard_list));
	}

	if (sbi->s_group_info) {
		for (i = 0; i < ngroups; i++) {
			cond_resched();
			grinfo = ext4_get_group_info(sb, i);
			if (!grinfo)
				continue;
			mb_group_bb_bitmap_free(grinfo);
			ext4_lock_group(sb, i);
			count = ext4_mb_cleanup_pa(grinfo);
			if (count)
				mb_debug(sb, "mballoc: %d PAs left\n",
					 count);
			ext4_unlock_group(sb, i);
			kmem_cache_free(cachep, grinfo);
		}
		num_meta_group_infos = (ngroups +
				EXT4_DESC_PER_BLOCK(sb) - 1) >>
			EXT4_DESC_PER_BLOCK_BITS(sb);
		rcu_read_lock();
		group_info = rcu_dereference(sbi->s_group_info);
		for (i = 0; i < num_meta_group_infos; i++)
			kfree(group_info[i]);
		kvfree(group_info);
		rcu_read_unlock();
	}
	kfree(sbi->s_mb_avg_fragment_size);
	kfree(sbi->s_mb_avg_fragment_size_locks);
	kfree(sbi->s_mb_largest_free_orders);
	kfree(sbi->s_mb_largest_free_orders_locks);
	kfree(sbi->s_mb_offsets);
	kfree(sbi->s_mb_maxs);
	iput(sbi->s_buddy_cache);
	if (sbi->s_mb_stats) {
		ext4_msg(sb, KERN_INFO,
		       "mballoc: %u blocks %u reqs (%u success)",
				atomic_read(&sbi->s_bal_allocated),
				atomic_read(&sbi->s_bal_reqs),
				atomic_read(&sbi->s_bal_success));
		ext4_msg(sb, KERN_INFO,
		      "mballoc: %u extents scanned, %u groups scanned, %u goal hits, "
				"%u 2^N hits, %u breaks, %u lost",
				atomic_read(&sbi->s_bal_ex_scanned),
				atomic_read(&sbi->s_bal_groups_scanned),
				atomic_read(&sbi->s_bal_goals),
				atomic_read(&sbi->s_bal_2orders),
				atomic_read(&sbi->s_bal_breaks),
				atomic_read(&sbi->s_mb_lost_chunks));
		ext4_msg(sb, KERN_INFO,
		       "mballoc: %u generated and it took %llu",
				atomic_read(&sbi->s_mb_buddies_generated),
				atomic64_read(&sbi->s_mb_generation_time));
		ext4_msg(sb, KERN_INFO,
		       "mballoc: %u preallocated, %u discarded",
				atomic_read(&sbi->s_mb_preallocated),
				atomic_read(&sbi->s_mb_discarded));
	}

	free_percpu(sbi->s_locality_groups);

	return 0;
}

static inline int ext4_issue_discard(struct super_block *sb,
		ext4_group_t block_group, ext4_grpblk_t cluster, int count,
		struct bio **biop)
{
	ext4_fsblk_t discard_block;

	discard_block = (EXT4_C2B(EXT4_SB(sb), cluster) +
			 ext4_group_first_block_no(sb, block_group));
	count = EXT4_C2B(EXT4_SB(sb), count);
	trace_ext4_discard_blocks(sb,
			(unsigned long long) discard_block, count);
	if (biop) {
		return __blkdev_issue_discard(sb->s_bdev,
			(sector_t)discard_block << (sb->s_blocksize_bits - 9),
			(sector_t)count << (sb->s_blocksize_bits - 9),
			GFP_NOFS, biop);
	} else
		return sb_issue_discard(sb, discard_block, count, GFP_NOFS, 0);
}

static void ext4_free_data_in_buddy(struct super_block *sb,
				    struct ext4_free_data *entry)
{
	struct ext4_buddy e4b;
	struct ext4_group_info *db;
	int err, count = 0;

	mb_debug(sb, "gonna free %u blocks in group %u (0x%p):",
		 entry->efd_count, entry->efd_group, entry);

	err = ext4_mb_load_buddy(sb, entry->efd_group, &e4b);
	/* we expect to find existing buddy because it's pinned */
	BUG_ON(err != 0);

	spin_lock(&EXT4_SB(sb)->s_md_lock);
	EXT4_SB(sb)->s_mb_free_pending -= entry->efd_count;
	spin_unlock(&EXT4_SB(sb)->s_md_lock);

	db = e4b.bd_info;
	/* there are blocks to put in buddy to make them really free */
	count += entry->efd_count;
	ext4_lock_group(sb, entry->efd_group);
	/* Take it out of per group rb tree */
	rb_erase(&entry->efd_node, &(db->bb_free_root));
	mb_free_blocks(NULL, &e4b, entry->efd_start_cluster, entry->efd_count);

	/*
	 * Clear the trimmed flag for the group so that the next
	 * ext4_trim_fs can trim it.
	 * If the volume is mounted with -o discard, online discard
	 * is supported and the free blocks will be trimmed online.
	 */
	if (!test_opt(sb, DISCARD))
		EXT4_MB_GRP_CLEAR_TRIMMED(db);

	if (!db->bb_free_root.rb_node) {
		/* No more items in the per group rb tree
		 * balance refcounts from ext4_mb_free_metadata()
		 */
		put_page(e4b.bd_buddy_page);
		put_page(e4b.bd_bitmap_page);
	}
	ext4_unlock_group(sb, entry->efd_group);
	ext4_mb_unload_buddy(&e4b);

	mb_debug(sb, "freed %d blocks in 1 structures\n", count);
}

/*
 * This function is called by the jbd2 layer once the commit has finished,
 * so we know we can free the blocks that were released with that commit.
 */
void ext4_process_freed_data(struct super_block *sb, tid_t commit_tid)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_free_data *entry, *tmp;
	LIST_HEAD(freed_data_list);
	struct list_head *cut_pos = NULL;
	bool wake;

	spin_lock(&sbi->s_md_lock);
	list_for_each_entry(entry, &sbi->s_freed_data_list, efd_list) {
		if (entry->efd_tid != commit_tid)
			break;
		cut_pos = &entry->efd_list;
	}
	if (cut_pos)
		list_cut_position(&freed_data_list, &sbi->s_freed_data_list,
				  cut_pos);
	spin_unlock(&sbi->s_md_lock);

	list_for_each_entry(entry, &freed_data_list, efd_list)
		ext4_free_data_in_buddy(sb, entry);

	if (test_opt(sb, DISCARD)) {
		spin_lock(&sbi->s_md_lock);
		wake = list_empty(&sbi->s_discard_list);
		list_splice_tail(&freed_data_list, &sbi->s_discard_list);
		spin_unlock(&sbi->s_md_lock);
		if (wake)
			queue_work(system_unbound_wq, &sbi->s_discard_work);
	} else {
		list_for_each_entry_safe(entry, tmp, &freed_data_list, efd_list)
			kmem_cache_free(ext4_free_data_cachep, entry);
	}
}

int __init ext4_init_mballoc(void)
{
	ext4_pspace_cachep = KMEM_CACHE(ext4_prealloc_space,
					SLAB_RECLAIM_ACCOUNT);
	if (ext4_pspace_cachep == NULL)
		goto out;

	ext4_ac_cachep = KMEM_CACHE(ext4_allocation_context,
				    SLAB_RECLAIM_ACCOUNT);
	if (ext4_ac_cachep == NULL)
		goto out_pa_free;

	ext4_free_data_cachep = KMEM_CACHE(ext4_free_data,
					   SLAB_RECLAIM_ACCOUNT);
	if (ext4_free_data_cachep == NULL)
		goto out_ac_free;

	return 0;

out_ac_free:
	kmem_cache_destroy(ext4_ac_cachep);
out_pa_free:
	kmem_cache_destroy(ext4_pspace_cachep);
out:
	return -ENOMEM;
}

void ext4_exit_mballoc(void)
{
	/*
	 * Wait for completion of call_rcu()'s on ext4_pspace_cachep
	 * before destroying the slab cache.
	 */
	rcu_barrier();
	kmem_cache_destroy(ext4_pspace_cachep);
	kmem_cache_destroy(ext4_ac_cachep);
	kmem_cache_destroy(ext4_free_data_cachep);
	ext4_groupinfo_destroy_slabs();
}


/*
 * Check quota and mark chosen space (ac->ac_b_ex) non-free in bitmaps
 * Returns 0 if success or error code
 */
static noinline_for_stack int
ext4_mb_mark_diskspace_used(struct ext4_allocation_context *ac,
				handle_t *handle, unsigned int reserv_clstrs)
{
	struct buffer_head *bitmap_bh = NULL;
	struct ext4_group_desc *gdp;
	struct buffer_head *gdp_bh;
	struct ext4_sb_info *sbi;
	struct super_block *sb;
	ext4_fsblk_t block;
	int err, len;

	BUG_ON(ac->ac_status != AC_STATUS_FOUND);
	BUG_ON(ac->ac_b_ex.fe_len <= 0);

	sb = ac->ac_sb;
	sbi = EXT4_SB(sb);

	bitmap_bh = ext4_read_block_bitmap(sb, ac->ac_b_ex.fe_group);
	if (IS_ERR(bitmap_bh)) {
		return PTR_ERR(bitmap_bh);
	}

	BUFFER_TRACE(bitmap_bh, "getting write access");
	err = ext4_journal_get_write_access(handle, sb, bitmap_bh,
					    EXT4_JTR_NONE);
	if (err)
		goto out_err;

	err = -EIO;
	gdp = ext4_get_group_desc(sb, ac->ac_b_ex.fe_group, &gdp_bh);
	if (!gdp)
		goto out_err;

	ext4_debug("using block group %u(%d)\n", ac->ac_b_ex.fe_group,
			ext4_free_group_clusters(sb, gdp));

	BUFFER_TRACE(gdp_bh, "get_write_access");
	err = ext4_journal_get_write_access(handle, sb, gdp_bh, EXT4_JTR_NONE);
	if (err)
		goto out_err;

	block = ext4_grp_offs_to_block(sb, &ac->ac_b_ex);

	len = EXT4_C2B(sbi, ac->ac_b_ex.fe_len);
	if (!ext4_inode_block_valid(ac->ac_inode, block, len)) {
		ext4_error(sb, "Allocating blocks %llu-%llu which overlap "
			   "fs metadata", block, block+len);
		/* File system mounted not to panic on error
		 * Fix the bitmap and return EFSCORRUPTED
		 * We leak some of the blocks here.
		 */
		ext4_lock_group(sb, ac->ac_b_ex.fe_group);
		mb_set_bits(bitmap_bh->b_data, ac->ac_b_ex.fe_start,
			      ac->ac_b_ex.fe_len);
		ext4_unlock_group(sb, ac->ac_b_ex.fe_group);
		err = ext4_handle_dirty_metadata(handle, NULL, bitmap_bh);
		if (!err)
			err = -EFSCORRUPTED;
		goto out_err;
	}

	ext4_lock_group(sb, ac->ac_b_ex.fe_group);
#ifdef AGGRESSIVE_CHECK
	{
		int i;
		for (i = 0; i < ac->ac_b_ex.fe_len; i++) {
			BUG_ON(mb_test_bit(ac->ac_b_ex.fe_start + i,
						bitmap_bh->b_data));
		}
	}
#endif
	mb_set_bits(bitmap_bh->b_data, ac->ac_b_ex.fe_start,
		      ac->ac_b_ex.fe_len);
	if (ext4_has_group_desc_csum(sb) &&
	    (gdp->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT))) {
		gdp->bg_flags &= cpu_to_le16(~EXT4_BG_BLOCK_UNINIT);
		ext4_free_group_clusters_set(sb, gdp,
					     ext4_free_clusters_after_init(sb,
						ac->ac_b_ex.fe_group, gdp));
	}
	len = ext4_free_group_clusters(sb, gdp) - ac->ac_b_ex.fe_len;
	ext4_free_group_clusters_set(sb, gdp, len);
	ext4_block_bitmap_csum_set(sb, gdp, bitmap_bh);
	ext4_group_desc_csum_set(sb, ac->ac_b_ex.fe_group, gdp);

	ext4_unlock_group(sb, ac->ac_b_ex.fe_group);
	percpu_counter_sub(&sbi->s_freeclusters_counter, ac->ac_b_ex.fe_len);
	/*
	 * Now reduce the dirty block count also. Should not go negative
	 */
	if (!(ac->ac_flags & EXT4_MB_DELALLOC_RESERVED))
		/* release all the reserved blocks if non delalloc */
		percpu_counter_sub(&sbi->s_dirtyclusters_counter,
				   reserv_clstrs);

	if (sbi->s_log_groups_per_flex) {
		ext4_group_t flex_group = ext4_flex_group(sbi,
							  ac->ac_b_ex.fe_group);
		atomic64_sub(ac->ac_b_ex.fe_len,
			     &sbi_array_rcu_deref(sbi, s_flex_groups,
						  flex_group)->free_clusters);
	}

	err = ext4_handle_dirty_metadata(handle, NULL, bitmap_bh);
	if (err)
		goto out_err;
	err = ext4_handle_dirty_metadata(handle, NULL, gdp_bh);

out_err:
	brelse(bitmap_bh);
	return err;
}

/*
 * Idempotent helper for Ext4 fast commit replay path to set the state of
 * blocks in bitmaps and update counters.
 */
void ext4_mb_mark_bb(struct super_block *sb, ext4_fsblk_t block,
			int len, int state)
{
	struct buffer_head *bitmap_bh = NULL;
	struct ext4_group_desc *gdp;
	struct buffer_head *gdp_bh;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_group_t group;
	ext4_grpblk_t blkoff;
	int i, err = 0;
	int already;
	unsigned int clen, clen_changed, thisgrp_len;

	while (len > 0) {
		ext4_get_group_no_and_offset(sb, block, &group, &blkoff);

		/*
		 * Check to see if we are freeing blocks across a group
		 * boundary.
		 * In case of flex_bg, this can happen that (block, len) may
		 * span across more than one group. In that case we need to
		 * get the corresponding group metadata to work with.
		 * For this we have goto again loop.
		 */
		thisgrp_len = min_t(unsigned int, (unsigned int)len,
			EXT4_BLOCKS_PER_GROUP(sb) - EXT4_C2B(sbi, blkoff));
		clen = EXT4_NUM_B2C(sbi, thisgrp_len);

		if (!ext4_sb_block_valid(sb, NULL, block, thisgrp_len)) {
			ext4_error(sb, "Marking blocks in system zone - "
				   "Block = %llu, len = %u",
				   block, thisgrp_len);
			bitmap_bh = NULL;
			break;
		}

		bitmap_bh = ext4_read_block_bitmap(sb, group);
		if (IS_ERR(bitmap_bh)) {
			err = PTR_ERR(bitmap_bh);
			bitmap_bh = NULL;
			break;
		}

		err = -EIO;
		gdp = ext4_get_group_desc(sb, group, &gdp_bh);
		if (!gdp)
			break;

		ext4_lock_group(sb, group);
		already = 0;
		for (i = 0; i < clen; i++)
			if (!mb_test_bit(blkoff + i, bitmap_bh->b_data) ==
					 !state)
				already++;

		clen_changed = clen - already;
		if (state)
			mb_set_bits(bitmap_bh->b_data, blkoff, clen);
		else
			mb_clear_bits(bitmap_bh->b_data, blkoff, clen);
		if (ext4_has_group_desc_csum(sb) &&
		    (gdp->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT))) {
			gdp->bg_flags &= cpu_to_le16(~EXT4_BG_BLOCK_UNINIT);
			ext4_free_group_clusters_set(sb, gdp,
			     ext4_free_clusters_after_init(sb, group, gdp));
		}
		if (state)
			clen = ext4_free_group_clusters(sb, gdp) - clen_changed;
		else
			clen = ext4_free_group_clusters(sb, gdp) + clen_changed;

		ext4_free_group_clusters_set(sb, gdp, clen);
		ext4_block_bitmap_csum_set(sb, gdp, bitmap_bh);
		ext4_group_desc_csum_set(sb, group, gdp);

		ext4_unlock_group(sb, group);

		if (sbi->s_log_groups_per_flex) {
			ext4_group_t flex_group = ext4_flex_group(sbi, group);
			struct flex_groups *fg = sbi_array_rcu_deref(sbi,
						   s_flex_groups, flex_group);

			if (state)
				atomic64_sub(clen_changed, &fg->free_clusters);
			else
				atomic64_add(clen_changed, &fg->free_clusters);

		}

		err = ext4_handle_dirty_metadata(NULL, NULL, bitmap_bh);
		if (err)
			break;
		sync_dirty_buffer(bitmap_bh);
		err = ext4_handle_dirty_metadata(NULL, NULL, gdp_bh);
		sync_dirty_buffer(gdp_bh);
		if (err)
			break;

		block += thisgrp_len;
		len -= thisgrp_len;
		brelse(bitmap_bh);
		BUG_ON(len < 0);
	}

	if (err)
		brelse(bitmap_bh);
}

/*
 * here we normalize request for locality group
 * Group request are normalized to s_mb_group_prealloc, which goes to
 * s_strip if we set the same via mount option.
 * s_mb_group_prealloc can be configured via
 * /sys/fs/ext4/<partition>/mb_group_prealloc
 *
 * XXX: should we try to preallocate more than the group has now?
 */
static void ext4_mb_normalize_group_request(struct ext4_allocation_context *ac)
{
	struct super_block *sb = ac->ac_sb;
	struct ext4_locality_group *lg = ac->ac_lg;

	BUG_ON(lg == NULL);
	ac->ac_g_ex.fe_len = EXT4_SB(sb)->s_mb_group_prealloc;
	mb_debug(sb, "goal %u blocks for locality group\n", ac->ac_g_ex.fe_len);
}

/*
 * This function returns the next element to look at during inode
 * PA rbtree walk. We assume that we have held the inode PA rbtree lock
 * (ei->i_prealloc_lock)
 *
 * new_start	The start of the range we want to compare
 * cur_start	The existing start that we are comparing against
 * node	The node of the rb_tree
 */
static inline struct rb_node*
ext4_mb_pa_rb_next_iter(ext4_lblk_t new_start, ext4_lblk_t cur_start, struct rb_node *node)
{
	if (new_start < cur_start)
		return node->rb_left;
	else
		return node->rb_right;
}

static inline void
ext4_mb_pa_assert_overlap(struct ext4_allocation_context *ac,
			  ext4_lblk_t start, loff_t end)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct ext4_inode_info *ei = EXT4_I(ac->ac_inode);
	struct ext4_prealloc_space *tmp_pa;
	ext4_lblk_t tmp_pa_start;
	loff_t tmp_pa_end;
	struct rb_node *iter;

	read_lock(&ei->i_prealloc_lock);
	for (iter = ei->i_prealloc_node.rb_node; iter;
	     iter = ext4_mb_pa_rb_next_iter(start, tmp_pa_start, iter)) {
		tmp_pa = rb_entry(iter, struct ext4_prealloc_space,
				  pa_node.inode_node);
		tmp_pa_start = tmp_pa->pa_lstart;
		tmp_pa_end = pa_logical_end(sbi, tmp_pa);

		spin_lock(&tmp_pa->pa_lock);
		if (tmp_pa->pa_deleted == 0)
			BUG_ON(!(start >= tmp_pa_end || end <= tmp_pa_start));
		spin_unlock(&tmp_pa->pa_lock);
	}
	read_unlock(&ei->i_prealloc_lock);
}

/*
 * Given an allocation context "ac" and a range "start", "end", check
 * and adjust boundaries if the range overlaps with any of the existing
 * preallocatoins stored in the corresponding inode of the allocation context.
 *
 * Parameters:
 *	ac			allocation context
 *	start			start of the new range
 *	end			end of the new range
 */
static inline void
ext4_mb_pa_adjust_overlap(struct ext4_allocation_context *ac,
			  ext4_lblk_t *start, loff_t *end)
{
	struct ext4_inode_info *ei = EXT4_I(ac->ac_inode);
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct ext4_prealloc_space *tmp_pa = NULL, *left_pa = NULL, *right_pa = NULL;
	struct rb_node *iter;
	ext4_lblk_t new_start, tmp_pa_start, right_pa_start = -1;
	loff_t new_end, tmp_pa_end, left_pa_end = -1;

	new_start = *start;
	new_end = *end;

	/*
	 * Adjust the normalized range so that it doesn't overlap with any
	 * existing preallocated blocks(PAs). Make sure to hold the rbtree lock
	 * so it doesn't change underneath us.
	 */
	read_lock(&ei->i_prealloc_lock);

	/* Step 1: find any one immediate neighboring PA of the normalized range */
	for (iter = ei->i_prealloc_node.rb_node; iter;
	     iter = ext4_mb_pa_rb_next_iter(ac->ac_o_ex.fe_logical,
					    tmp_pa_start, iter)) {
		tmp_pa = rb_entry(iter, struct ext4_prealloc_space,
				  pa_node.inode_node);
		tmp_pa_start = tmp_pa->pa_lstart;
		tmp_pa_end = pa_logical_end(sbi, tmp_pa);

		/* PA must not overlap original request */
		spin_lock(&tmp_pa->pa_lock);
		if (tmp_pa->pa_deleted == 0)
			BUG_ON(!(ac->ac_o_ex.fe_logical >= tmp_pa_end ||
				 ac->ac_o_ex.fe_logical < tmp_pa_start));
		spin_unlock(&tmp_pa->pa_lock);
	}

	/*
	 * Step 2: check if the found PA is left or right neighbor and
	 * get the other neighbor
	 */
	if (tmp_pa) {
		if (tmp_pa->pa_lstart < ac->ac_o_ex.fe_logical) {
			struct rb_node *tmp;

			left_pa = tmp_pa;
			tmp = rb_next(&left_pa->pa_node.inode_node);
			if (tmp) {
				right_pa = rb_entry(tmp,
						    struct ext4_prealloc_space,
						    pa_node.inode_node);
			}
		} else {
			struct rb_node *tmp;

			right_pa = tmp_pa;
			tmp = rb_prev(&right_pa->pa_node.inode_node);
			if (tmp) {
				left_pa = rb_entry(tmp,
						   struct ext4_prealloc_space,
						   pa_node.inode_node);
			}
		}
	}

	/* Step 3: get the non deleted neighbors */
	if (left_pa) {
		for (iter = &left_pa->pa_node.inode_node;;
		     iter = rb_prev(iter)) {
			if (!iter) {
				left_pa = NULL;
				break;
			}

			tmp_pa = rb_entry(iter, struct ext4_prealloc_space,
					  pa_node.inode_node);
			left_pa = tmp_pa;
			spin_lock(&tmp_pa->pa_lock);
			if (tmp_pa->pa_deleted == 0) {
				spin_unlock(&tmp_pa->pa_lock);
				break;
			}
			spin_unlock(&tmp_pa->pa_lock);
		}
	}

	if (right_pa) {
		for (iter = &right_pa->pa_node.inode_node;;
		     iter = rb_next(iter)) {
			if (!iter) {
				right_pa = NULL;
				break;
			}

			tmp_pa = rb_entry(iter, struct ext4_prealloc_space,
					  pa_node.inode_node);
			right_pa = tmp_pa;
			spin_lock(&tmp_pa->pa_lock);
			if (tmp_pa->pa_deleted == 0) {
				spin_unlock(&tmp_pa->pa_lock);
				break;
			}
			spin_unlock(&tmp_pa->pa_lock);
		}
	}

	if (left_pa) {
		left_pa_end = pa_logical_end(sbi, left_pa);
		BUG_ON(left_pa_end > ac->ac_o_ex.fe_logical);
	}

	if (right_pa) {
		right_pa_start = right_pa->pa_lstart;
		BUG_ON(right_pa_start <= ac->ac_o_ex.fe_logical);
	}

	/* Step 4: trim our normalized range to not overlap with the neighbors */
	if (left_pa) {
		if (left_pa_end > new_start)
			new_start = left_pa_end;
	}

	if (right_pa) {
		if (right_pa_start < new_end)
			new_end = right_pa_start;
	}
	read_unlock(&ei->i_prealloc_lock);

	/* XXX: extra loop to check we really don't overlap preallocations */
	ext4_mb_pa_assert_overlap(ac, new_start, new_end);

	*start = new_start;
	*end = new_end;
}

/*
 * Normalization means making request better in terms of
 * size and alignment
 */
static noinline_for_stack void
ext4_mb_normalize_request(struct ext4_allocation_context *ac,
				struct ext4_allocation_request *ar)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct ext4_super_block *es = sbi->s_es;
	int bsbits, max;
	loff_t size, start_off, end;
	loff_t orig_size __maybe_unused;
	ext4_lblk_t start;

	/* do normalize only data requests, metadata requests
	   do not need preallocation */
	if (!(ac->ac_flags & EXT4_MB_HINT_DATA))
		return;

	/* sometime caller may want exact blocks */
	if (unlikely(ac->ac_flags & EXT4_MB_HINT_GOAL_ONLY))
		return;

	/* caller may indicate that preallocation isn't
	 * required (it's a tail, for example) */
	if (ac->ac_flags & EXT4_MB_HINT_NOPREALLOC)
		return;

	if (ac->ac_flags & EXT4_MB_HINT_GROUP_ALLOC) {
		ext4_mb_normalize_group_request(ac);
		return ;
	}

	bsbits = ac->ac_sb->s_blocksize_bits;

	/* first, let's learn actual file size
	 * given current request is allocated */
	size = extent_logical_end(sbi, &ac->ac_o_ex);
	size = size << bsbits;
	if (size < i_size_read(ac->ac_inode))
		size = i_size_read(ac->ac_inode);
	orig_size = size;

	/* max size of free chunks */
	max = 2 << bsbits;

#define NRL_CHECK_SIZE(req, size, max, chunk_size)	\
		(req <= (size) || max <= (chunk_size))

	/* first, try to predict filesize */
	/* XXX: should this table be tunable? */
	start_off = 0;
	if (size <= 16 * 1024) {
		size = 16 * 1024;
	} else if (size <= 32 * 1024) {
		size = 32 * 1024;
	} else if (size <= 64 * 1024) {
		size = 64 * 1024;
	} else if (size <= 128 * 1024) {
		size = 128 * 1024;
	} else if (size <= 256 * 1024) {
		size = 256 * 1024;
	} else if (size <= 512 * 1024) {
		size = 512 * 1024;
	} else if (size <= 1024 * 1024) {
		size = 1024 * 1024;
	} else if (NRL_CHECK_SIZE(size, 4 * 1024 * 1024, max, 2 * 1024)) {
		start_off = ((loff_t)ac->ac_o_ex.fe_logical >>
						(21 - bsbits)) << 21;
		size = 2 * 1024 * 1024;
	} else if (NRL_CHECK_SIZE(size, 8 * 1024 * 1024, max, 4 * 1024)) {
		start_off = ((loff_t)ac->ac_o_ex.fe_logical >>
							(22 - bsbits)) << 22;
		size = 4 * 1024 * 1024;
	} else if (NRL_CHECK_SIZE(EXT4_C2B(sbi, ac->ac_o_ex.fe_len),
					(8<<20)>>bsbits, max, 8 * 1024)) {
		start_off = ((loff_t)ac->ac_o_ex.fe_logical >>
							(23 - bsbits)) << 23;
		size = 8 * 1024 * 1024;
	} else {
		start_off = (loff_t) ac->ac_o_ex.fe_logical << bsbits;
		size	  = (loff_t) EXT4_C2B(sbi,
					      ac->ac_o_ex.fe_len) << bsbits;
	}
	size = size >> bsbits;
	start = start_off >> bsbits;

	/*
	 * For tiny groups (smaller than 8MB) the chosen allocation
	 * alignment may be larger than group size. Make sure the
	 * alignment does not move allocation to a different group which
	 * makes mballoc fail assertions later.
	 */
	start = max(start, rounddown(ac->ac_o_ex.fe_logical,
			(ext4_lblk_t)EXT4_BLOCKS_PER_GROUP(ac->ac_sb)));

	/* don't cover already allocated blocks in selected range */
	if (ar->pleft && start <= ar->lleft) {
		size -= ar->lleft + 1 - start;
		start = ar->lleft + 1;
	}
	if (ar->pright && start + size - 1 >= ar->lright)
		size -= start + size - ar->lright;

	/*
	 * Trim allocation request for filesystems with artificially small
	 * groups.
	 */
	if (size > EXT4_BLOCKS_PER_GROUP(ac->ac_sb))
		size = EXT4_BLOCKS_PER_GROUP(ac->ac_sb);

	end = start + size;

	ext4_mb_pa_adjust_overlap(ac, &start, &end);

	size = end - start;

	/*
	 * In this function "start" and "size" are normalized for better
	 * alignment and length such that we could preallocate more blocks.
	 * This normalization is done such that original request of
	 * ac->ac_o_ex.fe_logical & fe_len should always lie within "start" and
	 * "size" boundaries.
	 * (Note fe_len can be relaxed since FS block allocation API does not
	 * provide gurantee on number of contiguous blocks allocation since that
	 * depends upon free space left, etc).
	 * In case of inode pa, later we use the allocated blocks
	 * [pa_pstart + fe_logical - pa_lstart, fe_len/size] from the preallocated
	 * range of goal/best blocks [start, size] to put it at the
	 * ac_o_ex.fe_logical extent of this inode.
	 * (See ext4_mb_use_inode_pa() for more details)
	 */
	if (start + size <= ac->ac_o_ex.fe_logical ||
			start > ac->ac_o_ex.fe_logical) {
		ext4_msg(ac->ac_sb, KERN_ERR,
			 "start %lu, size %lu, fe_logical %lu",
			 (unsigned long) start, (unsigned long) size,
			 (unsigned long) ac->ac_o_ex.fe_logical);
		BUG();
	}
	BUG_ON(size <= 0 || size > EXT4_BLOCKS_PER_GROUP(ac->ac_sb));

	/* now prepare goal request */

	/* XXX: is it better to align blocks WRT to logical
	 * placement or satisfy big request as is */
	ac->ac_g_ex.fe_logical = start;
	ac->ac_g_ex.fe_len = EXT4_NUM_B2C(sbi, size);
	ac->ac_orig_goal_len = ac->ac_g_ex.fe_len;

	/* define goal start in order to merge */
	if (ar->pright && (ar->lright == (start + size)) &&
	    ar->pright >= size &&
	    ar->pright - size >= le32_to_cpu(es->s_first_data_block)) {
		/* merge to the right */
		ext4_get_group_no_and_offset(ac->ac_sb, ar->pright - size,
						&ac->ac_g_ex.fe_group,
						&ac->ac_g_ex.fe_start);
		ac->ac_flags |= EXT4_MB_HINT_TRY_GOAL;
	}
	if (ar->pleft && (ar->lleft + 1 == start) &&
	    ar->pleft + 1 < ext4_blocks_count(es)) {
		/* merge to the left */
		ext4_get_group_no_and_offset(ac->ac_sb, ar->pleft + 1,
						&ac->ac_g_ex.fe_group,
						&ac->ac_g_ex.fe_start);
		ac->ac_flags |= EXT4_MB_HINT_TRY_GOAL;
	}

	mb_debug(ac->ac_sb, "goal: %lld(was %lld) blocks at %u\n", size,
		 orig_size, start);
}

static void ext4_mb_collect_stats(struct ext4_allocation_context *ac)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);

	if (sbi->s_mb_stats && ac->ac_g_ex.fe_len >= 1) {
		atomic_inc(&sbi->s_bal_reqs);
		atomic_add(ac->ac_b_ex.fe_len, &sbi->s_bal_allocated);
		if (ac->ac_b_ex.fe_len >= ac->ac_o_ex.fe_len)
			atomic_inc(&sbi->s_bal_success);

		atomic_add(ac->ac_found, &sbi->s_bal_ex_scanned);
		for (int i=0; i<EXT4_MB_NUM_CRS; i++) {
			atomic_add(ac->ac_cX_found[i], &sbi->s_bal_cX_ex_scanned[i]);
		}

		atomic_add(ac->ac_groups_scanned, &sbi->s_bal_groups_scanned);
		if (ac->ac_g_ex.fe_start == ac->ac_b_ex.fe_start &&
				ac->ac_g_ex.fe_group == ac->ac_b_ex.fe_group)
			atomic_inc(&sbi->s_bal_goals);
		/* did we allocate as much as normalizer originally wanted? */
		if (ac->ac_f_ex.fe_len == ac->ac_orig_goal_len)
			atomic_inc(&sbi->s_bal_len_goals);

		if (ac->ac_found > sbi->s_mb_max_to_scan)
			atomic_inc(&sbi->s_bal_breaks);
	}

	if (ac->ac_op == EXT4_MB_HISTORY_ALLOC)
		trace_ext4_mballoc_alloc(ac);
	else
		trace_ext4_mballoc_prealloc(ac);
}

/*
 * Called on failure; free up any blocks from the inode PA for this
 * context.  We don't need this for MB_GROUP_PA because we only change
 * pa_free in ext4_mb_release_context(), but on failure, we've already
 * zeroed out ac->ac_b_ex.fe_len, so group_pa->pa_free is not changed.
 */
static void ext4_discard_allocated_blocks(struct ext4_allocation_context *ac)
{
	struct ext4_prealloc_space *pa = ac->ac_pa;
	struct ext4_buddy e4b;
	int err;

	if (pa == NULL) {
		if (ac->ac_f_ex.fe_len == 0)
			return;
		err = ext4_mb_load_buddy(ac->ac_sb, ac->ac_f_ex.fe_group, &e4b);
		if (WARN_RATELIMIT(err,
				   "ext4: mb_load_buddy failed (%d)", err))
			/*
			 * This should never happen since we pin the
			 * pages in the ext4_allocation_context so
			 * ext4_mb_load_buddy() should never fail.
			 */
			return;
		ext4_lock_group(ac->ac_sb, ac->ac_f_ex.fe_group);
		mb_free_blocks(ac->ac_inode, &e4b, ac->ac_f_ex.fe_start,
			       ac->ac_f_ex.fe_len);
		ext4_unlock_group(ac->ac_sb, ac->ac_f_ex.fe_group);
		ext4_mb_unload_buddy(&e4b);
		return;
	}
	if (pa->pa_type == MB_INODE_PA) {
		spin_lock(&pa->pa_lock);
		pa->pa_free += ac->ac_b_ex.fe_len;
		spin_unlock(&pa->pa_lock);
	}
}

/*
 * use blocks preallocated to inode
 */
static void ext4_mb_use_inode_pa(struct ext4_allocation_context *ac,
				struct ext4_prealloc_space *pa)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	ext4_fsblk_t start;
	ext4_fsblk_t end;
	int len;

	/* found preallocated blocks, use them */
	start = pa->pa_pstart + (ac->ac_o_ex.fe_logical - pa->pa_lstart);
	end = min(pa->pa_pstart + EXT4_C2B(sbi, pa->pa_len),
		  start + EXT4_C2B(sbi, ac->ac_o_ex.fe_len));
	len = EXT4_NUM_B2C(sbi, end - start);
	ext4_get_group_no_and_offset(ac->ac_sb, start, &ac->ac_b_ex.fe_group,
					&ac->ac_b_ex.fe_start);
	ac->ac_b_ex.fe_len = len;
	ac->ac_status = AC_STATUS_FOUND;
	ac->ac_pa = pa;

	BUG_ON(start < pa->pa_pstart);
	BUG_ON(end > pa->pa_pstart + EXT4_C2B(sbi, pa->pa_len));
	BUG_ON(pa->pa_free < len);
	BUG_ON(ac->ac_b_ex.fe_len <= 0);
	pa->pa_free -= len;

	mb_debug(ac->ac_sb, "use %llu/%d from inode pa %p\n", start, len, pa);
}

/*
 * use blocks preallocated to locality group
 */
static void ext4_mb_use_group_pa(struct ext4_allocation_context *ac,
				struct ext4_prealloc_space *pa)
{
	unsigned int len = ac->ac_o_ex.fe_len;

	ext4_get_group_no_and_offset(ac->ac_sb, pa->pa_pstart,
					&ac->ac_b_ex.fe_group,
					&ac->ac_b_ex.fe_start);
	ac->ac_b_ex.fe_len = len;
	ac->ac_status = AC_STATUS_FOUND;
	ac->ac_pa = pa;

	/* we don't correct pa_pstart or pa_len here to avoid
	 * possible race when the group is being loaded concurrently
	 * instead we correct pa later, after blocks are marked
	 * in on-disk bitmap -- see ext4_mb_release_context()
	 * Other CPUs are prevented from allocating from this pa by lg_mutex
	 */
	mb_debug(ac->ac_sb, "use %u/%u from group pa %p\n",
		 pa->pa_lstart, len, pa);
}

/*
 * Return the prealloc space that have minimal distance
 * from the goal block. @cpa is the prealloc
 * space that is having currently known minimal distance
 * from the goal block.
 */
static struct ext4_prealloc_space *
ext4_mb_check_group_pa(ext4_fsblk_t goal_block,
			struct ext4_prealloc_space *pa,
			struct ext4_prealloc_space *cpa)
{
	ext4_fsblk_t cur_distance, new_distance;

	if (cpa == NULL) {
		atomic_inc(&pa->pa_count);
		return pa;
	}
	cur_distance = abs(goal_block - cpa->pa_pstart);
	new_distance = abs(goal_block - pa->pa_pstart);

	if (cur_distance <= new_distance)
		return cpa;

	/* drop the previous reference */
	atomic_dec(&cpa->pa_count);
	atomic_inc(&pa->pa_count);
	return pa;
}

/*
 * check if found pa meets EXT4_MB_HINT_GOAL_ONLY
 */
static bool
ext4_mb_pa_goal_check(struct ext4_allocation_context *ac,
		      struct ext4_prealloc_space *pa)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	ext4_fsblk_t start;

	if (likely(!(ac->ac_flags & EXT4_MB_HINT_GOAL_ONLY)))
		return true;

	/*
	 * If EXT4_MB_HINT_GOAL_ONLY is set, ac_g_ex will not be adjusted
	 * in ext4_mb_normalize_request and will keep same with ac_o_ex
	 * from ext4_mb_initialize_context. Choose ac_g_ex here to keep
	 * consistent with ext4_mb_find_by_goal.
	 */
	start = pa->pa_pstart +
		(ac->ac_g_ex.fe_logical - pa->pa_lstart);
	if (ext4_grp_offs_to_block(ac->ac_sb, &ac->ac_g_ex) != start)
		return false;

	if (ac->ac_g_ex.fe_len > pa->pa_len -
	    EXT4_B2C(sbi, ac->ac_g_ex.fe_logical - pa->pa_lstart))
		return false;

	return true;
}

/*
 * search goal blocks in preallocated space
 */
static noinline_for_stack bool
ext4_mb_use_preallocated(struct ext4_allocation_context *ac)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	int order, i;
	struct ext4_inode_info *ei = EXT4_I(ac->ac_inode);
	struct ext4_locality_group *lg;
	struct ext4_prealloc_space *tmp_pa = NULL, *cpa = NULL;
	struct rb_node *iter;
	ext4_fsblk_t goal_block;

	/* only data can be preallocated */
	if (!(ac->ac_flags & EXT4_MB_HINT_DATA))
		return false;

	/*
	 * first, try per-file preallocation by searching the inode pa rbtree.
	 *
	 * Here, we can't do a direct traversal of the tree because
	 * ext4_mb_discard_group_preallocation() can paralelly mark the pa
	 * deleted and that can cause direct traversal to skip some entries.
	 */
	read_lock(&ei->i_prealloc_lock);

	if (RB_EMPTY_ROOT(&ei->i_prealloc_node)) {
		goto try_group_pa;
	}

	/*
	 * Step 1: Find a pa with logical start immediately adjacent to the
	 * original logical start. This could be on the left or right.
	 *
	 * (tmp_pa->pa_lstart never changes so we can skip locking for it).
	 */
	for (iter = ei->i_prealloc_node.rb_node; iter;
	     iter = ext4_mb_pa_rb_next_iter(ac->ac_o_ex.fe_logical,
					    tmp_pa->pa_lstart, iter)) {
		tmp_pa = rb_entry(iter, struct ext4_prealloc_space,
				  pa_node.inode_node);
	}

	/*
	 * Step 2: The adjacent pa might be to the right of logical start, find
	 * the left adjacent pa. After this step we'd have a valid tmp_pa whose
	 * logical start is towards the left of original request's logical start
	 */
	if (tmp_pa->pa_lstart > ac->ac_o_ex.fe_logical) {
		struct rb_node *tmp;
		tmp = rb_prev(&tmp_pa->pa_node.inode_node);

		if (tmp) {
			tmp_pa = rb_entry(tmp, struct ext4_prealloc_space,
					    pa_node.inode_node);
		} else {
			/*
			 * If there is no adjacent pa to the left then finding
			 * an overlapping pa is not possible hence stop searching
			 * inode pa tree
			 */
			goto try_group_pa;
		}
	}

	BUG_ON(!(tmp_pa && tmp_pa->pa_lstart <= ac->ac_o_ex.fe_logical));

	/*
	 * Step 3: If the left adjacent pa is deleted, keep moving left to find
	 * the first non deleted adjacent pa. After this step we should have a
	 * valid tmp_pa which is guaranteed to be non deleted.
	 */
	for (iter = &tmp_pa->pa_node.inode_node;; iter = rb_prev(iter)) {
		if (!iter) {
			/*
			 * no non deleted left adjacent pa, so stop searching
			 * inode pa tree
			 */
			goto try_group_pa;
		}
		tmp_pa = rb_entry(iter, struct ext4_prealloc_space,
				  pa_node.inode_node);
		spin_lock(&tmp_pa->pa_lock);
		if (tmp_pa->pa_deleted == 0) {
			/*
			 * We will keep holding the pa_lock from
			 * this point on because we don't want group discard
			 * to delete this pa underneath us. Since group
			 * discard is anyways an ENOSPC operation it
			 * should be okay for it to wait a few more cycles.
			 */
			break;
		} else {
			spin_unlock(&tmp_pa->pa_lock);
		}
	}

	BUG_ON(!(tmp_pa && tmp_pa->pa_lstart <= ac->ac_o_ex.fe_logical));
	BUG_ON(tmp_pa->pa_deleted == 1);

	/*
	 * Step 4: We now have the non deleted left adjacent pa. Only this
	 * pa can possibly satisfy the request hence check if it overlaps
	 * original logical start and stop searching if it doesn't.
	 */
	if (ac->ac_o_ex.fe_logical >= pa_logical_end(sbi, tmp_pa)) {
		spin_unlock(&tmp_pa->pa_lock);
		goto try_group_pa;
	}

	/* non-extent files can't have physical blocks past 2^32 */
	if (!(ext4_test_inode_flag(ac->ac_inode, EXT4_INODE_EXTENTS)) &&
	    (tmp_pa->pa_pstart + EXT4_C2B(sbi, tmp_pa->pa_len) >
	     EXT4_MAX_BLOCK_FILE_PHYS)) {
		/*
		 * Since PAs don't overlap, we won't find any other PA to
		 * satisfy this.
		 */
		spin_unlock(&tmp_pa->pa_lock);
		goto try_group_pa;
	}

	if (tmp_pa->pa_free && likely(ext4_mb_pa_goal_check(ac, tmp_pa))) {
		atomic_inc(&tmp_pa->pa_count);
		ext4_mb_use_inode_pa(ac, tmp_pa);
		spin_unlock(&tmp_pa->pa_lock);
		read_unlock(&ei->i_prealloc_lock);
		return true;
	} else {
		/*
		 * We found a valid overlapping pa but couldn't use it because
		 * it had no free blocks. This should ideally never happen
		 * because:
		 *
		 * 1. When a new inode pa is added to rbtree it must have
		 *    pa_free > 0 since otherwise we won't actually need
		 *    preallocation.
		 *
		 * 2. An inode pa that is in the rbtree can only have it's
		 *    pa_free become zero when another thread calls:
		 *      ext4_mb_new_blocks
		 *       ext4_mb_use_preallocated
		 *        ext4_mb_use_inode_pa
		 *
		 * 3. Further, after the above calls make pa_free == 0, we will
		 *    immediately remove it from the rbtree in:
		 *      ext4_mb_new_blocks
		 *       ext4_mb_release_context
		 *        ext4_mb_put_pa
		 *
		 * 4. Since the pa_free becoming 0 and pa_free getting removed
		 * from tree both happen in ext4_mb_new_blocks, which is always
		 * called with i_data_sem held for data allocations, we can be
		 * sure that another process will never see a pa in rbtree with
		 * pa_free == 0.
		 */
		WARN_ON_ONCE(tmp_pa->pa_free == 0);
	}
	spin_unlock(&tmp_pa->pa_lock);
try_group_pa:
	read_unlock(&ei->i_prealloc_lock);

	/* can we use group allocation? */
	if (!(ac->ac_flags & EXT4_MB_HINT_GROUP_ALLOC))
		return false;

	/* inode may have no locality group for some reason */
	lg = ac->ac_lg;
	if (lg == NULL)
		return false;
	order  = fls(ac->ac_o_ex.fe_len) - 1;
	if (order > PREALLOC_TB_SIZE - 1)
		/* The max size of hash table is PREALLOC_TB_SIZE */
		order = PREALLOC_TB_SIZE - 1;

	goal_block = ext4_grp_offs_to_block(ac->ac_sb, &ac->ac_g_ex);
	/*
	 * search for the prealloc space that is having
	 * minimal distance from the goal block.
	 */
	for (i = order; i < PREALLOC_TB_SIZE; i++) {
		rcu_read_lock();
		list_for_each_entry_rcu(tmp_pa, &lg->lg_prealloc_list[i],
					pa_node.lg_list) {
			spin_lock(&tmp_pa->pa_lock);
			if (tmp_pa->pa_deleted == 0 &&
					tmp_pa->pa_free >= ac->ac_o_ex.fe_len) {

				cpa = ext4_mb_check_group_pa(goal_block,
								tmp_pa, cpa);
			}
			spin_unlock(&tmp_pa->pa_lock);
		}
		rcu_read_unlock();
	}
	if (cpa) {
		ext4_mb_use_group_pa(ac, cpa);
		return true;
	}
	return false;
}

/*
 * the function goes through all block freed in the group
 * but not yet committed and marks them used in in-core bitmap.
 * buddy must be generated from this bitmap
 * Need to be called with the ext4 group lock held
 */
static void ext4_mb_generate_from_freelist(struct super_block *sb, void *bitmap,
						ext4_group_t group)
{
	struct rb_node *n;
	struct ext4_group_info *grp;
	struct ext4_free_data *entry;

	grp = ext4_get_group_info(sb, group);
	if (!grp)
		return;
	n = rb_first(&(grp->bb_free_root));

	while (n) {
		entry = rb_entry(n, struct ext4_free_data, efd_node);
		mb_set_bits(bitmap, entry->efd_start_cluster, entry->efd_count);
		n = rb_next(n);
	}
}

/*
 * the function goes through all preallocation in this group and marks them
 * used in in-core bitmap. buddy must be generated from this bitmap
 * Need to be called with ext4 group lock held
 */
static noinline_for_stack
void ext4_mb_generate_from_pa(struct super_block *sb, void *bitmap,
					ext4_group_t group)
{
	struct ext4_group_info *grp = ext4_get_group_info(sb, group);
	struct ext4_prealloc_space *pa;
	struct list_head *cur;
	ext4_group_t groupnr;
	ext4_grpblk_t start;
	int preallocated = 0;
	int len;

	if (!grp)
		return;

	/* all form of preallocation discards first load group,
	 * so the only competing code is preallocation use.
	 * we don't need any locking here
	 * notice we do NOT ignore preallocations with pa_deleted
	 * otherwise we could leave used blocks available for
	 * allocation in buddy when concurrent ext4_mb_put_pa()
	 * is dropping preallocation
	 */
	list_for_each(cur, &grp->bb_prealloc_list) {
		pa = list_entry(cur, struct ext4_prealloc_space, pa_group_list);
		spin_lock(&pa->pa_lock);
		ext4_get_group_no_and_offset(sb, pa->pa_pstart,
					     &groupnr, &start);
		len = pa->pa_len;
		spin_unlock(&pa->pa_lock);
		if (unlikely(len == 0))
			continue;
		BUG_ON(groupnr != group);
		mb_set_bits(bitmap, start, len);
		preallocated += len;
	}
	mb_debug(sb, "preallocated %d for group %u\n", preallocated, group);
}

static void ext4_mb_mark_pa_deleted(struct super_block *sb,
				    struct ext4_prealloc_space *pa)
{
	struct ext4_inode_info *ei;

	if (pa->pa_deleted) {
		ext4_warning(sb, "deleted pa, type:%d, pblk:%llu, lblk:%u, len:%d\n",
			     pa->pa_type, pa->pa_pstart, pa->pa_lstart,
			     pa->pa_len);
		return;
	}

	pa->pa_deleted = 1;

	if (pa->pa_type == MB_INODE_PA) {
		ei = EXT4_I(pa->pa_inode);
		atomic_dec(&ei->i_prealloc_active);
	}
}

static inline void ext4_mb_pa_free(struct ext4_prealloc_space *pa)
{
	BUG_ON(!pa);
	BUG_ON(atomic_read(&pa->pa_count));
	BUG_ON(pa->pa_deleted == 0);
	kmem_cache_free(ext4_pspace_cachep, pa);
}

static void ext4_mb_pa_callback(struct rcu_head *head)
{
	struct ext4_prealloc_space *pa;

	pa = container_of(head, struct ext4_prealloc_space, u.pa_rcu);
	ext4_mb_pa_free(pa);
}

/*
 * drops a reference to preallocated space descriptor
 * if this was the last reference and the space is consumed
 */
static void ext4_mb_put_pa(struct ext4_allocation_context *ac,
			struct super_block *sb, struct ext4_prealloc_space *pa)
{
	ext4_group_t grp;
	ext4_fsblk_t grp_blk;
	struct ext4_inode_info *ei = EXT4_I(ac->ac_inode);

	/* in this short window concurrent discard can set pa_deleted */
	spin_lock(&pa->pa_lock);
	if (!atomic_dec_and_test(&pa->pa_count) || pa->pa_free != 0) {
		spin_unlock(&pa->pa_lock);
		return;
	}

	if (pa->pa_deleted == 1) {
		spin_unlock(&pa->pa_lock);
		return;
	}

	ext4_mb_mark_pa_deleted(sb, pa);
	spin_unlock(&pa->pa_lock);

	grp_blk = pa->pa_pstart;
	/*
	 * If doing group-based preallocation, pa_pstart may be in the
	 * next group when pa is used up
	 */
	if (pa->pa_type == MB_GROUP_PA)
		grp_blk--;

	grp = ext4_get_group_number(sb, grp_blk);

	/*
	 * possible race:
	 *
	 *  P1 (buddy init)			P2 (regular allocation)
	 *					find block B in PA
	 *  copy on-disk bitmap to buddy
	 *  					mark B in on-disk bitmap
	 *					drop PA from group
	 *  mark all PAs in buddy
	 *
	 * thus, P1 initializes buddy with B available. to prevent this
	 * we make "copy" and "mark all PAs" atomic and serialize "drop PA"
	 * against that pair
	 */
	ext4_lock_group(sb, grp);
	list_del(&pa->pa_group_list);
	ext4_unlock_group(sb, grp);

	if (pa->pa_type == MB_INODE_PA) {
		write_lock(pa->pa_node_lock.inode_lock);
		rb_erase(&pa->pa_node.inode_node, &ei->i_prealloc_node);
		write_unlock(pa->pa_node_lock.inode_lock);
		ext4_mb_pa_free(pa);
	} else {
		spin_lock(pa->pa_node_lock.lg_lock);
		list_del_rcu(&pa->pa_node.lg_list);
		spin_unlock(pa->pa_node_lock.lg_lock);
		call_rcu(&(pa)->u.pa_rcu, ext4_mb_pa_callback);
	}
}

static void ext4_mb_pa_rb_insert(struct rb_root *root, struct rb_node *new)
{
	struct rb_node **iter = &root->rb_node, *parent = NULL;
	struct ext4_prealloc_space *iter_pa, *new_pa;
	ext4_lblk_t iter_start, new_start;

	while (*iter) {
		iter_pa = rb_entry(*iter, struct ext4_prealloc_space,
				   pa_node.inode_node);
		new_pa = rb_entry(new, struct ext4_prealloc_space,
				   pa_node.inode_node);
		iter_start = iter_pa->pa_lstart;
		new_start = new_pa->pa_lstart;

		parent = *iter;
		if (new_start < iter_start)
			iter = &((*iter)->rb_left);
		else
			iter = &((*iter)->rb_right);
	}

	rb_link_node(new, parent, iter);
	rb_insert_color(new, root);
}

/*
 * creates new preallocated space for given inode
 */
static noinline_for_stack void
ext4_mb_new_inode_pa(struct ext4_allocation_context *ac)
{
	struct super_block *sb = ac->ac_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_prealloc_space *pa;
	struct ext4_group_info *grp;
	struct ext4_inode_info *ei;

	/* preallocate only when found space is larger then requested */
	BUG_ON(ac->ac_o_ex.fe_len >= ac->ac_b_ex.fe_len);
	BUG_ON(ac->ac_status != AC_STATUS_FOUND);
	BUG_ON(!S_ISREG(ac->ac_inode->i_mode));
	BUG_ON(ac->ac_pa == NULL);

	pa = ac->ac_pa;

	if (ac->ac_b_ex.fe_len < ac->ac_orig_goal_len) {
		struct ext4_free_extent ex = {
			.fe_logical = ac->ac_g_ex.fe_logical,
			.fe_len = ac->ac_orig_goal_len,
		};
		loff_t orig_goal_end = extent_logical_end(sbi, &ex);

		/* we can't allocate as much as normalizer wants.
		 * so, found space must get proper lstart
		 * to cover original request */
		BUG_ON(ac->ac_g_ex.fe_logical > ac->ac_o_ex.fe_logical);
		BUG_ON(ac->ac_g_ex.fe_len < ac->ac_o_ex.fe_len);

		/*
		 * Use the below logic for adjusting best extent as it keeps
		 * fragmentation in check while ensuring logical range of best
		 * extent doesn't overflow out of goal extent:
		 *
		 * 1. Check if best ex can be kept at end of goal (before
		 *    cr_best_avail trimmed it) and still cover original start
		 * 2. Else, check if best ex can be kept at start of goal and
		 *    still cover original start
		 * 3. Else, keep the best ex at start of original request.
		 */
		ex.fe_len = ac->ac_b_ex.fe_len;

		ex.fe_logical = orig_goal_end - EXT4_C2B(sbi, ex.fe_len);
		if (ac->ac_o_ex.fe_logical >= ex.fe_logical)
			goto adjust_bex;

		ex.fe_logical = ac->ac_g_ex.fe_logical;
		if (ac->ac_o_ex.fe_logical < extent_logical_end(sbi, &ex))
			goto adjust_bex;

		ex.fe_logical = ac->ac_o_ex.fe_logical;
adjust_bex:
		ac->ac_b_ex.fe_logical = ex.fe_logical;

		BUG_ON(ac->ac_o_ex.fe_logical < ac->ac_b_ex.fe_logical);
		BUG_ON(ac->ac_o_ex.fe_len > ac->ac_b_ex.fe_len);
		BUG_ON(extent_logical_end(sbi, &ex) > orig_goal_end);
	}

	pa->pa_lstart = ac->ac_b_ex.fe_logical;
	pa->pa_pstart = ext4_grp_offs_to_block(sb, &ac->ac_b_ex);
	pa->pa_len = ac->ac_b_ex.fe_len;
	pa->pa_free = pa->pa_len;
	spin_lock_init(&pa->pa_lock);
	INIT_LIST_HEAD(&pa->pa_group_list);
	pa->pa_deleted = 0;
	pa->pa_type = MB_INODE_PA;

	mb_debug(sb, "new inode pa %p: %llu/%d for %u\n", pa, pa->pa_pstart,
		 pa->pa_len, pa->pa_lstart);
	trace_ext4_mb_new_inode_pa(ac, pa);

	atomic_add(pa->pa_free, &sbi->s_mb_preallocated);
	ext4_mb_use_inode_pa(ac, pa);

	ei = EXT4_I(ac->ac_inode);
	grp = ext4_get_group_info(sb, ac->ac_b_ex.fe_group);
	if (!grp)
		return;

	pa->pa_node_lock.inode_lock = &ei->i_prealloc_lock;
	pa->pa_inode = ac->ac_inode;

	list_add(&pa->pa_group_list, &grp->bb_prealloc_list);

	write_lock(pa->pa_node_lock.inode_lock);
	ext4_mb_pa_rb_insert(&ei->i_prealloc_node, &pa->pa_node.inode_node);
	write_unlock(pa->pa_node_lock.inode_lock);
	atomic_inc(&ei->i_prealloc_active);
}

/*
 * creates new preallocated space for locality group inodes belongs to
 */
static noinline_for_stack void
ext4_mb_new_group_pa(struct ext4_allocation_context *ac)
{
	struct super_block *sb = ac->ac_sb;
	struct ext4_locality_group *lg;
	struct ext4_prealloc_space *pa;
	struct ext4_group_info *grp;

	/* preallocate only when found space is larger then requested */
	BUG_ON(ac->ac_o_ex.fe_len >= ac->ac_b_ex.fe_len);
	BUG_ON(ac->ac_status != AC_STATUS_FOUND);
	BUG_ON(!S_ISREG(ac->ac_inode->i_mode));
	BUG_ON(ac->ac_pa == NULL);

	pa = ac->ac_pa;

	pa->pa_pstart = ext4_grp_offs_to_block(sb, &ac->ac_b_ex);
	pa->pa_lstart = pa->pa_pstart;
	pa->pa_len = ac->ac_b_ex.fe_len;
	pa->pa_free = pa->pa_len;
	spin_lock_init(&pa->pa_lock);
	INIT_LIST_HEAD(&pa->pa_node.lg_list);
	INIT_LIST_HEAD(&pa->pa_group_list);
	pa->pa_deleted = 0;
	pa->pa_type = MB_GROUP_PA;

	mb_debug(sb, "new group pa %p: %llu/%d for %u\n", pa, pa->pa_pstart,
		 pa->pa_len, pa->pa_lstart);
	trace_ext4_mb_new_group_pa(ac, pa);

	ext4_mb_use_group_pa(ac, pa);
	atomic_add(pa->pa_free, &EXT4_SB(sb)->s_mb_preallocated);

	grp = ext4_get_group_info(sb, ac->ac_b_ex.fe_group);
	if (!grp)
		return;
	lg = ac->ac_lg;
	BUG_ON(lg == NULL);

	pa->pa_node_lock.lg_lock = &lg->lg_prealloc_lock;
	pa->pa_inode = NULL;

	list_add(&pa->pa_group_list, &grp->bb_prealloc_list);

	/*
	 * We will later add the new pa to the right bucket
	 * after updating the pa_free in ext4_mb_release_context
	 */
}

static void ext4_mb_new_preallocation(struct ext4_allocation_context *ac)
{
	if (ac->ac_flags & EXT4_MB_HINT_GROUP_ALLOC)
		ext4_mb_new_group_pa(ac);
	else
		ext4_mb_new_inode_pa(ac);
}

/*
 * finds all unused blocks in on-disk bitmap, frees them in
 * in-core bitmap and buddy.
 * @pa must be unlinked from inode and group lists, so that
 * nobody else can find/use it.
 * the caller MUST hold group/inode locks.
 * TODO: optimize the case when there are no in-core structures yet
 */
static noinline_for_stack int
ext4_mb_release_inode_pa(struct ext4_buddy *e4b, struct buffer_head *bitmap_bh,
			struct ext4_prealloc_space *pa)
{
	struct super_block *sb = e4b->bd_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	unsigned int end;
	unsigned int next;
	ext4_group_t group;
	ext4_grpblk_t bit;
	unsigned long long grp_blk_start;
	int free = 0;

	BUG_ON(pa->pa_deleted == 0);
	ext4_get_group_no_and_offset(sb, pa->pa_pstart, &group, &bit);
	grp_blk_start = pa->pa_pstart - EXT4_C2B(sbi, bit);
	BUG_ON(group != e4b->bd_group && pa->pa_len != 0);
	end = bit + pa->pa_len;

	while (bit < end) {
		bit = mb_find_next_zero_bit(bitmap_bh->b_data, end, bit);
		if (bit >= end)
			break;
		next = mb_find_next_bit(bitmap_bh->b_data, end, bit);
		mb_debug(sb, "free preallocated %u/%u in group %u\n",
			 (unsigned) ext4_group_first_block_no(sb, group) + bit,
			 (unsigned) next - bit, (unsigned) group);
		free += next - bit;

		trace_ext4_mballoc_discard(sb, NULL, group, bit, next - bit);
		trace_ext4_mb_release_inode_pa(pa, (grp_blk_start +
						    EXT4_C2B(sbi, bit)),
					       next - bit);
		mb_free_blocks(pa->pa_inode, e4b, bit, next - bit);
		bit = next + 1;
	}
	if (free != pa->pa_free) {
		ext4_msg(e4b->bd_sb, KERN_CRIT,
			 "pa %p: logic %lu, phys. %lu, len %d",
			 pa, (unsigned long) pa->pa_lstart,
			 (unsigned long) pa->pa_pstart,
			 pa->pa_len);
		ext4_grp_locked_error(sb, group, 0, 0, "free %u, pa_free %u",
					free, pa->pa_free);
		/*
		 * pa is already deleted so we use the value obtained
		 * from the bitmap and continue.
		 */
	}
	atomic_add(free, &sbi->s_mb_discarded);

	return 0;
}

static noinline_for_stack int
ext4_mb_release_group_pa(struct ext4_buddy *e4b,
				struct ext4_prealloc_space *pa)
{
	struct super_block *sb = e4b->bd_sb;
	ext4_group_t group;
	ext4_grpblk_t bit;

	trace_ext4_mb_release_group_pa(sb, pa);
	BUG_ON(pa->pa_deleted == 0);
	ext4_get_group_no_and_offset(sb, pa->pa_pstart, &group, &bit);
	if (unlikely(group != e4b->bd_group && pa->pa_len != 0)) {
		ext4_warning(sb, "bad group: expected %u, group %u, pa_start %llu",
			     e4b->bd_group, group, pa->pa_pstart);
		return 0;
	}
	mb_free_blocks(pa->pa_inode, e4b, bit, pa->pa_len);
	atomic_add(pa->pa_len, &EXT4_SB(sb)->s_mb_discarded);
	trace_ext4_mballoc_discard(sb, NULL, group, bit, pa->pa_len);

	return 0;
}

/*
 * releases all preallocations in given group
 *
 * first, we need to decide discard policy:
 * - when do we discard
 *   1) ENOSPC
 * - how many do we discard
 *   1) how many requested
 */
static noinline_for_stack int
ext4_mb_discard_group_preallocations(struct super_block *sb,
				     ext4_group_t group, int *busy)
{
	struct ext4_group_info *grp = ext4_get_group_info(sb, group);
	struct buffer_head *bitmap_bh = NULL;
	struct ext4_prealloc_space *pa, *tmp;
	LIST_HEAD(list);
	struct ext4_buddy e4b;
	struct ext4_inode_info *ei;
	int err;
	int free = 0;

	if (!grp)
		return 0;
	mb_debug(sb, "discard preallocation for group %u\n", group);
	if (list_empty(&grp->bb_prealloc_list))
		goto out_dbg;

	bitmap_bh = ext4_read_block_bitmap(sb, group);
	if (IS_ERR(bitmap_bh)) {
		err = PTR_ERR(bitmap_bh);
		ext4_error_err(sb, -err,
			       "Error %d reading block bitmap for %u",
			       err, group);
		goto out_dbg;
	}

	err = ext4_mb_load_buddy(sb, group, &e4b);
	if (err) {
		ext4_warning(sb, "Error %d loading buddy information for %u",
			     err, group);
		put_bh(bitmap_bh);
		goto out_dbg;
	}

	ext4_lock_group(sb, group);
	list_for_each_entry_safe(pa, tmp,
				&grp->bb_prealloc_list, pa_group_list) {
		spin_lock(&pa->pa_lock);
		if (atomic_read(&pa->pa_count)) {
			spin_unlock(&pa->pa_lock);
			*busy = 1;
			continue;
		}
		if (pa->pa_deleted) {
			spin_unlock(&pa->pa_lock);
			continue;
		}

		/* seems this one can be freed ... */
		ext4_mb_mark_pa_deleted(sb, pa);

		if (!free)
			this_cpu_inc(discard_pa_seq);

		/* we can trust pa_free ... */
		free += pa->pa_free;

		spin_unlock(&pa->pa_lock);

		list_del(&pa->pa_group_list);
		list_add(&pa->u.pa_tmp_list, &list);
	}

	/* now free all selected PAs */
	list_for_each_entry_safe(pa, tmp, &list, u.pa_tmp_list) {

		/* remove from object (inode or locality group) */
		if (pa->pa_type == MB_GROUP_PA) {
			spin_lock(pa->pa_node_lock.lg_lock);
			list_del_rcu(&pa->pa_node.lg_list);
			spin_unlock(pa->pa_node_lock.lg_lock);
		} else {
			write_lock(pa->pa_node_lock.inode_lock);
			ei = EXT4_I(pa->pa_inode);
			rb_erase(&pa->pa_node.inode_node, &ei->i_prealloc_node);
			write_unlock(pa->pa_node_lock.inode_lock);
		}

		list_del(&pa->u.pa_tmp_list);

		if (pa->pa_type == MB_GROUP_PA) {
			ext4_mb_release_group_pa(&e4b, pa);
			call_rcu(&(pa)->u.pa_rcu, ext4_mb_pa_callback);
		} else {
			ext4_mb_release_inode_pa(&e4b, bitmap_bh, pa);
			ext4_mb_pa_free(pa);
		}
	}

	ext4_unlock_group(sb, group);
	ext4_mb_unload_buddy(&e4b);
	put_bh(bitmap_bh);
out_dbg:
	mb_debug(sb, "discarded (%d) blocks preallocated for group %u bb_free (%d)\n",
		 free, group, grp->bb_free);
	return free;
}

/*
 * releases all non-used preallocated blocks for given inode
 *
 * It's important to discard preallocations under i_data_sem
 * We don't want another block to be served from the prealloc
 * space when we are discarding the inode prealloc space.
 *
 * FIXME!! Make sure it is valid at all the call sites
 */
void ext4_discard_preallocations(struct inode *inode, unsigned int needed)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bitmap_bh = NULL;
	struct ext4_prealloc_space *pa, *tmp;
	ext4_group_t group = 0;
	LIST_HEAD(list);
	struct ext4_buddy e4b;
	struct rb_node *iter;
	int err;

	if (!S_ISREG(inode->i_mode)) {
		return;
	}

	if (EXT4_SB(sb)->s_mount_state & EXT4_FC_REPLAY)
		return;

	mb_debug(sb, "discard preallocation for inode %lu\n",
		 inode->i_ino);
	trace_ext4_discard_preallocations(inode,
			atomic_read(&ei->i_prealloc_active), needed);

	if (needed == 0)
		needed = UINT_MAX;

repeat:
	/* first, collect all pa's in the inode */
	write_lock(&ei->i_prealloc_lock);
	for (iter = rb_first(&ei->i_prealloc_node); iter && needed;
	     iter = rb_next(iter)) {
		pa = rb_entry(iter, struct ext4_prealloc_space,
			      pa_node.inode_node);
		BUG_ON(pa->pa_node_lock.inode_lock != &ei->i_prealloc_lock);

		spin_lock(&pa->pa_lock);
		if (atomic_read(&pa->pa_count)) {
			/* this shouldn't happen often - nobody should
			 * use preallocation while we're discarding it */
			spin_unlock(&pa->pa_lock);
			write_unlock(&ei->i_prealloc_lock);
			ext4_msg(sb, KERN_ERR,
				 "uh-oh! used pa while discarding");
			WARN_ON(1);
			schedule_timeout_uninterruptible(HZ);
			goto repeat;

		}
		if (pa->pa_deleted == 0) {
			ext4_mb_mark_pa_deleted(sb, pa);
			spin_unlock(&pa->pa_lock);
			rb_erase(&pa->pa_node.inode_node, &ei->i_prealloc_node);
			list_add(&pa->u.pa_tmp_list, &list);
			needed--;
			continue;
		}

		/* someone is deleting pa right now */
		spin_unlock(&pa->pa_lock);
		write_unlock(&ei->i_prealloc_lock);

		/* we have to wait here because pa_deleted
		 * doesn't mean pa is already unlinked from
		 * the list. as we might be called from
		 * ->clear_inode() the inode will get freed
		 * and concurrent thread which is unlinking
		 * pa from inode's list may access already
		 * freed memory, bad-bad-bad */

		/* XXX: if this happens too often, we can
		 * add a flag to force wait only in case
		 * of ->clear_inode(), but not in case of
		 * regular truncate */
		schedule_timeout_uninterruptible(HZ);
		goto repeat;
	}
	write_unlock(&ei->i_prealloc_lock);

	list_for_each_entry_safe(pa, tmp, &list, u.pa_tmp_list) {
		BUG_ON(pa->pa_type != MB_INODE_PA);
		group = ext4_get_group_number(sb, pa->pa_pstart);

		err = ext4_mb_load_buddy_gfp(sb, group, &e4b,
					     GFP_NOFS|__GFP_NOFAIL);
		if (err) {
			ext4_error_err(sb, -err, "Error %d loading buddy information for %u",
				       err, group);
			continue;
		}

		bitmap_bh = ext4_read_block_bitmap(sb, group);
		if (IS_ERR(bitmap_bh)) {
			err = PTR_ERR(bitmap_bh);
			ext4_error_err(sb, -err, "Error %d reading block bitmap for %u",
				       err, group);
			ext4_mb_unload_buddy(&e4b);
			continue;
		}

		ext4_lock_group(sb, group);
		list_del(&pa->pa_group_list);
		ext4_mb_release_inode_pa(&e4b, bitmap_bh, pa);
		ext4_unlock_group(sb, group);

		ext4_mb_unload_buddy(&e4b);
		put_bh(bitmap_bh);

		list_del(&pa->u.pa_tmp_list);
		ext4_mb_pa_free(pa);
	}
}

static int ext4_mb_pa_alloc(struct ext4_allocation_context *ac)
{
	struct ext4_prealloc_space *pa;

	BUG_ON(ext4_pspace_cachep == NULL);
	pa = kmem_cache_zalloc(ext4_pspace_cachep, GFP_NOFS);
	if (!pa)
		return -ENOMEM;
	atomic_set(&pa->pa_count, 1);
	ac->ac_pa = pa;
	return 0;
}

static void ext4_mb_pa_put_free(struct ext4_allocation_context *ac)
{
	struct ext4_prealloc_space *pa = ac->ac_pa;

	BUG_ON(!pa);
	ac->ac_pa = NULL;
	WARN_ON(!atomic_dec_and_test(&pa->pa_count));
	/*
	 * current function is only called due to an error or due to
	 * len of found blocks < len of requested blocks hence the PA has not
	 * been added to grp->bb_prealloc_list. So we don't need to lock it
	 */
	pa->pa_deleted = 1;
	ext4_mb_pa_free(pa);
}

#ifdef CONFIG_EXT4_DEBUG
static inline void ext4_mb_show_pa(struct super_block *sb)
{
	ext4_group_t i, ngroups;

	if (ext4_forced_shutdown(sb))
		return;

	ngroups = ext4_get_groups_count(sb);
	mb_debug(sb, "groups: ");
	for (i = 0; i < ngroups; i++) {
		struct ext4_group_info *grp = ext4_get_group_info(sb, i);
		struct ext4_prealloc_space *pa;
		ext4_grpblk_t start;
		struct list_head *cur;

		if (!grp)
			continue;
		ext4_lock_group(sb, i);
		list_for_each(cur, &grp->bb_prealloc_list) {
			pa = list_entry(cur, struct ext4_prealloc_space,
					pa_group_list);
			spin_lock(&pa->pa_lock);
			ext4_get_group_no_and_offset(sb, pa->pa_pstart,
						     NULL, &start);
			spin_unlock(&pa->pa_lock);
			mb_debug(sb, "PA:%u:%d:%d\n", i, start,
				 pa->pa_len);
		}
		ext4_unlock_group(sb, i);
		mb_debug(sb, "%u: %d/%d\n", i, grp->bb_free,
			 grp->bb_fragments);
	}
}

static void ext4_mb_show_ac(struct ext4_allocation_context *ac)
{
	struct super_block *sb = ac->ac_sb;

	if (ext4_forced_shutdown(sb))
		return;

	mb_debug(sb, "Can't allocate:"
			" Allocation context details:");
	mb_debug(sb, "status %u flags 0x%x",
			ac->ac_status, ac->ac_flags);
	mb_debug(sb, "orig %lu/%lu/%lu@%lu, "
			"goal %lu/%lu/%lu@%lu, "
			"best %lu/%lu/%lu@%lu cr %d",
			(unsigned long)ac->ac_o_ex.fe_group,
			(unsigned long)ac->ac_o_ex.fe_start,
			(unsigned long)ac->ac_o_ex.fe_len,
			(unsigned long)ac->ac_o_ex.fe_logical,
			(unsigned long)ac->ac_g_ex.fe_group,
			(unsigned long)ac->ac_g_ex.fe_start,
			(unsigned long)ac->ac_g_ex.fe_len,
			(unsigned long)ac->ac_g_ex.fe_logical,
			(unsigned long)ac->ac_b_ex.fe_group,
			(unsigned long)ac->ac_b_ex.fe_start,
			(unsigned long)ac->ac_b_ex.fe_len,
			(unsigned long)ac->ac_b_ex.fe_logical,
			(int)ac->ac_criteria);
	mb_debug(sb, "%u found", ac->ac_found);
	mb_debug(sb, "used pa: %s, ", ac->ac_pa ? "yes" : "no");
	if (ac->ac_pa)
		mb_debug(sb, "pa_type %s\n", ac->ac_pa->pa_type == MB_GROUP_PA ?
			 "group pa" : "inode pa");
	ext4_mb_show_pa(sb);
}
#else
static inline void ext4_mb_show_pa(struct super_block *sb)
{
}
static inline void ext4_mb_show_ac(struct ext4_allocation_context *ac)
{
	ext4_mb_show_pa(ac->ac_sb);
}
#endif

/*
 * We use locality group preallocation for small size file. The size of the
 * file is determined by the current size or the resulting size after
 * allocation which ever is larger
 *
 * One can tune this size via /sys/fs/ext4/<partition>/mb_stream_req
 */
static void ext4_mb_group_or_file(struct ext4_allocation_context *ac)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	int bsbits = ac->ac_sb->s_blocksize_bits;
	loff_t size, isize;
	bool inode_pa_eligible, group_pa_eligible;

	if (!(ac->ac_flags & EXT4_MB_HINT_DATA))
		return;

	if (unlikely(ac->ac_flags & EXT4_MB_HINT_GOAL_ONLY))
		return;

	group_pa_eligible = sbi->s_mb_group_prealloc > 0;
	inode_pa_eligible = true;
	size = extent_logical_end(sbi, &ac->ac_o_ex);
	isize = (i_size_read(ac->ac_inode) + ac->ac_sb->s_blocksize - 1)
		>> bsbits;

	/* No point in using inode preallocation for closed files */
	if ((size == isize) && !ext4_fs_is_busy(sbi) &&
	    !inode_is_open_for_write(ac->ac_inode))
		inode_pa_eligible = false;

	size = max(size, isize);
	/* Don't use group allocation for large files */
	if (size > sbi->s_mb_stream_request)
		group_pa_eligible = false;

	if (!group_pa_eligible) {
		if (inode_pa_eligible)
			ac->ac_flags |= EXT4_MB_STREAM_ALLOC;
		else
			ac->ac_flags |= EXT4_MB_HINT_NOPREALLOC;
		return;
	}

	BUG_ON(ac->ac_lg != NULL);
	/*
	 * locality group prealloc space are per cpu. The reason for having
	 * per cpu locality group is to reduce the contention between block
	 * request from multiple CPUs.
	 */
	ac->ac_lg = raw_cpu_ptr(sbi->s_locality_groups);

	/* we're going to use group allocation */
	ac->ac_flags |= EXT4_MB_HINT_GROUP_ALLOC;

	/* serialize all allocations in the group */
	mutex_lock(&ac->ac_lg->lg_mutex);
}

static noinline_for_stack void
ext4_mb_initialize_context(struct ext4_allocation_context *ac,
				struct ext4_allocation_request *ar)
{
	struct super_block *sb = ar->inode->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_super_block *es = sbi->s_es;
	ext4_group_t group;
	unsigned int len;
	ext4_fsblk_t goal;
	ext4_grpblk_t block;

	/* we can't allocate > group size */
	len = ar->len;

	/* just a dirty hack to filter too big requests  */
	if (len >= EXT4_CLUSTERS_PER_GROUP(sb))
		len = EXT4_CLUSTERS_PER_GROUP(sb);

	/* start searching from the goal */
	goal = ar->goal;
	if (goal < le32_to_cpu(es->s_first_data_block) ||
			goal >= ext4_blocks_count(es))
		goal = le32_to_cpu(es->s_first_data_block);
	ext4_get_group_no_and_offset(sb, goal, &group, &block);

	/* set up allocation goals */
	ac->ac_b_ex.fe_logical = EXT4_LBLK_CMASK(sbi, ar->logical);
	ac->ac_status = AC_STATUS_CONTINUE;
	ac->ac_sb = sb;
	ac->ac_inode = ar->inode;
	ac->ac_o_ex.fe_logical = ac->ac_b_ex.fe_logical;
	ac->ac_o_ex.fe_group = group;
	ac->ac_o_ex.fe_start = block;
	ac->ac_o_ex.fe_len = len;
	ac->ac_g_ex = ac->ac_o_ex;
	ac->ac_orig_goal_len = ac->ac_g_ex.fe_len;
	ac->ac_flags = ar->flags;

	/* we have to define context: we'll work with a file or
	 * locality group. this is a policy, actually */
	ext4_mb_group_or_file(ac);

	mb_debug(sb, "init ac: %u blocks @ %u, goal %u, flags 0x%x, 2^%d, "
			"left: %u/%u, right %u/%u to %swritable\n",
			(unsigned) ar->len, (unsigned) ar->logical,
			(unsigned) ar->goal, ac->ac_flags, ac->ac_2order,
			(unsigned) ar->lleft, (unsigned) ar->pleft,
			(unsigned) ar->lright, (unsigned) ar->pright,
			inode_is_open_for_write(ar->inode) ? "" : "non-");
}

static noinline_for_stack void
ext4_mb_discard_lg_preallocations(struct super_block *sb,
					struct ext4_locality_group *lg,
					int order, int total_entries)
{
	ext4_group_t group = 0;
	struct ext4_buddy e4b;
	LIST_HEAD(discard_list);
	struct ext4_prealloc_space *pa, *tmp;

	mb_debug(sb, "discard locality group preallocation\n");

	spin_lock(&lg->lg_prealloc_lock);
	list_for_each_entry_rcu(pa, &lg->lg_prealloc_list[order],
				pa_node.lg_list,
				lockdep_is_held(&lg->lg_prealloc_lock)) {
		spin_lock(&pa->pa_lock);
		if (atomic_read(&pa->pa_count)) {
			/*
			 * This is the pa that we just used
			 * for block allocation. So don't
			 * free that
			 */
			spin_unlock(&pa->pa_lock);
			continue;
		}
		if (pa->pa_deleted) {
			spin_unlock(&pa->pa_lock);
			continue;
		}
		/* only lg prealloc space */
		BUG_ON(pa->pa_type != MB_GROUP_PA);

		/* seems this one can be freed ... */
		ext4_mb_mark_pa_deleted(sb, pa);
		spin_unlock(&pa->pa_lock);

		list_del_rcu(&pa->pa_node.lg_list);
		list_add(&pa->u.pa_tmp_list, &discard_list);

		total_entries--;
		if (total_entries <= 5) {
			/*
			 * we want to keep only 5 entries
			 * allowing it to grow to 8. This
			 * mak sure we don't call discard
			 * soon for this list.
			 */
			break;
		}
	}
	spin_unlock(&lg->lg_prealloc_lock);

	list_for_each_entry_safe(pa, tmp, &discard_list, u.pa_tmp_list) {
		int err;

		group = ext4_get_group_number(sb, pa->pa_pstart);
		err = ext4_mb_load_buddy_gfp(sb, group, &e4b,
					     GFP_NOFS|__GFP_NOFAIL);
		if (err) {
			ext4_error_err(sb, -err, "Error %d loading buddy information for %u",
				       err, group);
			continue;
		}
		ext4_lock_group(sb, group);
		list_del(&pa->pa_group_list);
		ext4_mb_release_group_pa(&e4b, pa);
		ext4_unlock_group(sb, group);

		ext4_mb_unload_buddy(&e4b);
		list_del(&pa->u.pa_tmp_list);
		call_rcu(&(pa)->u.pa_rcu, ext4_mb_pa_callback);
	}
}

/*
 * We have incremented pa_count. So it cannot be freed at this
 * point. Also we hold lg_mutex. So no parallel allocation is
 * possible from this lg. That means pa_free cannot be updated.
 *
 * A parallel ext4_mb_discard_group_preallocations is possible.
 * which can cause the lg_prealloc_list to be updated.
 */

static void ext4_mb_add_n_trim(struct ext4_allocation_context *ac)
{
	int order, added = 0, lg_prealloc_count = 1;
	struct super_block *sb = ac->ac_sb;
	struct ext4_locality_group *lg = ac->ac_lg;
	struct ext4_prealloc_space *tmp_pa, *pa = ac->ac_pa;

	order = fls(pa->pa_free) - 1;
	if (order > PREALLOC_TB_SIZE - 1)
		/* The max size of hash table is PREALLOC_TB_SIZE */
		order = PREALLOC_TB_SIZE - 1;
	/* Add the prealloc space to lg */
	spin_lock(&lg->lg_prealloc_lock);
	list_for_each_entry_rcu(tmp_pa, &lg->lg_prealloc_list[order],
				pa_node.lg_list,
				lockdep_is_held(&lg->lg_prealloc_lock)) {
		spin_lock(&tmp_pa->pa_lock);
		if (tmp_pa->pa_deleted) {
			spin_unlock(&tmp_pa->pa_lock);
			continue;
		}
		if (!added && pa->pa_free < tmp_pa->pa_free) {
			/* Add to the tail of the previous entry */
			list_add_tail_rcu(&pa->pa_node.lg_list,
						&tmp_pa->pa_node.lg_list);
			added = 1;
			/*
			 * we want to count the total
			 * number of entries in the list
			 */
		}
		spin_unlock(&tmp_pa->pa_lock);
		lg_prealloc_count++;
	}
	if (!added)
		list_add_tail_rcu(&pa->pa_node.lg_list,
					&lg->lg_prealloc_list[order]);
	spin_unlock(&lg->lg_prealloc_lock);

	/* Now trim the list to be not more than 8 elements */
	if (lg_prealloc_count > 8)
		ext4_mb_discard_lg_preallocations(sb, lg,
						  order, lg_prealloc_count);
}

/*
 * release all resource we used in allocation
 */
static int ext4_mb_release_context(struct ext4_allocation_context *ac)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct ext4_prealloc_space *pa = ac->ac_pa;
	if (pa) {
		if (pa->pa_type == MB_GROUP_PA) {
			/* see comment in ext4_mb_use_group_pa() */
			spin_lock(&pa->pa_lock);
			pa->pa_pstart += EXT4_C2B(sbi, ac->ac_b_ex.fe_len);
			pa->pa_lstart += EXT4_C2B(sbi, ac->ac_b_ex.fe_len);
			pa->pa_free -= ac->ac_b_ex.fe_len;
			pa->pa_len -= ac->ac_b_ex.fe_len;
			spin_unlock(&pa->pa_lock);

			/*
			 * We want to add the pa to the right bucket.
			 * Remove it from the list and while adding
			 * make sure the list to which we are adding
			 * doesn't grow big.
			 */
			if (likely(pa->pa_free)) {
				spin_lock(pa->pa_node_lock.lg_lock);
				list_del_rcu(&pa->pa_node.lg_list);
				spin_unlock(pa->pa_node_lock.lg_lock);
				ext4_mb_add_n_trim(ac);
			}
		}

		ext4_mb_put_pa(ac, ac->ac_sb, pa);
	}
	if (ac->ac_bitmap_page)
		put_page(ac->ac_bitmap_page);
	if (ac->ac_buddy_page)
		put_page(ac->ac_buddy_page);
	if (ac->ac_flags & EXT4_MB_HINT_GROUP_ALLOC)
		mutex_unlock(&ac->ac_lg->lg_mutex);
	ext4_mb_collect_stats(ac);
	return 0;
}

static int ext4_mb_discard_preallocations(struct super_block *sb, int needed)
{
	ext4_group_t i, ngroups = ext4_get_groups_count(sb);
	int ret;
	int freed = 0, busy = 0;
	int retry = 0;

	trace_ext4_mb_discard_preallocations(sb, needed);

	if (needed == 0)
		needed = EXT4_CLUSTERS_PER_GROUP(sb) + 1;
 repeat:
	for (i = 0; i < ngroups && needed > 0; i++) {
		ret = ext4_mb_discard_group_preallocations(sb, i, &busy);
		freed += ret;
		needed -= ret;
		cond_resched();
	}

	if (needed > 0 && busy && ++retry < 3) {
		busy = 0;
		goto repeat;
	}

	return freed;
}

static bool ext4_mb_discard_preallocations_should_retry(struct super_block *sb,
			struct ext4_allocation_context *ac, u64 *seq)
{
	int freed;
	u64 seq_retry = 0;
	bool ret = false;

	freed = ext4_mb_discard_preallocations(sb, ac->ac_o_ex.fe_len);
	if (freed) {
		ret = true;
		goto out_dbg;
	}
	seq_retry = ext4_get_discard_pa_seq_sum();
	if (!(ac->ac_flags & EXT4_MB_STRICT_CHECK) || seq_retry != *seq) {
		ac->ac_flags |= EXT4_MB_STRICT_CHECK;
		*seq = seq_retry;
		ret = true;
	}

out_dbg:
	mb_debug(sb, "freed %d, retry ? %s\n", freed, ret ? "yes" : "no");
	return ret;
}

/*
 * Simple allocator for Ext4 fast commit replay path. It searches for blocks
 * linearly starting at the goal block and also excludes the blocks which
 * are going to be in use after fast commit replay.
 */
static ext4_fsblk_t
ext4_mb_new_blocks_simple(struct ext4_allocation_request *ar, int *errp)
{
	struct buffer_head *bitmap_bh;
	struct super_block *sb = ar->inode->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_group_t group, nr;
	ext4_grpblk_t blkoff;
	ext4_grpblk_t max = EXT4_CLUSTERS_PER_GROUP(sb);
	ext4_grpblk_t i = 0;
	ext4_fsblk_t goal, block;
	struct ext4_super_block *es = sbi->s_es;

	goal = ar->goal;
	if (goal < le32_to_cpu(es->s_first_data_block) ||
			goal >= ext4_blocks_count(es))
		goal = le32_to_cpu(es->s_first_data_block);

	ar->len = 0;
	ext4_get_group_no_and_offset(sb, goal, &group, &blkoff);
	for (nr = ext4_get_groups_count(sb); nr > 0; nr--) {
		bitmap_bh = ext4_read_block_bitmap(sb, group);
		if (IS_ERR(bitmap_bh)) {
			*errp = PTR_ERR(bitmap_bh);
			pr_warn("Failed to read block bitmap\n");
			return 0;
		}

		while (1) {
			i = mb_find_next_zero_bit(bitmap_bh->b_data, max,
						blkoff);
			if (i >= max)
				break;
			if (ext4_fc_replay_check_excluded(sb,
				ext4_group_first_block_no(sb, group) +
				EXT4_C2B(sbi, i))) {
				blkoff = i + 1;
			} else
				break;
		}
		brelse(bitmap_bh);
		if (i < max)
			break;

		if (++group >= ext4_get_groups_count(sb))
			group = 0;

		blkoff = 0;
	}

	if (i >= max) {
		*errp = -ENOSPC;
		return 0;
	}

	block = ext4_group_first_block_no(sb, group) + EXT4_C2B(sbi, i);
	ext4_mb_mark_bb(sb, block, 1, 1);
	ar->len = 1;

	return block;
}

/*
 * Main entry point into mballoc to allocate blocks
 * it tries to use preallocation first, then falls back
 * to usual allocation
 */
ext4_fsblk_t ext4_mb_new_blocks(handle_t *handle,
				struct ext4_allocation_request *ar, int *errp)
{
	struct ext4_allocation_context *ac = NULL;
	struct ext4_sb_info *sbi;
	struct super_block *sb;
	ext4_fsblk_t block = 0;
	unsigned int inquota = 0;
	unsigned int reserv_clstrs = 0;
	int retries = 0;
	u64 seq;

	might_sleep();
	sb = ar->inode->i_sb;
	sbi = EXT4_SB(sb);

	trace_ext4_request_blocks(ar);
	if (sbi->s_mount_state & EXT4_FC_REPLAY)
		return ext4_mb_new_blocks_simple(ar, errp);

	/* Allow to use superuser reservation for quota file */
	if (ext4_is_quota_file(ar->inode))
		ar->flags |= EXT4_MB_USE_ROOT_BLOCKS;

	if ((ar->flags & EXT4_MB_DELALLOC_RESERVED) == 0) {
		/* Without delayed allocation we need to verify
		 * there is enough free blocks to do block allocation
		 * and verify allocation doesn't exceed the quota limits.
		 */
		while (ar->len &&
			ext4_claim_free_clusters(sbi, ar->len, ar->flags)) {

			/* let others to free the space */
			cond_resched();
			ar->len = ar->len >> 1;
		}
		if (!ar->len) {
			ext4_mb_show_pa(sb);
			*errp = -ENOSPC;
			return 0;
		}
		reserv_clstrs = ar->len;
		if (ar->flags & EXT4_MB_USE_ROOT_BLOCKS) {
			dquot_alloc_block_nofail(ar->inode,
						 EXT4_C2B(sbi, ar->len));
		} else {
			while (ar->len &&
				dquot_alloc_block(ar->inode,
						  EXT4_C2B(sbi, ar->len))) {

				ar->flags |= EXT4_MB_HINT_NOPREALLOC;
				ar->len--;
			}
		}
		inquota = ar->len;
		if (ar->len == 0) {
			*errp = -EDQUOT;
			goto out;
		}
	}

	ac = kmem_cache_zalloc(ext4_ac_cachep, GFP_NOFS);
	if (!ac) {
		ar->len = 0;
		*errp = -ENOMEM;
		goto out;
	}

	ext4_mb_initialize_context(ac, ar);

	ac->ac_op = EXT4_MB_HISTORY_PREALLOC;
	seq = this_cpu_read(discard_pa_seq);
	if (!ext4_mb_use_preallocated(ac)) {
		ac->ac_op = EXT4_MB_HISTORY_ALLOC;
		ext4_mb_normalize_request(ac, ar);

		*errp = ext4_mb_pa_alloc(ac);
		if (*errp)
			goto errout;
repeat:
		/* allocate space in core */
		*errp = ext4_mb_regular_allocator(ac);
		/*
		 * pa allocated above is added to grp->bb_prealloc_list only
		 * when we were able to allocate some block i.e. when
		 * ac->ac_status == AC_STATUS_FOUND.
		 * And error from above mean ac->ac_status != AC_STATUS_FOUND
		 * So we have to free this pa here itself.
		 */
		if (*errp) {
			ext4_mb_pa_put_free(ac);
			ext4_discard_allocated_blocks(ac);
			goto errout;
		}
		if (ac->ac_status == AC_STATUS_FOUND &&
			ac->ac_o_ex.fe_len >= ac->ac_f_ex.fe_len)
			ext4_mb_pa_put_free(ac);
	}
	if (likely(ac->ac_status == AC_STATUS_FOUND)) {
		*errp = ext4_mb_mark_diskspace_used(ac, handle, reserv_clstrs);
		if (*errp) {
			ext4_discard_allocated_blocks(ac);
			goto errout;
		} else {
			block = ext4_grp_offs_to_block(sb, &ac->ac_b_ex);
			ar->len = ac->ac_b_ex.fe_len;
		}
	} else {
		if (++retries < 3 &&
		    ext4_mb_discard_preallocations_should_retry(sb, ac, &seq))
			goto repeat;
		/*
		 * If block allocation fails then the pa allocated above
		 * needs to be freed here itself.
		 */
		ext4_mb_pa_put_free(ac);
		*errp = -ENOSPC;
	}

	if (*errp) {
errout:
		ac->ac_b_ex.fe_len = 0;
		ar->len = 0;
		ext4_mb_show_ac(ac);
	}
	ext4_mb_release_context(ac);
	kmem_cache_free(ext4_ac_cachep, ac);
out:
	if (inquota && ar->len < inquota)
		dquot_free_block(ar->inode, EXT4_C2B(sbi, inquota - ar->len));
	if (!ar->len) {
		if ((ar->flags & EXT4_MB_DELALLOC_RESERVED) == 0)
			/* release all the reserved blocks if non delalloc */
			percpu_counter_sub(&sbi->s_dirtyclusters_counter,
						reserv_clstrs);
	}

	trace_ext4_allocate_blocks(ar, (unsigned long long)block);

	return block;
}

/*
 * We can merge two free data extents only if the physical blocks
 * are contiguous, AND the extents were freed by the same transaction,
 * AND the blocks are associated with the same group.
 */
static void ext4_try_merge_freed_extent(struct ext4_sb_info *sbi,
					struct ext4_free_data *entry,
					struct ext4_free_data *new_entry,
					struct rb_root *entry_rb_root)
{
	if ((entry->efd_tid != new_entry->efd_tid) ||
	    (entry->efd_group != new_entry->efd_group))
		return;
	if (entry->efd_start_cluster + entry->efd_count ==
	    new_entry->efd_start_cluster) {
		new_entry->efd_start_cluster = entry->efd_start_cluster;
		new_entry->efd_count += entry->efd_count;
	} else if (new_entry->efd_start_cluster + new_entry->efd_count ==
		   entry->efd_start_cluster) {
		new_entry->efd_count += entry->efd_count;
	} else
		return;
	spin_lock(&sbi->s_md_lock);
	list_del(&entry->efd_list);
	spin_unlock(&sbi->s_md_lock);
	rb_erase(&entry->efd_node, entry_rb_root);
	kmem_cache_free(ext4_free_data_cachep, entry);
}

static noinline_for_stack void
ext4_mb_free_metadata(handle_t *handle, struct ext4_buddy *e4b,
		      struct ext4_free_data *new_entry)
{
	ext4_group_t group = e4b->bd_group;
	ext4_grpblk_t cluster;
	ext4_grpblk_t clusters = new_entry->efd_count;
	struct ext4_free_data *entry;
	struct ext4_group_info *db = e4b->bd_info;
	struct super_block *sb = e4b->bd_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct rb_node **n = &db->bb_free_root.rb_node, *node;
	struct rb_node *parent = NULL, *new_node;

	BUG_ON(!ext4_handle_valid(handle));
	BUG_ON(e4b->bd_bitmap_page == NULL);
	BUG_ON(e4b->bd_buddy_page == NULL);

	new_node = &new_entry->efd_node;
	cluster = new_entry->efd_start_cluster;

	if (!*n) {
		/* first free block exent. We need to
		   protect buddy cache from being freed,
		 * otherwise we'll refresh it from
		 * on-disk bitmap and lose not-yet-available
		 * blocks */
		get_page(e4b->bd_buddy_page);
		get_page(e4b->bd_bitmap_page);
	}
	while (*n) {
		parent = *n;
		entry = rb_entry(parent, struct ext4_free_data, efd_node);
		if (cluster < entry->efd_start_cluster)
			n = &(*n)->rb_left;
		else if (cluster >= (entry->efd_start_cluster + entry->efd_count))
			n = &(*n)->rb_right;
		else {
			ext4_grp_locked_error(sb, group, 0,
				ext4_group_first_block_no(sb, group) +
				EXT4_C2B(sbi, cluster),
				"Block already on to-be-freed list");
			kmem_cache_free(ext4_free_data_cachep, new_entry);
			return;
		}
	}

	rb_link_node(new_node, parent, n);
	rb_insert_color(new_node, &db->bb_free_root);

	/* Now try to see the extent can be merged to left and right */
	node = rb_prev(new_node);
	if (node) {
		entry = rb_entry(node, struct ext4_free_data, efd_node);
		ext4_try_merge_freed_extent(sbi, entry, new_entry,
					    &(db->bb_free_root));
	}

	node = rb_next(new_node);
	if (node) {
		entry = rb_entry(node, struct ext4_free_data, efd_node);
		ext4_try_merge_freed_extent(sbi, entry, new_entry,
					    &(db->bb_free_root));
	}

	spin_lock(&sbi->s_md_lock);
	list_add_tail(&new_entry->efd_list, &sbi->s_freed_data_list);
	sbi->s_mb_free_pending += clusters;
	spin_unlock(&sbi->s_md_lock);
}

static void ext4_free_blocks_simple(struct inode *inode, ext4_fsblk_t block,
					unsigned long count)
{
	struct buffer_head *bitmap_bh;
	struct super_block *sb = inode->i_sb;
	struct ext4_group_desc *gdp;
	struct buffer_head *gdp_bh;
	ext4_group_t group;
	ext4_grpblk_t blkoff;
	int already_freed = 0, err, i;

	ext4_get_group_no_and_offset(sb, block, &group, &blkoff);
	bitmap_bh = ext4_read_block_bitmap(sb, group);
	if (IS_ERR(bitmap_bh)) {
		pr_warn("Failed to read block bitmap\n");
		return;
	}
	gdp = ext4_get_group_desc(sb, group, &gdp_bh);
	if (!gdp)
		goto err_out;

	for (i = 0; i < count; i++) {
		if (!mb_test_bit(blkoff + i, bitmap_bh->b_data))
			already_freed++;
	}
	mb_clear_bits(bitmap_bh->b_data, blkoff, count);
	err = ext4_handle_dirty_metadata(NULL, NULL, bitmap_bh);
	if (err)
		goto err_out;
	ext4_free_group_clusters_set(
		sb, gdp, ext4_free_group_clusters(sb, gdp) +
		count - already_freed);
	ext4_block_bitmap_csum_set(sb, gdp, bitmap_bh);
	ext4_group_desc_csum_set(sb, group, gdp);
	ext4_handle_dirty_metadata(NULL, NULL, gdp_bh);
	sync_dirty_buffer(bitmap_bh);
	sync_dirty_buffer(gdp_bh);

err_out:
	brelse(bitmap_bh);
}

/**
 * ext4_mb_clear_bb() -- helper function for freeing blocks.
 *			Used by ext4_free_blocks()
 * @handle:		handle for this transaction
 * @inode:		inode
 * @block:		starting physical block to be freed
 * @count:		number of blocks to be freed
 * @flags:		flags used by ext4_free_blocks
 */
static void ext4_mb_clear_bb(handle_t *handle, struct inode *inode,
			       ext4_fsblk_t block, unsigned long count,
			       int flags)
{
	struct buffer_head *bitmap_bh = NULL;
	struct super_block *sb = inode->i_sb;
	struct ext4_group_desc *gdp;
	struct ext4_group_info *grp;
	unsigned int overflow;
	ext4_grpblk_t bit;
	struct buffer_head *gd_bh;
	ext4_group_t block_group;
	struct ext4_sb_info *sbi;
	struct ext4_buddy e4b;
	unsigned int count_clusters;
	int err = 0;
	int ret;

	sbi = EXT4_SB(sb);

	if (!(flags & EXT4_FREE_BLOCKS_VALIDATED) &&
	    !ext4_inode_block_valid(inode, block, count)) {
		ext4_error(sb, "Freeing blocks in system zone - "
			   "Block = %llu, count = %lu", block, count);
		/* err = 0. ext4_std_error should be a no op */
		goto error_return;
	}
	flags |= EXT4_FREE_BLOCKS_VALIDATED;

do_more:
	overflow = 0;
	ext4_get_group_no_and_offset(sb, block, &block_group, &bit);

	grp = ext4_get_group_info(sb, block_group);
	if (unlikely(!grp || EXT4_MB_GRP_BBITMAP_CORRUPT(grp)))
		return;

	/*
	 * Check to see if we are freeing blocks across a group
	 * boundary.
	 */
	if (EXT4_C2B(sbi, bit) + count > EXT4_BLOCKS_PER_GROUP(sb)) {
		overflow = EXT4_C2B(sbi, bit) + count -
			EXT4_BLOCKS_PER_GROUP(sb);
		count -= overflow;
		/* The range changed so it's no longer validated */
		flags &= ~EXT4_FREE_BLOCKS_VALIDATED;
	}
	count_clusters = EXT4_NUM_B2C(sbi, count);
	bitmap_bh = ext4_read_block_bitmap(sb, block_group);
	if (IS_ERR(bitmap_bh)) {
		err = PTR_ERR(bitmap_bh);
		bitmap_bh = NULL;
		goto error_return;
	}
	gdp = ext4_get_group_desc(sb, block_group, &gd_bh);
	if (!gdp) {
		err = -EIO;
		goto error_return;
	}

	if (!(flags & EXT4_FREE_BLOCKS_VALIDATED) &&
	    !ext4_inode_block_valid(inode, block, count)) {
		ext4_error(sb, "Freeing blocks in system zone - "
			   "Block = %llu, count = %lu", block, count);
		/* err = 0. ext4_std_error should be a no op */
		goto error_return;
	}

	BUFFER_TRACE(bitmap_bh, "getting write access");
	err = ext4_journal_get_write_access(handle, sb, bitmap_bh,
					    EXT4_JTR_NONE);
	if (err)
		goto error_return;

	/*
	 * We are about to modify some metadata.  Call the journal APIs
	 * to unshare ->b_data if a currently-committing transaction is
	 * using it
	 */
	BUFFER_TRACE(gd_bh, "get_write_access");
	err = ext4_journal_get_write_access(handle, sb, gd_bh, EXT4_JTR_NONE);
	if (err)
		goto error_return;
#ifdef AGGRESSIVE_CHECK
	{
		int i;
		for (i = 0; i < count_clusters; i++)
			BUG_ON(!mb_test_bit(bit + i, bitmap_bh->b_data));
	}
#endif
	trace_ext4_mballoc_free(sb, inode, block_group, bit, count_clusters);

	/* __GFP_NOFAIL: retry infinitely, ignore TIF_MEMDIE and memcg limit. */
	err = ext4_mb_load_buddy_gfp(sb, block_group, &e4b,
				     GFP_NOFS|__GFP_NOFAIL);
	if (err)
		goto error_return;

	/*
	 * We need to make sure we don't reuse the freed block until after the
	 * transaction is committed. We make an exception if the inode is to be
	 * written in writeback mode since writeback mode has weak data
	 * consistency guarantees.
	 */
	if (ext4_handle_valid(handle) &&
	    ((flags & EXT4_FREE_BLOCKS_METADATA) ||
	     !ext4_should_writeback_data(inode))) {
		struct ext4_free_data *new_entry;
		/*
		 * We use __GFP_NOFAIL because ext4_free_blocks() is not allowed
		 * to fail.
		 */
		new_entry = kmem_cache_alloc(ext4_free_data_cachep,
				GFP_NOFS|__GFP_NOFAIL);
		new_entry->efd_start_cluster = bit;
		new_entry->efd_group = block_group;
		new_entry->efd_count = count_clusters;
		new_entry->efd_tid = handle->h_transaction->t_tid;

		ext4_lock_group(sb, block_group);
		mb_clear_bits(bitmap_bh->b_data, bit, count_clusters);
		ext4_mb_free_metadata(handle, &e4b, new_entry);
	} else {
		/* need to update group_info->bb_free and bitmap
		 * with group lock held. generate_buddy look at
		 * them with group lock_held
		 */
		if (test_opt(sb, DISCARD)) {
			err = ext4_issue_discard(sb, block_group, bit,
						 count_clusters, NULL);
			if (err && err != -EOPNOTSUPP)
				ext4_msg(sb, KERN_WARNING, "discard request in"
					 " group:%u block:%d count:%lu failed"
					 " with %d", block_group, bit, count,
					 err);
		} else
			EXT4_MB_GRP_CLEAR_TRIMMED(e4b.bd_info);

		ext4_lock_group(sb, block_group);
		mb_clear_bits(bitmap_bh->b_data, bit, count_clusters);
		mb_free_blocks(inode, &e4b, bit, count_clusters);
	}

	ret = ext4_free_group_clusters(sb, gdp) + count_clusters;
	ext4_free_group_clusters_set(sb, gdp, ret);
	ext4_block_bitmap_csum_set(sb, gdp, bitmap_bh);
	ext4_group_desc_csum_set(sb, block_group, gdp);
	ext4_unlock_group(sb, block_group);

	if (sbi->s_log_groups_per_flex) {
		ext4_group_t flex_group = ext4_flex_group(sbi, block_group);
		atomic64_add(count_clusters,
			     &sbi_array_rcu_deref(sbi, s_flex_groups,
						  flex_group)->free_clusters);
	}

	/*
	 * on a bigalloc file system, defer the s_freeclusters_counter
	 * update to the caller (ext4_remove_space and friends) so they
	 * can determine if a cluster freed here should be rereserved
	 */
	if (!(flags & EXT4_FREE_BLOCKS_RERESERVE_CLUSTER)) {
		if (!(flags & EXT4_FREE_BLOCKS_NO_QUOT_UPDATE))
			dquot_free_block(inode, EXT4_C2B(sbi, count_clusters));
		percpu_counter_add(&sbi->s_freeclusters_counter,
				   count_clusters);
	}

	ext4_mb_unload_buddy(&e4b);

	/* We dirtied the bitmap block */
	BUFFER_TRACE(bitmap_bh, "dirtied bitmap block");
	err = ext4_handle_dirty_metadata(handle, NULL, bitmap_bh);

	/* And the group descriptor block */
	BUFFER_TRACE(gd_bh, "dirtied group descriptor block");
	ret = ext4_handle_dirty_metadata(handle, NULL, gd_bh);
	if (!err)
		err = ret;

	if (overflow && !err) {
		block += count;
		count = overflow;
		put_bh(bitmap_bh);
		/* The range changed so it's no longer validated */
		flags &= ~EXT4_FREE_BLOCKS_VALIDATED;
		goto do_more;
	}
error_return:
	brelse(bitmap_bh);
	ext4_std_error(sb, err);
}

/**
 * ext4_free_blocks() -- Free given blocks and update quota
 * @handle:		handle for this transaction
 * @inode:		inode
 * @bh:			optional buffer of the block to be freed
 * @block:		starting physical block to be freed
 * @count:		number of blocks to be freed
 * @flags:		flags used by ext4_free_blocks
 */
void ext4_free_blocks(handle_t *handle, struct inode *inode,
		      struct buffer_head *bh, ext4_fsblk_t block,
		      unsigned long count, int flags)
{
	struct super_block *sb = inode->i_sb;
	unsigned int overflow;
	struct ext4_sb_info *sbi;

	sbi = EXT4_SB(sb);

	if (bh) {
		if (block)
			BUG_ON(block != bh->b_blocknr);
		else
			block = bh->b_blocknr;
	}

	if (sbi->s_mount_state & EXT4_FC_REPLAY) {
		ext4_free_blocks_simple(inode, block, EXT4_NUM_B2C(sbi, count));
		return;
	}

	might_sleep();

	if (!(flags & EXT4_FREE_BLOCKS_VALIDATED) &&
	    !ext4_inode_block_valid(inode, block, count)) {
		ext4_error(sb, "Freeing blocks not in datazone - "
			   "block = %llu, count = %lu", block, count);
		return;
	}
	flags |= EXT4_FREE_BLOCKS_VALIDATED;

	ext4_debug("freeing block %llu\n", block);
	trace_ext4_free_blocks(inode, block, count, flags);

	if (bh && (flags & EXT4_FREE_BLOCKS_FORGET)) {
		BUG_ON(count > 1);

		ext4_forget(handle, flags & EXT4_FREE_BLOCKS_METADATA,
			    inode, bh, block);
	}

	/*
	 * If the extent to be freed does not begin on a cluster
	 * boundary, we need to deal with partial clusters at the
	 * beginning and end of the extent.  Normally we will free
	 * blocks at the beginning or the end unless we are explicitly
	 * requested to avoid doing so.
	 */
	overflow = EXT4_PBLK_COFF(sbi, block);
	if (overflow) {
		if (flags & EXT4_FREE_BLOCKS_NOFREE_FIRST_CLUSTER) {
			overflow = sbi->s_cluster_ratio - overflow;
			block += overflow;
			if (count > overflow)
				count -= overflow;
			else
				return;
		} else {
			block -= overflow;
			count += overflow;
		}
		/* The range changed so it's no longer validated */
		flags &= ~EXT4_FREE_BLOCKS_VALIDATED;
	}
	overflow = EXT4_LBLK_COFF(sbi, count);
	if (overflow) {
		if (flags & EXT4_FREE_BLOCKS_NOFREE_LAST_CLUSTER) {
			if (count > overflow)
				count -= overflow;
			else
				return;
		} else
			count += sbi->s_cluster_ratio - overflow;
		/* The range changed so it's no longer validated */
		flags &= ~EXT4_FREE_BLOCKS_VALIDATED;
	}

	if (!bh && (flags & EXT4_FREE_BLOCKS_FORGET)) {
		int i;
		int is_metadata = flags & EXT4_FREE_BLOCKS_METADATA;

		for (i = 0; i < count; i++) {
			cond_resched();
			if (is_metadata)
				bh = sb_find_get_block(inode->i_sb, block + i);
			ext4_forget(handle, is_metadata, inode, bh, block + i);
		}
	}

	ext4_mb_clear_bb(handle, inode, block, count, flags);
}

/**
 * ext4_group_add_blocks() -- Add given blocks to an existing group
 * @handle:			handle to this transaction
 * @sb:				super block
 * @block:			start physical block to add to the block group
 * @count:			number of blocks to free
 *
 * This marks the blocks as free in the bitmap and buddy.
 */
int ext4_group_add_blocks(handle_t *handle, struct super_block *sb,
			 ext4_fsblk_t block, unsigned long count)
{
	struct buffer_head *bitmap_bh = NULL;
	struct buffer_head *gd_bh;
	ext4_group_t block_group;
	ext4_grpblk_t bit;
	unsigned int i;
	struct ext4_group_desc *desc;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_buddy e4b;
	int err = 0, ret, free_clusters_count;
	ext4_grpblk_t clusters_freed;
	ext4_fsblk_t first_cluster = EXT4_B2C(sbi, block);
	ext4_fsblk_t last_cluster = EXT4_B2C(sbi, block + count - 1);
	unsigned long cluster_count = last_cluster - first_cluster + 1;

	ext4_debug("Adding block(s) %llu-%llu\n", block, block + count - 1);

	if (count == 0)
		return 0;

	ext4_get_group_no_and_offset(sb, block, &block_group, &bit);
	/*
	 * Check to see if we are freeing blocks across a group
	 * boundary.
	 */
	if (bit + cluster_count > EXT4_CLUSTERS_PER_GROUP(sb)) {
		ext4_warning(sb, "too many blocks added to group %u",
			     block_group);
		err = -EINVAL;
		goto error_return;
	}

	bitmap_bh = ext4_read_block_bitmap(sb, block_group);
	if (IS_ERR(bitmap_bh)) {
		err = PTR_ERR(bitmap_bh);
		bitmap_bh = NULL;
		goto error_return;
	}

	desc = ext4_get_group_desc(sb, block_group, &gd_bh);
	if (!desc) {
		err = -EIO;
		goto error_return;
	}

	if (!ext4_sb_block_valid(sb, NULL, block, count)) {
		ext4_error(sb, "Adding blocks in system zones - "
			   "Block = %llu, count = %lu",
			   block, count);
		err = -EINVAL;
		goto error_return;
	}

	BUFFER_TRACE(bitmap_bh, "getting write access");
	err = ext4_journal_get_write_access(handle, sb, bitmap_bh,
					    EXT4_JTR_NONE);
	if (err)
		goto error_return;

	/*
	 * We are about to modify some metadata.  Call the journal APIs
	 * to unshare ->b_data if a currently-committing transaction is
	 * using it
	 */
	BUFFER_TRACE(gd_bh, "get_write_access");
	err = ext4_journal_get_write_access(handle, sb, gd_bh, EXT4_JTR_NONE);
	if (err)
		goto error_return;

	for (i = 0, clusters_freed = 0; i < cluster_count; i++) {
		BUFFER_TRACE(bitmap_bh, "clear bit");
		if (!mb_test_bit(bit + i, bitmap_bh->b_data)) {
			ext4_error(sb, "bit already cleared for block %llu",
				   (ext4_fsblk_t)(block + i));
			BUFFER_TRACE(bitmap_bh, "bit already cleared");
		} else {
			clusters_freed++;
		}
	}

	err = ext4_mb_load_buddy(sb, block_group, &e4b);
	if (err)
		goto error_return;

	/*
	 * need to update group_info->bb_free and bitmap
	 * with group lock held. generate_buddy look at
	 * them with group lock_held
	 */
	ext4_lock_group(sb, block_group);
	mb_clear_bits(bitmap_bh->b_data, bit, cluster_count);
	mb_free_blocks(NULL, &e4b, bit, cluster_count);
	free_clusters_count = clusters_freed +
		ext4_free_group_clusters(sb, desc);
	ext4_free_group_clusters_set(sb, desc, free_clusters_count);
	ext4_block_bitmap_csum_set(sb, desc, bitmap_bh);
	ext4_group_desc_csum_set(sb, block_group, desc);
	ext4_unlock_group(sb, block_group);
	percpu_counter_add(&sbi->s_freeclusters_counter,
			   clusters_freed);

	if (sbi->s_log_groups_per_flex) {
		ext4_group_t flex_group = ext4_flex_group(sbi, block_group);
		atomic64_add(clusters_freed,
			     &sbi_array_rcu_deref(sbi, s_flex_groups,
						  flex_group)->free_clusters);
	}

	ext4_mb_unload_buddy(&e4b);

	/* We dirtied the bitmap block */
	BUFFER_TRACE(bitmap_bh, "dirtied bitmap block");
	err = ext4_handle_dirty_metadata(handle, NULL, bitmap_bh);

	/* And the group descriptor block */
	BUFFER_TRACE(gd_bh, "dirtied group descriptor block");
	ret = ext4_handle_dirty_metadata(handle, NULL, gd_bh);
	if (!err)
		err = ret;

error_return:
	brelse(bitmap_bh);
	ext4_std_error(sb, err);
	return err;
}

/**
 * ext4_trim_extent -- function to TRIM one single free extent in the group
 * @sb:		super block for the file system
 * @start:	starting block of the free extent in the alloc. group
 * @count:	number of blocks to TRIM
 * @e4b:	ext4 buddy for the group
 *
 * Trim "count" blocks starting at "start" in the "group". To assure that no
 * one will allocate those blocks, mark it as used in buddy bitmap. This must
 * be called with under the group lock.
 */
static int ext4_trim_extent(struct super_block *sb,
		int start, int count, struct ext4_buddy *e4b)
__releases(bitlock)
__acquires(bitlock)
{
	struct ext4_free_extent ex;
	ext4_group_t group = e4b->bd_group;
	int ret = 0;

	trace_ext4_trim_extent(sb, group, start, count);

	assert_spin_locked(ext4_group_lock_ptr(sb, group));

	ex.fe_start = start;
	ex.fe_group = group;
	ex.fe_len = count;

	/*
	 * Mark blocks used, so no one can reuse them while
	 * being trimmed.
	 */
	mb_mark_used(e4b, &ex);
	ext4_unlock_group(sb, group);
	ret = ext4_issue_discard(sb, group, start, count, NULL);
	ext4_lock_group(sb, group);
	mb_free_blocks(NULL, e4b, start, ex.fe_len);
	return ret;
}

static ext4_grpblk_t ext4_last_grp_cluster(struct super_block *sb,
					   ext4_group_t grp)
{
	if (grp < ext4_get_groups_count(sb))
		return EXT4_CLUSTERS_PER_GROUP(sb) - 1;
	return (ext4_blocks_count(EXT4_SB(sb)->s_es) -
		ext4_group_first_block_no(sb, grp) - 1) >>
					EXT4_CLUSTER_BITS(sb);
}

static bool ext4_trim_interrupted(void)
{
	return fatal_signal_pending(current) || freezing(current);
}

static int ext4_try_to_trim_range(struct super_block *sb,
		struct ext4_buddy *e4b, ext4_grpblk_t start,
		ext4_grpblk_t max, ext4_grpblk_t minblocks)
__acquires(ext4_group_lock_ptr(sb, e4b->bd_group))
__releases(ext4_group_lock_ptr(sb, e4b->bd_group))
{
	ext4_grpblk_t next, count, free_count;
	bool set_trimmed = false;
	void *bitmap;

	bitmap = e4b->bd_bitmap;
	if (start == 0 && max >= ext4_last_grp_cluster(sb, e4b->bd_group))
		set_trimmed = true;
	start = max(e4b->bd_info->bb_first_free, start);
	count = 0;
	free_count = 0;

	while (start <= max) {
		start = mb_find_next_zero_bit(bitmap, max + 1, start);
		if (start > max)
			break;
		next = mb_find_next_bit(bitmap, max + 1, start);

		if ((next - start) >= minblocks) {
			int ret = ext4_trim_extent(sb, start, next - start, e4b);

			if (ret && ret != -EOPNOTSUPP)
				return count;
			count += next - start;
		}
		free_count += next - start;
		start = next + 1;

		if (ext4_trim_interrupted())
			return count;

		if (need_resched()) {
			ext4_unlock_group(sb, e4b->bd_group);
			cond_resched();
			ext4_lock_group(sb, e4b->bd_group);
		}

		if ((e4b->bd_info->bb_free - free_count) < minblocks)
			break;
	}

	if (set_trimmed)
		EXT4_MB_GRP_SET_TRIMMED(e4b->bd_info);

	return count;
}

/**
 * ext4_trim_all_free -- function to trim all free space in alloc. group
 * @sb:			super block for file system
 * @group:		group to be trimmed
 * @start:		first group block to examine
 * @max:		last group block to examine
 * @minblocks:		minimum extent block count
 *
 * ext4_trim_all_free walks through group's block bitmap searching for free
 * extents. When the free extent is found, mark it as used in group buddy
 * bitmap. Then issue a TRIM command on this extent and free the extent in
 * the group buddy bitmap.
 */
static ext4_grpblk_t
ext4_trim_all_free(struct super_block *sb, ext4_group_t group,
		   ext4_grpblk_t start, ext4_grpblk_t max,
		   ext4_grpblk_t minblocks)
{
	struct ext4_buddy e4b;
	int ret;

	trace_ext4_trim_all_free(sb, group, start, max);

	ret = ext4_mb_load_buddy(sb, group, &e4b);
	if (ret) {
		ext4_warning(sb, "Error %d loading buddy information for %u",
			     ret, group);
		return ret;
	}

	ext4_lock_group(sb, group);

	if (!EXT4_MB_GRP_WAS_TRIMMED(e4b.bd_info) ||
	    minblocks < EXT4_SB(sb)->s_last_trim_minblks)
		ret = ext4_try_to_trim_range(sb, &e4b, start, max, minblocks);
	else
		ret = 0;

	ext4_unlock_group(sb, group);
	ext4_mb_unload_buddy(&e4b);

	ext4_debug("trimmed %d blocks in the group %d\n",
		ret, group);

	return ret;
}

/**
 * ext4_trim_fs() -- trim ioctl handle function
 * @sb:			superblock for filesystem
 * @range:		fstrim_range structure
 *
 * start:	First Byte to trim
 * len:		number of Bytes to trim from start
 * minlen:	minimum extent length in Bytes
 * ext4_trim_fs goes through all allocation groups containing Bytes from
 * start to start+len. For each such a group ext4_trim_all_free function
 * is invoked to trim all free space.
 */
int ext4_trim_fs(struct super_block *sb, struct fstrim_range *range)
{
	unsigned int discard_granularity = bdev_discard_granularity(sb->s_bdev);
	struct ext4_group_info *grp;
	ext4_group_t group, first_group, last_group;
	ext4_grpblk_t cnt = 0, first_cluster, last_cluster;
	uint64_t start, end, minlen, trimmed = 0;
	ext4_fsblk_t first_data_blk =
			le32_to_cpu(EXT4_SB(sb)->s_es->s_first_data_block);
	ext4_fsblk_t max_blks = ext4_blocks_count(EXT4_SB(sb)->s_es);
	int ret = 0;

	start = range->start >> sb->s_blocksize_bits;
	end = start + (range->len >> sb->s_blocksize_bits) - 1;
	minlen = EXT4_NUM_B2C(EXT4_SB(sb),
			      range->minlen >> sb->s_blocksize_bits);

	if (minlen > EXT4_CLUSTERS_PER_GROUP(sb) ||
	    start >= max_blks ||
	    range->len < sb->s_blocksize)
		return -EINVAL;
	/* No point to try to trim less than discard granularity */
	if (range->minlen < discard_granularity) {
		minlen = EXT4_NUM_B2C(EXT4_SB(sb),
				discard_granularity >> sb->s_blocksize_bits);
		if (minlen > EXT4_CLUSTERS_PER_GROUP(sb))
			goto out;
	}
	if (end >= max_blks - 1)
		end = max_blks - 1;
	if (end <= first_data_blk)
		goto out;
	if (start < first_data_blk)
		start = first_data_blk;

	/* Determine first and last group to examine based on start and end */
	ext4_get_group_no_and_offset(sb, (ext4_fsblk_t) start,
				     &first_group, &first_cluster);
	ext4_get_group_no_and_offset(sb, (ext4_fsblk_t) end,
				     &last_group, &last_cluster);

	/* end now represents the last cluster to discard in this group */
	end = EXT4_CLUSTERS_PER_GROUP(sb) - 1;

	for (group = first_group; group <= last_group; group++) {
		if (ext4_trim_interrupted())
			break;
		grp = ext4_get_group_info(sb, group);
		if (!grp)
			continue;
		/* We only do this if the grp has never been initialized */
		if (unlikely(EXT4_MB_GRP_NEED_INIT(grp))) {
			ret = ext4_mb_init_group(sb, group, GFP_NOFS);
			if (ret)
				break;
		}

		/*
		 * For all the groups except the last one, last cluster will
		 * always be EXT4_CLUSTERS_PER_GROUP(sb)-1, so we only need to
		 * change it for the last group, note that last_cluster is
		 * already computed earlier by ext4_get_group_no_and_offset()
		 */
		if (group == last_group)
			end = last_cluster;
		if (grp->bb_free >= minlen) {
			cnt = ext4_trim_all_free(sb, group, first_cluster,
						 end, minlen);
			if (cnt < 0) {
				ret = cnt;
				break;
			}
			trimmed += cnt;
		}

		/*
		 * For every group except the first one, we are sure
		 * that the first cluster to discard will be cluster #0.
		 */
		first_cluster = 0;
	}

	if (!ret)
		EXT4_SB(sb)->s_last_trim_minblks = minlen;

out:
	range->len = EXT4_C2B(EXT4_SB(sb), trimmed) << sb->s_blocksize_bits;
	return ret;
}

/* Iterate all the free extents in the group. */
int
ext4_mballoc_query_range(
	struct super_block		*sb,
	ext4_group_t			group,
	ext4_grpblk_t			start,
	ext4_grpblk_t			end,
	ext4_mballoc_query_range_fn	formatter,
	void				*priv)
{
	void				*bitmap;
	ext4_grpblk_t			next;
	struct ext4_buddy		e4b;
	int				error;

	error = ext4_mb_load_buddy(sb, group, &e4b);
	if (error)
		return error;
	bitmap = e4b.bd_bitmap;

	ext4_lock_group(sb, group);

	start = max(e4b.bd_info->bb_first_free, start);
	if (end >= EXT4_CLUSTERS_PER_GROUP(sb))
		end = EXT4_CLUSTERS_PER_GROUP(sb) - 1;

	while (start <= end) {
		start = mb_find_next_zero_bit(bitmap, end + 1, start);
		if (start > end)
			break;
		next = mb_find_next_bit(bitmap, end + 1, start);

		ext4_unlock_group(sb, group);
		error = formatter(sb, group, start, next - start, priv);
		if (error)
			goto out_unload;
		ext4_lock_group(sb, group);

		start = next + 1;
	}

	ext4_unlock_group(sb, group);
out_unload:
	ext4_mb_unload_buddy(&e4b);

	return error;
}
