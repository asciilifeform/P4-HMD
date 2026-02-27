// SPDX-License-Identifier: GPL-2.0
/*
 * P4 display driver - sender subsystem
 * Handles SPI, GPIO, IRQ, drainer thread.
 */

#include <linux/kthread.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/completion.h>

#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "sender.h"
#include "vblank.h"

struct p4_sender {
	struct p4_device *p4;

	/* GPIOs */
	struct gpio_desc *nreset_gpio;
	struct gpio_desc *enable_gpio;
	struct gpio_desc *ready_gpio;
	struct gpio_desc *cold_gpio;
	struct gpio_desc *nsleep_gpio;

	/* IRQs */
	int ready_irq;
	int cold_irq;
	struct completion ready_completion;

	/* Disconnect detection */
	struct timer_list disconnect_timer;  /* 600ms READY low = disconnect */
	struct work_struct reconnect_work;   /* Deferred reconnect handling */
	struct work_struct disconnect_work;  /* Deferred disconnect handling */

	/* Drainer */
	struct task_struct *drainer;
	struct completion vsync;              /* Signaled by VSYNC IRQ or backup timer */
	struct completion send_done;          /* Signaled when send completes */
	bool hw_vsync;                        /* True if woken by real VSYNC */

	/* Send buffer - simple contiguous buffer, no kfifo */
	u8 *buf;                             /* Encode buffer, size SPI_BUF_SIZE */
	size_t buf_len;                      /* Bytes encoded */
	size_t buf_pos;                      /* Bytes sent */
	u8 last_sent_flags;                  /* For detecting flag changes */

	/*
	 * Data flow is protected by two flags:
	 *
	 * - enabled: Set by DRM enable/disable lifecycle (p4_enable/p4_disable).
	 *   When false, p4_update() returns early without generating any data.
	 *   This ensures no data flows before DRM is fully initialized or after
	 *   it starts shutting down. Uses atomic_t for lock-free access.
	 *
	 * - device_online: Set by drainer thread based on hardware state.
	 *   When false, buf_emit() rejects data and sets force_full.
	 *   This handles hardware errors (device not responding, SPI failures).
	 *   The drainer determines online status at startup by probing READY,
	 *   and updates it dynamically based on device responses/timeouts.
	 *
	 * Both must be true for data to flow. The drainer is started
	 * at the end of p4_sender_probe() but enabled remains false until
	 * p4_enable() is called after full DRM initialization.
	 */
	bool device_online;
	atomic_t enabled;
	bool probe_complete;		/* Set after probe finishes */
	unsigned long last_reset_jiffies;  /* For rate-limiting resets */
};

/* ===== Buffer helpers ===== */

/* Forward declaration - defined below in Packet emission section */
static bool buf_emit(const struct packet_header *pkt, void *ctx);

static inline bool buf_is_empty(struct p4_sender *s)
{
	return s->buf_pos >= s->buf_len;
}

static inline void buf_reset(struct p4_sender *s)
{
	s->buf_len = 0;
	s->buf_pos = 0;
}

/* ===== State ===== */

/*
 * Go offline: reject new data and reset buffer.
 *
 * Called from:
 * - reset_device(): device needs reinitialization (cold detection, SPI
 *   error, probe, resume, powerup)
 * - suspend: shutting down for system sleep
 *
 * force_full ensures we'll resend everything on recovery.
 *
 * Signal send_done so any waiters (suspend/powerdown) wake
 * immediately instead of timing out.
 */
static void go_offline(struct p4_sender *s)
{
	DBG("going offline\n");
	s->device_online = false;
	
	mutex_lock(&s->p4->update_lock);
	s->p4->force_full = true;
	mutex_unlock(&s->p4->update_lock);
	
	complete(&s->send_done);
}

static void go_online(struct p4_sender *s)
{
	DBG("going online\n");
	s->device_online = true;
}

/* ===== Hardware functions ===== */

/* Forward declarations */
static void reconnect_work_fn(struct work_struct *work);
static void disconnect_work_fn(struct work_struct *work);
static void disconnect_timer_fn(struct timer_list *t);

