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
