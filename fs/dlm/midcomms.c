// SPDX-License-Identifier: GPL-2.0-only
/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
**  Copyright (C) 2004-2021 Red Hat, Inc.  All rights reserved.
**
**
*******************************************************************************
******************************************************************************/

/*
 * midcomms.c
 *
 * This is the appallingly named "mid-level" comms layer. It takes care about
 * deliver an on application layer "reliable" communication above the used
 * lowcomms transport layer.
 *
 * How it works:
 *
 * Each nodes keeps track of all send DLM messages in send_queue with a sequence
 * number. The receive will send an DLM_ACK message back for every DLM message
 * received at the other side. If a reconnect happens in lowcomms we will send
 * all unacknowledged dlm messages again. The receiving side might drop any already
 * received message by comparing sequence numbers.
 *
 * How version detection works:
 *
 * Due the fact that dlm has pre-configured node addresses on every side
 * it is in it's nature that every side connects at starts to transmit
 * dlm messages which ends in a race. However DLM_RCOM_NAMES, DLM_RCOM_STATUS
 * and their replies are the first messages which are exchanges. Due backwards
 * compatibility these messages are not covered by the midcomms re-transmission
 * layer. These messages have their own re-transmission handling in the dlm
 * application layer. The version field of every node will be set on these RCOM
 * messages as soon as they arrived and the node isn't yet part of the nodes
 * hash. There exists also logic to detect version mismatched if something weird
 * going on or the first messages isn't an expected one.
 *
 * Termination:
 *
 * The midcomms layer does a 4 way handshake for termination on DLM protocol
 * like TCP supports it with half-closed socket support. SCTP doesn't support
 * half-closed socket, so we do it on DLM layer. Also socket shutdown() can be
 * interrupted by .e.g. tcp reset itself. Additional there exists the othercon
 * paradigm in lowcomms which cannot be easily without breaking backwards
 * compatibility. A node cannot send anything to another node when a DLM_FIN
 * message was send. There exists additional logic to print a warning if
 * DLM wants to do it. There exists a state handling like RFC 793 but reduced
 * to termination only. The event "member removal event" describes the cluster
 * manager removed the node from internal lists, at this point DLM does not
 * send any message to the other node. There exists two cases:
 *
 * 1. The cluster member was removed and we received a FIN
 * OR
 * 2. We received a FIN but the member was not removed yet
 *
 * One of these cases will do the CLOSE_WAIT to LAST_ACK change.
 *
 *
 *                              +---------+
 *                              | CLOSED  |
 *                              +---------+
 *                                   | add member/receive RCOM version
 *                                   |            detection msg
 *                                   V
 *                              +---------+
 *                              |  ESTAB  |
 *                              +---------+
 *                       CLOSE    |     |    rcv FIN
 *                      -------   |     |    -------
 * +---------+          snd FIN  /       \   snd ACK          +---------+
 * |  FIN    |<-----------------           ------------------>|  CLOSE  |
 * | WAIT-1  |------------------                              |   WAIT  |
 * +---------+          rcv FIN  \                            +---------+
 * | rcv ACK of FIN   -------   |                            CLOSE  | member
 * | --------------   snd ACK   |                           ------- | removal
 * V        x                   V                           snd FIN V event
 * +---------+                  +---------+                   +---------+
 * |FINWAIT-2|                  | CLOSING |                   | LAST-ACK|
 * +---------+                  +---------+                   +---------+
 * |                rcv ACK of FIN |                 rcv ACK of FIN |
 * |  rcv FIN       -------------- |                 -------------- |
 * |  -------              x       V                        x       V
 *  \ snd ACK                 +---------+                   +---------+
 *   ------------------------>| CLOSED  |                   | CLOSED  |
 *                            +---------+                   +---------+
 *
 * NOTE: any state can interrupted by midcomms_close() and state will be
 * switched to CLOSED in case of fencing. There exists also some timeout
 * handling when we receive the version detection RCOM messages which is
 * made by observation.
 *
 * Future improvements:
 *
 * There exists some known issues/improvements of the dlm handling. Some
 * of them should be done in a next major dlm version bump which makes
 * it incompatible with previous versions.
 *
 * Unaligned memory access:
 *
 * There exists cases when the dlm message buffer length is not aligned
 * to 8 byte. However seems nobody detected any problem with it. This
 * can be fixed in the next major version bump of dlm.
 *
 * Version detection:
 *
 * The version detection and how it's done is related to backwards
 * compatibility. There exists better ways to make a better handling.
 * However this should be changed in the next major version bump of dlm.
 *
 * Tail Size checking:
 *
 * There exists a message tail payload in e.g. DLM_MSG however we don't
 * check it against the message length yet regarding to the receive buffer
 * length. That need to be validated.
 *
 * Fencing bad nodes:
 *
 * At timeout places or weird sequence number behaviours we should send
 * a fencing request to the cluster manager.
 */

/* Debug switch to enable a 5 seconds sleep waiting of a termination.
 * This can be useful to test fencing while termination is running.
 * This requires a setup with only gfs2 as dlm user, so that the
 * last umount will terminate the connection.
 *
 * However it became useful to test, while the 5 seconds block in umount
 * just press the reset button. In a lot of dropping the termination
 * process can could take several seconds.
 */
#define DLM_DEBUG_FENCE_TERMINATION	0

#include <trace/events/dlm.h>
#include <net/tcp.h>

#include "dlm_internal.h"
#include "lowcomms.h"
#include "config.h"
#include "memory.h"
#include "lock.h"
#include "util.h"
#include "midcomms.h"

/* init value for sequence numbers for testing purpose only e.g. overflows */
#define DLM_SEQ_INIT		0
/* 5 seconds wait to sync ending of dlm */
#define DLM_SHUTDOWN_TIMEOUT	msecs_to_jiffies(5000)
#define DLM_VERSION_NOT_SET	0
#define DLM_SEND_ACK_BACK_MSG_THRESHOLD 32
#define DLM_RECV_ACK_BACK_MSG_THRESHOLD (DLM_SEND_ACK_BACK_MSG_THRESHOLD * 8)

