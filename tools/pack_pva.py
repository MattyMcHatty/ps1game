#!/usr/bin/env python3
"""
pack_pva.py - convert a PVA1 vertex-animation clip to PVA2 (in place).

    python tools/pack_pva.py "assets/bosses/Rabisu_idle.pva"
    python tools/pack_pva.py --check "assets/bosses/Rabisu_idle.pva"

WHAT THE TWO FORMATS ARE
------------------------
Both are a 12-byte header followed by n_frames blocks of n_verts vertices:

    offset 0   magic  "PVA1" or "PVA2"
    offset 4   uint16 n_verts
    offset 6   uint16 n_frames
    offset 8   4 bytes, format-defined, copied through untouched
    offset 12  the frames

PVA1 stores FOUR int16 per vertex - x, y, z and a pad that is ALWAYS ZERO,
which makes a frame bit-for-bit an array of PSn00bSDK SVECTORs, so a reader can
point the GTE straight into the file. PVA2 drops the pad and stores three. Same
coordinates, same order, a quarter smaller, and the reader unpacks one frame
into a scratch buffer before drawing it.

WHY YOU WOULD RUN THIS
----------------------
Main RAM is this project's tightest resource and a clip is the largest thing a
boss reads - see tools/HEAP_BUDGET.txt and tools/ADDING_THE_ASAG_FIGHT.txt
PART 6. The pad is a quarter of the payload and it is bytes spent on zeroes, at
the top of the heap, where the ceiling is. Asag's six clips were packed for
exactly this reason (tools/export_asag.py asks the Blender exporter for
pack=True); the Rabisu's was converted with this script because it had already
been exported and there was no reason to re-bake the animation to save the
bytes.

THE BLENDER ADD-ON STILL WRITES PVA1 BY DEFAULT (tools/io_export_pva.py), so a
hand export is PVA1 and this is how it gets over the line afterwards.

THE PAD IS VERIFIED, NOT ASSUMED. If any fourth component is non-zero the file
is not what PVA1 claims to be and dropping it would silently change the
animation, so the conversion refuses rather than guessing. --check reports
without writing.

READERS MUST ACCEPT BOTH. src/asag.c and src/rabisu.c branch on the magic and
pick a stride of 4 or 3; a PVA1 clip dropped back in keeps working, it is just
bigger. Re-run this after any re-export, or the file silently grows again.
"""

import argparse
import os
import struct
import sys

HEADER_SIZE = 12


def read_header(blob, path):
    if len(blob) < HEADER_SIZE:
        sys.exit("%s: too short to be a PVA file (%d bytes)" % (path, len(blob)))
    magic = blob[0:4].decode("ascii", "replace")
    n_verts, n_frames = struct.unpack_from("<HH", blob, 4)
    return magic, n_verts, n_frames


def pack(path, check_only):
    with open(path, "rb") as f:
        blob = f.read()

    magic, n_verts, n_frames = read_header(blob, path)

    if magic == "PVA2":
        print("%s: already PVA2 (%d verts x %d frames, %d bytes) - nothing to do"
              % (os.path.basename(path), n_verts, n_frames, len(blob)))
        return 0
    if magic != "PVA1":
        sys.exit("%s: not a PVA clip (magic %r)" % (path, magic))
    if n_verts <= 0 or n_frames <= 0:
        sys.exit("%s: header says %d verts x %d frames" % (path, n_verts, n_frames))

    expect = HEADER_SIZE + n_frames * n_verts * 8
    if len(blob) != expect:
        sys.exit("%s: header says %d verts x %d frames = %d bytes, file is %d"
                 % (path, n_verts, n_frames, expect, len(blob)))

    # >>> THE PAD IS CHECKED BEFORE IT IS DROPPED. <<< A non-zero fourth
    # component would mean the exporter put something there, and throwing it
    # away would change the animation in a way nothing downstream could detect.
    total = n_frames * n_verts
    comps = struct.unpack_from("<%dh" % (total * 4), blob, HEADER_SIZE)
    bad = [i for i in range(total) if comps[i * 4 + 3] != 0]
    if bad:
        sys.exit("%s: %d of %d vertices have a NON-ZERO pad (first at index %d, "
                 "value %d). This is not a paddable PVA1 - refusing."
                 % (path, len(bad), total, bad[0], comps[bad[0] * 4 + 3]))

    out = bytearray()
    out += b"PVA2"
    out += struct.pack("<HH", n_verts, n_frames)
    out += blob[8:HEADER_SIZE]
    for v in range(total):
        out += struct.pack("<hhh", comps[v * 4], comps[v * 4 + 1], comps[v * 4 + 2])

    saved = len(blob) - len(out)
    print("%s: %d verts x %d frames" % (os.path.basename(path), n_verts, n_frames))
    print("  PVA1 %7d bytes" % len(blob))
    print("  PVA2 %7d bytes" % len(out))
    print("  SAVED %6d bytes (%.1f%%)" % (saved, 100.0 * saved / len(blob)))

    if check_only:
        print("  --check: not written")
        return 0

    with open(path, "wb") as f:
        f.write(out)
    print("  written in place")
    return 0


def main():
    ap = argparse.ArgumentParser(description="Convert a PVA1 clip to PVA2 in place.")
    ap.add_argument("files", nargs="+", help="the .pva file(s) to convert")
    ap.add_argument("--check", action="store_true",
                    help="report the saving and verify the pad, but do not write")
    args = ap.parse_args()
    for path in args.files:
        pack(path, args.check)
    return 0


if __name__ == "__main__":
    sys.exit(main())
