// SPDX-License-Identifier: GPL-2.0
/* Copyright 2019 Collabora ltd. */

#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/devfreq_cooling.h>
#include <linux/nvmem-consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <soc/rockchip/rockchip_opp_select.h>

#include "panfrost_device.h"
#include "panfrost_devfreq.h"

static void panfrost_devfreq_update_utilization(struct panfrost_devfreq *pfdevfreq)
{
	ktime_t now, last;

	now = ktime_get();
	last = pfdevfreq->time_last_update;

	if (pfdevfreq->busy_count > 0)
		pfdevfreq->busy_time += ktime_sub(now, last);
	else
		pfdevfreq->idle_time += ktime_sub(now, last);

	pfdevfreq->time_last_update = now;
}

static int panfrost_devfreq_target(struct device *dev, unsigned long *freq,
				   u32 flags)
{
	struct panfrost_device *pfdev = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	int err;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);

	err = dev_pm_opp_set_rate(dev, *freq);
	if (!err)
		pfdev->pfdevfreq.current_frequency = *freq;

	return err;
}

static void panfrost_devfreq_reset(struct panfrost_devfreq *pfdevfreq)
{
	pfdevfreq->busy_time = 0;
	pfdevfreq->idle_time = 0;
	pfdevfreq->time_last_update = ktime_get();
}

static int panfrost_devfreq_get_dev_status(struct device *dev,
					   struct devfreq_dev_status *status)
{
	struct panfrost_device *pfdev = dev_get_drvdata(dev);
	struct panfrost_devfreq *pfdevfreq = &pfdev->pfdevfreq;
	unsigned long irqflags;

	status->current_frequency = clk_get_rate(pfdev->clock);

	spin_lock_irqsave(&pfdevfreq->lock, irqflags);

	panfrost_devfreq_update_utilization(pfdevfreq);

	status->total_time = ktime_to_ns(ktime_add(pfdevfreq->busy_time,
						   pfdevfreq->idle_time));

	status->busy_time = ktime_to_ns(pfdevfreq->busy_time);

	panfrost_devfreq_reset(pfdevfreq);

	spin_unlock_irqrestore(&pfdevfreq->lock, irqflags);

	dev_dbg(pfdev->dev, "busy %lu total %lu %lu %% freq %lu MHz\n",
		status->busy_time, status->total_time,
		status->busy_time / (status->total_time / 100),
		status->current_frequency / 1000 / 1000);

	return 0;
}

static struct devfreq_dev_profile panfrost_devfreq_profile = {
	.timer = DEVFREQ_TIMER_DELAYED,
	.polling_ms = 50, /* ~3 frames */
	.target = panfrost_devfreq_target,
	.get_dev_status = panfrost_devfreq_get_dev_status,
};

static int panfrost_read_speedbin(struct device *dev)
{
	u32 val;
	int ret;

	ret = nvmem_cell_read_variable_le_u32(dev, "speed-bin", &val);
	if (ret) {
		/*
		 * -ENOENT means that this platform doesn't support speedbins
		 * as it didn't declare any speed-bin nvmem: in this case, we
		 * keep going without it; any other error means that we are
		 * supposed to read the bin value, but we failed doing so.
		 */
		if (ret != -ENOENT && ret != -EOPNOTSUPP) {
			DRM_DEV_ERROR(dev, "Cannot read speed-bin (%d).", ret);
			return ret;
		}

		return 0;
	}
	DRM_DEV_DEBUG(dev, "Using speed-bin = 0x%x\n", val);

	return devm_pm_opp_set_supported_hw(dev, &val, 1);
}

#if IS_REACHABLE(CONFIG_ROCKCHIP_OPP)
/*
 * A Rockchip GPU's OPP table can carry Rockchip's own voltage selection (by the chip's PVTM, leakage or bin:
 * rockchip,pvtm-voltage-sel, rockchip,leakage-voltage-sel, rockchip,bin-voltage-sel), which picks one of its
 * opp-microvolt-<name> sets. The generic OPP parser reads only opp-microvolt, so on such a chip the voltages would
 * not be the ones the SoC vendor qualified it at. Rockchip's helper builds the table with that selection, its
 * low-temperature adjustment and the "mali" supply, as Rockchip's own GPU driver does. Only on the RK3399 (Mali-T860),
 * where this is known to be what the vendor driver runs; other SoCs keep the generic table.
 */
