/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Read-only 755C calibration probe. Index selects on 3D4/3C4/3CE are
 * the only writes. Data registers are not written. --dry prints the
 * plan and does not open /dev/port. Do not run this on the 755C from
 * this tree; the owner runs it later.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int portfd = -1;

static int pout(unsigned addr, unsigned val)
{
	unsigned char b = (unsigned char)val;
	if (portfd < 0)
		return -1;
	return pwrite(portfd, &b, 1, (off_t)addr) == 1 ? 0 : -1;
}

static int pin(unsigned addr)
{
	unsigned char b = 0;
	if (portfd < 0)
		return -1;
	if (pread(portfd, &b, 1, (off_t)addr) != 1)
		return -1;
	return b;
}

static int cr(unsigned idx)
{
	if (pout(0x3d4, idx))
		return -1;
	return pin(0x3d5);
}

int main(int argc, char **argv)
{
	int i, v, high = 0, low = 0, edges = 0, prev = -1;
	int dry = argc > 1 && !strcmp(argv[1], "--dry");

	if (dry) {
		printf("PROBE dry cr00-18 3cc 3da-loops\n");
		printf("PROBE writes index 3d4 only\n");
		return 0;
	}
	portfd = open("/dev/port", O_RDWR);
	if (portfd < 0) {
		printf("PROBE FAIL no-port\n");
		return 1;
	}
	v = pin(0x3cc);
	printf("PROBE misc %02x\n", v & 0xff);
	for (i = 0; i <= 0x18; i++) {
		v = cr((unsigned)i);
		if (v < 0) {
			printf("PROBE FAIL cr%02x\n", i);
			return 1;
		}
		printf("PROBE cr%02x %02x\n", i, v);
	}
	for (i = 0; i < 200000; i++) {
		v = pin(0x3da);
		if (v < 0) {
			printf("PROBE FAIL 3da\n");
			return 1;
		}
		if (v & 0x08)
			high++;
		else
			low++;
		if (prev >= 0 && ((prev ^ v) & 0x08))
			edges++;
		prev = v;
	}
	printf("PROBE 3da high %d low %d edges %d\n", high, low, edges);
	close(portfd);
	return 0;
}
