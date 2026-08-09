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
#include "title.h"          /* current_area gate: the altar only exists here */
#include "attic_stairwell.h"
#include "collision.h"
#include "attic_stairwell_mesh_collision.h"
#include "attic_stairwell_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "concrete_props.h"
#include "hall_2f.h"
#include "conservatory.h"
#include "save_point.h"
#include "zombie.h"
#include "spider.h"
#include "rabisu.h"
#include "web.h"
#include "item_pickup.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Attic Stairwell: the floor above the East Stairwell. See attic_stairwell.h
   for the layout — a stair-head chamber, a main hall south of it, and a west
   room through a partition doorway, all flat at y=0. */

static SMD  *attic_stairwell_smd  = NULL;
static void *attic_stairwell_buff = NULL;

/* One flat zone per walkable region, all at y=0 (the seven floors detected in
   attic_stairwell_mesh_collision.c, with the two halves of the west doorway
   merged). The stair alcove west of x=-350 is deliberately left uncovered: it
   has no floor in the collision mesh and wall 3 keeps the player 195 clear of
   it. */
static void attic_stairwell_floor_zones_init(void) {
    /* Stair-head chamber. */
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -350;  floor_zones[0].max_x = 350;
    floor_zones[0].min_z = -350;  floor_zones[0].max_z = 350;
    floor_zones[0].y     = 0;

    /* Doorway in the chamber's south wall. */
    floor_zones[1].type  = FLOOR_FLAT;
    floor_zones[1].min_x = -150;  floor_zones[1].max_x = 200;
    floor_zones[1].min_z = -397;  floor_zones[1].max_z = -350;
    floor_zones[1].y     = 0;

    /* Main hall. */
    floor_zones[2].type  = FLOOR_FLAT;
    floor_zones[2].min_x = -1608; floor_zones[2].max_x = 351;
    floor_zones[2].min_z = -1399; floor_zones[2].max_z = -397;
    floor_zones[2].y     = 0;

    /* West corridor running north off the hall. */
    floor_zones[3].type  = FLOOR_FLAT;
    floor_zones[3].min_x = -1608; floor_zones[3].max_x = -1009;
    floor_zones[3].min_z = -1399; floor_zones[3].max_z = 349;
    floor_zones[3].y     = 0;

    /* Doorway through the partition into the west room. */
    floor_zones[4].type  = FLOOR_FLAT;
    floor_zones[4].min_x = -1655; floor_zones[4].max_x = -1608;
    floor_zones[4].min_z = -1200; floor_zones[4].max_z = -850;
    floor_zones[4].y     = 0;

    /* West room. */
    floor_zones[5].type  = FLOOR_FLAT;
    floor_zones[5].min_x = -2611; floor_zones[5].max_x = -1655;
    floor_zones[5].min_z = -1400; floor_zones[5].max_z = 349;
    floor_zones[5].y     = 0;

    floor_zone_count = 6;
}

/* ---- The altar (the con_tile block in the west room) -----------------------
   Modelled as part of the room mesh, so it needs no geometry or texture of its
   own here — only collision. It is collided as a PROP rather than as room walls:
   the wall routine holds the player 195 clear of every face, and with
   ITEM_PICKUP_RADIUS at 200 that put everything on the altar's top surface out
   of reach. A prop radius of 75 — what dining tables, dressers and the
   conservatory's concrete props already use for "things you walk right up to" —
   lets the player lean over it and take the two pickups world.c places there.

   Footprint is the collision mesh's (x[-2419,-2228], z[-542,-7]); the top face
   is the DRAWN y=-117, one the player can see over. The generated collision
   proxy claimed y[-346,0], nearly three times the drawn height, which would
   have stopped shots in mid-air above it. */
#define ALTAR_MIN_X   (-2419)
#define ALTAR_MAX_X   (-2228)
#define ALTAR_MIN_Z    (-542)
#define ALTAR_MAX_Z      (-7)
#define ALTAR_TOP_Y    (-117)   /* drawn top face; the base is the y=0 floor */
#define ALTAR_SLACK        4    /* absorbs the +/-1 between drawn and collision verts */

static int altar_rect_contains(int32_t x, int32_t z) {
    return x >= ALTAR_MIN_X - ALTAR_SLACK && x <= ALTAR_MAX_X + ALTAR_SLACK &&
           z >= ALTAR_MIN_Z - ALTAR_SLACK && z <= ALTAR_MAX_Z + ALTAR_SLACK;
}

