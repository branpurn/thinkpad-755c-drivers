// SPDX-License-Identifier: GPL-2.0
/*
 * vromdump: read-only snapshot of the PC firmware areas a VGA BIOS needs,
 * for offline emulation (Stage 0 of wd24adv, see stage0/README.md).
 *
 * At load time it COPIES (reads only; it never writes any I/O port or
 * device register, and maps nothing writable for user space):
 *   /proc/vromdump/vrom     the video option ROM at C0000 (55AA, byte 2 *
 *                           512 bytes; 32 KiB raw if no signature)
 *   /proc/vromdump/sysbios  E0000-FFFFF (system BIOS; on some laptops the
 *                           video BIOS is folded into it)
 *   /proc/vromdump/lowmem   0000-04FF: interrupt vector table + BIOS data
 *                           area (INT 10h vector, current video mode)
 *   /proc/vromdump/info     text summary: signatures, sizes, checksums,
 *                           INT 10h/1Fh/43h/6Dh vectors
 * With scan=1 it also lists option-ROM signatures at every 2 KiB step in
 * C0000-DFFFF (reads only; off by default because that range can hold
 * PC Card memory windows).
 *
 * Everything is snapshotted once into kernel memory; reading the files
 * touches no hardware. rmmod frees it.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>

#define VROM_BASE	0xc0000
#define VROM_MAX	0x10000		/* C0000-CFFFF */
#define VROM_RAW	0x8000
#define SYS_BASE	0xe0000
#define SYS_LEN		0x20000
#define LOW_LEN		0x500
#define SCAN_END	0xe0000

static bool scan;
module_param(scan, bool, 0444);
MODULE_PARM_DESC(scan, "Also list option-ROM signatures in C0000-DFFFF (reads only)");

struct blob {
	const char *name;
	u8 *data;
	size_t len;
};

static struct blob b_vrom = { "vrom" }, b_sys = { "sysbios" }, b_low = { "lowmem" };
static char *info;
static size_t info_len;
static struct proc_dir_entry *dir;

static int snap(struct blob *b, phys_addr_t phys, size_t len)
{
	void __iomem *p = ioremap(phys, len);

	if (!p)
		return -ENOMEM;
	b->data = vmalloc(len);
	if (!b->data) {
		iounmap(p);
		return -ENOMEM;
	}
	memcpy_fromio(b->data, p, len);
	iounmap(p);
	b->len = len;
	return 0;
}

static u8 sum8(const u8 *d, size_t n)
{
	u8 s = 0;

	while (n--)
		s += *d++;
	return s;
}

/* simple_read_from_buffer() is not exported by the trimmed tc755 kernel */
static ssize_t rd(char __user *buf, size_t n, loff_t *pos, const void *src, size_t len)
{
	loff_t p = *pos;

	if (p < 0)
		return -EINVAL;
	if (p >= len || !n)
		return 0;
	n = min_t(size_t, n, len - p);
	if (copy_to_user(buf, src + p, n))
		return -EFAULT;
	*pos = p + n;
	return n;
}

static ssize_t blob_read(struct file *f, char __user *buf, size_t n, loff_t *pos)
{
	struct blob *b = pde_data(file_inode(f));

	return rd(buf, n, pos, b->data, b->len);
}

static ssize_t info_read(struct file *f, char __user *buf, size_t n, loff_t *pos)
{
	return rd(buf, n, pos, info, info_len);
}

static const struct proc_ops blob_ops = {
	.proc_read = blob_read,
	.proc_lseek = default_llseek,
};

static const struct proc_ops info_ops = {
	.proc_read = info_read,
	.proc_lseek = default_llseek,
};

static u32 vec(unsigned int n)
{
	const u8 *v = b_low.data + n * 4;

	return v[0] | v[1] << 8 | v[2] << 16 | (u32)v[3] << 24;
}

static bool mk(struct blob *b)
{
	struct proc_dir_entry *e = proc_create_data(b->name, 0400, dir, &blob_ops, b);

	if (e)
		proc_set_size(e, b->len);
	return e;
}

#define INFO_MAX 4096
#define say(...) (len += scnprintf(info + len, INFO_MAX - len, __VA_ARGS__))

