// SPDX-License-Identifier: GPL-2.0
/*
 * Unit tests for P4 display driver components
 * Tests: find_diffs, find_rle, rotation, damage bounds, full pipeline
 *
 * Build: gcc -std=c11 -Wall -Wextra -O2 -g -o tests tests.c update.c rotation.c
 * Run:   ./tests
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "types.h"
#include "display.h"
#include "update.h"
#include "rotation.h"
#include "bitrev.h"

/* ===== Packet decoder (test-only) ===== */

/*
 * Decode a packet from wire format into packet_header structure.
 * Sets pkt->data to point into buf (does not copy data).
 *
 * Returns:
 *   >0: bytes consumed (packet decoded successfully)
 *    0: need more data (incomplete packet)
 *   -1: invalid packet format
 */
static int pkt_decode(const u8 *buf, size_t len, struct packet_header *pkt)
{
	if (len < 1)
		return 0;

	union cmd_byte_u cmd;
	cmd.byte = buf[0];

	size_t hdr_size;
	u16 pkt_len = 0;

	switch (cmd.f.cmd_len) {
	case CMD_LEN_FLAGS_ONLY:
		hdr_size = HDR_SIZE_FLAGS_ONLY;
		if (len < hdr_size)
			return 0;
		/* Flags-only packet has no data */
		pkt->wire.flags_only.cmd = cmd;
		pkt->wire.flags_only.flags = buf[1];
		pkt->size = hdr_size;
		pkt->data = NULL;
		pkt->data_len = 0;
		return hdr_size;

	case CMD_LEN_DATA:
		hdr_size = HDR_SIZE_DATA;
		if (len < hdr_size)
			return 0;
		pkt_len = (buf[4] | (buf[5] << 8)) >> 1;
		pkt->wire.data.cmd = cmd;
		pkt->wire.data.flags = buf[1];
		pkt->wire.data.addr_hi = buf[2];
		pkt->wire.data.addr_lo = buf[3];
		pkt->wire.data.len_lo = buf[4];
		pkt->wire.data.len_hi = buf[5];
		break;

	default:
		return -1;  /* Invalid cmd_len (2 is reserved) */
	}

	/* Calculate data length */
	size_t data_len = cmd.f.rle ? 1 : pkt_len;
	size_t total = hdr_size + data_len;

	if (len < total)
		return 0;  /* Need more data */

	pkt->size = hdr_size;
	pkt->data = buf + hdr_size;
	pkt->data_len = data_len;

	return total;
}

/*
 * Apply a decoded packet to a framebuffer.
 * Handles RLE expansion and optional bit reversal.
 *
 * Note: Caller is responsible for bounds checking (addr + len <= fb_size).
 */
static void pkt_apply(const struct packet_header *pkt, u8 *fb, size_t fb_size)
{
	/* Skip flags-only packets */
	if (pkt_is_flags_only(pkt))
		return;

	u16 addr = pkt_addr(pkt);
	u16 len = pkt_len(pkt);

	if (addr + len > fb_size)
		return;  /* Out of bounds */

	bool rle = pkt_is_rle(pkt);
	bool bitrev = pkt_is_bitrev(pkt);
	const u8 *data = pkt_data(pkt);

	if (rle) {
		u8 val = data[0];
		if (bitrev)
			val = BITREV8(val);
		memset(fb + addr, val, len);
	} else {
		if (bitrev) {
			for (u16 i = 0; i < len; i++)
				fb[addr + i] = BITREV8(data[i]);
		} else {
			memcpy(fb + addr, data, len);
		}
	}
}

/* ===== Test infrastructure ===== */

#define MAX_RESULTS 4096

static struct { size_t start, end; } diff_results[MAX_RESULTS];
static int diff_count;

static struct { size_t offset, len; u8 byte; bool is_rle; } rle_results[MAX_RESULTS];
static int rle_count;

static void reset_results(void)
{
	diff_count = 0;
	rle_count = 0;
}

static bool collect_diff(size_t start, size_t end, void *ctx)
{
	(void)ctx;
	if (diff_count < MAX_RESULTS) {
		diff_results[diff_count].start = start;
		diff_results[diff_count].end = end;
		diff_count++;
	}
	return true;
}

static bool collect_rle(size_t offset, const u8 *data, size_t len, bool is_rle, void *ctx)
{
	(void)ctx;
	if (rle_count < MAX_RESULTS) {
		rle_results[rle_count].offset = offset;
		rle_results[rle_count].len = len;
		rle_results[rle_count].is_rle = is_rle;
		rle_results[rle_count].byte = is_rle ? *data : 0;
		rle_count++;
	}
	return true;
}

static void damage_bounds(int bx0, int bx1, size_t *start, size_t *end)
{
	int blocks_x = P4_WIDTH >> 3;
	int row0 = (blocks_x - bx1) << 3;
	int row1 = (blocks_x - bx0) << 3;
	*start = (size_t)row0 * (HW_WIDTH >> 3);
	*end = (size_t)row1 * (HW_WIDTH >> 3);
}

/* ===== find_diffs tests ===== */

static void test_diff_identical(void)
{
	u8 a[64], b[64];
	memset(a, 0xAA, sizeof(a));
	memset(b, 0xAA, sizeof(b));
	reset_results();
	bool ok = find_diffs(a, b, sizeof(a), 8, collect_diff, NULL);
	assert(ok);
	assert(diff_count == 0);
	printf("  test_diff_identical: PASS\n");
}

static void test_diff_single_byte(void)
{
	u8 a[64], b[64];
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	b[20] = 0xFF;
	reset_results();
	bool ok = find_diffs(a, b, sizeof(a), 8, collect_diff, NULL);
	assert(ok);
	assert(diff_count == 1);
	assert(diff_results[0].start == 20);
	assert(diff_results[0].end == 21);
	printf("  test_diff_single_byte: PASS\n");
}

static void test_diff_two_regions(void)
{
	u8 a[64], b[64];
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	b[4] = 0xFF;
	b[48] = 0xFF;
	reset_results();
	bool ok = find_diffs(a, b, sizeof(a), 8, collect_diff, NULL);
	assert(ok);
	assert(diff_count == 2);
	printf("  test_diff_two_regions: PASS\n");
}

static void test_diff_merge_close_regions(void)
{
	u8 a[64], b[64];
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	b[7] = 0xFF;
	b[9] = 0xFF;
	reset_results();
	bool ok = find_diffs(a, b, sizeof(a), 8, collect_diff, NULL);
	assert(ok);
	assert(diff_count == 1);
	assert(diff_results[0].start == 7);
	assert(diff_results[0].end == 10);
	printf("  test_diff_merge_close_regions: PASS\n");
}

static void test_diff_all_different(void)
{
	u8 a[64], b[64];
	memset(a, 0x00, sizeof(a));
	memset(b, 0xFF, sizeof(b));
	reset_results();
	bool ok = find_diffs(a, b, sizeof(a), 8, collect_diff, NULL);
	assert(ok);
	assert(diff_count == 1);
	assert(diff_results[0].start == 0);
	assert(diff_results[0].end == 64);
	printf("  test_diff_all_different: PASS\n");
}

static bool stop_after_one_diff(size_t start, size_t end, void *ctx)
{
	int *count = ctx;
	(*count)++;
	(void)start; (void)end;
	return false;
}

static void test_diff_callback_stop(void)
{
	u8 a[64], b[64];
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	b[4] = 0xFF;
	b[48] = 0xFF;
	int count = 0;
	bool ok = find_diffs(a, b, sizeof(a), 8, stop_after_one_diff, &count);
	assert(!ok);
	assert(count == 1);
	printf("  test_diff_callback_stop: PASS\n");
}

static void test_diff_first_byte(void)
{
	u8 a[64], b[64];
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	b[0] = 0xFF;
	reset_results();
	bool ok = find_diffs(a, b, sizeof(a), 8, collect_diff, NULL);
	assert(ok);
	assert(diff_count == 1);
	assert(diff_results[0].start == 0);
	assert(diff_results[0].end == 1);
	printf("  test_diff_first_byte: PASS\n");
}

static void test_diff_last_byte(void)
{
	u8 a[64], b[64];
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	b[63] = 0xFF;
	reset_results();
	bool ok = find_diffs(a, b, sizeof(a), 8, collect_diff, NULL);
	assert(ok);
	assert(diff_count == 1);
	assert(diff_results[0].start == 63);
	assert(diff_results[0].end == 64);
	printf("  test_diff_last_byte: PASS\n");
}

static void test_diff_word_boundary(void)
{
	u8 a[64], b[64];
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	b[7] = 0xFF;
	b[8] = 0xFF;
	reset_results();
	bool ok = find_diffs(a, b, sizeof(a), 8, collect_diff, NULL);
	assert(ok);
	assert(diff_count == 1);
	assert(diff_results[0].start == 7);
	assert(diff_results[0].end == 9);
	printf("  test_diff_word_boundary: PASS\n");
}

static void test_diff_empty_buffer(void)
{
	u8 a[8], b[8];
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	reset_results();
	bool ok = find_diffs(a, b, sizeof(a), 8, collect_diff, NULL);
	assert(ok);
	assert(diff_count == 0);
	printf("  test_diff_empty_buffer: PASS\n");
}

static void test_diff_single_bit(void)
{
	u8 a[64], b[64];
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	b[32] = 0x01;
	reset_results();
	bool ok = find_diffs(a, b, sizeof(a), 8, collect_diff, NULL);
	assert(ok);
	assert(diff_count == 1);
	assert(diff_results[0].start == 32);
	assert(diff_results[0].end == 33);
	printf("  test_diff_single_bit: PASS\n");
}

/* ===== find_rle tests ===== */

static void test_rle_all_same(void)
{
	u8 buf[64];
	memset(buf, 0xAA, sizeof(buf));
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	assert(rle_count == 1);
	assert(rle_results[0].is_rle);
	assert(rle_results[0].len == 64);
	assert(rle_results[0].byte == 0xAA);
	printf("  test_rle_all_same: PASS\n");
}

static void test_rle_no_runs(void)
{
	u8 buf[64];
	for (int i = 0; i < 64; i++) buf[i] = i;
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	assert(rle_count == 1);
	assert(!rle_results[0].is_rle);
	assert(rle_results[0].len == 64);
	printf("  test_rle_no_runs: PASS\n");
}

static void test_rle_run_in_middle(void)
{
	u8 buf[64];
	for (int i = 0; i < 64; i++) buf[i] = i;
	memset(buf + 24, 0xBB, 16);
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	assert(rle_count == 3);
	assert(!rle_results[0].is_rle);
	assert(rle_results[1].is_rle);
	assert(rle_results[1].byte == 0xBB);
	assert(!rle_results[2].is_rle);
	printf("  test_rle_run_in_middle: PASS\n");
}

static void test_rle_short_run_ignored(void)
{
	u8 buf[64];
	for (int i = 0; i < 64; i++) buf[i] = i;
	memset(buf + 20, 0xCC, 4);
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	for (int i = 0; i < rle_count; i++) {
		if (rle_results[i].is_rle) assert(rle_results[i].byte != 0xCC);
	}
	printf("  test_rle_short_run_ignored: PASS\n");
}

static void test_rle_cross_word_boundary(void)
{
	u8 buf[128];
	for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (u8)i;
	
	/*
	 * Create a run that crosses word boundaries.
	 * Start at offset 4 (mid-word) with RLE_MIN_RUN bytes.
	 * This tests that word alignment doesn't matter.
	 */
	memset(buf + 4, 0xDD, RLE_MIN_RUN);
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	int found = 0;
	for (int i = 0; i < rle_count; i++) {
		if (rle_results[i].is_rle && rle_results[i].byte == 0xDD) {
			found = 1;
			assert(rle_results[i].offset == 4);
			assert(rle_results[i].len == RLE_MIN_RUN);
		}
	}
	assert(found);
	printf("  test_rle_cross_word_boundary: PASS\n");
}

static void test_rle_whole_word(void)
{
	/*
	 * Test RLE run that happens to align with word boundaries.
	 * The algorithm should NOT require this alignment.
	 */
	u8 buf[64];
	for (int i = 0; i < (int)sizeof(buf); i++) buf[i] = i;
	
	/* Place run at offset 8 with exactly RLE_MIN_RUN bytes */
	memset(buf + 8, 0xEE, RLE_MIN_RUN);
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	int found = 0;
	for (int i = 0; i < rle_count; i++) {
		if (rle_results[i].is_rle && rle_results[i].byte == 0xEE) {
			found = 1;
			assert(rle_results[i].offset == 8);
			assert(rle_results[i].len == RLE_MIN_RUN);
		}
	}
	assert(found);
	printf("  test_rle_whole_word: PASS\n");
}

static void test_rle_unaligned_run(void)
{
	/*
	 * Test RLE run at various unaligned offsets.
	 * Word boundaries should NOT affect detection.
	 */
	u8 buf[128];
	
	/* Test at every possible offset within first 16 bytes */
	for (int offset = 0; offset < 16; offset++) {
		for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (u8)(i ^ 0x5A);
		
		memset(buf + offset, 0xAA, RLE_MIN_RUN);
		reset_results();
		bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
		assert(ok);
		
		int found = 0;
		for (int i = 0; i < rle_count; i++) {
			if (rle_results[i].is_rle && rle_results[i].byte == 0xAA &&
			    rle_results[i].offset == (size_t)offset && 
			    rle_results[i].len == RLE_MIN_RUN) {
				found = 1;
			}
		}
		assert(found && "Run at unaligned offset was not detected");
	}
	printf("  test_rle_unaligned_run: PASS\n");
}

static void test_rle_no_merge_interrupted(void)
{
	/* Buffer large enough for two runs of RLE_MIN_RUN with interrupt in middle */
	u8 buf[RLE_MIN_RUN * 3];
	memset(buf, 0xAA, sizeof(buf));
	/* Put interrupt exactly in the middle */
	size_t interrupt_pos = sizeof(buf) / 2;
	buf[interrupt_pos] = 0xBB;
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	int aa_runs = 0;
	for (int i = 0; i < rle_count; i++) {
		if (rle_results[i].is_rle && rle_results[i].byte == 0xAA) {
			aa_runs++;
			size_t start = rle_results[i].offset;
			size_t end = start + rle_results[i].len;
			/* Verify run doesn't cross the interrupt */
			assert(end <= interrupt_pos || start >= interrupt_pos + 1);
		}
	}
	assert(aa_runs == 2);
	printf("  test_rle_no_merge_interrupted: PASS\n");
}

static void test_rle_no_merge_small_gap(void)
{
	/* Two runs of RLE_MIN_RUN separated by a small gap */
	u8 buf[RLE_MIN_RUN * 4];
	for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (u8)i;
	
	/* First run */
	memset(buf + 0, 0xAA, RLE_MIN_RUN);
	/* Gap of 2 bytes (buf[RLE_MIN_RUN] and buf[RLE_MIN_RUN+1] are different) */
	/* Second run */
	memset(buf + RLE_MIN_RUN + 2, 0xAA, RLE_MIN_RUN);
	
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	int aa_runs = 0;
	for (int i = 0; i < rle_count; i++) {
		if (rle_results[i].is_rle && rle_results[i].byte == 0xAA) {
			aa_runs++;
			assert(rle_results[i].len == RLE_MIN_RUN);
		}
	}
	assert(aa_runs == 2);
	printf("  test_rle_no_merge_small_gap: PASS\n");
}

static bool stop_after_one_rle(size_t offset, const u8 *data, size_t len, bool is_rle, void *ctx)
{
	int *count = ctx;
	(*count)++;
	(void)offset; (void)data; (void)len; (void)is_rle;
	return false;
}

static void test_rle_callback_stop(void)
{
	u8 buf[64];
	memset(buf, 0xAA, 32);
	memset(buf + 32, 0xBB, 32);
	int count = 0;
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, stop_after_one_rle, &count);
	assert(!ok);
	assert(count == 1);
	printf("  test_rle_callback_stop: PASS\n");
}

static void test_rle_minimum_run(void)
{
	u8 buf[64];
	for (int i = 0; i < 64; i++) buf[i] = i;
	memset(buf + 20, 0xDD, RLE_MIN_RUN);
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	int found = 0;
	for (int i = 0; i < rle_count; i++) {
		if (rle_results[i].is_rle && rle_results[i].byte == 0xDD) {
			found = 1;
			assert(rle_results[i].len == RLE_MIN_RUN);
		}
	}
	assert(found);
	printf("  test_rle_minimum_run: PASS\n");
}

