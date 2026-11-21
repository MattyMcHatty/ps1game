#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "room_arena.h"
#include "cull_arena.h"
#include "tim_slots.h"
#include "texmgr.h"
#include "camera.h"
#include "collision.h"
#include "room_data.h"
#include "lightnings_edge.h"
#include "lightnings_edge_tex_map.h"
#include "underworld.h"
#include "underworld_sky.h"
#include "hell_hath_no_fury.h"  /* the volcanic ground, a narrow uploader     */
#include "garden_courtyard.h"   /* the garden gate, a narrow uploader         */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "item_pickup.h"
#include "particles.h"
#include "btn_glyph.h"
#include "door.h"
#include "lightning_tablet.h"  /* the north wall's tablet                   */

/* Lightnings Edge - see lightnings_edge.h. The room half is src/lava_maze.c's
   (itself A Woman Scorned's, Wingscape's, Lava U's and Hell Hath No Fury's):
   cull keys, radius side cull and all. */

static SMD  *le_smd  = NULL;
static void *le_buff = NULL;

/* ---- The room --------------------------------------------------------------
   x[-1200,1200] z[-1200,2599] (the collision proxy's bounds), one floor at
   y=0: an octagon of volcanic rock with cut corners. The drawn mesh tops out
   at y=-400; the proxy's walls stand 600. The sky is centred on the middle of
   it, half way up the drawn walls. */
#define LE_CX                0
#define LE_CZ              700
#define LE_SKY_Y         (-200)

#define LE_FLOOR_Y          0
#define LE_EYE_Y         (LE_FLOOR_Y - GROUND_FLOOR_Y - 40)
/* Above both the drawn mesh (-400) and the proxy's walls (-600). */
#define LE_CEILING_Y     (-601)

/* ---- THE SOUTH GATE, to Ribcage --------------------------------------------
   The grdn_gte polys stand in the plane z=-1200, x[-200,400], 400 tall (in
   collision wall 16, x[-600,600], inward normal +Z), so the player walks up
   to it from the north. The gate is off the wall's centre: its own middle is
   x=100. So the press point is the middle of its foot, (100,-1200); the sign
   is 11 off it at y=-200, the middle of its face, on a free yaw facing south
   (2048, the way the reader faces); the spawn, for an arrival through it, is
   250 in along the normal, clear of the 195 push radius, facing north. */
#define LE_S_X            100
#define LE_S_Z          (-1200)
#define LE_S_TEXT_X       100
#define LE_S_TEXT_Z     (-1189)
#define LE_S_TEXT_Y      (LE_FLOOR_Y - 200)
#define LE_S_TEXT_YAW    2048
#define LE_S_SPAWN_X      100
#define LE_S_SPAWN_Z     (-950)
#define LE_S_SPAWN_ROT      0

#define LE_TEXT_RADIUS    1500
#define LE_FADE_NEAR      1000
#define LE_TRIGGER_RADIUS  500

/* Circle edge-detect. Seeded "held" by the arm so a press carried in through
   the transition cannot fire on the arrival frame. */
static int s_circle_prev = 1;

/* ---- THE FLOOR -------------------------------------------------------------
   One storey: the proxy has one floor plane, y=0, wider than the walls, so
   one FLOOR_FLAT zone over the collision bounds. The walls keep the player
   out of the cut corners. There is no lava here: the mesh has no lava
   texture at all, so the chapter's rule 5 has nothing to mask and the room
   has no ROOMS line in tools/gen_lava_floors.py (the Chamber of Destiny is
   the other such room). */
static void le_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -1200;  floor_zones[0].max_x = 1200;
    floor_zones[0].min_z = -1200;  floor_zones[0].max_z = 2599;
    floor_zones[0].y     = LE_FLOOR_Y;
    floor_zone_count = 1;
}

/* ---- Textures --------------------------------------------------------------
   TWO mesh textures, NONE owned (no new textures, the user's brief):
     0  volcanic     Hell Hath No Fury's, its narrow uploader (x384 y256)
     1  grdn_gte     the Garden Courtyard's gate (GRDNGTE.TIM, 4bpp x512 y0,
                     CLUT 672,490), through garden_courtyard_upload_gate() -
                     whose registration is in the Underworld bank for this
                     room. At its OWN address: down here x512 y0 is rocks'
                     page (Sharur's Trial, Ribcage) and the Plinth's and the
                     Chamber's top-right art, and its CLUT line is their
                     bottom-left art's - every one of those rooms re-uploads
                     its own on entry, and this room draws none of them, so
                     taking the page costs no restore. Its see-through texels
                     are CLUT 0, so the sky shows between the bars.
   The transition OUT of here is DOOR_PANEL_GATE, door_anim's own boot copy
   (x544 y128), always there; its sound, SFX_GATE, is in the chapter's bank.
   The sky's texture is src/underworld_sky.c's business. */
#define LE_TEX_COUNT 2

static uint16_t tex_tpage[LE_TEX_COUNT];
static uint16_t tex_clut[LE_TEX_COUNT];

