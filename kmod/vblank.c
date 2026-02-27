// SPDX-License-Identifier: GPL-2.0
/*
 * P4 display driver - vblank subsystem
 *
 * Hardware VSYNC from the FPGA is the authoritative vblank source. It triggers
 * both DRM vblank accounting and frame transmission via drainer wakeup.
 *
 * A backup software timer keeps the drainer alive when hardware VSYNC is
 * absent (display in standby/blank, disconnected, etc). This allows the
 * drainer to send flags-only packets to wake the display from power-saving
 * modes.
 *
 * Timer design:
 * - Runs at 22ms (slightly slower than 20ms hardware period)
 * - Only fires if hardware VSYNC hasn't arrived
 * - Prevents double-firing from clock drift
 * - Skipped if hardware VSYNC arrived since last timer
 *
 * Sleep/wake:
 * - drm_crtc_vblank_off() during suspend calls our disable_vblank callback
 * - drm_crtc_vblank_on() during resume calls our enable_vblank callback
 * - Timer and IRQ are properly stopped/started through these callbacks
 */

#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <drm/drm_vblank.h>

#include "vblank.h"
#include "sender.h"

struct p4_vblank {
	struct p4_device *p4;
	int vsync_irq;

	/* Backup timer for when hardware VSYNC is absent */
	struct hrtimer timer;
	bool enabled;

	/*
	 * Set true by hardware VSYNC, cleared by timer.
	 * Timer skips if this is set (hardware already handled it).
	 */
	atomic_t hw_vsync_seen;

	/* Rate limiting - prevent VSYNCs closer than VSYNC_MIN_INTERVAL_MS */
	unsigned long last_vsync_jiffies;
};

/*
 * Check if enough time has passed since last VSYNC.
 * Returns true if we should process this VSYNC, false to skip.
 */
static bool vsync_rate_limit_ok(struct p4_vblank *v)
{
	unsigned long now = jiffies;
	unsigned long min_interval = msecs_to_jiffies(VSYNC_MIN_INTERVAL_MS);

	if (time_before(now, v->last_vsync_jiffies + min_interval))
		return false;

	v->last_vsync_jiffies = now;
	return true;
}

/*
 * Backup timer callback.
 * Advances DRM vblank counter and wakes drainer.
 * Skips entirely if hardware VSYNC fired since last timer.
 */
static enum hrtimer_restart backup_timer_fn(struct hrtimer *timer)
{
	struct p4_vblank *v = container_of(timer, struct p4_vblank, timer);

	if (atomic_xchg(&v->hw_vsync_seen, 0)) {
		/* Hardware VSYNC fired - skip this timer period */
	} else if (vsync_rate_limit_ok(v)) {
		/* No hardware VSYNC - wake drainer (flags only) and advance DRM counter */
		p4_sender_signal_vsync(v->p4, false);
		drm_crtc_handle_vblank(&v->p4->pipe.crtc);
	}

	hrtimer_forward_now(timer, ns_to_ktime(VBLANK_BACKUP_PERIOD_NS));
	return HRTIMER_RESTART;
}

/*
 * Hardware VSYNC IRQ handler.
 * This is the authoritative vblank - triggers both DRM accounting
 * and frame transmission work.
 */
static irqreturn_t vsync_irq_handler(int irq, void *data)
{
	struct p4_vblank *v = data;

	/* Mark that hardware VSYNC arrived (for timer to see) */
	atomic_set(&v->hw_vsync_seen, 1);

	/* Rate limit - skip if too soon after previous VSYNC */
	if (!vsync_rate_limit_ok(v))
		return IRQ_HANDLED;

	/* Wake drainer to encode and send frame */
	p4_sender_signal_vsync(v->p4, true);

	/* Advance DRM vblank counter */
	drm_crtc_handle_vblank(&v->p4->pipe.crtc);

	return IRQ_HANDLED;
}

/*
 * DRM vblank enable callback.
 * Called when vblank refcount goes from 0 to 1.
 * Also called during resume via drm_crtc_vblank_on().
 */
int p4_vblank_enable(struct drm_simple_display_pipe *pipe)
{
	struct p4_device *p4 = drm_to_p4(pipe->crtc.dev);
	struct p4_vblank *v = p4->vblank;

	DBG("vblank enable\n");

	if (!v->enabled) {
		v->enabled = true;
		atomic_set(&v->hw_vsync_seen, 0);
		enable_irq(v->vsync_irq);
		hrtimer_start(&v->timer, ns_to_ktime(VBLANK_BACKUP_PERIOD_NS),
			      HRTIMER_MODE_REL);
	}

	return 0;
}

/*
 * DRM vblank disable callback.
 * Called when vblank refcount goes to 0.
 * Also called during suspend via drm_crtc_vblank_off().
 */
void p4_vblank_disable(struct drm_simple_display_pipe *pipe)
{
	struct p4_device *p4 = drm_to_p4(pipe->crtc.dev);
	struct p4_vblank *v = p4->vblank;

	DBG("vblank disable\n");

	if (v->enabled) {
		v->enabled = false;
		hrtimer_cancel(&v->timer);
		disable_irq(v->vsync_irq);
	}
}

/*
 * CRTC lifecycle - called by p4_enable/p4_disable.
 */
void p4_crtc_vblank_on(struct p4_device *p4)
{
	DBG("crtc vblank on\n");
	drm_crtc_vblank_on(&p4->pipe.crtc);
}

void p4_crtc_vblank_off(struct p4_device *p4)
{
	DBG("crtc vblank off\n");
	drm_crtc_vblank_off(&p4->pipe.crtc);
}

/* ===== Probe/remove ===== */

int p4_vblank_probe(struct p4_device *p4)
{
	struct device *dev = &p4->spi->dev;
	struct gpio_desc *vsync_gpio;
	struct p4_vblank *v;
	int ret;

	v = devm_kzalloc(dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;

	v->p4 = p4;
	v->enabled = false;
	atomic_set(&v->hw_vsync_seen, 0);
	p4->vblank = v;

	hrtimer_init(&v->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	v->timer.function = backup_timer_fn;

	vsync_gpio = devm_gpiod_get(dev, "vsync", GPIOD_IN);
	if (IS_ERR(vsync_gpio))
		return dev_err_probe(dev, PTR_ERR(vsync_gpio), "vsync GPIO\n");

	v->vsync_irq = gpiod_to_irq(vsync_gpio);
	if (v->vsync_irq < 0)
		return dev_err_probe(dev, v->vsync_irq, "vsync IRQ\n");

	ret = devm_request_irq(dev, v->vsync_irq, vsync_irq_handler,
			       IRQF_TRIGGER_RISING | IRQF_NO_AUTOEN,
			       "p4-vsync", v);
	if (ret)
		return dev_err_probe(dev, ret, "request vsync IRQ\n");

	DBG("vblank: VSYNC IRQ %d, backup timer at 22ms\n", v->vsync_irq);
	return 0;
}

void p4_vblank_remove(struct p4_device *p4)
{
	struct p4_vblank *v = p4->vblank;

	if (v && v->enabled) {
		v->enabled = false;
		hrtimer_cancel(&v->timer);
		disable_irq(v->vsync_irq);
	}
}
