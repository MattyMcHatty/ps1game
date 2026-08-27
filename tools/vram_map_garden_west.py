#!/usr/bin/env python3
"""
vram_map_garden_west.py - the GARDEN-WEST VRAM BANK's own map.

    python tools/vram_map_garden_west.py > tools/VRAM_MAP_GARDEN_WEST.txt

WHY A SECOND MAP EXISTS
-----------------------
tools/VRAM_MAP.txt is the map of the whole disc: every TIM in textures/, laid
over one 1024x512 sheet, with the time-shared pairs called out. It is the right
picture for a game whose rooms all reach each other, and it is the picture the
game had until the Stables.

The Stables and the Greenhouse behind it are different. They are reached ONLY
through the Rear Gate's west gate, they lead nowhere else, and nothing but those
two rooms is ever drawn while the player is inside them. So the question "what is
in VRAM" has a second, much emptier answer while the player is in that pair, and
this file is that answer: the fixed regions, the globals that have to survive
everywhere, these two rooms' own art, and - the useful part - everything else,
which is free for them to use.

WHAT COUNTS AS A GLOBAL HERE
----------------------------
Not "resident in VRAM" - almost everything is that. A texture is GLOBAL for this
bank if overwriting it would break something the player can still see while in
these two rooms, or would break another room that never puts it back:

  RESERVED   the HUD, the weapons and collectibles and their menu icons, the
             door-transition panels (including the GATE halves, which is the
             panel this room's only door uses), the shared sprites the two
             rooms are allowed to spawn (RAFFLESIA and MUSHROOM HEAD, both
             asked for by name), and the handful of textures that are uploaded
             once at startup and never re-uploaded by anybody.

  RECLAIMABLE  everything else: mansion and garden ROOM ART, and the enemy
             sprites for enemies that cannot appear in these two rooms. Every
             one of these is either re-uploaded on entry by the room that needs
             it (in which case taking it here costs nothing at all) or is
             startup-resident (in which case taking it costs whoever needs it a
             restore, and the "restored by" column says which). The Stables took
             four of the free ones and owed nobody anything.

The classification is BY HAND, in RESERVED below, because it is a statement
about what the code does, not something that can be read out of a .tim. Keep it
honest: a texture wrongly called reclaimable comes back as a room drawing another
room's art, silently.
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vram_map import read_tim, TEXDIR       # same parser, same textures/ dir
import glob

# ---------------------------------------------------------------------------
# RESERVED: must survive the bank. See the docstring for the test.
# ---------------------------------------------------------------------------
RESERVED = {
    # --- HUD, weapons, and the player's own overlays -----------------------
    "hud.tim":            "the HUD panel",
    "crucifaxe.tim":      "the weapon, world sprite and menu icon",
    "ghit.tim":           "bullet-impact sprite",
    "shadow.tim":         "entity drop shadows",
    "mist.tim":           "the poison wash",
    # --- Collectibles and their menu icons ---------------------------------
    "key.tim":            "door key (menu icon in every room)",
    "cppr_pt.tim":        "copper pot; time-shares the spent key slot",
    "sml_med.tim":        "small medkit",
    "graveolver.tim":     "the Grave-olver",
    "stnd_rnds.tim":      "standard rounds",
    "flm_rnds.tim":       "flame rounds",
    "wx_cb.tim":          "wax cube",
    "grn_ky_stn.tim":     "green key stone",
    "ylw_ky_stn.tim":     "yellow key stone",
    "bl_ky_stn.tim":      "blue key stone",
    "mgn_ky_stn.tim":     "magenta key stone",
    "pno_key.tim":        "piano key item",
    "helluminator.tim":   "the Helluminator (menu icon in every room)",
    "vlv_hndl.tim":       "the Valve Handle item (menu icon in every room)",
    # --- The door-transition panels ----------------------------------------
    "grdngtl.tim":        "GATE panel, left half - THIS bank's own transition",
    "grdngtr.tim":        "GATE panel, right half - THIS bank's own transition",
    "dbl_dr_hlf.tim":     "double-door panel half",
    "inr_dbl_dr_half.tim":"inner double-door panel half",
    "xt_dr_lft_hlf.tim":  "outer-door panel, left half",
    "xt_dr_rt_hlf.tim":   "outer-door panel, right half",
    # --- The two enemies this bank is allowed to spawn ----------------------
    "rafflesia1.tim":     "Rafflesia, resting  (streamed in on entry here)",
    "rafflesia2.tim":     "Rafflesia, waking   (streamed in on entry here)",
    "mushy_closed.tim":   "Mushroom Head, closed",
    "mushy_open.tim":     "Mushroom Head, open",
    "mushy_run.tim":      "Mushroom Head, running",
    "mushy_behind.tim":   "Mushroom Head, from behind",
}

# ---------------------------------------------------------------------------
# The bank's own room art. Slot notes match src/stables.c's table.
# ---------------------------------------------------------------------------
BANK = {
    "hedge.tim":           "Stables: east perimeter + gate alcove   (borrowed)",
    "grdn_gte.tim":        "Stables: the gate leaf                  (borrowed)",
    "grss_gs.tim":         "Stables: the yard                       (borrowed)",
    "brick_wall.tim":      "Stables: north/south/west walls         (borrowed)",
    "greenhouse.tim":      "Stables: the building beyond the west wall  OWNED",
    "stables wood.tim":    "Stables: the two stable blocks              OWNED",
    "stable glyphs.tim":   "Stables: markings on the blocks' faces      OWNED",
    "greenhouse door.tim": "Stables: the shut door in the west wall     OWNED",
    # The Greenhouse, the bank's second and last room. Five of its ten mesh
    # textures are the Stables' set above (grss_gs, brick_wall, greenhouse,
    # stables wood, greenhouse door); these five are its own, and every one of
    # them lands on a page the STABLES has just stamped and this room draws
    # nothing from. Note the two CLONES: poison_flower_base and pipe both exist
    # already, but at x640 y0 and x768 y0 - which is where `greenhouse` and
    # `brick_wall` live, and this room draws those. See src/greenhouse.c.
    "flowerbed.tim":       "Greenhouse: the beds along the aisles       OWNED",
    "cuneiform _symbols.tim":  "Greenhouse: the marked pipework             OWNED",
    "poison_flower_base_gh.tim":
                           "Greenhouse: flower-bed soil, 4bpp clone     OWNED",
    "pipe_gh.tim":         "Greenhouse: the standing pipe, 4bpp clone   OWNED",
    "pipe_button_off.tim": "Greenhouse: the button on the pipework      OWNED",
    # The lit half of the same button. It sits immediately to pipe_button_off's
    # RIGHT on the same page, at x[520,528), so the draw loop reaches it from
    # the SAME tpage by adding 32 to the poly's u - see src/greenhouse.c.
    "pipe_button_on.tim":  "Greenhouse: the button, lit                 OWNED",
    # The VINES prop. Not mesh art - the only prop texture this bank owns -
    # and 8bpp, so unlike pipe_gh and pipe_button_off it could not squeeze
    # onto a 4bpp left-half and needed a whole page. It took the Rabisu's,
    # which is why "Rabisu tex.tim" is no longer in NEEDS_RESTORE below.
    "vines.tim":           "Greenhouse: the vines prop                  OWNED",
}

# Textures that are startup-resident and are NOT re-uploaded by any room. Taking
# one of these costs a restore; everything else reclaimable is free.
NEEDS_RESTORE = {
    "wd_flr.tim", "red_wlppr.tim", "din_cl.tim", "stn_gls.tim",   # kitchen set
    "wd_dr.tim",                                                   # fatdoor
    # Loaded once at startup by whoever owns them and never re-uploaded by any
    # room's entry path. wd_dr_crk was missing from this list until the
    # Greenhouse took its page and the omission turned into a real bug (see
    # KITCHEN_SHARED_TEX in src/kitchen_dining.c, which now restores it);
    # inr_dbl_dr is in kitchen_stream_textures, which is startup-only - the
    # mid-game path is kitchen_restore_textures, and it is not in that list.
    "wd_dr_crk.tim", "inr_dbl_dr.tim",
    "trees_dl.tim", "grinder.tim", "wdcrate.tim", "mansion.tim",
    "anzu1.tim", "anzu2.tim", "anzu3.tim", "anzu4.tim", "anzu5.tim", "anzu6.tim",
    "tntcl_idle.tim", "tntcl_actv.tim",
    "zombie_sleep.tim", "zombie_alert.tim",
    "spdr_rst.tim", "spdr_wk.tim",
    "ddog_sleep.tim", "ddog_alert.tim", "ddog_alert2.tim",
    "ls_idle.tim", "ls_atk.tim",
    "hadad_idle_64.tim", "hadad_stp1_64.tim", "hadad_stp2_64.tim",
    # NOT "Rabisu tex.tim" any more. It WAS startup-upload-once, and the
    # Greenhouse's vines took its page (x704 y256) - so
    # garden_courtyard_upload_textures() now re-uploads it on entry to the room
    # the boss actually fights in. That makes it reclaimable like any other
    # room-restored texture, and it is the reason this set shrank rather than
    # grew when a page was taken.
}

tims = {}
for p in sorted(glob.glob(os.path.join(TEXDIR, "*.tim"))):
    r = read_tim(p)
    if r:
        tims[os.path.basename(p)] = r

print("=" * 78)
print(" GARDEN-WEST VRAM BANK  (Stables + Greenhouse)   generated by")
print(" tools/vram_map_garden_west.py")
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
for n, note in sorted(RESERVED.items(), key=lambda kv: (tims[kv[0]]["y"], tims[kv[0]]["x"])
                      if kv[0] in tims else (0, 0)):
    if n in tims:
        row(n, tims[n], note)
    else:
        print("  !! %s is listed as reserved but no such .tim exists" % n)
print()

print("THIS BANK'S ROOM ART")
print(hdr)
print("  " + "-" * 74)
for n, note in sorted(BANK.items(), key=lambda kv: (tims[kv[0]]["y"], tims[kv[0]]["x"])
                      if kv[0] in tims else (0, 0)):
    if n in tims:
        row(n, tims[n], note)
    else:
        print("  !! %s is listed in the bank but no such .tim exists" % n)
print()

# ---------------------------------------------------------------------------
# What is left. A slot is USABLE for mesh art only if it is page-aligned
# (x % 64 == 0) at Voff 0, because every room in this game sets a 128 texture
# window; the width available depends on what else shares the page.
# ---------------------------------------------------------------------------
print("WHAT IS LEFT FOR THE GREENHOUSE")
print("""
  A usable mesh-art slot needs x a multiple of 64, y in {0,256} (so Voff = 0),
  and 128 free rows. The page is 128 texels wide, which is 64 VRAM columns at
  8bpp but only 32 at 4bpp - so a page whose right half holds something reserved
  can still take a 4bpp texture and cannot take an 8bpp one. That distinction is
  the one Maze Two got wrong (an 8bpp plinth on the rusty_fence page landed on
  the Anzu), so it is spelled out per page below.

  "free" here means: every occupant of that page is RECLAIMABLE, i.e. it is
  either re-uploaded on entry by the room that needs it (costs nothing) or is
  startup-resident (marked RESTORE: taking it obliges you to put it back for
  whoever draws it, the way delivery_restore_textures and kitchen_restore_textures
  already do for their own).
