// SPDX-License-Identifier: GPL-2.0
/*
 * BlueZ - Bluetooth protocol stack for Linux
 *
 * Copyright (C) 2022 Intel Corporation
 * Copyright 2023 NXP
 */

#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/sched/signal.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#include <net/bluetooth/iso.h>

static const struct proto_ops iso_sock_ops;

static struct bt_sock_list iso_sk_list = {
	.lock = __RW_LOCK_UNLOCKED(iso_sk_list.lock)
};

/* ---- ISO connections ---- */
struct iso_conn {
	struct hci_conn	*hcon;

	/* @lock: spinlock protecting changes to iso_conn fields */
	spinlock_t	lock;
	struct sock	*sk;

	struct delayed_work	timeout_work;

	struct sk_buff	*rx_skb;
	__u32		rx_len;
	__u16		tx_sn;
};

#define iso_conn_lock(c)	spin_lock(&(c)->lock)
#define iso_conn_unlock(c)	spin_unlock(&(c)->lock)

static void iso_sock_close(struct sock *sk);
static void iso_sock_kill(struct sock *sk);

/* ----- ISO socket info ----- */
#define iso_pi(sk) ((struct iso_pinfo *)sk)

#define EIR_SERVICE_DATA_LENGTH 4
#define BASE_MAX_LENGTH (HCI_MAX_PER_AD_LENGTH - EIR_SERVICE_DATA_LENGTH)

/* iso_pinfo flags values */
enum {
	BT_SK_BIG_SYNC,
	BT_SK_PA_SYNC,
};

struct iso_pinfo {
	struct bt_sock		bt;
	bdaddr_t		src;
	__u8			src_type;
	bdaddr_t		dst;
	__u8			dst_type;
	__u8			bc_sid;
	__u8			bc_num_bis;
	__u8			bc_bis[ISO_MAX_NUM_BIS];
	__u16			sync_handle;
	unsigned long		flags;
	struct bt_iso_qos	qos;
	bool			qos_user_set;
	__u8			base_len;
	__u8			base[BASE_MAX_LENGTH];
	struct iso_conn		*conn;
};

static struct bt_iso_qos default_qos;

static bool check_ucast_qos(struct bt_iso_qos *qos);
static bool check_bcast_qos(struct bt_iso_qos *qos);
static bool iso_match_sid(struct sock *sk, void *data);
static void iso_sock_disconn(struct sock *sk);

/* ---- ISO timers ---- */
#define ISO_CONN_TIMEOUT	(HZ * 40)
#define ISO_DISCONN_TIMEOUT	(HZ * 2)

static void iso_sock_timeout(struct work_struct *work)
{
	struct iso_conn *conn = container_of(work, struct iso_conn,
					     timeout_work.work);
	struct sock *sk;

	iso_conn_lock(conn);
	sk = conn->sk;
	if (sk)
		sock_hold(sk);
	iso_conn_unlock(conn);

	if (!sk)
		return;

	BT_DBG("sock %p state %d", sk, sk->sk_state);

	lock_sock(sk);
	sk->sk_err = ETIMEDOUT;
	sk->sk_state_change(sk);
	release_sock(sk);
	sock_put(sk);
}

static void iso_sock_set_timer(struct sock *sk, long timeout)
{
	if (!iso_pi(sk)->conn)
		return;

	BT_DBG("sock %p state %d timeout %ld", sk, sk->sk_state, timeout);
	cancel_delayed_work(&iso_pi(sk)->conn->timeout_work);
	schedule_delayed_work(&iso_pi(sk)->conn->timeout_work, timeout);
}

static void iso_sock_clear_timer(struct sock *sk)
{
	if (!iso_pi(sk)->conn)
		return;

	BT_DBG("sock %p state %d", sk, sk->sk_state);
	cancel_delayed_work(&iso_pi(sk)->conn->timeout_work);
}

/* ---- ISO connections ---- */
static struct iso_conn *iso_conn_add(struct hci_conn *hcon)
{
	struct iso_conn *conn = hcon->iso_data;

	if (conn) {
		if (!conn->hcon)
			conn->hcon = hcon;
		return conn;
	}

	conn = kzalloc(sizeof(*conn), GFP_KERNEL);
	if (!conn)
		return NULL;

	spin_lock_init(&conn->lock);
	INIT_DELAYED_WORK(&conn->timeout_work, iso_sock_timeout);

	hcon->iso_data = conn;
	conn->hcon = hcon;
	conn->tx_sn = 0;

	BT_DBG("hcon %p conn %p", hcon, conn);

	return conn;
}

/* Delete channel. Must be called on the locked socket. */
static void iso_chan_del(struct sock *sk, int err)
{
	struct iso_conn *conn;
	struct sock *parent;

	conn = iso_pi(sk)->conn;

	BT_DBG("sk %p, conn %p, err %d", sk, conn, err);

	if (conn) {
		iso_conn_lock(conn);
		conn->sk = NULL;
		iso_pi(sk)->conn = NULL;
		iso_conn_unlock(conn);

		if (conn->hcon)
			hci_conn_drop(conn->hcon);
	}

	sk->sk_state = BT_CLOSED;
	sk->sk_err   = err;

	parent = bt_sk(sk)->parent;
	if (parent) {
		bt_accept_unlink(sk);
		parent->sk_data_ready(parent);
	} else {
		sk->sk_state_change(sk);
	}

	sock_set_flag(sk, SOCK_ZAPPED);
}

static void iso_conn_del(struct hci_conn *hcon, int err)
{
	struct iso_conn *conn = hcon->iso_data;
	struct sock *sk;

	if (!conn)
		return;

	BT_DBG("hcon %p conn %p, err %d", hcon, conn, err);

	/* Kill socket */
	iso_conn_lock(conn);
	sk = conn->sk;
	if (sk)
		sock_hold(sk);
	iso_conn_unlock(conn);

	if (sk) {
		lock_sock(sk);
		iso_sock_clear_timer(sk);
		iso_chan_del(sk, err);
		release_sock(sk);
		sock_put(sk);
	}

	/* Ensure no more work items will run before freeing conn. */
	cancel_delayed_work_sync(&conn->timeout_work);

	hcon->iso_data = NULL;
	kfree(conn);
}

static int __iso_chan_add(struct iso_conn *conn, struct sock *sk,
			  struct sock *parent)
{
	BT_DBG("conn %p", conn);

	if (iso_pi(sk)->conn == conn && conn->sk == sk)
		return 0;

	if (conn->sk) {
		BT_ERR("conn->sk already set");
		return -EBUSY;
	}

	iso_pi(sk)->conn = conn;
	conn->sk = sk;

	if (parent)
		bt_accept_enqueue(parent, sk, true);

	return 0;
}

static int iso_chan_add(struct iso_conn *conn, struct sock *sk,
			struct sock *parent)
{
	int err;

	iso_conn_lock(conn);
	err = __iso_chan_add(conn, sk, parent);
	iso_conn_unlock(conn);

	return err;
}

static inline u8 le_addr_type(u8 bdaddr_type)
{
	if (bdaddr_type == BDADDR_LE_PUBLIC)
		return ADDR_LE_DEV_PUBLIC;
	else
		return ADDR_LE_DEV_RANDOM;
}

