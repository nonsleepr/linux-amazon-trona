// SPDX-License-Identifier: GPL-2.0
//
// mt8183-mt6358-trona.c
//	--  ASoC machine driver for the Amazon Fire HD 10 (2021, "trona")
//
// Copyright (c) 2026 The Trona Linux porting effort
//
// Unlike the other MT8183 machine drivers in this directory, trona has no
// external I2C/I2S smart amplifier and no I2C jack-detect chip: its speaker
// is driven straight off MT6358's own analog headphone/line-out stage
// ("Headphone L/R Ext Spk Amp" in the codec's DAPM graph) into a discrete
// Class-D amplifier that is controlled purely by two GPIOs (enable and a
// static gain-mode select), not a digital bus. The microphone is likewise
// analog, captured through MT6358's AIN0/AIN2 inputs.
//
// Wiring (compatible string, GPIO numbers/polarity, DAPM routes and the
// amplifier enable timing) is transcribed from a third-party mainline port
// of this exact tablet, Cesarus85/omarchy-amazon-fire-hd10
// (drivers/audio/trona_audio.c), whose own header attributes the wiring to
// the shipped Amazon Trona device tree and mtk-auddrv-gpio.c, and whose
// hardware table reports both speaker test tones audible on physical
// hardware. That port's notes also record that the "LoudSPK Playback" UCM
// path lost a channel in testing while "Audio Playback" kept both channels
// independent -- preserved here as a comment for whoever next touches UCM.

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/soc.h>

struct mt8183_mt6358_trona_priv {
	struct gpio_desc *spk_amp_gpio;
	struct gpio_desc *spk_gain_gpio;
};

static int mt8183_mt6358_trona_speaker_event(struct snd_soc_dapm_widget *w,
					     struct snd_kcontrol *kcontrol,
					     int event)
{
	/*
	 * struct snd_soc_dapm_context is opaque outside soc-dapm.c in this
	 * tree, so get back to the card through the accessor instead of
	 * dereferencing w->dapm->card directly.
	 */
	struct snd_soc_card *card = snd_soc_dapm_to_card(w->dapm);
	struct mt8183_mt6358_trona_priv *priv =
		snd_soc_card_get_drvdata(card);

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		/*
		 * Stock trona only ever uses gain mode 1, selected by
		 * holding this GPIO at a fixed level -- not a pulse-coded
		 * gain ramp -- so set it before enabling the amplifier.
		 */
		gpiod_set_value_cansleep(priv->spk_gain_gpio, 1);
		gpiod_set_value_cansleep(priv->spk_amp_gpio, 1);
		/* Amplifier power-up settle time, from the vendor driver. */
		msleep(50);
	} else {
		gpiod_set_value_cansleep(priv->spk_amp_gpio, 0);
	}

	return 0;
}

static const struct snd_soc_dapm_widget mt8183_mt6358_trona_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", mt8183_mt6358_trona_speaker_event),
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Internal Mic", NULL),
};

static const struct snd_kcontrol_new mt8183_mt6358_trona_controls[] = {
	SOC_DAPM_PIN_SWITCH("Speaker"),
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Internal Mic"),
};

/*
 * The speaker amplifier sits downstream of MT6358's own headphone DAC/amp
 * pins, not on a separate DAI, so it only needs these two DAPM routes (plus
 * the GPIO toggling in the widget event above) rather than a codec or
 * component of its own.
 */
static const struct snd_soc_dapm_route mt8183_mt6358_trona_routes[] = {
	{"Speaker", NULL, "Headphone L Ext Spk Amp"},
	{"Speaker", NULL, "Headphone R Ext Spk Amp"},
	{"Headphone", NULL, "Headphone L"},
	{"Headphone", NULL, "Headphone R"},
	{"AIN0", NULL, "Internal Mic"},
	{"AIN2", NULL, "Internal Mic"},
};