/* IRQ handler - both edges for disconnect detection */
static irqreturn_t ready_irq_handler(int irq, void *data)
{
	struct p4_sender *s = data;
	bool ready_high = gpiod_get_value(s->ready_gpio);
	static unsigned long last_edge_jiffies;
	static bool last_state;
	unsigned long now = jiffies;

	/* Log edge transitions with timing (rate limited to avoid spam) */
	if (time_after(now, last_edge_jiffies + msecs_to_jiffies(HOTPLUG_DEBOUNCE_MS)) ||
	    ready_high != last_state) {
		pr_debug("p4: READY %s->%s (%u ms since last)\n",
			last_state ? "H" : "L",
			ready_high ? "H" : "L",
			jiffies_to_msecs(now - last_edge_jiffies));
	}
	last_edge_jiffies = now;
	last_state = ready_high;

	if (ready_high) {
		/* Rising edge - cancel disconnect timer, signal waiters */
		del_timer(&s->disconnect_timer);
		complete(&s->ready_completion);

		/* If we were disconnected and probe is complete, schedule reconnect */
		if (!s->device_online && s->probe_complete) {
			/* schedule_work returns false if already queued */
			if (schedule_work(&s->reconnect_work))
				pr_info("p4: READY rising while offline - scheduling reconnect\n");
		}
	} else {
		/* Falling edge - start disconnect timer */
		mod_timer(&s->disconnect_timer,
			  jiffies + msecs_to_jiffies(SPI_READY_TIMEOUT_MS));
	}
	return IRQ_HANDLED;
}

/*
 * COLD IRQ handler - rising edge means display needs reinitialization.
 * This happens on unplug or protocol error. Triggers immediate disconnect
 * instead of waiting for READY timeout.
 */
static irqreturn_t cold_irq_handler(int irq, void *data)
{
	struct p4_sender *s = data;

	/*
	 * COLD rising while we're online = display disconnected or failed.
	 * Cancel READY timer and trigger immediate disconnect.
	 */
	if (s->device_online) {
		del_timer(&s->disconnect_timer);
		schedule_work(&s->disconnect_work);
		pr_info("p4: COLD rising while online - immediate disconnect\n");
	}

	return IRQ_HANDLED;
}

/* Forward declarations */
static int hw_gpio_init(struct p4_sender *s, struct device *dev);
static int hw_irq_init(struct p4_sender *s, struct device *dev);
static void hw_reset_device(struct p4_sender *s);
static int hw_wait_ready(struct p4_sender *s, unsigned long timeout_ms);
static int hw_spi_transfer(struct p4_device *p4, const void *data, size_t len);
static bool hw_device_is_cold(struct p4_sender *s);
static void hw_irq_enable(struct p4_sender *s);
static void hw_irq_disable(struct p4_sender *s);
static int hw_verify_ready(struct p4_sender *s);

static int hw_gpio_init(struct p4_sender *s, struct device *dev)
{
	s->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	s->ready_gpio = devm_gpiod_get(dev, "ready", GPIOD_IN);
	s->cold_gpio = devm_gpiod_get(dev, "cold", GPIOD_IN);
	s->nreset_gpio = devm_gpiod_get(dev, "nreset", GPIOD_OUT_HIGH);
	s->nsleep_gpio = devm_gpiod_get(dev, "nsleep", GPIOD_OUT_HIGH);
	if (IS_ERR(s->enable_gpio) || IS_ERR(s->ready_gpio) ||
	    IS_ERR(s->cold_gpio) || IS_ERR(s->nreset_gpio) ||
	    IS_ERR(s->nsleep_gpio))
		return -ENODEV;
	return 0;
}

static int hw_irq_init(struct p4_sender *s, struct device *dev)
{
	int ret;

	/* READY IRQ - both edges for flow control and disconnect timeout */
	s->ready_irq = gpiod_to_irq(s->ready_gpio);
	if (s->ready_irq < 0)
		return s->ready_irq;

	ret = devm_request_irq(dev, s->ready_irq, ready_irq_handler,
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_NO_AUTOEN,
			       "p4-ready", s);
	if (ret)
		return ret;

	/* COLD IRQ - rising edge for immediate disconnect detection */
	s->cold_irq = gpiod_to_irq(s->cold_gpio);
	if (s->cold_irq < 0)
		return s->cold_irq;

	ret = devm_request_irq(dev, s->cold_irq, cold_irq_handler,
			       IRQF_TRIGGER_RISING | IRQF_NO_AUTOEN,
			       "p4-cold", s);
	return ret;
}

static void hw_reset_device(struct p4_sender *s)
{
	gpiod_set_value(s->nreset_gpio, 0);
	msleep(RESET_ASSERT_MS);
	gpiod_set_value(s->nreset_gpio, 1);
	msleep(RESET_SETTLE_MS);
}

static int hw_wait_ready(struct p4_sender *s, unsigned long timeout_ms)
{
	long ret;

	/*
	 * Check if READY is already high BEFORE reinit.
	 * This avoids a race where:
	 *   1. reinit_completion clears pending signal
	 *   2. READY goes high, IRQ fires, completion is signaled
	 *   3. gpiod_get_value sees low (race - just missed it)
	 *   4. wait_for_completion waits forever
	 *
	 * By checking FIRST, we catch the already-high case.
	 * If READY goes high after our check but before wait,
	 * the IRQ will signal the completion and wait returns immediately.
	 */
	if (gpiod_get_value(s->ready_gpio))
		return 0;

	/* Now safe to reinit and wait */
	reinit_completion(&s->ready_completion);

	/* Re-check after reinit in case READY went high during reinit */
	if (gpiod_get_value(s->ready_gpio))
		return 0;

	/* Wait for rising edge IRQ */
	ret = wait_for_completion_interruptible_timeout(
		&s->ready_completion, msecs_to_jiffies(timeout_ms));

	if (ret <= 0)
		return -ETIMEDOUT;

	return 0;
}

