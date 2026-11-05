// SPDX-License-Identifier: GPL-2.0
/* wd24adv S6b dual-window mmap session + PR31 b7 copy; see wd24adv_s6b.h */
#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/string.h>
#else
#include <stdio.h>
#include <string.h>
#endif
#include "wd24adv_s6b.h"

#define LOG(b, ...) do { if ((b)->loglen < WD24S6B_LOG) (b)->loglen += \
	snprintf((b)->log + (b)->loglen, WD24S6B_LOG - (b)->loglen, __VA_ARGS__); } while (0)

#define E_INVAL	(-22)
#define E_IO	(-5)
#define E_NODEV	(-19)

static const char *const stn[] = { "OFF", "ON", "DEAD" };

const char *wd24s6b_state_name(enum wd24s6b_state s)
{
	return s <= WD24S6B_DEAD ? stn[s] : "?";
}

static int wr(struct wd24s6b *b, enum wd24_rf rf, u8 idx, u8 v)
{
	b->nwr++;
	return b->io->wr(b->io->priv, rf, idx, v);
}

static int wrv(struct wd24s6b *b, enum wd24_rf rf, u8 idx, u8 v, u8 mask)
{
	int err = wr(b, rf, idx, v);

	if (!err && ((b->io->rd(b->io->priv, rf, idx) ^ v) & mask))
		err = E_IO;
	return err;
}

/* SR06 lock step, as S6: unlock to (BIOS & EFh) | 48h, relock to BIOS;
 * verified by bits 3:0 and the 3C4 index width */
static int lockstep(struct wd24s6b *b, bool on)
{
	const struct wd24s6_io *io = b->io;
	u8 v = on ? WD24_PR20_UNLOCK(b->sr06) : b->sr06;
	int err, lk;
	u8 rb;

	if (on)
		io->lock(io->priv, true);
	err = wr(b, WD24_SR, WD24_SR_PR20, v);
	rb = io->rd(io->priv, WD24_SR, WD24_SR_PR20);
	lk = io->unlocked(io->priv);
	if (!on)
		io->lock(io->priv, false);
	b->unl = on || lk != 0;		/* relock owed unless verified locked */
	if (err || (rb & 0x0f) != (v & 0x0f) || lk != (on ? 1 : 0)) {
		LOG(b, "  SR06 %s FAILED: rb %02x width %d err %d\n", on ? "unlock" : "relock",
		    rb, lk, err);
		return err ? err : E_IO;
	}
	return 0;
}

static int setbanks(struct wd24s6b *b, u8 a, u8 bb)
{
	int err = 0;

	if (a != b->cur_a) {
		err = wrv(b, WD24_GR, WD24_GR_PR0A, a, 0x7f);
		b->cur_a = err ? 0xff : a;
	}
	if (!err && bb != b->cur_b) {
		err = wrv(b, WD24_GR, WD24_GR_PR0B, bb, 0x7f);
		b->cur_b = err ? 0xff : bb;
	}
	return err;
}

/* best effort: PR31 b7 off (unlock/relock), PR1, PR0A, PR0B to entry */
static void restore(struct wd24s6b *b)
{
	const struct wd24s6_io *io = b->io;
	u8 v;

	b->rest_bad = 0;
	if (b->b7) {
		if (lockstep(b, true) == 0) {
			if (wrv(b, WD24_SR, WD24_SR_PR31, b->pr31, 0xff))
				b->rest_bad++;
			else
				b->b7 = false;
		} else {
			b->rest_bad++;
		}
		if (lockstep(b, false))
			b->rest_bad++;
	} else if (b->unl && lockstep(b, false)) {
		b->rest_bad++;
	}
	if (b->pr1set || (io->rd(io->priv, WD24_GR, WD24_GR_PR1) != b->pr1)) {
		if (wrv(b, WD24_GR, WD24_GR_PR1, b->pr1, 0xff))
			b->rest_bad++;
		else
			b->pr1set = false;
	}
	if (wrv(b, WD24_GR, WD24_GR_PR0A, b->pr0a, 0x7f))
		b->rest_bad++;
	if (wrv(b, WD24_GR, WD24_GR_PR0B, b->pr0b, 0x7f))
		b->rest_bad++;
	b->cur_a = b->pr0a;
	b->cur_b = b->pr0b;
	v = io->rd(io->priv, WD24_SR, WD24_SR_PR20);
	if ((v & 0x0f) != (b->sr06 & 0x0f) || io->unlocked(io->priv) != 0)
		b->rest_bad++;
	LOG(b, "restore: PR1 %02x PR0A %02x PR0B %02x SR06 %02x %s, PR31 b7 %s, %u bad\n",
	    io->rd(io->priv, WD24_GR, WD24_GR_PR1), io->rd(io->priv, WD24_GR, WD24_GR_PR0A),
	    io->rd(io->priv, WD24_GR, WD24_GR_PR0B), v,
	    io->unlocked(io->priv) == 0 ? "locked" : "NOT LOCKED",
	    b->b7 ? "MAY BE SET" : "clear", b->rest_bad);
}

