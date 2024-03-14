// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 */

#include <linux/sched.h>
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "locking.h"
#include "accessors.h"
#include "messages.h"
#include "delalloc-space.h"
#include "subpage.h"
#include "defrag.h"
#include "file-item.h"
#include "super.h"

static struct kmem_cache *btrfs_inode_defrag_cachep;

/*
 * When auto defrag is enabled we queue up these defrag structs to remember
 * which inodes need defragging passes.
 */
struct inode_defrag {
	struct rb_node rb_node;
	/* Inode number */
	u64 ino;
	/*
	 * Transid where the defrag was added, we search for extents newer than
	 * this.
	 */
	u64 transid;

	/* Root objectid */
	u64 root;

	/*
	 * The extent size threshold for autodefrag.
	 *
	 * This value is different for compressed/non-compressed extents, thus
	 * needs to be passed from higher layer.
	 * (aka, inode_should_defrag())
	 */
	u32 extent_thresh;
};

static int __compare_inode_defrag(struct inode_defrag *defrag1,
				  struct inode_defrag *defrag2)
{
	if (defrag1->root > defrag2->root)
		return 1;
	else if (defrag1->root < defrag2->root)
		return -1;
	else if (defrag1->ino > defrag2->ino)
		return 1;
	else if (defrag1->ino < defrag2->ino)
		return -1;
	else
		return 0;
}

/*
 * Pop a record for an inode into the defrag tree.  The lock must be held
 * already.
 *
 * If you're inserting a record for an older transid than an existing record,
 * the transid already in the tree is lowered.
 *
 * If an existing record is found the defrag item you pass in is freed.
 */
static int __btrfs_add_inode_defrag(struct btrfs_inode *inode,
				    struct inode_defrag *defrag)
{
	struct btrfs_fs_info *fs_info = inode->root->fs_info;
	struct inode_defrag *entry;
	struct rb_node **p;
	struct rb_node *parent = NULL;
	int ret;

	p = &fs_info->defrag_inodes.rb_node;
	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct inode_defrag, rb_node);

		ret = __compare_inode_defrag(defrag, entry);
		if (ret < 0)
			p = &parent->rb_left;
		else if (ret > 0)
			p = &parent->rb_right;
		else {
			/*
			 * If we're reinserting an entry for an old defrag run,
			 * make sure to lower the transid of our existing
			 * record.
			 */
			if (defrag->transid < entry->transid)
				entry->transid = defrag->transid;
			entry->extent_thresh = min(defrag->extent_thresh,
						   entry->extent_thresh);
			return -EEXIST;
		}
	}
	set_bit(BTRFS_INODE_IN_DEFRAG, &inode->runtime_flags);
	rb_link_node(&defrag->rb_node, parent, p);
	rb_insert_color(&defrag->rb_node, &fs_info->defrag_inodes);
	return 0;
}

static inline int __need_auto_defrag(struct btrfs_fs_info *fs_info)
{
	if (!btrfs_test_opt(fs_info, AUTO_DEFRAG))
		return 0;

	if (btrfs_fs_closing(fs_info))
		return 0;

	return 1;
}

/*
 * Insert a defrag record for this inode if auto defrag is enabled.
 */
int btrfs_add_inode_defrag(struct btrfs_trans_handle *trans,
			   struct btrfs_inode *inode, u32 extent_thresh)
{
	struct btrfs_root *root = inode->root;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct inode_defrag *defrag;
	u64 transid;
	int ret;

	if (!__need_auto_defrag(fs_info))
		return 0;

	if (test_bit(BTRFS_INODE_IN_DEFRAG, &inode->runtime_flags))
		return 0;

	if (trans)
		transid = trans->transid;
	else
		transid = inode->root->last_trans;

	defrag = kmem_cache_zalloc(btrfs_inode_defrag_cachep, GFP_NOFS);
	if (!defrag)
		return -ENOMEM;

	defrag->ino = btrfs_ino(inode);
	defrag->transid = transid;
	defrag->root = root->root_key.objectid;
	defrag->extent_thresh = extent_thresh;

	spin_lock(&fs_info->defrag_inodes_lock);
	if (!test_bit(BTRFS_INODE_IN_DEFRAG, &inode->runtime_flags)) {
		/*
		 * If we set IN_DEFRAG flag and evict the inode from memory,
		 * and then re-read this inode, this new inode doesn't have
		 * IN_DEFRAG flag. At the case, we may find the existed defrag.
		 */
		ret = __btrfs_add_inode_defrag(inode, defrag);
		if (ret)
			kmem_cache_free(btrfs_inode_defrag_cachep, defrag);
	} else {
		kmem_cache_free(btrfs_inode_defrag_cachep, defrag);
	}
	spin_unlock(&fs_info->defrag_inodes_lock);
	return 0;
}

/*
 * Pick the defragable inode that we want, if it doesn't exist, we will get the
 * next one.
 */
static struct inode_defrag *btrfs_pick_defrag_inode(
			struct btrfs_fs_info *fs_info, u64 root, u64 ino)
{
	struct inode_defrag *entry = NULL;
	struct inode_defrag tmp;
	struct rb_node *p;
	struct rb_node *parent = NULL;
	int ret;

	tmp.ino = ino;
	tmp.root = root;

