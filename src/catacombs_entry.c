#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "room_arena.h"
#include "cull_arena.h"
#include "tim_slots.h"
#include "camera.h"
#include "catacombs_entry.h"
#include "collision.h"
#include "catacombs_entry_mesh_collision.h"
#include "catacombs_entry_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "save_point.h"
#include "player.h"             /* show_pickup_msg_raw, current_weapon */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* Catacombs Entry — see catacombs_entry.h for the layout and the chapter note. */

static SMD  *catacombs_entry_smd  = NULL;
static void *catacombs_entry_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   Cull and fog-far are equal, the invariant that makes culling invisible:
   nothing is dropped until the fog has already faded it into the background.

   That invariant holds for every value the factors below can produce, because
   both are derived from the same resolved number (ce_fog_far).

   THE BASE IS HALF WHAT THIS ROOM SHIPPED WITH. It was 3200/900, set by the
   LOWER HALL — it runs x[1800,4800], so from the west end the inner door is
   3000 units away and used to be visible as a door rather than as a hole. 1600
   deliberately does NOT reach it: the far end of the hall is dark now, and the
   player walks into that door rather than toward it. Everything else here is a
   corridor or a 1500-square chamber and was bounded by its own walls long
   before either number. The near value is halved with the far one so the fog
   CURVE keeps its shape instead of becoming a hard edge at half the distance.

   Shortening it cannot cost frames — the distance cull only ever rejects more
   at 1600 than it did at 3200 — but 1073 prims is a middling mesh for this game
   (Maze One is 2000+) and the corridors mean most of it is behind geometry
   rather than culled, so a factor that LENGTHENS it is the one to measure per
   tools/DIAGNOSING_FRAME_RATE.txt before adding. */
#define CE_BASE_FOG_NEAR   450
#define CE_BASE_FOG_FAR   1600

/* ---- View-distance factors -------------------------------------------------
   The view distance is no longer a constant. It is the base above times a SCALE
   in 1/256ths, resolved once a frame by ce_view_resolve() into ce_fog_near /
   ce_fog_far — which the mesh draw culls and fogs with, and which g_fog_* hands
   to everything else that fogs in this room. Nothing else may read the base
   directly.

   Factors ADD their bonus to the scale rather than multiplying it, so no two of
   them can be made to cancel or compound by the order they happen to be applied
   in, and a factor that is off costs one test.

   ADDING A FACTOR (a lit brazier the player is standing near, a room state, a
   pickup): give it a CE_VIEW_* bonus and one term in ce_view_target(). Keep it
   additive, keep it in 1/256ths, and let the ease below carry it in.

   Both factors so far are the Helluminator's, and they are TWO, not one with a
   condition:

     EQUIPPED (not lit)   +128   the lantern is the light the player carries, so
                                 raising it is what pushes the dark back. A
                                 player who had to hold the trigger to see would
                                 never put it down.
     BURNING              +128   the flame itself, on top of the above. Only
                                 while oil is actually going (helluminator.h:
                                 Square down AND oil left), so it stops with the
                                 tank and cannot be held for free.

   They stack, and stacking is the point: holding the lantern is +50% (far 2400,
   near 675) and burning is +100% (far 3200, near 900) — which is exactly the
   distance this room shipped with, so a burst buys back the ORIGINAL sight line
   down the lower hall and nothing more. */
#define CE_VIEW_UNIT        256
#define CE_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define CE_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */

/* How fast the scale chases its target, in 1/256ths per frame. NOT instant, and
   the cull line is why: geometry between the old fog-far and the new one has
   faded to exactly the clear colour, so a one-frame jump would POP all of it in
   at once and read as a draw-distance change rather than as light.

   8 a frame walks one +128 bonus across in 16 frames. That is deliberately
   close to the lantern model's own HELL_GLOW_RAMP (10 frames, helluminator.c):
   the burn factor is switched by the trigger, so the room has to open out on
   roughly the same swell as the flame that is opening it, and a one-second
   burst — the tick the lantern charges oil in — must reach full distance well
   inside itself rather than easing for half of it. */
#define CE_VIEW_RATE  8

