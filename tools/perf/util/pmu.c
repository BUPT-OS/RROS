// SPDX-License-Identifier: GPL-2.0
#include <linux/list.h>
#include <linux/compiler.h>
#include <linux/string.h>
#include <linux/zalloc.h>
#include <linux/ctype.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <dirent.h>
#include <api/fs/fs.h>
#include <locale.h>
#include <fnmatch.h>
#include <math.h>
#include "debug.h"
#include "evsel.h"
#include "pmu.h"
#include "pmus.h"
#include <util/pmu-bison.h>
#include <util/pmu-flex.h>
#include "parse-events.h"
#include "print-events.h"
#include "header.h"
#include "string2.h"
#include "strbuf.h"
#include "fncache.h"
#include "util/evsel_config.h"

struct perf_pmu perf_pmu__fake = {
	.name = "fake",
};

#define UNIT_MAX_LEN	31 /* max length for event unit name */

/**
 * struct perf_pmu_alias - An event either read from sysfs or builtin in
 * pmu-events.c, created by parsing the pmu-events json files.
 */
struct perf_pmu_alias {
	/** @name: Name of the event like "mem-loads". */
	char *name;
	/** @desc: Optional short description of the event. */
	char *desc;
	/** @long_desc: Optional long description. */
	char *long_desc;
	/**
	 * @topic: Optional topic such as cache or pipeline, particularly for
	 * json events.
	 */
	char *topic;
	/** @terms: Owned list of the original parsed parameters. */
	struct list_head terms;
	/** @list: List element of struct perf_pmu aliases. */
	struct list_head list;
	/**
	 * @pmu_name: The name copied from the json struct pmu_event. This can
	 * differ from the PMU name as it won't have suffixes.
	 */
	char *pmu_name;
	/** @unit: Units for the event, such as bytes or cache lines. */
	char unit[UNIT_MAX_LEN+1];
	/** @scale: Value to scale read counter values by. */
	double scale;
	/**
	 * @per_pkg: Does the file
	 * <sysfs>/bus/event_source/devices/<pmu_name>/events/<name>.per-pkg or
	 * equivalent json value exist and have the value 1.
	 */
	bool per_pkg;
	/**
	 * @snapshot: Does the file
	 * <sysfs>/bus/event_source/devices/<pmu_name>/events/<name>.snapshot
	 * exist and have the value 1.
	 */
	bool snapshot;
	/**
	 * @deprecated: Is the event hidden and so not shown in perf list by
	 * default.
	 */
	bool deprecated;
	/** @from_sysfs: Was the alias from sysfs or a json event? */
	bool from_sysfs;
	/** @info_loaded: Have the scale, unit and other values been read from disk? */
	bool info_loaded;
};

/**
 * struct perf_pmu_format - Values from a format file read from
 * <sysfs>/devices/cpu/format/ held in struct perf_pmu.
 *
 * For example, the contents of <sysfs>/devices/cpu/format/event may be
 * "config:0-7" and will be represented here as name="event",
 * value=PERF_PMU_FORMAT_VALUE_CONFIG and bits 0 to 7 will be set.
 */
struct perf_pmu_format {
	/** @list: Element on list within struct perf_pmu. */
	struct list_head list;
	/** @bits: Which config bits are set by this format value. */
	DECLARE_BITMAP(bits, PERF_PMU_FORMAT_BITS);
	/** @name: The modifier/file name. */
	char *name;
	/**
	 * @value : Which config value the format relates to. Supported values
	 * are from PERF_PMU_FORMAT_VALUE_CONFIG to
	 * PERF_PMU_FORMAT_VALUE_CONFIG_END.
	 */
	u16 value;
	/** @loaded: Has the contents been loaded/parsed. */
	bool loaded;
};

static int pmu_aliases_parse(struct perf_pmu *pmu);

static struct perf_pmu_format *perf_pmu__new_format(struct list_head *list, char *name)
{
	struct perf_pmu_format *format;

	format = zalloc(sizeof(*format));
	if (!format)
		return NULL;

	format->name = strdup(name);
	if (!format->name) {
		free(format);
		return NULL;
	}
	list_add_tail(&format->list, list);
	return format;
}

/* Called at the end of parsing a format. */
void perf_pmu_format__set_value(void *vformat, int config, unsigned long *bits)
{
	struct perf_pmu_format *format = vformat;

	format->value = config;
	memcpy(format->bits, bits, sizeof(format->bits));
}

static void __perf_pmu_format__load(struct perf_pmu_format *format, FILE *file)
{
	void *scanner;
	int ret;

	ret = perf_pmu_lex_init(&scanner);
	if (ret)
		return;

	perf_pmu_set_in(file, scanner);
	ret = perf_pmu_parse(format, scanner);
	perf_pmu_lex_destroy(scanner);
	format->loaded = true;
}

static void perf_pmu_format__load(struct perf_pmu *pmu, struct perf_pmu_format *format)
{
	char path[PATH_MAX];
	FILE *file = NULL;

	if (format->loaded)
		return;

	if (!perf_pmu__pathname_scnprintf(path, sizeof(path), pmu->name, "format"))
		return;

	assert(strlen(path) + strlen(format->name) + 2 < sizeof(path));
	strcat(path, "/");
	strcat(path, format->name);

	file = fopen(path, "r");
	if (!file)
		return;
	__perf_pmu_format__load(format, file);
	fclose(file);
}

/*
 * Parse & process all the sysfs attributes located under
 * the directory specified in 'dir' parameter.
 */
int perf_pmu__format_parse(struct perf_pmu *pmu, int dirfd, bool eager_load)
{
	struct dirent *evt_ent;
	DIR *format_dir;
	int ret = 0;

	format_dir = fdopendir(dirfd);
	if (!format_dir)
		return -EINVAL;

	while ((evt_ent = readdir(format_dir)) != NULL) {
		struct perf_pmu_format *format;
		char *name = evt_ent->d_name;

		if (!strcmp(name, ".") || !strcmp(name, ".."))
			continue;

		format = perf_pmu__new_format(&pmu->format, name);
		if (!format) {
			ret = -ENOMEM;
			break;
		}

		if (eager_load) {
			FILE *file;
			int fd = openat(dirfd, name, O_RDONLY);

			if (fd < 0) {
				ret = -errno;
				break;
			}
			file = fdopen(fd, "r");
			if (!file) {
				close(fd);
				break;
			}
			__perf_pmu_format__load(format, file);
			fclose(file);
		}
	}

	closedir(format_dir);
	return ret;
}

/*
 * Reading/parsing the default pmu format definition, which should be
 * located at:
 * /sys/bus/event_source/devices/<dev>/format as sysfs group attributes.
 */
static int pmu_format(struct perf_pmu *pmu, int dirfd, const char *name)
{
	int fd;

	fd = perf_pmu__pathname_fd(dirfd, name, "format", O_DIRECTORY);
	if (fd < 0)
		return 0;

	/* it'll close the fd */
	if (perf_pmu__format_parse(pmu, fd, /*eager_load=*/false))
		return -1;

	return 0;
}

