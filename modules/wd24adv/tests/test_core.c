// SPDX-License-Identifier: GPL-2.0
/* Host unit test for the wd24adv core: runs the same self-test as
 * /proc/wd24adv/selftest, plus host-only checks of the plans.
 * Build/run: make -C modules/wd24adv/tests   (or: make wd24adv-test) */
#include <stdio.h>
#include <string.h>
#include "../wd24adv_core.h"

static int fails, passes;
static bool verbose;

#define T(cond, what) do { if (cond) passes++; else { fails++; \
	printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

static struct wd24_ctx c;

static void report(void *arg, bool ok, const char *what)
{
	(void)arg;
	if (ok)
		passes++;
	else
		fails++;
	if (!ok || verbose)
		printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

static unsigned int mode_writes(const struct wd24_mode *m, int *err)
{
	wd24_ctx_init(&c, &wd24_dryrun_backend);
	wd24_model_boot(&c);
	*err = wd24_plan_load(&c);
	if (*err)
		return ~0u;
	c.nwrites = 0;
	*err = wd24_plan_mode(&c, m);
	return c.nwrites;
}

int main(int argc, char **argv)
{
	unsigned int i, n;
	int err = 0;
	const char *why;

	verbose = argc > 1 && !strcmp(argv[1], "-v");
	n = wd24_selftest(&c, report, NULL);
	T(n == 0, "shared self-test");

	/* host-only: mode table shape */
	T(wd24_mode_find("640x480-8") && wd24_mode_find("640x400-8") &&
	  wd24_mode_find("320x200-8"), "the three PROVEN modes exist");
	T(!wd24_mode_find("800x600-crt") && !wd24_mode_find("text-03h"),
	  "CRT and text entries are gone");
	for (i = 0; i < wd24_nmodes; i++) {
		const struct wd24_mode *m = &wd24_modes[i];

		n = mode_writes(m, &err);
		printf("  %-16s %-8s writes=%u err=%d\n", m->name,
		       m->status == WD24_PROVEN ? "PROVEN" : "UNPROVEN", n, err);
		if (m->status == WD24_PROVEN)
			T(err == 0 && n > 0 && n < 200, "PROVEN plan size");
		else
			T(err == -EOPNOTSUPP && n == 0, "UNPROVEN plan empty");
	}

	/* validator rejects a malformed UNPROVEN entry carrying registers */
	{
		struct wd24_mode bad = wd24_modes[0];

		bad.status = WD24_UNPROVEN;
		T(wd24_mode_check(&bad, &why) == -EINVAL, "UNPROVEN with regs rejected");
		bad = wd24_modes[1];
		bad.flags &= ~WD24M_CENTRED;
		T(wd24_mode_check(&bad, &why) == -EINVAL, "400-line mode must be CENTRED");
		bad = wd24_modes[2];
		bad.flags |= WD24M_BANKED;
		T(wd24_mode_check(&bad, &why) == -EINVAL, "13h may not bank");
	}

	/* before load nothing may be planned */
	wd24_ctx_init(&c, &wd24_dryrun_backend);
	wd24_model_boot(&c);
	T(wd24_plan_mode(&c, &wd24_modes[0]) == -EINVAL && c.nwrites == 0,
	  "mode before load refused");
	T(wd24_plan_text(&c) == -EINVAL, "text before load refused");

	/* every write in every PROVEN plan carries a source tag */
	for (i = 0; i < wd24_nmodes; i++) {
		bool tagged = true;

		if (wd24_modes[i].status != WD24_PROVEN)
			continue;
		mode_writes(&wd24_modes[i], &err);
		wd24_plan_text(&c);
		for (unsigned int k = 0; k < c.nev; k++)
			if (c.ev[k].type == WD24_EV_WRITE && !c.ev[k].note)
				tagged = false;
		T(tagged, "every write has a wd90c24fb citation");
	}

	printf("wd24adv host test: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
