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
#include "chamber_of_destiny.h"
#include "chamber_of_destiny_tex_map.h"
#include "underworld.h"
#include "underworld_sky.h"
#include "heros_plinth.h"       /* Destiny's Door's art, a narrow uploader    */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "particles.h"
#include "btn_glyph.h"
#include "door.h"

/* The Chamber of Destiny - see chamber_of_destiny.h. The room half is
   src/heros_plinth.c's (itself Wingscape's, itself Hell Hath No Fury's): cull
   keys, the radius side cull and all, without the jewel ceremony. */

static SMD  *cd_smd  = NULL;
static void *cd_buff = NULL;

/* ---- The room --------------------------------------------------------------
   x[-6600,0] z[-1800,1800] (the collision proxy's bounds). The SKY is the
   dome, modelled in this room's own coordinates, so it is drawn centred on
   (0,0,0) - underworld_sky_draw adds the centre to every vertex. */
#define CD_SKY_X             0
#define CD_SKY_Y             0
#define CD_SKY_Z             0

#define CD_FLOOR_Y           0
#define CD_EYE_Y          (CD_FLOOR_Y - GROUND_FLOOR_Y - 40)   /* Hero's Plinth's */
/* The tallest drawn thing is the Ninurta tablet's top, y=-1293. */
#define CD_CEILING_Y      (-1300)

/* The dais and its ramp (collision floors 0 and 2). */
#define CD_DAIS_Y         (-200)

/* >>> BLACK, NOT THE CHAPTER'S RED. <<< The clear colour, and the fog colour
   with it (they are one colour by the chapter's rule). See
   chamber_of_destiny.h. */
#define CD_FOG_R             0
#define CD_FOG_G             0
#define CD_FOG_B             0

/* >>> AND A SHORTER FOG THAN THE CHAPTER'S, BY THE USER'S ORDER (Nov 2026):
   "only far enough that the player can see the Ninurta tablet from the top of
   the ramp". Measured off the export the way the loop measures (Manhattan,
   the cull on each face's first vertex, the fog on its centre): from anywhere
   on the ramp's top edge (x=-4800, z[-400,400]) every tablet face is within
   2800, and within 2400 from its middle. It was 3000 first; the user then
   set 2400 - the whole tablet still drawn from the MIDDLE of the ramp top,
   but faint there, and from the ramp's corners its far faces are past the
   cull. Re-measure if the tablet or the ramp moves. The NEAR stays the
   chapter's. */
#define CD_FOG_NEAR    UW_FOG_NEAR
#define CD_FOG_FAR          2400

/* ---- THE DOOR, Destiny's Door, back to Hero's Plinth -----------------------
   The destinys door polys stand on the corridor's east wall, x=0 z[-400,400]
   y[-600,0] (collision wall 0, inward normal -X): centre (0,0), approached
   from the west.

   THE SIGN on a free yaw facing the way the reader faces - 1024, east - 11
   off the door toward the player. THE SPAWN, for an arrival through it, is
   250 in along the normal, clear of the 195 push radius, facing west down
   the corridor. */
#define CD_DOOR_X              0
#define CD_DOOR_Z              0
#define CD_DOOR_TEXT_X      (-11)
#define CD_DOOR_TEXT_Z         0
#define CD_DOOR_TEXT_YAW    1024
#define CD_DOOR_SPAWN_X     (-250)
#define CD_DOOR_SPAWN_Z        0
#define CD_DOOR_SPAWN_ROT   3072

#define CD_DOOR_TEXT_Y    (CD_FLOOR_Y - 200)
#define CD_TEXT_RADIUS     1500
#define CD_FADE_NEAR       1000
#define CD_TRIGGER_RADIUS   500

/* Circle edge-detect. Seeded "held" by the arm so a press carried in through
   the transition cannot fire on the arrival frame. */
static int door_circle_prev = 1;

/* ---- THE FLOOR ZONES -------------------------------------------------------
   Two storeys, laid over one another in plan, so ORDER IS THE MECHANISM:
   apply_height takes the first zone not above the player, so the dais and
   its ramp come first and the y=0 catch-all LAST (tools/ADDING_A_ROOM.txt
   STEP 5, the Up Down Maze's note). */
