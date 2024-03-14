// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt)				"bcmasp_ethtool: " fmt

#include <asm-generic/unaligned.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>

#include "bcmasp.h"
#include "bcmasp_intf_defs.h"

enum bcmasp_stat_type {
	BCMASP_STAT_RX_EDPKT,
	BCMASP_STAT_RX_CTRL,
	BCMASP_STAT_RX_CTRL_PER_INTF,
	BCMASP_STAT_SOFT,
};

struct bcmasp_stats {
	char stat_string[ETH_GSTRING_LEN];
	enum bcmasp_stat_type type;
	u32 reg_offset;
};

#define STAT_BCMASP_SOFT_MIB(str) { \
	.stat_string = str, \
	.type = BCMASP_STAT_SOFT, \
}

#define STAT_BCMASP_OFFSET(str, _type, offset) { \
	.stat_string = str, \
	.type = _type, \
	.reg_offset = offset, \
}

#define STAT_BCMASP_RX_EDPKT(str, offset) \
	STAT_BCMASP_OFFSET(str, BCMASP_STAT_RX_EDPKT, offset)
#define STAT_BCMASP_RX_CTRL(str, offset) \
	STAT_BCMASP_OFFSET(str, BCMASP_STAT_RX_CTRL, offset)
#define STAT_BCMASP_RX_CTRL_PER_INTF(str, offset) \
	STAT_BCMASP_OFFSET(str, BCMASP_STAT_RX_CTRL_PER_INTF, offset)

/* Must match the order of struct bcmasp_mib_counters */
static const struct bcmasp_stats bcmasp_gstrings_stats[] = {
	/* EDPKT counters */
	STAT_BCMASP_RX_EDPKT("RX Time Stamp", ASP_EDPKT_RX_TS_COUNTER),
	STAT_BCMASP_RX_EDPKT("RX PKT Count", ASP_EDPKT_RX_PKT_CNT),
	STAT_BCMASP_RX_EDPKT("RX PKT Buffered", ASP_EDPKT_HDR_EXTR_CNT),
	STAT_BCMASP_RX_EDPKT("RX PKT Pushed to DRAM", ASP_EDPKT_HDR_OUT_CNT),
	/* ASP RX control */
	STAT_BCMASP_RX_CTRL_PER_INTF("Frames From Unimac",
				     ASP_RX_CTRL_UMAC_0_FRAME_COUNT),
	STAT_BCMASP_RX_CTRL_PER_INTF("Frames From Port",
				     ASP_RX_CTRL_FB_0_FRAME_COUNT),
	STAT_BCMASP_RX_CTRL_PER_INTF("RX Buffer FIFO Depth",
				     ASP_RX_CTRL_FB_RX_FIFO_DEPTH),
	STAT_BCMASP_RX_CTRL("Frames Out(Buffer)",
			    ASP_RX_CTRL_FB_OUT_FRAME_COUNT),
	STAT_BCMASP_RX_CTRL("Frames Out(Filters)",
			    ASP_RX_CTRL_FB_FILT_OUT_FRAME_COUNT),
	/* Software maintained statistics */
	STAT_BCMASP_SOFT_MIB("RX SKB Alloc Failed"),
	STAT_BCMASP_SOFT_MIB("TX DMA Failed"),
	STAT_BCMASP_SOFT_MIB("Multicast Filters Full"),
	STAT_BCMASP_SOFT_MIB("Unicast Filters Full"),
	STAT_BCMASP_SOFT_MIB("MDA Filters Combined"),
	STAT_BCMASP_SOFT_MIB("Promisc Filter Set"),
	STAT_BCMASP_SOFT_MIB("TX Realloc For Offload Failed"),
	STAT_BCMASP_SOFT_MIB("Tx Timeout Count"),
};

#define BCMASP_STATS_LEN	ARRAY_SIZE(bcmasp_gstrings_stats)

