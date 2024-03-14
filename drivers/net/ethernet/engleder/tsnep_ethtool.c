// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2021 Gerhard Engleder <gerhard@engleder-embedded.com> */

#include "tsnep.h"

static const char tsnep_stats_strings[][ETH_GSTRING_LEN] = {
	"rx_packets",
	"rx_bytes",
	"rx_dropped",
	"rx_multicast",
	"rx_alloc_failed",
	"rx_phy_errors",
	"rx_forwarded_phy_errors",
	"rx_invalid_frame_errors",
	"tx_packets",
	"tx_bytes",
	"tx_dropped",
};

struct tsnep_stats {
	u64 rx_packets;
	u64 rx_bytes;
	u64 rx_dropped;
	u64 rx_multicast;
	u64 rx_alloc_failed;
	u64 rx_phy_errors;
	u64 rx_forwarded_phy_errors;
	u64 rx_invalid_frame_errors;
	u64 tx_packets;
	u64 tx_bytes;
	u64 tx_dropped;
};

#define TSNEP_STATS_COUNT (sizeof(struct tsnep_stats) / sizeof(u64))

static const char tsnep_rx_queue_stats_strings[][ETH_GSTRING_LEN] = {
	"rx_%d_packets",
	"rx_%d_bytes",
	"rx_%d_dropped",
	"rx_%d_multicast",
	"rx_%d_alloc_failed",
	"rx_%d_no_descriptor_errors",
	"rx_%d_buffer_too_small_errors",
	"rx_%d_fifo_overflow_errors",
	"rx_%d_invalid_frame_errors",
};

struct tsnep_rx_queue_stats {
	u64 rx_packets;
	u64 rx_bytes;
	u64 rx_dropped;
	u64 rx_multicast;
	u64 rx_alloc_failed;
	u64 rx_no_descriptor_errors;
	u64 rx_buffer_too_small_errors;
	u64 rx_fifo_overflow_errors;
	u64 rx_invalid_frame_errors;
};

#define TSNEP_RX_QUEUE_STATS_COUNT (sizeof(struct tsnep_rx_queue_stats) / \
				    sizeof(u64))

static const char tsnep_tx_queue_stats_strings[][ETH_GSTRING_LEN] = {
	"tx_%d_packets",
	"tx_%d_bytes",
	"tx_%d_dropped",
};

struct tsnep_tx_queue_stats {
	u64 tx_packets;
	u64 tx_bytes;
	u64 tx_dropped;
};

#define TSNEP_TX_QUEUE_STATS_COUNT (sizeof(struct tsnep_tx_queue_stats) / \
				    sizeof(u64))

static void tsnep_ethtool_get_drvinfo(struct net_device *netdev,
				      struct ethtool_drvinfo *drvinfo)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);

	strscpy(drvinfo->driver, TSNEP, sizeof(drvinfo->driver));
	strscpy(drvinfo->bus_info, dev_name(&adapter->pdev->dev),
		sizeof(drvinfo->bus_info));
}

static int tsnep_ethtool_get_regs_len(struct net_device *netdev)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);
	int len;
	int num_additional_queues;

	len = TSNEP_MAC_SIZE;

	/* first queue pair is within TSNEP_MAC_SIZE, only queues additional to
	 * the first queue pair extend the register length by TSNEP_QUEUE_SIZE
	 */
	num_additional_queues =
		max(adapter->num_tx_queues, adapter->num_rx_queues) - 1;
	len += TSNEP_QUEUE_SIZE * num_additional_queues;

	return len;
}

static void tsnep_ethtool_get_regs(struct net_device *netdev,
				   struct ethtool_regs *regs,
				   void *p)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);

	regs->version = 1;

	memcpy_fromio(p, adapter->addr, regs->len);
}

static u32 tsnep_ethtool_get_msglevel(struct net_device *netdev)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);

	return adapter->msg_enable;
}

static void tsnep_ethtool_set_msglevel(struct net_device *netdev, u32 data)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);

	adapter->msg_enable = data;
}

static void tsnep_ethtool_get_strings(struct net_device *netdev, u32 stringset,
				      u8 *data)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);
	int rx_count = adapter->num_rx_queues;
	int tx_count = adapter->num_tx_queues;
	int i, j;

	switch (stringset) {
	case ETH_SS_STATS:
		memcpy(data, tsnep_stats_strings, sizeof(tsnep_stats_strings));
		data += sizeof(tsnep_stats_strings);

		for (i = 0; i < rx_count; i++) {
			for (j = 0; j < TSNEP_RX_QUEUE_STATS_COUNT; j++) {
				snprintf(data, ETH_GSTRING_LEN,
					 tsnep_rx_queue_stats_strings[j], i);
				data += ETH_GSTRING_LEN;
			}
		}

		for (i = 0; i < tx_count; i++) {
			for (j = 0; j < TSNEP_TX_QUEUE_STATS_COUNT; j++) {
				snprintf(data, ETH_GSTRING_LEN,
					 tsnep_tx_queue_stats_strings[j], i);
				data += ETH_GSTRING_LEN;
			}
		}
		break;
	case ETH_SS_TEST:
		tsnep_ethtool_get_test_strings(data);
		break;
	}
}

