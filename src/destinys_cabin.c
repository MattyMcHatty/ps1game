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
#include "destinys_cabin.h"
#include "destinys_cabin_tex_map.h"
#include "underworld.h"
#include "underworld_sky.h"
#include "descent_to_hell.h"    /* the lava, a narrow uploader               */
#include "hell_hath_no_fury.h"  /* the volcanic rock, likewise               */
#include "stables.h"            /* the cabin's wood, likewise                */
#include "conservatory.h"       /* the lawn, likewise                        */
#include "garden_courtyard.h"   /* the west gate, likewise                   */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "item_pickup.h"
#include "particles.h"
#include "btn_glyph.h"
#include "door.h"

/* Destinys Cabin - see destinys_cabin.h. The room half is src/heavens_gate.c's
   (itself Lightnings Edge's, the Lava Maze's and Hell Hath No Fury's): cull
   keys, radius side cull and all. */

static SMD  *dc_smd  = NULL;
static void *dc_buff = NULL;

/* ---- The room --------------------------------------------------------------
   x[0,5400] z[-199,7000] (the collision proxy's bounds), one floor at y=0: a
   field of volcanic rock and grass inside an octagon of 600-tall proxy walls
   that the drawn mesh does not show, with lava along its south edge (z below
   ~800) and its north edge (z above 6400). In the middle stands THE CABIN,
   stables wood, x[800,4600] z[1800,5200], 1000 tall - the tallest drawn
   thing. The proxy gives it a wall on both faces of each side, so its
   inside x[1000,4400] z[2000,5000] is walkable, through a doorway in its
   south side at x[3600,4200]. The sky is centred on the middle of the room,
   half way up the cabin. */
#define DC_CX             2700
#define DC_CZ             3400
#define DC_SKY_Y         (-500)

#define DC_FLOOR_Y          0
#define DC_EYE_Y         (DC_FLOOR_Y - GROUND_FLOOR_Y - 40)
/* Above both the drawn mesh (the cabin's roof, -1000) and the proxy's walls
   (-600). */
#define DC_CEILING_Y     (-1001)

/* ---- THE WEST GATE, to The Tube ---------------------------------------------
   The grdn_gte polys stand in the plane x=0, z[5400,6200], 500 tall (two rows
   of four 200-wide panels), flush in collision wall 20 (x=0, z[0,6200],
   inward normal +X). The floor in front of it is rock, not lava (the lava
   map in tools/gen_lava_floors.py is clear from z=800 to z=6400). So:
     - the PRESS point is the middle of its foot, (0,5800);
     - THE SIGN is 11 off it along the normal at y=-250, the middle of the
       face, on a free yaw facing the way the reader faces - 3072, west;
     - THE SPAWN, for an arrival through it, is 250 in along the normal
       ((250,5800)), clear of the 195 push radius, facing east (1024), into
       the room.
   Out of here plays DOOR_PANEL_GATE: door_anim's own boot copy (x544 y128),
   always there, and SFX_GATE is in SND_BANK_UNDERWORLD (Lightnings Edge's).
   The far side is The Tube's north door, a catacomb inner door, so the way
   in plays DOOR_PANEL_CATACOMB (per direction, the chapter's rule 4). */
#define DC_W_X               0
#define DC_W_Z            5800
#define DC_W_TEXT_X         11
#define DC_W_TEXT_Z       5800
#define DC_W_TEXT_Y      (DC_FLOOR_Y - 250)
#define DC_W_TEXT_YAW     3072
#define DC_W_SPAWN_X       250
#define DC_W_SPAWN_Z      5800
#define DC_W_SPAWN_ROT    1024

#define DC_TEXT_RADIUS    1500
#define DC_FADE_NEAR      1000
#define DC_TRIGGER_RADIUS  500

/* Circle edge-detect. Seeded "held" by the arm so a press carried in through
   the transition cannot fire on the arrival frame. */
static int w_circle_prev = 1;

/* ---- THE FLOOR -------------------------------------------------------------
   One storey: the proxy has one floor plane, y=0, wider than the walls, so
   one FLOOR_FLAT zone over the collision bounds. The lava is on that same
   plane and BURNS (the chapter's rule 5): its mask is the STATE_DESTINYS_CABIN
   line in tools/gen_lava_floors.py, which src/lava.c finds by area - no code
   here. */
static void dc_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = 0;      floor_zones[0].max_x = 5400;
    floor_zones[0].min_z = -199;   floor_zones[0].max_z = 7000;
    floor_zones[0].y     = DC_FLOOR_Y;
    floor_zone_count = 1;
}

