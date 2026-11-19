// SPDX-License-Identifier: GPL-2.0
/*
 * What MediaTek's kernel has built in for its connectivity modules, in
 * connectivity_build_in_adapter.c, and that they still need without it.
 */

#include <linux/export.h>
#include <linux/types.h>

/*
 * The memory the CONSYS shares with the AP ("EMI"). MediaTek's kernel has
 * these from a reserved-memory hook at boot. Here the WMT driver sets them
 * from the memory-region of the CONSYS device when it probes it; the WiFi
 * driver uses them as well, and finds them 0 until then.
 */
phys_addr_t gConEmiPhyBase;
EXPORT_SYMBOL(gConEmiPhyBase);

unsigned long long gConEmiSize;
EXPORT_SYMBOL(gConEmiSize);
