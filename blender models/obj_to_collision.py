#!/usr/bin/env python3
"""
obj_to_collision.py
Converts a Blender OBJ file into a C array of Wall structs for PS1 collision.

Usage:
    python3 obj_to_collision.py collision_mesh.obj

Output:
    collision_mesh.c  -- paste into your collision.c
    collision_mesh.h  -- paste into your collision.h

How to use in Blender:
    1. Create a simplified collision mesh (just key surfaces, no detail)
    2. File -> Export -> Wavefront (.obj)
    3. In export settings tick "Apply Modifiers" and "Triangulate Faces"
    4. Run this script on the exported OBJ

Notes:
    - Only vertical (or near-vertical) faces are exported as walls
    - Horizontal faces (floor/ceiling) are exported as floor planes
    - Scale factor of 100 is applied (1 Blender unit = 100 PS1 units)
    - Normals are computed from face winding and normalised to 4096 = 1.0
"""

import sys
import os
import math

# -----------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------

SCALE           = 100.0   # 1 Blender unit = 100 PS1 units
WALL_THRESHOLD  = 0.3     # How vertical a face must be to count as a wall
                          # (dot product of face normal with Y axis, 0=vertical 1=horizontal)
FLOOR_THRESHOLD = 0.7     # How horizontal a face must be to count as a floor
PLAYER_RADIUS   = 120     # Player collision radius in PS1 units
ROOM_NAME       = "room"  # Name used in generated C code

# -----------------------------------------------------------------------
# OBJ Parser
# -----------------------------------------------------------------------

def parse_obj(filepath):
    vertices = []
    faces    = []
    normals  = []

    with open(filepath, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue

            parts = line.split()
            token = parts[0]

            if token == 'v':
                x = float(parts[1])
                y = float(parts[2])
                z = float(parts[3])
                vertices.append((x, y, z))

            elif token == 'vn':
                nx = float(parts[1])
                ny = float(parts[2])
                nz = float(parts[3])
                normals.append((nx, ny, nz))

            elif token == 'f':
                # Face indices (1-based in OBJ, supports v, v/vt, v/vt/vn, v//vn)
                face_verts = []
                face_normals = []
                for p in parts[1:]:
                    indices = p.split('/')
                    vi = int(indices[0]) - 1
                    face_verts.append(vi)
                    if len(indices) == 3 and indices[2]:
                        ni = int(indices[2]) - 1
                        face_normals.append(ni)
                faces.append((face_verts, face_normals))

    return vertices, faces, normals


# -----------------------------------------------------------------------
# Math helpers
# -----------------------------------------------------------------------

def cross(a, b):
    return (
        a[1]*b[2] - a[2]*b[1],
        a[2]*b[0] - a[0]*b[2],
        a[0]*b[1] - a[1]*b[0],
    )

def sub(a, b):
    return (a[0]-b[0], a[1]-b[1], a[2]-b[2])

def dot(a, b):
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]

def length(a):
    return math.sqrt(a[0]**2 + a[1]**2 + a[2]**2)

def normalize(a):
    l = length(a)
    if l < 0.0001:
        return (0.0, 0.0, 0.0)
    return (a[0]/l, a[1]/l, a[2]/l)

def face_normal(verts, face_verts):
    """Compute face normal from first 3 vertices using cross product."""
    v0 = verts[face_verts[0]]
    v1 = verts[face_verts[1]]
    v2 = verts[face_verts[2]]
    e1 = sub(v1, v0)
    e2 = sub(v2, v0)
    n  = cross(e1, e2)
    return normalize(n)

def ps1_coords(blender_x, blender_y, blender_z):
    """Convert Blender coordinates to PS1 coordinates.
    Blender: X=right, Y=forward, Z=up
    PS1:     X=right, Y=down,    Z=forward
    """
    x =  blender_x * SCALE
    y = -blender_z * SCALE   # Blender Z -> PS1 Y (negated because PS1 Y is down)
    z =  blender_y * SCALE   # Blender Y -> PS1 Z
    return (x, y, z)


# -----------------------------------------------------------------------
# Wall extraction
# -----------------------------------------------------------------------

