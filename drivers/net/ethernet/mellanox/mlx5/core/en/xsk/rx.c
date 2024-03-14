// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2019 Mellanox Technologies. */

#include "rx.h"
#include "en/xdp.h"
#include <net/xdp_sock_drv.h>
#include <linux/filter.h>

/* RX data path */

static struct mlx5e_xdp_buff *xsk_buff_to_mxbuf(struct xdp_buff *xdp)
{
	/* mlx5e_xdp_buff shares its layout with xdp_buff_xsk
	 * and private mlx5e_xdp_buff fields fall into xdp_buff_xsk->cb
	 */
	return (struct mlx5e_xdp_buff *)xdp;
}

int mlx5e_xsk_alloc_rx_mpwqe(struct mlx5e_rq *rq, u16 ix)
{
	struct mlx5e_mpw_info *wi = mlx5e_get_mpw_info(rq, ix);
	struct mlx5e_icosq *icosq = rq->icosq;
	struct mlx5_wq_cyc *wq = &icosq->wq;
	struct mlx5e_umr_wqe *umr_wqe;
	struct xdp_buff **xsk_buffs;
	int batch, i;
	u32 offset; /* 17-bit value with MTT. */
	u16 pi;

	if (unlikely(!xsk_buff_can_alloc(rq->xsk_pool, rq->mpwqe.pages_per_wqe)))
		goto err;

	XSK_CHECK_PRIV_TYPE(struct mlx5e_xdp_buff);
	xsk_buffs = (struct xdp_buff **)wi->alloc_units.xsk_buffs;
	batch = xsk_buff_alloc_batch(rq->xsk_pool, xsk_buffs,
				     rq->mpwqe.pages_per_wqe);

	/* If batch < pages_per_wqe, either:
	 * 1. Some (or all) descriptors were invalid.
	 * 2. dma_need_sync is true, and it fell back to allocating one frame.
	 * In either case, try to continue allocating frames one by one, until
	 * the first error, which will mean there are no more valid descriptors.
	 */
	for (; batch < rq->mpwqe.pages_per_wqe; batch++) {
		xsk_buffs[batch] = xsk_buff_alloc(rq->xsk_pool);
		if (unlikely(!xsk_buffs[batch]))
			goto err_reuse_batch;
	}

	pi = mlx5e_icosq_get_next_pi(icosq, rq->mpwqe.umr_wqebbs);
	umr_wqe = mlx5_wq_cyc_get_wqe(wq, pi);
	memcpy(umr_wqe, &rq->mpwqe.umr_wqe, sizeof(struct mlx5e_umr_wqe));

	if (likely(rq->mpwqe.umr_mode == MLX5E_MPWRQ_UMR_MODE_ALIGNED)) {
		for (i = 0; i < batch; i++) {
			struct mlx5e_xdp_buff *mxbuf = xsk_buff_to_mxbuf(xsk_buffs[i]);
			dma_addr_t addr = xsk_buff_xdp_get_frame_dma(xsk_buffs[i]);

			umr_wqe->inline_mtts[i] = (struct mlx5_mtt) {
				.ptag = cpu_to_be64(addr | MLX5_EN_WR),
			};
			mxbuf->rq = rq;
		}
	} else if (unlikely(rq->mpwqe.umr_mode == MLX5E_MPWRQ_UMR_MODE_UNALIGNED)) {
		for (i = 0; i < batch; i++) {
			struct mlx5e_xdp_buff *mxbuf = xsk_buff_to_mxbuf(xsk_buffs[i]);
			dma_addr_t addr = xsk_buff_xdp_get_frame_dma(xsk_buffs[i]);

			umr_wqe->inline_ksms[i] = (struct mlx5_ksm) {
				.key = rq->mkey_be,
				.va = cpu_to_be64(addr),
			};
			mxbuf->rq = rq;
		}
	} else if (likely(rq->mpwqe.umr_mode == MLX5E_MPWRQ_UMR_MODE_TRIPLE)) {
		u32 mapping_size = 1 << (rq->mpwqe.page_shift - 2);

		for (i = 0; i < batch; i++) {
			struct mlx5e_xdp_buff *mxbuf = xsk_buff_to_mxbuf(xsk_buffs[i]);
			dma_addr_t addr = xsk_buff_xdp_get_frame_dma(xsk_buffs[i]);

			umr_wqe->inline_ksms[i << 2] = (struct mlx5_ksm) {
				.key = rq->mkey_be,
				.va = cpu_to_be64(addr),
			};
			umr_wqe->inline_ksms[(i << 2) + 1] = (struct mlx5_ksm) {
				.key = rq->mkey_be,
				.va = cpu_to_be64(addr + mapping_size),
			};
			umr_wqe->inline_ksms[(i << 2) + 2] = (struct mlx5_ksm) {
				.key = rq->mkey_be,
				.va = cpu_to_be64(addr + mapping_size * 2),
			};
			umr_wqe->inline_ksms[(i << 2) + 3] = (struct mlx5_ksm) {
				.key = rq->mkey_be,
				.va = cpu_to_be64(rq->wqe_overflow.addr),
			};
			mxbuf->rq = rq;
		}
	} else {
		__be32 pad_size = cpu_to_be32((1 << rq->mpwqe.page_shift) -
					      rq->xsk_pool->chunk_size);
		__be32 frame_size = cpu_to_be32(rq->xsk_pool->chunk_size);

		for (i = 0; i < batch; i++) {
			struct mlx5e_xdp_buff *mxbuf = xsk_buff_to_mxbuf(xsk_buffs[i]);
			dma_addr_t addr = xsk_buff_xdp_get_frame_dma(xsk_buffs[i]);

			umr_wqe->inline_klms[i << 1] = (struct mlx5_klm) {
				.key = rq->mkey_be,
				.va = cpu_to_be64(addr),
				.bcount = frame_size,
			};
			umr_wqe->inline_klms[(i << 1) + 1] = (struct mlx5_klm) {
				.key = rq->mkey_be,
				.va = cpu_to_be64(rq->wqe_overflow.addr),
				.bcount = pad_size,
			};
			mxbuf->rq = rq;
		}
	}

	bitmap_zero(wi->skip_release_bitmap, rq->mpwqe.pages_per_wqe);
	wi->consumed_strides = 0;

	umr_wqe->ctrl.opmod_idx_opcode =
		cpu_to_be32((icosq->pc << MLX5_WQE_CTRL_WQE_INDEX_SHIFT) | MLX5_OPCODE_UMR);

	/* Optimized for speed: keep in sync with mlx5e_mpwrq_umr_entry_size. */
	offset = ix * rq->mpwqe.mtts_per_wqe;
	if (likely(rq->mpwqe.umr_mode == MLX5E_MPWRQ_UMR_MODE_ALIGNED))
		offset = offset * sizeof(struct mlx5_mtt) / MLX5_OCTWORD;
	else if (unlikely(rq->mpwqe.umr_mode == MLX5E_MPWRQ_UMR_MODE_OVERSIZED))
		offset = offset * sizeof(struct mlx5_klm) * 2 / MLX5_OCTWORD;
	else if (unlikely(rq->mpwqe.umr_mode == MLX5E_MPWRQ_UMR_MODE_TRIPLE))
		offset = offset * sizeof(struct mlx5_ksm) * 4 / MLX5_OCTWORD;
	umr_wqe->uctrl.xlt_offset = cpu_to_be16(offset);

	icosq->db.wqe_info[pi] = (struct mlx5e_icosq_wqe_info) {
		.wqe_type = MLX5E_ICOSQ_WQE_UMR_RX,
		.num_wqebbs = rq->mpwqe.umr_wqebbs,
		.umr.rq = rq,
	};

	icosq->pc += rq->mpwqe.umr_wqebbs;

	icosq->doorbell_cseg = &umr_wqe->ctrl;

	return 0;

err_reuse_batch:
	while (--batch >= 0)
		xsk_buff_free(xsk_buffs[batch]);

err:
	rq->stats->buff_alloc_err++;
	return -ENOMEM;
}