	spin_lock(&fs_info->defrag_inodes_lock);
	p = fs_info->defrag_inodes.rb_node;
	while (p) {
		parent = p;
		entry = rb_entry(parent, struct inode_defrag, rb_node);

		ret = __compare_inode_defrag(&tmp, entry);
		if (ret < 0)
			p = parent->rb_left;
		else if (ret > 0)
			p = parent->rb_right;
		else
			goto out;
	}

	if (parent && __compare_inode_defrag(&tmp, entry) > 0) {
		parent = rb_next(parent);
		if (parent)
			entry = rb_entry(parent, struct inode_defrag, rb_node);
		else
			entry = NULL;
	}
out:
	if (entry)
		rb_erase(parent, &fs_info->defrag_inodes);
	spin_unlock(&fs_info->defrag_inodes_lock);
	return entry;
}

void btrfs_cleanup_defrag_inodes(struct btrfs_fs_info *fs_info)
{
	struct inode_defrag *defrag;
	struct rb_node *node;

	spin_lock(&fs_info->defrag_inodes_lock);
	node = rb_first(&fs_info->defrag_inodes);
	while (node) {
		rb_erase(node, &fs_info->defrag_inodes);
		defrag = rb_entry(node, struct inode_defrag, rb_node);
		kmem_cache_free(btrfs_inode_defrag_cachep, defrag);

		cond_resched_lock(&fs_info->defrag_inodes_lock);

		node = rb_first(&fs_info->defrag_inodes);
	}
	spin_unlock(&fs_info->defrag_inodes_lock);
}

#define BTRFS_DEFRAG_BATCH	1024

static int __btrfs_run_defrag_inode(struct btrfs_fs_info *fs_info,
				    struct inode_defrag *defrag)
{
	struct btrfs_root *inode_root;
	struct inode *inode;
	struct btrfs_ioctl_defrag_range_args range;
	int ret = 0;
	u64 cur = 0;

again:
	if (test_bit(BTRFS_FS_STATE_REMOUNTING, &fs_info->fs_state))
		goto cleanup;
	if (!__need_auto_defrag(fs_info))
		goto cleanup;

	/* Get the inode */
	inode_root = btrfs_get_fs_root(fs_info, defrag->root, true);
	if (IS_ERR(inode_root)) {
		ret = PTR_ERR(inode_root);
		goto cleanup;
	}

	inode = btrfs_iget(fs_info->sb, defrag->ino, inode_root);
	btrfs_put_root(inode_root);
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
		goto cleanup;
	}

	if (cur >= i_size_read(inode)) {
		iput(inode);
		goto cleanup;
	}

	/* Do a chunk of defrag */
	clear_bit(BTRFS_INODE_IN_DEFRAG, &BTRFS_I(inode)->runtime_flags);
	memset(&range, 0, sizeof(range));
	range.len = (u64)-1;
	range.start = cur;
	range.extent_thresh = defrag->extent_thresh;

	sb_start_write(fs_info->sb);
	ret = btrfs_defrag_file(inode, NULL, &range, defrag->transid,
				       BTRFS_DEFRAG_BATCH);
	sb_end_write(fs_info->sb);
	iput(inode);

	if (ret < 0)
		goto cleanup;

	cur = max(cur + fs_info->sectorsize, range.start);
	goto again;

cleanup:
	kmem_cache_free(btrfs_inode_defrag_cachep, defrag);
	return ret;
}

/*
 * Run through the list of inodes in the FS that need defragging.
 */
int btrfs_run_defrag_inodes(struct btrfs_fs_info *fs_info)
{
	struct inode_defrag *defrag;
	u64 first_ino = 0;
	u64 root_objectid = 0;

	atomic_inc(&fs_info->defrag_running);
	while (1) {
		/* Pause the auto defragger. */
		if (test_bit(BTRFS_FS_STATE_REMOUNTING, &fs_info->fs_state))
			break;

		if (!__need_auto_defrag(fs_info))
			break;

		/* find an inode to defrag */
		defrag = btrfs_pick_defrag_inode(fs_info, root_objectid, first_ino);
		if (!defrag) {
			if (root_objectid || first_ino) {
				root_objectid = 0;
				first_ino = 0;
				continue;
			} else {
				break;
			}
		}

		first_ino = defrag->ino + 1;
		root_objectid = defrag->root;

		__btrfs_run_defrag_inode(fs_info, defrag);
	}
	atomic_dec(&fs_info->defrag_running);

	/*
	 * During unmount, we use the transaction_wait queue to wait for the
	 * defragger to stop.
	 */
	wake_up(&fs_info->transaction_wait);
	return 0;
}

/*
 * Defrag all the leaves in a given btree.
 * Read all the leaves and try to get key order to
 * better reflect disk order
 */

int btrfs_defrag_leaves(struct btrfs_trans_handle *trans,
			struct btrfs_root *root)
{
	struct btrfs_path *path = NULL;
	struct btrfs_key key;
	int ret = 0;
	int wret;
	int level;
	int next_key_ret = 0;
	u64 last_ret = 0;

	if (!test_bit(BTRFS_ROOT_SHAREABLE, &root->state))
		goto out;

	path = btrfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	level = btrfs_header_level(root->node);

	if (level == 0)
		goto out;

	if (root->defrag_progress.objectid == 0) {
		struct extent_buffer *root_node;
		u32 nritems;

		root_node = btrfs_lock_root_node(root);
		nritems = btrfs_header_nritems(root_node);
		root->defrag_max.objectid = 0;
		/* from above we know this is not a leaf */
		btrfs_node_key_to_cpu(root_node, &root->defrag_max,
				      nritems - 1);
		btrfs_tree_unlock(root_node);
		free_extent_buffer(root_node);
		memset(&key, 0, sizeof(key));
	} else {
		memcpy(&key, &root->defrag_progress, sizeof(key));
	}

