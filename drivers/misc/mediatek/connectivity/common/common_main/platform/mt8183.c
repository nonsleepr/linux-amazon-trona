/*
 * Copyright (C) 2016 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */
/*! \file
*    \brief  Declaration of library functions
*
*    Any definitions in this file will be shared among GLUE Layer and internal Driver Stack.
*/
/*
 * MediaTek's version of this file drove the CONSYS's power domain through a
 * clock of their own power domain driver, and the MCU's reset, the PMIC, the
 * pin controller and INFRACFG by writing their registers. Here each of them
 * is reached through its framework; what is done, and in which order, is
 * unchanged.
 */
/*******************************************************************************
*                         C O M P I L E R   F L A G S
********************************************************************************
*/

/*******************************************************************************
*                                 M A C R O S
********************************************************************************
*/

#ifdef DFT_TAG
#undef DFT_TAG
#endif
#define DFT_TAG "[WMT-CONSYS-HW]"

/*******************************************************************************
*                    E X T E R N A L   R E F E R E N C E S
********************************************************************************
*/
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/irq.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include "osal_typedef.h"
#include "mt8183.h"
#include "mtk_wcn_consys_hw.h"

/*******************************************************************************
*                              C O N S T A N T S
********************************************************************************
*/

/*******************************************************************************
*                             D A T A   T Y P E S
********************************************************************************
*/

/*******************************************************************************
*                  F U N C T I O N   D E C L A R A T I O N S
********************************************************************************
*/
static INT32 consys_clock_buffer_ctrl(MTK_WCN_BOOL enable);
static VOID consys_hw_reset_bit_set(MTK_WCN_BOOL enable);
static VOID consys_hw_spm_clk_gating_enable(VOID);
static INT32 consys_hw_power_ctrl(MTK_WCN_BOOL enable);
static INT32 consys_ahb_clock_ctrl(MTK_WCN_BOOL enable);
static INT32 polling_consys_chipid(VOID);
static VOID update_consys_rom_desel(VOID);
static VOID consys_hang_debug_info(VOID);
static VOID consys_acr_reg_setting(VOID);
static VOID consys_afe_reg_setting(VOID);
static INT32 consys_hw_vcn18_ctrl(MTK_WCN_BOOL enable);
static VOID consys_vcn28_hw_mode_ctrl(UINT32 enable);
static INT32 consys_hw_vcn28_ctrl(UINT32 enable);
static INT32 consys_hw_wifi_vcn33_ctrl(UINT32 enable);
static INT32 consys_hw_bt_vcn33_ctrl(UINT32 enable);
static UINT32 consys_soc_chipid_get(VOID);
static INT32 consys_emi_mpu_set_region_protection(VOID);
static UINT32 consys_emi_set_remapping_reg(VOID);
static INT32 bt_wifi_share_v33_spin_lock_init(VOID);
static INT32 consys_clk_get_from_dts(struct platform_device *pdev);
static INT32 consys_pmic_get_from_dts(struct platform_device *pdev);
static INT32 consys_read_irq_info_from_dts(struct platform_device *pdev, PINT32 irq_num, PUINT32 irq_flag);
static INT32 consys_read_reg_from_dts(struct platform_device *pdev);
static UINT32 consys_read_cpupcr(VOID);
static VOID force_trigger_assert_debug_pin(VOID);
static INT32 consys_co_clock_type(VOID);
static P_CONSYS_EMI_ADDR_INFO consys_soc_get_emi_phy_add(VOID);
static MTK_WCN_BOOL consys_need_store_pdev(VOID);
static INT32 consys_store_pdev(struct platform_device *pdev);
static INT32 consys_store_reset_control(struct platform_device *pdev);
static VOID consys_set_if_pinmux(MTK_WCN_BOOL enable);
static INT32 consys_check_reg_readable(VOID);
static INT32 consys_emi_coredump_remapping(UINT8 __iomem **addr, UINT32 enable);
static INT32 consys_reset_emi_coredump(UINT8 __iomem *addr);
static INT32 consys_is_connsys_reg(UINT32 addr);

/*******************************************************************************
*                            P U B L I C   D A T A
********************************************************************************
*/
#if CONSYS_BT_WIFI_SHARE_V33
struct bt_wifi_v33_status gBtWifiV33;
#endif

/* The device, for its power domain (runtime PM) */
static struct device *consys_dev;

/* CCF part */
static struct clk *clk_infra_pmic_conn;	/* the CONSYS's own channel to the PMIC */

/* Reset part */
static struct reset_control *rst_conn_mcu;	/* the CONSYS's MCU */

/* PMIC part */
#if CONSYS_PMIC_CTRL_ENABLE
static struct regmap *consys_pmic_regmap;
static struct regulator *reg_VCN18;
static struct regulator *reg_VCN28;
static struct regulator *reg_VCN33_BT;	/* one LDO for BT and WIFI here: "vcn33" */
#endif

/* GPIO part: the A-Die interface pins as GPIOs, and handed to the CONSYS */
static struct pinctrl_state *consys_pins_default;
static struct pinctrl_state *consys_pins_active;

#if CONSYS_AFE_REG_SETTING
static UINT8 __iomem *consys_afe_reg_base;
#endif

