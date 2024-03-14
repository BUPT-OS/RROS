// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2009-2017, The Linux Foundation. All rights reserved.
 * Copyright (c) 2017-2019, Linaro Ltd.
 */

#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/socinfo.h>
#include <linux/string.h>
#include <linux/stringify.h>
#include <linux/sys_soc.h>
#include <linux/types.h>

#include <asm/unaligned.h>

#include <dt-bindings/arm/qcom,ids.h>

/*
 * SoC version type with major number in the upper 16 bits and minor
 * number in the lower 16 bits.
 */
#define SOCINFO_MAJOR(ver) (((ver) >> 16) & 0xffff)
#define SOCINFO_MINOR(ver) ((ver) & 0xffff)
#define SOCINFO_VERSION(maj, min)  ((((maj) & 0xffff) << 16)|((min) & 0xffff))

/* Helper macros to create soc_id table */
#define qcom_board_id(id) QCOM_ID_ ## id, __stringify(id)
#define qcom_board_id_named(id, name) QCOM_ID_ ## id, (name)

#ifdef CONFIG_DEBUG_FS
#define SMEM_IMAGE_VERSION_BLOCKS_COUNT        32
#define SMEM_IMAGE_VERSION_SIZE                4096
#define SMEM_IMAGE_VERSION_NAME_SIZE           75
#define SMEM_IMAGE_VERSION_VARIANT_SIZE        20
#define SMEM_IMAGE_VERSION_OEM_SIZE            32

/*
 * SMEM Image table indices
 */
#define SMEM_IMAGE_TABLE_BOOT_INDEX     0
#define SMEM_IMAGE_TABLE_TZ_INDEX       1
#define SMEM_IMAGE_TABLE_RPM_INDEX      3
#define SMEM_IMAGE_TABLE_APPS_INDEX     10
#define SMEM_IMAGE_TABLE_MPSS_INDEX     11
#define SMEM_IMAGE_TABLE_ADSP_INDEX     12
#define SMEM_IMAGE_TABLE_CNSS_INDEX     13
#define SMEM_IMAGE_TABLE_VIDEO_INDEX    14
#define SMEM_IMAGE_VERSION_TABLE       469

/*
 * SMEM Image table names
 */
static const char *const socinfo_image_names[] = {
	[SMEM_IMAGE_TABLE_ADSP_INDEX] = "adsp",
	[SMEM_IMAGE_TABLE_APPS_INDEX] = "apps",
	[SMEM_IMAGE_TABLE_BOOT_INDEX] = "boot",
	[SMEM_IMAGE_TABLE_CNSS_INDEX] = "cnss",
	[SMEM_IMAGE_TABLE_MPSS_INDEX] = "mpss",
	[SMEM_IMAGE_TABLE_RPM_INDEX] = "rpm",
	[SMEM_IMAGE_TABLE_TZ_INDEX] = "tz",
	[SMEM_IMAGE_TABLE_VIDEO_INDEX] = "video",
};

static const char *const pmic_models[] = {
	[0]  = "Unknown PMIC model",
	[1]  = "PM8941",
	[2]  = "PM8841",
	[3]  = "PM8019",
	[4]  = "PM8226",
	[5]  = "PM8110",
	[6]  = "PMA8084",
	[7]  = "PMI8962",
	[8]  = "PMD9635",
	[9]  = "PM8994",
	[10] = "PMI8994",
	[11] = "PM8916",
	[12] = "PM8004",
	[13] = "PM8909/PM8058",
	[14] = "PM8028",
	[15] = "PM8901",
	[16] = "PM8950/PM8027",
	[17] = "PMI8950/ISL9519",
	[18] = "PMK8001/PM8921",
	[19] = "PMI8996/PM8018",
	[20] = "PM8998/PM8015",
	[21] = "PMI8998/PM8014",
	[22] = "PM8821",
	[23] = "PM8038",
	[24] = "PM8005/PM8922",
	[25] = "PM8917",
	[26] = "PM660L",
	[27] = "PM660",
	[30] = "PM8150",
	[31] = "PM8150L",
	[32] = "PM8150B",
	[33] = "PMK8002",
	[36] = "PM8009",
	[37] = "PMI632",
	[38] = "PM8150C",
	[40] = "PM6150",
	[41] = "SMB2351",
	[44] = "PM8008",
	[45] = "PM6125",
	[46] = "PM7250B",
	[47] = "PMK8350",
	[48] = "PM8350",
	[49] = "PM8350C",
	[50] = "PM8350B",
	[51] = "PMR735A",
	[52] = "PMR735B",
	[55] = "PM2250",
	[58] = "PM8450",
	[65] = "PM8010",
};

