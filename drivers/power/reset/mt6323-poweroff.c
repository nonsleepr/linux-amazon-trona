// SPDX-License-Identifier: GPL-2.0
/*
 * Power off through MediaTek PMIC
 *
 * Copyright (C) 2018 MediaTek Inc.
 *
 * Author: Sean Wang <sean.wang@mediatek.com>
 *
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6397/rtc.h>
#include <linux/bitfield.h>
#include <linux/property.h>
#include <linux/reboot.h>

struct mt6323_pwrc {
	struct device *dev;
	struct regmap *regmap;
	u32 base;
};

struct mt6323_pwrc_data {
	void (*power_off)(struct mt6323_pwrc *pwrc);
	int priority;
};

static int mt6323_rtc_busy_wait(struct mt6323_pwrc *pwrc)
{
	unsigned int val;

	return regmap_read_poll_timeout_atomic(pwrc->regmap,
					       pwrc->base + RTC_BBPU, val,
					       !(val & RTC_BBPU_CBUSY),
					       MTK_RTC_POLL_DELAY_US,
					       MTK_RTC_POLL_TIMEOUT);
}

static void mt6323_power_off(struct mt6323_pwrc *pwrc)
{
	int ret;

	regmap_write(pwrc->regmap, pwrc->base + RTC_BBPU, RTC_BBPU_KEY);
	regmap_write(pwrc->regmap, pwrc->base + RTC_WRTGR_MT6323, 1);

	ret = mt6323_rtc_busy_wait(pwrc);
	if (ret)
		dev_err(pwrc->dev, "failed to write BBPU: %d\n", ret);
}

/*
 * MT6358, as upstream TF-A powers it off for MT8183 SYSTEM_OFF. MediaTek's
 * own TF-A for the SoC leaves it to the kernel, where SYSTEM_OFF just hangs.
 */
#define MT6358_RTC_AL_YEA		0x24
#define MT6358_RTC_OSC32CON		0x26
#define MT6358_RTC_PROT			0x34

#define MT6358_RTC_BBPU_PWREN		BIT(0)
#define MT6358_RTC_BBPU_CLR		BIT(1)
#define MT6358_RTC_BBPU_RELOAD		BIT(5)
#define MT6358_RTC_AL_SEC_AUTO_PDN_SEL	BIT(6)
#define MT6358_RTC_AL_SEC_2SEC_EN	BIT(8)
#define MT6358_RTC_K_EOSC_RSV_0		BIT(8)
#define MT6358_RTC_K_EOSC_RSV_1		BIT(9)
#define MT6358_RTC_K_EOSC_RSV_2		BIT(10)
#define MT6358_RTC_EMBCK_SRC_SEL	BIT(8)

#define MT6358_TOP_CON			0x001e
#define MT6358_SRCLKEN_IN_HW_MODE	(BIT(1) | BIT(3))
#define MT6358_SCK_TOP_CKPDN_CON0	0x051a
#define MT6358_RTC_EOSC32_CK_PDN	BIT(2)
#define MT6358_EOSC_CALI_CON0		0x0540
#define MT6358_EOSC_CALI_TD		GENMASK(7, 5)
#define MT6358_DCXO_CW02		0x0790
#define MT6358_XO_EN32K_MAN		BIT(0)
#define MT6358_TOP_TMA_KEY		0x03a8
#define MT6358_PSEQ_ELR11		0x0a62
#define MT6358_SDN_DLY_ENB		BIT(10)
#define MT6358_PPCCTL0			0x0a08

static unsigned int mt6323_rtc_read(struct mt6323_pwrc *pwrc, u32 reg)
{
	unsigned int val = 0;

	regmap_read(pwrc->regmap, pwrc->base + reg, &val);
	return val;
}

static void mt6323_rtc_write(struct mt6323_pwrc *pwrc, u32 reg, unsigned int val)
{
	regmap_write(pwrc->regmap, pwrc->base + reg, val);
}

static int mt6358_rtc_trigger(struct mt6323_pwrc *pwrc)
{
	mt6323_rtc_write(pwrc, RTC_WRTGR_MT6358, 1);
	return mt6323_rtc_busy_wait(pwrc);
}

static void mt6358_rtc_reload(struct mt6323_pwrc *pwrc)
{
	mt6323_rtc_write(pwrc, RTC_BBPU, mt6323_rtc_read(pwrc, RTC_BBPU) |
			 RTC_BBPU_KEY | MT6358_RTC_BBPU_RELOAD);
	mt6358_rtc_trigger(pwrc);
}

