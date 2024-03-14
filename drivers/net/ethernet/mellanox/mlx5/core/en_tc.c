/*
 * Copyright (c) 2016, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <net/flow_dissector.h>
#include <net/flow_offload.h>
#include <net/sch_generic.h>
#include <net/pkt_cls.h>
#include <linux/mlx5/fs.h>
#include <linux/mlx5/device.h>
#include <linux/rhashtable.h>
#include <linux/refcount.h>
#include <linux/completion.h>
#include <net/arp.h>
#include <net/ipv6_stubs.h>
#include <net/bareudp.h>
#include <net/bonding.h>
#include <net/dst_metadata.h>
#include "devlink.h"
#include "en.h"
#include "en/tc/post_act.h"
#include "en/tc/act_stats.h"
#include "en_rep.h"
#include "en/rep/tc.h"
#include "en/rep/neigh.h"
#include "en_tc.h"
#include "eswitch.h"
#include "fs_core.h"
#include "en/port.h"
#include "en/tc_tun.h"
#include "en/mapping.h"
#include "en/tc_ct.h"
#include "en/mod_hdr.h"
#include "en/tc_tun_encap.h"
#include "en/tc/sample.h"
#include "en/tc/act/act.h"
#include "en/tc/post_meter.h"
#include "lib/devcom.h"
#include "lib/geneve.h"
#include "lib/fs_chains.h"
#include "diag/en_tc_tracepoint.h"
#include <asm/div64.h>
#include "lag/lag.h"
#include "lag/mp.h"

#define MLX5E_TC_TABLE_NUM_GROUPS 4
#define MLX5E_TC_TABLE_MAX_GROUP_SIZE BIT(18)

struct mlx5e_tc_table {
	/* Protects the dynamic assignment of the t parameter
	 * which is the nic tc root table.
	 */
	struct mutex			t_lock;
	struct mlx5e_priv		*priv;
	struct mlx5_flow_table		*t;
	struct mlx5_flow_table		*miss_t;
	struct mlx5_fs_chains           *chains;
	struct mlx5e_post_act		*post_act;

	struct rhashtable               ht;

	struct mod_hdr_tbl mod_hdr;
	struct mutex hairpin_tbl_lock; /* protects hairpin_tbl */
	DECLARE_HASHTABLE(hairpin_tbl, 8);

	struct notifier_block     netdevice_nb;
	struct netdev_net_notifier	netdevice_nn;

	struct mlx5_tc_ct_priv         *ct;
	struct mapping_ctx             *mapping;
	struct dentry                  *dfs_root;

	/* tc action stats */
	struct mlx5e_tc_act_stats_handle *action_stats_handle;
};

struct mlx5e_tc_attr_to_reg_mapping mlx5e_tc_attr_to_reg_mappings[] = {
	[MAPPED_OBJ_TO_REG] = {
		.mfield = MLX5_ACTION_IN_FIELD_METADATA_REG_C_0,
		.moffset = 0,
		.mlen = 16,
	},
	[VPORT_TO_REG] = {
		.mfield = MLX5_ACTION_IN_FIELD_METADATA_REG_C_0,
		.moffset = 16,
		.mlen = 16,
	},
	[TUNNEL_TO_REG] = {
		.mfield = MLX5_ACTION_IN_FIELD_METADATA_REG_C_1,
		.moffset = 8,
		.mlen = ESW_TUN_OPTS_BITS + ESW_TUN_ID_BITS,
		.soffset = MLX5_BYTE_OFF(fte_match_param,
					 misc_parameters_2.metadata_reg_c_1),
	},
	[ZONE_TO_REG] = zone_to_reg_ct,
	[ZONE_RESTORE_TO_REG] = zone_restore_to_reg_ct,
	[CTSTATE_TO_REG] = ctstate_to_reg_ct,
	[MARK_TO_REG] = mark_to_reg_ct,
	[LABELS_TO_REG] = labels_to_reg_ct,
	[FTEID_TO_REG] = fteid_to_reg_ct,
	/* For NIC rules we store the restore metadata directly
	 * into reg_b that is passed to SW since we don't
	 * jump between steering domains.
	 */
	[NIC_MAPPED_OBJ_TO_REG] = {
		.mfield = MLX5_ACTION_IN_FIELD_METADATA_REG_B,
		.moffset = 0,
		.mlen = 16,
	},
	[NIC_ZONE_RESTORE_TO_REG] = nic_zone_restore_to_reg_ct,
	[PACKET_COLOR_TO_REG] = packet_color_to_reg,
};

struct mlx5e_tc_jump_state {
	u32 jump_count;
	bool jump_target;
	struct mlx5_flow_attr *jumping_attr;

	enum flow_action_id last_id;
	u32 last_index;
};

struct mlx5e_tc_table *mlx5e_tc_table_alloc(void)
{
	struct mlx5e_tc_table *tc;

	tc = kvzalloc(sizeof(*tc), GFP_KERNEL);
	return tc ? tc : ERR_PTR(-ENOMEM);
}

void mlx5e_tc_table_free(struct mlx5e_tc_table *tc)
{
	kvfree(tc);
}

struct mlx5_fs_chains *mlx5e_nic_chains(struct mlx5e_tc_table *tc)
{
	return tc->chains;
}

/* To avoid false lock dependency warning set the tc_ht lock
 * class different than the lock class of the ht being used when deleting
 * last flow from a group and then deleting a group, we get into del_sw_flow_group()
 * which call rhashtable_destroy on fg->ftes_hash which will take ht->mutex but
 * it's different than the ht->mutex here.
 */
static struct lock_class_key tc_ht_lock_key;
static struct lock_class_key tc_ht_wq_key;

static void mlx5e_put_flow_tunnel_id(struct mlx5e_tc_flow *flow);
static void free_flow_post_acts(struct mlx5e_tc_flow *flow);
static void mlx5_free_flow_attr_actions(struct mlx5e_tc_flow *flow,
					struct mlx5_flow_attr *attr);

void
mlx5e_tc_match_to_reg_match(struct mlx5_flow_spec *spec,
			    enum mlx5e_tc_attr_to_reg type,
			    u32 val,
			    u32 mask)
{
	void *headers_c = spec->match_criteria, *headers_v = spec->match_value, *fmask, *fval;
	int soffset = mlx5e_tc_attr_to_reg_mappings[type].soffset;
	int moffset = mlx5e_tc_attr_to_reg_mappings[type].moffset;
	int match_len = mlx5e_tc_attr_to_reg_mappings[type].mlen;
	u32 max_mask = GENMASK(match_len - 1, 0);
	__be32 curr_mask_be, curr_val_be;
	u32 curr_mask, curr_val;

	fmask = headers_c + soffset;
	fval = headers_v + soffset;

	memcpy(&curr_mask_be, fmask, 4);
	memcpy(&curr_val_be, fval, 4);

	curr_mask = be32_to_cpu(curr_mask_be);
	curr_val = be32_to_cpu(curr_val_be);

	//move to correct offset
	WARN_ON(mask > max_mask);
	mask <<= moffset;
	val <<= moffset;
	max_mask <<= moffset;

	//zero val and mask
	curr_mask &= ~max_mask;
	curr_val &= ~max_mask;

	//add current to mask
	curr_mask |= mask;
	curr_val |= val;

	//back to be32 and write
	curr_mask_be = cpu_to_be32(curr_mask);
	curr_val_be = cpu_to_be32(curr_val);

	memcpy(fmask, &curr_mask_be, 4);
	memcpy(fval, &curr_val_be, 4);

	spec->match_criteria_enable |= MLX5_MATCH_MISC_PARAMETERS_2;
}

void
mlx5e_tc_match_to_reg_get_match(struct mlx5_flow_spec *spec,
				enum mlx5e_tc_attr_to_reg type,
				u32 *val,
				u32 *mask)
{
	void *headers_c = spec->match_criteria, *headers_v = spec->match_value, *fmask, *fval;
	int soffset = mlx5e_tc_attr_to_reg_mappings[type].soffset;
	int moffset = mlx5e_tc_attr_to_reg_mappings[type].moffset;
	int match_len = mlx5e_tc_attr_to_reg_mappings[type].mlen;
	u32 max_mask = GENMASK(match_len - 1, 0);
	__be32 curr_mask_be, curr_val_be;
	u32 curr_mask, curr_val;

	fmask = headers_c + soffset;
	fval = headers_v + soffset;

	memcpy(&curr_mask_be, fmask, 4);
	memcpy(&curr_val_be, fval, 4);

	curr_mask = be32_to_cpu(curr_mask_be);
	curr_val = be32_to_cpu(curr_val_be);

	*mask = (curr_mask >> moffset) & max_mask;
	*val = (curr_val >> moffset) & max_mask;
}

int
mlx5e_tc_match_to_reg_set_and_get_id(struct mlx5_core_dev *mdev,
				     struct mlx5e_tc_mod_hdr_acts *mod_hdr_acts,
				     enum mlx5_flow_namespace_type ns,
				     enum mlx5e_tc_attr_to_reg type,
				     u32 data)
{
	int moffset = mlx5e_tc_attr_to_reg_mappings[type].moffset;
	int mfield = mlx5e_tc_attr_to_reg_mappings[type].mfield;
	int mlen = mlx5e_tc_attr_to_reg_mappings[type].mlen;
	char *modact;
	int err;

	modact = mlx5e_mod_hdr_alloc(mdev, ns, mod_hdr_acts);
	if (IS_ERR(modact))
		return PTR_ERR(modact);

	/* Firmware has 5bit length field and 0 means 32bits */
	if (mlen == 32)
		mlen = 0;

	MLX5_SET(set_action_in, modact, action_type, MLX5_ACTION_TYPE_SET);
	MLX5_SET(set_action_in, modact, field, mfield);
	MLX5_SET(set_action_in, modact, offset, moffset);
	MLX5_SET(set_action_in, modact, length, mlen);
	MLX5_SET(set_action_in, modact, data, data);
	err = mod_hdr_acts->num_actions;
	mod_hdr_acts->num_actions++;

	return err;
}

static struct mlx5e_tc_act_stats_handle  *
get_act_stats_handle(struct mlx5e_priv *priv)
{
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5_rep_uplink_priv *uplink_priv;
	struct mlx5e_rep_priv *uplink_rpriv;

	if (is_mdev_switchdev_mode(priv->mdev)) {
		uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
		uplink_priv = &uplink_rpriv->uplink_priv;

		return uplink_priv->action_stats_handle;
	}

	return tc->action_stats_handle;
}

struct mlx5e_tc_int_port_priv *
mlx5e_get_int_port_priv(struct mlx5e_priv *priv)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5_rep_uplink_priv *uplink_priv;
	struct mlx5e_rep_priv *uplink_rpriv;

	if (is_mdev_switchdev_mode(priv->mdev)) {
		uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
		uplink_priv = &uplink_rpriv->uplink_priv;

		return uplink_priv->int_port_priv;
	}

	return NULL;
}

struct mlx5e_flow_meters *
mlx5e_get_flow_meters(struct mlx5_core_dev *dev)
{
	struct mlx5_eswitch *esw = dev->priv.eswitch;
	struct mlx5_rep_uplink_priv *uplink_priv;
	struct mlx5e_rep_priv *uplink_rpriv;
	struct mlx5e_priv *priv;

	if (is_mdev_switchdev_mode(dev)) {
		uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
		uplink_priv = &uplink_rpriv->uplink_priv;
		priv = netdev_priv(uplink_rpriv->netdev);
		if (!uplink_priv->flow_meters)
			uplink_priv->flow_meters =
				mlx5e_flow_meters_init(priv,
						       MLX5_FLOW_NAMESPACE_FDB,
						       uplink_priv->post_act);
		if (!IS_ERR(uplink_priv->flow_meters))
			return uplink_priv->flow_meters;
	}

	return NULL;
}

static struct mlx5_tc_ct_priv *
get_ct_priv(struct mlx5e_priv *priv)
{
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5_rep_uplink_priv *uplink_priv;
	struct mlx5e_rep_priv *uplink_rpriv;

	if (is_mdev_switchdev_mode(priv->mdev)) {
		uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
		uplink_priv = &uplink_rpriv->uplink_priv;

		return uplink_priv->ct_priv;
	}

	return tc->ct;
}

static struct mlx5e_tc_psample *
get_sample_priv(struct mlx5e_priv *priv)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5_rep_uplink_priv *uplink_priv;
	struct mlx5e_rep_priv *uplink_rpriv;

	if (is_mdev_switchdev_mode(priv->mdev)) {
		uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
		uplink_priv = &uplink_rpriv->uplink_priv;

		return uplink_priv->tc_psample;
	}

	return NULL;
}

static struct mlx5e_post_act *
get_post_action(struct mlx5e_priv *priv)
{
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5_rep_uplink_priv *uplink_priv;
	struct mlx5e_rep_priv *uplink_rpriv;

	if (is_mdev_switchdev_mode(priv->mdev)) {
		uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
		uplink_priv = &uplink_rpriv->uplink_priv;

		return uplink_priv->post_act;
	}

	return tc->post_act;
}

struct mlx5_flow_handle *
mlx5_tc_rule_insert(struct mlx5e_priv *priv,
		    struct mlx5_flow_spec *spec,
		    struct mlx5_flow_attr *attr)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;

	if (is_mdev_switchdev_mode(priv->mdev))
		return mlx5_eswitch_add_offloaded_rule(esw, spec, attr);

	return	mlx5e_add_offloaded_nic_rule(priv, spec, attr);
}

void
mlx5_tc_rule_delete(struct mlx5e_priv *priv,
		    struct mlx5_flow_handle *rule,
		    struct mlx5_flow_attr *attr)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;

	if (is_mdev_switchdev_mode(priv->mdev)) {
		mlx5_eswitch_del_offloaded_rule(esw, rule, attr);
		return;
	}

	mlx5e_del_offloaded_nic_rule(priv, rule, attr);
}

static bool
is_flow_meter_action(struct mlx5_flow_attr *attr)
{
	return (((attr->action & MLX5_FLOW_CONTEXT_ACTION_EXECUTE_ASO) &&
		 (attr->exe_aso_type == MLX5_EXE_ASO_FLOW_METER)) ||
		attr->flags & MLX5_ATTR_FLAG_MTU);
}

static int
mlx5e_tc_add_flow_meter(struct mlx5e_priv *priv,
			struct mlx5_flow_attr *attr)
{
	struct mlx5e_post_act *post_act = get_post_action(priv);
	struct mlx5e_post_meter_priv *post_meter;
	enum mlx5_flow_namespace_type ns_type;
	struct mlx5e_flow_meter_handle *meter;
	enum mlx5e_post_meter_type type;

	meter = mlx5e_tc_meter_replace(priv->mdev, &attr->meter_attr.params);
	if (IS_ERR(meter)) {
		mlx5_core_err(priv->mdev, "Failed to get flow meter\n");
		return PTR_ERR(meter);
	}

	ns_type = mlx5e_tc_meter_get_namespace(meter->flow_meters);
	type = meter->params.mtu ? MLX5E_POST_METER_MTU : MLX5E_POST_METER_RATE;
	post_meter = mlx5e_post_meter_init(priv, ns_type, post_act,
					   type,
					   meter->act_counter, meter->drop_counter,
					   attr->branch_true, attr->branch_false);
	if (IS_ERR(post_meter)) {
		mlx5_core_err(priv->mdev, "Failed to init post meter\n");
		goto err_meter_init;
	}

	attr->meter_attr.meter = meter;
	attr->meter_attr.post_meter = post_meter;
	attr->dest_ft = mlx5e_post_meter_get_ft(post_meter);
	attr->action |= MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;

	return 0;

err_meter_init:
	mlx5e_tc_meter_put(meter);
	return PTR_ERR(post_meter);
}

static void
mlx5e_tc_del_flow_meter(struct mlx5_eswitch *esw, struct mlx5_flow_attr *attr)
{
	mlx5e_post_meter_cleanup(esw, attr->meter_attr.post_meter);
	mlx5e_tc_meter_put(attr->meter_attr.meter);
}

struct mlx5_flow_handle *
mlx5e_tc_rule_offload(struct mlx5e_priv *priv,
		      struct mlx5_flow_spec *spec,
		      struct mlx5_flow_attr *attr)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	int err;

	if (!is_mdev_switchdev_mode(priv->mdev))
		return mlx5e_add_offloaded_nic_rule(priv, spec, attr);

	if (attr->flags & MLX5_ATTR_FLAG_SAMPLE)
		return mlx5e_tc_sample_offload(get_sample_priv(priv), spec, attr);

	if (is_flow_meter_action(attr)) {
		err = mlx5e_tc_add_flow_meter(priv, attr);
		if (err)
			return ERR_PTR(err);
	}

	return mlx5_eswitch_add_offloaded_rule(esw, spec, attr);
}

void
mlx5e_tc_rule_unoffload(struct mlx5e_priv *priv,
			struct mlx5_flow_handle *rule,
			struct mlx5_flow_attr *attr)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;

	if (!is_mdev_switchdev_mode(priv->mdev)) {
		mlx5e_del_offloaded_nic_rule(priv, rule, attr);
		return;
	}

	if (attr->flags & MLX5_ATTR_FLAG_SAMPLE) {
		mlx5e_tc_sample_unoffload(get_sample_priv(priv), rule, attr);
		return;
	}

	mlx5_eswitch_del_offloaded_rule(esw, rule, attr);

	if (attr->meter_attr.meter)
		mlx5e_tc_del_flow_meter(esw, attr);
}

int
mlx5e_tc_match_to_reg_set(struct mlx5_core_dev *mdev,
			  struct mlx5e_tc_mod_hdr_acts *mod_hdr_acts,
			  enum mlx5_flow_namespace_type ns,
			  enum mlx5e_tc_attr_to_reg type,
			  u32 data)
{
	int ret = mlx5e_tc_match_to_reg_set_and_get_id(mdev, mod_hdr_acts, ns, type, data);

	return ret < 0 ? ret : 0;
}

void mlx5e_tc_match_to_reg_mod_hdr_change(struct mlx5_core_dev *mdev,
					  struct mlx5e_tc_mod_hdr_acts *mod_hdr_acts,
					  enum mlx5e_tc_attr_to_reg type,
					  int act_id, u32 data)
{
	int moffset = mlx5e_tc_attr_to_reg_mappings[type].moffset;
	int mfield = mlx5e_tc_attr_to_reg_mappings[type].mfield;
	int mlen = mlx5e_tc_attr_to_reg_mappings[type].mlen;
	char *modact;

	modact = mlx5e_mod_hdr_get_item(mod_hdr_acts, act_id);

	/* Firmware has 5bit length field and 0 means 32bits */
	if (mlen == 32)
		mlen = 0;

	MLX5_SET(set_action_in, modact, action_type, MLX5_ACTION_TYPE_SET);
	MLX5_SET(set_action_in, modact, field, mfield);
	MLX5_SET(set_action_in, modact, offset, moffset);
	MLX5_SET(set_action_in, modact, length, mlen);
	MLX5_SET(set_action_in, modact, data, data);
}

struct mlx5e_hairpin {
	struct mlx5_hairpin *pair;

	struct mlx5_core_dev *func_mdev;
	struct mlx5e_priv *func_priv;
	u32 tdn;
	struct mlx5e_tir direct_tir;

	int num_channels;
	u8 log_num_packets;
	struct mlx5e_rqt indir_rqt;
	struct mlx5e_tir indir_tir[MLX5E_NUM_INDIR_TIRS];
	struct mlx5_ttc_table *ttc;
};

struct mlx5e_hairpin_entry {
	/* a node of a hash table which keeps all the  hairpin entries */
	struct hlist_node hairpin_hlist;

	/* protects flows list */
	spinlock_t flows_lock;
	/* flows sharing the same hairpin */
	struct list_head flows;
	/* hpe's that were not fully initialized when dead peer update event
	 * function traversed them.
	 */
	struct list_head dead_peer_wait_list;

	u16 peer_vhca_id;
	u8 prio;
	struct mlx5e_hairpin *hp;
	refcount_t refcnt;
	struct completion res_ready;
};

static void mlx5e_tc_del_flow(struct mlx5e_priv *priv,
			      struct mlx5e_tc_flow *flow);

struct mlx5e_tc_flow *mlx5e_flow_get(struct mlx5e_tc_flow *flow)
{
	if (!flow || !refcount_inc_not_zero(&flow->refcnt))
		return ERR_PTR(-EINVAL);
	return flow;
}

void mlx5e_flow_put(struct mlx5e_priv *priv, struct mlx5e_tc_flow *flow)
{
	if (refcount_dec_and_test(&flow->refcnt)) {
		mlx5e_tc_del_flow(priv, flow);
		kfree_rcu(flow, rcu_head);
	}
}

bool mlx5e_is_eswitch_flow(struct mlx5e_tc_flow *flow)
{
	return flow_flag_test(flow, ESWITCH);
}

bool mlx5e_is_ft_flow(struct mlx5e_tc_flow *flow)
{
	return flow_flag_test(flow, FT);
}

bool mlx5e_is_offloaded_flow(struct mlx5e_tc_flow *flow)
{
	return flow_flag_test(flow, OFFLOADED);
}

int mlx5e_get_flow_namespace(struct mlx5e_tc_flow *flow)
{
	return mlx5e_is_eswitch_flow(flow) ?
		MLX5_FLOW_NAMESPACE_FDB : MLX5_FLOW_NAMESPACE_KERNEL;
}

static struct mlx5_core_dev *
get_flow_counter_dev(struct mlx5e_tc_flow *flow)
{
	return mlx5e_is_eswitch_flow(flow) ? flow->attr->esw_attr->counter_dev : flow->priv->mdev;
}

static struct mod_hdr_tbl *
get_mod_hdr_table(struct mlx5e_priv *priv, struct mlx5e_tc_flow *flow)
{
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;

	return mlx5e_get_flow_namespace(flow) == MLX5_FLOW_NAMESPACE_FDB ?
		&esw->offloads.mod_hdr :
		&tc->mod_hdr;
}

int mlx5e_tc_attach_mod_hdr(struct mlx5e_priv *priv,
			    struct mlx5e_tc_flow *flow,
			    struct mlx5_flow_attr *attr)
{
	struct mlx5e_mod_hdr_handle *mh;

	mh = mlx5e_mod_hdr_attach(priv->mdev, get_mod_hdr_table(priv, flow),
				  mlx5e_get_flow_namespace(flow),
				  &attr->parse_attr->mod_hdr_acts);
	if (IS_ERR(mh))
		return PTR_ERR(mh);

	WARN_ON(attr->modify_hdr);
	attr->modify_hdr = mlx5e_mod_hdr_get(mh);
	attr->mh = mh;

	return 0;
}

void mlx5e_tc_detach_mod_hdr(struct mlx5e_priv *priv,
			     struct mlx5e_tc_flow *flow,
			     struct mlx5_flow_attr *attr)
{
	/* flow wasn't fully initialized */
	if (!attr->mh)
		return;

	mlx5e_mod_hdr_detach(priv->mdev, get_mod_hdr_table(priv, flow),
			     attr->mh);
	attr->mh = NULL;
}

static
struct mlx5_core_dev *mlx5e_hairpin_get_mdev(struct net *net, int ifindex)
{
	struct mlx5_core_dev *mdev;
	struct net_device *netdev;
	struct mlx5e_priv *priv;

	netdev = dev_get_by_index(net, ifindex);
	if (!netdev)
		return ERR_PTR(-ENODEV);

	priv = netdev_priv(netdev);
	mdev = priv->mdev;
	dev_put(netdev);

	/* Mirred tc action holds a refcount on the ifindex net_device (see
	 * net/sched/act_mirred.c:tcf_mirred_get_dev). So, it's okay to continue using mdev
	 * after dev_put(netdev), while we're in the context of adding a tc flow.
	 *
	 * The mdev pointer corresponds to the peer/out net_device of a hairpin. It is then
	 * stored in a hairpin object, which exists until all flows, that refer to it, get
	 * removed.
	 *
	 * On the other hand, after a hairpin object has been created, the peer net_device may
	 * be removed/unbound while there are still some hairpin flows that are using it. This
	 * case is handled by mlx5e_tc_hairpin_update_dead_peer, which is hooked to
	 * NETDEV_UNREGISTER event of the peer net_device.
	 */
	return mdev;
}

static int mlx5e_hairpin_create_transport(struct mlx5e_hairpin *hp)
{
	struct mlx5e_tir_builder *builder;
	int err;

	builder = mlx5e_tir_builder_alloc(false);
	if (!builder)
		return -ENOMEM;

	err = mlx5_core_alloc_transport_domain(hp->func_mdev, &hp->tdn);
	if (err)
		goto out;

	mlx5e_tir_builder_build_inline(builder, hp->tdn, hp->pair->rqn[0]);
	err = mlx5e_tir_init(&hp->direct_tir, builder, hp->func_mdev, false);
	if (err)
		goto create_tir_err;

out:
	mlx5e_tir_builder_free(builder);
	return err;

create_tir_err:
	mlx5_core_dealloc_transport_domain(hp->func_mdev, hp->tdn);

	goto out;
}

static void mlx5e_hairpin_destroy_transport(struct mlx5e_hairpin *hp)
{
	mlx5e_tir_destroy(&hp->direct_tir);
	mlx5_core_dealloc_transport_domain(hp->func_mdev, hp->tdn);
}

static int mlx5e_hairpin_create_indirect_rqt(struct mlx5e_hairpin *hp)
{
	struct mlx5e_priv *priv = hp->func_priv;
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5e_rss_params_indir *indir;
	int err;

	indir = kvmalloc(sizeof(*indir), GFP_KERNEL);
	if (!indir)
		return -ENOMEM;

	mlx5e_rss_params_indir_init_uniform(indir, hp->num_channels);
	err = mlx5e_rqt_init_indir(&hp->indir_rqt, mdev, hp->pair->rqn, hp->num_channels,
				   mlx5e_rx_res_get_current_hash(priv->rx_res).hfunc,
				   indir);

	kvfree(indir);
	return err;
}

