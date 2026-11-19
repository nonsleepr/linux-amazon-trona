/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Kernel interfaces that MediaTek's WMT driver was written against and that
 * are gone, for the source files that still use them. Not to be included from
 * a header: the WiFi driver includes the WMT driver's, and has its own ways.
 */

#ifndef _MTK_CONN_COMPAT_H_
#define _MTK_CONN_COMPAT_H_

#include <linux/ktime.h>
#include <linux/time64.h>

/* struct timeval and do_gettimeofday() went away with v5.6 */
struct timeval {
	long tv_sec;
	long tv_usec;
};

static inline void do_gettimeofday(struct timeval *tv)
{
	struct timespec64 now;

	ktime_get_real_ts64(&now);
	tv->tv_sec = now.tv_sec;
	tv->tv_usec = now.tv_nsec / NSEC_PER_USEC;
}

#endif /* _MTK_CONN_COMPAT_H_ */