/* Drop the altar's four faces out of the generated wall list, so only the prop
   push below acts on it. Matched by GEOMETRY (both endpoints inside the altar
   footprint), not by index: the generator renumbers walls whenever the mesh is
   re-exported, and no other wall in this room lies within that rect. */
static void altar_walls_remove(CollisionRoom *r) {
    int src, dst = 0;
    for (src = 0; src < r->wall_count; src++) {
        Wall *w = &r->walls[src];
        if (altar_rect_contains(w->x1, w->z1) && altar_rect_contains(w->x2, w->z2))
            continue;
        if (dst != src) r->walls[dst] = *w;
        dst++;
    }
    r->wall_count = dst;
}

/* Player push-out (Minkowski AABB, as the concrete props do, minus the rotation
   — the altar is axis-aligned). Gated to this room so the shared collision
   routine can call it unconditionally. */
void attic_stairwell_altar_collide(int32_t *px, int32_t py, int32_t *pz,
                                   int32_t radius) {
    (void)py;   /* single flat floor, and the altar is too tall to step onto */
    if (current_area != STATE_ATTIC_STAIRWELL) return;

    int32_t min_x = ALTAR_MIN_X - radius, max_x = ALTAR_MAX_X + radius;
    int32_t min_z = ALTAR_MIN_Z - radius, max_z = ALTAR_MAX_Z + radius;
    if (*px <= min_x || *px >= max_x) return;
    if (*pz <= min_z || *pz >= max_z) return;

    /* Push out along the axis with the smallest penetration. */
    int32_t push_l = *px - min_x, push_r = max_x - *px;
    int32_t push_f = *pz - min_z, push_b = max_z - *pz;

    int32_t min_push = push_l, dx = -push_l, dz = 0;
    if (push_r < min_push) { min_push = push_r; dx =  push_r; dz = 0; }
    if (push_f < min_push) { min_push = push_f; dx = 0; dz = -push_f; }
    if (push_b < min_push) {                    dx = 0; dz =  push_b; }

    *px += dx;
    *pz += dz;
}

/* Hitscan volume: the real block, no player standoff, height-aware — a shot
   passing over the altar's top face is not blocked. */
int attic_stairwell_altar_point_solid(int32_t x, int32_t y, int32_t z,
                                      int32_t slack) {
    if (current_area != STATE_ATTIC_STAIRWELL) return 0;
    if (y < ALTAR_TOP_Y || y > 0) return 0;          /* -Y is up */
    if (x < ALTAR_MIN_X - slack || x > ALTAR_MAX_X + slack) return 0;
    if (z < ALTAR_MIN_Z - slack || z > ALTAR_MAX_Z + slack) return 0;
    return 1;
}

/* ---- Per-room textures -----------------------------------------------------
   Seven mesh textures. Two are resident from startup (wd_flr with the kitchen,
   wd_dr with the fatdoor) and just need their headers captured. Four more live
   in time-shared slots owned by OTHER modules, so we call their narrow uploads
   rather than keeping a second RAM copy of each — texmgr has a hard TEXMGR_MAX
   and a registration past it fails SILENTLY, handing back tpage/clut 0 and
   breaking that texture in every room:

     - cncrte    -> the kchn_tile slot (x384 y0), via concrete_props. Already on
                    kitchen_restore_textures()' list.
     - upstairs  -> the DELIVERY-only rusty_fence slot (x704 y0), via hall_2f.
                    delivery_restore_textures() puts rusty_fence back.
     - strs      -> the stn_stl slot (x320 y0), via hall_2f. Already on
                    kitchen_restore_textures()' list.
     - con_tile  -> the DELIVERY-only double_door slot (x832 y0), via the
                    conservatory's new narrow con_tile upload. Restored by
                    delivery_restore_textures() and kitchen_restore_textures().

   trck_clue (the picture on the hall's south wall) is this room's only NEW art.
   It time-shares the DELIVERY-only gravel slot (x640 y0) that the conservatory
   already streams trees over and the East Stairwell chnlnk over, so it adds no
   new restore obligation. It is 8bpp, so it covers the whole x[640,704) tpage
   rather than the 4bpp trio's x[640,672) half — the extra columns were empty.

   All seven textures sit at Voff 0, so the one 128 texture window set in
   attic_stairwell_draw serves them all (see tools/VRAM_MAP.txt). */