def extract_walls(vertices, faces, normals_list):
    walls  = []
    floors = []

    for face_verts, face_normals in faces:
        if len(face_verts) < 3:
            continue

        # Compute face normal in Blender space
        n = face_normal(vertices, face_verts)

        # Convert normal to PS1 space
        # Blender normal (nx, ny, nz) -> PS1 (nx, -nz, ny)
        ps1_nx = n[0]
        ps1_ny = -n[2]
        ps1_nz = n[1]

        # How vertical is this face?
        # A purely vertical wall has ps1_ny = 0
        # A purely horizontal floor has ps1_ny = 1 or -1
        abs_ny = abs(ps1_ny)

        if abs_ny < WALL_THRESHOLD:
            # --- WALL FACE ---
            # For walls we only care about XZ plane
            # Find the two most extreme vertices along XZ to define the wall segment

            # Project all vertices to PS1 XZ plane
            ps1_verts = []
            for vi in face_verts:
                bv = vertices[vi]
                px, py, pz = ps1_coords(bv[0], bv[1], bv[2])
                ps1_verts.append((px, py, pz))

            # Find bounding segment along wall direction
            # Wall direction is perpendicular to normal in XZ plane
            wall_dir = (-ps1_nz, ps1_nx)  # rotate normal 90 degrees in XZ
            wall_dir_len = math.sqrt(wall_dir[0]**2 + wall_dir[1]**2)
            if wall_dir_len > 0.0001:
                wall_dir = (wall_dir[0]/wall_dir_len, wall_dir[1]/wall_dir_len)

            # Project verts onto wall direction to find extent
            projections = []
            for pv in ps1_verts:
                proj = pv[0]*wall_dir[0] + pv[2]*wall_dir[1]
                projections.append((proj, pv))

            projections.sort(key=lambda x: x[0])
            start_vert = projections[0][1]
            end_vert   = projections[-1][1]

            # Normalise XZ normal to 4096
            xz_len = math.sqrt(ps1_nx**2 + ps1_nz**2)
            if xz_len < 0.0001:
                continue

            norm_nx = int((ps1_nx / xz_len) * 4096)
            norm_nz = int((ps1_nz / xz_len) * 4096)

            walls.append({
                'x1': int(start_vert[0]),
                'z1': int(start_vert[2]),
                'x2': int(end_vert[0]),
                'z2': int(end_vert[2]),
                'nx': norm_nx,
                'nz': norm_nz,
            })

        elif abs_ny > FLOOR_THRESHOLD:
            # --- FLOOR/CEILING FACE ---
            # Compute average Y height of this face
            total_y = 0.0
            ps1_verts = []
            for vi in face_verts:
                bv = vertices[vi]
                px, py, pz = ps1_coords(bv[0], bv[1], bv[2])
                ps1_verts.append((px, py, pz))
                total_y += py

            avg_y = total_y / len(face_verts)

            # Compute XZ bounds
            xs = [v[0] for v in ps1_verts]
            zs = [v[2] for v in ps1_verts]

            floors.append({
                'y':     int(avg_y),
                'min_x': int(min(xs)),
                'max_x': int(max(xs)),
                'min_z': int(min(zs)),
                'max_z': int(max(zs)),
                'is_ceiling': ps1_ny < 0,
            })

    return walls, floors


# -----------------------------------------------------------------------
# Deduplication
# -----------------------------------------------------------------------

def deduplicate_walls(walls, tolerance=50):
    """Remove duplicate or near-duplicate walls."""
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

