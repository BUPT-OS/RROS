// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 - 2022, NVIDIA CORPORATION. All rights reserved
 */

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/units.h>

#include <asm/smp_plat.h>

#include <soc/tegra/bpmp.h>
#include <soc/tegra/bpmp-abi.h>

#define KHZ                     1000
#define REF_CLK_MHZ             408 /* 408 MHz */
#define US_DELAY                500
#define CPUFREQ_TBL_STEP_HZ     (50 * KHZ * KHZ)
#define MAX_CNT                 ~0U

#define NDIV_MASK              0x1FF

#define CORE_OFFSET(cpu)			(cpu * 8)
#define CMU_CLKS_BASE				0x2000
#define SCRATCH_FREQ_CORE_REG(data, cpu)	(data->regs + CMU_CLKS_BASE + CORE_OFFSET(cpu))

#define MMCRAB_CLUSTER_BASE(cl)			(0x30000 + (cl * 0x10000))
#define CLUSTER_ACTMON_BASE(data, cl) \
			(data->regs + (MMCRAB_CLUSTER_BASE(cl) + data->soc->actmon_cntr_base))
#define CORE_ACTMON_CNTR_REG(data, cl, cpu)	(CLUSTER_ACTMON_BASE(data, cl) + CORE_OFFSET(cpu))

/* cpufreq transisition latency */
#define TEGRA_CPUFREQ_TRANSITION_LATENCY (300 * 1000) /* unit in nanoseconds */

struct tegra_cpu_ctr {
	u32 cpu;
	u32 coreclk_cnt, last_coreclk_cnt;
	u32 refclk_cnt, last_refclk_cnt;
};

struct read_counters_work {
	struct work_struct work;
	struct tegra_cpu_ctr c;
};

struct tegra_cpufreq_ops {
	void (*read_counters)(struct tegra_cpu_ctr *c);
	void (*set_cpu_ndiv)(struct cpufreq_policy *policy, u64 ndiv);
	void (*get_cpu_cluster_id)(u32 cpu, u32 *cpuid, u32 *clusterid);
	int (*get_cpu_ndiv)(u32 cpu, u32 cpuid, u32 clusterid, u64 *ndiv);
};

struct tegra_cpufreq_soc {
	struct tegra_cpufreq_ops *ops;
	int maxcpus_per_cluster;
	unsigned int num_clusters;
	phys_addr_t actmon_cntr_base;
};

struct tegra194_cpufreq_data {
	void __iomem *regs;
	struct cpufreq_frequency_table **bpmp_luts;
	const struct tegra_cpufreq_soc *soc;
	bool icc_dram_bw_scaling;
};

static struct workqueue_struct *read_counters_wq;

static int tegra_cpufreq_set_bw(struct cpufreq_policy *policy, unsigned long freq_khz)
{
	struct tegra194_cpufreq_data *data = cpufreq_get_driver_data();
	struct dev_pm_opp *opp;
	struct device *dev;
	int ret;

	dev = get_cpu_device(policy->cpu);
	if (!dev)
		return -ENODEV;

	opp = dev_pm_opp_find_freq_exact(dev, freq_khz * KHZ, true);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	ret = dev_pm_opp_set_opp(dev, opp);
	if (ret)
		data->icc_dram_bw_scaling = false;

	dev_pm_opp_put(opp);
	return ret;
}

static void tegra_get_cpu_mpidr(void *mpidr)
{
	*((u64 *)mpidr) = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;
}

static void tegra234_get_cpu_cluster_id(u32 cpu, u32 *cpuid, u32 *clusterid)
{
	u64 mpidr;

	smp_call_function_single(cpu, tegra_get_cpu_mpidr, &mpidr, true);

	if (cpuid)
		*cpuid = MPIDR_AFFINITY_LEVEL(mpidr, 1);
	if (clusterid)
		*clusterid = MPIDR_AFFINITY_LEVEL(mpidr, 2);
}

