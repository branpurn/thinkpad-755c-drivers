/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Host replay of the 6.18.57 FBIOGETCMAP lifetime. Not the kernel and
 * not the 755C.
 *
 * fb_chrdev.c copies info->cmap under info->lock and calls
 * fb_cmap_to_user only after unlock. The rejected module freed those
 * arrays in that window. s6d_cmap_invalidate sets len to 0 and leaves
 * the arrays until framebuffer_release.
 *
 * Interleavings below are single-threaded unless the function says it
 * starts pthreads. The threads force one order with condition variables.
 * They are not a race fuzzer. info->lock is a pthread mutex standing in
 * for the kernel mutex: the test shows exclusion, not the kernel lock.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "vgamodel.h"
#include "../wd24adv_dac_entry.h"

#if defined(__SANITIZE_ADDRESS__)
#define WD24_ASAN 1
#else
#define WD24_ASAN 0
#endif

static int fails, passes;

#define T(cond, what) do { \
	if (cond) \
		passes++; \
	else { \
		fails++; \
		printf("FAIL %s (line %d)\n", what, __LINE__); \
	} \
} while (0)

struct cmap {
	uint32_t start, len;
	uint16_t *red, *green, *blue;
	unsigned published;
};

struct snap {
	uint32_t start, len;
	uint16_t *red, *green, *blue;
};

/* fb_chrdev.c FBIOGETCMAP: the shallow copy, done under info->lock. */
static void get_snapshot(const struct cmap *c, struct snap *s)
{
	s->start = c->start;
	s->len = c->len;
	s->red = c->red;
	s->green = c->green;
	s->blue = c->blue;
}

/*
 * fb_cmap_to_user after the unlock. memcpy stands in for copy_to_user.
 * Same offset rule: a short cmap returns -EINVAL before any load.
 */
static int copy_out(const struct snap *from, uint32_t start, uint32_t len,
		    uint16_t *red, uint16_t *green, uint16_t *blue,
		    unsigned *loads)
{
	unsigned int tooff = 0, fromoff = 0;
	size_t n;

	if (start > from->start)
		fromoff = start - from->start;
	else
		tooff = from->start - start;
	if (fromoff >= from->len || tooff >= len || from->len == 0 || len == 0)
		return -EINVAL;
	n = len - tooff;
	if (from->len - fromoff < n)
		n = from->len - fromoff;
	if (!n)
		return -EINVAL;
	if (loads)
		*loads += 1;
	memcpy(red + tooff, from->red + fromoff, n * sizeof(uint16_t));
	memcpy(green + tooff, from->green + fromoff, n * sizeof(uint16_t));
	memcpy(blue + tooff, from->blue + fromoff, n * sizeof(uint16_t));
	return 0;
}

/* s6d_cmap_invalidate. Does not free. */
static void invalidate_like_module(struct cmap *c)
{
	c->published = 0;
	if (c->len)
		c->len = 0;
}

/* framebuffer_release -> fb_dealloc_cmap. Only after in-flight copies. */
static void release_like_kernel(struct cmap *c)
{
	free(c->red);
	free(c->green);
	free(c->blue);
	c->red = c->green = c->blue = NULL;
	c->len = 0;
	c->published = 0;
}

/* 30415e50: fb_dealloc_cmap while a snapshot may still point here. */
static void rejected_30415e50_free(struct cmap *c)
{
	free(c->red);
	free(c->green);
	free(c->blue);
	c->red = c->green = c->blue = NULL;
	c->len = 0;
	c->published = 0;
}

static int alloc_cmap(struct cmap *c)
{
	unsigned i;

	memset(c, 0, sizeof(*c));
	c->len = 256;
	c->red = calloc(256, sizeof(uint16_t));
	c->green = calloc(256, sizeof(uint16_t));
	c->blue = calloc(256, sizeof(uint16_t));
	if (!c->red || !c->green || !c->blue)
		return -1;
	for (i = 0; i < 256; i++) {
		c->red[i] = (uint16_t)(0x1000 + i);
		c->green[i] = (uint16_t)(0x2000 + i);
		c->blue[i] = (uint16_t)(0x3000 + i);
	}
	c->published = 1;
	return 0;
}

/* Defined poison of a still-owned buffer. Shows what a GET observes
 * if the arrays are scrapped before copy_to_user. Not a real free. */
static void poison(struct cmap *c)
{
	unsigned i;

	for (i = 0; i < 256; i++) {
		c->red[i] = 0xdead;
		c->green[i] = 0xdead;
		c->blue[i] = 0xdead;
	}
}

