// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#include <linux/mlx5/device.h>
#include <linux/mlx5/mlx5_ifc.h>
#include <linux/xarray.h>
#include <linux/if_vlan.h>

#include "en.h"
#include "lib/aso.h"
#include "lib/crypto.h"
#include "en_accel/macsec.h"

#define MLX5_MACSEC_EPN_SCOPE_MID 0x80000000L
#define MLX5E_MACSEC_ASO_CTX_SZ MLX5_ST_SZ_BYTES(macsec_aso)

enum mlx5_macsec_aso_event_arm {
	MLX5E_ASO_EPN_ARM = BIT(0),
};

enum {
	MLX5_MACSEC_ASO_REMOVE_FLOW_PKT_CNT_OFFSET,
};

struct mlx5e_macsec_handle {
	struct mlx5e_macsec *macsec;
	u32 obj_id;
	u8 idx;
};

enum {
	MLX5_MACSEC_EPN,
};

struct mlx5e_macsec_aso_out {
	u8 event_arm;
	u32 mode_param;
};

struct mlx5e_macsec_aso_in {
	u8 mode;
	u32 obj_id;
};

struct mlx5e_macsec_epn_state {
	u32 epn_msb;
	u8 epn_enabled;
	u8 overlap;
};

struct mlx5e_macsec_async_work {
	struct mlx5e_macsec *macsec;
	struct mlx5_core_dev *mdev;
	struct work_struct work;
	u32 obj_id;
};

struct mlx5e_macsec_sa {
	bool active;
	u8  assoc_num;
	u32 macsec_obj_id;
	u32 enc_key_id;
	u32 next_pn;
	sci_t sci;
	ssci_t ssci;
	salt_t salt;

	union mlx5_macsec_rule *macsec_rule;
	struct rcu_head rcu_head;
	struct mlx5e_macsec_epn_state epn_state;
};

struct mlx5e_macsec_rx_sc;
struct mlx5e_macsec_rx_sc_xarray_element {
	u32 fs_id;
	struct mlx5e_macsec_rx_sc *rx_sc;
};

struct mlx5e_macsec_rx_sc {
	bool active;
	sci_t sci;
	struct mlx5e_macsec_sa *rx_sa[MACSEC_NUM_AN];
	struct list_head rx_sc_list_element;
	struct mlx5e_macsec_rx_sc_xarray_element *sc_xarray_element;
	struct metadata_dst *md_dst;
	struct rcu_head rcu_head;
};

struct mlx5e_macsec_umr {
	u8 __aligned(64) ctx[MLX5_ST_SZ_BYTES(macsec_aso)];
	dma_addr_t dma_addr;
	u32 mkey;
};

struct mlx5e_macsec_aso {
	/* ASO */
	struct mlx5_aso *maso;
	/* Protects macsec ASO */
	struct mutex aso_lock;
	/* UMR */
	struct mlx5e_macsec_umr *umr;

	u32 pdn;
};

struct mlx5e_macsec_device {
	const struct net_device *netdev;
	struct mlx5e_macsec_sa *tx_sa[MACSEC_NUM_AN];
	struct list_head macsec_rx_sc_list_head;
	unsigned char *dev_addr;
	struct list_head macsec_device_list_element;
};

struct mlx5e_macsec {
	struct list_head macsec_device_list_head;
	int num_of_devices;
	struct mutex lock; /* Protects mlx5e_macsec internal contexts */

	/* Rx fs_id -> rx_sc mapping */
	struct xarray sc_xarray;

	struct mlx5_core_dev *mdev;

	/* ASO */
	struct mlx5e_macsec_aso aso;

	struct notifier_block nb;
	struct workqueue_struct *wq;
};

struct mlx5_macsec_obj_attrs {
	u32 aso_pdn;
	u32 next_pn;
	__be64 sci;
	u32 enc_key_id;
	bool encrypt;
	struct mlx5e_macsec_epn_state epn_state;
	salt_t salt;
	__be32 ssci;
	bool replay_protect;
	u32 replay_window;
};

struct mlx5_aso_ctrl_param {
	u8   data_mask_mode;
	u8   condition_0_operand;
	u8   condition_1_operand;
	u8   condition_0_offset;
	u8   condition_1_offset;
	u8   data_offset;
	u8   condition_operand;
	u32  condition_0_data;
	u32  condition_0_mask;
	u32  condition_1_data;
	u32  condition_1_mask;
	u64  bitwise_data;
	u64  data_mask;
};

static int mlx5e_macsec_aso_reg_mr(struct mlx5_core_dev *mdev, struct mlx5e_macsec_aso *aso)
{
	struct mlx5e_macsec_umr *umr;
	struct device *dma_device;
	dma_addr_t dma_addr;
	int err;

	umr = kzalloc(sizeof(*umr), GFP_KERNEL);
	if (!umr) {
		err = -ENOMEM;
		return err;
	}

	dma_device = mlx5_core_dma_dev(mdev);
	dma_addr = dma_map_single(dma_device, umr->ctx, sizeof(umr->ctx), DMA_BIDIRECTIONAL);
	err = dma_mapping_error(dma_device, dma_addr);
	if (err) {
		mlx5_core_err(mdev, "Can't map dma device, err=%d\n", err);
		goto out_dma;
	}

	err = mlx5e_create_mkey(mdev, aso->pdn, &umr->mkey);
	if (err) {
		mlx5_core_err(mdev, "Can't create mkey, err=%d\n", err);
		goto out_mkey;
	}

	umr->dma_addr = dma_addr;

	aso->umr = umr;

	return 0;

out_mkey:
	dma_unmap_single(dma_device, dma_addr, sizeof(umr->ctx), DMA_BIDIRECTIONAL);
out_dma:
	kfree(umr);
	return err;
}

static void mlx5e_macsec_aso_dereg_mr(struct mlx5_core_dev *mdev, struct mlx5e_macsec_aso *aso)
{
	struct mlx5e_macsec_umr *umr = aso->umr;

	mlx5_core_destroy_mkey(mdev, umr->mkey);
	dma_unmap_single(&mdev->pdev->dev, umr->dma_addr, sizeof(umr->ctx), DMA_BIDIRECTIONAL);
	kfree(umr);
}

static int macsec_set_replay_protection(struct mlx5_macsec_obj_attrs *attrs, void *aso_ctx)
{
	u8 window_sz;

	if (!attrs->replay_protect)
		return 0;

	switch (attrs->replay_window) {
	case 256:
		window_sz = MLX5_MACSEC_ASO_REPLAY_WIN_256BIT;
		break;
	case 128:
		window_sz = MLX5_MACSEC_ASO_REPLAY_WIN_128BIT;
		break;
	case 64:
		window_sz = MLX5_MACSEC_ASO_REPLAY_WIN_64BIT;
		break;
	case 32:
		window_sz = MLX5_MACSEC_ASO_REPLAY_WIN_32BIT;
		break;
	default:
		return -EINVAL;
	}
	MLX5_SET(macsec_aso, aso_ctx, window_size, window_sz);
	MLX5_SET(macsec_aso, aso_ctx, mode, MLX5_MACSEC_ASO_REPLAY_PROTECTION);

	return 0;
}

