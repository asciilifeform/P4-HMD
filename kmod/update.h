/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UPDATE_H
#define _UPDATE_H

#include "types.h"
#include "bitrev.h"

/*
 * Callback invoked for each differing region found.
 * start: byte offset where difference begins
 * end: byte offset where difference ends (exclusive)
 * ctx: user context pointer
 * Returns: true to continue scanning, false to stop
 */
typedef bool (*diff_cb)(size_t start, size_t end, void *ctx);

/*
 * Callback invoked for each run found (RLE or literal).
 * For RLE: data points to single byte, len is repeat count
 * For literal: data points to bytes, len is byte count
 * is_rle: true if RLE run, false if literal
 * ctx: user context pointer
 * Returns: true to continue scanning, false to stop
 */
typedef bool (*rle_cb)(size_t offset, const u8 *data, size_t len, bool is_rle, void *ctx);

/*
 * Find differing regions between old and new buffers.
 * Calls cb for each contiguous region of differences.
 * min_gap: minimum unchanged bytes to split regions (header overhead)
 * Returns: true if completed, false if callback stopped early
 */
bool find_diffs(const u8 *old_buf, const u8 *new_buf, size_t len,
		size_t min_gap, diff_cb cb, void *ctx);

/*
 * Find RLE runs and literal spans in buffer.
 * Calls cb for each run or literal span found.
 * min_run: minimum run length to use RLE
 * Returns: true if completed, false if callback stopped early
 */
bool find_rle(const u8 *buf, size_t len, size_t min_run, rle_cb cb, void *ctx);

/* ===== Packet encoder ===== */

#define RLE_MIN_RUN      8

/*
 * Command byte bitfield layout (bits numbered from LSB):
 *   7: rle       - payload is RLE-encoded (single byte repeated len times)
 *   6: new_frame - first packet of a new frame
 *   5: bitrev    - FPGA should bit-reverse payload bytes
 *   4..3: reserved (zero)
 *   2..1: cmd_len - number of bytes following cmd before data (0, 1, or 3)
 *   0: always 1
 *
 * Packet types (cmd_len values):
 *   0 = reserved (not used)
 *   1 = flags-only packet (flags byte only, no data)
 *   3 = data packet (flags + addr_hi + addr_lo before len + data)
 *
 * Note: C bitfield ordering is implementation-defined. This layout assumes
 * GCC on little-endian (ARM, x86) which allocates LSB to MSB. Bit positions
 * are verified at runtime by test_cmd_byte_bitfield().
 */
struct cmd_byte {
	u8 _one : 1;		/* Bit 0: always 1 */
	u8 cmd_len : 2;		/* Bits 2..1: bytes following cmd before data */
	u8 _reserved : 2;	/* Bits 4..3: reserved (zero) */
	u8 bitrev : 1;		/* Bit 5: reverse bits in payload */
	u8 new_frame : 1;	/* Bit 6: first packet of frame */
	u8 rle : 1;		/* Bit 7: RLE encoded data */
};

union cmd_byte_u {
	struct cmd_byte f;
	u8 byte;
};

/* Command lengths (bytes following cmd before data payload) */
#define CMD_LEN_FLAGS_ONLY	1	/* flags byte only */
#define CMD_LEN_DATA		3	/* flags + addr_hi + addr_lo */

/* Header sizes (cmd byte + following bytes before data payload) */
#define HDR_SIZE_FLAGS_ONLY	2	/* cmd + flags */
#define HDR_SIZE_DATA		6	/* cmd + flags + addr + len */

/*
 * Display flags bitfield - sent with every packet to control display state.
 * Bits 3..0 are reserved (must be zero).
 *
 * Note: standby and blank both pause VSYNC signals from the display.
 * READY signals continue (drainer unaffected), but hardware vblank stops.
 *
 * Bit layout (MSB to LSB):
 *   7: upside_down   - 180° rotation
 *   6: standby       - display off (deep sleep, pauses vsync)
 *   5: blank         - screen blank (fast resume, pauses vsync)
 *   4: low_intensity - half brightness
 *   3..0: reserved (zero)
 *
 * Note: C bitfield ordering is implementation-defined. This layout assumes
 * GCC on little-endian (ARM, x86) which allocates LSB to MSB. Bit positions
 * are verified at runtime by test_encoder_display_flags().
 */
