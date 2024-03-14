// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * amd-pstate.c - AMD Processor P-state Frequency Driver
 *
 * Copyright (C) 2021 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 * Author: Huang Rui <ray.huang@amd.com>
 *
 * AMD P-State introduces a new CPU performance scaling design for AMD
 * processors using the ACPI Collaborative Performance and Power Control (CPPC)
 * feature which works with the AMD SMU firmware providing a finer grained
 * frequency control range. It is to replace the legacy ACPI P-States control,
 * allows a flexible, low-latency interface for the Linux kernel to directly
 * communicate the performance hints to hardware.
 *
 * AMD P-State is supported on recent AMD Zen base CPU series include some of
 * Zen2 and Zen3 processors. _CPC needs to be present in the ACPI tables of AMD
 * P-State supported system. And there are two types of hardware implementations
 * for AMD P-State: 1) Full MSR Solution and 2) Shared Memory Solution.
 * X86_FEATURE_CPPC CPU feature flag is used to distinguish the different types.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/compiler.h>
#include <linux/dmi.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/static_call.h>
#include <linux/amd-pstate.h>

#include <acpi/processor.h>
#include <acpi/cppc_acpi.h>

#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/cpufeature.h>
#include <asm/cpu_device_id.h>
#include "amd-pstate-trace.h"

#define AMD_PSTATE_TRANSITION_LATENCY	20000
#define AMD_PSTATE_TRANSITION_DELAY	1000

/*
 * TODO: We need more time to fine tune processors with shared memory solution
 * with community together.
 *
 * There are some performance drops on the CPU benchmarks which reports from
 * Suse. We are co-working with them to fine tune the shared memory solution. So
 * we disable it by default to go acpi-cpufreq on these processors and add a
 * module parameter to be able to enable it manually for debugging.
 */
static struct cpufreq_driver *current_pstate_driver;
static struct cpufreq_driver amd_pstate_driver;
static struct cpufreq_driver amd_pstate_epp_driver;
static int cppc_state = AMD_PSTATE_UNDEFINED;
static bool cppc_enabled;

/*
 * AMD Energy Preference Performance (EPP)
 * The EPP is used in the CCLK DPM controller to drive
 * the frequency that a core is going to operate during
 * short periods of activity. EPP values will be utilized for
 * different OS profiles (balanced, performance, power savings)
 * display strings corresponding to EPP index in the
 * energy_perf_strings[]
 *	index		String
 *-------------------------------------
 *	0		default
 *	1		performance
 *	2		balance_performance
 *	3		balance_power
 *	4		power
 */
enum energy_perf_value_index {
	EPP_INDEX_DEFAULT = 0,
	EPP_INDEX_PERFORMANCE,
	EPP_INDEX_BALANCE_PERFORMANCE,
	EPP_INDEX_BALANCE_POWERSAVE,
	EPP_INDEX_POWERSAVE,
};

static const char * const energy_perf_strings[] = {
	[EPP_INDEX_DEFAULT] = "default",
	[EPP_INDEX_PERFORMANCE] = "performance",
	[EPP_INDEX_BALANCE_PERFORMANCE] = "balance_performance",
	[EPP_INDEX_BALANCE_POWERSAVE] = "balance_power",
	[EPP_INDEX_POWERSAVE] = "power",
	NULL
};

static unsigned int epp_values[] = {
	[EPP_INDEX_DEFAULT] = 0,
	[EPP_INDEX_PERFORMANCE] = AMD_CPPC_EPP_PERFORMANCE,
	[EPP_INDEX_BALANCE_PERFORMANCE] = AMD_CPPC_EPP_BALANCE_PERFORMANCE,
	[EPP_INDEX_BALANCE_POWERSAVE] = AMD_CPPC_EPP_BALANCE_POWERSAVE,
	[EPP_INDEX_POWERSAVE] = AMD_CPPC_EPP_POWERSAVE,
 };

typedef int (*cppc_mode_transition_fn)(int);

static inline int get_mode_idx_from_str(const char *str, size_t size)
{
	int i;

	for (i=0; i < AMD_PSTATE_MAX; i++) {
		if (!strncmp(str, amd_pstate_mode_string[i], size))
			return i;
	}
	return -EINVAL;
}

static DEFINE_MUTEX(amd_pstate_limits_lock);
static DEFINE_MUTEX(amd_pstate_driver_lock);

static s16 amd_pstate_get_epp(struct amd_cpudata *cpudata, u64 cppc_req_cached)
{
	u64 epp;
	int ret;

	if (boot_cpu_has(X86_FEATURE_CPPC)) {
		if (!cppc_req_cached) {
			epp = rdmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ,
					&cppc_req_cached);
			if (epp)
				return epp;
		}
		epp = (cppc_req_cached >> 24) & 0xFF;
	} else {
		ret = cppc_get_epp_perf(cpudata->cpu, &epp);
		if (ret < 0) {
			pr_debug("Could not retrieve energy perf value (%d)\n", ret);
			return -EIO;
		}
	}

	return (s16)(epp & 0xff);
}

static int amd_pstate_get_energy_pref_index(struct amd_cpudata *cpudata)
{
	s16 epp;
	int index = -EINVAL;

	epp = amd_pstate_get_epp(cpudata, 0);
	if (epp < 0)
		return epp;

	switch (epp) {
	case AMD_CPPC_EPP_PERFORMANCE:
		index = EPP_INDEX_PERFORMANCE;
		break;
	case AMD_CPPC_EPP_BALANCE_PERFORMANCE:
		index = EPP_INDEX_BALANCE_PERFORMANCE;
		break;
	case AMD_CPPC_EPP_BALANCE_POWERSAVE:
		index = EPP_INDEX_BALANCE_POWERSAVE;
		break;
	case AMD_CPPC_EPP_POWERSAVE:
		index = EPP_INDEX_POWERSAVE;
		break;
	default:
		break;
	}

	return index;
}

static int amd_pstate_set_epp(struct amd_cpudata *cpudata, u32 epp)
{
	int ret;
	struct cppc_perf_ctrls perf_ctrls;

	if (boot_cpu_has(X86_FEATURE_CPPC)) {
		u64 value = READ_ONCE(cpudata->cppc_req_cached);

		value &= ~GENMASK_ULL(31, 24);
		value |= (u64)epp << 24;
		WRITE_ONCE(cpudata->cppc_req_cached, value);

		ret = wrmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ, value);
		if (!ret)
			cpudata->epp_cached = epp;
	} else {
		perf_ctrls.energy_perf = epp;
		ret = cppc_set_epp_perf(cpudata->cpu, &perf_ctrls, 1);
		if (ret) {
			pr_debug("failed to set energy perf value (%d)\n", ret);
			return ret;
		}
		cpudata->epp_cached = epp;
	}

	return ret;
}

