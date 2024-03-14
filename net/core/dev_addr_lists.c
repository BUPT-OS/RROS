// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * net/core/dev_addr_lists.c - Functions for handling net device lists
 * Copyright (c) 2010 Jiri Pirko <jpirko@redhat.com>
 *
 * This file contains functions for working with unicast, multicast and device
 * addresses lists.
 */

#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/export.h>
#include <linux/list.h>

#include "dev.h"

/*
 * General list handling functions
 */

static int __hw_addr_insert(struct netdev_hw_addr_list *list,
			    struct netdev_hw_addr *new, int addr_len)
{
	struct rb_node **ins_point = &list->tree.rb_node, *parent = NULL;
	struct netdev_hw_addr *ha;

	while (*ins_point) {
		int diff;

		ha = rb_entry(*ins_point, struct netdev_hw_addr, node);
		diff = memcmp(new->addr, ha->addr, addr_len);
		if (diff == 0)
			diff = memcmp(&new->type, &ha->type, sizeof(new->type));

		parent = *ins_point;
		if (diff < 0)
			ins_point = &parent->rb_left;
		else if (diff > 0)
			ins_point = &parent->rb_right;
		else
			return -EEXIST;
	}

	rb_link_node_rcu(&new->node, parent, ins_point);
	rb_insert_color(&new->node, &list->tree);

	return 0;
}

static struct netdev_hw_addr*
__hw_addr_create(const unsigned char *addr, int addr_len,
		 unsigned char addr_type, bool global, bool sync)
{
	struct netdev_hw_addr *ha;
	int alloc_size;

	alloc_size = sizeof(*ha);
	if (alloc_size < L1_CACHE_BYTES)
		alloc_size = L1_CACHE_BYTES;
	ha = kmalloc(alloc_size, GFP_ATOMIC);
	if (!ha)
		return NULL;
	memcpy(ha->addr, addr, addr_len);
	ha->type = addr_type;
	ha->refcount = 1;
	ha->global_use = global;
	ha->synced = sync ? 1 : 0;
	ha->sync_cnt = 0;

	return ha;
}

static int __hw_addr_add_ex(struct netdev_hw_addr_list *list,
			    const unsigned char *addr, int addr_len,
			    unsigned char addr_type, bool global, bool sync,
			    int sync_count, bool exclusive)
{
	struct rb_node **ins_point = &list->tree.rb_node, *parent = NULL;
	struct netdev_hw_addr *ha;

	if (addr_len > MAX_ADDR_LEN)
		return -EINVAL;

	while (*ins_point) {
		int diff;

		ha = rb_entry(*ins_point, struct netdev_hw_addr, node);
		diff = memcmp(addr, ha->addr, addr_len);
		if (diff == 0)
			diff = memcmp(&addr_type, &ha->type, sizeof(addr_type));

		parent = *ins_point;
		if (diff < 0) {
			ins_point = &parent->rb_left;
		} else if (diff > 0) {
			ins_point = &parent->rb_right;
		} else {
			if (exclusive)
				return -EEXIST;
			if (global) {
				/* check if addr is already used as global */
				if (ha->global_use)
					return 0;
				else
					ha->global_use = true;
			}
			if (sync) {
				if (ha->synced && sync_count)
					return -EEXIST;
				else
					ha->synced++;
			}
			ha->refcount++;
			return 0;
		}
	}

	ha = __hw_addr_create(addr, addr_len, addr_type, global, sync);
	if (!ha)
		return -ENOMEM;

	rb_link_node(&ha->node, parent, ins_point);
	rb_insert_color(&ha->node, &list->tree);

	list_add_tail_rcu(&ha->list, &list->list);
	list->count++;

	return 0;
}

static int __hw_addr_add(struct netdev_hw_addr_list *list,
			 const unsigned char *addr, int addr_len,
			 unsigned char addr_type)
{
	return __hw_addr_add_ex(list, addr, addr_len, addr_type, false, false,
				0, false);
}