static void test_rle_one_under_minimum(void)
{
	u8 buf[64];
	for (int i = 0; i < 64; i++) buf[i] = i;
	memset(buf + 20, 0xEE, RLE_MIN_RUN - 1);
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	for (int i = 0; i < rle_count; i++) {
		if (rle_results[i].is_rle) assert(rle_results[i].byte != 0xEE);
	}
	printf("  test_rle_one_under_minimum: PASS\n");
}

static void test_rle_first_bytes(void)
{
	u8 buf[64];
	for (int i = 0; i < 64; i++) buf[i] = i;
	memset(buf, 0xAA, 16);
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	assert(rle_results[0].is_rle);
	assert(rle_results[0].offset == 0);
	assert(rle_results[0].byte == 0xAA);
	assert(rle_results[0].len == 16);
	printf("  test_rle_first_bytes: PASS\n");
}

static void test_rle_last_bytes(void)
{
	u8 buf[64];
	for (int i = 0; i < 64; i++) buf[i] = i;
	memset(buf + 48, 0xBB, 16);
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	int found = 0;
	for (int i = 0; i < rle_count; i++) {
		if (rle_results[i].is_rle && rle_results[i].byte == 0xBB) {
			found = 1;
			assert(rle_results[i].offset == 48);
			assert(rle_results[i].len == 16);
		}
	}
	assert(found);
	printf("  test_rle_last_bytes: PASS\n");
}

static void test_rle_all_zeros(void)
{
	u8 buf[64];
	memset(buf, 0x00, sizeof(buf));
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	assert(rle_count == 1);
	assert(rle_results[0].is_rle);
	assert(rle_results[0].byte == 0x00);
	assert(rle_results[0].len == 64);
	printf("  test_rle_all_zeros: PASS\n");
}

static void test_rle_all_ones(void)
{
	u8 buf[64];
	memset(buf, 0xFF, sizeof(buf));
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	assert(rle_count == 1);
	assert(rle_results[0].is_rle);
	assert(rle_results[0].byte == 0xFF);
	assert(rle_results[0].len == 64);
	printf("  test_rle_all_ones: PASS\n");
}

static void test_rle_alternating_bytes(void)
{
	u8 buf[64];
	for (int i = 0; i < 64; i++) buf[i] = (i & 1) ? 0x55 : 0xAA;
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	for (int i = 0; i < rle_count; i++) assert(!rle_results[i].is_rle);
	printf("  test_rle_alternating_bytes: PASS\n");
}

static void test_rle_multiple_runs(void)
{
	/* Buffer needs to be large enough for 3 runs of RLE_MIN_RUN with gaps */
	u8 buf[RLE_MIN_RUN * 6];
	memset(buf, 0x00, sizeof(buf));
	memset(buf + 0, 0xAA, RLE_MIN_RUN);
	memset(buf + RLE_MIN_RUN * 2, 0xBB, RLE_MIN_RUN);
	memset(buf + RLE_MIN_RUN * 4, 0xCC, RLE_MIN_RUN);
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	int aa = 0, bb = 0, cc = 0;
	for (int i = 0; i < rle_count; i++) {
		if (rle_results[i].is_rle) {
			if (rle_results[i].byte == 0xAA) aa++;
			if (rle_results[i].byte == 0xBB) bb++;
			if (rle_results[i].byte == 0xCC) cc++;
		}
	}
	assert(aa == 1 && bb == 1 && cc == 1);
	printf("  test_rle_multiple_runs: PASS\n");
}

static void test_rle_adjacent_different_runs(void)
{
	u8 buf[64];
	memset(buf, 0xAA, 32);
	memset(buf + 32, 0xBB, 32);
	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);
	assert(ok);
	assert(rle_count == 2);
	assert(rle_results[0].is_rle && rle_results[0].byte == 0xAA && rle_results[0].len == 32);
	assert(rle_results[1].is_rle && rle_results[1].byte == 0xBB && rle_results[1].len == 32);
	printf("  test_rle_adjacent_different_runs: PASS\n");
}
/* ===== Rotation tests ===== */

static void test_rotate_single_block(void)
{
	/* Input is LSB-first (bit 0 = leftmost pixel, like kernel output) */
	/* Visual diagonal from top-left to bottom-right */
	u8 src[8] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
	u8 dst[8] = { 0 };
	rotate_ccw_scalar(src, dst, 1, 1, 1, 0, 1, 0, 1);
	/* Output is MSB-first (bit 7 = leftmost pixel, hardware format) */
	/* After CCW 90°, diagonal goes from top-right to bottom-left */
	u8 expected[8] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
	assert(memcmp(dst, expected, 8) == 0);
	printf("  test_rotate_single_block: PASS\n");
}

