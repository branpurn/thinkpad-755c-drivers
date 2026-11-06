/* SPDX-License-Identifier: GPL-2.0 */
/*
 * VGA RAMDAC entry programming, host-testable.
 *
 * The loaded module calls this only from the s6d_cmap=1 fb_setcmap hook.
 * s6d_cmap defaults off. The numbers -1, -5, -16, -19, and -22 are
 * -EPERM, -EIO, -EBUSY, -ENODEV, and -EINVAL on Linux. The module build
 * checks those values against this kernel.
 *
 * Port sequence, matching this tree's VGA model (tests/vgamodel.c) and
 * the vgastate order used by wd24_save_cmap / wd24_restore_cmap:
 *   3C6 write sets the pixel mask and does not reset the colour component.
 *   3C6 is never read: a read is the hidden-register sequence, and the
 *   model counts it as a violation.
 *   3C8 write selects the write index and sets the component to red.
 *   3C7 write selects the read index and sets the component to red.
 *   3C9 write stores the low 6 bits and advances the component. After
 *   blue, the write index increments. 3C9 read does the same on the
 *   read index. The component counter is one counter for both.
 * A batch writes 3C6=FFh once, then for each entry 3C8, three 3C9
 * writes, 3C7, and three 3C9 reads. It does not stop between the three
 * data cycles. The saved 768-byte snapshot is not an argument and is
 * not modified. A caller that sees -EIO writes that snapshot back with
 * wd24_restore_cmap.
 *
 * X's 16-bit channel becomes the 3C9 byte by >> 10, which is what
 * fbcon's (byte<<8)|byte palette does. Expanding a stored 6-bit value
 * with << 10 round-trips through that shift.
 *
 * This is the model contract. It is not a measurement of the 755C.
 */
#ifndef WD24ADV_DAC_ENTRY_H
#define WD24ADV_DAC_ENTRY_H

#ifndef WD24ADV_CORE_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif
#endif

#define WD24_DAC_PEL	0x3c6
#define WD24_DAC_RI	0x3c7
#define WD24_DAC_WI	0x3c8
#define WD24_DAC_D	0x3c9

struct wd24_dac_entry {
	u8 index;
	u8 pel;		/* 3C6, same FFh the full restore writes */
	u8 rgb[3];	/* 3C9, already masked to 6 bits */
};

struct wd24_dac_io {
	void (*outb)(void *p, u8 v, u16 port);
	u8 (*inb)(void *p, u16 port);
	void *p;
};

/* Opt-in is refused unless every live condition holds. cmap==0 is not a refuse. */
static inline int wd24_cmap_param_refuse(int cmap, int s6d, int dryrun,
					 int live, int bind, int fbcon,
					 int mode_640, int other)
{
	if (!cmap)
		return 0;
	if (!s6d || dryrun || !live || bind || !fbcon || !mode_640 || other)
		return -22;
	return 0;
}

/* What the hook may do. Filled by the caller under hw_lock. */
struct wd24_cmap_view {
	unsigned param, dryrun, live, bind, fbcon;
	unsigned armed, active, stopping, busy, registering;
	unsigned in_text, hw_dead, have_cmap, bad, published, frozen;
};

static inline int wd24_cmap_allow(const struct wd24_cmap_view *v)
{
	if (!v || !v->param || v->dryrun || !v->live || v->bind || !v->fbcon)
		return -1;
	if (!v->armed || !v->active || v->stopping || !v->busy ||
	    v->registering || v->frozen)
		return -16;
	if (v->in_text || v->hw_dead || !v->have_cmap || v->bad || !v->published)
		return -19;
	return 0;
}

/* Publish reads only when this is 0. A latched mismatch is `bad`. */
static inline int wd24_cmap_before_read(unsigned frozen, unsigned dead,
					unsigned have, unsigned in_text,
					unsigned bad)
{
	if (frozen || dead || !have || in_text || bad)
		return -19;
	return 0;
}

/* What the hook does with info->cmap after a PUT or a publish read.
 * rewrite: store the load snapshot (<<10). The kernel must not also
 * copy the rejected request.
 * invalidate: set len to 0 and keep the arrays. FBIOGETCMAP then
 * returns -EINVAL. Do not kfree them here: a GET may already have
 * copied the pointers and be in copy_to_user.
 * latch: this decision itself sets failed and nmismatch. A restore
 * that already called mismatch() must not ask for it again.
 */
