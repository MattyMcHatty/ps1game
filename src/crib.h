#ifndef CRIB_H
#define CRIB_H

#include <stdint.h>
#include "render.h"
#include "title.h"

/* Crib: an iron cot, Chapter 3's fifth prop and the first thing in the
   Catacombs that is furniture rather than machinery. One stands in the alcove in
   the SOUTH-WEST corner of the Room of Arms (src/room_of_arms.c places it).

   ONE texture of its own ("crib", \TEXCTCMB\CRIB.TIM) and one mesh
   ("Crib.smx" -> assets/props/crib.smd, 60 primitives, 3644 bytes — remodelled
   down from 100 and 5692 before it had ever been placed).

   >>> IT IS NOT STATIC ANY MORE. <<< The header used to say "STATIC, AND ONLY
   FOR NOW ... THE MOVEMENT IS COMING", and this is it: a crucifaxe swing wakes
   the cot, it rocks, it throws a beam of light out of its top face, and it pours
   ten Creeps into the room. Everything that note said was built for the movement
   turned out to be exactly what the movement needed, so it is worth repeating
   why neither can be simplified away:

     - THE INSTANCE CARRIES ITS OWN rot_y AND ITS OWN BAKED AABB, rather than
       the module holding one world box. crib_place() is still the one function
       that derives the box from the instance.
     - THE DRAW REBUILDS ITS MODEL MATRIX EVERY FRAME from the instance's
       fields. So the rock is a write to one field and not a cache
       invalidation — see cribs_update().

   =========================================================================
    THE ENCOUNTER
   =========================================================================
   A state machine per instance, driven by cribs_update() and woken by
   cribs_try_hit() from src/crucifaxe.c. The whole timeline, in frames at 60 to
   the second:

     CRIB_IDLE      at rest, unsolved. A connected crucifaxe swing -> ACTIVE.
     CRIB_ACTIVE    tick 0            the rock starts, at full amplitude, and
                                      SFX_CREEP is keyed on (CRIB_LOOP_FRAMES)
                    tick 0..180       the beam grows from nothing to full (3 s)
                    tick 180..240     the beam holds, nothing spawns (1 s)
                    tick 240          the first Creep
                    every 90 after    the next, to ten in all (last at 1050)
                    then it waits, still rocking and lit, for the tenth body
     CRIB_CLOSING   all ten dead: SFX_CREEP stops, the beam fades and the rock
                    damps out over 60 frames. >>> THE SOLVED BIT IS SET ON ENTRY TO THIS
                    STATE, NOT AT THE END OF IT. <<< A player who walks out
                    during the one-second outro has beaten it; making them
                    watch the light go out to keep the result would be the
                    FLAG_ASAG_DEAD trap (player.h) inverted — that flag is set
                    LATE because a room init reads it, and this one is set
                    EARLY because nothing reads it but the crib itself.
     CRIB_SOLVED    at rest, solved. A swing -> ECHO.
     CRIB_ECHO      one damped rock, 90 frames, and nothing else happens. The
                    brief's "it will only rock once".

   THE ENCOUNTER IS NOT RESUMABLE and src/creep.h argues that at length: the
   creeps are transient and carry no save state, so leaving the room, dying, or
   loading mid-fight puts the cot back to IDLE with the whole thing to do again.
   Only the outcome sticks.

   ---- WHICH ROOMS HAVE BEEN SOLVED ---------------------------------------
   >>> IT IS ONE BIT PER ROOM, IN THE WorldDelta, AND IT IS DELIBERATELY NOT A
   GameFlag. <<< game_flags is a 32-bit word in SaveData and 31 of its bits are
   spent (FLAG_ASAG_DEAD is bit 30; player.h's _Static_assert is one flag from
   firing). A mechanic that is going into "several other rooms" needs a SET of
   bits, and there is exactly one left, so the flag word was never an option —
   worth knowing before reaching for game_flag_set() for the next puzzle either.

   The bitmask is keyed by room_index(area), which is the fixed table in
   src/world.c and so is stable across playthroughs that took different routes.
   That is a different key from the one every global area-tagged ENTITY array
   uses (canonical_index(), keyed by (room, ordinal)) and the difference is not
   an oversight: a crib is placed by its ROOM's init and cribs_clear() empties
   the array on every room entry, so there is no stable whole-game ordinal for
   an instance to be keyed by. The room is the only stable identity available,
   and it is also the identity the design actually cares about.

   >>> SO THE FIRST CRIB PLACED IN AN AREA IS THE ENCOUNTER, AND ANY FURTHER
   CRIB IN THE SAME AREA IS SCENERY. <<< crib_place() enforces it. One bit
   cannot describe two independent encounters in one room, and the honest way to
   spend one bit is to make the second cot furniture rather than to let two of
   them share a solved flag and quietly solve each other. A room that genuinely
   needs two live cribs wants a bit PAIR here and crib_place()'s gate lifted;
   that is a ten-line change and this paragraph is the note to read first.

   ADDING THE MECHANIC TO A ROOM is therefore: call crib_place() from its init
   with STATE_<ROOM>, call crib_upload_texture() and creeps_upload_texture()
   from its uploader, call cribs_update() and update_creeps() in its branch of
   update_current_area, and call cribs_draw() and draw_creeps() in its draw.
   Nothing in world.c, savegame.c or player.h has to move — the room already has
   a room_index() and therefore already has its bit.

   ---- THE ROCK -----------------------------------------------------------
   A tilt about the cot's OWN LONG AXIS — side to side, the way a cradle on
   curved runners moves — and not a yaw shimmy on rot_y. The long axis is model
   X (350 units against Z's 200), so the tilt is a rotation about X.

   >>> THAT CANNOT BE ONE RotMatrix CALL. <<< PSn00bSDK builds Rx * Ry * Rz (its
   header spells the product out), so {tilt, rot_y, 0} applies the yaw FIRST and
   then tilts about the WORLD x axis — which at this prop's rot_y of 512 is 45
   degrees off its long axis, so the cot would rock diagonally. The draw composes
   the two explicitly instead: MulMatrix0(&yaw, &tilt, &m) gives yaw * tilt, i.e.
   the tilt applied first, in model space. src/rabisu.c's foot-slash lean does
   the same thing for the same reason, and src/valve_handle.c's note is the
   general statement of it.

   THE BAKED AABB IS NOT RE-DERIVED WHILE IT ROCKS, and that is a choice rather
   than an omission. Tilting a 200-deep, 195-tall box by CRIB_ROCK_AMP (about 17
   degrees) widens its plan footprint across Z by roughly 23 units a side. The
   player is already held CR_PLAYER_RADIUS (75) clear of the static box and the
   room holds them 195 off every wall, so 23 units of breathing is inside the
   standoff and invisible; re-deriving four corners every frame to chase it would
   cost the arithmetic and buy nothing. If the amplitude is ever raised a long
   way, call crib_place()'s derivation again from cribs_update() — it is written
   so it can be.

   ---- THE BEAM -----------------------------------------------------------
   The brief says the TOP TWO POLYS let the light out, and they are poly 8 and
   poly 9 of Crib.smx: two quads at y=-195 covering x[0,175] and x[-175,0],
   z[-100,100], which together are exactly the measured box's top face. So the
   beam is a rectangular shaft standing on cr_min/cr_max — DERIVED from the same
   six numbers the collision box is, not authored — and it therefore follows a
   re-export the way everything else about this prop does.

   It is drawn INSIDE the instance's model matrix, so it rocks with the cot.
   POLY_G4 rather than POLY_F4 because the fall-off is the whole effect: the
   bottom pair of each quad carries the light and the top pair carries black,
   which additive blending turns into nothing at all (src/incinerator.c's opening
   glow makes the same move and its note is the long version).

   IT FLARES OUTWARD AS IT RISES — CRIB_BEAM_TOP_SCALE, about 1.8x the rim's
   plan size at the far end — so it is a splayed cone and not a box. Each
   perimeter quad is a trapezoid, and the scale is applied to each corner's
   offset from the cot's own plan CENTRE so the cone opens symmetrically whatever
   the mesh's origin happens to be.

   >>> IT IS SUBDIVIDED 3 x 3 AND THAT IS A DRAWING LIMIT, NOT A STYLE CHOICE.
   <<< The GPU DROPS a primitive whose screen extent passes 1023 pixels in either
   axis — it does not clip it (tools/ADDING_AN_ENEMY.txt mistake 13). At
   gte_SetGeomScreen(256) a span S passes 1023 at a VIEW DEPTH of S * 256 / 1023,
   so the undivided shaft would vanish inside a depth of 175, and view depth is
   not distance: a player standing 269 from the cot and merely TURNING drives it
   toward zero. The grid has to put every quad's threshold inside the 75 units the
   collision already holds the player off the box, so that no piece can be dropped
   while it is still on screen — at 3 x 3 the widest quad is 210 across (threshold
   52) and the tallest 233 (threshold 58).

   >>> SIZE THE GRID ON THE WIDEST QUAD, WHICH SINCE THE FLARE IS AT THE TOP.
   <<< It was 2 x 4 while the shaft was a box, and two columns no longer fit: the
   long face is 350 at the rim but 630 at the far end, which at two columns is 315
   a quad and a threshold of 79 — PAST the 75 the collision guarantees, i.e.
   droppable in play. Re-derive both numbers from CRIB_BEAM_TOP_SCALE whenever it
   moves. A round number is not the input.

   BOTH WALLS OF THE SHAFT ARE DRAWN — there is no backface cull — because an
   additive volume wants them. The far wall and the near wall sum, so the middle
   of the shaft is brighter than its edges, which is what a beam looks like. That
   is also why CRIB_BEAM_PEAK is 150 and not 255: two layers of 150 saturate to
   white at the core and stay short of it at the rim, where one layer of 255
   would blow the whole thing out flat.

   >>> IT IS 36 ADDITIVE QUADS OVER A LOT OF SCREEN AND FILL RATE IS THIS
   CONSOLE'S WEAK POINT — AND THE FLARE MADE EACH ONE BIGGER. <<< Nothing has
   measured it. If the Room of Arms drops frames, read
   tools/DIAGNOSING_FRAME_RATE.txt and measure U/D/G FIRST — optimising this room
   by reasoning alone once made it 56% worse. The honest lever is then dropping
   the far wall with a gte_nclip(), which halves the fill at the price of the
   summed core above. Coarsening the grid is NOT a lever any more: 3 x 3 is the
   minimum the flared shaft's own width allows, so going below it trades frames
   for a beam that blinks out when the player stands beside the cot.

   The intensity ramp is CUBIC rather than linear, and crib_beam_level() in the .c
   has the table showing why — a linear ramp over these same 180 frames looked
   like it finished in one second, because two summed additive layers are already
   near white at a third of full brightness. THE FIX WAS THE CURVE, NOT THE
   DURATION; do not reach for CRIB_BEAM_RAMP if it ever needs to feel slower
   still.

   =========================================================================

   >>> ITS COLLISION IS ITS OWN MESH. <<< There is no Crib_mesh.smx and no
   smx_to_collision.py pass for this prop: crib_load_assets() walks the loaded
   SMD's vertices for the real min/max on all three axes, and crib_place() bakes
   those into a world AABB for the instance. Re-export the model bigger and the
   box follows it on the next build, with nothing to keep in step by hand. Same
   arrangement as src/sconce.h, src/oil_dispenser.h and src/save_point.c, and it
   means the same thing: the engine collides BOXES, so the mesh's job is to SIZE
   the box.

   IT IS MEASURED min/max AND NOT AS HALF-EXTENTS, which the oil dispenser's
   header argues at length. THIS MODEL WOULD SURVIVE HALF-EXTENTS — as authored
   it spans x[-175,175] z[-100,100] y[-195,0], i.e. it is centred on its origin
   in plan and stands up off the floor the origin sits on, so |vx|max and |vz|max
   would describe it exactly. The measured form is here anyway because it costs
   the same six comparisons and it cannot be wrong after a re-export that shifts
   the origin — which is precisely the change an animator makes when they decide
   the thing should rock about one end.

   SO x/z IS THE CRIB'S CENTRE IN PLAN and y is the floor under it (world y = y +
   GROUND_FLOOR_Y, so a room whose floor is y=0 passes -GROUND_FLOOR_Y).
   rot_y 0 lays the LONG axis (350) along X and the short one (200) along Z.

   Area-tagged, like the sconces, the levers and the oil dispenser, so
   cribs_collide() and cribs_draw() can be called unconditionally from the shared
   routines and are a no-op in every other room. Without the tag an instance left
   standing would block the player invisibly anywhere its coordinates land — and
   the Room of Arms' footprint is centred on the origin, so that is a live risk
   here rather than a formality (the note in room_of_arms_init() spells it out).

   CHAPTER 3 ONLY, and it is NOT in src/area_bank.c's free list for that reason:
   the purge at the catacomb mouth gives back the props Chapters 1 and 2 use, and
   this is one of the few that has to survive it. Its texture is an ordinary
   deferred TEXBANK_CATACOMBS registration, so it holds no pixels at all until
   the chapter door; only its ~3.6 KB of geometry is permanent.

   >>> IT IS THE 70th texmgr REGISTRATION OF 72, AND THE CREEP IS THE 71st.
   <<< TEXMGR_MAX in src/texmgr.c is the cap and texmgr_register past it returns
   -1 SILENTLY, which breaks that texture in every room that draws it. ONE LEFT.
   The way to spend none is the one the Room of Arms' cobblestone and door slots
   take: if another module already uploads the page you need, call ITS narrow
   uploader and use TIM_SLOT() for the header rather than registering a second
   RAM copy of the same file. Count with py tools/heap_budget.py before adding
   the seventy-second.

   THE VRAM PAGE IS x576 y0 (8bpp, Voff 0) — an exact 128x128 fit, and the first
   use of a page tools/VRAM_MAP_CATACOMBS.txt has been marking "AVAILABLE, needs a
   way back first" ever since the way back was written. It is not this prop's to
   owe: anzu_tex_stream() re-reads all six Anzu tiles off the CD on piano-room
   entry (src/anzu_tex.c says so at length, and names x576/x704 y0 as the two whole
   pages it unlocks), and kitchen_stream_owned_textures() re-reads red_wlppr on
   kitchen entry. BOTH CALLS ARE LOAD-BEARING FOR THIS PROP NOW — delete either
   and two frames of the Anzu puzzle, or a wall of the kitchen, become a cot until
   the console is reset.

   >>> IT IS x576 AND NOT x704, AND IT WENT TO x704 FIRST. <<< The catacombs map
   calls the two pages equally available because it prints only the occupants it
   knows need a way back; x704's LEFT half is six 4bpp garden textures it does not
   print at all, and an 8bpp 128 texture takes all 64 columns of a page. py
   tools/vram_map.py is what caught that, and it is the thing to run before
   believing either map about a page.

   The CLUT is borrowed on the arms field's argument, from anzu2.tim at (256,484):
   a palette belonging to a texture whose PIXELS this one is already displacing,
   so the two go back together in the one stream.

   Voff 0 is what lets the Room of Arms' own 128x128 texture window serve this
   prop rather than mask it, which is the whole requirement — a Voff >= 128 page
   would have the cot sampling 128 texels too high in every windowed room.

   >>> THE PROP NO LONGER DEPENDS ON THAT WINDOW, AND IT DID BEFORE THE REMODEL.
   <<< The first export reached u=128, one past the tile, and only landed back on
   the art because the window wrapped it mod-128; the 60-poly model keeps every UV
   inside u[2,127] v[2,124]. So it is now correct with or without one, which is a
   property of THIS export and not a rule — re-UV the model past 127 on any axis
   and the window is load-bearing again. */

