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
#include "incinerator_room.h"
#include "collision.h"
#include "incinerator_room_mesh_collision.h"
#include "incinerator_room_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "incinerator.h"       /* the machine this room is named after */
#include "incinerator_panel.h"  /* ...and the board over its conveyor    */
#include "player.h"             /* current_weapon, player_weapons */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* Incinerator Room — see incinerator_room.h for the layout and the door list. */

static SMD  *incinerator_room_smd  = NULL;
static void *incinerator_room_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY, unchanged: cull and fog-far are equal (so nothing is
   dropped until the fog has already faded it into the background), the base is
   scaled by what the player is carrying, and the scale eases rather than jumping.

   >>> THE NUMBERS ARE THE CATACOMBS ENTRY'S, NOT THE UP DOWN MAZE'S, and the
   room it sits next door to is the wrong place to take them from. <<< The maze
   pulled its base in to 1300 for a reason that is entirely about being a maze:
   blocks on a 600 grid, two cells of corridor visible and no further, because a
   longer sight line would hand the player a plan of the room from the doorway.
   Nothing here is hidden by a corner. This is one open hall 4800 by 3600 with an
   alcove off it, the same kind of space as the burial hall, so it gets that
   room's 450/1600 — far enough to read the hall as a room and nowhere near far
   enough to see the end of it.

   THE HELLUMINATOR IS THE WAY DOWN THE HALL. Raising the lantern and then
   burning it each add 50% of the base, so the three distances are 1600, 2400 and
   3200 — and even lit that is 400 short of the far wall from the north door.
   That is the chapter's bargain and it is deliberately still not enough to light
   the whole room at once.

   AND IT IS ALSO THIS ROOM'S FRAME BUDGET. `cull` and fog-far being equal means
   these numbers decide how much MESH is walked, transformed and queued. The
   saving grace here is the mesh itself: 708 primitives against the Up Down
   Maze's 1990, so the longer sight line is being spent on about a third as much
   geometry. Anything that lengthens these has to be measured BURNING, not at
   rest (STEP 3J of tools/DIAGNOSING_FRAME_RATE.txt). */
#define INC_BASE_FOG_NEAR   450
#define INC_BASE_FOG_FAR   1600

#define INC_VIEW_UNIT        256
#define INC_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define INC_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */
#define INC_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t inc_view     = INC_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t inc_fog_near = INC_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t inc_fog_far  = INC_BASE_FOG_FAR;