static int32_t ce_view     = CE_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t ce_fog_near = CE_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t ce_fog_far  = CE_BASE_FOG_FAR;

static int32_t ce_view_target(void) {
    int32_t s = CE_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += CE_VIEW_HELL_BONUS;
        /* Only asked INSIDE the equipped test: helluminator_burning() cannot be
           true for an unequipped lantern, but nesting it says so rather than
           relying on the weapon layer to keep clearing it. */
        if (helluminator_burning()) s += CE_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry: there is no previous frame to ease from
   and walking in with the lantern already up must not open the room out over
   the first half second. */
static void ce_view_resolve(int snap) {
    int32_t target = ce_view_target();
    if (snap) {
        ce_view = target;
    } else if (ce_view < target) {
        ce_view += CE_VIEW_RATE;
        if (ce_view > target) ce_view = target;
    } else if (ce_view > target) {
        ce_view -= CE_VIEW_RATE;
        if (ce_view < target) ce_view = target;
    }
    ce_fog_near = (CE_BASE_FOG_NEAR * ce_view) >> 8;
    ce_fog_far  = (CE_BASE_FOG_FAR  * ce_view) >> 8;
}

/* UNDERGROUND, so it does NOT take the garden's purple sky — the player has
   walked in through a stone facade and the night is behind them now. Near-black
   with a cold lift, on Asag's arena's reasoning (src/asag_arena.c): a fog that
   saturates to a true 0,0,0 makes the cull line invisible, which sounds ideal
   and is how you lose an hour to "the end of the hall is missing". */
#define CE_FOG_R             7
#define CE_FOG_G             6
#define CE_FOG_B             9

/* Wall standoff. The DEFAULT 195, not the 260 the garden rooms use for their
   1100-tall hedges, and the corridors are why: ramp one is 642 wide between the
   walls at x=+-321, and 260 would leave a 122-wide slot to walk down. 195
   leaves 252, which is comfortable. The cost is that the entry chamber's 1100-
   tall walls can clip against the GTE near plane if the player stands right
   against one — the same trade the interior rooms all make, and a corridor the
   player fights is worse than a corner that shimmers. */
#define CE_WALL_RADIUS     195

/* Standing eye on the ENTRY CHAMBER's floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. Used for the spawn only — apply_height
   settles cam_y every frame afterwards, and it has to, because this room falls
   1240 units between its two ends. */
#define CE_EYE_Y  (0 - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   SEVEN, one per floor face the collision generator found, and this is the
   first room in the game with TWO ramps that are not a stairwell.

   They do not overlap in XZ — each pair that looks adjacent meets on a single
   line (the landing and ramp two share z=4192; the lower approach and the
   connector share x=1621) — so unlike reception's stack the order here is not
   load-bearing. They are listed in WALK ORDER anyway, because the next person
   to read this wants the route, not the mesh's face numbering.

   THE RAMP Y VALUES COME FROM THE VISUAL MESH, NOT FROM THE GENERATOR'S FLOOR
   LIST. That list prints a ramp's AVERAGE ("FLOOR 3: y=360" for a surface that
   really runs 0 to 719), which is exactly the trap tools/ADDING_A_ROOM.txt STEP
   4b warns about. The pairs below are the face's own extents:
     ramp one  z 1400 -> 4192,  y    0 -> 719   (falling north)
     ramp two  z 4192 -> 2102,  y  722 -> 1240  (falling back south)
   ramp_y_start is the height AT ramp_axis_start, whatever order those are in —
   see apply_height() in collision.c, which simply lerps between them. */
