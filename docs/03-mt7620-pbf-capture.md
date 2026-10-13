# The MT7620A raw-IQ capture engine ("PBF capture")

This is the finding that makes the project possible. The MT7620A radio is not
an `mt76` part and has no mainline IQ API, but the Ralink MAC has a
**Packet Buffer (PBF) capture mode** that dumps the raw receive ADC output —
8-bit signed I/Q, three RF chains — into internal MAC memory. MediaTek's own
driver uses it for TX-beamforming and IQ calibration, and exposes it in
production-test builds as `ADCDump`.

Everything below is taken from MediaTek/Ralink GPL driver source, not guessed:

| Source | Used for |
| --- | --- |
| **MediaTek MT7620 SDK** (`github.com/i80s/mtk-sources`, `mt7620/src/`): `ate/common/rt_ate.c` → `Set_ADCDump_Proc()`, `common/cmm_txbf_cal.c` → `ReadCaptureData()`, `include/chip/rtmp_mac.h` | the MT7620's *own* copy: `PBF_CAP_CTRL 0x0440` in the MT7620 MAC header, plus the same sequence and decode |
| `rt2860v2` (`github.com/wuqiong/rt2860v2-for-openwrt-mt7620`), `ate/common/rt_ate.c` → `Set_ADCDump_Proc()` | the capture register sequence |
| same repo, `common/cmm_txbf_cal.c` → `DoCalibrationCapture()`, `ReadCaptureData()` | buffer decode and chain assignment |
| same repo, `include/chip/rtmp_mac.h`, `include/rtmp.h` | `PBF_*` registers, `CAPTURE_MODE_PACKET_BUFFER` |
| `mtk-wifi-gpl` (`github.com/openwrt/mtk-wifi-gpl`), `include/mac_ral/omac/ral_omac_pbf.h` | newer `PBF_SYS_CTRL`/`PBF_CAP_CTRL` definitions |
| mainline `rt2800.h`, `mt7620a.dtsi` | `MAC_SYS_CTRL`, the `wmac` window |

## The window

The `wmac` node is `0x10180000` with a `0x40000` span
(`arch/mips/boot/dts/ralink/mt7620a.dtsi`). All offsets below are relative to
it, and all of them are inside that span.

## The registers

| Offset | Name | Role in a capture |
| --- | --- | --- |
| `0x0400` | `PBF_SYS_CTRL` | bit 14 `CAP_MODE` enters capture mode |
| `0x0408` | `PBF_CFG` | set to 0 to drain the TX/RX queues; restore to `0x00F40016` |
| `0x0410` | `PBF_CTRL` | write `0x20` to reset the packet buffer afterwards |
| `0x0440` | `PBF_CAP_CTRL` | arm, trigger, source select, buffer start address |
| `0x1004` | `MAC_SYS_CTRL` | bit 3 `ENABLE_RX` runs the receiver during the capture |

`PBF_CAP_CTRL` (`0x0440`) bits, per `Set_ADCDump_Proc()`:

| Bits | Meaning |
| --- | --- |
| `31` | ADC source: `0` = ADC8 (normal receive path), `1` = ADC6 |
| `30` | capture start; hardware clears it when the buffer is full |
| `29` | capture trigger (used by the calibration path) |
| `15` | ADC6 select, paired with bit 31 = 0 |
| `[28:16]` | trigger offset |
| `[12:0]` | start address of this capture inside the capture buffer |

## The captured buffer

While `CAP_MODE` is set, the CSR window aliases two memory slices:

| Region | Address | Contents |
| --- | --- | --- |
| SMM (share memory) | `0x4000`–`0x8000` | chain 2 |
| PKT (packet buffer) | `0x8000`–`0x10000` | chains 0 and 1 |

`ReadCaptureData()` consumes, per **two samples**:

- one **SMM word** — the two samples' chain 2, `I` then `Q` per half-word;
- two **PKT words** — the two samples' chains 0 and 1;
  - byte 0/1 = chain 0 `I`/`Q`, byte 2/3 = chain 1 `I`/`Q`.

Total per capture: **8192 complex samples per chain, three chains** (SMM
16 KiB = chain 2; PKT 32 KiB = chains 0+1). Values are **signed 8-bit**.

> The capture is **one-shot**: the hardware fills the buffer and clears
> `CAP_START`. A continuous stream therefore means re-arming and reading as
> fast as the CPU can, and each grab becomes a burst.

## The sequence

`mt7620_capture()` in `core/capture.c` is exactly this, against an IO vtable:

1. Save `MAC_SYS_CTRL`, `PBF_SYS_CTRL`, `PBF_CAP_CTRL`.
2. `MAC_SYS_CTRL = 0`; `PBF_CFG = 0`.
3. `PBF_SYS_CTRL |= CAP_MODE`; clear `PBF_CAP_CTRL` source and trigger-offset bits.
4. `PBF_CAP_CTRL |= CAP_START`.
5. `MAC_SYS_CTRL = ENABLE_RX`.
6. Poll `PBF_CAP_CTRL` until `CAP_START` clears (vendor allows up to 3 s; the
   real fill is sub-millisecond at 20/40 MHz).
7. `MAC_SYS_CTRL = 0`; read start address from `PBF_CAP_CTRL[12:0]`.
8. Read the SMM/PKT regions with the two-samples-per-iteration decode above.
9. Restore all three saved registers, reset the packet buffer.

## What this gives you

- Real baseband IQ from the 2.4 GHz receiver, at the current channel's LO.
- 2×2 (three chains in the buffer), 802.11n, on any channel 1–13.
- 8192-sample bursts: ~0.4 ms at 20 Msps, ~0.2 ms at 40 Msps.

## What is not yet established

- **The exact sample rate.** The capture is at the BBP/ADC clock; whether that
  is the channel bandwidth, or 2× it, is to be measured on the device (capture
  a known carrier and measure the offset). Until then the host records the
  rate as a parameter and calibration sets it.
- **I/Q orientation.** The two vendor readers disagree. `ReadCaptureData()`
  (used above, and what `core/capture.c` implements) gives chain 2 `I` = SMM
  byte 0 and `Q` = byte 1. `Set_ADCDump_Proc()` prints each pair as `Q` then
  `I`, and only ever reads the SMM *low* half-word, so it silently drops
  chain 2's second sample of each pair. Trust `ReadCaptureData()`; confirm the
  sign/orientation on silicon with a known carrier (a swapped I/Q mirrors the
  spectrum).
- **Throughput ceiling.** Reading 8192 samples costs ~12,288 CSR reads. On a
  MIPS 24KEc at ~580 MHz the capture rate will be low tens of Hz at worst and
  a few hundred at best; this is a toy SDR, not a 20 Msps stream.
- **Coexistence.** Capture mode stops normal RX, so the radio is either a
  normal interface or an SDR, not both at once.