static void tsnep_ethtool_get_ethtool_stats(struct net_device *netdev,
					    struct ethtool_stats *stats,
					    u64 *data)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);
	int rx_count = adapter->num_rx_queues;
	int tx_count = adapter->num_tx_queues;
	struct tsnep_stats tsnep_stats;
	struct tsnep_rx_queue_stats tsnep_rx_queue_stats;
	struct tsnep_tx_queue_stats tsnep_tx_queue_stats;
	u32 reg;
	int i;

	memset(&tsnep_stats, 0, sizeof(tsnep_stats));
	for (i = 0; i < adapter->num_rx_queues; i++) {
		tsnep_stats.rx_packets += adapter->rx[i].packets;
		tsnep_stats.rx_bytes += adapter->rx[i].bytes;
		tsnep_stats.rx_dropped += adapter->rx[i].dropped;
		tsnep_stats.rx_multicast += adapter->rx[i].multicast;
		tsnep_stats.rx_alloc_failed += adapter->rx[i].alloc_failed;
	}
	reg = ioread32(adapter->addr + ECM_STAT);
	tsnep_stats.rx_phy_errors =
		(reg & ECM_STAT_RX_ERR_MASK) >> ECM_STAT_RX_ERR_SHIFT;
	tsnep_stats.rx_forwarded_phy_errors =
		(reg & ECM_STAT_FWD_RX_ERR_MASK) >> ECM_STAT_FWD_RX_ERR_SHIFT;
	tsnep_stats.rx_invalid_frame_errors =
		(reg & ECM_STAT_INV_FRM_MASK) >> ECM_STAT_INV_FRM_SHIFT;
	for (i = 0; i < adapter->num_tx_queues; i++) {
		tsnep_stats.tx_packets += adapter->tx[i].packets;
		tsnep_stats.tx_bytes += adapter->tx[i].bytes;
		tsnep_stats.tx_dropped += adapter->tx[i].dropped;
	}
	memcpy(data, &tsnep_stats, sizeof(tsnep_stats));
	data += TSNEP_STATS_COUNT;

	for (i = 0; i < rx_count; i++) {
		memset(&tsnep_rx_queue_stats, 0, sizeof(tsnep_rx_queue_stats));
		tsnep_rx_queue_stats.rx_packets = adapter->rx[i].packets;
		tsnep_rx_queue_stats.rx_bytes = adapter->rx[i].bytes;
		tsnep_rx_queue_stats.rx_dropped = adapter->rx[i].dropped;
		tsnep_rx_queue_stats.rx_multicast = adapter->rx[i].multicast;
		tsnep_rx_queue_stats.rx_alloc_failed =
			adapter->rx[i].alloc_failed;
		reg = ioread32(adapter->addr + TSNEP_QUEUE(i) +
			       TSNEP_RX_STATISTIC);
		tsnep_rx_queue_stats.rx_no_descriptor_errors =
			(reg & TSNEP_RX_STATISTIC_NO_DESC_MASK) >>
			TSNEP_RX_STATISTIC_NO_DESC_SHIFT;
		tsnep_rx_queue_stats.rx_buffer_too_small_errors =
			(reg & TSNEP_RX_STATISTIC_BUFFER_TOO_SMALL_MASK) >>
			TSNEP_RX_STATISTIC_BUFFER_TOO_SMALL_SHIFT;
		tsnep_rx_queue_stats.rx_fifo_overflow_errors =
			(reg & TSNEP_RX_STATISTIC_FIFO_OVERFLOW_MASK) >>
			TSNEP_RX_STATISTIC_FIFO_OVERFLOW_SHIFT;
		tsnep_rx_queue_stats.rx_invalid_frame_errors =
			(reg & TSNEP_RX_STATISTIC_INVALID_FRAME_MASK) >>
			TSNEP_RX_STATISTIC_INVALID_FRAME_SHIFT;
		memcpy(data, &tsnep_rx_queue_stats,
		       sizeof(tsnep_rx_queue_stats));
		data += TSNEP_RX_QUEUE_STATS_COUNT;
	}

	for (i = 0; i < tx_count; i++) {
		memset(&tsnep_tx_queue_stats, 0, sizeof(tsnep_tx_queue_stats));
		tsnep_tx_queue_stats.tx_packets += adapter->tx[i].packets;
		tsnep_tx_queue_stats.tx_bytes += adapter->tx[i].bytes;
		tsnep_tx_queue_stats.tx_dropped += adapter->tx[i].dropped;
		memcpy(data, &tsnep_tx_queue_stats,
		       sizeof(tsnep_tx_queue_stats));
		data += TSNEP_TX_QUEUE_STATS_COUNT;
	}
}

