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
#include "kitchen_dining.h"
#include "collision.h"
#include "kitchen_dining_mesh_collision.h"
#include "kitchen_dining_tex_map.h"
#include "door.h"
#include "fatdoor.h"
#include "zombie.h"
#include "dining_table.h"
#include "sml_med.h"
#include "particles.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Kitchen door back to the delivery area (the "double_door" mesh: YZ plane at
   X=600, Z centered on 583). Text floats just above eye level (cam_y ~ -149),
   same look as the delivery-area door sign. Viewed from the -X interior side,
   so the text is mirrored. */
#define KDOOR_X            600
#define KDOOR_Z            583
#define KDOOR_TEXT_Y      (-186)
#define KDOOR_TEXT_RADIUS    1500
#define KDOOR_FADE_NEAR      1000   /* fully opaque within this distance */
#define KDOOR_TRIGGER_RADIUS  500   /* distance at which Circle activates the door */

/* Stove sign ("Press O to ignite"), rotated 90deg CCW (XY plane, fixed Z).
   TODO: set X/Z over the stove — walk there in debug mode (Select) and read
   cam_x/cam_z. Flip STOVE_TEXT_MIRROR (0/1) if the text reads backwards. */
#define STOVE_TEXT_X         13
#define STOVE_TEXT_Z        980
#define STOVE_TEXT_MIRROR    0
#define STOVE_TEXT_PIXEL     2   /* half the default sign size (DOOR_PIXEL_SIZE=4) */

/* Stove flame: where it burns and how close the player must stand to toggle it.
   FIRE_Y is the burner height the flame rises from (negative Y = up; floor ~0,
   eye level -149). Tune FIRE_Y/RADIUS/RATE to taste. */
#define STOVE_FIRE_X        (-107)   /* moved 120 toward the dining room (-X) */
#define STOVE_FIRE_Z        1080
#define STOVE_FIRE_Y        (-105)
#define STOVE_TRIGGER_RADIUS  500
#define STOVE_FIRE_RATE        2   /* new flame particles emitted per frame */

/* "to reception" door sign ("Press O to enter"), rotated 180deg (YZ plane,
   mirror=0) from the kitchen door sign. Circle within the trigger radius opens
   it and loads the Reception area. */
#define TO_RECEPTION_TEXT_X (-3255)
#define TO_RECEPTION_TEXT_Z  (-26)
#define TO_RECEPTION_TRIGGER_RADIUS  500

/* Circle edge-detect for the kitchen door; seeded by kitchen_door_arm(). */
static int kdoor_circle_prev = 1;

/* Stove flame toggle + its own Circle edge-detect (independent of the door's,
   so a single press near the stove only toggles the flame). */
static int stove_lit         = 0;
static int stove_circle_prev = 1;

/* "to reception" door has its own Circle edge-detect too. */
static int rdoor_circle_prev = 1;

static SMD  *kitchen_smd  = NULL;
static void *kitchen_buff = NULL;

/* tpage/clut per texture slot, indexed by the SMX texture order:
   0=stn_stl 1=kchn_wl 2=kchn_tile 3=wd_flr 4=red_wlppr 5=inr_dbl_dr
   6=red_crpt 7=stn_gls 8=stove 9=din_cl 10=double_door */
#define KITCHEN_TEX_COUNT 11
static uint16_t tex_tpage[KITCHEN_TEX_COUNT];
static uint16_t tex_clut[KITCHEN_TEX_COUNT];

static void *load_file_from_cd(const char *filename) {
    CdlFILE file;
    if (!CdSearchFile(&file, filename)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    void *buff = malloc(sectors * 2048);
    if (!buff) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buff, CdlModeSpeed);
    CdReadSync(0, NULL);
    return buff;
}

/* Load a TIM into VRAM using a caller-supplied scratch buffer, so all kitchen
   textures share ONE allocation instead of mallocing/freeing per file. This
   avoids heap churn/leakage from many transient buffers (which was crashing
   the kitchen load). */
