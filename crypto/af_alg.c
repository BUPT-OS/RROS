// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * af_alg: User-space algorithm interface
 *
 * This file provides the user-space API for algorithms.
 *
 * Copyright (c) 2010 Herbert Xu <herbert@gondor.apana.org.au>
 */

#include <linux/atomic.h>
#include <crypto/if_alg.h>
#include <linux/crypto.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/key.h>
#include <linux/key-type.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/security.h>
#include <linux/string.h>
#include <keys/user-type.h>
#include <keys/trusted-type.h>
#include <keys/encrypted-type.h>

struct alg_type_list {
	const struct af_alg_type *type;
	struct list_head list;
};

static struct proto alg_proto = {
	.name			= "ALG",
	.owner			= THIS_MODULE,
	.obj_size		= sizeof(struct alg_sock),
};

static LIST_HEAD(alg_types);
static DECLARE_RWSEM(alg_types_sem);

static const struct af_alg_type *alg_get_type(const char *name)
{
	const struct af_alg_type *type = ERR_PTR(-ENOENT);
	struct alg_type_list *node;

	down_read(&alg_types_sem);
	list_for_each_entry(node, &alg_types, list) {
		if (strcmp(node->type->name, name))
			continue;

		if (try_module_get(node->type->owner))
			type = node->type;
		break;
	}
	up_read(&alg_types_sem);

	return type;
}

int af_alg_register_type(const struct af_alg_type *type)
{
	struct alg_type_list *node;
	int err = -EEXIST;

	down_write(&alg_types_sem);
	list_for_each_entry(node, &alg_types, list) {
		if (!strcmp(node->type->name, type->name))
			goto unlock;
	}

	node = kmalloc(sizeof(*node), GFP_KERNEL);
	err = -ENOMEM;
	if (!node)
		goto unlock;

	type->ops->owner = THIS_MODULE;
	if (type->ops_nokey)
		type->ops_nokey->owner = THIS_MODULE;
	node->type = type;
	list_add(&node->list, &alg_types);
	err = 0;

unlock:
	up_write(&alg_types_sem);

	return err;
}
EXPORT_SYMBOL_GPL(af_alg_register_type);

int af_alg_unregister_type(const struct af_alg_type *type)
{
	struct alg_type_list *node;
	int err = -ENOENT;

	down_write(&alg_types_sem);
	list_for_each_entry(node, &alg_types, list) {
		if (strcmp(node->type->name, type->name))
			continue;

		list_del(&node->list);
		kfree(node);
		err = 0;
		break;
	}
	up_write(&alg_types_sem);

	return err;
}
EXPORT_SYMBOL_GPL(af_alg_unregister_type);

static void alg_do_release(const struct af_alg_type *type, void *private)
{
	if (!type)
		return;

	type->release(private);
	module_put(type->owner);
}