static int mlx5e_hairpin_create_indirect_tirs(struct mlx5e_hairpin *hp)
{
	struct mlx5e_priv *priv = hp->func_priv;
	struct mlx5e_rss_params_hash rss_hash;
	enum mlx5_traffic_types tt, max_tt;
	struct mlx5e_tir_builder *builder;
	int err = 0;

	builder = mlx5e_tir_builder_alloc(false);
	if (!builder)
		return -ENOMEM;

	rss_hash = mlx5e_rx_res_get_current_hash(priv->rx_res);

	for (tt = 0; tt < MLX5E_NUM_INDIR_TIRS; tt++) {
		struct mlx5e_rss_params_traffic_type rss_tt;

		rss_tt = mlx5e_rss_get_default_tt_config(tt);

		mlx5e_tir_builder_build_rqt(builder, hp->tdn,
					    mlx5e_rqt_get_rqtn(&hp->indir_rqt),
					    false);
		mlx5e_tir_builder_build_rss(builder, &rss_hash, &rss_tt, false);

		err = mlx5e_tir_init(&hp->indir_tir[tt], builder, hp->func_mdev, false);
		if (err) {
			mlx5_core_warn(hp->func_mdev, "create indirect tirs failed, %d\n", err);
			goto err_destroy_tirs;
		}

		mlx5e_tir_builder_clear(builder);
	}

out:
	mlx5e_tir_builder_free(builder);
	return err;

err_destroy_tirs:
	max_tt = tt;
	for (tt = 0; tt < max_tt; tt++)
		mlx5e_tir_destroy(&hp->indir_tir[tt]);

	goto out;
}

static void mlx5e_hairpin_destroy_indirect_tirs(struct mlx5e_hairpin *hp)
{
	int tt;

	for (tt = 0; tt < MLX5E_NUM_INDIR_TIRS; tt++)
		mlx5e_tir_destroy(&hp->indir_tir[tt]);
}

static void mlx5e_hairpin_set_ttc_params(struct mlx5e_hairpin *hp,
					 struct ttc_params *ttc_params)
{
	struct mlx5_flow_table_attr *ft_attr = &ttc_params->ft_attr;
	int tt;

	memset(ttc_params, 0, sizeof(*ttc_params));

	ttc_params->ns = mlx5_get_flow_namespace(hp->func_mdev,
						 MLX5_FLOW_NAMESPACE_KERNEL);
	for (tt = 0; tt < MLX5_NUM_TT; tt++) {
		ttc_params->dests[tt].type = MLX5_FLOW_DESTINATION_TYPE_TIR;
		ttc_params->dests[tt].tir_num =
			tt == MLX5_TT_ANY ?
				mlx5e_tir_get_tirn(&hp->direct_tir) :
				mlx5e_tir_get_tirn(&hp->indir_tir[tt]);
	}

	ft_attr->level = MLX5E_TC_TTC_FT_LEVEL;
	ft_attr->prio = MLX5E_TC_PRIO;
}

static int mlx5e_hairpin_rss_init(struct mlx5e_hairpin *hp)
{
	struct mlx5e_priv *priv = hp->func_priv;
	struct ttc_params ttc_params;
	struct mlx5_ttc_table *ttc;
	int err;

	err = mlx5e_hairpin_create_indirect_rqt(hp);
	if (err)
		return err;

	err = mlx5e_hairpin_create_indirect_tirs(hp);
	if (err)
		goto err_create_indirect_tirs;

	mlx5e_hairpin_set_ttc_params(hp, &ttc_params);
	hp->ttc = mlx5_create_ttc_table(priv->mdev, &ttc_params);
	if (IS_ERR(hp->ttc)) {
		err = PTR_ERR(hp->ttc);
		goto err_create_ttc_table;
	}

	ttc = mlx5e_fs_get_ttc(priv->fs, false);
	netdev_dbg(priv->netdev, "add hairpin: using %d channels rss ttc table id %x\n",
		   hp->num_channels,
		   mlx5_get_ttc_flow_table(ttc)->id);

	return 0;

err_create_ttc_table:
	mlx5e_hairpin_destroy_indirect_tirs(hp);
err_create_indirect_tirs:
	mlx5e_rqt_destroy(&hp->indir_rqt);

	return err;
}

static void mlx5e_hairpin_rss_cleanup(struct mlx5e_hairpin *hp)
{
	mlx5_destroy_ttc_table(hp->ttc);
	mlx5e_hairpin_destroy_indirect_tirs(hp);
	mlx5e_rqt_destroy(&hp->indir_rqt);
}

static struct mlx5e_hairpin *
mlx5e_hairpin_create(struct mlx5e_priv *priv, struct mlx5_hairpin_params *params,
		     int peer_ifindex)
{
	struct mlx5_core_dev *func_mdev, *peer_mdev;
	struct mlx5e_hairpin *hp;
	struct mlx5_hairpin *pair;
	int err;

	hp = kzalloc(sizeof(*hp), GFP_KERNEL);
	if (!hp)
		return ERR_PTR(-ENOMEM);

	func_mdev = priv->mdev;
	peer_mdev = mlx5e_hairpin_get_mdev(dev_net(priv->netdev), peer_ifindex);
	if (IS_ERR(peer_mdev)) {
		err = PTR_ERR(peer_mdev);
		goto create_pair_err;
	}

	pair = mlx5_core_hairpin_create(func_mdev, peer_mdev, params);
	if (IS_ERR(pair)) {
		err = PTR_ERR(pair);
		goto create_pair_err;
	}
	hp->pair = pair;
	hp->func_mdev = func_mdev;
	hp->func_priv = priv;
	hp->num_channels = params->num_channels;
	hp->log_num_packets = params->log_num_packets;

	err = mlx5e_hairpin_create_transport(hp);
	if (err)
		goto create_transport_err;

	if (hp->num_channels > 1) {
		err = mlx5e_hairpin_rss_init(hp);
		if (err)
			goto rss_init_err;
	}

	return hp;

rss_init_err:
	mlx5e_hairpin_destroy_transport(hp);
create_transport_err:
	mlx5_core_hairpin_destroy(hp->pair);
create_pair_err:
	kfree(hp);
	return ERR_PTR(err);
}

static void mlx5e_hairpin_destroy(struct mlx5e_hairpin *hp)
{
	if (hp->num_channels > 1)
		mlx5e_hairpin_rss_cleanup(hp);
	mlx5e_hairpin_destroy_transport(hp);
	mlx5_core_hairpin_destroy(hp->pair);
	kvfree(hp);
}

static inline u32 hash_hairpin_info(u16 peer_vhca_id, u8 prio)
{
	return (peer_vhca_id << 16 | prio);
}

static struct mlx5e_hairpin_entry *mlx5e_hairpin_get(struct mlx5e_priv *priv,
						     u16 peer_vhca_id, u8 prio)
{
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);
	struct mlx5e_hairpin_entry *hpe;
	u32 hash_key = hash_hairpin_info(peer_vhca_id, prio);

	hash_for_each_possible(tc->hairpin_tbl, hpe,
			       hairpin_hlist, hash_key) {
		if (hpe->peer_vhca_id == peer_vhca_id && hpe->prio == prio) {
			refcount_inc(&hpe->refcnt);
			return hpe;
		}
	}

	return NULL;
}

static void mlx5e_hairpin_put(struct mlx5e_priv *priv,
			      struct mlx5e_hairpin_entry *hpe)
{
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);
	/* no more hairpin flows for us, release the hairpin pair */
	if (!refcount_dec_and_mutex_lock(&hpe->refcnt, &tc->hairpin_tbl_lock))
		return;
	hash_del(&hpe->hairpin_hlist);
	mutex_unlock(&tc->hairpin_tbl_lock);

	if (!IS_ERR_OR_NULL(hpe->hp)) {
		netdev_dbg(priv->netdev, "del hairpin: peer %s\n",
			   dev_name(hpe->hp->pair->peer_mdev->device));

		mlx5e_hairpin_destroy(hpe->hp);
	}

	WARN_ON(!list_empty(&hpe->flows));
	kfree(hpe);
}

#define UNKNOWN_MATCH_PRIO 8

static int mlx5e_hairpin_get_prio(struct mlx5e_priv *priv,
				  struct mlx5_flow_spec *spec, u8 *match_prio,
				  struct netlink_ext_ack *extack)
{
	void *headers_c, *headers_v;
	u8 prio_val, prio_mask = 0;
	bool vlan_present;

#ifdef CONFIG_MLX5_CORE_EN_DCB
	if (priv->dcbx_dp.trust_state != MLX5_QPTS_TRUST_PCP) {
		NL_SET_ERR_MSG_MOD(extack,
				   "only PCP trust state supported for hairpin");
		return -EOPNOTSUPP;
	}
#endif
	headers_c = MLX5_ADDR_OF(fte_match_param, spec->match_criteria, outer_headers);
	headers_v = MLX5_ADDR_OF(fte_match_param, spec->match_value, outer_headers);

	vlan_present = MLX5_GET(fte_match_set_lyr_2_4, headers_v, cvlan_tag);
	if (vlan_present) {
		prio_mask = MLX5_GET(fte_match_set_lyr_2_4, headers_c, first_prio);
		prio_val = MLX5_GET(fte_match_set_lyr_2_4, headers_v, first_prio);
	}

	if (!vlan_present || !prio_mask) {
		prio_val = UNKNOWN_MATCH_PRIO;
	} else if (prio_mask != 0x7) {
		NL_SET_ERR_MSG_MOD(extack,
				   "masked priority match not supported for hairpin");
		return -EOPNOTSUPP;
	}

	*match_prio = prio_val;
	return 0;
}

static int debugfs_hairpin_num_active_get(void *data, u64 *val)
{
	struct mlx5e_tc_table *tc = data;
	struct mlx5e_hairpin_entry *hpe;
	u32 cnt = 0;
	u32 bkt;

	mutex_lock(&tc->hairpin_tbl_lock);
	hash_for_each(tc->hairpin_tbl, bkt, hpe, hairpin_hlist)
		cnt++;
	mutex_unlock(&tc->hairpin_tbl_lock);

	*val = cnt;

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(fops_hairpin_num_active,
			 debugfs_hairpin_num_active_get, NULL, "%llu\n");

static int debugfs_hairpin_table_dump_show(struct seq_file *file, void *priv)

{
	struct mlx5e_tc_table *tc = file->private;
	struct mlx5e_hairpin_entry *hpe;
	u32 bkt;

	mutex_lock(&tc->hairpin_tbl_lock);
	hash_for_each(tc->hairpin_tbl, bkt, hpe, hairpin_hlist)
		seq_printf(file,
			   "Hairpin peer_vhca_id %u prio %u refcnt %u num_channels %u num_packets %lu\n",
			   hpe->peer_vhca_id, hpe->prio,
			   refcount_read(&hpe->refcnt), hpe->hp->num_channels,
			   BIT(hpe->hp->log_num_packets));
	mutex_unlock(&tc->hairpin_tbl_lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(debugfs_hairpin_table_dump);

static void mlx5e_tc_debugfs_init(struct mlx5e_tc_table *tc,
				  struct dentry *dfs_root)
{
	if (IS_ERR_OR_NULL(dfs_root))
		return;

	tc->dfs_root = debugfs_create_dir("tc", dfs_root);

	debugfs_create_file("hairpin_num_active", 0444, tc->dfs_root, tc,
			    &fops_hairpin_num_active);
	debugfs_create_file("hairpin_table_dump", 0444, tc->dfs_root, tc,
			    &debugfs_hairpin_table_dump_fops);
}

static int mlx5e_hairpin_flow_add(struct mlx5e_priv *priv,
				  struct mlx5e_tc_flow *flow,
				  struct mlx5e_tc_flow_parse_attr *parse_attr,
				  struct netlink_ext_ack *extack)
{
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);
	struct devlink *devlink = priv_to_devlink(priv->mdev);
	int peer_ifindex = parse_attr->mirred_ifindex[0];
	union devlink_param_value val = {};
	struct mlx5_hairpin_params params;
	struct mlx5_core_dev *peer_mdev;
	struct mlx5e_hairpin_entry *hpe;
	struct mlx5e_hairpin *hp;
	u8 match_prio;
	u16 peer_id;
	int err;

	peer_mdev = mlx5e_hairpin_get_mdev(dev_net(priv->netdev), peer_ifindex);
	if (IS_ERR(peer_mdev)) {
		NL_SET_ERR_MSG_MOD(extack, "invalid ifindex of mirred device");
		return PTR_ERR(peer_mdev);
	}

	if (!MLX5_CAP_GEN(priv->mdev, hairpin) || !MLX5_CAP_GEN(peer_mdev, hairpin)) {
		NL_SET_ERR_MSG_MOD(extack, "hairpin is not supported");
		return -EOPNOTSUPP;
	}

	peer_id = MLX5_CAP_GEN(peer_mdev, vhca_id);
	err = mlx5e_hairpin_get_prio(priv, &parse_attr->spec, &match_prio,
				     extack);
	if (err)
		return err;

	mutex_lock(&tc->hairpin_tbl_lock);
	hpe = mlx5e_hairpin_get(priv, peer_id, match_prio);
	if (hpe) {
		mutex_unlock(&tc->hairpin_tbl_lock);
		wait_for_completion(&hpe->res_ready);

		if (IS_ERR(hpe->hp)) {
			err = -EREMOTEIO;
			goto out_err;
		}
		goto attach_flow;
	}

	hpe = kzalloc(sizeof(*hpe), GFP_KERNEL);
	if (!hpe) {
		mutex_unlock(&tc->hairpin_tbl_lock);
		return -ENOMEM;
	}

	spin_lock_init(&hpe->flows_lock);
	INIT_LIST_HEAD(&hpe->flows);
	INIT_LIST_HEAD(&hpe->dead_peer_wait_list);
	hpe->peer_vhca_id = peer_id;
	hpe->prio = match_prio;
	refcount_set(&hpe->refcnt, 1);
	init_completion(&hpe->res_ready);

	hash_add(tc->hairpin_tbl, &hpe->hairpin_hlist,
		 hash_hairpin_info(peer_id, match_prio));
	mutex_unlock(&tc->hairpin_tbl_lock);

	err = devl_param_driverinit_value_get(
		devlink, MLX5_DEVLINK_PARAM_ID_HAIRPIN_QUEUE_SIZE, &val);
	if (err) {
		err = -ENOMEM;
		goto out_err;
	}

	params.log_num_packets = ilog2(val.vu32);
	params.log_data_size =
		clamp_t(u32,
			params.log_num_packets +
				MLX5_MPWRQ_MIN_LOG_STRIDE_SZ(priv->mdev),
			MLX5_CAP_GEN(priv->mdev, log_min_hairpin_wq_data_sz),
			MLX5_CAP_GEN(priv->mdev, log_max_hairpin_wq_data_sz));

	params.q_counter = priv->q_counter;
	err = devl_param_driverinit_value_get(
		devlink, MLX5_DEVLINK_PARAM_ID_HAIRPIN_NUM_QUEUES, &val);
	if (err) {
		err = -ENOMEM;
		goto out_err;
	}

	params.num_channels = val.vu32;

	hp = mlx5e_hairpin_create(priv, &params, peer_ifindex);
	hpe->hp = hp;
	complete_all(&hpe->res_ready);
	if (IS_ERR(hp)) {
		err = PTR_ERR(hp);
		goto out_err;
	}

	netdev_dbg(priv->netdev, "add hairpin: tirn %x rqn %x peer %s sqn %x prio %d (log) data %d packets %d\n",
		   mlx5e_tir_get_tirn(&hp->direct_tir), hp->pair->rqn[0],
		   dev_name(hp->pair->peer_mdev->device),
		   hp->pair->sqn[0], match_prio, params.log_data_size, params.log_num_packets);

attach_flow:
	if (hpe->hp->num_channels > 1) {
		flow_flag_set(flow, HAIRPIN_RSS);
		flow->attr->nic_attr->hairpin_ft =
			mlx5_get_ttc_flow_table(hpe->hp->ttc);
	} else {
		flow->attr->nic_attr->hairpin_tirn = mlx5e_tir_get_tirn(&hpe->hp->direct_tir);
	}

	flow->hpe = hpe;
	spin_lock(&hpe->flows_lock);
	list_add(&flow->hairpin, &hpe->flows);
	spin_unlock(&hpe->flows_lock);

	return 0;

out_err:
	mlx5e_hairpin_put(priv, hpe);
	return err;
}

static void mlx5e_hairpin_flow_del(struct mlx5e_priv *priv,
				   struct mlx5e_tc_flow *flow)
{
	/* flow wasn't fully initialized */
	if (!flow->hpe)
		return;

	spin_lock(&flow->hpe->flows_lock);
	list_del(&flow->hairpin);
	spin_unlock(&flow->hpe->flows_lock);

	mlx5e_hairpin_put(priv, flow->hpe);
	flow->hpe = NULL;
}

struct mlx5_flow_handle *
mlx5e_add_offloaded_nic_rule(struct mlx5e_priv *priv,
			     struct mlx5_flow_spec *spec,
			     struct mlx5_flow_attr *attr)
{
	struct mlx5_flow_context *flow_context = &spec->flow_context;
	struct mlx5e_vlan_table *vlan = mlx5e_fs_get_vlan(priv->fs);
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);
	struct mlx5_nic_flow_attr *nic_attr = attr->nic_attr;
	struct mlx5_flow_destination dest[2] = {};
	struct mlx5_fs_chains *nic_chains;
	struct mlx5_flow_act flow_act = {
		.action = attr->action,
		.flags    = FLOW_ACT_NO_APPEND,
	};
	struct mlx5_flow_handle *rule;
	struct mlx5_flow_table *ft;
	int dest_ix = 0;

	nic_chains = mlx5e_nic_chains(tc);
	flow_context->flags |= FLOW_CONTEXT_HAS_TAG;
	flow_context->flow_tag = nic_attr->flow_tag;

	if (attr->dest_ft) {
		dest[dest_ix].type = MLX5_FLOW_DESTINATION_TYPE_FLOW_TABLE;
		dest[dest_ix].ft = attr->dest_ft;
		dest_ix++;
	} else if (nic_attr->hairpin_ft) {
		dest[dest_ix].type = MLX5_FLOW_DESTINATION_TYPE_FLOW_TABLE;
		dest[dest_ix].ft = nic_attr->hairpin_ft;
		dest_ix++;
	} else if (nic_attr->hairpin_tirn) {
		dest[dest_ix].type = MLX5_FLOW_DESTINATION_TYPE_TIR;
		dest[dest_ix].tir_num = nic_attr->hairpin_tirn;
		dest_ix++;
	} else if (attr->action & MLX5_FLOW_CONTEXT_ACTION_FWD_DEST) {
		dest[dest_ix].type = MLX5_FLOW_DESTINATION_TYPE_FLOW_TABLE;
		if (attr->dest_chain) {
			dest[dest_ix].ft = mlx5_chains_get_table(nic_chains,
								 attr->dest_chain, 1,
								 MLX5E_TC_FT_LEVEL);
			if (IS_ERR(dest[dest_ix].ft))
				return ERR_CAST(dest[dest_ix].ft);
		} else {
			dest[dest_ix].ft = mlx5e_vlan_get_flowtable(vlan);
		}
		dest_ix++;
	}

	if (dest[0].type == MLX5_FLOW_DESTINATION_TYPE_FLOW_TABLE &&
	    MLX5_CAP_FLOWTABLE_NIC_RX(priv->mdev, ignore_flow_level))
		flow_act.flags |= FLOW_ACT_IGNORE_FLOW_LEVEL;

	if (flow_act.action & MLX5_FLOW_CONTEXT_ACTION_COUNT) {
		dest[dest_ix].type = MLX5_FLOW_DESTINATION_TYPE_COUNTER;
		dest[dest_ix].counter_id = mlx5_fc_id(attr->counter);
		dest_ix++;
	}

	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_MOD_HDR)
		flow_act.modify_hdr = attr->modify_hdr;

	mutex_lock(&tc->t_lock);
	if (IS_ERR_OR_NULL(tc->t)) {
		/* Create the root table here if doesn't exist yet */
		tc->t =
			mlx5_chains_get_table(nic_chains, 0, 1, MLX5E_TC_FT_LEVEL);

		if (IS_ERR(tc->t)) {
			mutex_unlock(&tc->t_lock);
			netdev_err(priv->netdev,
				   "Failed to create tc offload table\n");
			rule = ERR_CAST(tc->t);
			goto err_ft_get;
		}
	}
	mutex_unlock(&tc->t_lock);

	if (attr->chain || attr->prio)
		ft = mlx5_chains_get_table(nic_chains,
					   attr->chain, attr->prio,
					   MLX5E_TC_FT_LEVEL);
	else
		ft = attr->ft;

	if (IS_ERR(ft)) {
		rule = ERR_CAST(ft);
		goto err_ft_get;
	}

	if (attr->outer_match_level != MLX5_MATCH_NONE)
		spec->match_criteria_enable |= MLX5_MATCH_OUTER_HEADERS;

	rule = mlx5_add_flow_rules(ft, spec,
				   &flow_act, dest, dest_ix);
	if (IS_ERR(rule))
		goto err_rule;

	return rule;

err_rule:
	if (attr->chain || attr->prio)
		mlx5_chains_put_table(nic_chains,
				      attr->chain, attr->prio,
				      MLX5E_TC_FT_LEVEL);
err_ft_get:
	if (attr->dest_chain)
		mlx5_chains_put_table(nic_chains,
				      attr->dest_chain, 1,
				      MLX5E_TC_FT_LEVEL);

	return ERR_CAST(rule);
}

static int
alloc_flow_attr_counter(struct mlx5_core_dev *counter_dev,
			struct mlx5_flow_attr *attr)

{
	struct mlx5_fc *counter;

	counter = mlx5_fc_create(counter_dev, true);
	if (IS_ERR(counter))
		return PTR_ERR(counter);

	attr->counter = counter;
	return 0;
}

static int
mlx5e_tc_add_nic_flow(struct mlx5e_priv *priv,
		      struct mlx5e_tc_flow *flow,
		      struct netlink_ext_ack *extack)
{
	struct mlx5e_tc_flow_parse_attr *parse_attr;
	struct mlx5_flow_attr *attr = flow->attr;
	struct mlx5_core_dev *dev = priv->mdev;
	int err;

	parse_attr = attr->parse_attr;

	if (flow_flag_test(flow, HAIRPIN)) {
		err = mlx5e_hairpin_flow_add(priv, flow, parse_attr, extack);
		if (err)
			return err;
	}

	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_COUNT) {
		err = alloc_flow_attr_counter(dev, attr);
		if (err)
			return err;
	}

	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_MOD_HDR) {
		err = mlx5e_tc_attach_mod_hdr(priv, flow, attr);
		if (err)
			return err;
	}

	flow->rule[0] = mlx5e_add_offloaded_nic_rule(priv, &parse_attr->spec, attr);
	return PTR_ERR_OR_ZERO(flow->rule[0]);
}

void mlx5e_del_offloaded_nic_rule(struct mlx5e_priv *priv,
				  struct mlx5_flow_handle *rule,
				  struct mlx5_flow_attr *attr)
{
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);
	struct mlx5_fs_chains *nic_chains;

	nic_chains = mlx5e_nic_chains(tc);
	mlx5_del_flow_rules(rule);

	if (attr->chain || attr->prio)
		mlx5_chains_put_table(nic_chains, attr->chain, attr->prio,
				      MLX5E_TC_FT_LEVEL);

	if (attr->dest_chain)
		mlx5_chains_put_table(nic_chains, attr->dest_chain, 1,
				      MLX5E_TC_FT_LEVEL);
}

static void mlx5e_tc_del_nic_flow(struct mlx5e_priv *priv,
				  struct mlx5e_tc_flow *flow)
{
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);
	struct mlx5_flow_attr *attr = flow->attr;

	flow_flag_clear(flow, OFFLOADED);

	if (!IS_ERR_OR_NULL(flow->rule[0]))
		mlx5e_del_offloaded_nic_rule(priv, flow->rule[0], attr);

	/* Remove root table if no rules are left to avoid
	 * extra steering hops.
	 */
	mutex_lock(&tc->t_lock);
	if (!mlx5e_tc_num_filters(priv, MLX5_TC_FLAG(NIC_OFFLOAD)) &&
	    !IS_ERR_OR_NULL(tc->t)) {
		mlx5_chains_put_table(mlx5e_nic_chains(tc), 0, 1, MLX5E_TC_FT_LEVEL);
		tc->t = NULL;
	}
	mutex_unlock(&tc->t_lock);

	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_MOD_HDR) {
		mlx5e_mod_hdr_dealloc(&attr->parse_attr->mod_hdr_acts);
		mlx5e_tc_detach_mod_hdr(priv, flow, attr);
	}

	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_COUNT)
		mlx5_fc_destroy(priv->mdev, attr->counter);

	if (flow_flag_test(flow, HAIRPIN))
		mlx5e_hairpin_flow_del(priv, flow);

	free_flow_post_acts(flow);
	mlx5_tc_ct_delete_flow(get_ct_priv(flow->priv), attr);

	kvfree(attr->parse_attr);
	kfree(flow->attr);
}

struct mlx5_flow_handle *
mlx5e_tc_offload_fdb_rules(struct mlx5_eswitch *esw,
			   struct mlx5e_tc_flow *flow,
			   struct mlx5_flow_spec *spec,
			   struct mlx5_flow_attr *attr)
{
	struct mlx5_flow_handle *rule;

	if (attr->flags & MLX5_ATTR_FLAG_SLOW_PATH)
		return mlx5_eswitch_add_offloaded_rule(esw, spec, attr);

