/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 *  Definitions for the SMC module (socket related)
 *
 *  Copyright IBM Corp. 2016
 *
 *  Author(s):  Ursula Braun <ubraun@linux.vnet.ibm.com>
 */
#ifndef _SMC_H
#define _SMC_H

#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>
#include "linux/ism.h"

struct sock;

#define SMC_MAX_PNETID_LEN	16	/* Max. length of PNET id */

struct smc_hashinfo {
	rwlock_t lock;
	struct hlist_head ht;
};

int smc_hash_sk(struct sock *sk);
void smc_unhash_sk(struct sock *sk);

/* SMCD/ISM device driver interface */
struct smcd_dmb {
	u64 dmb_tok;
	u64 rgid;
	u32 dmb_len;
	u32 sba_idx;
	u32 vlan_valid;
	u32 vlan_id;
	void *cpu_addr;
	dma_addr_t dma_addr;
};

#define ISM_EVENT_DMB	0
#define ISM_EVENT_GID	1
#define ISM_EVENT_SWR	2

#define ISM_RESERVED_VLANID	0x1FFF

#define ISM_ERROR	0xFFFF

struct smcd_dev;
struct ism_client;

struct smcd_ops {
	int (*query_remote_gid)(struct smcd_dev *dev, u64 rgid, u32 vid_valid,
				u32 vid);
	int (*register_dmb)(struct smcd_dev *dev, struct smcd_dmb *dmb,
			    struct ism_client *client);
	int (*unregister_dmb)(struct smcd_dev *dev, struct smcd_dmb *dmb);
	int (*add_vlan_id)(struct smcd_dev *dev, u64 vlan_id);
	int (*del_vlan_id)(struct smcd_dev *dev, u64 vlan_id);
	int (*set_vlan_required)(struct smcd_dev *dev);
	int (*reset_vlan_required)(struct smcd_dev *dev);
	int (*signal_event)(struct smcd_dev *dev, u64 rgid, u32 trigger_irq,
			    u32 event_code, u64 info);
	int (*move_data)(struct smcd_dev *dev, u64 dmb_tok, unsigned int idx,
			 bool sf, unsigned int offset, void *data,
			 unsigned int size);
	int (*supports_v2)(void);
	u8* (*get_system_eid)(void);
	u64 (*get_local_gid)(struct smcd_dev *dev);
	u16 (*get_chid)(struct smcd_dev *dev);
	struct device* (*get_dev)(struct smcd_dev *dev);
};

struct smcd_dev {
	const struct smcd_ops *ops;
	void *priv;
	struct list_head list;
	spinlock_t lock;
	struct smc_connection **conn;
	struct list_head vlan;
	struct workqueue_struct *event_wq;
	u8 pnetid[SMC_MAX_PNETID_LEN];
	bool pnetid_by_user;
	struct list_head lgr_list;
	spinlock_t lgr_lock;
	atomic_t lgr_cnt;
	wait_queue_head_t lgrs_deleted;
	u8 going_away : 1;
};

#endif	/* _SMC_H */