#define MAX_CRIBS 4

/* ---- Encounter timing, all in frames at 60 to the second ---------------- */
#define CRIB_BEAM_RAMP        180   /* nothing -> full intensity: 3 seconds   */
#define CRIB_BEAM_HOLD         60   /* lit, and one more second before a body */
#define CRIB_SPAWN_INTERVAL   90   /* a Creep every 1.5 seconds              */
#define CRIB_CREEP_TOTAL       10   /* ten of them, and then no more          */
#define CRIB_CLOSING_FRAMES   60   /* the outro: beam out, rock damps, 1 s    */
#define CRIB_ECHO_FRAMES      90   /* a solved crib's single rock             */

/* ---- THE ENCOUNTER'S LOOP -------------------------------------------------
 * SFX_CREEP is keyed on the first frame of CRIB_ACTIVE and re-keyed every
 * CRIB_LOOP_FRAMES for as long as the cot is pouring, so the room has a voice
 * under it from the swing that wakes the crib to the death of the tenth Creep.
 * It is silenced on entry to CRIB_CLOSING — with the SOLVED bit, at the START of
 * the outro — and again by cribs_rest(), which is what covers leaving the room,
 * dying and saving.
 *
 * >>> 350 IS THE CLIP'S OWN LENGTH AND NOT A ROUND NUMBER. <<< creep.vag is
 * 2302 ADPCM blocks of 28 samples at 11025 Hz, i.e. 350.8 frames at 60, so
 * re-keying on 350 lands just INSIDE the tail and the loop has no seam. Round it
 * up and there is an audible gap every six seconds; re-cut the clip and this
 * number has to be re-derived from the new one (tools/ADDING_A_SOUND.txt STEP 1
 * prints the block count).
 *
 * >>> IT IS A C-SIDE RETRIGGER AND IT MUST STAY ONE. <<< The clip has no ADPCM
 * loop flag on its first block, which is the only reason SFX_CREEP could be put
 * on voice 20 at all — a hardware-looped sample latches its own address into
 * that voice's repeat register for the rest of the run and breaks every one-shot
 * later given the voice. sound.c's note on the voice is the long version, and
 * zombie.c's groan is the pattern this copies. */
