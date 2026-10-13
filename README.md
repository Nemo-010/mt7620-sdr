# mt7620-sdr

Turning a Netgear WN3000RP v3 (MediaTek MT7620A, 32 MB RAM / 8 MB flash,
2.4 GHz 802.11n) into a small
receive-only software-defined receiver, in the spirit of
[h0m3us3r/w1700k-sdr](https://github.com/h0m3us3r/w1700k-sdr) — but honest
about the hardware it actually is.

The MT7620A is not an `mt76` chip; it is a Ralink/rt2800 part. It has no
mainline IQ API. It does have Ralink's **PBF capture engine**, which MediaTek's
own driver uses for calibration and test: it copies the raw receive ADC output
— 8-bit signed I/Q, three RF chains, 8192 samples — into internal MAC SRAM on
command. That is enough for a toy SDR.

| | |
| --- | --- |
| [`docs/01-reference-w1700k-sdr.md`](docs/01-reference-w1700k-sdr.md) | how the reference project works |
| [`docs/02-hardware-wn3000rp-v3.md`](docs/02-hardware-wn3000rp-v3.md) | what the board is, and the "TP-Link WN3000" dead end |
| [`docs/03-mt7620-pbf-capture.md`](docs/03-mt7620-pbf-capture.md) | the capture engine, register by register |
| [`docs/04-architecture.md`](docs/04-architecture.md) | the toy-SDR design and wire format |
| [`docs/05-openwrt.md`](docs/05-openwrt.md) | OpenWrt, mainline rt2x00 vs vendor driver, QEMU |
| [`docs/06-verified-artifacts.md`](docs/06-verified-artifacts.md) | everything below that was actually built and run |
| [`docs/07-node.md`](docs/07-node.md) | running the box as a standalone node, and the PC side |
| `include/mt7620_pbf.h` | register and buffer-layout reference |
| `core/capture.c` | one capture, portable, IO-vtable based |
| `core/capture_test.c` | mock-hardware test of the sequence and decode |
| `kmod/` | the same core as a loadable MT7620 capture module |
| `agent/mt7620agent.c` | device side: stream capture records over TCP |
| `device/` | init script, config and wrapper that make it a standalone node |
| `host/mt7620cap.py` | host receiver: device stream → `.cs16`/`.cf32` + ZMQ |
| `host/mt7620cap.grc` | GNU Radio receiver (ZMQ SUB → frequency + waterfall) |
| `tools/build-target.sh` | build the `.ko` and the agent with an OpenWrt SDK |
| `tools/build-image.sh` | assemble a flashable, LuCI-free image |
| `openwrt/` | board build seed |

## Build and test the core

```sh
make check
```

This compiles `core/capture.c` against a simulated MT7620 MAC, fills the
capture SRAM in the layout the hardware uses, runs a real capture, and checks
all 8192×3 samples, the timeout path, the NULL guards and register restore.
No hardware needed.

## Status

- Capture engine specified from MediaTek's own MT7620 SDK and implemented.
- Core logic verified by the mock test, and the same test passes as a
  `mipsel_24kc` binary under `qemu-mipsel`.
- The capture builds and links as a loadable `.ko` against the target kernel
  (`5.10.176`, every symbol exported), so no full buildroot run is needed.
- A LuCI-free image round-trips through the Image Builder at 5.5 MiB of the
  8 MiB flash, carrying the module and the agent.
- See `docs/06-verified-artifacts.md` for the commands and results.
- **Not yet run on silicon.** The capture path, the sample rate and the I/Q
  orientation all wait on the board being reachable. The module is not
  auto-loaded and the agent is not started, on purpose.

## What this is not

- Not a wideband continuous SDR; captures are 8192-sample bursts.
- Not transmit-capable; receive only.
- Not a replacement for monitor mode for sniffing traffic. Use monitor mode for
  frames and this for IQ.
- **Not GNU Radio on the device.** GNU Radio is not an OpenWrt package, and
  32 MB RAM / 8 MB flash could not hold it anyway. It runs on the host.