int perf_pmu__convert_scale(const char *scale, char **end, double *sval)
{
	char *lc;
	int ret = 0;

	/*
	 * save current locale
	 */
	lc = setlocale(LC_NUMERIC, NULL);

	/*
	 * The lc string may be allocated in static storage,
	 * so get a dynamic copy to make it survive setlocale
	 * call below.
	 */
	lc = strdup(lc);
	if (!lc) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * force to C locale to ensure kernel
	 * scale string is converted correctly.
	 * kernel uses default C locale.
	 */
	setlocale(LC_NUMERIC, "C");

	*sval = strtod(scale, end);

out:
	/* restore locale */
	setlocale(LC_NUMERIC, lc);
	free(lc);
	return ret;
}

static int perf_pmu__parse_scale(struct perf_pmu *pmu, struct perf_pmu_alias *alias)
{
	struct stat st;
	ssize_t sret;
	size_t len;
	char scale[128];
	int fd, ret = -1;
	char path[PATH_MAX];

	len = perf_pmu__event_source_devices_scnprintf(path, sizeof(path));
	if (!len)
		return 0;
	scnprintf(path + len, sizeof(path) - len, "%s/%s.scale", pmu->name, alias->name);

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return -1;

	if (fstat(fd, &st) < 0)
		goto error;

	sret = read(fd, scale, sizeof(scale)-1);
	if (sret < 0)
		goto error;

	if (scale[sret - 1] == '\n')
		scale[sret - 1] = '\0';
	else
		scale[sret] = '\0';

	ret = perf_pmu__convert_scale(scale, NULL, &alias->scale);
error:
	close(fd);
	return ret;
}

static int perf_pmu__parse_unit(struct perf_pmu *pmu, struct perf_pmu_alias *alias)
{
	char path[PATH_MAX];
	size_t len;
	ssize_t sret;
	int fd;


	len = perf_pmu__event_source_devices_scnprintf(path, sizeof(path));
	if (!len)
		return 0;
	scnprintf(path + len, sizeof(path) - len, "%s/%s.unit", pmu->name, alias->name);

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return -1;

	sret = read(fd, alias->unit, UNIT_MAX_LEN);
	if (sret < 0)
		goto error;

	close(fd);

	if (alias->unit[sret - 1] == '\n')
		alias->unit[sret - 1] = '\0';
	else
		alias->unit[sret] = '\0';

	return 0;
error:
	close(fd);
	alias->unit[0] = '\0';
	return -1;
}

static int
perf_pmu__parse_per_pkg(struct perf_pmu *pmu, struct perf_pmu_alias *alias)
{
	char path[PATH_MAX];
	size_t len;
	int fd;

	len = perf_pmu__event_source_devices_scnprintf(path, sizeof(path));
	if (!len)
		return 0;
	scnprintf(path + len, sizeof(path) - len, "%s/%s.per-pkg", pmu->name, alias->name);

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return -1;

	close(fd);

	alias->per_pkg = true;
	return 0;
}

static int perf_pmu__parse_snapshot(struct perf_pmu *pmu, struct perf_pmu_alias *alias)
{
	char path[PATH_MAX];
	size_t len;
	int fd;

	len = perf_pmu__event_source_devices_scnprintf(path, sizeof(path));
	if (!len)
		return 0;
	scnprintf(path + len, sizeof(path) - len, "%s/%s.snapshot", pmu->name, alias->name);

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return -1;

	alias->snapshot = true;
	close(fd);
	return 0;
}

/* Delete an alias entry. */
static void perf_pmu_free_alias(struct perf_pmu_alias *newalias)
{
	zfree(&newalias->name);
	zfree(&newalias->desc);
	zfree(&newalias->long_desc);
	zfree(&newalias->topic);
	zfree(&newalias->pmu_name);
	parse_events_terms__purge(&newalias->terms);
	free(newalias);
}

static void perf_pmu__del_aliases(struct perf_pmu *pmu)
{
	struct perf_pmu_alias *alias, *tmp;

	list_for_each_entry_safe(alias, tmp, &pmu->aliases, list) {
		list_del(&alias->list);
		perf_pmu_free_alias(alias);
	}
}

static struct perf_pmu_alias *perf_pmu__find_alias(struct perf_pmu *pmu,
						   const char *name,
						   bool load)
{
	struct perf_pmu_alias *alias;

	if (load && !pmu->sysfs_aliases_loaded)
		pmu_aliases_parse(pmu);

	list_for_each_entry(alias, &pmu->aliases, list) {
		if (!strcasecmp(alias->name, name))
			return alias;
	}
	return NULL;
}

static bool assign_str(const char *name, const char *field, char **old_str,
				const char *new_str)
{
	if (!*old_str && new_str) {
		*old_str = strdup(new_str);
		return true;
	}

	if (!new_str || !strcasecmp(*old_str, new_str))
		return false; /* Nothing to update. */

	pr_debug("alias %s differs in field '%s' ('%s' != '%s')\n",
		name, field, *old_str, new_str);
	zfree(old_str);
	*old_str = strdup(new_str);
	return true;
}

static void read_alias_info(struct perf_pmu *pmu, struct perf_pmu_alias *alias)
{
	if (!alias->from_sysfs || alias->info_loaded)
		return;

	/*
	 * load unit name and scale if available
	 */
	perf_pmu__parse_unit(pmu, alias);
	perf_pmu__parse_scale(pmu, alias);
	perf_pmu__parse_per_pkg(pmu, alias);
	perf_pmu__parse_snapshot(pmu, alias);
}

struct update_alias_data {
	struct perf_pmu *pmu;
	struct perf_pmu_alias *alias;
};

static int update_alias(const struct pmu_event *pe,
			const struct pmu_events_table *table __maybe_unused,
			void *vdata)
{
	struct update_alias_data *data = vdata;
	int ret = 0;

	read_alias_info(data->pmu, data->alias);
	assign_str(pe->name, "desc", &data->alias->desc, pe->desc);
	assign_str(pe->name, "long_desc", &data->alias->long_desc, pe->long_desc);
	assign_str(pe->name, "topic", &data->alias->topic, pe->topic);
	data->alias->per_pkg = pe->perpkg;
	if (pe->event) {
		parse_events_terms__purge(&data->alias->terms);
		ret = parse_events_terms(&data->alias->terms, pe->event, /*input=*/NULL);
	}
	if (!ret && pe->unit) {
		char *unit;

		ret = perf_pmu__convert_scale(pe->unit, &unit, &data->alias->scale);
		if (!ret)
			snprintf(data->alias->unit, sizeof(data->alias->unit), "%s", unit);
	}
	return ret;
}

