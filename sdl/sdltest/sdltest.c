/*
 * sdltest: smoke test for the SDL2 wdfb (wd90c24fb) video backend.
 *
 * Draws 256 palette bars once, then bounces a box, pushing only the old
 * and new box rects each frame with SDL_UpdateWindowSurfaceRects (one
 * WDFB_OP_DAMAGE each on wdfb). Prints frames per second to stdout.
 *
 *   sdltest [-w W] [-h H] [-f|--fullscreen] [-t SECONDS] [-n FRAMES]
 *           [-c|--cycle] [-s BOX] [-p SECONDS]
 *
 * Any key or SDL_QUIT ends it. Default window 320x200, 10 s. -p holds the
 * last frame on screen (a key ends the hold) before SDL_Quit: a look at
 * the result on the panel, or a moment for a test to read the driver's
 * verify/stats while the window is still up.
 * Released under the same terms as the rest of this repository.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SDL.h"

static void usage(void)
{
    fprintf(stderr, "usage: sdltest [-w W] [-h H] [-f] [-t SECONDS] [-n FRAMES] [-c] [-s BOX] [-p SECONDS]\n");
    exit(2);
}

static void set_palette(SDL_Palette *pal, int shift)
{
    SDL_Color c[256];
    int i;

    for (i = 0; i < 256; i++) {
        int k = (i + shift) & 255;
        int band = k >> 6, v = (k & 63) * 4 + 3;

        c[i].r = (band == 0 || band == 3) ? v : 0;
        c[i].g = (band == 1 || band == 3) ? v : 0;
        c[i].b = (band == 2 || band == 3) ? v : 0;
        c[i].a = 255;
    }
    c[0].r = c[0].g = c[0].b = 0;       /* background */
    c[255].r = c[255].g = c[255].b = 255; /* box */
    SDL_SetPaletteColors(pal, c, 0, 256);
}

static void fill(SDL_Surface *s, const SDL_Rect *r, Uint8 v)
{
    int y;

    for (y = 0; y < r->h; y++) {
        memset((Uint8 *)s->pixels + (r->y + y) * s->pitch + r->x, v, r->w);
    }
}

/* the bars live in the top half; the box bounces in the bottom half */
static void draw_bars(SDL_Surface *s, int bars_h)
{
    int x, y;

    for (y = 0; y < bars_h; y++) {
        Uint8 *row = (Uint8 *)s->pixels + y * s->pitch;
        for (x = 0; x < s->w; x++) {
            row[x] = (Uint8)(1 + x * 254 / s->w);
        }
    }
}

