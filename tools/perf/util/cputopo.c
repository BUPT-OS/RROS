// SPDX-License-Identifier: GPL-2.0
#include <sys/param.h>
#include <sys/utsname.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <api/fs/fs.h>
#include <linux/zalloc.h>
#include <perf/cpumap.h>

#include "cputopo.h"
#include "cpumap.h"
#include "debug.h"
#include "env.h"
#include "pmu.h"
#include "pmus.h"

#define PACKAGE_CPUS_FMT \
	"%s/devices/system/cpu/cpu%d/topology/package_cpus_list"
#define PACKAGE_CPUS_FMT_OLD \
	"%s/devices/system/cpu/cpu%d/topology/core_siblings_list"
#define DIE_CPUS_FMT \
	"%s/devices/system/cpu/cpu%d/topology/die_cpus_list"
#define CORE_CPUS_FMT \
	"%s/devices/system/cpu/cpu%d/topology/core_cpus_list"
#define CORE_CPUS_FMT_OLD \
	"%s/devices/system/cpu/cpu%d/topology/thread_siblings_list"
#define NODE_ONLINE_FMT \
	"%s/devices/system/node/online"
#define NODE_MEMINFO_FMT \
	"%s/devices/system/node/node%d/meminfo"
#define NODE_CPULIST_FMT \
	"%s/devices/system/node/node%d/cpulist"

static int build_cpu_topology(struct cpu_topology *tp, int cpu)
{
	FILE *fp;
	char filename[MAXPATHLEN];
	char *buf = NULL, *p;
	size_t len = 0;
	ssize_t sret;
	u32 i = 0;
	int ret = -1;

	scnprintf(filename, MAXPATHLEN, PACKAGE_CPUS_FMT,
		  sysfs__mountpoint(), cpu);
	if (access(filename, F_OK) == -1) {
		scnprintf(filename, MAXPATHLEN, PACKAGE_CPUS_FMT_OLD,
			sysfs__mountpoint(), cpu);
	}
	fp = fopen(filename, "r");
	if (!fp)
		goto try_dies;

	sret = getline(&buf, &len, fp);
	fclose(fp);
	if (sret <= 0)
		goto try_dies;

	p = strchr(buf, '\n');
	if (p)
		*p = '\0';

	for (i = 0; i < tp->package_cpus_lists; i++) {
		if (!strcmp(buf, tp->package_cpus_list[i]))
			break;
	}
	if (i == tp->package_cpus_lists) {
		tp->package_cpus_list[i] = buf;
		tp->package_cpus_lists++;
		buf = NULL;
		len = 0;
	}
	ret = 0;

try_dies:
	if (!tp->die_cpus_list)
		goto try_threads;

	scnprintf(filename, MAXPATHLEN, DIE_CPUS_FMT,
		  sysfs__mountpoint(), cpu);
	fp = fopen(filename, "r");
	if (!fp)
		goto try_threads;

	sret = getline(&buf, &len, fp);
	fclose(fp);
	if (sret <= 0)
		goto try_threads;

	p = strchr(buf, '\n');
	if (p)
		*p = '\0';

	for (i = 0; i < tp->die_cpus_lists; i++) {
		if (!strcmp(buf, tp->die_cpus_list[i]))
			break;
	}
	if (i == tp->die_cpus_lists) {
		tp->die_cpus_list[i] = buf;
		tp->die_cpus_lists++;
		buf = NULL;
		len = 0;
	}
	ret = 0;

try_threads:
	scnprintf(filename, MAXPATHLEN, CORE_CPUS_FMT,
		  sysfs__mountpoint(), cpu);
	if (access(filename, F_OK) == -1) {
		scnprintf(filename, MAXPATHLEN, CORE_CPUS_FMT_OLD,
			  sysfs__mountpoint(), cpu);
	}
	fp = fopen(filename, "r");
	if (!fp)
		goto done;

	if (getline(&buf, &len, fp) <= 0)
		goto done;

	p = strchr(buf, '\n');
	if (p)
		*p = '\0';

	for (i = 0; i < tp->core_cpus_lists; i++) {
		if (!strcmp(buf, tp->core_cpus_list[i]))
			break;
	}
	if (i == tp->core_cpus_lists) {
		tp->core_cpus_list[i] = buf;
		tp->core_cpus_lists++;
		buf = NULL;
	}
	ret = 0;
done:
	if (fp)
		fclose(fp);
	free(buf);
	return ret;
}

