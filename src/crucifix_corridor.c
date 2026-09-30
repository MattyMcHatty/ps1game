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
#include "crucifix_corridor.h"
#include "lumberer.h"
#include "crawler.h"
#include "collision.h"
#include "crucifix_corridor_mesh_collision.h"
#include "crucifix_corridor_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* The Crucifix Corridor — see crucifix_corridor.h for the layout, the doors and
   the sconce. */

static SMD  *xc_smd  = NULL;
static void *xc_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   450/1600 with +50%/+50% for the lantern — the chapter's usual numbers, and
   the Cleaver Corridor's. 288 primitives is a small mesh. From the west door
   the alcove is 3300 away, well past 1600: what reaches the player down the
   corridor is the SCONCE's light, which widens the reach locally around it
   (render.h "Point lights"), so the alcove shows at the end of a dark corridor.
   That is why this room's draw loop is the Catacombs Entry's light-aware one. */
#define XC_BASE_FOG_NEAR   450
#define XC_BASE_FOG_FAR   1600

#define XC_VIEW_UNIT        256
#define XC_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define XC_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */
#define XC_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t xc_view     = XC_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t xc_fog_near = XC_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t xc_fog_far  = XC_BASE_FOG_FAR;

static int32_t xc_view_target(void) {
    int32_t s = XC_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += XC_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += XC_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void xc_view_resolve(int snap) {
    int32_t target = xc_view_target();
    if (snap) {
        xc_view = target;
    } else if (xc_view < target) {
        xc_view += XC_VIEW_RATE;
        if (xc_view > target) xc_view = target;
    } else if (xc_view > target) {
        xc_view -= XC_VIEW_RATE;
        if (xc_view < target) xc_view = target;
    }
    xc_fog_near = (XC_BASE_FOG_NEAR * xc_view) >> 8;
    xc_fog_far  = (XC_BASE_FOG_FAR  * xc_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). */
#define XC_FOG_R             7
#define XC_FOG_G             6
#define XC_FOG_B             9

/* Wall standoff. The chapter's 195. The corridor and the arm are both 600
   wide, which leaves a 210 band down the middle of each. */
#define XC_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define XC_FLOOR_Y            0
#define XC_EYE_Y           (XC_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   THREE, FLAT, AT y=0, over the proxy's three floor faces exactly: the corridor
   (which includes the east alcove) and the arm's two halves either side of it.
   One plane, so their order does not matter. */
static void xc_floor_zones_init(void) {
    static const int16_t zones[3][4] = {   /* min_x, max_x, min_z, max_z */
        {    0, 3600,  -300,   300 },     /* the corridor and the east alcove */
        { 2400, 3000,   300,  1500 },     /* the arm, north half              */
        { 2400, 3000, -1500,  -300 },     /* the arm, south half              */
    };
    int i;
    for (i = 0; i < 3; i++) {
        floor_zones[i].type  = FLOOR_FLAT;
        floor_zones[i].min_x = zones[i][0]; floor_zones[i].max_x = zones[i][1];
        floor_zones[i].min_z = zones[i][2]; floor_zones[i].max_z = zones[i][3];
        floor_zones[i].y     = XC_FLOOR_Y;
    }
    floor_zone_count = 3;
}

/* ---- Textures --------------------------------------------------------------
   TWO, AND THE ROOM OWNS NEITHER.

     0 cobblestones        the corridor, the arm and the vault — 282 of the
                           288 polys                      (x384 y0)
     1 catacomb inner door the three doorways             (x832 y0)

   Both come through src/catacombs_entry.c's narrow uploaders, as every Chapter
   3 room takes them. The sconce's page is put up beside them by its own
   module's uploader. All sit at Voff 0, so the one 128 texture window in the
   draw serves them. */
#define CRUCIFIX_CORRIDOR_TEX_COUNT 2

static uint16_t tex_tpage[CRUCIFIX_CORRIDOR_TEX_COUNT];
static uint16_t tex_clut[CRUCIFIX_CORRIDOR_TEX_COUNT];

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/cleaver_corridor.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int xc_key_count = 0;

static void xc_build_cull_keys(void) {
    xc_key_count = 0;
    if (!xc_smd) return;
    uint8_t *p = (uint8_t *)xc_smd->p_prims;
    int i, n = xc_smd->n_prims;
    if (n > CRUCIFIX_CORRIDOR_PRIM_COUNT) n = CRUCIFIX_CORRIDOR_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &xc_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    xc_key_count = n;
}

void crucifix_corridor_load_geometry(void) {
    xc_buff = room_arena_load("\\TEXCTCMB\\CRCFXCRD.SMD;1");
    xc_smd  = xc_buff ? smdInitData(xc_buff) : NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    xc_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: two compile-time headers and no
   registration at all, so no texmgr_set_bank() either. The bank this room's art
   is in is decided by its owners'. */
void crucifix_corridor_load_assets(void) {
    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, CTCMBDR);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first). */
void crucifix_corridor_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    /* ...and the SCONCE's own page (x448 y0), for the lit one in the alcove. */
    sconce_upload_texture();
}

/* ---- THE SCONCE ------------------------------------------------------------
   LIT, in the centre of the east alcove x[3000,3600] z[-300,300]. The alcove is
   a dead end, and the stand's box fills its 210-wide walkable band, so the
   player walks up to it rather than round it. */
#define XC_SCONCE_X          3300
#define XC_SCONCE_Z             0

/* ---- THE WEST DOOR ---------------------------------------------------------
   x=0, z[-100,100]. Out to THE UP DOWN MAZE, onto its lower storey at the
   north-east door.

   In the YZ plane at fixed X, approached from +X (wall 4 runs x=0 with
   nx = +4096, so the walkable side is +X): TEXT_PLANE_YZ with mirror=0, the
   sign 11 proud of the wall along +X, and the -200 door_draw_string_3d wants on
   the Z argument. The Up Down Maze's west door's pair. */
#define XC_WEST_X               0
#define XC_WEST_Z               0     /* the art spans z[-100,100] */
#define XC_WEST_TEXT_Y       (-186)   /* eye level on the y=0 floor */
#define XC_TEXT_RADIUS       1200
#define XC_FADE_NEAR          800
#define XC_TRIGGER_RADIUS     500

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int west_circle_prev = 1;

void crucifix_corridor_arm(void) {
    west_circle_prev = interact_tapped();
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int crucifix_corridor_west_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !west_circle_prev;
    int32_t dx, dz, xz;
    west_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - XC_WEST_X;
    dz = cam_z - XC_WEST_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= XC_TRIGGER_RADIUS) return 0;
    if (!interact_facing(XC_WEST_X, XC_WEST_Z)) return 0;
    return 1;
}