/*
 * The CONSYS's registers can only be accessed while its power domain is on:
 * with it off, the access hangs the bus. Every access in this file is made
 * with this lock held, and only when the domain is known to be on.
 */
static DEFINE_SPINLOCK(consys_domain_lock);
static MTK_WCN_BOOL consys_domain_on;

EMI_CTRL_STATE_OFFSET mtk_wcn_emi_state_off = {
	.emi_apmem_ctrl_state = EXP_APMEM_CTRL_STATE,
	.emi_apmem_ctrl_host_sync_state = EXP_APMEM_CTRL_HOST_SYNC_STATE,
	.emi_apmem_ctrl_host_sync_num = EXP_APMEM_CTRL_HOST_SYNC_NUM,
	.emi_apmem_ctrl_chip_sync_state = EXP_APMEM_CTRL_CHIP_SYNC_STATE,
	.emi_apmem_ctrl_chip_sync_num = EXP_APMEM_CTRL_CHIP_SYNC_NUM,
	.emi_apmem_ctrl_chip_sync_addr = EXP_APMEM_CTRL_CHIP_SYNC_ADDR,
	.emi_apmem_ctrl_chip_sync_len = EXP_APMEM_CTRL_CHIP_SYNC_LEN,
	.emi_apmem_ctrl_chip_print_buff_start = EXP_APMEM_CTRL_CHIP_PRINT_BUFF_START,
	.emi_apmem_ctrl_chip_print_buff_len = EXP_APMEM_CTRL_CHIP_PRINT_BUFF_LEN,
	.emi_apmem_ctrl_chip_print_buff_idx = EXP_APMEM_CTRL_CHIP_PRINT_BUFF_IDX,
	.emi_apmem_ctrl_chip_int_status = EXP_APMEM_CTRL_CHIP_INT_STATUS,
	.emi_apmem_ctrl_chip_paded_dump_end = EXP_APMEM_CTRL_CHIP_PAGED_DUMP_END,
	.emi_apmem_ctrl_host_outband_assert_w1 = EXP_APMEM_CTRL_HOST_OUTBAND_ASSERT_W1,
	.emi_apmem_ctrl_chip_page_dump_num = EXP_APMEM_CTRL_CHIP_PAGE_DUMP_NUM,
	.emi_apmem_ctrl_assert_flag = EXP_APMEM_CTRL_ASSERT_FLAG,
};

CONSYS_EMI_ADDR_INFO mtk_wcn_emi_addr_info = {
	.emi_phy_addr = CONSYS_EMI_FW_PHY_BASE,
	.paged_trace_off = CONSYS_EMI_PAGED_TRACE_OFFSET,
	.paged_dump_off = CONSYS_EMI_PAGED_DUMP_OFFSET,
	.full_dump_off = CONSYS_EMI_FULL_DUMP_OFFSET,
	.emi_remap_offset = CONSYS_EMI_MAPPING_OFFSET,
	.p_ecso = &mtk_wcn_emi_state_off,
	.emi_core_dump_offset = CONSYS_EMI_COREDUMP_OFFSET,
};

WMT_CONSYS_IC_OPS consys_ic_ops = {
	.consys_ic_clock_buffer_ctrl = consys_clock_buffer_ctrl,
	.consys_ic_hw_reset_bit_set = consys_hw_reset_bit_set,
	.consys_ic_hw_spm_clk_gating_enable = consys_hw_spm_clk_gating_enable,
	.consys_ic_hw_power_ctrl = consys_hw_power_ctrl,
	.consys_ic_ahb_clock_ctrl = consys_ahb_clock_ctrl,
	.polling_consys_ic_chipid = polling_consys_chipid,
	.update_consys_rom_desel_value = update_consys_rom_desel,
	.consys_hang_debug = consys_hang_debug_info,
	.consys_ic_acr_reg_setting = consys_acr_reg_setting,
	.consys_ic_afe_reg_setting = consys_afe_reg_setting,
	.consys_ic_hw_vcn18_ctrl = consys_hw_vcn18_ctrl,
	.consys_ic_vcn28_hw_mode_ctrl = consys_vcn28_hw_mode_ctrl,
	.consys_ic_hw_vcn28_ctrl = consys_hw_vcn28_ctrl,
	.consys_ic_hw_wifi_vcn33_ctrl = consys_hw_wifi_vcn33_ctrl,
	.consys_ic_hw_bt_vcn33_ctrl = consys_hw_bt_vcn33_ctrl,
	.consys_ic_soc_chipid_get = consys_soc_chipid_get,
	.consys_ic_emi_mpu_set_region_protection = consys_emi_mpu_set_region_protection,
	.consys_ic_emi_set_remapping_reg = consys_emi_set_remapping_reg,
	.ic_bt_wifi_share_v33_spin_lock_init = bt_wifi_share_v33_spin_lock_init,
	.consys_ic_clk_get_from_dts = consys_clk_get_from_dts,
	.consys_ic_pmic_get_from_dts = consys_pmic_get_from_dts,
	.consys_ic_read_irq_info_from_dts = consys_read_irq_info_from_dts,
	.consys_ic_read_reg_from_dts = consys_read_reg_from_dts,
	.consys_ic_read_cpupcr = consys_read_cpupcr,
	.ic_force_trigger_assert_debug_pin = force_trigger_assert_debug_pin,
	.consys_ic_co_clock_type = consys_co_clock_type,
	.consys_ic_soc_get_emi_phy_add = consys_soc_get_emi_phy_add,
	.consys_ic_need_store_pdev = consys_need_store_pdev,
	.consys_ic_store_pdev = consys_store_pdev,
	.consys_ic_store_reset_control = consys_store_reset_control,
	.consys_ic_set_if_pinmux = consys_set_if_pinmux,
	.consys_ic_check_reg_readable = consys_check_reg_readable,
	.consys_ic_emi_coredump_remapping = consys_emi_coredump_remapping,
	.consys_ic_reset_emi_coredump = consys_reset_emi_coredump,
	.consys_ic_is_connsys_reg = consys_is_connsys_reg,
};