struct socinfo_params {
	u32 raw_device_family;
	u32 hw_plat_subtype;
	u32 accessory_chip;
	u32 raw_device_num;
	u32 chip_family;
	u32 foundry_id;
	u32 plat_ver;
	u32 raw_ver;
	u32 hw_plat;
	u32 fmt;
	u32 nproduct_id;
	u32 num_clusters;
	u32 ncluster_array_offset;
	u32 num_subset_parts;
	u32 nsubset_parts_array_offset;
	u32 nmodem_supported;
	u32 feature_code;
	u32 pcode;
	u32 oem_variant;
	u32 num_func_clusters;
	u32 boot_cluster;
	u32 boot_core;
};

struct smem_image_version {
	char name[SMEM_IMAGE_VERSION_NAME_SIZE];
	char variant[SMEM_IMAGE_VERSION_VARIANT_SIZE];
	char pad;
	char oem[SMEM_IMAGE_VERSION_OEM_SIZE];
};
#endif /* CONFIG_DEBUG_FS */

struct qcom_socinfo {
	struct soc_device *soc_dev;
	struct soc_device_attribute attr;
#ifdef CONFIG_DEBUG_FS
	struct dentry *dbg_root;
	struct socinfo_params info;
#endif /* CONFIG_DEBUG_FS */
};

struct soc_id {
	unsigned int id;
	const char *name;
};