	path->keep_locks = 1;

	ret = btrfs_search_forward(root, &key, path, BTRFS_OLDEST_GENERATION);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = 0;
		goto out;
	}
	btrfs_release_path(path);
	/*
	 * We don't need a lock on a leaf. btrfs_realloc_node() will lock all
	 * leafs from path->nodes[1], so set lowest_level to 1 to avoid later
	 * a deadlock (attempting to write lock an already write locked leaf).
	 */
	path->lowest_level = 1;
	wret = btrfs_search_slot(trans, root, &key, path, 0, 1);

	if (wret < 0) {
		ret = wret;
		goto out;
	}
	if (!path->nodes[1]) {
		ret = 0;
		goto out;
	}
	/*
	 * The node at level 1 must always be locked when our path has
	 * keep_locks set and lowest_level is 1, regardless of the value of
	 * path->slots[1].
	 */
	BUG_ON(path->locks[1] == 0);
	ret = btrfs_realloc_node(trans, root,
				 path->nodes[1], 0,
				 &last_ret,
				 &root->defrag_progress);
	if (ret) {
		WARN_ON(ret == -EAGAIN);
		goto out;
	}
	/*
	 * Now that we reallocated the node we can find the next key. Note that
	 * btrfs_find_next_key() can release our path and do another search
	 * without COWing, this is because even with path->keep_locks = 1,
	 * btrfs_search_slot() / ctree.c:unlock_up() does not keeps a lock on a
	 * node when path->slots[node_level - 1] does not point to the last
	 * item or a slot beyond the last item (ctree.c:unlock_up()). Therefore
	 * we search for the next key after reallocating our node.
	 */
	path->slots[1] = btrfs_header_nritems(path->nodes[1]);
	next_key_ret = btrfs_find_next_key(root, path, &key, 1,
					   BTRFS_OLDEST_GENERATION);
	if (next_key_ret == 0) {
		memcpy(&root->defrag_progress, &key, sizeof(key));
		ret = -EAGAIN;
	}
out:
	btrfs_free_path(path);
	if (ret == -EAGAIN) {
		if (root->defrag_max.objectid > root->defrag_progress.objectid)
			goto done;
		if (root->defrag_max.type > root->defrag_progress.type)
			goto done;
		if (root->defrag_max.offset > root->defrag_progress.offset)
			goto done;
		ret = 0;
	}
done:
	if (ret != -EAGAIN)
		memset(&root->defrag_progress, 0,
		       sizeof(root->defrag_progress));

	return ret;
}

/*
 * Defrag specific helper to get an extent map.
 *
 * Differences between this and btrfs_get_extent() are:
 *
 * - No extent_map will be added to inode->extent_tree
 *   To reduce memory usage in the long run.
 *
 * - Extra optimization to skip file extents older than @newer_than
 *   By using btrfs_search_forward() we can skip entire file ranges that
 *   have extents created in past transactions, because btrfs_search_forward()
 *   will not visit leaves and nodes with a generation smaller than given
 *   minimal generation threshold (@newer_than).
 *
 * Return valid em if we find a file extent matching the requirement.
 * Return NULL if we can not find a file extent matching the requirement.
 *
 * Return ERR_PTR() for error.
 */
static struct extent_map *defrag_get_extent(struct btrfs_inode *inode,
					    u64 start, u64 newer_than)
{
	struct btrfs_root *root = inode->root;
	struct btrfs_file_extent_item *fi;
	struct btrfs_path path = { 0 };
	struct extent_map *em;
	struct btrfs_key key;
	u64 ino = btrfs_ino(inode);
	int ret;

	em = alloc_extent_map();
	if (!em) {
		ret = -ENOMEM;
		goto err;
	}

	key.objectid = ino;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = start;

	if (newer_than) {
		ret = btrfs_search_forward(root, &key, &path, newer_than);
		if (ret < 0)
			goto err;
		/* Can't find anything newer */
		if (ret > 0)
			goto not_found;
	} else {
		ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
		if (ret < 0)
			goto err;
	}
	if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
		/*
		 * If btrfs_search_slot() makes path to point beyond nritems,
		 * we should not have an empty leaf, as this inode must at
		 * least have its INODE_ITEM.
		 */
		ASSERT(btrfs_header_nritems(path.nodes[0]));
		path.slots[0] = btrfs_header_nritems(path.nodes[0]) - 1;
	}
	btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
	/* Perfect match, no need to go one slot back */
	if (key.objectid == ino && key.type == BTRFS_EXTENT_DATA_KEY &&
	    key.offset == start)
		goto iterate;

	/* We didn't find a perfect match, needs to go one slot back */
	if (path.slots[0] > 0) {
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.objectid == ino && key.type == BTRFS_EXTENT_DATA_KEY)
			path.slots[0]--;
	}

