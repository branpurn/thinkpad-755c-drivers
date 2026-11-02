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
 * wdfb: SDL2 video driver for wd90c24fb (IBM ThinkPad 755C, WD90C24,
 * 640x480, 8 bits per pixel, palette). Altered source: not part of
 * upstream SDL. See thinkpad-755c-drivers/sdl/README.md.
 *
 * mmap() of /dev/fb0 gives wd90c24fb's frame shadow in system RAM. The
 * driver copies it to video memory whole, 10 times a second, until the
 * client reports damage with WDFB_IOC_OPS; from then on only reported
 * rectangles are copied, at once, in the client's time slice. So this
 * driver gives the application the shadow itself as its window surface
 * (no copy at scale 1) and turns SDL_UpdateWindowSurfaceRects() into one
 * WDFB_OP_DAMAGE per rectangle. The palette goes through FBIOPUTCMAP.
 *
 * One window, centred in the 640x480 frame (or 2x scaled with
 * SDL_WDFB_SCALE=2). Keyboard: evdev if present, else the console in
 * K_MEDIUMRAW. No X, no OpenGL, no mouse.
 *
 * Environment:
 *   SDL_WDFB_DEVICE  frame buffer device (default $SDL_FBDEV, /dev/fb0). A
 *                    regular file of >= 640*480 bytes is a fake frame buffer
 *                    for tests; the palette then goes to <file>.pal.
 *   SDL_WDFB_SCALE   1 (default), 2 (integer 2x, when it fits) or auto
 *                    (2 when the window fits twice, else 1).
 *   SDL_WDFB_CENTER  0 = window at the top left (default 1, centred).
 *   SDL_WDFB_BORDER  palette index for the area around the window (0).
 *   SDL_WDFB_FORCE   1 = share the frame buffer with another client (X).
 *   SDL_WDFB_NOOPS   1 = no damage ioctl: the driver's 10 Hz poll.
 *   SDL_WDFB_DEBUG   1 = statistics once a second on stderr, 2 = each op.
 *   SDL_WDFB_TTY, SDL_WDFB_INPUT, SDL_WDFB_KBD: see SDL_wdfbevents.c.
 */

#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_WDFB

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>

#include "SDL_video.h"
#include "SDL_hints.h"
#include "SDL_timer.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_keyboard_c.h"
#include "SDL_wdfbvideo.h"
#include "wd90c24fb_ioctl.h"

#define WDFB_DRIVER_NAME "wdfb"
#define WDFB_W 640
#define WDFB_H 480

static int WDFB_VideoInit(_THIS);
static void WDFB_VideoQuit(_THIS);

static const char *WDFB_Env(const char *name, const char *def)
{
    const char *v = SDL_getenv(name);
    return (v && *v) ? v : def;
}

static SDL_bool WDFB_EnvTrue(const char *name)
{
    const char *v = SDL_getenv(name);
    return (v && *v && *v != '0') ? SDL_TRUE : SDL_FALSE;
}

static const char *WDFB_DevicePath(void)
{
    return WDFB_Env("SDL_WDFB_DEVICE", WDFB_Env("SDL_FBDEV", "/dev/fb0"));
}

/* Without SDL_VIDEODRIVER=wdfb, only claim a frame buffer that is wd90c24fb. */
static SDL_bool WDFB_Available(void)
{
    const char *hint = SDL_GetHint(SDL_HINT_VIDEODRIVER);
    struct fb_fix_screeninfo fix;
    SDL_bool ok = SDL_FALSE;
    int fd;

    if (hint && *hint) {
        return SDL_TRUE; /* SDL_VideoInit only calls us when the hint names "wdfb" */
    }
    fd = open(WDFB_DevicePath(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return SDL_FALSE;
    }
    SDL_zero(fix);
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0 && SDL_strncmp(fix.id, "WD90C24", 7) == 0) {
        ok = SDL_TRUE;
    }
    close(fd);
    return ok;
}

static void WDFB_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device->driverdata);
    SDL_free(device);
}

static int WDFB_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    return 0; /* every mode is the 640x480 frame; smaller ones are centred */
}