static int32_t inc_view_target(void) {
    int32_t s = INC_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += INC_VIEW_HELL_BONUS;
        /* Only asked INSIDE the equipped test: helluminator_burning() cannot be
           true for an unequipped lantern, but nesting it says so rather than
           relying on the weapon layer to keep clearing it. */
        if (helluminator_burning()) s += INC_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry: there is no previous frame to ease from
   and walking in with the lantern already up must not open the room out over the
   first half second. */
static void inc_view_resolve(int snap) {
    int32_t target = inc_view_target();
    if (snap) {
        inc_view = target;
    } else if (inc_view < target) {
        inc_view += INC_VIEW_RATE;
        if (inc_view > target) inc_view = target;
    } else if (inc_view > target) {
        inc_view -= INC_VIEW_RATE;
        if (inc_view < target) inc_view = target;
    }
    inc_fog_near = (INC_BASE_FOG_NEAR * inc_view) >> 8;
    inc_fog_far  = (INC_BASE_FOG_FAR  * inc_view) >> 8;
}

/* Underground, so the same near-black-with-a-cold-lift the rest of the chapter
   uses, and for the same reason (src/catacombs_entry.c): a fog that saturates to
   a true 0,0,0 makes the cull line invisible, which sounds ideal and is how you
   lose an hour to "the end of the corridor is missing". */
#define INC_FOG_R             7
#define INC_FOG_G             6
#define INC_FOG_B             9

/* Wall standoff. The chapter's 195, and here it is simply the default rather
   than a decision: the narrowest thing the player has to get through is the
   alcove mouth at x[600,1000], 400 wide, which leaves 10 units of walking room —
   tight, but it is a niche to look into and not a corridor to pass down. The
   garden rooms' 260 would seal it completely. */
#define INC_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. Used for the spawn only — apply_height
   settles cam_y every frame afterwards. */
#define INC_FLOOR_Y            0
#define INC_EYE_Y           (INC_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   TWO, BOTH FLAT AND BOTH AT y=0: the hall and the alcove off its north-east
   corner, exactly the two planes the collision generator found.

   >>> THE ORDER IS FREE HERE, and that is worth stating because next door it is
   the whole mechanism. <<< The Up Down Maze stacks ten walkways over one
   catch-all and apply_height() takes the first zone that is not above the
   player, so listing them the other way round makes its upper storey
   unstandable. These two are DISJOINT IN PLAN — they meet on the single line
   x=600, z[-1800,-600] — so no player is ever inside both and the walk order
   cannot decide anything. The delivery area's and the Catacombs Entry's zones
   are the same easy case.

   FLOOR_FLAT, not FLOOR_UPPER: nothing in this room sets player_on_upper_floor,
   because there is no upper floor to be on. */
static void incinerator_room_floor_zones_init(void) {
    int i = 0;

    /* The long hall. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -4199; floor_zones[i].max_x =  600;
    floor_zones[i].min_z = -3599; floor_zones[i].max_z =    0;
    floor_zones[i].y     = INC_FLOOR_Y;
    i++;

    /* The alcove off its north-east corner. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x =   600; floor_zones[i].max_x = 1000;
    floor_zones[i].min_z = -1800; floor_zones[i].max_z = -600;
    floor_zones[i].y     = INC_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   TWO, AND THE ROOM OWNS NEITHER.

     0 cobblestones        the hall, the alcove, the floor and the vaulting
                           (clsd_drwr page, x384 y0)
     1 catacomb inner door the two doorways in the outer walls
                           (opn_drwr page, x832 y0)

   Both are registered by src/catacombs_entry.c, in TEXBANK_CATACOMBS, and both
   sit at Voff 0, so the one 128 texture window set in incinerator_room_draw
   serves them. This room therefore spends NOTHING: no texmgr registration, no
   permanent RAM, no VRAM page of its own. What it has instead is a call to each
   of that module's two NARROW uploaders — the conservatory_upload_con_tile
   pattern — and the narrow form matters for the usual reason
   (tools/ADDING_A_ROOM.txt STEP 3b): the Catacombs Entry's FULL uploader would
   also stamp the lamashtu tablet, the loculus, the sconce and the oil dispenser,
   none of which this room draws.

   >>> AND BORROWING IS NOT MERELY THRIFTY HERE, IT IS THE ONLY THING THAT WORKS.
   <<< Two registrations of the same two files would be two RAM copies of art the
   chapter already holds, at the same two VRAM addresses, and which pixels were up
   would then depend on whichever room's uploader ran last. One entry, one copy,
   one address. */
#define INCINERATOR_ROOM_TEX_COUNT 2

static uint16_t tex_tpage[INCINERATOR_ROOM_TEX_COUNT];
static uint16_t tex_clut[INCINERATOR_ROOM_TEX_COUNT];

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/up_down_maze.c, which took it from src/catacombs_entry.c,
   which has been through that document. This is the SMALLEST of the three
   Chapter 3 meshes — 708 primitives against 1073 and 1990 — and the key table
   is copied anyway, because the reject path costing nothing is what lets the
   room afford the longer sight line above.

   The keys go in the SHARED arena (src/cull_arena.h) and are built on the same
   call that reloads the mesh they describe, which is the one rule that file has.
   A rejected primitive is then ONE sequential 6-byte read and never addresses the
   SMD header, the index array or the vertex array.

   NO BOX KEY AND NO SIDE-PLANE CULL, for the reason the Up Down Maze, the
   Catacombs Entry, the Greenhouse, Maze One, Reception and the Master Bedroom
   all record: cull_boxes pays for itself only where a side-plane test would
   otherwise chase v1..v3 per surviving primitive, and there is no such test here
   to feed. "The room is open so it would cull a lot" is wrong turn #2 in
   DIAGNOSING_FRAME_RATE.txt and it has now been the tempting wrong answer six
   times. If a METER says this room is different, that is the case for adding one
   — with a box key under it and an offline hole sweep beside it. */
static int inc_key_count = 0;

static void inc_build_cull_keys(void) {
    inc_key_count = 0;
    if (!incinerator_room_smd) return;
    uint8_t *p = (uint8_t *)incinerator_room_smd->p_prims;
    int i, n = incinerator_room_smd->n_prims;
    if (n > INCINERATOR_ROOM_PRIM_COUNT) n = INCINERATOR_ROOM_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &incinerator_room_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    inc_key_count = n;
}

void incinerator_room_load_geometry(void) {
    incinerator_room_buff = room_arena_load("\\TEXCTCMB\\INCINRTR.SMD;1");
    incinerator_room_smd  = incinerator_room_buff
                            ? smdInitData(incinerator_room_buff) : NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    inc_build_cull_keys();
}

/* STARTUP, and it does nothing at all at runtime: two compile-time headers, not
   one registration and not one CD read. The registrations this room draws
   through belong to src/catacombs_entry.c and are already deferred into
   TEXBANK_CATACOMBS.

   NO texmgr_set_bank() HERE, and that is correct rather than an omission: that
   call tags the registrations a module MAKES, and this module makes none. Its
   place in the bank graph is established by the two uploader calls below, which
   py tools/check_tex_banks.py walks. */
void incinerator_room_load_assets(void) {
    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, CTCMBDR);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   NO ORDERING RULE, for the Catacombs Entry's reason: nothing else uploads to
   these two pages while this chapter is resident, because nothing else in the
   game is reachable. If a texmgr entry is somehow not loaded, texmgr_upload is a
   no-op and the slot keeps whatever the previous room left in it — visibly wrong,
   and quietly, which is why area_bank_sync runs before this and not after. */
void incinerator_room_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    /* ...and the Incinerator's own page, which unlike the two above this room
       DOES own: one narrow uploader, the oil dispenser's arrangement exactly
       (src/incinerator.h). It is the only texture registration anything in this
       room makes, and the only reason check_tex_banks.py reaches this module. */
    incinerator_upload_texture();
}

/* ---- THE NORTH DOOR --------------------------------------------------------
   z=0, x[200,400], y[-400,0] — in the outer north wall, and the only one of this
   room's two drawn doors that goes anywhere. Back up into the Up Down Maze's
   LOWER storey, through the door that room's header lists as "south, lower".

   A door in the XY plane at fixed Z, approached from -Z (wall 0 runs z=0 with
   nz = -4096, so the walkable side is -Z), so TEXT_PLANE_XY with mirror=0 and
   the sign 11 units proud of the wall along -Z.

   >>> AND NO STOREY TEST, WHICH IS THE NORMAL CASE AND NOT AN OVERSIGHT. <<<
   The far side of this doorway takes one, because it stands under the Up Down
   Maze's south walkway and a Manhattan-in-plan trigger would otherwise let a
   player open it from a storey away. This room is flat: the walkable surface is
   a function of XZ, which is the assumption every trigger in the engine makes,
   so the plain distance test is correct here. */
#define INC_NORTH_X            300     /* the art spans x[200,400] */
#define INC_NORTH_Z              0
#define INC_NORTH_TEXT_Y      (-186)   /* eye level on the y=0 floor */
#define INC_TEXT_RADIUS       1200
#define INC_FADE_NEAR          800
#define INC_TRIGGER_RADIUS     500

/* ---- THE INCINERATOR'S BUTTON ----------------------------------------------
   The panel on the machine's NORTH face, and the only one of its two
   interactions that lives in this file. Its coordinates are MEASURED off
   assets/props/Incinerator.smx rather than chosen: exactly one face in that
   model wears the button patch of the texture (u[3,42] v[95,123], the red
   button and the strip beside it in the bottom-left of incinerator.png), and it
   spans x[-500,-400], y[-300,-200] at z=-2550. So the point below is that
   face's centre, and re-exporting the machine with the panel somewhere else is
   the one change that would need these three numbers looked at again.

   z=-2550 is the machine's north face and the hall runs north from there to
   z=0, so the player reads it looking -Z. A sign in the XY plane approached
   from +Z is mirror=1 — the pairing src/catacombs_entry.c's tablet spells out —
   and it floats 11 units proud on the player's side.

   FULL DOOR_PIXEL_SIZE, unlike the oil dispenser's half-size sign. That one was
   halved because a 432-unit line over a 40-wide tank was ten times the object
   it labelled; this machine is 2000 long, so the line is a fifth of it and sits
   on the face like a stencil. The two signs on this prop are 1550 apart and
   cannot collide at either size.

   >>> THE HEIGHTS MOVED WITH THE RE-EXPORT AND THE X/Z DID NOT. <<< The machine
   was shortened from 400 tall to 300, which scaled every Y on the model by
   three quarters and left the plan alone: the button panel was y[-300,-200] and
   is now y[-200,-100], so the sign came down 100 with it. Nothing else about
   this block changed, which is the check worth making after any re-export - if
   an X or a Z moves too, the model has been repositioned and not just resized,
   and the collision box (measured at load) will have followed while these
   constants will not have.

   >>> THE CONVEYOR IS NOT HERE. <<< It is a camera-locked board and lives in
   src/incinerator_panel.c, which owns its own trigger, its own radius and its
   own edge state. What is here is the walk-up press, which is an ordinary
   interaction like the door's. */
#define INC_BUTTON_X         (-450)    /* the panel spans x[-500,-400] */
#define INC_BUTTON_Z        (-2550)
#define INC_BUTTON_TEXT_Y    (-210)    /* glyph TOP, just above the panel */
/* >>> HOW FAR THE SIGN STANDS OFF THE FACE, AND 11 WAS NOT ENOUGH. <<< Every
   door sign in the game floats 11 units proud of its wall, which works because
   a door is a flat slab of one colour and the glyphs sit on it like paint. This
   face is not: it is the machine's busiest panel - the button, the strip beside
   it and the plating's tiling all inside the same 100 units - and at 11 the
   green ran into that detail and stopped being legible. 60 lifts it clear into
   the air in front of the machine, where it reads against the dark of the hall
   instead of against the artwork.

   IT COSTS NOTHING IN REACH. The sign is not the trigger; the press is tested
   against INC_BUTTON_X/Z, the face itself, so moving the text does not move the
   button. And 60 is still well inside the 195 the collision box holds the
   player off by, so the line can never end up behind them. */
#define INC_BUTTON_TEXT_OUT     60     /* north of the face, along +Z */

/* The conveyor's sign, and ONLY its sign — the press belongs to the panel
   module. The tray's outer face is at x=-2000 over z[-2950,-2750], approached
   from -X, so TEXT_PLANE_YZ with mirror=1 and the line 11 units back along -X:
   src/catacombs_entry.c's inner door is the same pairing. The reading axis for
   a YZ sign is Z, hence the -200 on that coordinate and not on X. */
#define INC_CONV_X          (-2000)
#define INC_CONV_Z          (-2850)    /* (-2950 + -2750) / 2 */
#define INC_CONV_TEXT_Y      (-200)    /* glyph TOP, above the tray surface;
                                          down 100 with the shortened machine,
                                          the tray having gone y[-250,-150] ->
                                          y[-150,-50]. */

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int north_circle_prev = 1;
static int button_circle_prev = 1;

/* What the running machine will say when it stops. Latched on the press and
   read ~8 seconds later, because incinerator.h puts the log line at the END of
   the three grinds — so the branch has to be remembered across them. INT rather
   than IncPress only so "nothing pending" has a value that is not one of the
   three. */
static int button_pending = -1;

static int circle_held(void) {
    return interact_tapped();
}

void incinerator_room_arm(void) {
    north_circle_prev  = circle_held();
    button_circle_prev = circle_held();
    /* The conveyor board keeps its own edge state and its own trigger, so it is
       armed rather than reached into — the stove's arrangement with the
       kitchen. It does NOT clear the hopper; see incinerator_panel_arm(). */
    incinerator_panel_arm();
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts —
   hatch_puzzle_update()'s rule, for its reason. */
int incinerator_room_north_door_triggered(int lock) {
    int held = circle_held();
    int just = held && !north_circle_prev;
    int32_t dx, dz, xz;
    north_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - INC_NORTH_X;
    dz = cam_z - INC_NORTH_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= INC_TRIGGER_RADIUS) return 0;
    if (!interact_facing(INC_NORTH_X, INC_NORTH_Z)) return 0;
    return 1;
}

/* ---- THE MACHINE, ONE FRAME ------------------------------------------------
   Called unconditionally from main.c with the usual `lock`, and it does two
   jobs that have to happen in this order.

   FIRST THE CYCLE, because it is running whether or not the player is pressing
   anything, and its line is owed from a press made up to eight seconds ago. The
   wording is the room's and the arithmetic is the prop's — the split
   src/catacombs_entry.c keeps with the oil dispenser.

   THEN THE PRESS. It runs through the same range/facing/edge test the door
   does, and its edge state is kept current EVEN WHILE LOCKED so a Circle held
   across a menu closing does not read as a fresh press on the frame the lock
   lifts (hatch_puzzle_update()'s rule, for its reason).

   >>> AND THE DOOR VETOES IT. <<< main.c passes `took` in as part of `lock`,
   so a frame that opened the north door cannot also work the machine. They are
   3900 apart and the two trigger circles are 500, so this can never actually
   fire — the veto is the pattern the Catacombs Entry's three interactions
   established, and it costs a branch. */
int incinerator_room_machine_update(int lock) {
    int held, just;
    int32_t dx, dz, xz;

    incinerator_cycle_update();
    if (incinerator_cycle_finished()) {
        /* The brief's two lines, and the ONLY place either is printed. */
        if (button_pending == INC_PRESS_BURNED) show_pickup_msg_raw("The item was destroyed!");
        else                                   show_pickup_msg_raw("It has no effect");
        button_pending = -1;
    }

    held = circle_held();
    just = held && !button_circle_prev;
    button_circle_prev = held;
    if (lock || !just) return 0;

    dx = cam_x - INC_BUTTON_X;
    dz = cam_z - INC_BUTTON_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= INC_TRIGGER_RADIUS) return 0;
    if (!interact_facing(INC_BUTTON_X, INC_BUTTON_Z)) return 0;

    /* >>> THE PRESS IS TESTED FIRST AND THE MACHINE IS STARTED SECOND. <<< It
       DESTROYS AN ITEM, so it must never be asked speculatively — the oil
       dispenser's rule, and the stakes here are higher than a hundred units of
       oil. A press landing while it is already grinding comes back IGNORED and
       changes nothing, so leaning on the button cannot burn two things. */
    switch (incinerator_button_press()) {
    case INC_PRESS_IGNORED:  return 0;   /* already running; not consumed */
    case INC_PRESS_EMPTY:    button_pending = INC_PRESS_EMPTY;  break;
    case INC_PRESS_BURNED:   button_pending = INC_PRESS_BURNED; break;
    }
    return 1;
}