static void catacombs_entry_floor_zones_init(void) {
    int i = 0;

    /* The entry chamber, at the tablet. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -750; floor_zones[i].max_x = 750;
    floor_zones[i].min_z =    0; floor_zones[i].max_z = 1400;
    floor_zones[i].y     = 0;
    i++;

    /* Ramp one: the corridor north, falling 719 over 2792. */
    floor_zones[i].type            = FLOOR_RAMP;
    floor_zones[i].min_x = -321; floor_zones[i].max_x = 321;
    floor_zones[i].min_z = 1400; floor_zones[i].max_z = 4192;
    floor_zones[i].ramp_y_start    = 0;      /* y at z=1400 (the chamber end) */
    floor_zones[i].ramp_y_end      = 719;    /* y at z=4192 (the landing end) */
    floor_zones[i].ramp_axis_start = 1400;
    floor_zones[i].ramp_axis_end   = 4192;
    floor_zones[i].ramp_along_x    = 0;
    i++;

    /* The north landing, where the corridor turns east. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -321; floor_zones[i].max_x = 1621;
    floor_zones[i].min_z = 4192; floor_zones[i].max_z = 4792;
    floor_zones[i].y     = 719;
    i++;

    /* Ramp two: back south and down again, 518 over 2090. */
    floor_zones[i].type            = FLOOR_RAMP;
    floor_zones[i].min_x =  971; floor_zones[i].max_x = 1621;
    floor_zones[i].min_z = 2102; floor_zones[i].max_z = 4192;
    floor_zones[i].ramp_y_start    = 1240;   /* y at z=2102 (the bottom) */
    floor_zones[i].ramp_y_end      = 722;    /* y at z=4192 (the landing) */
    floor_zones[i].ramp_axis_start = 2102;
    floor_zones[i].ramp_axis_end   = 4192;
    floor_zones[i].ramp_along_x    = 0;
    i++;

    /* The lower approach, and the short connector east out of it. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x =  971; floor_zones[i].max_x = 1621;
    floor_zones[i].min_z = 1102; floor_zones[i].max_z = 2102;
    floor_zones[i].y     = 1240;
    i++;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = 1621; floor_zones[i].max_x = 1800;
    floor_zones[i].min_z = 1102; floor_zones[i].max_z = 1702;
    floor_zones[i].y     = 1240;
    i++;

    /* The hall with the burial niches. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = 1800; floor_zones[i].max_x = 4800;
    floor_zones[i].min_z =  502; floor_zones[i].max_z = 2302;
    floor_zones[i].y     = 1240;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   Four, and the room owns all four — see the header for why it cannot borrow.

     0 cobblestones        the floors, walls and vaulting  (clsd_drwr page, x384 y0)
     1 lamashtu tablet     the south wall of the chamber   (brick_wall page, x768 y0)
     2 catacomb inner door the door at the hall's east end (opn_drwr page,  x832 y0)
     3 loculus             the burial niches               (kchn_wl page,   x512 y0)

   All four sit at Voff 0, so the one 128 texture window set in
   catacombs_entry_draw serves them all.

   ALL FOUR ARE IN TEXBANK_CATACOMBS AND NOTHING ELSE, which is what makes this
   chapter cost the heap ZERO while the player is anywhere in the mansion or the
   garden: a registration reads its TIM HEADER at startup and its PIXELS only
   when a bank containing it is selected (src/texmgr.h). Adding a texture to
   this chapter is therefore free in every area but this one.

   SLOT 1 IS A SECOND REGISTRATION OF \TEX\LMSHTBLT.TIM, the file the Outside
   Catacombs also registers, and the duplicate is deliberate. That room's copy
   is in TEXBANK_GARDEN and this one is in TEXBANK_CATACOMBS, so only ever one
   of the two holds pixels; tools/heap_budget.py lists it as a duplicate and it
   is not costing anything. Sharing one entry would mean one entry in two banks,
   which works — but it would also mean this room's art was not all in one
   bank, and the value of that is being able to read the mask and know. */
#define CATACOMBS_ENTRY_TEX_COUNT 4

static uint16_t tex_tpage[CATACOMBS_ENTRY_TEX_COUNT];
static uint16_t tex_clut[CATACOMBS_ENTRY_TEX_COUNT];

#define CATACOMBS_ENTRY_NEW_TEX 4
static int new_tex_id[CATACOMBS_ENTRY_NEW_TEX];
static const char *new_tex_file[CATACOMBS_ENTRY_NEW_TEX] = {
    "\\TEXCTCMB\\COBBLE.TIM;1",    /* slot 0 */
    "\\TEX\\LMSHTBLT.TIM;1",       /* slot 1 — see the duplicate note above */
    "\\TEXCTCMB\\CTCMBDR.TIM;1",   /* slot 2 */
    "\\TEXCTCMB\\LOCULUS.TIM;1",   /* slot 3 */
};

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   This room was written in September 2026 and inherited none of the reject-path
   work the garden rooms have had since August — the same three-item list
   Reception (STEP 3D) and the Master Bedroom (STEP 3E) both came in with. Its
   draw loop read the primitive header for its stride, then three vertex
   INDICES, then chased three scattered reads into the 8.7 KB vertex array, for
   all 1073 primitives, every frame, BEFORE either cull had run. The R3000 has
   no data cache behind any of that.

   THE COUNT, offline against assets/catacombs_entry.smd over the seven floor
   zones at 200-unit spacing and 16 headings (6272 poses; the tool is twenty
   lines — write one, do not estimate):

       1073  primitives walked, every frame, from anywhere in the room
        561  mean surviving the distance cull, i.e. ~512 rejected
        772  WORST, in the connector at (1621,1102) looking +Z down the hall
        398  mean reaching the GTE after the "behind me" test

   So about half the mesh is pure overhead on an average frame, and every one of
   those was paying four scattered reads to answer a question about two int16s.
   A rejected primitive is now ONE sequential 6-byte read out of the shared
   arena and never addresses the mesh. Identical output: this changes what the
   reject path READS, not what it decides.

   THE WORST STANCES ARE THE THREE THE PLAYER WALKS THROUGH BACK TO BACK — the
   lower approach, the connector and the west end of the hall all count 700+,
   because the 3200 view distance set by the hall's long sight line reaches back
   up both ramps from there. That is the stretch to stand in with the meter up.

   NO BOX KEY, for Reception's and the Bedroom's reason (src/reception.c):
   cull_boxes pays for itself where a SIDE-PLANE frustum test would otherwise
   chase v1..v3 per surviving primitive, and this room has no such test to feed.
   One WAS counted while the numbers above were being taken — a side-plane test
   would cut the mean reaching the GTE from 398 to 121 — and it is still not
   going in, because that is exactly the shape of reasoning that produced wrong
   turn #2. The Greenhouse measured a side-plane cull four hblanks on the WRONG
   side of neutral and Maze One measured it exactly neutral. Levels 4/6/7/8 now
   exist in this room; if a meter says this corridor is different from those two,
   the 398-to-121 count above is the case for adding one, with a box key under
   it and an offline hole sweep beside it. */
static int ce_key_count = 0;

static void ce_build_cull_keys(void) {
    ce_key_count = 0;
    if (!catacombs_entry_smd) return;
    uint8_t *p = (uint8_t *)catacombs_entry_smd->p_prims;
    int i, n = catacombs_entry_smd->n_prims;
    if (n > CATACOMBS_ENTRY_PRIM_COUNT) n = CATACOMBS_ENTRY_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &catacombs_entry_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    ce_key_count = n;
}

void catacombs_entry_load_geometry(void) {
    catacombs_entry_buff = room_arena_load("\\TEXCTCMB\\CTCMBENT.SMD;1");
    catacombs_entry_smd  = catacombs_entry_buff
                           ? smdInitData(catacombs_entry_buff) : NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    ce_build_cull_keys();
}

/* STARTUP, and it does NOT touch the drive. Four deferred registrations and
   four compile-time headers; the reads happen at the chapter door. */
void catacombs_entry_load_assets(void) {
    /* BANK: Chapter 3. Derived, not guessed - py tools/check_tex_banks.py
       walks the uploader call graph and fails the build if this is short. */
    texmgr_set_bank(TEXBANK_CATACOMBS);
    for (int i = 0; i < CATACOMBS_ENTRY_NEW_TEX; i++)
        new_tex_id[i] = texmgr_register(new_tex_file[i]);

    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, LMSHTBLT);
    TIM_SLOT(2, CTCMBDR);
    TIM_SLOT(3, LOCULUS);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   NO ORDERING RULE HERE, unlike every garden room: nothing else uploads to
   these four pages while this chapter is resident, because nothing else in the
   game is reachable. If a texmgr entry is somehow not loaded, texmgr_upload is
   a no-op and the slot keeps whatever the previous room left in it — visibly
   wrong, and quietly, which is why area_bank_sync runs before this and not
   after. */
void catacombs_entry_upload_textures(void) {
    for (int i = 0; i < CATACOMBS_ENTRY_NEW_TEX; i++)
        texmgr_upload(new_tex_id[i]);
}

/* ---- THE TABLET, AND WHAT IT SAYS ------------------------------------------
   The lamashtu tablet across the entry chamber's south wall: the inside face of
   the doors the player walked between, and the only thing in this room that
   answers a button. It is not a door and never becomes one — the transition in
   was one-way (src/catacomb_walk.h) — so it offers EXAMINE rather than ENTER,
   which is the whole message: the player is meant to walk up to the way they
   came in, press the button they have pressed at every other door in the game,
   and be told what has happened to them.

   The wall is at z=0 and collision wall 2 runs across it with nz = +4096, so
   the walkable side is +Z and the player reads the sign looking -Z. A sign in
   the XY plane approached from +Z is mirror=1 — the pairing the Outside
   Catacombs' own south gate spells out — and it floats 11 units proud of the
   wall on the player's side. */
#define CE_TABLET_X              0      /* the art spans x[-536,536] */
#define CE_TABLET_Z              0
#define CE_TABLET_TEXT_Y     (-186)     /* eye level on the y=0 chamber floor */
#define CE_TEXT_RADIUS        1200
#define CE_FADE_NEAR           800
#define CE_TRIGGER_RADIUS      500

/* ---- THE INNER DOOR, AND WHAT IS BEHIND IT ---------------------------------
   The far end of the lower hall: x=4800, z[1302,1502], y[840,1240]. A door in
   the YZ plane at fixed X, approached from -X, so TEXT_PLANE_YZ with mirror=1
   and the sign 11 units back along -X.

   >>> WHAT IS BEHIND IT IS NOT BUILT. <<< Circle posts "COMING SOON" to the log
   and nothing else happens. That is a placeholder and it is meant to read as
   one to the developer and as a locked door to the player — the same
   placeholder the catacomb MOUTH carried until this chapter existed, and
   replacing it will be the same one line. */
#define CE_INNER_X            4800
#define CE_INNER_Z            1402     /* (1302 + 1502) / 2 */
#define CE_INNER_TEXT_Y       1054     /* eye level on the y=1240 hall floor */

/* Circle edge-detect for the two of them. Seeded "held" by the arm below so a
   press carried in through the transition cannot fire on the arrival frame. */
static int tablet_circle_prev = 1;
static int inner_circle_prev  = 1;

static int circle_held(void) {
    return interact_tapped();
}

static void catacombs_entry_arm(void) {
    tablet_circle_prev = circle_held();
    inner_circle_prev  = circle_held();
}

/* The shared body of both interactions: edge-detect, range, facing, message.
   THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across
   a menu closing does not read as a fresh press on the frame the lock lifts —
   hatch_puzzle_update()'s rule, for its reason. */
static int ce_examine(int lock, int *prev, int32_t wx, int32_t wz,
                      const char *msg) {
    int held = circle_held();
    int just = held && !*prev;
    *prev = held;
    if (lock || !just) return 0;

    int32_t dx = cam_x - wx;
    int32_t dz = cam_z - wz;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= CE_TRIGGER_RADIUS) return 0;
    if (!interact_facing(wx, wz)) return 0;

    show_pickup_msg_raw(msg);
    return 1;
}

