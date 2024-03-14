// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(C) 2015 Linaro Limited. All rights reserved.
 * Author: Mathieu Poirier <mathieu.poirier@linaro.org>
 */

#include <dirent.h>
#include <stdbool.h>
#include <linux/coresight-pmu.h>
#include <linux/zalloc.h>
#include <api/fs/fs.h>

#include "../../../util/auxtrace.h"
#include "../../../util/debug.h"
#include "../../../util/evlist.h"
#include "../../../util/pmu.h"
#include "../../../util/pmus.h"
#include "cs-etm.h"
#include "arm-spe.h"
#include "hisi-ptt.h"

static struct perf_pmu **find_all_arm_spe_pmus(int *nr_spes, int *err)
{
	struct perf_pmu **arm_spe_pmus = NULL;
	int ret, i, nr_cpus = sysconf(_SC_NPROCESSORS_CONF);
	/* arm_spe_xxxxxxxxx\0 */
	char arm_spe_pmu_name[sizeof(ARM_SPE_PMU_NAME) + 10];

	arm_spe_pmus = zalloc(sizeof(struct perf_pmu *) * nr_cpus);
	if (!arm_spe_pmus) {
		pr_err("spes alloc failed\n");
		*err = -ENOMEM;
		return NULL;
	}

	for (i = 0; i < nr_cpus; i++) {
		ret = sprintf(arm_spe_pmu_name, "%s%d", ARM_SPE_PMU_NAME, i);
		if (ret < 0) {
			pr_err("sprintf failed\n");
			*err = -ENOMEM;
			return NULL;
		}

		arm_spe_pmus[*nr_spes] = perf_pmus__find(arm_spe_pmu_name);
		if (arm_spe_pmus[*nr_spes]) {
			pr_debug2("%s %d: arm_spe_pmu %d type %d name %s\n",
				 __func__, __LINE__, *nr_spes,
				 arm_spe_pmus[*nr_spes]->type,
				 arm_spe_pmus[*nr_spes]->name);
			(*nr_spes)++;
		}
	}

	return arm_spe_pmus;
}

static struct perf_pmu **find_all_hisi_ptt_pmus(int *nr_ptts, int *err)
{
	struct perf_pmu **hisi_ptt_pmus = NULL;
	struct dirent *dent;
	char path[PATH_MAX];
	DIR *dir = NULL;
	int idx = 0;

	perf_pmu__event_source_devices_scnprintf(path, sizeof(path));
	dir = opendir(path);
	if (!dir) {
		pr_err("can't read directory '%s'\n", path);
		*err = -EINVAL;
		return NULL;
	}

	while ((dent = readdir(dir))) {
		if (strstr(dent->d_name, HISI_PTT_PMU_NAME))
			(*nr_ptts)++;
	}

	if (!(*nr_ptts))
		goto out;

	hisi_ptt_pmus = zalloc(sizeof(struct perf_pmu *) * (*nr_ptts));
	if (!hisi_ptt_pmus) {
		pr_err("hisi_ptt alloc failed\n");
		*err = -ENOMEM;
		goto out;
	}

	rewinddir(dir);
	while ((dent = readdir(dir))) {
		if (strstr(dent->d_name, HISI_PTT_PMU_NAME) && idx < *nr_ptts) {
			hisi_ptt_pmus[idx] = perf_pmus__find(dent->d_name);
			if (hisi_ptt_pmus[idx])
				idx++;
		}
	}

out:
	closedir(dir);
	return hisi_ptt_pmus;
}

static struct perf_pmu *find_pmu_for_event(struct perf_pmu **pmus,
					   int pmu_nr, struct evsel *evsel)
{
	int i;

	if (!pmus)
		return NULL;

	for (i = 0; i < pmu_nr; i++) {
		if (evsel->core.attr.type == pmus[i]->type)
			return pmus[i];
	}

	return NULL;
}

struct auxtrace_record
*auxtrace_record__init(struct evlist *evlist, int *err)
{
	struct perf_pmu	*cs_etm_pmu = NULL;
	struct perf_pmu **arm_spe_pmus = NULL;
	struct perf_pmu **hisi_ptt_pmus = NULL;
	struct evsel *evsel;
	struct perf_pmu *found_etm = NULL;
	struct perf_pmu *found_spe = NULL;
	struct perf_pmu *found_ptt = NULL;
	int auxtrace_event_cnt = 0;
	int nr_spes = 0;
	int nr_ptts = 0;

	if (!evlist)
		return NULL;

	cs_etm_pmu = perf_pmus__find(CORESIGHT_ETM_PMU_NAME);
	arm_spe_pmus = find_all_arm_spe_pmus(&nr_spes, err);
	hisi_ptt_pmus = find_all_hisi_ptt_pmus(&nr_ptts, err);

	evlist__for_each_entry(evlist, evsel) {
		if (cs_etm_pmu && !found_etm)
			found_etm = find_pmu_for_event(&cs_etm_pmu, 1, evsel);

		if (arm_spe_pmus && !found_spe)
			found_spe = find_pmu_for_event(arm_spe_pmus, nr_spes, evsel);

		if (hisi_ptt_pmus && !found_ptt)
			found_ptt = find_pmu_for_event(hisi_ptt_pmus, nr_ptts, evsel);
	}

	free(arm_spe_pmus);
	free(hisi_ptt_pmus);

	if (found_etm)
		auxtrace_event_cnt++;

	if (found_spe)
		auxtrace_event_cnt++;

	if (found_ptt)
		auxtrace_event_cnt++;

	if (auxtrace_event_cnt > 1) {
		pr_err("Concurrent AUX trace operation not currently supported\n");
		*err = -EOPNOTSUPP;
		return NULL;
	}

	if (found_etm)
		return cs_etm_record_init(err);

#if defined(__aarch64__)
	if (found_spe)
		return arm_spe_recording_init(err, found_spe);

	if (found_ptt)
		return hisi_ptt_recording_init(err, found_ptt);
#endif

	/*
	 * Clear 'err' even if we haven't found an event - that way perf
	 * record can still be used even if tracers aren't present.  The NULL
	 * return value will take care of telling the infrastructure HW tracing
	 * isn't available.
	 */
	*err = 0;
	return NULL;
}

#if defined(__arm__)
u64 compat_auxtrace_mmap__read_head(struct auxtrace_mmap *mm)
{
	struct perf_event_mmap_page *pc = mm->userpg;
	u64 result;

	__asm__ __volatile__(
"	ldrd    %0, %H0, [%1]"
	: "=&r" (result)
	: "r" (&pc->aux_head), "Qo" (pc->aux_head)
	);

	return result;
}

int compat_auxtrace_mmap__write_tail(struct auxtrace_mmap *mm, u64 tail)
{
	struct perf_event_mmap_page *pc = mm->userpg;

	/* Ensure all reads are done before we write the tail out */
	smp_mb();

	__asm__ __volatile__(
"	strd    %2, %H2, [%1]"
	: "=Qo" (pc->aux_tail)
	: "r" (&pc->aux_tail), "r" (tail)
	);

	return 0;
}
#endif