iterate:
	/* Iterate through the path to find a file extent covering @start */
	while (true) {
		u64 extent_end;

		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0]))
			goto next;

		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);

		/*
		 * We may go one slot back to INODE_REF/XATTR item, then
		 * need to go forward until we reach an EXTENT_DATA.
		 * But we should still has the correct ino as key.objectid.
		 */
		if (WARN_ON(key.objectid < ino) || key.type < BTRFS_EXTENT_DATA_KEY)
			goto next;

		/* It's beyond our target range, definitely not extent found */
		if (key.objectid > ino || key.type > BTRFS_EXTENT_DATA_KEY)
			goto not_found;

		/*
		 *	|	|<- File extent ->|
		 *	\- start
		 *
		 * This means there is a hole between start and key.offset.
		 */
		if (key.offset > start) {
			em->start = start;
			em->orig_start = start;
			em->block_start = EXTENT_MAP_HOLE;
			em->len = key.offset - start;
			break;
		}

		fi = btrfs_item_ptr(path.nodes[0], path.slots[0],
				    struct btrfs_file_extent_item);
		extent_end = btrfs_file_extent_end(&path);

		/*
		 *	|<- file extent ->|	|
		 *				\- start
		 *
		 * We haven't reached start, search next slot.
		 */
		if (extent_end <= start)
			goto next;

		/* Now this extent covers @start, convert it to em */
		btrfs_extent_item_to_extent_map(inode, &path, fi, em);
		break;
next:
		ret = btrfs_next_item(root, &path);
		if (ret < 0)
			goto err;
		if (ret > 0)
			goto not_found;
	}
	btrfs_release_path(&path);
	return em;

not_found:
	btrfs_release_path(&path);
	free_extent_map(em);
	return NULL;

err:
	btrfs_release_path(&path);
	free_extent_map(em);
	return ERR_PTR(ret);
}

static struct extent_map *defrag_lookup_extent(struct inode *inode, u64 start,
					       u64 newer_than, bool locked)
{
	struct extent_map_tree *em_tree = &BTRFS_I(inode)->extent_tree;
	struct extent_io_tree *io_tree = &BTRFS_I(inode)->io_tree;
	struct extent_map *em;
	const u32 sectorsize = BTRFS_I(inode)->root->fs_info->sectorsize;

	/*
	 * Hopefully we have this extent in the tree already, try without the
	 * full extent lock.
	 */
	read_lock(&em_tree->lock);
	em = lookup_extent_mapping(em_tree, start, sectorsize);
	read_unlock(&em_tree->lock);

	/*
	 * We can get a merged extent, in that case, we need to re-search
	 * tree to get the original em for defrag.
	 *
	 * If @newer_than is 0 or em::generation < newer_than, we can trust
	 * this em, as either we don't care about the generation, or the
	 * merged extent map will be rejected anyway.
	 */
	if (em && test_bit(EXTENT_FLAG_MERGED, &em->flags) &&
	    newer_than && em->generation >= newer_than) {
		free_extent_map(em);
		em = NULL;
	}

	if (!em) {
		struct extent_state *cached = NULL;
		u64 end = start + sectorsize - 1;

		/* Get the big lock and read metadata off disk. */
		if (!locked)
			lock_extent(io_tree, start, end, &cached);
		em = defrag_get_extent(BTRFS_I(inode), start, newer_than);
		if (!locked)
			unlock_extent(io_tree, start, end, &cached);

		if (IS_ERR(em))
			return NULL;
	}

	return em;
}

static u32 get_extent_max_capacity(const struct btrfs_fs_info *fs_info,
				   const struct extent_map *em)
{
	if (test_bit(EXTENT_FLAG_COMPRESSED, &em->flags))
		return BTRFS_MAX_COMPRESSED;
	return fs_info->max_extent_size;
}

static bool defrag_check_next_extent(struct inode *inode, struct extent_map *em,
				     u32 extent_thresh, u64 newer_than, bool locked)
{
	struct btrfs_fs_info *fs_info = btrfs_sb(inode->i_sb);
	struct extent_map *next;
	bool ret = false;

	/* This is the last extent */
	if (em->start + em->len >= i_size_read(inode))
		return false;

	/*
	 * Here we need to pass @newer_then when checking the next extent, or
	 * we will hit a case we mark current extent for defrag, but the next
	 * one will not be a target.
	 * This will just cause extra IO without really reducing the fragments.
	 */
	next = defrag_lookup_extent(inode, em->start + em->len, newer_than, locked);
	/* No more em or hole */
	if (!next || next->block_start >= EXTENT_MAP_LAST_BYTE)
		goto out;
	if (test_bit(EXTENT_FLAG_PREALLOC, &next->flags))
		goto out;
	/*
	 * If the next extent is at its max capacity, defragging current extent
	 * makes no sense, as the total number of extents won't change.
	 */
	if (next->len >= get_extent_max_capacity(fs_info, em))
		goto out;
	/* Skip older extent */
	if (next->generation < newer_than)
		goto out;
	/* Also check extent size */
	if (next->len >= extent_thresh)
		goto out;

	ret = true;
out:
	free_extent_map(next);
	return ret;
}

/*
 * Prepare one page to be defragged.
 *
 * This will ensure:
 *
 * - Returned page is locked and has been set up properly.
 * - No ordered extent exists in the page.
 * - The page is uptodate.
 *
 * NOTE: Caller should also wait for page writeback after the cluster is
 * prepared, here we don't do writeback wait for each page.
 */
static struct page *defrag_prepare_one_page(struct btrfs_inode *inode, pgoff_t index)
{
	struct address_space *mapping = inode->vfs_inode.i_mapping;
	gfp_t mask = btrfs_alloc_write_mask(mapping);
	u64 page_start = (u64)index << PAGE_SHIFT;
	u64 page_end = page_start + PAGE_SIZE - 1;
	struct extent_state *cached_state = NULL;
	struct page *page;
	int ret;

again:
	page = find_or_create_page(mapping, index, mask);
	if (!page)
		return ERR_PTR(-ENOMEM);