static int tsnep_ethtool_get_sset_count(struct net_device *netdev, int sset)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);
	int rx_count;
	int tx_count;

	switch (sset) {
	case ETH_SS_STATS:
		rx_count = adapter->num_rx_queues;
		tx_count = adapter->num_tx_queues;
		return TSNEP_STATS_COUNT +
		       TSNEP_RX_QUEUE_STATS_COUNT * rx_count +
		       TSNEP_TX_QUEUE_STATS_COUNT * tx_count;
	case ETH_SS_TEST:
		return tsnep_ethtool_get_test_count();
	default:
		return -EOPNOTSUPP;
	}
}

static int tsnep_ethtool_get_rxnfc(struct net_device *netdev,
				   struct ethtool_rxnfc *cmd, u32 *rule_locs)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);

	switch (cmd->cmd) {
	case ETHTOOL_GRXRINGS:
		cmd->data = adapter->num_rx_queues;
		return 0;
	case ETHTOOL_GRXCLSRLCNT:
		cmd->rule_cnt = adapter->rxnfc_count;
		cmd->data = adapter->rxnfc_max;
		cmd->data |= RX_CLS_LOC_SPECIAL;
		return 0;
	case ETHTOOL_GRXCLSRULE:
		return tsnep_rxnfc_get_rule(adapter, cmd);
	case ETHTOOL_GRXCLSRLALL:
		return tsnep_rxnfc_get_all(adapter, cmd, rule_locs);
	default:
		return -EOPNOTSUPP;
	}
}

static int tsnep_ethtool_set_rxnfc(struct net_device *netdev,
				   struct ethtool_rxnfc *cmd)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);

	switch (cmd->cmd) {
	case ETHTOOL_SRXCLSRLINS:
		return tsnep_rxnfc_add_rule(adapter, cmd);
	case ETHTOOL_SRXCLSRLDEL:
		return tsnep_rxnfc_del_rule(adapter, cmd);
	default:
		return -EOPNOTSUPP;
	}
}

static void tsnep_ethtool_get_channels(struct net_device *netdev,
				       struct ethtool_channels *ch)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);

	ch->max_combined = adapter->num_queues;
	ch->combined_count = adapter->num_queues;
}

static int tsnep_ethtool_get_ts_info(struct net_device *netdev,
				     struct ethtool_ts_info *info)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);

	info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE |
				SOF_TIMESTAMPING_RX_SOFTWARE |
				SOF_TIMESTAMPING_SOFTWARE |
				SOF_TIMESTAMPING_TX_HARDWARE |
				SOF_TIMESTAMPING_RX_HARDWARE |
				SOF_TIMESTAMPING_RAW_HARDWARE;

	if (adapter->ptp_clock)
		info->phc_index = ptp_clock_index(adapter->ptp_clock);
	else
		info->phc_index = -1;

	info->tx_types = BIT(HWTSTAMP_TX_OFF) |
			 BIT(HWTSTAMP_TX_ON);
	info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) |
			   BIT(HWTSTAMP_FILTER_ALL);

	return 0;
}

static struct tsnep_queue *tsnep_get_queue_with_tx(struct tsnep_adapter *adapter,
						   int index)
{
	int i;

	for (i = 0; i < adapter->num_queues; i++) {
		if (adapter->queue[i].tx) {
			if (index == 0)
				return &adapter->queue[i];

			index--;
		}
	}

	return NULL;
}

static struct tsnep_queue *tsnep_get_queue_with_rx(struct tsnep_adapter *adapter,
						   int index)
{
	int i;

	for (i = 0; i < adapter->num_queues; i++) {
		if (adapter->queue[i].rx) {
			if (index == 0)
				return &adapter->queue[i];

			index--;
		}
	}

	return NULL;
}

