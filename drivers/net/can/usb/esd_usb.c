// SPDX-License-Identifier: GPL-2.0-only
/*
 * CAN driver for esd electronics gmbh CAN-USB/2, CAN-USB/3 and CAN-USB/Micro
 *
 * Copyright (C) 2010-2012 esd electronic system design gmbh, Matthias Fuchs <socketcan@esd.eu>
 * Copyright (C) 2022-2023 esd electronics gmbh, Frank Jungclaus <frank.jungclaus@esd.eu>
 */

#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/ethtool.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/units.h>
#include <linux/usb.h>

MODULE_AUTHOR("Matthias Fuchs <socketcan@esd.eu>");
MODULE_AUTHOR("Frank Jungclaus <frank.jungclaus@esd.eu>");
MODULE_DESCRIPTION("CAN driver for esd electronics gmbh CAN-USB/2, CAN-USB/3 and CAN-USB/Micro interfaces");
MODULE_LICENSE("GPL v2");

/* USB vendor and product ID */
#define ESD_USB_ESDGMBH_VENDOR_ID	0x0ab4
#define ESD_USB_CANUSB2_PRODUCT_ID	0x0010
#define ESD_USB_CANUSBM_PRODUCT_ID	0x0011
#define ESD_USB_CANUSB3_PRODUCT_ID	0x0014

/* CAN controller clock frequencies */
#define ESD_USB_2_CAN_CLOCK	(60 * MEGA) /* Hz */
#define ESD_USB_M_CAN_CLOCK	(36 * MEGA) /* Hz */
#define ESD_USB_3_CAN_CLOCK	(80 * MEGA) /* Hz */

/* Maximum number of CAN nets */
#define ESD_USB_MAX_NETS	2

/* USB commands */
#define ESD_USB_CMD_VERSION		1 /* also used for VERSION_REPLY */
#define ESD_USB_CMD_CAN_RX		2 /* device to host only */
#define ESD_USB_CMD_CAN_TX		3 /* also used for TX_DONE */
#define ESD_USB_CMD_SETBAUD		4 /* also used for SETBAUD_REPLY */
#define ESD_USB_CMD_TS			5 /* also used for TS_REPLY */
#define ESD_USB_CMD_IDADD		6 /* also used for IDADD_REPLY */

/* esd CAN message flags - dlc field */
#define ESD_USB_RTR	BIT(4)
#define ESD_USB_NO_BRS	BIT(4)
#define ESD_USB_ESI	BIT(5)
#define ESD_USB_FD	BIT(7)

/* esd CAN message flags - id field */
#define ESD_USB_EXTID	BIT(29)
#define ESD_USB_EVENT	BIT(30)
#define ESD_USB_IDMASK	GENMASK(28, 0)

/* esd CAN event ids */
#define ESD_USB_EV_CAN_ERROR_EXT	2 /* CAN controller specific diagnostic data */

/* baudrate message flags */
#define ESD_USB_LOM	BIT(30) /* Listen Only Mode */
#define ESD_USB_UBR	BIT(31) /* User Bit Rate (controller BTR) in bits 0..27 */
#define ESD_USB_NO_BAUDRATE	GENMASK(30, 0) /* bit rate unconfigured */

/* bit timing esd CAN-USB */
#define ESD_USB_2_TSEG1_SHIFT	16
#define ESD_USB_2_TSEG2_SHIFT	20
#define ESD_USB_2_SJW_SHIFT	14
#define ESD_USB_M_SJW_SHIFT	24
#define ESD_USB_TRIPLE_SAMPLES	BIT(23)

/* Transmitter Delay Compensation */
#define ESD_USB_3_TDC_MODE_AUTO	0

/* esd IDADD message */
#define ESD_USB_ID_ENABLE	BIT(7)
#define ESD_USB_MAX_ID_SEGMENT	64

/* SJA1000 ECC register (emulated by usb firmware) */
#define ESD_USB_SJA1000_ECC_SEG		GENMASK(4, 0)
#define ESD_USB_SJA1000_ECC_DIR		BIT(5)
#define ESD_USB_SJA1000_ECC_ERR		BIT(2, 1)
#define ESD_USB_SJA1000_ECC_BIT		0x00
#define ESD_USB_SJA1000_ECC_FORM	BIT(6)
#define ESD_USB_SJA1000_ECC_STUFF	BIT(7)
#define ESD_USB_SJA1000_ECC_MASK	GENMASK(7, 6)

/* esd bus state event codes */
#define ESD_USB_BUSSTATE_MASK	GENMASK(7, 6)
#define ESD_USB_BUSSTATE_WARN	BIT(6)
#define ESD_USB_BUSSTATE_ERRPASSIVE	BIT(7)
#define ESD_USB_BUSSTATE_BUSOFF	GENMASK(7, 6)

#define ESD_USB_RX_BUFFER_SIZE		1024
#define ESD_USB_MAX_RX_URBS		4
#define ESD_USB_MAX_TX_URBS		16 /* must be power of 2 */

/* Modes for CAN-USB/3, to be used for esd_usb_3_set_baudrate_msg_x.mode */
#define ESD_USB_3_BAUDRATE_MODE_DISABLE		0 /* remove from bus */
#define ESD_USB_3_BAUDRATE_MODE_INDEX		1 /* ESD (CiA) bit rate idx */
#define ESD_USB_3_BAUDRATE_MODE_BTR_CTRL	2 /* BTR values (controller)*/
#define ESD_USB_3_BAUDRATE_MODE_BTR_CANONICAL	3 /* BTR values (canonical) */
#define ESD_USB_3_BAUDRATE_MODE_NUM		4 /* numerical bit rate */
#define ESD_USB_3_BAUDRATE_MODE_AUTOBAUD	5 /* autobaud */

/* Flags for CAN-USB/3, to be used for esd_usb_3_set_baudrate_msg_x.flags */
#define ESD_USB_3_BAUDRATE_FLAG_FD	BIT(0) /* enable CAN FD mode */
#define ESD_USB_3_BAUDRATE_FLAG_LOM	BIT(1) /* enable listen only mode */
#define ESD_USB_3_BAUDRATE_FLAG_STM	BIT(2) /* enable self test mode */
#define ESD_USB_3_BAUDRATE_FLAG_TRS	BIT(3) /* enable triple sampling */
#define ESD_USB_3_BAUDRATE_FLAG_TXP	BIT(4) /* enable transmit pause */