int mlx5e_xsk_alloc_rx_wqes_batched(struct mlx5e_rq *rq, u16 ix, int wqe_bulk)
{
	struct mlx5_wq_cyc *wq = &rq->wqe.wq;
	struct xdp_buff **buffs;
	u32 contig, alloc;
	int i;

	/* Each rq->wqe.frags->xskp is 1:1 mapped to an element inside the
	 * rq->wqe.alloc_units->xsk_buffs array allocated here.
	 */
	buffs = rq->wqe.alloc_units->xsk_buffs;
	contig = mlx5_wq_cyc_get_size(wq) - ix;
	if (wqe_bulk <= contig) {
		alloc = xsk_buff_alloc_batch(rq->xsk_pool, buffs + ix, wqe_bulk);
	} else {
		alloc = xsk_buff_alloc_batch(rq->xsk_pool, buffs + ix, contig);
		if (likely(alloc == contig))
			alloc += xsk_buff_alloc_batch(rq->xsk_pool, buffs, wqe_bulk - contig);
	}

	for (i = 0; i < alloc; i++) {
		int j = mlx5_wq_cyc_ctr2ix(wq, ix + i);
		struct mlx5e_wqe_frag_info *frag;
		struct mlx5e_rx_wqe_cyc *wqe;
		dma_addr_t addr;

		wqe = mlx5_wq_cyc_get_wqe(wq, j);
		/* Assumes log_num_frags == 0. */
		frag = &rq->wqe.frags[j];

		addr = xsk_buff_xdp_get_frame_dma(*frag->xskp);
		wqe->data[0].addr = cpu_to_be64(addr + rq->buff.headroom);
		frag->flags &= ~BIT(MLX5E_WQE_FRAG_SKIP_RELEASE);
	}

	return alloc;
}

