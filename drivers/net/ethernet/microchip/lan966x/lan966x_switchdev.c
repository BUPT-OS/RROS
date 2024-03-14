// SPDX-License-Identifier: GPL-2.0+

#include <linux/if_bridge.h>
#include <net/switchdev.h>

#include "lan966x_main.h"

static struct notifier_block lan966x_netdevice_nb __read_mostly;

static void lan966x_port_set_mcast_ip_flood(struct lan966x_port *port,
					    u32 pgid_ip)
{
	struct lan966x *lan966x = port->lan966x;
	u32 flood_mask_ip;

	flood_mask_ip = lan_rd(lan966x, ANA_PGID(pgid_ip));
	flood_mask_ip = ANA_PGID_PGID_GET(flood_mask_ip);

	/* If mcast snooping is not enabled then use mcast flood mask
	 * to decide to enable multicast flooding or not.
	 */
	if (!port->mcast_ena) {
		u32 flood_mask;

		flood_mask = lan_rd(lan966x, ANA_PGID(PGID_MC));
		flood_mask = ANA_PGID_PGID_GET(flood_mask);

		if (flood_mask & BIT(port->chip_port))
			flood_mask_ip |= BIT(port->chip_port);
		else
			flood_mask_ip &= ~BIT(port->chip_port);
	} else {
		flood_mask_ip &= ~BIT(port->chip_port);
	}

	lan_rmw(ANA_PGID_PGID_SET(flood_mask_ip),
		ANA_PGID_PGID,
		lan966x, ANA_PGID(pgid_ip));
}

static void lan966x_port_set_mcast_flood(struct lan966x_port *port,
					 bool enabled)
{
	u32 val = lan_rd(port->lan966x, ANA_PGID(PGID_MC));

	val = ANA_PGID_PGID_GET(val);
	if (enabled)
		val |= BIT(port->chip_port);
	else
		val &= ~BIT(port->chip_port);

	lan_rmw(ANA_PGID_PGID_SET(val),
		ANA_PGID_PGID,
		port->lan966x, ANA_PGID(PGID_MC));

	if (!port->mcast_ena) {
		lan966x_port_set_mcast_ip_flood(port, PGID_MCIPV4);
		lan966x_port_set_mcast_ip_flood(port, PGID_MCIPV6);
	}
}

static void lan966x_port_set_ucast_flood(struct lan966x_port *port,
					 bool enabled)
{
	u32 val = lan_rd(port->lan966x, ANA_PGID(PGID_UC));

	val = ANA_PGID_PGID_GET(val);
	if (enabled)
		val |= BIT(port->chip_port);
	else
		val &= ~BIT(port->chip_port);

	lan_rmw(ANA_PGID_PGID_SET(val),
		ANA_PGID_PGID,
		port->lan966x, ANA_PGID(PGID_UC));
}

static void lan966x_port_set_bcast_flood(struct lan966x_port *port,
					 bool enabled)
{
	u32 val = lan_rd(port->lan966x, ANA_PGID(PGID_BC));

	val = ANA_PGID_PGID_GET(val);
	if (enabled)
		val |= BIT(port->chip_port);
	else
		val &= ~BIT(port->chip_port);

	lan_rmw(ANA_PGID_PGID_SET(val),
		ANA_PGID_PGID,
		port->lan966x, ANA_PGID(PGID_BC));
}

static void lan966x_port_set_learning(struct lan966x_port *port, bool enabled)
{
	lan_rmw(ANA_PORT_CFG_LEARN_ENA_SET(enabled),
		ANA_PORT_CFG_LEARN_ENA,
		port->lan966x, ANA_PORT_CFG(port->chip_port));

	port->learn_ena = enabled;
}

static void lan966x_port_bridge_flags(struct lan966x_port *port,
				      struct switchdev_brport_flags flags)
{
	if (flags.mask & BR_MCAST_FLOOD)
		lan966x_port_set_mcast_flood(port,
					     !!(flags.val & BR_MCAST_FLOOD));

	if (flags.mask & BR_FLOOD)
		lan966x_port_set_ucast_flood(port,
					     !!(flags.val & BR_FLOOD));

	if (flags.mask & BR_BCAST_FLOOD)
		lan966x_port_set_bcast_flood(port,
					     !!(flags.val & BR_BCAST_FLOOD));

	if (flags.mask & BR_LEARNING)
		lan966x_port_set_learning(port,
					  !!(flags.val & BR_LEARNING));
}

