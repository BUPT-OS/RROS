/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_EVSEL_H
#define __PERF_EVSEL_H 1

#include <linux/list.h>
#include <stdbool.h>
#include <sys/types.h>
#include <linux/perf_event.h>
#include <linux/types.h>
#include <internal/evsel.h>
#include <perf/evsel.h>
#include "symbol_conf.h"
#include "pmus.h"

struct bpf_object;
struct cgroup;
struct perf_counts;
struct perf_stat_evsel;
union perf_event;
struct bpf_counter_ops;
struct target;
struct hashmap;
struct bperf_leader_bpf;
struct bperf_follower_bpf;
struct perf_pmu;

typedef int (evsel__sb_cb_t)(union perf_event *event, void *data);

enum perf_tool_event {
	PERF_TOOL_NONE		= 0,
	PERF_TOOL_DURATION_TIME = 1,
	PERF_TOOL_USER_TIME = 2,
	PERF_TOOL_SYSTEM_TIME = 3,

	PERF_TOOL_MAX,
};

const char *perf_tool_event__to_str(enum perf_tool_event ev);
enum perf_tool_event perf_tool_event__from_str(const char *str);

#define perf_tool_event__for_each_event(ev)		\
	for ((ev) = PERF_TOOL_DURATION_TIME; (ev) < PERF_TOOL_MAX; ev++)

/** struct evsel - event selector
 *
 * @evlist - evlist this evsel is in, if it is in one.
 * @core - libperf evsel object
 * @name - Can be set to retain the original event name passed by the user,
 *         so that when showing results in tools such as 'perf stat', we
 *         show the name used, not some alias.
 * @id_pos: the position of the event id (PERF_SAMPLE_ID or
 *          PERF_SAMPLE_IDENTIFIER) in a sample event i.e. in the array of
 *          struct perf_record_sample
 * @is_pos: the position (counting backwards) of the event id (PERF_SAMPLE_ID or
 *          PERF_SAMPLE_IDENTIFIER) in a non-sample event i.e. if sample_id_all
 *          is used there is an id sample appended to non-sample events
 * @priv:   And what is in its containing unnamed union are tool specific
 */
struct evsel {
	struct perf_evsel	core;
	struct evlist		*evlist;
	off_t			id_offset;
	int			id_pos;
	int			is_pos;
	unsigned int		sample_size;

	/*
	 * These fields can be set in the parse-events code or similar.
	 * Please check evsel__clone() to copy them properly so that
	 * they can be released properly.
	 */
	struct {
		char			*name;
		char			*group_name;
		const char		*pmu_name;
		const char		*group_pmu_name;
#ifdef HAVE_LIBTRACEEVENT
		struct tep_event	*tp_format;
#endif
		char			*filter;
		unsigned long		max_events;
		double			scale;
		const char		*unit;
		struct cgroup		*cgrp;
		const char		*metric_id;
		enum perf_tool_event	tool_event;
		/* parse modifier helper */
		int			exclude_GH;
		int			sample_read;
		bool			snapshot;
		bool			per_pkg;
		bool			percore;
		bool			precise_max;
		bool			is_libpfm_event;
		bool			auto_merge_stats;
		bool			collect_stat;
		bool			weak_group;
		bool			bpf_counter;
		bool			use_config_name;
		bool			skippable;
		int			bpf_fd;
		struct bpf_object	*bpf_obj;
		struct list_head	config_terms;
	};

	/*
	 * metric fields are similar, but needs more care as they can have
	 * references to other metric (evsel).
	 */
	struct evsel		**metric_events;
	struct evsel		*metric_leader;