static int mlx5e_macsec_create_object(struct mlx5_core_dev *mdev,
				      struct mlx5_macsec_obj_attrs *attrs,
				      bool is_tx,
				      u32 *macsec_obj_id)
{
	u32 in[MLX5_ST_SZ_DW(create_macsec_obj_in)] = {};
	u32 out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)];
	void *aso_ctx;
	void *obj;
	int err;

	obj = MLX5_ADDR_OF(create_macsec_obj_in, in, macsec_object);
	aso_ctx = MLX5_ADDR_OF(macsec_offload_obj, obj, macsec_aso);

	MLX5_SET(macsec_offload_obj, obj, confidentiality_en, attrs->encrypt);
	MLX5_SET(macsec_offload_obj, obj, dekn, attrs->enc_key_id);
	MLX5_SET(macsec_offload_obj, obj, aso_return_reg, MLX5_MACSEC_ASO_REG_C_4_5);
	MLX5_SET(macsec_offload_obj, obj, macsec_aso_access_pd, attrs->aso_pdn);
	MLX5_SET(macsec_aso, aso_ctx, mode_parameter, attrs->next_pn);

	/* Epn */
	if (attrs->epn_state.epn_enabled) {
		void *salt_p;
		int i;

		MLX5_SET(macsec_aso, aso_ctx, epn_event_arm, 1);
		MLX5_SET(macsec_offload_obj, obj, epn_en, 1);
		MLX5_SET(macsec_offload_obj, obj, epn_msb, attrs->epn_state.epn_msb);
		MLX5_SET(macsec_offload_obj, obj, epn_overlap, attrs->epn_state.overlap);
		MLX5_SET64(macsec_offload_obj, obj, sci, (__force u64)attrs->ssci);
		salt_p = MLX5_ADDR_OF(macsec_offload_obj, obj, salt);
		for (i = 0; i < 3 ; i++)
			memcpy((u32 *)salt_p + i, &attrs->salt.bytes[4 * (2 - i)], 4);
	} else {
		MLX5_SET64(macsec_offload_obj, obj, sci, (__force u64)(attrs->sci));
	}

	MLX5_SET(macsec_aso, aso_ctx, valid, 0x1);
	if (is_tx) {
		MLX5_SET(macsec_aso, aso_ctx, mode, MLX5_MACSEC_ASO_INC_SN);
	} else {
		err = macsec_set_replay_protection(attrs, aso_ctx);
		if (err)
			return err;
	}

	/* general object fields set */
	MLX5_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_type, MLX5_GENERAL_OBJECT_TYPES_MACSEC);

	err = mlx5_cmd_exec(mdev, in, sizeof(in), out, sizeof(out));
	if (err) {
		mlx5_core_err(mdev,
			      "MACsec offload: Failed to create MACsec object (err = %d)\n",
			      err);
		return err;
	}

	*macsec_obj_id = MLX5_GET(general_obj_out_cmd_hdr, out, obj_id);

	return err;
}

static void mlx5e_macsec_destroy_object(struct mlx5_core_dev *mdev, u32 macsec_obj_id)
{
	u32 in[MLX5_ST_SZ_DW(general_obj_in_cmd_hdr)] = {};
	u32 out[MLX5_ST_SZ_DW(general_obj_out_cmd_hdr)];

	MLX5_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_DESTROY_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_type, MLX5_GENERAL_OBJECT_TYPES_MACSEC);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_id, macsec_obj_id);

	mlx5_cmd_exec(mdev, in, sizeof(in), out, sizeof(out));
}

static void mlx5e_macsec_cleanup_sa(struct mlx5e_macsec *macsec,
				    struct mlx5e_macsec_sa *sa,
				    bool is_tx, struct net_device *netdev, u32 fs_id)
{
	int action =  (is_tx) ?  MLX5_ACCEL_MACSEC_ACTION_ENCRYPT :
				 MLX5_ACCEL_MACSEC_ACTION_DECRYPT;

	if (!sa->macsec_rule)
		return;

	mlx5_macsec_fs_del_rule(macsec->mdev->macsec_fs, sa->macsec_rule, action, netdev,
				fs_id);
	mlx5e_macsec_destroy_object(macsec->mdev, sa->macsec_obj_id);
	sa->macsec_rule = NULL;
}

static int mlx5e_macsec_init_sa(struct macsec_context *ctx,
				struct mlx5e_macsec_sa *sa,
				bool encrypt, bool is_tx, u32 *fs_id)
{
	struct mlx5e_priv *priv = macsec_netdev_priv(ctx->netdev);
	struct mlx5e_macsec *macsec = priv->macsec;
	struct mlx5_macsec_rule_attrs rule_attrs;
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5_macsec_obj_attrs obj_attrs;
	union mlx5_macsec_rule *macsec_rule;
	int err;

	obj_attrs.next_pn = sa->next_pn;
	obj_attrs.sci = cpu_to_be64((__force u64)sa->sci);
	obj_attrs.enc_key_id = sa->enc_key_id;
	obj_attrs.encrypt = encrypt;
	obj_attrs.aso_pdn = macsec->aso.pdn;
	obj_attrs.epn_state = sa->epn_state;

	if (sa->epn_state.epn_enabled) {
		obj_attrs.ssci = cpu_to_be32((__force u32)sa->ssci);
		memcpy(&obj_attrs.salt, &sa->salt, sizeof(sa->salt));
	}

	obj_attrs.replay_window = ctx->secy->replay_window;
	obj_attrs.replay_protect = ctx->secy->replay_protect;

	err = mlx5e_macsec_create_object(mdev, &obj_attrs, is_tx, &sa->macsec_obj_id);
	if (err)
		return err;

	rule_attrs.macsec_obj_id = sa->macsec_obj_id;
	rule_attrs.sci = sa->sci;
	rule_attrs.assoc_num = sa->assoc_num;
	rule_attrs.action = (is_tx) ? MLX5_ACCEL_MACSEC_ACTION_ENCRYPT :
				      MLX5_ACCEL_MACSEC_ACTION_DECRYPT;

	macsec_rule = mlx5_macsec_fs_add_rule(mdev->macsec_fs, ctx, &rule_attrs, fs_id);
	if (!macsec_rule) {
		err = -ENOMEM;
		goto destroy_macsec_object;
	}

	sa->macsec_rule = macsec_rule;

	return 0;

destroy_macsec_object:
	mlx5e_macsec_destroy_object(mdev, sa->macsec_obj_id);

	return err;
}

static struct mlx5e_macsec_rx_sc *
mlx5e_macsec_get_rx_sc_from_sc_list(const struct list_head *list, sci_t sci)
{
	struct mlx5e_macsec_rx_sc *iter;

	list_for_each_entry_rcu(iter, list, rx_sc_list_element) {
		if (iter->sci == sci)
			return iter;
	}

	return NULL;
}

static int macsec_rx_sa_active_update(struct macsec_context *ctx,
				      struct mlx5e_macsec_sa *rx_sa,
				      bool active, u32 *fs_id)
{
	struct mlx5e_priv *priv = macsec_netdev_priv(ctx->netdev);
	struct mlx5e_macsec *macsec = priv->macsec;
	int err = 0;

	if (rx_sa->active == active)
		return 0;

	rx_sa->active = active;
	if (!active) {
		mlx5e_macsec_cleanup_sa(macsec, rx_sa, false, ctx->secy->netdev, *fs_id);
		return 0;
	}

	err = mlx5e_macsec_init_sa(ctx, rx_sa, true, false, fs_id);
	if (err)
		rx_sa->active = false;

	return err;
}

static bool mlx5e_macsec_secy_features_validate(struct macsec_context *ctx)
{
	const struct net_device *netdev = ctx->netdev;
	const struct macsec_secy *secy = ctx->secy;

	if (secy->validate_frames != MACSEC_VALIDATE_STRICT) {
		netdev_err(netdev,
			   "MACsec offload is supported only when validate_frame is in strict mode\n");
		return false;
	}

	if (secy->icv_len != MACSEC_DEFAULT_ICV_LEN) {
		netdev_err(netdev, "MACsec offload is supported only when icv_len is %d\n",
			   MACSEC_DEFAULT_ICV_LEN);
		return false;
	}

	if (!secy->protect_frames) {
		netdev_err(netdev,
			   "MACsec offload is supported only when protect_frames is set\n");
		return false;
	}

	if (!ctx->secy->tx_sc.encrypt) {
		netdev_err(netdev, "MACsec offload: encrypt off isn't supported\n");
		return false;
	}

	return true;
}

static struct mlx5e_macsec_device *
mlx5e_macsec_get_macsec_device_context(const struct mlx5e_macsec *macsec,
				       const struct macsec_context *ctx)
{
	struct mlx5e_macsec_device *iter;
	const struct list_head *list;

	list = &macsec->macsec_device_list_head;
	list_for_each_entry_rcu(iter, list, macsec_device_list_element) {
		if (iter->netdev == ctx->secy->netdev)
			return iter;
	}

	return NULL;
}

static void update_macsec_epn(struct mlx5e_macsec_sa *sa, const struct macsec_key *key,
			      const pn_t *next_pn_halves, ssci_t ssci)
{
	struct mlx5e_macsec_epn_state *epn_state = &sa->epn_state;

	sa->ssci = ssci;
	sa->salt = key->salt;
	epn_state->epn_enabled = 1;
	epn_state->epn_msb = next_pn_halves->upper;
	epn_state->overlap = next_pn_halves->lower < MLX5_MACSEC_EPN_SCOPE_MID ? 0 : 1;
}