static int lan966x_port_pre_bridge_flags(struct lan966x_port *port,
					 struct switchdev_brport_flags flags)
{
	if (flags.mask & ~(BR_MCAST_FLOOD | BR_FLOOD | BR_BCAST_FLOOD |
			   BR_LEARNING))
		return -EINVAL;

	return 0;
}

void lan966x_update_fwd_mask(struct lan966x *lan966x)
{
	int i;

	for (i = 0; i < lan966x->num_phys_ports; i++) {
		struct lan966x_port *port = lan966x->ports[i];
		unsigned long mask = 0;

		if (port && lan966x->bridge_fwd_mask & BIT(i)) {
			mask = lan966x->bridge_fwd_mask & ~BIT(i);

			if (port->bond)
				mask &= ~lan966x_lag_get_mask(lan966x,
							      port->bond);
		}

		mask |= BIT(CPU_PORT);

		lan_wr(ANA_PGID_PGID_SET(mask),
		       lan966x, ANA_PGID(PGID_SRC + i));
	}
}

void lan966x_port_stp_state_set(struct lan966x_port *port, u8 state)
{
	struct lan966x *lan966x = port->lan966x;
	bool learn_ena = false;

	if ((state == BR_STATE_FORWARDING || state == BR_STATE_LEARNING) &&
	    port->learn_ena)
		learn_ena = true;

	if (state == BR_STATE_FORWARDING)
		lan966x->bridge_fwd_mask |= BIT(port->chip_port);
	else
		lan966x->bridge_fwd_mask &= ~BIT(port->chip_port);

	lan_rmw(ANA_PORT_CFG_LEARN_ENA_SET(learn_ena),
		ANA_PORT_CFG_LEARN_ENA,
		lan966x, ANA_PORT_CFG(port->chip_port));

	lan966x_update_fwd_mask(lan966x);
}

void lan966x_port_ageing_set(struct lan966x_port *port,
			     unsigned long ageing_clock_t)
{
	unsigned long ageing_jiffies = clock_t_to_jiffies(ageing_clock_t);
	u32 ageing_time = jiffies_to_msecs(ageing_jiffies) / 1000;

	lan966x_mac_set_ageing(port->lan966x, ageing_time);
}

static void lan966x_port_mc_set(struct lan966x_port *port, bool mcast_ena)
{
	struct lan966x *lan966x = port->lan966x;

	port->mcast_ena = mcast_ena;
	if (mcast_ena)
		lan966x_mdb_restore_entries(lan966x);
	else
		lan966x_mdb_clear_entries(lan966x);

	lan_rmw(ANA_CPU_FWD_CFG_IGMP_REDIR_ENA_SET(mcast_ena) |
		ANA_CPU_FWD_CFG_MLD_REDIR_ENA_SET(mcast_ena) |
		ANA_CPU_FWD_CFG_IPMC_CTRL_COPY_ENA_SET(mcast_ena),
		ANA_CPU_FWD_CFG_IGMP_REDIR_ENA |
		ANA_CPU_FWD_CFG_MLD_REDIR_ENA |
		ANA_CPU_FWD_CFG_IPMC_CTRL_COPY_ENA,
		lan966x, ANA_CPU_FWD_CFG(port->chip_port));

	lan966x_port_set_mcast_ip_flood(port, PGID_MCIPV4);
	lan966x_port_set_mcast_ip_flood(port, PGID_MCIPV6);
}

static int lan966x_port_attr_set(struct net_device *dev, const void *ctx,
				 const struct switchdev_attr *attr,
				 struct netlink_ext_ack *extack)
{
	struct lan966x_port *port = netdev_priv(dev);
	int err = 0;

	if (ctx && ctx != port)
		return 0;