static void WDFB_GetDisplayModes(_THIS, SDL_VideoDisplay *display)
{
    static const int sizes[][2] = { { 640, 480 }, { 640, 400 }, { 320, 240 }, { 320, 200 } };
    WDFB_Data *data = (WDFB_Data *)_this->driverdata;
    SDL_DisplayMode mode;
    int i;

    for (i = 0; i < (int)SDL_arraysize(sizes); i++) {
        if (sizes[i][0] > data->xres || sizes[i][1] > data->yres) {
            continue;
        }
        SDL_zero(mode);
        mode.format = SDL_PIXELFORMAT_INDEX8;
        mode.w = sizes[i][0];
        mode.h = sizes[i][1];
        SDL_AddDisplayMode(display, &mode);
    }
}

static int WDFB_CreateWindow(_THIS, SDL_Window *window)
{
    WDFB_Data *data = (WDFB_Data *)_this->driverdata;

    if (data->window) {
        return SDL_SetError("wdfb: only one window");
    }
    data->window = window;
    window->x = 0;
    window->y = 0;
    if (window->w > data->xres) {
        window->w = data->xres;
    }
    if (window->h > data->yres) {
        window->h = data->yres;
    }
    window->flags |= SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_BORDERLESS;
    SDL_SetKeyboardFocus(window);
    return 0;
}

static void WDFB_DestroyWindow(_THIS, SDL_Window *window)
{
    WDFB_Data *data = (WDFB_Data *)_this->driverdata;

    if (data->window == window) {
        data->window = NULL;
    }
}

/* --- damage ------------------------------------------------------------ */

static void WDFB_SendOps(WDFB_Data *data, struct wdfb_op *ops, int n)
{
    struct wdfb_ops req;
    int i, r;

    if (data->debug > 1) {
        for (i = 0; i < n; i++) {
            fprintf(stderr, "wdfb: DAMAGE %d,%d %dx%d\n", ops[i].x, ops[i].y, ops[i].w, ops[i].h);
        }
    }
    if (!data->use_ops || n == 0) {
        return;
    }
    data->st_ioctls++;
    req.n = (Uint32)n;
    req.flags = 0;
    req.ops = (__u64)(uintptr_t)ops;
    r = ioctl(data->fd, WDFB_IOC_OPS, &req);
    if (r < 0) {
        /* the driver went back to its 10 Hz poll on its own */
        if (!data->warned_ops) {
            fprintf(stderr, "wdfb: WDFB_IOC_OPS: %s; using the driver's 10 Hz refresh\n", strerror(errno));
            data->warned_ops = SDL_TRUE;
        }
        data->use_ops = SDL_FALSE;
    }
}

static void WDFB_Damage(WDFB_Data *data, const SDL_Rect *rects, int n)
{
    struct wdfb_op ops[64];
    int i, k = 0;

    for (i = 0; i < n; i++) {
        struct wdfb_op *o = &ops[k];
        SDL_memset(o, 0, sizeof(*o));
        o->op = WDFB_OP_DAMAGE;
        o->x = (__u16)rects[i].x;
        o->y = (__u16)rects[i].y;
        o->w = (__u16)rects[i].w;
        o->h = (__u16)rects[i].h;
        data->st_pixels += (Uint32)rects[i].w * (Uint32)rects[i].h;
        if (++k == (int)SDL_arraysize(ops)) {
            WDFB_SendOps(data, ops, k);
            k = 0;
        }
    }
    if (k) {
        WDFB_SendOps(data, ops, k);
    }
}

static void WDFB_DamageAll(WDFB_Data *data)
{
    SDL_Rect r;

    r.x = 0;
    r.y = 0;
    r.w = data->xres;
    r.h = data->yres;
    WDFB_Damage(data, &r, 1);
}

/* --- palette ----------------------------------------------------------- */

static void WDFB_PutCmap(WDFB_Data *data, const Uint16 *r, const Uint16 *g, const Uint16 *b, int n)
{
    struct fb_cmap cmap;

    if (data->fake) {
        /* tests: <file>.pal, 256 x RGB, 8 bits each */
        char path[sizeof(data->device) + 8];
        Uint8 rgb[256 * 3];
        int i, fd;

        for (i = 0; i < n && i < 256; i++) {
            rgb[i * 3 + 0] = (Uint8)(r[i] >> 8);
            rgb[i * 3 + 1] = (Uint8)(g[i] >> 8);
            rgb[i * 3 + 2] = (Uint8)(b[i] >> 8);
        }
        SDL_snprintf(path, sizeof(path), "%s.pal", data->device);
        fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0) {
            if (write(fd, rgb, (size_t)n * 3) < 0) {
                /* tests only */
            }
            close(fd);
        }
        return;
    }
    cmap.start = 0;
    cmap.len = (__u32)n;
    cmap.red = (__u16 *)r;
    cmap.green = (__u16 *)g;
    cmap.blue = (__u16 *)b;
    cmap.transp = NULL;
    if (ioctl(data->fd, FBIOPUTCMAP, &cmap) < 0 && data->debug) {
        fprintf(stderr, "wdfb: FBIOPUTCMAP: %s\n", strerror(errno));
    }
}

