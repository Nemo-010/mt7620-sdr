# What was actually built and run

Every claim here is the result of a command that ran, against the real target
ABI. The target release is 22.03.5, the last OpenWrt release that ships images
for this board.

## 1. The capture core runs on the target ABI

```
$ mipsel-openwrt-linux-gcc -O2 -Wall -Wextra -Werror -std=c11 -static \
      -Iinclude -o capture_test.mips core/capture.c core/capture_test.c
$ file capture_test.mips
capture_test.mips: ELF 32-bit LSB executable, MIPS, MIPS32 rel2, statically linked
  Tag_GNU_MIPS_ABI_FP: Soft float
$ qemu-mipsel-static ./capture_test.mips
mt7620 capture core tests
  decode: ok (8192 samples x 3 chains)
  timeout + register restore: ok
  guards: ok
  register restore after success: ok
PASS
```

So the sequence and the decode work as a `mipsel_24kc`, soft-float, musl
binary — not just on the build host.

## 2. The capture ships as a loadable module

The SDK carries the target kernel tree (`linux-5.10.176`) and its
`Module.symvers`, so the capture can be a `.ko` instead of a multi-hour
buildroot run:

```
$ tools/build-target.sh /path/to/openwrt-sdk-22.03.5-ramips-mt7620_...
$ modinfo kmod/mt7620sdr_capture.ko
vermagic:  5.10.176 mod_unload MIPS32_R2 32BIT
```

All 19 undefined symbols (`ioremap_prot`, `debugfs_create_file`,
`simple_read_from_buffer`, `__udelay`, …) are exported by that kernel, so it
links at load time. `core/capture.c` is compiled in **unchanged**; two shim
headers supply `stdint.h`/`stddef.h` out of the kernel's own types.

What it does: `ioremap(0x10180000, 0x40000)` for the `wmac` window, and a
`/sys/kernel/debug/mt7620sdr/capture` file that runs one capture per `open()`
and returns the record from `docs/04`.

## 3. The agent and the host agree

```
$ qemu-mipsel-static ./mt7620agent -s rec.bin -o out3.bin -n 3
seq 42: 2 samples x 3 chains @ 20000000 Hz, lo 2412000000 Hz
$ cmp <(cat rec.bin rec.bin rec.bin) out3.bin        # byte-identical
$ python3 host/mt7620cap.py --file out3.bin --out test.cs16 --limit 3 --no-gap
records: 3
```

The agent is a static `mipsel_24kc` binary (84 KiB stripped). It never parses
I/Q: it reopens the debugfs node (which triggers a fresh capture) and forwards
the record.

## 4. A flashable, LuCI-free image

```
$ tools/build-image.sh /path/to/openwrt-imagebuilder-22.03.5-ramips-mt7620_...
$ ls -l out/
openwrt-...-netgear_wn3000rp-v3-squashfs-factory.bin      5,779,589 bytes
openwrt-...-netgear_wn3000rp-v3-squashfs-sysupgrade.bin   5,780,280 bytes
```

5,780,280 bytes is **68.9 % of the 8 MiB flash**. The image contains
`/usr/bin/mt7620agent`, `/lib/modules/5.10.176/mt7620sdr_capture.ko` and the
`rt2800soc`/`rt2x00`/`mac80211` stack, and no LuCI: the board's default package
set never included it, so "no web UI" costs nothing to arrange.

## 5. GNU Radio on the device: no

Two independent reasons, both checked:

- **It is not an OpenWrt package.** Zero hits for `gnuradio`, `volk` or
  `soapy` in the 22.03.5 `mipsel_24kc` package index. You cannot `opkg install`
  it.
- **It would not fit anyway.** 32 MB RAM / 8 MB flash, and the radio produces
  one-shot 8192-sample bursts, not a continuous stream.

GNU Radio belongs on the host, which is exactly the `w1700k-sdr` split. The
device ships ~92 KiB of binary.

## Still not verified, and why

- **Capture on real silicon.** Nothing here proves the PBF engine answers on
  this specific board; that needs the device.
- **The sample rate.** The ADC/BBP clock is not documented in the sources read
  so far.
- **I/Q orientation.** The two vendor readers disagree (see `docs/03`).

The module is deliberately **not** auto-loaded and the agent is **not** started
by the image: capture stops TX/RX, so both are manual, on purpose.