	rule = mlx5e_tc_rule_offload(flow->priv, spec, attr);

	if (IS_ERR(rule))
		return rule;

	if (attr->esw_attr->split_count) {
		flow->rule[1] = mlx5_eswitch_add_fwd_rule(esw, spec, attr);
		if (IS_ERR(flow->rule[1]))
			goto err_rule1;
	}

	return rule;

err_rule1:
	mlx5e_tc_rule_unoffload(flow->priv, rule, attr);
	return flow->rule[1];
}

void mlx5e_tc_unoffload_fdb_rules(struct mlx5_eswitch *esw,
				  struct mlx5e_tc_flow *flow,
				  struct mlx5_flow_attr *attr)
{
	flow_flag_clear(flow, OFFLOADED);

	if (attr->flags & MLX5_ATTR_FLAG_SLOW_PATH)
		return mlx5_eswitch_del_offloaded_rule(esw, flow->rule[0], attr);

	if (attr->esw_attr->split_count)
		mlx5_eswitch_del_fwd_rule(esw, flow->rule[1], attr);

	mlx5e_tc_rule_unoffload(flow->priv, flow->rule[0], attr);
}

struct mlx5_flow_handle *
mlx5e_tc_offload_to_slow_path(struct mlx5_eswitch *esw,
			      struct mlx5e_tc_flow *flow,
			      struct mlx5_flow_spec *spec)
{
	struct mlx5e_tc_mod_hdr_acts mod_acts = {};
	struct mlx5e_mod_hdr_handle *mh = NULL;
	struct mlx5_flow_attr *slow_attr;
	struct mlx5_flow_handle *rule;
	bool fwd_and_modify_cap;
	u32 chain_mapping = 0;
	int err;

	slow_attr = mlx5_alloc_flow_attr(MLX5_FLOW_NAMESPACE_FDB);
	if (!slow_attr)
		return ERR_PTR(-ENOMEM);

	memcpy(slow_attr, flow->attr, ESW_FLOW_ATTR_SZ);
	slow_attr->action = MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;
	slow_attr->esw_attr->split_count = 0;
	slow_attr->flags |= MLX5_ATTR_FLAG_SLOW_PATH;

	fwd_and_modify_cap = MLX5_CAP_ESW_FLOWTABLE((esw)->dev, fdb_modify_header_fwd_to_table);
	if (!fwd_and_modify_cap)
		goto skip_restore;

	err = mlx5_chains_get_chain_mapping(esw_chains(esw), flow->attr->chain, &chain_mapping);
	if (err)
		goto err_get_chain;

	err = mlx5e_tc_match_to_reg_set(esw->dev, &mod_acts, MLX5_FLOW_NAMESPACE_FDB,
					MAPPED_OBJ_TO_REG, chain_mapping);
	if (err)
		goto err_reg_set;

	mh = mlx5e_mod_hdr_attach(esw->dev, get_mod_hdr_table(flow->priv, flow),
				  MLX5_FLOW_NAMESPACE_FDB, &mod_acts);
	if (IS_ERR(mh)) {
		err = PTR_ERR(mh);
		goto err_attach;
	}

	slow_attr->action |= MLX5_FLOW_CONTEXT_ACTION_MOD_HDR;
	slow_attr->modify_hdr = mlx5e_mod_hdr_get(mh);

skip_restore:
	rule = mlx5e_tc_offload_fdb_rules(esw, flow, spec, slow_attr);
	if (IS_ERR(rule)) {
		err = PTR_ERR(rule);
		goto err_offload;
	}

	flow->attr->slow_mh = mh;
	flow->chain_mapping = chain_mapping;
	flow_flag_set(flow, SLOW);

	mlx5e_mod_hdr_dealloc(&mod_acts);
	kfree(slow_attr);

	return rule;

err_offload:
	if (fwd_and_modify_cap)
		mlx5e_mod_hdr_detach(esw->dev, get_mod_hdr_table(flow->priv, flow), mh);
err_attach:
err_reg_set:
	if (fwd_and_modify_cap)
		mlx5_chains_put_chain_mapping(esw_chains(esw), chain_mapping);
err_get_chain:
	mlx5e_mod_hdr_dealloc(&mod_acts);
	kfree(slow_attr);
	return ERR_PTR(err);
}

void mlx5e_tc_unoffload_from_slow_path(struct mlx5_eswitch *esw,
				       struct mlx5e_tc_flow *flow)
{
	struct mlx5e_mod_hdr_handle *slow_mh = flow->attr->slow_mh;
	struct mlx5_flow_attr *slow_attr;

	slow_attr = mlx5_alloc_flow_attr(MLX5_FLOW_NAMESPACE_FDB);
	if (!slow_attr) {
		mlx5_core_warn(flow->priv->mdev, "Unable to alloc attr to unoffload slow path rule\n");
		return;
	}

	memcpy(slow_attr, flow->attr, ESW_FLOW_ATTR_SZ);
	slow_attr->action = MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;
	slow_attr->esw_attr->split_count = 0;
	slow_attr->flags |= MLX5_ATTR_FLAG_SLOW_PATH;
	if (slow_mh) {
		slow_attr->action |= MLX5_FLOW_CONTEXT_ACTION_MOD_HDR;
		slow_attr->modify_hdr = mlx5e_mod_hdr_get(slow_mh);
	}
	mlx5e_tc_unoffload_fdb_rules(esw, flow, slow_attr);
	if (slow_mh) {
		mlx5e_mod_hdr_detach(esw->dev, get_mod_hdr_table(flow->priv, flow), slow_mh);
		mlx5_chains_put_chain_mapping(esw_chains(esw), flow->chain_mapping);
		flow->chain_mapping = 0;
		flow->attr->slow_mh = NULL;
	}
	flow_flag_clear(flow, SLOW);
	kfree(slow_attr);
}

/* Caller must obtain uplink_priv->unready_flows_lock mutex before calling this
 * function.
 */
static void unready_flow_add(struct mlx5e_tc_flow *flow,
			     struct list_head *unready_flows)
{
	flow_flag_set(flow, NOT_READY);
	list_add_tail(&flow->unready, unready_flows);
}

/* Caller must obtain uplink_priv->unready_flows_lock mutex before calling this
 * function.
 */
static void unready_flow_del(struct mlx5e_tc_flow *flow)
{
	list_del(&flow->unready);
	flow_flag_clear(flow, NOT_READY);
}

static void add_unready_flow(struct mlx5e_tc_flow *flow)
{
	struct mlx5_rep_uplink_priv *uplink_priv;
	struct mlx5e_rep_priv *rpriv;
	struct mlx5_eswitch *esw;

	esw = flow->priv->mdev->priv.eswitch;
	rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
	uplink_priv = &rpriv->uplink_priv;

	mutex_lock(&uplink_priv->unready_flows_lock);
	unready_flow_add(flow, &uplink_priv->unready_flows);
	mutex_unlock(&uplink_priv->unready_flows_lock);
}

static void remove_unready_flow(struct mlx5e_tc_flow *flow)
{
	struct mlx5_rep_uplink_priv *uplink_priv;
	struct mlx5e_rep_priv *rpriv;
	struct mlx5_eswitch *esw;

	esw = flow->priv->mdev->priv.eswitch;
	rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
	uplink_priv = &rpriv->uplink_priv;

	mutex_lock(&uplink_priv->unready_flows_lock);
	if (flow_flag_test(flow, NOT_READY))
		unready_flow_del(flow);
	mutex_unlock(&uplink_priv->unready_flows_lock);
}

bool mlx5e_tc_is_vf_tunnel(struct net_device *out_dev, struct net_device *route_dev)
{
	struct mlx5_core_dev *out_mdev, *route_mdev;
	struct mlx5e_priv *out_priv, *route_priv;

	out_priv = netdev_priv(out_dev);
	out_mdev = out_priv->mdev;
	route_priv = netdev_priv(route_dev);
	route_mdev = route_priv->mdev;

	if (out_mdev->coredev_type != MLX5_COREDEV_PF)
		return false;

	if (route_mdev->coredev_type != MLX5_COREDEV_VF &&
	    route_mdev->coredev_type != MLX5_COREDEV_SF)
		return false;

	return mlx5e_same_hw_devs(out_priv, route_priv);
}

int mlx5e_tc_query_route_vport(struct net_device *out_dev, struct net_device *route_dev, u16 *vport)
{
	struct mlx5e_priv *out_priv, *route_priv;
	struct mlx5_core_dev *route_mdev;
	struct mlx5_devcom_comp_dev *pos;
	struct mlx5_eswitch *esw;
	u16 vhca_id;
	int err;

	out_priv = netdev_priv(out_dev);
	esw = out_priv->mdev->priv.eswitch;
	route_priv = netdev_priv(route_dev);
	route_mdev = route_priv->mdev;

	vhca_id = MLX5_CAP_GEN(route_mdev, vhca_id);
	err = mlx5_eswitch_vhca_id_to_vport(esw, vhca_id, vport);
	if (!err)
		return err;

	if (!mlx5_lag_is_active(out_priv->mdev))
		return err;

	rcu_read_lock();
	err = -ENODEV;
	mlx5_devcom_for_each_peer_entry_rcu(esw->devcom, esw, pos) {
		err = mlx5_eswitch_vhca_id_to_vport(esw, vhca_id, vport);
		if (!err)
			break;
	}
	rcu_read_unlock();

	return err;
}

static int
verify_attr_actions(u32 actions, struct netlink_ext_ack *extack)
{
	if (!(actions &
	      (MLX5_FLOW_CONTEXT_ACTION_FWD_DEST | MLX5_FLOW_CONTEXT_ACTION_DROP))) {
		NL_SET_ERR_MSG_MOD(extack, "Rule must have at least one forward/drop action");
		return -EOPNOTSUPP;
	}

	if (!(~actions &
	      (MLX5_FLOW_CONTEXT_ACTION_FWD_DEST | MLX5_FLOW_CONTEXT_ACTION_DROP))) {
		NL_SET_ERR_MSG_MOD(extack, "Rule cannot support forward+drop action");
		return -EOPNOTSUPP;
	}

	if (actions & MLX5_FLOW_CONTEXT_ACTION_MOD_HDR &&
	    actions & MLX5_FLOW_CONTEXT_ACTION_DROP) {
		NL_SET_ERR_MSG_MOD(extack, "Drop with modify header action is not supported");
		return -EOPNOTSUPP;
	}

	return 0;
}

static bool
has_encap_dests(struct mlx5_flow_attr *attr)
{
	struct mlx5_esw_flow_attr *esw_attr = attr->esw_attr;
	int out_index;

	for (out_index = 0; out_index < MLX5_MAX_FLOW_FWD_VPORTS; out_index++)
		if (esw_attr->dests[out_index].flags & MLX5_ESW_DEST_ENCAP)
			return true;

	return false;
}

static int
post_process_attr(struct mlx5e_tc_flow *flow,
		  struct mlx5_flow_attr *attr,
		  struct netlink_ext_ack *extack)
{
	bool vf_tun;
	int err = 0;

	err = verify_attr_actions(attr->action, extack);
	if (err)
		goto err_out;

	if (mlx5e_is_eswitch_flow(flow) && has_encap_dests(attr)) {
		err = mlx5e_tc_tun_encap_dests_set(flow->priv, flow, attr, extack, &vf_tun);
		if (err)
			goto err_out;
	}

	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_MOD_HDR) {
		err = mlx5e_tc_attach_mod_hdr(flow->priv, flow, attr);
		if (err)
			goto err_out;
	}

	if (attr->branch_true &&
	    attr->branch_true->action & MLX5_FLOW_CONTEXT_ACTION_MOD_HDR) {
		err = mlx5e_tc_attach_mod_hdr(flow->priv, flow, attr->branch_true);
		if (err)
			goto err_out;
	}

	if (attr->branch_false &&
	    attr->branch_false->action & MLX5_FLOW_CONTEXT_ACTION_MOD_HDR) {
		err = mlx5e_tc_attach_mod_hdr(flow->priv, flow, attr->branch_false);
		if (err)
			goto err_out;
	}

	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_COUNT) {
		err = alloc_flow_attr_counter(get_flow_counter_dev(flow), attr);
		if (err)
			goto err_out;
	}

err_out:
	return err;
}

static int
mlx5e_tc_add_fdb_flow(struct mlx5e_priv *priv,
		      struct mlx5e_tc_flow *flow,
		      struct netlink_ext_ack *extack)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5e_tc_flow_parse_attr *parse_attr;
	struct mlx5_flow_attr *attr = flow->attr;
	struct mlx5_esw_flow_attr *esw_attr;
	u32 max_prio, max_chain;
	int err = 0;

	parse_attr = attr->parse_attr;
	esw_attr = attr->esw_attr;

	/* We check chain range only for tc flows.
	 * For ft flows, we checked attr->chain was originally 0 and set it to
	 * FDB_FT_CHAIN which is outside tc range.
	 * See mlx5e_rep_setup_ft_cb().
	 */
	max_chain = mlx5_chains_get_chain_range(esw_chains(esw));
	if (!mlx5e_is_ft_flow(flow) && attr->chain > max_chain) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Requested chain is out of supported range");
		err = -EOPNOTSUPP;
		goto err_out;
	}

	max_prio = mlx5_chains_get_prio_range(esw_chains(esw));
	if (attr->prio > max_prio) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Requested priority is out of supported range");
		err = -EOPNOTSUPP;
		goto err_out;
	}

	if (flow_flag_test(flow, TUN_RX)) {
		err = mlx5e_attach_decap_route(priv, flow);
		if (err)
			goto err_out;

		if (!attr->chain && esw_attr->int_port &&
		    attr->action & MLX5_FLOW_CONTEXT_ACTION_FWD_DEST) {
			/* If decap route device is internal port, change the
			 * source vport value in reg_c0 back to uplink just in
			 * case the rule performs goto chain > 0. If we have a miss
			 * on chain > 0 we want the metadata regs to hold the
			 * chain id so SW will resume handling of this packet
			 * from the proper chain.
			 */
			u32 metadata = mlx5_eswitch_get_vport_metadata_for_set(esw,
									esw_attr->in_rep->vport);

			err = mlx5e_tc_match_to_reg_set(priv->mdev, &parse_attr->mod_hdr_acts,
							MLX5_FLOW_NAMESPACE_FDB, VPORT_TO_REG,
							metadata);
			if (err)
				goto err_out;

			attr->action |= MLX5_FLOW_CONTEXT_ACTION_MOD_HDR;
		}
	}

	if (flow_flag_test(flow, L3_TO_L2_DECAP)) {
		err = mlx5e_attach_decap(priv, flow, extack);
		if (err)
			goto err_out;
	}

	if (netif_is_ovs_master(parse_attr->filter_dev)) {
		struct mlx5e_tc_int_port *int_port;

		if (attr->chain) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Internal port rule is only supported on chain 0");
			err = -EOPNOTSUPP;
			goto err_out;
		}

		if (attr->dest_chain) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Internal port rule offload doesn't support goto action");
			err = -EOPNOTSUPP;
			goto err_out;
		}

		int_port = mlx5e_tc_int_port_get(mlx5e_get_int_port_priv(priv),
						 parse_attr->filter_dev->ifindex,
						 flow_flag_test(flow, EGRESS) ?
						 MLX5E_TC_INT_PORT_EGRESS :
						 MLX5E_TC_INT_PORT_INGRESS);
		if (IS_ERR(int_port)) {
			err = PTR_ERR(int_port);
			goto err_out;
		}

		esw_attr->int_port = int_port;
	}

	err = post_process_attr(flow, attr, extack);
	if (err)
		goto err_out;

	err = mlx5e_tc_act_stats_add_flow(get_act_stats_handle(priv), flow);
	if (err)
		goto err_out;

	/* we get here if one of the following takes place:
	 * (1) there's no error
	 * (2) there's an encap action and we don't have valid neigh
	 */
	if (flow_flag_test(flow, SLOW))
		flow->rule[0] = mlx5e_tc_offload_to_slow_path(esw, flow, &parse_attr->spec);
	else
		flow->rule[0] = mlx5e_tc_offload_fdb_rules(esw, flow, &parse_attr->spec, attr);

	if (IS_ERR(flow->rule[0])) {
		err = PTR_ERR(flow->rule[0]);
		goto err_out;
	}
	flow_flag_set(flow, OFFLOADED);

	return 0;

err_out:
	flow_flag_set(flow, FAILED);
	return err;
}

static bool mlx5_flow_has_geneve_opt(struct mlx5e_tc_flow *flow)
{
	struct mlx5_flow_spec *spec = &flow->attr->parse_attr->spec;
	void *headers_v = MLX5_ADDR_OF(fte_match_param,
				       spec->match_value,
				       misc_parameters_3);
	u32 geneve_tlv_opt_0_data = MLX5_GET(fte_match_set_misc3,
					     headers_v,
					     geneve_tlv_option_0_data);

	return !!geneve_tlv_opt_0_data;
}

static void free_branch_attr(struct mlx5e_tc_flow *flow, struct mlx5_flow_attr *attr)
{
	if (!attr)
		return;

	mlx5_free_flow_attr_actions(flow, attr);
	kvfree(attr->parse_attr);
	kfree(attr);
}

static void mlx5e_tc_del_fdb_flow(struct mlx5e_priv *priv,
				  struct mlx5e_tc_flow *flow)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5_flow_attr *attr = flow->attr;

	mlx5e_put_flow_tunnel_id(flow);

	remove_unready_flow(flow);

	if (mlx5e_is_offloaded_flow(flow)) {
		if (flow_flag_test(flow, SLOW))
			mlx5e_tc_unoffload_from_slow_path(esw, flow);
		else
			mlx5e_tc_unoffload_fdb_rules(esw, flow, attr);
	}
	complete_all(&flow->del_hw_done);

	if (mlx5_flow_has_geneve_opt(flow))
		mlx5_geneve_tlv_option_del(priv->mdev->geneve);

	if (flow->decap_route)
		mlx5e_detach_decap_route(priv, flow);

	mlx5_tc_ct_match_del(get_ct_priv(priv), &flow->attr->ct_attr);

	if (flow_flag_test(flow, L3_TO_L2_DECAP))
		mlx5e_detach_decap(priv, flow);

	mlx5e_tc_act_stats_del_flow(get_act_stats_handle(priv), flow);

	free_flow_post_acts(flow);
	mlx5_free_flow_attr_actions(flow, attr);

	kvfree(attr->esw_attr->rx_tun_attr);
	kvfree(attr->parse_attr);
	kfree(flow->attr);
}

struct mlx5_fc *mlx5e_tc_get_counter(struct mlx5e_tc_flow *flow)
{
	struct mlx5_flow_attr *attr;

	attr = list_first_entry(&flow->attrs, struct mlx5_flow_attr, list);
	return attr->counter;
}

/* Iterate over tmp_list of flows attached to flow_list head. */
void mlx5e_put_flow_list(struct mlx5e_priv *priv, struct list_head *flow_list)
{
	struct mlx5e_tc_flow *flow, *tmp;

	list_for_each_entry_safe(flow, tmp, flow_list, tmp_list)
		mlx5e_flow_put(priv, flow);
}

static void mlx5e_tc_del_fdb_peer_flow(struct mlx5e_tc_flow *flow,
				       int peer_index)
{
	struct mlx5_eswitch *esw = flow->priv->mdev->priv.eswitch;
	struct mlx5e_tc_flow *peer_flow;
	struct mlx5e_tc_flow *tmp;

	if (!flow_flag_test(flow, ESWITCH) ||
	    !flow_flag_test(flow, DUP))
		return;

	mutex_lock(&esw->offloads.peer_mutex);
	list_del(&flow->peer[peer_index]);
	mutex_unlock(&esw->offloads.peer_mutex);

	list_for_each_entry_safe(peer_flow, tmp, &flow->peer_flows, peer_flows) {
		if (peer_index != mlx5_get_dev_index(peer_flow->priv->mdev))
			continue;
		if (refcount_dec_and_test(&peer_flow->refcnt)) {
			mlx5e_tc_del_fdb_flow(peer_flow->priv, peer_flow);
			list_del(&peer_flow->peer_flows);
			kfree(peer_flow);
		}
	}

	if (list_empty(&flow->peer_flows))
		flow_flag_clear(flow, DUP);
}

static void mlx5e_tc_del_fdb_peers_flow(struct mlx5e_tc_flow *flow)
{
	int i;

	for (i = 0; i < MLX5_MAX_PORTS; i++) {
		if (i == mlx5_get_dev_index(flow->priv->mdev))
			continue;
		mlx5e_tc_del_fdb_peer_flow(flow, i);
	}
}

static void mlx5e_tc_del_flow(struct mlx5e_priv *priv,
			      struct mlx5e_tc_flow *flow)
{
	if (mlx5e_is_eswitch_flow(flow)) {
		struct mlx5_devcom_comp_dev *devcom = flow->priv->mdev->priv.eswitch->devcom;

		if (!mlx5_devcom_for_each_peer_begin(devcom)) {
			mlx5e_tc_del_fdb_flow(priv, flow);
			return;
		}

		mlx5e_tc_del_fdb_peers_flow(flow);
		mlx5_devcom_for_each_peer_end(devcom);
		mlx5e_tc_del_fdb_flow(priv, flow);
	} else {
		mlx5e_tc_del_nic_flow(priv, flow);
	}
}

static bool flow_requires_tunnel_mapping(u32 chain, struct flow_cls_offload *f)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct flow_action *flow_action = &rule->action;
	const struct flow_action_entry *act;
	int i;

	if (chain)
		return false;

	flow_action_for_each(i, act, flow_action) {
		switch (act->id) {
		case FLOW_ACTION_GOTO:
			return true;
		case FLOW_ACTION_SAMPLE:
			return true;
		default:
			continue;
		}
	}

	return false;
}

static int
enc_opts_is_dont_care_or_full_match(struct mlx5e_priv *priv,
				    struct flow_dissector_key_enc_opts *opts,
				    struct netlink_ext_ack *extack,
				    bool *dont_care)
{
	struct geneve_opt *opt;
	int off = 0;

	*dont_care = true;

	while (opts->len > off) {
		opt = (struct geneve_opt *)&opts->data[off];

		if (!(*dont_care) || opt->opt_class || opt->type ||
		    memchr_inv(opt->opt_data, 0, opt->length * 4)) {
			*dont_care = false;

			if (opt->opt_class != htons(U16_MAX) ||
			    opt->type != U8_MAX) {
				NL_SET_ERR_MSG_MOD(extack,
						   "Partial match of tunnel options in chain > 0 isn't supported");
				netdev_warn(priv->netdev,
					    "Partial match of tunnel options in chain > 0 isn't supported");
				return -EOPNOTSUPP;
			}
		}

		off += sizeof(struct geneve_opt) + opt->length * 4;
	}

	return 0;
}

#define COPY_DISSECTOR(rule, diss_key, dst)\
({ \
	struct flow_rule *__rule = (rule);\
	typeof(dst) __dst = dst;\
\
	memcpy(__dst,\
	       skb_flow_dissector_target(__rule->match.dissector,\
					 diss_key,\
					 __rule->match.key),\
	       sizeof(*__dst));\
})

static int mlx5e_get_flow_tunnel_id(struct mlx5e_priv *priv,
				    struct mlx5e_tc_flow *flow,
				    struct flow_cls_offload *f,
				    struct net_device *filter_dev)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct netlink_ext_ack *extack = f->common.extack;
	struct mlx5e_tc_mod_hdr_acts *mod_hdr_acts;
	struct flow_match_enc_opts enc_opts_match;
	struct tunnel_match_enc_opts tun_enc_opts;
	struct mlx5_rep_uplink_priv *uplink_priv;
	struct mlx5_flow_attr *attr = flow->attr;
	struct mlx5e_rep_priv *uplink_rpriv;
	struct tunnel_match_key tunnel_key;
	bool enc_opts_is_dont_care = true;
	u32 tun_id, enc_opts_id = 0;
	struct mlx5_eswitch *esw;
	u32 value, mask;
	int err;

	esw = priv->mdev->priv.eswitch;
	uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
	uplink_priv = &uplink_rpriv->uplink_priv;

	memset(&tunnel_key, 0, sizeof(tunnel_key));
	COPY_DISSECTOR(rule, FLOW_DISSECTOR_KEY_ENC_CONTROL,
		       &tunnel_key.enc_control);
	if (tunnel_key.enc_control.addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS)
		COPY_DISSECTOR(rule, FLOW_DISSECTOR_KEY_ENC_IPV4_ADDRS,
			       &tunnel_key.enc_ipv4);
	else
		COPY_DISSECTOR(rule, FLOW_DISSECTOR_KEY_ENC_IPV6_ADDRS,
			       &tunnel_key.enc_ipv6);
	COPY_DISSECTOR(rule, FLOW_DISSECTOR_KEY_ENC_IP, &tunnel_key.enc_ip);
	COPY_DISSECTOR(rule, FLOW_DISSECTOR_KEY_ENC_PORTS,
		       &tunnel_key.enc_tp);
	COPY_DISSECTOR(rule, FLOW_DISSECTOR_KEY_ENC_KEYID,
		       &tunnel_key.enc_key_id);
	tunnel_key.filter_ifindex = filter_dev->ifindex;

	err = mapping_add(uplink_priv->tunnel_mapping, &tunnel_key, &tun_id);
	if (err)
		return err;

	flow_rule_match_enc_opts(rule, &enc_opts_match);
	err = enc_opts_is_dont_care_or_full_match(priv,
						  enc_opts_match.mask,
						  extack,
						  &enc_opts_is_dont_care);
	if (err)
		goto err_enc_opts;

	if (!enc_opts_is_dont_care) {
		memset(&tun_enc_opts, 0, sizeof(tun_enc_opts));
		memcpy(&tun_enc_opts.key, enc_opts_match.key,
		       sizeof(*enc_opts_match.key));
		memcpy(&tun_enc_opts.mask, enc_opts_match.mask,
		       sizeof(*enc_opts_match.mask));

		err = mapping_add(uplink_priv->tunnel_enc_opts_mapping,
				  &tun_enc_opts, &enc_opts_id);
		if (err)
			goto err_enc_opts;
	}

	value = tun_id << ENC_OPTS_BITS | enc_opts_id;
	mask = enc_opts_id ? TUNNEL_ID_MASK :
			     (TUNNEL_ID_MASK & ~ENC_OPTS_BITS_MASK);

	if (attr->chain) {
		mlx5e_tc_match_to_reg_match(&attr->parse_attr->spec,
					    TUNNEL_TO_REG, value, mask);
	} else {
		mod_hdr_acts = &attr->parse_attr->mod_hdr_acts;
		err = mlx5e_tc_match_to_reg_set(priv->mdev,
						mod_hdr_acts, MLX5_FLOW_NAMESPACE_FDB,
						TUNNEL_TO_REG, value);
		if (err)
			goto err_set;

		attr->action |= MLX5_FLOW_CONTEXT_ACTION_MOD_HDR;
	}

	flow->attr->tunnel_id = value;
	return 0;

