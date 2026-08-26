#!/usr/bin/env python3
"""
vram_map.py - authoritative VRAM occupancy map for the PS1 horror game.

VRAM is 1024x512 16-bit words. It is the game's tightest resource (see
tools/TEXTURING_NOTES.txt). This script is the SINGLE SOURCE OF TRUTH for what
lives where: it reads the real rects out of every textures/*.tim, adds the fixed
regions (framebuffers, font, CLUT band), detects overlaps, and prints a sorted
map. Regenerate tools/VRAM_MAP.txt after adding/moving any texture:

    python tools/vram_map.py > tools/VRAM_MAP.txt

KEY COLUMN FOR RENDERING BUGS: "Voff" = the texture's y within its 256-tall
tpage (VRAM y % 256). A texture with Voff >= 128 drawn while a 128x128 texture
window is active gets its V wrapped mod-128 and samples the WRONG region (this
was the crucifaxe menu-icon bug). Such textures must be drawn with the window
reset/adjusted (see tools/TEXTURING_NOTES.txt, PART on texture windows).

OVERLAPS are NOT necessarily bugs: several slots are intentionally time-shared
via texture streaming (e.g. reception uploads strs/bnnstr/frnt_dr over the
kitchen's stn_stl/kchn_tile/red_crpt, and the dresser prop streams over kchn_wl
while in reception). Those pairs are listed under "time-shared".
"""
import struct, glob, os

TEXDIR = os.path.join(os.path.dirname(__file__), "..", "textures")

