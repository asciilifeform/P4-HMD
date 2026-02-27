/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _P4_VBLANK_H
#define _P4_VBLANK_H

#include <drm/drm_simple_kms_helper.h>
#include "p4.h"

/*
 * Vblank subsystem - software timer + hardware VSYNC sync.
 *
 * A 50Hz software timer provides vblank events regardless of display state.
 * Hardware VSYNC resets the timer to stay synchronized when available.
 */

/* Minimum interval between processed VSYNCs (prevents double-processing) */
#define VSYNC_MIN_INTERVAL_MS	10

/*
 * Backup timer period: 22ms, slightly slower than 50Hz (20ms).
 * This ensures the timer only fires when hardware VSYNC is truly missing,
 * preventing double-fires if the display's 50Hz clock drifts slightly.
 */
#define VBLANK_BACKUP_PERIOD_NS	(22 * NSEC_PER_MSEC)

/* Probe/remove */
int p4_vblank_probe(struct p4_device *p4);
void p4_vblank_remove(struct p4_device *p4);

/* DRM vblank callbacks */
int p4_vblank_enable(struct drm_simple_display_pipe *pipe);
void p4_vblank_disable(struct drm_simple_display_pipe *pipe);

/* CRTC lifecycle */
void p4_crtc_vblank_on(struct p4_device *p4);
void p4_crtc_vblank_off(struct p4_device *p4);

#endif /* _P4_VBLANK_H */
