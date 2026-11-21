// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Rockchip RK805 PMIC Power Key driver
 *
 * Copyright (c) 2017, Fuzhou Rockchip Electronics Co., Ltd
 *
 * Author: Joseph Chen <chenjh@rock-chips.com>
 */

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

/* NovaStar TU carriers: the stock 4.19 driver consults the "low power" flag of the NovaStar
 * low_power_ctrl driver (kernel/modules/tu2k/low_power_ctrl.c here) before it reports KEY_POWER,
 * and while the flag is set it records the release in pwrkey_wakeup and replays one press at
 * resume. In the stock Image this is pwrkey_fall_irq, pwrkey_rise_irq,
 * rk805_pwrkey_suspend and
 * rk805_pwrkey_resume. The stock flag driver is built in and these
 * handlers BL low_power_ctrl_get_state @ 0xffffff80085eb69c directly; on this kernel the flag
 * driver is a module and this driver is built in, so the call is resolved at run time
 * (symbol_get, which takes only a GPL export: kernel/module/main.c __symbol_get), and no module
 * reads as flag clear -- the stock flag's value until "enable" is written. This kernel is
 * shared with boards whose stock pwrkey driver has not been read, so all of it runs on the
 * two TU machine compatibles only, rockchip,rk3566-TU15-v1 and rockchip,rk3566-MeeBox210-v1
 * (TU20 Pro), which boot the same stock Image (models/tu20pro/boot-profile.toml
 * experimental_boot_sha256 = the TU15's). */
bool low_power_ctrl_get_state(void);

static bool nova_tu_low_power_ctrl(void)
{
	return of_machine_is_compatible("rockchip,rk3566-TU15-v1") ||
	       of_machine_is_compatible("rockchip,rk3566-MeeBox210-v1");
}

static bool nova_low_power_ctrl_get_state(void)
{
	bool (*get_state)(void);
	bool state;

	if (!nova_tu_low_power_ctrl())
		return false;
	get_state = symbol_get(low_power_ctrl_get_state);
	if (!get_state)
		return false;
	state = get_state();
	symbol_put(low_power_ctrl_get_state);
	return state;
}

/* pwrkey_wakeup @ 0xffffff8009f00f75 (kallsyms 'b', one byte) */
static bool pwrkey_wakeup;

static irqreturn_t pwrkey_fall_irq(int irq, void *_pwr)
{
	struct input_dev *pwr = _pwr;

	/* pwrkey_fall_irq @ 0xffffff80089abdc0: the first printk runs before the flag test (+0x24),
	 * the report and the second printk only while the flag is clear (+0x2c tbz). */
	if (nova_tu_low_power_ctrl())
		printk("%s ,%d\n", __func__, 0x1c);
	if (nova_low_power_ctrl_get_state())
		return IRQ_HANDLED;

	input_report_key(pwr, KEY_POWER, 1);
	input_sync(pwr);
	if (nova_tu_low_power_ctrl())
		printk("%s %s ,%d\n", "nova_low_power_ctrl", __func__, 0x20);

	return IRQ_HANDLED;
}

static irqreturn_t pwrkey_rise_irq(int irq, void *_pwr)
{
	struct input_dev *pwr = _pwr;

	/* pwrkey_rise_irq @ 0xffffff80089abe4c: flag set -> no report, pwrkey_wakeup = 1 (+0x54). */
	if (nova_tu_low_power_ctrl())
		printk("TAG %s ,%d\n", __func__, 0x29);
	if (nova_low_power_ctrl_get_state()) {
		pwrkey_wakeup = true;
		printk("%s %s ,%d set pwrkey_wakeup = true\n", "nova_low_power_ctrl", __func__, 0x30);
		return IRQ_HANDLED;
	}

	input_report_key(pwr, KEY_POWER, 0);
	input_sync(pwr);
	if (nova_tu_low_power_ctrl())
		printk("%s send %s KEY_POWER 0,%d\n", "nova_low_power_ctrl", __func__, 0x2d);

	return IRQ_HANDLED;
}

static void rk8xx_get_input_info(const char *device_name,
				 const char **input_name,
				 const char **input_phys)
{
	if (strstr(device_name, "rk806")) {
		*input_name = "rk806 pwrkey";
		*input_phys = "rk806_pwrkey/input0";
	} else if (strstr(device_name, "rk809") ||
		   strstr(device_name, "rk817")) {
		*input_name = "rk809 pwrkey";
		*input_phys = "rk809_pwrkey/input0";
	} else if (strstr(device_name, "rk816")) {
		*input_name = "rk816 pwrkey";
		*input_phys = "rk816_pwrkey/input0";
	} else if (strstr(device_name, "rk818")) {
		*input_name = "rk818 pwrkey";
		*input_phys = "rk818_pwrkey/input0";
	} else {
		*input_name = "rk805 pwrkey";
		*input_phys = "rk805_pwrkey/input0";
	}
}