# Slots that are deliberately time-shared by streaming (physical, logical-a, logical-b).
# The map flags overlaps between these as expected rather than a collision.
KNOWN_STREAM_PAIRS = [
    ("stn_stl.tim",  "strs.tim"),      # reception streams strs over stn_stl
    ("red_crpt.tim", "frnt_dr.tim"),   # reception streams frnt_dr over red_crpt
    ("kchn_wl.tim",  "dresser.tim"),   # dresser prop streams over kchn_wl in reception
    ("stove.tim",    "prpl_wlppr.tim"),# piano room streams prpl_wlppr over stove
    ("stn_stl.tim",  "bookshelf.tim"), # piano room streams bookshelf over stn_stl...
    ("strs.tim",     "bookshelf.tim"), # ...which reception also time-shares (strs)
    ("kchn_tile.tim","piano_keys.tim"),# piano room streams piano_keys over kchn_tile
    ("kchn_tile.tim","cncrte.tim"),    # conservatory concrete props stream cncrte over kchn_tile
    ("piano_keys.tim","cncrte.tim"),   # ...same slot the piano room uses for piano_keys
    ("kchn_tile.tim","clsd_drwr.tim"), # 2F hall trick-drawers prop streams over kchn_tile...
    ("cncrte.tim",   "clsd_drwr.tim"), # ...the same slot the conservatory uses for cncrte
    ("piano_keys.tim","clsd_drwr.tim"),# ...and the piano room uses for piano_keys
    # The repaired keyboard is a straight re-upload of the piano_keys slot (same
    # rect AND same CLUT rect), swapped in when the piano puzzle is solved, so it
    # inherits every one of piano_keys' sharing relationships.
    ("piano_keys.tim","piano_keys_full.tim"),
    ("kchn_tile.tim", "piano_keys_full.tim"),
    ("cncrte.tim",    "piano_keys_full.tim"),
    ("clsd_drwr.tim", "piano_keys_full.tim"),
    ("con_tile.tim", "opn_drwr.tim"),  # open-drawer texture streams over con_tile...
    ("double_door.tim","opn_drwr.tim"),# ...the same slot delivery/kitchen use for double_door
    ("key.tim",      "cppr_pt.tim"),   # copper pot time-shares the (spent) key slot
    ("gravel_texture.tim", "trees.tim"),   # conservatory streams over delivery slots...
    ("rusty_fence.tim",    "upstairs.tim"),
    ("brick_wall.tim",     "grss.tim"),
    ("double_door.tim",    "con_tile.tim"),# ...double_door is kitchen+delivery
    ("brick_wall.tim",     "bed.tim"),     # master bedroom streams bed over the...
    ("grss.tim",           "bed.tim"),     # ...same delivery slot the conservatory uses for grss
    # chnlnk_dl is the Delivery Area's clone of chnlnk (the room draws chnlnk
    # AND gravel, which share x640). It is parked on the clsd_drwr/cncrte page,
    # none of whose textures the Delivery Area draws, and every one of which its
    # own room re-uploads on entry. Delivery re-uploads chnlnk_dl the same way.
    ("chnlnk_dl.tim", "clsd_drwr.tim"),
    ("chnlnk_dl.tim", "cncrte.tim"),
    ("chnlnk_dl.tim", "kchn_tile.tim"),
    ("chnlnk_dl.tim", "piano_keys.tim"),
    ("chnlnk_dl.tim", "piano_keys_full.tim"),
    ("chnlnk_dl.tim", "xt_dr_outr.tim"),
    ("gravel_texture.tim", "chnlnk.tim"),  # east stairwell streams chnlnk over the...
    ("trees.tim",          "chnlnk.tim"),  # ...same delivery slot the conservatory uses for trees
    ("gravel_texture.tim", "trck_clue.tim"),# attic stairwell streams trck_clue over that
    ("trees.tim",          "trck_clue.tim"),# same slot (8bpp, so it covers x[640,704),
    ("chnlnk.tim",         "trck_clue.tim"),# a whole tpage — the 4bpp trio only use half)
    ("brick_wall.tim",     "xt_dr_lckd.tim"),# attic exit streams its locked exit door
    ("grss.tim",           "xt_dr_lckd.tim"),# over the same delivery slot that already
    ("bed.tim",            "xt_dr_lckd.tim"),# holds brick_wall / grss / bed
    # The unlocked exit door is a straight re-upload of the xt_dr_lckd slot (same
    # rect AND same CLUT rect), swapped in when the exit-door puzzle is solved,
    # so it inherits every one of xt_dr_lckd's sharing relationships — exactly as
    # piano_keys_full does for piano_keys above.
    ("xt_dr_lckd.tim",     "xt_dr_cmplt.tim"),
    ("brick_wall.tim",     "xt_dr_cmplt.tim"),
    ("grss.tim",           "xt_dr_cmplt.tim"),
    ("bed.tim",            "xt_dr_cmplt.tim"),
    # Garden Stairs streams its two new door faces over slots that are already
    # time-shared, so neither adds a restore obligation. Note it could NOT take
    # the obvious delivery pages: it draws brick_wall itself (x768 y0) and chnlnk
    # itself (x640 y0), and the rusty_fence page's right half is anzu3/anzu6,
    # which are resident for the whole run and never re-uploaded.
    #   xt_dr_outr -> the clsd_drwr page (x384 y0), shared with cncrte /
    #   kchn_tile / piano_keys; every consumer re-uploads on room entry.
    #   xt_dr_cg   -> the opn_drwr page (x832 y0), shared with double_door
    #   (delivery/kitchen) and con_tile (conservatory/attic stairwell).
    ("clsd_drwr.tim",      "xt_dr_outr.tim"),
    ("cncrte.tim",         "xt_dr_outr.tim"),
    ("kchn_tile.tim",      "xt_dr_outr.tim"),
    ("piano_keys.tim",     "xt_dr_outr.tim"),
    ("piano_keys_full.tim","xt_dr_outr.tim"),
    ("double_door.tim",    "xt_dr_cg.tim"),
    ("con_tile.tim",       "xt_dr_cg.tim"),
    ("opn_drwr.tim",       "xt_dr_cg.tim"),
    # ...and the garden it looks out over needs grss and gravel_texture, whose
    # OWN slots this room fills with brick_wall (x768) and chnlnk (x640). Both
    # are byte-for-byte clones at pages the room does not touch (see
    # tools/retarget_tim.py), so each inherits its host page's sharing:
    #   grss_gs   -> the stn_stl page (x320 y0), shared with strs / bookshelf
    #   gravel_gs -> the rusty_fence page (x704 y0), shared with upstairs
    ("stn_stl.tim",        "grss_gs.tim"),
    ("strs.tim",           "grss_gs.tim"),
    ("bookshelf.tim",      "grss_gs.tim"),
    ("rusty_fence.tim",    "gravel_gs.tim"),
    ("upstairs.tim",       "gravel_gs.tim"),
    # The Garden Courtyard's re-exported north wall: a hedge with a gate in it,
    # replacing most of the chainlink run. Every delivery/outdoor page was
    # already spoken for by this room (brick_wall x768, chnlnk x640, gravel_gs
    # x704, grss_gs x320, xt_dr_cg x832), so the two new textures took the only
    # 8bpp-wide pages this room draws nothing from, both already time-shared:
    #   hedge    -> the clsd_drwr page (x384 y0). The room DOES upload
    #               xt_dr_outr here (it reuses the Garden Stairs' uploader
    #               wholesale) but never draws it, so hedge simply lands on top
    #               — see the upload order in garden_courtyard_upload_textures.
    #   grdn_gte -> the kchn_wl page (x512 y0), which the dresser prop already
    #               streams over in reception; kitchen_restore_textures puts
    #               kchn_wl back, reception re-uploads the dresser.
    ("clsd_drwr.tim",      "hedge.tim"),
    ("cncrte.tim",         "hedge.tim"),
    ("kchn_tile.tim",      "hedge.tim"),
    ("piano_keys.tim",     "hedge.tim"),
    ("piano_keys_full.tim","hedge.tim"),
    ("chnlnk_dl.tim",      "hedge.tim"),
    ("xt_dr_outr.tim",     "hedge.tim"),
    ("kchn_wl.tim",        "grdn_gte.tim"),
    ("dresser.tim",        "grdn_gte.tim"),
    # Fountain Square, the walled garden north of the courtyard. It draws hedge,
    # grdn_gte, grss_gs and gravel_gs from the Garden Courtyard's slots verbatim
    # (it reuses that room's uploader wholesale), so only its two OWN textures
    # needed homes — and, like the courtyard's, both went to pages this room
    # draws nothing from and that were already time-shared:
    #   fountain -> the brick_wall page (x768 y0), shared with grss / bed /
    #               xt_dr_lckd / xt_dr_cmplt. The Garden Stairs uploader puts
    #               brick_wall here on the way in, so fountain must go up AFTER
    #               it — see fountain_square_upload_textures.
    #   drain    -> the opn_drwr page (x832 y0), shared with double_door /
    #               con_tile / xt_dr_cg. Same ordering rule, same uploader.
    ("brick_wall.tim",     "fountain.tim"),
    ("grss.tim",           "fountain.tim"),
    ("bed.tim",            "fountain.tim"),
    ("xt_dr_lckd.tim",     "fountain.tim"),
    ("xt_dr_cmplt.tim",    "fountain.tim"),
    ("con_tile.tim",       "drain.tim"),
    ("double_door.tim",    "drain.tim"),
    ("opn_drwr.tim",       "drain.tim"),
    ("xt_dr_cg.tim",       "drain.tim"),
    # Outside Catacombs, north of Fountain Square through the same kind of gate.
    # It draws hedge, grdn_gte, grss_gs and gravel_gs from the Garden Courtyard's
    # slots (it reuses that room's uploader wholesale) plus con_tile from the
    # conservatory's narrow upload, so again only its two OWN textures needed
    # homes, and again both went to pages it draws nothing from that were
    # already time-shared:
    #   lamashtu tablet    -> the brick_wall page (x768 y0), shared with grss /
    #               bed / xt_dr_lckd / xt_dr_cmplt / fountain. The Garden Stairs
    #               uploader puts brick_wall here on the way in, so the tablet
    #               must go up AFTER it — see outside_catacombs_upload_textures.
    #   poison_flower_base -> the trck_clue page (x640 y0), an 8bpp full-page
    #               slot shared with gravel_texture / trees / chnlnk (all 4bpp,
    #               using only its left half). Everyone re-uploads on entry:
    #               delivery restores gravel_texture+trees, the east stairwell
    #               and garden stairs chnlnk, the attic stairwell trck_clue.
    ("brick_wall.tim",     "lamashtu tablet.tim"),
    ("grss.tim",           "lamashtu tablet.tim"),
    ("bed.tim",            "lamashtu tablet.tim"),
    ("xt_dr_lckd.tim",     "lamashtu tablet.tim"),
    ("xt_dr_cmplt.tim",    "lamashtu tablet.tim"),
    ("fountain.tim",       "lamashtu tablet.tim"),
    ("gravel_texture.tim", "poison_flower_base.tim"),
    ("trees.tim",          "poison_flower_base.tim"),
    ("chnlnk.tim",         "poison_flower_base.tim"),
    ("trck_clue.tim",      "poison_flower_base.tim"),
    # The Rafflesia (Outside Catacombs) takes the SPIDERS' two sprite slots
    # outright — x320 y128 and x384 y128. This is the first ENEMY-on-enemy
    # timeshare and it is forced: by the time the garden was built there was no
    # 128-row hole left anywhere in VRAM for another pair of 128x128 sprites (the
    # only free band, y[384,480) at x>=800, is 96 rows and runs into the CLUT
    # band at y=480). It is safe because the two never share a room — spiders are
    # house-interior, rafflesias are garden — and main.c streams exactly one of
    # the two pairs in on EVERY room entry, keyed on pending_area. The CLUTs are
    # NOT shared (there was room for two more lines), so only the pixels collide.
    ("spdr_rst.tim",       "rafflesia1.tim"),
    ("spdr_wk.tim",        "rafflesia2.tim"),
    # Maze One, east of Fountain Square through that room's east gate. It draws
    # hedge, grdn_gte and grss_gs from the Garden Courtyard's slots, drain from
    # Fountain Square's narrow upload and poison_flower_base from the Outside
    # Catacombs' — so of its six textures exactly ONE needed a home of its own:
    #   pipe -> the brick_wall page (x768 y0), shared with grss / bed /
    #           xt_dr_lckd / xt_dr_cmplt / fountain / lamashtu tablet. Note this
    #           room draws NO gravel at all, so unlike every other garden room
    #           the gravel_gs page (x704) was free too; brick_wall was chosen to
    #           keep the pipe with the rest of the already-shared 8bpp set. The
    #           Garden Stairs uploader puts brick_wall here on the way in, so
    #           pipe must go up AFTER it — see maze_one_upload_textures.
    ("brick_wall.tim",     "pipe.tim"),
    ("grss.tim",           "pipe.tim"),
    ("bed.tim",            "pipe.tim"),
    ("xt_dr_lckd.tim",     "pipe.tim"),
    ("xt_dr_cmplt.tim",    "pipe.tim"),
    ("fountain.tim",       "pipe.tim"),
    ("lamashtu tablet.tim","pipe.tim"),
    # Maze Two, north of Maze One through that room's north gate. It draws
    # hedge, grdn_gte and grss_gs from the Garden Courtyard's slots,
    # poison_flower_base from the Outside Catacombs' narrow upload and pipe from
    # Maze One's — so of its six textures exactly ONE needed a home of its own:
    #   plinth -> the opn_drwr page (x832 y0), shared with con_tile /
    #             double_door / drain / xt_dr_cg. Maze One's note said the
    #             gravel_gs page (x704 y0) was free for a maze, and for a 4bpp
    #             texture it is — but only its LEFT half. gravel_gs, rusty_fence
    #             and upstairs are all 4bpp and occupy 32 VRAM columns each,
    #             while anzu3/anzu6 sit in the right half at x736; a full-page
    #             8bpp texture there lands on the Anzu. x832 is the page this
    #             room genuinely draws nothing from — it has no drain, the one
    #             texture Maze One took from Fountain Square and this room does
    #             not. It adds no restore obligation: the conservatory re-uploads
    #             con_tile, delivery double_door, and the garden chain puts
    #             xt_dr_cg back through the courtyard's uploader. That uploader
    #             runs on the way in here too, so plinth must go up AFTER it —
    #             see maze_two_upload_textures.
    ("con_tile.tim",       "plinth.tim"),
    ("double_door.tim",    "plinth.tim"),
    ("drain.tim",          "plinth.tim"),
    ("opn_drwr.tim",       "plinth.tim"),
    ("xt_dr_cg.tim",       "plinth.tim"),
    # Rear Gate, west of Fountain Square through that room's west gate. NINE mesh
    # textures — the most of any room in the game — and the first room to want
    # THREE of the opn_drwr page's occupants at once: it draws drain, plinth and
    # double_door together, and all three live at x832 y0. Two therefore had to
    # move, and both moved as byte-for-byte RETARGETS (tools/retarget_tim.py)
    # rather than as new art, exactly as grss_gs/gravel_gs/trees_dl did:
    #   plinth_rg   -> the trck_clue page (x640 y0), an 8bpp FULL page this room
    #               draws nothing from — no flower beds, no chnlnk, and its
    #               gravel and trees resolve elsewhere (see below). Already
    #               time-shared five ways and everyone re-uploads on their own
    #               entry: delivery restores gravel_texture+trees, the east
    #               stairwell and garden stairs chnlnk, the attic stairwell
    #               trck_clue, the catacombs and both mazes poison_flower_base.
    #               The courtyard's uploader puts chnlnk here on the way in, so
    #               plinth_rg must go up AFTER it — see rear_gate_upload_textures.
    #   dbl_dr_rg   -> the frnt_dr/red_crpt page (x320 y256). 4bpp, so it needs
    #               only 32 VRAM columns and stays inside that page's left half —
    #               nothing sits in the right half at x352 y256 but graveolver,
    #               and this does not reach it. frnt_dr and red_crpt are already
    #               an intentional streaming pair (reception over the kitchen's)
    #               and both rooms re-upload on entry, so a third sharer adds no
    #               restore obligation.
    # The room's other seven cost nothing at all: hedge, grdn_gte, grss_gs and
    # gravel_gs through the courtyard's uploader, brick_wall with them via the
    # Garden Stairs', drain through fountain_square_upload_drain(), and its
    # 'trees' resolved to the delivery area's TREES_DL clone, which owns its page
    # outright and is resident from startup.
    ("chnlnk.tim",             "plinth_rg.tim"),
    ("gravel_texture.tim",     "plinth_rg.tim"),
    ("trees.tim",              "plinth_rg.tim"),
    ("poison_flower_base.tim", "plinth_rg.tim"),
    ("trck_clue.tim",          "plinth_rg.tim"),
    ("frnt_dr.tim",            "dbl_dr_rg.tim"),
    ("red_crpt.tim",           "dbl_dr_rg.tim"),
    # ---- THE GARDEN-WEST BANK: Stables (and the Greenhouse behind it) -------
    # These two rooms are reached ONLY through the Rear Gate's west gate, and
    # nothing else is drawn while the player is in them. That is what makes them
    # a BANK rather than one more room squeezing into the shared map: the whole
    # room-art region of VRAM is theirs to re-lay-out, and the four pages the
    # Stables takes are restored for free by whichever room needs them next,
    # because every one of their occupants is already streamed on room entry.
    # See tools/VRAM_MAP_GARDEN_WEST.txt for the bank's own map.
    #   greenhouse      -> the trck_clue page (x640 y0), 8bpp FULL page. Shared
    #                      with chnlnk / gravel_texture / trees / trck_clue /
    #                      poison_flower_base / plinth_rg; delivery restores
    #                      gravel_texture+trees, the stairwells chnlnk, the attic
    #                      stairwell trck_clue, the catacombs and mazes the
    #                      flower bed, the Rear Gate its own plinth_rg.
    #   stables wood    -> the opn_drwr page (x832 y0), 8bpp FULL page. Shared
    #                      with con_tile / double_door / drain / plinth /
    #                      xt_dr_cg; conservatory, delivery, Fountain Square,
    #                      Maze Two and the courtyard chain each put theirs back.
    #   stable glyphs   -> the rusty_fence page (x704 y0), 4bpp so it occupies
    #                      only x[704,736) and stays clear of anzu3/anzu6 at
    #                      x736 — the trap Maze Two fell into with an 8bpp
    #                      plinth. Shared with gravel_gs / upstairs.
    #   greenhouse door -> the frnt_dr/red_crpt page (x320 y256), 4bpp, left half
    #                      only, clear of graveolver at x352 — the same slot and
    #                      the same reasoning as the Rear Gate's dbl_dr_rg, which
    #                      is now its third sharer.
    # The Stables draws NONE of the displaced textures, which is the whole test.
    ("chnlnk.tim",             "greenhouse.tim"),
    ("gravel_texture.tim",     "greenhouse.tim"),
    ("trees.tim",              "greenhouse.tim"),
    ("trck_clue.tim",          "greenhouse.tim"),
    ("poison_flower_base.tim", "greenhouse.tim"),
    ("plinth_rg.tim",          "greenhouse.tim"),
    ("con_tile.tim",           "stables wood.tim"),
    ("double_door.tim",        "stables wood.tim"),
    ("drain.tim",              "stables wood.tim"),
    ("opn_drwr.tim",           "stables wood.tim"),
    ("plinth.tim",             "stables wood.tim"),
    ("xt_dr_cg.tim",           "stables wood.tim"),
    ("gravel_gs.tim",          "stable glyphs.tim"),
    ("rusty_fence.tim",        "stable glyphs.tim"),
    ("upstairs.tim",           "stable glyphs.tim"),
    ("frnt_dr.tim",            "greenhouse door.tim"),
    ("red_crpt.tim",           "greenhouse door.tim"),
    ("dbl_dr_rg.tim",          "greenhouse door.tim"),
    # Keystone Maze, east of Maze One through that room's east gate. Six mesh
    # textures and five of them cost nothing: hedge, grdn_gte, grss_gs and
    # gravel_gs come through the Garden Courtyard's uploader wholesale, and
    # plinth through maze_two_upload_plinth() — the narrow accessor added for
    # this room, because Maze Two's full uploader would also drop its borrowed
    # pipe on x768 y0, which is exactly where this room's own texture goes.
    #   plinth_diamond -> the brick_wall page (x768 y0), an 8bpp FULL page this
    #               room draws nothing from: no fountain, no drain, no flower
    #               bed, no pipe, no bed. Already time-shared seven ways (grss /
    #               brick_wall / bed / fountain / lamashtu tablet / pipe /
    #               xt_dr_lckd / xt_dr_cmplt) and every consumer re-uploads on
    #               its own entry, so it adds no restore obligation. The Garden
    #               Stairs uploader (reached through the courtyard's) puts
    #               brick_wall here on the way in, so plinth_diamond must go up
    #               AFTER it — see keystone_maze_upload_textures.
    ("brick_wall.tim",         "plinth_diamond.tim"),
    ("grss.tim",               "plinth_diamond.tim"),
    ("bed.tim",                "plinth_diamond.tim"),
    ("xt_dr_lckd.tim",         "plinth_diamond.tim"),
    ("xt_dr_cmplt.tim",        "plinth_diamond.tim"),
    ("fountain.tim",           "plinth_diamond.tim"),
    ("lamashtu tablet.tim",    "plinth_diamond.tim"),
    ("pipe.tim",               "plinth_diamond.tim"),
    # ---- THE GARDEN-WEST BANK, second room: the Greenhouse -----------------
    # West of the Stables through the greenhouse door in that room's west wall,
    # and the end of the line — nothing leads on from it. Ten mesh textures, of
    # which FIVE come free: grss_gs and brick_wall through the Garden Courtyard's
    # uploader, and greenhouse / stables wood / greenhouse door through the
    # Stables' (this room calls stables_upload_textures wholesale, so the door
    # panel for its own transition is in VRAM whichever side it is triggered
    # from). The other five are this room's own, and they take pages the STABLES
    # has just stamped and this room draws nothing from — hedge, grdn_gte and
    # stable glyphs — plus two 4bpp left-halves of mansion pages.
    #
    # >>> CLUTS ARE TIME-SHARED HERE TOO, WHICH IS NEW. <<< There is exactly ONE
    # free 256-word CLUT run left in the whole map (y=511, x[288,544)) and
    # spending it on garden decoration would have been the last of it. So the two
    # 8bpp textures take the CLUT of the very texture whose PIXELS they replace:
    # flowerbed sits on hedge's page AND on hedge's palette row, cuneiform _symbols
    # on grdn_gte's. A CLUT is only words in VRAM and it streams with the pixels
    # (texmgr_upload uploads both), so "replaces hedge, palette and all" restores
    # for free the moment garden_courtyard_upload_textures runs again. Same trick
    # for the 4bpp flower-bed clone over stable glyphs.
    #   flowerbed             -> hedge's page      (x384 y0) + hedge's CLUT
    #   cuneiform _symbols    -> grdn_gte's page   (x512 y0) + grdn_gte's CLUT
    #   poison_flower_base_gh -> stable glyphs'    (x704 y0) + its CLUT. A 4bpp
    #                            CLONE of poison_flower_base, which cannot be
    #                            used as it stands: its own TIM is at x640 y0,
    #                            which is where `greenhouse` lives and this room
    #                            draws both at once.
    #   pipe_gh               -> the prpl_wlppr/stove page (x384 y256), 4bpp left
    #                            half, clear of stnd_rnds at x416. Also a clone,
    #                            for the same reason: pipe.tim is on brick_wall's
    #                            page and this room draws brick_wall.
    #   pipe_button_off       -> the wd_dr_crk page (x512 y256), 4bpp left half,
    #                            clear of wx_cb at x544.
    # The last two take 16-word CLUTs at y=502 x[512,544) — a 128-word gap that
    # could never have held a 256-word CLUT anyway, so y=511's run survives whole.
    ("chnlnk_dl.tim",          "flowerbed.tim"),
    ("clsd_drwr.tim",          "flowerbed.tim"),
    ("cncrte.tim",             "flowerbed.tim"),
    ("hedge.tim",              "flowerbed.tim"),
    ("kchn_tile.tim",          "flowerbed.tim"),
    ("piano_keys.tim",         "flowerbed.tim"),
    ("piano_keys_full.tim",    "flowerbed.tim"),
    ("xt_dr_outr.tim",         "flowerbed.tim"),
    ("dresser.tim",            "cuneiform _symbols.tim"),
    ("grdn_gte.tim",           "cuneiform _symbols.tim"),
    ("kchn_wl.tim",            "cuneiform _symbols.tim"),
    ("gravel_gs.tim",          "poison_flower_base_gh.tim"),
    ("rusty_fence.tim",        "poison_flower_base_gh.tim"),
    ("stable glyphs.tim",      "poison_flower_base_gh.tim"),
    ("upstairs.tim",           "poison_flower_base_gh.tim"),
    ("prpl_wlppr.tim",         "pipe_gh.tim"),
    ("stove.tim",              "pipe_gh.tim"),
    ("wd_dr_crk.tim",          "pipe_button_off.tim"),
]

