#!/usr/bin/env python3
"""
pack_rooms.py - put each room's texture map, no-cull table and collision walls
on the DISC, at the end of the room's own .SMD, instead of in the executable.

    python tools/pack_rooms.py            # run by CMake on every build

WHY: a table compiled into HORROR.BIN is in RAM for the whole game, in every
area. These three are only ever read while the player is IN that room, and the
room's mesh already arrives in the room arena on entry (src/room_arena.h). So
they ride in with it: one copy, the current room's, instead of 43 forever. See
tools/RAM_RECLAIM_CHECKLIST.txt PART A.

THE SOURCES DO NOT CHANGE, AND THEY STAY THE SOURCE OF TRUTH:
    src/<slug>_tex_map.h          written by gen_<slug>_tex_map.py
    src/<slug>_mesh_collision.c   written by smx_to_collision.py, then hand-
                                  edited (multi_level, shoot_over_mask, ...)
    assets/<Name>.smd             written by smxlink
This script reads all three and writes build/rooms/<Name>.smd = the SMD bytes
unchanged + one appended block. disc.xml points at build\\rooms\\<Name>.smd.
The packed file has the SAME basename as the asset, which is how the input is
found again on the next run: build\\rooms\\X  <-  assets\\X.

WHICH ROOMS: every src/<slug>_mesh_collision.c. Each must have a
src/<slug>_tex_map.h and a src/<slug>.c with exactly one room_arena_load().

>>> IT FAILS THE BUILD RATHER THAN GUESS. <<< A collision statement that is not
a literal assignment, a wall missing a field, a value outside int16, more than
MAX_WALLS_PER_ROOM walls, or a prim count that disagrees with the SMD's own
header all stop it with the file and line. The one non-literal construct in
the tree today - the kitchen's #ifdef DEBUG_COLLISION loop, live because
collision.h defines DEBUG_COLLISION - is reproduced exactly, by name.

THE BLOCK (little-endian, starts 4-aligned after the SMD bytes):
    +0   u32  'RDAT'
    +4   u16  version (1)          +6   u16  prim count
    +8   u16  wall count           +10  u16  multi_level
    +12  i32  min_x, max_x, min_z, max_z
    +28  u32  shoot_over_mask low, high
    +36  u8   tex map[prim count]       slot, 0xFF = untextured (the header's
                                        values, unchanged)
         u8   no-cull bits[(prims+7)/8] bit i = prim i is never backface-culled
         pad to 4
         i16  walls[wall count][8]      x1 z1 x2 z2 nx nz y_min y_max
    FOOTER, the file's last 8 bytes: u32 block offset, u32 'RDFT'
src/room_data.c reads it. Change one, change both.
"""
import glob
import os
import re
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
OUT_DIR = os.path.join(ROOT, 'build', 'rooms')
MAX_WALLS = 128          # MAX_WALLS_PER_ROOM, src/collision.h
WALL_FIELDS = ('x1', 'z1', 'x2', 'z2', 'nx', 'nz', 'y_min', 'y_max')
VERSION = 1


def die(msg):
    sys.exit('pack_rooms.py: ERROR: ' + msg)


def read(path):
    with open(path, encoding='latin-1') as f:
        return f.read()


def blank_comments(src):
    """Comments to spaces, keeping every newline so line numbers survive."""
    src = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), src, flags=re.S)
    return re.sub(r'//[^\n]*', '', src)


def parse_int(tok, where):
    tok = tok.strip()
    try:
        return int(tok, 0)
    except ValueError:
        die(f'{where}: not an integer literal: {tok!r}')


# ---- the texture map ---------------------------------------------------------
def parse_tex_map(slug):
    path = os.path.join(ROOT, 'src', f'{slug}_tex_map.h')
    src = blank_comments(read(path))
    m = re.search(r'#define\s+%s_PRIM_COUNT\s+(\d+)' % slug.upper(), src)
    if not m:
        die(f'{path}: no {slug.upper()}_PRIM_COUNT')
    count = int(m.group(1))

    def array(name, required):
        a = re.search(r'%s_%s\s*\[\s*\d+\s*\]\s*=\s*\{(.*?)\}' % (slug, name), src, flags=re.S)
        if not a:
            if required:
                die(f'{path}: no {slug}_{name}[]')
            return None
        vals = [parse_int(t, path) for t in a.group(1).split(',') if t.strip()]
        if len(vals) != count:
            die(f'{path}: {slug}_{name} has {len(vals)} entries, PRIM_COUNT is {count}')
        if any(v < 0 or v > 0xFF for v in vals):
            die(f'{path}: {slug}_{name} has a value outside 0..255')
        return vals

    tex = array('tex_map', True)
    nocull = array('nocull', False) or [0] * count
    return count, tex, nocull