static int __hw_addr_del_entry(struct netdev_hw_addr_list *list,
			       struct netdev_hw_addr *ha, bool global,
			       bool sync)
{
	if (global && !ha->global_use)
		return -ENOENT;

	if (sync && !ha->synced)
		return -ENOENT;

	if (global)
		ha->global_use = false;

	if (sync)
		ha->synced--;

	if (--ha->refcount)
		return 0;

	rb_erase(&ha->node, &list->tree);

	list_del_rcu(&ha->list);
	kfree_rcu(ha, rcu_head);
	list->count--;
	return 0;
}

static struct netdev_hw_addr *__hw_addr_lookup(struct netdev_hw_addr_list *list,
					       const unsigned char *addr, int addr_len,
					       unsigned char addr_type)
{
	struct rb_node *node;

	node = list->tree.rb_node;

	while (node) {
		struct netdev_hw_addr *ha = rb_entry(node, struct netdev_hw_addr, node);
		int diff = memcmp(addr, ha->addr, addr_len);

		if (diff == 0 && addr_type)
			diff = memcmp(&addr_type, &ha->type, sizeof(addr_type));

		if (diff < 0)
			node = node->rb_left;
		else if (diff > 0)
			node = node->rb_right;
		else
			return ha;
	}

	return NULL;
}

static int __hw_addr_del_ex(struct netdev_hw_addr_list *list,
			    const unsigned char *addr, int addr_len,
			    unsigned char addr_type, bool global, bool sync)
{
	struct netdev_hw_addr *ha = __hw_addr_lookup(list, addr, addr_len, addr_type);

	if (!ha)
		return -ENOENT;
	return __hw_addr_del_entry(list, ha, global, sync);
}

static int __hw_addr_del(struct netdev_hw_addr_list *list,
			 const unsigned char *addr, int addr_len,
			 unsigned char addr_type)
{
	return __hw_addr_del_ex(list, addr, addr_len, addr_type, false, false);
}

static int __hw_addr_sync_one(struct netdev_hw_addr_list *to_list,
			       struct netdev_hw_addr *ha,
			       int addr_len)
{
	int err;

	err = __hw_addr_add_ex(to_list, ha->addr, addr_len, ha->type,
			       false, true, ha->sync_cnt, false);
	if (err && err != -EEXIST)
		return err;

	if (!err) {
		ha->sync_cnt++;
		ha->refcount++;
	}

	return 0;
}

static void __hw_addr_unsync_one(struct netdev_hw_addr_list *to_list,
				 struct netdev_hw_addr_list *from_list,
				 struct netdev_hw_addr *ha,
				 int addr_len)
{
	int err;

	err = __hw_addr_del_ex(to_list, ha->addr, addr_len, ha->type,
			       false, true);
	if (err)
		return;
	ha->sync_cnt--;
	/* address on from list is not marked synced */
	__hw_addr_del_entry(from_list, ha, false, false);
}

static int __hw_addr_sync_multiple(struct netdev_hw_addr_list *to_list,
				   struct netdev_hw_addr_list *from_list,
				   int addr_len)
{
	int err = 0;
	struct netdev_hw_addr *ha, *tmp;

	list_for_each_entry_safe(ha, tmp, &from_list->list, list) {
		if (ha->sync_cnt == ha->refcount) {
			__hw_addr_unsync_one(to_list, from_list, ha, addr_len);
		} else {
			err = __hw_addr_sync_one(to_list, ha, addr_len);
			if (err)
				break;
		}
	}
	return err;
}

/* This function only works where there is a strict 1-1 relationship
 * between source and destionation of they synch. If you ever need to
 * sync addresses to more then 1 destination, you need to use
 * __hw_addr_sync_multiple().
 */
int __hw_addr_sync(struct netdev_hw_addr_list *to_list,
		   struct netdev_hw_addr_list *from_list,
		   int addr_len)
{
	int err = 0;
	struct netdev_hw_addr *ha, *tmp;

	list_for_each_entry_safe(ha, tmp, &from_list->list, list) {
		if (!ha->sync_cnt) {
			err = __hw_addr_sync_one(to_list, ha, addr_len);
			if (err)
				break;
		} else if (ha->refcount == 1)
			__hw_addr_unsync_one(to_list, from_list, ha, addr_len);
	}
	return err;
}
EXPORT_SYMBOL(__hw_addr_sync);

