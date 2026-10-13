#include "capture.h"

static void put(struct mt7620_sample *out, unsigned s, unsigned c,
		int8_t i, int8_t q)
{
	out[s * MT7620_CAPTURE_CHAINS + c].i = i;
	out[s * MT7620_CAPTURE_CHAINS + c].q = q;
}

static void restore(struct mt7620_io *io, uint32_t mac_sys, uint32_t pbf_sys,
		    uint32_t cap_orig)
{
	io->write32(io->ctx, MT7620_PBF_CAP_CTRL, cap_orig);
	io->write32(io->ctx, MT7620_PBF_SYS_CTRL, pbf_sys);
	io->write32(io->ctx, MT7620_PBF_CTRL, 0x00000020u); /* reset packet buffer */
	io->write32(io->ctx, MT7620_PBF_CFG, MT7620_PBF_CFG_RUN);
	io->write32(io->ctx, MT7620_MAC_SYS_CTRL, mac_sys);
}

int mt7620_capture_ex(struct mt7620_io *io, struct mt7620_sample *out,
		      unsigned *nsamples, struct mt7620_capture_trace *tr)
{
	uint32_t mac_sys, pbf_sys, cap_orig, cap, start, smm, pkt;
	unsigned i, k;
	int done = 0;

	if (!io || !io->read32 || !io->write32 || !out || !nsamples)
		return -2;

	mac_sys = io->read32(io->ctx, MT7620_MAC_SYS_CTRL);
	pbf_sys = io->read32(io->ctx, MT7620_PBF_SYS_CTRL);
	cap_orig = io->read32(io->ctx, MT7620_PBF_CAP_CTRL);
	cap = cap_orig;

	/* Stop TX/RX and drain the PBF queues. */
	io->write32(io->ctx, MT7620_MAC_SYS_CTRL, 0);
	io->write32(io->ctx, MT7620_PBF_CFG, 0);

	/* Enter capture mode, source = ADC8 (the normal receive path). */
	io->write32(io->ctx, MT7620_PBF_SYS_CTRL,
		    pbf_sys | MT7620_PBF_SYS_CTRL_CAP_MODE);
	if (tr) {
		tr->pbf_sys_ctrl_set = pbf_sys | MT7620_PBF_SYS_CTRL_CAP_MODE;
		tr->pbf_sys_ctrl_read = io->read32(io->ctx, MT7620_PBF_SYS_CTRL);
	}
	cap &= ~(MT7620_CAP_SRC_ADC8 | MT7620_CAP_ADC6 | MT7620_CAP_OFFSET_MASK);
	io->write32(io->ctx, MT7620_PBF_CAP_CTRL, cap);

	/* Arm the capture, then let the receiver run. */
	io->write32(io->ctx, MT7620_PBF_CAP_CTRL, cap | MT7620_CAP_START);
	io->write32(io->ctx, MT7620_MAC_SYS_CTRL, MT7620_MAC_ENABLE_RX);

	/* Wait for the buffer to fill: CAP_START clears when it is full. */
	for (i = 0; i < 1000; i++) {
		cap = io->read32(io->ctx, MT7620_PBF_CAP_CTRL);
		if (!(cap & MT7620_CAP_START)) {
			done = 1;
			break;
		}
		io->delay_us(io->ctx, 10);
	}
	if (tr) {
		tr->polls = i;
		tr->timed_out = !done;
		tr->cap_ctrl_final = cap;
		tr->start_addr = cap & MT7620_CAP_START_MASK;
	}

	io->write32(io->ctx, MT7620_MAC_SYS_CTRL, 0);
	if (!done) {
		restore(io, mac_sys, pbf_sys, cap_orig);
		return -1;
	}

	start = io->read32(io->ctx, MT7620_PBF_CAP_CTRL) & MT7620_CAP_START_MASK;
	smm = MT7620_SMM_BASE + start * 2;
	pkt = MT7620_PKT_BASE + start * 4;
	smm = (smm / 4) * 4; /* vendor driver aligns SMM to a word */

	/* One SMM word plus two PKT words carry two samples of three chains. */
	for (k = 0; k < MT7620_CAPTURE_SAMPLES / 2; k++) {
		uint32_t sw = io->read32(io->ctx, smm);
		uint32_t p1, p2;

		smm += 4;
		if (smm >= MT7620_SMM_END)
			smm -= (MT7620_SMM_END - MT7620_SMM_BASE);

		p1 = io->read32(io->ctx, pkt);
		pkt += 4;
		if (pkt >= MT7620_PKT_END)
			pkt -= (MT7620_PKT_END - MT7620_PKT_BASE);

		p2 = io->read32(io->ctx, pkt);
		pkt += 4;
		if (pkt >= MT7620_PKT_END)
			pkt -= (MT7620_PKT_END - MT7620_PKT_BASE);

		/* chain 2 lives in SMM, chains 1 and 0 in the PKT word */
		put(out, 2 * k,     2, (int8_t)(sw >> 0),  (int8_t)(sw >> 8));
		put(out, 2 * k,     1, (int8_t)(p1 >> 16), (int8_t)(p1 >> 24));
		put(out, 2 * k,     0, (int8_t)(p1 >> 0),  (int8_t)(p1 >> 8));

		put(out, 2 * k + 1, 2, (int8_t)(sw >> 16), (int8_t)(sw >> 24));
		put(out, 2 * k + 1, 1, (int8_t)(p2 >> 16), (int8_t)(p2 >> 24));
		put(out, 2 * k + 1, 0, (int8_t)(p2 >> 0),  (int8_t)(p2 >> 8));
	}

	restore(io, mac_sys, pbf_sys, cap_orig);

	*nsamples = MT7620_CAPTURE_SAMPLES;
	return 0;
}

int mt7620_capture(struct mt7620_io *io, struct mt7620_sample *out,
		   unsigned *nsamples)
{
	return mt7620_capture_ex(io, out, nsamples, NULL);
}
