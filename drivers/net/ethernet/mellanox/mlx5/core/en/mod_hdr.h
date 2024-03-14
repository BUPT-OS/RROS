/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2020 Mellanox Technologies */

#ifndef __MLX5E_EN_MOD_HDR_H__
#define __MLX5E_EN_MOD_HDR_H__

#include <linux/hashtable.h>
#include <linux/mlx5/fs.h>

#define MLX5_MH_ACT_SZ MLX5_UN_SZ_BYTES(set_add_copy_action_in_auto)

struct mlx5e_mod_hdr_handle;

struct mlx5e_tc_mod_hdr_acts {
	int num_actions;
	int max_actions;
	bool is_static;
	void *actions;
};

#define DECLARE_MOD_HDR_ACTS_ACTIONS(name, len) \
	u8 name[len][MLX5_MH_ACT_SZ] = {}

#define DECLARE_MOD_HDR_ACTS(name, acts_arr) \
	struct mlx5e_tc_mod_hdr_acts name = { \
		.max_actions = ARRAY_SIZE(acts_arr), \
		.is_static = true, \
		.actions = acts_arr, \
	}

char *mlx5e_mod_hdr_alloc(struct mlx5_core_dev *mdev, int namespace,
			  struct mlx5e_tc_mod_hdr_acts *mod_hdr_acts);
void mlx5e_mod_hdr_dealloc(struct mlx5e_tc_mod_hdr_acts *mod_hdr_acts);
char *mlx5e_mod_hdr_get_item(struct mlx5e_tc_mod_hdr_acts *mod_hdr_acts, int pos);

struct mlx5e_mod_hdr_handle *
mlx5e_mod_hdr_attach(struct mlx5_core_dev *mdev,
		     struct mod_hdr_tbl *tbl,
		     enum mlx5_flow_namespace_type namespace,
		     struct mlx5e_tc_mod_hdr_acts *mod_hdr_acts);
void mlx5e_mod_hdr_detach(struct mlx5_core_dev *mdev,
			  struct mod_hdr_tbl *tbl,
			  struct mlx5e_mod_hdr_handle *mh);
struct mlx5_modify_hdr *mlx5e_mod_hdr_get(struct mlx5e_mod_hdr_handle *mh);

void mlx5e_mod_hdr_tbl_init(struct mod_hdr_tbl *tbl);
void mlx5e_mod_hdr_tbl_destroy(struct mod_hdr_tbl *tbl);

static inline int mlx5e_mod_hdr_max_actions(struct mlx5_core_dev *mdev, int namespace)
{
	if (namespace == MLX5_FLOW_NAMESPACE_FDB) /* FDB offloading */
		return MLX5_CAP_ESW_FLOWTABLE_FDB(mdev, max_modify_header_actions);
	else /* namespace is MLX5_FLOW_NAMESPACE_KERNEL - NIC offloading */
		return MLX5_CAP_FLOWTABLE_NIC_RX(mdev, max_modify_header_actions);
}

#endif /* __MLX5E_EN_MOD_HDR_H__ */