	switch (attr->id) {
	case SWITCHDEV_ATTR_ID_PORT_BRIDGE_FLAGS:
		lan966x_port_bridge_flags(port, attr->u.brport_flags);
		break;
	case SWITCHDEV_ATTR_ID_PORT_PRE_BRIDGE_FLAGS:
		err = lan966x_port_pre_bridge_flags(port, attr->u.brport_flags);
		break;
	case SWITCHDEV_ATTR_ID_PORT_STP_STATE:
		lan966x_port_stp_state_set(port, attr->u.stp_state);
		break;
	case SWITCHDEV_ATTR_ID_BRIDGE_AGEING_TIME:
		lan966x_port_ageing_set(port, attr->u.ageing_time);
		break;
	case SWITCHDEV_ATTR_ID_BRIDGE_VLAN_FILTERING:
		lan966x_vlan_port_set_vlan_aware(port, attr->u.vlan_filtering);
		lan966x_vlan_port_apply(port);
		break;
	case SWITCHDEV_ATTR_ID_BRIDGE_MC_DISABLED:
		lan966x_port_mc_set(port, !attr->u.mc_disabled);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

static int lan966x_port_bridge_join(struct lan966x_port *port,
				    struct net_device *brport_dev,
				    struct net_device *bridge,
				    struct netlink_ext_ack *extack)
{
	struct switchdev_brport_flags flags = {0};
	struct lan966x *lan966x = port->lan966x;
	struct net_device *dev = port->dev;
	int err;

	if (!lan966x->bridge_mask) {
		lan966x->bridge = bridge;
	} else {
		if (lan966x->bridge != bridge) {
			NL_SET_ERR_MSG_MOD(extack, "Not allow to add port to different bridge");
			return -ENODEV;
		}
	}

	err = switchdev_bridge_port_offload(brport_dev, dev, port,
					    &lan966x_switchdev_nb,
					    &lan966x_switchdev_blocking_nb,
					    false, extack);
	if (err)
		return err;

	lan966x->bridge_mask |= BIT(port->chip_port);

	flags.mask = BR_LEARNING | BR_FLOOD | BR_MCAST_FLOOD | BR_BCAST_FLOOD;
	flags.val = flags.mask;
	lan966x_port_bridge_flags(port, flags);

	return 0;
}

static void lan966x_port_bridge_leave(struct lan966x_port *port,
				      struct net_device *bridge)
{
	struct switchdev_brport_flags flags = {0};
	struct lan966x *lan966x = port->lan966x;

	flags.mask = BR_LEARNING | BR_FLOOD | BR_MCAST_FLOOD | BR_BCAST_FLOOD;
	flags.val = flags.mask & ~BR_LEARNING;
	lan966x_port_bridge_flags(port, flags);

	lan966x->bridge_mask &= ~BIT(port->chip_port);

	if (!lan966x->bridge_mask)
		lan966x->bridge = NULL;

	/* Set the port back to host mode */
	lan966x_vlan_port_set_vlan_aware(port, false);
	lan966x_vlan_port_set_vid(port, HOST_PVID, false, false);
	lan966x_vlan_port_apply(port);
}

int lan966x_port_changeupper(struct net_device *dev,
			     struct net_device *brport_dev,
			     struct netdev_notifier_changeupper_info *info)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct netlink_ext_ack *extack;
	int err = 0;

	extack = netdev_notifier_info_to_extack(&info->info);

	if (netif_is_bridge_master(info->upper_dev)) {
		if (info->linking)
			err = lan966x_port_bridge_join(port, brport_dev,
						       info->upper_dev,
						       extack);
		else
			lan966x_port_bridge_leave(port, info->upper_dev);
	}

	if (netif_is_lag_master(info->upper_dev)) {
		if (info->linking)
			err = lan966x_lag_port_join(port, info->upper_dev,
						    info->upper_dev,
						    extack);
		else
			lan966x_lag_port_leave(port, info->upper_dev);
	}

	return err;
}

int lan966x_port_prechangeupper(struct net_device *dev,
				struct net_device *brport_dev,
				struct netdev_notifier_changeupper_info *info)
{
	struct lan966x_port *port = netdev_priv(dev);
	int err = NOTIFY_DONE;

	if (netif_is_bridge_master(info->upper_dev) && !info->linking) {
		switchdev_bridge_port_unoffload(port->dev, port, NULL, NULL);
		lan966x_fdb_flush_workqueue(port->lan966x);
	}

	if (netif_is_lag_master(info->upper_dev)) {
		err = lan966x_lag_port_prechangeupper(dev, info);
		if (err || info->linking)
			return err;

		switchdev_bridge_port_unoffload(brport_dev, port, NULL, NULL);
		lan966x_fdb_flush_workqueue(port->lan966x);
	}

	return err;
}

static int lan966x_foreign_bridging_check(struct net_device *upper,
					  bool *has_foreign,
					  bool *seen_lan966x,
					  struct netlink_ext_ack *extack)
{
	struct lan966x *lan966x = NULL;
	struct net_device *dev;
	struct list_head *iter;