void cpu_topology__delete(struct cpu_topology *tp)
{
	u32 i;

	if (!tp)
		return;

	for (i = 0 ; i < tp->package_cpus_lists; i++)
		zfree(&tp->package_cpus_list[i]);

	for (i = 0 ; i < tp->die_cpus_lists; i++)
		zfree(&tp->die_cpus_list[i]);

	for (i = 0 ; i < tp->core_cpus_lists; i++)
		zfree(&tp->core_cpus_list[i]);

	free(tp);
}

bool cpu_topology__smt_on(const struct cpu_topology *topology)
{
	for (u32 i = 0; i < topology->core_cpus_lists; i++) {
		const char *cpu_list = topology->core_cpus_list[i];

		/*
		 * If there is a need to separate siblings in a core then SMT is
		 * enabled.
		 */
		if (strchr(cpu_list, ',') || strchr(cpu_list, '-'))
			return true;
	}
	return false;
}

bool cpu_topology__core_wide(const struct cpu_topology *topology,
			     const char *user_requested_cpu_list)
{
	struct perf_cpu_map *user_requested_cpus;

	/*
	 * If user_requested_cpu_list is empty then all CPUs are recorded and so
	 * core_wide is true.
	 */
	if (!user_requested_cpu_list)
		return true;

	user_requested_cpus = perf_cpu_map__new(user_requested_cpu_list);
	/* Check that every user requested CPU is the complete set of SMT threads on a core. */
	for (u32 i = 0; i < topology->core_cpus_lists; i++) {
		const char *core_cpu_list = topology->core_cpus_list[i];
		struct perf_cpu_map *core_cpus = perf_cpu_map__new(core_cpu_list);
		struct perf_cpu cpu;
		int idx;
		bool has_first, first = true;

		perf_cpu_map__for_each_cpu(cpu, idx, core_cpus) {
			if (first) {
				has_first = perf_cpu_map__has(user_requested_cpus, cpu);
				first = false;
			} else {
				/*
				 * If the first core CPU is user requested then
				 * all subsequent CPUs in the core must be user
				 * requested too. If the first CPU isn't user
				 * requested then none of the others must be
				 * too.
				 */
				if (perf_cpu_map__has(user_requested_cpus, cpu) != has_first) {
					perf_cpu_map__put(core_cpus);
					perf_cpu_map__put(user_requested_cpus);
					return false;
				}
			}
		}
		perf_cpu_map__put(core_cpus);
	}
	perf_cpu_map__put(user_requested_cpus);
	return true;
}

static bool has_die_topology(void)
{
	char filename[MAXPATHLEN];
	struct utsname uts;

	if (uname(&uts) < 0)
		return false;

	if (strncmp(uts.machine, "x86_64", 6) &&
	    strncmp(uts.machine, "s390x", 5))
		return false;

	scnprintf(filename, MAXPATHLEN, DIE_CPUS_FMT,
		  sysfs__mountpoint(), 0);
	if (access(filename, F_OK) == -1)
		return false;

	return true;
}

const struct cpu_topology *online_topology(void)
{
	static const struct cpu_topology *topology;

	if (!topology) {
		topology = cpu_topology__new();
		if (!topology) {
			pr_err("Error creating CPU topology");
			abort();
		}
	}
	return topology;
}

struct cpu_topology *cpu_topology__new(void)
{
	struct cpu_topology *tp = NULL;
	void *addr;
	u32 nr, i, nr_addr;
	size_t sz;
	long ncpus;
	int ret = -1;
	struct perf_cpu_map *map;
	bool has_die = has_die_topology();

	ncpus = cpu__max_present_cpu().cpu;

	/* build online CPU map */
	map = perf_cpu_map__new(NULL);
	if (map == NULL) {
		pr_debug("failed to get system cpumap\n");
		return NULL;
	}

	nr = (u32)(ncpus & UINT_MAX);

	sz = nr * sizeof(char *);
	if (has_die)
		nr_addr = 3;
	else
		nr_addr = 2;
	addr = calloc(1, sizeof(*tp) + nr_addr * sz);
	if (!addr)
		goto out_free;

	tp = addr;
	addr += sizeof(*tp);
	tp->package_cpus_list = addr;
	addr += sz;
	if (has_die) {
		tp->die_cpus_list = addr;
		addr += sz;
	}
	tp->core_cpus_list = addr;

	for (i = 0; i < nr; i++) {
		if (!perf_cpu_map__has(map, (struct perf_cpu){ .cpu = i }))
			continue;

		ret = build_cpu_topology(tp, i);
		if (ret < 0)
			break;
	}

out_free:
	perf_cpu_map__put(map);
	if (ret) {
		cpu_topology__delete(tp);
		tp = NULL;
	}
	return tp;
}

