// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2019 Mellanox Technologies */

#include <devlink.h>

#include "mlx5_core.h"
#include "fw_reset.h"
#include "fs_core.h"
#include "eswitch.h"
#include "esw/qos.h"
#include "sf/dev/dev.h"
#include "sf/sf.h"

static int mlx5_devlink_flash_update(struct devlink *devlink,
				     struct devlink_flash_update_params *params,
				     struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	return mlx5_firmware_flash(dev, params->fw, extack);
}

static u8 mlx5_fw_ver_major(u32 version)
{
	return (version >> 24) & 0xff;
}

static u8 mlx5_fw_ver_minor(u32 version)
{
	return (version >> 16) & 0xff;
}

static u16 mlx5_fw_ver_subminor(u32 version)
{
	return version & 0xffff;
}

#define DEVLINK_FW_STRING_LEN 32

static int
mlx5_devlink_info_get(struct devlink *devlink, struct devlink_info_req *req,
		      struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	char version_str[DEVLINK_FW_STRING_LEN];
	u32 running_fw, stored_fw;
	int err;

	err = devlink_info_version_fixed_put(req, "fw.psid", dev->board_id);
	if (err)
		return err;

	err = mlx5_fw_version_query(dev, &running_fw, &stored_fw);
	if (err)
		return err;

	snprintf(version_str, sizeof(version_str), "%d.%d.%04d",
		 mlx5_fw_ver_major(running_fw), mlx5_fw_ver_minor(running_fw),
		 mlx5_fw_ver_subminor(running_fw));
	err = devlink_info_version_running_put(req, "fw.version", version_str);
	if (err)
		return err;
	err = devlink_info_version_running_put(req,
					       DEVLINK_INFO_VERSION_GENERIC_FW,
					       version_str);
	if (err)
		return err;

	/* no pending version, return running (stored) version */
	if (stored_fw == 0)
		stored_fw = running_fw;

	snprintf(version_str, sizeof(version_str), "%d.%d.%04d",
		 mlx5_fw_ver_major(stored_fw), mlx5_fw_ver_minor(stored_fw),
		 mlx5_fw_ver_subminor(stored_fw));
	err = devlink_info_version_stored_put(req, "fw.version", version_str);
	if (err)
		return err;
	return devlink_info_version_stored_put(req,
					       DEVLINK_INFO_VERSION_GENERIC_FW,
					       version_str);
}

static int mlx5_devlink_reload_fw_activate(struct devlink *devlink, struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	u8 reset_level, reset_type, net_port_alive;
	int err;

	err = mlx5_fw_reset_query(dev, &reset_level, &reset_type);
	if (err)
		return err;
	if (!(reset_level & MLX5_MFRL_REG_RESET_LEVEL3)) {
		NL_SET_ERR_MSG_MOD(extack, "FW activate requires reboot");
		return -EINVAL;
	}

	net_port_alive = !!(reset_type & MLX5_MFRL_REG_RESET_TYPE_NET_PORT_ALIVE);
	err = mlx5_fw_reset_set_reset_sync(dev, net_port_alive, extack);
	if (err)
		return err;

	err = mlx5_fw_reset_wait_reset_done(dev);
	if (err)
		return err;

	mlx5_unload_one_devl_locked(dev, true);
	err = mlx5_health_wait_pci_up(dev);
	if (err)
		NL_SET_ERR_MSG_MOD(extack, "FW activate aborted, PCI reads fail after reset");

	return err;
}

static int mlx5_devlink_trigger_fw_live_patch(struct devlink *devlink,
					      struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	u8 reset_level;
	int err;

	err = mlx5_fw_reset_query(dev, &reset_level, NULL);
	if (err)
		return err;
	if (!(reset_level & MLX5_MFRL_REG_RESET_LEVEL0)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "FW upgrade to the stored FW can't be done by FW live patching");
		return -EINVAL;
	}

	return mlx5_fw_reset_set_live_patch(dev);
}