static int iso_connect_bis(struct sock *sk)
{
	struct iso_conn *conn;
	struct hci_conn *hcon;
	struct hci_dev  *hdev;
	int err;

	BT_DBG("%pMR", &iso_pi(sk)->src);

	hdev = hci_get_route(&iso_pi(sk)->dst, &iso_pi(sk)->src,
			     iso_pi(sk)->src_type);
	if (!hdev)
		return -EHOSTUNREACH;

	hci_dev_lock(hdev);

	if (!bis_capable(hdev)) {
		err = -EOPNOTSUPP;
		goto unlock;
	}

	/* Fail if user set invalid QoS */
	if (iso_pi(sk)->qos_user_set && !check_bcast_qos(&iso_pi(sk)->qos)) {
		iso_pi(sk)->qos = default_qos;
		err = -EINVAL;
		goto unlock;
	}

	/* Fail if out PHYs are marked as disabled */
	if (!iso_pi(sk)->qos.bcast.out.phy) {
		err = -EINVAL;
		goto unlock;
	}

	/* Just bind if DEFER_SETUP has been set */
	if (test_bit(BT_SK_DEFER_SETUP, &bt_sk(sk)->flags)) {
		hcon = hci_bind_bis(hdev, &iso_pi(sk)->dst,
				    &iso_pi(sk)->qos, iso_pi(sk)->base_len,
				    iso_pi(sk)->base);
		if (IS_ERR(hcon)) {
			err = PTR_ERR(hcon);
			goto unlock;
		}
	} else {
		hcon = hci_connect_bis(hdev, &iso_pi(sk)->dst,
				       le_addr_type(iso_pi(sk)->dst_type),
				       &iso_pi(sk)->qos, iso_pi(sk)->base_len,
				       iso_pi(sk)->base);
		if (IS_ERR(hcon)) {
			err = PTR_ERR(hcon);
			goto unlock;
		}
	}

	conn = iso_conn_add(hcon);
	if (!conn) {
		hci_conn_drop(hcon);
		err = -ENOMEM;
		goto unlock;
	}

	lock_sock(sk);

	err = iso_chan_add(conn, sk, NULL);
	if (err) {
		release_sock(sk);
		goto unlock;
	}

	/* Update source addr of the socket */
	bacpy(&iso_pi(sk)->src, &hcon->src);

	if (hcon->state == BT_CONNECTED) {
		iso_sock_clear_timer(sk);
		sk->sk_state = BT_CONNECTED;
	} else if (test_bit(BT_SK_DEFER_SETUP, &bt_sk(sk)->flags)) {
		iso_sock_clear_timer(sk);
		sk->sk_state = BT_CONNECT;
	} else {
		sk->sk_state = BT_CONNECT;
		iso_sock_set_timer(sk, sk->sk_sndtimeo);
	}

	release_sock(sk);

unlock:
	hci_dev_unlock(hdev);
	hci_dev_put(hdev);
	return err;
}

static int iso_connect_cis(struct sock *sk)
{
	struct iso_conn *conn;
	struct hci_conn *hcon;
	struct hci_dev  *hdev;
	int err;

	BT_DBG("%pMR -> %pMR", &iso_pi(sk)->src, &iso_pi(sk)->dst);

	hdev = hci_get_route(&iso_pi(sk)->dst, &iso_pi(sk)->src,
			     iso_pi(sk)->src_type);
	if (!hdev)
		return -EHOSTUNREACH;

	hci_dev_lock(hdev);

	if (!cis_central_capable(hdev)) {
		err = -EOPNOTSUPP;
		goto unlock;
	}

	/* Fail if user set invalid QoS */
	if (iso_pi(sk)->qos_user_set && !check_ucast_qos(&iso_pi(sk)->qos)) {
		iso_pi(sk)->qos = default_qos;
		err = -EINVAL;
		goto unlock;
	}

	/* Fail if either PHYs are marked as disabled */
	if (!iso_pi(sk)->qos.ucast.in.phy && !iso_pi(sk)->qos.ucast.out.phy) {
		err = -EINVAL;
		goto unlock;
	}

	/* Just bind if DEFER_SETUP has been set */
	if (test_bit(BT_SK_DEFER_SETUP, &bt_sk(sk)->flags)) {
		hcon = hci_bind_cis(hdev, &iso_pi(sk)->dst,
				    le_addr_type(iso_pi(sk)->dst_type),
				    &iso_pi(sk)->qos);
		if (IS_ERR(hcon)) {
			err = PTR_ERR(hcon);
			goto unlock;
		}
	} else {
		hcon = hci_connect_cis(hdev, &iso_pi(sk)->dst,
				       le_addr_type(iso_pi(sk)->dst_type),
				       &iso_pi(sk)->qos);
		if (IS_ERR(hcon)) {
			err = PTR_ERR(hcon);
			goto unlock;
		}
	}

	conn = iso_conn_add(hcon);
	if (!conn) {
		hci_conn_drop(hcon);
		err = -ENOMEM;
		goto unlock;
	}

	lock_sock(sk);

	err = iso_chan_add(conn, sk, NULL);
	if (err) {
		release_sock(sk);
		goto unlock;
	}

	/* Update source addr of the socket */
	bacpy(&iso_pi(sk)->src, &hcon->src);

	if (hcon->state == BT_CONNECTED) {
		iso_sock_clear_timer(sk);
		sk->sk_state = BT_CONNECTED;
	} else if (test_bit(BT_SK_DEFER_SETUP, &bt_sk(sk)->flags)) {
		iso_sock_clear_timer(sk);
		sk->sk_state = BT_CONNECT;
	} else {
		sk->sk_state = BT_CONNECT;
		iso_sock_set_timer(sk, sk->sk_sndtimeo);
	}

	release_sock(sk);

unlock:
	hci_dev_unlock(hdev);
	hci_dev_put(hdev);
	return err;
}

static struct bt_iso_qos *iso_sock_get_qos(struct sock *sk)
{
	if (sk->sk_state == BT_CONNECTED || sk->sk_state == BT_CONNECT2)
		return &iso_pi(sk)->conn->hcon->iso_qos;

	return &iso_pi(sk)->qos;
}

static int iso_send_frame(struct sock *sk, struct sk_buff *skb)
{
	struct iso_conn *conn = iso_pi(sk)->conn;
	struct bt_iso_qos *qos = iso_sock_get_qos(sk);
	struct hci_iso_data_hdr *hdr;
	int len = 0;

	BT_DBG("sk %p len %d", sk, skb->len);

	if (skb->len > qos->ucast.out.sdu)
		return -EMSGSIZE;

	len = skb->len;

	/* Push ISO data header */
	hdr = skb_push(skb, HCI_ISO_DATA_HDR_SIZE);
	hdr->sn = cpu_to_le16(conn->tx_sn++);
	hdr->slen = cpu_to_le16(hci_iso_data_len_pack(len,
						      HCI_ISO_STATUS_VALID));

	if (sk->sk_state == BT_CONNECTED)
		hci_send_iso(conn->hcon, skb);
	else
		len = -ENOTCONN;

	return len;
}

static void iso_recv_frame(struct iso_conn *conn, struct sk_buff *skb)
{
	struct sock *sk;

	iso_conn_lock(conn);
	sk = conn->sk;
	iso_conn_unlock(conn);

	if (!sk)
		goto drop;

	BT_DBG("sk %p len %d", sk, skb->len);

	if (sk->sk_state != BT_CONNECTED)
		goto drop;

	if (!sock_queue_rcv_skb(sk, skb))
		return;

drop:
	kfree_skb(skb);
}

/* -------- Socket interface ---------- */
static struct sock *__iso_get_sock_listen_by_addr(bdaddr_t *ba)
{
	struct sock *sk;

	sk_for_each(sk, &iso_sk_list.head) {
		if (sk->sk_state != BT_LISTEN)
			continue;

		if (!bacmp(&iso_pi(sk)->src, ba))
			return sk;
	}

	return NULL;
}

static struct sock *__iso_get_sock_listen_by_sid(bdaddr_t *ba, bdaddr_t *bc,
						 __u8 sid)
{
	struct sock *sk;

	sk_for_each(sk, &iso_sk_list.head) {
		if (sk->sk_state != BT_LISTEN)
			continue;

		if (bacmp(&iso_pi(sk)->src, ba))
			continue;

		if (bacmp(&iso_pi(sk)->dst, bc))
			continue;

		if (iso_pi(sk)->bc_sid == sid)
			return sk;
	}

	return NULL;
}

typedef bool (*iso_sock_match_t)(struct sock *sk, void *data);