static void load_tim_buf(const char *filename, int slot,
                         uint8_t *buf, int bufcap) {
    CdlFILE file;
    if (!CdSearchFile(&file, filename)) return;
    int sectors = (file.size + 2047) / 2048;
    if (sectors * 2048 > bufcap) return;   /* file too big for scratch buffer */
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);

    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);
    if (tim.mode & 0x8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
        tex_clut[slot] = getClut(tim.crect->x, tim.crect->y);
    }
    tex_tpage[slot] = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);
}

static void kitchen_dining_floor_zones_init(void) {
    int i = 0;

    /* Zones are flat at y=0 and EXTENDED past their true edges so adjacent
       zones OVERLAP across each doorway threshold — otherwise the gaps where
       the doors sit fall between zones and the player drops through. Overlap is
       harmless since every zone is the same height. */

    /* Large kitchen room (FLOOR 7) — +X edge -629 -> -590 (overlap dining). */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -3294; floor_zones[i].max_x = -590;
    floor_zones[i].min_z = -1000; floor_zones[i].max_z =  1000;
    floor_zones[i].y     = 0;
    i++;

    /* Dining / entry area (FLOOR 8) — -X -600 -> -640 (overlap big room),
       -Z -1000 -> -1040 (overlap corridor + SE room). */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -640; floor_zones[i].max_x = 600;
    floor_zones[i].min_z = -1040; floor_zones[i].max_z = 1000;
    floor_zones[i].y     = 0;
    i++;

    /* South corridor (FLOOR 9) — +Z -1025 -> -980 (overlap dining). */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -581; floor_zones[i].max_x = -8;
    floor_zones[i].min_z = -1699; floor_zones[i].max_z = -980;
    floor_zones[i].y     = 0;
    i++;

    /* Southeast little room (FLOOR 7) — +Z -1025 -> -980 (overlap dining). */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x =  18; floor_zones[i].max_x = 591;
    floor_zones[i].min_z = -1699; floor_zones[i].max_z = -980;
    floor_zones[i].y     = 0;
    i++;

    floor_zone_count = i;
}

/* Load all kitchen CD assets (textures + geometry) at STARTUP, before the main
   render loop begins. This MUST happen at startup, not mid-game:
   - LoadImage is unreliable once the per-frame VSync/draw cycle is running
     (it hard-crashes after a few uploads), but is safe at startup.
   - Doing it at startup also means there are NO CD reads during gameplay, so
     area transitions never have to stop the CD-DA music.
   Kitchen textures occupy VRAM regions that don't overlap delivery-area or prop
   textures, so all stay resident together. */
/* Upload the kitchen's 11 textures into VRAM. Called at startup AND on every
   transition into the kitchen, because the reception room streams its own
   textures over some of these VRAM slots (stn_stl/kchn_tile/red_crpt) — so they
   must be restored when the player returns. Safe mid-game only when the caller
   has idled the GPU first (see main STATE_LOADING). */
void kitchen_stream_textures(void) {
    const int TBUF_CAP = 32 * 1024;
    uint8_t *tbuf = malloc(TBUF_CAP);
    if (tbuf) {
        load_tim_buf("\\TEX\\STNSTL.TIM;1",   0, tbuf, TBUF_CAP);
        load_tim_buf("\\TEX\\KCHNWL.TIM;1",   1, tbuf, TBUF_CAP);
        load_tim_buf("\\TEX\\KCHNTILE.TIM;1", 2, tbuf, TBUF_CAP);
        load_tim_buf("\\WDFLR.TIM;1",    3, tbuf, TBUF_CAP);
        load_tim_buf("\\REDWLPPR.TIM;1", 4, tbuf, TBUF_CAP);
        load_tim_buf("\\INRDBLDR.TIM;1", 5, tbuf, TBUF_CAP);
        load_tim_buf("\\TEX\\REDCRPT.TIM;1",  6, tbuf, TBUF_CAP);
        load_tim_buf("\\STNGLS.TIM;1",   7, tbuf, TBUF_CAP);
        load_tim_buf("\\TEX\\STOVE.TIM;1",    8, tbuf, TBUF_CAP);
        load_tim_buf("\\DINCL.TIM;1",    9, tbuf, TBUF_CAP);
        load_tim_buf("\\DBLDOOR.TIM;1", 10, tbuf, TBUF_CAP);
        free(tbuf);
    }
}

