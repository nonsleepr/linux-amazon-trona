# SPDX-License-Identifier: GPL-2.0-only
#
# hci0: a Bluetooth HCI driver for the connectivity subsystem's BT function.
# Switches BT on through the WMT core the same way wlan/adaptor/
# wmt_cdev_wifi.c switches WiFi on, and frames HCI packets over the WMT
# core's STP transport (common_main/core/stp_core.c, BT_TASK_INDX).
#

WMT_SRC := $(srctree)/drivers/misc/mediatek/connectivity/common

# The WMT core's exported headers, and what they include in turn
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat
ccflags-y += -I$(WMT_SRC)/common_main/include
ccflags-y += -I$(WMT_SRC)/common_main/linux/include

# As the WMT core is built: its headers only have the types with this set
ccflags-y += -DMTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT

obj-$(CONFIG_MTK_COMBO_BT)	+= btmtk_stp.o
