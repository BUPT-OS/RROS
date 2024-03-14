// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
// Copyright (c) 2021, NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#include "en/tc_priv.h"
#include "en_tc.h"
#include "post_act.h"
#include "mlx5_core.h"
#include "fs_core.h"

struct mlx5e_post_act {
	enum mlx5_flow_namespace_type ns_type;
	struct mlx5_fs_chains *chains;
	struct mlx5_flow_table *ft;
	struct mlx5e_priv *priv;
	struct xarray ids;
};

struct mlx5e_post_act_handle {
	enum mlx5_flow_namespace_type ns_type;
	struct mlx5_flow_attr *attr;
	struct mlx5_flow_handle *rule;
	u32 id;
};

#define MLX5_POST_ACTION_BITS MLX5_REG_MAPPING_MBITS(FTEID_TO_REG)
#define MLX5_POST_ACTION_MASK MLX5_REG_MAPPING_MASK(FTEID_TO_REG)
#define MLX5_POST_ACTION_MAX MLX5_POST_ACTION_MASK

struct mlx5e_post_act *
mlx5e_tc_post_act_init(struct mlx5e_priv *priv, struct mlx5_fs_chains *chains,
		       enum mlx5_flow_namespace_type ns_type)
{
	enum fs_flow_table_type table_type = ns_type == MLX5_FLOW_NAMESPACE_FDB ?
					     FS_FT_FDB : FS_FT_NIC_RX;
	struct mlx5e_post_act *post_act;
	int err;

	if (!MLX5_CAP_FLOWTABLE_TYPE(priv->mdev, ignore_flow_level, table_type)) {
		if (priv->mdev->coredev_type == MLX5_COREDEV_PF)
			mlx5_core_warn(priv->mdev, "firmware level support is missing\n");
		err = -EOPNOTSUPP;
		goto err_check;
	}

	post_act = kzalloc(sizeof(*post_act), GFP_KERNEL);
	if (!post_act) {
		err = -ENOMEM;
		goto err_check;
	}
	post_act->ft = mlx5_chains_create_global_table(chains);
	if (IS_ERR(post_act->ft)) {
		err = PTR_ERR(post_act->ft);
		mlx5_core_warn(priv->mdev, "failed to create post action table, err: %d\n", err);
		goto err_ft;
	}
	post_act->chains = chains;
	post_act->ns_type = ns_type;
	post_act->priv = priv;
	xa_init_flags(&post_act->ids, XA_FLAGS_ALLOC1);
	return post_act;

err_ft:
	kfree(post_act);
err_check:
	return ERR_PTR(err);
}

void
mlx5e_tc_post_act_destroy(struct mlx5e_post_act *post_act)
{
	if (IS_ERR_OR_NULL(post_act))
		return;

	xa_destroy(&post_act->ids);
	mlx5_chains_destroy_global_table(post_act->chains, post_act->ft);
	kfree(post_act);
}

int
mlx5e_tc_post_act_offload(struct mlx5e_post_act *post_act,
			  struct mlx5e_post_act_handle *handle)
{
	struct mlx5_flow_spec *spec;
	int err;

	spec = kvzalloc(sizeof(*spec), GFP_KERNEL);
	if (!spec)
		return -ENOMEM;

	/* Post action rule matches on fte_id and executes original rule's tc rule action */
	mlx5e_tc_match_to_reg_match(spec, FTEID_TO_REG, handle->id, MLX5_POST_ACTION_MASK);

	handle->rule = mlx5e_tc_rule_offload(post_act->priv, spec, handle->attr);
	if (IS_ERR(handle->rule)) {
		err = PTR_ERR(handle->rule);
		netdev_warn(post_act->priv->netdev, "Failed to add post action rule");
		goto err_rule;
	}

	kvfree(spec);
	return 0;

err_rule:
	kvfree(spec);
	return err;
}

struct mlx5e_post_act_handle *
mlx5e_tc_post_act_add(struct mlx5e_post_act *post_act, struct mlx5_flow_attr *post_attr)
{
	struct mlx5e_post_act_handle *handle;
	int err;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return ERR_PTR(-ENOMEM);

	post_attr->chain = 0;
	post_attr->prio = 0;
	post_attr->ft = post_act->ft;
	post_attr->inner_match_level = MLX5_MATCH_NONE;
	post_attr->outer_match_level = MLX5_MATCH_NONE;
	post_attr->action &= ~MLX5_FLOW_CONTEXT_ACTION_DECAP;
	post_attr->flags |= MLX5_ATTR_FLAG_NO_IN_PORT;

	handle->ns_type = post_act->ns_type;
	/* Splits were handled before post action */
	if (handle->ns_type == MLX5_FLOW_NAMESPACE_FDB)
		post_attr->esw_attr->split_count = 0;

	err = xa_alloc(&post_act->ids, &handle->id, post_attr,
		       XA_LIMIT(1, MLX5_POST_ACTION_MAX), GFP_KERNEL);
	if (err)
		goto err_xarray;

	handle->attr = post_attr;

	return handle;

err_xarray:
	kfree(handle);
	return ERR_PTR(err);
}

void
mlx5e_tc_post_act_unoffload(struct mlx5e_post_act *post_act,
			    struct mlx5e_post_act_handle *handle)
{
	mlx5e_tc_rule_unoffload(post_act->priv, handle->rule, handle->attr);
	handle->rule = NULL;
}

void
mlx5e_tc_post_act_del(struct mlx5e_post_act *post_act, struct mlx5e_post_act_handle *handle)
{
	if (!IS_ERR_OR_NULL(handle->rule))
		mlx5e_tc_post_act_unoffload(post_act, handle);
	xa_erase(&post_act->ids, handle->id);
	kfree(handle);
}

struct mlx5_flow_table *
mlx5e_tc_post_act_get_ft(struct mlx5e_post_act *post_act)
{
	return post_act->ft;
}

/* Allocate a header modify action to write the post action handle fte id to a register. */
int
mlx5e_tc_post_act_set_handle(struct mlx5_core_dev *dev,
			     struct mlx5e_post_act_handle *handle,
			     struct mlx5e_tc_mod_hdr_acts *acts)
{
	return mlx5e_tc_match_to_reg_set(dev, acts, handle->ns_type, FTEID_TO_REG, handle->id);
}
