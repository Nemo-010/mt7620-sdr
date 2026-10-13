#!/bin/sh
# Assemble a flashable image with an OpenWrt Image Builder.
#
#   tools/build-image.sh /path/to/openwrt-imagebuilder-22.03.5-ramips-mt7620_...
#
# The board's default package set contains no LuCI, so "no web UI" is free.
# The module and the agent from tools/build-target.sh are dropped in through
# FILES= rather than built as .ipk, which keeps this to one step.
set -eu

IB=${1:-${IB:-}}
if [ -z "$IB" ]; then
	echo "usage: $0 /path/to/openwrt-imagebuilder-*" >&2
	exit 2
fi

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
files="$here/out/files"
mod=$(echo "$here"/kmod/mt7620sdr_capture.ko)

[ -f "$mod" ] || { echo "run tools/build-target.sh first" >&2; exit 1; }
[ -f "$here/kmod/mt7620agent" ] || { echo "run tools/build-target.sh first" >&2; exit 1; }

# 22.03.x's prerequisite check wants distutils, which Python >= 3.12 dropped.
# It is not used to assemble an image; a stub satisfies the check.
if ! python3 -c 'import distutils' 2>/dev/null; then
	stub="$here/out/pystub"
	mkdir -p "$stub/distutils"
	: > "$stub/distutils/__init__.py"
	PYTHONPATH="$stub${PYTHONPATH:+:$PYTHONPATH}"
	export PYTHONPATH
fi

rm -rf "$files"
kver=$(modinfo -F vermagic "$mod" | awk '{print $1}')
[ -n "$kver" ] || { echo "cannot read vermagic from $mod" >&2; exit 1; }
echo "== installing module for kernel $kver =="
mkdir -p "$files/usr/bin" "$files/lib/modules/$kver"
cp "$here/kmod/mt7620agent" "$files/usr/bin/mt7620agent"
cp "$mod" "$files/lib/modules/$kver/"
cp -a "$here/device/." "$files/"
chmod 755 "$files/usr/bin/mt7620agent" "$files/usr/bin/mt7620sdr-node" \
	"$files/etc/init.d/mt7620sdr"

make -C "$IB" image \
	PROFILE=netgear_wn3000rp-v3 \
	PACKAGES="kmod-rt2800-soc wpad-basic-wolfssl iw tcpdump" \
	FILES="$files"

mkdir -p "$here/out"
cp "$IB"/bin/targets/ramips/mt7620/openwrt-*-ramips-mt7620-netgear_wn3000rp-v3-*squashfs-*.bin "$here/out/"
cp "$IB"/bin/targets/ramips/mt7620/sha256sums "$here/out/sha256sums"
ls -l "$here/out"