static int mlx5_devlink_reload_down(struct devlink *devlink, bool netns_change,
				    enum devlink_reload_action action,
				    enum devlink_reload_limit limit,
				    struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	struct pci_dev *pdev = dev->pdev;
	bool sf_dev_allocated;
	int ret = 0;

	if (mlx5_dev_is_lightweight(dev)) {
		if (action != DEVLINK_RELOAD_ACTION_DRIVER_REINIT)
			return -EOPNOTSUPP;
		mlx5_unload_one_light(dev);
		return 0;
	}

	sf_dev_allocated = mlx5_sf_dev_allocated(dev);
	if (sf_dev_allocated) {
		/* Reload results in deleting SF device which further results in
		 * unregistering devlink instance while holding devlink_mutext.
		 * Hence, do not support reload.
		 */
		NL_SET_ERR_MSG_MOD(extack, "reload is unsupported when SFs are allocated");
		return -EOPNOTSUPP;
	}

	if (mlx5_lag_is_active(dev)) {
		NL_SET_ERR_MSG_MOD(extack, "reload is unsupported in Lag mode");
		return -EOPNOTSUPP;
	}

	if (mlx5_core_is_mp_slave(dev)) {
		NL_SET_ERR_MSG_MOD(extack, "reload is unsupported for multi port slave");
		return -EOPNOTSUPP;
	}

	if (mlx5_core_is_pf(dev) && pci_num_vf(pdev))
		NL_SET_ERR_MSG_MOD(extack, "reload while VFs are present is unfavorable");

	switch (action) {
	case DEVLINK_RELOAD_ACTION_DRIVER_REINIT:
		mlx5_unload_one_devl_locked(dev, false);
		break;
	case DEVLINK_RELOAD_ACTION_FW_ACTIVATE:
		if (limit == DEVLINK_RELOAD_LIMIT_NO_RESET)
			ret = mlx5_devlink_trigger_fw_live_patch(devlink, extack);
		else
			ret = mlx5_devlink_reload_fw_activate(devlink, extack);
		break;
	default:
		/* Unsupported action should not get to this function */
		WARN_ON(1);
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static int mlx5_devlink_reload_up(struct devlink *devlink, enum devlink_reload_action action,
				  enum devlink_reload_limit limit, u32 *actions_performed,
				  struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	int ret = 0;

	*actions_performed = BIT(action);
	switch (action) {
	case DEVLINK_RELOAD_ACTION_DRIVER_REINIT:
		if (mlx5_dev_is_lightweight(dev)) {
			mlx5_fw_reporters_create(dev);
			return mlx5_init_one_devl_locked(dev);
		}
		ret = mlx5_load_one_devl_locked(dev, false);
		break;
	case DEVLINK_RELOAD_ACTION_FW_ACTIVATE:
		if (limit == DEVLINK_RELOAD_LIMIT_NO_RESET)
			break;
		/* On fw_activate action, also driver is reloaded and reinit performed */
		*actions_performed |= BIT(DEVLINK_RELOAD_ACTION_DRIVER_REINIT);
		ret = mlx5_load_one_devl_locked(dev, true);
		if (ret)
			return ret;
		ret = mlx5_fw_reset_verify_fw_complete(dev, extack);
		break;
	default:
		/* Unsupported action should not get to this function */
		WARN_ON(1);
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static struct mlx5_devlink_trap *mlx5_find_trap_by_id(struct mlx5_core_dev *dev, int trap_id)
{
	struct mlx5_devlink_trap *dl_trap;

	list_for_each_entry(dl_trap, &dev->priv.traps, list)
		if (dl_trap->trap.id == trap_id)
			return dl_trap;

	return NULL;
}

static int mlx5_devlink_trap_init(struct devlink *devlink, const struct devlink_trap *trap,
				  void *trap_ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	struct mlx5_devlink_trap *dl_trap;

	dl_trap = kzalloc(sizeof(*dl_trap), GFP_KERNEL);
	if (!dl_trap)
		return -ENOMEM;

	dl_trap->trap.id = trap->id;
	dl_trap->trap.action = DEVLINK_TRAP_ACTION_DROP;
	dl_trap->item = trap_ctx;

	if (mlx5_find_trap_by_id(dev, trap->id)) {
		kfree(dl_trap);
		mlx5_core_err(dev, "Devlink trap: Trap 0x%x already found", trap->id);
		return -EEXIST;
	}

	list_add_tail(&dl_trap->list, &dev->priv.traps);
	return 0;
}

static void mlx5_devlink_trap_fini(struct devlink *devlink, const struct devlink_trap *trap,
				   void *trap_ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	struct mlx5_devlink_trap *dl_trap;

	dl_trap = mlx5_find_trap_by_id(dev, trap->id);
	if (!dl_trap) {
		mlx5_core_err(dev, "Devlink trap: Missing trap id 0x%x", trap->id);
		return;
	}
	list_del(&dl_trap->list);
	kfree(dl_trap);
}

static int mlx5_devlink_trap_action_set(struct devlink *devlink,
					const struct devlink_trap *trap,
					enum devlink_trap_action action,
					struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	struct mlx5_devlink_trap_event_ctx trap_event_ctx;
	enum devlink_trap_action action_orig;
	struct mlx5_devlink_trap *dl_trap;
	int err;

	if (is_mdev_switchdev_mode(dev)) {
		NL_SET_ERR_MSG_MOD(extack, "Devlink traps can't be set in switchdev mode");
		return -EOPNOTSUPP;
	}

	dl_trap = mlx5_find_trap_by_id(dev, trap->id);
	if (!dl_trap) {
		mlx5_core_err(dev, "Devlink trap: Set action on invalid trap id 0x%x", trap->id);
		return -EINVAL;
	}

	if (action != DEVLINK_TRAP_ACTION_DROP && action != DEVLINK_TRAP_ACTION_TRAP)
		return -EOPNOTSUPP;

	if (action == dl_trap->trap.action)
		return 0;

	action_orig = dl_trap->trap.action;
	dl_trap->trap.action = action;
	trap_event_ctx.trap = &dl_trap->trap;
	trap_event_ctx.err = 0;
	err = mlx5_blocking_notifier_call_chain(dev, MLX5_DRIVER_EVENT_TYPE_TRAP,
						&trap_event_ctx);
	if (err == NOTIFY_BAD)
		dl_trap->trap.action = action_orig;

	return trap_event_ctx.err;
}

static const struct devlink_ops mlx5_devlink_ops = {
#ifdef CONFIG_MLX5_ESWITCH
	.eswitch_mode_set = mlx5_devlink_eswitch_mode_set,
	.eswitch_mode_get = mlx5_devlink_eswitch_mode_get,
	.eswitch_inline_mode_set = mlx5_devlink_eswitch_inline_mode_set,
	.eswitch_inline_mode_get = mlx5_devlink_eswitch_inline_mode_get,
	.eswitch_encap_mode_set = mlx5_devlink_eswitch_encap_mode_set,
	.eswitch_encap_mode_get = mlx5_devlink_eswitch_encap_mode_get,
	.rate_leaf_tx_share_set = mlx5_esw_devlink_rate_leaf_tx_share_set,
	.rate_leaf_tx_max_set = mlx5_esw_devlink_rate_leaf_tx_max_set,
	.rate_node_tx_share_set = mlx5_esw_devlink_rate_node_tx_share_set,
	.rate_node_tx_max_set = mlx5_esw_devlink_rate_node_tx_max_set,
	.rate_node_new = mlx5_esw_devlink_rate_node_new,
	.rate_node_del = mlx5_esw_devlink_rate_node_del,
	.rate_leaf_parent_set = mlx5_esw_devlink_rate_parent_set,
#endif
#ifdef CONFIG_MLX5_SF_MANAGER
	.port_new = mlx5_devlink_sf_port_new,
#endif
	.flash_update = mlx5_devlink_flash_update,
	.info_get = mlx5_devlink_info_get,
	.reload_actions = BIT(DEVLINK_RELOAD_ACTION_DRIVER_REINIT) |
			  BIT(DEVLINK_RELOAD_ACTION_FW_ACTIVATE),
	.reload_limits = BIT(DEVLINK_RELOAD_LIMIT_NO_RESET),
	.reload_down = mlx5_devlink_reload_down,
	.reload_up = mlx5_devlink_reload_up,
	.trap_init = mlx5_devlink_trap_init,
	.trap_fini = mlx5_devlink_trap_fini,
	.trap_action_set = mlx5_devlink_trap_action_set,
};

void mlx5_devlink_trap_report(struct mlx5_core_dev *dev, int trap_id, struct sk_buff *skb,
			      struct devlink_port *dl_port)
{
	struct devlink *devlink = priv_to_devlink(dev);
	struct mlx5_devlink_trap *dl_trap;

	dl_trap = mlx5_find_trap_by_id(dev, trap_id);
	if (!dl_trap) {
		mlx5_core_err(dev, "Devlink trap: Report on invalid trap id 0x%x", trap_id);
		return;
	}

	if (dl_trap->trap.action != DEVLINK_TRAP_ACTION_TRAP) {
		mlx5_core_dbg(dev, "Devlink trap: Trap id %d has action %d", trap_id,
			      dl_trap->trap.action);
		return;
	}
	devlink_trap_report(devlink, skb, dl_trap->item, dl_port, NULL);
}

int mlx5_devlink_trap_get_num_active(struct mlx5_core_dev *dev)
{
	struct mlx5_devlink_trap *dl_trap;
	int count = 0;

	list_for_each_entry(dl_trap, &dev->priv.traps, list)
		if (dl_trap->trap.action == DEVLINK_TRAP_ACTION_TRAP)
			count++;

	return count;
}

int mlx5_devlink_traps_get_action(struct mlx5_core_dev *dev, int trap_id,
				  enum devlink_trap_action *action)
{
	struct mlx5_devlink_trap *dl_trap;

	dl_trap = mlx5_find_trap_by_id(dev, trap_id);
	if (!dl_trap) {
		mlx5_core_err(dev, "Devlink trap: Get action on invalid trap id 0x%x",
			      trap_id);
		return -EINVAL;
	}

	*action = dl_trap->trap.action;
	return 0;
}

struct devlink *mlx5_devlink_alloc(struct device *dev)
{
	return devlink_alloc(&mlx5_devlink_ops, sizeof(struct mlx5_core_dev),
			     dev);
}

void mlx5_devlink_free(struct devlink *devlink)
{
	devlink_free(devlink);
}

static int mlx5_devlink_enable_roce_validate(struct devlink *devlink, u32 id,
					     union devlink_param_value val,
					     struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	bool new_state = val.vbool;

	if (new_state && !MLX5_CAP_GEN(dev, roce) &&
	    !(MLX5_CAP_GEN(dev, roce_rw_supported) && MLX5_CAP_GEN_MAX(dev, roce))) {
		NL_SET_ERR_MSG_MOD(extack, "Device doesn't support RoCE");
		return -EOPNOTSUPP;
	}
	if (mlx5_core_is_mp_slave(dev) || mlx5_lag_is_active(dev)) {
		NL_SET_ERR_MSG_MOD(extack, "Multi port slave/Lag device can't configure RoCE");
		return -EOPNOTSUPP;
	}

	return 0;
}

#ifdef CONFIG_MLX5_ESWITCH
static int mlx5_devlink_large_group_num_validate(struct devlink *devlink, u32 id,
						 union devlink_param_value val,
						 struct netlink_ext_ack *extack)
{
	int group_num = val.vu32;

	if (group_num < 1 || group_num > 1024) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Unsupported group number, supported range is 1-1024");
		return -EOPNOTSUPP;
	}

	return 0;
}
#endif

static int mlx5_devlink_eq_depth_validate(struct devlink *devlink, u32 id,
					  union devlink_param_value val,
					  struct netlink_ext_ack *extack)
{
	return (val.vu32 >= 64 && val.vu32 <= 4096) ? 0 : -EINVAL;
}

static int
mlx5_devlink_hairpin_num_queues_validate(struct devlink *devlink, u32 id,
					 union devlink_param_value val,
					 struct netlink_ext_ack *extack)
{
	return val.vu32 ? 0 : -EINVAL;
}

static int
mlx5_devlink_hairpin_queue_size_validate(struct devlink *devlink, u32 id,
					 union devlink_param_value val,
					 struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	u32 val32 = val.vu32;

	if (!is_power_of_2(val32)) {
		NL_SET_ERR_MSG_MOD(extack, "Value is not power of two");
		return -EINVAL;
	}

	if (val32 > BIT(MLX5_CAP_GEN(dev, log_max_hairpin_num_packets))) {
		NL_SET_ERR_MSG_FMT_MOD(
			extack, "Maximum hairpin queue size is %lu",
			BIT(MLX5_CAP_GEN(dev, log_max_hairpin_num_packets)));
		return -EINVAL;
	}

	return 0;
}

static void mlx5_devlink_hairpin_params_init_values(struct devlink *devlink)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	union devlink_param_value value;
	u32 link_speed = 0;
	u64 link_speed64;

	/* set hairpin pair per each 50Gbs share of the link */
	mlx5_port_max_linkspeed(dev, &link_speed);
	link_speed = max_t(u32, link_speed, 50000);
	link_speed64 = link_speed;
	do_div(link_speed64, 50000);

	value.vu32 = link_speed64;
	devl_param_driverinit_value_set(
		devlink, MLX5_DEVLINK_PARAM_ID_HAIRPIN_NUM_QUEUES, value);

	value.vu32 =
		BIT(min_t(u32, 16 - MLX5_MPWRQ_MIN_LOG_STRIDE_SZ(dev),
			  MLX5_CAP_GEN(dev, log_max_hairpin_num_packets)));
	devl_param_driverinit_value_set(
		devlink, MLX5_DEVLINK_PARAM_ID_HAIRPIN_QUEUE_SIZE, value);
}

static const struct devlink_param mlx5_devlink_params[] = {
	DEVLINK_PARAM_GENERIC(ENABLE_ROCE, BIT(DEVLINK_PARAM_CMODE_DRIVERINIT),
			      NULL, NULL, mlx5_devlink_enable_roce_validate),
#ifdef CONFIG_MLX5_ESWITCH
	DEVLINK_PARAM_DRIVER(MLX5_DEVLINK_PARAM_ID_ESW_LARGE_GROUP_NUM,
			     "fdb_large_groups", DEVLINK_PARAM_TYPE_U32,
			     BIT(DEVLINK_PARAM_CMODE_DRIVERINIT),
			     NULL, NULL,
			     mlx5_devlink_large_group_num_validate),
#endif
	DEVLINK_PARAM_GENERIC(IO_EQ_SIZE, BIT(DEVLINK_PARAM_CMODE_DRIVERINIT),
			      NULL, NULL, mlx5_devlink_eq_depth_validate),
	DEVLINK_PARAM_GENERIC(EVENT_EQ_SIZE, BIT(DEVLINK_PARAM_CMODE_DRIVERINIT),
			      NULL, NULL, mlx5_devlink_eq_depth_validate),
};

static void mlx5_devlink_set_params_init_values(struct devlink *devlink)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	union devlink_param_value value;

	value.vbool = MLX5_CAP_GEN(dev, roce) && !mlx5_dev_is_lightweight(dev);
	devl_param_driverinit_value_set(devlink,
					DEVLINK_PARAM_GENERIC_ID_ENABLE_ROCE,
					value);

#ifdef CONFIG_MLX5_ESWITCH
	value.vu32 = ESW_OFFLOADS_DEFAULT_NUM_GROUPS;
	devl_param_driverinit_value_set(devlink,
					MLX5_DEVLINK_PARAM_ID_ESW_LARGE_GROUP_NUM,
					value);
#endif

	value.vu32 = MLX5_COMP_EQ_SIZE;
	devl_param_driverinit_value_set(devlink,
					DEVLINK_PARAM_GENERIC_ID_IO_EQ_SIZE,
					value);

	value.vu32 = MLX5_NUM_ASYNC_EQE;
	devl_param_driverinit_value_set(devlink,
					DEVLINK_PARAM_GENERIC_ID_EVENT_EQ_SIZE,
					value);
}

static const struct devlink_param mlx5_devlink_eth_params[] = {
	DEVLINK_PARAM_GENERIC(ENABLE_ETH, BIT(DEVLINK_PARAM_CMODE_DRIVERINIT),
			      NULL, NULL, NULL),
	DEVLINK_PARAM_DRIVER(MLX5_DEVLINK_PARAM_ID_HAIRPIN_NUM_QUEUES,
			     "hairpin_num_queues", DEVLINK_PARAM_TYPE_U32,
			     BIT(DEVLINK_PARAM_CMODE_DRIVERINIT), NULL, NULL,
			     mlx5_devlink_hairpin_num_queues_validate),
	DEVLINK_PARAM_DRIVER(MLX5_DEVLINK_PARAM_ID_HAIRPIN_QUEUE_SIZE,
			     "hairpin_queue_size", DEVLINK_PARAM_TYPE_U32,
			     BIT(DEVLINK_PARAM_CMODE_DRIVERINIT), NULL, NULL,
			     mlx5_devlink_hairpin_queue_size_validate),
};

static int mlx5_devlink_eth_params_register(struct devlink *devlink)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	union devlink_param_value value;
	int err;

	if (!mlx5_eth_supported(dev))
		return 0;

	err = devl_params_register(devlink, mlx5_devlink_eth_params,
				   ARRAY_SIZE(mlx5_devlink_eth_params));
	if (err)
		return err;

	value.vbool = !mlx5_dev_is_lightweight(dev);
	devl_param_driverinit_value_set(devlink,
					DEVLINK_PARAM_GENERIC_ID_ENABLE_ETH,
					value);

	mlx5_devlink_hairpin_params_init_values(devlink);

	return 0;
}

