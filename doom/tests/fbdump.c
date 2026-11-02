/* fbdump [DEV]: print an 8 bpp frame buffer and its palette as hex text,
   for doom/tests/qemu-doom.py to turn into a PNG (test helper):
     FBDUMP <width> <height>
     PAL <768 hex digits: 256 x RGB, 8 bits each>
     R <y> <hex row>          (one line per row)
     FBDUMP END */
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    unsigned short r[256], g[256], b[256];
    struct fb_cmap cmap = { 0, 256, r, g, b, NULL };
    unsigned char *fb;
    unsigned x, y;
    int fd = open(dev, O_RDONLY);

    if (fd < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fd, FBIOGETCMAP, &cmap) < 0 || var.bits_per_pixel != 8) {
        perror(dev);
        return 1;
    }
    fb = mmap(NULL, fix.smem_len, PROT_READ, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    printf("FBDUMP %u %u\nPAL ", var.xres, var.yres);
    for (x = 0; x < 256; x++)
        printf("%02x%02x%02x", r[x] >> 8, g[x] >> 8, b[x] >> 8);
    printf("\n");
    for (y = 0; y < var.yres; y++) {
        const unsigned char *p = fb + y * fix.line_length;
        printf("R %u ", y);
        for (x = 0; x < var.xres; x++)
            printf("%02x", p[x]);
        printf("\n");
    }
    printf("FBDUMP END\n");
    return 0;
}
