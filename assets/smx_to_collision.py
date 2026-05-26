#!/usr/bin/env python3
"""
smx_to_collision.py
Converts a PSn00bSDK SMX file into a C array of Wall structs for PS1 collision.

Usage:
    python3 smx_to_collision.py room.smx
    python3 smx_to_collision.py room.smx --scale 1.0 --room main_room --threshold 0.3

Output:
    room_collision.c  -- add to CMakeLists.txt
    room_collision.h  -- include in collision.c

How it works:
    - Parses the SMX XML format exported by the Blender plugin
    - Reads all vertices and primitives (tris and quads)
    - For each face, computes the normal from vertex positions
    - Vertical faces (walls) -> exported as Wall structs
    - Horizontal faces (floors/ceilings) -> exported as comments with Y values
    - Normals are normalised to fixed point 4096 = 1.0

SMX coordinate system (after Blender export):
    X = right
    Y = up (negated from Blender Z)
    Z = forward (from Blender Y)
"""

import sys
import os
import math
import xml.etree.ElementTree as ET

# -----------------------------------------------------------------------
# Configuration (can also be set via command line args)
# -----------------------------------------------------------------------

SCALE           = 1.0    # SMX coords are already in PS1 units - usually no scaling needed
WALL_THRESHOLD  = 0.4    # Max Y component of normal to count as a wall (0=vertical, 1=horizontal)
FLOOR_THRESHOLD = 0.7    # Min Y component of normal to count as a floor
ROOM_NAME       = "room" # Name used in generated C code
MAX_WALLS       = 64     # Warn if more walls than this are found

# -----------------------------------------------------------------------
# SMX Parser
# -----------------------------------------------------------------------

def parse_smx(filepath):
    """Parse SMX XML file and return vertices and primitives."""
    tree = ET.parse(filepath)
    root = tree.getroot()

    # Parse vertices
    vertices = []
    verts_elem = root.find('vertices')
    if verts_elem is not None:
        for v in verts_elem.findall('v'):
            x = float(v.get('x', 0))
            y = float(v.get('y', 0))
            z = float(v.get('z', 0))
            vertices.append((x * SCALE, y * SCALE, z * SCALE))

    # Parse primitives (tris and quads)
    primitives = []
    prims_elem = root.find('primitives')
    if prims_elem is not None:
        for p in prims_elem.findall('poly'):
            ptype = p.get('type', '')
            v0 = int(p.get('v0', 0))
            v1 = int(p.get('v1', 0))
            v2 = int(p.get('v2', 0))

            if '4' in ptype:
                # Quad
                v3 = int(p.get('v3', 0))
                primitives.append([v0, v1, v2, v3])
            else:
                # Triangle
                primitives.append([v0, v1, v2])

    return vertices, primitives


# -----------------------------------------------------------------------
# Math helpers
# -----------------------------------------------------------------------

def sub(a, b):
    return (a[0]-b[0], a[1]-b[1], a[2]-b[2])

def cross(a, b):
    return (
        a[1]*b[2] - a[2]*b[1],
        a[2]*b[0] - a[0]*b[2],
        a[0]*b[1] - a[1]*b[0],
    )

def length(a):
    return math.sqrt(a[0]**2 + a[1]**2 + a[2]**2)

def normalize(a):
    l = length(a)
    if l < 0.0001:
        return (0.0, 0.0, 0.0)
    return (a[0]/l, a[1]/l, a[2]/l)

def face_normal(verts, face):
    """Compute face normal from first 3 vertices."""
    v0 = verts[face[0]]
    v1 = verts[face[1]]
    v2 = verts[face[2]]
    e1 = sub(v1, v0)
    e2 = sub(v2, v0)
    n  = cross(e1, e2)
    return normalize(n)

def face_centre(verts, face):
    """Compute centroid of face."""
    xs = [verts[i][0] for i in face]
    ys = [verts[i][1] for i in face]
    zs = [verts[i][2] for i in face]
    n  = len(face)
    return (sum(xs)/n, sum(ys)/n, sum(zs)/n)


# -----------------------------------------------------------------------
# Wall and floor extraction
# -----------------------------------------------------------------------

