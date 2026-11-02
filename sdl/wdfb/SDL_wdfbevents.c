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
 * wdfb: console and keyboard for the wd90c24fb video driver. Altered
 * source: not part of upstream SDL.
 *
 * The console is put in KD_GRAPHICS (fbcon stops drawing) and given back
 * in KD_TEXT on SDL_Quit, at exit() and on fatal signals. Keys come from
 * evdev keyboards (/dev/input/event*, grabbed: the console sees nothing,
 * and a crash releases the grab) or, without evdev (it is a module on the
 * 755C kernel), from the console itself in K_MEDIUMRAW.
 *
 * Environment:
 *   SDL_WDFB_TTY    console to drive (default /dev/tty, else /dev/tty0)
 *   SDL_WDFB_INPUT  auto (default), evdev, console or none
 *   SDL_WDFB_KBD    one evdev device to use instead of scanning
 *   SDL_WDFB_NOGRAPHICS  1 = leave the console in KD_TEXT
 * Ctrl+Alt+Backspace: quit (SDL_QUIT); a second time: restore and exit.
 */

#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_WDFB

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/kd.h>

#include "../SDL_sysvideo.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_scancode_tables_c.h"
#include "SDL_wdfbvideo.h"

#ifndef K_OFF
#define K_OFF 0x04
#endif

/* for the signal handler and atexit(): plain statics */
static volatile int wdfb_con_fd = -1;
static int wdfb_old_kd = KD_TEXT, wdfb_old_kb = K_XLATE;
static struct termios wdfb_old_tio;
static int wdfb_have_tio;
static int wdfb_atexit_done;
static const int wdfb_sigs[] = { SIGHUP, SIGQUIT, SIGILL, SIGABRT, SIGBUS, SIGFPE, SIGSEGV };
static struct sigaction wdfb_old_sa[SDL_arraysize(wdfb_sigs)];
static int wdfb_sigs_set;
static int wdfb_grab_failed;

void WDFB_RestoreConsole(void)
{
    int fd = wdfb_con_fd;

    if (fd < 0) {
        return;
    }
    wdfb_con_fd = -1;
    ioctl(fd, KDSKBMODE, wdfb_old_kb);
    tcflush(fd, TCIFLUSH); /* no raw key bytes for the shell */
    if (wdfb_have_tio) {
        tcsetattr(fd, TCSAFLUSH, &wdfb_old_tio);
    }
    ioctl(fd, KDSETMODE, wdfb_old_kd);
}

static void WDFB_FatalSignal(int sig)
{
    WDFB_RestoreConsole();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void WDFB_AtExit(void)
{
    WDFB_RestoreConsole();
}

static void WDFB_InstallSignals(void)
{
    struct sigaction sa;
    unsigned i;

    if (wdfb_sigs_set) {
        return;
    }
    SDL_zero(sa);
    sa.sa_handler = WDFB_FatalSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    for (i = 0; i < SDL_arraysize(wdfb_sigs); i++) {
        sigaction(wdfb_sigs[i], &sa, &wdfb_old_sa[i]);
    }
    wdfb_sigs_set = 1;
    if (!wdfb_atexit_done) {
        atexit(WDFB_AtExit);
        wdfb_atexit_done = 1;
    }
}

static void WDFB_RemoveSignals(void)
{
    unsigned i;

    if (!wdfb_sigs_set) {
        return;
    }
    for (i = 0; i < SDL_arraysize(wdfb_sigs); i++) {
        sigaction(wdfb_sigs[i], &wdfb_old_sa[i], NULL);
    }
    wdfb_sigs_set = 0;
}

/* a VT we may switch to graphics: the controlling tty, else the active one */
static int WDFB_OpenConsole(void)
{
    const char *want = SDL_getenv("SDL_WDFB_TTY");
    const char *paths[2];
    int i, fd, mode;

    paths[0] = (want && *want) ? want : "/dev/tty";
    paths[1] = (want && *want) ? NULL : "/dev/tty0";
    for (i = 0; i < 2 && paths[i]; i++) {
        fd = open(paths[i], O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        if (ioctl(fd, KDGKBMODE, &mode) == 0) {
            return fd;
        }
        close(fd);
    }
    return -1;
}

static SDL_bool WDFB_TestBit(const unsigned long *bits, int bit)
{
    return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1 ? SDL_TRUE : SDL_FALSE;
}

static SDL_bool WDFB_IsKeyboard(int fd)
{
    unsigned long bits[(KEY_MAX + 1) / (8 * sizeof(long)) + 1];

    SDL_zero(bits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) {
        return SDL_FALSE;
    }
    return WDFB_TestBit(bits, KEY_A) && WDFB_TestBit(bits, KEY_Z) && WDFB_TestBit(bits, KEY_SPACE);
}

static void WDFB_AddKbd(WDFB_Data *data, const char *path, SDL_bool must)
{
    int fd;

    if (data->nkbd >= (int)SDL_arraysize(data->kbd_fd)) {
        return;
    }
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (must) {
            fprintf(stderr, "wdfb: %s: %s\n", path, strerror(errno));
        }
        return;
    }
    if (!must && !WDFB_IsKeyboard(fd)) {
        close(fd);
        return;
    }
    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        fprintf(stderr, "wdfb: %s: cannot grab (%s); keys may also reach the console\n", path, strerror(errno));
        wdfb_grab_failed = 1;
    }
    if (data->debug) {
        fprintf(stderr, "wdfb: keyboard %s\n", path);
    }
    data->kbd_fd[data->nkbd++] = fd;
}