static void mlx5_devlink_eth_params_unregister(struct devlink *devlink)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	if (!mlx5_eth_supported(dev))
		return;

	devl_params_unregister(devlink, mlx5_devlink_eth_params,
			       ARRAY_SIZE(mlx5_devlink_eth_params));
}

static int mlx5_devlink_enable_rdma_validate(struct devlink *devlink, u32 id,
					     union devlink_param_value val,
					     struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	bool new_state = val.vbool;

	if (new_state && !mlx5_rdma_supported(dev))
		return -EOPNOTSUPP;
	return 0;
}

static const struct devlink_param mlx5_devlink_rdma_params[] = {
	DEVLINK_PARAM_GENERIC(ENABLE_RDMA, BIT(DEVLINK_PARAM_CMODE_DRIVERINIT),
			      NULL, NULL, mlx5_devlink_enable_rdma_validate),
};

static int mlx5_devlink_rdma_params_register(struct devlink *devlink)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	union devlink_param_value value;
	int err;

	if (!IS_ENABLED(CONFIG_MLX5_INFINIBAND))
		return 0;

	err = devl_params_register(devlink, mlx5_devlink_rdma_params,
				   ARRAY_SIZE(mlx5_devlink_rdma_params));
	if (err)
		return err;

	value.vbool = !mlx5_dev_is_lightweight(dev);
	devl_param_driverinit_value_set(devlink,
					DEVLINK_PARAM_GENERIC_ID_ENABLE_RDMA,
					value);
	return 0;
}

