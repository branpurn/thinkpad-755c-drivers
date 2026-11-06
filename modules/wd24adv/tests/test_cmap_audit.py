#!/usr/bin/env python3
"""Source gates for the palette path. Not a hardware run."""
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[3]
main = (root / "modules/wd24adv/wd24adv_main.c").read_text()
get_c = (root / "modules/wd24adv/tests/wd24x-cmap-get.c").read_text()
set_c = (root / "modules/wd24adv/tests/wd24x-cmap-set.c").read_text()
p2 = (root / "docs/s6d-cmap-p2-run.sh").read_text()
p3 = (root / "docs/s6d-cmap-p3-run.sh").read_text()
fails = 0


def body(src, sig):
    start = 0
    while True:
        i = src.find(sig, start)
        if i < 0:
            raise SystemExit(f"missing {sig}")
        j = src.find("{", i)
        semi = src.find(";", i)
        if semi != -1 and (j == -1 or semi < j):
            start = semi + 1
            continue
        depth = 0
        for k in range(j, len(src)):
            if src[k] == "{":
                depth += 1
            elif src[k] == "}":
                depth -= 1
                if depth == 0:
                    return src[j:k + 1]
        raise SystemExit(f"unclosed {sig}")


def code(text):
    kept = []
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("*") or s.startswith("/*") or s.startswith("//"):
            continue
        kept.append(line)
    return "\n".join(kept)


def need(cond, what):
    global fails
    if cond:
        return
    fails += 1
    print("FAIL", what)


rel = body(main, "static int s6d_fb_release(")
end = body(main, "static void s6d_end_locked(")
stop = body(main, "static void s6d_stop(")
pub = body(main, "static int s6d_cmap_publish(")
opn = body(main, "static int s6d_fb_open(")
hook = body(main, "static int s6d_fb_setcmap(")

for name, text in (("release", rel), ("end", end)):
    need("wd24_restore_cmap" not in text, f"{name} restores cmap")
    need("dac_write" not in text, f"{name} dac_write")
    need("s6d_cmap_published =" not in text, f"{name} clears published")

need("s6d_cmap_frozen = true" in stop, "stop freezes")
need("s6d_cmap_published =" not in stop, "stop clears published")
need("wd24_restore_cmap" not in stop, "stop restores")
need("dac_read" in pub and "dac_write" not in pub, "publish is a read")
need("wd24_restore_cmap" not in pub, "publish restores")
need("s6d_cmap_publish" in opn, "open publishes")
need("dac_write" not in opn and "wd24_restore_cmap" not in opn, "open writes colour")
need("wd24_restore_cmap" in hook, "put failure restores the snapshot")
need("wd24_dac_put_entry" in hook, "put writes one entry through the helper")

get_code = code(get_c)
set_code = code(set_c)
need("FBIOPUTCMAP" not in get_code, "get client put ioctl")
need("FBIOPUT_VSCREENINFO" not in get_code and "FBIOGET_VSCREENINFO" not in get_code,
     "get client modeset call")
need("mmap(" not in get_code, "get client mmap")
need(set_code.count("FBIOPUTCMAP") == 1, "set client one put")
need("FBIOPUT_VSCREENINFO" not in set_code, "set client modeset call")
need("mmap(" not in set_code, "set client mmap")
need("0x0001" in get_code and "0x0001" in set_code, "prefill")

need("Xfbdev" not in p2 and "Xfbdev" not in p3, "phase scripts start X")
need("s6d_cmap=1" in p2 and "s6d_cmap=1" in p3, "cmap param")
insmods = [ln for ln in p2.splitlines() if ln.startswith("insmod ")]
need(len(insmods) == 1 and "92591b1e" in insmods[0] and "pr14" not in insmods[0],
     "phase 2 loads baseline")
need("cmap_param 1 published 1 frozen" in p2, "published gate")
need('wd24adv live: trial expiry -> 0' in p2, "exact expiry line")
need("WD24X_BATCH_ARM" not in p2 and "WD24X_BATCH_ARM" not in p3, "phases self-arm")

print(f"test_cmap_audit: {0 if fails else 1} file, {fails} failed")
sys.exit(1 if fails else 0)