#define ATTIC_STAIRWELL_TEX_COUNT 7

/* Streamed slots we own a RAM copy of: engine slot -> texmgr id. Only
   trck_clue — every other texture here is already registered by the module that
   owns it. */
#define ATTIC_STAIRWELL_NEW_TEX 1
static int new_tex_id[ATTIC_STAIRWELL_NEW_TEX];
static const struct { const char *file; int slot; } new_tex[ATTIC_STAIRWELL_NEW_TEX] = {
    { "\\TEX\\TRCKCLUE.TIM;1", 6 },
};

static uint16_t tex_tpage[ATTIC_STAIRWELL_TEX_COUNT];
static uint16_t tex_clut[ATTIC_STAIRWELL_TEX_COUNT];

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale. */
void attic_stairwell_load_geometry(void) {
    attic_stairwell_buff = room_arena_load("\\TEX\\ATTCSTRW.SMD;1");
    attic_stairwell_smd  = attic_stairwell_buff ? smdInitData(attic_stairwell_buff) : NULL;
}

/* Register this room's streamed textures at STARTUP. Geometry is NOT loaded
   here any more — see attic_stairwell_load_geometry above — but the texmgr registrations
   still are: they keep a RAM copy so the entry-time upload is a pure LoadImage
   (tools/TEXTURING_NOTES.txt). */
void attic_stairwell_load_assets(void) {
    /* Streamed slots we own: RAM-resident via the texture manager, uploaded on
       entry. */
    for (int i = 0; i < ATTIC_STAIRWELL_NEW_TEX; i++) {
        int slot = new_tex[i].slot;
        new_tex_id[i]   = texmgr_register(new_tex[i].file);
        tex_tpage[slot] = texmgr_tpage(new_tex_id[i]);
        tex_clut[slot]  = texmgr_clut(new_tex_id[i]);
    }

    /* Owned by other modules, all of whose uploads attic_stairwell_upload_textures
       calls: concrete_props (cncrte), the 2F hall (upstairs, strs) and the
       conservatory (con_tile). Header only — no LoadImage, no second RAM copy. */
    TIM_SLOT(0, CNCRTE);
    TIM_SLOT(3, UPSTAIRS);
    TIM_SLOT(4, STRS);
    TIM_SLOT(5, CONTILE);

    /* Resident from startup (kitchen + fatdoor). */
    TIM_SLOT(1, WDFLR);
    TIM_SLOT(2, WDDR);
}

/* Upload the streamed textures from their resident RAM copies. Pure LoadImage
   — no CD access — safe during the room transition (the caller DrawSyncs first,
   as main's STATE_LOADING does). */
void attic_stairwell_upload_textures(void) {
    for (int i = 0; i < ATTIC_STAIRWELL_NEW_TEX; i++)
        texmgr_upload(new_tex_id[i]);       /* trck_clue -> gravel slot       */
    concrete_props_upload_textures();       /* cncrte    -> kchn_tile slot    */
    hall_2f_upload_upstairs();              /* upstairs  -> rusty_fence slot  */
    hall_2f_upload_strs();                  /* strs      -> stn_stl slot      */
    conservatory_upload_con_tile();         /* con_tile  -> double_door slot  */
}

/* ---- The stairs back down to the East Stairwell ----------------------------
   The chamber's west wall (x=-350) backs onto the modelled stair alcove, whose
   treads drop west across z[-175,175]. The player stands in the chamber, on the
   +X side, so the floating sign lies in the YZ plane with mirror=0 (the mirror
   image of the East Stairwell's, which is read from -X). A fresh Circle press
   in range hands off to the same stair-climb transition the conservatory and
   the 2F hall use. */
#define ASTAIRS_X               (-350)   /* the alcove wall (stair head)        */
#define ASTAIRS_Z                   0    /* centre of the stair width           */
#define ASTAIRS_TEXT_Y          (-186)
#define ASTAIRS_TEXT_RADIUS      1500
#define ASTAIRS_FADE_NEAR        1000
#define ASTAIRS_TRIGGER_RADIUS    350
#define ASTAIRS_TEXT_PIXEL          2    /* smaller so the line fits the stairs */