# ---- the collision walls -----------------------------------------------------
KITCHEN_DEBUG_LOOP = re.compile(
    r'#ifdef\s+DEBUG_COLLISION\s*\{\s*int\s+i\s*;\s*'
    r'for\s*\(\s*i\s*=\s*0\s*;\s*i\s*<\s*KITCHEN_DINING_WALL_COUNT\s*;\s*i\+\+\s*\)\s*\{\s*'
    r'r->walls\[i\]\.y_max\s*=\s*(-?\d+)\s*;\s*'
    r'r->walls\[i\]\.y_min\s*=\s*(-?\d+)\s*;\s*'
    r'\}\s*\}\s*#endif', re.S)


def parse_collision(slug):
    path = os.path.join(ROOT, 'src', f'{slug}_mesh_collision.c')
    src = blank_comments(read(path))
    m = re.search(r'void\s+%s_collision_init\s*\(\s*CollisionRoom\s*\*\s*r\s*\)\s*\{' % slug, src)
    if not m:
        die(f'{path}: no {slug}_collision_init(CollisionRoom *r)')
    body = src[m.end():src.rindex('}')]

    # The one loop in the tree, applied AFTER every literal (it is last in the
    # function, and C applies statements in order).
    loop = None
    if slug == 'kitchen_dining':
        lm = KITCHEN_DEBUG_LOOP.search(body)
        if not lm:
            die(f'{path}: the DEBUG_COLLISION loop changed shape; update pack_rooms.py')
        loop = (int(lm.group(1)), int(lm.group(2)))     # y_max, y_min
        body = body[:lm.start()] + body[lm.end():]

    room = {}
    walls = {}
    line_of = lambda pos: src[:m.end() + pos].count('\n') + 1
    pos = 0
    for st in body.split(';'):
        here = line_of(pos)
        pos += len(st) + 1
        s = ' '.join(st.split())
        if not s:
            continue
        w = re.fullmatch(r'r->walls\[(\d+)\]\.(\w+) = (-?(?:0x)?[0-9A-Fa-f]+)', s)
        if w:
            idx, field = int(w.group(1)), w.group(2)
            if field not in WALL_FIELDS:
                die(f'{path}:{here}: unknown wall field {field!r}')
            walls.setdefault(idx, {})[field] = parse_int(w.group(3), f'{path}:{here}')
            continue
        r = re.fullmatch(r'r->(\w+) = (-?(?:0x)?[0-9A-Fa-f]+|[A-Z0-9_]+)', s)
        if r:
            field, val = r.group(1), r.group(2)
            if field == 'wall_count':
                if not re.fullmatch(r'%s_WALL_COUNT' % slug.upper(), val):
                    val = parse_int(val, f'{path}:{here}')
                room[field] = val
            elif field in ('min_x', 'max_x', 'min_z', 'max_z',
                           'multi_level', 'shoot_over_mask'):
                room[field] = parse_int(val, f'{path}:{here}')
            else:
                die(f'{path}:{here}: unknown room field {field!r}')
            continue
        die(f'{path}:{here}: not a literal assignment: {s!r}')

    # wall_count is the header's #define in every generated file.
    hdr = read(os.path.join(ROOT, 'src', f'{slug}_mesh_collision.h'))
    hm = re.search(r'#define\s+%s_WALL_COUNT\s+(\d+)' % slug.upper(), hdr)
    if not hm:
        die(f'{slug}_mesh_collision.h: no {slug.upper()}_WALL_COUNT')
    if isinstance(room.get('wall_count'), str) or 'wall_count' not in room:
        room['wall_count'] = int(hm.group(1))
    n = room['wall_count']
    if n > MAX_WALLS:
        die(f'{path}: {n} walls, MAX_WALLS_PER_ROOM is {MAX_WALLS}')
    if set(walls) != set(range(n)):
        extra = sorted(set(walls) ^ set(range(n)))
        die(f'{path}: wall indices do not match wall_count {n} (odd ones: {extra[:8]})')
    for i in range(n):
        for f in WALL_FIELDS[:6]:
            if f not in walls[i]:
                die(f'{path}: wall {i} has no {f}')
        if loop:
            walls[i]['y_max'], walls[i]['y_min'] = loop
        # No Y data at all means y_min == y_max == 0: "full height"
        # (collision.h). The generated code always writes both or neither.
        if ('y_min' in walls[i]) != ('y_max' in walls[i]):
            die(f'{path}: wall {i} has only one of y_min / y_max')
        walls[i].setdefault('y_min', 0)
        walls[i].setdefault('y_max', 0)
        for f in WALL_FIELDS:
            v = walls[i][f]
            if not -32768 <= v <= 32767:
                die(f'{path}: wall {i}.{f} = {v} does not fit int16')
    for f in ('min_x', 'max_x', 'min_z', 'max_z'):
        if f not in room:
            die(f'{path}: no r->{f}')
    room.setdefault('multi_level', 0)
    room.setdefault('shoot_over_mask', 0)
    return room, [walls[i] for i in range(n)]


