// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
/*
 *
 * (C) COPYRIGHT 2016-2023 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 */

#include <linux/freezer.h>
#include <uapi/linux/thermal.h>
#include <linux/thermal.h>
#if IS_ENABLED(CONFIG_DEVFREQ_THERMAL)
#include <linux/devfreq_cooling.h>
#endif
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <soc/rockchip/rockchip_ipa.h>

#include "mali_kbase.h"
#include "mali_kbase_defs.h"
#include "mali_kbase_ipa_simple.h"
#include "mali_kbase_ipa_debugfs.h"

#if MALI_USE_CSF

/* This is used if the dynamic power for top-level is estimated separately
 * through the counter model. To roughly match the contribution of top-level
 * power in the total dynamic power, when calculated through counter model,
 * this scalar is used for the dynamic coefficient specified in the device tree
 * for simple power model. This value was provided by the HW team after
 * taking all the power data collected and dividing top level power by shader
 * core power and then averaging it across all samples.
 */
#define TOP_LEVEL_DYN_COEFF_SCALER (3)

#endif /* MALI_USE_CSF */

#if MALI_UNIT_TEST

static int dummy_temp;

static int kbase_simple_power_model_get_dummy_temp(struct thermal_zone_device *tz, int *temp)
{
	CSTD_UNUSED(tz);

	*temp = READ_ONCE(dummy_temp);
	return 0;
}

/* Intercept calls to the kernel function using a macro */
#ifdef thermal_zone_get_temp
#undef thermal_zone_get_temp
#endif
#define thermal_zone_get_temp(tz, temp) kbase_simple_power_model_get_dummy_temp(tz, temp)

void kbase_simple_power_model_set_dummy_temp(int temp)
{
	WRITE_ONCE(dummy_temp, temp);
}
KBASE_EXPORT_TEST_API(kbase_simple_power_model_set_dummy_temp);

#endif /* MALI_UNIT_TEST */

/*
 * This model is primarily designed for the Juno platform. It may not be
 * suitable for other platforms. The additional resources in this model
 * should preferably be minimal, as this model is rarely used when a dynamic
 * model is available.
 */

/**
 * struct kbase_ipa_model_simple_data - IPA context per device
 * @dynamic_coefficient: dynamic coefficient of the model
 * @static_coefficient:  static coefficient of the model
 * @ts:                  Thermal scaling coefficients of the model
 * @tz_name:             Thermal zone name
 * @gpu_tz:              thermal zone device
 * @poll_temperature_thread: Handle for temperature polling thread
 * @current_temperature: Most recent value of polled temperature
 * @temperature_poll_interval_ms: How often temperature should be checked, in ms
 * @vendor_model:        the parameters come from the GPU node's Rockchip
 *                       "simple-power-model" child, in its conventions (see
 *                       vendor_model_init())
 */

struct kbase_ipa_model_simple_data {
	u32 dynamic_coefficient;
	u32 static_coefficient;
	s32 ts[4];
	char tz_name[THERMAL_NAME_LENGTH];
	struct thermal_zone_device *gpu_tz;
	struct task_struct *poll_temperature_thread;
	int current_temperature;
	int temperature_poll_interval_ms;
	bool vendor_model;
};
#define FALLBACK_STATIC_TEMPERATURE 55000

/**
 * calculate_temp_scaling_factor() - Calculate temperature scaling coefficient
 * @ts:		Signed coefficients, in order t^0 to t^3, with units Deg^-N
 * @t:		Temperature, in mDeg C. Range: -2^17 < t < 2^17
 *
 * Scale the temperature according to a cubic polynomial whose coefficients are
 * provided in the device tree. The result is used to scale the static power
 * coefficient, where 1000000 means no change.
 *
 * Return: Temperature scaling factor. Range 0 <= ret <= 10,000,000.
 */
static u32 calculate_temp_scaling_factor(s32 ts[4], s64 t)
{
	/* Range: -2^24 < t2 < 2^24 m(Deg^2) */
	const s64 t2 = div_s64((t * t), 1000);

	/* Range: -2^31 < t3 < 2^31 m(Deg^3) */
	const s64 t3 = div_s64((t * t2), 1000);

	/*
	 * Sum the parts. t^[1-3] are in m(Deg^N), but the coefficients are in
	 * Deg^-N, so we need to multiply the last coefficient by 1000.
	 * Range: -2^63 < res_big < 2^63
	 */
	const s64 res_big = ts[3] * t3 /* +/- 2^62 */
			    + ts[2] * t2 /* +/- 2^55 */
			    + ts[1] * t /* +/- 2^48 */
			    + ts[0] * (s64)1000; /* +/- 2^41 */

	/* Range: -2^60 < res_unclamped < 2^60 */
	s64 res_unclamped = div_s64(res_big, 1000);

	/* Clamp to range of 0x to 10x the static power */
	return clamp(res_unclamped, (s64)0, (s64)10000000);
}

