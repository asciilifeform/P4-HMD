/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _P4_TYPES_H
#define _P4_TYPES_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint64_t u64;
#endif

#endif /* _P4_TYPES_H */
