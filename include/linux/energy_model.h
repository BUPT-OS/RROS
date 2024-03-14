/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ENERGY_MODEL_H
#define _LINUX_ENERGY_MODEL_H
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/jump_label.h>
#include <linux/kobject.h>
#include <linux/rcupdate.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/topology.h>
#include <linux/types.h>

/**
 * struct em_perf_state - Performance state of a performance domain
 * @frequency:	The frequency in KHz, for consistency with CPUFreq
 * @power:	The power consumed at this level (by 1 CPU or by a registered
 *		device). It can be a total power: static and dynamic.
 * @cost:	The cost coefficient associated with this level, used during
 *		energy calculation. Equal to: power * max_frequency / frequency
 * @flags:	see "em_perf_state flags" description below.
 */
struct em_perf_state {
	unsigned long frequency;
	unsigned long power;
	unsigned long cost;
	unsigned long flags;
};

/*
 * em_perf_state flags:
 *
 * EM_PERF_STATE_INEFFICIENT: The performance state is inefficient. There is
 * in this em_perf_domain, another performance state with a higher frequency
 * but a lower or equal power cost. Such inefficient states are ignored when
 * using em_pd_get_efficient_*() functions.
 */
#define EM_PERF_STATE_INEFFICIENT BIT(0)

/**
 * struct em_perf_domain - Performance domain
 * @table:		List of performance states, in ascending order
 * @nr_perf_states:	Number of performance states
 * @flags:		See "em_perf_domain flags"
 * @cpus:		Cpumask covering the CPUs of the domain. It's here
 *			for performance reasons to avoid potential cache
 *			misses during energy calculations in the scheduler
 *			and simplifies allocating/freeing that memory region.
 *
 * In case of CPU device, a "performance domain" represents a group of CPUs
 * whose performance is scaled together. All CPUs of a performance domain
 * must have the same micro-architecture. Performance domains often have
 * a 1-to-1 mapping with CPUFreq policies. In case of other devices the @cpus
 * field is unused.
 */
struct em_perf_domain {
	struct em_perf_state *table;
	int nr_perf_states;
	unsigned long flags;
	unsigned long cpus[];
};

/*
 *  em_perf_domain flags:
 *
 *  EM_PERF_DOMAIN_MICROWATTS: The power values are in micro-Watts or some
 *  other scale.
 *
 *  EM_PERF_DOMAIN_SKIP_INEFFICIENCIES: Skip inefficient states when estimating
 *  energy consumption.
 *
 *  EM_PERF_DOMAIN_ARTIFICIAL: The power values are artificial and might be
 *  created by platform missing real power information
 */
#define EM_PERF_DOMAIN_MICROWATTS BIT(0)
#define EM_PERF_DOMAIN_SKIP_INEFFICIENCIES BIT(1)
#define EM_PERF_DOMAIN_ARTIFICIAL BIT(2)

#define em_span_cpus(em) (to_cpumask((em)->cpus))
#define em_is_artificial(em) ((em)->flags & EM_PERF_DOMAIN_ARTIFICIAL)

#ifdef CONFIG_ENERGY_MODEL
/*
 * The max power value in micro-Watts. The limit of 64 Watts is set as
 * a safety net to not overflow multiplications on 32bit platforms. The
 * 32bit value limit for total Perf Domain power implies a limit of
 * maximum CPUs in such domain to 64.
 */
#define EM_MAX_POWER (64000000) /* 64 Watts */

/*
 * To avoid possible energy estimation overflow on 32bit machines add
 * limits to number of CPUs in the Perf. Domain.
 * We are safe on 64bit machine, thus some big number.
 */
#ifdef CONFIG_64BIT
#define EM_MAX_NUM_CPUS 4096
#else
#define EM_MAX_NUM_CPUS 16
#endif

/*
 * To avoid an overflow on 32bit machines while calculating the energy
 * use a different order in the operation. First divide by the 'cpu_scale'
 * which would reduce big value stored in the 'cost' field, then multiply by
 * the 'sum_util'. This would allow to handle existing platforms, which have
 * e.g. power ~1.3 Watt at max freq, so the 'cost' value > 1mln micro-Watts.
 * In such scenario, where there are 4 CPUs in the Perf. Domain the 'sum_util'
 * could be 4096, then multiplication: 'cost' * 'sum_util'  would overflow.
 * This reordering of operations has some limitations, we lose small
 * precision in the estimation (comparing to 64bit platform w/o reordering).
 *
 * We are safe on 64bit machine.
 */
