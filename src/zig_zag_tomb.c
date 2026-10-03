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
#include "zig_zag_tomb.h"
#include "lumberer.h"
#include "maggot.h"
#include "crawler.h"
#include "collision.h"
#include "room_data.h"
#include "zig_zag_tomb_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the three narrow uploaders this room borrows */
#include "bars.h"               /* ...and the bars'                              */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons, game_flag */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* The Zig Zag Tomb — see zig_zag_tomb.h for the layout, the zig-zag and the
   doors. */

static SMD  *zz_smd  = NULL;
static void *zz_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   THE UP DOWN MAZE'S NUMBERS (src/up_down_maze.c says how they were chosen):
   366/1300 at rest, and two equal ~46% bonuses for the lantern, so the three
   sight lines are 1300 / 1900 / 2500 — even steps of 600, resolving to 1300,
   1899 and 2498 in the integer arithmetic. 366/1300 keeps the 450/1600 ramp's
   shape. At 1300 a lane shows two cells and then the fog, so the room's
   Lumberers come out of it close; the Helluminator buys the long view down a
   lane, and costs oil for it.

   These were the Sliding Bars Room's 450/1600 with +50%/+50% (1600 / 2400 /
   3200) until the room was given the maze's shorter reach. Shorter only
   lightens the draw: cull and fog-far are equal, so less mesh is walked. */
#define ZZ_BASE_FOG_NEAR   366
#define ZZ_BASE_FOG_FAR   1300

#define ZZ_VIEW_UNIT        256
#define ZZ_VIEW_HELL_BONUS  118   /* ~+46% while the lantern is in hand   */
#define ZZ_VIEW_BURN_BONUS  118   /* ~+46% more while it is actually lit  */
#define ZZ_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t zz_view     = ZZ_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t zz_fog_near = ZZ_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t zz_fog_far  = ZZ_BASE_FOG_FAR;

