/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2021, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#ifndef __MLX5_EN_TC_ACT_H__
#define __MLX5_EN_TC_ACT_H__

#include <net/tc_act/tc_pedit.h>
#include <net/flow_offload.h>
#include <linux/netlink.h>
#include "eswitch.h"
#include "pedit.h"

struct mlx5_flow_attr;

struct mlx5e_tc_act_parse_state {
	struct flow_action *flow_action;
	struct mlx5e_tc_flow *flow;
	struct netlink_ext_ack *extack;
	u32 actions;
	bool encap;
	bool decap;
	bool mpls_push;
	bool eth_push;
	bool eth_pop;
	bool ptype_host;
	const struct ip_tunnel_info *tun_info;
	struct mlx5e_mpls_info mpls_info;
	int ifindexes[MLX5_MAX_FLOW_FWD_VPORTS];
	int if_count;
	struct mlx5_tc_ct_priv *ct_priv;
};

struct mlx5e_tc_act_branch_ctrl {
	enum flow_action_id act_id;
	u32 extval;
};

struct mlx5e_tc_act {
	bool (*can_offload)(struct mlx5e_tc_act_parse_state *parse_state,
			    const struct flow_action_entry *act,
			    int act_index,
			    struct mlx5_flow_attr *attr);

	int (*parse_action)(struct mlx5e_tc_act_parse_state *parse_state,
			    const struct flow_action_entry *act,
			    struct mlx5e_priv *priv,
			    struct mlx5_flow_attr *attr);

	int (*post_parse)(struct mlx5e_tc_act_parse_state *parse_state,
			  struct mlx5e_priv *priv,
			  struct mlx5_flow_attr *attr);

	bool (*is_multi_table_act)(struct mlx5e_priv *priv,
				   const struct flow_action_entry *act,
				   struct mlx5_flow_attr *attr);

	bool (*is_missable)(const struct flow_action_entry *act);

	int (*offload_action)(struct mlx5e_priv *priv,
			      struct flow_offload_action *fl_act,
			      struct flow_action_entry *act);

	int (*destroy_action)(struct mlx5e_priv *priv,
			      struct flow_offload_action *fl_act);

	int (*stats_action)(struct mlx5e_priv *priv,
			    struct flow_offload_action *fl_act);

	bool (*get_branch_ctrl)(const struct flow_action_entry *act,
				struct mlx5e_tc_act_branch_ctrl *cond_true,
				struct mlx5e_tc_act_branch_ctrl *cond_false);

	bool is_terminating_action;
};

struct mlx5e_tc_flow_action {
	unsigned int num_entries;
	struct flow_action_entry **entries;
};

extern struct mlx5e_tc_act mlx5e_tc_act_drop;
extern struct mlx5e_tc_act mlx5e_tc_act_trap;
extern struct mlx5e_tc_act mlx5e_tc_act_accept;
extern struct mlx5e_tc_act mlx5e_tc_act_mark;
extern struct mlx5e_tc_act mlx5e_tc_act_goto;
extern struct mlx5e_tc_act mlx5e_tc_act_tun_encap;
extern struct mlx5e_tc_act mlx5e_tc_act_tun_decap;
extern struct mlx5e_tc_act mlx5e_tc_act_csum;
extern struct mlx5e_tc_act mlx5e_tc_act_pedit;
extern struct mlx5e_tc_act mlx5e_tc_act_vlan;
extern struct mlx5e_tc_act mlx5e_tc_act_vlan_mangle;
extern struct mlx5e_tc_act mlx5e_tc_act_mpls_push;
extern struct mlx5e_tc_act mlx5e_tc_act_mpls_pop;
extern struct mlx5e_tc_act mlx5e_tc_act_mirred;
extern struct mlx5e_tc_act mlx5e_tc_act_redirect;
extern struct mlx5e_tc_act mlx5e_tc_act_mirred_nic;
extern struct mlx5e_tc_act mlx5e_tc_act_ct;
extern struct mlx5e_tc_act mlx5e_tc_act_sample;
extern struct mlx5e_tc_act mlx5e_tc_act_ptype;
extern struct mlx5e_tc_act mlx5e_tc_act_redirect_ingress;
extern struct mlx5e_tc_act mlx5e_tc_act_police;

struct mlx5e_tc_act *
mlx5e_tc_act_get(enum flow_action_id act_id,
		 enum mlx5_flow_namespace_type ns_type);

void
mlx5e_tc_act_init_parse_state(struct mlx5e_tc_act_parse_state *parse_state,
			      struct mlx5e_tc_flow *flow,
			      struct flow_action *flow_action,
			      struct netlink_ext_ack *extack);

int
mlx5e_tc_act_post_parse(struct mlx5e_tc_act_parse_state *parse_state,
			struct flow_action *flow_action, int from, int to,
			struct mlx5_flow_attr *attr,
			enum mlx5_flow_namespace_type ns_type);

int
mlx5e_tc_act_set_next_post_act(struct mlx5e_tc_flow *flow,
			       struct mlx5_flow_attr *attr,
			       struct mlx5_flow_attr *next_attr);

#endif /* __MLX5_EN_TC_ACT_H__ */