def extract_walls(vertices, primitives):
    walls  = []
    floors = []

    # In PS1 Y-down space the floor is at the most-positive Y in the mesh.
    # We use this to reject wall faces that exist entirely above the ceiling
    # (geometry like stairwell overhangs, roof panels, etc.) which have no
    # vertices at floor level and would otherwise create phantom walls.
    level_floor_y = max(v[1] for v in vertices)

    for face in primitives:
        if len(face) < 3:
            continue

        n = face_normal(vertices, face)

        # n[1] is the Y component (up/down)
        # A vertical wall has n[1] close to 0
        # A horizontal floor has n[1] close to 1 or -1
        abs_ny = abs(n[1])

        if abs_ny < WALL_THRESHOLD:
            # Skip faces whose highest vertex (most floor-ward in Y-down) is
            # more than 10 units above the detected floor. These are geometry
            # that lives above the ceiling and would create phantom collision.
            face_max_y = max(vertices[i][1] for i in face)
            if face_max_y < level_floor_y - 10:
                continue

            # --- WALL ---
            # Project all face vertices onto XZ plane
            ps1_verts = [(vertices[i][0], vertices[i][2]) for i in face]

            # Find wall extent by projecting onto wall direction
            # Wall direction is perpendicular to normal in XZ
            wall_dir = (-n[2], n[0])
            wdl = math.sqrt(wall_dir[0]**2 + wall_dir[1]**2)
            if wdl < 0.0001:
                continue
            wall_dir = (wall_dir[0]/wdl, wall_dir[1]/wdl)

            projections = sorted(
                [(pv[0]*wall_dir[0] + pv[1]*wall_dir[1], pv) for pv in ps1_verts],
                key=lambda x: x[0]
            )

            start = projections[0][1]
            end   = projections[-1][1]

            # Normalise XZ normal to fixed point 4096
            xz_len = math.sqrt(n[0]**2 + n[2]**2)
            if xz_len < 0.0001:
                continue

            norm_nx = int(-(n[0] / xz_len) * 4096)
            norm_nz = int(-(n[2] / xz_len) * 4096)

            # Average Y of the face for reference
            avg_y = sum(vertices[i][1] for i in face) / len(face)

            walls.append({
                'x1': int(start[0]),
                'z1': int(start[1]),
                'x2': int(end[0]),
                'z2': int(end[1]),
                'nx': norm_nx,
                'nz': norm_nz,
                'avg_y': int(avg_y),
            })

        elif abs_ny > FLOOR_THRESHOLD:
            # --- FLOOR / CEILING ---
            ps1_verts = [(vertices[i][0], vertices[i][1], vertices[i][2]) for i in face]
            xs = [v[0] for v in ps1_verts]
            ys = [v[1] for v in ps1_verts]
            zs = [v[2] for v in ps1_verts]

            floors.append({
                'y':         int(sum(ys) / len(ys)),
                'min_x':     int(min(xs)),
                'max_x':     int(max(xs)),
                'min_z':     int(min(zs)),
                'max_z':     int(max(zs)),
                'is_ceiling': n[1] < 0,
            })

    return walls, floors


def deduplicate_walls(walls, tolerance=50):
    """Remove near-duplicate walls."""
    unique = []
    for w in walls:
        is_dup = False
        for u in unique:
            if (abs(w['x1']-u['x1']) < tolerance and
                abs(w['z1']-u['z1']) < tolerance and
                abs(w['x2']-u['x2']) < tolerance and
                abs(w['z2']-u['z2']) < tolerance):
                is_dup = True
                break
        if not is_dup:
            unique.append(w)
    return unique


# -----------------------------------------------------------------------
# C code generation
# -----------------------------------------------------------------------

def generate_c(walls, floors, output_base, smx_filename):
    c_file = output_base + ".c"
    h_file = output_base + ".h"

    guard    = os.path.basename(output_base).upper().replace('-','_').replace(' ','_') + "_H"
    var_name = ROOM_NAME.upper()

    all_x = [w['x1'] for w in walls] + [w['x2'] for w in walls]
    all_z = [w['z1'] for w in walls] + [w['z2'] for w in walls]
    min_x = min(all_x) if all_x else -1600
    max_x = max(all_x) if all_x else  1600
    min_z = min(all_z) if all_z else -1600
    max_z = max(all_z) if all_z else  1600

    # ---- Header ----
    with open(h_file, 'w') as f:
        f.write("/*\n")
        f.write(" * Auto-generated collision data from: %s\n" % smx_filename)
        f.write(" * Generated by smx_to_collision.py\n")
        f.write(" * DO NOT EDIT MANUALLY - regenerate from SMX instead\n")
        f.write(" */\n\n")
        f.write("#ifndef %s\n" % guard)
        f.write("#define %s\n\n" % guard)
        f.write("#include \"collision.h\"\n\n")
        f.write("#define %s_WALL_COUNT  %d\n" % (var_name, len(walls)))
        f.write("#define %s_FLOOR_COUNT %d\n\n" % (var_name, len(floors)))
        f.write("void %s_collision_init(CollisionRoom *r);\n\n" % ROOM_NAME)
        f.write("#endif /* %s */\n" % guard)

    # ---- Source ----
    with open(c_file, 'w') as f:
        f.write("/*\n")
        f.write(" * Auto-generated collision data from: %s\n" % smx_filename)
        f.write(" * Generated by smx_to_collision.py\n")
        f.write(" * Walls:  %d\n" % len(walls))
        f.write(" * Floors: %d\n" % len(floors))
        f.write(" * Bounds: X(%d to %d)  Z(%d to %d)\n" % (min_x, max_x, min_z, max_z))
        f.write(" * Normal scale: 4096 = 1.0 (fixed point)\n")
        f.write(" */\n\n")
        f.write("#include \"%s\"\n\n" % os.path.basename(h_file))

        f.write("void %s_collision_init(CollisionRoom *r) {\n" % ROOM_NAME)
        f.write("    r->wall_count = %s_WALL_COUNT;\n" % var_name)
        f.write("    r->min_x = %d;\n" % min_x)
        f.write("    r->max_x = %d;\n" % max_x)
        f.write("    r->min_z = %d;\n" % min_z)
        f.write("    r->max_z = %d;\n\n" % max_z)

        for i, w in enumerate(walls):
            f.write("    /* Wall %d (avg Y: %d) */\n" % (i, w['avg_y']))
            f.write("    r->walls[%d].x1 = %7d;  r->walls[%d].z1 = %7d;\n" % (i, w['x1'], i, w['z1']))
            f.write("    r->walls[%d].x2 = %7d;  r->walls[%d].z2 = %7d;\n" % (i, w['x2'], i, w['z2']))
            f.write("    r->walls[%d].nx = %7d;  r->walls[%d].nz = %7d;\n\n" % (i, w['nx'], i, w['nz']))

        if floors:
            f.write("    /*\n")
            f.write("     * Floor/ceiling planes detected:\n")
            f.write("     * Use these in apply_height() for vertical movement.\n")
            f.write("     *\n")
            for i, fl in enumerate(floors):
                kind = "CEILING" if fl['is_ceiling'] else "FLOOR  "
                f.write("     * %s %d: y=%6d  x(%d to %d)  z(%d to %d)\n" % (
                    kind, i, fl['y'],
                    fl['min_x'], fl['max_x'],
                    fl['min_z'], fl['max_z']))
            f.write("     */\n")

        f.write("}\n")

    return c_file, h_file