static int perf_pmu__new_alias(struct perf_pmu *pmu, const char *name,
				const char *desc, const char *val, FILE *val_fd,
				const struct pmu_event *pe)
{
	struct perf_pmu_alias *alias;
	int ret;
	const char *long_desc = NULL, *topic = NULL, *unit = NULL, *pmu_name = NULL;
	bool deprecated = false, perpkg = false;

	if (perf_pmu__find_alias(pmu, name, /*load=*/ false)) {
		/* Alias was already created/loaded. */
		return 0;
	}

	if (pe) {
		long_desc = pe->long_desc;
		topic = pe->topic;
		unit = pe->unit;
		perpkg = pe->perpkg;
		deprecated = pe->deprecated;
		pmu_name = pe->pmu;
	}

	alias = zalloc(sizeof(*alias));
	if (!alias)
		return -ENOMEM;

	INIT_LIST_HEAD(&alias->terms);
	alias->scale = 1.0;
	alias->unit[0] = '\0';
	alias->per_pkg = perpkg;
	alias->snapshot = false;
	alias->deprecated = deprecated;

	ret = parse_events_terms(&alias->terms, val, val_fd);
	if (ret) {
		pr_err("Cannot parse alias %s: %d\n", val, ret);
		free(alias);
		return ret;
	}

	alias->name = strdup(name);
	alias->desc = desc ? strdup(desc) : NULL;
	alias->long_desc = long_desc ? strdup(long_desc) :
				desc ? strdup(desc) : NULL;
	alias->topic = topic ? strdup(topic) : NULL;
	alias->pmu_name = pmu_name ? strdup(pmu_name) : NULL;
	if (unit) {
		if (perf_pmu__convert_scale(unit, (char **)&unit, &alias->scale) < 0) {
			perf_pmu_free_alias(alias);
			return -1;
		}
		snprintf(alias->unit, sizeof(alias->unit), "%s", unit);
	}
	if (!pe) {
		/* Update an event from sysfs with json data. */
		struct update_alias_data data = {
			.pmu = pmu,
			.alias = alias,
		};

		alias->from_sysfs = true;
		if (pmu->events_table) {
			if (pmu_events_table__find_event(pmu->events_table, pmu, name,
							 update_alias, &data) == 0)
				pmu->loaded_json_aliases++;
		}
	}

	if (!pe)
		pmu->sysfs_aliases++;
	else
		pmu->loaded_json_aliases++;
	list_add_tail(&alias->list, &pmu->aliases);
	return 0;
}

static inline bool pmu_alias_info_file(char *name)
{
	size_t len;

	len = strlen(name);
	if (len > 5 && !strcmp(name + len - 5, ".unit"))
		return true;
	if (len > 6 && !strcmp(name + len - 6, ".scale"))
		return true;
	if (len > 8 && !strcmp(name + len - 8, ".per-pkg"))
		return true;
	if (len > 9 && !strcmp(name + len - 9, ".snapshot"))
		return true;

	return false;
}

/*
 * Reading the pmu event aliases definition, which should be located at:
 * /sys/bus/event_source/devices/<dev>/events as sysfs group attributes.
 */
static int pmu_aliases_parse(struct perf_pmu *pmu)
{
	char path[PATH_MAX];
	struct dirent *evt_ent;
	DIR *event_dir;
	size_t len;
	int fd, dir_fd;

	len = perf_pmu__event_source_devices_scnprintf(path, sizeof(path));
	if (!len)
		return 0;
	scnprintf(path + len, sizeof(path) - len, "%s/events", pmu->name);

	dir_fd = open(path, O_DIRECTORY);
	if (dir_fd == -1) {
		pmu->sysfs_aliases_loaded = true;
		return 0;
	}

	event_dir = fdopendir(dir_fd);
	if (!event_dir){
		close (dir_fd);
		return -EINVAL;
	}

	while ((evt_ent = readdir(event_dir))) {
		char *name = evt_ent->d_name;
		FILE *file;

		if (!strcmp(name, ".") || !strcmp(name, ".."))
			continue;

		/*
		 * skip info files parsed in perf_pmu__new_alias()
		 */
		if (pmu_alias_info_file(name))
			continue;

		fd = openat(dir_fd, name, O_RDONLY);
		if (fd == -1) {
			pr_debug("Cannot open %s\n", name);
			continue;
		}
		file = fdopen(fd, "r");
		if (!file) {
			close(fd);
			continue;
		}

		if (perf_pmu__new_alias(pmu, name, /*desc=*/ NULL,
					/*val=*/ NULL, file, /*pe=*/ NULL) < 0)
			pr_debug("Cannot set up %s\n", name);
		fclose(file);
	}

	closedir(event_dir);
	close (dir_fd);
	pmu->sysfs_aliases_loaded = true;
	return 0;
}

static int pmu_alias_terms(struct perf_pmu_alias *alias,
			   struct list_head *terms)
{
	struct parse_events_term *term, *cloned;
	LIST_HEAD(list);
	int ret;

	list_for_each_entry(term, &alias->terms, list) {
		ret = parse_events_term__clone(&cloned, term);
		if (ret) {
			parse_events_terms__purge(&list);
			return ret;
		}
		/*
		 * Weak terms don't override command line options,
		 * which we don't want for implicit terms in aliases.
		 */
		cloned->weak = true;
		list_add_tail(&cloned->list, &list);
	}
	list_splice(&list, terms);
	return 0;
}

/*
 * Uncore PMUs have a "cpumask" file under sysfs. CPU PMUs (e.g. on arm/arm64)
 * may have a "cpus" file.
 */
static struct perf_cpu_map *pmu_cpumask(int dirfd, const char *name, bool is_core)
{
	struct perf_cpu_map *cpus;
	const char *templates[] = {
		"cpumask",
		"cpus",
		NULL
	};
	const char **template;
	char pmu_name[PATH_MAX];
	struct perf_pmu pmu = {.name = pmu_name};
	FILE *file;

	strlcpy(pmu_name, name, sizeof(pmu_name));
	for (template = templates; *template; template++) {
		file = perf_pmu__open_file_at(&pmu, dirfd, *template);
		if (!file)
			continue;
		cpus = perf_cpu_map__read(file);
		fclose(file);
		if (cpus)
			return cpus;
	}

	/* Nothing found, for core PMUs assume this means all CPUs. */
	return is_core ? perf_cpu_map__get(cpu_map__online()) : NULL;
}

static bool pmu_is_uncore(int dirfd, const char *name)
{
	int fd;

	fd = perf_pmu__pathname_fd(dirfd, name, "cpumask", O_PATH);
	if (fd < 0)
		return false;

	close(fd);
	return true;
}

static char *pmu_id(const char *name)
{
	char path[PATH_MAX], *str;
	size_t len;

	perf_pmu__pathname_scnprintf(path, sizeof(path), name, "identifier");

	if (filename__read_str(path, &str, &len) < 0)
		return NULL;

	str[len - 1] = 0; /* remove line feed */

	return str;
}

/**
 * is_sysfs_pmu_core() - PMU CORE devices have different name other than cpu in
 *         sysfs on some platforms like ARM or Intel hybrid. Looking for
 *         possible the cpus file in sysfs files to identify whether this is a
 *         core device.
 * @name: The PMU name such as "cpu_atom".
 */
static int is_sysfs_pmu_core(const char *name)
{
	char path[PATH_MAX];

	if (!perf_pmu__pathname_scnprintf(path, sizeof(path), name, "cpus"))
		return 0;
	return file_available(path);
}

char *perf_pmu__getcpuid(struct perf_pmu *pmu)
{
	char *cpuid;
	static bool printed;

	cpuid = getenv("PERF_CPUID");
	if (cpuid)
		cpuid = strdup(cpuid);
	if (!cpuid)
		cpuid = get_cpuid_str(pmu);
	if (!cpuid)
		return NULL;

	if (!printed) {
		pr_debug("Using CPUID %s\n", cpuid);
		printed = true;
	}
	return cpuid;
}

__weak const struct pmu_events_table *pmu_events_table__find(void)
{
	return perf_pmu__find_events_table(NULL);
}

__weak const struct pmu_metrics_table *pmu_metrics_table__find(void)
{
	return perf_pmu__find_metrics_table(NULL);
}

