/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek's kernels have a "built-in adapter" that re-exports kernel
 * symbols which modules cannot reach, for the connectivity drivers. This
 * takes the place of that adapter's header (the one in connectivity/common)
 * for the WLAN driver, which only wants show_stack() from it, for one of its
 * threads when that looks stuck: sched_show_task() does that and is exported.
 *
 * The adapter's event_trace_printk() is only used by the MET packet trace
 * (CFG_MET_PACKET_TRACE_SUPPORT), which is not built.
 */

#ifndef _WLAN_COMPAT_CONNECTIVITY_BUILD_IN_ADAPTER_H
#define _WLAN_COMPAT_CONNECTIVITY_BUILD_IN_ADAPTER_H

#include <linux/sched.h>
#include <linux/sched/debug.h>

#define KERNEL_show_stack(tsk, sp)	sched_show_task(tsk)

#endif /* _WLAN_COMPAT_CONNECTIVITY_BUILD_IN_ADAPTER_H */
