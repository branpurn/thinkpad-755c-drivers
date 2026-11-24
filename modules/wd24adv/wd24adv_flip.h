/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Two 320x200 pages do not fit in one 64K chain-4 window
 * (64000 * 2 > 65536). A third page is the same size again.
 * The scanout start that moves the picture is CR0C/CR0D. Mode 13h
 * sets CR14 bit 6, so that register counts units of 4 bytes:
 *   page 0  byte 0       CR0C=0x00 CR0D=0x00
 *   page 1  byte 65536   CR0C=0x40 CR0D=0x00
 *   page 2  byte 131072  CR0C=0x80 CR0D=0x00
 * vgacon's writes to those two registers moved this panel, and
 * clearing them fixed the wrap. KD_GRAPHICS stays on so vgacon does
 * not write them back.
 *
 * PR3 bits 4:3 are CA17:CA16 (datasheet 13.4; VGADOC paradise.txt
 * calls them display-start bits 16-17, with bits 0-15 in CR0C/CR0D).
 * One of those bits is a +65536 character step. In doubleword mode
 * that is +256KiB, not the next 64K. Linux vga16fb_pan_var writes
 * the 8bpp byte offset >> 2 to those two CRTC registers.
 * PR16 bits 4:3 are added to those same two bits (datasheet 13.13;
 * VGADOC: address-counter offset, not the display start).
 * PR16 bit 6 stays 1 in the BIOS value (CR2F=0x42), which limits the
 * counter to 64K locations. 128KiB is inside that. Toggling PR16 bit
 * 3 on 2fe57de left CR0C/CR0D at 0 and the picture did not move.
 * This step does not write PR16 or PR3.
 *
 * PR0A (GR09) bits 0-7 are added to CPU address bits 12-19 (VGADOC
 * paradise.txt, 3CEh index 9). wd_selftest_banks uses 0x00, 0x08,
 * 0x10 and 0x40, which is that same shift: value << 12.
 *   0x00 -> byte 0
 *   0x10 -> byte 65536
 *   0x20 -> byte 131072
 * Chain-4 applies that sum before the plane split, so the CPU byte
 * and the CRTC byte (start << 2) are the same VRAM. The pages run on
 * 2138ee1 showed it: green written at PR0A 0x10 came up when CR0C
 * was 0x40. The banks are not what ghosts the sprites.
 *
 * What does: the CRTC uses CR0C/CR0D for screen refresh (datasheet
 * 10.12) and loads that start at vertical retrace. 3DA bit 3 is that
 * retrace (datasheet 15.8). go5 wrote the start and pointed PR0A at
 * the other page in the same call, then Doom drew at once. With two
 * pages that other page is the one the beam is still finishing, for
 * up to one 70 Hz field. This step keeps three pages, so the page
 * drawn next is neither the one just published nor the one before
 * it. The flip waits for bit 3 only when that next page is still
 * the one on the beam. A free page does not stall the frame. A
 * start write that does not wait can meet the beam mid-line.
 */
#ifndef WD24ADV_FLIP_H
#define WD24ADV_FLIP_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t u8;
#endif

#define WD24_FLIP_PAGES		3u
#define WD24_FLIP_PR0A_PAGE1	0x10u
#define WD24_FLIP_CR0C_PAGE1	0x40u
#define WD24_FLIP_CR0D_PAGE1	0x00u
#define WD24_FLIP_WRITES_HOT	3u	/* CR0C, CR0D, PR0A */
#define WD24_FLIP_WRITES_COLD	7u	/* those, plus the four unlock writes */

struct wd24_flip_step {
	u8 cr0c;
	u8 cr0d;
	u8 pr0a;
	unsigned int show;
	unsigned int cpu;
};

static inline unsigned int wd24_flip_writes(int already_unlocked)
{
	return already_unlocked ? WD24_FLIP_WRITES_HOT : WD24_FLIP_WRITES_COLD;
}

/* PR0A bits 0-7 added to address bits 12-19. */
static inline unsigned int wd24_flip_cpu_byte(u8 pr0a)
{
	return (unsigned int)(pr0a & 0x7fu) << 12;
}

/* CR14 bit 6: the start counts units of 4 bytes. */
static inline unsigned int wd24_flip_crtc_byte(u8 cr0c, u8 cr0d)
{
	return ((unsigned int)cr0c << 8 | cr0d) << 2;
}

/* cpu_page is the page just drawn. The next draw is the page after
 * the one published, not the one published and not the one before it.
 */
static inline void wd24_flip_step(unsigned int cpu_page, struct wd24_flip_step *o)
{
	unsigned int show = cpu_page % WD24_FLIP_PAGES;
	unsigned int cpu = (show + 1u) % WD24_FLIP_PAGES;

	o->show = show;
	o->cpu = cpu;
	o->cr0c = (u8)(show * WD24_FLIP_CR0C_PAGE1);
	o->cr0d = (u8)(show ? WD24_FLIP_CR0D_PAGE1 : 0);
	o->pr0a = (u8)(cpu * WD24_FLIP_PR0A_PAGE1);
}

/* One field at 70 Hz is about 14 ms. 20 ms is the host-side stand-in
 * for "a retrace has happened since the start was written".
 */
#define WD24_FLIP_FIELD_MS	20u

struct wd24_flip_beam {
	unsigned int scanning;	/* page the beam is still on */
	unsigned int pending;	/* last start written, not yet known latched */
	int fresh;		/* pending is newer than scanning */
};

static inline void wd24_flip_beam_init(struct wd24_flip_beam *b)
{
	b->scanning = 0;
	b->pending = 0;
	b->fresh = 0;
}

static inline int wd24_flip_beam_advanced(unsigned int elapsed_ms)
{
	return elapsed_ms >= WD24_FLIP_FIELD_MS;
}

/* A field since the write moves the beam onto the written page. */
static inline void wd24_flip_beam_note(struct wd24_flip_beam *b,
				       unsigned int elapsed_ms)
{
	if (b->fresh && wd24_flip_beam_advanced(elapsed_ms)) {
		b->scanning = b->pending;
		b->fresh = 0;
	}
}

/* 1 when the CPU page is the one just published or the one the beam
 * may still be scanning. A third page is neither, so the answer is 0
 * and the flip does not block.
 */
static inline int wd24_flip_needs_wait(const struct wd24_flip_beam *b,
				       unsigned int show, unsigned int cpu)
{
	if (!b || show >= WD24_FLIP_PAGES || cpu >= WD24_FLIP_PAGES)
		return 1;
	if (cpu == show)
		return 1;
	if (cpu == b->scanning)
		return 1;
	if (b->fresh && cpu == b->pending)
		return 1;
	return 0;
}

static inline void wd24_flip_beam_arm(struct wd24_flip_beam *b,
				      unsigned int show, int waited)
{
	b->pending = show;
	if (waited) {
		b->scanning = show;
		b->fresh = 0;
	} else {
		b->fresh = 1;
	}
}

/* latched is 1 after a field (or an in-retrace write) has moved the
 * beam onto the page this call publishes. old is the page that was
 * scanning before that. go5 aimed the CPU at old before the sample.
 */
static inline int wd24_flip_draw_ok(unsigned int show, unsigned int cpu,
				    unsigned int old, int latched)
{
	struct wd24_flip_beam b;

	wd24_flip_beam_init(&b);
	b.scanning = latched ? show : old;
	b.pending = show;
	b.fresh = latched ? 0 : 1;
	return !wd24_flip_needs_wait(&b, show, cpu);
}

#endif