int WDFB_InitInput(_THIS)
{
    WDFB_Data *data = (WDFB_Data *)_this->driverdata;
    const char *input = SDL_getenv("SDL_WDFB_INPUT");
    const char *kbd = SDL_getenv("SDL_WDFB_KBD");
    SDL_bool want_evdev, want_console;
    char path[32];
    int i;

    if (!input || !*input) {
        input = "auto";
    }
    want_evdev = (SDL_strcasecmp(input, "auto") == 0 || SDL_strcasecmp(input, "evdev") == 0) ? SDL_TRUE : SDL_FALSE;
    want_console = (SDL_strcasecmp(input, "auto") == 0 || SDL_strcasecmp(input, "console") == 0) ? SDL_TRUE : SDL_FALSE;

    data->nkbd = 0;
    data->raw_console = SDL_FALSE;
    data->tty_fd = WDFB_OpenConsole();
    if (data->tty_fd < 0) {
        fprintf(stderr, "wdfb: not on a VT console (and no /dev/tty0): console text may draw over the game\n");
    } else {
        int kd = KD_TEXT, kb = K_XLATE;

        ioctl(data->tty_fd, KDGETMODE, &kd);
        ioctl(data->tty_fd, KDGKBMODE, &kb);
        data->old_kd_mode = kd;
        data->old_kb_mode = kb;
        wdfb_old_kd = kd;
        wdfb_old_kb = kb;
        wdfb_have_tio = tcgetattr(data->tty_fd, &wdfb_old_tio) == 0;
        wdfb_con_fd = data->tty_fd;
        WDFB_InstallSignals();
        if (!SDL_getenv("SDL_WDFB_NOGRAPHICS") || *SDL_getenv("SDL_WDFB_NOGRAPHICS") == '0') {
            if (ioctl(data->tty_fd, KDSETMODE, KD_GRAPHICS) < 0) {
                fprintf(stderr, "wdfb: KDSETMODE KD_GRAPHICS: %s\n", strerror(errno));
            }
        }
        data->tty_saved = SDL_TRUE;
    }

    if (want_evdev) {
        if (kbd && *kbd) {
            WDFB_AddKbd(data, kbd, SDL_TRUE);
        } else {
            for (i = 0; i < 32; i++) {
                SDL_snprintf(path, sizeof(path), "/dev/input/event%d", i);
                WDFB_AddKbd(data, path, SDL_FALSE);
            }
        }
    }

    if (data->tty_fd >= 0) {
        struct termios tio;

        if (data->nkbd == 0 && want_console) {
            /* keys from the console itself: Linux key codes, bit 7 = release */
            if (wdfb_have_tio) {
                tio = wdfb_old_tio;
                tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
                tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
                tio.c_cflag &= ~(CSIZE | PARENB);
                tio.c_cflag |= CS8;
                tio.c_cc[VMIN] = 0;
                tio.c_cc[VTIME] = 0;
                tcsetattr(data->tty_fd, TCSAFLUSH, &tio);
            }
            if (ioctl(data->tty_fd, KDSKBMODE, K_MEDIUMRAW) == 0) {
                fcntl(data->tty_fd, F_SETFL, fcntl(data->tty_fd, F_GETFL) | O_NONBLOCK);
                data->raw_console = SDL_TRUE;
                if (data->debug) {
                    fprintf(stderr, "wdfb: keyboard: console (K_MEDIUMRAW)\n");
                }
            } else {
                fprintf(stderr, "wdfb: KDSKBMODE K_MEDIUMRAW: %s\n", strerror(errno));
            }
        } else if (data->nkbd > 0 && wdfb_grab_failed) {
            /* an evdev keyboard we could not grab: keep its keys off the
               console (a grab is better: a crash cannot leave it K_OFF) */
            ioctl(data->tty_fd, KDSKBMODE, K_OFF);
        }
    }
    if (data->nkbd == 0 && !data->raw_console && SDL_strcasecmp(input, "none") != 0) {
        fprintf(stderr, "wdfb: no keyboard (no evdev keyboard, no VT console)\n");
    }
    return 0;
}

