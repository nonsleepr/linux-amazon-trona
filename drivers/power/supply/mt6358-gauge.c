// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6358 PMIC fuel gauge
 *
 * The MT6358 FGADC measures the battery current across a sense resistor and
 * integrates it in a coulomb counter (CAR) that keeps running in the PMIC's
 * always-on domain, also while the SoC is suspended. The battery voltage
 * comes from the PMIC AUXADC.
 *
 * MediaTek's own gauge (GM3.0) runs its algorithm in a userspace daemon.
 * This driver does a simple version of it:
 *
 *  - The open circuit voltage is estimated as voltage-now minus the IR drop
 *    over the battery's internal resistance, averaged over a few samples,
 *    and looked up in the battery's OCV table.
 *  - For the first INIT_POLLS samples after probe/resume the charge follows
 *    that estimate; afterwards the coulomb counter tracks it.
 *  - Whenever the current stays low for a while, the OCV estimate is
 *    accurate, and the coulomb-counted charge is pulled towards it to undo
 *    drift (and a wrong starting point taken under load).
 *  - The battery temperature, if the DT has a "temp" IIO channel for it, is
 *    only reported; the OCV table is the one for AMBIENT_CELSIUS.
 *
 * Copyright (c) 2026 Toshit Chawda <r58playz@gmail.com>
 */

#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/devm-helpers.h>
#include <linux/iio/consumer.h>
#include <linux/math64.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define MT6358_FGADC_CON0		0x0d08
#define MT6358_FG_ON			BIT(0)
#define MT6358_FGADC_CON1		0x0d0a
#define MT6358_FG_SW_CMD		GENMASK(3, 0)
#define MT6358_FG_SW_READ_PRE		BIT(0)
#define MT6358_FG_SW_CLEAR		BIT(3)
#define MT6358_FG_LATCHDATA_ST		BIT(15)
#define MT6358_FGADC_CAR_CON0		0x0d14
#define MT6358_FGADC_CAR_CON1		0x0d16
#define MT6358_FGADC_CUR_CON0		0x0d8a

/*
 * Scaling from MediaTek's gauge HAL, for its reference 10 mOhm sense
 * resistor: 381.47 uA per current LSB, and 0.108507 mAh per 2^11 coulomb
 * counter LSBs (the counter integrates the current every 0.5 ms).
 */
#define MT6358_FG_REF_RSENSE_UOHM	10000
#define MT6358_FG_CUR_NA_PER_LSB	381470
#define MT6358_FG_CAR_NAH_PER_2K_LSB	108507

/* BATADC: 15 bits over 3 x 1.8 V */
#define MT6358_BATADC_FULL_SCALE_UV	(3 * 1800000)
#define MT6358_BATADC_BITS		15

#define POLL_INTERVAL			(30 * HZ)
#define INIT_POLL_INTERVAL		(5 * HZ)
#define INIT_POLLS			6
#define OCV_AVG_SAMPLES			4

/* Below this the IR compensation error is small and the OCV trustworthy */
#define RELAXED_CURRENT_UA		50000
#define RELAXED_POLLS			3
/* Hysteresis for charging/discharging status */
#define STATUS_CURRENT_UA		50000
#define VBUS_PRESENT_MV			4400
#define FULL_CAPACITY			95
#define AMBIENT_CELSIUS			25

struct mt6358_gauge {
	struct device *dev;
	struct regmap *regmap;
	struct iio_channel *vbat;
	struct iio_channel *vbus;
	struct iio_channel *temp;
	struct power_supply_desc desc;
	struct power_supply *psy;
	struct power_supply_battery_info *info;
	struct delayed_work work;
	struct mutex lock;

	u32 rsense_uohm;
	int res_mohm;
	int full_uah;

	/* charge_ref_uah was the charge when the CAR read car_ref */
	s32 car_ref;
	s64 charge_ref_uah;