static int hw_spi_transfer(struct p4_device *p4, const void *data, size_t len)
{
	struct spi_transfer xfer = {
		.tx_buf = data,
		.len = len,
		.speed_hz = p4->spi->max_speed_hz,
		.bits_per_word = 8,
	};
	int ret;

	/* Full hex dump for debugging - controlled by module param */
	if (p4->hexdump_spi)
		print_hex_dump(KERN_INFO, "SPI: ", DUMP_PREFIX_OFFSET, 32, 1, data, len, false);

	ret = spi_sync_transfer(p4->spi, &xfer, 1);

	if (ret)
		dev_err(&p4->spi->dev, "spi_sync_transfer failed: %d\n", ret);

	return ret;
}

static bool hw_device_is_cold(struct p4_sender *s)
{
	return gpiod_get_value(s->cold_gpio);
}

static void hw_irq_enable(struct p4_sender *s)
{
	gpiod_set_value(s->enable_gpio, 1);  /* ENABLE high */
	enable_irq(s->ready_irq);
	enable_irq(s->cold_irq);
}

static void hw_irq_disable(struct p4_sender *s)
{
	disable_irq(s->cold_irq);
	disable_irq(s->ready_irq);
	del_timer_sync(&s->disconnect_timer);
	cancel_work_sync(&s->disconnect_work);
	gpiod_set_value(s->enable_gpio, 0);  /* ENABLE low */
}

/* ===== Disconnect detection ===== */

/*
 * Called when READY has been low for 600ms.
 * This means the display is disconnected.
 */
/*
 * Disconnect work function - runs in process context.
 * Called from disconnect_timer_fn via schedule_work().
 *
 * When display is unplugged:
 * 1. READY falls and stays low
 * 2. VSYNC ceases
 * 3. If FPGA was mid-transfer, it enters fail state (COLD rises)
 *
 * We immediately reset the FPGA so it's ready when display is plugged back in.
 * READY IRQ remains enabled to detect reconnection.
 */
static void disconnect_work_fn(struct work_struct *work)
{
	struct p4_sender *s = container_of(work, struct p4_sender, disconnect_work);
	struct p4_device *p4 = s->p4;

	dev_info(&p4->spi->dev, "display disconnected\n");

	/* Mark offline - drainer will discard buffer on next iteration */
	s->device_online = false;

	mutex_lock(&p4->update_lock);
	p4->dirty = true;
	p4->force_full = true;
	mutex_unlock(&p4->update_lock);

	/* Signal waiters (suspend/powerdown) */
	complete(&s->send_done);

	/* Reset FPGA immediately so it's ready when display returns */
	hw_reset_device(s);
	s->last_reset_jiffies = jiffies;

	/* Notify DRM of hotplug */
	if (p4->display_connected) {
		p4->display_connected = false;
		drm_kms_helper_hotplug_event(&p4->drm);
	}
}

static void disconnect_timer_fn(struct timer_list *t)
{
	struct p4_sender *s = from_timer(s, t, disconnect_timer);

	/* 
	 * Timer runs in softirq context - cannot use mutex or call
	 * functions that may sleep. Defer to work queue.
	 */
	schedule_work(&s->disconnect_work);
}

/*
 * Called from IRQ context (via schedule_work) when READY rises
 * after we were disconnected.
 *
 * Reconnect sequence:
 * 1. READY rose - display was plugged back in
 * 2. Issue one reset and wait for READY to rise again
 * 3. Send flags packet to restore display state
 * 4. Resume vblank work processing
 * 5. Notify DRM of reconnection
 */
static void reconnect_work_fn(struct work_struct *work)
{
	struct p4_sender *s = container_of(work, struct p4_sender, reconnect_work);
	struct p4_device *p4 = s->p4;
	int ret;

	/* Already online - nothing to do (can happen on initial probe race) */
	if (s->device_online)
		return;

	dev_info(&p4->spi->dev, "display reconnecting...\n");

	/* Reset and wait for READY */
	hw_reset_device(s);
	s->last_reset_jiffies = jiffies;

	ret = hw_verify_ready(s);
	if (ret) {
		dev_warn(&p4->spi->dev, "reconnect: READY not high after reset, will retry\n");
		/* READY IRQ will fire again when display is ready */
		return;
	}

	/* Go online */
	s->device_online = true;

	/* Mark for full refresh - flags will go out with first frame */
	mutex_lock(&p4->update_lock);
	p4->dirty = true;
	p4->force_full = true;
	mutex_unlock(&p4->update_lock);
	if (hw_device_is_cold(s))
		dev_info(&p4->spi->dev, "COLD high after reconnect - will send full refresh\n");

	/* Notify DRM of hotplug */
	if (!p4->display_connected) {
		dev_info(&p4->spi->dev, "display reconnected\n");
		p4->display_connected = true;
		drm_kms_helper_hotplug_event(&p4->drm);
	} else {
		dev_info(&p4->spi->dev, "display recovered\n");
	}
}

