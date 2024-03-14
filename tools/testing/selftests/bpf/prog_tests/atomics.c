// SPDX-License-Identifier: GPL-2.0

#include <test_progs.h>

#include "atomics.lskel.h"

static void test_add(struct atomics_lskel *skel)
{
	int err, prog_fd;
	LIBBPF_OPTS(bpf_test_run_opts, topts);

	/* No need to attach it, just run it directly */
	prog_fd = skel->progs.add.prog_fd;
	err = bpf_prog_test_run_opts(prog_fd, &topts);
	if (!ASSERT_OK(err, "test_run_opts err"))
		return;
	if (!ASSERT_OK(topts.retval, "test_run_opts retval"))
		return;

	ASSERT_EQ(skel->data->add64_value, 3, "add64_value");
	ASSERT_EQ(skel->bss->add64_result, 1, "add64_result");

	ASSERT_EQ(skel->data->add32_value, 3, "add32_value");
	ASSERT_EQ(skel->bss->add32_result, 1, "add32_result");

	ASSERT_EQ(skel->bss->add_stack_value_copy, 3, "add_stack_value");
	ASSERT_EQ(skel->bss->add_stack_result, 1, "add_stack_result");

	ASSERT_EQ(skel->data->add_noreturn_value, 3, "add_noreturn_value");
}

static void test_sub(struct atomics_lskel *skel)
{
	int err, prog_fd;
	LIBBPF_OPTS(bpf_test_run_opts, topts);

	/* No need to attach it, just run it directly */
	prog_fd = skel->progs.sub.prog_fd;
	err = bpf_prog_test_run_opts(prog_fd, &topts);
	if (!ASSERT_OK(err, "test_run_opts err"))
		return;
	if (!ASSERT_OK(topts.retval, "test_run_opts retval"))
		return;

	ASSERT_EQ(skel->data->sub64_value, -1, "sub64_value");
	ASSERT_EQ(skel->bss->sub64_result, 1, "sub64_result");

	ASSERT_EQ(skel->data->sub32_value, -1, "sub32_value");
	ASSERT_EQ(skel->bss->sub32_result, 1, "sub32_result");

	ASSERT_EQ(skel->bss->sub_stack_value_copy, -1, "sub_stack_value");
	ASSERT_EQ(skel->bss->sub_stack_result, 1, "sub_stack_result");

	ASSERT_EQ(skel->data->sub_noreturn_value, -1, "sub_noreturn_value");
}

static void test_and(struct atomics_lskel *skel)
{
	int err, prog_fd;
	LIBBPF_OPTS(bpf_test_run_opts, topts);

	/* No need to attach it, just run it directly */
	prog_fd = skel->progs.and.prog_fd;
	err = bpf_prog_test_run_opts(prog_fd, &topts);
	if (!ASSERT_OK(err, "test_run_opts err"))
		return;
	if (!ASSERT_OK(topts.retval, "test_run_opts retval"))
		return;

	ASSERT_EQ(skel->data->and64_value, 0x010ull << 32, "and64_value");
	ASSERT_EQ(skel->bss->and64_result, 0x110ull << 32, "and64_result");

	ASSERT_EQ(skel->data->and32_value, 0x010, "and32_value");
	ASSERT_EQ(skel->bss->and32_result, 0x110, "and32_result");

	ASSERT_EQ(skel->data->and_noreturn_value, 0x010ull << 32, "and_noreturn_value");
}

static void test_or(struct atomics_lskel *skel)
{
	int err, prog_fd;
	LIBBPF_OPTS(bpf_test_run_opts, topts);

	/* No need to attach it, just run it directly */
	prog_fd = skel->progs.or.prog_fd;
	err = bpf_prog_test_run_opts(prog_fd, &topts);
	if (!ASSERT_OK(err, "test_run_opts err"))
		return;
	if (!ASSERT_OK(topts.retval, "test_run_opts retval"))
		return;

	ASSERT_EQ(skel->data->or64_value, 0x111ull << 32, "or64_value");
	ASSERT_EQ(skel->bss->or64_result, 0x110ull << 32, "or64_result");

	ASSERT_EQ(skel->data->or32_value, 0x111, "or32_value");
	ASSERT_EQ(skel->bss->or32_result, 0x110, "or32_result");

	ASSERT_EQ(skel->data->or_noreturn_value, 0x111ull << 32, "or_noreturn_value");
}

