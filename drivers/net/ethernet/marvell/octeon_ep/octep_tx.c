// SPDX-License-Identifier: GPL-2.0
/* Marvell Octeon EP (EndPoint) Ethernet Driver
 *
 * Copyright (C) 2020 Marvell.
 *
 */

#include <linux/pci.h>
#include <linux/etherdevice.h>
#include <linux/vmalloc.h>

#include "octep_config.h"
#include "octep_main.h"

/* Reset various index of Tx queue data structure. */
static void octep_iq_reset_indices(struct octep_iq *iq)
{
	iq->fill_cnt = 0;
	iq->host_write_index = 0;
	iq->octep_read_index = 0;
	iq->flush_index = 0;
	iq->pkts_processed = 0;
	iq->pkt_in_done = 0;
	atomic_set(&iq->instr_pending, 0);
}

/**
 * octep_iq_process_completions() - Process Tx queue completions.
 *
 * @iq: Octeon Tx queue data structure.
 * @budget: max number of completions to be processed in one invocation.
 */
int octep_iq_process_completions(struct octep_iq *iq, u16 budget)
{
	u32 compl_pkts, compl_bytes, compl_sg;
	struct octep_device *oct = iq->octep_dev;
	struct octep_tx_buffer *tx_buffer;
	struct skb_shared_info *shinfo;
	u32 fi = iq->flush_index;
	struct sk_buff *skb;
	u8 frags, i;

	compl_pkts = 0;
	compl_sg = 0;
	compl_bytes = 0;
	iq->octep_read_index = oct->hw_ops.update_iq_read_idx(iq);

	while (likely(budget && (fi != iq->octep_read_index))) {
		tx_buffer = iq->buff_info + fi;
		skb = tx_buffer->skb;

		fi++;
		if (unlikely(fi == iq->max_count))
			fi = 0;
		compl_bytes += skb->len;
		compl_pkts++;
		budget--;

		if (!tx_buffer->gather) {
			dma_unmap_single(iq->dev, tx_buffer->dma,
					 tx_buffer->skb->len, DMA_TO_DEVICE);
			dev_kfree_skb_any(skb);
			continue;
		}

		/* Scatter/Gather */
		shinfo = skb_shinfo(skb);
		frags = shinfo->nr_frags;
		compl_sg++;

		dma_unmap_single(iq->dev, tx_buffer->sglist[0].dma_ptr[0],
				 tx_buffer->sglist[0].len[3], DMA_TO_DEVICE);

		i = 1; /* entry 0 is main skb, unmapped above */
		while (frags--) {
			dma_unmap_page(iq->dev, tx_buffer->sglist[i >> 2].dma_ptr[i & 3],
				       tx_buffer->sglist[i >> 2].len[3 - (i & 3)], DMA_TO_DEVICE);
			i++;
		}

		dev_kfree_skb_any(skb);
	}

	iq->pkts_processed += compl_pkts;
	atomic_sub(compl_pkts, &iq->instr_pending);
	iq->stats.instr_completed += compl_pkts;
	iq->stats.bytes_sent += compl_bytes;
	iq->stats.sgentry_sent += compl_sg;
	iq->flush_index = fi;

	netdev_tx_completed_queue(iq->netdev_q, compl_pkts, compl_bytes);

	if (unlikely(__netif_subqueue_stopped(iq->netdev, iq->q_no)) &&
	    ((iq->max_count - atomic_read(&iq->instr_pending)) >
	     OCTEP_WAKE_QUEUE_THRESHOLD))
		netif_wake_subqueue(iq->netdev, iq->q_no);
	return !budget;
}

/**
 * octep_iq_free_pending() - Free Tx buffers for pending completions.
 *
 * @iq: Octeon Tx queue data structure.
 */
