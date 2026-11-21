#!/usr/bin/env python3
"""
pack_models.py - the prop, boss and sky .smd files the disc ships, minus the
normals array none of their draws read.

    python tools/pack_models.py            # run by CMake on every build

WHY: every .smd smxlink writes carries a normals array, ~28% of the file. Only
four places in the game read normals (NEEDS_NORMALS below); every other model
just costs that much more RAM while it is held - and the 25 prop models are
held for whole chapters. tools/RAM_AB_CHECKLIST.txt PART B. Rooms get the same
treatment inside tools/pack_rooms.py, which owns strip_normals().

WHAT IT WRITES: for each name in ALLOWLIST, build/models/<path below assets\\>
= that asset with its normals cut out. disc.xml points the entry at the packed
copy; assets/ itself is never modified, smxlink's output stays the source. The
packed file has the same path below build\\models\\ as the asset has below
assets\\, which is how the input is found again once disc.xml points here.

>>> A MODEL GOES ON THE ALLOWLIST ONLY IF NOTHING THAT DRAWS IT READS NORMALS.
<<< Today that is everything except NEEDS_NORMALS. The script also re-derives
WHICH SOURCE FILES read normals (p_norms, a raw p+12 normal index, or the SDK's
smdSortModel, which lights with them) and fails if that set changes - so a new
reader cannot quietly start drawing a stripped model with garbage lighting.

IT FAILS THE BUILD RATHER THAN GUESS: an allowlisted name disc.xml does not
have, one sourced from neither assets\\ nor build\\models\\, a NEEDS_NORMALS
model pointed at build\\models\\, or an SMD layout strip_normals() does not
recognise.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pack_rooms import strip_normals, read, ROOT  # noqa: E402


def die(msg):
    sys.exit('pack_models.py: ERROR: ' + msg)

OUT_DIR = os.path.join(ROOT, 'build', 'models')

ALLOWLIST = [
    # props: kept for the run, or for Chapters 1-2 (tools/HEAP_BUDGET.txt)
    'ARMSWTCH.SMD', 'BARS.SMD', 'BOOKCSE.SMD', 'CHNLNKDR.SMD', 'CLEAVER.SMD',
    'CONBLOCK.SMD', 'CONCHAIR.SMD', 'CRIB.SMD', 'DINTABLE.SMD', 'DRESSER.SMD',
    'FATDOOR.SMD', 'GRINDER.SMD', 'GULATBLT.SMD', 'INCINPRP.SMD', 'LEVER.SMD',
    'OILDISP.SMD', 'PIANO.SMD', 'SAVEPT.SMD', 'SCONCE.SMD', 'SHARTBLT.SMD',
    'TABLETS.SMD', 'TRKDRWR.SMD', 'VALVEH.SMD', 'VINES.SMD', 'WINGTBLT.SMD',
    # room-scoped props: the arena extra and the door pairs
    'LTNGTBLT.SMD', 'HATCHDL.SMD', 'HATCHDR.SMD', 'CTCMBDL.SMD', 'CTCMBDR.SMD',
    # bosses: read at the door of the room they fight in
    'RABISU.SMD', 'ASGBODY.SMD', 'LAMASHTU.SMD',
    # skies and set dressing
    'DSCTUBE.SMD', 'UWSKY.SMD', 'CDSKY.SMD',
]

# Drawn by something that reads normals. NEVER strip these.
NEEDS_NORMALS = {
    'CRFAXE.SMD':   'weapon.c weapon_render_model (via crucifaxe.c melee_draw_model)',
    'SHARURWP.SMD': 'weapon.c weapon_render_model (via sharur.c melee_draw_model)',
    'GRAVOLVR.SMD': 'weapon.c weapon_render_model (graveolver.c)',
    'HELLUM.SMD':   'helluminator.c reads p_norms',
    'CRATE.SMD':    'crate.c smdSortModel (NCDS lighting)',
}

# The source files that read normals, as of 21 Nov 2026. If this changes, find
# out what the new reader draws before touching the lists above.
NORMAL_READERS = {'crate.c', 'helluminator.c', 'sharur.c', 'weapon.c'}


def blank_comments(src):
    return re.sub(r'/\*.*?\*/', ' ', re.sub(r'//[^\n]*', '', src), flags=re.S)


def check_readers():
    found = set()
    for name in sorted(os.listdir(os.path.join(ROOT, 'src'))):
        if not name.endswith('.c'):
            continue
        s = blank_comments(read(os.path.join(ROOT, 'src', name)))
        if (re.search(r'\bp_norms\b|\bn_norms\b|\bsmdSortModel\w*\s*\(|'
                      r'\(\s*p\s*\+\s*12\s*\)', s)):
            found.add(name)
    if found != NORMAL_READERS:
        die('pack_models.py: the set of files that read SMD normals changed:\n'
            f'    now      {sorted(found)}\n    expected {sorted(NORMAL_READERS)}\n'
            'Find what the new reader draws: if it is on ALLOWLIST, move it to '
            'NEEDS_NORMALS. Then update NORMAL_READERS.')


def entries(disc_xml):
    out = {}
    for m in re.finditer(r'<file\s+name="([^"]+)"[^>]*?source="([^"]+)"', disc_xml):
        out.setdefault(m.group(1).upper(), []).append(m.group(2))
    return out


def rel_of(src_path):
    src = os.path.normpath(src_path.replace('\\', os.sep).replace('/', os.sep))
    for base in (os.path.join(ROOT, 'assets'), OUT_DIR):
        b = os.path.normpath(base)
        if os.path.normcase(src).startswith(os.path.normcase(b) + os.sep):
            return src[len(b) + 1:], os.path.normcase(b) == os.path.normcase(os.path.normpath(OUT_DIR))
    return None, False


def main():
    check_readers()
    clash = set(ALLOWLIST) & set(NEEDS_NORMALS)
    if clash:
        die(f'pack_models.py: {sorted(clash)} are on both ALLOWLIST and NEEDS_NORMALS')
    disc = entries(read(os.path.join(ROOT, 'disc.xml')))

    for name, why in NEEDS_NORMALS.items():
        for src in disc.get(name, []):
            if rel_of(src)[1]:
                die(f'disc.xml: {name} reads build\\models\\ but its draw needs '
                    f'normals ({why}) - point it back at assets\\')

    before = after = 0
    for name in ALLOWLIST:
        srcs = disc.get(name, [])
        if len(srcs) != 1:
            die(f'disc.xml: expected one <file name="{name}">, found {len(srcs)}')
        rel, _ = rel_of(srcs[0])
        if rel is None:
            die(f'disc.xml: {name} is sourced from {srcs[0]}, which is under '
                f'neither assets\\ nor build\\models\\')
        asset = os.path.join(ROOT, 'assets', rel)
        with open(asset, 'rb') as f:
            smd = f.read()
        data = strip_normals(smd, asset)
        before += len(smd)
        after += len(data)
        out_path = os.path.join(OUT_DIR, rel)
        old = None
        if os.path.exists(out_path):
            with open(out_path, 'rb') as f:
                old = f.read()
        if old != data:                     # leave the timestamp alone if equal
            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            with open(out_path, 'wb') as f:
                f.write(data)
        if '-v' in sys.argv:
            print(f'  {name:13s} {len(smd):6d} -> {len(data):6d}  {rel}')
    print(f'pack_models.py: {len(ALLOWLIST)} models packed into build/models '
          f'({before - after} bytes of unused normals removed)')


if __name__ == '__main__':
    main()
