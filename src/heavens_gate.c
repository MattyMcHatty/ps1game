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
#include "heavens_gate.h"
#include "heavens_gate_tex_map.h"
#include "underworld.h"
#include "underworld_sky.h"
#include "conservatory.h"       /* the lawn, a narrow uploader               */
#include "catacombs_entry.h"    /* the east wall's inner door, likewise      */
#include "stables.h"            /* the greenhouse door panel's art, likewise */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "item_pickup.h"
#include "particles.h"
#include "btn_glyph.h"
#include "door.h"

/* Heavens Gate - see heavens_gate.h. The room half is src/lightnings_edge.c's
   (itself the Lava Maze's, A Woman Scorned's and Hell Hath No Fury's): cull
   keys, radius side cull and all. */

static SMD  *hg_smd  = NULL;
static void *hg_buff = NULL;

/* ---- The room --------------------------------------------------------------
   x[0,5599] z[0,5400] (the collision proxy's bounds), one floor at y=0: a
   lawn inside an octagon of 600-tall proxy walls that the drawn mesh does not
   show - there is no drawn rim, only the grass, the gate and the two doors,
   standing in the chapter's sky. The gate's masonry is the tallest drawn
   thing (y=-1073). The sky is centred on the middle of the lawn, half way up
   the gate. */
#define HG_CX             2800
#define HG_CZ             2700
#define HG_SKY_Y         (-536)

#define HG_FLOOR_Y          0
#define HG_EYE_Y         (HG_FLOOR_Y - GROUND_FLOOR_Y - 40)
/* Above both the drawn mesh (the gate's masonry, -1073) and the proxy's
   walls (-600). */
#define HG_CEILING_Y     (-1074)

/* ---- THE GATE --------------------------------------------------------------
   A brick gatehouse against the south wall, x[1600,3600] z[0,2400], its two
   leaves (heavens gate right x[1745,2600], left x[2600,3454]) in the planes
   z=2254/2346 facing north onto the lawn, and the stairs behind them rising
   SOUTH from y=0 at z=2200 to y=-1000 at z=0. The proxy boxes the whole of
   it in (walls 1, 3, 10, 23, 26, 28 and 30, every normal facing out of it),
   so the gate is SHUT and the stairs are scenery: nothing here opens it yet. */

/* ---- THE NORTH-WEST DOOR, to the Lava Maze ---------------------------------
   The greenhouse door_128 polys stand on the diagonal x - z = -4200, their
   foot (600,4800)..(1000,5200), in collision wall 22 ((1200,5400)-(600,4800),
   inward normal (+2896,-2896), i.e. south-east). Like the Underworld's other
   doors it is DISTORTED: its top edge flares out to (530,4700)..(1295,5446)
   at y=-500. So:
     - the PRESS point is the middle of its foot, (800,5000);
     - THE SIGN is 11 off it along the normal at y=-250, the middle of the
       face, on a free yaw facing the way the reader faces - 3584, north-west;
     - THE SPAWN, for an arrival through it, is 250 in along the normal
       ((977,4823)), clear of the 195 push radius, facing south-east (1536),
       towards the gate.
   Out of here plays DOOR_PANEL_GREENHOUSE: its art (greenhouse door.tim,
   x320 y256) is put up by this room's own upload, because the panel draws
   before STATE_LOADING. Its sound is SFX_DOOR, resident. The far side is a
   wooden door, so the way in plays DOOR_PANEL_WOOD (the user's choice, per
   direction). */
#define HG_NW_X            800
#define HG_NW_Z           5000
#define HG_NW_TEXT_X       808
#define HG_NW_TEXT_Z      4992
#define HG_NW_TEXT_Y     (HG_FLOOR_Y - 250)
#define HG_NW_TEXT_YAW    3584
#define HG_NW_SPAWN_X      977
#define HG_NW_SPAWN_Z     4823
#define HG_NW_SPAWN_ROT   1536

