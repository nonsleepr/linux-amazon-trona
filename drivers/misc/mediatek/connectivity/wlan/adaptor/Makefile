# SPDX-License-Identifier: GPL-2.0-only
#
# /dev/wmtWifi: switches the WiFi function of the connectivity subsystem on
# and off through the WMT core. The object list and flags are those of the
# vendor's module (fw_log_wifi.c is only built for the dedicated log path).
#

WMT_SRC := $(srctree)/drivers/misc/mediatek/connectivity/common

# The WMT core's exported headers, and what they include in turn
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat
ccflags-y += -I$(WMT_SRC)/common_main/include
ccflags-y += -I$(WMT_SRC)/common_main/linux/include
ccflags-y += -I$(WMT_SRC)/../compat

# As the WMT core is built: its headers only have the types with this set
ccflags-y += -DMTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT

ccflags-y += -DCREATE_NODE_DYNAMIC=1

obj-$(CONFIG_MTK_COMBO_WIFI)	+= wmt_chrdev_wifi.o

wmt_chrdev_wifi-y		:= wmt_cdev_wifi.o