int catacombs_entry_interact_update(int lock) {
    /* The tablet first, because it is the one the player meets first. Both are
       run every frame whatever the other returns: the edge state has to stay
       current on the one that did not fire, or a press near the tablet would
       leave the inner door's `prev` stale and the next tap down the hall would
       not register. */
    int took = ce_examine(lock, &tablet_circle_prev,
                          CE_TABLET_X, CE_TABLET_Z,
                          "There is no way back now...");
    int took2 = ce_examine(took ? 1 : lock, &inner_circle_prev,
                           CE_INNER_X, CE_INNER_Z,
                           "COMING SOON");
    return took || took2;
}

/* Their floating signs. Same shape as every other sign in the game: opaque
   within CE_FADE_NEAR, gone by CE_TEXT_RADIUS. */
static void ce_sign(RenderContext *ctx, int32_t wx, int32_t wy, int32_t wz,
                    int32_t reading_axis_origin, int plane, int mirror) {
    int32_t dx = cam_x - wx;
    int32_t dz = cam_z - wz;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= CE_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > CE_FADE_NEAR) {
        int range = CE_TEXT_RADIUS - CE_FADE_NEAR;
        int prog  = xz - CE_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* door_draw_string_3d adds 200 to the reading axis before centring, hence
       the -200 the callers have already applied to reading_axis_origin. */
    if (plane == TEXT_PLANE_XY)
        door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to examine",
                            reading_axis_origin, wy, wz,
                            50, 255, 50, fade, mirror, TEXT_PLANE_XY,
                            DOOR_PIXEL_SIZE);
    else
        door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to examine",
                            wx, wy, reading_axis_origin,
                            50, 255, 50, fade, mirror, TEXT_PLANE_YZ,
                            DOOR_PIXEL_SIZE);
}