struct esd_usb_header_msg {
	u8 len; /* total message length in 32bit words */
	u8 cmd;
	u8 rsvd[2];
};

struct esd_usb_version_msg {
	u8 len; /* total message length in 32bit words */
	u8 cmd;
	u8 rsvd;
	u8 flags;
	__le32 drv_version;
};

struct esd_usb_version_reply_msg {
	u8 len; /* total message length in 32bit words */
	u8 cmd;
	u8 nets;
	u8 features;
	__le32 version;
	u8 name[16];
	__le32 rsvd;
	__le32 ts;
};

struct esd_usb_rx_msg {
	u8 len; /* total message length in 32bit words */
	u8 cmd;
	u8 net;
	u8 dlc;
	__le32 ts;
	__le32 id; /* upper 3 bits contain flags */
	union {
		u8 data[CAN_MAX_DLEN];
		u8 data_fd[CANFD_MAX_DLEN];
		struct {
			u8 status; /* CAN Controller Status */
			u8 ecc;    /* Error Capture Register */
			u8 rec;    /* RX Error Counter */
			u8 tec;    /* TX Error Counter */
		} ev_can_err_ext;  /* For ESD_EV_CAN_ERROR_EXT */
	};
};

struct esd_usb_tx_msg {
	u8 len; /* total message length in 32bit words */
	u8 cmd;
	u8 net;
	u8 dlc;
	u32 hnd;	/* opaque handle, not used by device */
	__le32 id; /* upper 3 bits contain flags */
	union {
		u8 data[CAN_MAX_DLEN];
		u8 data_fd[CANFD_MAX_DLEN];
	};
};

struct esd_usb_tx_done_msg {
	u8 len; /* total message length in 32bit words */
	u8 cmd;
	u8 net;
	u8 status;
	u32 hnd;	/* opaque handle, not used by device */
	__le32 ts;
};

struct esd_usb_id_filter_msg {
	u8 len; /* total message length in 32bit words */
	u8 cmd;
	u8 net;
	u8 option;
	__le32 mask[ESD_USB_MAX_ID_SEGMENT + 1]; /* +1 for 29bit extended IDs */
};

struct esd_usb_set_baudrate_msg {
	u8 len; /* total message length in 32bit words */
	u8 cmd;
	u8 net;
	u8 rsvd;
	__le32 baud;
};

/* CAN-USB/3 baudrate configuration, used for nominal as well as for data bit rate */
struct esd_usb_3_baudrate_cfg {
	__le16 brp;	/* bit rate pre-scaler */
	__le16 tseg1;	/* time segment before sample point */
	__le16 tseg2;	/* time segment after sample point */
	__le16 sjw;	/* synchronization jump Width */
};

/* In principle, the esd CAN-USB/3 supports Transmitter Delay Compensation (TDC),
 * but currently only the automatic TDC mode is supported by this driver.
 * An implementation for manual TDC configuration will follow.
 *
 * For information about struct esd_usb_3_tdc_cfg, see
 * NTCAN Application Developers Manual, 6.2.25 NTCAN_TDC_CFG + related chapters
 * https://esd.eu/fileadmin/esd/docs/manuals/NTCAN_Part1_Function_API_Manual_en_56.pdf
 */
struct esd_usb_3_tdc_cfg {
	u8 tdc_mode;	/* transmitter delay compensation mode  */
	u8 ssp_offset;	/* secondary sample point offset in mtq */
	s8 ssp_shift;	/* secondary sample point shift in mtq */
	u8 tdc_filter;	/* TDC filter in mtq */
};

/* Extended version of the above set_baudrate_msg for a CAN-USB/3
 * to define the CAN bit timing configuration of the CAN controller in
 * CAN FD mode as well as in Classical CAN mode.
 *
 * The payload of this command is a NTCAN_BAUDRATE_X structure according to
 * esd electronics gmbh, NTCAN Application Developers Manual, 6.2.15 NTCAN_BAUDRATE_X
 * https://esd.eu/fileadmin/esd/docs/manuals/NTCAN_Part1_Function_API_Manual_en_56.pdf
 */
struct esd_usb_3_set_baudrate_msg_x {
	u8 len;	/* total message length in 32bit words */
	u8 cmd;
	u8 net;
	u8 rsvd;	/*reserved */
	/* Payload ... */
	__le16 mode;	/* mode word, see ESD_USB_3_BAUDRATE_MODE_xxx */
	__le16 flags;	/* control flags, see ESD_USB_3_BAUDRATE_FLAG_xxx */
	struct esd_usb_3_tdc_cfg tdc;	/* TDC configuration */
	struct esd_usb_3_baudrate_cfg nom;	/* nominal bit rate */
	struct esd_usb_3_baudrate_cfg data;	/* data bit rate */
};

/* Main message type used between library and application */
union __packed esd_usb_msg {
	struct esd_usb_header_msg hdr;
	struct esd_usb_version_msg version;
	struct esd_usb_version_reply_msg version_reply;
	struct esd_usb_rx_msg rx;
	struct esd_usb_tx_msg tx;
	struct esd_usb_tx_done_msg txdone;
	struct esd_usb_set_baudrate_msg setbaud;
	struct esd_usb_3_set_baudrate_msg_x setbaud_x;
	struct esd_usb_id_filter_msg filter;
};

static struct usb_device_id esd_usb_table[] = {
	{USB_DEVICE(ESD_USB_ESDGMBH_VENDOR_ID, ESD_USB_CANUSB2_PRODUCT_ID)},
	{USB_DEVICE(ESD_USB_ESDGMBH_VENDOR_ID, ESD_USB_CANUSBM_PRODUCT_ID)},
	{USB_DEVICE(ESD_USB_ESDGMBH_VENDOR_ID, ESD_USB_CANUSB3_PRODUCT_ID)},
	{}
};
MODULE_DEVICE_TABLE(usb, esd_usb_table);

struct esd_usb_net_priv;

struct esd_tx_urb_context {
	struct esd_usb_net_priv *priv;
	u32 echo_index;
};

struct esd_usb {
	struct usb_device *udev;
	struct esd_usb_net_priv *nets[ESD_USB_MAX_NETS];

	struct usb_anchor rx_submitted;