static int die(struct wd24s6b *b, const char *why, int err)
{
	b->why = why;
	LOG(b, "DEAD: %s (%d)\n", why, err);
	restore(b);
	b->state = WD24S6B_DEAD;
	return err ? err : E_IO;
}

int wd24s6b_begin(struct wd24s6b *b, const struct wd24s6_io *io)
{
	int lk, err;

	memset(b, 0, sizeof(*b));
	b->io = io;
	b->state = WD24S6B_OFF;
	b->pr0a = io->rd(io->priv, WD24_GR, WD24_GR_PR0A);
	b->pr0b = io->rd(io->priv, WD24_GR, WD24_GR_PR0B);
	b->pr1 = io->rd(io->priv, WD24_GR, WD24_GR_PR1);
	b->gr6 = io->rd(io->priv, WD24_GR, 0x06);
	b->sr06 = io->rd(io->priv, WD24_SR, WD24_SR_PR20);
	lk = io->unlocked(io->priv);
	b->cur_a = b->pr0a;
	b->cur_b = b->pr0b;
	LOG(b, "S6b begin: PR0A %02x PR0B %02x PR1 %02x GR06 %02x SR06 %02x lock %s\n",
	    b->pr0a, b->pr0b, b->pr1, b->gr6, b->sr06,
	    lk > 0 ? "OPEN" : lk == 0 ? "locked" : "no answer");
	if (b->pr1 & WD24_PR1_PR0B)
		b->why = "PR1 b3 already set";
	else if ((b->pr1 & 0xc0) != 0xc0)
		b->why = "PR1 b7:6 not 1 MiB";
	else if ((b->gr6 & 0x0c) != 0x04)
		b->why = "GR06 map is not 64 KiB at A0000";
	else if (b->pr0a != 0)
		b->why = "PR0A not 0 after the mode set";
	else if (lk != 0)
		b->why = "SR06 lock not in the expected locked state";
	if (b->why) {
		LOG(b, "refused: %s (no write)\n", b->why);
		return E_NODEV;
	}
	io->open(io->priv, true);
	b->w0 = 0;
	b->w1 = WD24S6B_HPG;
	if ((err = setbanks(b, 0, 0)))
		return die(b, "PR0A/PR0B readback", err);
	b->pr1set = true;
	if ((err = wrv(b, WD24_GR, WD24_GR_PR1, b->pr1 | WD24_PR1_PR0B, 0xff)))
		return die(b, "PR1 b3 readback", err);
	b->state = WD24S6B_ON;
	LOG(b, "ON: PR1 %02x, A0000 -> %05x, A8000 -> %05x, SR06 locked\n",
	    b->pr1 | WD24_PR1_PR0B, 0, WD24S6B_HPG << 12);
	return 0;
}

static bool win_ok(u32 w0, u32 w1)
{
	if (w0 > WD24S6B_MAXW || w1 > WD24S6B_MAXW || w1 < WD24S6B_HPG)
		return false;
	/* disjoint halves: the same VRAM never has two user addresses */
	return w0 + WD24S6B_HPG <= w1 || w1 + WD24S6B_HPG <= w0;
}

int wd24s6b_setwin(struct wd24s6b *b, u32 w0, u32 w1)
{
	int err;

	if (b->state != WD24S6B_ON)
		return E_NODEV;
	if (!win_ok(w0, w1))
		return E_INVAL;
	if ((err = setbanks(b, (u8)(w1 - WD24S6B_HPG), (u8)w0)))
		return die(b, "SETWIN PR0A/PR0B readback", err);
	b->w0 = (u8)w0;
	b->w1 = (u8)w1;
	b->nset++;
	return 0;
}

