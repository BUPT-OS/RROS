// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip Polarfire FPGA programming over slave SPI interface.
 */

#include <asm/unaligned.h>
#include <linux/delay.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spi/spi.h>

#define	MPF_SPI_ISC_ENABLE	0x0B
#define	MPF_SPI_ISC_DISABLE	0x0C
#define	MPF_SPI_READ_STATUS	0x00
#define	MPF_SPI_READ_DATA	0x01
#define	MPF_SPI_FRAME_INIT	0xAE
#define	MPF_SPI_FRAME		0xEE
#define	MPF_SPI_PRG_MODE	0x01
#define	MPF_SPI_RELEASE		0x23

#define	MPF_SPI_FRAME_SIZE	16

#define	MPF_HEADER_SIZE_OFFSET	24
#define	MPF_DATA_SIZE_OFFSET	55

#define	MPF_LOOKUP_TABLE_RECORD_SIZE		9
#define	MPF_LOOKUP_TABLE_BLOCK_ID_OFFSET	0
#define	MPF_LOOKUP_TABLE_BLOCK_START_OFFSET	1

#define	MPF_COMPONENTS_SIZE_ID	5
#define	MPF_BITSTREAM_ID	8

#define	MPF_BITS_PER_COMPONENT_SIZE	22

#define	MPF_STATUS_POLL_TIMEOUT		(2 * USEC_PER_SEC)
#define	MPF_STATUS_BUSY			BIT(0)
#define	MPF_STATUS_READY		BIT(1)
#define	MPF_STATUS_SPI_VIOLATION	BIT(2)
#define	MPF_STATUS_SPI_ERROR		BIT(3)

struct mpf_priv {
	struct spi_device *spi;
	bool program_mode;
	u8 tx __aligned(ARCH_KMALLOC_MINALIGN);
	u8 rx;
};

static int mpf_read_status(struct mpf_priv *priv)
{
	/*
	 * HW status is returned on MISO in the first byte after CS went
	 * active. However, first reading can be inadequate, so we submit
	 * two identical SPI transfers and use result of the later one.
	 */
	struct spi_transfer xfers[2] = {
		{
			.tx_buf = &priv->tx,
			.rx_buf = &priv->rx,
			.len = 1,
			.cs_change = 1,
		}, {
			.tx_buf = &priv->tx,
			.rx_buf = &priv->rx,
			.len = 1,
		},
	};
	u8 status;
	int ret;

	priv->tx = MPF_SPI_READ_STATUS;

	ret = spi_sync_transfer(priv->spi, xfers, 2);
	if (ret)
		return ret;

	status = priv->rx;

	if ((status & MPF_STATUS_SPI_VIOLATION) ||
	    (status & MPF_STATUS_SPI_ERROR))
		return -EIO;

	return status;
}

static enum fpga_mgr_states mpf_ops_state(struct fpga_manager *mgr)
{
	struct mpf_priv *priv = mgr->priv;
	bool program_mode;
	int status;

	program_mode = priv->program_mode;
	status = mpf_read_status(priv);

	if (!program_mode && !status)
		return FPGA_MGR_STATE_OPERATING;

	return FPGA_MGR_STATE_UNKNOWN;
}

