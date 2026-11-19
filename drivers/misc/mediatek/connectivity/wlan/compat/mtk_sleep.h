/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek's SPM driver tells why the system last woke up, which the WLAN
 * driver uses to log the packet that did it (kalIsWakeupByWlan()). There is
 * no such interface here: the answer is always "not by a wakeup source".
 */

#ifndef _WLAN_COMPAT_MTK_SLEEP_H
#define _WLAN_COMPAT_MTK_SLEEP_H

#include <linux/types.h>

enum {
	WR_NONE = 0,
	WR_UART_BUSY = 1,
	WR_PCM_ASSERT = 2,
	WR_PCM_TIMER = 3,
	WR_WAKE_SRC = 4,
	WR_UNKNOWN = 5,
};

/* The CONN2AP wakeup source's bit in the SPM's wakeup status (R12) */
#define WAKE_SRC_CONN2AP	(1U << 5)

static inline unsigned int slp_get_wake_reason(void)
{
	return WR_NONE;
}

static inline u32 spm_get_last_wakeup_src(void)
{
	return 0;
}

#endif /* _WLAN_COMPAT_MTK_SLEEP_H */