static int amd_pstate_set_energy_pref_index(struct amd_cpudata *cpudata,
		int pref_index)
{
	int epp = -EINVAL;
	int ret;

	if (!pref_index) {
		pr_debug("EPP pref_index is invalid\n");
		return -EINVAL;
	}

	if (epp == -EINVAL)
		epp = epp_values[pref_index];

	if (epp > 0 && cpudata->policy == CPUFREQ_POLICY_PERFORMANCE) {
		pr_debug("EPP cannot be set under performance policy\n");
		return -EBUSY;
	}

	ret = amd_pstate_set_epp(cpudata, epp);

	return ret;
}

static inline int pstate_enable(bool enable)
{
	int ret, cpu;
	unsigned long logical_proc_id_mask = 0;

	if (enable == cppc_enabled)
		return 0;

	for_each_present_cpu(cpu) {
		unsigned long logical_id = topology_logical_die_id(cpu);

		if (test_bit(logical_id, &logical_proc_id_mask))
			continue;

		set_bit(logical_id, &logical_proc_id_mask);

		ret = wrmsrl_safe_on_cpu(cpu, MSR_AMD_CPPC_ENABLE,
				enable);
		if (ret)
			return ret;
	}

	cppc_enabled = enable;
	return 0;
}

static int cppc_enable(bool enable)
{
	int cpu, ret = 0;
	struct cppc_perf_ctrls perf_ctrls;

	if (enable == cppc_enabled)
		return 0;

	for_each_present_cpu(cpu) {
		ret = cppc_set_enable(cpu, enable);
		if (ret)
			return ret;

		/* Enable autonomous mode for EPP */
		if (cppc_state == AMD_PSTATE_ACTIVE) {
			/* Set desired perf as zero to allow EPP firmware control */
			perf_ctrls.desired_perf = 0;
			ret = cppc_set_perf(cpu, &perf_ctrls);
			if (ret)
				return ret;
		}
	}

	cppc_enabled = enable;
	return ret;
}

DEFINE_STATIC_CALL(amd_pstate_enable, pstate_enable);

static inline int amd_pstate_enable(bool enable)
{
	return static_call(amd_pstate_enable)(enable);
}

static int pstate_init_perf(struct amd_cpudata *cpudata)
{
	u64 cap1;
	u32 highest_perf;

	int ret = rdmsrl_safe_on_cpu(cpudata->cpu, MSR_AMD_CPPC_CAP1,
				     &cap1);
	if (ret)
		return ret;

	/*
	 * TODO: Introduce AMD specific power feature.
	 *
	 * CPPC entry doesn't indicate the highest performance in some ASICs.
	 */
	highest_perf = amd_get_highest_perf();
	if (highest_perf > AMD_CPPC_HIGHEST_PERF(cap1))
		highest_perf = AMD_CPPC_HIGHEST_PERF(cap1);

	WRITE_ONCE(cpudata->highest_perf, highest_perf);

	WRITE_ONCE(cpudata->nominal_perf, AMD_CPPC_NOMINAL_PERF(cap1));
	WRITE_ONCE(cpudata->lowest_nonlinear_perf, AMD_CPPC_LOWNONLIN_PERF(cap1));
	WRITE_ONCE(cpudata->lowest_perf, AMD_CPPC_LOWEST_PERF(cap1));

	return 0;
}

static int cppc_init_perf(struct amd_cpudata *cpudata)
{
	struct cppc_perf_caps cppc_perf;
	u32 highest_perf;

	int ret = cppc_get_perf_caps(cpudata->cpu, &cppc_perf);
	if (ret)
		return ret;

	highest_perf = amd_get_highest_perf();
	if (highest_perf > cppc_perf.highest_perf)
		highest_perf = cppc_perf.highest_perf;

	WRITE_ONCE(cpudata->highest_perf, highest_perf);

	WRITE_ONCE(cpudata->nominal_perf, cppc_perf.nominal_perf);
	WRITE_ONCE(cpudata->lowest_nonlinear_perf,
		   cppc_perf.lowest_nonlinear_perf);
	WRITE_ONCE(cpudata->lowest_perf, cppc_perf.lowest_perf);

	if (cppc_state == AMD_PSTATE_ACTIVE)
		return 0;

	ret = cppc_get_auto_sel_caps(cpudata->cpu, &cppc_perf);
	if (ret) {
		pr_warn("failed to get auto_sel, ret: %d\n", ret);
		return 0;
	}

	ret = cppc_set_auto_sel(cpudata->cpu,
			(cppc_state == AMD_PSTATE_PASSIVE) ? 0 : 1);

	if (ret)
		pr_warn("failed to set auto_sel, ret: %d\n", ret);

	return ret;
}

DEFINE_STATIC_CALL(amd_pstate_init_perf, pstate_init_perf);

static inline int amd_pstate_init_perf(struct amd_cpudata *cpudata)
{
	return static_call(amd_pstate_init_perf)(cpudata);
}

static void pstate_update_perf(struct amd_cpudata *cpudata, u32 min_perf,
			       u32 des_perf, u32 max_perf, bool fast_switch)
{
	if (fast_switch)
		wrmsrl(MSR_AMD_CPPC_REQ, READ_ONCE(cpudata->cppc_req_cached));
	else
		wrmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ,
			      READ_ONCE(cpudata->cppc_req_cached));
}

static void cppc_update_perf(struct amd_cpudata *cpudata,
			     u32 min_perf, u32 des_perf,
			     u32 max_perf, bool fast_switch)
{
	struct cppc_perf_ctrls perf_ctrls;

	perf_ctrls.max_perf = max_perf;
	perf_ctrls.min_perf = min_perf;
	perf_ctrls.desired_perf = des_perf;

	cppc_set_perf(cpudata->cpu, &perf_ctrls);
}

DEFINE_STATIC_CALL(amd_pstate_update_perf, pstate_update_perf);

static inline void amd_pstate_update_perf(struct amd_cpudata *cpudata,
					  u32 min_perf, u32 des_perf,
					  u32 max_perf, bool fast_switch)
{
	static_call(amd_pstate_update_perf)(cpudata, min_perf, des_perf,
					    max_perf, fast_switch);
}