static void mlx5_devlink_rdma_params_unregister(struct devlink *devlink)
{
	if (!IS_ENABLED(CONFIG_MLX5_INFINIBAND))
		return;

	devl_params_unregister(devlink, mlx5_devlink_rdma_params,
			       ARRAY_SIZE(mlx5_devlink_rdma_params));
}

static const struct devlink_param mlx5_devlink_vnet_params[] = {
	DEVLINK_PARAM_GENERIC(ENABLE_VNET, BIT(DEVLINK_PARAM_CMODE_DRIVERINIT),
			      NULL, NULL, NULL),
};

static int mlx5_devlink_vnet_params_register(struct devlink *devlink)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	union devlink_param_value value;
	int err;

	if (!mlx5_vnet_supported(dev))
		return 0;

	err = devl_params_register(devlink, mlx5_devlink_vnet_params,
				   ARRAY_SIZE(mlx5_devlink_vnet_params));
	if (err)
		return err;

	value.vbool = !mlx5_dev_is_lightweight(dev);
	devl_param_driverinit_value_set(devlink,
					DEVLINK_PARAM_GENERIC_ID_ENABLE_VNET,
					value);
	return 0;
}

static void mlx5_devlink_vnet_params_unregister(struct devlink *devlink)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	if (!mlx5_vnet_supported(dev))
		return;

	devl_params_unregister(devlink, mlx5_devlink_vnet_params,
			       ARRAY_SIZE(mlx5_devlink_vnet_params));
}

