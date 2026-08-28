"""
gen_chain_room_tex_map.py - generate src/chain_room_tex_map.h from
"Chain Room.smx". Same name-based scheme as gen_rear_gate_tex_map.py (the
Blender exporter renumbers the SMX's own texture list whenever the material set
changes, so raw indices are unstable).

NAME_TO_SLOT MUST match the slot order loaded in src/chain_room.c
(chain_room_load_assets). If you add/move a texture slot there, update it here too.

NOTE THE SOURCE PATH: this room's exports live in assets/garden/, not in
assets/ like the older rooms' - the same as both mazes, the Keystone Maze, the
Rear Gate, Fountain Square and the Outside Catacombs. Nothing in the build reads
them except this script and the two conversion commands in
tools/ADDING_A_ROOM.txt STEP 4.

Usage:
    python gen_chain_room_tex_map.py       # assets/garden/Chain Room.smx ->
                                           # src/chain_room_tex_map.h
    python gen_chain_room_tex_map.py <smx> <out>
"""
import xml.etree.ElementTree as ET
import sys

SMX = sys.argv[1] if len(sys.argv) > 1 else 'assets/garden/Chain Room.smx'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'src/chain_room_tex_map.h'

# Engine VRAM slot for each texture name (must match src/chain_room.c load order).
#
# SEVEN textures and this room OWNS NONE OF THEM: every one is a TIM that is
# already on the disc for another room. That is not thrift for its own sake. The
# TEX directory's ISO9660 record was 112 bytes short of filling its FOURTH
# 2048-byte sector, and the two new TIMs this room first shipped with pushed it
# into a fifth — which killed the game on the loading screen, in a CdSearchFile
# nowhere near this room. Read the long note in src/chain_room.c before adding a
# file here.
#
# The four redirections below are what make owning nothing possible:
#
#   'grss'           -> the grss_gs CLONE at x320 y0, as in every garden room
#                       since the Garden Stairs. Forced rather than tidy here:
#                       grss.tim's own home is x768 y0, this room's brick wall.
#   'gravel_texture' -> ITS OWN PAGE at x640 y0, NOT the gravel_gs clone at
#                       x704 y0 that every other garden room uses. The whole
#                       point is to leave x704 free for the chain below.
#                       GRAVEL.TIM also lives in the disc ROOT rather than TEX,
#                       so borrowing it costs no directory bytes either.
#                       Uploaded by delivery_upload_gravel().
#   'chain_128'      -> Maze One's CHAIN at x704 y0, through the narrow
#                       maze_one_upload_chain(). The same 4x4-tiled art.
#   'pipe_128'       -> the Greenhouse's 4bpp PIPEGH clone at x384 y256, NOT
#                       Maze One's 8bpp PIPE at x768 y0 — that is brick_wall's
#                       page. Nobody holds PIPEGH in RAM, so it is streamed on
#                       entry.
#
# NOTE ON THE TWO ALIAS FILES. smxlink resolves a material NAME to
# textures/<name>.tim, so textures/pipe_128.tim and textures/chain_128.tim must
# exist for the mesh to convert at all — but they are byte-identical copies of
# pipe_gh.tim and chain.tim, they are NOT on the disc, and tools/vram_map.py
# skips them by name. Do not add either to disc.xml.
NAME_TO_SLOT = {
    'hedge':          0,   # x384 y0,   courtyard's uploader
    'grdn_gte':       1,   # x512 y0,   courtyard's uploader
    'grss':           2,   # -> grss_gs, x320 y0, courtyard's uploader
    'gravel_texture': 3,   # own page,   x640 y0, delivery_upload_gravel()
    'brick_wall':     4,   # x768 y0,    courtyard's, via the Garden Stairs'
    'pipe_128':       5,   # -> pipe_gh, x384 y256, streamed on entry
    'chain_128':      6,   # -> chain,   x704 y0, maze_one_upload_chain()
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
                     f"(update both this script and src/chain_room.c).")
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
    "/* Auto-generated by gen_chain_room_tex_map.py - do not edit.",
    "   Per-poly texture mapped by NAME to engine VRAM slots (see the script). */",
    "#pragma once",
    "#include <stdint.h>",
    f"#define CHAIN_ROOM_PRIM_COUNT {len(entries)}",
    f"static const uint8_t chain_room_tex_map[{len(entries)}] = {{",
]
row = []
for i, e in enumerate(entries):
    row.append(f"0x{e:02X}")
    if len(row) == 16 or i == len(entries) - 1:
        lines.append("    " + ",".join(row) + ",")
        row = []
lines.append("};")

lines.append("/* 1 = triangle-shaped (degenerate) quad: never backface-cull it. */")
lines.append(f"static const uint8_t chain_room_nocull[{len(nocull)}] = {{")
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
