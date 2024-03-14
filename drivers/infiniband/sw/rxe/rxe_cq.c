// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2016 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 */
#include <linux/vmalloc.h>
#include "rxe.h"
#include "rxe_loc.h"
#include "rxe_queue.h"

int rxe_cq_chk_attr(struct rxe_dev *rxe, struct rxe_cq *cq,
		    int cqe, int comp_vector)
{
	int count;

	if (cqe <= 0) {
		rxe_dbg_dev(rxe, "cqe(%d) <= 0\n", cqe);
		goto err1;
	}

	if (cqe > rxe->attr.max_cqe) {
		rxe_dbg_dev(rxe, "cqe(%d) > max_cqe(%d)\n",
				cqe, rxe->attr.max_cqe);
		goto err1;
	}

	if (cq) {
		count = queue_count(cq->queue, QUEUE_TYPE_TO_CLIENT);
		if (cqe < count) {
			rxe_dbg_cq(cq, "cqe(%d) < current # elements in queue (%d)",
					cqe, count);
			goto err1;
		}
	}

	return 0;

err1:
	return -EINVAL;
}

int rxe_cq_from_init(struct rxe_dev *rxe, struct rxe_cq *cq, int cqe,
		     int comp_vector, struct ib_udata *udata,
		     struct rxe_create_cq_resp __user *uresp)
{
	int err;
	enum queue_type type;

	type = QUEUE_TYPE_TO_CLIENT;
	cq->queue = rxe_queue_init(rxe, &cqe,
			sizeof(struct rxe_cqe), type);
	if (!cq->queue) {
		rxe_dbg_dev(rxe, "unable to create cq\n");
		return -ENOMEM;
	}

	err = do_mmap_info(rxe, uresp ? &uresp->mi : NULL, udata,
			   cq->queue->buf, cq->queue->buf_size, &cq->queue->ip);
	if (err) {
		vfree(cq->queue->buf);
		kfree(cq->queue);
		return err;
	}

	cq->is_user = uresp;

	spin_lock_init(&cq->cq_lock);
	cq->ibcq.cqe = cqe;
	return 0;
}

int rxe_cq_resize_queue(struct rxe_cq *cq, int cqe,
			struct rxe_resize_cq_resp __user *uresp,
			struct ib_udata *udata)
{
	int err;

	err = rxe_queue_resize(cq->queue, (unsigned int *)&cqe,
			       sizeof(struct rxe_cqe), udata,
			       uresp ? &uresp->mi : NULL, NULL, &cq->cq_lock);
	if (!err)
		cq->ibcq.cqe = cqe;

	return err;
}

/* caller holds reference to cq */
int rxe_cq_post(struct rxe_cq *cq, struct rxe_cqe *cqe, int solicited)
{
	struct ib_event ev;
	int full;
	void *addr;
	unsigned long flags;

	spin_lock_irqsave(&cq->cq_lock, flags);

	full = queue_full(cq->queue, QUEUE_TYPE_TO_CLIENT);
	if (unlikely(full)) {
		rxe_err_cq(cq, "queue full");
		spin_unlock_irqrestore(&cq->cq_lock, flags);
		if (cq->ibcq.event_handler) {
			ev.device = cq->ibcq.device;
			ev.element.cq = &cq->ibcq;
			ev.event = IB_EVENT_CQ_ERR;
			cq->ibcq.event_handler(&ev, cq->ibcq.cq_context);
		}

		return -EBUSY;
	}

	addr = queue_producer_addr(cq->queue, QUEUE_TYPE_TO_CLIENT);
	memcpy(addr, cqe, sizeof(*cqe));

	queue_advance_producer(cq->queue, QUEUE_TYPE_TO_CLIENT);

	if ((cq->notify & IB_CQ_NEXT_COMP) ||
	    (cq->notify & IB_CQ_SOLICITED && solicited)) {
		cq->notify = 0;
		cq->ibcq.comp_handler(&cq->ibcq, cq->ibcq.cq_context);
	}

	spin_unlock_irqrestore(&cq->cq_lock, flags);

	return 0;
}

void rxe_cq_cleanup(struct rxe_pool_elem *elem)
{
	struct rxe_cq *cq = container_of(elem, typeof(*cq), elem);

	if (cq->queue)
		rxe_queue_cleanup(cq->queue);
}
