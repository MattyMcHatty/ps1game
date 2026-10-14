#!/usr/bin/env python3
"""Split textures/catacombs/gula.png into the two 8bpp TIM halves Gula ships as.

RUN THIS WHENEVER THE ART CHANGES.

    py tools\\split_gula_sheet.py

Gula's four frames are one 256x256 sheet, one frame per quarter, read left to
right and top row first (src/gula.h). The brief was a FULL 256x256 texture with
more than 16 colours, so it ships at 8bpp and 256 colours - and an 8bpp texture
256 rows tall has no legal home in this VRAM (V is eight bits: y%256 + height
<= 256, so it must start at y 0 or 256, and both are spoken for; STEP 3b-quater
of tools/ADDING_AN_ENEMY.txt). So it is cut on the row boundary, which is also a
frame boundary, into two 256x128 halves - the Crawler's own shape - and each
goes over one of the Crawler's two blocks while the player is in the Kennels:

    GULAA  frames 0, 1   x[320,448) y128   (crawler_a's block)
    GULAB  frames 2, 3   x[704,832) y128   (crawler_b's block)

Both carry ONE palette, at crawler_a's CLUT line (672,506). No crawler is ever
in the Kennels, and every other Chapter 3 room re-uploads the crawler (pixels
AND palettes) on entry, so nothing has to be put back (src/gula.h).

>>> THIS WRITES THE TIMs ITSELF RATHER THAN CALLING png_to_tim.py. <<< That
tool quantises whatever single image it is handed, so two halves would come out
on two different palettes (in two different index orders) and the cycle would
shift colour between frame 1 and frame 2. Here the palette is built ONCE over
the opaque pixels of the WHOLE sheet - 255 colours - and index 0 is reserved for
the holes (CLUT 0x0000, which the GPU skips: see-through, no blending needed).

STP IS SET ON EVERY OPAQUE ENTRY, and it is load-bearing: she fades IN over
three seconds and fades OUT on her death, and a fade here is ADDITIVE blending
(tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 5, TRICK 1). The PS1 blends a textured
primitive per TEXEL and only where the palette entry has bit 15 set. STP has no
effect on a primitive that is not semi-transparent, so her ordinary draw is
unchanged. (It also keeps an opaque pure black from reading as 0x0000.)

The two halves are also written as PNGs (gula_top.png / gula_bot.png) already
reduced to the master palette, so the conversion can be eyeballed and stays
reproducible.
"""
import os
import struct
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'textures'))
import png_to_tim  # noqa: E402  (rgb_to_ps1, write_tim)

TEXDIR    = os.path.join(ROOT, 'textures', 'catacombs')
SRC       = os.path.join(TEXDIR, 'gula.png')
ALPHA_CUT = 128          # png_to_tim.py's default
CLUT_XY   = (672, 506)   # crawler_a's CLUT line; both halves share it

#        out .tim                             preview png                     y0   tx   ty
HALVES = [(os.path.join(TEXDIR, 'gula_a.tim'), os.path.join(TEXDIR, 'gula_top.png'),   0, 320, 128),
          (os.path.join(TEXDIR, 'gula_b.tim'), os.path.join(TEXDIR, 'gula_bot.png'), 128, 704, 128)]


def main():
    im = Image.open(SRC).convert('RGBA')
    if im.size != (256, 256):
        sys.exit('%s is %dx%d; the sheet must be 256x256' % ((SRC,) + im.size))
    w, h = im.size
    px = im.load()

    opaque = [(x, y) for y in range(h) for x in range(w) if px[x, y][3] >= ALPHA_CUT]
    if not opaque:
        sys.exit('%s has no opaque pixels' % SRC)

    # The palette: 255 colours over the opaque pixels ALONE, so the holes cost
    # nothing and cannot drag a colour toward whatever fills them.
    strip = Image.new('RGB', (len(opaque), 1))
    strip.putdata([px[x, y][:3] for (x, y) in opaque])
    pal_img = strip.quantize(colors=255)
    pal = pal_img.getpalette()[:255 * 3]
    # Pad to 256 with copies of colour 0, never with black: a padding entry
    # chosen by the mapping below would otherwise introduce a colour that is
    # not in the art. Any pick of 255 is folded back onto 0.
    pal += pal[:3]
    pal_img.putpalette(pal)

    # Map the whole sheet onto it. Holes are filled with colour 0 first so the
    # dither has something neutral to diffuse into, then overwritten.
    rgb = Image.new('RGB', (w, h))
    fill = tuple(pal[:3])
    rgb.putdata([px[x, y][:3] if px[x, y][3] >= ALPHA_CUT else fill
                 for y in range(h) for x in range(w)])
    mapped = rgb.quantize(palette=pal_img).load()

    # TIM index: 0 = hole, 1..255 = palette 0..254.
    idx = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if px[x, y][3] >= ALPHA_CUT:
                i = mapped[x, y]
                if i >= 255:
                    i = 0
                idx[y][x] = i + 1

    clut = struct.pack('<H', 0x0000)
    for i in range(255):
        r, g, b = pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]
        clut += struct.pack('<H', png_to_tim.rgb_to_ps1(r, g, b) | 0x8000)
    assert len(clut) == 512

    used = set()
    for out, preview, y0, tx, ty in HALVES:
        pixels = bytes(idx[y][x] for y in range(y0, y0 + 128) for x in range(w))
        png_to_tim.write_tim(out, 8, pixels, clut, tx, ty, w, 128,
                             CLUT_XY[0], CLUT_XY[1])
        used |= set(pixels)

        prev = Image.new('RGBA', (w, 128))
        prev.putdata([(0, 0, 0, 0) if k == 0 else
                      (pal[(k - 1) * 3], pal[(k - 1) * 3 + 1], pal[(k - 1) * 3 + 2], 255)
                      for k in pixels])
        prev.save(preview)
        print('%s  %d bytes  at (%d,%d)  CLUT (%d,%d)' %
              (out, os.path.getsize(out), tx, ty, CLUT_XY[0], CLUT_XY[1]))

    print('palette entries used: %d of 256 (index 0 = transparent)' % len(used))


if __name__ == '__main__':
    main()