static void cd_floor_zones_init(void) {
    int i = 0;

    /* THE DAIS, y=-200 (collision floor 2). */
    floor_zones[i].type  = FLOOR_UPPER;
    floor_zones[i].min_x = -6000;  floor_zones[i].max_x = -4800;
    floor_zones[i].min_z = -1200;  floor_zones[i].max_z =  1200;
    floor_zones[i].y     = CD_DAIS_Y;
    i++;

    /* THE RAMP up to it, rising WEST: y=0 at x=-4000, -200 at x=-4800. */
    floor_zones[i].type            = FLOOR_RAMP;
    floor_zones[i].min_x = -4800;  floor_zones[i].max_x = -4000;
    floor_zones[i].min_z =  -400;  floor_zones[i].max_z =   400;
    floor_zones[i].ramp_y_start    = CD_FLOOR_Y;   /* at x=-4000 */
    floor_zones[i].ramp_y_end      = CD_DAIS_Y;    /* at x=-4800 */
    floor_zones[i].ramp_axis_start = -4000;
    floor_zones[i].ramp_axis_end   = -4800;
    floor_zones[i].ramp_along_x    = 1;
    i++;

    /* THE FLOOR, y=0: corridor and chamber, the catch-all, LAST. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -6600;  floor_zones[i].max_x =    0;
    floor_zones[i].min_z = -1800;  floor_zones[i].max_z = 1800;
    floor_zones[i].y     = CD_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   SEVEN mesh textures, SIX owned:
     0  marble                       OWNED, 8bpp x320 y256, CLUT (256,490) -
                                     the lava's page (this room draws no lava)
                                     and the closed drawer's CLUT line
     1  night sky                    OWNED, 4bpp x384 y256, CLUT (928,502) -
                                     the volcanic's page and CLUT. 4bpp because
                                     the art was SUPPLIED as 16 colours. Also
                                     the DOME's texture (src/underworld_sky.c).
     2  destinys door                Hero's Plinth's, its narrow uploader (8bpp
                                     x768 y0) - also DOOR_PANEL_DESTINY's panel,
                                     the way out, so it MUST be uploaded here
                                     (the panel draws before STATE_LOADING)
     3  ninurta tablet top left      OWNED, 8bpp x384 y0,   CLUT (672,499)
     4  ninurta tablet top right     OWNED, 8bpp x512 y0,   CLUT (672,500)
     5  ninurta tablet bottom left   OWNED, 8bpp x896 y256, CLUT (672,490)
     6  ninurta tablet bottom right  OWNED, 8bpp x704 y0,   CLUT (672,484)
   The tablet's four quarters take the four pages AND CLUTs of Destiny's
   Door's FRAME, which this room never draws: the frame's own uploads put
   them back on entry to Hero's Plinth, the only way out. Every other
   displaced tenant (lava, volcanic, the closed drawer) is re-uploaded by its
   own room on entry too (tools/VRAM_MAP_UNDERWORLD.txt). Registered in
   tools/vram_map.py KNOWN_STREAM_PAIRS.
   >>> ALL OF THE ROOM'S OWN FULL-COLOUR ART IS 8bpp. <<< Never drop one to
   16 colours without asking (tools/ADDING_A_ROOM.txt STEP 3d). */
#define CD_TEX_COUNT 7
#define CD_OWNED     6

static uint16_t tex_tpage[CD_TEX_COUNT];
static uint16_t tex_clut[CD_TEX_COUNT];
static int      owned_tex[CD_OWNED] = { -1, -1, -1, -1, -1, -1 };

void chamber_of_destiny_load_assets(void) {
    /* BANK: Chapter 4. Derived, not guessed - py tools/check_tex_banks.py walks
       the uploader call graph and fails the build if this is short. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    owned_tex[0] = texmgr_register("\\TEXUNDW\\MARBLE.TIM;1");
    owned_tex[1] = texmgr_register("\\TEXUNDW\\NIGHTSKY.TIM;1");
    owned_tex[2] = texmgr_register("\\TEXUNDW\\NTABTL.TIM;1");
    owned_tex[3] = texmgr_register("\\TEXUNDW\\NTABTR.TIM;1");
    owned_tex[4] = texmgr_register("\\TEXUNDW\\NTABBL.TIM;1");
    owned_tex[5] = texmgr_register("\\TEXUNDW\\NTABBR.TIM;1");
    TIM_SLOT(0, MARBLE);
    TIM_SLOT(1, NIGHTSKY);
    TIM_SLOT(2, DSTNYDR);
    TIM_SLOT(3, NTABTL);
    TIM_SLOT(4, NTABTR);
    TIM_SLOT(5, NTABBL);
    TIM_SLOT(6, NTABBR);
}

/* Pure LoadImage from the RAM copies texmgr holds - no CD access, safe during
   the transition (main's STATE_LOADING DrawSyncs first). The chapter sky's
   uploader is called although this room draws the dome, not the ring: it is
   also what re-stamps the chapter's ITEM art (src/underworld.h rule 5), and
   the hell bg it puts up at x640 y0 is on a page this room does not use. */
void chamber_of_destiny_upload_textures(void) {
    int i;
    heros_plinth_upload_destiny_door();
    for (i = 0; i < CD_OWNED; i++)
        if (owned_tex[i] >= 0) texmgr_upload(owned_tex[i]);
    underworld_sky_upload_textures();
}

/* ---- The cull key, and its RADIUS ------------------------------------------
   hp_build_cull_keys() verbatim (src/heros_plinth.c, Wingscape's, Hell Hath
   No Fury's; tools/DIAGNOSING_FRAME_RATE.txt STEP 3I for why the radius).
   Built on the same call that reloads the mesh it describes, and nowhere
   else. */
static int     cd_key_count = 0;
static int32_t cd_y_lo = 0, cd_y_hi = 0;

static void cd_build_cull_keys(void) {
    cd_key_count = 0;
    if (!cd_smd) return;
    uint8_t *p = (uint8_t *)cd_smd->p_prims;
    int i, n = cd_smd->n_prims;
    if (n > CHAMBER_OF_DESTINY_PRIM_COUNT) n = CHAMBER_OF_DESTINY_PRIM_COUNT;
    cd_y_lo = cd_y_hi = 0;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &cd_smd->p_verts[vi[0]];
        int nv = (pt->type >= 2) ? 4 : 3;
        int16_t lo_x = v0->vx, hi_x = v0->vx;
        int16_t lo_z = v0->vz, hi_z = v0->vz;
        int k;
        for (k = 0; k < nv; k++) {
            SVECTOR *vk = &cd_smd->p_verts[vi[k]];
            if (vk->vx < lo_x) lo_x = vk->vx;
            if (vk->vx > hi_x) hi_x = vk->vx;
            if (vk->vz < lo_z) lo_z = vk->vz;
            if (vk->vz > hi_z) hi_z = vk->vz;
            if (vk->vy < cd_y_lo) cd_y_lo = vk->vy;
            if (vk->vy > cd_y_hi) cd_y_hi = vk->vy;
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
    cd_key_count = n;
}

void chamber_of_destiny_load_geometry(void) {
    cd_buff = room_arena_load("\\TEXUNDW\\CHMBDST.SMD;1");
    cd_smd  = cd_buff ? smdInitData(cd_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (cd_smd && !room_data_bind(cd_smd->n_prims)) cd_smd = NULL;
    cd_build_cull_keys();
    /* THE DOME, not the chapter's ring: underworld_sky_load_destiny swaps the
       held model (the ring is freed on the way in, read back on the way out). */
    underworld_sky_load_destiny();
}

/* ---- The door --------------------------------------------------------------*/
void chamber_of_destiny_door_arm(void) {
    door_circle_prev = interact_tapped();
}

int chamber_of_destiny_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !door_circle_prev;
    int32_t dx, dz;
    door_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - CD_DOOR_X;
    dz = cam_z - CD_DOOR_Z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= CD_TRIGGER_RADIUS)
        return 0;
    return interact_facing(CD_DOOR_X, CD_DOOR_Z);
}

static void cd_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - CD_DOOR_X;
    int32_t dz = cam_z - CD_DOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= CD_TEXT_RADIUS) return;
    if (xz > CD_FADE_NEAR) {
        int range = CD_TEXT_RADIUS - CD_FADE_NEAR;
        int prog  = xz - CD_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            CD_DOOR_TEXT_X, CD_DOOR_TEXT_Y, CD_DOOR_TEXT_Z,
                            50, 255, 50, fade, CD_DOOR_TEXT_YAW,
                            DOOR_PIXEL_SIZE);
}