err_set:
	if (enc_opts_id)
		mapping_remove(uplink_priv->tunnel_enc_opts_mapping,
			       enc_opts_id);
err_enc_opts:
	mapping_remove(uplink_priv->tunnel_mapping, tun_id);
	return err;
}

static void mlx5e_put_flow_tunnel_id(struct mlx5e_tc_flow *flow)
{
	u32 enc_opts_id = flow->attr->tunnel_id & ENC_OPTS_BITS_MASK;
	u32 tun_id = flow->attr->tunnel_id >> ENC_OPTS_BITS;
	struct mlx5_rep_uplink_priv *uplink_priv;
	struct mlx5e_rep_priv *uplink_rpriv;
	struct mlx5_eswitch *esw;

	esw = flow->priv->mdev->priv.eswitch;
	uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
	uplink_priv = &uplink_rpriv->uplink_priv;

	if (tun_id)
		mapping_remove(uplink_priv->tunnel_mapping, tun_id);
	if (enc_opts_id)
		mapping_remove(uplink_priv->tunnel_enc_opts_mapping,
			       enc_opts_id);
}

void mlx5e_tc_set_ethertype(struct mlx5_core_dev *mdev,
			    struct flow_match_basic *match, bool outer,
			    void *headers_c, void *headers_v)
{
	bool ip_version_cap;

	ip_version_cap = outer ?
		MLX5_CAP_FLOWTABLE_NIC_RX(mdev,
					  ft_field_support.outer_ip_version) :
		MLX5_CAP_FLOWTABLE_NIC_RX(mdev,
					  ft_field_support.inner_ip_version);

	if (ip_version_cap && match->mask->n_proto == htons(0xFFFF) &&
	    (match->key->n_proto == htons(ETH_P_IP) ||
	     match->key->n_proto == htons(ETH_P_IPV6))) {
		MLX5_SET_TO_ONES(fte_match_set_lyr_2_4, headers_c, ip_version);
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, ip_version,
			 match->key->n_proto == htons(ETH_P_IP) ? 4 : 6);
	} else {
		MLX5_SET(fte_match_set_lyr_2_4, headers_c, ethertype,
			 ntohs(match->mask->n_proto));
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, ethertype,
			 ntohs(match->key->n_proto));
	}
}

u8 mlx5e_tc_get_ip_version(struct mlx5_flow_spec *spec, bool outer)
{
	void *headers_v;
	u16 ethertype;
	u8 ip_version;

	if (outer)
		headers_v = MLX5_ADDR_OF(fte_match_param, spec->match_value, outer_headers);
	else
		headers_v = MLX5_ADDR_OF(fte_match_param, spec->match_value, inner_headers);

	ip_version = MLX5_GET(fte_match_set_lyr_2_4, headers_v, ip_version);
	/* Return ip_version converted from ethertype anyway */
	if (!ip_version) {
		ethertype = MLX5_GET(fte_match_set_lyr_2_4, headers_v, ethertype);
		if (ethertype == ETH_P_IP || ethertype == ETH_P_ARP)
			ip_version = 4;
		else if (ethertype == ETH_P_IPV6)
			ip_version = 6;
	}
	return ip_version;
}

/* Tunnel device follows RFC 6040, see include/net/inet_ecn.h.
 * And changes inner ip_ecn depending on inner and outer ip_ecn as follows:
 *      +---------+----------------------------------------+
 *      |Arriving |         Arriving Outer Header          |
 *      |   Inner +---------+---------+---------+----------+
 *      |  Header | Not-ECT | ECT(0)  | ECT(1)  |   CE     |
 *      +---------+---------+---------+---------+----------+
 *      | Not-ECT | Not-ECT | Not-ECT | Not-ECT | <drop>   |
 *      |  ECT(0) |  ECT(0) | ECT(0)  | ECT(1)  |   CE*    |
 *      |  ECT(1) |  ECT(1) | ECT(1)  | ECT(1)* |   CE*    |
 *      |    CE   |   CE    |  CE     | CE      |   CE     |
 *      +---------+---------+---------+---------+----------+
 *
 * Tc matches on inner after decapsulation on tunnel device, but hw offload matches
 * the inner ip_ecn value before hardware decap action.
 *
 * Cells marked are changed from original inner packet ip_ecn value during decap, and
 * so matching those values on inner ip_ecn before decap will fail.
 *
 * The following helper allows offload when inner ip_ecn won't be changed by outer ip_ecn,
 * except for the outer ip_ecn = CE, where in all cases inner ip_ecn will be changed to CE,
 * and such we can drop the inner ip_ecn=CE match.
 */

static int mlx5e_tc_verify_tunnel_ecn(struct mlx5e_priv *priv,
				      struct flow_cls_offload *f,
				      bool *match_inner_ecn)
{
	u8 outer_ecn_mask = 0, outer_ecn_key = 0, inner_ecn_mask = 0, inner_ecn_key = 0;
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct netlink_ext_ack *extack = f->common.extack;
	struct flow_match_ip match;

	*match_inner_ecn = true;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_IP)) {
		flow_rule_match_enc_ip(rule, &match);
		outer_ecn_key = match.key->tos & INET_ECN_MASK;
		outer_ecn_mask = match.mask->tos & INET_ECN_MASK;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		flow_rule_match_ip(rule, &match);
		inner_ecn_key = match.key->tos & INET_ECN_MASK;
		inner_ecn_mask = match.mask->tos & INET_ECN_MASK;
	}

	if (outer_ecn_mask != 0 && outer_ecn_mask != INET_ECN_MASK) {
		NL_SET_ERR_MSG_MOD(extack, "Partial match on enc_tos ecn bits isn't supported");
		netdev_warn(priv->netdev, "Partial match on enc_tos ecn bits isn't supported");
		return -EOPNOTSUPP;
	}

	if (!outer_ecn_mask) {
		if (!inner_ecn_mask)
			return 0;

		NL_SET_ERR_MSG_MOD(extack,
				   "Matching on tos ecn bits without also matching enc_tos ecn bits isn't supported");
		netdev_warn(priv->netdev,
			    "Matching on tos ecn bits without also matching enc_tos ecn bits isn't supported");
		return -EOPNOTSUPP;
	}

	if (inner_ecn_mask && inner_ecn_mask != INET_ECN_MASK) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Partial match on tos ecn bits with match on enc_tos ecn bits isn't supported");
		netdev_warn(priv->netdev,
			    "Partial match on tos ecn bits with match on enc_tos ecn bits isn't supported");
		return -EOPNOTSUPP;
	}

	if (!inner_ecn_mask)
		return 0;

	/* Both inner and outer have full mask on ecn */

	if (outer_ecn_key == INET_ECN_ECT_1) {
		/* inner ecn might change by DECAP action */

		NL_SET_ERR_MSG_MOD(extack, "Match on enc_tos ecn = ECT(1) isn't supported");
		netdev_warn(priv->netdev, "Match on enc_tos ecn = ECT(1) isn't supported");
		return -EOPNOTSUPP;
	}

	if (outer_ecn_key != INET_ECN_CE)
		return 0;

	if (inner_ecn_key != INET_ECN_CE) {
		/* Can't happen in software, as packet ecn will be changed to CE after decap */
		NL_SET_ERR_MSG_MOD(extack,
				   "Match on tos enc_tos ecn = CE while match on tos ecn != CE isn't supported");
		netdev_warn(priv->netdev,
			    "Match on tos enc_tos ecn = CE while match on tos ecn != CE isn't supported");
		return -EOPNOTSUPP;
	}

	/* outer ecn = CE, inner ecn = CE, as decap will change inner ecn to CE in anycase,
	 * drop match on inner ecn
	 */
	*match_inner_ecn = false;

	return 0;
}

static int parse_tunnel_attr(struct mlx5e_priv *priv,
			     struct mlx5e_tc_flow *flow,
			     struct mlx5_flow_spec *spec,
			     struct flow_cls_offload *f,
			     struct net_device *filter_dev,
			     u8 *match_level,
			     bool *match_inner)
{
	struct mlx5e_tc_tunnel *tunnel = mlx5e_get_tc_tun(filter_dev);
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct netlink_ext_ack *extack = f->common.extack;
	bool needs_mapping, sets_mapping;
	int err;

	if (!mlx5e_is_eswitch_flow(flow)) {
		NL_SET_ERR_MSG_MOD(extack, "Match on tunnel is not supported");
		return -EOPNOTSUPP;
	}

	needs_mapping = !!flow->attr->chain;
	sets_mapping = flow_requires_tunnel_mapping(flow->attr->chain, f);
	*match_inner = !needs_mapping;

	if ((needs_mapping || sets_mapping) &&
	    !mlx5_eswitch_reg_c1_loopback_enabled(esw)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Chains on tunnel devices isn't supported without register loopback support");
		netdev_warn(priv->netdev,
			    "Chains on tunnel devices isn't supported without register loopback support");
		return -EOPNOTSUPP;
	}

	if (!flow->attr->chain) {
		err = mlx5e_tc_tun_parse(filter_dev, priv, spec, f,
					 match_level);
		if (err) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Failed to parse tunnel attributes");
			netdev_warn(priv->netdev,
				    "Failed to parse tunnel attributes");
			return err;
		}

		/* With mpls over udp we decapsulate using packet reformat
		 * object
		 */
		if (!netif_is_bareudp(filter_dev))
			flow->attr->action |= MLX5_FLOW_CONTEXT_ACTION_DECAP;
		err = mlx5e_tc_set_attr_rx_tun(flow, spec);
		if (err)
			return err;
	} else if (tunnel) {
		struct mlx5_flow_spec *tmp_spec;

		tmp_spec = kvzalloc(sizeof(*tmp_spec), GFP_KERNEL);
		if (!tmp_spec) {
			NL_SET_ERR_MSG_MOD(extack, "Failed to allocate memory for tunnel tmp spec");
			netdev_warn(priv->netdev, "Failed to allocate memory for tunnel tmp spec");
			return -ENOMEM;
		}
		memcpy(tmp_spec, spec, sizeof(*tmp_spec));

		err = mlx5e_tc_tun_parse(filter_dev, priv, tmp_spec, f, match_level);
		if (err) {
			kvfree(tmp_spec);
			NL_SET_ERR_MSG_MOD(extack, "Failed to parse tunnel attributes");
			netdev_warn(priv->netdev, "Failed to parse tunnel attributes");
			return err;
		}
		err = mlx5e_tc_set_attr_rx_tun(flow, tmp_spec);
		kvfree(tmp_spec);
		if (err)
			return err;
	}

	if (!needs_mapping && !sets_mapping)
		return 0;

	return mlx5e_get_flow_tunnel_id(priv, flow, f, filter_dev);
}

static void *get_match_inner_headers_criteria(struct mlx5_flow_spec *spec)
{
	return MLX5_ADDR_OF(fte_match_param, spec->match_criteria,
			    inner_headers);
}

static void *get_match_inner_headers_value(struct mlx5_flow_spec *spec)
{
	return MLX5_ADDR_OF(fte_match_param, spec->match_value,
			    inner_headers);
}

static void *get_match_outer_headers_criteria(struct mlx5_flow_spec *spec)
{
	return MLX5_ADDR_OF(fte_match_param, spec->match_criteria,
			    outer_headers);
}

static void *get_match_outer_headers_value(struct mlx5_flow_spec *spec)
{
	return MLX5_ADDR_OF(fte_match_param, spec->match_value,
			    outer_headers);
}

void *mlx5e_get_match_headers_value(u32 flags, struct mlx5_flow_spec *spec)
{
	return (flags & MLX5_FLOW_CONTEXT_ACTION_DECAP) ?
		get_match_inner_headers_value(spec) :
		get_match_outer_headers_value(spec);
}

void *mlx5e_get_match_headers_criteria(u32 flags, struct mlx5_flow_spec *spec)
{
	return (flags & MLX5_FLOW_CONTEXT_ACTION_DECAP) ?
		get_match_inner_headers_criteria(spec) :
		get_match_outer_headers_criteria(spec);
}

static int mlx5e_flower_parse_meta(struct net_device *filter_dev,
				   struct flow_cls_offload *f)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct netlink_ext_ack *extack = f->common.extack;
	struct net_device *ingress_dev;
	struct flow_match_meta match;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META))
		return 0;

	flow_rule_match_meta(rule, &match);

	if (match.mask->l2_miss) {
		NL_SET_ERR_MSG_MOD(f->common.extack, "Can't match on \"l2_miss\"");
		return -EOPNOTSUPP;
	}

	if (!match.mask->ingress_ifindex)
		return 0;

	if (match.mask->ingress_ifindex != 0xFFFFFFFF) {
		NL_SET_ERR_MSG_MOD(extack, "Unsupported ingress ifindex mask");
		return -EOPNOTSUPP;
	}

	ingress_dev = __dev_get_by_index(dev_net(filter_dev),
					 match.key->ingress_ifindex);
	if (!ingress_dev) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Can't find the ingress port to match on");
		return -ENOENT;
	}

	if (ingress_dev != filter_dev) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Can't match on the ingress filter port");
		return -EOPNOTSUPP;
	}

	return 0;
}

static bool skip_key_basic(struct net_device *filter_dev,
			   struct flow_cls_offload *f)
{
	/* When doing mpls over udp decap, the user needs to provide
	 * MPLS_UC as the protocol in order to be able to match on mpls
	 * label fields.  However, the actual ethertype is IP so we want to
	 * avoid matching on this, otherwise we'll fail the match.
	 */
	if (netif_is_bareudp(filter_dev) && f->common.chain_index == 0)
		return true;

	return false;
}

static int __parse_cls_flower(struct mlx5e_priv *priv,
			      struct mlx5e_tc_flow *flow,
			      struct mlx5_flow_spec *spec,
			      struct flow_cls_offload *f,
			      struct net_device *filter_dev,
			      u8 *inner_match_level, u8 *outer_match_level)
{
	struct netlink_ext_ack *extack = f->common.extack;
	void *headers_c = MLX5_ADDR_OF(fte_match_param, spec->match_criteria,
				       outer_headers);
	void *headers_v = MLX5_ADDR_OF(fte_match_param, spec->match_value,
				       outer_headers);
	void *misc_c = MLX5_ADDR_OF(fte_match_param, spec->match_criteria,
				    misc_parameters);
	void *misc_v = MLX5_ADDR_OF(fte_match_param, spec->match_value,
				    misc_parameters);
	void *misc_c_3 = MLX5_ADDR_OF(fte_match_param, spec->match_criteria,
				    misc_parameters_3);
	void *misc_v_3 = MLX5_ADDR_OF(fte_match_param, spec->match_value,
				    misc_parameters_3);
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct flow_dissector *dissector = rule->match.dissector;
	enum fs_flow_table_type fs_type;
	bool match_inner_ecn = true;
	u16 addr_type = 0;
	u8 ip_proto = 0;
	u8 *match_level;
	int err;

	fs_type = mlx5e_is_eswitch_flow(flow) ? FS_FT_FDB : FS_FT_NIC_RX;
	match_level = outer_match_level;

	if (dissector->used_keys &
	    ~(BIT_ULL(FLOW_DISSECTOR_KEY_META) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_CONTROL) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_BASIC) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_ETH_ADDRS) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_VLAN) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_CVLAN) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_IPV4_ADDRS) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_IPV6_ADDRS) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_PORTS) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_ENC_KEYID) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_ENC_IPV4_ADDRS) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_ENC_IPV6_ADDRS) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_ENC_PORTS)	|
	      BIT_ULL(FLOW_DISSECTOR_KEY_ENC_CONTROL) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_TCP) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_IP)  |
	      BIT_ULL(FLOW_DISSECTOR_KEY_CT) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_ENC_IP) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_ENC_OPTS) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_ICMP) |
	      BIT_ULL(FLOW_DISSECTOR_KEY_MPLS))) {
		NL_SET_ERR_MSG_MOD(extack, "Unsupported key");
		netdev_dbg(priv->netdev, "Unsupported key used: 0x%llx\n",
			   dissector->used_keys);
		return -EOPNOTSUPP;
	}

	if (mlx5e_get_tc_tun(filter_dev)) {
		bool match_inner = false;

		err = parse_tunnel_attr(priv, flow, spec, f, filter_dev,
					outer_match_level, &match_inner);
		if (err)
			return err;

		if (match_inner) {
			/* header pointers should point to the inner headers
			 * if the packet was decapsulated already.
			 * outer headers are set by parse_tunnel_attr.
			 */
			match_level = inner_match_level;
			headers_c = get_match_inner_headers_criteria(spec);
			headers_v = get_match_inner_headers_value(spec);
		}

		err = mlx5e_tc_verify_tunnel_ecn(priv, f, &match_inner_ecn);
		if (err)
			return err;
	}

	err = mlx5e_flower_parse_meta(filter_dev, f);
	if (err)
		return err;

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC) &&
	    !skip_key_basic(filter_dev, f)) {
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);
		mlx5e_tc_set_ethertype(priv->mdev, &match,
				       match_level == outer_match_level,
				       headers_c, headers_v);

		if (match.mask->n_proto)
			*match_level = MLX5_MATCH_L2;
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN) ||
	    is_vlan_dev(filter_dev)) {
		struct flow_dissector_key_vlan filter_dev_mask;
		struct flow_dissector_key_vlan filter_dev_key;
		struct flow_match_vlan match;

		if (is_vlan_dev(filter_dev)) {
			match.key = &filter_dev_key;
			match.key->vlan_id = vlan_dev_vlan_id(filter_dev);
			match.key->vlan_tpid = vlan_dev_vlan_proto(filter_dev);
			match.key->vlan_priority = 0;
			match.mask = &filter_dev_mask;
			memset(match.mask, 0xff, sizeof(*match.mask));
			match.mask->vlan_priority = 0;
		} else {
			flow_rule_match_vlan(rule, &match);
		}
		if (match.mask->vlan_id ||
		    match.mask->vlan_priority ||
		    match.mask->vlan_tpid) {
			if (match.key->vlan_tpid == htons(ETH_P_8021AD)) {
				MLX5_SET(fte_match_set_lyr_2_4, headers_c,
					 svlan_tag, 1);
				MLX5_SET(fte_match_set_lyr_2_4, headers_v,
					 svlan_tag, 1);
			} else {
				MLX5_SET(fte_match_set_lyr_2_4, headers_c,
					 cvlan_tag, 1);
				MLX5_SET(fte_match_set_lyr_2_4, headers_v,
					 cvlan_tag, 1);
			}

			MLX5_SET(fte_match_set_lyr_2_4, headers_c, first_vid,
				 match.mask->vlan_id);
			MLX5_SET(fte_match_set_lyr_2_4, headers_v, first_vid,
				 match.key->vlan_id);

			MLX5_SET(fte_match_set_lyr_2_4, headers_c, first_prio,
				 match.mask->vlan_priority);
			MLX5_SET(fte_match_set_lyr_2_4, headers_v, first_prio,
				 match.key->vlan_priority);

			*match_level = MLX5_MATCH_L2;

			if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN) &&
			    match.mask->vlan_eth_type &&
			    MLX5_CAP_FLOWTABLE_TYPE(priv->mdev,
						    ft_field_support.outer_second_vid,
						    fs_type)) {
				MLX5_SET(fte_match_set_misc, misc_c,
					 outer_second_cvlan_tag, 1);
				spec->match_criteria_enable |=
					MLX5_MATCH_MISC_PARAMETERS;
			}
		}
	} else if (*match_level != MLX5_MATCH_NONE) {
		/* cvlan_tag enabled in match criteria and
		 * disabled in match value means both S & C tags
		 * don't exist (untagged of both)
		 */
		MLX5_SET(fte_match_set_lyr_2_4, headers_c, cvlan_tag, 1);
		*match_level = MLX5_MATCH_L2;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_cvlan(rule, &match);
		if (match.mask->vlan_id ||
		    match.mask->vlan_priority ||
		    match.mask->vlan_tpid) {
			if (!MLX5_CAP_FLOWTABLE_TYPE(priv->mdev, ft_field_support.outer_second_vid,
						     fs_type)) {
				NL_SET_ERR_MSG_MOD(extack,
						   "Matching on CVLAN is not supported");
				return -EOPNOTSUPP;
			}

			if (match.key->vlan_tpid == htons(ETH_P_8021AD)) {
				MLX5_SET(fte_match_set_misc, misc_c,
					 outer_second_svlan_tag, 1);
				MLX5_SET(fte_match_set_misc, misc_v,
					 outer_second_svlan_tag, 1);
			} else {
				MLX5_SET(fte_match_set_misc, misc_c,
					 outer_second_cvlan_tag, 1);
				MLX5_SET(fte_match_set_misc, misc_v,
					 outer_second_cvlan_tag, 1);
			}

			MLX5_SET(fte_match_set_misc, misc_c, outer_second_vid,
				 match.mask->vlan_id);
			MLX5_SET(fte_match_set_misc, misc_v, outer_second_vid,
				 match.key->vlan_id);
			MLX5_SET(fte_match_set_misc, misc_c, outer_second_prio,
				 match.mask->vlan_priority);
			MLX5_SET(fte_match_set_misc, misc_v, outer_second_prio,
				 match.key->vlan_priority);

			*match_level = MLX5_MATCH_L2;
			spec->match_criteria_enable |= MLX5_MATCH_MISC_PARAMETERS;
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);
		ether_addr_copy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_c,
					     dmac_47_16),
				match.mask->dst);
		ether_addr_copy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_v,
					     dmac_47_16),
				match.key->dst);

		ether_addr_copy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_c,
					     smac_47_16),
				match.mask->src);
		ether_addr_copy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_v,
					     smac_47_16),
				match.key->src);

		if (!is_zero_ether_addr(match.mask->src) ||
		    !is_zero_ether_addr(match.mask->dst))
			*match_level = MLX5_MATCH_L2;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;

		/* the HW doesn't support frag first/later */
		if (match.mask->flags & FLOW_DIS_FIRST_FRAG) {
			NL_SET_ERR_MSG_MOD(extack, "Match on frag first/later is not supported");
			return -EOPNOTSUPP;
		}

		if (match.mask->flags & FLOW_DIS_IS_FRAGMENT) {
			MLX5_SET(fte_match_set_lyr_2_4, headers_c, frag, 1);
			MLX5_SET(fte_match_set_lyr_2_4, headers_v, frag,
				 match.key->flags & FLOW_DIS_IS_FRAGMENT);

			/* the HW doesn't need L3 inline to match on frag=no */
			if (!(match.key->flags & FLOW_DIS_IS_FRAGMENT))
				*match_level = MLX5_MATCH_L2;
	/* ***  L2 attributes parsing up to here *** */
			else
				*match_level = MLX5_MATCH_L3;
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);
		ip_proto = match.key->ip_proto;

		MLX5_SET(fte_match_set_lyr_2_4, headers_c, ip_protocol,
			 match.mask->ip_proto);
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, ip_protocol,
			 match.key->ip_proto);

		if (match.mask->ip_proto)
			*match_level = MLX5_MATCH_L3;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_c,
				    src_ipv4_src_ipv6.ipv4_layout.ipv4),
		       &match.mask->src, sizeof(match.mask->src));
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_v,
				    src_ipv4_src_ipv6.ipv4_layout.ipv4),
		       &match.key->src, sizeof(match.key->src));
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_c,
				    dst_ipv4_dst_ipv6.ipv4_layout.ipv4),
		       &match.mask->dst, sizeof(match.mask->dst));
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_v,
				    dst_ipv4_dst_ipv6.ipv4_layout.ipv4),
		       &match.key->dst, sizeof(match.key->dst));

		if (match.mask->src || match.mask->dst)
			*match_level = MLX5_MATCH_L3;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs match;

		flow_rule_match_ipv6_addrs(rule, &match);
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_c,
				    src_ipv4_src_ipv6.ipv6_layout.ipv6),
		       &match.mask->src, sizeof(match.mask->src));
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_v,
				    src_ipv4_src_ipv6.ipv6_layout.ipv6),
		       &match.key->src, sizeof(match.key->src));

		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_c,
				    dst_ipv4_dst_ipv6.ipv6_layout.ipv6),
		       &match.mask->dst, sizeof(match.mask->dst));
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_v,
				    dst_ipv4_dst_ipv6.ipv6_layout.ipv6),
		       &match.key->dst, sizeof(match.key->dst));

		if (ipv6_addr_type(&match.mask->src) != IPV6_ADDR_ANY ||
		    ipv6_addr_type(&match.mask->dst) != IPV6_ADDR_ANY)
			*match_level = MLX5_MATCH_L3;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);
		if (match_inner_ecn) {
			MLX5_SET(fte_match_set_lyr_2_4, headers_c, ip_ecn,
				 match.mask->tos & 0x3);
			MLX5_SET(fte_match_set_lyr_2_4, headers_v, ip_ecn,
				 match.key->tos & 0x3);
		}

		MLX5_SET(fte_match_set_lyr_2_4, headers_c, ip_dscp,
			 match.mask->tos >> 2);
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, ip_dscp,
			 match.key->tos  >> 2);

		MLX5_SET(fte_match_set_lyr_2_4, headers_c, ttl_hoplimit,
			 match.mask->ttl);
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, ttl_hoplimit,
			 match.key->ttl);

		if (match.mask->ttl &&
		    !MLX5_CAP_ESW_FLOWTABLE_FDB(priv->mdev,
						ft_field_support.outer_ipv4_ttl)) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Matching on TTL is not supported");
			return -EOPNOTSUPP;
		}

		if (match.mask->tos || match.mask->ttl)
			*match_level = MLX5_MATCH_L3;
	}

	/* ***  L3 attributes parsing up to here *** */

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);
		switch (ip_proto) {
		case IPPROTO_TCP:
			MLX5_SET(fte_match_set_lyr_2_4, headers_c,
				 tcp_sport, ntohs(match.mask->src));
			MLX5_SET(fte_match_set_lyr_2_4, headers_v,
				 tcp_sport, ntohs(match.key->src));

			MLX5_SET(fte_match_set_lyr_2_4, headers_c,
				 tcp_dport, ntohs(match.mask->dst));
			MLX5_SET(fte_match_set_lyr_2_4, headers_v,
				 tcp_dport, ntohs(match.key->dst));
			break;

		case IPPROTO_UDP:
			MLX5_SET(fte_match_set_lyr_2_4, headers_c,
				 udp_sport, ntohs(match.mask->src));
			MLX5_SET(fte_match_set_lyr_2_4, headers_v,
				 udp_sport, ntohs(match.key->src));

			MLX5_SET(fte_match_set_lyr_2_4, headers_c,
				 udp_dport, ntohs(match.mask->dst));
			MLX5_SET(fte_match_set_lyr_2_4, headers_v,
				 udp_dport, ntohs(match.key->dst));
			break;
		default:
			NL_SET_ERR_MSG_MOD(extack,
					   "Only UDP and TCP transports are supported for L4 matching");
			netdev_err(priv->netdev,
				   "Only UDP and TCP transport are supported\n");
			return -EINVAL;
		}

		if (match.mask->src || match.mask->dst)
			*match_level = MLX5_MATCH_L4;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_TCP)) {
		struct flow_match_tcp match;

		flow_rule_match_tcp(rule, &match);
		MLX5_SET(fte_match_set_lyr_2_4, headers_c, tcp_flags,
			 ntohs(match.mask->flags));
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, tcp_flags,
			 ntohs(match.key->flags));

		if (match.mask->flags)
			*match_level = MLX5_MATCH_L4;
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ICMP)) {
		struct flow_match_icmp match;

		flow_rule_match_icmp(rule, &match);
		switch (ip_proto) {
		case IPPROTO_ICMP:
			if (!(MLX5_CAP_GEN(priv->mdev, flex_parser_protocols) &
			      MLX5_FLEX_PROTO_ICMP)) {
				NL_SET_ERR_MSG_MOD(extack,
						   "Match on Flex protocols for ICMP is not supported");
				return -EOPNOTSUPP;
			}
			MLX5_SET(fte_match_set_misc3, misc_c_3, icmp_type,
				 match.mask->type);
			MLX5_SET(fte_match_set_misc3, misc_v_3, icmp_type,
				 match.key->type);
			MLX5_SET(fte_match_set_misc3, misc_c_3, icmp_code,
				 match.mask->code);
			MLX5_SET(fte_match_set_misc3, misc_v_3, icmp_code,
				 match.key->code);
			break;
		case IPPROTO_ICMPV6:
			if (!(MLX5_CAP_GEN(priv->mdev, flex_parser_protocols) &
			      MLX5_FLEX_PROTO_ICMPV6)) {
				NL_SET_ERR_MSG_MOD(extack,
						   "Match on Flex protocols for ICMPV6 is not supported");
				return -EOPNOTSUPP;
			}
			MLX5_SET(fte_match_set_misc3, misc_c_3, icmpv6_type,
				 match.mask->type);
			MLX5_SET(fte_match_set_misc3, misc_v_3, icmpv6_type,
				 match.key->type);
			MLX5_SET(fte_match_set_misc3, misc_c_3, icmpv6_code,
				 match.mask->code);
			MLX5_SET(fte_match_set_misc3, misc_v_3, icmpv6_code,
				 match.key->code);
			break;
		default:
			NL_SET_ERR_MSG_MOD(extack,
					   "Code and type matching only with ICMP and ICMPv6");
			netdev_err(priv->netdev,
				   "Code and type matching only with ICMP and ICMPv6\n");
			return -EINVAL;
		}
		if (match.mask->code || match.mask->type) {
			*match_level = MLX5_MATCH_L4;
			spec->match_criteria_enable |= MLX5_MATCH_MISC_PARAMETERS_3;
		}
	}
	/* Currently supported only for MPLS over UDP */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_MPLS) &&
	    !netif_is_bareudp(filter_dev)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Matching on MPLS is supported only for MPLS over UDP");
		netdev_err(priv->netdev,
			   "Matching on MPLS is supported only for MPLS over UDP\n");
		return -EOPNOTSUPP;
	}

	return 0;
}

