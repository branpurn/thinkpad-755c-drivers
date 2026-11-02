/*
 * doomgeneric_wdfb.c: doomgeneric platform for wd90c24fb (IBM ThinkPad
 * 755C, WD90C24, 640x480, 8 bits per pixel, palette), part of
 * thinkpad-755c-drivers. No SDL, no X.
 *
 * Copyright (C) 2026 the thinkpad-755c-drivers authors. Frame buffer and
 * console code adapted from sdl/wdfb (zlib licence); as part of doomgeneric
 * this file is distributed under the GNU GPL, version 2 or later.
 *
 * mmap() of /dev/fb0 gives the driver's frame shadow in system RAM. Doom
 * draws its 320x200 8-bit frame straight into it (build with
 * -DDG_DIRECT -DSCREENPITCH=640, see patches/): DG_DrawFrame only reports
 * the frame with one WDFB_OP_DAMAGE, which also stops the driver's 10 Hz
 * poll. With DOOM_WDFB_SCALE=2, Doom draws into a buffer that is doubled
 * into the shadow (640x400). The palette goes through FBIOPUTCMAP.
 *
 * Keyboard: evdev (/dev/input/event*, grabbed), else the console in
 * K_MEDIUMRAW. The console goes to KD_GRAPHICS and is given back on exit
 * and on fatal signals. Ctrl+Alt+Backspace quits at once.
 *
 * Environment:
 *   DOOM_WDFB_DEVICE  frame buffer (default /dev/fb0). A regular file of
 *                     640*480 bytes is a fake frame buffer for tests; the
 *                     palette then goes to <file>.pal (256 x RGB).
 *   DOOM_WDFB_SCALE   1 (default: 320x200 centred) or 2 (640x400)
 *   DOOM_WDFB_FORCE   1 = use the frame buffer even if another process
 *                     (X) has it open
 *   DOOM_WDFB_INPUT   auto (default), evdev, console or none
 *   DOOM_WDFB_KBD     one evdev device instead of scanning
 *   DOOM_WDFB_TTY     console to drive (default /dev/tty, else /dev/tty0)
 *   DOOM_WDFB_DEBUG   1 = frames per second and flush time on stderr
 * Sound: none yet (the CS4248's OSS /dev/dsp from snd-cs4248.tcz is the
 * way to add it).
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>

/* Linux and doomkeys.h share some KEY_ names: keep the Linux codes as LK_ */
enum {
    LK_ENTER = KEY_ENTER, LK_TAB = KEY_TAB, LK_BACKSPACE = KEY_BACKSPACE, LK_PAUSE = KEY_PAUSE,
    LK_CAPSLOCK = KEY_CAPSLOCK, LK_HOME = KEY_HOME, LK_END = KEY_END, LK_F1 = KEY_F1,
    LK_F10 = KEY_F10, LK_F11 = KEY_F11, LK_F12 = KEY_F12,
};
#undef KEY_ENTER
#undef KEY_TAB
#undef KEY_BACKSPACE
#undef KEY_PAUSE
#undef KEY_CAPSLOCK
#undef KEY_NUMLOCK
#undef KEY_HOME
#undef KEY_END
#undef KEY_MINUS
#undef KEY_F1
#undef KEY_F2
#undef KEY_F3
#undef KEY_F4
#undef KEY_F5
#undef KEY_F6
#undef KEY_F7
#undef KEY_F8
#undef KEY_F9
#undef KEY_F10
#undef KEY_F11
#undef KEY_F12

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_video.h"
#include "wd90c24fb_ioctl.h"

#ifndef K_OFF
#define K_OFF 0x04
#endif

#define FB_W 640
#define FB_H 480
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static struct {
    char device[128];
    int fd, fake, use_ops;
    unsigned char *fb;          /* mmap'd shadow */
    size_t maplen;
    int xres, yres, pitch;
    int scale, ox, oy;          /* Doom's frame on the screen */
    unsigned char *back;        /* Doom draws here unless direct */
    __u16 saved_r[256], saved_g[256], saved_b[256];
    int have_saved_cmap;
    int kbd_fd[4], nkbd, raw_console;
    int raw_pending, raw_code, raw_release;
    int ctrl, alt;
    int debug;
    unsigned frames, last_ms, flush_us;
} wd = { .fd = -1 };