int mlx5e_xsk_alloc_rx_wqes(struct mlx5e_rq *rq, u16 ix, int wqe_bulk)
{
	struct mlx5_wq_cyc *wq = &rq->wqe.wq;
	int i;

	for (i = 0; i < wqe_bulk; i++) {
		int j = mlx5_wq_cyc_ctr2ix(wq, ix + i);
		struct mlx5e_wqe_frag_info *frag;
		struct mlx5e_rx_wqe_cyc *wqe;
		dma_addr_t addr;

		wqe = mlx5_wq_cyc_get_wqe(wq, j);
		/* Assumes log_num_frags == 0. */
		frag = &rq->wqe.frags[j];

		*frag->xskp = xsk_buff_alloc(rq->xsk_pool);
		if (unlikely(!*frag->xskp))
			return i;

		addr = xsk_buff_xdp_get_frame_dma(*frag->xskp);
		wqe->data[0].addr = cpu_to_be64(addr + rq->buff.headroom);
		frag->flags &= ~BIT(MLX5E_WQE_FRAG_SKIP_RELEASE);
	}

	return wqe_bulk;
}

static struct sk_buff *mlx5e_xsk_construct_skb(struct mlx5e_rq *rq, struct xdp_buff *xdp)
{
	u32 totallen = xdp->data_end - xdp->data_meta;
	u32 metalen = xdp->data - xdp->data_meta;
	struct sk_buff *skb;

	skb = napi_alloc_skb(rq->cq.napi, totallen);
	if (unlikely(!skb)) {
		rq->stats->buff_alloc_err++;
		return NULL;
	}

	skb_put_data(skb, xdp->data_meta, totallen);

	if (metalen) {
		skb_metadata_set(skb, metalen);
		__skb_pull(skb, metalen);
	}

	return skb;
}

