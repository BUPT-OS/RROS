/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the RAW-IP module.
 *
 * Version:	@(#)raw.h	1.0.2	05/07/93
 *
 * Author:	Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 */
#ifndef _RAW_H
#define _RAW_H

#include <net/inet_sock.h>
#include <net/protocol.h>
#include <net/netns/hash.h>
#include <linux/hash.h>
#include <linux/icmp.h>

extern struct proto raw_prot;

extern struct raw_hashinfo raw_v4_hashinfo;
bool raw_v4_match(struct net *net, const struct sock *sk, unsigned short num,
		  __be32 raddr, __be32 laddr, int dif, int sdif);

int raw_abort(struct sock *sk, int err);
void raw_icmp_error(struct sk_buff *, int, u32);
int raw_local_deliver(struct sk_buff *, int);

int raw_rcv(struct sock *, struct sk_buff *);

#define RAW_HTABLE_LOG	8
#define RAW_HTABLE_SIZE	(1U << RAW_HTABLE_LOG)

struct raw_hashinfo {
	spinlock_t lock;

	struct hlist_head ht[RAW_HTABLE_SIZE] ____cacheline_aligned;
};

static inline u32 raw_hashfunc(const struct net *net, u32 proto)
{
	return hash_32(net_hash_mix(net) ^ proto, RAW_HTABLE_LOG);
}

static inline void raw_hashinfo_init(struct raw_hashinfo *hashinfo)
{
	int i;

	spin_lock_init(&hashinfo->lock);
	for (i = 0; i < RAW_HTABLE_SIZE; i++)
		INIT_HLIST_HEAD(&hashinfo->ht[i]);
}

#ifdef CONFIG_PROC_FS
int raw_proc_init(void);
void raw_proc_exit(void);

struct raw_iter_state {
	struct seq_net_private p;
	int bucket;
};

static inline struct raw_iter_state *raw_seq_private(struct seq_file *seq)
{
	return seq->private;
}
void *raw_seq_start(struct seq_file *seq, loff_t *pos);
void *raw_seq_next(struct seq_file *seq, void *v, loff_t *pos);
void raw_seq_stop(struct seq_file *seq, void *v);
#endif

int raw_hash_sk(struct sock *sk);
void raw_unhash_sk(struct sock *sk);
void raw_init(void);

struct raw_sock {
	/* inet_sock has to be the first member */
	struct inet_sock   inet;
	struct icmp_filter filter;
	u32		   ipmr_table;
};

#define raw_sk(ptr) container_of_const(ptr, struct raw_sock, inet.sk)

static inline bool raw_sk_bound_dev_eq(struct net *net, int bound_dev_if,
				       int dif, int sdif)
{
#if IS_ENABLED(CONFIG_NET_L3_MASTER_DEV)
	return inet_bound_dev_eq(READ_ONCE(net->ipv4.sysctl_raw_l3mdev_accept),
				 bound_dev_if, dif, sdif);
#else
	return inet_bound_dev_eq(true, bound_dev_if, dif, sdif);
#endif
}

#endif	/* _RAW_H */
