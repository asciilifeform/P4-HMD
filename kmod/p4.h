/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _P4_H
#define _P4_H

#include <linux/atomic.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>

#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>

#include "display.h"
#include "rotation.h"
#include "update.h"

/*
 * Power modes for the display. These are mutually exclusive states
 * controlling standby, blank, and low_intensity flags.
 */
enum p4_power_mode {
	P4_POWER_OFF,		/* standby=1 (deepest sleep, FPGA clock stopped) */
	P4_POWER_BLANK,		/* blank=1 (screen off, fast resume) */
	P4_POWER_LOW,		/* low_intensity=1 (dim display) */
	P4_POWER_NORMAL,	/* all power bits clear (full power) */
};

/* Feature flags - can be overridden from Makefile via -DP4_DEBUG=N */
#ifndef P4_DEBUG
#define P4_DEBUG	1
#endif
#ifndef P4_DEBUG_VERBOSE
#define P4_DEBUG_VERBOSE 0  /* Set to 1 for per-vblank/per-transfer logging */
#endif

#if P4_DEBUG
#define DBG(fmt, ...) DRM_DEBUG_DRIVER(fmt, ##__VA_ARGS__)
#else
#define DBG(fmt, ...) do {} while (0)
#endif

/* Verbose debug - high-frequency events (vblank, transfers) */
#if P4_DEBUG && P4_DEBUG_VERBOSE
#define DBG_V(fmt, ...) DRM_DEBUG_DRIVER(fmt, ##__VA_ARGS__)
#else
#define DBG_V(fmt, ...) do {} while (0)
#endif

/* Forward declarations for subsystem state */
struct p4_sender;
struct p4_vblank;
struct p4_backlight;

/* Rotation modes - combinations of SW rotation and HW flip */
enum p4_rotation {
	P4_ROT_0 = 0,	/* Native 280x720, no SW rotation */
	P4_ROT_90 = 1,	/* 720x280, SW rotate 90° (default) */
	P4_ROT_180 = 2,	/* Native 280x720 + HW upside_down */
	P4_ROT_270 = 3,	/* 720x280, SW rotate 90° + HW upside_down */
};

struct p4_device {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct spi_device *spi;

	/* Subsystems */
	struct p4_sender *sender;
	struct p4_vblank *vblank;
	struct p4_backlight *backlight;

	/*
	 * Frame buffers (triple buffering for vblank-synchronized updates):
	 *   dst_buf[0] - p4_update() accumulates rotated data here
	 *   dst_buf[1] - encoder copies here then diffs/sends
	 *   dst_buf[2] - reference (last sent state)
	 *
	 * src_buf - temporary for mono conversion before rotation
	 */
	u8 *src_buf;
	u8 *dst_buf[3];

	/*
	 * Vblank-synchronized update state:
	 *   update_lock - protects dst_buf[0], dirty, and force_full flags
	 *   dirty - true when dst_buf[0] has new data since last send
	 *   force_full - true when next send should diff against zeroed reference
	 *
	 * On VSYNC, the drainer encodes and sends in one shot.
	 */
	struct mutex update_lock;
	bool dirty;
	bool force_full;

	/*
	 * Display state.
	 * display_connected: true when physical display is responding to SPI.
	 * Set by drainer thread based on READY signal behavior. Used by
	 * connector callbacks and CRTC atomic_check.
	 * target_power: brightness ceiling (NORMAL or LOW), applied when
	 * display powers up from OFF/BLANK state.
	 */
	bool display_connected;
	union display_flags_u display_flags;
	enum p4_power_mode target_power;
	enum p4_rotation rotation;
	bool is_native;		/* true for P4_ROT_0/P4_ROT_180 (no SW rotation) */
	bool user_flip;		/* user-controlled 180° flip, XORed with DRM rotation */
	int blank_state;	/* FB_BLANK_* state for DPMS */
	bool hexdump_spi;	/* dump full SPI transfers to dmesg */
};

#define drm_to_p4(d) container_of(d, struct p4_device, drm)

/*
 * Display flag setters. Updates the flag and wakes drainer to send
 * a flags-only packet if flags differ from last sent.
 */
void p4_set_upside_down(struct p4_device *p4, bool enable);

/*
 * Set display power mode. These are mutually exclusive states:
 *   P4_POWER_OFF    - deep sleep (standby=1, FPGA clock stopped)
 *   P4_POWER_BLANK  - screen off (blank=1, fast resume)
 *   P4_POWER_LOW    - dim display (low_intensity=1)
 *   P4_POWER_NORMAL - full power (all power bits clear)
 *
 * When transitioning to NORMAL, the actual mode applied respects
 * target_power (set by p4_set_brightness), so NORMAL may become LOW.
 * Preserves upside_down flag.
 */
void p4_set_power_mode(struct p4_device *p4, enum p4_power_mode mode);

/*
 * Set brightness (called by backlight subsystem).
 * Valid modes: P4_POWER_BLANK (0), P4_POWER_LOW (dim), P4_POWER_NORMAL (full).
 * If display is on, applies immediately and updates target_power.
 * If display is off (standby), this is a no-op - target_power is preserved
 * from before powerdown and restored on power-up.
 */
void p4_set_brightness(struct p4_device *p4, enum p4_power_mode brightness);

#endif /* _P4_H */
