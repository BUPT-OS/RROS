// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright IBM Corp. 2022
 *  Author(s): Steffen Eiden <seiden@linux.ibm.com>
 *
 *  This file provides a Linux misc device to give userspace access to some
 *  Ultravisor (UV) functions. The device only accepts IOCTLs and will only
 *  be present if the Ultravisor facility (158) is present.
 *
 *  When userspace sends a valid IOCTL uvdevice will copy the input data to
 *  kernel space, do some basic validity checks to avoid kernel/system
 *  corruption. Any other check that the Ultravisor does will not be done by
 *  the uvdevice to keep changes minimal when adding new functionalities
 *  to existing UV-calls.
 *  After the checks uvdevice builds a corresponding
 *  Ultravisor Call Control Block, and sends the request to the Ultravisor.
 *  Then, it copies the response, including the return codes, back to userspace.
 *  It is the responsibility of the userspace to check for any error issued
 *  by UV and to interpret the UV response. The uvdevice acts as a communication
 *  channel for userspace to the Ultravisor.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/cpufeature.h>

#include <asm/uvdevice.h>
#include <asm/uv.h>

#define BIT_UVIO_INTERNAL U32_MAX
/* Mapping from IOCTL-nr to UVC-bit */
static const u32 ioctl_nr_to_uvc_bit[] __initconst = {
	[UVIO_IOCTL_UVDEV_INFO_NR] = BIT_UVIO_INTERNAL,
	[UVIO_IOCTL_ATT_NR] = BIT_UVC_CMD_RETR_ATTEST,
	[UVIO_IOCTL_ADD_SECRET_NR] = BIT_UVC_CMD_ADD_SECRET,
	[UVIO_IOCTL_LIST_SECRETS_NR] = BIT_UVC_CMD_LIST_SECRETS,
	[UVIO_IOCTL_LOCK_SECRETS_NR] = BIT_UVC_CMD_LOCK_SECRETS,
};

static_assert(ARRAY_SIZE(ioctl_nr_to_uvc_bit) == UVIO_IOCTL_NUM_IOCTLS);

static struct uvio_uvdev_info uvdev_info = {
	.supp_uvio_cmds = GENMASK_ULL(UVIO_IOCTL_NUM_IOCTLS - 1, 0),
};

static void __init set_supp_uv_cmds(unsigned long *supp_uv_cmds)
{
	int i;

	for (i = 0; i < UVIO_IOCTL_NUM_IOCTLS; i++) {
		if (ioctl_nr_to_uvc_bit[i] == BIT_UVIO_INTERNAL)
			continue;
		if (!test_bit_inv(ioctl_nr_to_uvc_bit[i], uv_info.inst_calls_list))
			continue;
		__set_bit(i, supp_uv_cmds);
	}
}

/**
 * uvio_uvdev_info() - get information about the uvdevice
 *
 * @uv_ioctl: ioctl control block
 *
 * Lists all IOCTLs that are supported by this uvdevice
 */
static int uvio_uvdev_info(struct uvio_ioctl_cb *uv_ioctl)
{
	void __user *user_buf_arg = (void __user *)uv_ioctl->argument_addr;

	if (uv_ioctl->argument_len < sizeof(uvdev_info))
		return -EINVAL;
	if (copy_to_user(user_buf_arg, &uvdev_info, sizeof(uvdev_info)))
		return -EFAULT;

	uv_ioctl->uv_rc = UVC_RC_EXECUTED;
	return 0;
}

static int uvio_build_uvcb_attest(struct uv_cb_attest *uvcb_attest, u8 *arcb,
				  u8 *meas, u8 *add_data, struct uvio_attest *uvio_attest)
{
	void __user *user_buf_arcb = (void __user *)uvio_attest->arcb_addr;

	if (copy_from_user(arcb, user_buf_arcb, uvio_attest->arcb_len))
		return -EFAULT;

	uvcb_attest->header.len = sizeof(*uvcb_attest);
	uvcb_attest->header.cmd = UVC_CMD_RETR_ATTEST;
	uvcb_attest->arcb_addr = (u64)arcb;
	uvcb_attest->cont_token = 0;
	uvcb_attest->user_data_len = uvio_attest->user_data_len;
	memcpy(uvcb_attest->user_data, uvio_attest->user_data, sizeof(uvcb_attest->user_data));
	uvcb_attest->meas_len = uvio_attest->meas_len;
	uvcb_attest->meas_addr = (u64)meas;
	uvcb_attest->add_data_len = uvio_attest->add_data_len;
	uvcb_attest->add_data_addr = (u64)add_data;

	return 0;
}