struct sk_buff *mlx5e_xsk_skb_from_cqe_mpwrq_linear(struct mlx5e_rq *rq,
						    struct mlx5e_mpw_info *wi,
						    struct mlx5_cqe64 *cqe,
						    u16 cqe_bcnt,
						    u32 head_offset,
						    u32 page_idx)
{
	struct mlx5e_xdp_buff *mxbuf = xsk_buff_to_mxbuf(wi->alloc_units.xsk_buffs[page_idx]);
	struct bpf_prog *prog;

	/* Check packet size. Note LRO doesn't use linear SKB */
	if (unlikely(cqe_bcnt > rq->hw_mtu)) {
		rq->stats->oversize_pkts_sw_drop++;
		return NULL;
	}

	/* head_offset is not used in this function, because xdp->data and the
	 * DMA address point directly to the necessary place. Furthermore, in
	 * the current implementation, UMR pages are mapped to XSK frames, so
	 * head_offset should always be 0.
	 */
	WARN_ON_ONCE(head_offset);

	/* mxbuf->rq is set on allocation, but cqe is per-packet so set it here */
	mxbuf->cqe = cqe;
	xsk_buff_set_size(&mxbuf->xdp, cqe_bcnt);
	xsk_buff_dma_sync_for_cpu(&mxbuf->xdp, rq->xsk_pool);
	net_prefetch(mxbuf->xdp.data);

	/* Possible flows:
	 * - XDP_REDIRECT to XSKMAP:
	 *   The page is owned by the userspace from now.
	 * - XDP_TX and other XDP_REDIRECTs:
	 *   The page was returned by ZCA and recycled.
	 * - XDP_DROP:
	 *   Recycle the page.
	 * - XDP_PASS:
	 *   Allocate an SKB, copy the data and recycle the page.
	 *
	 * Pages to be recycled go to the Reuse Ring on MPWQE deallocation. Its
	 * size is the same as the Driver RX Ring's size, and pages for WQEs are
	 * allocated first from the Reuse Ring, so it has enough space.
	 */

	prog = rcu_dereference(rq->xdp_prog);
	if (likely(prog && mlx5e_xdp_handle(rq, prog, mxbuf))) {
		if (likely(__test_and_clear_bit(MLX5E_RQ_FLAG_XDP_XMIT, rq->flags)))
			__set_bit(page_idx, wi->skip_release_bitmap); /* non-atomic */
		return NULL; /* page/packet was consumed by XDP */
	}

	/* XDP_PASS: copy the data from the UMEM to a new SKB and reuse the
	 * frame. On SKB allocation failure, NULL is returned.
	 */
	return mlx5e_xsk_construct_skb(rq, &mxbuf->xdp);
}

struct sk_buff *mlx5e_xsk_skb_from_cqe_linear(struct mlx5e_rq *rq,
					      struct mlx5e_wqe_frag_info *wi,
					      struct mlx5_cqe64 *cqe,
					      u32 cqe_bcnt)
{
	struct mlx5e_xdp_buff *mxbuf = xsk_buff_to_mxbuf(*wi->xskp);
	struct bpf_prog *prog;

	/* wi->offset is not used in this function, because xdp->data and the
	 * DMA address point directly to the necessary place. Furthermore, the
	 * XSK allocator allocates frames per packet, instead of pages, so
	 * wi->offset should always be 0.
	 */
	WARN_ON_ONCE(wi->offset);

	/* mxbuf->rq is set on allocation, but cqe is per-packet so set it here */
	mxbuf->cqe = cqe;
	xsk_buff_set_size(&mxbuf->xdp, cqe_bcnt);
	xsk_buff_dma_sync_for_cpu(&mxbuf->xdp, rq->xsk_pool);
	net_prefetch(mxbuf->xdp.data);

	prog = rcu_dereference(rq->xdp_prog);
	if (likely(prog && mlx5e_xdp_handle(rq, prog, mxbuf))) {
		if (likely(__test_and_clear_bit(MLX5E_RQ_FLAG_XDP_XMIT, rq->flags)))
			wi->flags |= BIT(MLX5E_WQE_FRAG_SKIP_RELEASE);
		return NULL; /* page/packet was consumed by XDP */
	}

	/* XDP_PASS: copy the data from the UMEM to a new SKB. The frame reuse
	 * will be handled by mlx5e_free_rx_wqe.
	 * On SKB allocation failure, NULL is returned.
	 */
	return mlx5e_xsk_construct_skb(rq, &mxbuf->xdp);
}
