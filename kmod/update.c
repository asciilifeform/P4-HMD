// SPDX-License-Identifier: GPL-2.0
/*
 * P4 display driver - differential update encoding
 * Finds changed regions (diff) and RLE-compresses them for efficient SPI transfer.
 */

#include "update.h"
#include "display.h"  /* For FB_SIZE */

/*
 * Verify struct layouts are correct.
 *
 * Bitfield ordering is implementation-defined. GCC on little-endian
 * (ARM, x86) allocates from LSB to MSB within each byte. The runtime
 * tests (test_cmd_byte_bitfield, test_encoder_display_flags) verify
 * the actual bit positions match our expectations:
 *
 *   cmd_byte:      bit 7=rle, bit 6=new_frame, bit 5=bitrev, bits 2..1=cmd_len, bit 0=1
 *   display_flags: bit 7=upside_down, bit 6=standby, bit 5=blank, bit 4=low_intensity
 */

/* Verify bitfield structs are single bytes */
_Static_assert(sizeof(struct cmd_byte) == 1, "cmd_byte must be 1 byte");
_Static_assert(sizeof(union cmd_byte_u) == 1, "cmd_byte_u must be 1 byte");
_Static_assert(sizeof(struct display_flags) == 1, "display_flags must be 1 byte");
_Static_assert(sizeof(union display_flags_u) == 1, "display_flags_u must be 1 byte");

/* Verify packet_header wire fields are contiguous (no padding) */
_Static_assert(
	offsetof(struct packet_header, wire.data.len_hi) -
	offsetof(struct packet_header, wire.data.cmd) + 1 == HDR_SIZE_DATA,
	"data packet wire fields must be contiguous");

_Static_assert(
	offsetof(struct packet_header, wire.flags_only.flags) -
	offsetof(struct packet_header, wire.flags_only.cmd) + 1 == HDR_SIZE_FLAGS_ONLY,
	"flags_only packet wire fields must be contiguous");

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LZB(W) (__builtin_ctzll(W) >> 3)
#define TZB(W) (__builtin_clzll(W) >> 3)
#define LAST_BYTE(W) ((W) >> 56)
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define LZB(W) (__builtin_clzll(W) >> 3)
#define TZB(W) (__builtin_ctzll(W) >> 3)
#define LAST_BYTE(W) ((u8)(W))
#else
#error "Unknown byte order"
#endif

#define SPREAD(b) ((u64)(b) * 0x0101010101010101ULL)

bool find_diffs(const u8 *old_buf, const u8 *new_buf, size_t len,
		size_t min_gap, diff_cb cb, void *ctx)
{
	const size_t words = len >> 3;
	const u64 *old_w = (const u64 *)old_buf;
	const u64 *new_w = (const u64 *)new_buf;

	for (size_t i = 0; i < words; ) {
		u64 xor;
		while (i < words && (xor = old_w[i] ^ new_w[i]) == 0)
			i++;
		if (i >= words)
			break;

		size_t start = i * 8 + LZB(xor);
		u8 trailing = TZB(xor);
		i++;

		while (i < words) {
			xor = old_w[i] ^ new_w[i];
			if (xor == 0 || (size_t)(trailing + LZB(xor)) >= min_gap)
				break;
			trailing = TZB(xor);
			i++;
		}

		if (!cb(start, i * 8 - trailing, ctx))
			return false;
	}
	return true;
}

/*
 * Find ALL runs >= min_run bytes.
 *
 * Key insight: A run of 8+ bytes at arbitrary position must either:
 * 1. Fully contain an aligned 8-byte word (run of 15+ bytes), OR
 * 2. Span at least one word boundary with enough bytes on each side
 *
 * Strategy: For each word, check if bytes at the end (trailing) could be
 * part of a run extending into subsequent words. Use LZB/TZB to count
 * matching bytes at word boundaries. Byte-level scanning only for the
 * tail (0-7 bytes past the last full word).
 */