/* The door's floating sign. Same shape as every door sign in the game: opaque
   within XC_FADE_NEAR, gone by XC_TEXT_RADIUS. */
static void xc_west_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - XC_WEST_X;
    int32_t dz = cam_z - XC_WEST_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= XC_TEXT_RADIUS) return;

    if (xz > XC_FADE_NEAR) {
        int range = XC_TEXT_RADIUS - XC_FADE_NEAR;
        int prog  = xz - XC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        XC_WEST_X + 11, XC_WEST_TEXT_Y, XC_WEST_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void crucifix_corridor_spawn_west(void) {
    /* In from the Up Down Maze. 220 off wall 4 on its walkable +X side, on the
       corridor's centre line, facing +X — the direction of travel, east down
       the corridor toward the lit alcove. */
    cam_x   = XC_WEST_X + (XC_WALL_RADIUS + 25);
    cam_y   = XC_EYE_Y;
    cam_vy  = 0;
    cam_z   = XC_WEST_Z;
    cam_rot = 1024;                    /* facing +X, east down the corridor */
    crucifix_corridor_arm();
}

void crucifix_corridor_init(void) {
    crucifix_corridor_collision_init(&current_collision_room);
    /* The vault, read off the VISUAL mesh: y=-800 over the whole cross, where
       the proxy's walls stop too. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(XC_WALL_RADIUS);

    xc_floor_zones_init();
    cam_pitch = 0;

    crucifix_corridor_spawn_west();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every collide routine, so an
       instance left from another room would block invisibly anywhere it falls
       inside the cross. Safe to clear: every room that has one re-places its
       own on entry. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    /* ...then this room's own sconce, LIT, in the centre of the east alcove. */
    sconce_place(STATE_CRUCIFIX_CORRIDOR, XC_SCONCE_X, -GROUND_FLOOR_Y,
                 XC_SCONCE_Z, 0, 1);

    xc_view_resolve(1);
}

static void draw_crucifix_corridor_smd(RenderContext *ctx) {
    if (!xc_smd) return;

    uint8_t *p = (uint8_t *)xc_smd->p_prims;
    int i, n = xc_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = xc_fog_far;   /* resolved by xc_view_resolve() this frame */
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < n; i++) {
        /* THE REJECT PATH READS cull_keys, NOT THE MESH. */
        uint8_t stride = cull_keys[i].stride;
        {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            int32_t cd = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            /* SHORT-CIRCUITED ON PURPOSE, the Catacombs Entry's arrangement: a
               primitive inside the camera's own reach never asks the light, and
               only one the camera would drop pays for the test (which
               box-rejects before it loops — render.h). */
            if (cd > cull &&
                render_light_dist((int32_t)cull_keys[i].x,
                                  (int32_t)cull_keys[i].z, cd) > cull)
                { p += stride; continue; }
            if (dx * sn + dz * cs < -(700 << 12))
                { p += stride; continue; }
        }

        /* SURVIVED BOTH CULLS: only now is the header read and the vertex array
           addressed. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &xc_smd->p_verts[vi[0]];
        SVECTOR *v1 = &xc_smd->p_verts[vi[1]];
        SVECTOR *v2 = &xc_smd->p_verts[vi[2]];

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

        int nocull = (i < CRUCIFIX_CORRIDOR_PRIM_COUNT) && crucifix_corridor_nocull[i];
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
            v3 = &xc_smd->p_verts[vi[3]];
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
        /* The sconce's discount, and it MUST match the one the cull above
           applied or a lit poly survives the cull and is then shaded as though
           it had not been — drawn in the clear colour, a hole. */
        dist = render_light_dist(face_cx, face_cz, dist);
        int32_t fog = dist < xc_fog_near ? xc_fog_near : (dist > xc_fog_far ? xc_fog_far : dist);
        int32_t fog_factor = ((xc_fog_far - fog) << 8) / (xc_fog_far - xc_fog_near);

        uint8_t tex_idx = (i < CRUCIFIX_CORRIDOR_PRIM_COUNT) ? crucifix_corridor_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < CRUCIFIX_CORRIDOR_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + XC_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + XC_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + XC_FOG_B * (256 - fog_factor)) >> 8);

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