static int tegra234_get_cpu_ndiv(u32 cpu, u32 cpuid, u32 clusterid, u64 *ndiv)
{
	struct tegra194_cpufreq_data *data = cpufreq_get_driver_data();
	void __iomem *freq_core_reg;
	u64 mpidr_id;

	/* use physical id to get address of per core frequency register */
	mpidr_id = (clusterid * data->soc->maxcpus_per_cluster) + cpuid;
	freq_core_reg = SCRATCH_FREQ_CORE_REG(data, mpidr_id);

	*ndiv = readl(freq_core_reg) & NDIV_MASK;

	return 0;
}

static void tegra234_set_cpu_ndiv(struct cpufreq_policy *policy, u64 ndiv)
{
	struct tegra194_cpufreq_data *data = cpufreq_get_driver_data();
	void __iomem *freq_core_reg;
	u32 cpu, cpuid, clusterid;
	u64 mpidr_id;

	for_each_cpu_and(cpu, policy->cpus, cpu_online_mask) {
		data->soc->ops->get_cpu_cluster_id(cpu, &cpuid, &clusterid);

		/* use physical id to get address of per core frequency register */
		mpidr_id = (clusterid * data->soc->maxcpus_per_cluster) + cpuid;
		freq_core_reg = SCRATCH_FREQ_CORE_REG(data, mpidr_id);

		writel(ndiv, freq_core_reg);
	}
}

/*
 * This register provides access to two counter values with a single
 * 64-bit read. The counter values are used to determine the average
 * actual frequency a core has run at over a period of time.
 *     [63:32] PLLP counter: Counts at fixed frequency (408 MHz)
 *     [31:0] Core clock counter: Counts on every core clock cycle
 */
static void tegra234_read_counters(struct tegra_cpu_ctr *c)
{
	struct tegra194_cpufreq_data *data = cpufreq_get_driver_data();
	void __iomem *actmon_reg;
	u32 cpuid, clusterid;
	u64 val;

	data->soc->ops->get_cpu_cluster_id(c->cpu, &cpuid, &clusterid);
	actmon_reg = CORE_ACTMON_CNTR_REG(data, clusterid, cpuid);

	val = readq(actmon_reg);
	c->last_refclk_cnt = upper_32_bits(val);
	c->last_coreclk_cnt = lower_32_bits(val);
	udelay(US_DELAY);
	val = readq(actmon_reg);
	c->refclk_cnt = upper_32_bits(val);
	c->coreclk_cnt = lower_32_bits(val);
}

static struct tegra_cpufreq_ops tegra234_cpufreq_ops = {
	.read_counters = tegra234_read_counters,
	.get_cpu_cluster_id = tegra234_get_cpu_cluster_id,
	.get_cpu_ndiv = tegra234_get_cpu_ndiv,
	.set_cpu_ndiv = tegra234_set_cpu_ndiv,
};

static const struct tegra_cpufreq_soc tegra234_cpufreq_soc = {
	.ops = &tegra234_cpufreq_ops,
	.actmon_cntr_base = 0x9000,
	.maxcpus_per_cluster = 4,
	.num_clusters = 3,
};

static const struct tegra_cpufreq_soc tegra239_cpufreq_soc = {
	.ops = &tegra234_cpufreq_ops,
	.actmon_cntr_base = 0x4000,
	.maxcpus_per_cluster = 8,
	.num_clusters = 1,
};

static void tegra194_get_cpu_cluster_id(u32 cpu, u32 *cpuid, u32 *clusterid)
{
	u64 mpidr;

	smp_call_function_single(cpu, tegra_get_cpu_mpidr, &mpidr, true);

	if (cpuid)
		*cpuid = MPIDR_AFFINITY_LEVEL(mpidr, 0);
	if (clusterid)
		*clusterid = MPIDR_AFFINITY_LEVEL(mpidr, 1);
}

/*
 * Read per-core Read-only system register NVFREQ_FEEDBACK_EL1.
 * The register provides frequency feedback information to
 * determine the average actual frequency a core has run at over
 * a period of time.
 *	[31:0] PLLP counter: Counts at fixed frequency (408 MHz)
 *	[63:32] Core clock counter: counts on every core clock cycle
 *			where the core is architecturally clocking
 */