/* Circle edge-detect, seeded by attic_stairwell_stairs_arm(). Starts "held" so
   a press carried in through the transition doesn't bounce the player straight
   back down. */
static int stairs_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void attic_stairwell_stairs_arm(void) {
    stairs_circle_prev = circle_held();
}

int attic_stairwell_stairs_triggered(void) {
    int held = circle_held();
    int just = held && !stairs_circle_prev;
    stairs_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - ASTAIRS_X;
    int32_t dz = cam_z - ASTAIRS_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < ASTAIRS_TRIGGER_RADIUS && interact_facing(ASTAIRS_X, ASTAIRS_Z);
}

static void stairs_text(RenderContext *ctx) {
    int32_t dx = cam_x - ASTAIRS_X;
    int32_t dz = cam_z - ASTAIRS_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= ASTAIRS_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > ASTAIRS_FADE_NEAR) {
        int range = ASTAIRS_TEXT_RADIUS - ASTAIRS_FADE_NEAR;
        int prog  = xz - ASTAIRS_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* YZ plane: door_draw_string_3d centres the reading axis (Z) on world_z
       after adding 200, so pass ASTAIRS_Z - 200. Sits just east (x+11) of the
       alcove wall so it floats in front of the descent. The player views it
       from the +X (chamber) side, so mirror=0. */
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to descend",
                        ASTAIRS_X + 11, ASTAIRS_TEXT_Y, ASTAIRS_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, ASTAIRS_TEXT_PIXEL);
}

/* ---- The west room's north-wall door, through to the Attic Exit ------------
   The wd_dr poly in "Attic Stairwell.smx" sits at x[-2037,-1846], z=350 (the
   drawn wall; collision wall 4 behind it is at z=349, normal -Z). Its centre is
   x=-1942. It maps to the Attic Exit's south-wall door at x=-400, z=-1000 —
   the meshes share a world, offset by exit_x = stairwell_x + 1542.

   The player approaches from the -Z (room) side, so the sign lies in the XY
   plane with mirror=0, the same orientation as the Master Bedroom's. */
#define AEXITDOOR_X           (-1942)
#define AEXITDOOR_Z              350
#define AEXITDOOR_TEXT_Y       (-186)
#define AEXITDOOR_TEXT_RADIUS   1500
#define AEXITDOOR_FADE_NEAR     1000
#define AEXITDOOR_TRIGGER_RADIUS 500

/* Circle edge-detect, seeded by attic_stairwell_exit_door_arm(). Starts "held"
   so a press carried in through the transition doesn't bounce the player
   straight back out. */
static int exit_door_circle_prev = 1;

void attic_stairwell_exit_door_arm(void) {
    exit_door_circle_prev = circle_held();
}

int attic_stairwell_exit_door_triggered(void) {
    int held = circle_held();
    int just = held && !exit_door_circle_prev;
    exit_door_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - AEXITDOOR_X;
    int32_t dz = cam_z - AEXITDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < AEXITDOOR_TRIGGER_RADIUS;
}

/* Floating sign on that door. XY plane: door_draw_string_3d centres the reading
   axis (X) on world_x after adding 200, so pass door_x - 200. Sits just south
   (z-11) of the wall so it floats in front of the door. */
static void exit_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - AEXITDOOR_X;
    int32_t dz = cam_z - AEXITDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= AEXITDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > AEXITDOOR_FADE_NEAR) {
        int range = AEXITDOOR_TEXT_RADIUS - AEXITDOOR_FADE_NEAR;
        int prog  = xz - AEXITDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        AEXITDOOR_X - 200, AEXITDOOR_TEXT_Y, AEXITDOOR_Z - 11,
                        50, 255, 50, fade, 0, TEXT_PLANE_XY, DOOR_PIXEL_SIZE);
}

/* Arriving at the top of the stairs: stand east of the alcove wall, far enough
   clear of the 195 push radius apply_collision_reception uses, facing +X — the
   direction of travel out of the climb, with the stairs behind. Both
   interactions are armed, so a Circle held through the transition can't fire
   the nearest trigger. */
void attic_stairwell_spawn_stairs(void) {
    cam_x   = -120;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = 0;
    cam_rot = 1024;   /* facing +X, into the chamber */
    attic_stairwell_stairs_arm();
    attic_stairwell_exit_door_arm();
}

