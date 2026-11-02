/*
  wdfb: SDL2 video driver for wd90c24fb, part of thinkpad-755c-drivers.
  Copyright (C) 2026 the thinkpad-755c-drivers authors.
  Licensed like SDL itself (zlib), so it can be linked into any SDL app:

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
/*
 * wdfb: SDL2 video driver for the wd90c24fb Linux framebuffer driver
 * (IBM ThinkPad 755C, WD90C24 640x480 8bpp). Altered source: not part of
 * upstream SDL. See thinkpad-755c-drivers/sdl/README.md.
 */
#include "../../SDL_internal.h"

#ifndef SDL_wdfbvideo_h_
#define SDL_wdfbvideo_h_

#include "../SDL_sysvideo.h"

typedef struct WDFB_Data
{
    /* frame buffer */
    int fd;
    SDL_bool fake;          /* a regular file stands in for /dev/fb0 (tests) */
    char device[256];
    Uint8 *fb;              /* mmap()ed shadow (wd90c24fb) or the file */
    size_t maplen;
    int xres, yres, pitch;
    Uint32 caps;            /* WDFB_IOC_CAPS; 0 = no damage ioctl */
    SDL_bool use_ops;       /* send WDFB_OP_DAMAGE per updated rect */
    SDL_bool warned_ops;
    int border;             /* palette index for the area outside the window */

    /* palette */
    SDL_bool have_saved_cmap;
    Uint16 saved_r[256], saved_g[256], saved_b[256];
    const SDL_Palette *pal_seen;
    Uint32 pal_version;

    /* the one window */
    SDL_Window *window;
    int scale;              /* 1 or 2 */
    int ox, oy, ww, wh;     /* window origin and size in frame pixels/1 */
    Uint8 *back;            /* scale 2: the window surface; else NULL */

    /* statistics (SDL_WDFB_DEBUG) */
    int debug;
    Uint32 st_updates, st_rects, st_ioctls, st_pixels, st_last;

    /* console / input (SDL_wdfbevents.c) */
    int tty_fd;
    SDL_bool tty_saved;
    int old_kd_mode, old_kb_mode;
    int kbd_fd[8];
    int nkbd;
    SDL_bool raw_console;   /* keys come from tty_fd in K_MEDIUMRAW */
    int raw_pending, raw_code, raw_release;
    int cab_count;          /* Ctrl+Alt+Backspace presses */
} WDFB_Data;

/* SDL_wdfbevents.c */
extern int WDFB_InitInput(_THIS);
extern void WDFB_QuitInput(_THIS);
extern void WDFB_PumpEvents(_THIS);
extern void WDFB_RestoreConsole(void);  /* async-signal-safe */

#endif /* SDL_wdfbvideo_h_ */
