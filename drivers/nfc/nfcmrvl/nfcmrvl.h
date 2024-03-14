/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Marvell NFC driver
 *
 * Copyright (C) 2014-2015, Marvell International Ltd.
 */

#ifndef _NFCMRVL_H_
#define _NFCMRVL_H_

#include "fw_dnld.h"

/* Define private flags: */
#define NFCMRVL_NCI_RUNNING			1
#define NFCMRVL_PHY_ERROR			2

#define NFCMRVL_EXT_COEX_ID			0xE0
#define NFCMRVL_NOT_ALLOWED_ID			0xE1
#define NFCMRVL_ACTIVE_ID			0xE2
#define NFCMRVL_EXT_COEX_ENABLE			1
#define NFCMRVL_GPIO_PIN_NFC_NOT_ALLOWED	0xA
#define NFCMRVL_GPIO_PIN_NFC_ACTIVE		0xB
#define NFCMRVL_NCI_MAX_EVENT_SIZE		260

/*
 * NCI FW Parameters
 */

#define NFCMRVL_PB_BAIL_OUT			0x11
#define NFCMRVL_PROP_REF_CLOCK			0xF0
#define NFCMRVL_PROP_SET_HI_CONFIG		0xF1

/*
 * HCI defines
 */

#define NFCMRVL_HCI_EVENT_HEADER_SIZE		0x04
#define NFCMRVL_HCI_EVENT_CODE			0x04
#define NFCMRVL_HCI_NFC_EVENT_CODE		0xFF
#define NFCMRVL_HCI_COMMAND_CODE		0x01
#define NFCMRVL_HCI_OGF				0x81
#define NFCMRVL_HCI_OCF				0xFE

enum nfcmrvl_phy {
	NFCMRVL_PHY_USB		= 0,
	NFCMRVL_PHY_UART	= 1,
	NFCMRVL_PHY_I2C		= 2,
	NFCMRVL_PHY_SPI		= 3,
};

struct nfcmrvl_platform_data {
	/*
	 * Generic
	 */

	/* GPIO that is wired to RESET_N signal */
	int reset_n_io;
	/* Tell if transport is muxed in HCI one */
	bool hci_muxed;

	/*
	 * UART specific
	 */

	/* Tell if UART needs flow control at init */
	bool flow_control;
	/* Tell if firmware supports break control for power management */
	bool break_control;


	/*
	 * I2C specific
	 */

	unsigned int irq;
	unsigned int irq_polarity;
};

struct nfcmrvl_private {

	unsigned long flags;

	/* Platform configuration */
	struct nfcmrvl_platform_data config;

	/* Parent dev */
	struct nci_dev *ndev;

	/* FW download context */
	struct nfcmrvl_fw_dnld fw_dnld;

	/* FW download support */
	bool support_fw_dnld;

	/*
	 * PHY related information
	 */

	/* PHY driver context */
	void *drv_data;
	/* PHY device */
	struct device *dev;
	/* PHY type */
	enum nfcmrvl_phy phy;
	/* Low level driver ops */
	const struct nfcmrvl_if_ops *if_ops;
};

struct nfcmrvl_if_ops {
	int (*nci_open) (struct nfcmrvl_private *priv);
	int (*nci_close) (struct nfcmrvl_private *priv);
	int (*nci_send) (struct nfcmrvl_private *priv, struct sk_buff *skb);
	void (*nci_update_config)(struct nfcmrvl_private *priv,
				  const void *param);
};

void nfcmrvl_nci_unregister_dev(struct nfcmrvl_private *priv);
int nfcmrvl_nci_recv_frame(struct nfcmrvl_private *priv, struct sk_buff *skb);
struct nfcmrvl_private *nfcmrvl_nci_register_dev(enum nfcmrvl_phy phy,
				void *drv_data,
				const struct nfcmrvl_if_ops *ops,
				struct device *dev,
				const struct nfcmrvl_platform_data *pdata);


void nfcmrvl_chip_reset(struct nfcmrvl_private *priv);
void nfcmrvl_chip_halt(struct nfcmrvl_private *priv);

int nfcmrvl_parse_dt(struct device_node *node,
		     struct nfcmrvl_platform_data *pdata);

#endif