static void build_info(void)
{
	static const u8 ints[] = { 0x10, 0x1f, 0x43, 0x6d, 0x42 };
	size_t len = 0;
	unsigned int i;
	const u8 *v = b_vrom.data, *bda = b_low.data + 0x400;

	say("vromdump 1 (read-only snapshot)\n");
	say("vrom: phys 0x%05x len %zu sig %02x%02x size-byte %u sum8 0x%02x%s\n",
	    VROM_BASE, b_vrom.len, v[0], v[1], v[2], sum8(v, b_vrom.len),
	    (v[0] == 0x55 && v[1] == 0xaa) ? "" : " (NO 55AA: raw 32 KiB)");
	say("sysbios: phys 0x%05x len %zu sum8(F0000-FFFFF) 0x%02x\n", SYS_BASE, b_sys.len,
	    sum8(b_sys.data + 0x10000, 0x10000));
	for (i = 0; i < ARRAY_SIZE(ints); i++) {
		u32 x = vec(ints[i]);

		say("int %02xh -> %04x:%04x (linear 0x%05x)\n", ints[i], x >> 16, x & 0xffff,
		    ((x >> 16) << 4) + (x & 0xffff));
	}
	say("bda: mode 0x%02x cols %u page-size %u crtc 0x%03x rows %u char-height %u egamisc 0x%02x vgaflags 0x%02x\n",
	    bda[0x49], bda[0x4a] | bda[0x4b] << 8, bda[0x4c] | bda[0x4d] << 8,
	    bda[0x63] | bda[0x64] << 8, bda[0x84] + 1, bda[0x85] | bda[0x86] << 8,
	    bda[0x87], bda[0x89]);
	if (scan) {
		phys_addr_t a;
		void __iomem *p = ioremap(VROM_BASE, SCAN_END - VROM_BASE);

		if (p) {
			for (a = VROM_BASE; a < SCAN_END; a += 0x800) {
				void __iomem *q = p + (a - VROM_BASE);

				if (readb(q) == 0x55 && readb(q + 1) == 0xaa)
					say("option rom at 0x%05llx, %u bytes\n",
					    (unsigned long long)a, readb(q + 2) * 512);
			}
			iounmap(p);
		}
	}
	info_len = len;
}

static void free_all(void)
{
	vfree(b_vrom.data);
	vfree(b_sys.data);
	vfree(b_low.data);
	kfree(info);
}

static int __init vromdump_init(void)
{
	size_t vlen = VROM_RAW;
	int ret;
	u8 hdr[3];
	void __iomem *p;

	p = ioremap(VROM_BASE, 3);
	if (!p)
		return -ENOMEM;
	memcpy_fromio(hdr, p, 3);
	iounmap(p);
	if (hdr[0] == 0x55 && hdr[1] == 0xaa && hdr[2])
		vlen = min_t(size_t, hdr[2] * 512, VROM_MAX);

	ret = snap(&b_vrom, VROM_BASE, vlen);
	if (!ret)
		ret = snap(&b_sys, SYS_BASE, SYS_LEN);
	if (!ret)
		ret = snap(&b_low, 0, LOW_LEN);
	info = kzalloc(INFO_MAX, GFP_KERNEL);
	if (ret || !info) {
		free_all();
		return ret ?: -ENOMEM;
	}
	build_info();

	dir = proc_mkdir("vromdump", NULL);
	if (!dir) {
		free_all();
		return -ENOMEM;
	}
	if (!mk(&b_vrom) || !mk(&b_sys) || !mk(&b_low) ||
	    !proc_create_data("info", 0444, dir, &info_ops, NULL)) {
		proc_remove(dir);
		free_all();
		return -ENOMEM;
	}
	pr_info("vromdump: video ROM %zu bytes at 0x%x snapshotted; see /proc/vromdump/info\n",
		b_vrom.len, VROM_BASE);
	return 0;
}

static void __exit vromdump_exit(void)
{
	proc_remove(dir);
	free_all();
}

module_init(vromdump_init);
module_exit(vromdump_exit);
MODULE_DESCRIPTION("Read-only snapshot of the video BIOS ROM, system BIOS and IVT/BDA");
MODULE_LICENSE("GPL");