/*******************************************************************************
*                           P R I V A T E   D A T A
********************************************************************************
*/

/*******************************************************************************
*                              F U N C T I O N S
********************************************************************************
*/
UINT32 gJtagCtrl;

/*
 * Read a CONSYS register, if that can be done. Returns MTK_WCN_BOOL_FALSE,
 * and reads nothing, with the power domain off.
 */
static MTK_WCN_BOOL consys_reg_read(const UINT8 __iomem *addr, PUINT32 value)
{
	MTK_WCN_BOOL ret;
	ULONG flags;

	spin_lock_irqsave(&consys_domain_lock, flags);
	ret = consys_domain_on;
	if (ret)
		*value = CONSYS_REG_READ(addr);
	spin_unlock_irqrestore(&consys_domain_lock, flags);

	return ret;
}

/* Write a CONSYS register. Does nothing with the power domain off. */
static MTK_WCN_BOOL consys_reg_write(UINT8 __iomem *addr, UINT32 value)
{
	MTK_WCN_BOOL ret;
	ULONG flags;

	spin_lock_irqsave(&consys_domain_lock, flags);
	ret = consys_domain_on;
	if (ret)
		CONSYS_REG_WRITE(addr, value);
	spin_unlock_irqrestore(&consys_domain_lock, flags);

	return ret;
}

/* A CONN_MCU_CONFIG register; it reads as 0 with the power domain off */
static UINT32 consys_mcu_reg_read(UINT32 offset)
{
	UINT32 value = 0;

	consys_reg_read((UINT8 __iomem *)conn_reg.mcu_base + offset, &value);

	return value;
}

static VOID consys_mcu_reg_write(UINT32 offset, UINT32 value)
{
	consys_reg_write((UINT8 __iomem *)conn_reg.mcu_base + offset, value);
}

INT32 mtk_wcn_consys_jtag_set_for_mcu(VOID)
{
	/*
	 * MediaTek's kernel wrote the pin controller's registers here to give
	 * the MCU's JTAG signals pins. There are no pin states for that.
	 */
	if (gJtagCtrl)
		WMT_PLAT_PR_WARN("no pins for the MCU's JTAG(%d) here\n", gJtagCtrl);

	return 0;
}

UINT32 mtk_wcn_consys_jtag_flag_ctrl(UINT32 en)
{
	WMT_PLAT_PR_INFO("%s jtag set for MCU\n", en ? "enable" : "disable");
	gJtagCtrl = en;
	return 0;
}

static MTK_WCN_BOOL consys_need_store_pdev(VOID)
{
	/* For runtime PM: that is what switches the power domain here */
	return MTK_WCN_BOOL_TRUE;
}

static INT32 consys_store_pdev(struct platform_device *pdev)
{
	struct pinctrl *pinctrl = mtk_wcn_consys_get_pinctrl();

	consys_pins_default = pinctrl_lookup_state(pinctrl, PINCTRL_STATE_DEFAULT);
	if (IS_ERR(consys_pins_default)) {
		WMT_PLAT_PR_ERR("cannot find the %s pin state\n", PINCTRL_STATE_DEFAULT);
		return PTR_ERR(consys_pins_default);
	}

	consys_pins_active = pinctrl_lookup_state(pinctrl, CONSYS_PINCTRL_STATE_ACTIVE);
	if (IS_ERR(consys_pins_active)) {
		WMT_PLAT_PR_ERR("cannot find the %s pin state\n", CONSYS_PINCTRL_STATE_ACTIVE);
		return PTR_ERR(consys_pins_active);
	}

	consys_dev = &pdev->dev;

	return 0;
}

static INT32 consys_store_reset_control(struct platform_device *pdev)
{
	rst_conn_mcu = devm_reset_control_get_exclusive(&pdev->dev, CONSYS_RESET_NAME_MCU);
	if (IS_ERR(rst_conn_mcu)) {
		WMT_PLAT_PR_ERR("cannot get the MCU's reset control.\n");
		return PTR_ERR(rst_conn_mcu);
	}

	return 0;
}

static INT32 consys_clk_get_from_dts(struct platform_device *pdev)
{
	/*
	 * The clock of the PMIC wrapper's channel for the CONSYS, which follows
	 * the CONSYS's request for its 26MHz reference with PMIC writes.
	 * MediaTek's kernel never gates it. Keep it on here as well, rather
	 * than switch it at the very moments the CONSYS raises and drops that
	 * request: when its power domain goes on and off.
	 */
	clk_infra_pmic_conn = devm_clk_get_enabled(&pdev->dev, CONSYS_CLK_NAME_PMIC_CONN);
	if (IS_ERR(clk_infra_pmic_conn)) {
		WMT_PLAT_PR_ERR("[CCF]cannot get clk_infra_pmic_conn clock.\n");
		return PTR_ERR(clk_infra_pmic_conn);
	}
	WMT_PLAT_PR_DBG("[CCF]clk_infra_pmic_conn=%p\n", clk_infra_pmic_conn);

	return 0;
}

