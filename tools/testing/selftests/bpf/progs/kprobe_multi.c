// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <stdbool.h>

char _license[] SEC("license") = "GPL";

extern const void bpf_fentry_test1 __ksym;
extern const void bpf_fentry_test2 __ksym;
extern const void bpf_fentry_test3 __ksym;
extern const void bpf_fentry_test4 __ksym;
extern const void bpf_fentry_test5 __ksym;
extern const void bpf_fentry_test6 __ksym;
extern const void bpf_fentry_test7 __ksym;
extern const void bpf_fentry_test8 __ksym;

int pid = 0;
bool test_cookie = false;

__u64 kprobe_test1_result = 0;
__u64 kprobe_test2_result = 0;
__u64 kprobe_test3_result = 0;
__u64 kprobe_test4_result = 0;
__u64 kprobe_test5_result = 0;
__u64 kprobe_test6_result = 0;
__u64 kprobe_test7_result = 0;
__u64 kprobe_test8_result = 0;

__u64 kretprobe_test1_result = 0;
__u64 kretprobe_test2_result = 0;
__u64 kretprobe_test3_result = 0;
__u64 kretprobe_test4_result = 0;
__u64 kretprobe_test5_result = 0;
__u64 kretprobe_test6_result = 0;
__u64 kretprobe_test7_result = 0;
__u64 kretprobe_test8_result = 0;

static void kprobe_multi_check(void *ctx, bool is_return)
{
	if (bpf_get_current_pid_tgid() >> 32 != pid)
		return;

	__u64 cookie = test_cookie ? bpf_get_attach_cookie(ctx) : 0;
	__u64 addr = bpf_get_func_ip(ctx);

#define SET(__var, __addr, __cookie) ({			\
	if (((const void *) addr == __addr) &&		\
	     (!test_cookie || (cookie == __cookie)))	\
		__var = 1;				\
})

	if (is_return) {
		SET(kretprobe_test1_result, &bpf_fentry_test1, 8);
		SET(kretprobe_test2_result, &bpf_fentry_test2, 2);
		SET(kretprobe_test3_result, &bpf_fentry_test3, 7);
		SET(kretprobe_test4_result, &bpf_fentry_test4, 6);
		SET(kretprobe_test5_result, &bpf_fentry_test5, 5);
		SET(kretprobe_test6_result, &bpf_fentry_test6, 4);
		SET(kretprobe_test7_result, &bpf_fentry_test7, 3);
		SET(kretprobe_test8_result, &bpf_fentry_test8, 1);
	} else {
		SET(kprobe_test1_result, &bpf_fentry_test1, 1);
		SET(kprobe_test2_result, &bpf_fentry_test2, 7);
		SET(kprobe_test3_result, &bpf_fentry_test3, 2);
		SET(kprobe_test4_result, &bpf_fentry_test4, 3);
		SET(kprobe_test5_result, &bpf_fentry_test5, 4);
		SET(kprobe_test6_result, &bpf_fentry_test6, 5);
		SET(kprobe_test7_result, &bpf_fentry_test7, 6);
		SET(kprobe_test8_result, &bpf_fentry_test8, 8);
	}

#undef SET
}

/*
 * No tests in here, just to trigger 'bpf_fentry_test*'
 * through tracing test_run
 */
SEC("fentry/bpf_modify_return_test")
int BPF_PROG(trigger)
{
	return 0;
}

SEC("kprobe.multi/bpf_fentry_tes??")
int test_kprobe(struct pt_regs *ctx)
{
	kprobe_multi_check(ctx, false);
	return 0;
}

SEC("kretprobe.multi/bpf_fentry_test*")
int test_kretprobe(struct pt_regs *ctx)
{
	kprobe_multi_check(ctx, true);
	return 0;
}

SEC("kprobe.multi")
int test_kprobe_manual(struct pt_regs *ctx)
{
	kprobe_multi_check(ctx, false);
	return 0;
}

SEC("kretprobe.multi")
int test_kretprobe_manual(struct pt_regs *ctx)
{
	kprobe_multi_check(ctx, true);
	return 0;
}

extern const void bpf_testmod_fentry_test1 __ksym;
extern const void bpf_testmod_fentry_test2 __ksym;
extern const void bpf_testmod_fentry_test3 __ksym;

__u64 kprobe_testmod_test1_result = 0;
__u64 kprobe_testmod_test2_result = 0;
__u64 kprobe_testmod_test3_result = 0;

__u64 kretprobe_testmod_test1_result = 0;
__u64 kretprobe_testmod_test2_result = 0;
__u64 kretprobe_testmod_test3_result = 0;

static void kprobe_multi_testmod_check(void *ctx, bool is_return)
{
	if (bpf_get_current_pid_tgid() >> 32 != pid)
		return;

	__u64 addr = bpf_get_func_ip(ctx);

	if (is_return) {
		if ((const void *) addr == &bpf_testmod_fentry_test1)
			kretprobe_testmod_test1_result = 1;
		if ((const void *) addr == &bpf_testmod_fentry_test2)
			kretprobe_testmod_test2_result = 1;
		if ((const void *) addr == &bpf_testmod_fentry_test3)
			kretprobe_testmod_test3_result = 1;
	} else {
		if ((const void *) addr == &bpf_testmod_fentry_test1)
			kprobe_testmod_test1_result = 1;
		if ((const void *) addr == &bpf_testmod_fentry_test2)
			kprobe_testmod_test2_result = 1;
		if ((const void *) addr == &bpf_testmod_fentry_test3)
			kprobe_testmod_test3_result = 1;
	}
}

SEC("kprobe.multi")
int test_kprobe_testmod(struct pt_regs *ctx)
{
	kprobe_multi_testmod_check(ctx, false);
	return 0;
}

SEC("kretprobe.multi")
int test_kretprobe_testmod(struct pt_regs *ctx)
{
	kprobe_multi_testmod_check(ctx, true);
	return 0;
}
