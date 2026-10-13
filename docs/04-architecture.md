# Toy-SDR architecture

The reference project's shape, reduced to what an MT7620A can actually do.

## What is kept from `w1700k-sdr`

- The radio is the SDR; the host only reassembles and displays.
- The device streams to the host over the network; the host never pokes a
  register.
- The host turns the stream into IQ and feeds spectrum/waterfall/recording.

## What changes

| `w1700k-sdr` | this board |
| --- | --- |
| MT7996 with an ICAP ring, DMA to DRAM | MT7620A PBF capture, 8192 samples into MAC SRAM |
| continuous 40–320 Msps | one-shot 8192-sample bursts, low duty cycle |
| 10 GbE export | 100 Mbit Ethernet |
| own radio firmware | vendor/mainline Ralink driver plus a capture hook |
| 14-bit IQ | 8-bit IQ, 2×2 |

## Data path

```
2.4 GHz RX → BBP/ADC → PBF capture → SMM/PKT SRAM
   device: kmod/mt7620sdr_capture.ko → /sys/kernel/debug/mt7620sdr/capture
   device: mt7620agent - run the capture loop, tag, send over TCP
     │
   host: mt7620cap.py → complex float → ZMQ PUB / .cs16 file
     └── GNU Radio: ZMQ SUB → QT Frequency/Waterfall Sink, recording
```

## Wire format

A capture is 8192 × 3 × 2 = 49,152 bytes, too big for one Ethernet datagram
and slow to fragment, so the device agent uses a length-prefixed **TCP**
stream (lossless, simple):

```
struct mt7620_cap_hdr {          /* little-endian, 44 bytes */
    char     magic[8];           /* "M7620SDR"                      */
    uint32_t version;            /* 1                               */
    uint32_t flags;              /* bit0: single chain              */
    uint64_t seq;                /* capture counter                 */
    uint32_t nsamples;           /* 8192                            */
    uint32_t chains;             /* 1 or 3                          */
    uint32_t sample_rate_hz;     /* measured, see docs/03           */
    uint64_t lo_hz;              /* channel centre                  */
    int8_t   data[];             /* nsamples*chains*2 bytes, I,Q    */
};
```

`data` is interleaved `I,Q` per sample per chain:
`data[(sample*chains + chain)*2 + 0] = I`, `+1 = Q`.

## Device agent

The capture ships as a standalone module, `kmod/mt7620sdr_capture.ko`. It
`ioremap`s the `wmac` window (`0x10180000 + 0x40000`) itself and exposes
`/sys/kernel/debug/mt7620sdr/capture`; each `open()` runs one capture. It does
not patch or depend on rt2x00, so it loads on a stock image, and compiling
`core/capture.c` into it is the whole of the radio work. `docs/06` has the
build, the `vermagic` and the symbol check.

Alternatives, if the standalone module misbehaves on silicon:

1. **Patch mainline rt2x00** to add the same capture as a debugfs entry under
   the phy, wired to `rt2800_register_read/write`. More invasive, but it then
   shares the driver's locking and can quiesce the radio itself.
2. **Vendor `rt2860v2`** with `RALINK_ATE`: it already has
   `iwpriv ra0 set ADCDump`, and raw `mac`/`bbp` pokes. Old kernel, fastest to
   a first capture, good as a reference.
3. **Userspace over a widened rt2x00 debugfs**: mainline's `csr` file stops at
   `0x1000`–`0x17FF`, so it cannot reach `PBF_SYS_CTRL`/`PBF_CAP_CTRL` or the
   capture SRAM without widening `CSR_REG_SIZE`.

The agent above the capture loop is the same in all three cases: capture,
timestamp, send.

## Host

`host/mt7620cap.py`:

- connects to the device agent, parses the records,
- writes a `.cs16` recording (`I,Q` signed 16-bit, GNURadio-native),
- optionally publishes the same samples on a ZMQ PUB socket, so a GNU Radio
  `ZMQ SUB Source` sees a normal complex-float-ish stream.

`host/mt7620cap.grc` is the receiving flowgraph: `ZMQ SUB` into a frequency
sink and a waterfall. `docs/07-node.md` covers running the box as a standalone
node, and the `.cs16`/`.cf32` file paths for `inspectrum`.

Because captures are bursts, each record is a separate, gap-tagged block: the
spectrum is correct, but this will not decode full 802.11 frames across
records. For actual traffic sniffing, run the normal rt2x00 interface in
monitor mode alongside (time-multiplexed), which is the honest division of
labour: **monitor mode sniffs frames, PBF capture gives IQ.**

## Rate budget

100 Mbit Ethernet ≈ 11 MB/s. One chain per capture is 16 KiB; the link allows
roughly 700 captures/s of one chain, or ~230/s of all three. The capture loop
itself is the likely bottleneck first. The agent therefore defaults to one
chain and makes three-chain captures optional.