static void octep_iq_free_pending(struct octep_iq *iq)
{
	struct octep_tx_buffer *tx_buffer;
	struct skb_shared_info *shinfo;
	u32 fi = iq->flush_index;
	struct sk_buff *skb;
	u8 frags, i;

	while (fi != iq->host_write_index) {
		tx_buffer = iq->buff_info + fi;
		skb = tx_buffer->skb;

		fi++;
		if (unlikely(fi == iq->max_count))
			fi = 0;

		if (!tx_buffer->gather) {
			dma_unmap_single(iq->dev, tx_buffer->dma,
					 tx_buffer->skb->len, DMA_TO_DEVICE);
			dev_kfree_skb_any(skb);
			continue;
		}

		/* Scatter/Gather */
		shinfo = skb_shinfo(skb);
		frags = shinfo->nr_frags;

		dma_unmap_single(iq->dev,
				 tx_buffer->sglist[0].dma_ptr[0],
				 tx_buffer->sglist[0].len[3],
				 DMA_TO_DEVICE);

		i = 1; /* entry 0 is main skb, unmapped above */
		while (frags--) {
			dma_unmap_page(iq->dev, tx_buffer->sglist[i >> 2].dma_ptr[i & 3],
				       tx_buffer->sglist[i >> 2].len[3 - (i & 3)], DMA_TO_DEVICE);
			i++;
		}

		dev_kfree_skb_any(skb);
	}

	atomic_set(&iq->instr_pending, 0);
	iq->flush_index = fi;
	netdev_tx_reset_queue(netdev_get_tx_queue(iq->netdev, iq->q_no));
}

/**
 * octep_clean_iqs()  - Clean Tx queues to shutdown the device.
 *
 * @oct: Octeon device private data structure.
 *
 * Free the buffers in Tx queue descriptors pending completion and
 * reset queue indices
 */
void octep_clean_iqs(struct octep_device *oct)
{
	int i;

	for (i = 0; i < oct->num_iqs; i++) {
		octep_iq_free_pending(oct->iq[i]);
		octep_iq_reset_indices(oct->iq[i]);
	}
}

/**
 * octep_setup_iq() - Setup a Tx queue.
 *
 * @oct: Octeon device private data structure.
 * @q_no: Tx queue number to be setup.
 *
 * Allocate resources for a Tx queue.
 */
static int octep_setup_iq(struct octep_device *oct, int q_no)
{
	u32 desc_ring_size, buff_info_size, sglist_size;
	struct octep_iq *iq;
	int i;

	iq = vzalloc(sizeof(*iq));
	if (!iq)
		goto iq_alloc_err;
	oct->iq[q_no] = iq;

	iq->octep_dev = oct;
	iq->netdev = oct->netdev;
	iq->dev = &oct->pdev->dev;
	iq->q_no = q_no;
	iq->max_count = CFG_GET_IQ_NUM_DESC(oct->conf);
	iq->ring_size_mask = iq->max_count - 1;
	iq->fill_threshold = CFG_GET_IQ_DB_MIN(oct->conf);
	iq->netdev_q = netdev_get_tx_queue(iq->netdev, q_no);

	/* Allocate memory for hardware queue descriptors */
	desc_ring_size = OCTEP_IQ_DESC_SIZE * CFG_GET_IQ_NUM_DESC(oct->conf);
	iq->desc_ring = dma_alloc_coherent(iq->dev, desc_ring_size,
					   &iq->desc_ring_dma, GFP_KERNEL);
	if (unlikely(!iq->desc_ring)) {
		dev_err(iq->dev,
			"Failed to allocate DMA memory for IQ-%d\n", q_no);
		goto desc_dma_alloc_err;
	}

	/* Allocate memory for hardware SGLIST descriptors */
	sglist_size = OCTEP_SGLIST_SIZE_PER_PKT *
		      CFG_GET_IQ_NUM_DESC(oct->conf);
	iq->sglist = dma_alloc_coherent(iq->dev, sglist_size,
					&iq->sglist_dma, GFP_KERNEL);
	if (unlikely(!iq->sglist)) {
		dev_err(iq->dev,
			"Failed to allocate DMA memory for IQ-%d SGLIST\n",
			q_no);
		goto sglist_alloc_err;
	}

	/* allocate memory to manage Tx packets pending completion */
	buff_info_size = OCTEP_IQ_TXBUFF_INFO_SIZE * iq->max_count;
	iq->buff_info = vzalloc(buff_info_size);
	if (!iq->buff_info) {
		dev_err(iq->dev,
			"Failed to allocate buff info for IQ-%d\n", q_no);
		goto buff_info_err;
	}

	/* Setup sglist addresses in tx_buffer entries */
	for (i = 0; i < CFG_GET_IQ_NUM_DESC(oct->conf); i++) {
		struct octep_tx_buffer *tx_buffer;

		tx_buffer = &iq->buff_info[i];
		tx_buffer->sglist =
			&iq->sglist[i * OCTEP_SGLIST_ENTRIES_PER_PKT];
		tx_buffer->sglist_dma =
			iq->sglist_dma + (i * OCTEP_SGLIST_SIZE_PER_PKT);
	}

	octep_iq_reset_indices(iq);
	oct->hw_ops.setup_iq_regs(oct, q_no);

	oct->num_iqs++;
	return 0;

buff_info_err:
	dma_free_coherent(iq->dev, sglist_size, iq->sglist, iq->sglist_dma);
sglist_alloc_err:
	dma_free_coherent(iq->dev, desc_ring_size,
			  iq->desc_ring, iq->desc_ring_dma);
desc_dma_alloc_err:
	vfree(iq);
	oct->iq[q_no] = NULL;
iq_alloc_err:
	return -1;
}

