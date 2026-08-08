#!/usr/bin/env python3
"""
gen_tim_slots.py - bake every TIM's tpage/clut into a compile-time header.

WHY THIS EXISTS
---------------
A texture's tpage and clut are pure functions of the VRAM rectangle baked into
its .tim file at build time. Nothing at runtime can change them. Yet the game
used to discover them the expensive way: twelve room modules each defined a
local `capture_tpage()` that read the ENTIRE .tim off the CD, handed it to
GetTimInfo, kept eight bytes of the answer, and freed the buffer. Fifty-one of
those calls ran during the red LOADING screen, every one a directory search plus
a seek plus a full-file read, all to recover two 16-bit constants.

This script computes the same two constants from the same .tim files and writes
them into src/tim_slots.h, so the room modules can just say

    tex_tpage[0] = TIM_TPAGE_CNCRTE;
    tex_clut [0] = TIM_CLUT_CNCRTE;

REGENERATE AFTER MOVING ANY TEXTURE IN VRAM:

    py tools/gen_tim_slots.py

A texture that moves and is not regenerated here renders from the WRONG VRAM
page in every room that reads its constants from this header, with no crash and
no warning - exactly the class of bug tools/VRAM_MAP.txt exists to catch. Run
`py tools/vram_map.py > tools/VRAM_MAP.txt` alongside this.

WHAT IT READS
-------------
disc.xml, which is the authoritative map from the on-disc path the game opens
("\\TEX\\CNCRTE.TIM;1") to the source file on the host ("textures/cncrte.tim").
Deriving the name from disc.xml rather than from a hand-kept list means a
texture that is renamed or moved between the root and \\TEX\\ cannot silently
drift out of step with the header.

THE FORMULAS
------------
Straight from psxgpu.h, and they must stay in step with it:

    getTPage(tp, abr, x, y) = ((x & 0x3c0) >> 6) | ((y & 0x100) >> 4)
                            | ((y & 0x200) << 2) | ((abr & 3) << 5)
                            | ((tp & 3) << 7)
    getClut(x, y)           = (y << 6) | ((x >> 4) & 0x3f)

`tp` is the TIM flags' low two bits (0 = 4bpp, 1 = 8bpp, 2 = 16bpp), which is
the same encoding getTPage wants, and `abr` is 0 - matching every call site the
game makes: getTPage(tim.mode & 0x3, 0, prect->x, prect->y).
"""
import os
import struct
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
DISC = os.path.join(ROOT, "disc.xml")
OUT = os.path.join(ROOT, "src", "tim_slots.h")


def get_tpage(tp, abr, x, y):
    return (((x & 0x3C0) >> 6) | ((y & 0x100) >> 4) | ((y & 0x200) << 2)
            | ((abr & 3) << 5) | ((tp & 3) << 7))


def get_clut(x, y):
    return ((y << 6) | ((x >> 4) & 0x3F))


def read_tim(path):
    """Parse a TIM header. Returns (bpp_code, has_clut, px, py, cx, cy)."""
    with open(path, "rb") as f:
        d = f.read()
    if not d or d[0] != 0x10:
        return None
    flags = struct.unpack("<I", d[4:8])[0]
    bpp_code = flags & 3
    has_clut = bool(flags & 8)
    off = 8
    cx = cy = 0
    if has_clut:
        bs = struct.unpack("<I", d[off:off + 4])[0]
        cx, cy = struct.unpack("<HH", d[off + 4:off + 8])
        off += bs
    px, py = struct.unpack("<HH", d[off + 4:off + 8])
    return bpp_code, has_clut, px, py, cx, cy


def collect_tims():
    """Walk disc.xml and yield (disc_path, symbol, source_path) for each TIM."""
    tree = ET.parse(DISC)
    out = []

    def walk(node, prefix):
        for child in node:
            if child.tag == "dir":
                walk(child, prefix + "\\" + child.get("name"))
            elif child.tag == "file":
                name = child.get("name")
                if not name.upper().endswith(".TIM"):
                    continue
                out.append((prefix + "\\" + name,
                            os.path.splitext(name)[0].upper(),
                            child.get("source")))

    walk(tree.find(".//directory_tree"), "")
    return out