static inline bool amd_pstate_sample(struct amd_cpudata *cpudata)
{
	u64 aperf, mperf, tsc;
	unsigned long flags;

	local_irq_save(flags);
	rdmsrl(MSR_IA32_APERF, aperf);
	rdmsrl(MSR_IA32_MPERF, mperf);
	tsc = rdtsc();

	if (cpudata->prev.mperf == mperf || cpudata->prev.tsc == tsc) {
		local_irq_restore(flags);
		return false;
	}

	local_irq_restore(flags);

	cpudata->cur.aperf = aperf;
	cpudata->cur.mperf = mperf;
	cpudata->cur.tsc =  tsc;
	cpudata->cur.aperf -= cpudata->prev.aperf;
	cpudata->cur.mperf -= cpudata->prev.mperf;
	cpudata->cur.tsc -= cpudata->prev.tsc;

	cpudata->prev.aperf = aperf;
	cpudata->prev.mperf = mperf;
	cpudata->prev.tsc = tsc;

	cpudata->freq = div64_u64((cpudata->cur.aperf * cpu_khz), cpudata->cur.mperf);

	return true;
}

static void amd_pstate_update(struct amd_cpudata *cpudata, u32 min_perf,
			      u32 des_perf, u32 max_perf, bool fast_switch, int gov_flags)
{
	u64 prev = READ_ONCE(cpudata->cppc_req_cached);
	u64 value = prev;

	des_perf = clamp_t(unsigned long, des_perf, min_perf, max_perf);

	if ((cppc_state == AMD_PSTATE_GUIDED) && (gov_flags & CPUFREQ_GOV_DYNAMIC_SWITCHING)) {
		min_perf = des_perf;
		des_perf = 0;
	}

	value &= ~AMD_CPPC_MIN_PERF(~0L);
	value |= AMD_CPPC_MIN_PERF(min_perf);

	value &= ~AMD_CPPC_DES_PERF(~0L);
	value |= AMD_CPPC_DES_PERF(des_perf);

	value &= ~AMD_CPPC_MAX_PERF(~0L);
	value |= AMD_CPPC_MAX_PERF(max_perf);

	if (trace_amd_pstate_perf_enabled() && amd_pstate_sample(cpudata)) {
		trace_amd_pstate_perf(min_perf, des_perf, max_perf, cpudata->freq,
			cpudata->cur.mperf, cpudata->cur.aperf, cpudata->cur.tsc,
				cpudata->cpu, (value != prev), fast_switch);
	}

	if (value == prev)
		return;

	WRITE_ONCE(cpudata->cppc_req_cached, value);

	amd_pstate_update_perf(cpudata, min_perf, des_perf,
			       max_perf, fast_switch);
}

static int amd_pstate_verify(struct cpufreq_policy_data *policy)
{
	cpufreq_verify_within_cpu_limits(policy);

	return 0;
}

static int amd_pstate_update_freq(struct cpufreq_policy *policy,
				  unsigned int target_freq, bool fast_switch)
{
	struct cpufreq_freqs freqs;
	struct amd_cpudata *cpudata = policy->driver_data;
	unsigned long max_perf, min_perf, des_perf, cap_perf;

	if (!cpudata->max_freq)
		return -ENODEV;

	cap_perf = READ_ONCE(cpudata->highest_perf);
	min_perf = READ_ONCE(cpudata->lowest_perf);
	max_perf = cap_perf;

	freqs.old = policy->cur;
	freqs.new = target_freq;

	des_perf = DIV_ROUND_CLOSEST(target_freq * cap_perf,
				     cpudata->max_freq);

	WARN_ON(fast_switch && !policy->fast_switch_enabled);
	/*
	 * If fast_switch is desired, then there aren't any registered
	 * transition notifiers. See comment for
	 * cpufreq_enable_fast_switch().
	 */
	if (!fast_switch)
		cpufreq_freq_transition_begin(policy, &freqs);

	amd_pstate_update(cpudata, min_perf, des_perf,
			max_perf, fast_switch, policy->governor->flags);

	if (!fast_switch)
		cpufreq_freq_transition_end(policy, &freqs, false);

	return 0;
}

static int amd_pstate_target(struct cpufreq_policy *policy,
			     unsigned int target_freq,
			     unsigned int relation)
{
	return amd_pstate_update_freq(policy, target_freq, false);
}

static unsigned int amd_pstate_fast_switch(struct cpufreq_policy *policy,
				  unsigned int target_freq)
{
	return amd_pstate_update_freq(policy, target_freq, true);
}

static void amd_pstate_adjust_perf(unsigned int cpu,
				   unsigned long _min_perf,
				   unsigned long target_perf,
				   unsigned long capacity)
{
	unsigned long max_perf, min_perf, des_perf,
		      cap_perf, lowest_nonlinear_perf, max_freq;
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	struct amd_cpudata *cpudata = policy->driver_data;
	unsigned int target_freq;

	cap_perf = READ_ONCE(cpudata->highest_perf);
	lowest_nonlinear_perf = READ_ONCE(cpudata->lowest_nonlinear_perf);
	max_freq = READ_ONCE(cpudata->max_freq);

	des_perf = cap_perf;
	if (target_perf < capacity)
		des_perf = DIV_ROUND_UP(cap_perf * target_perf, capacity);

	min_perf = READ_ONCE(cpudata->highest_perf);
	if (_min_perf < capacity)
		min_perf = DIV_ROUND_UP(cap_perf * _min_perf, capacity);

	if (min_perf < lowest_nonlinear_perf)
		min_perf = lowest_nonlinear_perf;

	max_perf = cap_perf;
	if (max_perf < min_perf)
		max_perf = min_perf;

	des_perf = clamp_t(unsigned long, des_perf, min_perf, max_perf);
	target_freq = div_u64(des_perf * max_freq, max_perf);
	policy->cur = target_freq;

	amd_pstate_update(cpudata, min_perf, des_perf, max_perf, true,
			policy->governor->flags);
	cpufreq_cpu_put(policy);
}

static int amd_get_min_freq(struct amd_cpudata *cpudata)
{
	struct cppc_perf_caps cppc_perf;

	int ret = cppc_get_perf_caps(cpudata->cpu, &cppc_perf);
	if (ret)
		return ret;

	/* Switch to khz */
	return cppc_perf.lowest_freq * 1000;
}

