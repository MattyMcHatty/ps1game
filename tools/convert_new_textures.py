#!/usr/bin/env python3
"""
convert_new_textures.py - turn the reworked art in textures/new textures/ into
4bpp TIMs, in place of the TIMs it replaces.

    py tools\\convert_new_textures.py            # convert everything in TABLE
    py tools\\convert_new_textures.py --check    # report only, write nothing

WHY NOT png_to_tim.py
---------------------
png_to_tim quantises: it runs median cut to 16 colours even when the source
already HAS 16 or fewer, and median cut is free to merge or nudge them. This
art was hand-reduced, so the palette is taken from the image EXACTLY - every
distinct colour gets its own CLUT entry - and an image with more than 16 is
refused instead of being quietly squeezed. That refusal is the point: an image
that comes out with 17 colours was not reduced properly and goes back to the
artist.

Transparency follows png_to_tim's rules (alpha below 128 is a hole, CLUT entry
0 is 0x0000 and the GPU skips it; opaque black is 0x8000 so it is NOT a hole),
and colours are compared AFTER the drop to 5 bits a channel, because two 8-bit
colours that land on the same 15-bit value are one colour on the PS1.

WHERE EACH ONE GOES
-------------------
By default a texture keeps the VRAM rect and CLUT slot of the TIM it replaces
(read out of that TIM), so nothing that addresses it has to change. The table
overrides that where the 4bpp version cannot sit there - see the notes beside
those rows, and "new textures/CHECKLIST.md" for the whole job.

A 4bpp texture is a quarter-width in VRAM words (four texels to a word) and its
U is still in texels, so a page-aligned texture keeps every UV it had. One that
is NOT page-aligned does not: its U offset in the page doubles. That is why the
pickup sprites move (section 3 of the checklist).
"""
import os
import struct
import sys

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
NEW = os.path.join(ROOT, 'textures', 'new textures')
TEX = os.path.join(ROOT, 'textures')
ALPHA_CUT = 128