/* Find socket listening:
 * source bdaddr (Unicast)
 * destination bdaddr (Broadcast only)
 * match func - pass NULL to ignore
 * match func data - pass -1 to ignore
 * Returns closest match.
 */
static struct sock *iso_get_sock_listen(bdaddr_t *src, bdaddr_t *dst,
					iso_sock_match_t match, void *data)
{
	struct sock *sk = NULL, *sk1 = NULL;

	read_lock(&iso_sk_list.lock);

	sk_for_each(sk, &iso_sk_list.head) {
		if (sk->sk_state != BT_LISTEN)
			continue;

		/* Match Broadcast destination */
		if (bacmp(dst, BDADDR_ANY) && bacmp(&iso_pi(sk)->dst, dst))
			continue;

		/* Use Match function if provided */
		if (match && !match(sk, data))
			continue;

		/* Exact match. */
		if (!bacmp(&iso_pi(sk)->src, src))
			break;

		/* Closest match */
		if (!bacmp(&iso_pi(sk)->src, BDADDR_ANY))
			sk1 = sk;
	}

	read_unlock(&iso_sk_list.lock);

	return sk ? sk : sk1;
}

static void iso_sock_destruct(struct sock *sk)
{
	BT_DBG("sk %p", sk);

	skb_queue_purge(&sk->sk_receive_queue);
	skb_queue_purge(&sk->sk_write_queue);
}

static void iso_sock_cleanup_listen(struct sock *parent)
{
	struct sock *sk;

	BT_DBG("parent %p", parent);

	/* Close not yet accepted channels */
	while ((sk = bt_accept_dequeue(parent, NULL))) {
		iso_sock_close(sk);
		iso_sock_kill(sk);
	}

	/* If listening socket stands for a PA sync connection,
	 * properly disconnect the hcon and socket.
	 */
	if (iso_pi(parent)->conn && iso_pi(parent)->conn->hcon &&
	    test_bit(HCI_CONN_PA_SYNC, &iso_pi(parent)->conn->hcon->flags)) {
		iso_sock_disconn(parent);
		return;
	}

	parent->sk_state  = BT_CLOSED;
	sock_set_flag(parent, SOCK_ZAPPED);
}

/* Kill socket (only if zapped and orphan)
 * Must be called on unlocked socket.
 */
static void iso_sock_kill(struct sock *sk)
{
	if (!sock_flag(sk, SOCK_ZAPPED) || sk->sk_socket ||
	    sock_flag(sk, SOCK_DEAD))
		return;

	BT_DBG("sk %p state %d", sk, sk->sk_state);

	/* Kill poor orphan */
	bt_sock_unlink(&iso_sk_list, sk);
	sock_set_flag(sk, SOCK_DEAD);
	sock_put(sk);
}

static void iso_sock_disconn(struct sock *sk)
{
	sk->sk_state = BT_DISCONN;
	iso_sock_set_timer(sk, ISO_DISCONN_TIMEOUT);
	iso_conn_lock(iso_pi(sk)->conn);
	hci_conn_drop(iso_pi(sk)->conn->hcon);
	iso_pi(sk)->conn->hcon = NULL;
	iso_conn_unlock(iso_pi(sk)->conn);
}

static void __iso_sock_close(struct sock *sk)
{
	BT_DBG("sk %p state %d socket %p", sk, sk->sk_state, sk->sk_socket);

	switch (sk->sk_state) {
	case BT_LISTEN:
		iso_sock_cleanup_listen(sk);
		break;

	case BT_CONNECT:
	case BT_CONNECTED:
	case BT_CONFIG:
		if (iso_pi(sk)->conn->hcon)
			iso_sock_disconn(sk);
		else
			iso_chan_del(sk, ECONNRESET);
		break;

	case BT_CONNECT2:
		if (iso_pi(sk)->conn->hcon &&
		    (test_bit(HCI_CONN_PA_SYNC, &iso_pi(sk)->conn->hcon->flags) ||
		    test_bit(HCI_CONN_PA_SYNC_FAILED, &iso_pi(sk)->conn->hcon->flags)))
			iso_sock_disconn(sk);
		else
			iso_chan_del(sk, ECONNRESET);
		break;
	case BT_DISCONN:
		iso_chan_del(sk, ECONNRESET);
		break;

	default:
		sock_set_flag(sk, SOCK_ZAPPED);
		break;
	}
}

/* Must be called on unlocked socket. */
static void iso_sock_close(struct sock *sk)
{
	iso_sock_clear_timer(sk);
	lock_sock(sk);
	__iso_sock_close(sk);
	release_sock(sk);
	iso_sock_kill(sk);
}

static void iso_sock_init(struct sock *sk, struct sock *parent)
{
	BT_DBG("sk %p", sk);

	if (parent) {
		sk->sk_type = parent->sk_type;
		bt_sk(sk)->flags = bt_sk(parent)->flags;
		security_sk_clone(parent, sk);
	}
}

static struct proto iso_proto = {
	.name		= "ISO",
	.owner		= THIS_MODULE,
	.obj_size	= sizeof(struct iso_pinfo)
};

#define DEFAULT_IO_QOS \
{ \
	.interval	= 10000u, \
	.latency	= 10u, \
	.sdu		= 40u, \
	.phy		= BT_ISO_PHY_2M, \
	.rtn		= 2u, \
}

static struct bt_iso_qos default_qos = {
	.bcast = {
		.big			= BT_ISO_QOS_BIG_UNSET,
		.bis			= BT_ISO_QOS_BIS_UNSET,
		.sync_factor		= 0x01,
		.packing		= 0x00,
		.framing		= 0x00,
		.in			= DEFAULT_IO_QOS,
		.out			= DEFAULT_IO_QOS,
		.encryption		= 0x00,
		.bcode			= {0x00},
		.options		= 0x00,
		.skip			= 0x0000,
		.sync_timeout		= 0x4000,
		.sync_cte_type		= 0x00,
		.mse			= 0x00,
		.timeout		= 0x4000,
	},
};

static struct sock *iso_sock_alloc(struct net *net, struct socket *sock,
				   int proto, gfp_t prio, int kern)
{
	struct sock *sk;

	sk = bt_sock_alloc(net, sock, &iso_proto, proto, prio, kern);
	if (!sk)
		return NULL;

	sk->sk_destruct = iso_sock_destruct;
	sk->sk_sndtimeo = ISO_CONN_TIMEOUT;

	/* Set address type as public as default src address is BDADDR_ANY */
	iso_pi(sk)->src_type = BDADDR_LE_PUBLIC;

	iso_pi(sk)->qos = default_qos;

	bt_sock_link(&iso_sk_list, sk);
	return sk;
}

static int iso_sock_create(struct net *net, struct socket *sock, int protocol,
			   int kern)
{
	struct sock *sk;

	BT_DBG("sock %p", sock);

	sock->state = SS_UNCONNECTED;

	if (sock->type != SOCK_SEQPACKET)
		return -ESOCKTNOSUPPORT;

	sock->ops = &iso_sock_ops;

	sk = iso_sock_alloc(net, sock, protocol, GFP_ATOMIC, kern);
	if (!sk)
		return -ENOMEM;

	iso_sock_init(sk, NULL);
	return 0;
}