static int amd_get_max_freq(struct amd_cpudata *cpudata)
{
	struct cppc_perf_caps cppc_perf;
	u32 max_perf, max_freq, nominal_freq, nominal_perf;
	u64 boost_ratio;

	int ret = cppc_get_perf_caps(cpudata->cpu, &cppc_perf);
	if (ret)
		return ret;

	nominal_freq = cppc_perf.nominal_freq;
	nominal_perf = READ_ONCE(cpudata->nominal_perf);
	max_perf = READ_ONCE(cpudata->highest_perf);

	boost_ratio = div_u64(max_perf << SCHED_CAPACITY_SHIFT,
			      nominal_perf);

	max_freq = nominal_freq * boost_ratio >> SCHED_CAPACITY_SHIFT;

	/* Switch to khz */
	return max_freq * 1000;
}

static int amd_get_nominal_freq(struct amd_cpudata *cpudata)
{
	struct cppc_perf_caps cppc_perf;

	int ret = cppc_get_perf_caps(cpudata->cpu, &cppc_perf);
	if (ret)
		return ret;

	/* Switch to khz */
	return cppc_perf.nominal_freq * 1000;
}

static int amd_get_lowest_nonlinear_freq(struct amd_cpudata *cpudata)
{
	struct cppc_perf_caps cppc_perf;
	u32 lowest_nonlinear_freq, lowest_nonlinear_perf,
	    nominal_freq, nominal_perf;
	u64 lowest_nonlinear_ratio;

	int ret = cppc_get_perf_caps(cpudata->cpu, &cppc_perf);
	if (ret)
		return ret;

	nominal_freq = cppc_perf.nominal_freq;
	nominal_perf = READ_ONCE(cpudata->nominal_perf);

	lowest_nonlinear_perf = cppc_perf.lowest_nonlinear_perf;

	lowest_nonlinear_ratio = div_u64(lowest_nonlinear_perf << SCHED_CAPACITY_SHIFT,
					 nominal_perf);

	lowest_nonlinear_freq = nominal_freq * lowest_nonlinear_ratio >> SCHED_CAPACITY_SHIFT;

	/* Switch to khz */
	return lowest_nonlinear_freq * 1000;
}

static int amd_pstate_set_boost(struct cpufreq_policy *policy, int state)
{
	struct amd_cpudata *cpudata = policy->driver_data;
	int ret;

	if (!cpudata->boost_supported) {
		pr_err("Boost mode is not supported by this processor or SBIOS\n");
		return -EINVAL;
	}

	if (state)
		policy->cpuinfo.max_freq = cpudata->max_freq;
	else
		policy->cpuinfo.max_freq = cpudata->nominal_freq;

	policy->max = policy->cpuinfo.max_freq;

	ret = freq_qos_update_request(&cpudata->req[1],
				      policy->cpuinfo.max_freq);
	if (ret < 0)
		return ret;

	return 0;
}

static void amd_pstate_boost_init(struct amd_cpudata *cpudata)
{
	u32 highest_perf, nominal_perf;

	highest_perf = READ_ONCE(cpudata->highest_perf);
	nominal_perf = READ_ONCE(cpudata->nominal_perf);

	if (highest_perf <= nominal_perf)
		return;

	cpudata->boost_supported = true;
	current_pstate_driver->boost_enabled = true;
}

static void amd_perf_ctl_reset(unsigned int cpu)
{
	wrmsrl_on_cpu(cpu, MSR_AMD_PERF_CTL, 0);
}

static int amd_pstate_cpu_init(struct cpufreq_policy *policy)
{
	int min_freq, max_freq, nominal_freq, lowest_nonlinear_freq, ret;
	struct device *dev;
	struct amd_cpudata *cpudata;

	/*
	 * Resetting PERF_CTL_MSR will put the CPU in P0 frequency,
	 * which is ideal for initialization process.
	 */
	amd_perf_ctl_reset(policy->cpu);
	dev = get_cpu_device(policy->cpu);
	if (!dev)
		return -ENODEV;

	cpudata = kzalloc(sizeof(*cpudata), GFP_KERNEL);
	if (!cpudata)
		return -ENOMEM;

	cpudata->cpu = policy->cpu;

	ret = amd_pstate_init_perf(cpudata);
	if (ret)
		goto free_cpudata1;

	min_freq = amd_get_min_freq(cpudata);
	max_freq = amd_get_max_freq(cpudata);
	nominal_freq = amd_get_nominal_freq(cpudata);
	lowest_nonlinear_freq = amd_get_lowest_nonlinear_freq(cpudata);

	if (min_freq < 0 || max_freq < 0 || min_freq > max_freq) {
		dev_err(dev, "min_freq(%d) or max_freq(%d) value is incorrect\n",
			min_freq, max_freq);
		ret = -EINVAL;
		goto free_cpudata1;
	}

	policy->cpuinfo.transition_latency = AMD_PSTATE_TRANSITION_LATENCY;
	policy->transition_delay_us = AMD_PSTATE_TRANSITION_DELAY;

	policy->min = min_freq;
	policy->max = max_freq;

	policy->cpuinfo.min_freq = min_freq;
	policy->cpuinfo.max_freq = max_freq;

	/* It will be updated by governor */
	policy->cur = policy->cpuinfo.min_freq;

	if (boot_cpu_has(X86_FEATURE_CPPC))
		policy->fast_switch_possible = true;

	ret = freq_qos_add_request(&policy->constraints, &cpudata->req[0],
				   FREQ_QOS_MIN, policy->cpuinfo.min_freq);
	if (ret < 0) {
		dev_err(dev, "Failed to add min-freq constraint (%d)\n", ret);
		goto free_cpudata1;
	}

	ret = freq_qos_add_request(&policy->constraints, &cpudata->req[1],
				   FREQ_QOS_MAX, policy->cpuinfo.max_freq);
	if (ret < 0) {
		dev_err(dev, "Failed to add max-freq constraint (%d)\n", ret);
		goto free_cpudata2;
	}

	/* Initial processor data capability frequencies */
	cpudata->max_freq = max_freq;
	cpudata->min_freq = min_freq;
	cpudata->nominal_freq = nominal_freq;
	cpudata->lowest_nonlinear_freq = lowest_nonlinear_freq;

	policy->driver_data = cpudata;

	amd_pstate_boost_init(cpudata);
	if (!current_pstate_driver->adjust_perf)
		current_pstate_driver->adjust_perf = amd_pstate_adjust_perf;

	return 0;

free_cpudata2:
	freq_qos_remove_request(&cpudata->req[0]);
free_cpudata1:
	kfree(cpudata);
	return ret;
}