#if WD24_ASAN
static int asan_old_uaf_child(void)
{
	struct cmap info;
	struct snap got;
	uint16_t ur[16], ug[16], ub[16];

	if (alloc_cmap(&info))
		return 1;
	get_snapshot(&info, &got);
	rejected_30415e50_free(&info);
	/* Load through the snapshot. AddressSanitizer must stop this. */
	(void)copy_out(&got, 0, 16, ur, ug, ub, NULL);
	return 0;
}
#endif

static struct vgamodel vm, vmsnap;
static struct wd24_ctx ctx;
static struct wd24_io vio;
static uint8_t planes[4 * WD24_PLANE];
static struct wd24_io lie_saved;
static unsigned lie_on, lie_all, lie_n;

static uint8_t lie_inb(void *p, uint16_t port)
{
	uint8_t v = lie_saved.inb(p, port);

	if (port == 0x3c9 && (lie_all || (lie_on && (++lie_n == 4))))
		v ^= 0x01;
	return v;
}

#define RUN(expr) ({ int _e; ctx.be->begin(&ctx); _e = (expr); ctx.be->end(&ctx); _e; })

static void model_setup(void)
{
	vm_init(&vm, VM_WD24, 90);
	vm_io(&vio, &vm);
	wd24_ctx_init(&ctx, &wd24_port_backend);
	ctx.io = &vio;
	ctx.planes = planes;
	memset(planes, 0, sizeof(planes));
	vmsnap = vm;
	lie_on = 0;
}

/* Real threads, one forced order. Expiry sets frozen while it holds hw
 * and before the PUT's allow() runs. */
static pthread_mutex_t info_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t hw_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t meta_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t cv_rel = PTHREAD_COND_INITIALIZER;
static int stage;
static unsigned frozen_flag;
static int allow_result = 99;
static int program_calls;

static struct wd24_cmap_view live_view(void)
{
	struct wd24_cmap_view v;

	memset(&v, 0, sizeof(v));
	v.param = v.live = v.fbcon = 1;
	v.armed = v.active = v.busy = v.have_cmap = v.published = 1;
	v.frozen = frozen_flag;
	return v;
}

static void *put_thread(void *arg)
{
	struct wd24_cmap_view v;

	(void)arg;
	pthread_mutex_lock(&info_mu);
	stage = 1;
	pthread_cond_broadcast(&cv);
	while (stage < 2)
		pthread_cond_wait(&cv, &info_mu);
	pthread_mutex_unlock(&info_mu);

	pthread_mutex_lock(&hw_mu);
	v = live_view();
	allow_result = wd24_cmap_allow(&v);
	if (allow_result == 0)
		program_calls++;
	pthread_mutex_unlock(&hw_mu);
	return NULL;
}

static void *expiry_thread(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&info_mu);
	while (stage < 1)
		pthread_cond_wait(&cv, &info_mu);
	pthread_mutex_unlock(&info_mu);

	pthread_mutex_lock(&hw_mu);
	frozen_flag = 1;		/* s6d_stop, before hw_lock drops */
	pthread_mutex_unlock(&hw_mu);

	pthread_mutex_lock(&info_mu);
	stage = 2;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&info_mu);
	return NULL;
}

/* Release blocks on info->lock until the PUT drops it. */
static int release_entered;

static void *put_holds_info(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&info_mu);
	pthread_mutex_lock(&meta_mu);
	stage = 1;
	pthread_cond_broadcast(&cv_rel);
	while (stage < 2)
		pthread_cond_wait(&cv_rel, &meta_mu);
	pthread_mutex_unlock(&meta_mu);
	/* info->lock is still ours, so release cannot have entered. */
	if (release_entered != 0)
		stage = 99;
	pthread_mutex_unlock(&info_mu);
	return NULL;
}

static void *release_thread(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&meta_mu);
	while (stage < 1)
		pthread_cond_wait(&cv_rel, &meta_mu);
	stage = 2;
	pthread_cond_broadcast(&cv_rel);
	pthread_mutex_unlock(&meta_mu);

	/* Blocks until the holder drops info->lock. */
	pthread_mutex_lock(&info_mu);
	release_entered = 1;
	frozen_flag = 1;
	pthread_mutex_unlock(&info_mu);
	return NULL;
}