/* for atexit() and signal handlers */
static volatile int con_fd = -1;
static int old_kd = KD_TEXT, old_kb = K_XLATE, have_tio;
static struct termios old_tio;

static const char *env(const char *name, const char *def)
{
    const char *v = getenv(name);
    return (v && *v) ? v : def;
}

static void die(const char *fmt, const char *arg)
{
    fprintf(stderr, "doom-wdfb: ");
    fprintf(stderr, fmt, arg, strerror(errno));
    fprintf(stderr, "\n");
    exit(1);
}

/* --- frame buffer ---------------------------------------------------------- */

static void damage(int x, int y, int w, int h)
{
    struct wdfb_op op = { .op = WDFB_OP_DAMAGE, .x = x, .y = y, .w = w, .h = h };
    struct wdfb_ops req = { .n = 1, .flags = 0, .ops = (__u64)(unsigned long)&op };

    if (wd.use_ops && ioctl(wd.fd, WDFB_IOC_OPS, &req) < 0) {
        fprintf(stderr, "doom-wdfb: WDFB_IOC_OPS: %s; using the driver's 10 Hz refresh\n",
                strerror(errno));
        wd.use_ops = 0;
    }
}

static void put_cmap(__u16 *r, __u16 *g, __u16 *b)
{
    struct fb_cmap cmap = { .start = 0, .len = 256, .red = r, .green = g, .blue = b };

    if (wd.fake) {
        unsigned char rgb[256 * 3];
        char path[sizeof(wd.device) + 8];
        int i, fd;

        for (i = 0; i < 256; i++) {
            rgb[i * 3] = r[i] >> 8;
            rgb[i * 3 + 1] = g[i] >> 8;
            rgb[i * 3 + 2] = b[i] >> 8;
        }
        snprintf(path, sizeof(path), "%s.pal", wd.device);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) {
            if (write(fd, rgb, sizeof(rgb)) != (ssize_t)sizeof(rgb))
                fprintf(stderr, "doom-wdfb: %s: short write\n", path);
            close(fd);
        }
        return;
    }
    ioctl(wd.fd, FBIOPUTCMAP, &cmap);
}