	int net_count;
	u32 version;
	int rxinitdone;
	void *rxbuf[ESD_USB_MAX_RX_URBS];
	dma_addr_t rxbuf_dma[ESD_USB_MAX_RX_URBS];
};

struct esd_usb_net_priv {
	struct can_priv can; /* must be the first member */

	atomic_t active_tx_jobs;
	struct usb_anchor tx_submitted;
	struct esd_tx_urb_context tx_contexts[ESD_USB_MAX_TX_URBS];

	struct esd_usb *usb;
	struct net_device *netdev;
	int index;
	u8 old_state;
	struct can_berr_counter bec;
};

static void esd_usb_rx_event(struct esd_usb_net_priv *priv,
			     union esd_usb_msg *msg)
{
	struct net_device_stats *stats = &priv->netdev->stats;
	struct can_frame *cf;
	struct sk_buff *skb;
	u32 id = le32_to_cpu(msg->rx.id) & ESD_USB_IDMASK;

	if (id == ESD_USB_EV_CAN_ERROR_EXT) {
		u8 state = msg->rx.ev_can_err_ext.status;
		u8 ecc = msg->rx.ev_can_err_ext.ecc;

		priv->bec.rxerr = msg->rx.ev_can_err_ext.rec;
		priv->bec.txerr = msg->rx.ev_can_err_ext.tec;

		netdev_dbg(priv->netdev,
			   "CAN_ERR_EV_EXT: dlc=%#02x state=%02x ecc=%02x rec=%02x tec=%02x\n",
			   msg->rx.dlc, state, ecc,
			   priv->bec.rxerr, priv->bec.txerr);

		/* if berr-reporting is off, only pass through on state change ... */
		if (!(priv->can.ctrlmode & CAN_CTRLMODE_BERR_REPORTING) &&
		    state == priv->old_state)
			return;

		skb = alloc_can_err_skb(priv->netdev, &cf);
		if (!skb)
			stats->rx_dropped++;

		if (state != priv->old_state) {
			enum can_state tx_state, rx_state;
			enum can_state new_state = CAN_STATE_ERROR_ACTIVE;

			priv->old_state = state;

			switch (state & ESD_USB_BUSSTATE_MASK) {
			case ESD_USB_BUSSTATE_BUSOFF:
				new_state = CAN_STATE_BUS_OFF;
				can_bus_off(priv->netdev);
				break;
			case ESD_USB_BUSSTATE_WARN:
				new_state = CAN_STATE_ERROR_WARNING;
				break;
			case ESD_USB_BUSSTATE_ERRPASSIVE:
				new_state = CAN_STATE_ERROR_PASSIVE;
				break;
			default:
				new_state = CAN_STATE_ERROR_ACTIVE;
				priv->bec.txerr = 0;
				priv->bec.rxerr = 0;
				break;
			}

			if (new_state != priv->can.state) {
				tx_state = (priv->bec.txerr >= priv->bec.rxerr) ? new_state : 0;
				rx_state = (priv->bec.txerr <= priv->bec.rxerr) ? new_state : 0;
				can_change_state(priv->netdev, cf,
						 tx_state, rx_state);
			}
		} else if (skb) {
			priv->can.can_stats.bus_error++;
			stats->rx_errors++;

			cf->can_id |= CAN_ERR_PROT | CAN_ERR_BUSERROR;

			switch (ecc & ESD_USB_SJA1000_ECC_MASK) {
			case ESD_USB_SJA1000_ECC_BIT:
				cf->data[2] |= CAN_ERR_PROT_BIT;
				break;
			case ESD_USB_SJA1000_ECC_FORM:
				cf->data[2] |= CAN_ERR_PROT_FORM;
				break;
			case ESD_USB_SJA1000_ECC_STUFF:
				cf->data[2] |= CAN_ERR_PROT_STUFF;
				break;
			default:
				break;
			}

			/* Error occurred during transmission? */
			if (!(ecc & ESD_USB_SJA1000_ECC_DIR))
				cf->data[2] |= CAN_ERR_PROT_TX;

			/* Bit stream position in CAN frame as the error was detected */
			cf->data[3] = ecc & ESD_USB_SJA1000_ECC_SEG;
		}

		if (skb) {
			cf->can_id |= CAN_ERR_CNT;
			cf->data[6] = priv->bec.txerr;
			cf->data[7] = priv->bec.rxerr;

			netif_rx(skb);
		}
	}
}

static void esd_usb_rx_can_msg(struct esd_usb_net_priv *priv,
			       union esd_usb_msg *msg)
{
	struct net_device_stats *stats = &priv->netdev->stats;
	struct can_frame *cf;
	struct canfd_frame *cfd;
	struct sk_buff *skb;
	u32 id;
	u8 len;

	if (!netif_device_present(priv->netdev))
		return;

	id = le32_to_cpu(msg->rx.id);

	if (id & ESD_USB_EVENT) {
		esd_usb_rx_event(priv, msg);
	} else {
		if (msg->rx.dlc & ESD_USB_FD) {
			skb = alloc_canfd_skb(priv->netdev, &cfd);
		} else {
			skb = alloc_can_skb(priv->netdev, &cf);
			cfd = (struct canfd_frame *)cf;
		}

		if (skb == NULL) {
			stats->rx_dropped++;
			return;
		}

		cfd->can_id = id & ESD_USB_IDMASK;

		if (msg->rx.dlc & ESD_USB_FD) {
			/* masking by 0x0F is already done within can_fd_dlc2len() */
			cfd->len = can_fd_dlc2len(msg->rx.dlc);
			len = cfd->len;
			if ((msg->rx.dlc & ESD_USB_NO_BRS) == 0)
				cfd->flags |= CANFD_BRS;
			if (msg->rx.dlc & ESD_USB_ESI)
				cfd->flags |= CANFD_ESI;
		} else {
			can_frame_set_cc_len(cf, msg->rx.dlc & ~ESD_USB_RTR, priv->can.ctrlmode);
			len = cf->len;
			if (msg->rx.dlc & ESD_USB_RTR) {
				cf->can_id |= CAN_RTR_FLAG;
				len = 0;
			}
		}

		if (id & ESD_USB_EXTID)
			cfd->can_id |= CAN_EFF_FLAG;

		memcpy(cfd->data, msg->rx.data_fd, len);
		stats->rx_bytes += len;
		stats->rx_packets++;

		netif_rx(skb);
	}
}

