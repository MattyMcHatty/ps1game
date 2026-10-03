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
#include "the_shelf.h"
#include "lumberer.h"
#include "maggot.h"
#include "crawler.h"
#include "collision.h"
#include "room_data.h"
#include "the_shelf_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "north_chamber.h"      /* ...the ladder's                            */
#include "bars.h"               /* ...and the bars'                           */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons             */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */
#include "sliding_bars_room.h"  /* the reset button: sliding_bars_room_reset_gates */

/* The Shelf — see the_shelf.h for the layout, the bars and the ways out. */

static SMD  *ts_smd  = NULL;
static void *ts_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY, copied from src/h_corridor.c with its numbers:
   cull and fog-far are equal, the base is scaled by what the player is
   carrying, and the scale eases rather than jumping. 366/1300 at rest and two
   equal ~46% bonuses for the lantern, so the sight lines are 1300 / 1900 /
   2500. The H Corridor's and the Up Down Maze's reach, so climbing the ladder
   or walking through the door does not change how far the player can see —
   and the Lumberers come out of the fog close, down a 3600-long corridor. */
#define TS_BASE_FOG_NEAR   366
#define TS_BASE_FOG_FAR   1300

#define TS_VIEW_UNIT        256
#define TS_VIEW_HELL_BONUS  118   /* ~+46% while the lantern is in hand   */
#define TS_VIEW_BURN_BONUS  118   /* ~+46% more while it is actually lit  */
#define TS_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t ts_view     = TS_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t ts_fog_near = TS_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t ts_fog_far  = TS_BASE_FOG_FAR;