/* ---- Textures --------------------------------------------------------------
   FIVE mesh textures, NONE owned (the user's brief: no new textures), each
   put up at its own address by its owner's narrow uploader. All 4bpp at
   Voff 0, on pages tools/VRAM_MAP_UNDERWORLD.txt lets a room re-lay:
     0  lava          the Descent's LAVA.TIM (x320 y256, CLUT 944,502),
                      descent_to_hell_upload_lava(). The greenhouse door and
                      marble share that page; their rooms re-upload them.
     1  volcanic      Hell Hath No Fury's (x384 y256, CLUT 928,502),
                      hell_hath_no_fury_upload_volcanic().
     2  stables wood  the Stables' STBLWOOD.TIM (x832 y0, CLUT 672,498),
                      stables_upload_wood(). The Catacombs Entry's inner door
                      shares that page AND that CLUT; every room that draws
                      the inner door re-uploads it, and this one never does
                      (its panel out is the gate's, not the catacomb's).
     3  grss          the Conservatory's GRSS.TIM (x768 y0, CLUT 80,511),
                      conservatory_upload_grss(), as Heavens Gate.
     4  grdn_gte      the Garden Courtyard's (x512 y0, CLUT 672,490),
                      garden_courtyard_upload_gate(), as Lightnings Edge.
   The sky's texture is src/underworld_sky.c's business (x640 y0). */
#define DC_TEX_COUNT 5

static uint16_t tex_tpage[DC_TEX_COUNT];
static uint16_t tex_clut[DC_TEX_COUNT];

void destinys_cabin_load_assets(void) {
    /* No registrations: every texture is another module's, so no
       texmgr_set_bank() either (as The Tube and Lightnings Edge). */
    TIM_SLOT(0, LAVA);
    TIM_SLOT(1, VOLCANIC);
    TIM_SLOT(2, STBLWOOD);
    TIM_SLOT(3, GRSS);
    TIM_SLOT(4, GRDNGTE);
}

/* Inside main's STATE_LOADING (which DrawSyncs first). */
void destinys_cabin_upload_textures(void) {
    descent_to_hell_upload_lava();
    hell_hath_no_fury_upload_volcanic();
    stables_upload_wood();
    conservatory_upload_grss();
    garden_courtyard_upload_gate();
    underworld_sky_upload_textures();
}

/* ---- The cull key, and its RADIUS ------------------------------------------
   Heavens Gate's hg_build_cull_keys() verbatim (itself Lightnings Edge's;
   tools/DIAGNOSING_FRAME_RATE.txt STEP 3I for why the radius). Built on the
   same call that reloads the mesh it describes, and nowhere else. */
static int     dc_key_count = 0;
static int32_t dc_y_lo = 0, dc_y_hi = 0;

static void dc_build_cull_keys(void) {
    dc_key_count = 0;
    if (!dc_smd) return;
    uint8_t *p = (uint8_t *)dc_smd->p_prims;
    int i, n = dc_smd->n_prims;
    if (n > DESTINYS_CABIN_PRIM_COUNT) n = DESTINYS_CABIN_PRIM_COUNT;
    dc_y_lo = dc_y_hi = 0;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &dc_smd->p_verts[vi[0]];
        int nv = (pt->type >= 2) ? 4 : 3;
        int16_t lo_x = v0->vx, hi_x = v0->vx;
        int16_t lo_z = v0->vz, hi_z = v0->vz;
        int k;
        for (k = 0; k < nv; k++) {
            SVECTOR *vk = &dc_smd->p_verts[vi[k]];
            if (vk->vx < lo_x) lo_x = vk->vx;
            if (vk->vx > hi_x) hi_x = vk->vx;
            if (vk->vz < lo_z) lo_z = vk->vz;
            if (vk->vz > hi_z) hi_z = vk->vz;
            if (vk->vy < dc_y_lo) dc_y_lo = vk->vy;
            if (vk->vy > dc_y_hi) dc_y_hi = vk->vy;
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
    dc_key_count = n;
}

void destinys_cabin_load_geometry(void) {
    dc_buff = room_arena_load("\\TEXUNDW\\DSTCABIN.SMD;1");
    dc_smd  = dc_buff ? smdInitData(dc_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (dc_smd && !room_data_bind(dc_smd->n_prims)) dc_smd = NULL;
    dc_build_cull_keys();
    /* The sky: kept across Chapter 4's rooms (a no-op when already held). */
    underworld_sky_load();
}

/* ---- The west gate ---------------------------------------------------------*/
void destinys_cabin_doors_arm(void) {
    w_circle_prev = interact_tapped();
}

/* A fresh Circle within reach of the gate and facing it. The edge state is
   kept up to date even while locked. */
int destinys_cabin_w_gate_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !w_circle_prev;
    int32_t dx, dz;
    w_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - DC_W_X;
    dz = cam_z - DC_W_Z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= DC_TRIGGER_RADIUS)
        return 0;
    return interact_facing(DC_W_X, DC_W_Z);
}

