#!/usr/bin/env python3
"""
analyze_record.py - what is actually inside a capture record.

Reads a record (the 44-byte header from docs/04, or a raw file) and reports,
per RF chain: DC offset, spread, how many distinct byte values appear, and the
strongest tone in an FFT. Pure Python; no numpy.

The point is to tell "this is live receiver output" apart from "this is a
frozen word or uninitialised scratch" without squinting at hexdumps.

usage: analyze_record.py FILE [--chains N] [--rate HZ]
"""

import argparse
import cmath
import math
import struct
import sys

HDR = struct.Struct("<8sIIQIIIQ")
MAGIC = b"M7620SDR"


def fft(a):
    """In-place iterative radix-2 Cooley-Tukey."""
    n = len(a)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    length = 2
    while length <= n:
        wl = cmath.exp(-2j * math.pi / length)
        half = length >> 1
        for start in range(0, n, length):
            w = 1 + 0j
            for k in range(start, start + half):
                u = a[k]
                v = a[k + half] * w
                a[k] = u + v
                a[k + half] = u - v
                w *= wl
        length <<= 1
    return a


def describe(name, vals):
    n = len(vals)
    mean = sum(vals) / n
    var = sum((v - mean) ** 2 for v in vals) / n
    uniq = len(set(vals))
    out = ("  %-8s mean %8.2f  sd %7.2f  min %5d  max %5d  distinct %4d"
           % (name, mean, math.sqrt(var), min(vals), max(vals), uniq))
    if uniq == 1:
        out += "   <-- FROZEN"
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--chains", type=int, default=3)
    ap.add_argument("--rate", type=float, default=None)
    args = ap.parse_args()

    data = open(args.file, "rb").read()
    if data[:8] == MAGIC:
        magic, ver, flags, seq, ns, chains, rate, lo = HDR.unpack_from(data)
        payload = data[HDR.size:HDR.size + ns * chains * 2]
        print("header   : %s v%d flags 0x%x seq %d" % (magic.decode(), ver, flags, seq))
        print("           %d samples x %d chains @ %d Hz, lo %.3f MHz"
              % (ns, chains, rate, lo / 1e6))
    else:
        ns = len(data) // (args.chains * 2)
        chains = args.chains
        rate = args.rate or 20e6
        payload = data
        print("no header: assuming %d samples x %d chains @ %.0f Hz" % (ns, chains, rate))

    if args.rate:
        rate = args.rate
    print("payload  : %d bytes (expected %d)" % (len(payload), ns * chains * 2))
    print()

    # de-interleave: sample-major, chain-minor, I then Q
    chans = [[] for _ in range(chains)]
    for s in range(ns):
        for c in range(chains):
            off = (s * chains + c) * 2
            i = payload[off]
            q = payload[off + 1]
            if i > 127:
                i -= 256
            if q > 127:
                q -= 256
            chans[c].append(complex(i, q))

    for c, samples in enumerate(chans):
        i_vals = [int(s.real) for s in samples]
        q_vals = [int(s.imag) for s in samples]
        print("chain %d" % c)
        print(describe("I", i_vals))
        print(describe("Q", q_vals))

        if len(set(i_vals)) == 1 and len(set(q_vals)) == 1:
            print("  verdict : frozen, nothing to transform\n")
            continue

        f = fft(list(samples))
        power = [abs(x) ** 2 for x in f]
        peak = max(range(1, ns // 2), key=lambda k: power[k])
        mean_power = sum(power[1:ns // 2]) / (ns // 2 - 1)
        snr = 10 * math.log10(power[peak] / mean_power) if mean_power else 0
        freq = peak * rate / ns
        # a real tone can land either side of DC; report the sign too
        sign = peak if peak < ns // 2 else peak - ns
        print("  peak    : bin %d  %+.3f MHz  (%.1f dB over average power)"
              % (sign, sign * rate / ns / 1e6, snr))
        print()

    print("If I and Q on chains 0 and 1 are 'FROZEN' but chain 2 varies, the")
    print("PKT window (0x8000-0x10000) is not holding capture data while the")
    print("SMM window (0x4000-0x8000) is.")


if __name__ == "__main__":
    sys.exit(main())
