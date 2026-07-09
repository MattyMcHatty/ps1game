#!/usr/bin/env python3
"""
preview_4bpp.py - judge which textures survive a 16-colour (4bpp) palette
BEFORE committing them to VRAM.

For each texture it renders the FAITHFUL PS1 result: the same median-cut +
Floyd-Steinberg quantisation png_to_tim.py uses, with every palette colour
rounded to BGR555 (the PS1's real 15-bit colour), then lays the original and
the 4bpp version side by side in one contact-sheet PNG.

Usage (from project root):
    python tools/preview_4bpp.py                 # a default candidate set
    python tools/preview_4bpp.py wd_flr stn_stl  # specific textures (no ext)
Output: tools/preview_4bpp.png
"""
import sys, os
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "textures"))
import png_to_tim as p2t

TEXDIR = os.path.join(os.path.dirname(__file__), "..", "textures")
OUT    = os.path.join(os.path.dirname(__file__), "preview_4bpp.png")

DEFAULT = ["wd_flr", "stn_stl", "kchn_tile", "kchn_wl", "red_crpt", "red_wlppr",
           "din_cl", "stove", "gravel_texture", "brick_wall", "rusty_fence",
           "stn_gls"]

def ps1_round(rgb):
    """Round an 8-bit RGB triple to BGR555 and back, as the PS1 CLUT stores it."""
    r, g, b = rgb
    return ((r >> 3) << 3, (g >> 3) << 3, (b >> 3) << 3)

def render_4bpp(img):
    """Return an RGB image showing the true 4bpp/BGR555 result."""
    _, img_p = p2t.build_clut_4bit(img.convert("RGB"))
    pal = img_p.getpalette() or []
    lut = [ps1_round((pal[i*3], pal[i*3+1], pal[i*3+2])) for i in range(16)]
    out = Image.new("RGB", img_p.size)
    src = img_p.load(); dst = out.load()
    for y in range(img_p.height):
        for x in range(img_p.width):
            dst[x, y] = lut[src[x, y] & 0xF]
    return out

def main():
    names = sys.argv[1:] or DEFAULT
    SCALE, PAD, LBL = 2, 10, 16
    rows = []
    for n in names:
        path = os.path.join(TEXDIR, n + ".png")
        if not os.path.exists(path):
            print("skip (no png):", n); continue
        orig = Image.open(path).convert("RGB")
        four = render_4bpp(orig)
        w, h = orig.size
        orig_s = orig.resize((w*SCALE, h*SCALE), Image.NEAREST)
        four_s = four.resize((w*SCALE, h*SCALE), Image.NEAREST)
        rows.append((n, orig_s, four_s))
        print("rendered:", n)

    if not rows:
        print("nothing to preview"); return
    col_w = max(r[1].width for r in rows)
    row_h = max(r[1].height for r in rows)
    sheet_w = LBL*8 + PAD*3 + col_w*2
    sheet_h = LBL + PAD + len(rows)*(row_h + LBL + PAD)
    sheet = Image.new("RGB", (sheet_w, sheet_h), (30, 30, 30))
    d = ImageDraw.Draw(sheet)
    d.text((PAD, 4), "ORIGINAL", fill=(200, 200, 200))
    d.text((LBL*8 + PAD*2 + col_w, 4), "4bpp (16-colour, PS1 BGR555)", fill=(200, 200, 200))
    y = LBL + PAD
    for name, o, f in rows:
        d.text((PAD, y), name, fill=(255, 220, 120))
        sheet.paste(o, (LBL*8 + PAD, y))
        sheet.paste(f, (LBL*8 + PAD*2 + col_w, y))
        y += row_h + LBL + PAD
    sheet.save(OUT)
    print("\nwrote", OUT)

if __name__ == "__main__":
    main()