static int tsnep_ethtool_get_coalesce(struct net_device *netdev,
				      struct ethtool_coalesce *ec,
				      struct kernel_ethtool_coalesce *kernel_coal,
				      struct netlink_ext_ack *extack)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);
	struct tsnep_queue *queue;

	queue = tsnep_get_queue_with_rx(adapter, 0);
	if (queue)
		ec->rx_coalesce_usecs = tsnep_get_irq_coalesce(queue);

	queue = tsnep_get_queue_with_tx(adapter, 0);
	if (queue)
		ec->tx_coalesce_usecs = tsnep_get_irq_coalesce(queue);

	return 0;
}

static int tsnep_ethtool_set_coalesce(struct net_device *netdev,
				      struct ethtool_coalesce *ec,
				      struct kernel_ethtool_coalesce *kernel_coal,
				      struct netlink_ext_ack *extack)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);
	int i;
	int retval;

	for (i = 0; i < adapter->num_queues; i++) {
		/* RX coalesce has priority for queues with TX and RX */
		if (adapter->queue[i].rx)
			retval = tsnep_set_irq_coalesce(&adapter->queue[i],
							ec->rx_coalesce_usecs);
		else
			retval = tsnep_set_irq_coalesce(&adapter->queue[i],
							ec->tx_coalesce_usecs);
		if (retval != 0)
			return retval;
	}

	return 0;
}

static int tsnep_ethtool_get_per_queue_coalesce(struct net_device *netdev,
						u32 queue,
						struct ethtool_coalesce *ec)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);
	struct tsnep_queue *queue_with_rx;
	struct tsnep_queue *queue_with_tx;

	if (queue >= max(adapter->num_tx_queues, adapter->num_rx_queues))
		return -EINVAL;

	queue_with_rx = tsnep_get_queue_with_rx(adapter, queue);
	if (queue_with_rx)
		ec->rx_coalesce_usecs = tsnep_get_irq_coalesce(queue_with_rx);

	queue_with_tx = tsnep_get_queue_with_tx(adapter, queue);
	if (queue_with_tx)
		ec->tx_coalesce_usecs = tsnep_get_irq_coalesce(queue_with_tx);

	return 0;
}

static int tsnep_ethtool_set_per_queue_coalesce(struct net_device *netdev,
						u32 queue,
						struct ethtool_coalesce *ec)
{
	struct tsnep_adapter *adapter = netdev_priv(netdev);
	struct tsnep_queue *queue_with_rx;
	struct tsnep_queue *queue_with_tx;
	int retval;

	if (queue >= max(adapter->num_tx_queues, adapter->num_rx_queues))
		return -EINVAL;

	queue_with_rx = tsnep_get_queue_with_rx(adapter, queue);
	if (queue_with_rx) {
		retval = tsnep_set_irq_coalesce(queue_with_rx, ec->rx_coalesce_usecs);
		if (retval != 0)
			return retval;
	}

	/* RX coalesce has priority for queues with TX and RX */
	queue_with_tx = tsnep_get_queue_with_tx(adapter, queue);
	if (queue_with_tx && !queue_with_tx->rx) {
		retval = tsnep_set_irq_coalesce(queue_with_tx, ec->tx_coalesce_usecs);
		if (retval != 0)
			return retval;
	}

	return 0;
}

const struct ethtool_ops tsnep_ethtool_ops = {
	.supported_coalesce_params = ETHTOOL_COALESCE_USECS,
	.get_drvinfo = tsnep_ethtool_get_drvinfo,
	.get_regs_len = tsnep_ethtool_get_regs_len,
	.get_regs = tsnep_ethtool_get_regs,
	.get_msglevel = tsnep_ethtool_get_msglevel,
	.set_msglevel = tsnep_ethtool_set_msglevel,
	.nway_reset = phy_ethtool_nway_reset,
	.get_link = ethtool_op_get_link,
	.self_test = tsnep_ethtool_self_test,
	.get_strings = tsnep_ethtool_get_strings,
	.get_ethtool_stats = tsnep_ethtool_get_ethtool_stats,
	.get_sset_count = tsnep_ethtool_get_sset_count,
	.get_rxnfc = tsnep_ethtool_get_rxnfc,
	.set_rxnfc = tsnep_ethtool_set_rxnfc,
	.get_channels = tsnep_ethtool_get_channels,
	.get_ts_info = tsnep_ethtool_get_ts_info,
	.get_coalesce = tsnep_ethtool_get_coalesce,
	.set_coalesce = tsnep_ethtool_set_coalesce,
	.get_per_queue_coalesce = tsnep_ethtool_get_per_queue_coalesce,
	.set_per_queue_coalesce = tsnep_ethtool_set_per_queue_coalesce,
	.get_link_ksettings = phy_ethtool_get_link_ksettings,
	.set_link_ksettings = phy_ethtool_set_link_ksettings,
};