/**
 * octep_free_iq() - Free Tx queue resources.
 *
 * @iq: Octeon Tx queue data structure.
 *
 * Free all the resources allocated for a Tx queue.
 */
static void octep_free_iq(struct octep_iq *iq)
{
	struct octep_device *oct = iq->octep_dev;
	u64 desc_ring_size, sglist_size;
	int q_no = iq->q_no;

	desc_ring_size = OCTEP_IQ_DESC_SIZE * CFG_GET_IQ_NUM_DESC(oct->conf);

	vfree(iq->buff_info);

	if (iq->desc_ring)
		dma_free_coherent(iq->dev, desc_ring_size,
				  iq->desc_ring, iq->desc_ring_dma);

	sglist_size = OCTEP_SGLIST_SIZE_PER_PKT *
		      CFG_GET_IQ_NUM_DESC(oct->conf);
	if (iq->sglist)
		dma_free_coherent(iq->dev, sglist_size,
				  iq->sglist, iq->sglist_dma);

	vfree(iq);
	oct->iq[q_no] = NULL;
	oct->num_iqs--;
}

/**
 * octep_setup_iqs() - setup resources for all Tx queues.
 *
 * @oct: Octeon device private data structure.
 */
int octep_setup_iqs(struct octep_device *oct)
{
	int i;

	oct->num_iqs = 0;
	for (i = 0; i < CFG_GET_PORTS_ACTIVE_IO_RINGS(oct->conf); i++) {
		if (octep_setup_iq(oct, i)) {
			dev_err(&oct->pdev->dev,
				"Failed to setup IQ(TxQ)-%d.\n", i);
			goto iq_setup_err;
		}
		dev_dbg(&oct->pdev->dev, "Successfully setup IQ(TxQ)-%d.\n", i);
	}

	return 0;

iq_setup_err:
	while (i) {
		i--;
		octep_free_iq(oct->iq[i]);
	}
	return -1;
}

/**
 * octep_free_iqs() - Free resources of all Tx queues.
 *
 * @oct: Octeon device private data structure.
 */
void octep_free_iqs(struct octep_device *oct)
{
	int i;

	for (i = 0; i < CFG_GET_PORTS_ACTIVE_IO_RINGS(oct->conf); i++) {
		octep_free_iq(oct->iq[i]);
		dev_dbg(&oct->pdev->dev,
			"Successfully destroyed IQ(TxQ)-%d.\n", i);
	}
	oct->num_iqs = 0;
}