struct midcomms_node {
	int nodeid;
	uint32_t version;
	atomic_t seq_send;
	atomic_t seq_next;
	/* These queues are unbound because we cannot drop any message in dlm.
	 * We could send a fence signal for a specific node to the cluster
	 * manager if queues hits some maximum value, however this handling
	 * not supported yet.
	 */
	struct list_head send_queue;
	spinlock_t send_queue_lock;
	atomic_t send_queue_cnt;
#define DLM_NODE_FLAG_CLOSE	1
#define DLM_NODE_FLAG_STOP_TX	2
#define DLM_NODE_FLAG_STOP_RX	3
	atomic_t ulp_delivered;
	unsigned long flags;
	wait_queue_head_t shutdown_wait;

	/* dlm tcp termination state */
#define DLM_CLOSED	1
#define DLM_ESTABLISHED	2
#define DLM_FIN_WAIT1	3
#define DLM_FIN_WAIT2	4
#define DLM_CLOSE_WAIT	5
#define DLM_LAST_ACK	6
#define DLM_CLOSING	7
	int state;
	spinlock_t state_lock;

	/* counts how many lockspaces are using this node
	 * this refcount is necessary to determine if the
	 * node wants to disconnect.
	 */
	int users;

	/* not protected by srcu, node_hash lifetime */
	void *debugfs;

	struct hlist_node hlist;
	struct rcu_head rcu;
};

struct dlm_mhandle {
	const union dlm_packet *inner_p;
	struct midcomms_node *node;
	struct dlm_opts *opts;
	struct dlm_msg *msg;
	bool committed;
	uint32_t seq;

	void (*ack_rcv)(struct midcomms_node *node);

	/* get_mhandle/commit srcu idx exchange */
	int idx;

	struct list_head list;
	struct rcu_head rcu;
};

static struct hlist_head node_hash[CONN_HASH_SIZE];
static DEFINE_SPINLOCK(nodes_lock);
DEFINE_STATIC_SRCU(nodes_srcu);

/* This mutex prevents that midcomms_close() is running while
 * stop() or remove(). As I experienced invalid memory access
 * behaviours when DLM_DEBUG_FENCE_TERMINATION is enabled and
 * resetting machines. I will end in some double deletion in nodes
 * datastructure.
 */
static DEFINE_MUTEX(close_lock);

struct kmem_cache *dlm_midcomms_cache_create(void)
{
	return kmem_cache_create("dlm_mhandle", sizeof(struct dlm_mhandle),
				 0, 0, NULL);
}

static inline const char *dlm_state_str(int state)
{
	switch (state) {
	case DLM_CLOSED:
		return "CLOSED";
	case DLM_ESTABLISHED:
		return "ESTABLISHED";
	case DLM_FIN_WAIT1:
		return "FIN_WAIT1";
	case DLM_FIN_WAIT2:
		return "FIN_WAIT2";
	case DLM_CLOSE_WAIT:
		return "CLOSE_WAIT";
	case DLM_LAST_ACK:
		return "LAST_ACK";
	case DLM_CLOSING:
		return "CLOSING";
	default:
		return "UNKNOWN";
	}
}

const char *dlm_midcomms_state(struct midcomms_node *node)
{
	return dlm_state_str(node->state);
}

unsigned long dlm_midcomms_flags(struct midcomms_node *node)
{
	return node->flags;
}

int dlm_midcomms_send_queue_cnt(struct midcomms_node *node)
{
	return atomic_read(&node->send_queue_cnt);
}

uint32_t dlm_midcomms_version(struct midcomms_node *node)
{
	return node->version;
}

static struct midcomms_node *__find_node(int nodeid, int r)
{
	struct midcomms_node *node;

	hlist_for_each_entry_rcu(node, &node_hash[r], hlist) {
		if (node->nodeid == nodeid)
			return node;
	}

	return NULL;
}

static void dlm_mhandle_release(struct rcu_head *rcu)
{
	struct dlm_mhandle *mh = container_of(rcu, struct dlm_mhandle, rcu);

	dlm_lowcomms_put_msg(mh->msg);
	dlm_free_mhandle(mh);
}

static void dlm_mhandle_delete(struct midcomms_node *node,
			       struct dlm_mhandle *mh)
{
	list_del_rcu(&mh->list);
	atomic_dec(&node->send_queue_cnt);
	call_rcu(&mh->rcu, dlm_mhandle_release);
}

static void dlm_send_queue_flush(struct midcomms_node *node)
{
	struct dlm_mhandle *mh;

	pr_debug("flush midcomms send queue of node %d\n", node->nodeid);

	rcu_read_lock();
	spin_lock_bh(&node->send_queue_lock);
	list_for_each_entry_rcu(mh, &node->send_queue, list) {
		dlm_mhandle_delete(node, mh);
	}
	spin_unlock_bh(&node->send_queue_lock);
	rcu_read_unlock();
}

static void midcomms_node_reset(struct midcomms_node *node)
{
	pr_debug("reset node %d\n", node->nodeid);

	atomic_set(&node->seq_next, DLM_SEQ_INIT);
	atomic_set(&node->seq_send, DLM_SEQ_INIT);
	atomic_set(&node->ulp_delivered, 0);
	node->version = DLM_VERSION_NOT_SET;
	node->flags = 0;

	dlm_send_queue_flush(node);
	node->state = DLM_CLOSED;
	wake_up(&node->shutdown_wait);
}

static struct midcomms_node *nodeid2node(int nodeid)
{
	return __find_node(nodeid, nodeid_hash(nodeid));
}

