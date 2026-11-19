/*
* Copyright (C) 2016 MediaTek Inc.
*
* This program is free software: you can redistribute it and/or modify it under the terms of the
* GNU General Public License version 2 as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
* without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
* See the GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along with this program.
* If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * Id: //Department/DaVinci/BRANCHES/MT6620_WIFI_DRIVER_V2_3/os/linux/include/gl_rst.h#1
 */

/*
 * ! \file   gl_rst.h
 *   \brief  Declaration of functions and finite state machine for
 *   MT6620 Whole-Chip Reset Mechanism
 */

#ifndef _GL_RST_H
#define _GL_RST_H

/*******************************************************************************
*                         C O M P I L E R   F L A G S
********************************************************************************
*/

/*******************************************************************************
*                    E X T E R N A L   R E F E R E N C E S
********************************************************************************
*/
#include "gl_typedef.h"

/*
 * This file used to have copies of the WMT core's types and prototypes "for
 * better driver isolation". They come from the WMT core's own header now, see
 * gl_os.h for why.
 */
#include "wmt_exp.h"
#include "wmt_cdev_wifi.h"

/*******************************************************************************
*                              C O N S T A N T S
********************************************************************************
*/
#define RST_FLAG_CHIP_RESET          0
#define RST_FLAG_DO_CORE_DUMP        BIT(0)
#define RST_FLAG_PREVENT_POWER_OFF   BIT(1)
/*******************************************************************************
*                             D A T A   T Y P E S
********************************************************************************
*/

/* RESET_FAIL and RESET_SUCCESS, from wmt_cdev_wifi.h */
typedef enum ENUM_RESET_STATUS ENUM_RESET_STATUS_T;

typedef struct _RESET_STRUCT_T {
	ENUM_RESET_STATUS_T rst_data;
	struct work_struct rst_work;
	struct work_struct rst_trigger_work;
	UINT_32 rst_trigger_flag;
#if CFG_WLAN_RESET_SUPPORT
	struct work_struct wlan_rst_work;
#endif
} RESET_STRUCT_T;

/*******************************************************************************
*                    E X T E R N A L   F U N C T I O N S
********************************************************************************
*/
/*
 * The WMT core's types and functions for message callbacks, chip resets and
 * the registration of the WLAN driver are in wmt_exp.h, wmt_plat_read_cpupcr()
 * is in wmt_plat.h; wifi_reset_start(), wifi_off_start() and wifi_reset_end()
 * are in wmt_cdev_wifi.h
 */

/*******************************************************************************
*                            P U B L I C   D A T A
********************************************************************************
*/

/*******************************************************************************
*                           P R I V A T E   D A T A
********************************************************************************
*/

/*******************************************************************************
*                                 M A C R O S
********************************************************************************
*/
#if CFG_CHIP_RESET_SUPPORT
#define GL_RESET_TRIGGER(_prAdapter, _u4Flags) \
	glResetTrigger(_prAdapter, (_u4Flags), (const PUINT_8)__FILE__, __LINE__)
#else
#define GL_RESET_TRIGGER(_prAdapter, _u4Flags) \
	DBGLOG(INIT, INFO, "DO NOT support chip reset\n")
#endif

#if CFG_WLAN_RESET_SUPPORT
#define GL_WLAN_RESET_TRIGGER() \
	glWlanResetTrigger((const PUINT_8)__FILE__, __LINE__)
#else
#define GL_WLAN_RESET_TRIGGER() \
	DBGLOG(INIT, INFO, "DO NOT support WLAN reset\n")
#endif

/*******************************************************************************
*                  F U N C T I O N   D E C L A R A T I O N S
********************************************************************************
*/
VOID glResetInit(VOID);

VOID glResetUninit(VOID);

VOID glSendResetRequest(VOID);

BOOLEAN kalIsResetting(VOID);

BOOLEAN kalIsResetTriggered(VOID);

BOOLEAN glResetTrigger(P_ADAPTER_T prAdapter, UINT_32 u4RstFlag, const PUINT_8 pucFile, UINT_32 u4Line);

UINT32 wlanPollingCpupcr(UINT32 u4Times, UINT32 u4Sleep);

WLAN_STATUS wlanGetCpupcr(PUINT32 pu4Cpupcr);
#if CFG_WLAN_RESET_SUPPORT
BOOLEAN glWlanResetTrigger(const PUINT_8 pucFile, UINT_32 u4Line);
#endif
#endif /* _GL_RST_H */