static void esd_usb_tx_done_msg(struct esd_usb_net_priv *priv,
				union esd_usb_msg *msg)
{
	struct net_device_stats *stats = &priv->netdev->stats;
	struct net_device *netdev = priv->netdev;
	struct esd_tx_urb_context *context;

	if (!netif_device_present(netdev))
		return;

	context = &priv->tx_contexts[msg->txdone.hnd & (ESD_USB_MAX_TX_URBS - 1)];

	if (!msg->txdone.status) {
		stats->tx_packets++;
		stats->tx_bytes += can_get_echo_skb(netdev, context->echo_index,
						    NULL);
	} else {
		stats->tx_errors++;
		can_free_echo_skb(netdev, context->echo_index, NULL);
	}

	/* Release context */
	context->echo_index = ESD_USB_MAX_TX_URBS;
	atomic_dec(&priv->active_tx_jobs);

	netif_wake_queue(netdev);
}

static void esd_usb_read_bulk_callback(struct urb *urb)
{
	struct esd_usb *dev = urb->context;
	int retval;
	int pos = 0;
	int i;

	switch (urb->status) {
	case 0: /* success */
		break;

	case -ENOENT:
	case -EPIPE:
	case -EPROTO:
	case -ESHUTDOWN:
		return;

	default:
		dev_info(dev->udev->dev.parent,
			 "Rx URB aborted (%d)\n", urb->status);
		goto resubmit_urb;
	}

	while (pos < urb->actual_length) {
		union esd_usb_msg *msg;

		msg = (union esd_usb_msg *)(urb->transfer_buffer + pos);

		switch (msg->hdr.cmd) {
		case ESD_USB_CMD_CAN_RX:
			if (msg->rx.net >= dev->net_count) {
				dev_err(dev->udev->dev.parent, "format error\n");
				break;
			}

			esd_usb_rx_can_msg(dev->nets[msg->rx.net], msg);
			break;

		case ESD_USB_CMD_CAN_TX:
			if (msg->txdone.net >= dev->net_count) {
				dev_err(dev->udev->dev.parent, "format error\n");
				break;
			}

			esd_usb_tx_done_msg(dev->nets[msg->txdone.net],
					    msg);
			break;
		}

		pos += msg->hdr.len * sizeof(u32); /* convert to # of bytes */

		if (pos > urb->actual_length) {
			dev_err(dev->udev->dev.parent, "format error\n");
			break;
		}
	}

resubmit_urb:
	usb_fill_bulk_urb(urb, dev->udev, usb_rcvbulkpipe(dev->udev, 1),
			  urb->transfer_buffer, ESD_USB_RX_BUFFER_SIZE,
			  esd_usb_read_bulk_callback, dev);

	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval == -ENODEV) {
		for (i = 0; i < dev->net_count; i++) {
			if (dev->nets[i])
				netif_device_detach(dev->nets[i]->netdev);
		}
	} else if (retval) {
		dev_err(dev->udev->dev.parent,
			"failed resubmitting read bulk urb: %d\n", retval);
	}
}

/* callback for bulk IN urb */
static void esd_usb_write_bulk_callback(struct urb *urb)
{
	struct esd_tx_urb_context *context = urb->context;
	struct esd_usb_net_priv *priv;
	struct net_device *netdev;
	size_t size = sizeof(union esd_usb_msg);

	WARN_ON(!context);

	priv = context->priv;
	netdev = priv->netdev;

	/* free up our allocated buffer */
	usb_free_coherent(urb->dev, size,
			  urb->transfer_buffer, urb->transfer_dma);

	if (!netif_device_present(netdev))
		return;

	if (urb->status)
		netdev_info(netdev, "Tx URB aborted (%d)\n", urb->status);

	netif_trans_update(netdev);
}

static ssize_t firmware_show(struct device *d,
			     struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf = to_usb_interface(d);
	struct esd_usb *dev = usb_get_intfdata(intf);

	return sprintf(buf, "%d.%d.%d\n",
		       (dev->version >> 12) & 0xf,
		       (dev->version >> 8) & 0xf,
		       dev->version & 0xff);
}
static DEVICE_ATTR_RO(firmware);

static ssize_t hardware_show(struct device *d,
			     struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf = to_usb_interface(d);
	struct esd_usb *dev = usb_get_intfdata(intf);

	return sprintf(buf, "%d.%d.%d\n",
		       (dev->version >> 28) & 0xf,
		       (dev->version >> 24) & 0xf,
		       (dev->version >> 16) & 0xff);
}
static DEVICE_ATTR_RO(hardware);

static ssize_t nets_show(struct device *d,
			 struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf = to_usb_interface(d);
	struct esd_usb *dev = usb_get_intfdata(intf);

	return sprintf(buf, "%d", dev->net_count);
}
static DEVICE_ATTR_RO(nets);

static int esd_usb_send_msg(struct esd_usb *dev, union esd_usb_msg *msg)
{
	int actual_length;

	return usb_bulk_msg(dev->udev,
			    usb_sndbulkpipe(dev->udev, 2),
			    msg,
			    msg->hdr.len * sizeof(u32), /* convert to # of bytes */
			    &actual_length,
			    1000);
}

static int esd_usb_wait_msg(struct esd_usb *dev,
			    union esd_usb_msg *msg)
{
	int actual_length;

	return usb_bulk_msg(dev->udev,
			    usb_rcvbulkpipe(dev->udev, 1),
			    msg,
			    sizeof(*msg),
			    &actual_length,
			    1000);
}