struct wd24_cmap_fault {
	int err;
	unsigned published;
	unsigned rewrite;
	unsigned invalidate;
	unsigned latch;
};

static inline struct wd24_cmap_fault wd24_cmap_settle(int put_err,
						      int restore_err,
						      unsigned had_arrays)
{
	struct wd24_cmap_fault f;

	f.err = put_err;
	f.published = 0;
	f.rewrite = 0;
	f.invalidate = 0;
	f.latch = 0;
	if (!put_err) {
		f.published = 1;
		return f;
	}
	if (!restore_err && had_arrays) {
		f.rewrite = 1;
		f.published = 1;
		return f;
	}
	f.invalidate = 1;
	return f;
}

static inline struct wd24_cmap_fault wd24_cmap_read_mismatch(void)
{
	struct wd24_cmap_fault f;

	f.err = -5;
	f.published = 0;
	f.rewrite = 0;
	f.invalidate = 1;
	f.latch = 1;
	return f;
}

static inline void wd24_cmap_latch(unsigned *failed, unsigned *nmismatch)
{
	*failed = 1;
	if (nmismatch)
		*nmismatch += 1;
}

static inline int wd24_dac_entry(u32 regno, u16 r, u16 g, u16 b,
				 struct wd24_dac_entry *e)
{
	if (!e || regno > 255)
		return -22;
	e->index = (u8)regno;
	e->pel = 0xff;
	e->rgb[0] = (u8)((r >> 10) & 0x3f);
	e->rgb[1] = (u8)((g >> 10) & 0x3f);
	e->rgb[2] = (u8)((b >> 10) & 0x3f);
	return 0;
}

static inline void wd24_dac_entry_store(u8 image[768],
					const struct wd24_dac_entry *e)
{
	unsigned int n = (unsigned int)e->index * 3;

	image[n] = e->rgb[0];
	image[n + 1] = e->rgb[1];
	image[n + 2] = e->rgb[2];
}

static inline unsigned int wd24_dac_diff6(const u8 *a, const u8 *b)
{
	unsigned int i, n = 0;

	for (i = 0; i < 768; i++)
		n += ((a[i] ^ b[i]) & 0x3f) != 0;
	return n;
}

static inline void wd24_dac_expand_image(const u8 image[768],
					 u16 *r, u16 *g, u16 *b)
{
	unsigned int i;

	for (i = 0; i < 256; i++) {
		r[i] = (u16)(image[i * 3] & 0x3f) << 10;
		g[i] = (u16)(image[i * 3 + 1] & 0x3f) << 10;
		b[i] = (u16)(image[i * 3 + 2] & 0x3f) << 10;
	}
}

static inline void wd24_dac_mask_ff(const struct wd24_dac_io *io)
{
	io->outb(io->p, 0xff, WD24_DAC_PEL);
}

/* One entry. No 3C6 access. Returns -EIO when the three reads differ. */
static inline int wd24_dac_put_entry(const struct wd24_dac_io *io,
				     u8 index, u8 r, u8 g, u8 b)
{
	u8 rgb[3], back[3], i;

	if (!io || !io->outb || !io->inb)
		return -22;
	rgb[0] = r & 0x3f;
	rgb[1] = g & 0x3f;
	rgb[2] = b & 0x3f;
	io->outb(io->p, index, WD24_DAC_WI);
	io->outb(io->p, rgb[0], WD24_DAC_D);
	io->outb(io->p, rgb[1], WD24_DAC_D);
	io->outb(io->p, rgb[2], WD24_DAC_D);
	io->outb(io->p, index, WD24_DAC_RI);
	for (i = 0; i < 3; i++)
		back[i] = io->inb(io->p, WD24_DAC_D) & 0x3f;
	for (i = 0; i < 3; i++)
		if (back[i] != rgb[i])
			return -5;
	return 0;
}

struct wd24_dac_span {
	u8 index;
	u8 r, g, b;
};

/* Validate, then one 3C6=FFh, then each entry. n==0 writes nothing. */
static inline int wd24_dac_put_span(const struct wd24_dac_io *io,
				    const struct wd24_dac_span *e, unsigned int n)
{
	unsigned int i;
	int err;

	if (!io || !io->outb || !io->inb || (!e && n) || !n)
		return -22;
	wd24_dac_mask_ff(io);
	for (i = 0; i < n; i++) {
		err = wd24_dac_put_entry(io, e[i].index, e[i].r, e[i].g, e[i].b);
		if (err)
			return err;
	}
	return 0;
}

#endif
