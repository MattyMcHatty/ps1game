"""
gen_asag_arena_tex_map.py - generate src/asag_arena_tex_map.h from
"assets/bosses/Asag Version Two/Asag Version Two_Arena.smx". Same name-based scheme as
gen_chain_room_tex_map.py: the Blender exporter renumbers the SMX's own texture
list whenever the material set changes, so a raw-index map silently mis-assigns
every texture past the one that moved.

NAME_TO_SLOT MUST match the row order of stream_tex_file[] in src/asag_arena.c.
Unlike every other room in the game, that table is NOT a set of compile-time
constants out of src/tim_slots.h - asag_arena.c captures each TIM's tpage/clut
from GetTimInfo at read time, precisely so a texture can be added here and to
disc.xml without a generator run. The SLOT INDEX is still the row position, so
this map and that table have to agree.

>>> SLOT 2 IS NOT THE ROOM'S. <<< It is the boss's one skin, streamed by the
ROOM because the room already owns the one loading screen that reaches this area
and the boss owns none. src/asag.c reads it back through
asag_arena_tex_page()/asag_arena_tex_clut(). No arena polygon uses it, so it
never appears in the map below - if one ever does, that is a texturing mistake,
not a feature.

Usage:
    python gen_asag_arena_tex_map.py       # the arena SMX -> src/asag_arena_tex_map.h
    python gen_asag_arena_tex_map.py <smx> <out>
"""
import xml.etree.ElementTree as ET
import sys

SMX = (sys.argv[1] if len(sys.argv) > 1 else
       'assets/bosses/Asag Version Two/Asag Version Two_Arena.smx')
OUT = sys.argv[2] if len(sys.argv) > 2 else 'src/asag_arena_tex_map.h'

# Engine VRAM slot for each texture name. MUST match stream_tex_file[] in
# src/asag_arena.c. VRAM slots come from tools/VRAM_MAP_ASAG.txt: every one is a
# full 8bpp page at Voff 0 whose occupants are all reclaimable in this bank, so
# taking them costs nothing and owes nobody a restore.
NAME_TO_SLOT = {
    'mud':       0,   # x384 y0    the arena floor and its mud banks
    'Boss Wall': 1,   # x512 y0    the perimeter wall
    # 2 asag is the BOSS's own skin, streamed by the room but never referenced
    # by an arena polygon. See the header.
}
UNTEXTURED = 0xFF

# ---------------------------------------------------------------------------
# THE BOILS, AND WHY THEY ARE DETECTED RATHER THAN LISTED
# ---------------------------------------------------------------------------
# Asag has two clusters of four polys on the back wall, one either side of him,
# which the art gives a red cast and which the encounter lights up. They are the
# raised FRONT FACES of two lumps pushed out of the wall - the wall's plane is
# z=2800 and these stand 66 units proud of it at z=2734.
#
# >>> THAT ONE NUMBER IDENTIFIES THEM EXACTLY, AND NOTHING ELSE IN THE MESH
# COMES NEAR IT. <<< Exactly EIGHT primitives in all 522 lie flat at z=2734 with
# every corner on that plane; all eight are "Boss Wall"; they fall into two
# 312x312 squares of four, centred at x=-700 y=-400 and x=+900 y=-600; and their
# UVs land on the two reddest patches of textures/Boss Wall.png (u[31,41]
# v[82,103] and u[94,107] v[65,91], against redness peaks at u~36 v~88 and u~100
# v~76). Four separate properties agreeing is what makes this a detection and
# not a guess.
#
# >>> AND IT IS DONE HERE RATHER THAN AS EIGHT LITERALS IN THE .c FOR THE REASON
# THE TEXTURE MAP ITSELF IS. <<< Primitive indices shuffle on every re-export.
# They happen to be 440..447 today, which is contiguous and therefore especially
# inviting to write as a range - and a range would go quietly wrong, lighting up
# eight arbitrary wall panels, the next time the mesh is touched. The plane does
# not move; the indices do.
#
# If this ever finds a count other than 8 it FAILS rather than emitting a
# half-right table: at that point either the art changed or the plane did, and
# both want a human.
BOIL_Z       = 2734.0
BOIL_Z_TOL   = 2.0
BOIL_EXPECT  = 8

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
nocull = []    # 1 = do NOT backface-cull (degenerate "triangle-shaped" quad)
boil = []      # 1 = one of Asag's eight boil faces (see BOIL_Z above)
for p in root.find('primitives').findall('poly'):
    n = p.get('texture')
    if n is None:
        entries.append(UNTEXTURED)
    else:
        name = smx_tex[int(n)]
        if name not in NAME_TO_SLOT:
            sys.exit(f"ERROR: SMX texture '{name}' has no engine slot in "
                     f"NAME_TO_SLOT (update both this script and "
                     f"stream_tex_file[] in src/asag_arena.c).")
        entries.append(NAME_TO_SLOT[name])

    deg = 0
    if p.get('v3') is not None:
        q = [verts[int(p.get(f'v{k}'))] for k in range(4)]
        if any(_collinear(q[a], q[b], q[c])
               for a, b, c in ((0, 1, 2), (0, 1, 3), (0, 2, 3), (1, 2, 3))):
            deg = 1
    nocull.append(deg)

    # A boil face: every corner flat on the z=2734 plane, and Boss Wall. The
    # texture test is not redundant — it is what stops a future mud prop that
    # happens to sit at the same depth from being lit up as an organ.
    ks = [int(p.get(f'v{k}')) for k in range(4) if p.get(f'v{k}') is not None]
    zs = [verts[k][2] for k in ks]
    is_boil = (n is not None and smx_tex[int(n)] == 'Boss Wall'
               and all(abs(z - BOIL_Z) <= BOIL_Z_TOL for z in zs))
    boil.append(1 if is_boil else 0)

