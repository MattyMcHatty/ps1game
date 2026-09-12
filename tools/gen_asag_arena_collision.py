"""Build Asag's arena collision from the room proxy alone.

    py assets/smx_to_collision.py "assets/bosses/Asag Version Two/Asag Version Two mesh.smx"      --room asag_arena
    py assets/smx_to_collision.py "assets/bosses/Asag Version Two/Asag Version Two_Boss.smx"      --room asag_boss
    py tools/gen_asag_arena_collision.py

WHY THIS SCRIPT EXISTS AT ALL, NOW THAT IT HAS ONE SOURCE. It was a MERGE: the
boss had no collision proxy of its own, so its drawn geometry was converted and
its walls appended to the room's. That is gone - see below - and what is left is
the two edits smx_to_collision.py never emits (the lowercase #include and the
two CollisionRoom flags), plus a place for the next source if the room ever
grows one.

>>> THE BOSS'S 26 WALLS WERE REMOVED, AND THE REASON IS THE WHOLE LESSON HERE.
<<< They were baked from frame 1 of Head_Idle_Baked and were defensible exactly
as long as the body never moved: it rested at y[-960,-528], 528 above the floor,
so collision.c's vertical gate rejected all of them and they were harmless
placeholders for the day the art came down.

The day came in the same pass that added the position track. The body now slides
up to 969 units into the arena, and the slam and the faint bring it to floor
level - so a table baked from the bind pose is wrong in Z and in Y at once. A
CollisionRoom is a FIXED table read by every movement query in the frame and
cannot be re-baked per frame, which is the warning the old version of this file
already carried. asag_collide() in src/asag.c does the job instead, from the
same posed vertices the draw uses. DO NOT PUT THE WALLS BACK: they would fight
that collider, and they would be wrong.

The original note on why a merge existed:

  Asag Version Two mesh.smx   the room proxy - a purpose-built low-poly box with
                              a recess at each end. 12 walls, 3 floor planes.
  Asag Version Two_Boss.smx   THE BOSS'S OWN DRAWN GEOMETRY, at the user's
                              request: this model has no separate collision
                              proxy, so the mesh the player sees is the mesh the
                              player cannot walk through. 26 walls.

>>> THOSE 26 WALLS ARE ENTIRELY ABOVE THE PLAYER, AND THAT IS NOT A BUG. <<<
The bind pose spans y[-960,-528], i.e. its LOWEST point is 528 units above the
y=0 floor, and the player's eye is 186 above it. Nothing about the resting boss
is at body height, so these walls gate on Y and never fire (collision.c's
vertical gate, "body_top > y_max || y_min > body_bot"). They are generated
anyway because they cost 26 rows in a table with 90 spare and they are what
makes the answer right the moment the art comes down to floor level - which is
the whole point of deriving collision from the drawn mesh instead of a proxy.
The rule in tools/ADDING_A_ROOM.txt applies in reverse here: read the height off
the mesh, and do not assume a boss occupies the ground just because it is large.

>>> AND THE WALLS ARE STATIC WHILE THE BOSS IS NOT. <<< They are baked from
frame 1 of Head_Idle_Baked. The slam clip travels the head a long way and NONE
of that moves these walls. That is the right trade while the fight is a clip
cycle and nothing else; when the encounter is real, either accept the resting
silhouette or replace these walls with a runtime capsule the way
rabisus_collide() does. Do NOT try to re-bake walls per frame - CollisionRoom is
a fixed table read by every movement query in the frame.

The generator's output is not hand-editable, so the two edits the runbook says
smx_to_collision.py never emits are applied HERE instead:
  - the lowercase #include (src/ filenames are lowercase)
  - r->multi_level and r->shoot_over_mask, both 0 and both argued for below
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "assets", "bosses", "Asag Version Two")
OUT_C = os.path.join(ROOT, "src", "asag_arena_mesh_collision.c")
OUT_H = os.path.join(ROOT, "src", "asag_arena_mesh_collision.h")

SOURCES = [
    ("Asag Version Two mesh_collision.c",      "the room proxy: perimeter + a recess at each end"),
    # >>> THE BOSS USED TO BE THE SECOND SOURCE AND IS NOT ANY MORE. <<< See the
    # docstring. Its 26 walls were baked from the bind pose; the body now slides
    # up to 969 units into the arena and comes down to floor level, so a static
    # table is wrong in both axes. asag_collide() in src/asag.c replaces them
    # with a per-frame box. Re-adding a row here would fight that collider.
]

WALL_RE = re.compile(
    r"r->walls\[\d+\]\.x1 =\s*(-?\d+);\s*r->walls\[\d+\]\.z1 =\s*(-?\d+);\s*"
    r"r->walls\[\d+\]\.x2 =\s*(-?\d+);\s*r->walls\[\d+\]\.z2 =\s*(-?\d+);\s*"
    r"r->walls\[\d+\]\.nx =\s*(-?\d+);\s*r->walls\[\d+\]\.nz =\s*(-?\d+);\s*"
    r"r->walls\[\d+\]\.y_min =\s*(-?\d+);\s*r->walls\[\d+\]\.y_max =\s*(-?\d+);")

FLOOR_RE = re.compile(
    r"FLOOR\s+\d+: y=\s*(-?\d+)\s+x\((-?\d+) to (-?\d+)\)\s+z\((-?\d+) to (-?\d+)\)")


def main():
    walls, floors, groups = [], [], []
    for fname, what in SOURCES:
        text = open(os.path.join(GEN, fname), encoding="utf-8").read()
        w = WALL_RE.findall(text)
        first = len(walls)
        walls += [(tuple(int(v) for v in m), what) for m in w]
        groups.append((what, first, len(w)))
        if "Two mesh" in fname:
            floors = [tuple(int(v) for v in m) for m in FLOOR_RE.findall(text)]

    xs = [w[0][0] for w in walls] + [w[0][2] for w in walls]
    zs = [w[0][1] for w in walls] + [w[0][3] for w in walls]

    h = ['#ifndef ASAG_ARENA_MESH_COLLISION_H',
         '#define ASAG_ARENA_MESH_COLLISION_H',
         '',
         '#include "collision.h"',
         '',
         '/* GENERATED by tools/gen_asag_arena_collision.py - do not hand-edit.',
         '   See that script for what the three source meshes are and for why the',
         '   tentacle walls do not follow the tentacle animation. */',
         '']
    for what, first, n in groups:
        h.append('/* walls %3d..%-3d  %s */' % (first, first + n - 1, what))
    h += ['',
          '#define ASAG_ARENA_WALL_COUNT  %d' % len(walls),
          '#define ASAG_ARENA_FLOOR_COUNT %d' % len(floors),
          '',
          'void asag_arena_collision_init(CollisionRoom *r);',
          '',
          '#endif /* ASAG_ARENA_MESH_COLLISION_H */',
          '']
    open(OUT_H, "w", encoding="utf-8", newline="\n").write("\n".join(h))

    c = ['/*',
         ' * GENERATED by tools/gen_asag_arena_collision.py - do not hand-edit.',
         ' *',
         ' * Merged from three meshes (see the generator for the argument):']
    for what, first, n in groups:
        c.append(' *   walls %3d..%-3d  %s' % (first, first + n - 1, what))
    c += [' *',
          ' * Walls:  %d   (MAX_WALLS_PER_ROOM is 128)' % len(walls),
          ' * Floors: %d' % len(floors),
          ' * Bounds: X(%d to %d)  Z(%d to %d)' % (min(xs), max(xs), min(zs), max(zs)),
          ' * Normal scale: 4096 = 1.0 (fixed point)',
          ' */',
          '',
          '#include "asag_arena_mesh_collision.h"',
          '',
          'void asag_arena_collision_init(CollisionRoom *r) {',
          '    r->wall_count = ASAG_ARENA_WALL_COUNT;',
          '    r->min_x = %d;' % min(xs),
          '    r->max_x = %d;' % max(xs),
          '    r->min_z = %d;' % min(zs),
          '    r->max_z = %d;' % max(zs),
          '',
          '    /* SINGLE LEVEL. Both floor planes the proxy carries are at y=0 - the',
          '       arena floor and the head alcove\'s - so there is one walkable height',
          '       and asag_arena_floor_zones_init() describes it with two flat zones.',
          '       A terrace, a step or a pit in a later mesh needs this set AND one',
          '       floor zone per level. */',
          '    r->multi_level = 0;',
          '',
          '    /* NO SHORT-WALL EXEMPTION, and that is a measurement, not an oversight.',
          '       collision_shoot_over_short_walls() exists so a projectile is not eaten',
          '       by a knee-high lip. The shortest wall here is the head alcove\'s side',
          '       at 550 tall, which is chest height on a 900-unit room and is meant to',
          '       stop a shot; the tentacles are 200 tall but the player is supposed to',
          '       be unable to shoot THROUGH an arm at floor level either. Nothing here',
          '       is decorative, so nothing is exempt. If the finished fight wants shots',
          '       to clear the resting arms, the call goes in asag_arena_init() AFTER',
          '       this function, which zeroes the mask. */',
          '    r->shoot_over_mask = 0;',
          '']
    for i, (w, what) in enumerate(walls):
        x1, z1, x2, z2, nx, nz, ymin, ymax = w
        c += ['    /* Wall %d - %s */' % (i, what),
              '    r->walls[%d].x1 = %6d;  r->walls[%d].z1 = %6d;' % (i, x1, i, z1),
              '    r->walls[%d].x2 = %6d;  r->walls[%d].z2 = %6d;' % (i, x2, i, z2),
              '    r->walls[%d].nx = %6d;  r->walls[%d].nz = %6d;' % (i, nx, i, nz),
              '    r->walls[%d].y_min = %6d;  r->walls[%d].y_max = %6d;' % (i, ymin, i, ymax),
              '']
    c += ['    /*',
          '     * Floor planes from the room proxy. These are what',
          '     * asag_arena_floor_zones_init() in src/asag_arena.c mirrors.',
          '     *']
    for i, f in enumerate(floors):
        c.append('     * FLOOR %d: y=%d  x(%d to %d)  z(%d to %d)' % (i, f[0], f[1], f[2], f[3], f[4]))
    c += ['     */', '}', '']
    open(OUT_C, "w", encoding="utf-8", newline="\n").write("\n".join(c))

    print("walls %d, floors %d" % (len(walls), len(floors)))
    for what, first, n in groups:
        print("  %3d..%-3d  %s" % (first, first + n - 1, what))
    for i, f in enumerate(floors):
        print("  FLOOR %d: y=%d x(%d..%d) z(%d..%d)" % (i, f[0], f[1], f[2], f[3], f[4]))
    print("wrote %s" % OUT_C)
    print("wrote %s" % OUT_H)


main()
