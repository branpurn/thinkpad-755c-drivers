// SPDX-License-Identifier: GPL-2.0
/*
 * wd24adv port backend: executes the plans of wd24adv_core.c on real
 * VGA ports.  The I/O primitives come from c->io (kernel: inb/outb and an
 * ioremap of A0000; host tests: the register/memory model in
 * tests/vgamodel.c), so the same code runs in both.
 *
 * The kernel module uses this backend only with dryrun=0 live=1 (see
 * wd24adv_main.c); every plan then starts with the read-only entry check
 * and reads back every write (wd24_verify).
 *
 * Port access follows Linux include/video/vga.h and drivers/video/
 * vgastate.c: indexed SEQ/GFX/CRTC via index then data port; attribute
 * controller via a 3DA read (flip-flop to index), index, data; after an
 * attribute access the palette address source (bit 5) is set again so
 * the screen stays on (vga_io_r(VGA_IS1_RC); vga_io_w(VGA_ATT_IW, 0x20)).
 */
#include "wd24adv_core.h"

#define P_AR	0x3c0
#define P_ARR	0x3c1
#define P_MISCW	0x3c2
#define P_SRI	0x3c4
#define P_SRD	0x3c5
#define P_PEL	0x3c6
#define P_DACR	0x3c7
#define P_DACW	0x3c8
#define P_DACD	0x3c9
#define P_MISCR	0x3cc
#define P_GRI	0x3ce
#define P_GRD	0x3cf
#define P_CRI	0x3d4
#define P_CRD	0x3d5
#define P_IS1	0x3da

static u8 in(struct wd24_ctx *c, u16 port)
{
	return c->io->inb(c->io->priv, port);
}

static void out_idx(struct wd24_ctx *c, u8 v, u16 port)
{
	c->io->outb(c->io->priv, v, port);
	c->hw_index++;
}

static void out_data(struct wd24_ctx *c, u8 v, u16 port)
{
	c->io->outb(c->io->priv, v, port);
	c->hw_data++;
}

static u16 idx_port(enum wd24_rf rf)
{
	return rf == WD24_SR ? P_SRI : rf == WD24_GR ? P_GRI : P_CRI;
}

static int port_write(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 val)
{
	switch (rf) {
	case WD24_MISC:
		out_data(c, val, P_MISCW);
		return 0;
	case WD24_AR:
		(void)in(c, P_IS1);
		if (idx == 0x20) {		/* palette address source on */
			out_idx(c, 0x20, P_AR);
			return 0;
		}
		out_idx(c, idx & 0x1f, P_AR);
		out_data(c, val, P_AR);
		(void)in(c, P_IS1);
		out_idx(c, 0x20, P_AR);
		return 0;
	case WD24_SR:
	case WD24_GR:
	case WD24_CR:
		out_idx(c, idx, idx_port(rf));
		out_data(c, val, idx_port(rf) + 1);
		return 0;
	default:
		return -EINVAL;
	}
}

static u8 port_read(struct wd24_ctx *c, enum wd24_rf rf, u8 idx)
{
	u8 v;

	switch (rf) {
	case WD24_MISC:
		return in(c, P_MISCR);
	case WD24_AR:
		if (idx == 0x20)
			return 0;
		(void)in(c, P_IS1);
		out_idx(c, idx & 0x1f, P_AR);
		v = in(c, P_ARR);
		(void)in(c, P_IS1);
		out_idx(c, 0x20, P_AR);
		return v;
	case WD24_SR:
	case WD24_GR:
	case WD24_CR:
		out_idx(c, idx, idx_port(rf));
		return in(c, idx_port(rf) + 1);
	default:
		return 0xff;
	}
}

/* wd24probe3: with the SR06 latch locked only index bits 2:0 stick
 * (write 11h, read 01h); unlocked they read back 11h */
static int port_seq_unlocked(struct wd24_ctx *c)
{
	u8 save = in(c, P_SRI), r;

	out_idx(c, 0x11, P_SRI);
	r = in(c, P_SRI);
	out_idx(c, save, P_SRI);
	if (r == 0x11)
		return 1;
	if (r == 0x01)
		return 0;
	return -1;
}

/* vgastate save_vga_cmap / restore_vga_cmap */
static void port_dac_read(struct wd24_ctx *c, u8 pel, u8 *rgb)
{
	unsigned int i;

	out_data(c, pel, P_PEL);
	out_idx(c, 0x00, P_DACR);
	for (i = 0; i < 768; i++)
		rgb[i] = in(c, P_DACD);
}

static void port_dac_write(struct wd24_ctx *c, u8 pel, const u8 *rgb)
{
	unsigned int i;

	out_data(c, pel, P_PEL);
	out_idx(c, 0x00, P_DACW);
	for (i = 0; i < 768; i++)
		out_data(c, rgb[i], P_DACD);
}

static void port_mem_read(struct wd24_ctx *c, u32 off, u8 *buf, u32 n)
{
	u32 i;

	for (i = 0; i < n && off + i < WD24_PLANE; i++)
		buf[i] = c->io->memr(c->io->priv, off + i);
}

static void port_mem_write(struct wd24_ctx *c, u32 off, const u8 *buf, u32 n)
{
	u32 i;

	for (i = 0; i < n && off + i < WD24_PLANE; i++)
		c->io->memw(c->io->priv, off + i, buf[i]);
}

static void port_begin(struct wd24_ctx *c)
{
	c->saved_idx[0] = in(c, P_SRI);
	c->saved_idx[1] = in(c, P_GRI);
	c->saved_idx[2] = in(c, P_CRI);
}

static void port_end(struct wd24_ctx *c)
{
	out_idx(c, c->saved_idx[0], P_SRI);
	out_idx(c, c->saved_idx[1], P_GRI);
	out_idx(c, c->saved_idx[2], P_CRI);
}

const struct wd24_backend wd24_port_backend = {
	.name = "port (live)",
	.live = true,
	.write = port_write,
	.read = port_read,
	.seq_unlocked = port_seq_unlocked,
	.dac_read = port_dac_read,
	.dac_write = port_dac_write,
	.mem_read = port_mem_read,
	.mem_write = port_mem_write,
	.begin = port_begin,
	.end = port_end,
};
