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
#include "a_woman_scorned.h"
#include "a_woman_scorned_tex_map.h"
#include "underworld.h"
#include "underworld_sky.h"
#include "hell_hath_no_fury.h"  /* the volcanic ground, a narrow uploader     */
#include "descent_to_hell.h"    /* the lava, a narrow uploader                */
#include "sharurs_trial.h"      /* the rocks, a narrow uploader               */
#include "catacombs_entry.h"    /* the catacomb inner door, a narrow uploader */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "item_pickup.h"
#include "particles.h"
#include "btn_glyph.h"
#include "door.h"

/* A Woman Scorned - see a_woman_scorned.h. The room half is src/wingscape.c's
   (itself Lava U's, itself Hell Hath No Fury's): cull keys, radius side cull
   and all. */

static SMD  *aws_smd  = NULL;
static void *aws_buff = NULL;

/* ---- The room --------------------------------------------------------------
   x[0,6600] z[-600,4800] (the collision proxy's bounds), one floor at y=0.
   The sky is centred on the middle of it, half way up the pillars (the
   tallest drawn thing reaches y=-600). */
#define AWS_CX             3300
#define AWS_CZ             2100
#define AWS_SKY_Y         (-300)

#define AWS_FLOOR_Y          0
#define AWS_EYE_Y         (AWS_FLOOR_Y - GROUND_FLOOR_Y - 40)
/* The drawn mesh tops out at y=-600; the proxy's rim walls reach -774. */
#define AWS_CEILING_Y     (-775)

/* ---- THE SOUTH-WEST DOOR, to Hell Hath No Fury -----------------------------
   On the cut corner x+z=600 (collision wall 89, inward normal
   (+0.707,+0.707)), so the player walks up to it from the north-east.

   >>> THE DOOR IS DISTORTED, ON PURPOSE. <<< Its catacomb inner door polys
   are a trapezoid only ~90 wide at the floor ((306,310)-(243,373)) that
   flares out to ~480 at the top (y=-600), LEANING IN off the wall as it
   rises (x+z 616 at the floor, ~740 at y=-300, ~910 at the top). So the
   numbers are taken off its face, not the wall line:
     - the PRESS point is its centre at y=-200, (305,382): ~190 from where
       the push radius holds a player squarely in front of it, well inside
       the 500 trigger;
     - THE SIGN is its centre at y=-300, (330,410), 11 toward the player (8
       on each axis), on a free yaw facing the way the reader faces - 2560,
       south-west - at that mid-height (the door is 600 tall);
     - THE SPAWN, for an arrival through it, is 250 in along the normal from
       the wall under the door (177 on each axis from (262,338)), clear of
       the 195 push radius and in front of the top of the lean, facing
       north-east into the room. */
#define AWS_SW_X            305
#define AWS_SW_Z            382
#define AWS_SW_TEXT_X       338
#define AWS_SW_TEXT_Z       418
#define AWS_SW_TEXT_Y      (AWS_FLOOR_Y - 300)
#define AWS_SW_TEXT_YAW    2560
#define AWS_SW_SPAWN_X      439
#define AWS_SW_SPAWN_Z      515
#define AWS_SW_SPAWN_ROT    512

/* ---- THE SOUTH-EAST DOOR ---------------------------------------------------
   The inr_dbl_dr polys on the cut corner (6000,-600)-(6600,0) (wall 61). It
   leads nowhere yet: no prompt and no trigger until its room exists. */

#define AWS_TEXT_RADIUS    1500
#define AWS_FADE_NEAR      1000
#define AWS_TRIGGER_RADIUS  500

/* Circle edge-detect. Seeded "held" by the arm so a press carried in through
   the transition cannot fire on the arrival frame. */
static int sw_circle_prev = 1;

/* ---- THE FLOOR -------------------------------------------------------------
   One storey: the proxy has one floor plane, y=0, wider than the walls, so
   one FLOOR_FLAT zone over the collision bounds. The rim's walls keep the
   player out of the notches in the outline. */
static void aws_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = 0;     floor_zones[0].max_x = 6600;
    floor_zones[0].min_z = -600;  floor_zones[0].max_z = 4800;
    floor_zones[0].y     = AWS_FLOOR_Y;
    floor_zone_count = 1;
}

