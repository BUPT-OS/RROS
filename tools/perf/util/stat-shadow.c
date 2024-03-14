// SPDX-License-Identifier: GPL-2.0
#include <math.h>
#include <stdio.h>
#include "evsel.h"
#include "stat.h"
#include "color.h"
#include "debug.h"
#include "pmu.h"
#include "rblist.h"
#include "evlist.h"
#include "expr.h"
#include "metricgroup.h"
#include "cgroup.h"
#include "units.h"
#include <linux/zalloc.h>
#include "iostat.h"
#include "util/hashmap.h"

struct stats walltime_nsecs_stats;
struct rusage_stats ru_stats;

enum {
	CTX_BIT_USER	= 1 << 0,
	CTX_BIT_KERNEL	= 1 << 1,
	CTX_BIT_HV	= 1 << 2,
	CTX_BIT_HOST	= 1 << 3,
	CTX_BIT_IDLE	= 1 << 4,
	CTX_BIT_MAX	= 1 << 5,
};

enum stat_type {
	STAT_NONE = 0,
	STAT_NSECS,
	STAT_CYCLES,
	STAT_INSTRUCTIONS,
	STAT_STALLED_CYCLES_FRONT,
	STAT_STALLED_CYCLES_BACK,
	STAT_BRANCHES,
	STAT_BRANCH_MISS,
	STAT_CACHE_REFS,
	STAT_CACHE_MISSES,
	STAT_L1_DCACHE,
	STAT_L1_ICACHE,
	STAT_LL_CACHE,
	STAT_ITLB_CACHE,
	STAT_DTLB_CACHE,
	STAT_L1D_MISS,
	STAT_L1I_MISS,
	STAT_LL_MISS,
	STAT_DTLB_MISS,
	STAT_ITLB_MISS,
	STAT_MAX
};

static int evsel_context(const struct evsel *evsel)
{
	int ctx = 0;

	if (evsel->core.attr.exclude_kernel)
		ctx |= CTX_BIT_KERNEL;
	if (evsel->core.attr.exclude_user)
		ctx |= CTX_BIT_USER;
	if (evsel->core.attr.exclude_hv)
		ctx |= CTX_BIT_HV;
	if (evsel->core.attr.exclude_host)
		ctx |= CTX_BIT_HOST;
	if (evsel->core.attr.exclude_idle)
		ctx |= CTX_BIT_IDLE;

	return ctx;
}

void perf_stat__reset_shadow_stats(void)
{
	memset(&walltime_nsecs_stats, 0, sizeof(walltime_nsecs_stats));
	memset(&ru_stats, 0, sizeof(ru_stats));
}

static enum stat_type evsel__stat_type(const struct evsel *evsel)
{
	/* Fake perf_hw_cache_op_id values for use with evsel__match. */
	u64 PERF_COUNT_hw_cache_l1d_miss = PERF_COUNT_HW_CACHE_L1D |
		((PERF_COUNT_HW_CACHE_OP_READ) << 8) |
		((PERF_COUNT_HW_CACHE_RESULT_MISS) << 16);
	u64 PERF_COUNT_hw_cache_l1i_miss = PERF_COUNT_HW_CACHE_L1I |
		((PERF_COUNT_HW_CACHE_OP_READ) << 8) |
		((PERF_COUNT_HW_CACHE_RESULT_MISS) << 16);
	u64 PERF_COUNT_hw_cache_ll_miss = PERF_COUNT_HW_CACHE_LL |
		((PERF_COUNT_HW_CACHE_OP_READ) << 8) |
		((PERF_COUNT_HW_CACHE_RESULT_MISS) << 16);
	u64 PERF_COUNT_hw_cache_dtlb_miss = PERF_COUNT_HW_CACHE_DTLB |
		((PERF_COUNT_HW_CACHE_OP_READ) << 8) |
		((PERF_COUNT_HW_CACHE_RESULT_MISS) << 16);
	u64 PERF_COUNT_hw_cache_itlb_miss = PERF_COUNT_HW_CACHE_ITLB |
		((PERF_COUNT_HW_CACHE_OP_READ) << 8) |
		((PERF_COUNT_HW_CACHE_RESULT_MISS) << 16);