# (source png under new textures/, target tim under textures/, overrides)
# overrides: tx, ty (VRAM word coords), cx, cy (CLUT), crop (x0, y0, x1, y1)
TABLE = [
    # ---- 1. Straight swaps: same rect, same CLUT --------------------------
    ('128x128/arms2.png',               'arms2.tim', {}),
    ('128x128/bed.png',                 'bed.tim', {}),
    ('128x128/bones.png',               'bones.tim', {}),
    ('128x128/catacomb inner door.png', 'catacomb inner door.tim', {}),
    ('128x128/cobblestones.png',        'cobblestones.tim', {}),
    ('128x128/crib.png',                'crib.tim', {}),
    ('128x128/drain.png',               'drain.tim', {}),
    ('128x128/dresser.png',             'dresser.tim', {}),
    ('128x128/flowerbed.png',           'flowerbed.tim', {}),
    ('128x128/fountain.png',            'fountain.tim', {}),
    ('128x128/gaol_mud.png',            'catacombs/gaol_mud.tim', {}),
    ('128x128/grdn_gte.png',            'grdn_gte.tim', {}),
    ('128x128/greenhouse.png',          'greenhouse.tim', {}),
    ('128x128/hatch.png',               'hatch.tim', {}),
    ('128x128/heads.png',               'heads.tim', {}),
    ('128x128/hedge.png',               'hedge.tim', {}),
    ('128x128/kchn_wl.png',             'kchn_wl.tim', {}),
    ('128x128/ladder.png',              'ladder.tim', {}),
    ('128x128/lamashtu tablet.png',     'lamashtu tablet.tim', {}),
    ('128x128/legs.png',                'legs.tim', {}),
    ('128x128/loculus.png',             'loculus.tim', {}),
    ('128x128/oil_container.png',       'oil_container.tim', {}),
    ('128x128/pipe.png',                'pipe.tim', {}),
    ('128x128/plinth.png',              'plinth.tim', {}),
    ('128x128/plinth_diamond.png',      'plinth_diamond.tim', {}),
    ('128x128/rafflesia1.png',          'rafflesia1.tim', {}),
    ('128x128/rafflesia2.png',          'rafflesia2.tim', {}),
    ('128x128/rusty.png',               'rusty.tim', {}),
    ('128x128/stables wood.png',        'stables wood.tim', {}),
    ('128x128/stn_gls.png',             'stn_gls.tim', {}),
    ('128x128/torsos.png',              'torsos.tim', {}),
    ('128x128/trck_clue.png',           'trck_clue.tim', {}),
    ('128x128/vines.png',               'vines.tim', {}),
    ('32x32/cppr_pt.png',               'cppr_pt.tim', {}),
    ('32x32/key.png',                   'key.tim', {}),
    ('32x32/sml_med.png',               'sml_med.tim', {}),   # 64x64 -> 32x32
    ('64x64/creep.png',                 'creep.tim', {}),
    ('64x64/crucifaxe.png',             'crucifaxe.tim', {}),
    ('64x64/poison_flower_base.png',    'poison_flower_base.tim', {}),
    ('Other/ghit.png',                  'ghit.tim', {}),
    ('Other/ls_atk.png',                'ls_atk.tim', {}),
    ('Other/ls_idle.png',               'ls_idle.tim', {}),
    ('Other/shadow.png',                'shadow.tim', {}),
    ('Other/zombie_alert.png',          'zombie_alert.tim', {}),
    ('Other/zombie_sleep.png',          'zombie_sleep.tim', {}),

    # ---- 2. 16bpp -> 4bpp: same rect, a CLUT slot of their own ------------
    # Both are loaded at startup and resident everywhere, so the slots are ones
    # no TIM in any room claims (x[112,144) of row 480).
    ('128x128/wd_dr.png',               'wd_dr.tim',   {'cx': 112, 'cy': 480}),
    ('64x64/wdcrate.png',               'wdcrate.tim', {'cx': 128, 'cy': 480}),

    # ---- 3. Pickup sprites, moved ------------------------------------------
    # item_pickup.c draws these under the room's 128x128 texture window with no
    # bracket, so they need U < 128 and V % 256 < 128. Their old slots are the
    # RIGHT half of a page, which at 4bpp is U 128..255. These are left-half
    # cells the 16bpp crate/door textures used to cover, free in every room.
    ('64x64/graveolver.png',            'graveolver.tim', {'tx': 912, 'ty': 0}),
    ('32x32/hatch_key.png',             'hatch_key.tim',  {'tx': 896, 'ty': 80}),
    ('32x32/pno_key.png',               'pno_key.tim',    {'tx': 904, 'ty': 96}),
    ('32x32/bl_ky_stn.png',             'bl_ky_stn.tim',  {'tx': 920, 'ty': 96}),
    ('32x32/ylw_ky_stn.png',            'ylw_ky_stn.tim', {'tx': 648, 'ty': 256}),
    # Second batch: the left half of page 832 at y256, which the Anzu tablet
    # gave up (see anzu_tab below).
    ('64x64/stnd_rnds.png',             'stnd_rnds.tim',    {'tx': 832, 'ty': 256}),
    ('64x64/flm_rnds.png',              'flm_rnds.tim',     {'tx': 848, 'ty': 256}),
    ('64x64/helluminator.png',          'helluminator.tim', {'tx': 832, 'ty': 320}),
    ('32x32/mgn_ky_stn.png',            'mgn_ky_stn.tim',   {'tx': 848, 'ty': 320}),

    # Menu icon only: menu.c resets the texture window before its icons, so a
    # right-half slot (U 128+) is fine and it stays where it was.
    ('64x64/valve handle.png',          'vlv_hndl.tim', {}),

    # Hadad: 64x96 -> 128x128, too tall for the old slots (they would run into
    # the CLUT band at y480). hadad.c brackets its own texture window and reads
    # its UVs from the TIM, so any slot free in every room will do; these three
    # are (the right half of page 576 at both Voffs, and of page 896 at y0).
    ('128x128/hadad_idle.png',          'hadad_idle_64.tim', {'tx': 608, 'ty': 0}),
    ('128x128/hadad_stp1.png',          'hadad_stp1_64.tim', {'tx': 928, 'ty': 0}),
    ('128x128/hadad_stp2.png',          'hadad_stp2_64.tim', {'tx': 608, 'ty': 128}),

    # ---- 4. Rework -----------------------------------------------------------
    # The whole relief in one texture, cut into the six tiles by UV
    # (src/anzu_tex.c). The tiles draw under the piano room's 128 window, so the
    # texture must sit at U < 128 and Voff 0: the left half of page 832 at y0.
    # That slot is ROOM ART (double_door, drain, plinth, ... all texmgr-owned,
    # none of it used in the piano room), which is fine because anzu_tex_stream
    # re-reads the tablet on every piano-room entry and every owner is
    # re-uploaded on entry to its own room. CLUT on anzu1's old row.
    ('128x128/anzu_tab.png',            'anzu_tab.tim', {'tx': 832, 'ty': 0,
                                                         'cx': 256, 'cy': 483}),
]

# Sheets too tall to upload whole (a 256-row texture needs a whole free tpage,
# and Chapter 3 has none), cut on the frame row boundary into two halves that
# keep the slots of the 8bpp halves they replace. Both halves get ONE palette
# taken over the whole sheet, so a walk cycle crossing the cut keeps its colours.
SHEETS = [
    ('256x256/crawler.png',  ['crawler_a.tim', 'crawler_b.tim']),
    ('256x256/lumberer.png', ['lumberer_a.tim', 'lumberer_b.tim']),
]


