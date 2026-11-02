/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal port shim so tests/xcheck.c can run wd90c24fb's register layer
 * (modules/wd90c24fb/wd90c24_hw.c, used read-only, not modified) on the
 * host and record every register write it makes.  wd90c24_hw.h includes
 * "wdmock.h"; this directory's copy is found through -I.
 */
#ifndef WD24_XCHECK_SHIM_H
#define WD24_XCHECK_SHIM_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
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
#endif