static u64 read_freq_feedback(void)
{
	u64 val = 0;

	asm volatile("mrs %0, s3_0_c15_c0_5" : "=r" (val) : );

	return val;
}

static inline u32 map_ndiv_to_freq(struct mrq_cpu_ndiv_limits_response
				   *nltbl, u16 ndiv)
{
	return nltbl->ref_clk_hz / KHZ * ndiv / (nltbl->pdiv * nltbl->mdiv);
}

static void tegra194_read_counters(struct tegra_cpu_ctr *c)
{
	u64 val;

	val = read_freq_feedback();
	c->last_refclk_cnt = lower_32_bits(val);
	c->last_coreclk_cnt = upper_32_bits(val);
	udelay(US_DELAY);
	val = read_freq_feedback();
	c->refclk_cnt = lower_32_bits(val);
	c->coreclk_cnt = upper_32_bits(val);
}

static void tegra_read_counters(struct work_struct *work)
{
	struct tegra194_cpufreq_data *data = cpufreq_get_driver_data();
	struct read_counters_work *read_counters_work;
	struct tegra_cpu_ctr *c;

	/*
	 * ref_clk_counter(32 bit counter) runs on constant clk,
	 * pll_p(408MHz).
	 * It will take = 2 ^ 32 / 408 MHz to overflow ref clk counter
	 *              = 10526880 usec = 10.527 sec to overflow
	 *
	 * Like wise core_clk_counter(32 bit counter) runs on core clock.
	 * It's synchronized to crab_clk (cpu_crab_clk) which runs at
	 * freq of cluster. Assuming max cluster clock ~2000MHz,
	 * It will take = 2 ^ 32 / 2000 MHz to overflow core clk counter
	 *              = ~2.147 sec to overflow
	 */
	read_counters_work = container_of(work, struct read_counters_work,
					  work);
	c = &read_counters_work->c;

	data->soc->ops->read_counters(c);
}

/*
 * Return instantaneous cpu speed
 * Instantaneous freq is calculated as -
 * -Takes sample on every query of getting the freq.
 *	- Read core and ref clock counters;
 *	- Delay for X us
 *	- Read above cycle counters again
 *	- Calculates freq by subtracting current and previous counters
 *	  divided by the delay time or eqv. of ref_clk_counter in delta time
 *	- Return Kcycles/second, freq in KHz
 *
 *	delta time period = x sec
 *			  = delta ref_clk_counter / (408 * 10^6) sec
 *	freq in Hz = cycles/sec
 *		   = (delta cycles / x sec
 *		   = (delta cycles * 408 * 10^6) / delta ref_clk_counter
 *	in KHz	   = (delta cycles * 408 * 10^3) / delta ref_clk_counter
 *
 * @cpu - logical cpu whose freq to be updated
 * Returns freq in KHz on success, 0 if cpu is offline
 */
static unsigned int tegra194_calculate_speed(u32 cpu)
{
	struct read_counters_work read_counters_work;
	struct tegra_cpu_ctr c;
	u32 delta_refcnt;
	u32 delta_ccnt;
	u32 rate_mhz;

	/*
	 * udelay() is required to reconstruct cpu frequency over an
	 * observation window. Using workqueue to call udelay() with
	 * interrupts enabled.
	 */
	read_counters_work.c.cpu = cpu;
	INIT_WORK_ONSTACK(&read_counters_work.work, tegra_read_counters);
	queue_work_on(cpu, read_counters_wq, &read_counters_work.work);
	flush_work(&read_counters_work.work);
	c = read_counters_work.c;

	if (c.coreclk_cnt < c.last_coreclk_cnt)
		delta_ccnt = c.coreclk_cnt + (MAX_CNT - c.last_coreclk_cnt);
	else
		delta_ccnt = c.coreclk_cnt - c.last_coreclk_cnt;
	if (!delta_ccnt)
		return 0;

	/* ref clock is 32 bits */
	if (c.refclk_cnt < c.last_refclk_cnt)
		delta_refcnt = c.refclk_cnt + (MAX_CNT - c.last_refclk_cnt);
	else
		delta_refcnt = c.refclk_cnt - c.last_refclk_cnt;
	if (!delta_refcnt) {
		pr_debug("cpufreq: %d is idle, delta_refcnt: 0\n", cpu);
		return 0;
	}
	rate_mhz = ((unsigned long)(delta_ccnt * REF_CLK_MHZ)) / delta_refcnt;

	return (rate_mhz * KHZ); /* in KHz */
}

