/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_NET_AFUNIX_H
#define __LINUX_NET_AFUNIX_H

#include <linux/socket.h>
#include <linux/un.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <net/sock.h>

void unix_inflight(struct user_struct *user, struct file *fp);
void unix_notinflight(struct user_struct *user, struct file *fp);
void unix_destruct_scm(struct sk_buff *skb);
void io_uring_destruct_scm(struct sk_buff *skb);
void unix_gc(void);
void wait_for_unix_gc(void);
struct sock *unix_get_socket(struct file *filp);
struct sock *unix_peer_get(struct sock *sk);

#define UNIX_HASH_MOD	(256 - 1)
#define UNIX_HASH_SIZE	(256 * 2)
#define UNIX_HASH_BITS	8

extern unsigned int unix_tot_inflight;

struct unix_address {
	refcount_t	refcnt;
	int		len;
	struct sockaddr_un name[];
};

struct unix_skb_parms {
	struct pid		*pid;		/* Skb credentials	*/
	kuid_t			uid;
	kgid_t			gid;
	struct scm_fp_list	*fp;		/* Passed files		*/
#ifdef CONFIG_SECURITY_NETWORK
	u32			secid;		/* Security ID		*/
#endif
	u32			consumed;
} __randomize_layout;

struct scm_stat {
	atomic_t nr_fds;
};

#define UNIXCB(skb)	(*(struct unix_skb_parms *)&((skb)->cb))

#define unix_state_lock(s)	spin_lock(&unix_sk(s)->lock)
#define unix_state_unlock(s)	spin_unlock(&unix_sk(s)->lock)
#define unix_state_lock_nested(s) \
				spin_lock_nested(&unix_sk(s)->lock, \
				SINGLE_DEPTH_NESTING)

/* The AF_UNIX socket */
struct unix_sock {
	/* WARNING: sk has to be the first member */
	struct sock		sk;
	struct unix_address	*addr;
	struct path		path;
	struct mutex		iolock, bindlock;
	struct sock		*peer;
	struct list_head	link;
	atomic_long_t		inflight;
	spinlock_t		lock;
	unsigned long		gc_flags;
#define UNIX_GC_CANDIDATE	0
#define UNIX_GC_MAYBE_CYCLE	1
	struct socket_wq	peer_wq;
	wait_queue_entry_t	peer_wake;
	struct scm_stat		scm_stat;
#if IS_ENABLED(CONFIG_AF_UNIX_OOB)
	struct sk_buff		*oob_skb;
#endif
};

#define unix_sk(ptr) container_of_const(ptr, struct unix_sock, sk)

#define peer_wait peer_wq.wait

long unix_inq_len(struct sock *sk);
long unix_outq_len(struct sock *sk);

int __unix_dgram_recvmsg(struct sock *sk, struct msghdr *msg, size_t size,
			 int flags);
int __unix_stream_recvmsg(struct sock *sk, struct msghdr *msg, size_t size,
			  int flags);
#ifdef CONFIG_SYSCTL
int unix_sysctl_register(struct net *net);
void unix_sysctl_unregister(struct net *net);
#else
static inline int unix_sysctl_register(struct net *net) { return 0; }
static inline void unix_sysctl_unregister(struct net *net) {}
#endif

#ifdef CONFIG_BPF_SYSCALL
extern struct proto unix_dgram_proto;
extern struct proto unix_stream_proto;

int unix_dgram_bpf_update_proto(struct sock *sk, struct sk_psock *psock, bool restore);
int unix_stream_bpf_update_proto(struct sock *sk, struct sk_psock *psock, bool restore);
void __init unix_bpf_build_proto(void);
#else
static inline void __init unix_bpf_build_proto(void)
{}
#endif
#endif
