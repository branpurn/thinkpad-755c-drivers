# Third-party sources

## XFree86 3.3.x pvga1 driver (WD90C24 register logic)

`modules/wd90c24fb/` ports register sequences from the XFree86 3.3.x
`vga256/drivers/pvga1` driver: probe order, unlock values, save/restore
order, the WD90C24 memory-size rule, the 256-colour CRTC and Paradise
settings, and the WD90C31/C24A BITBLT command layout (`pvg_driver.c`,
`paradise.h`, `pvgaBitBlt.c`, `pvgablt.c`). The code here is written for
this driver; it ports logic rather than copying code. That code is under
the X11/MIT-style permissive licence below, which is GPL-2.0-compatible.
Its notice is reproduced as the licence asks:

> Copyright 1990,91 by Thomas Roell, Dinkelscherben, Germany.
>
> Permission to use, copy, modify, distribute, and sell this software and its
> documentation for any purpose is hereby granted without fee, provided that
> the above copyright notice appear in all copies and that both that
> copyright notice and this permission notice appear in supporting
> documentation, and that the name of Thomas Roell not be used in
> advertising or publicity pertaining to distribution of the software without
> specific, written prior permission.  Thomas Roell makes no representations
> about the suitability of this software for any purpose.  It is provided
> "as is" without express or implied warranty.
>
> THOMAS ROELL DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
> INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
> EVENT SHALL THOMAS ROELL BE LIABLE FOR ANY SPECIAL, INDIRECT OR
> CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
> DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
> TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
> PERFORMANCE OF THIS SOFTWARE.

The WD90C31/C24A acceleration in that driver was added by Mike Tierney and
the 90C33 support by Bill Conn and Bill Morgart (per its source comments).
The blit loop structure in `pvgaBitBlt.c` derives from the X Consortium's
`cfbbitblt.c` (Keith Packard), under the X Consortium licence, which is
also permissive and GPL-compatible.

## Data sheets (facts only, nothing copied)

* Western Digital WD90C24A/A2 data sheet, 79-890002-001 Rev D (1994),
  bitsavers.org `components/westernDigital/_dataSheets/WD90C24A_199410.pdf`
* Western Digital WD90C24 (1993 data book chapter 13), same archive.

## 86Box (bench only)

`vid_paradise.c` (GPL-2.0-or-later) was read to cross-check banking
behaviour. No code is used.
