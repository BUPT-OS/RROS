// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2019-2020, Mellanox Technologies inc. All rights reserved. */

#include <net/xdp_sock_drv.h>
#include "pool.h"
#include "setup.h"
#include "en/params.h"

static int mlx5e_xsk_map_pool(struct mlx5e_priv *priv,
			      struct xsk_buff_pool *pool)
{
	struct device *dev = mlx5_core_dma_dev(priv->mdev);

	return xsk_pool_dma_map(pool, dev, DMA_ATTR_SKIP_CPU_SYNC);
}

static void mlx5e_xsk_unmap_pool(struct mlx5e_priv *priv,
				 struct xsk_buff_pool *pool)
{
	return xsk_pool_dma_unmap(pool, DMA_ATTR_SKIP_CPU_SYNC);
}

static int mlx5e_xsk_get_pools(struct mlx5e_xsk *xsk)
{
	if (!xsk->pools) {
		xsk->pools = kcalloc(MLX5E_MAX_NUM_CHANNELS,
				     sizeof(*xsk->pools), GFP_KERNEL);
		if (unlikely(!xsk->pools))
			return -ENOMEM;
	}

	xsk->refcnt++;
	xsk->ever_used = true;

	return 0;
}

static void mlx5e_xsk_put_pools(struct mlx5e_xsk *xsk)
{
	if (!--xsk->refcnt) {
		kfree(xsk->pools);
		xsk->pools = NULL;
	}
}

static int mlx5e_xsk_add_pool(struct mlx5e_xsk *xsk, struct xsk_buff_pool *pool, u16 ix)
{
	int err;

	err = mlx5e_xsk_get_pools(xsk);
	if (unlikely(err))
		return err;

	xsk->pools[ix] = pool;
	return 0;
}

static void mlx5e_xsk_remove_pool(struct mlx5e_xsk *xsk, u16 ix)
{
	xsk->pools[ix] = NULL;

	mlx5e_xsk_put_pools(xsk);
}

static bool mlx5e_xsk_is_pool_sane(struct xsk_buff_pool *pool)
{
	return xsk_pool_get_headroom(pool) <= 0xffff &&
		xsk_pool_get_chunk_size(pool) <= 0xffff;
}

void mlx5e_build_xsk_param(struct xsk_buff_pool *pool, struct mlx5e_xsk_param *xsk)
{
	xsk->headroom = xsk_pool_get_headroom(pool);
	xsk->chunk_size = xsk_pool_get_chunk_size(pool);
	xsk->unaligned = pool->unaligned;
}

static int mlx5e_xsk_enable_locked(struct mlx5e_priv *priv,
				   struct xsk_buff_pool *pool, u16 ix)
{
	struct mlx5e_params *params = &priv->channels.params;
	struct mlx5e_xsk_param xsk;
	struct mlx5e_channel *c;
	int err;

	if (unlikely(mlx5e_xsk_get_pool(&priv->channels.params, &priv->xsk, ix)))
		return -EBUSY;

	if (unlikely(!mlx5e_xsk_is_pool_sane(pool)))
		return -EINVAL;

	err = mlx5e_xsk_map_pool(priv, pool);
	if (unlikely(err))
		return err;

	err = mlx5e_xsk_add_pool(&priv->xsk, pool, ix);
	if (unlikely(err))
		goto err_unmap_pool;

	mlx5e_build_xsk_param(pool, &xsk);

	if (priv->channels.params.rq_wq_type == MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ &&
	    mlx5e_mpwrq_umr_mode(priv->mdev, &xsk) == MLX5E_MPWRQ_UMR_MODE_OVERSIZED) {
		const char *recommendation = is_power_of_2(xsk.chunk_size) ?
			"Upgrade firmware" : "Disable striding RQ";

		mlx5_core_warn(priv->mdev, "Expected slowdown with XSK frame size %u. %s for better performance.\n",
			       xsk.chunk_size, recommendation);
	}

	if (!test_bit(MLX5E_STATE_OPENED, &priv->state)) {
		/* XSK objects will be created on open. */
		goto validate_closed;
	}

	if (!params->xdp_prog) {
		/* XSK objects will be created when an XDP program is set,
		 * and the channels are reopened.
		 */
		goto validate_closed;
	}

	c = priv->channels.c[ix];

	err = mlx5e_open_xsk(priv, params, &xsk, pool, c);
	if (unlikely(err))
		goto err_remove_pool;

	mlx5e_activate_xsk(c);
	mlx5e_trigger_napi_icosq(c);

	/* Don't wait for WQEs, because the newer xdpsock sample doesn't provide
	 * any Fill Ring entries at the setup stage.
	 */

	mlx5e_rx_res_xsk_update(priv->rx_res, &priv->channels, ix, true);

	mlx5e_deactivate_rq(&c->rq);
	mlx5e_flush_rq(&c->rq, MLX5_RQC_STATE_RDY);

	return 0;

err_remove_pool:
	mlx5e_xsk_remove_pool(&priv->xsk, ix);

err_unmap_pool:
	mlx5e_xsk_unmap_pool(priv, pool);

	return err;

validate_closed:
	/* Check the configuration in advance, rather than fail at a later stage
	 * (in mlx5e_xdp_set or on open) and end up with no channels.
	 */
	if (!mlx5e_validate_xsk_param(params, &xsk, priv->mdev)) {
		err = -EINVAL;
		goto err_remove_pool;
	}

	return 0;
}

static int mlx5e_xsk_disable_locked(struct mlx5e_priv *priv, u16 ix)
{
	struct xsk_buff_pool *pool = mlx5e_xsk_get_pool(&priv->channels.params,
						   &priv->xsk, ix);
	struct mlx5e_channel *c;

	if (unlikely(!pool))
		return -EINVAL;

	if (!test_bit(MLX5E_STATE_OPENED, &priv->state))
		goto remove_pool;

	/* XSK RQ and SQ are only created if XDP program is set. */
	if (!priv->channels.params.xdp_prog)
		goto remove_pool;

	c = priv->channels.c[ix];

	mlx5e_activate_rq(&c->rq);
	mlx5e_trigger_napi_icosq(c);
	mlx5e_wait_for_min_rx_wqes(&c->rq, MLX5E_RQ_WQES_TIMEOUT);

	mlx5e_rx_res_xsk_update(priv->rx_res, &priv->channels, ix, false);

	mlx5e_deactivate_xsk(c);
	mlx5e_close_xsk(c);

remove_pool:
	mlx5e_xsk_remove_pool(&priv->xsk, ix);
	mlx5e_xsk_unmap_pool(priv, pool);

	return 0;
}

static int mlx5e_xsk_enable_pool(struct mlx5e_priv *priv, struct xsk_buff_pool *pool,
				 u16 ix)
{
	int err;

	mutex_lock(&priv->state_lock);
	err = mlx5e_xsk_enable_locked(priv, pool, ix);
	mutex_unlock(&priv->state_lock);

	return err;
}

static int mlx5e_xsk_disable_pool(struct mlx5e_priv *priv, u16 ix)
{
	int err;

	mutex_lock(&priv->state_lock);
	err = mlx5e_xsk_disable_locked(priv, ix);
	mutex_unlock(&priv->state_lock);

	return err;
}

int mlx5e_xsk_setup_pool(struct net_device *dev, struct xsk_buff_pool *pool, u16 qid)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5e_params *params = &priv->channels.params;

	if (unlikely(qid >= params->num_channels))
		return -EINVAL;

	return pool ? mlx5e_xsk_enable_pool(priv, pool, qid) :
		      mlx5e_xsk_disable_pool(priv, qid);
}
