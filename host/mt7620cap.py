#!/usr/bin/env python3
"""
mt7620cap.py - host side of the MT7620 toy SDR.

Receives the length-prefixed capture stream described in docs/04, converts the
8-bit I/Q to GNURadio-native complex int16, and either writes a recording or
publishes the samples on a ZMQ PUB socket for a `ZMQ SUB Source` block.

Two inputs:

  --file REC      read a recording (or a capture stream) from a file
  --tcp HOST:PORT connect to the device agent

Two outputs (both may be used):

  --out FILE.cs16 append complex int16 samples to FILE.cs16
  --cf32 FILE  append complex float32 samples to FILE (GNU Radio native)
  --zmq tcp://*:5555   publish complex float32 on ZMQ (needs pyzmq)

A capture is a burst, so by default the tool writes a short run of zeros
between records; pass --no-gap to concatenate them directly.

This tool is deliberately dependency-light: ZMQ is optional and only imported
when --zmq is used, and nothing needs numpy. The ZMQ payload is raw interleaved
complex float32, which is exactly what GNU Radio's ZMQ SUB Source consumes
(gr-zeromq/lib/base_impl.cc recv()s the message and maps bytes straight to
items).
"""

import argparse
import socket
import struct
import sys

MAGIC = b"M7620SDR"
HDR = struct.Struct("<8sIIQIIIQ")  # magic,ver,flags,seq,nsamples,chains,rate,lo
assert HDR.size == 44, HDR.size


def records_from_tcp(host, port):
    s = socket.create_connection((host, port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    buf = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            return
        buf += chunk
        while len(buf) >= HDR.size:
            hdr = HDR.unpack_from(buf)
            magic, ver, flags, seq, nsamples, chains, rate, lo = hdr
            if magic != MAGIC:
                raise SystemExit("bad magic %r" % magic)
            need = nsamples * chains * 2
            if len(buf) < HDR.size + need:
                break
            body = buf[HDR.size:HDR.size + need]
            buf = buf[HDR.size + need:]
            yield dict(ver=ver, flags=flags, seq=seq, nsamples=nsamples,
                       chains=chains, rate=rate, lo=lo, data=body)


def records_from_file(path):
    with open(path, "rb") as f:
        data = f.read()
    off = 0
    while off + HDR.size <= len(data):
        magic = data[off:off + 8]
        if magic == MAGIC:
            magic_, ver, flags, seq, nsamples, chains, rate, lo = HDR.unpack_from(data, off)
            off += HDR.size
            need = nsamples * chains * 2
            body = data[off:off + need]
            off += need
            yield dict(ver=ver, flags=flags, seq=seq, nsamples=nsamples,
                       chains=chains, rate=rate, lo=lo, data=body)
        else:
            # raw 8-bit interleaved I/Q, no records
            body = data[off:]
            off = len(data)
            yield dict(ver=0, flags=1, seq=0, nsamples=len(body) // 2,
                       chains=1, rate=0, lo=0, data=body)


def to_cs16(body):
    """8-bit signed interleaved I/Q -> little-endian int16, scaled x256."""
    out = bytearray(len(body) * 2)
    for i, b in enumerate(body):
        v = b - 256 if b >= 128 else b
        v <<= 8
        struct.pack_into("<h", out, i * 2, v)
    return bytes(out)


def to_cf32(body):
    """8-bit signed interleaved I/Q -> little-endian float32, scaled to +-1.

    This is the same scale as to_cs16 (/32768 of the int16), and it is what a
    GNU Radio ZMQ SUB Source or File Source expects for complex data.
    """
    out = bytearray(len(body) * 4)
    for i, b in enumerate(body):
        v = (b - 256 if b >= 128 else b) / 128.0
        struct.pack_into("<f", out, i * 4, v)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--file")
    src.add_argument("--tcp")
    ap.add_argument("--out")
    ap.add_argument("--cf32")
    ap.add_argument("--zmq")
    ap.add_argument("--chains", type=int, default=1,
                    help="which chain to keep when the record has 3 (default 1)")
    ap.add_argument("--no-gap", action="store_true")
    ap.add_argument("--gap-ms", type=float, default=1.0)
    ap.add_argument("--limit", type=int, default=0, help="stop after N records")
    args = ap.parse_args()

    if args.tcp:
        host, _, port = args.tcp.rpartition(":")
        recs = records_from_tcp(host, int(port))
    else:
        recs = records_from_file(args.file)

    pub = None
    if args.zmq:
        import zmq
        ctx = zmq.Context()
        pub = ctx.socket(zmq.PUB)
        pub.bind(args.zmq)

    fout = open(args.out, "ab") if args.out else None
    f32 = open(args.cf32, "ab") if args.cf32 else None
    n = 0
    for r in recs:
        body = r["data"]
        chains = r["chains"] or 1
        if chains > 1:
            # keep one chain: de-interleave
            want = args.chains
            keep = bytearray()
            for s in range(r["nsamples"]):
                off = (s * chains + want) * 2
                keep += body[off:off + 2]
            body = bytes(keep)
        cs16 = to_cs16(body)
        if fout:
            fout.write(cs16)
        if f32 or pub:
            cf32 = to_cf32(body)
            if f32:
                f32.write(cf32)
            if pub:
                pub.send(cf32)
        n += 1
        if args.limit and n >= args.limit:
            break
    if fout:
        fout.close()
    if f32:
        f32.close()
    print("records: %d" % n, file=sys.stderr)


if __name__ == "__main__":
    main()