static int mlx5e_macsec_add_txsa(struct macsec_context *ctx)
{
	struct mlx5e_priv *priv = macsec_netdev_priv(ctx->netdev);
	const struct macsec_tx_sc *tx_sc = &ctx->secy->tx_sc;
	const struct macsec_tx_sa *ctx_tx_sa = ctx->sa.tx_sa;
	const struct macsec_secy *secy = ctx->secy;
	struct mlx5e_macsec_device *macsec_device;
	struct mlx5_core_dev *mdev = priv->mdev;
	u8 assoc_num = ctx->sa.assoc_num;
	struct mlx5e_macsec_sa *tx_sa;
	struct mlx5e_macsec *macsec;
	int err = 0;

	mutex_lock(&priv->macsec->lock);

	macsec = priv->macsec;
	macsec_device = mlx5e_macsec_get_macsec_device_context(macsec, ctx);
	if (!macsec_device) {
		netdev_err(ctx->netdev, "MACsec offload: Failed to find device context\n");
		err = -EEXIST;
		goto out;
	}

	if (macsec_device->tx_sa[assoc_num]) {
		netdev_err(ctx->netdev, "MACsec offload tx_sa: %d already exist\n", assoc_num);
		err = -EEXIST;
		goto out;
	}

	tx_sa = kzalloc(sizeof(*tx_sa), GFP_KERNEL);
	if (!tx_sa) {
		err = -ENOMEM;
		goto out;
	}

	tx_sa->active = ctx_tx_sa->active;
	tx_sa->next_pn = ctx_tx_sa->next_pn_halves.lower;
	tx_sa->sci = secy->sci;
	tx_sa->assoc_num = assoc_num;

	if (secy->xpn)
		update_macsec_epn(tx_sa, &ctx_tx_sa->key, &ctx_tx_sa->next_pn_halves,
				  ctx_tx_sa->ssci);

	err = mlx5_create_encryption_key(mdev, ctx->sa.key, secy->key_len,
					 MLX5_ACCEL_OBJ_MACSEC_KEY,
					 &tx_sa->enc_key_id);
	if (err)
		goto destroy_sa;

	macsec_device->tx_sa[assoc_num] = tx_sa;
	if (!secy->operational ||
	    assoc_num != tx_sc->encoding_sa ||
	    !tx_sa->active)
		goto out;

	err = mlx5e_macsec_init_sa(ctx, tx_sa, tx_sc->encrypt, true, NULL);
	if (err)
		goto destroy_encryption_key;

	mutex_unlock(&macsec->lock);

	return 0;

destroy_encryption_key:
	macsec_device->tx_sa[assoc_num] = NULL;
	mlx5_destroy_encryption_key(mdev, tx_sa->enc_key_id);
destroy_sa:
	kfree(tx_sa);
out:
	mutex_unlock(&macsec->lock);

	return err;
}

static int mlx5e_macsec_upd_txsa(struct macsec_context *ctx)
{
	struct mlx5e_priv *priv = macsec_netdev_priv(ctx->netdev);
	const struct macsec_tx_sc *tx_sc = &ctx->secy->tx_sc;
	const struct macsec_tx_sa *ctx_tx_sa = ctx->sa.tx_sa;
	struct mlx5e_macsec_device *macsec_device;
	u8 assoc_num = ctx->sa.assoc_num;
	struct mlx5e_macsec_sa *tx_sa;
	struct mlx5e_macsec *macsec;
	struct net_device *netdev;
	int err = 0;

	mutex_lock(&priv->macsec->lock);

	macsec = priv->macsec;
	netdev = ctx->netdev;
	macsec_device = mlx5e_macsec_get_macsec_device_context(macsec, ctx);
	if (!macsec_device) {
		netdev_err(netdev, "MACsec offload: Failed to find device context\n");
		err = -EINVAL;
		goto out;
	}

	tx_sa = macsec_device->tx_sa[assoc_num];
	if (!tx_sa) {
		netdev_err(netdev, "MACsec offload: TX sa 0x%x doesn't exist\n", assoc_num);
		err = -EEXIST;
		goto out;
	}

	if (tx_sa->next_pn != ctx_tx_sa->next_pn_halves.lower) {
		netdev_err(netdev, "MACsec offload: update TX sa %d PN isn't supported\n",
			   assoc_num);
		err = -EINVAL;
		goto out;
	}

	if (tx_sa->active == ctx_tx_sa->active)
		goto out;

	tx_sa->active = ctx_tx_sa->active;
	if (tx_sa->assoc_num != tx_sc->encoding_sa)
		goto out;

	if (ctx_tx_sa->active) {
		err = mlx5e_macsec_init_sa(ctx, tx_sa, tx_sc->encrypt, true, NULL);
		if (err)
			goto out;
	} else {
		if (!tx_sa->macsec_rule) {
			err = -EINVAL;
			goto out;
		}

		mlx5e_macsec_cleanup_sa(macsec, tx_sa, true, ctx->secy->netdev, 0);
	}
out:
	mutex_unlock(&macsec->lock);

	return err;
}

static int mlx5e_macsec_del_txsa(struct macsec_context *ctx)
{
	struct mlx5e_priv *priv = macsec_netdev_priv(ctx->netdev);
	struct mlx5e_macsec_device *macsec_device;
	u8 assoc_num = ctx->sa.assoc_num;
	struct mlx5e_macsec_sa *tx_sa;
	struct mlx5e_macsec *macsec;
	int err = 0;

	mutex_lock(&priv->macsec->lock);
	macsec = priv->macsec;
	macsec_device = mlx5e_macsec_get_macsec_device_context(macsec, ctx);
	if (!macsec_device) {
		netdev_err(ctx->netdev, "MACsec offload: Failed to find device context\n");
		err = -EINVAL;
		goto out;
	}

	tx_sa = macsec_device->tx_sa[assoc_num];
	if (!tx_sa) {
		netdev_err(ctx->netdev, "MACsec offload: TX sa 0x%x doesn't exist\n", assoc_num);
		err = -EEXIST;
		goto out;
	}

	mlx5e_macsec_cleanup_sa(macsec, tx_sa, true, ctx->secy->netdev, 0);
	mlx5_destroy_encryption_key(macsec->mdev, tx_sa->enc_key_id);
	kfree_rcu_mightsleep(tx_sa);
	macsec_device->tx_sa[assoc_num] = NULL;

out:
	mutex_unlock(&macsec->lock);

	return err;
}

static int mlx5e_macsec_add_rxsc(struct macsec_context *ctx)
{
	struct mlx5e_macsec_rx_sc_xarray_element *sc_xarray_element;
	struct mlx5e_priv *priv = macsec_netdev_priv(ctx->netdev);
	const struct macsec_rx_sc *ctx_rx_sc = ctx->rx_sc;
	struct mlx5e_macsec_device *macsec_device;
	struct mlx5e_macsec_rx_sc *rx_sc;
	struct list_head *rx_sc_list;
	struct mlx5e_macsec *macsec;
	int err = 0;

	mutex_lock(&priv->macsec->lock);
	macsec = priv->macsec;
	macsec_device = mlx5e_macsec_get_macsec_device_context(macsec, ctx);
	if (!macsec_device) {
		netdev_err(ctx->netdev, "MACsec offload: Failed to find device context\n");
		err = -EINVAL;
		goto out;
	}

	rx_sc_list = &macsec_device->macsec_rx_sc_list_head;
	rx_sc = mlx5e_macsec_get_rx_sc_from_sc_list(rx_sc_list, ctx_rx_sc->sci);
	if (rx_sc) {
		netdev_err(ctx->netdev, "MACsec offload: rx_sc (sci %lld) already exists\n",
			   ctx_rx_sc->sci);
		err = -EEXIST;
		goto out;
	}

	rx_sc = kzalloc(sizeof(*rx_sc), GFP_KERNEL);
	if (!rx_sc) {
		err = -ENOMEM;
		goto out;
	}

	sc_xarray_element = kzalloc(sizeof(*sc_xarray_element), GFP_KERNEL);
	if (!sc_xarray_element) {
		err = -ENOMEM;
		goto destroy_rx_sc;
	}

	sc_xarray_element->rx_sc = rx_sc;
	err = xa_alloc(&macsec->sc_xarray, &sc_xarray_element->fs_id, sc_xarray_element,
		       XA_LIMIT(1, MLX5_MACEC_RX_FS_ID_MAX), GFP_KERNEL);
	if (err) {
		if (err == -EBUSY)
			netdev_err(ctx->netdev,
				   "MACsec offload: unable to create entry for RX SC (%d Rx SCs already allocated)\n",
				   MLX5_MACEC_RX_FS_ID_MAX);
		goto destroy_sc_xarray_elemenet;
	}

	rx_sc->md_dst = metadata_dst_alloc(0, METADATA_MACSEC, GFP_KERNEL);
	if (!rx_sc->md_dst) {
		err = -ENOMEM;
		goto erase_xa_alloc;
	}

	rx_sc->sci = ctx_rx_sc->sci;
	rx_sc->active = ctx_rx_sc->active;
	list_add_rcu(&rx_sc->rx_sc_list_element, rx_sc_list);

	rx_sc->sc_xarray_element = sc_xarray_element;
	rx_sc->md_dst->u.macsec_info.sci = rx_sc->sci;
	mutex_unlock(&macsec->lock);

	return 0;

erase_xa_alloc:
	xa_erase(&macsec->sc_xarray, sc_xarray_element->fs_id);
destroy_sc_xarray_elemenet:
	kfree(sc_xarray_element);
destroy_rx_sc:
	kfree(rx_sc);

out:
	mutex_unlock(&macsec->lock);

	return err;
}