	int ocv_samples[OCV_AVG_SAMPLES];
	unsigned int nr_samples;
	int ocv_uv;
	unsigned int polls;
	unsigned int relaxed_polls;

	int status;
	int capacity;
};

static int mt6358_gauge_read_fg(struct mt6358_gauge *gg, int *curr_ua, s32 *car)
{
	unsigned int val, cur, lo, hi;
	int ret, ret2;

	/* Latch the current and coulomb counter, read them, release the latch */
	ret = regmap_update_bits(gg->regmap, MT6358_FGADC_CON1, MT6358_FG_SW_CMD,
				 MT6358_FG_SW_READ_PRE);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(gg->regmap, MT6358_FGADC_CON1, val,
				       val & MT6358_FG_LATCHDATA_ST, 20, 10000);
	if (!ret)
		ret = regmap_read(gg->regmap, MT6358_FGADC_CUR_CON0, &cur);
	if (!ret)
		ret = regmap_read(gg->regmap, MT6358_FGADC_CAR_CON0, &lo);
	if (!ret)
		ret = regmap_read(gg->regmap, MT6358_FGADC_CAR_CON1, &hi);

	ret2 = regmap_update_bits(gg->regmap, MT6358_FGADC_CON1, MT6358_FG_SW_CMD,
				  MT6358_FG_SW_CLEAR);
	if (!ret2)
		ret2 = regmap_read_poll_timeout(gg->regmap, MT6358_FGADC_CON1, val,
						!(val & MT6358_FG_LATCHDATA_ST), 20, 10000);
	regmap_update_bits(gg->regmap, MT6358_FGADC_CON1, MT6358_FG_SW_CMD, 0);

	if (ret || ret2) {
		dev_err(gg->dev, "failed to latch FGADC: %d/%d\n", ret, ret2);
		return ret ?: ret2;
	}

	/* Positive while charging */
	if (curr_ua)
		*curr_ua = div_s64((s64)(s16)cur * MT6358_FG_CUR_NA_PER_LSB *
				   MT6358_FG_REF_RSENSE_UOHM,
				   1000LL * gg->rsense_uohm);
	if (car)
		*car = (s32)(hi << 16 | lo);

	return 0;
}

static s64 mt6358_gauge_car_to_uah(struct mt6358_gauge *gg, s32 car)
{
	return div_s64((s64)car * MT6358_FG_CAR_NAH_PER_2K_LSB *
		       MT6358_FG_REF_RSENSE_UOHM,
		       2048LL * 1000 * gg->rsense_uohm);
}

static int mt6358_gauge_read_vbat(struct mt6358_gauge *gg, int *volt_uv)
{
	int ret, raw;

	/*
	 * Convert the raw code here: the AUXADC's IIO scale leaves out the
	 * division by the converter's resolution.
	 */
	ret = iio_read_channel_raw(gg->vbat, &raw);
	if (ret < 0)
		return ret;

	*volt_uv = ((s64)raw * MT6358_BATADC_FULL_SCALE_UV) >> MT6358_BATADC_BITS;
	return 0;
}

/*
 * The temperature sensor is optional and may probe late or never: look it up
 * on first use instead of holding up the gauge itself.
 */
static int mt6358_gauge_read_temp(struct mt6358_gauge *gg, int *temp_mc)
{
	if (!gg->temp) {
		struct iio_channel *chan = devm_iio_channel_get(gg->dev, "temp");

		if (IS_ERR(chan))
			return PTR_ERR(chan) == -EPROBE_DEFER ? -ENODATA : PTR_ERR(chan);
		gg->temp = chan;
	}

	return iio_read_channel_processed(gg->temp, temp_mc);
}

static s64 mt6358_gauge_charge_now(struct mt6358_gauge *gg, s32 car)
{
	s64 charge = gg->charge_ref_uah +
		     mt6358_gauge_car_to_uah(gg, car - gg->car_ref);

	return clamp_val(charge, 0, gg->full_uah);
}

