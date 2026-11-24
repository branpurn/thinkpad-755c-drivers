/* SPDX-License-Identifier: GPL-2.0 */
/*
 * WD90C24 subset used by the 13h flip path. Host model and the QEMU
 * device share this file. Side effects that are not cited are counted
 * and logged. They do not change the picture.
 */
#ifndef WD24_CHIP_H
#define WD24_CHIP_H

#include <stddef.h>

struct wd24_chip;

struct wd24_chip *wd24_chip_new(void);
void wd24_chip_free(struct wd24_chip *c);

/* 0: modeled write. -1: refused or unmodeled (the byte may still read back). */
int wd24_chip_outb(struct wd24_chip *c, unsigned port, unsigned val);
int wd24_chip_inb(struct wd24_chip *c, unsigned port);

int wd24_chip_mem_write(struct wd24_chip *c, unsigned off,
			const unsigned char *p, unsigned n);
int wd24_chip_mem_read(struct wd24_chip *c, unsigned off,
		       unsigned char *p, unsigned n);

/* std_13h from wd24adv_modes.c, through the port writes. */
int wd24_chip_program_13h(struct wd24_chip *c);

/* Step to the next 3DA bit 3 rising edge. 1 if the armed start latched. */
int wd24_chip_wait_latch(struct wd24_chip *c);

/* 0 when the CRTC clock or totals are not the modeled set. */
unsigned wd24_chip_field_millihz(const struct wd24_chip *c);
unsigned wd24_chip_h_dots(const struct wd24_chip *c);
unsigned wd24_chip_v_lines(const struct wd24_chip *c);
unsigned wd24_chip_v_disp(const struct wd24_chip *c);

/* 640x480 panel. Returns 0, or -1 if the scale is not the 13h set. */
int wd24_chip_render(struct wd24_chip *c, unsigned char *rgb);

unsigned wd24_chip_unknown(const struct wd24_chip *c);
unsigned wd24_chip_refused(const struct wd24_chip *c);
unsigned wd24_chip_unmodeled(const struct wd24_chip *c);
unsigned wd24_chip_latch_to(const struct wd24_chip *c);
unsigned wd24_chip_latches(const struct wd24_chip *c);
unsigned char wd24_chip_lat_cr0c(const struct wd24_chip *c);
unsigned char wd24_chip_lat_cr0d(const struct wd24_chip *c);
unsigned char wd24_chip_pr0a(const struct wd24_chip *c);
const char *wd24_chip_log(const struct wd24_chip *c);

/* Extra stall per VRAM byte. 0 until a measured figure is applied. */
void wd24_chip_set_stall_ns(struct wd24_chip *c, unsigned ns);
unsigned wd24_chip_stall_ns(const struct wd24_chip *c);

#endif
