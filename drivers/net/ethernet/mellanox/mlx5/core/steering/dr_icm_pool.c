// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2019 Mellanox Technologies. */

#include "dr_types.h"

#define DR_ICM_MODIFY_HDR_ALIGN_BASE 64
#define DR_ICM_POOL_STE_HOT_MEM_PERCENT 25
#define DR_ICM_POOL_MODIFY_HDR_PTRN_HOT_MEM_PERCENT 50
#define DR_ICM_POOL_MODIFY_ACTION_HOT_MEM_PERCENT 90

struct mlx5dr_icm_hot_chunk {
	struct mlx5dr_icm_buddy_mem *buddy_mem;
	unsigned int seg;
	enum mlx5dr_icm_chunk_size size;
};

struct mlx5dr_icm_pool {
	enum mlx5dr_icm_type icm_type;
	enum mlx5dr_icm_chunk_size max_log_chunk_sz;
	struct mlx5dr_domain *dmn;
	struct kmem_cache *chunks_kmem_cache;

	/* memory management */
	struct mutex mutex; /* protect the ICM pool and ICM buddy */
	struct list_head buddy_mem_list;

	/* Hardware may be accessing this memory but at some future,
	 * undetermined time, it might cease to do so.
	 * sync_ste command sets them free.
	 */
	struct mlx5dr_icm_hot_chunk *hot_chunks_arr;
	u32 hot_chunks_num;
	u64 hot_memory_size;
	/* hot memory size threshold for triggering sync */
	u64 th;
};

struct mlx5dr_icm_dm {
	u32 obj_id;
	enum mlx5_sw_icm_type type;
	phys_addr_t addr;
	size_t length;
};

struct mlx5dr_icm_mr {
	u32 mkey;
	struct mlx5dr_icm_dm dm;
	struct mlx5dr_domain *dmn;
	size_t length;
	u64 icm_start_addr;
};

static int dr_icm_create_dm_mkey(struct mlx5_core_dev *mdev,
				 u32 pd, u64 length, u64 start_addr, int mode,
				 u32 *mkey)
{
	u32 inlen = MLX5_ST_SZ_BYTES(create_mkey_in);
	u32 in[MLX5_ST_SZ_DW(create_mkey_in)] = {};
	void *mkc;

	mkc = MLX5_ADDR_OF(create_mkey_in, in, memory_key_mkey_entry);

	MLX5_SET(mkc, mkc, access_mode_1_0, mode);
	MLX5_SET(mkc, mkc, access_mode_4_2, (mode >> 2) & 0x7);
	MLX5_SET(mkc, mkc, lw, 1);
	MLX5_SET(mkc, mkc, lr, 1);
	if (mode == MLX5_MKC_ACCESS_MODE_SW_ICM) {
		MLX5_SET(mkc, mkc, rw, 1);
		MLX5_SET(mkc, mkc, rr, 1);
	}

	MLX5_SET64(mkc, mkc, len, length);
	MLX5_SET(mkc, mkc, pd, pd);
	MLX5_SET(mkc, mkc, qpn, 0xffffff);
	MLX5_SET64(mkc, mkc, start_addr, start_addr);

	return mlx5_core_create_mkey(mdev, mkey, in, inlen);
}

u64 mlx5dr_icm_pool_get_chunk_mr_addr(struct mlx5dr_icm_chunk *chunk)
{
	u32 offset = mlx5dr_icm_pool_dm_type_to_entry_size(chunk->buddy_mem->pool->icm_type);

	return (u64)offset * chunk->seg;
}

u32 mlx5dr_icm_pool_get_chunk_rkey(struct mlx5dr_icm_chunk *chunk)
{
	return chunk->buddy_mem->icm_mr->mkey;
}

u64 mlx5dr_icm_pool_get_chunk_icm_addr(struct mlx5dr_icm_chunk *chunk)
{
	u32 size = mlx5dr_icm_pool_dm_type_to_entry_size(chunk->buddy_mem->pool->icm_type);

	return (u64)chunk->buddy_mem->icm_mr->icm_start_addr + size * chunk->seg;
}

u32 mlx5dr_icm_pool_get_chunk_byte_size(struct mlx5dr_icm_chunk *chunk)
{
	return mlx5dr_icm_pool_chunk_size_to_byte(chunk->size,
			chunk->buddy_mem->pool->icm_type);
}

u32 mlx5dr_icm_pool_get_chunk_num_of_entries(struct mlx5dr_icm_chunk *chunk)
{
	return mlx5dr_icm_pool_chunk_size_to_entries(chunk->size);
}

