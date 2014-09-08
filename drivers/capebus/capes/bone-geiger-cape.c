/*
 * Driver for beaglebone Geiger cape
 *
 * Copyright (C) 2012 Pantelis Antoniou <panto@antoniou-consulting.com>
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
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/consumer.h>
#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/clkdev.h>
#include <linux/pwm.h>
#include <linux/math64.h>
#include <linux/atomic.h>
#include <linux/leds.h>
#include <linux/input/ti_am335x_tsc.h>
#include <linux/platform_data/ti_am335x_adc.h>
#include <linux/mfd/ti_am335x_tscadc.h>
#include <linux/iio/iio.h>
#include <linux/iio/machine.h>
#include <linux/iio/consumer.h>

#include <linux/capebus/capebus-bone.h>

/* fwd decl. */
extern struct cape_driver bonegeiger_driver;

struct bone_geiger_info {
	struct cape_dev *dev;
	struct bone_capebus_generic_info *geninfo;
	struct pwm_device *pwm_dev;
	int pwm_frequency;
	int pwm_duty_cycle;
	int run;
	atomic64_t counter;
	int event_gpio;
	int event_irq;
	struct led_trigger *event_led;		/* event detect */
	struct led_trigger *run_led;		/* running      */
	unsigned long event_blink_delay;
	struct sysfs_dirent *counter_sd;	/* notifier */
	const char *vsense_name;
	unsigned int vsense_scale;
	struct iio_channel *vsense_channel;
};

