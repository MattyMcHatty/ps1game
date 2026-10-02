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
#include "meat_plant.h"
#include "lumberer.h"
#include "crawler.h"
#include "collision.h"
#include "meat_plant_mesh_collision.h"
#include "meat_plant_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the inner door's narrow uploader */
#include "the_pit.h"            /* ...rusty's */
#include "room_of_legs.h"       /* ...and the legs' */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "crib.h"
#include "player.h"             /* current_weapon, player_weapons */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */
#include "item_pickup.h"        /* the Meat Sack in the courtyard */

/* The Meat Plant — see meat_plant.h for the layout and the doors. */

static SMD  *mp_smd  = NULL;
static void *mp_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   450/1600 with +50%/+50% for the lantern — the Sliding Bars Room's numbers.
   1426 primitives against that room's 1179, over a hall of much the same size,
   and the mass of legs in the middle hides most of the far side anyway. */
#define MP_BASE_FOG_NEAR   450
#define MP_BASE_FOG_FAR   1600

#define MP_VIEW_UNIT        256
#define MP_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define MP_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */
#define MP_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t mp_view     = MP_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t mp_fog_near = MP_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t mp_fog_far  = MP_BASE_FOG_FAR;

static int32_t mp_view_target(void) {
    int32_t s = MP_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += MP_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += MP_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void mp_view_resolve(int snap) {
    int32_t target = mp_view_target();
    if (snap) {
        mp_view = target;
    } else if (mp_view < target) {
        mp_view += MP_VIEW_RATE;
        if (mp_view > target) mp_view = target;
    } else if (mp_view > target) {
        mp_view -= MP_VIEW_RATE;
        if (mp_view < target) mp_view = target;
    }
    mp_fog_near = (MP_BASE_FOG_NEAR * mp_view) >> 8;
    mp_fog_far  = (MP_BASE_FOG_FAR  * mp_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). */
#define MP_FOG_R             7
#define MP_FOG_G             6
#define MP_FOG_B             9

/* Wall standoff. The chapter's 195. The narrowest walkable gap is the band
   between the legs' west face (x=-2100) and the hall's west wall (x=-2700),
   600 wide, which leaves 210 down its middle. */
#define MP_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define MP_FLOOR_Y            0
#define MP_EYE_Y           (MP_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   ONE, FLAT, AT y=0, over the proxy's floor. Its five floor faces — the hall
   x[-2700,2700] z[600,6000] and the four alcoves off its sides — are all one
   plane (see the FLOOR list at the foot of src/meat_plant_mesh_collision.c), so
   one rect over the collision bounds covers them. It also covers the corners
   beside the alcoves and the inside of the mass of legs, which the walls make
   unreachable. */
static void mp_floor_zones_init(void) {
    int i = 0;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -3299;  floor_zones[i].max_x = 3299;
    floor_zones[i].min_z =     0;  floor_zones[i].max_z = 6600;
    floor_zones[i].y     = MP_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   THREE, ALL BORROWED. The room registers nothing.

     0 rusty               the walls, the floor and the vault
                           — 998 of the 1426 polys        (x704 y0)
     1 legs                the mass of legs, 422 polys    (x640 y0)
     2 catacomb inner door the three doorways             (x832 y0)

   Each comes through its owner's NARROW uploader: rusty through The Pit's
   the_pit_upload_rusty() (the cleaver prop's), the legs through the Room of
   Legs' room_of_legs_upload_legs(), the door through the Catacombs Entry's. The
   full uploaders would also stamp those rooms' other art — the Room of Legs'
   crib and creeps, The Pit's cobble and bars — which nothing here draws.

   >>> THE LEGS ARE ON THE ROOM OF ARMS' PAGE AND PALETTE (x640 y0, CLUT
   (672,501)), which the Rooms of Arms, Heads and Legs take in turns. <<< This
   room is never drawn with any of them, and each of those rooms' uploaders puts
   its own art back on entry, so this is one more room taking a turn and it owes
   nobody a restore. Rusty's page (x704 y0) and palette are The Pit's on the
   same terms. The two pages and the two CLUTs are disjoint, so drawing rusty
   and the legs together is safe.

   All three sit at Voff 0, so the one 128 texture window in the draw serves
   them. */
#define MEAT_PLANT_TEX_COUNT 3

static uint16_t tex_tpage[MEAT_PLANT_TEX_COUNT];
static uint16_t tex_clut[MEAT_PLANT_TEX_COUNT];

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/room_of_legs.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int mp_key_count = 0;

static void mp_build_cull_keys(void) {
    mp_key_count = 0;
    if (!mp_smd) return;
    uint8_t *p = (uint8_t *)mp_smd->p_prims;
    int i, n = mp_smd->n_prims;
    if (n > MEAT_PLANT_PRIM_COUNT) n = MEAT_PLANT_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &mp_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    mp_key_count = n;
}

void meat_plant_load_geometry(void) {
    mp_buff = room_arena_load("\\TEXCTCMB\\MEATPLNT.SMD;1");
    mp_smd  = mp_buff ? smdInitData(mp_buff) : NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    mp_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: three compile-time headers and NO
   registration. With nothing registered there is no bank to declare here; the
   three owners declare theirs, and py tools/check_tex_banks.py walks this
   room's uploader to check that CATACOMBS covers all three. */
void meat_plant_load_assets(void) {
    TIM_SLOT(0, RUSTY);
    TIM_SLOT(1, LEGS);
    TIM_SLOT(2, CTCMBDR);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).
   The three pages are disjoint, so there is no ordering rule between the
   calls. */
void meat_plant_upload_textures(void) {
    the_pit_upload_rusty();
    room_of_legs_upload_legs();
    catacombs_entry_upload_inner_door();
}

/* ---- THE WEST DOOR ---------------------------------------------------------
   x=-2700, z[5400,5600], y[-400,0] — in the hall's west wall (wall 9, x=-2700
   over z[4199,6000], nx=+4096), 400 short of its north end. Back into the
   Sliding Bars Room, through the SOUTH of the two doors in its east wall.

   A door in the YZ plane at fixed X, approached from +X, so TEXT_PLANE_YZ with
   mirror=0 and the sign 11 proud of the wall along +X — the Crucifix
   Corridor's west door exactly. The reading axis for a YZ sign is Z, so the
   -200 door_draw_string_3d wants goes on the Z argument.

   THE SOUTH-ALCOVE DOOR, z=0 x[-100,100] at the back of the south alcove, is
   drawn and nothing else: no sign, no trigger. It reads as a sealed door until
   the room behind it exists. */
#define MP_WEST_X          (-2700)
#define MP_WEST_Z            5500     /* the art spans z[5400,5600] */
#define MP_WEST_TEXT_Y      (-186)   /* eye level on the y=0 floor */
#define MP_TEXT_RADIUS      1200
#define MP_FADE_NEAR         800
#define MP_TRIGGER_RADIUS    500

/* ---- THE WEST-ALCOVE DOOR -------------------------------------------------
   x=-3300, z[3200,3400], y[-400,0] — at the back of the west alcove (wall 7,
   x=-3299 over z[2399,4199], nx=+4096), in the middle of the room's west side.
   Out to THE ROOM OF BONES, at its one door.

   The west door's plane and approach again: YZ, approached from +X, mirror=0,
   the sign 11 proud of the wall along +X and the -200 on the Z argument. The
   two doors are 2800 apart in Manhattan terms, so they can never both be in
   reach on one frame. */
#define MP_WA_X            (-3300)
#define MP_WA_Z              3300     /* the art spans z[3200,3400] */

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int west_circle_prev = 1;
static int wa_circle_prev   = 1;

void meat_plant_arm(void) {
    west_circle_prev = interact_tapped();
    wa_circle_prev   = west_circle_prev;
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int meat_plant_west_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !west_circle_prev;
    int32_t dx, dz, xz;
    west_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - MP_WEST_X;
    dz = cam_z - MP_WEST_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= MP_TRIGGER_RADIUS) return 0;
    if (!interact_facing(MP_WEST_X, MP_WEST_Z)) return 0;
    return 1;
}

/* The door's floating sign. Same shape as every other sign in the game: opaque
   within MP_FADE_NEAR, gone by MP_TEXT_RADIUS. */
static void mp_west_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - MP_WEST_X;
    int32_t dz = cam_z - MP_WEST_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= MP_TEXT_RADIUS) return;

    if (xz > MP_FADE_NEAR) {
        int range = MP_TEXT_RADIUS - MP_FADE_NEAR;
        int prog  = xz - MP_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        MP_WEST_X + 11, MP_WEST_TEXT_Y, MP_WEST_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

/* The west-alcove door: the west door's test and sign, at the alcove's back. */
int meat_plant_west_alcove_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !wa_circle_prev;
    int32_t dx, dz, xz;
    wa_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - MP_WA_X;
    dz = cam_z - MP_WA_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= MP_TRIGGER_RADIUS) return 0;
    if (!interact_facing(MP_WA_X, MP_WA_Z)) return 0;
    return 1;
}

static void mp_west_alcove_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - MP_WA_X;
    int32_t dz = cam_z - MP_WA_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= MP_TEXT_RADIUS) return;

    if (xz > MP_FADE_NEAR) {
        int range = MP_TEXT_RADIUS - MP_FADE_NEAR;
        int prog  = xz - MP_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        MP_WA_X + 11, MP_WEST_TEXT_Y, MP_WA_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void meat_plant_spawn_west_alcove(void) {
    /* Back from the Room of Bones. 220 off the door plane (219 off wall 7) on
       its walkable +X side, on the door's centre line — 900 clear of both of
       the alcove's side walls, 3 and 18 — facing +X, the direction of travel,
       out of the alcove into the hall. The mass of legs' spine is at x=-2100,
       ~980 off, so the 195 push is quiet. */
    cam_x   = MP_WA_X + (MP_WALL_RADIUS + 25);
    cam_y   = MP_EYE_Y;
    cam_vy  = 0;
    cam_z   = MP_WA_Z;
    cam_rot = 1024;                    /* facing +X, east into the hall */
    meat_plant_arm();
}

void meat_plant_spawn_west(void) {
    /* 220 off wall 9 on its walkable +X side, on the door's centre line (500
       clear of the north wall 20), facing +X — the direction of travel, east
       along the north side of the hall. The nearest part of the mass of legs
       is its north-west step, (-2100,4600)..(-1900,4800), ~900 off, so the
       195 push is quiet. */
    cam_x   = MP_WEST_X + (MP_WALL_RADIUS + 25);
    cam_y   = MP_EYE_Y;
    cam_vy  = 0;
    cam_z   = MP_WEST_Z;
    cam_rot = 1024;                    /* facing +X, east into the room */
    meat_plant_arm();
}

void meat_plant_init(void) {
    meat_plant_collision_init(&current_collision_room);
    /* The vault, read off the VISUAL mesh: y=-800 over the whole hall, where
       the proxy's outer walls stop too. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(MP_WALL_RADIUS);

    mp_floor_zones_init();
    cam_pitch = 0;

    meat_plant_spawn_west();

    /* Save points, dressers, sconces, oil dispensers and cribs are global
       arrays, and not all of them are area-gated in every collide routine, so
       an instance left from another room would block invisibly anywhere it
       falls inside x[-3299,3299] z[0,6600]. Safe to clear: each owning room
       re-places its own on entry. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    cribs_clear();

    mp_view_resolve(1);
}

static void draw_meat_plant_smd(RenderContext *ctx) {
    if (!mp_smd) return;

    uint8_t *p = (uint8_t *)mp_smd->p_prims;
    int i, n = mp_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = mp_fog_far;   /* resolved by mp_view_resolve() this frame */
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
        SVECTOR *v0 = &mp_smd->p_verts[vi[0]];
        SVECTOR *v1 = &mp_smd->p_verts[vi[1]];
        SVECTOR *v2 = &mp_smd->p_verts[vi[2]];

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

        int nocull = (i < MEAT_PLANT_PRIM_COUNT) && meat_plant_nocull[i];
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
            v3 = &mp_smd->p_verts[vi[3]];
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
           must not be sorted over the legs standing on it. */
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
        int32_t fog = dist < mp_fog_near ? mp_fog_near : (dist > mp_fog_far ? mp_fog_far : dist);
        int32_t fog_factor = ((mp_fog_far - fog) << 8) / (mp_fog_far - mp_fog_near);

        uint8_t tex_idx = (i < MEAT_PLANT_PRIM_COUNT) ? meat_plant_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < MEAT_PLANT_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + MP_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + MP_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + MP_FOG_B * (256 - fog_factor)) >> 8);

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

void meat_plant_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    mp_view_resolve(0);

    g_fog_near = mp_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : mp_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, MP_FOG_R, MP_FOG_G, MP_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All three textures sit at Voff 0. */
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

    if (exp != DBG_EXP_NO_MESH) draw_meat_plant_smd(ctx);

    /* >>> LEVEL 8 REMOVES THE SIGN AND THE ENEMIES. <<< */
    if (exp != DBG_EXP_NO_ENTITIES) {
        mp_west_door_text(ctx);    /* west: YZ plane, approached from +X */
        mp_west_alcove_door_text(ctx);   /* west alcove: the same plane */
        /* BOTH CHAPTER 3 ENEMIES, drawn in every room of the chapter whether or
           not world.c places one here: the area tag makes an absent enemy free.
           Both sheets sit at Voff 128, so each is handed the window to restore
           after drawing unmasked. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
            lumberers_set_texwindow(&tw);
        }
        draw_crawlers(ctx);
        draw_lumberers(ctx);
        /* The Meat Sack. After the enemy draws, which restore the 128 window
           it samples under (Voff 64, src/menu.c). */
        item_pickups_draw(ctx);
    }
}