static INT32 consys_pmic_get_from_dts(struct platform_device *pdev)
{
#if CONSYS_PMIC_CTRL_ENABLE
	struct device_node *node;
	struct platform_device *pmic;

	/* The PMIC's regmap belongs to its parent, the PMIC wrapper */
	node = of_parse_phandle(pdev->dev.of_node, "mediatek,pmic", 0);
	pmic = node ? of_find_device_by_node(node) : NULL;
	of_node_put(node);
	if (!pmic) {
		WMT_PLAT_PR_ERR("cannot find the PMIC\n");
		return -EPROBE_DEFER;
	}
	consys_pmic_regmap = dev_get_regmap(pmic->dev.parent, NULL);
	put_device(&pmic->dev);
	if (!consys_pmic_regmap) {
		WMT_PLAT_PR_ERR("cannot get the PMIC's regmap\n");
		return -EPROBE_DEFER;
	}

	reg_VCN18 = devm_regulator_get(&pdev->dev, "vcn18");
	if (IS_ERR(reg_VCN18)) {
		WMT_PLAT_PR_ERR("Regulator_get VCN_1V8 fail\n");
		return PTR_ERR(reg_VCN18);
	}
	reg_VCN28 = devm_regulator_get(&pdev->dev, "vcn28");
	if (IS_ERR(reg_VCN28)) {
		WMT_PLAT_PR_ERR("Regulator_get VCN_2V8 fail\n");
		return PTR_ERR(reg_VCN28);
	}
	reg_VCN33_BT = devm_regulator_get(&pdev->dev, "vcn33");
	if (IS_ERR(reg_VCN33_BT)) {
		WMT_PLAT_PR_ERR("Regulator_get VCN33 fail\n");
		return PTR_ERR(reg_VCN33_BT);
	}
#endif
	return 0;
}

static INT32 consys_co_clock_type(VOID)
{
	struct device_node *node = NULL;
	struct gpio_desc *gpio_coclock;
	INT32 value = 0;

	/*
	 * The strap that tells where the 26MHz reference comes from: high for
	 * the PMIC's XO_WCN buffer (co-clock), low for a TCXO of the RF chip's
	 * own. This is asked for when the WMT driver starts, and so possibly
	 * before there is a probed device to get the GPIO for: get it for the
	 * device's node, and only for as long as it takes to read it.
	 */
	node = of_find_compatible_node(NULL, NULL, "mediatek,mt8183-consys");
	if (!node) {
		WMT_PLAT_PR_ERR("Can not find device tree node\n");
		return -1;
	}

	gpio_coclock = fwnode_gpiod_get_index(of_fwnode_handle(node), CONSYS_GPIO_NAME_COCLOCK, 0,
					      GPIOD_IN, "consys coclock");
	of_node_put(node);
	if (IS_ERR(gpio_coclock)) {
		WMT_PLAT_PR_ERR("Can not get coclock gpio(%ld)!\n", PTR_ERR(gpio_coclock));
		return -1;
	}

	value = gpiod_get_value_cansleep(gpio_coclock);

	gpiod_put(gpio_coclock);

	return value;
}

static INT32 consys_clock_buffer_ctrl(MTK_WCN_BOOL enable)
{
	/*
	 * MediaTek's kernel called clk_buf_ctrl(CLK_BUF_CONN, enable) here to
	 * open and close the PMIC's XO_WCN buffer. On this SoC that function
	 * writes no register for this buffer.
	 */
	return 0;
}

static VOID consys_set_if_pinmux(MTK_WCN_BOOL enable)
{
	/* Switch D die pinmux for connecting A die */
	if (pinctrl_select_state(mtk_wcn_consys_get_pinctrl(),
				 enable ? consys_pins_active : consys_pins_default))
		WMT_PLAT_PR_ERR("select the %s pin state fail\n", enable ? "active" : "default");
}

static VOID consys_hw_reset_bit_set(MTK_WCN_BOOL enable)
{
	INT32 iRet;

	if (enable) {
		/*3.assert CONNSYS CPU SW reset  0x10007018 "[12]=1'b1  [31:24]=8'h88 (key)" */
		iRet = reset_control_assert(rst_conn_mcu);
	} else {
		/*16.deassert CONNSYS CPU SW reset 0x10007018 "[12]=1'b0 [31:24] =8'h88 (key)" */
		iRet = reset_control_deassert(rst_conn_mcu);
	}
	if (iRet)
		WMT_PLAT_PR_ERR("%s the MCU's reset fail(%d)\n", enable ? "assert" : "deassert", iRet);
}

static VOID consys_hw_spm_clk_gating_enable(VOID)
{
	/*turn on SPM clock gating enable PWRON_CONFG_EN 0x10006000 32'h0b160001 */
}