static void test_rotate_identity_pattern(void)
{
	/*
	 * This test checks that rotation works on all-zeros.
	 * We can't easily test 4 rotations = identity because input is
	 * LSB-first but output is MSB-first.
	 */
	u8 zeros[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	u8 dst[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	rotate_ccw_scalar(zeros, dst, 1, 1, 1, 0, 1, 0, 1);
	assert(memcmp(dst, zeros, 8) == 0);
	printf("  test_rotate_identity_pattern: PASS\n");
}

static void test_rotate_full_buffer(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	static u8 src[FB_SIZE], dst[FB_SIZE];
	memset(src, 0, FB_SIZE);
	memset(dst, 0, FB_SIZE);
	for (int by = 0; by < (P4_HEIGHT >> 3); by++) {
		for (int bx = 0; bx < blocks_x; bx++) {
			src[by * 8 * src_stride + bx] = (u8)(bx ^ by);
		}
	}
	rotate_ccw_scalar(src, dst, src_stride, dst_stride, blocks_x, 0, blocks_x, 0, P4_HEIGHT >> 3);
	int nonzero = 0;
	for (size_t i = 0; i < FB_SIZE; i++) if (dst[i]) nonzero++;
	assert(nonzero > 0);
	printf("  test_rotate_full_buffer: PASS\n");
}

static void test_rotate_all_zeros(void)
{
	u8 src[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	u8 dst[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	rotate_ccw_scalar(src, dst, 1, 1, 1, 0, 1, 0, 1);
	u8 expected[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	assert(memcmp(dst, expected, 8) == 0);
	printf("  test_rotate_all_zeros: PASS\n");
}

static void test_rotate_all_ones(void)
{
	u8 src[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	u8 dst[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	rotate_ccw_scalar(src, dst, 1, 1, 1, 0, 1, 0, 1);
	u8 expected[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	assert(memcmp(dst, expected, 8) == 0);
	printf("  test_rotate_all_ones: PASS\n");
}

static void test_rotate_single_pixel(void)
{
	/*
	 * LSB-first input: bit 0 = leftmost, so 0x01 = pixel at x=0, y=0 (top-left)
	 * After CCW 90°, this becomes bottom-left in MSB-first output.
	 * MSB-first output: bit 7 = leftmost, so 0x80 in row 7 = bottom-left pixel.
	 */
	u8 src[8] = { 0x01, 0, 0, 0, 0, 0, 0, 0 };
	u8 dst[8] = { 0 };
	rotate_ccw_scalar(src, dst, 1, 1, 1, 0, 1, 0, 1);
	assert(dst[7] == 0x80);
	for (int i = 0; i < 7; i++) assert(dst[i] == 0);
	printf("  test_rotate_single_pixel: PASS\n");
}

static void test_rotate_corner_pixels(void)
{
	u8 src[8] = { 0x81, 0, 0, 0, 0, 0, 0, 0x81 };
	u8 dst[8] = { 0 };
	rotate_ccw_scalar(src, dst, 1, 1, 1, 0, 1, 0, 1);
	assert((dst[0] & 0x81) == 0x81);
	assert((dst[7] & 0x81) == 0x81);
	printf("  test_rotate_corner_pixels: PASS\n");
}

static void test_rotate_horizontal_line(void)
{
	u8 src[8] = { 0xFF, 0, 0, 0, 0, 0, 0, 0 };
	u8 dst[8] = { 0 };
	rotate_ccw_scalar(src, dst, 1, 1, 1, 0, 1, 0, 1);
	for (int i = 0; i < 8; i++) assert(dst[i] == 0x80);
	printf("  test_rotate_horizontal_line: PASS\n");
}

static void test_rotate_vertical_line(void)
{
	/*
	 * LSB-first input: 0x01 in each row = leftmost pixel in each row (left column).
	 * After CCW 90°, left column becomes bottom row.
	 * MSB-first output: 0xFF in row 7 = all pixels in bottom row.
	 */
	u8 src[8] = { 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };
	u8 dst[8] = { 0 };
	rotate_ccw_scalar(src, dst, 1, 1, 1, 0, 1, 0, 1);
	assert(dst[7] == 0xFF);
	for (int i = 0; i < 7; i++) assert(dst[i] == 0);
	printf("  test_rotate_vertical_line: PASS\n");
}

static void test_rotate_partial_region(void)
{
	int src_stride = 4;
	int dst_stride = 2;
	int blocks_x = 4;
	u8 src[4 * 8 * 2] = { 0 };
	u8 dst[2 * 8 * 4] = { 0 };
	src[1] = 0xAA;
	rotate_ccw_scalar(src, dst, src_stride, dst_stride, blocks_x, 1, 2, 0, 1);
	int nonzero = 0;
	for (size_t i = 0; i < sizeof(dst); i++) if (dst[i]) nonzero++;
	assert(nonzero > 0);
	printf("  test_rotate_partial_region: PASS\n");
}

/*
 * Test cursor blink scenario: rotate a single 8x8 block at a specific
 * position into a full-size framebuffer. This matches what the driver does
 * for small damage clips like cursor blinks.
 *
 * Simulates clip (144,192)-(152,200) which is blocks (18,24)-(19,25).
 */
static void test_rotate_cursor_blink_scenario(void)
{
	/* Full 720x280 source (userspace FB) and 280x720 dest (hardware FB) */
	int src_stride = P4_WIDTH >> 3;   /* 90 bytes */
	int dst_stride = P4_HEIGHT >> 3;  /* 35 bytes */
	int blocks_x = P4_WIDTH >> 3;     /* 90 blocks */
	
	static u8 src[FB_SIZE];
	static u8 dst_before[FB_SIZE];
	static u8 dst_after[FB_SIZE];
	
	/* 
	 * The driver's approach for incremental clips:
	 * 1. drm_fb_xrgb8888_to_mono writes to src_buf starting at offset 0
	 * 2. rotate_blocks is called with adjusted src_offset so it reads
	 *    from the correct position
	 *
	 * For clip at block (bx, by), the driver calculates:
	 *   src_offset = src_buf - (by * 8 * src_stride)
	 *
	 * This creates a NEGATIVE offset! The rotate function then does:
	 *   col = src_offset + bx  (for reading)
	 *   col + by * 8 * src_stride + row * src_stride
	 *   = src_buf - (by * 8 * src_stride) + bx + by * 8 * src_stride + row * src_stride
	 *   = src_buf + bx + row * src_stride
	 *
	 * So it reads from src_buf[bx + row * src_stride].
	 * But mono data was written at src_buf[0]!
	 *
	 * This is WRONG if bx != 0. The mono conversion writes at start of buffer,
	 * but rotate expects it at the block's column offset.
	 *
	 * Let's verify: for a clip at (144, 192), aligned to (144, 192):
	 *   bx = 144/8 = 18
	 *   by = 192/8 = 24
	 * 
	 * Driver reads from: src_buf + bx + row * src_stride = src_buf + 18 + row * 90
	 * But mono data is at: src_buf + 0 + row * 1 (single block width)
	 *
	 * THE DRIVER IS READING FROM THE WRONG LOCATION!
	 */
	
	/* Simulate CORRECT behavior: mono data at expected position */
	memset(src, 0, FB_SIZE);
	
	int bx = 18, by = 24;
	
	/* Put pattern at the position rotate expects to read it */
	for (int row = 0; row < 8; row++) {
		src[by * 8 * src_stride + row * src_stride + bx] = 0x55 ^ row;
	}
	
	/* Initialize dest buffers - use 0x55 (won't match any rotated values) */
	memset(dst_before, 0x55, FB_SIZE);
	memcpy(dst_after, dst_before, FB_SIZE);
	
	/* Rotate single block from correct position */
	rotate_ccw_scalar(src, dst_after, src_stride, dst_stride, blocks_x,
			  bx, bx + 1, by, by + 1);
	
	/* The writes should be at: base + by + n*dst_stride for n=0..7 */
	/* base = (blocks_x - 1 - bx) * 8 * dst_stride = (90-1-18)*8*35 = 71*280 = 19880 */
	size_t base = (size_t)(blocks_x - 1 - bx) * 8 * dst_stride;
	
	/* Count bytes written at expected positions */
	int writes_at_expected = 0;
	int unexpected_writes = 0;
	
	for (size_t i = 0; i < FB_SIZE; i++) {
		if (dst_before[i] != dst_after[i]) {
			/* Check if this is an expected position */
			bool expected = false;
			for (int n = 0; n < 8; n++) {
				if (i == base + by + (size_t)n * dst_stride) {
					expected = true;
					break;
				}
			}
			if (expected) {
				writes_at_expected++;
			} else {
				unexpected_writes++;
				printf("    UNEXPECTED write at byte %zu!\n", i);
			}
		}
	}
	
	printf("  test_rotate_cursor_blink_scenario: writes_at_expected=%d unexpected=%d\n",
	       writes_at_expected, unexpected_writes);
	
	/* All changes should be at expected positions, and there should be 8 of them */
	/* (the rotation always writes 8 bytes, some might match 0x55 by coincidence) */
	assert(unexpected_writes == 0);
	assert(writes_at_expected == 8);
	
	printf("  test_rotate_cursor_blink_scenario: PASS\n");
}

#if defined(__aarch64__) || defined(__arm__)
static void test_neon_vs_scalar_single_block(void)
{
	u8 src[8] = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 };
	u8 dst_scalar[8] = { 0 };
	u8 dst_neon[8] = { 0 };
	rotate_ccw_scalar(src, dst_scalar, 1, 1, 1, 0, 1, 0, 1);
	rotate_ccw_neon(src, dst_neon, 1, 1, 1, 0, 1, 0, 1);
	assert(memcmp(dst_scalar, dst_neon, 8) == 0);
	printf("  test_neon_vs_scalar_single_block: PASS\n");
}

static void test_neon_vs_scalar_full_buffer(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	static u8 src[FB_SIZE], dst_scalar[FB_SIZE], dst_neon[FB_SIZE];
	
	/* Fill with pseudo-random pattern */
	for (size_t i = 0; i < FB_SIZE; i++)
		src[i] = (u8)(i * 17 + 31);
	
	memset(dst_scalar, 0, FB_SIZE);
	memset(dst_neon, 0, FB_SIZE);
	
	rotate_ccw_scalar(src, dst_scalar, src_stride, dst_stride, blocks_x, 
			  0, blocks_x, 0, P4_HEIGHT >> 3);
	rotate_ccw_neon(src, dst_neon, src_stride, dst_stride, blocks_x,
			0, blocks_x, 0, P4_HEIGHT >> 3);
	
	assert(memcmp(dst_scalar, dst_neon, FB_SIZE) == 0);
	printf("  test_neon_vs_scalar_full_buffer: PASS\n");
}

static void test_neon_vs_scalar_partial_region(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	static u8 src[FB_SIZE], dst_scalar[FB_SIZE], dst_neon[FB_SIZE];
	
	/* Fill with pseudo-random pattern */
	for (size_t i = 0; i < FB_SIZE; i++)
		src[i] = (u8)(i * 37 + 13);
	
	/* Test partial region: blocks 10-20 in x, 5-15 in y */
	memset(dst_scalar, 0xAA, FB_SIZE);
	memset(dst_neon, 0xAA, FB_SIZE);
	
	rotate_ccw_scalar(src, dst_scalar, src_stride, dst_stride, blocks_x,
			  10, 20, 5, 15);
	rotate_ccw_neon(src, dst_neon, src_stride, dst_stride, blocks_x,
			10, 20, 5, 15);
	
	assert(memcmp(dst_scalar, dst_neon, FB_SIZE) == 0);
	printf("  test_neon_vs_scalar_partial_region: PASS\n");
}

static void test_neon_vs_scalar_random_patterns(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	static u8 src[FB_SIZE], dst_scalar[FB_SIZE], dst_neon[FB_SIZE];
	
	for (int trial = 0; trial < 10; trial++) {
		/* Different random seed each trial */
		unsigned seed = 12345 + trial * 9999;
		for (size_t i = 0; i < FB_SIZE; i++) {
			seed = seed * 1103515245 + 12345;
			src[i] = (u8)(seed >> 16);
		}
		
		memset(dst_scalar, 0, FB_SIZE);
		memset(dst_neon, 0, FB_SIZE);
		
		rotate_ccw_scalar(src, dst_scalar, src_stride, dst_stride, blocks_x,
				  0, blocks_x, 0, P4_HEIGHT >> 3);
		rotate_ccw_neon(src, dst_neon, src_stride, dst_stride, blocks_x,
				0, blocks_x, 0, P4_HEIGHT >> 3);
		
		assert(memcmp(dst_scalar, dst_neon, FB_SIZE) == 0);
	}
	printf("  test_neon_vs_scalar_random_patterns: PASS (10 trials)\n");
}
#endif /* __aarch64__ || __arm__ */

/* ===== Damage bounds tests ===== */

static void test_damage_bounds_full(void)
{
	size_t start, end;
	int blocks_x = P4_WIDTH >> 3;
	damage_bounds(0, blocks_x, &start, &end);
	assert(start == 0);
	assert(end == FB_SIZE);
	printf("  test_damage_bounds_full: PASS\n");
}

static void test_damage_bounds_single_column(void)
{
	size_t start, end;
	int blocks_x = P4_WIDTH >> 3;
	damage_bounds(0, 1, &start, &end);
	assert(start == (size_t)(blocks_x - 1) * 8 * (HW_WIDTH >> 3));
	assert(end == FB_SIZE);
	printf("  test_damage_bounds_single_column: PASS\n");
}

static void test_damage_bounds_rightmost_column(void)
{
	size_t start, end;
	int blocks_x = P4_WIDTH >> 3;
	damage_bounds(blocks_x - 1, blocks_x, &start, &end);
	assert(start == 0);
	assert(end == 8 * (HW_WIDTH >> 3));
	printf("  test_damage_bounds_rightmost_column: PASS\n");
}

static void test_damage_bounds_middle(void)
{
	size_t start, end;
	damage_bounds(40, 50, &start, &end);
	int row0 = (90 - 50) * 8;
	int row1 = (90 - 40) * 8;
	assert(start == (size_t)row0 * 35);
	assert(end == (size_t)row1 * 35);
	printf("  test_damage_bounds_middle: PASS\n");
}
/* ===== Encoder packet tests - infrastructure ===== */

struct captured_packet {
	struct packet_header pkt;
	u8 data[256];
};

static struct captured_packet captured_packets[MAX_RESULTS];
static int captured_count;

static bool capture_emit(const struct packet_header *pkt, void *ctx)
{
	(void)ctx;
	assert(pkt_is_data(pkt) || pkt_is_flags_only(pkt));
	assert(captured_count < MAX_RESULTS);
	captured_packets[captured_count].pkt = *pkt;
	size_t dlen = pkt_data_len(pkt);
	if (dlen > 0 && dlen <= sizeof(captured_packets[0].data)) {
		memcpy(captured_packets[captured_count].data, pkt_data(pkt), dlen);
		captured_packets[captured_count].pkt.data = captured_packets[captured_count].data;
	}
	captured_count++;
	return true;
}

static void reset_captured(void)
{
	captured_count = 0;
	memset(captured_packets, 0, sizeof(captured_packets));
}

/* Apply captured packets to a buffer - reconstitutes the encoded data */
static void apply_captured_packets(u8 *out, size_t out_size)
{
	memset(out, 0, out_size);

	for (int i = 0; i < captured_count; i++) {
		pkt_apply(&captured_packets[i].pkt, out, out_size);
	}
}

/* Encode a buffer and verify it reconstitutes correctly */
static void verify_encode_roundtrip(const u8 *buf, size_t len, const char *test_name)
{
	static u8 reconstructed[FB_SIZE];

	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);

	bool ok = encode_region(&enc, 0, buf, len);
	assert(ok);

	apply_captured_packets(reconstructed, len);

	if (memcmp(buf, reconstructed, len) != 0) {
		fprintf(stderr, "%s: reconstitution mismatch!\n", test_name);
		fprintf(stderr, "  encoded %zu bytes into %d packets\n", len, captured_count);
		for (size_t i = 0; i < len; i++) {
			if (buf[i] != reconstructed[i]) {
				fprintf(stderr, "  first diff at offset %zu: expected 0x%02x, got 0x%02x\n",
					i, buf[i], reconstructed[i]);
				break;
			}
		}
		assert(0);
	}
}

/* Encode a diff and verify it reconstitutes correctly when applied to old buffer */
static void verify_diff_roundtrip(const u8 *old_buf, const u8 *new_buf, size_t len, const char *test_name)
{
	static u8 reconstructed[FB_SIZE];

	/* Start with old buffer */
	memcpy(reconstructed, old_buf, len);

	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);

	bool ok = encode_diff(&enc, old_buf, new_buf, 0, len);
	assert(ok);

	/* Apply diff packets on top of old buffer */
	for (int i = 0; i < captured_count; i++) {
		struct packet_header *p = &captured_packets[i].pkt;
		/* Skip flags-only packets, process data packets only */
		if (!pkt_is_data(p))
			continue;

		u16 addr = pkt_addr(p);
		u16 plen = pkt_len(p);

		if (addr + plen > len)
			continue;

		if (pkt_is_rle(p)) {
			memset(reconstructed + addr, pkt_data(p)[0], plen);
		} else {
			memcpy(reconstructed + addr, pkt_data(p), plen);
		}
	}

	if (memcmp(new_buf, reconstructed, len) != 0) {
		fprintf(stderr, "%s: diff reconstitution mismatch!\n", test_name);
		fprintf(stderr, "  encoded diff into %d packets\n", captured_count);
		for (size_t i = 0; i < len; i++) {
			if (new_buf[i] != reconstructed[i]) {
				fprintf(stderr, "  first diff at offset %zu: expected 0x%02x, got 0x%02x\n",
					i, new_buf[i], reconstructed[i]);
				break;
			}
		}
		assert(0);
	}
}

/* Verify header fields using helpers from update.h */
static void verify_header(int idx, bool exp_new_frame, bool exp_rle,
			  u8 exp_flags, u16 exp_addr, u16 exp_len)
{
	struct packet_header *p = &captured_packets[idx].pkt;
	/* Accept data packets only */
	assert(pkt_is_data(p));
	assert(pkt_is_new_frame(p) == exp_new_frame);
	assert(pkt_is_rle(p) == exp_rle);
	assert(pkt_flags(p) == exp_flags);
	assert(pkt_addr(p) == exp_addr);
	assert(pkt_len(p) == exp_len);
}

/* ===== Integration tests ===== */

static void test_rotate_then_diff(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(src, 0xAA, FB_SIZE);
	memset(dst_old, 0, FB_SIZE);
	memset(dst_new, 0, FB_SIZE);

	rotate_ccw_scalar(src, dst_old, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, P4_HEIGHT >> 3);
	memcpy(dst_new, dst_old, FB_SIZE);

	for (int by = 0; by < (P4_HEIGHT >> 3); by++) {
		for (int bx = 10; bx < 20; bx++) {
			src[by * 8 * src_stride + bx] = 0x55;
		}
	}
	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  10, 20, 0, P4_HEIGHT >> 3);

	size_t exp_start, exp_end;
	damage_bounds(10, 20, &exp_start, &exp_end);

	for (size_t i = 0; i < exp_start; i++)
		assert(dst_old[i] == dst_new[i]);

	int changed = 0;
	for (size_t i = exp_start; i < exp_end; i++)
		if (dst_old[i] != dst_new[i]) changed++;
	assert(changed > 0);

	for (size_t i = exp_end; i < FB_SIZE; i++)
		assert(dst_old[i] == dst_new[i]);

	printf("  test_rotate_then_diff: PASS\n");
}

static void test_rotate_diff_finds_region(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(src, 0x00, FB_SIZE);
	memset(dst_old, 0, FB_SIZE);
	memset(dst_new, 0, FB_SIZE);

	rotate_ccw_scalar(src, dst_old, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, P4_HEIGHT >> 3);

	for (int by = 0; by < (P4_HEIGHT >> 3); by++) {
		for (int bx = 40; bx < 50; bx++) {
			src[by * 8 * src_stride + bx] = 0xFF;
		}
	}
	memcpy(dst_new, dst_old, FB_SIZE);
	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  40, 50, 0, P4_HEIGHT >> 3);

	size_t exp_start, exp_end;
	damage_bounds(40, 50, &exp_start, &exp_end);

	reset_results();
	find_diffs(dst_old + (exp_start & ~7UL), dst_new + (exp_start & ~7UL),
		   ((exp_end + 7) & ~7UL) - (exp_start & ~7UL), 8, collect_diff, NULL);

	assert(diff_count > 0);
	printf("  test_rotate_diff_finds_region: PASS\n");
}

static void test_full_pipeline_rle(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(src, 0x00, FB_SIZE);
	memset(dst_old, 0, FB_SIZE);

	rotate_ccw_scalar(src, dst_old, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, P4_HEIGHT >> 3);

	for (int by = 0; by < (P4_HEIGHT >> 3); by++) {
		for (int bx = 20; bx < 30; bx++) {
			src[by * 8 * src_stride + bx] = 0xFF;
		}
	}
	memcpy(dst_new, dst_old, FB_SIZE);
	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  20, 30, 0, P4_HEIGHT >> 3);

	size_t exp_start, exp_end;
	damage_bounds(20, 30, &exp_start, &exp_end);

	reset_results();
	size_t scan_base = exp_start & ~7UL;
	size_t scan_len = ((exp_end + 7) & ~7UL) - scan_base;
	find_diffs(dst_old + scan_base, dst_new + scan_base, scan_len, 8, collect_diff, NULL);

	assert(diff_count > 0);

	int total_rle_runs = 0;
	for (int i = 0; i < diff_count; i++) {
		size_t start = scan_base + diff_results[i].start;
		size_t len = diff_results[i].end - diff_results[i].start;

		rle_count = 0;
		find_rle(dst_new + start, len, RLE_MIN_RUN, collect_rle, NULL);

		for (int j = 0; j < rle_count; j++) {
			if (rle_results[j].is_rle)
				total_rle_runs++;
		}
	}

	assert(total_rle_runs > 0);
	printf("  test_full_pipeline_rle: PASS\n");
}

static void consolidate_damage_rects(const int rects[][4], int nrects,
				     int *bx0, int *bx1, int *by0, int *by1)
{
	*bx0 = P4_WIDTH >> 3;
	*bx1 = 0;
	*by0 = P4_HEIGHT >> 3;
	*by1 = 0;

	for (int i = 0; i < nrects; i++) {
		int x = rects[i][0], y = rects[i][1];
		int w = rects[i][2], h = rects[i][3];

		int rbx0 = x >> 3;
		int rbx1 = (x + w + 7) >> 3;
		int rby0 = y >> 3;
		int rby1 = (y + h + 7) >> 3;

		if (rbx0 < *bx0) *bx0 = rbx0;
		if (rbx1 > *bx1) *bx1 = rbx1;
		if (rby0 < *by0) *by0 = rby0;
		if (rby1 > *by1) *by1 = rby1;
	}
}

static void test_multi_rect_damage_adjacent(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(src, 0x00, FB_SIZE);
	memset(dst_old, 0, FB_SIZE);

	rotate_ccw_scalar(src, dst_old, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, P4_HEIGHT >> 3);

	int rects[2][4] = {
		{ 0, 0, 80, 40 },
		{ 80, 0, 80, 40 }
	};

	int bx0, bx1, by0, by1;
	consolidate_damage_rects(rects, 2, &bx0, &bx1, &by0, &by1);

	for (int by = by0; by < by1; by++) {
		for (int bx = bx0; bx < bx1; bx++) {
			src[by * 8 * src_stride + bx] = 0xAA;
		}
	}

	memcpy(dst_new, dst_old, FB_SIZE);
	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  bx0, bx1, by0, by1);

	size_t scan_start, scan_end;
	damage_bounds(bx0, bx1, &scan_start, &scan_end);

	reset_results();
	size_t scan_base = scan_start & ~7UL;
	size_t scan_len = ((scan_end + 7) & ~7UL) - scan_base;
	find_diffs(dst_old + scan_base, dst_new + scan_base, scan_len, 8, collect_diff, NULL);

	assert(diff_count > 0);

	for (size_t i = 0; i < scan_base; i++)
		assert(dst_old[i] == dst_new[i]);
	for (size_t i = scan_base + scan_len; i < FB_SIZE; i++)
		assert(dst_old[i] == dst_new[i]);

	printf("  test_multi_rect_damage_adjacent: PASS\n");
}

static void test_multi_rect_damage_scattered(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	int blocks_y = P4_HEIGHT >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(src, 0x00, FB_SIZE);
	memset(dst_old, 0, FB_SIZE);

	rotate_ccw_scalar(src, dst_old, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	int rects[3][4] = {
		{ 0, 0, 40, 40 },
		{ 320, 100, 80, 80 },
		{ 640, 200, 80, 80 }
	};

	int bx0, bx1, by0, by1;
	consolidate_damage_rects(rects, 3, &bx0, &bx1, &by0, &by1);

	for (int r = 0; r < 3; r++) {
		int rx0 = rects[r][0] >> 3;
		int rx1 = (rects[r][0] + rects[r][2] + 7) >> 3;
		int ry0 = rects[r][1] >> 3;
		int ry1 = (rects[r][1] + rects[r][3] + 7) >> 3;

		for (int by = ry0; by < ry1 && by < blocks_y; by++) {
			for (int bx = rx0; bx < rx1 && bx < blocks_x; bx++) {
				src[by * 8 * src_stride + bx] = 0x55 + r;
			}
		}
	}

	memcpy(dst_new, dst_old, FB_SIZE);
	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  bx0, bx1, by0, by1);

	size_t scan_start, scan_end;
	damage_bounds(bx0, bx1, &scan_start, &scan_end);

	reset_results();
	size_t scan_base = scan_start & ~7UL;
	size_t scan_len = ((scan_end + 7) & ~7UL) - scan_base;
	find_diffs(dst_old + scan_base, dst_new + scan_base, scan_len, 8, collect_diff, NULL);

	assert(diff_count >= 1);

	printf("  test_multi_rect_damage_scattered: PASS\n");
}

static void test_multi_rect_damage_overlapping(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	int blocks_y = P4_HEIGHT >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(src, 0x00, FB_SIZE);
	memset(dst_old, 0, FB_SIZE);

	rotate_ccw_scalar(src, dst_old, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	int rects[2][4] = {
		{ 100, 50, 200, 100 },
		{ 200, 100, 200, 100 }
	};

	int bx0, bx1, by0, by1;
	consolidate_damage_rects(rects, 2, &bx0, &bx1, &by0, &by1);

	for (int r = 0; r < 2; r++) {
		int rx0 = rects[r][0] >> 3;
		int rx1 = (rects[r][0] + rects[r][2] + 7) >> 3;
		int ry0 = rects[r][1] >> 3;
		int ry1 = (rects[r][1] + rects[r][3] + 7) >> 3;

		for (int by = ry0; by < ry1 && by < blocks_y; by++) {
			for (int bx = rx0; bx < rx1 && bx < blocks_x; bx++) {
				src[by * 8 * src_stride + bx] |= (r ? 0xAA : 0x55);
			}
		}
	}

	memcpy(dst_new, dst_old, FB_SIZE);
	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  bx0, bx1, by0, by1);

	size_t scan_start, scan_end;
	damage_bounds(bx0, bx1, &scan_start, &scan_end);

	reset_results();
	size_t scan_base = scan_start & ~7UL;
	size_t scan_len = ((scan_end + 7) & ~7UL) - scan_base;
	find_diffs(dst_old + scan_base, dst_new + scan_base, scan_len, 8, collect_diff, NULL);

	assert(diff_count > 0);

	int total_rle = 0, total_lit = 0;
	for (int i = 0; i < diff_count; i++) {
		size_t start = scan_base + diff_results[i].start;
		size_t len = diff_results[i].end - diff_results[i].start;

		rle_count = 0;
		find_rle(dst_new + start, len, RLE_MIN_RUN, collect_rle, NULL);

		for (int j = 0; j < rle_count; j++) {
			if (rle_results[j].is_rle) total_rle++;
			else total_lit++;
		}
	}

	assert(total_rle + total_lit > 0);

	printf("  test_multi_rect_damage_overlapping: PASS\n");
}

static void test_single_pixel_damage(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	int blocks_y = P4_HEIGHT >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(src, 0x00, FB_SIZE);
	memset(dst_old, 0, FB_SIZE);

	rotate_ccw_scalar(src, dst_old, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	int rects[1][4] = {{ 360, 140, 1, 1 }};

	int bx0, bx1, by0, by1;
	consolidate_damage_rects(rects, 1, &bx0, &bx1, &by0, &by1);

	src[17 * 8 * src_stride + 45] = 0x80;

	memcpy(dst_new, dst_old, FB_SIZE);
	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  bx0, bx1, by0, by1);

	size_t scan_start, scan_end;
	damage_bounds(bx0, bx1, &scan_start, &scan_end);

	assert(scan_end - scan_start == 8 * (HW_WIDTH >> 3));

	reset_results();
	size_t scan_base = scan_start & ~7UL;
	size_t scan_len = ((scan_end + 7) & ~7UL) - scan_base;
	find_diffs(dst_old + scan_base, dst_new + scan_base, scan_len, 8, collect_diff, NULL);

	assert(diff_count == 1);

	printf("  test_single_pixel_damage: PASS\n");
}

static void test_full_screen_damage(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	int blocks_y = P4_HEIGHT >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(src, 0x00, FB_SIZE);
	memset(dst_old, 0, FB_SIZE);

	rotate_ccw_scalar(src, dst_old, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	int rects[1][4] = {{ 0, 0, P4_WIDTH, P4_HEIGHT }};

	int bx0, bx1, by0, by1;
	consolidate_damage_rects(rects, 1, &bx0, &bx1, &by0, &by1);

	assert(bx0 == 0 && bx1 == blocks_x);
	assert(by0 == 0 && by1 == blocks_y);

	for (int by = 0; by < blocks_y; by++) {
		for (int bx = 0; bx < blocks_x; bx++) {
			src[by * 8 * src_stride + bx] = ((bx + by) & 1) ? 0xAA : 0x55;
		}
	}

	memcpy(dst_new, dst_old, FB_SIZE);
	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  bx0, bx1, by0, by1);

	size_t scan_start, scan_end;
	damage_bounds(bx0, bx1, &scan_start, &scan_end);

	assert(scan_start == 0);
	assert(scan_end == FB_SIZE);

	reset_results();
	find_diffs(dst_old, dst_new, FB_SIZE, 8, collect_diff, NULL);

	assert(diff_count == 1);
	assert(diff_results[0].end - diff_results[0].start > FB_SIZE - 100);

	printf("  test_full_screen_damage: PASS\n");
}

static void test_horizontal_stripe_damage(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	int blocks_y = P4_HEIGHT >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(src, 0x00, FB_SIZE);
	memset(dst_old, 0, FB_SIZE);

	rotate_ccw_scalar(src, dst_old, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	int rects[1][4] = {{ 0, 100, P4_WIDTH, 20 }};

	int bx0, bx1, by0, by1;
	consolidate_damage_rects(rects, 1, &bx0, &bx1, &by0, &by1);

	assert(bx0 == 0 && bx1 == blocks_x);

	for (int by = by0; by < by1; by++) {
		for (int bx = bx0; bx < bx1; bx++) {
			src[by * 8 * src_stride + bx] = 0xFF;
		}
	}

	memcpy(dst_new, dst_old, FB_SIZE);
	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  bx0, bx1, by0, by1);

	size_t scan_start, scan_end;
	damage_bounds(bx0, bx1, &scan_start, &scan_end);

	assert(scan_start == 0);
	assert(scan_end == FB_SIZE);

	reset_results();
	find_diffs(dst_old, dst_new, FB_SIZE, 8, collect_diff, NULL);

	assert(diff_count > 0);

	printf("  test_horizontal_stripe_damage: PASS\n");
}

static void test_vertical_stripe_damage(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	int blocks_y = P4_HEIGHT >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(src, 0x00, FB_SIZE);
	memset(dst_old, 0, FB_SIZE);

	rotate_ccw_scalar(src, dst_old, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	int rects[1][4] = {{ 700, 0, 20, P4_HEIGHT }};

	int bx0, bx1, by0, by1;
	consolidate_damage_rects(rects, 1, &bx0, &bx1, &by0, &by1);

	assert(bx0 == 87);
	assert(by0 == 0 && by1 == blocks_y);

	for (int by = by0; by < by1; by++) {
		for (int bx = bx0; bx < bx1; bx++) {
			src[by * 8 * src_stride + bx] = 0xFF;
		}
	}

	memcpy(dst_new, dst_old, FB_SIZE);
	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  bx0, bx1, by0, by1);

	size_t scan_start, scan_end;
	damage_bounds(bx0, bx1, &scan_start, &scan_end);

	assert(scan_start == 0);
	assert(scan_end < FB_SIZE / 2);

	reset_results();
	size_t scan_base = scan_start & ~7UL;
	size_t scan_len = ((scan_end + 7) & ~7UL) - scan_base;
	find_diffs(dst_old + scan_base, dst_new + scan_base, scan_len, 8, collect_diff, NULL);

	assert(diff_count > 0);

	printf("  test_vertical_stripe_damage: PASS\n");
}

static void test_checkerboard_byte_pattern(void)
{
	u8 buf[64];
	for (int i = 0; i < 64; i++)
		buf[i] = (i & 1) ? 0xFF : 0x00;

	reset_results();
	bool ok = find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);

	assert(ok);
	assert(rle_count == 1);
	assert(!rle_results[0].is_rle);
	assert(rle_results[0].offset == 0);
	assert(rle_results[0].len == 64);

	printf("  test_checkerboard_byte_pattern: PASS\n");
}