static const struct of_device_id bonegeiger_of_match[] = {
	{
		.compatible = "bone-geiger-cape",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, bonegeiger_of_match);

static int bonegeiger_start(struct cape_dev *dev)
{
	struct bone_geiger_info *info = dev->drv_priv;
	int duty, period;

	if (info->run != 0)
		return 0;

	/* checks */
	if (info->pwm_frequency < 1000 || info->pwm_frequency > 50000) {
		dev_err(&dev->dev, "Cowardly refusing to use a "
				"frequency of %d\n",
				info->pwm_frequency);
		return -EINVAL;
	}
	if (info->pwm_duty_cycle > 80) {
		dev_err(&dev->dev, "Cowardly refusing to use a "
				"duty cycle of %d\n",
				info->pwm_duty_cycle);
		return -EINVAL;
	}

	period = div_u64(1000000000LLU, info->pwm_frequency);
	duty = (period * info->pwm_duty_cycle) / 100;

	dev_info(&dev->dev, "starting geiger tube with "
			"duty=%duns period=%dus\n",
			duty, period);

	pwm_config(info->pwm_dev, duty, period);
	pwm_enable(info->pwm_dev);

	info->run = 1;
	led_trigger_event(info->run_led, LED_FULL);

	return 0;
}

static int bonegeiger_stop(struct cape_dev *dev)
{
	struct bone_geiger_info *info = dev->drv_priv;

	if (info->run == 0)
		return 0;

	dev_info(&dev->dev, "disabling geiger tube\n");
	pwm_config(info->pwm_dev, 0, 50000);	/* 0% duty cycle, 20KHz */
	pwm_disable(info->pwm_dev);

	info->run = 0;
	led_trigger_event(info->run_led, LED_OFF);

	return 0;
}

static ssize_t bonegeiger_show_run(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct cape_dev *cdev = to_cape_dev(dev);
	struct bone_geiger_info *info = cdev->drv_priv;

	return sprintf(buf, "%d\n", info->run);
}

static ssize_t bonegeiger_store_run(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct cape_dev *cdev = to_cape_dev(dev);
	int run, err;

	if (sscanf(buf, "%i", &run) != 1)
		return -EINVAL;

	if (run)
		err = bonegeiger_start(cdev);
	else
		err = bonegeiger_stop(cdev);

	return err ? err : count;
}

static ssize_t bonegeiger_show_counter(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct cape_dev *cdev = to_cape_dev(dev);
	struct bone_geiger_info *info = cdev->drv_priv;

	return sprintf(buf, "%llu\n", atomic64_read(&info->counter));
}

static ssize_t bonegeiger_store_counter(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct cape_dev *cdev = to_cape_dev(dev);
	struct bone_geiger_info *info = cdev->drv_priv;

	atomic64_set(&info->counter, 0);	/* just reset */
	return count;
}

static ssize_t bonegeiger_show_vsense(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct cape_dev *cdev = to_cape_dev(dev);
	struct bone_geiger_info *info = cdev->drv_priv;
	int ret, val;
	u32 mvolts;

	ret = iio_read_channel_raw(info->vsense_channel, &val);
	if (ret < 0)
		return ret;

	/* V = (1800 / 4096) * val * scale) = (1.8 * val * scale / 4096) */
	mvolts = div_u64(1800 * info->vsense_scale * (u64)val, 4096 * 100);

	return sprintf(buf, "%d\n", mvolts);
}

static DEVICE_ATTR(run, S_IRUGO | S_IWUSR,
		bonegeiger_show_run, bonegeiger_store_run);
static DEVICE_ATTR(counter, S_IRUGO | S_IWUSR,
		bonegeiger_show_counter, bonegeiger_store_counter);
static DEVICE_ATTR(vsense, S_IRUGO,
		bonegeiger_show_vsense, NULL);

static int bonegeiger_sysfs_register(struct cape_dev *cdev)
{
	int err;

	err = device_create_file(&cdev->dev, &dev_attr_run);
	if (err != 0)
		goto err_no_run;

	err = device_create_file(&cdev->dev, &dev_attr_counter);
	if (err != 0)
		goto err_no_counter;

	err = device_create_file(&cdev->dev, &dev_attr_vsense);
	if (err != 0)
		goto err_no_vsense;

	return 0;

err_no_vsense:
	device_remove_file(&cdev->dev, &dev_attr_counter);
err_no_counter:
	device_remove_file(&cdev->dev, &dev_attr_run);
err_no_run:
	return err;
}

static void bonegeiger_sysfs_unregister(struct cape_dev *cdev)
{
	device_remove_file(&cdev->dev, &dev_attr_vsense);
	device_remove_file(&cdev->dev, &dev_attr_counter);
	device_remove_file(&cdev->dev, &dev_attr_run);
}

static irqreturn_t bonegeiger_irq_handler(int irq, void *dev_id)
{
	struct cape_dev *dev = dev_id;
	struct bone_geiger_info *info = dev->drv_priv;

	atomic64_inc(&info->counter);

	led_trigger_blink_oneshot(info->event_led,
		  &info->event_blink_delay, &info->event_blink_delay, 0);

	sysfs_notify_dirent(info->counter_sd);

	return IRQ_HANDLED;
}

static int bonegeiger_probe(struct cape_dev *dev, const struct cape_device_id *id)
{
	char boardbuf[33];
	char versionbuf[5];
	const char *board_name;
	const char *version;
	struct bone_geiger_info *info;
	struct pinctrl *pinctrl;
	struct device_node *node, *pwm_node;
	phandle phandle;
	u32 val;
	int err;

	/* boiler plate probing */
	err = bone_capebus_probe_prolog(dev, id);
	if (err != 0)
		return err;

	/* get the board name (after check of cntrlboard match) */
	board_name = bone_capebus_id_get_field(id, BONE_CAPEBUS_BOARD_NAME,
			boardbuf, sizeof(boardbuf));
	/* get the board version */
	version = bone_capebus_id_get_field(id, BONE_CAPEBUS_VERSION,
			versionbuf, sizeof(versionbuf));
	/* should never happen; but check anyway */
	if (board_name == NULL || version == NULL)
		return -ENODEV;

	dev->drv_priv = devm_kzalloc(&dev->dev, sizeof(*info), GFP_KERNEL);
	if (dev->drv_priv == NULL) {
		dev_err(&dev->dev, "Failed to allocate info\n");
		err = -ENOMEM;
		goto err_no_mem;
	}
	info = dev->drv_priv;

	pinctrl = devm_pinctrl_get_select_default(&dev->dev);
	if (IS_ERR(pinctrl))
		dev_warn(&dev->dev,
			"pins are not configured from the driver\n");

	node = capebus_of_find_property_node(dev, "version", version, "pwms");
	if (node == NULL) {
		dev_err(&dev->dev, "unable to find pwms property\n");
		err = -ENODEV;
		goto err_no_pwm;
	}

	err = of_property_read_u32(node, "pwms", &val);
	if (err != 0) {
		dev_err(&dev->dev, "unable to read pwm handle\n");
		goto err_no_pwm;
	}
	phandle = val;

	pwm_node = of_find_node_by_phandle(phandle);
	if (pwm_node == NULL) {
		dev_err(&dev->dev, "Failed to pwm node\n");
		err = -EINVAL;
		goto err_no_pwm;
	}

	err = capebus_of_platform_device_enable(pwm_node);
	of_node_put(pwm_node);
	if (err != 0) {
		dev_err(&dev->dev, "Failed to pwm node\n");
		goto err_no_pwm;
	}

	info->pwm_dev = of_pwm_request(node, NULL);
	of_node_put(node);
	if (IS_ERR(info->pwm_dev)) {
		dev_err(&dev->dev, "unable to request PWM\n");
		err = PTR_ERR(info->pwm_dev);
		goto err_no_pwm;
	}

	if (capebus_of_property_read_u32(dev,
				"version", version,
				"pwm-frequency", &val) != 0) {
		val = 20000;
		dev_warn(&dev->dev, "Could not read pwm-frequency property; "
				"using default %u\n",
				val);
	}
	info->pwm_frequency = val;

	if (capebus_of_property_read_u32(dev,
				"version", version,
				"pwm-duty-cycle", &val) != 0) {
		val = 60;
		dev_warn(&dev->dev, "Could not read pwm-duty-cycle property; "
				"using default %u\n",
				val);
	}
	info->pwm_duty_cycle = val;

	node = capebus_of_find_property_node(dev, "gpios", version, "pwms");
	info->event_gpio = of_get_gpio_flags(node, 0, NULL);
	of_node_put(node);
	if (IS_ERR_VALUE(info->event_gpio)) {
		dev_err(&dev->dev, "unable to get event GPIO\n");
		err = info->event_gpio;
		goto err_no_gpio;
	}

	err = gpio_request_one(info->event_gpio,
			GPIOF_DIR_IN | GPIOF_EXPORT,
			"bone-geiger-cape-event");
	if (err != 0) {
		dev_err(&dev->dev, "failed to request event GPIO\n");
		goto err_no_gpio;
	}

	atomic64_set(&info->counter, 0);

	info->event_irq = gpio_to_irq(info->event_gpio);
	if (IS_ERR_VALUE(info->event_irq)) {
		dev_err(&dev->dev, "unable to get event GPIO IRQ\n");
		err = info->event_irq;
		goto err_no_irq;
	}

	err = request_irq(info->event_irq, bonegeiger_irq_handler,
			IRQF_TRIGGER_RISING | IRQF_SHARED,
			"bone-geiger-irq", dev);
	if (err != 0) {
		dev_err(&dev->dev, "unable to request irq\n");
		goto err_no_irq;
	}

	err = bonegeiger_sysfs_register(dev);
	if (err != 0) {
		dev_err(&dev->dev, "unable to register sysfs\n");
		goto err_no_sysfs;
	}

	info->counter_sd = sysfs_get_dirent(dev->dev.kobj.sd, NULL, "counter");
	if (info->counter_sd == NULL) {
		dev_err(&dev->dev, "unable to get dirent of counter\n");
		err = -ENODEV;
		goto err_no_counter_dirent;
	}

	led_trigger_register_simple("geiger-event", &info->event_led);
	led_trigger_register_simple("geiger-run", &info->run_led);

	/* pick up the generics; tsc & leds */
	info->geninfo = bone_capebus_probe_generic(dev, id);
	if (info->geninfo == NULL) {
		dev_err(&dev->dev, "Could not probe generic\n");
		goto err_no_generic;
	}

	led_trigger_event(info->run_led, LED_OFF);

	/* default */
	if (capebus_of_property_read_u32(dev,
				"version", version,
				"event-blink-delay", &val) != 0) {
		val = 30;
		dev_warn(&dev->dev, "Could not read event-blink-delay "
				"property; using default %u\n",
					val);
	}
	info->event_blink_delay = val;

	/* default */
	if (capebus_of_property_read_string(dev,
				"version", version,
				"vsense-name", &info->vsense_name) != 0) {
		info->vsense_name = "AIN5";
		dev_warn(&dev->dev, "Could not read vsense-name property; "
				"using default '%s'\n",
					info->vsense_name);
	}

	if (capebus_of_property_read_u32(dev,
				"version", version,
				"vsense-scale", &info->vsense_scale) != 0) {
		info->vsense_scale = 37325;	/* 373.25 */
		dev_warn(&dev->dev, "Could not read vsense-scale property; "
				"using default %u\n",
					info->vsense_scale);
	}

	info->vsense_channel = iio_channel_get(NULL, info->vsense_name);
	if (IS_ERR(info->vsense_channel)) {
		dev_err(&dev->dev, "Could not get AIN5 analog input\n");
		err = PTR_ERR(info->vsense_channel);
		goto err_no_vsense;
	}

	dev_info(&dev->dev, "ready\n");

	err = bonegeiger_start(dev);
	if (err != 0) {
		dev_err(&dev->dev, "Could not start geiger device\n");
		goto err_no_start;
	}

	return 0;

err_no_start:
	iio_channel_release(info->vsense_channel);
err_no_vsense:
	bone_capebus_remove_generic(info->geninfo);
err_no_generic:
	led_trigger_unregister_simple(info->run_led);
	led_trigger_unregister_simple(info->event_led);
	sysfs_put(info->counter_sd);
err_no_counter_dirent:
	bonegeiger_sysfs_unregister(dev);
err_no_sysfs:
	free_irq(info->event_irq, dev);
err_no_irq:
	gpio_free(info->event_gpio);
err_no_gpio:
	pwm_put(info->pwm_dev);
err_no_pwm:
	devm_kfree(&dev->dev, info);
err_no_mem:
	return err;
}

static void bonegeiger_remove(struct cape_dev *dev)
{
	struct bone_geiger_info *info = dev->drv_priv;

	dev_info(&dev->dev, "Removing geiger cape driver...\n");

	bonegeiger_stop(dev);

	iio_channel_release(info->vsense_channel);
	bone_capebus_remove_generic(info->geninfo);
	led_trigger_unregister_simple(info->run_led);
	led_trigger_unregister_simple(info->event_led);
	sysfs_put(info->counter_sd);
	bonegeiger_sysfs_unregister(dev);
	free_irq(info->event_irq, dev);
	gpio_free(info->event_gpio);
	pwm_put(info->pwm_dev);
}

struct cape_driver bonegeiger_driver = {
	.driver = {
		.name		= "bonegeiger",
		.owner		= THIS_MODULE,
		.of_match_table	= bonegeiger_of_match,
	},
	.probe		= bonegeiger_probe,
	.remove		= bonegeiger_remove,
};

module_capebus_driver(bonegeiger_driver);

MODULE_AUTHOR("Pantelis Antoniou");
MODULE_DESCRIPTION("Beaglebone geiger cape");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:bone-geiger-cape");
