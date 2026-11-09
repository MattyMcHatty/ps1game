#!/usr/bin/env python3
"""Split textures/underworld/ahriman.png into the two halves the TIM pair is
built from, and (with --tim) convert both.

RUN THIS WHENEVER THE ART CHANGES. The Ahriman's four frames are one 256x256
sheet, 2x2, read left to right and top row first - the wing-beat cycle, played
0, 1, 2, 3 for as long as it lives:

    frames 0, 1  (top row)      AHRIMANA
    frames 2, 3  (bottom row)   AHRIMANB

A 4bpp texture 256 rows tall cannot be placed (V is eight bits, so
y%256 + height <= 256 and a 256-row texture can only start at VRAM y 0 or 256,
both spoken for), so it ships as two 256x128 halves cut on the row boundary,
which is also a frame boundary.

>>> THE TWO HALVES MUST SHARE ONE PALETTE. <<< png_to_tim.py quantises
whatever single image it is handed, so converting each half on its own would
give two independent 16-colour palettes and the wing-beat would shift colour
every time it crossed from frame 1 to frame 2. The palette is taken over the
WHOLE sheet here, with the transparent-pixel sentinel already folded in, and
each half is written out already quantised against it. This is
tools/split_nessie_sheet.py with a 2x2 layout.

WHY 4bpp: the art was checked against its own 16-colour quantise side by side
(Nov 2026) and the difference is not visible at sprite size, and 4bpp is what
lets each half fit one 64-word page.

THE SLOTS ARE THE CRAWLER'S (and Gula's), the two 64-word blocks at
x[320,384) and x[704,768), y128. Neither is drawn by anything in Chapter 4,
and both are put back on the way out without anyone having to remember to:
main.c restores the crawler / flowers / spiders over x320 and the zombies over
x704 on entry to every room that can hold them, keyed on the DESTINATION, and
the Underworld is left only through the title, whose load passes through that
same block. The CLUT lines (144,480) and (160,480) are the Ahriman's own. See
src/ahriman.h.

  py tools\\split_ahriman_sheet.py          # halves only
  py tools\\split_ahriman_sheet.py --tim    # halves + both .tim at their slots
"""
import os
import subprocess
import sys

from PIL import Image

SRC       = os.path.join('textures', 'underworld', 'ahriman.png')
TOP       = os.path.join('textures', 'underworld', 'ahriman_top.png')
BOT       = os.path.join('textures', 'underworld', 'ahriman_bot.png')
SENTINEL  = (255, 0, 255)   # must match png_to_tim.py's own sentinel
ALPHA_CUT = 128             # must match png_to_tim.py's default

#            out .tim                                         from   tx   ty   cx   cy
TIMS = [(os.path.join('textures', 'underworld', 'ahriman_a.tim'), TOP, 320, 128, 144, 480),
        (os.path.join('textures', 'underworld', 'ahriman_b.tim'), BOT, 704, 128, 160, 480)]


def main():
    im = Image.open(SRC).convert('RGBA')
    if im.size != (256, 256):
        sys.exit('%s is %dx%d; the sheet must be 256x256' % ((SRC,) + im.size))

    # Paint every transparent pixel with the sentinel BEFORE quantising, so the
    # holes claim a palette slot of their own instead of collapsing into
    # whatever opaque colour they happen to sit next to.
    rgb     = im.convert('RGB')
    px_rgb  = rgb.load()
    px_rgba = im.load()
    holes   = []
    for y in range(256):
        for x in range(256):
            if px_rgba[x, y][3] < ALPHA_CUT:
                px_rgb[x, y] = SENTINEL
                holes.append((x, y))

    master = rgb.quantize(colors=16).convert('RGB').convert('RGBA')
    mp     = master.load()
    for (x, y) in holes:
        mp[x, y] = (0, 0, 0, 0)

    for path, y0 in ((TOP, 0), (BOT, 128)):
        half = master.crop((0, y0, 256, y0 + 128))
        n    = len(half.getcolors(1 << 20))
        if n > 16:
            sys.exit('%s came out with %d colours' % (path, n))
        half.save(path)
        print('%s  %d colours' % (path, n))

    if '--tim' in sys.argv[1:]:
        for out, src, tx, ty, cx, cy in TIMS:
            subprocess.check_call([sys.executable,
                                   os.path.join('textures', 'png_to_tim.py'),
                                   src, '--bpp', '4',
                                   '--tx', str(tx), '--ty', str(ty),
                                   '--cx', str(cx), '--cy', str(cy),
                                   '--out', out])


if __name__ == '__main__':
    main()
