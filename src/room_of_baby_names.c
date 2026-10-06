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
#include "room_of_baby_names.h"
#include "lumberer.h"
#include "maggot.h"
#include "crawler.h"
#include "collision.h"
#include "room_data.h"
#include "room_of_baby_names_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "sconce.h"             /* ...and the third: the plinth's gold */
#include "save_point.h"
#include "dresser.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons, show_pickup_msg_raw */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* The Room of Baby Names — see room_of_baby_names.h for the layout, the name
   plates, the door and the plinth. */

static SMD  *rbn_smd  = NULL;
static void *rbn_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   450/1600 and the full +50%/+50% lantern bonus — the Rooms of Arms', Heads',
   Legs', Bones' and Torsos' numbers for the same octagon. The Nursery caps its
   bonus because six crib beams were being drawn at once; nothing in here is
   additive, so there is no reason to. */
#define RBN_BASE_FOG_NEAR   450
#define RBN_BASE_FOG_FAR   1600

#define RBN_VIEW_UNIT        256
#define RBN_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define RBN_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */
#define RBN_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t rbn_view     = RBN_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t rbn_fog_near = RBN_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t rbn_fog_far  = RBN_BASE_FOG_FAR;

static int32_t rbn_view_target(void) {
    int32_t s = RBN_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += RBN_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += RBN_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void rbn_view_resolve(int snap) {
    int32_t target = rbn_view_target();
    if (snap) {
        rbn_view = target;
    } else if (rbn_view < target) {
        rbn_view += RBN_VIEW_RATE;
        if (rbn_view > target) rbn_view = target;
    } else if (rbn_view > target) {
        rbn_view -= RBN_VIEW_RATE;
        if (rbn_view < target) rbn_view = target;
    }
    rbn_fog_near = (RBN_BASE_FOG_NEAR * rbn_view) >> 8;
    rbn_fog_far  = (RBN_BASE_FOG_FAR  * rbn_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). */
#define RBN_FOG_R             7
#define RBN_FOG_G             6
#define RBN_FOG_B             9

/* Wall standoff. The chapter's 195. */
#define RBN_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define RBN_FLOOR_Y            0
#define RBN_EYE_Y           (RBN_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   ONE, FLAT, AT y=0, over the proxy's floor. Its three floor faces are three
   slabs of one plane (see the FLOOR list at the foot of
   src/room_of_baby_names_mesh_collision.c) — x[-1293,-535], x[-535,535] and
   x[535,1293], all z[-1293,1293] — the Nursery's case exactly. */
static void rbn_floor_zones_init(void) {
    int i = 0;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -1293;  floor_zones[i].max_x = 1293;
    floor_zones[i].min_z = -1293;  floor_zones[i].max_z = 1293;
    floor_zones[i].y     = RBN_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   FOUR, AND THE ROOM OWNS ONE.

     0 cobblestones        the walls and the floor
                           — 286 of the 315 polys          (x384 y0)
     1 sconce              the gold plinth, 5 polys        (x448 y0)
     2 catacomb inner door the eight doorways, 16 polys    (x832 y0)
     3 baby names          the eight name plates, 4bpp     (x640 y0)  OWNED HERE

   Slots 0 and 2 come through src/catacombs_entry.c's narrow uploaders, as every
   Chapter 3 room takes them, and slot 1 through src/sconce.c's — the plinth is
   modelled in the sconces' gold, and the Gaol Cells already borrow that page
   the same way.

   >>> SLOT 3 TIME-SHARES THE ROOM OF ARMS' PAGE, NOT ITS PALETTE. <<< 4bpp,
   so it covers x[640,672) only — the gaol door's and the gula tablet's terms
   exactly: the arms, heads, legs, bones, torsos, the Shelf's panel, the gaol
   door and the gula tablet all take turns on x640 y0, each room puts its own
   art back on entry, and none of them is drawn here. Its CLUT is its own, at
   (608,502), beside the gaol door's and the gula tablet's.

   All four sit at Voff 0, so the one 128 texture window in the draw serves
   them. */
#define ROOM_OF_BABY_NAMES_TEX_COUNT 4

static uint16_t tex_tpage[ROOM_OF_BABY_NAMES_TEX_COUNT];
static uint16_t tex_clut[ROOM_OF_BABY_NAMES_TEX_COUNT];

/* The one texmgr entry this room owns (slot 3). -1 until registration — a
   refused registration stops the boot on texmgr_refused()'s red screen, so a
   -1 here past startup would be a bug. texmgr_upload on -1 is a no-op. */
static int names_tex = -1;

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/nursery.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int rbn_key_count = 0;

static void rbn_build_cull_keys(void) {
    rbn_key_count = 0;
    if (!rbn_smd) return;
    uint8_t *p = (uint8_t *)rbn_smd->p_prims;
    int i, n = rbn_smd->n_prims;
    if (n > ROOM_OF_BABY_NAMES_PRIM_COUNT) n = ROOM_OF_BABY_NAMES_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &rbn_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    rbn_key_count = n;
}

void room_of_baby_names_load_geometry(void) {
    rbn_buff = room_arena_load("\\TEXCTCMB\\BBYNAMES.SMD;1");
    rbn_smd  = rbn_buff ? smdInitData(rbn_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of
       the same file (src/room_data.h). No block, no room. */
    if (rbn_smd && !room_data_bind(rbn_smd->n_prims)) rbn_smd = NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    rbn_build_cull_keys();
}

/* STARTUP: one registration — the name plates, header only (the bank holds
   the pixels) — and four compile-time headers. No CD access beyond that
   header. It declares the bank, which py tools/check_tex_banks.py checks
   against the uploader graph. */
void room_of_baby_names_load_assets(void) {
    texmgr_set_bank(TEXBANK_CATACOMBS);
    names_tex = texmgr_register("\\TEXCTCMB\\BBYNAMES.TIM;1");

    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, SCONCE);
    TIM_SLOT(2, CTCMBDR);
    TIM_SLOT(3, BBYNAMES);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   >>> THE NAMES LINE IS WHAT OVERWRITES THE ARMS' PAGE (OR WHICHEVER OF ITS
   EIGHT OTHER OWNERS WAS LAST). <<< Nothing else in this room draws x640 y0,
   and each of those rooms' uploaders puts its own art back on the way in
   there — the Gaol Cells' through gaol_entry_upload_gaol_door(), which is the
   way back out of here. */
void room_of_baby_names_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    sconce_upload_texture();
    texmgr_upload(names_tex);
}

/* ---- THE WEST DOOR ---------------------------------------------------------
   x=-1293, z[-107,107], y[-400,0] — the door with JOHN over it, in the
   octagon's west face, and the only door the player can use. Back into the
   Gaol Cells, through the catacomb door in their east wall.

   A door in the YZ plane at fixed X, approached from +X (wall 6 runs x=-1293
   with nx=+4096), so TEXT_PLANE_YZ with mirror=0 and the sign 11 proud of the
   wall along +X — the Gaol Cells' west door's terms. The reading axis for a
   YZ sign is Z, so the -200 door_draw_string_3d wants goes on the Z argument.

   FIVE MORE ARE WIRED to the Dead Ends (the table below), and the last two —
   the blank plate north-east and ANTONI south — are drawn and nothing else,
   waiting on The Neck and the Throat. */
#define RBN_WEST_X          (-1293)
#define RBN_WEST_Z              0     /* the art spans z[-107,107] */
#define RBN_WEST_TEXT_Y      (-186)   /* eye level on the y=0 floor */
#define RBN_TEXT_RADIUS      1200
#define RBN_FADE_NEAR         800
#define RBN_TRIGGER_RADIUS    500

/* ---- THE FIVE DEAD END DOORS -----------------------------------------------
   One row per named door that leads to a Dead End (src/dead_end.h). Each
   centre is the midpoint of its octagon face, read off the inner-door polys
   in Room of Baby Names.smx: the four diagonals at (+-914, +-914), the two
   square faces at 1293 out.

     in_x/in_z  the face's INWARD normal, the collision wall's nx/nz (4096 =
                1.0; the diagonals are 2896 = 4096/sqrt2). The spawn stands
                RBN_DE_SPAWN_OFF along it and the sign 11 proud of the face.
     in_rot     cam_rot along that normal — the arrival faces into the room.
     sign_yaw   cam_rot of a player FACING the door, i.e. the outward normal.
                door_draw_string_3d_yaw reads along the right of that yaw, so
                the sign reads forwards from inside the room. The yaw form is
                used for all five because four of the faces are diagonal and
                TEXT_PLANE_XY/_YZ only offer the square facings.

   The trigger is the west door's: Manhattan from the centre under
   RBN_TRIGGER_RADIUS, and facing it. Neighbouring doors are 1293 apart in
   Manhattan terms, so no two 500 radii overlap. */
#define RBN_DE_SPAWN_OFF      220   /* the chapter's 195 push + 25, as the west door */
#define RBN_DE_DOOR_COUNT       5

typedef struct {
    int16_t   x, z;
    int16_t   in_x, in_z;
    int16_t   in_rot, sign_yaw;
    GameState dest;
} RbnDeadEndDoor;

static const RbnDeadEndDoor rbn_de_doors[RBN_DE_DOOR_COUNT] = {
    /*   x      z     in_x   in_z  in_rot yaw   */
    { -914,   914,  2896, -2896,  1536, 3584, STATE_DEAD_END_BENJ     }, /* NW */
    {    0,  1293,     0, -4096,  2048,    0, STATE_DEAD_END_MATTHEW  }, /* N  */
    { 1293,     0, -4096,     0,  3072, 1024, STATE_DEAD_END_CHRISTOF }, /* E  */
    {  914,  -914, -2896,  2896,  3584, 1536, STATE_DEAD_END_LUKE     }, /* SE */
    { -914,  -914,  2896,  2896,   512, 2560, STATE_DEAD_END_MARK     }, /* SW */
};

/* ---- THE PLINTH ------------------------------------------------------------
   The gold lectern in the middle of the room: the five polys of
   Room of Baby Names.smx textured `sconce`, a box x[-107,107] z[-99,99] whose
   top slopes from y=-120 on its west edge to y=-230 on its east. Its four
   sides are collision walls 0-3 (y[-229,0]), so the player stands at least
   the 195 push off it: ~300 from the centre square-on, and at a corner about
   (245,237) — ~480 Manhattan.

   THE SIGN floats just over the high edge of the top — glyph top -290, so
   its 28 rows end at -262, clear of the -230 slab — and is turned to the
   camera's yaw every frame, so it reads forwards from all four sides. The
   reach is 550 Manhattan from the centre plus the usual facing test, which
   takes a press from every side and every corner. The door is ~1290 away, far
   outside it.

   THE INSCRIPTION is one sentence, and it does not fit the HUD log: the box is
   15 columns by 4 rows (src/hud.c), and word-wrapped the full line is FIVE
   rows, so posted whole its first row would scroll off before it was ever
   drawn. So it goes up in TWO halves, a beat apart:

       "Babies with no name"                       2 rows
       "cannot walk through the gates of heaven"   3 rows, the last exactly 15

   The first half is on screen alone for RBN_READ_GAP frames, and when the
   second follows, the four rows left in the box read "name / cannot walk /
   through the / gates of heaven" — still a sentence. Presses while the second
   half is pending are swallowed, so the two cannot interleave with a repeat. */
#define RBN_PLINTH_X            0
#define RBN_PLINTH_Z            0
#define RBN_PLINTH_TEXT_Y    (-290)   /* glyph top; just over the -230 high edge */
#define RBN_PLINTH_RADIUS     550
#define RBN_READ_GAP           75     /* 1.25 s between the two halves */

static const char RBN_READ_1[] = "Babies with no name";
static const char RBN_READ_2[] = "cannot walk through the gates of heaven";

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int west_circle_prev   = 1;
static int de_circle_prev     = 1;   /* one for all five: one test a frame */
static int plinth_circle_prev = 1;

/* Frames until the second half of the inscription posts; 0 = none pending. */
static int rbn_read_timer = 0;

void room_of_baby_names_arm(void) {
    west_circle_prev   = interact_tapped();
    de_circle_prev     = interact_tapped();
    plinth_circle_prev = interact_tapped();
    rbn_read_timer     = 0;
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int room_of_baby_names_west_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !west_circle_prev;
    int32_t dx, dz, xz;
    west_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - RBN_WEST_X;
    dz = cam_z - RBN_WEST_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= RBN_TRIGGER_RADIUS) return 0;
    if (!interact_facing(RBN_WEST_X, RBN_WEST_Z)) return 0;
    return 1;
}

int room_of_baby_names_dead_end_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !de_circle_prev;
    int k;
    de_circle_prev = held;
    if (lock || !just) return 0;
    for (k = 0; k < RBN_DE_DOOR_COUNT; k++) {
        const RbnDeadEndDoor *d = &rbn_de_doors[k];
        int32_t dx = cam_x - d->x;
        int32_t dz = cam_z - d->z;
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (xz >= RBN_TRIGGER_RADIUS) continue;
        if (!interact_facing(d->x, d->z)) continue;
        return d->dest;
    }
    return 0;
}