static int parse_cls_flower(struct mlx5e_priv *priv,
			    struct mlx5e_tc_flow *flow,
			    struct mlx5_flow_spec *spec,
			    struct flow_cls_offload *f,
			    struct net_device *filter_dev)
{
	u8 inner_match_level, outer_match_level, non_tunnel_match_level;
	struct netlink_ext_ack *extack = f->common.extack;
	struct mlx5_core_dev *dev = priv->mdev;
	struct mlx5_eswitch *esw = dev->priv.eswitch;
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_eswitch_rep *rep;
	bool is_eswitch_flow;
	int err;

	inner_match_level = MLX5_MATCH_NONE;
	outer_match_level = MLX5_MATCH_NONE;

	err = __parse_cls_flower(priv, flow, spec, f, filter_dev,
				 &inner_match_level, &outer_match_level);
	non_tunnel_match_level = (inner_match_level == MLX5_MATCH_NONE) ?
				 outer_match_level : inner_match_level;

	is_eswitch_flow = mlx5e_is_eswitch_flow(flow);
	if (!err && is_eswitch_flow) {
		rep = rpriv->rep;
		if (rep->vport != MLX5_VPORT_UPLINK &&
		    (esw->offloads.inline_mode != MLX5_INLINE_MODE_NONE &&
		    esw->offloads.inline_mode < non_tunnel_match_level)) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Flow is not offloaded due to min inline setting");
			netdev_warn(priv->netdev,
				    "Flow is not offloaded due to min inline setting, required %d actual %d\n",
				    non_tunnel_match_level, esw->offloads.inline_mode);
			return -EOPNOTSUPP;
		}
	}

	flow->attr->inner_match_level = inner_match_level;
	flow->attr->outer_match_level = outer_match_level;


	return err;
}

struct mlx5_fields {
	u8  field;
	u8  field_bsize;
	u32 field_mask;
	u32 offset;
	u32 match_offset;
};

#define OFFLOAD(fw_field, field_bsize, field_mask, field, off, match_field) \
		{MLX5_ACTION_IN_FIELD_OUT_ ## fw_field, field_bsize, field_mask, \
		 offsetof(struct pedit_headers, field) + (off), \
		 MLX5_BYTE_OFF(fte_match_set_lyr_2_4, match_field)}

/* masked values are the same and there are no rewrites that do not have a
 * match.
 */
#define SAME_VAL_MASK(type, valp, maskp, matchvalp, matchmaskp) ({ \
	type matchmaskx = *(type *)(matchmaskp); \
	type matchvalx = *(type *)(matchvalp); \
	type maskx = *(type *)(maskp); \
	type valx = *(type *)(valp); \
	\
	(valx & maskx) == (matchvalx & matchmaskx) && !(maskx & (maskx ^ \
								 matchmaskx)); \
})

static bool cmp_val_mask(void *valp, void *maskp, void *matchvalp,
			 void *matchmaskp, u8 bsize)
{
	bool same = false;

	switch (bsize) {
	case 8:
		same = SAME_VAL_MASK(u8, valp, maskp, matchvalp, matchmaskp);
		break;
	case 16:
		same = SAME_VAL_MASK(u16, valp, maskp, matchvalp, matchmaskp);
		break;
	case 32:
		same = SAME_VAL_MASK(u32, valp, maskp, matchvalp, matchmaskp);
		break;
	}

	return same;
}

static struct mlx5_fields fields[] = {
	OFFLOAD(DMAC_47_16, 32, U32_MAX, eth.h_dest[0], 0, dmac_47_16),
	OFFLOAD(DMAC_15_0,  16, U16_MAX, eth.h_dest[4], 0, dmac_15_0),
	OFFLOAD(SMAC_47_16, 32, U32_MAX, eth.h_source[0], 0, smac_47_16),
	OFFLOAD(SMAC_15_0,  16, U16_MAX, eth.h_source[4], 0, smac_15_0),
	OFFLOAD(ETHERTYPE,  16, U16_MAX, eth.h_proto, 0, ethertype),
	OFFLOAD(FIRST_VID,  16, U16_MAX, vlan.h_vlan_TCI, 0, first_vid),

	OFFLOAD(IP_DSCP, 8,    0xfc, ip4.tos,   0, ip_dscp),
	OFFLOAD(IP_TTL,  8,  U8_MAX, ip4.ttl,   0, ttl_hoplimit),
	OFFLOAD(SIPV4,  32, U32_MAX, ip4.saddr, 0, src_ipv4_src_ipv6.ipv4_layout.ipv4),
	OFFLOAD(DIPV4,  32, U32_MAX, ip4.daddr, 0, dst_ipv4_dst_ipv6.ipv4_layout.ipv4),

	OFFLOAD(SIPV6_127_96, 32, U32_MAX, ip6.saddr.s6_addr32[0], 0,
		src_ipv4_src_ipv6.ipv6_layout.ipv6[0]),
	OFFLOAD(SIPV6_95_64,  32, U32_MAX, ip6.saddr.s6_addr32[1], 0,
		src_ipv4_src_ipv6.ipv6_layout.ipv6[4]),
	OFFLOAD(SIPV6_63_32,  32, U32_MAX, ip6.saddr.s6_addr32[2], 0,
		src_ipv4_src_ipv6.ipv6_layout.ipv6[8]),
	OFFLOAD(SIPV6_31_0,   32, U32_MAX, ip6.saddr.s6_addr32[3], 0,
		src_ipv4_src_ipv6.ipv6_layout.ipv6[12]),
	OFFLOAD(DIPV6_127_96, 32, U32_MAX, ip6.daddr.s6_addr32[0], 0,
		dst_ipv4_dst_ipv6.ipv6_layout.ipv6[0]),
	OFFLOAD(DIPV6_95_64,  32, U32_MAX, ip6.daddr.s6_addr32[1], 0,
		dst_ipv4_dst_ipv6.ipv6_layout.ipv6[4]),
	OFFLOAD(DIPV6_63_32,  32, U32_MAX, ip6.daddr.s6_addr32[2], 0,
		dst_ipv4_dst_ipv6.ipv6_layout.ipv6[8]),
	OFFLOAD(DIPV6_31_0,   32, U32_MAX, ip6.daddr.s6_addr32[3], 0,
		dst_ipv4_dst_ipv6.ipv6_layout.ipv6[12]),
	OFFLOAD(IPV6_HOPLIMIT, 8,  U8_MAX, ip6.hop_limit, 0, ttl_hoplimit),
	OFFLOAD(IP_DSCP, 16,  0xc00f, ip6, 0, ip_dscp),

	OFFLOAD(TCP_SPORT, 16, U16_MAX, tcp.source,  0, tcp_sport),
	OFFLOAD(TCP_DPORT, 16, U16_MAX, tcp.dest,    0, tcp_dport),
	/* in linux iphdr tcp_flags is 8 bits long */
	OFFLOAD(TCP_FLAGS,  8,  U8_MAX, tcp.ack_seq, 5, tcp_flags),

	OFFLOAD(UDP_SPORT, 16, U16_MAX, udp.source, 0, udp_sport),
	OFFLOAD(UDP_DPORT, 16, U16_MAX, udp.dest,   0, udp_dport),
};

static unsigned long mask_to_le(unsigned long mask, int size)
{
	__be32 mask_be32;
	__be16 mask_be16;

	if (size == 32) {
		mask_be32 = (__force __be32)(mask);
		mask = (__force unsigned long)cpu_to_le32(be32_to_cpu(mask_be32));
	} else if (size == 16) {
		mask_be32 = (__force __be32)(mask);
		mask_be16 = *(__be16 *)&mask_be32;
		mask = (__force unsigned long)cpu_to_le16(be16_to_cpu(mask_be16));
	}

	return mask;
}

static int offload_pedit_fields(struct mlx5e_priv *priv,
				int namespace,
				struct mlx5e_tc_flow_parse_attr *parse_attr,
				u32 *action_flags,
				struct netlink_ext_ack *extack)
{
	struct pedit_headers *set_masks, *add_masks, *set_vals, *add_vals;
	struct pedit_headers_action *hdrs = parse_attr->hdrs;
	void *headers_c, *headers_v, *action, *vals_p;
	u32 *s_masks_p, *a_masks_p, s_mask, a_mask;
	struct mlx5e_tc_mod_hdr_acts *mod_acts;
	unsigned long mask, field_mask;
	int i, first, last, next_z;
	struct mlx5_fields *f;
	u8 cmd;

	mod_acts = &parse_attr->mod_hdr_acts;
	headers_c = mlx5e_get_match_headers_criteria(*action_flags, &parse_attr->spec);
	headers_v = mlx5e_get_match_headers_value(*action_flags, &parse_attr->spec);

	set_masks = &hdrs[0].masks;
	add_masks = &hdrs[1].masks;
	set_vals = &hdrs[0].vals;
	add_vals = &hdrs[1].vals;

	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		bool skip;

		f = &fields[i];
		/* avoid seeing bits set from previous iterations */
		s_mask = 0;
		a_mask = 0;

		s_masks_p = (void *)set_masks + f->offset;
		a_masks_p = (void *)add_masks + f->offset;

		s_mask = *s_masks_p & f->field_mask;
		a_mask = *a_masks_p & f->field_mask;

		if (!s_mask && !a_mask) /* nothing to offload here */
			continue;

		if (s_mask && a_mask) {
			NL_SET_ERR_MSG_MOD(extack,
					   "can't set and add to the same HW field");
			netdev_warn(priv->netdev,
				    "mlx5: can't set and add to the same HW field (%x)\n",
				    f->field);
			return -EOPNOTSUPP;
		}

		skip = false;
		if (s_mask) {
			void *match_mask = headers_c + f->match_offset;
			void *match_val = headers_v + f->match_offset;

			cmd  = MLX5_ACTION_TYPE_SET;
			mask = s_mask;
			vals_p = (void *)set_vals + f->offset;
			/* don't rewrite if we have a match on the same value */
			if (cmp_val_mask(vals_p, s_masks_p, match_val,
					 match_mask, f->field_bsize))
				skip = true;
			/* clear to denote we consumed this field */
			*s_masks_p &= ~f->field_mask;
		} else {
			cmd  = MLX5_ACTION_TYPE_ADD;
			mask = a_mask;
			vals_p = (void *)add_vals + f->offset;
			/* add 0 is no change */
			if ((*(u32 *)vals_p & f->field_mask) == 0)
				skip = true;
			/* clear to denote we consumed this field */
			*a_masks_p &= ~f->field_mask;
		}
		if (skip)
			continue;

		mask = mask_to_le(mask, f->field_bsize);

		first = find_first_bit(&mask, f->field_bsize);
		next_z = find_next_zero_bit(&mask, f->field_bsize, first);
		last  = find_last_bit(&mask, f->field_bsize);
		if (first < next_z && next_z < last) {
			NL_SET_ERR_MSG_MOD(extack,
					   "rewrite of few sub-fields isn't supported");
			netdev_warn(priv->netdev,
				    "mlx5: rewrite of few sub-fields (mask %lx) isn't offloaded\n",
				    mask);
			return -EOPNOTSUPP;
		}

		action = mlx5e_mod_hdr_alloc(priv->mdev, namespace, mod_acts);
		if (IS_ERR(action)) {
			NL_SET_ERR_MSG_MOD(extack,
					   "too many pedit actions, can't offload");
			mlx5_core_warn(priv->mdev,
				       "mlx5: parsed %d pedit actions, can't do more\n",
				       mod_acts->num_actions);
			return PTR_ERR(action);
		}

		MLX5_SET(set_action_in, action, action_type, cmd);
		MLX5_SET(set_action_in, action, field, f->field);

		if (cmd == MLX5_ACTION_TYPE_SET) {
			int start;

			field_mask = mask_to_le(f->field_mask, f->field_bsize);

			/* if field is bit sized it can start not from first bit */
			start = find_first_bit(&field_mask, f->field_bsize);

			MLX5_SET(set_action_in, action, offset, first - start);
			/* length is num of bits to be written, zero means length of 32 */
			MLX5_SET(set_action_in, action, length, (last - first + 1));
		}

		if (f->field_bsize == 32)
			MLX5_SET(set_action_in, action, data, ntohl(*(__be32 *)vals_p) >> first);
		else if (f->field_bsize == 16)
			MLX5_SET(set_action_in, action, data, ntohs(*(__be16 *)vals_p) >> first);
		else if (f->field_bsize == 8)
			MLX5_SET(set_action_in, action, data, *(u8 *)vals_p >> first);

		++mod_acts->num_actions;
	}

	return 0;
}

static const struct pedit_headers zero_masks = {};

static int verify_offload_pedit_fields(struct mlx5e_priv *priv,
				       struct mlx5e_tc_flow_parse_attr *parse_attr,
				       struct netlink_ext_ack *extack)
{
	struct pedit_headers *cmd_masks;
	u8 cmd;

	for (cmd = 0; cmd < __PEDIT_CMD_MAX; cmd++) {
		cmd_masks = &parse_attr->hdrs[cmd].masks;
		if (memcmp(cmd_masks, &zero_masks, sizeof(zero_masks))) {
			NL_SET_ERR_MSG_MOD(extack, "attempt to offload an unsupported field");
			netdev_warn(priv->netdev, "attempt to offload an unsupported field (cmd %d)\n", cmd);
			print_hex_dump(KERN_WARNING, "mask: ", DUMP_PREFIX_ADDRESS,
				       16, 1, cmd_masks, sizeof(zero_masks), true);
			return -EOPNOTSUPP;
		}
	}

	return 0;
}

static int alloc_tc_pedit_action(struct mlx5e_priv *priv, int namespace,
				 struct mlx5e_tc_flow_parse_attr *parse_attr,
				 u32 *action_flags,
				 struct netlink_ext_ack *extack)
{
	int err;

	err = offload_pedit_fields(priv, namespace, parse_attr, action_flags, extack);
	if (err)
		goto out_dealloc_parsed_actions;

	err = verify_offload_pedit_fields(priv, parse_attr, extack);
	if (err)
		goto out_dealloc_parsed_actions;

	return 0;

out_dealloc_parsed_actions:
	mlx5e_mod_hdr_dealloc(&parse_attr->mod_hdr_acts);
	return err;
}

struct ip_ttl_word {
	__u8	ttl;
	__u8	protocol;
	__sum16	check;
};

struct ipv6_hoplimit_word {
	__be16	payload_len;
	__u8	nexthdr;
	__u8	hop_limit;
};

static bool
is_flow_action_modify_ip_header(struct flow_action *flow_action)
{
	const struct flow_action_entry *act;
	u32 mask, offset;
	u8 htype;
	int i;

	/* For IPv4 & IPv6 header check 4 byte word,
	 * to determine that modified fields
	 * are NOT ttl & hop_limit only.
	 */
	flow_action_for_each(i, act, flow_action) {
		if (act->id != FLOW_ACTION_MANGLE &&
		    act->id != FLOW_ACTION_ADD)
			continue;

		htype = act->mangle.htype;
		offset = act->mangle.offset;
		mask = ~act->mangle.mask;

		if (htype == FLOW_ACT_MANGLE_HDR_TYPE_IP4) {
			struct ip_ttl_word *ttl_word =
				(struct ip_ttl_word *)&mask;

			if (offset != offsetof(struct iphdr, ttl) ||
			    ttl_word->protocol ||
			    ttl_word->check)
				return true;
		} else if (htype == FLOW_ACT_MANGLE_HDR_TYPE_IP6) {
			struct ipv6_hoplimit_word *hoplimit_word =
				(struct ipv6_hoplimit_word *)&mask;

			if (offset != offsetof(struct ipv6hdr, payload_len) ||
			    hoplimit_word->payload_len ||
			    hoplimit_word->nexthdr)
				return true;
		}
	}

	return false;
}

static bool modify_header_match_supported(struct mlx5e_priv *priv,
					  struct mlx5_flow_spec *spec,
					  struct flow_action *flow_action,
					  u32 actions,
					  struct netlink_ext_ack *extack)
{
	bool modify_ip_header;
	void *headers_c;
	void *headers_v;
	u16 ethertype;
	u8 ip_proto;

	headers_c = mlx5e_get_match_headers_criteria(actions, spec);
	headers_v = mlx5e_get_match_headers_value(actions, spec);
	ethertype = MLX5_GET(fte_match_set_lyr_2_4, headers_v, ethertype);

	/* for non-IP we only re-write MACs, so we're okay */
	if (MLX5_GET(fte_match_set_lyr_2_4, headers_c, ip_version) == 0 &&
	    ethertype != ETH_P_IP && ethertype != ETH_P_IPV6)
		goto out_ok;

	modify_ip_header = is_flow_action_modify_ip_header(flow_action);
	ip_proto = MLX5_GET(fte_match_set_lyr_2_4, headers_v, ip_protocol);
	if (modify_ip_header && ip_proto != IPPROTO_TCP &&
	    ip_proto != IPPROTO_UDP && ip_proto != IPPROTO_ICMP) {
		NL_SET_ERR_MSG_MOD(extack,
				   "can't offload re-write of non TCP/UDP");
		netdev_info(priv->netdev, "can't offload re-write of ip proto %d\n",
			    ip_proto);
		return false;
	}

out_ok:
	return true;
}

static bool
actions_match_supported_fdb(struct mlx5e_priv *priv,
			    struct mlx5e_tc_flow *flow,
			    struct netlink_ext_ack *extack)
{
	struct mlx5_esw_flow_attr *esw_attr = flow->attr->esw_attr;

	if (esw_attr->split_count > 0 && !mlx5_esw_has_fwd_fdb(priv->mdev)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "current firmware doesn't support split rule for port mirroring");
		netdev_warn_once(priv->netdev,
				 "current firmware doesn't support split rule for port mirroring\n");
		return false;
	}

	return true;
}

static bool
actions_match_supported(struct mlx5e_priv *priv,
			struct flow_action *flow_action,
			u32 actions,
			struct mlx5e_tc_flow_parse_attr *parse_attr,
			struct mlx5e_tc_flow *flow,
			struct netlink_ext_ack *extack)
{
	if (actions & MLX5_FLOW_CONTEXT_ACTION_MOD_HDR &&
	    !modify_header_match_supported(priv, &parse_attr->spec, flow_action, actions,
					   extack))
		return false;

	if (mlx5e_is_eswitch_flow(flow) &&
	    !actions_match_supported_fdb(priv, flow, extack))
		return false;

	return true;
}

static bool same_port_devs(struct mlx5e_priv *priv, struct mlx5e_priv *peer_priv)
{
	return priv->mdev == peer_priv->mdev;
}

bool mlx5e_same_hw_devs(struct mlx5e_priv *priv, struct mlx5e_priv *peer_priv)
{
	struct mlx5_core_dev *fmdev, *pmdev;
	u64 fsystem_guid, psystem_guid;

	fmdev = priv->mdev;
	pmdev = peer_priv->mdev;

	fsystem_guid = mlx5_query_nic_system_image_guid(fmdev);
	psystem_guid = mlx5_query_nic_system_image_guid(pmdev);

	return (fsystem_guid == psystem_guid);
}

static int
actions_prepare_mod_hdr_actions(struct mlx5e_priv *priv,
				struct mlx5e_tc_flow *flow,
				struct mlx5_flow_attr *attr,
				struct netlink_ext_ack *extack)
{
	struct mlx5e_tc_flow_parse_attr *parse_attr = attr->parse_attr;
	struct pedit_headers_action *hdrs = parse_attr->hdrs;
	enum mlx5_flow_namespace_type ns_type;
	int err;

	if (!hdrs[TCA_PEDIT_KEY_EX_CMD_SET].pedits &&
	    !hdrs[TCA_PEDIT_KEY_EX_CMD_ADD].pedits)
		return 0;

	ns_type = mlx5e_get_flow_namespace(flow);

	err = alloc_tc_pedit_action(priv, ns_type, parse_attr, &attr->action, extack);
	if (err)
		return err;

	if (parse_attr->mod_hdr_acts.num_actions > 0)
		return 0;

	/* In case all pedit actions are skipped, remove the MOD_HDR flag. */
	attr->action &= ~MLX5_FLOW_CONTEXT_ACTION_MOD_HDR;
	mlx5e_mod_hdr_dealloc(&parse_attr->mod_hdr_acts);

	if (ns_type != MLX5_FLOW_NAMESPACE_FDB)
		return 0;

	if (!((attr->action & MLX5_FLOW_CONTEXT_ACTION_VLAN_POP) ||
	      (attr->action & MLX5_FLOW_CONTEXT_ACTION_VLAN_PUSH)))
		attr->esw_attr->split_count = 0;

	return 0;
}

