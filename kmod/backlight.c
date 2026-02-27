// SPDX-License-Identifier: GPL-2.0
/*
 * P4 display driver - backlight subsystem
 */

#include <linux/backlight.h>

#include "backlight.h"

struct p4_backlight {
	struct p4_device *p4;
	struct backlight_device *bl_dev;
};

static int bl_update(struct backlight_device *bl)
{
	struct p4_backlight *b = bl_get_data(bl);
	struct p4_device *p4 = b->p4;
	int brightness = backlight_get_brightness(bl);

	DBG("backlight: %d -> %d\n", bl->props.brightness, brightness);

	/* brightness 0=blank, 1=low, 2=full */
	static const enum p4_power_mode map[] = {
		P4_POWER_BLANK,		/* 0 */
		P4_POWER_LOW,		/* 1 */
		P4_POWER_NORMAL,	/* 2 */
	};

	if (brightness <= 2)
		p4_set_brightness(p4, map[brightness]);

	return 0;
}

static const struct backlight_ops bl_ops = {
	.update_status = bl_update,
};

int p4_backlight_probe(struct p4_device *p4)
{
	struct device *dev = &p4->spi->dev;
	struct p4_backlight *b;

	DBG("backlight probe\n");
	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b) {
		dev_err(dev, "failed to allocate backlight state\n");
		return -ENOMEM;
	}

	b->p4 = p4;
	p4->backlight = b;

	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = 2,
		.brightness = 2,	/* default: full brightness */
	};

	b->bl_dev = devm_backlight_device_register(dev, "p4-backlight",
						   dev, b, &bl_ops, &props);
	if (IS_ERR(b->bl_dev)) {
		dev_err(dev, "failed to register backlight: %ld\n", PTR_ERR(b->bl_dev));
		return PTR_ERR(b->bl_dev);
	}

	DBG("backlight registered\n");
	return 0;
}

void p4_backlight_remove(struct p4_device *p4)
{
	DBG("backlight remove\n");
	/* Handled by devm */
}
