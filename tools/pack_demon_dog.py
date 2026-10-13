#!/usr/bin/env python3
"""
pack_demon_dog.py - turn the authored "demon dog" sheet into the strip the game loads.

    py tools/pack_demon_dog.py
    py textures/png_to_tim.py textures/demon_dog_strip.png --bpp 8 --tx 640 --ty 384 \
           --cx 0 --cy 483 --out textures/demon_dog.tim

THE SOURCE, textures/demon dog.png, is 128x128 with a 64x64 frame in each corner:

    top-left     ACTIVE, frame 1      top-right    ACTIVE, frame 2
    bottom-left  IDLE                 bottom-right unused

THE STRIP is those three frames side by side, 192x64, in that order:

    u   0..63   ACTIVE 1     u  64..127  ACTIVE 2     u 128..191  IDLE

WHY NOT LOAD THE SQUARE AS AUTHORED: at 8bpp a 128-wide texture is 64 VRAM words,
and the only place the dog can go - x640 y384, where its three old frames were -
would run x[640,704) y[384,512). Rows 480..511 of x[672,704) are CLUT band: some
forty other textures keep their palettes there (tools/VRAM_MAP.txt's overlap
list), and every one of those uploads would land in the dog. The old frames were
32 words wide and stopped at x672. The strip is 96 words by 64 rows,
x[640,736) y[384,448), clear of the band in every chapter.

The frame corners in the strip are DDOG_ACTIVE1_U and kin in src/demondog.c.
Repack and re-convert after any edit to the source.
"""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, 'textures', 'demon dog.png')
OUT  = os.path.join(ROOT, 'textures', 'demon_dog_strip.png')

FRAMES = [(0, 0), (64, 0), (0, 64)]   # active 1, active 2, idle: (x, y) in the source

src = Image.open(SRC).convert('RGBA')
assert src.size == (128, 128), 'expected a 128x128 sheet, got %r' % (src.size,)
strip = Image.new('RGBA', (64 * len(FRAMES), 64), (0, 0, 0, 0))
for i, (x, y) in enumerate(FRAMES):
    strip.paste(src.crop((x, y, x + 64, y + 64)), (64 * i, 0))
strip.save(OUT)
print('wrote %s (%dx%d)' % (OUT, strip.size[0], strip.size[1]))
