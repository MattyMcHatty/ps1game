#!/usr/bin/env python3
"""
vram_map_catacombs.py - what VRAM is really free while the player is in Chapter 3.

    py tools/vram_map_catacombs.py > tools/VRAM_MAP_CATACOMBS.txt

WHY A FOURTH MAP EXISTS
-----------------------
tools/VRAM_MAP.txt is the map of the whole disc: every TIM in textures/, laid
over one 1024x512 sheet. Read on its own it says there is almost nothing free,
and for a game whose rooms all reach each other that is the right picture.

It is the WRONG picture for a pocket. tools/VRAM_MAP_GARDEN_WEST.txt was the
first answer to that (the Stables and the Greenhouse, behind the Rear Gate) and
tools/VRAM_MAP_ASAG.txt pushed it as far as it goes (one sealed arena, eleven
free pages). THE CATACOMBS IS THE THIRD SUCH POCKET AND NOBODY HAD MEASURED IT.

It is a good one. Chapter 3 is FOUR rooms behind a one-way mouth and between
them they draw SEVEN textures:

    cobblestones, catacomb inner door, loculus, lamashtu tablet, sconce,
    oil_container, incinerator

...plus the enemy sheets, the shadow, the HUD and the player's kit. Everything
else in VRAM while the player is down there is art from a chapter they cannot
walk back to.

WHAT THIS MAP WAS WRITTEN FOR, AND WHAT IT FOUND
------------------------------------------------
The Lumberer (src/lumberer.c) needs two 128-word by 128-row blocks for its sheet
halves and there were none: it shipped sharing the CRAWLER's, which worked and
cost a rule nobody wanted - the two Chapter 3 enemies could never stand in the
same room. This sweep is what retired that rule. Two blocks were sitting in the
Voff-128 band the whole time:

    x[448,576)  mansion.tim (the New Game opening still) + dbl_dr_hlf + grdngtl
    x[832,960)  xt_dr_lft_hlf + xt_dr_rt_hlf

>>> AND NEITHER WAS "IN USE". BOTH WERE MERELY UNRESTORABLE. <<< All five of
those textures were startup-only LoadImages - read once in main()'s init block,
buffer freed, never written again - so overwriting one was permanent for the run
and nothing could borrow the page. That is a fact about the CODE and not about
VRAM, it is the SECOND time it has been the answer (the Crawler found the zombie
pair in exactly this state), and it is the first thing to check before accepting
that a slot is unavailable:

    door_anim_restore_panels()   re-reads the four leaves; main.c calls it on
                                 entry to any room outside TEXBANK_CATACOMBS
    intro_start()                re-reads the still on the frame New Game is
                                 confirmed, instead of main() doing it at boot

About forty lines between them, and they converted two untouchable blocks into
ordinary time-shares.

WHAT COUNTS AS RESERVED HERE
----------------------------
The same test the arena's map uses, because it is the same question: a texture
is RESERVED if overwriting it would break something the player can still SEE
while they are in Chapter 3, or during the transition out of it. That is the
HUD, the weapons, healing, the drop shadow, the bullet-impact sprite and every
collectible menu icon - the inventory opens down here like anywhere else, and an
icon whose page was taken renders as a square of cobblestone.

Everything else is reclaimable, but reclaimable comes in two grades and the
difference is the whole point of this file:

  FREE        every occupant is re-uploaded on entry by the room that needs it.
              Taking it costs nothing at all.
  NEEDS A WAY BACK
              the occupant is startup-resident and no room's entry path puts it
              back. Taking one obliges you to write the restore FIRST - see the
              two above for what that looks like.

THE CHAPTER IS A POCKET, NOT A SEALED ROOM. Unlike Asag's arena there are four
rooms in here and they transition between each other, so a page one of them
takes must be back before another one that draws it is entered. That is already
how the chapter works (the narrow uploaders in src/catacombs_entry.c), and it is
why the BANK below is chapter-wide rather than per-room.

MAINTENANCE: BANK and RESERVED are hand-kept, like the other two maps', because
both are statements about what the code does rather than something readable out
of a .tim. Keep them honest - a texture wrongly called reclaimable comes back as
a room drawing another room's art, silently.
"""
import os
import sys
import glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vram_map import read_tim, EXPORTER_ALIASES
import vram_map_asag as ASAG

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
TEXDIR = os.path.join(ROOT, "textures")

# ---------------------------------------------------------------------------
# RESERVED: a global the player can still see from inside Chapter 3. Shared
# verbatim with the arena's map rather than copied - the test is identical and
# two drifting copies of it would be worse than one.
# ---------------------------------------------------------------------------
RESERVED = dict(ASAG.RESERVED)

