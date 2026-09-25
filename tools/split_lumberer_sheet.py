#!/usr/bin/env python3
"""Split textures/catacombs/lumberer.png into the two halves the TIM pair is
built from, and (with --tim) convert both.

RUN THIS WHENEVER THE ART CHANGES. The Lumberer's six frames are one 256x256
sheet and an 8bpp texture 256 rows tall has nowhere legal to sit in this game's
VRAM (y%256 + height <= 256, and both legal starts are spoken for), so it ships
as two 256x128 halves: LUMBERA is images 1-3, LUMBERB is images 4-6. The cut
falls on the row boundary, which is also a frame boundary. See the LUMBERA/
LUMBERB comment in disc.xml for the whole arrangement.

>>> THE TWO HALVES MUST SHARE ONE PALETTE, AND THAT IS THE ONLY REASON THIS
SCRIPT EXISTS. <<< png_to_tim.py quantises whatever single image it is handed.
Run it on each half separately and you get two independent 256-colour palettes,
whereupon the walk cycle's step from image 3 to image 4 - which crosses from one
half to the other - shifts the creature's colour on every other step of every
walk. So the master palette is taken over the WHOLE sheet here, with the
transparent-pixel sentinel already folded in, and each half written out already
quantised against it. png_to_tim then sees at most 256 distinct colours per half
and reproduces them exactly, identically on both sides of the cut.

  py tools\\split_lumberer_sheet.py          # halves only
  py tools\\split_lumberer_sheet.py --tim    # halves + both .tim at their slots

The VRAM slots below are the ones tools/VRAM_MAP_CATACOMBS.txt found and
disc.xml documents; they are on loan from the front end, so do not move them
without reading both.
"""
import os
import subprocess
import sys

from PIL import Image

SRC       = os.path.join('textures', 'catacombs', 'lumberer.png')
TOP       = os.path.join('textures', 'catacombs', 'lumberer_top.png')
BOT       = os.path.join('textures', 'catacombs', 'lumberer_bot.png')
SENTINEL  = (255, 0, 255)   # must match png_to_tim.py's own sentinel
ALPHA_CUT = 128             # must match png_to_tim.py's default

#            out .tim                       from   tx   ty   cx   cy
TIMS = [(os.path.join('textures', 'lumberer_a.tim'), TOP, 448, 128, 256, 495),
        (os.path.join('textures', 'lumberer_b.tim'), BOT, 832, 128, 672, 486)]


def main():
    im = Image.open(SRC).convert('RGBA')
    if im.size != (256, 256):
        sys.exit('%s is %dx%d; the sheet must be 256x256' % ((SRC,) + im.size))

    # Paint every transparent pixel with the sentinel BEFORE quantising, so the
    # holes claim a palette slot of their own instead of collapsing into
    # whatever opaque colour they happen to sit next to.
    rgb      = im.convert('RGB')
    px_rgb   = rgb.load()
    px_rgba  = im.load()
    holes    = []
    for y in range(256):
        for x in range(256):
            if px_rgba[x, y][3] < ALPHA_CUT:
                px_rgb[x, y] = SENTINEL
                holes.append((x, y))

    master = rgb.quantize(colors=256).convert('RGB').convert('RGBA')
    mp     = master.load()
    for (x, y) in holes:
        mp[x, y] = (0, 0, 0, 0)

    for path, y0 in ((TOP, 0), (BOT, 128)):
        half = master.crop((0, y0, 256, y0 + 128))
        n    = len(half.getcolors(1 << 20))
        if n > 256:
            sys.exit('%s came out with %d colours' % (path, n))
        half.save(path)
        print('%s  %d colours' % (path, n))

    if '--tim' in sys.argv[1:]:
        for out, src, tx, ty, cx, cy in TIMS:
            subprocess.check_call([sys.executable,
                                   os.path.join('textures', 'png_to_tim.py'),
                                   src, '--bpp', '8',
                                   '--tx', str(tx), '--ty', str(ty),
                                   '--cx', str(cx), '--cy', str(cy),
                                   '--out', out])


if __name__ == '__main__':
    main()