static const struct soc_id soc_id[] = {
	{ qcom_board_id(MSM8260) },
	{ qcom_board_id(MSM8660) },
	{ qcom_board_id(APQ8060) },
	{ qcom_board_id(MSM8960) },
	{ qcom_board_id(APQ8064) },
	{ qcom_board_id(MSM8930) },
	{ qcom_board_id(MSM8630) },
	{ qcom_board_id(MSM8230) },
	{ qcom_board_id(APQ8030) },
	{ qcom_board_id(MSM8627) },
	{ qcom_board_id(MSM8227) },
	{ qcom_board_id(MSM8660A) },
	{ qcom_board_id(MSM8260A) },
	{ qcom_board_id(APQ8060A) },
	{ qcom_board_id(MSM8974) },
	{ qcom_board_id(MSM8225) },
	{ qcom_board_id(MSM8625) },
	{ qcom_board_id(MPQ8064) },
	{ qcom_board_id(MSM8960AB) },
	{ qcom_board_id(APQ8060AB) },
	{ qcom_board_id(MSM8260AB) },
	{ qcom_board_id(MSM8660AB) },
	{ qcom_board_id(MSM8930AA) },
	{ qcom_board_id(MSM8630AA) },
	{ qcom_board_id(MSM8230AA) },
	{ qcom_board_id(MSM8626) },
	{ qcom_board_id(MSM8610) },
	{ qcom_board_id(APQ8064AB) },
	{ qcom_board_id(MSM8930AB) },
	{ qcom_board_id(MSM8630AB) },
	{ qcom_board_id(MSM8230AB) },
	{ qcom_board_id(APQ8030AB) },
	{ qcom_board_id(MSM8226) },
	{ qcom_board_id(MSM8526) },
	{ qcom_board_id(APQ8030AA) },
	{ qcom_board_id(MSM8110) },
	{ qcom_board_id(MSM8210) },
	{ qcom_board_id(MSM8810) },
	{ qcom_board_id(MSM8212) },
	{ qcom_board_id(MSM8612) },
	{ qcom_board_id(MSM8112) },
	{ qcom_board_id(MSM8125) },
	{ qcom_board_id(MSM8225Q) },
	{ qcom_board_id(MSM8625Q) },
	{ qcom_board_id(MSM8125Q) },
	{ qcom_board_id(APQ8064AA) },
	{ qcom_board_id(APQ8084) },
	{ qcom_board_id(MSM8130) },
	{ qcom_board_id(MSM8130AA) },
	{ qcom_board_id(MSM8130AB) },
	{ qcom_board_id(MSM8627AA) },
	{ qcom_board_id(MSM8227AA) },
	{ qcom_board_id(APQ8074) },
	{ qcom_board_id(MSM8274) },
	{ qcom_board_id(MSM8674) },
	{ qcom_board_id(MDM9635) },
	{ qcom_board_id_named(MSM8974PRO_AC, "MSM8974PRO-AC") },
	{ qcom_board_id(MSM8126) },
	{ qcom_board_id(APQ8026) },
	{ qcom_board_id(MSM8926) },
	{ qcom_board_id(IPQ8062) },
	{ qcom_board_id(IPQ8064) },
	{ qcom_board_id(IPQ8066) },
	{ qcom_board_id(IPQ8068) },
	{ qcom_board_id(MSM8326) },
	{ qcom_board_id(MSM8916) },
	{ qcom_board_id(MSM8994) },
	{ qcom_board_id_named(APQ8074PRO_AA, "APQ8074PRO-AA") },
	{ qcom_board_id_named(APQ8074PRO_AB, "APQ8074PRO-AB") },
	{ qcom_board_id_named(APQ8074PRO_AC, "APQ8074PRO-AC") },
	{ qcom_board_id_named(MSM8274PRO_AA, "MSM8274PRO-AA") },
	{ qcom_board_id_named(MSM8274PRO_AB, "MSM8274PRO-AB") },
	{ qcom_board_id_named(MSM8274PRO_AC, "MSM8274PRO-AC") },
	{ qcom_board_id_named(MSM8674PRO_AA, "MSM8674PRO-AA") },
	{ qcom_board_id_named(MSM8674PRO_AB, "MSM8674PRO-AB") },
	{ qcom_board_id_named(MSM8674PRO_AC, "MSM8674PRO-AC") },
	{ qcom_board_id_named(MSM8974PRO_AA, "MSM8974PRO-AA") },
	{ qcom_board_id_named(MSM8974PRO_AB, "MSM8974PRO-AB") },
	{ qcom_board_id(APQ8028) },
	{ qcom_board_id(MSM8128) },
	{ qcom_board_id(MSM8228) },
	{ qcom_board_id(MSM8528) },
	{ qcom_board_id(MSM8628) },
	{ qcom_board_id(MSM8928) },
	{ qcom_board_id(MSM8510) },
	{ qcom_board_id(MSM8512) },
	{ qcom_board_id(MSM8936) },
	{ qcom_board_id(MDM9640) },
	{ qcom_board_id(MSM8939) },
	{ qcom_board_id(APQ8036) },
	{ qcom_board_id(APQ8039) },
	{ qcom_board_id(MSM8236) },
	{ qcom_board_id(MSM8636) },
	{ qcom_board_id(MSM8909) },
	{ qcom_board_id(MSM8996) },
	{ qcom_board_id(APQ8016) },
	{ qcom_board_id(MSM8216) },
	{ qcom_board_id(MSM8116) },
	{ qcom_board_id(MSM8616) },
	{ qcom_board_id(MSM8992) },
	{ qcom_board_id(APQ8092) },
	{ qcom_board_id(APQ8094) },
	{ qcom_board_id(MSM8209) },
	{ qcom_board_id(MSM8208) },
	{ qcom_board_id(MDM9209) },
	{ qcom_board_id(MDM9309) },
	{ qcom_board_id(MDM9609) },
	{ qcom_board_id(MSM8239) },
	{ qcom_board_id(MSM8952) },
	{ qcom_board_id(APQ8009) },
	{ qcom_board_id(MSM8956) },
	{ qcom_board_id(MSM8929) },
	{ qcom_board_id(MSM8629) },
	{ qcom_board_id(MSM8229) },
	{ qcom_board_id(APQ8029) },
	{ qcom_board_id(APQ8056) },
	{ qcom_board_id(MSM8609) },
	{ qcom_board_id(APQ8076) },
	{ qcom_board_id(MSM8976) },
	{ qcom_board_id(IPQ8065) },
	{ qcom_board_id(IPQ8069) },
	{ qcom_board_id(MDM9650) },
	{ qcom_board_id(MDM9655) },
	{ qcom_board_id(MDM9250) },
	{ qcom_board_id(MDM9255) },
	{ qcom_board_id(MDM9350) },
	{ qcom_board_id(APQ8052) },
	{ qcom_board_id(MDM9607) },
	{ qcom_board_id(APQ8096) },
	{ qcom_board_id(MSM8998) },
	{ qcom_board_id(MSM8953) },
	{ qcom_board_id(MSM8937) },
	{ qcom_board_id(APQ8037) },
	{ qcom_board_id(MDM8207) },
	{ qcom_board_id(MDM9207) },
	{ qcom_board_id(MDM9307) },
	{ qcom_board_id(MDM9628) },
	{ qcom_board_id(MSM8909W) },
	{ qcom_board_id(APQ8009W) },
	{ qcom_board_id(MSM8996L) },
	{ qcom_board_id(MSM8917) },
	{ qcom_board_id(APQ8053) },
	{ qcom_board_id(MSM8996SG) },
	{ qcom_board_id(APQ8017) },
	{ qcom_board_id(MSM8217) },
	{ qcom_board_id(MSM8617) },
	{ qcom_board_id(MSM8996AU) },
	{ qcom_board_id(APQ8096AU) },
	{ qcom_board_id(APQ8096SG) },
	{ qcom_board_id(MSM8940) },
	{ qcom_board_id(SDX201) },
	{ qcom_board_id(SDM660) },
	{ qcom_board_id(SDM630) },
	{ qcom_board_id(APQ8098) },
	{ qcom_board_id(MSM8920) },
	{ qcom_board_id(SDM845) },
	{ qcom_board_id(MDM9206) },
	{ qcom_board_id(IPQ8074) },
	{ qcom_board_id(SDA660) },
	{ qcom_board_id(SDM658) },
	{ qcom_board_id(SDA658) },
	{ qcom_board_id(SDA630) },
	{ qcom_board_id(MSM8905) },
	{ qcom_board_id(SDX202) },
	{ qcom_board_id(SDM450) },
	{ qcom_board_id(SM8150) },
	{ qcom_board_id(SDA845) },
	{ qcom_board_id(IPQ8072) },
	{ qcom_board_id(IPQ8076) },
	{ qcom_board_id(IPQ8078) },
	{ qcom_board_id(SDM636) },
	{ qcom_board_id(SDA636) },
	{ qcom_board_id(SDM632) },
	{ qcom_board_id(SDA632) },
	{ qcom_board_id(SDA450) },
	{ qcom_board_id(SDM439) },
	{ qcom_board_id(SDM429) },
	{ qcom_board_id(SM8250) },
	{ qcom_board_id(SA8155) },
	{ qcom_board_id(SDA439) },
	{ qcom_board_id(SDA429) },
	{ qcom_board_id(SM7150) },
	{ qcom_board_id(IPQ8070) },
	{ qcom_board_id(IPQ8071) },
	{ qcom_board_id(QM215) },
	{ qcom_board_id(IPQ8072A) },
	{ qcom_board_id(IPQ8074A) },
	{ qcom_board_id(IPQ8076A) },
	{ qcom_board_id(IPQ8078A) },
	{ qcom_board_id(SM6125) },
	{ qcom_board_id(IPQ8070A) },
	{ qcom_board_id(IPQ8071A) },
	{ qcom_board_id(IPQ6018) },
	{ qcom_board_id(IPQ6028) },
	{ qcom_board_id(SDM429W) },
	{ qcom_board_id(SM4250) },
	{ qcom_board_id(IPQ6000) },
	{ qcom_board_id(IPQ6010) },
	{ qcom_board_id(SC7180) },
	{ qcom_board_id(SM6350) },
	{ qcom_board_id(QCM2150) },
	{ qcom_board_id(SDA429W) },
	{ qcom_board_id(SM8350) },
	{ qcom_board_id(QCM2290) },
	{ qcom_board_id(SM7125) },
	{ qcom_board_id(SM6115) },
	{ qcom_board_id(IPQ5010) },
	{ qcom_board_id(IPQ5018) },
	{ qcom_board_id(IPQ5028) },
	{ qcom_board_id(SC8280XP) },
	{ qcom_board_id(IPQ6005) },
	{ qcom_board_id(QRB5165) },
	{ qcom_board_id(SM8450) },
	{ qcom_board_id(SM7225) },
	{ qcom_board_id(SA8295P) },
	{ qcom_board_id(SA8540P) },
	{ qcom_board_id(QCM4290) },
	{ qcom_board_id(QCS4290) },
	{ qcom_board_id_named(SM8450_2, "SM8450") },
	{ qcom_board_id_named(SM8450_3, "SM8450") },
	{ qcom_board_id(SC7280) },
	{ qcom_board_id(SC7180P) },
	{ qcom_board_id(IPQ5000) },
	{ qcom_board_id(IPQ0509) },
	{ qcom_board_id(IPQ0518) },
	{ qcom_board_id(SM6375) },
	{ qcom_board_id(IPQ9514) },
	{ qcom_board_id(IPQ9550) },
	{ qcom_board_id(IPQ9554) },
	{ qcom_board_id(IPQ9570) },
	{ qcom_board_id(IPQ9574) },
	{ qcom_board_id(SM8550) },
	{ qcom_board_id(IPQ5016) },
	{ qcom_board_id(IPQ9510) },
	{ qcom_board_id(QRB4210) },
	{ qcom_board_id(QRB2210) },
	{ qcom_board_id(SA8775P) },
	{ qcom_board_id(QRU1000) },
	{ qcom_board_id(QDU1000) },
	{ qcom_board_id(SM4450) },
	{ qcom_board_id(QDU1010) },
	{ qcom_board_id(QRU1032) },
	{ qcom_board_id(QRU1052) },
	{ qcom_board_id(QRU1062) },
	{ qcom_board_id(IPQ5332) },
	{ qcom_board_id(IPQ5322) },
	{ qcom_board_id(IPQ5312) },
	{ qcom_board_id(IPQ5302) },
	{ qcom_board_id(IPQ5300) },
};

