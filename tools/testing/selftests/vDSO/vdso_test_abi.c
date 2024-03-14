// SPDX-License-Identifier: GPL-2.0
/*
 * vdso_full_test.c: Sample code to test all the timers.
 * Copyright (c) 2019 Arm Ltd.
 *
 * Compile with:
 * gcc -std=gnu99 vdso_full_test.c parse_vdso.c
 *
 */

#include <stdint.h>
#include <elf.h>
#include <stdio.h>
#include <time.h>
#include <sys/auxv.h>
#include <sys/time.h>
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>

#include "../kselftest.h"
#include "vdso_config.h"

extern void *vdso_sym(const char *version, const char *name);
extern void vdso_init_from_sysinfo_ehdr(uintptr_t base);
extern void vdso_init_from_auxv(void *auxv);

static const char *version;
static const char **name;

typedef long (*vdso_gettimeofday_t)(struct timeval *tv, struct timezone *tz);
typedef long (*vdso_clock_gettime_t)(clockid_t clk_id, struct timespec *ts);
typedef long (*vdso_clock_getres_t)(clockid_t clk_id, struct timespec *ts);
typedef time_t (*vdso_time_t)(time_t *t);

#define VDSO_TEST_PASS_MSG()	"\n%s(): PASS\n", __func__
#define VDSO_TEST_FAIL_MSG(x)	"\n%s(): %s FAIL\n", __func__, x
#define VDSO_TEST_SKIP_MSG(x)	"\n%s(): SKIP: Could not find %s\n", __func__, x

static void vdso_test_gettimeofday(void)
{
	/* Find gettimeofday. */
	vdso_gettimeofday_t vdso_gettimeofday =
		(vdso_gettimeofday_t)vdso_sym(version, name[0]);

	if (!vdso_gettimeofday) {
		ksft_test_result_skip(VDSO_TEST_SKIP_MSG(name[0]));
		return;
	}

	struct timeval tv;
	long ret = vdso_gettimeofday(&tv, 0);

	if (ret == 0) {
		ksft_print_msg("The time is %lld.%06lld\n",
			       (long long)tv.tv_sec, (long long)tv.tv_usec);
		ksft_test_result_pass(VDSO_TEST_PASS_MSG());
	} else {
		ksft_test_result_fail(VDSO_TEST_FAIL_MSG(name[0]));
	}
}

static void vdso_test_clock_gettime(clockid_t clk_id)
{
	/* Find clock_gettime. */
	vdso_clock_gettime_t vdso_clock_gettime =
		(vdso_clock_gettime_t)vdso_sym(version, name[1]);

	if (!vdso_clock_gettime) {
		ksft_test_result_skip(VDSO_TEST_SKIP_MSG(name[1]));
		return;
	}

	struct timespec ts;
	long ret = vdso_clock_gettime(clk_id, &ts);

	if (ret == 0) {
		ksft_print_msg("The time is %lld.%06lld\n",
			       (long long)ts.tv_sec, (long long)ts.tv_nsec);
		ksft_test_result_pass(VDSO_TEST_PASS_MSG());
	} else {
		ksft_test_result_fail(VDSO_TEST_FAIL_MSG(name[1]));
	}
}

static void vdso_test_time(void)
{
	/* Find time. */
	vdso_time_t vdso_time =
		(vdso_time_t)vdso_sym(version, name[2]);

	if (!vdso_time) {
		ksft_test_result_skip(VDSO_TEST_SKIP_MSG(name[2]));
		return;
	}

	long ret = vdso_time(NULL);

	if (ret > 0) {
		ksft_print_msg("The time in hours since January 1, 1970 is %lld\n",
				(long long)(ret / 3600));
		ksft_test_result_pass(VDSO_TEST_PASS_MSG());
	} else {
		ksft_test_result_fail(VDSO_TEST_FAIL_MSG(name[2]));
	}
}

