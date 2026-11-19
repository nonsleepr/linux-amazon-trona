/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek's RTC driver can put the PMIC's 32 kHz clock out on a pin for an
 * external combo chip. The subsystem built into the SoC has no use for it.
 */

#ifndef _CONN_COMPAT_MTK_RTC_H_
#define _CONN_COMPAT_MTK_RTC_H_

enum rtc_gpio_user_t {
	RTC_GPIO_USER_WIFI = 8,
	RTC_GPIO_USER_GPS = 9,
	RTC_GPIO_USER_BT = 10,
	RTC_GPIO_USER_FM = 11,
	RTC_GPIO_USER_PMIC = 12,
};

static inline void rtc_gpio_enable_32k(enum rtc_gpio_user_t user)
{
}

static inline void rtc_gpio_disable_32k(enum rtc_gpio_user_t user)
{
}

#endif /* _CONN_COMPAT_MTK_RTC_H_ */