/* refuse a frame buffer someone else (X) has open */
static void check_exclusive(void)
{
    char path[300], comm[32] = "?";
    struct stat me, st;
    struct dirent *p, *f;
    DIR *proc, *fds;
    int self = getpid(), n, fd;

    if (fstat(wd.fd, &me) < 0 || !(proc = opendir("/proc")))
        return;
    while ((p = readdir(proc)) != NULL) {
        if (p->d_name[0] < '1' || p->d_name[0] > '9' || atoi(p->d_name) == self)
            continue;
        snprintf(path, sizeof(path), "/proc/%s/fd", p->d_name);
        if (!(fds = opendir(path)))
            continue;
        while ((f = readdir(fds)) != NULL) {
            snprintf(path, sizeof(path), "/proc/%s/fd/%s", p->d_name, f->d_name);
            if (f->d_name[0] == '.' || stat(path, &st) < 0)
                continue;
            if (wd.fake ? st.st_ino == me.st_ino && st.st_dev == me.st_dev
                        : S_ISCHR(st.st_mode) && st.st_rdev == me.st_rdev)
                break;
        }
        closedir(fds);
        if (!f)
            continue;
        snprintf(path, sizeof(path), "/proc/%s/comm", p->d_name);
        if ((fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0) {
            n = read(fd, comm, sizeof(comm) - 1);
            comm[n > 0 ? n - 1 : 0] = '\0';
            close(fd);
        }
        if (atoi(env("DOOM_WDFB_FORCE", "0"))) {
            fprintf(stderr, "doom-wdfb: warning: %s is in use by pid %s (%s); sharing it\n",
                    wd.device, p->d_name, comm);
            break;
        }
        fprintf(stderr, "doom-wdfb: %s is in use by pid %s (%s); quit it first, "
                "or DOOM_WDFB_FORCE=1\n", wd.device, p->d_name, comm);
        exit(1);
    }
    closedir(proc);
}

static void open_fb(void)
{
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    struct stat st;
    __u32 caps = 0;

    snprintf(wd.device, sizeof(wd.device), "%s", env("DOOM_WDFB_DEVICE", "/dev/fb0"));
    if ((wd.fd = open(wd.device, O_RDWR | O_CLOEXEC)) < 0)
        die("%s: %s", wd.device);
    if (fstat(wd.fd, &st) == 0 && S_ISREG(st.st_mode)) {
        wd.fake = 1;
        wd.xres = wd.pitch = FB_W;
        wd.yres = FB_H;
        wd.maplen = FB_W * FB_H;
        if ((size_t)st.st_size < wd.maplen) {
            errno = EINVAL;
            die("fake frame buffer %s: %s (need 640*480 bytes)", wd.device);
        }
    } else {
        if (ioctl(wd.fd, FBIOGET_FSCREENINFO, &fix) < 0 || ioctl(wd.fd, FBIOGET_VSCREENINFO, &var) < 0)
            die("%s is not a frame buffer: %s", wd.device);
        if (var.bits_per_pixel != 8 || fix.visual != FB_VISUAL_PSEUDOCOLOR) {
            errno = EINVAL;
            die("%s is not 8 bpp pseudocolour: %s", wd.device);
        }
        wd.xres = var.xres;
        wd.yres = var.yres;
        wd.pitch = fix.line_length;
        wd.maplen = fix.smem_len;
        if (ioctl(wd.fd, WDFB_IOC_CAPS, &caps) == 0 && (caps & WDFB_CAP_DAMAGE))
            wd.use_ops = 1;
        else
            fprintf(stderr, "doom-wdfb: no damage ioctl (not wd90c24fb, or xaccel=0): "
                    "the driver refreshes at 10 Hz\n");
        struct fb_cmap cmap = { .start = 0, .len = 256,
                                .red = wd.saved_r, .green = wd.saved_g, .blue = wd.saved_b };
        wd.have_saved_cmap = ioctl(wd.fd, FBIOGETCMAP, &cmap) == 0;
    }
    if (wd.xres < SCREENWIDTH || wd.yres < SCREENHEIGHT) {
        errno = EINVAL;
        die("%s: %s (smaller than 320x200)", wd.device);
    }
    check_exclusive();
    wd.fb = mmap(NULL, wd.maplen, PROT_READ | PROT_WRITE, MAP_SHARED, wd.fd, 0);
    if (wd.fb == MAP_FAILED)
        die("mmap %s: %s", wd.device);
}

static void clear_screen(void)
{
    int y;

    for (y = 0; y < wd.yres; y++)
        memset(wd.fb + y * wd.pitch, 0, wd.xres);
    damage(0, 0, wd.xres, wd.yres);
}

/* --- console and keyboard -------------------------------------------------- */

static void restore_console(void)
{
    int fd = con_fd;

    if (fd < 0)
        return;
    con_fd = -1;
    ioctl(fd, KDSKBMODE, old_kb);
    tcflush(fd, TCIFLUSH);      /* no raw key bytes for the shell */
    if (have_tio)
        tcsetattr(fd, TCSAFLUSH, &old_tio);
    ioctl(fd, KDSETMODE, old_kd); /* fbcon redraws the text */
}

/* exit(), I_Quit and I_Error end here: blank, old palette, text console */
static void shutdown_wdfb(void)
{
    int i;

    if (wd.fb && wd.fb != MAP_FAILED) {
        clear_screen();
        if (wd.have_saved_cmap)
            put_cmap(wd.saved_r, wd.saved_g, wd.saved_b);
    }
    for (i = 0; i < wd.nkbd; i++)
        ioctl(wd.kbd_fd[i], EVIOCGRAB, 0);
    restore_console();
}

static void fatal_signal(int sig)
{
    shutdown_wdfb();
    signal(sig, SIG_DFL);
    raise(sig);
}

static int open_console(void)
{
    const char *want = getenv("DOOM_WDFB_TTY");
    const char *paths[2] = { (want && *want) ? want : "/dev/tty", (want && *want) ? NULL : "/dev/tty0" };
    int i, fd, mode;

    for (i = 0; i < 2 && paths[i]; i++) {
        if ((fd = open(paths[i], O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0)
            continue;
        if (ioctl(fd, KDGKBMODE, &mode) == 0)
            return fd;
        close(fd);
    }
    return -1;
}

static int is_keyboard(int fd)
{
    unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1];
#define BIT(b) ((bits[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long)))) & 1)

    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0)
        return 0;
    return BIT(KEY_A) && BIT(KEY_Z) && BIT(KEY_SPACE);
#undef BIT
}

static int add_kbd(const char *path, int must)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

    if (fd < 0) {
        if (must)
            fprintf(stderr, "doom-wdfb: %s: %s\n", path, strerror(errno));
        return 0;
    }
    if ((!must && !is_keyboard(fd)) || wd.nkbd >= (int)ARRAY_SIZE(wd.kbd_fd)) {
        close(fd);
        return 0;
    }
    wd.kbd_fd[wd.nkbd++] = fd;
    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        fprintf(stderr, "doom-wdfb: %s: cannot grab (%s)\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static void init_input(void)
{
    static const int sigs[] = { SIGHUP, SIGINT, SIGTERM, SIGQUIT, SIGILL, SIGABRT, SIGBUS, SIGFPE, SIGSEGV };
    const char *input = env("DOOM_WDFB_INPUT", "auto");
    const char *kbd = getenv("DOOM_WDFB_KBD");
    int evdev = !strcmp(input, "auto") || !strcmp(input, "evdev");
    int console = !strcmp(input, "auto") || !strcmp(input, "console");
    int fd, i, grab_failed = 0;
    struct sigaction sa;
    char path[32];

    atexit(shutdown_wdfb);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fatal_signal;
    sa.sa_flags = SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < (int)ARRAY_SIZE(sigs); i++)
        sigaction(sigs[i], &sa, NULL);

    if ((fd = open_console()) < 0) {
        fprintf(stderr, "doom-wdfb: not on a VT console: console text may draw over the game\n");
    } else {
        ioctl(fd, KDGETMODE, &old_kd);
        ioctl(fd, KDGKBMODE, &old_kb);
        have_tio = tcgetattr(fd, &old_tio) == 0;
        con_fd = fd;
        if (ioctl(fd, KDSETMODE, KD_GRAPHICS) < 0)
            fprintf(stderr, "doom-wdfb: KDSETMODE KD_GRAPHICS: %s\n", strerror(errno));
    }

    if (evdev && kbd && *kbd) {
        grab_failed |= add_kbd(kbd, 1);
    } else if (evdev) {
        for (i = 0; i < 32; i++) {
            snprintf(path, sizeof(path), "/dev/input/event%d", i);
            grab_failed |= add_kbd(path, 0);
        }
    }

    if (fd >= 0 && wd.nkbd == 0 && console) {
        /* keys from the console: Linux key codes, bit 7 = release */
        if (have_tio) {
            struct termios tio = old_tio;

            cfmakeraw(&tio);
            tio.c_cc[VMIN] = 0;
            tio.c_cc[VTIME] = 0;
            tcsetattr(fd, TCSAFLUSH, &tio);
        }
        if (ioctl(fd, KDSKBMODE, K_MEDIUMRAW) == 0) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            wd.raw_console = 1;
        } else {
            fprintf(stderr, "doom-wdfb: KDSKBMODE K_MEDIUMRAW: %s\n", strerror(errno));
        }
    } else if (fd >= 0 && grab_failed) {
        ioctl(fd, KDSKBMODE, K_OFF); /* keep ungrabbed keys off the console */
    }
    if (wd.nkbd == 0 && !wd.raw_console && strcmp(input, "none"))
        fprintf(stderr, "doom-wdfb: no keyboard (no evdev keyboard, no VT console)\n");
    if (wd.debug)
        fprintf(stderr, "doom-wdfb: keyboard: %d evdev%s\n", wd.nkbd,
                wd.raw_console ? ", console K_MEDIUMRAW" : "");
}