int wd24s6b_copy(struct wd24s6b *b, u32 dst, u32 src, u32 len)
{
	const struct wd24s6_io *io = b->io;
	bool back = dst > src;
	u32 done = 0;
	int err;
	u8 v;

	if (b->state != WD24S6B_ON)
		return E_NODEV;
	if (!len || len > WD24S6B_VRAM || src > WD24S6B_VRAM - len || dst > WD24S6B_VRAM - len)
		return E_INVAL;
	if (dst == src)
		return 0;
	/* 1: PR1 b3 off: b7 runs only in the S6c-proven PR1 state */
	if ((err = wrv(b, WD24_GR, WD24_GR_PR1, b->pr1, 0xff)))
		return die(b, "COPY PR1 b3 off readback", err);
	b->pr1set = false;
	/* 2-4: unlock, PR31 b7 on, relock */
	if ((err = lockstep(b, true)))
		return die(b, "COPY SR06 unlock", err);
	v = io->rd(io->priv, WD24_SR, WD24_SR_PR31);
	if (!b->pr31_known) {
		if (v & WD24_PR31_RWSPLIT)
			return die(b, "PR31 b7 set before the first COPY", 0);
		b->pr31 = v;
		b->pr31_known = true;
		LOG(b, "PR31 base (unlocked) %02x\n", v);
	} else if (v != b->pr31) {
		return die(b, "PR31 changed between COPYs", 0);
	}
	b->b7 = true;
	if ((err = wrv(b, WD24_SR, WD24_SR_PR31, b->pr31 | WD24_PR31_RWSPLIT, 0xff)))
		return die(b, "COPY PR31 b7 readback", err);
	if ((err = lockstep(b, false)))
		return die(b, "COPY SR06 relock with b7", err);
	/* 5: copy, SR06 LOCKED; reads PR0A (src bank), writes PR0B (dst) */
	while (done < len) {
		u32 n = len - done < WD24S6B_CHUNK ? len - done : WD24S6B_CHUNK;
		u32 s = back ? src + len - done - n : src + done;
		u32 d = back ? dst + len - done - n : dst + done;

		if ((err = setbanks(b, (u8)(s >> 12), (u8)(d >> 12))))
			return die(b, "COPY bank readback", err);
		io->mr(io->priv, s & 0xfff, b->buf, n);
		io->mw(io->priv, d & 0xfff, b->buf, n);
		done += n;
	}
	/* 6-8: unlock, PR31 base, relock */
	if ((err = lockstep(b, true)))
		return die(b, "COPY SR06 re-unlock", err);
	v = io->rd(io->priv, WD24_SR, WD24_SR_PR31);
	if (v != (b->pr31 | WD24_PR31_RWSPLIT))
		return die(b, "PR31 b7 not held through the copy (data unverified)", 0);
	if ((err = wrv(b, WD24_SR, WD24_SR_PR31, b->pr31, 0xff)))
		return die(b, "COPY PR31 restore readback", err);
	b->b7 = false;
	if ((err = lockstep(b, false)))
		return die(b, "COPY SR06 relock", err);
	/* 9: back to the split windows */
	b->pr1set = true;
	if ((err = wrv(b, WD24_GR, WD24_GR_PR1, b->pr1 | WD24_PR1_PR0B, 0xff)))
		return die(b, "COPY PR1 b3 on readback", err);
	if ((err = setbanks(b, (u8)(b->w1 - WD24S6B_HPG), b->w0)))
		return die(b, "COPY window restore readback", err);
	if (b->ncopy++ < 8)
		LOG(b, "COPY %05x -> %05x len %x %s ok\n", src, dst, len, back ? "backward" : "forward");
	return 0;
}

int wd24s6b_end(struct wd24s6b *b)
{
	if (b->state == WD24S6B_OFF)
		return 0;
	LOG(b, "end: %u COPY, %u SETWIN, %u writes\n", b->ncopy, b->nset, b->nwr);
	restore(b);
	b->io->open(b->io->priv, false);
	if (b->state == WD24S6B_ON)
		b->state = b->rest_bad ? WD24S6B_DEAD : WD24S6B_OFF;
	if (b->rest_bad && !b->why)
		b->why = "RESTORE FAIL";
	return b->rest_bad ? E_IO : 0;
}
