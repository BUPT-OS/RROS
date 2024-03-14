/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2014 - 2022 Intel Corporation */
#ifndef QAT_BL_H
#define QAT_BL_H
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/types.h>

#define QAT_MAX_BUFF_DESC	4

struct qat_alg_buf {
	u32 len;
	u32 resrvd;
	u64 addr;
} __packed;

struct qat_alg_buf_list {
	u64 resrvd;
	u32 num_bufs;
	u32 num_mapped_bufs;
	struct qat_alg_buf buffers[];
} __packed;

struct qat_alg_fixed_buf_list {
	struct qat_alg_buf_list sgl_hdr;
	struct qat_alg_buf descriptors[QAT_MAX_BUFF_DESC];
} __packed __aligned(64);

struct qat_request_buffs {
	struct qat_alg_buf_list *bl;
	dma_addr_t blp;
	struct qat_alg_buf_list *blout;
	dma_addr_t bloutp;
	size_t sz;
	size_t sz_out;
	bool sgl_src_valid;
	bool sgl_dst_valid;
	struct qat_alg_fixed_buf_list sgl_src;
	struct qat_alg_fixed_buf_list sgl_dst;
};

struct qat_sgl_to_bufl_params {
	dma_addr_t extra_dst_buff;
	size_t sz_extra_dst_buff;
	unsigned int sskip;
	unsigned int dskip;
};

void qat_bl_free_bufl(struct adf_accel_dev *accel_dev,
		      struct qat_request_buffs *buf);
int qat_bl_sgl_to_bufl(struct adf_accel_dev *accel_dev,
		       struct scatterlist *sgl,
		       struct scatterlist *sglout,
		       struct qat_request_buffs *buf,
		       struct qat_sgl_to_bufl_params *params,
		       gfp_t flags);

static inline gfp_t qat_algs_alloc_flags(struct crypto_async_request *req)
{
	return req->flags & CRYPTO_TFM_REQ_MAY_SLEEP ? GFP_KERNEL : GFP_ATOMIC;
}

int qat_bl_realloc_map_new_dst(struct adf_accel_dev *accel_dev,
			       struct scatterlist **newd,
			       unsigned int dlen,
			       struct qat_request_buffs *qat_bufs,
			       gfp_t gfp);

#endif