static INT32 consys_hw_power_ctrl(MTK_WCN_BOOL enable)
{
	INT32 iRet = 0;
	ULONG flags;

	if (enable) {
		if (consys_domain_on)
			return 0;

		iRet = pm_runtime_resume_and_get(consys_dev);
		if (iRet) {
			WMT_PLAT_PR_ERR("power domain on fail(%d)\n", iRet);
			return iRet;
		}
		WMT_PLAT_PR_DBG("power domain on ok\n");

		spin_lock_irqsave(&consys_domain_lock, flags);
		consys_domain_on = MTK_WCN_BOOL_TRUE;
		spin_unlock_irqrestore(&consys_domain_lock, flags);
	} else {
		if (!consys_domain_on)
			return 0;

		/* No access to the CONSYS's registers from here on */
		spin_lock_irqsave(&consys_domain_lock, flags);
		consys_domain_on = MTK_WCN_BOOL_FALSE;
		spin_unlock_irqrestore(&consys_domain_lock, flags);

		pm_runtime_put_sync(consys_dev);
		WMT_PLAT_PR_DBG("power domain off calling\n");

		/*
		 * MediaTek's flow leaves the MCU's reset released here, to
		 * assert it at the start of the next power on. Don't leave an
		 * unpowered CONSYS ready to run: whatever switches its domain
		 * on would start the MCU without its supplies.
		 */
		iRet = reset_control_assert(rst_conn_mcu);
		if (iRet)
			WMT_PLAT_PR_ERR("assert the MCU's reset fail(%d)\n", iRet);
		iRet = 0;
	}

	return iRet;
}

static INT32 consys_ahb_clock_ctrl(MTK_WCN_BOOL enable)
{
	return 0;
}

static INT32 polling_consys_chipid(VOID)
{
	UINT32 retry = 10;
	UINT32 consysHwChipId = 0;

	/*12.poll CONNSYS CHIP ID until chipid is returned  0x18070008 */
	while (retry-- > 0) {
		consysHwChipId = consys_mcu_reg_read(CONSYS_CHIP_ID_OFFSET);
		if (consysHwChipId == CONSYS_CHIP_ID_VALUE) {
			WMT_PLAT_PR_INFO("retry(%d)consys chipId(0x%08x)\n", retry, consysHwChipId);
			return 0;
		}
		msleep(20);
	}

	WMT_PLAT_PR_ERR("Maybe has a consys power on issue,(0x%08x)\n", consysHwChipId);

	return -1;
}

static VOID update_consys_rom_desel(VOID)
{
	consys_mcu_reg_write(CONSYS_ROM_DESEL_OFFSET, CONSYS_ROM_DESEL_MASK);
	WMT_PLAT_PR_INFO("Update consys rom desel value(0x%x)\n",
			consys_mcu_reg_read(CONSYS_ROM_DESEL_OFFSET));
}