static void WDFB_SyncPalette(WDFB_Data *data, SDL_Window *window)
{
    const SDL_Palette *pal;
    Uint16 r[256], g[256], b[256];
    int i, n;

    if (!window->surface || !window->surface->format) {
        return;
    }
    pal = window->surface->format->palette;
    if (!pal || (pal == data->pal_seen && pal->version == data->pal_version)) {
        return;
    }
    n = SDL_min(pal->ncolors, 256);
    for (i = 0; i < n; i++) {
        r[i] = (Uint16)(pal->colors[i].r * 0x101);
        g[i] = (Uint16)(pal->colors[i].g * 0x101);
        b[i] = (Uint16)(pal->colors[i].b * 0x101);
    }
    WDFB_PutCmap(data, r, g, b, n);
    data->pal_seen = pal;
    data->pal_version = pal->version;
}

/* --- window framebuffer -------------------------------------------------- */

static int WDFB_PickScale(WDFB_Data *data, int w, int h)
{
    const char *s = WDFB_Env("SDL_WDFB_SCALE", "1");
    SDL_bool fits2 = (w * 2 <= data->xres && h * 2 <= data->yres) ? SDL_TRUE : SDL_FALSE;

    if (SDL_strcasecmp(s, "auto") == 0) {
        return fits2 ? 2 : 1;
    }
    if (SDL_atoi(s) == 2) {
        if (fits2) {
            return 2;
        }
        fprintf(stderr, "wdfb: %dx%d does not fit twice in %dx%d: scale 1\n", w, h, data->xres, data->yres);
    }
    return 1;
}

static void WDFB_DestroyWindowFramebuffer(_THIS, SDL_Window *window)
{
    WDFB_Data *data = (WDFB_Data *)_this->driverdata;

    SDL_free(data->back);
    data->back = NULL;
    data->pal_seen = NULL;
}

static int WDFB_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels, int *pitch)
{
    WDFB_Data *data = (WDFB_Data *)_this->driverdata;
    int w, h, y;

    WDFB_DestroyWindowFramebuffer(_this, window);
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0 || w > data->xres || h > data->yres) {
        return SDL_SetError("wdfb: window %dx%d does not fit in %dx%d", w, h, data->xres, data->yres);
    }
    data->scale = WDFB_PickScale(data, w, h);
    data->ww = w;
    data->wh = h;
    if (WDFB_EnvTrue("SDL_WDFB_CENTER") || !SDL_getenv("SDL_WDFB_CENTER")) {
        data->ox = (data->xres - w * data->scale) / 2;
        data->oy = (data->yres - h * data->scale) / 2;
    } else {
        data->ox = data->oy = 0;
    }

    /* a clean frame around the window */
    for (y = 0; y < data->yres; y++) {
        SDL_memset(data->fb + y * data->pitch, data->border, data->xres);
    }
    WDFB_DamageAll(data);

    *format = SDL_PIXELFORMAT_INDEX8;
    if (data->scale == 1) {
        /* the application draws straight into the shadow */
        *pixels = data->fb + data->oy * data->pitch + data->ox;
        *pitch = data->pitch;
    } else {
        data->back = (Uint8 *)SDL_calloc(1, (size_t)w * h);
        if (!data->back) {
            return SDL_OutOfMemory();
        }
        *pixels = data->back;
        *pitch = w;
    }
    if (data->debug) {
        fprintf(stderr, "wdfb: window %dx%d at %d,%d scale %d%s\n", w, h, data->ox, data->oy, data->scale,
                data->scale == 1 ? " (direct)" : "");
    }
    return 0;
}