static int iso_sock_bind_bc(struct socket *sock, struct sockaddr *addr,
			    int addr_len)
{
	struct sockaddr_iso *sa = (struct sockaddr_iso *)addr;
	struct sock *sk = sock->sk;
	int i;

	BT_DBG("sk %p bc_sid %u bc_num_bis %u", sk, sa->iso_bc->bc_sid,
	       sa->iso_bc->bc_num_bis);

	if (addr_len > sizeof(*sa) + sizeof(*sa->iso_bc) ||
	    sa->iso_bc->bc_num_bis < 0x01 || sa->iso_bc->bc_num_bis > 0x1f)
		return -EINVAL;

	bacpy(&iso_pi(sk)->dst, &sa->iso_bc->bc_bdaddr);
	iso_pi(sk)->dst_type = sa->iso_bc->bc_bdaddr_type;
	iso_pi(sk)->sync_handle = -1;
	iso_pi(sk)->bc_sid = sa->iso_bc->bc_sid;
	iso_pi(sk)->bc_num_bis = sa->iso_bc->bc_num_bis;

	for (i = 0; i < iso_pi(sk)->bc_num_bis; i++) {
		if (sa->iso_bc->bc_bis[i] < 0x01 ||
		    sa->iso_bc->bc_bis[i] > 0x1f)
			return -EINVAL;

		memcpy(iso_pi(sk)->bc_bis, sa->iso_bc->bc_bis,
		       iso_pi(sk)->bc_num_bis);
	}

	return 0;
}

static int iso_sock_bind(struct socket *sock, struct sockaddr *addr,
			 int addr_len)
{
	struct sockaddr_iso *sa = (struct sockaddr_iso *)addr;
	struct sock *sk = sock->sk;
	int err = 0;

	BT_DBG("sk %p %pMR type %u", sk, &sa->iso_bdaddr, sa->iso_bdaddr_type);

	if (!addr || addr_len < sizeof(struct sockaddr_iso) ||
	    addr->sa_family != AF_BLUETOOTH)
		return -EINVAL;

	lock_sock(sk);

	if (sk->sk_state != BT_OPEN) {
		err = -EBADFD;
		goto done;
	}

	if (sk->sk_type != SOCK_SEQPACKET) {
		err = -EINVAL;
		goto done;
	}

	/* Check if the address type is of LE type */
	if (!bdaddr_type_is_le(sa->iso_bdaddr_type)) {
		err = -EINVAL;
		goto done;
	}

	bacpy(&iso_pi(sk)->src, &sa->iso_bdaddr);
	iso_pi(sk)->src_type = sa->iso_bdaddr_type;

	/* Check for Broadcast address */
	if (addr_len > sizeof(*sa)) {
		err = iso_sock_bind_bc(sock, addr, addr_len);
		if (err)
			goto done;
	}

	sk->sk_state = BT_BOUND;

done:
	release_sock(sk);
	return err;
}

static int iso_sock_connect(struct socket *sock, struct sockaddr *addr,
			    int alen, int flags)
{
	struct sockaddr_iso *sa = (struct sockaddr_iso *)addr;
	struct sock *sk = sock->sk;
	int err;

	BT_DBG("sk %p", sk);

	if (alen < sizeof(struct sockaddr_iso) ||
	    addr->sa_family != AF_BLUETOOTH)
		return -EINVAL;

	if (sk->sk_state != BT_OPEN && sk->sk_state != BT_BOUND)
		return -EBADFD;

	if (sk->sk_type != SOCK_SEQPACKET)
		return -EINVAL;

	/* Check if the address type is of LE type */
	if (!bdaddr_type_is_le(sa->iso_bdaddr_type))
		return -EINVAL;

	lock_sock(sk);

	bacpy(&iso_pi(sk)->dst, &sa->iso_bdaddr);
	iso_pi(sk)->dst_type = sa->iso_bdaddr_type;

	release_sock(sk);

	if (bacmp(&iso_pi(sk)->dst, BDADDR_ANY))
		err = iso_connect_cis(sk);
	else
		err = iso_connect_bis(sk);

	if (err)
		return err;

	lock_sock(sk);

	if (!test_bit(BT_SK_DEFER_SETUP, &bt_sk(sk)->flags)) {
		err = bt_sock_wait_state(sk, BT_CONNECTED,
					 sock_sndtimeo(sk, flags & O_NONBLOCK));
	}

	release_sock(sk);
	return err;
}

static int iso_listen_bis(struct sock *sk)
{
	struct hci_dev *hdev;
	int err = 0;

	BT_DBG("%pMR -> %pMR (SID 0x%2.2x)", &iso_pi(sk)->src,
	       &iso_pi(sk)->dst, iso_pi(sk)->bc_sid);

	write_lock(&iso_sk_list.lock);

	if (__iso_get_sock_listen_by_sid(&iso_pi(sk)->src, &iso_pi(sk)->dst,
					 iso_pi(sk)->bc_sid))
		err = -EADDRINUSE;

	write_unlock(&iso_sk_list.lock);

	if (err)
		return err;

	hdev = hci_get_route(&iso_pi(sk)->dst, &iso_pi(sk)->src,
			     iso_pi(sk)->src_type);
	if (!hdev)
		return -EHOSTUNREACH;

	/* Fail if user set invalid QoS */
	if (iso_pi(sk)->qos_user_set && !check_bcast_qos(&iso_pi(sk)->qos)) {
		iso_pi(sk)->qos = default_qos;
		return -EINVAL;
	}

	err = hci_pa_create_sync(hdev, &iso_pi(sk)->dst,
				 le_addr_type(iso_pi(sk)->dst_type),
				 iso_pi(sk)->bc_sid, &iso_pi(sk)->qos);

	hci_dev_put(hdev);

	return err;
}

static int iso_listen_cis(struct sock *sk)
{
	int err = 0;

	BT_DBG("%pMR", &iso_pi(sk)->src);

	write_lock(&iso_sk_list.lock);

	if (__iso_get_sock_listen_by_addr(&iso_pi(sk)->src))
		err = -EADDRINUSE;

	write_unlock(&iso_sk_list.lock);

	return err;
}

static int iso_sock_listen(struct socket *sock, int backlog)
{
	struct sock *sk = sock->sk;
	int err = 0;

	BT_DBG("sk %p backlog %d", sk, backlog);

	lock_sock(sk);

	if (sk->sk_state != BT_BOUND) {
		err = -EBADFD;
		goto done;
	}

	if (sk->sk_type != SOCK_SEQPACKET) {
		err = -EINVAL;
		goto done;
	}

	if (!bacmp(&iso_pi(sk)->dst, BDADDR_ANY))
		err = iso_listen_cis(sk);
	else
		err = iso_listen_bis(sk);

	if (err)
		goto done;

	sk->sk_max_ack_backlog = backlog;
	sk->sk_ack_backlog = 0;

	sk->sk_state = BT_LISTEN;

done:
	release_sock(sk);
	return err;
}

static int iso_sock_accept(struct socket *sock, struct socket *newsock,
			   int flags, bool kern)
{
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	struct sock *sk = sock->sk, *ch;
	long timeo;
	int err = 0;

	lock_sock(sk);

	timeo = sock_rcvtimeo(sk, flags & O_NONBLOCK);

	BT_DBG("sk %p timeo %ld", sk, timeo);

	/* Wait for an incoming connection. (wake-one). */
	add_wait_queue_exclusive(sk_sleep(sk), &wait);
	while (1) {
		if (sk->sk_state != BT_LISTEN) {
			err = -EBADFD;
			break;
		}

		ch = bt_accept_dequeue(sk, newsock);
		if (ch)
			break;

		if (!timeo) {
			err = -EAGAIN;
			break;
		}

		if (signal_pending(current)) {
			err = sock_intr_errno(timeo);
			break;
		}

		release_sock(sk);

		timeo = wait_woken(&wait, TASK_INTERRUPTIBLE, timeo);
		lock_sock(sk);
	}
	remove_wait_queue(sk_sleep(sk), &wait);

	if (err)
		goto done;

	newsock->state = SS_CONNECTED;

	BT_DBG("new socket %p", ch);

done:
	release_sock(sk);
	return err;
}

static int iso_sock_getname(struct socket *sock, struct sockaddr *addr,
			    int peer)
{
	struct sockaddr_iso *sa = (struct sockaddr_iso *)addr;
	struct sock *sk = sock->sk;

	BT_DBG("sock %p, sk %p", sock, sk);

	addr->sa_family = AF_BLUETOOTH;

	if (peer) {
		bacpy(&sa->iso_bdaddr, &iso_pi(sk)->dst);
		sa->iso_bdaddr_type = iso_pi(sk)->dst_type;
	} else {
		bacpy(&sa->iso_bdaddr, &iso_pi(sk)->src);
		sa->iso_bdaddr_type = iso_pi(sk)->src_type;
	}

	return sizeof(struct sockaddr_iso);
}

