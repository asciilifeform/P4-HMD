// SPDX-License-Identifier: GPL-2.0
/*
 * DRM driver for P4 rotated monochrome SPI display
 * Display: 720x280 or 280x720 (userspace) -> 280x720 (hardware)
 */

#include <linux/crc32.h>
#include <linux/fb.h>
#include <linux/of.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "p4.h"
#include "sender.h"
#include "vblank.h"
#include "backlight.h"
#include "sleep.h"
#include "drm_boilerplate.h"

/* Module parameter for full SPI hex dump */
static bool hexdump;
module_param(hexdump, bool, 0644);
MODULE_PARM_DESC(hexdump, "Dump full SPI transfers to dmesg (default: 0)");

/*
 * Process a damage clip in ROTATED mode (720x280 userspace).
 * Convert XRGB8888 to mono, then rotate 90° to destination.
 */
static void p4_process_clip_rotated(struct p4_device *p4,
				    const struct iosys_map *src_map,
				    struct drm_framebuffer *fb,
				    const struct drm_rect *clip,
				    struct drm_format_conv_state *fmtcnv,
				    int dst_idx)
{
	struct iosys_map dst_map;
	unsigned int fb_width = fb->width;
	unsigned int fb_height = fb->height;

	/*
	 * Align clip to 8-pixel boundaries for rotation.
	 * The rotation works on 8x8 blocks, so we need the mono conversion
	 * to include complete blocks.
	 */
	struct drm_rect aligned = {
		.x1 = clip->x1 & ~7,
		.y1 = clip->y1 & ~7,
		.x2 = (clip->x2 + 7) & ~7,
		.y2 = (clip->y2 + 7) & ~7,
	};
	/* Clamp to frame bounds */
	if (aligned.x2 > fb_width)
		aligned.x2 = fb_width;
	if (aligned.y2 > fb_height)
		aligned.y2 = fb_height;

	int bx1 = aligned.x1 >> 3;
	int bx2 = aligned.x2 >> 3;
	int by1 = aligned.y1 >> 3;
	int by2 = aligned.y2 >> 3;
	int clip_width_blocks = bx2 - bx1;

	/* Sanity check: blocks should be non-empty */
	if (bx1 >= bx2 || by1 >= by2)
		return;

	iosys_map_set_vaddr(&dst_map, p4->src_buf);

	/*
	 * Convert to mono (into src_buf) using aligned clip.
	 *
	 * IMPORTANT: drm_fb_xrgb8888_to_mono() ALWAYS writes starting at
	 * dst[0].vaddr (offset 0), regardless of clip position. The clip
	 * only controls which source pixels are read. Output is written
	 * as a compact block: clip_width_blocks bytes per line, with lines
	 * contiguous (when dst_pitch=NULL) or spaced by dst_pitch.
	 *
	 * For rotation to work correctly, we need the mono data laid out
	 * with the clip's stride (clip_width_blocks bytes per row).
	 * Pass NULL for pitch to get tightly packed output.
	 */
	drm_fb_xrgb8888_to_mono(&dst_map, NULL, src_map, fb, &aligned, fmtcnv);

	/*
	 * Rotate from src_buf to dst_buf.
	 *
	 * The mono data is at src_buf[0] with stride = clip_width_blocks.
	 * We need to rotate this data to the correct position in dst_buf.
	 *
	 * We pre-offset the source pointer so rotation reads from the right place:
	 *   src_offset = src_buf - (by1 * 8 * clip_width_blocks) - bx1
	 */
	unsigned int src_stride = clip_width_blocks;
	const u8 *src_offset = p4->src_buf - (by1 * 8 * src_stride) - bx1;

	rotate_blocks(src_offset, p4->dst_buf[dst_idx],
		      src_stride, fb_height >> 3, fb_width >> 3,
		      bx1, bx2, by1, by2);
}

/*
 * Process a damage clip in NATIVE mode (280x720 userspace).
 * Convert XRGB8888 to mono directly - no rotation needed.
 */
