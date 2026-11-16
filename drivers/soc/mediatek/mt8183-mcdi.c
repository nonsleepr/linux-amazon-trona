// SPDX-License-Identifier: GPL-2.0
/*
 * MT8183 MCDI controller setup, for MediaTek's vendor TF-A
 *
 * On MT8183 the CPU power-down that PSCI CPU_SUSPEND idle states ask for is
 * carried out by the MCDI ("multi-core deep idle") task on the SSPM, which
 * the kernel talks to through SSPM mailbox 3. Chromebook TF-A drives it from
 * EL3. MediaTek's vendor TF-A, as found on Amazon's tablets, leaves part of
 * that to the kernel: until the mask of available CPUs is written to the
 * mailbox, the SSPM ignores CPUs asking to be powered down, a CPU entering
 * a power-down idle state never comes back, and the system hangs.
 *
 * Do what MediaTek's kernel driver does for CPU power-down: publish the
 * online CPUs (and clear the isolation and buck-off masks) before cpuidle
 * starts, and keep CPUs out of power-down idle states while a CPU is
 * brought up or taken down. CPU_OFF doesn't power a hotplugged CPU down
 * with this firmware either: like MediaTek's kernel, ask for that with a
 * SiP call once the CPU is dead, or CPU_ON never brings it back, and for
 * the cluster once its last CPU is.
 *
 * Cluster power-down needs one more thing from the kernel: the last CPU of
 * a cluster to go idle tells the controller that the cluster may power off
 * ("CAN_POWER_OFF"). TF-A still only prepares the cluster, and flags that
 * to the controller, when PSCI coordination has every CPU of the cluster
 * asking for the cluster state. cpuidle-psci has no hook for that, so the
 * cluster states' enter callbacks are wrapped here once they exist. Until
 * then a latency limit keeps CPUs out of them. mt8183_mcdi.cluster_off=0
 * starts them disabled (per CPU, from sysfs) instead.
 *
 * With every CPU powered down in suspend-to-idle only some wake sources get
 * through (the power key does, the RTC alarm doesn't), so power-down states
 * are kept off over system suspend as well.
 *
 * Copyright (c) 2026 Toshit Chawda <r58playz@gmail.com>
 */

#define pr_fmt(fmt) "mt8183-mcdi: " fmt

#include <linux/arm-smccc.h>
#include <linux/bits.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/cpuidle.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pm_qos.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/topology.h>

#define MTK_SIP_POWER_DOWN_CLUSTER	0xc2000215
#define MTK_SIP_POWER_DOWN_CORE		0xc2000218

/* Mailbox slots, 32 bits each */
#define MCDI_CLUSTER_CAN_POWER_OFF(c)	((0 + (c)) * 4)
#define MCDI_BUCK_POWER_OFF_MASK	(2 * 4)
#define MCDI_AVAIL_CPU_MASK		(7 * 4)
#define MCDI_CPU_CLUSTER_PWR_STAT	(8 * 4)
#define MCDI_CPU_ISOLATION_MASK		(12 * 4)

#define MCDI_NR_CPUS			8
#define MCDI_NR_CLUSTERS		2
#define MCDI_CPUS_OFF			GENMASK(MCDI_NR_CPUS - 1, 0)

/* Between CPU (200 us) and cluster (400 us) exit latency */
#define MCDI_NO_CLUSTER_LATENCY_US	300

static void __iomem *mcdi_mbox;
static struct pm_qos_request mcdi_qos;
static struct pm_qos_request mcdi_boot_qos;

static bool cluster_off = true;
module_param(cluster_off, bool, 0444);
MODULE_PARM_DESC(cluster_off, "Enable the cluster power-down idle states at boot (default: on)");

static int mcdi_cluster_of[MCDI_NR_CPUS];
static atomic_t mcdi_cluster_idle[MCDI_NR_CLUSTERS];
static int (*mcdi_psci_enter)(struct cpuidle_device *dev,
			      struct cpuidle_driver *drv, int idx);

