// SPDX-License-Identifier: GPL-2.0
/*
 * Rotation functions for P4 display driver
 * 90° counter-clockwise rotation of 8x8 bit blocks
 */

#include "types.h"
#include "rotation.h"
#include "rotation_ops.h"

#if defined(__KERNEL__)
  #if defined(CONFIG_KERNEL_MODE_NEON) && (defined(CONFIG_ARM64) || defined(CONFIG_ARM))
    #include <asm/neon.h>
    #include <asm/simd.h>
    #define HAVE_NEON 1
  #else
    #define HAVE_NEON 0
  #endif
#else
  /* Userspace: enable NEON on ARM64/ARM */
  #if defined(__aarch64__) || defined(__arm__)
    #define HAVE_NEON 1
    /* Stub out kernel NEON guards for userspace */
    static inline int may_use_simd(void) { return 1; }
    static inline void kernel_neon_begin(void) {}
    static inline void kernel_neon_end(void) {}
  #else
    #define HAVE_NEON 0
  #endif
#endif

void rotate_ccw_scalar(const u8 *src, u8 *dst, int src_stride, int dst_stride,
		       int blocks_x, int bx0, int bx1, int by0, int by1)
{
	for (int bx = bx0; bx < bx1; bx++) {
		const u8 *col = src + bx;
		u8 *base = dst + (blocks_x - 1 - bx) * 8 * dst_stride;
		for (int by = by0; by < by1; by++) {
			u64 x = gather8_rot(col + by * 8 * src_stride, src_stride);
			x = transpose8x8_rot(x);
			scatter8_rot(base + by, dst_stride, __builtin_bswap64(x));
		}
	}
}

#if HAVE_NEON
void rotate_ccw_neon(const u8 *src, u8 *dst, int src_stride, int dst_stride,
		     int blocks_x, int bx0, int bx1, int by0, int by1);
#endif

void rotate_blocks(const u8 *src, u8 *dst, int src_stride, int dst_stride,
		   int blocks_x, int bx0, int bx1, int by0, int by1)
{
#if HAVE_NEON
	if (may_use_simd()) {
		kernel_neon_begin();
		rotate_ccw_neon(src, dst, src_stride, dst_stride, blocks_x,
				bx0, bx1, by0, by1);
		kernel_neon_end();
		return;
	}
#endif
	rotate_ccw_scalar(src, dst, src_stride, dst_stride, blocks_x,
			  bx0, bx1, by0, by1);
}