static int uvio_copy_attest_result_to_user(struct uv_cb_attest *uvcb_attest,
					   struct uvio_ioctl_cb *uv_ioctl,
					   u8 *measurement, u8 *add_data,
					   struct uvio_attest *uvio_attest)
{
	struct uvio_attest __user *user_uvio_attest = (void __user *)uv_ioctl->argument_addr;
	void __user *user_buf_add = (void __user *)uvio_attest->add_data_addr;
	void __user *user_buf_meas = (void __user *)uvio_attest->meas_addr;
	void __user *user_buf_uid = &user_uvio_attest->config_uid;

	if (copy_to_user(user_buf_meas, measurement, uvio_attest->meas_len))
		return -EFAULT;
	if (add_data && copy_to_user(user_buf_add, add_data, uvio_attest->add_data_len))
		return -EFAULT;
	if (copy_to_user(user_buf_uid, uvcb_attest->config_uid, sizeof(uvcb_attest->config_uid)))
		return -EFAULT;
	return 0;
}

static int get_uvio_attest(struct uvio_ioctl_cb *uv_ioctl, struct uvio_attest *uvio_attest)
{
	u8 __user *user_arg_buf = (u8 __user *)uv_ioctl->argument_addr;

	if (copy_from_user(uvio_attest, user_arg_buf, sizeof(*uvio_attest)))
		return -EFAULT;

	if (uvio_attest->arcb_len > UVIO_ATT_ARCB_MAX_LEN)
		return -EINVAL;
	if (uvio_attest->arcb_len == 0)
		return -EINVAL;
	if (uvio_attest->meas_len > UVIO_ATT_MEASUREMENT_MAX_LEN)
		return -EINVAL;
	if (uvio_attest->meas_len == 0)
		return -EINVAL;
	if (uvio_attest->add_data_len > UVIO_ATT_ADDITIONAL_MAX_LEN)
		return -EINVAL;
	if (uvio_attest->reserved136)
		return -EINVAL;
	return 0;
}

/**
 * uvio_attestation() - Perform a Retrieve Attestation Measurement UVC.
 *
 * @uv_ioctl: ioctl control block
 *
 * uvio_attestation() does a Retrieve Attestation Measurement Ultravisor Call.
 * It verifies that the given userspace addresses are valid and request sizes
 * are sane. Every other check is made by the Ultravisor (UV) and won't result
 * in a negative return value. It copies the input to kernelspace, builds the
 * request, sends the UV-call, and copies the result to userspace.
 *
 * The Attestation Request has two input and two outputs.
 * ARCB and User Data are inputs for the UV generated by userspace.
 * Measurement and Additional Data are outputs for userspace generated by UV.
 *
 * The Attestation Request Control Block (ARCB) is a cryptographically verified
 * and secured request to UV and User Data is some plaintext data which is
 * going to be included in the Attestation Measurement calculation.
 *
 * Measurement is a cryptographic measurement of the callers properties,
 * optional data configured by the ARCB and the user data. If specified by the
 * ARCB, UV will add some Additional Data to the measurement calculation.
 * This Additional Data is then returned as well.
 *
 * If the Retrieve Attestation Measurement UV facility is not present,
 * UV will return invalid command rc. This won't be fenced in the driver
 * and does not result in a negative return value.
 *
 * Context: might sleep
 *
 * Return: 0 on success or a negative error code on error.
 */
static int uvio_attestation(struct uvio_ioctl_cb *uv_ioctl)
{
	struct uv_cb_attest *uvcb_attest = NULL;
	struct uvio_attest *uvio_attest = NULL;
	u8 *measurement = NULL;
	u8 *add_data = NULL;
	u8 *arcb = NULL;
	int ret;

	ret = -EINVAL;
	if (uv_ioctl->argument_len != sizeof(*uvio_attest))
		goto out;

	ret = -ENOMEM;
	uvio_attest = kzalloc(sizeof(*uvio_attest), GFP_KERNEL);
	if (!uvio_attest)
		goto out;

	ret = get_uvio_attest(uv_ioctl, uvio_attest);
	if (ret)
		goto out;

	ret = -ENOMEM;
	arcb = kvzalloc(uvio_attest->arcb_len, GFP_KERNEL);
	measurement = kvzalloc(uvio_attest->meas_len, GFP_KERNEL);
	if (!arcb || !measurement)
		goto out;

	if (uvio_attest->add_data_len) {
		add_data = kvzalloc(uvio_attest->add_data_len, GFP_KERNEL);
		if (!add_data)
			goto out;
	}

	uvcb_attest = kzalloc(sizeof(*uvcb_attest), GFP_KERNEL);
	if (!uvcb_attest)
		goto out;

	ret = uvio_build_uvcb_attest(uvcb_attest, arcb,  measurement, add_data, uvio_attest);
	if (ret)
		goto out;

	uv_call_sched(0, (u64)uvcb_attest);

	uv_ioctl->uv_rc = uvcb_attest->header.rc;
	uv_ioctl->uv_rrc = uvcb_attest->header.rrc;

	ret = uvio_copy_attest_result_to_user(uvcb_attest, uv_ioctl, measurement, add_data,
					      uvio_attest);
out:
	kvfree(arcb);
	kvfree(measurement);
	kvfree(add_data);
	kfree(uvio_attest);
	kfree(uvcb_attest);
	return ret;
}

