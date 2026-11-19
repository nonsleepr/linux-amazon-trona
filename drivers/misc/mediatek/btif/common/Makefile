# SPDX-License-Identifier: GPL-2.0-only
#
# MediaTek BTIF driver (btif)
#

ccflags-y += -I$(src)/inc
ccflags-y += -I$(src)/plat_inc
# Stand-ins for the vendor kernel's headers
ccflags-y += -I$(src)/../compat

obj-$(CONFIG_MTK_BTIF) += btif.o

btif-y := mtk_btif.o mtk_btif_exp.o btif_dma_plat.o btif_plat.o
