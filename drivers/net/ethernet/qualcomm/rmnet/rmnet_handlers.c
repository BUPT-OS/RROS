// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2013-2018, 2021, The Linux Foundation. All rights reserved.
 *
 * RMNET Data ingress/egress handler
 */

#include <linux/netdevice.h>
#include <linux/netdev_features.h>
#include <linux/if_arp.h>
#include <net/sock.h>
#include "rmnet_private.h"
#include "rmnet_config.h"
#include "rmnet_vnd.h"
#include "rmnet_map.h"
#include "rmnet_handlers.h"

#define RMNET_IP_VERSION_4 0x40
#define RMNET_IP_VERSION_6 0x60

/* Helper Functions */

static void rmnet_set_skb_proto(struct sk_buff *skb)
{
	switch (skb->data[0] & 0xF0) {
	case RMNET_IP_VERSION_4:
		skb->protocol = htons(ETH_P_IP);
		break;
	case RMNET_IP_VERSION_6:
		skb->protocol = htons(ETH_P_IPV6);
		break;
	default:
		skb->protocol = htons(ETH_P_MAP);
		break;
	}
}

/* Generic handler */

static void
rmnet_deliver_skb(struct sk_buff *skb)
{
	struct rmnet_priv *priv = netdev_priv(skb->dev);

	skb_reset_transport_header(skb);
	skb_reset_network_header(skb);
	rmnet_vnd_rx_fixup(skb, skb->dev);

	skb->pkt_type = PACKET_HOST;
	skb_set_mac_header(skb, 0);
	gro_cells_receive(&priv->gro_cells, skb);
}

/* MAP handler */

static void
__rmnet_map_ingress_handler(struct sk_buff *skb,
			    struct rmnet_port *port)
{
	struct rmnet_map_header *map_header = (void *)skb->data;
	struct rmnet_endpoint *ep;
	u16 len, pad;
	u8 mux_id;

	if (map_header->flags & MAP_CMD_FLAG) {
		/* Packet contains a MAP command (not data) */
		if (port->data_format & RMNET_FLAGS_INGRESS_MAP_COMMANDS)
			return rmnet_map_command(skb, port);

		goto free_skb;
	}

	mux_id = map_header->mux_id;
	pad = map_header->flags & MAP_PAD_LEN_MASK;
	len = ntohs(map_header->pkt_len) - pad;

	if (mux_id >= RMNET_MAX_LOGICAL_EP)
		goto free_skb;

	ep = rmnet_get_endpoint(port, mux_id);
	if (!ep)
		goto free_skb;

	skb->dev = ep->egress_dev;

	if ((port->data_format & RMNET_FLAGS_INGRESS_MAP_CKSUMV5) &&
	    (map_header->flags & MAP_NEXT_HEADER_FLAG)) {
		if (rmnet_map_process_next_hdr_packet(skb, len))
			goto free_skb;
		skb_pull(skb, sizeof(*map_header));
		rmnet_set_skb_proto(skb);
	} else {
		/* Subtract MAP header */
		skb_pull(skb, sizeof(*map_header));
		rmnet_set_skb_proto(skb);
		if (port->data_format & RMNET_FLAGS_INGRESS_MAP_CKSUMV4 &&
		    !rmnet_map_checksum_downlink_packet(skb, len + pad))
			skb->ip_summed = CHECKSUM_UNNECESSARY;
	}

	skb_trim(skb, len);
	rmnet_deliver_skb(skb);
	return;

free_skb:
	kfree_skb(skb);
}

static void
rmnet_map_ingress_handler(struct sk_buff *skb,
			  struct rmnet_port *port)
{
	struct sk_buff *skbn;

	if (skb->dev->type == ARPHRD_ETHER) {
		if (pskb_expand_head(skb, ETH_HLEN, 0, GFP_ATOMIC)) {
			kfree_skb(skb);
			return;
		}

		skb_push(skb, ETH_HLEN);
	}

	if (port->data_format & RMNET_FLAGS_INGRESS_DEAGGREGATION) {
		while ((skbn = rmnet_map_deaggregate(skb, port)) != NULL)
			__rmnet_map_ingress_handler(skbn, port);

		consume_skb(skb);
	} else {
		__rmnet_map_ingress_handler(skb, port);
	}
}

