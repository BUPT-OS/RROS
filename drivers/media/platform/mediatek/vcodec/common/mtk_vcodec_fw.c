// SPDX-License-Identifier: GPL-2.0

#include "../decoder/mtk_vcodec_dec_drv.h"
#include "../encoder/mtk_vcodec_enc_drv.h"
#include "mtk_vcodec_fw_priv.h"

struct mtk_vcodec_fw *mtk_vcodec_fw_select(void *priv, enum mtk_vcodec_fw_type type,
					   enum mtk_vcodec_fw_use fw_use)
{
	struct platform_device *plat_dev;

	if (fw_use == ENCODER)
		plat_dev = ((struct mtk_vcodec_enc_dev *)priv)->plat_dev;
	else
		plat_dev = ((struct mtk_vcodec_dec_dev *)priv)->plat_dev;

	switch (type) {
	case VPU:
		return mtk_vcodec_fw_vpu_init(priv, fw_use);
	case SCP:
		return mtk_vcodec_fw_scp_init(priv, fw_use);
	default:
		dev_err(&plat_dev->dev, "Invalid vcodec fw type");
		return ERR_PTR(-EINVAL);
	}
}
EXPORT_SYMBOL_GPL(mtk_vcodec_fw_select);

void mtk_vcodec_fw_release(struct mtk_vcodec_fw *fw)
{
	fw->ops->release(fw);
}
EXPORT_SYMBOL_GPL(mtk_vcodec_fw_release);

int mtk_vcodec_fw_load_firmware(struct mtk_vcodec_fw *fw)
{
	return fw->ops->load_firmware(fw);
}
EXPORT_SYMBOL_GPL(mtk_vcodec_fw_load_firmware);

unsigned int mtk_vcodec_fw_get_vdec_capa(struct mtk_vcodec_fw *fw)
{
	return fw->ops->get_vdec_capa(fw);
}
EXPORT_SYMBOL_GPL(mtk_vcodec_fw_get_vdec_capa);

unsigned int mtk_vcodec_fw_get_venc_capa(struct mtk_vcodec_fw *fw)
{
	return fw->ops->get_venc_capa(fw);
}
EXPORT_SYMBOL_GPL(mtk_vcodec_fw_get_venc_capa);

void *mtk_vcodec_fw_map_dm_addr(struct mtk_vcodec_fw *fw, u32 mem_addr)
{
	return fw->ops->map_dm_addr(fw, mem_addr);
}
EXPORT_SYMBOL_GPL(mtk_vcodec_fw_map_dm_addr);

int mtk_vcodec_fw_ipi_register(struct mtk_vcodec_fw *fw, int id,
			       mtk_vcodec_ipi_handler handler,
			       const char *name, void *priv)
{
	return fw->ops->ipi_register(fw, id, handler, name, priv);
}
EXPORT_SYMBOL_GPL(mtk_vcodec_fw_ipi_register);

int mtk_vcodec_fw_ipi_send(struct mtk_vcodec_fw *fw, int id, void *buf,
			   unsigned int len, unsigned int wait)
{
	return fw->ops->ipi_send(fw, id, buf, len, wait);
}
EXPORT_SYMBOL_GPL(mtk_vcodec_fw_ipi_send);

int mtk_vcodec_fw_get_type(struct mtk_vcodec_fw *fw)
{
	return fw->type;
}
EXPORT_SYMBOL_GPL(mtk_vcodec_fw_get_type);