static int iso_sock_sendmsg(struct socket *sock, struct msghdr *msg,
			    size_t len)
{
	struct sock *sk = sock->sk;
	struct sk_buff *skb, **frag;
	size_t mtu;
	int err;

	BT_DBG("sock %p, sk %p", sock, sk);

	err = sock_error(sk);
	if (err)
		return err;

	if (msg->msg_flags & MSG_OOB)
		return -EOPNOTSUPP;

	lock_sock(sk);

	if (sk->sk_state != BT_CONNECTED) {
		release_sock(sk);
		return -ENOTCONN;
	}

	mtu = iso_pi(sk)->conn->hcon->hdev->iso_mtu;

	release_sock(sk);

	skb = bt_skb_sendmsg(sk, msg, len, mtu, HCI_ISO_DATA_HDR_SIZE, 0);
	if (IS_ERR(skb))
		return PTR_ERR(skb);

	len -= skb->len;

	BT_DBG("skb %p len %d", sk, skb->len);

	/* Continuation fragments */
	frag = &skb_shinfo(skb)->frag_list;
	while (len) {
		struct sk_buff *tmp;

		tmp = bt_skb_sendmsg(sk, msg, len, mtu, 0, 0);
		if (IS_ERR(tmp)) {
			kfree_skb(skb);
			return PTR_ERR(tmp);
		}

		*frag = tmp;

		len  -= tmp->len;

		skb->len += tmp->len;
		skb->data_len += tmp->len;

		BT_DBG("frag %p len %d", *frag, tmp->len);

		frag = &(*frag)->next;
	}

	lock_sock(sk);

	if (sk->sk_state == BT_CONNECTED)
		err = iso_send_frame(sk, skb);
	else
		err = -ENOTCONN;

	release_sock(sk);

	if (err < 0)
		kfree_skb(skb);
	return err;
}

static void iso_conn_defer_accept(struct hci_conn *conn)
{
	struct hci_cp_le_accept_cis cp;
	struct hci_dev *hdev = conn->hdev;

	BT_DBG("conn %p", conn);

	conn->state = BT_CONFIG;

	cp.handle = cpu_to_le16(conn->handle);

	hci_send_cmd(hdev, HCI_OP_LE_ACCEPT_CIS, sizeof(cp), &cp);
}

static void iso_conn_big_sync(struct sock *sk)
{
	int err;
	struct hci_dev *hdev;

	hdev = hci_get_route(&iso_pi(sk)->dst, &iso_pi(sk)->src,
			     iso_pi(sk)->src_type);

	if (!hdev)
		return;

	if (!test_and_set_bit(BT_SK_BIG_SYNC, &iso_pi(sk)->flags)) {
		err = hci_le_big_create_sync(hdev, iso_pi(sk)->conn->hcon,
					     &iso_pi(sk)->qos,
					     iso_pi(sk)->sync_handle,
					     iso_pi(sk)->bc_num_bis,
					     iso_pi(sk)->bc_bis);
		if (err)
			bt_dev_err(hdev, "hci_le_big_create_sync: %d",
				   err);
	}
}

static int iso_sock_recvmsg(struct socket *sock, struct msghdr *msg,
			    size_t len, int flags)
{
	struct sock *sk = sock->sk;
	struct iso_pinfo *pi = iso_pi(sk);

	BT_DBG("sk %p", sk);

	if (test_and_clear_bit(BT_SK_DEFER_SETUP, &bt_sk(sk)->flags)) {
		lock_sock(sk);
		switch (sk->sk_state) {
		case BT_CONNECT2:
			if (pi->conn->hcon &&
			    test_bit(HCI_CONN_PA_SYNC, &pi->conn->hcon->flags)) {
				iso_conn_big_sync(sk);
				sk->sk_state = BT_LISTEN;
				set_bit(BT_SK_PA_SYNC, &iso_pi(sk)->flags);
			} else {
				iso_conn_defer_accept(pi->conn->hcon);
				sk->sk_state = BT_CONFIG;
			}
			release_sock(sk);
			return 0;
		case BT_CONNECT:
			release_sock(sk);
			return iso_connect_cis(sk);
		default:
			release_sock(sk);
			break;
		}
	}

	return bt_sock_recvmsg(sock, msg, len, flags);
}

static bool check_io_qos(struct bt_iso_io_qos *qos)
{
	/* If no PHY is enable SDU must be 0 */
	if (!qos->phy && qos->sdu)
		return false;

	if (qos->interval && (qos->interval < 0xff || qos->interval > 0xfffff))
		return false;

	if (qos->latency && (qos->latency < 0x05 || qos->latency > 0xfa0))
		return false;

	if (qos->phy > BT_ISO_PHY_ANY)
		return false;

	return true;
}

static bool check_ucast_qos(struct bt_iso_qos *qos)
{
	if (qos->ucast.cig > 0xef && qos->ucast.cig != BT_ISO_QOS_CIG_UNSET)
		return false;

	if (qos->ucast.cis > 0xef && qos->ucast.cis != BT_ISO_QOS_CIS_UNSET)
		return false;

	if (qos->ucast.sca > 0x07)
		return false;

	if (qos->ucast.packing > 0x01)
		return false;

	if (qos->ucast.framing > 0x01)
		return false;

	if (!check_io_qos(&qos->ucast.in))
		return false;

	if (!check_io_qos(&qos->ucast.out))
		return false;

	return true;
}

static bool check_bcast_qos(struct bt_iso_qos *qos)
{
	if (qos->bcast.sync_factor == 0x00)
		return false;

	if (qos->bcast.packing > 0x01)
		return false;

	if (qos->bcast.framing > 0x01)
		return false;

	if (!check_io_qos(&qos->bcast.in))
		return false;

	if (!check_io_qos(&qos->bcast.out))
		return false;

	if (qos->bcast.encryption > 0x01)
		return false;

	if (qos->bcast.options > 0x07)
		return false;

	if (qos->bcast.skip > 0x01f3)
		return false;

	if (qos->bcast.sync_timeout < 0x000a || qos->bcast.sync_timeout > 0x4000)
		return false;

	if (qos->bcast.sync_cte_type > 0x1f)
		return false;

	if (qos->bcast.mse > 0x1f)
		return false;

	if (qos->bcast.timeout < 0x000a || qos->bcast.timeout > 0x4000)
		return false;

	return true;
}