static const char *socinfo_machine(struct device *dev, unsigned int id)
{
	int idx;

	for (idx = 0; idx < ARRAY_SIZE(soc_id); idx++) {
		if (soc_id[idx].id == id)
			return soc_id[idx].name;
	}

	return NULL;
}

#ifdef CONFIG_DEBUG_FS

#define QCOM_OPEN(name, _func)						\
static int qcom_open_##name(struct inode *inode, struct file *file)	\
{									\
	return single_open(file, _func, inode->i_private);		\
}									\
									\
static const struct file_operations qcom_ ##name## _ops = {		\
	.open = qcom_open_##name,					\
	.read = seq_read,						\
	.llseek = seq_lseek,						\
	.release = single_release,					\
}

#define DEBUGFS_ADD(info, name)						\
	debugfs_create_file(__stringify(name), 0444,			\
			    qcom_socinfo->dbg_root,			\
			    info, &qcom_ ##name## _ops)


static int qcom_show_build_id(struct seq_file *seq, void *p)
{
	struct socinfo *socinfo = seq->private;

	seq_printf(seq, "%s\n", socinfo->build_id);

	return 0;
}

static int qcom_show_pmic_model(struct seq_file *seq, void *p)
{
	struct socinfo *socinfo = seq->private;
	int model = SOCINFO_MINOR(le32_to_cpu(socinfo->pmic_model));

	if (model < 0)
		return -EINVAL;

	if (model < ARRAY_SIZE(pmic_models) && pmic_models[model])
		seq_printf(seq, "%s\n", pmic_models[model]);
	else
		seq_printf(seq, "unknown (%d)\n", model);

	return 0;
}

static int qcom_show_pmic_model_array(struct seq_file *seq, void *p)
{
	struct socinfo *socinfo = seq->private;
	unsigned int num_pmics = le32_to_cpu(socinfo->num_pmics);
	unsigned int pmic_array_offset = le32_to_cpu(socinfo->pmic_array_offset);
	int i;
	void *ptr = socinfo;

	ptr += pmic_array_offset;

	/* No need for bounds checking, it happened at socinfo_debugfs_init */
	for (i = 0; i < num_pmics; i++) {
		unsigned int model = SOCINFO_MINOR(get_unaligned_le32(ptr + 2 * i * sizeof(u32)));
		unsigned int die_rev = get_unaligned_le32(ptr + (2 * i + 1) * sizeof(u32));

		if (model < ARRAY_SIZE(pmic_models) && pmic_models[model])
			seq_printf(seq, "%s %u.%u\n", pmic_models[model],
				   SOCINFO_MAJOR(die_rev),
				   SOCINFO_MINOR(die_rev));
		else
			seq_printf(seq, "unknown (%d)\n", model);
	}

	return 0;
}

static int qcom_show_pmic_die_revision(struct seq_file *seq, void *p)
{
	struct socinfo *socinfo = seq->private;

	seq_printf(seq, "%u.%u\n",
		   SOCINFO_MAJOR(le32_to_cpu(socinfo->pmic_die_rev)),
		   SOCINFO_MINOR(le32_to_cpu(socinfo->pmic_die_rev)));

	return 0;
}

static int qcom_show_chip_id(struct seq_file *seq, void *p)
{
	struct socinfo *socinfo = seq->private;

	seq_printf(seq, "%s\n", socinfo->chip_id);

	return 0;
}

QCOM_OPEN(build_id, qcom_show_build_id);
QCOM_OPEN(pmic_model, qcom_show_pmic_model);
QCOM_OPEN(pmic_model_array, qcom_show_pmic_model_array);
QCOM_OPEN(pmic_die_rev, qcom_show_pmic_die_revision);
QCOM_OPEN(chip_id, qcom_show_chip_id);

#define DEFINE_IMAGE_OPS(type)					\
static int show_image_##type(struct seq_file *seq, void *p)		  \
{								  \
	struct smem_image_version *image_version = seq->private;  \
	if (image_version->type[0] != '\0')			  \
		seq_printf(seq, "%s\n", image_version->type);	  \
	return 0;						  \
}								  \
static int open_image_##type(struct inode *inode, struct file *file)	  \
{									  \
	return single_open(file, show_image_##type, inode->i_private); \
}									  \
									  \
static const struct file_operations qcom_image_##type##_ops = {	  \
	.open = open_image_##type,					  \
	.read = seq_read,						  \
	.llseek = seq_lseek,						  \
	.release = single_release,					  \
}

DEFINE_IMAGE_OPS(name);
DEFINE_IMAGE_OPS(variant);
DEFINE_IMAGE_OPS(oem);

static void socinfo_debugfs_init(struct qcom_socinfo *qcom_socinfo,
				 struct socinfo *info, size_t info_size)
{
	struct smem_image_version *versions;
	struct dentry *dentry;
	size_t size;
	int i;
	unsigned int num_pmics;
	unsigned int pmic_array_offset;

	qcom_socinfo->dbg_root = debugfs_create_dir("qcom_socinfo", NULL);

	qcom_socinfo->info.fmt = __le32_to_cpu(info->fmt);

	debugfs_create_x32("info_fmt", 0444, qcom_socinfo->dbg_root,
			   &qcom_socinfo->info.fmt);

	switch (qcom_socinfo->info.fmt) {
	case SOCINFO_VERSION(0, 19):
		qcom_socinfo->info.num_func_clusters = __le32_to_cpu(info->num_func_clusters);
		qcom_socinfo->info.boot_cluster = __le32_to_cpu(info->boot_cluster);
		qcom_socinfo->info.boot_core = __le32_to_cpu(info->boot_core);

		debugfs_create_u32("num_func_clusters", 0444, qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.num_func_clusters);
		debugfs_create_u32("boot_cluster", 0444, qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.boot_cluster);
		debugfs_create_u32("boot_core", 0444, qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.boot_core);
		fallthrough;
	case SOCINFO_VERSION(0, 18):
	case SOCINFO_VERSION(0, 17):
		qcom_socinfo->info.oem_variant = __le32_to_cpu(info->oem_variant);
		debugfs_create_u32("oem_variant", 0444, qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.oem_variant);
		fallthrough;
	case SOCINFO_VERSION(0, 16):
		qcom_socinfo->info.feature_code = __le32_to_cpu(info->feature_code);
		qcom_socinfo->info.pcode = __le32_to_cpu(info->pcode);

		debugfs_create_u32("feature_code", 0444, qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.feature_code);
		debugfs_create_u32("pcode", 0444, qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.pcode);
		fallthrough;
	case SOCINFO_VERSION(0, 15):
		qcom_socinfo->info.nmodem_supported = __le32_to_cpu(info->nmodem_supported);

		debugfs_create_u32("nmodem_supported", 0444, qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.nmodem_supported);
		fallthrough;
	case SOCINFO_VERSION(0, 14):
		qcom_socinfo->info.num_clusters = __le32_to_cpu(info->num_clusters);
		qcom_socinfo->info.ncluster_array_offset = __le32_to_cpu(info->ncluster_array_offset);
		qcom_socinfo->info.num_subset_parts = __le32_to_cpu(info->num_subset_parts);
		qcom_socinfo->info.nsubset_parts_array_offset =
			__le32_to_cpu(info->nsubset_parts_array_offset);

		debugfs_create_u32("num_clusters", 0444, qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.num_clusters);
		debugfs_create_u32("ncluster_array_offset", 0444, qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.ncluster_array_offset);
		debugfs_create_u32("num_subset_parts", 0444, qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.num_subset_parts);
		debugfs_create_u32("nsubset_parts_array_offset", 0444, qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.nsubset_parts_array_offset);
		fallthrough;
	case SOCINFO_VERSION(0, 13):
		qcom_socinfo->info.nproduct_id = __le32_to_cpu(info->nproduct_id);

		debugfs_create_u32("nproduct_id", 0444, qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.nproduct_id);
		DEBUGFS_ADD(info, chip_id);
		fallthrough;
	case SOCINFO_VERSION(0, 12):
		qcom_socinfo->info.chip_family =
			__le32_to_cpu(info->chip_family);
		qcom_socinfo->info.raw_device_family =
			__le32_to_cpu(info->raw_device_family);
		qcom_socinfo->info.raw_device_num =
			__le32_to_cpu(info->raw_device_num);

		debugfs_create_x32("chip_family", 0444, qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.chip_family);
		debugfs_create_x32("raw_device_family", 0444,
				   qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.raw_device_family);
		debugfs_create_x32("raw_device_number", 0444,
				   qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.raw_device_num);
		fallthrough;
	case SOCINFO_VERSION(0, 11):
		num_pmics = le32_to_cpu(info->num_pmics);
		pmic_array_offset = le32_to_cpu(info->pmic_array_offset);
		if (pmic_array_offset + 2 * num_pmics * sizeof(u32) <= info_size)
			DEBUGFS_ADD(info, pmic_model_array);
		fallthrough;
	case SOCINFO_VERSION(0, 10):
	case SOCINFO_VERSION(0, 9):
		qcom_socinfo->info.foundry_id = __le32_to_cpu(info->foundry_id);

		debugfs_create_u32("foundry_id", 0444, qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.foundry_id);
		fallthrough;
	case SOCINFO_VERSION(0, 8):
	case SOCINFO_VERSION(0, 7):
		DEBUGFS_ADD(info, pmic_model);
		DEBUGFS_ADD(info, pmic_die_rev);
		fallthrough;
	case SOCINFO_VERSION(0, 6):
		qcom_socinfo->info.hw_plat_subtype =
			__le32_to_cpu(info->hw_plat_subtype);

		debugfs_create_u32("hardware_platform_subtype", 0444,
				   qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.hw_plat_subtype);
		fallthrough;
	case SOCINFO_VERSION(0, 5):
		qcom_socinfo->info.accessory_chip =
			__le32_to_cpu(info->accessory_chip);

		debugfs_create_u32("accessory_chip", 0444,
				   qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.accessory_chip);
		fallthrough;
	case SOCINFO_VERSION(0, 4):
		qcom_socinfo->info.plat_ver = __le32_to_cpu(info->plat_ver);

		debugfs_create_u32("platform_version", 0444,
				   qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.plat_ver);
		fallthrough;
	case SOCINFO_VERSION(0, 3):
		qcom_socinfo->info.hw_plat = __le32_to_cpu(info->hw_plat);

		debugfs_create_u32("hardware_platform", 0444,
				   qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.hw_plat);
		fallthrough;
	case SOCINFO_VERSION(0, 2):
		qcom_socinfo->info.raw_ver  = __le32_to_cpu(info->raw_ver);

		debugfs_create_u32("raw_version", 0444, qcom_socinfo->dbg_root,
				   &qcom_socinfo->info.raw_ver);
		fallthrough;
	case SOCINFO_VERSION(0, 1):
		DEBUGFS_ADD(info, build_id);
		break;
	}

	versions = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_IMAGE_VERSION_TABLE,
				 &size);

	for (i = 0; i < ARRAY_SIZE(socinfo_image_names); i++) {
		if (!socinfo_image_names[i])
			continue;

		dentry = debugfs_create_dir(socinfo_image_names[i],
					    qcom_socinfo->dbg_root);
		debugfs_create_file("name", 0444, dentry, &versions[i],
				    &qcom_image_name_ops);
		debugfs_create_file("variant", 0444, dentry, &versions[i],
				    &qcom_image_variant_ops);
		debugfs_create_file("oem", 0444, dentry, &versions[i],
				    &qcom_image_oem_ops);
	}
}

