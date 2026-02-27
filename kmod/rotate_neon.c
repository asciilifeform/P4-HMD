// SPDX-License-Identifier: GPL-2.0
/*
 * rotate_neon.c - NEON-accelerated 1bpp rotation for P4 display driver
 *
 * Called from rotation.c within kernel_neon_begin/end().
 *
 * This file is compiled with -ffreestanding to avoid kernel header
 * conflicts with arm_neon.h. Types are defined here before including
 * rotation_ops.h which provides the common primitives.
 */

#include <arm_neon.h>

/* Define types for freestanding context (before rotation_ops.h) */
typedef unsigned char      u8;
typedef unsigned long long u64;

#include "rotation_ops.h"

/* Prototype (defined in rotation.h, but we can't include it here) */
void rotate_ccw_neon(const u8 *src, u8 *dst,
		     int src_stride, int dst_stride, int blocks_x,
		     int bx_start, int bx_end, int by_start, int by_end);

/*
 * rotate_ccw_neon - NEON 90° CCW rotation for 1bpp framebuffer
 *
 * Processes 8x8 blocks in the specified range. Uses 128-bit NEON to
 * process 2 blocks in parallel when possible.
 */
void rotate_ccw_neon(const u8 *src, u8 *dst,
		     int src_stride, int dst_stride, int blocks_x,
		     int bx_start, int bx_end, int by_start, int by_end)
{
	const uint64x2_t mask4 = vdupq_n_u64(0x00000000F0F0F0F0ULL);
	const uint64x2_t mask2 = vdupq_n_u64(0x0000CCCC0000CCCCULL);
	const uint64x2_t mask1 = vdupq_n_u64(0x00AA00AA00AA00AAULL);
	int bx, by;

	for (bx = bx_start; bx < bx_end; bx++) {
		const u8 *src_col = src + bx;
		u8 *dst_base = dst + (blocks_x - 1 - bx) * 8 * dst_stride;

		/* Process 2 blocks at a time */
		for (by = by_start; by + 2 <= by_end; by += 2) {
			const u8 *s0 = src_col + (by + 0) * 8 * src_stride;
			const u8 *s1 = src_col + (by + 1) * 8 * src_stride;
			uint64x2_t x, t;
			uint8x16_t bytes;

			x = vcombine_u64(vcreate_u64(gather8_rot(s0, src_stride)),
					 vcreate_u64(gather8_rot(s1, src_stride)));

			/* Parallel transpose */
			t = vandq_u64(veorq_u64(x, vshrq_n_u64(x, 28)), mask4);
			x = veorq_u64(veorq_u64(x, t), vshlq_n_u64(t, 28));
			t = vandq_u64(veorq_u64(x, vshrq_n_u64(x, 14)), mask2);
			x = veorq_u64(veorq_u64(x, t), vshlq_n_u64(t, 14));
			t = vandq_u64(veorq_u64(x, vshrq_n_u64(x, 7)), mask1);
			x = veorq_u64(veorq_u64(x, t), vshlq_n_u64(t, 7));

			/* Byte-reverse for CCW */
			bytes = vreinterpretq_u8_u64(x);
			bytes = vrev64q_u8(bytes);
			x = vreinterpretq_u64_u8(bytes);

			scatter8_rot(dst_base + (by + 0), dst_stride, vgetq_lane_u64(x, 0));
			scatter8_rot(dst_base + (by + 1), dst_stride, vgetq_lane_u64(x, 1));
		}

		/* Handle odd block */
		for (; by < by_end; by++) {
			const u8 *s = src_col + by * 8 * src_stride;
			uint64x1_t v;
			uint8x8_t b;
			u64 val;

			val = gather8_rot(s, src_stride);
			val = transpose8x8_rot(val);

			v = vcreate_u64(val);
			b = vreinterpret_u8_u64(v);
			b = vrev64_u8(b);
			v = vreinterpret_u64_u8(b);
			val = vget_lane_u64(v, 0);

			scatter8_rot(dst_base + by, dst_stride, val);
		}
	}
}