/**
 * perf_pmu__match_ignoring_suffix - Does the pmu_name match tok ignoring any
 *                                   trailing suffix? The Suffix must be in form
 *                                   tok_{digits}, or tok{digits}.
 * @pmu_name: The pmu_name with possible suffix.
 * @tok: The possible match to pmu_name without suffix.
 */
static bool perf_pmu__match_ignoring_suffix(const char *pmu_name, const char *tok)
{
	const char *p;

	if (strncmp(pmu_name, tok, strlen(tok)))
		return false;

	p = pmu_name + strlen(tok);
	if (*p == 0)
		return true;

	if (*p == '_')
		++p;

	/* Ensure we end in a number */
	while (1) {
		if (!isdigit(*p))
			return false;
		if (*(++p) == 0)
			break;
	}

	return true;
}

/**
 * pmu_uncore_alias_match - does name match the PMU name?
 * @pmu_name: the json struct pmu_event name. This may lack a suffix (which
 *            matches) or be of the form "socket,pmuname" which will match
 *            "socketX_pmunameY".
 * @name: a real full PMU name as from sysfs.
 */
static bool pmu_uncore_alias_match(const char *pmu_name, const char *name)
{
	char *tmp = NULL, *tok, *str;
	bool res;

	if (strchr(pmu_name, ',') == NULL)
		return perf_pmu__match_ignoring_suffix(name, pmu_name);

	str = strdup(pmu_name);
	if (!str)
		return false;

	/*
	 * uncore alias may be from different PMU with common prefix
	 */
	tok = strtok_r(str, ",", &tmp);
	if (strncmp(pmu_name, tok, strlen(tok))) {
		res = false;
		goto out;
	}

	/*
	 * Match more complex aliases where the alias name is a comma-delimited
	 * list of tokens, orderly contained in the matching PMU name.
	 *
	 * Example: For alias "socket,pmuname" and PMU "socketX_pmunameY", we
	 *	    match "socket" in "socketX_pmunameY" and then "pmuname" in
	 *	    "pmunameY".
	 */
	while (1) {
		char *next_tok = strtok_r(NULL, ",", &tmp);

		name = strstr(name, tok);
		if (!name ||
		    (!next_tok && !perf_pmu__match_ignoring_suffix(name, tok))) {
			res = false;
			goto out;
		}
		if (!next_tok)
			break;
		tok = next_tok;
		name += strlen(tok);
	}

	res = true;
out:
	free(str);
	return res;
}

static int pmu_add_cpu_aliases_map_callback(const struct pmu_event *pe,
					const struct pmu_events_table *table __maybe_unused,
					void *vdata)
{
	struct perf_pmu *pmu = vdata;

	perf_pmu__new_alias(pmu, pe->name, pe->desc, pe->event, /*val_fd=*/ NULL, pe);
	return 0;
}

/*
 * From the pmu_events_table, find the events that correspond to the given
 * PMU and add them to the list 'head'.
 */
void pmu_add_cpu_aliases_table(struct perf_pmu *pmu, const struct pmu_events_table *table)
{
	pmu_events_table__for_each_event(table, pmu, pmu_add_cpu_aliases_map_callback, pmu);
}

static void pmu_add_cpu_aliases(struct perf_pmu *pmu)
{
	if (!pmu->events_table)
		return;

	if (pmu->cpu_aliases_added)
		return;

	pmu_add_cpu_aliases_table(pmu, pmu->events_table);
	pmu->cpu_aliases_added = true;
}

static int pmu_add_sys_aliases_iter_fn(const struct pmu_event *pe,
				       const struct pmu_events_table *table __maybe_unused,
				       void *vdata)
{
	struct perf_pmu *pmu = vdata;

	if (!pe->compat || !pe->pmu)
		return 0;

	if (!strcmp(pmu->id, pe->compat) &&
	    pmu_uncore_alias_match(pe->pmu, pmu->name)) {
		perf_pmu__new_alias(pmu,
				pe->name,
				pe->desc,
				pe->event,
				/*val_fd=*/ NULL,
				pe);
	}

	return 0;
}

void pmu_add_sys_aliases(struct perf_pmu *pmu)
{
	if (!pmu->id)
		return;

	pmu_for_each_sys_event(pmu_add_sys_aliases_iter_fn, pmu);
}

struct perf_event_attr * __weak
perf_pmu__get_default_config(struct perf_pmu *pmu __maybe_unused)
{
	return NULL;
}

const char * __weak
pmu_find_real_name(const char *name)
{
	return name;
}

const char * __weak
pmu_find_alias_name(const char *name __maybe_unused)
{
	return NULL;
}

static int pmu_max_precise(int dirfd, struct perf_pmu *pmu)
{
	int max_precise = -1;

	perf_pmu__scan_file_at(pmu, dirfd, "caps/max_precise", "%d", &max_precise);
	return max_precise;
}

struct perf_pmu *perf_pmu__lookup(struct list_head *pmus, int dirfd, const char *lookup_name)
{
	struct perf_pmu *pmu;
	__u32 type;
	const char *name = pmu_find_real_name(lookup_name);
	const char *alias_name;

	pmu = zalloc(sizeof(*pmu));
	if (!pmu)
		return NULL;

	pmu->name = strdup(name);
	if (!pmu->name)
		goto err;

	/*
	 * Read type early to fail fast if a lookup name isn't a PMU. Ensure
	 * that type value is successfully assigned (return 1).
	 */
	if (perf_pmu__scan_file_at(pmu, dirfd, "type", "%u", &type) != 1)
		goto err;

	INIT_LIST_HEAD(&pmu->format);
	INIT_LIST_HEAD(&pmu->aliases);
	INIT_LIST_HEAD(&pmu->caps);

	/*
	 * The pmu data we store & need consists of the pmu
	 * type value and format definitions. Load both right
	 * now.
	 */
	if (pmu_format(pmu, dirfd, name)) {
		free(pmu);
		return NULL;
	}
	pmu->is_core = is_pmu_core(name);
	pmu->cpus = pmu_cpumask(dirfd, name, pmu->is_core);

	alias_name = pmu_find_alias_name(name);
	if (alias_name) {
		pmu->alias_name = strdup(alias_name);
		if (!pmu->alias_name)
			goto err;
	}

	pmu->type = type;
	pmu->is_uncore = pmu_is_uncore(dirfd, name);
	if (pmu->is_uncore)
		pmu->id = pmu_id(name);
	pmu->max_precise = pmu_max_precise(dirfd, pmu);
	pmu->events_table = perf_pmu__find_events_table(pmu);
	pmu_add_sys_aliases(pmu);
	list_add_tail(&pmu->list, pmus);

	pmu->default_config = perf_pmu__get_default_config(pmu);

	return pmu;
err:
	zfree(&pmu->name);
	free(pmu);
	return NULL;
}

/* Creates the PMU when sysfs scanning fails. */
struct perf_pmu *perf_pmu__create_placeholder_core_pmu(struct list_head *core_pmus)
{
	struct perf_pmu *pmu = zalloc(sizeof(*pmu));

	if (!pmu)
		return NULL;

	pmu->name = strdup("cpu");
	if (!pmu->name) {
		free(pmu);
		return NULL;
	}

	pmu->is_core = true;
	pmu->type = PERF_TYPE_RAW;
	pmu->cpus = cpu_map__online();