static const struct of_device_id panfrost_rockchip_opp_match[] = {
	{},
};

static bool panfrost_devfreq_rockchip_opp(struct device *dev)
{
	struct device_node *np;
	bool ret;

	if (!of_machine_is_compatible("rockchip,rk3399"))
		return false;
	np = of_parse_phandle(dev->of_node, "operating-points-v2", 0);
	if (!np)
		return false;
	ret = of_property_present(np, "rockchip,pvtm-voltage-sel") ||
	      of_property_present(np, "rockchip,leakage-voltage-sel") ||
	      of_property_present(np, "rockchip,bin-voltage-sel");
	of_node_put(np);
	return ret;
}

static void panfrost_devfreq_rockchip_uninit(void *data)
{
	struct panfrost_device *pfdev = data;

	rockchip_uninit_opp_table(&pfdev->pdev->dev, &pfdev->pfdevfreq.rockchip_opp);
}

static int panfrost_devfreq_rockchip_init(struct panfrost_device *pfdev)
{
	struct device *dev = &pfdev->pdev->dev;
	int ret;

	rockchip_get_opp_data(panfrost_rockchip_opp_match, &pfdev->pfdevfreq.rockchip_opp);
	ret = rockchip_init_opp_table(dev, &pfdev->pfdevfreq.rockchip_opp, NULL, "mali");
	if (ret)
		return ret;
	/* before the devfreq device, so devm removes the devfreq device first and the table after it */
	return devm_add_action_or_reset(dev, panfrost_devfreq_rockchip_uninit, pfdev);
}
#else
static bool panfrost_devfreq_rockchip_opp(struct device *dev)
{
	return false;
}

static int panfrost_devfreq_rockchip_init(struct panfrost_device *pfdev)
{
	return -ENODEV;
}
#endif