int main(int argc, char **argv)
{
    int w = 320, h = 200, box = 32, cycle = 0, fullscreen = 0, i;
    long frames_max = 0;
    double seconds = 10.0, hold = 0;
    SDL_Window *win;
    SDL_Surface *s;
    SDL_Rect r, old, upd[2];
    int dx = 3, dy = 2, bars_h, running = 1, shift = 0;
    Uint32 t0, tsec, now;
    long frames = 0, fsec = 0, nrects = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-f") || !strcmp(a, "--fullscreen")) {
            fullscreen = 1;
        } else if (!strcmp(a, "-c") || !strcmp(a, "--cycle")) {
            cycle = 1;
        } else if (i + 1 < argc && !strcmp(a, "-w")) {
            w = atoi(argv[++i]);
        } else if (i + 1 < argc && !strcmp(a, "-h")) {
            h = atoi(argv[++i]);
        } else if (i + 1 < argc && !strcmp(a, "-t")) {
            seconds = atof(argv[++i]);
        } else if (i + 1 < argc && (!strcmp(a, "-n") || !strcmp(a, "--frames"))) {
            frames_max = atol(argv[++i]);
            seconds = 0;
        } else if (i + 1 < argc && !strcmp(a, "-p")) {
            hold = atof(argv[++i]);
        } else if (i + 1 < argc && !strcmp(a, "-s")) {
            box = atoi(argv[++i]);
        } else {
            usage();
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "sdltest: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    printf("sdltest: SDL %d.%d.%d, video driver %s\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
           SDL_PATCHLEVEL, SDL_GetCurrentVideoDriver());
    win = SDL_CreateWindow("sdltest", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                           fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    if (!win) {
        fprintf(stderr, "sdltest: SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    s = SDL_GetWindowSurface(win);
    if (!s) {
        fprintf(stderr, "sdltest: SDL_GetWindowSurface: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    printf("sdltest: surface %dx%d pitch %d %s\n", s->w, s->h, s->pitch,
           SDL_GetPixelFormatName(s->format->format));
    if (s->format->BitsPerPixel != 8 || !s->format->palette) {
        fprintf(stderr, "sdltest: need an 8bpp indexed surface\n");
        SDL_Quit();
        return 1;
    }
    w = s->w;
    h = s->h;
    if (box > w / 2) box = w / 2;
    if (box > h / 4) box = h / 4;
    if (box < 1) box = 1;

    set_palette(s->format->palette, 0);
    SDL_LockSurface(s);
    memset(s->pixels, 0, (size_t)s->pitch * h);
    bars_h = h / 2;
    draw_bars(s, bars_h);
    r.x = 0; r.y = bars_h; r.w = box; r.h = box;
    fill(s, &r, 255);
    SDL_UnlockSurface(s);
    SDL_UpdateWindowSurface(win);

    t0 = tsec = SDL_GetTicks();
    while (running) {
        SDL_Event ev;

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_KEYDOWN) {
                printf("sdltest: key %s\n", SDL_GetScancodeName(ev.key.keysym.scancode));
            }
            if (ev.type == SDL_QUIT || ev.type == SDL_KEYDOWN) {
                running = 0;
            }
        }
        old = r;
        r.x += dx;
        r.y += dy;
        if (r.x < 0) { r.x = 0; dx = -dx; }
        if (r.x + r.w > w) { r.x = w - r.w; dx = -dx; }
        if (r.y < bars_h) { r.y = bars_h; dy = -dy; }
        if (r.y + r.h > h) { r.y = h - r.h; dy = -dy; }

        SDL_LockSurface(s);
        fill(s, &old, 0);
        fill(s, &r, 255);
        SDL_UnlockSurface(s);
        if (cycle) {
            set_palette(s->format->palette, ++shift);
        }
        upd[0] = old;
        upd[1] = r;
        if (SDL_UpdateWindowSurfaceRects(win, upd, 2) < 0) {
            fprintf(stderr, "sdltest: update: %s\n", SDL_GetError());
            break;
        }
        nrects += 2;
        frames++;
        fsec++;
        now = SDL_GetTicks();
        if (now - tsec >= 1000) {
            printf("fps %.1f\n", fsec * 1000.0 / (now - tsec));
            fflush(stdout);
            tsec = now;
            fsec = 0;
        }
        if (frames_max && frames >= frames_max) break;
        if (seconds > 0 && (now - t0) >= seconds * 1000.0) break;
    }
    now = SDL_GetTicks();
    printf("sdltest: %ld frames, %ld rects in %.2f s, %.1f fps; box at %d,%d\n", frames, nrects,
           (now - t0) / 1000.0, (now - t0) ? frames * 1000.0 / (now - t0) : 0.0, r.x, r.y);
    if (hold > 0 && running) {
        printf("sdltest: holding %.1f s\n", hold);
        fflush(stdout);
        t0 = SDL_GetTicks();
        while (running && SDL_GetTicks() - t0 < hold * 1000.0) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_KEYDOWN) {
                    printf("sdltest: key %s\n", SDL_GetScancodeName(ev.key.keysym.scancode));
                }
                if (ev.type == SDL_QUIT || ev.type == SDL_KEYDOWN) {
                    running = 0;
                }
            }
            SDL_Delay(20);
        }
    }
    printf("sdltest: done\n");
    fflush(stdout);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
