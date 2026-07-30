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
#include "hall_2f.h"
#include "collision.h"
#include "hall_2f_mesh_collision.h"
#include "hall_2f_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "trick_drawers.h"
#include "fatdoor.h"
#include "zombie.h"
#include "spider.h"
#include "sound.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Second-floor hall: an L-shaped corridor off the top of the conservatory
   stairs, rendered the same way as the conservatory (per-poly tex map + 128
   texture window + fog). */

static SMD  *hall_2f_smd  = NULL;
static void *hall_2f_buff = NULL;

/* Single flat floor at y=0 across the walkable bounds (all four floors detected
   in hall_2f_mesh_collision.c sit at y=0 — the descending stairwell is
   scenery). */
static void hall_2f_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -3797; floor_zones[0].max_x = 101;
    floor_zones[0].min_z = -1398; floor_zones[0].max_z = 500;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   Seven mesh textures. Three are resident from startup (wd_flr/din_cl with the
   kitchen, wd_dr with the fat door). FOUR are streamed on every hall entry:
     - prpl_wlppr time-shares the stove slot (also streamed by the piano room
       and conservatory);
     - strs time-shares the stn_stl slot (also streamed by reception/conservatory);
     - upstairs time-shares the rusty_fence slot (also streamed by the conservatory);
     - red_crpt time-shares the frnt_dr slot (also streamed by the kitchen).
   Every one of these four slots is ALREADY stomped by the conservatory or the
   kitchen, so every room that needs the underlying texture back restores it on
   its own entry — the hall adds no new restore obligation. */
#define HALL_2F_TEX_COUNT 7

/* Streamed slots: engine slot -> texmgr id. */
#define HALL_2F_NEW_TEX 4
static int new_tex_id[HALL_2F_NEW_TEX];
static const struct { const char *file; int slot; } new_tex[HALL_2F_NEW_TEX] = {
    { "\\TEX\\PRPLWLP.TIM;1",  1 },
    { "\\TEX\\STRS.TIM;1",     4 },
    { "\\TEX\\UPSTAIRS.TIM;1", 5 },
    { "\\TEX\\REDCRPT.TIM;1",  6 },
};

static uint16_t tex_tpage[HALL_2F_TEX_COUNT];
static uint16_t tex_clut[HALL_2F_TEX_COUNT];

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
   with another room): read its header only, no LoadImage. */
static void capture_tpage(const char *filename, int slot) {
    uint8_t *buf = read_tim(filename);
    if (!buf) return;
    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    if (tim.mode & 0x8) tex_clut[slot] = getClut(tim.crect->x, tim.crect->y);
    tex_tpage[slot] = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);
    free(buf);
}

/* Load geometry AND register/capture textures at STARTUP (the only time CD
   access is safe — see tools/TEXTURING_NOTES.txt). */
void hall_2f_load_assets(void) {
    hall_2f_buff = load_file_from_cd("\\TEX\\HALL2F.SMD;1");
    if (hall_2f_buff)
        hall_2f_smd = smdInitData(hall_2f_buff);

    /* Streamed slots: RAM-resident via the texture manager, uploaded on entry. */
    for (int i = 0; i < HALL_2F_NEW_TEX; i++) {
        int slot = new_tex[i].slot;
        new_tex_id[i]   = texmgr_register(new_tex[i].file);
        tex_tpage[slot] = texmgr_tpage(new_tex_id[i]);
        tex_clut[slot]  = texmgr_clut(new_tex_id[i]);
    }

    /* Resident from startup (kitchen + fatdoor); just capture tpage/clut. */
    capture_tpage("\\WDFLR.TIM;1", 0);
    capture_tpage("\\DINCL.TIM;1", 2);
    capture_tpage("\\WDDR.TIM;1",  3);
}

/* Upload the four streamed textures from their resident RAM copies. Pure
   LoadImage — no CD access — safe during the room transition (the caller
   DrawSyncs first, as main's STATE_LOADING does). */
void hall_2f_upload_textures(void) {
    for (int i = 0; i < HALL_2F_NEW_TEX; i++)
        texmgr_upload(new_tex_id[i]);
    trick_drawers_upload_texture();   /* clsd_drwr -> cncrte/kchn_tile slot (unused here) */
}

/* ---- Stairs down to the conservatory ---------------------------------------
   The mesh has an open stairwell descending south (+Z) from the hall floor at
   x[-1988,-1688], z[-13..486] (down to y=290). The player arrives at its north
   lip (z~-13) after ascending. A floating "Press CIRCLE to descend" sign sits
   in front of the stairwell (XY plane, reads along X, faces north toward the
   corridor). A fresh Circle press within range transitions back down to the
   conservatory. */
#define STAIRS_X               (-1838)   /* centre of the stair width            */
#define STAIRS_Z                    0    /* north lip of the stairwell           */
#define STAIRS_TEXT_Y           (-186)
#define STAIRS_TEXT_RADIUS       1500
#define STAIRS_FADE_NEAR         1000
#define STAIRS_TRIGGER_RADIUS     350
#define STAIRS_TEXT_PIXEL           2    /* smaller so the line fits the stairs */

