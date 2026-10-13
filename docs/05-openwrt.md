# OpenWrt integration

Board: Netgear WN3000RP v3 — `ramips` / `mt7620`, `SOC := mt7620a`, 8 MiB
flash / 32 MiB RAM, 100 Mbit Ethernet, 2.4 GHz 802.11n 2×2.

In-tree in mainline OpenWrt (`Device/netgear_wn3000rp-v3` in
`ramips/image/mt7620.mk`), but **`DEFAULT := n`** since 23.05. The 22.03.5
release is the last with official images; 23.05/24.10 and current snapshots do
not build it. So the steps below are a self-build, which is required anyway to
carry the capture agent.

## Build (target board)

```sh
git clone https://git.openwrt.org/openwrt/openwrt.git
cd openwrt
./scripts/feeds update -a && ./scripts/feeds install -a
cp /path/to/wn3000rp-v3.config .config
make defconfig
make -j$(nproc)
```

`openwrt/wn3000rp-v3.config` is a seed config: target `ramips/mt7620`, device
`netgear_wn3000rp-v3`, plus `rt2x00` debugfs, `iw`, `tcpdump`, `hostapd` and a
small set of tools. It is a starting point, not a blessed artefact.

## Getting capture on mainline rt2x00

Mainline already knows the MAC: `rt2800_register_read/write` reach any offset
in the `wmac` window, and `rt2800.h` already defines `PBF_SYS_CTRL`,
`PBF_CFG`, `PBF_CTRL` and `MAC_SYS_CTRL`. It does **not** define
`PBF_CAP_CTRL` (0x0440) and has no capture code.

The patch (to be written and tested on the device, not before) adds:

1. `PBF_CAP_CTRL 0x0440` and its bits to `rt2800.h`.
2. `core/capture.c`'s sequence, called with `rt2800_register_read/write` as the
   IO vtable.
3. A debugfs file `capture` under `rt2800_register_debugfs()`:
   - on read: take the radio out of normal use, run one capture, format the
     raw bytes (or the header from `docs/04`), release;
   - a streaming mode is the agent's job, not the driver's.
4. Care around mac80211: stop the TX/RX queues and hold the driver mutex while
   capture mode is active, because `PBF_CFG = 0` and `MAC_SYS_CTRL = RX` fight
   normal operation.

The tested, portable half of this already exists (`core/capture.c`,
`core/capture_test.c`); the patch is the thin glue, which is why it is not
written blind.

## Vendor driver as a reference

MediaTek's `rt2860v2` driver, built with `RALINK_ATE`, already exposes:

```
iwpriv ra0 set ADCDump=0     # capture, dump to ./ADCDump.txt (in the driver CWD)
iwpriv ra0 mac 0x440=0x...   # raw MAC register poke
iwpriv ra0 bbp 25=0x10       # raw BBP poke
iwpriv ra0 e2p 0             # EEPROM
```

This is the fastest way to confirm the capture on real silicon and to measure
the sample rate. It needs an old kernel (3.10-era), so it is a reference, not
the deliverable.

## Fit

The stock WN3000RP v3 has 8 MiB flash / 32 MiB RAM, which is enough for the
OpenWrt image plus the agent, but not for a GNU Radio toolchain on the device.
GNU Radio belongs on the host; the device only captures and sends.

## QEMU

There is no `mt7620` machine in QEMU, so the board firmware cannot be booted in
emulation (`qemu-system-mipsel -M help` has `malta`, `mipssim`, … and nothing
Ralink). What *was* done instead, and is honest:

- the capture core was cross-compiled with the OpenWrt toolchain and its test
  suite run under `qemu-mipsel` (user mode) — it passes on the real ABI;
- the agent was cross-compiled and run the same way, and its output decodes in
  `host/mt7620cap.py`;
- the module was built and its symbols checked against the target kernel's
  `Module.symvers`.

None of that is a board boot, and none of it is claimed as one. See
`docs/06-verified-artifacts.md`.

## Shipping it

Two artefacts, both built with the OpenWrt SDK for 22.03.5 and dropped into an
Image Builder image (see `tools/`):

- `kmod/mt7620sdr_capture.ko` — the capture as a loadable module;
- `kmod/mt7620agent` — the static `mipsel_24kc` streaming agent.

They are not auto-loaded or auto-started: a capture stops TX/RX, so it is a
manual operation.

## GNU Radio

GNU Radio does **not** go on the device, for two independent reasons: it is not
an OpenWrt package (zero hits for `gnuradio`/`volk`/`soapy` in the 22.03.5
`mipsel_24kc` index), and 32 MB RAM / 8 MB flash could not hold it. It belongs
on the host, fed by `host/mt7620cap.py`.