bool find_rle(const u8 *buf, size_t len, size_t min_run, rle_cb cb, void *ctx)
{
	if (len == 0)
		return true;

	if (len < 8) {
		/* Too small for word operations - just emit as literal */
		return cb(0, buf, len, false, ctx);
	}

	const u64 *wbuf = (const u64 *)buf;
	const size_t words = len >> 3;
	const size_t tail = len & 7;

	size_t lit_start = 0;
	size_t w = 0;

	while (w < words) {
		u64 word = wbuf[w];
		
		/*
		 * Get the LAST byte of this word (highest memory address).
		 * This is the byte at offset (w+1)*8 - 1.
		 */
		u8 byte = LAST_BYTE(word);
		u64 spread = SPREAD(byte);
		u64 xored = word ^ spread;

		/*
		 * Count TRAILING matching bytes (from high address down).
		 * TZB counts bytes at high addresses that are zero in xored.
		 */
		int trailing = xored ? TZB(xored) : 8;

		if (trailing == 0) {
			/* Last byte differs - no run can end here */
			w++;
			continue;
		}

		/*
		 * We have 'trailing' bytes at the END of word w that match 'byte'.
		 * Compute run_start. Only extend backwards if the ENTIRE current
		 * word matches (trailing == 8), meaning the run extends contiguously
		 * from the previous word.
		 */
		size_t run_start = (w + 1) * 8 - trailing;
		
		if (trailing == 8 && w > 0 && run_start > lit_start) {
			/* Current word is all 'byte'. Check previous word. */
			u64 prev_xor = wbuf[w - 1] ^ spread;
			int prev_match = prev_xor ? TZB(prev_xor) : 8;
			if (prev_match > 0) {
				size_t extended_start = w * 8 - prev_match;
				if (extended_start >= lit_start)
					run_start = extended_start;
			}
		}

		/*
		 * Extend forwards: count leading bytes of subsequent words.
		 */
		size_t run_end;
		size_t wend = w + 1;
		
		while (wend < words) {
			u64 next_xor = wbuf[wend] ^ spread;
			if (next_xor == 0) {
				wend++;
			} else {
				/* Count leading matching bytes (from low address) */
				int leading = LZB(next_xor);
				run_end = wend * 8 + leading;
				goto have_run_end;
			}
		}
		
		/* Ran through all words - run_end is at last word boundary */
		run_end = words * 8;
		
		/* Check tail bytes (up to 7 bytes past last word) */
		if (tail > 0) {
			const u8 *tail_ptr = buf + words * 8;
			size_t tail_match = 0;
			while (tail_match < tail && tail_ptr[tail_match] == byte)
				tail_match++;
			run_end += tail_match;
		}

	have_run_end:;
		size_t run_len = run_end - run_start;

		if (run_len >= min_run) {
			/* Emit literal before run */
			if (run_start > lit_start) {
				if (!cb(lit_start, buf + lit_start, run_start - lit_start, false, ctx))
					return false;
			}
			/* Emit RLE run */
			if (!cb(run_start, &byte, run_len, true, ctx))
				return false;
			lit_start = run_end;

			/* Skip to word containing or after run_end */
			w = run_end >> 3;
			if (w <= (run_start >> 3))
				w++;
		} else {
			w++;
		}
	}

	/* Emit any remaining literal */
	if (lit_start < len) {
		if (!cb(lit_start, buf + lit_start, len - lit_start, false, ctx))
			return false;
	}

	return true;
}

/* ===== Packet encoder ===== */

/* Emit a single packet (RLE or literal) */
static bool emit_packet(struct encoder_state *enc, bool rle, const u8 *data, u16 len)
{
	struct packet_header pkt;

	/*
	 * Always use data packet format with explicit address.
	 * The PE does not reset address automatically - it keeps the last
	 * address used. So we must always send explicit addr, even for addr=0.
	 *
	 * Flags are managed separately via flags-only packets: when flags
	 * change, the FIFO drains first, then a flags-only packet is sent,
	 * then normal updates resume. All subsequent packets (of any type
	 * that include a flags field) will contain the updated flags, since
	 * the encoder reads the current flags at encode time. This guarantees
	 * correct ordering without needing explicit flags in every packet.
	 */
	pkt_init_data(&pkt, enc->flags, enc->new_frame, rle, enc->bitrev,
		      enc->addr, len);
	pkt.data = data;
	pkt.data_len = rle ? 1 : len;

	if (!enc->emit(&pkt, enc->emit_ctx))
		return false;

	enc->new_frame = false;
	enc->addr += len;
	return true;
}

/* Callback for find_rle */
static bool rle_emit_cb(size_t offset, const u8 *data, size_t len, bool is_rle, void *ctx)
{
	(void)offset;
	return emit_packet(ctx, is_rle, data, len);
}

bool encode_region(struct encoder_state *enc, u16 addr, const u8 *buf, size_t len)
{
	/* Sanity check: addr + len must not overflow FB_SIZE */
	if ((size_t)addr + len > FB_SIZE) {
		/* This is a bug in the caller - should never happen */
		return false;
	}
	enc->addr = addr;
	return find_rle(buf, len, RLE_MIN_RUN, rle_emit_cb, enc);
}

/* Context for diff callback */
struct diff_enc_ctx {
	struct encoder_state *enc;
	const u8 *new_buf;
	size_t base;
};

/* Callback for find_diffs */
static bool diff_emit_cb(size_t start, size_t end, void *ctx)
{
	struct diff_enc_ctx *d = ctx;
	size_t abs_start = d->base + start;
	size_t abs_end = d->base + end;
	size_t len = abs_end - abs_start;

	/* Check for overflow before truncating to u16 */
	if (abs_start > 0xFFFF || abs_end > FB_SIZE || abs_start + len > FB_SIZE) {
		/* Bug: diff found region outside valid range */
		return false;
	}

	return encode_region(d->enc, (u16)abs_start, d->new_buf + abs_start, len);
}

bool encode_diff(struct encoder_state *enc, const u8 *old_buf, const u8 *new_buf,
		 size_t scan_start, size_t scan_end)
{
	/* Align to word boundaries */
	scan_start &= ~7UL;
	scan_end = (scan_end + 7) & ~7UL;

	struct diff_enc_ctx ctx = {
		.enc = enc,
		.new_buf = new_buf,
		.base = scan_start,
	};

	return find_diffs(old_buf + scan_start, new_buf + scan_start,
			  scan_end - scan_start, HDR_SIZE_DATA, diff_emit_cb, &ctx);
}