void __hw_addr_unsync(struct netdev_hw_addr_list *to_list,
		      struct netdev_hw_addr_list *from_list,
		      int addr_len)
{
	struct netdev_hw_addr *ha, *tmp;

	list_for_each_entry_safe(ha, tmp, &from_list->list, list) {
		if (ha->sync_cnt)
			__hw_addr_unsync_one(to_list, from_list, ha, addr_len);
	}
}
EXPORT_SYMBOL(__hw_addr_unsync);

/**
 *  __hw_addr_sync_dev - Synchonize device's multicast list
 *  @list: address list to syncronize
 *  @dev:  device to sync
 *  @sync: function to call if address should be added
 *  @unsync: function to call if address should be removed
 *
 *  This function is intended to be called from the ndo_set_rx_mode
 *  function of devices that require explicit address add/remove
 *  notifications.  The unsync function may be NULL in which case
 *  the addresses requiring removal will simply be removed without
 *  any notification to the device.
 **/
int __hw_addr_sync_dev(struct netdev_hw_addr_list *list,
		       struct net_device *dev,
		       int (*sync)(struct net_device *, const unsigned char *),
		       int (*unsync)(struct net_device *,
				     const unsigned char *))
{
	struct netdev_hw_addr *ha, *tmp;
	int err;

	/* first go through and flush out any stale entries */
	list_for_each_entry_safe(ha, tmp, &list->list, list) {
		if (!ha->sync_cnt || ha->refcount != 1)
			continue;

		/* if unsync is defined and fails defer unsyncing address */
		if (unsync && unsync(dev, ha->addr))
			continue;

		ha->sync_cnt--;
		__hw_addr_del_entry(list, ha, false, false);
	}

	/* go through and sync new entries to the list */
	list_for_each_entry_safe(ha, tmp, &list->list, list) {
		if (ha->sync_cnt)
			continue;

		err = sync(dev, ha->addr);
		if (err)
			return err;

		ha->sync_cnt++;
		ha->refcount++;
	}

	return 0;
}
EXPORT_SYMBOL(__hw_addr_sync_dev);

/**
 *  __hw_addr_ref_sync_dev - Synchronize device's multicast address list taking
 *  into account references
 *  @list: address list to synchronize
 *  @dev:  device to sync
 *  @sync: function to call if address or reference on it should be added
 *  @unsync: function to call if address or some reference on it should removed
 *
 *  This function is intended to be called from the ndo_set_rx_mode
 *  function of devices that require explicit address or references on it
 *  add/remove notifications. The unsync function may be NULL in which case
 *  the addresses or references on it requiring removal will simply be
 *  removed without any notification to the device. That is responsibility of
 *  the driver to identify and distribute address or references on it between
 *  internal address tables.
 **/
int __hw_addr_ref_sync_dev(struct netdev_hw_addr_list *list,
			   struct net_device *dev,
			   int (*sync)(struct net_device *,
				       const unsigned char *, int),
			   int (*unsync)(struct net_device *,
					 const unsigned char *, int))
{
	struct netdev_hw_addr *ha, *tmp;
	int err, ref_cnt;

	/* first go through and flush out any unsynced/stale entries */
	list_for_each_entry_safe(ha, tmp, &list->list, list) {
		/* sync if address is not used */
		if ((ha->sync_cnt << 1) <= ha->refcount)
			continue;

		/* if fails defer unsyncing address */
		ref_cnt = ha->refcount - ha->sync_cnt;
		if (unsync && unsync(dev, ha->addr, ref_cnt))
			continue;

		ha->refcount = (ref_cnt << 1) + 1;
		ha->sync_cnt = ref_cnt;
		__hw_addr_del_entry(list, ha, false, false);
	}

	/* go through and sync updated/new entries to the list */
	list_for_each_entry_safe(ha, tmp, &list->list, list) {
		/* sync if address added or reused */
		if ((ha->sync_cnt << 1) >= ha->refcount)
			continue;

		ref_cnt = ha->refcount - ha->sync_cnt;
		err = sync(dev, ha->addr, ref_cnt);
		if (err)
			return err;

		ha->refcount = ref_cnt << 1;
		ha->sync_cnt = ref_cnt;
	}

	return 0;
}
EXPORT_SYMBOL(__hw_addr_ref_sync_dev);

