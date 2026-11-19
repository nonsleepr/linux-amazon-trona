/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * What wmt_chrdev_wifi exports to the WLAN driver.
 *
 * The WLAN driver used to declare all of this a second time, with types of
 * its own. Its handlers are called through pointers from here, so with kCFI
 * both sides must use the very same types: both include this header now.
 */

#ifndef _WMT_CDEV_WIFI_H_
#define _WMT_CDEV_WIFI_H_

#include <linux/compiler_types.h>
#include <linux/types.h>

struct net_device;

enum ENUM_RESET_STATUS {
	RESET_FAIL,
	RESET_SUCCESS
};

/*
 *  enable = 1, mode = 0  => init P2P network
 *  enable = 1, mode = 1  => init Soft AP network
 *  enable = 0  => uninit P2P/AP network
 */
struct PARAM_CUSTOM_P2P_SET_STRUCT {
	uint32_t u4Enable;
	uint32_t u4Mode;
};

typedef int32_t(*set_p2p_mode) (struct net_device *netdev, struct PARAM_CUSTOM_P2P_SET_STRUCT p2pmode);
void register_set_p2p_mode_handler(set_p2p_mode handler);

typedef uint8_t(*file_buf_handler)(void *ctx, const char __user *buf, uint16_t length);
void register_file_buf_handler(file_buf_handler handler, void *handler_ctx, uint8_t ucType);

int32_t wifi_reset_start(void);
int32_t wifi_off_start(void);
int32_t wifi_reset_end(enum ENUM_RESET_STATUS status);

/* What writing "1" and "0" to /dev/wmtWifi does, for callers in the kernel */
int wifi_func_on(void);
int wifi_func_off(void);

#endif /* _WMT_CDEV_WIFI_H_ */