/* Arriving back from the Attic Exit: stand south of the z=349 wall, clear of
   the 195 push radius, facing -Z — the direction of travel through the door. */
void attic_stairwell_spawn_exit_door(void) {
    cam_x   = AEXITDOOR_X;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = 120;
    cam_rot = 2048;   /* facing -Z, into the west room */
    attic_stairwell_stairs_arm();
    attic_stairwell_exit_door_arm();
}

void attic_stairwell_init(void) {
    attic_stairwell_collision_init(&current_collision_room);
    /* The altar is collided as a prop (see above), not as room walls — take its
       four faces back out of the freshly-installed wall list. */
    altar_walls_remove(&current_collision_room);
    /* Proxy walls top out at y=-519, one unit shy of the drawn ceiling; state
       the DRAWN value so ceiling-mounted enemies hang flush with the roof the
       player can see. */
    collision_set_ceiling_y(-520);
    attic_stairwell_floor_zones_init();

    /* Default spawn: the stair head (main.c overrides it for an arrival through
       the Attic Exit's door). */
    attic_stairwell_spawn_stairs();

    /* Reception's save point and dresser prop are global (not room-swapped) and
       neither is area-gated in its collide routine, so reception's instances
       would block the player invisibly inside this room's bounds — the save
       point sits at (78,-67), right in the middle of the stair-head chamber.
       Clearing them is safe: reception_init() re-places both on every reception
       entry. */
    save_points_clear();
    dressers_clear();
}

static void draw_attic_stairwell_smd(RenderContext *ctx) {
    if (!attic_stairwell_smd) return;

    uint8_t *p = (uint8_t *)attic_stairwell_smd->p_prims;
    int i;

    for (i = 0; i < attic_stairwell_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &attic_stairwell_smd->p_verts[vi[0]];
        SVECTOR *v1 = &attic_stairwell_smd->p_verts[vi[1]];
        SVECTOR *v2 = &attic_stairwell_smd->p_verts[vi[2]];

        {
            int32_t dx = (int32_t)v0->vx - cam_x;
            int32_t dz = (int32_t)v0->vz - cam_z;
            /* Distance cull (Manhattan) at the fog-out distance so culled polys
               are already invisible (same budget as the other rooms). */
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

        /* Backface cull, except degenerate (triangle-shaped) quads flagged at
           build time in attic_stairwell_nocull — same scheme as the other rooms. */
        int nocull = (i < ATTIC_STAIRWELL_PRIM_COUNT) && attic_stairwell_nocull[i];
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
            v3 = &attic_stairwell_smd->p_verts[vi[3]];
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
        /* Horizontal polys sort by their farthest corner, not their average,
           so floors stay behind whatever stands on them (see render.h). */
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
        int32_t fog_start = 350, fog_end = 1500;   /* fog saturates at the cull distance */
        int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
        int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in attic_stairwell_draw. */
        uint8_t tex_idx = (i < ATTIC_STAIRWELL_PRIM_COUNT) ? attic_stairwell_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < ATTIC_STAIRWELL_TEX_COUNT);
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

void attic_stairwell_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = 350; g_fog_far = 1500;

    /* Dark interior background, same as the other rooms. */
    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setXY0(bg, 0, 0);
    setWH(bg, SCREEN_XRES, SCREEN_YRES);
    setRGB0(bg, 20, 15, 10);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
    ctx->next_packet += sizeof(TILE);

    /* 128x128 texture window so per-poly UVs wrap (tile) within each texture's
       page. All seven attic textures sit at page-top (Voff 0), so one window
       serves them (see tools/VRAM_MAP.txt). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    /* View matrix from the camera (same construction as the other rooms). */
    MATRIX rot_matrix;
    camera_build_view(&rot_matrix);

    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    draw_attic_stairwell_smd(ctx);

    /* No enemies placed here yet; the room's 128 texture window is still handed
       to the sprite renderers so a future spawn brackets its Voff>=128 sprite
       correctly (see tools/TEXTURING_NOTES.txt PART 5). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
        spiders_set_texwindow(&tw);
    }
    draw_zombies(ctx);
    draw_spiders(ctx);
    draw_rabisus(ctx);
    webs_draw(ctx);
    item_pickups_draw(ctx);

    stairs_text(ctx);
    exit_door_text(ctx);
}