	if (!netif_is_bridge_master(upper) &&
	    !netif_is_lag_master(upper))
		return 0;

	netdev_for_each_lower_dev(upper, dev, iter) {
		if (lan966x_netdevice_check(dev)) {
			struct lan966x_port *port = netdev_priv(dev);

			if (lan966x) {
				/* Upper already has at least one port of a
				 * lan966x switch inside it, check that it's
				 * the same instance of the driver.
				 */
				if (port->lan966x != lan966x) {
					NL_SET_ERR_MSG_MOD(extack,
							   "Bridging between multiple lan966x switches disallowed");
					return -EINVAL;
				}
			} else {
				/* This is the first lan966x port inside this
				 * upper device
				 */
				lan966x = port->lan966x;
				*seen_lan966x = true;
			}
		} else if (netif_is_lag_master(dev)) {
			/* Allow to have bond interfaces that have only lan966x
			 * devices
			 */
			if (lan966x_foreign_bridging_check(dev, has_foreign,
							   seen_lan966x,
							   extack))
				return -EINVAL;
		} else {
			*has_foreign = true;
		}

		if (*seen_lan966x && *has_foreign) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Bridging lan966x ports with foreign interfaces disallowed");
			return -EINVAL;
		}
	}

	return 0;
}

static int lan966x_bridge_check(struct net_device *dev,
				struct netdev_notifier_changeupper_info *info)
{
	bool has_foreign = false;
	bool seen_lan966x = false;

	return lan966x_foreign_bridging_check(info->upper_dev,
					      &has_foreign,
					      &seen_lan966x,
					      info->info.extack);
}

static int lan966x_netdevice_port_event(struct net_device *dev,
					struct notifier_block *nb,
					unsigned long event, void *ptr)
{
	int err = 0;

	if (!lan966x_netdevice_check(dev)) {
		switch (event) {
		case NETDEV_CHANGEUPPER:
		case NETDEV_PRECHANGEUPPER:
			err = lan966x_bridge_check(dev, ptr);
			if (err)
				return err;

			if (netif_is_lag_master(dev)) {
				if (event == NETDEV_CHANGEUPPER)
					err = lan966x_lag_netdev_changeupper(dev,
									     ptr);
				else
					err = lan966x_lag_netdev_prechangeupper(dev,
										ptr);

				return err;
			}
			break;
		default:
			return 0;
		}

		return 0;
	}

	switch (event) {
	case NETDEV_PRECHANGEUPPER:
		err = lan966x_port_prechangeupper(dev, dev, ptr);
		break;
	case NETDEV_CHANGEUPPER:
		err = lan966x_bridge_check(dev, ptr);
		if (err)
			return err;

		err = lan966x_port_changeupper(dev, dev, ptr);
		break;
	case NETDEV_CHANGELOWERSTATE:
		err = lan966x_lag_port_changelowerstate(dev, ptr);
		break;
	}

	return err;
}

static int lan966x_netdevice_event(struct notifier_block *nb,
				   unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	int ret;

	ret = lan966x_netdevice_port_event(dev, nb, event, ptr);

	return notifier_from_errno(ret);
}

static bool lan966x_foreign_dev_check(const struct net_device *dev,
				      const struct net_device *foreign_dev)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	int i;

	if (netif_is_bridge_master(foreign_dev))
		if (lan966x->bridge == foreign_dev)
			return false;

	if (netif_is_lag_master(foreign_dev))
		for (i = 0; i < lan966x->num_phys_ports; ++i)
			if (lan966x->ports[i] &&
			    lan966x->ports[i]->bond == foreign_dev)
				return false;

	return true;
}

static int lan966x_switchdev_event(struct notifier_block *nb,
				   unsigned long event, void *ptr)
{
	struct net_device *dev = switchdev_notifier_info_to_dev(ptr);
	int err;

	switch (event) {
	case SWITCHDEV_PORT_ATTR_SET:
		err = switchdev_handle_port_attr_set(dev, ptr,
						     lan966x_netdevice_check,
						     lan966x_port_attr_set);
		return notifier_from_errno(err);
	case SWITCHDEV_FDB_ADD_TO_DEVICE:
	case SWITCHDEV_FDB_DEL_TO_DEVICE:
		err = switchdev_handle_fdb_event_to_device(dev, event, ptr,
							   lan966x_netdevice_check,
							   lan966x_foreign_dev_check,
							   lan966x_handle_fdb);
		return notifier_from_errno(err);
	}

	return NOTIFY_DONE;
}