/**
 *  __hw_addr_ref_unsync_dev - Remove synchronized addresses and references on
 *  it from device
 *  @list: address list to remove synchronized addresses (references on it) from
 *  @dev:  device to sync
 *  @unsync: function to call if address and references on it should be removed
 *
 *  Remove all addresses that were added to the device by
 *  __hw_addr_ref_sync_dev(). This function is intended to be called from the
 *  ndo_stop or ndo_open functions on devices that require explicit address (or
 *  references on it) add/remove notifications. If the unsync function pointer
 *  is NULL then this function can be used to just reset the sync_cnt for the
 *  addresses in the list.
 **/
void __hw_addr_ref_unsync_dev(struct netdev_hw_addr_list *list,
			      struct net_device *dev,
			      int (*unsync)(struct net_device *,
					    const unsigned char *, int))
{
	struct netdev_hw_addr *ha, *tmp;

	list_for_each_entry_safe(ha, tmp, &list->list, list) {
		if (!ha->sync_cnt)
			continue;

		/* if fails defer unsyncing address */
		if (unsync && unsync(dev, ha->addr, ha->sync_cnt))
			continue;

		ha->refcount -= ha->sync_cnt - 1;
		ha->sync_cnt = 0;
		__hw_addr_del_entry(list, ha, false, false);
	}
}
EXPORT_SYMBOL(__hw_addr_ref_unsync_dev);

/**
 *  __hw_addr_unsync_dev - Remove synchronized addresses from device
 *  @list: address list to remove synchronized addresses from
 *  @dev:  device to sync
 *  @unsync: function to call if address should be removed
 *
 *  Remove all addresses that were added to the device by __hw_addr_sync_dev().
 *  This function is intended to be called from the ndo_stop or ndo_open
 *  functions on devices that require explicit address add/remove
 *  notifications.  If the unsync function pointer is NULL then this function
 *  can be used to just reset the sync_cnt for the addresses in the list.
 **/
void __hw_addr_unsync_dev(struct netdev_hw_addr_list *list,
			  struct net_device *dev,
			  int (*unsync)(struct net_device *,
					const unsigned char *))
{
	struct netdev_hw_addr *ha, *tmp;

	list_for_each_entry_safe(ha, tmp, &list->list, list) {
		if (!ha->sync_cnt)
			continue;

		/* if unsync is defined and fails defer unsyncing address */
		if (unsync && unsync(dev, ha->addr))
			continue;

		ha->sync_cnt--;
		__hw_addr_del_entry(list, ha, false, false);
	}
}
EXPORT_SYMBOL(__hw_addr_unsync_dev);

static void __hw_addr_flush(struct netdev_hw_addr_list *list)
{
	struct netdev_hw_addr *ha, *tmp;

	list->tree = RB_ROOT;
	list_for_each_entry_safe(ha, tmp, &list->list, list) {
		list_del_rcu(&ha->list);
		kfree_rcu(ha, rcu_head);
	}
	list->count = 0;
}

void __hw_addr_init(struct netdev_hw_addr_list *list)
{
	INIT_LIST_HEAD(&list->list);
	list->count = 0;
	list->tree = RB_ROOT;
}
EXPORT_SYMBOL(__hw_addr_init);

/*
 * Device addresses handling functions
 */

/* Check that netdev->dev_addr is not written to directly as this would
 * break the rbtree layout. All changes should go thru dev_addr_set() and co.
 * Remove this check in mid-2024.
 */
void dev_addr_check(struct net_device *dev)
{
	if (!memcmp(dev->dev_addr, dev->dev_addr_shadow, MAX_ADDR_LEN))
		return;

	netdev_warn(dev, "Current addr:  %*ph\n", MAX_ADDR_LEN, dev->dev_addr);
	netdev_warn(dev, "Expected addr: %*ph\n",
		    MAX_ADDR_LEN, dev->dev_addr_shadow);
	netdev_WARN(dev, "Incorrect netdev->dev_addr\n");
}