static int mlx5e_macsec_upd_rxsc(struct macsec_context *ctx)
{
	struct mlx5e_priv *priv = macsec_netdev_priv(ctx->netdev);
	const struct macsec_rx_sc *ctx_rx_sc = ctx->rx_sc;
	struct mlx5e_macsec_device *macsec_device;
	struct mlx5e_macsec_rx_sc *rx_sc;
	struct mlx5e_macsec_sa *rx_sa;
	struct mlx5e_macsec *macsec;
	struct list_head *list;
	int i;
	int err = 0;

	mutex_lock(&priv->macsec->lock);

	macsec = priv->macsec;
	macsec_device = mlx5e_macsec_get_macsec_device_context(macsec, ctx);
	if (!macsec_device) {
		netdev_err(ctx->netdev, "MACsec offload: Failed to find device context\n");
		err = -EINVAL;
		goto out;
	}

	list = &macsec_device->macsec_rx_sc_list_head;
	rx_sc = mlx5e_macsec_get_rx_sc_from_sc_list(list, ctx_rx_sc->sci);
	if (!rx_sc) {
		err = -EINVAL;
		goto out;
	}

	if (rx_sc->active == ctx_rx_sc->active)
		goto out;

	rx_sc->active = ctx_rx_sc->active;
	for (i = 0; i < MACSEC_NUM_AN; ++i) {
		rx_sa = rx_sc->rx_sa[i];
		if (!rx_sa)
			continue;

		err = macsec_rx_sa_active_update(ctx, rx_sa, rx_sa->active && ctx_rx_sc->active,
						 &rx_sc->sc_xarray_element->fs_id);
		if (err)
			goto out;
	}

out:
	mutex_unlock(&macsec->lock);

	return err;
}

static void macsec_del_rxsc_ctx(struct mlx5e_macsec *macsec, struct mlx5e_macsec_rx_sc *rx_sc,
				struct net_device *netdev)
{
	struct mlx5e_macsec_sa *rx_sa;
	int i;

	for (i = 0; i < MACSEC_NUM_AN; ++i) {
		rx_sa = rx_sc->rx_sa[i];
		if (!rx_sa)
			continue;

		mlx5e_macsec_cleanup_sa(macsec, rx_sa, false, netdev,
					rx_sc->sc_xarray_element->fs_id);
		mlx5_destroy_encryption_key(macsec->mdev, rx_sa->enc_key_id);

		kfree(rx_sa);
		rx_sc->rx_sa[i] = NULL;
	}

	/* At this point the relevant MACsec offload Rx rule already removed at
	 * mlx5e_macsec_cleanup_sa need to wait for datapath to finish current
	 * Rx related data propagating using xa_erase which uses rcu to sync,
	 * once fs_id is erased then this rx_sc is hidden from datapath.
	 */
	list_del_rcu(&rx_sc->rx_sc_list_element);
	xa_erase(&macsec->sc_xarray, rx_sc->sc_xarray_element->fs_id);
	metadata_dst_free(rx_sc->md_dst);
	kfree(rx_sc->sc_xarray_element);
	kfree_rcu_mightsleep(rx_sc);
}

static int mlx5e_macsec_del_rxsc(struct macsec_context *ctx)
{
	struct mlx5e_priv *priv = macsec_netdev_priv(ctx->netdev);
	struct mlx5e_macsec_device *macsec_device;
	struct mlx5e_macsec_rx_sc *rx_sc;
	struct mlx5e_macsec *macsec;
	struct list_head *list;
	int err = 0;

	mutex_lock(&priv->macsec->lock);

	macsec = priv->macsec;
	macsec_device = mlx5e_macsec_get_macsec_device_context(macsec, ctx);
	if (!macsec_device) {
		netdev_err(ctx->netdev, "MACsec offload: Failed to find device context\n");
		err = -EINVAL;
		goto out;
	}

	list = &macsec_device->macsec_rx_sc_list_head;
	rx_sc = mlx5e_macsec_get_rx_sc_from_sc_list(list, ctx->rx_sc->sci);
	if (!rx_sc) {
		netdev_err(ctx->netdev,
			   "MACsec offload rx_sc sci %lld doesn't exist\n",
			   ctx->sa.rx_sa->sc->sci);
		err = -EINVAL;
		goto out;
	}

	macsec_del_rxsc_ctx(macsec, rx_sc, ctx->secy->netdev);
out:
	mutex_unlock(&macsec->lock);

	return err;
}

static int mlx5e_macsec_add_rxsa(struct macsec_context *ctx)
{
	struct mlx5e_priv *priv = macsec_netdev_priv(ctx->netdev);
	const struct macsec_rx_sa *ctx_rx_sa = ctx->sa.rx_sa;
	struct mlx5e_macsec_device *macsec_device;
	struct mlx5_core_dev *mdev = priv->mdev;
	u8 assoc_num = ctx->sa.assoc_num;
	struct mlx5e_macsec_rx_sc *rx_sc;
	sci_t sci = ctx_rx_sa->sc->sci;
	struct mlx5e_macsec_sa *rx_sa;
	struct mlx5e_macsec *macsec;
	struct list_head *list;
	int err = 0;

	mutex_lock(&priv->macsec->lock);

	macsec = priv->macsec;
	macsec_device = mlx5e_macsec_get_macsec_device_context(macsec, ctx);
	if (!macsec_device) {
		netdev_err(ctx->netdev, "MACsec offload: Failed to find device context\n");
		err = -EINVAL;
		goto out;
	}

	list = &macsec_device->macsec_rx_sc_list_head;
	rx_sc = mlx5e_macsec_get_rx_sc_from_sc_list(list, sci);
	if (!rx_sc) {
		netdev_err(ctx->netdev,
			   "MACsec offload rx_sc sci %lld doesn't exist\n",
			   ctx->sa.rx_sa->sc->sci);
		err = -EINVAL;
		goto out;
	}

	if (rx_sc->rx_sa[assoc_num]) {
		netdev_err(ctx->netdev,
			   "MACsec offload rx_sc sci %lld rx_sa %d already exist\n",
			   sci, assoc_num);
		err = -EEXIST;
		goto out;
	}

	rx_sa = kzalloc(sizeof(*rx_sa), GFP_KERNEL);
	if (!rx_sa) {
		err = -ENOMEM;
		goto out;
	}

	rx_sa->active = ctx_rx_sa->active;
	rx_sa->next_pn = ctx_rx_sa->next_pn;
	rx_sa->sci = sci;
	rx_sa->assoc_num = assoc_num;

	if (ctx->secy->xpn)
		update_macsec_epn(rx_sa, &ctx_rx_sa->key, &ctx_rx_sa->next_pn_halves,
				  ctx_rx_sa->ssci);

	err = mlx5_create_encryption_key(mdev, ctx->sa.key, ctx->secy->key_len,
					 MLX5_ACCEL_OBJ_MACSEC_KEY,
					 &rx_sa->enc_key_id);
	if (err)
		goto destroy_sa;

	rx_sc->rx_sa[assoc_num] = rx_sa;
	if (!rx_sa->active)
		goto out;

	//TODO - add support for both authentication and encryption flows
	err = mlx5e_macsec_init_sa(ctx, rx_sa, true, false, &rx_sc->sc_xarray_element->fs_id);
	if (err)
		goto destroy_encryption_key;

	goto out;

destroy_encryption_key:
	rx_sc->rx_sa[assoc_num] = NULL;
	mlx5_destroy_encryption_key(mdev, rx_sa->enc_key_id);
destroy_sa:
	kfree(rx_sa);
out:
	mutex_unlock(&macsec->lock);

	return err;
}