void catacombs_entry_spawn_south(void) {
    cam_x   = CE_TABLET_X;
    cam_y   = CE_EYE_Y;
    cam_vy  = 0;
    /* Clear of the wall push radius so the player is not shoved on their first
       frame, and facing +Z — the direction of travel through the doors, looking
       up the chamber at the opening in the north wall. */
    cam_z   = CE_TABLET_Z + CE_WALL_RADIUS + 25;
    cam_rot = 0;
    catacombs_entry_arm();
}

void catacombs_entry_init(void) {
    catacombs_entry_collision_init(&current_collision_room);
    /* The ENTRY CHAMBER's ceiling, which is the drawn one the player stands
       under on arrival — the lower hall's is at y=440, 800 above its own floor.
       One value is all collision_set_ceiling_y takes, and nothing in this room
       hangs from a ceiling yet, so the tallest is the honest answer. Read off
       the VISUAL mesh, not the collision proxy: -1100 is where the vaulting is
       drawn. */
    collision_set_ceiling_y(-1100);
    collision_set_wall_radius(CE_WALL_RADIUS);

    catacombs_entry_floor_zones_init();

    catacombs_entry_spawn_south();

    /* Save points and dresser props are global (not room-swapped) and neither
       is area-gated in its collide routine, so an instance left over from the
       mansion would block the player invisibly anywhere its coordinates fall
       inside this room's bounds — and this room spans x[-750,4800] z[0,4792].
       Clearing is safe: each is re-placed by its own room's init.

       No save point of this room's OWN yet. The prop and its art deliberately
       survive the chapter purge (src/area_bank.h) so Chapter 3 can have them the
       moment a room wants one. */
    save_points_clear();
    dressers_clear();

    /* Resolve the view distance with no ease: the first frame in the room shows
       whatever the player walked in holding, rather than easing out from the
       unlit distance. */
    ce_view_resolve(1);
}

