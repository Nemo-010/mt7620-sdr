# First capture on real silicon

Board: Netgear WN3000RP v3, OpenWrt 22.03.7, kernel 5.10.221.
Module: `mt7620sdr_capture.ko`, vermagic `5.10.221 mod_unload MIPS32_R2 32BIT`.

## It was not working at first, and the failure was informative

The first buffer (`c.bin`) came back with the **PKT** window
(`0x8000`-`0x10000`, chains 0 and 1) holding *one* four-byte word,
`0xa4a3a2a1`, repeated across all 8192 samples, while the **SMM** window
(`0x4000`-`0x8000`, chain 2) held noise that was white, uniform-ish
(kurtosis 2.18) and had no DC offset — statistically what uninitialised
memory looks like, not a receiver.

Two mistakes of ours made that harder to see than it needed to be:

- `c.bin` and a second capture were compared with `cmp` and hashed whole.
  Both include the 44-byte header, whose `seq` field increments every
  capture, so *any* two captures differ. The test was meaningless.
- `mt7620_capture_ex()` returns `-1` on a fill timeout, and a bare `-1`
  reaches userspace as `-EPERM`, so `cat .../capture` printed
  `Operation not permitted` and looked like a permissions problem. It was a
  timeout, because the poll budget (1000) was barely above the observed 785.
  Now `-ETIMEDOUT`, budget 4000.

## The trace that settled it

```
before  mac_sys_ctrl 0x0000000c  pbf_sys_ctrl 0x00000e00
        pbf_cfg      0x00f40016  cap_ctrl     0x014000a4
wrote   pbf_sys_ctrl 0x00004e00 (cap_mode bit14 = 1)
readbk  pbf_sys_ctrl 0x00004e00 (cap_mode bit14 = 1)
polls   386   timed_out 0
final   cap_ctrl     0x00000ffc   start addr 0xffc
```

`CAP_MODE` sticks, the PBF clocks (bits 9-11) were already on, the engine
completes, and the start-address field advances between captures
(`0x260`, `0xffc`), so it is a moving ring pointer.

## The buffer that works

With the radio up, the same capture now gives:

| | chain 0 | chain 1 | chain 2 |
| --- | --- | --- | --- |
| I mean / sd | -0.13 / 6.24 | +0.11 / 5.81 | 0 / 0 |
| I distinct values | 84 | 70 | **1** |
| kurtosis (I) | 11.7 | 8.4 | — |
| lag-1 autocorr (I) | +0.567 | +0.594 | — |
| lag-2 autocorr (I) | -0.068 | — | — |

- **Chain 2 is exactly zero across all 8192 samples.** Uninitialised memory
  cannot do that; an unused ADC chain on a 2x2 radio can.
- `corr(I, Q) = -0.009` inside chain 0, so I and Q are genuinely independent.
- The spectrum is **not white**: energy sits below ~5.6 MHz and there is
  nothing above 6.25 MHz (spectral flatness 0.12).
- Block RMS: mean 8.33, sd 1.90 over 64-sample blocks, coefficient of
  variation 0.228 — steady, not bursty.

## What this means, and what it does not

Working: the PBF capture engine returns per-chain receive data.

Not yet established:

- **The sample rate.** `rate_hz` is a module parameter (20 MHz), not a
  measurement. The occupied band ends near 5.6 MHz *in those assumed units*.
  If the receiver's channel filter is a 20 MHz channel (+-10 MHz), then the
  true rate is roughly `20 * 10 / 5.5` ~= 36-40 Msps, i.e. the header is
  probably wrong by ~2x. This needs a signal with a known width to confirm.
- **I/Q orientation.** Noise cannot tell you which way round I and Q are.
- **The channel.** `lo_hz` is likewise a parameter; it said 2412 MHz
  (channel 1) whether or not that is where the radio was.