static void tegra194_get_cpu_ndiv_sysreg(void *ndiv)
{
	u64 ndiv_val;

	asm volatile("mrs %0, s3_0_c15_c0_4" : "=r" (ndiv_val) : );

	*(u64 *)ndiv = ndiv_val;
}

static int tegra194_get_cpu_ndiv(u32 cpu, u32 cpuid, u32 clusterid, u64 *ndiv)
{
	return smp_call_function_single(cpu, tegra194_get_cpu_ndiv_sysreg, &ndiv, true);
}

static void tegra194_set_cpu_ndiv_sysreg(void *data)
{
	u64 ndiv_val = *(u64 *)data;

	asm volatile("msr s3_0_c15_c0_4, %0" : : "r" (ndiv_val));
}

static void tegra194_set_cpu_ndiv(struct cpufreq_policy *policy, u64 ndiv)
{
	on_each_cpu_mask(policy->cpus, tegra194_set_cpu_ndiv_sysreg, &ndiv, true);
}

static unsigned int tegra194_get_speed(u32 cpu)
{
	struct tegra194_cpufreq_data *data = cpufreq_get_driver_data();
	struct cpufreq_frequency_table *pos;
	u32 cpuid, clusterid;
	unsigned int rate;
	u64 ndiv;
	int ret;

	data->soc->ops->get_cpu_cluster_id(cpu, &cpuid, &clusterid);

	/* reconstruct actual cpu freq using counters */
	rate = tegra194_calculate_speed(cpu);

	/* get last written ndiv value */
	ret = data->soc->ops->get_cpu_ndiv(cpu, cpuid, clusterid, &ndiv);
	if (WARN_ON_ONCE(ret))
		return rate;

	/*
	 * If the reconstructed frequency has acceptable delta from
	 * the last written value, then return freq corresponding
	 * to the last written ndiv value from freq_table. This is
	 * done to return consistent value.
	 */
	cpufreq_for_each_valid_entry(pos, data->bpmp_luts[clusterid]) {
		if (pos->driver_data != ndiv)
			continue;

		if (abs(pos->frequency - rate) > 115200) {
			pr_warn("cpufreq: cpu%d,cur:%u,set:%u,set ndiv:%llu\n",
				cpu, rate, pos->frequency, ndiv);
		} else {
			rate = pos->frequency;
		}
		break;
	}
	return rate;
}

static int tegra_cpufreq_init_cpufreq_table(struct cpufreq_policy *policy,
					    struct cpufreq_frequency_table *bpmp_lut,
					    struct cpufreq_frequency_table **opp_table)
{
	struct tegra194_cpufreq_data *data = cpufreq_get_driver_data();
	struct cpufreq_frequency_table *freq_table = NULL;
	struct cpufreq_frequency_table *pos;
	struct device *cpu_dev;
	struct dev_pm_opp *opp;
	unsigned long rate;
	int ret, max_opps;
	int j = 0;

	cpu_dev = get_cpu_device(policy->cpu);
	if (!cpu_dev) {
		pr_err("%s: failed to get cpu%d device\n", __func__, policy->cpu);
		return -ENODEV;
	}

	/* Initialize OPP table mentioned in operating-points-v2 property in DT */
	ret = dev_pm_opp_of_add_table_indexed(cpu_dev, 0);
	if (!ret) {
		max_opps = dev_pm_opp_get_opp_count(cpu_dev);
		if (max_opps <= 0) {
			dev_err(cpu_dev, "Failed to add OPPs\n");
			return max_opps;
		}

		/* Disable all opps and cross-validate against LUT later */
		for (rate = 0; ; rate++) {
			opp = dev_pm_opp_find_freq_ceil(cpu_dev, &rate);
			if (IS_ERR(opp))
				break;

			dev_pm_opp_put(opp);
			dev_pm_opp_disable(cpu_dev, rate);
		}
	} else {
		dev_err(cpu_dev, "Invalid or empty opp table in device tree\n");
		data->icc_dram_bw_scaling = false;
		return ret;
	}

