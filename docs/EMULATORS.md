# Emulator coverage for 755C hardware

| Device | QEMU 8.2 (`-M isapc -cpu 486`) | 86Box (master, Oct 2026) | PCem |
|---|---|---|---|
| WD90C24 / WD90C24A / A2 | **no** | **no.** Has Paradise PVGA1A, WD90C11-LR (+ MegaPC onboard), WD90C20 (IBM PS/55 5535-S onboard), WD90C30-LR, WD90C31A-LR (+ Diamond SpeedStar 24X, onboard C31); a WD90C33 machine is marked "not yet implemented". No ThinkPad machine | no WD90C24 found (code search) |
| closest stand-in for video work | ISA VGA (`vga16fb`, generic VGA paths) | **WD90C30/WD90C31**: same Paradise PR register file and banking; C31 blitter is "somewhat compatible" with the C24A engine. Needs the card BIOS ROMs | WD90C11 |
| CS4248 | `cs4231a` ISA (0x534, IRQ 9, DMA 3). **Works with `snd-ad1848` and `snd-cs4231`**; wss_lib identifies it as "CMI8330/C3D" | AD1848 / **CS4248** / CS4231 codec model (`snd_ad1848.c`, `AD1848_TYPE_CS4248`) behind its WSS-family cards: a truer test of the CS4248 detection path | WSS-class |
| PCMCIA (i82365 / Ricoh) | **no** | no i82365 / RF5C emulation found | no |
| PS/2 mouse (TrackPoint stand-in) | yes (i8042 AUX, ImExPS/2) | yes | yes |
| APM BIOS | SeaBIOS APM 1.2 (power-off works) | machine BIOS dependent | machine BIOS dependent |
| 486 strictness | TCG does not trap all i686 opcodes | cycle-accurate-ish 486 models | 486 models |

Consequences:

* **Automated CI here is QEMU only** (`make test`): it proves the module
  build, the `.tcz` packaging, tce-load, dependency ordering, parameter
  passing, `vga16fb`+fbcon (screenshot), `psmouse`, and the exact 755C
  audio driver (`snd-ad1848`) doing DMA playback on a WSS codec. It cannot
  test the WD90C24A, the Ricoh bridges, `thinkpad=1`, or APM suspend.
* **86Box** is the right bench for the native video driver's register
  paths (WD90C30/C31) and for CS4248 detection. It is GUI-driven and needs
  copyrighted ROM images, so it is documented, not wired into `make test`.
* **Real 755C** over COM1 (9600 8N1) is required for panel/CRT, the
  WD90C24A-specific blitter differences, PC Card, suspend and the Fn keys.

Sources: QEMU `-device cs4231a,help`; 86Box `src/video/vid_paradise.c`,
`src/sound/snd_ad1848.c`, `src/machine/machine_table.c`
(<https://github.com/86Box/86Box>); 86Box display docs
(<https://86box.readthedocs.io/en/stable/settings/display.html>); a 2024
r/86box thread trying to approximate a ThinkPad 755 with a WD90C11-LR
(<https://www.reddit.com/r/86box/comments/1h6256x/thinkpad_755_configuration/>);
GitHub code search of the PCem repository for `90c24` (no hits; not checked further).
