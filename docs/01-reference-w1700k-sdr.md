# How `w1700k-sdr` works

Source: <https://github.com/h0m3us3r/w1700k-sdr> (README, `docs/architecture.md`,
and the per-component READMEs in `firmware/`, `kernel/w1700k_sdr/`,
`host/w1700k/`, `host/stream/`).

## Hardware it runs on

- Gemtek W1700K: Airoha **AN7581** SoC (4× Cortex-A53, 2 GiB DRAM, 10 GbE) with
  a MediaTek **MT7996** Wi-Fi 7 radio on PCIe.
- The MT7996 has three RF front ends ("A-dies"): an **MT7976** for 2.4 GHz and
  two **MT7977**s for 5 and 6 GHz.
- The radio's internal processors (WM, WA, DSP) are RISC-V (Andes).

## Why it is even possible

1. The MT7996 PHY has an **ICAP0 capture engine** that can write raw 14-bit
   complex IQ into a ring in SYSRAM.
2. The radio DMAs that ring over PCIe into host DRAM (WFDMA), which the
   driver exposes as 8 KiB slices.
3. The ROM **does not authenticate** the replacement firmware: not the patch's
   hook table, and (behind their CRC hook) not the RAM images either. So they
   can run their own WM/WA on the radio.

Remove any one of those three and the project does not exist. This is the part
that does not transfer to cheaper MediaTek radios.

## The stack, top to bottom

| Layer | What it is |
| --- | --- |
| `platform/` | Their own U-Boot, Linux 6.18 + board patches, read-only BusyBox rootfs (Buildroot) |
| `kernel/w1700k_sdr/` | Out-of-tree driver: firmware download over WFDMA rings, the 1 GiB stream ring mapped read-only to userspace, a zero-copy UDP export, and a mailbox for talking to their firmware |
| `firmware/` | Their own MT7996 firmware: ROM patch (3 hooks), WM (radio bring-up, band set-up, stream producer, RF control), WA (28-bit packer). No MediaTek firmware image is used |
| `host/w1700k/` | Python CLI/`serve` protocol: load, band, tune, gain, filter, stream |
| `host/stream/` | `w1700k-stream`: reunites the UDP export, unpacks, optionally corrects the decimation roll-off, serves complex int16 IQ to every consumer via shared memory |
| `host/spectrum/` | 262144-point FFT + browser spectrum/waterfall, drives the unit |
| `host/recv/` | `sdr-recv`: records the served stream |

## Data path (one sentence per hop)

A-die (LNA/mixer/LO/analog filter) → analog IQ → MT7996 PHY/DFE → ICAP0 capture
engine → 1 MiB SYSRAM ring → WM producer publishes 8 KiB chunks (WM+WA pack
half of them to 28 bits/sample-pair) → MCU DMA TX0 over PCIe → host WFDMA RX0
→ 1 GiB DRAM ring of 8 KiB slices (131072 of them, mapped read-only) → kernel
export sends one UDP datagram per slice, zero copy, on the 10 GbE port → host
`w1700k-stream` reorders/unpacks/corrects → shared ring of complex int16 →
consumers.

At 320 Msps one 10 GbE link cannot carry unpacked 32-bit samples, so half the
chunks are losslessly packed (8 KiB → 7 KiB). The 1 GiB ring absorbs ~0.8 s of
stall.

## Control path

The host never writes a radio register during normal operation. It writes
words into a mailbox page in the WM's patch SRAM; the WM's resumable
interpreter runs each command in slices between stream chunks, so a retune does
not stall a 320 Msps stream. Two kinds of commands share the mailbox:

- **Radio commands**: BRINGUP, SETUP band/lo/bw, RATE, DCOC, CAPTURE, SELECT,
  STREAM.
- **RF commands**: tune (LO in Hz), gain (AGC or manual LNA/LPF), filter
  (20–320 MHz), RC trim, status, band switch.

There are **no channels**. A band is three independent values: LO (any Hz the
synthesizer locks at), sample rate (40/80/160/320 Msps, plus band 2's 640/2),
and analog filter. Bands are set up at load; switching band is one register
write mid-stream.

## Firmware: the real work

`firmware/README.md` is explicit that the bring-up is MediaTek's own
initialisation and channel-set, "decoded record for record against a trace of
MediaTek's firmware on a power-cycled unit, reduced to what reception needs".
They dropped transmit calibration, TX power control and TTG IQ calibration
(whose filters are after the capture tap) and keep the MAC transmit path
disabled. The result is a **receive-only** SDR.

## The takeaways for any clone

1. The special sauce is the radio's capture engine plus a route to run your
   own firmware on the radio. You need both.
2. The host side (`w1700k-stream`, shared memory, spectrum UI) is the reusable,
   transferable half.
3. "Any Hz, no channels" is a property of driving the LO directly — also a
   property of the hardware, not the software.
