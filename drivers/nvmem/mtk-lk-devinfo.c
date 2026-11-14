// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek preloader efuse copy ("devinfo"), as passed on by LK
 *
 * MediaTek's preloader copies a selection of efuse words into a table that
 * LK hands to the kernel as the /chosen "atag,devinfo" property. On devices
 * whose efuse block reads back as zeros from the normal world (Amazon's
 * MT8183 tablets, at least), this is the only place to get the calibration
 * data from.
 *
 * The property holds raw little-endian words as LK copied them from its
 * ATAG: a word count, then that many words. Word n is exposed at offset
 * 4 * n. The selection and order are the preloader's, so cells have to be
 * described by devinfo index, not by efuse address.
 *
 * Copyright (c) 2026 Toshit Chawda <r58playz@gmail.com>
 */

#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>

struct lk_devinfo {
	const __le32 *words;
};

static int lk_devinfo_read(void *priv, unsigned int offset, void *val,
			   size_t bytes)
{
	struct lk_devinfo *di = priv;
	u32 *out = val;

	/* word_size and stride are 4, so offset and bytes are aligned */
	for (; bytes; bytes -= 4, offset += 4)
		*out++ = le32_to_cpu(di->words[offset / 4]);

	return 0;
}

static int lk_devinfo_probe(struct platform_device *pdev)
{
	struct nvmem_config config = {
		.dev = &pdev->dev,
		.name = "lk-devinfo",
		.id = NVMEM_DEVID_NONE,
		.read_only = true,
		.root_only = true,
		.word_size = 4,
		.stride = 4,
		.reg_read = lk_devinfo_read,
		.add_legacy_fixed_of_cells = true,
	};
	struct device_node *chosen;
	struct lk_devinfo *di;
	const __le32 *prop;
	u32 count;
	int len;

	di = devm_kzalloc(&pdev->dev, sizeof(*di), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	chosen = of_find_node_by_path("/chosen");
	prop = of_get_property(chosen, "atag,devinfo", &len);
	of_node_put(chosen);
	if (!prop || len < sizeof(*prop))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "no /chosen/atag,devinfo from the bootloader\n");

	count = le32_to_cpu(prop[0]);
	if (!count || len < sizeof(*prop) * (count + 1))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "atag,devinfo: %u words in %d bytes\n", count, len);

	di->words = prop + 1;
	config.priv = di;
	config.size = count * sizeof(*prop);

	return PTR_ERR_OR_ZERO(devm_nvmem_register(&pdev->dev, &config));
}

static const struct of_device_id lk_devinfo_of_match[] = {
	{ .compatible = "mediatek,lk-devinfo" },
	{ }
};
MODULE_DEVICE_TABLE(of, lk_devinfo_of_match);

static struct platform_driver lk_devinfo_driver = {
	.probe = lk_devinfo_probe,
	.driver = {
		.name = "mtk-lk-devinfo",
		.of_match_table = lk_devinfo_of_match,
	},
};
module_platform_driver(lk_devinfo_driver);

MODULE_DESCRIPTION("MediaTek preloader efuse copy (LK atag,devinfo)");
MODULE_AUTHOR("Toshit Chawda <r58playz@gmail.com>");
MODULE_LICENSE("GPL");
