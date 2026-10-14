#!/usr/bin/env python3
"""
gen_tex_rooms.py - which ROOMS need each registered texture held in RAM.

Run by CMake on every build (the tex_rooms target); writes
build/tex_rooms_gen.c only when its contents change. Run it by hand to see the
per-room report:

    py tools/gen_tex_rooms.py --report

WHY
---
Until October 2026 texmgr held one AREA's textures at a time (src/texmgr.h,
src/area_bank.h): the whole of Chapter 3 - thirty-four rooms - was one 524 KB
bank, and the biggest bank is what sets free-at-rest. But no single room needs
more than about 300 KB of it. So each room now holds EXACTLY the textures its
own entry path uploads, and every door frees what the next room does not need
and reads only what it lacks (texmgr_room_select). Rooms that share a set (the
whole chapter shares cobble and the inner door) pay nothing for it at the door.

HOW THE SETS ARE FOUND
----------------------
The same call-graph walk tools/check_tex_banks.py does, per ROOM instead of per
area:

  1. main.c's STATE_LOADING room chain says which uploader(s) each room runs
     (`pending_area == STATE_X` ... `x_upload_textures()`).
  2. main.c's bank-keyed enemy lines below that chain (spiders, flowers,
     crawler, zombies, lumberer, maggots, Adapa) are mirrored in EXTRA_RULES
     here - they are conditions, not a table, so they are restated.
  3. The walk follows every *_upload*/*_restore* call to the modules whose own
     registrations get uploaded, and each module's registered TIM names are
     read out of its source.

THE SAFETY NET. If this table is ever missing something - EXTRA_RULES drifts
from main.c, or an uploader is called in a way the walk cannot see - the room
does NOT draw the wrong texture: texmgr_upload() reads a missing texture on the
spot while STATE_LOADING is running (a CD read is legal there), counts it and
prints "TEXMGR: late load <file> for room <n>" to the emulator log. A late load
is a bug in this file, not in the game; fix it here. Mid-room uploads (the
Attic Exit's door, the piano's repaired keys) have no such net, but both are in
modules their own room's entry already uploads, so the walk covers them.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import check_tex_banks as CTB  # noqa: E402

SRC = os.path.join(ROOT, "src")


def read(p):
    return open(p, encoding="utf-8", errors="replace").read()


def game_states():
    """STATE_ name -> enum value, from src/title.h."""
    s = CTB.strip_comments(read(os.path.join(SRC, "title.h")))
    body = s[s.index("typedef enum"):s.index("} GameState;")]
    names = re.findall(r'\b(STATE_\w+)\b', body)
    out, v = {}, 0
    for n in names:
        if n in out:
            continue
        out[n] = v
        v += 1
    return out


def area_of_rooms():
    """room -> area name, from area_bank_of() in src/area_bank.c."""
    a = CTB.strip_comments(read(os.path.join(SRC, "area_bank.c")))
    body = a[a.index("TexBank area_bank_of"):]
    body = body[:body.index("\n}\n")]
    out, pend = {}, []
    for t in re.finditer(r'case (STATE_\w+):|return TEXBANK_(\w+);', body):
        if t.group(1):
            pend.append(t.group(1))
        else:
            for r in pend:
                out[r] = t.group(2)
            pend = []
    return out


def room_uploaders(rooms):
    """room -> set of uploader functions main.c's STATE_LOADING chain runs."""
    m = CTB.strip_comments(read(os.path.join(SRC, "main.c")))
    i = m.index("reception_upload_textures();")
    seg = m[m.rfind("if (pending_area", 0, i):m.index("copper_pot_owned()", i)]
    up, cur = {}, []
    for t in re.finditer(r'(?:else\s+)?if\s*\(([^{]*?)\)\s*\{|(\w+_(?:upload|restore)\w*)\s*\(', seg):
        if t.group(1) is not None:
            cond = t.group(1)
            rs = re.findall(r'pending_area == (STATE_\w+)', cond)
            if 'area_is_dead_end(pending_area)' in cond:
                rs += [r for r in rooms if r.startswith("STATE_DEAD_END_")]
            if rs:
                cur = rs
        elif cur:
            for r in cur:
                up.setdefault(r, set()).add(t.group(2))
    return up


# main.c's enemy-sheet lines below the room chain, restated (see the head).
FLOWER_ROOMS = {"STATE_OUTSIDE_CATACOMBS", "STATE_MAZE_ONE", "STATE_MAZE_TWO",
                "STATE_STABLES", "STATE_GREENHOUSE"}


def extra_rules(room, area):
    e = []
    if area == "CATACOMBS" and room != "STATE_KENNELS":
        e.append("crawlers_upload_textures")
    elif room == "STATE_KENNELS":
        pass                                    # Gula's sheet, streamed
    elif room in FLOWER_ROOMS:
        e.append("rafflesias_upload_textures")
    elif room != "STATE_ASAG_ARENA":
        e.append("spiders_upload_textures")
    if area == "MANSION":
        e.append("zombies_upload_textures")
    if area == "CATACOMBS":
        e += ["lumberers_upload_textures", "maggots_upload_textures"]
    if room in ("STATE_PIANO_ROOM", "STATE_LIBRARY"):
        e.append("adapas_upload_textures")
    return e