	INIT_LIST_HEAD(&pmu->format);
	INIT_LIST_HEAD(&pmu->aliases);
	INIT_LIST_HEAD(&pmu->caps);
	list_add_tail(&pmu->list, core_pmus);
	return pmu;
}

void perf_pmu__warn_invalid_formats(struct perf_pmu *pmu)
{
	struct perf_pmu_format *format;

	if (pmu->formats_checked)
		return;

	pmu->formats_checked = true;

	/* fake pmu doesn't have format list */
	if (pmu == &perf_pmu__fake)
		return;

	list_for_each_entry(format, &pmu->format, list) {
		perf_pmu_format__load(pmu, format);
		if (format->value >= PERF_PMU_FORMAT_VALUE_CONFIG_END) {
			pr_warning("WARNING: '%s' format '%s' requires 'perf_event_attr::config%d'"
				   "which is not supported by this version of perf!\n",
				   pmu->name, format->name, format->value);
			return;
		}
	}
}

bool evsel__is_aux_event(const struct evsel *evsel)
{
	struct perf_pmu *pmu = evsel__find_pmu(evsel);

	return pmu && pmu->auxtrace;
}

/*
 * Set @config_name to @val as long as the user hasn't already set or cleared it
 * by passing a config term on the command line.
 *
 * @val is the value to put into the bits specified by @config_name rather than
 * the bit pattern. It is shifted into position by this function, so to set
 * something to true, pass 1 for val rather than a pre shifted value.
 */
#define field_prep(_mask, _val) (((_val) << (ffsll(_mask) - 1)) & (_mask))
void evsel__set_config_if_unset(struct perf_pmu *pmu, struct evsel *evsel,
				const char *config_name, u64 val)
{
	u64 user_bits = 0, bits;
	struct evsel_config_term *term = evsel__get_config_term(evsel, CFG_CHG);

	if (term)
		user_bits = term->val.cfg_chg;

	bits = perf_pmu__format_bits(pmu, config_name);

	/* Do nothing if the user changed the value */
	if (bits & user_bits)
		return;

	/* Otherwise replace it */
	evsel->core.attr.config &= ~bits;
	evsel->core.attr.config |= field_prep(bits, val);
}

static struct perf_pmu_format *
pmu_find_format(struct list_head *formats, const char *name)
{
	struct perf_pmu_format *format;

	list_for_each_entry(format, formats, list)
		if (!strcmp(format->name, name))
			return format;

	return NULL;
}

__u64 perf_pmu__format_bits(struct perf_pmu *pmu, const char *name)
{
	struct perf_pmu_format *format = pmu_find_format(&pmu->format, name);
	__u64 bits = 0;
	int fbit;

	if (!format)
		return 0;

	for_each_set_bit(fbit, format->bits, PERF_PMU_FORMAT_BITS)
		bits |= 1ULL << fbit;

	return bits;
}

int perf_pmu__format_type(struct perf_pmu *pmu, const char *name)
{
	struct perf_pmu_format *format = pmu_find_format(&pmu->format, name);

	if (!format)
		return -1;

	perf_pmu_format__load(pmu, format);
	return format->value;
}

/*
 * Sets value based on the format definition (format parameter)
 * and unformatted value (value parameter).
 */
static void pmu_format_value(unsigned long *format, __u64 value, __u64 *v,
			     bool zero)
{
	unsigned long fbit, vbit;

	for (fbit = 0, vbit = 0; fbit < PERF_PMU_FORMAT_BITS; fbit++) {

		if (!test_bit(fbit, format))
			continue;

		if (value & (1llu << vbit++))
			*v |= (1llu << fbit);
		else if (zero)
			*v &= ~(1llu << fbit);
	}
}

static __u64 pmu_format_max_value(const unsigned long *format)
{
	int w;

	w = bitmap_weight(format, PERF_PMU_FORMAT_BITS);
	if (!w)
		return 0;
	if (w < 64)
		return (1ULL << w) - 1;
	return -1;
}

/*
 * Term is a string term, and might be a param-term. Try to look up it's value
 * in the remaining terms.
 * - We have a term like "base-or-format-term=param-term",
 * - We need to find the value supplied for "param-term" (with param-term named
 *   in a config string) later on in the term list.
 */
static int pmu_resolve_param_term(struct parse_events_term *term,
				  struct list_head *head_terms,
				  __u64 *value)
{
	struct parse_events_term *t;

	list_for_each_entry(t, head_terms, list) {
		if (t->type_val == PARSE_EVENTS__TERM_TYPE_NUM &&
		    t->config && !strcmp(t->config, term->config)) {
			t->used = true;
			*value = t->val.num;
			return 0;
		}
	}

	if (verbose > 0)
		printf("Required parameter '%s' not specified\n", term->config);

	return -1;
}

static char *pmu_formats_string(struct list_head *formats)
{
	struct perf_pmu_format *format;
	char *str = NULL;
	struct strbuf buf = STRBUF_INIT;
	unsigned int i = 0;

	if (!formats)
		return NULL;

	/* sysfs exported terms */
	list_for_each_entry(format, formats, list)
		if (strbuf_addf(&buf, i++ ? ",%s" : "%s", format->name) < 0)
			goto error;

	str = strbuf_detach(&buf, NULL);
error:
	strbuf_release(&buf);

	return str;
}

/*
 * Setup one of config[12] attr members based on the
 * user input data - term parameter.
 */
static int pmu_config_term(struct perf_pmu *pmu,
			   struct perf_event_attr *attr,
			   struct parse_events_term *term,
			   struct list_head *head_terms,
			   bool zero, struct parse_events_error *err)
{
	struct perf_pmu_format *format;
	__u64 *vp;
	__u64 val, max_val;

	/*
	 * If this is a parameter we've already used for parameterized-eval,
	 * skip it in normal eval.
	 */
	if (term->used)
		return 0;

	/*
	 * Hardcoded terms should be already in, so nothing
	 * to be done for them.
	 */
	if (parse_events__is_hardcoded_term(term))
		return 0;

	format = pmu_find_format(&pmu->format, term->config);
	if (!format) {
		char *pmu_term = pmu_formats_string(&pmu->format);
		char *unknown_term;
		char *help_msg;

		if (asprintf(&unknown_term,
				"unknown term '%s' for pmu '%s'",
				term->config, pmu->name) < 0)
			unknown_term = NULL;
		help_msg = parse_events_formats_error_string(pmu_term);
		if (err) {
			parse_events_error__handle(err, term->err_term,
						   unknown_term,
						   help_msg);
		} else {
			pr_debug("%s (%s)\n", unknown_term, help_msg);
			free(unknown_term);
		}
		free(pmu_term);
		return -EINVAL;
	}
	perf_pmu_format__load(pmu, format);
	switch (format->value) {
	case PERF_PMU_FORMAT_VALUE_CONFIG:
		vp = &attr->config;
		break;
	case PERF_PMU_FORMAT_VALUE_CONFIG1:
		vp = &attr->config1;
		break;
	case PERF_PMU_FORMAT_VALUE_CONFIG2:
		vp = &attr->config2;
		break;
	case PERF_PMU_FORMAT_VALUE_CONFIG3:
		vp = &attr->config3;
		break;
	default:
		return -EINVAL;
	}

	/*
	 * Either directly use a numeric term, or try to translate string terms
	 * using event parameters.
	 */
	if (term->type_val == PARSE_EVENTS__TERM_TYPE_NUM) {
		if (term->no_value &&
		    bitmap_weight(format->bits, PERF_PMU_FORMAT_BITS) > 1) {
			if (err) {
				parse_events_error__handle(err, term->err_val,
					   strdup("no value assigned for term"),
					   NULL);
			}
			return -EINVAL;
		}

		val = term->val.num;
	} else if (term->type_val == PARSE_EVENTS__TERM_TYPE_STR) {
		if (strcmp(term->val.str, "?")) {
			if (verbose > 0) {
				pr_info("Invalid sysfs entry %s=%s\n",
						term->config, term->val.str);
			}
			if (err) {
				parse_events_error__handle(err, term->err_val,
					strdup("expected numeric value"),
					NULL);
			}
			return -EINVAL;
		}

		if (pmu_resolve_param_term(term, head_terms, &val))
			return -EINVAL;
	} else
		return -EINVAL;

	max_val = pmu_format_max_value(format->bits);
	if (val > max_val) {
		if (err) {
			char *err_str;

			parse_events_error__handle(err, term->err_val,
				asprintf(&err_str,
				    "value too big for format, maximum is %llu",
				    (unsigned long long)max_val) < 0
				    ? strdup("value too big for format")
				    : err_str,
				    NULL);
			return -EINVAL;
		}
		/*
		 * Assume we don't care if !err, in which case the value will be
		 * silently truncated.
		 */
	}

	pmu_format_value(format->bits, val, vp, zero);
	return 0;
}

