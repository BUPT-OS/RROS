// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, MediaTek Inc.
 * Copyright (c) 2021-2022, Intel Corporation.
 *
 * Authors:
 *  Amir Hanania <amir.hanania@intel.com>
 *  Chandrashekar Devegowda <chandrashekar.devegowda@intel.com>
 *  Haijun Liu <haijun.liu@mediatek.com>
 *  Moises Veleta <moises.veleta@intel.com>
 *  Ricardo Martinez <ricardo.martinez@linux.intel.com>
 *
 * Contributors:
 *  Andy Shevchenko <andriy.shevchenko@linux.intel.com>
 *  Chiranjeevi Rapolu <chiranjeevi.rapolu@intel.com>
 *  Eliot Lee <eliot.lee@intel.com>
 *  Sreehari Kancharla <sreehari.kancharla@intel.com>
 */

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/minmax.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/wwan.h>

#include "t7xx_port.h"
#include "t7xx_port_proxy.h"
#include "t7xx_state_monitor.h"

static int t7xx_port_ctrl_start(struct wwan_port *port)
{
	struct t7xx_port *port_mtk = wwan_port_get_drvdata(port);

	if (atomic_read(&port_mtk->usage_cnt))
		return -EBUSY;

	atomic_inc(&port_mtk->usage_cnt);
	return 0;
}

static void t7xx_port_ctrl_stop(struct wwan_port *port)
{
	struct t7xx_port *port_mtk = wwan_port_get_drvdata(port);

	atomic_dec(&port_mtk->usage_cnt);
}

static int t7xx_port_ctrl_tx(struct wwan_port *port, struct sk_buff *skb)
{
	struct t7xx_port *port_private = wwan_port_get_drvdata(port);
	const struct t7xx_port_conf *port_conf;
	struct sk_buff *cur = skb, *cloned;
	struct t7xx_fsm_ctl *ctl;
	enum md_state md_state;
	int cnt = 0, ret;

	if (!port_private->chan_enable)
		return -EINVAL;

	port_conf = port_private->port_conf;
	ctl = port_private->t7xx_dev->md->fsm_ctl;
	md_state = t7xx_fsm_get_md_state(ctl);
	if (md_state == MD_STATE_WAITING_FOR_HS1 || md_state == MD_STATE_WAITING_FOR_HS2) {
		dev_warn(port_private->dev, "Cannot write to %s port when md_state=%d\n",
			 port_conf->name, md_state);
		return -ENODEV;
	}

	while (cur) {
		cloned = skb_clone(cur, GFP_KERNEL);
		cloned->len = skb_headlen(cur);
		ret = t7xx_port_send_skb(port_private, cloned, 0, 0);
		if (ret) {
			dev_kfree_skb(cloned);
			dev_err(port_private->dev, "Write error on %s port, %d\n",
				port_conf->name, ret);
			return cnt ? cnt + ret : ret;
		}
		cnt += cur->len;
		if (cur == skb)
			cur = skb_shinfo(skb)->frag_list;
		else
			cur = cur->next;
	}

	dev_kfree_skb(skb);
	return 0;
}

static const struct wwan_port_ops wwan_ops = {
	.start = t7xx_port_ctrl_start,
	.stop = t7xx_port_ctrl_stop,
	.tx = t7xx_port_ctrl_tx,
};

static int t7xx_port_wwan_init(struct t7xx_port *port)
{
	port->rx_length_th = RX_QUEUE_MAXLEN;
	return 0;
}

static void t7xx_port_wwan_uninit(struct t7xx_port *port)
{
	if (!port->wwan.wwan_port)
		return;

	port->rx_length_th = 0;
	wwan_remove_port(port->wwan.wwan_port);
	port->wwan.wwan_port = NULL;
}

static int t7xx_port_wwan_recv_skb(struct t7xx_port *port, struct sk_buff *skb)
{
	if (!atomic_read(&port->usage_cnt) || !port->chan_enable) {
		const struct t7xx_port_conf *port_conf = port->port_conf;

		dev_kfree_skb_any(skb);
		dev_err_ratelimited(port->dev, "Port %s is not opened, drop packets\n",
				    port_conf->name);
		/* Dropping skb, caller should not access skb.*/
		return 0;
	}

	wwan_port_rx(port->wwan.wwan_port, skb);
	return 0;
}

static int t7xx_port_wwan_enable_chl(struct t7xx_port *port)
{
	spin_lock(&port->port_update_lock);
	port->chan_enable = true;
	spin_unlock(&port->port_update_lock);

	return 0;
}

static int t7xx_port_wwan_disable_chl(struct t7xx_port *port)
{
	spin_lock(&port->port_update_lock);
	port->chan_enable = false;
	spin_unlock(&port->port_update_lock);

	return 0;
}

static void t7xx_port_wwan_md_state_notify(struct t7xx_port *port, unsigned int state)
{
	const struct t7xx_port_conf *port_conf = port->port_conf;
	unsigned int header_len = sizeof(struct ccci_header);
	struct wwan_port_caps caps;

	if (state != MD_STATE_READY)
		return;

	if (!port->wwan.wwan_port) {
		caps.frag_len = CLDMA_MTU - header_len;
		caps.headroom_len = header_len;
		port->wwan.wwan_port = wwan_create_port(port->dev, port_conf->port_type,
							&wwan_ops, &caps, port);
		if (IS_ERR(port->wwan.wwan_port))
			dev_err(port->dev, "Unable to create WWWAN port %s", port_conf->name);
	}
}

struct port_ops wwan_sub_port_ops = {
	.init = t7xx_port_wwan_init,
	.recv_skb = t7xx_port_wwan_recv_skb,
	.uninit = t7xx_port_wwan_uninit,
	.enable_chl = t7xx_port_wwan_enable_chl,
	.disable_chl = t7xx_port_wwan_disable_chl,
	.md_state_notify = t7xx_port_wwan_md_state_notify,
};
