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
 * \brief  Declaration of library functions
 *
 * Any definitions in this file will be shared among GLUE Layer and internal Driver Stack.
*/

#ifndef _MTK_MT8183_H_
#define _MTK_MT8183_H_

/*******************************************************************************
*                         C O M P I L E R   F L A G S
********************************************************************************
*/

#define CONSYS_BT_WIFI_SHARE_V33	0
#define CONSYS_PMIC_CTRL_ENABLE		1
#define CONSYS_AHB_CLK_MAGEMENT		1
#define CONSYS_USE_PLATFORM_WRITE	1
#define CONSYS_AFE_REG_SETTING		1

/*******************************************************************************
*                                 M A C R O S
********************************************************************************
*/

/*tag start:new platform need to make sure these define */
#define PLATFORM_SOC_CHIP 0x6771
/*tag end*/

/*device tree mode*/
/*
 * The A-Die interface pins, the MCU's reset (AP_RGU) and the power domain
 * (SPM) are not registers here: they belong to the pin controller's "active"
 * state, the "mcu" reset control and the power domain of the device.
 */

/*device tree property names*/
#define CONSYS_PINCTRL_STATE_ACTIVE	"active"
#define CONSYS_RESET_NAME_MCU		"mcu"
#define CONSYS_CLK_NAME_PMIC_CONN	"pmic-conn"
#define CONSYS_GPIO_NAME_COCLOCK	"coclock"

/*TOPCKGEN_BASE (INFRACFG_AO, through its syscon regmap)*/
#define CONSYS_AP2CONN_OSC_EN_OFFSET	0x00000f00
#define CONSYS_EMI_MAPPING_OFFSET	0x00000380
/*CONSYS_EMI_MAPPING_REG: the shared memory's address in 2 MiB units*/
#define CONSYS_EMI_MAPPING_ADDR_MASK	0x00001FFF
#define CONSYS_EMI_MAPPING_EN_BIT	(0x1 << 13)
/*CONSYS registers as the SoC's bus sees them*/
#define CONSYS_REG_PHY_BASE		0x18000000
#define CONSYS_REG_PHY_SIZE		0x01000000
/*CONN_MCU_CONFIG_BASE*/
#define CONSYS_CHIP_ID_OFFSET		0x00000008
#define CONSYS_ROM_DESEL_OFFSET        0x00000600
#define CONSYS_HANG_DBG_OFFSET_1	0x00000400
#define CONSYS_HANG_DBG_OFFSET_2	0x00000404
#define CONSYS_HANG_DBG_OFFSET_3	0x0000040C
#define CONSYS_MCU_CFG_ACR_OFFSET	0x00000140
#define CONSYS_CPUPCR_OFFSET		0x00000160

#if CONSYS_AFE_REG_SETTING
/*CONN AFE, the second "reg" entry of the device*/
#define CONSYS_AFE_REG_BASE			(0x180B6000)
#define CONSYS_AFE_REG_SIZE			(0x00000100)
#define CONSYS_AFE_RG_WBG_PLL_03_OFFSET		(0x00000038)
#define CONSYS_AFE_RG_WBG_PLL_03_VALUE		(0x000C1DF0)
#define CONSYS_AFE_RG_WBG_GPS_02_OFFSET		(0x00000054)
#define CONSYS_AFE_RG_WBG_GPS_02_VALUE		(0x110A2000)
#endif

/*CONSYS_CHIP_ID_REG*/
#define CONSYS_CHIP_ID_VALUE		0x0788

/*
 * MT6358 PMIC: who switches each LDO the CONSYS uses. Bit 0 is software (the
 * regulator driver), bit 1 a hardware request ("HW0"), which is the CONSYS's.
 */
#define MT6358_LDO_VCN18_OP_EN		0x1c5a
#define MT6358_LDO_VCN33_OP_EN		0x1d1e
#define MT6358_LDO_VCN28_OP_EN		0x1d8a
#define MT6358_LDO_VCN28_OP_CFG		0x1d90
#define MT6358_LDO_SW_OP_EN_BIT		(0x1 << 0)
#define MT6358_LDO_HW0_OP_BIT		(0x1 << 1)

/*CONSYS_MCU_CFG_ACR_REG*/
#define CONSYS_MCU_CFG_ACR_MBIST_BIT	(0x1 << 0 | 0x1 << 1)

/*control app2cnn_osc_en*/
#define CONSYS_AP2CONN_OSC_EN_BIT	(0x1 << 10)
#define CONSYS_AP2CONN_WAKEUP_BIT	(0x1 << 9)
#define CONSYS_ROM_DESEL_MASK		((0x17 << 16) | 0x2a4)

/* EMI part mapping & ctrl*/
#define CONSYS_EMI_COREDUMP_OFFSET	(0x80000)
#define CONSYS_EMI_AP_PHY_BASE		(0x80080000)
#define CONSYS_EMI_FW_PHY_BASE		(0xf0080000)
#define CONSYS_EMI_PAGED_TRACE_OFFSET	(0x400)
#define CONSYS_EMI_PAGED_DUMP_OFFSET	(0x8400)
#define CONSYS_EMI_FULL_DUMP_OFFSET	(0x10400)

/*******************************************************************************
*                    E X T E R N A L   R E F E R E N C E S
********************************************************************************
*/

/*******************************************************************************
*                              C O N S T A N T S
********************************************************************************
*/

/*******************************************************************************
*                             D A T A   T Y P E S
********************************************************************************
*/

#ifdef CONSYS_BT_WIFI_SHARE_V33
struct bt_wifi_v33_status {
	UINT32 counter;
	UINT32 flags;
	spinlock_t lock;
};

extern struct bt_wifi_v33_status gBtWifiV33;
#endif

/*******************************************************************************
*                            P U B L I C   D A T A
*******************************************************************************
*/

/*******************************************************************************
*                           P R I V A T E   D A T A
********************************************************************************
*/

/*******************************************************************************
*                  F U N C T I O N   D E C L A R A T I O N S
********************************************************************************
*/

/*******************************************************************************
*                              F U N C T I O N S
********************************************************************************
*/

#endif /* _MTK_MT8183_H_ */