static int esd_usb_setup_rx_urbs(struct esd_usb *dev)
{
	int i, err = 0;

	if (dev->rxinitdone)
		return 0;

	for (i = 0; i < ESD_USB_MAX_RX_URBS; i++) {
		struct urb *urb = NULL;
		u8 *buf = NULL;
		dma_addr_t buf_dma;

		/* create a URB, and a buffer for it */
		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb) {
			err = -ENOMEM;
			break;
		}

		buf = usb_alloc_coherent(dev->udev, ESD_USB_RX_BUFFER_SIZE, GFP_KERNEL,
					 &buf_dma);
		if (!buf) {
			dev_warn(dev->udev->dev.parent,
				 "No memory left for USB buffer\n");
			err = -ENOMEM;
			goto freeurb;
		}

		urb->transfer_dma = buf_dma;

		usb_fill_bulk_urb(urb, dev->udev,
				  usb_rcvbulkpipe(dev->udev, 1),
				  buf, ESD_USB_RX_BUFFER_SIZE,
				  esd_usb_read_bulk_callback, dev);
		urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		usb_anchor_urb(urb, &dev->rx_submitted);

		err = usb_submit_urb(urb, GFP_KERNEL);
		if (err) {
			usb_unanchor_urb(urb);
			usb_free_coherent(dev->udev, ESD_USB_RX_BUFFER_SIZE, buf,
					  urb->transfer_dma);
			goto freeurb;
		}

		dev->rxbuf[i] = buf;
		dev->rxbuf_dma[i] = buf_dma;

freeurb:
		/* Drop reference, USB core will take care of freeing it */
		usb_free_urb(urb);
		if (err)
			break;
	}

	/* Did we submit any URBs */
	if (i == 0) {
		dev_err(dev->udev->dev.parent, "couldn't setup read URBs\n");
		return err;
	}

	/* Warn if we've couldn't transmit all the URBs */
	if (i < ESD_USB_MAX_RX_URBS) {
		dev_warn(dev->udev->dev.parent,
			 "rx performance may be slow\n");
	}

	dev->rxinitdone = 1;
	return 0;
}

/* Start interface */
static int esd_usb_start(struct esd_usb_net_priv *priv)
{
	struct esd_usb *dev = priv->usb;
	struct net_device *netdev = priv->netdev;
	union esd_usb_msg *msg;
	int err, i;

	msg = kmalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg) {
		err = -ENOMEM;
		goto out;
	}

	/* Enable all IDs
	 * The IDADD message takes up to 64 32 bit bitmasks (2048 bits).
	 * Each bit represents one 11 bit CAN identifier. A set bit
	 * enables reception of the corresponding CAN identifier. A cleared
	 * bit disabled this identifier. An additional bitmask value
	 * following the CAN 2.0A bits is used to enable reception of
	 * extended CAN frames. Only the LSB of this final mask is checked
	 * for the complete 29 bit ID range. The IDADD message also allows
	 * filter configuration for an ID subset. In this case you can add
	 * the number of the starting bitmask (0..64) to the filter.option
	 * field followed by only some bitmasks.
	 */
	msg->hdr.cmd = ESD_USB_CMD_IDADD;
	msg->hdr.len = sizeof(struct esd_usb_id_filter_msg) / sizeof(u32); /* # of 32bit words */
	msg->filter.net = priv->index;
	msg->filter.option = ESD_USB_ID_ENABLE; /* start with segment 0 */
	for (i = 0; i < ESD_USB_MAX_ID_SEGMENT; i++)
		msg->filter.mask[i] = cpu_to_le32(GENMASK(31, 0));
	/* enable 29bit extended IDs */
	msg->filter.mask[ESD_USB_MAX_ID_SEGMENT] = cpu_to_le32(BIT(0));

	err = esd_usb_send_msg(dev, msg);
	if (err)
		goto out;

	err = esd_usb_setup_rx_urbs(dev);
	if (err)
		goto out;

	priv->can.state = CAN_STATE_ERROR_ACTIVE;

out:
	if (err == -ENODEV)
		netif_device_detach(netdev);
	if (err)
		netdev_err(netdev, "couldn't start device: %d\n", err);

	kfree(msg);
	return err;
}

static void unlink_all_urbs(struct esd_usb *dev)
{
	struct esd_usb_net_priv *priv;
	int i, j;

	usb_kill_anchored_urbs(&dev->rx_submitted);

	for (i = 0; i < ESD_USB_MAX_RX_URBS; ++i)
		usb_free_coherent(dev->udev, ESD_USB_RX_BUFFER_SIZE,
				  dev->rxbuf[i], dev->rxbuf_dma[i]);

	for (i = 0; i < dev->net_count; i++) {
		priv = dev->nets[i];
		if (priv) {
			usb_kill_anchored_urbs(&priv->tx_submitted);
			atomic_set(&priv->active_tx_jobs, 0);

			for (j = 0; j < ESD_USB_MAX_TX_URBS; j++)
				priv->tx_contexts[j].echo_index = ESD_USB_MAX_TX_URBS;
		}
	}
}

static int esd_usb_open(struct net_device *netdev)
{
	struct esd_usb_net_priv *priv = netdev_priv(netdev);
	int err;

	/* common open */
	err = open_candev(netdev);
	if (err)
		return err;

	/* finally start device */
	err = esd_usb_start(priv);
	if (err) {
		netdev_warn(netdev, "couldn't start device: %d\n", err);
		close_candev(netdev);
		return err;
	}

	netif_start_queue(netdev);

	return 0;
}

static netdev_tx_t esd_usb_start_xmit(struct sk_buff *skb,
				      struct net_device *netdev)
{
	struct esd_usb_net_priv *priv = netdev_priv(netdev);
	struct esd_usb *dev = priv->usb;
	struct esd_tx_urb_context *context = NULL;
	struct net_device_stats *stats = &netdev->stats;
	struct canfd_frame *cfd = (struct canfd_frame *)skb->data;
	union esd_usb_msg *msg;
	struct urb *urb;
	u8 *buf;
	int i, err;
	int ret = NETDEV_TX_OK;
	size_t size = sizeof(union esd_usb_msg);

	if (can_dev_dropped_skb(netdev, skb))
		return NETDEV_TX_OK;

