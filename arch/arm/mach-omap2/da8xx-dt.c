/*
 * DA8XX-DT: Device tree adapter using the legacy driver
 *
 * Copyright (C) 2012 Pantelis Antoniou <panto@antoniou-consulting.com>
 * Copyright (C) 2012 Texas Instruments Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <video/da8xx-fb.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/consumer.h>
#include <linux/clk.h>
#include <plat/clock.h>
#include <plat/omap_device.h>

struct da8xx_priv {
	struct da8xx_lcdc_platform_data lcd_pdata;
	struct lcd_ctrl_config lcd_cfg;
	struct display_panel lcd_panel;
	struct platform_device *lcdc_pdev;
	struct omap_hwmod *lcdc_oh;
	struct resource lcdc_res[1];
	int power_dn_gpio;
};

static const struct of_device_id of_da8xx_dt_match[] = {
	{ .compatible = "da8xx-dt", },
	{},
};

static int __devinit da8xx_dt_probe(struct platform_device *pdev)
{
	struct da8xx_priv *priv;
	struct clk *disp_pll;
	struct pinctrl *pinctrl;
	u32 disp_pll_val;
	const char *panel_type;
	int ret = -EINVAL;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (priv == NULL) {
		dev_err(&pdev->dev, "Failed to allocate priv\n");
		return -ENOMEM;
	}
	priv->power_dn_gpio = -1;

	pinctrl = devm_pinctrl_get_select_default(&pdev->dev);
	if (IS_ERR(pinctrl))
		dev_warn(&pdev->dev,
			"pins are not configured from the driver\n");

	ret = of_property_read_u32(pdev->dev.of_node, "disp-pll",
			&disp_pll_val);
	if (ret != 0) {
		dev_err(&pdev->dev, "Failed to read disp-pll property\n");
		return ret;
	}

	ret = of_property_read_string(pdev->dev.of_node, "panel-type",
			&panel_type);
	if (ret != 0) {
		dev_err(&pdev->dev, "Failed to read panel-type property\n");
		return ret;
	}

	/* conf_disp_pll(disp_pll); */
	disp_pll = clk_get(NULL, "dpll_disp_ck");
	if (IS_ERR(disp_pll)) {
		dev_err(&pdev->dev, "Cannot clk_get disp_pll\n");
		return PTR_ERR(disp_pll);
	}
	ret = clk_set_rate(disp_pll, disp_pll_val);
	clk_put(disp_pll);
	if (ret != 0) {
		dev_err(&pdev->dev, "Failed to set disp_pll\n");
		return ret;
	}

	ret = of_get_named_gpio_flags(pdev->dev.of_node, "powerdn-gpio",
			0, NULL);
	if (IS_ERR_VALUE(ret)) {
		dev_info(&pdev->dev, "No power down GPIO\n");
	} else {
		priv->power_dn_gpio = ret;

		ret = devm_gpio_request(&pdev->dev, priv->power_dn_gpio,
				"da8xx-dt:PDN");
		if (ret != 0) {
			dev_err(&pdev->dev, "Failed to gpio_request\n");
			return ret;
		}

		ret = gpio_direction_output(priv->power_dn_gpio, 1);
		if (ret != 0) {
			dev_err(&pdev->dev, "Failed to set powerdn to 1\n");
			return ret;
		}
	}

	/* display_panel */
	priv->lcd_panel.panel_type	= QVGA;
	priv->lcd_panel.max_bpp		= 16;
	priv->lcd_panel.min_bpp		= 16;
	priv->lcd_panel.panel_shade	= COLOR_ACTIVE;

	/* lcd_ctrl_config */
	priv->lcd_cfg.p_disp_panel	= &priv->lcd_panel;
	priv->lcd_cfg.ac_bias		= 255;
	priv->lcd_cfg.ac_bias_intrpt	= 0;
	priv->lcd_cfg.dma_burst_sz	= 16;
	priv->lcd_cfg.bpp		= 16;
	priv->lcd_cfg.fdd		= 0x80;
	priv->lcd_cfg.tft_alt_mode	= 0;
	priv->lcd_cfg.stn_565_mode	= 0;
	priv->lcd_cfg.mono_8bit_mode	= 0;
	priv->lcd_cfg.invert_line_clock	= 1;
	priv->lcd_cfg.invert_frm_clock	= 1;
	priv->lcd_cfg.sync_edge		= 0;
	priv->lcd_cfg.sync_ctrl		= 1;
	priv->lcd_cfg.raster_order	= 0;

	/* da8xx_lcdc_platform_data */
	strcpy(priv->lcd_pdata.manu_name, "BBToys");
	priv->lcd_pdata.controller_data = &priv->lcd_cfg;
	strcpy(priv->lcd_pdata.type, panel_type);

	priv->lcdc_oh = omap_hwmod_lookup("lcdc");
	if (priv->lcdc_oh == NULL) {
		dev_err(&pdev->dev, "Failed to lookup omap_hwmod lcdc\n");
		return -ENODEV;
	}

	priv->lcdc_pdev = omap_device_build("da8xx_lcdc", 0, priv->lcdc_oh,
			&priv->lcd_pdata,
			sizeof(struct da8xx_lcdc_platform_data),
			NULL, 0, 0);
	if (priv->lcdc_pdev == NULL) {
		dev_err(&pdev->dev, "Failed to build LCDC device\n");
		return -ENODEV;
	}

	dev_info(&pdev->dev, "Registered bone LCDC OK.\n");

	platform_set_drvdata(pdev, priv);

	return 0;
}

static int __devexit da8xx_dt_remove(struct platform_device *pdev)
{
	return -EINVAL;	/* not supporting removal yet */
}

static struct platform_driver da8xx_dt_driver = {
	.probe		= da8xx_dt_probe,
	.remove		= __devexit_p(da8xx_dt_remove),
	.driver		= {
		.name	= "da8xx-dt",
		.owner	= THIS_MODULE,
		.of_match_table = of_da8xx_dt_match,
	},
};

static __init int da8xx_dt_init(void)
{
	return platform_driver_register(&da8xx_dt_driver);
}

static __exit void da8xx_dt_exit(void)
{
	platform_driver_unregister(&da8xx_dt_driver);
}

postcore_initcall(da8xx_dt_init);
module_exit(da8xx_dt_exit);