/* The door's floating sign. Same shape as every other sign in the game: opaque
   within INC_FADE_NEAR, gone by INC_TEXT_RADIUS. */
static void inc_north_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - INC_NORTH_X;
    int32_t dz = cam_z - INC_NORTH_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= INC_TEXT_RADIUS) return;

    if (xz > INC_FADE_NEAR) {
        int range = INC_TEXT_RADIUS - INC_FADE_NEAR;
        int prog  = xz - INC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* door_draw_string_3d adds 200 to the reading axis before centring, hence
       the -200. mirror=0: an XY-plane door approached from -Z. */
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        INC_NORTH_X - 200, INC_NORTH_TEXT_Y, INC_NORTH_Z - 11,
                        50, 255, 50, fade, 0, TEXT_PLANE_XY,
                        DOOR_PIXEL_SIZE);
}

/* The two signs on the machine. Same fade curve as the door's — one helper for
   the range/fade half so the three can never drift onto different curves, which
   is the split src/catacombs_entry.c makes for the same three. */
static int inc_sign_fade(int32_t wx, int32_t wz) {
    int32_t dx = cam_x - wx;
    int32_t dz = cam_z - wz;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade;

    if (xz >= INC_TEXT_RADIUS) return 0;
    fade = 256;
    if (xz > INC_FADE_NEAR) {
        int range = INC_TEXT_RADIUS - INC_FADE_NEAR;
        int prog  = xz - INC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    return fade;
}

/* "OPERATE", not "INTERACT": the button does one fixed thing and says so, while
   the conveyor opens a board whose meaning depends on what is on it. The brief
   asks for exactly that pair of words and they carry the distinction. */
static void inc_button_text(RenderContext *ctx) {
    int fade = inc_sign_fade(INC_BUTTON_X, INC_BUTTON_Z);
    if (!fade) return;
    /* door_draw_string_3d adds 200 to the reading axis before centring, hence
       the -200. mirror=1: an XY-plane face approached from +Z. */
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to operate",
                        INC_BUTTON_X - 200, INC_BUTTON_TEXT_Y,
                        INC_BUTTON_Z + INC_BUTTON_TEXT_OUT,
                        50, 255, 50, fade, 1, TEXT_PLANE_XY,
                        DOOR_PIXEL_SIZE);
}