static int iso_sock_setsockopt(struct socket *sock, int level, int optname,
			       sockptr_t optval, unsigned int optlen)
{
	struct sock *sk = sock->sk;
	int len, err = 0;
	struct bt_iso_qos qos = default_qos;
	u32 opt;

	BT_DBG("sk %p", sk);

	lock_sock(sk);

	switch (optname) {
	case BT_DEFER_SETUP:
		if (sk->sk_state != BT_BOUND && sk->sk_state != BT_LISTEN) {
			err = -EINVAL;
			break;
		}

		if (copy_from_sockptr(&opt, optval, sizeof(u32))) {
			err = -EFAULT;
			break;
		}

		if (opt)
			set_bit(BT_SK_DEFER_SETUP, &bt_sk(sk)->flags);
		else
			clear_bit(BT_SK_DEFER_SETUP, &bt_sk(sk)->flags);
		break;

	case BT_PKT_STATUS:
		if (copy_from_sockptr(&opt, optval, sizeof(u32))) {
			err = -EFAULT;
			break;
		}

		if (opt)
			set_bit(BT_SK_PKT_STATUS, &bt_sk(sk)->flags);
		else
			clear_bit(BT_SK_PKT_STATUS, &bt_sk(sk)->flags);
		break;

	case BT_ISO_QOS:
		if (sk->sk_state != BT_OPEN && sk->sk_state != BT_BOUND &&
		    sk->sk_state != BT_CONNECT2) {
			err = -EINVAL;
			break;
		}

		len = min_t(unsigned int, sizeof(qos), optlen);

		if (copy_from_sockptr(&qos, optval, len)) {
			err = -EFAULT;
			break;
		}

		if (len == sizeof(qos.ucast) && !check_ucast_qos(&qos)) {
			err = -EINVAL;
			break;
		}

		iso_pi(sk)->qos = qos;
		iso_pi(sk)->qos_user_set = true;

		break;

	case BT_ISO_BASE:
		if (sk->sk_state != BT_OPEN && sk->sk_state != BT_BOUND &&
		    sk->sk_state != BT_CONNECT2) {
			err = -EINVAL;
			break;
		}

		if (optlen > sizeof(iso_pi(sk)->base)) {
			err = -EOVERFLOW;
			break;
		}

		len = min_t(unsigned int, sizeof(iso_pi(sk)->base), optlen);

		if (copy_from_sockptr(iso_pi(sk)->base, optval, len)) {
			err = -EFAULT;
			break;
		}

		iso_pi(sk)->base_len = len;

		break;

	default:
		err = -ENOPROTOOPT;
		break;
	}

	release_sock(sk);
	return err;
}

static int iso_sock_getsockopt(struct socket *sock, int level, int optname,
			       char __user *optval, int __user *optlen)
{
	struct sock *sk = sock->sk;
	int len, err = 0;
	struct bt_iso_qos *qos;
	u8 base_len;
	u8 *base;

	BT_DBG("sk %p", sk);

	if (get_user(len, optlen))
		return -EFAULT;

	lock_sock(sk);

	switch (optname) {
	case BT_DEFER_SETUP:
		if (sk->sk_state == BT_CONNECTED) {
			err = -EINVAL;
			break;
		}

		if (put_user(test_bit(BT_SK_DEFER_SETUP, &bt_sk(sk)->flags),
			     (u32 __user *)optval))
			err = -EFAULT;

		break;

	case BT_PKT_STATUS:
		if (put_user(test_bit(BT_SK_PKT_STATUS, &bt_sk(sk)->flags),
			     (int __user *)optval))
			err = -EFAULT;
		break;

	case BT_ISO_QOS:
		qos = iso_sock_get_qos(sk);

		len = min_t(unsigned int, len, sizeof(*qos));
		if (copy_to_user(optval, qos, len))
			err = -EFAULT;

		break;

	case BT_ISO_BASE:
		if (sk->sk_state == BT_CONNECTED &&
		    !bacmp(&iso_pi(sk)->dst, BDADDR_ANY)) {
			base_len = iso_pi(sk)->conn->hcon->le_per_adv_data_len;
			base = iso_pi(sk)->conn->hcon->le_per_adv_data;
		} else {
			base_len = iso_pi(sk)->base_len;
			base = iso_pi(sk)->base;
		}

		len = min_t(unsigned int, len, base_len);
		if (copy_to_user(optval, base, len))
			err = -EFAULT;

		break;

	default:
		err = -ENOPROTOOPT;
		break;
	}

	release_sock(sk);
	return err;
}

static int iso_sock_shutdown(struct socket *sock, int how)
{
	struct sock *sk = sock->sk;
	int err = 0;

	BT_DBG("sock %p, sk %p, how %d", sock, sk, how);

	if (!sk)
		return 0;

	sock_hold(sk);
	lock_sock(sk);

	switch (how) {
	case SHUT_RD:
		if (sk->sk_shutdown & RCV_SHUTDOWN)
			goto unlock;
		sk->sk_shutdown |= RCV_SHUTDOWN;
		break;
	case SHUT_WR:
		if (sk->sk_shutdown & SEND_SHUTDOWN)
			goto unlock;
		sk->sk_shutdown |= SEND_SHUTDOWN;
		break;
	case SHUT_RDWR:
		if (sk->sk_shutdown & SHUTDOWN_MASK)
			goto unlock;
		sk->sk_shutdown |= SHUTDOWN_MASK;
		break;
	}

	iso_sock_clear_timer(sk);
	__iso_sock_close(sk);

	if (sock_flag(sk, SOCK_LINGER) && sk->sk_lingertime &&
	    !(current->flags & PF_EXITING))
		err = bt_sock_wait_state(sk, BT_CLOSED, sk->sk_lingertime);

unlock:
	release_sock(sk);
	sock_put(sk);

	return err;
}

static int iso_sock_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	int err = 0;

	BT_DBG("sock %p, sk %p", sock, sk);

	if (!sk)
		return 0;

	iso_sock_close(sk);

	if (sock_flag(sk, SOCK_LINGER) && READ_ONCE(sk->sk_lingertime) &&
	    !(current->flags & PF_EXITING)) {
		lock_sock(sk);
		err = bt_sock_wait_state(sk, BT_CLOSED, sk->sk_lingertime);
		release_sock(sk);
	}

	sock_orphan(sk);
	iso_sock_kill(sk);
	return err;
}

static void iso_sock_ready(struct sock *sk)
{
	BT_DBG("sk %p", sk);

	if (!sk)
		return;

	lock_sock(sk);
	iso_sock_clear_timer(sk);
	sk->sk_state = BT_CONNECTED;
	sk->sk_state_change(sk);
	release_sock(sk);
}

struct iso_list_data {
	struct hci_conn *hcon;
	int count;
};

static bool iso_match_big(struct sock *sk, void *data)
{
	struct hci_evt_le_big_sync_estabilished *ev = data;

	return ev->handle == iso_pi(sk)->qos.bcast.big;
}

static bool iso_match_pa_sync_flag(struct sock *sk, void *data)
{
	return test_bit(BT_SK_PA_SYNC, &iso_pi(sk)->flags);
}

