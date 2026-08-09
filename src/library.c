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
#include "tim_slots.h"
#include "camera.h"
#include "library.h"
#include "collision.h"
#include "library_mesh_collision.h"
#include "library_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "concrete_props.h"
#include "piano_room.h"
#include "piano_props.h"
#include "save_point.h"
#include "zombie.h"
#include "spider.h"
#include "rabisu.h"
#include "web.h"
#include "item_pickup.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Library: the reading room at the east end of the East Hall, rendered the same
   way as the East Hall / master bedroom (per-poly tex map + 128 texture window
   + fog). The double door in the entrance vestibule's west wall leads back to
   the hall. */

static SMD  *library_smd  = NULL;
static void *library_buff = NULL;

/* Single flat floor at y=0 (both floors detected in library_mesh_collision.c
   sit at y=0). The rect over-covers the L-shape's empty north-west quarter
   (x<-350, z>-349), which is harmless: wall 0 at z=-349 keeps the player out of
   it. */
static void library_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -1780; floor_zones[0].max_x = 350;
    floor_zones[0].min_z = -2080; floor_zones[0].max_z = 349;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   Seven mesh textures, ALL already in the game — this room adds no new art and
   therefore claims no new VRAM. Four are resident from startup (wd_flr, din_cl
   and inr_dbl_dr with the kitchen/reception, wd_dr with the fat door). Three
   live in time-shared slots owned by other modules, so we call their uploads on
   entry rather than duplicating the RAM copies:
     - cncrte     occupies the kchn_tile slot (x384 y0), the same slot the
                  conservatory and East Hall stream it into via
                  concrete_props_upload_textures();
     - prpl_wlppr occupies the stove slot (x384 y256), streamed by the piano
                  room;
     - bookshelf  occupies the stn_stl slot (x320 y0), streamed by the piano
                  room's bookcase prop.
   NOTE the deliberate use of the narrow, single-texture upload entry points:
   piano_room_upload_textures() would also drag in piano_keys, which shares the
   kchn_tile slot with the cncrte this room needs.

   All three slots are already on kitchen_restore_textures()' list (and
   reception/conservatory/hall_2f each re-upload their own strs over stn_stl on
   entry), so stomping them here creates no NEW restore obligation. All seven
   textures sit at Voff 0, so the one 128 texture window set in library_draw
   serves them all. */
#define LIBRARY_TEX_COUNT 7

static uint16_t tex_tpage[LIBRARY_TEX_COUNT];
static uint16_t tex_clut[LIBRARY_TEX_COUNT];

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale. */
void library_load_geometry(void) {
    library_buff = room_arena_load("\\TEX\\LIBRARY.SMD;1");
    library_smd  = library_buff ? smdInitData(library_buff) : NULL;
}

/* Nothing to read at STARTUP: geometry moved to library_load_geometry above,
   and every texture this room draws is registered/uploaded by another module, so
   the slots below are compile-time constants. */
void library_load_assets(void) {
    /* Uploaded by concrete_props (cncrte), the piano room (prpl_wlppr) and the
       bookcase prop (bookshelf), all of which library_upload_textures calls. */
    TIM_SLOT(0, CNCRTE);
    TIM_SLOT(1, PRPLWLP);
    TIM_SLOT(4, BOOKSHLF);

    /* Resident from startup (kitchen/reception + fatdoor). */
    TIM_SLOT(2, WDFLR);
    TIM_SLOT(3, DINCL);
    TIM_SLOT(5, INRDBLDR);
    TIM_SLOT(6, WDDR);
}

/* Upload the three streamed textures from their owners' resident RAM copies.
   Pure LoadImage — no CD access — safe during the room transition (the caller
   DrawSyncs first, as main's STATE_LOADING does). */
void library_upload_textures(void) {
    concrete_props_upload_textures();       /* cncrte     -> kchn_tile slot */
    piano_room_upload_wallpaper();          /* prpl_wlppr -> stove slot     */
    piano_props_upload_bookcase_texture();  /* bookshelf  -> stn_stl slot   */
}