	if (evsel__is_clock(evsel))
		return STAT_NSECS;
	else if (evsel__match(evsel, HARDWARE, HW_CPU_CYCLES))
		return STAT_CYCLES;
	else if (evsel__match(evsel, HARDWARE, HW_INSTRUCTIONS))
		return STAT_INSTRUCTIONS;
	else if (evsel__match(evsel, HARDWARE, HW_STALLED_CYCLES_FRONTEND))
		return STAT_STALLED_CYCLES_FRONT;
	else if (evsel__match(evsel, HARDWARE, HW_STALLED_CYCLES_BACKEND))
		return STAT_STALLED_CYCLES_BACK;
	else if (evsel__match(evsel, HARDWARE, HW_BRANCH_INSTRUCTIONS))
		return STAT_BRANCHES;
	else if (evsel__match(evsel, HARDWARE, HW_BRANCH_MISSES))
		return STAT_BRANCH_MISS;
	else if (evsel__match(evsel, HARDWARE, HW_CACHE_REFERENCES))
		return STAT_CACHE_REFS;
	else if (evsel__match(evsel, HARDWARE, HW_CACHE_MISSES))
		return STAT_CACHE_MISSES;
	else if (evsel__match(evsel, HW_CACHE, HW_CACHE_L1D))
		return STAT_L1_DCACHE;
	else if (evsel__match(evsel, HW_CACHE, HW_CACHE_L1I))
		return STAT_L1_ICACHE;
	else if (evsel__match(evsel, HW_CACHE, HW_CACHE_LL))
		return STAT_LL_CACHE;
	else if (evsel__match(evsel, HW_CACHE, HW_CACHE_DTLB))
		return STAT_DTLB_CACHE;
	else if (evsel__match(evsel, HW_CACHE, HW_CACHE_ITLB))
		return STAT_ITLB_CACHE;
	else if (evsel__match(evsel, HW_CACHE, hw_cache_l1d_miss))
		return STAT_L1D_MISS;
	else if (evsel__match(evsel, HW_CACHE, hw_cache_l1i_miss))
		return STAT_L1I_MISS;
	else if (evsel__match(evsel, HW_CACHE, hw_cache_ll_miss))
		return STAT_LL_MISS;
	else if (evsel__match(evsel, HW_CACHE, hw_cache_dtlb_miss))
		return STAT_DTLB_MISS;
	else if (evsel__match(evsel, HW_CACHE, hw_cache_itlb_miss))
		return STAT_ITLB_MISS;
	return STAT_NONE;
}

static const char *get_ratio_color(const double ratios[3], double val)
{
	const char *color = PERF_COLOR_NORMAL;

	if (val > ratios[0])
		color = PERF_COLOR_RED;
	else if (val > ratios[1])
		color = PERF_COLOR_MAGENTA;
	else if (val > ratios[2])
		color = PERF_COLOR_YELLOW;

	return color;
}

static double find_stat(const struct evsel *evsel, int aggr_idx, enum stat_type type)
{
	const struct evsel *cur;
	int evsel_ctx = evsel_context(evsel);

	evlist__for_each_entry(evsel->evlist, cur) {
		struct perf_stat_aggr *aggr;

		/* Ignore the evsel that is being searched from. */
		if (evsel == cur)
			continue;

		/* Ignore evsels that are part of different groups. */
		if (evsel->core.leader->nr_members > 1 &&
		    evsel->core.leader != cur->core.leader)
			continue;
		/* Ignore evsels with mismatched modifiers. */
		if (evsel_ctx != evsel_context(cur))
			continue;
		/* Ignore if not the cgroup we're looking for. */
		if (evsel->cgrp != cur->cgrp)
			continue;
		/* Ignore if not the stat we're looking for. */
		if (type != evsel__stat_type(cur))
			continue;

		aggr = &cur->stats->aggr[aggr_idx];
		if (type == STAT_NSECS)
			return aggr->counts.val;
		return aggr->counts.val * cur->scale;
	}
	return 0.0;
}

static void print_ratio(struct perf_stat_config *config,
			const struct evsel *evsel, int aggr_idx,
			double numerator, struct perf_stat_output_ctx *out,
			enum stat_type denominator_type,
			const double color_ratios[3], const char *unit)
{
	double denominator = find_stat(evsel, aggr_idx, denominator_type);

	if (numerator && denominator) {
		double ratio = numerator / denominator * 100.0;
		const char *color = get_ratio_color(color_ratios, ratio);

		out->print_metric(config, out->ctx, color, "%7.2f%%", unit, ratio);
	} else
		out->print_metric(config, out->ctx, NULL, NULL, unit, 0);
}

