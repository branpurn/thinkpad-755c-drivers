/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wd24adv - experimental "advanced" driver for the WD90C24/WD90C24A(2) on
 * the ThinkPad 755C.  This header is shared by the kernel module and the
 * host-side unit tests (tests/), so it must not depend on kernel headers
 * beyond the small compat block below.
 *
 * wd24adv is a SEPARATE module from wd90c24fb: it shares no source, header,
 * ioctl or build file with it, and only one of the two may be loaded.
 *
 * Safety model (see README.md):
 *  - no runtime panel/CRT routing at all: the BIOS chooses the route at
 *    POST (Fn+F7 is handled by SMI).  PR18/PR19/PR1A/PR36, the shadow CRTC
 *    (CR34 = xxxxx110b), CR35/CR3A, panel power (PR57 b2, PR4 b5) are never
 *    written; PM ports 24h-27h, 0D00h/0D01h and CMOS are not representable;
 *  - every write is either a standard VGA register or a register that
 *    wd90c24fb already writes (whitelist in wd24_guard());
 *  - unlock/relock idioms are exactly the wd90c24fb ones.
 *
 * Register knowledge: public WD90C24 data sheet, XFree86 pvga1 driver (via
 * wd90c24fb), public IBM VGA mode values, and live reads of the 755C.  No
 * vendor code is copied or disassembled here.
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
typedef uint64_t u64;
#endif

/* ---- register files ---------------------------------------------------- */
enum wd24_rf {
	WD24_SR,	/* 3C4/3C5 sequencer */
	WD24_GR,	/* 3CE/3CF graphics */
	WD24_CR,	/* 3D4/3D5 CRTC (colour: 3CC b0 = 1 on the 755C) */
	WD24_AR,	/* 3C0 attribute (idx 20h = palette address source) */
	WD24_MISC,	/* 3C2 write / 3CC read, idx 0 */
	WD24_NRF
};

#define WD24_NIDX	0x40	/* indices tracked per file (model copy) */

/* Paradise registers by data-sheet name (same numbers as wd90c24_hw.h) */
#define WD24_GR_PR0A	0x09	/* bank A */
#define WD24_GR_PR0B	0x0a	/* bank B */
#define WD24_GR_PR1	0x0b	/* memory size / map: b5:4 aperture (never) */
#define WD24_GR_PR2	0x0c	/* video select (not written on a C24) */
#define WD24_GR_PR3	0x0d	/* CRTC control */
#define WD24_GR_PR4	0x0e	/* video control; b5 = panel pins tri-state */
#define WD24_GR_PR5	0x0f	/* 05h unlocks PR0-PR4 */
#define WD24_CR_PR10	0x29	/* 85h unlocks PR11-PR17 */
#define WD24_CR_PR11	0x2a
#define WD24_CR_PR12	0x2b
#define WD24_CR_PR13	0x2c
#define WD24_CR_PR14	0x2d
#define WD24_CR_PR15	0x2e
#define WD24_CR_PR16	0x2f
#define WD24_CR_PR18	0x31	/* flat panel status   (read only here) */
#define WD24_CR_PR19	0x32	/* flat panel control  (read only here) */
#define WD24_CR_PR1A	0x33
#define WD24_CR_PR1B	0x34	/* write-only lock: (L&0F)|A0 opens PR18.. */
#define WD24_CR_PR36	0x3b
#define WD24_SR_PR20	0x06	/* (S & EFh) | 48h unlocks SR07+ */
#define WD24_SR_PR30A	0x10
#define WD24_SR_PR31	0x11
#define WD24_SR_PR32	0x12
#define WD24_SR_PR57	0x19	/* b2: panel power (never written) */
#define WD24_SR_PR68	0x31	/* clock select, behind SR35 = 50h */
#define WD24_SR_PR69	0x32
#define WD24_SR_PR72	0x35	/* 50h opens PR68/PR69 */

