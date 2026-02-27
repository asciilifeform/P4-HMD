/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ROTATION_H
#define _ROTATION_H

#include "types.h"

void rotate_ccw_scalar(const u8 *src, u8 *dst, int src_stride, int dst_stride,
		       int blocks_x, int bx0, int bx1, int by0, int by1);

void rotate_ccw_neon(const u8 *src, u8 *dst, int src_stride, int dst_stride,
		     int blocks_x, int bx0, int bx1, int by0, int by1);

void rotate_blocks(const u8 *src, u8 *dst, int src_stride, int dst_stride,
		   int blocks_x, int bx0, int bx1, int by0, int by1);

#endif /* _ROTATION_H */
