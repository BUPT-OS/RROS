// SPDX-License-Identifier: GPL-2.0 OR MIT
/**************************************************************************
 *
 * Copyright 2014-2022 VMware, Inc., Palo Alto, CA., USA
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "vmwgfx_drv.h"
#include "vmwgfx_resource_priv.h"

#include <linux/hashtable.h>

#define VMW_CMDBUF_RES_MAN_HT_ORDER 12

/**
 * struct vmw_cmdbuf_res - Command buffer managed resource entry.
 *
 * @res: Refcounted pointer to a struct vmw_resource.
 * @hash: Hash entry for the manager hash table.
 * @head: List head used either by the staging list or the manager list
 * of committed resources.
 * @state: Staging state of this resource entry.
 * @man: Pointer to a resource manager for this entry.
 */
struct vmw_cmdbuf_res {
	struct vmw_resource *res;
	struct vmwgfx_hash_item hash;
	struct list_head head;
	enum vmw_cmdbuf_res_state state;
	struct vmw_cmdbuf_res_manager *man;
};

/**
 * struct vmw_cmdbuf_res_manager - Command buffer resource manager.
 *
 * @resources: Hash table containing staged and committed command buffer
 * resources
 * @list: List of committed command buffer resources.
 * @dev_priv: Pointer to a device private structure.
 *
 * @resources and @list are protected by the cmdbuf mutex for now.
 */
struct vmw_cmdbuf_res_manager {
	DECLARE_HASHTABLE(resources, VMW_CMDBUF_RES_MAN_HT_ORDER);
	struct list_head list;
	struct vmw_private *dev_priv;
};


/**
 * vmw_cmdbuf_res_lookup - Look up a command buffer resource
 *
 * @man: Pointer to the command buffer resource manager
 * @res_type: The resource type, that combined with the user key
 * identifies the resource.
 * @user_key: The user key.
 *
 * Returns a valid refcounted struct vmw_resource pointer on success,
 * an error pointer on failure.
 */
struct vmw_resource *
vmw_cmdbuf_res_lookup(struct vmw_cmdbuf_res_manager *man,
		      enum vmw_cmdbuf_res_type res_type,
		      u32 user_key)
{
	struct vmwgfx_hash_item *hash;
	unsigned long key = user_key | (res_type << 24);

	hash_for_each_possible_rcu(man->resources, hash, head, key) {
		if (hash->key == key)
			return hlist_entry(hash, struct vmw_cmdbuf_res, hash)->res;
	}
	return ERR_PTR(-EINVAL);
}

/**
 * vmw_cmdbuf_res_free - Free a command buffer resource.
 *
 * @man: Pointer to the command buffer resource manager
 * @entry: Pointer to a struct vmw_cmdbuf_res.
 *
 * Frees a struct vmw_cmdbuf_res entry and drops its reference to the
 * struct vmw_resource.
 */
static void vmw_cmdbuf_res_free(struct vmw_cmdbuf_res_manager *man,
				struct vmw_cmdbuf_res *entry)
{
	list_del(&entry->head);
	hash_del_rcu(&entry->hash.head);
	vmw_resource_unreference(&entry->res);
	kfree(entry);
}

/**
 * vmw_cmdbuf_res_commit - Commit a list of command buffer resource actions
 *
 * @list: Caller's list of command buffer resource actions.
 *
 * This function commits a list of command buffer resource
 * additions or removals.
 * It is typically called when the execbuf ioctl call triggering these
 * actions has committed the fifo contents to the device.
 */
void vmw_cmdbuf_res_commit(struct list_head *list)
{
	struct vmw_cmdbuf_res *entry, *next;

	list_for_each_entry_safe(entry, next, list, head) {
		list_del(&entry->head);
		if (entry->res->func->commit_notify)
			entry->res->func->commit_notify(entry->res,
							entry->state);
		switch (entry->state) {
		case VMW_CMDBUF_RES_ADD:
			entry->state = VMW_CMDBUF_RES_COMMITTED;
			list_add_tail(&entry->head, &entry->man->list);
			break;
		case VMW_CMDBUF_RES_DEL:
			vmw_resource_unreference(&entry->res);
			kfree(entry);
			break;
		default:
			BUG();
			break;
		}
	}
}

/**
 * vmw_cmdbuf_res_revert - Revert a list of command buffer resource actions
 *
 * @list: Caller's list of command buffer resource action
 *
 * This function reverts a list of command buffer resource
 * additions or removals.
 * It is typically called when the execbuf ioctl call triggering these
 * actions failed for some reason, and the command stream was never
 * submitted.
 */