#define WD24_PR19_LCD		0x10
#define WD24_PR19_CRT		0x20
#define WD24_PR19_SCALE		0x0c	/* b2 centre/expand enable, b3 1 = expand */
#define WD24_PR19_SC_ON		0x04
#define WD24_PR19_SC_EXPAND	0x08
#define WD24_PR4_TRISTATE	0x20
#define WD24_PR1B_FP		0xa0
#define WD24_PR1_APERTURE	0x30

/* The only panel state wd24adv accepts: live-verified on the 755C
 * (wd90c24fb load message and wd24probe3, 2026-11-02). */
#define WD24_LIVE_PR18		0xc7	/* single TFT, LCD-only route */
#define WD24_LIVE_PR19		0x54	/* LCD on, CRT off, centring on */

/* ---- event log (what was, or in dry-run would have been, done) -------- */
enum wd24_ev {
	WD24_EV_WRITE,		/* rf, idx, old, val */
	WD24_EV_REFUSED,	/* guard refused rf/idx/val; err */
	WD24_EV_NOTE,		/* static text */
	WD24_EV_CHECK,		/* entry check failed: rf/idx read val, expected old */
	WD24_EV_MISMATCH,	/* readback after a write: wrote old, read val */
	WD24_EV_BULK,		/* DAC / video memory block: note, val = plane */
};

struct wd24_event {
	u8 type, rf, idx, old, val;
	bool old_known;
	int err;
	const char *note;	/* NOTE text, or the source tag of a write */
};

#define WD24_MAXEV	1024

struct wd24_ctx;

/*
 * Backends: the dry-run recorder (wd24_dryrun_backend, model only) and the
 * port backend (wd24_port_backend, wd24adv_port.c), which executes the same
 * plan through a struct wd24_io: inb/outb on the real ports in the kernel
 * (live=1), or a host port model in tests/.  The plan code is identical.
 *
 * live: reads come from the chip, so wd24_write() pre-reads the registers
 * the guard needs and reads every written register back (wd24_verify()).
 */
struct wd24_backend {
	const char *name;
	bool live;
	int (*write)(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 val);
	u8 (*read)(struct wd24_ctx *c, enum wd24_rf rf, u8 idx);
	/* 3C4 index width test (wd24probe3): 1 = SR06 latch unlocked
	 * (index bits 5:3 stick), 0 = locked, <0 = no hardware */
	int (*seq_unlocked)(struct wd24_ctx *c);
	/* DAC: 3C8/3C7 index 0, 768 bytes via 3C9 (vgastate cmap order);
	 * pel = 3C6 value written first (vgastate writes FFh) */
	void (*dac_read)(struct wd24_ctx *c, u8 pel, u8 *rgb768);
	void (*dac_write)(struct wd24_ctx *c, u8 pel, const u8 *rgb768);
	/* video memory window A0000-AFFFF, as currently mapped by the chip */
	void (*mem_read)(struct wd24_ctx *c, u32 off, u8 *buf, u32 n);
	void (*mem_write)(struct wd24_ctx *c, u32 off, const u8 *buf, u32 n);
	/* bracket a whole plan: save / restore the 3C4, 3CE, 3D4 indices */
	void (*begin)(struct wd24_ctx *c);
	void (*end)(struct wd24_ctx *c);
};

/* Port access used by wd24_port_backend; kernel: inb/outb + ioremap */
struct wd24_io {
	u8 (*inb)(void *priv, u16 port);
	void (*outb)(void *priv, u8 val, u16 port);
	u8 (*memr)(void *priv, u32 off);
	void (*memw)(void *priv, u32 off, u8 val);
	void *priv;
};

/* Saved extended state: the same fields wd90c24fb's wd_ext_state has. */
struct wd24_ext {
	u8 misc, cr11;
	u8 pr0a, pr0b, pr1, pr3, pr4, pr5;
	u8 pr10, pr11, pr13, pr14, pr15, pr16;
	u8 pr20, pr30a, pr31, pr32;
	u8 pr68, pr69, pr72;
};