int dlm_midcomms_addr(int nodeid, struct sockaddr_storage *addr, int len)
{
	int ret, r = nodeid_hash(nodeid);
	struct midcomms_node *node;

	ret = dlm_lowcomms_addr(nodeid, addr, len);
	if (ret)
		return ret;

	node = kmalloc(sizeof(*node), GFP_NOFS);
	if (!node)
		return -ENOMEM;

	node->nodeid = nodeid;
	spin_lock_init(&node->state_lock);
	spin_lock_init(&node->send_queue_lock);
	atomic_set(&node->send_queue_cnt, 0);
	INIT_LIST_HEAD(&node->send_queue);
	init_waitqueue_head(&node->shutdown_wait);
	node->users = 0;
	midcomms_node_reset(node);

	spin_lock(&nodes_lock);
	hlist_add_head_rcu(&node->hlist, &node_hash[r]);
	spin_unlock(&nodes_lock);

	node->debugfs = dlm_create_debug_comms_file(nodeid, node);
	return 0;
}

static int dlm_send_ack(int nodeid, uint32_t seq)
{
	int mb_len = sizeof(struct dlm_header);
	struct dlm_header *m_header;
	struct dlm_msg *msg;
	char *ppc;

	msg = dlm_lowcomms_new_msg(nodeid, mb_len, GFP_ATOMIC, &ppc,
				   NULL, NULL);
	if (!msg)
		return -ENOMEM;

	m_header = (struct dlm_header *)ppc;

	m_header->h_version = cpu_to_le32(DLM_HEADER_MAJOR | DLM_HEADER_MINOR);
	m_header->h_nodeid = cpu_to_le32(dlm_our_nodeid());
	m_header->h_length = cpu_to_le16(mb_len);
	m_header->h_cmd = DLM_ACK;
	m_header->u.h_seq = cpu_to_le32(seq);

	dlm_lowcomms_commit_msg(msg);
	dlm_lowcomms_put_msg(msg);

	return 0;
}

static void dlm_send_ack_threshold(struct midcomms_node *node,
				   uint32_t threshold)
{
	uint32_t oval, nval;
	bool send_ack;

	/* let only send one user trigger threshold to send ack back */
	do {
		oval = atomic_read(&node->ulp_delivered);
		send_ack = (oval > threshold);
		/* abort if threshold is not reached */
		if (!send_ack)
			break;

		nval = 0;
		/* try to reset ulp_delivered counter */
	} while (atomic_cmpxchg(&node->ulp_delivered, oval, nval) != oval);

	if (send_ack)
		dlm_send_ack(node->nodeid, atomic_read(&node->seq_next));
}

static int dlm_send_fin(struct midcomms_node *node,
			void (*ack_rcv)(struct midcomms_node *node))
{
	int mb_len = sizeof(struct dlm_header);
	struct dlm_header *m_header;
	struct dlm_mhandle *mh;
	char *ppc;

	mh = dlm_midcomms_get_mhandle(node->nodeid, mb_len, GFP_ATOMIC, &ppc);
	if (!mh)
		return -ENOMEM;

	set_bit(DLM_NODE_FLAG_STOP_TX, &node->flags);
	mh->ack_rcv = ack_rcv;

	m_header = (struct dlm_header *)ppc;

	m_header->h_version = cpu_to_le32(DLM_HEADER_MAJOR | DLM_HEADER_MINOR);
	m_header->h_nodeid = cpu_to_le32(dlm_our_nodeid());
	m_header->h_length = cpu_to_le16(mb_len);
	m_header->h_cmd = DLM_FIN;

	pr_debug("sending fin msg to node %d\n", node->nodeid);
	dlm_midcomms_commit_mhandle(mh, NULL, 0);

	return 0;
}

static void dlm_receive_ack(struct midcomms_node *node, uint32_t seq)
{
	struct dlm_mhandle *mh;

	rcu_read_lock();
	list_for_each_entry_rcu(mh, &node->send_queue, list) {
		if (before(mh->seq, seq)) {
			if (mh->ack_rcv)
				mh->ack_rcv(node);
		} else {
			/* send queue should be ordered */
			break;
		}
	}

	spin_lock_bh(&node->send_queue_lock);
	list_for_each_entry_rcu(mh, &node->send_queue, list) {
		if (before(mh->seq, seq)) {
			dlm_mhandle_delete(node, mh);
		} else {
			/* send queue should be ordered */
			break;
		}
	}
	spin_unlock_bh(&node->send_queue_lock);
	rcu_read_unlock();
}

static void dlm_pas_fin_ack_rcv(struct midcomms_node *node)
{
	spin_lock(&node->state_lock);
	pr_debug("receive passive fin ack from node %d with state %s\n",
		 node->nodeid, dlm_state_str(node->state));

	switch (node->state) {
	case DLM_LAST_ACK:
		/* DLM_CLOSED */
		midcomms_node_reset(node);
		break;
	case DLM_CLOSED:
		/* not valid but somehow we got what we want */
		wake_up(&node->shutdown_wait);
		break;
	default:
		spin_unlock(&node->state_lock);
		log_print("%s: unexpected state: %d",
			  __func__, node->state);
		WARN_ON_ONCE(1);
		return;
	}
	spin_unlock(&node->state_lock);
}

static void dlm_receive_buffer_3_2_trace(uint32_t seq,
					 const union dlm_packet *p)
{
	switch (p->header.h_cmd) {
	case DLM_MSG:
		trace_dlm_recv_message(dlm_our_nodeid(), seq, &p->message);
		break;
	case DLM_RCOM:
		trace_dlm_recv_rcom(dlm_our_nodeid(), seq, &p->rcom);
		break;
	default:
		break;
	}
}