/*
 * Verify READY handshake after reset.
 *
 * After reset with ENABLE high, device FIFO has space so READY should be high.
 * Uses the IRQ path to verify interrupt handling works.
 * Returns 0 on success, -EIO if handshake fails.
 *
 * Note: ENABLE is managed by hw_irq_enable/disable, not here.
 */
static int hw_verify_ready(struct p4_sender *s)
{
	long ret;

	/* ENABLE should already be high (set by hw_irq_enable) */
	/* Check if READY is already high */
	if (gpiod_get_value(s->ready_gpio))
		return 0;

	/* Wait for IRQ */
	reinit_completion(&s->ready_completion);

	/* Re-check after reinit */
	if (gpiod_get_value(s->ready_gpio))
		return 0;

	ret = wait_for_completion_timeout(&s->ready_completion,
					  msecs_to_jiffies(READY_REPLUG_MS));
	if (ret == 0) {
		pr_err("p4: READY IRQ timeout after reset (device not responding?)\n");
		return -EIO;
	}

	return 0;
}

/* ===== FPGA sleep control ===== */

/*
 * Put FPGA into sleep mode (stop clock).
 * NSLEEP is active-low: drive LOW to assert sleep.
 *
 * Prerequisites (caller must ensure):
 * - FIFO is drained (no pending SPI transfers)
 * - IRQ is disabled (which also sets ENABLE low via hw_irq_disable)
 */
static void fpga_sleep(struct p4_sender *s)
{
	if (!s->nsleep_gpio)
		return;

	gpiod_set_value(s->nsleep_gpio, 0);
	DBG("FPGA sleep\n");
}

/*
 * Wake FPGA from sleep mode (restart clock).
 * NSLEEP is active-low: drive HIGH to deassert sleep.
 * Must be called before any SPI communication.
 */
static void fpga_wake(struct p4_sender *s)
{
	if (!s->nsleep_gpio)
		return;
	gpiod_set_value(s->nsleep_gpio, 1);
	DBG("FPGA wake\n");
}

/*
 * Reset device to known state.
 *
 * Always go offline first to:
 * 1. Flush stale FIFO data (device state is unknown during reset)
 * 2. Prevent drainer from racing with verification
 * 3. Set force_full (device framebuffer cleared by reset)
 *
 * Caller must call go_online() after successful reset if drainer
 * should resume operation.
 *
 * Rate-limited to prevent reset storms - waits if called too soon
 * after previous reset.
 */
static int reset_device(struct p4_sender *s)
{
	int ret;
	unsigned long min_interval = msecs_to_jiffies(RESET_MIN_INTERVAL_MS);
	unsigned long elapsed = jiffies - s->last_reset_jiffies;

	/* Rate limit resets */
	if (s->last_reset_jiffies && time_before(jiffies, s->last_reset_jiffies + min_interval)) {
		unsigned long wait_ms = jiffies_to_msecs(min_interval - elapsed);
		DBG("rate-limiting reset, waiting %lu ms\n", wait_ms);
		msleep(wait_ms);
	}

	go_offline(s);
	hw_reset_device(s);
	s->last_reset_jiffies = jiffies;

	ret = hw_verify_ready(s);
	if (ret)
		dev_err(&s->p4->spi->dev, "device reset verification failed\n");

	return ret;
}

/* ===== Drainer ===== */

/*
 * Encode current frame into buffer.
 * Called by drainer on VSYNC with SCHED_FIFO priority.
 * Returns true if data was encoded, false if nothing to send.
 */