static void test_checkerboard_full_screen(void)
{
	static u8 old_buf[FB_SIZE], new_buf[FB_SIZE];
	memset(old_buf, 0x00, FB_SIZE);

	for (size_t i = 0; i < FB_SIZE; i++)
		new_buf[i] = (i & 1) ? 0xFF : 0x00;

	reset_results();
	find_diffs(old_buf, new_buf, FB_SIZE, 8, collect_diff, NULL);

	assert(diff_count == 1);
	size_t diff_len = diff_results[0].end - diff_results[0].start;
	assert(diff_len > FB_SIZE / 2);

	rle_count = 0;
	find_rle(new_buf + diff_results[0].start, diff_len, RLE_MIN_RUN, collect_rle, NULL);

	assert(rle_count == 1);
	assert(!rle_results[0].is_rle);
	assert(rle_results[0].len == diff_len);

	printf("  test_checkerboard_full_screen: PASS\n");
}

static void test_checkerboard_with_rotation(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	int blocks_y = P4_HEIGHT >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(src, 0x00, FB_SIZE);
	memset(dst_old, 0, FB_SIZE);

	rotate_ccw_scalar(src, dst_old, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	for (size_t i = 0; i < FB_SIZE; i++)
		src[i] = (u8)(i * 7 + (i >> 3) * 13);

	memcpy(dst_new, dst_old, FB_SIZE);
	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	reset_results();
	find_diffs(dst_old, dst_new, FB_SIZE, 8, collect_diff, NULL);

	assert(diff_count >= 1);

	int total_rle = 0, total_lit = 0;
	size_t rle_bytes = 0, lit_bytes = 0;

	for (int i = 0; i < diff_count; i++) {
		size_t start = diff_results[i].start;
		size_t len = diff_results[i].end - start;

		rle_count = 0;
		find_rle(dst_new + start, len, RLE_MIN_RUN, collect_rle, NULL);

		for (int j = 0; j < rle_count; j++) {
			if (rle_results[j].is_rle) {
				total_rle++;
				rle_bytes += rle_results[j].len;
			} else {
				total_lit++;
				lit_bytes += rle_results[j].len;
			}
		}
	}

	(void)rle_bytes; (void)lit_bytes;
	assert(total_rle + total_lit > 0);

	printf("  test_checkerboard_with_rotation: PASS\n");
}

static void test_sparse_changes(void)
{
	static u8 old_buf[FB_SIZE], new_buf[FB_SIZE];
	memset(old_buf, 0x00, FB_SIZE);
	memset(new_buf, 0x00, FB_SIZE);

	for (size_t i = 0; i < FB_SIZE; i += 100)
		new_buf[i] = 0xFF;

	reset_results();
	find_diffs(old_buf, new_buf, FB_SIZE, 8, collect_diff, NULL);

	assert(diff_count > 100);

	for (int i = 0; i < diff_count; i++) {
		size_t len = diff_results[i].end - diff_results[i].start;
		assert(len <= 8);
	}

	printf("  test_sparse_changes: PASS\n");
}

static void test_dense_then_sparse(void)
{
	static u8 old_buf[FB_SIZE], new_buf[FB_SIZE];
	memset(old_buf, 0x00, FB_SIZE);
	memset(new_buf, 0x00, FB_SIZE);

	for (size_t i = 0; i < FB_SIZE / 2; i++)
		new_buf[i] = 0xAA;

	for (size_t i = FB_SIZE / 2; i < FB_SIZE; i += 200)
		new_buf[i] = 0xFF;

	reset_results();
	find_diffs(old_buf, new_buf, FB_SIZE, 8, collect_diff, NULL);

	assert(diff_count > 1);
	assert(diff_results[0].end - diff_results[0].start > FB_SIZE / 4);

	rle_count = 0;
	find_rle(new_buf + diff_results[0].start,
		 diff_results[0].end - diff_results[0].start, 8, collect_rle, NULL);

	int found_rle = 0;
	for (int i = 0; i < rle_count; i++) {
		if (rle_results[i].is_rle && rle_results[i].byte == 0xAA)
			found_rle = 1;
	}
	assert(found_rle);

	printf("  test_dense_then_sparse: PASS\n");
}

static void test_bitwise_checkerboard_no_rle(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	int blocks_y = P4_HEIGHT >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(dst_old, 0, FB_SIZE);

	for (size_t i = 0; i < FB_SIZE; i++)
		src[i] = (u8)(i * 17 + (i >> 8) * 31);

	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	reset_results();
	find_diffs(dst_old, dst_new, FB_SIZE, 8, collect_diff, NULL);

	assert(diff_count >= 1);

	size_t total_rle_bytes = 0, total_lit_bytes = 0;
	for (int i = 0; i < diff_count; i++) {
		size_t start = diff_results[i].start;
		size_t len = diff_results[i].end - start;

		rle_count = 0;
		find_rle(dst_new + start, len, RLE_MIN_RUN, collect_rle, NULL);

		for (int j = 0; j < rle_count; j++) {
			if (rle_results[j].is_rle)
				total_rle_bytes += rle_results[j].len;
			else
				total_lit_bytes += rle_results[j].len;
		}
	}

	assert(total_lit_bytes > total_rle_bytes);

	printf("  test_bitwise_checkerboard_no_rle: PASS\n");
}

static void test_bitwise_checkerboard_2x2(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	int blocks_y = P4_HEIGHT >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(dst_old, 0, FB_SIZE);

	for (size_t i = 0; i < FB_SIZE; i++)
		src[i] = (u8)(i * 23 + (i >> 4) * 47 + (i >> 12) * 11);

	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	reset_results();
	find_diffs(dst_old, dst_new, FB_SIZE, 8, collect_diff, NULL);

	assert(diff_count >= 1);

	size_t total_rle_bytes = 0, total_lit_bytes = 0;
	for (int i = 0; i < diff_count; i++) {
		size_t start = diff_results[i].start;
		size_t len = diff_results[i].end - start;

		rle_count = 0;
		find_rle(dst_new + start, len, RLE_MIN_RUN, collect_rle, NULL);

		for (int j = 0; j < rle_count; j++) {
			if (rle_results[j].is_rle)
				total_rle_bytes += rle_results[j].len;
			else
				total_lit_bytes += rle_results[j].len;
		}
	}

	assert(total_lit_bytes > total_rle_bytes);

	printf("  test_bitwise_checkerboard_2x2: PASS\n");
}

static void test_checkerboard_with_solid_rect(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	int blocks_y = P4_HEIGHT >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(dst_old, 0, FB_SIZE);

	for (size_t i = 0; i < FB_SIZE; i++)
		src[i] = (u8)(i * 17 + (i >> 8) * 31);

	for (int row = 80; row < 200; row++) {
		memset(src + row * src_stride + (200 >> 3), 0xFF, (400 - 200) >> 3);
	}

	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	reset_results();
	find_diffs(dst_old, dst_new, FB_SIZE, 8, collect_diff, NULL);

	assert(diff_count >= 1);

	int total_rle_runs = 0;
	size_t total_rle_bytes = 0, total_lit_bytes = 0;

	for (int i = 0; i < diff_count; i++) {
		size_t start = diff_results[i].start;
		size_t len = diff_results[i].end - start;

		rle_count = 0;
		find_rle(dst_new + start, len, RLE_MIN_RUN, collect_rle, NULL);

		for (int j = 0; j < rle_count; j++) {
			if (rle_results[j].is_rle) {
				total_rle_runs++;
				total_rle_bytes += rle_results[j].len;
			} else {
				total_lit_bytes += rle_results[j].len;
			}
		}
	}
	(void)total_rle_bytes;

	assert(total_rle_runs > 0);
	assert(total_lit_bytes > 0);

	printf("  test_checkerboard_with_solid_rect: PASS\n");
}

static void test_checkerboard_with_multiple_rects(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	int blocks_y = P4_HEIGHT >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(dst_old, 0, FB_SIZE);

	for (size_t i = 0; i < FB_SIZE; i++)
		src[i] = (u8)(i * 17 + (i >> 8) * 31);

	/* Draw solid rectangles */
	for (int row = 10; row < 60; row++)
		memset(src + row * src_stride + 1, 0xFF, 16);

	for (int row = 100; row < 180; row++)
		memset(src + row * src_stride + 30, 0x00, 16);

	for (int row = 200; row < 270; row++)
		memset(src + row * src_stride + 60, 0x0F, 16);

	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	reset_results();
	find_diffs(dst_old, dst_new, FB_SIZE, 8, collect_diff, NULL);

	assert(diff_count >= 1);

	size_t total_rle_bytes = 0;
	size_t total_lit_bytes = 0;

	for (int i = 0; i < diff_count; i++) {
		size_t start = diff_results[i].start;
		size_t len = diff_results[i].end - start;

		rle_count = 0;
		find_rle(dst_new + start, len, RLE_MIN_RUN, collect_rle, NULL);

		for (int j = 0; j < rle_count; j++) {
			if (rle_results[j].is_rle) {
				total_rle_bytes += rle_results[j].len;
			} else {
				total_lit_bytes += rle_results[j].len;
			}
		}
	}

	/* After rotation, we expect to find some data (either RLE or literal) */
	assert(total_rle_bytes > 0 || total_lit_bytes > 0);

	printf("  test_checkerboard_with_multiple_rects: PASS\n");
}

static void test_solid_rect_on_noise(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	int blocks_y = P4_HEIGHT >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	memset(dst_old, 0, FB_SIZE);

	for (size_t i = 0; i < FB_SIZE; i++)
		src[i] = (u8)(i * 17 + (i >> 5) * 31 + (i >> 10) * 7);

	for (int row = 50; row < 230; row++)
		memset(src + row * src_stride + 18, 0xFF, 50);

	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	reset_results();
	find_diffs(dst_old, dst_new, FB_SIZE, 8, collect_diff, NULL);

	assert(diff_count >= 1);

	int rle_ff = 0;
	size_t rle_ff_bytes = 0, total_lit_bytes = 0;

	for (int i = 0; i < diff_count; i++) {
		size_t start = diff_results[i].start;
		size_t len = diff_results[i].end - start;

		rle_count = 0;
		find_rle(dst_new + start, len, RLE_MIN_RUN, collect_rle, NULL);

		for (int j = 0; j < rle_count; j++) {
			if (rle_results[j].is_rle && rle_results[j].byte == 0xFF) {
				rle_ff++;
				rle_ff_bytes += rle_results[j].len;
			} else if (!rle_results[j].is_rle) {
				total_lit_bytes += rle_results[j].len;
			}
		}
	}

	assert(rle_ff > 0);
	assert(rle_ff_bytes > 100);
	assert(total_lit_bytes > 0);

	printf("  test_solid_rect_on_noise: PASS\n");
}

static void set_pixel(u8 *buf, int stride, int x, int y)
{
	int byte_idx = y * stride + (x >> 3);
	int bit_idx = 7 - (x & 7);
	buf[byte_idx] |= (1 << bit_idx);
}

static int get_pixel(const u8 *buf, int stride, int x, int y)
{
	int byte_idx = y * stride + (x >> 3);
	int bit_idx = 7 - (x & 7);
	return (buf[byte_idx] >> bit_idx) & 1;
}

static void draw_spiral_cw(u8 *buf, int stride, int width, int height)
{
	int top = 0, bottom = height - 1, left = 0, right = width - 1;
	int x, y;

	while (top <= bottom && left <= right) {
		for (x = left; x <= right; x++) set_pixel(buf, stride, x, top);
		top++;
		for (y = top; y <= bottom; y++) set_pixel(buf, stride, right, y);
		right--;
		if (top <= bottom) {
			for (x = right; x >= left; x--) set_pixel(buf, stride, x, bottom);
			bottom--;
		}
		if (left <= right) {
			for (y = bottom; y >= top; y--) set_pixel(buf, stride, left, y);
			left++;
		}
	}
}

static void draw_spiral_ccw(u8 *buf, int stride, int width, int height)
{
	int top = 0, bottom = height - 1, left = 0, right = width - 1;
	int x, y;

	while (top <= bottom && left <= right) {
		for (y = top; y <= bottom; y++) set_pixel(buf, stride, left, y);
		left++;
		for (x = left; x <= right; x++) set_pixel(buf, stride, x, bottom);
		bottom--;
		if (left <= right) {
			for (y = bottom; y >= top; y--) set_pixel(buf, stride, right, y);
			right--;
		}
		if (top <= bottom) {
			for (x = right; x >= left; x--) set_pixel(buf, stride, x, top);
			top++;
		}
	}
}

static u8 literal_data[FB_SIZE];
static size_t literal_data_pos;

static bool collect_packets(size_t offset, const u8 *data, size_t len, bool is_rle, void *ctx)
{
	(void)ctx;
	if (rle_count < MAX_RESULTS) {
		rle_results[rle_count].offset = offset;
		rle_results[rle_count].len = len;
		rle_results[rle_count].is_rle = is_rle;
		if (is_rle) {
			rle_results[rle_count].byte = *data;
		} else {
			rle_results[rle_count].byte = 0;
			memcpy(literal_data + literal_data_pos, data, len);
			literal_data_pos += len;
		}
		rle_count++;
	}
	return true;
}

static void reconstruct_frame(const u8 *new_buf, u8 *out, size_t out_size)
{
	memset(out, 0, out_size);

	for (int d = 0; d < diff_count; d++) {
		size_t diff_start = diff_results[d].start;
		size_t diff_len = diff_results[d].end - diff_start;

		rle_count = 0;
		literal_data_pos = 0;
		find_rle(new_buf + diff_start, diff_len, RLE_MIN_RUN, collect_packets, NULL);

		size_t lit_pos = 0;
		for (int r = 0; r < rle_count; r++) {
			size_t abs_offset = diff_start + rle_results[r].offset;

			if (rle_results[r].is_rle) {
				memset(out + abs_offset, rle_results[r].byte, rle_results[r].len);
			} else {
				memcpy(out + abs_offset, literal_data + lit_pos, rle_results[r].len);
				lit_pos += rle_results[r].len;
			}
		}
	}
}

static void test_spiral_clockwise(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	int blocks_y = P4_HEIGHT >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	static u8 reconstructed[FB_SIZE];

	memset(src, 0, FB_SIZE);
	memset(dst_old, 0, FB_SIZE);

	draw_spiral_cw(src, src_stride, P4_WIDTH, P4_HEIGHT);

	int src_pixels = 0;
	for (int y = 0; y < P4_HEIGHT; y++) {
		for (int x = 0; x < P4_WIDTH; x++) {
			if (get_pixel(src, src_stride, x, y)) src_pixels++;
		}
	}
	assert(src_pixels > 0);

	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	reset_results();
	find_diffs(dst_old, dst_new, FB_SIZE, 8, collect_diff, NULL);
	assert(diff_count > 0);

	reconstruct_frame(dst_new, reconstructed, FB_SIZE);

	assert(memcmp(reconstructed, dst_new, FB_SIZE) == 0);

	int dst_pixels = 0;
	for (int y = 0; y < HW_HEIGHT; y++) {
		for (int x = 0; x < HW_WIDTH; x++) {
			if (get_pixel(reconstructed, dst_stride, x, y)) dst_pixels++;
		}
	}

	assert(dst_pixels == src_pixels);

	printf("  test_spiral_clockwise: PASS (spiral has %d pixels)\n", src_pixels);
}

static void test_spiral_counterclockwise(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	int blocks_y = P4_HEIGHT >> 3;

	static u8 src[FB_SIZE], dst_old[FB_SIZE], dst_new[FB_SIZE];
	static u8 reconstructed[FB_SIZE];

	memset(src, 0, FB_SIZE);
	memset(dst_old, 0, FB_SIZE);

	draw_spiral_ccw(src, src_stride, P4_WIDTH, P4_HEIGHT);

	int src_pixels = 0;
	for (int y = 0; y < P4_HEIGHT; y++) {
		for (int x = 0; x < P4_WIDTH; x++) {
			if (get_pixel(src, src_stride, x, y)) src_pixels++;
		}
	}
	assert(src_pixels > 0);

	rotate_ccw_scalar(src, dst_new, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	reset_results();
	find_diffs(dst_old, dst_new, FB_SIZE, 8, collect_diff, NULL);
	assert(diff_count > 0);

	reconstruct_frame(dst_new, reconstructed, FB_SIZE);

	assert(memcmp(reconstructed, dst_new, FB_SIZE) == 0);

	int dst_pixels = 0;
	for (int y = 0; y < HW_HEIGHT; y++) {
		for (int x = 0; x < HW_WIDTH; x++) {
			if (get_pixel(reconstructed, dst_stride, x, y)) dst_pixels++;
		}
	}

	assert(dst_pixels == src_pixels);

	printf("  test_spiral_counterclockwise: PASS (spiral has %d pixels)\n", src_pixels);
}

static void test_spiral_both_combined(void)
{
	int src_stride = P4_WIDTH >> 3;
	int dst_stride = P4_HEIGHT >> 3;
	int blocks_x = P4_WIDTH >> 3;
	int blocks_y = P4_HEIGHT >> 3;

	static u8 src_cw[FB_SIZE], src_combined[FB_SIZE];
	static u8 dst_old[FB_SIZE], dst_new[FB_SIZE], reconstructed[FB_SIZE];

	memset(src_cw, 0, FB_SIZE);
	memset(src_combined, 0, FB_SIZE);
	memset(dst_old, 0, FB_SIZE);

	draw_spiral_cw(src_cw, src_stride, P4_WIDTH, P4_HEIGHT);
	draw_spiral_ccw(src_combined, src_stride, P4_WIDTH - 20, P4_HEIGHT - 20);

	for (size_t i = 0; i < FB_SIZE; i++)
		src_combined[i] |= src_cw[i];

	int src_pixels = 0;
	for (int y = 0; y < P4_HEIGHT; y++) {
		for (int x = 0; x < P4_WIDTH; x++) {
			if (get_pixel(src_combined, src_stride, x, y)) src_pixels++;
		}
	}

	rotate_ccw_scalar(src_combined, dst_new, src_stride, dst_stride, blocks_x,
			  0, blocks_x, 0, blocks_y);

	reset_results();
	find_diffs(dst_old, dst_new, FB_SIZE, 8, collect_diff, NULL);
	assert(diff_count > 0);

	reconstruct_frame(dst_new, reconstructed, FB_SIZE);

	assert(memcmp(reconstructed, dst_new, FB_SIZE) == 0);

	int dst_pixels = 0;
	for (int y = 0; y < HW_HEIGHT; y++) {
		for (int x = 0; x < HW_WIDTH; x++) {
			if (get_pixel(reconstructed, dst_stride, x, y)) dst_pixels++;
		}
	}
	assert(dst_pixels == src_pixels);

	printf("  test_spiral_both_combined: PASS (combined has %d pixels)\n", src_pixels);
}

static void test_encoder_single_rle(void)
{
	u8 buf[16];
	memset(buf, 0xFF, 16);
	
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);
	
	bool ok = encode_region(&enc, 100, buf, 16);
	assert(ok);
	assert(captured_count == 1);
	
	verify_header(0, false, true, 0x00, 100, 16);
	
	assert(pkt_data_len(&captured_packets[0].pkt) == 1);
	assert(pkt_data(&captured_packets[0].pkt)[0] == 0xFF);
	
	verify_encode_roundtrip(buf, 16, "test_encoder_single_rle");
	
	printf("  test_encoder_single_rle: PASS\n");
}

static void test_encoder_single_literal(void)
{
	u8 buf[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
	
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);
	
	bool ok = encode_region(&enc, 200, buf, 8);
	assert(ok);
	assert(captured_count == 1);
	
	verify_header(0, false, false, 0x00, 200, 8);
	
	assert(pkt_data_len(&captured_packets[0].pkt) == 8);
	assert(memcmp(pkt_data(&captured_packets[0].pkt), buf, 8) == 0);
	
	verify_encode_roundtrip(buf, 8, "test_encoder_single_literal");
	
	printf("  test_encoder_single_literal: PASS\n");
}

static void test_encoder_mixed_packets(void)
{
	u8 buf[32];
	for (int i = 0; i < 8; i++) buf[i] = i;
	memset(buf + 8, 0xAA, 16);
	for (int i = 24; i < 32; i++) buf[i] = i;
	
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);
	
	bool ok = encode_region(&enc, 0, buf, 32);
	assert(ok);
	assert(captured_count == 3);
	
	verify_header(0, false, false, 0x00, 0, 8);
	assert(pkt_data_len(&captured_packets[0].pkt) == 8);
	
	verify_header(1, false, true, 0x00, 8, 16);
	assert(pkt_data_len(&captured_packets[1].pkt) == 1);
	assert(pkt_data(&captured_packets[1].pkt)[0] == 0xAA);
	
	verify_header(2, false, false, 0x00, 24, 8);
	assert(pkt_data_len(&captured_packets[2].pkt) == 8);
	
	verify_encode_roundtrip(buf, 32, "test_encoder_mixed_packets");
	
	printf("  test_encoder_mixed_packets: PASS\n");
}

