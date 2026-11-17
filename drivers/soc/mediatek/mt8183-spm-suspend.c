// SPDX-License-Identifier: GPL-2.0
/*
 * MT8183 system suspend for MediaTek's vendor TF-A
 *
 * Upstream TF-A implements PSCI SYSTEM_SUSPEND for MT8183 on its own: it
 * configures the SPM and tells the SSPM. MediaTek's vendor TF-A, as found
 * on Amazon's tablets, expects the kernel to do that part, as MediaTek's
 * kernel does; called bare, its SYSTEM_SUSPEND never returns. So, like
 * MediaTek's kernel:
 *
 *  - at boot, tell TF-A which SPM firmware matches the DRAM, and start it:
 *    MediaTek's kernel does that from its Vcore DVFS code, and with the
 *    SPM's firmware stopped TF-A's suspend never completes;
 *  - service the SPM's interrupt (and so set up its polarity in the
 *    sysirq inverter; unrequested, the GIC sees it pending for good);
 *  - around suspend to RAM, send the SSPM SPM_SUSPEND_PREPARE /
 *    SPM_POST_SUSPEND (SSPM IPI 6, which lives in SSPM mailbox 1);
 *  - pause the SSPM's MCDI task, arm the SPM's PCM watchdog, send the SSPM
 *    SPM_SUSPEND, pass the suspend flags and wake timer to TF-A, mask every
 *    SPI but the SPM's, and enter the SoC's suspend state with PSCI
 *    CPU_SUSPEND; undo it all on resume.
 *
 * The IPI is a plain mailbox write and poll: data in the pin's slots, the
 * pin's bit in the mailbox's IN register raises the SSPM's interrupt, and
 * the SSPM answers with the same bit in the OUT register (write 1 to clear)
 * and the return value in the pin's first slot.
 *
 * Bring-up aids, all from the DT: the entry method (CPU_SUSPEND, PSCI
 * SYSTEM_SUSPEND, MediaTek's "legacy sleep" SMC with the CPU left powered,
 * or none at all), switches for the individual steps, and breadcrumbs in a
 * piece of RAM that survives a reset, reported at the next boot.
 *
 * Copyright (c) 2026 Toshit Chawda <r58playz@gmail.com>
 */

#define pr_fmt(fmt) "mt8183-spm: " fmt

#include <linux/arm-smccc.h>
#include <linux/bits.h>
#include <linux/cpuhotplug.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/psci.h>
#include <linux/sched/clock.h>
#include <linux/suspend.h>
#include <uapi/linux/psci.h>

#include <asm/cacheflush.h>
#include <asm/suspend.h>
#include <clocksource/arm_arch_timer.h>

#define MTK_SIP_KERNEL_SPM_SUSPEND_ARGS		0xc200021a
#define MTK_SIP_KERNEL_SPM_FIRMWARE_STATUS	0xc200021b
#define MTK_SIP_KERNEL_SPM_IRQ0_HANDLER		0xc200021c
#define MTK_SIP_KERNEL_SPM_LEGACY_SLEEP		0xc200021f
#define MTK_SIP_KERNEL_SPM_VCOREFS_ARGS		0xc2000220
#define MTK_SIP_KERNEL_SPM_ARGS			0xc2000228
#define MTK_SIP_KERNEL_SPM_GET_PWR_CTRL_ARGS	0xc2000229

/* MTK_SIP_KERNEL_SPM_ARGS commands (MT8183 numbering) */
#define SPM_ARGS_SPMFW_IDX		0
#define SPM_ARGS_SUSPEND		2
#define SPM_ARGS_SUSPEND_FINISH		3
#define SPM_ARGS_PCM_WDT		8

/* MTK_SIP_KERNEL_SPM_VCOREFS_ARGS commands */
#define VCOREFS_SMC_CMD_FW_INIT		0	/* current Vcore OPP */
#define VCOREFS_SMC_CMD_FW_RUN		1	/* SPM flags */
#define VCORE_BOOT_UP_OPP		0

