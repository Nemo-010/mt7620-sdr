/*
 * Register and buffer layout of the Ralink/MediaTek "PBF" raw-IQ capture
 * engine, as used by the MT7620A (RT6352) radio in the Netgear WN3000RP v3.
 *
 * Nothing here is guessed. Sources (all MediaTek/Ralink GPL driver code):
 *
 *   rt2860v2 (github.com/wuqiong/rt2860v2-for-openwrt-mt7620)
 *     - ate/common/rt_ate.c        Set_ADCDump_Proc()    the capture sequence
 *     - common/cmm_txbf_cal.c      DoCalibrationCapture()/ReadCaptureData()
 *     - include/chip/rtmp_mac.h    PBF_* registers
 *     - include/rtmp.h             CAPTURE_MODE_PACKET_BUFFER, COMPLEX_VALUE
 *
 *   mtk-wifi-gpl (github.com/openwrt/mtk-wifi-gpl)   -- newer, 802.11ac
 *     - include/mac_ral/omac/ral_omac_pbf.h            PBF_SYS_CTRL/CAP_CTRL
 *
 *   mainline Linux rt2x00
 *     - drivers/net/wireless/ralink/rt2x00/rt2800.h     MAC_SYS_CTRL, PBF_*
 *     - arch/mips/boot/dts/ralink/mt7620a.dtsi          wmac @ 0x10180000, 0x40000
 */
#ifndef MT7620_PBF_H
#define MT7620_PBF_H

/* MAC CSR byte offsets into the wmac window (SoC: 0x10180000 + off). */
#define MT7620_PBF_SYS_CTRL      0x0400u
#define MT7620_PBF_CFG           0x0408u
#define MT7620_PBF_MAX_PCNT      0x040cu
#define MT7620_PBF_CTRL          0x0410u
#define MT7620_PBF_CAP_CTRL      0x0440u
#define MT7620_MAC_SYS_CTRL      0x1004u

/* PBF_SYS_CTRL: bit 14 is "packet buffer capture mode" (CAP_MODE). */
#define MT7620_PBF_SYS_CTRL_CAP_MODE (1u << 14)

/*
 * PBF_CAP_CTRL (0x0440), from Set_ADCDump_Proc() and DoCalibrationCapture():
 *   bit 31     ADC source select: 0 = ADC8 (normal RX path), 1 = ADC6
 *   bit 30     capture start; hardware clears it when the buffer is full
 *   bit 29     capture trigger (used by the calibration path)
 *   bit 15     ADC6 select (paired with bit 31 = 0)
 *   [28:16]    trigger offset
 *   [12:0]     start address of this capture inside the buffer
 */
#define MT7620_CAP_SRC_ADC8      (1u << 31)
#define MT7620_CAP_START         (1u << 30)
#define MT7620_CAP_TRIGGER       (1u << 29)
#define MT7620_CAP_ADC6          (1u << 15)
#define MT7620_CAP_OFFSET_MASK   0x1FFF0000u
#define MT7620_CAP_START_MASK    0x00001FFFu

/* MAC_SYS_CTRL bits (rt2800.h) */
#define MT7620_MAC_ENABLE_RX     (1u << 3)

/*
 * Capture memory inside the same CSR window. While CAP_MODE is set,
 * 0x4000..0x8000 is the "share memory" (SMM) slice and 0x8000..0x10000 is the
 * "packet buffer" (PKT) slice. ReadCaptureData() consumes, per two samples,
 * one SMM word and two PKT words.
 */
#define MT7620_SMM_BASE          0x4000u
#define MT7620_SMM_END           0x8000u
#define MT7620_PKT_BASE          0x8000u
#define MT7620_PKT_END           0x10000u

/*
 * The capture buffer holds 8192 complex samples per RF chain, 8-bit signed
 * I and Q. (SMM = 16 KiB = chain2 only; PKT = 32 KiB = chains 0 and 1.)
 */
#define MT7620_CAPTURE_SAMPLES   8192u
#define MT7620_CAPTURE_CHAINS    3u

/* Values the vendor driver restores after a capture. */
#define MT7620_PBF_CFG_RUN       0x00F40016u

#endif /* MT7620_PBF_H */