#define CRIB_LOOP_FRAMES     350

/* The rock. CRIB_ROCK_PERIOD is one complete left-right-left cycle, so
   CRIB_ECHO_FRAMES is exactly one of them. CRIB_ROCK_AMP is in PS1 angle units
   (4096 = a full turn), so 190 is a peak tilt of about 16.7 degrees either
   way — enough to read across the room, short of a cot that is about to go
   over. See the AABB note at the top before raising it much. */
#define CRIB_ROCK_PERIOD      90
#define CRIB_ROCK_AMP        190

/* The beam. Height is measured UP from the cot's top face, and -Y is up, so the
   shaft's far end is at (rim y - CRIB_BEAM_HEIGHT). 700 takes it from the rim at
   world y=-195 to y=-895, which is just past the Room of Arms' wall tops at
   -800 — the room has no drawn ceiling at all (src/room_of_arms.c says so), so
   the light reads as going up into the dark rather than stopping on a surface.

   CRIB_BEAM_COLS x CRIB_BEAM_ROWS is the subdivision, and the top of this file
   explains why it is a drawing limit rather than a preference. CRIB_BEAM_PEAK is
   the per-layer brightness at the rim; both walls of the shaft are drawn and
   they sum.

   >>> IT FLARES: THE SHAFT IS CRIB_BEAM_TOP_SCALE TIMES WIDER AT ITS FAR END
   THAN AT THE RIM. <<< The scale is in 1/256ths, so 460 is about 1.8x, applied
   to each plan corner's offset from the cot's own plan CENTRE and interpolated
   linearly with height. That turns each perimeter quad from a rectangle into a
   trapezoid and the shaft from a box into a splayed cone, which is what light
   escaping a gap actually does.

   >>> AND THE FLARE IS WHY COLS WENT 2 -> 3 AND ROWS 4 -> 3. <<< The 1023-pixel
   primitive drop (see the top of this file) is sized on the WIDEST quad, and the
   widest quad is now at the top: 350 x 1.8 = 630 across the long face, which at
   two columns is 315 a quad and a drop threshold of 315 * 256 / 1023 = 79 — past
   the 75 units the collision holds the player off the box, i.e. droppable while
   still on screen. Three columns makes it 210 and a threshold of 52, comfortably
   inside 75. Dropping to three rows then buys most of the quad count back (36
   against the old 32) and its own threshold, 233 * 256 / 1023 = 58, is still
   inside 75. BOTH numbers have to be re-derived from CRIB_BEAM_TOP_SCALE
   whenever it moves; the widest dimension is the one that governs. */