static VOID consys_hang_debug_info(VOID)
{
	UINT32 rv1, wv1[2], rv2[2], rv3, wv3 = 0;
	UINT32 i = 0;

	if (!consys_check_reg_readable()) {
		WMT_PLAT_PR_INFO("the CONSYS is off, no debug info\n");
		return;
	}

	rv1 = consys_mcu_reg_read(CONSYS_HANG_DBG_OFFSET_1);
	rv3 = consys_mcu_reg_read(CONSYS_HANG_DBG_OFFSET_3);
	WMT_PLAT_PR_INFO("addr1:%x:0x%x/addr2:%x/addr3:%x:0x%x\n",
			   CONSYS_HANG_DBG_OFFSET_1, rv1, CONSYS_HANG_DBG_OFFSET_2,
			   CONSYS_HANG_DBG_OFFSET_3, rv3);

	wv1[0] = (rv1 & 0xFF0000FF) | (0x0201 << 8);
	consys_mcu_reg_write(CONSYS_HANG_DBG_OFFSET_1, wv1[0]);
	rv2[0] = consys_mcu_reg_read(CONSYS_HANG_DBG_OFFSET_2);

	wv1[1] = (rv1 & 0xFF0000FF) | (0x0403 << 8);
	consys_mcu_reg_write(CONSYS_HANG_DBG_OFFSET_1, wv1[1]);
	rv2[1] = consys_mcu_reg_read(CONSYS_HANG_DBG_OFFSET_2);
	WMT_PLAT_PR_INFO("0x%x->addr1, addr2:0x%x/0x%x->addr1, addr2:0x%x\n",
			   wv1[0], rv2[0], wv1[1], rv2[1]);

	for (i = 0; i < 9; i++) {
		wv3 = (rv3 & 0xFFFFFF0F) | (i << 4);
		consys_mcu_reg_write(CONSYS_HANG_DBG_OFFSET_3, wv3);

		wv1[0] = (rv1 & 0xFF0000FF) | (0x5251 << 8);
		consys_mcu_reg_write(CONSYS_HANG_DBG_OFFSET_1, wv1[0]);
		rv2[0] = consys_mcu_reg_read(CONSYS_HANG_DBG_OFFSET_2);

		wv1[1] = (rv1 & 0xFF0000FF) | (0x5453 << 8);
		consys_mcu_reg_write(CONSYS_HANG_DBG_OFFSET_1, wv1[1]);
		rv2[1] = consys_mcu_reg_read(CONSYS_HANG_DBG_OFFSET_2);
		WMT_PLAT_PR_INFO("0x%x->addr3,0x%x->addr1,addr2:0x%x/0x%x->addr1,addr2:0x%x\n",
				   wv3, wv1[0], rv2[0], wv1[1], rv2[1]);
	}

	WMT_PLAT_PR_INFO("apb0_dbg_prob\n");
	for (i = 0; i < 10; i++) {
		wv3 = (rv3 & 0xFFFF0FFF) | (i << 12);
		consys_mcu_reg_write(CONSYS_HANG_DBG_OFFSET_3, wv3);

		wv1[0] = (rv1 & 0xFF0000FF) | (0x5655 << 8);
		consys_mcu_reg_write(CONSYS_HANG_DBG_OFFSET_1, wv1[0]);
		rv2[0] = consys_mcu_reg_read(CONSYS_HANG_DBG_OFFSET_2);

		wv1[1] = (rv1 & 0xFF0000FF) | (0x5857 << 8);
		consys_mcu_reg_write(CONSYS_HANG_DBG_OFFSET_1, wv1[1]);
		rv2[1] = consys_mcu_reg_read(CONSYS_HANG_DBG_OFFSET_2);
		WMT_PLAT_PR_INFO("0x%x->addr3,0x%x->addr1,addr2:0x%x/0x%x->addr1,addr2:0x%x\n",
				   wv3, wv1[0], rv2[0], wv1[1], rv2[1]);
	}

	WMT_PLAT_PR_INFO("apb1_dbg_prob\n");
	for (i = 0; i < 10; i++) {
		wv3 = (rv3 & 0xFF0FFFFF) | (i << 20);
		consys_mcu_reg_write(CONSYS_HANG_DBG_OFFSET_3, wv3);

		wv1[0] = (rv1 & 0xFF0000FF) | (0x5A59 << 8);
		consys_mcu_reg_write(CONSYS_HANG_DBG_OFFSET_1, wv1[0]);
		rv2[0] = consys_mcu_reg_read(CONSYS_HANG_DBG_OFFSET_2);

		wv1[1] = (rv1 & 0xFF0000FF) | (0x5C5B << 8);
		consys_mcu_reg_write(CONSYS_HANG_DBG_OFFSET_1, wv1[1]);
		rv2[1] = consys_mcu_reg_read(CONSYS_HANG_DBG_OFFSET_2);
		WMT_PLAT_PR_INFO("0x%x->addr3,0x%x->addr1,addr2:0x%x/0x%x->addr1,addr2:0x%x\n",
				   wv3, wv1[0], rv2[0], wv1[1], rv2[1]);
	}
}

static VOID consys_acr_reg_setting(VOID)
{
	/*
	 * if bypass at-speed MBIST (default is at-speed) 0x18070140[1:0] 2'b11
	 */
	consys_mcu_reg_write(CONSYS_MCU_CFG_ACR_OFFSET,
			consys_mcu_reg_read(CONSYS_MCU_CFG_ACR_OFFSET) |
			CONSYS_MCU_CFG_ACR_MBIST_BIT);
}

static VOID consys_afe_reg_setting(VOID)
{
#if CONSYS_AFE_REG_SETTING
	UINT8 i = 0;
	UINT32 value = 0;

	/*15.default no need,update ANA_WBG(AFE) CR if needed, CONSYS_AFE_REG */
	if (!consys_reg_write(consys_afe_reg_base + CONSYS_AFE_RG_WBG_PLL_03_OFFSET,
			      CONSYS_AFE_RG_WBG_PLL_03_VALUE) ||
	    !consys_reg_write(consys_afe_reg_base + CONSYS_AFE_RG_WBG_GPS_02_OFFSET,
			      CONSYS_AFE_RG_WBG_GPS_02_VALUE)) {
		WMT_PLAT_PR_ERR("the CONSYS is off, AFE not set!\n");
		return;
	}

	if (wmtPlatLogLvl >= WMT_PLAT_LOG_DBG) {
		WMT_PLAT_PR_DBG("Dump AFE register\n");
		for (i = 0; i < CONSYS_AFE_REG_SIZE / 4; i++) {
			if (!consys_reg_read(consys_afe_reg_base + 4*i, &value))
				break;
			WMT_PLAT_PR_DBG("reg:0x%08x|val:0x%08x\n",
					CONSYS_AFE_REG_BASE + 4*i, value);
		}
	}
#endif
}

