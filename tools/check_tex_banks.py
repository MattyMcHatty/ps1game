#!/usr/bin/env python3
"""
check_tex_banks.py - verify every texture bank mask against the real call graph.

WHY THIS EXISTS
---------------
src/texmgr.c holds one texture BANK at a time instead of the whole map (see the
long note in src/texmgr.h, and src/area_bank.h for which room is in which bank).
Each registering module declares the banks its textures belong to with a
texmgr_set_bank() call at the top of its *_load_assets().

>>> GETTING THAT MASK WRONG IS SILENT. <<< texmgr_upload() on an entry whose
bank is not loaded does nothing at all: no crash, no message. The room simply
draws with whatever the PREVIOUS room left in that VRAM page, which looks like a
texturing bug and sends you to tools/VRAM_MAP.txt, which will tell you
everything is fine. It is the same failure sound_play() has on an evicted clip
and it needs the same answer: do not reason about the masks, check them.

AND THE MASKS ARE NOT GUESSABLE. Rooms borrow each other's textures through
narrow uploaders and those chains cross the map:

    garden_stairs_upload_textures() -> delivery_upload_brick_wall()
                                    -> east_stairwell_upload_chnlnk()
    delivery_restore_textures()     -> garden_stairs_upload_grss_gs()
    outside_catacombs_upload_...()  -> conservatory_upload_con_tile()

So the DELIVERY AREA's registrations are needed in the garden's bank, the GARDEN
STAIRS' in the mansion's, and the CONSERVATORY's in both. Three of the six banks
contain modules from the other side of the map.

WHAT IT DOES
------------
  1. Parses every function in src/*.c that calls texmgr_upload() or another
     upload/restore function, building a call graph.
  2. For each AREA, walks that graph from the entry points listed in AREAS
     below and collects the MODULES whose own registrations get uploaded.
  3. Reads the declared texmgr_set_bank(...) mask out of each module.
  4. Fails if a declared mask does not cover every area that reaches it.

It also prints what each bank costs, from the .tim sizes on disc, which is the
number to watch: the PEAK bank is what sets free-at-rest, not the total.

RUN IT AFTER TOUCHING ANY *_upload_* OR *_restore_* FUNCTION, and after adding
a room. Exit status is non-zero on a mismatch, so it can gate a build.

    py tools/check_tex_banks.py

MAINTENANCE: AREAS below is hand-kept and must match area_bank_of() in
src/area_bank.c. There is no way to derive it - which room is in which bank is
a design decision, not a fact about the code - so a new room needs a line in
both. The script checks that every *_upload_textures() in src/ is listed in
SOME area and complains if one is orphaned, which is the half that can be
automated.
"""
import os
import re
import sys
import glob
import struct
import xml.etree.ElementTree as ET

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SRC = os.path.join(ROOT, "src")

# --- the areas, and the uploader each of their rooms runs on entry ----------
# Keep in step with area_bank_of() in src/area_bank.c.
AREAS = {
    "MANSION": [
        "delivery_restore_textures", "delivery_upload_brick_wall",
        "kitchen_restore_textures", "reception_upload_textures",
        "piano_room_upload_textures", "conservatory_upload_textures",
        "hall_2f_upload_textures", "master_bedroom_upload_textures",
        "east_hall_upload_textures", "library_upload_textures",
        "library_destroyed_upload_textures", "east_stairwell_upload_textures",
        "attic_stairwell_upload_textures", "attic_exit_upload_textures",
        "west_corridor_upload_textures",
        # main.c streams the spider pair into every room but the flower rooms
        # and the arena.
        "spiders_upload_textures",
        # ...and the ZOMBIE pair, on its own line because it is its own pair of
        # slots (x704/x768 y128, which the crawler's second sheet takes). It is
        # listed under MANSION and nowhere else, and that is exact rather than
        # approximate: main.c gates the call on
        # area_bank_of(pending_area) & TEXBANK_MANSION, so it genuinely does not
        # run in the garden, the arena or the Catacombs. Widening that gate
        # means widening this list AND the mask in zombies_load_textures, which
        # would make ~34 KB of zombie sprites resident in banks that never draw
        # one.
        "zombies_upload_textures",
    ],
    "GARDEN": [
        "garden_stairs_upload_textures", "fountain_square_upload_textures",
        "outside_catacombs_upload_textures", "maze_one_upload_textures",
        "maze_two_upload_textures", "keystone_maze_upload_textures",
        "chain_room_upload_textures", "the_hatch_upload_textures",
        "rear_gate_upload_textures",
        "spiders_upload_textures", "rafflesias_upload_textures",
    ],
    "RABISU": ["garden_courtyard_upload_textures", "spiders_upload_textures"],
    "WEST_GARDEN": ["stables_upload_textures", "greenhouse_upload_textures",
                    "rafflesias_upload_textures"],
    "ASAG": ["asag_arena_upload_textures"],
    # Chapter 3. The Up Down Maze registers nothing of its own: its uploader
    # calls the Catacombs Entry's two NARROW ones, so listing it here is what
    # makes the walk below reach them from this area as well.
    # main.c streams the CRAWLER's sheet into both Catacombs rooms, in the same
    # line and on the same terms that it streams the spider pair everywhere else
    # and the flowers into the garden rooms — one enemy's art per room entry into
    # the shared x320 y128 slot.
    "CATACOMBS": ["catacombs_entry_upload_textures",
                  "up_down_maze_upload_textures",
                  "incinerator_room_upload_textures",
                  # The Tomb registers nothing of its own either, and it is the
                  # first room besides the burial hall to draw the LOCULUS -
                  # hence the third narrow uploader on catacombs_entry.c that
                  # the walk below reaches through this entry.
                  "tomb_upload_textures",
                  "crawlers_upload_textures",
                  # ...and the LUMBERER, which main.c streams into the TOMB in
                  # the crawler's place rather than beside it: the two share
                  # every VRAM rectangle, so exactly one of them is in the pages
                  # at a time and no Catacombs room may hold both. Listed here
                  # for the same reason the crawler is - the module registers its
                  # own art, so this entry is what puts it in this area's bank.
                  "lumberers_upload_textures"],
}

