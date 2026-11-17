#!/usr/bin/env python3
"""Split textures/underworld/golem.png into the two halves the TIM pair is
built from, and (with --tim) convert both.

RUN THIS WHENEVER THE ART CHANGES. The Golem's six frames are one 256x256
sheet, 3x2, numbered as the design numbers them - top row 1 2 3, bottom row
4 5 6:

    frames 1, 2, 3  (top row)      GOLEMA   idle A, laser glow, laser fire
    frames 4, 5, 6  (bottom row)   GOLEMB   punch raise, punch down, idle B

The three columns are NOT equal: 256 does not divide by three, and the art is
cut at x 0 / 85 / 171 / 256 (widths 85, 86, 85). src/golem.c's GLM_COL_U0 /
GLM_COL_W restate that; change both together.

A 4bpp texture 256 rows tall cannot be placed (V is eight bits, so a 256-row
texture can only start at VRAM y 0 or 256, both spoken for), so it ships as two
256x128 halves cut on the row boundary, which is also a frame boundary - the
Ahriman's split (tools/split_ahriman_sheet.py), which this is.

>>> THE TWO HALVES MUST SHARE ONE PALETTE. <<< The idle loop is frame 1 <-> 6,
which crosses halves; two independent palettes would shift its colour on every
breath. The palette is taken over the WHOLE sheet with the transparent-pixel
sentinel folded in, and each half is written already quantised against it.

WHY 4bpp: the source art is already 15 colours, so a 16-colour quantise is
lossless.

THE SLOTS are the SECOND halves of the crawler's two blocks, x[384,448) and
x[768,832) at y128 - the Ahriman has the first halves (x320 / x704). Their
Chapter 1-3 tenants (spdr_wk, rafflesia2, gula_a/b, zombie_alert) are put back
by main.c's own destination-keyed STATE_LOADING lines on entry to every room
that draws them, and the Underworld is left only through the title, whose load
passes through that same block - the Ahriman's argument, word for word. The
CLUT lines (176,480) and (192,480) are the Golem's own. See src/golem.h.

  py tools\\split_golem_sheet.py          # halves only
  py tools\\split_golem_sheet.py --tim    # halves + both .tim at their slots
"""
import os
import subprocess
import sys

from PIL import Image

SRC       = os.path.join('textures', 'underworld', 'golem.png')
TOP       = os.path.join('textures', 'underworld', 'golem_top.png')
BOT       = os.path.join('textures', 'underworld', 'golem_bot.png')
SENTINEL  = (255, 0, 255)   # must match png_to_tim.py's own sentinel
ALPHA_CUT = 128             # must match png_to_tim.py's default

#            out .tim                                         from   tx   ty   cx   cy
TIMS = [(os.path.join('textures', 'underworld', 'golem_a.tim'), TOP, 384, 128, 176, 480),
        (os.path.join('textures', 'underworld', 'golem_b.tim'), BOT, 768, 128, 192, 480)]


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