def read_tim(path):
    with open(path, "rb") as f: d = f.read()
    if d[0] != 0x10: return None
    flags = struct.unpack("<I", d[4:8])[0]
    bpp = {0: 4, 1: 8, 2: 16, 3: 16}[flags & 3]
    off = 8; clut = None
    if flags & 8:
        bs = struct.unpack("<I", d[off:off+4])[0]
        cx, cy, cw, ch = struct.unpack("<HHHH", d[off+4:off+12]); clut = (cx, cy, cw, ch); off += bs
    bs = struct.unpack("<I", d[off:off+4])[0]
    px, py, pw, ph = struct.unpack("<HHHH", d[off+4:off+12])   # pw = VRAM columns
    return {"bpp": bpp, "x": px, "y": py, "cols": pw,
            "wpx": pw * (16 // bpp), "h": ph, "clut": clut}

def rects_overlap(a, b):
    ax, ay, aw, ah = a; bx, by, bw, bh = b
    return not (ax+aw <= bx or bx+bw <= ax or ay+ah <= by or by+bh <= ay)


# The report. Wrapped in main() rather than left at module scope so that other
# tools can `from vram_map import read_tim, KNOWN_STREAM_PAIRS` without this
# file printing the whole map at them — tools/vram_map_garden_west.py does
# exactly that, so the two maps read the same TIMs through the same parser.
def main():
    tims = {}
    for p in sorted(glob.glob(os.path.join(TEXDIR, "*.tim"))):
        r = read_tim(p)
        if r: tims[os.path.basename(p)] = r

    print("=" * 78)
    print(" VRAM MAP  (1024 x 512 16-bit words)   generated by tools/vram_map.py")
    print("=" * 78)
    print(__doc__.strip().split("\n\n", 1)[1])   # print the explanatory prose
    print()
    print("FIXED REGIONS")
    print("  Framebuffers : x[0,320)   y[0,480)   (two 320x240 buffers stacked)")
    print("  Font (FntLoad): x[960,1024) y[0,256)  approx")
    print("  CLUT band    : x[0,256)   y[480,512)  (256-word CLUTs, 1 line each)")
    print()

    # Texture pixel slots, sorted by (y, x).
    hdr = "  %-20s %-5s %5s %5s %5s %5s %5s   %s" % (
        "texture", "bpp", "x", "y", "wpx", "h", "Voff", "clut(x,y)")
    print("TEXTURE PIXEL SLOTS  (Voff = y %% 256; Voff>=128 is window-sensitive)")
    print(hdr)
    print("  " + "-" * 74)
    for name, r in sorted(tims.items(), key=lambda kv: (kv[1]["y"], kv[1]["x"])):
        voff = r["y"] % 256
        warn = "  <-- Voff>=128" if voff >= 128 else ""
        clut = "%d,%d" % (r["clut"][0], r["clut"][1]) if r["clut"] else "-"
        print("  %-20s %-5d %5d %5d %5d %5d %5d   %s%s" %
              (name, r["bpp"], r["x"], r["y"], r["wpx"], r["h"], voff, clut, warn))
    print()

    # CLUT slots, sorted by y.
    print("CLUT SLOTS  (8bpp = 256 words wide, 4bpp = 16)")
    for name, r in sorted(tims.items(), key=lambda kv: (kv[1]["clut"][1] if kv[1]["clut"] else 0)):
        if not r["clut"]: continue
        cx, cy, cw, ch = r["clut"]
        print("  y=%-4d x[%d,%d)  %s" % (cy, cx, cx + cw, name))
    print()

    # Overlap detection.
    print("OVERLAP CHECK")
    def pair_is_known(a, b):
        for x, y in KNOWN_STREAM_PAIRS:
            if {a, b} == {x, y}: return True
        return False
    names = list(tims.keys())
    unexpected = []
    for i in range(len(names)):
        for j in range(i+1, len(names)):
            a, b = names[i], names[j]
            ra, rb = tims[a], tims[b]
            ta = (ra["x"], ra["y"], ra["cols"], ra["h"])
            tb = (rb["x"], rb["y"], rb["cols"], rb["h"])
            if rects_overlap(ta, tb):
                tag = "time-shared (streaming)" if pair_is_known(a, b) else "!! UNEXPECTED COLLISION"
                print("  %-40s %-20s %s" % (a, b, tag))
                if not pair_is_known(a, b): unexpected.append((a, b))
    if not unexpected:
        print("  No unexpected pixel collisions.")
    print()

    # CLUT overlap detection. A CLUT is just words in VRAM: drop one on top of a
    # texture's pixels and those scanlines display the palette as image data — a
    # bright band across the sprite, in every room, with no crash and no warning.
    # This bit the demon dog once (trck_clue's and bl_ky_stn's 256-word CLUTs were
    # placed at x[512,768) on y=481/482, straight through ddog_alert's
    # x[640,672) y[448,512) body). Pixel-vs-pixel checking alone does NOT see it.
    print("CLUT OVERLAP CHECK  (CLUT words landing in pixel data, or on each other)")
    clut_bad = []
    for name, r in sorted(tims.items()):
        if not r["clut"]: continue
        cx, cy, cw, ch = r["clut"]
        crect = (cx, cy, cw, ch)
        for other, ro in sorted(tims.items()):
            prect = (ro["x"], ro["y"], ro["cols"], ro["h"])
            if rects_overlap(crect, prect):
                print("  !! %s's CLUT x[%d,%d) y=%d lands in %s's PIXELS x[%d,%d) y[%d,%d)"
                      % (name, cx, cx + cw, cy, other,
                         ro["x"], ro["x"] + ro["cols"], ro["y"], ro["y"] + ro["h"]))
                clut_bad.append((name, other))

    names_c = [n for n in sorted(tims) if tims[n]["clut"]]
    for i in range(len(names_c)):
        for j in range(i + 1, len(names_c)):
            a, b = names_c[i], names_c[j]
            ca, cb = tims[a]["clut"], tims[b]["clut"]
            if rects_overlap(ca, cb) and not pair_is_known(a, b):
                print("  !! %s's CLUT overlaps %s's CLUT (both at y=%d)" % (a, b, ca[1]))
                clut_bad.append((a, b))

    if not clut_bad:
        print("  No CLUT collisions.")
    print()
    print("Regenerate with:  python tools/vram_map.py > tools/VRAM_MAP.txt")



if __name__ == "__main__":
    main()