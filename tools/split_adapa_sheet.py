#!/usr/bin/env python3
"""Split textures/adapa.png into the two halves the TIM pair is built from, and
(with --tim) convert both.

RUN THIS WHENEVER THE ART CHANGES. Adapa's four frames are one 256x256 sheet,
one frame per quarter, read left to right and top row first. A 4bpp texture
256 rows tall has the same problem an 8bpp one does - V is eight bits, so
y%256 + height <= 256 and a 256-row texture can only start at VRAM y 0 or 256,
both of which are spoken for - so it ships as two 256x128 halves: ADAPAA is
frames 0-1 (the top row), ADAPAB is frames 2-3 (the bottom row). The cut falls
on the row boundary, which is also a frame boundary.

>>> THE TWO HALVES MUST SHARE ONE PALETTE. <<< png_to_tim.py quantises whatever
single image it is handed, so converting each half on its own would give two
independent 16-colour palettes and the float cycle would shift colour every
time it crossed from frame 1 to frame 2. The palette is taken over the WHOLE
sheet here, with the transparent-pixel sentinel already folded in (so the holes
claim a slot of their own: 15 colours + transparent), and each half is written
out already quantised against it. png_to_tim then sees at most 16 distinct
colours per half and reproduces them exactly. This is
tools/split_lumberer_sheet.py at 4bpp.

  py tools\\split_adapa_sheet.py          # halves only
  py tools\\split_adapa_sheet.py --tim    # halves + both .tim at their slots

--stp IS LOAD-BEARING. He fades out after every hit and on his death, and a
fade here is ADDITIVE blending (tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 5,
TRICK 1). The PS1 blends a textured primitive per TEXEL, and only texels whose
palette entry has bit 15 set, so without it the fade would draw solid until the
frame he vanished. STP has no effect on a primitive that is NOT semi-
transparent, so his ordinary draw is unchanged.

THE SLOTS ARE THE LUMBERER'S SECOND BLOCK, x[832,960) y128, which the lumberer
itself borrowed from the front end (the exit door's two leaves). Adapa only
exists in the Library and the Piano Room, neither of which draws a lumberer or
opens an exit door. See src/adapa.h and the ADAPAA/ADAPAB comment in disc.xml.
"""
import os
import subprocess
import sys

from PIL import Image

SRC       = os.path.join('textures', 'adapa.png')
TOP       = os.path.join('textures', 'adapa_top.png')
BOT       = os.path.join('textures', 'adapa_bot.png')
SENTINEL  = (255, 0, 255)   # must match png_to_tim.py's own sentinel
ALPHA_CUT = 128             # must match png_to_tim.py's default

#            out .tim                    from   tx   ty   cx   cy
TIMS = [(os.path.join('textures', 'adapa_a.tim'), TOP, 832, 128, 80, 480),
        (os.path.join('textures', 'adapa_b.tim'), BOT, 896, 128, 96, 480)]


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
                                   '--stp', '--out', out])


if __name__ == '__main__':
    main()