static void socinfo_debugfs_exit(struct qcom_socinfo *qcom_socinfo)
{
	debugfs_remove_recursive(qcom_socinfo->dbg_root);
}
#else
static void socinfo_debugfs_init(struct qcom_socinfo *qcom_socinfo,
				 struct socinfo *info, size_t info_size)
{
}
static void socinfo_debugfs_exit(struct qcom_socinfo *qcom_socinfo) {  }
#endif /* CONFIG_DEBUG_FS */

static int qcom_socinfo_probe(struct platform_device *pdev)
{
	struct qcom_socinfo *qs;
	struct socinfo *info;
	size_t item_size;

	info = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_HW_SW_BUILD_ID,
			      &item_size);
	if (IS_ERR(info)) {
		dev_err(&pdev->dev, "Couldn't find socinfo\n");
		return PTR_ERR(info);
	}

	qs = devm_kzalloc(&pdev->dev, sizeof(*qs), GFP_KERNEL);
	if (!qs)
		return -ENOMEM;

	qs->attr.family = "Snapdragon";
	qs->attr.machine = socinfo_machine(&pdev->dev,
					   le32_to_cpu(info->id));
	qs->attr.soc_id = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%u",
					 le32_to_cpu(info->id));
	qs->attr.revision = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%u.%u",
					   SOCINFO_MAJOR(le32_to_cpu(info->ver)),
					   SOCINFO_MINOR(le32_to_cpu(info->ver)));
	if (offsetof(struct socinfo, serial_num) <= item_size)
		qs->attr.serial_number = devm_kasprintf(&pdev->dev, GFP_KERNEL,
							"%u",
							le32_to_cpu(info->serial_num));

	qs->soc_dev = soc_device_register(&qs->attr);
	if (IS_ERR(qs->soc_dev))
		return PTR_ERR(qs->soc_dev);

	socinfo_debugfs_init(qs, info, item_size);

	/* Feed the soc specific unique data into entropy pool */
	add_device_randomness(info, item_size);

	platform_set_drvdata(pdev, qs);

	return 0;
}

static int qcom_socinfo_remove(struct platform_device *pdev)
{
	struct qcom_socinfo *qs = platform_get_drvdata(pdev);

	soc_device_unregister(qs->soc_dev);

	socinfo_debugfs_exit(qs);

	return 0;
}

static struct platform_driver qcom_socinfo_driver = {
	.probe = qcom_socinfo_probe,
	.remove = qcom_socinfo_remove,
	.driver  = {
		.name = "qcom-socinfo",
	},
};

module_platform_driver(qcom_socinfo_driver);

MODULE_DESCRIPTION("Qualcomm SoCinfo driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:qcom-socinfo");