	/* create a URB, and a buffer for it, and copy the data to the URB */
	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!urb) {
		stats->tx_dropped++;
		dev_kfree_skb(skb);
		goto nourbmem;
	}

	buf = usb_alloc_coherent(dev->udev, size, GFP_ATOMIC,
				 &urb->transfer_dma);
	if (!buf) {
		netdev_err(netdev, "No memory left for USB buffer\n");
		stats->tx_dropped++;
		dev_kfree_skb(skb);
		goto nobufmem;
	}

	msg = (union esd_usb_msg *)buf;

	/* minimal length as # of 32bit words */
	msg->hdr.len = offsetof(struct esd_usb_tx_msg, data) / sizeof(u32);
	msg->hdr.cmd = ESD_USB_CMD_CAN_TX;
	msg->tx.net = priv->index;

	if (can_is_canfd_skb(skb)) {
		msg->tx.dlc = can_fd_len2dlc(cfd->len);
		msg->tx.dlc |= ESD_USB_FD;

		if ((cfd->flags & CANFD_BRS) == 0)
			msg->tx.dlc |= ESD_USB_NO_BRS;
	} else {
		msg->tx.dlc = can_get_cc_dlc((struct can_frame *)cfd, priv->can.ctrlmode);

		if (cfd->can_id & CAN_RTR_FLAG)
			msg->tx.dlc |= ESD_USB_RTR;
	}

	msg->tx.id = cpu_to_le32(cfd->can_id & CAN_ERR_MASK);

	if (cfd->can_id & CAN_EFF_FLAG)
		msg->tx.id |= cpu_to_le32(ESD_USB_EXTID);

	memcpy(msg->tx.data_fd, cfd->data, cfd->len);

	/* round up, then divide by 4 to add the payload length as # of 32bit words */
	msg->hdr.len += DIV_ROUND_UP(cfd->len, sizeof(u32));

	for (i = 0; i < ESD_USB_MAX_TX_URBS; i++) {
		if (priv->tx_contexts[i].echo_index == ESD_USB_MAX_TX_URBS) {
			context = &priv->tx_contexts[i];
			break;
		}
	}

	/* This may never happen */
	if (!context) {
		netdev_warn(netdev, "couldn't find free context\n");
		ret = NETDEV_TX_BUSY;
		goto releasebuf;
	}

	context->priv = priv;
	context->echo_index = i;

	/* hnd must not be 0 - MSB is stripped in txdone handling */
	msg->tx.hnd = BIT(31) | i; /* returned in TX done message */

	usb_fill_bulk_urb(urb, dev->udev, usb_sndbulkpipe(dev->udev, 2), buf,
			  msg->hdr.len * sizeof(u32), /* convert to # of bytes */
			  esd_usb_write_bulk_callback, context);

	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	usb_anchor_urb(urb, &priv->tx_submitted);

	can_put_echo_skb(skb, netdev, context->echo_index, 0);

	atomic_inc(&priv->active_tx_jobs);

	/* Slow down tx path */
	if (atomic_read(&priv->active_tx_jobs) >= ESD_USB_MAX_TX_URBS)
		netif_stop_queue(netdev);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err) {
		can_free_echo_skb(netdev, context->echo_index, NULL);

		atomic_dec(&priv->active_tx_jobs);
		usb_unanchor_urb(urb);

		stats->tx_dropped++;

		if (err == -ENODEV)
			netif_device_detach(netdev);
		else
			netdev_warn(netdev, "failed tx_urb %d\n", err);

		goto releasebuf;
	}

	netif_trans_update(netdev);

	/* Release our reference to this URB, the USB core will eventually free
	 * it entirely.
	 */
	usb_free_urb(urb);

	return NETDEV_TX_OK;

releasebuf:
	usb_free_coherent(dev->udev, size, buf, urb->transfer_dma);

nobufmem:
	usb_free_urb(urb);

nourbmem:
	return ret;
}

static int esd_usb_close(struct net_device *netdev)
{
	struct esd_usb_net_priv *priv = netdev_priv(netdev);
	union esd_usb_msg *msg;
	int i;

	msg = kmalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	/* Disable all IDs (see esd_usb_start()) */
	msg->hdr.cmd = ESD_USB_CMD_IDADD;
	msg->hdr.len = sizeof(struct esd_usb_id_filter_msg) / sizeof(u32);/* # of 32bit words */
	msg->filter.net = priv->index;
	msg->filter.option = ESD_USB_ID_ENABLE; /* start with segment 0 */
	for (i = 0; i <= ESD_USB_MAX_ID_SEGMENT; i++)
		msg->filter.mask[i] = 0;
	if (esd_usb_send_msg(priv->usb, msg) < 0)
		netdev_err(netdev, "sending idadd message failed\n");

	/* set CAN controller to reset mode */
	msg->hdr.len = sizeof(struct esd_usb_set_baudrate_msg) / sizeof(u32); /* # of 32bit words */
	msg->hdr.cmd = ESD_USB_CMD_SETBAUD;
	msg->setbaud.net = priv->index;
	msg->setbaud.rsvd = 0;
	msg->setbaud.baud = cpu_to_le32(ESD_USB_NO_BAUDRATE);
	if (esd_usb_send_msg(priv->usb, msg) < 0)
		netdev_err(netdev, "sending setbaud message failed\n");

	priv->can.state = CAN_STATE_STOPPED;

	netif_stop_queue(netdev);

	close_candev(netdev);

	kfree(msg);

	return 0;
}

static const struct net_device_ops esd_usb_netdev_ops = {
	.ndo_open = esd_usb_open,
	.ndo_stop = esd_usb_close,
	.ndo_start_xmit = esd_usb_start_xmit,
	.ndo_change_mtu = can_change_mtu,
};

static const struct ethtool_ops esd_usb_ethtool_ops = {
	.get_ts_info = ethtool_op_get_ts_info,
};

static const struct can_bittiming_const esd_usb_2_bittiming_const = {
	.name = "esd_usb_2",
	.tseg1_min = 1,
	.tseg1_max = 16,
	.tseg2_min = 1,
	.tseg2_max = 8,
	.sjw_max = 4,
	.brp_min = 1,
	.brp_max = 1024,
	.brp_inc = 1,
};

static int esd_usb_2_set_bittiming(struct net_device *netdev)
{
	const struct can_bittiming_const *btc = &esd_usb_2_bittiming_const;
	struct esd_usb_net_priv *priv = netdev_priv(netdev);
	struct can_bittiming *bt = &priv->can.bittiming;
	union esd_usb_msg *msg;
	int err;
	u32 canbtr;
	int sjw_shift;

	canbtr = ESD_USB_UBR;
	if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
		canbtr |= ESD_USB_LOM;

	canbtr |= (bt->brp - 1) & (btc->brp_max - 1);

	if (le16_to_cpu(priv->usb->udev->descriptor.idProduct) ==
	    ESD_USB_CANUSBM_PRODUCT_ID)
		sjw_shift = ESD_USB_M_SJW_SHIFT;
	else
		sjw_shift = ESD_USB_2_SJW_SHIFT;

	canbtr |= ((bt->sjw - 1) & (btc->sjw_max - 1))
		<< sjw_shift;
	canbtr |= ((bt->prop_seg + bt->phase_seg1 - 1)
		   & (btc->tseg1_max - 1))
		<< ESD_USB_2_TSEG1_SHIFT;
	canbtr |= ((bt->phase_seg2 - 1) & (btc->tseg2_max - 1))
		<< ESD_USB_2_TSEG2_SHIFT;
	if (priv->can.ctrlmode & CAN_CTRLMODE_3_SAMPLES)
		canbtr |= ESD_USB_TRIPLE_SAMPLES;

	msg = kmalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	msg->hdr.len = sizeof(struct esd_usb_set_baudrate_msg) / sizeof(u32); /* # of 32bit words */
	msg->hdr.cmd = ESD_USB_CMD_SETBAUD;
	msg->setbaud.net = priv->index;
	msg->setbaud.rsvd = 0;
	msg->setbaud.baud = cpu_to_le32(canbtr);

	netdev_dbg(netdev, "setting BTR=%#x\n", canbtr);

	err = esd_usb_send_msg(priv->usb, msg);

	kfree(msg);
	return err;
}

