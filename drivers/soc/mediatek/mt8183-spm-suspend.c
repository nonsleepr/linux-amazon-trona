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
 *  - at boot, tell TF-A which SPM firmware matches the DRAM;
 *  - around suspend to RAM, send the SSPM SPM_SUSPEND_PREPARE /
 *    SPM_POST_SUSPEND (SSPM IPI 6, which lives in SSPM mailbox 1);
 *  - pause the SSPM's MCDI task, arm the SPM's PCM watchdog, send the SSPM
 *    SPM_SUSPEND, pass the suspend flags and wake timer to TF-A, and enter
 *    the SoC's suspend state with PSCI CPU_SUSPEND; undo it all on resume.
 *
 * The IPI is a plain mailbox write and poll: data in the pin's slots, the
 * pin's bit in the mailbox's IN register raises the SSPM's interrupt, and
 * the SSPM answers with the same bit in the OUT register (write 1 to clear)
 * and the return value in the pin's first slot.
 *
 * Copyright (c) 2026 Toshit Chawda <r58playz@gmail.com>
 */

#define pr_fmt(fmt) "mt8183-spm: " fmt

#include <linux/arm-smccc.h>
#include <linux/bits.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/psci.h>
#include <linux/sched/clock.h>
#include <linux/suspend.h>

#include <clocksource/arm_arch_timer.h>

#define MTK_SIP_KERNEL_SPM_SUSPEND_ARGS		0xc200021a
#define MTK_SIP_KERNEL_SPM_FIRMWARE_STATUS	0xc200021b
#define MTK_SIP_KERNEL_SPM_ARGS			0xc2000228
#define MTK_SIP_KERNEL_SPM_GET_PWR_CTRL_ARGS	0xc2000229

/* MTK_SIP_KERNEL_SPM_ARGS commands (MT8183 numbering) */
#define SPM_ARGS_SPMFW_IDX		0
#define SPM_ARGS_PCM_WDT		8

/* SPM_GET_PWR_CTRL_ARGS: scenario and field */
#define SPM_PWR_CTRL_SUSPEND		17
#define PWR_WAKE_SRC			12

/* The "suspend" state of MediaTek's DT: system level, power down, id 5 */
#define MT8183_PSCI_SUSPEND		0x01010005

#define PCM_WDT_TIMEOUT_S		30

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

struct mt8183_spm {
	void __iomem *mbox;
	void __iomem *mbox_irq;
	void __iomem *mcdi;
	u32 pcm_flags;
	u32 pcm_flags1;
	u32 timer_val;
	bool prepared;
};

static struct mt8183_spm spm;

static unsigned long mt8183_spm_smc(unsigned long fid, unsigned long a1,
				    unsigned long a2, unsigned long a3)
{
	struct arm_smccc_res res;

	arm_smccc_smc(fid, a1, a2, a3, 0, 0, 0, 0, &res);
	return res.a0;
}

/* Send one SPM command to the SSPM and wait for its answer */
static int mt8183_spm_sspm_cmd(u32 cmd, const u32 *args, int nargs)
{
	void __iomem *data = spm.mbox + SSPM_SPM_SLOT * 4;
	u32 out;
	int i, ret;

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

	if (!spm.mcdi)
		return 0;

	writel(pause, spm.mcdi + MCDI_PAUSE_ACTION);
	ret = readl_poll_timeout_atomic(spm.mcdi + MCDI_PAUSE_ACK, ack,
					ack == pause, 1, MCDI_PAUSE_TIMEOUT_US);
	if (ret)
		pr_err("MCDI task didn't %s\n", pause ? "pause" : "resume");
	return ret;
}

/* Only suspend to RAM goes through the SPM (suspend-to-idle never gets here) */
static int mt8183_spm_suspend_begin(suspend_state_t state)
{
	int ret;

	if (state != PM_SUSPEND_MEM)
		return 0;

	ret = mt8183_spm_sspm_cmd(SPM_SUSPEND_PREPARE, NULL, 0);
	if (ret)
		return ret;

	spm.prepared = true;
	pm_set_suspend_via_firmware();
	return 0;
}

static void mt8183_spm_suspend_end(void)
{
	if (!spm.prepared)
		return;

	mt8183_spm_sspm_cmd(SPM_POST_SUSPEND, NULL, 0);
	spm.prepared = false;
}

static int mt8183_spm_suspend_enter(suspend_state_t state)
{
	int ret;

	pm_set_resume_via_firmware();

	ret = mt8183_spm_mcdi_pause(true);
	if (ret) {
		mt8183_spm_mcdi_pause(false);
		return ret;
	}

	/* Reset rather than hang if the SPM doesn't bring the system back */
	mt8183_spm_smc(MTK_SIP_KERNEL_SPM_ARGS, SPM_ARGS_PCM_WDT, 1,
		       PCM_WDT_TIMEOUT_S);

	mt8183_spm_sspm_cmd_time(SPM_SUSPEND);
	mt8183_spm_smc(MTK_SIP_KERNEL_SPM_SUSPEND_ARGS, spm.pcm_flags,
		       spm.pcm_flags1, spm.timer_val);

	ret = psci_cpu_suspend_enter(MT8183_PSCI_SUSPEND);

	mt8183_spm_sspm_cmd_time(SPM_RESUME);
	mt8183_spm_smc(MTK_SIP_KERNEL_SPM_ARGS, SPM_ARGS_PCM_WDT, 0, 0);
	mt8183_spm_mcdi_pause(false);

	return ret;
}

static const struct platform_suspend_ops mt8183_spm_suspend_ops = {
	.valid = suspend_valid_only_mem,
	.begin = mt8183_spm_suspend_begin,
	.enter = mt8183_spm_suspend_enter,
	.end = mt8183_spm_suspend_end,
};

/* After psci's own suspend_set_ops() (psci_dt_init at setup_arch time) */
static int __init mt8183_spm_suspend_init(void)
{
	struct device_node *np;
	u32 fw_idx;
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
	spm.mbox = of_iomap(np, 0);
	spm.mbox_irq = of_iomap(np, 1);
	of_node_put(np);
	if (ret || !spm.mbox || !spm.mbox_irq) {
		pr_err("incomplete DT node\n");
		ret = -EINVAL;
		goto err_unmap;
	}

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

	suspend_set_ops(&mt8183_spm_suspend_ops);

	pr_info("deep suspend via vendor SPM (firmware %u, flags %#x/%#x, timer %#x, wake sources %#lx)\n",
		fw_idx, spm.pcm_flags, spm.pcm_flags1, spm.timer_val,
		mt8183_spm_smc(MTK_SIP_KERNEL_SPM_GET_PWR_CTRL_ARGS,
			       SPM_PWR_CTRL_SUSPEND, PWR_WAKE_SRC, 0));
	return 0;

err_unmap:
	if (spm.mcdi)
		iounmap(spm.mcdi);
	if (spm.mbox_irq)
		iounmap(spm.mbox_irq);
	if (spm.mbox)
		iounmap(spm.mbox);
	return ret;
}
late_initcall(mt8183_spm_suspend_init);