static void dlm_midcomms_receive_buffer(const union dlm_packet *p,
					struct midcomms_node *node,
					uint32_t seq)
{
	bool is_expected_seq;
	uint32_t oval, nval;

	do {
		oval = atomic_read(&node->seq_next);
		is_expected_seq = (oval == seq);
		if (!is_expected_seq)
			break;

		nval = oval + 1;
	} while (atomic_cmpxchg(&node->seq_next, oval, nval) != oval);

	if (is_expected_seq) {
		switch (p->header.h_cmd) {
		case DLM_FIN:
			spin_lock(&node->state_lock);
			pr_debug("receive fin msg from node %d with state %s\n",
				 node->nodeid, dlm_state_str(node->state));

			switch (node->state) {
			case DLM_ESTABLISHED:
				dlm_send_ack(node->nodeid, nval);

				/* passive shutdown DLM_LAST_ACK case 1
				 * additional we check if the node is used by
				 * cluster manager events at all.
				 */
				if (node->users == 0) {
					node->state = DLM_LAST_ACK;
					pr_debug("switch node %d to state %s case 1\n",
						 node->nodeid, dlm_state_str(node->state));
					set_bit(DLM_NODE_FLAG_STOP_RX, &node->flags);
					dlm_send_fin(node, dlm_pas_fin_ack_rcv);
				} else {
					node->state = DLM_CLOSE_WAIT;
					pr_debug("switch node %d to state %s\n",
						 node->nodeid, dlm_state_str(node->state));
				}
				break;
			case DLM_FIN_WAIT1:
				dlm_send_ack(node->nodeid, nval);
				node->state = DLM_CLOSING;
				set_bit(DLM_NODE_FLAG_STOP_RX, &node->flags);
				pr_debug("switch node %d to state %s\n",
					 node->nodeid, dlm_state_str(node->state));
				break;
			case DLM_FIN_WAIT2:
				dlm_send_ack(node->nodeid, nval);
				midcomms_node_reset(node);
				pr_debug("switch node %d to state %s\n",
					 node->nodeid, dlm_state_str(node->state));
				break;
			case DLM_LAST_ACK:
				/* probably remove_member caught it, do nothing */
				break;
			default:
				spin_unlock(&node->state_lock);
				log_print("%s: unexpected state: %d",
					  __func__, node->state);
				WARN_ON_ONCE(1);
				return;
			}
			spin_unlock(&node->state_lock);
			break;
		default:
			WARN_ON_ONCE(test_bit(DLM_NODE_FLAG_STOP_RX, &node->flags));
			dlm_receive_buffer_3_2_trace(seq, p);
			dlm_receive_buffer(p, node->nodeid);
			atomic_inc(&node->ulp_delivered);
			/* unlikely case to send ack back when we don't transmit */
			dlm_send_ack_threshold(node, DLM_RECV_ACK_BACK_MSG_THRESHOLD);
			break;
		}
	} else {
		/* retry to ack message which we already have by sending back
		 * current node->seq_next number as ack.
		 */
		if (seq < oval)
			dlm_send_ack(node->nodeid, oval);

		log_print_ratelimited("ignore dlm msg because seq mismatch, seq: %u, expected: %u, nodeid: %d",
				      seq, oval, node->nodeid);
	}
}

static int dlm_opts_check_msglen(const union dlm_packet *p, uint16_t msglen,
				 int nodeid)
{
	int len = msglen;

	/* we only trust outer header msglen because
	 * it's checked against receive buffer length.
	 */
	if (len < sizeof(struct dlm_opts))
		return -1;
	len -= sizeof(struct dlm_opts);

	if (len < le16_to_cpu(p->opts.o_optlen))
		return -1;
	len -= le16_to_cpu(p->opts.o_optlen);

	switch (p->opts.o_nextcmd) {
	case DLM_FIN:
		if (len < sizeof(struct dlm_header)) {
			log_print("fin too small: %d, will skip this message from node %d",
				  len, nodeid);
			return -1;
		}

		break;
	case DLM_MSG:
		if (len < sizeof(struct dlm_message)) {
			log_print("msg too small: %d, will skip this message from node %d",
				  msglen, nodeid);
			return -1;
		}

		break;
	case DLM_RCOM:
		if (len < sizeof(struct dlm_rcom)) {
			log_print("rcom msg too small: %d, will skip this message from node %d",
				  len, nodeid);
			return -1;
		}

		break;
	default:
		log_print("unsupported o_nextcmd received: %u, will skip this message from node %d",
			  p->opts.o_nextcmd, nodeid);
		return -1;
	}

	return 0;
}

static void dlm_midcomms_receive_buffer_3_2(const union dlm_packet *p, int nodeid)
{
	uint16_t msglen = le16_to_cpu(p->header.h_length);
	struct midcomms_node *node;
	uint32_t seq;
	int ret, idx;

	idx = srcu_read_lock(&nodes_srcu);
	node = nodeid2node(nodeid);
	if (WARN_ON_ONCE(!node))
		goto out;

	switch (node->version) {
	case DLM_VERSION_NOT_SET:
		node->version = DLM_VERSION_3_2;
		wake_up(&node->shutdown_wait);
		log_print("version 0x%08x for node %d detected", DLM_VERSION_3_2,
			  node->nodeid);

		spin_lock(&node->state_lock);
		switch (node->state) {
		case DLM_CLOSED:
			node->state = DLM_ESTABLISHED;
			pr_debug("switch node %d to state %s\n",
				 node->nodeid, dlm_state_str(node->state));
			break;
		default:
			break;
		}
		spin_unlock(&node->state_lock);

		break;
	case DLM_VERSION_3_2:
		break;
	default:
		log_print_ratelimited("version mismatch detected, assumed 0x%08x but node %d has 0x%08x",
				      DLM_VERSION_3_2, node->nodeid, node->version);
		goto out;
	}

	switch (p->header.h_cmd) {
	case DLM_RCOM:
		/* these rcom message we use to determine version.
		 * they have their own retransmission handling and
		 * are the first messages of dlm.
		 *
		 * length already checked.
		 */
		switch (p->rcom.rc_type) {
		case cpu_to_le32(DLM_RCOM_NAMES):
			fallthrough;
		case cpu_to_le32(DLM_RCOM_NAMES_REPLY):
			fallthrough;
		case cpu_to_le32(DLM_RCOM_STATUS):
			fallthrough;
		case cpu_to_le32(DLM_RCOM_STATUS_REPLY):
			break;
		default:
			log_print("unsupported rcom type received: %u, will skip this message from node %d",
				  le32_to_cpu(p->rcom.rc_type), nodeid);
			goto out;
		}

		WARN_ON_ONCE(test_bit(DLM_NODE_FLAG_STOP_RX, &node->flags));
		dlm_receive_buffer(p, nodeid);
		break;
	case DLM_OPTS:
		seq = le32_to_cpu(p->header.u.h_seq);

		ret = dlm_opts_check_msglen(p, msglen, nodeid);
		if (ret < 0) {
			log_print("opts msg too small: %u, will skip this message from node %d",
				  msglen, nodeid);
			goto out;
		}

		p = (union dlm_packet *)((unsigned char *)p->opts.o_opts +
					 le16_to_cpu(p->opts.o_optlen));

		/* recheck inner msglen just if it's not garbage */
		msglen = le16_to_cpu(p->header.h_length);
		switch (p->header.h_cmd) {
		case DLM_RCOM:
			if (msglen < sizeof(struct dlm_rcom)) {
				log_print("inner rcom msg too small: %u, will skip this message from node %d",
					  msglen, nodeid);
				goto out;
			}

			break;
		case DLM_MSG:
			if (msglen < sizeof(struct dlm_message)) {
				log_print("inner msg too small: %u, will skip this message from node %d",
					  msglen, nodeid);
				goto out;
			}

			break;
		case DLM_FIN:
			if (msglen < sizeof(struct dlm_header)) {
				log_print("inner fin too small: %u, will skip this message from node %d",
					  msglen, nodeid);
				goto out;
			}

			break;
		default:
			log_print("unsupported inner h_cmd received: %u, will skip this message from node %d",
				  msglen, nodeid);
			goto out;
		}

		dlm_midcomms_receive_buffer(p, node, seq);
		break;
	case DLM_ACK:
		seq = le32_to_cpu(p->header.u.h_seq);
		dlm_receive_ack(node, seq);
		break;
	default:
		log_print("unsupported h_cmd received: %u, will skip this message from node %d",
			  p->header.h_cmd, nodeid);
		break;
	}

out:
	srcu_read_unlock(&nodes_srcu, idx);
}

