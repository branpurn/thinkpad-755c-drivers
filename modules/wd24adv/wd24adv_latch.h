/* SPDX-License-Identifier: GPL-2.0 */
/*
 * One thrash latch schedules one console give-back. Later latched
 * draws must not schedule again and must not print: fbcon calls the
 * draw with console_lock held, and a printk from there writes that
 * same console.
 */
#ifndef WD24ADV_LATCH_H
#define WD24ADV_LATCH_H

static inline int wd24_latch_arm(int latched, int *armed)
{
	if (!latched || !armed || *armed)
		return 0;
	*armed = 1;
	return 1;
}

#endif