static u16 bcmasp_stat_fixup_offset(struct bcmasp_intf *intf,
				    const struct bcmasp_stats *s)
{
	struct bcmasp_priv *priv = intf->parent;

	if (!strcmp("Frames Out(Buffer)", s->stat_string))
		return priv->hw_info->rx_ctrl_fb_out_frame_count;

	if (!strcmp("Frames Out(Filters)", s->stat_string))
		return priv->hw_info->rx_ctrl_fb_filt_out_frame_count;

	if (!strcmp("RX Buffer FIFO Depth", s->stat_string))
		return priv->hw_info->rx_ctrl_fb_rx_fifo_depth;

	return s->reg_offset;
}

static int bcmasp_get_sset_count(struct net_device *dev, int string_set)
{
	switch (string_set) {
	case ETH_SS_STATS:
		return BCMASP_STATS_LEN;
	default:
		return -EOPNOTSUPP;
	}
}

static void bcmasp_get_strings(struct net_device *dev, u32 stringset,
			       u8 *data)
{
	unsigned int i;

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < BCMASP_STATS_LEN; i++) {
			memcpy(data + i * ETH_GSTRING_LEN,
			       bcmasp_gstrings_stats[i].stat_string,
			       ETH_GSTRING_LEN);
		}
		break;
	default:
		return;
	}
}

static void bcmasp_update_mib_counters(struct bcmasp_intf *intf)
{
	unsigned int i;

	for (i = 0; i < BCMASP_STATS_LEN; i++) {
		const struct bcmasp_stats *s;
		u32 offset, val;
		char *p;

		s = &bcmasp_gstrings_stats[i];
		offset = bcmasp_stat_fixup_offset(intf, s);
		switch (s->type) {
		case BCMASP_STAT_SOFT:
			continue;
		case BCMASP_STAT_RX_EDPKT:
			val = rx_edpkt_core_rl(intf->parent, offset);
			break;
		case BCMASP_STAT_RX_CTRL:
			val = rx_ctrl_core_rl(intf->parent, offset);
			break;
		case BCMASP_STAT_RX_CTRL_PER_INTF:
			offset += sizeof(u32) * intf->port;
			val = rx_ctrl_core_rl(intf->parent, offset);
			break;
		default:
			continue;
		}
		p = (char *)(&intf->mib) + (i * sizeof(u32));
		put_unaligned(val, (u32 *)p);
	}
}

static void bcmasp_get_ethtool_stats(struct net_device *dev,
				     struct ethtool_stats *stats,
				     u64 *data)
{
	struct bcmasp_intf *intf = netdev_priv(dev);
	unsigned int i;
	char *p;

	if (netif_running(dev))
		bcmasp_update_mib_counters(intf);

	for (i = 0; i < BCMASP_STATS_LEN; i++) {
		p = (char *)(&intf->mib) + (i * sizeof(u32));
		data[i] = *(u32 *)p;
	}
}

static void bcmasp_get_drvinfo(struct net_device *dev,
			       struct ethtool_drvinfo *info)
{
	strscpy(info->driver, "bcmasp", sizeof(info->driver));
	strscpy(info->bus_info, dev_name(dev->dev.parent),
		sizeof(info->bus_info));
}

static u32 bcmasp_get_msglevel(struct net_device *dev)
{
	struct bcmasp_intf *intf = netdev_priv(dev);

	return intf->msg_enable;
}

static void bcmasp_set_msglevel(struct net_device *dev, u32 level)
{
	struct bcmasp_intf *intf = netdev_priv(dev);

	intf->msg_enable = level;
}

#define BCMASP_SUPPORTED_WAKE   (WAKE_MAGIC | WAKE_MAGICSECURE | WAKE_FILTER)
static void bcmasp_get_wol(struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct bcmasp_intf *intf = netdev_priv(dev);

	wol->supported = BCMASP_SUPPORTED_WAKE;
	wol->wolopts = intf->wolopts;
	memset(wol->sopass, 0, sizeof(wol->sopass));

	if (wol->wolopts & WAKE_MAGICSECURE)
		memcpy(wol->sopass, intf->sopass, sizeof(intf->sopass));
}