/* SPM_FLAG_RUN_COMMON_SCENARIO | SPM_FLAG_DIS_VCORE_DVS | _DFS */
#define SPM_FW_FLAGS_NO_DVFS		0x418

/* SPM registers */
#define SPM_IRQ_STA			0x158

/* SPM_GET_PWR_CTRL_ARGS: scenario and field */
#define SPM_PWR_CTRL_SUSPEND		17
#define PWR_WAKE_SRC			12

/* The "suspend" state of MediaTek's DT: system level, power down, id 5 */
#define MT8183_PSCI_SUSPEND		0x01010005

#define PCM_WDT_TIMEOUT_S		30

#define GIC_MAX_SPI_REGS		32

/* SSPM IPI 6 (SPM suspend): mailbox 1, slots 2-9, IRQ bit 1 */
#define SSPM_SPM_SLOT			2
#define SSPM_SPM_WORDS			8
#define SSPM_SPM_IRQ			BIT(1)
#define SSPM_MBOX_IN_IRQ		0x0
#define SSPM_MBOX_OUT_IRQ		0x4
#define SSPM_ACK_TIMEOUT_US		2000000

/* SSPM commands */
#define SPM_SUSPEND			0
#define SPM_RESUME			1
#define SPM_SUSPEND_PREPARE		8
#define SPM_POST_SUSPEND		9

/* MCDI mailbox (SSPM mailbox 3) slots */
#define MCDI_PAUSE_ACTION		(6 * 4)
#define MCDI_PAUSE_ACK			(13 * 4)
#define MCDI_PAUSE_TIMEOUT_US		100000

/* "mediatek,suspend-method" */
enum {
	METHOD_CPU_SUSPEND,
	METHOD_SYSTEM_SUSPEND,
	METHOD_LEGACY_SLEEP,
	METHOD_NONE,
};

/*
 * Breadcrumbs: word 0 the magic, word 1 the last step reached, word 2 the
 * CPUs that went down during the suspend, words 4 + step a value per step.
 */
#define CRUMB_MAGIC			0x53504d43	/* "SPMC" */
#define CRUMB_WORDS			32
#define CRUMB_LAST			1
#define CRUMB_CPUS_DEAD			2
#define CRUMB_VAL(step)			(4 + (step))

enum {
	STEP_BEGIN = 1,
	STEP_PREPARED,
	STEP_ENTER,
	STEP_MCDI_PAUSED,
	STEP_WDT_ARMED,
	STEP_SSPM_SUSPEND,
	STEP_SUSPEND_ARGS,
	STEP_GIC_MASKED,
	STEP_FIRMWARE_CALL,
	STEP_FIRMWARE_RETURN,
	STEP_GIC_RESTORED,
	STEP_SSPM_RESUME,
	STEP_WDT_OFF,
	STEP_MCDI_RESUMED,
	STEP_END,
};

struct mt8183_spm {
	void __iomem *mbox;
	void __iomem *mbox_irq;
	void __iomem *mcdi;
	void __iomem *spm_base;
	u32 pcm_flags;
	u32 pcm_flags1;
	u32 timer_val;
	u32 psci_state;
	u32 method;
	bool no_gic_mask;
	bool no_mcdi_pause;
	bool no_sspm;
	bool prepared;
	/* GIC distributor, and the SPM's interrupt (INTID) left enabled */
	void __iomem *gicd;
	u32 spm_irq;
	int gic_regs;
	u32 gic_enabled[GIC_MAX_SPI_REGS];
	u32 *crumbs;
};

static struct mt8183_spm spm;

static void mt8183_spm_crumb_flush(void)
{
	dcache_clean_poc((unsigned long)spm.crumbs,
			 (unsigned long)(spm.crumbs + CRUMB_WORDS));
}

static void mt8183_spm_crumb(int step, u32 val)
{
	if (!spm.crumbs)
		return;

	spm.crumbs[0] = CRUMB_MAGIC;
	spm.crumbs[CRUMB_LAST] = step;
	spm.crumbs[CRUMB_VAL(step)] = val;
	mt8183_spm_crumb_flush();
}