SND_SOC_DAILINK_DEFS(playback,
	DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture,
	DAILINK_COMP_ARRAY(COMP_CPU("UL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(primary_codec,
	DAILINK_COMP_ARRAY(COMP_CPU("ADDA")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt6358-sound", "mt6358-snd-codec-aif1")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link mt8183_mt6358_trona_dai_links[] = {
	/* FE */
	{
		.name = "Playback",
		.stream_name = "Playback",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(playback),
	},
	{
		.name = "Capture",
		.stream_name = "Capture",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(capture),
	},
	/* BE */
	{
		.name = "Primary Codec",
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(primary_codec),
	},
};

static int mt8183_mt6358_trona_late_probe(struct snd_soc_card *card)
{
	/*
	 * card->dapm is an allocated struct snd_soc_dapm_context *, not an
	 * embedded struct, in this tree -- use the accessor instead of
	 * assuming its storage.
	 */
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(card);

	/* UCM enables the speaker/headphone/mic pins it actually needs. */
	snd_soc_dapm_disable_pin(dapm, "Speaker");
	snd_soc_dapm_disable_pin(dapm, "Headphone");
	snd_soc_dapm_disable_pin(dapm, "Internal Mic");

	return snd_soc_dapm_sync(dapm);
}

static struct snd_soc_card mt8183_mt6358_trona_card = {
	.name = "mt8183_mt6358_trona",
	.owner = THIS_MODULE,
	.dai_link = mt8183_mt6358_trona_dai_links,
	.num_links = ARRAY_SIZE(mt8183_mt6358_trona_dai_links),
	.controls = mt8183_mt6358_trona_controls,
	.num_controls = ARRAY_SIZE(mt8183_mt6358_trona_controls),
	.dapm_widgets = mt8183_mt6358_trona_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mt8183_mt6358_trona_widgets),
	.dapm_routes = mt8183_mt6358_trona_routes,
	.num_dapm_routes = ARRAY_SIZE(mt8183_mt6358_trona_routes),
	.late_probe = mt8183_mt6358_trona_late_probe,
};

static int mt8183_mt6358_trona_dev_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &mt8183_mt6358_trona_card;
	struct device *dev = &pdev->dev;
	struct device_node *platform_node;
	struct snd_soc_dai_link *dai_link;
	struct mt8183_mt6358_trona_priv *priv;
	int i, ret;

	platform_node = of_parse_phandle(dev->of_node, "mediatek,platform", 0);
	if (!platform_node)
		return dev_err_probe(dev, -EINVAL,
				      "Property 'mediatek,platform' missing or invalid\n");

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto put_platform_node;
	}

	priv->spk_amp_gpio = devm_gpiod_get(dev, "speaker-enable",
					    GPIOD_OUT_LOW);
	if (IS_ERR(priv->spk_amp_gpio)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->spk_amp_gpio),
				    "Failed to get speaker-enable GPIO\n");
		goto put_platform_node;
	}

	priv->spk_gain_gpio = devm_gpiod_get(dev, "speaker-gain",
					     GPIOD_OUT_LOW);
	if (IS_ERR(priv->spk_gain_gpio)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->spk_gain_gpio),
				    "Failed to get speaker-gain GPIO\n");
		goto put_platform_node;
	}

	for_each_card_prelinks(card, i, dai_link)
		dai_link->platforms->of_node = platform_node;

	card->dev = dev;
	snd_soc_card_set_drvdata(card, priv);

	ret = devm_snd_soc_register_card(dev, card);
	if (ret)
		dev_err_probe(dev, ret, "Failed to register card %s\n",
			      card->name);

put_platform_node:
	of_node_put(platform_node);
	return ret;
}

#ifdef CONFIG_OF
static const struct of_device_id mt8183_mt6358_trona_dt_match[] = {
	{ .compatible = "amazon,trona-audio" },
	{}
};
MODULE_DEVICE_TABLE(of, mt8183_mt6358_trona_dt_match);
#endif

static struct platform_driver mt8183_mt6358_trona_driver = {
	.driver = {
		.name = "mt8183_mt6358_trona",
#ifdef CONFIG_OF
		.of_match_table = mt8183_mt6358_trona_dt_match,
#endif
		.pm = &snd_soc_pm_ops,
	},
	.probe = mt8183_mt6358_trona_dev_probe,
};

module_platform_driver(mt8183_mt6358_trona_driver);

/* Module information */
MODULE_DESCRIPTION("MT8183-MT6358 ALSA SoC machine driver for Amazon Trona");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("mt8183_mt6358_trona soc card");
