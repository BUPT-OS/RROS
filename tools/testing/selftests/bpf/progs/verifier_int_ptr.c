// SPDX-License-Identifier: GPL-2.0
/* Converted from tools/testing/selftests/bpf/verifier/int_ptr.c */

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include "bpf_misc.h"

SEC("cgroup/sysctl")
__description("ARG_PTR_TO_LONG uninitialized")
__failure __msg("invalid indirect read from stack R4 off -16+0 size 8")
__naked void arg_ptr_to_long_uninitialized(void)
{
	asm volatile ("					\
	/* bpf_strtoul arg1 (buf) */			\
	r7 = r10;					\
	r7 += -8;					\
	r0 = 0x00303036;				\
	*(u64*)(r7 + 0) = r0;				\
	r1 = r7;					\
	/* bpf_strtoul arg2 (buf_len) */		\
	r2 = 4;						\
	/* bpf_strtoul arg3 (flags) */			\
	r3 = 0;						\
	/* bpf_strtoul arg4 (res) */			\
	r7 += -8;					\
	r4 = r7;					\
	/* bpf_strtoul() */				\
	call %[bpf_strtoul];				\
	r0 = 1;						\
	exit;						\
"	:
	: __imm(bpf_strtoul)
	: __clobber_all);
}

SEC("socket")
__description("ARG_PTR_TO_LONG half-uninitialized")
/* in privileged mode reads from uninitialized stack locations are permitted */
__success __failure_unpriv
__msg_unpriv("invalid indirect read from stack R4 off -16+4 size 8")
__retval(0)
__naked void ptr_to_long_half_uninitialized(void)
{
	asm volatile ("					\
	/* bpf_strtoul arg1 (buf) */			\
	r7 = r10;					\
	r7 += -8;					\
	r0 = 0x00303036;				\
	*(u64*)(r7 + 0) = r0;				\
	r1 = r7;					\
	/* bpf_strtoul arg2 (buf_len) */		\
	r2 = 4;						\
	/* bpf_strtoul arg3 (flags) */			\
	r3 = 0;						\
	/* bpf_strtoul arg4 (res) */			\
	r7 += -8;					\
	*(u32*)(r7 + 0) = r0;				\
	r4 = r7;					\
	/* bpf_strtoul() */				\
	call %[bpf_strtoul];				\
	r0 = 0;						\
	exit;						\
"	:
	: __imm(bpf_strtoul)
	: __clobber_all);
}

SEC("cgroup/sysctl")
__description("ARG_PTR_TO_LONG misaligned")
__failure __msg("misaligned stack access off (0x0; 0x0)+-20+0 size 8")
__naked void arg_ptr_to_long_misaligned(void)
{
	asm volatile ("					\
	/* bpf_strtoul arg1 (buf) */			\
	r7 = r10;					\
	r7 += -8;					\
	r0 = 0x00303036;				\
	*(u64*)(r7 + 0) = r0;				\
	r1 = r7;					\
	/* bpf_strtoul arg2 (buf_len) */		\
	r2 = 4;						\
	/* bpf_strtoul arg3 (flags) */			\
	r3 = 0;						\
	/* bpf_strtoul arg4 (res) */			\
	r7 += -12;					\
	r0 = 0;						\
	*(u32*)(r7 + 0) = r0;				\
	*(u64*)(r7 + 4) = r0;				\
	r4 = r7;					\
	/* bpf_strtoul() */				\
	call %[bpf_strtoul];				\
	r0 = 1;						\
	exit;						\
"	:
	: __imm(bpf_strtoul)
	: __clobber_all);
}

SEC("cgroup/sysctl")
__description("ARG_PTR_TO_LONG size < sizeof(long)")
__failure __msg("invalid indirect access to stack R4 off=-4 size=8")
__naked void to_long_size_sizeof_long(void)
{
	asm volatile ("					\
	/* bpf_strtoul arg1 (buf) */			\
	r7 = r10;					\
	r7 += -16;					\
	r0 = 0x00303036;				\
	*(u64*)(r7 + 0) = r0;				\
	r1 = r7;					\
	/* bpf_strtoul arg2 (buf_len) */		\
	r2 = 4;						\
	/* bpf_strtoul arg3 (flags) */			\
	r3 = 0;						\
	/* bpf_strtoul arg4 (res) */			\
	r7 += 12;					\
	*(u32*)(r7 + 0) = r0;				\
	r4 = r7;					\
	/* bpf_strtoul() */				\
	call %[bpf_strtoul];				\
	r0 = 1;						\
	exit;						\
"	:
	: __imm(bpf_strtoul)
	: __clobber_all);
}

SEC("cgroup/sysctl")
__description("ARG_PTR_TO_LONG initialized")
__success
__naked void arg_ptr_to_long_initialized(void)
{
	asm volatile ("					\
	/* bpf_strtoul arg1 (buf) */			\
	r7 = r10;					\
	r7 += -8;					\
	r0 = 0x00303036;				\
	*(u64*)(r7 + 0) = r0;				\
	r1 = r7;					\
	/* bpf_strtoul arg2 (buf_len) */		\
	r2 = 4;						\
	/* bpf_strtoul arg3 (flags) */			\
	r3 = 0;						\
	/* bpf_strtoul arg4 (res) */			\
	r7 += -8;					\
	*(u64*)(r7 + 0) = r0;				\
	r4 = r7;					\
	/* bpf_strtoul() */				\
	call %[bpf_strtoul];				\
	r0 = 1;						\
	exit;						\
"	:
	: __imm(bpf_strtoul)
	: __clobber_all);
}

char _license[] SEC("license") = "GPL";
