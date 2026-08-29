#!/usr/bin/env python3
"""
vram_map_asag.py - ASAG'S ARENA's own VRAM map.

    py tools/vram_map_asag.py > tools/VRAM_MAP_ASAG.txt

WHY A THIRD MAP EXISTS
----------------------
tools/VRAM_MAP.txt is the map of the whole disc: every TIM in textures/, laid
over one 1024x512 sheet. It is the right picture for a game whose rooms all
reach each other, and read on its own it says there is almost nothing free.

tools/VRAM_MAP_GARDEN_WEST.txt was the first answer to that: the Stables and the
Greenhouse are a two-room pocket off the Rear Gate, so while the player is in
them the question "what is in VRAM" has a second, much emptier answer.

ASAG'S ARENA IS THE SAME ARGUMENT, PUSHED AS FAR AS IT GOES. It is ONE room,
reached ONLY by the one-way drop down The Hatch's shaft, leading nowhere the
player can walk back to. Nothing else in the game is drawn while the player is
down there, no neighbour's art has to survive alongside its own, and NOTHING HAS
TO BE PUT BACK ON THE WAY OUT - the exit is a transition, and the destination
room's own uploader runs on the far side of it.

So the reserved list here is shorter than any other bank's in the game. It is
essentially "what the PLAYER brought with them", and that is exactly the brief
this room was budgeted to.

WHAT COUNTS AS RESERVED HERE
----------------------------
Not "resident in VRAM" - almost everything is that. A texture is RESERVED for
this bank if overwriting it would break something the player can still SEE while
they are in the arena, or during the transition out of it:

  THE HUD AND THE PLAYER'S KIT   the panel, the bullet-impact sprite, entity
                                 drop shadows.
  THE WEAPONS                    the Crucifaxe, the Grave-olver, both ammo
                                 types, the Helluminator - world sprites AND
                                 menu icons, since the inventory menu opens in
                                 this room like any other.
  THE COLLECTIBLE MENU ICONS     everything the player may be carrying. The menu
                                 draws the icon for anything in the inventory,
                                 so an icon whose page was taken renders as a
                                 square of arena wall.
  THE EXIT'S DOOR PANEL          >>> AND ONLY THE ONE THE EXIT ACTUALLY USES.
                                 <<< door_anim draws the panel AFTER the room's
                                 update and BEFORE STATE_LOADING, so the arena's
                                 own textures are still in VRAM while it plays.
                                 The arena's exit is DOOR_PANEL_GATE, so grdngtl
                                 and grdngtr are reserved and the other panels
                                 are not. Change the exit's panel in main.c and
                                 this list moves with it.

  EVERYTHING ELSE IS RECLAIMABLE, and unlike in the garden-west bank almost all
  of it is FREE rather than merely available: every mansion and garden room
  re-uploads its own art on entry, and the arena is only ever left through a
  transition that runs one of those uploaders. The handful marked RESTORE are
  startup-resident textures that NO room's entry path puts back - taking one of
  those obliges you to restore it, the way delivery_restore_textures() and
  kitchen_restore_textures() already do for their own.

  >>> THE ENEMY SPRITES ARE ALL RECLAIMABLE, INCLUDING THE SHARED 128-ROW PAIR.
  <<< A sealed one-way arena holds the boss and nothing else, so no zombie,
  dog, spider, tentacle, Rafflesia, Mushroom Head, Living Statue, Hadad or
  Rabisu can be in it. main.c's STATE_LOADING branch skips the spider/rafflesia
  slot upload for this room specifically, which is what makes x320/x384 y128 -
  a genuine 128-row pair, the largest contiguous hole in this VRAM - honestly
  free rather than merely intended to be.

The classification is BY HAND, in RESERVED below, because it is a statement
about what the code does, not something that can be read out of a .tim. Keep it
honest: a texture wrongly called reclaimable comes back as a room drawing
another room's art, silently.
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vram_map import read_tim, TEXDIR       # same parser, same textures/ dir
import glob

# ---------------------------------------------------------------------------
# RESERVED: must survive the bank. See the docstring for the test.
# ---------------------------------------------------------------------------
RESERVED = {
    # --- The HUD and the player's own overlays -----------------------------
    "hud.tim":            "the HUD panel",
    "ghit.tim":           "bullet-impact sprite",
    "shadow.tim":         "entity drop shadows",
    # --- The weapons, world sprites AND menu icons -------------------------
    "crucifaxe.tim":      "the Crucifaxe - the parry, if the axe cannot cut",
    "graveolver.tim":     "the Grave-olver",
    "stnd_rnds.tim":      "standard rounds",
    "flm_rnds.tim":       "flame rounds",
    "helluminator.tim":   "the Helluminator (menu icon in every room)",
    # --- Healing, which the player may use mid-fight -----------------------
    "sml_med.tim":        "small medkit",
    # --- Collectible menu icons: the inventory opens in this room like any
    #     other, and it draws the icon for anything the player is carrying.
    "key.tim":            "door key",
    "cppr_pt.tim":        "copper pot; time-shares the spent key slot",
    "wx_cb.tim":          "wax cube",
    "grn_ky_stn.tim":     "green key stone",
    "ylw_ky_stn.tim":     "yellow key stone",
    "bl_ky_stn.tim":      "blue key stone",
    "mgn_ky_stn.tim":     "magenta key stone",
    "pno_key.tim":        "piano key item",
    "vlv_hndl.tim":       "the Valve Handle item",
    "hatch_key.tim":      "hatch key (both break in the lock, but a save made"
                          " carrying one arrives here with it)",
    # --- The ONE door panel this room's transitions use --------------------
    "grdngtl.tim":        "GATE panel, left half  - the arrival AND the exit",
    "grdngtr.tim":        "GATE panel, right half - the arrival AND the exit",
}

# ---------------------------------------------------------------------------
# The arena's own art. EMPTY - the room has none yet. Add a row here at the same
# time as the row in stream_tex_file[] in src/asag_arena.c, so this map and the
# code cannot drift.
# ---------------------------------------------------------------------------
BANK = {
    # e.g. "asag_floor.tim":  "Arena: the floor          OWNED",
}

# Textures that are startup-resident and are NOT re-uploaded by any room. Taking
# one of these costs a restore; everything else reclaimable is free.
# Copied from vram_map_garden_west.py - the same set, for the same reason - with
# the Rabisu's skin still absent from it (garden_courtyard_upload_textures()
# re-uploads that on entry, ever since the Greenhouse's vines took its page).
NEEDS_RESTORE = {
    "wd_flr.tim", "red_wlppr.tim", "din_cl.tim", "stn_gls.tim",   # kitchen set
    "wd_dr.tim",                                                   # fatdoor
    "wd_dr_crk.tim", "inr_dbl_dr.tim",
    "trees_dl.tim", "grinder.tim", "wdcrate.tim", "mansion.tim",
    "anzu1.tim", "anzu2.tim", "anzu3.tim", "anzu4.tim", "anzu5.tim", "anzu6.tim",
    "tntcl_idle.tim", "tntcl_actv.tim",
    "zombie_sleep.tim", "zombie_alert.tim",
    "spdr_rst.tim", "spdr_wk.tim",
    "ddog_sleep.tim", "ddog_alert.tim", "ddog_alert2.tim",
    "ls_idle.tim", "ls_atk.tim",
    "hadad_idle_64.tim", "hadad_stp1_64.tim", "hadad_stp2_64.tim",
}

tims = {}
for p in sorted(glob.glob(os.path.join(TEXDIR, "*.tim"))):
    r = read_tim(p)
    if r:
        tims[os.path.basename(p)] = r

print("=" * 78)
print(" ASAG'S ARENA VRAM BANK   generated by tools/vram_map_asag.py")
print("=" * 78)
print(__doc__.strip().split("\n\n", 1)[1])
print()
print("FIXED REGIONS  (identical in every bank)")
print("  Framebuffers : x[0,320)   y[0,480)")
print("  Font (FntLoad): x[960,1024) y[0,256)")
print("  CLUT band    : x[0,256)   y[480,512)")
print()

def row(name, r, note):
    voff = r["y"] % 256
    clut = "%d,%d" % (r["clut"][0], r["clut"][1]) if r["clut"] else "-"
    print("  %-20s %-3d %5d %5d %5d %5d %5d  %-11s %s" %
          (name, r["bpp"], r["x"], r["y"], r["wpx"], r["h"], voff, clut, note))

hdr = "  %-20s %-3s %5s %5s %5s %5s %5s  %-11s %s" % (
    "texture", "bpp", "x", "y", "wpx", "h", "Voff", "clut(x,y)", "what it is")

print("RESERVED  (a global; overwriting it breaks something)")
print(hdr)
print("  " + "-" * 74)
for n, note in sorted(RESERVED.items(),
                      key=lambda kv: (tims[kv[0]]["y"], tims[kv[0]]["x"])
                      if kv[0] in tims else (0, 0)):
    if n in tims:
        row(n, tims[n], note)
    else:
        print("  !! %s is listed as reserved but no such .tim exists" % n)
print()

print("THIS BANK'S ROOM ART")
print(hdr)
print("  " + "-" * 74)
if not BANK:
    print("  (none yet - the arena owns no textures. Add them to")
    print("   stream_tex_file[] in src/asag_arena.c and to BANK in this script.)")
for n, note in sorted(BANK.items(),
                      key=lambda kv: (tims[kv[0]]["y"], tims[kv[0]]["x"])
                      if kv[0] in tims else (0, 0)):
    if n in tims:
        row(n, tims[n], note)
    else:
        print("  !! %s is listed in the bank but no such .tim exists" % n)
print()

# ---------------------------------------------------------------------------
# What is left. Identical page arithmetic to vram_map_garden_west.py - a usable
# mesh-art slot is page-aligned at Voff 0, because every room in this game sets a
# 128 texture window.
# ---------------------------------------------------------------------------
print("WHAT IS LEFT FOR THE ARENA")
print("""
  A usable mesh-art slot needs x a multiple of 64, y in {0,256} (so Voff = 0),
  and 128 free rows. The page is 128 texels wide, which is 64 VRAM columns at
  8bpp but only 32 at 4bpp - so a page whose right half holds something reserved
  can still take a 4bpp texture and cannot take an 8bpp one. That distinction is
  the one Maze Two got wrong (an 8bpp plinth on the rusty_fence page landed on
  the Anzu), so it is spelled out per page below.

  "free" here means: every occupant of that page is RECLAIMABLE, i.e. it is
  re-uploaded on entry by the room that needs it and taking it costs NOTHING at
  all. A page marked RESTORE holds a startup-resident texture that no room's
  entry path puts back; taking one of those obliges you to restore it.

  >>> THE ARENA OWES NOBODY A RESTORE ON THE WAY OUT. <<< That is the difference
  between this bank and every other one, and it comes from the room's shape, not
  from restraint: the only exit is a transition, and every transition runs the
  destination room's own uploader.

  >>> THE TABLE BELOW ONLY WALKS THE Voff-0 BANDS (y=0 and y=256). <<< The two
  Voff-128 bands, y=128 and y=384, are not mesh-art slots - a 128 texture window
  wraps their V mod 128 and samples the wrong region - but they are real VRAM and
  the arena may use them for SPRITES, which bracket the window themselves.
  Everything in both bands is reclaimable here, and x320/x384 y128 in particular
  is the SHARED SPIDER/RAFFLESIA PAIR: 128 rows by 128 columns, the largest
  contiguous hole in this bank, and free because main.c skips that upload for
  this room specifically. A boss sprite sheet is what it is shaped for.