BIT = {"MANSION": 1 << 0, "GARDEN": 1 << 1, "RABISU": 1 << 2,
       "WEST_GARDEN": 1 << 3, "ASAG": 1 << 4, "CATACOMBS": 1 << 5}
NAME = {v: k for k, v in BIT.items()}
CONST = {"TEXBANK_MANSION": BIT["MANSION"], "TEXBANK_GARDEN": BIT["GARDEN"],
         "TEXBANK_RABISU": BIT["RABISU"], "TEXBANK_WEST_GARDEN": BIT["WEST_GARDEN"],
         "TEXBANK_ASAG": BIT["ASAG"], "TEXBANK_CATACOMBS": BIT["CATACOMBS"],
         "TEXBANK_RESIDENT": 0}


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', s)


def scan():
    """-> (fn -> module, fn -> [called fns], fn -> uploads_own_regs,
            module -> declared mask or None)"""
    fn_mod, fn_calls, fn_up, declared = {}, {}, {}, {}
    for path in sorted(glob.glob(os.path.join(SRC, "*.c"))):
        raw = open(path, encoding="utf-8", errors="replace").read()
        src = strip_comments(raw)
        mod = os.path.basename(path)[:-2]

        m = re.search(r'texmgr_set_bank\s*\(([^)]*)\)', src)
        if m:
            mask, bad = 0, []
            for tok in re.findall(r'TEXBANK_\w+', m.group(1)):
                if tok not in CONST:
                    bad.append(tok)
                mask |= CONST.get(tok, 0)
            if bad:
                sys.exit("%s: unknown bank constant %s" % (mod, ", ".join(bad)))
            declared[mod] = mask

        # Every function body at file scope. Brace-matched from the signature so
        # a nested block does not end it early.
        for sig in re.finditer(r'^[A-Za-z_][\w \*]*?\b(\w+)\s*\([^;{]*\)\s*\{', src, re.M):
            name, i, depth = sig.group(1), sig.end() - 1, 0
            while i < len(src):
                if src[i] == '{':
                    depth += 1
                elif src[i] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = src[sig.end():i]
            if 'texmgr_upload' not in body and '_upload' not in body and '_restore' not in body:
                continue
            fn_mod[name] = mod
            fn_up[name] = 'texmgr_upload' in body
            fn_calls[name] = [c for c in re.findall(r'(\w+)\s*\(', body)
                              if c != 'texmgr_upload'
                              and ('_upload' in c or '_restore' in c)]
    return fn_mod, fn_calls, fn_up, declared


def closure(entries, fn_mod, fn_calls, fn_up):
    seen, mods, stack = set(), set(), list(entries)
    while stack:
        f = stack.pop()
        if f in seen or f not in fn_mod:
            continue
        seen.add(f)
        if fn_up[f]:
            mods.add(fn_mod[f])
        stack += fn_calls[f]
    return mods