int af_alg_release(struct socket *sock)
{
	if (sock->sk) {
		sock_put(sock->sk);
		sock->sk = NULL;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(af_alg_release);

void af_alg_release_parent(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	unsigned int nokey = atomic_read(&ask->nokey_refcnt);

	sk = ask->parent;
	ask = alg_sk(sk);

	if (nokey)
		atomic_dec(&ask->nokey_refcnt);

	if (atomic_dec_and_test(&ask->refcnt))
		sock_put(sk);
}
EXPORT_SYMBOL_GPL(af_alg_release_parent);

static int alg_bind(struct socket *sock, struct sockaddr *uaddr, int addr_len)
{
	const u32 allowed = CRYPTO_ALG_KERN_DRIVER_ONLY;
	struct sock *sk = sock->sk;
	struct alg_sock *ask = alg_sk(sk);
	struct sockaddr_alg_new *sa = (void *)uaddr;
	const struct af_alg_type *type;
	void *private;
	int err;

	if (sock->state == SS_CONNECTED)
		return -EINVAL;

	BUILD_BUG_ON(offsetof(struct sockaddr_alg_new, salg_name) !=
		     offsetof(struct sockaddr_alg, salg_name));
	BUILD_BUG_ON(offsetof(struct sockaddr_alg, salg_name) != sizeof(*sa));

	if (addr_len < sizeof(*sa) + 1)
		return -EINVAL;

	/* If caller uses non-allowed flag, return error. */
	if ((sa->salg_feat & ~allowed) || (sa->salg_mask & ~allowed))
		return -EINVAL;

	sa->salg_type[sizeof(sa->salg_type) - 1] = 0;
	sa->salg_name[addr_len - sizeof(*sa) - 1] = 0;

	type = alg_get_type(sa->salg_type);
	if (PTR_ERR(type) == -ENOENT) {
		request_module("algif-%s", sa->salg_type);
		type = alg_get_type(sa->salg_type);
	}

	if (IS_ERR(type))
		return PTR_ERR(type);

	private = type->bind(sa->salg_name, sa->salg_feat, sa->salg_mask);
	if (IS_ERR(private)) {
		module_put(type->owner);
		return PTR_ERR(private);
	}

	err = -EBUSY;
	lock_sock(sk);
	if (atomic_read(&ask->refcnt))
		goto unlock;

	swap(ask->type, type);
	swap(ask->private, private);

	err = 0;

unlock:
	release_sock(sk);

	alg_do_release(type, private);

	return err;
}

static int alg_setkey(struct sock *sk, sockptr_t ukey, unsigned int keylen)
{
	struct alg_sock *ask = alg_sk(sk);
	const struct af_alg_type *type = ask->type;
	u8 *key;
	int err;

	key = sock_kmalloc(sk, keylen, GFP_KERNEL);
	if (!key)
		return -ENOMEM;

	err = -EFAULT;
	if (copy_from_sockptr(key, ukey, keylen))
		goto out;

	err = type->setkey(ask->private, key, keylen);

out:
	sock_kzfree_s(sk, key, keylen);

	return err;
}

#ifdef CONFIG_KEYS

static const u8 *key_data_ptr_user(const struct key *key,
				   unsigned int *datalen)
{
	const struct user_key_payload *ukp;

	ukp = user_key_payload_locked(key);
	if (IS_ERR_OR_NULL(ukp))
		return ERR_PTR(-EKEYREVOKED);

	*datalen = key->datalen;

	return ukp->data;
}

static const u8 *key_data_ptr_encrypted(const struct key *key,
					unsigned int *datalen)
{
	const struct encrypted_key_payload *ekp;

	ekp = dereference_key_locked(key);
	if (IS_ERR_OR_NULL(ekp))
		return ERR_PTR(-EKEYREVOKED);

	*datalen = ekp->decrypted_datalen;

	return ekp->decrypted_data;
}

static const u8 *key_data_ptr_trusted(const struct key *key,
				      unsigned int *datalen)
{
	const struct trusted_key_payload *tkp;

	tkp = dereference_key_locked(key);
	if (IS_ERR_OR_NULL(tkp))
		return ERR_PTR(-EKEYREVOKED);

	*datalen = tkp->key_len;

	return tkp->key;
}

static struct key *lookup_key(key_serial_t serial)
{
	key_ref_t key_ref;

	key_ref = lookup_user_key(serial, 0, KEY_NEED_SEARCH);
	if (IS_ERR(key_ref))
		return ERR_CAST(key_ref);

	return key_ref_to_ptr(key_ref);
}

static int alg_setkey_by_key_serial(struct alg_sock *ask, sockptr_t optval,
				    unsigned int optlen)
{
	const struct af_alg_type *type = ask->type;
	u8 *key_data = NULL;
	unsigned int key_datalen;
	key_serial_t serial;
	struct key *key;
	const u8 *ret;
	int err;

	if (optlen != sizeof(serial))
		return -EINVAL;

	if (copy_from_sockptr(&serial, optval, optlen))
		return -EFAULT;

	key = lookup_key(serial);
	if (IS_ERR(key))
		return PTR_ERR(key);

	down_read(&key->sem);

	ret = ERR_PTR(-ENOPROTOOPT);
	if (!strcmp(key->type->name, "user") ||
	    !strcmp(key->type->name, "logon")) {
		ret = key_data_ptr_user(key, &key_datalen);
	} else if (IS_REACHABLE(CONFIG_ENCRYPTED_KEYS) &&
			   !strcmp(key->type->name, "encrypted")) {
		ret = key_data_ptr_encrypted(key, &key_datalen);
	} else if (IS_REACHABLE(CONFIG_TRUSTED_KEYS) &&
			   !strcmp(key->type->name, "trusted")) {
		ret = key_data_ptr_trusted(key, &key_datalen);
	}

	if (IS_ERR(ret)) {
		up_read(&key->sem);
		key_put(key);
		return PTR_ERR(ret);
	}

	key_data = sock_kmalloc(&ask->sk, key_datalen, GFP_KERNEL);
	if (!key_data) {
		up_read(&key->sem);
		key_put(key);
		return -ENOMEM;
	}

	memcpy(key_data, ret, key_datalen);

	up_read(&key->sem);
	key_put(key);

	err = type->setkey(ask->private, key_data, key_datalen);

	sock_kzfree_s(&ask->sk, key_data, key_datalen);

	return err;
}

#else

static inline int alg_setkey_by_key_serial(struct alg_sock *ask,
					   sockptr_t optval,
					   unsigned int optlen)
{
	return -ENOPROTOOPT;
}

#endif

static int alg_setsockopt(struct socket *sock, int level, int optname,
			  sockptr_t optval, unsigned int optlen)
{
	struct sock *sk = sock->sk;
	struct alg_sock *ask = alg_sk(sk);
	const struct af_alg_type *type;
	int err = -EBUSY;

	lock_sock(sk);
	if (atomic_read(&ask->refcnt) != atomic_read(&ask->nokey_refcnt))
		goto unlock;

	type = ask->type;

	err = -ENOPROTOOPT;
	if (level != SOL_ALG || !type)
		goto unlock;

	switch (optname) {
	case ALG_SET_KEY:
	case ALG_SET_KEY_BY_KEY_SERIAL:
		if (sock->state == SS_CONNECTED)
			goto unlock;
		if (!type->setkey)
			goto unlock;

		if (optname == ALG_SET_KEY_BY_KEY_SERIAL)
			err = alg_setkey_by_key_serial(ask, optval, optlen);
		else
			err = alg_setkey(sk, optval, optlen);
		break;
	case ALG_SET_AEAD_AUTHSIZE:
		if (sock->state == SS_CONNECTED)
			goto unlock;
		if (!type->setauthsize)
			goto unlock;
		err = type->setauthsize(ask->private, optlen);
		break;
	case ALG_SET_DRBG_ENTROPY:
		if (sock->state == SS_CONNECTED)
			goto unlock;
		if (!type->setentropy)
			goto unlock;

		err = type->setentropy(ask->private, optval, optlen);
	}

unlock:
	release_sock(sk);

	return err;
}

int af_alg_accept(struct sock *sk, struct socket *newsock, bool kern)
{
	struct alg_sock *ask = alg_sk(sk);
	const struct af_alg_type *type;
	struct sock *sk2;
	unsigned int nokey;
	int err;

	lock_sock(sk);
	type = ask->type;

	err = -EINVAL;
	if (!type)
		goto unlock;

	sk2 = sk_alloc(sock_net(sk), PF_ALG, GFP_KERNEL, &alg_proto, kern);
	err = -ENOMEM;
	if (!sk2)
		goto unlock;

	sock_init_data(newsock, sk2);
	security_sock_graft(sk2, newsock);
	security_sk_clone(sk, sk2);

	/*
	 * newsock->ops assigned here to allow type->accept call to override
	 * them when required.
	 */
	newsock->ops = type->ops;
	err = type->accept(ask->private, sk2);

	nokey = err == -ENOKEY;
	if (nokey && type->accept_nokey)
		err = type->accept_nokey(ask->private, sk2);

	if (err)
		goto unlock;

	if (atomic_inc_return_relaxed(&ask->refcnt) == 1)
		sock_hold(sk);
	if (nokey) {
		atomic_inc(&ask->nokey_refcnt);
		atomic_set(&alg_sk(sk2)->nokey_refcnt, 1);
	}
	alg_sk(sk2)->parent = sk;
	alg_sk(sk2)->type = type;

	newsock->state = SS_CONNECTED;

	if (nokey)
		newsock->ops = type->ops_nokey;

	err = 0;

unlock:
	release_sock(sk);

	return err;
}
EXPORT_SYMBOL_GPL(af_alg_accept);

static int alg_accept(struct socket *sock, struct socket *newsock, int flags,
		      bool kern)
{
	return af_alg_accept(sock->sk, newsock, kern);
}

static const struct proto_ops alg_proto_ops = {
	.family		=	PF_ALG,
	.owner		=	THIS_MODULE,

	.connect	=	sock_no_connect,
	.socketpair	=	sock_no_socketpair,
	.getname	=	sock_no_getname,
	.ioctl		=	sock_no_ioctl,
	.listen		=	sock_no_listen,
	.shutdown	=	sock_no_shutdown,
	.mmap		=	sock_no_mmap,
	.sendmsg	=	sock_no_sendmsg,
	.recvmsg	=	sock_no_recvmsg,

	.bind		=	alg_bind,
	.release	=	af_alg_release,
	.setsockopt	=	alg_setsockopt,
	.accept		=	alg_accept,
};

static void alg_sock_destruct(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);

	alg_do_release(ask->type, ask->private);
}

static int alg_create(struct net *net, struct socket *sock, int protocol,
		      int kern)
{
	struct sock *sk;
	int err;

	if (sock->type != SOCK_SEQPACKET)
		return -ESOCKTNOSUPPORT;
	if (protocol != 0)
		return -EPROTONOSUPPORT;

	err = -ENOMEM;
	sk = sk_alloc(net, PF_ALG, GFP_KERNEL, &alg_proto, kern);
	if (!sk)
		goto out;

	sock->ops = &alg_proto_ops;
	sock_init_data(sock, sk);

	sk->sk_destruct = alg_sock_destruct;

	return 0;
out:
	return err;
}

static const struct net_proto_family alg_family = {
	.family	=	PF_ALG,
	.create	=	alg_create,
	.owner	=	THIS_MODULE,
};

static void af_alg_link_sg(struct af_alg_sgl *sgl_prev,
			   struct af_alg_sgl *sgl_new)
{
	sg_unmark_end(sgl_prev->sgt.sgl + sgl_prev->sgt.nents - 1);
	sg_chain(sgl_prev->sgt.sgl, sgl_prev->sgt.nents + 1, sgl_new->sgt.sgl);
}

void af_alg_free_sg(struct af_alg_sgl *sgl)
{
	int i;

	if (sgl->sgt.sgl) {
		if (sgl->need_unpin)
			for (i = 0; i < sgl->sgt.nents; i++)
				unpin_user_page(sg_page(&sgl->sgt.sgl[i]));
		if (sgl->sgt.sgl != sgl->sgl)
			kvfree(sgl->sgt.sgl);
		sgl->sgt.sgl = NULL;
	}
}
EXPORT_SYMBOL_GPL(af_alg_free_sg);

static int af_alg_cmsg_send(struct msghdr *msg, struct af_alg_control *con)
{
	struct cmsghdr *cmsg;

	for_each_cmsghdr(cmsg, msg) {
		if (!CMSG_OK(msg, cmsg))
			return -EINVAL;
		if (cmsg->cmsg_level != SOL_ALG)
			continue;

		switch (cmsg->cmsg_type) {
		case ALG_SET_IV:
			if (cmsg->cmsg_len < CMSG_LEN(sizeof(*con->iv)))
				return -EINVAL;
			con->iv = (void *)CMSG_DATA(cmsg);
			if (cmsg->cmsg_len < CMSG_LEN(con->iv->ivlen +
						      sizeof(*con->iv)))
				return -EINVAL;
			break;

		case ALG_SET_OP:
			if (cmsg->cmsg_len < CMSG_LEN(sizeof(u32)))
				return -EINVAL;
			con->op = *(u32 *)CMSG_DATA(cmsg);
			break;

		case ALG_SET_AEAD_ASSOCLEN:
			if (cmsg->cmsg_len < CMSG_LEN(sizeof(u32)))
				return -EINVAL;
			con->aead_assoclen = *(u32 *)CMSG_DATA(cmsg);
			break;

		default:
			return -EINVAL;
		}
	}

	return 0;
}

/**
 * af_alg_alloc_tsgl - allocate the TX SGL
 *
 * @sk: socket of connection to user space
 * Return: 0 upon success, < 0 upon error
 */
static int af_alg_alloc_tsgl(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	struct af_alg_ctx *ctx = ask->private;
	struct af_alg_tsgl *sgl;
	struct scatterlist *sg = NULL;

	sgl = list_entry(ctx->tsgl_list.prev, struct af_alg_tsgl, list);
	if (!list_empty(&ctx->tsgl_list))
		sg = sgl->sg;

	if (!sg || sgl->cur >= MAX_SGL_ENTS) {
		sgl = sock_kmalloc(sk,
				   struct_size(sgl, sg, (MAX_SGL_ENTS + 1)),
				   GFP_KERNEL);
		if (!sgl)
			return -ENOMEM;

		sg_init_table(sgl->sg, MAX_SGL_ENTS + 1);
		sgl->cur = 0;

		if (sg)
			sg_chain(sg, MAX_SGL_ENTS + 1, sgl->sg);

		list_add_tail(&sgl->list, &ctx->tsgl_list);
	}

	return 0;
}

/**
 * af_alg_count_tsgl - Count number of TX SG entries
 *
 * The counting starts from the beginning of the SGL to @bytes. If
 * an @offset is provided, the counting of the SG entries starts at the @offset.
 *
 * @sk: socket of connection to user space
 * @bytes: Count the number of SG entries holding given number of bytes.
 * @offset: Start the counting of SG entries from the given offset.
 * Return: Number of TX SG entries found given the constraints
 */
unsigned int af_alg_count_tsgl(struct sock *sk, size_t bytes, size_t offset)
{
	const struct alg_sock *ask = alg_sk(sk);
	const struct af_alg_ctx *ctx = ask->private;
	const struct af_alg_tsgl *sgl;
	unsigned int i;
	unsigned int sgl_count = 0;

	if (!bytes)
		return 0;

	list_for_each_entry(sgl, &ctx->tsgl_list, list) {
		const struct scatterlist *sg = sgl->sg;

		for (i = 0; i < sgl->cur; i++) {
			size_t bytes_count;

			/* Skip offset */
			if (offset >= sg[i].length) {
				offset -= sg[i].length;
				bytes -= sg[i].length;
				continue;
			}

			bytes_count = sg[i].length - offset;

			offset = 0;
			sgl_count++;

			/* If we have seen requested number of bytes, stop */
			if (bytes_count >= bytes)
				return sgl_count;

			bytes -= bytes_count;
		}
	}

	return sgl_count;
}
EXPORT_SYMBOL_GPL(af_alg_count_tsgl);

/**
 * af_alg_pull_tsgl - Release the specified buffers from TX SGL
 *
 * If @dst is non-null, reassign the pages to @dst. The caller must release
 * the pages. If @dst_offset is given only reassign the pages to @dst starting
 * at the @dst_offset (byte). The caller must ensure that @dst is large
 * enough (e.g. by using af_alg_count_tsgl with the same offset).
 *
 * @sk: socket of connection to user space
 * @used: Number of bytes to pull from TX SGL
 * @dst: If non-NULL, buffer is reassigned to dst SGL instead of releasing. The
 *	 caller must release the buffers in dst.
 * @dst_offset: Reassign the TX SGL from given offset. All buffers before
 *	        reaching the offset is released.
 */
void af_alg_pull_tsgl(struct sock *sk, size_t used, struct scatterlist *dst,
		      size_t dst_offset)
{
	struct alg_sock *ask = alg_sk(sk);
	struct af_alg_ctx *ctx = ask->private;
	struct af_alg_tsgl *sgl;
	struct scatterlist *sg;
	unsigned int i, j = 0;

	while (!list_empty(&ctx->tsgl_list)) {
		sgl = list_first_entry(&ctx->tsgl_list, struct af_alg_tsgl,
				       list);
		sg = sgl->sg;

		for (i = 0; i < sgl->cur; i++) {
			size_t plen = min_t(size_t, used, sg[i].length);
			struct page *page = sg_page(sg + i);

			if (!page)
				continue;

			/*
			 * Assumption: caller created af_alg_count_tsgl(len)
			 * SG entries in dst.
			 */
			if (dst) {
				if (dst_offset >= plen) {
					/* discard page before offset */
					dst_offset -= plen;
				} else {
					/* reassign page to dst after offset */
					get_page(page);
					sg_set_page(dst + j, page,
						    plen - dst_offset,
						    sg[i].offset + dst_offset);
					dst_offset = 0;
					j++;
				}
			}

			sg[i].length -= plen;
			sg[i].offset += plen;

			used -= plen;
			ctx->used -= plen;

			if (sg[i].length)
				return;

			put_page(page);
			sg_assign_page(sg + i, NULL);
		}

		list_del(&sgl->list);
		sock_kfree_s(sk, sgl, struct_size(sgl, sg, MAX_SGL_ENTS + 1));
	}

	if (!ctx->used)
		ctx->merge = 0;
	ctx->init = ctx->more;
}
EXPORT_SYMBOL_GPL(af_alg_pull_tsgl);

/**
 * af_alg_free_areq_sgls - Release TX and RX SGLs of the request
 *
 * @areq: Request holding the TX and RX SGL
 */
static void af_alg_free_areq_sgls(struct af_alg_async_req *areq)
{
	struct sock *sk = areq->sk;
	struct alg_sock *ask = alg_sk(sk);
	struct af_alg_ctx *ctx = ask->private;
	struct af_alg_rsgl *rsgl, *tmp;
	struct scatterlist *tsgl;
	struct scatterlist *sg;
	unsigned int i;

	list_for_each_entry_safe(rsgl, tmp, &areq->rsgl_list, list) {
		atomic_sub(rsgl->sg_num_bytes, &ctx->rcvused);
		af_alg_free_sg(&rsgl->sgl);
		list_del(&rsgl->list);
		if (rsgl != &areq->first_rsgl)
			sock_kfree_s(sk, rsgl, sizeof(*rsgl));
	}

	tsgl = areq->tsgl;
	if (tsgl) {
		for_each_sg(tsgl, sg, areq->tsgl_entries, i) {
			if (!sg_page(sg))
				continue;
			put_page(sg_page(sg));
		}

		sock_kfree_s(sk, tsgl, areq->tsgl_entries * sizeof(*tsgl));
	}
}

/**
 * af_alg_wait_for_wmem - wait for availability of writable memory
 *
 * @sk: socket of connection to user space
 * @flags: If MSG_DONTWAIT is set, then only report if function would sleep
 * Return: 0 when writable memory is available, < 0 upon error
 */
static int af_alg_wait_for_wmem(struct sock *sk, unsigned int flags)
{
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	int err = -ERESTARTSYS;
	long timeout;

	if (flags & MSG_DONTWAIT)
		return -EAGAIN;

	sk_set_bit(SOCKWQ_ASYNC_NOSPACE, sk);

	add_wait_queue(sk_sleep(sk), &wait);
	for (;;) {
		if (signal_pending(current))
			break;
		timeout = MAX_SCHEDULE_TIMEOUT;
		if (sk_wait_event(sk, &timeout, af_alg_writable(sk), &wait)) {
			err = 0;
			break;
		}
	}
	remove_wait_queue(sk_sleep(sk), &wait);

	return err;
}

/**
 * af_alg_wmem_wakeup - wakeup caller when writable memory is available
 *
 * @sk: socket of connection to user space
 */
void af_alg_wmem_wakeup(struct sock *sk)
{
	struct socket_wq *wq;

	if (!af_alg_writable(sk))
		return;

	rcu_read_lock();
	wq = rcu_dereference(sk->sk_wq);
	if (skwq_has_sleeper(wq))
		wake_up_interruptible_sync_poll(&wq->wait, EPOLLIN |
							   EPOLLRDNORM |
							   EPOLLRDBAND);
	sk_wake_async(sk, SOCK_WAKE_WAITD, POLL_IN);
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(af_alg_wmem_wakeup);

/**
 * af_alg_wait_for_data - wait for availability of TX data
 *
 * @sk: socket of connection to user space
 * @flags: If MSG_DONTWAIT is set, then only report if function would sleep
 * @min: Set to minimum request size if partial requests are allowed.
 * Return: 0 when writable memory is available, < 0 upon error
 */
int af_alg_wait_for_data(struct sock *sk, unsigned flags, unsigned min)
{
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	struct alg_sock *ask = alg_sk(sk);
	struct af_alg_ctx *ctx = ask->private;
	long timeout;
	int err = -ERESTARTSYS;

	if (flags & MSG_DONTWAIT)
		return -EAGAIN;

	sk_set_bit(SOCKWQ_ASYNC_WAITDATA, sk);

	add_wait_queue(sk_sleep(sk), &wait);
	for (;;) {
		if (signal_pending(current))
			break;
		timeout = MAX_SCHEDULE_TIMEOUT;
		if (sk_wait_event(sk, &timeout,
				  ctx->init && (!ctx->more ||
						(min && ctx->used >= min)),
				  &wait)) {
			err = 0;
			break;
		}
	}
	remove_wait_queue(sk_sleep(sk), &wait);

	sk_clear_bit(SOCKWQ_ASYNC_WAITDATA, sk);

	return err;
}
EXPORT_SYMBOL_GPL(af_alg_wait_for_data);

/**
 * af_alg_data_wakeup - wakeup caller when new data can be sent to kernel
 *
 * @sk: socket of connection to user space
 */
static void af_alg_data_wakeup(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	struct af_alg_ctx *ctx = ask->private;
	struct socket_wq *wq;

	if (!ctx->used)
		return;

	rcu_read_lock();
	wq = rcu_dereference(sk->sk_wq);
	if (skwq_has_sleeper(wq))
		wake_up_interruptible_sync_poll(&wq->wait, EPOLLOUT |
							   EPOLLRDNORM |
							   EPOLLRDBAND);
	sk_wake_async(sk, SOCK_WAKE_SPACE, POLL_OUT);
	rcu_read_unlock();
}

/**
 * af_alg_sendmsg - implementation of sendmsg system call handler
 *
 * The sendmsg system call handler obtains the user data and stores it
 * in ctx->tsgl_list. This implies allocation of the required numbers of
 * struct af_alg_tsgl.
 *
 * In addition, the ctx is filled with the information sent via CMSG.
 *
 * @sock: socket of connection to user space
 * @msg: message from user space
 * @size: size of message from user space
 * @ivsize: the size of the IV for the cipher operation to verify that the
 *	   user-space-provided IV has the right size
 * Return: the number of copied data upon success, < 0 upon error
 */
int af_alg_sendmsg(struct socket *sock, struct msghdr *msg, size_t size,
		   unsigned int ivsize)
{
	struct sock *sk = sock->sk;
	struct alg_sock *ask = alg_sk(sk);
	struct af_alg_ctx *ctx = ask->private;
	struct af_alg_tsgl *sgl;
	struct af_alg_control con = {};
	long copied = 0;
	bool enc = false;
	bool init = false;
	int err = 0;

	if (msg->msg_controllen) {
		err = af_alg_cmsg_send(msg, &con);
		if (err)
			return err;

		init = true;
		switch (con.op) {
		case ALG_OP_ENCRYPT:
			enc = true;
			break;
		case ALG_OP_DECRYPT:
			enc = false;
			break;
		default:
			return -EINVAL;
		}

		if (con.iv && con.iv->ivlen != ivsize)
			return -EINVAL;
	}

	lock_sock(sk);
	if (ctx->init && !ctx->more) {
		if (ctx->used) {
			err = -EINVAL;
			goto unlock;
		}

		pr_info_once(
			"%s sent an empty control message without MSG_MORE.\n",
			current->comm);
	}
	ctx->init = true;

	if (init) {
		ctx->enc = enc;
		if (con.iv)
			memcpy(ctx->iv, con.iv->iv, ivsize);

		ctx->aead_assoclen = con.aead_assoclen;
	}

	while (size) {
		struct scatterlist *sg;
		size_t len = size;
		ssize_t plen;

		/* use the existing memory in an allocated page */
		if (ctx->merge && !(msg->msg_flags & MSG_SPLICE_PAGES)) {
			sgl = list_entry(ctx->tsgl_list.prev,
					 struct af_alg_tsgl, list);
			sg = sgl->sg + sgl->cur - 1;
			len = min_t(size_t, len,
				    PAGE_SIZE - sg->offset - sg->length);

			err = memcpy_from_msg(page_address(sg_page(sg)) +
					      sg->offset + sg->length,
					      msg, len);
			if (err)
				goto unlock;

			sg->length += len;
			ctx->merge = (sg->offset + sg->length) &
				     (PAGE_SIZE - 1);

			ctx->used += len;
			copied += len;
			size -= len;
			continue;
		}

		if (!af_alg_writable(sk)) {
			err = af_alg_wait_for_wmem(sk, msg->msg_flags);
			if (err)
				goto unlock;
		}

		/* allocate a new page */
		len = min_t(unsigned long, len, af_alg_sndbuf(sk));

		err = af_alg_alloc_tsgl(sk);
		if (err)
			goto unlock;

		sgl = list_entry(ctx->tsgl_list.prev, struct af_alg_tsgl,
				 list);
		sg = sgl->sg;
		if (sgl->cur)
			sg_unmark_end(sg + sgl->cur - 1);

		if (msg->msg_flags & MSG_SPLICE_PAGES) {
			struct sg_table sgtable = {
				.sgl		= sg,
				.nents		= sgl->cur,
				.orig_nents	= sgl->cur,
			};

			plen = extract_iter_to_sg(&msg->msg_iter, len, &sgtable,
						  MAX_SGL_ENTS - sgl->cur, 0);
			if (plen < 0) {
				err = plen;
				goto unlock;
			}

			for (; sgl->cur < sgtable.nents; sgl->cur++)
				get_page(sg_page(&sg[sgl->cur]));
			len -= plen;
			ctx->used += plen;
			copied += plen;
			size -= plen;
			ctx->merge = 0;
		} else {
			do {
				struct page *pg;
				unsigned int i = sgl->cur;

				plen = min_t(size_t, len, PAGE_SIZE);

				pg = alloc_page(GFP_KERNEL);
				if (!pg) {
					err = -ENOMEM;
					goto unlock;
				}

				sg_assign_page(sg + i, pg);

				err = memcpy_from_msg(
					page_address(sg_page(sg + i)),
					msg, plen);
				if (err) {
					__free_page(sg_page(sg + i));
					sg_assign_page(sg + i, NULL);
					goto unlock;
				}

				sg[i].length = plen;
				len -= plen;
				ctx->used += plen;
				copied += plen;
				size -= plen;
				sgl->cur++;
			} while (len && sgl->cur < MAX_SGL_ENTS);

			ctx->merge = plen & (PAGE_SIZE - 1);
		}

		if (!size)
			sg_mark_end(sg + sgl->cur - 1);
	}

	err = 0;

	ctx->more = msg->msg_flags & MSG_MORE;

unlock:
	af_alg_data_wakeup(sk);
	release_sock(sk);

	return copied ?: err;
}
EXPORT_SYMBOL_GPL(af_alg_sendmsg);

/**
 * af_alg_free_resources - release resources required for crypto request
 * @areq: Request holding the TX and RX SGL
 */
void af_alg_free_resources(struct af_alg_async_req *areq)
{
	struct sock *sk = areq->sk;

	af_alg_free_areq_sgls(areq);
	sock_kfree_s(sk, areq, areq->areqlen);
}
EXPORT_SYMBOL_GPL(af_alg_free_resources);

/**
 * af_alg_async_cb - AIO callback handler
 * @data: async request completion data
 * @err: if non-zero, error result to be returned via ki_complete();
 *       otherwise return the AIO output length via ki_complete().
 *
 * This handler cleans up the struct af_alg_async_req upon completion of the
 * AIO operation.
 *
 * The number of bytes to be generated with the AIO operation must be set
 * in areq->outlen before the AIO callback handler is invoked.
 */
void af_alg_async_cb(void *data, int err)
{
	struct af_alg_async_req *areq = data;
	struct sock *sk = areq->sk;
	struct kiocb *iocb = areq->iocb;
	unsigned int resultlen;

	/* Buffer size written by crypto operation. */
	resultlen = areq->outlen;

	af_alg_free_resources(areq);
	sock_put(sk);

	iocb->ki_complete(iocb, err ? err : (int)resultlen);
}
EXPORT_SYMBOL_GPL(af_alg_async_cb);

/**
 * af_alg_poll - poll system call handler
 * @file: file pointer
 * @sock: socket to poll
 * @wait: poll_table
 */
__poll_t af_alg_poll(struct file *file, struct socket *sock,
			 poll_table *wait)
{
	struct sock *sk = sock->sk;
	struct alg_sock *ask = alg_sk(sk);
	struct af_alg_ctx *ctx = ask->private;
	__poll_t mask;

	sock_poll_wait(file, sock, wait);
	mask = 0;

	if (!ctx->more || ctx->used)
		mask |= EPOLLIN | EPOLLRDNORM;

	if (af_alg_writable(sk))
		mask |= EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND;

	return mask;
}
EXPORT_SYMBOL_GPL(af_alg_poll);

/**
 * af_alg_alloc_areq - allocate struct af_alg_async_req
 *
 * @sk: socket of connection to user space
 * @areqlen: size of struct af_alg_async_req + crypto_*_reqsize
 * Return: allocated data structure or ERR_PTR upon error
 */
struct af_alg_async_req *af_alg_alloc_areq(struct sock *sk,
					   unsigned int areqlen)
{
	struct af_alg_async_req *areq = sock_kmalloc(sk, areqlen, GFP_KERNEL);

	if (unlikely(!areq))
		return ERR_PTR(-ENOMEM);

	areq->areqlen = areqlen;
	areq->sk = sk;
	areq->first_rsgl.sgl.sgt.sgl = areq->first_rsgl.sgl.sgl;
	areq->last_rsgl = NULL;
	INIT_LIST_HEAD(&areq->rsgl_list);
	areq->tsgl = NULL;
	areq->tsgl_entries = 0;

	return areq;
}
EXPORT_SYMBOL_GPL(af_alg_alloc_areq);

/**
 * af_alg_get_rsgl - create the RX SGL for the output data from the crypto
 *		     operation
 *
 * @sk: socket of connection to user space
 * @msg: user space message
 * @flags: flags used to invoke recvmsg with
 * @areq: instance of the cryptographic request that will hold the RX SGL
 * @maxsize: maximum number of bytes to be pulled from user space
 * @outlen: number of bytes in the RX SGL
 * Return: 0 on success, < 0 upon error
 */
int af_alg_get_rsgl(struct sock *sk, struct msghdr *msg, int flags,
		    struct af_alg_async_req *areq, size_t maxsize,
		    size_t *outlen)
{
	struct alg_sock *ask = alg_sk(sk);
	struct af_alg_ctx *ctx = ask->private;
	size_t len = 0;

	while (maxsize > len && msg_data_left(msg)) {
		struct af_alg_rsgl *rsgl;
		ssize_t err;
		size_t seglen;

		/* limit the amount of readable buffers */
		if (!af_alg_readable(sk))
			break;

		seglen = min_t(size_t, (maxsize - len),
			       msg_data_left(msg));

		if (list_empty(&areq->rsgl_list)) {
			rsgl = &areq->first_rsgl;
		} else {
			rsgl = sock_kmalloc(sk, sizeof(*rsgl), GFP_KERNEL);
			if (unlikely(!rsgl))
				return -ENOMEM;
		}

		rsgl->sgl.need_unpin =
			iov_iter_extract_will_pin(&msg->msg_iter);
		rsgl->sgl.sgt.sgl = rsgl->sgl.sgl;
		rsgl->sgl.sgt.nents = 0;
		rsgl->sgl.sgt.orig_nents = 0;
		list_add_tail(&rsgl->list, &areq->rsgl_list);

		sg_init_table(rsgl->sgl.sgt.sgl, ALG_MAX_PAGES);
		err = extract_iter_to_sg(&msg->msg_iter, seglen, &rsgl->sgl.sgt,
					 ALG_MAX_PAGES, 0);
		if (err < 0) {
			rsgl->sg_num_bytes = 0;
			return err;
		}

		sg_mark_end(rsgl->sgl.sgt.sgl + rsgl->sgl.sgt.nents - 1);

		/* chain the new scatterlist with previous one */
		if (areq->last_rsgl)
			af_alg_link_sg(&areq->last_rsgl->sgl, &rsgl->sgl);

		areq->last_rsgl = rsgl;
		len += err;
		atomic_add(err, &ctx->rcvused);
		rsgl->sg_num_bytes = err;
	}

	*outlen = len;
	return 0;
}
EXPORT_SYMBOL_GPL(af_alg_get_rsgl);

static int __init af_alg_init(void)
{
	int err = proto_register(&alg_proto, 0);

	if (err)
		goto out;

	err = sock_register(&alg_family);
	if (err != 0)
		goto out_unregister_proto;

out:
	return err;

out_unregister_proto:
	proto_unregister(&alg_proto);
	goto out;
}

static void __exit af_alg_exit(void)
{
	sock_unregister(PF_ALG);
	proto_unregister(&alg_proto);
}

module_init(af_alg_init);
module_exit(af_alg_exit);
MODULE_LICENSE("GPL");
MODULE_ALIAS_NETPROTO(AF_ALG);