static int mt6358_gauge_pct_to_uah(struct mt6358_gauge *gg, int pct)
{
	return div_s64((s64)gg->full_uah * pct, 100);
}

/* Charge corresponding to an open circuit voltage, from the OCV table */
static int mt6358_gauge_ocv_to_uah(struct mt6358_gauge *gg, int ocv_uv)
{
	const struct power_supply_battery_ocv_table *t;
	int len, i, lo, hi;

	t = power_supply_find_ocv2cap_table(gg->info, AMBIENT_CELSIUS, &len);
	if (!t || !len)
		return 0;

	/* The table is sorted by descending voltage */
	for (i = 0; i < len; i++)
		if (ocv_uv > t[i].ocv)
			break;
	if (i == 0)
		return mt6358_gauge_pct_to_uah(gg, t[0].capacity);
	if (i == len || t[i - 1].ocv == t[i].ocv)
		return mt6358_gauge_pct_to_uah(gg, t[i - 1].capacity);

	lo = mt6358_gauge_pct_to_uah(gg, t[i].capacity);
	hi = mt6358_gauge_pct_to_uah(gg, t[i - 1].capacity);
	return lo + div_s64((s64)(hi - lo) * (ocv_uv - t[i].ocv),
			    t[i - 1].ocv - t[i].ocv);
}

static bool mt6358_gauge_supplied(struct mt6358_gauge *gg)
{
	int mv;

	if (gg->vbus)
		return !iio_read_channel_processed(gg->vbus, &mv) &&
		       mv > VBUS_PRESENT_MV;

	return power_supply_am_i_supplied(gg->psy) > 0;
}

static int mt6358_gauge_get_status(struct mt6358_gauge *gg, int curr_ua,
				   int capacity)
{
	if (curr_ua > STATUS_CURRENT_UA)
		return POWER_SUPPLY_STATUS_CHARGING;
	if (curr_ua < -STATUS_CURRENT_UA)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	if (!mt6358_gauge_supplied(gg))
		return POWER_SUPPLY_STATUS_DISCHARGING;
	/*
	 * Plugged in and the charger isn't charging. That is only "full" if
	 * the charger's termination voltage is near the battery's maximum.
	 */
	return capacity >= FULL_CAPACITY ? POWER_SUPPLY_STATUS_FULL :
					   POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static void mt6358_gauge_work(struct work_struct *work)
{
	struct mt6358_gauge *gg = container_of(work, struct mt6358_gauge, work.work);
	int volt_uv, curr_ua, ocv_uah, capacity, status, i, n;
	s64 charge, delta;
	bool changed;
	s32 car;

	scoped_guard(mutex, &gg->lock) {
		if (mt6358_gauge_read_vbat(gg, &volt_uv) ||
		    mt6358_gauge_read_fg(gg, &curr_ua, &car))
			goto out;

		gg->ocv_samples[gg->nr_samples++ % OCV_AVG_SAMPLES] =
			volt_uv - curr_ua / 1000 * gg->res_mohm;
		n = min(gg->nr_samples, OCV_AVG_SAMPLES);
		for (gg->ocv_uv = 0, i = 0; i < n; i++)
			gg->ocv_uv += gg->ocv_samples[i];
		gg->ocv_uv /= n;
		ocv_uah = mt6358_gauge_ocv_to_uah(gg, gg->ocv_uv);

		if (abs(curr_ua) < RELAXED_CURRENT_UA)
			gg->relaxed_polls++;
		else
			gg->relaxed_polls = 0;

		if (gg->polls < INIT_POLLS) {
			/* Starting point: follow the OCV estimate */
			gg->charge_ref_uah = ocv_uah;
			gg->car_ref = car;
		} else if (gg->relaxed_polls >= RELAXED_POLLS) {
			/* Move a quarter of the way to the OCV estimate per poll */
			charge = mt6358_gauge_charge_now(gg, car);
			delta = div_s64(ocv_uah - charge, 4);
			gg->charge_ref_uah = charge + delta;
			gg->car_ref = car;
		}
		gg->polls++;

		charge = mt6358_gauge_charge_now(gg, car);
		capacity = DIV_ROUND_CLOSEST_ULL(charge * 100, gg->full_uah);
		status = mt6358_gauge_get_status(gg, curr_ua, capacity);

		dev_dbg(gg->dev, "V %d uV, I %d uA, OCV %d uV (%d uAh), charge %lld uAh, %d%%\n",
			volt_uv, curr_ua, gg->ocv_uv, ocv_uah, charge, capacity);

		changed = capacity != gg->capacity || status != gg->status;
		gg->capacity = capacity;
		gg->status = status;
	}

	if (changed)
		power_supply_changed(gg->psy);
out:
	schedule_delayed_work(&gg->work, gg->polls < INIT_POLLS ?
			      INIT_POLL_INTERVAL : POLL_INTERVAL);
}

static const enum power_supply_property mt6358_gauge_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CAPACITY,
	/* Keep last: only advertised if the DT has a "temp" channel */
	POWER_SUPPLY_PROP_TEMP,
};