static int mlx5e_macsec_upd_rxsa(struct macsec_context *ctx)
{
	struct mlx5e_priv *priv = macsec_netdev_priv(ctx->netdev);
	const struct macsec_rx_sa *ctx_rx_sa = ctx->sa.rx_sa;
	struct mlx5e_macsec_device *macsec_device;
	u8 assoc_num = ctx->sa.assoc_num;
	struct mlx5e_macsec_rx_sc *rx_sc;
	sci_t sci = ctx_rx_sa->sc->sci;
	struct mlx5e_macsec_sa *rx_sa;
	struct mlx5e_macsec *macsec;
	struct list_head *list;
	int err = 0;

	mutex_lock(&priv->macsec->lock);

	macsec = priv->macsec;
	macsec_device = mlx5e_macsec_get_macsec_device_context(macsec, ctx);
	if (!macsec_device) {
		netdev_err(ctx->netdev, "MACsec offload: Failed to find device context\n");
		err = -EINVAL;
		goto out;
	}

	list = &macsec_device->macsec_rx_sc_list_head;
	rx_sc = mlx5e_macsec_get_rx_sc_from_sc_list(list, sci);
	if (!rx_sc) {
		netdev_err(ctx->netdev,
			   "MACsec offload rx_sc sci %lld doesn't exist\n",
			   ctx->sa.rx_sa->sc->sci);
		err = -EINVAL;
		goto out;
	}

	rx_sa = rx_sc->rx_sa[assoc_num];
	if (!rx_sa) {
		netdev_err(ctx->netdev,
			   "MACsec offload rx_sc sci %lld rx_sa %d doesn't exist\n",
			   sci, assoc_num);
		err = -EINVAL;
		goto out;
	}

	if (rx_sa->next_pn != ctx_rx_sa->next_pn_halves.lower) {
		netdev_err(ctx->netdev,
			   "MACsec offload update RX sa %d PN isn't supported\n",
			   assoc_num);
		err = -EINVAL;
		goto out;
	}

	err = macsec_rx_sa_active_update(ctx, rx_sa, ctx_rx_sa->active,
					 &rx_sc->sc_xarray_element->fs_id);
out:
	mutex_unlock(&macsec->lock);

	return err;
}

static int mlx5e_macsec_del_rxsa(struct macsec_context *ctx)
{
	struct mlx5e_priv *priv = macsec_netdev_priv(ctx->netdev);
	struct mlx5e_macsec_device *macsec_device;
	sci_t sci = ctx->sa.rx_sa->sc->sci;
	struct mlx5e_macsec_rx_sc *rx_sc;
	u8 assoc_num = ctx->sa.assoc_num;
	struct mlx5e_macsec_sa *rx_sa;
	struct mlx5e_macsec *macsec;
	struct list_head *list;
	int err = 0;

	mutex_lock(&priv->macsec->lock);

	macsec = priv->macsec;
	macsec_device = mlx5e_macsec_get_macsec_device_context(macsec, ctx);
	if (!macsec_device) {
		netdev_err(ctx->netdev, "MACsec offload: Failed to find device context\n");
		err = -EINVAL;
		goto out;
	}

	list = &macsec_device->macsec_rx_sc_list_head;
	rx_sc = mlx5e_macsec_get_rx_sc_from_sc_list(list, sci);
	if (!rx_sc) {
		netdev_err(ctx->netdev,
			   "MACsec offload rx_sc sci %lld doesn't exist\n",
			   ctx->sa.rx_sa->sc->sci);
		err = -EINVAL;
		goto out;
	}

	rx_sa = rx_sc->rx_sa[assoc_num];
	if (!rx_sa) {
		netdev_err(ctx->netdev,
			   "MACsec offload rx_sc sci %lld rx_sa %d doesn't exist\n",
			   sci, assoc_num);
		err = -EINVAL;
		goto out;
	}

	mlx5e_macsec_cleanup_sa(macsec, rx_sa, false, ctx->secy->netdev,
				rx_sc->sc_xarray_element->fs_id);
	mlx5_destroy_encryption_key(macsec->mdev, rx_sa->enc_key_id);
	kfree(rx_sa);
	rx_sc->rx_sa[assoc_num] = NULL;

out:
	mutex_unlock(&macsec->lock);

	return err;
}

static int mlx5e_macsec_add_secy(struct macsec_context *ctx)
{
	struct mlx5e_priv *priv = macsec_netdev_priv(ctx->netdev);
	const struct net_device *dev = ctx->secy->netdev;
	const struct net_device *netdev = ctx->netdev;
	struct mlx5e_macsec_device *macsec_device;
	struct mlx5e_macsec *macsec;
	int err = 0;

	if (!mlx5e_macsec_secy_features_validate(ctx))
		return -EINVAL;

	mutex_lock(&priv->macsec->lock);
	macsec = priv->macsec;
	if (mlx5e_macsec_get_macsec_device_context(macsec, ctx)) {
		netdev_err(netdev, "MACsec offload: MACsec net_device already exist\n");
		goto out;
	}

	if (macsec->num_of_devices >= MLX5_MACSEC_NUM_OF_SUPPORTED_INTERFACES) {
		netdev_err(netdev, "Currently, only %d MACsec offload devices can be set\n",
			   MLX5_MACSEC_NUM_OF_SUPPORTED_INTERFACES);
		err = -EBUSY;
		goto out;
	}

	macsec_device = kzalloc(sizeof(*macsec_device), GFP_KERNEL);
	if (!macsec_device) {
		err = -ENOMEM;
		goto out;
	}

	macsec_device->dev_addr = kmemdup(dev->dev_addr, dev->addr_len, GFP_KERNEL);
	if (!macsec_device->dev_addr) {
		kfree(macsec_device);
		err = -ENOMEM;
		goto out;
	}

	macsec_device->netdev = dev;

	INIT_LIST_HEAD_RCU(&macsec_device->macsec_rx_sc_list_head);
	list_add_rcu(&macsec_device->macsec_device_list_element, &macsec->macsec_device_list_head);

	++macsec->num_of_devices;
out:
	mutex_unlock(&macsec->lock);

	return err;
}

static int macsec_upd_secy_hw_address(struct macsec_context *ctx,
				      struct mlx5e_macsec_device *macsec_device)
{
	struct mlx5e_priv *priv = macsec_netdev_priv(ctx->netdev);
	const struct net_device *dev = ctx->secy->netdev;
	struct mlx5e_macsec *macsec = priv->macsec;
	struct mlx5e_macsec_rx_sc *rx_sc, *tmp;
	struct mlx5e_macsec_sa *rx_sa;
	struct list_head *list;
	int i, err = 0;


	list = &macsec_device->macsec_rx_sc_list_head;
	list_for_each_entry_safe(rx_sc, tmp, list, rx_sc_list_element) {
		for (i = 0; i < MACSEC_NUM_AN; ++i) {
			rx_sa = rx_sc->rx_sa[i];
			if (!rx_sa || !rx_sa->macsec_rule)
				continue;

			mlx5e_macsec_cleanup_sa(macsec, rx_sa, false, ctx->secy->netdev,
						rx_sc->sc_xarray_element->fs_id);
		}
	}

	list_for_each_entry_safe(rx_sc, tmp, list, rx_sc_list_element) {
		for (i = 0; i < MACSEC_NUM_AN; ++i) {
			rx_sa = rx_sc->rx_sa[i];
			if (!rx_sa)
				continue;

			if (rx_sa->active) {
				err = mlx5e_macsec_init_sa(ctx, rx_sa, true, false,
							   &rx_sc->sc_xarray_element->fs_id);
				if (err)
					goto out;
			}
		}
	}

	memcpy(macsec_device->dev_addr, dev->dev_addr, dev->addr_len);
out:
	return err;
}