static int amd_pstate_cpu_exit(struct cpufreq_policy *policy)
{
	struct amd_cpudata *cpudata = policy->driver_data;

	freq_qos_remove_request(&cpudata->req[1]);
	freq_qos_remove_request(&cpudata->req[0]);
	policy->fast_switch_possible = false;
	kfree(cpudata);

	return 0;
}

static int amd_pstate_cpu_resume(struct cpufreq_policy *policy)
{
	int ret;

	ret = amd_pstate_enable(true);
	if (ret)
		pr_err("failed to enable amd-pstate during resume, return %d\n", ret);

	return ret;
}

static int amd_pstate_cpu_suspend(struct cpufreq_policy *policy)
{
	int ret;

	ret = amd_pstate_enable(false);
	if (ret)
		pr_err("failed to disable amd-pstate during suspend, return %d\n", ret);

	return ret;
}

/* Sysfs attributes */

/*
 * This frequency is to indicate the maximum hardware frequency.
 * If boost is not active but supported, the frequency will be larger than the
 * one in cpuinfo.
 */
static ssize_t show_amd_pstate_max_freq(struct cpufreq_policy *policy,
					char *buf)
{
	int max_freq;
	struct amd_cpudata *cpudata = policy->driver_data;

	max_freq = amd_get_max_freq(cpudata);
	if (max_freq < 0)
		return max_freq;

	return sysfs_emit(buf, "%u\n", max_freq);
}

static ssize_t show_amd_pstate_lowest_nonlinear_freq(struct cpufreq_policy *policy,
						     char *buf)
{
	int freq;
	struct amd_cpudata *cpudata = policy->driver_data;

	freq = amd_get_lowest_nonlinear_freq(cpudata);
	if (freq < 0)
		return freq;

	return sysfs_emit(buf, "%u\n", freq);
}

/*
 * In some of ASICs, the highest_perf is not the one in the _CPC table, so we
 * need to expose it to sysfs.
 */
static ssize_t show_amd_pstate_highest_perf(struct cpufreq_policy *policy,
					    char *buf)
{
	u32 perf;
	struct amd_cpudata *cpudata = policy->driver_data;

	perf = READ_ONCE(cpudata->highest_perf);

	return sysfs_emit(buf, "%u\n", perf);
}

static ssize_t show_energy_performance_available_preferences(
				struct cpufreq_policy *policy, char *buf)
{
	int i = 0;
	int offset = 0;

	while (energy_perf_strings[i] != NULL)
		offset += sysfs_emit_at(buf, offset, "%s ", energy_perf_strings[i++]);

	sysfs_emit_at(buf, offset, "\n");

	return offset;
}

static ssize_t store_energy_performance_preference(
		struct cpufreq_policy *policy, const char *buf, size_t count)
{
	struct amd_cpudata *cpudata = policy->driver_data;
	char str_preference[21];
	ssize_t ret;

	ret = sscanf(buf, "%20s", str_preference);
	if (ret != 1)
		return -EINVAL;

	ret = match_string(energy_perf_strings, -1, str_preference);
	if (ret < 0)
		return -EINVAL;

	mutex_lock(&amd_pstate_limits_lock);
	ret = amd_pstate_set_energy_pref_index(cpudata, ret);
	mutex_unlock(&amd_pstate_limits_lock);

	return ret ?: count;
}

static ssize_t show_energy_performance_preference(
				struct cpufreq_policy *policy, char *buf)
{
	struct amd_cpudata *cpudata = policy->driver_data;
	int preference;

	preference = amd_pstate_get_energy_pref_index(cpudata);
	if (preference < 0)
		return preference;

	return sysfs_emit(buf, "%s\n", energy_perf_strings[preference]);
}

static void amd_pstate_driver_cleanup(void)
{
	amd_pstate_enable(false);
	cppc_state = AMD_PSTATE_DISABLE;
	current_pstate_driver = NULL;
}

static int amd_pstate_register_driver(int mode)
{
	int ret;

	if (mode == AMD_PSTATE_PASSIVE || mode == AMD_PSTATE_GUIDED)
		current_pstate_driver = &amd_pstate_driver;
	else if (mode == AMD_PSTATE_ACTIVE)
		current_pstate_driver = &amd_pstate_epp_driver;
	else
		return -EINVAL;

	cppc_state = mode;
	ret = cpufreq_register_driver(current_pstate_driver);
	if (ret) {
		amd_pstate_driver_cleanup();
		return ret;
	}
	return 0;
}

static int amd_pstate_unregister_driver(int dummy)
{
	cpufreq_unregister_driver(current_pstate_driver);
	amd_pstate_driver_cleanup();
	return 0;
}

static int amd_pstate_change_mode_without_dvr_change(int mode)
{
	int cpu = 0;

	cppc_state = mode;

	if (boot_cpu_has(X86_FEATURE_CPPC) || cppc_state == AMD_PSTATE_ACTIVE)
		return 0;

	for_each_present_cpu(cpu) {
		cppc_set_auto_sel(cpu, (cppc_state == AMD_PSTATE_PASSIVE) ? 0 : 1);
	}

	return 0;
}

static int amd_pstate_change_driver_mode(int mode)
{
	int ret;

	ret = amd_pstate_unregister_driver(0);
	if (ret)
		return ret;

	ret = amd_pstate_register_driver(mode);
	if (ret)
		return ret;

	return 0;
}

static cppc_mode_transition_fn mode_state_machine[AMD_PSTATE_MAX][AMD_PSTATE_MAX] = {
	[AMD_PSTATE_DISABLE]         = {
		[AMD_PSTATE_DISABLE]     = NULL,
		[AMD_PSTATE_PASSIVE]     = amd_pstate_register_driver,
		[AMD_PSTATE_ACTIVE]      = amd_pstate_register_driver,
		[AMD_PSTATE_GUIDED]      = amd_pstate_register_driver,
	},
	[AMD_PSTATE_PASSIVE]         = {
		[AMD_PSTATE_DISABLE]     = amd_pstate_unregister_driver,
		[AMD_PSTATE_PASSIVE]     = NULL,
		[AMD_PSTATE_ACTIVE]      = amd_pstate_change_driver_mode,
		[AMD_PSTATE_GUIDED]      = amd_pstate_change_mode_without_dvr_change,
	},
	[AMD_PSTATE_ACTIVE]          = {
		[AMD_PSTATE_DISABLE]     = amd_pstate_unregister_driver,
		[AMD_PSTATE_PASSIVE]     = amd_pstate_change_driver_mode,
		[AMD_PSTATE_ACTIVE]      = NULL,
		[AMD_PSTATE_GUIDED]      = amd_pstate_change_driver_mode,
	},
	[AMD_PSTATE_GUIDED]          = {
		[AMD_PSTATE_DISABLE]     = amd_pstate_unregister_driver,
		[AMD_PSTATE_PASSIVE]     = amd_pstate_change_mode_without_dvr_change,
		[AMD_PSTATE_ACTIVE]      = amd_pstate_change_driver_mode,
		[AMD_PSTATE_GUIDED]      = NULL,
	},
};