/* 2x: each source pixel becomes a 2x2 block */
static void WDFB_Scale2(WDFB_Data *data, const SDL_Rect *r)
{
    const Uint8 *src = data->back + r->y * data->ww + r->x;
    Uint8 *dst = data->fb + (data->oy + r->y * 2) * data->pitch + data->ox + r->x * 2;
    int x, y;

    for (y = 0; y < r->h; y++) {
        Uint8 *d = dst;
        for (x = 0; x < r->w; x++) {
            Uint8 c = src[x];
            d[0] = c;
            d[1] = c;
            d += 2;
        }
        SDL_memcpy(dst + data->pitch, dst, (size_t)r->w * 2);
        src += data->ww;
        dst += data->pitch * 2;
    }
}

static void WDFB_Stats(WDFB_Data *data)
{
    Uint32 now = SDL_GetTicks();

    if (!data->debug) {
        return;
    }
    if (!data->st_last) {
        data->st_last = now;
    } else if (now - data->st_last >= 1000) {
        fprintf(stderr, "wdfb: %u updates %u rects %u ioctls %u KiB in %u ms\n",
                (unsigned)data->st_updates, (unsigned)data->st_rects, (unsigned)data->st_ioctls,
                (unsigned)(data->st_pixels / 1024), (unsigned)(now - data->st_last));
        data->st_updates = data->st_rects = data->st_ioctls = data->st_pixels = 0;
        data->st_last = now;
    }
}

static int WDFB_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    WDFB_Data *data = (WDFB_Data *)_this->driverdata;
    SDL_Rect out[64];
    int i, k = 0;

    WDFB_SyncPalette(data, window);
    data->st_updates++;
    for (i = 0; i < numrects; i++) {
        SDL_Rect r = rects[i];

        /* clip to the window */
        if (r.x < 0) {
            r.w += r.x;
            r.x = 0;
        }
        if (r.y < 0) {
            r.h += r.y;
            r.y = 0;
        }
        if (r.x + r.w > data->ww) {
            r.w = data->ww - r.x;
        }
        if (r.y + r.h > data->wh) {
            r.h = data->wh - r.y;
        }
        if (r.w <= 0 || r.h <= 0) {
            continue;
        }
        data->st_rects++;
        if (data->scale == 2) {
            WDFB_Scale2(data, &r);
        }
        out[k].x = data->ox + r.x * data->scale;
        out[k].y = data->oy + r.y * data->scale;
        out[k].w = r.w * data->scale;
        out[k].h = r.h * data->scale;
        if (++k == (int)SDL_arraysize(out)) {
            WDFB_Damage(data, out, k);
            k = 0;
        }
    }
    if (k) {
        WDFB_Damage(data, out, k);
    }
    WDFB_Stats(data);
    return 0;
}

/* --- coexistence: refuse a frame buffer that X (or anyone) has open ------- */

static SDL_bool WDFB_ProcOwnsFB(const char *pid, dev_t rdev, ino_t ino, char *comm, size_t commlen)
{
    char path[64], name[64];
    struct dirent *e;
    struct stat st;
    DIR *d;
    int fd, n;
    SDL_bool found = SDL_FALSE;

    comm[0] = '\0';
    SDL_snprintf(path, sizeof(path), "/proc/%s/comm", pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        n = (int)read(fd, comm, commlen - 1);
        comm[n > 0 ? n : 0] = '\0';
        if (n > 0 && comm[n - 1] == '\n') {
            comm[n - 1] = '\0';
        }
        close(fd);
    }
    SDL_snprintf(path, sizeof(path), "/proc/%s/fd", pid);
    d = opendir(path);
    if (!d) {
        /* not ours to look at: go by name */
        return (SDL_strcmp(comm, "Xfbdev") == 0 || SDL_strcmp(comm, "Xvesa") == 0 ||
                SDL_strcmp(comm, "Xorg") == 0) ? SDL_TRUE : SDL_FALSE;
    }
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        SDL_snprintf(name, sizeof(name), "%s/%s", path, e->d_name);
        if (stat(name, &st) == 0 &&
            (rdev ? (S_ISCHR(st.st_mode) && st.st_rdev == rdev) : (st.st_ino == ino))) {
            found = SDL_TRUE;
        }
    }
    closedir(d);
    return found;
}