static struct mlx5dr_icm_mr *
dr_icm_pool_mr_create(struct mlx5dr_icm_pool *pool)
{
	struct mlx5_core_dev *mdev = pool->dmn->mdev;
	enum mlx5_sw_icm_type dm_type = 0;
	struct mlx5dr_icm_mr *icm_mr;
	size_t log_align_base = 0;
	int err;

	icm_mr = kvzalloc(sizeof(*icm_mr), GFP_KERNEL);
	if (!icm_mr)
		return NULL;

	icm_mr->dmn = pool->dmn;

	icm_mr->dm.length = mlx5dr_icm_pool_chunk_size_to_byte(pool->max_log_chunk_sz,
							       pool->icm_type);

	switch (pool->icm_type) {
	case DR_ICM_TYPE_STE:
		dm_type = MLX5_SW_ICM_TYPE_STEERING;
		log_align_base = ilog2(icm_mr->dm.length);
		break;
	case DR_ICM_TYPE_MODIFY_ACTION:
		dm_type = MLX5_SW_ICM_TYPE_HEADER_MODIFY;
		/* Align base is 64B */
		log_align_base = ilog2(DR_ICM_MODIFY_HDR_ALIGN_BASE);
		break;
	case DR_ICM_TYPE_MODIFY_HDR_PTRN:
		dm_type = MLX5_SW_ICM_TYPE_HEADER_MODIFY_PATTERN;
		/* Align base is 64B */
		log_align_base = ilog2(DR_ICM_MODIFY_HDR_ALIGN_BASE);
		break;
	default:
		WARN_ON(pool->icm_type);
	}

	icm_mr->dm.type = dm_type;

	err = mlx5_dm_sw_icm_alloc(mdev, icm_mr->dm.type, icm_mr->dm.length,
				   log_align_base, 0, &icm_mr->dm.addr,
				   &icm_mr->dm.obj_id);
	if (err) {
		mlx5dr_err(pool->dmn, "Failed to allocate SW ICM memory, err (%d)\n", err);
		goto free_icm_mr;
	}

	/* Register device memory */
	err = dr_icm_create_dm_mkey(mdev, pool->dmn->pdn,
				    icm_mr->dm.length,
				    icm_mr->dm.addr,
				    MLX5_MKC_ACCESS_MODE_SW_ICM,
				    &icm_mr->mkey);
	if (err) {
		mlx5dr_err(pool->dmn, "Failed to create SW ICM MKEY, err (%d)\n", err);
		goto free_dm;
	}

	icm_mr->icm_start_addr = icm_mr->dm.addr;

	if (icm_mr->icm_start_addr & (BIT(log_align_base) - 1)) {
		mlx5dr_err(pool->dmn, "Failed to get Aligned ICM mem (asked: %zu)\n",
			   log_align_base);
		goto free_mkey;
	}

	return icm_mr;

free_mkey:
	mlx5_core_destroy_mkey(mdev, icm_mr->mkey);
free_dm:
	mlx5_dm_sw_icm_dealloc(mdev, icm_mr->dm.type, icm_mr->dm.length, 0,
			       icm_mr->dm.addr, icm_mr->dm.obj_id);
free_icm_mr:
	kvfree(icm_mr);
	return NULL;
}

static void dr_icm_pool_mr_destroy(struct mlx5dr_icm_mr *icm_mr)
{
	struct mlx5_core_dev *mdev = icm_mr->dmn->mdev;
	struct mlx5dr_icm_dm *dm = &icm_mr->dm;

	mlx5_core_destroy_mkey(mdev, icm_mr->mkey);
	mlx5_dm_sw_icm_dealloc(mdev, dm->type, dm->length, 0,
			       dm->addr, dm->obj_id);
	kvfree(icm_mr);
}

static int dr_icm_buddy_get_ste_size(struct mlx5dr_icm_buddy_mem *buddy)
{
	/* We support only one type of STE size, both for ConnectX-5 and later
	 * devices. Once the support for match STE which has a larger tag is
	 * added (32B instead of 16B), the STE size for devices later than
	 * ConnectX-5 needs to account for that.
	 */
	return DR_STE_SIZE_REDUCED;
}