static void inc_conveyor_text(RenderContext *ctx) {
    int fade;
    /* >>> GONE WHILE THE MACHINE IS RUNNING. <<< The offer and the ability have
       to disappear together: src/incinerator_panel.c refuses the press for the
       same 336 frames, and a prompt that stayed up over a button that did
       nothing would read as a dropped input rather than as a machine that is
       busy. The BUTTON's sign is deliberately NOT hidden - it is what the
       player is being told is working. */
    if (incinerator_cycle_active()) return;
    fade = inc_sign_fade(INC_CONV_X, INC_CONV_Z);
    if (!fade) return;
    /* A YZ sign reads along Z, so the -200 goes on the Z argument and not on
       the X one. mirror=1: approached from -X. */
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to interact",
                        INC_CONV_X - 11, INC_CONV_TEXT_Y, INC_CONV_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void incinerator_room_spawn_north(void) {
    /* Clear of the wall push radius so the player is not shoved on their first
       frame, and facing -Z — the direction of travel through the door, looking
       south down the length of the hall. */
    cam_x   = INC_NORTH_X;
    cam_y   = INC_EYE_Y;
    cam_vy  = 0;
    cam_z   = INC_NORTH_Z - (INC_WALL_RADIUS + 25);
    cam_rot = 2048;
    incinerator_room_arm();
}

void incinerator_room_init(void) {
    incinerator_room_collision_init(&current_collision_room);
    /* The DRAWN ceiling, read off the VISUAL mesh and not only off the collision
       proxy: the vaulting over the whole room is at y=-800, which is also where
       the outer walls' proxy faces stop, so the two agree and the honest number
       is available. That is 800 above the floor — HALF the Up Down Maze's
       headroom, and the one dimension of this room that will surprise anyone
       coming from next door. Anything hung from it has to be authored against
       800, not 1800. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(INC_WALL_RADIUS);

    incinerator_room_floor_zones_init();

    incinerator_room_spawn_north();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every one of their collide routines
       — so an instance left over from another room would block the player
       invisibly anywhere its coordinates fall inside this room's bounds
       (x[-4199,1000] z[-3599,0]).

       Nothing in Chapter 3 actually lands in there today: the Catacombs Entry's
       save point is at (4606,2106), its two sconces at (±595,200) and its oil
       dispenser at (4798,503), all of them off the +Z or +X side of this room,
       and the sconce and dispenser instances are area-tagged besides. Clearing
       is the cheap guarantee that stays true when one of those moves, and it is
       safe: catacombs_entry_init() re-places all four on every entry to that
       room, and this room places none of them. (Mistake 5 in
       tools/ADDING_A_ROOM.txt.) */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    /* ---- THE INCINERATOR ---------------------------------------------------
       >>> PLACED AT THE ORIGIN, AND THAT IS THE WHOLE PLACEMENT. <<<
       assets/props/Incinerator.smx is authored in this room's own coordinates —
       its vertices ARE x[-2000,-1] y[-400,0] z[-3150,-2550] in the hall — so
       the position comes out of the file and there is no pair of #defines here
       to keep in step with Blender. src/incinerator.h sets out what that buys
       and what it costs (rot_y is meaningless for this prop, hence the 0).

       y is the floor reference the prop adds GROUND_FLOOR_Y back onto, so it is
       INC_FLOOR_Y less that constant — the oil dispenser's call next door reads
       `1240 - GROUND_FLOOR_Y` for the same reason. The model's own y=0 is this
       room's floor, so the machine lands sitting on it.

       CLEARED FIRST, like the four above: it is a singleton and a re-entry must
       not leave two claims on it. Clearing also STOPS a cycle left running by a
       button press the player walked out on (src/incinerator.c). */
    incinerator_clear();
    incinerator_place(STATE_INCINERATOR_ROOM, 0, INC_FLOOR_Y - GROUND_FLOOR_Y, 0, 0);
    button_pending = -1;

    /* Resolve the view distance with no ease: the first frame in the room shows
       whatever the player walked in holding. */
    inc_view_resolve(1);
}

/* ---- The mesh --------------------------------------------------------------
   The standard room draw loop, copied verbatim from src/up_down_maze.c apart
   from the identifiers — and that file took it verbatim from
   src/catacombs_entry.c, which has been through tools/DIAGNOSING_FRAME_RATE.txt.
   Like the Up Down Maze's it carries no render_light_dist() discount, because no
   light is placed here; put it back in BOTH places at once the day this room
   gets one — the cull's discount and the shading's must match, or a lit poly
   survives the cull and is then shaded as though it had not been, i.e. drawn in
   the clear colour, a hole.

   Do not redesign the rest: the culling, the flat-poly OT sorting, the fog maths
   and the packet-overflow guards are all load-bearing (STEP 1). */
static void draw_incinerator_room_smd(RenderContext *ctx) {
    if (!incinerator_room_smd) return;

    uint8_t *p = (uint8_t *)incinerator_room_smd->p_prims;
    int i, n = inc_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). None of them can change
       while a frame is being queued: the two trig lookups would otherwise be a
       pair of SDK calls for every primitive that passed the distance cull, the
       cull distance would be re-read all 708 times, and buf_end is a double
       indirection through ctx->active_buffer, which a draw cannot change. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = inc_fog_far;   /* resolved by inc_view_resolve() this frame */
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < n; i++) {
        /* >>> THE REJECT PATH READS cull_keys, NOT THE MESH. <<< Six sequential
           bytes carry this primitive's first vertex X/Z and its stride, which is
           everything both cheap tests below need AND everything the walk needs to
           advance — so a rejected primitive never touches the SMD header, the
           vertex index array or the vertex array. See inc_build_cull_keys. */
        uint8_t stride = cull_keys[i].stride;
        {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            int32_t cd = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            if (cd > cull)                        { p += stride; continue; }
            if (dx * sn + dz * cs < -(700 << 12)) { p += stride; continue; }
        }

        /* SURVIVED BOTH CULLS: only now is the header read and the vertex array
           addressed. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &incinerator_room_smd->p_verts[vi[0]];
        SVECTOR *v1 = &incinerator_room_smd->p_verts[vi[1]];
        SVECTOR *v2 = &incinerator_room_smd->p_verts[vi[2]];

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

        int nocull = (i < INCINERATOR_ROOM_PRIM_COUNT) && incinerator_room_nocull[i];
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
            v3 = &incinerator_room_smd->p_verts[vi[3]];
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
           floors stay behind whatever stands on them (see render.h). Here that is
           the hall's floor and its vault and nothing else: unlike the Up Down
           Maze next door, no surface in this room is a floor on one side and a
           ceiling on the other. */
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
        int32_t fog = dist < inc_fog_near ? inc_fog_near : (dist > inc_fog_far ? inc_fog_far : dist);
        int32_t fog_factor = ((inc_fog_far - fog) << 8) / (inc_fog_far - inc_fog_near);

        uint8_t tex_idx = (i < INCINERATOR_ROOM_PRIM_COUNT) ? incinerator_room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < INCINERATOR_ROOM_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + INC_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + INC_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + INC_FOG_B * (256 - fog_factor)) >> 8);

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