static int WDFB_CheckExclusive(WDFB_Data *data)
{
    struct stat me;
    struct dirent *e;
    char self[16], comm[32];
    DIR *d;

    if (fstat(data->fd, &me) < 0) {
        return 0;
    }
    d = opendir("/proc");
    if (!d) {
        return 0;
    }
    SDL_snprintf(self, sizeof(self), "%d", (int)getpid());
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9' || SDL_strcmp(e->d_name, self) == 0) {
            continue;
        }
        if (WDFB_ProcOwnsFB(e->d_name, data->fake ? 0 : me.st_rdev, me.st_ino, comm, sizeof(comm))) {
            char pid[16];
            SDL_strlcpy(pid, e->d_name, sizeof(pid));  /* e is gone after closedir */
            closedir(d);
            if (WDFB_EnvTrue("SDL_WDFB_FORCE")) {
                fprintf(stderr, "wdfb: warning: %s is in use by pid %s (%s); sharing it (SDL_WDFB_FORCE)\n",
                        data->device, pid, comm);
                return 0;
            }
            return SDL_SetError("wdfb: %s is in use by pid %s (%s); quit it first, or SDL_WDFB_FORCE=1",
                                data->device, pid, comm);
        }
    }
    closedir(d);
    return 0;
}

/* --- init / quit --------------------------------------------------------- */

static int WDFB_OpenFB(WDFB_Data *data)
{
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    struct stat st;

    SDL_strlcpy(data->device, WDFB_DevicePath(), sizeof(data->device));
    data->fd = open(data->device, O_RDWR | O_CLOEXEC);
    if (data->fd < 0) {
        return SDL_SetError("wdfb: %s: %s", data->device, strerror(errno));
    }
    if (fstat(data->fd, &st) == 0 && S_ISREG(st.st_mode)) {
        data->fake = SDL_TRUE;
        data->xres = WDFB_W;
        data->yres = WDFB_H;
        data->pitch = WDFB_W;
        data->maplen = (size_t)WDFB_W * WDFB_H;
        if ((size_t)st.st_size < data->maplen) {
            return SDL_SetError("wdfb: fake frame buffer %s is smaller than %d bytes", data->device,
                                WDFB_W * WDFB_H);
        }
        fprintf(stderr, "wdfb: %s is a file: fake 640x480 frame buffer (test)\n", data->device);
    } else {
        SDL_zero(fix);
        SDL_zero(var);
        if (ioctl(data->fd, FBIOGET_FSCREENINFO, &fix) < 0 || ioctl(data->fd, FBIOGET_VSCREENINFO, &var) < 0) {
            return SDL_SetError("wdfb: %s is not a frame buffer", data->device);
        }
        if (var.bits_per_pixel != 8 || fix.type != FB_TYPE_PACKED_PIXELS ||
            fix.visual != FB_VISUAL_PSEUDOCOLOR) {
            return SDL_SetError("wdfb: %s is not 8 bpp packed pseudocolour", data->device);
        }
        if (SDL_strncmp(fix.id, "WD90C24", 7) != 0) {
            fprintf(stderr, "wdfb: %s is \"%.16s\", not wd90c24fb: no damage ioctl\n", data->device, fix.id);
        }
        data->xres = (int)var.xres;
        data->yres = (int)var.yres;
        data->pitch = (int)fix.line_length;
        data->maplen = fix.smem_len;
        if (data->maplen < (size_t)data->pitch * data->yres) {
            return SDL_SetError("wdfb: %s: smem_len too small", data->device);
        }
    }
    if (WDFB_CheckExclusive(data) < 0) {
        return -1;
    }
    data->fb = (Uint8 *)mmap(NULL, data->maplen, PROT_READ | PROT_WRITE, MAP_SHARED, data->fd, 0);
    if (data->fb == (Uint8 *)MAP_FAILED) {
        data->fb = NULL;
        return SDL_SetError("wdfb: mmap %s: %s", data->device, strerror(errno));
    }
    return 0;
}

static void WDFB_CloseFB(WDFB_Data *data)
{
    if (data->fb) {
        munmap(data->fb, data->maplen);
        data->fb = NULL;
    }
    if (data->fd >= 0) {
        close(data->fd);  /* wd90c24fb: last close resets client_ops */
        data->fd = -1;
    }
}