	void			*handler;
	struct perf_counts	*counts;
	struct perf_counts	*prev_raw_counts;
	unsigned long		nr_events_printed;
	struct perf_stat_evsel  *stats;
	void			*priv;
	u64			db_id;
	bool			uniquified_name;
	bool 			supported;
	bool 			needs_swap;
	bool 			disabled;
	bool			no_aux_samples;
	bool			immediate;
	bool			tracking;
	bool			ignore_missing_thread;
	bool			forced_leader;
	bool			cmdline_group_boundary;
	bool			merged_stat;
	bool			reset_group;
	bool			errored;
	bool			needs_auxtrace_mmap;
	bool			default_metricgroup; /* A member of the Default metricgroup */
	struct hashmap		*per_pkg_mask;
	int			err;
	struct {
		evsel__sb_cb_t	*cb;
		void		*data;
	} side_band;
	/*
	 * For reporting purposes, an evsel sample can have a callchain
	 * synthesized from AUX area data. Keep track of synthesized sample
	 * types here. Note, the recorded sample_type cannot be changed because
	 * it is needed to continue to parse events.
	 * See also evsel__has_callchain().
	 */
	__u64			synth_sample_type;

	/*
	 * bpf_counter_ops serves two use cases:
	 *   1. perf-stat -b          counting events used byBPF programs
	 *   2. perf-stat --use-bpf   use BPF programs to aggregate counts
	 */
	struct bpf_counter_ops	*bpf_counter_ops;

	struct list_head	bpf_counter_list; /* for perf-stat -b */
	struct list_head	bpf_filters; /* for perf-record --filter */

	/* for perf-stat --use-bpf */
	int			bperf_leader_prog_fd;
	int			bperf_leader_link_fd;
	union {
		struct bperf_leader_bpf *leader_skel;
		struct bperf_follower_bpf *follower_skel;
		void *bpf_skel;
	};
	unsigned long		open_flags;
	int			precise_ip_original;

	/* for missing_features */
	struct perf_pmu		*pmu;
};

struct perf_missing_features {
	bool sample_id_all;
	bool exclude_guest;
	bool mmap2;
	bool cloexec;
	bool clockid;
	bool clockid_wrong;
	bool lbr_flags;
	bool write_backward;
	bool group_read;
	bool ksymbol;
	bool bpf;
	bool aux_output;
	bool branch_hw_idx;
	bool cgroup;
	bool data_page_size;
	bool code_page_size;
	bool weight_struct;
	bool read_lost;
};

extern struct perf_missing_features perf_missing_features;

struct perf_cpu_map;
struct thread_map;
struct record_opts;

static inline struct perf_cpu_map *evsel__cpus(struct evsel *evsel)
{
	return perf_evsel__cpus(&evsel->core);
}

static inline int evsel__nr_cpus(struct evsel *evsel)
{
	return perf_cpu_map__nr(evsel__cpus(evsel));
}

void evsel__compute_deltas(struct evsel *evsel, int cpu, int thread,
			   struct perf_counts_values *count);

int evsel__object_config(size_t object_size,
			 int (*init)(struct evsel *evsel),
			 void (*fini)(struct evsel *evsel));

struct perf_pmu *evsel__find_pmu(const struct evsel *evsel);
bool evsel__is_aux_event(const struct evsel *evsel);

struct evsel *evsel__new_idx(struct perf_event_attr *attr, int idx);

static inline struct evsel *evsel__new(struct perf_event_attr *attr)
{
	return evsel__new_idx(attr, 0);
}

struct evsel *evsel__clone(struct evsel *orig);

int copy_config_terms(struct list_head *dst, struct list_head *src);
void free_config_terms(struct list_head *config_terms);


#ifdef HAVE_LIBTRACEEVENT
struct evsel *evsel__newtp_idx(const char *sys, const char *name, int idx);

/*
 * Returns pointer with encoded error via <linux/err.h> interface.
 */
static inline struct evsel *evsel__newtp(const char *sys, const char *name)
{
	return evsel__newtp_idx(sys, name, 0);
}
#endif

#ifdef HAVE_LIBTRACEEVENT
struct tep_event *event_format__new(const char *sys, const char *name);
#endif

void evsel__init(struct evsel *evsel, struct perf_event_attr *attr, int idx);
void evsel__exit(struct evsel *evsel);
void evsel__delete(struct evsel *evsel);

struct callchain_param;

void evsel__config(struct evsel *evsel, struct record_opts *opts,
		   struct callchain_param *callchain);
void evsel__config_callchain(struct evsel *evsel, struct record_opts *opts,
			     struct callchain_param *callchain);

int __evsel__sample_size(u64 sample_type);
void evsel__calc_id_pos(struct evsel *evsel);