static int rk805_pwrkey_probe(struct platform_device *pdev)
{
	const char *input_name, *input_phys;
	struct input_dev *pwr;
	int fall_irq, rise_irq;
	struct device_node *np;
	int err;

	np = of_get_child_by_name(pdev->dev.parent->of_node, "pwrkey");
	if (np && !of_device_is_available(np)) {
		dev_info(&pdev->dev, "device is disabled\n");
		return -EINVAL;
	}

	rk8xx_get_input_info(pdev->name, &input_name, &input_phys);
	/* NovaStar TU carriers: the stock 4.19 driver registers this input as "rk805 pwrkey" /
	 * "rk805_pwrkey/input0" whatever the PMIC -- the only pwrkey name strings in the stock Image,
	 * and the stock boot log lists input1 "rk805 pwrkey" on the TU15's RK809 -- and the vendor
	 * launcher recognises the front POWER button by that name (NewLauncher
	 * InputDeviceExKt.java RK805_PWRKEY, isFontPanel). The TU20 Pro (MeeBox210) boots
	 * that same stock Image (models/tu20pro/boot-profile.toml experimental_boot_sha256 =
	 * the TU15's), so both TU machine compatibles take the stock name. */
	if (of_machine_is_compatible("rockchip,rk3566-TU15-v1") ||
	    of_machine_is_compatible("rockchip,rk3566-MeeBox210-v1")) {
		input_name = "rk805 pwrkey";
		input_phys = "rk805_pwrkey/input0";
	}
	dev_info(&pdev->dev, "Probing %s -> %s\n", pdev->name, input_name);

	pwr = devm_input_allocate_device(&pdev->dev);
	if (!pwr) {
		dev_err(&pdev->dev, "Can't allocate power button\n");
		return -ENOMEM;
	}

	pwr->name = input_name;
	pwr->phys = input_phys;
	pwr->id.bustype = BUS_HOST;
	input_set_capability(pwr, EV_KEY, KEY_POWER);

	fall_irq = platform_get_irq(pdev, 0);
	if (fall_irq < 0)
		return fall_irq;

	rise_irq = platform_get_irq(pdev, 1);
	if (rise_irq < 0)
		return rise_irq;

	err = devm_request_any_context_irq(&pwr->dev, fall_irq,
					   pwrkey_fall_irq,
					   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					   "rk805_pwrkey_fall", pwr);
	if (err < 0) {
		dev_err(&pdev->dev, "Can't register fall irq: %d\n", err);
		return err;
	}

	err = devm_request_any_context_irq(&pwr->dev, rise_irq,
					   pwrkey_rise_irq,
					   IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					   "rk805_pwrkey_rise", pwr);
	if (err < 0) {
		dev_err(&pdev->dev, "Can't register rise irq: %d\n", err);
		return err;
	}

	err = input_register_device(pwr);
	if (err) {
		dev_err(&pdev->dev, "Can't register power button: %d\n", err);
		return err;
	}

	platform_set_drvdata(pdev, pwr);
	device_init_wakeup(&pdev->dev, true);

	return 0;
}

/* rk805_pwrkey_suspend @ 0xffffff80089abf00 */
static int rk805_pwrkey_suspend(struct device *dev)
{
	if (!nova_tu_low_power_ctrl())
		return 0;
	pwrkey_wakeup = false;
	printk("%s, %s ,%d pwrkey_wakeup = false\n", "nova_low_power_ctrl", __func__, 0x74);
	return 0;
}

/* rk805_pwrkey_resume @ 0xffffff80089abf3c: a release recorded while the flag was set is
 * replayed as one KEY_POWER press + release (input_event x3, +0x60..+0x88). */
static int rk805_pwrkey_resume(struct device *dev)
{
	struct input_dev *pwr = dev_get_drvdata(dev);

	if (!nova_tu_low_power_ctrl())
		return 0;
	printk("%s %s ,%d\n", "nova_low_power_ctrl", __func__, 0x7c);
	if (pwrkey_wakeup) {
		input_report_key(pwr, KEY_POWER, 1);
		input_report_key(pwr, KEY_POWER, 0);
		input_sync(pwr);
		printk("%s %s ,%d pwrkey_wakeup is true\n", "nova_low_power_ctrl", __func__, 0x81);
	}
	return 0;
}

/* rk805_pwrkey_pm_ops @ 0xffffff80093dad88: suspend/freeze/poweroff = rk805_pwrkey_suspend,
 * resume/thaw/restore = rk805_pwrkey_resume (the SIMPLE_DEV_PM_OPS slots). */
static DEFINE_SIMPLE_DEV_PM_OPS(rk805_pwrkey_pm_ops, rk805_pwrkey_suspend, rk805_pwrkey_resume);

static const struct platform_device_id rk8xx_pwrkey_id_table[] = {
	{ "rk805-pwrkey", 0 },
	{ "rk806-pwrkey", 0 },
	{ "rk809-pwrkey", 0 },
	{ "rk816-pwrkey", 0 },
	{ "rk817-pwrkey", 0 },
	{ "rk818-pwrkey", 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, rk8xx_pwrkey_id_table);

static struct platform_driver rk805_pwrkey_driver = {
	.probe	= rk805_pwrkey_probe,
	.id_table = rk8xx_pwrkey_id_table,
	.driver	= {
		.name = "rk805-pwrkey",
		.pm = pm_sleep_ptr(&rk805_pwrkey_pm_ops),	/* rk805_pwrkey_driver + 0x98 */
	},
};
module_platform_driver(rk805_pwrkey_driver);

MODULE_ALIAS("platform:rk805-pwrkey");
MODULE_AUTHOR("Joseph Chen <chenjh@rock-chips.com>");
MODULE_DESCRIPTION("RK805 PMIC Power Key driver");
MODULE_LICENSE("GPL");