static void test_xor(struct atomics_lskel *skel)
{
	int err, prog_fd;
	LIBBPF_OPTS(bpf_test_run_opts, topts);

	/* No need to attach it, just run it directly */
	prog_fd = skel->progs.xor.prog_fd;
	err = bpf_prog_test_run_opts(prog_fd, &topts);
	if (!ASSERT_OK(err, "test_run_opts err"))
		return;
	if (!ASSERT_OK(topts.retval, "test_run_opts retval"))
		return;

	ASSERT_EQ(skel->data->xor64_value, 0x101ull << 32, "xor64_value");
	ASSERT_EQ(skel->bss->xor64_result, 0x110ull << 32, "xor64_result");

	ASSERT_EQ(skel->data->xor32_value, 0x101, "xor32_value");
	ASSERT_EQ(skel->bss->xor32_result, 0x110, "xor32_result");

	ASSERT_EQ(skel->data->xor_noreturn_value, 0x101ull << 32, "xor_nxoreturn_value");
}

static void test_cmpxchg(struct atomics_lskel *skel)
{
	int err, prog_fd;
	LIBBPF_OPTS(bpf_test_run_opts, topts);

	/* No need to attach it, just run it directly */
	prog_fd = skel->progs.cmpxchg.prog_fd;
	err = bpf_prog_test_run_opts(prog_fd, &topts);
	if (!ASSERT_OK(err, "test_run_opts err"))
		return;
	if (!ASSERT_OK(topts.retval, "test_run_opts retval"))
		return;

	ASSERT_EQ(skel->data->cmpxchg64_value, 2, "cmpxchg64_value");
	ASSERT_EQ(skel->bss->cmpxchg64_result_fail, 1, "cmpxchg_result_fail");
	ASSERT_EQ(skel->bss->cmpxchg64_result_succeed, 1, "cmpxchg_result_succeed");

	ASSERT_EQ(skel->data->cmpxchg32_value, 2, "lcmpxchg32_value");
	ASSERT_EQ(skel->bss->cmpxchg32_result_fail, 1, "cmpxchg_result_fail");
	ASSERT_EQ(skel->bss->cmpxchg32_result_succeed, 1, "cmpxchg_result_succeed");
}

static void test_xchg(struct atomics_lskel *skel)
{
	int err, prog_fd;
	LIBBPF_OPTS(bpf_test_run_opts, topts);

	/* No need to attach it, just run it directly */
	prog_fd = skel->progs.xchg.prog_fd;
	err = bpf_prog_test_run_opts(prog_fd, &topts);
	if (!ASSERT_OK(err, "test_run_opts err"))
		return;
	if (!ASSERT_OK(topts.retval, "test_run_opts retval"))
		return;

	ASSERT_EQ(skel->data->xchg64_value, 2, "xchg64_value");
	ASSERT_EQ(skel->bss->xchg64_result, 1, "xchg64_result");

	ASSERT_EQ(skel->data->xchg32_value, 2, "xchg32_value");
	ASSERT_EQ(skel->bss->xchg32_result, 1, "xchg32_result");
}

void test_atomics(void)
{
	struct atomics_lskel *skel;

	skel = atomics_lskel__open_and_load();
	if (!ASSERT_OK_PTR(skel, "atomics skeleton load"))
		return;

	if (skel->data->skip_tests) {
		printf("%s:SKIP:no ENABLE_ATOMICS_TESTS (missing Clang BPF atomics support)",
		       __func__);
		test__skip();
		goto cleanup;
	}
	skel->bss->pid = getpid();

	if (test__start_subtest("add"))
		test_add(skel);
	if (test__start_subtest("sub"))
		test_sub(skel);
	if (test__start_subtest("and"))
		test_and(skel);
	if (test__start_subtest("or"))
		test_or(skel);
	if (test__start_subtest("xor"))
		test_xor(skel);
	if (test__start_subtest("cmpxchg"))
		test_cmpxchg(skel);
	if (test__start_subtest("xchg"))
		test_xchg(skel);

cleanup:
	atomics_lskel__destroy(skel);
}
