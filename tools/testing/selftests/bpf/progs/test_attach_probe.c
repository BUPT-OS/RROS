// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017 Facebook

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "bpf_misc.h"

int kprobe2_res = 0;
int kretprobe2_res = 0;
int uprobe_byname_res = 0;
int uretprobe_byname_res = 0;
int uprobe_byname2_res = 0;
int uretprobe_byname2_res = 0;
int uprobe_byname3_sleepable_res = 0;
int uprobe_byname3_res = 0;
int uretprobe_byname3_sleepable_res = 0;
int uretprobe_byname3_res = 0;
void *user_ptr = 0;

SEC("ksyscall/nanosleep")
int BPF_KSYSCALL(handle_kprobe_auto, struct __kernel_timespec *req, struct __kernel_timespec *rem)
{
	kprobe2_res = 11;
	return 0;
}

SEC("kretsyscall/nanosleep")
int BPF_KRETPROBE(handle_kretprobe_auto, int ret)
{
	kretprobe2_res = 22;
	return ret;
}

SEC("uprobe")
int handle_uprobe_ref_ctr(struct pt_regs *ctx)
{
	return 0;
}

SEC("uretprobe")
int handle_uretprobe_ref_ctr(struct pt_regs *ctx)
{
	return 0;
}

SEC("uprobe")
int handle_uprobe_byname(struct pt_regs *ctx)
{
	uprobe_byname_res = 5;
	return 0;
}

/* use auto-attach format for section definition. */
SEC("uretprobe//proc/self/exe:trigger_func2")
int handle_uretprobe_byname(struct pt_regs *ctx)
{
	uretprobe_byname_res = 6;
	return 0;
}

SEC("uprobe")
int BPF_UPROBE(handle_uprobe_byname2, const char *pathname, const char *mode)
{
	char mode_buf[2] = {};

	/* verify fopen mode */
	bpf_probe_read_user(mode_buf, sizeof(mode_buf), mode);
	if (mode_buf[0] == 'r' && mode_buf[1] == 0)
		uprobe_byname2_res = 7;
	return 0;
}

SEC("uretprobe")
int BPF_URETPROBE(handle_uretprobe_byname2, void *ret)
{
	uretprobe_byname2_res = 8;
	return 0;
}

static __always_inline bool verify_sleepable_user_copy(void)
{
	char data[9];

	bpf_copy_from_user(data, sizeof(data), user_ptr);
	return bpf_strncmp(data, sizeof(data), "test_data") == 0;
}

SEC("uprobe.s//proc/self/exe:trigger_func3")
int handle_uprobe_byname3_sleepable(struct pt_regs *ctx)
{
	if (verify_sleepable_user_copy())
		uprobe_byname3_sleepable_res = 9;
	return 0;
}

/**
 * same target as the uprobe.s above to force sleepable and non-sleepable
 * programs in the same bpf_prog_array
 */
SEC("uprobe//proc/self/exe:trigger_func3")
int handle_uprobe_byname3(struct pt_regs *ctx)
{
	uprobe_byname3_res = 10;
	return 0;
}

SEC("uretprobe.s//proc/self/exe:trigger_func3")
int handle_uretprobe_byname3_sleepable(struct pt_regs *ctx)
{
	if (verify_sleepable_user_copy())
		uretprobe_byname3_sleepable_res = 11;
	return 0;
}

SEC("uretprobe//proc/self/exe:trigger_func3")
int handle_uretprobe_byname3(struct pt_regs *ctx)
{
	uretprobe_byname3_res = 12;
	return 0;
}


char _license[] SEC("license") = "GPL";