static void p4_process_clip_native(struct p4_device *p4,
				   const struct iosys_map *src_map,
				   struct drm_framebuffer *fb,
				   const struct drm_rect *clip,
				   struct drm_format_conv_state *fmtcnv,
				   int dst_idx)
{
	struct iosys_map dst_map;
	unsigned int fb_width = fb->width;
	unsigned int fb_height = fb->height;
	unsigned int dst_pitch = HW_WIDTH / 8;  /* 35 bytes */

	/*
	 * Align x to 8-pixel boundaries (byte alignment for mono).
	 * y doesn't need alignment in native mode.
	 */
	struct drm_rect aligned = {
		.x1 = clip->x1 & ~7,
		.y1 = clip->y1,
		.x2 = (clip->x2 + 7) & ~7,
		.y2 = clip->y2,
	};
	/* Clamp to frame bounds */
	if (aligned.x2 > fb_width)
		aligned.x2 = fb_width;
	if (aligned.y2 > fb_height)
		aligned.y2 = fb_height;

	/*
	 * In native mode, we write directly to dst_buf.
	 * Hardware layout: 280 pixels wide = 35 bytes per row, 720 rows.
	 *
	 * drm_fb_xrgb8888_to_mono() writes to dst starting at offset
	 * (clip.y1 * dst_pitch + clip.x1/8) when dst_pitch is provided.
	 * So we pass the base of dst_buf and the hardware pitch.
	 */
	iosys_map_set_vaddr(&dst_map, p4->dst_buf[dst_idx]);

	drm_fb_xrgb8888_to_mono(&dst_map, &dst_pitch, src_map, fb, &aligned, fmtcnv);

	/*
	 * The kernel's drm_fb_xrgb8888_to_mono() outputs LSB-first bit ordering
	 * (bit 0 = leftmost pixel), but hardware expects MSB-first (bit 7 =
	 * leftmost). We set the bitrev flag in cmd_byte so FPGA reverses bits
	 * in payload bytes - no software conversion needed here.
	 */
}

/*
 * Process a damage clip using the current rotation mode.
 */
static void p4_process_clip(struct p4_device *p4, const struct iosys_map *src_map,
			    struct drm_framebuffer *fb, const struct drm_rect *clip,
			    struct drm_format_conv_state *fmtcnv, int dst_idx)
{
	if (p4->is_native)
		p4_process_clip_native(p4, src_map, fb, clip, fmtcnv, dst_idx);
	else
		p4_process_clip_rotated(p4, src_map, fb, clip, fmtcnv, dst_idx);
}

/* ===== DRM callbacks ===== */

static void p4_enable(struct drm_simple_display_pipe *pipe,
		      struct drm_crtc_state *crtc, struct drm_plane_state *plane)
{
	struct p4_device *p4 = drm_to_p4(pipe->crtc.dev);

	p4_sender_enable(p4);
	mutex_lock(&p4->update_lock);
	p4->force_full = true;
	mutex_unlock(&p4->update_lock);
	p4_crtc_vblank_on(p4);
}

static void p4_disable(struct drm_simple_display_pipe *pipe)
{
	struct p4_device *p4 = drm_to_p4(pipe->crtc.dev);
	DBG("disable\n");

	p4_crtc_vblank_off(p4);
	/* Ensure any pending vblank work completes before shutdown */
	/* Note: screen blanking handled by dpms callback before we get here */
	p4_sender_disable(p4);
}