/* Linux key code -> Doom key (0: ignored) */
static unsigned char doom_key(int code)
{
    static const char ascii[] = "\0\0" "1234567890-=\0\0" "qwertyuiop[]\0\0" "asdfghjkl;'`\0\\" "zxcvbnm,./";

    switch (code) {
    case KEY_ESC:        return KEY_ESCAPE;
    case LK_ENTER:
    case KEY_KPENTER:    return KEY_ENTER;
    case LK_TAB:        return KEY_TAB;
    case LK_BACKSPACE:   return KEY_BACKSPACE;
    case KEY_SPACE:      return KEY_USE;
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:  return KEY_FIRE;
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT: return KEY_RSHIFT;
    case KEY_LEFTALT:
    case KEY_RIGHTALT:   return KEY_RALT;
    case KEY_UP:         return KEY_UPARROW;
    case KEY_DOWN:       return KEY_DOWNARROW;
    case KEY_LEFT:       return KEY_LEFTARROW;
    case KEY_RIGHT:      return KEY_RIGHTARROW;
    case LK_HOME:       return KEY_HOME;
    case LK_END:         return KEY_END;
    case KEY_PAGEUP:     return KEY_PGUP;
    case KEY_PAGEDOWN:   return KEY_PGDN;
    case KEY_INSERT:     return KEY_INS;
    case KEY_DELETE:     return KEY_DEL;
    case LK_PAUSE:      return KEY_PAUSE;
    case LK_CAPSLOCK:   return KEY_CAPSLOCK;
    case KEY_KPMINUS:    return KEY_MINUS;
    case KEY_KPPLUS:     return KEY_EQUALS;
    case LK_F11:        return KEY_F11;
    case LK_F12:        return KEY_F12;
    }
    if (code >= LK_F1 && code <= LK_F10)
        return 0x80 + code;     /* doomkeys.h: 0x80 + AT scan code */
    if (code < (int)sizeof(ascii) - 1)
        return ascii[code];
    return 0;
}