	/*
	 * Since we can defragment files opened read-only, we can encounter
	 * transparent huge pages here (see CONFIG_READ_ONLY_THP_FOR_FS). We
	 * can't do I/O using huge pages yet, so return an error for now.
	 * Filesystem transparent huge pages are typically only used for
	 * executables that explicitly enable them, so this isn't very
	 * restrictive.
	 */
	if (PageCompound(page)) {
		unlock_page(page);
		put_page(page);
		return ERR_PTR(-ETXTBSY);
	}

	ret = set_page_extent_mapped(page);
	if (ret < 0) {
		unlock_page(page);
		put_page(page);
		return ERR_PTR(ret);
	}

	/* Wait for any existing ordered extent in the range */
	while (1) {
		struct btrfs_ordered_extent *ordered;

		lock_extent(&inode->io_tree, page_start, page_end, &cached_state);
		ordered = btrfs_lookup_ordered_range(inode, page_start, PAGE_SIZE);
		unlock_extent(&inode->io_tree, page_start, page_end,
			      &cached_state);
		if (!ordered)
			break;

		unlock_page(page);
		btrfs_start_ordered_extent(ordered);
		btrfs_put_ordered_extent(ordered);
		lock_page(page);
		/*
		 * We unlocked the page above, so we need check if it was
		 * released or not.
		 */
		if (page->mapping != mapping || !PagePrivate(page)) {
			unlock_page(page);
			put_page(page);
			goto again;
		}
	}

	/*
	 * Now the page range has no ordered extent any more.  Read the page to
	 * make it uptodate.
	 */
	if (!PageUptodate(page)) {
		btrfs_read_folio(NULL, page_folio(page));
		lock_page(page);
		if (page->mapping != mapping || !PagePrivate(page)) {
			unlock_page(page);
			put_page(page);
			goto again;
		}
		if (!PageUptodate(page)) {
			unlock_page(page);
			put_page(page);
			return ERR_PTR(-EIO);
		}
	}
	return page;
}

struct defrag_target_range {
	struct list_head list;
	u64 start;
	u64 len;
};

/*
 * Collect all valid target extents.
 *
 * @start:	   file offset to lookup
 * @len:	   length to lookup
 * @extent_thresh: file extent size threshold, any extent size >= this value
 *		   will be ignored
 * @newer_than:    only defrag extents newer than this value
 * @do_compress:   whether the defrag is doing compression
 *		   if true, @extent_thresh will be ignored and all regular
 *		   file extents meeting @newer_than will be targets.
 * @locked:	   if the range has already held extent lock
 * @target_list:   list of targets file extents
 */
