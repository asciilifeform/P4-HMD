// SPDX-License-Identifier: GPL-2.0
/*
 * P4 display driver - DPMS subsystem
 * Handles display power management states.
 */

#include <linux/fb.h>
#include <drm/drm_modes.h>

#include "p4.h"
#include "dpms.h"
#include "sender.h"

/*
 * Map DRM DPMS mode to FB_BLANK state.
 * 
 * DRM_MODE_DPMS_ON      -> FB_BLANK_UNBLANK       (full power)
 * DRM_MODE_DPMS_STANDBY -> FB_BLANK_NORMAL        (screen off, vsync on)
 * DRM_MODE_DPMS_SUSPEND -> FB_BLANK_VSYNC_SUSPEND (vsync off)
 * DRM_MODE_DPMS_OFF     -> FB_BLANK_POWERDOWN     (deep sleep)
 */
static int dpms_to_blank(int drm_dpms_mode)
{
	switch (drm_dpms_mode) {
	case DRM_MODE_DPMS_ON:
		return FB_BLANK_UNBLANK;
	case DRM_MODE_DPMS_STANDBY:
		return FB_BLANK_NORMAL;
	case DRM_MODE_DPMS_SUSPEND:
		return FB_BLANK_VSYNC_SUSPEND;
	case DRM_MODE_DPMS_OFF:
	default:
		return FB_BLANK_POWERDOWN;
	}
}

void p4_dpms_set(struct p4_device *p4, int drm_dpms_mode)
{
	int blank_state = dpms_to_blank(drm_dpms_mode);

	if (blank_state == p4->blank_state)
		return;

	DBG("dpms: blank %d -> %d\n", p4->blank_state, blank_state);

	switch (blank_state) {
	case FB_BLANK_UNBLANK:
		/* Full power */
		p4_sender_powerup(p4);
		p4_set_power_mode(p4, P4_POWER_NORMAL);
		DBG("dpms: unblank\n");
		break;

	case FB_BLANK_NORMAL:
		/* Screen off (fast resume) */
		p4_set_power_mode(p4, P4_POWER_BLANK);
		DBG("dpms: standby\n");
		break;

	case FB_BLANK_VSYNC_SUSPEND:
	case FB_BLANK_HSYNC_SUSPEND:
		/* Deep sleep */
		p4_set_power_mode(p4, P4_POWER_OFF);
		DBG("dpms: suspend\n");
		break;

	case FB_BLANK_POWERDOWN:
		/* Full power down */
		p4_set_power_mode(p4, P4_POWER_OFF);
		p4_sender_powerdown(p4);
		DBG("dpms: off\n");
		break;
	}

	p4->blank_state = blank_state;
}