static ssize_t amd_pstate_show_status(char *buf)
{
	if (!current_pstate_driver)
		return sysfs_emit(buf, "disable\n");

	return sysfs_emit(buf, "%s\n", amd_pstate_mode_string[cppc_state]);
}

static int amd_pstate_update_status(const char *buf, size_t size)
{
	int mode_idx;

	if (size > strlen("passive") || size < strlen("active"))
		return -EINVAL;

	mode_idx = get_mode_idx_from_str(buf, size);

	if (mode_idx < 0 || mode_idx >= AMD_PSTATE_MAX)
		return -EINVAL;

	if (mode_state_machine[cppc_state][mode_idx])
		return mode_state_machine[cppc_state][mode_idx](mode_idx);

	return 0;
}

static ssize_t status_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	ssize_t ret;

	mutex_lock(&amd_pstate_driver_lock);
	ret = amd_pstate_show_status(buf);
	mutex_unlock(&amd_pstate_driver_lock);

	return ret;
}

static ssize_t status_store(struct device *a, struct device_attribute *b,
			    const char *buf, size_t count)
{
	char *p = memchr(buf, '\n', count);
	int ret;

	mutex_lock(&amd_pstate_driver_lock);
	ret = amd_pstate_update_status(buf, p ? p - buf : count);
	mutex_unlock(&amd_pstate_driver_lock);

	return ret < 0 ? ret : count;
}

cpufreq_freq_attr_ro(amd_pstate_max_freq);
cpufreq_freq_attr_ro(amd_pstate_lowest_nonlinear_freq);

cpufreq_freq_attr_ro(amd_pstate_highest_perf);
cpufreq_freq_attr_rw(energy_performance_preference);
cpufreq_freq_attr_ro(energy_performance_available_preferences);
static DEVICE_ATTR_RW(status);

static struct freq_attr *amd_pstate_attr[] = {
	&amd_pstate_max_freq,
	&amd_pstate_lowest_nonlinear_freq,
	&amd_pstate_highest_perf,
	NULL,
};

static struct freq_attr *amd_pstate_epp_attr[] = {
	&amd_pstate_max_freq,
	&amd_pstate_lowest_nonlinear_freq,
	&amd_pstate_highest_perf,
	&energy_performance_preference,
	&energy_performance_available_preferences,
	NULL,
};

static struct attribute *pstate_global_attributes[] = {
	&dev_attr_status.attr,
	NULL
};

static const struct attribute_group amd_pstate_global_attr_group = {
	.name = "amd_pstate",
	.attrs = pstate_global_attributes,
};

static bool amd_pstate_acpi_pm_profile_server(void)
{
	switch (acpi_gbl_FADT.preferred_profile) {
	case PM_ENTERPRISE_SERVER:
	case PM_SOHO_SERVER:
	case PM_PERFORMANCE_SERVER:
		return true;
	}
	return false;
}

static bool amd_pstate_acpi_pm_profile_undefined(void)
{
	if (acpi_gbl_FADT.preferred_profile == PM_UNSPECIFIED)
		return true;
	if (acpi_gbl_FADT.preferred_profile >= NR_PM_PROFILES)
		return true;
	return false;
}

static int amd_pstate_epp_cpu_init(struct cpufreq_policy *policy)
{
	int min_freq, max_freq, nominal_freq, lowest_nonlinear_freq, ret;
	struct amd_cpudata *cpudata;
	struct device *dev;
	u64 value;

	/*
	 * Resetting PERF_CTL_MSR will put the CPU in P0 frequency,
	 * which is ideal for initialization process.
	 */
	amd_perf_ctl_reset(policy->cpu);
	dev = get_cpu_device(policy->cpu);
	if (!dev)
		return -ENODEV;

	cpudata = kzalloc(sizeof(*cpudata), GFP_KERNEL);
	if (!cpudata)
		return -ENOMEM;

	cpudata->cpu = policy->cpu;
	cpudata->epp_policy = 0;

	ret = amd_pstate_init_perf(cpudata);
	if (ret)
		goto free_cpudata1;

	min_freq = amd_get_min_freq(cpudata);
	max_freq = amd_get_max_freq(cpudata);
	nominal_freq = amd_get_nominal_freq(cpudata);
	lowest_nonlinear_freq = amd_get_lowest_nonlinear_freq(cpudata);
	if (min_freq < 0 || max_freq < 0 || min_freq > max_freq) {
		dev_err(dev, "min_freq(%d) or max_freq(%d) value is incorrect\n",
				min_freq, max_freq);
		ret = -EINVAL;
		goto free_cpudata1;
	}

	policy->cpuinfo.min_freq = min_freq;
	policy->cpuinfo.max_freq = max_freq;
	/* It will be updated by governor */
	policy->cur = policy->cpuinfo.min_freq;

	/* Initial processor data capability frequencies */
	cpudata->max_freq = max_freq;
	cpudata->min_freq = min_freq;
	cpudata->nominal_freq = nominal_freq;
	cpudata->lowest_nonlinear_freq = lowest_nonlinear_freq;

	policy->driver_data = cpudata;

	cpudata->epp_cached = amd_pstate_get_epp(cpudata, 0);

	policy->min = policy->cpuinfo.min_freq;
	policy->max = policy->cpuinfo.max_freq;

	/*
	 * Set the policy to provide a valid fallback value in case
	 * the default cpufreq governor is neither powersave nor performance.
	 */
	if (amd_pstate_acpi_pm_profile_server() ||
	    amd_pstate_acpi_pm_profile_undefined())
		policy->policy = CPUFREQ_POLICY_PERFORMANCE;
	else
		policy->policy = CPUFREQ_POLICY_POWERSAVE;

	if (boot_cpu_has(X86_FEATURE_CPPC)) {
		ret = rdmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ, &value);
		if (ret)
			return ret;
		WRITE_ONCE(cpudata->cppc_req_cached, value);

		ret = rdmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_CAP1, &value);
		if (ret)
			return ret;
		WRITE_ONCE(cpudata->cppc_cap1_cached, value);
	}
	amd_pstate_boost_init(cpudata);

	return 0;