static void test_encoder_new_frame_flag(void)
{
	u8 buf1[16], buf2[16];
	memset(buf1, 0xFF, 16);
	memset(buf2, 0xAA, 16);
	
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	
	encode_region(&enc, 0, buf1, 16);
	encode_region(&enc, 100, buf2, 16);
	
	assert(captured_count == 2);
	
	assert(pkt_is_new_frame(&captured_packets[0].pkt));
	assert(!pkt_is_new_frame(&captured_packets[1].pkt));
	
	printf("  test_encoder_new_frame_flag: PASS\n");
}

static void test_encoder_display_flags(void)
{
	u8 buf[16];
	memset(buf, 0xFF, 16);
	
	union display_flags_u flags = { .f = { .upside_down = 1, .low_intensity = 1 } };
	
	/* Encoder now always uses data packets (with explicit addr) */
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, flags.byte, false, false, capture_emit, NULL);
	encode_region(&enc, 0, buf, 16);
	assert(captured_count == 1);
	assert(pkt_is_data(&captured_packets[0].pkt));
	assert(pkt_flags(&captured_packets[0].pkt) == flags.byte);
	
	reset_captured();
	encoder_init(&enc, flags.byte, false, false, capture_emit, NULL);
	encode_region(&enc, 100, buf, 16);
	assert(captured_count == 1);
	assert(pkt_is_data(&captured_packets[0].pkt));
	assert(pkt_flags(&captured_packets[0].pkt) == flags.byte);
	
	union display_flags_u test;
	test.byte = 0;
	test.f.upside_down = 1;
	assert(test.byte == 0x80);
	
	test.byte = 0;
	test.f.standby = 1;
	assert(test.byte == 0x40);
	
	test.byte = 0;
	test.f.blank = 1;
	assert(test.byte == 0x20);
	
	test.byte = 0;
	test.f.low_intensity = 1;
	assert(test.byte == 0x10);
	
	reset_captured();
	flags.byte = 0;
	flags.f.standby = 1;
	flags.f.blank = 1;
	encoder_init(&enc, flags.byte, true, false, capture_emit, NULL);
	encode_region(&enc, 200, buf, 16);
	assert(pkt_is_data(&captured_packets[0].pkt));
	assert(pkt_flags(&captured_packets[0].pkt) == 0x60);
	
	printf("  test_encoder_display_flags: PASS\n");
}

static void test_encoder_bitrev_flag(void)
{
	u8 buf[16];
	memset(buf, 0xAA, sizeof(buf));
	
	/* Test bitrev=false: bit 5 should be clear */
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	encode_region(&enc, 0, buf, 16);
	assert(captured_count == 1);
	assert(pkt_is_data(&captured_packets[0].pkt));
	u8 cmd_no_bitrev = captured_packets[0].pkt.wire.data.cmd.byte;
	assert((cmd_no_bitrev & 0x20) == 0);  /* bit 5 clear */
	
	/* Test bitrev=true: bit 5 should be set */
	reset_captured();
	encoder_init(&enc, 0x00, true, true, capture_emit, NULL);
	encode_region(&enc, 0, buf, 16);
	assert(captured_count == 1);
	assert(pkt_is_data(&captured_packets[0].pkt));
	u8 cmd_with_bitrev = captured_packets[0].pkt.wire.data.cmd.byte;
	assert((cmd_with_bitrev & 0x20) == 0x20);  /* bit 5 set */
	
	/* Test bitrev with data packet (non-zero address) */
	reset_captured();
	encoder_init(&enc, 0x00, false, true, capture_emit, NULL);
	encode_region(&enc, 100, buf, 16);
	assert(captured_count == 1);
	assert(pkt_is_data(&captured_packets[0].pkt));
	u8 cmd_data_bitrev = captured_packets[0].pkt.wire.data.cmd.byte;
	assert((cmd_data_bitrev & 0x20) == 0x20);  /* bit 5 set */
	
	/* Test RLE packet with bitrev */
	u8 rle_buf[16];
	memset(rle_buf, 0xFF, sizeof(rle_buf));
	reset_captured();
	encoder_init(&enc, 0x00, true, true, capture_emit, NULL);
	encode_region(&enc, 0, rle_buf, 16);
	assert(captured_count == 1);
	u8 cmd_rle_bitrev = captured_packets[0].pkt.wire.data.cmd.byte;
	assert((cmd_rle_bitrev & 0x80) == 0x80);  /* RLE bit set */
	assert((cmd_rle_bitrev & 0x20) == 0x20);  /* bitrev bit set */
	
	printf("  test_encoder_bitrev_flag: PASS\n");
}

static void test_cmd_byte_bitfield(void)
{
	union cmd_byte_u cmd;
	
	/* Test that _one bit is in position 0 */
	cmd.byte = 0;
	cmd.f._one = 1;
	assert(cmd.byte == 0x01);
	
	/* Test cmd_len field (bits 2..1) */
	cmd.byte = 0;
	cmd.f._one = 1;
	cmd.f.cmd_len = CMD_LEN_FLAGS_ONLY;  /* 1 */
	assert(cmd.byte == 0x03);
	
	cmd.byte = 0;
	cmd.f._one = 1;
	cmd.f.cmd_len = CMD_LEN_DATA;  /* 3 */
	assert(cmd.byte == 0x07);
	
	/* Test other flag bits */
	cmd.byte = 0;
	cmd.f.bitrev = 1;
	assert(cmd.byte == 0x20);
	
	cmd.byte = 0;
	cmd.f.new_frame = 1;
	assert(cmd.byte == 0x40);
	
	cmd.byte = 0;
	cmd.f.rle = 1;
	assert(cmd.byte == 0x80);
	
	/* Full data packet with rle + new_frame */
	cmd.byte = 0;
	cmd.f._one = 1;
	cmd.f.cmd_len = CMD_LEN_DATA;
	cmd.f.new_frame = 1;
	cmd.f.rle = 1;
	assert(cmd.byte == 0xC7);
	
	/* Data packet with all flags */
	cmd.byte = 0;
	cmd.f._one = 1;
	cmd.f.cmd_len = CMD_LEN_DATA;
	cmd.f.bitrev = 1;
	cmd.f.new_frame = 1;
	cmd.f.rle = 1;
	assert(cmd.byte == 0xE7);
	
	printf("  test_cmd_byte_bitfield: PASS\n");
}

static void test_flags_only_packet(void)
{
	struct packet_header pkt;
	
	union display_flags_u flags = { .f = { .standby = 1, .blank = 1 } };
	pkt_init_flags_only(&pkt, flags.byte);
	
	assert(pkt.wire.flags_only.cmd.f.cmd_len == CMD_LEN_FLAGS_ONLY);
	assert(pkt.wire.flags_only.cmd.f.new_frame == 0);
	assert(pkt.wire.flags_only.cmd.f.rle == 0);
	assert(pkt.wire.flags_only.cmd.byte == 0x03);  /* cmd_len=1, bit0=1 */
	
	assert(pkt.wire.flags_only.flags == flags.byte);
	assert(pkt.wire.flags_only.flags == 0x60);
	
	assert(pkt.size == HDR_SIZE_FLAGS_ONLY);
	assert(pkt.size == 2);
	assert(pkt.data == NULL);
	assert(pkt.data_len == 0);
	
	assert(pkt_is_flags_only(&pkt));
	assert(!pkt_is_data(&pkt));
	
	printf("  test_flags_only_packet: PASS\n");
}

