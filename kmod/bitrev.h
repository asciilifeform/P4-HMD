/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BITREV_H
#define _BITREV_H

/*
 * bitrev.h - Bit reversal primitive
 *
 * Works in kernel, userspace, and freestanding (-ffreestanding) contexts.
 * Caller must ensure an 8-bit unsigned type is available as the argument.
 */

/*
 * BITREV8 - Reverse bits in a byte
 *
 * Converts between LSB-first and MSB-first bit ordering.
 * Implemented as a macro to work with any 8-bit type (u8, uint8_t, unsigned char).
 */
#define BITREV8(x) ({ \
	unsigned char _x = (x); \
	_x = ((_x & 0xAA) >> 1) | ((_x & 0x55) << 1); \
	_x = ((_x & 0xCC) >> 2) | ((_x & 0x33) << 2); \
	_x = ((_x & 0xF0) >> 4) | ((_x & 0x0F) << 4); \
	_x; \
})

#endif /* _BITREV_H */