/**
 *	dev_addr_flush - Flush device address list
 *	@dev: device
 *
 *	Flush device address list and reset ->dev_addr.
 *
 *	The caller must hold the rtnl_mutex.
 */
void dev_addr_flush(struct net_device *dev)
{
	/* rtnl_mutex must be held here */
	dev_addr_check(dev);

	__hw_addr_flush(&dev->dev_addrs);
	dev->dev_addr = NULL;
}

/**
 *	dev_addr_init - Init device address list
 *	@dev: device
 *
 *	Init device address list and create the first element,
 *	used by ->dev_addr.
 *
 *	The caller must hold the rtnl_mutex.
 */
int dev_addr_init(struct net_device *dev)
{
	unsigned char addr[MAX_ADDR_LEN];
	struct netdev_hw_addr *ha;
	int err;

	/* rtnl_mutex must be held here */

	__hw_addr_init(&dev->dev_addrs);
	memset(addr, 0, sizeof(addr));
	err = __hw_addr_add(&dev->dev_addrs, addr, sizeof(addr),
			    NETDEV_HW_ADDR_T_LAN);
	if (!err) {
		/*
		 * Get the first (previously created) address from the list
		 * and set dev_addr pointer to this location.
		 */
		ha = list_first_entry(&dev->dev_addrs.list,
				      struct netdev_hw_addr, list);
		dev->dev_addr = ha->addr;
	}
	return err;
}

void dev_addr_mod(struct net_device *dev, unsigned int offset,
		  const void *addr, size_t len)
{
	struct netdev_hw_addr *ha;

	dev_addr_check(dev);

	ha = container_of(dev->dev_addr, struct netdev_hw_addr, addr[0]);
	rb_erase(&ha->node, &dev->dev_addrs.tree);
	memcpy(&ha->addr[offset], addr, len);
	memcpy(&dev->dev_addr_shadow[offset], addr, len);
	WARN_ON(__hw_addr_insert(&dev->dev_addrs, ha, dev->addr_len));
}
EXPORT_SYMBOL(dev_addr_mod);

/**
 *	dev_addr_add - Add a device address
 *	@dev: device
 *	@addr: address to add
 *	@addr_type: address type
 *
 *	Add a device address to the device or increase the reference count if
 *	it already exists.
 *
 *	The caller must hold the rtnl_mutex.
 */
int dev_addr_add(struct net_device *dev, const unsigned char *addr,
		 unsigned char addr_type)
{
	int err;

	ASSERT_RTNL();

	err = dev_pre_changeaddr_notify(dev, addr, NULL);
	if (err)
		return err;
	err = __hw_addr_add(&dev->dev_addrs, addr, dev->addr_len, addr_type);
	if (!err)
		call_netdevice_notifiers(NETDEV_CHANGEADDR, dev);
	return err;
}
EXPORT_SYMBOL(dev_addr_add);

/**
 *	dev_addr_del - Release a device address.
 *	@dev: device
 *	@addr: address to delete
 *	@addr_type: address type
 *
 *	Release reference to a device address and remove it from the device
 *	if the reference count drops to zero.
 *
 *	The caller must hold the rtnl_mutex.
 */
int dev_addr_del(struct net_device *dev, const unsigned char *addr,
		 unsigned char addr_type)
{
	int err;
	struct netdev_hw_addr *ha;

	ASSERT_RTNL();

	/*
	 * We can not remove the first address from the list because
	 * dev->dev_addr points to that.
	 */
	ha = list_first_entry(&dev->dev_addrs.list,
			      struct netdev_hw_addr, list);
	if (!memcmp(ha->addr, addr, dev->addr_len) &&
	    ha->type == addr_type && ha->refcount == 1)
		return -ENOENT;

	err = __hw_addr_del(&dev->dev_addrs, addr, dev->addr_len,
			    addr_type);
	if (!err)
		call_netdevice_notifiers(NETDEV_CHANGEADDR, dev);
	return err;
}
EXPORT_SYMBOL(dev_addr_del);