static int rbn_plinth_in_reach(void) {
    int32_t dx = cam_x - RBN_PLINTH_X;
    int32_t dz = cam_z - RBN_PLINTH_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < RBN_PLINTH_RADIUS && interact_facing(RBN_PLINTH_X, RBN_PLINTH_Z);
}

int room_of_baby_names_update(int lock) {
    int held = interact_tapped();
    int just = held && !plinth_circle_prev;
    plinth_circle_prev = held;

    /* The second half, a beat after the first. Held while a menu is up, so it
       does not land under one. */
    if (rbn_read_timer > 0 && !lock) {
        if (--rbn_read_timer == 0) show_pickup_msg_raw(RBN_READ_2);
    }

    if (lock || !just || !rbn_plinth_in_reach()) return 0;

    if (rbn_read_timer == 0) {
        show_pickup_msg_raw(RBN_READ_1);
        rbn_read_timer = RBN_READ_GAP;
    }
    return 1;
}

/* The two floating signs. Same shape as every other sign in the game: opaque
   within RBN_FADE_NEAR, gone by RBN_TEXT_RADIUS. */
static int rbn_sign_fade(int32_t wx, int32_t wz) {
    int32_t dx = cam_x - wx;
    int32_t dz = cam_z - wz;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);

    if (xz >= RBN_TEXT_RADIUS) return 0;
    if (xz > RBN_FADE_NEAR) {
        int range = RBN_TEXT_RADIUS - RBN_FADE_NEAR;
        int prog  = xz - RBN_FADE_NEAR;
        if (prog > range) prog = range;
        return 256 - ((prog * 256) / range);
    }
    return 256;
}