static void print_stalled_cycles_front(struct perf_stat_config *config,
				const struct evsel *evsel,
				int aggr_idx, double stalled,
				struct perf_stat_output_ctx *out)
{
	static const double color_ratios[3] = {50.0, 30.0, 10.0};

	print_ratio(config, evsel, aggr_idx, stalled, out, STAT_CYCLES, color_ratios,
		    "frontend cycles idle");
}

static void print_stalled_cycles_back(struct perf_stat_config *config,
				const struct evsel *evsel,
				int aggr_idx, double stalled,
				struct perf_stat_output_ctx *out)
{
	static const double color_ratios[3] = {75.0, 50.0, 20.0};

	print_ratio(config, evsel, aggr_idx, stalled, out, STAT_CYCLES, color_ratios,
		    "backend cycles idle");
}

static void print_branch_miss(struct perf_stat_config *config,
			const struct evsel *evsel,
			int aggr_idx, double misses,
			struct perf_stat_output_ctx *out)
{
	static const double color_ratios[3] = {20.0, 10.0, 5.0};

	print_ratio(config, evsel, aggr_idx, misses, out, STAT_BRANCHES, color_ratios,
		    "of all branches");
}

static void print_l1d_miss(struct perf_stat_config *config,
			const struct evsel *evsel,
			int aggr_idx, double misses,
			struct perf_stat_output_ctx *out)
{
	static const double color_ratios[3] = {20.0, 10.0, 5.0};

	print_ratio(config, evsel, aggr_idx, misses, out, STAT_L1_DCACHE, color_ratios,
		    "of all L1-dcache accesses");
}

static void print_l1i_miss(struct perf_stat_config *config,
			const struct evsel *evsel,
			int aggr_idx, double misses,
			struct perf_stat_output_ctx *out)
{
	static const double color_ratios[3] = {20.0, 10.0, 5.0};

	print_ratio(config, evsel, aggr_idx, misses, out, STAT_L1_ICACHE, color_ratios,
		    "of all L1-icache accesses");
}

static void print_ll_miss(struct perf_stat_config *config,
			const struct evsel *evsel,
			int aggr_idx, double misses,
			struct perf_stat_output_ctx *out)
{
	static const double color_ratios[3] = {20.0, 10.0, 5.0};

	print_ratio(config, evsel, aggr_idx, misses, out, STAT_LL_CACHE, color_ratios,
		    "of all L1-icache accesses");
}

static void print_dtlb_miss(struct perf_stat_config *config,
			const struct evsel *evsel,
			int aggr_idx, double misses,
			struct perf_stat_output_ctx *out)
{
	static const double color_ratios[3] = {20.0, 10.0, 5.0};

	print_ratio(config, evsel, aggr_idx, misses, out, STAT_DTLB_CACHE, color_ratios,
		    "of all dTLB cache accesses");
}

static void print_itlb_miss(struct perf_stat_config *config,
			const struct evsel *evsel,
			int aggr_idx, double misses,
			struct perf_stat_output_ctx *out)
{
	static const double color_ratios[3] = {20.0, 10.0, 5.0};

	print_ratio(config, evsel, aggr_idx, misses, out, STAT_ITLB_CACHE, color_ratios,
		    "of all iTLB cache accesses");
}

static void print_cache_miss(struct perf_stat_config *config,
			const struct evsel *evsel,
			int aggr_idx, double misses,
			struct perf_stat_output_ctx *out)
{
	static const double color_ratios[3] = {20.0, 10.0, 5.0};

	print_ratio(config, evsel, aggr_idx, misses, out, STAT_CACHE_REFS, color_ratios,
		    "of all cache refs");
}

static void print_instructions(struct perf_stat_config *config,
			const struct evsel *evsel,
			int aggr_idx, double instructions,
			struct perf_stat_output_ctx *out)
{
	print_metric_t print_metric = out->print_metric;
	void *ctxp = out->ctx;
	double cycles = find_stat(evsel, aggr_idx, STAT_CYCLES);
	double max_stalled = max(find_stat(evsel, aggr_idx, STAT_STALLED_CYCLES_FRONT),
				find_stat(evsel, aggr_idx, STAT_STALLED_CYCLES_BACK));

	if (cycles) {
		print_metric(config, ctxp, NULL, "%7.2f ", "insn per cycle",
			instructions / cycles);
	} else
		print_metric(config, ctxp, NULL, NULL, "insn per cycle", 0);