static void mcdi_publish_online_cpus(void)
{
	writel(cpumask_bits(cpu_online_mask)[0] & GENMASK(MCDI_NR_CPUS - 1, 0),
	       mcdi_mbox + MCDI_AVAIL_CPU_MASK);
}

/* Keep CPUs out of power-down idle while CPUs come and go */
static void mcdi_hold(void)
{
	u32 stat;

	/* A zero latency limit wakes all idle CPUs and keeps them in WFI */
	cpu_latency_qos_update_request(&mcdi_qos, 0);

	if (readl_poll_timeout(mcdi_mbox + MCDI_CPU_CLUSTER_PWR_STAT, stat,
			       !(stat & MCDI_CPUS_OFF), 10, 10000))
		pr_warn("CPUs still powered down: %#x\n", stat);
}

static void mcdi_release(void)
{
	mcdi_publish_online_cpus();
	cpu_latency_qos_update_request(&mcdi_qos, PM_QOS_DEFAULT_VALUE);
}

/* Bring-up: hold on the control CPU, release once the new CPU is online */
static int mcdi_cpu_prepare(unsigned int cpu)
{
	mcdi_hold();
	return 0;
}

static int mcdi_cpu_online(unsigned int cpu)
{
	mcdi_release();
	return 0;
}

/* Take-down: hold on the outgoing CPU, release once it is dead */
static int mcdi_cpu_offline(unsigned int cpu)
{
	mcdi_hold();
	return 0;
}

static int mcdi_cpu_dead(unsigned int cpu)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_POWER_DOWN_CORE, cpu, 0, 0, 0, 0, 0, 0, &res);
	if (res.a0)
		pr_warn("CPU%u: power-down call failed: %ld\n", cpu, (long)res.a0);

	/*
	 * The last CPU of a cluster: power the cluster down too, or it stays
	 * on (and the SPM never gets to system suspend). CPU_ON brings it
	 * back by itself.
	 */
	if (!cpumask_intersects(topology_cluster_cpumask(cpu), cpu_online_mask)) {
		arm_smccc_smc(MTK_SIP_POWER_DOWN_CLUSTER, topology_cluster_id(cpu),
			      0, 0, 0, 0, 0, 0, &res);
		if (res.a0)
			pr_warn("cluster %d: power-down call failed: %ld\n",
				topology_cluster_id(cpu), (long)res.a0);
	}

	mcdi_release();
	return 0;
}

/* Disable or re-enable every idle state past WFI on all CPUs */
static void mcdi_power_down_states(bool enable)
{
	struct cpuidle_driver *drv, *done = NULL;
	unsigned int cpu;
	int i;

	for_each_possible_cpu(cpu) {
		drv = cpuidle_get_cpu_driver(per_cpu(cpuidle_devices, cpu));
		if (!drv || drv == done)
			continue;
		for (i = 1; i < drv->state_count; i++)
			cpuidle_driver_state_disabled(drv, i, !enable);
		done = drv;
	}
}

static int __cpuidle mcdi_enter_cluster_state(struct cpuidle_device *dev,
					      struct cpuidle_driver *drv, int idx)
{
	int cluster = mcdi_cluster_of[dev->cpu];
	atomic_t *idle = &mcdi_cluster_idle[cluster];
	int ret;

	/* The last CPU of the cluster in lets the controller power it off */
	if (atomic_inc_return(idle) ==
	    cpumask_weight_and(cpu_online_mask, topology_cluster_cpumask(dev->cpu)))
		writel(1, mcdi_mbox + MCDI_CLUSTER_CAN_POWER_OFF(cluster));

	ret = mcdi_psci_enter(dev, drv, idx);

	atomic_dec(idle);

	return ret;
}