void crucifix_corridor_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    xc_view_resolve(0);

    g_fog_near = xc_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : xc_fog_far;

    /* THE SCONCE'S LIGHT, after the two numbers it is a discount on and before
       the mesh that reads it. The list was cleared in draw_current_area(), so
       this only ever adds. */
    sconces_publish_lights();

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, XC_FOG_R, XC_FOG_G, XC_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       Both textures and the sconce's sit at Voff 0. */
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

    if (exp != DBG_EXP_NO_MESH) draw_crucifix_corridor_smd(ctx);

    /* The lit sconce, stand and flame. Its texture sits at Voff 0, so the
       window above serves it. */
    sconces_draw(ctx);

    /* >>> LEVEL 8 REMOVES THE SIGN AND THE ENEMIES. <<< The room holds no
       enemies today, so what it takes away is the one door sign — STEP 3D's
       case. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        xc_west_door_text(ctx);   /* the west door: YZ plane, approached from +X */
        /* BOTH CHAPTER 3 ENEMIES, drawn in every room of the chapter whether or
           not world.c places one here: the area tag makes an absent enemy free.
           Both sheets sit at Voff 128, so each is handed the window to restore
           after drawing unmasked. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
            lumberers_set_texwindow(&tw);
        }
        draw_crawlers(ctx);
        draw_lumberers(ctx);
    }
}
