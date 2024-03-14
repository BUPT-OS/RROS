// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2022 Meta Platforms, Inc. and affiliates. */

#define _GNU_SOURCE
#include <sys/wait.h>
#include <test_progs.h>
#include <unistd.h>

#include "task_kfunc_failure.skel.h"
#include "task_kfunc_success.skel.h"

static struct task_kfunc_success *open_load_task_kfunc_skel(void)
{
	struct task_kfunc_success *skel;
	int err;

	skel = task_kfunc_success__open();
	if (!ASSERT_OK_PTR(skel, "skel_open"))
		return NULL;

	skel->bss->pid = getpid();

	err = task_kfunc_success__load(skel);
	if (!ASSERT_OK(err, "skel_load"))
		goto cleanup;

	return skel;

cleanup:
	task_kfunc_success__destroy(skel);
	return NULL;
}

static void run_success_test(const char *prog_name)
{
	struct task_kfunc_success *skel;
	int status;
	pid_t child_pid;
	struct bpf_program *prog;
	struct bpf_link *link = NULL;

	skel = open_load_task_kfunc_skel();
	if (!ASSERT_OK_PTR(skel, "open_load_skel"))
		return;

	if (!ASSERT_OK(skel->bss->err, "pre_spawn_err"))
		goto cleanup;

	prog = bpf_object__find_program_by_name(skel->obj, prog_name);
	if (!ASSERT_OK_PTR(prog, "bpf_object__find_program_by_name"))
		goto cleanup;

	link = bpf_program__attach(prog);
	if (!ASSERT_OK_PTR(link, "attached_link"))
		goto cleanup;

	child_pid = fork();
	if (!ASSERT_GT(child_pid, -1, "child_pid"))
		goto cleanup;
	if (child_pid == 0)
		_exit(0);
	waitpid(child_pid, &status, 0);

	ASSERT_OK(skel->bss->err, "post_wait_err");

cleanup:
	bpf_link__destroy(link);
	task_kfunc_success__destroy(skel);
}

static const char * const success_tests[] = {
	"test_task_acquire_release_argument",
	"test_task_acquire_release_current",
	"test_task_acquire_leave_in_map",
	"test_task_xchg_release",
	"test_task_map_acquire_release",
	"test_task_current_acquire_release",
	"test_task_from_pid_arg",
	"test_task_from_pid_current",
	"test_task_from_pid_invalid",
	"task_kfunc_acquire_trusted_walked",
	"test_task_kfunc_flavor_relo",
	"test_task_kfunc_flavor_relo_not_found",
};

void test_task_kfunc(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(success_tests); i++) {
		if (!test__start_subtest(success_tests[i]))
			continue;

		run_success_test(success_tests[i]);
	}

	RUN_TESTS(task_kfunc_failure);
}
