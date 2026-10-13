/*
 * Mock test for the MT7620 PBF capture engine. It simulates the MAC register
 * space and the capture buffer in the exact layout ReadCaptureData() expects,
 * runs the real capture code, and checks every extracted sample. If the
 * register sequence or the buffer decode were wrong, this fails.
 */
#include "capture.h"

#include <stdio.h>
#include <string.h>

/* The whole wmac CSR window we care about (0x0000..0x10000). */
static uint32_t mem[MT7620_PKT_END / 4];
static int never_complete;

/* Deterministic test signal, signed 8-bit. */
static int8_t pat_i(unsigned s, unsigned c)
{
	return (int8_t)((s * 3 + c * 11 + 7) & 0xff);
}
static int8_t pat_q(unsigned s, unsigned c)
{
	return (int8_t)((s * 5 + c * 13 + 3) & 0xff);
}

static void fill_capture_buffer(void)
{
	unsigned k;

	for (k = 0; k < MT7620_CAPTURE_SAMPLES / 2; k++) {
		uint32_t sw, p1, p2;

		sw  = (uint32_t)(uint8_t)pat_q(2 * k + 1, 2) << 24; /* byte3 */
		sw |= (uint32_t)(uint8_t)pat_i(2 * k + 1, 2) << 16; /* byte2 */
		sw |= (uint32_t)(uint8_t)pat_q(2 * k, 2) << 8;      /* byte1 */
		sw |= (uint32_t)(uint8_t)pat_i(2 * k, 2);           /* byte0 */

		p1  = (uint32_t)(uint8_t)pat_q(2 * k, 1) << 24;     /* byte3 */
		p1 |= (uint32_t)(uint8_t)pat_i(2 * k, 1) << 16;     /* byte2 */
		p1 |= (uint32_t)(uint8_t)pat_q(2 * k, 0) << 8;      /* byte1 */
		p1 |= (uint32_t)(uint8_t)pat_i(2 * k, 0);           /* byte0 */

		p2  = (uint32_t)(uint8_t)pat_q(2 * k + 1, 1) << 24;
		p2 |= (uint32_t)(uint8_t)pat_i(2 * k + 1, 1) << 16;
		p2 |= (uint32_t)(uint8_t)pat_q(2 * k + 1, 0) << 8;
		p2 |= (uint32_t)(uint8_t)pat_i(2 * k + 1, 0);

		mem[(MT7620_SMM_BASE + k * 4) / 4] = sw;
		mem[(MT7620_PKT_BASE + k * 8) / 4] = p1;
		mem[(MT7620_PKT_BASE + k * 8 + 4) / 4] = p2;
	}
}

static uint32_t mock_read32(void *ctx, uint32_t off)
{
	(void)ctx;
	return mem[off / 4];
}

static void mock_write32(void *ctx, uint32_t off, uint32_t val)
{
	(void)ctx;
	mem[off / 4] = val;

	/* Enabling the receiver while the capture is armed fills the buffer. */
	if (off == MT7620_MAC_SYS_CTRL && (val & MT7620_MAC_ENABLE_RX) &&
	    (mem[MT7620_PBF_CAP_CTRL / 4] & MT7620_CAP_START)) {
		if (!never_complete) {
			fill_capture_buffer();
			mem[MT7620_PBF_CAP_CTRL / 4] &= ~MT7620_CAP_START;
		}
	}
}

static uint8_t mock_bbp_read8(void *ctx, uint8_t reg)
{
	(void)ctx;
	return (uint8_t)(0x5a ^ reg);
}

static void mock_bbp_write8(void *ctx, uint8_t reg, uint8_t val)
{
	(void)ctx;
	(void)reg;
	(void)val;
}

static void mock_delay_us(void *ctx, unsigned us)
{
	(void)ctx;
	(void)us;
}

static struct mt7620_io mock_io = {
	.read32 = mock_read32,
	.write32 = mock_write32,
	.bbp_read8 = mock_bbp_read8,
	.bbp_write8 = mock_bbp_write8,
	.delay_us = mock_delay_us,
};

static int test_decode(void)
{
	static struct mt7620_sample out[MT7620_CAPTURE_SAMPLES * MT7620_CAPTURE_CHAINS];
	unsigned n = 0, s, c;
	int failures = 0;

	memset(mem, 0, sizeof(mem));
	never_complete = 0;

	if (mt7620_capture(&mock_io, out, &n) != 0) {
		fprintf(stderr, "FAIL: capture returned error\n");
		return 1;
	}
	if (n != MT7620_CAPTURE_SAMPLES) {
		fprintf(stderr, "FAIL: nsamples %u != %u\n", n, MT7620_CAPTURE_SAMPLES);
		return 1;
	}

	for (s = 0; s < MT7620_CAPTURE_SAMPLES; s++) {
		for (c = 0; c < MT7620_CAPTURE_CHAINS; c++) {
			struct mt7620_sample *p = &out[s * MT7620_CAPTURE_CHAINS + c];
			if (p->i != pat_i(s, c) || p->q != pat_q(s, c)) {
				if (failures < 5)
					fprintf(stderr,
						"FAIL: s=%u c=%u got (%d,%d) want (%d,%d)\n",
						s, c, p->i, p->q, pat_i(s, c), pat_q(s, c));
				failures++;
			}
		}
	}
	return failures ? 1 : 0;
}