static void p4_update(struct drm_simple_display_pipe *pipe,
		      struct drm_plane_state *old)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_crtc *crtc = &pipe->crtc;
	struct p4_device *p4 = drm_to_p4(pipe->crtc.dev);
	struct drm_format_conv_state fmtcnv;
	struct iosys_map src_map;
	int idx;

	/* Handle vblank event for atomic commit completion */
	if (crtc->state->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		if (drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, crtc->state->event);
		else
			drm_crtc_send_vblank_event(crtc, crtc->state->event);
		spin_unlock_irq(&crtc->dev->event_lock);
		crtc->state->event = NULL;
	}

	if (!p4_sender_is_enabled(p4) || !drm_dev_enter(&p4->drm, &idx))
		return;

	struct drm_gem_dma_object *dma_obj = drm_fb_dma_get_gem_obj(state->fb, 0);
	if (!dma_obj || !dma_obj->vaddr)
		goto out;

	/*
	 * Map DRM rotation property to our internal rotation enum.
	 * DRM_MODE_ROTATE_0/180: userspace is 720x280, we rotate 90° in SW
	 * DRM_MODE_ROTATE_90/270: userspace is 280x720, no SW rotation needed
	 *
	 * The 180° variants also set the hardware upside_down flag.
	 */
	enum p4_rotation new_rotation;
	bool hw_flip;

	switch (state->rotation & DRM_MODE_ROTATE_MASK) {
	case DRM_MODE_ROTATE_0:
		new_rotation = P4_ROT_90;  /* 720x280 -> rotate 90° -> 280x720 */
		hw_flip = false;
		break;
	case DRM_MODE_ROTATE_90:
		new_rotation = P4_ROT_0;   /* 280x720 -> no rotation -> 280x720 */
		hw_flip = false;
		break;
	case DRM_MODE_ROTATE_180:
		new_rotation = P4_ROT_270; /* 720x280 -> rotate 90° + flip -> 280x720 */
		hw_flip = true;
		break;
	case DRM_MODE_ROTATE_270:
		new_rotation = P4_ROT_180; /* 280x720 -> flip only -> 280x720 */
		hw_flip = true;
		break;
	default:
		new_rotation = P4_ROT_90;
		hw_flip = false;
		break;
	}

	/* Rotation change requires full refresh */
	bool rotation_changed = (p4->rotation != new_rotation);
	if (rotation_changed) {
		p4->rotation = new_rotation;
		p4->is_native = (new_rotation == P4_ROT_0 || new_rotation == P4_ROT_180);
	}

	/* New framebuffer requires full refresh */
	bool new_fb = !old->fb || old->fb != state->fb;

	/*
	 * Update hardware flip flag. XOR the DRM-requested flip with the
	 * user_flip setting so user can flip independently of X11/Wayland.
	 */
	p4_set_upside_down(p4, hw_flip ^ p4->user_flip);

	drm_format_conv_state_init(&fmtcnv);
	iosys_map_set_vaddr(&src_map, dma_obj->vaddr);

	/*
	 * Determine if we need full refresh.
	 * Rotation change, new FB, or force_full flag already set.
	 */
	mutex_lock(&p4->update_lock);

	bool full_refresh = p4->force_full || rotation_changed || new_fb;

	/*
	 * Both paths accumulate into dst_buf[0] and set dirty.
	 * Full refresh also sets force_full so drainer zeroes
	 * the reference buffer before diffing.
	 */
	if (full_refresh) {
		/* Use framebuffer dimensions */
		struct drm_rect full = { 0, 0, state->fb->width, state->fb->height };
		/* Clear accumulation buffer before processing full frame */
		memset(p4->dst_buf[0], 0, FB_SIZE);
		p4_process_clip(p4, &src_map, state->fb, &full, &fmtcnv, 0);
		p4->force_full = true;
	} else {
		struct drm_atomic_helper_damage_iter iter;
		struct drm_rect clip;

		drm_atomic_helper_damage_iter_init(&iter, old, state);
		while (drm_atomic_helper_damage_iter_next(&iter, &clip))
			p4_process_clip(p4, &src_map, state->fb, &clip, &fmtcnv, 0);
	}

	p4->dirty = true;

	mutex_unlock(&p4->update_lock);

	drm_format_conv_state_release(&fmtcnv);
out:
	drm_dev_exit(idx);
}

/* ===== Sysfs ===== */

static ssize_t user_flip_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	struct p4_device *p4 = spi_get_drvdata(spi);

	return sysfs_emit(buf, "%d\n", p4->user_flip ? 1 : 0);
}

static ssize_t user_flip_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	struct p4_device *p4 = spi_get_drvdata(spi);
	bool new_flip;
	int ret;

	ret = kstrtobool(buf, &new_flip);
	if (ret)
		return ret;

	if (p4->user_flip != new_flip) {
		p4->user_flip = new_flip;
		/*
		 * Update hardware flip state. Current DRM flip state is
		 * determined by rotation: 180°/270° modes set hw_flip=true.
		 */
		bool drm_flip = (p4->rotation == P4_ROT_180 ||
				 p4->rotation == P4_ROT_270);
		p4_set_upside_down(p4, drm_flip ^ new_flip);
	}

	return count;
}

static DEVICE_ATTR_RW(user_flip);

static struct attribute *p4_sysfs_attrs[] = {
	&dev_attr_user_flip.attr,
	NULL,
};

static const struct attribute_group p4_sysfs_group = {
	.attrs = p4_sysfs_attrs,
};

/* ===== Probe/Remove ===== */