#define KEYQ 64
static unsigned short keyq[KEYQ];
static unsigned keyq_r, keyq_w;

static void key_event(int code, int pressed)
{
    unsigned char k;

    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL)
        wd.ctrl = pressed;
    if (code == KEY_LEFTALT || code == KEY_RIGHTALT)
        wd.alt = pressed;
    if (pressed && code == LK_BACKSPACE && wd.ctrl && wd.alt) {
        fprintf(stderr, "doom-wdfb: Ctrl+Alt+Backspace: exiting\n");
        exit(1);
    }
    if ((k = doom_key(code)) && keyq_w - keyq_r < KEYQ)
        keyq[keyq_w++ % KEYQ] = (pressed << 8) | k;
}

static void poll_input(void)
{
    struct input_event ev[16];
    unsigned char buf[64];
    int i, k;
    ssize_t n;

    for (i = 0; i < wd.nkbd; i++) {
        while ((n = read(wd.kbd_fd[i], ev, sizeof(ev))) > 0)
            for (k = 0; k < n / (ssize_t)sizeof(ev[0]); k++)
                if (ev[k].type == EV_KEY && ev[k].value != 2) /* Doom repeats itself */
                    key_event(ev[k].code, ev[k].value);
    }
    if (!wd.raw_console)
        return;
    while ((n = read(con_fd, buf, sizeof(buf))) > 0) {
        for (k = 0; k < n; k++) {
            unsigned char b = buf[k];

            /* K_MEDIUMRAW: code | 0x80 on release; codes > 127 are 0 (or
               0x80), then 7 high bits | 0x80, then 7 low bits | 0x80 */
            if (wd.raw_pending == 2) {
                wd.raw_code = (b & 0x7f) << 7;
                wd.raw_pending = 1;
            } else if (wd.raw_pending == 1) {
                wd.raw_pending = 0;
                key_event(wd.raw_code | (b & 0x7f), !wd.raw_release);
            } else if ((b & 0x7f) == 0) {
                wd.raw_release = b & 0x80;
                wd.raw_pending = 2;
            } else {
                key_event(b & 0x7f, !(b & 0x80));
            }
        }
    }
}

/* --- doomgeneric ----------------------------------------------------------- */

void DG_Init(void)
{
    /* the frame buffer and console are taken in DG_VideoBuffer, so start-up
       messages and errors (no IWAD, ...) stay on the text console */
    wd.debug = atoi(env("DOOM_WDFB_DEBUG", "0"));
}