static int32_t ts_view_target(void) {
    int32_t s = TS_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += TS_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += TS_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void ts_view_resolve(int snap) {
    int32_t target = ts_view_target();
    if (snap) {
        ts_view = target;
    } else if (ts_view < target) {
        ts_view += TS_VIEW_RATE;
        if (ts_view > target) ts_view = target;
    } else if (ts_view > target) {
        ts_view -= TS_VIEW_RATE;
        if (ts_view < target) ts_view = target;
    }
    ts_fog_near = (TS_BASE_FOG_NEAR * ts_view) >> 8;
    ts_fog_far  = (TS_BASE_FOG_FAR  * ts_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). */
#define TS_FOG_R             7
#define TS_FOG_G             6
#define TS_FOG_B             9

/* Wall standoff. The chapter's 195. The narrowest walkway is the barred
   corridors' 666, which leaves a 276 band down the middle of each. */
#define TS_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define TS_FLOOR_Y            0
#define TS_EYE_Y           (TS_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zone ------------------------------------------------------------
   ONE, FLAT, AT y=0, over the bounding box of the proxy's three floor faces
   (the hall and the two legs of the east passage — see the FLOOR list at the
   foot of src/the_shelf_mesh_collision.c). All three are the one plane, and the
   walls keep the player out of the box's empty corner north-east of the hall
   and out of the shaft, so one zone serves the whole room. */
static void ts_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -4800; floor_zones[0].max_x =  600;
    floor_zones[0].min_z = -2200; floor_zones[0].max_z =  400;
    floor_zones[0].y     = TS_FLOOR_Y;
    floor_zone_count = 1;
}

/* ---- Textures --------------------------------------------------------------
   FIVE, AND THE ROOM OWNS ONE.

     0 cobblestones        the walls, the floor and the shaft — 703 of the
                           939 polys                              (x384 y0)
     1 catacomb inner door the north door                         (x832 y0)
     2 ladder              down the shaft's south wall, 2 polys
                           (x704 y256, the North Chamber's, on the
                           incinerator's page; 'ladder_128' in the
                           export, the same art)
     3 incinerator         ONE panel on the west wall          (x640 y0)
                           OWNED HERE — see below
     4 bars                the two barred partitions, 4bpp     (x512 y256)

   The nine untextured polys are the black floor of the shaft at y=+1200; the
   tex map gives them 0xFF and they draw flat.

   >>> WHY THE INCINERATOR IS A COPY. <<< incinerator.tim and ladder.tim are
   the chapter's first in-bank time-share: both are 128x128 8bpp at x704 y256,
   and they share the CLUT line (304,511) too, because until this room no room
   drew both. This one does, so whichever went up second would paint the other.
   SHLFINCN.TIM is incinerator.tim byte for byte with its two header addresses
   moved to the arms page (x640 y0) and the arms' palette line (672,501) — the
   Room of Torsos' terms exactly, and a sixth owner taking turns there. Nothing
   else in this room draws x640 y0, and every room that does (the Rooms of
   Arms, Heads, Legs, Bones and Torsos, and the Meat Plant for the legs) puts
   its own pixels and CLUT line back on entry.

   Slots 0, 1, 2 and 4 come through their owners' narrow uploaders, as every
   Chapter 3 room takes them. All five sit at Voff 0, so the one 128 texture
   window in the draw serves them. */
#define THE_SHELF_TEX_COUNT 5

static uint16_t tex_tpage[THE_SHELF_TEX_COUNT];
static uint16_t tex_clut[THE_SHELF_TEX_COUNT];

/* The registration. Unloaded until area_bank_sync() reads the Catacombs bank;
   an upload before that is a silent no-op, and the panel draws as whatever was
   last on x640 y0. */
static int incin_tex = -1;

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/h_corridor.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int ts_key_count = 0;

static void ts_build_cull_keys(void) {
    ts_key_count = 0;
    if (!ts_smd) return;
    uint8_t *p = (uint8_t *)ts_smd->p_prims;
    int i, n = ts_smd->n_prims;
    if (n > THE_SHELF_PRIM_COUNT) n = THE_SHELF_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &ts_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    ts_key_count = n;
}

void the_shelf_load_geometry(void) {
    ts_buff = room_arena_load("\\TEXCTCMB\\THESHELF.SMD;1");
    ts_smd  = ts_buff ? smdInitData(ts_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of
       the same file (src/room_data.h). No block, no room. */
    if (ts_smd && !room_data_bind(ts_smd->n_prims)) ts_smd = NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    ts_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: four compile-time headers and ONE
   deferred registration. The bank mask is derived, not guessed — py
   tools/check_tex_banks.py walks the uploader graph and fails if CATACOMBS is
   not in it. */
void the_shelf_load_assets(void) {
    texmgr_set_bank(TEXBANK_CATACOMBS);
    incin_tex = texmgr_register("\\TEXCTCMB\\SHLFINCN.TIM;1");

    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, CTCMBDR);
    TIM_SLOT(2, LADDER);
    TIM_SLOT(3, SHLFINCN);
    TIM_SLOT(4, BARS);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   >>> TWO LINES HERE OVERWRITE SOMEBODY ELSE'S PAGE. <<< The ladder overwrites
   the incinerator, as it does in the North Chamber and the H Corridor, and the
   panel overwrites the arms (or the heads, legs, bones or torsos). Nothing
   else in this room draws either page, and the owners' own uploaders put them
   back on the way in to every room that does, so there is no ordering rule
   between the five calls. */
void the_shelf_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    north_chamber_upload_ladder();
    bars_upload_texture();
    texmgr_upload(incin_tex);
}

/* ---- THE NORTH DOOR --------------------------------------------------------
   z=400, x[-2400,-2200], y[-400,0] — in the north wall, halfway along the
   north corridor. Out to THE UP DOWN MAZE, through the south door on its upper
   storey.

   A door in the XY plane at fixed Z, approached from -Z (wall 15 runs z=400
   with nz=-4096), so TEXT_PLANE_XY with mirror=0, the sign 11 proud of the wall
   along -Z, and the -200 door_draw_string_3d wants on the X argument (the
   reading axis for an XY sign). The maze's south-upper door, the far side,
   takes the opposite pair.

   ---- THE LADDER ----
   z=0, x[200,400], running DOWN the shaft's south wall from the passage floor.
   Down to THE H CORRIDOR, onto the floor at the foot of its ladder.

   The prompt stands at the shaft's edge, over the top rung, at eye level. The
   edge is z=0 — wall 0, nz=-4096, approached from -Z — so it is an XY-plane
   sign with mirror=0, 11 proud of the edge along -Z, and the -200 on the X
   argument: the north door's pair, on the other side of the room. Wall 0 holds
   the player at z<=-195, well inside the 500 trigger.

   The two are 3000 apart in Manhattan terms, so they can never share a
   trigger.

   ---- THE RESET BUTTON ----
   x=-4800, z[-1000,-800], y[-300,-100] — the one incinerator panel, on the
   west wall at the west end, halfway between the two rows of bars. It is THE
   SLIDING BARS ROOM'S RESET BUTTON (the "not built yet" one in
   src/sliding_bars_room.c's THE GATES): a press puts all four gates back to
   their start spots. It works every time; there is nothing to jam.

   The west wall faces +X, so the sign is a YZ sign approached from +X:
   mirror=0, 11 proud of the wall along +X, and the -200 on the Z argument —
   the Sliding Bars Room's gate 4 panel exactly. Its text sits above the panel
   at that room's -343, for the same reason. Wall radius 195 holds the player
   at x>=-4605, well inside the 500 trigger, and the button is far from both
   ways out. */
#define TS_NORTH_X        (-2300)     /* the art spans x[-2400,-2200] */
#define TS_NORTH_Z           400
#define TS_NORTH_TEXT_Y     (-186)    /* eye level on the y=0 floor */
#define TS_LADDER_X          300      /* the art spans x[200,400] */
#define TS_LADDER_Z            0
#define TS_LADDER_TEXT_Y    (-186)    /* eye level on the y=0 floor */
#define TS_BUTTON_X        (-4800)    /* the panel spans z[-1000,-800] */
#define TS_BUTTON_Z         (-900)
#define TS_BUTTON_TEXT_Y    (-343)    /* glyph TOP, 15 above the y[-300,-100] panel */

#define TS_TEXT_RADIUS      1200
#define TS_FADE_NEAR         800
#define TS_TRIGGER_RADIUS    500

/* Circle edge-detect, one per way out. Seeded "held" by the arm below so a
   press carried in through the transition cannot fire on the arrival frame. */
static int north_circle_prev  = 1;
static int ladder_circle_prev = 1;
static int button_circle_prev = 1;

void the_shelf_arm(void) {
    north_circle_prev  = interact_tapped();
    ladder_circle_prev = north_circle_prev;
    button_circle_prev = north_circle_prev;
}

/* One Circle test for either way out: a fresh press, in range, facing it.
   THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
static int ts_triggered(int lock, int *circle_prev, int32_t x, int32_t z) {
    int held = interact_tapped();
    int just = held && !*circle_prev;
    int32_t dx, dz, xz;
    *circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - x;
    dz = cam_z - z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= TS_TRIGGER_RADIUS) return 0;
    if (!interact_facing(x, z)) return 0;
    return 1;
}

int the_shelf_north_door_triggered(int lock) {
    return ts_triggered(lock, &north_circle_prev, TS_NORTH_X, TS_NORTH_Z);
}

int the_shelf_ladder_triggered(int lock) {
    return ts_triggered(lock, &ladder_circle_prev, TS_LADDER_X, TS_LADDER_Z);
}

/* The reset button. The gates are re-placed from the state on every entry to
   the Sliding Bars Room, so clearing it here is the whole job. */
void the_shelf_update(int lock) {
    if (!ts_triggered(lock, &button_circle_prev, TS_BUTTON_X, TS_BUTTON_Z))
        return;
    sliding_bars_room_reset_gates();
    show_pickup_msg_raw("You heard some slamming in the distance");
}

/* A floating prompt. Same shape as every door sign in the game: opaque within
   TS_FADE_NEAR, gone by TS_TEXT_RADIUS. Both are XY signs approached from -Z,
   so the plane, the mirror flag and the -11 standoff are shared too. */
static void ts_sign(RenderContext *ctx, const char *text,
                    int32_t x, int32_t text_y, int32_t z) {
    int32_t dx = cam_x - x;
    int32_t dz = cam_z - z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= TS_TEXT_RADIUS) return;

    if (xz > TS_FADE_NEAR) {
        int range = TS_TEXT_RADIUS - TS_FADE_NEAR;
        int prog  = xz - TS_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, text, x - 200, text_y, z - 11,
                        50, 255, 50, fade,
                        0, TEXT_PLANE_XY,   /* mirror=0: XY, approached from -Z */
                        DOOR_PIXEL_SIZE);
}

void the_shelf_spawn_north(void) {
    /* In from the Up Down Maze. 220 off wall 15 on its walkable -Z side, on the
       door's centre line, facing -Z — the direction of travel, across the north
       corridor. That leaves the player at z=180, 446 short of the north bars:
       inside the corridor, clear of both. */
    cam_x   = TS_NORTH_X;
    cam_y   = TS_EYE_Y;
    cam_vy  = 0;
    cam_z   = TS_NORTH_Z - (TS_WALL_RADIUS + 25);
    cam_rot = 2048;                    /* facing -Z, south */
    the_shelf_arm();
}

void the_shelf_spawn_ladder(void) {
    /* Up the ladder from the H Corridor. 220 off wall 0 on its walkable -Z
       side, on the ladder's centre line (the passage's, 300 clear of both its
       walls), facing -Z — the direction of travel: the climb comes up out of
       the shaft and steps off south, down the passage. */
    cam_x   = TS_LADDER_X;
    cam_y   = TS_EYE_Y;
    cam_vy  = 0;
    cam_z   = TS_LADDER_Z - (TS_WALL_RADIUS + 25);
    cam_rot = 2048;                    /* facing -Z, south */
    the_shelf_arm();
}

void the_shelf_init(void) {
    room_data_collision(&current_collision_room);
    /* The walls' tops, y=-800: there is no vault mesh, the chapter's open
       dark. The shaft drops below the floor, but nothing walks down there. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(TS_WALL_RADIUS);

    ts_floor_zones_init();
    cam_pitch = 0;

    the_shelf_spawn_north();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every collide routine, so an
       instance left from another room would block invisibly anywhere it falls
       inside the hall. Safe to clear: every room that has one re-places its
       own on entry. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    ts_view_resolve(1);
}

static void draw_the_shelf_smd(RenderContext *ctx) {
    if (!ts_smd) return;

    uint8_t *p = (uint8_t *)ts_smd->p_prims;
    int i, n = ts_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = ts_fog_far;   /* resolved by ts_view_resolve() this frame */
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
        SVECTOR *v0 = &ts_smd->p_verts[vi[0]];
        SVECTOR *v1 = &ts_smd->p_verts[vi[1]];
        SVECTOR *v2 = &ts_smd->p_verts[vi[2]];

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

        int nocull = (i < THE_SHELF_PRIM_COUNT) && room_nocull(i);
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
            v3 = &ts_smd->p_verts[vi[3]];
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
        int32_t fog = dist < ts_fog_near ? ts_fog_near : (dist > ts_fog_far ? ts_fog_far : dist);
        int32_t fog_factor = ((ts_fog_far - fog) << 8) / (ts_fog_far - ts_fog_near);

        uint8_t tex_idx = (i < THE_SHELF_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < THE_SHELF_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + TS_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + TS_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + TS_FOG_B * (256 - fog_factor)) >> 8);

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

void the_shelf_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    ts_view_resolve(0);

    g_fog_near = ts_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : ts_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, TS_FOG_R, TS_FOG_G, TS_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All five textures sit at Voff 0. */
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

    if (exp != DBG_EXP_NO_MESH) draw_the_shelf_smd(ctx);

    /* >>> LEVEL 8 REMOVES THE FOUR MONSTERS AND THE THREE SIGNS. <<< Nothing else
       stands in this room. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        ts_sign(ctx, "Press " BTN_CIRCLE " to enter",
                TS_NORTH_X, TS_NORTH_TEXT_Y, TS_NORTH_Z);
        ts_sign(ctx, "Press " BTN_CIRCLE " to descend",
                TS_LADDER_X, TS_LADDER_TEXT_Y, TS_LADDER_Z);
        /* The reset button: a YZ sign approached from +X (mirror=0), so it
           does not go through ts_sign's XY offsets. */
        {
            int32_t dx = cam_x - TS_BUTTON_X;
            int32_t dz = cam_z - TS_BUTTON_Z;
            int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            if (xz < TS_TEXT_RADIUS) {
                int fade = 256;
                if (xz > TS_FADE_NEAR)
                    fade = 256 - (((xz - TS_FADE_NEAR) * 256) /
                                  (TS_TEXT_RADIUS - TS_FADE_NEAR));
                door_draw_string_3d(ctx, "press " BTN_CIRCLE " to activate",
                                    TS_BUTTON_X + 11, TS_BUTTON_TEXT_Y,
                                    TS_BUTTON_Z - 200,
                                    50, 255, 50, fade,
                                    0, TEXT_PLANE_YZ,  /* mirror=0: from +X */
                                    DOOR_PIXEL_SIZE);
            }
        }
        /* THE CHAPTER 3 ENEMIES. Their sheets sit at Voff 128, so each is
           handed the window to restore after drawing unmasked. */
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
