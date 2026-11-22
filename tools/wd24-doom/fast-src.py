#!/usr/bin/env python3
# Copy two doomgeneric sources and apply the opt-in fast edits.
# direct: I_FinishUpdate does not copy when the index frame is already
#         the 320x200 buffer. unroll: column, span, fuzz, and translated
#         column write four pixels per trip, same pixels as the do/while.
import pathlib
import sys

src = pathlib.Path(sys.argv[1])
dst = pathlib.Path(sys.argv[2])
mode = sys.argv[3]
dst.mkdir(parents=True, exist_ok=True)

col_old = """\
    do 
    {
	// Re-map color indices from wall texture column
	//  using a lighting/special effects LUT.
	*dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];
	
	dest += SCREENWIDTH; 
	frac += fracstep;
	
    } while (count--); 
"""
col_new = """\
    {
	int left = count + 1;

	while (left >= 4) {
	    *dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];
	    dest += SCREENWIDTH;
	    frac += fracstep;
	    *dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];
	    dest += SCREENWIDTH;
	    frac += fracstep;
	    *dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];
	    dest += SCREENWIDTH;
	    frac += fracstep;
	    *dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];
	    dest += SCREENWIDTH;
	    frac += fracstep;
	    left -= 4;
	}
	while (left--) {
	    *dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];
	    dest += SCREENWIDTH;
	    frac += fracstep;
	}
    }
"""
span_old = """\
    do
    {
	// Calculate current texture index in u,v.
        ytemp = (position >> 4) & 0x0fc0;
        xtemp = (position >> 26);
        spot = xtemp | ytemp;

	// Lookup pixel from flat texture tile,
	//  re-index using light/colormap.
	*dest++ = ds_colormap[ds_source[spot]];

        position += step;

    } while (count--);
"""
span_new = """\
    {
	int left = count + 1;

	while (left >= 4) {
	    ytemp = (position >> 4) & 0x0fc0;
	    xtemp = (position >> 26);
	    spot = xtemp | ytemp;
	    *dest++ = ds_colormap[ds_source[spot]];
	    position += step;
	    ytemp = (position >> 4) & 0x0fc0;
	    xtemp = (position >> 26);
	    spot = xtemp | ytemp;
	    *dest++ = ds_colormap[ds_source[spot]];
	    position += step;
	    ytemp = (position >> 4) & 0x0fc0;
	    xtemp = (position >> 26);
	    spot = xtemp | ytemp;
	    *dest++ = ds_colormap[ds_source[spot]];
	    position += step;
	    ytemp = (position >> 4) & 0x0fc0;
	    xtemp = (position >> 26);
	    spot = xtemp | ytemp;
	    *dest++ = ds_colormap[ds_source[spot]];
	    position += step;
	    left -= 4;
	}
	while (left--) {
	    ytemp = (position >> 4) & 0x0fc0;
	    xtemp = (position >> 26);
	    spot = xtemp | ytemp;
	    *dest++ = ds_colormap[ds_source[spot]];
	    position += step;
	}
    }
"""
fuzz_old = """\
    do 
    {
	// Lookup framebuffer, and retrieve
	//  a pixel that is either one column
	//  left or right of the current one.
	// Add index from colormap to index.
	*dest = colormaps[6*256+dest[fuzzoffset[fuzzpos]]]; 

	// Clamp table lookup index.
	if (++fuzzpos == FUZZTABLE) 
	    fuzzpos = 0;
	
	dest += SCREENWIDTH;

	frac += fracstep; 
    } while (count--); 
"""
fuzz_body = """\
	*dest = colormaps[6*256+dest[fuzzoffset[fuzzpos]]];
	if (++fuzzpos == FUZZTABLE)
	    fuzzpos = 0;
	dest += SCREENWIDTH;
	frac += fracstep;
"""
fuzz_new = """\
    {
	int left = count + 1;

	while (left >= 4) {
""" + (fuzz_body * 4) + """\
	    left -= 4;
	}
	while (left--) {
""" + fuzz_body + """\
	}
    }
"""
xlat_old = """\
    do 
    {
	// Translation tables are used
	//  to map certain colorramps to other ones,
	//  used with PLAY sprites.
	// Thus the "green" ramp of the player 0 sprite
	//  is mapped to gray, red, black/indigo. 
	*dest = dc_colormap[dc_translation[dc_source[frac>>FRACBITS]]];
	dest += SCREENWIDTH;
	
	frac += fracstep; 
    } while (count--); 
"""
xlat_body = """\
	*dest = dc_colormap[dc_translation[dc_source[frac>>FRACBITS]]];
	dest += SCREENWIDTH;
	frac += fracstep;
"""
xlat_new = """\
    {
	int left = count + 1;

	while (left >= 4) {
""" + (xlat_body * 4) + """\
	    left -= 4;
	}
	while (left--) {
""" + xlat_body + """\
	}
    }
"""

iv = (src / "i_video.c").read_text()
rd = (src / "r_draw.c").read_text()
if "direct" in mode or mode == "all":
    old_alloc = "\tI_VideoBuffer = (byte*)Z_Malloc (SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);  // For DOOM to draw on"
    new_alloc = """\
#ifdef WD24X_DRAW_DIRECT
	/* Same 320x200 index bytes DG_DrawFrame already sends. */
	I_VideoBuffer = (byte *)DG_ScreenBuffer;
#else
	I_VideoBuffer = (byte*)Z_Malloc (SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);  // For DOOM to draw on
#endif"""
    old_free = "\tZ_Free (I_VideoBuffer);"
    new_free = """\
#ifndef WD24X_DRAW_DIRECT
	Z_Free (I_VideoBuffer);
#endif
	I_VideoBuffer = NULL;"""
    old_loop = "    y = SCREENHEIGHT;\n"
    new_loop = """\
#ifdef WD24X_DRAW_DIRECT
    if (fb_scaling == 1 && s_Fb.bits_per_pixel == 8 &&
	s_Fb.xres == SCREENWIDTH && s_Fb.yres == SCREENHEIGHT &&
	x_offset == 0 && y_offset == 0) {
	DG_DrawFrame();
	return;
    }
#endif
    y = SCREENHEIGHT;
"""
    if old_alloc not in iv or iv.count(old_free) != 1 or iv.count(old_loop) != 1:
        sys.exit("i_video.c shape changed")
    iv = iv.replace(old_alloc, new_alloc, 1).replace(old_free, new_free, 1)
    iv = iv.replace(old_loop, new_loop, 1)
if "unroll" in mode or mode == "all":
    if rd.count(col_old) != 1:
        sys.exit("r_draw.c column loop changed")
    if rd.count(span_old) != 1:
        sys.exit("r_draw.c span loop changed")
    if rd.count(fuzz_old) != 1:
        sys.exit("r_draw.c fuzz loop changed")
    if rd.count(xlat_old) != 1:
        sys.exit("r_draw.c translated loop changed")
    rd = rd.replace(col_old, col_new, 1).replace(span_old, span_new, 1)
    rd = rd.replace(fuzz_old, fuzz_new, 1).replace(xlat_old, xlat_new, 1)
(dst / "i_video.c").write_text(iv)
(dst / "r_draw.c").write_text(rd)
print("fast-src", mode)