# -----------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------

def main():
    global SCALE, WALL_THRESHOLD, FLOOR_THRESHOLD, ROOM_NAME

    # Simple arg parsing
    args = sys.argv[1:]
    if not args or args[0] in ('-h', '--help'):
        print("Usage: python3 smx_to_collision.py <room.smx> [options]")
        print("")
        print("Options:")
        print("  --scale N       Multiply coordinates by N (default: 1.0)")
        print("  --room NAME     Room name for C identifiers (default: room)")
        print("  --threshold N   Wall detection threshold 0.0-1.0 (default: 0.4)")
        print("")
        print("Example:")
        print("  python3 smx_to_collision.py level1.smx --room level1 --scale 1.0")
        sys.exit(0)

    smx_path = args[0]

    i = 1
    while i < len(args):
        if args[i] == '--scale' and i+1 < len(args):
            SCALE = float(args[i+1]); i += 2
        elif args[i] == '--room' and i+1 < len(args):
            ROOM_NAME = args[i+1]; i += 2
        elif args[i] == '--threshold' and i+1 < len(args):
            WALL_THRESHOLD = float(args[i+1]); i += 2
        else:
            i += 1

    if not os.path.exists(smx_path):
        print("Error: file not found: %s" % smx_path)
        sys.exit(1)

    output_base = os.path.splitext(smx_path)[0] + "_collision"

    print("Parsing SMX: %s" % smx_path)
    vertices, primitives = parse_smx(smx_path)
    print("  Vertices:   %d" % len(vertices))
    print("  Primitives: %d" % len(primitives))

    print("Extracting walls (threshold=%.2f)..." % WALL_THRESHOLD)
    walls, floors = extract_walls(vertices, primitives)
    print("  Walls found:  %d" % len(walls))
    print("  Floors found: %d" % len(floors))

    print("Deduplicating...")
    walls = deduplicate_walls(walls)
    print("  Walls after dedup: %d" % len(walls))

    if len(walls) == 0:
        print("")
        print("WARNING: No walls found!")
        print("  Try lowering --threshold (currently %.2f)" % WALL_THRESHOLD)
        print("  e.g. python3 smx_to_collision.py %s --threshold 0.1" % smx_path)

    if len(walls) > MAX_WALLS:
        print("")
        print("WARNING: %d walls found, MAX_WALLS_PER_ROOM is %d." % (len(walls), MAX_WALLS))
        print("  Increase MAX_WALLS_PER_ROOM in collision.h")
        print("  or simplify your collision mesh.")

    print("Generating C code...")
    c_file, h_file = generate_c(walls, floors, output_base,
                                 os.path.basename(smx_path))
    print("  Written: %s" % c_file)
    print("  Written: %s" % h_file)
    print("")
    print("Next steps:")
    print("  1. Add %s to CMakeLists.txt" % os.path.basename(c_file))
    print("  2. #include \"%s\" in collision.c" % os.path.basename(h_file))
    print("  3. Call %s_collision_init(&rooms[0]) in collision_init()" % ROOM_NAME)
    print("  4. Check floor Y values in comments -> wire into apply_height()")
    print("  5. Rebuild and test")

if __name__ == "__main__":
    main()