static int defrag_collect_targets(struct btrfs_inode *inode,
				  u64 start, u64 len, u32 extent_thresh,
				  u64 newer_than, bool do_compress,
				  bool locked, struct list_head *target_list,
				  u64 *last_scanned_ret)
{
	struct btrfs_fs_info *fs_info = inode->root->fs_info;
	bool last_is_target = false;
	u64 cur = start;
	int ret = 0;

	while (cur < start + len) {
		struct extent_map *em;
		struct defrag_target_range *new;
		bool next_mergeable = true;
		u64 range_len;

		last_is_target = false;
		em = defrag_lookup_extent(&inode->vfs_inode, cur, newer_than, locked);
		if (!em)
			break;

		/*
		 * If the file extent is an inlined one, we may still want to
		 * defrag it (fallthrough) if it will cause a regular extent.
		 * This is for users who want to convert inline extents to
		 * regular ones through max_inline= mount option.
		 */
		if (em->block_start == EXTENT_MAP_INLINE &&
		    em->len <= inode->root->fs_info->max_inline)
			goto next;

		/* Skip hole/delalloc/preallocated extents */
		if (em->block_start == EXTENT_MAP_HOLE ||
		    em->block_start == EXTENT_MAP_DELALLOC ||
		    test_bit(EXTENT_FLAG_PREALLOC, &em->flags))
			goto next;

		/* Skip older extent */
		if (em->generation < newer_than)
			goto next;

		/* This em is under writeback, no need to defrag */
		if (em->generation == (u64)-1)
			goto next;

		/*
		 * Our start offset might be in the middle of an existing extent
		 * map, so take that into account.
		 */
		range_len = em->len - (cur - em->start);
		/*
		 * If this range of the extent map is already flagged for delalloc,
		 * skip it, because:
		 *
		 * 1) We could deadlock later, when trying to reserve space for
		 *    delalloc, because in case we can't immediately reserve space
		 *    the flusher can start delalloc and wait for the respective
		 *    ordered extents to complete. The deadlock would happen
		 *    because we do the space reservation while holding the range
		 *    locked, and starting writeback, or finishing an ordered
		 *    extent, requires locking the range;
		 *
		 * 2) If there's delalloc there, it means there's dirty pages for
		 *    which writeback has not started yet (we clean the delalloc
		 *    flag when starting writeback and after creating an ordered
		 *    extent). If we mark pages in an adjacent range for defrag,
		 *    then we will have a larger contiguous range for delalloc,
		 *    very likely resulting in a larger extent after writeback is
		 *    triggered (except in a case of free space fragmentation).
		 */
		if (test_range_bit(&inode->io_tree, cur, cur + range_len - 1,
				   EXTENT_DELALLOC, 0, NULL))
			goto next;

		/*
		 * For do_compress case, we want to compress all valid file
		 * extents, thus no @extent_thresh or mergeable check.
		 */
		if (do_compress)
			goto add;

		/* Skip too large extent */
		if (range_len >= extent_thresh)
			goto next;

		/*
		 * Skip extents already at its max capacity, this is mostly for
		 * compressed extents, which max cap is only 128K.
		 */
		if (em->len >= get_extent_max_capacity(fs_info, em))
			goto next;

		/*
		 * Normally there are no more extents after an inline one, thus
		 * @next_mergeable will normally be false and not defragged.
		 * So if an inline extent passed all above checks, just add it
		 * for defrag, and be converted to regular extents.
		 */
		if (em->block_start == EXTENT_MAP_INLINE)
			goto add;

		next_mergeable = defrag_check_next_extent(&inode->vfs_inode, em,
						extent_thresh, newer_than, locked);
		if (!next_mergeable) {
			struct defrag_target_range *last;

			/* Empty target list, no way to merge with last entry */
			if (list_empty(target_list))
				goto next;
			last = list_entry(target_list->prev,
					  struct defrag_target_range, list);
			/* Not mergeable with last entry */
			if (last->start + last->len != cur)
				goto next;

			/* Mergeable, fall through to add it to @target_list. */
		}

add:
		last_is_target = true;
		range_len = min(extent_map_end(em), start + len) - cur;
		/*
		 * This one is a good target, check if it can be merged into
		 * last range of the target list.
		 */
		if (!list_empty(target_list)) {
			struct defrag_target_range *last;

			last = list_entry(target_list->prev,
					  struct defrag_target_range, list);
			ASSERT(last->start + last->len <= cur);
			if (last->start + last->len == cur) {
				/* Mergeable, enlarge the last entry */
				last->len += range_len;
				goto next;
			}
			/* Fall through to allocate a new entry */
		}

		/* Allocate new defrag_target_range */
		new = kmalloc(sizeof(*new), GFP_NOFS);
		if (!new) {
			free_extent_map(em);
			ret = -ENOMEM;
			break;
		}
		new->start = cur;
		new->len = range_len;
		list_add_tail(&new->list, target_list);

next:
		cur = extent_map_end(em);
		free_extent_map(em);
	}
	if (ret < 0) {
		struct defrag_target_range *entry;
		struct defrag_target_range *tmp;

		list_for_each_entry_safe(entry, tmp, target_list, list) {
			list_del_init(&entry->list);
			kfree(entry);
		}
	}
	if (!ret && last_scanned_ret) {
		/*
		 * If the last extent is not a target, the caller can skip to
		 * the end of that extent.
		 * Otherwise, we can only go the end of the specified range.
		 */
		if (!last_is_target)
			*last_scanned_ret = max(cur, *last_scanned_ret);
		else
			*last_scanned_ret = max(start + len, *last_scanned_ret);
	}
	return ret;
}

#define CLUSTER_SIZE	(SZ_256K)
static_assert(PAGE_ALIGNED(CLUSTER_SIZE));

/*
 * Defrag one contiguous target range.
 *
 * @inode:	target inode
 * @target:	target range to defrag
 * @pages:	locked pages covering the defrag range
 * @nr_pages:	number of locked pages
 *
 * Caller should ensure:
 *
 * - Pages are prepared
 *   Pages should be locked, no ordered extent in the pages range,
 *   no writeback.
 *
 * - Extent bits are locked
 */
static int defrag_one_locked_target(struct btrfs_inode *inode,
				    struct defrag_target_range *target,
				    struct page **pages, int nr_pages,
				    struct extent_state **cached_state)
{
	struct btrfs_fs_info *fs_info = inode->root->fs_info;
	struct extent_changeset *data_reserved = NULL;
	const u64 start = target->start;
	const u64 len = target->len;
	unsigned long last_index = (start + len - 1) >> PAGE_SHIFT;
	unsigned long start_index = start >> PAGE_SHIFT;
	unsigned long first_index = page_index(pages[0]);
	int ret = 0;
	int i;

	ASSERT(last_index - first_index + 1 <= nr_pages);

	ret = btrfs_delalloc_reserve_space(inode, &data_reserved, start, len);
	if (ret < 0)
		return ret;
	clear_extent_bit(&inode->io_tree, start, start + len - 1,
			 EXTENT_DELALLOC | EXTENT_DO_ACCOUNTING |
			 EXTENT_DEFRAG, cached_state);
	set_extent_bit(&inode->io_tree, start, start + len - 1,
		       EXTENT_DELALLOC | EXTENT_DEFRAG, cached_state);

	/* Update the page status */
	for (i = start_index - first_index; i <= last_index - first_index; i++) {
		ClearPageChecked(pages[i]);
		btrfs_page_clamp_set_dirty(fs_info, pages[i], start, len);
	}
	btrfs_delalloc_release_extents(inode, len);
	extent_changeset_free(data_reserved);

	return ret;
}