static void rbn_west_door_text(RenderContext *ctx) {
    int fade = rbn_sign_fade(RBN_WEST_X, RBN_WEST_Z);
    if (!fade) return;
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        RBN_WEST_X + 11, RBN_WEST_TEXT_Y, RBN_WEST_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

/* The five Dead End doors' signs, each on its door's fixed outward yaw and 11
   proud of the face. Centred on the point given, so no -200. */
static void rbn_dead_end_doors_text(RenderContext *ctx) {
    int k;
    for (k = 0; k < RBN_DE_DOOR_COUNT; k++) {
        const RbnDeadEndDoor *d = &rbn_de_doors[k];
        int fade = rbn_sign_fade(d->x, d->z);
        if (!fade) continue;
        door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                                d->x + (d->in_x * 11) / 4096, RBN_WEST_TEXT_Y,
                                d->z + (d->in_z * 11) / 4096,
                                50, 255, 50, fade, d->sign_yaw,
                                DOOR_PIXEL_SIZE);
    }
}

/* On the camera's own yaw, so it is square to the view from whichever side the
   player walks up to the plinth; door_draw_string_3d_yaw centres on the point
   it is given, so no -200 here. */
static void rbn_plinth_text(RenderContext *ctx) {
    int fade = rbn_sign_fade(RBN_PLINTH_X, RBN_PLINTH_Z);
    if (!fade) return;
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to read",
                            RBN_PLINTH_X, RBN_PLINTH_TEXT_Y, RBN_PLINTH_Z,
                            50, 255, 50, fade, cam_rot & 4095,
                            DOOR_PIXEL_SIZE);
}