def tim_sizes():
    """disc name -> resident bytes, rounded to whole sectors as CdRead reads."""
    tree = ET.parse(os.path.join(ROOT, "disc.xml"))
    out = {}

    def walk(node):
        for child in node:
            if child.tag == "dir":
                walk(child)
            elif child.tag == "file" and child.get("name", "").upper().endswith(".TIM"):
                src = child.get("source")
                if src and os.path.exists(src):
                    n = os.path.getsize(src)
                    out[child.get("name").upper()] = ((n + 2047) // 2048) * 2048
    walk(tree.find(".//directory_tree"))
    return out


def module_bytes():
    """module -> bytes its registrations hold when its bank is in."""
    sizes, out = tim_sizes(), {}
    LIT = re.compile(r'"(\\\\(?:[A-Za-z0-9_]+\\\\)?[A-Za-z0-9_ ]+\.TIM)(?:;1)?"')
    for path in sorted(glob.glob(os.path.join(SRC, "*.c"))):
        src = strip_comments(open(path, encoding="utf-8", errors="replace").read())
        mod = os.path.basename(path)[:-2]
        names = []
        for m in re.finditer(r'texmgr_register\s*\(\s*"([^"]+)"', src):
            names.append(m.group(1))
        if re.search(r'texmgr_register\s*\(\s*(?:new_tex|shared_tex|raf_tex)', src):
            for m in re.finditer(
                    r'(?:new_tex|shared_tex|raf_tex)[a-z_]*\[[^\]]*\]\s*=\s*\{(.*?)\};',
                    src, re.S):
                names += LIT.findall(m.group(1))
        tot = 0
        for n in names:
            base = n.replace('\\\\', '\\').split('\\')[-1].split(';')[0].upper()
            tot += sizes.get(base, 0)
        if tot:
            out[mod] = tot
    return out


def main():
    fn_mod, fn_calls, fn_up, declared = scan()
    sizes = module_bytes()

    # An orphan is a room-entry uploader no area runs. A PROP's uploader
    # (concrete_props_upload_textures, piano_props_upload_textures) is not one:
    # it is reached from the room uploaders that borrow it, so it shows up in
    # some area's closure. Test reachability, not the name.
    listed, reached = set(), set()
    for fns in AREAS.values():
        listed |= set(fns)
        stack, seen = list(fns), set()
        while stack:
            f = stack.pop()
            if f in seen or f not in fn_mod:
                continue
            seen.add(f)
            reached.add(f)
            stack += fn_calls[f]
    orphans = sorted(f for f in fn_mod
                     if f.endswith("_upload_textures") and f not in reached)

    needed = {}   # module -> mask of areas that reach it
    print("=" * 74)
    print(" TEXTURE BANKS   generated by tools/check_tex_banks.py")
    print("=" * 74)
    print()
    print("PER-AREA RESIDENT SET")
    peak = 0
    for area, fns in AREAS.items():
        mods = closure(fns, fn_mod, fn_calls, fn_up)
        total = sum(sizes.get(m, 0) for m in mods)
        peak = max(peak, total)
        print("  %-12s %7d bytes (%3d KB)  %2d modules"
              % (area, total, total // 1024, len(mods)))
        for m in mods:
            needed[m] = needed.get(m, 0) | BIT[area]
    every = sum(sizes.values())
    print()
    print("  peak bank        %7d bytes (%3d KB)  <- this is what sets free-at-rest"
          % (peak, peak // 1024))
    print("  all at once      %7d bytes (%3d KB)  <- what the pre-bank design held"
          % (every, every // 1024))
    print()

    print("DECLARED vs REQUIRED")
    bad = 0
    for mod in sorted(needed, key=lambda m: -sizes.get(m, 0)):
        req = needed[mod]
        dec = declared.get(mod)
        req_s = "|".join(NAME[b] for b in sorted(NAME) if req & b)
        if dec is None:
            print("  %-20s %7d  *** NO texmgr_set_bank() - needs %s ***"
                  % (mod, sizes.get(mod, 0), req_s))
            bad += 1
            continue
        missing = req & ~dec
        if missing:
            print("  %-20s %7d  *** MISSING %s ***"
                  % (mod, sizes.get(mod, 0),
                     "|".join(NAME[b] for b in sorted(NAME) if missing & b)))
            bad += 1
        else:
            extra = dec & ~req
            note = ""
            if extra:
                note = "   (wider than needed: +%s)" % "|".join(
                    NAME[b] for b in sorted(NAME) if extra & b)
            print("  %-20s %7d  %s%s" % (mod, sizes.get(mod, 0), req_s, note))

    # A module that declares a bank but is reached by nobody: dead weight, or a
    # missing AREAS entry. Either way, say so.
    for mod in sorted(declared):
        if mod not in needed and sizes.get(mod):
            print("  %-20s %7d  *** declared, but NO area's uploaders reach it ***"
                  % (mod, sizes[mod]))
            bad += 1

    if orphans:
        print()
        print("ORPHANED UPLOADERS  (not listed in any area above)")
        for f in orphans:
            print("  %s  in %s" % (f, fn_mod[f]))
        bad += len(orphans)

    print()
    if bad:
        print("*** %d problem(s). A missing bank is a SILENT wrong texture at" % bad)
        print("    runtime - see the head of this file. Fix the mask in the")
        print("    module's *_load_assets(), or add the room to AREAS here and")
        print("    to area_bank_of() in src/area_bank.c.")
        return 1
    print("OK: every module's declared bank mask covers every area that reaches it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