void lightnings_edge_load_assets(void) {
    /* Nothing to register: both textures are other rooms'. Header only. */
    TIM_SLOT(0, VOLCANIC);
    TIM_SLOT(1, GRDNGTE);
}

/* Pure LoadImage from the RAM copies texmgr holds - no CD access, safe during
   the transition (main's STATE_LOADING DrawSyncs first). */
void lightnings_edge_upload_textures(void) {
    hell_hath_no_fury_upload_volcanic();
    garden_courtyard_upload_gate();
    underworld_sky_upload_textures();
    lightning_tablet_upload_texture();   /* the tablet (owned, the tablets' page) */
}

/* ---- The cull key, and its RADIUS ------------------------------------------
   le_build_cull_keys() verbatim (src/lava_maze.c, itself A Woman Scorned's;
   tools/DIAGNOSING_FRAME_RATE.txt STEP 3I for why the radius). Built on the
   same call that reloads the mesh it describes, and nowhere else. */
static int     le_key_count = 0;
static int32_t le_y_lo = 0, le_y_hi = 0;

static void le_build_cull_keys(void) {
    le_key_count = 0;
    if (!le_smd) return;
    uint8_t *p = (uint8_t *)le_smd->p_prims;
    int i, n = le_smd->n_prims;
    if (n > LIGHTNINGS_EDGE_PRIM_COUNT) n = LIGHTNINGS_EDGE_PRIM_COUNT;
    le_y_lo = le_y_hi = 0;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &le_smd->p_verts[vi[0]];
        int nv = (pt->type >= 2) ? 4 : 3;
        int16_t lo_x = v0->vx, hi_x = v0->vx;
        int16_t lo_z = v0->vz, hi_z = v0->vz;
        int k;
        for (k = 0; k < nv; k++) {
            SVECTOR *vk = &le_smd->p_verts[vi[k]];
            if (vk->vx < lo_x) lo_x = vk->vx;
            if (vk->vx > hi_x) hi_x = vk->vx;
            if (vk->vz < lo_z) lo_z = vk->vz;
            if (vk->vz > hi_z) hi_z = vk->vz;
            if (vk->vy < le_y_lo) le_y_lo = vk->vy;
            if (vk->vy > le_y_hi) le_y_hi = vk->vy;
        }
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        {
            int32_t rad = (hi_x - lo_x) + (hi_z - lo_z);   /* >= any |v - v0| */
            rad = (rad + 3) >> 2;
            cull_keys[i].pad = (uint8_t)(rad >= 255 ? 255 : rad);
        }
        p += pt->len;
    }
    le_key_count = n;
}

void lightnings_edge_load_geometry(void) {
    le_buff = room_arena_load("\\TEXUNDW\\LGHTEDGE.SMD;1");
    le_smd  = le_buff ? smdInitData(le_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (le_smd && !room_data_bind(le_smd->n_prims)) le_smd = NULL;
    le_build_cull_keys();
    /* The tablet's mesh, into the arena's tail behind the room's own file -
       after room_data_bind, which reads that file's end (src/room_arena.h). */
    lightning_tablet_load_geometry();
    /* The sky: kept across Chapter 4's rooms (a no-op when already held). */
    underworld_sky_load();
}

/* ---- The gate --------------------------------------------------------------*/
void lightnings_edge_doors_arm(void) {
    s_circle_prev = interact_tapped();
}

/* A fresh Circle within reach of the gate and facing it. The edge state is
   kept up to date even while locked. */
int lightnings_edge_s_gate_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !s_circle_prev;
    int32_t dx, dz;
    s_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - LE_S_X;
    dz = cam_z - LE_S_Z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= LE_TRIGGER_RADIUS)
        return 0;
    return interact_facing(LE_S_X, LE_S_Z);
}

static void le_s_text(RenderContext *ctx) {
    int32_t dx = cam_x - LE_S_X;
    int32_t dz = cam_z - LE_S_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= LE_TEXT_RADIUS) return;
    if (xz > LE_FADE_NEAR) {
        int range = LE_TEXT_RADIUS - LE_FADE_NEAR;
        int prog  = xz - LE_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            LE_S_TEXT_X, LE_S_TEXT_Y, LE_S_TEXT_Z,
                            50, 255, 50, fade, LE_S_TEXT_YAW,
                            DOOR_PIXEL_SIZE);
}

void lightnings_edge_spawn_s(void) {
    cam_x     = LE_S_SPAWN_X;
    cam_y     = LE_EYE_Y;
    cam_vy    = 0;
    cam_z     = LE_S_SPAWN_Z;
    cam_rot   = LE_S_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();
    lightnings_edge_doors_arm();
}

void lightnings_edge_init(void) {
    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(LE_CEILING_Y);
    le_floor_zones_init();

    /* The one arrival, and where a debug jump lands: the south gate, from
       Ribcage's east door. */
    lightnings_edge_spawn_s();

    /* The global prop arrays, as Ribcage clears them: Hell Hath No Fury's
       save point and tank would otherwise block invisibly inside this room's
       bounds. The clear drops the placement, never OD_TANK_HHNF's oil. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    bars_clear();
    reset_particles();

    /* THE LIGHTNING TABLET (src/lightning_tablet.h): no shot, no shards, and
       a fresh deal of its twelve squares on every entry. Its broken flag is
       read live. */
    lightning_tablet_arm();
}