static void vdso_test_clock_getres(clockid_t clk_id)
{
	int clock_getres_fail = 0;

	/* Find clock_getres. */
	vdso_clock_getres_t vdso_clock_getres =
		(vdso_clock_getres_t)vdso_sym(version, name[3]);

	if (!vdso_clock_getres) {
		ksft_test_result_skip(VDSO_TEST_SKIP_MSG(name[3]));
		return;
	}

	struct timespec ts, sys_ts;
	long ret = vdso_clock_getres(clk_id, &ts);

	if (ret == 0) {
		ksft_print_msg("The vdso resolution is %lld %lld\n",
			       (long long)ts.tv_sec, (long long)ts.tv_nsec);
	} else {
		clock_getres_fail++;
	}

	ret = syscall(SYS_clock_getres, clk_id, &sys_ts);

	ksft_print_msg("The syscall resolution is %lld %lld\n",
			(long long)sys_ts.tv_sec, (long long)sys_ts.tv_nsec);

	if ((sys_ts.tv_sec != ts.tv_sec) || (sys_ts.tv_nsec != ts.tv_nsec))
		clock_getres_fail++;

	if (clock_getres_fail > 0) {
		ksft_test_result_fail(VDSO_TEST_FAIL_MSG(name[3]));
	} else {
		ksft_test_result_pass(VDSO_TEST_PASS_MSG());
	}
}

const char *vdso_clock_name[12] = {
	"CLOCK_REALTIME",
	"CLOCK_MONOTONIC",
	"CLOCK_PROCESS_CPUTIME_ID",
	"CLOCK_THREAD_CPUTIME_ID",
	"CLOCK_MONOTONIC_RAW",
	"CLOCK_REALTIME_COARSE",
	"CLOCK_MONOTONIC_COARSE",
	"CLOCK_BOOTTIME",
	"CLOCK_REALTIME_ALARM",
	"CLOCK_BOOTTIME_ALARM",
	"CLOCK_SGI_CYCLE",
	"CLOCK_TAI",
};

/*
 * This function calls vdso_test_clock_gettime and vdso_test_clock_getres
 * with different values for clock_id.
 */
static inline void vdso_test_clock(clockid_t clock_id)
{
	ksft_print_msg("\nclock_id: %s\n", vdso_clock_name[clock_id]);

	vdso_test_clock_gettime(clock_id);

	vdso_test_clock_getres(clock_id);
}

#define VDSO_TEST_PLAN	16

int main(int argc, char **argv)
{
	unsigned long sysinfo_ehdr = getauxval(AT_SYSINFO_EHDR);

	ksft_print_header();
	ksft_set_plan(VDSO_TEST_PLAN);

	if (!sysinfo_ehdr) {
		printf("AT_SYSINFO_EHDR is not present!\n");
		return KSFT_SKIP;
	}

	version = versions[VDSO_VERSION];
	name = (const char **)&names[VDSO_NAMES];

	printf("[vDSO kselftest] VDSO_VERSION: %s\n", version);

	vdso_init_from_sysinfo_ehdr(getauxval(AT_SYSINFO_EHDR));

	vdso_test_gettimeofday();

#if _POSIX_TIMERS > 0

#ifdef CLOCK_REALTIME
	vdso_test_clock(CLOCK_REALTIME);
#endif

#ifdef CLOCK_BOOTTIME
	vdso_test_clock(CLOCK_BOOTTIME);
#endif

#ifdef CLOCK_TAI
	vdso_test_clock(CLOCK_TAI);
#endif

#ifdef CLOCK_REALTIME_COARSE
	vdso_test_clock(CLOCK_REALTIME_COARSE);
#endif

#ifdef CLOCK_MONOTONIC
	vdso_test_clock(CLOCK_MONOTONIC);
#endif

#ifdef CLOCK_MONOTONIC_RAW
	vdso_test_clock(CLOCK_MONOTONIC_RAW);
#endif

#ifdef CLOCK_MONOTONIC_COARSE
	vdso_test_clock(CLOCK_MONOTONIC_COARSE);
#endif

#endif

	vdso_test_time();

	ksft_print_cnts();
	return ksft_get_fail_cnt() == 0 ? KSFT_PASS : KSFT_FAIL;
}
