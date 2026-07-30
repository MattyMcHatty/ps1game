#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "east_hall.h"
#include "collision.h"
#include "east_hall_mesh_collision.h"
#include "east_hall_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "concrete_props.h"
#include "save_point.h"
#include "zombie.h"
#include "item_pickup.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* East Hall: a long east-west corridor off reception's upper floor, rendered
   the same way as the master bedroom / 2F hall (per-poly tex map + 128 texture
   window + fog). The double door in its west wall leads back to reception. */

static SMD  *east_hall_smd  = NULL;
static void *east_hall_buff = NULL;

/* Single flat floor at y=0 across the walkable bounds (all fourteen floors
   detected in east_hall_mesh_collision.c sit at y=0). The rect over-covers the
   L-shape's empty south-east corner, which is harmless: the walls keep the
   player out of it. */
static void east_hall_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = 19;   floor_zones[0].max_x = 2671;
    floor_zones[0].min_z = -992; floor_zones[0].max_z = 721;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   Five mesh textures, ALL already in the game — this room adds no new art and
   therefore claims no new VRAM. Three are resident from startup (wd_flr and
   inr_dbl_dr with the kitchen/reception, wd_dr with the fat door). Two live in
   time-shared slots owned by other modules, so we just call their uploads on
   entry rather than duplicating the RAM copies:
     - cncrte occupies the kchn_tile slot (x384 y0), the same slot the
       conservatory streams it into via concrete_props_upload_textures();
     - dresser occupies the kchn_wl slot (x512 y0), uploaded by the dresser
       prop module for the dressers modelled into this room's mesh.
   Both slots are already on kitchen_restore_textures()' list, so stomping them
   here creates no NEW restore obligation. All five sit at Voff 0, so the one
   128 texture window set in east_hall_draw serves them all. */
#define EAST_HALL_TEX_COUNT 5

static uint16_t tex_tpage[EAST_HALL_TEX_COUNT];
static uint16_t tex_clut[EAST_HALL_TEX_COUNT];

/* Read a whole TIM into a freshly malloc'd buffer (caller owns it). NULL on fail. */
static uint8_t *read_tim(const char *filename) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)filename)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    uint8_t *buf = malloc(sectors * 2048);
    if (!buf) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);
    return buf;
}

static void *load_file_from_cd(const char *filename) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)filename)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    void *buff = malloc(sectors * 2048);
    if (!buff) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buff, CdlModeSpeed);
    CdReadSync(0, NULL);
    return buff;
}

/* Capture tpage/clut for a texture that is ALREADY resident in VRAM (shared
   with another room), or whose upload another module owns: read its header
   only, no LoadImage. */
static void capture_tpage(const char *filename, int slot) {
    uint8_t *buf = read_tim(filename);
    if (!buf) return;
    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    if (tim.mode & 0x8) tex_clut[slot] = getClut(tim.crect->x, tim.crect->y);
    tex_tpage[slot] = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);
    free(buf);
}

/* Load geometry AND capture texture headers at STARTUP (the only time CD access
   is safe — see tools/TEXTURING_NOTES.txt). Every texture this room draws is
   registered by another module, so there is nothing to texmgr_register here. */
void east_hall_load_assets(void) {
    east_hall_buff = load_file_from_cd("\\TEX\\EASTHALL.SMD;1");
    if (east_hall_buff)
        east_hall_smd = smdInitData(east_hall_buff);

    /* Uploaded by concrete_props (cncrte) and the dresser prop (dresser), both
       of which east_hall_upload_textures calls on entry. */
    capture_tpage("\\TEX\\CNCRTE.TIM;1",  0);
    capture_tpage("\\TEX\\DRESSER.TIM;1", 3);

    /* Resident from startup (kitchen/reception + fatdoor). */
    capture_tpage("\\WDFLR.TIM;1",    1);
    capture_tpage("\\INRDBLDR.TIM;1", 2);
    capture_tpage("\\WDDR.TIM;1",     4);
}

/* Upload the two streamed textures from their owners' resident RAM copies.
   Pure LoadImage — no CD access — safe during the room transition (the caller
   DrawSyncs first, as main's STATE_LOADING does). */
void east_hall_upload_textures(void) {
    concrete_props_upload_textures();   /* cncrte  -> kchn_tile slot */
    dresser_upload_texture();           /* dresser -> kchn_wl slot   */
}

/* ---- The west double door back to reception --------------------------------
   In the room's west wall (x=20, from the inr_dbl_dr polys in "East Hall.smx"),
   centred on z=372. It maps to the double door on the EAST wall of reception's
   upper floor (reception x=1500, z=1071). The player approaches from the +X
   (room) side, so the sign lies in the YZ plane with mirror=0 — the same
   orientation as reception's own west-wall signs. The interaction point sits 65
   units east of the wall so the sign floats in front of the door. */