#ifdef CONFIG_64BIT
#define em_estimate_energy(cost, sum_util, scale_cpu) \
	(((cost) * (sum_util)) / (scale_cpu))
#else
#define em_estimate_energy(cost, sum_util, scale_cpu) \
	(((cost) / (scale_cpu)) * (sum_util))
#endif

struct em_data_callback {
	/**
	 * active_power() - Provide power at the next performance state of
	 *		a device
	 * @dev		: Device for which we do this operation (can be a CPU)
	 * @power	: Active power at the performance state
	 *		(modified)
	 * @freq	: Frequency at the performance state in kHz
	 *		(modified)
	 *
	 * active_power() must find the lowest performance state of 'dev' above
	 * 'freq' and update 'power' and 'freq' to the matching active power
	 * and frequency.
	 *
	 * In case of CPUs, the power is the one of a single CPU in the domain,
	 * expressed in micro-Watts or an abstract scale. It is expected to
	 * fit in the [0, EM_MAX_POWER] range.
	 *
	 * Return 0 on success.
	 */
	int (*active_power)(struct device *dev, unsigned long *power,
			    unsigned long *freq);

	/**
	 * get_cost() - Provide the cost at the given performance state of
	 *		a device
	 * @dev		: Device for which we do this operation (can be a CPU)
	 * @freq	: Frequency at the performance state in kHz
	 * @cost	: The cost value for the performance state
	 *		(modified)
	 *
	 * In case of CPUs, the cost is the one of a single CPU in the domain.
	 * It is expected to fit in the [0, EM_MAX_POWER] range due to internal
	 * usage in EAS calculation.
	 *
	 * Return 0 on success, or appropriate error value in case of failure.
	 */
	int (*get_cost)(struct device *dev, unsigned long freq,
			unsigned long *cost);
};
#define EM_SET_ACTIVE_POWER_CB(em_cb, cb) ((em_cb).active_power = cb)
#define EM_ADV_DATA_CB(_active_power_cb, _cost_cb)	\
	{ .active_power = _active_power_cb,		\
	  .get_cost = _cost_cb }
#define EM_DATA_CB(_active_power_cb)			\
		EM_ADV_DATA_CB(_active_power_cb, NULL)

struct em_perf_domain *em_cpu_get(int cpu);
struct em_perf_domain *em_pd_get(struct device *dev);
int em_dev_register_perf_domain(struct device *dev, unsigned int nr_states,
				struct em_data_callback *cb, cpumask_t *span,
				bool microwatts);
void em_dev_unregister_perf_domain(struct device *dev);

/**
 * em_pd_get_efficient_state() - Get an efficient performance state from the EM
 * @pd   : Performance domain for which we want an efficient frequency
 * @freq : Frequency to map with the EM
 *
 * It is called from the scheduler code quite frequently and as a consequence
 * doesn't implement any check.
 *
 * Return: An efficient performance state, high enough to meet @freq
 * requirement.
 */
static inline
struct em_perf_state *em_pd_get_efficient_state(struct em_perf_domain *pd,
						unsigned long freq)
{
	struct em_perf_state *ps;
	int i;

	for (i = 0; i < pd->nr_perf_states; i++) {
		ps = &pd->table[i];
		if (ps->frequency >= freq) {
			if (pd->flags & EM_PERF_DOMAIN_SKIP_INEFFICIENCIES &&
			    ps->flags & EM_PERF_STATE_INEFFICIENT)
				continue;
			break;
		}
	}

	return ps;
}

/**
 * em_cpu_energy() - Estimates the energy consumed by the CPUs of a
 *		performance domain
 * @pd		: performance domain for which energy has to be estimated
 * @max_util	: highest utilization among CPUs of the domain
 * @sum_util	: sum of the utilization of all CPUs in the domain
 * @allowed_cpu_cap	: maximum allowed CPU capacity for the @pd, which
 *			  might reflect reduced frequency (due to thermal)
 *
 * This function must be used only for CPU devices. There is no validation,
 * i.e. if the EM is a CPU type and has cpumask allocated. It is called from
 * the scheduler code quite frequently and that is why there is not checks.
 *
 * Return: the sum of the energy consumed by the CPUs of the domain assuming
 * a capacity state satisfying the max utilization of the domain.
 */