static int mlx5_devlink_auxdev_params_register(struct devlink *devlink)
{
	int err;

	err = mlx5_devlink_eth_params_register(devlink);
	if (err)
		return err;

	err = mlx5_devlink_rdma_params_register(devlink);
	if (err)
		goto rdma_err;

	err = mlx5_devlink_vnet_params_register(devlink);
	if (err)
		goto vnet_err;
	return 0;

vnet_err:
	mlx5_devlink_rdma_params_unregister(devlink);
rdma_err:
	mlx5_devlink_eth_params_unregister(devlink);
	return err;
}

static void mlx5_devlink_auxdev_params_unregister(struct devlink *devlink)
{
	mlx5_devlink_vnet_params_unregister(devlink);
	mlx5_devlink_rdma_params_unregister(devlink);
	mlx5_devlink_eth_params_unregister(devlink);
}

static int mlx5_devlink_max_uc_list_validate(struct devlink *devlink, u32 id,
					     union devlink_param_value val,
					     struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	if (val.vu32 == 0) {
		NL_SET_ERR_MSG_MOD(extack, "max_macs value must be greater than 0");
		return -EINVAL;
	}

	if (!is_power_of_2(val.vu32)) {
		NL_SET_ERR_MSG_MOD(extack, "Only power of 2 values are supported for max_macs");
		return -EINVAL;
	}

	if (ilog2(val.vu32) >
	    MLX5_CAP_GEN_MAX(dev, log_max_current_uc_list)) {
		NL_SET_ERR_MSG_MOD(extack, "max_macs value is out of the supported range");
		return -EINVAL;
	}

	return 0;
}