static int load_numa_node(struct numa_topology_node *node, int nr)
{
	char str[MAXPATHLEN];
	char field[32];
	char *buf = NULL, *p;
	size_t len = 0;
	int ret = -1;
	FILE *fp;
	u64 mem;

	node->node = (u32) nr;

	scnprintf(str, MAXPATHLEN, NODE_MEMINFO_FMT,
		  sysfs__mountpoint(), nr);
	fp = fopen(str, "r");
	if (!fp)
		return -1;

	while (getline(&buf, &len, fp) > 0) {
		/* skip over invalid lines */
		if (!strchr(buf, ':'))
			continue;
		if (sscanf(buf, "%*s %*d %31s %"PRIu64, field, &mem) != 2)
			goto err;
		if (!strcmp(field, "MemTotal:"))
			node->mem_total = mem;
		if (!strcmp(field, "MemFree:"))
			node->mem_free = mem;
		if (node->mem_total && node->mem_free)
			break;
	}

	fclose(fp);
	fp = NULL;

	scnprintf(str, MAXPATHLEN, NODE_CPULIST_FMT,
		  sysfs__mountpoint(), nr);

	fp = fopen(str, "r");
	if (!fp)
		return -1;

	if (getline(&buf, &len, fp) <= 0)
		goto err;

	p = strchr(buf, '\n');
	if (p)
		*p = '\0';

	node->cpus = buf;
	fclose(fp);
	return 0;

err:
	free(buf);
	if (fp)
		fclose(fp);
	return ret;
}

struct numa_topology *numa_topology__new(void)
{
	struct perf_cpu_map *node_map = NULL;
	struct numa_topology *tp = NULL;
	char path[MAXPATHLEN];
	char *buf = NULL;
	size_t len = 0;
	u32 nr, i;
	FILE *fp;
	char *c;

	scnprintf(path, MAXPATHLEN, NODE_ONLINE_FMT,
		  sysfs__mountpoint());

	fp = fopen(path, "r");
	if (!fp)
		return NULL;

	if (getline(&buf, &len, fp) <= 0)
		goto out;

	c = strchr(buf, '\n');
	if (c)
		*c = '\0';

	node_map = perf_cpu_map__new(buf);
	if (!node_map)
		goto out;

	nr = (u32) perf_cpu_map__nr(node_map);

	tp = zalloc(sizeof(*tp) + sizeof(tp->nodes[0])*nr);
	if (!tp)
		goto out;

	tp->nr = nr;

	for (i = 0; i < nr; i++) {
		if (load_numa_node(&tp->nodes[i], perf_cpu_map__cpu(node_map, i).cpu)) {
			numa_topology__delete(tp);
			tp = NULL;
			break;
		}
	}

out:
	free(buf);
	fclose(fp);
	perf_cpu_map__put(node_map);
	return tp;
}

void numa_topology__delete(struct numa_topology *tp)
{
	u32 i;

	for (i = 0; i < tp->nr; i++)
		zfree(&tp->nodes[i].cpus);

	free(tp);
}

static int load_hybrid_node(struct hybrid_topology_node *node,
			    struct perf_pmu *pmu)
{
	char *buf = NULL, *p;
	FILE *fp;
	size_t len = 0;

	node->pmu_name = strdup(pmu->name);
	if (!node->pmu_name)
		return -1;

	fp = perf_pmu__open_file(pmu, "cpus");
	if (!fp)
		goto err;

	if (getline(&buf, &len, fp) <= 0) {
		fclose(fp);
		goto err;
	}

	p = strchr(buf, '\n');
	if (p)
		*p = '\0';

	fclose(fp);
	node->cpus = buf;
	return 0;

err:
	zfree(&node->pmu_name);
	free(buf);
	return -1;
}

struct hybrid_topology *hybrid_topology__new(void)
{
	struct perf_pmu *pmu = NULL;
	struct hybrid_topology *tp = NULL;
	int nr = perf_pmus__num_core_pmus(), i = 0;

	if (nr <= 1)
		return NULL;

	tp = zalloc(sizeof(*tp) + sizeof(tp->nodes[0]) * nr);
	if (!tp)
		return NULL;

	tp->nr = nr;
	while ((pmu = perf_pmus__scan_core(pmu)) != NULL) {
		if (load_hybrid_node(&tp->nodes[i], pmu)) {
			hybrid_topology__delete(tp);
			return NULL;
		}
		i++;
	}

	return tp;
}

void hybrid_topology__delete(struct hybrid_topology *tp)
{
	u32 i;

	for (i = 0; i < tp->nr; i++) {
		zfree(&tp->nodes[i].pmu_name);
		zfree(&tp->nodes[i].cpus);
	}

	free(tp);
}