static int mt6358_gauge_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct mt6358_gauge *gg = power_supply_get_drvdata(psy);
	int ret, temp_mc;
	s32 car;

	guard(mutex)(&gg->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = gg->status;
		return 0;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = gg->info && gg->info->technology != POWER_SUPPLY_TECHNOLOGY_UNKNOWN ?
			      gg->info->technology : POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return mt6358_gauge_read_vbat(gg, &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		val->intval = gg->ocv_uv;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = gg->info->voltage_min_design_uv;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = gg->info->voltage_max_design_uv;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return mt6358_gauge_read_fg(gg, &val->intval, NULL);
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = gg->full_uah;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = mt6358_gauge_read_fg(gg, NULL, &car);
		if (ret)
			return ret;
		val->intval = mt6358_gauge_charge_now(gg, car);
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		/*
		 * The coulomb counter as it is, since the FGADC was started:
		 * no OCV correction, so differences of it are the coulombs
		 * that went through the sense resistor (positive in), also
		 * over a suspend.
		 */
		ret = mt6358_gauge_read_fg(gg, NULL, &car);
		if (ret)
			return ret;
		val->intval = mt6358_gauge_car_to_uah(gg, car);
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = gg->capacity;
		return 0;
	case POWER_SUPPLY_PROP_TEMP:
		ret = mt6358_gauge_read_temp(gg, &temp_mc);
		if (ret)
			return ret;
		/* In tenths of a degree */
		val->intval = DIV_ROUND_CLOSEST(temp_mc, 100);
		return 0;
	default:
		return -EINVAL;
	}
}

static void mt6358_gauge_external_power_changed(struct power_supply *psy)
{
	struct mt6358_gauge *gg = power_supply_get_drvdata(psy);

	mod_delayed_work(system_percpu_wq, &gg->work, HZ);
}

static const struct power_supply_desc mt6358_gauge_desc = {
	.name = "mt6358-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = mt6358_gauge_props,
	.num_properties = ARRAY_SIZE(mt6358_gauge_props),
	.get_property = mt6358_gauge_get_property,
	.external_power_changed = mt6358_gauge_external_power_changed,
	/* The DT's own thermal zone for the sensor is the one to use */
	.no_thermal = true,
};

