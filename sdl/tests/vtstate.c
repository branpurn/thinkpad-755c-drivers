/* vtstate [TTY]: print a VT's display and keyboard mode (test helper for
   qemu-wdfb.py: did wdfb give the console back?). Default /dev/tty0. */
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <linux/kd.h>

int main(int argc, char **argv)
{
    const char *tty = argc > 1 ? argv[1] : "/dev/tty0";
    int fd = open(tty, O_RDONLY | O_NOCTTY), kd = -1, kb = -1;
    static const char *kbn[] = { "raw", "xlate", "mediumraw", "unicode", "off" };

    if (fd < 0 || ioctl(fd, KDGETMODE, &kd) < 0 || ioctl(fd, KDGKBMODE, &kb) < 0) {
        perror(tty);
        return 1;
    }
    printf("vtstate %s kd=%s kb=%s\n", tty, kd == KD_TEXT ? "text" : kd == KD_GRAPHICS ? "graphics" : "?",
           kb >= 0 && kb <= 4 ? kbn[kb] : "?");
    return 0;
}