void incinerator_room_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    inc_view_resolve(0);

    /* Anything else that fogs in this room follows the debug view distance when
       one is selected, so levels 6/7 change what the room LOOKS like
       consistently rather than only where the mesh stops. */
    g_fog_near = inc_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : inc_fog_far;

    /* Background in the SAME colour the fog saturates to, as the CLEAR COLOUR
       and not as a primitive: the draw environments carry isbg=1, so DrawOTagEnv
       has already filled the whole framebuffer before the first poly is drawn,
       and a full-screen TILE on top would be a second 77,000-pixel fill every
       frame for the colour alone. Wrong turn #3 in
       tools/DIAGNOSING_FRAME_RATE.txt. */
    render_set_clear_colour(ctx, INC_FOG_R, INC_FOG_G, INC_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       Both of this room's textures sit at page-top (Voff 0), so one window serves
       them (tools/VRAM_MAP.txt). */
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

    if (exp != DBG_EXP_NO_MESH) draw_incinerator_room_smd(ctx);

    /* >>> LEVEL 8 IS ONE SIGN, AND POINTING IT THERE IS THE WHOLE POINT. <<<
       The room holds no props and no enemies yet, so the only thing queued
       outside the mesh is the north door's floating string — which is exactly
       the case STEP 3D of tools/DIAGNOSING_FRAME_RATE.txt was written about:
       Reception's frame turned out to be its SIGNAGE and not its mesh, because
       door_draw_string_3d has no facing test and queues every glyph in full with
       the player's back to it. Reading D at levels 1, 4 and 8 splits this room's
       frame between the mesh and that string. When something is finally placed
       in here, it goes inside this same test. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        /* The machine FIRST, before the three strings: it is geometry and sorts
           at real scene depth, and queueing it inside this same test is what the
           note above asked for when something was finally placed in here.

           It draws under the 128 texture window set at the top of this function,
           which it NEEDS — its UVs run past a tile (src/incinerator.c). */
        incinerator_draw(ctx);
        inc_north_door_text(ctx);
        inc_button_text(ctx);
        inc_conveyor_text(ctx);
    }

    /* The conveyor board LAST and OUTSIDE the entity test: it is a 2D overlay in
       the menu-reserved OT range, not something in the room, and a debug switch
       that hid the room's contents must not also swallow the screen the player
       is looking at. It is a no-op unless the board is up. */
    incinerator_panel_draw(ctx);
}