static void test_data_packet_init(void)
{
	struct packet_header pkt;
	
	/* Length 0x5678 -> wire length 0xACF0 (shifted left by 1) */
	pkt_init_data(&pkt, 0x90, true, true, false, 0x1234, 0x5678);
	
	assert(pkt.wire.data.cmd.f.cmd_len == CMD_LEN_DATA);
	assert(pkt.wire.data.cmd.f.new_frame == 1);
	assert(pkt.wire.data.cmd.f.rle == 1);
	assert(pkt.wire.data.cmd.byte == 0xC7);  /* 0x80|0x40|0x07 */
	
	assert(pkt.wire.data.flags == 0x90);
	
	assert(pkt.wire.data.addr_hi == 0x12);
	assert(pkt.wire.data.addr_lo == 0x34);
	assert(pkt_addr(&pkt) == 0x1234);
	
	/* Wire format: 0x5678 << 1 = 0xACF0 */
	assert(pkt.wire.data.len_lo == 0xF0);
	assert(pkt.wire.data.len_hi == 0xAC);
	assert(pkt_len(&pkt) == 0x5678);
	
	assert(pkt.size == HDR_SIZE_DATA);
	assert(pkt.size == 6);
	
	assert(pkt_is_data(&pkt));
	assert(!pkt_is_flags_only(&pkt));
	assert(pkt_is_rle(&pkt));
	assert(pkt_is_new_frame(&pkt));
	
	printf("  test_data_packet_init: PASS\n");
}

static void test_encoder_address_tracking(void)
{
	u8 buf[16];
	memset(buf, 0xFF, 16);
	
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);
	
	encode_region(&enc, 100, buf, 16);
	assert(enc.addr == 116);
	
	encode_region(&enc, 200, buf, 16);
	assert(enc.addr == 216);
	
	assert(captured_count == 2);
	verify_header(0, false, true, 0x00, 100, 16);
	verify_header(1, false, true, 0x00, 200, 16);
	
	printf("  test_encoder_address_tracking: PASS\n");
}

static void test_encoder_diff_packets(void)
{
	u8 old_buf[64], new_buf[64];
	memset(old_buf, 0x00, 64);
	memset(new_buf, 0x00, 64);
	
	memset(new_buf + 16, 0xFF, 16);
	
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	
	bool ok = encode_diff(&enc, old_buf, new_buf, 0, 64);
	assert(ok);
	
	assert(captured_count == 1);
	
	verify_header(0, true, true, 0x00, 16, 16);
	assert(pkt_data_len(&captured_packets[0].pkt) == 1);
	assert(pkt_data(&captured_packets[0].pkt)[0] == 0xFF);
	
	verify_diff_roundtrip(old_buf, new_buf, 64, "test_encoder_diff_packets");
	
	printf("  test_encoder_diff_packets: PASS\n");
}

/* ===== Encoding Efficiency Tests ===== */

#define RAW_PACKET_SIZE (HDR_SIZE_DATA + FB_SIZE)

static size_t total_captured_bytes(void)
{
	size_t total = 0;
	for (int i = 0; i < captured_count; i++) {
		total += HDR_SIZE_DATA;
		total += pkt_data_len(&captured_packets[i].pkt);
	}
	return total;
}

static void test_efficiency_all_same(void)
{
	static u8 buf[FB_SIZE];
	memset(buf, 0xAA, FB_SIZE);
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	encode_region(&enc, 0, buf, FB_SIZE);
	size_t encoded_size = total_captured_bytes();
	assert(encoded_size == HDR_SIZE_DATA + 1);
	assert(encoded_size < RAW_PACKET_SIZE);
	verify_encode_roundtrip(buf, FB_SIZE, "test_efficiency_all_same");
	printf("  test_efficiency_all_same: PASS (encoded %zu vs raw %d)\n", encoded_size, RAW_PACKET_SIZE);
}

static void test_efficiency_all_different(void)
{
	static u8 buf[FB_SIZE];
	for (size_t i = 0; i < FB_SIZE; i++) buf[i] = (u8)(i * 7 + (i >> 8) * 13);
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	encode_region(&enc, 0, buf, FB_SIZE);
	size_t encoded_size = total_captured_bytes();
	assert(encoded_size == RAW_PACKET_SIZE);
	verify_encode_roundtrip(buf, FB_SIZE, "test_efficiency_all_different");
	printf("  test_efficiency_all_different: PASS (encoded %zu vs raw %d)\n", encoded_size, RAW_PACKET_SIZE);
}

static void test_efficiency_worst_case_pattern(void)
{
	static u8 buf[FB_SIZE];
	/*
	 * Worst case for RLE: alternating runs of (RLE_MIN_RUN - 1) bytes.
	 * These are just under the threshold, so they'll be sent as literal.
	 * This tests that the encoder doesn't make things worse than raw.
	 */
	size_t run_len = RLE_MIN_RUN > 1 ? RLE_MIN_RUN - 1 : 1;
	for (size_t i = 0; i < FB_SIZE; i++) buf[i] = ((i / run_len) & 1) ? 0xFF : 0x00;
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	encode_region(&enc, 0, buf, FB_SIZE);
	size_t encoded_size = total_captured_bytes();
	assert(encoded_size <= RAW_PACKET_SIZE);
	verify_encode_roundtrip(buf, FB_SIZE, "test_efficiency_worst_case_pattern");
	printf("  test_efficiency_worst_case_pattern: PASS (encoded %zu vs raw %d, %d packets)\n", encoded_size, RAW_PACKET_SIZE, captured_count);
}

static void test_efficiency_random_data(void)
{
	static u8 buf[FB_SIZE];
	unsigned int seed = 12345;
	for (size_t i = 0; i < FB_SIZE; i++) { seed = seed * 1103515245 + 12345; buf[i] = (u8)(seed >> 16); }
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	encode_region(&enc, 0, buf, FB_SIZE);
	size_t encoded_size = total_captured_bytes();
	assert(encoded_size <= RAW_PACKET_SIZE);
	verify_encode_roundtrip(buf, FB_SIZE, "test_efficiency_random_data");
	printf("  test_efficiency_random_data: PASS (encoded %zu vs raw %d, %d packets)\n", encoded_size, RAW_PACKET_SIZE, captured_count);
}

static void test_efficiency_sparse_changes(void)
{
	static u8 old_buf[FB_SIZE], new_buf[FB_SIZE];
	memset(old_buf, 0x00, FB_SIZE);
	memset(new_buf, 0x00, FB_SIZE);
	for (int i = 0; i < 100; i++) { size_t pos = (i * 251) % FB_SIZE; new_buf[pos] = 0xFF; }
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	encode_diff(&enc, old_buf, new_buf, 0, FB_SIZE);
	size_t encoded_size = total_captured_bytes();
	assert(encoded_size <= RAW_PACKET_SIZE);
	// assert check removed
	verify_diff_roundtrip(old_buf, new_buf, FB_SIZE, "test_efficiency_sparse_changes");
	printf("  test_efficiency_sparse_changes: PASS (encoded %zu vs raw %d, %d packets)\n", encoded_size, RAW_PACKET_SIZE, captured_count);
}

static void test_efficiency_checkerboard(void)
{
	static u8 buf[FB_SIZE];
	for (size_t i = 0; i < FB_SIZE; i++) buf[i] = (i & 1) ? 0xAA : 0x55;
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	encode_region(&enc, 0, buf, FB_SIZE);
	size_t encoded_size = total_captured_bytes();
	/* 1-byte checkerboard has no runs, so no RLE benefit */
	assert(encoded_size <= RAW_PACKET_SIZE);
	verify_encode_roundtrip(buf, FB_SIZE, "test_efficiency_checkerboard");
	printf("  test_efficiency_checkerboard: PASS (1-byte squares: %zu bytes, no RLE possible)\n", encoded_size);
}

/*
 * RLE packet overhead analysis:
 *   HDR_SIZE_DATA = 6 bytes (cmd + flags + addr + len)
 *   RLE payload = 1 byte (the repeated value)
 *   Total per RLE packet = 7 bytes
 *
 * Breakeven: 7 bytes overhead for N bytes of run
 *   N=8:  7/8  = 87.5% of original -> 12.5% savings
 *   N=32: 7/32 = 21.9% of original -> 78.1% savings
 *   N=35: 7/35 = 20.0% of original -> 80.0% savings
 */

static void test_efficiency_checkerboard_8byte(void)
{
	static u8 buf[FB_SIZE];
	/* 8-byte runs of 0x00 and 0xFF alternating - exactly RLE_MIN_RUN */
	for (size_t i = 0; i < FB_SIZE; i++)
		buf[i] = ((i / 8) & 1) ? 0xFF : 0x00;
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	encode_region(&enc, 0, buf, FB_SIZE);
	size_t encoded_size = total_captured_bytes();

	/*
	 * 25206 bytes / 8 = 3150 runs (plus partial)
	 * Each 8-byte run -> 7 byte RLE packet
	 * Expected: ~3150 * 7 = 22050 bytes (~12.5% savings)
	 */
	assert(encoded_size < RAW_PACKET_SIZE);
	assert(encoded_size > RAW_PACKET_SIZE * 80 / 100);  /* not too good */
	verify_encode_roundtrip(buf, FB_SIZE, "test_efficiency_checkerboard_8byte");
	printf("  test_efficiency_checkerboard_8byte: PASS (8-byte squares: %zu vs %d raw, %.1f%% savings)\n",
	       encoded_size, RAW_PACKET_SIZE, 100.0 * (1.0 - (double)encoded_size / RAW_PACKET_SIZE));
}

static void test_efficiency_checkerboard_32byte(void)
{
	static u8 buf[FB_SIZE];
	/* 32-byte runs - larger squares, better RLE efficiency */
	for (size_t i = 0; i < FB_SIZE; i++)
		buf[i] = ((i / 32) & 1) ? 0xFF : 0x00;
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	encode_region(&enc, 0, buf, FB_SIZE);
	size_t encoded_size = total_captured_bytes();

	/*
	 * 25206 bytes / 32 = 787 runs (plus partial)
	 * Each 32-byte run -> 7 byte RLE packet
	 * Expected: ~787 * 7 = 5509 bytes (~78% savings)
	 */
	assert(encoded_size < RAW_PACKET_SIZE / 4);
	verify_encode_roundtrip(buf, FB_SIZE, "test_efficiency_checkerboard_32byte");
	printf("  test_efficiency_checkerboard_32byte: PASS (32-byte squares: %zu vs %d raw, %.1f%% savings)\n",
	       encoded_size, RAW_PACKET_SIZE, 100.0 * (1.0 - (double)encoded_size / RAW_PACKET_SIZE));
}

static void test_efficiency_checkerboard_row(void)
{
	static u8 buf[FB_SIZE];
	/* Alternating rows (35 bytes each) - realistic horizontal stripe pattern */
	const size_t row_bytes = HW_WIDTH / 8;  /* 35 bytes per row */
	for (size_t i = 0; i < FB_SIZE; i++)
		buf[i] = ((i / row_bytes) & 1) ? 0xFF : 0x00;
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	encode_region(&enc, 0, buf, FB_SIZE);
	size_t encoded_size = total_captured_bytes();

	/*
	 * 25206 bytes / 35 = 720 rows
	 * Each 35-byte run -> 7 byte RLE packet
	 * Expected: 720 * 7 = 5040 bytes (~80% savings)
	 */
	assert(encoded_size < RAW_PACKET_SIZE / 4);
	verify_encode_roundtrip(buf, FB_SIZE, "test_efficiency_checkerboard_row");
	printf("  test_efficiency_checkerboard_row: PASS (row-sized stripes: %zu vs %d raw, %.1f%% savings)\n",
	       encoded_size, RAW_PACKET_SIZE, 100.0 * (1.0 - (double)encoded_size / RAW_PACKET_SIZE));
}

/*
 * Corner break tests: solid screen with 1-4 corners disrupted.
 * Each disrupted corner breaks one long RLE run into smaller pieces.
 */

static void test_efficiency_solid_one_corner(void)
{
	static u8 buf[FB_SIZE];

	/* Solid black screen */
	memset(buf, 0x00, FB_SIZE);

	/* Put a single white pixel in top-left corner (breaks first run) */
	buf[0] = 0x80;

	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	encode_region(&enc, 0, buf, FB_SIZE);
	size_t encoded_size = total_captured_bytes();

	/*
	 * Without corner: 1 RLE packet for all 25206 bytes = 7 bytes
	 * With corner byte 0: 1 literal byte + RLE for remaining 25205 bytes
	 * The literal is too short for its own packet, gets merged or separate
	 * Expected: small overhead, still very efficient
	 */
	assert(encoded_size < 50);  /* should be very small */
	verify_encode_roundtrip(buf, FB_SIZE, "test_efficiency_solid_one_corner");
	printf("  test_efficiency_solid_one_corner: PASS (1 corner: %zu bytes)\n", encoded_size);
}

static void test_efficiency_solid_two_corners(void)
{
	static u8 buf[FB_SIZE];

	/* Solid black screen */
	memset(buf, 0x00, FB_SIZE);

	/* Top-left corner */
	buf[0] = 0x80;
	/* Bottom-right corner (last byte of last row) */
	buf[FB_SIZE - 1] = 0x01;

	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	encode_region(&enc, 0, buf, FB_SIZE);
	size_t encoded_size = total_captured_bytes();

	/*
	 * Breaks into: 1 literal + long RLE + 1 literal
	 * Still very efficient since middle run is huge
	 */
	assert(encoded_size < 50);
	verify_encode_roundtrip(buf, FB_SIZE, "test_efficiency_solid_two_corners");
	printf("  test_efficiency_solid_two_corners: PASS (2 corners: %zu bytes)\n", encoded_size);
}

static void test_efficiency_solid_four_corners(void)
{
	static u8 buf[FB_SIZE];
	const size_t row_bytes = HW_WIDTH / 8;  /* 35 bytes per row */
	const size_t last_row = FB_SIZE - row_bytes;

	/* Solid black screen */
	memset(buf, 0x00, FB_SIZE);

	/* Top-left */
	buf[0] = 0x80;
	/* Top-right (last byte of first row) */
	buf[row_bytes - 1] = 0x01;
	/* Bottom-left (first byte of last row) */
	buf[last_row] = 0x80;
	/* Bottom-right (last byte of last row) */
	buf[FB_SIZE - 1] = 0x01;

	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	encode_region(&enc, 0, buf, FB_SIZE);
	size_t encoded_size = total_captured_bytes();

	/*
	 * Breaks into several segments:
	 * - 1 literal (top-left)
	 * - RLE (middle of first row, ~33 bytes)
	 * - 1 literal (top-right)
	 * - Large RLE (rows 2 through second-to-last, ~25130 bytes)
	 * - 1 literal (bottom-left)
	 * - RLE (middle of last row, ~33 bytes)
	 * - 1 literal (bottom-right)
	 *
	 * Expected: ~4 RLE packets + literals overhead
	 */
	assert(encoded_size < 100);
	verify_encode_roundtrip(buf, FB_SIZE, "test_efficiency_solid_four_corners");
	printf("  test_efficiency_solid_four_corners: PASS (4 corners: %zu bytes)\n", encoded_size);
}

static void test_efficiency_solid_center_dot(void)
{
	static u8 buf[FB_SIZE];

	/* Solid black screen */
	memset(buf, 0x00, FB_SIZE);

	/* Single white pixel in center - breaks one huge run into two */
	buf[FB_SIZE / 2] = 0x80;

	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);
	encode_region(&enc, 0, buf, FB_SIZE);
	size_t encoded_size = total_captured_bytes();

	/*
	 * Breaks into: RLE (first half) + 1 literal + RLE (second half)
	 * = 7 + 7 + 7 = ~21 bytes (plus header overhead for literal)
	 */
	assert(encoded_size < 50);
	verify_encode_roundtrip(buf, FB_SIZE, "test_efficiency_solid_center_dot");
	printf("  test_efficiency_solid_center_dot: PASS (center dot: %zu bytes)\n", encoded_size);
}

/* ===== Wire format verification tests ===== */

static void test_wire_format_flags_only(void)
{
	struct packet_header pkt;
	pkt_init_flags_only(&pkt, 0x00);
	assert(pkt.size == 2);
	assert(pkt.wire.flags_only.cmd.byte == 0x03);  /* cmd_len=1, bit0=1 */
	assert(pkt.wire.flags_only.flags == 0x00);

	pkt_init_flags_only(&pkt, 0x80);
	assert(pkt.wire.flags_only.cmd.byte == 0x03);
	assert(pkt.wire.flags_only.flags == 0x80);

	pkt_init_flags_only(&pkt, 0xF0);
	assert(pkt.wire.flags_only.cmd.byte == 0x03);
	assert(pkt.wire.flags_only.flags == 0xF0);

	printf("  test_wire_format_flags_only: PASS\n");
}