/* The three kitchen textures whose VRAM slots reception overwrites with its own
   (stn_stl, kchn_tile, red_crpt). Their TIM data is preloaded into resident RAM
   at startup so they can be re-uploaded with a pure LoadImage — NO mid-game CD
   read (which would hang the drive) — when the player returns from reception.
   Same approach reception uses for its own textures (reception_upload_textures);
   their tpage/clut are unchanged (same VRAM slots), so only the pixels need
   restoring. */
/* kchn_wl is stomped by the dresser prop's texture while the player is in
   reception (the dresser streams into kchn_wl's VRAM slot, x512 y0), so it must
   be restored on return alongside the three reception overwrites. */
#define KITCHEN_SHARED_TEX 4
static uint8_t  *shared_tim_buf[KITCHEN_SHARED_TEX];
static TIM_IMAGE shared_tim[KITCHEN_SHARED_TEX];
static const char *shared_tex_file[KITCHEN_SHARED_TEX] = {
    "\\TEX\\STNSTL.TIM;1",
    "\\TEX\\KCHNTILE.TIM;1",
    "\\TEX\\REDCRPT.TIM;1",
    "\\TEX\\KCHNWL.TIM;1",
};

void kitchen_load_assets(void) {
    kitchen_stream_textures();   /* initial upload at startup */

    /* Preload the reception-shared textures into resident RAM for re-upload on
       return from reception. All CD access happens here, at startup. */
    for (int i = 0; i < KITCHEN_SHARED_TEX; i++) {
        CdlFILE file;
        if (!CdSearchFile(&file, shared_tex_file[i])) continue;
        int sectors = (file.size + 2047) / 2048;
        shared_tim_buf[i] = malloc(sectors * 2048);
        if (!shared_tim_buf[i]) continue;
        CdControl(CdlSetloc, &file.pos, NULL);
        CdRead(sectors, (uint32_t *)shared_tim_buf[i], CdlModeSpeed);
        CdReadSync(0, NULL);
        GetTimInfo((uint32_t *)shared_tim_buf[i], &shared_tim[i]);
    }

    /* Geometry, kept resident so entering the kitchen needs no CD read. */
    kitchen_buff = load_file_from_cd("\\KITCHN.SMD;1");
    if (kitchen_buff)
        kitchen_smd = smdInitData(kitchen_buff);
}

/* Re-upload the reception-shared textures into VRAM from their resident RAM
   copies. Pure LoadImage, no CD access — safe to call mid-game on the room
   transition (the caller idles the GPU with DrawSync first), exactly like
   reception_upload_textures. Call only when returning from reception. */
void kitchen_restore_textures(void) {
    for (int i = 0; i < KITCHEN_SHARED_TEX; i++) {
        if (!shared_tim_buf[i]) continue;
        LoadImage(shared_tim[i].prect, shared_tim[i].paddr);
        DrawSync(0);
        if (shared_tim[i].mode & 0x8) {
            LoadImage(shared_tim[i].crect, shared_tim[i].caddr);
            DrawSync(0);
        }
    }
}

void kitchen_dining_init(void) {
    kitchen_dining_collision_init(&current_collision_room);
    kitchen_dining_floor_zones_init();

    cam_x   = 381;
    cam_y   = -149;   /* floor at world y=0 → target = 0 - GROUND_FLOOR_Y = -149 */
    cam_vy  = 0;
    cam_z   = 676;
    cam_rot = 3072;   /* facing west (-X) */
    /* Assets (textures + geometry) are already loaded by kitchen_load_assets(). */
}

/* Seed the Circle edge state on entering the kitchen, so a press held from the
   delivery door doesn't immediately bounce the player back. */
void kitchen_door_arm(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    kdoor_circle_prev = held;
    stove_circle_prev = held;
    rdoor_circle_prev = held;
}

/* Returns 1 when Circle is freshly pressed within range of the kitchen door. */
int kitchen_door_triggered(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    int just = held && !kdoor_circle_prev;
    kdoor_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - KDOOR_X;
    int32_t dz = cam_z - KDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < KDOOR_TRIGGER_RADIUS;
}

