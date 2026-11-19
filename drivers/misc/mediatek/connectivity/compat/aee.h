/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek's kernels have an "Android Exception Engine" that collects a
 * report (a "DB") when a driver signals a problem. There is none here: what
 * the connectivity drivers would report is logged instead.
 */

#ifndef _CONN_COMPAT_AEE_H_
#define _CONN_COMPAT_AEE_H_

#include <linux/printk.h>

#define DB_OPT_DEFAULT			(0)
#define DB_OPT_FTRACE			(1 << 0)
#define DB_OPT_WCN_ISSUE_INFO		(1 << 19)

/*
 * The callers have logged what they report. They are not careful with msg and
 * its arguments, so those are left alone.
 */
static inline void aee_kernel_warning_api(const char *file, const int line,
					  const int db_opt, const char *module,
					  const char *msg, ...)
{
	pr_warn("[AEE] warning for %s from %s:%d\n", module, file, line);
}

#define aee_kernel_exception_api(file, line, db_opt, module, msg...)	\
	aee_kernel_warning_api(file, line, db_opt, module, msg)

#define aee_kernel_warning(module, msg...)	\
	aee_kernel_warning_api(__FILE__, __LINE__, DB_OPT_DEFAULT, module, msg)
#define aee_kernel_exception(module, msg...)	\
	aee_kernel_exception_api(__FILE__, __LINE__, DB_OPT_DEFAULT, module, msg)

static inline void aee_kernel_dal_show(const char *msg)
{
	pr_info("[AEE] %s", msg);
}

/* A core dump of the connectivity subsystem's firmware: dropped */
static inline void aed_combo_exception_api(const int *log, int log_size,
					   const int *phy, int phy_size,
					   const char *detail, const int db_opt)
{
	pr_warn("[AEE] combo exception: %s (%d bytes of dump dropped)\n",
		detail ? detail : "", phy_size);
}

#define aed_combo_exception(log, log_size, phy, phy_size, detail)	\
	aed_combo_exception_api(log, log_size, phy, phy_size, detail,	\
				DB_OPT_DEFAULT)

#endif /* _CONN_COMPAT_AEE_H_ */