static void dr_icm_chunk_ste_init(struct mlx5dr_icm_chunk *chunk, int offset)
{
	int num_of_entries = mlx5dr_icm_pool_get_chunk_num_of_entries(chunk);
	struct mlx5dr_icm_buddy_mem *buddy = chunk->buddy_mem;
	int ste_size = dr_icm_buddy_get_ste_size(buddy);
	int index = offset / DR_STE_SIZE;

	chunk->ste_arr = &buddy->ste_arr[index];
	chunk->miss_list = &buddy->miss_list[index];
	chunk->hw_ste_arr = buddy->hw_ste_arr + index * ste_size;

	memset(chunk->hw_ste_arr, 0, num_of_entries * ste_size);
	memset(chunk->ste_arr, 0,
	       num_of_entries * sizeof(chunk->ste_arr[0]));
}

static int dr_icm_buddy_init_ste_cache(struct mlx5dr_icm_buddy_mem *buddy)
{
	int num_of_entries =
		mlx5dr_icm_pool_chunk_size_to_entries(buddy->pool->max_log_chunk_sz);

	buddy->ste_arr = kvcalloc(num_of_entries,
				  sizeof(struct mlx5dr_ste), GFP_KERNEL);
	if (!buddy->ste_arr)
		return -ENOMEM;

	/* Preallocate full STE size on non-ConnectX-5 devices since
	 * we need to support both full and reduced with the same cache.
	 */
	buddy->hw_ste_arr = kvcalloc(num_of_entries,
				     dr_icm_buddy_get_ste_size(buddy), GFP_KERNEL);
	if (!buddy->hw_ste_arr)
		goto free_ste_arr;

	buddy->miss_list = kvmalloc(num_of_entries * sizeof(struct list_head), GFP_KERNEL);
	if (!buddy->miss_list)
		goto free_hw_ste_arr;

	return 0;

free_hw_ste_arr:
	kvfree(buddy->hw_ste_arr);
free_ste_arr:
	kvfree(buddy->ste_arr);
	return -ENOMEM;
}

static void dr_icm_buddy_cleanup_ste_cache(struct mlx5dr_icm_buddy_mem *buddy)
{
	kvfree(buddy->ste_arr);
	kvfree(buddy->hw_ste_arr);
	kvfree(buddy->miss_list);
}

static int dr_icm_buddy_create(struct mlx5dr_icm_pool *pool)
{
	struct mlx5dr_icm_buddy_mem *buddy;
	struct mlx5dr_icm_mr *icm_mr;

	icm_mr = dr_icm_pool_mr_create(pool);
	if (!icm_mr)
		return -ENOMEM;

	buddy = kvzalloc(sizeof(*buddy), GFP_KERNEL);
	if (!buddy)
		goto free_mr;

	if (mlx5dr_buddy_init(buddy, pool->max_log_chunk_sz))
		goto err_free_buddy;

	buddy->icm_mr = icm_mr;
	buddy->pool = pool;

	if (pool->icm_type == DR_ICM_TYPE_STE) {
		/* Reduce allocations by preallocating and reusing the STE structures */
		if (dr_icm_buddy_init_ste_cache(buddy))
			goto err_cleanup_buddy;
	}

	/* add it to the -start- of the list in order to search in it first */
	list_add(&buddy->list_node, &pool->buddy_mem_list);

	pool->dmn->num_buddies[pool->icm_type]++;

	return 0;

err_cleanup_buddy:
	mlx5dr_buddy_cleanup(buddy);
err_free_buddy:
	kvfree(buddy);
free_mr:
	dr_icm_pool_mr_destroy(icm_mr);
	return -ENOMEM;
}

static void dr_icm_buddy_destroy(struct mlx5dr_icm_buddy_mem *buddy)
{
	enum mlx5dr_icm_type icm_type = buddy->pool->icm_type;

	dr_icm_pool_mr_destroy(buddy->icm_mr);

	mlx5dr_buddy_cleanup(buddy);

	if (icm_type == DR_ICM_TYPE_STE)
		dr_icm_buddy_cleanup_ste_cache(buddy);

	buddy->pool->dmn->num_buddies[icm_type]--;

	kvfree(buddy);
}

static void
dr_icm_chunk_init(struct mlx5dr_icm_chunk *chunk,
		  struct mlx5dr_icm_pool *pool,
		  enum mlx5dr_icm_chunk_size chunk_size,
		  struct mlx5dr_icm_buddy_mem *buddy_mem_pool,
		  unsigned int seg)
{
	int offset;

	chunk->seg = seg;
	chunk->size = chunk_size;
	chunk->buddy_mem = buddy_mem_pool;

	if (pool->icm_type == DR_ICM_TYPE_STE) {
		offset = mlx5dr_icm_pool_dm_type_to_entry_size(pool->icm_type) * seg;
		dr_icm_chunk_ste_init(chunk, offset);
	}

	buddy_mem_pool->used_memory += mlx5dr_icm_pool_get_chunk_byte_size(chunk);
}