/* Wrap the cluster states of one CPU's cpuidle driver */
static int mcdi_hook_cluster_states(unsigned int cpu)
{
	struct cpuidle_device *dev = per_cpu(cpuidle_devices, cpu);
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);
	int cluster = topology_cluster_id(cpu);
	int i, n = 0;

	if (!dev || !drv || cpu >= MCDI_NR_CPUS ||
	    cluster < 0 || cluster >= MCDI_NR_CLUSTERS)
		return 0;

	mcdi_cluster_of[cpu] = cluster;

	for (i = 1; i < drv->state_count; i++) {
		struct cpuidle_state *state = &drv->states[i];

		if (!strstarts(state->name, "cluster-"))
			continue;

		if (state->enter != mcdi_enter_cluster_state) {
			if (mcdi_psci_enter && state->enter != mcdi_psci_enter) {
				pr_warn("CPU%u %s: unexpected enter callback\n",
					cpu, state->name);
				continue;
			}
			mcdi_psci_enter = state->enter;
			WRITE_ONCE(state->enter, mcdi_enter_cluster_state);
		}

		if (!cluster_off)
			dev->states_usage[i].disable |= CPUIDLE_STATE_DISABLED_BY_USER;
		n++;
	}

	return n;
}

static int mcdi_pm_notifier(struct notifier_block *nb, unsigned long action,
			    void *data)
{
	switch (action) {
	case PM_SUSPEND_PREPARE:
		mcdi_power_down_states(false);
		break;
	case PM_POST_SUSPEND:
		mcdi_power_down_states(true);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block mcdi_pm_nb = {
	.notifier_call = mcdi_pm_notifier,
};

/*
 * Before device_initcall, where cpuidle-psci registers the idle states:
 * the mailbox has to be set up before any CPU enters one.
 */
static int __init mt8183_mcdi_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt8183-mcdi-mbox");
	if (!np)
		return 0;
	if (!of_device_is_available(np)) {
		of_node_put(np);
		return 0;
	}

	mcdi_mbox = of_iomap(np, 0);
	of_node_put(np);
	if (!mcdi_mbox)
		return -ENOMEM;

	cpu_latency_qos_add_request(&mcdi_qos, PM_QOS_DEFAULT_VALUE);
	/* No cluster states until their enter callbacks are wrapped */
	cpu_latency_qos_add_request(&mcdi_boot_qos, MCDI_NO_CLUSTER_LATENCY_US);

	cpus_read_lock();
	writel(0, mcdi_mbox + MCDI_CPU_ISOLATION_MASK);
	writel(0, mcdi_mbox + MCDI_BUCK_POWER_OFF_MASK);
	mcdi_publish_online_cpus();

	ret = cpuhp_setup_state_nocalls_cpuslocked(CPUHP_BP_PREPARE_DYN,
						   "soc/mt8183-mcdi:prepare",
						   mcdi_cpu_prepare, mcdi_cpu_dead);
	if (ret >= 0)
		ret = cpuhp_setup_state_nocalls_cpuslocked(CPUHP_AP_ONLINE_DYN,
							   "soc/mt8183-mcdi:online",
							   mcdi_cpu_online,
							   mcdi_cpu_offline);
	cpus_read_unlock();
	if (ret < 0) {
		pr_err("failed to set up CPU hotplug callbacks: %d\n", ret);
		return ret;
	}

	register_pm_notifier(&mcdi_pm_nb);

	pr_info("available CPUs %#x published to the MCDI controller\n",
		readl(mcdi_mbox + MCDI_AVAIL_CPU_MASK));
	return 0;
}
subsys_initcall(mt8183_mcdi_init);

/* After cpuidle-psci (subsys_initcall) has registered the idle states */
static int __init mt8183_mcdi_cluster_init(void)
{
	unsigned int cpu;
	int n = 0;

	if (!mcdi_mbox)
		return 0;

	for_each_possible_cpu(cpu)
		n += mcdi_hook_cluster_states(cpu);

	cpu_latency_qos_remove_request(&mcdi_boot_qos);

	if (n)
		pr_info("cluster power-down: %d idle states, %s\n", n,
			cluster_off ? "enabled" : "disabled (enable from sysfs)");
	return 0;
}
late_initcall(mt8183_mcdi_cluster_init);
