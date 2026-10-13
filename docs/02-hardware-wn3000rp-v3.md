# Hardware identification: "TP-Link WN3000 v2"

## Finding

There is no TP-Link product called WN3000. The only real "WN3000" is
**NETGEAR's WN3000RP** (N300 Wi-Fi range extender). The "v2" in the request
matches the Netgear naming, not any TP-Link naming.

## Evidence

Checked against primary sources, not search snippets:

1. **OpenWrt Table of Hardware** (`github.com/openwrt/toh`, generated from
   `https://openwrt.org/_media/toh_dump_tab_separated.zip`; the wiki itself is
   behind an Anubis proof-of-work wall, so the `gh-pages` branch was used):
   - 263 TP-Link devices in the table, **none** named WN3000.
   - No TP-Link model in the table contains the string `3000` at all.
   - The only `WN3000*` entries are Netgear: `netgear_wn3000rp_v1` and
     `netgear_wn3000rp_v3`. There is no `WN3000RPH` (an earlier draft of this
     file claimed one; it is not in the table or anywhere else).
2. **TP-Link regional sitemaps** (`tp-link.com/{us,in,uk,de,fr,au,br,sg,ae,jp,kr,latam}/sitemap.xml`):
   no `wn3000` string in any of them.
3. **TP-Link US download centre** (`tp-link.com/us/support/download/`, the full
   2.3 MB product list): the entire `TL-WN*` adapter line is
   WN7200ND, WN721N, WN723N, WN725N, WN727N, WN751ND, WN781ND, WN8200ND,
   WN821N, WN822N, WN823N, WN851ND, WN881ND, WN951N. **No WN3000.**
   Models containing `3000`: Archer-AX3000, AX3000-Pro, C3000, TX3000E,
   Deco-BE13000, Deco-W3000.
4. **GitHub code search** for `WN3000`, `TL-WN3000`, `WN3000 openwrt`:
   only Netgear WN3000RP and the MT7620A `wn3000rp-v3` DTS. Nothing TP-Link.
5. **Internet Archive availability API** for plausible TP-Link URLs
   (`/us/home-networking/usb-adapter/tl-wn3000/`, `/in/products/details/cat-11_TL-WN3000.html`,
   the old US product URL): zero archived snapshots.

## The device that does exist: NETGEAR WN3000RP

| Variant | SoC | OpenWrt target | Wi-Fi driver |
| --- | --- | --- | --- |
| v1 | Broadcom BCM5357B0 | `brcm47xx` / `mips74k` | brcm (b43/etc.) |
| v2 | *not in OpenWrt ToH; unconfirmed* | — | — |
| v3 | **MediaTek MT7620A** | `ramips` / `mt7620` | **`kmod-rt2800-soc`** (Ralink rt2x00) |

Board facts for v3, from the OpenWrt commit that added the port
(`ramips: add support for Netgear WN3000RPv3`, `7fb11c8ed2`) and from the DTS:

- SoC: MediaTek MT7620A, 1× MIPS 24KEc @ 580 MHz
- **RAM: 32 MB DDR** — *not* 64 MB. The port commit says 32 MB, the wiki says
  `rammb: 32`, and the DTS `NETGEAR_HW_ID := 29764836+8+0+32+2x2+0` carries the
  same 32.
- **Storage: 8 MB NOR SPI flash** — the DTS partition table ends at `0x800000`.
- Wi-Fi: built-in MT7620A 2×2:2, 2.4 GHz only (u.FL connectors).
- Ethernet: 1× 100 Mbit. UART: JP1 header, 57600 8N1.
- Stock firmware is "based on OpenWRT Kamikaze" (same commit).

Supporting files:
- `openwrt/openwrt:target/linux/ramips/image/mt7620.mk` — `Device/netgear_wn3000rp-v3`,
  `SOC := mt7620a`, `IMAGE_SIZE := 7872k`, `NETGEAR_BOARD_ID := WN3000RPv3`.
- `target/linux/ramips/dts/mt7620a_netgear_wn3000rp-v3.dts` +
  `mt7620a_netgear_wn3x00rp.dtsi`. The DTSI wires the radio through the **`&wmac`**
  node (the SoC's integrated 2.4 GHz WMAC), **not** through a PCIe `mt76` radio.
- `target/linux/ramips/mt7620/target.mk`:
  `DEFAULT_PACKAGES += kmod-rt2800-soc wpad-basic-mbedtls swconfig`.
- The same DTSI covers `netgear_wn3100rp-v2` (also MT7620A), so "…RP v2" as a
  Netgear thing exists on that family of boards.

## Why this matters

- **MT7620A is not an `mt76` chip.** OpenWrt drives its integrated radio with
  **rt2x00/rt2800**, a MAC/demodulator driver. The "mt76xx" assumption is wrong
  for this board.
- The integrated WMAC is 2.4 GHz 802.11n only (N300 = 2×2 2.4 GHz). There is no
  5 GHz radio.
- **The wiki's `wlandriver` field is wrong here.** OpenWrt's Table of Hardware
  says `mt76` for this board, but the port commit says "builtin MT7620A",
  `target.mk` installs `kmod-rt2800-soc`, `rt2800soc.c` matches the
  `ralink,rt2880-wifi` node, and `mt76` has no MT7620 binding anywhere. Do not
  trust that one field.

## Release status

The board is in-tree on every branch, but it is **`DEFAULT := n`** on 23.05,
24.10 and main, so:

- the last official release with images is **22.03.5** (factory + sysupgrade);
- 23.05 and 24.10 release `profiles.json` and the current snapshot target do
  not build it at all;
- getting it onto 23.05/24.10/main means a self-build, which is what this
  project wants anyway (the image has to carry the capture agent).

## Information still required

Whatever the box actually is, the device itself must say so. If it is running
OpenWrt (or any Linux):

```sh
cat /tmp/sysinfo/model 2>/dev/null
cat /proc/device-tree/model 2>/dev/null; echo
ubus call system board 2>/dev/null
cat /etc/openwrt_release 2>/dev/null
uname -a
cat /proc/cpuinfo | grep -iE 'system type|machine|processor'
dmesg | grep -iE 'mt76|rt2|ralink|mediatek|mt76[0-9]{4}|mt7[0-9]{3}'
ls -l /sys/class/ieee80211/ ; iw dev ; lspci -nn 2>/dev/null
```

If it is still on stock firmware: a photo of the label (model + version +
FCC ID) is the fastest identifier; the FCC ID resolves to the exact radio.
