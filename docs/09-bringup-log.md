# Bring-up log: getting the capture to return real data

A chronological record of the silicon debugging, including the turns that were
wrong. The point of keeping it is that most of the time was spent on things
that *looked* like data and were not.

Board: Netgear WN3000RP v3, OpenWrt 22.03.7, kernel 5.10.221.
Module: `mt7620sdr_capture.ko`.

## Stage 1: the module loads, the capture "succeeds", the data is fake

`insmod` printed the expected line, debugfs appeared, and `cat capture`
returned exactly 49196 bytes with a correct header. It looked done.

```
0000002c  a1 a2 a3 a4   ->  signed: -95 -94 -93 -92
```

That is a ramp, and every 6 bytes the first 4 repeated exactly. Decoding it
against the wire layout (sample-major, chain-minor) gave:

- chain 0 = (161, 162) constant, chain 1 = (163, 164) constant - the whole
  PKT word frozen at `0xa4a3a2a1` for all 8192 samples
- chain 2 = the two varying bytes

So the PKT window (`0x8000`-`0x10000`) held one word repeated, and the SMM
window (`0x4000`-`0x8000`) held noise that was white, uniform-ish
(kurtosis 2.18) with no DC offset. That is uninitialised memory, not a
receiver.

**Two of our own mistakes made this harder to read than it should have been:**

1. Captures were compared with `cmp` and hashed *whole*. Both include the
   44-byte header, whose `seq` field increments every capture, so any two
   captures always differ. `differ: char 17` was the `seq` field. Useless test.
2. `mt7620_capture_ex()` returns `-1` on a fill timeout, and a bare `-1`
   reaches userspace as `-EPERM`, so `cat` printed `Operation not permitted`
   and looked like a permissions problem. It was a timeout, because the poll
   budget (1000) was barely above the observed 785. Now `-ETIMEDOUT` and 4000.

## Stage 2: the trace showed the engine was fine

Adding `/sys/kernel/debug/mt7620sdr/trace` settled the engine question:

```
before  mac_sys_ctrl 0x0000000c  pbf_sys_ctrl 0x00000e00
        pbf_cfg      0x00f40016  cap_ctrl     0x014000a4
wrote   pbf_sys_ctrl 0x00004e00 (cap_mode bit14 = 1)
readbk  pbf_sys_ctrl 0x00004e00 (cap_mode bit14 = 1)
polls   386   timed_out 0
final   cap_ctrl     0x00000ffc   start addr 0xffc
```

`CAP_MODE` sticks, the PBF clocks (bits 9-11) were already on, the engine
completes, and the start-address field advances between captures. The capture
sequence was never the problem.

## Stage 3: the real cause was a radio that was off

`iw dev` showed `txpower 0.00 dBm` and `set channel` returned `Resource busy`.
With the radio down the ADC is not clocked, nothing is written, and the buffer
reads back whatever was in it. Once `wifi up` gave `txpower 20.00 dBm`, the
same module and the same decode produced:

| | chain 0 | chain 1 | chain 2 |
| --- | --- | --- | --- |
| I mean / sd | -0.13 / 6.24 | +0.11 / 5.81 | 0 / 0 |
| distinct I values | 84 | 70 | **1** |
| kurtosis | 11.7 | 8.4 | - |
| lag-1 autocorr | +0.567 | +0.594 | - |

Chain 2 exactly zero across all 8192 samples, `corr(I,Q) = -0.009`, and a
band-limited spectrum with an out-of-band floor of 0.4. That is a receiver.

## How to tell real data from unwritten SRAM

This is the single most useful thing learned, and it is why the earlier
failures were hard to see:

| | real capture | unwritten SRAM |
| --- | --- | --- |
| lag-1 autocorrelation | +0.5 .. +0.7 | ~0.00 |
| kurtosis | 4 .. 12 | 1.8 .. 1.9 (uniform) |
| spectrum | band-limited, floor ~0.4 .. 3.4 | white, positive/negative ratio ~0.36 |
| out-of-band power (>8 MHz) | ~0.4 .. 3.4 | ~10,881 |
| sd | ~6 .. 18 | ~71 |

**Never rank captures by energy.** Uniform noise at sd ~71 outranks real noise
at sd ~7 every time, so "keep the loudest" reliably selects the garbage. That
mistake cost a round trip: the capture the loop picked as "best" was
`kurtosis 1.92, lag1 -0.001`, i.e. memory. Gate on lag1 > 0.4 first, then
prefer the loudest of the real ones. `tools/score-capture.sh` does this.

## Stage 4: hypotheses that were tested and retired

- **An access point in range.** Nine APs were visible in a scan, but none ever
  appeared in a capture - no flat-topped 20 MHz hump, and the spectral peaks
  scatter between captures (+0.26, +1.77, +2.66, +0.30, +1.84, +1.25, +4.0 MHz)
  with no repeat, so they are noise. The likely reason is that the radio's real
  channel was never confirmed: `lo_hz` prints 2412 regardless of where the PLL
  is, and `iw dev` was never pasted.
- **An 802.11 burst.** Checked for the preamble periodicity (0.8 us, i.e. 16
  samples at 20 Msps or 32 at 40 Msps). Lag 16 and 32 came back at 0.04-0.05
  against lag-1 of 0.60, so there is no preamble - the correlation decays
  smoothly, which is what band-limited noise does.
- **A 2.4 GHz wireless keyboard**, spammed deliberately as a signal source.
  An A/B batch with keys hammered versus hands off showed no difference, and
  the hands-off file was 38x *louder* (total power 26,168,318 vs 682,994).
  A GFSK packet is constant envelope (`sd/mean` ~0.05-0.15); the two files
  measured 1.367 and 0.493. Retired.

## The open problem: the capture is not reliable

```
cat: can't open '/sys/kernel/debug/mt7620sdr/capture': Operation timed out
cat: can't open '/sys/kernel/debug/mt7620sdr/capture': Operation timed out
```

2 of 9 captures failed outright, and quality swings from `lag1 +0.706` to
`lag1 +0.199` to `lag1 ~0.0` with white garbage.

Most likely cause: **rt2800soc is live and competing for the same MAC/PBF
registers.** The capture writes `MAC_SYS_CTRL = 0` and `PBF_CFG = 0` out from
under a running driver, so an in-flight beacon or DMA collides with it. This
would explain the timeouts, the garbage, and why no AP was ever visible.

## Next experiment

Keep the radio calibrated but make the driver quiet, then count the failures:

```sh
iw phy phy0 interface add mon0 type monitor
ip link set mon0 up
iw dev mon0 set channel 1
ip link set wlan0 down
for i in $(seq 1 20); do tools/score-capture.sh; done
```

If reliability jumps, the fix is to stop the driver's TX/RX around each
capture - the rt2x00-patch path in `docs/04` - and not to change the capture
sequence, which the trace already proved correct.