/* Note which CPUs went down on the way into suspend */
static int mt8183_spm_cpu_dead(unsigned int cpu)
{
	if (spm.crumbs && spm.prepared) {
		spm.crumbs[CRUMB_CPUS_DEAD] |= BIT(cpu);
		mt8183_spm_crumb_flush();
	}
	return 0;
}

static unsigned long mt8183_spm_smc(unsigned long fid, unsigned long a1,
				    unsigned long a2, unsigned long a3)
{
	struct arm_smccc_res res;

	arm_smccc_smc(fid, a1, a2, a3, 0, 0, 0, 0, &res);
	return res.a0;
}

/* As MediaTek's spm_irq0_handler(): TF-A clears the status we pass back */
static irqreturn_t mt8183_spm_irq(int irq, void *data)
{
	u32 isr = readl(spm.spm_base + SPM_IRQ_STA);

	mt8183_spm_smc(MTK_SIP_KERNEL_SPM_IRQ0_HANDLER, isr, 0, 0);
	return IRQ_HANDLED;
}

/* The GIC INTID behind a Linux interrupt (under the sysirq inverter) */
static u32 mt8183_spm_irq_intid(unsigned int irq)
{
	struct irq_data *d = irq_get_irq_data(irq);

	while (d && d->parent_data)
		d = d->parent_data;
	return d ? irqd_to_hwirq(d) : 0;
}

/* Send one SPM command to the SSPM and wait for its answer */
static int mt8183_spm_sspm_cmd(u32 cmd, const u32 *args, int nargs)
{
	void __iomem *data = spm.mbox + SSPM_SPM_SLOT * 4;
	u32 out;
	int i, ret;

	if (spm.no_sspm)
		return 0;

	if (readl(spm.mbox_irq + SSPM_MBOX_OUT_IRQ) & SSPM_SPM_IRQ) {
		pr_err("SSPM mailbox busy\n");
		return -EBUSY;
	}

	writel(cmd, data);
	for (i = 1; i < SSPM_SPM_WORDS; i++)
		writel(i <= nargs ? args[i - 1] : 0, data + i * 4);
	writel(SSPM_SPM_IRQ, spm.mbox_irq + SSPM_MBOX_IN_IRQ);

	ret = readl_poll_timeout_atomic(spm.mbox_irq + SSPM_MBOX_OUT_IRQ, out,
					out & SSPM_SPM_IRQ, 1,
					SSPM_ACK_TIMEOUT_US);
	if (ret) {
		pr_err("SSPM didn't answer command %u\n", cmd);
		return ret;
	}
	writel(SSPM_SPM_IRQ, spm.mbox_irq + SSPM_MBOX_OUT_IRQ);

	ret = (s32)readl(data);
	if (ret < 0)
		pr_err("SSPM failed command %u: %d\n", cmd, ret);
	return ret < 0 ? ret : 0;
}

/* SPM_SUSPEND and SPM_RESUME carry the time, for the SSPM's timesync */
static int mt8183_spm_sspm_cmd_time(u32 cmd)
{
	u64 ns = sched_clock(), cnt = arch_timer_read_counter();
	u32 args[] = { lower_32_bits(ns), upper_32_bits(ns),
		       lower_32_bits(cnt), upper_32_bits(cnt), 0 };

	return mt8183_spm_sspm_cmd(cmd, args, ARRAY_SIZE(args));
}

/* Stop (or restart) the SSPM's MCDI task, as MediaTek's kernel does */
static int mt8183_spm_mcdi_pause(bool pause)
{
	u32 ack;
	int ret;

	if (!spm.mcdi || spm.no_mcdi_pause)
		return 0;

	writel(pause, spm.mcdi + MCDI_PAUSE_ACTION);
	ret = readl_poll_timeout_atomic(spm.mcdi + MCDI_PAUSE_ACK, ack,
					ack == pause, 1, MCDI_PAUSE_TIMEOUT_US);
	if (ret)
		pr_err("MCDI task didn't %s\n", pause ? "pause" : "resume");
	return ret;
}

/*
 * As MediaTek's kernel does (mt_irq_mask_all()): with every SPI but the
 * SPM's masked, nothing pulls CPU0 out of TF-A's power-down WFI loop, which
 * the SPM waits for before taking the system down.
 */