static INT32 consys_hw_vcn18_ctrl(MTK_WCN_BOOL enable)
{
#if CONSYS_PMIC_CTRL_ENABLE
	INT32 iRet;

	if (enable) {
		/*need PMIC driver provide new API protocol */
		/*1.AP power on VCN_1V8 LDO (with PMIC_WRAP API) VCN_1V8  */
		/*set vcn18 SW mode*/
		regmap_write(consys_pmic_regmap, MT6358_LDO_VCN18_OP_EN, MT6358_LDO_SW_OP_EN_BIT);

		regulator_set_voltage(reg_VCN18, 1800000, 1800000);
		iRet = regulator_enable(reg_VCN18);
		if (iRet) {
			WMT_PLAT_PR_ERR("enable VCN18 fail\n");
			return iRet;
		}
		WMT_PLAT_PR_DBG("enable VCN18 ok\n");

		/*set vcn33 SW mode*/
		regmap_write(consys_pmic_regmap, MT6358_LDO_VCN33_OP_EN, MT6358_LDO_SW_OP_EN_BIT);

		regulator_set_voltage(reg_VCN33_BT, 3500000, 3500000);
		iRet = regulator_enable(reg_VCN33_BT);
		if (iRet) {
			WMT_PLAT_PR_ERR("WMT do BT PMIC on fail!\n");
			regulator_disable(reg_VCN18);
			return iRet;
		}
	} else {
		regulator_disable(reg_VCN33_BT);

		/*AP power off MT6351L VCN_1V8 LDO */
		if (regulator_disable(reg_VCN18))
			WMT_PLAT_PR_ERR("disable VCN_1V8 fail!\n");
		else
			WMT_PLAT_PR_DBG("disable VCN_1V8 ok\n");
	}
#endif
	return 0;
}

static VOID consys_vcn28_hw_mode_ctrl(UINT32 enable)
{
#if CONSYS_PMIC_CTRL_ENABLE
	/*
	 * PMIC_RG_LDO_VCN28_HW0_OP_EN and PMIC_RG_LDO_VCN28_HW0_OP_CFG: with
	 * its own TCXO the RF chip gets it powered from vcn28 whenever the
	 * CONSYS asks.
	 */
	if (enable) {
		regmap_update_bits(consys_pmic_regmap, MT6358_LDO_VCN28_OP_EN,
				   MT6358_LDO_HW0_OP_BIT, MT6358_LDO_HW0_OP_BIT);
		regmap_update_bits(consys_pmic_regmap, MT6358_LDO_VCN28_OP_CFG,
				   MT6358_LDO_HW0_OP_BIT, 0);
	} else {
		regmap_update_bits(consys_pmic_regmap, MT6358_LDO_VCN28_OP_EN,
				   MT6358_LDO_HW0_OP_BIT, 0);
		regmap_update_bits(consys_pmic_regmap, MT6358_LDO_VCN28_OP_CFG,
				   MT6358_LDO_HW0_OP_BIT, 0);
	}
#endif
}

static INT32 consys_hw_vcn28_ctrl(UINT32 enable)
{
#if CONSYS_PMIC_CTRL_ENABLE
	INT32 iRet;

	if (enable) {
		/*in co-clock mode,need to turn on vcn28 when fm on */
		regulator_set_voltage(reg_VCN28, 2800000, 2800000);
		iRet = regulator_enable(reg_VCN28);
		if (iRet) {
			WMT_PLAT_PR_ERR("WMT do VCN28 PMIC on fail!\n");
			return iRet;
		}
		WMT_PLAT_PR_INFO("turn on vcn28 for fm/gps usage in co-clock mode\n");
	} else {
		/*in co-clock mode,need to turn off vcn28 when fm off */
		regulator_disable(reg_VCN28);
		WMT_PLAT_PR_INFO("turn off vcn28 for fm/gps usage in co-clock mode\n");
	}
#endif
	return 0;
}

static INT32 consys_hw_bt_vcn33_ctrl(UINT32 enable)
{
	/* vcn33 is on whenever the CONSYS is, see consys_hw_vcn18_ctrl() */
	return 0;
}

static INT32 consys_hw_wifi_vcn33_ctrl(UINT32 enable)
{
	/* vcn33 is on whenever the CONSYS is, see consys_hw_vcn18_ctrl() */
	return 0;
}

static INT32 consys_emi_mpu_set_region_protection(VOID)
{
	/*
	 * MediaTek's kernel set up a region of the EMI's memory protection
	 * unit here, for only the AP and the CONSYS to have access to the
	 * shared memory. That is an optional call into the secure firmware,
	 * and nothing here depends on it.
	 */
	return 0;
}

static UINT32 consys_emi_set_remapping_reg(VOID)
{
	UINT32 addrPhy = 0;
	UINT32 value = 0;

	/*consys to ap emi remapping register:10000380, cal remapping address */
	addrPhy = (gConEmiPhyBase >> 21) & CONSYS_EMI_MAPPING_ADDR_MASK;

	/*enable consys to ap emi remapping bit13 */
	addrPhy = addrPhy | CONSYS_EMI_MAPPING_EN_BIT;

	regmap_update_bits(conn_reg.infracfg_regmap, CONSYS_EMI_MAPPING_OFFSET,
			   CONSYS_EMI_MAPPING_EN_BIT | CONSYS_EMI_MAPPING_ADDR_MASK, addrPhy);

	regmap_read(conn_reg.infracfg_regmap, CONSYS_EMI_MAPPING_OFFSET, &value);
	WMT_PLAT_PR_INFO("CONSYS_EMI_MAPPING dump in restore cb(0x%08x)\n", value);

	return 0;
}

static INT32 bt_wifi_share_v33_spin_lock_init(VOID)
{
#if CONSYS_BT_WIFI_SHARE_V33
	gBtWifiV33.counter = 0;
	spin_lock_init(&gBtWifiV33.lock);
#endif
	return 0;
}

