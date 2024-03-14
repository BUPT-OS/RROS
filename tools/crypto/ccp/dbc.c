// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD Secure Processor Dynamic Boost Control sample library
 *
 * Copyright (C) 2023 Advanced Micro Devices, Inc.
 *
 * Author: Mario Limonciello <mario.limonciello@amd.com>
 */

#include <assert.h>
#include <string.h>
#include <sys/ioctl.h>

/* if uapi header isn't installed, this might not yet exist */
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include <linux/psp-dbc.h>

int get_nonce(int fd, void *nonce_out, void *signature)
{
	struct dbc_user_nonce tmp = {
		.auth_needed = !!signature,
	};
	int ret;

	assert(nonce_out);

	if (signature)
		memcpy(tmp.signature, signature, sizeof(tmp.signature));

	ret = ioctl(fd, DBCIOCNONCE, &tmp);
	if (ret)
		return ret;
	memcpy(nonce_out, tmp.nonce, sizeof(tmp.nonce));

	return 0;
}

int set_uid(int fd, __u8 *uid, __u8 *signature)
{
	struct dbc_user_setuid tmp;

	assert(uid);
	assert(signature);

	memcpy(tmp.uid, uid, sizeof(tmp.uid));
	memcpy(tmp.signature, signature, sizeof(tmp.signature));

	return ioctl(fd, DBCIOCUID, &tmp);
}

int process_param(int fd, int msg_index, __u8 *signature, int *data)
{
	struct dbc_user_param tmp = {
		.msg_index = msg_index,
		.param = *data,
	};
	int ret;

	assert(signature);
	assert(data);

	memcpy(tmp.signature, signature, sizeof(tmp.signature));

	ret = ioctl(fd, DBCIOCPARAM, &tmp);
	if (ret)
		return ret;

	*data = tmp.param;
	return 0;
}