static bool dr_icm_pool_is_sync_required(struct mlx5dr_icm_pool *pool)
{
	return pool->hot_memory_size > pool->th;
}

static void dr_icm_pool_clear_hot_chunks_arr(struct mlx5dr_icm_pool *pool)
{
	struct mlx5dr_icm_hot_chunk *hot_chunk;
	u32 i, num_entries;

	for (i = 0; i < pool->hot_chunks_num; i++) {
		hot_chunk = &pool->hot_chunks_arr[i];
		num_entries = mlx5dr_icm_pool_chunk_size_to_entries(hot_chunk->size);
		mlx5dr_buddy_free_mem(hot_chunk->buddy_mem,
				      hot_chunk->seg, ilog2(num_entries));
		hot_chunk->buddy_mem->used_memory -=
			mlx5dr_icm_pool_chunk_size_to_byte(hot_chunk->size,
							   pool->icm_type);
	}

	pool->hot_chunks_num = 0;
	pool->hot_memory_size = 0;
}

static int dr_icm_pool_sync_all_buddy_pools(struct mlx5dr_icm_pool *pool)
{
	struct mlx5dr_icm_buddy_mem *buddy, *tmp_buddy;
	int err;

	err = mlx5dr_cmd_sync_steering(pool->dmn->mdev);
	if (err) {
		mlx5dr_err(pool->dmn, "Failed to sync to HW (err: %d)\n", err);
		return err;
	}

	dr_icm_pool_clear_hot_chunks_arr(pool);

	list_for_each_entry_safe(buddy, tmp_buddy, &pool->buddy_mem_list, list_node) {
		if (!buddy->used_memory && pool->icm_type == DR_ICM_TYPE_STE)
			dr_icm_buddy_destroy(buddy);
	}

	return 0;
}

static int dr_icm_handle_buddies_get_mem(struct mlx5dr_icm_pool *pool,
					 enum mlx5dr_icm_chunk_size chunk_size,
					 struct mlx5dr_icm_buddy_mem **buddy,
					 unsigned int *seg)
{
	struct mlx5dr_icm_buddy_mem *buddy_mem_pool;
	bool new_mem = false;
	int err;

alloc_buddy_mem:
	/* find the next free place from the buddy list */
	list_for_each_entry(buddy_mem_pool, &pool->buddy_mem_list, list_node) {
		err = mlx5dr_buddy_alloc_mem(buddy_mem_pool,
					     chunk_size, seg);
		if (!err)
			goto found;

		if (WARN_ON(new_mem)) {
			/* We have new memory pool, first in the list */
			mlx5dr_err(pool->dmn,
				   "No memory for order: %d\n",
				   chunk_size);
			goto out;
		}
	}

	/* no more available allocators in that pool, create new */
	err = dr_icm_buddy_create(pool);
	if (err) {
		mlx5dr_err(pool->dmn,
			   "Failed creating buddy for order %d\n",
			   chunk_size);
		goto out;
	}

	/* mark we have new memory, first in list */
	new_mem = true;
	goto alloc_buddy_mem;

found:
	*buddy = buddy_mem_pool;
out:
	return err;
}

/* Allocate an ICM chunk, each chunk holds a piece of ICM memory and
 * also memory used for HW STE management for optimizations.
 */
struct mlx5dr_icm_chunk *
mlx5dr_icm_alloc_chunk(struct mlx5dr_icm_pool *pool,
		       enum mlx5dr_icm_chunk_size chunk_size)
{
	struct mlx5dr_icm_chunk *chunk = NULL;
	struct mlx5dr_icm_buddy_mem *buddy;
	unsigned int seg;
	int ret;

	if (chunk_size > pool->max_log_chunk_sz)
		return NULL;

	mutex_lock(&pool->mutex);
	/* find mem, get back the relevant buddy pool and seg in that mem */
	ret = dr_icm_handle_buddies_get_mem(pool, chunk_size, &buddy, &seg);
	if (ret)
		goto out;

	chunk = kmem_cache_alloc(pool->chunks_kmem_cache, GFP_KERNEL);
	if (!chunk)
		goto out_err;

	dr_icm_chunk_init(chunk, pool, chunk_size, buddy, seg);

	goto out;

out_err:
	mlx5dr_buddy_free_mem(buddy, seg, chunk_size);
out:
	mutex_unlock(&pool->mutex);
	return chunk;
}