static struct mlx5_flow_attr*
mlx5e_clone_flow_attr_for_post_act(struct mlx5_flow_attr *attr,
				   enum mlx5_flow_namespace_type ns_type)
{
	struct mlx5e_tc_flow_parse_attr *parse_attr;
	u32 attr_sz = ns_to_attr_sz(ns_type);
	struct mlx5_flow_attr *attr2;

	attr2 = mlx5_alloc_flow_attr(ns_type);
	parse_attr = kvzalloc(sizeof(*parse_attr), GFP_KERNEL);
	if (!attr2 || !parse_attr) {
		kvfree(parse_attr);
		kfree(attr2);
		return NULL;
	}

	memcpy(attr2, attr, attr_sz);
	INIT_LIST_HEAD(&attr2->list);
	parse_attr->filter_dev = attr->parse_attr->filter_dev;
	attr2->action = 0;
	attr2->counter = NULL;
	attr2->tc_act_cookies_count = 0;
	attr2->flags = 0;
	attr2->parse_attr = parse_attr;
	attr2->dest_chain = 0;
	attr2->dest_ft = NULL;
	attr2->act_id_restore_rule = NULL;
	memset(&attr2->ct_attr, 0, sizeof(attr2->ct_attr));

	if (ns_type == MLX5_FLOW_NAMESPACE_FDB) {
		attr2->esw_attr->out_count = 0;
		attr2->esw_attr->split_count = 0;
	}

	attr2->branch_true = NULL;
	attr2->branch_false = NULL;
	attr2->jumping_attr = NULL;
	return attr2;
}

struct mlx5_flow_attr *
mlx5e_tc_get_encap_attr(struct mlx5e_tc_flow *flow)
{
	struct mlx5_esw_flow_attr *esw_attr;
	struct mlx5_flow_attr *attr;
	int i;

	list_for_each_entry(attr, &flow->attrs, list) {
		esw_attr = attr->esw_attr;
		for (i = 0; i < MLX5_MAX_FLOW_FWD_VPORTS; i++) {
			if (esw_attr->dests[i].flags & MLX5_ESW_DEST_ENCAP)
				return attr;
		}
	}

	return NULL;
}

void
mlx5e_tc_unoffload_flow_post_acts(struct mlx5e_tc_flow *flow)
{
	struct mlx5e_post_act *post_act = get_post_action(flow->priv);
	struct mlx5_flow_attr *attr;

	list_for_each_entry(attr, &flow->attrs, list) {
		if (list_is_last(&attr->list, &flow->attrs))
			break;

		mlx5e_tc_post_act_unoffload(post_act, attr->post_act_handle);
	}
}

static void
free_flow_post_acts(struct mlx5e_tc_flow *flow)
{
	struct mlx5_flow_attr *attr, *tmp;

	list_for_each_entry_safe(attr, tmp, &flow->attrs, list) {
		if (list_is_last(&attr->list, &flow->attrs))
			break;

		mlx5_free_flow_attr_actions(flow, attr);

		list_del(&attr->list);
		kvfree(attr->parse_attr);
		kfree(attr);
	}
}

int
mlx5e_tc_offload_flow_post_acts(struct mlx5e_tc_flow *flow)
{
	struct mlx5e_post_act *post_act = get_post_action(flow->priv);
	struct mlx5_flow_attr *attr;
	int err = 0;

	list_for_each_entry(attr, &flow->attrs, list) {
		if (list_is_last(&attr->list, &flow->attrs))
			break;

		err = mlx5e_tc_post_act_offload(post_act, attr->post_act_handle);
		if (err)
			break;
	}

	return err;
}

/* TC filter rule HW translation:
 *
 * +---------------------+
 * + ft prio (tc chain)  +
 * + original match      +
 * +---------------------+
 *           |
 *           | if multi table action
 *           |
 *           v
 * +---------------------+
 * + post act ft         |<----.
 * + match fte id        |     | split on multi table action
 * + do actions          |-----'
 * +---------------------+
 *           |
 *           |
 *           v
 * Do rest of the actions after last multi table action.
 */
static int
alloc_flow_post_acts(struct mlx5e_tc_flow *flow, struct netlink_ext_ack *extack)
{
	struct mlx5e_post_act *post_act = get_post_action(flow->priv);
	struct mlx5_flow_attr *attr, *next_attr = NULL;
	struct mlx5e_post_act_handle *handle;
	int err;

	/* This is going in reverse order as needed.
	 * The first entry is the last attribute.
	 */
	list_for_each_entry(attr, &flow->attrs, list) {
		if (!next_attr) {
			/* Set counter action on last post act rule. */
			attr->action |= MLX5_FLOW_CONTEXT_ACTION_COUNT;
		}

		if (next_attr && !(attr->flags & MLX5_ATTR_FLAG_TERMINATING)) {
			err = mlx5e_tc_act_set_next_post_act(flow, attr, next_attr);
			if (err)
				goto out_free;
		}

		/* Don't add post_act rule for first attr (last in the list).
		 * It's being handled by the caller.
		 */
		if (list_is_last(&attr->list, &flow->attrs))
			break;

		err = actions_prepare_mod_hdr_actions(flow->priv, flow, attr, extack);
		if (err)
			goto out_free;

		err = post_process_attr(flow, attr, extack);
		if (err)
			goto out_free;

		handle = mlx5e_tc_post_act_add(post_act, attr);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			goto out_free;
		}

		attr->post_act_handle = handle;

		if (attr->jumping_attr) {
			err = mlx5e_tc_act_set_next_post_act(flow, attr->jumping_attr, attr);
			if (err)
				goto out_free;
		}

		next_attr = attr;
	}

	if (flow_flag_test(flow, SLOW))
		goto out;

	err = mlx5e_tc_offload_flow_post_acts(flow);
	if (err)
		goto out_free;

out:
	return 0;

out_free:
	free_flow_post_acts(flow);
	return err;
}

static int
alloc_branch_attr(struct mlx5e_tc_flow *flow,
		  struct mlx5e_tc_act_branch_ctrl *cond,
		  struct mlx5_flow_attr **cond_attr,
		  u32 *jump_count,
		  struct netlink_ext_ack *extack)
{
	struct mlx5_flow_attr *attr;
	int err = 0;

	*cond_attr = mlx5e_clone_flow_attr_for_post_act(flow->attr,
							mlx5e_get_flow_namespace(flow));
	if (!(*cond_attr))
		return -ENOMEM;

	attr = *cond_attr;

	switch (cond->act_id) {
	case FLOW_ACTION_DROP:
		attr->action |= MLX5_FLOW_CONTEXT_ACTION_DROP;
		break;
	case FLOW_ACTION_ACCEPT:
	case FLOW_ACTION_PIPE:
		attr->action |= MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;
		attr->dest_ft = mlx5e_tc_post_act_get_ft(get_post_action(flow->priv));
		break;
	case FLOW_ACTION_JUMP:
		if (*jump_count) {
			NL_SET_ERR_MSG_MOD(extack, "Cannot offload flows with nested jumps");
			err = -EOPNOTSUPP;
			goto out_err;
		}
		*jump_count = cond->extval;
		attr->action |= MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;
		attr->dest_ft = mlx5e_tc_post_act_get_ft(get_post_action(flow->priv));
		break;
	default:
		err = -EOPNOTSUPP;
		goto out_err;
	}

	return err;
out_err:
	kfree(*cond_attr);
	*cond_attr = NULL;
	return err;
}

static void
dec_jump_count(struct flow_action_entry *act, struct mlx5e_tc_act *tc_act,
	       struct mlx5_flow_attr *attr, struct mlx5e_priv *priv,
	       struct mlx5e_tc_jump_state *jump_state)
{
	if (!jump_state->jump_count)
		return;

	/* Single tc action can instantiate multiple offload actions (e.g. pedit)
	 * Jump only over a tc action
	 */
	if (act->id == jump_state->last_id && act->hw_index == jump_state->last_index)
		return;

	jump_state->last_id = act->id;
	jump_state->last_index = act->hw_index;

	/* nothing to do for intermediate actions */
	if (--jump_state->jump_count > 1)
		return;

	if (jump_state->jump_count == 1) { /* last action in the jump action list */

		/* create a new attribute after this action */
		jump_state->jump_target = true;

		if (tc_act->is_terminating_action) { /* the branch ends here */
			attr->flags |= MLX5_ATTR_FLAG_TERMINATING;
			attr->action |= MLX5_FLOW_CONTEXT_ACTION_COUNT;
		} else { /* the branch continues executing the rest of the actions */
			struct mlx5e_post_act *post_act;

			attr->action |= MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;
			post_act = get_post_action(priv);
			attr->dest_ft = mlx5e_tc_post_act_get_ft(post_act);
		}
	} else if (jump_state->jump_count == 0) { /* first attr after the jump action list */
		/* This is the post action for the jumping attribute (either red or green)
		 * Use the stored jumping_attr to set the post act id on the jumping attribute
		 */
		attr->jumping_attr = jump_state->jumping_attr;
	}
}

static int
parse_branch_ctrl(struct flow_action_entry *act, struct mlx5e_tc_act *tc_act,
		  struct mlx5e_tc_flow *flow, struct mlx5_flow_attr *attr,
		  struct mlx5e_tc_jump_state *jump_state,
		  struct netlink_ext_ack *extack)
{
	struct mlx5e_tc_act_branch_ctrl cond_true, cond_false;
	u32 jump_count = jump_state->jump_count;
	int err;

	if (!tc_act->get_branch_ctrl)
		return 0;

	tc_act->get_branch_ctrl(act, &cond_true, &cond_false);

	err = alloc_branch_attr(flow, &cond_true,
				&attr->branch_true, &jump_count, extack);
	if (err)
		goto out_err;

	if (jump_count)
		jump_state->jumping_attr = attr->branch_true;

	err = alloc_branch_attr(flow, &cond_false,
				&attr->branch_false, &jump_count, extack);
	if (err)
		goto err_branch_false;

	if (jump_count && !jump_state->jumping_attr)
		jump_state->jumping_attr = attr->branch_false;

	jump_state->jump_count = jump_count;

	/* branching action requires its own counter */
	attr->action |= MLX5_FLOW_CONTEXT_ACTION_COUNT;
	flow_flag_set(flow, USE_ACT_STATS);

	return 0;

err_branch_false:
	free_branch_attr(flow, attr->branch_true);
out_err:
	return err;
}

static int
parse_tc_actions(struct mlx5e_tc_act_parse_state *parse_state,
		 struct flow_action *flow_action)
{
	struct netlink_ext_ack *extack = parse_state->extack;
	struct mlx5e_tc_flow *flow = parse_state->flow;
	struct mlx5e_tc_jump_state jump_state = {};
	struct mlx5_flow_attr *attr = flow->attr;
	enum mlx5_flow_namespace_type ns_type;
	struct mlx5e_priv *priv = flow->priv;
	struct mlx5_flow_attr *prev_attr;
	struct flow_action_entry *act;
	struct mlx5e_tc_act *tc_act;
	int err, i, i_split = 0;
	bool is_missable;

	ns_type = mlx5e_get_flow_namespace(flow);
	list_add(&attr->list, &flow->attrs);

	flow_action_for_each(i, act, flow_action) {
		jump_state.jump_target = false;
		is_missable = false;
		prev_attr = attr;

		tc_act = mlx5e_tc_act_get(act->id, ns_type);
		if (!tc_act) {
			NL_SET_ERR_MSG_MOD(extack, "Not implemented offload action");
			err = -EOPNOTSUPP;
			goto out_free_post_acts;
		}

		if (tc_act->can_offload && !tc_act->can_offload(parse_state, act, i, attr)) {
			err = -EOPNOTSUPP;
			goto out_free_post_acts;
		}

		err = tc_act->parse_action(parse_state, act, priv, attr);
		if (err)
			goto out_free_post_acts;

		dec_jump_count(act, tc_act, attr, priv, &jump_state);

		err = parse_branch_ctrl(act, tc_act, flow, attr, &jump_state, extack);
		if (err)
			goto out_free_post_acts;

		parse_state->actions |= attr->action;

		/* Split attr for multi table act if not the last act. */
		if (jump_state.jump_target ||
		    (tc_act->is_multi_table_act &&
		    tc_act->is_multi_table_act(priv, act, attr) &&
		    i < flow_action->num_entries - 1)) {
			is_missable = tc_act->is_missable ? tc_act->is_missable(act) : false;

			err = mlx5e_tc_act_post_parse(parse_state, flow_action, i_split, i, attr,
						      ns_type);
			if (err)
				goto out_free_post_acts;

			attr = mlx5e_clone_flow_attr_for_post_act(flow->attr, ns_type);
			if (!attr) {
				err = -ENOMEM;
				goto out_free_post_acts;
			}

			i_split = i + 1;
			parse_state->if_count = 0;
			list_add(&attr->list, &flow->attrs);
		}

		if (is_missable) {
			/* Add counter to prev, and assign act to new (next) attr */
			prev_attr->action |= MLX5_FLOW_CONTEXT_ACTION_COUNT;
			flow_flag_set(flow, USE_ACT_STATS);

			attr->tc_act_cookies[attr->tc_act_cookies_count++] = act->cookie;
		} else if (!tc_act->stats_action) {
			prev_attr->tc_act_cookies[prev_attr->tc_act_cookies_count++] = act->cookie;
		}
	}

	err = mlx5e_tc_act_post_parse(parse_state, flow_action, i_split, i, attr, ns_type);
	if (err)
		goto out_free_post_acts;

	err = alloc_flow_post_acts(flow, extack);
	if (err)
		goto out_free_post_acts;

	return 0;

out_free_post_acts:
	free_flow_post_acts(flow);

	return err;
}

static int
flow_action_supported(struct flow_action *flow_action,
		      struct netlink_ext_ack *extack)
{
	if (!flow_action_has_entries(flow_action)) {
		NL_SET_ERR_MSG_MOD(extack, "Flow action doesn't have any entries");
		return -EINVAL;
	}

	if (!flow_action_hw_stats_check(flow_action, extack,
					FLOW_ACTION_HW_STATS_DELAYED_BIT)) {
		NL_SET_ERR_MSG_MOD(extack, "Flow action HW stats type is not supported");
		return -EOPNOTSUPP;
	}

	return 0;
}

static int
parse_tc_nic_actions(struct mlx5e_priv *priv,
		     struct flow_action *flow_action,
		     struct mlx5e_tc_flow *flow,
		     struct netlink_ext_ack *extack)
{
	struct mlx5e_tc_act_parse_state *parse_state;
	struct mlx5e_tc_flow_parse_attr *parse_attr;
	struct mlx5_flow_attr *attr = flow->attr;
	int err;

	err = flow_action_supported(flow_action, extack);
	if (err)
		return err;

	attr->nic_attr->flow_tag = MLX5_FS_DEFAULT_FLOW_TAG;
	parse_attr = attr->parse_attr;
	parse_state = &parse_attr->parse_state;
	mlx5e_tc_act_init_parse_state(parse_state, flow, flow_action, extack);
	parse_state->ct_priv = get_ct_priv(priv);

	err = parse_tc_actions(parse_state, flow_action);
	if (err)
		return err;

	err = actions_prepare_mod_hdr_actions(priv, flow, attr, extack);
	if (err)
		return err;

	err = verify_attr_actions(attr->action, extack);
	if (err)
		return err;

	if (!actions_match_supported(priv, flow_action, parse_state->actions,
				     parse_attr, flow, extack))
		return -EOPNOTSUPP;

	return 0;
}

static bool is_merged_eswitch_vfs(struct mlx5e_priv *priv,
				  struct net_device *peer_netdev)
{
	struct mlx5e_priv *peer_priv;

	peer_priv = netdev_priv(peer_netdev);

	return (MLX5_CAP_ESW(priv->mdev, merged_eswitch) &&
		mlx5e_eswitch_vf_rep(priv->netdev) &&
		mlx5e_eswitch_vf_rep(peer_netdev) &&
		mlx5e_same_hw_devs(priv, peer_priv));
}

static bool same_hw_reps(struct mlx5e_priv *priv,
			 struct net_device *peer_netdev)
{
	struct mlx5e_priv *peer_priv;

	peer_priv = netdev_priv(peer_netdev);

	return mlx5e_eswitch_rep(priv->netdev) &&
	       mlx5e_eswitch_rep(peer_netdev) &&
	       mlx5e_same_hw_devs(priv, peer_priv);
}

static bool is_lag_dev(struct mlx5e_priv *priv,
		       struct net_device *peer_netdev)
{
	return ((mlx5_lag_is_sriov(priv->mdev) ||
		 mlx5_lag_is_multipath(priv->mdev)) &&
		 same_hw_reps(priv, peer_netdev));
}

static bool is_multiport_eligible(struct mlx5e_priv *priv, struct net_device *out_dev)
{
	return same_hw_reps(priv, out_dev) && mlx5_lag_is_mpesw(priv->mdev);
}

bool mlx5e_is_valid_eswitch_fwd_dev(struct mlx5e_priv *priv,
				    struct net_device *out_dev)
{
	if (is_merged_eswitch_vfs(priv, out_dev))
		return true;

	if (is_multiport_eligible(priv, out_dev))
		return true;

	if (is_lag_dev(priv, out_dev))
		return true;

	return mlx5e_eswitch_rep(out_dev) &&
	       same_port_devs(priv, netdev_priv(out_dev));
}

int mlx5e_set_fwd_to_int_port_actions(struct mlx5e_priv *priv,
				      struct mlx5_flow_attr *attr,
				      int ifindex,
				      enum mlx5e_tc_int_port_type type,
				      u32 *action,
				      int out_index)
{
	struct mlx5_esw_flow_attr *esw_attr = attr->esw_attr;
	struct mlx5e_tc_int_port_priv *int_port_priv;
	struct mlx5e_tc_flow_parse_attr *parse_attr;
	struct mlx5e_tc_int_port *dest_int_port;
	int err;

	parse_attr = attr->parse_attr;
	int_port_priv = mlx5e_get_int_port_priv(priv);

	dest_int_port = mlx5e_tc_int_port_get(int_port_priv, ifindex, type);
	if (IS_ERR(dest_int_port))
		return PTR_ERR(dest_int_port);

	err = mlx5e_tc_match_to_reg_set(priv->mdev, &parse_attr->mod_hdr_acts,
					MLX5_FLOW_NAMESPACE_FDB, VPORT_TO_REG,
					mlx5e_tc_int_port_get_metadata(dest_int_port));
	if (err) {
		mlx5e_tc_int_port_put(int_port_priv, dest_int_port);
		return err;
	}

	*action |= MLX5_FLOW_CONTEXT_ACTION_MOD_HDR;

	esw_attr->dest_int_port = dest_int_port;
	esw_attr->dests[out_index].flags |= MLX5_ESW_DEST_CHAIN_WITH_SRC_PORT_CHANGE;
	esw_attr->split_count = out_index;

	/* Forward to root fdb for matching against the new source vport */
	attr->dest_chain = 0;

	return 0;
}

static int
parse_tc_fdb_actions(struct mlx5e_priv *priv,
		     struct flow_action *flow_action,
		     struct mlx5e_tc_flow *flow,
		     struct netlink_ext_ack *extack)
{
	struct mlx5e_tc_act_parse_state *parse_state;
	struct mlx5e_tc_flow_parse_attr *parse_attr;
	struct mlx5_flow_attr *attr = flow->attr;
	struct mlx5_esw_flow_attr *esw_attr;
	struct net_device *filter_dev;
	int err;

	err = flow_action_supported(flow_action, extack);
	if (err)
		return err;

	esw_attr = attr->esw_attr;
	parse_attr = attr->parse_attr;
	filter_dev = parse_attr->filter_dev;
	parse_state = &parse_attr->parse_state;
	mlx5e_tc_act_init_parse_state(parse_state, flow, flow_action, extack);
	parse_state->ct_priv = get_ct_priv(priv);

	err = parse_tc_actions(parse_state, flow_action);
	if (err)
		return err;

	/* Forward to/from internal port can only have 1 dest */
	if ((netif_is_ovs_master(filter_dev) || esw_attr->dest_int_port) &&
	    esw_attr->out_count > 1) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Rules with internal port can have only one destination");
		return -EOPNOTSUPP;
	}

	/* Forward from tunnel/internal port to internal port is not supported */
	if ((mlx5e_get_tc_tun(filter_dev) || netif_is_ovs_master(filter_dev)) &&
	    esw_attr->dest_int_port) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Forwarding from tunnel/internal port to internal port is not supported");
		return -EOPNOTSUPP;
	}

	err = actions_prepare_mod_hdr_actions(priv, flow, attr, extack);
	if (err)
		return err;

	if (!actions_match_supported(priv, flow_action, parse_state->actions,
				     parse_attr, flow, extack))
		return -EOPNOTSUPP;

	return 0;
}

static void get_flags(int flags, unsigned long *flow_flags)
{
	unsigned long __flow_flags = 0;

	if (flags & MLX5_TC_FLAG(INGRESS))
		__flow_flags |= BIT(MLX5E_TC_FLOW_FLAG_INGRESS);
	if (flags & MLX5_TC_FLAG(EGRESS))
		__flow_flags |= BIT(MLX5E_TC_FLOW_FLAG_EGRESS);

	if (flags & MLX5_TC_FLAG(ESW_OFFLOAD))
		__flow_flags |= BIT(MLX5E_TC_FLOW_FLAG_ESWITCH);
	if (flags & MLX5_TC_FLAG(NIC_OFFLOAD))
		__flow_flags |= BIT(MLX5E_TC_FLOW_FLAG_NIC);
	if (flags & MLX5_TC_FLAG(FT_OFFLOAD))
		__flow_flags |= BIT(MLX5E_TC_FLOW_FLAG_FT);

	*flow_flags = __flow_flags;
}

static const struct rhashtable_params tc_ht_params = {
	.head_offset = offsetof(struct mlx5e_tc_flow, node),
	.key_offset = offsetof(struct mlx5e_tc_flow, cookie),
	.key_len = sizeof(((struct mlx5e_tc_flow *)0)->cookie),
	.automatic_shrinking = true,
};

static struct rhashtable *get_tc_ht(struct mlx5e_priv *priv,
				    unsigned long flags)
{
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);
	struct mlx5e_rep_priv *rpriv;

	if (flags & MLX5_TC_FLAG(ESW_OFFLOAD)) {
		rpriv = priv->ppriv;
		return &rpriv->tc_ht;
	} else /* NIC offload */
		return &tc->ht;
}

static bool is_peer_flow_needed(struct mlx5e_tc_flow *flow)
{
	struct mlx5_esw_flow_attr *esw_attr = flow->attr->esw_attr;
	struct mlx5_flow_attr *attr = flow->attr;
	bool is_rep_ingress = esw_attr->in_rep->vport != MLX5_VPORT_UPLINK &&
		flow_flag_test(flow, INGRESS);
	bool act_is_encap = !!(attr->action &
			       MLX5_FLOW_CONTEXT_ACTION_PACKET_REFORMAT);
	bool esw_paired = mlx5_devcom_comp_is_ready(esw_attr->in_mdev->priv.eswitch->devcom);

	if (!esw_paired)
		return false;

	if ((mlx5_lag_is_sriov(esw_attr->in_mdev) ||
	     mlx5_lag_is_multipath(esw_attr->in_mdev)) &&
	    (is_rep_ingress || act_is_encap))
		return true;

	if (mlx5_lag_is_mpesw(esw_attr->in_mdev))
		return true;

	return false;
}

struct mlx5_flow_attr *
mlx5_alloc_flow_attr(enum mlx5_flow_namespace_type type)
{
	u32 ex_attr_size = (type == MLX5_FLOW_NAMESPACE_FDB)  ?
				sizeof(struct mlx5_esw_flow_attr) :
				sizeof(struct mlx5_nic_flow_attr);
	struct mlx5_flow_attr *attr;

	attr = kzalloc(sizeof(*attr) + ex_attr_size, GFP_KERNEL);
	if (!attr)
		return attr;

	INIT_LIST_HEAD(&attr->list);
	return attr;
}

static void
mlx5_free_flow_attr_actions(struct mlx5e_tc_flow *flow, struct mlx5_flow_attr *attr)
{
	struct mlx5_core_dev *counter_dev = get_flow_counter_dev(flow);
	struct mlx5_esw_flow_attr *esw_attr;

	if (!attr)
		return;

	if (attr->post_act_handle)
		mlx5e_tc_post_act_del(get_post_action(flow->priv), attr->post_act_handle);

	mlx5e_tc_tun_encap_dests_unset(flow->priv, flow, attr);

	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_COUNT)
		mlx5_fc_destroy(counter_dev, attr->counter);

	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_MOD_HDR) {
		mlx5e_mod_hdr_dealloc(&attr->parse_attr->mod_hdr_acts);
		mlx5e_tc_detach_mod_hdr(flow->priv, flow, attr);
	}

	if (mlx5e_is_eswitch_flow(flow)) {
		esw_attr = attr->esw_attr;

		if (esw_attr->int_port)
			mlx5e_tc_int_port_put(mlx5e_get_int_port_priv(flow->priv),
					      esw_attr->int_port);

		if (esw_attr->dest_int_port)
			mlx5e_tc_int_port_put(mlx5e_get_int_port_priv(flow->priv),
					      esw_attr->dest_int_port);
	}

	mlx5_tc_ct_delete_flow(get_ct_priv(flow->priv), attr);

	free_branch_attr(flow, attr->branch_true);
	free_branch_attr(flow, attr->branch_false);
}

static int
mlx5e_alloc_flow(struct mlx5e_priv *priv, int attr_size,
		 struct flow_cls_offload *f, unsigned long flow_flags,
		 struct mlx5e_tc_flow_parse_attr **__parse_attr,
		 struct mlx5e_tc_flow **__flow)
{
	struct mlx5e_tc_flow_parse_attr *parse_attr;
	struct mlx5_flow_attr *attr;
	struct mlx5e_tc_flow *flow;
	int err = -ENOMEM;
	int out_index;