bool evsel__is_cache_op_valid(u8 type, u8 op);

static inline bool evsel__is_bpf(struct evsel *evsel)
{
	return evsel->bpf_counter_ops != NULL;
}

static inline bool evsel__is_bperf(struct evsel *evsel)
{
	return evsel->bpf_counter_ops != NULL && list_empty(&evsel->bpf_counter_list);
}

#define EVSEL__MAX_ALIASES 8

extern const char *const evsel__hw_cache[PERF_COUNT_HW_CACHE_MAX][EVSEL__MAX_ALIASES];
extern const char *const evsel__hw_cache_op[PERF_COUNT_HW_CACHE_OP_MAX][EVSEL__MAX_ALIASES];
extern const char *const evsel__hw_cache_result[PERF_COUNT_HW_CACHE_RESULT_MAX][EVSEL__MAX_ALIASES];
extern const char *const evsel__hw_names[PERF_COUNT_HW_MAX];
extern const char *const evsel__sw_names[PERF_COUNT_SW_MAX];
extern char *evsel__bpf_counter_events;
bool evsel__match_bpf_counter_events(const char *name);
int arch_evsel__hw_name(struct evsel *evsel, char *bf, size_t size);

int __evsel__hw_cache_type_op_res_name(u8 type, u8 op, u8 result, char *bf, size_t size);
const char *evsel__name(struct evsel *evsel);
bool evsel__name_is(struct evsel *evsel, const char *name);
const char *evsel__metric_id(const struct evsel *evsel);

static inline bool evsel__is_tool(const struct evsel *evsel)
{
	return evsel->tool_event != PERF_TOOL_NONE;
}

const char *evsel__group_name(struct evsel *evsel);
int evsel__group_desc(struct evsel *evsel, char *buf, size_t size);

void __evsel__set_sample_bit(struct evsel *evsel, enum perf_event_sample_format bit);
void __evsel__reset_sample_bit(struct evsel *evsel, enum perf_event_sample_format bit);