def generate_c(walls, floors, output_base):
    c_file = output_base + ".c"
    h_file = output_base + ".h"

    guard = os.path.basename(output_base).upper() + "_H"

    # Compute room bounds from walls
    all_x = [w['x1'] for w in walls] + [w['x2'] for w in walls]
    all_z = [w['z1'] for w in walls] + [w['z2'] for w in walls]
    min_x = min(all_x) if all_x else -1600
    max_x = max(all_x) if all_x else  1600
    min_z = min(all_z) if all_z else -1600
    max_z = max(all_z) if all_z else  1600

    # ---- Header file ----
    with open(h_file, 'w') as f:
        f.write("/*\n")
        f.write(" * Auto-generated collision data\n")
        f.write(" * Generated by obj_to_collision.py\n")
        f.write(" * DO NOT EDIT - regenerate from OBJ instead\n")
        f.write(" */\n\n")
        f.write("#ifndef %s\n" % guard)
        f.write("#define %s\n\n" % guard)
        f.write("#include \"collision.h\"\n\n")
        f.write("/* Number of walls in this room */\n")
        f.write("#define %s_WALL_COUNT %d\n\n" % (ROOM_NAME.upper(), len(walls)))
        f.write("/* Number of floor planes in this room */\n")
        f.write("#define %s_FLOOR_COUNT %d\n\n" % (ROOM_NAME.upper(), len(floors)))
        f.write("void %s_collision_init(CollisionRoom *r);\n\n" % ROOM_NAME)
        f.write("#endif /* %s */\n" % guard)

    # ---- C file ----
    with open(c_file, 'w') as f:
        f.write("/*\n")
        f.write(" * Auto-generated collision data\n")
        f.write(" * Generated by obj_to_collision.py\n")
        f.write(" * Scale factor: %.1f (1 Blender unit = %.0f PS1 units)\n" % (SCALE, SCALE))
        f.write(" * Wall count: %d\n" % len(walls))
        f.write(" * Floor count: %d\n" % len(floors))
        f.write(" * Room bounds: X(%d to %d) Z(%d to %d)\n" % (min_x, max_x, min_z, max_z))
        f.write(" */\n\n")
        f.write("#include \"%s\"\n\n" % (os.path.basename(h_file)))

        f.write("void %s_collision_init(CollisionRoom *r) {\n" % ROOM_NAME)
        f.write("    r->wall_count = %s_WALL_COUNT;\n" % ROOM_NAME.upper())
        f.write("    r->min_x = %d;\n" % min_x)
        f.write("    r->max_x = %d;\n" % max_x)
        f.write("    r->min_z = %d;\n" % min_z)
        f.write("    r->max_z = %d;\n\n" % max_z)

        # Write walls
        for i, w in enumerate(walls):
            f.write("    /* Wall %d */\n" % i)
            f.write("    r->walls[%d].x1 = %6d;  r->walls[%d].z1 = %6d;\n" % (i, w['x1'], i, w['z1']))
            f.write("    r->walls[%d].x2 = %6d;  r->walls[%d].z2 = %6d;\n" % (i, w['x2'], i, w['z2']))
            f.write("    r->walls[%d].nx = %6d;  r->walls[%d].nz = %6d;\n\n" % (i, w['nx'], i, w['nz']))

        # Write floor planes as comments (useful reference even if not directly used)
        if floors:
            f.write("    /*\n")
            f.write("     * Floor/ceiling planes detected:\n")
            for i, fl in enumerate(floors):
                kind = "CEILING" if fl['is_ceiling'] else "FLOOR"
                f.write("     * %s %d: y=%d, x(%d to %d), z(%d to %d)\n" % (
                    kind, i, fl['y'], fl['min_x'], fl['max_x'], fl['min_z'], fl['max_z']))
            f.write("     *\n")
            f.write("     * Use these Y values in apply_height() for floor detection.\n")
            f.write("     */\n")

        f.write("}\n")

    return c_file, h_file


# -----------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 obj_to_collision.py <collision_mesh.obj>")
        print("")
        print("Options (edit at top of script):")
        print("  SCALE          = %.1f  (1 Blender unit = %.0f PS1 units)" % (SCALE, SCALE))
        print("  WALL_THRESHOLD = %.1f  (lower = more faces counted as walls)" % WALL_THRESHOLD)
        print("  ROOM_NAME      = '%s'" % ROOM_NAME)
        sys.exit(1)

    obj_path = sys.argv[1]
    if not os.path.exists(obj_path):
        print("Error: file not found: %s" % obj_path)
        sys.exit(1)

    output_base = os.path.splitext(obj_path)[0] + "_collision"

    print("Parsing OBJ: %s" % obj_path)
    vertices, faces, normals = parse_obj(obj_path)
    print("  Vertices: %d" % len(vertices))
    print("  Faces:    %d" % len(faces))

    print("Extracting walls...")
    walls, floors = extract_walls(vertices, faces, normals)
    print("  Walls found:  %d" % len(walls))
    print("  Floors found: %d" % len(floors))

    print("Deduplicating...")
    walls = deduplicate_walls(walls)
    print("  Walls after dedup: %d" % len(walls))

    if len(walls) > 32:
        print("WARNING: %d walls found but MAX_WALLS_PER_ROOM is 32." % len(walls))
        print("         Increase MAX_WALLS_PER_ROOM in collision.h or simplify your mesh.")

    print("Generating C code...")
    c_file, h_file = generate_c(walls, floors, output_base)
    print("  Written: %s" % c_file)
    print("  Written: %s" % h_file)
    print("")
    print("Next steps:")
    print("  1. Add %s.c to your CMakeLists.txt" % os.path.basename(output_base))
    print("  2. Include %s.h in collision.c" % os.path.basename(output_base))
    print("  3. Call %s_collision_init(&rooms[0]) in collision_init()" % ROOM_NAME)
    print("  4. Check the floor Y values in the comments and add to apply_height()")
    print("  5. Rebuild and test")

if __name__ == "__main__":
    main()