	flow = kzalloc(sizeof(*flow), GFP_KERNEL);
	parse_attr = kvzalloc(sizeof(*parse_attr), GFP_KERNEL);
	if (!parse_attr || !flow)
		goto err_free;

	flow->flags = flow_flags;
	flow->cookie = f->cookie;
	flow->priv = priv;

	attr = mlx5_alloc_flow_attr(mlx5e_get_flow_namespace(flow));
	if (!attr)
		goto err_free;

	flow->attr = attr;

	for (out_index = 0; out_index < MLX5_MAX_FLOW_FWD_VPORTS; out_index++)
		INIT_LIST_HEAD(&flow->encaps[out_index].list);
	INIT_LIST_HEAD(&flow->hairpin);
	INIT_LIST_HEAD(&flow->l3_to_l2_reformat);
	INIT_LIST_HEAD(&flow->attrs);
	INIT_LIST_HEAD(&flow->peer_flows);
	refcount_set(&flow->refcnt, 1);
	init_completion(&flow->init_done);
	init_completion(&flow->del_hw_done);

	*__flow = flow;
	*__parse_attr = parse_attr;

	return 0;

err_free:
	kfree(flow);
	kvfree(parse_attr);
	return err;
}

static void
mlx5e_flow_attr_init(struct mlx5_flow_attr *attr,
		     struct mlx5e_tc_flow_parse_attr *parse_attr,
		     struct flow_cls_offload *f)
{
	attr->parse_attr = parse_attr;
	attr->chain = f->common.chain_index;
	attr->prio = f->common.prio;
}

static void
mlx5e_flow_esw_attr_init(struct mlx5_flow_attr *attr,
			 struct mlx5e_priv *priv,
			 struct mlx5e_tc_flow_parse_attr *parse_attr,
			 struct flow_cls_offload *f,
			 struct mlx5_eswitch_rep *in_rep,
			 struct mlx5_core_dev *in_mdev)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5_esw_flow_attr *esw_attr = attr->esw_attr;

	mlx5e_flow_attr_init(attr, parse_attr, f);

	esw_attr->in_rep = in_rep;
	esw_attr->in_mdev = in_mdev;

	if (MLX5_CAP_ESW(esw->dev, counter_eswitch_affinity) ==
	    MLX5_COUNTER_SOURCE_ESWITCH)
		esw_attr->counter_dev = in_mdev;
	else
		esw_attr->counter_dev = priv->mdev;
}

static struct mlx5e_tc_flow *
__mlx5e_add_fdb_flow(struct mlx5e_priv *priv,
		     struct flow_cls_offload *f,
		     unsigned long flow_flags,
		     struct net_device *filter_dev,
		     struct mlx5_eswitch_rep *in_rep,
		     struct mlx5_core_dev *in_mdev)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct netlink_ext_ack *extack = f->common.extack;
	struct mlx5e_tc_flow_parse_attr *parse_attr;
	struct mlx5e_tc_flow *flow;
	int attr_size, err;

	flow_flags |= BIT(MLX5E_TC_FLOW_FLAG_ESWITCH);
	attr_size  = sizeof(struct mlx5_esw_flow_attr);
	err = mlx5e_alloc_flow(priv, attr_size, f, flow_flags,
			       &parse_attr, &flow);
	if (err)
		goto out;

	parse_attr->filter_dev = filter_dev;
	mlx5e_flow_esw_attr_init(flow->attr,
				 priv, parse_attr,
				 f, in_rep, in_mdev);

	err = parse_cls_flower(flow->priv, flow, &parse_attr->spec,
			       f, filter_dev);
	if (err)
		goto err_free;

	/* actions validation depends on parsing the ct matches first */
	err = mlx5_tc_ct_match_add(get_ct_priv(priv), &parse_attr->spec, f,
				   &flow->attr->ct_attr, extack);
	if (err)
		goto err_free;

	err = parse_tc_fdb_actions(priv, &rule->action, flow, extack);
	if (err)
		goto err_free;

	err = mlx5e_tc_add_fdb_flow(priv, flow, extack);
	complete_all(&flow->init_done);
	if (err) {
		if (!(err == -ENETUNREACH && mlx5_lag_is_multipath(in_mdev)))
			goto err_free;

		add_unready_flow(flow);
	}

	return flow;

err_free:
	mlx5e_flow_put(priv, flow);
out:
	return ERR_PTR(err);
}

static int mlx5e_tc_add_fdb_peer_flow(struct flow_cls_offload *f,
				      struct mlx5e_tc_flow *flow,
				      unsigned long flow_flags,
				      struct mlx5_eswitch *peer_esw)
{
	struct mlx5e_priv *priv = flow->priv, *peer_priv;
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5_esw_flow_attr *attr = flow->attr->esw_attr;
	struct mlx5e_tc_flow_parse_attr *parse_attr;
	int i = mlx5_get_dev_index(peer_esw->dev);
	struct mlx5e_rep_priv *peer_urpriv;
	struct mlx5e_tc_flow *peer_flow;
	struct mlx5_core_dev *in_mdev;
	int err = 0;

	peer_urpriv = mlx5_eswitch_get_uplink_priv(peer_esw, REP_ETH);
	peer_priv = netdev_priv(peer_urpriv->netdev);

	/* in_mdev is assigned of which the packet originated from.
	 * So packets redirected to uplink use the same mdev of the
	 * original flow and packets redirected from uplink use the
	 * peer mdev.
	 * In multiport eswitch it's a special case that we need to
	 * keep the original mdev.
	 */
	if (attr->in_rep->vport == MLX5_VPORT_UPLINK && !mlx5_lag_is_mpesw(priv->mdev))
		in_mdev = peer_priv->mdev;
	else
		in_mdev = priv->mdev;

	parse_attr = flow->attr->parse_attr;
	peer_flow = __mlx5e_add_fdb_flow(peer_priv, f, flow_flags,
					 parse_attr->filter_dev,
					 attr->in_rep, in_mdev);
	if (IS_ERR(peer_flow)) {
		err = PTR_ERR(peer_flow);
		goto out;
	}

	list_add_tail(&peer_flow->peer_flows, &flow->peer_flows);
	flow_flag_set(flow, DUP);
	mutex_lock(&esw->offloads.peer_mutex);
	list_add_tail(&flow->peer[i], &esw->offloads.peer_flows[i]);
	mutex_unlock(&esw->offloads.peer_mutex);

out:
	return err;
}

static int
mlx5e_add_fdb_flow(struct mlx5e_priv *priv,
		   struct flow_cls_offload *f,
		   unsigned long flow_flags,
		   struct net_device *filter_dev,
		   struct mlx5e_tc_flow **__flow)
{
	struct mlx5_devcom_comp_dev *devcom = priv->mdev->priv.eswitch->devcom, *pos;
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_eswitch_rep *in_rep = rpriv->rep;
	struct mlx5_core_dev *in_mdev = priv->mdev;
	struct mlx5_eswitch *peer_esw;
	struct mlx5e_tc_flow *flow;
	int err;

	flow = __mlx5e_add_fdb_flow(priv, f, flow_flags, filter_dev, in_rep,
				    in_mdev);
	if (IS_ERR(flow))
		return PTR_ERR(flow);

	if (!is_peer_flow_needed(flow)) {
		*__flow = flow;
		return 0;
	}

	if (!mlx5_devcom_for_each_peer_begin(devcom)) {
		err = -ENODEV;
		goto clean_flow;
	}

	mlx5_devcom_for_each_peer_entry(devcom, peer_esw, pos) {
		err = mlx5e_tc_add_fdb_peer_flow(f, flow, flow_flags, peer_esw);
		if (err)
			goto peer_clean;
	}

	mlx5_devcom_for_each_peer_end(devcom);

	*__flow = flow;
	return 0;

peer_clean:
	mlx5e_tc_del_fdb_peers_flow(flow);
	mlx5_devcom_for_each_peer_end(devcom);
clean_flow:
	mlx5e_tc_del_fdb_flow(priv, flow);
	return err;
}

static int
mlx5e_add_nic_flow(struct mlx5e_priv *priv,
		   struct flow_cls_offload *f,
		   unsigned long flow_flags,
		   struct net_device *filter_dev,
		   struct mlx5e_tc_flow **__flow)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct netlink_ext_ack *extack = f->common.extack;
	struct mlx5e_tc_flow_parse_attr *parse_attr;
	struct mlx5e_tc_flow *flow;
	int attr_size, err;

	if (!MLX5_CAP_FLOWTABLE_NIC_RX(priv->mdev, ignore_flow_level)) {
		if (!tc_cls_can_offload_and_chain0(priv->netdev, &f->common))
			return -EOPNOTSUPP;
	} else if (!tc_can_offload_extack(priv->netdev, f->common.extack)) {
		return -EOPNOTSUPP;
	}

	flow_flags |= BIT(MLX5E_TC_FLOW_FLAG_NIC);
	attr_size  = sizeof(struct mlx5_nic_flow_attr);
	err = mlx5e_alloc_flow(priv, attr_size, f, flow_flags,
			       &parse_attr, &flow);
	if (err)
		goto out;

	parse_attr->filter_dev = filter_dev;
	mlx5e_flow_attr_init(flow->attr, parse_attr, f);

	err = parse_cls_flower(flow->priv, flow, &parse_attr->spec,
			       f, filter_dev);
	if (err)
		goto err_free;

	err = mlx5_tc_ct_match_add(get_ct_priv(priv), &parse_attr->spec, f,
				   &flow->attr->ct_attr, extack);
	if (err)
		goto err_free;

	err = parse_tc_nic_actions(priv, &rule->action, flow, extack);
	if (err)
		goto err_free;

	err = mlx5e_tc_add_nic_flow(priv, flow, extack);
	if (err)
		goto err_free;

	flow_flag_set(flow, OFFLOADED);
	*__flow = flow;

	return 0;

err_free:
	flow_flag_set(flow, FAILED);
	mlx5e_mod_hdr_dealloc(&parse_attr->mod_hdr_acts);
	mlx5e_flow_put(priv, flow);
out:
	return err;
}

static int
mlx5e_tc_add_flow(struct mlx5e_priv *priv,
		  struct flow_cls_offload *f,
		  unsigned long flags,
		  struct net_device *filter_dev,
		  struct mlx5e_tc_flow **flow)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	unsigned long flow_flags;
	int err;

	get_flags(flags, &flow_flags);

	if (!tc_can_offload_extack(priv->netdev, f->common.extack))
		return -EOPNOTSUPP;

	if (esw && esw->mode == MLX5_ESWITCH_OFFLOADS)
		err = mlx5e_add_fdb_flow(priv, f, flow_flags,
					 filter_dev, flow);
	else
		err = mlx5e_add_nic_flow(priv, f, flow_flags,
					 filter_dev, flow);

	return err;
}

static bool is_flow_rule_duplicate_allowed(struct net_device *dev,
					   struct mlx5e_rep_priv *rpriv)
{
	/* Offloaded flow rule is allowed to duplicate on non-uplink representor
	 * sharing tc block with other slaves of a lag device. Rpriv can be NULL if this
	 * function is called from NIC mode.
	 */
	return netif_is_lag_port(dev) && rpriv && rpriv->rep->vport != MLX5_VPORT_UPLINK;
}

/* As IPsec and TC order is not aligned between software and hardware-offload,
 * either IPsec offload or TC offload, not both, is allowed for a specific interface.
 */
static bool is_tc_ipsec_order_check_needed(struct net_device *filter, struct mlx5e_priv *priv)
{
	if (!IS_ENABLED(CONFIG_MLX5_EN_IPSEC))
		return false;

	if (filter != priv->netdev)
		return false;

	if (mlx5e_eswitch_vf_rep(priv->netdev))
		return false;

	return true;
}

static int mlx5e_tc_block_ipsec_offload(struct net_device *filter, struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;

	if (!is_tc_ipsec_order_check_needed(filter, priv))
		return 0;

	if (mdev->num_block_tc)
		return -EBUSY;

	mdev->num_block_ipsec++;

	return 0;
}

static void mlx5e_tc_unblock_ipsec_offload(struct net_device *filter, struct mlx5e_priv *priv)
{
	if (!is_tc_ipsec_order_check_needed(filter, priv))
		return;

	priv->mdev->num_block_ipsec--;
}

int mlx5e_configure_flower(struct net_device *dev, struct mlx5e_priv *priv,
			   struct flow_cls_offload *f, unsigned long flags)
{
	struct netlink_ext_ack *extack = f->common.extack;
	struct rhashtable *tc_ht = get_tc_ht(priv, flags);
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5e_tc_flow *flow;
	int err = 0;

	if (!mlx5_esw_hold(priv->mdev))
		return -EBUSY;

	err = mlx5e_tc_block_ipsec_offload(dev, priv);
	if (err)
		goto esw_release;

	mlx5_esw_get(priv->mdev);

	rcu_read_lock();
	flow = rhashtable_lookup(tc_ht, &f->cookie, tc_ht_params);
	if (flow) {
		/* Same flow rule offloaded to non-uplink representor sharing tc block,
		 * just return 0.
		 */
		if (is_flow_rule_duplicate_allowed(dev, rpriv) && flow->orig_dev != dev)
			goto rcu_unlock;

		NL_SET_ERR_MSG_MOD(extack,
				   "flow cookie already exists, ignoring");
		netdev_warn_once(priv->netdev,
				 "flow cookie %lx already exists, ignoring\n",
				 f->cookie);
		err = -EEXIST;
		goto rcu_unlock;
	}
rcu_unlock:
	rcu_read_unlock();
	if (flow)
		goto out;

	trace_mlx5e_configure_flower(f);
	err = mlx5e_tc_add_flow(priv, f, flags, dev, &flow);
	if (err)
		goto out;

	/* Flow rule offloaded to non-uplink representor sharing tc block,
	 * set the flow's owner dev.
	 */
	if (is_flow_rule_duplicate_allowed(dev, rpriv))
		flow->orig_dev = dev;

	err = rhashtable_lookup_insert_fast(tc_ht, &flow->node, tc_ht_params);
	if (err)
		goto err_free;

	mlx5_esw_release(priv->mdev);
	return 0;

err_free:
	mlx5e_flow_put(priv, flow);
out:
	mlx5e_tc_unblock_ipsec_offload(dev, priv);
	mlx5_esw_put(priv->mdev);
esw_release:
	mlx5_esw_release(priv->mdev);
	return err;
}

static bool same_flow_direction(struct mlx5e_tc_flow *flow, int flags)
{
	bool dir_ingress = !!(flags & MLX5_TC_FLAG(INGRESS));
	bool dir_egress = !!(flags & MLX5_TC_FLAG(EGRESS));

	return flow_flag_test(flow, INGRESS) == dir_ingress &&
		flow_flag_test(flow, EGRESS) == dir_egress;
}

int mlx5e_delete_flower(struct net_device *dev, struct mlx5e_priv *priv,
			struct flow_cls_offload *f, unsigned long flags)
{
	struct rhashtable *tc_ht = get_tc_ht(priv, flags);
	struct mlx5e_tc_flow *flow;
	int err;

	rcu_read_lock();
	flow = rhashtable_lookup(tc_ht, &f->cookie, tc_ht_params);
	if (!flow || !same_flow_direction(flow, flags)) {
		err = -EINVAL;
		goto errout;
	}

	/* Only delete the flow if it doesn't have MLX5E_TC_FLOW_DELETED flag
	 * set.
	 */
	if (flow_flag_test_and_set(flow, DELETED)) {
		err = -EINVAL;
		goto errout;
	}
	rhashtable_remove_fast(tc_ht, &flow->node, tc_ht_params);
	rcu_read_unlock();

	trace_mlx5e_delete_flower(f);
	mlx5e_flow_put(priv, flow);

	mlx5e_tc_unblock_ipsec_offload(dev, priv);
	mlx5_esw_put(priv->mdev);
	return 0;

errout:
	rcu_read_unlock();
	return err;
}

int mlx5e_tc_fill_action_stats(struct mlx5e_priv *priv,
			       struct flow_offload_action *fl_act)
{
	return mlx5e_tc_act_stats_fill_stats(get_act_stats_handle(priv), fl_act);
}

int mlx5e_stats_flower(struct net_device *dev, struct mlx5e_priv *priv,
		       struct flow_cls_offload *f, unsigned long flags)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct rhashtable *tc_ht = get_tc_ht(priv, flags);
	struct mlx5e_tc_flow *flow;
	struct mlx5_fc *counter;
	u64 lastuse = 0;
	u64 packets = 0;
	u64 bytes = 0;
	int err = 0;

	rcu_read_lock();
	flow = mlx5e_flow_get(rhashtable_lookup(tc_ht, &f->cookie,
						tc_ht_params));
	rcu_read_unlock();
	if (IS_ERR(flow))
		return PTR_ERR(flow);

	if (!same_flow_direction(flow, flags)) {
		err = -EINVAL;
		goto errout;
	}

	if (mlx5e_is_offloaded_flow(flow)) {
		if (flow_flag_test(flow, USE_ACT_STATS)) {
			f->use_act_stats = true;
		} else {
			counter = mlx5e_tc_get_counter(flow);
			if (!counter)
				goto errout;

			mlx5_fc_query_cached(counter, &bytes, &packets, &lastuse);
		}
	}

	/* Under multipath it's possible for one rule to be currently
	 * un-offloaded while the other rule is offloaded.
	 */
	if (esw && !mlx5_devcom_for_each_peer_begin(esw->devcom))
		goto out;

	if (flow_flag_test(flow, DUP)) {
		struct mlx5e_tc_flow *peer_flow;

		list_for_each_entry(peer_flow, &flow->peer_flows, peer_flows) {
			u64 packets2;
			u64 lastuse2;
			u64 bytes2;

			if (!flow_flag_test(peer_flow, OFFLOADED))
				continue;
			if (flow_flag_test(flow, USE_ACT_STATS)) {
				f->use_act_stats = true;
				break;
			}

			counter = mlx5e_tc_get_counter(peer_flow);
			if (!counter)
				goto no_peer_counter;
			mlx5_fc_query_cached(counter, &bytes2, &packets2,
					     &lastuse2);

			bytes += bytes2;
			packets += packets2;
			lastuse = max_t(u64, lastuse, lastuse2);
		}
	}

no_peer_counter:
	if (esw)
		mlx5_devcom_for_each_peer_end(esw->devcom);
out:
	flow_stats_update(&f->stats, bytes, packets, 0, lastuse,
			  FLOW_ACTION_HW_STATS_DELAYED);
	trace_mlx5e_stats_flower(f);
errout:
	mlx5e_flow_put(priv, flow);
	return err;
}

static int apply_police_params(struct mlx5e_priv *priv, u64 rate,
			       struct netlink_ext_ack *extack)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_eswitch *esw;
	u32 rate_mbps = 0;
	u16 vport_num;
	int err;

	vport_num = rpriv->rep->vport;
	if (vport_num >= MLX5_VPORT_ECPF) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Ingress rate limit is supported only for Eswitch ports connected to VFs");
		return -EOPNOTSUPP;
	}

	esw = priv->mdev->priv.eswitch;
	/* rate is given in bytes/sec.
	 * First convert to bits/sec and then round to the nearest mbit/secs.
	 * mbit means million bits.
	 * Moreover, if rate is non zero we choose to configure to a minimum of
	 * 1 mbit/sec.
	 */
	if (rate) {
		rate = (rate * BITS_PER_BYTE) + 500000;
		do_div(rate, 1000000);
		rate_mbps = max_t(u32, rate, 1);
	}

	err = mlx5_esw_qos_modify_vport_rate(esw, vport_num, rate_mbps);
	if (err)
		NL_SET_ERR_MSG_MOD(extack, "failed applying action to hardware");

	return err;
}

static int
tc_matchall_police_validate(const struct flow_action *action,
			    const struct flow_action_entry *act,
			    struct netlink_ext_ack *extack)
{
	if (act->police.notexceed.act_id != FLOW_ACTION_CONTINUE) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Offload not supported when conform action is not continue");
		return -EOPNOTSUPP;
	}

	if (act->police.exceed.act_id != FLOW_ACTION_DROP) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Offload not supported when exceed action is not drop");
		return -EOPNOTSUPP;
	}

	if (act->police.notexceed.act_id == FLOW_ACTION_ACCEPT &&
	    !flow_action_is_last_entry(action, act)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Offload not supported when conform action is ok, but action is not last");
		return -EOPNOTSUPP;
	}

	if (act->police.peakrate_bytes_ps ||
	    act->police.avrate || act->police.overhead) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Offload not supported when peakrate/avrate/overhead is configured");
		return -EOPNOTSUPP;
	}

	return 0;
}

static int scan_tc_matchall_fdb_actions(struct mlx5e_priv *priv,
					struct flow_action *flow_action,
					struct netlink_ext_ack *extack)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	const struct flow_action_entry *act;
	int err;
	int i;

	if (!flow_action_has_entries(flow_action)) {
		NL_SET_ERR_MSG_MOD(extack, "matchall called with no action");
		return -EINVAL;
	}

	if (!flow_offload_has_one_action(flow_action)) {
		NL_SET_ERR_MSG_MOD(extack, "matchall policing support only a single action");
		return -EOPNOTSUPP;
	}

	if (!flow_action_basic_hw_stats_check(flow_action, extack)) {
		NL_SET_ERR_MSG_MOD(extack, "Flow action HW stats type is not supported");
		return -EOPNOTSUPP;
	}

	flow_action_for_each(i, act, flow_action) {
		switch (act->id) {
		case FLOW_ACTION_POLICE:
			err = tc_matchall_police_validate(flow_action, act, extack);
			if (err)
				return err;

			err = apply_police_params(priv, act->police.rate_bytes_ps, extack);
			if (err)
				return err;

			rpriv->prev_vf_vport_stats = priv->stats.vf_vport;
			break;
		default:
			NL_SET_ERR_MSG_MOD(extack, "mlx5 supports only police action for matchall");
			return -EOPNOTSUPP;
		}
	}

	return 0;
}

int mlx5e_tc_configure_matchall(struct mlx5e_priv *priv,
				struct tc_cls_matchall_offload *ma)
{
	struct netlink_ext_ack *extack = ma->common.extack;

	if (ma->common.prio != 1) {
		NL_SET_ERR_MSG_MOD(extack, "only priority 1 is supported");
		return -EINVAL;
	}

	return scan_tc_matchall_fdb_actions(priv, &ma->rule->action, extack);
}

int mlx5e_tc_delete_matchall(struct mlx5e_priv *priv,
			     struct tc_cls_matchall_offload *ma)
{
	struct netlink_ext_ack *extack = ma->common.extack;

	return apply_police_params(priv, 0, extack);
}

void mlx5e_tc_stats_matchall(struct mlx5e_priv *priv,
			     struct tc_cls_matchall_offload *ma)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct rtnl_link_stats64 cur_stats;
	u64 dbytes;
	u64 dpkts;

	cur_stats = priv->stats.vf_vport;
	dpkts = cur_stats.rx_packets - rpriv->prev_vf_vport_stats.rx_packets;
	dbytes = cur_stats.rx_bytes - rpriv->prev_vf_vport_stats.rx_bytes;
	rpriv->prev_vf_vport_stats = cur_stats;
	flow_stats_update(&ma->stats, dbytes, dpkts, 0, jiffies,
			  FLOW_ACTION_HW_STATS_DELAYED);
}

static void mlx5e_tc_hairpin_update_dead_peer(struct mlx5e_priv *priv,
					      struct mlx5e_priv *peer_priv)
{
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);
	struct mlx5_core_dev *peer_mdev = peer_priv->mdev;
	struct mlx5e_hairpin_entry *hpe, *tmp;
	LIST_HEAD(init_wait_list);
	u16 peer_vhca_id;
	int bkt;

	if (!mlx5e_same_hw_devs(priv, peer_priv))
		return;

	peer_vhca_id = MLX5_CAP_GEN(peer_mdev, vhca_id);

	mutex_lock(&tc->hairpin_tbl_lock);
	hash_for_each(tc->hairpin_tbl, bkt, hpe, hairpin_hlist)
		if (refcount_inc_not_zero(&hpe->refcnt))
			list_add(&hpe->dead_peer_wait_list, &init_wait_list);
	mutex_unlock(&tc->hairpin_tbl_lock);

	list_for_each_entry_safe(hpe, tmp, &init_wait_list, dead_peer_wait_list) {
		wait_for_completion(&hpe->res_ready);
		if (!IS_ERR_OR_NULL(hpe->hp) && hpe->peer_vhca_id == peer_vhca_id)
			mlx5_core_hairpin_clear_dead_peer(hpe->hp->pair);

		mlx5e_hairpin_put(priv, hpe);
	}
}

static int mlx5e_tc_netdev_event(struct notifier_block *this,
				 unsigned long event, void *ptr)
{
	struct net_device *ndev = netdev_notifier_info_to_dev(ptr);
	struct mlx5e_priv *peer_priv;
	struct mlx5e_tc_table *tc;
	struct mlx5e_priv *priv;

	if (ndev->netdev_ops != &mlx5e_netdev_ops ||
	    event != NETDEV_UNREGISTER ||
	    ndev->reg_state == NETREG_REGISTERED)
		return NOTIFY_DONE;

	tc = container_of(this, struct mlx5e_tc_table, netdevice_nb);
	priv = tc->priv;
	peer_priv = netdev_priv(ndev);
	if (priv == peer_priv ||
	    !(priv->netdev->features & NETIF_F_HW_TC))
		return NOTIFY_DONE;

	mlx5e_tc_hairpin_update_dead_peer(priv, peer_priv);

	return NOTIFY_DONE;
}