static int test_timeout_restores(void)
{
	static struct mt7620_sample out[MT7620_CAPTURE_SAMPLES * MT7620_CAPTURE_CHAINS];
	unsigned n = 0;
	uint32_t mac_before = 0x12345678, pbf_before = 0x00000600,
		 cap_before = 0x80000000;

	memset(mem, 0, sizeof(mem));
	never_complete = 1;
	mem[MT7620_MAC_SYS_CTRL / 4] = mac_before;
	mem[MT7620_PBF_SYS_CTRL / 4] = pbf_before;
	mem[MT7620_PBF_CAP_CTRL / 4] = cap_before;

	if (mt7620_capture(&mock_io, out, &n) == 0) {
		fprintf(stderr, "FAIL: capture should have timed out\n");
		return 1;
	}
	if (mem[MT7620_MAC_SYS_CTRL / 4] != mac_before ||
	    mem[MT7620_PBF_SYS_CTRL / 4] != pbf_before ||
	    mem[MT7620_PBF_CAP_CTRL / 4] != cap_before) {
		fprintf(stderr, "FAIL: registers not restored on timeout\n");
		return 1;
	}
	return 0;
}

static int test_guards(void)
{
	memset(mem, 0, sizeof(mem));
	never_complete = 0;
	if (mt7620_capture(&mock_io, NULL, NULL) == 0) {
		fprintf(stderr, "FAIL: NULL out accepted\n");
		return 1;
	}
	if (mt7620_capture(NULL, NULL, NULL) == 0) {
		fprintf(stderr, "FAIL: NULL io accepted\n");
		return 1;
	}
	return 0;
}

static int test_restore_after_success(void)
{
	static struct mt7620_sample out[MT7620_CAPTURE_SAMPLES * MT7620_CAPTURE_CHAINS];
	unsigned n = 0;
	uint32_t mac_before = 0x0000000c, pbf_before = 0x00000600,
		 cap_before = 0x80000000;

	memset(mem, 0, sizeof(mem));
	never_complete = 0;
	mem[MT7620_MAC_SYS_CTRL / 4] = mac_before;
	mem[MT7620_PBF_SYS_CTRL / 4] = pbf_before;
	mem[MT7620_PBF_CAP_CTRL / 4] = cap_before;

	if (mt7620_capture(&mock_io, out, &n) != 0)
		return 1;
	if (mem[MT7620_MAC_SYS_CTRL / 4] != mac_before ||
	    mem[MT7620_PBF_SYS_CTRL / 4] != pbf_before ||
	    mem[MT7620_PBF_CAP_CTRL / 4] != cap_before) {
		fprintf(stderr, "FAIL: registers not restored after success\n");
		return 1;
	}
	if (mem[MT7620_PBF_SYS_CTRL / 4] & MT7620_PBF_SYS_CTRL_CAP_MODE) {
		fprintf(stderr, "FAIL: CAP_MODE left set\n");
		return 1;
	}
	if (mem[MT7620_PBF_CFG / 4] != MT7620_PBF_CFG_RUN) {
		fprintf(stderr, "FAIL: PBF_CFG not restored\n");
		return 1;
	}
	return 0;
}

static int test_trace(void)
{
	static struct mt7620_sample out[MT7620_CAPTURE_SAMPLES * MT7620_CAPTURE_CHAINS];
	struct mt7620_capture_trace tr;
	unsigned n = 0;

	memset(mem, 0, sizeof(mem));
	memset(&tr, 0, sizeof(tr));
	never_complete = 0;

	if (mt7620_capture_ex(&mock_io, out, &n, &tr) != 0) {
		fprintf(stderr, "FAIL: traced capture errored\n");
		return 1;
	}
	if (!(tr.pbf_sys_ctrl_read & MT7620_PBF_SYS_CTRL_CAP_MODE)) {
		fprintf(stderr, "FAIL: CAP_MODE did not read back set\n");
		return 1;
	}
	if (tr.timed_out) {
		fprintf(stderr, "FAIL: trace reports timeout on a good capture\n");
		return 1;
	}
	if (tr.cap_ctrl_final & MT7620_CAP_START) {
		fprintf(stderr, "FAIL: CAP_START still set in the trace\n");
		return 1;
	}
	return 0;
}

int main(void)
{
	int rc = 0;

	printf("mt7620 capture core tests\n");
	if (test_decode())          { fprintf(stderr, "  decode: FAIL\n"); rc = 1; }
	else                        printf("  decode: ok (8192 samples x 3 chains)\n");
	if (test_timeout_restores()){ fprintf(stderr, "  timeout: FAIL\n"); rc = 1; }
	else                        printf("  timeout + register restore: ok\n");
	if (test_guards())              { fprintf(stderr, "  guards: FAIL\n"); rc = 1; }
	else                        printf("  guards: ok\n");
	if (test_restore_after_success()) { fprintf(stderr, "  restore: FAIL\n"); rc = 1; }
	else                        printf("  register restore after success: ok\n");
	if (test_trace())               { fprintf(stderr, "  trace: FAIL\n"); rc = 1; }
	else                        printf("  trace: ok (CAP_MODE sticks, no timeout)\n");

	if (rc == 0)
		printf("PASS\n");
	return rc;
}