def module_tims():
    """module -> [TIM basenames it registers], module_bytes()'s parse."""
    LIT = re.compile(r'"(\\\\(?:[A-Za-z0-9_]+\\\\)?[A-Za-z0-9_ ]+\.TIM)(?:;1)?"')
    out = {}
    for path in sorted(os.listdir(SRC)):
        if not path.endswith(".c"):
            continue
        src = CTB.strip_comments(read(os.path.join(SRC, path)))
        names = [m.group(1) for m in
                 re.finditer(r'texmgr_register\s*\(\s*"([^"]+)"', src)]
        if re.search(r'texmgr_register\s*\(\s*(?:new_tex|shared_tex|raf_tex)', src):
            for m in re.finditer(
                    r'(?:new_tex|shared_tex|raf_tex)[a-z_]*\[[^\]]*\]\s*=\s*\{(.*?)\};',
                    src, re.S):
                names += LIT.findall(m.group(1))
        base = [n.replace('\\\\', '\\').split('\\')[-1].split(';')[0].upper()
                for n in names]
        if base:
            out[path[:-2]] = base
    return out


def build():
    states = game_states()
    areas = area_of_rooms()
    ups = room_uploaders(areas)
    fn_mod, fn_calls, fn_up, _ = CTB.scan()
    tims = module_tims()
    sizes = CTB.module_bytes()

    room_mods = {}
    for room, area in areas.items():
        fns = sorted(ups.get(room, ())) + extra_rules(room, area)
        room_mods[room] = CTB.closure(fns, fn_mod, fn_calls, fn_up)

    tim_rooms = {}
    for room, mods in room_mods.items():
        for mod in mods:
            for t in tims.get(mod, ()):
                tim_rooms.setdefault(t, set()).add(room)
    return states, areas, ups, room_mods, tim_rooms, sizes


def emit(states, tim_rooms):
    width = (max(states.values()) + 32) // 32
    lines = [
        "/* GENERATED by tools/gen_tex_rooms.py on every build - do not edit.",
        "   For each registered TIM, the rooms (GameState bits) whose entry",
        "   uploads it, i.e. the rooms that must hold it in RAM. See",
        "   src/texmgr.h, TEXTURES BY ROOM. */",
        "#include <stdint.h>",
        "#include \"texmgr.h\"",
        "",
        "const int tex_rooms_words = %d;" % width,
        "const TexRoomRow tex_rooms[] = {",
    ]
    for t in sorted(tim_rooms):
        words = [0] * width
        for r in tim_rooms[t]:
            v = states[r]
            words[v // 32] |= 1 << (v % 32)
        lines.append('    { "%s", { %s } },' % (
            t, ", ".join("0x%08Xu" % w for w in words)))
    lines += ["};",
              "const int tex_rooms_count = %d;" % len(tim_rooms), ""]
    return "\n".join(lines)


def main():
    states, areas, ups, room_mods, tim_rooms, sizes = build()
    width = (max(states.values()) + 32) // 32
    if width > 4:
        sys.exit("GameState has outgrown TEX_ROOM_WORDS (4) in src/texmgr.h")

    if "--report" in sys.argv:
        rows = sorted(((sum(sizes.get(m, 0) for m in mods), r)
                       for r, mods in room_mods.items()), reverse=True)
        print("PER-ROOM RESIDENT TEXTURES (tools/gen_tex_rooms.py)")
        for tot, r in rows:
            print("  %4d KB  %-12s %s" % (tot // 1024, areas[r], r[6:]))
        print("  peak room: %d KB" % (rows[0][0] // 1024))
        return 0

    missing = sorted(r for r in areas if r not in ups and r != "STATE_ASAG_ARENA"
                     and r != "STATE_DELIVERY_AREA")
    if missing:
        print("gen_tex_rooms: no uploader found for %s - its textures will be"
              " late-loaded (see the head of this file)" % ", ".join(missing))

    out = os.path.join(sys.argv[1] if len(sys.argv) > 1 else
                       os.path.join(ROOT, "build"), "tex_rooms_gen.c")
    text = emit(states, tim_rooms)
    old = open(out, encoding="utf-8").read() if os.path.exists(out) else None
    if old != text:
        open(out, "w", encoding="utf-8", newline="\n").write(text)
        print("gen_tex_rooms: wrote %s (%d textures)" % (out, len(tim_rooms)))
    return 0


def peak_room_bytes():
    """For tools/heap_budget.py: (bytes, room) of the largest room set."""
    states, areas, ups, room_mods, tim_rooms, sizes = build()
    return max((sum(sizes.get(m, 0) for m in mods), r)
               for r, mods in room_mods.items())


if __name__ == "__main__":
    sys.exit(main())