#define CRIB_BEAM_HEIGHT     700
#define CRIB_BEAM_COLS         3
#define CRIB_BEAM_ROWS         3
#define CRIB_BEAM_PEAK       150
#define CRIB_BEAM_TOP_SCALE  460   /* 1/256ths: 460 ~ 1.8x the rim's width */

/* The crucifaxe's reach to the cot, taken to the box's SURFACE and not to its
   centre. It has to be: the player is held 75 clear of an AABB whose plan
   half-extent is 194, so a swing aimed at the centre from the corner is a
   Manhattan 538 away and SWING_RANGE is 350. The living statue's and Hadad's
   reach tests are in their own modules for exactly this reason, and
   cribs_try_hit() is here for it too.

   >>> 210 AND NOT 140, AND THE DIFFERENCE IS THE CORNERS. <<< The push-out in
   cribs_collide expands the box by the caller's radius (75, from collision.c)
   and resolves along the shallowest axis, so the boundary the player can stand
   on is an expanded RECTANGLE and not a rounded one. Along a face that is a
   clamped Manhattan distance of 75; at a CORNER it is 75 + 75 = 150. A reach of
   140 would therefore work everywhere except the two corners of the alcove the
   room actually funnels the player toward, which is the worst possible place
   for it to fail and would have read as "the axe sometimes does nothing". 210
   clears the 150 with real margin and is still well inside SWING_RANGE.

   Work this out again from the COLLISION RADIUS if either changes: the floor on
   this number is twice whatever cribs_collide is called with, not once. */
