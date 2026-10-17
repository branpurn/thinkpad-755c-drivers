/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Host-side register model of Paradise/WD VGA chips for unit tests of
 * modules/wd90c24fb/wd90c24_hw.c. Lock semantics follow the WD90C24A/A2
 * data sheet and what XFree86's probe relies on; plain VGA behaves like
 * QEMU's std VGA (extended indices read 0, writes ignored).
 */
#ifndef WDMOCK_H
#define WDMOCK_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

u8 wd_inb(u16 port);
void wd_outb(u8 val, u16 port);
u16 wd_inw(u16 port);
void wd_outw(u16 val, u16 port);
void wd_msleep(unsigned int ms);
void wd_udelay(unsigned int us);
struct wd_hw;
void wd_win_write(struct wd_hw *hw, u32 off, const void *src, size_t n);
void wd_win_writeb(struct wd_hw *hw, u32 off, u8 v);
u8 wd_win_readb(struct wd_hw *hw, u32 off);

enum mock_kind { M_VGA, M_PVGA1, M_C00, M_C1X, M_C20, M_C24, M_C24A, M_C30, M_C31 };

struct mock {
	enum mock_kind kind;
	u8 misc, seq_i, gfx_i, crtc_i;
	u8 seq[256], gfx[256], crtc[256];
	u8 pr1b;		/* CR34, write-only on the chip */
	u8 fp[256];		/* flat-panel registers behind PR1B */
	u16 ext_index, cursor;
	u16 blt[16];
	u8 vram[1 << 20];
	u8 attr[32], attr_i, attr_ff;
	u8 dac[256][3], dac_i, dac_c;
	unsigned int win_oob;		/* window accesses past 64 KiB */
	/* accounting */
	unsigned int timing_writes;	/* CRTC 0x00-0x18 except 0x11, MISC, SEQ 0-4 */
	unsigned int ext_port_writes;
	unsigned int sleeps_ms;
};
extern struct mock M;

void mock_reset(enum mock_kind k);
/* Snapshot of everything a probe must leave alone. */
struct mock_snap {
	u8 misc, seq[256], gfx[256], crtc[256], pr1b, fp[256];
	u16 cursor;
};
void mock_snap(struct mock_snap *s);
int mock_snap_diff(const struct mock_snap *a, const struct mock_snap *b, char *why, size_t n);
u32 mock_win_addr(u32 off);	/* VRAM address the chip would use */
#endif