static int mt6358_gauge_probe(struct platform_device *pdev)
{
	struct mt6397_chip *chip = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config cfg = {};
	struct device *dev = &pdev->dev;
	struct mt6358_gauge *gg;
	unsigned int con0;
	int ret;

	gg = devm_kzalloc(dev, sizeof(*gg), GFP_KERNEL);
	if (!gg)
		return -ENOMEM;

	gg->dev = dev;
	gg->regmap = chip->regmap;
	gg->status = POWER_SUPPLY_STATUS_UNKNOWN;
	gg->capacity = -1;
	platform_set_drvdata(pdev, gg);

	ret = regmap_read(gg->regmap, MT6358_FGADC_CON0, &con0);
	if (ret)
		return ret;
	if (!(con0 & MT6358_FG_ON))
		return dev_err_probe(dev, -ENODEV,
				     "FGADC is off (the bootloader normally starts it)\n");

	gg->rsense_uohm = MT6358_FG_REF_RSENSE_UOHM;
	device_property_read_u32(dev, "mediatek,sense-resistor-micro-ohms",
				 &gg->rsense_uohm);
	if (!gg->rsense_uohm)
		return -EINVAL;

	gg->vbat = devm_iio_channel_get(dev, "vbat");
	if (IS_ERR(gg->vbat))
		return dev_err_probe(dev, PTR_ERR(gg->vbat), "no vbat channel\n");

	gg->vbus = devm_iio_channel_get(dev, "vbus");
	if (IS_ERR(gg->vbus)) {
		if (PTR_ERR(gg->vbus) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(gg->vbus), "bad vbus channel\n");
		gg->vbus = NULL;
	}

	ret = devm_mutex_init(dev, &gg->lock);
	if (ret)
		return ret;

	ret = devm_delayed_work_autocancel(dev, &gg->work, mt6358_gauge_work);
	if (ret)
		return ret;

	gg->desc = mt6358_gauge_desc;
	if (device_property_match_string(dev, "io-channel-names", "temp") < 0)
		gg->desc.num_properties--;

	cfg.drv_data = gg;
	cfg.fwnode = dev_fwnode(dev);
	gg->psy = devm_power_supply_register(dev, &gg->desc, &cfg);
	if (IS_ERR(gg->psy))
		return dev_err_probe(dev, PTR_ERR(gg->psy), "failed to register battery\n");

	gg->info = gg->psy->battery_info;
	if (!gg->info || gg->info->charge_full_design_uah <= 0 ||
	    gg->info->factory_internal_resistance_uohm <= 0 ||
	    !gg->info->ocv_table[0])
		return dev_err_probe(dev, -EINVAL,
				     "monitored-battery needs charge-full-design, internal resistance and an OCV table\n");

	gg->full_uah = gg->info->charge_full_design_uah;
	gg->res_mohm = gg->info->factory_internal_resistance_uohm / 1000;

	schedule_delayed_work(&gg->work, 0);
	flush_delayed_work(&gg->work);

	return 0;
}

static int mt6358_gauge_suspend(struct device *dev)
{
	struct mt6358_gauge *gg = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&gg->work);
	return 0;
}

static int mt6358_gauge_resume(struct device *dev)
{
	struct mt6358_gauge *gg = dev_get_drvdata(dev);

	/* The coulomb counter ran through suspend; just resample */
	scoped_guard(mutex, &gg->lock)
		gg->relaxed_polls = 0;
	schedule_delayed_work(&gg->work, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mt6358_gauge_pm_ops, mt6358_gauge_suspend,
				mt6358_gauge_resume);

static const struct of_device_id mt6358_gauge_of_match[] = {
	{ .compatible = "mediatek,mt6358-gauge" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6358_gauge_of_match);

static struct platform_driver mt6358_gauge_driver = {
	.driver = {
		.name = "mt6358-gauge",
		.of_match_table = mt6358_gauge_of_match,
		.pm = pm_sleep_ptr(&mt6358_gauge_pm_ops),
	},
	.probe = mt6358_gauge_probe,
};
module_platform_driver(mt6358_gauge_driver);

MODULE_DESCRIPTION("MediaTek MT6358 PMIC fuel gauge");
MODULE_AUTHOR("Toshit Chawda <r58playz@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_CONSUMER");
