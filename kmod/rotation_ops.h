/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ROTATION_OPS_H
#define _ROTATION_OPS_H

/*
 * rotation_ops.h - Low-level rotation primitives
 *
 * This header is designed to work in three contexts:
 *   1. Kernel module - include types.h first (provides u8, u64)
 *   2. Userspace - include types.h first (provides u8, u64 via stdint)
 *   3. NEON code (-ffreestanding) - define u8, u64 before including
 *
 * The caller must ensure u8 and u64 types are defined before including
 * this header.
 */

#include "bitrev.h"

/*
 * gather8 - Gather 8 strided bytes into a u64, bit-reversing each byte
 *
 * Collects 8 bytes separated by 'stride' bytes, reverses bits in each,
 * and packs them into a u64 (first byte in MSB position).
 */
static inline u64 gather8_rot(const u8 *s, int stride)
{
	return ((u64)BITREV8(s[0*stride]) << 56) |
	       ((u64)BITREV8(s[1*stride]) << 48) |
	       ((u64)BITREV8(s[2*stride]) << 40) |
	       ((u64)BITREV8(s[3*stride]) << 32) |
	       ((u64)BITREV8(s[4*stride]) << 24) |
	       ((u64)BITREV8(s[5*stride]) << 16) |
	       ((u64)BITREV8(s[6*stride]) <<  8) |
	       ((u64)BITREV8(s[7*stride]));
}

/*
 * scatter8 - Scatter a u64 to 8 strided bytes
 *
 * Unpacks a u64 into 8 bytes separated by 'stride' bytes
 * (MSB position to first byte).
 */
static inline void scatter8_rot(u8 *d, int stride, u64 x)
{
	d[0*stride] = x >> 56;
	d[1*stride] = x >> 48;
	d[2*stride] = x >> 40;
	d[3*stride] = x >> 32;
	d[4*stride] = x >> 24;
	d[5*stride] = x >> 16;
	d[6*stride] = x >>  8;
	d[7*stride] = x;
}

/*
 * transpose8x8 - Transpose an 8x8 bit matrix packed in a u64
 *
 * Uses the standard three-stage delta swap algorithm.
 * Input/output: each byte is a row, bit 7 is column 0.
 */
static inline u64 transpose8x8_rot(u64 x)
{
	u64 t;
	t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ULL;
	x = x ^ t ^ (t << 28);
	t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCULL;
	x = x ^ t ^ (t << 14);
	t = (x ^ (x >>  7)) & 0x00AA00AA00AA00AAULL;
	x = x ^ t ^ (t <<  7);
	return x;
}

#endif /* _ROTATION_OPS_H */
