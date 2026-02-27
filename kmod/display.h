/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _P4_DISPLAY_H
#define _P4_DISPLAY_H

/*
 * Display constants - shared between kernel driver and userspace tools.
 * No kernel dependencies.
 */

/* Display dimensions (userspace view) */
#define P4_WIDTH	720
#define P4_HEIGHT	280

/* Hardware dimensions are rotated 90° from userspace */
#define HW_WIDTH	P4_HEIGHT
#define HW_HEIGHT	P4_WIDTH

/* Framebuffer size in bytes (1bpp) */
#define FB_SIZE		((HW_WIDTH / 8) * HW_HEIGHT)

/*
 * Hardware timing constants (milliseconds).
 * Used by both kernel driver and userspace calibrator.
 */
#define RESET_ASSERT_MS		10	/* Time to hold reset low */
#define RESET_SETTLE_MS		50	/* Time after reset before device ready */
#define ENABLE_SETTLE_MS	10	/* Time after ENABLE change for READY */
#define GPIO_SETTLE_MS		1	/* General GPIO settling time */

/* SPI READY signal timeout (ms)
 * Worst case: FIFO has 4096 bytes of minimal (7-byte) packets with new_frame=1,
 * each requiring a vblank (20ms). 4096/7 ≈ 585 packets × 20ms ≈ 11.7 seconds.
 * This pathological case can occur during frame-test with many iterations.
 * 600ms handles typical operation; extreme cases may still timeout.
 */
#define SPI_READY_TIMEOUT_MS	600

/* Hotplug debounce delay (ms) - prevents spurious events */
#define HOTPLUG_DEBOUNCE_MS	50

/* FPGA wake-from-sleep settling time (ms) */
#define WAKE_SETTLE_MS		5

/* Minimum interval between device resets (ms) - prevents reset storms */
#define RESET_MIN_INTERVAL_MS	1000

/* Maximum SPI transfer size per chunk */
#define SPI_CHUNK_MAX		4096

#endif /* _P4_DISPLAY_H */
