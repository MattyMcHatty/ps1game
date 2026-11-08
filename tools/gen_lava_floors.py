#!/usr/bin/env python3
"""Generate src/lava_floors.c/.h - the lava that burns the player.

    py tools\\gen_lava_floors.py

Chapter 4's rule: WHEREVER THE LAVA TEXTURE IS ON THE FLOOR, STANDING IN IT
COSTS 5 HP A SECOND and the screen pulses orange (src/lava.c). The game cannot
ask the drawn mesh what texture is under the player at run time - the visual
SMD is a stream of prims, not a floor map - so each Underworld room ships a
BITMASK of LAVA_CELL-unit cells, bit set = burning lava, built here from the
room's VISUAL mesh (the .smx that is drawn, so the burn is exactly the lava the
player sees, not the collision proxy's idea of it).

>>> EVERY UNDERWORLD ROOM WITH LAVA ON ITS FLOOR NEEDS A LINE IN ROOMS BELOW. <<<
Give it the room's GameState, its visual mesh, its collision proxy and the
texture name the lava is on in the visual mesh, run this, rebuild. Nothing else
changes: src/lava.c finds the mask by area. A room with no line simply has no
burning lava, which is right for a room whose lava is out of reach, and WRONG
for one whose lava the player can walk into - see tools/ADDING_A_ROOM.txt.

HOW A CELL IS LAVA:
  1. Its centre is inside a FLAT floor poly on the lava texture.
  2. Its centre is NOT inside a flat non-lava poly at or just above the lava's
     plane (within COVER_RISE): a path, a slab or the Seal drawn over the lava
     is what the player is standing on, and it does not burn. A floor much
     higher than that (a bridge) is handled at run time instead, by the feet
     test in src/lava.c, so it needs no rule here.
  3. Its centre is inside one of the COLLISION PROXY's floor polys: lava the
     player can never stand on (outside the rim of Hell Hath No Fury's
     platform) is dropped, which keeps the mask small. A room whose proxy has
     no floor polys keeps every lava cell.
The mask is cropped to the cells that survive; a room with none is reported and
emitted with an empty mask, so a lava line never silently goes missing.

ONE LAVA PLANE PER ROOM. The lava's y is taken from its polys and a room whose
lava is at two heights fails the script - give the runtime a list of planes
before you build a room like that.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
sys.path.insert(0, HERE)
from gen_nessie_pools import load_smx, ring, inside   # noqa: E402

LAVA_CELL  = 50
COVER_RISE = 50   # a flat floor this far above the lava (-y is up) covers it

#        area                       visual mesh                                     collision proxy                                       lava tex
ROOMS = [('STATE_HELL_HATH_NO_FURY', 'assets/underworld/Hell Hath No Fury.smx',      'assets/underworld/Hell Hath No Fury mesh.smx',      'lava'),
         ('STATE_SHARURS_TRIAL',     'assets/underworld/Sharurs Trial.smx',          'assets/underworld/Sharurs Trial mesh.smx',          'lava'),
         ('STATE_LAVA_U',            'assets/underworld/Lava U.smx',                 'assets/underworld/Lava U mesh.smx',                 'lava'),
         ]


def flat(p):
    return max(v[1] for v in p) - min(v[1] for v in p) < 1


def build(area, smx, proxy, tex):
    texs, polys = load_smx(smx)
    if tex not in texs:
        sys.exit('%s: no texture "%s" in %s' % (area, tex, smx))
    ti = texs.index(tex)
    lava = [p for t, p in polys if t == ti and flat(p)]
    if not lava:
        sys.exit('%s: no flat "%s" floor in %s' % (area, tex, smx))
    ys = sorted(set(round(p[0][1]) for p in lava))
    if len(ys) != 1:
        sys.exit('%s: lava at several heights %s - one plane per room' % (area, ys))
    lava_y = ys[0]

    lava_r  = [ring(p) for p in lava]
    cover_r = [ring(p) for t, p in polys if t != ti and flat(p) and
               lava_y - COVER_RISE <= p[0][1] <= lava_y + 0.5]
    _, ppolys = load_smx(proxy)
    walk_r  = [ring(p) for _, p in ppolys if len(p) >= 3 and flat(p)]

    allx = [x for r in lava_r for x, _ in r]
    allz = [z for r in lava_r for _, z in r]
    x0 = int(min(allx)) // LAVA_CELL * LAVA_CELL
    z0 = int(min(allz)) // LAVA_CELL * LAVA_CELL
    w = (int(max(allx)) - x0 + LAVA_CELL - 1) // LAVA_CELL
    h = (int(max(allz)) - z0 + LAVA_CELL - 1) // LAVA_CELL

    def burns(cx, cz):
        x = x0 + cx * LAVA_CELL + LAVA_CELL // 2
        z = z0 + cz * LAVA_CELL + LAVA_CELL // 2
        if not any(inside(x, z, r) for r in lava_r):
            return False
        if any(inside(x, z, r) for r in cover_r):
            return False
        if walk_r and not any(inside(x, z, r) for r in walk_r):
            return False
        return True

    cells = {(cx, cz) for cz in range(h) for cx in range(w) if burns(cx, cz)}
    if not cells:
        return dict(area=area, smx=smx, y=lava_y, cells=0, x0=0, z0=0,
                    w=0, h=0, stride=0, bits=b'')

    lx = min(c[0] for c in cells); hx = max(c[0] for c in cells)
    lz = min(c[1] for c in cells); hz = max(c[1] for c in cells)
    pw, ph = hx - lx + 1, hz - lz + 1
    stride = (pw + 7) // 8
    bits = bytearray(stride * ph)
    for cx, cz in cells:
        u, v = cx - lx, cz - lz
        bits[v * stride + u // 8] |= 1 << (u % 8)
    return dict(area=area, smx=smx, y=lava_y, cells=len(cells),
                x0=x0 + lx * LAVA_CELL, z0=z0 + lz * LAVA_CELL,
                w=pw, h=ph, stride=stride, bits=bytes(bits))


def main():
    out = [build(*r) for r in ROOMS]

    hdr = ['/* lava_floors.h - GENERATED by tools/gen_lava_floors.py. DO NOT EDIT.',
           ' *',
           ' * The lava that burns the player (src/lava.h): one bitmask per room,',
           ' * LAVA_CELL-unit cells, bit set = lava floor the player can stand in.',
           ' * Regenerate whenever an Underworld room is added (ROOMS in the script)',
           ' * or a room\'s lava changes. */',
           '#ifndef LAVA_FLOORS_H', '#define LAVA_FLOORS_H', '',
           '#include <stdint.h>',
           '#include "title.h"   /* GameState */', '',
           '#define LAVA_CELL %d' % LAVA_CELL, '',
           'typedef struct {',
           '    GameState      area;      /* the room this mask belongs to         */',
           '    int32_t        y;         /* the lava plane (world y, -y is up)    */',
           '    int32_t        x0, z0;    /* world x/z of cell (0,0)\'s low corner */',
           '    int16_t        w, h;      /* cells; 0 = no reachable lava          */',
           '    int16_t        stride;    /* bytes per row of cells                */',
           '    const uint8_t *bits;      /* row-major, bit (u & 7) of byte u >> 3  */',
           '} LavaFloor;', '',
           '#define LAVA_FLOOR_COUNT %d' % len(out), '',
           'extern const LavaFloor lava_floors[LAVA_FLOOR_COUNT];', '',
           '#endif', '']

    src = ['/* lava_floors.c - GENERATED by tools/gen_lava_floors.py. DO NOT EDIT. */',
           '#include "lava_floors.h"', '']
    for p in out:
        name = p['area'][len('STATE_'):].lower()
        if not p['cells']:
            src.append('/* %s: no lava the player can reach */' % p['area'])
            continue
        src.append('/* %s, y=%d: x[%d,%d) z[%d,%d), %dx%d cells, %d burning */'
                   % (p['area'], p['y'], p['x0'], p['x0'] + p['w'] * LAVA_CELL,
                      p['z0'], p['z0'] + p['h'] * LAVA_CELL, p['w'], p['h'], p['cells']))
        src.append('static const uint8_t lava_%s[%d] = {' % (name, len(p['bits'])))
        for r in range(p['h']):
            row = p['bits'][r * p['stride']:(r + 1) * p['stride']]
            src.append('    ' + ', '.join('0x%02x' % b for b in row) + ',')
        src += ['};', '']
    src.append('const LavaFloor lava_floors[LAVA_FLOOR_COUNT] = {')
    for p in out:
        name = p['area'][len('STATE_'):].lower()
        src.append('    { %s, %d, %d, %d, %d, %d, %d, %s },'
                   % (p['area'], p['y'], p['x0'], p['z0'], p['w'], p['h'],
                      p['stride'], ('lava_' + name) if p['cells'] else '0'))
    src += ['};', '']

    open(os.path.join(ROOT, 'src', 'lava_floors.h'), 'w', encoding='utf-8',
         newline='\n').write('\n'.join(hdr))
    open(os.path.join(ROOT, 'src', 'lava_floors.c'), 'w', encoding='utf-8',
         newline='\n').write('\n'.join(src))
    for p in out:
        print('%-24s %5d cells  y=%d  x[%d,%d) z[%d,%d)  %d bytes' % (
            p['area'], p['cells'], p['y'], p['x0'], p['x0'] + p['w'] * LAVA_CELL,
            p['z0'], p['z0'] + p['h'] * LAVA_CELL, len(p['bits'])))
        if '-v' in sys.argv:
            for r in range(p['h'] - 1, -1, -1):
                row = p['bits'][r * p['stride']:(r + 1) * p['stride']]
                print('  ' + ''.join('#' if row[u // 8] >> (u % 8) & 1 else '.'
                                     for u in range(p['w'])))


if __name__ == '__main__':
    main()