/* this function is called from 2 macsec ops functions:
 *  macsec_set_mac_address – MAC address was changed, therefore we need to destroy
 *  and create new Tx contexts(macsec object + steering).
 *  macsec_changelink – in this case the tx SC or SecY may be changed, therefore need to
 *  destroy Tx and Rx contexts(macsec object + steering)
 */
static int mlx5e_macsec_upd_secy(struct macsec_context *ctx)
{
	struct mlx5e_priv *priv = macsec_netdev_priv(ctx->netdev);
	const struct macsec_tx_sc *tx_sc = &ctx->secy->tx_sc;
	const struct net_device *dev = ctx->secy->netdev;
	struct mlx5e_macsec_device *macsec_device;
	struct mlx5e_macsec_sa *tx_sa;
	struct mlx5e_macsec *macsec;
	int i, err = 0;

	if (!mlx5e_macsec_secy_features_validate(ctx))
		return -EINVAL;

	mutex_lock(&priv->macsec->lock);

	macsec = priv->macsec;
	macsec_device = mlx5e_macsec_get_macsec_device_context(macsec, ctx);
	if (!macsec_device) {
		netdev_err(ctx->netdev, "MACsec offload: Failed to find device context\n");
		err = -EINVAL;
		goto out;
	}

	/* if the dev_addr hasn't change, it mean the callback is from macsec_changelink */
	if (!memcmp(macsec_device->dev_addr, dev->dev_addr, dev->addr_len)) {
		err = macsec_upd_secy_hw_address(ctx, macsec_device);
		if (err)
			goto out;
	}

	for (i = 0; i < MACSEC_NUM_AN; ++i) {
		tx_sa = macsec_device->tx_sa[i];
		if (!tx_sa)
			continue;

		mlx5e_macsec_cleanup_sa(macsec, tx_sa, true, ctx->secy->netdev, 0);
	}

	for (i = 0; i < MACSEC_NUM_AN; ++i) {
		tx_sa = macsec_device->tx_sa[i];
		if (!tx_sa)
			continue;

		if (tx_sa->assoc_num == tx_sc->encoding_sa && tx_sa->active) {
			err = mlx5e_macsec_init_sa(ctx, tx_sa, tx_sc->encrypt, true, NULL);
			if (err)
				goto out;
		}
	}

out:
	mutex_unlock(&macsec->lock);

	return err;
}

static int mlx5e_macsec_del_secy(struct macsec_context *ctx)
{
	struct mlx5e_priv *priv = macsec_netdev_priv(ctx->netdev);
	struct mlx5e_macsec_device *macsec_device;
	struct mlx5e_macsec_rx_sc *rx_sc, *tmp;
	struct mlx5e_macsec_sa *tx_sa;
	struct mlx5e_macsec *macsec;
	struct list_head *list;
	int err = 0;
	int i;

	mutex_lock(&priv->macsec->lock);
	macsec = priv->macsec;
	macsec_device = mlx5e_macsec_get_macsec_device_context(macsec, ctx);
	if (!macsec_device) {
		netdev_err(ctx->netdev, "MACsec offload: Failed to find device context\n");
		err = -EINVAL;

		goto out;
	}

	for (i = 0; i < MACSEC_NUM_AN; ++i) {
		tx_sa = macsec_device->tx_sa[i];
		if (!tx_sa)
			continue;

		mlx5e_macsec_cleanup_sa(macsec, tx_sa, true, ctx->secy->netdev, 0);
		mlx5_destroy_encryption_key(macsec->mdev, tx_sa->enc_key_id);
		kfree(tx_sa);
		macsec_device->tx_sa[i] = NULL;
	}

	list = &macsec_device->macsec_rx_sc_list_head;
	list_for_each_entry_safe(rx_sc, tmp, list, rx_sc_list_element)
		macsec_del_rxsc_ctx(macsec, rx_sc, ctx->secy->netdev);

	kfree(macsec_device->dev_addr);
	macsec_device->dev_addr = NULL;

	list_del_rcu(&macsec_device->macsec_device_list_element);
	--macsec->num_of_devices;
	kfree(macsec_device);

out:
	mutex_unlock(&macsec->lock);

	return err;
}

static void macsec_build_accel_attrs(struct mlx5e_macsec_sa *sa,
				     struct mlx5_macsec_obj_attrs *attrs)
{
	attrs->epn_state.epn_msb = sa->epn_state.epn_msb;
	attrs->epn_state.overlap = sa->epn_state.overlap;
}

static void macsec_aso_build_wqe_ctrl_seg(struct mlx5e_macsec_aso *macsec_aso,
					  struct mlx5_wqe_aso_ctrl_seg *aso_ctrl,
					  struct mlx5_aso_ctrl_param *param)
{
	struct mlx5e_macsec_umr *umr = macsec_aso->umr;

	memset(aso_ctrl, 0, sizeof(*aso_ctrl));
	aso_ctrl->va_l = cpu_to_be32(umr->dma_addr | ASO_CTRL_READ_EN);
	aso_ctrl->va_h = cpu_to_be32((u64)umr->dma_addr >> 32);
	aso_ctrl->l_key = cpu_to_be32(umr->mkey);

	if (!param)
		return;

	aso_ctrl->data_mask_mode = param->data_mask_mode << 6;
	aso_ctrl->condition_1_0_operand = param->condition_1_operand |
						param->condition_0_operand << 4;
	aso_ctrl->condition_1_0_offset = param->condition_1_offset |
						param->condition_0_offset << 4;
	aso_ctrl->data_offset_condition_operand = param->data_offset |
						param->condition_operand << 6;
	aso_ctrl->condition_0_data = cpu_to_be32(param->condition_0_data);
	aso_ctrl->condition_0_mask = cpu_to_be32(param->condition_0_mask);
	aso_ctrl->condition_1_data = cpu_to_be32(param->condition_1_data);
	aso_ctrl->condition_1_mask = cpu_to_be32(param->condition_1_mask);
	aso_ctrl->bitwise_data = cpu_to_be64(param->bitwise_data);
	aso_ctrl->data_mask = cpu_to_be64(param->data_mask);
}

static int mlx5e_macsec_modify_obj(struct mlx5_core_dev *mdev, struct mlx5_macsec_obj_attrs *attrs,
				   u32 macsec_id)
{
	u32 in[MLX5_ST_SZ_DW(modify_macsec_obj_in)] = {};
	u32 out[MLX5_ST_SZ_DW(query_macsec_obj_out)];
	u64 modify_field_select = 0;
	void *obj;
	int err;

	/* General object fields set */
	MLX5_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_QUERY_GENERAL_OBJECT);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_type, MLX5_GENERAL_OBJECT_TYPES_MACSEC);
	MLX5_SET(general_obj_in_cmd_hdr, in, obj_id, macsec_id);
	err = mlx5_cmd_exec(mdev, in, sizeof(in), out, sizeof(out));
	if (err) {
		mlx5_core_err(mdev, "Query MACsec object failed (Object id %d), err = %d\n",
			      macsec_id, err);
		return err;
	}

	obj = MLX5_ADDR_OF(query_macsec_obj_out, out, macsec_object);
	modify_field_select = MLX5_GET64(macsec_offload_obj, obj, modify_field_select);

	/* EPN */
	if (!(modify_field_select & MLX5_MODIFY_MACSEC_BITMASK_EPN_OVERLAP) ||
	    !(modify_field_select & MLX5_MODIFY_MACSEC_BITMASK_EPN_MSB)) {
		mlx5_core_dbg(mdev, "MACsec object field is not modifiable (Object id %d)\n",
			      macsec_id);
		return -EOPNOTSUPP;
	}

	obj = MLX5_ADDR_OF(modify_macsec_obj_in, in, macsec_object);
	MLX5_SET64(macsec_offload_obj, obj, modify_field_select,
		   MLX5_MODIFY_MACSEC_BITMASK_EPN_OVERLAP | MLX5_MODIFY_MACSEC_BITMASK_EPN_MSB);
	MLX5_SET(macsec_offload_obj, obj, epn_msb, attrs->epn_state.epn_msb);
	MLX5_SET(macsec_offload_obj, obj, epn_overlap, attrs->epn_state.overlap);

	/* General object fields set */
	MLX5_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_MODIFY_GENERAL_OBJECT);

	return mlx5_cmd_exec(mdev, in, sizeof(in), out, sizeof(out));
}

static void macsec_aso_build_ctrl(struct mlx5e_macsec_aso *aso,
				  struct mlx5_wqe_aso_ctrl_seg *aso_ctrl,
				  struct mlx5e_macsec_aso_in *in)
{
	struct mlx5_aso_ctrl_param param = {};