/* ---- THE EAST DOOR ---------------------------------------------------------
   The catacomb inner door polys stand in the plane x=5600, z[1600,2800], only
   150 tall (collision wall 6, inward normal -X). It leads nowhere yet: no
   prompt and no trigger until its room exists (the user's call, Nov 2026). */

#define HG_TEXT_RADIUS    1500
#define HG_FADE_NEAR      1000
#define HG_TRIGGER_RADIUS  500

/* Circle edge-detect. Seeded "held" by the arm so a press carried in through
   the transition cannot fire on the arrival frame. */
static int nw_circle_prev = 1;

/* ---- THE FLOOR -------------------------------------------------------------
   One storey: the proxy has one floor plane, y=0, wider than the walls, so
   one FLOOR_FLAT zone over the collision bounds. The walls keep the player
   out of the cut corners and out of the gatehouse. There is no lava here: the
   mesh has no lava texture at all, so the chapter's rule 5 has nothing to
   mask and the room has no ROOMS line in tools/gen_lava_floors.py (as
   Lightnings Edge and the Chamber of Destiny). */
static void hg_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = 0;      floor_zones[0].max_x = 5599;
    floor_zones[0].min_z = 0;      floor_zones[0].max_z = 5400;
    floor_zones[0].y     = HG_FLOOR_Y;
    floor_zone_count = 1;
}

/* ---- Textures --------------------------------------------------------------
   SEVEN mesh textures, FIVE owned. Every owned one is 4bpp - the user
   supplied the four heavens textures as 16 colours, and the door is the 4bpp
   greenhouse door doubled - on a Voff-0 page start whose tenants this room
   never draws and whose own rooms re-upload them on entry
   (tools/VRAM_MAP_UNDERWORLD.txt), each with a free 16-word CLUT on y=511:
     0  grss                 the Conservatory's GRSS.TIM (4bpp x768 y0, CLUT
                             80,511), through conservatory_upload_grss() -
                             whose registrations are in the Underworld bank
                             for this room. At its own address: down here
                             x768 y0 is the cracked tablet's, Destiny's
                             Door's and the Lightning Tablet's Ninurta page,
                             each re-uploaded by its own room.
     1  heavens bricks       OWNED, HVNBRCK.TIM  x384 y0   CLUT (928,511) -
                             cobblestones' page (the Plinth's frame and the
                             Chamber's tablet too)
     2  heavens stairs       OWNED, HVNSTRS.TIM  x512 y0   CLUT (944,511) -
                             rocks' page (grdn_gte's, the frame's, the tablet's)
     3  heavens gate left    OWNED, HVNGTL.TIM   x576 y0   CLUT (960,511) -
                             the seal's page (and Ribcage's and the Lava
                             Maze's door copies, which they re-upload)
     4  heavens gate right   OWNED, HVNGTR.TIM   x704 y0   CLUT (976,511) -
                             the tablets' page
     5  greenhouse door_128  OWNED, GRNHDR12.TIM x384 y256 CLUT (992,511) -
                             volcanic's page. greenhouse door.tim's own 64x64
                             pixels doubled to 128x128 (the same 16 colours),
                             because the mesh was UV'd on a 128 stretch of it
                             and the original's UVs run 0..63.
     6  catacomb inner door  the Catacombs Entry's (x832 y0), its narrow
                             uploader. The east wall's door.
   ...and, drawn by no poly here but by the transition OUT: the greenhouse
   door itself (GRNHSDR.TIM, 4bpp x320 y256, CLUT 272,511), through
   stables_upload_greenhouse_door(), for DOOR_PANEL_GREENHOUSE. x320 y256 is
   lava's page down here and this room has no lava; every room that draws
   lava re-uploads it.
   The sky's texture is src/underworld_sky.c's business. */
#define HG_TEX_COUNT 7

static uint16_t tex_tpage[HG_TEX_COUNT];
static uint16_t tex_clut[HG_TEX_COUNT];

