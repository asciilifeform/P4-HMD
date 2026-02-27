/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _P4_SLEEP_H
#define _P4_SLEEP_H

struct device;

/* Suspend the device - PM callback compatible */
int p4_suspend(struct device *dev);

/* Resume the device - PM callback compatible */
int p4_resume(struct device *dev);

#endif /* _P4_SLEEP_H */