""")
pages = []
for py in (0, 256):
    for px in range(320, 1024, 64):
        occ = []
        for n, r in tims.items():
            if r["y"] >= py + 128 or r["y"] + r["h"] <= py:
                continue
            if r["x"] >= px + 64 or r["x"] + r["cols"] <= px:
                continue
            occ.append(n)
        pages.append((px, py, occ))

free_full = 0
for px, py, occ in pages:
    if px >= 960 and py == 0:
        print("  x%-4d y%-3d  FONT - never available" % (px, py)); continue
    bank = sorted(n for n in occ if n in BANK)
    res  = sorted(n for n in occ if n in RESERVED)
    recl = sorted(n for n in occ if n not in RESERVED and n not in BANK)
    res_left  = [n for n in res if tims[n]["x"] < px + 32]
    res_right = [n for n in res if tims[n]["x"] + tims[n]["cols"] > px + 32]
    need = [n for n in recl if n in NEEDS_RESTORE]

    if bank:
        verdict = "IN USE by this bank: " + ", ".join(bank)
    elif res_left:
        verdict = "TAKEN - reserved: " + ", ".join(res_left[:3])
    elif res_right:
        verdict = "4bpp ONLY - right half reserved: " + ", ".join(res_right[:2])
    elif not occ:
        verdict = "FREE - 8bpp full page, EMPTY"; free_full += 1
    else:
        verdict = "FREE - 8bpp full page"
        free_full += 1
        if need:
            verdict += "  [RESTORE: " + ", ".join(need[:4]) + "]"
        else:
            verdict += "  [free: every occupant is re-uploaded on room entry]"
    print("  x%-4d y%-3d  %s" % (px, py, verdict))
print()
print("  FULL 8bpp PAGES AVAILABLE TO THE ARENA: %d" % free_full)
print()
print("Regenerate with:  py tools/vram_map_asag.py > tools/VRAM_MAP_ASAG.txt")
print("And keep tools/VRAM_MAP.txt regenerated too - it is still the authority on")
print("collisions; this file is a view of it, not a replacement for it.")
