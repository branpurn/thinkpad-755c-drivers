# wd24adv next stage after S6 v2 (design, not built)

Proven live (S6 v2): PR1 b3 split window; PR31 b7 read-PR0A / write-PR0B over
the whole window with SR06 unlocked; relock to 0F reads back. Sources: data
book and 86Box only.

## Design hole (blocks building)
Any persistent use (banked mmap fbdev or accel copyarea) needs PR31 b7 to stay
set while the driver runs. S6 v2 restored PR31 to 5d *before* relocking, so it
is unproven whether PR31 b7 (a) survives SR06 relock and (b) still steers
reads/writes while locked. 86Box gates only access to SR07+, not their
effect, which suggests yes, but that is not live evidence.

## Options
A. S6c probe (recommended first): same guard/model as S6 v2; E2 baseline,
   unlock, set PR31 b7, relock, verify SR06=0f + locked, run the D2 copy test
   while LOCKED, unlock, restore 5d, relock. Answers (a)+(b) in one short run.
B. Per-operation unlock: keep PR31 b7 only inside a narrow unlock window per
   blit. Works with proven behaviour, but holds SR06 unlocked during each
   accel op; needs IRQ-off/serialised sections. Safe but slower.
C. Single-window banked mmap (S6b-lite): fbdev mmap with fault-driven PR0A
   bank switching only; no PR31, no SR06 unlock. Buildable now; no dual-window
   speedup.
D. Accel without split: copyarea via PR1 b3 split (A0000->PR0B, A8000->PR0A)
   with CPU copy across halves; no SR06 unlock needed. Buildable now.

Recommendation: run A (one live run, new approval), then build S6b
(banked mmap + dual-window copyarea) if b7 persists locked, else C+D.

## Status: option A built as S6c (box only, not run live)
`s6=1 s6c=1` with `dryrun=0 live=1 mode=640x480-8`. Same entry, E2, save,
prefill, P1/P2/D1/V1, U, P3 as S6 v2, then:
L1 relock SR06 with PR31 b7 set (must read 0f + index width locked, PR31
locked read logged) -> D2 copy test LOCKED -> U2 re-unlock, read PR31 (b7
held or cleared) -> P4 PR31 = 5d -> L relock -> V2 -> regions back.
Results: OK = b7 held and effective while locked; B7 NOT HELD LOCKED
(why: "relock cleared PR31 b7" or "kept but no effect while locked");
RESTORE FAIL if L1, U2 or L do not read back.
Decision: OK -> build S6b (banked mmap + dual-window copyarea, SR06 locked
in steady state). NOT HELD -> option B or C+D.

## Status: S6c run live OK -> S6b built (box only, not run live)
`s6b=1` with `dryrun=0 live=1 mode=640x480-8`: /dev/wd24s6b (chrdev, no
fbdev: CONFIG_FRAMEBUFFER_CONSOLE=y would bind fbcon). One exclusive open =
one session.

Export blocker: the tc755x kernel does not export vmf_insert_pfn,
zap_vma_ptes or unmap_mapping_range, so a fault-banked mmap cannot move
banks under a live mapping. S6b therefore maps A0000-AFFFF ONCE
(vm_iomap_memory, pgprot_framebuffer = UC- on the 486) and banks
explicitly: A0000 half = PR0B (w0), A8000 half = PR0A+8000 (w1), PR1 b3 on,
SR06 locked, PR31 b7 off (all proven S6). SETWIN moves the halves (disjoint,
w1 >= 8, <= 78h). No fault path, so no livelock: a cross-bank memmove points
w0 at the source and w1 at the destination.

COPY ioctl (kernel memmove in VRAM): PR1 back to BIOS (b3 off, the S6c
state) -> unlock -> PR31 |= 80 -> relock -> 4 KiB chunks read PR0A / write
PR0B (backward when dst > src) -> unlock, check b7 still set, PR31 base ->
relock -> PR1 b3 on, windows back. Exactly 2 SR06 unlocks per COPY. Any
readback failure -> best-effort restore -> session DEAD (device refuses
more). Teardown: close, munmap, exit and kill -9 all end in release() ->
end() (registers back to entry); text / mode / trial expiry / rmmod end the
session first. A stale user mapping after text restore only touches the
plane window (harmless); the vma pins the module (rmmod refused).

X: Xfbdev cannot drive an explicit-bank device, so X stays on wd90c24fb.
S6b is the proof of the banking + copy primitives for a later wd24adv fb.
