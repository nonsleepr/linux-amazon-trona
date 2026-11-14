// SPDX-License-Identifier: GPL-2.0-only
/*
 * Starry 10.1" 1200x1920 in-cell DSI panel with a FocalTech FT8201AB
 * TDDI controller, as used in the Amazon Fire HD 10 (11th gen, "trona").
 *
 * The controller loads its own initialisation from OTP; the host only
 * powers it up in order, takes it out of sleep and unlocks the FocalTech
 * command set used by the touch side. Sequence and timings follow the
 * vendor driver (ft8201ab_wuxga_dsi_vdo).
 */

#include <linux/array_size.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct ft8201ab_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;

	struct regulator *vddi;
	struct regulator *avdd;
	struct regulator *avee;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *tp_reset_gpio;

	enum drm_panel_orientation orientation;
};

static inline struct ft8201ab_panel *to_ft8201ab(struct drm_panel *panel)
{
	return container_of(panel, struct ft8201ab_panel, panel);
}

static void ft8201ab_init_sequence(struct mipi_dsi_multi_context *ctx)
{
	mipi_dsi_dcs_write_seq_multi(ctx, MIPI_DCS_SET_TEAR_ON, 0x00);
	mipi_dsi_dcs_exit_sleep_mode_multi(ctx);
	mipi_dsi_msleep(ctx, 85);
	mipi_dsi_dcs_set_display_on_multi(ctx);
	mipi_dsi_dcs_write_seq_multi(ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x2c);

	/* Unlock CMD2 and the FocalTech command set, enable register reads */
	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x82, 0x01, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x80);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x82, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x88);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xb0, 0x07);
	/* Address the master IC only */
	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xfa, 0x02);
}

static void ft8201ab_power_off(struct ft8201ab_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->tp_reset_gpio, 1);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);

	regulator_disable(ctx->avee);
	regulator_disable(ctx->avdd);
	usleep_range(9000, 10000);

	regulator_disable(ctx->vddi);
}

static int ft8201ab_prepare(struct drm_panel *panel)
{
	struct ft8201ab_panel *ctx = to_ft8201ab(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	struct device *dev = panel->dev;
	int ret;

	ret = regulator_enable(ctx->vddi);
	if (ret) {
		dev_err(dev, "failed to enable vddi: %d\n", ret);
		return ret;
	}
	usleep_range(2000, 3000);

	/* The touch side comes out of reset before the bias rails */
	gpiod_set_value_cansleep(ctx->tp_reset_gpio, 0);
	usleep_range(3000, 4000);

	ret = regulator_enable(ctx->avdd);
	if (ret) {
		dev_err(dev, "failed to enable avdd: %d\n", ret);
		goto err_vddi;
	}
	usleep_range(5000, 6000);

	ret = regulator_enable(ctx->avee);
	if (ret) {
		dev_err(dev, "failed to enable avee: %d\n", ret);
		goto err_avdd;
	}
	usleep_range(9000, 10000);

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(80);

	ft8201ab_init_sequence(&dsi_ctx);
	if (dsi_ctx.accum_err) {
		ft8201ab_power_off(ctx);
		return dsi_ctx.accum_err;
	}

	return 0;

err_avdd:
	regulator_disable(ctx->avdd);
err_vddi:
	gpiod_set_value_cansleep(ctx->tp_reset_gpio, 1);
	regulator_disable(ctx->vddi);
	return ret;
}

static int ft8201ab_unprepare(struct drm_panel *panel)
{
	struct ft8201ab_panel *ctx = to_ft8201ab(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	/* Address master and slave, disable register reads */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfa, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x00, 0x88);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x01);

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 60);

	/* Deep standby */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x5a, 0xa5, 0x95, 0x27);
	mipi_dsi_msleep(&dsi_ctx, 17);

	ft8201ab_power_off(ctx);

	return 0;
}

static const struct drm_display_mode ft8201ab_mode = {
	/* 994 Mbps per lane like the vendor's 497 MHz DSI clock, ~63 Hz */
	.clock = 165670,
	.hdisplay = 1200,
	.hsync_start = 1200 + 8,
	.hsync_end = 1200 + 8 + 6,
	.htotal = 1200 + 8 + 6 + 8,
	.vdisplay = 1920,
	.vsync_start = 1920 + 205,
	.vsync_end = 1920 + 205 + 6,
	.vtotal = 1920 + 205 + 6 + 10,
	.width_mm = 135,
	.height_mm = 217,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int ft8201ab_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct ft8201ab_panel *ctx = to_ft8201ab(panel);

	drm_connector_set_panel_orientation(connector, ctx->orientation);

	return drm_connector_helper_get_modes_fixed(connector, &ft8201ab_mode);
}

static enum drm_panel_orientation ft8201ab_get_orientation(struct drm_panel *panel)
{
	return to_ft8201ab(panel)->orientation;
}

static const struct drm_panel_funcs ft8201ab_panel_funcs = {
	.prepare = ft8201ab_prepare,
	.unprepare = ft8201ab_unprepare,
	.get_modes = ft8201ab_get_modes,
	.get_orientation = ft8201ab_get_orientation,
};

static int ft8201ab_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ft8201ab_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct ft8201ab_panel, panel,
				   &ft8201ab_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->vddi = devm_regulator_get(dev, "vddi");
	if (IS_ERR(ctx->vddi))
		return dev_err_probe(dev, PTR_ERR(ctx->vddi), "failed to get vddi\n");
	ctx->avdd = devm_regulator_get(dev, "avdd");
	if (IS_ERR(ctx->avdd))
		return dev_err_probe(dev, PTR_ERR(ctx->avdd), "failed to get avdd\n");
	ctx->avee = devm_regulator_get(dev, "avee");
	if (IS_ERR(ctx->avee))
		return dev_err_probe(dev, PTR_ERR(ctx->avee), "failed to get avee\n");

	/* Both left deasserted: the bootloader hands over a running panel */
	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset gpio\n");
	ctx->tp_reset_gpio = devm_gpiod_get_optional(dev, "touch-reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->tp_reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->tp_reset_gpio),
				     "failed to get touch reset gpio\n");

	ret = drm_of_get_panel_orientation(dev->of_node, &ctx->orientation);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get orientation\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	/* Sync-event video mode (no sync pulses, no burst), continuous clock */
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach to DSI host\n");
	}

	return 0;
}

static void ft8201ab_remove(struct mipi_dsi_device *dsi)
{
	struct ft8201ab_panel *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ft8201ab_of_match[] = {
	{ .compatible = "starry,ft8201ab" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ft8201ab_of_match);

static struct mipi_dsi_driver ft8201ab_driver = {
	.driver = {
		.name = "panel-starry-ft8201ab",
		.of_match_table = ft8201ab_of_match,
	},
	.probe = ft8201ab_probe,
	.remove = ft8201ab_remove,
};
module_mipi_dsi_driver(ft8201ab_driver);

MODULE_DESCRIPTION("Starry FT8201AB 1200x1920 DSI panel driver");
MODULE_LICENSE("GPL");