/* ---- The mesh --------------------------------------------------------------
   draw_le_smd() (src/lava_maze.c, itself A Woman Scorned's) verbatim: the chapter's fog, set in
   src/underworld.h and changed by nothing, the key-point-and-radius side cull,
   and far primitives clamped one OT slot in front of the sky. */
static void draw_le_smd(RenderContext *ctx) {
    if (!le_smd) return;

    uint8_t *p = (uint8_t *)le_smd->p_prims;
    int i, n = le_key_count;

    /* HOISTED OUT OF THE LOOP (STEP 3C fix 1): none of these can change while
       a frame is being queued. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = UW_FOG_FAR;
    int32_t fog_start = UW_FOG_NEAR < cull ? UW_FOG_NEAR : cull / 2;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    /* ---- THE SIDE-PLANE CULL'S PER-FRAME TERMS ------------------------------
       Hell Hath No Fury's, unchanged: see draw_hhnf_smd for the derivation
       (the pitch bound, the 10*R radius term and the arithmetic). */
#define LE_SIDE_MARGIN  (8 << 12)
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    int     pitched    = (cam_pitch != 0);
    int32_t pc = icos(cam_pitch);
    int32_t pslack = 0;
    if (pc < 0) no_frustum = 1;
    if (pitched) {
        int32_t ps   = isin(cam_pitch);
        int32_t dylo = le_y_lo - cam_y, dyhi = le_y_hi - cam_y;
        if (ps   < 0) ps   = -ps;
        if (dylo < 0) dylo = -dylo;
        if (dyhi < 0) dyhi = -dyhi;
        pslack = ps * (dylo > dyhi ? dylo : dyhi) + 512;
    }

    for (i = 0; i < n; i++) {
        /* >>> THE REJECT PATH READS cull_keys, NOT THE MESH. <<< */
        uint8_t stride = cull_keys[i].stride;
        {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > cull)
                { p += stride; continue; }
            if (dx * sn + dz * cs < -(700 << 12))
                { p += stride; continue; }
        }
        /* The side planes, off the key point and its radius. */
        if (!no_frustum && cull_keys[i].pad != 255) {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            int32_t f  = dx * sn + dz * cs;
            int32_t sd = dx * cs - dz * sn;
            int32_t r  = ((int32_t)cull_keys[i].pad * 40) << 12;   /* >= 10R */
            if (pitched) f = (((f >> 8) * pc) >> 4) + pslack;
            f = f * 5 + LE_SIDE_MARGIN + r;
            if (sd * 8 > f || -sd * 8 > f) { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &le_smd->p_verts[vi[0]];
        SVECTOR *v1 = &le_smd->p_verts[vi[1]];
        SVECTOR *v2 = &le_smd->p_verts[vi[2]];

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

        int nocull = (i < LIGHTNINGS_EDGE_PRIM_COUNT) && room_nocull(i);
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
            v3 = &le_smd->p_verts[vi[3]];
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
        if (otz >= UW_SKY_OT) otz = UW_SKY_OT - 1;   /* in front of the sky */

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog = dist < fog_start ? fog_start : (dist > cull ? cull : dist);
        int32_t fog_factor = ((cull - fog) << 8) / (cull - fog_start);

        uint8_t tex_idx = (i < LIGHTNINGS_EDGE_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < LE_TEX_COUNT);
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + UW_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + UW_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + UW_FOG_B * (256 - fog_factor)) >> 8);

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

void lightnings_edge_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THE CHAPTER'S FOG, and nothing else's (src/underworld.h). */
    g_fog_near = UW_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : UW_FOG_FAR;

    /* The clear colour, not a full-screen TILE (wrong turn #3). The sky is an
       open ring, so this is what shows above and below it. */
    render_set_clear_colour(ctx, UW_FOG_R, UW_FOG_G, UW_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       Both room textures and the sky's sit at Voff 0. No enemy sheet. */
    RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
            setTexWindow(twin, &tw);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    MATRIX view;
    camera_build_view(&view);
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);

    if (exp != DBG_EXP_NO_MESH) {
        underworld_sky_draw(ctx, LE_CX, LE_SKY_Y, LE_CZ);
        draw_le_smd(ctx);
    }

    /* >>> LEVEL 8 IS THE LIGHTNING TABLET (its slab, lit lines, shards and
       sign), the LIGHTNING BOLT once it has shattered, and the GATE'S SIGN.
       The tablet reloads the plain view itself. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        lightning_tablet_draw(ctx);
        item_pickups_draw(ctx);
        le_s_text(ctx);
    }

    /* The tablet's cursor and controls, over the puzzle's shot. Screen space
       and outside the entity gate: it is the puzzle. */
    lightning_tablet_draw_overlay(ctx);
}