static INT32 consys_read_irq_info_from_dts(struct platform_device *pdev, PINT32 irq_num, PUINT32 irq_flag)
{
	INT32 irq;

	if (!pdev) {
		WMT_PLAT_PR_ERR("[%s] can't find CONSYS compatible node\n", __func__);
		return -1;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		WMT_PLAT_PR_ERR("get irq id from DTS fail(%d)!!\n", irq);
		return -1;
	}
	*irq_num = irq;
	/* get the interrupt line behaviour */
	*irq_flag = irq_get_trigger_type(irq);
	WMT_PLAT_PR_INFO("get irq id(%d) and irq trigger flag(%d) from DT\n", *irq_num,
			   *irq_flag);

	return 0;
}

static INT32 consys_read_reg_from_dts(struct platform_device *pdev)
{
	UINT8 __iomem *base;

	/* registers base address */
	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	conn_reg.mcu_base = (SIZE_T) base;
	WMT_PLAT_PR_DBG("Get mcu register base(0x%zx)\n", conn_reg.mcu_base);

#if CONSYS_AFE_REG_SETTING
	consys_afe_reg_base = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(consys_afe_reg_base))
		return PTR_ERR(consys_afe_reg_base);
#endif

	/*
	 * The watchdog (ap_rgu), INFRACFG_AO (topckgen) and the SPM have their
	 * own drivers, and are not mapped here. Of them only INFRACFG_AO has
	 * registers that no framework covers.
	 */
	conn_reg.infracfg_regmap = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
								   "mediatek,infracfg");
	if (IS_ERR(conn_reg.infracfg_regmap)) {
		WMT_PLAT_PR_ERR("cannot get the INFRACFG_AO regmap\n");
		return PTR_ERR(conn_reg.infracfg_regmap);
	}

	return 0;
}

static VOID force_trigger_assert_debug_pin(VOID)
{
	UINT32 value = 0;

	regmap_update_bits(conn_reg.infracfg_regmap, CONSYS_AP2CONN_OSC_EN_OFFSET,
			   CONSYS_AP2CONN_WAKEUP_BIT, 0);
	regmap_read(conn_reg.infracfg_regmap, CONSYS_AP2CONN_OSC_EN_OFFSET, &value);
	WMT_PLAT_PR_INFO("enable:dump CONSYS_AP2CONN_OSC_EN_REG(0x%x)\n", value);
	usleep_range(64, 96);
	regmap_update_bits(conn_reg.infracfg_regmap, CONSYS_AP2CONN_OSC_EN_OFFSET,
			   CONSYS_AP2CONN_WAKEUP_BIT, CONSYS_AP2CONN_WAKEUP_BIT);
	regmap_read(conn_reg.infracfg_regmap, CONSYS_AP2CONN_OSC_EN_OFFSET, &value);
	WMT_PLAT_PR_INFO("disable:dump CONSYS_AP2CONN_OSC_EN_REG(0x%x)\n", value);
}

static UINT32 consys_read_cpupcr(VOID)
{
	return consys_mcu_reg_read(CONSYS_CPUPCR_OFFSET);
}

static INT32 consys_check_reg_readable(VOID)
{
	return consys_domain_on ? 1 : 0;
}

static INT32 consys_is_connsys_reg(UINT32 addr)
{
	return (addr >= CONSYS_REG_PHY_BASE &&
		addr - CONSYS_REG_PHY_BASE < CONSYS_REG_PHY_SIZE) ? 1 : 0;
}

static UINT32 consys_soc_chipid_get(VOID)
{
	return PLATFORM_SOC_CHIP;
}

static P_CONSYS_EMI_ADDR_INFO consys_soc_get_emi_phy_add(VOID)
{
	return &mtk_wcn_emi_addr_info;
}

P_WMT_CONSYS_IC_OPS mtk_wcn_get_consys_ic_ops(VOID)
{
	return &consys_ic_ops;
}

static INT32 consys_emi_coredump_remapping(UINT8 __iomem **addr, UINT32 enable)
{
	if (enable) {
		*addr = ioremap(gConEmiPhyBase + CONSYS_EMI_COREDUMP_OFFSET, CONSYS_EMI_MEM_SIZE);
		if (*addr) {
			WMT_PLAT_PR_INFO("COREDUMP EMI mapping OK virtual(0x%p) physical(0x%x)\n",
					   *addr, (UINT32) gConEmiPhyBase + CONSYS_EMI_COREDUMP_OFFSET);
			memset_io(*addr, 0, CONSYS_EMI_MEM_SIZE);
		} else {
			WMT_PLAT_PR_ERR("EMI mapping fail\n");
			return -1;
		}
	} else {
		if (*addr) {
			iounmap(*addr);
			*addr = NULL;
		}
	}
	return 0;
}

static INT32 consys_reset_emi_coredump(UINT8 __iomem *addr)
{
	if (!addr) {
		WMT_PLAT_PR_ERR("get virtual address fail\n");
		return -1;
	}
	WMT_PLAT_PR_INFO("Reset EMI(0xF0080000 ~ 0xF0080400) and (0xF0088400 ~ 0xF0090400)\n");
	/* reset 0xF0080000 ~ 0xF0080400 (1K) */
	memset_io(addr, 0, 0x400);
	/* reset 0xF0088400 ~ 0xF0090400 (32K)  */
	memset_io(addr + CONSYS_EMI_PAGED_DUMP_OFFSET, 0, 0x8000);
	return 0;
}
