/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register writes followed by a barrier, as MediaTek's kernels define them
 * for arm64 in mt-plat/sync_write.h.
 */

#ifndef _CONN_COMPAT_SYNC_WRITE_H_
#define _CONN_COMPAT_SYNC_WRITE_H_

#include <linux/io.h>

#define mt_reg_sync_writel(v, a) \
	do { \
		__raw_writel((v), (void __force __iomem *)((a))); \
		/* add memory barrier */ \
		mb(); \
	} while (0)

#define mt_reg_sync_writew(v, a) \
	do { \
		__raw_writew((v), (void __force __iomem *)((a))); \
		/* add memory barrier */ \
		mb(); \
	} while (0)

#define mt_reg_sync_writeb(v, a) \
	do { \
		__raw_writeb((v), (void __force __iomem *)((a))); \
		/* add memory barrier */ \
		mb(); \
	} while (0)

#endif /* _CONN_COMPAT_SYNC_WRITE_H_ */