static const struct devlink_param mlx5_devlink_max_uc_list_params[] = {
	DEVLINK_PARAM_GENERIC(MAX_MACS, BIT(DEVLINK_PARAM_CMODE_DRIVERINIT),
			      NULL, NULL, mlx5_devlink_max_uc_list_validate),
};

static int mlx5_devlink_max_uc_list_params_register(struct devlink *devlink)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);
	union devlink_param_value value;
	int err;

	if (!MLX5_CAP_GEN_MAX(dev, log_max_current_uc_list_wr_supported))
		return 0;

	err = devl_params_register(devlink, mlx5_devlink_max_uc_list_params,
				   ARRAY_SIZE(mlx5_devlink_max_uc_list_params));
	if (err)
		return err;

	value.vu32 = 1 << MLX5_CAP_GEN(dev, log_max_current_uc_list);
	devl_param_driverinit_value_set(devlink,
					DEVLINK_PARAM_GENERIC_ID_MAX_MACS,
					value);
	return 0;
}

static void
mlx5_devlink_max_uc_list_params_unregister(struct devlink *devlink)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	if (!MLX5_CAP_GEN_MAX(dev, log_max_current_uc_list_wr_supported))
		return;

	devl_params_unregister(devlink, mlx5_devlink_max_uc_list_params,
			       ARRAY_SIZE(mlx5_devlink_max_uc_list_params));
}