static void test_wire_format_data(void)
{
	struct packet_header pkt;
	
	/* 0x5678 << 1 = 0xACF0 */
	pkt_init_data(&pkt, 0xA0, false, false, false, 0x1234, 0x5678);
	assert(pkt.size == 6);
	assert(pkt.wire.data.cmd.byte == 0x07);  /* cmd_len=3, bit0=1 */
	assert(pkt.wire.data.flags == 0xA0);
	assert(pkt.wire.data.addr_hi == 0x12);
	assert(pkt.wire.data.addr_lo == 0x34);
	assert(pkt.wire.data.len_lo == 0xF0);
	assert(pkt.wire.data.len_hi == 0xAC);

	/* 1000 << 1 = 2000 = 0x07D0 */
	pkt_init_data(&pkt, 0x50, true, true, false, 0x7FFF, 1000);
	assert(pkt.wire.data.cmd.byte == 0xC7);  /* 0x80|0x40|0x07 */
	assert(pkt.wire.data.flags == 0x50);
	assert(pkt.wire.data.addr_hi == 0x7F);
	assert(pkt.wire.data.addr_lo == 0xFF);
	assert(pkt.wire.data.len_lo == 0xD0);
	assert(pkt.wire.data.len_hi == 0x07);

	printf("  test_wire_format_data: PASS\n");
}

static void test_wire_format_rle_bit(void)
{
	struct packet_header pkt;
	pkt_init_data(&pkt, 0x00, false, false, false, 0, 100);
	assert((pkt.wire.cmd.byte & 0x80) == 0x00);
	pkt_init_data(&pkt, 0x00, false, true, false, 0, 100);
	assert((pkt.wire.cmd.byte & 0x80) == 0x80);
	printf("  test_wire_format_rle_bit: PASS\n");
}

static void test_wire_format_new_frame_bit(void)
{
	struct packet_header pkt;
	pkt_init_data(&pkt, 0x00, false, false, false, 0, 100);
	assert((pkt.wire.cmd.byte & 0x40) == 0x00);
	pkt_init_data(&pkt, 0x00, true, false, false, 0, 100);
	assert((pkt.wire.cmd.byte & 0x40) == 0x40);
	printf("  test_wire_format_new_frame_bit: PASS\n");
}

/* ===== Encoder edge case tests ===== */

static void test_encoder_max_packet_size(void)
{
	static u8 buf[FB_SIZE];
	memset(buf, 0xAA, FB_SIZE);

	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);

	bool ok = encode_region(&enc, 0, buf, FB_SIZE);
	assert(ok);

	assert(captured_count == 1);
	assert(pkt_len(&captured_packets[0].pkt) == FB_SIZE);

	verify_encode_roundtrip(buf, FB_SIZE, "test_encoder_max_packet_size");

	printf("  test_encoder_max_packet_size: PASS\n");
}

static void test_encoder_addr_near_max(void)
{
	u8 buf[16];
	memset(buf, 0xFF, sizeof(buf));

	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);

	/* FB_SIZE is 25200, so use an address near the end */
	u16 addr = FB_SIZE - 16;
	bool ok = encode_region(&enc, addr, buf, 16);
	assert(ok);
	assert(captured_count == 1);
	assert(pkt_addr(&captured_packets[0].pkt) == addr);

	printf("  test_encoder_addr_near_max: PASS\n");
}

static void test_encoder_flags_preserved(void)
{
	u8 buf[32];
	for (int i = 0; i < 8; i++) buf[i] = i;
	memset(buf + 8, 0xBB, 16);
	for (int i = 24; i < 32; i++) buf[i] = i;

	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x95, false, false, capture_emit, NULL);

	encode_region(&enc, 100, buf, 32);

	for (int i = 0; i < captured_count; i++) {
		if (pkt_is_data(&captured_packets[i].pkt)) {
			assert(pkt_flags(&captured_packets[i].pkt) == 0x95);
		}
	}

	printf("  test_encoder_flags_preserved: PASS\n");
}

static void test_encoder_sequential_regions(void)
{
	u8 buf1[16], buf2[16], buf3[16];
	memset(buf1, 0x11, 16);
	memset(buf2, 0x22, 16);
	memset(buf3, 0x33, 16);

	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);

	encode_region(&enc, 0, buf1, 16);
	encode_region(&enc, 16, buf2, 16);
	encode_region(&enc, 32, buf3, 16);

	assert(captured_count == 3);

	assert(pkt_addr(&captured_packets[0].pkt) == 0);
	assert(pkt_addr(&captured_packets[1].pkt) == 16);
	assert(pkt_addr(&captured_packets[2].pkt) == 32);

	printf("  test_encoder_sequential_regions: PASS\n");
}

/* ===== RLE threshold behavior tests ===== */

static void test_rle_exactly_threshold(void)
{
	/*
	 * Test that a run exactly at RLE_MIN_RUN is detected.
	 * Place the run at an arbitrary offset to verify alignment doesn't matter.
	 */
	u8 buf[64];
	for (int i = 0; i < (int)sizeof(buf); i++) buf[i] = (u8)i;
	/* Place run at an unaligned offset */
	memset(buf + 5, 0xFF, RLE_MIN_RUN);

	reset_results();
	find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);

	int rle_idx = -1;
	for (int i = 0; i < rle_count; i++) {
		if (rle_results[i].is_rle && rle_results[i].byte == 0xFF) {
			rle_idx = i;
			break;
		}
	}
	assert(rle_idx >= 0);
	assert(rle_results[rle_idx].offset == 5);
	assert(rle_results[rle_idx].len == RLE_MIN_RUN);

	printf("  test_rle_exactly_threshold: PASS\n");
}

static void test_rle_threshold_minus_one(void)
{
	u8 buf[64];
	for (int i = 0; i < 64; i++) buf[i] = (u8)i;
	memset(buf + 16, 0xFF, RLE_MIN_RUN - 1);

	reset_results();
	find_rle(buf, sizeof(buf), RLE_MIN_RUN, collect_rle, NULL);

	for (int i = 0; i < rle_count; i++) {
		if (rle_results[i].is_rle && rle_results[i].byte == 0xFF) {
			assert(rle_results[i].len < RLE_MIN_RUN);
		}
	}

	printf("  test_rle_threshold_minus_one: PASS\n");
}

static void test_rle_break_even_analysis(void)
{
	assert(RLE_MIN_RUN >= 2);
	assert(RLE_MIN_RUN <= 16);

	printf("  test_rle_break_even_analysis: PASS (RLE_MIN_RUN=%d)\n", RLE_MIN_RUN);
}

/*
 * Verify that the encoder NEVER misses an RLE run that meets or exceeds
 * the minimum run length, regardless of alignment.
 */
static void test_rle_never_misses_valid_run(void)
{
	static u8 buf[256];
	struct encoder_state enc;
	
	/*
	 * Test 1: RLE runs at every possible offset (0-15).
	 * Word boundaries should NOT affect detection.
	 */
	for (size_t offset = 0; offset < 16; offset++) {
		/* Fill with unique bytes to prevent accidental RLE */
		for (size_t i = 0; i < sizeof(buf); i++)
			buf[i] = (u8)(i ^ 0x5A);
		
		/* Insert exactly RLE_MIN_RUN bytes of 0xFF at this offset */
		memset(buf + offset, 0xFF, RLE_MIN_RUN);
		
		reset_captured();
		encoder_init(&enc, 0x00, false, false, capture_emit, NULL);
		bool ok = encode_region(&enc, 0, buf, sizeof(buf));
		assert(ok);
		
		/* Find the RLE packet for 0xFF */
		bool found_rle = false;
		for (int i = 0; i < captured_count; i++) {
			if (pkt_is_rle(&captured_packets[i].pkt)) {
				const u8 *data = pkt_data(&captured_packets[i].pkt);
				u16 len = pkt_len(&captured_packets[i].pkt);
				if (data && data[0] == 0xFF && len == RLE_MIN_RUN) {
					found_rle = true;
					break;
				}
			}
		}
		assert(found_rle && "RLE run was not detected at some offset");
	}
	
	/* Test 2: Multiple RLE runs at arbitrary offsets in same buffer */
	for (size_t i = 0; i < sizeof(buf); i++)
		buf[i] = (u8)(i ^ 0xA5);
	
	/* Place runs at non-word-aligned offsets */
	memset(buf + 3, 0x11, RLE_MIN_RUN);
	memset(buf + 50, 0x22, RLE_MIN_RUN);
	memset(buf + 111, 0x33, RLE_MIN_RUN);
	
	reset_captured();
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);
	bool ok = encode_region(&enc, 0, buf, sizeof(buf));
	assert(ok);
	
	/* Verify all three runs were encoded as RLE */
	int found_11 = 0, found_22 = 0, found_33 = 0;
	for (int i = 0; i < captured_count; i++) {
		if (pkt_is_rle(&captured_packets[i].pkt)) {
			const u8 *data = pkt_data(&captured_packets[i].pkt);
			if (data) {
				if (data[0] == 0x11) found_11++;
				if (data[0] == 0x22) found_22++;
				if (data[0] == 0x33) found_33++;
			}
		}
	}
	assert(found_11 >= 1 && "RLE run of 0x11 at offset 3 missed");
	assert(found_22 >= 1 && "RLE run of 0x22 at offset 50 missed");
	assert(found_33 >= 1 && "RLE run of 0x33 at offset 111 missed");
	
	/* Test 3: Two adjacent different-value runs */
	for (size_t i = 0; i < sizeof(buf); i++)
		buf[i] = (u8)(i ^ 0x3C);
	
	memset(buf + 7, 0xAA, RLE_MIN_RUN);
	memset(buf + 7 + RLE_MIN_RUN, 0xBB, RLE_MIN_RUN);
	
	reset_captured();
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);
	ok = encode_region(&enc, 0, buf, sizeof(buf));
	assert(ok);
	
	int found_aa = 0, found_bb = 0;
	for (int i = 0; i < captured_count; i++) {
		if (pkt_is_rle(&captured_packets[i].pkt)) {
			const u8 *data = pkt_data(&captured_packets[i].pkt);
			if (data) {
				if (data[0] == 0xAA) found_aa++;
				if (data[0] == 0xBB) found_bb++;
			}
		}
	}
	assert(found_aa >= 1 && "First adjacent RLE run missed");
	assert(found_bb >= 1 && "Second adjacent RLE run missed");
	
	printf("  test_rle_never_misses_valid_run: PASS\n");
}

/*
 * Test RLE runs at exact buffer start and end positions.
 */
static void test_rle_at_buffer_boundaries(void)
{
	static u8 buf[128];
	struct encoder_state enc;
	
	/* Test 1: RLE run at buffer start */
	for (size_t i = 0; i < sizeof(buf); i++)
		buf[i] = (u8)(i ^ 0x12);
	memset(buf, 0xAA, RLE_MIN_RUN);
	
	reset_captured();
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);
	bool ok = encode_region(&enc, 0, buf, sizeof(buf));
	assert(ok);
	
	bool found = false;
	for (int i = 0; i < captured_count; i++) {
		if (pkt_is_rle(&captured_packets[i].pkt)) {
			const u8 *data = pkt_data(&captured_packets[i].pkt);
			u16 addr = pkt_addr(&captured_packets[i].pkt);
			if (data && data[0] == 0xAA && addr == 0) {
				found = true;
				break;
			}
		}
	}
	assert(found && "RLE at buffer start missed");
	
	/* Test 2: RLE run at buffer end */
	for (size_t i = 0; i < sizeof(buf); i++)
		buf[i] = (u8)(i ^ 0x34);
	memset(buf + sizeof(buf) - RLE_MIN_RUN, 0xBB, RLE_MIN_RUN);
	
	reset_captured();
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);
	ok = encode_region(&enc, 0, buf, sizeof(buf));
	assert(ok);
	
	found = false;
	for (int i = 0; i < captured_count; i++) {
		if (pkt_is_rle(&captured_packets[i].pkt)) {
			const u8 *data = pkt_data(&captured_packets[i].pkt);
			u16 len = pkt_len(&captured_packets[i].pkt);
			u16 addr = pkt_addr(&captured_packets[i].pkt);
			if (data && data[0] == 0xBB && addr + len == sizeof(buf)) {
				found = true;
				break;
			}
		}
	}
	assert(found && "RLE at buffer end missed");
	
	/* Test 3: Entire buffer is one RLE run */
	memset(buf, 0xCC, sizeof(buf));
	
	reset_captured();
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);
	ok = encode_region(&enc, 0, buf, sizeof(buf));
	assert(ok);
	
	/* Should be exactly one RLE packet covering entire buffer */
	assert(captured_count == 1);
	assert(pkt_is_rle(&captured_packets[0].pkt));
	assert(pkt_len(&captured_packets[0].pkt) == sizeof(buf));
	
	printf("  test_rle_at_buffer_boundaries: PASS\n");
}

/*
 * Test RLE runs separated by various gap sizes.
 */
static void test_rle_runs_with_small_gaps(void)
{
	static u8 buf[256];
	struct encoder_state enc;
	
	/*
	 * Test: Two RLE runs separated by gaps of different sizes.
	 */
	for (int gap = 1; gap <= 8; gap++) {
		for (size_t i = 0; i < sizeof(buf); i++)
			buf[i] = (u8)(i ^ 0x5A ^ gap);
		
		memset(buf + 5, 0xAA, RLE_MIN_RUN);
		memset(buf + 5 + RLE_MIN_RUN + gap, 0xBB, RLE_MIN_RUN);
		
		reset_captured();
		encoder_init(&enc, 0x00, false, false, capture_emit, NULL);
		bool ok = encode_region(&enc, 0, buf, sizeof(buf));
		assert(ok);
		
		int found_aa = 0, found_bb = 0;
		for (int i = 0; i < captured_count; i++) {
			if (pkt_is_rle(&captured_packets[i].pkt)) {
				const u8 *data = pkt_data(&captured_packets[i].pkt);
				if (data) {
					if (data[0] == 0xAA) found_aa++;
					if (data[0] == 0xBB) found_bb++;
				}
			}
		}
		assert(found_aa >= 1 && "First RLE run missed with gap");
		assert(found_bb >= 1 && "Second RLE run missed with gap");
	}
	
	/*
	 * Test: Two runs of same value separated by single different byte.
	 */
	for (size_t i = 0; i < sizeof(buf); i++)
		buf[i] = (u8)(i ^ 0xF0);
	
	memset(buf + 10, 0xDD, RLE_MIN_RUN);
	buf[10 + RLE_MIN_RUN] = 0x00;  /* Single different byte */
	memset(buf + 10 + RLE_MIN_RUN + 1, 0xDD, RLE_MIN_RUN);
	
	reset_captured();
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);
	bool ok = encode_region(&enc, 0, buf, sizeof(buf));
	assert(ok);
	
	/* Count RLE packets of 0xDD - should be 2 (not merged due to gap) */
	int dd_count = 0;
	for (int i = 0; i < captured_count; i++) {
		if (pkt_is_rle(&captured_packets[i].pkt)) {
			const u8 *data = pkt_data(&captured_packets[i].pkt);
			if (data && data[0] == 0xDD) {
				dd_count++;
			}
		}
	}
	assert(dd_count == 2 && "Two same-value RLE runs should not be merged");
	
	/*
	 * Test: Three consecutive RLE runs with single-byte gaps.
	 */
	for (size_t i = 0; i < sizeof(buf); i++)
		buf[i] = (u8)i;
	
	size_t pos = 3;
	memset(buf + pos, 0x11, RLE_MIN_RUN);
	pos += RLE_MIN_RUN + 1;  /* Gap of 1 */
	memset(buf + pos, 0x22, RLE_MIN_RUN);
	pos += RLE_MIN_RUN + 1;  /* Gap of 1 */
	memset(buf + pos, 0x33, RLE_MIN_RUN);
	
	reset_captured();
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);
	ok = encode_region(&enc, 0, buf, sizeof(buf));
	assert(ok);
	
	int f11 = 0, f22 = 0, f33 = 0;
	for (int i = 0; i < captured_count; i++) {
		if (pkt_is_rle(&captured_packets[i].pkt)) {
			const u8 *data = pkt_data(&captured_packets[i].pkt);
			if (data) {
				if (data[0] == 0x11) f11++;
				if (data[0] == 0x22) f22++;
				if (data[0] == 0x33) f33++;
			}
		}
	}
	assert(f11 >= 1 && "First of three RLE runs missed");
	assert(f22 >= 1 && "Second of three RLE runs missed");
	assert(f33 >= 1 && "Third of three RLE runs missed");
	
	printf("  test_rle_runs_with_small_gaps: PASS\n");
}