static int bcmasp_set_wol(struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct bcmasp_intf *intf = netdev_priv(dev);
	struct bcmasp_priv *priv = intf->parent;
	struct device *kdev = &priv->pdev->dev;

	if (!device_can_wakeup(kdev))
		return -EOPNOTSUPP;

	/* Interface Specific */
	intf->wolopts = wol->wolopts;
	if (intf->wolopts & WAKE_MAGICSECURE)
		memcpy(intf->sopass, wol->sopass, sizeof(wol->sopass));

	mutex_lock(&priv->wol_lock);
	priv->enable_wol(intf, !!intf->wolopts);
	mutex_unlock(&priv->wol_lock);

	return 0;
}

static int bcmasp_flow_insert(struct net_device *dev, struct ethtool_rxnfc *cmd)
{
	struct bcmasp_intf *intf = netdev_priv(dev);
	struct bcmasp_net_filter *nfilter;
	u32 loc = cmd->fs.location;
	bool wake = false;

	if (cmd->fs.ring_cookie == RX_CLS_FLOW_WAKE)
		wake = true;

	/* Currently only supports WAKE filters */
	if (!wake)
		return -EOPNOTSUPP;

	switch (cmd->fs.flow_type & ~(FLOW_EXT | FLOW_MAC_EXT)) {
	case ETHER_FLOW:
	case IP_USER_FLOW:
	case TCP_V4_FLOW:
	case UDP_V4_FLOW:
	case TCP_V6_FLOW:
	case UDP_V6_FLOW:
		break;
	default:
		return -EOPNOTSUPP;
	}

	/* Check if filter already exists */
	if (bcmasp_netfilt_check_dup(intf, &cmd->fs))
		return -EINVAL;

	nfilter = bcmasp_netfilt_get_init(intf, loc, wake, true);
	if (IS_ERR(nfilter))
		return PTR_ERR(nfilter);

	/* Return the location where we did insert the filter */
	cmd->fs.location = nfilter->hw_index;
	memcpy(&nfilter->fs, &cmd->fs, sizeof(struct ethtool_rx_flow_spec));

	/* Since we only support wake filters, defer register programming till
	 * suspend time.
	 */
	return 0;
}

static int bcmasp_flow_delete(struct net_device *dev, struct ethtool_rxnfc *cmd)
{
	struct bcmasp_intf *intf = netdev_priv(dev);
	struct bcmasp_net_filter *nfilter;

	nfilter = bcmasp_netfilt_get_init(intf, cmd->fs.location, false, false);
	if (IS_ERR(nfilter))
		return PTR_ERR(nfilter);

	bcmasp_netfilt_release(intf, nfilter);

	return 0;
}

static int bcmasp_flow_get(struct bcmasp_intf *intf, struct ethtool_rxnfc *cmd)
{
	struct bcmasp_net_filter *nfilter;

	nfilter = bcmasp_netfilt_get_init(intf, cmd->fs.location, false, false);
	if (IS_ERR(nfilter))
		return PTR_ERR(nfilter);

	memcpy(&cmd->fs, &nfilter->fs, sizeof(nfilter->fs));

	cmd->data = NUM_NET_FILTERS;

	return 0;
}

static int bcmasp_set_rxnfc(struct net_device *dev, struct ethtool_rxnfc *cmd)
{
	struct bcmasp_intf *intf = netdev_priv(dev);
	int ret = -EOPNOTSUPP;

	mutex_lock(&intf->parent->net_lock);

	switch (cmd->cmd) {
	case ETHTOOL_SRXCLSRLINS:
		ret = bcmasp_flow_insert(dev, cmd);
		break;
	case ETHTOOL_SRXCLSRLDEL:
		ret = bcmasp_flow_delete(dev, cmd);
		break;
	default:
		break;
	}

	mutex_unlock(&intf->parent->net_lock);

	return ret;
}