/** uvio_add_secret() - perform an Add Secret UVC
 *
 * @uv_ioctl: ioctl control block
 *
 * uvio_add_secret() performs the Add Secret Ultravisor Call.
 *
 * The given userspace argument address and size are verified to be
 * valid but every other check is made by the Ultravisor
 * (UV). Therefore UV errors won't result in a negative return
 * value. The request is then copied to kernelspace, the UV-call is
 * performed and the results are copied back to userspace.
 *
 * The argument has to point to an Add Secret Request Control Block
 * which is an encrypted and cryptographically verified request that
 * inserts a protected guest's secrets into the Ultravisor for later
 * use.
 *
 * If the Add Secret UV facility is not present, UV will return
 * invalid command rc. This won't be fenced in the driver and does not
 * result in a negative return value.
 *
 * Context: might sleep
 *
 * Return: 0 on success or a negative error code on error.
 */
static int uvio_add_secret(struct uvio_ioctl_cb *uv_ioctl)
{
	void __user *user_buf_arg = (void __user *)uv_ioctl->argument_addr;
	struct uv_cb_guest_addr uvcb = {
		.header.len = sizeof(uvcb),
		.header.cmd = UVC_CMD_ADD_SECRET,
	};
	void *asrcb = NULL;
	int ret;

	if (uv_ioctl->argument_len > UVIO_ADD_SECRET_MAX_LEN)
		return -EINVAL;
	if (uv_ioctl->argument_len == 0)
		return -EINVAL;

	asrcb = kvzalloc(uv_ioctl->argument_len, GFP_KERNEL);
	if (!asrcb)
		return -ENOMEM;

	ret = -EFAULT;
	if (copy_from_user(asrcb, user_buf_arg, uv_ioctl->argument_len))
		goto out;

	ret = 0;
	uvcb.addr = (u64)asrcb;
	uv_call_sched(0, (u64)&uvcb);
	uv_ioctl->uv_rc = uvcb.header.rc;
	uv_ioctl->uv_rrc = uvcb.header.rrc;

out:
	kvfree(asrcb);
	return ret;
}

/** uvio_list_secrets() - perform a List Secret UVC
 * @uv_ioctl: ioctl control block
 *
 * uvio_list_secrets() performs the List Secret Ultravisor Call. It verifies
 * that the given userspace argument address is valid and its size is sane.
 * Every other check is made by the Ultravisor (UV) and won't result in a
 * negative return value. It builds the request, performs the UV-call, and
 * copies the result to userspace.
 *
 * The argument specifies the location for the result of the UV-Call.
 *
 * If the List Secrets UV facility is not present, UV will return invalid
 * command rc. This won't be fenced in the driver and does not result in a
 * negative return value.
 *
 * Context: might sleep
 *
 * Return: 0 on success or a negative error code on error.
 */
static int uvio_list_secrets(struct uvio_ioctl_cb *uv_ioctl)
{
	void __user *user_buf_arg = (void __user *)uv_ioctl->argument_addr;
	struct uv_cb_guest_addr uvcb = {
		.header.len = sizeof(uvcb),
		.header.cmd = UVC_CMD_LIST_SECRETS,
	};
	void *secrets = NULL;
	int ret = 0;

	if (uv_ioctl->argument_len != UVIO_LIST_SECRETS_LEN)
		return -EINVAL;

	secrets = kvzalloc(UVIO_LIST_SECRETS_LEN, GFP_KERNEL);
	if (!secrets)
		return -ENOMEM;

	uvcb.addr = (u64)secrets;
	uv_call_sched(0, (u64)&uvcb);
	uv_ioctl->uv_rc = uvcb.header.rc;
	uv_ioctl->uv_rrc = uvcb.header.rrc;

	if (copy_to_user(user_buf_arg, secrets, UVIO_LIST_SECRETS_LEN))
		ret = -EFAULT;

	kvfree(secrets);
	return ret;
}