/* Returns 1 when Circle is freshly pressed within range of the "to reception"
   door (same scheme as the kitchen door, separate edge state). */
int to_reception_door_triggered(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    int just = held && !rdoor_circle_prev;
    rdoor_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - TO_RECEPTION_TEXT_X;
    int32_t dz = cam_z - TO_RECEPTION_TEXT_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < TO_RECEPTION_TRIGGER_RADIUS;
}

/* Toggle the stove flame on a fresh Circle press near the stove, then keep it
   fed while lit. update_fire() runs regardless so a just-extinguished flame
   finishes rising out instead of vanishing. Called each frame in the kitchen. */
void kitchen_stove_update(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    int just = held && !stove_circle_prev;
    stove_circle_prev = held;

    if (just) {
        int32_t dx = cam_x - STOVE_FIRE_X;
        int32_t dz = cam_z - STOVE_FIRE_Z;
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (xz < STOVE_TRIGGER_RADIUS) stove_lit = !stove_lit;
    }

    if (stove_lit) fire_emit(STOVE_FIRE_X, STOVE_FIRE_Y, STOVE_FIRE_Z, STOVE_FIRE_RATE);
    update_fire();
}

/* Extinguish the stove and clear any lingering flame particles (new game). */
void kitchen_stove_reset(void) {
    stove_lit = 0;
    reset_fire();
}

static void draw_kitchen_smd(RenderContext *ctx) {
    if (!kitchen_smd) return;

    uint8_t *p = (uint8_t *)kitchen_smd->p_prims;
    int i;

    for (i = 0; i < kitchen_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &kitchen_smd->p_verts[vi[0]];
        SVECTOR *v1 = &kitchen_smd->p_verts[vi[1]];
        SVECTOR *v2 = &kitchen_smd->p_verts[vi[2]];

        {
            int32_t dx = (int32_t)v0->vx - cam_x;
            int32_t dz = (int32_t)v0->vz - cam_z;
            /* Distance cull (Manhattan), just past where fog fully saturates
               (fog_end below) so culled polys are already invisible. */
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 2300)
                { p += stride; continue; }
            /* Behind-camera cull: dot of (poly-cam) with the camera forward
               vector (isin,icos, scaled by 4096). Skip polys clearly behind the
               view before paying for the GTE transform. The generous margin
               keeps large polys that straddle the view plane from popping. */
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

        if (!pt->nocull) {
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }

        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        if (is_quad) {
            SVECTOR *v3 = &kitchen_smd->p_verts[vi[3]];
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
        if (otz <= 0) { p += stride; continue; }
        otz += 40;
        /* Reserve OT_LENGTH-1 for the texture-window primitive (set in
           kitchen_dining_draw) so it is processed before every textured poly. */
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog_start = 350, fog_end = 2200;
        int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
        int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        uint8_t tex_idx = (i < KITCHEN_DINING_PRIM_COUNT) ? kitchen_dining_tex_map[i] : 0xFF;

        if (is_quad && tex_idx != 0xFF && tex_idx < KITCHEN_TEX_COUNT) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            /* Use the exporter's per-poly UVs straight from the SMD primitive
               (FT4 stride 32: tu0,tv0..tu3,tv3 at offsets 20-27). They are
               already normalized into 0-255 and wrap via the 128 texture
               window set in kitchen_dining_draw, reproducing the Blender UVs. */
            uint8_t *uv = p + 20;
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly,
                (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8));
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
            setRGB0(poly,
                (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8));
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F4);
        } else {
            if (ctx->next_packet + sizeof(POLY_F3) > buf_end) { p += stride; continue; }
            POLY_F3 *poly = (POLY_F3 *)ctx->next_packet;
            setPolyF3(poly);
            setRGB0(poly,
                (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8));
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F3);
        }

        p += stride;
    }
}

/* Floating "Press O to enter" sign at the kitchen door, shown when the player
   is within range, fading in from KDOOR_TEXT_RADIUS to KDOOR_FADE_NEAR.
   The view matrix set in kitchen_dining_draw is still active here. */
