/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _P4_DPMS_H
#define _P4_DPMS_H

#include "p4.h"

/*
 * DPMS subsystem - handles display power management states.
 *
 * Maps DRM DPMS modes to hardware-specific power states and sends
 * appropriate commands to the display controller.
 */

/* Called from connector dpms callback */
void p4_dpms_set(struct p4_device *p4, int drm_dpms_mode);

#endif /* _P4_DPMS_H */