int perf_pmu__config_terms(struct perf_pmu *pmu,
			   struct perf_event_attr *attr,
			   struct list_head *head_terms,
			   bool zero, struct parse_events_error *err)
{
	struct parse_events_term *term;

	list_for_each_entry(term, head_terms, list) {
		if (pmu_config_term(pmu, attr, term, head_terms, zero, err))
			return -EINVAL;
	}

	return 0;
}

/*
 * Configures event's 'attr' parameter based on the:
 * 1) users input - specified in terms parameter
 * 2) pmu format definitions - specified by pmu parameter
 */
int perf_pmu__config(struct perf_pmu *pmu, struct perf_event_attr *attr,
		     struct list_head *head_terms,
		     struct parse_events_error *err)
{
	bool zero = !!pmu->default_config;

	return perf_pmu__config_terms(pmu, attr, head_terms, zero, err);
}

static struct perf_pmu_alias *pmu_find_alias(struct perf_pmu *pmu,
					     struct parse_events_term *term)
{
	struct perf_pmu_alias *alias;
	const char *name;

	if (parse_events__is_hardcoded_term(term))
		return NULL;

	if (term->type_val == PARSE_EVENTS__TERM_TYPE_NUM) {
		if (!term->no_value)
			return NULL;
		if (pmu_find_format(&pmu->format, term->config))
			return NULL;
		name = term->config;

	} else if (term->type_val == PARSE_EVENTS__TERM_TYPE_STR) {
		if (strcasecmp(term->config, "event"))
			return NULL;
		name = term->val.str;
	} else {
		return NULL;
	}

	alias = perf_pmu__find_alias(pmu, name, /*load=*/ true);
	if (alias || pmu->cpu_aliases_added)
		return alias;

	/* Alias doesn't exist, try to get it from the json events. */
	if (pmu->events_table &&
	    pmu_events_table__find_event(pmu->events_table, pmu, name,
				         pmu_add_cpu_aliases_map_callback,
				         pmu) == 0) {
		alias = perf_pmu__find_alias(pmu, name, /*load=*/ false);
	}
	return alias;
}


static int check_info_data(struct perf_pmu *pmu,
			   struct perf_pmu_alias *alias,
			   struct perf_pmu_info *info,
			   struct parse_events_error *err,
			   int column)
{
	read_alias_info(pmu, alias);
	/*
	 * Only one term in event definition can
	 * define unit, scale and snapshot, fail
	 * if there's more than one.
	 */
	if (info->unit && alias->unit[0]) {
		parse_events_error__handle(err, column,
					strdup("Attempt to set event's unit twice"),
					NULL);
		return -EINVAL;
	}
	if (info->scale && alias->scale) {
		parse_events_error__handle(err, column,
					strdup("Attempt to set event's scale twice"),
					NULL);
		return -EINVAL;
	}
	if (info->snapshot && alias->snapshot) {
		parse_events_error__handle(err, column,
					strdup("Attempt to set event snapshot twice"),
					NULL);
		return -EINVAL;
	}

	if (alias->unit[0])
		info->unit = alias->unit;

	if (alias->scale)
		info->scale = alias->scale;

	if (alias->snapshot)
		info->snapshot = alias->snapshot;

	return 0;
}

/*
 * Find alias in the terms list and replace it with the terms
 * defined for the alias
 */
int perf_pmu__check_alias(struct perf_pmu *pmu, struct list_head *head_terms,
			  struct perf_pmu_info *info, struct parse_events_error *err)
{
	struct parse_events_term *term, *h;
	struct perf_pmu_alias *alias;
	int ret;

	info->per_pkg = false;

	/*
	 * Mark unit and scale as not set
	 * (different from default values, see below)
	 */
	info->unit     = NULL;
	info->scale    = 0.0;
	info->snapshot = false;

	list_for_each_entry_safe(term, h, head_terms, list) {
		alias = pmu_find_alias(pmu, term);
		if (!alias)
			continue;
		ret = pmu_alias_terms(alias, &term->list);
		if (ret) {
			parse_events_error__handle(err, term->err_term,
						strdup("Failure to duplicate terms"),
						NULL);
			return ret;
		}

		ret = check_info_data(pmu, alias, info, err, term->err_term);
		if (ret)
			return ret;

		if (alias->per_pkg)
			info->per_pkg = true;

		list_del_init(&term->list);
		parse_events_term__delete(term);
	}

	/*
	 * if no unit or scale found in aliases, then
	 * set defaults as for evsel
	 * unit cannot left to NULL
	 */
	if (info->unit == NULL)
		info->unit   = "";

	if (info->scale == 0.0)
		info->scale  = 1.0;

	return 0;
}

struct find_event_args {
	const char *event;
	void *state;
	pmu_event_callback cb;
};

static int find_event_callback(void *state, struct pmu_event_info *info)
{
	struct find_event_args *args = state;

	if (!strcmp(args->event, info->name))
		return args->cb(args->state, info);

	return 0;
}

int perf_pmu__find_event(struct perf_pmu *pmu, const char *event, void *state, pmu_event_callback cb)
{
	struct find_event_args args = {
		.event = event,
		.state = state,
		.cb = cb,
	};

	/* Sub-optimal, but function is only used by tests. */
	return perf_pmu__for_each_event(pmu, /*skip_duplicate_pmus=*/ false,
					&args, find_event_callback);
}

static void perf_pmu__del_formats(struct list_head *formats)
{
	struct perf_pmu_format *fmt, *tmp;

	list_for_each_entry_safe(fmt, tmp, formats, list) {
		list_del(&fmt->list);
		zfree(&fmt->name);
		free(fmt);
	}
}

bool perf_pmu__has_format(const struct perf_pmu *pmu, const char *name)
{
	struct perf_pmu_format *format;

	list_for_each_entry(format, &pmu->format, list) {
		if (!strcmp(format->name, name))
			return true;
	}
	return false;
}

