/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _P4_BACKLIGHT_H
#define _P4_BACKLIGHT_H

#include "p4.h"

/*
 * Backlight subsystem - handles backlight device registration.
 */

/* Probe: register backlight device */
int p4_backlight_probe(struct p4_device *p4);

/* Remove: cleanup (handled by devm) */
void p4_backlight_remove(struct p4_device *p4);

#endif /* _P4_BACKLIGHT_H */