/* The gate's floating sign, faded in from DC_TEXT_RADIUS to DC_FADE_NEAR. */
static void dc_w_text(RenderContext *ctx) {
    int32_t dx = cam_x - DC_W_X;
    int32_t dz = cam_z - DC_W_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= DC_TEXT_RADIUS) return;
    if (xz > DC_FADE_NEAR) {
        int range = DC_TEXT_RADIUS - DC_FADE_NEAR;
        int prog  = xz - DC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            DC_W_TEXT_X, DC_W_TEXT_Y, DC_W_TEXT_Z,
                            50, 255, 50, fade, DC_W_TEXT_YAW,
                            DOOR_PIXEL_SIZE);
}

void destinys_cabin_spawn_w(void) {
    cam_x     = DC_W_SPAWN_X;
    cam_y     = DC_EYE_Y;
    cam_vy    = 0;
    cam_z     = DC_W_SPAWN_Z;
    cam_rot   = DC_W_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();
    destinys_cabin_doors_arm();
}

void destinys_cabin_init(void) {
    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(DC_CEILING_Y);
    dc_floor_zones_init();

    /* The one arrival, and where a debug jump lands: the west gate, from
       The Tube's north door. */
    destinys_cabin_spawn_w();

    /* The global prop arrays, as every Chapter 4 room clears them: Hell Hath
       No Fury's save point and tank would otherwise block invisibly inside
       this room's bounds. The clear drops the placement, never OD_TANK_HHNF's
       oil. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    bars_clear();
    reset_particles();
}

/* ---- The mesh --------------------------------------------------------------
   Heavens Gate's draw_hg_smd() (itself Lightnings Edge's) verbatim: the chapter's fog, set in
   src/underworld.h and changed by nothing, the key-point-and-radius side cull,
   and far primitives clamped one OT slot in front of the sky. */
static void draw_dc_smd(RenderContext *ctx) {
    if (!dc_smd) return;

    uint8_t *p = (uint8_t *)dc_smd->p_prims;
    int i, n = dc_key_count;

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
#define DC_SIDE_MARGIN  (8 << 12)
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    int     pitched    = (cam_pitch != 0);
    int32_t pc = icos(cam_pitch);
    int32_t pslack = 0;
    if (pc < 0) no_frustum = 1;
    if (pitched) {
        int32_t ps   = isin(cam_pitch);
        int32_t dylo = dc_y_lo - cam_y, dyhi = dc_y_hi - cam_y;
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
            f = f * 5 + DC_SIDE_MARGIN + r;
            if (sd * 8 > f || -sd * 8 > f) { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &dc_smd->p_verts[vi[0]];
        SVECTOR *v1 = &dc_smd->p_verts[vi[1]];
        SVECTOR *v2 = &dc_smd->p_verts[vi[2]];

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

        int nocull = (i < DESTINYS_CABIN_PRIM_COUNT) && room_nocull(i);
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
            v3 = &dc_smd->p_verts[vi[3]];
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

        uint8_t tex_idx = (i < DESTINYS_CABIN_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < DC_TEX_COUNT);
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

void destinys_cabin_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THE CHAPTER'S FOG, and nothing else's (src/underworld.h). */
    g_fog_near = UW_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : UW_FOG_FAR;

    /* The clear colour, not a full-screen TILE (wrong turn #3). The sky is an
       open ring, so this is what shows above and below it. */
    render_set_clear_colour(ctx, UW_FOG_R, UW_FOG_G, UW_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All five room textures and the sky's sit at Voff 0. No enemy sheet. */
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
        underworld_sky_draw(ctx, DC_CX, DC_SKY_Y, DC_CZ);
        draw_dc_smd(ctx);
    }

    /* >>> LEVEL 8 IS THE GATE'S SIGN <<< - no props, no enemies - and any
       pickup a later seed puts down. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        item_pickups_draw(ctx);
        dc_w_text(ctx);
    }
}