static int defrag_one_range(struct btrfs_inode *inode, u64 start, u32 len,
			    u32 extent_thresh, u64 newer_than, bool do_compress,
			    u64 *last_scanned_ret)
{
	struct extent_state *cached_state = NULL;
	struct defrag_target_range *entry;
	struct defrag_target_range *tmp;
	LIST_HEAD(target_list);
	struct page **pages;
	const u32 sectorsize = inode->root->fs_info->sectorsize;
	u64 last_index = (start + len - 1) >> PAGE_SHIFT;
	u64 start_index = start >> PAGE_SHIFT;
	unsigned int nr_pages = last_index - start_index + 1;
	int ret = 0;
	int i;

	ASSERT(nr_pages <= CLUSTER_SIZE / PAGE_SIZE);
	ASSERT(IS_ALIGNED(start, sectorsize) && IS_ALIGNED(len, sectorsize));

	pages = kcalloc(nr_pages, sizeof(struct page *), GFP_NOFS);
	if (!pages)
		return -ENOMEM;

	/* Prepare all pages */
	for (i = 0; i < nr_pages; i++) {
		pages[i] = defrag_prepare_one_page(inode, start_index + i);
		if (IS_ERR(pages[i])) {
			ret = PTR_ERR(pages[i]);
			pages[i] = NULL;
			goto free_pages;
		}
	}
	for (i = 0; i < nr_pages; i++)
		wait_on_page_writeback(pages[i]);

	/* Lock the pages range */
	lock_extent(&inode->io_tree, start_index << PAGE_SHIFT,
		    (last_index << PAGE_SHIFT) + PAGE_SIZE - 1,
		    &cached_state);
	/*
	 * Now we have a consistent view about the extent map, re-check
	 * which range really needs to be defragged.
	 *
	 * And this time we have extent locked already, pass @locked = true
	 * so that we won't relock the extent range and cause deadlock.
	 */
	ret = defrag_collect_targets(inode, start, len, extent_thresh,
				     newer_than, do_compress, true,
				     &target_list, last_scanned_ret);
	if (ret < 0)
		goto unlock_extent;

	list_for_each_entry(entry, &target_list, list) {
		ret = defrag_one_locked_target(inode, entry, pages, nr_pages,
					       &cached_state);
		if (ret < 0)
			break;
	}

	list_for_each_entry_safe(entry, tmp, &target_list, list) {
		list_del_init(&entry->list);
		kfree(entry);
	}
unlock_extent:
	unlock_extent(&inode->io_tree, start_index << PAGE_SHIFT,
		      (last_index << PAGE_SHIFT) + PAGE_SIZE - 1,
		      &cached_state);
free_pages:
	for (i = 0; i < nr_pages; i++) {
		if (pages[i]) {
			unlock_page(pages[i]);
			put_page(pages[i]);
		}
	}
	kfree(pages);
	return ret;
}

static int defrag_one_cluster(struct btrfs_inode *inode,
			      struct file_ra_state *ra,
			      u64 start, u32 len, u32 extent_thresh,
			      u64 newer_than, bool do_compress,
			      unsigned long *sectors_defragged,
			      unsigned long max_sectors,
			      u64 *last_scanned_ret)
{
	const u32 sectorsize = inode->root->fs_info->sectorsize;
	struct defrag_target_range *entry;
	struct defrag_target_range *tmp;
	LIST_HEAD(target_list);
	int ret;

	ret = defrag_collect_targets(inode, start, len, extent_thresh,
				     newer_than, do_compress, false,
				     &target_list, NULL);
	if (ret < 0)
		goto out;

	list_for_each_entry(entry, &target_list, list) {
		u32 range_len = entry->len;

		/* Reached or beyond the limit */
		if (max_sectors && *sectors_defragged >= max_sectors) {
			ret = 1;
			break;
		}

		if (max_sectors)
			range_len = min_t(u32, range_len,
				(max_sectors - *sectors_defragged) * sectorsize);

		/*
		 * If defrag_one_range() has updated last_scanned_ret,
		 * our range may already be invalid (e.g. hole punched).
		 * Skip if our range is before last_scanned_ret, as there is
		 * no need to defrag the range anymore.
		 */
		if (entry->start + range_len <= *last_scanned_ret)
			continue;

		if (ra)
			page_cache_sync_readahead(inode->vfs_inode.i_mapping,
				ra, NULL, entry->start >> PAGE_SHIFT,
				((entry->start + range_len - 1) >> PAGE_SHIFT) -
				(entry->start >> PAGE_SHIFT) + 1);
		/*
		 * Here we may not defrag any range if holes are punched before
		 * we locked the pages.
		 * But that's fine, it only affects the @sectors_defragged
		 * accounting.
		 */
		ret = defrag_one_range(inode, entry->start, range_len,
				       extent_thresh, newer_than, do_compress,
				       last_scanned_ret);
		if (ret < 0)
			break;
		*sectors_defragged += range_len >>
				      inode->root->fs_info->sectorsize_bits;
	}
out:
	list_for_each_entry_safe(entry, tmp, &target_list, list) {
		list_del_init(&entry->list);
		kfree(entry);
	}
	if (ret >= 0)
		*last_scanned_ret = max(*last_scanned_ret, start + len);
	return ret;
}

/*
 * Entry point to file defragmentation.
 *
 * @inode:	   inode to be defragged
 * @ra:		   readahead state (can be NUL)
 * @range:	   defrag options including range and flags
 * @newer_than:	   minimum transid to defrag
 * @max_to_defrag: max number of sectors to be defragged, if 0, the whole inode
 *		   will be defragged.
 *
 * Return <0 for error.
 * Return >=0 for the number of sectors defragged, and range->start will be updated
 * to indicate the file offset where next defrag should be started at.
 * (Mostly for autodefrag, which sets @max_to_defrag thus we may exit early without
 *  defragging all the range).
 */