/* ---- The west double door back to the East Hall ----------------------------
   In the entrance vestibule's west wall (x=-350, from the inr_dbl_dr polys in
   "Library.smx"), centred on z=0. It maps to the double door at the EAST end of
   the East Hall (east hall x=2672, z=372). The player approaches from the +X
   (vestibule) side, so the sign lies in the YZ plane with mirror=0 — the same
   orientation as the East Hall's own west-wall sign. The interaction point sits
   65 units east of the wall so the sign floats in front of the door. */
#define LDOOR_W_X                (-285)
#define LDOOR_W_Z                    0
#define LDOOR_TEXT_Y            (-186)
#define LDOOR_TEXT_RADIUS         1500
#define LDOOR_FADE_NEAR           1000
#define LDOOR_TRIGGER_RADIUS       500

/* ---- The south single door onto the East Stairwell -------------------------
   In the reading room's south wall (z=-2080, from the wd_dr polys in
   "Library.smx"), centred on x=-1400. It maps to the East Stairwell's EAST
   landing door (stairwell x=503, z=349). The player approaches from +Z, so the
   sign lies in the XY plane with mirror=1. Interaction point 65 units north of
   the wall so the sign floats in front of the door. */
#define LSDOOR_X               (-1400)
#define LSDOOR_Z               (-2015)

/* Circle edge-detect, one per door, seeded by library_doors_arm(). Both start
   "held" so a press carried in from a transition doesn't bounce the player
   straight back out. */
static int wdoor_circle_prev = 1;
static int sdoor_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void library_doors_arm(void) {
    int held = circle_held();
    wdoor_circle_prev = held;
    sdoor_circle_prev = held;
}

int library_wdoor_triggered(void) {
    int held = circle_held();
    int just = held && !wdoor_circle_prev;
    wdoor_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - LDOOR_W_X;
    int32_t dz = cam_z - LDOOR_W_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < LDOOR_TRIGGER_RADIUS && interact_facing(LDOOR_W_X, LDOOR_W_Z);
}

int library_sdoor_triggered(void) {
    int held = circle_held();
    int just = held && !sdoor_circle_prev;
    sdoor_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - LSDOOR_X;
    int32_t dz = cam_z - LSDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < LDOOR_TRIGGER_RADIUS && interact_facing(LSDOOR_X, LSDOOR_Z);
}

/* Floating "Press O to enter" sign on the west door. YZ plane:
   door_draw_string_3d centres the reading axis (Z) on world_z after adding 200,
   so pass door_z - 200. mirror=0 because the player reads it from +X. */
static void ldoor_text(RenderContext *ctx) {
    int32_t dx = cam_x - LDOOR_W_X;
    int32_t dz = cam_z - LDOOR_W_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= LDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > LDOOR_FADE_NEAR) {
        int range = LDOOR_TEXT_RADIUS - LDOOR_FADE_NEAR;
        int prog  = xz - LDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        LDOOR_W_X, LDOOR_TEXT_Y, LDOOR_W_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* Floating sign on the south door. XY plane (fixed Z): door_draw_string_3d
   centres the reading axis (X) on world_x after adding 200, so pass door_x-200.
   mirror=1 because the player reads it from +Z. */
static void lsdoor_text(RenderContext *ctx) {
    int32_t dx = cam_x - LSDOOR_X;
    int32_t dz = cam_z - LSDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= LDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > LDOOR_FADE_NEAR) {
        int range = LDOOR_TEXT_RADIUS - LDOOR_FADE_NEAR;
        int prog  = xz - LDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        LSDOOR_X - 200, LDOOR_TEXT_Y, LSDOOR_Z,
                        50, 255, 50, fade, 1, TEXT_PLANE_XY, DOOR_PIXEL_SIZE);
}

/* Spawn just inside the west door, far enough east of the x=-350 wall to clear
   the 195 push radius apply_collision_reception uses, facing +X (east) along
   the direction of travel through the door. */
void library_spawn_west(void) {
    cam_x   = -80;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = LDOOR_W_Z;
    cam_rot = 1024;   /* facing +X, into the vestibule */
    library_doors_arm();
}