	if (max_stalled && instructions) {
		out->new_line(config, ctxp);
		print_metric(config, ctxp, NULL, "%7.2f ", "stalled cycles per insn",
			max_stalled / instructions);
	}
}

static void print_cycles(struct perf_stat_config *config,
			const struct evsel *evsel,
			int aggr_idx, double cycles,
			struct perf_stat_output_ctx *out)
{
	double nsecs = find_stat(evsel, aggr_idx, STAT_NSECS);

	if (cycles && nsecs) {
		double ratio = cycles / nsecs;

		out->print_metric(config, out->ctx, NULL, "%8.3f", "GHz", ratio);
	} else
		out->print_metric(config, out->ctx, NULL, NULL, "GHz", 0);
}

static void print_nsecs(struct perf_stat_config *config,
			const struct evsel *evsel,
			int aggr_idx __maybe_unused, double nsecs,
			struct perf_stat_output_ctx *out)
{
	print_metric_t print_metric = out->print_metric;
	void *ctxp = out->ctx;
	double wall_time = avg_stats(&walltime_nsecs_stats);

	if (wall_time) {
		print_metric(config, ctxp, NULL, "%8.3f", "CPUs utilized",
			nsecs / (wall_time * evsel->scale));
	} else
		print_metric(config, ctxp, NULL, NULL, "CPUs utilized", 0);
}

static int prepare_metric(struct evsel **metric_events,
			  struct metric_ref *metric_refs,
			  struct expr_parse_ctx *pctx,
			  int aggr_idx)
{
	int i;

	for (i = 0; metric_events[i]; i++) {
		char *n;
		double val;
		int source_count = 0;

		if (evsel__is_tool(metric_events[i])) {
			struct stats *stats;
			double scale;

			switch (metric_events[i]->tool_event) {
			case PERF_TOOL_DURATION_TIME:
				stats = &walltime_nsecs_stats;
				scale = 1e-9;
				break;
			case PERF_TOOL_USER_TIME:
				stats = &ru_stats.ru_utime_usec_stat;
				scale = 1e-6;
				break;
			case PERF_TOOL_SYSTEM_TIME:
				stats = &ru_stats.ru_stime_usec_stat;
				scale = 1e-6;
				break;
			case PERF_TOOL_NONE:
				pr_err("Invalid tool event 'none'");
				abort();
			case PERF_TOOL_MAX:
				pr_err("Invalid tool event 'max'");
				abort();
			default:
				pr_err("Unknown tool event '%s'", evsel__name(metric_events[i]));
				abort();
			}
			val = avg_stats(stats) * scale;
			source_count = 1;
		} else {
			struct perf_stat_evsel *ps = metric_events[i]->stats;
			struct perf_stat_aggr *aggr = &ps->aggr[aggr_idx];

			if (!aggr)
				break;

                        if (!metric_events[i]->supported) {
				/*
				 * Not supported events will have a count of 0,
				 * which can be confusing in a
				 * metric. Explicitly set the value to NAN. Not
				 * counted events (enable time of 0) are read as
				 * 0.
				 */
				val = NAN;
				source_count = 0;
			} else {
				/*
				 * If an event was scaled during stat gathering,
				 * reverse the scale before computing the
				 * metric.
				 */
				val = aggr->counts.val * (1.0 / metric_events[i]->scale);
				source_count = evsel__source_count(metric_events[i]);
			}
		}
		n = strdup(evsel__metric_id(metric_events[i]));
		if (!n)
			return -ENOMEM;

		expr__add_id_val_source_count(pctx, n, val, source_count);
	}

	for (int j = 0; metric_refs && metric_refs[j].metric_name; j++) {
		int ret = expr__add_ref(pctx, &metric_refs[j]);

		if (ret)
			return ret;
	}

	return i;
}

static void generic_metric(struct perf_stat_config *config,
			   const char *metric_expr,
			   const char *metric_threshold,
			   struct evsel **metric_events,
			   struct metric_ref *metric_refs,
			   char *name,
			   const char *metric_name,
			   const char *metric_unit,
			   int runtime,
			   int aggr_idx,
			   struct perf_stat_output_ctx *out)
{
	print_metric_t print_metric = out->print_metric;
	struct expr_parse_ctx *pctx;
	double ratio, scale, threshold;
	int i;
	void *ctxp = out->ctx;
	const char *color = NULL;

