/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright(c) 2016-20 Intel Corporation.
 */

#ifndef DEFINES_H
#define DEFINES_H

#include <stdint.h>

#define PAGE_SIZE 4096
#define PAGE_MASK (~(PAGE_SIZE - 1))

#define __aligned(x) __attribute__((__aligned__(x)))
#define __packed __attribute__((packed))

#include "../../../../arch/x86/include/asm/sgx.h"
#include "../../../../arch/x86/include/asm/enclu.h"
#include "../../../../arch/x86/include/uapi/asm/sgx.h"

enum encl_op_type {
	ENCL_OP_PUT_TO_BUFFER,
	ENCL_OP_GET_FROM_BUFFER,
	ENCL_OP_PUT_TO_ADDRESS,
	ENCL_OP_GET_FROM_ADDRESS,
	ENCL_OP_NOP,
	ENCL_OP_EACCEPT,
	ENCL_OP_EMODPE,
	ENCL_OP_INIT_TCS_PAGE,
	ENCL_OP_MAX,
};

struct encl_op_header {
	uint64_t type;
};

struct encl_op_put_to_buf {
	struct encl_op_header header;
	uint64_t value;
};

struct encl_op_get_from_buf {
	struct encl_op_header header;
	uint64_t value;
};

struct encl_op_put_to_addr {
	struct encl_op_header header;
	uint64_t value;
	uint64_t addr;
};

struct encl_op_get_from_addr {
	struct encl_op_header header;
	uint64_t value;
	uint64_t addr;
};

struct encl_op_eaccept {
	struct encl_op_header header;
	uint64_t epc_addr;
	uint64_t flags;
	uint64_t ret;
};

struct encl_op_emodpe {
	struct encl_op_header header;
	uint64_t epc_addr;
	uint64_t flags;
};

struct encl_op_init_tcs_page {
	struct encl_op_header header;
	uint64_t tcs_page;
	uint64_t ssa;
	uint64_t entry;
};

#endif /* DEFINES_H */