/* Spawn just inside the south door (z=-2080 wall), 230 north of it to clear the
   195 push radius, facing +Z back into the reading room. */
void library_spawn_south(void) {
    cam_x   = LSDOOR_X;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = -1850;
    cam_rot = 0;      /* facing +Z, into the reading room */
    library_doors_arm();
}

void library_init(void) {
    library_collision_init(&current_collision_room);
    /* This room's proxy mesh (Library mesh.smx) tops its walls out at y=-500,
       but the mesh actually DRAWN (Library.smx -> library.smd) has the reading
       room's ceiling at y=-730 (the entrance vestibule's is the lower -500).
       State the drawn value of the room proper so ceiling-mounted enemies hang
       flush with the roof the player can see. */
    collision_set_ceiling_y(-730);
    library_floor_zones_init();

    /* Default arrival spawn; main.c's STATE_LOADING overrides it with
       library_spawn_south() when the player came back from the East Stairwell. */
    library_spawn_west();

    /* Reception's save point and dresser prop are global (not room-swapped) and
       neither is area-gated in its collide routine, so reception's instances
       would block the player invisibly inside this room's bounds — the save
       point sits at (78,-67), inside the entrance vestibule. Clearing them is
       safe: reception_init() re-places both on every reception entry. */
    save_points_clear();
    dressers_clear();
}

static void draw_library_smd(RenderContext *ctx) {
    if (!library_smd) return;

    uint8_t *p = (uint8_t *)library_smd->p_prims;
    int i;

    for (i = 0; i < library_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &library_smd->p_verts[vi[0]];
        SVECTOR *v1 = &library_smd->p_verts[vi[1]];
        SVECTOR *v2 = &library_smd->p_verts[vi[2]];

        {
            int32_t dx = (int32_t)v0->vx - cam_x;
            int32_t dz = (int32_t)v0->vz - cam_z;
            /* Distance cull (Manhattan) at the fog-out distance so culled polys
               are already invisible (same budget as the other rooms). */
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 1500)
                { p += stride; continue; }
            int32_t fwd = dx * isin(cam_rot) + dz * icos(cam_rot);
            if (fwd < -(700 << 12))
                { p += stride; continue; }
        }

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

        /* Backface cull, except degenerate (triangle-shaped) quads flagged at
           build time in library_nocull — same scheme as the other rooms. */
        int nocull = (i < LIBRARY_PRIM_COUNT) && library_nocull[i];
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
            v3 = &library_smd->p_verts[vi[3]];
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
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog_start = 350, fog_end = 1500;   /* fog saturates at the cull distance */
        int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
        int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in library_draw. */
        uint8_t tex_idx = (i < LIBRARY_PRIM_COUNT) ? library_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < LIBRARY_TEX_COUNT);
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8);

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

void library_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = 350; g_fog_far = 1500;

    /* Dark interior background, same as the other rooms. */
    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setXY0(bg, 0, 0);
    setWH(bg, SCREEN_XRES, SCREEN_YRES);
    setRGB0(bg, 20, 15, 10);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
    ctx->next_packet += sizeof(TILE);

    /* 128x128 texture window so per-poly UVs wrap (tile) within each texture's
       page. All seven Library textures sit at page-top (Voff 0), so one window
       serves them (see tools/VRAM_MAP.txt). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    /* View matrix from the camera (same construction as the other rooms). */
    MATRIX rot_matrix;
    camera_build_view(&rot_matrix);

    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    draw_library_smd(ctx);

    /* Three spiders hang from the reading room's ceiling and one zombie walks
       its northern strip (both placed in world.c). Each renderer is handed the
       room's 128 texture window so its Voff>=128 sprites are bracketed
       correctly (see tools/TEXTURING_NOTES.txt PART 5). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
        spiders_set_texwindow(&tw);
    }
    draw_zombies(ctx);
    draw_spiders(ctx);
    draw_rabisus(ctx);
    webs_draw(ctx);
    item_pickups_draw(ctx);

    ldoor_text(ctx);
    lsdoor_text(ctx);
}