/* Nominal bittiming constants, see
 * Microchip SAM E70/S70/V70/V71, Data Sheet, Rev. G - 07/2022
 * 48.6.8 MCAN Nominal Bit Timing and Prescaler Register
 */
static const struct can_bittiming_const esd_usb_3_nom_bittiming_const = {
	.name = "esd_usb_3",
	.tseg1_min = 2,
	.tseg1_max = 256,
	.tseg2_min = 2,
	.tseg2_max = 128,
	.sjw_max = 128,
	.brp_min = 1,
	.brp_max = 512,
	.brp_inc = 1,
};

/* Data bittiming constants, see
 * Microchip SAM E70/S70/V70/V71, Data Sheet, Rev. G - 07/2022
 * 48.6.4 MCAN Data Bit Timing and Prescaler Register
 */
static const struct can_bittiming_const esd_usb_3_data_bittiming_const = {
	.name = "esd_usb_3",
	.tseg1_min = 2,
	.tseg1_max = 32,
	.tseg2_min = 1,
	.tseg2_max = 16,
	.sjw_max = 8,
	.brp_min = 1,
	.brp_max = 32,
	.brp_inc = 1,
};

static int esd_usb_3_set_bittiming(struct net_device *netdev)
{
	const struct can_bittiming_const *nom_btc = &esd_usb_3_nom_bittiming_const;
	const struct can_bittiming_const *data_btc = &esd_usb_3_data_bittiming_const;
	struct esd_usb_net_priv *priv = netdev_priv(netdev);
	struct can_bittiming *nom_bt = &priv->can.bittiming;
	struct can_bittiming *data_bt = &priv->can.data_bittiming;
	struct esd_usb_3_set_baudrate_msg_x *baud_x;
	union esd_usb_msg *msg;
	u16 flags = 0;
	int err;

	msg = kmalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	baud_x = &msg->setbaud_x;

	/* Canonical is the most reasonable mode for SocketCAN on CAN-USB/3 ... */
	baud_x->mode = cpu_to_le16(ESD_USB_3_BAUDRATE_MODE_BTR_CANONICAL);

	if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY)
		flags |= ESD_USB_3_BAUDRATE_FLAG_LOM;

	if (priv->can.ctrlmode & CAN_CTRLMODE_3_SAMPLES)
		flags |= ESD_USB_3_BAUDRATE_FLAG_TRS;

	baud_x->nom.brp = cpu_to_le16(nom_bt->brp & (nom_btc->brp_max - 1));
	baud_x->nom.sjw = cpu_to_le16(nom_bt->sjw & (nom_btc->sjw_max - 1));
	baud_x->nom.tseg1 = cpu_to_le16((nom_bt->prop_seg + nom_bt->phase_seg1)
					& (nom_btc->tseg1_max - 1));
	baud_x->nom.tseg2 = cpu_to_le16(nom_bt->phase_seg2 & (nom_btc->tseg2_max - 1));

	if (priv->can.ctrlmode & CAN_CTRLMODE_FD) {
		baud_x->data.brp = cpu_to_le16(data_bt->brp & (data_btc->brp_max - 1));
		baud_x->data.sjw = cpu_to_le16(data_bt->sjw & (data_btc->sjw_max - 1));
		baud_x->data.tseg1 = cpu_to_le16((data_bt->prop_seg + data_bt->phase_seg1)
						 & (data_btc->tseg1_max - 1));
		baud_x->data.tseg2 = cpu_to_le16(data_bt->phase_seg2 & (data_btc->tseg2_max - 1));
		flags |= ESD_USB_3_BAUDRATE_FLAG_FD;
	}

	/* Currently this driver only supports the automatic TDC mode */
	baud_x->tdc.tdc_mode = ESD_USB_3_TDC_MODE_AUTO;
	baud_x->tdc.ssp_offset = 0;
	baud_x->tdc.ssp_shift = 0;
	baud_x->tdc.tdc_filter = 0;

	baud_x->flags = cpu_to_le16(flags);
	baud_x->net = priv->index;
	baud_x->rsvd = 0;

	/* set len as # of 32bit words */
	msg->hdr.len = sizeof(struct esd_usb_3_set_baudrate_msg_x) / sizeof(u32);
	msg->hdr.cmd = ESD_USB_CMD_SETBAUD;

	netdev_dbg(netdev,
		   "ctrlmode=%#x/%#x, esd-net=%u, esd-mode=%#x, esd-flags=%#x\n",
		   priv->can.ctrlmode, priv->can.ctrlmode_supported,
		   priv->index, le16_to_cpu(baud_x->mode), flags);

	err = esd_usb_send_msg(priv->usb, msg);

	kfree(msg);
	return err;
}

static int esd_usb_get_berr_counter(const struct net_device *netdev,
				    struct can_berr_counter *bec)
{
	struct esd_usb_net_priv *priv = netdev_priv(netdev);

	bec->txerr = priv->bec.txerr;
	bec->rxerr = priv->bec.rxerr;

	return 0;
}