static int32_t zz_view_target(void) {
    int32_t s = ZZ_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += ZZ_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += ZZ_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void zz_view_resolve(int snap) {
    int32_t target = zz_view_target();
    if (snap) {
        zz_view = target;
    } else if (zz_view < target) {
        zz_view += ZZ_VIEW_RATE;
        if (zz_view > target) zz_view = target;
    } else if (zz_view > target) {
        zz_view -= ZZ_VIEW_RATE;
        if (zz_view < target) zz_view = target;
    }
    zz_fog_near = (ZZ_BASE_FOG_NEAR * zz_view) >> 8;
    zz_fog_far  = (ZZ_BASE_FOG_FAR  * zz_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). */
#define ZZ_FOG_R             7
#define ZZ_FOG_G             6
#define ZZ_FOG_B             9

/* Wall standoff. The chapter's 195. Every lane is 600 wide, which leaves a 210
   band down the middle of each. */
#define ZZ_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define ZZ_FLOOR_Y            0
#define ZZ_EYE_Y           (ZZ_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zone ------------------------------------------------------------
   ONE, FLAT, AT y=0, over the proxy's one floor face exactly — the whole
   square (see the FLOOR list at the foot of src/zig_zag_tomb_mesh_collision.c).
   The blocks stand on it; their walls keep the player out of them. */
static void zz_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x =    0; floor_zones[0].max_x = 4200;
    floor_zones[0].min_z =    0; floor_zones[0].max_z = 4200;
    floor_zones[0].y     = ZZ_FLOOR_Y;
    floor_zone_count = 1;
}

/* ---- Textures --------------------------------------------------------------
   FOUR, AND THE ROOM OWNS NONE.

     0 cobblestones        the walls, the floor and the blocks — 956 of the
                           1260 polys                             (x384 y0)
     1 catacomb inner door the three doorways                     (x832 y0)
     2 loculus             the burial niches in the blocks' faces
     3 bars                the nine barred partitions, 4bpp       (x512 y256)

   The first three come through src/catacombs_entry.c's narrow uploaders, as
   every Chapter 3 room takes them, and the bars through bars_upload_texture() —
   the Sliding Bars Room's set less its incinerator panel.

   All sit at Voff 0, so the one 128 texture window in the draw serves them. */
#define ZIG_ZAG_TOMB_TEX_COUNT 4

static uint16_t tex_tpage[ZIG_ZAG_TOMB_TEX_COUNT];
static uint16_t tex_clut[ZIG_ZAG_TOMB_TEX_COUNT];

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/cleaver_l.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int zz_key_count = 0;

static void zz_build_cull_keys(void) {
    zz_key_count = 0;
    if (!zz_smd) return;
    uint8_t *p = (uint8_t *)zz_smd->p_prims;
    int i, n = zz_smd->n_prims;
    if (n > ZIG_ZAG_TOMB_PRIM_COUNT) n = ZIG_ZAG_TOMB_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &zz_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    zz_key_count = n;
}

void zig_zag_tomb_load_geometry(void) {
    zz_buff = room_arena_load("\\TEXCTCMB\\ZIGZAGTB.SMD;1");
    zz_smd  = zz_buff ? smdInitData(zz_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of
       the same file (src/room_data.h). No block, no room. */
    if (zz_smd && !room_data_bind(zz_smd->n_prims)) zz_smd = NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    zz_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: four compile-time headers and no
   registration at all, so no texmgr_set_bank() either. The bank this room's art
   is in is decided by its owners'. */
void zig_zag_tomb_load_assets(void) {
    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, CTCMBDR);
    TIM_SLOT(2, LOCULUS);
    TIM_SLOT(3, BARS);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).
   Four pages, none shared with another here, so no ordering rule. */
void zig_zag_tomb_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    catacombs_entry_upload_loculus();
    bars_upload_texture();
}

/* ---- THE NORTH DOOR --------------------------------------------------------
   z=4200, x[200,400], y[-400,0] — in the north wall at its west end. Back into
   the Crucifix Corridor, through the south door at the foot of its cross arm.

   A door in the XY plane at fixed Z, approached from -Z (wall 4 runs z=4200
   with nz=-4095), so TEXT_PLANE_XY with mirror=0, the sign 11 proud of the wall
   along -Z, and the -200 door_draw_string_3d wants on the X argument (the
   reading axis for an XY sign). The Crucifix Corridor's south door, the far
   side, takes the opposite pair. */
#define ZZ_NORTH_X           300     /* the art spans x[200,400] */
#define ZZ_NORTH_Z          4200
#define ZZ_NORTH_TEXT_Y     (-186)   /* eye level on the y=0 floor */

/* ---- THE EAST DOOR ---------------------------------------------------------
   x=4200, z[1400,1600], y[-400,0] — in the east wall, at the east end of the
   lane between the z~900 and z~2100 rows of bars. Out to CLEAVER L, through the
   west door at the far end of its east-west shaft.

   In the YZ plane at fixed X, approached from -X (wall 32 runs x=4199 with
   nx=-4095, so the walkable side is -X): TEXT_PLANE_YZ with mirror=1, the sign
   11 proud of the wall along -X, and the -200 on the Z argument.

   >>> LOCKED FROM THE OTHER SIDE until Cleaver L unlocks it — the Up Down
   Maze's north door and the Cleaver Corridor exactly, on FLAG_ZIG_ZAG_DOOR. <<<

   THE SOUTH DOOR, z=0 x[200,400], is drawn and nothing else: no sign, no
   trigger. It reads as a sealed door until the room behind it exists. */
#define ZZ_EAST_X           4200
#define ZZ_EAST_Z           1500     /* the art spans z[1400,1600] */
#define ZZ_EAST_TEXT_Y      (-186)   /* eye level on the y=0 floor */

#define ZZ_TEXT_RADIUS      1200
#define ZZ_FADE_NEAR         800
#define ZZ_TRIGGER_RADIUS    500

/* Circle edge-detect, one per door. Seeded "held" by the arm below so a press
   carried in through the transition cannot fire on the arrival frame. */
static int north_circle_prev = 1;
static int east_circle_prev  = 1;

void zig_zag_tomb_arm(void) {
    int held = interact_tapped();
    north_circle_prev = held;
    east_circle_prev  = held;
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. Each
   door keeps its OWN `prev`, and main.c calls both every frame. */
static int zz_door_triggered(int lock, int *circle_prev,
                             int32_t door_x, int32_t door_z) {
    int held = interact_tapped();
    int just = held && !*circle_prev;
    int32_t dx, dz, xz;
    *circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - door_x;
    dz = cam_z - door_z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= ZZ_TRIGGER_RADIUS) return 0;
    if (!interact_facing(door_x, door_z)) return 0;
    return 1;
}

int zig_zag_tomb_north_door_triggered(int lock) {
    return zz_door_triggered(lock, &north_circle_prev, ZZ_NORTH_X, ZZ_NORTH_Z);
}

/* Tested AFTER the shared trigger so the edge state still advances while it is
   locked — the Up Down Maze's north door's order, for its reason. */
int zig_zag_tomb_east_door_triggered(int lock) {
    return zz_door_triggered(lock, &east_circle_prev, ZZ_EAST_X, ZZ_EAST_Z) &&
           game_flag(FLAG_ZIG_ZAG_DOOR);
}

/* A door's floating sign. Same shape as every door sign in the game: opaque
   within ZZ_FADE_NEAR, gone by ZZ_TEXT_RADIUS. The caller passes the string's
   already-placed position and the plane/mirror for its wall — and `locked`,
   which swaps the prompt for Reception's red "Locked from the other side". */
static void zz_door_text(RenderContext *ctx, int32_t door_x, int32_t door_z,
                         int32_t tx, int32_t ty, int32_t tz,
                         int mirror, int plane, int locked) {
    int32_t dx = cam_x - door_x;
    int32_t dz = cam_z - door_z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= ZZ_TEXT_RADIUS) return;

    if (xz > ZZ_FADE_NEAR) {
        int range = ZZ_TEXT_RADIUS - ZZ_FADE_NEAR;
        int prog  = xz - ZZ_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    if (locked) {
        door_draw_string_3d(ctx, "Locked from the other side",
                            tx, ty, tz, 255, 50, 50, fade, mirror, plane,
                            DOOR_PIXEL_SIZE);
        return;
    }
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        tx, ty, tz, 50, 255, 50, fade, mirror, plane,
                        DOOR_PIXEL_SIZE);
}