static int mpf_ops_parse_header(struct fpga_manager *mgr,
				struct fpga_image_info *info,
				const char *buf, size_t count)
{
	size_t component_size_byte_num, component_size_byte_off,
	       components_size_start, bitstream_start,
	       block_id_offset, block_start_offset;
	u8 header_size, blocks_num, block_id;
	u32 block_start, component_size;
	u16 components_num, i;

	if (!buf) {
		dev_err(&mgr->dev, "Image buffer is not provided\n");
		return -EINVAL;
	}

	header_size = *(buf + MPF_HEADER_SIZE_OFFSET);
	if (header_size > count) {
		info->header_size = header_size;
		return -EAGAIN;
	}

	/*
	 * Go through look-up table to find out where actual bitstream starts
	 * and where sizes of components of the bitstream lies.
	 */
	blocks_num = *(buf + header_size - 1);
	block_id_offset = header_size + MPF_LOOKUP_TABLE_BLOCK_ID_OFFSET;
	block_start_offset = header_size + MPF_LOOKUP_TABLE_BLOCK_START_OFFSET;

	header_size += blocks_num * MPF_LOOKUP_TABLE_RECORD_SIZE;
	if (header_size > count) {
		info->header_size = header_size;
		return -EAGAIN;
	}

	components_size_start = 0;
	bitstream_start = 0;

	while (blocks_num--) {
		block_id = *(buf + block_id_offset);
		block_start = get_unaligned_le32(buf + block_start_offset);

		switch (block_id) {
		case MPF_BITSTREAM_ID:
			bitstream_start = block_start;
			info->header_size = block_start;
			if (block_start > count)
				return -EAGAIN;

			break;
		case MPF_COMPONENTS_SIZE_ID:
			components_size_start = block_start;
			break;
		default:
			break;
		}

		if (bitstream_start && components_size_start)
			break;

		block_id_offset += MPF_LOOKUP_TABLE_RECORD_SIZE;
		block_start_offset += MPF_LOOKUP_TABLE_RECORD_SIZE;
	}

	if (!bitstream_start || !components_size_start) {
		dev_err(&mgr->dev, "Failed to parse header look-up table\n");
		return -EFAULT;
	}

	/*
	 * Parse bitstream size.
	 * Sizes of components of the bitstream are 22-bits long placed next
	 * to each other. Image header should be extended by now up to where
	 * actual bitstream starts, so no need for overflow check anymore.
	 */
	components_num = get_unaligned_le16(buf + MPF_DATA_SIZE_OFFSET);

	for (i = 0; i < components_num; i++) {
		component_size_byte_num =
			(i * MPF_BITS_PER_COMPONENT_SIZE) / BITS_PER_BYTE;
		component_size_byte_off =
			(i * MPF_BITS_PER_COMPONENT_SIZE) % BITS_PER_BYTE;

		component_size = get_unaligned_le32(buf +
						    components_size_start +
						    component_size_byte_num);
		component_size >>= component_size_byte_off;
		component_size &= GENMASK(MPF_BITS_PER_COMPONENT_SIZE - 1, 0);

		info->data_size += component_size * MPF_SPI_FRAME_SIZE;
	}

	return 0;
}

static int mpf_poll_status(struct mpf_priv *priv, u8 mask)
{
	int ret, status;

	/*
	 * Busy poll HW status. Polling stops if any of the following
	 * conditions are met:
	 *  - timeout is reached
	 *  - mpf_read_status() returns an error
	 *  - busy bit is cleared AND mask bits are set
	 */
	ret = read_poll_timeout(mpf_read_status, status,
				(status < 0) ||
				((status & (MPF_STATUS_BUSY | mask)) == mask),
				0, MPF_STATUS_POLL_TIMEOUT, false, priv);
	if (ret < 0)
		return ret;

	return status;
}

static int mpf_spi_write(struct mpf_priv *priv, const void *buf, size_t buf_size)
{
	int status = mpf_poll_status(priv, 0);

	if (status < 0)
		return status;

	return spi_write_then_read(priv->spi, buf, buf_size, NULL, 0);
}

static int mpf_spi_write_then_read(struct mpf_priv *priv,
				   const void *txbuf, size_t txbuf_size,
				   void *rxbuf, size_t rxbuf_size)
{
	const u8 read_command[] = { MPF_SPI_READ_DATA };
	int ret;

	ret = mpf_spi_write(priv, txbuf, txbuf_size);
	if (ret)
		return ret;

	ret = mpf_poll_status(priv, MPF_STATUS_READY);
	if (ret < 0)
		return ret;

	return spi_write_then_read(priv->spi, read_command, sizeof(read_command),
				   rxbuf, rxbuf_size);
}

static int mpf_ops_write_init(struct fpga_manager *mgr,
			      struct fpga_image_info *info, const char *buf,
			      size_t count)
{
	const u8 program_mode[] = { MPF_SPI_FRAME_INIT, MPF_SPI_PRG_MODE };
	const u8 isc_en_command[] = { MPF_SPI_ISC_ENABLE };
	struct mpf_priv *priv = mgr->priv;
	struct device *dev = &mgr->dev;
	u32 isc_ret = 0;
	int ret;

	if (info->flags & FPGA_MGR_PARTIAL_RECONFIG) {
		dev_err(dev, "Partial reconfiguration is not supported\n");
		return -EOPNOTSUPP;
	}

	ret = mpf_spi_write_then_read(priv, isc_en_command, sizeof(isc_en_command),
				      &isc_ret, sizeof(isc_ret));
	if (ret || isc_ret) {
		dev_err(dev, "Failed to enable ISC: spi_ret %d, isc_ret %u\n",
			ret, isc_ret);
		return -EFAULT;
	}

	ret = mpf_spi_write(priv, program_mode, sizeof(program_mode));
	if (ret) {
		dev_err(dev, "Failed to enter program mode: %d\n", ret);
		return ret;
	}

	priv->program_mode = true;

	return 0;
}