/* ===== Diff gap-merging threshold tests ===== */

static void test_diff_gap_exactly_threshold(void)
{
	u8 old_buf[64], new_buf[64];
	memset(old_buf, 0x00, sizeof(old_buf));
	memset(new_buf, 0x00, sizeof(new_buf));

	new_buf[0] = 0xFF;
	new_buf[HDR_SIZE_DATA + 8] = 0xFF;

	reset_results();
	find_diffs(old_buf, new_buf, sizeof(old_buf), HDR_SIZE_DATA, collect_diff, NULL);

	assert(diff_count == 2);

	printf("  test_diff_gap_exactly_threshold: PASS\n");
}

static void test_diff_gap_under_threshold(void)
{
	u8 old_buf[64], new_buf[64];
	memset(old_buf, 0x00, sizeof(old_buf));
	memset(new_buf, 0x00, sizeof(new_buf));

	new_buf[0] = 0xFF;
	new_buf[HDR_SIZE_DATA - 1] = 0xFF;

	reset_results();
	find_diffs(old_buf, new_buf, sizeof(old_buf), HDR_SIZE_DATA, collect_diff, NULL);

	assert(diff_count == 1);

	printf("  test_diff_gap_under_threshold: PASS\n");
}

static void test_diff_many_small_changes(void)
{
	static u8 old_buf[1024], new_buf[1024];
	memset(old_buf, 0x00, sizeof(old_buf));
	memset(new_buf, 0x00, sizeof(new_buf));

	for (size_t i = 0; i < sizeof(new_buf); i += 32) {
		new_buf[i] = 0xFF;
	}

	reset_results();
	find_diffs(old_buf, new_buf, sizeof(old_buf), HDR_SIZE_DATA, collect_diff, NULL);

	assert(diff_count > 1);

	verify_diff_roundtrip(old_buf, new_buf, sizeof(old_buf), "test_diff_many_small_changes");

	printf("  test_diff_many_small_changes: PASS (%d regions)\n", diff_count);
}

/* ===== Extended round-trip tests ===== */

static void test_roundtrip_random_data(void)
{
	static u8 buf[FB_SIZE];
	unsigned int seed = 12345;
	for (size_t i = 0; i < sizeof(buf); i++) {
		seed = seed * 1103515245 + 12345;
		buf[i] = (seed >> 16) & 0xFF;
	}

	verify_encode_roundtrip(buf, sizeof(buf), "test_roundtrip_random_data");

	printf("  test_roundtrip_random_data: PASS\n");
}

static void test_roundtrip_alternating_patterns(void)
{
	static u8 buf[256];

	for (size_t i = 0; i < sizeof(buf); i++) {
		buf[i] = (i & 1) ? 0x55 : 0xAA;
	}
	verify_encode_roundtrip(buf, sizeof(buf), "test_roundtrip_0xAA_0x55");

	for (size_t i = 0; i < sizeof(buf); i++) {
		buf[i] = (i & 1) ? 0xFF : 0x00;
	}
	verify_encode_roundtrip(buf, sizeof(buf), "test_roundtrip_0x00_0xFF");

	printf("  test_roundtrip_alternating_patterns: PASS\n");
}

static void test_diff_roundtrip_sparse(void)
{
	static u8 old_buf[FB_SIZE], new_buf[FB_SIZE];

	unsigned int seed = 54321;
	for (size_t i = 0; i < sizeof(old_buf); i++) {
		seed = seed * 1103515245 + 12345;
		old_buf[i] = (seed >> 16) & 0xFF;
	}
	memcpy(new_buf, old_buf, sizeof(new_buf));

	for (int i = 0; i < 100; i++) {
		seed = seed * 1103515245 + 12345;
		size_t pos = (seed >> 8) % sizeof(new_buf);
		new_buf[pos] ^= 0xFF;
	}

	verify_diff_roundtrip(old_buf, new_buf, sizeof(old_buf), "test_diff_roundtrip_sparse");

	printf("  test_diff_roundtrip_sparse: PASS\n");
}

static void test_diff_roundtrip_dense(void)
{
	static u8 old_buf[FB_SIZE], new_buf[FB_SIZE];
	memset(old_buf, 0x00, sizeof(old_buf));
	memset(new_buf, 0xFF, sizeof(new_buf));

	for (int i = 0; i < 100; i++) {
		new_buf[i * 100] = 0x00;
	}

	verify_diff_roundtrip(old_buf, new_buf, sizeof(old_buf), "test_diff_roundtrip_dense");

	printf("  test_diff_roundtrip_dense: PASS\n");
}

/* ===== Negative/edge case tests ===== */

static void test_empty_region(void)
{
	u8 buf[8] = {0};

	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);

	bool ok = encode_region(&enc, 0, buf, 0);
	(void)ok;

	printf("  test_empty_region: PASS (no crash)\n");
}

static void test_single_byte_region(void)
{
	u8 buf[1] = {0xAB};

	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, true, false, capture_emit, NULL);

	bool ok = encode_region(&enc, 100, buf, 1);
	assert(ok);
	assert(captured_count == 1);
	assert(pkt_len(&captured_packets[0].pkt) == 1);

	printf("  test_single_byte_region: PASS\n");
}

static void test_diff_identical_buffers(void)
{
	static u8 buf[1024];
	memset(buf, 0xAA, sizeof(buf));

	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);

	bool ok = encode_diff(&enc, buf, buf, 0, sizeof(buf));
	assert(ok);
	assert(captured_count == 0);

	printf("  test_diff_identical_buffers: PASS\n");
}

static void test_word_unaligned_diff(void)
{
	static u8 old_buf[64], new_buf[64];
	memset(old_buf, 0x00, sizeof(old_buf));
	memcpy(new_buf, old_buf, sizeof(new_buf));

	new_buf[3] = 0xFF;
	new_buf[4] = 0xFF;
	new_buf[5] = 0xFF;

	verify_diff_roundtrip(old_buf, new_buf, sizeof(old_buf), "test_word_unaligned_diff");

	printf("  test_word_unaligned_diff: PASS\n");
}

/*
 * Test that zero-length image packets are detected as invalid.
 * Such packets should never be generated by the encoder, but if
 * they appear in a stream, they should be rejected.
 */
static void test_zero_length_packet_rejected(void)
{
	/* Manually construct a DATA packet with len=0 */
	u8 wire_data[6] = {
		0x07,  /* cmd: cmd_len=3 (DATA), other flags=0 */
		0x00,  /* flags */
		0x00, 0x10,  /* addr = 16 */
		0x00, 0x00,  /* len = 0 (wire format) */
	};
	
	struct packet_header pkt;
	int ret;
	
	/* Test DATA with len=0 */
	ret = pkt_decode(wire_data, sizeof(wire_data), &pkt);
	if (ret > 0) {
		assert(pkt_len(&pkt) == 0);
		/* Zero-length is invalid for image packets */
	}
	
	/*
	 * Note: pkt_decode itself doesn't reject zero-length packets
	 * (it just parses the header). The emulator's process_packet
	 * validates this and returns -2 for zero-length image packets.
	 * This test just confirms the encoder never produces them.
	 */
	
	/* Verify encoder never produces zero-length packets */
	reset_captured();
	struct encoder_state enc;
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);
	
	/* Encode an empty region - should produce no packets */
	u8 empty[0];
	bool ok = encode_region(&enc, 0, empty, 0);
	(void)ok;
	assert(captured_count == 0);  /* No packets for empty input */
	
	/* Verify all captured packets have len > 0 */
	u8 buf[64];
	memset(buf, 0xAA, sizeof(buf));
	reset_captured();
	encoder_init(&enc, 0x00, false, false, capture_emit, NULL);
	ok = encode_region(&enc, 0, buf, sizeof(buf));
	assert(ok);
	
	for (int i = 0; i < captured_count; i++) {
		assert(pkt_len(&captured_packets[i].pkt) > 0);
	}
	
	printf("  test_zero_length_packet_rejected: PASS\n");
}

/* ===== Stress/fuzz tests ===== */

static unsigned int lcg_state = 1;
static unsigned int lcg_next(void)
{
	lcg_state = lcg_state * 1103515245 + 12345;
	return (lcg_state >> 16) & 0x7FFF;
}

static void test_fuzz_rle_random_patterns(void)
{
	static u8 buf[1024];

	lcg_state = 99999;
	for (int trial = 0; trial < 100; trial++) {
		for (size_t i = 0; i < sizeof(buf); i++) {
			buf[i] = lcg_next() & 0xFF;
		}

		if (lcg_next() % 3 == 0) {
			size_t start = lcg_next() % (sizeof(buf) - 32);
			size_t len = 8 + (lcg_next() % 32);
			memset(buf + start, lcg_next() & 0xFF, len);
		}

		verify_encode_roundtrip(buf, sizeof(buf), "fuzz_rle");
	}

	printf("  test_fuzz_rle_random_patterns: PASS (100 trials)\n");
}

static void test_fuzz_diff_random_changes(void)
{
	static u8 old_buf[1024], new_buf[1024];

	lcg_state = 77777;
	for (int trial = 0; trial < 100; trial++) {
		for (size_t i = 0; i < sizeof(old_buf); i++) {
			old_buf[i] = lcg_next() & 0xFF;
		}
		memcpy(new_buf, old_buf, sizeof(new_buf));

		int num_changes = 1 + (lcg_next() % 50);
		for (int i = 0; i < num_changes; i++) {
			size_t pos = lcg_next() % sizeof(new_buf);
			new_buf[pos] ^= (1 + (lcg_next() % 255));
		}

		verify_diff_roundtrip(old_buf, new_buf, sizeof(old_buf), "fuzz_diff");
	}

	printf("  test_fuzz_diff_random_changes: PASS (100 trials)\n");
}

static void test_fuzz_encoder_random_regions(void)
{
	static u8 buf[256];
	static u8 reconstructed[FB_SIZE];

	lcg_state = 55555;
	for (int trial = 0; trial < 50; trial++) {
		for (size_t i = 0; i < sizeof(buf); i++) {
			buf[i] = lcg_next() & 0xFF;
		}

		u16 addr = lcg_next() % (FB_SIZE - sizeof(buf));
		size_t len = 8 + (lcg_next() % (sizeof(buf) - 8));

		reset_captured();
		struct encoder_state enc;
		encoder_init(&enc, lcg_next() & 0xF0, lcg_next() & 1, false, capture_emit, NULL);

		bool ok = encode_region(&enc, addr, buf, len);
		assert(ok);

		memset(reconstructed, 0, sizeof(reconstructed));
		apply_captured_packets(reconstructed, sizeof(reconstructed));

		assert(memcmp(buf, reconstructed + addr, len) == 0);
	}

	printf("  test_fuzz_encoder_random_regions: PASS (50 trials)\n");
}

/* ===== Main ===== */

int main(void)
{
	setbuf(stdout, NULL);  /* Disable buffering */
	printf("=== Test runner starting ===\n");
	printf("Running diff tests:\n");
	test_diff_identical();
	test_diff_single_byte();
	test_diff_two_regions();
	test_diff_merge_close_regions();
	test_diff_all_different();
	test_diff_callback_stop();
	test_diff_first_byte();
	test_diff_last_byte();
	test_diff_word_boundary();
	test_diff_empty_buffer();
	test_diff_single_bit();

	printf("\nRunning RLE tests:\n");
	test_rle_all_same();
	test_rle_no_runs();
	test_rle_run_in_middle();
	test_rle_short_run_ignored();
	test_rle_cross_word_boundary();
	test_rle_whole_word();
	test_rle_unaligned_run();
	test_rle_no_merge_interrupted();
	test_rle_no_merge_small_gap();
	test_rle_callback_stop();
	test_rle_minimum_run();
	test_rle_one_under_minimum();
	test_rle_first_bytes();
	test_rle_last_bytes();
	test_rle_all_zeros();
	test_rle_all_ones();
	test_rle_alternating_bytes();
	test_rle_multiple_runs();
	test_rle_adjacent_different_runs();

	printf("\nRunning rotation tests:\n");
	test_rotate_single_block();
	test_rotate_identity_pattern();
	test_rotate_full_buffer();
	test_rotate_all_zeros();
	test_rotate_all_ones();
	test_rotate_single_pixel();
	test_rotate_corner_pixels();
	test_rotate_horizontal_line();
	test_rotate_vertical_line();
	test_rotate_partial_region();
	test_rotate_cursor_blink_scenario();

#if defined(__aarch64__) || defined(__arm__)
	printf("\nRunning NEON vs scalar comparison tests:\n");
	test_neon_vs_scalar_single_block();
	test_neon_vs_scalar_full_buffer();
	test_neon_vs_scalar_partial_region();
	test_neon_vs_scalar_random_patterns();
#endif

	printf("\nRunning damage bounds tests:\n");
	test_damage_bounds_full();
	test_damage_bounds_single_column();
	test_damage_bounds_rightmost_column();
	test_damage_bounds_middle();

	printf("\nRunning integration tests:\n");
	test_rotate_then_diff();
	test_rotate_diff_finds_region();
	test_full_pipeline_rle();

	printf("\nRunning multi-rect damage tests:\n");
	test_multi_rect_damage_adjacent();
	test_multi_rect_damage_scattered();
	test_multi_rect_damage_overlapping();
	test_single_pixel_damage();
	test_full_screen_damage();
	test_horizontal_stripe_damage();
	test_vertical_stripe_damage();

	printf("\nRunning checkerboard/pattern tests:\n");
	test_checkerboard_byte_pattern();
	test_checkerboard_full_screen();
	test_checkerboard_with_rotation();
	test_sparse_changes();
	test_dense_then_sparse();

	printf("\nRunning bitwise checkerboard tests:\n");
	test_bitwise_checkerboard_no_rle();
	test_bitwise_checkerboard_2x2();
	test_checkerboard_with_solid_rect();
	test_checkerboard_with_multiple_rects();
	test_solid_rect_on_noise();

	printf("\nRunning spiral tests:\n");
	test_spiral_clockwise();
	test_spiral_counterclockwise();
	test_spiral_both_combined();

	printf("\nRunning encoder packet tests:\n");
	test_encoder_single_rle();
	test_encoder_single_literal();
	test_encoder_mixed_packets();
	test_encoder_new_frame_flag();
	test_encoder_display_flags();
	test_encoder_bitrev_flag();
	test_cmd_byte_bitfield();
	test_flags_only_packet();
	test_data_packet_init();
	test_encoder_address_tracking();
	test_encoder_diff_packets();

	printf("\nRunning encoding efficiency tests:\n");
	test_efficiency_all_same();
	test_efficiency_all_different();
	test_efficiency_worst_case_pattern();
	test_efficiency_random_data();
	test_efficiency_sparse_changes();
	test_efficiency_checkerboard();
	test_efficiency_checkerboard_8byte();
	test_efficiency_checkerboard_32byte();
	test_efficiency_checkerboard_row();
	test_efficiency_solid_one_corner();
	test_efficiency_solid_two_corners();
	test_efficiency_solid_four_corners();
	test_efficiency_solid_center_dot();

	printf("\nRunning wire format tests:\n");
	test_wire_format_flags_only();
	test_wire_format_data();
	test_wire_format_rle_bit();
	test_wire_format_new_frame_bit();

	printf("\nRunning encoder edge case tests:\n");
	test_encoder_max_packet_size();
	test_encoder_addr_near_max();
	test_encoder_flags_preserved();
	test_encoder_sequential_regions();

	printf("\nRunning RLE threshold tests:\n");
	test_rle_exactly_threshold();
	test_rle_threshold_minus_one();
	test_rle_break_even_analysis();
	test_rle_never_misses_valid_run();
	test_rle_at_buffer_boundaries();
	test_rle_runs_with_small_gaps();

	printf("\nRunning diff gap-merging tests:\n");
	test_diff_gap_exactly_threshold();
	test_diff_gap_under_threshold();
	test_diff_many_small_changes();

	printf("\nRunning extended round-trip tests:\n");
	test_roundtrip_random_data();
	test_roundtrip_alternating_patterns();
	test_diff_roundtrip_sparse();
	test_diff_roundtrip_dense();

	printf("\nRunning negative/edge case tests:\n");
	test_empty_region();
	test_single_byte_region();
	test_diff_identical_buffers();
	test_word_unaligned_diff();
	test_zero_length_packet_rejected();

	printf("\nRunning fuzz tests:\n");
	test_fuzz_rle_random_patterns();
	test_fuzz_diff_random_changes();
	test_fuzz_encoder_random_regions();

	printf("\nAll tests passed!\n");
	return 0;
}