static void mt6358_power_off(struct mt6323_pwrc *pwrc)
{
	struct regmap *map = pwrc->regmap;
	unsigned int val;

	/* Unlock RTC writes */
	mt6323_rtc_write(pwrc, MT6358_RTC_PROT, 0x586a);
	mt6358_rtc_trigger(pwrc);
	mt6323_rtc_write(pwrc, MT6358_RTC_PROT, 0x9136);
	mt6358_rtc_trigger(pwrc);

	/* No "2 second reboot" back on */
	mt6323_rtc_write(pwrc, RTC_AL_SEC, mt6323_rtc_read(pwrc, RTC_AL_SEC) &
			 ~(MT6358_RTC_AL_SEC_2SEC_EN | MT6358_RTC_AL_SEC_AUTO_PDN_SEL));
	mt6358_rtc_trigger(pwrc);

	/*
	 * The RTC runs off the 26 MHz crystal (32k-less mode); keep time on
	 * the calibrated internal oscillator while the crystal is off.
	 */
	regmap_update_bits(map, MT6358_TOP_CON, MT6358_SRCLKEN_IN_HW_MODE,
			   MT6358_SRCLKEN_IN_HW_MODE);
	regmap_update_bits(map, MT6358_SCK_TOP_CKPDN_CON0, MT6358_RTC_EOSC32_CK_PDN, 0);
	regmap_update_bits(map, MT6358_EOSC_CALI_CON0, MT6358_EOSC_CALI_TD,
			   FIELD_PREP(MT6358_EOSC_CALI_TD, 0x6));
	regmap_update_bits(map, MT6358_DCXO_CW02, MT6358_XO_EN32K_MAN, 0);
	mt6358_rtc_reload(pwrc);
	val = mt6323_rtc_read(pwrc, MT6358_RTC_AL_YEA);
	val = ((val | MT6358_RTC_K_EOSC_RSV_0) & ~MT6358_RTC_K_EOSC_RSV_1) |
	      MT6358_RTC_K_EOSC_RSV_2;
	mt6323_rtc_write(pwrc, MT6358_RTC_AL_YEA, val);
	mt6358_rtc_trigger(pwrc);
	mt6323_rtc_write(pwrc, MT6358_RTC_OSC32CON, 0x1a57);
	mt6323_rtc_busy_wait(pwrc);
	mt6323_rtc_write(pwrc, MT6358_RTC_OSC32CON, 0x2b68);
	mt6323_rtc_busy_wait(pwrc);
	mt6323_rtc_write(pwrc, MT6358_RTC_OSC32CON,
			 mt6323_rtc_read(pwrc, MT6358_RTC_OSC32CON) | MT6358_RTC_EMBCK_SRC_SEL);
	mt6323_rtc_busy_wait(pwrc);
	mt6358_rtc_reload(pwrc);

	/* Clear the alarm */
	mt6323_rtc_write(pwrc, RTC_BBPU, RTC_BBPU_KEY | MT6358_RTC_BBPU_CLR |
			 MT6358_RTC_BBPU_PWREN);
	mt6323_rtc_write(pwrc, RTC_AL_MASK, RTC_AL_MASK_DOW);
	mt6358_rtc_trigger(pwrc);
	mdelay(1);
	mt6358_rtc_reload(pwrc);

	/* Shutdown delay on, then let go of the power hold */
	regmap_write(map, MT6358_TOP_TMA_KEY, 0x9ca7);
	regmap_update_bits(map, MT6358_PSEQ_ELR11, MT6358_SDN_DLY_ENB, 0);
	regmap_write(map, MT6358_TOP_TMA_KEY, 0);
	regmap_write(map, MT6358_PPCCTL0, 0);
}

static int mt6323_do_pwroff(struct sys_off_data *data)
{
	struct mt6323_pwrc *pwrc = data->cb_data;
	const struct mt6323_pwrc_data *pdata = device_get_match_data(pwrc->dev);

	pdata->power_off(pwrc);

	/* Wait some time until system down, otherwise, notice with a warn */
	mdelay(1000);

	WARN_ONCE(1, "Unable to power off system\n");

	return NOTIFY_DONE;
}

static int mt6323_pwrc_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6397_chip = dev_get_drvdata(pdev->dev.parent);
	const struct mt6323_pwrc_data *pdata = device_get_match_data(&pdev->dev);
	struct mt6323_pwrc *pwrc;
	struct resource *res;
	int ret;

	pwrc = devm_kzalloc(&pdev->dev, sizeof(*pwrc), GFP_KERNEL);
	if (!pwrc)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	pwrc->base = res->start;
	pwrc->regmap = mt6397_chip->regmap;
	pwrc->dev = &pdev->dev;

	ret = devm_register_sys_off_handler(pwrc->dev,
					    SYS_OFF_MODE_POWER_OFF,
					    pdata->priority,
					    mt6323_do_pwroff,
					    pwrc);
	if (ret)
		return dev_err_probe(pwrc->dev, ret, "failed to register power-off handler\n");

	return 0;
}

static const struct mt6323_pwrc_data mt6323_pwrc_data = {
	.power_off = mt6323_power_off,
	.priority = SYS_OFF_PRIO_DEFAULT,
};

/* Ahead of PSCI SYSTEM_OFF, which the firmware doesn't act on */
static const struct mt6323_pwrc_data mt6358_pwrc_data = {
	.power_off = mt6358_power_off,
	.priority = SYS_OFF_PRIO_FIRMWARE + 1,
};

static const struct of_device_id mt6323_pwrc_dt_match[] = {
	{ .compatible = "mediatek,mt6323-pwrc", .data = &mt6323_pwrc_data },
	{ .compatible = "mediatek,mt6358-pwrc", .data = &mt6358_pwrc_data },
	{},
};
MODULE_DEVICE_TABLE(of, mt6323_pwrc_dt_match);

static struct platform_driver mt6323_pwrc_driver = {
	.probe          = mt6323_pwrc_probe,
	.driver         = {
		.name   = "mt6323-pwrc",
		.of_match_table = mt6323_pwrc_dt_match,
	},
};

module_platform_driver(mt6323_pwrc_driver);

MODULE_DESCRIPTION("Poweroff driver for MT6323 PMIC");
MODULE_AUTHOR("Sean Wang <sean.wang@mediatek.com>");