	pctx = expr__ctx_new();
	if (!pctx)
		return;

	if (config->user_requested_cpu_list)
		pctx->sctx.user_requested_cpu_list = strdup(config->user_requested_cpu_list);
	pctx->sctx.runtime = runtime;
	pctx->sctx.system_wide = config->system_wide;
	i = prepare_metric(metric_events, metric_refs, pctx, aggr_idx);
	if (i < 0) {
		expr__ctx_free(pctx);
		return;
	}
	if (!metric_events[i]) {
		if (expr__parse(&ratio, pctx, metric_expr) == 0) {
			char *unit;
			char metric_bf[64];

			if (metric_threshold &&
			    expr__parse(&threshold, pctx, metric_threshold) == 0 &&
			    !isnan(threshold)) {
				color = fpclassify(threshold) == FP_ZERO
					? PERF_COLOR_GREEN : PERF_COLOR_RED;
			}

			if (metric_unit && metric_name) {
				if (perf_pmu__convert_scale(metric_unit,
					&unit, &scale) >= 0) {
					ratio *= scale;
				}
				if (strstr(metric_expr, "?"))
					scnprintf(metric_bf, sizeof(metric_bf),
					  "%s  %s_%d", unit, metric_name, runtime);
				else
					scnprintf(metric_bf, sizeof(metric_bf),
					  "%s  %s", unit, metric_name);

				print_metric(config, ctxp, color, "%8.1f",
					     metric_bf, ratio);
			} else {
				print_metric(config, ctxp, color, "%8.2f",
					metric_name ?
					metric_name :
					out->force_header ?  name : "",
					ratio);
			}
		} else {
			print_metric(config, ctxp, color, /*unit=*/NULL,
				     out->force_header ?
				     (metric_name ? metric_name : name) : "", 0);
		}
	} else {
		print_metric(config, ctxp, color, /*unit=*/NULL,
			     out->force_header ?
			     (metric_name ? metric_name : name) : "", 0);
	}

	expr__ctx_free(pctx);
}

double test_generic_metric(struct metric_expr *mexp, int aggr_idx)
{
	struct expr_parse_ctx *pctx;
	double ratio = 0.0;

	pctx = expr__ctx_new();
	if (!pctx)
		return NAN;

	if (prepare_metric(mexp->metric_events, mexp->metric_refs, pctx, aggr_idx) < 0)
		goto out;

	if (expr__parse(&ratio, pctx, mexp->metric_expr))
		ratio = 0.0;

out:
	expr__ctx_free(pctx);
	return ratio;
}

static void perf_stat__print_metricgroup_header(struct perf_stat_config *config,
						struct evsel *evsel,
						void *ctxp,
						const char *name,
						struct perf_stat_output_ctx *out)
{
	bool need_full_name = perf_pmus__num_core_pmus() > 1;
	static const char *last_name;
	static const char *last_pmu;
	char full_name[64];

	/*
	 * A metricgroup may have several metric events,
	 * e.g.,TopdownL1 on e-core of ADL.
	 * The name has been output by the first metric
	 * event. Only align with other metics from
	 * different metric events.
	 */
	if (last_name && !strcmp(last_name, name)) {
		if (!need_full_name || !strcmp(last_pmu, evsel->pmu_name)) {
			out->print_metricgroup_header(config, ctxp, NULL);
			return;
		}
	}

	if (need_full_name)
		scnprintf(full_name, sizeof(full_name), "%s (%s)", name, evsel->pmu_name);
	else
		scnprintf(full_name, sizeof(full_name), "%s", name);

	out->print_metricgroup_header(config, ctxp, full_name);

	last_name = name;
	last_pmu = evsel->pmu_name;
}

/**
 * perf_stat__print_shadow_stats_metricgroup - Print out metrics associated with the evsel
 *					       For the non-default, all metrics associated
 *					       with the evsel are printed.
 *					       For the default mode, only the metrics from
 *					       the same metricgroup and the name of the
 *					       metricgroup are printed. To print the metrics
 *					       from the next metricgroup (if available),
 *					       invoke the function with correspoinding
 *					       metric_expr.
 */