# ---------------------------------------------------------------------------
# THE CHAPTER'S OWN ART. Eight room textures across five rooms, plus the two
# enemies' sheets. Keep in step with the TIM_SLOT headers in the five *_tex_map
# rooms and with crawlers_load_textures() / lumberers_load_textures().
#
# arms.tim took the LAST page this map called "FREE - costs nothing", and the
# crib then took x576 y0 - the first of the "needs a way back first" pages to be
# spent, and it owed nothing for it, because both the calls that put its
# occupants back already existed (anzu_tex_stream, kitchen_stream_owned_textures).
# What is left on the mesh-art rows is x320 and x704 at y=0 and x768/x832 at
# y=256, and the one to write a restore against is whichever module still puts
# the displaced texture up on its own entry.
#
# >>> AND READ THE OTHER OCCUPANTS LINE BELOW BEFORE PICKING ONE. <<< This map
# used to print only the occupants it knew needed a way back, which made x704 y0
# look identical to x576 y0 when in fact its left half is six 4bpp garden
# textures. The crib went there first on the strength of that. An 8bpp 128
# texture takes all 64 columns of a page, so it lands on both halves; the
# "others" list is now printed for exactly that reason, and py tools/vram_map.py
# is still the authority that catches it.
# ---------------------------------------------------------------------------
BANK = {
    "cobblestones.tim":         "every room's floor and walls   x384 y0",
    "sconce.tim":               "the wall sconces               x448 y0",
    "loculus.tim":              "the burial niches              x512 y0",
    "lamashtu tablet.tim":      "the entry hall's tablet        x768 y0",
    "catacomb inner door.tim":  "the chapter's door panel too   x832 y0",
    "incinerator.tim":          "the Incinerator machine        x704 y256",
    "arms.tim":                 "the Room of Arms' arms field   x640 y0",
    "oil_container.tim":        "the oil dispenser              x896 y256",
    "crib.tim":                 "the Room of Arms' crib         x576 y0",
    "crawler_a.tim":            "Crawler frames 0,1             x320 y128",
    "crawler_b.tim":            "Crawler frames 2,3             x704 y128",
    "lumberer_a.tim":           "Lumberer images 1-3            x448 y128",
    "lumberer_b.tim":           "Lumberer images 4-6            x832 y128",
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


print("=" * 78)
print(" CATACOMBS VRAM BANK   generated by tools/vram_map_catacombs.py")
print("=" * 78)
print(__doc__.strip().split("\n\n", 1)[1])
print()

print("FIXED REGIONS  (identical in every bank)")
print("  Framebuffers : x[0,320)   y[0,480)")
print("  Font (FntLoad): x[960,1024) y[0,256)")
print("  CLUT band    : x[0,256)   y[480,512)")
print()

print("THIS BANK'S OWN ART")
print("  %-26s %s" % ("texture", "what it is"))
print("  " + "-" * 74)
for n in sorted(BANK):
    print("  %-26s %s" % (n, BANK[n]))
print()

# ---------------------------------------------------------------------------
# The Voff-128 sprite band, which is what this map was written to measure. A
# sheet half is 128 VRAM words wide (256 texels at 8bpp) by 128 rows, and its x
# must be a multiple of 64 so the tpage base lands on it exactly.
# ---------------------------------------------------------------------------
print("THE SPRITE BAND  y[128,256)   (Voff 128: sprites only - a room's 128-tall")
print("texture window wraps V here, so anything drawn from this band must bracket")
print("its own window. Blocks are 128 words x 128 rows: one enemy sheet half.)")
print()
for px in range(320, 960, 128):
    occ  = occupants(px, 128, 128, 128)
    res  = sorted(n for n in occ if n in RESERVED)
    bank = sorted(n for n in occ if n in BANK)
    rest = sorted(n for n in occ if n not in RESERVED and n not in BANK
                  and n in ASAG.NEEDS_RESTORE)
    free = sorted(n for n in occ if n not in RESERVED and n not in BANK
                  and n not in ASAG.NEEDS_RESTORE)
    if bank:
        v = "IN USE by this bank: " + ", ".join(bank)
    elif res:
        v = "BLOCKED - reserved: " + ", ".join(res)
    elif rest:
        v = "AVAILABLE, needs a way back first: " + ", ".join(rest)
    else:
        v = "FREE - costs nothing" + (" [over: %s]" % ", ".join(free) if free else "")
    print("  x[%d,%d)  %s" % (px, px + 128, v))
print()

# ---------------------------------------------------------------------------
# The Voff-0 bands, where mesh art goes.
# ---------------------------------------------------------------------------
for py, label in ((0, "y[0,128)"), (256, "y[256,384)")):
    print("MESH-ART PAGES  %s   (Voff 0: a room's texture window is safe here)" % label)
    for px in range(320, 960, 64):
        occ  = occupants(px, 64, py, 128)
        res  = sorted(n for n in occ if n in RESERVED)
        bank = sorted(n for n in occ if n in BANK)
        rest = sorted(n for n in occ if n not in RESERVED and n not in BANK
                      and n in ASAG.NEEDS_RESTORE)
        # EVERY remaining occupant, and it is printed rather than dropped: an
        # 8bpp 128 texture takes all 64 columns of a page, so a page whose OTHER
        # half is a pile of 4bpp art is not the same proposition as an empty one,
        # however restorable that art is. Leaving this list out is what sent the
        # crib to x704 y0 first.
        other = sorted(n for n in occ if n not in RESERVED and n not in BANK
                       and n not in ASAG.NEEDS_RESTORE)
        if bank:
            v = "IN USE by this bank: " + ", ".join(bank)
        elif res:
            v = "BLOCKED - reserved: " + ", ".join(res[:3])
        elif rest:
            v = "AVAILABLE, needs a way back first: " + ", ".join(rest[:3])
        else:
            v = "FREE - costs nothing"
        if other:
            v += "   [also on this page, all re-uploaded on room entry: %s]" % ", ".join(other)
        print("  x%-4d %s" % (px, v))
    print()

print("Regenerate with:  py tools/vram_map_catacombs.py > tools/VRAM_MAP_CATACOMBS.txt")
print("And keep tools/VRAM_MAP.txt regenerated too - it is still the authority on")
print("collisions; this file is a view of it, not a replacement for it.")