/* We can't call thermal_zone_get_temp() directly in model_static_coeff(),
 * because we don't know if tz->lock is held in the same thread. So poll it in
 * a separate thread to get around this.
 */
static int poll_temperature(void *data)
{
	struct kbase_ipa_model_simple_data *model_data = (struct kbase_ipa_model_simple_data *)data;
	int temp;

	set_freezable();

	while (!kthread_should_stop()) {
		struct thermal_zone_device *tz = READ_ONCE(model_data->gpu_tz);

		if (tz) {
			int ret;

			ret = thermal_zone_get_temp(tz, &temp);
			if (ret) {
				pr_warn_ratelimited(
					"Error reading temperature for gpu thermal zone: %d\n",
					ret);
				temp = FALLBACK_STATIC_TEMPERATURE;
			}
		} else {
			temp = FALLBACK_STATIC_TEMPERATURE;
		}

		WRITE_ONCE(model_data->current_temperature, temp);

		msleep_interruptible(READ_ONCE(model_data->temperature_poll_interval_ms));

		try_to_freeze();
	}

	return 0;
}

static int model_static_coeff(struct kbase_ipa_model *model, u32 *coeffp)
{
	u32 temp_scaling_factor;
	struct kbase_ipa_model_simple_data *model_data =
		(struct kbase_ipa_model_simple_data *)model->model_data;
	u64 coeff_big;
	int temp;

	temp = READ_ONCE(model_data->current_temperature);

	/* Range: 0 <= temp_scaling_factor < 2^24 */
	temp_scaling_factor = calculate_temp_scaling_factor(model_data->ts, temp);

	/*
	 * Range: 0 <= coeff_big < 2^52 to avoid overflowing *coeffp. This
	 * means static_coefficient must be in range
	 * 0 <= static_coefficient < 2^28.
	 */
	coeff_big = (u64)model_data->static_coefficient * (u64)temp_scaling_factor;
	*coeffp = div_u64(coeff_big, 1000000);

	return 0;
}

static int model_dynamic_coeff(struct kbase_ipa_model *model, u32 *coeffp)
{
	struct kbase_ipa_model_simple_data *model_data =
		(struct kbase_ipa_model_simple_data *)model->model_data;

#if MALI_USE_CSF
	/* On CSF GPUs, the dynamic power for top-level and shader cores is
	 * estimated separately. Currently there is a single dynamic
	 * coefficient value provided in the device tree for simple model.
	 * As per the discussion with HW team the coefficient value needs to
	 * be scaled down for top-level to limit its contribution in the
	 * total dyanmic power.
	 */
	coeffp[KBASE_IPA_BLOCK_TYPE_TOP_LEVEL] =
		model_data->dynamic_coefficient / TOP_LEVEL_DYN_COEFF_SCALER;
	coeffp[KBASE_IPA_BLOCK_TYPE_SHADER_CORES] = model_data->dynamic_coefficient;
#else
	*coeffp = model_data->dynamic_coefficient;
#endif

	return 0;
}

/*
 * Static power in the Rockchip "simple-power-model" conventions, the same arithmetic as
 * rockchip_ipa_get_static_power() (drivers/soc/rockchip/rockchip_ipa.c): the ts
 * coefficients are stored x10 (hence / 10000 where the ARM model divides by 1000), and
 * the static power scales as V^4. The temperature is the one poll_temperature() keeps,
 * not a thermal_zone_get_temp() call from here (see the comment above poll_temperature()).
 */
static u32 vendor_temp_scaling_factor(s32 ts[4], s64 t)
{
	const s64 t2 = div_s64((t * t), 1000);
	const s64 t3 = div_s64((t * t2), 1000);
	const s64 res_big = ts[3] * t3 + ts[2] * t2 + ts[1] * t + ts[0] * 1000LL;
	s64 res_unclamped = div_s64(res_big, 10000);

	return clamp(res_unclamped, (s64)0, (s64)10000000);
}