static bool encode_frame(struct p4_sender *s)
{
	struct p4_device *p4 = s->p4;
	bool do_full;
	bool ok;

	/* Skip if device is offline */
	if (!s->device_online)
		return false;

	mutex_lock(&p4->update_lock);

	if (!p4->dirty) {
		mutex_unlock(&p4->update_lock);
		return false;
	}

	memcpy(p4->dst_buf[1], p4->dst_buf[0], FB_SIZE);
	do_full = p4->force_full;

	/* Reset buffer for new frame */
	buf_reset(s);

	if (do_full) {
		memset(p4->dst_buf[2], 0, FB_SIZE);
		ok = p4_send_clear(p4);
		if (!ok)
			goto fail;
	}

	ok = p4_send_diff(p4, p4->dst_buf[2], p4->dst_buf[1], 0, FB_SIZE, !do_full);
	if (!ok)
		goto fail;

	/*
	 * If nothing was encoded (buffers identical), just clear dirty.
	 * Don't clear force_full - reference buffer unchanged.
	 */
	if (buf_is_empty(s)) {
		p4->dirty = false;
		mutex_unlock(&p4->update_lock);
		return false;
	}

	/*
	 * Encoded successfully. Clear dirty and force_full.
	 * Note: we do NOT update buf2 here - that happens after
	 * successful send to ensure buf2 always matches FPGA state.
	 */
	p4->dirty = false;
	p4->force_full = false;
	mutex_unlock(&p4->update_lock);
	return true;

fail:
	buf_reset(s);
	mutex_unlock(&p4->update_lock);
	if (s->device_online)
		dev_err(&p4->spi->dev, "drainer: encode failed!\n");
	return false;
}

static int drainer_thread(void *data)
{
	struct p4_sender *s = data;
	struct p4_device *p4 = s->p4;
	int ret;

	DBG("drainer started\n");

	/*
	 * Check device status at startup.
	 * If READY is high, we're connected. If not, we're disconnected
	 * and the 600ms timer will handle it.
	 */
	if (gpiod_get_value(s->ready_gpio)) {
		s->device_online = true;
		p4->display_connected = true;
	} else {
		s->device_online = false;
		p4->display_connected = false;
	}

	while (!kthread_should_stop()) {
		/*
		 * Wait for VSYNC (real or backup timer).
		 */
		wait_for_completion_interruptible(&s->vsync);

		if (kthread_should_stop())
			break;

		/*
		 * Reinit completion for next VSYNC. Must be done before
		 * processing to avoid missing a VSYNC that arrives during
		 * encode/send.
		 */
		reinit_completion(&s->vsync);

		/*
		 * Only real hardware VSYNC can trigger frame encoding.
		 * Faux VSYNC (backup timer) can only send flags.
		 */
		if (s->hw_vsync)
			encode_frame(s);

		/*
		 * If no frame data (or faux VSYNC), check if flags changed.
		 * This allows waking display from standby/blank.
		 */
		if (buf_is_empty(s)) {
			u8 current_flags = p4->display_flags.byte;
			if (current_flags != s->last_sent_flags) {
				struct packet_header pkt;
				pkt_init_flags_only(&pkt, current_flags);
				buf_emit(&pkt, s);
			}
		}

		/*
		 * Skip send if nothing to send, device offline, or READY low.
		 * READY low at start means device is disconnected or stuck.
		 * buf2 remains valid - next vsync will retry.
		 */
		if (buf_is_empty(s) || !s->device_online ||
		    !gpiod_get_value(s->ready_gpio))
			goto next;

		/*
		 * Send everything in buffer.
		 * On timeout or error, discard remaining data.
		 */
		bool send_ok = true;
		while (s->buf_pos < s->buf_len) {
			size_t chunk = min_t(size_t, SPI_CHUNK_MAX,
					     s->buf_len - s->buf_pos);

			ret = hw_wait_ready(s, SPI_READY_TIMEOUT_MS);
			if (ret != 0) {
				if (kthread_should_stop())
					goto out;
				dev_warn(&p4->spi->dev, "drainer: READY timeout\n");
				buf_reset(s);
				send_ok = false;
				break;
			}

			ret = hw_spi_transfer(p4, s->buf + s->buf_pos, chunk);
			if (ret) {
				dev_err(&p4->spi->dev,
					"SPI transfer error: %d\n", ret);
				buf_reset(s);
				send_ok = false;
				break;
			}

			s->buf_pos += chunk;
		}

		/*
		 * Update reference buffer after successful send.
		 * buf2 must always reflect what FPGA actually has.
		 * No lock needed - buf1/buf2 are drainer-private.
		 * If send failed, buf2 remains valid - next vsync will retry.
		 */
		if (send_ok && s->buf_pos == s->buf_len && s->buf_len > 0) {
			swap(p4->dst_buf[1], p4->dst_buf[2]);
			s->last_sent_flags = p4->display_flags.byte;
		}

next:
		/* Reset for next iteration */
		buf_reset(s);

		complete(&s->send_done);
	}

out:
	DBG("drainer exiting\n");
	return 0;
}