static int lan966x_handle_port_vlan_add(struct lan966x_port *port,
					const struct switchdev_obj *obj)
{
	const struct switchdev_obj_port_vlan *v = SWITCHDEV_OBJ_PORT_VLAN(obj);
	struct lan966x *lan966x = port->lan966x;

	if (!netif_is_bridge_master(obj->orig_dev))
		lan966x_vlan_port_add_vlan(port, v->vid,
					   v->flags & BRIDGE_VLAN_INFO_PVID,
					   v->flags & BRIDGE_VLAN_INFO_UNTAGGED);
	else
		lan966x_vlan_cpu_add_vlan(lan966x, v->vid);

	return 0;
}

static int lan966x_handle_port_obj_add(struct net_device *dev, const void *ctx,
				       const struct switchdev_obj *obj,
				       struct netlink_ext_ack *extack)
{
	struct lan966x_port *port = netdev_priv(dev);
	int err;

	if (ctx && ctx != port)
		return 0;

	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		err = lan966x_handle_port_vlan_add(port, obj);
		break;
	case SWITCHDEV_OBJ_ID_PORT_MDB:
	case SWITCHDEV_OBJ_ID_HOST_MDB:
		err = lan966x_handle_port_mdb_add(port, obj);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

static int lan966x_handle_port_vlan_del(struct lan966x_port *port,
					const struct switchdev_obj *obj)
{
	const struct switchdev_obj_port_vlan *v = SWITCHDEV_OBJ_PORT_VLAN(obj);
	struct lan966x *lan966x = port->lan966x;

	if (!netif_is_bridge_master(obj->orig_dev))
		lan966x_vlan_port_del_vlan(port, v->vid);
	else
		lan966x_vlan_cpu_del_vlan(lan966x, v->vid);

	return 0;
}

static int lan966x_handle_port_obj_del(struct net_device *dev, const void *ctx,
				       const struct switchdev_obj *obj)
{
	struct lan966x_port *port = netdev_priv(dev);
	int err;

	if (ctx && ctx != port)
		return 0;

	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		err = lan966x_handle_port_vlan_del(port, obj);
		break;
	case SWITCHDEV_OBJ_ID_PORT_MDB:
	case SWITCHDEV_OBJ_ID_HOST_MDB:
		err = lan966x_handle_port_mdb_del(port, obj);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

static int lan966x_switchdev_blocking_event(struct notifier_block *nb,
					    unsigned long event,
					    void *ptr)
{
	struct net_device *dev = switchdev_notifier_info_to_dev(ptr);
	int err;

	switch (event) {
	case SWITCHDEV_PORT_OBJ_ADD:
		err = switchdev_handle_port_obj_add(dev, ptr,
						    lan966x_netdevice_check,
						    lan966x_handle_port_obj_add);
		return notifier_from_errno(err);
	case SWITCHDEV_PORT_OBJ_DEL:
		err = switchdev_handle_port_obj_del(dev, ptr,
						    lan966x_netdevice_check,
						    lan966x_handle_port_obj_del);
		return notifier_from_errno(err);
	case SWITCHDEV_PORT_ATTR_SET:
		err = switchdev_handle_port_attr_set(dev, ptr,
						     lan966x_netdevice_check,
						     lan966x_port_attr_set);
		return notifier_from_errno(err);
	}

	return NOTIFY_DONE;
}

static struct notifier_block lan966x_netdevice_nb __read_mostly = {
	.notifier_call = lan966x_netdevice_event,
};

struct notifier_block lan966x_switchdev_nb __read_mostly = {
	.notifier_call = lan966x_switchdev_event,
};

struct notifier_block lan966x_switchdev_blocking_nb __read_mostly = {
	.notifier_call = lan966x_switchdev_blocking_event,
};

void lan966x_register_notifier_blocks(void)
{
	register_netdevice_notifier(&lan966x_netdevice_nb);
	register_switchdev_notifier(&lan966x_switchdev_nb);
	register_switchdev_blocking_notifier(&lan966x_switchdev_blocking_nb);
}

void lan966x_unregister_notifier_blocks(void)
{
	unregister_switchdev_blocking_notifier(&lan966x_switchdev_blocking_nb);
	unregister_switchdev_notifier(&lan966x_switchdev_nb);
	unregister_netdevice_notifier(&lan966x_netdevice_nb);
}
