# The node, and what runs on your PC

The division of labour is the whole design:

- **The box is a sensor.** It loads the capture module, tunes the radio, and
  serves capture records on one TCP port. It runs no DSP and no GUI.
- **Your PC is the SDR.** It receives the records and turns them into IQ for
  GNU Radio, a file, or a spectrum tool.

That is the same shape as `w1700k-sdr`: a small dumb edge, a smart ground
station.

## On the box

Three files are baked into the image (`device/` in this tree):

| Path | Role |
| --- | --- |
| `/etc/init.d/mt7620sdr` | procd service, `START=95` |
| `/etc/config/mt7620sdr` | channel, port, interface |
| `/usr/bin/mt7620sdr-node` | loads the module, tunes, execs the agent |

It is **disabled by default**, because a capture stops TX/RX on the radio and
that has not been confirmed on silicon yet. Turn it on when you are ready:

```sh
uci set mt7620sdr.node.enabled=1
uci set mt7620sdr.node.channel=6        # ch1 = 2412, ch6 = 2437, ch11 = 2462
uci commit mt7620sdr
/etc/init.d/mt7620sdr start
```

What the wrapper does, in order:

1. `insmod mt7620sdr_capture` (the module ioremaps the `wmac` window);
2. mounts debugfs and checks `/sys/kernel/debug/mt7620sdr/capture` exists;
3. `ip link set wlan0 up` then `iw dev wlan0 set channel N`;
4. `exec mt7620agent -s <capture> -l <port>`.

**Tuning goes through the driver, never around it.** The capture only reads the
ADC; it does not program the synthesiser. So "change channel" means "tell
rt2800 to change channel", which is why the interface has to be up. Whether the
driver tolerates the MAC being poked for ~1 ms per capture is exactly the sort
of thing only the board can answer — see "Still open" below.

## On your PC

```sh
# live: records -> .cs16 recording, and complex float32 on a ZMQ PUB socket
python3 host/mt7620cap.py --tcp NODE:5555 --out chan6.cs16 --zmq tcp://127.0.0.1:5555

# or offline only
python3 host/mt7620cap.py --tcp NODE:5555 --cf32 chan6.cf32
```

`--cf32` is GNU Radio-native complex float32; `--out` is complex int16, which
`inspectrum` and most offline tools read.

### GNU Radio

`host/mt7620cap.grc` is a minimal receiver: `ZMQ SUB Source` (complex,
`tcp://127.0.0.1:5555`) into a frequency sink and a waterfall sink. Open it in
`gnuradio-companion` and run it next to `mt7620cap.py --zmq`.

The ZMQ payload is raw interleaved complex float32 with no framing, which is
what `zeromq.sub_source` expects: `gr-zeromq/lib/base_impl.cc` `recv()`s a
message and maps the bytes straight onto items. `pass_tags` is off, so no
header is involved.

The flowgraph was written against GNU Radio 3.10's block schemas but **could
not be executed here** (no GNU Radio in the build sandbox). `gnuradio-companion`
will tell you at load if your version disagrees.

### Something else

The records are self-describing (44-byte header, see `docs/04`), so anything
that reads complex int16 or float32 works: `inspectrum` for burst-by-burst
eyeballing, `gqrx` is not usable (no file input), and plain `numpy` for
one-off analysis.

## Still open

- The tuning sequence assumes the radio survives a capture with the interface
  up. If it does not, the fallbacks are in `docs/04` (patch rt2x00, or the
  vendor driver).
- The sample rate in the record header is a parameter (`rate_hz` on the
  module), not a measurement. Measure it once and set it.
- Channel hopping is not implemented: the agent serves one configured channel.
  Hopping needs a control path into the capture loop.
