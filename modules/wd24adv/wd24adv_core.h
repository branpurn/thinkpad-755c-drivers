/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wd24adv - experimental "advanced" driver for the WD90C24/WD90C24A(2) on
 * the ThinkPad 755C.  This header is shared by the kernel module and the
 * host-side unit test (tests/), so it must not depend on kernel headers
 * beyond the small compat block below.
 *
 * wd24adv is a SEPARATE module from wd90c24fb: it shares no source, header,
 * ioctl or build file with it, and only one of the two may be loaded.
 *
 * Register knowledge comes from the public WD90C24 datasheet, the XFree86
 * WD driver and the clean-room Stage 0 BIOS trace (stage0/), never from
 * disassembled vendor code.
 */
#ifndef WD24ADV_CORE_H
#define WD24ADV_CORE_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

/* ---- register files ---------------------------------------------------- */
enum wd24_rf {
	WD24_SR,	/* 3C4/3C5 sequencer (PR20-PR23 at 06-09, PR30A.. at 10..) */
	WD24_GR,	/* 3CE/3CF graphics (PR0A-PR5 at 09-0F) */
	WD24_CR,	/* 3D4/3D5 CRTC (PR10-PR1B at 29-34, PR36 at 3B) */
	WD24_AR,	/* 3C0 attribute */
	WD24_MISC,	/* 3C2 write / 3CC read */
	WD24_NRF
};

#define WD24_NIDX	0x40	/* indices tracked per file (shadow copy) */

/* CRTC / sequencer / GR indices named after the WD "PR" numbers */
#define WD24_GR_PR0A	0x09
#define WD24_GR_PR1	0x0b	/* memory size / map: aperture bits b5:4 */
#define WD24_GR_PR4	0x0e	/* b5: tri-state panel interface */
#define WD24_GR_PR5	0x0f	/* write 05h to unlock PR0-PR4 */
#define WD24_CR_PR10	0x29	/* write 85h to unlock PR11-PR17 */
#define WD24_CR_PR18	0x31	/* flat panel status (routing) */
#define WD24_CR_PR19	0x32	/* flat panel control: b5:4 route, b3:2 scale */
#define WD24_CR_PR1A	0x33	/* flat panel control II */
#define WD24_CR_PR1B	0x34	/* unlock: A0h = FP regs, xxxxx110b = shadows */
#define WD24_CR_PR36	0x3b	/* panel interface / timing */
#define WD24_SR_PR20	0x06	/* unlock: (old & EFh) | 48h */
#define WD24_SR_PR57	0x19	/* b2: PNLOFF (panel power) */
#define WD24_SR_PR64	0x27	/* stretch control (values from Stage 0) */
#define WD24_SR_PR68	0x31	/* clock (VCLK/MCLK) - needs SR35=50h */
#define WD24_SR_PR69	0x32
#define WD24_SR_PR68UL	0x35

#define WD24_PR19_ROUTE		0x30	/* b5:4: 00 off, 01 panel, 10 CRT, 11 both */
#define WD24_PR19_PANEL		0x10
#define WD24_PR19_CRT		0x20
#define WD24_PR19_SCALE		0x0c	/* b3:2: centre/expand (Stage 0 to confirm) */
#define WD24_PR57_PNLON		0x04
#define WD24_PR4_TRISTATE	0x20
#define WD24_PR1B_FP_UNLOCK	0xa0
#define WD24_PR1B_SHADOW	0xa6	/* xxxxx110b: write the panel shadow CRTC */

/* ---- output routing ---------------------------------------------------- */
enum wd24_route {
	WD24_ROUTE_PANEL = 1,
	WD24_ROUTE_CRT = 2,
	WD24_ROUTE_BOTH = 3,
};

/* ---- event log (what was, or in dry-run would have been, done) --------- */
enum wd24_ev {
	WD24_EV_WRITE,		/* rf, idx, old, val */
	WD24_EV_DELAY,		/* ms in val32 */
	WD24_EV_REFUSED,	/* guard refused rf/idx/val; err */
	WD24_EV_NOTE,		/* static text */
};