void room_of_baby_names_spawn_west(void) {
    /* 220 off wall 6 on its walkable +X side, on the door's centre line,
       facing +X — the direction of travel, toward the plinth. (-1073,0) is
       ~535 from either western chamfer and ~966 from the plinth's west face,
       so the 195 push is quiet. */
    cam_x   = RBN_WEST_X + (RBN_WALL_RADIUS + 25);
    cam_y   = RBN_EYE_Y;
    cam_vy  = 0;
    cam_z   = RBN_WEST_Z;
    cam_rot = 1024;                    /* facing +X, east into the room */
    room_of_baby_names_arm();
}

void room_of_baby_names_spawn_from_dead_end(int dead_end) {
    int k;
    for (k = 0; k < RBN_DE_DOOR_COUNT; k++) {
        const RbnDeadEndDoor *d = &rbn_de_doors[k];
        if (d->dest != dead_end) continue;
        /* RBN_DE_SPAWN_OFF along the face's inward normal, on its centre
           line, facing into the room. The diagonals land ~535 from either
           neighbouring face, the square ones likewise, so the 195 push is
           quiet. */
        cam_x   = d->x + (d->in_x * RBN_DE_SPAWN_OFF) / 4096;
        cam_y   = RBN_EYE_Y;
        cam_vy  = 0;
        cam_z   = d->z + (d->in_z * RBN_DE_SPAWN_OFF) / 4096;
        cam_rot = d->in_rot;
        room_of_baby_names_arm();
        return;
    }
}