bool is_pmu_core(const char *name)
{
	return !strcmp(name, "cpu") || !strcmp(name, "cpum_cf") || is_sysfs_pmu_core(name);
}

bool perf_pmu__supports_legacy_cache(const struct perf_pmu *pmu)
{
	return pmu->is_core;
}

bool perf_pmu__auto_merge_stats(const struct perf_pmu *pmu)
{
	return !pmu->is_core || perf_pmus__num_core_pmus() == 1;
}

bool perf_pmu__have_event(struct perf_pmu *pmu, const char *name)
{
	if (perf_pmu__find_alias(pmu, name, /*load=*/ true) != NULL)
		return true;
	if (pmu->cpu_aliases_added || !pmu->events_table)
		return false;
	return pmu_events_table__find_event(pmu->events_table, pmu, name, NULL, NULL) == 0;
}

size_t perf_pmu__num_events(struct perf_pmu *pmu)
{
	size_t nr;

	if (!pmu->sysfs_aliases_loaded)
		pmu_aliases_parse(pmu);

	nr = pmu->sysfs_aliases;

	if (pmu->cpu_aliases_added)
		 nr += pmu->loaded_json_aliases;
	else if (pmu->events_table)
		nr += pmu_events_table__num_events(pmu->events_table, pmu) - pmu->loaded_json_aliases;

	return pmu->selectable ? nr + 1 : nr;
}

static int sub_non_neg(int a, int b)
{
	if (b > a)
		return 0;
	return a - b;
}

static char *format_alias(char *buf, int len, const struct perf_pmu *pmu,
			  const struct perf_pmu_alias *alias, bool skip_duplicate_pmus)
{
	struct parse_events_term *term;
	int pmu_name_len = skip_duplicate_pmus
		? pmu_name_len_no_suffix(pmu->name, /*num=*/NULL)
		: (int)strlen(pmu->name);
	int used = snprintf(buf, len, "%.*s/%s", pmu_name_len, pmu->name, alias->name);

	list_for_each_entry(term, &alias->terms, list) {
		if (term->type_val == PARSE_EVENTS__TERM_TYPE_STR)
			used += snprintf(buf + used, sub_non_neg(len, used),
					",%s=%s", term->config,
					term->val.str);
	}

	if (sub_non_neg(len, used) > 0) {
		buf[used] = '/';
		used++;
	}
	if (sub_non_neg(len, used) > 0) {
		buf[used] = '\0';
		used++;
	} else
		buf[len - 1] = '\0';

	return buf;
}

int perf_pmu__for_each_event(struct perf_pmu *pmu, bool skip_duplicate_pmus,
			     void *state, pmu_event_callback cb)
{
	char buf[1024];
	struct perf_pmu_alias *event;
	struct pmu_event_info info = {
		.pmu = pmu,
	};
	int ret = 0;
	struct strbuf sb;

	strbuf_init(&sb, /*hint=*/ 0);
	pmu_add_cpu_aliases(pmu);
	list_for_each_entry(event, &pmu->aliases, list) {
		size_t buf_used;

		info.pmu_name = event->pmu_name ?: pmu->name;
		info.alias = NULL;
		if (event->desc) {
			info.name = event->name;
			buf_used = 0;
		} else {
			info.name = format_alias(buf, sizeof(buf), pmu, event,
						 skip_duplicate_pmus);
			if (pmu->is_core) {
				info.alias = info.name;
				info.name = event->name;
			}
			buf_used = strlen(buf) + 1;
		}
		info.scale_unit = NULL;
		if (strlen(event->unit) || event->scale != 1.0) {
			info.scale_unit = buf + buf_used;
			buf_used += snprintf(buf + buf_used, sizeof(buf) - buf_used,
					"%G%s", event->scale, event->unit) + 1;
		}
		info.desc = event->desc;
		info.long_desc = event->long_desc;
		info.encoding_desc = buf + buf_used;
		parse_events_term__to_strbuf(&event->terms, &sb);
		buf_used += snprintf(buf + buf_used, sizeof(buf) - buf_used,
				"%s/%s/", info.pmu_name, sb.buf) + 1;
		info.topic = event->topic;
		info.str = sb.buf;
		info.deprecated = event->deprecated;
		ret = cb(state, &info);
		if (ret)
			goto out;
		strbuf_setlen(&sb, /*len=*/ 0);
	}
	if (pmu->selectable) {
		info.name = buf;
		snprintf(buf, sizeof(buf), "%s//", pmu->name);
		info.alias = NULL;
		info.scale_unit = NULL;
		info.desc = NULL;
		info.long_desc = NULL;
		info.encoding_desc = NULL;
		info.topic = NULL;
		info.pmu_name = pmu->name;
		info.deprecated = false;
		ret = cb(state, &info);
	}
out:
	strbuf_release(&sb);
	return ret;
}

bool pmu__name_match(const struct perf_pmu *pmu, const char *pmu_name)
{
	return !strcmp(pmu->name, pmu_name) ||
		(pmu->is_uncore && pmu_uncore_alias_match(pmu_name, pmu->name)) ||
		/*
		 * jevents and tests use default_core as a marker for any core
		 * PMU as the PMU name varies across architectures.
		 */
	        (pmu->is_core && !strcmp(pmu_name, "default_core"));
}

bool perf_pmu__is_software(const struct perf_pmu *pmu)
{
	if (pmu->is_core || pmu->is_uncore || pmu->auxtrace)
		return false;
	switch (pmu->type) {
	case PERF_TYPE_HARDWARE:	return false;
	case PERF_TYPE_SOFTWARE:	return true;
	case PERF_TYPE_TRACEPOINT:	return true;
	case PERF_TYPE_HW_CACHE:	return false;
	case PERF_TYPE_RAW:		return false;
	case PERF_TYPE_BREAKPOINT:	return true;
	default: break;
	}
	return !strcmp(pmu->name, "kprobe") || !strcmp(pmu->name, "uprobe");
}

FILE *perf_pmu__open_file(struct perf_pmu *pmu, const char *name)
{
	char path[PATH_MAX];

	if (!perf_pmu__pathname_scnprintf(path, sizeof(path), pmu->name, name) ||
	    !file_available(path))
		return NULL;

	return fopen(path, "r");
}

FILE *perf_pmu__open_file_at(struct perf_pmu *pmu, int dirfd, const char *name)
{
	int fd;

	fd = perf_pmu__pathname_fd(dirfd, pmu->name, name, O_RDONLY);
	if (fd < 0)
		return NULL;

	return fdopen(fd, "r");
}

int perf_pmu__scan_file(struct perf_pmu *pmu, const char *name, const char *fmt,
			...)
{
	va_list args;
	FILE *file;
	int ret = EOF;

	va_start(args, fmt);
	file = perf_pmu__open_file(pmu, name);
	if (file) {
		ret = vfscanf(file, fmt, args);
		fclose(file);
	}
	va_end(args);
	return ret;
}

int perf_pmu__scan_file_at(struct perf_pmu *pmu, int dirfd, const char *name,
			   const char *fmt, ...)
{
	va_list args;
	FILE *file;
	int ret = EOF;

	va_start(args, fmt);
	file = perf_pmu__open_file_at(pmu, dirfd, name);
	if (file) {
		ret = vfscanf(file, fmt, args);
		fclose(file);
	}
	va_end(args);
	return ret;
}