int main(void)
{
	struct cmap info;
	struct snap got;
	uint16_t ur[256], ug[256], ub[256];
	unsigned loads;
	struct wd24_cmap_fault fault;
	int err;

	printf("test_cmap_life: host replay of 6.18.57 GET lifetime, not the kernel\n");

	/* In-flight GET already holds the pointers. Invalidate must not free. */
	T(alloc_cmap(&info) == 0, "alloc");
	get_snapshot(&info, &got);
	fault = wd24_cmap_read_mismatch();
	T(fault.invalidate && !fault.rewrite, "read mismatch asks to invalidate");
	invalidate_like_module(&info);
	T(info.len == 0 && info.red != NULL && got.red == info.red && got.len == 256,
	  "invalidate clears len and leaves the snapshotted arrays");
	loads = 0;
	err = copy_out(&got, 0, 256, ur, ug, ub, &loads);
	T(err == 0 && loads == 1 && ur[4] == 0x1004 && ug[4] == 0x2004 && ub[4] == 0x3004,
	  "in-flight GET still copies the pre-invalidate bytes");
	{
		struct snap again;
		unsigned loads2 = 0;

		get_snapshot(&info, &again);
		err = copy_out(&again, 0, 256, ur, ug, ub, &loads2);
		T(err == -EINVAL && loads2 == 0 && again.len == 0,
		  "a later GET sees len 0 and does not load the arrays");
	}
	release_like_kernel(&info);
	T(info.red == NULL && info.len == 0, "deferred free is the release path");

	/* Old ordering, defined poison: the copy observes the scrapped bytes. */
	T(alloc_cmap(&info) == 0, "alloc poison");
	get_snapshot(&info, &got);
	poison(&info);
	err = copy_out(&got, 0, 256, ur, ug, ub, NULL);
	T(err == 0 && ur[0] == 0xdead && ug[1] == 0xdead && ub[2] == 0xdead,
	  "GET that already snapshotted sees a scrap of the arrays");
	release_like_kernel(&info);

	/* Mixed image: rewrite between two channel loads. Single-threaded
	 * stand-in for copy_to_user after unlock. Not an atomic palette. */
	T(alloc_cmap(&info) == 0, "alloc mix");
	get_snapshot(&info, &got);
	ur[0] = got.red[0];
	{
		unsigned i;

		for (i = 0; i < 256; i++)
			info.red[i] = 0x00a0;
	}
	ur[1] = got.red[1];
	T(ur[0] == 0x1000 && ur[1] == 0x00a0,
	  "one GET can observe mixed old and new channels");
	release_like_kernel(&info);

	/* Partial PUT: second entry readback fails, full restore, cache
	 * becomes the whole snapshot. Kernel copies the request only on 0. */
	model_setup();
	err = RUN(wd24_plan_load(&ctx));
	T(err == 0, "model load");
	{
		struct wd24_dac_io dio;
		struct wd24_dac_span sp[2];
		uint8_t saved[768];
		uint16_t er[256], eg[256], eb[256];
		int put;

		memcpy(saved, ctx.cmap, 768);
		dio.outb = vio.outb;
		dio.p = vio.priv;
		sp[0].index = 4;
		sp[0].r = 0x11;
		sp[0].g = 0x12;
		sp[0].b = 0x13;
		sp[1].index = 5;
		sp[1].r = 0x21;
		sp[1].g = 0x22;
		sp[1].b = 0x23;
		lie_saved = vio;
		lie_on = 1;
		lie_n = 0;
		dio.inb = lie_inb;
		put = wd24_dac_put_span(&dio, sp, 2);
		lie_on = 0;
		vio.inb = lie_saved.inb;
		T(put == -5 && !memcmp(ctx.cmap, saved, 768) && vm.dac[12] == 0x11,
		  "partial program fails on the second entry and keeps the snapshot");
		err = RUN(wd24_restore_cmap(&ctx));
		fault = wd24_cmap_settle(put, err, 1);
		T(err == 0 && fault.rewrite && fault.published && !fault.invalidate,
		  "honest full restore rewrites the cache from the snapshot");
		T(!memcmp(vm.dac, vmsnap.dac, 768), "model DAC is the load snapshot again");
		wd24_dac_expand_image(ctx.cmap, er, eg, eb);
		T((er[4] >> 10) == (saved[12] & 0x3f) && er[4] != (uint16_t)(0x11 << 10),
		  "rollback cache is the snapshot, not the rejected entry");
		/* fb_set_cmap copies the request only when rc == 0. */
		T(put != 0, "kernel does not copy the rejected request");
	}

	/* Restore readback fails while a GET already holds the pointers. */
	model_setup();
	T(RUN(wd24_plan_load(&ctx)) == 0, "second load");
	T(alloc_cmap(&info) == 0, "alloc during restore failure");
	get_snapshot(&info, &got);
	{
		struct wd24_dac_io dio;
		struct wd24_dac_span sp;

		dio.outb = vio.outb;
		dio.inb = vio.inb;
		dio.p = vio.priv;
		sp.index = 1;
		sp.r = 1;
		sp.g = 2;
		sp.b = 3;
		T(wd24_dac_put_span(&dio, &sp, 1) == 0, "one entry before the bad restore");
		lie_saved = vio;
		lie_all = 1;
		vio.inb = lie_inb;
		err = RUN(wd24_restore_cmap(&ctx));
		lie_all = 0;
		vio.inb = lie_saved.inb;
		fault = wd24_cmap_settle(-5, err, 1);
		T(err == -EIO && ctx.failed && fault.invalidate && !fault.rewrite,
		  "restore failure invalidates and does not publish the snapshot");
		invalidate_like_module(&info);
		loads = 0;
		T(copy_out(&got, 0, 16, ur, ug, ub, &loads) == 0 && loads == 1 &&
		  ur[3] == 0x1003,
		  "concurrent GET still reads the old arrays; they were not freed");
		{
			struct snap later;
			unsigned none = 0;

			get_snapshot(&info, &later);
			T(copy_out(&later, 0, 16, ur, ug, ub, &none) == -EINVAL && none == 0,
			  "GET after the invalidate does not touch the arrays");
		}
		T(wd24_cmap_allow(&(struct wd24_cmap_view){
			.param = 1, .live = 1, .fbcon = 1, .armed = 1, .active = 1,
			.busy = 1, .have_cmap = 1, .published = 0, .bad = 1
		  }) == -19, "latched failure refuses another program");
		T(wd24_cmap_before_read(0, 0, 1, 0, 1) == -19,
		  "latched failure refuses another DAC read");
	}
	release_like_kernel(&info);

	/* Default off: allow refuses, so the hook does not program. */
	program_calls = 0;
	T(wd24_cmap_param_refuse(0, 1, 0, 1, 0, 1, 1, 0) == 0, "param default is accepted");
	T(wd24_cmap_allow(&(struct wd24_cmap_view){
		.param = 0, .live = 1, .fbcon = 1, .armed = 1, .active = 1,
		.busy = 1, .have_cmap = 1, .published = 1
	  }) == -1, "default off refuses the hook");

	/* fb_set_var holds console_lock then info->lock, then fb_set_cmap.
	 * Frozen: no program. The hook would take hw_lock third. */
	{
		const char *order[4];
		int n = 0;
		struct wd24_cmap_view v = live_view();

		order[n++] = "console";
		order[n++] = "info";
		v.frozen = 1;
		err = wd24_cmap_allow(&v);
		order[n++] = err ? "refuse" : "hw";
		T(err == -16 && n == 3 && !strcmp(order[0], "console") &&
		  !strcmp(order[1], "info") && !strcmp(order[2], "refuse"),
		  "modeset cmap call is inside console and info, and frozen skips hw");
	}

	/* Threads. */
	stage = 0;
	frozen_flag = 0;
	program_calls = 0;
	allow_result = 99;
	{
		pthread_t a, b;

		T(pthread_create(&a, NULL, put_thread, NULL) == 0, "put thread");
		T(pthread_create(&b, NULL, expiry_thread, NULL) == 0, "expiry thread");
		pthread_join(a, NULL);
		pthread_join(b, NULL);
		T(frozen_flag == 1 && allow_result == -16 && program_calls == 0,
		  "expiry freezes under hw before PUT programs");
	}
	stage = 0;
	frozen_flag = 0;
	release_entered = 0;
	{
		pthread_t a, b;

		T(pthread_create(&a, NULL, put_holds_info, NULL) == 0, "holder");
		T(pthread_create(&b, NULL, release_thread, NULL) == 0, "release");
		pthread_join(a, NULL);
		pthread_join(b, NULL);
		T(stage != 99 && release_entered == 1 && frozen_flag == 1,
		  "release sets frozen only after it acquires info->lock");
	}

#if WD24_ASAN
	{
		pid_t pid = fork();
		int st = 0;

		T(pid >= 0, "fork");
		if (pid == 0) {
			int rc = asan_old_uaf_child();

			_exit(rc);
		}
		if (pid > 0) {
			T(waitpid(pid, &st, 0) == pid, "wait old uaf");
			T(WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) != 0),
			  "30415e50 free-then-load is caught");
			if (WIFSIGNALED(st))
				printf("old lifetime: child signal %d (sanitizer)\n", WTERMSIG(st));
			else if (WIFEXITED(st))
				printf("old lifetime: child exit %d\n", WEXITSTATUS(st));
		}
	}
#else
	T(alloc_cmap(&info) == 0, "alloc old free");
	get_snapshot(&info, &got);
	err = copy_out(&got, 0, 4, ur, ug, ub, NULL);
	rejected_30415e50_free(&info);
	T(err == 0 && ur[0] == 0x1000 && info.red == NULL && info.len == 0,
	  "old free nulls the owner; this build does not load through it");
	printf("ASAN child not in this binary; test_cmap_life_asan runs the real free\n");
#endif

	printf("test_cmap_life: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