#define EHDOOR_W_X                 85
#define EHDOOR_W_Z                372
#define EHDOOR_TEXT_Y           (-186)
#define EHDOOR_TEXT_RADIUS       1500
#define EHDOOR_FADE_NEAR         1000
#define EHDOOR_TRIGGER_RADIUS     500

/* Circle edge-detect, seeded by east_hall_doors_arm(). Starts "held" so a press
   carried in from the reception-side transition doesn't bounce the player
   straight back out. */
static int wdoor_circle_prev = 1;

static int circle_held(void) {
    if (!pad_buff_len[0]) return 0;
    PadResponse *pad = (PadResponse *)pad_buff[0];
    return (~pad->btn & PAD_CIRCLE) ? 1 : 0;
}

void east_hall_doors_arm(void) {
    wdoor_circle_prev = circle_held();
}

int east_hall_wdoor_triggered(void) {
    int held = circle_held();
    int just = held && !wdoor_circle_prev;
    wdoor_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - EHDOOR_W_X;
    int32_t dz = cam_z - EHDOOR_W_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < EHDOOR_TRIGGER_RADIUS;
}

/* Floating "Press O to enter" sign on the west door. YZ plane:
   door_draw_string_3d centres the reading axis (Z) on world_z after adding 200,
   so pass door_z - 200. mirror=0 because the player reads it from +X. */
static void ehdoor_text(RenderContext *ctx) {
    int32_t dx = cam_x - EHDOOR_W_X;
    int32_t dz = cam_z - EHDOOR_W_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= EHDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > EHDOOR_FADE_NEAR) {
        int range = EHDOOR_TEXT_RADIUS - EHDOOR_FADE_NEAR;
        int prog  = xz - EHDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        EHDOOR_W_X, EHDOOR_TEXT_Y, EHDOOR_W_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* Spawn just inside the west door, far enough east of the x=20 wall to clear
   the 195 push radius apply_collision_reception uses, facing +X into the hall. */
void east_hall_spawn_west(void) {
    cam_x   = 260;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = EHDOOR_W_Z;
    cam_rot = 1024;   /* facing +X, into the hall */
    east_hall_doors_arm();
}

void east_hall_init(void) {
    east_hall_collision_init(&current_collision_room);
    east_hall_floor_zones_init();

    /* Only one wired door so far, so this is also the arrival spawn. */
    east_hall_spawn_west();

    /* Reception's save point and dresser prop are global (not room-swapped) and
       neither is area-gated in its collide routine, so reception's instances
       would block the player invisibly inside this room's bounds — the save
       point sits at (78,-67), just inside the hall's west end. Clearing them is
       safe: reception_init() re-places both on every reception entry. */
    save_points_clear();
    dressers_clear();
}

static void draw_east_hall_smd(RenderContext *ctx) {
    if (!east_hall_smd) return;

    uint8_t *p = (uint8_t *)east_hall_smd->p_prims;
    int i;

    for (i = 0; i < east_hall_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &east_hall_smd->p_verts[vi[0]];
        SVECTOR *v1 = &east_hall_smd->p_verts[vi[1]];
        SVECTOR *v2 = &east_hall_smd->p_verts[vi[2]];

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
           build time in east_hall_nocull — same scheme as the other rooms. */
        int nocull = (i < EAST_HALL_PRIM_COUNT) && east_hall_nocull[i];
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
            v3 = &east_hall_smd->p_verts[vi[3]];
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
           texture window set in east_hall_draw. */
        uint8_t tex_idx = (i < EAST_HALL_PRIM_COUNT) ? east_hall_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < EAST_HALL_TEX_COUNT);
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

void east_hall_draw(RenderContext *ctx) {
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
       page. All five East Hall textures sit at page-top (Voff 0), so one window
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
    SVECTOR neg_rot = {0, -cam_rot, 0, 0};
    RotMatrix(&neg_rot, &rot_matrix);

    VECTOR trans;
    trans.vx = -cam_x;
    trans.vy = -cam_y;
    trans.vz = -cam_z;
    ApplyMatrixLV(&rot_matrix, &trans, &trans);
    rot_matrix.t[0] = trans.vx;
    rot_matrix.t[1] = trans.vy;
    rot_matrix.t[2] = trans.vz;

    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    draw_east_hall_smd(ctx);

    /* No enemies placed here yet; the room's 128 texture window is still handed
       to the zombie renderer so a future spawn brackets its Voff>=128 sprite
       correctly (see tools/TEXTURING_NOTES.txt PART 5). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
    }
    draw_zombies(ctx);
    item_pickups_draw(ctx);

    ehdoor_text(ctx);
}
