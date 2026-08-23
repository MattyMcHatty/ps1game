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
#include "tim_slots.h"
#include "camera.h"
#include "reception.h"
#include "collision.h"
#include "reception_mesh_collision.h"
#include "reception_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "save_point.h"
#include "dresser.h"
#include "fatdoor.h"
#include "item_pickup.h"
#include "hadad.h"     /* he can come into the mansion through here */
#include "texmgr.h"
#include "player.h"    /* FLAG_HALL_2F_DOOR / FLAG_WEST_CORR_DOOR — the saved
                          locks on the two upper-floor west-wall doors. Both are
                          set from the FAR side (hall_2f.c / west_corridor.c);
                          this room only reads them. Plus FLAG_HADAD_TWO, which
                          seals the room — see reception_sealed() below. */

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Placeholder reception room: untextured geometry rendered flat-shaded with the
   same distance fog as the kitchen, so it blends with the dark interior until
   the finished art replaces it. Modelled on kitchen_dining.c but with no
   textures/tex_map (Reception.smx carries no texture references yet). */

static SMD  *reception_smd  = NULL;
static void *reception_buff = NULL;

/* Multi-level floor layout (taken from the collision mesh, Reception_mesh.smx):
   ground (y=0) -> ramp A up to a y=-150 platform -> ramp B up to the y=-600
   upper floor. apply_height() picks the first matching zone, skipping flat/upper
   zones that sit ABOVE the player, so elevated zones must be listed BEFORE the
   ground catch-all (which spans the whole room beneath everything). */
