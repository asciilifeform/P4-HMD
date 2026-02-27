/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _P4_DRM_BOILERPLATE_H
#define _P4_DRM_BOILERPLATE_H

#include <drm/drm_atomic.h>

#include "dpms.h"

/* Forward declarations for callbacks defined in main */
static void p4_enable(struct drm_simple_display_pipe *pipe,
		      struct drm_crtc_state *crtc, struct drm_plane_state *plane);
static void p4_disable(struct drm_simple_display_pipe *pipe);
static void p4_update(struct drm_simple_display_pipe *pipe,
		      struct drm_plane_state *old);
static int p4_probe(struct spi_device *spi);
static void p4_remove(struct spi_device *spi);
static void p4_shutdown(struct spi_device *spi);

/* ===== Connector ===== */

/*
 * Connector detect - check if display is connected.
 * Called by DRM core when polling or on hotplug event.
 */
static enum drm_connector_status p4_connector_detect(struct drm_connector *conn,
						     bool force)
{
	struct p4_device *p4 = drm_to_p4(conn->dev);
	return p4->display_connected ?
		connector_status_connected : connector_status_disconnected;
}

static int p4_get_modes(struct drm_connector *conn)
{
	struct p4_device *p4 = drm_to_p4(conn->dev);
	struct drm_display_mode *m;

	/* Return no modes when display is disconnected */
	if (!p4->display_connected)
		return 0;

	/*
	 * Report a single 720x280 mode. Rotation is handled via the
	 * DRM rotation property - when set to 90°/270°, userspace will
	 * create a 280x720 framebuffer instead.
	 */
	m = drm_mode_create(conn->dev);
	if (!m)
		return 0;
	m->hdisplay = m->hsync_start = m->hsync_end = m->htotal = P4_WIDTH;
	m->vdisplay = m->vsync_start = m->vsync_end = m->vtotal = P4_HEIGHT;
	m->clock = P4_WIDTH * P4_HEIGHT * 50 / 1000;
	m->width_mm = 61;
	m->height_mm = 24;
	m->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(m);
	drm_mode_probed_add(conn, m);

	return 1;
}

static const struct drm_connector_helper_funcs conn_helper = {
	.get_modes = p4_get_modes
};

/*
 * DPMS callback - called by DRM core when power state changes.
 * Delegates to dpms subsystem for hardware-specific handling.
 */
static int p4_connector_dpms(struct drm_connector *conn, int mode)
{
	struct p4_device *p4 = drm_to_p4(conn->dev);
	p4_dpms_set(p4, mode);
	return 0;
}

static const struct drm_connector_funcs conn_funcs = {
	.detect = p4_connector_detect,
	.dpms = p4_connector_dpms,
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* ===== Pipe ===== */

static const struct drm_simple_display_pipe_funcs pipe_funcs = {
	.enable = p4_enable,
	.disable = p4_disable,
	.update = p4_update,
	.enable_vblank = p4_vblank_enable,
	.disable_vblank = p4_vblank_disable,
	DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS,
};

/* ===== Mode config ===== */

/*
 * Custom atomic check - reject enabling CRTC when display is disconnected.
 * Follows the DRM DP MST pattern: allow disabling (so X11 can clean up),
 * but reject enabling when display is not connected.
 */
static int p4_atomic_check(struct drm_device *dev, struct drm_atomic_state *state)
{
	struct p4_device *p4 = drm_to_p4(dev);
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	int i, ret;

	/* Run standard atomic checks first */
	ret = drm_atomic_helper_check(dev, state);
	if (ret)
		return ret;

	/* Check each CRTC being modified */
	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		/* Allow disabling even when disconnected */
		if (!crtc_state->active)
			continue;

		/* Reject enabling when display is disconnected */
		if (!p4->display_connected) {
			drm_dbg(dev, "Rejecting CRTC enable: display disconnected\n");
			return -ENODEV;
		}
	}

	return 0;
}

static const struct drm_mode_config_funcs mode_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = p4_atomic_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* ===== Driver ===== */

static const u32 p4_formats[] = { DRM_FORMAT_XRGB8888 };

DEFINE_DRM_GEM_DMA_FOPS(p4_fops);

static const struct drm_driver p4_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &p4_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	.name = "p4_display",
	.desc = "Rotated Mono Display",
	.date = "20241201",
	.major = 1,
	.minor = 0,
};

/* ===== SPI driver ===== */

static const struct of_device_id p4_of_match[] = {
	{ .compatible = "example,p4-display" },
	{ }
};
MODULE_DEVICE_TABLE(of, p4_of_match);

static DEFINE_SIMPLE_DEV_PM_OPS(p4_pm_ops, p4_suspend, p4_resume);

static struct spi_driver p4_spi_driver = {
	.driver = {
		.name = "p4-rotate",
		.of_match_table = p4_of_match,
		.pm = pm_sleep_ptr(&p4_pm_ops),
	},
	.probe = p4_probe,
	.remove = p4_remove,
	.shutdown = p4_shutdown,
};

#endif /* _P4_DRM_BOILERPLATE_H */
