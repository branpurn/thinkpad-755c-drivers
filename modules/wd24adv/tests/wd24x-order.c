/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Print selected log lines in the order they appear.
 * It does not assign a cause to a stripe, a square, or a band.
 */
#include <stdio.h>
#include <string.h>

static const char *const mark[] = {
	"live: mode",
	"session ON",
	"fb open failed",
	"end (",
	"trial expiry",
	"Can't deallocate",
};

int main(int argc, char **argv)
{
	FILE *fp;
	char line[512];
	unsigned n = 0;
	unsigned i;

	fp = argc > 1 ? fopen(argv[1], "r") : stdin;
	if (!fp) {
		printf("WD24X-ORDER bad open\n");
		return 1;
	}
	printf("WD24X-ORDER begin\n");
	while (fgets(line, sizeof line, fp)) {
		size_t len = strlen(line);
		while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = 0;
		for (i = 0; i < sizeof mark / sizeof mark[0]; i++) {
			if (strstr(line, mark[i])) {
				printf("%u %s\n", ++n, line);
				break;
			}
		}
	}
	if (fp != stdin)
		fclose(fp);
	printf("WD24X-ORDER count %u\n", n);
	return 0;
}