static int WDFB_VideoInit(_THIS)
{
    WDFB_Data *data = (WDFB_Data *)_this->driverdata;
    SDL_DisplayMode mode;

    data->debug = SDL_atoi(WDFB_Env("SDL_WDFB_DEBUG", "0"));
    data->border = SDL_atoi(WDFB_Env("SDL_WDFB_BORDER", "0")) & 0xff;
    if (WDFB_OpenFB(data) < 0) {
        WDFB_CloseFB(data);
        return -1;
    }

    /* damage ioctl? (wd90c24fb with xaccel=1) */
    data->caps = 0;
    if (!data->fake && !WDFB_EnvTrue("SDL_WDFB_NOOPS")) {
        __u32 caps = 0;
        if (ioctl(data->fd, WDFB_IOC_CAPS, &caps) == 0) {
            data->caps = caps;
        }
    }
    data->use_ops = (data->caps & WDFB_CAP_DAMAGE) ? SDL_TRUE : SDL_FALSE;
    if (!data->use_ops && !data->fake) {
        fprintf(stderr, "wdfb: no WDFB damage ioctl (xaccel=0?): the driver refreshes at 10 Hz\n");
    }
    if (data->debug) {
        fprintf(stderr, "wdfb: %s %dx%d pitch %d caps 0x%x%s\n", data->device, data->xres, data->yres,
                data->pitch, (unsigned)data->caps, data->use_ops ? " (per-rect damage)" : "");
    }

    /* keep the console's palette to give it back */
    if (!data->fake) {
        struct fb_cmap cmap;
        cmap.start = 0;
        cmap.len = 256;
        cmap.red = data->saved_r;
        cmap.green = data->saved_g;
        cmap.blue = data->saved_b;
        cmap.transp = NULL;
        data->have_saved_cmap = ioctl(data->fd, FBIOGETCMAP, &cmap) == 0 ? SDL_TRUE : SDL_FALSE;
    }

    if (WDFB_InitInput(_this) < 0) {
        WDFB_CloseFB(data);
        return -1;
    }

    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_INDEX8;
    mode.w = data->xres;
    mode.h = data->yres;
    if (SDL_AddBasicVideoDisplay(&mode) < 0) {
        WDFB_QuitInput(_this);
        WDFB_CloseFB(data);
        return -1;
    }
    return 0;
}

static void WDFB_VideoQuit(_THIS)
{
    WDFB_Data *data = (WDFB_Data *)_this->driverdata;

    if (data->fb) {
        int y;
        for (y = 0; y < data->yres; y++) {
            SDL_memset(data->fb + y * data->pitch, 0, data->xres);
        }
        WDFB_DamageAll(data);
    }
    if (data->have_saved_cmap) {
        WDFB_PutCmap(data, data->saved_r, data->saved_g, data->saved_b, 256);
    }
    WDFB_QuitInput(_this);  /* back to text mode: fbcon redraws */
    WDFB_CloseFB(data);
}

static SDL_VideoDevice *WDFB_CreateDevice(void)
{
    SDL_VideoDevice *device;
    WDFB_Data *data;

    if (!WDFB_Available()) {
        return NULL;
    }
    device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    data = (WDFB_Data *)SDL_calloc(1, sizeof(WDFB_Data));
    if (!device || !data) {
        SDL_free(device);
        SDL_free(data);
        SDL_OutOfMemory();
        return NULL;
    }
    data->fd = -1;
    data->tty_fd = -1;
    device->driverdata = data;

    device->VideoInit = WDFB_VideoInit;
    device->VideoQuit = WDFB_VideoQuit;
    device->GetDisplayModes = WDFB_GetDisplayModes;
    device->SetDisplayMode = WDFB_SetDisplayMode;
    device->PumpEvents = WDFB_PumpEvents;
    device->CreateSDLWindow = WDFB_CreateWindow;
    device->DestroyWindow = WDFB_DestroyWindow;
    device->CreateWindowFramebuffer = WDFB_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = WDFB_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = WDFB_DestroyWindowFramebuffer;
    device->free = WDFB_DeleteDevice;
    return device;
}

VideoBootStrap WDFB_bootstrap = {
    WDFB_DRIVER_NAME, "wd90c24fb (ThinkPad 755C) frame buffer",
    WDFB_CreateDevice,
    NULL /* no ShowMessageBox implementation */
};

#endif /* SDL_VIDEO_DRIVER_WDFB */

/* vi: set ts=4 sw=4 expandtab: */