free_cpudata1:
	kfree(cpudata);
	return ret;
}

static int amd_pstate_epp_cpu_exit(struct cpufreq_policy *policy)
{
	pr_debug("CPU %d exiting\n", policy->cpu);
	return 0;
}

static void amd_pstate_epp_init(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	struct amd_cpudata *cpudata = policy->driver_data;
	u32 max_perf, min_perf;
	u64 value;
	s16 epp;

	max_perf = READ_ONCE(cpudata->highest_perf);
	min_perf = READ_ONCE(cpudata->lowest_perf);

	value = READ_ONCE(cpudata->cppc_req_cached);

	if (cpudata->policy == CPUFREQ_POLICY_PERFORMANCE)
		min_perf = max_perf;

	/* Initial min/max values for CPPC Performance Controls Register */
	value &= ~AMD_CPPC_MIN_PERF(~0L);
	value |= AMD_CPPC_MIN_PERF(min_perf);

	value &= ~AMD_CPPC_MAX_PERF(~0L);
	value |= AMD_CPPC_MAX_PERF(max_perf);

	/* CPPC EPP feature require to set zero to the desire perf bit */
	value &= ~AMD_CPPC_DES_PERF(~0L);
	value |= AMD_CPPC_DES_PERF(0);

	if (cpudata->epp_policy == cpudata->policy)
		goto skip_epp;

	cpudata->epp_policy = cpudata->policy;

	/* Get BIOS pre-defined epp value */
	epp = amd_pstate_get_epp(cpudata, value);
	if (epp < 0) {
		/**
		 * This return value can only be negative for shared_memory
		 * systems where EPP register read/write not supported.
		 */
		goto skip_epp;
	}

	if (cpudata->policy == CPUFREQ_POLICY_PERFORMANCE)
		epp = 0;

	/* Set initial EPP value */
	if (boot_cpu_has(X86_FEATURE_CPPC)) {
		value &= ~GENMASK_ULL(31, 24);
		value |= (u64)epp << 24;
	}

	WRITE_ONCE(cpudata->cppc_req_cached, value);
	amd_pstate_set_epp(cpudata, epp);
skip_epp:
	cpufreq_cpu_put(policy);
}

static int amd_pstate_epp_set_policy(struct cpufreq_policy *policy)
{
	struct amd_cpudata *cpudata = policy->driver_data;

	if (!policy->cpuinfo.max_freq)
		return -ENODEV;

	pr_debug("set_policy: cpuinfo.max %u policy->max %u\n",
				policy->cpuinfo.max_freq, policy->max);

	cpudata->policy = policy->policy;

	amd_pstate_epp_init(policy->cpu);

	return 0;
}

static void amd_pstate_epp_reenable(struct amd_cpudata *cpudata)
{
	struct cppc_perf_ctrls perf_ctrls;
	u64 value, max_perf;
	int ret;

	ret = amd_pstate_enable(true);
	if (ret)
		pr_err("failed to enable amd pstate during resume, return %d\n", ret);

	value = READ_ONCE(cpudata->cppc_req_cached);
	max_perf = READ_ONCE(cpudata->highest_perf);

	if (boot_cpu_has(X86_FEATURE_CPPC)) {
		wrmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ, value);
	} else {
		perf_ctrls.max_perf = max_perf;
		perf_ctrls.energy_perf = AMD_CPPC_ENERGY_PERF_PREF(cpudata->epp_cached);
		cppc_set_perf(cpudata->cpu, &perf_ctrls);
	}
}

static int amd_pstate_epp_cpu_online(struct cpufreq_policy *policy)
{
	struct amd_cpudata *cpudata = policy->driver_data;

	pr_debug("AMD CPU Core %d going online\n", cpudata->cpu);

	if (cppc_state == AMD_PSTATE_ACTIVE) {
		amd_pstate_epp_reenable(cpudata);
		cpudata->suspended = false;
	}

	return 0;
}

static void amd_pstate_epp_offline(struct cpufreq_policy *policy)
{
	struct amd_cpudata *cpudata = policy->driver_data;
	struct cppc_perf_ctrls perf_ctrls;
	int min_perf;
	u64 value;

	min_perf = READ_ONCE(cpudata->lowest_perf);
	value = READ_ONCE(cpudata->cppc_req_cached);

	mutex_lock(&amd_pstate_limits_lock);
	if (boot_cpu_has(X86_FEATURE_CPPC)) {
		cpudata->epp_policy = CPUFREQ_POLICY_UNKNOWN;

		/* Set max perf same as min perf */
		value &= ~AMD_CPPC_MAX_PERF(~0L);
		value |= AMD_CPPC_MAX_PERF(min_perf);
		value &= ~AMD_CPPC_MIN_PERF(~0L);
		value |= AMD_CPPC_MIN_PERF(min_perf);
		wrmsrl_on_cpu(cpudata->cpu, MSR_AMD_CPPC_REQ, value);
	} else {
		perf_ctrls.desired_perf = 0;
		perf_ctrls.max_perf = min_perf;
		perf_ctrls.energy_perf = AMD_CPPC_ENERGY_PERF_PREF(HWP_EPP_BALANCE_POWERSAVE);
		cppc_set_perf(cpudata->cpu, &perf_ctrls);
	}
	mutex_unlock(&amd_pstate_limits_lock);
}

static int amd_pstate_epp_cpu_offline(struct cpufreq_policy *policy)
{
	struct amd_cpudata *cpudata = policy->driver_data;

	pr_debug("AMD CPU Core %d going offline\n", cpudata->cpu);

	if (cpudata->suspended)
		return 0;

	if (cppc_state == AMD_PSTATE_ACTIVE)
		amd_pstate_epp_offline(policy);

	return 0;
}

static int amd_pstate_epp_verify_policy(struct cpufreq_policy_data *policy)
{
	cpufreq_verify_within_cpu_limits(policy);
	pr_debug("policy_max =%d, policy_min=%d\n", policy->max, policy->min);
	return 0;
}