static int p4_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct p4_device *p4;
	int ret;

	DBG("probe: SPI %s @ %u Hz\n", dev_name(dev), spi->max_speed_hz);

	/*
	 * SPI child devices don't inherit DMA masks from their parent.
	 * Set up a 32-bit DMA mask so DRM GEM can allocate framebuffers.
	 * This follows the pattern used by repaper.c and other SPI DRM drivers.
	 */
	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_warn(dev, "Failed to set DMA mask: %d\n", ret);
			return ret;
		}
	}

	/* Ensure SPI mode 0 (CPOL=0, CPHA=0) and 8-bit words */
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "SPI setup failed\n");

	/* Allocate main device struct */
	p4 = devm_drm_dev_alloc(dev, &p4_drm_driver, struct p4_device, drm);
	if (IS_ERR(p4)) {
		dev_err(dev, "failed to allocate DRM device\n");
		return PTR_ERR(p4);
	}

	p4->spi = spi;
	spi_set_drvdata(spi, p4);
	p4->blank_state = FB_BLANK_UNBLANK;
	p4->target_power = P4_POWER_NORMAL;
	p4->rotation = P4_ROT_90;  /* Default: 720x280 rotated mode */
	p4->is_native = false;     /* P4_ROT_90 requires SW rotation */
	p4->user_flip = device_property_read_bool(dev, "upside-down");
	p4->hexdump_spi = hexdump; /* From module parameter */
	mutex_init(&p4->update_lock);

	/* Create sysfs attributes */
	ret = devm_device_add_group(dev, &p4_sysfs_group);
	if (ret) {
		dev_err(dev, "failed to create sysfs group: %d\n", ret);
		return ret;
	}

	/*
	 * Allocate framebuffers (triple buffering):
	 *   src_buf    - mono conversion temporary
	 *   dst_buf[0] - accumulation buffer (p4_update writes here)
	 *   dst_buf[1] - stable buffer (vblank work copies here, then diffs)
	 *   dst_buf[2] - reference buffer (last sent state)
	 *
	 * These buffers are CPU-only (no DMA), so use normal cached memory.
	 */
	p4->src_buf = devm_kzalloc(dev, FB_SIZE, GFP_KERNEL);
	p4->dst_buf[0] = devm_kzalloc(dev, FB_SIZE, GFP_KERNEL);
	p4->dst_buf[1] = devm_kzalloc(dev, FB_SIZE, GFP_KERNEL);
	p4->dst_buf[2] = devm_kzalloc(dev, FB_SIZE, GFP_KERNEL);
	if (!p4->src_buf || !p4->dst_buf[0] || !p4->dst_buf[1] || !p4->dst_buf[2]) {
		dev_err(dev, "failed to allocate framebuffers\n");
		return -ENOMEM;
	}
	/* devm_kzalloc already zeroes memory */
	DBG("framebuffers allocated: %u bytes each (x4)\n", FB_SIZE);

	ret = p4_sender_probe(p4);
	if (ret) {
		dev_err(dev, "sender probe failed: %d\n", ret);
		return ret;
	}

	ret = p4_vblank_probe(p4);
	if (ret) {
		dev_err(dev, "vblank probe failed: %d\n", ret);
		goto err_sender;
	}

	ret = p4_backlight_probe(p4);
	if (ret) {
		dev_err(dev, "backlight probe failed: %d\n", ret);
		goto err_vblank;
	}

	ret = drmm_mode_config_init(&p4->drm);
	if (ret) {
		dev_err(dev, "mode config init failed: %d\n", ret);
		goto err_backlight;
	}

	p4->drm.mode_config.funcs = &mode_funcs;
	/*
	 * Allow both 720x280 and 280x720 framebuffers.
	 * xrandr rotation creates a rotated framebuffer when rotation
	 * is set to 90° or 270°.
	 */
	p4->drm.mode_config.min_width = P4_HEIGHT;
	p4->drm.mode_config.max_width = P4_WIDTH;
	p4->drm.mode_config.min_height = P4_HEIGHT;
	p4->drm.mode_config.max_height = P4_WIDTH;

	drm_connector_helper_add(&p4->connector, &conn_helper);
	ret = drm_connector_init(&p4->drm, &p4->connector, &conn_funcs,
				 DRM_MODE_CONNECTOR_SPI);
	if (ret) {
		dev_err(dev, "connector init failed: %d\n", ret);
		goto err_backlight;
	}
	/* Enable hotplug detection - we signal events from drainer thread */
	p4->connector.polled = DRM_CONNECTOR_POLL_HPD;

	ret = drm_simple_display_pipe_init(&p4->drm, &p4->pipe, &pipe_funcs,
					   p4_formats, ARRAY_SIZE(p4_formats),
					   NULL, &p4->connector);
	if (ret) {
		dev_err(dev, "pipe init failed: %d\n", ret);
		goto err_backlight;
	}

	drm_plane_enable_fb_damage_clips(&p4->pipe.plane);
	drm_plane_create_rotation_property(&p4->pipe.plane,
					   DRM_MODE_ROTATE_0,
					   DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_90 |
					   DRM_MODE_ROTATE_180 | DRM_MODE_ROTATE_270);
	drm_mode_config_reset(&p4->drm);

	ret = drm_vblank_init(&p4->drm, 1);
	if (ret) {
		dev_err(dev, "vblank init failed: %d\n", ret);
		goto err_backlight;
	}

	ret = drm_dev_register(&p4->drm, 0);
	if (ret) {
		dev_err(dev, "DRM register failed: %d\n", ret);
		goto err_backlight;
	}

	/*
	 * Apply initial user_flip from device tree. At this point rotation
	 * is P4_ROT_90 (no hw_flip), so effective flip = user_flip.
	 */
	if (p4->user_flip)
		p4_set_upside_down(p4, true);

	/*
	 * Set up fbdev emulation. Newer kernels (6.12+) prefer drm_client_setup()
	 * with DRM_FBDEV_DMA_DRIVER_OPS, but drm_fbdev_dma_setup() still works.
	 */
	drm_fbdev_dma_setup(&p4->drm, 32);

	dev_info(dev, "registered /dev/dri/card%d (%ux%u @ %u Hz SPI)%s\n",
		 p4->drm.primary->index, P4_WIDTH, P4_HEIGHT,
		 spi->max_speed_hz, p4->user_flip ? " [flipped]" : "");
	return 0;