	freq_table = kcalloc((max_opps + 1), sizeof(*freq_table), GFP_KERNEL);
	if (!freq_table)
		return -ENOMEM;

	/*
	 * Cross check the frequencies from BPMP-FW LUT against the OPP's present in DT.
	 * Enable only those DT OPP's which are present in LUT also.
	 */
	cpufreq_for_each_valid_entry(pos, bpmp_lut) {
		opp = dev_pm_opp_find_freq_exact(cpu_dev, pos->frequency * KHZ, false);
		if (IS_ERR(opp))
			continue;

		ret = dev_pm_opp_enable(cpu_dev, pos->frequency * KHZ);
		if (ret < 0)
			return ret;

		freq_table[j].driver_data = pos->driver_data;
		freq_table[j].frequency = pos->frequency;
		j++;
	}

	freq_table[j].driver_data = pos->driver_data;
	freq_table[j].frequency = CPUFREQ_TABLE_END;

	*opp_table = &freq_table[0];

	dev_pm_opp_set_sharing_cpus(cpu_dev, policy->cpus);

	return ret;
}

static int tegra194_cpufreq_init(struct cpufreq_policy *policy)
{
	struct tegra194_cpufreq_data *data = cpufreq_get_driver_data();
	int maxcpus_per_cluster = data->soc->maxcpus_per_cluster;
	struct cpufreq_frequency_table *freq_table;
	struct cpufreq_frequency_table *bpmp_lut;
	u32 start_cpu, cpu;
	u32 clusterid;
	int ret;

	data->soc->ops->get_cpu_cluster_id(policy->cpu, NULL, &clusterid);
	if (clusterid >= data->soc->num_clusters || !data->bpmp_luts[clusterid])
		return -EINVAL;

	start_cpu = rounddown(policy->cpu, maxcpus_per_cluster);
	/* set same policy for all cpus in a cluster */
	for (cpu = start_cpu; cpu < (start_cpu + maxcpus_per_cluster); cpu++) {
		if (cpu_possible(cpu))
			cpumask_set_cpu(cpu, policy->cpus);
	}
	policy->cpuinfo.transition_latency = TEGRA_CPUFREQ_TRANSITION_LATENCY;

	bpmp_lut = data->bpmp_luts[clusterid];

	if (data->icc_dram_bw_scaling) {
		ret = tegra_cpufreq_init_cpufreq_table(policy, bpmp_lut, &freq_table);
		if (!ret) {
			policy->freq_table = freq_table;
			return 0;
		}
	}

	data->icc_dram_bw_scaling = false;
	policy->freq_table = bpmp_lut;
	pr_info("OPP tables missing from DT, EMC frequency scaling disabled\n");

	return 0;
}

static int tegra194_cpufreq_online(struct cpufreq_policy *policy)
{
	/* We did light-weight tear down earlier, nothing to do here */
	return 0;
}

static int tegra194_cpufreq_offline(struct cpufreq_policy *policy)
{
	/*
	 * Preserve policy->driver_data and don't free resources on light-weight
	 * tear down.
	 */

	return 0;
}

static int tegra194_cpufreq_exit(struct cpufreq_policy *policy)
{
	struct device *cpu_dev = get_cpu_device(policy->cpu);

	dev_pm_opp_remove_all_dynamic(cpu_dev);
	dev_pm_opp_of_cpumask_remove_table(policy->related_cpus);

	return 0;
}

static int tegra194_cpufreq_set_target(struct cpufreq_policy *policy,
				       unsigned int index)
{
	struct cpufreq_frequency_table *tbl = policy->freq_table + index;
	struct tegra194_cpufreq_data *data = cpufreq_get_driver_data();

	/*
	 * Each core writes frequency in per core register. Then both cores
	 * in a cluster run at same frequency which is the maximum frequency
	 * request out of the values requested by both cores in that cluster.
	 */
	data->soc->ops->set_cpu_ndiv(policy, (u64)tbl->driver_data);

	if (data->icc_dram_bw_scaling)
		tegra_cpufreq_set_bw(policy, tbl->frequency);

	return 0;
}