	param.data_mask_mode = MLX5_ASO_DATA_MASK_MODE_BITWISE_64BIT;
	param.condition_0_operand = MLX5_ASO_ALWAYS_TRUE;
	param.condition_1_operand = MLX5_ASO_ALWAYS_TRUE;
	if (in->mode == MLX5_MACSEC_EPN) {
		param.data_offset = MLX5_MACSEC_ASO_REMOVE_FLOW_PKT_CNT_OFFSET;
		param.bitwise_data = BIT_ULL(54);
		param.data_mask = param.bitwise_data;
	}
	macsec_aso_build_wqe_ctrl_seg(aso, aso_ctrl, &param);
}

static int macsec_aso_set_arm_event(struct mlx5_core_dev *mdev, struct mlx5e_macsec *macsec,
				    struct mlx5e_macsec_aso_in *in)
{
	struct mlx5e_macsec_aso *aso;
	struct mlx5_aso_wqe *aso_wqe;
	struct mlx5_aso *maso;
	int err;

	aso = &macsec->aso;
	maso = aso->maso;

	mutex_lock(&aso->aso_lock);
	aso_wqe = mlx5_aso_get_wqe(maso);
	mlx5_aso_build_wqe(maso, MLX5_MACSEC_ASO_DS_CNT, aso_wqe, in->obj_id,
			   MLX5_ACCESS_ASO_OPC_MOD_MACSEC);
	macsec_aso_build_ctrl(aso, &aso_wqe->aso_ctrl, in);
	mlx5_aso_post_wqe(maso, false, &aso_wqe->ctrl);
	err = mlx5_aso_poll_cq(maso, false);
	mutex_unlock(&aso->aso_lock);

	return err;
}

static int macsec_aso_query(struct mlx5_core_dev *mdev, struct mlx5e_macsec *macsec,
			    struct mlx5e_macsec_aso_in *in, struct mlx5e_macsec_aso_out *out)
{
	struct mlx5e_macsec_aso *aso;
	struct mlx5_aso_wqe *aso_wqe;
	struct mlx5_aso *maso;
	unsigned long expires;
	int err;

	aso = &macsec->aso;
	maso = aso->maso;

	mutex_lock(&aso->aso_lock);

	aso_wqe = mlx5_aso_get_wqe(maso);
	mlx5_aso_build_wqe(maso, MLX5_MACSEC_ASO_DS_CNT, aso_wqe, in->obj_id,
			   MLX5_ACCESS_ASO_OPC_MOD_MACSEC);
	macsec_aso_build_wqe_ctrl_seg(aso, &aso_wqe->aso_ctrl, NULL);

	mlx5_aso_post_wqe(maso, false, &aso_wqe->ctrl);
	expires = jiffies + msecs_to_jiffies(10);
	do {
		err = mlx5_aso_poll_cq(maso, false);
		if (err)
			usleep_range(2, 10);
	} while (err && time_is_after_jiffies(expires));

	if (err)
		goto err_out;

	if (MLX5_GET(macsec_aso, aso->umr->ctx, epn_event_arm))
		out->event_arm |= MLX5E_ASO_EPN_ARM;

	out->mode_param = MLX5_GET(macsec_aso, aso->umr->ctx, mode_parameter);

err_out:
	mutex_unlock(&aso->aso_lock);
	return err;
}

static struct mlx5e_macsec_sa *get_macsec_tx_sa_from_obj_id(const struct mlx5e_macsec *macsec,
							    const u32 obj_id)
{
	const struct list_head *device_list;
	struct mlx5e_macsec_sa *macsec_sa;
	struct mlx5e_macsec_device *iter;
	int i;

	device_list = &macsec->macsec_device_list_head;

	list_for_each_entry(iter, device_list, macsec_device_list_element) {
		for (i = 0; i < MACSEC_NUM_AN; ++i) {
			macsec_sa = iter->tx_sa[i];
			if (!macsec_sa || !macsec_sa->active)
				continue;
			if (macsec_sa->macsec_obj_id == obj_id)
				return macsec_sa;
		}
	}

	return NULL;
}

static struct mlx5e_macsec_sa *get_macsec_rx_sa_from_obj_id(const struct mlx5e_macsec *macsec,
							    const u32 obj_id)
{
	const struct list_head *device_list, *sc_list;
	struct mlx5e_macsec_rx_sc *mlx5e_rx_sc;
	struct mlx5e_macsec_sa *macsec_sa;
	struct mlx5e_macsec_device *iter;
	int i;

	device_list = &macsec->macsec_device_list_head;

	list_for_each_entry(iter, device_list, macsec_device_list_element) {
		sc_list = &iter->macsec_rx_sc_list_head;
		list_for_each_entry(mlx5e_rx_sc, sc_list, rx_sc_list_element) {
			for (i = 0; i < MACSEC_NUM_AN; ++i) {
				macsec_sa = mlx5e_rx_sc->rx_sa[i];
				if (!macsec_sa || !macsec_sa->active)
					continue;
				if (macsec_sa->macsec_obj_id == obj_id)
					return macsec_sa;
			}
		}
	}

	return NULL;
}

static void macsec_epn_update(struct mlx5e_macsec *macsec, struct mlx5_core_dev *mdev,
			      struct mlx5e_macsec_sa *sa, u32 obj_id, u32 mode_param)
{
	struct mlx5_macsec_obj_attrs attrs = {};
	struct mlx5e_macsec_aso_in in = {};

	/* When the bottom of the replay protection window (mode_param) crosses 2^31 (half sequence
	 * number wraparound) hence mode_param > MLX5_MACSEC_EPN_SCOPE_MID the SW should update the
	 * esn_overlap to OLD (1).
	 * When the bottom of the replay protection window (mode_param) crosses 2^32 (full sequence
	 * number wraparound) hence mode_param < MLX5_MACSEC_EPN_SCOPE_MID since it did a
	 * wraparound, the SW should update the esn_overlap to NEW (0), and increment the esn_msb.
	 */

	if (mode_param < MLX5_MACSEC_EPN_SCOPE_MID) {
		sa->epn_state.epn_msb++;
		sa->epn_state.overlap = 0;
	} else {
		sa->epn_state.overlap = 1;
	}

	macsec_build_accel_attrs(sa, &attrs);
	mlx5e_macsec_modify_obj(mdev, &attrs, obj_id);

	/* Re-set EPN arm event */
	in.obj_id = obj_id;
	in.mode = MLX5_MACSEC_EPN;
	macsec_aso_set_arm_event(mdev, macsec, &in);
}

static void macsec_async_event(struct work_struct *work)
{
	struct mlx5e_macsec_async_work *async_work;
	struct mlx5e_macsec_aso_out out = {};
	struct mlx5e_macsec_aso_in in = {};
	struct mlx5e_macsec_sa *macsec_sa;
	struct mlx5e_macsec *macsec;
	struct mlx5_core_dev *mdev;
	u32 obj_id;

	async_work = container_of(work, struct mlx5e_macsec_async_work, work);
	macsec = async_work->macsec;
	mutex_lock(&macsec->lock);

	mdev = async_work->mdev;
	obj_id = async_work->obj_id;
	macsec_sa = get_macsec_tx_sa_from_obj_id(macsec, obj_id);
	if (!macsec_sa) {
		macsec_sa = get_macsec_rx_sa_from_obj_id(macsec, obj_id);
		if (!macsec_sa) {
			mlx5_core_dbg(mdev, "MACsec SA is not found (SA object id %d)\n", obj_id);
			goto out_async_work;
		}
	}

	/* Query MACsec ASO context */
	in.obj_id = obj_id;
	macsec_aso_query(mdev, macsec, &in, &out);

	/* EPN case */
	if (macsec_sa->epn_state.epn_enabled && !(out.event_arm & MLX5E_ASO_EPN_ARM))
		macsec_epn_update(macsec, mdev, macsec_sa, obj_id, out.mode_param);

out_async_work:
	kfree(async_work);
	mutex_unlock(&macsec->lock);
}