bool perf_pmu__file_exists(struct perf_pmu *pmu, const char *name)
{
	char path[PATH_MAX];

	if (!perf_pmu__pathname_scnprintf(path, sizeof(path), pmu->name, name))
		return false;

	return file_available(path);
}

static int perf_pmu__new_caps(struct list_head *list, char *name, char *value)
{
	struct perf_pmu_caps *caps = zalloc(sizeof(*caps));

	if (!caps)
		return -ENOMEM;

	caps->name = strdup(name);
	if (!caps->name)
		goto free_caps;
	caps->value = strndup(value, strlen(value) - 1);
	if (!caps->value)
		goto free_name;
	list_add_tail(&caps->list, list);
	return 0;

free_name:
	zfree(&caps->name);
free_caps:
	free(caps);

	return -ENOMEM;
}

static void perf_pmu__del_caps(struct perf_pmu *pmu)
{
	struct perf_pmu_caps *caps, *tmp;

	list_for_each_entry_safe(caps, tmp, &pmu->caps, list) {
		list_del(&caps->list);
		zfree(&caps->name);
		zfree(&caps->value);
		free(caps);
	}
}

/*
 * Reading/parsing the given pmu capabilities, which should be located at:
 * /sys/bus/event_source/devices/<dev>/caps as sysfs group attributes.
 * Return the number of capabilities
 */
int perf_pmu__caps_parse(struct perf_pmu *pmu)
{
	struct stat st;
	char caps_path[PATH_MAX];
	DIR *caps_dir;
	struct dirent *evt_ent;
	int caps_fd;

	if (pmu->caps_initialized)
		return pmu->nr_caps;

	pmu->nr_caps = 0;

	if (!perf_pmu__pathname_scnprintf(caps_path, sizeof(caps_path), pmu->name, "caps"))
		return -1;

	if (stat(caps_path, &st) < 0) {
		pmu->caps_initialized = true;
		return 0;	/* no error if caps does not exist */
	}

	caps_dir = opendir(caps_path);
	if (!caps_dir)
		return -EINVAL;

	caps_fd = dirfd(caps_dir);

	while ((evt_ent = readdir(caps_dir)) != NULL) {
		char *name = evt_ent->d_name;
		char value[128];
		FILE *file;
		int fd;

		if (!strcmp(name, ".") || !strcmp(name, ".."))
			continue;

		fd = openat(caps_fd, name, O_RDONLY);
		if (fd == -1)
			continue;
		file = fdopen(fd, "r");
		if (!file) {
			close(fd);
			continue;
		}

		if (!fgets(value, sizeof(value), file) ||
		    (perf_pmu__new_caps(&pmu->caps, name, value) < 0)) {
			fclose(file);
			continue;
		}

		pmu->nr_caps++;
		fclose(file);
	}

	closedir(caps_dir);

	pmu->caps_initialized = true;
	return pmu->nr_caps;
}

static void perf_pmu__compute_config_masks(struct perf_pmu *pmu)
{
	struct perf_pmu_format *format;

	if (pmu->config_masks_computed)
		return;

	list_for_each_entry(format, &pmu->format, list)	{
		unsigned int i;
		__u64 *mask;

		if (format->value >= PERF_PMU_FORMAT_VALUE_CONFIG_END)
			continue;

		pmu->config_masks_present = true;
		mask = &pmu->config_masks[format->value];

		for_each_set_bit(i, format->bits, PERF_PMU_FORMAT_BITS)
			*mask |= 1ULL << i;
	}
	pmu->config_masks_computed = true;
}

void perf_pmu__warn_invalid_config(struct perf_pmu *pmu, __u64 config,
				   const char *name, int config_num,
				   const char *config_name)
{
	__u64 bits;
	char buf[100];

	perf_pmu__compute_config_masks(pmu);

	/*
	 * Kernel doesn't export any valid format bits.
	 */
	if (!pmu->config_masks_present)
		return;

	bits = config & ~pmu->config_masks[config_num];
	if (bits == 0)
		return;

	bitmap_scnprintf((unsigned long *)&bits, sizeof(bits) * 8, buf, sizeof(buf));

	pr_warning("WARNING: event '%s' not valid (bits %s of %s "
		   "'%llx' not supported by kernel)!\n",
		   name ?: "N/A", buf, config_name, config);
}

int perf_pmu__match(const char *pattern, const char *name, const char *tok)
{
	if (!name)
		return -1;

	if (fnmatch(pattern, name, 0))
		return -1;

	if (tok && !perf_pmu__match_ignoring_suffix(name, tok))
		return -1;

	return 0;
}

double __weak perf_pmu__cpu_slots_per_cycle(void)
{
	return NAN;
}

int perf_pmu__event_source_devices_scnprintf(char *pathname, size_t size)
{
	const char *sysfs = sysfs__mountpoint();

	if (!sysfs)
		return 0;
	return scnprintf(pathname, size, "%s/bus/event_source/devices/", sysfs);
}

int perf_pmu__event_source_devices_fd(void)
{
	char path[PATH_MAX];
	const char *sysfs = sysfs__mountpoint();

	if (!sysfs)
		return -1;

	scnprintf(path, sizeof(path), "%s/bus/event_source/devices/", sysfs);
	return open(path, O_DIRECTORY);
}

/*
 * Fill 'buf' with the path to a file or folder in 'pmu_name' in
 * sysfs. For example if pmu_name = "cs_etm" and 'filename' = "format"
 * then pathname will be filled with
 * "/sys/bus/event_source/devices/cs_etm/format"
 *
 * Return 0 if the sysfs mountpoint couldn't be found, if no characters were
 * written or if the buffer size is exceeded.
 */
int perf_pmu__pathname_scnprintf(char *buf, size_t size,
				 const char *pmu_name, const char *filename)
{
	size_t len;

	len = perf_pmu__event_source_devices_scnprintf(buf, size);
	if (!len || (len + strlen(pmu_name) + strlen(filename) + 1)  >= size)
		return 0;

	return scnprintf(buf + len, size - len, "%s/%s", pmu_name, filename);
}

int perf_pmu__pathname_fd(int dirfd, const char *pmu_name, const char *filename, int flags)
{
	char path[PATH_MAX];

	scnprintf(path, sizeof(path), "%s/%s", pmu_name, filename);
	return openat(dirfd, path, flags);
}

void perf_pmu__delete(struct perf_pmu *pmu)
{
	perf_pmu__del_formats(&pmu->format);
	perf_pmu__del_aliases(pmu);
	perf_pmu__del_caps(pmu);

	perf_cpu_map__put(pmu->cpus);

	zfree(&pmu->default_config);
	zfree(&pmu->name);
	zfree(&pmu->alias_name);
	zfree(&pmu->id);
	free(pmu);
}

struct perf_pmu *pmu__find_core_pmu(void)
{
	struct perf_pmu *pmu = NULL;

	while ((pmu = perf_pmus__scan_core(pmu))) {
		/*
		 * The cpumap should cover all CPUs. Otherwise, some CPUs may
		 * not support some events or have different event IDs.
		 */
		if (RC_CHK_ACCESS(pmu->cpus)->nr != cpu__max_cpu().cpu)
			return NULL;

		return pmu;
	}
	return NULL;
}