void mlx5dr_icm_free_chunk(struct mlx5dr_icm_chunk *chunk)
{
	struct mlx5dr_icm_buddy_mem *buddy = chunk->buddy_mem;
	struct mlx5dr_icm_pool *pool = buddy->pool;
	struct mlx5dr_icm_hot_chunk *hot_chunk;
	struct kmem_cache *chunks_cache;

	chunks_cache = pool->chunks_kmem_cache;

	/* move the chunk to the waiting chunks array, AKA "hot" memory */
	mutex_lock(&pool->mutex);

	pool->hot_memory_size += mlx5dr_icm_pool_get_chunk_byte_size(chunk);

	hot_chunk = &pool->hot_chunks_arr[pool->hot_chunks_num++];
	hot_chunk->buddy_mem = chunk->buddy_mem;
	hot_chunk->seg = chunk->seg;
	hot_chunk->size = chunk->size;

	kmem_cache_free(chunks_cache, chunk);

	/* Check if we have chunks that are waiting for sync-ste */
	if (dr_icm_pool_is_sync_required(pool))
		dr_icm_pool_sync_all_buddy_pools(pool);

	mutex_unlock(&pool->mutex);
}

struct mlx5dr_ste_htbl *mlx5dr_icm_pool_alloc_htbl(struct mlx5dr_icm_pool *pool)
{
	return kmem_cache_alloc(pool->dmn->htbls_kmem_cache, GFP_KERNEL);
}

void mlx5dr_icm_pool_free_htbl(struct mlx5dr_icm_pool *pool, struct mlx5dr_ste_htbl *htbl)
{
	kmem_cache_free(pool->dmn->htbls_kmem_cache, htbl);
}

struct mlx5dr_icm_pool *mlx5dr_icm_pool_create(struct mlx5dr_domain *dmn,
					       enum mlx5dr_icm_type icm_type)
{
	u32 num_of_chunks, entry_size;
	struct mlx5dr_icm_pool *pool;
	u32 max_hot_size = 0;

	pool = kvzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return NULL;

	pool->dmn = dmn;
	pool->icm_type = icm_type;
	pool->chunks_kmem_cache = dmn->chunks_kmem_cache;

	INIT_LIST_HEAD(&pool->buddy_mem_list);
	mutex_init(&pool->mutex);

	switch (icm_type) {
	case DR_ICM_TYPE_STE:
		pool->max_log_chunk_sz = dmn->info.max_log_sw_icm_sz;
		max_hot_size = mlx5dr_icm_pool_chunk_size_to_byte(pool->max_log_chunk_sz,
								  pool->icm_type) *
			       DR_ICM_POOL_STE_HOT_MEM_PERCENT / 100;
		break;
	case DR_ICM_TYPE_MODIFY_ACTION:
		pool->max_log_chunk_sz = dmn->info.max_log_action_icm_sz;
		max_hot_size = mlx5dr_icm_pool_chunk_size_to_byte(pool->max_log_chunk_sz,
								  pool->icm_type) *
			       DR_ICM_POOL_MODIFY_ACTION_HOT_MEM_PERCENT / 100;
		break;
	case DR_ICM_TYPE_MODIFY_HDR_PTRN:
		pool->max_log_chunk_sz = dmn->info.max_log_modify_hdr_pattern_icm_sz;
		max_hot_size = mlx5dr_icm_pool_chunk_size_to_byte(pool->max_log_chunk_sz,
								  pool->icm_type) *
			       DR_ICM_POOL_MODIFY_HDR_PTRN_HOT_MEM_PERCENT / 100;
		break;
	default:
		WARN_ON(icm_type);
	}

	entry_size = mlx5dr_icm_pool_dm_type_to_entry_size(pool->icm_type);

	num_of_chunks = DIV_ROUND_UP(max_hot_size, entry_size) + 1;
	pool->th = max_hot_size;

	pool->hot_chunks_arr = kvcalloc(num_of_chunks,
					sizeof(struct mlx5dr_icm_hot_chunk),
					GFP_KERNEL);
	if (!pool->hot_chunks_arr)
		goto free_pool;

	return pool;

free_pool:
	kvfree(pool);
	return NULL;
}

void mlx5dr_icm_pool_destroy(struct mlx5dr_icm_pool *pool)
{
	struct mlx5dr_icm_buddy_mem *buddy, *tmp_buddy;

	dr_icm_pool_clear_hot_chunks_arr(pool);

	list_for_each_entry_safe(buddy, tmp_buddy, &pool->buddy_mem_list, list_node)
		dr_icm_buddy_destroy(buddy);

	kvfree(pool->hot_chunks_arr);
	mutex_destroy(&pool->mutex);
	kvfree(pool);
}