static int mpf_spi_frame_write(struct mpf_priv *priv, const char *buf)
{
	struct spi_transfer xfers[2] = {
		{
			.tx_buf = &priv->tx,
			.len = 1,
		}, {
			.tx_buf = buf,
			.len = MPF_SPI_FRAME_SIZE,
		},
	};
	int ret;

	ret = mpf_poll_status(priv, 0);
	if (ret < 0)
		return ret;

	priv->tx = MPF_SPI_FRAME;

	return spi_sync_transfer(priv->spi, xfers, ARRAY_SIZE(xfers));
}

static int mpf_ops_write(struct fpga_manager *mgr, const char *buf, size_t count)
{
	struct mpf_priv *priv = mgr->priv;
	struct device *dev = &mgr->dev;
	int ret, i;

	if (count % MPF_SPI_FRAME_SIZE) {
		dev_err(dev, "Bitstream size is not a multiple of %d\n",
			MPF_SPI_FRAME_SIZE);
		return -EINVAL;
	}

	for (i = 0; i < count / MPF_SPI_FRAME_SIZE; i++) {
		ret = mpf_spi_frame_write(priv, buf + i * MPF_SPI_FRAME_SIZE);
		if (ret) {
			dev_err(dev, "Failed to write bitstream frame %d/%zu\n",
				i, count / MPF_SPI_FRAME_SIZE);
			return ret;
		}
	}

	return 0;
}

static int mpf_ops_write_complete(struct fpga_manager *mgr,
				  struct fpga_image_info *info)
{
	const u8 isc_dis_command[] = { MPF_SPI_ISC_DISABLE };
	const u8 release_command[] = { MPF_SPI_RELEASE };
	struct mpf_priv *priv = mgr->priv;
	struct device *dev = &mgr->dev;
	int ret;

	ret = mpf_spi_write(priv, isc_dis_command, sizeof(isc_dis_command));
	if (ret) {
		dev_err(dev, "Failed to disable ISC: %d\n", ret);
		return ret;
	}

	usleep_range(1000, 2000);

	ret = mpf_spi_write(priv, release_command, sizeof(release_command));
	if (ret) {
		dev_err(dev, "Failed to exit program mode: %d\n", ret);
		return ret;
	}

	priv->program_mode = false;

	return 0;
}

static const struct fpga_manager_ops mpf_ops = {
	.state = mpf_ops_state,
	.initial_header_size = 71,
	.skip_header = true,
	.parse_header = mpf_ops_parse_header,
	.write_init = mpf_ops_write_init,
	.write = mpf_ops_write,
	.write_complete = mpf_ops_write_complete,
};

static int mpf_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct fpga_manager *mgr;
	struct mpf_priv *priv;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->spi = spi;

	mgr = devm_fpga_mgr_register(dev, "Microchip Polarfire SPI FPGA Manager",
				     &mpf_ops, priv);

	return PTR_ERR_OR_ZERO(mgr);
}

static const struct spi_device_id mpf_spi_ids[] = {
	{ .name = "mpf-spi-fpga-mgr", },
	{},
};
MODULE_DEVICE_TABLE(spi, mpf_spi_ids);

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id mpf_of_ids[] = {
	{ .compatible = "microchip,mpf-spi-fpga-mgr" },
	{},
};
MODULE_DEVICE_TABLE(of, mpf_of_ids);
#endif /* IS_ENABLED(CONFIG_OF) */

static struct spi_driver mpf_driver = {
	.probe = mpf_probe,
	.id_table = mpf_spi_ids,
	.driver = {
		.name = "microchip_mpf_spi_fpga_mgr",
		.of_match_table = of_match_ptr(mpf_of_ids),
	},
};

module_spi_driver(mpf_driver);

MODULE_DESCRIPTION("Microchip Polarfire SPI FPGA Manager");
MODULE_AUTHOR("Ivan Bornyakov <i.bornyakov@metrotek.ru>");
MODULE_LICENSE("GPL");
