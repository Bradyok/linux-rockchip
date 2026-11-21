// SPDX-License-Identifier: GPL-2.0+

#include "vkms_drv.h"
#include <drm/drm_atomic_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_probe_helper.h>

static const struct drm_connector_funcs vkms_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_encoder_funcs vkms_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

/*
 * NovaStar V-series: the seven fixed modes the vendor's vkms offers instead of the
 * drm_add_modes_noedid() list -- the struct drm_display_mode array at 0xffffff80094c2d50
 * (stride 0xf0) that vkms_add_modes walks. Values as stored there: name, type 0x40
 * (DRIVER), clock, h/v timings, flags (5 = +hsync +vsync, 0xa = -hsync -vsync) and
 * picture_aspect_ratio (2 = 16:9, 1 = 4:3, 0 = none). Entry 1 is the vendor's own: named
 * 1920x1080 with the 1080p60 timing but a 24750 kHz clock (its 4.19 vrefresh field says
 * 10), so the simulated vblank -- and with it every page flip -- runs at 10 Hz.
 */
static const struct drm_display_mode vkms_vendor_modes[] = {
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 594000, 3840, 4016, 4104, 4400, 0,
		   2160, 2168, 2178, 2250, 0, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = HDMI_PICTURE_ASPECT_16_9 },
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 24750, 1920, 2008, 2052, 2200, 0,
		   1080, 1084, 1089, 1125, 0, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = HDMI_PICTURE_ASPECT_16_9 },
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 1390, 1430, 1650, 0,
		   720, 725, 730, 750, 0, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = HDMI_PICTURE_ASPECT_16_9 },
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 1720, 1760, 1980, 0,
		   720, 725, 730, 750, 0, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = HDMI_PICTURE_ASPECT_16_9 },
	{ DRM_MODE("1024x768", DRM_MODE_TYPE_DRIVER, 65000, 1024, 1048, 1184, 1344, 0,
		   768, 771, 777, 806, 0, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	{ DRM_MODE("720x576", DRM_MODE_TYPE_DRIVER, 27000, 720, 732, 796, 864, 0,
		   576, 581, 586, 625, 0, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = HDMI_PICTURE_ASPECT_4_3 },
	{ DRM_MODE("720x480", DRM_MODE_TYPE_DRIVER, 27000, 720, 736, 798, 858, 0,
		   480, 489, 495, 525, 0, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = HDMI_PICTURE_ASPECT_4_3 },
};

/*
 * vkms_add_modes @ 0xffffff800862b0b0: drm_mode_duplicate() each of the seven entries
 * (loop count 7, +0x18) and drm_mode_probed_add() the ones that duplicated; returns how
 * many were added.
 */
int vkms_add_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	int count = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(vkms_vendor_modes); i++) {
		mode = drm_mode_duplicate(connector->dev, &vkms_vendor_modes[i]);
		if (!mode)
			continue;
		count++;
		drm_mode_probed_add(connector, mode);
	}

	return count;
}

/*
 * vkms_conn_get_modes @ 0xffffff800862b11c: the vendor's fixed list, with 1920x1080
 * preferred (drm_set_preferred_mode(connector, 0x780, 0x438), +0x20..+0x30).
 */
static int vkms_conn_get_modes(struct drm_connector *connector)
{
	int count;

	count = vkms_add_modes(connector);
	drm_set_preferred_mode(connector, 1920, 1080);

	return count;
}

static const struct drm_connector_helper_funcs vkms_conn_helper_funcs = {
	.get_modes    = vkms_conn_get_modes,
};

static int vkms_add_overlay_plane(struct vkms_device *vkmsdev, int index,
				  struct drm_crtc *crtc)
{
	struct vkms_plane *overlay;

	overlay = vkms_plane_init(vkmsdev, DRM_PLANE_TYPE_OVERLAY, index);
	if (IS_ERR(overlay))
		return PTR_ERR(overlay);

	if (!overlay->base.possible_crtcs)
		overlay->base.possible_crtcs = drm_crtc_mask(crtc);

	return 0;
}

int vkms_output_init(struct vkms_device *vkmsdev, int index)
{
	struct vkms_output *output = &vkmsdev->output;
	struct drm_device *dev = &vkmsdev->drm;
	struct drm_connector *connector = &output->connector;
	struct drm_encoder *encoder = &output->encoder;
	struct drm_crtc *crtc = &output->crtc;
	struct vkms_plane *primary, *cursor = NULL;
	int ret;
	int writeback;
	unsigned int n;

	primary = vkms_plane_init(vkmsdev, DRM_PLANE_TYPE_PRIMARY, index);
	if (IS_ERR(primary))
		return PTR_ERR(primary);

	if (vkmsdev->config->overlay) {
		for (n = 0; n < NUM_OVERLAY_PLANES; n++) {
			ret = vkms_add_overlay_plane(vkmsdev, index, crtc);
			if (ret)
				return ret;
		}
	}

	if (vkmsdev->config->cursor) {
		cursor = vkms_plane_init(vkmsdev, DRM_PLANE_TYPE_CURSOR, index);
		if (IS_ERR(cursor))
			return PTR_ERR(cursor);
	}

	ret = vkms_crtc_init(dev, crtc, &primary->base, &cursor->base);
	if (ret)
		return ret;

	ret = drm_connector_init(dev, connector, &vkms_connector_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret) {
		DRM_ERROR("Failed to init connector\n");
		return ret;
	}

	drm_connector_helper_add(connector, &vkms_conn_helper_funcs);

	ret = drm_encoder_init(dev, encoder, &vkms_encoder_funcs,
			       DRM_MODE_ENCODER_VIRTUAL, NULL);
	if (ret) {
		DRM_ERROR("Failed to init encoder\n");
		goto err_encoder;
	}
	encoder->possible_crtcs = 1;

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret) {
		DRM_ERROR("Failed to attach connector to encoder\n");
		goto err_attach;
	}

	if (vkmsdev->config->writeback) {
		writeback = vkms_enable_writeback_connector(vkmsdev);
		if (writeback)
			DRM_ERROR("Failed to init writeback connector\n");
	}

	drm_mode_config_reset(dev);

	/*
	 * NovaStar V-series, vkms_output_init @ 0xffffff800862b160 +0x16c..+0x18c: after the mode
	 * config reset, bring up /dev/vkms_misc; a failure is logged and the output still
	 * initialises (w19 = 0, +0x180).
	 */
	if (vkms_misc_init(&output->vkms_misc))
		DRM_ERROR("[zmh]Failed to init vkms misc\n");

	return 0;

err_attach:
	drm_encoder_cleanup(encoder);

err_encoder:
	drm_connector_cleanup(connector);

	return ret;
}