/* The five textures this room owns, in slot order 1..5. */
static const char *const new_tex_file[5] = {
    "\\TEXUNDW\\HVNBRCK.TIM;1",
    "\\TEXUNDW\\HVNSTRS.TIM;1",
    "\\TEXUNDW\\HVNGTL.TIM;1",
    "\\TEXUNDW\\HVNGTR.TIM;1",
    "\\TEXUNDW\\GRNHDR12.TIM;1",
};
static int new_tex_id[5] = { -1, -1, -1, -1, -1 };

void heavens_gate_load_assets(void) {
    int i;
    /* BANK: the five are drawn here alone. Derived, not guessed - py
       tools/check_tex_banks.py. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    for (i = 0; i < 5; i++)
        new_tex_id[i] = texmgr_register(new_tex_file[i]);

    TIM_SLOT(0, GRSS);
    TIM_SLOT(1, HVNBRCK);
    TIM_SLOT(2, HVNSTRS);
    TIM_SLOT(3, HVNGTL);
    TIM_SLOT(4, HVNGTR);
    TIM_SLOT(5, GRNHDR12);
    TIM_SLOT(6, CTCMBDR);
}

/* Pure LoadImage, inside main's STATE_LOADING (which DrawSyncs first). */
void heavens_gate_upload_textures(void) {
    int i;
    conservatory_upload_grss();
    for (i = 0; i < 5; i++)
        texmgr_upload(new_tex_id[i]);
    catacombs_entry_upload_inner_door();
    stables_upload_greenhouse_door();    /* the panel out of here */
    underworld_sky_upload_textures();
}

/* ---- The cull key, and its RADIUS ------------------------------------------
   hg_build_cull_keys() verbatim (src/lightnings_edge.c, itself the Lava Maze's;
   tools/DIAGNOSING_FRAME_RATE.txt STEP 3I for why the radius). Built on the
   same call that reloads the mesh it describes, and nowhere else. */
static int     hg_key_count = 0;
static int32_t hg_y_lo = 0, hg_y_hi = 0;

static void hg_build_cull_keys(void) {
    hg_key_count = 0;
    if (!hg_smd) return;
    uint8_t *p = (uint8_t *)hg_smd->p_prims;
    int i, n = hg_smd->n_prims;
    if (n > HEAVENS_GATE_PRIM_COUNT) n = HEAVENS_GATE_PRIM_COUNT;
    hg_y_lo = hg_y_hi = 0;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &hg_smd->p_verts[vi[0]];
        int nv = (pt->type >= 2) ? 4 : 3;
        int16_t lo_x = v0->vx, hi_x = v0->vx;
        int16_t lo_z = v0->vz, hi_z = v0->vz;
        int k;
        for (k = 0; k < nv; k++) {
            SVECTOR *vk = &hg_smd->p_verts[vi[k]];
            if (vk->vx < lo_x) lo_x = vk->vx;
            if (vk->vx > hi_x) hi_x = vk->vx;
            if (vk->vz < lo_z) lo_z = vk->vz;
            if (vk->vz > hi_z) hi_z = vk->vz;
            if (vk->vy < hg_y_lo) hg_y_lo = vk->vy;
            if (vk->vy > hg_y_hi) hg_y_hi = vk->vy;
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
    hg_key_count = n;
}

void heavens_gate_load_geometry(void) {
    hg_buff = room_arena_load("\\TEXUNDW\\HVNGATE.SMD;1");
    hg_smd  = hg_buff ? smdInitData(hg_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (hg_smd && !room_data_bind(hg_smd->n_prims)) hg_smd = NULL;
    hg_build_cull_keys();
    /* The sky: kept across Chapter 4's rooms (a no-op when already held). */
    underworld_sky_load();
}

/* ---- The north-west door ---------------------------------------------------*/
void heavens_gate_doors_arm(void) {
    nw_circle_prev = interact_tapped();
}

/* A fresh Circle within reach of the door and facing it. The edge state is
   kept up to date even while locked. */
int heavens_gate_nw_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !nw_circle_prev;
    int32_t dx, dz;
    nw_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - HG_NW_X;
    dz = cam_z - HG_NW_Z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= HG_TRIGGER_RADIUS)
        return 0;
    return interact_facing(HG_NW_X, HG_NW_Z);
}

static void hg_nw_text(RenderContext *ctx) {
    int32_t dx = cam_x - HG_NW_X;
    int32_t dz = cam_z - HG_NW_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= HG_TEXT_RADIUS) return;
    if (xz > HG_FADE_NEAR) {
        int range = HG_TEXT_RADIUS - HG_FADE_NEAR;
        int prog  = xz - HG_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            HG_NW_TEXT_X, HG_NW_TEXT_Y, HG_NW_TEXT_Z,
                            50, 255, 50, fade, HG_NW_TEXT_YAW,
                            DOOR_PIXEL_SIZE);
}

