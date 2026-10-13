/*
 * mt7620_capture: portable implementation of one MT7620A PBF raw-IQ capture.
 *
 * The register sequence is a direct port of MediaTek's Set_ADCDump_Proc()
 * (rt2860v2: ate/common/rt_ate.c) with the buffer decode from
 * ReadCaptureData() (rt2860v2: common/cmm_txbf_cal.c). It is written against
 * an IO vtable so the exact same code runs in a kernel driver, in userspace
 * over the rt2x00 debugfs, and under a mock in the test.
 */
#ifndef MT7620_CAPTURE_H
#define MT7620_CAPTURE_H

#include <stdint.h>
#include <stddef.h>
#include "mt7620_pbf.h"

struct mt7620_io {
	void *ctx;
	uint32_t (*read32)(void *ctx, uint32_t off);
	void     (*write32)(void *ctx, uint32_t off, uint32_t val);
	uint8_t  (*bbp_read8)(void *ctx, uint8_t reg);
	void     (*bbp_write8)(void *ctx, uint8_t reg, uint8_t val);
	void     (*delay_us)(void *ctx, unsigned us);
};

struct mt7620_sample {
	int8_t i;
	int8_t q;
};

/*
 * Optional record of what the hardware actually did. Filled in by
 * mt7620_capture_ex() when a non-NULL pointer is passed.
 *
 * This exists to separate "the ADC never ran" from "we decoded it wrong". If
 * CAP_MODE does not read back set, capture mode never took effect and the two
 * capture windows are just ordinary MAC memory, whatever bytes happen to be in
 * them.
 */
struct mt7620_capture_trace {
	uint32_t pbf_sys_ctrl_set;	/* value written, with CAP_MODE */
	uint32_t pbf_sys_ctrl_read;	/* read back immediately after */
	unsigned polls;			/* poll iterations before completion */
	int      timed_out;		/* CAP_START never cleared */
	uint32_t cap_ctrl_final;	/* PBF_CAP_CTRL when the poll ended */
	uint32_t start_addr;		/* cap_ctrl_final & 0x1fff */
};

/*
 * Capture one full buffer. `out` must hold MT7620_CAPTURE_SAMPLES *
 * MT7620_CAPTURE_CHAINS entries, laid out as
 *     out[sample * MT7620_CAPTURE_CHAINS + chain]
 * On success returns 0 and sets *nsamples to MT7620_CAPTURE_SAMPLES.
 * Returns negative on the capture timeout and restores every register.
 */
int mt7620_capture(struct mt7620_io *io, struct mt7620_sample *out,
		   unsigned *nsamples);

/* Same, but also reports what the hardware did. `tr` may be NULL. */
int mt7620_capture_ex(struct mt7620_io *io, struct mt7620_sample *out,
		      unsigned *nsamples, struct mt7620_capture_trace *tr);

#endif /* MT7620_CAPTURE_H */