/*
 * Unicast list handling functions
 */

/**
 *	dev_uc_add_excl - Add a global secondary unicast address
 *	@dev: device
 *	@addr: address to add
 */
int dev_uc_add_excl(struct net_device *dev, const unsigned char *addr)
{
	int err;

	netif_addr_lock_bh(dev);
	err = __hw_addr_add_ex(&dev->uc, addr, dev->addr_len,
			       NETDEV_HW_ADDR_T_UNICAST, true, false,
			       0, true);
	if (!err)
		__dev_set_rx_mode(dev);
	netif_addr_unlock_bh(dev);
	return err;
}
EXPORT_SYMBOL(dev_uc_add_excl);

/**
 *	dev_uc_add - Add a secondary unicast address
 *	@dev: device
 *	@addr: address to add
 *
 *	Add a secondary unicast address to the device or increase
 *	the reference count if it already exists.
 */
int dev_uc_add(struct net_device *dev, const unsigned char *addr)
{
	int err;

	netif_addr_lock_bh(dev);
	err = __hw_addr_add(&dev->uc, addr, dev->addr_len,
			    NETDEV_HW_ADDR_T_UNICAST);
	if (!err)
		__dev_set_rx_mode(dev);
	netif_addr_unlock_bh(dev);
	return err;
}
EXPORT_SYMBOL(dev_uc_add);

/**
 *	dev_uc_del - Release secondary unicast address.
 *	@dev: device
 *	@addr: address to delete
 *
 *	Release reference to a secondary unicast address and remove it
 *	from the device if the reference count drops to zero.
 */
int dev_uc_del(struct net_device *dev, const unsigned char *addr)
{
	int err;

	netif_addr_lock_bh(dev);
	err = __hw_addr_del(&dev->uc, addr, dev->addr_len,
			    NETDEV_HW_ADDR_T_UNICAST);
	if (!err)
		__dev_set_rx_mode(dev);
	netif_addr_unlock_bh(dev);
	return err;
}
EXPORT_SYMBOL(dev_uc_del);

/**
 *	dev_uc_sync - Synchronize device's unicast list to another device
 *	@to: destination device
 *	@from: source device
 *
 *	Add newly added addresses to the destination device and release
 *	addresses that have no users left. The source device must be
 *	locked by netif_addr_lock_bh.
 *
 *	This function is intended to be called from the dev->set_rx_mode
 *	function of layered software devices.  This function assumes that
 *	addresses will only ever be synced to the @to devices and no other.
 */
int dev_uc_sync(struct net_device *to, struct net_device *from)
{
	int err = 0;

	if (to->addr_len != from->addr_len)
		return -EINVAL;

	netif_addr_lock(to);
	err = __hw_addr_sync(&to->uc, &from->uc, to->addr_len);
	if (!err)
		__dev_set_rx_mode(to);
	netif_addr_unlock(to);
	return err;
}
EXPORT_SYMBOL(dev_uc_sync);

/**
 *	dev_uc_sync_multiple - Synchronize device's unicast list to another
 *	device, but allow for multiple calls to sync to multiple devices.
 *	@to: destination device
 *	@from: source device
 *
 *	Add newly added addresses to the destination device and release
 *	addresses that have been deleted from the source. The source device
 *	must be locked by netif_addr_lock_bh.
 *
 *	This function is intended to be called from the dev->set_rx_mode
 *	function of layered software devices.  It allows for a single source
 *	device to be synced to multiple destination devices.
 */
int dev_uc_sync_multiple(struct net_device *to, struct net_device *from)
{
	int err = 0;

	if (to->addr_len != from->addr_len)
		return -EINVAL;

	netif_addr_lock(to);
	err = __hw_addr_sync_multiple(&to->uc, &from->uc, to->addr_len);
	if (!err)
		__dev_set_rx_mode(to);
	netif_addr_unlock(to);
	return err;
}
EXPORT_SYMBOL(dev_uc_sync_multiple);

