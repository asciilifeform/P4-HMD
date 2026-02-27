// SPDX-License-Identifier: GPL-2.0
/*
 * P4 display driver - sleep subsystem
 * Coordinates suspend/resume across subsystems.
 */

#include <linux/device.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_mode_config.h>

#include "p4.h"
#include "sleep.h"
#include "sender.h"

int p4_suspend(struct device *dev)
{
	struct p4_device *p4 = dev_get_drvdata(dev);
	int ret;

	DBG("suspend\n");

	/* Set standby flag - display should enter deep sleep */
	p4_set_power_mode(p4, P4_POWER_OFF);

	/* DRM suspend disables CRTC, which calls p4_crtc_vblank_off */
	ret = drm_mode_config_helper_suspend(&p4->drm);
	if (ret) {
		dev_err(dev, "DRM suspend failed: %d\n", ret);
		p4_set_power_mode(p4, P4_POWER_NORMAL);
		return ret;
	}

	p4_sender_suspend(p4);

	return 0;
}

int p4_resume(struct device *dev)
{
	struct p4_device *p4 = dev_get_drvdata(dev);
	int ret;

	DBG("resume\n");

	ret = p4_sender_resume(p4);
	if (ret) {
		dev_err(dev, "sender resume failed: %d\n", ret);
		return ret;
	}

	/* Clear standby - display wakes to target brightness */
	p4_set_power_mode(p4, P4_POWER_NORMAL);

	/* DRM resume re-enables CRTC, which calls p4_crtc_vblank_on */
	ret = drm_mode_config_helper_resume(&p4->drm);
	if (ret)
		dev_err(dev, "DRM resume failed: %d\n", ret);

	return ret;
}