static void dlm_midcomms_receive_buffer_3_1(const union dlm_packet *p, int nodeid)
{
	uint16_t msglen = le16_to_cpu(p->header.h_length);
	struct midcomms_node *node;
	int idx;

	idx = srcu_read_lock(&nodes_srcu);
	node = nodeid2node(nodeid);
	if (WARN_ON_ONCE(!node)) {
		srcu_read_unlock(&nodes_srcu, idx);
		return;
	}

	switch (node->version) {
	case DLM_VERSION_NOT_SET:
		node->version = DLM_VERSION_3_1;
		wake_up(&node->shutdown_wait);
		log_print("version 0x%08x for node %d detected", DLM_VERSION_3_1,
			  node->nodeid);
		break;
	case DLM_VERSION_3_1:
		break;
	default:
		log_print_ratelimited("version mismatch detected, assumed 0x%08x but node %d has 0x%08x",
				      DLM_VERSION_3_1, node->nodeid, node->version);
		srcu_read_unlock(&nodes_srcu, idx);
		return;
	}
	srcu_read_unlock(&nodes_srcu, idx);

	switch (p->header.h_cmd) {
	case DLM_RCOM:
		/* length already checked */
		break;
	case DLM_MSG:
		if (msglen < sizeof(struct dlm_message)) {
			log_print("msg too small: %u, will skip this message from node %d",
				  msglen, nodeid);
			return;
		}

		break;
	default:
		log_print("unsupported h_cmd received: %u, will skip this message from node %d",
			  p->header.h_cmd, nodeid);
		return;
	}

	dlm_receive_buffer(p, nodeid);
}

int dlm_validate_incoming_buffer(int nodeid, unsigned char *buf, int len)
{
	const unsigned char *ptr = buf;
	const struct dlm_header *hd;
	uint16_t msglen;
	int ret = 0;

	while (len >= sizeof(struct dlm_header)) {
		hd = (struct dlm_header *)ptr;

		/* no message should be more than DLM_MAX_SOCKET_BUFSIZE or
		 * less than dlm_header size.
		 *
		 * Some messages does not have a 8 byte length boundary yet
		 * which can occur in a unaligned memory access of some dlm
		 * messages. However this problem need to be fixed at the
		 * sending side, for now it seems nobody run into architecture
		 * related issues yet but it slows down some processing.
		 * Fixing this issue should be scheduled in future by doing
		 * the next major version bump.
		 */
		msglen = le16_to_cpu(hd->h_length);
		if (msglen > DLM_MAX_SOCKET_BUFSIZE ||
		    msglen < sizeof(struct dlm_header)) {
			log_print("received invalid length header: %u from node %d, will abort message parsing",
				  msglen, nodeid);
			return -EBADMSG;
		}

		/* caller will take care that leftover
		 * will be parsed next call with more data
		 */
		if (msglen > len)
			break;

		ret += msglen;
		len -= msglen;
		ptr += msglen;
	}

	return ret;
}

/*
 * Called from the low-level comms layer to process a buffer of
 * commands.
 */
int dlm_process_incoming_buffer(int nodeid, unsigned char *buf, int len)
{
	const unsigned char *ptr = buf;
	const struct dlm_header *hd;
	uint16_t msglen;
	int ret = 0;

	while (len >= sizeof(struct dlm_header)) {
		hd = (struct dlm_header *)ptr;

		msglen = le16_to_cpu(hd->h_length);
		if (msglen > len)
			break;

		switch (hd->h_version) {
		case cpu_to_le32(DLM_VERSION_3_1):
			dlm_midcomms_receive_buffer_3_1((const union dlm_packet *)ptr, nodeid);
			break;
		case cpu_to_le32(DLM_VERSION_3_2):
			dlm_midcomms_receive_buffer_3_2((const union dlm_packet *)ptr, nodeid);
			break;
		default:
			log_print("received invalid version header: %u from node %d, will skip this message",
				  le32_to_cpu(hd->h_version), nodeid);
			break;
		}

		ret += msglen;
		len -= msglen;
		ptr += msglen;
	}

	return ret;
}