static void iso_conn_ready(struct iso_conn *conn)
{
	struct sock *parent = NULL;
	struct sock *sk = conn->sk;
	struct hci_ev_le_big_sync_estabilished *ev = NULL;
	struct hci_ev_le_pa_sync_established *ev2 = NULL;
	struct hci_conn *hcon;

	BT_DBG("conn %p", conn);

	if (sk) {
		iso_sock_ready(conn->sk);
	} else {
		hcon = conn->hcon;
		if (!hcon)
			return;

		if (test_bit(HCI_CONN_BIG_SYNC, &hcon->flags) ||
		    test_bit(HCI_CONN_BIG_SYNC_FAILED, &hcon->flags)) {
			ev = hci_recv_event_data(hcon->hdev,
						 HCI_EVT_LE_BIG_SYNC_ESTABILISHED);

			/* Get reference to PA sync parent socket, if it exists */
			parent = iso_get_sock_listen(&hcon->src,
						     &hcon->dst,
						     iso_match_pa_sync_flag, NULL);
			if (!parent && ev)
				parent = iso_get_sock_listen(&hcon->src,
							     &hcon->dst,
							     iso_match_big, ev);
		} else if (test_bit(HCI_CONN_PA_SYNC, &hcon->flags) ||
				test_bit(HCI_CONN_PA_SYNC_FAILED, &hcon->flags)) {
			ev2 = hci_recv_event_data(hcon->hdev,
						  HCI_EV_LE_PA_SYNC_ESTABLISHED);
			if (ev2)
				parent = iso_get_sock_listen(&hcon->src,
							     &hcon->dst,
							     iso_match_sid, ev2);
		}

		if (!parent)
			parent = iso_get_sock_listen(&hcon->src,
							BDADDR_ANY, NULL, NULL);

		if (!parent)
			return;

		lock_sock(parent);

		sk = iso_sock_alloc(sock_net(parent), NULL,
				    BTPROTO_ISO, GFP_ATOMIC, 0);
		if (!sk) {
			release_sock(parent);
			return;
		}

		iso_sock_init(sk, parent);

		bacpy(&iso_pi(sk)->src, &hcon->src);

		/* Convert from HCI to three-value type */
		if (hcon->src_type == ADDR_LE_DEV_PUBLIC)
			iso_pi(sk)->src_type = BDADDR_LE_PUBLIC;
		else
			iso_pi(sk)->src_type = BDADDR_LE_RANDOM;

		/* If hcon has no destination address (BDADDR_ANY) it means it
		 * was created by HCI_EV_LE_BIG_SYNC_ESTABILISHED or
		 * HCI_EV_LE_PA_SYNC_ESTABLISHED so we need to initialize using
		 * the parent socket destination address.
		 */
		if (!bacmp(&hcon->dst, BDADDR_ANY)) {
			bacpy(&hcon->dst, &iso_pi(parent)->dst);
			hcon->dst_type = iso_pi(parent)->dst_type;
			hcon->sync_handle = iso_pi(parent)->sync_handle;
		}

		if (ev2 && !ev2->status) {
			iso_pi(sk)->sync_handle = iso_pi(parent)->sync_handle;
			iso_pi(sk)->qos = iso_pi(parent)->qos;
			iso_pi(sk)->bc_num_bis = iso_pi(parent)->bc_num_bis;
			memcpy(iso_pi(sk)->bc_bis, iso_pi(parent)->bc_bis, ISO_MAX_NUM_BIS);
		}

		bacpy(&iso_pi(sk)->dst, &hcon->dst);
		iso_pi(sk)->dst_type = hcon->dst_type;
		iso_pi(sk)->sync_handle = iso_pi(parent)->sync_handle;
		memcpy(iso_pi(sk)->base, iso_pi(parent)->base, iso_pi(parent)->base_len);
		iso_pi(sk)->base_len = iso_pi(parent)->base_len;

		hci_conn_hold(hcon);
		iso_chan_add(conn, sk, parent);

		if ((ev && ((struct hci_evt_le_big_sync_estabilished *)ev)->status) ||
		    (ev2 && ev2->status)) {
			/* Trigger error signal on child socket */
			sk->sk_err = ECONNREFUSED;
			sk->sk_error_report(sk);
		}

		if (test_bit(BT_SK_DEFER_SETUP, &bt_sk(parent)->flags))
			sk->sk_state = BT_CONNECT2;
		else
			sk->sk_state = BT_CONNECTED;

		/* Wake up parent */
		parent->sk_data_ready(parent);

		release_sock(parent);
	}
}

static bool iso_match_sid(struct sock *sk, void *data)
{
	struct hci_ev_le_pa_sync_established *ev = data;

	return ev->sid == iso_pi(sk)->bc_sid;
}

static bool iso_match_sync_handle(struct sock *sk, void *data)
{
	struct hci_evt_le_big_info_adv_report *ev = data;

	return le16_to_cpu(ev->sync_handle) == iso_pi(sk)->sync_handle;
}

static bool iso_match_sync_handle_pa_report(struct sock *sk, void *data)
{
	struct hci_ev_le_per_adv_report *ev = data;

	return le16_to_cpu(ev->sync_handle) == iso_pi(sk)->sync_handle;
}

/* ----- ISO interface with lower layer (HCI) ----- */

int iso_connect_ind(struct hci_dev *hdev, bdaddr_t *bdaddr, __u8 *flags)
{
	struct hci_ev_le_pa_sync_established *ev1;
	struct hci_evt_le_big_info_adv_report *ev2;
	struct hci_ev_le_per_adv_report *ev3;
	struct sock *sk;
	int lm = 0;

	bt_dev_dbg(hdev, "bdaddr %pMR", bdaddr);

	/* Broadcast receiver requires handling of some events before it can
	 * proceed to establishing a BIG sync:
	 *
	 * 1. HCI_EV_LE_PA_SYNC_ESTABLISHED: The socket may specify a specific
	 * SID to listen to and once sync is estabilished its handle needs to
	 * be stored in iso_pi(sk)->sync_handle so it can be matched once
	 * receiving the BIG Info.
	 * 2. HCI_EVT_LE_BIG_INFO_ADV_REPORT: When connect_ind is triggered by a
	 * a BIG Info it attempts to check if there any listening socket with
	 * the same sync_handle and if it does then attempt to create a sync.
	 * 3. HCI_EV_LE_PER_ADV_REPORT: When a PA report is received, it is stored
	 * in iso_pi(sk)->base so it can be passed up to user, in the case of a
	 * broadcast sink.
	 */
	ev1 = hci_recv_event_data(hdev, HCI_EV_LE_PA_SYNC_ESTABLISHED);
	if (ev1) {
		sk = iso_get_sock_listen(&hdev->bdaddr, bdaddr, iso_match_sid,
					 ev1);
		if (sk && !ev1->status)
			iso_pi(sk)->sync_handle = le16_to_cpu(ev1->handle);

		goto done;
	}

	ev2 = hci_recv_event_data(hdev, HCI_EVT_LE_BIG_INFO_ADV_REPORT);
	if (ev2) {
		/* Try to get PA sync listening socket, if it exists */
		sk = iso_get_sock_listen(&hdev->bdaddr, bdaddr,
						iso_match_pa_sync_flag, NULL);
		if (!sk)
			sk = iso_get_sock_listen(&hdev->bdaddr, bdaddr,
						 iso_match_sync_handle, ev2);
		if (sk) {
			int err;

			if (ev2->num_bis < iso_pi(sk)->bc_num_bis)
				iso_pi(sk)->bc_num_bis = ev2->num_bis;

			if (!test_bit(BT_SK_DEFER_SETUP, &bt_sk(sk)->flags) &&
			    !test_and_set_bit(BT_SK_BIG_SYNC, &iso_pi(sk)->flags)) {
				err = hci_le_big_create_sync(hdev, NULL,
							     &iso_pi(sk)->qos,
							     iso_pi(sk)->sync_handle,
							     iso_pi(sk)->bc_num_bis,
							     iso_pi(sk)->bc_bis);
				if (err) {
					bt_dev_err(hdev, "hci_le_big_create_sync: %d",
						   err);
					sk = NULL;
				}
			}
		}
	}

	ev3 = hci_recv_event_data(hdev, HCI_EV_LE_PER_ADV_REPORT);
	if (ev3) {
		sk = iso_get_sock_listen(&hdev->bdaddr, bdaddr,
					 iso_match_sync_handle_pa_report, ev3);

		if (sk) {
			memcpy(iso_pi(sk)->base, ev3->data, ev3->length);
			iso_pi(sk)->base_len = ev3->length;
		}
	} else {
		sk = iso_get_sock_listen(&hdev->bdaddr, BDADDR_ANY, NULL, NULL);
	}

done:
	if (!sk)
		return lm;

	lm |= HCI_LM_ACCEPT;

	if (test_bit(BT_SK_DEFER_SETUP, &bt_sk(sk)->flags))
		*flags |= HCI_PROTO_DEFER;

	return lm;
}

static void iso_connect_cfm(struct hci_conn *hcon, __u8 status)
{
	if (hcon->type != ISO_LINK) {
		if (hcon->type != LE_LINK)
			return;

		/* Check if LE link has failed */
		if (status) {
			struct hci_link *link, *t;

			list_for_each_entry_safe(link, t, &hcon->link_list,
						 list)
				iso_conn_del(link->conn, bt_to_errno(status));

			return;
		}

		/* Create CIS if pending */
		hci_le_create_cis_pending(hcon->hdev);
		return;
	}

	BT_DBG("hcon %p bdaddr %pMR status %d", hcon, &hcon->dst, status);

	/* Similar to the success case, if HCI_CONN_BIG_SYNC_FAILED or
	 * HCI_CONN_PA_SYNC_FAILED is set, queue the failed connection
	 * into the accept queue of the listening socket and wake up
	 * userspace, to inform the user about the event.
	 */
	if (!status || test_bit(HCI_CONN_BIG_SYNC_FAILED, &hcon->flags) ||
	    test_bit(HCI_CONN_PA_SYNC_FAILED, &hcon->flags)) {
		struct iso_conn *conn;

		conn = iso_conn_add(hcon);
		if (conn)
			iso_conn_ready(conn);
	} else {
		iso_conn_del(hcon, bt_to_errno(status));
	}
}

