#!/bin/sh
# pbf-bringup.sh - manual harness for one MT7620 PBF capture.
#
# NOT the product. This is for confirming the capture on real silicon and for
# measuring the sample rate, before any agent exists. It has two backends:
#
#   debugfs   a mainline rt2x00 built with an accessible CSR range
#             (/sys/kernel/debug/ieee80211/phyN/<driver>/register/csr_{offset,value})
#   iwpriv    MediaTek's vendor rt2860v2 driver with RALINK_ATE
#             (iwpriv ra0 mac ADDR[=VALUE], iwpriv ra0 bbp ID[=VALUE])
#
# It does not parse the capture into IQ; it writes the raw MAC words to
# capture.raw and prints a summary. Use host/mt7620cap.py to interpret it once
# there is a device agent.
#
# Registers (see docs/03): PBF_SYS_CTRL 0x400, PBF_CFG 0x408, PBF_CTRL 0x410,
# PBF_CAP_CTRL 0x440, MAC_SYS_CTRL 0x1004. Capture SRAM: SMM 0x4000..0x8000,
# PKT 0x8000..0x10000. CAP_START = bit30 of 0x440.

set -e

IFACE=${IFACE:-ra0}
PHY=${PHY:-phy0}
DRV=${DRV:-rt2800pci}
MODE=${MODE:-iwpriv}
OUT=${OUT:-capture.raw}

dbg=/sys/kernel/debug/ieee80211/$PHY/$DRV/register

csr_w() { # offset value (mac)
	if [ "$MODE" = debugfs ]; then
		# csr_offset is in words past CSR_REG_BASE (0x1000). This only
		# works if the driver's CSR range has been widened to cover the
		# PBF block and the capture SRAM.
		echo "need a widened CSR range for offset $1" >&2
		return 1
	else
		iwpriv "$IFACE" mac "$1=$2" >/dev/null
	fi
}

csr_r() {
	if [ "$MODE" = debugfs ]; then
		echo "need a widened CSR range for offset $1" >&2
		return 1
	else
		iwpriv "$IFACE" mac "$1" | sed -n 's/.*= *0x\([0-9a-fA-F]*\).*/\1/p'
	fi
}

echo "== one PBF capture via $MODE =="

# 1. stop TX/RX and drain queues
csr_w 1004 0            # MAC_SYS_CTRL = 0
csr_w 408 0             # PBF_CFG = 0

# 2. capture mode, source ADC8 (normal RX)
csr_w 400 4e80          # PBF_SYS_CTRL: clocks + CAP_MODE (bit14)
csr_w 440 0             # PBF_CAP_CTRL: ADC8, no trigger offset

# 3. arm, then receive
csr_w 440 40000000      # PBF_CAP_CTRL |= CAP_START
csr_w 1004 8            # MAC_SYS_CTRL = ENABLE_RX

# 4. wait for CAP_START to clear (up to 3 s, as the vendor does)
i=0
while [ $i -lt 300 ]; do
	cap=$(csr_r 440)
	[ $((0x$cap & 0x40000000)) -eq 0 ] && break
	usleep 10000 2>/dev/null || sleep 0.01
	i=$((i + 1))
done
csr_w 1004 0            # MAC_SYS_CTRL = 0

echo "capture polled for ~$((i * 10)) ms"
start=$(csr_r 440)
echo "PBF_CAP_CTRL now 0x$start (start addr = 0x$((0x$start & 0x1fff)))"

# 5. read 4096 iterations of 1 SMM + 2 PKT words (= 8192 samples/chain)
#    Output is raw little-endian words; decode is in core/capture.c.
: > "$OUT"
k=0
while [ $k -lt 4096 ]; do
	csr_r 4000 >> "$OUT"
	csr_r 8000 >> "$OUT"
	csr_r 8000 >> "$OUT"
	k=$((k + 1))
done

# 6. restore
csr_w 410 20            # PBF_CTRL: reset packet buffer
csr_w 408 f40016        # PBF_CFG
csr_w 1004 c            # MAC_SYS_CTRL

echo "wrote $OUT"
echo "note: reading 0x4000/0x8000 linearly only works for a capture that"
echo "      started at address 0; the real loop must apply the wrap from"
echo "      core/capture.c."
