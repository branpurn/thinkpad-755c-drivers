/* SPDX-License-Identifier: GPL-2.0 */
/* One insmod argument list, checked by wd24_load_params_refuse. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../wd24adv_dac_entry.h"

static int truth(const char *v, int *out)
{
	if (!v)
		return -1;
	if (v[0] == '1' && v[1] == 0) {
		*out = 1;
		return 0;
	}
	if (v[0] == '0' && v[1] == 0) {
		*out = 0;
		return 0;
	}
	return -1;
}

int main(int argc, char **argv)
{
	struct wd24_load_params p;
	const char *why = NULL;
	int i, rc;

	memset(&p, 0, sizeof(p));
	p.dryrun = 1;
	p.bind = 1;
	p.fbcon = 1;
	p.trial = 15;
	p.mode = "";
	for (i = 1; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		char *key, *val;
		int bit;

		if (!eq || strchr(argv[i], '$')) {
			fprintf(stderr, "bad token %s\n", argv[i]);
			return 2;
		}
		*eq = 0;
		key = argv[i];
		val = eq + 1;
		if (!strcmp(key, "dryrun")) {
			if (truth(val, &p.dryrun))
				return 2;
		} else if (!strcmp(key, "live")) {
			if (truth(val, &p.live))
				return 2;
		} else if (!strcmp(key, "s6d")) {
			if (truth(val, &p.s6d))
				return 2;
		} else if (!strcmp(key, "s6d_bind")) {
			if (truth(val, &p.bind))
				return 2;
		} else if (!strcmp(key, "s6d_fbcon")) {
			if (truth(val, &p.fbcon))
				return 2;
		} else if (!strcmp(key, "s6d_cmap")) {
			if (truth(val, &p.cmap))
				return 2;
		} else if (!strcmp(key, "s1")) {
			if (truth(val, &p.s1))
				return 2;
		} else if (!strcmp(key, "s2")) {
			if (truth(val, &p.s2))
				return 2;
		} else if (!strcmp(key, "s3")) {
			if (truth(val, &p.s3))
				return 2;
		} else if (!strcmp(key, "s4")) {
			if (truth(val, &p.s4))
				return 2;
		} else if (!strcmp(key, "s5")) {
			p.s5 = atoi(val);
		} else if (!strcmp(key, "s6")) {
			if (truth(val, &p.s6))
				return 2;
		} else if (!strcmp(key, "s6b")) {
			if (truth(val, &p.s6b))
				return 2;
		} else if (!strcmp(key, "s6c")) {
			if (truth(val, &p.s6c))
				return 2;
		} else if (!strcmp(key, "trial")) {
			p.trial = (unsigned int)atoi(val);
		} else if (!strcmp(key, "mode")) {
			p.mode = val;
		} else if (!strcmp(key, "present_mode")) {
			p.present_mode = val;
		} else if (!strcmp(key, "pattern") || !strcmp(key, "fix_sr06") ||
			   !strcmp(key, "accel") || !strcmp(key, "accel_gate") ||
			   !strcmp(key, "cursor_gate") || !strcmp(key, "s6d_thrash_bound")) {
			if (truth(val, &bit) && strcmp(key, "s6d_thrash_bound"))
				return 2;
		} else {
			fprintf(stderr, "unknown %s\n", key);
			return 2;
		}
	}
	rc = wd24_load_params_refuse(&p, NULL, &why);
	if (rc) {
		fprintf(stderr, "REFUSE %s\n", why ? why : "?");
		return 1;
	}
	printf("OK\n");
	return 0;
}