/* ---- standard VGA register set (same layout as wd90c24fb's wd_std) ----- */
struct wd24_std {
	u8 misc;
	u8 seq[5];
	u8 crtc[25];
	u8 gfx[9];
	u8 attr[21];
};

#define WD24_PLANE	0x10000u	/* bytes per plane (64 KiB window) */
#define WD24_PLANES_SZ	(4 * WD24_PLANE)

/* what wd24_plan_load() saved (c->have) */
#define WD24_HAVE_LOCKS		0x01	/* MISC, CR11, PR5, PR10, PR20 */
#define WD24_HAVE_EXT		0x02	/* the rest of struct wd24_ext */
#define WD24_HAVE_CMAP		0x04	/* DAC, 768 bytes */
#define WD24_HAVE_STD		0x08	/* struct wd24_std (vgastate save_vga_mode) */
#define WD24_HAVE_PLANES	0x10	/* 4 x 64 KiB (fonts + text) */

enum wd24_phase { WD24_PH_LOAD, WD24_PH_MODE, WD24_PH_RESTORE };

struct wd24_ctx {
	const struct wd24_backend *be;
	const struct wd24_io *io;	/* port backend only */
	bool allow_clock;	/* PR68/PR69 data writes (no mode uses them) */
	/* state */
	bool saved;		/* lock values in bios are valid (guard) */
	unsigned int have;	/* WD24_HAVE_* */
	bool restoring;		/* inside wd24_restore_all(): mismatches don't stop */
	bool failed;		/* a readback mismatch happened */
	bool in_text;		/* chip is (believed) in the BIOS text state */
	bool trial_pending;	/* a mode set waits for confirm or expiry */
	bool confirmed;		/* the pending mode was confirmed */
	/* entry state the next plan expects (set at the end of each plan) */
	u8 exp_misc, exp_gr0f, exp_cr29, exp_sr06;
	struct wd24_std vga;	/* saved standard registers */
	u8 cmap[768];		/* saved DAC */
	u8 *planes;		/* 4 x 64 KiB, caller-owned; NULL: not saved */
	u8 saved_idx[3];	/* port backend: 3C4/3CE/3D4 index at begin() */
	unsigned long hw_data, hw_index;	/* port backend: port writes */
	unsigned int nmismatch, nbulk;
	struct wd24_ext bios;	/* BIOS state saved at load */
	bool pr1b_armed;	/* CR34 lock value just read: idiom may start */
	bool pr1b_open;		/* inside the CR34 read idiom */
	u8 pr1b_lock;		/* CR34 value read at the start of the idiom */
	bool pr72_open;		/* SR35 = 50h written, not yet restored */
	u8 reg[WD24_NRF][WD24_NIDX];	/* model: last known value */
	u64 known[WD24_NRF];		/* bit i: reg[rf][i] is known */
	const char *tag;	/* source tag attached to the next writes */
	/* log */
	struct wd24_event ev[WD24_MAXEV];
	unsigned int nev, nwrites, nrefused, dropped;
};

/* ---- mode table -------------------------------------------------------- */
enum wd24_status { WD24_PROVEN = 1, WD24_UNPROVEN = 2 };
enum wd24_extsel {
	WD24_EXT_NONE,		/* UNPROVEN: no sequence */
	WD24_EXT_PACKED8,	/* wd90c24fb 640x480x8 extended values */
	WD24_EXT_BIOS,		/* the BIOS state saved at load (text restore) */
};

#define WD24M_DIRECT		0x0008	/* fits the 64 KiB window: direct mmap */
#define WD24M_BANKED		0x0010	/* PR0A banking */
#define WD24M_DOUBLESCAN	0x0020
#define WD24M_PLANAR		0x0040	/* unchained 4-plane */
#define WD24M_CENTRED		0x0080	/* < 480 lines: panel centres (PR19 b3:2=01) */