static inline unsigned long em_cpu_energy(struct em_perf_domain *pd,
				unsigned long max_util, unsigned long sum_util,
				unsigned long allowed_cpu_cap)
{
	unsigned long freq, scale_cpu;
	struct em_perf_state *ps;
	int cpu;

	if (!sum_util)
		return 0;

	/*
	 * In order to predict the performance state, map the utilization of
	 * the most utilized CPU of the performance domain to a requested
	 * frequency, like schedutil. Take also into account that the real
	 * frequency might be set lower (due to thermal capping). Thus, clamp
	 * max utilization to the allowed CPU capacity before calculating
	 * effective frequency.
	 */
	cpu = cpumask_first(to_cpumask(pd->cpus));
	scale_cpu = arch_scale_cpu_capacity(cpu);
	ps = &pd->table[pd->nr_perf_states - 1];

	max_util = map_util_perf(max_util);
	max_util = min(max_util, allowed_cpu_cap);
	freq = map_util_freq(max_util, ps->frequency, scale_cpu);

	/*
	 * Find the lowest performance state of the Energy Model above the
	 * requested frequency.
	 */
	ps = em_pd_get_efficient_state(pd, freq);

	/*
	 * The capacity of a CPU in the domain at the performance state (ps)
	 * can be computed as:
	 *
	 *             ps->freq * scale_cpu
	 *   ps->cap = --------------------                          (1)
	 *                 cpu_max_freq
	 *
	 * So, ignoring the costs of idle states (which are not available in
	 * the EM), the energy consumed by this CPU at that performance state
	 * is estimated as:
	 *
	 *             ps->power * cpu_util
	 *   cpu_nrg = --------------------                          (2)
	 *                   ps->cap
	 *
	 * since 'cpu_util / ps->cap' represents its percentage of busy time.
	 *
	 *   NOTE: Although the result of this computation actually is in
	 *         units of power, it can be manipulated as an energy value
	 *         over a scheduling period, since it is assumed to be
	 *         constant during that interval.
	 *
	 * By injecting (1) in (2), 'cpu_nrg' can be re-expressed as a product
	 * of two terms:
	 *
	 *             ps->power * cpu_max_freq   cpu_util
	 *   cpu_nrg = ------------------------ * ---------          (3)
	 *                    ps->freq            scale_cpu
	 *
	 * The first term is static, and is stored in the em_perf_state struct
	 * as 'ps->cost'.
	 *
	 * Since all CPUs of the domain have the same micro-architecture, they
	 * share the same 'ps->cost', and the same CPU capacity. Hence, the
	 * total energy of the domain (which is the simple sum of the energy of
	 * all of its CPUs) can be factorized as:
	 *
	 *            ps->cost * \Sum cpu_util
	 *   pd_nrg = ------------------------                       (4)
	 *                  scale_cpu
	 */
	return em_estimate_energy(ps->cost, sum_util, scale_cpu);
}

/**
 * em_pd_nr_perf_states() - Get the number of performance states of a perf.
 *				domain
 * @pd		: performance domain for which this must be done
 *
 * Return: the number of performance states in the performance domain table
 */
static inline int em_pd_nr_perf_states(struct em_perf_domain *pd)
{
	return pd->nr_perf_states;
}

#else
struct em_data_callback {};
#define EM_ADV_DATA_CB(_active_power_cb, _cost_cb) { }
#define EM_DATA_CB(_active_power_cb) { }
#define EM_SET_ACTIVE_POWER_CB(em_cb, cb) do { } while (0)

static inline
int em_dev_register_perf_domain(struct device *dev, unsigned int nr_states,
				struct em_data_callback *cb, cpumask_t *span,
				bool microwatts)
{
	return -EINVAL;
}
static inline void em_dev_unregister_perf_domain(struct device *dev)
{
}
static inline struct em_perf_domain *em_cpu_get(int cpu)
{
	return NULL;
}
static inline struct em_perf_domain *em_pd_get(struct device *dev)
{
	return NULL;
}
static inline unsigned long em_cpu_energy(struct em_perf_domain *pd,
			unsigned long max_util, unsigned long sum_util,
			unsigned long allowed_cpu_cap)
{
	return 0;
}
static inline int em_pd_nr_perf_states(struct em_perf_domain *pd)
{
	return 0;
}
#endif

#endif