static int esd_usb_set_mode(struct net_device *netdev, enum can_mode mode)
{
	switch (mode) {
	case CAN_MODE_START:
		netif_wake_queue(netdev);
		break;

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int esd_usb_probe_one_net(struct usb_interface *intf, int index)
{
	struct esd_usb *dev = usb_get_intfdata(intf);
	struct net_device *netdev;
	struct esd_usb_net_priv *priv;
	int err = 0;
	int i;

	netdev = alloc_candev(sizeof(*priv), ESD_USB_MAX_TX_URBS);
	if (!netdev) {
		dev_err(&intf->dev, "couldn't alloc candev\n");
		err = -ENOMEM;
		goto done;
	}

	priv = netdev_priv(netdev);

	init_usb_anchor(&priv->tx_submitted);
	atomic_set(&priv->active_tx_jobs, 0);

	for (i = 0; i < ESD_USB_MAX_TX_URBS; i++)
		priv->tx_contexts[i].echo_index = ESD_USB_MAX_TX_URBS;

	priv->usb = dev;
	priv->netdev = netdev;
	priv->index = index;

	priv->can.state = CAN_STATE_STOPPED;
	priv->can.ctrlmode_supported = CAN_CTRLMODE_LISTENONLY |
		CAN_CTRLMODE_CC_LEN8_DLC |
		CAN_CTRLMODE_BERR_REPORTING;

	switch (le16_to_cpu(dev->udev->descriptor.idProduct)) {
	case ESD_USB_CANUSB3_PRODUCT_ID:
		priv->can.clock.freq = ESD_USB_3_CAN_CLOCK;
		priv->can.ctrlmode_supported |= CAN_CTRLMODE_3_SAMPLES;
		priv->can.ctrlmode_supported |= CAN_CTRLMODE_FD;
		priv->can.bittiming_const = &esd_usb_3_nom_bittiming_const;
		priv->can.data_bittiming_const = &esd_usb_3_data_bittiming_const;
		priv->can.do_set_bittiming = esd_usb_3_set_bittiming;
		priv->can.do_set_data_bittiming = esd_usb_3_set_bittiming;
		break;

	case ESD_USB_CANUSBM_PRODUCT_ID:
		priv->can.clock.freq = ESD_USB_M_CAN_CLOCK;
		priv->can.bittiming_const = &esd_usb_2_bittiming_const;
		priv->can.do_set_bittiming = esd_usb_2_set_bittiming;
		break;

	case ESD_USB_CANUSB2_PRODUCT_ID:
	default:
		priv->can.clock.freq = ESD_USB_2_CAN_CLOCK;
		priv->can.ctrlmode_supported |= CAN_CTRLMODE_3_SAMPLES;
		priv->can.bittiming_const = &esd_usb_2_bittiming_const;
		priv->can.do_set_bittiming = esd_usb_2_set_bittiming;
		break;
	}

	priv->can.do_set_mode = esd_usb_set_mode;
	priv->can.do_get_berr_counter = esd_usb_get_berr_counter;

	netdev->flags |= IFF_ECHO; /* we support local echo */

	netdev->netdev_ops = &esd_usb_netdev_ops;
	netdev->ethtool_ops = &esd_usb_ethtool_ops;

	SET_NETDEV_DEV(netdev, &intf->dev);
	netdev->dev_id = index;

	err = register_candev(netdev);
	if (err) {
		dev_err(&intf->dev, "couldn't register CAN device: %d\n", err);
		free_candev(netdev);
		err = -ENOMEM;
		goto done;
	}

	dev->nets[index] = priv;
	netdev_info(netdev, "device %s registered\n", netdev->name);

done:
	return err;
}

/* probe function for new USB devices
 *
 * check version information and number of available
 * CAN interfaces
 */
static int esd_usb_probe(struct usb_interface *intf,
			 const struct usb_device_id *id)
{
	struct esd_usb *dev;
	union esd_usb_msg *msg;
	int i, err;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		err = -ENOMEM;
		goto done;
	}

	dev->udev = interface_to_usbdev(intf);

	init_usb_anchor(&dev->rx_submitted);

	usb_set_intfdata(intf, dev);

	msg = kmalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg) {
		err = -ENOMEM;
		goto free_msg;
	}

	/* query number of CAN interfaces (nets) */
	msg->hdr.cmd = ESD_USB_CMD_VERSION;
	msg->hdr.len = sizeof(struct esd_usb_version_msg) / sizeof(u32); /* # of 32bit words */
	msg->version.rsvd = 0;
	msg->version.flags = 0;
	msg->version.drv_version = 0;

	err = esd_usb_send_msg(dev, msg);
	if (err < 0) {
		dev_err(&intf->dev, "sending version message failed\n");
		goto free_msg;
	}

	err = esd_usb_wait_msg(dev, msg);
	if (err < 0) {
		dev_err(&intf->dev, "no version message answer\n");
		goto free_msg;
	}

	dev->net_count = (int)msg->version_reply.nets;
	dev->version = le32_to_cpu(msg->version_reply.version);

	if (device_create_file(&intf->dev, &dev_attr_firmware))
		dev_err(&intf->dev,
			"Couldn't create device file for firmware\n");

	if (device_create_file(&intf->dev, &dev_attr_hardware))
		dev_err(&intf->dev,
			"Couldn't create device file for hardware\n");

	if (device_create_file(&intf->dev, &dev_attr_nets))
		dev_err(&intf->dev,
			"Couldn't create device file for nets\n");

	/* do per device probing */
	for (i = 0; i < dev->net_count; i++)
		esd_usb_probe_one_net(intf, i);

free_msg:
	kfree(msg);
	if (err)
		kfree(dev);
done:
	return err;
}

/* called by the usb core when the device is removed from the system */
static void esd_usb_disconnect(struct usb_interface *intf)
{
	struct esd_usb *dev = usb_get_intfdata(intf);
	struct net_device *netdev;
	int i;

	device_remove_file(&intf->dev, &dev_attr_firmware);
	device_remove_file(&intf->dev, &dev_attr_hardware);
	device_remove_file(&intf->dev, &dev_attr_nets);

	usb_set_intfdata(intf, NULL);

	if (dev) {
		for (i = 0; i < dev->net_count; i++) {
			if (dev->nets[i]) {
				netdev = dev->nets[i]->netdev;
				unregister_netdev(netdev);
				free_candev(netdev);
			}
		}
		unlink_all_urbs(dev);
		kfree(dev);
	}
}

/* usb specific object needed to register this driver with the usb subsystem */
static struct usb_driver esd_usb_driver = {
	.name = KBUILD_MODNAME,
	.probe = esd_usb_probe,
	.disconnect = esd_usb_disconnect,
	.id_table = esd_usb_table,
};

module_usb_driver(esd_usb_driver);
