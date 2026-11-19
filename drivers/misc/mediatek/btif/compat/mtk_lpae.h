/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Stand-in for the vendor kernel's <mtk_lpae.h>. enable_4G() tells drivers
 * that the SoC runs in MediaTek's "4GB mode", in which DMA addresses need an
 * extra bit set. The vendor kernel for MT8183 has only the weak default,
 * which returns 0, and so does this: the BTIF driver programs the address of
 * its vFIFOs as it is, and the device keeps the default 32-bit DMA mask.
 */

#ifndef __MTK_LPAE_H__
#define __MTK_LPAE_H__

static inline unsigned int enable_4G(void)
{
	return 0;
}

#endif  /*!__MTK_LPAE_H__ */