static int drainer_start(struct p4_sender *s)
{
	DBG("starting drainer\n");

	/* Reinit completions in case they're stale from previous run */
	reinit_completion(&s->vsync);
	reinit_completion(&s->send_done);

	s->drainer = kthread_run(drainer_thread, s, "p4-drainer");
	if (IS_ERR(s->drainer)) {
		int ret = PTR_ERR(s->drainer);
		s->drainer = NULL;
		DBG("drainer start failed: %d\n", ret);
		return ret;
	}

#ifdef DRAINER_SCHED_FIFO
	sched_set_fifo_low(s->drainer);
	DBG("drainer set to SCHED_FIFO (low priority)\n");
#endif

	return 0;
}

static void drainer_stop(struct p4_sender *s)
{
	if (s->drainer) {
		DBG("stopping drainer\n");
		/*
		 * Wake drainer from any wait it might be blocked on.
		 * vsync: waiting for VSYNC (timer stopped during suspend)
		 * ready_completion: waiting for READY GPIO
		 */
		complete(&s->vsync);
		complete(&s->ready_completion);
		kthread_stop(s->drainer);
		s->drainer = NULL;
	}
}

/* ===== Packet emission ===== */

/*
 * Emit a packet to the buffer.
 *
 * Returns false if device is offline/disabled or buffer is full.
 * Buffer overflow is a fatal bug - should never happen since we
 * reset buffer before encoding each frame.
 */
static bool buf_emit(const struct packet_header *pkt, void *ctx)
{
	struct p4_sender *s = ctx;
	size_t needed = pkt_hdr_size(pkt) + pkt_data_len(pkt);

	/* Check for address overflow */
	if (!pkt_is_flags_only(pkt)) {
		u16 addr = pkt_addr(pkt);
		u16 len = pkt_len(pkt);
		u32 end = (u32)addr + (u32)len;

		if (end > FB_SIZE) {
			dev_err(&s->p4->spi->dev,
				"FATAL: addr overflow! addr=%u len=%u end=%u > %u\n",
				addr, len, end, FB_SIZE);
			return false;
		}
	}

	if (!atomic_read(&s->enabled) || !s->device_online)
		return false;

	if (s->buf_len + needed > SPI_BUF_SIZE) {
		dev_err(&s->p4->spi->dev,
			"FATAL: buffer overflow! needed=%zu avail=%zu\n",
			needed, SPI_BUF_SIZE - s->buf_len);
		return false;
	}

	memcpy(s->buf + s->buf_len, pkt_wire(pkt), pkt_hdr_size(pkt));
	s->buf_len += pkt_hdr_size(pkt);

	if (pkt_data_len(pkt)) {
		memcpy(s->buf + s->buf_len, pkt_data(pkt), pkt_data_len(pkt));
		s->buf_len += pkt_data_len(pkt);
	}

	return true;
}

/* ===== Public API ===== */

int p4_sender_probe(struct p4_device *p4)
{
	struct device *dev = &p4->spi->dev;
	struct p4_sender *s;
	int ret;

	/* Allocate sender state */
	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s) {
		dev_err(dev, "failed to allocate sender state\n");
		return -ENOMEM;
	}

	s->p4 = p4;
	p4->sender = s;

	init_completion(&s->vsync);
	init_completion(&s->send_done);
	init_completion(&s->ready_completion);
	timer_setup(&s->disconnect_timer, disconnect_timer_fn, 0);
	INIT_WORK(&s->reconnect_work, reconnect_work_fn);
	INIT_WORK(&s->disconnect_work, disconnect_work_fn);
	s->probe_complete = false;

	/* GPIOs */
	DBG("getting GPIOs\n");
	ret = hw_gpio_init(s, dev);
	if (ret)
		return dev_err_probe(dev, ret, "GPIO init failed\n");
	DBG("GPIOs OK\n");

	/* Ready IRQ */
	ret = hw_irq_init(s, dev);
	if (ret)
		return dev_err_probe(dev, ret, "IRQ init failed\n");
	DBG("IRQ OK\n");

	/* Allocate send buffer */
	DBG("allocating send buffer\n");
	s->buf = devm_kmalloc(dev, SPI_BUF_SIZE, GFP_KERNEL);
	if (!s->buf) {
		dev_err(dev, "failed to allocate send buffer\n");
		return -ENOMEM;
	}
	s->buf_len = 0;
	s->buf_pos = 0;
	DBG("send buffer allocated: %u bytes\n", SPI_BUF_SIZE);

	/* Enable IRQ before reset verification (needs IRQ for completion) */
	hw_irq_enable(s);

	/*
	 * Reset device to known state. If this fails, the device isn't
	 * responding - but we still start the drainer, which will detect
	 * the actual state and wait for reconnection if needed.
	 */
	ret = reset_device(s);
	if (ret)
		dev_warn(dev, "device not ready at probe, will wait for connection\n");
	else
		go_online(s);

	ret = drainer_start(s);
	if (ret) {
		dev_err(dev, "failed to start drainer: %d\n", ret);
		hw_irq_disable(s);
		return ret;
	}

	s->probe_complete = true;

	/*
	 * If device wasn't ready during probe but is ready now,
	 * schedule reconnect to bring it online.
	 */
	if (!s->device_online && gpiod_get_value(s->ready_gpio)) {
		dev_info(dev, "device ready after probe - scheduling reconnect\n");
		schedule_work(&s->reconnect_work);
	}

	return 0;
}