void chamber_of_destiny_spawn_door(void) {
    cam_x     = CD_DOOR_SPAWN_X;
    cam_y     = CD_EYE_Y;
    cam_vy    = 0;
    cam_z     = CD_DOOR_SPAWN_Z;
    cam_rot   = CD_DOOR_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();
    chamber_of_destiny_door_arm();
}

void chamber_of_destiny_init(void) {
    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(CD_CEILING_Y);
    cd_floor_zones_init();

    /* The one arrival, and where a debug jump lands. */
    chamber_of_destiny_spawn_door();

    /* The global prop arrays, as Hero's Plinth clears them: Hell Hath No
       Fury's save point and tank would otherwise block invisibly inside this
       room's bounds. The clear drops the placement, never OD_TANK_HHNF's oil. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    bars_clear();
    reset_particles();
}

/* ---- The mesh --------------------------------------------------------------
   draw_hp_smd() (src/heros_plinth.c) without Destiny's Door's crossfade: the
   room's own fog (CD_FOG_NEAR/FAR) fading to its BLACK, the
   key-point-and-radius side cull, and far primitives clamped one OT slot in
   front of the sky. */
static void draw_cd_smd(RenderContext *ctx) {
    if (!cd_smd) return;

    uint8_t *p = (uint8_t *)cd_smd->p_prims;
    int i, n = cd_key_count;

    /* HOISTED OUT OF THE LOOP (STEP 3C fix 1): none of these can change while
       a frame is being queued. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = CD_FOG_FAR;
    int32_t fog_start = CD_FOG_NEAR < cull ? CD_FOG_NEAR : cull / 2;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    /* ---- THE SIDE-PLANE CULL'S PER-FRAME TERMS ------------------------------
       Hell Hath No Fury's, unchanged: see draw_hhnf_smd for the derivation
       (the pitch bound, the 10*R radius term and the arithmetic). */
#define CD_SIDE_MARGIN  (8 << 12)
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    int     pitched    = (cam_pitch != 0);
    int32_t pc = icos(cam_pitch);
    int32_t pslack = 0;
    if (pc < 0) no_frustum = 1;
    if (pitched) {
        int32_t ps   = isin(cam_pitch);
        int32_t dylo = cd_y_lo - cam_y, dyhi = cd_y_hi - cam_y;
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
            f = f * 5 + CD_SIDE_MARGIN + r;
            if (sd * 8 > f || -sd * 8 > f) { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &cd_smd->p_verts[vi[0]];
        SVECTOR *v1 = &cd_smd->p_verts[vi[1]];
        SVECTOR *v2 = &cd_smd->p_verts[vi[2]];

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

        int nocull = (i < CHAMBER_OF_DESTINY_PRIM_COUNT) && room_nocull(i);
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
            v3 = &cd_smd->p_verts[vi[3]];
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

        uint8_t tex_idx = (i < CHAMBER_OF_DESTINY_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < CD_TEX_COUNT);
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + CD_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + CD_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + CD_FOG_B * (256 - fog_factor)) >> 8);

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

void chamber_of_destiny_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS ROOM'S FOG: the chapter's near, its own shorter far (CD_FOG_FAR). */
    g_fog_near = CD_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : CD_FOG_FAR;

    /* The clear colour, not a full-screen TILE (wrong turn #3): BLACK here,
       the user's, where the chapter's is red. It shows below the dome's rim. */
    render_set_clear_colour(ctx, CD_FOG_R, CD_FOG_G, CD_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All seven room textures sit at Voff 0, and the dome's generated UVs
       rely on the wrap; no sprite is drawn here. */
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
        underworld_sky_draw(ctx, CD_SKY_X, CD_SKY_Y, CD_SKY_Z);
        draw_cd_smd(ctx);
    }

    /* >>> LEVEL 8 IS THE DOOR'S SIGN, the only thing standing in the room. */
    if (exp != DBG_EXP_NO_ENTITIES)
        cd_door_text(ctx);
}
