#!/usr/bin/env python3
"""
vram_map_underworld.py - what VRAM is really free while the player is in Chapter 4.

    py tools/vram_map_underworld.py > tools/VRAM_MAP_UNDERWORLD.txt

WHY A FIFTH MAP EXISTS
----------------------
tools/VRAM_MAP.txt is the map of the whole disc and says almost nothing is free.
The Underworld is the fourth pocket after the West Garden, Asag's arena and the
Catacombs, and the most sealed of the chapters: it is reached through the Seal's
white and Descent to Hell, one way, and the only way out of it is the title. So
while the player is down here nothing of Chapters 1-3 is ever DRAWN, and every
room-art page whose tenants are re-uploaded by their own rooms on entry is free.

THE CHAPTER RE-LAYS VRAM ROOM BY ROOM, ON PURPOSE. Every Underworld room uploads
its own set on entry (STATE_LOADING), so two Underworld rooms may put different
art on the same page as long as each uploads what it draws. That is the licence
this chapter runs on, and it is cheap: a few LoadImages behind the transition.

WHAT COUNTS AS RESERVED HERE
----------------------------
Exactly the Catacombs' test, and the same list: the HUD, the weapons, healing,
the drop shadow, the bullet-impact sprite and every collectible's menu icon,
including the three only Chapter 3 can put in the inventory - a save carried
down here may hold any of them.

  FREE        every occupant is re-uploaded on entry by the room that needs it.
  NEEDS A WAY BACK
              the occupant is startup-resident and no room's entry path puts it
              back. Taking one obliges you to write the restore first.

>>> ONE BORROW HERE RELIES ON A STARTUP UPLOAD STAYING PUT. <<< Hell Hath No
Fury's two wooden doors draw wd_dr at x768 y256, which src/fatdoor.c uploads
ONCE at boot and nothing streams. It is listed in THIS BANK'S ART below as
borrowed-resident: a future Underworld room that takes x768 y256 must give
Hell Hath No Fury a way to put wd_dr back first.

THE SKY owns no texture: src/underworld_sky.c draws the Descent's hell bg. Every
room's mesh art needs a page START (x a multiple of 64, Voff 0) because rooms,
and the sky, draw under the 128 window.

MAINTENANCE: BANK is hand-kept, like the other maps'. Keep it in step with the
TIM_SLOT lines of every Underworld room.
"""
import os
import sys
import glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import contextlib
import io
from vram_map import read_tim, EXPORTER_ALIASES
# The arena's map prints itself as it is imported; only its tables are wanted.
with contextlib.redirect_stdout(io.StringIO()):
    import vram_map_asag as ASAG

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
TEXDIR = os.path.join(ROOT, "textures")

# The Catacombs' list: the arena's, plus the three Chapter 3 items.
RESERVED = dict(ASAG.RESERVED)
RESERVED["bld_prl.tim"] = "the Blood Pearl item (the Cleaver Corridor's sconce)"
RESERVED["mt_sck.tim"]  = "the Meat Sack item (the Meat Plant's courtyard)"
RESERVED["gl_ky.tim"]   = "the Gaol Key item (the Incinerator's east tray)"

# The chapter's art: what its rooms OWN, and what they BORROW from Chapter 3 or
# from the startup set. A borrowed page is in use while that room is entered.
BANK = {
    "lava.tim":                "Descent: the lava (owned; HHNF too) x320 y256",
    "hell bg.tim":             "Descent: the backdrop (owned; the sky) x640 y0",
    "volcanic.tim":            "HHNF: the platform (owned)         x384 y256",
    "cobblestones.tim":        "HHNF: the ruin (Catacombs Entry's) x384 y0",
    "catacomb inner door.tim": "HHNF: two doors (Catacombs Entry's) x832 y0",
    "seal.tim":                "HHNF: the floor seal (the Seal's)  x576 y0",
    "wd_dr.tim":               "HHNF: two doors - BORROWED RESIDENT, see head x768 y256",
    "oil_container.tim":       "HHNF: the ruin's oil tank (the prop's) x896 y256",
}

tims = {}
for p in sorted(glob.glob(os.path.join(TEXDIR, "**", "*.tim"), recursive=True)):
    if os.path.basename(p) in EXPORTER_ALIASES:
        continue
    r = read_tim(p)
    if r:
        tims[os.path.basename(p)] = r


def occupants(px, pw, py, ph):
    return [n for n, r in tims.items()
            if not (r["y"] >= py + ph or r["y"] + r["h"] <= py
                    or r["x"] >= px + pw or r["x"] + r["cols"] <= px)]


def verdict(occ, limit=None):
    res  = sorted(n for n in occ if n in RESERVED)
    bank = sorted(n for n in occ if n in BANK)
    rest = sorted(n for n in occ if n not in RESERVED and n not in BANK
                  and n in ASAG.NEEDS_RESTORE)
    other = sorted(n for n in occ if n not in RESERVED and n not in BANK
                   and n not in ASAG.NEEDS_RESTORE)
    if bank:
        v = "IN USE by this bank: " + ", ".join(bank)
    elif res:
        v = "BLOCKED - reserved: " + ", ".join(res[:limit])
    elif rest:
        v = "AVAILABLE, needs a way back first: " + ", ".join(rest[:limit])
    else:
        v = "FREE - costs nothing"
    return v, other


print("=" * 78)
print(" UNDERWORLD VRAM BANK   generated by tools/vram_map_underworld.py")
print("=" * 78)
print(__doc__.strip().split("\n\n", 1)[1])
print()

print("FIXED REGIONS  (identical in every bank)")
print("  Framebuffers : x[0,320)   y[0,480)")
print("  Font (FntLoad): x[960,1024) y[0,256)")
print("  CLUT band    : x[0,256)   y[480,512)")
print()

print("RESERVED  (a global; overwriting it breaks something)")
print("  %-22s %s" % ("texture", "what it is"))
print("  " + "-" * 74)
for n in sorted(RESERVED):
    print("  %-22s %s" % (n, RESERVED[n]))
print()

print("THIS BANK'S ART")
print("  %-26s %s" % ("texture", "what it is"))
print("  " + "-" * 74)
for n in sorted(BANK):
    print("  %-26s %s" % (n, BANK[n]))
print()

print("THE SPRITE BAND  y[128,256)   (Voff 128: sprites only. No Chapter 4 enemy")
print("exists yet, and no enemy sheet of Chapters 1-3 is uploaded down here.)")
print()
for px in range(320, 960, 128):
    v, other = verdict(occupants(px, 128, 128, 128))
    if other and v.startswith("FREE"):
        v += " [over: %s]" % ", ".join(other)
    print("  x[%d,%d)  %s" % (px, px + 128, v))
print()

for py, label in ((0, "y[0,128)"), (256, "y[256,384)")):
    print("MESH-ART PAGES  %s   (Voff 0: a room's texture window is safe here)" % label)
    for px in range(320, 960, 64):
        v, other = verdict(occupants(px, 64, py, 128), 3)
        if other:
            v += "   [also on this page, all re-uploaded on room entry: %s]" % ", ".join(other)
        print("  x%-4d %s" % (px, v))
    print()

print("Regenerate with:  py tools/vram_map_underworld.py > tools/VRAM_MAP_UNDERWORLD.txt")
print("And keep tools/VRAM_MAP.txt regenerated too - it is still the authority on")
print("collisions; this file is a view of it, not a replacement for it.")