struct display_flags {
	u8 _reserved : 4;	/* Bits 3..0 */
	u8 low_intensity : 1;	/* Bit 4 */
	u8 blank : 1;		/* Bit 5 */
	u8 standby : 1;		/* Bit 6 */
	u8 upside_down : 1;	/* Bit 7 */
};

/* For direct byte access */
union display_flags_u {
	struct display_flags f;
	u8 byte;
};

/*
 * Packet header with flexible payload layouts.
 *
 * Wire format varies by cmd_len (bits 2..1 of cmd byte):
 *   cmd_len=1 (flags-only): cmd=0x03 | flags
 *   cmd_len=3 (data):       cmd=0x07 | flags | addr_hi | addr_lo | len_lo | len_hi | data...
 *
 * Cmd byte bit 0 is always 1. Flags (rle, new_frame, bitrev) OR'd in.
 * Length encoding: wire_len = len << 1 (LSB of len_lo is always 0).
 * Maximum length: 32767 bytes.
 *
 * The size/data_len/data fields are metadata (not transmitted).
 * The wire union contains the actual bytes to send.
 */
struct packet_header {
	size_t size;		/* Header size (not transmitted) */
	size_t data_len;	/* Payload size (not transmitted) */
	const u8 *data;		/* Payload pointer (not transmitted) */

	/* Wire format - use appropriate variant based on cmd_len */
	union {
		/* For accessing cmd byte in all variants */
		union cmd_byte_u cmd;

		/* cmd_len=1: flags-only */
		struct {
			union cmd_byte_u cmd;
			u8 flags;
		} flags_only;

		/* cmd_len=3: full data packet */
		struct {
			union cmd_byte_u cmd;
			u8 flags;
			u8 addr_hi;
			u8 addr_lo;
			u8 len_lo;
			u8 len_hi;
		} data;
	} wire;
};

/*
 * Initialize a flags-only packet header.
 */
static inline void pkt_init_flags_only(struct packet_header *pkt, u8 display_flags)
{
	memset(pkt, 0, sizeof(*pkt));
	pkt->wire.flags_only.cmd.f._one = 1;
	pkt->wire.flags_only.cmd.f.cmd_len = CMD_LEN_FLAGS_ONLY;
	pkt->wire.flags_only.flags = display_flags;
	pkt->size = HDR_SIZE_FLAGS_ONLY;
	/* data and data_len are zero from memset */
}

/*
 * Initialize a data packet header.
 * Length is shifted left by 1 bit on wire (LSB of len_lo must be 0).
 * Max length: 32767 bytes.
 */
static inline void pkt_init_data(struct packet_header *pkt, u8 display_flags,
				 bool new_frame, bool is_rle, bool bitrev,
				 u16 addr, u16 len)
{
	u16 wire_len = len << 1;
	memset(pkt, 0, sizeof(*pkt));
	pkt->wire.data.cmd.f._one = 1;
	pkt->wire.data.cmd.f.cmd_len = CMD_LEN_DATA;
	pkt->wire.data.cmd.f.new_frame = new_frame ? 1 : 0;
	pkt->wire.data.cmd.f.rle = is_rle ? 1 : 0;
	pkt->wire.data.cmd.f.bitrev = bitrev ? 1 : 0;
	pkt->wire.data.flags = display_flags;
	pkt->wire.data.addr_hi = (addr >> 8) & 0x7F;
	pkt->wire.data.addr_lo = addr & 0xFF;
	pkt->wire.data.len_lo = wire_len & 0xFF;
	pkt->wire.data.len_hi = (wire_len >> 8) & 0xFF;
	pkt->size = HDR_SIZE_DATA;
	/* data and data_len are zero from memset, caller sets if needed */
}

