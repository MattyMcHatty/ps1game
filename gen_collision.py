"""
Generate level1_mesh_collision.c from level1.smx.
Reads PS1-space coordinates directly; merges collinear wall segments.
"""
import xml.etree.ElementTree as ET
import math
from collections import defaultdict

SMX = 'assets/level1.smx'
OUT = 'src/level1_mesh_collision.c'

tree = ET.parse(SMX)
root = tree.getroot()

verts = [(float(v.get('x')), float(v.get('y')), float(v.get('z')))
         for v in root.find('vertices').findall('v')]

normals = [(float(n.get('x')), float(n.get('y')), float(n.get('z')))
           for n in root.find('normals').findall('v')]

# Collect raw axis-aligned wall segments
x_walls = defaultdict(lambda: defaultdict(list))  # nx -> x_val -> [(zmin,zmax)]
z_walls = defaultdict(lambda: defaultdict(list))  # nz -> z_val -> [(xmin,xmax)]

for prim in root.find('primitives').findall('poly'):
    ptype = prim.get('type')
    vi = [int(prim.get(f'v{i}')) for i in range(4 if ptype == 'F4' else 3)]
    n0 = int(prim.get('n0'))
    pv = [verts[i] for i in vi]
    ys = [v[1] for v in pv]
    if max(ys) - min(ys) < 50:
        continue

    fnx, fny, fnz = normals[n0]
    xz_len = math.sqrt(fnx**2 + fnz**2)
    if xz_len < 0.1:
        continue

    # Round to nearest axis (all walls are axis-aligned in this level)
    if abs(fnx) >= abs(fnz):
        nx = 4096 if fnx > 0 else -4096
        nz = 0
    else:
        nx = 0
        nz = 4096 if fnz > 0 else -4096

    seen_xz = list(dict.fromkeys((round(v[0]), round(v[2])) for v in pv))
    if len(seen_xz) < 2:
        continue

    xs = [p[0] for p in seen_xz]
    zs = [p[1] for p in seen_xz]

    if nx != 0:  # X-constant wall
        x_val = round(sum(xs) / len(xs))
        z_walls_range = (min(zs), max(zs))
        x_walls[nx][x_val].append(z_walls_range)
    else:         # Z-constant wall
        z_val = round(sum(zs) / len(zs))
        x_walls_range = (min(xs), max(xs))
        z_walls[nz][z_val].append(x_walls_range)


def merge_ranges(ranges, tol=5):
    if not ranges:
        return []
    sr = sorted(set(ranges))
    merged = [list(sr[0])]
    for lo, hi in sr[1:]:
        if lo <= merged[-1][1] + tol:
            merged[-1][1] = max(merged[-1][1], hi)
        else:
            merged.append([lo, hi])
    return [(r[0], r[1]) for r in merged]


wall_list = []

for nx, x_groups in x_walls.items():
    for x_val, ranges in x_groups.items():
        for zmin, zmax in merge_ranges(ranges):
            wall_list.append((x_val, zmin, x_val, zmax, nx, 0))

for nz, z_groups in z_walls.items():
    for z_val, ranges in z_groups.items():
        for xmin, xmax in merge_ranges(ranges):
            wall_list.append((xmin, z_val, xmax, z_val, 0, nz))

wall_list.sort()

all_x = [w[0] for w in wall_list] + [w[2] for w in wall_list]
all_z = [w[1] for w in wall_list] + [w[3] for w in wall_list]
min_x, max_x = min(all_x), max(all_x)
min_z, max_z = min(all_z), max(all_z)

print(f"Merged to {len(wall_list)} walls")
print(f"Bounds: X({min_x} to {max_x})  Z({min_z} to {max_z})")
for w in wall_list:
    print(f"  ({w[0]:6},{w[1]:6}) -> ({w[2]:6},{w[3]:6})  n=({w[4]:5},{w[5]:5})")

header = f"""\
/*
 * Auto-generated collision data
 * Source: {SMX}
 * Wall count: {len(wall_list)}
 */

#include "level1_mesh_collision.h"

void room_collision_init(CollisionRoom *r) {{
    r->wall_count = {len(wall_list)};
    r->min_x = {min_x};
    r->max_x = {max_x};
    r->min_z = {min_z};
    r->max_z = {max_z};
"""

body = ""
for i, (x1, z1, x2, z2, nx, nz) in enumerate(wall_list):
    body += f"""
    /* Wall {i} */
    r->walls[{i}].x1 = {x1:6};  r->walls[{i}].z1 = {z1:6};
    r->walls[{i}].x2 = {x2:6};  r->walls[{i}].z2 = {z2:6};
    r->walls[{i}].nx = {nx:6};  r->walls[{i}].nz = {nz:6};
"""

footer = "}\n"

with open(OUT, 'w') as f:
    f.write(header + body + footer)

print(f"\nWritten to {OUT}")
