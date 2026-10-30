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
#include "hell_hath_no_fury.h"
#include "hell_hath_no_fury_tex_map.h"
#include "underworld.h"
#include "underworld_sky.h"
#include "catacombs_entry.h"    /* cobble + the inner door, narrow uploaders  */
#include "seal.h"               /* the Seal's floor art                       */
#include "descent_to_hell.h"    /* the lava, a narrow uploader                */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "item_pickup.h"
#include "particles.h"

/* Hell Hath No Fury - see hell_hath_no_fury.h. The room half is
   src/master_bedroom.c's, which has been through DIAGNOSING_FRAME_RATE.txt,
   with the chapter's fog and sky (src/underworld.h) in place of its own. */

static SMD  *hhnf_smd  = NULL;
static void *hhnf_buff = NULL;

/* ---- The platform ----------------------------------------------------------
   x[0,4200) z[0,4200) (the collision proxy's bounds), floor at y=0, and the
   sky centred on the middle of it, half way up the ruin. */
#define HHNF_CX           2100
#define HHNF_CZ           2100
#define HHNF_SKY_Y       (-300)

#define HHNF_FLOOR_Y         0
#define HHNF_EYE_Y        (HHNF_FLOOR_Y - GROUND_FLOOR_Y - 40)
/* The ruin is the tallest thing here and its proxy walls reach y=-600. */
#define HHNF_CEILING_Y    (-600)

/* THE CENTRE, on the seal, facing north (+Z): out across the platform to the
   two north doors with the ruin behind the player's right shoulder. */
#define HHNF_SPAWN_ROT       0

static void hhnf_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = 0;  floor_zones[0].max_x = 4200;
    floor_zones[0].min_z = 0;  floor_zones[0].max_z = 4200;
    floor_zones[0].y     = HHNF_FLOOR_Y;
    floor_zone_count = 1;
}

/* ---- Textures --------------------------------------------------------------
   FIVE mesh textures, ONE owned:
     0  volcanic             OWNED. 4bpp x384 y256, CLUT (928,502): the start
                             of a page the Catacombs map calls FREE, i.e. every
                             tenant is re-uploaded by its own room on entry.
     1  cobblestones         the Catacombs Entry's, its narrow uploader
     2  catacomb inner door  the Catacombs Entry's, its narrow uploader
     3  wd_dr                RESIDENT: the fat door's startup upload at x768
                             y256, which nothing in Chapter 3 or 4 overwrites
                             (VRAM_MAP_CATACOMBS.txt: "needs a way back first").
                             A future Underworld room that takes that page owes
                             this room a restore.
     4  seal                 the Seal's, its uploader (pixels and its CLUT line,
                             Lamashtu's, at 640,502)
     5  lava                 the Descent's, its narrow uploader: x320 y256, CLUT
                             (944,502). Its UVs run past 128 and wrap in the
                             room's 128 window, so it must stay a page start.
   The sky's texture is src/underworld_sky.c's business. */
#define HHNF_TEX_COUNT 6

static uint16_t tex_tpage[HHNF_TEX_COUNT];
static uint16_t tex_clut[HHNF_TEX_COUNT];
static int      volcanic_tex = -1;