static u32 vendor_volt_scaling_factor(const u32 voltage_mv)
{
	const u32 v2 = (voltage_mv * voltage_mv) / 1000;
	const u32 v3 = (v2 * voltage_mv) / 1000;

	return (v3 * voltage_mv) / 1000;
}

static int model_static_power(struct kbase_ipa_model *model, u32 voltage, u32 *powerp)
{
	struct kbase_ipa_model_simple_data *model_data =
		(struct kbase_ipa_model_simple_data *)model->model_data;
	u64 power_big;
	u32 power;

	if (!model_data->vendor_model)
		return -ENOENT;

	power_big = (u64)model_data->static_coefficient *
		    vendor_temp_scaling_factor(model_data->ts,
					       READ_ONCE(model_data->current_temperature));
	power = div_u64(power_big, 1000000);
	power_big = (u64)power * vendor_volt_scaling_factor(voltage);
	*powerp = div_u64(power_big, 1000000);

	return 0;
}

/*
 * The GPU's own power model. Rockchip describes the RK356x Mali-G52 with a
 * "simple-power-model" child of the GPU node (rk356x.dtsi gpu_power_model; the stock T40
 * DTB carries the same node and values), not with this driver's
 * "arm,mali-simple-power-model" binding, so without this the model starts with every
 * coefficient zero and no thermal zone. The 4.19 Rockchip kbase consumed that node
 * (backend/gpu/mali_kbase_devfreq.c there: rockchip_ipa_power_model_init() for the
 * static power, its dynamic-coefficient for the dynamic power). Here it fills this
 * model's parameters: dynamic-coefficient has the same units in both conventions
 * (P = c * f[MHz] * V^2 / 1000 mW, kbase_scale_dynamic_power()), and the static
 * coefficient is the one rockchip_ipa_power_model_init() scales by this chip's eFuse
 * leakage. An arm,mali-simple-power-model node, if present, still takes precedence.
 */
static void vendor_model_init(struct kbase_ipa_model *model)
{
	struct kbase_ipa_model_simple_data *model_data =
		(struct kbase_ipa_model_simple_data *)model->model_data;
	struct device *dev = model->kbdev->dev;
	struct ipa_power_model_data *rk;
	struct device_node *np;
	const char *tz_name;
	char compat[64];

	scnprintf(compat, sizeof(compat), "arm,%s", model->ops->name);
	np = of_get_compatible_child(dev->of_node, compat);
	if (np) {
		of_node_put(np);
		return;
	}

	np = of_get_compatible_child(dev->of_node, "simple-power-model");
	if (!np)
		return;

	if (of_property_read_string(np, "thermal-zone", &tz_name)) {
		dev_warn(dev, "simple-power-model has no thermal-zone\n");
		goto out;
	}

	rk = rockchip_ipa_power_model_init(dev, "leakage");
	if (IS_ERR_OR_NULL(rk)) {
		dev_warn(dev, "simple-power-model unusable (%ld)\n", PTR_ERR(rk));
		goto out;
	}

	model_data->static_coefficient = rk->static_coefficient;
	model_data->dynamic_coefficient = rk->dynamic_coefficient;
	memcpy(model_data->ts, rk->ts, sizeof(model_data->ts));
	strscpy(model_data->tz_name, tz_name, sizeof(model_data->tz_name));
	model_data->vendor_model = true;
	dev_info(dev, "power model: simple-power-model, leakage %u, static %u, dynamic %u, zone %s\n",
		 rk->leakage, rk->static_coefficient, rk->dynamic_coefficient, tz_name);
	kfree(rk);
out:
	of_node_put(np);
}

static int add_params(struct kbase_ipa_model *model)
{
	int err = 0;
	struct kbase_ipa_model_simple_data *model_data =
		(struct kbase_ipa_model_simple_data *)model->model_data;
	/* With the vendor model the values are already set; the DT entries are optional. */
	const bool dt_required = !model_data->vendor_model;

	err = kbase_ipa_model_add_param_s32(model, "static-coefficient",
					    (s32 *)&model_data->static_coefficient, 1, dt_required);
	if (err)
		goto end;

	err = kbase_ipa_model_add_param_s32(model, "dynamic-coefficient",
					    (s32 *)&model_data->dynamic_coefficient, 1, dt_required);
	if (err)
		goto end;

	err = kbase_ipa_model_add_param_s32(model, "ts", model_data->ts, 4, dt_required);
	if (err)
		goto end;

	err = kbase_ipa_model_add_param_string(model, "thermal-zone", model_data->tz_name,
					       sizeof(model_data->tz_name), dt_required);
	if (err)
		goto end;

	model_data->temperature_poll_interval_ms = 200;
	err = kbase_ipa_model_add_param_s32(model, "temp-poll-interval-ms",
					    &model_data->temperature_poll_interval_ms, 1, false);

end:
	return err;
}