""")
pages = []
for py in (0, 256):
    for px in range(320, 1024, 64):
        # A Voff-0 mesh texture occupies rows [py, py+128) of the 256-tall page.
        # Anything at Voff 128 (y=128 or y=384) shares the TPAGE but not those
        # rows, so it does not block this slot - it only means the room drawing
        # it has to bracket the 128 texture window, which every enemy sprite
        # renderer already does.
        occ = []
        for n, r in tims.items():
            if r["y"] >= py + 128 or r["y"] + r["h"] <= py:
                continue
            if r["x"] >= px + 64 or r["x"] + r["cols"] <= px:
                continue
            occ.append(n)
        pages.append((px, py, occ))

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
        if all(tims[n]["bpp"] == 4 for n in bank) and not res_right and            not any(tims[n]["x"] + tims[n]["cols"] > px + 32 for n in occ):
            verdict += "  (4bpp: right half still free, 4bpp only)"
    elif res_left:
        verdict = "TAKEN - reserved: " + ", ".join(res_left[:3])
    elif res_right:
        verdict = "4bpp ONLY - right half reserved: " + ", ".join(res_right[:2])
    elif not occ:
        verdict = "FREE - 8bpp full page, EMPTY"
    else:
        verdict = "FREE - 8bpp full page"
        if need:
            verdict += "  [RESTORE: " + ", ".join(need[:4]) + "]"
        else:
            verdict += "  [free: every occupant is re-uploaded on room entry]"
    print("  x%-4d y%-3d  %s" % (px, py, verdict))
print()
print("Regenerate with:  python tools/vram_map_garden_west.py > tools/VRAM_MAP_GARDEN_WEST.txt")
print("And keep tools/VRAM_MAP.txt regenerated too - it is still the authority on")
print("collisions; this file is a view of it, not a replacement for it.")
