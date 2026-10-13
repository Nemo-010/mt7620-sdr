// SPDX-License-Identifier: GPL-2.0
/*
 * mt7620sdr_capture - expose the MT7620A PBF raw-IQ capture as a debugfs file.
 *
 * The capture arithmetic is core/capture.c, unchanged. This file only supplies
 * the IO vtable (an ioremap of the wmac window) and the debugfs plumbing, and
 * hands userspace the record described in docs/04.
 *
 * Reading the file stops TX/RX and puts the MAC into capture mode, so bring the
 * wireless interface down first. Every register the capture touches is saved
 * and restored by core/capture.c.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/string.h>

#include "capture.h"

#define MT7620_WMAC_PHYS	0x10180000UL
#define MT7620_WMAC_SIZE	0x40000UL

#define MT7620_RATE_HZ_DEFAULT	20000000U
#define MT7620_LO_HZ_DEFAULT	2412000000ULL

struct mt7620sdr_hdr {
	u8 magic[8];
	u32 version;
	u32 flags;
	u64 seq;
	u32 nsamples;
	u32 chains;
	u32 rate;
	u64 lo;
} __packed;

struct mt7620sdr_kctx {
	void __iomem *base;
};

struct mt7620sdr_file {
	char *buf;
	size_t len;
};

static void __iomem *mt7620sdr_base;
static struct dentry *mt7620sdr_dir;
static DEFINE_MUTEX(mt7620sdr_lock);
static u64 mt7620sdr_seq;
static unsigned int captures_ok;
static unsigned int captures_failed;

static unsigned int rate_hz = MT7620_RATE_HZ_DEFAULT;
module_param(rate_hz, uint, 0444);
MODULE_PARM_DESC(rate_hz, "sample rate advertised to the host (Hz)");

static unsigned long long lo_hz = MT7620_LO_HZ_DEFAULT;
module_param(lo_hz, ullong, 0444);
MODULE_PARM_DESC(lo_hz, "centre frequency advertised to the host (Hz)");

static u32 kw_read32(void *ctx, u32 off)
{
	return readl(((struct mt7620sdr_kctx *)ctx)->base + off);
}

static void kw_write32(void *ctx, u32 off, u32 val)
{
	writel(val, ((struct mt7620sdr_kctx *)ctx)->base + off);
}

/* The ADC8 path needs no BBP access; these exist only to fill the vtable. */
static u8 kw_bbp_read8(void *ctx, u8 reg)
{
	return 0;
}

static void kw_bbp_write8(void *ctx, u8 reg, u8 val)
{
}

static void kw_delay_us(void *ctx, unsigned us)
{
	udelay(us);
}

static int mt7620sdr_open(struct inode *inode, struct file *file)
{
	struct mt7620sdr_kctx ctx = { .base = mt7620sdr_base };
	struct mt7620_io io = {
		.ctx = &ctx,
		.read32 = kw_read32,
		.write32 = kw_write32,
		.bbp_read8 = kw_bbp_read8,
		.bbp_write8 = kw_bbp_write8,
		.delay_us = kw_delay_us,
	};
	struct mt7620_sample *samples;
	struct mt7620sdr_hdr *h;
	struct mt7620sdr_file *cf;
	unsigned nsamples = 0, s, c;
	size_t need, off;
	int ret;

	if (!mutex_trylock(&mt7620sdr_lock))
		return -EBUSY;

	samples = vmalloc(MT7620_CAPTURE_SAMPLES * MT7620_CAPTURE_CHAINS *
			  sizeof(*samples));
	if (!samples) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	ret = mt7620_capture(&io, samples, &nsamples);
	if (ret) {
		captures_failed++;
		pr_warn("mt7620sdr: capture failed (%d)\n", ret);
		goto out_free_samples;
	}

	need = sizeof(*h) + (size_t)nsamples * MT7620_CAPTURE_CHAINS * 2;

	cf = kzalloc(sizeof(*cf), GFP_KERNEL);
	if (!cf) {
		ret = -ENOMEM;
		goto out_free_samples;
	}
	cf->buf = vmalloc(need);
	if (!cf->buf) {
		kfree(cf);
		ret = -ENOMEM;
		goto out_free_samples;
	}

	h = (struct mt7620sdr_hdr *)cf->buf;
	memcpy(h->magic, "M7620SDR", 8);
	h->version = 1;
	h->flags = 0;
	h->seq = mt7620sdr_seq++;
	h->nsamples = nsamples;
	h->chains = MT7620_CAPTURE_CHAINS;
	h->rate = rate_hz;
	h->lo = lo_hz;

	off = sizeof(*h);
	for (s = 0; s < nsamples; s++) {
		for (c = 0; c < MT7620_CAPTURE_CHAINS; c++) {
			cf->buf[off++] = samples[s * MT7620_CAPTURE_CHAINS + c].i;
			cf->buf[off++] = samples[s * MT7620_CAPTURE_CHAINS + c].q;
		}
	}
	cf->len = need;
	file->private_data = cf;
	captures_ok++;

	vfree(samples);
	mutex_unlock(&mt7620sdr_lock);
	return 0;

out_free_samples:
	vfree(samples);
out_unlock:
	mutex_unlock(&mt7620sdr_lock);
	return ret;
}

static ssize_t mt7620sdr_read(struct file *file, char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct mt7620sdr_file *cf = file->private_data;

	return simple_read_from_buffer(ubuf, count, ppos, cf->buf, cf->len);
}

static int mt7620sdr_release(struct inode *inode, struct file *file)
{
	struct mt7620sdr_file *cf = file->private_data;

	if (cf) {
		vfree(cf->buf);
		kfree(cf);
		file->private_data = NULL;
	}
	return 0;
}

static const struct file_operations mt7620sdr_fops = {
	.owner = THIS_MODULE,
	.open = mt7620sdr_open,
	.read = mt7620sdr_read,
	.release = mt7620sdr_release,
	.llseek = default_llseek,
};

static int __init mt7620sdr_init(void)
{
	mt7620sdr_base = ioremap(MT7620_WMAC_PHYS, MT7620_WMAC_SIZE);
	if (!mt7620sdr_base)
		return -ENOMEM;

	mt7620sdr_dir = debugfs_create_dir("mt7620sdr", NULL);
	if (IS_ERR_OR_NULL(mt7620sdr_dir)) {
		iounmap(mt7620sdr_base);
		return -ENODEV;
	}

	debugfs_create_file("capture", 0444, mt7620sdr_dir, NULL,
			    &mt7620sdr_fops);
	debugfs_create_u32("captures_ok", 0444, mt7620sdr_dir, &captures_ok);
	debugfs_create_u32("captures_failed", 0444, mt7620sdr_dir,
			   &captures_failed);

	pr_info("mt7620sdr: wmac at 0x%lx, read %u samples x %u chains from "
		"/sys/kernel/debug/mt7620sdr/capture\n",
		(unsigned long)MT7620_WMAC_PHYS, MT7620_CAPTURE_SAMPLES,
		MT7620_CAPTURE_CHAINS);
	return 0;
}

static void __exit mt7620sdr_exit(void)
{
	debugfs_remove_recursive(mt7620sdr_dir);
	iounmap(mt7620sdr_base);
}

module_init(mt7620sdr_init);
module_exit(mt7620sdr_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Neucom Info - mt7620-sdr");
MODULE_DESCRIPTION("MT7620A PBF raw-IQ capture (Netgear WN3000RP v3)");
