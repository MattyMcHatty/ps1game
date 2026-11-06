#!/usr/bin/env python3
"""Split textures/underworld/nessie.png into the two halves the TIM pair is
built from, and (with --tim) convert both.

RUN THIS WHENEVER THE ART CHANGES. Nessie's six frames are one 256x256 sheet,
3x2, read left to right and top row first:

    frames 0, 1, 2  (top row)       the attack - 0/1 over the pull-in, 2 over
    frame  3        (bottom left)    the fireball, 3 the spit and the hold
    frames 4, 5     (bottom middle,  idle / swimming, one a second
                     bottom right)

A 4bpp texture 256 rows tall cannot be placed (V is eight bits, so
y%256 + height <= 256 and a 256-row texture can only start at VRAM y 0 or 256,
both spoken for), so it ships as two 256x128 halves cut on the row boundary,
which is also a frame boundary: NESSIEA is frames 0-2, NESSIEB frames 3-5.

>>> THE TWO HALVES MUST SHARE ONE PALETTE. <<< png_to_tim.py quantises
whatever single image it is handed, so converting each half on its own would
give two independent 16-colour palettes and the attack would shift colour
crossing from frame 2 to frame 3. The palette is taken over the WHOLE sheet
here, with the transparent-pixel sentinel already folded in, and each half is
written out already quantised against it. This is tools/split_adapa_sheet.py
without --stp: nothing fades Nessie, so no CLUT entry needs the blend bit.

  py tools\\split_nessie_sheet.py          # halves only
  py tools\\split_nessie_sheet.py --tim    # halves + both .tim at their slots

THE SLOTS ARE ADAPA'S, pixels and CLUT lines both: x[832,960) y128, CLUTs
(80,480) and (96,480). Adapa exists only in the Library and the Piano Room and
Nessie only in the Underworld, which is reached one way and left only through
the title - so they can never be drawn in the same room. The pages are also the
exit door's two leaves (the Lumberer's second block), and nessies_upload_
textures() calls door_anim_panels_taken() exactly as Adapa's uploader does. See
src/nessie.h.
"""
import os
import subprocess
import sys

from PIL import Image

SRC       = os.path.join('textures', 'underworld', 'nessie.png')
TOP       = os.path.join('textures', 'underworld', 'nessie_top.png')
BOT       = os.path.join('textures', 'underworld', 'nessie_bot.png')
SENTINEL  = (255, 0, 255)   # must match png_to_tim.py's own sentinel
ALPHA_CUT = 128             # must match png_to_tim.py's default

#            out .tim                                     from   tx   ty   cx   cy
TIMS = [(os.path.join('textures', 'underworld', 'nessie_a.tim'), TOP, 832, 128, 80, 480),
        (os.path.join('textures', 'underworld', 'nessie_b.tim'), BOT, 896, 128, 96, 480)]


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