/**
 *	dev_uc_unsync - Remove synchronized addresses from the destination device
 *	@to: destination device
 *	@from: source device
 *
 *	Remove all addresses that were added to the destination device by
 *	dev_uc_sync(). This function is intended to be called from the
 *	dev->stop function of layered software devices.
 */
void dev_uc_unsync(struct net_device *to, struct net_device *from)
{
	if (to->addr_len != from->addr_len)
		return;

	/* netif_addr_lock_bh() uses lockdep subclass 0, this is okay for two
	 * reasons:
	 * 1) This is always called without any addr_list_lock, so as the
	 *    outermost one here, it must be 0.
	 * 2) This is called by some callers after unlinking the upper device,
	 *    so the dev->lower_level becomes 1 again.
	 * Therefore, the subclass for 'from' is 0, for 'to' is either 1 or
	 * larger.
	 */
	netif_addr_lock_bh(from);
	netif_addr_lock(to);
	__hw_addr_unsync(&to->uc, &from->uc, to->addr_len);
	__dev_set_rx_mode(to);
	netif_addr_unlock(to);
	netif_addr_unlock_bh(from);
}
EXPORT_SYMBOL(dev_uc_unsync);

/**
 *	dev_uc_flush - Flush unicast addresses
 *	@dev: device
 *
 *	Flush unicast addresses.
 */
void dev_uc_flush(struct net_device *dev)
{
	netif_addr_lock_bh(dev);
	__hw_addr_flush(&dev->uc);
	netif_addr_unlock_bh(dev);
}
EXPORT_SYMBOL(dev_uc_flush);

/**
 *	dev_uc_init - Init unicast address list
 *	@dev: device
 *
 *	Init unicast address list.
 */
void dev_uc_init(struct net_device *dev)
{
	__hw_addr_init(&dev->uc);
}
EXPORT_SYMBOL(dev_uc_init);

/*
 * Multicast list handling functions
 */

/**
 *	dev_mc_add_excl - Add a global secondary multicast address
 *	@dev: device
 *	@addr: address to add
 */
int dev_mc_add_excl(struct net_device *dev, const unsigned char *addr)
{
	int err;

	netif_addr_lock_bh(dev);
	err = __hw_addr_add_ex(&dev->mc, addr, dev->addr_len,
			       NETDEV_HW_ADDR_T_MULTICAST, true, false,
			       0, true);
	if (!err)
		__dev_set_rx_mode(dev);
	netif_addr_unlock_bh(dev);
	return err;
}
EXPORT_SYMBOL(dev_mc_add_excl);

static int __dev_mc_add(struct net_device *dev, const unsigned char *addr,
			bool global)
{
	int err;

	netif_addr_lock_bh(dev);
	err = __hw_addr_add_ex(&dev->mc, addr, dev->addr_len,
			       NETDEV_HW_ADDR_T_MULTICAST, global, false,
			       0, false);
	if (!err)
		__dev_set_rx_mode(dev);
	netif_addr_unlock_bh(dev);
	return err;
}
/**
 *	dev_mc_add - Add a multicast address
 *	@dev: device
 *	@addr: address to add
 *
 *	Add a multicast address to the device or increase
 *	the reference count if it already exists.
 */
int dev_mc_add(struct net_device *dev, const unsigned char *addr)
{
	return __dev_mc_add(dev, addr, false);
}
EXPORT_SYMBOL(dev_mc_add);

/**
 *	dev_mc_add_global - Add a global multicast address
 *	@dev: device
 *	@addr: address to add
 *
 *	Add a global multicast address to the device.
 */
int dev_mc_add_global(struct net_device *dev, const unsigned char *addr)
{
	return __dev_mc_add(dev, addr, true);
}
EXPORT_SYMBOL(dev_mc_add_global);

static int __dev_mc_del(struct net_device *dev, const unsigned char *addr,
			bool global)
{
	int err;

	netif_addr_lock_bh(dev);
	err = __hw_addr_del_ex(&dev->mc, addr, dev->addr_len,
			       NETDEV_HW_ADDR_T_MULTICAST, global, false);
	if (!err)
		__dev_set_rx_mode(dev);
	netif_addr_unlock_bh(dev);
	return err;
}