static void kitchen_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - KDOOR_X;
    int32_t dz = cam_z - KDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= KDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > KDOOR_FADE_NEAR) {
        int range = KDOOR_TEXT_RADIUS - KDOOR_FADE_NEAR;
        int prog  = xz - KDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* world_z is offset by -200: door_draw_string_3d adds 200 internally, so
       this centres the text on KDOOR_Z. mirror=1 for the -X viewing side. */
    door_draw_string_3d(ctx, "Press O to enter",
                        KDOOR_X, KDOOR_TEXT_Y, KDOOR_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* Floating "Press O to ignite" sign over the stove. Rotated 90deg CCW from the
   door signs: it lies in the XY plane (fixed Z) so it reads along X and faces
   along Z. mirror flips reading direction for the side the player approaches. */
static void stove_text(RenderContext *ctx) {
    int32_t dx = cam_x - STOVE_TEXT_X;
    int32_t dz = cam_z - STOVE_TEXT_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= KDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > KDOOR_FADE_NEAR) {
        int range = KDOOR_TEXT_RADIUS - KDOOR_FADE_NEAR;
        int prog  = xz - KDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* XY plane: door_draw_string_3d centres the reading axis (X) on world_x
       after adding 200, so pass STOVE_TEXT_X - 200 to centre on the stove. */
    door_draw_string_3d(ctx, stove_lit ? "Press O to extinguish" : "Press O to ignite",
                        STOVE_TEXT_X - 200, KDOOR_TEXT_Y, STOVE_TEXT_Z,
                        50, 255, 50, fade, STOVE_TEXT_MIRROR, TEXT_PLANE_XY, STOVE_TEXT_PIXEL);
}

/* Floating "Press O to enter" sign at the "to reception" door, rotated 180deg
   from the kitchen door sign: same YZ plane, opposite facing side, so mirror=0
   (vs. 1 for the kitchen door). */
static void to_reception_text(RenderContext *ctx) {
    int32_t dx = cam_x - TO_RECEPTION_TEXT_X;
    int32_t dz = cam_z - TO_RECEPTION_TEXT_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= KDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > KDOOR_FADE_NEAR) {
        int range = KDOOR_TEXT_RADIUS - KDOOR_FADE_NEAR;
        int prog  = xz - KDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press O to enter",
                        TO_RECEPTION_TEXT_X, KDOOR_TEXT_Y, TO_RECEPTION_TEXT_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

void kitchen_dining_draw(RenderContext *ctx) {
    /* Dark interior background */
    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setXY0(bg, 0, 0);
    setWH(bg, SCREEN_XRES, SCREEN_YRES);
    setRGB0(bg, 20, 15, 10);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
    ctx->next_packet += sizeof(TILE);

    /* 128x128 texture window so per-poly UVs wrap (tile) within each texture's
       page. Placed at the top of the OT (OT_LENGTH-1) so the GPU applies it
       before any textured kitchen poly (those are clamped to OT_LENGTH-2).
       Mask is texels>>3, matching the SDK convention (128>>3 = 16). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

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

    draw_kitchen_smd(ctx);
    /* Static dining tables — drawn with the wd_flr texture (slot 2) the kitchen
       keeps resident, reusing the 128 texture window set above. Restores the
       view matrix before returning. */
    dining_tables_draw(ctx, tex_tpage[3], tex_clut[3]);
    /* Floating pickups. Draws billboards in the active view matrix (restored by
       dining_tables_draw); the 64x64 sprite's UVs sit inside the 128 texture
       window so no bracketing is needed. */
    sml_meds_draw(ctx);
    kitchen_door_text(ctx);
    stove_text(ctx);
    to_reception_text(ctx);
    draw_fire(ctx);       /* stove flame billboards in the active view matrix */
    fatdoors_draw(ctx);   /* breakable entryway doors (restores view matrix) */

    /* Tell the zombie renderer about our texture window (set above at
       OT_LENGTH-1) so it can draw its sprites unmasked and then restore it. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
    }
    draw_zombies(ctx);
    /* Player overlays + debug collision view are drawn by the shared
       draw_player_systems step in main (applies to every area). */
}