/* ---- The mesh --------------------------------------------------------------
   The standard room draw loop, copied verbatim from src/outside_catacombs.c
   apart from the identifiers and the two things that room has and this one does
   not: the lit-backing gouraud branch, and the entity draws. Do not redesign
   it — the culling, the flat-poly OT sorting, the fog maths and the packet
   overflow guards are all load-bearing (tools/ADDING_A_ROOM.txt STEP 1). */
static void draw_catacombs_entry_smd(RenderContext *ctx) {
    if (!catacombs_entry_smd) return;

    uint8_t *p = (uint8_t *)catacombs_entry_smd->p_prims;
    int i, n = ce_key_count;

    /* HOISTED OUT OF THE LOOP, all four. None of them can change while a frame
       is being queued, and all four were being recomputed inside the hottest
       loop in the room: the two trig lookups once EACH for every primitive that
       passed the distance cull — up to 772 pairs of SDK calls a frame from the
       connector, which is one of the stances the lag was reported from — the
       cull distance all 1073 times, and buf_end (a double indirection through
       ctx->active_buffer, which a draw cannot change) for every primitive that
       got as far as being queued. STEP 3C fix 1 in
       tools/DIAGNOSING_FRAME_RATE.txt; this room predates none of that work, it
       simply never received it. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = ce_fog_far;   /* resolved by ce_view_resolve() this frame */
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < n; i++) {
        /* >>> THE REJECT PATH READS cull_keys, NOT THE MESH. <<< Six sequential
           bytes carry this primitive's first vertex X/Z and its stride, which is
           everything both cheap tests below need AND everything the walk needs
           to advance — so a rejected primitive never touches the SMD header, the
           vertex index array or the vertex array. See ce_build_cull_keys. */
        uint8_t stride = cull_keys[i].stride;
        {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > cull)
                { p += stride; continue; }
            if (dx * sn + dz * cs < -(700 << 12))
                { p += stride; continue; }
        }

        /* SURVIVED BOTH CULLS: only now is the header read and the vertex array
           addressed. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &catacombs_entry_smd->p_verts[vi[0]];
        SVECTOR *v1 = &catacombs_entry_smd->p_verts[vi[1]];
        SVECTOR *v2 = &catacombs_entry_smd->p_verts[vi[2]];

        DVECTOR sv[4];
        int32_t sz[4];
        int32_t otz, nclip;

        gte_ldv3(v0, v1, v2);
        gte_rtpt();
        gte_stsxy3c(sv);

        if (sv[0].vx <= -1023 || sv[0].vx >= 1023 || sv[0].vy <= -1023 || sv[0].vy >= 1023 ||
            sv[1].vx <= -1023 || sv[1].vx >= 1023 || sv[1].vy <= -1023 || sv[1].vy >= 1023 ||
            sv[2].vx <= -1023 || sv[2].vx >= 1023 || sv[2].vy <= -1023 || sv[2].vy >= 1023) {
            p += stride; continue;
        }

        int nocull = (i < CATACOMBS_ENTRY_PRIM_COUNT) && catacombs_entry_nocull[i];
        if (!pt->nocull && !nocull) {
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }

        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        SVECTOR *v3    = 0;
        int32_t  v2_sz = sz[3];   /* v2's SZ, before the quad path reuses sz[3] */
        if (is_quad) {
            v3 = &catacombs_entry_smd->p_verts[vi[3]];
            gte_ldv0(v3);
            gte_rtps();
            gte_stsxy(&sv[3]);
            gte_stsz(&sz[3]);
            if (sv[3].vx <= -1023 || sv[3].vx >= 1023 || sv[3].vy <= -1023 || sv[3].vy >= 1023) { p += stride; continue; }
            if (sz[3] == 0) { p += stride; continue; }
            gte_avsz4();
        } else {
            gte_avsz3();
        }

        gte_stotz(&otz);
        /* Horizontal polys sort by their farthest corner, not their average, so
           floors stay behind whatever stands on them (see render.h). */
        if (poly_is_flat_y(v0, v1, v2, v3))
            otz = is_quad ? otz_far4(sz[1], sz[2], v2_sz, sz[3])
                          : otz_far3(sz[1], sz[2], sz[3]);
        if (otz <= 0) { p += stride; continue; }
        otz += 40;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog = dist < ce_fog_near ? ce_fog_near : (dist > ce_fog_far ? ce_fog_far : dist);
        int32_t fog_factor = ((ce_fog_far - fog) << 8) / (ce_fog_far - ce_fog_near);

        uint8_t tex_idx = (i < CATACOMBS_ENTRY_PRIM_COUNT) ? catacombs_entry_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < CATACOMBS_ENTRY_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + CE_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + CE_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + CE_FOG_B * (256 - fog_factor)) >> 8);

        if (is_quad && textured) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            uint8_t *uv = p + 20;
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, r, g, b);
            poly->tpage = tex_tpage[tex_idx];
            poly->clut  = tex_clut[tex_idx];
            poly->u0=uv[0]; poly->v0=uv[1];
            poly->u1=uv[2]; poly->v1=uv[3];
            poly->u2=uv[4]; poly->v2=uv[5];
            poly->u3=uv[6]; poly->v3=uv[7];
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_FT4);
        } else if (is_quad) {
            if (ctx->next_packet + sizeof(POLY_F4) > buf_end) { p += stride; continue; }
            POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
            setPolyF4(poly);
            setRGB0(poly, r, g, b);
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F4);
        } else if (textured) {
            if (ctx->next_packet + sizeof(POLY_FT3) > buf_end) { p += stride; continue; }
            uint8_t *uv = p + 20;
            POLY_FT3 *poly = (POLY_FT3 *)ctx->next_packet;
            setPolyFT3(poly);
            setRGB0(poly, r, g, b);
            poly->tpage = tex_tpage[tex_idx];
            poly->clut  = tex_clut[tex_idx];
            poly->u0=uv[0]; poly->v0=uv[1];
            poly->u1=uv[2]; poly->v1=uv[3];
            poly->u2=uv[4]; poly->v2=uv[5];
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_FT3);
        } else {
            if (ctx->next_packet + sizeof(POLY_F3) > buf_end) { p += stride; continue; }
            POLY_F3 *poly = (POLY_F3 *)ctx->next_packet;
            setPolyF3(poly);
            setRGB0(poly, r, g, b);
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F3);
        }

        p += stride;
    }
}

