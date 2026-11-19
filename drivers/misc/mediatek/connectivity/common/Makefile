# SPDX-License-Identifier: GPL-2.0-only
#
# MediaTek WMT core (wmt_drv). The object list and the defines are those of
# the module MediaTek's Android build made from these sources.
#

ccflags-y += -I$(src)
ccflags-y += -I$(src)/../compat
ccflags-y += -I$(src)/common_main/include
ccflags-y += -I$(src)/common_main/linux/include
ccflags-y += -I$(src)/common_main/core/include
ccflags-y += -I$(src)/common_main/platform/include
ccflags-y += -I$(src)/common_detect
ccflags-y += -I$(src)/common_detect/drv_init/inc
ccflags-y += -I$(src)/debug_utility
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat
ccflags-y += -I$(srctree)/drivers/misc/mediatek/btif/common/inc

ccflags-y += -DWMT_PLAT_ALPS=1
ccflags-y += -DWMT_UART_RX_MODE_WORK=0
ccflags-y += -DWMT_SDIO_MODE=1
ccflags-y += -DWMT_CREATE_NODE_DYNAMIC=1
ccflags-y += -DMTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT
ccflags-y += -DWMT_IDC_SUPPORT=0
ccflags-y += -DCFG_WMT_STEP
ccflags-y += -DWMT_DBG_SUPPORT=0

# The sources test this one without the CONFIG_ prefix
ccflags-$(CONFIG_MTK_WCN_COMBO_CHIP_SUPPORT) += -DMTK_WCN_COMBO_CHIP_SUPPORT

obj-$(CONFIG_MTK_COMBO) += wmt_drv.o

wmt_drv-y += common_detect/wmt_detect.o
wmt_drv-y += common_detect/mtk_wcn_stub_alps.o
wmt_drv-y += common_detect/wmt_gpio.o
wmt_drv-y += common_detect/drv_init/fm_drv_init.o
wmt_drv-y += common_detect/drv_init/bluetooth_drv_init.o
wmt_drv-y += common_detect/drv_init/wlan_drv_init.o
wmt_drv-y += common_detect/drv_init/gps_drv_init.o

wmt_drv-y += common_main/platform/mt8183.o
wmt_drv-y += common_main/platform/wmt_plat_alps.o
wmt_drv-y += common_main/platform/mtk_wcn_consys_hw.o

wmt_drv-y += common_main/core/wmt_conf.o
wmt_drv-y += common_main/core/stp_core.o
wmt_drv-y += common_main/core/wmt_ctrl.o
wmt_drv-y += common_main/core/wmt_func.o
wmt_drv-y += common_main/core/wmt_core.o
wmt_drv-y += common_main/core/psm_core.o
wmt_drv-y += common_main/core/wmt_ic_soc.o
wmt_drv-y += common_main/core/wmt_lib.o
wmt_drv-y += common_main/core/stp_exp.o
wmt_drv-y += common_main/core/wmt_exp.o
wmt_drv-y += common_main/core/btm_core.o

wmt_drv-y += common_main/linux/stp_dbg_soc.o
wmt_drv-y += common_main/linux/osal.o
wmt_drv-y += common_main/linux/wmt_dev.o
wmt_drv-y += common_main/linux/bgw_desense.o
wmt_drv-y += common_main/linux/wmt_dbg.o
wmt_drv-y += common_main/linux/stp_dbg.o
wmt_drv-y += common_main/linux/wmt_step.o
wmt_drv-y += common_main/linux/stp_btif.o

# Not in MediaTek's source drop; rewritten from what their module does
wmt_drv-y += debug_utility/ring.o

# What MediaTek's kernel had built in for this module
wmt_drv-y += wmt_build_in_adapter.o
wmt_drv-y += ../compat/conn_compat.o

# External MT66xx combo chips on SDIO or UART
wmt_drv-$(CONFIG_MTK_WCN_COMBO_CHIP_SUPPORT) += common_detect/sdio_detect.o
wmt_drv-$(CONFIG_MTK_WCN_COMBO_CHIP_SUPPORT) += common_detect/wmt_detect_pwr.o
wmt_drv-$(CONFIG_MTK_WCN_COMBO_CHIP_SUPPORT) += common_main/platform/mtk_wcn_cmb_hw.o
wmt_drv-$(CONFIG_MTK_WCN_COMBO_CHIP_SUPPORT) += common_main/core/wmt_ic_6620.o
wmt_drv-$(CONFIG_MTK_WCN_COMBO_CHIP_SUPPORT) += common_main/core/wmt_ic_6628.o
wmt_drv-$(CONFIG_MTK_WCN_COMBO_CHIP_SUPPORT) += common_main/core/wmt_ic_6630.o
wmt_drv-$(CONFIG_MTK_WCN_COMBO_CHIP_SUPPORT) += common_main/core/wmt_ic_6632.o
wmt_drv-$(CONFIG_MTK_WCN_COMBO_CHIP_SUPPORT) += common_main/linux/hif_sdio.o
wmt_drv-$(CONFIG_MTK_WCN_COMBO_CHIP_SUPPORT) += common_main/linux/stp_sdio.o
wmt_drv-$(CONFIG_MTK_WCN_COMBO_CHIP_SUPPORT) += common_main/linux/stp_uart.o
wmt_drv-$(CONFIG_MTK_WCN_COMBO_CHIP_SUPPORT) += common_main/linux/stp_dbg_combo.o