int btrfs_defrag_file(struct inode *inode, struct file_ra_state *ra,
		      struct btrfs_ioctl_defrag_range_args *range,
		      u64 newer_than, unsigned long max_to_defrag)
{
	struct btrfs_fs_info *fs_info = btrfs_sb(inode->i_sb);
	unsigned long sectors_defragged = 0;
	u64 isize = i_size_read(inode);
	u64 cur;
	u64 last_byte;
	bool do_compress = (range->flags & BTRFS_DEFRAG_RANGE_COMPRESS);
	bool ra_allocated = false;
	int compress_type = BTRFS_COMPRESS_ZLIB;
	int ret = 0;
	u32 extent_thresh = range->extent_thresh;
	pgoff_t start_index;

	if (isize == 0)
		return 0;

	if (range->start >= isize)
		return -EINVAL;

	if (do_compress) {
		if (range->compress_type >= BTRFS_NR_COMPRESS_TYPES)
			return -EINVAL;
		if (range->compress_type)
			compress_type = range->compress_type;
	}

	if (extent_thresh == 0)
		extent_thresh = SZ_256K;

	if (range->start + range->len > range->start) {
		/* Got a specific range */
		last_byte = min(isize, range->start + range->len);
	} else {
		/* Defrag until file end */
		last_byte = isize;
	}

	/* Align the range */
	cur = round_down(range->start, fs_info->sectorsize);
	last_byte = round_up(last_byte, fs_info->sectorsize) - 1;

	/*
	 * If we were not given a ra, allocate a readahead context. As
	 * readahead is just an optimization, defrag will work without it so
	 * we don't error out.
	 */
	if (!ra) {
		ra_allocated = true;
		ra = kzalloc(sizeof(*ra), GFP_KERNEL);
		if (ra)
			file_ra_state_init(ra, inode->i_mapping);
	}

	/*
	 * Make writeback start from the beginning of the range, so that the
	 * defrag range can be written sequentially.
	 */
	start_index = cur >> PAGE_SHIFT;
	if (start_index < inode->i_mapping->writeback_index)
		inode->i_mapping->writeback_index = start_index;

	while (cur < last_byte) {
		const unsigned long prev_sectors_defragged = sectors_defragged;
		u64 last_scanned = cur;
		u64 cluster_end;

		if (btrfs_defrag_cancelled(fs_info)) {
			ret = -EAGAIN;
			break;
		}

		/* We want the cluster end at page boundary when possible */
		cluster_end = (((cur >> PAGE_SHIFT) +
			       (SZ_256K >> PAGE_SHIFT)) << PAGE_SHIFT) - 1;
		cluster_end = min(cluster_end, last_byte);

		btrfs_inode_lock(BTRFS_I(inode), 0);
		if (IS_SWAPFILE(inode)) {
			ret = -ETXTBSY;
			btrfs_inode_unlock(BTRFS_I(inode), 0);
			break;
		}
		if (!(inode->i_sb->s_flags & SB_ACTIVE)) {
			btrfs_inode_unlock(BTRFS_I(inode), 0);
			break;
		}
		if (do_compress)
			BTRFS_I(inode)->defrag_compress = compress_type;
		ret = defrag_one_cluster(BTRFS_I(inode), ra, cur,
				cluster_end + 1 - cur, extent_thresh,
				newer_than, do_compress, &sectors_defragged,
				max_to_defrag, &last_scanned);

		if (sectors_defragged > prev_sectors_defragged)
			balance_dirty_pages_ratelimited(inode->i_mapping);

		btrfs_inode_unlock(BTRFS_I(inode), 0);
		if (ret < 0)
			break;
		cur = max(cluster_end + 1, last_scanned);
		if (ret > 0) {
			ret = 0;
			break;
		}
		cond_resched();
	}

	if (ra_allocated)
		kfree(ra);
	/*
	 * Update range.start for autodefrag, this will indicate where to start
	 * in next run.
	 */
	range->start = cur;
	if (sectors_defragged) {
		/*
		 * We have defragged some sectors, for compression case they
		 * need to be written back immediately.
		 */
		if (range->flags & BTRFS_DEFRAG_RANGE_START_IO) {
			filemap_flush(inode->i_mapping);
			if (test_bit(BTRFS_INODE_HAS_ASYNC_EXTENT,
				     &BTRFS_I(inode)->runtime_flags))
				filemap_flush(inode->i_mapping);
		}
		if (range->compress_type == BTRFS_COMPRESS_LZO)
			btrfs_set_fs_incompat(fs_info, COMPRESS_LZO);
		else if (range->compress_type == BTRFS_COMPRESS_ZSTD)
			btrfs_set_fs_incompat(fs_info, COMPRESS_ZSTD);
		ret = sectors_defragged;
	}
	if (do_compress) {
		btrfs_inode_lock(BTRFS_I(inode), 0);
		BTRFS_I(inode)->defrag_compress = BTRFS_COMPRESS_NONE;
		btrfs_inode_unlock(BTRFS_I(inode), 0);
	}
	return ret;
}

void __cold btrfs_auto_defrag_exit(void)
{
	kmem_cache_destroy(btrfs_inode_defrag_cachep);
}

int __init btrfs_auto_defrag_init(void)
{
	btrfs_inode_defrag_cachep = kmem_cache_create("btrfs_inode_defrag",
					sizeof(struct inode_defrag), 0,
					SLAB_MEM_SPREAD,
					NULL);
	if (!btrfs_inode_defrag_cachep)
		return -ENOMEM;

	return 0;
}
