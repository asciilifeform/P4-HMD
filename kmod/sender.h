/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _P4_SENDER_H
#define _P4_SENDER_H

#include "p4.h"
#include "display.h"

/*
 * Sender subsystem - handles SPI, GPIO, IRQ, FIFO, drainer thread.
 * All hardware details encapsulated.
 */

/* FIFO size constant (SPI_CHUNK_MAX is in display.h) */
#define SPI_BUF_SIZE		32768

/* Timing constants - SPI_READY_TIMEOUT_MS is in display.h */
#define READY_REPLUG_MS		100
#define SUSPEND_DRAIN_MS	1000

/*
 * Real-time scheduling for drainer thread.
 * Define to enable SCHED_FIFO scheduling, which ensures the drainer
 * preempts all userland processes immediately when runnable.
 *
 * Uses sched_set_fifo_low() which sets priority to MAX_RT_PRIO/2 (50),
 * leaving headroom for critical kernel threads.
 */
#define DRAINER_SCHED_FIFO

/* Probe: init GPIOs, IRQs, FIFO, start drainer */
int p4_sender_probe(struct p4_device *p4);

/* Remove: stop drainer, free resources */
void p4_sender_remove(struct p4_device *p4);

/* System suspend: drain FIFO, stop drainer, put FPGA to sleep */
int p4_sender_suspend(struct p4_device *p4);

/* System resume: wake FPGA, reset device, restart drainer */
int p4_sender_resume(struct p4_device *p4);

/* DPMS powerdown: drain FIFO, disable IRQ, put FPGA to sleep */
void p4_sender_powerdown(struct p4_device *p4);

/* DPMS powerup: wake FPGA, re-enable IRQ */
void p4_sender_powerup(struct p4_device *p4);

/* Send diff-encoded frame data */
bool p4_send_diff(struct p4_device *p4, const u8 *old_buf, const u8 *new_buf,
		  size_t scan_start, size_t scan_end, bool new_frame);

/* Clear screen (full-screen RLE of zeros with new_frame flag) */
bool p4_send_clear(struct p4_device *p4);

/* Check if sender is enabled (not in powerdown/suspend) */
bool p4_sender_is_enabled(struct p4_device *p4);

/* Check if device is online (connected and initialized) */
bool p4_sender_is_online(struct p4_device *p4);

/* Check if READY GPIO is high (can send) */
bool p4_sender_is_ready(struct p4_device *p4);

/* DRM lifecycle enable/disable */
void p4_sender_enable(struct p4_device *p4);
void p4_sender_disable(struct p4_device *p4);

/*
 * Signal VSYNC - wakes drainer to process.
 * hw=true: real hardware VSYNC, can send frame data
 * hw=false: backup timer, can only send flags
 */
void p4_sender_signal_vsync(struct p4_device *p4, bool hw);

#endif /* _P4_SENDER_H */