static int stairs_circle_prev = 1;

void hall_2f_stairs_arm(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    stairs_circle_prev = held;
}

int hall_2f_stairs_triggered(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    int just = held && !stairs_circle_prev;
    stairs_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - STAIRS_X;
    int32_t dz = cam_z - STAIRS_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < STAIRS_TRIGGER_RADIUS;
}

static void stairs_text(RenderContext *ctx) {
    int32_t dx = cam_x - STAIRS_X;
    int32_t dz = cam_z - STAIRS_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= STAIRS_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > STAIRS_FADE_NEAR) {
        int range = STAIRS_TEXT_RADIUS - STAIRS_FADE_NEAR;
        int prog  = xz - STAIRS_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* XY plane: door_draw_string_3d centres the reading axis (X) on world_x
       after adding 200, so pass STAIRS_X - 200. Sits just north (z-11) of the
       stairwell lip so it floats in front of the descent. The player views it
       from the -Z (corridor) side, so mirror=0 (opposite of the conservatory
       ascend sign, which is viewed from +Z). */
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to descend",
                        STAIRS_X - 200, STAIRS_TEXT_Y, STAIRS_Z - 11,
                        50, 255, 50, fade, 0, TEXT_PLANE_XY, STAIRS_TEXT_PIXEL);
}

/* ---- Far-east door out to the Reception -----------------------------------
   A single wooden door in the hall's east wall (x=101), centred on z=-320 (from
   the wd_dr polys in "2f Hall.smx"). Leads to Reception's 2nd-floor southerly
   door. The player approaches from the -X (room) side, so the sign is in the YZ
   plane with mirror=1, same as the conservatory's east door. */
#define EDOOR_X                    101
#define EDOOR_Z                 (-320)
#define EDOOR_TEXT_Y            (-186)
#define EDOOR_TEXT_RADIUS        1500
#define EDOOR_FADE_NEAR          1000
#define EDOOR_TRIGGER_RADIUS      500

/* Shared lock state (declared in hall_2f.h). The door is locked from the
   Reception side until the player unlocks it here in the Hall 2F. */
int hall_2f_door_unlocked = 0;

static int edoor_circle_prev = 1;

void hall_2f_edoor_arm(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    edoor_circle_prev = held;
}

int hall_2f_edoor_triggered(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    int just = held && !edoor_circle_prev;
    edoor_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - EDOOR_X;
    int32_t dz = cam_z - EDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= EDOOR_TRIGGER_RADIUS) return 0;

    /* First Circle press unlocks the door (no transition yet); once unlocked,
       further presses enter Reception. The Reception side reads the same flag. */
    if (!hall_2f_door_unlocked) {
        hall_2f_door_unlocked = 1;
        sound_play(SFX_UNLOCK);
        return 0;
    }
    return 1;
}

static void edoor_text(RenderContext *ctx) {
    int32_t dx = cam_x - EDOOR_X;
    int32_t dz = cam_z - EDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= EDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > EDOOR_FADE_NEAR) {
        int range = EDOOR_TEXT_RADIUS - EDOOR_FADE_NEAR;
        int prog  = xz - EDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx,
                        hall_2f_door_unlocked ? "Press " BTN_CIRCLE " to enter"
                                              : "Press " BTN_CIRCLE " to unlock",
                        EDOOR_X, EDOOR_TEXT_Y, EDOOR_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* ---- The two doors into the Master Bedroom ---------------------------------
   Both sit in the corridor's south wall (x from -2770 to 101 at z=-656, from
   the wd_dr polys in "2f Hall.smx"): the east one centred on x=-400 and the
   west one on x=-1889. The corridor is on the +Z side of that wall (collision
   wall 5's normal points +Z), so the player approaches both from +Z and the
   signs lie in the XY plane with mirror=1 — the opposite flip from the
   "descend" sign above, which is read from -Z. */
#define BDOOR_Z                 (-656)
#define BDOOR_E_X                (-400)
#define BDOOR_W_X               (-1889)
#define BDOOR_TEXT_Y            (-186)
#define BDOOR_TEXT_RADIUS        1500
#define BDOOR_FADE_NEAR          1000
#define BDOOR_TRIGGER_RADIUS      500
/* Stand this far north of the wall on arrival: clear of the 195 push radius and
   of the doors' own trigger radius is handled by arming them instead. */
#define BDOOR_SPAWN_Z           (-436)

static int bdoor_e_circle_prev = 1;
static int bdoor_w_circle_prev = 1;

static int hall_circle_held(void) {
    if (!pad_buff_len[0]) return 0;
    PadResponse *pad = (PadResponse *)pad_buff[0];
    return (~pad->btn & PAD_CIRCLE) ? 1 : 0;
}

void hall_2f_bdoors_arm(void) {
    int held = hall_circle_held();
    bdoor_e_circle_prev = held;
    bdoor_w_circle_prev = held;
}

static int bdoor_triggered(int32_t door_x, int *circle_prev) {
    int held = hall_circle_held();
    int just = held && !*circle_prev;
    *circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - door_x;
    int32_t dz = cam_z - BDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < BDOOR_TRIGGER_RADIUS;
}

int hall_2f_bdoor_e_triggered(void) {
    return bdoor_triggered(BDOOR_E_X, &bdoor_e_circle_prev);
}

int hall_2f_bdoor_w_triggered(void) {
    return bdoor_triggered(BDOOR_W_X, &bdoor_w_circle_prev);
}

/* XY plane: door_draw_string_3d centres the reading axis (X) on world_x after
   adding 200, so pass door_x - 200. Sits just north (z+11) of the wall so it
   floats in front of the door. */
static void bdoor_text(RenderContext *ctx, int32_t door_x) {
    int32_t dx = cam_x - door_x;
    int32_t dz = cam_z - BDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= BDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > BDOOR_FADE_NEAR) {
        int range = BDOOR_TEXT_RADIUS - BDOOR_FADE_NEAR;
        int prog  = xz - BDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        door_x - 200, BDOOR_TEXT_Y, BDOOR_Z + 11,
                        50, 255, 50, fade, 1, TEXT_PLANE_XY, DOOR_PIXEL_SIZE);
}