#define MLX5_TRAP_DROP(_id, _group_id)					\
	DEVLINK_TRAP_GENERIC(DROP, DROP, _id,				\
			     DEVLINK_TRAP_GROUP_GENERIC_ID_##_group_id, \
			     DEVLINK_TRAP_METADATA_TYPE_F_IN_PORT)

static const struct devlink_trap mlx5_traps_arr[] = {
	MLX5_TRAP_DROP(INGRESS_VLAN_FILTER, L2_DROPS),
	MLX5_TRAP_DROP(DMAC_FILTER, L2_DROPS),
};

static const struct devlink_trap_group mlx5_trap_groups_arr[] = {
	DEVLINK_TRAP_GROUP_GENERIC(L2_DROPS, 0),
};

int mlx5_devlink_traps_register(struct devlink *devlink)
{
	struct mlx5_core_dev *core_dev = devlink_priv(devlink);
	int err;

	err = devl_trap_groups_register(devlink, mlx5_trap_groups_arr,
					ARRAY_SIZE(mlx5_trap_groups_arr));
	if (err)
		return err;

	err = devl_traps_register(devlink, mlx5_traps_arr, ARRAY_SIZE(mlx5_traps_arr),
				  &core_dev->priv);
	if (err)
		goto err_trap_group;
	return 0;

err_trap_group:
	devl_trap_groups_unregister(devlink, mlx5_trap_groups_arr,
				    ARRAY_SIZE(mlx5_trap_groups_arr));
	return err;
}

