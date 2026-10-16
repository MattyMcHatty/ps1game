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
#include "lamashtu_arena.h"
#include "lumberer.h"
#include "crawler.h"
#include "collision.h"
#include "room_data.h"
#include "lamashtu_arena_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "catacombs_entry.h"    /* the three narrow uploaders this room borrows */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons             */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */
#include "lamashtu.h"           /* the boss: drawn, reset and textured from here */
#include "lms_creep.h"          /* her Creeps' sheet, in VRAM for the fight      */
#include "bars.h"               /* the fight's gates (src/lamashtu_boss.c)       */
#include "lamashtu_boss.h"      /* the encounter: its seal, its lights and lines */

/* The Lamashtu Arena — see lamashtu_arena.h. Copied from src/the_head.c, next
   door, with the relief and the arm switches taken out, the Tomb's three
   textures put in and the Kennels' view distance. */

static SMD  *la_smd  = NULL;
static void *la_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   1300 / 1800 / 2200 - at rest, with the lantern in hand, and with it lit:
   the user's figures, SHORTER than the Kennels' 1300 / 1900 / 2500 under the
   lantern. Anything that lengthens these has to be measured BURNING, not at
   rest - this is the biggest mesh on the disc.

   >>> THE EASED QUANTITY IS FOG-FAR ITSELF, IN WORLD UNITS, not the Kennels'
   256ths scale. <<< 1300 x n/256 cannot land on 1800 or 2200 (the nearest are
   1797 and 2193), so la_view holds the distance and fog-near follows it at the
   Kennels' 366:1300 ratio. LA_VIEW_RATE 40 is the Kennels' 8/256 of 1300 a
   frame, so the ease feels the same. */
#define LA_BASE_FOG_NEAR   366
#define LA_BASE_FOG_FAR   1300

#define LA_VIEW_REST      1300   /* fog-far at rest                       */
#define LA_VIEW_HELL      1800   /* ...with the lantern in hand            */
#define LA_VIEW_BURN      2200   /* ...and while it is actually lit        */
#define LA_VIEW_RATE        40   /* world units a frame                    */

static int32_t la_view     = LA_VIEW_REST;       /* eased fog-far, world units */
static int32_t la_fog_near = LA_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t la_fog_far  = LA_BASE_FOG_FAR;

static int32_t la_view_target(void) {
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR)))
        return helluminator_burning() ? LA_VIEW_BURN : LA_VIEW_HELL;
    return LA_VIEW_REST;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void la_view_resolve(int snap) {
    int32_t target = la_view_target();
    if (snap) {
        la_view = target;
    } else if (la_view < target) {
        la_view += LA_VIEW_RATE;
        if (la_view > target) la_view = target;
    } else if (la_view > target) {
        la_view -= LA_VIEW_RATE;
        if (la_view < target) la_view = target;
    }
    la_fog_far  = la_view;
    la_fog_near = (LA_BASE_FOG_NEAR * la_view) / LA_BASE_FOG_FAR;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black) - the Kennels' colour too. */
#define LA_FOG_R             7
#define LA_FOG_G             6
#define LA_FOG_B             9

/* Wall standoff. The chapter's 195. */
#define LA_WALL_RADIUS      195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define LA_FLOOR_Y            0
#define LA_EYE_Y           (LA_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   ONE, FLAT, AT y=0, over the proxy's one floor face (x[0,7799]
   z[-1200,4200], the FLOOR list at the foot of
   src/lamashtu_arena_mesh_collision.c). The pillars stand on it; their walls
   keep the player out of them. */