/* Coming back out of the bedroom: stand just north of the door the player used,
   facing +Z out into the corridor (the direction of travel through it). */
static void hall_2f_spawn_bdoor(int32_t door_x) {
    cam_x   = door_x;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = BDOOR_SPAWN_Z;
    cam_rot = 0;      /* facing +Z, into the corridor with the door behind */
    hall_2f_bdoors_arm();
    hall_2f_stairs_arm();
    hall_2f_edoor_arm();
}

void hall_2f_spawn_bdoor_e(void) { hall_2f_spawn_bdoor(BDOOR_E_X); }
void hall_2f_spawn_bdoor_w(void) { hall_2f_spawn_bdoor(BDOOR_W_X); }

void hall_2f_init(void) {
    hall_2f_collision_init(&current_collision_room);
    collision_set_ceiling_y(0);   /* proxy wall tops reach the drawn ceiling */
    hall_2f_floor_zones_init();

    /* Spawn just north of the stairwell lip, facing -Z down the corridor with
       the descent behind them — the natural direction of travel coming up the
       stairs (which climb toward -Z). Flat floor y=0, standing eye height as
       the conservatory. */
    cam_x   = STAIRS_X;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = -280;   /* clear of the z=-13 stairwell-edge wall's 195 push radius */
    cam_rot = 2048;   /* facing -Z (down the corridor, stairwell behind) */

    hall_2f_stairs_arm();   /* don't re-trigger on a held Circle from the entry */
    hall_2f_edoor_arm();    /* same for the far-east door out to reception */
    hall_2f_bdoors_arm();   /* and the two south-wall doors into the bedroom */
    trick_drawers_place();  /* the static chest of drawers in the west room */
}

static void draw_hall_2f_smd(RenderContext *ctx) {
    if (!hall_2f_smd) return;

    uint8_t *p = (uint8_t *)hall_2f_smd->p_prims;
    int i;

    for (i = 0; i < hall_2f_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &hall_2f_smd->p_verts[vi[0]];
        SVECTOR *v1 = &hall_2f_smd->p_verts[vi[1]];
        SVECTOR *v2 = &hall_2f_smd->p_verts[vi[2]];

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
           build time in hall_2f_nocull — same scheme as the conservatory. */
        int nocull = (i < HALL_2F_PRIM_COUNT) && hall_2f_nocull[i];
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
            v3 = &hall_2f_smd->p_verts[vi[3]];
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
           texture window set in hall_2f_draw. */
        uint8_t tex_idx = (i < HALL_2F_PRIM_COUNT) ? hall_2f_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < HALL_2F_TEX_COUNT);
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

void hall_2f_draw(RenderContext *ctx) {
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
       page. All seven hall textures sit at page-top (Voff 0), so one window
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

    draw_hall_2f_smd(ctx);
    /* Static chest of drawers (west room). Its texture sits at page-top (Voff 0)
       so the room's 128 texture window serves it; restores the view matrix
       before returning. */
    trick_drawers_draw(ctx);

    /* Breakable door in the corridor<->drawer-room doorway. Draws with the
       room's active 128 window (its wd_dr UVs are 0-127) and restores the view
       matrix before returning, which the zombie renderer below relies on. */
    fatdoors_draw(ctx);

    /* The two hall zombies. Their sprites sit at VRAM Voff>=128, so hand them the
       room's 128 texture window to bracket before they draw. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
        spiders_set_texwindow(&tw);
    }
    draw_zombies(ctx);
    draw_spiders(ctx);

    stairs_text(ctx);
    edoor_text(ctx);
    bdoor_text(ctx, BDOOR_E_X);
    bdoor_text(ctx, BDOOR_W_X);
}
