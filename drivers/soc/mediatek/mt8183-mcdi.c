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
 * brought up or taken down. Cluster power-down also needs a per-entry
 * "cluster may power off" handshake that cpuidle-psci has no hook for, so
 * only CPU-level power-down states can be used with this.
 *
 * With every CPU powered down in suspend-to-idle only some wake sources get
 * through (the power key does, the RTC alarm doesn't), so power-down states
 * are kept off over system suspend as well.
 *
 * Copyright (c) 2026 Toshit Chawda <r58playz@gmail.com>
 */

#define pr_fmt(fmt) "mt8183-mcdi: " fmt

#include <linux/bits.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/cpuidle.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pm_qos.h>
#include <linux/suspend.h>

/* Mailbox slots, 32 bits each */
#define MCDI_BUCK_POWER_OFF_MASK	(2 * 4)
#define MCDI_AVAIL_CPU_MASK		(7 * 4)
#define MCDI_CPU_CLUSTER_PWR_STAT	(8 * 4)
#define MCDI_CPU_ISOLATION_MASK		(12 * 4)

#define MCDI_NR_CPUS			8
#define MCDI_CPUS_OFF			GENMASK(MCDI_NR_CPUS - 1, 0)

static void __iomem *mcdi_mbox;
static struct pm_qos_request mcdi_qos;

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