static void iso_disconn_cfm(struct hci_conn *hcon, __u8 reason)
{
	if (hcon->type != ISO_LINK)
		return;

	BT_DBG("hcon %p reason %d", hcon, reason);

	iso_conn_del(hcon, bt_to_errno(reason));
}

void iso_recv(struct hci_conn *hcon, struct sk_buff *skb, u16 flags)
{
	struct iso_conn *conn = hcon->iso_data;
	__u16 pb, ts, len;

	if (!conn)
		goto drop;

	pb     = hci_iso_flags_pb(flags);
	ts     = hci_iso_flags_ts(flags);

	BT_DBG("conn %p len %d pb 0x%x ts 0x%x", conn, skb->len, pb, ts);

	switch (pb) {
	case ISO_START:
	case ISO_SINGLE:
		if (conn->rx_len) {
			BT_ERR("Unexpected start frame (len %d)", skb->len);
			kfree_skb(conn->rx_skb);
			conn->rx_skb = NULL;
			conn->rx_len = 0;
		}

		if (ts) {
			struct hci_iso_ts_data_hdr *hdr;

			/* TODO: add timestamp to the packet? */
			hdr = skb_pull_data(skb, HCI_ISO_TS_DATA_HDR_SIZE);
			if (!hdr) {
				BT_ERR("Frame is too short (len %d)", skb->len);
				goto drop;
			}

			len = __le16_to_cpu(hdr->slen);
		} else {
			struct hci_iso_data_hdr *hdr;

			hdr = skb_pull_data(skb, HCI_ISO_DATA_HDR_SIZE);
			if (!hdr) {
				BT_ERR("Frame is too short (len %d)", skb->len);
				goto drop;
			}

			len = __le16_to_cpu(hdr->slen);
		}

		flags  = hci_iso_data_flags(len);
		len    = hci_iso_data_len(len);

		BT_DBG("Start: total len %d, frag len %d flags 0x%4.4x", len,
		       skb->len, flags);

		if (len == skb->len) {
			/* Complete frame received */
			hci_skb_pkt_status(skb) = flags & 0x03;
			iso_recv_frame(conn, skb);
			return;
		}

		if (pb == ISO_SINGLE) {
			BT_ERR("Frame malformed (len %d, expected len %d)",
			       skb->len, len);
			goto drop;
		}

		if (skb->len > len) {
			BT_ERR("Frame is too long (len %d, expected len %d)",
			       skb->len, len);
			goto drop;
		}

		/* Allocate skb for the complete frame (with header) */
		conn->rx_skb = bt_skb_alloc(len, GFP_KERNEL);
		if (!conn->rx_skb)
			goto drop;

		hci_skb_pkt_status(conn->rx_skb) = flags & 0x03;
		skb_copy_from_linear_data(skb, skb_put(conn->rx_skb, skb->len),
					  skb->len);
		conn->rx_len = len - skb->len;
		break;

	case ISO_CONT:
		BT_DBG("Cont: frag len %d (expecting %d)", skb->len,
		       conn->rx_len);

		if (!conn->rx_len) {
			BT_ERR("Unexpected continuation frame (len %d)",
			       skb->len);
			goto drop;
		}

		if (skb->len > conn->rx_len) {
			BT_ERR("Fragment is too long (len %d, expected %d)",
			       skb->len, conn->rx_len);
			kfree_skb(conn->rx_skb);
			conn->rx_skb = NULL;
			conn->rx_len = 0;
			goto drop;
		}

		skb_copy_from_linear_data(skb, skb_put(conn->rx_skb, skb->len),
					  skb->len);
		conn->rx_len -= skb->len;
		return;

	case ISO_END:
		skb_copy_from_linear_data(skb, skb_put(conn->rx_skb, skb->len),
					  skb->len);
		conn->rx_len -= skb->len;

		if (!conn->rx_len) {
			struct sk_buff *rx_skb = conn->rx_skb;

			/* Complete frame received. iso_recv_frame
			 * takes ownership of the skb so set the global
			 * rx_skb pointer to NULL first.
			 */
			conn->rx_skb = NULL;
			iso_recv_frame(conn, rx_skb);
		}
		break;
	}

drop:
	kfree_skb(skb);
}

static struct hci_cb iso_cb = {
	.name		= "ISO",
	.connect_cfm	= iso_connect_cfm,
	.disconn_cfm	= iso_disconn_cfm,
};

static int iso_debugfs_show(struct seq_file *f, void *p)
{
	struct sock *sk;

	read_lock(&iso_sk_list.lock);

	sk_for_each(sk, &iso_sk_list.head) {
		seq_printf(f, "%pMR %pMR %d\n", &iso_pi(sk)->src,
			   &iso_pi(sk)->dst, sk->sk_state);
	}

	read_unlock(&iso_sk_list.lock);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(iso_debugfs);

static struct dentry *iso_debugfs;

static const struct proto_ops iso_sock_ops = {
	.family		= PF_BLUETOOTH,
	.owner		= THIS_MODULE,
	.release	= iso_sock_release,
	.bind		= iso_sock_bind,
	.connect	= iso_sock_connect,
	.listen		= iso_sock_listen,
	.accept		= iso_sock_accept,
	.getname	= iso_sock_getname,
	.sendmsg	= iso_sock_sendmsg,
	.recvmsg	= iso_sock_recvmsg,
	.poll		= bt_sock_poll,
	.ioctl		= bt_sock_ioctl,
	.mmap		= sock_no_mmap,
	.socketpair	= sock_no_socketpair,
	.shutdown	= iso_sock_shutdown,
	.setsockopt	= iso_sock_setsockopt,
	.getsockopt	= iso_sock_getsockopt
};

static const struct net_proto_family iso_sock_family_ops = {
	.family	= PF_BLUETOOTH,
	.owner	= THIS_MODULE,
	.create	= iso_sock_create,
};

static bool iso_inited;

bool iso_enabled(void)
{
	return iso_inited;
}

int iso_init(void)
{
	int err;

	BUILD_BUG_ON(sizeof(struct sockaddr_iso) > sizeof(struct sockaddr));

	if (iso_inited)
		return -EALREADY;

	err = proto_register(&iso_proto, 0);
	if (err < 0)
		return err;

	err = bt_sock_register(BTPROTO_ISO, &iso_sock_family_ops);
	if (err < 0) {
		BT_ERR("ISO socket registration failed");
		goto error;
	}

	err = bt_procfs_init(&init_net, "iso", &iso_sk_list, NULL);
	if (err < 0) {
		BT_ERR("Failed to create ISO proc file");
		bt_sock_unregister(BTPROTO_ISO);
		goto error;
	}

	BT_INFO("ISO socket layer initialized");

	hci_register_cb(&iso_cb);

	if (IS_ERR_OR_NULL(bt_debugfs))
		return 0;

	if (!iso_debugfs) {
		iso_debugfs = debugfs_create_file("iso", 0444, bt_debugfs,
						  NULL, &iso_debugfs_fops);
	}

	iso_inited = true;

	return 0;

error:
	proto_unregister(&iso_proto);
	return err;
}

int iso_exit(void)
{
	if (!iso_inited)
		return -EALREADY;

	bt_procfs_cleanup(&init_net, "iso");

	debugfs_remove(iso_debugfs);
	iso_debugfs = NULL;

	hci_unregister_cb(&iso_cb);

	bt_sock_unregister(BTPROTO_ISO);

	proto_unregister(&iso_proto);

	iso_inited = false;

	return 0;
}