/*
 * Callback to emit a packet.
 * pkt: complete packet descriptor with header and data pointer
 * ctx: user context pointer
 * Returns: true on success, false to abort encoding
 */
typedef bool (*packet_emit_fn)(const struct packet_header *pkt, void *ctx);

/*
 * Encoder state - tracks position and flags during packet generation.
 * Used by both driver and tests.
 */
struct encoder_state {
	u16 addr;		/* Current address (updated as packets emitted) */
	u8 flags;		/* Display flags byte */
	bool new_frame;		/* Set FLAG_NEW_FRAME on next packet */
	bool bitrev;		/* Set bitrev flag in cmd_byte (FPGA reverses payload bits) */
	packet_emit_fn emit;	/* Packet emission callback */
	void *emit_ctx;		/* Context for emit callback */
};

/*
 * Initialize encoder state.
 */
static inline void encoder_init(struct encoder_state *enc, u8 flags,
				bool new_frame, bool bitrev,
				packet_emit_fn emit, void *ctx)
{
	enc->addr = 0;
	enc->flags = flags;
	enc->new_frame = new_frame;
	enc->bitrev = bitrev;
	enc->emit = emit;
	enc->emit_ctx = ctx;
}

/*
 * Encode a region of data into packets (RLE + literal).
 * Updates enc->addr and enc->new_frame.
 * Returns: true if all packets emitted, false if emit callback failed
 */
bool encode_region(struct encoder_state *enc, u16 addr, const u8 *buf, size_t len);

/*
 * Encode differences between old and new buffers.
 * scan_start/scan_end limit the range to scan.
 * Returns: true if all packets emitted, false if emit callback failed
 */
bool encode_diff(struct encoder_state *enc, const u8 *old_buf, const u8 *new_buf,
		 size_t scan_start, size_t scan_end);

/* ===== Header parsing helpers (for tests and debugging) ===== */

/* Packet metadata accessors */
static inline size_t pkt_hdr_size(const struct packet_header *p) { return p->size; }
static inline size_t pkt_data_len(const struct packet_header *p) { return p->data_len; }
static inline const u8 *pkt_data(const struct packet_header *p) { return p->data; }
static inline const void *pkt_wire(const struct packet_header *p) { return &p->wire; }

/* Extract fields from packet header struct */
static inline u8 pkt_cmd(const struct packet_header *p) { return p->wire.cmd.byte; }

/* Flags - only valid for flags_only and data packets */
static inline u8 pkt_flags(const struct packet_header *p) {
	/* flags_only.flags and data.flags are at the same wire offset (byte 1) */
	if (p->wire.cmd.f.cmd_len == CMD_LEN_FLAGS_ONLY)
		return p->wire.flags_only.flags;
	return p->wire.data.flags;
}

/* Address - only valid for data packets */
static inline u16 pkt_addr(const struct packet_header *p) {
	return ((u16)(p->wire.data.addr_hi & 0x7F) << 8) | p->wire.data.addr_lo;
}

/* Length - valid for data packets.
 * Wire format has length shifted left by 1, so we shift right to decode. */
static inline u16 pkt_len(const struct packet_header *p) {
	u16 wire_len = p->wire.data.len_lo | ((u16)p->wire.data.len_hi << 8);
	return wire_len >> 1;
}

/* Test header type */
static inline bool pkt_is_flags_only(const struct packet_header *p) {
	return p->wire.cmd.f.cmd_len == CMD_LEN_FLAGS_ONLY;
}
static inline bool pkt_is_data(const struct packet_header *p) {
	return p->wire.cmd.f.cmd_len == CMD_LEN_DATA;
}
static inline bool pkt_is_rle(const struct packet_header *p) {
	return p->wire.cmd.f.rle != 0;
}
static inline bool pkt_is_new_frame(const struct packet_header *p) {
	return p->wire.cmd.f.new_frame != 0;
}
static inline bool pkt_is_bitrev(const struct packet_header *p) {
	return p->wire.cmd.f.bitrev != 0;
}

#endif /* _UPDATE_H */