static void zz_north_door_text(RenderContext *ctx) {
    zz_door_text(ctx, ZZ_NORTH_X, ZZ_NORTH_Z,
                 ZZ_NORTH_X - 200, ZZ_NORTH_TEXT_Y, ZZ_NORTH_Z - 11,
                 0, TEXT_PLANE_XY, 0);   /* mirror=0: XY door approached from -Z */
}

static void zz_east_door_text(RenderContext *ctx) {
    zz_door_text(ctx, ZZ_EAST_X, ZZ_EAST_Z,
                 ZZ_EAST_X - 11, ZZ_EAST_TEXT_Y, ZZ_EAST_Z - 200,
                 1, TEXT_PLANE_YZ,       /* mirror=1: YZ door approached from -X */
                 !game_flag(FLAG_ZIG_ZAG_DOOR));
}

void zig_zag_tomb_spawn_north(void) {
    /* In from the Crucifix Corridor. 220 off wall 4 on its walkable -Z side, on
       the door's centre line (the west lane's, 300 clear of the west wall and
       of the first block's face), facing -Z — the direction of travel, south.
       The z~3300 bars across this lane are 580 further on. */
    cam_x   = ZZ_NORTH_X;
    cam_y   = ZZ_EYE_Y;
    cam_vy  = 0;
    cam_z   = ZZ_NORTH_Z - (ZZ_WALL_RADIUS + 25);
    cam_rot = 2048;                    /* facing -Z, south */
    zig_zag_tomb_arm();
}

void zig_zag_tomb_spawn_east(void) {
    /* Back from Cleaver L. 220 off wall 32 on its walkable -X side, on the
       door's centre line (the lane's, 300 clear of the blocks either side),
       facing -X — the direction of travel, west along the lane. */
    cam_x   = ZZ_EAST_X - (ZZ_WALL_RADIUS + 25);
    cam_y   = ZZ_EYE_Y;
    cam_vy  = 0;
    cam_z   = ZZ_EAST_Z;
    cam_rot = 3072;                    /* facing -X, west */
    zig_zag_tomb_arm();
}

void zig_zag_tomb_init(void) {
    room_data_collision(&current_collision_room);
    /* The walls' tops, y=-800: there is no vault mesh, Cleaver L's open dark. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(ZZ_WALL_RADIUS);

    zz_floor_zones_init();
    cam_pitch = 0;

    zig_zag_tomb_spawn_north();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every collide routine, so an
       instance left from another room would block invisibly anywhere it falls
       inside the square. Safe to clear: every room that has one re-places its
       own on entry. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    zz_view_resolve(1);
}

static void draw_zig_zag_tomb_smd(RenderContext *ctx) {
    if (!zz_smd) return;

    uint8_t *p = (uint8_t *)zz_smd->p_prims;
    int i, n = zz_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = zz_fog_far;   /* resolved by zz_view_resolve() this frame */
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
        SVECTOR *v0 = &zz_smd->p_verts[vi[0]];
        SVECTOR *v1 = &zz_smd->p_verts[vi[1]];
        SVECTOR *v2 = &zz_smd->p_verts[vi[2]];

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

        int nocull = (i < ZIG_ZAG_TOMB_PRIM_COUNT) && room_nocull(i);
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
            v3 = &zz_smd->p_verts[vi[3]];
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
        /* Horizontal polys sort by their farthest corner (render.h), so the
           floor stays behind whatever stands on it. */
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
        int32_t fog = dist < zz_fog_near ? zz_fog_near : (dist > zz_fog_far ? zz_fog_far : dist);
        int32_t fog_factor = ((zz_fog_far - fog) << 8) / (zz_fog_far - zz_fog_near);

        uint8_t tex_idx = (i < ZIG_ZAG_TOMB_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < ZIG_ZAG_TOMB_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + ZZ_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + ZZ_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + ZZ_FOG_B * (256 - fog_factor)) >> 8);

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

void zig_zag_tomb_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    zz_view_resolve(0);

    g_fog_near = zz_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : zz_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, ZZ_FOG_R, ZZ_FOG_G, ZZ_FOG_B);

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

    if (exp != DBG_EXP_NO_MESH) draw_zig_zag_tomb_smd(ctx);

    /* >>> LEVEL 8 REMOVES THE SIGNS AND THE ENEMIES. <<< Nothing else stands
       in this room. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        zz_north_door_text(ctx);   /* north: XY plane, approached from -Z */
        zz_east_door_text(ctx);    /* east:  YZ plane, approached from -X */
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
