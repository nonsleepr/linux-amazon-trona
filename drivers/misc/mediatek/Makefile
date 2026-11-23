# SPDX-License-Identifier: GPL-2.0-only
#
# MediaTek's vendor connectivity stack
#

# The vendor code was written for other warning levels than the kernel's...
subdir-ccflags-y += -Wno-missing-prototypes -Wno-missing-declarations
subdir-ccflags-y += -Wno-implicit-fallthrough -Wno-undef -Wno-format
subdir-ccflags-y += -Wno-unused-variable -Wno-unused-function
subdir-ccflags-y += -Wno-enum-conversion
# ...and for kernels without kCFI: a function called through a pointer of
# another type is a crash here, so have the compiler point every cast out.
# Clang-only warnings (GCC has no equivalent) - guard with cc-option so a
# GCC cross build doesn't hard-fail on an unrecognized flag.
subdir-ccflags-y += $(call cc-option,-Wcast-function-type-strict)
subdir-ccflags-y += $(call cc-option,-Wincompatible-function-pointer-types-strict)

obj-$(CONFIG_MTK_BTIF)	+= btif/
obj-$(CONFIG_MTK_COMBO)	+= connectivity/