def main():
    entries = collect_tims()
    if not entries:
        sys.exit("gen_tim_slots.py: no .tim files found in disc.xml")

    # A duplicate symbol would mean two different textures compiling to one
    # #define - the second silently winning. Refuse rather than emit that.
    seen = {}
    for disc_path, sym, _ in entries:
        if sym in seen:
            sys.exit("gen_tim_slots.py: duplicate symbol %s (%s and %s). "
                     "Rename one on the disc." % (sym, seen[sym], disc_path))
        seen[sym] = disc_path

    rows = []
    for disc_path, sym, source in entries:
        if not os.path.isfile(source):
            sys.exit("gen_tim_slots.py: %s missing (disc.xml says %s)"
                     % (source, disc_path))
        info = read_tim(source)
        if info is None:
            sys.exit("gen_tim_slots.py: %s is not a TIM" % source)
        bpp_code, has_clut, px, py, cx, cy = info
        rows.append((sym, disc_path, os.path.basename(source),
                     {0: 4, 1: 8, 2: 16, 3: 16}[bpp_code], px, py,
                     get_tpage(bpp_code, 0, px, py),
                     get_clut(cx, cy) if has_clut else 0,
                     has_clut))

    rows.sort()
    width = max(len(r[0]) for r in rows)

    with open(OUT, "w", newline="\n") as f:
        f.write("""/* tim_slots.h - GENERATED by tools/gen_tim_slots.py. DO NOT EDIT BY HAND.
 *
 * Every texture's tpage and clut, baked at build time. These are pure functions
 * of the VRAM rectangle inside each .tim, so they cannot change at runtime and
 * there is nothing to discover on the CD - which is the point: reading a whole
 * TIM off the disc to recover two 16-bit constants cost the loading screen
 * fifty-one seeks before this header existed.
 *
 * REGENERATE WHENEVER A TEXTURE MOVES IN VRAM:
 *     py tools/gen_tim_slots.py
 *     py tools/vram_map.py > tools/VRAM_MAP.txt
 *
 * A stale entry here does NOT crash. It renders that texture from the wrong
 * VRAM page in every room that uses it. See tools/TEXTURING_NOTES.txt.
 *
 * TIM_CLUT_* is 0 for a 16bpp texture, which has no palette - the same value
 * the old capture_tpage() left in the slot, since it only assigned the clut
 * when the TIM's mode had bit 3 set.
 */
#ifndef TIM_SLOTS_H
#define TIM_SLOTS_H

/* Fill one entry of a room's tex_tpage[]/tex_clut[] pair.
 *
 * This is the drop-in for the twelve identical capture_tpage() helpers it
 * replaced, and it keeps their contract exactly: the caller must have file-scope
 * arrays named tex_tpage and tex_clut, which is what every room module already
 * declares next to its <ROOM>_TEX_COUNT. NAME is the bare symbol suffix, so
 *
 *     capture_tpage("\\\\TEX\\\\CNCRTE.TIM;1", 0);   becomes   TIM_SLOT(0, CNCRTE);
 *
 * A typo in NAME is a compile error, where a typo in the old path string was a
 * silent no-op that left the slot pointing at tpage 0.
 */
#define TIM_SLOT(slot, NAME) \\
    (tex_tpage[slot] = TIM_TPAGE_##NAME, tex_clut[slot] = TIM_CLUT_##NAME)

""")
        for sym, disc_path, src, bpp, px, py, tpage, clut, has_clut in rows:
            f.write("/* %-14s %2dbpp  VRAM (%4d,%3d)  %s */\n"
                    % (src, bpp, px, py, disc_path))
            f.write("#define TIM_TPAGE_%-*s 0x%04x\n" % (width, sym, tpage))
            if has_clut:
                f.write("#define TIM_CLUT_%-*s  0x%04x\n" % (width, sym, clut))
            else:
                f.write("#define TIM_CLUT_%-*s  0x0000  /* 16bpp: no palette */\n"
                        % (width, sym))
            f.write("\n")
        f.write("#endif /* TIM_SLOTS_H */\n")

    print("gen_tim_slots.py: wrote %s (%d textures)" % (OUT, len(rows)))


if __name__ == "__main__":
    main()