static struct cpufreq_driver tegra194_cpufreq_driver = {
	.name = "tegra194",
	.flags = CPUFREQ_CONST_LOOPS | CPUFREQ_NEED_INITIAL_FREQ_CHECK |
		 CPUFREQ_IS_COOLING_DEV,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = tegra194_cpufreq_set_target,
	.get = tegra194_get_speed,
	.init = tegra194_cpufreq_init,
	.exit = tegra194_cpufreq_exit,
	.online = tegra194_cpufreq_online,
	.offline = tegra194_cpufreq_offline,
	.attr = cpufreq_generic_attr,
};

static struct tegra_cpufreq_ops tegra194_cpufreq_ops = {
	.read_counters = tegra194_read_counters,
	.get_cpu_cluster_id = tegra194_get_cpu_cluster_id,
	.get_cpu_ndiv = tegra194_get_cpu_ndiv,
	.set_cpu_ndiv = tegra194_set_cpu_ndiv,
};

static const struct tegra_cpufreq_soc tegra194_cpufreq_soc = {
	.ops = &tegra194_cpufreq_ops,
	.maxcpus_per_cluster = 2,
	.num_clusters = 4,
};

static void tegra194_cpufreq_free_resources(void)
{
	destroy_workqueue(read_counters_wq);
}

static struct cpufreq_frequency_table *
tegra_cpufreq_bpmp_read_lut(struct platform_device *pdev, struct tegra_bpmp *bpmp,
			    unsigned int cluster_id)
{
	struct cpufreq_frequency_table *freq_table;
	struct mrq_cpu_ndiv_limits_response resp;
	unsigned int num_freqs, ndiv, delta_ndiv;
	struct mrq_cpu_ndiv_limits_request req;
	struct tegra_bpmp_message msg;
	u16 freq_table_step_size;
	int err, index;

	memset(&req, 0, sizeof(req));
	req.cluster_id = cluster_id;

	memset(&msg, 0, sizeof(msg));
	msg.mrq = MRQ_CPU_NDIV_LIMITS;
	msg.tx.data = &req;
	msg.tx.size = sizeof(req);
	msg.rx.data = &resp;
	msg.rx.size = sizeof(resp);

	err = tegra_bpmp_transfer(bpmp, &msg);
	if (err)
		return ERR_PTR(err);
	if (msg.rx.ret == -BPMP_EINVAL) {
		/* Cluster not available */
		return NULL;
	}
	if (msg.rx.ret)
		return ERR_PTR(-EINVAL);

	/*
	 * Make sure frequency table step is a multiple of mdiv to match
	 * vhint table granularity.
	 */
	freq_table_step_size = resp.mdiv *
			DIV_ROUND_UP(CPUFREQ_TBL_STEP_HZ, resp.ref_clk_hz);

	dev_dbg(&pdev->dev, "cluster %d: frequency table step size: %d\n",
		cluster_id, freq_table_step_size);

	delta_ndiv = resp.ndiv_max - resp.ndiv_min;

	if (unlikely(delta_ndiv == 0)) {
		num_freqs = 1;
	} else {
		/* We store both ndiv_min and ndiv_max hence the +1 */
		num_freqs = delta_ndiv / freq_table_step_size + 1;
	}

	num_freqs += (delta_ndiv % freq_table_step_size) ? 1 : 0;

	freq_table = devm_kcalloc(&pdev->dev, num_freqs + 1,
				  sizeof(*freq_table), GFP_KERNEL);
	if (!freq_table)
		return ERR_PTR(-ENOMEM);

	for (index = 0, ndiv = resp.ndiv_min;
			ndiv < resp.ndiv_max;
			index++, ndiv += freq_table_step_size) {
		freq_table[index].driver_data = ndiv;
		freq_table[index].frequency = map_ndiv_to_freq(&resp, ndiv);
	}

	freq_table[index].driver_data = resp.ndiv_max;
	freq_table[index++].frequency = map_ndiv_to_freq(&resp, resp.ndiv_max);
	freq_table[index].frequency = CPUFREQ_TABLE_END;

	return freq_table;
}

