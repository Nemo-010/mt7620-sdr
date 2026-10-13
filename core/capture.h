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
 * Capture one full buffer. `out` must hold MT7620_CAPTURE_SAMPLES *
 * MT7620_CAPTURE_CHAINS entries, laid out as
 *     out[sample * MT7620_CAPTURE_CHAINS + chain]
 * On success returns 0 and sets *nsamples to MT7620_CAPTURE_SAMPLES.
 * Returns negative on the capture timeout and restores every register.
 */
int mt7620_capture(struct mt7620_io *io, struct mt7620_sample *out,
		   unsigned *nsamples);

#endif /* MT7620_CAPTURE_H */