struct wd24_mode {
	const char *name;
	u16 xres, yres;		/* visible */
	u16 vxres, vyres;	/* virtual */
	u8 bpp;
	u8 bios_mode;		/* INT 10h mode it corresponds to, 0 = none */
	u8 status;		/* enum wd24_status */
	u8 ext;			/* enum wd24_extsel */
	u32 frame_bytes;
	u32 flags;
	const struct wd24_std *std;	/* NULL when UNPROVEN */
	const char *evidence;	/* one line: why PROVEN / why not */
};

extern const struct wd24_mode wd24_modes[];
extern const unsigned int wd24_nmodes;

const struct wd24_mode *wd24_mode_find(const char *name);
int wd24_mode_check(const struct wd24_mode *m, const char **why);
int wd24_modes_validate(const char **bad_name, const char **why);

/* ---- model of the boot (BIOS text) state, for the dry-run backend ----- */
enum wd24_src { WD24_SRC_LIVE = 'L', WD24_SRC_POST = 'P', WD24_SRC_STD = 'S' };
struct wd24_model_reg {
	u8 rf, idx, val, src;
};
extern const struct wd24_model_reg wd24_boot_model[];
extern const unsigned int wd24_boot_model_n;
void wd24_model_boot(struct wd24_ctx *c);

/* ---- core API ---------------------------------------------------------- */
void wd24_ctx_init(struct wd24_ctx *c, const struct wd24_backend *be);
int wd24_guard(const struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 old, u8 val);
int wd24_write(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 val);
u8 wd24_read(struct wd24_ctx *c, enum wd24_rf rf, u8 idx);
int wd24_modify(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 mask, u8 val);
void wd24_note(struct wd24_ctx *c, const char *text);

int wd24_read_panel(struct wd24_ctx *c, u8 *pr18, u8 *pr19);
int wd24_check_panel(struct wd24_ctx *c);
int wd24_unlock(struct wd24_ctx *c);
int wd24_save_ext(struct wd24_ctx *c, struct wd24_ext *s);
int wd24_restore_ext(struct wd24_ctx *c, const struct wd24_ext *s);
int wd24_relock(struct wd24_ctx *c, const struct wd24_ext *s);
int wd24_screen_on(struct wd24_ctx *c, bool on);
int wd24_write_std(struct wd24_ctx *c, const struct wd24_std *std);
void wd24_mode_ext(const struct wd24_mode *m, const struct wd24_ext *bios,
		   struct wd24_ext *ext);

int wd24_entry_check(struct wd24_ctx *c, enum wd24_phase ph);
int wd24_verify(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 val);
int wd24_save_cmap(struct wd24_ctx *c);
int wd24_restore_cmap(struct wd24_ctx *c);
int wd24_save_std(struct wd24_ctx *c);
int wd24_restore_std(struct wd24_ctx *c);
int wd24_save_planes(struct wd24_ctx *c);
int wd24_restore_planes(struct wd24_ctx *c);
int wd24_restore_all(struct wd24_ctx *c);
void wd24_log_reset(struct wd24_ctx *c);

int wd24_plan_load(struct wd24_ctx *c);
int wd24_plan_mode(struct wd24_ctx *c, const struct wd24_mode *m);
int wd24_plan_text(struct wd24_ctx *c);
int wd24_pattern(struct wd24_ctx *c, const struct wd24_mode *m);
/* trial: every successful mode set leaves trial_pending; expiry without a
 * confirm runs the text restore (the kernel module arms a timer) */
int wd24_confirm(struct wd24_ctx *c);
int wd24_trial_expire(struct wd24_ctx *c);

/* shared self-test (kernel /proc/wd24adv/selftest and host test) */
typedef void (*wd24_report_fn)(void *arg, bool ok, const char *what);
int wd24_selftest(struct wd24_ctx *t, wd24_report_fn report, void *arg);

extern const struct wd24_backend wd24_dryrun_backend;
extern const struct wd24_backend wd24_port_backend;

const char *wd24_rf_name(enum wd24_rf rf);

#endif
