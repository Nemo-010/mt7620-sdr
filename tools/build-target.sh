#!/bin/sh
# Build the device artefacts against an extracted OpenWrt SDK.
#
#   tools/build-target.sh /path/to/openwrt-sdk-22.03.5-ramips-mt7620_...
#
# Produces kmod/mt7620sdr_capture.ko and kmod/mt7620agent, both mipsel_24kc.
# No root, no buildroot run: the SDK ships the target kernel tree and toolchain.
set -eu

SDK=${1:-${SDK:-}}
if [ -z "$SDK" ]; then
	echo "usage: $0 /path/to/openwrt-sdk-*" >&2
	exit 2
fi

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
crossbin=$(echo "$SDK"/staging_dir/toolchain-mipsel_24kc_gcc-*/bin)
cross="$crossbin/mipsel-openwrt-linux-"
kernel=$(echo "$SDK"/build_dir/target-mipsel_24kc_musl/linux-ramips_mt7620/linux-*)

[ -d "$kernel" ] || { echo "no kernel tree in $SDK" >&2; exit 1; }

STAGING_DIR="$SDK/staging_dir"
export STAGING_DIR

echo "== kernel module =="
make -C "$kernel" M="$here/kmod" ARCH=mips CROSS_COMPILE="$cross" modules
"${cross}strip" --strip-debug "$here/kmod/mt7620sdr_capture.ko"

echo "== agent =="
"${cross}gcc" -O2 -Wall -Wextra -std=gnu11 -static \
	-o "$here/kmod/mt7620agent" "$here/agent/mt7620agent.c"
"${cross}strip" "$here/kmod/mt7620agent"

echo "== built =="
ls -l "$here/kmod/mt7620sdr_capture.ko" "$here/kmod/mt7620agent"