struct wd24_event {
	u8 type, rf, idx, old, val;
	int err;
	u32 ms;
	const char *note;
};

#define WD24_MAXEV	256

/* ---- context ----------------------------------------------------------- */
struct wd24_ctx;

/* Backend: dry-run (record only) or, from Stage 2 on, real ports. */
struct wd24_backend {
	const char *name;
	int (*write)(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 val);
	void (*delay)(struct wd24_ctx *c, unsigned int ms);
};

enum wd24_panel_state { WD24_PANEL_UNKNOWN, WD24_PANEL_ON, WD24_PANEL_OFF };

struct wd24_ctx {
	const struct wd24_backend *be;
	/* permissions from module parameters; all default to false */
	bool allow_routing;	/* PR18/PR19 b7:4/PR1A/PR36/shadow CRTC */
	bool allow_crt;		/* CRT-only modes (panel powered down) */
	bool allow_clock;	/* PR68/PR69 (Stage 6/7) */
	/* state */
	enum wd24_panel_state panel;
	bool in_panel_seq;	/* set only by wd24_panel_on/off */
	bool shadow_open;	/* CR34 currently selects the shadow CRTC */
	u8 reg[WD24_NRF][WD24_NIDX];	/* last known value of each register */
	/* log */
	struct wd24_event ev[WD24_MAXEV];
	unsigned int nev;
	unsigned int dropped;
};

/* ---- mode table -------------------------------------------------------- */
#define WD24M_PANEL_OK		0x0001	/* may be shown on the 640x480 TFT */
#define WD24M_CRT_ONLY		0x0002	/* panel must be powered down */
#define WD24M_PLANAR		0x0004	/* unchained 4-plane (Mode X/Y) */
#define WD24M_DIRECT		0x0008	/* fits the 64 KiB window: direct mmap */
#define WD24M_BANKED		0x0010	/* needs PR0A banking / RAM shadow */
#define WD24M_DOUBLESCAN	0x0020
#define WD24M_TEXT		0x0040
#define WD24M_STAGE0		0x8000	/* register values pending the Stage 0 trace */

struct wd24_reg {
	u8 rf, idx, val;
};

struct wd24_mode {
	const char *name;
	u16 xres, yres;		/* visible */
	u16 vxres, vyres;	/* virtual (panning / flipping) */
	u8 bpp;
	u8 bios_mode;		/* INT 10h mode it corresponds to, 0 = none */
	u8 stage;		/* staged-plan stage that enables it */
	u8 pr19_scale;		/* PR19 b3:2 value for the panel (masked) */
	u32 pixclock_khz;	/* dot clock; 0 = keep the BIOS value */
	u32 frame_bytes;
	u32 flags;
	const struct wd24_reg *regs;	/* std then ext, in write order */
	unsigned int nregs;
};

extern const struct wd24_mode wd24_modes[];
extern const unsigned int wd24_nmodes;

const struct wd24_mode *wd24_mode_find(const char *name);
/* returns 0, or -EINVAL with *why set to a static explanation */
int wd24_mode_check(const struct wd24_mode *m, const char **why);
int wd24_modes_validate(const char **bad_name, const char **why);

/* ---- core API ---------------------------------------------------------- */
void wd24_ctx_init(struct wd24_ctx *c, const struct wd24_backend *be);
int wd24_guard(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 old, u8 val);
int wd24_write(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 val);
int wd24_update(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 mask, u8 val);
void wd24_note(struct wd24_ctx *c, const char *text);

int wd24_unlock(struct wd24_ctx *c);
int wd24_panel_off(struct wd24_ctx *c);
int wd24_panel_on(struct wd24_ctx *c);
int wd24_set_route(struct wd24_ctx *c, enum wd24_route r,
		   const struct wd24_reg *shadow, unsigned int nshadow);
int wd24_plan_mode(struct wd24_ctx *c, const struct wd24_mode *m,
		   enum wd24_route r);

extern const struct wd24_backend wd24_dryrun_backend;

const char *wd24_rf_name(enum wd24_rf rf);

#endif