/* ---- Textures --------------------------------------------------------------
   FIVE mesh textures, NONE owned - so no texmgr_set_bank() and no CD access:
     0  volcanic             Hell Hath No Fury's, its narrow uploader (x384 y256)
     1  lava                 the Descent's, its narrow uploader (x320 y256). Its
                             UVs run past 128 and wrap in the 128 window.
     2  rocks_128            Sharur's Trial's ROCKS.TIM (4bpp x512 y0): the same
                             128x128 art under its Blender material name,
                             through sharurs_trial_upload_rocks().
     3  catacomb inner door  the Catacombs Entry's, its narrow uploader (x832
                             y0) - which is also DOOR_PANEL_CATACOMB's panel,
                             the transition out of this room, so it MUST be
                             uploaded here (the panel draws before
                             STATE_LOADING).
     4  inr_dbl_dr           RESIDENT: the kitchen's startup stream at x576 y256
                             (kitchen_stream_textures), the south-east double
                             door. Nothing in Chapters 2-4 re-lays that page
                             (VRAM_MAP_UNDERWORLD.txt: "needs a way back
                             first"), so, like wd_dr, it is borrowed as it
                             stands. A future Underworld room that takes x576
                             y256 owes this room a restore.
   The sky's texture is src/underworld_sky.c's business. */
#define AWS_TEX_COUNT 5

static uint16_t tex_tpage[AWS_TEX_COUNT];
static uint16_t tex_clut[AWS_TEX_COUNT];

void a_woman_scorned_load_assets(void) {
    TIM_SLOT(0, VOLCANIC);
    TIM_SLOT(1, LAVA);
    TIM_SLOT(2, ROCKS);
    TIM_SLOT(3, CTCMBDR);
    TIM_SLOT(4, INRDBLDR);
}

/* Pure LoadImage from the RAM copies texmgr holds - no CD access, safe during
   the transition (main's STATE_LOADING DrawSyncs first). */
void a_woman_scorned_upload_textures(void) {
    hell_hath_no_fury_upload_volcanic();
    descent_to_hell_upload_lava();
    sharurs_trial_upload_rocks();
    catacombs_entry_upload_inner_door();
    underworld_sky_upload_textures();
}

/* ---- The cull key, and its RADIUS ------------------------------------------
   ws_build_cull_keys() verbatim (src/wingscape.c, itself Hell Hath No Fury's;
   tools/DIAGNOSING_FRAME_RATE.txt STEP 3I for why the radius). Built on the
   same call that reloads the mesh it describes, and nowhere else. */
static int     aws_key_count = 0;
static int32_t aws_y_lo = 0, aws_y_hi = 0;

static void aws_build_cull_keys(void) {
    aws_key_count = 0;
    if (!aws_smd) return;
    uint8_t *p = (uint8_t *)aws_smd->p_prims;
    int i, n = aws_smd->n_prims;
    if (n > A_WOMAN_SCORNED_PRIM_COUNT) n = A_WOMAN_SCORNED_PRIM_COUNT;
    aws_y_lo = aws_y_hi = 0;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &aws_smd->p_verts[vi[0]];
        int nv = (pt->type >= 2) ? 4 : 3;
        int16_t lo_x = v0->vx, hi_x = v0->vx;
        int16_t lo_z = v0->vz, hi_z = v0->vz;
        int k;
        for (k = 0; k < nv; k++) {
            SVECTOR *vk = &aws_smd->p_verts[vi[k]];
            if (vk->vx < lo_x) lo_x = vk->vx;
            if (vk->vx > hi_x) hi_x = vk->vx;
            if (vk->vz < lo_z) lo_z = vk->vz;
            if (vk->vz > hi_z) hi_z = vk->vz;
            if (vk->vy < aws_y_lo) aws_y_lo = vk->vy;
            if (vk->vy > aws_y_hi) aws_y_hi = vk->vy;
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
    aws_key_count = n;
}

void a_woman_scorned_load_geometry(void) {
    aws_buff = room_arena_load("\\TEXUNDW\\AWSCORN.SMD;1");
    aws_smd  = aws_buff ? smdInitData(aws_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (aws_smd && !room_data_bind(aws_smd->n_prims)) aws_smd = NULL;
    aws_build_cull_keys();
    /* The sky: kept across Chapter 4's rooms (a no-op when already held). */
    underworld_sky_load();
}

/* ---- The door --------------------------------------------------------------*/
void a_woman_scorned_doors_arm(void) {
    sw_circle_prev = interact_tapped();
}

/* A fresh Circle within reach of the south-west door and facing it. The edge
   state is kept up to date even while locked. */
int a_woman_scorned_sw_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !sw_circle_prev;
    int32_t dx, dz;
    sw_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - AWS_SW_X;
    dz = cam_z - AWS_SW_Z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= AWS_TRIGGER_RADIUS)
        return 0;
    return interact_facing(AWS_SW_X, AWS_SW_Z);
}

