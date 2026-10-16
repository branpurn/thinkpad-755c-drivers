# IBM ThinkPad 755C / 755CS (MT 9545): hardware survey for Linux 6.18

Scope: the original **755C** (10.4" TFT) and **755CS** (9.5" DSTN),
486DX4-75 (some 486DX2-50) on a processor card, ISA + VESA local bus, **no
PCI**. The later 755CE/CSE/CD/CV/CX/CDV are different machines for our
purposes (Mwave DSP instead of the CS4248, IrDA, IBM "Zipang" PC Card
controllers, some Pentium-75). They are noted only where they differ.

Kernel baseline: **Linux 6.18 LTS** (the last LTS with i486 support; i486
removal began in 7.1). Core = [tinycore-32-ram](https://github.com/branpurn/tinycore-32-ram)
tc32 kernel (tinyconfig + i486 + VGA/serial console, floppy, FAT, IDE
`pata_legacy`, ext2, loop/squashfs, APM).

Legend for **Linux status**:
* **core**: already built into the tc32 floppy kernel.
* **existing**: driver is in 6.18 mainline; this repo builds it `=m` and ships it as a `.tcz`.
* **port**: Linux (or XFree86) once had working code for this, but it is gone or never existed as a kernel driver; old logic must be ported.
* **new**: no usable prior Linux code; needs a new driver.
* **n/a**: handled by BIOS/EC firmware; no OS driver needed.

## Summary table

| Device | Exact part (755C) | Resources | Linux 6.18 status | Delivered as | Notes |
|---|---|---|---|---|---|
| CPU | IntelDX4-75 (3.3 V) or 486DX2-50 (SL-enhanced) on processor card | | **core** `CONFIG_M486` | floppy | FPU + CPUID; no TSC/CMOV/CX8 [1][2] |
| RAM | 4 MB on board, parity; IC DRAM card or DIMM card adapter, **max 36 MB** | | core | | [1][3] |
| Video | **WD90C24A** or **WD90C24A2** (Paradise), VL-bus 2.0, 1 MB DRAM; 10.4" TFT 640x480x8 (755C) / 9.5" DSTN (755CS); CRT up to 1024x768x8 | VGA ports 0x3C0-0x3DF, A0000 window; **video BIOS at E0000** (not C0000); **no VBE** in the video BIOS | **core**: vgacon text. **existing**: `vga16fb` 640x480x4bpp (+ fbcon). **new**: native WD90C24A fbdev (8 bpp, panel/CRT, BitBLT) | `fb-vga16.tcz` (now), `wd90c24-probe.tcz` (stage 0 of native) | vesafb/uvesafb impossible (no VBE BIOS; vesafb is also `bool`). Linux never had a WD90C2x fbdev; XFree86 3.3.x SVGA server's `wd90c24` support never made it to 4.x [4][5][6][7] |
| Audio | **Crystal CS4248** (AD1848-compatible WSS codec, 16-bit 48 kHz). **No OPL FM, no MPU-401, no game port** (SB/AdLib were DOS TSR emulation) | default I/O **0x4E30** (alt 0x0030/0x8E30/0xCE30), **IRQ 10** (alt 5/11/15), DMA 0 play / DMA 1 capture; power control via ThinkPad port **0x15E8/0x15E9** bit 2 | **existing**: `snd-ad1848` with `thinkpad=1` (that exact case is in mainline `wss_lib`) | `snd-cs4248.tcz` | `snd_ad1848 port=0x4e30 irq=10 dma1=0 thinkpad=1`. ad1848 driver is single-DMA (half-duplex) [5][8] |
| PC Card | **2x Ricoh RF5C266** (one per slot), Intel 82365SL ExCA register-compatible, ISA. 2x Type II or 1x Type III, PCMCIA 2.1, 16-bit only (no CardBus) | index/data 0x3E0/0x3E1 (dock: 0x3E2/0x3E3) | **existing** in 6.18: `i82365` + `pcmcia` + card drivers. **Removed from mainline in 7.1** (i82365, tcic, i82092) | `pcmcia.tcz` (core + i82365 + `pata_pcmcia` CF/ATA + `serial_cs` modem) | NIC cards need `CONFIG_NET` (bool, large: see ROADMAP). Ricoh RF5C266 Rev. A cannot use C0000/D0000 windows [9][10][11] |
| IDE | internal 2.5" IDE on the system board (legacy ports, PIO) | 0x1F0-0x1F7, 0x3F6, IRQ 14 | **core** `pata_legacy` | floppy | old drivers/ide removed in 5.14; libata `pata_legacy` is the only ISA path [12] |
| Floppy | internal 3.5" 1.44 MB, 82077-class FDC | 0x3F0, IRQ 6, DMA 2 | **core** `floppy` | floppy | ThinkPads invert the disk-change line: kernel option **`floppy=thinkpad`** is documented for this [5][13] |
| Keyboard | 85-key, i8042-compatible controller; Fn keys handled by BIOS/EC | 0x60/0x64, IRQ 1 | **core** `atkbd` + `i8042` | floppy | Fn combos (battery info, blank, suspend, volume, display switch, invert, power mode, hibernate) are firmware functions [14] |
| TrackPoint | IBM TrackPoint (II-era) on the i8042 AUX (PS/2) port; external PS/2 mouse shares it | IRQ 12 | **existing** `psmouse` (+ `MOUSE_PS2_TRACKPOINT`), `mousedev`, `evdev` | `trackpoint.tcz` | Whether the 1994 TrackPoint answers the TrackPoint extended ID (0xE1) is unknown; falls back to plain PS/2 either way |
| Serial | COM1 16550A-class UART | 0x3F8, IRQ 4 | **core** `8250` | floppy | serial console |
| Parallel | bidirectional parallel port (+ docking) | 0x378/0x3BC, IRQ 7 | **existing** `parport_pc`, `lp`, `pata_parport` (replaces paride since 6.4) | (future `parport.tcz`) | Zip / parallel CD-ROM install path |
| Power | **APM BIOS** (no ACPI); smart battery behind the EC; Fn+F4 suspend-to-RAM, Fn+F12 suspend-to-disk | APM 32-bit PM interface | **core** `apm` (`CONFIG_APM`, power-off, `/proc/apm` battery, `/dev/apm_bios`) | floppy | Hibernation needs a type **0xA0** partition or a FAT `PHDISK` file made with `tphdisk`. SMAPI: the old `thinkpad`/`tpctl` module (2.4/2.6) is obsolete and does not build; the 755C BIOS may not even have an `_SM_` entry point → **port**, low priority [5][15][16] |
| LCD panel | 10.4" TFT 640x480 256-of-256K colours (755C); 9.5" DSTN (755CS) | via WD90C24A flat-panel registers (PR18+) | vgacon/vga16fb drive it through BIOS-set panel timing. **new** for native panel/CRT/simultaneous handling | (native driver) | DSTN + CRT simultaneous needs a VRAM frame buffer half (WD90C24) [5][6] |
| IR | **none** on 755C/755CS (IrDA 1.0 is on 755CE/CSE/CD/CV/CX) | | n/a. (IrDA stack was removed from mainline in 4.17 anyway) | | [3][17] |
| Mwave DSP | **not on 755C/CS** (755CE/CSE/CD/CV: MDSP2780) | | 6.18 still has `CONFIG_MWAVE` (modem only) | | out of scope [3][17] |
| PC speaker | standard | 0x61 | **existing** `pcspkr` (input) | (future) | |
| RTC/CMOS | MC146818-compatible | 0x70 | core (x86 RTC reads) | | |

## Video: WD90C24A in detail

* **Chip lineage.** The WD90C24A is a WD90C24 with a 2D engine (BitBLT,
  hardware cursor, line draw) added; it is fully backward compatible with
  the WD90C24 and "somewhat compatible" with the WD90C31's accelerator,
  enough to run the XFree86 C31 path in a pinch. The WD90C30 driver works as
  a substitute for the WD90C24 [5]. VL-bus, 16/32-bit DRAM interface, 1 MB [6][7].
* **No VBE.** The IBM video BIOS has no VESA BIOS Extensions (IBM shipped a
  DOS TSR for that), so `vesafb` / XFree86 `vesa` / `uvesafb` cannot work [5].
  This leaves vgacon text, `vga16fb` (640x480x16, planar) or a native driver.
* **vga16fb on 6.18 needs a shim.** Since vga16fb became a platform
  driver, the `vga-framebuffer` device is created by `sysfb`
  (`CONFIG_SYSFB`, a bool that would have to live in the floppy kernel), and
  its `check_mode_supported()` refuses unless the *boot* BIOS mode was
  already 16-colour graphics (0x0D/0x0E/0x10/0x12), which a text-mode boot
  never is. `modules/tc32-vgafb-dev` registers the device from a module and
  presents mode 0x12; vga16fb then programs the mode itself. Tested in QEMU
  (fbcon 80x30 on 640x480x4).
* **Paradise extended registers** (what a native driver touches; also what
  `modules/wd90c24` reads and restores): PR0-PR4 at GR 0x09-0x0E behind
  PR5 = GR 0x0F (unlock 0x05; PR0A/PR0B are the two 4 KB-granular bank
  registers for the A0000 window); PR10-PR17 at CR 0x29-0x30 behind
  PR10 = CR 0x29 (unlock 0x85); PR20+ at SR 0x06+ (unlock 0x48). On desktop
  WD90C1x/C3x, CR 0x31-0x37 read back "WD90Cxx"; on the flat-panel WD90C2x
  parts that range holds panel registers (PR18 = CR 0x31, flat panel status,
  read-only), as 86Box's WD90C20 model shows [18]. The Paradise BIOS
  signature "VGA=" sits at offset 0x7D of the video BIOS, which on the 755C
  is at E0000.
* **Clocks.** VRAM (memory) clock defaults to 44.297 MHz, programmable to
  55.035 MHz; pixel clock may be at most 1.6x VRAM clock [5].
* **Prior art.** XFree86 3.3.6 SVGA server ("wd" driver, "wd90c24" chipset):
  640x480x8 on the panel, or virtual 640x816 / 1088x480 at 8 bpp [4][19];
  dual-scan 750Cs needed a VRAM tweak (`tpdualscan.c`) [4]. Never ported to
  XFree86 4 / X.Org [4][5]. No Linux kernel fbdev ever existed. A 2006
  XFree86-devel thread notes the Windows 3.1 driver is 76k lines of 16-bit
  assembly and the blitter is weak [20].

## Audio: CS4248 in detail

* AD1848K pin-compatible WSS codec, 16-bit stereo 48 kHz, u-law/A-law,
  software mixer. No synth / MIDI / game port [5].
* Mainline `snd-ad1848` has a `thinkpad` parameter "for the onboard CS4248
  of IBM Thinkpad 360/750/755 series" that toggles bit 2 of index 0x1C at
  ports 0x15E8/0x15E9 to power the codec up (and down on suspend). The same
  register/bit is described on the 755C Linux page [5][8].
* 755C defaults: `port=0x4e30 irq=10 dma1=0 thinkpad=1`; settable in
  ThinkPad Features / Easy-Setup (alternatives 0x0030/0x8E30/0xCE30,
  IRQ 5/11/15) [5]. `snd-cs4248.tcz` ships exactly these defaults; any
  `snd_ad1848.<param>=` on the kernel command line overrides them.

## PC Card in detail

* IBM's own table lists **Ricoh RF5C266 (x2)** for 755C/CS (1-slot part,
  Rev. A cannot map C0000/D0000), the 750 used an Intel 82365SL, the
  755CD/CE/CSE/CV/CX used three IBM "Zipang" [9][10]. The ThinkPad
  controller is always at 0x3E0-0x3E1, docks at 0x3E2-0x3E3 [9].