static int tegra194_cpufreq_probe(struct platform_device *pdev)
{
	const struct tegra_cpufreq_soc *soc;
	struct tegra194_cpufreq_data *data;
	struct tegra_bpmp *bpmp;
	struct device *cpu_dev;
	int err, i;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	soc = of_device_get_match_data(&pdev->dev);

	if (soc->ops && soc->maxcpus_per_cluster && soc->num_clusters) {
		data->soc = soc;
	} else {
		dev_err(&pdev->dev, "soc data missing\n");
		return -EINVAL;
	}

	data->bpmp_luts = devm_kcalloc(&pdev->dev, data->soc->num_clusters,
				       sizeof(*data->bpmp_luts), GFP_KERNEL);
	if (!data->bpmp_luts)
		return -ENOMEM;

	if (soc->actmon_cntr_base) {
		/* mmio registers are used for frequency request and re-construction */
		data->regs = devm_platform_ioremap_resource(pdev, 0);
		if (IS_ERR(data->regs))
			return PTR_ERR(data->regs);
	}

	platform_set_drvdata(pdev, data);

	bpmp = tegra_bpmp_get(&pdev->dev);
	if (IS_ERR(bpmp))
		return PTR_ERR(bpmp);

	read_counters_wq = alloc_workqueue("read_counters_wq", __WQ_LEGACY, 1);
	if (!read_counters_wq) {
		dev_err(&pdev->dev, "fail to create_workqueue\n");
		err = -EINVAL;
		goto put_bpmp;
	}

	for (i = 0; i < data->soc->num_clusters; i++) {
		data->bpmp_luts[i] = tegra_cpufreq_bpmp_read_lut(pdev, bpmp, i);
		if (IS_ERR(data->bpmp_luts[i])) {
			err = PTR_ERR(data->bpmp_luts[i]);
			goto err_free_res;
		}
	}

	tegra194_cpufreq_driver.driver_data = data;

	/* Check for optional OPPv2 and interconnect paths on CPU0 to enable ICC scaling */
	cpu_dev = get_cpu_device(0);
	if (!cpu_dev) {
		err = -EPROBE_DEFER;
		goto err_free_res;
	}

	if (dev_pm_opp_of_get_opp_desc_node(cpu_dev)) {
		err = dev_pm_opp_of_find_icc_paths(cpu_dev, NULL);
		if (!err)
			data->icc_dram_bw_scaling = true;
	}

	err = cpufreq_register_driver(&tegra194_cpufreq_driver);
	if (!err)
		goto put_bpmp;

err_free_res:
	tegra194_cpufreq_free_resources();
put_bpmp:
	tegra_bpmp_put(bpmp);
	return err;
}

static void tegra194_cpufreq_remove(struct platform_device *pdev)
{
	cpufreq_unregister_driver(&tegra194_cpufreq_driver);
	tegra194_cpufreq_free_resources();
}

static const struct of_device_id tegra194_cpufreq_of_match[] = {
	{ .compatible = "nvidia,tegra194-ccplex", .data = &tegra194_cpufreq_soc },
	{ .compatible = "nvidia,tegra234-ccplex-cluster", .data = &tegra234_cpufreq_soc },
	{ .compatible = "nvidia,tegra239-ccplex-cluster", .data = &tegra239_cpufreq_soc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tegra194_cpufreq_of_match);

static struct platform_driver tegra194_ccplex_driver = {
	.driver = {
		.name = "tegra194-cpufreq",
		.of_match_table = tegra194_cpufreq_of_match,
	},
	.probe = tegra194_cpufreq_probe,
	.remove_new = tegra194_cpufreq_remove,
};
module_platform_driver(tegra194_ccplex_driver);

MODULE_AUTHOR("Mikko Perttunen <mperttunen@nvidia.com>");
MODULE_AUTHOR("Sumit Gupta <sumitg@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA Tegra194 cpufreq driver");
MODULE_LICENSE("GPL v2");
