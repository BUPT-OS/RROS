// SPDX-License-Identifier: GPL-2.0
/*
 * For transport using shared mem structure.
 *
 * Copyright (C) 2019 ARM Ltd.
 */

#include <linux/ktime.h>
#include <linux/io.h>
#include <linux/processor.h>
#include <linux/types.h>

#include <asm-generic/bug.h>

#include "common.h"

/*
 * SCMI specification requires all parameters, message headers, return
 * arguments or any protocol data to be expressed in little endian
 * format only.
 */
struct scmi_shared_mem {
	__le32 reserved;
	__le32 channel_status;
#define SCMI_SHMEM_CHAN_STAT_CHANNEL_ERROR	BIT(1)
#define SCMI_SHMEM_CHAN_STAT_CHANNEL_FREE	BIT(0)
	__le32 reserved1[2];
	__le32 flags;
#define SCMI_SHMEM_FLAG_INTR_ENABLED	BIT(0)
	__le32 length;
	__le32 msg_header;
	u8 msg_payload[];
};

void shmem_tx_prepare(struct scmi_shared_mem __iomem *shmem,
		      struct scmi_xfer *xfer, struct scmi_chan_info *cinfo)
{
	ktime_t stop;

	/*
	 * Ideally channel must be free by now unless OS timeout last
	 * request and platform continued to process the same, wait
	 * until it releases the shared memory, otherwise we may endup
	 * overwriting its response with new message payload or vice-versa.
	 * Giving up anyway after twice the expected channel timeout so as
	 * not to bail-out on intermittent issues where the platform is
	 * occasionally a bit slower to answer.
	 *
	 * Note that after a timeout is detected we bail-out and carry on but
	 * the transport functionality is probably permanently compromised:
	 * this is just to ease debugging and avoid complete hangs on boot
	 * due to a misbehaving SCMI firmware.
	 */
	stop = ktime_add_ms(ktime_get(), 2 * cinfo->rx_timeout_ms);
	spin_until_cond((ioread32(&shmem->channel_status) &
			 SCMI_SHMEM_CHAN_STAT_CHANNEL_FREE) ||
			 ktime_after(ktime_get(), stop));
	if (!(ioread32(&shmem->channel_status) &
	      SCMI_SHMEM_CHAN_STAT_CHANNEL_FREE)) {
		WARN_ON_ONCE(1);
		dev_err(cinfo->dev,
			"Timeout waiting for a free TX channel !\n");
		return;
	}

	/* Mark channel busy + clear error */
	iowrite32(0x0, &shmem->channel_status);
	iowrite32(xfer->hdr.poll_completion ? 0 : SCMI_SHMEM_FLAG_INTR_ENABLED,
		  &shmem->flags);
	iowrite32(sizeof(shmem->msg_header) + xfer->tx.len, &shmem->length);
	iowrite32(pack_scmi_header(&xfer->hdr), &shmem->msg_header);
	if (xfer->tx.buf)
		memcpy_toio(shmem->msg_payload, xfer->tx.buf, xfer->tx.len);
}

u32 shmem_read_header(struct scmi_shared_mem __iomem *shmem)
{
	return ioread32(&shmem->msg_header);
}

void shmem_fetch_response(struct scmi_shared_mem __iomem *shmem,
			  struct scmi_xfer *xfer)
{
	size_t len = ioread32(&shmem->length);

	xfer->hdr.status = ioread32(shmem->msg_payload);
	/* Skip the length of header and status in shmem area i.e 8 bytes */
	xfer->rx.len = min_t(size_t, xfer->rx.len, len > 8 ? len - 8 : 0);

	/* Take a copy to the rx buffer.. */
	memcpy_fromio(xfer->rx.buf, shmem->msg_payload + 4, xfer->rx.len);
}

void shmem_fetch_notification(struct scmi_shared_mem __iomem *shmem,
			      size_t max_len, struct scmi_xfer *xfer)
{
	size_t len = ioread32(&shmem->length);

	/* Skip only the length of header in shmem area i.e 4 bytes */
	xfer->rx.len = min_t(size_t, max_len, len > 4 ? len - 4 : 0);

	/* Take a copy to the rx buffer.. */
	memcpy_fromio(xfer->rx.buf, shmem->msg_payload, xfer->rx.len);
}

void shmem_clear_channel(struct scmi_shared_mem __iomem *shmem)
{
	iowrite32(SCMI_SHMEM_CHAN_STAT_CHANNEL_FREE, &shmem->channel_status);
}

bool shmem_poll_done(struct scmi_shared_mem __iomem *shmem,
		     struct scmi_xfer *xfer)
{
	u16 xfer_id;

	xfer_id = MSG_XTRACT_TOKEN(ioread32(&shmem->msg_header));

	if (xfer->hdr.seq != xfer_id)
		return false;

	return ioread32(&shmem->channel_status) &
		(SCMI_SHMEM_CHAN_STAT_CHANNEL_ERROR |
		 SCMI_SHMEM_CHAN_STAT_CHANNEL_FREE);
}