static void reception_floor_zones_init(void) {
    int i = 0;

    /* Ramp A — climbs along +X from ground (y=0 @ x=-100) to the platform
       (y=-150 @ x=300). */
    floor_zones[i].type            = FLOOR_RAMP;
    floor_zones[i].min_x = -100; floor_zones[i].max_x = 300;
    floor_zones[i].min_z = -700; floor_zones[i].max_z = -300;
    floor_zones[i].ramp_y_start    = 0;     /* y at x=-100 (ground)   */
    floor_zones[i].ramp_y_end      = -150;  /* y at x=300  (platform) */
    floor_zones[i].ramp_axis_start = -100;
    floor_zones[i].ramp_axis_end   = 300;
    floor_zones[i].ramp_along_x    = 1;
    i++;

    /* Ramp B — climbs along +Z from the platform (y=-150 @ z=-300) to the
       upper floor (y=-600 @ z=500). */
    floor_zones[i].type            = FLOOR_RAMP;
    floor_zones[i].min_x = 300; floor_zones[i].max_x = 700;
    floor_zones[i].min_z = -300; floor_zones[i].max_z = 500;
    floor_zones[i].ramp_y_start    = -150;  /* y at z=-300 (platform)    */
    floor_zones[i].ramp_y_end      = -600;  /* y at z=500  (upper floor) */
    floor_zones[i].ramp_axis_start = -300;
    floor_zones[i].ramp_axis_end   = 500;
    floor_zones[i].ramp_along_x    = 0;
    i++;

    /* Platform between the two ramps (y=-150). */
    floor_zones[i].type  = FLOOR_UPPER;
    floor_zones[i].min_x = 300; floor_zones[i].max_x = 700;
    floor_zones[i].min_z = -700; floor_zones[i].max_z = -300;
    floor_zones[i].y     = -150;
    i++;

    /* Upper floor (y=-600), L-shaped: the +Z/+X area... */
    floor_zones[i].type  = FLOOR_UPPER;
    floor_zones[i].min_x = -900; floor_zones[i].max_x = 1500;
    floor_zones[i].min_z = 500;  floor_zones[i].max_z = 1500;
    floor_zones[i].y     = -600;
    i++;

    /* ...and the west strip. */
    floor_zones[i].type  = FLOOR_UPPER;
    floor_zones[i].min_x = -1500; floor_zones[i].max_x = -900;
    floor_zones[i].min_z = -1500; floor_zones[i].max_z = 1500;
    floor_zones[i].y     = -600;
    i++;

    /* Ground catch-all (whole room) — MUST be last so elevated zones win. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -1500; floor_zones[i].max_x = 1500;
    floor_zones[i].min_z = -1500; floor_zones[i].max_z = 1500;
    floor_zones[i].y     = 0;
    i++;

    floor_zone_count = i;
}

/* ---- Per-room textures -----------------------------------------------------
   Reception's nine mesh textures. Six are resident from startup (shared with the
   kitchen / fatdoor); three are UNIQUE to reception (strs, bnnstr, frnt_dr) and
   occupy VRAM slots the kitchen also uses, so they must be (re)uploaded into VRAM
   each time reception is entered.

   The unique textures' TIM data is held resident in RAM (preloaded at startup),
   so the entry-time upload is a PURE LoadImage with NO CD access. The old design
   did a CdRead at transition time, which hung: a mid-game CdRead competes with
   the CD-DA drive and the psxcd library gets into a bad state. Preloading the
   bytes at startup removes the only CD access from the transition. */
#define RECEPTION_TEX_COUNT 9
static uint16_t tex_tpage[RECEPTION_TEX_COUNT];
static uint16_t tex_clut[RECEPTION_TEX_COUNT];

/* The reception-only textures, with their reception_tex_map slot indices. The
   texture manager keeps them RAM-resident so reception_upload_textures() needs
   no CD read; new_tex_id[] holds the manager ids captured at startup. */
/* Slot 3 (formerly bnnstr, the banister) is retired: the mesh no longer
   references it, so nothing streams into the kchn_tile slot for reception —
   the piano room's piano_keys texture time-shares it instead. */
#define RECEPTION_NEW_TEX 2
static int new_tex_id[RECEPTION_NEW_TEX];
static const struct { const char *file; int slot; } new_tex[RECEPTION_NEW_TEX] = {
    { "\\TEX\\STRS.TIM;1",   0 },
    { "\\TEX\\FRNTDR.TIM;1", 5 },
};

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale. */
void reception_load_geometry(void) {
    reception_buff = room_arena_load("\\RECEPT.SMD;1");
    reception_smd  = reception_buff ? smdInitData(reception_buff) : NULL;
}

/* Preload textures at STARTUP. Geometry moved to reception_load_geometry above;
   what remains is the texmgr registration of reception's unique textures, kept
   RAM-resident so the entry-time upload is a pure LoadImage with no CD read. */
void reception_load_assets(void) {
    /* Register the 3 reception-only textures with the texture manager (RAM-
       resident, uploaded to VRAM on each reception entry) and capture their
       tpage/clut into the renderer's slot table. */
    for (int i = 0; i < RECEPTION_NEW_TEX; i++) {
        int slot = new_tex[i].slot;
        new_tex_id[i]   = texmgr_register(new_tex[i].file);
        tex_tpage[slot] = texmgr_tpage(new_tex_id[i]);
        tex_clut[slot]  = texmgr_clut(new_tex_id[i]);
    }

    /* The other 6 are resident from startup (kitchen + fatdoor); just capture
       their tpage/clut for reception's renderer. */
    TIM_SLOT(1, REDWLPPR);
    TIM_SLOT(2, WDFLR);
    TIM_SLOT(4, DINCL);
    TIM_SLOT(6, WDDR);
    TIM_SLOT(7, INRDBLDR);
    TIM_SLOT(8, STNGLS);
}

/* Upload reception's 3 unique textures into VRAM from their resident RAM copies.
   Pure LoadImage — no CD access — so it is safe during the room transition (the
   caller idles the GPU with DrawSync first, and kicks no draw until the next
   flip_buffers). */
void reception_upload_textures(void) {
    for (int i = 0; i < RECEPTION_NEW_TEX; i++)
        texmgr_upload(new_tex_id[i]);
}

/* ---- Door back to the kitchen ---------------------------------------------
   The double door on the bottom floor (where the player spawns). Mirrors the
   kitchen's "to reception" door: a floating "Press " BTN_CIRCLE " to enter" sign, and a fresh
   Circle press within range starts the transition back. */
#define RDOOR_X                  1450
#define RDOOR_Z                 (-414)
/* Text TOP, centred on the drawn door leaf. In "Reception v2.smx" every
   ground-floor door spans y[-429,0] (mid -214) and the glyphs are 7 rows of
   DOOR_PIXEL_SIZE = 28 units tall, so a top of -229 puts the text's middle on
   the door's. Shared by RDOOR, WDOOR and CDOOR — all three leaves match.
   (The upper-floor doors use their own constants; see HDOOR_TEXT_Y.) */
#define RDOOR_TEXT_Y            (-229)
#define RDOOR_TEXT_RADIUS        1500
#define RDOOR_FADE_NEAR          1000   /* fully opaque within this distance */
#define RDOOR_TRIGGER_RADIUS      500   /* distance at which Circle activates */

/* Circle edge-detect, seeded by reception_door_arm(). Starts "held" so a press
   carried in from the kitchen-side transition doesn't immediately bounce back. */
static int rdoor_circle_prev = 1;

void reception_door_arm(void) {
    int held = interact_tapped();
    rdoor_circle_prev = held;
}

int reception_door_triggered(void) {
    int held = interact_tapped();
    int just = held && !rdoor_circle_prev;
    rdoor_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - RDOOR_X;
    int32_t dz = cam_z - RDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < RDOOR_TRIGGER_RADIUS && interact_facing(RDOOR_X, RDOOR_Z);
}

/* Floating "Press " BTN_CIRCLE " to enter" sign on the double door, in the YZ plane (faces
   along X). The player approaches from the -X (room) side, so mirror=1. */
static void reception_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - RDOOR_X;
    int32_t dz = cam_z - RDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= RDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > RDOOR_FADE_NEAR) {
        int range = RDOOR_TEXT_RADIUS - RDOOR_FADE_NEAR;
        int prog  = xz - RDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        RDOOR_X, RDOOR_TEXT_Y, RDOOR_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* ---- Door to the new west room --------------------------------------------
   On the west wall; the player approaches from the +X (room) side, so the
   sign is in the YZ plane with mirror=0 (same facing as the kitchen's
   "to reception" sign). Shares the fade radii and text height with RDOOR. */
#define WDOOR_X                (-1435)
#define WDOOR_Z                 (-756)

/* Circle edge-detect for the west door, seeded by wdoor_arm() (same pattern
   as rdoor_circle_prev above). */
static int wdoor_circle_prev = 1;

void wdoor_arm(void) {
    int held = interact_tapped();
    wdoor_circle_prev = held;
}

int wdoor_triggered(void) {
    int held = interact_tapped();
    int just = held && !wdoor_circle_prev;
    wdoor_circle_prev = held;
    if (!just) return 0;
    if (player_on_upper_floor) return 0;   /* ground-floor door only (2F door sits above it) */

    int32_t dx = cam_x - WDOOR_X;
    int32_t dz = cam_z - WDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < RDOOR_TRIGGER_RADIUS && interact_facing(WDOOR_X, WDOOR_Z);
}

static void wdoor_text(RenderContext *ctx) {
    int32_t dx = cam_x - WDOOR_X;
    int32_t dz = cam_z - WDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= RDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > RDOOR_FADE_NEAR) {
        int range = RDOOR_TEXT_RADIUS - RDOOR_FADE_NEAR;
        int prog  = xz - RDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        WDOOR_X, RDOOR_TEXT_Y, WDOOR_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* ---- Door to the conservatory ---------------------------------------------
   Also on the west wall (bottom floor), north of the piano-room door; same
   facing, so the sign is again YZ-plane with mirror=0. Shares the fade radii
   and text height with the other reception doors. */
#define CDOOR_X                (-1435)
#define CDOOR_Z                  757

/* Circle edge-detect for the conservatory door, seeded by cdoor_arm(). */
static int cdoor_circle_prev = 1;

void cdoor_arm(void) {
    int held = interact_tapped();
    cdoor_circle_prev = held;
}

int cdoor_triggered(void) {
    int held = interact_tapped();
    int just = held && !cdoor_circle_prev;
    cdoor_circle_prev = held;
    if (!just) return 0;
    if (player_on_upper_floor) return 0;   /* ground-floor door only */

    int32_t dx = cam_x - CDOOR_X;
    int32_t dz = cam_z - CDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < RDOOR_TRIGGER_RADIUS && interact_facing(CDOOR_X, CDOOR_Z);
}

static void cdoor_text(RenderContext *ctx) {
    int32_t dx = cam_x - CDOOR_X;
    int32_t dz = cam_z - CDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= RDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > RDOOR_FADE_NEAR) {
        int range = RDOOR_TEXT_RADIUS - RDOOR_FADE_NEAR;
        int prog  = xz - RDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        CDOOR_X, RDOOR_TEXT_Y, CDOOR_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* ---- 2nd-floor door to the 2F Hall ----------------------------------------
   The southernmost door on the UPPER floor (y=-600), on the west wall at
   z=-750. It sits almost directly above the ground-floor piano door (z=-756),
   so this interaction — and wdoor/cdoor — are gated by player_on_upper_floor to
   keep the two levels' west-wall doors from cross-triggering. Player approaches
   from the +X (room) side, so the sign is YZ-plane with mirror=0, at the upper
   floor's eye height. */
#define HDOOR_X                (-1435)
#define HDOOR_Z                 (-750)
/* Text TOP, centred on the drawn door leaf, as RDOOR_TEXT_Y is. The upper
   floor's doors span y[-1029,-600] (mid -814), so the top sits at -829. */
#define HDOOR_TEXT_Y            (-829)

static int hdoor_circle_prev = 1;

void hdoor_arm(void) {
    int held = interact_tapped();
    hdoor_circle_prev = held;
}

int hdoor_triggered(void) {
    int held = interact_tapped();
    int just = held && !hdoor_circle_prev;
    hdoor_circle_prev = held;
    if (!just) return 0;
    if (!player_on_upper_floor) return 0;   /* upper-floor door only */
    if (!game_flag(FLAG_HALL_2F_DOOR)) return 0;   /* locked from the Hall 2F side */

    int32_t dx = cam_x - HDOOR_X;
    int32_t dz = cam_z - HDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < RDOOR_TRIGGER_RADIUS && interact_facing(HDOOR_X, HDOOR_Z);
}

static void hdoor_text(RenderContext *ctx) {
    if (!player_on_upper_floor) return;   /* only visible from the upper floor */
    int32_t dx = cam_x - HDOOR_X;
    int32_t dz = cam_z - HDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= RDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > RDOOR_FADE_NEAR) {
        int range = RDOOR_TEXT_RADIUS - RDOOR_FADE_NEAR;
        int prog  = xz - RDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* Until the Hall 2F side unlocks it, this door reads "Locked from the other
       side" in red and does nothing; afterwards it's a normal entry door. */
    if (!game_flag(FLAG_HALL_2F_DOOR)) {
        door_draw_string_3d(ctx, "Locked from the other side",
                            HDOOR_X, HDOOR_TEXT_Y, HDOOR_Z - 200,
                            255, 50, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
        return;
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        HDOOR_X, HDOOR_TEXT_Y, HDOOR_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* ---- 2nd-floor NORTH-WEST door, into the West Corridor ---------------------
   The other UPPER-floor door on the west wall (x=-1500, z spans 857..1071 in
   Reception.smx, so centre 964), north of HDOOR. It used to be a dead end that
   only ever showed the red "Locked from the other side" sign; it now opens on
   the West Corridor's EAST door (see src/west_corridor.h), a single wooden leaf
   so the transition is DOOR_PANEL_WOOD.

   Same facing as HDOOR: approached from the +X (room) side, YZ-plane sign with
   mirror=0, at the upper floor's eye height. Gated on player_on_upper_floor
   like the rest of the west wall's doors so the two levels never
   cross-trigger.

   LOCKED FROM THE OTHER SIDE until the player unlocks it in the West Corridor
   — the same arrangement HDOOR has with the 2F Hall, reading
   FLAG_WEST_CORR_DOOR instead of FLAG_HALL_2F_DOOR. Until then this
   side shows the red sign and does nothing at all. */
#define NDOOR_X                (-1435)
#define NDOOR_Z                   964
#define NDOOR_TEXT_Y            (-829)   /* centred on the door leaf; see HDOOR_TEXT_Y */

static int ndoor_circle_prev = 1;

void ndoor_arm(void) {
    int held = interact_tapped();
    ndoor_circle_prev = held;
}

int ndoor_triggered(void) {
    int held = interact_tapped();
    int just = held && !ndoor_circle_prev;
    ndoor_circle_prev = held;
    if (!just) return 0;
    if (!player_on_upper_floor) return 0;   /* upper-floor door only */
    if (!game_flag(FLAG_WEST_CORR_DOOR)) return 0;   /* locked from the corridor side */

    int32_t dx = cam_x - NDOOR_X;
    int32_t dz = cam_z - NDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < RDOOR_TRIGGER_RADIUS && interact_facing(NDOOR_X, NDOOR_Z);
}

/* Arrive back on reception's upper floor through this door, facing +X into the
   room. 210 clear of the x=-1500 wall — past the 195 push radius — the same
   standoff HDOOR's and EDOOR's arrivals use. */
void reception_spawn_northwest(void) {
    cam_x   = -1290;
    cam_y   = -789;   /* upper-floor standing eye (-600 - 189) */
    cam_vy  = 0;
    cam_z   = NDOOR_Z;
    cam_rot = 1024;   /* face +X, into reception */
    ndoor_arm();      /* don't re-trigger on the held Circle */
}

static void ndoor_text(RenderContext *ctx) {
    if (!player_on_upper_floor) return;   /* only visible from the upper floor */
    int32_t dx = cam_x - NDOOR_X;
    int32_t dz = cam_z - NDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= RDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > RDOOR_FADE_NEAR) {
        int range = RDOOR_TEXT_RADIUS - RDOOR_FADE_NEAR;
        int prog  = xz - RDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* Until the West Corridor side unlocks it, this door reads "Locked from the
       other side" in red and does nothing; afterwards it's a normal entry door.
       Exactly what hdoor_text does with the 2F Hall's flag. */
    if (!game_flag(FLAG_WEST_CORR_DOOR)) {
        door_draw_string_3d(ctx, "Locked from the other side",
                            NDOOR_X, NDOOR_TEXT_Y, NDOOR_Z - 200,
                            255, 50, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
        return;
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        NDOOR_X, NDOOR_TEXT_Y, NDOOR_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* ---- 2nd-floor double door to the East Hall --------------------------------
   On the UPPER floor's EAST wall (x=1500, z=1071, from the inr_dbl_dr polys in
   Reception.smx). It sits well north of the ground-floor kitchen door at
   z=-414, but is gated by player_on_upper_floor anyway so the two east-wall
   doors can never cross-trigger. The player approaches from the -X (room) side,
   so the sign is YZ-plane with mirror=1 — the same facing as the ground-floor
   kitchen door — at the upper floor's eye height. */
#define EDOOR_X                  1435
#define EDOOR_Z                  1071
#define EDOOR_TEXT_Y            (-829)   /* centred on the door leaf; see HDOOR_TEXT_Y */

static int edoor_circle_prev = 1;

void edoor_arm(void) {
    int held = interact_tapped();
    edoor_circle_prev = held;
}

int edoor_triggered(void) {
    int held = interact_tapped();
    int just = held && !edoor_circle_prev;
    edoor_circle_prev = held;
    if (!just) return 0;
    if (!player_on_upper_floor) return 0;   /* upper-floor door only */

    int32_t dx = cam_x - EDOOR_X;
    int32_t dz = cam_z - EDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < RDOOR_TRIGGER_RADIUS && interact_facing(EDOOR_X, EDOOR_Z);
}

static void edoor_text(RenderContext *ctx) {
    if (!player_on_upper_floor) return;   /* only visible from the upper floor */
    int32_t dx = cam_x - EDOOR_X;
    int32_t dz = cam_z - EDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= RDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > RDOOR_FADE_NEAR) {
        int range = RDOOR_TEXT_RADIUS - RDOOR_FADE_NEAR;
        int prog  = xz - RDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        EDOOR_X, EDOOR_TEXT_Y, EDOOR_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

void reception_init(void) {
    reception_collision_init(&current_collision_room);
    collision_set_ceiling_y(0);   /* proxy wall tops reach the drawn ceiling */
    reception_floor_zones_init();

    /* Spawn by the double door on the bottom floor, facing west (-X). */
    cam_x   = 1306;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = -414;
    cam_rot = 3072;   /* facing west (-X) */

    reception_door_arm();   /* don't re-trigger on a held Circle from the entry */
    wdoor_arm();            /* same, for the west single door */
    cdoor_arm();            /* same, for the conservatory door */
    hdoor_arm();            /* same, for the 2nd-floor door to the 2F hall */
    edoor_arm();            /* same, for the 2nd-floor door to the East Hall */
    ndoor_arm();            /* same, for the 2nd-floor NW door to the West Corridor */
    save_point_arm();       /* same, for the Circle-to-save interaction */

    /* Place reception's props. */
    save_points_clear();
    /* rot 512 = 45 deg (4096 = 360); scale 2048 = half size (4096 = full). */
    save_point_add(78, -300, -67, 512, 2048);

    dressers_clear();
    /* Dresser in the ground-floor room tucked under the upper floor (bottom-floor
       standing reference -189). rot_y is 0..4096 = a full turn. */
    dresser_add(580, -189, 958, 1024);
}

/* ---- What FLAG_HADAD_TWO does to this room ---------------------------------
   The whole rule is written up in reception.h; this is the half of it that is
   state rather than a per-frame test. Keep reception_sealed() as the ONLY place
   the flag is named, so the doors (main.c), the music (main.c) and the save
   point (here) can never disagree about whether the room is sealed.

   The save point is CLEARED, not hidden: save_points_clear() takes it out of the
   draw, out of save_point_triggered()'s range test and out of the player's
   collision in one go, and reception_init() re-places it on every entry, so
   nothing here has to be undone. There is no flag of its own to save — the room
   re-derives from FLAG_HADAD_TWO each time the player walks in.

   Called from main.c's post-entry re-derive block, AFTER world_enter and
   savegame_apply_pending; see the header for why that slot and no earlier. */
int reception_sealed(void) {
    return game_flag(FLAG_HADAD_TWO);
}

void reception_apply_flags(void) {
    if (!reception_sealed()) return;
    save_points_clear();   /* no saving once the house has closed in */
}

static void draw_reception_smd(RenderContext *ctx) {
    if (!reception_smd) return;

    uint8_t *p = (uint8_t *)reception_smd->p_prims;
    int i;

    for (i = 0; i < reception_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &reception_smd->p_verts[vi[0]];
        SVECTOR *v1 = &reception_smd->p_verts[vi[1]];
        SVECTOR *v2 = &reception_smd->p_verts[vi[2]];

        {
            int32_t dx = (int32_t)v0->vx - cam_x;
            int32_t dz = (int32_t)v0->vz - cam_z;
            /* Distance cull (Manhattan) at the fog-out distance so culled polys
               are already invisible. 1500 keeps the room GPU-fill within a 60fps
               frame (was 2300, which pushed the fill to VB2/30fps). */
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 1500)
                { p += stride; continue; }
            int32_t fwd = dx * isin(cam_rot) + dz * icos(cam_rot);
            if (fwd < -(700 << 12))
                { p += stride; continue; }
        }

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

        /* Backface cull exactly like the baseline, EXCEPT the handful of
           triangle-shaped (degenerate) quads flagged at build time in
           reception_nocull: their first triangle is collinear so gte_nclip is
           ~0 and the plain <=0 test flickered them in and out. Everything else
           (normal quads AND real triangles) uses the original test, so there is
           no extra back-face over-draw — perf matches the baseline. */
        int nocull = (i < RECEPTION_PRIM_COUNT) && reception_nocull[i];
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
            v3 = &reception_smd->p_verts[vi[3]];
            gte_ldv0(v3);
            gte_rtps();
            gte_stsxy(&sv[3]);
            gte_stsz(&sz[3]);
            if (sv[3].vx <= -1023 || sv[3].vx >= 1023 || sv[3].vy <= -1023 || sv[3].vy >= 1023) { p += stride; continue; }
            if (sz[3] == 0) { p += stride; continue; }
        }

        /* EVERY poly sorts by its farthest corner — not just the flat ones, and
           never by the GTE average. See render.h: the average ties along a
           floor/wall contact line, which is why flat polys were switched to the
           far corner. Leaving VERTICAL polys on the average then broke the
           reverse case here: the upper floor's edge ring is a band of long
           sheared quads (the outer edge loop is cut every 3000/14 = 214 units to
           match the wall, the inner grid every 250), so a single floor poly can
           be 600 units deep. Sorted at its far corner it lands BEHIND the
           ground-floor wall tucked under the balcony, whose average is nearer —
           so that wall painted over the floor edge, and over the wooden
           underside when viewed from below. Farthest-corner for everything is
           the consistent choice: it keeps floors behind what stands on them and
           keeps geometry hidden behind a floor slab from punching through it. */
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
        int32_t fog_start = 350, fog_end = 1500;   /* fog fully saturates at the cull distance */
        int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
        int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). Reception
           textures every face (no 0xFF), but keep the guard for safety. UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in reception_draw, reproducing the Blender UVs. */
        uint8_t tex_idx = (i < RECEPTION_PRIM_COUNT) ? reception_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < RECEPTION_TEX_COUNT);
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8);

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

void reception_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = 350; g_fog_far = 1500;

    /* Dark interior background, same as the kitchen. */
    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setXY0(bg, 0, 0);
    setWH(bg, SCREEN_XRES, SCREEN_YRES);
    setRGB0(bg, 20, 15, 10);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
    ctx->next_packet += sizeof(TILE);

    /* 128x128 texture window so per-poly UVs wrap (tile) within each texture's
       page. Sorted at OT_LENGTH-1 so the GPU applies it before any textured poly
       (those are clamped to OT_LENGTH-2). Mask = texels>>3 (128>>3 = 16). All
       reception textures sit at page-top (V 0-127), so one window serves them. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    /* View matrix from the camera (same construction as kitchen_dining_draw). */
    MATRIX rot_matrix;
    camera_build_view(&rot_matrix);

    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    draw_reception_smd(ctx);
    /* Floating collectibles (Grave-olver + rounds) in the room behind the fat
       door. Billboards drawn in the active view matrix; their 64x64 sprites sit
       at VRAM Voff 0 so the room's 128 texture window leaves their UVs intact. */
    item_pickups_draw(ctx);
    /* Dresser props — reuse reception's resident wd_flr (slot 2) for their
       non-drawer faces and the room's 128 texture window set above; the module
       owns the drawer texture. Restores the view matrix before returning. */
    dressers_draw(ctx, tex_tpage[2], tex_clut[2]);
    /* Breakable door in the small-room doorway. Draws with reception's active
       128 texture window (its UVs are 0-127, so wrapping is a no-op) and restores
       the view matrix before returning. */
    fatdoors_draw(ctx);
    /* HADAD. He is the first sprite enemy this room renders, and he needs no
       texture work to stand here: hadad_stp1_64 (x608 y384) and hadad_stp2_64
       (x992 y384) own their VRAM outright and are LoadImaged once at startup by
       hadads_load_textures(), so nothing in reception's upload list streams over
       them and there is nothing to restore. What IS owed is the window bracket
       — all his frames sit at Voff 128, so under the 128 window set above an
       unbracketed quad would wrap his V and draw this room's wallpaper on him.
       draw_hadads is area-gated, so it costs nothing until one is placed. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        hadads_set_texwindow(&tw);
    }
    draw_hadads(ctx);
    save_points_draw(ctx);
    reception_door_text(ctx);
    wdoor_text(ctx);
    cdoor_text(ctx);
    hdoor_text(ctx);
    ndoor_text(ctx);
    edoor_text(ctx);
}