#define evsel__set_sample_bit(evsel, bit) \
	__evsel__set_sample_bit(evsel, PERF_SAMPLE_##bit)

#define evsel__reset_sample_bit(evsel, bit) \
	__evsel__reset_sample_bit(evsel, PERF_SAMPLE_##bit)

void evsel__set_sample_id(struct evsel *evsel, bool use_sample_identifier);

void arch_evsel__set_sample_weight(struct evsel *evsel);
void arch__post_evsel_config(struct evsel *evsel, struct perf_event_attr *attr);
int arch_evsel__open_strerror(struct evsel *evsel, char *msg, size_t size);

int evsel__set_filter(struct evsel *evsel, const char *filter);
int evsel__append_tp_filter(struct evsel *evsel, const char *filter);
int evsel__append_addr_filter(struct evsel *evsel, const char *filter);
int evsel__enable_cpu(struct evsel *evsel, int cpu_map_idx);
int evsel__enable(struct evsel *evsel);
int evsel__disable(struct evsel *evsel);
int evsel__disable_cpu(struct evsel *evsel, int cpu_map_idx);

int evsel__open_per_cpu(struct evsel *evsel, struct perf_cpu_map *cpus, int cpu_map_idx);
int evsel__open_per_thread(struct evsel *evsel, struct perf_thread_map *threads);
int evsel__open(struct evsel *evsel, struct perf_cpu_map *cpus,
		struct perf_thread_map *threads);
void evsel__close(struct evsel *evsel);
int evsel__prepare_open(struct evsel *evsel, struct perf_cpu_map *cpus,
		struct perf_thread_map *threads);
bool evsel__detect_missing_features(struct evsel *evsel);

enum rlimit_action { NO_CHANGE, SET_TO_MAX, INCREASED_MAX };
bool evsel__increase_rlimit(enum rlimit_action *set_rlimit);

bool evsel__precise_ip_fallback(struct evsel *evsel);

struct perf_sample;

#ifdef HAVE_LIBTRACEEVENT
void *evsel__rawptr(struct evsel *evsel, struct perf_sample *sample, const char *name);
u64 evsel__intval(struct evsel *evsel, struct perf_sample *sample, const char *name);

static inline char *evsel__strval(struct evsel *evsel, struct perf_sample *sample, const char *name)
{
	return evsel__rawptr(evsel, sample, name);
}
#endif

struct tep_format_field;

u64 format_field__intval(struct tep_format_field *field, struct perf_sample *sample, bool needs_swap);

struct tep_format_field *evsel__field(struct evsel *evsel, const char *name);

static inline bool __evsel__match(const struct evsel *evsel, u32 type, u64 config)
{
	if (evsel->core.attr.type != type)
		return false;

	if ((type == PERF_TYPE_HARDWARE || type == PERF_TYPE_HW_CACHE)  &&
	    perf_pmus__supports_extended_type())
		return (evsel->core.attr.config & PERF_HW_EVENT_MASK) == config;

	return evsel->core.attr.config == config;
}

#define evsel__match(evsel, t, c) __evsel__match(evsel, PERF_TYPE_##t, PERF_COUNT_##c)

static inline bool evsel__match2(struct evsel *e1, struct evsel *e2)
{
	return (e1->core.attr.type == e2->core.attr.type) &&
	       (e1->core.attr.config == e2->core.attr.config);
}

int evsel__read_counter(struct evsel *evsel, int cpu_map_idx, int thread);

int __evsel__read_on_cpu(struct evsel *evsel, int cpu_map_idx, int thread, bool scale);

/**
 * evsel__read_on_cpu - Read out the results on a CPU and thread
 *
 * @evsel - event selector to read value
 * @cpu_map_idx - CPU of interest
 * @thread - thread of interest
 */
static inline int evsel__read_on_cpu(struct evsel *evsel, int cpu_map_idx, int thread)
{
	return __evsel__read_on_cpu(evsel, cpu_map_idx, thread, false);
}

/**
 * evsel__read_on_cpu_scaled - Read out the results on a CPU and thread, scaled
 *
 * @evsel - event selector to read value
 * @cpu_map_idx - CPU of interest
 * @thread - thread of interest
 */
static inline int evsel__read_on_cpu_scaled(struct evsel *evsel, int cpu_map_idx, int thread)
{
	return __evsel__read_on_cpu(evsel, cpu_map_idx, thread, true);
}

int evsel__parse_sample(struct evsel *evsel, union perf_event *event,
			struct perf_sample *sample);

int evsel__parse_sample_timestamp(struct evsel *evsel, union perf_event *event,
				  u64 *timestamp);

u16 evsel__id_hdr_size(struct evsel *evsel);

static inline struct evsel *evsel__next(struct evsel *evsel)
{
	return list_entry(evsel->core.node.next, struct evsel, core.node);
}

static inline struct evsel *evsel__prev(struct evsel *evsel)
{
	return list_entry(evsel->core.node.prev, struct evsel, core.node);
}

/**
 * evsel__is_group_leader - Return whether given evsel is a leader event
 *
 * @evsel - evsel selector to be tested
 *
 * Return %true if @evsel is a group leader or a stand-alone event
 */
static inline bool evsel__is_group_leader(const struct evsel *evsel)
{
	return evsel->core.leader == &evsel->core;
}

/**
 * evsel__is_group_event - Return whether given evsel is a group event
 *
 * @evsel - evsel selector to be tested
 *
 * Return %true iff event group view is enabled and @evsel is a actual group
 * leader which has other members in the group
 */
static inline bool evsel__is_group_event(struct evsel *evsel)
{
	if (!symbol_conf.event_group)
		return false;

	return evsel__is_group_leader(evsel) && evsel->core.nr_members > 1;
}

bool evsel__is_function_event(struct evsel *evsel);

static inline bool evsel__is_bpf_output(struct evsel *evsel)
{
	return evsel__match(evsel, SOFTWARE, SW_BPF_OUTPUT);
}

static inline bool evsel__is_clock(const struct evsel *evsel)
{
	return evsel__match(evsel, SOFTWARE, SW_CPU_CLOCK) ||
	       evsel__match(evsel, SOFTWARE, SW_TASK_CLOCK);
}

bool evsel__fallback(struct evsel *evsel, int err, char *msg, size_t msgsize);
int evsel__open_strerror(struct evsel *evsel, struct target *target,
			 int err, char *msg, size_t size);

static inline int evsel__group_idx(struct evsel *evsel)
{
	return evsel->core.idx - evsel->core.leader->idx;
}

/* Iterates group WITHOUT the leader. */
#define for_each_group_member_head(_evsel, _leader, _head)				\
for ((_evsel) = list_entry((_leader)->core.node.next, struct evsel, core.node);		\
	(_evsel) && &(_evsel)->core.node != (_head) &&					\
	(_evsel)->core.leader == &(_leader)->core;					\
	(_evsel) = list_entry((_evsel)->core.node.next, struct evsel, core.node))

#define for_each_group_member(_evsel, _leader)				\
	for_each_group_member_head(_evsel, _leader, &(_leader)->evlist->core.entries)

/* Iterates group WITH the leader. */
#define for_each_group_evsel_head(_evsel, _leader, _head)				\
for ((_evsel) = _leader;								\
	(_evsel) && &(_evsel)->core.node != (_head) &&					\
	(_evsel)->core.leader == &(_leader)->core;					\
	(_evsel) = list_entry((_evsel)->core.node.next, struct evsel, core.node))

#define for_each_group_evsel(_evsel, _leader)				\
	for_each_group_evsel_head(_evsel, _leader, &(_leader)->evlist->core.entries)

static inline bool evsel__has_branch_callstack(const struct evsel *evsel)
{
	return evsel->core.attr.branch_sample_type & PERF_SAMPLE_BRANCH_CALL_STACK;
}

static inline bool evsel__has_branch_hw_idx(const struct evsel *evsel)
{
	return evsel->core.attr.branch_sample_type & PERF_SAMPLE_BRANCH_HW_INDEX;
}

static inline bool evsel__has_callchain(const struct evsel *evsel)
{
	/*
	 * For reporting purposes, an evsel sample can have a recorded callchain
	 * or a callchain synthesized from AUX area data.
	 */
	return evsel->core.attr.sample_type & PERF_SAMPLE_CALLCHAIN ||
	       evsel->synth_sample_type & PERF_SAMPLE_CALLCHAIN;
}

static inline bool evsel__has_br_stack(const struct evsel *evsel)
{
	/*
	 * For reporting purposes, an evsel sample can have a recorded branch
	 * stack or a branch stack synthesized from AUX area data.
	 */
	return evsel->core.attr.sample_type & PERF_SAMPLE_BRANCH_STACK ||
	       evsel->synth_sample_type & PERF_SAMPLE_BRANCH_STACK;
}

static inline bool evsel__is_dummy_event(struct evsel *evsel)
{
	return (evsel->core.attr.type == PERF_TYPE_SOFTWARE) &&
	       (evsel->core.attr.config == PERF_COUNT_SW_DUMMY);
}

struct perf_env *evsel__env(struct evsel *evsel);

int evsel__store_ids(struct evsel *evsel, struct evlist *evlist);

void evsel__zero_per_pkg(struct evsel *evsel);
bool evsel__is_hybrid(const struct evsel *evsel);
struct evsel *evsel__leader(const struct evsel *evsel);
bool evsel__has_leader(struct evsel *evsel, struct evsel *leader);
bool evsel__is_leader(struct evsel *evsel);
void evsel__set_leader(struct evsel *evsel, struct evsel *leader);
int evsel__source_count(const struct evsel *evsel);
void evsel__remove_from_group(struct evsel *evsel, struct evsel *leader);

bool arch_evsel__must_be_in_group(const struct evsel *evsel);

/*
 * Macro to swap the bit-field postition and size.
 * Used when,
 * - dont need to swap the entire u64 &&
 * - when u64 has variable bit-field sizes &&
 * - when presented in a host endian which is different
 *   than the source endian of the perf.data file
 */
#define bitfield_swap(src, pos, size)	\
	((((src) >> (pos)) & ((1ull << (size)) - 1)) << (63 - ((pos) + (size) - 1)))

u64 evsel__bitfield_swap_branch_flags(u64 value);
void evsel__set_config_if_unset(struct perf_pmu *pmu, struct evsel *evsel,
				const char *config_name, u64 val);

#endif /* __PERF_EVSEL_H */
