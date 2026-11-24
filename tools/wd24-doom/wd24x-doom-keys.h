/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Bytes from a termios tty become Doom key-down events.
 * A tty does not report key-up. Each recognized byte is one tap:
 * the caller emits down, then up, so a direction does not stick.
 * Arrow keys are the Linux console sequence ESC [ A/B/C/D.
 * This does not change KD mode.
 */
#ifndef WD24X_DOOM_KEYS_H
#define WD24X_DOOM_KEYS_H

#define WD24X_KEY_RIGHT		0xae
#define WD24X_KEY_LEFT		0xac
#define WD24X_KEY_UP		0xad
#define WD24X_KEY_DOWN		0xaf
#define WD24X_KEY_ESCAPE	27
#define WD24X_KEY_ENTER		13

struct wd24x_keys {
	unsigned esc;		/* 0, 1 = saw ESC, 2 = saw ESC [ */
};

/* Returns 1 and sets *doomkey when b completes a key. */
static inline int wd24x_keys_byte(struct wd24x_keys *s, unsigned char b,
				  unsigned char *doomkey)
{
	if (!s || !doomkey)
		return 0;
	if (s->esc == 1) {
		if (b == '[') {
			s->esc = 2;
			return 0;
		}
		s->esc = 0;
		*doomkey = WD24X_KEY_ESCAPE;
		return 1;
	}
	if (s->esc == 2) {
		s->esc = 0;
		if (b == 'A')
			*doomkey = WD24X_KEY_UP;
		else if (b == 'B')
			*doomkey = WD24X_KEY_DOWN;
		else if (b == 'C')
			*doomkey = WD24X_KEY_RIGHT;
		else if (b == 'D')
			*doomkey = WD24X_KEY_LEFT;
		else
			return 0;
		return 1;
	}
	if (b == 27) {
		s->esc = 1;
		return 0;
	}
	if (b == '\r' || b == '\n') {
		*doomkey = WD24X_KEY_ENTER;
		return 1;
	}
	if (b >= 'a' && b <= 'z')
		b = (unsigned char)(b - 'a' + 'A');
	if (b >= 32 && b < 127) {
		*doomkey = b;
		return 1;
	}
	return 0;
}

#endif