void p4_sender_remove(struct p4_device *p4)
{
	struct p4_sender *s = p4->sender;
	if (!s)
		return;

	DBG("sender remove\n");
	
	/* Cancel pending work and timer before stopping drainer */
	del_timer_sync(&s->disconnect_timer);
	cancel_work_sync(&s->disconnect_work);
	cancel_work_sync(&s->reconnect_work);
	
	drainer_stop(s);
}

/*
 * Wait for current send to complete before suspend/powerdown.
 * Drainer signals send_done after each iteration.
 */
static void wait_send_done(struct p4_sender *s)
{
	DBG("waiting for send to complete\n");
	reinit_completion(&s->send_done);
	/*
	 * Wake drainer from any blocking wait:
	 * - vsync: sleeping between frames
	 * - ready_completion: waiting for READY GPIO during send
	 */
	complete(&s->vsync);
	complete(&s->ready_completion);
	wait_for_completion_timeout(&s->send_done,
				    msecs_to_jiffies(SUSPEND_DRAIN_MS));
}

/*
 * System suspend - called when machine enters sleep.
 * Drain FIFO, disable IRQ, stop drainer, put FPGA to sleep.
 */
int p4_sender_suspend(struct p4_device *p4)
{
	struct p4_sender *s = p4->sender;

	dev_info(&p4->spi->dev, "suspending\n");
	atomic_set(&s->enabled, 0);
	wait_send_done(s);

	hw_irq_disable(s);
	go_offline(s);
	drainer_stop(s);
	fpga_sleep(s);

	return 0;
}

/*
 * System resume - called when machine wakes from sleep.
 * Wake FPGA, reset device, restart drainer, enable IRQ.
 * Check COLD to see if full refresh needed.
 */
int p4_sender_resume(struct p4_device *p4)
{
	struct p4_sender *s = p4->sender;
	int ret;

	dev_info(&p4->spi->dev, "resuming\n");

	/* Wake FPGA before any hardware communication */
	fpga_wake(s);

	/* Enable IRQ before reset verification (needs IRQ for completion) */
	hw_irq_enable(s);

	/*
	 * Reset device to known state. If this fails, the device isn't
	 * responding - but we still start the drainer, which will detect
	 * the actual state and wait for reconnection if needed.
	 */
	ret = reset_device(s);
	if (ret) {
		dev_warn(&p4->spi->dev, "device not ready at resume, will wait for connection\n");
	} else {
		go_online(s);
		/* Check if display lost state during system sleep */
		if (hw_device_is_cold(s)) {
			dev_info(&p4->spi->dev, "COLD high after resume - forcing full refresh\n");
			mutex_lock(&p4->update_lock);
			p4->dirty = true;
			p4->force_full = true;
			mutex_unlock(&p4->update_lock);
		}
	}

	ret = drainer_start(s);
	if (ret) {
		dev_err(&p4->spi->dev, "failed to restart drainer: %d\n", ret);
		hw_irq_disable(s);
		return ret;
	}

	atomic_set(&s->enabled, 1);

	return 0;
}

/*
 * DPMS powerdown - called when display enters FB_BLANK_POWERDOWN.
 * Drain FIFO, put FPGA to sleep.
 * READY IRQ stays enabled so we can send wakeup flags on powerup.
 * Drainer thread stays running (idle) for faster resume.
 */
void p4_sender_powerdown(struct p4_device *p4)
{
	struct p4_sender *s = p4->sender;

	/* Atomically clear enabled, bail if already disabled */
	if (!atomic_xchg(&s->enabled, 0))
		return;

	dev_info(&p4->spi->dev, "DPMS powerdown\n");
	wait_send_done(s);
	/* Keep READY IRQ enabled - needed for powerup */
	fpga_sleep(s);
}

/*
 * DPMS powerup - called when display exits FB_BLANK_POWERDOWN.
 * Wake FPGA and re-enable. Check COLD to see if full refresh needed.
 * Drainer is still running but idle.
 */
void p4_sender_powerup(struct p4_device *p4)
{
	struct p4_sender *s = p4->sender;

	/* Atomically set enabled, bail if already enabled */
	if (atomic_xchg(&s->enabled, 1))
		return;

	dev_info(&p4->spi->dev, "DPMS powerup\n");

	/* Wake FPGA before any hardware communication */
	fpga_wake(s);

	/* Check if display lost state during sleep */
	if (hw_device_is_cold(s)) {
		dev_info(&p4->spi->dev, "COLD high after powerup - forcing full refresh\n");
		mutex_lock(&p4->update_lock);
		p4->dirty = true;
		p4->force_full = true;
		mutex_unlock(&p4->update_lock);
	}
}

