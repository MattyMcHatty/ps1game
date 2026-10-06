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
#include "neck.h"
#include "lumberer.h"
#include "crawler.h"
#include "collision.h"
#include "room_data.h"
#include "neck_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons             */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* The Neck — see neck.h. Copied from src/throat.c, the last Chapter 3 room
   with no art of its own. */

static SMD  *nk_smd  = NULL;
static void *nk_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   450/1600 and the full +50%/+50% lantern bonus - the Room of Baby Names'
   numbers, next door, as the Throat takes them. */
#define NK_BASE_FOG_NEAR   450
#define NK_BASE_FOG_FAR   1600

#define NK_VIEW_UNIT        256
#define NK_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define NK_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */
#define NK_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t nk_view     = NK_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t nk_fog_near = NK_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t nk_fog_far  = NK_BASE_FOG_FAR;

static int32_t nk_view_target(void) {
    int32_t s = NK_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += NK_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += NK_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void nk_view_resolve(int snap) {
    int32_t target = nk_view_target();
    if (snap) {
        nk_view = target;
    } else if (nk_view < target) {
        nk_view += NK_VIEW_RATE;
        if (nk_view > target) nk_view = target;
    } else if (nk_view > target) {
        nk_view -= NK_VIEW_RATE;
        if (nk_view < target) nk_view = target;
    }
    nk_fog_near = (NK_BASE_FOG_NEAR * nk_view) >> 8;
    nk_fog_far  = (NK_BASE_FOG_FAR  * nk_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). */
#define NK_FOG_R             7
#define NK_FOG_G             6
#define NK_FOG_B             9

/* Wall standoff. The chapter's 195. The pillars stand 600 off the north and
   south walls, so the aisles either side of them are 210 wide walkable. */
#define NK_WALL_RADIUS      195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define NK_FLOOR_Y            0
#define NK_EYE_Y           (NK_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   ONE, FLAT, AT y=0, over the proxy's one floor face (x[0,2999] z[0,1800],
   the FLOOR list at the foot of src/neck_mesh_collision.c). The pillars are
   walls only; the zone runs under them and nobody can stand there. */
static void nk_floor_zones_init(void) {
    int i = 0;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x =    0;  floor_zones[i].max_x = 3000;
    floor_zones[i].min_z =    0;  floor_zones[i].max_z = 1800;
    floor_zones[i].y     = NK_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   TWO, BOTH BORROWED, BOTH AT Voff 0:

     0 cobblestones        walls, pillars, floor, ceiling: 329 of 333 (x384 y0)
     1 catacomb inner door the two doorways, 2 polys each            (x832 y0)

   Both come through src/catacombs_entry.c's narrow uploaders, as every
   Chapter 3 room takes them. The room owns nothing, so there is no
   registration and nothing of anybody else's to put back. */
#define NECK_TEX_COUNT 2

static uint16_t tex_tpage[NECK_TEX_COUNT];
static uint16_t tex_clut[NECK_TEX_COUNT];

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/throat.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int nk_key_count = 0;

static void nk_build_cull_keys(void) {
    nk_key_count = 0;
    if (!nk_smd) return;
    uint8_t *p = (uint8_t *)nk_smd->p_prims;
    int i, n = nk_smd->n_prims;
    if (n > NECK_PRIM_COUNT) n = NECK_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &nk_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    nk_key_count = n;
}

void neck_load_geometry(void) {
    nk_buff = room_arena_load("\\TEXCTCMB\\NECK.SMD;1");
    nk_smd  = nk_buff ? smdInitData(nk_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of
       the same file (src/room_data.h). No block, no room. */
    if (nk_smd && !room_data_bind(nk_smd->n_prims)) nk_smd = NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    nk_build_cull_keys();
}

/* STARTUP: two compile-time headers and no CD access. It declares the bank,
   which py tools/check_tex_banks.py checks against the uploader graph. */
void neck_load_assets(void) {
    texmgr_set_bank(TEXBANK_CATACOMBS);

    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, CTCMBDR);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first). */
void neck_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
}

/* ---- THE DOORS -------------------------------------------------------------
   TWO DRAWN, ONE WIRED. Both are z[800,1000], y[-400,0], on the centre line:

     WEST  x=0     -> the Room of Baby Names, through the door with the blank
                      plate over it (that room's north-east face)
     EAST  x=2999  -> nothing yet: drawn and sealed, no sign, no trigger

   The west door is in the YZ plane at fixed X, approached from +X (wall 1 runs
   x=0 with nx=+4096), so TEXT_PLANE_YZ with mirror=0 and the sign 11 proud of
   the wall along +X - the Room of Baby Names' own west door's terms. The
   reading axis for a YZ sign is Z, so the -200 door_draw_string_3d wants goes
   on the Z argument. */
#define NK_WEST_X               0
#define NK_WEST_Z             900
#define NK_DOOR_TEXT_Y       (-186)   /* eye level on the y=0 floor */
#define NK_TEXT_RADIUS       1200
#define NK_FADE_NEAR          800
#define NK_TRIGGER_RADIUS     500

/* ---- THE SAVE POINT --------------------------------------------------------
   In the NORTH-EAST CORNER, 200 off the east wall (x=2999) and 200 off the
   north wall (z=1800) - the Catacombs Entry's corner save on that room's
   194/195, rounded. The model's own footprint is 70 plus the 55 standoff
   save_points_collide is called with, so the player stops 125 short of it and
   is never pushed into the corner.

   y is the floor (0) less 300 and rot/scale are reception's, so it reads as
   the identical prop on the identical terms (reception's floor is y=0 too).

   It is the whole length of the hall from the wired door, so the two trigger
   circles never meet; main.c still asks the save point first and hands its
   answer to the door as a veto, the Catacombs Entry's order, so a sealed east
   door wired later cannot share a press with it. It is 399 clear of the east
   pillar's corner. */
#define NK_SAVE_X            2799
#define NK_SAVE_Y            (NK_FLOOR_Y - 300)
#define NK_SAVE_Z            1600

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int west_circle_prev = 1;

void neck_arm(void) {
    west_circle_prev = interact_tapped();
    save_point_arm();
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int neck_west_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !west_circle_prev;
    int32_t dx, dz, xz;
    west_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - NK_WEST_X;
    dz = cam_z - NK_WEST_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= NK_TRIGGER_RADIUS) return 0;
    if (!interact_facing(NK_WEST_X, NK_WEST_Z)) return 0;
    return 1;
}

