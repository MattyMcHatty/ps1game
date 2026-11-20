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
The Catacombs' test: the HUD, the weapons and their rounds, healing, the drop
shadow, the bullet-impact sprite - and the art of the collectibles the player
CAN BE HOLDING down here, which today is Ninurta's Arm alone.

>>> NO OTHER CHAPTER'S ITEM IS RESERVED HERE. <<< Nothing of Chapters 1/2
survives into the Catacombs, and Chapter 3's three (the Blood Pearl, the Meat
Sack, the Gaol Key) are spent on its own puzzles before the descent, so not one
of their icons can be drawn in the Underworld. They used to be listed here,
and that is how Ninurta's Arm came to be shrunk to 32x32 for want of a 64x64
slot (Nov 2026) - space this chapter always had. Those icons are uploaded ONCE,
at boot (menu_init, item_pickups_load_textures), so they sit in the NEEDS A
WAY BACK grade: take one and you owe it a re-upload for the rooms where it CAN
be held, or a title-screen load of an earlier save shows a wrong icon.

  FREE        every occupant is re-uploaded on entry by the room that needs it.
  NEEDS A WAY BACK
              the occupant is startup-resident and no room's entry path puts it
              back. Taking one obliges you to write the restore first.

>>> TWO BORROWS HERE RELY ON A STARTUP UPLOAD STAYING PUT. <<< Hell Hath No
Fury's two wooden doors (and Sharur's Trial's two) draw wd_dr at x768 y256,
which src/fatdoor.c uploads ONCE at boot and nothing streams - and it is ALSO
DOOR_PANEL_WOOD's panel, the transition out of Sharur's Trial. A Woman
Scorned's south-east double door draws inr_dbl_dr at x576 y256, which the
kitchen streams ONCE at boot (kitchen_stream_textures) and only the kitchen's
own entry re-stamps. Both are listed in THIS BANK'S ART below as
borrowed-resident: a future Underworld room that takes either page must give
the rooms that draw it (and, for wd_dr, the wooden transition) a way to put it
back first.

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

# The player's own kit (the arena's list, less its Chapter 1/2 collectibles -
# the Catacombs' map does the same), plus the Underworld's own items.
with contextlib.redirect_stdout(io.StringIO()):
    import vram_map_catacombs as CATA
CHAPTER_3_ITEMS = {"bld_prl.tim", "mt_sck.tim", "gl_ky.tim"}
RESERVED = {n: v for n, v in CATA.RESERVED.items() if n not in CHAPTER_3_ITEMS}
# ...and the Underworld's own: Ninurta's Arm, under Sharur's Tablet.
RESERVED["nnrt_arm.tim"] = "the Ninurta's Arm item (under Sharur's Tablet)"
RESERVED["nnrt_wngs.tim"] = "the Ninurta's Wings item (under the Wings Tablet)"
RESERVED["sharur.tim"] = "Sharur, the weapon's icon (Chamber of Destiny's lower plinth)"
RESERVED["tblt_dst.tim"] = "the Tablet of Destinies item (Chamber's higher plinth)"
# Every earlier chapter's item: startup-resident, never drawn down here.
NEEDS_RESTORE = set(CATA.NEEDS_RESTORE) | CHAPTER_3_ITEMS