static void la_floor_zones_init(void) {
    int i = 0;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x =     0;  floor_zones[i].max_x = 7799;
    floor_zones[i].min_z = -1200;  floor_zones[i].max_z = 4200;
    floor_zones[i].y     = LA_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   THREE, ALL AT Voff 0, AND ALL BORROWED - the Tomb's set exactly:

     0 cobblestones        walls, floor, ceiling, pillars: 1771 of 2097 (x384 y0)
     1 loculus             the pillars' burial niches: 324              (x512 y0)
     2 catacomb inner door the west doorway, 2 polys                    (x832 y0)

   All three come through src/catacombs_entry.c's narrow uploaders, as the
   Tomb takes them, so there is nothing to register and no page to restore. */
#define LAMASHTU_ARENA_TEX_COUNT 3

static uint16_t tex_tpage[LAMASHTU_ARENA_TEX_COUNT];
static uint16_t tex_clut[LAMASHTU_ARENA_TEX_COUNT];

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/the_head.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int la_key_count = 0;

static void la_build_cull_keys(void) {
    la_key_count = 0;
    if (!la_smd) return;
    uint8_t *p = (uint8_t *)la_smd->p_prims;
    int i, n = la_smd->n_prims;
    if (n > LAMASHTU_ARENA_PRIM_COUNT) n = LAMASHTU_ARENA_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &la_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    la_key_count = n;
}

void lamashtu_arena_load_geometry(void) {
    la_buff = room_arena_load("\\TEXCTCMB\\LMSARENA.SMD;1");
    la_smd  = la_buff ? smdInitData(la_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of
       the same file (src/room_data.h). No block, no room. */
    if (la_smd && !room_data_bind(la_smd->n_prims)) la_smd = NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    la_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: three compile-time headers and no
   registration. NO texmgr_set_bank() HERE, and that is correct rather than an
   omission: the room registers nothing (the Tomb's terms, src/tomb.c). The
   three textures are catacombs_entry.c's, whose own registration declares the
   bank; py tools/check_tex_banks.py walks the uploader graph and checks it. */
void lamashtu_arena_load_assets(void) {
    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, LOCULUS);
    TIM_SLOT(2, CTCMBDR);
}

/* Pure LoadImage from the RAM copies texmgr already holds — no CD access, safe
   during the transition (main's STATE_LOADING DrawSyncs first).

   THREE MORE FOR THE FIGHT, all registered by their own modules, so the room
   still owns nothing:
     lamashtu  her skin, x576 y0 - the CRIB's page; no crib stands in here and
               every room that has one uploads it on entry (src/lamashtu.h)
     lms creep the LAMASHTU CREEP's sheet, x640 y320 - the Creep's own slot,
               time-shared: no crib stands in here (src/lms_creep.h)
     bars      the gate beside her, x512 y256 */
void lamashtu_arena_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_loculus();
    catacombs_entry_upload_inner_door();
    lamashtu_upload_texture();
    lms_creeps_upload_texture();
    bars_upload_texture();
}

/* ---- THE DOOR --------------------------------------------------------------
   ONE, WIRED: x=0, z[1400,1600], y[-400,0], on the hall's door aisle.

     WEST  x=0  -> The Head, through that room's east door

   In the YZ plane at fixed X, approached from +X (wall 0 runs x=0 with
   nx=+4095), so TEXT_PLANE_YZ with mirror=0 and the sign 11 proud of the wall
   along +X - The Head's west door's terms. The reading axis for a YZ sign is
   Z, so the -200 door_draw_string_3d wants goes on the Z argument. */
#define LA_WEST_X               0
#define LA_WEST_Z            1500
#define LA_DOOR_TEXT_Y       (-186)   /* eye level on the y=0 floor */
#define LA_TEXT_RADIUS       1200
#define LA_FADE_NEAR          800
#define LA_TRIGGER_RADIUS     500

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int west_circle_prev = 1;

void lamashtu_arena_arm(void) {
    west_circle_prev = interact_tapped();
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int lamashtu_arena_west_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !west_circle_prev;
    int32_t dx, dz, xz;
    west_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - LA_WEST_X;
    dz = cam_z - LA_WEST_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= LA_TRIGGER_RADIUS) return 0;
    if (!interact_facing(LA_WEST_X, LA_WEST_Z)) return 0;
    return 1;
}

/* The floating sign. Same shape as every other sign in the game: opaque within
   LA_FADE_NEAR, gone by LA_TEXT_RADIUS. */
static void la_west_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - LA_WEST_X;
    int32_t dz = cam_z - LA_WEST_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= LA_TEXT_RADIUS) return;
    if (xz > LA_FADE_NEAR) {
        int range = LA_TEXT_RADIUS - LA_FADE_NEAR;
        int prog  = xz - LA_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        LA_WEST_X + 11, LA_DOOR_TEXT_Y, LA_WEST_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void lamashtu_arena_spawn_west(void) {
    /* 220 off wall 0 on its walkable +X side, on the door's centre line,
       facing +X down the hall. The first pillar column starts at x=600, so
       the spawn has 380 clear in front of it. */
    cam_x   = LA_WEST_X + (LA_WALL_RADIUS + 25);
    cam_y   = LA_EYE_Y;
    cam_vy  = 0;
    cam_z   = LA_WEST_Z;
    cam_rot = 1024;                    /* facing +X, east down the hall */
    lamashtu_arena_arm();
}

void lamashtu_arena_init(void) {
    room_data_collision(&current_collision_room);
    /* THE WALLS, read off the VISUAL mesh: the stone rises to y=-800, where
       the proxy's walls and pillars stop too. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(LA_WALL_RADIUS);

    la_floor_zones_init();
    cam_pitch = 0;

    lamashtu_arena_spawn_west();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every collide routine, so an
       instance left from another room would block invisibly anywhere it falls
       inside x[0,7799] z[-1200,4200]. Cleared as The Head clears them; safe,
       because catacombs_entry_init() re-places all four. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    /* LAMASHTU AND HER ENCOUNTER, on EVERY arrival - a debug jump finds what a
       walk-in does. The body comes up hidden at full health; the director
       arms itself on its first update (src/lamashtu_boss.h) unless she is
       dead, and places her, shows her and brings phase 1's gates up out of
       the floor. bars_clear() first: the gate instances are global, and the
       Kennels' and the Pit's would otherwise ride in. */
    bars_clear();
    lamashtu_reset();
    lamashtu_boss_enter();

    la_view_resolve(1);
}

static void draw_lamashtu_arena_smd(RenderContext *ctx) {
    if (!la_smd) return;

    uint8_t *p = (uint8_t *)la_smd->p_prims;
    int i, n = la_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = la_fog_far;   /* resolved by la_view_resolve() this frame */
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    /* THE VOID after her death (src/lamashtu_boss.h): the whole room scaled
       toward black, the fog colour with it. Out altogether, nothing is queued. */
    int32_t lit = lamashtu_boss_room_light();
    if (lit <= 0) return;
    int32_t fog_r = (LA_FOG_R * lit) >> 8;
    int32_t fog_g = (LA_FOG_G * lit) >> 8;
    int32_t fog_b = (LA_FOG_B * lit) >> 8;

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
        SVECTOR *v0 = &la_smd->p_verts[vi[0]];
        SVECTOR *v1 = &la_smd->p_verts[vi[1]];
        SVECTOR *v2 = &la_smd->p_verts[vi[2]];

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

        int nocull = (i < LAMASHTU_ARENA_PRIM_COUNT) && room_nocull(i);
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
            v3 = &la_smd->p_verts[vi[3]];
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
           must not be sorted over the doorway standing on it. */
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
        int32_t fog = dist < la_fog_near ? la_fog_near : (dist > la_fog_far ? la_fog_far : dist);
        int32_t fog_factor = ((la_fog_far - fog) << 8) / (la_fog_far - la_fog_near);
        fog_factor = (fog_factor * lit) >> 8;

        uint8_t tex_idx = (i < LAMASHTU_ARENA_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < LAMASHTU_ARENA_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + fog_r * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + fog_g * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + fog_b * (256 - fog_factor)) >> 8);

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

void lamashtu_arena_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    la_view_resolve(0);

    g_fog_near = la_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : la_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    {
        int32_t lit = lamashtu_boss_room_light();   /* the void: out to black */
        render_set_clear_colour(ctx, (LA_FOG_R * lit) >> 8, (LA_FOG_G * lit) >> 8,
                                (LA_FOG_B * lit) >> 8);
    }

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

    if (exp != DBG_EXP_NO_MESH) draw_lamashtu_arena_smd(ctx);

    /* LAMASHTU, straight after the room and under the same plain view: her
       vertices are world space and she loads no matrix (src/lamashtu.h). Her
       skin is at Voff 0 on a page-aligned slot, so the room's 128 window is
       hers too and there is nothing to bracket. Drawn at any range, unfogged -
       Gula's rule. Her shockwave draws with her. */
    if (exp != DBG_EXP_NO_ENTITIES) lamashtu_draw(ctx);

    /* >>> LEVEL 8 IS THE SIGN, THE GATE AND THE ENEMIES. <<< The Chapter 3
       enemy draws are area-tagged and cost nothing until something is placed;
       the Creep's is here because the fight will pour them. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        /* Not while the encounter seals the door: offering "Press O" on a
           door that will not answer is worse than offering nothing. */
        if (!lamashtu_boss_seals_door())
            la_west_door_text(ctx);    /* west: YZ plane, approached from +X */
        /* THE CHAPTER 3 ENEMIES. Their sheets sit at Voff 128, so each is
           handed the window to restore after drawing unmasked. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
            lumberers_set_texwindow(&tw);
            lms_creeps_set_texwindow(&tw);
        }
        draw_crawlers(ctx);
        draw_lumberers(ctx);
        draw_lms_creeps(ctx);
        /* AFTER the enemy sprites, which restore the 128 window this prop's
           UVs (0..192) depend on - bars.h says it is not happy either way. */
        bars_draw(ctx);
        /* The death's lights, which want the plain view the bars put back. */
        lamashtu_boss_draw(ctx);
    }
    /* The two lines, on top of everything. */
    lamashtu_boss_draw_overlay(ctx);
}
