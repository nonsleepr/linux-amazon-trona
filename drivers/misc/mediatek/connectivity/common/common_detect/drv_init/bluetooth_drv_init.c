/*
* Copyright (C) 2011-2014 MediaTek Inc.
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

#ifdef DFT_TAG
#undef DFT_TAG
#endif
#define DFT_TAG         "[BT-MOD-INIT]"

#include "wmt_detect.h"
#include "bluetooth_drv_init.h"

/*
 * The vendor's per-chip driver init hook (conn_drv_init.c, only built with
 * MTK_WCN_REMOVE_KO, which nothing here defines) called a weak
 * mtk_wcn_stpbt_drv_init() stub for the BT adaptor, behind a CONFIG_MTK_COMBO_BT
 * that did not exist. That option is now the HCI driver in bt/adaptor/, a module
 * that switches BT on by itself like wmt_cdev_wifi.c does for WiFi, so there is
 * nothing for this hook to call.
 */
int do_bluetooth_drv_init(int chip_id)
{
	WMT_DETECT_PR_INFO("BT driver init is not done from here\n");
	return -1;
}