void *perf_stat__print_shadow_stats_metricgroup(struct perf_stat_config *config,
						struct evsel *evsel,
						int aggr_idx,
						int *num,
						void *from,
						struct perf_stat_output_ctx *out,
						struct rblist *metric_events)
{
	struct metric_event *me;
	struct metric_expr *mexp = from;
	void *ctxp = out->ctx;
	bool header_printed = false;
	const char *name = NULL;

	me = metricgroup__lookup(metric_events, evsel, false);
	if (me == NULL)
		return NULL;

	if (!mexp)
		mexp = list_first_entry(&me->head, typeof(*mexp), nd);

	list_for_each_entry_from(mexp, &me->head, nd) {
		/* Print the display name of the Default metricgroup */
		if (!config->metric_only && me->is_default) {
			if (!name)
				name = mexp->default_metricgroup_name;
			/*
			 * Two or more metricgroup may share the same metric
			 * event, e.g., TopdownL1 and TopdownL2 on SPR.
			 * Return and print the prefix, e.g., noise, running
			 * for the next metricgroup.
			 */
			if (strcmp(name, mexp->default_metricgroup_name))
				return (void *)mexp;
			/* Only print the name of the metricgroup once */
			if (!header_printed) {
				header_printed = true;
				perf_stat__print_metricgroup_header(config, evsel, ctxp,
								    name, out);
			}
		}

		if ((*num)++ > 0)
			out->new_line(config, ctxp);
		generic_metric(config, mexp->metric_expr, mexp->metric_threshold,
			       mexp->metric_events, mexp->metric_refs, evsel->name,
			       mexp->metric_name, mexp->metric_unit, mexp->runtime,
			       aggr_idx, out);
	}

	return NULL;
}

void perf_stat__print_shadow_stats(struct perf_stat_config *config,
				   struct evsel *evsel,
				   double avg, int aggr_idx,
				   struct perf_stat_output_ctx *out,
				   struct rblist *metric_events)
{
	typedef void (*stat_print_function_t)(struct perf_stat_config *config,
					const struct evsel *evsel,
					int aggr_idx, double misses,
					struct perf_stat_output_ctx *out);
	static const stat_print_function_t stat_print_function[STAT_MAX] = {
		[STAT_INSTRUCTIONS] = print_instructions,
		[STAT_BRANCH_MISS] = print_branch_miss,
		[STAT_L1D_MISS] = print_l1d_miss,
		[STAT_L1I_MISS] = print_l1i_miss,
		[STAT_DTLB_MISS] = print_dtlb_miss,
		[STAT_ITLB_MISS] = print_itlb_miss,
		[STAT_LL_MISS] = print_ll_miss,
		[STAT_CACHE_MISSES] = print_cache_miss,
		[STAT_STALLED_CYCLES_FRONT] = print_stalled_cycles_front,
		[STAT_STALLED_CYCLES_BACK] = print_stalled_cycles_back,
		[STAT_CYCLES] = print_cycles,
		[STAT_NSECS] = print_nsecs,
	};
	print_metric_t print_metric = out->print_metric;
	void *ctxp = out->ctx;
	int num = 1;

	if (config->iostat_run) {
		iostat_print_metric(config, evsel, out);
	} else {
		stat_print_function_t fn = stat_print_function[evsel__stat_type(evsel)];

		if (fn)
			fn(config, evsel, aggr_idx, avg, out);
		else {
			double nsecs =	find_stat(evsel, aggr_idx, STAT_NSECS);

			if (nsecs) {
				char unit = ' ';
				char unit_buf[10] = "/sec";
				double ratio = convert_unit_double(1000000000.0 * avg / nsecs,
								   &unit);

				if (unit != ' ')
					snprintf(unit_buf, sizeof(unit_buf), "%c/sec", unit);
				print_metric(config, ctxp, NULL, "%8.3f", unit_buf, ratio);
			} else
				num = 0;
		}
	}

	perf_stat__print_shadow_stats_metricgroup(config, evsel, aggr_idx,
						  &num, NULL, out, metric_events);

	if (num == 0)
		print_metric(config, ctxp, NULL, NULL, NULL, 0);
}

/**
 * perf_stat__skip_metric_event - Skip the evsel in the Default metricgroup,
 *				  if it's not running or not the metric event.
 */
bool perf_stat__skip_metric_event(struct evsel *evsel,
				  struct rblist *metric_events,
				  u64 ena, u64 run)
{
	if (!evsel->default_metricgroup)
		return false;

	if (!ena || !run)
		return true;

	return !metricgroup__lookup(metric_events, evsel, false);
}