void dlm_midcomms_unack_msg_resend(int nodeid)
{
	struct midcomms_node *node;
	struct dlm_mhandle *mh;
	int idx, ret;

	idx = srcu_read_lock(&nodes_srcu);
	node = nodeid2node(nodeid);
	if (WARN_ON_ONCE(!node)) {
		srcu_read_unlock(&nodes_srcu, idx);
		return;
	}

	/* old protocol, we don't support to retransmit on failure */
	switch (node->version) {
	case DLM_VERSION_3_2:
		break;
	default:
		srcu_read_unlock(&nodes_srcu, idx);
		return;
	}

	rcu_read_lock();
	list_for_each_entry_rcu(mh, &node->send_queue, list) {
		if (!mh->committed)
			continue;

		ret = dlm_lowcomms_resend_msg(mh->msg);
		if (!ret)
			log_print_ratelimited("retransmit dlm msg, seq %u, nodeid %d",
					      mh->seq, node->nodeid);
	}
	rcu_read_unlock();
	srcu_read_unlock(&nodes_srcu, idx);
}

static void dlm_fill_opts_header(struct dlm_opts *opts, uint16_t inner_len,
				 uint32_t seq)
{
	opts->o_header.h_cmd = DLM_OPTS;
	opts->o_header.h_version = cpu_to_le32(DLM_HEADER_MAJOR | DLM_HEADER_MINOR);
	opts->o_header.h_nodeid = cpu_to_le32(dlm_our_nodeid());
	opts->o_header.h_length = cpu_to_le16(DLM_MIDCOMMS_OPT_LEN + inner_len);
	opts->o_header.u.h_seq = cpu_to_le32(seq);
}

static void midcomms_new_msg_cb(void *data)
{
	struct dlm_mhandle *mh = data;

	atomic_inc(&mh->node->send_queue_cnt);

	spin_lock_bh(&mh->node->send_queue_lock);
	list_add_tail_rcu(&mh->list, &mh->node->send_queue);
	spin_unlock_bh(&mh->node->send_queue_lock);

	mh->seq = atomic_fetch_inc(&mh->node->seq_send);
}

static struct dlm_msg *dlm_midcomms_get_msg_3_2(struct dlm_mhandle *mh, int nodeid,
						int len, gfp_t allocation, char **ppc)
{
	struct dlm_opts *opts;
	struct dlm_msg *msg;

	msg = dlm_lowcomms_new_msg(nodeid, len + DLM_MIDCOMMS_OPT_LEN,
				   allocation, ppc, midcomms_new_msg_cb, mh);
	if (!msg)
		return NULL;

	opts = (struct dlm_opts *)*ppc;
	mh->opts = opts;

	/* add possible options here */
	dlm_fill_opts_header(opts, len, mh->seq);

	*ppc += sizeof(*opts);
	mh->inner_p = (const union dlm_packet *)*ppc;
	return msg;
}

/* avoid false positive for nodes_srcu, unlock happens in
 * dlm_midcomms_commit_mhandle which is a must call if success
 */
#ifndef __CHECKER__
struct dlm_mhandle *dlm_midcomms_get_mhandle(int nodeid, int len,
					     gfp_t allocation, char **ppc)
{
	struct midcomms_node *node;
	struct dlm_mhandle *mh;
	struct dlm_msg *msg;
	int idx;

	idx = srcu_read_lock(&nodes_srcu);
	node = nodeid2node(nodeid);
	if (WARN_ON_ONCE(!node))
		goto err;

	/* this is a bug, however we going on and hope it will be resolved */
	WARN_ON_ONCE(test_bit(DLM_NODE_FLAG_STOP_TX, &node->flags));

	mh = dlm_allocate_mhandle(allocation);
	if (!mh)
		goto err;

	mh->committed = false;
	mh->ack_rcv = NULL;
	mh->idx = idx;
	mh->node = node;

	switch (node->version) {
	case DLM_VERSION_3_1:
		msg = dlm_lowcomms_new_msg(nodeid, len, allocation, ppc,
					   NULL, NULL);
		if (!msg) {
			dlm_free_mhandle(mh);
			goto err;
		}

		break;
	case DLM_VERSION_3_2:
		msg = dlm_midcomms_get_msg_3_2(mh, nodeid, len, allocation,
					       ppc);
		if (!msg) {
			dlm_free_mhandle(mh);
			goto err;
		}

		/* send ack back if necessary */
		dlm_send_ack_threshold(node, DLM_SEND_ACK_BACK_MSG_THRESHOLD);
		break;
	default:
		dlm_free_mhandle(mh);
		WARN_ON_ONCE(1);
		goto err;
	}

	mh->msg = msg;

	/* keep in mind that is a must to call
	 * dlm_midcomms_commit_msg() which releases
	 * nodes_srcu using mh->idx which is assumed
	 * here that the application will call it.
	 */
	return mh;

err:
	srcu_read_unlock(&nodes_srcu, idx);
	return NULL;
}
#endif

static void dlm_midcomms_commit_msg_3_2_trace(const struct dlm_mhandle *mh,
					      const void *name, int namelen)
{
	switch (mh->inner_p->header.h_cmd) {
	case DLM_MSG:
		trace_dlm_send_message(mh->node->nodeid, mh->seq,
				       &mh->inner_p->message,
				       name, namelen);
		break;
	case DLM_RCOM:
		trace_dlm_send_rcom(mh->node->nodeid, mh->seq,
				    &mh->inner_p->rcom);
		break;
	default:
		/* nothing to trace */
		break;
	}
}

static void dlm_midcomms_commit_msg_3_2(struct dlm_mhandle *mh,
					const void *name, int namelen)
{
	/* nexthdr chain for fast lookup */
	mh->opts->o_nextcmd = mh->inner_p->header.h_cmd;
	mh->committed = true;
	dlm_midcomms_commit_msg_3_2_trace(mh, name, namelen);
	dlm_lowcomms_commit_msg(mh->msg);
}

/* avoid false positive for nodes_srcu, lock was happen in
 * dlm_midcomms_get_mhandle
 */