static void aws_sw_text(RenderContext *ctx) {
    int32_t dx = cam_x - AWS_SW_X;
    int32_t dz = cam_z - AWS_SW_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= AWS_TEXT_RADIUS) return;
    if (xz > AWS_FADE_NEAR) {
        int range = AWS_TEXT_RADIUS - AWS_FADE_NEAR;
        int prog  = xz - AWS_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            AWS_SW_TEXT_X, AWS_SW_TEXT_Y, AWS_SW_TEXT_Z,
                            50, 255, 50, fade, AWS_SW_TEXT_YAW,
                            DOOR_PIXEL_SIZE);
}

void a_woman_scorned_spawn_sw(void) {
    cam_x     = AWS_SW_SPAWN_X;
    cam_y     = AWS_EYE_Y;
    cam_vy    = 0;
    cam_z     = AWS_SW_SPAWN_Z;
    cam_rot   = AWS_SW_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();
    a_woman_scorned_doors_arm();
}

void a_woman_scorned_init(void) {
    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(AWS_CEILING_Y);
    aws_floor_zones_init();

    /* The one arrival, and where a debug jump lands: the south-west door,
       from Hell Hath No Fury. */
    a_woman_scorned_spawn_sw();

    /* The global prop arrays, as Wingscape clears them: Hell Hath No Fury's
       save point and tank would otherwise block invisibly inside this room's
       bounds. The clear drops the placement, never OD_TANK_HHNF's oil. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    bars_clear();
    reset_particles();
}

/* ---- The mesh --------------------------------------------------------------
   draw_ws_smd() (src/wingscape.c) verbatim: the chapter's fog, set in
   src/underworld.h and changed by nothing, the key-point-and-radius side cull,
   and far primitives clamped one OT slot in front of the sky. */
static void draw_aws_smd(RenderContext *ctx) {
    if (!aws_smd) return;

    uint8_t *p = (uint8_t *)aws_smd->p_prims;
    int i, n = aws_key_count;

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
#define AWS_SIDE_MARGIN  (8 << 12)
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    int     pitched    = (cam_pitch != 0);
    int32_t pc = icos(cam_pitch);
    int32_t pslack = 0;
    if (pc < 0) no_frustum = 1;
    if (pitched) {
        int32_t ps   = isin(cam_pitch);
        int32_t dylo = aws_y_lo - cam_y, dyhi = aws_y_hi - cam_y;
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
            f = f * 5 + AWS_SIDE_MARGIN + r;
            if (sd * 8 > f || -sd * 8 > f) { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &aws_smd->p_verts[vi[0]];
        SVECTOR *v1 = &aws_smd->p_verts[vi[1]];
        SVECTOR *v2 = &aws_smd->p_verts[vi[2]];

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

        int nocull = (i < A_WOMAN_SCORNED_PRIM_COUNT) && room_nocull(i);
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
            v3 = &aws_smd->p_verts[vi[3]];
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

        uint8_t tex_idx = (i < A_WOMAN_SCORNED_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < AWS_TEX_COUNT);
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

void a_woman_scorned_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THE CHAPTER'S FOG, and nothing else's (src/underworld.h). */
    g_fog_near = UW_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : UW_FOG_FAR;

    /* The clear colour, not a full-screen TILE (wrong turn #3). The sky is an
       open ring, so this is what shows above and below it. */
    render_set_clear_colour(ctx, UW_FOG_R, UW_FOG_G, UW_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All five room textures and the sky's sit at Voff 0. */
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
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
        underworld_sky_draw(ctx, AWS_CX, AWS_SKY_Y, AWS_CZ);
        draw_aws_smd(ctx);
    }

    /* >>> LEVEL 8 IS THE SOUTH-WEST DOOR'S SIGN and any pickup a later seed
       puts down. No props, no enemies. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        item_pickups_draw(ctx);
        aws_sw_text(ctx);
    }
}