err_backlight:
	p4_backlight_remove(p4);
err_vblank:
	p4_vblank_remove(p4);
err_sender:
	p4_sender_remove(p4);
	return ret;
}

static void p4_remove(struct spi_device *spi)
{
	struct p4_device *p4 = spi_get_drvdata(spi);

	/*
	 * Unplug DRM first - this prevents new operations from starting
	 * and marks the device as gone.
	 */
	if (!drm_dev_is_unplugged(&p4->drm))
		drm_dev_unplug(&p4->drm);

	/*
	 * Shutdown DRM while vblank and sender are still functional.
	 * This allows drm_atomic_helper_shutdown() to complete its
	 * final commit without timing out waiting for vblank.
	 */
	drm_atomic_helper_shutdown(&p4->drm);

	/* Now safe to turn off vblank */
	drm_crtc_vblank_off(&p4->pipe.crtc);

	/*
	 * Stop sender/drainer after DRM shutdown is complete.
	 * The drainer thread might call drm_kms_helper_hotplug_event(),
	 * which would crash if DRM is being torn down.
	 */
	p4_sender_remove(p4);

	p4_backlight_remove(p4);
	p4_vblank_remove(p4);
}

static void p4_shutdown(struct spi_device *spi)
{
	struct p4_device *p4 = spi_get_drvdata(spi);

	drm_atomic_helper_shutdown(&p4->drm);
	p4_backlight_remove(p4);
	p4_vblank_remove(p4);
	p4_sender_remove(p4);
}

static int __init p4_module_init(void)
{
	int ret;

	pr_info("p4-rotate: module loading\n");
	ret = spi_register_driver(&p4_spi_driver);
	if (ret)
		pr_err("p4-rotate: spi_register_driver failed: %d\n", ret);
	else
		pr_info("p4-rotate: driver registered, waiting for device match\n");
	return ret;
}

static void __exit p4_module_exit(void)
{
	pr_info("p4-rotate: module unloading\n");
	spi_unregister_driver(&p4_spi_driver);
}

module_init(p4_module_init);
module_exit(p4_module_exit);

MODULE_DESCRIPTION("DRM driver for P4 rotated monochrome SPI display");
MODULE_LICENSE("GPL");