#ifndef __CHECKER__
void dlm_midcomms_commit_mhandle(struct dlm_mhandle *mh,
				 const void *name, int namelen)
{

	switch (mh->node->version) {
	case DLM_VERSION_3_1:
		srcu_read_unlock(&nodes_srcu, mh->idx);

		dlm_lowcomms_commit_msg(mh->msg);
		dlm_lowcomms_put_msg(mh->msg);
		/* mh is not part of rcu list in this case */
		dlm_free_mhandle(mh);
		break;
	case DLM_VERSION_3_2:
		/* held rcu read lock here, because we sending the
		 * dlm message out, when we do that we could receive
		 * an ack back which releases the mhandle and we
		 * get a use after free.
		 */
		rcu_read_lock();
		dlm_midcomms_commit_msg_3_2(mh, name, namelen);
		srcu_read_unlock(&nodes_srcu, mh->idx);
		rcu_read_unlock();
		break;
	default:
		srcu_read_unlock(&nodes_srcu, mh->idx);
		WARN_ON_ONCE(1);
		break;
	}
}
#endif

int dlm_midcomms_start(void)
{
	return dlm_lowcomms_start();
}

void dlm_midcomms_stop(void)
{
	dlm_lowcomms_stop();
}

void dlm_midcomms_init(void)
{
	int i;

	for (i = 0; i < CONN_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&node_hash[i]);

	dlm_lowcomms_init();
}

static void midcomms_node_release(struct rcu_head *rcu)
{
	struct midcomms_node *node = container_of(rcu, struct midcomms_node, rcu);

	WARN_ON_ONCE(atomic_read(&node->send_queue_cnt));
	dlm_send_queue_flush(node);
	kfree(node);
}

void dlm_midcomms_exit(void)
{
	struct midcomms_node *node;
	int i, idx;

	idx = srcu_read_lock(&nodes_srcu);
	for (i = 0; i < CONN_HASH_SIZE; i++) {
		hlist_for_each_entry_rcu(node, &node_hash[i], hlist) {
			dlm_delete_debug_comms_file(node->debugfs);

			spin_lock(&nodes_lock);
			hlist_del_rcu(&node->hlist);
			spin_unlock(&nodes_lock);

			call_srcu(&nodes_srcu, &node->rcu, midcomms_node_release);
		}
	}
	srcu_read_unlock(&nodes_srcu, idx);

	dlm_lowcomms_exit();
}

static void dlm_act_fin_ack_rcv(struct midcomms_node *node)
{
	spin_lock(&node->state_lock);
	pr_debug("receive active fin ack from node %d with state %s\n",
		 node->nodeid, dlm_state_str(node->state));

	switch (node->state) {
	case DLM_FIN_WAIT1:
		node->state = DLM_FIN_WAIT2;
		pr_debug("switch node %d to state %s\n",
			 node->nodeid, dlm_state_str(node->state));
		break;
	case DLM_CLOSING:
		midcomms_node_reset(node);
		pr_debug("switch node %d to state %s\n",
			 node->nodeid, dlm_state_str(node->state));
		break;
	case DLM_CLOSED:
		/* not valid but somehow we got what we want */
		wake_up(&node->shutdown_wait);
		break;
	default:
		spin_unlock(&node->state_lock);
		log_print("%s: unexpected state: %d",
			  __func__, node->state);
		WARN_ON_ONCE(1);
		return;
	}
	spin_unlock(&node->state_lock);
}

void dlm_midcomms_add_member(int nodeid)
{
	struct midcomms_node *node;
	int idx;

	idx = srcu_read_lock(&nodes_srcu);
	node = nodeid2node(nodeid);
	if (WARN_ON_ONCE(!node)) {
		srcu_read_unlock(&nodes_srcu, idx);
		return;
	}

	spin_lock(&node->state_lock);
	if (!node->users) {
		pr_debug("receive add member from node %d with state %s\n",
			 node->nodeid, dlm_state_str(node->state));
		switch (node->state) {
		case DLM_ESTABLISHED:
			break;
		case DLM_CLOSED:
			node->state = DLM_ESTABLISHED;
			pr_debug("switch node %d to state %s\n",
				 node->nodeid, dlm_state_str(node->state));
			break;
		default:
			/* some invalid state passive shutdown
			 * was failed, we try to reset and
			 * hope it will go on.
			 */
			log_print("reset node %d because shutdown stuck",
				  node->nodeid);

			midcomms_node_reset(node);
			node->state = DLM_ESTABLISHED;
			break;
		}
	}

	node->users++;
	pr_debug("node %d users inc count %d\n", nodeid, node->users);
	spin_unlock(&node->state_lock);

	srcu_read_unlock(&nodes_srcu, idx);
}

void dlm_midcomms_remove_member(int nodeid)
{
	struct midcomms_node *node;
	int idx;

	idx = srcu_read_lock(&nodes_srcu);
	node = nodeid2node(nodeid);
	if (WARN_ON_ONCE(!node)) {
		srcu_read_unlock(&nodes_srcu, idx);
		return;
	}

	spin_lock(&node->state_lock);
	node->users--;
	pr_debug("node %d users dec count %d\n", nodeid, node->users);

	/* hitting users count to zero means the
	 * other side is running dlm_midcomms_stop()
	 * we meet us to have a clean disconnect.
	 */
	if (node->users == 0) {
		pr_debug("receive remove member from node %d with state %s\n",
			 node->nodeid, dlm_state_str(node->state));
		switch (node->state) {
		case DLM_ESTABLISHED:
			break;
		case DLM_CLOSE_WAIT:
			/* passive shutdown DLM_LAST_ACK case 2 */
			node->state = DLM_LAST_ACK;
			pr_debug("switch node %d to state %s case 2\n",
				 node->nodeid, dlm_state_str(node->state));
			set_bit(DLM_NODE_FLAG_STOP_RX, &node->flags);
			dlm_send_fin(node, dlm_pas_fin_ack_rcv);
			break;
		case DLM_LAST_ACK:
			/* probably receive fin caught it, do nothing */
			break;
		case DLM_CLOSED:
			/* already gone, do nothing */
			break;
		default:
			log_print("%s: unexpected state: %d",
				  __func__, node->state);
			break;
		}
	}
	spin_unlock(&node->state_lock);

	srcu_read_unlock(&nodes_srcu, idx);
}