static void mt8183_spm_gic_mask(void)
{
	int i;

	if (!spm.gicd || spm.no_gic_mask)
		return;

	for (i = 1; i < spm.gic_regs; i++) {
		spm.gic_enabled[i] = readl(spm.gicd + GICD_ISENABLER + i * 4);
		writel(~0, spm.gicd + GICD_ICENABLER + i * 4);
	}
	if (spm.spm_irq)
		writel(BIT(spm.spm_irq % 32),
		       spm.gicd + GICD_ISENABLER + spm.spm_irq / 32 * 4);
}

static void mt8183_spm_gic_restore(void)
{
	int i;

	if (!spm.gicd || spm.no_gic_mask)
		return;

	for (i = 1; i < spm.gic_regs; i++) {
		writel(~spm.gic_enabled[i], spm.gicd + GICD_ICENABLER + i * 4);
		writel(spm.gic_enabled[i], spm.gicd + GICD_ISENABLER + i * 4);
	}
}

static int mt8183_spm_system_suspend_finisher(unsigned long arg)
{
	struct arm_smccc_res res;

	arm_smccc_smc(PSCI_1_0_FN64_SYSTEM_SUSPEND, __pa_symbol(cpu_resume),
		      0, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

static int mt8183_spm_firmware_suspend(void)
{
	switch (spm.method) {
	case METHOD_CPU_SUSPEND:
		return psci_cpu_suspend_enter(spm.psci_state);
	case METHOD_SYSTEM_SUSPEND:
		return cpu_suspend(0, mt8183_spm_system_suspend_finisher);
	case METHOD_LEGACY_SLEEP:
		/* The CPU stays powered; needs SPM_FLAG_DIS_CPU/INFRA_PDN */
		mt8183_spm_smc(MTK_SIP_KERNEL_SPM_ARGS, SPM_ARGS_SUSPEND, 0, 0);
		mt8183_spm_smc(MTK_SIP_KERNEL_SPM_LEGACY_SLEEP, 0, 0, 0);
		mt8183_spm_smc(MTK_SIP_KERNEL_SPM_ARGS, SPM_ARGS_SUSPEND_FINISH, 0, 0);
		return 0;
	default:
		return 0;
	}
}

/* Only suspend to RAM goes through the SPM (suspend-to-idle never gets here) */
static int mt8183_spm_suspend_begin(suspend_state_t state)
{
	int ret;

	if (state != PM_SUSPEND_MEM)
		return 0;

	if (spm.crumbs) {
		memset(spm.crumbs, 0, CRUMB_WORDS * sizeof(u32));
		mt8183_spm_crumb(STEP_BEGIN, spm.method);
	}

	ret = mt8183_spm_sspm_cmd(SPM_SUSPEND_PREPARE, NULL, 0);
	mt8183_spm_crumb(STEP_PREPARED, ret);
	if (ret)
		return ret;

	spm.prepared = true;
	pm_set_suspend_via_firmware();
	return 0;
}

static void mt8183_spm_suspend_end(void)
{
	int ret;

	if (!spm.prepared)
		return;

	ret = mt8183_spm_sspm_cmd(SPM_POST_SUSPEND, NULL, 0);
	mt8183_spm_crumb(STEP_END, ret);
	spm.prepared = false;
}

static int mt8183_spm_suspend_enter(suspend_state_t state)
{
	int ret;

	mt8183_spm_crumb(STEP_ENTER, spm.crumbs ? spm.crumbs[CRUMB_CPUS_DEAD] : 0);
	pm_set_resume_via_firmware();

	ret = mt8183_spm_mcdi_pause(true);
	mt8183_spm_crumb(STEP_MCDI_PAUSED, ret);
	if (ret) {
		mt8183_spm_mcdi_pause(false);
		return ret;
	}

	/* Reset rather than hang if the SPM doesn't bring the system back */
	ret = mt8183_spm_smc(MTK_SIP_KERNEL_SPM_ARGS, SPM_ARGS_PCM_WDT, 1,
			     PCM_WDT_TIMEOUT_S);
	mt8183_spm_crumb(STEP_WDT_ARMED, ret);

	ret = mt8183_spm_sspm_cmd_time(SPM_SUSPEND);
	mt8183_spm_crumb(STEP_SSPM_SUSPEND, ret);
	ret = mt8183_spm_smc(MTK_SIP_KERNEL_SPM_SUSPEND_ARGS, spm.pcm_flags,
			     spm.pcm_flags1, spm.timer_val);
	mt8183_spm_crumb(STEP_SUSPEND_ARGS, ret);

	mt8183_spm_gic_mask();
	mt8183_spm_crumb(STEP_GIC_MASKED, spm.gic_regs);
	mt8183_spm_crumb(STEP_FIRMWARE_CALL, spm.method);
	ret = mt8183_spm_firmware_suspend();
	mt8183_spm_crumb(STEP_FIRMWARE_RETURN, ret);
	mt8183_spm_gic_restore();
	mt8183_spm_crumb(STEP_GIC_RESTORED, 0);
	pr_debug("back from suspend (method %u): %d\n", spm.method, ret);

	mt8183_spm_crumb(STEP_SSPM_RESUME, mt8183_spm_sspm_cmd_time(SPM_RESUME));
	mt8183_spm_crumb(STEP_WDT_OFF,
			 mt8183_spm_smc(MTK_SIP_KERNEL_SPM_ARGS, SPM_ARGS_PCM_WDT, 0, 0));
	mt8183_spm_crumb(STEP_MCDI_RESUMED, mt8183_spm_mcdi_pause(false));

	return ret;
}

static const struct platform_suspend_ops mt8183_spm_suspend_ops = {
	.valid = suspend_valid_only_mem,
	.begin = mt8183_spm_suspend_begin,
	.enter = mt8183_spm_suspend_enter,
	.end = mt8183_spm_suspend_end,
};

static void __init mt8183_spm_crumbs_init(struct device_node *np)
{
	u32 addr;
	int i;

	if (of_property_read_u32(np, "mediatek,breadcrumbs", &addr))
		return;

	spm.crumbs = memremap(addr, CRUMB_WORDS * sizeof(u32), MEMREMAP_WB);
	if (!spm.crumbs)
		return;

	/* Only a suspend that didn't finish is worth reporting */
	if (spm.crumbs[0] == CRUMB_MAGIC && spm.crumbs[CRUMB_LAST] != STEP_END) {
		pr_warn("last suspend stopped at step %u, CPUs down %#x\n",
			spm.crumbs[CRUMB_LAST], spm.crumbs[CRUMB_CPUS_DEAD]);
		for (i = STEP_BEGIN; i <= STEP_END; i++)
			pr_info("  step %2d: %#x\n", i, spm.crumbs[CRUMB_VAL(i)]);
	}
	memset(spm.crumbs, 0, CRUMB_WORDS * sizeof(u32));
	mt8183_spm_crumb_flush();
}

/* After psci's own suspend_set_ops() (psci_dt_init at setup_arch time) */
static int __init mt8183_spm_suspend_init(void)
{
	struct device_node *np;
	u32 fw_idx, fw_flags = SPM_FW_FLAGS_NO_DVFS;
	unsigned int irq;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt8183-vendor-spm");
	if (!np || !of_device_is_available(np)) {
		of_node_put(np);
		return 0;
	}

	ret = of_property_read_u32(np, "mediatek,spm-firmware-index", &fw_idx);
	of_property_read_u32(np, "mediatek,pcm-flags", &spm.pcm_flags);
	of_property_read_u32(np, "mediatek,pcm-flags1", &spm.pcm_flags1);
	of_property_read_u32(np, "mediatek,timer-val", &spm.timer_val);
	spm.psci_state = MT8183_PSCI_SUSPEND;
	of_property_read_u32(np, "mediatek,psci-suspend-param", &spm.psci_state);
	of_property_read_u32(np, "mediatek,suspend-method", &spm.method);
	spm.no_gic_mask = of_property_read_bool(np, "mediatek,no-gic-mask");
	spm.no_mcdi_pause = of_property_read_bool(np, "mediatek,no-mcdi-pause");
	spm.no_sspm = of_property_read_bool(np, "mediatek,no-sspm");
	of_property_read_u32(np, "mediatek,spm-fw-flags", &fw_flags);
	mt8183_spm_crumbs_init(np);
	spm.mbox = of_iomap(np, 0);
	spm.mbox_irq = of_iomap(np, 1);
	spm.spm_base = of_iomap(np, 2);
	irq = irq_of_parse_and_map(np, 0);
	of_node_put(np);
	if (ret || !spm.mbox || !spm.mbox_irq || !spm.spm_base || !irq) {
		pr_err("incomplete DT node\n");
		ret = -EINVAL;
		goto err_unmap;
	}

	np = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
	if (np) {
		spm.gicd = of_iomap(np, 0);
		if (spm.gicd)
			spm.gic_regs = min(GIC_MAX_SPI_REGS,
					   (int)GICD_TYPER_SPIS(readl(spm.gicd + GICD_TYPER)) / 32);
	}
	of_node_put(np);

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt8183-mcdi-mbox");
	if (np && of_device_is_available(np))
		spm.mcdi = of_iomap(np, 0);
	of_node_put(np);

	if (mt8183_spm_smc(MTK_SIP_KERNEL_SPM_FIRMWARE_STATUS, 0, 0, 0) != 1) {
		pr_warn("SPM firmware not loaded, deep suspend unavailable\n");
		ret = 0;
		goto err_unmap;
	}

	/* Which of the SPM firmwares (per DRAM type and rate) to run */
	mt8183_spm_smc(MTK_SIP_KERNEL_SPM_ARGS, SPM_ARGS_SPMFW_IDX, fw_idx, 0);

	/* Start it, without Vcore DVFS, which nothing here drives */
	mt8183_spm_smc(MTK_SIP_KERNEL_SPM_VCOREFS_ARGS, VCOREFS_SMC_CMD_FW_INIT,
		       VCORE_BOOT_UP_OPP, 0);
	mt8183_spm_smc(MTK_SIP_KERNEL_SPM_VCOREFS_ARGS, VCOREFS_SMC_CMD_FW_RUN,
		       fw_flags, 0);

	/* Left enabled while asleep: the SPM wakes CPU0 with it */
	ret = request_irq(irq, mt8183_spm_irq, IRQF_NO_SUSPEND, "mt8183-spm", NULL);
	if (ret) {
		pr_err("can't get the SPM interrupt: %d\n", ret);
		goto err_unmap;
	}
	spm.spm_irq = mt8183_spm_irq_intid(irq);

	if (spm.crumbs)
		cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN, "soc/mt8183-spm:dead",
					  NULL, mt8183_spm_cpu_dead);

	suspend_set_ops(&mt8183_spm_suspend_ops);

	pr_info("deep suspend via vendor SPM (firmware %u flags %#x, SPM IRQ %u, method %u, state %#x, flags %#x/%#x, timer %#x, wake sources %#lx%s%s%s)\n",
		fw_idx, fw_flags, spm.spm_irq, spm.method, spm.psci_state,
		spm.pcm_flags, spm.pcm_flags1, spm.timer_val,
		mt8183_spm_smc(MTK_SIP_KERNEL_SPM_GET_PWR_CTRL_ARGS,
			       SPM_PWR_CTRL_SUSPEND, PWR_WAKE_SRC, 0),
		spm.no_gic_mask ? ", no GIC mask" : "",
		spm.no_mcdi_pause ? ", no MCDI pause" : "",
		spm.no_sspm ? ", no SSPM" : "");
	return 0;

err_unmap:
	if (spm.spm_base)
		iounmap(spm.spm_base);
	if (spm.gicd)
		iounmap(spm.gicd);
	if (spm.mcdi)
		iounmap(spm.mcdi);
	if (spm.mbox_irq)
		iounmap(spm.mbox_irq);
	if (spm.mbox)
		iounmap(spm.mbox);
	return ret;
}
late_initcall(mt8183_spm_suspend_init);