static int bcmasp_get_rxnfc(struct net_device *dev, struct ethtool_rxnfc *cmd,
			    u32 *rule_locs)
{
	struct bcmasp_intf *intf = netdev_priv(dev);
	int err = 0;

	mutex_lock(&intf->parent->net_lock);

	switch (cmd->cmd) {
	case ETHTOOL_GRXCLSRLCNT:
		cmd->rule_cnt = bcmasp_netfilt_get_active(intf);
		/* We support specifying rule locations */
		cmd->data |= RX_CLS_LOC_SPECIAL;
		break;
	case ETHTOOL_GRXCLSRULE:
		err = bcmasp_flow_get(intf, cmd);
		break;
	case ETHTOOL_GRXCLSRLALL:
		err = bcmasp_netfilt_get_all_active(intf, rule_locs, &cmd->rule_cnt);
		cmd->data = NUM_NET_FILTERS;
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	mutex_unlock(&intf->parent->net_lock);

	return err;
}

void bcmasp_eee_enable_set(struct bcmasp_intf *intf, bool enable)
{
	u32 reg;

	reg = umac_rl(intf, UMC_EEE_CTRL);
	if (enable)
		reg |= EEE_EN;
	else
		reg &= ~EEE_EN;
	umac_wl(intf, reg, UMC_EEE_CTRL);

	intf->eee.eee_enabled = enable;
	intf->eee.eee_active = enable;
}

static int bcmasp_get_eee(struct net_device *dev, struct ethtool_eee *e)
{
	struct bcmasp_intf *intf = netdev_priv(dev);
	struct ethtool_eee *p = &intf->eee;

	if (!dev->phydev)
		return -ENODEV;

	e->eee_enabled = p->eee_enabled;
	e->eee_active = p->eee_active;
	e->tx_lpi_enabled = p->tx_lpi_enabled;
	e->tx_lpi_timer = umac_rl(intf, UMC_EEE_LPI_TIMER);

	return phy_ethtool_get_eee(dev->phydev, e);
}

static int bcmasp_set_eee(struct net_device *dev, struct ethtool_eee *e)
{
	struct bcmasp_intf *intf = netdev_priv(dev);
	struct ethtool_eee *p = &intf->eee;
	int ret;

	if (!dev->phydev)
		return -ENODEV;

	if (!p->eee_enabled) {
		bcmasp_eee_enable_set(intf, false);
	} else {
		ret = phy_init_eee(dev->phydev, 0);
		if (ret) {
			netif_err(intf, hw, dev,
				  "EEE initialization failed: %d\n", ret);
			return ret;
		}

		umac_wl(intf, e->tx_lpi_timer, UMC_EEE_LPI_TIMER);
		intf->eee.eee_active = ret >= 0;
		intf->eee.tx_lpi_enabled = e->tx_lpi_enabled;
		bcmasp_eee_enable_set(intf, true);
	}

	return phy_ethtool_set_eee(dev->phydev, e);
}

static void bcmasp_get_eth_mac_stats(struct net_device *dev,
				     struct ethtool_eth_mac_stats *mac_stats)
{
	struct bcmasp_intf *intf = netdev_priv(dev);

	mac_stats->FramesTransmittedOK = umac_rl(intf, UMC_GTPOK);
	mac_stats->SingleCollisionFrames = umac_rl(intf, UMC_GTSCL);
	mac_stats->MultipleCollisionFrames = umac_rl(intf, UMC_GTMCL);
	mac_stats->FramesReceivedOK = umac_rl(intf, UMC_GRPOK);
	mac_stats->FrameCheckSequenceErrors = umac_rl(intf, UMC_GRFCS);
	mac_stats->AlignmentErrors = umac_rl(intf, UMC_GRALN);
	mac_stats->OctetsTransmittedOK = umac_rl(intf, UMC_GTBYT);
	mac_stats->FramesWithDeferredXmissions = umac_rl(intf, UMC_GTDRF);
	mac_stats->LateCollisions = umac_rl(intf, UMC_GTLCL);
	mac_stats->FramesAbortedDueToXSColls = umac_rl(intf, UMC_GTXCL);
	mac_stats->OctetsReceivedOK = umac_rl(intf, UMC_GRBYT);
	mac_stats->MulticastFramesXmittedOK = umac_rl(intf, UMC_GTMCA);
	mac_stats->BroadcastFramesXmittedOK = umac_rl(intf, UMC_GTBCA);
	mac_stats->FramesWithExcessiveDeferral = umac_rl(intf, UMC_GTEDF);
	mac_stats->MulticastFramesReceivedOK = umac_rl(intf, UMC_GRMCA);
	mac_stats->BroadcastFramesReceivedOK = umac_rl(intf, UMC_GRBCA);
}

static const struct ethtool_rmon_hist_range bcmasp_rmon_ranges[] = {
	{    0,   64},
	{   65,  127},
	{  128,  255},
	{  256,  511},
	{  512, 1023},
	{ 1024, 1518},
	{ 1519, 1522},
	{}
};

static void bcmasp_get_rmon_stats(struct net_device *dev,
				  struct ethtool_rmon_stats *rmon_stats,
				  const struct ethtool_rmon_hist_range **ranges)
{
	struct bcmasp_intf *intf = netdev_priv(dev);

	*ranges = bcmasp_rmon_ranges;

	rmon_stats->undersize_pkts = umac_rl(intf, UMC_RRUND);
	rmon_stats->oversize_pkts = umac_rl(intf, UMC_GROVR);
	rmon_stats->fragments = umac_rl(intf, UMC_RRFRG);
	rmon_stats->jabbers = umac_rl(intf, UMC_GRJBR);

	rmon_stats->hist[0] = umac_rl(intf, UMC_GR64);
	rmon_stats->hist[1] = umac_rl(intf, UMC_GR127);
	rmon_stats->hist[2] = umac_rl(intf, UMC_GR255);
	rmon_stats->hist[3] = umac_rl(intf, UMC_GR511);
	rmon_stats->hist[4] = umac_rl(intf, UMC_GR1023);
	rmon_stats->hist[5] = umac_rl(intf, UMC_GR1518);
	rmon_stats->hist[6] = umac_rl(intf, UMC_GRMGV);

	rmon_stats->hist_tx[0] = umac_rl(intf, UMC_TR64);
	rmon_stats->hist_tx[1] = umac_rl(intf, UMC_TR127);
	rmon_stats->hist_tx[2] = umac_rl(intf, UMC_TR255);
	rmon_stats->hist_tx[3] = umac_rl(intf, UMC_TR511);
	rmon_stats->hist_tx[4] = umac_rl(intf, UMC_TR1023);
	rmon_stats->hist_tx[5] = umac_rl(intf, UMC_TR1518);
	rmon_stats->hist_tx[6] = umac_rl(intf, UMC_TRMGV);
}

static void bcmasp_get_eth_ctrl_stats(struct net_device *dev,
				      struct ethtool_eth_ctrl_stats *ctrl_stats)
{
	struct bcmasp_intf *intf = netdev_priv(dev);

	ctrl_stats->MACControlFramesTransmitted = umac_rl(intf, UMC_GTXCF);
	ctrl_stats->MACControlFramesReceived = umac_rl(intf, UMC_GRXCF);
	ctrl_stats->UnsupportedOpcodesReceived = umac_rl(intf, UMC_GRXUO);
}

const struct ethtool_ops bcmasp_ethtool_ops = {
	.get_drvinfo		= bcmasp_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_link_ksettings	= phy_ethtool_get_link_ksettings,
	.set_link_ksettings	= phy_ethtool_set_link_ksettings,
	.get_msglevel		= bcmasp_get_msglevel,
	.set_msglevel		= bcmasp_set_msglevel,
	.get_wol		= bcmasp_get_wol,
	.set_wol		= bcmasp_set_wol,
	.get_rxnfc		= bcmasp_get_rxnfc,
	.set_rxnfc		= bcmasp_set_rxnfc,
	.set_eee		= bcmasp_set_eee,
	.get_eee		= bcmasp_get_eee,
	.get_eth_mac_stats	= bcmasp_get_eth_mac_stats,
	.get_rmon_stats		= bcmasp_get_rmon_stats,
	.get_eth_ctrl_stats	= bcmasp_get_eth_ctrl_stats,
	.get_strings		= bcmasp_get_strings,
	.get_ethtool_stats	= bcmasp_get_ethtool_stats,
	.get_sset_count		= bcmasp_get_sset_count,
};