inv = {v: k for k, v in NAME_TO_SLOT.items()}
counts = {}
for e in entries:
    counts[e] = counts.get(e, 0) + 1
print(f"{len(entries)} prims: " + ", ".join(
    (f"slot{k}({inv.get(k, '?')})={v}" if k != UNTEXTURED else f"untextured={v}")
    for k, v in sorted(counts.items())))

lines = [
    "/* Auto-generated by gen_asag_arena_tex_map.py - do not edit.",
    "   Per-poly texture mapped by NAME to engine VRAM slots (see the script). */",
    "#pragma once",
    "#include <stdint.h>",
    f"#define ASAG_ARENA_PRIM_COUNT {len(entries)}",
    f"static const uint8_t asag_arena_tex_map[{len(entries)}] = {{",
]
row = []
for i, e in enumerate(entries):
    row.append(f"0x{e:02X}")
    if len(row) == 16 or i == len(entries) - 1:
        lines.append("    " + ",".join(row) + ",")
        row = []
lines.append("};")

lines.append("/* 1 = triangle-shaped (degenerate) quad: never backface-cull it. */")
lines.append(f"static const uint8_t asag_arena_nocull[{len(nocull)}] = {{")
row = []
for i, e in enumerate(nocull):
    row.append(str(e))
    if len(row) == 32 or i == len(nocull) - 1:
        lines.append("    " + ",".join(row) + ",")
        row = []
lines.append("};")
print(f"degenerate (never-cull) quads: {sum(nocull)}")

# ---- The boils. FAIL rather than emit a half-right table; see BOIL_Z. -------
if sum(boil) != BOIL_EXPECT:
    idx = [i for i, b in enumerate(boil) if b]
    sys.exit(f"ERROR: found {sum(boil)} boil faces at z={BOIL_Z}, expected "
             f"{BOIL_EXPECT} (prims {idx}). Either the art changed or the "
             f"lumps moved off that plane - read the BOIL_Z note in this "
             f"script and re-derive it before shipping a glow that lights up "
             f"the wrong polygons.")
bidx = [i for i, b in enumerate(boil) if b]
lcen = [i for i in bidx if min(verts[int(root.find('primitives').findall('poly')[i]
        .get(f'v{k}'))][0] for k in range(4)) < 0]
print(f"boil faces: {sum(boil)} -> prims {bidx} "
      f"({len(lcen)} left of centre, {len(bidx) - len(lcen)} right)")
lines.append("/* 1 = one of Asag's eight boil faces: the raised z=2734 front")
lines.append("   plates of the two lumps either side of him, which the encounter")
lines.append("   lights up. DETECTED off that plane, not listed - see the BOIL_Z")
lines.append("   note in gen_asag_arena_tex_map.py for why indices would rot. */")
lines.append(f"static const uint8_t asag_arena_boil[{len(boil)}] = {{")
row = []
for i, e in enumerate(boil):
    row.append(str(e))
    if len(row) == 32 or i == len(boil) - 1:
        lines.append("    " + ",".join(row) + ",")
        row = []
lines.append("};")

with open(OUT, 'w') as f:
    f.write('\n'.join(lines) + '\n')
print(f"Written to {OUT}")