void hell_hath_no_fury_load_assets(void) {
    /* BANK: Chapter 4. Derived, not guessed - py tools/check_tex_banks.py walks
       the uploader call graph and fails the build if this is short. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    volcanic_tex = texmgr_register("\\TEXUNDW\\VOLCANIC.TIM;1");
    TIM_SLOT(0, VOLCANIC);
    TIM_SLOT(1, COBBLE);
    TIM_SLOT(2, CTCMBDR);
    TIM_SLOT(3, WDDR);
    TIM_SLOT(4, SEAL);
    TIM_SLOT(5, LAVA);
}

/* Pure LoadImage from the RAM copies texmgr holds - no CD access, safe during
   the transition (main's STATE_LOADING DrawSyncs first). */
void hell_hath_no_fury_upload_textures(void) {
    if (volcanic_tex >= 0) texmgr_upload(volcanic_tex);
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    seal_upload_textures();
    descent_to_hell_upload_lava();
    underworld_sky_upload_textures();
}

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/master_bedroom.c. Built on the same call that reloads the
   mesh it describes, and nowhere else. NO BOX KEY, for that room's reason. */
static int hhnf_key_count = 0;

static void hhnf_build_cull_keys(void) {
    hhnf_key_count = 0;
    if (!hhnf_smd) return;
    uint8_t *p = (uint8_t *)hhnf_smd->p_prims;
    int i, n = hhnf_smd->n_prims;
    if (n > HELL_HATH_NO_FURY_PRIM_COUNT) n = HELL_HATH_NO_FURY_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &hhnf_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    hhnf_key_count = n;
}

void hell_hath_no_fury_load_geometry(void) {
    hhnf_buff = room_arena_load("\\TEXUNDW\\HHNF.SMD;1");
    hhnf_smd  = hhnf_buff ? smdInitData(hhnf_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (hhnf_smd && !room_data_bind(hhnf_smd->n_prims)) hhnf_smd = NULL;
    hhnf_build_cull_keys();
    /* The sky: its own small model, kept across Chapter 4's rooms and handed
       back by load_area_geometry on the way into any other. */
    underworld_sky_load();
}

/* ---- The arrival out of white ----------------------------------------------
   The Descent's white held, the room built under it, and then 2 s up out of
   it with the player rooted: turn, but no walking, and no HUD. */
#define HHNF_T_FADE   120

static int32_t fade_t = HHNF_T_FADE;   /* HHNF_T_FADE = not fading */

/* ---- The opening drop ------------------------------------------------------
   Under the fade, and on long after it: the camera hangs straight over the
   centre of the seal, looking straight down (the Descent's pitch, yaw north),
   and comes down onto it, slowing to nothing as it reaches the player's eye.

   THE HEIGHT. The fog is measured in Manhattan XZ, so an overhead camera reads
   as close however high it is. For the drop only, draw_hhnf_smd adds the
   camera's rise above the eye to that distance - the chapter's 800/3500 are
   untouched, only where they are measured from changes (src/underworld.h) -
   and the drop starts 3300 up: the seal's centre is then 200 inside the cull
   line, a few percent out of the fog, and the floor round it further in.

   THE SPEED. 9 s, by request (an even deceleration from the Descent's speed
   took 13.7). It starts at the Descent's one steady speed (5950 in 740
   frames, 8.04 a frame) and slows to zero, LATE rather than evenly: speed =
   v0 * (1 - tau^3), tau = t / T, which averages 3/4 of v0, so the 3300 takes
   T = 3300 / (0.75 * v0) = 540 frames at v0 = 8.15 - the Descent's to 1.4%.
   Integrated: rise = R * (1 - (4 tau - tau^4) / 3).

   THE TURN. In the last HHNF_T_TURN frames - from 200 above the eye, just
   short of the player's height - the pitch eases from straight down to level
   (smoothstep), the yaw held north throughout, so the drop lands on the
   ground facing north and hands the player control on that frame.

   >>> NO BEHIND-THE-YAW CULL DURING THE DROP. <<< draw_hhnf_smd's 700-unit
   test ignores pitch; looking straight down from 3300 the screen reaches
   ~1550 south of the camera, so it would cut a straight edge across the seal
   (memory: overhead-camera-behind-cull). The whole platform is cheap. */
#define HHNF_DROP_RISE   3300
#define HHNF_T_DROP       540
#define HHNF_T_TURN       100
#define HHNF_PITCH_DOWN  1024   /* the Descent's DH_PITCH_DOWN */

static int32_t drop_t = HHNF_T_DROP;   /* HHNF_T_DROP = not dropping */

/* The ends are early returns, not clamps (memory: clamp-before-divide). */
static void hhnf_place_camera(void) {
    if (drop_t >= HHNF_T_DROP) return;
    int32_t s = HHNF_T_DROP - drop_t;              /* frames left, 1..T */
    int32_t u  = (drop_t << 12) / HHNF_T_DROP;     /* tau, 0..4096 */
    int32_t u2 = (u * u) >> 12;
    int32_t u4 = (u2 * u2) >> 12;
    int32_t r  = HHNF_DROP_RISE -
                 ((HHNF_DROP_RISE * ((4 * u - u4) / 3)) >> 12);
    cam_x   = HHNF_CX;
    cam_z   = HHNF_CZ;
    cam_y   = HHNF_EYE_Y - r;
    cam_vy  = 0;
    cam_rot = HHNF_SPAWN_ROT;
    if (s >= HHNF_T_TURN) {
        cam_pitch = HHNF_PITCH_DOWN;
    } else {
        int32_t w = (s << 8) / HHNF_T_TURN;        /* 256 -> 0 */
        cam_pitch = (w * w * (768 - 2 * w)) >> 14; /* 1024 * smoothstep */
    }
}

/* On the ground, facing north: the spawn, and the player's from here. */
static void hhnf_land(void) {
    cam_x     = HHNF_CX;
    cam_z     = HHNF_CZ;
    cam_y     = HHNF_EYE_Y;
    cam_vy    = 0;
    cam_rot   = HHNF_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();
}

void hell_hath_no_fury_from_white(void) {
    fade_t = 0;
    drop_t = 0;
    hhnf_place_camera();   /* the first frame drawn is the top of the drop */
}

int hell_hath_no_fury_fading(void) { return fade_t < HHNF_T_FADE; }

int hell_hath_no_fury_intro(void) { return drop_t < HHNF_T_DROP; }

void hell_hath_no_fury_reset(void) {
    fade_t = HHNF_T_FADE;
    drop_t = HHNF_T_DROP;
}

void hell_hath_no_fury_init(void) {
    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(HHNF_CEILING_Y);
    hhnf_floor_zones_init();

    cam_x     = HHNF_CX;
    cam_y     = HHNF_EYE_Y;
    cam_vy    = 0;
    cam_z     = HHNF_CZ;
    cam_rot   = HHNF_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();

    /* No fade or drop unless main.c asks for one (the Descent's hand-off). */
    fade_t = HHNF_T_FADE;
    drop_t = HHNF_T_DROP;

    /* The global prop arrays, as the Seal clears them: an instance left from
       another room would block invisibly inside x[0,4200) z[0,4200). */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    bars_clear();
    reset_particles();
}

void hell_hath_no_fury_update(int lock) {
    (void)lock;
    if (fade_t < HHNF_T_FADE) fade_t++;
    if (drop_t < HHNF_T_DROP) {
        drop_t++;
        if (drop_t >= HHNF_T_DROP) hhnf_land();
        else                       hhnf_place_camera();
    }
}

/* ---- The mesh --------------------------------------------------------------
   src/master_bedroom.c's loop, with the chapter's fog: one distance, set in
   src/underworld.h and changed by nothing. Far primitives clamp one OT slot in
   front of the sky so none is ever drawn under it. */
static void draw_hhnf_smd(RenderContext *ctx) {
    if (!hhnf_smd) return;

    uint8_t *p = (uint8_t *)hhnf_smd->p_prims;
    int i, n = hhnf_key_count;

    /* HOISTED OUT OF THE LOOP (STEP 3C fix 1): none of these can change while
       a frame is being queued. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = UW_FOG_FAR;
    int32_t fog_start = UW_FOG_NEAR < cull ? UW_FOG_NEAR : cull / 2;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    /* THE OPENING DROP: its height counts toward the fog and the cull, and
       the behind test is off (see "The opening drop"). 0 / on otherwise. */
    int     dropping = hell_hath_no_fury_intro();
    int32_t rise     = dropping ? HHNF_EYE_Y - cam_y : 0;
    if (rise < 0) rise = 0;

    for (i = 0; i < n; i++) {
        /* >>> THE REJECT PATH READS cull_keys, NOT THE MESH. <<< */
        uint8_t stride = cull_keys[i].stride;
        {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) + rise > cull)
                { p += stride; continue; }
            if (!dropping && dx * sn + dz * cs < -(700 << 12))
                { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &hhnf_smd->p_verts[vi[0]];
        SVECTOR *v1 = &hhnf_smd->p_verts[vi[1]];
        SVECTOR *v2 = &hhnf_smd->p_verts[vi[2]];

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

        int nocull = (i < HELL_HATH_NO_FURY_PRIM_COUNT) && room_nocull(i);
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
            v3 = &hhnf_smd->p_verts[vi[3]];
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
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) + rise;
        int32_t fog = dist < fog_start ? fog_start : (dist > cull ? cull : dist);
        int32_t fog_factor = ((cull - fog) << 8) / (cull - fog_start);

        uint8_t tex_idx = (i < HELL_HATH_NO_FURY_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < HHNF_TEX_COUNT);
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

/* ---- The white -------------------------------------------------------------
   src/descent_to_hell.c's draw_white: an ADDITIVE full-screen tile in front of
   the scene, 255 on the first frame (the Descent's white, unbroken) down to 0. */
#define HHNF_OT_WHITE  2

static void draw_white(RenderContext *ctx) {
    if (fade_t >= HHNF_T_FADE) return;
    int32_t k = 255 - (fade_t * 255) / HHNF_T_FADE;
    if (k <= 0) return;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) + sizeof(DR_TPAGE) > buf_end) return;

    TILE *tl = (TILE *)ctx->next_packet;
    setTile(tl);
    setSemiTrans(tl, 1);
    setRGB0(tl, (uint8_t)k, (uint8_t)k, (uint8_t)k);
    setXY0(tl, 0, 0);
    setWH(tl, SCREEN_XRES, SCREEN_YRES);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[HHNF_OT_WHITE], tl);
    ctx->next_packet += sizeof(TILE);

    /* After the tile in the same slot: LIFO, so the blend mode is set first. */
    DR_TPAGE *dp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(dp, 0, 0, getTPage(0, 1 /* ABR=1: B + F */, 0, 0));
    addPrim(&ctx->buffers[ctx->active_buffer].ot[HHNF_OT_WHITE], dp);
    ctx->next_packet += sizeof(DR_TPAGE);
}

void hell_hath_no_fury_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THE CHAPTER'S FOG, and nothing else's (src/underworld.h). */
    g_fog_near = UW_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : UW_FOG_FAR;

    /* The clear colour, not a full-screen TILE (wrong turn #3). The sky is an
       open ring, so this is what shows above and below it. */
    render_set_clear_colour(ctx, UW_FOG_R, UW_FOG_G, UW_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All six room textures sit at Voff 0, and so does the sky's, which draws
       under this same window. */
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
        underworld_sky_draw(ctx, HHNF_CX, HHNF_SKY_Y, HHNF_CZ);
        draw_hhnf_smd(ctx);
    }

    /* >>> LEVEL 8 IS NEARLY EMPTY HERE. <<< Nothing stands on the platform
       yet - no enemies, no props, no door signs - so the only entity is any
       pickup a later seed puts down. */
    if (exp != DBG_EXP_NO_ENTITIES)
        item_pickups_draw(ctx);

    draw_white(ctx);
}