static int amd_pstate_epp_suspend(struct cpufreq_policy *policy)
{
	struct amd_cpudata *cpudata = policy->driver_data;
	int ret;

	/* avoid suspending when EPP is not enabled */
	if (cppc_state != AMD_PSTATE_ACTIVE)
		return 0;

	/* set this flag to avoid setting core offline*/
	cpudata->suspended = true;

	/* disable CPPC in lowlevel firmware */
	ret = amd_pstate_enable(false);
	if (ret)
		pr_err("failed to suspend, return %d\n", ret);

	return 0;
}

static int amd_pstate_epp_resume(struct cpufreq_policy *policy)
{
	struct amd_cpudata *cpudata = policy->driver_data;

	if (cpudata->suspended) {
		mutex_lock(&amd_pstate_limits_lock);

		/* enable amd pstate from suspend state*/
		amd_pstate_epp_reenable(cpudata);

		mutex_unlock(&amd_pstate_limits_lock);

		cpudata->suspended = false;
	}

	return 0;
}

static struct cpufreq_driver amd_pstate_driver = {
	.flags		= CPUFREQ_CONST_LOOPS | CPUFREQ_NEED_UPDATE_LIMITS,
	.verify		= amd_pstate_verify,
	.target		= amd_pstate_target,
	.fast_switch    = amd_pstate_fast_switch,
	.init		= amd_pstate_cpu_init,
	.exit		= amd_pstate_cpu_exit,
	.suspend	= amd_pstate_cpu_suspend,
	.resume		= amd_pstate_cpu_resume,
	.set_boost	= amd_pstate_set_boost,
	.name		= "amd-pstate",
	.attr		= amd_pstate_attr,
};

static struct cpufreq_driver amd_pstate_epp_driver = {
	.flags		= CPUFREQ_CONST_LOOPS,
	.verify		= amd_pstate_epp_verify_policy,
	.setpolicy	= amd_pstate_epp_set_policy,
	.init		= amd_pstate_epp_cpu_init,
	.exit		= amd_pstate_epp_cpu_exit,
	.offline	= amd_pstate_epp_cpu_offline,
	.online		= amd_pstate_epp_cpu_online,
	.suspend	= amd_pstate_epp_suspend,
	.resume		= amd_pstate_epp_resume,
	.name		= "amd-pstate-epp",
	.attr		= amd_pstate_epp_attr,
};

static int __init amd_pstate_set_driver(int mode_idx)
{
	if (mode_idx >= AMD_PSTATE_DISABLE && mode_idx < AMD_PSTATE_MAX) {
		cppc_state = mode_idx;
		if (cppc_state == AMD_PSTATE_DISABLE)
			pr_info("driver is explicitly disabled\n");

		if (cppc_state == AMD_PSTATE_ACTIVE)
			current_pstate_driver = &amd_pstate_epp_driver;

		if (cppc_state == AMD_PSTATE_PASSIVE || cppc_state == AMD_PSTATE_GUIDED)
			current_pstate_driver = &amd_pstate_driver;

		return 0;
	}

	return -EINVAL;
}

static int __init amd_pstate_init(void)
{
	struct device *dev_root;
	int ret;

	if (boot_cpu_data.x86_vendor != X86_VENDOR_AMD)
		return -ENODEV;

	if (!acpi_cpc_valid()) {
		pr_warn_once("the _CPC object is not present in SBIOS or ACPI disabled\n");
		return -ENODEV;
	}

	/* don't keep reloading if cpufreq_driver exists */
	if (cpufreq_get_current_driver())
		return -EEXIST;

	switch (cppc_state) {
	case AMD_PSTATE_UNDEFINED:
		/* Disable on the following configs by default:
		 * 1. Undefined platforms
		 * 2. Server platforms
		 * 3. Shared memory designs
		 */
		if (amd_pstate_acpi_pm_profile_undefined() ||
		    amd_pstate_acpi_pm_profile_server() ||
		    !boot_cpu_has(X86_FEATURE_CPPC)) {
			pr_info("driver load is disabled, boot with specific mode to enable this\n");
			return -ENODEV;
		}
		ret = amd_pstate_set_driver(CONFIG_X86_AMD_PSTATE_DEFAULT_MODE);
		if (ret)
			return ret;
		break;
	case AMD_PSTATE_DISABLE:
		return -ENODEV;
	case AMD_PSTATE_PASSIVE:
	case AMD_PSTATE_ACTIVE:
	case AMD_PSTATE_GUIDED:
		break;
	default:
		return -EINVAL;
	}

	/* capability check */
	if (boot_cpu_has(X86_FEATURE_CPPC)) {
		pr_debug("AMD CPPC MSR based functionality is supported\n");
		if (cppc_state != AMD_PSTATE_ACTIVE)
			current_pstate_driver->adjust_perf = amd_pstate_adjust_perf;
	} else {
		pr_debug("AMD CPPC shared memory based functionality is supported\n");
		static_call_update(amd_pstate_enable, cppc_enable);
		static_call_update(amd_pstate_init_perf, cppc_init_perf);
		static_call_update(amd_pstate_update_perf, cppc_update_perf);
	}

	/* enable amd pstate feature */
	ret = amd_pstate_enable(true);
	if (ret) {
		pr_err("failed to enable with return %d\n", ret);
		return ret;
	}

	ret = cpufreq_register_driver(current_pstate_driver);
	if (ret)
		pr_err("failed to register with return %d\n", ret);

	dev_root = bus_get_dev_root(&cpu_subsys);
	if (dev_root) {
		ret = sysfs_create_group(&dev_root->kobj, &amd_pstate_global_attr_group);
		put_device(dev_root);
		if (ret) {
			pr_err("sysfs attribute export failed with error %d.\n", ret);
			goto global_attr_free;
		}
	}

	return ret;

global_attr_free:
	cpufreq_unregister_driver(current_pstate_driver);
	return ret;
}
device_initcall(amd_pstate_init);

static int __init amd_pstate_param(char *str)
{
	size_t size;
	int mode_idx;

	if (!str)
		return -EINVAL;

	size = strlen(str);
	mode_idx = get_mode_idx_from_str(str, size);

	return amd_pstate_set_driver(mode_idx);
}
early_param("amd_pstate", amd_pstate_param);

MODULE_AUTHOR("Huang Rui <ray.huang@amd.com>");
MODULE_DESCRIPTION("AMD Processor P-state Frequency Driver");