void dlm_midcomms_version_wait(void)
{
	struct midcomms_node *node;
	int i, idx, ret;

	idx = srcu_read_lock(&nodes_srcu);
	for (i = 0; i < CONN_HASH_SIZE; i++) {
		hlist_for_each_entry_rcu(node, &node_hash[i], hlist) {
			ret = wait_event_timeout(node->shutdown_wait,
						 node->version != DLM_VERSION_NOT_SET ||
						 node->state == DLM_CLOSED ||
						 test_bit(DLM_NODE_FLAG_CLOSE, &node->flags),
						 DLM_SHUTDOWN_TIMEOUT);
			if (!ret || test_bit(DLM_NODE_FLAG_CLOSE, &node->flags))
				pr_debug("version wait timed out for node %d with state %s\n",
					 node->nodeid, dlm_state_str(node->state));
		}
	}
	srcu_read_unlock(&nodes_srcu, idx);
}

static void midcomms_shutdown(struct midcomms_node *node)
{
	int ret;

	/* old protocol, we don't wait for pending operations */
	switch (node->version) {
	case DLM_VERSION_3_2:
		break;
	default:
		return;
	}

	spin_lock(&node->state_lock);
	pr_debug("receive active shutdown for node %d with state %s\n",
		 node->nodeid, dlm_state_str(node->state));
	switch (node->state) {
	case DLM_ESTABLISHED:
		node->state = DLM_FIN_WAIT1;
		pr_debug("switch node %d to state %s case 2\n",
			 node->nodeid, dlm_state_str(node->state));
		dlm_send_fin(node, dlm_act_fin_ack_rcv);
		break;
	case DLM_CLOSED:
		/* we have what we want */
		break;
	default:
		/* busy to enter DLM_FIN_WAIT1, wait until passive
		 * done in shutdown_wait to enter DLM_CLOSED.
		 */
		break;
	}
	spin_unlock(&node->state_lock);

	if (DLM_DEBUG_FENCE_TERMINATION)
		msleep(5000);

	/* wait for other side dlm + fin */
	ret = wait_event_timeout(node->shutdown_wait,
				 node->state == DLM_CLOSED ||
				 test_bit(DLM_NODE_FLAG_CLOSE, &node->flags),
				 DLM_SHUTDOWN_TIMEOUT);
	if (!ret)
		pr_debug("active shutdown timed out for node %d with state %s\n",
			 node->nodeid, dlm_state_str(node->state));
	else
		pr_debug("active shutdown done for node %d with state %s\n",
			 node->nodeid, dlm_state_str(node->state));
}

void dlm_midcomms_shutdown(void)
{
	struct midcomms_node *node;
	int i, idx;

	mutex_lock(&close_lock);
	idx = srcu_read_lock(&nodes_srcu);
	for (i = 0; i < CONN_HASH_SIZE; i++) {
		hlist_for_each_entry_rcu(node, &node_hash[i], hlist) {
			midcomms_shutdown(node);
		}
	}
	srcu_read_unlock(&nodes_srcu, idx);
	mutex_unlock(&close_lock);

	dlm_lowcomms_shutdown();
}

int dlm_midcomms_close(int nodeid)
{
	struct midcomms_node *node;
	int idx, ret;

	idx = srcu_read_lock(&nodes_srcu);
	/* Abort pending close/remove operation */
	node = nodeid2node(nodeid);
	if (node) {
		/* let shutdown waiters leave */
		set_bit(DLM_NODE_FLAG_CLOSE, &node->flags);
		wake_up(&node->shutdown_wait);
	}
	srcu_read_unlock(&nodes_srcu, idx);

	synchronize_srcu(&nodes_srcu);

	mutex_lock(&close_lock);
	idx = srcu_read_lock(&nodes_srcu);
	node = nodeid2node(nodeid);
	if (!node) {
		srcu_read_unlock(&nodes_srcu, idx);
		mutex_unlock(&close_lock);
		return dlm_lowcomms_close(nodeid);
	}

	ret = dlm_lowcomms_close(nodeid);
	dlm_delete_debug_comms_file(node->debugfs);

	spin_lock(&nodes_lock);
	hlist_del_rcu(&node->hlist);
	spin_unlock(&nodes_lock);
	srcu_read_unlock(&nodes_srcu, idx);

	/* wait that all readers left until flush send queue */
	synchronize_srcu(&nodes_srcu);

	/* drop all pending dlm messages, this is fine as
	 * this function get called when the node is fenced
	 */
	dlm_send_queue_flush(node);

	call_srcu(&nodes_srcu, &node->rcu, midcomms_node_release);
	mutex_unlock(&close_lock);

	return ret;
}

/* debug functionality to send raw dlm msg from user space */
struct dlm_rawmsg_data {
	struct midcomms_node *node;
	void *buf;
};

static void midcomms_new_rawmsg_cb(void *data)
{
	struct dlm_rawmsg_data *rd = data;
	struct dlm_header *h = rd->buf;

	switch (h->h_version) {
	case cpu_to_le32(DLM_VERSION_3_1):
		break;
	default:
		switch (h->h_cmd) {
		case DLM_OPTS:
			if (!h->u.h_seq)
				h->u.h_seq = cpu_to_le32(atomic_fetch_inc(&rd->node->seq_send));
			break;
		default:
			break;
		}
		break;
	}
}

int dlm_midcomms_rawmsg_send(struct midcomms_node *node, void *buf,
			     int buflen)
{
	struct dlm_rawmsg_data rd;
	struct dlm_msg *msg;
	char *msgbuf;

	rd.node = node;
	rd.buf = buf;

	msg = dlm_lowcomms_new_msg(node->nodeid, buflen, GFP_NOFS,
				   &msgbuf, midcomms_new_rawmsg_cb, &rd);
	if (!msg)
		return -ENOMEM;

	memcpy(msgbuf, buf, buflen);
	dlm_lowcomms_commit_msg(msg);
	return 0;
}