uint8_t *DG_VideoBuffer(void)
{
    open_fb();
    wd.scale = atoi(env("DOOM_WDFB_SCALE", "1")) == 2 && wd.xres >= 2 * SCREENWIDTH
               && wd.yres >= 2 * SCREENHEIGHT ? 2 : 1;
    wd.ox = (wd.xres - SCREENWIDTH * wd.scale) / 2;
    wd.oy = (wd.yres - SCREENHEIGHT * wd.scale) / 2;
    init_input();
    clear_screen();             /* the border stays black from now on */
    if (wd.debug)
        fprintf(stderr, "doom-wdfb: %s %dx%d pitch %d, %dx%d at %d,%d%s%s\n", wd.device,
                wd.xres, wd.yres, wd.pitch, SCREENWIDTH * wd.scale, SCREENHEIGHT * wd.scale,
                wd.ox, wd.oy, wd.scale == 1 && wd.pitch == SCREENPITCH ? " (direct)" : "",
                wd.use_ops ? ", damage ioctl" : "");
    if (wd.scale == 1 && wd.pitch == SCREENPITCH)
        return wd.fb + wd.oy * wd.pitch + wd.ox;
    if (!(wd.back = calloc(SCREENHEIGHT, SCREENPITCH)))
        die("%s%s", "out of memory");
    return wd.back;
}

static unsigned now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000u + ts.tv_nsec / 1000000;
}

static void present(void)
{
    unsigned char *dst = wd.fb + wd.oy * wd.pitch + wd.ox;
    const unsigned char *src = wd.back;
    int x, y;

    if (!src) {
        /* Doom drew straight into the shadow */
    } else if (wd.scale == 1) {
        for (y = 0; y < SCREENHEIGHT; y++, src += SCREENPITCH, dst += wd.pitch)
            memcpy(dst, src, SCREENWIDTH);
    } else {
        for (y = 0; y < SCREENHEIGHT; y++, src += SCREENPITCH, dst += 2 * wd.pitch) {
            unsigned *d = (unsigned *)dst; /* wd.ox is a multiple of 4 */
            for (x = 0; x < SCREENWIDTH; x += 2) {
                unsigned a = src[x], b = src[x + 1];
                *d++ = a | a << 8 | b << 16 | b << 24;
            }
            memcpy(dst + wd.pitch, dst, 2 * SCREENWIDTH);
        }
    }
    damage(wd.ox, wd.oy, SCREENWIDTH * wd.scale, SCREENHEIGHT * wd.scale);
}

void DG_DrawFrame(void)
{
    unsigned t0 = 0, t;

    if (palette_changed) {
        __u16 r[256], g[256], b[256];
        int i;

        for (i = 0; i < 256; i++) {
            r[i] = colors[i].r * 0x101;
            g[i] = colors[i].g * 0x101;
            b[i] = colors[i].b * 0x101;
        }
        put_cmap(r, g, b);
        palette_changed = false;
    }
    if (wd.debug)
        t0 = now_ms();
    present();
    if (!wd.debug)
        return;
    t = now_ms();
    wd.flush_us += (t - t0) * 1000;
    wd.frames++;
    if (!wd.last_ms) {
        wd.last_ms = t;
    } else if (t - wd.last_ms >= 2000) {
        fprintf(stderr, "doom-wdfb: %u.%u fps, present %u ms/frame\n",
                wd.frames * 1000 / (t - wd.last_ms), wd.frames * 10000 / (t - wd.last_ms) % 10,
                wd.flush_us / 1000 / wd.frames);
        wd.frames = wd.flush_us = 0;
        wd.last_ms = t;
    }
}

void DG_SleepMs(uint32_t ms)
{
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void)
{
    return now_ms();
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (keyq_r == keyq_w)
        poll_input();
    if (keyq_r == keyq_w)
        return 0;
    *pressed = keyq[keyq_r % KEYQ] >> 8;
    *key = keyq[keyq_r % KEYQ] & 0xff;
    keyq_r++;
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

int main(int argc, char **argv)
{
    doomgeneric_Create(argc, argv);
    for (;;)
        doomgeneric_Tick();
}