void mlx5_devlink_traps_unregister(struct devlink *devlink)
{
	devl_traps_unregister(devlink, mlx5_traps_arr, ARRAY_SIZE(mlx5_traps_arr));
	devl_trap_groups_unregister(devlink, mlx5_trap_groups_arr,
				    ARRAY_SIZE(mlx5_trap_groups_arr));
}

int mlx5_devlink_params_register(struct devlink *devlink)
{
	int err;

	/* Here only the driver init params should be registered.
	 * Runtime params should be registered by the code which
	 * behaviour they configure.
	 */

	err = devl_params_register(devlink, mlx5_devlink_params,
				   ARRAY_SIZE(mlx5_devlink_params));
	if (err)
		return err;

	mlx5_devlink_set_params_init_values(devlink);

	err = mlx5_devlink_auxdev_params_register(devlink);
	if (err)
		goto auxdev_reg_err;

	err = mlx5_devlink_max_uc_list_params_register(devlink);
	if (err)
		goto max_uc_list_err;

	return 0;

max_uc_list_err:
	mlx5_devlink_auxdev_params_unregister(devlink);
auxdev_reg_err:
	devl_params_unregister(devlink, mlx5_devlink_params,
			       ARRAY_SIZE(mlx5_devlink_params));
	return err;
}

void mlx5_devlink_params_unregister(struct devlink *devlink)
{
	mlx5_devlink_max_uc_list_params_unregister(devlink);
	mlx5_devlink_auxdev_params_unregister(devlink);
	devl_params_unregister(devlink, mlx5_devlink_params,
			       ARRAY_SIZE(mlx5_devlink_params));
}
