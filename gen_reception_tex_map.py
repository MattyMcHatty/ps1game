"""
gen_reception_tex_map.py — generate src/reception_tex_map.h from the Reception SMX.

Each <poly> in the SMX carries texture="N", where N indexes the SMX's own
<textures> list. That list is renumbered by the Blender exporter whenever the
material set changes (e.g. dropping a texture shifts every later index), so a
raw-index map silently mis-assigns textures after a re-export.

To be robust, we map per-poly by texture NAME: read the SMX <textures> list to
resolve N -> name, then look the name up in the engine's fixed VRAM slot order
(NAME_TO_SLOT below). The poly order in the SMX matches the SMD primitive order
after smxlink, so reception_tex_map[i] lines up with reception_smd prim i.

NAME_TO_SLOT MUST match the slot order loaded in src/reception.c
(reception_load_assets / new_tex[] + TIM_SLOT calls). If you add/move a
texture slot there, update it here too.

The room's model is "Reception v2.smx" (the remodelled export); assets/Reception.smx
is the superseded original, kept only for reference. Both convert to Reception.smd,
which is what disc.xml packs as RECEPT.SMD.

Usage:
    python gen_reception_tex_map.py            # assets/Reception v2.smx -> src/reception_tex_map.h
    python gen_reception_tex_map.py <smx> <out>
"""
import xml.etree.ElementTree as ET
import sys

SMX = sys.argv[1] if len(sys.argv) > 1 else 'assets/Reception v2.smx'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'src/reception_tex_map.h'

# Engine VRAM slot for each texture name (must match src/reception.c load order).
NAME_TO_SLOT = {
    'strs':       0,
    'red_wlppr':  1,
    'wd_flr':     2,
    # slot 3 retired (was bnnstr, the banister — no longer referenced)
    'din_cl':     4,
    'frnt_dr':    5,
    'wd_dr':      6,
    'inr_dbl_dr': 7,
    'stn_gls':    8,
}
UNTEXTURED = 0xFF

root = ET.parse(SMX).getroot()

smx_tex = [t.get('file') for t in root.find('textures').findall('texture')]
print("SMX textures: " + ", ".join(f"{i}={n}" for i, n in enumerate(smx_tex)))

verts = [(float(v.get('x')), float(v.get('y')), float(v.get('z')))
         for v in root.find('vertices').findall('v')]

def _collinear(a, b, c):
    ux, uy, uz = b[0]-a[0], b[1]-a[1], b[2]-a[2]
    vx, vy, vz = c[0]-a[0], c[1]-a[1], c[2]-a[2]
    cx, cy, cz = uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx
    return (cx*cx + cy*cy + cz*cz) < 1e-3

entries = []   # tex slot per prim
nocull  = []   # 1 = do NOT backface-cull (degenerate "triangle-shaped" quad)
for p in root.find('primitives').findall('poly'):
    n = p.get('texture')
    if n is None:
        entries.append(UNTEXTURED)
    else:
        name = smx_tex[int(n)]
        if name not in NAME_TO_SLOT:
            sys.exit(f"ERROR: SMX texture '{name}' has no engine slot in NAME_TO_SLOT "
                     f"(update both this script and src/reception.c).")
        entries.append(NAME_TO_SLOT[name])

    # A quad with three collinear verts renders as a triangle; gte_nclip on its
    # degenerate first triangle is ~0 and flickers the backface cull. Flag it so
    # the renderer never culls it. Triangles (no v3) are handled by the renderer's
    # is_quad check, so leave them 0.
    deg = 0
    if p.get('v3') is not None:
        q = [verts[int(p.get(f'v{k}'))] for k in range(4)]
        if any(_collinear(q[a], q[b], q[c])
               for a, b, c in ((0, 1, 2), (0, 1, 3), (0, 2, 3), (1, 2, 3))):
            deg = 1
    nocull.append(deg)

