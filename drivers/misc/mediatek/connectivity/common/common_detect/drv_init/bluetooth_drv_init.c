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
 * This legacy per-chip "driver init" dispatch (do_connectivity_driver_init()
 * in conn_drv_init.c, which called do_bluetooth_drv_init() below) is dead
 * code in this tree: do_connectivity_driver_init() only exists under
 * "#if (MTK_WCN_REMOVE_KO)", which is only ever 1 if something defines
 * MTK_WCN_REMOVE_KERNEL_MODULE, and nothing in this kernel's build does -
 * see wmt_detect.c's own "#if (MTK_WCN_REMOVE_KO)" guard around its only
 * call site. So this function has never been reachable.
 *
 * It used to also carry a mtk_wcn_stpbt_drv_init() weak stub behind
 * "#ifdef CONFIG_MTK_COMBO_BT" - a symbol that, before the MTK_COMBO_BT
 * Kconfig option and the real driver in bt/adaptor/btmtk_stp.c, didn't
 * exist anywhere in this tree's Kconfig either, so the stub could never
 * even compile in. Now that CONFIG_MTK_COMBO_BT is a real, selectable
 * option, that guard would start compiling the dead stub back in next to
 * the real driver, which would be confusing for no benefit (this hook is
 * still never called); it's removed instead. The real BT driver does not
 * use this hook - like WiFi's wmt_cdev_wifi.c, it is a standalone module
 * that powers its function on itself, via mtk_wcn_wmt_func_on().
 */
int do_bluetooth_drv_init(int chip_id)
{
	WMT_DETECT_PR_INFO("do_bluetooth_drv_init is dead code, see comment; not used\n");
	return 0;
}
