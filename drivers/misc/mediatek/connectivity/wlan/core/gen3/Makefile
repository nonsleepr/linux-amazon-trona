# SPDX-License-Identifier: GPL-2.0-only
#
# MediaTek's "gen3" WLAN driver for the connectivity subsystem (MT6631 RF,
# AHB host interface with a CQDMA channel).
#
# The vendor's source drop has no build files. The object list and the -D
# flags below were reconstructed from the module it shipped; the QA/ATE tool
# agents, the PDMA, SDIO and eHPI host interfaces, sec_fsm.c and p2p_state.c
# are not part of that module.
#
# _HIF_SDIO is not defined: that module has the register access of hal.h's
# other branch (HAL_MCR_HIF_RD() and kalDumpAhbDebugInfo()), and with it set
# gl_os.h would include the MMC core's struct sdio_func on top of the one in
# hif/ahb_sdioLike/include/sdio.h.
#
# That module also has the MET packet trace (CFG_MET_PACKET_TRACE_SUPPORT=1).
# It marks packets in the ftrace buffer through event_trace_printk() and the
# address of tracing_mark_write(), which modules cannot look up any more, so
# it is left out here.
#

WMT_SRC := $(srctree)/drivers/misc/mediatek/connectivity/common

ccflags-y += -I$(src)/../../compat
ccflags-y += -I$(src)/os -I$(src)/os/linux/include
ccflags-y += -I$(src)/os/linux/hif/ahb_sdioLike/include
ccflags-y += -I$(src)/include -I$(src)/include/nic -I$(src)/include/mgmt

# What wmt_chrdev_wifi exports
ccflags-y += -I$(src)/../../adaptor

# The WMT core's exported headers, and what they include in turn
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat
ccflags-y += -I$(WMT_SRC)/common_main/include
ccflags-y += -I$(WMT_SRC)/common_main/linux/include
ccflags-y += -I$(WMT_SRC)/../compat

# As the WMT core is built: its headers only have the types with this set
ccflags-y += -DMTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT

ccflags-y += -DLINUX -DMT6631 -DDBG=0 -DBUILD_QA_DBG=0
ccflags-y += -DWLAN_INCLUDE_PROC -DFW_CFG_SUPPORT
ccflags-y += -DCFG_SUPPORT_AGPS_ASSIST=1
ccflags-y += -DCFG_SUPPORT_TSF_USING_BOOTTIME=1
ccflags-y += -DCFG_SUPPORT_DEBUG_FS=1
ccflags-y += -DARP_MONITER_ENABLE=0
ccflags-y += -DCFG_SUPPORT_CHNL_CONFLICT_REVISE=0
ccflags-y += -DCFG_SUPPORT_AEE=0
ccflags-y += -DCFG_SUPPORT_PASSPOINT=0 -DCFG_HS20_DEBUG=0
ccflags-y += -DCFG_ENABLE_GTK_FRAME_FILTER=0
ccflags-y += -DCFG_MET_PACKET_TRACE_SUPPORT=0
ccflags-y += -DCFG_SUPPORT_VO_ENTERPRISE=1
ccflags-y += -DCFG_SUPPORT_DFS_CHANNEL=1
ccflags-y += -DCFG_PWR_LIMIT_COUNTRY=1
ccflags-y += -DCFG_ENABLE_RX_STATUS_G3
ccflags-y += -DCFG_SUPPORT_WIFI_FW_LOG_UI=0

obj-$(CONFIG_MTK_COMBO_WIFI)	+= wlan_drv_gen3.o

wlan_drv_gen3-y	:= os/linux/plat/mt8183/plat_priv.o

wlan_drv_gen3-y	+= common/dump.o \
		   common/wlan_lib.o \
		   common/wlan_oid.o \
		   common/wlan_bow.o \
		   common/debug.o \
		   common/wlan_p2p.o \
		   common/fwcfg.o

wlan_drv_gen3-y	+= nic/nic.o \
		   nic/nic_tx.o \
		   nic/nic_rx.o \
		   nic/nic_pwr_mgt.o \
		   nic/nic_rate.o \
		   nic/cmd_buf.o \
		   nic/que_mgt.o \
		   nic/nic_cmd_event.o \
		   nic/p2p_nic.o

wlan_drv_gen3-y	+= os/linux/gl_init.o \
		   os/linux/gl_kal.o \
		   os/linux/gl_bow.o \
		   os/linux/gl_wext.o \
		   os/linux/gl_wext_priv.o \
		   os/linux/gl_proc.o \
		   os/linux/gl_rst.o \
		   os/linux/gl_cfg80211.o \
		   os/linux/gl_vendor.o \
		   os/linux/platform.o \
		   os/linux/gl_p2p.o \
		   os/linux/gl_p2p_cfg80211.o \
		   os/linux/gl_p2p_init.o \
		   os/linux/gl_p2p_kal.o

wlan_drv_gen3-y	+= os/linux/hif/ahb_sdioLike/ahb.o \
		   os/linux/hif/ahb_sdioLike/sdio_bus_driver.o \
		   os/linux/hif/ahb_sdioLike/ahb_cqdma.o

wlan_drv_gen3-y	+= mgmt/ais_fsm.o \
		   mgmt/aaa_fsm.o \
		   mgmt/assoc.o \
		   mgmt/auth.o \
		   mgmt/bss.o \
		   mgmt/cnm.o \
		   mgmt/cnm_timer.o \
		   mgmt/cnm_mem.o \
		   mgmt/hem_mbox.o \
		   mgmt/mib.o \
		   mgmt/privacy.o \
		   mgmt/rate.o \
		   mgmt/rlm.o \
		   mgmt/rlm_domain.o \
		   mgmt/rlm_obss.o \
		   mgmt/rsn.o \
		   mgmt/saa_fsm.o \
		   mgmt/scan.o \
		   mgmt/scan_fsm.o \
		   mgmt/swcr.o \
		   mgmt/roaming_fsm.o \
		   mgmt/tkip_mic.o \
		   mgmt/tdls.o \
		   mgmt/wnm.o \
		   mgmt/wmm.o \
		   mgmt/stats.o \
		   mgmt/p2p_dev_fsm.o \
		   mgmt/p2p_dev_state.o \
		   mgmt/p2p_role_fsm.o \
		   mgmt/p2p_role_state.o \
		   mgmt/p2p_func.o \
		   mgmt/p2p_scan.o \
		   mgmt/p2p_ie.o \
		   mgmt/p2p_rlm.o \
		   mgmt/p2p_assoc.o \
		   mgmt/p2p_rlm_obss.o