void room_of_baby_names_init(void) {
    room_data_collision(&current_collision_room);
    /* THE WALLS, read off the VISUAL mesh: the octagon's stone rises to
       y=-800, where the proxy's walls stop too. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(RBN_WALL_RADIUS);

    rbn_floor_zones_init();
    cam_pitch = 0;

    room_of_baby_names_spawn_west();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every collide routine, so an
       instance left from another room would block invisibly anywhere it falls
       inside x/z[-1293,1293]. The Catacombs Entry's sconces at (+-595,200) land
       inside it; sconces gate on area, so they do not bite, and this is the
       cheap guarantee rather than a fix. Safe to clear: catacombs_entry_init()
       re-places all four. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    rbn_view_resolve(1);
}

static void draw_room_of_baby_names_smd(RenderContext *ctx) {
    if (!rbn_smd) return;

    uint8_t *p = (uint8_t *)rbn_smd->p_prims;
    int i, n = rbn_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = rbn_fog_far;   /* resolved by rbn_view_resolve() this frame */
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < n; i++) {
        /* THE REJECT PATH READS cull_keys, NOT THE MESH. */
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
        SVECTOR *v0 = &rbn_smd->p_verts[vi[0]];
        SVECTOR *v1 = &rbn_smd->p_verts[vi[1]];
        SVECTOR *v2 = &rbn_smd->p_verts[vi[2]];

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

        int nocull = (i < ROOM_OF_BABY_NAMES_PRIM_COUNT) && room_nocull(i);
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
            v3 = &rbn_smd->p_verts[vi[3]];
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
        /* Horizontal polys sort by their farthest corner (render.h): the floor
           must not be sorted over the plinth standing on it. */
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
        int32_t fog = dist < rbn_fog_near ? rbn_fog_near : (dist > rbn_fog_far ? rbn_fog_far : dist);
        int32_t fog_factor = ((rbn_fog_far - fog) << 8) / (rbn_fog_far - rbn_fog_near);

        uint8_t tex_idx = (i < ROOM_OF_BABY_NAMES_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < ROOM_OF_BABY_NAMES_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + RBN_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + RBN_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + RBN_FOG_B * (256 - fog_factor)) >> 8);

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

void room_of_baby_names_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    rbn_view_resolve(0);

    g_fog_near = rbn_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : rbn_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, RBN_FOG_R, RBN_FOG_G, RBN_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All four textures sit at Voff 0. */
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

    if (exp != DBG_EXP_NO_MESH) draw_room_of_baby_names_smd(ctx);

    /* >>> LEVEL 8 REMOVES THE SEVEN SIGNS AND THE ENEMIES. <<< The room has
       no props, so the signs are what stands in it. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        rbn_west_door_text(ctx);   /* west: YZ plane, approached from +X */
        rbn_dead_end_doors_text(ctx); /* the five Dead Ends: fixed yaws   */
        rbn_plinth_text(ctx);     /* the plinth: on the camera's yaw     */
        /* THE CHAPTER 3 ENEMIES, drawn in every room of the chapter whether or
           not world.c places one here: the area tag makes an absent enemy free.
           Their sheets sit at Voff 128, so each is handed the window to restore
           after drawing unmasked. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
            lumberers_set_texwindow(&tw);
            maggots_set_texwindow(&tw);
        }
        draw_crawlers(ctx);
        draw_lumberers(ctx);
        draw_maggots(ctx);   /* area-tagged: free where none is placed */
    }
}