void WDFB_QuitInput(_THIS)
{
    WDFB_Data *data = (WDFB_Data *)_this->driverdata;
    int i;

    for (i = 0; i < data->nkbd; i++) {
        ioctl(data->kbd_fd[i], EVIOCGRAB, 0);
        close(data->kbd_fd[i]);
    }
    data->nkbd = 0;
    if (data->tty_fd >= 0) {
        if (data->raw_console) {
            fcntl(data->tty_fd, F_SETFL, fcntl(data->tty_fd, F_GETFL) & ~O_NONBLOCK);
        }
        WDFB_RestoreConsole();
        close(data->tty_fd);
        data->tty_fd = -1;
    }
    data->raw_console = SDL_FALSE;
    data->tty_saved = SDL_FALSE;
    WDFB_RemoveSignals();
}

/* US layout, for SDL_TEXTINPUT: Linux key codes 2..57 */
static const char wdfb_us_lower[] = "\0\0" "1234567890-=\0\0" "qwertyuiop[]\0\0" "asdfghjkl;'`\0\\" "zxcvbnm,./\0*\0 ";
static const char wdfb_us_upper[] = "\0\0" "!@#$%^&*()_+\0\0" "QWERTYUIOP{}\0\0" "ASDFGHJKL:\"~\0|" "ZXCVBNM<>?\0*\0 ";

static void WDFB_Key(WDFB_Data *data, int code, SDL_bool pressed)
{
    SDL_Scancode sc = SDL_GetScancodeFromTable(SDL_SCANCODE_TABLE_LINUX, code);
    SDL_Keymod mod;

    if (sc == SDL_SCANCODE_UNKNOWN) {
        return;
    }
    SDL_SendKeyboardKey(pressed ? SDL_PRESSED : SDL_RELEASED, sc);
    if (!pressed) {
        return;
    }
    mod = SDL_GetModState();
    if (sc == SDL_SCANCODE_BACKSPACE && (mod & KMOD_CTRL) && (mod & KMOD_ALT)) {
        if (++data->cab_count >= 2) {
            fprintf(stderr, "wdfb: Ctrl+Alt+Backspace twice: exiting\n");
            WDFB_RestoreConsole();
            _exit(1);
        }
        SDL_SendQuit();
        return;
    }
    if (SDL_IsTextInputActive() && !(mod & (KMOD_CTRL | KMOD_ALT)) &&
        code < (int)sizeof(wdfb_us_lower) - 1) {
        SDL_bool upper = (mod & KMOD_SHIFT) ? SDL_TRUE : SDL_FALSE;
        char text[2];

        if ((mod & KMOD_CAPS) && ((code >= KEY_Q && code <= KEY_P) || (code >= KEY_A && code <= KEY_L) ||
                                  (code >= KEY_Z && code <= KEY_M))) {
            upper = !upper;
        }
        text[0] = upper ? wdfb_us_upper[code] : wdfb_us_lower[code];
        text[1] = '\0';
        if (text[0]) {
            SDL_SendKeyboardText(text);
        }
    }
}

void WDFB_PumpEvents(_THIS)
{
    WDFB_Data *data = (WDFB_Data *)_this->driverdata;
    struct input_event ev[32];
    Uint8 buf[64];
    int i, k;
    ssize_t n;

    for (i = 0; i < data->nkbd; i++) {
        while ((n = read(data->kbd_fd[i], ev, sizeof(ev))) > 0) {
            for (k = 0; k < (int)(n / (ssize_t)sizeof(ev[0])); k++) {
                if (ev[k].type == EV_KEY) {
                    /* value 0 release, 1 press, 2 autorepeat (SDL sees a repeat) */
                    WDFB_Key(data, ev[k].code, ev[k].value ? SDL_TRUE : SDL_FALSE);
                }
            }
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            fprintf(stderr, "wdfb: keyboard read: %s; dropped\n", strerror(errno));
            close(data->kbd_fd[i]);
            data->kbd_fd[i] = data->kbd_fd[--data->nkbd];
            i--;
        }
    }

    if (data->raw_console) {
        while ((n = read(data->tty_fd, buf, sizeof(buf))) > 0) {
            for (k = 0; k < n; k++) {
                Uint8 b = buf[k];

                /* K_MEDIUMRAW: code | 0x80 on release; codes > 127 are
                   0 (or 0x80), then 7 high bits | 0x80, then 7 low bits | 0x80 */
                if (data->raw_pending == 2) {
                    data->raw_code = (b & 0x7f) << 7;
                    data->raw_pending = 1;
                } else if (data->raw_pending == 1) {
                    data->raw_code |= b & 0x7f;
                    data->raw_pending = 0;
                    WDFB_Key(data, data->raw_code, data->raw_release ? SDL_FALSE : SDL_TRUE);
                } else if ((b & 0x7f) == 0) {
                    data->raw_release = b & 0x80;
                    data->raw_pending = 2;
                } else {
                    WDFB_Key(data, b & 0x7f, (b & 0x80) ? SDL_FALSE : SDL_TRUE);
                }
            }
        }
    }
}

#endif /* SDL_VIDEO_DRIVER_WDFB */

/* vi: set ts=4 sw=4 expandtab: */