/** uvio_lock_secrets() - perform a Lock Secret Store UVC
 * @uv_ioctl: ioctl control block
 *
 * uvio_lock_secrets() performs the Lock Secret Store Ultravisor Call. It
 * performs the UV-call and copies the return codes to the ioctl control block.
 * After this call was dispatched successfully every following Add Secret UVC
 * and Lock Secrets UVC will fail with return code 0x102.
 *
 * The argument address and size must be 0.
 *
 * If the Lock Secrets UV facility is not present, UV will return invalid
 * command rc. This won't be fenced in the driver and does not result in a
 * negative return value.
 *
 * Context: might sleep
 *
 * Return: 0 on success or a negative error code on error.
 */
static int uvio_lock_secrets(struct uvio_ioctl_cb *ioctl)
{
	struct uv_cb_nodata uvcb = {
		.header.len = sizeof(uvcb),
		.header.cmd = UVC_CMD_LOCK_SECRETS,
	};

	if (ioctl->argument_addr || ioctl->argument_len)
		return -EINVAL;

	uv_call(0, (u64)&uvcb);
	ioctl->uv_rc = uvcb.header.rc;
	ioctl->uv_rrc = uvcb.header.rrc;

	return 0;
}

static int uvio_copy_and_check_ioctl(struct uvio_ioctl_cb *ioctl, void __user *argp,
				     unsigned long cmd)
{
	u8 nr = _IOC_NR(cmd);

	if (_IOC_DIR(cmd) != (_IOC_READ | _IOC_WRITE))
		return -ENOIOCTLCMD;
	if (_IOC_TYPE(cmd) != UVIO_TYPE_UVC)
		return -ENOIOCTLCMD;
	if (nr >= UVIO_IOCTL_NUM_IOCTLS)
		return -ENOIOCTLCMD;
	if (_IOC_SIZE(cmd) != sizeof(*ioctl))
		return -ENOIOCTLCMD;
	if (copy_from_user(ioctl, argp, sizeof(*ioctl)))
		return -EFAULT;
	if (ioctl->flags != 0)
		return -EINVAL;
	if (memchr_inv(ioctl->reserved14, 0, sizeof(ioctl->reserved14)))
		return -EINVAL;

	return nr;
}

/*
 * IOCTL entry point for the Ultravisor device.
 */
static long uvio_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	struct uvio_ioctl_cb uv_ioctl = { };
	long ret;
	int nr;

	nr = uvio_copy_and_check_ioctl(&uv_ioctl, argp, cmd);
	if (nr < 0)
		return nr;

	switch (nr) {
	case UVIO_IOCTL_UVDEV_INFO_NR:
		ret = uvio_uvdev_info(&uv_ioctl);
		break;
	case UVIO_IOCTL_ATT_NR:
		ret = uvio_attestation(&uv_ioctl);
		break;
	case UVIO_IOCTL_ADD_SECRET_NR:
		ret = uvio_add_secret(&uv_ioctl);
		break;
	case UVIO_IOCTL_LIST_SECRETS_NR:
		ret = uvio_list_secrets(&uv_ioctl);
		break;
	case UVIO_IOCTL_LOCK_SECRETS_NR:
		ret = uvio_lock_secrets(&uv_ioctl);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}
	if (ret)
		return ret;

	if (copy_to_user(argp, &uv_ioctl, sizeof(uv_ioctl)))
		ret = -EFAULT;

	return ret;
}

static const struct file_operations uvio_dev_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = uvio_ioctl,
	.llseek = no_llseek,
};

static struct miscdevice uvio_dev_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = UVIO_DEVICE_NAME,
	.fops = &uvio_dev_fops,
};

static void __exit uvio_dev_exit(void)
{
	misc_deregister(&uvio_dev_miscdev);
}

static int __init uvio_dev_init(void)
{
	set_supp_uv_cmds((unsigned long *)&uvdev_info.supp_uv_cmds);
	return misc_register(&uvio_dev_miscdev);
}

module_cpu_feature_match(S390_CPU_FEATURE_UV, uvio_dev_init);
module_exit(uvio_dev_exit);

MODULE_AUTHOR("IBM Corporation");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ultravisor UAPI driver");
