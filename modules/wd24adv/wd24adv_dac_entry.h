/* SPDX-License-Identifier: GPL-2.0 */
/*
 * One VGA DAC entry, host-testable. Not called by the loaded module.
 *
 * X gives 16-bit channels. The restore path compares the low 6 bits
 * (wd24_restore_cmap). The value written to 3C9 is channel >> 10.
 * regno is the 3C8 write index. The saved 768-byte snapshot is not
 * an argument: a later full restore writes that snapshot back.
 */
#ifndef WD24ADV_DAC_ENTRY_H
#define WD24ADV_DAC_ENTRY_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

struct wd24_dac_entry {
	u8 index;
	u8 pel;		/* 3C6, same FFh the full restore writes */
	u8 rgb[3];	/* 3C9, already masked to 6 bits */
};

static inline int wd24_dac_entry(u32 regno, u16 r, u16 g, u16 b,
				 struct wd24_dac_entry *e)
{
	if (!e || regno > 255)
		return -22; /* -EINVAL, no errno.h in the kernel header */
	e->index = (u8)regno;
	e->pel = 0xff;
	e->rgb[0] = (u8)((r >> 10) & 0x3f);
	e->rgb[1] = (u8)((g >> 10) & 0x3f);
	e->rgb[2] = (u8)((b >> 10) & 0x3f);
	return 0;
}

/* Copy of a DAC image. Does not touch the saved snapshot. */
static inline void wd24_dac_entry_store(u8 image[768],
					const struct wd24_dac_entry *e)
{
	unsigned int n = (unsigned int)e->index * 3;

	image[n] = e->rgb[0];
	image[n + 1] = e->rgb[1];
	image[n + 2] = e->rgb[2];
}

#endif