int panfrost_devfreq_init(struct panfrost_device *pfdev)
{
	int ret;
	struct dev_pm_opp *opp;
	unsigned long cur_freq;
	struct device *dev = &pfdev->pdev->dev;
	struct devfreq *devfreq;
	struct thermal_cooling_device *cooling;
	struct panfrost_devfreq *pfdevfreq = &pfdev->pfdevfreq;
	unsigned long freq = ULONG_MAX;

	if (pfdev->comp->num_supplies > 1) {
		/*
		 * GPUs with more than 1 supply require platform-specific handling:
		 * continue without devfreq
		 */
		DRM_DEV_INFO(dev, "More than 1 supply is not supported yet\n");
		return 0;
	}

	ret = panfrost_read_speedbin(dev);
	if (ret)
		return ret;

	if (panfrost_devfreq_rockchip_opp(dev)) {
		ret = panfrost_devfreq_rockchip_init(pfdev);
		if (ret) {
			if (ret != -EPROBE_DEFER)
				DRM_DEV_ERROR(dev, "Couldn't set up the Rockchip OPP table (%d)\n", ret);
			return ret;
		}
		pfdevfreq->opp_of_table_added = true;
	} else {
		ret = devm_pm_opp_set_regulators(dev, pfdev->comp->supply_names);
		if (ret) {
			/* Continue if the optional regulator is missing */
			if (ret != -ENODEV) {
				if (ret != -EPROBE_DEFER)
					DRM_DEV_ERROR(dev, "Couldn't set OPP regulators\n");
				return ret;
			}
		}

		ret = devm_pm_opp_of_add_table(dev);
		if (ret) {
			/* Optional, continue without devfreq */
			if (ret == -ENODEV)
				ret = 0;
			return ret;
		}
		pfdevfreq->opp_of_table_added = true;
	}

	spin_lock_init(&pfdevfreq->lock);

	panfrost_devfreq_reset(pfdevfreq);

	cur_freq = clk_get_rate(pfdev->clock);

	opp = devfreq_recommended_opp(dev, &cur_freq, 0);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	panfrost_devfreq_profile.initial_freq = cur_freq;

	/*
	 * We could wait until panfrost_devfreq_target() to set this value, but
	 * since the simple_ondemand governor works asynchronously, there's a
	 * chance by the time someone opens the device's fdinfo file, current
	 * frequency hasn't been updated yet, so let's just do an early set.
	 */
	pfdevfreq->current_frequency = cur_freq;

	/*
	 * Set the recommend OPP this will enable and configure the regulator
	 * if any and will avoid a switch off by regulator_late_cleanup()
	 */
	ret = dev_pm_opp_set_opp(dev, opp);
	dev_pm_opp_put(opp);
	if (ret) {
		DRM_DEV_ERROR(dev, "Couldn't set recommended OPP\n");
		return ret;
	}

	/* Find the fastest defined rate  */
	opp = dev_pm_opp_find_freq_floor(dev, &freq);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	pfdevfreq->fast_rate = freq;

	dev_pm_opp_put(opp);

	/*
	 * Setup default thresholds for the simple_ondemand governor.
	 * The values are chosen based on experiments.
	 */
	pfdevfreq->gov_data.upthreshold = 45;
	pfdevfreq->gov_data.downdifferential = 5;

	devfreq = devm_devfreq_add_device(dev, &panfrost_devfreq_profile,
					  DEVFREQ_GOV_SIMPLE_ONDEMAND,
					  &pfdevfreq->gov_data);
	if (IS_ERR(devfreq)) {
		DRM_DEV_ERROR(dev, "Couldn't initialize GPU devfreq\n");
		return PTR_ERR(devfreq);
	}
	pfdevfreq->devfreq = devfreq;

	cooling = devfreq_cooling_em_register(devfreq, NULL);
	if (IS_ERR(cooling))
		DRM_DEV_INFO(dev, "Failed to register cooling device\n");
	else
		pfdevfreq->cooling = cooling;

	return 0;
}

void panfrost_devfreq_fini(struct panfrost_device *pfdev)
{
	struct panfrost_devfreq *pfdevfreq = &pfdev->pfdevfreq;

	if (pfdevfreq->cooling) {
		devfreq_cooling_unregister(pfdevfreq->cooling);
		pfdevfreq->cooling = NULL;
	}
}

void panfrost_devfreq_resume(struct panfrost_device *pfdev)
{
	struct panfrost_devfreq *pfdevfreq = &pfdev->pfdevfreq;

	if (!pfdevfreq->devfreq)
		return;

	panfrost_devfreq_reset(pfdevfreq);

	devfreq_resume_device(pfdevfreq->devfreq);
}

void panfrost_devfreq_suspend(struct panfrost_device *pfdev)
{
	struct panfrost_devfreq *pfdevfreq = &pfdev->pfdevfreq;

	if (!pfdevfreq->devfreq)
		return;

	devfreq_suspend_device(pfdevfreq->devfreq);
}

void panfrost_devfreq_record_busy(struct panfrost_devfreq *pfdevfreq)
{
	unsigned long irqflags;

	if (!pfdevfreq->devfreq)
		return;

	spin_lock_irqsave(&pfdevfreq->lock, irqflags);

	panfrost_devfreq_update_utilization(pfdevfreq);

	pfdevfreq->busy_count++;

	spin_unlock_irqrestore(&pfdevfreq->lock, irqflags);
}

void panfrost_devfreq_record_idle(struct panfrost_devfreq *pfdevfreq)
{
	unsigned long irqflags;

	if (!pfdevfreq->devfreq)
		return;

	spin_lock_irqsave(&pfdevfreq->lock, irqflags);

	panfrost_devfreq_update_utilization(pfdevfreq);

	WARN_ON(--pfdevfreq->busy_count < 0);

	spin_unlock_irqrestore(&pfdevfreq->lock, irqflags);
}