void catacombs_entry_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it: the mesh draw culls
       and fogs with ce_fog_*, and g_fog_* below hands the same pair to
       everything else. */
    ce_view_resolve(0);

    /* Anything else that fogs in this room follows the debug view distance when
       one is selected, so levels 6/7 change what the room LOOKS like
       consistently rather than only where the mesh stops. */
    g_fog_near = ce_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : ce_fog_far;

    /* Background in the SAME colour the fog saturates to, so a poly that has
       faded out is indistinguishable from the void behind it and the cull never
       shows a seam — but as the CLEAR COLOUR, not as a primitive. The draw
       environments carry isbg=1, so DrawOTagEnv has already filled the whole
       framebuffer before the first poly is drawn; the full-screen TILE this room
       used to queue on top was a SECOND 77,000-pixel fill every frame, for the
       colour alone. Wrong turn #3 in tools/DIAGNOSING_FRAME_RATE.txt. */
    render_set_clear_colour(ctx, CE_FOG_R, CE_FOG_G, CE_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All four of this room's textures sit at page-top (Voff 0), so one window
       serves them (tools/VRAM_MAP.txt). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    MATRIX rot_matrix;
    camera_build_view(&rot_matrix);
    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    if (exp != DBG_EXP_NO_MESH) draw_catacombs_entry_smd(ctx);

    /* No entity draws: nothing from Chapters 1 or 2 can be placed down here
       (src/area_bank.h has freed their art) and Chapter 3 has no monsters yet.
       When it does, this is where they go — and they will need the 128 window
       above handing to them if their sprites sit at Voff >= 128. */

    /* The two signs, last.

       >>> LEVEL 8 REMOVES THE SIGNS. <<< In most rooms that level takes out the
       monsters and the props, because that is what stands in them; this room has
       neither, and the only thing standing in its mesh is the SIGNAGE. STEP 3D
       is the reason it is worth a switch at all — Reception's frame turned out
       to be the text rather than the room — though the two here are 3000 units
       and two ramps apart and CE_TEXT_RADIUS is 1200, so unlike Reception's west
       wall they can never both be live at once. D read at 1, at 4 and at 8 now
       splits this room's frame three ways in one sitting. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        ce_sign(ctx, CE_TABLET_X, CE_TABLET_TEXT_Y, CE_TABLET_Z + 11,
                CE_TABLET_X - 200, TEXT_PLANE_XY, 1);
        ce_sign(ctx, CE_INNER_X - 11, CE_INNER_TEXT_Y, CE_INNER_Z,
                CE_INNER_Z - 200, TEXT_PLANE_YZ, 1);
    }
}