static int macsec_obj_change_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct mlx5e_macsec *macsec = container_of(nb, struct mlx5e_macsec, nb);
	struct mlx5e_macsec_async_work *async_work;
	struct mlx5_eqe_obj_change *obj_change;
	struct mlx5_eqe *eqe = data;
	u16 obj_type;
	u32 obj_id;

	if (event != MLX5_EVENT_TYPE_OBJECT_CHANGE)
		return NOTIFY_DONE;

	obj_change = &eqe->data.obj_change;
	obj_type = be16_to_cpu(obj_change->obj_type);
	obj_id = be32_to_cpu(obj_change->obj_id);

	if (obj_type != MLX5_GENERAL_OBJECT_TYPES_MACSEC)
		return NOTIFY_DONE;

	async_work = kzalloc(sizeof(*async_work), GFP_ATOMIC);
	if (!async_work)
		return NOTIFY_DONE;

	async_work->macsec = macsec;
	async_work->mdev = macsec->mdev;
	async_work->obj_id = obj_id;

	INIT_WORK(&async_work->work, macsec_async_event);

	WARN_ON(!queue_work(macsec->wq, &async_work->work));

	return NOTIFY_OK;
}

static int mlx5e_macsec_aso_init(struct mlx5e_macsec_aso *aso, struct mlx5_core_dev *mdev)
{
	struct mlx5_aso *maso;
	int err;

	err = mlx5_core_alloc_pd(mdev, &aso->pdn);
	if (err) {
		mlx5_core_err(mdev,
			      "MACsec offload: Failed to alloc pd for MACsec ASO, err=%d\n",
			      err);
		return err;
	}

	maso = mlx5_aso_create(mdev, aso->pdn);
	if (IS_ERR(maso)) {
		err = PTR_ERR(maso);
		goto err_aso;
	}

	err = mlx5e_macsec_aso_reg_mr(mdev, aso);
	if (err)
		goto err_aso_reg;

	mutex_init(&aso->aso_lock);

	aso->maso = maso;

	return 0;

err_aso_reg:
	mlx5_aso_destroy(maso);
err_aso:
	mlx5_core_dealloc_pd(mdev, aso->pdn);
	return err;
}

static void mlx5e_macsec_aso_cleanup(struct mlx5e_macsec_aso *aso, struct mlx5_core_dev *mdev)
{
	if (!aso)
		return;

	mlx5e_macsec_aso_dereg_mr(mdev, aso);

	mlx5_aso_destroy(aso->maso);

	mlx5_core_dealloc_pd(mdev, aso->pdn);
}

static const struct macsec_ops macsec_offload_ops = {
	.mdo_add_txsa = mlx5e_macsec_add_txsa,
	.mdo_upd_txsa = mlx5e_macsec_upd_txsa,
	.mdo_del_txsa = mlx5e_macsec_del_txsa,
	.mdo_add_rxsc = mlx5e_macsec_add_rxsc,
	.mdo_upd_rxsc = mlx5e_macsec_upd_rxsc,
	.mdo_del_rxsc = mlx5e_macsec_del_rxsc,
	.mdo_add_rxsa = mlx5e_macsec_add_rxsa,
	.mdo_upd_rxsa = mlx5e_macsec_upd_rxsa,
	.mdo_del_rxsa = mlx5e_macsec_del_rxsa,
	.mdo_add_secy = mlx5e_macsec_add_secy,
	.mdo_upd_secy = mlx5e_macsec_upd_secy,
	.mdo_del_secy = mlx5e_macsec_del_secy,
};

bool mlx5e_macsec_handle_tx_skb(struct mlx5e_macsec *macsec, struct sk_buff *skb)
{
	struct metadata_dst *md_dst = skb_metadata_dst(skb);
	u32 fs_id;

	fs_id = mlx5_macsec_fs_get_fs_id_from_hashtable(macsec->mdev->macsec_fs,
							&md_dst->u.macsec_info.sci);
	if (!fs_id)
		goto err_out;

	return true;

err_out:
	dev_kfree_skb_any(skb);
	return false;
}

void mlx5e_macsec_tx_build_eseg(struct mlx5e_macsec *macsec,
				struct sk_buff *skb,
				struct mlx5_wqe_eth_seg *eseg)
{
	struct metadata_dst *md_dst = skb_metadata_dst(skb);
	u32 fs_id;

	fs_id = mlx5_macsec_fs_get_fs_id_from_hashtable(macsec->mdev->macsec_fs,
							&md_dst->u.macsec_info.sci);
	if (!fs_id)
		return;

	eseg->flow_table_metadata = cpu_to_be32(MLX5_ETH_WQE_FT_META_MACSEC | fs_id << 2);
}

void mlx5e_macsec_offload_handle_rx_skb(struct net_device *netdev,
					struct sk_buff *skb,
					struct mlx5_cqe64 *cqe)
{
	struct mlx5e_macsec_rx_sc_xarray_element *sc_xarray_element;
	u32 macsec_meta_data = be32_to_cpu(cqe->ft_metadata);
	struct mlx5e_priv *priv = macsec_netdev_priv(netdev);
	struct mlx5e_macsec_rx_sc *rx_sc;
	struct mlx5e_macsec *macsec;
	u32  fs_id;

	macsec = priv->macsec;
	if (!macsec)
		return;

	fs_id = MLX5_MACSEC_RX_METADAT_HANDLE(macsec_meta_data);

	rcu_read_lock();
	sc_xarray_element = xa_load(&macsec->sc_xarray, fs_id);
	rx_sc = sc_xarray_element->rx_sc;
	if (rx_sc) {
		dst_hold(&rx_sc->md_dst->dst);
		skb_dst_set(skb, &rx_sc->md_dst->dst);
	}

	rcu_read_unlock();
}

void mlx5e_macsec_build_netdev(struct mlx5e_priv *priv)
{
	struct net_device *netdev = priv->netdev;

	if (!mlx5e_is_macsec_device(priv->mdev))
		return;

	/* Enable MACsec */
	mlx5_core_dbg(priv->mdev, "mlx5e: MACsec acceleration enabled\n");
	netdev->macsec_ops = &macsec_offload_ops;
	netdev->features |= NETIF_F_HW_MACSEC;
	netif_keep_dst(netdev);
}

int mlx5e_macsec_init(struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5e_macsec *macsec = NULL;
	struct mlx5_macsec_fs *macsec_fs;
	int err;

	if (!mlx5e_is_macsec_device(priv->mdev)) {
		mlx5_core_dbg(mdev, "Not a MACsec offload device\n");
		return 0;
	}

	macsec = kzalloc(sizeof(*macsec), GFP_KERNEL);
	if (!macsec)
		return -ENOMEM;

	INIT_LIST_HEAD(&macsec->macsec_device_list_head);
	mutex_init(&macsec->lock);

	err = mlx5e_macsec_aso_init(&macsec->aso, priv->mdev);
	if (err) {
		mlx5_core_err(mdev, "MACsec offload: Failed to init aso, err=%d\n", err);
		goto err_aso;
	}

	macsec->wq = alloc_ordered_workqueue("mlx5e_macsec_%s", 0, priv->netdev->name);
	if (!macsec->wq) {
		err = -ENOMEM;
		goto err_wq;
	}

	xa_init_flags(&macsec->sc_xarray, XA_FLAGS_ALLOC1);

	priv->macsec = macsec;

	macsec->mdev = mdev;

	macsec_fs = mlx5_macsec_fs_init(mdev);
	if (!macsec_fs) {
		err = -ENOMEM;
		goto err_out;
	}

	mdev->macsec_fs = macsec_fs;

	macsec->nb.notifier_call = macsec_obj_change_event;
	mlx5_notifier_register(mdev, &macsec->nb);

	mlx5_core_dbg(mdev, "MACsec attached to netdevice\n");

	return 0;

err_out:
	destroy_workqueue(macsec->wq);
err_wq:
	mlx5e_macsec_aso_cleanup(&macsec->aso, priv->mdev);
err_aso:
	kfree(macsec);
	priv->macsec = NULL;
	return err;
}

void mlx5e_macsec_cleanup(struct mlx5e_priv *priv)
{
	struct mlx5e_macsec *macsec = priv->macsec;
	struct mlx5_core_dev *mdev = priv->mdev;

	if (!macsec)
		return;

	mlx5_notifier_unregister(mdev, &macsec->nb);
	mlx5_macsec_fs_cleanup(mdev->macsec_fs);
	destroy_workqueue(macsec->wq);
	mlx5e_macsec_aso_cleanup(&macsec->aso, mdev);
	mutex_destroy(&macsec->lock);
	kfree(macsec);
}