void heavens_gate_spawn_nw(void) {
    cam_x     = HG_NW_SPAWN_X;
    cam_y     = HG_EYE_Y;
    cam_vy    = 0;
    cam_z     = HG_NW_SPAWN_Z;
    cam_rot   = HG_NW_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();
    heavens_gate_doors_arm();
}

void heavens_gate_init(void) {
    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(HG_CEILING_Y);
    hg_floor_zones_init();

    /* The one arrival, and where a debug jump lands: the north-west door,
       from the Lava Maze's south-east door. */
    heavens_gate_spawn_nw();

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
   draw_le_smd() (src/lightnings_edge.c, itself the Lava Maze's) verbatim: the chapter's fog, set in
   src/underworld.h and changed by nothing, the key-point-and-radius side cull,
   and far primitives clamped one OT slot in front of the sky. */
static void draw_hg_smd(RenderContext *ctx) {
    if (!hg_smd) return;

    uint8_t *p = (uint8_t *)hg_smd->p_prims;
    int i, n = hg_key_count;

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
#define HG_SIDE_MARGIN  (8 << 12)
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    int     pitched    = (cam_pitch != 0);
    int32_t pc = icos(cam_pitch);
    int32_t pslack = 0;
    if (pc < 0) no_frustum = 1;
    if (pitched) {
        int32_t ps   = isin(cam_pitch);
        int32_t dylo = hg_y_lo - cam_y, dyhi = hg_y_hi - cam_y;
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
            f = f * 5 + HG_SIDE_MARGIN + r;
            if (sd * 8 > f || -sd * 8 > f) { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &hg_smd->p_verts[vi[0]];
        SVECTOR *v1 = &hg_smd->p_verts[vi[1]];
        SVECTOR *v2 = &hg_smd->p_verts[vi[2]];

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

        int nocull = (i < HEAVENS_GATE_PRIM_COUNT) && room_nocull(i);
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
            v3 = &hg_smd->p_verts[vi[3]];
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

        uint8_t tex_idx = (i < HEAVENS_GATE_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < HG_TEX_COUNT);
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

void heavens_gate_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THE CHAPTER'S FOG, and nothing else's (src/underworld.h). */
    g_fog_near = UW_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : UW_FOG_FAR;

    /* The clear colour, not a full-screen TILE (wrong turn #3). The sky is an
       open ring, so this is what shows above and below it. */
    render_set_clear_colour(ctx, UW_FOG_R, UW_FOG_G, UW_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All seven room textures and the sky's sit at Voff 0. No enemy sheet. */
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
        underworld_sky_draw(ctx, HG_CX, HG_SKY_Y, HG_CZ);
        draw_hg_smd(ctx);
    }

    /* >>> LEVEL 8 IS THE DOOR SIGN <<< - no props, no enemies - and any
       pickup a later seed puts down. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        item_pickups_draw(ctx);
        hg_nw_text(ctx);
    }
}