void vmw_cmdbuf_res_revert(struct list_head *list)
{
	struct vmw_cmdbuf_res *entry, *next;

	list_for_each_entry_safe(entry, next, list, head) {
		switch (entry->state) {
		case VMW_CMDBUF_RES_ADD:
			vmw_cmdbuf_res_free(entry->man, entry);
			break;
		case VMW_CMDBUF_RES_DEL:
			hash_add_rcu(entry->man->resources, &entry->hash.head,
						entry->hash.key);
			list_move_tail(&entry->head, &entry->man->list);
			entry->state = VMW_CMDBUF_RES_COMMITTED;
			break;
		default:
			BUG();
			break;
		}
	}
}

/**
 * vmw_cmdbuf_res_add - Stage a command buffer managed resource for addition.
 *
 * @man: Pointer to the command buffer resource manager.
 * @res_type: The resource type.
 * @user_key: The user-space id of the resource.
 * @res: Valid (refcount != 0) pointer to a struct vmw_resource.
 * @list: The staging list.
 *
 * This function allocates a struct vmw_cmdbuf_res entry and adds the
 * resource to the hash table of the manager identified by @man. The
 * entry is then put on the staging list identified by @list.
 */
int vmw_cmdbuf_res_add(struct vmw_cmdbuf_res_manager *man,
		       enum vmw_cmdbuf_res_type res_type,
		       u32 user_key,
		       struct vmw_resource *res,
		       struct list_head *list)
{
	struct vmw_cmdbuf_res *cres;

	cres = kzalloc(sizeof(*cres), GFP_KERNEL);
	if (unlikely(!cres))
		return -ENOMEM;

	cres->hash.key = user_key | (res_type << 24);
	hash_add_rcu(man->resources, &cres->hash.head, cres->hash.key);

	cres->state = VMW_CMDBUF_RES_ADD;
	cres->res = vmw_resource_reference(res);
	cres->man = man;
	list_add_tail(&cres->head, list);

	return 0;
}

/**
 * vmw_cmdbuf_res_remove - Stage a command buffer managed resource for removal.
 *
 * @man: Pointer to the command buffer resource manager.
 * @res_type: The resource type.
 * @user_key: The user-space id of the resource.
 * @list: The staging list.
 * @res_p: If the resource is in an already committed state, points to the
 * struct vmw_resource on successful return. The pointer will be
 * non ref-counted.
 *
 * This function looks up the struct vmw_cmdbuf_res entry from the manager
 * hash table and, if it exists, removes it. Depending on its current staging
 * state it then either removes the entry from the staging list or adds it
 * to it with a staging state of removal.
 */
int vmw_cmdbuf_res_remove(struct vmw_cmdbuf_res_manager *man,
			  enum vmw_cmdbuf_res_type res_type,
			  u32 user_key,
			  struct list_head *list,
			  struct vmw_resource **res_p)
{
	struct vmw_cmdbuf_res *entry = NULL;
	struct vmwgfx_hash_item *hash;
	unsigned long key = user_key | (res_type << 24);

	hash_for_each_possible_rcu(man->resources, hash, head, key) {
		if (hash->key == key) {
			entry = hlist_entry(hash, struct vmw_cmdbuf_res, hash);
			break;
		}
	}
	if (unlikely(!entry))
		return -EINVAL;

	switch (entry->state) {
	case VMW_CMDBUF_RES_ADD:
		vmw_cmdbuf_res_free(man, entry);
		*res_p = NULL;
		break;
	case VMW_CMDBUF_RES_COMMITTED:
		hash_del_rcu(&entry->hash.head);
		list_del(&entry->head);
		entry->state = VMW_CMDBUF_RES_DEL;
		list_add_tail(&entry->head, list);
		*res_p = entry->res;
		break;
	default:
		BUG();
		break;
	}

	return 0;
}

/**
 * vmw_cmdbuf_res_man_create - Allocate a command buffer managed resource
 * manager.
 *
 * @dev_priv: Pointer to a struct vmw_private
 *
 * Allocates and initializes a command buffer managed resource manager. Returns
 * an error pointer on failure.
 */
struct vmw_cmdbuf_res_manager *
vmw_cmdbuf_res_man_create(struct vmw_private *dev_priv)
{
	struct vmw_cmdbuf_res_manager *man;

	man = kzalloc(sizeof(*man), GFP_KERNEL);
	if (!man)
		return ERR_PTR(-ENOMEM);

	man->dev_priv = dev_priv;
	INIT_LIST_HEAD(&man->list);
	hash_init(man->resources);
	return man;
}

/**
 * vmw_cmdbuf_res_man_destroy - Destroy a command buffer managed resource
 * manager.
 *
 * @man: Pointer to the  manager to destroy.
 *
 * This function destroys a command buffer managed resource manager and
 * unreferences / frees all command buffer managed resources and -entries
 * associated with it.
 */
void vmw_cmdbuf_res_man_destroy(struct vmw_cmdbuf_res_manager *man)
{
	struct vmw_cmdbuf_res *entry, *next;

	list_for_each_entry_safe(entry, next, &man->list, head)
		vmw_cmdbuf_res_free(man, entry);

	kfree(man);
}