static int kbase_simple_power_model_init(struct kbase_ipa_model *model)
{
	int err;
	struct kbase_ipa_model_simple_data *model_data;

	model_data = kzalloc(sizeof(struct kbase_ipa_model_simple_data), GFP_KERNEL);
	if (!model_data)
		return -ENOMEM;

	model->model_data = (void *)model_data;

	model_data->current_temperature = FALLBACK_STATIC_TEMPERATURE;
	model_data->poll_temperature_thread = kthread_run(poll_temperature, (void *)model_data,
							  "mali-simple-power-model-temp-poll");
	if (IS_ERR(model_data->poll_temperature_thread)) {
		err = PTR_ERR(model_data->poll_temperature_thread);
		kfree(model_data);
		return err;
	}

	vendor_model_init(model);
	err = add_params(model);
	if (err) {
		kbase_ipa_model_param_free_all(model);
		kthread_stop(model_data->poll_temperature_thread);
		kfree(model_data);
	}

	return err;
}

static int kbase_simple_power_model_recalculate(struct kbase_ipa_model *model)
{
	struct kbase_ipa_model_simple_data *model_data =
		(struct kbase_ipa_model_simple_data *)model->model_data;
	struct thermal_zone_device *tz;

	lockdep_assert_held(&model->kbdev->ipa.lock);

	if (!strnlen(model_data->tz_name, sizeof(model_data->tz_name))) {
		model_data->gpu_tz = NULL;
		dev_warn(model->kbdev->dev,
			 "No thermal zone specified, will use the default temperature value of %u",
			 FALLBACK_STATIC_TEMPERATURE);
	} else {
		char tz_name[THERMAL_NAME_LENGTH];
		u32 string_len = strscpy(tz_name, model_data->tz_name, sizeof(tz_name));

		string_len += sizeof(char);
		/* Make sure that the source string fit into the buffer. */
		KBASE_DEBUG_ASSERT(string_len <= sizeof(tz_name));
		CSTD_UNUSED(string_len);

		/* Release ipa.lock so that thermal_list_lock is not acquired
		 * with ipa.lock held, thereby avoid lock ordering violation
		 * lockdep warning. The warning comes as a chain of locks
		 * ipa.lock --> thermal_list_lock --> tz->lock gets formed
		 * on registering devfreq cooling device when probe method
		 * of mali platform driver is invoked.
		 */
		mutex_unlock(&model->kbdev->ipa.lock);
		tz = thermal_zone_get_zone_by_name(tz_name);
		mutex_lock(&model->kbdev->ipa.lock);

		if (IS_ERR_OR_NULL(tz)) {
			pr_warn_ratelimited(
				"Error %d getting thermal zone \'%s\', not yet ready?\n",
				PTR_ERR_OR_ZERO(tz), tz_name);
			return -EPROBE_DEFER;
		}

		/* Check if another thread raced against us & updated the
		 * thermal zone name string. Update the gpu_tz pointer only if
		 * the name string did not change whilst we retrieved the new
		 * thermal_zone_device pointer, otherwise model_data->tz_name &
		 * model_data->gpu_tz would become inconsistent with each other.
		 * The below check will succeed only for the thread which last
		 * updated the name string.
		 */
		if (strncmp(tz_name, model_data->tz_name, sizeof(tz_name)) == 0)
			model_data->gpu_tz = tz;
	}

	return 0;
}

static void kbase_simple_power_model_term(struct kbase_ipa_model *model)
{
	struct kbase_ipa_model_simple_data *model_data =
		(struct kbase_ipa_model_simple_data *)model->model_data;

	kthread_stop(model_data->poll_temperature_thread);

	kfree(model_data);
}

struct kbase_ipa_model_ops kbase_simple_ipa_model_ops = {
	.name = "mali-simple-power-model",
	.init = &kbase_simple_power_model_init,
	.recalculate = &kbase_simple_power_model_recalculate,
	.term = &kbase_simple_power_model_term,
	.get_dynamic_coeff = &model_dynamic_coeff,
	.get_static_coeff = &model_static_coeff,
	.get_static_power = &model_static_power,
};
KBASE_EXPORT_TEST_API(kbase_simple_ipa_model_ops);