bool p4_send_diff(struct p4_device *p4, const u8 *old_buf, const u8 *new_buf,
		  size_t scan_start, size_t scan_end, bool new_frame)
{
	struct p4_sender *s = p4->sender;
	struct encoder_state enc;

	/*
	 * Native mode (280x720) needs FPGA bit reversal because
	 * drm_fb_xrgb8888_to_mono() outputs LSB-first (bit 0 = leftmost)
	 * but hardware expects MSB-first (bit 7 = leftmost).
	 *
	 * Rotated mode does NOT need FPGA bit reversal because the
	 * rotation gather8() function bit-reverses each byte as it
	 * reads, converting LSB-first to MSB-first during the rotation.
	 */
	encoder_init(&enc, p4->display_flags.byte, new_frame, p4->is_native,
		     buf_emit, s);
	return encode_diff(&enc, old_buf, new_buf, scan_start, scan_end);
}

bool p4_send_clear(struct p4_device *p4)
{
	struct p4_sender *s = p4->sender;

	DBG("send_clear\n");

	/* Emit a single RLE packet of zeros covering the entire framebuffer.
	 * Use data packet with explicit addr=0 because the
	 * PE does not reset address automatically - it keeps the last address.
	 */
	struct packet_header pkt;
	static const u8 zero = 0x00;

	pkt_init_data(&pkt, 0 /* flags */, true /* new_frame */, true /* rle */,
		      p4->is_native /* bitrev */, 0 /* addr */, FB_SIZE);
	pkt.data = &zero;
	pkt.data_len = 1;

	bool ok = buf_emit(&pkt, s);
	DBG("send_clear: %s\n", ok ? "ok" : "failed");
	return ok;
}

bool p4_sender_is_enabled(struct p4_device *p4)
{
	return atomic_read(&p4->sender->enabled);
}

bool p4_sender_is_online(struct p4_device *p4)
{
	return p4->sender->device_online;
}

bool p4_sender_is_ready(struct p4_device *p4)
{
	return gpiod_get_value(p4->sender->ready_gpio);
}

/* DRM lifecycle enable - called from p4_enable() */
void p4_sender_enable(struct p4_device *p4)
{
	atomic_set(&p4->sender->enabled, 1);
}

/* DRM lifecycle disable - called from p4_disable() */
void p4_sender_disable(struct p4_device *p4)
{
	atomic_set(&p4->sender->enabled, 0);
}

void p4_sender_signal_vsync(struct p4_device *p4, bool hw)
{
	struct p4_sender *s = p4->sender;

	s->hw_vsync = hw;
	complete(&s->vsync);
}

void p4_set_upside_down(struct p4_device *p4, bool enable)
{
	p4->display_flags.f.upside_down = enable ? 1 : 0;
}

/*
 * Apply power mode to display flags.
 * Clears all power bits then sets the appropriate one. Preserves upside_down.
 */
static void apply_power_mode(struct p4_device *p4, enum p4_power_mode mode)
{
	p4->display_flags.f.standby = 0;
	p4->display_flags.f.blank = 0;
	p4->display_flags.f.low_intensity = 0;

	switch (mode) {
	case P4_POWER_OFF:
		p4->display_flags.f.standby = 1;
		break;
	case P4_POWER_BLANK:
		p4->display_flags.f.blank = 1;
		break;
	case P4_POWER_LOW:
		p4->display_flags.f.low_intensity = 1;
		break;
	case P4_POWER_NORMAL:
		break;
	}
}

/*
 * Set display power mode. These are mutually exclusive states.
 * When transitioning to NORMAL, respects target_power (brightness).
 * Preserves upside_down flag.
 */
void p4_set_power_mode(struct p4_device *p4, enum p4_power_mode mode)
{
	apply_power_mode(p4, mode == P4_POWER_NORMAL ? p4->target_power : mode);
}

/*
 * Set brightness (called by backlight subsystem).
 * Valid modes: P4_POWER_BLANK, P4_POWER_LOW, P4_POWER_NORMAL.
 * If display is on, applies immediately and updates target_power.
 * If display is off (standby), just update target_power for next wake.
 */
void p4_set_brightness(struct p4_device *p4, enum p4_power_mode brightness)
{
	if (brightness < P4_POWER_BLANK || brightness > P4_POWER_NORMAL)
		return;

	p4->target_power = brightness;

	if (!p4->display_flags.f.standby)
		apply_power_mode(p4, brightness);
}