/**
 *	dev_mc_del - Delete a multicast address.
 *	@dev: device
 *	@addr: address to delete
 *
 *	Release reference to a multicast address and remove it
 *	from the device if the reference count drops to zero.
 */
int dev_mc_del(struct net_device *dev, const unsigned char *addr)
{
	return __dev_mc_del(dev, addr, false);
}
EXPORT_SYMBOL(dev_mc_del);

/**
 *	dev_mc_del_global - Delete a global multicast address.
 *	@dev: device
 *	@addr: address to delete
 *
 *	Release reference to a multicast address and remove it
 *	from the device if the reference count drops to zero.
 */
int dev_mc_del_global(struct net_device *dev, const unsigned char *addr)
{
	return __dev_mc_del(dev, addr, true);
}
EXPORT_SYMBOL(dev_mc_del_global);

/**
 *	dev_mc_sync - Synchronize device's multicast list to another device
 *	@to: destination device
 *	@from: source device
 *
 *	Add newly added addresses to the destination device and release
 *	addresses that have no users left. The source device must be
 *	locked by netif_addr_lock_bh.
 *
 *	This function is intended to be called from the ndo_set_rx_mode
 *	function of layered software devices.
 */
int dev_mc_sync(struct net_device *to, struct net_device *from)
{
	int err = 0;

	if (to->addr_len != from->addr_len)
		return -EINVAL;

	netif_addr_lock(to);
	err = __hw_addr_sync(&to->mc, &from->mc, to->addr_len);
	if (!err)
		__dev_set_rx_mode(to);
	netif_addr_unlock(to);
	return err;
}
EXPORT_SYMBOL(dev_mc_sync);

/**
 *	dev_mc_sync_multiple - Synchronize device's multicast list to another
 *	device, but allow for multiple calls to sync to multiple devices.
 *	@to: destination device
 *	@from: source device
 *
 *	Add newly added addresses to the destination device and release
 *	addresses that have no users left. The source device must be
 *	locked by netif_addr_lock_bh.
 *
 *	This function is intended to be called from the ndo_set_rx_mode
 *	function of layered software devices.  It allows for a single
 *	source device to be synced to multiple destination devices.
 */
int dev_mc_sync_multiple(struct net_device *to, struct net_device *from)
{
	int err = 0;

	if (to->addr_len != from->addr_len)
		return -EINVAL;

	netif_addr_lock(to);
	err = __hw_addr_sync_multiple(&to->mc, &from->mc, to->addr_len);
	if (!err)
		__dev_set_rx_mode(to);
	netif_addr_unlock(to);
	return err;
}
EXPORT_SYMBOL(dev_mc_sync_multiple);

/**
 *	dev_mc_unsync - Remove synchronized addresses from the destination device
 *	@to: destination device
 *	@from: source device
 *
 *	Remove all addresses that were added to the destination device by
 *	dev_mc_sync(). This function is intended to be called from the
 *	dev->stop function of layered software devices.
 */
void dev_mc_unsync(struct net_device *to, struct net_device *from)
{
	if (to->addr_len != from->addr_len)
		return;

	/* See the above comments inside dev_uc_unsync(). */
	netif_addr_lock_bh(from);
	netif_addr_lock(to);
	__hw_addr_unsync(&to->mc, &from->mc, to->addr_len);
	__dev_set_rx_mode(to);
	netif_addr_unlock(to);
	netif_addr_unlock_bh(from);
}
EXPORT_SYMBOL(dev_mc_unsync);

/**
 *	dev_mc_flush - Flush multicast addresses
 *	@dev: device
 *
 *	Flush multicast addresses.
 */
void dev_mc_flush(struct net_device *dev)
{
	netif_addr_lock_bh(dev);
	__hw_addr_flush(&dev->mc);
	netif_addr_unlock_bh(dev);
}
EXPORT_SYMBOL(dev_mc_flush);

/**
 *	dev_mc_init - Init multicast address list
 *	@dev: device
 *
 *	Init multicast address list.
 */
void dev_mc_init(struct net_device *dev)
{
	__hw_addr_init(&dev->mc);
}
EXPORT_SYMBOL(dev_mc_init);