def to_ps1(r, g, b):
    r5, g5, b5 = r >> 3, g >> 3, b >> 3
    if r5 == 0 and g5 == 0 and b5 == 0:
        return 0x8000            # opaque black; 0x0000 would be a hole
    return (b5 << 10) | (g5 << 5) | r5


def ps1_pixels(im):
    """Each pixel as its PS1 colour, or None for a hole."""
    im = im.convert('RGBA')
    get = getattr(im, 'get_flattened_data', im.getdata)
    return [None if a < ALPHA_CUT else to_ps1(r, g, b) for (r, g, b, a) in get()]


def make_palette(pixels):
    """Exact palette: entry 0 is the hole if there is one, then every distinct
    colour in first-seen order. Returns (palette, index_of) or raises."""
    has_hole = any(p is None for p in pixels)
    colours = []
    seen = set()
    for p in pixels:
        if p is not None and p not in seen:
            seen.add(p)
            colours.append(p)
    pal = ([0x0000] if has_hole else []) + colours
    if len(pal) > 16:
        raise ValueError('%d colours%s' % (len(colours), ' + transparency' if has_hole else ''))
    pal += [0x0000] * (16 - len(pal))
    index_of = {c: i for i, c in enumerate(pal) if c != 0x0000}
    if has_hole:
        index_of[None] = 0
    return pal, index_of


def read_tim_slots(path):
    d = open(path, 'rb').read()
    flag = struct.unpack_from('<I', d, 4)[0]
    off = 8
    cx = cy = None
    if flag & 8:
        bl, cx, cy, cw, ch = struct.unpack_from('<IHHHH', d, off)
        off += bl
    bl, tx, ty, tw, th = struct.unpack_from('<IHHHH', d, off)
    return dict(tx=tx, ty=ty, cx=cx, cy=cy)


def write_tim4(path, w, h, idx, pal, tx, ty, cx, cy):
    assert w % 4 == 0, 'a 4bpp width must be a multiple of 4'
    data = bytearray()
    for y in range(h):
        for x in range(0, w, 2):
            data.append((idx[y * w + x] & 15) | ((idx[y * w + x + 1] & 15) << 4))
    with open(path, 'wb') as f:
        f.write(struct.pack('<II', 0x10, 0x08))                 # magic, 4bpp + CLUT
        f.write(struct.pack('<IHHHH', 12 + 32, cx, cy, 16, 1))
        f.write(struct.pack('<16H', *pal))
        f.write(struct.pack('<IHHHH', 12 + len(data), tx, ty, w // 4, h))
        f.write(data)


def convert(src, dst, ov, check, pal_pixels=None, crop=None):
    im = Image.open(src).convert('RGBA')
    if crop:
        im = im.crop(crop)
    px = ps1_pixels(im)
    pal, index_of = make_palette(pal_pixels if pal_pixels is not None else px)
    slots = read_tim_slots(dst) if os.path.exists(dst) else {}
    slots.update({k: v for k, v in ov.items() if k in ('tx', 'ty', 'cx', 'cy')})
    missing = [k for k in ('tx', 'ty', 'cx', 'cy') if slots.get(k) is None]
    if missing:
        raise ValueError('no %s for %s' % (','.join(missing), os.path.basename(dst)))
    used = sum(1 for c in pal if c) + (1 if None in index_of else 0)
    line = '%-26s %3dx%-3d  %2d colours  VRAM (%d,%d)  CLUT (%d,%d)' % (
        os.path.basename(dst), im.width, im.height, used,
        slots['tx'], slots['ty'], slots['cx'], slots['cy'])
    if not check:
        write_tim4(dst, im.width, im.height, [index_of[p] for p in px], pal,
                   slots['tx'], slots['ty'], slots['cx'], slots['cy'])
    print(line)


def main():
    check = '--check' in sys.argv[1:]
    bad = []
    for rel, tim, ov in TABLE:
        try:
            convert(os.path.join(NEW, rel), os.path.join(TEX, tim), ov, check)
        except ValueError as e:
            bad.append((rel, str(e)))
    for rel, tims in SHEETS:
        src = os.path.join(NEW, rel)
        sheet = Image.open(src).convert('RGBA')
        try:
            whole = ps1_pixels(sheet)
            make_palette(whole)
            half = sheet.height // 2
            for k, tim in enumerate(tims):
                convert(src, os.path.join(TEX, tim), {}, check, pal_pixels=whole,
                        crop=(0, k * half, sheet.width, (k + 1) * half))
        except ValueError as e:
            bad.append((rel, str(e)))
    if bad:
        print('\nREFUSED (too many colours for 4bpp):')
        for rel, why in bad:
            print('  %-30s %s' % (rel, why))
        sys.exit(1)


if __name__ == '__main__':
    main()