# ---- which disc file, and which asset -----------------------------------------
def disc_file(slug, disc_xml):
    path = os.path.join(ROOT, 'src', f'{slug}.c')
    loads = re.findall(r'room_arena_load\(\s*"([^"]+)"\s*\)', blank_comments(read(path)))
    if len(loads) != 1:
        die(f'{path}: expected exactly one room_arena_load(), found {len(loads)}')
    name = loads[0].replace('\\\\', '\\').split('\\')[-1].split(';')[0]
    hits = re.findall(r'<file\s+name="%s"[^>]*?source="([^"]+)"' % re.escape(name), disc_xml)
    if len(hits) != 1:
        die(f'disc.xml: expected one <file name="{name}"> for {slug}, found {len(hits)}')
    # The path BELOW assets\ (or below build\rooms\, once disc.xml points at the
    # packed copy) is the same on both sides, subfolders included
    # (assets\bosses\..., assets\catacombs\...).
    src = os.path.normpath(hits[0].replace('\\', os.sep).replace('/', os.sep))
    rel = None
    for base in (os.path.join(ROOT, 'assets'), OUT_DIR):
        b = os.path.normpath(base)
        if os.path.normcase(src).startswith(os.path.normcase(b) + os.sep):
            rel = src[len(b) + 1:]
    if rel is None:
        die(f'disc.xml: {name} is sourced from {hits[0]}, which is under neither '
            f'assets\\ nor build\\rooms\\')
    return name, os.path.join(ROOT, 'assets', rel), os.path.join(OUT_DIR, rel)


def pack(slug, disc_xml):
    count, tex, nocull = parse_tex_map(slug)
    room, walls = parse_collision(slug)
    name, smd_path, out_path = disc_file(slug, disc_xml)
    with open(smd_path, 'rb') as f:
        smd = f.read()
    if smd[:3] != b'SMD':
        die(f'{smd_path}: not an SMD')
    n_prims = struct.unpack_from('<H', smd, 10)[0]
    if n_prims != count:
        die(f'{slug}: {smd_path} has {n_prims} prims but {slug}_tex_map.h says '
            f'{count} - re-run gen_{slug}_tex_map.py (or smxlink)')

    mask = room['shoot_over_mask'] & 0xFFFFFFFFFFFFFFFF
    blk = bytearray()
    blk += b'RDAT'
    blk += struct.pack('<HHHH', VERSION, count, len(walls), room['multi_level'] & 0xFFFF)
    blk += struct.pack('<iiii', room['min_x'], room['max_x'], room['min_z'], room['max_z'])
    blk += struct.pack('<II', mask & 0xFFFFFFFF, mask >> 32)
    blk += bytes(tex)
    bits = bytearray((count + 7) // 8)
    for i, v in enumerate(nocull):
        if v:
            bits[i >> 3] |= 1 << (i & 7)
    blk += bits
    while len(blk) % 4:
        blk += b'\0'
    for w in walls:
        blk += struct.pack('<8h', *(w[f] for f in WALL_FIELDS))

    offset = (len(smd) + 3) & ~3
    data = smd + b'\0' * (offset - len(smd)) + blk
    data += struct.pack('<I', offset) + b'RDFT'
    old = None
    if os.path.exists(out_path):
        with open(out_path, 'rb') as f:
            old = f.read()
    if old != data:                       # leave the timestamp alone if equal
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, 'wb') as f:
            f.write(data)
    return name, len(smd), len(data), len(walls), count


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    disc_xml = read(os.path.join(ROOT, 'disc.xml'))
    slugs = sorted(os.path.basename(p)[:-len('_mesh_collision.c')]
                   for p in glob.glob(os.path.join(ROOT, 'src', '*_mesh_collision.c')))
    total = 0
    for slug in slugs:
        name, smd_len, out_len, nw, np_ = pack(slug, disc_xml)
        total += out_len - smd_len
        if '-v' in sys.argv:
            print(f'  {name:13s} {smd_len:7d} + {out_len - smd_len:5d}  '
                  f'{np_:5d} prims {nw:4d} walls  {slug}')
    print(f'pack_rooms.py: {len(slugs)} rooms packed into build/rooms '
          f'({total} bytes of room data moved out of the executable)')


if __name__ == '__main__':
    main()