static int rmnet_map_egress_handler(struct sk_buff *skb,
				    struct rmnet_port *port, u8 mux_id,
				    struct net_device *orig_dev)
{
	int required_headroom, additional_header_len, csum_type = 0;
	struct rmnet_map_header *map_header;

	additional_header_len = 0;
	required_headroom = sizeof(struct rmnet_map_header);

	if (port->data_format & RMNET_FLAGS_EGRESS_MAP_CKSUMV4) {
		additional_header_len = sizeof(struct rmnet_map_ul_csum_header);
		csum_type = RMNET_FLAGS_EGRESS_MAP_CKSUMV4;
	} else if (port->data_format & RMNET_FLAGS_EGRESS_MAP_CKSUMV5) {
		additional_header_len = sizeof(struct rmnet_map_v5_csum_header);
		csum_type = RMNET_FLAGS_EGRESS_MAP_CKSUMV5;
	}

	required_headroom += additional_header_len;

	if (skb_cow_head(skb, required_headroom) < 0)
		return -ENOMEM;

	if (csum_type)
		rmnet_map_checksum_uplink_packet(skb, port, orig_dev,
						 csum_type);

	map_header = rmnet_map_add_map_header(skb, additional_header_len,
					      port, 0);
	if (!map_header)
		return -ENOMEM;

	map_header->mux_id = mux_id;

	if (READ_ONCE(port->egress_agg_params.count) > 1) {
		unsigned int len;

		len = rmnet_map_tx_aggregate(skb, port, orig_dev);
		if (likely(len)) {
			rmnet_vnd_tx_fixup_len(len, orig_dev);
			return -EINPROGRESS;
		}
		return -ENOMEM;
	}

	skb->protocol = htons(ETH_P_MAP);
	return 0;
}

static void
rmnet_bridge_handler(struct sk_buff *skb, struct net_device *bridge_dev)
{
	if (skb_mac_header_was_set(skb))
		skb_push(skb, skb->mac_len);

	if (bridge_dev) {
		skb->dev = bridge_dev;
		dev_queue_xmit(skb);
	}
}

/* Ingress / Egress Entry Points */

/* Processes packet as per ingress data format for receiving device. Logical
 * endpoint is determined from packet inspection. Packet is then sent to the
 * egress device listed in the logical endpoint configuration.
 */
rx_handler_result_t rmnet_rx_handler(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct rmnet_port *port;
	struct net_device *dev;

	if (!skb)
		goto done;

	if (skb_linearize(skb)) {
		kfree_skb(skb);
		goto done;
	}

	if (skb->pkt_type == PACKET_LOOPBACK)
		return RX_HANDLER_PASS;

	dev = skb->dev;
	port = rmnet_get_port_rcu(dev);
	if (unlikely(!port)) {
		dev_core_stats_rx_nohandler_inc(skb->dev);
		kfree_skb(skb);
		goto done;
	}

	switch (port->rmnet_mode) {
	case RMNET_EPMODE_VND:
		rmnet_map_ingress_handler(skb, port);
		break;
	case RMNET_EPMODE_BRIDGE:
		rmnet_bridge_handler(skb, port->bridge_ep);
		break;
	}

done:
	return RX_HANDLER_CONSUMED;
}

/* Modifies packet as per logical endpoint configuration and egress data format
 * for egress device configured in logical endpoint. Packet is then transmitted
 * on the egress device.
 */
void rmnet_egress_handler(struct sk_buff *skb)
{
	struct net_device *orig_dev;
	struct rmnet_port *port;
	struct rmnet_priv *priv;
	u8 mux_id;
	int err;

	sk_pacing_shift_update(skb->sk, 8);

	orig_dev = skb->dev;
	priv = netdev_priv(orig_dev);
	skb->dev = priv->real_dev;
	mux_id = priv->mux_id;

	port = rmnet_get_port_rcu(skb->dev);
	if (!port)
		goto drop;

	err = rmnet_map_egress_handler(skb, port, mux_id, orig_dev);
	if (err == -ENOMEM)
		goto drop;
	else if (err == -EINPROGRESS)
		return;

	rmnet_vnd_tx_fixup(skb, orig_dev);

	dev_queue_xmit(skb);
	return;

drop:
	this_cpu_inc(priv->pcpu_stats->stats.tx_drops);
	kfree_skb(skb);
}
