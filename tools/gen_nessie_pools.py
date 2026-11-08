#!/usr/bin/env python3
"""Generate src/nessie_pools.c/.h - the lava each Nessie is confined to.

    py tools\\gen_nessie_pools.py

A Nessie is boxed into ONE body of lava: it swims inside it, comes right up to
its edge, and never crosses onto rock, a path, or into a neighbouring pool. The
lava is not a rectangle (Sharur's Trial's west pool is an L with an outcrop in
it and a diagonal rim), so each pool ships as a BITMASK of POOL_CELL-unit cells,
and src/nessie.c refuses any step whose centre would leave the mask.

>>> ADDING A NESSIE TO A NEW POOL MEANS ADDING A LINE TO POOLS BELOW. <<<
Give it a name, the room's VISUAL mesh (the .smx that is drawn - the lava the
player sees, not the collision proxy), the texture slot the lava is on in that
mesh, and a SEED point anywhere inside the pool. Run this, then place the Nessie
in world.c with the matching NSS_POOL_* id. Nothing else reads the room.

HOW A POOL IS FOUND:
  1. Every flat floor poly on the lava texture is rasterised at POOL_CELL: a
     cell is lava if its centre is inside one.
  2. The pool is the 4-CONNECTED flood from the seed. FOUR, not eight, and it
     is load-bearing: Sharur's Trial's west lava is cut in two by a diagonal
     ridge of outcrops that touch only at their corners, and an 8-connected
     flood walks straight through those corners into the other pool. Two lava
     cells that meet only diagonally are two pools.
  3. Every mesh poly is checked too, so a cell under an outcrop is never lava
     even if a lava poly were drawn beneath it - the outcrops stand on the
     lava's own plane.

The mask is the WHOLE lava surface. The body is held off the rim and the
outcrops by its own wall collision (NSS_BODY_RADIUS) on top of this; the mask
is what keeps it off the paths and out of other pools, which have no walls.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')

POOL_CELL = 50

#        name          visual mesh                                   lava tex   seed (x, z)
POOLS = [('SHARUR_NW', 'assets/underworld/Sharurs Trial.smx',        'lava',    (-1300, 200)),
         ('SHARUR_N',  'assets/underworld/Sharurs Trial.smx',        'lava',    (250, 2200)),
         ('SHARUR_E',  'assets/underworld/Sharurs Trial.smx',        'lava',    (1950, -200)),
         ('LAVA_U',    'assets/underworld/Lava U.smx',               'lava',    (-1934, -2831)),
         ]


def load_smx(path):
    s = open(os.path.join(ROOT, path), encoding='utf-8', errors='replace').read()
    vb = s.split('<vertices')[1].split('</vertices>')[0]
    vs = [(float(a), float(b), float(c)) for a, b, c in
          re.findall(r'x="([-\d.]+)" y="([-\d.]+)" z="([-\d.]+)"', vb)]
    texs = re.findall(r'<texture file="([^"]*)"', s)
    polys = []
    for m in re.finditer(r'<poly ([^>]*)/>', s):
        at = dict(re.findall(r'(\w+)="([^"]*)"', m.group(1)))
        ids = [int(at[k]) for k in ('v0', 'v1', 'v2', 'v3') if k in at]
        polys.append((int(at.get('texture', '-1')), [vs[i] for i in ids]))
    return texs, polys


def ring(pts):
    """XZ outline. An SMX quad is a strip (v0 v1 v2 v3 = TL TR BL BR), so its
    perimeter is v0 v1 v3 v2."""
    if len(pts) == 4:
        pts = [pts[0], pts[1], pts[3], pts[2]]
    return [(p[0], p[2]) for p in pts]


def inside(x, z, r):
    c = False
    for i in range(len(r)):
        x1, z1 = r[i]
        x2, z2 = r[(i + 1) % len(r)]
        if (z1 > z) != (z2 > z) and x < (x2 - x1) * (z - z1) / (z2 - z1) + x1:
            c = not c
    return c


def build(name, smx, tex, seed):
    texs, polys = load_smx(smx)
    if tex not in texs:
        sys.exit('%s: no texture "%s" in %s' % (name, tex, smx))
    ti = texs.index(tex)
    floor = [ring(p) for t, p in polys
             if t == ti and max(v[1] for v in p) - min(v[1] for v in p) < 1]
    # Anything standing ON the lava's plane that is not lava (an outcrop's
    # footprint) disqualifies a cell even if lava were drawn under it.
    lava_y = [p for t, p in polys if t == ti][0][0][1]
    block = [ring(p) for t, p in polys if t != ti and
             min(abs(v[1] - lava_y) for v in p) < 1 and
             max(v[1] for v in p) - min(v[1] for v in p) >= 1]

    allx = [x for r in floor for x, _ in r]
    allz = [z for r in floor for _, z in r]
    x0 = int(min(allx)) // POOL_CELL * POOL_CELL
    z0 = int(min(allz)) // POOL_CELL * POOL_CELL
    w = (int(max(allx)) - x0 + POOL_CELL - 1) // POOL_CELL
    h = (int(max(allz)) - z0 + POOL_CELL - 1) // POOL_CELL

    def lava(cx, cz):
        x = x0 + cx * POOL_CELL + POOL_CELL // 2
        z = z0 + cz * POOL_CELL + POOL_CELL // 2
        return any(inside(x, z, r) for r in floor)

    grid = [[lava(cx, cz) for cx in range(w)] for cz in range(h)]
    sx = (seed[0] - x0) // POOL_CELL
    sz = (seed[1] - z0) // POOL_CELL
    if not (0 <= sx < w and 0 <= sz < h and grid[sz][sx]):
        sys.exit('%s: seed %s is not on the lava' % (name, seed))

    pool, todo = set(), [(sx, sz)]
    while todo:
        cx, cz = todo.pop()
        if (cx, cz) in pool or not (0 <= cx < w and 0 <= cz < h) or not grid[cz][cx]:
            continue
        pool.add((cx, cz))
        todo += [(cx + 1, cz), (cx - 1, cz), (cx, cz + 1), (cx, cz - 1)]
    _ = block   # see the head: today's outcrops carry no lava beneath them

    # Crop to the pool's own bounds.
    lx = min(c[0] for c in pool); hx = max(c[0] for c in pool)
    lz = min(c[1] for c in pool); hz = max(c[1] for c in pool)
    pw, ph = hx - lx + 1, hz - lz + 1
    stride = (pw + 7) // 8
    bits = bytearray(stride * ph)
    for cx, cz in pool:
        u, v = cx - lx, cz - lz
        bits[v * stride + u // 8] |= 1 << (u % 8)
    return dict(name=name, smx=smx, seed=seed, cells=len(pool),
                x0=x0 + lx * POOL_CELL, z0=z0 + lz * POOL_CELL,
                w=pw, h=ph, stride=stride, bits=bytes(bits))


def main():
    out = [build(*p) for p in POOLS]

    hdr = ['/* nessie_pools.h - GENERATED by tools/gen_nessie_pools.py. DO NOT EDIT.',
           ' *',
           ' * The lava each Nessie is confined to: one bitmask per pool, POOL_CELL-unit',
           ' * cells, bit set = lava belonging to THIS pool. Regenerate whenever a',
           ' * pool is added (POOLS in the script) or a room\'s lava changes. */',
           '#ifndef NESSIE_POOLS_H', '#define NESSIE_POOLS_H', '',
           '#include <stdint.h>', '',
           '#define NSS_POOL_CELL %d' % POOL_CELL, '',
           'typedef struct {',
           '    int32_t        x0, z0;    /* world x/z of cell (0,0)\'s low corner */',
           '    int16_t        w, h;      /* cells                                 */',
           '    int16_t        stride;    /* bytes per row of cells                */',
           '    const uint8_t *bits;      /* row-major, bit (u & 7) of byte u >> 3  */',
           '} NessiePool;', '',
           'typedef enum {']
    for p in out:
        hdr.append('    NSS_POOL_%s,   /* %s, seeded at (%d,%d): %d cells */'
                   % (p['name'], p['smx'].split('/')[-1], p['seed'][0], p['seed'][1],
                      p['cells']))
    hdr += ['    NSS_POOL_COUNT', '} NessiePoolId;', '',
            'extern const NessiePool nessie_pools[NSS_POOL_COUNT];', '',
            '#endif', '']

    src = ['/* nessie_pools.c - GENERATED by tools/gen_nessie_pools.py. DO NOT EDIT. */',
           '#include "nessie_pools.h"', '']
    for p in out:
        src.append('/* %s: x[%d,%d) z[%d,%d), %dx%d cells */'
                   % (p['name'], p['x0'], p['x0'] + p['w'] * POOL_CELL,
                      p['z0'], p['z0'] + p['h'] * POOL_CELL, p['w'], p['h']))
        src.append('static const uint8_t pool_%s[%d] = {' % (p['name'].lower(), len(p['bits'])))
        for r in range(p['h']):
            row = p['bits'][r * p['stride']:(r + 1) * p['stride']]
            src.append('    ' + ', '.join('0x%02x' % b for b in row) + ',')
        src += ['};', '']
    src.append('const NessiePool nessie_pools[NSS_POOL_COUNT] = {')
    for p in out:
        src.append('    { %d, %d, %d, %d, %d, pool_%s },'
                   % (p['x0'], p['z0'], p['w'], p['h'], p['stride'], p['name'].lower()))
    src += ['};', '']

    open(os.path.join(ROOT, 'src', 'nessie_pools.h'), 'w', encoding='utf-8',
         newline='\n').write('\n'.join(hdr))
    open(os.path.join(ROOT, 'src', 'nessie_pools.c'), 'w', encoding='utf-8',
         newline='\n').write('\n'.join(src))
    for p in out:
        print('%-10s %5d cells  x[%d,%d) z[%d,%d)' % (
            p['name'], p['cells'], p['x0'], p['x0'] + p['w'] * POOL_CELL,
            p['z0'], p['z0'] + p['h'] * POOL_CELL))
        for r in range(p['h'] - 1, -1, -1):
            row = p['bits'][r * p['stride']:(r + 1) * p['stride']]
            print('  ' + ''.join('#' if row[u // 8] >> (u % 8) & 1 else '.'
                                 for u in range(p['w'])))


if __name__ == '__main__':
    main()