* Linux `i82365` drives 82365-compatible ExCA bridges and knows the Ricoh
  RF5C296/396 family; the RF5C266 is expected to probe as an 82365SL-class
  (or Ricoh) socket. To verify on hardware.
* **Mainline removal timeline:** PCMCIA char drivers (cm4000/cm4040/scr24x/
  synclink_cs) removed in 2023 [21]; the ISA host controllers
  **i82365, tcic** and the PCI i82092 removed in **7.1** [22][23]. 7.1 is
  also where i486 support removal began, so 6.18 LTS (EOL Dec 2028) is the
  natural end of the line for this machine anyway; if the core ever moves
  past 7.0 we must carry `i82365.c` out of tree.

## Sources

1. IBM announcement 194-168, ThinkPad 755C/755Cs: <https://sharktastica.co.uk/resources/docs/txt/IBM_194-168_TP755C-TP755Cs-Announce_1994_IBM.txt>
2. IBM PC Institute product overview: <http://ps-2.kev009.com/pcpartnerinfo/ctstips/370e.htm>
3. OldThinkPad, ThinkPad 750/755/370C (MT 9545) service reference: <https://oldthinkpad.com/parts/9545>
4. ThinkWiki, WD90C24: <https://www.thinkwiki.org/wiki/WD90C24>
5. "Thinkpad 755C/755Cs" Linux notes (graphics, CS4248 ports/IRQs, ThinkPad control port, floppy=thinkpad, BIOS at E0000, no VBE): <http://home.icequake.net/~nemesis/linuxlaptops/ibm/thinkpad755c/>
6. DOS Days, Western Digital WD90C24: <https://dosdays.co.uk/topics/Manufacturers/paradise/wd90c24.php>
7. Wikipedia, IBM ThinkPad 755: <https://en.wikipedia.org/wiki/ThinkPad_755>
8. Linux 6.18 `sound/isa/ad1848/ad1848.c` (`thinkpad` param) and `sound/isa/wss/wss_lib.c` (`snd_wss_thinkpad_twiddle`)
9. IBM, "TP General - PCMCIA Controllers used in ThinkPad Products": <http://ps-2.kev009.com/pcpartnerinfo/ctstips/caba.htm>
10. Ardent Tool, ThinkPad PCMCIA host controllers: <https://ardent-tool.com/misc/pcmcia_thinkpads.html>
11. Linux 6.18 `drivers/pcmcia/i82365.c`, `drivers/pcmcia/Kconfig`
12. Linux 5.14 removal of the legacy IDE layer (drivers/ide)
13. Linux `Documentation/admin-guide/blockdev/floppy.rst` (`floppy=thinkpad`)
14. ThinkWiki, default meanings of special keys: <https://www.thinkwiki.org/wiki/Default_meanings_of_special_keys>
15. ThinkWiki, How to make APM work (hibernation partition, tphdisk): <https://www.thinkwiki.org/wiki/How_to_make_APM_work>
16. ThinkWiki, SMAPI support for Linux / tpctl: <https://www.thinkwiki.org/wiki/SMAPI_support_for_Linux>, <https://www.thinkwiki.org/wiki/Tpctl>
17. IBM announcement 194-352, ThinkPad 755CE/CSE/CD (Mwave, IrDA): <https://sharktastica.co.uk/resources/docs/txt/IBM_194-352_TP755CE-TP755CSE-TP755CD-Announce_1994_IBM.txt>
18. 86Box `src/video/vid_paradise.c` (Paradise/WD register model): <https://github.com/86Box/86Box/blob/master/src/video/vid_paradise.c>
19. XFree86 3.3.6, "Information for Western Digital Chipset Users": <http://www.xfree86.org/3.3.6/WstDig.html>
20. XFree86-devel, "Re: WD90C24 Anyone?" (2006): <https://www.spinics.net/lists/xfree86-devel/msg00296.html>
21. Linux commit 9b12f050c76f "char: pcmcia: remove all the drivers": <https://github.com/torvalds/linux/commit/9b12f050c76f090cc6d0aebe0ef76fed79ec3f15>
22. "pcmcia: remove obsolete host controller drivers" (LKML, 2026): <https://lists.openwall.net/linux-kernel/2026/03/30/218>
23. Merge "pcmcia-7.1-rc1": <https://github.com/torvalds/linux/commit/45dcf5e28813954da4150e7260ccb61e95856176>
24. IBM Hardware Maintenance Manual vol. 2 (355/360/370/750/755): <https://thinkpads.com/support/hmm/hmm_pdf/tpvol2.pdf>
25. ThinkPad 750/755 device driver matrix: <https://basterfield.com/TPAD-MATRIX/doc/750-755.html>