/* The floating sign. Same shape as every other sign in the game: opaque within
   NK_FADE_NEAR, gone by NK_TEXT_RADIUS. */
static void nk_west_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - NK_WEST_X;
    int32_t dz = cam_z - NK_WEST_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= NK_TEXT_RADIUS) return;
    if (xz > NK_FADE_NEAR) {
        int range = NK_TEXT_RADIUS - NK_FADE_NEAR;
        int prog  = xz - NK_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        NK_WEST_X + 11, NK_DOOR_TEXT_Y, NK_WEST_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void neck_spawn_west(void) {
    /* 220 off wall 1 on its walkable +X side, on the door's centre line,
       facing +X down the hall. (220,900) is 380 clear of the west pillar. */
    cam_x   = NK_WEST_X + (NK_WALL_RADIUS + 25);
    cam_y   = NK_EYE_Y;
    cam_vy  = 0;
    cam_z   = NK_WEST_Z;
    cam_rot = 1024;                    /* facing +X, east down the hall */
    neck_arm();
}

void neck_init(void) {
    room_data_collision(&current_collision_room);
    /* THE WALLS, read off the VISUAL mesh: the stone rises to y=-800, where
       the proxy's walls and pillars stop too. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(NK_WALL_RADIUS);

    nk_floor_zones_init();
    cam_pitch = 0;

    neck_spawn_west();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every collide routine, so an
       instance left from another room would block invisibly anywhere it falls
       inside x[0,3000] z[0,1800]. Cleared as the Throat clears them; safe,
       because catacombs_entry_init() re-places all four. THEN this room's own
       save point (see NK_SAVE_X above). */
    save_points_clear();
    save_point_add(NK_SAVE_X, NK_SAVE_Y, NK_SAVE_Z, 512, 2048);
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    nk_view_resolve(1);
}

static void draw_neck_smd(RenderContext *ctx) {
    if (!nk_smd) return;

    uint8_t *p = (uint8_t *)nk_smd->p_prims;
    int i, n = nk_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = nk_fog_far;   /* resolved by nk_view_resolve() this frame */
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
        SVECTOR *v0 = &nk_smd->p_verts[vi[0]];
        SVECTOR *v1 = &nk_smd->p_verts[vi[1]];
        SVECTOR *v2 = &nk_smd->p_verts[vi[2]];

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

        int nocull = (i < NECK_PRIM_COUNT) && room_nocull(i);
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
            v3 = &nk_smd->p_verts[vi[3]];
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
        int32_t fog = dist < nk_fog_near ? nk_fog_near : (dist > nk_fog_far ? nk_fog_far : dist);
        int32_t fog_factor = ((nk_fog_far - fog) << 8) / (nk_fog_far - nk_fog_near);

        uint8_t tex_idx = (i < NECK_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < NECK_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + NK_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + NK_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + NK_FOG_B * (256 - fog_factor)) >> 8);

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

void neck_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    nk_view_resolve(0);

    g_fog_near = nk_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : nk_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, NK_FOG_R, NK_FOG_G, NK_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       Both textures sit at Voff 0. */
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

    if (exp != DBG_EXP_NO_MESH) draw_neck_smd(ctx);

    /* >>> LEVEL 8 IS THE SAVE POINT AND THE SIGN. <<< Nothing else stands in
       the Neck. The two Chapter 3 enemy draws are area-tagged and cost nothing
       here; they are called on the Tomb's argument, so a room that is later
       given an occupant in world.c draws it without an edit. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        save_points_draw(ctx);
        nk_west_door_text(ctx);    /* west: YZ plane, approached from +X */
        /* THE CHAPTER 3 ENEMIES. Their sheets sit at Voff 128, so each is
           handed the window to restore after drawing unmasked. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
            lumberers_set_texwindow(&tw);
        }
        draw_crawlers(ctx);
        draw_lumberers(ctx);
    }
}