static int mlx5e_tc_nic_create_miss_table(struct mlx5e_priv *priv)
{
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);
	struct mlx5_flow_table **ft = &tc->miss_t;
	struct mlx5_flow_table_attr ft_attr = {};
	struct mlx5_flow_namespace *ns;
	int err = 0;

	ft_attr.max_fte = 1;
	ft_attr.autogroup.max_num_groups = 1;
	ft_attr.level = MLX5E_TC_MISS_LEVEL;
	ft_attr.prio = 0;
	ns = mlx5_get_flow_namespace(priv->mdev, MLX5_FLOW_NAMESPACE_KERNEL);

	*ft = mlx5_create_auto_grouped_flow_table(ns, &ft_attr);
	if (IS_ERR(*ft)) {
		err = PTR_ERR(*ft);
		netdev_err(priv->netdev, "failed to create tc nic miss table err=%d\n", err);
	}

	return err;
}

static void mlx5e_tc_nic_destroy_miss_table(struct mlx5e_priv *priv)
{
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);

	mlx5_destroy_flow_table(tc->miss_t);
}

int mlx5e_tc_nic_init(struct mlx5e_priv *priv)
{
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);
	struct mlx5_core_dev *dev = priv->mdev;
	struct mapping_ctx *chains_mapping;
	struct mlx5_chains_attr attr = {};
	u64 mapping_id;
	int err;

	mlx5e_mod_hdr_tbl_init(&tc->mod_hdr);
	mutex_init(&tc->t_lock);
	mutex_init(&tc->hairpin_tbl_lock);
	hash_init(tc->hairpin_tbl);
	tc->priv = priv;

	err = rhashtable_init(&tc->ht, &tc_ht_params);
	if (err)
		return err;

	lockdep_set_class(&tc->ht.mutex, &tc_ht_lock_key);
	lockdep_init_map(&tc->ht.run_work.lockdep_map, "tc_ht_wq_key", &tc_ht_wq_key, 0);

	mapping_id = mlx5_query_nic_system_image_guid(dev);

	chains_mapping = mapping_create_for_id(mapping_id, MAPPING_TYPE_CHAIN,
					       sizeof(struct mlx5_mapped_obj),
					       MLX5E_TC_TABLE_CHAIN_TAG_MASK, true);

	if (IS_ERR(chains_mapping)) {
		err = PTR_ERR(chains_mapping);
		goto err_mapping;
	}
	tc->mapping = chains_mapping;

	err = mlx5e_tc_nic_create_miss_table(priv);
	if (err)
		goto err_chains;

	if (MLX5_CAP_FLOWTABLE_NIC_RX(priv->mdev, ignore_flow_level))
		attr.flags = MLX5_CHAINS_AND_PRIOS_SUPPORTED |
			MLX5_CHAINS_IGNORE_FLOW_LEVEL_SUPPORTED;
	attr.ns = MLX5_FLOW_NAMESPACE_KERNEL;
	attr.max_grp_num = MLX5E_TC_TABLE_NUM_GROUPS;
	attr.default_ft = tc->miss_t;
	attr.mapping = chains_mapping;
	attr.fs_base_prio = MLX5E_TC_PRIO;

	tc->chains = mlx5_chains_create(dev, &attr);
	if (IS_ERR(tc->chains)) {
		err = PTR_ERR(tc->chains);
		goto err_miss;
	}

	mlx5_chains_print_info(tc->chains);

	tc->post_act = mlx5e_tc_post_act_init(priv, tc->chains, MLX5_FLOW_NAMESPACE_KERNEL);
	tc->ct = mlx5_tc_ct_init(priv, tc->chains, &tc->mod_hdr,
				 MLX5_FLOW_NAMESPACE_KERNEL, tc->post_act);

	tc->netdevice_nb.notifier_call = mlx5e_tc_netdev_event;
	err = register_netdevice_notifier_dev_net(priv->netdev,
						  &tc->netdevice_nb,
						  &tc->netdevice_nn);
	if (err) {
		tc->netdevice_nb.notifier_call = NULL;
		mlx5_core_warn(priv->mdev, "Failed to register netdev notifier\n");
		goto err_reg;
	}

	mlx5e_tc_debugfs_init(tc, mlx5e_fs_get_debugfs_root(priv->fs));

	tc->action_stats_handle = mlx5e_tc_act_stats_create();
	if (IS_ERR(tc->action_stats_handle)) {
		err = PTR_ERR(tc->action_stats_handle);
		goto err_act_stats;
	}

	return 0;

err_act_stats:
	unregister_netdevice_notifier_dev_net(priv->netdev,
					      &tc->netdevice_nb,
					      &tc->netdevice_nn);
err_reg:
	mlx5_tc_ct_clean(tc->ct);
	mlx5e_tc_post_act_destroy(tc->post_act);
	mlx5_chains_destroy(tc->chains);
err_miss:
	mlx5e_tc_nic_destroy_miss_table(priv);
err_chains:
	mapping_destroy(chains_mapping);
err_mapping:
	rhashtable_destroy(&tc->ht);
	return err;
}

static void _mlx5e_tc_del_flow(void *ptr, void *arg)
{
	struct mlx5e_tc_flow *flow = ptr;
	struct mlx5e_priv *priv = flow->priv;

	mlx5e_tc_del_flow(priv, flow);
	kfree(flow);
}

void mlx5e_tc_nic_cleanup(struct mlx5e_priv *priv)
{
	struct mlx5e_tc_table *tc = mlx5e_fs_get_tc(priv->fs);

	debugfs_remove_recursive(tc->dfs_root);

	if (tc->netdevice_nb.notifier_call)
		unregister_netdevice_notifier_dev_net(priv->netdev,
						      &tc->netdevice_nb,
						      &tc->netdevice_nn);

	mlx5e_mod_hdr_tbl_destroy(&tc->mod_hdr);
	mutex_destroy(&tc->hairpin_tbl_lock);

	rhashtable_free_and_destroy(&tc->ht, _mlx5e_tc_del_flow, NULL);

	if (!IS_ERR_OR_NULL(tc->t)) {
		mlx5_chains_put_table(tc->chains, 0, 1, MLX5E_TC_FT_LEVEL);
		tc->t = NULL;
	}
	mutex_destroy(&tc->t_lock);

	mlx5_tc_ct_clean(tc->ct);
	mlx5e_tc_post_act_destroy(tc->post_act);
	mapping_destroy(tc->mapping);
	mlx5_chains_destroy(tc->chains);
	mlx5e_tc_nic_destroy_miss_table(priv);
	mlx5e_tc_act_stats_free(tc->action_stats_handle);
}

int mlx5e_tc_ht_init(struct rhashtable *tc_ht)
{
	int err;

	err = rhashtable_init(tc_ht, &tc_ht_params);
	if (err)
		return err;

	lockdep_set_class(&tc_ht->mutex, &tc_ht_lock_key);
	lockdep_init_map(&tc_ht->run_work.lockdep_map, "tc_ht_wq_key", &tc_ht_wq_key, 0);

	return 0;
}

void mlx5e_tc_ht_cleanup(struct rhashtable *tc_ht)
{
	rhashtable_free_and_destroy(tc_ht, _mlx5e_tc_del_flow, NULL);
}

int mlx5e_tc_esw_init(struct mlx5_rep_uplink_priv *uplink_priv)
{
	const size_t sz_enc_opts = sizeof(struct tunnel_match_enc_opts);
	struct netdev_phys_item_id ppid;
	struct mlx5e_rep_priv *rpriv;
	struct mapping_ctx *mapping;
	struct mlx5_eswitch *esw;
	struct mlx5e_priv *priv;
	u64 mapping_id, key;
	int err = 0;

	rpriv = container_of(uplink_priv, struct mlx5e_rep_priv, uplink_priv);
	priv = netdev_priv(rpriv->netdev);
	esw = priv->mdev->priv.eswitch;

	uplink_priv->post_act = mlx5e_tc_post_act_init(priv, esw_chains(esw),
						       MLX5_FLOW_NAMESPACE_FDB);
	uplink_priv->ct_priv = mlx5_tc_ct_init(netdev_priv(priv->netdev),
					       esw_chains(esw),
					       &esw->offloads.mod_hdr,
					       MLX5_FLOW_NAMESPACE_FDB,
					       uplink_priv->post_act);

	uplink_priv->int_port_priv = mlx5e_tc_int_port_init(netdev_priv(priv->netdev));

	uplink_priv->tc_psample = mlx5e_tc_sample_init(esw, uplink_priv->post_act);

	mapping_id = mlx5_query_nic_system_image_guid(esw->dev);

	mapping = mapping_create_for_id(mapping_id, MAPPING_TYPE_TUNNEL,
					sizeof(struct tunnel_match_key),
					TUNNEL_INFO_BITS_MASK, true);

	if (IS_ERR(mapping)) {
		err = PTR_ERR(mapping);
		goto err_tun_mapping;
	}
	uplink_priv->tunnel_mapping = mapping;

	/* Two last values are reserved for stack devices slow path table mark
	 * and bridge ingress push mark.
	 */
	mapping = mapping_create_for_id(mapping_id, MAPPING_TYPE_TUNNEL_ENC_OPTS,
					sz_enc_opts, ENC_OPTS_BITS_MASK - 2, true);
	if (IS_ERR(mapping)) {
		err = PTR_ERR(mapping);
		goto err_enc_opts_mapping;
	}
	uplink_priv->tunnel_enc_opts_mapping = mapping;

	uplink_priv->encap = mlx5e_tc_tun_init(priv);
	if (IS_ERR(uplink_priv->encap)) {
		err = PTR_ERR(uplink_priv->encap);
		goto err_register_fib_notifier;
	}

	uplink_priv->action_stats_handle = mlx5e_tc_act_stats_create();
	if (IS_ERR(uplink_priv->action_stats_handle)) {
		err = PTR_ERR(uplink_priv->action_stats_handle);
		goto err_action_counter;
	}

	err = dev_get_port_parent_id(priv->netdev, &ppid, false);
	if (!err) {
		memcpy(&key, &ppid.id, sizeof(key));
		mlx5_esw_offloads_devcom_init(esw, key);
	}

	return 0;

err_action_counter:
	mlx5e_tc_tun_cleanup(uplink_priv->encap);
err_register_fib_notifier:
	mapping_destroy(uplink_priv->tunnel_enc_opts_mapping);
err_enc_opts_mapping:
	mapping_destroy(uplink_priv->tunnel_mapping);
err_tun_mapping:
	mlx5e_tc_sample_cleanup(uplink_priv->tc_psample);
	mlx5e_tc_int_port_cleanup(uplink_priv->int_port_priv);
	mlx5_tc_ct_clean(uplink_priv->ct_priv);
	netdev_warn(priv->netdev,
		    "Failed to initialize tc (eswitch), err: %d", err);
	mlx5e_tc_post_act_destroy(uplink_priv->post_act);
	return err;
}

void mlx5e_tc_esw_cleanup(struct mlx5_rep_uplink_priv *uplink_priv)
{
	struct mlx5e_rep_priv *rpriv;
	struct mlx5_eswitch *esw;
	struct mlx5e_priv *priv;

	rpriv = container_of(uplink_priv, struct mlx5e_rep_priv, uplink_priv);
	priv = netdev_priv(rpriv->netdev);
	esw = priv->mdev->priv.eswitch;

	mlx5_esw_offloads_devcom_cleanup(esw);

	mlx5e_tc_tun_cleanup(uplink_priv->encap);

	mapping_destroy(uplink_priv->tunnel_enc_opts_mapping);
	mapping_destroy(uplink_priv->tunnel_mapping);

	mlx5e_tc_sample_cleanup(uplink_priv->tc_psample);
	mlx5e_tc_int_port_cleanup(uplink_priv->int_port_priv);
	mlx5_tc_ct_clean(uplink_priv->ct_priv);
	mlx5e_flow_meters_cleanup(uplink_priv->flow_meters);
	mlx5e_tc_post_act_destroy(uplink_priv->post_act);
	mlx5e_tc_act_stats_free(uplink_priv->action_stats_handle);
}

int mlx5e_tc_num_filters(struct mlx5e_priv *priv, unsigned long flags)
{
	struct rhashtable *tc_ht = get_tc_ht(priv, flags);

	return atomic_read(&tc_ht->nelems);
}

void mlx5e_tc_clean_fdb_peer_flows(struct mlx5_eswitch *esw)
{
	struct mlx5e_tc_flow *flow, *tmp;
	int i;

	for (i = 0; i < MLX5_MAX_PORTS; i++) {
		if (i == mlx5_get_dev_index(esw->dev))
			continue;
		list_for_each_entry_safe(flow, tmp, &esw->offloads.peer_flows[i], peer[i])
			mlx5e_tc_del_fdb_peers_flow(flow);
	}
}

void mlx5e_tc_reoffload_flows_work(struct work_struct *work)
{
	struct mlx5_rep_uplink_priv *rpriv =
		container_of(work, struct mlx5_rep_uplink_priv,
			     reoffload_flows_work);
	struct mlx5e_tc_flow *flow, *tmp;

	mutex_lock(&rpriv->unready_flows_lock);
	list_for_each_entry_safe(flow, tmp, &rpriv->unready_flows, unready) {
		if (!mlx5e_tc_add_fdb_flow(flow->priv, flow, NULL))
			unready_flow_del(flow);
	}
	mutex_unlock(&rpriv->unready_flows_lock);
}

static int mlx5e_setup_tc_cls_flower(struct mlx5e_priv *priv,
				     struct flow_cls_offload *cls_flower,
				     unsigned long flags)
{
	switch (cls_flower->command) {
	case FLOW_CLS_REPLACE:
		return mlx5e_configure_flower(priv->netdev, priv, cls_flower,
					      flags);
	case FLOW_CLS_DESTROY:
		return mlx5e_delete_flower(priv->netdev, priv, cls_flower,
					   flags);
	case FLOW_CLS_STATS:
		return mlx5e_stats_flower(priv->netdev, priv, cls_flower,
					  flags);
	default:
		return -EOPNOTSUPP;
	}
}

int mlx5e_setup_tc_block_cb(enum tc_setup_type type, void *type_data,
			    void *cb_priv)
{
	unsigned long flags = MLX5_TC_FLAG(INGRESS);
	struct mlx5e_priv *priv = cb_priv;

	if (!priv->netdev || !netif_device_present(priv->netdev))
		return -EOPNOTSUPP;

	if (mlx5e_is_uplink_rep(priv))
		flags |= MLX5_TC_FLAG(ESW_OFFLOAD);
	else
		flags |= MLX5_TC_FLAG(NIC_OFFLOAD);

	switch (type) {
	case TC_SETUP_CLSFLOWER:
		return mlx5e_setup_tc_cls_flower(priv, type_data, flags);
	default:
		return -EOPNOTSUPP;
	}
}

static bool mlx5e_tc_restore_tunnel(struct mlx5e_priv *priv, struct sk_buff *skb,
				    struct mlx5e_tc_update_priv *tc_priv,
				    u32 tunnel_id)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct tunnel_match_enc_opts enc_opts = {};
	struct mlx5_rep_uplink_priv *uplink_priv;
	struct mlx5e_rep_priv *uplink_rpriv;
	struct metadata_dst *tun_dst;
	struct tunnel_match_key key;
	u32 tun_id, enc_opts_id;
	struct net_device *dev;
	int err;

	enc_opts_id = tunnel_id & ENC_OPTS_BITS_MASK;
	tun_id = tunnel_id >> ENC_OPTS_BITS;

	if (!tun_id)
		return true;

	uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
	uplink_priv = &uplink_rpriv->uplink_priv;

	err = mapping_find(uplink_priv->tunnel_mapping, tun_id, &key);
	if (err) {
		netdev_dbg(priv->netdev,
			   "Couldn't find tunnel for tun_id: %d, err: %d\n",
			   tun_id, err);
		return false;
	}

	if (enc_opts_id) {
		err = mapping_find(uplink_priv->tunnel_enc_opts_mapping,
				   enc_opts_id, &enc_opts);
		if (err) {
			netdev_dbg(priv->netdev,
				   "Couldn't find tunnel (opts) for tun_id: %d, err: %d\n",
				   enc_opts_id, err);
			return false;
		}
	}

	switch (key.enc_control.addr_type) {
	case FLOW_DISSECTOR_KEY_IPV4_ADDRS:
		tun_dst = __ip_tun_set_dst(key.enc_ipv4.src, key.enc_ipv4.dst,
					   key.enc_ip.tos, key.enc_ip.ttl,
					   key.enc_tp.dst, TUNNEL_KEY,
					   key32_to_tunnel_id(key.enc_key_id.keyid),
					   enc_opts.key.len);
		break;
	case FLOW_DISSECTOR_KEY_IPV6_ADDRS:
		tun_dst = __ipv6_tun_set_dst(&key.enc_ipv6.src, &key.enc_ipv6.dst,
					     key.enc_ip.tos, key.enc_ip.ttl,
					     key.enc_tp.dst, 0, TUNNEL_KEY,
					     key32_to_tunnel_id(key.enc_key_id.keyid),
					     enc_opts.key.len);
		break;
	default:
		netdev_dbg(priv->netdev,
			   "Couldn't restore tunnel, unsupported addr_type: %d\n",
			   key.enc_control.addr_type);
		return false;
	}

	if (!tun_dst) {
		netdev_dbg(priv->netdev, "Couldn't restore tunnel, no tun_dst\n");
		return false;
	}

	tun_dst->u.tun_info.key.tp_src = key.enc_tp.src;

	if (enc_opts.key.len)
		ip_tunnel_info_opts_set(&tun_dst->u.tun_info,
					enc_opts.key.data,
					enc_opts.key.len,
					enc_opts.key.dst_opt_type);

	skb_dst_set(skb, (struct dst_entry *)tun_dst);
	dev = dev_get_by_index(&init_net, key.filter_ifindex);
	if (!dev) {
		netdev_dbg(priv->netdev,
			   "Couldn't find tunnel device with ifindex: %d\n",
			   key.filter_ifindex);
		return false;
	}

	/* Set fwd_dev so we do dev_put() after datapath */
	tc_priv->fwd_dev = dev;

	skb->dev = dev;

	return true;
}

static bool mlx5e_tc_restore_skb_tc_meta(struct sk_buff *skb, struct mlx5_tc_ct_priv *ct_priv,
					 struct mlx5_mapped_obj *mapped_obj, u32 zone_restore_id,
					 u32 tunnel_id,  struct mlx5e_tc_update_priv *tc_priv)
{
	struct mlx5e_priv *priv = netdev_priv(skb->dev);
	struct tc_skb_ext *tc_skb_ext;
	u64 act_miss_cookie;
	u32 chain;

	chain = mapped_obj->type == MLX5_MAPPED_OBJ_CHAIN ? mapped_obj->chain : 0;
	act_miss_cookie = mapped_obj->type == MLX5_MAPPED_OBJ_ACT_MISS ?
			  mapped_obj->act_miss_cookie : 0;
	if (chain || act_miss_cookie) {
		if (!mlx5e_tc_ct_restore_flow(ct_priv, skb, zone_restore_id))
			return false;

		tc_skb_ext = tc_skb_ext_alloc(skb);
		if (!tc_skb_ext) {
			WARN_ON(1);
			return false;
		}

		if (act_miss_cookie) {
			tc_skb_ext->act_miss_cookie = act_miss_cookie;
			tc_skb_ext->act_miss = 1;
		} else {
			tc_skb_ext->chain = chain;
		}
	}

	if (tc_priv)
		return mlx5e_tc_restore_tunnel(priv, skb, tc_priv, tunnel_id);

	return true;
}

static void mlx5e_tc_restore_skb_sample(struct mlx5e_priv *priv, struct sk_buff *skb,
					struct mlx5_mapped_obj *mapped_obj,
					struct mlx5e_tc_update_priv *tc_priv)
{
	if (!mlx5e_tc_restore_tunnel(priv, skb, tc_priv, mapped_obj->sample.tunnel_id)) {
		netdev_dbg(priv->netdev,
			   "Failed to restore tunnel info for sampled packet\n");
		return;
	}
	mlx5e_tc_sample_skb(skb, mapped_obj);
}

static bool mlx5e_tc_restore_skb_int_port(struct mlx5e_priv *priv, struct sk_buff *skb,
					  struct mlx5_mapped_obj *mapped_obj,
					  struct mlx5e_tc_update_priv *tc_priv,
					  u32 tunnel_id)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5_rep_uplink_priv *uplink_priv;
	struct mlx5e_rep_priv *uplink_rpriv;
	bool forward_tx = false;

	/* Tunnel restore takes precedence over int port restore */
	if (tunnel_id)
		return mlx5e_tc_restore_tunnel(priv, skb, tc_priv, tunnel_id);

	uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
	uplink_priv = &uplink_rpriv->uplink_priv;

	if (mlx5e_tc_int_port_dev_fwd(uplink_priv->int_port_priv, skb,
				      mapped_obj->int_port_metadata, &forward_tx)) {
		/* Set fwd_dev for future dev_put */
		tc_priv->fwd_dev = skb->dev;
		tc_priv->forward_tx = forward_tx;

		return true;
	}

	return false;
}

bool mlx5e_tc_update_skb(struct mlx5_cqe64 *cqe, struct sk_buff *skb,
			 struct mapping_ctx *mapping_ctx, u32 mapped_obj_id,
			 struct mlx5_tc_ct_priv *ct_priv,
			 u32 zone_restore_id, u32 tunnel_id,
			 struct mlx5e_tc_update_priv *tc_priv)
{
	struct mlx5e_priv *priv = netdev_priv(skb->dev);
	struct mlx5_mapped_obj mapped_obj;
	int err;

	err = mapping_find(mapping_ctx, mapped_obj_id, &mapped_obj);
	if (err) {
		netdev_dbg(skb->dev,
			   "Couldn't find mapped object for mapped_obj_id: %d, err: %d\n",
			   mapped_obj_id, err);
		return false;
	}

	switch (mapped_obj.type) {
	case MLX5_MAPPED_OBJ_CHAIN:
	case MLX5_MAPPED_OBJ_ACT_MISS:
		return mlx5e_tc_restore_skb_tc_meta(skb, ct_priv, &mapped_obj, zone_restore_id,
						    tunnel_id, tc_priv);
	case MLX5_MAPPED_OBJ_SAMPLE:
		mlx5e_tc_restore_skb_sample(priv, skb, &mapped_obj, tc_priv);
		tc_priv->skb_done = true;
		return true;
	case MLX5_MAPPED_OBJ_INT_PORT_METADATA:
		return mlx5e_tc_restore_skb_int_port(priv, skb, &mapped_obj, tc_priv, tunnel_id);
	default:
		netdev_dbg(priv->netdev, "Invalid mapped object type: %d\n", mapped_obj.type);
		return false;
	}

	return false;
}

bool mlx5e_tc_update_skb_nic(struct mlx5_cqe64 *cqe, struct sk_buff *skb)
{
	struct mlx5e_priv *priv = netdev_priv(skb->dev);
	u32 mapped_obj_id, reg_b, zone_restore_id;
	struct mlx5_tc_ct_priv *ct_priv;
	struct mapping_ctx *mapping_ctx;
	struct mlx5e_tc_table *tc;

	reg_b = be32_to_cpu(cqe->ft_metadata);
	tc = mlx5e_fs_get_tc(priv->fs);
	mapped_obj_id = reg_b & MLX5E_TC_TABLE_CHAIN_TAG_MASK;
	zone_restore_id = (reg_b >> MLX5_REG_MAPPING_MOFFSET(NIC_ZONE_RESTORE_TO_REG)) &
			  ESW_ZONE_ID_MASK;
	ct_priv = tc->ct;
	mapping_ctx = tc->mapping;

	return mlx5e_tc_update_skb(cqe, skb, mapping_ctx, mapped_obj_id, ct_priv, zone_restore_id,
				   0, NULL);
}

static struct mapping_ctx *
mlx5e_get_priv_obj_mapping(struct mlx5e_priv *priv)
{
	struct mlx5e_tc_table *tc;
	struct mlx5_eswitch *esw;
	struct mapping_ctx *ctx;

	if (is_mdev_switchdev_mode(priv->mdev)) {
		esw = priv->mdev->priv.eswitch;
		ctx = esw->offloads.reg_c0_obj_pool;
	} else {
		tc = mlx5e_fs_get_tc(priv->fs);
		ctx = tc->mapping;
	}

	return ctx;
}

int mlx5e_tc_action_miss_mapping_get(struct mlx5e_priv *priv, struct mlx5_flow_attr *attr,
				     u64 act_miss_cookie, u32 *act_miss_mapping)
{
	struct mlx5_mapped_obj mapped_obj = {};
	struct mlx5_eswitch *esw;
	struct mapping_ctx *ctx;
	int err;

	ctx = mlx5e_get_priv_obj_mapping(priv);
	mapped_obj.type = MLX5_MAPPED_OBJ_ACT_MISS;
	mapped_obj.act_miss_cookie = act_miss_cookie;
	err = mapping_add(ctx, &mapped_obj, act_miss_mapping);
	if (err)
		return err;

	if (!is_mdev_switchdev_mode(priv->mdev))
		return 0;

	esw = priv->mdev->priv.eswitch;
	attr->act_id_restore_rule = esw_add_restore_rule(esw, *act_miss_mapping);
	if (IS_ERR(attr->act_id_restore_rule))
		goto err_rule;

	return 0;

err_rule:
	mapping_remove(ctx, *act_miss_mapping);
	return err;
}

void mlx5e_tc_action_miss_mapping_put(struct mlx5e_priv *priv, struct mlx5_flow_attr *attr,
				      u32 act_miss_mapping)
{
	struct mapping_ctx *ctx = mlx5e_get_priv_obj_mapping(priv);

	if (is_mdev_switchdev_mode(priv->mdev))
		mlx5_del_flow_rules(attr->act_id_restore_rule);
	mapping_remove(ctx, act_miss_mapping);
}