#define CRIB_HIT_REACH       210

typedef enum {
    CRIB_IDLE,      /* unsolved, at rest                                     */
    CRIB_ACTIVE,    /* rocking, lit, pouring — the encounter                  */
    CRIB_CLOSING,   /* all ten dead: beam fading, rock damping out            */
    CRIB_SOLVED,    /* solved, at rest                                       */
    CRIB_ECHO,      /* solved and just hit: one rock, and nothing else        */
} CribState;

void crib_load_assets(void);      /* startup: geometry + DEFERRED registration  */
void crib_upload_texture(void);   /* room entry: pure LoadImage, no CD           */
void cribs_clear(void);           /* drop every placed instance                 */

/* Place one. x/z is the model's CENTRE in plan and y the floor reference (world
   y = y + GROUND_FLOOR_Y). rot_y is 0..4096 = a full turn.

   The FIRST crib placed in an area is that room's encounter and starts IDLE or
   SOLVED according to the saved bitmask; any later one in the same area is
   scenery and never wakes. See the note at the top of this file. */
void crib_place(GameState area, int32_t x, int32_t y, int32_t z, int32_t rot_y);

/* Run the state machine for every crib in the current area: the rock, the beam
   ramp, the spawn cadence and the test for the encounter being over. Call it
   from the room's branch of update_current_area, beside update_creeps(). */