def _warn_duplicate_faces():
    """Report same-facing coplanar faces that overlap — a pure z-fighting source.

    The PS1 has no depth buffer, so two faces sharing a plane and facing the same
    way flicker against each other ("criss-crossing" polys that look like they
    clip). Back-to-back pairs (opposite facing) are fine — the backface cull
    drops one — so only same-facing overlaps are reported.
    """
    from collections import defaultdict

    def cross(a, b, c):
        u = [b[i]-a[i] for i in range(3)]
        v = [c[i]-a[i] for i in range(3)]
        n = [u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]]
        L = sum(x*x for x in n) ** 0.5
        return [x/L for x in n] if L > 1e-9 else None

    planes = defaultdict(list)
    for pi, p in enumerate(root.find('primitives').findall('poly')):
        idx = [int(p.get(f'v{k}')) for k in range(4) if p.get(f'v{k}') is not None]
        pts = [verts[i] for i in idx]
        n = cross(*pts[:3])
        if n is None:
            continue
        # Unsigned plane key, but keep the true facing so back-to-back pairs
        # can be told apart from real duplicates.
        key_n = n if n > [-x for x in n] else [-x for x in n]
        facing = 1 if sum(key_n[i]*n[i] for i in range(3)) > 0 else -1
        d = sum(key_n[i]*pts[0][i] for i in range(3))
        # Project onto the plane so overlap is a plain 2D test.
        ax = [1, 0, 0] if abs(key_n[0]) < 0.9 else [0, 1, 0]
        e1 = [ax[1]*key_n[2]-ax[2]*key_n[1], ax[2]*key_n[0]-ax[0]*key_n[2],
              ax[0]*key_n[1]-ax[1]*key_n[0]]
        L = sum(x*x for x in e1) ** 0.5
        e1 = [x/L for x in e1]
        e2 = [key_n[1]*e1[2]-key_n[2]*e1[1], key_n[2]*e1[0]-key_n[0]*e1[2],
              key_n[0]*e1[1]-key_n[1]*e1[0]]
        flat = [(sum(q[i]*e1[i] for i in range(3)),
                 sum(q[i]*e2[i] for i in range(3))) for q in pts]
        # SMX quads store v0,v1,v2,v3 in strip order; the ring is v0,v1,v3,v2.
        ring = [flat[0], flat[1], flat[3], flat[2]] if len(flat) == 4 else flat
        planes[(round(key_n[0], 3), round(key_n[1], 3), round(key_n[2], 3),
                round(d, 1))].append((pi, ring, facing, pts))

    def tris(r):
        return [(r[0], r[1], r[2]), (r[0], r[2], r[3])] if len(r) == 4 else [tuple(r)]

    def in_tri(pt, t):
        x, y = pt
        den = ((t[1][1]-t[2][1])*(t[0][0]-t[2][0]) + (t[2][0]-t[1][0])*(t[0][1]-t[2][1]))
        if abs(den) < 1e-9:
            return False
        a = ((t[1][1]-t[2][1])*(x-t[2][0]) + (t[2][0]-t[1][0])*(y-t[2][1])) / den
        b = ((t[2][1]-t[0][1])*(x-t[2][0]) + (t[0][0]-t[2][0])*(y-t[2][1])) / den
        return a > 1e-3 and b > 1e-3 and (1-a-b) > 1e-3

    hits = set()
    for lst in planes.values():
        for i, (pi, ri, fi, pts_i) in enumerate(lst):
            centres = [(sum(q[0] for q in t)/3, sum(q[1] for q in t)/3) for t in tris(ri)]
            for j, (pj, rj, fj, _) in enumerate(lst):
                if i == j or fi != fj:
                    continue
                if any(any(in_tri(c, t) for t in tris(rj)) for c in centres):
                    hits.add((min(pi, pj), max(pi, pj),
                              tuple(round(v) for v in pts_i[0])))
    if hits:
        print(f"WARNING: {len(hits)} same-facing coplanar overlap(s) — these "
              f"z-fight on hardware. Delete the duplicate face in Blender:")
        for pi, pj, at in sorted(hits):
            print(f"    prims {pi} and {pj} share a plane through {at}")
    else:
        print("No same-facing coplanar overlaps.")


_warn_duplicate_faces()

inv = {v: k for k, v in NAME_TO_SLOT.items()}
counts = {}
for e in entries:
    counts[e] = counts.get(e, 0) + 1
print(f"{len(entries)} prims: " + ", ".join(
    (f"slot{k}({inv.get(k, '?')})={v}" if k != UNTEXTURED else f"untextured={v}")
    for k, v in sorted(counts.items())))

lines = [
    "/* Auto-generated by gen_reception_tex_map.py — do not edit.",
    "   Per-poly texture mapped by NAME to engine VRAM slots (see the script). */",
    "#pragma once",
    "#include <stdint.h>",
    f"#define RECEPTION_PRIM_COUNT {len(entries)}",
    f"static const uint8_t reception_tex_map[{len(entries)}] = {{",
]
row = []
for i, e in enumerate(entries):
    row.append(f"0x{e:02X}")
    if len(row) == 16 or i == len(entries) - 1:
        lines.append("    " + ",".join(row) + ",")
        row = []
lines.append("};")

lines.append("/* 1 = triangle-shaped (degenerate) quad: never backface-cull it. */")
lines.append(f"static const uint8_t reception_nocull[{len(nocull)}] = {{")
row = []
for i, e in enumerate(nocull):
    row.append(str(e))
    if len(row) == 32 or i == len(nocull) - 1:
        lines.append("    " + ",".join(row) + ",")
        row = []
lines.append("};")
print(f"degenerate (never-cull) quads: {sum(nocull)}")

with open(OUT, 'w') as f:
    f.write('\n'.join(lines) + '\n')
print(f"Written to {OUT}")
