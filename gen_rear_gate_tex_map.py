"""
gen_rear_gate_tex_map.py - generate src/rear_gate_tex_map.h from "Rear Gate.smx".
Same name-based scheme as gen_maze_two_tex_map.py (the Blender exporter
renumbers the SMX's own texture list whenever the material set changes, so raw
indices are unstable).

NAME_TO_SLOT MUST match the slot order loaded in src/rear_gate.c
(rear_gate_load_assets). If you add/move a texture slot there, update it here too.

NOTE THE SOURCE PATH: this room's exports live in assets/garden/, not in
assets/ like the older rooms' — the same as Maze One/Two, Fountain Square and
the Outside Catacombs. Nothing in the build reads them except this script and
the two conversion commands in tools/ADDING_A_ROOM.txt STEP 4.

Usage:
    python gen_rear_gate_tex_map.py        # assets/garden/Rear Gate.smx ->
                                           # src/rear_gate_tex_map.h
    python gen_rear_gate_tex_map.py <smx> <out>
"""
import xml.etree.ElementTree as ET
import sys

SMX = sys.argv[1] if len(sys.argv) > 1 else 'assets/garden/Rear Gate.smx'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'src/rear_gate_tex_map.h'

# Engine VRAM slot for each texture name (must match src/rear_gate.c load order).
#
# NINE textures, the most of any room in the game, and only two of them are this
# room's own. Four of the redirections below are the whole reason the other seven
# are free, so they are worth reading rather than skimming:
#
#   'grss'           -> the grss_gs CLONE at x320 y0, not grss.tim's own page.
#                       grss.tim lives at x768 y0, which is where this room's
#                       brick_wall goes; every garden room since the Garden
#                       Stairs has made the same substitution so the whole chain
#                       draws from one set of slots.
#   'gravel_texture' -> the gravel_gs CLONE at x704 y0, for the mirror-image
#                       reason: gravel_texture.tim's own page is x640 y0, which
#                       this room fills with plinth_rg.
#   'trees'          -> the delivery area's TREES_DL clone at x960 y256. That
#                       page is owned outright and uploaded once at startup, so
#                       this costs nothing at all — no registration, no entry-time
#                       LoadImage, not even a restore obligation. trees.tim's own
#                       home is x640 y0, again where plinth_rg goes.
#   'plinth' /       -> this room's two retargeted copies. It draws drain, plinth
#   'double_door'       AND double_door at once and all three live at x832 y0;
#                       drain keeps that page, so the other two moved. See
#                       tools/vram_map.py's KNOWN_STREAM_PAIRS for where and why.
NAME_TO_SLOT = {
    # Borrowed, and free. hedge / grdn_gte / grss_gs / gravel_gs / brick_wall all
    # come from the Garden Courtyard's uploader (which runs the Garden Stairs'
    # first), and drain from fountain_square_upload_drain().
    'hedge':          0,
    'grdn_gte':       1,
    'grss':           2,   # -> grss_gs
    'gravel_texture': 3,   # -> gravel_gs
    'brick_wall':     4,
    'drain':          5,
    'trees':          6,   # -> trees_dl, resident from startup
    # The two this room OWNS, both byte-for-byte retargets of textures that
    # already exist (tools/retarget_tim.py), not new art.
    'plinth':         7,   # -> plinth_rg,  x640 y0
    'double_door':    8,   # -> dbl_dr_rg,  x320 y256
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
                     f"(update both this script and src/rear_gate.c).")
        entries.append(NAME_TO_SLOT[name])

    deg = 0
    if p.get('v3') is not None:
        q = [verts[int(p.get(f'v{k}'))] for k in range(4)]
        if any(_collinear(q[a], q[b], q[c])
               for a, b, c in ((0, 1, 2), (0, 1, 3), (0, 2, 3), (1, 2, 3))):
            deg = 1
    nocull.append(deg)

inv = {v: k for k, v in NAME_TO_SLOT.items()}
counts = {}
for e in entries:
    counts[e] = counts.get(e, 0) + 1
print(f"{len(entries)} prims: " + ", ".join(
    (f"slot{k}({inv.get(k, '?')})={v}" if k != UNTEXTURED else f"untextured={v}")
    for k, v in sorted(counts.items())))

lines = [
    "/* Auto-generated by gen_rear_gate_tex_map.py - do not edit.",
    "   Per-poly texture mapped by NAME to engine VRAM slots (see the script). */",
    "#pragma once",
    "#include <stdint.h>",
    f"#define REAR_GATE_PRIM_COUNT {len(entries)}",
    f"static const uint8_t rear_gate_tex_map[{len(entries)}] = {{",
]
row = []
for i, e in enumerate(entries):
    row.append(f"0x{e:02X}")
    if len(row) == 16 or i == len(entries) - 1:
        lines.append("    " + ",".join(row) + ",")
        row = []
lines.append("};")

lines.append("/* 1 = triangle-shaped (degenerate) quad: never backface-cull it. */")
lines.append(f"static const uint8_t rear_gate_nocull[{len(nocull)}] = {{")
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