void cribs_update(void);

/* Abandon any encounter in progress: a running crib goes back to IDLE, or to
   SOLVED if its bit is set. Called from world_leave() beside creeps_reset(),
   which means on every room change AND on every save, because savegame_capture()
   snapshots through world_leave().
 *
 * >>> THE SAVE CASE IS THE REASON THIS FUNCTION EXISTS. <<< world_leave() drops
 * the creeps, and it does that while the player is still standing in the room.
 * Without this, saving with all ten released and one alive would empty the room
 * and hand the player the solve on the next frame — the encounter's end
 * condition is "ten released and none alive", and a save satisfies the second
 * half of it for free. Resetting the crib in the same breath makes a save
 * mid-encounter an ABANDONMENT, which is what leaving the room already is and
 * what src/creep.h says the bargain is. It costs a player who saves mid-fight
 * the progress they had; that is the same deal zombies_rest() has always made
 * (it walks every living zombie back to its spawn on save) and it is the honest
 * side of the trade to be on. */
void cribs_rest(void);

/* The crucifaxe's hit test. Returns 1 if a swing connected with a crib — which
   wakes an IDLE one into the encounter, starts a SOLVED one's single rock, and
   does nothing at all to one that is already moving. Called from
   src/crucifaxe.c beside the other props' try_smash/try_hit functions. */
int  cribs_try_hit(void);

void cribs_draw(RenderContext *ctx);
void cribs_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* ---- The solved set ------------------------------------------------------
 * One bit per room, keyed by world.c's room_index(area). Read and written by
 * world.c's save codec and by nothing else; the crib asks crib_room_solved()
 * live every frame rather than caching the answer in the instance, so the order
 * of room init, world_enter() and savegame_apply_pending() cannot matter.
 * See the long note at the top of this file for why this is not a GameFlag. */
uint32_t crib_solved_mask(void);
void     crib_solved_mask_set(uint32_t mask);   /* load; replaces the whole set */
int      crib_room_solved(GameState area);
#endif