# The chapter's art: what its rooms OWN, and what they BORROW from Chapter 3 or
# from the startup set. A borrowed page is in use while that room is entered.
BANK = {
    "lava.tim":                "Descent: the lava (owned; HHNF too) x320 y256",
    "hell bg.tim":             "Descent: the backdrop (owned; the sky) x640 y0",
    "volcanic.tim":            "HHNF: the platform (owned)         x384 y256",
    "cobblestones.tim":        "HHNF: the ruin (Catacombs Entry's) x384 y0",
    "catacomb inner door.tim": "HHNF: two doors (Catacombs Entry's) x832 y0",
    "seal.tim":                "HHNF: the floor seal (the Seal's)  x576 y0",
    "wd_dr.tim":               "HHNF, Sharur, Ribcage: doors - BORROWED RESIDENT, see head x768 y256",
    "double_door_uw.tim":      "Ribcage: E double door (owned copy of double_door; seal's page) x576 y0",
    "inr_dbl_dr.tim":          "A Woman Scorned: SE double door - BORROWED RESIDENT (the kitchen's boot stream) x576 y256",
    "rocks.tim":               "Sharur: the outcrops (owned)       x512 y0",
    "oil_container.tim":       "HHNF: the ruin's oil tank (the prop's) x896 y256",
    "nessie_a.tim":            "Sharur: Nessie frames 0-2 (Adapa's) x832 y128",
    "nessie_b.tim":            "Sharur: Nessie frames 3-5 (Adapa's) x896 y128",
    # The AHRIMAN (src/ahriman.h), on the crawler's two blocks. Their owners
    # (crawler, Gula, spiders, flowers, zombies) are put back by main.c on
    # entry to any room that can hold them, so the way back already exists.
    "ahriman_a.tim":           "Lava U: Ahriman frames 0-1 (crawler's) x320 y128",
    "ahriman_b.tim":           "Lava U: Ahriman frames 2-3 (crawler's) x704 y128",
    # The GOLEM (src/golem.h), on the SECOND halves of the same two blocks -
    # the Ahriman only fills the first 64 words of each. Same owners, same
    # way back (spiders, flowers, Gula, zombies).
    "golem_a.tim":             "Wingscape: Golem frames 1-3 (crawler's) x384 y128",
    "golem_b.tim":             "Wingscape: Golem frames 4-6 (crawler's) x768 y128",
    # SHARUR'S TABLET (src/sharur_tablet.h): the plain art and the cracked
    # art the front faces switch to on the fourth blow. Two FREE page starts.
    "sharur tablet.tim":         "Sharur: the tablet (owned)         x704 y0",
    "sharur tablet cracked.tim": "Sharur: the tablet, cracked (owned) x768 y0",
    # THE WINGS TABLET (src/wings_tablet.h): Wingscape's terrace slab, on
    # Sharur's Tablet's page and palette - neither room draws the other's.
    "wing tablet.tim":           "Wingscape: the tablet (owned; Sharur's page) x704 y0",
    # ...and the puzzle's cuneiform, on the CRACKED art's page and palette.
    "wings tablet question and answers.tim": "Wingscape: the puzzle's cuneiform (owned; Sharur's cracked page) x768 y0",
    # HERO'S PLINTH (src/heros_plinth.h): Destiny's Door's frame and the door,
    # 8bpp, each on a Voff-0 page whose tenant it never draws.
    "destiny door frame top left.tim":      "Plinth: frame, top left (owned 8bpp; cobble's page) x384 y0",
    "destinys door frame top right.tim":    "Plinth: frame, top right (owned 8bpp; rocks' page) x512 y0",
    "destinys door frame bottom left.tim":  "Plinth: frame, bottom left (owned 8bpp; the oil tank's page) x896 y256",
    "destinys door frame bottom right.tim": "Plinth: frame, bottom right (owned 8bpp; the tablets' page) x704 y0",
    "destinys door.tim":                    "Plinth: Destiny's Door (owned 8bpp; cracked tablet's page; the Chamber too) x768 y0",
    # THE CHAMBER OF DESTINY (src/chamber_of_destiny.h): the tablet on the
    # frame's pages and CLUTs, the marble on the lava's, the night sky (also
    # its dome's) on the volcanic's.
    "ninurta tablet top left.tim":      "Chamber: tablet, top left (owned 8bpp; the frame's page) x384 y0",
    "ninurta tablet top right.tim":     "Chamber: tablet, top right (owned 8bpp; the frame's page) x512 y0",
    "ninurta tablet bottom left.tim":   "Chamber: tablet, bottom left (owned 8bpp; the frame's page) x896 y256",
    "ninurta tablet bottom right.tim":  "Chamber: tablet, bottom right (owned 8bpp; the frame's page) x704 y0",
    "marble.tim":                       "Chamber: walls, corridor floor (owned 8bpp; lava's page) x320 y256",
    "night sky.tim":                    "Chamber: floor, dais, the dome (owned 4bpp as supplied; volcanic's page) x384 y256",
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
                  and n in NEEDS_RESTORE)
    other = sorted(n for n in occ if n not in RESERVED and n not in BANK
                   and n not in NEEDS_RESTORE)
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

print("THE SPRITE BAND  y[128,256)   (Voff 128: sprites only. Nessie takes x[832,960)")
print("- Adapa's block - uploaded by main.c after the door-panel restore; the Ahriman")
print("takes x[320,384) and x[704,768) - the crawler's blocks, restored on the way out")
print("by the same STATE_LOADING lines that restore them after the Catacombs - and the")
print("Golem their second halves, x[384,448) and x[768,832), on the same terms. No")
print("enemy sheet of Chapters 1-3 is uploaded down here.)")
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
