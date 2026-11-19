/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Stand-in for the vendor kernel's <sync_write.h>: register writes that are
 * followed by a memory barrier. The BTIF driver keeps its register addresses
 * in unsigned longs.
 */

#ifndef _MT_SYNC_WRITE_H
#define _MT_SYNC_WRITE_H

#include <linux/io.h>

#define mt_reg_sync_writel(v, a) \
	do {    \
		__raw_writel((v), (void __force __iomem *)((a)));   \
		/* add memory barrier */ \
		mb();  \
	} while (0)

#define mt_reg_sync_writew(v, a) \
	do {    \
		__raw_writew((v), (void __force __iomem *)((a)));   \
		/* add memory barrier */ \
		mb();  \
	} while (0)

#define mt_reg_sync_writeb(v, a) \
	do {    \
		__raw_writeb((v), (void __force __iomem *)((a)));   \
		/* add memory barrier */ \
		mb();  \
	} while (0)

#endif				/* !_MT_SYNC_WRITE_H */
