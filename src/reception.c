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
#include "reception.h"
#include "collision.h"
#include "reception_mesh_collision.h"
#include "reception_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "save_point.h"
#include "dresser.h"
#include "fatdoor.h"
#include "item_pickup.h"
#include "texmgr.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Placeholder reception room: untextured geometry rendered flat-shaded with the
   same distance fog as the kitchen, so it blends with the dark interior until
   the finished art replaces it. Modelled on kitchen_dining.c but with no
   textures/tex_map (Reception.smx carries no texture references yet). */

static SMD  *reception_smd  = NULL;
static void *reception_buff = NULL;

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

/* Multi-level floor layout (taken from the collision mesh, Reception_mesh.smx):
   ground (y=0) -> ramp A up to a y=-150 platform -> ramp B up to the y=-600
   upper floor. apply_height() picks the first matching zone, skipping flat/upper
   zones that sit ABOVE the player, so elevated zones must be listed BEFORE the
   ground catch-all (which spans the whole room beneath everything). */
static void reception_floor_zones_init(void) {
    int i = 0;

    /* Ramp A — climbs along +X from ground (y=0 @ x=-100) to the platform
       (y=-150 @ x=300). */
    floor_zones[i].type            = FLOOR_RAMP;
    floor_zones[i].min_x = -100; floor_zones[i].max_x = 300;
    floor_zones[i].min_z = -700; floor_zones[i].max_z = -300;
    floor_zones[i].ramp_y_start    = 0;     /* y at x=-100 (ground)   */
    floor_zones[i].ramp_y_end      = -150;  /* y at x=300  (platform) */
    floor_zones[i].ramp_axis_start = -100;
    floor_zones[i].ramp_axis_end   = 300;
    floor_zones[i].ramp_along_x    = 1;
    i++;

    /* Ramp B — climbs along +Z from the platform (y=-150 @ z=-300) to the
       upper floor (y=-600 @ z=500). */
    floor_zones[i].type            = FLOOR_RAMP;
    floor_zones[i].min_x = 300; floor_zones[i].max_x = 700;
    floor_zones[i].min_z = -300; floor_zones[i].max_z = 500;
    floor_zones[i].ramp_y_start    = -150;  /* y at z=-300 (platform)    */
    floor_zones[i].ramp_y_end      = -600;  /* y at z=500  (upper floor) */
    floor_zones[i].ramp_axis_start = -300;
    floor_zones[i].ramp_axis_end   = 500;
    floor_zones[i].ramp_along_x    = 0;
    i++;

    /* Platform between the two ramps (y=-150). */
    floor_zones[i].type  = FLOOR_UPPER;
    floor_zones[i].min_x = 300; floor_zones[i].max_x = 700;
    floor_zones[i].min_z = -700; floor_zones[i].max_z = -300;
    floor_zones[i].y     = -150;
    i++;

    /* Upper floor (y=-600), L-shaped: the +Z/+X area... */
    floor_zones[i].type  = FLOOR_UPPER;
    floor_zones[i].min_x = -900; floor_zones[i].max_x = 1500;
    floor_zones[i].min_z = 500;  floor_zones[i].max_z = 1500;
    floor_zones[i].y     = -600;
    i++;

    /* ...and the west strip. */
    floor_zones[i].type  = FLOOR_UPPER;
    floor_zones[i].min_x = -1500; floor_zones[i].max_x = -900;
    floor_zones[i].min_z = -1500; floor_zones[i].max_z = 1500;
    floor_zones[i].y     = -600;
    i++;

    /* Ground catch-all (whole room) — MUST be last so elevated zones win. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -1500; floor_zones[i].max_x = 1500;
    floor_zones[i].min_z = -1500; floor_zones[i].max_z = 1500;
    floor_zones[i].y     = 0;
    i++;

    floor_zone_count = i;
}

/* ---- Per-room textures -----------------------------------------------------
   Reception's nine mesh textures. Six are resident from startup (shared with the
   kitchen / fatdoor); three are UNIQUE to reception (strs, bnnstr, frnt_dr) and
   occupy VRAM slots the kitchen also uses, so they must be (re)uploaded into VRAM
   each time reception is entered.

   The unique textures' TIM data is held resident in RAM (preloaded at startup),
   so the entry-time upload is a PURE LoadImage with NO CD access. The old design
   did a CdRead at transition time, which hung: a mid-game CdRead competes with
   the CD-DA drive and the psxcd library gets into a bad state. Preloading the
   bytes at startup removes the only CD access from the transition. */
#define RECEPTION_TEX_COUNT 9
static uint16_t tex_tpage[RECEPTION_TEX_COUNT];
static uint16_t tex_clut[RECEPTION_TEX_COUNT];

/* The reception-only textures, with their reception_tex_map slot indices. The
   texture manager keeps them RAM-resident so reception_upload_textures() needs
   no CD read; new_tex_id[] holds the manager ids captured at startup. */
/* Slot 3 (formerly bnnstr, the banister) is retired: the mesh no longer
   references it, so nothing streams into the kchn_tile slot for reception —
   the piano room's piano_keys texture time-shares it instead. */
#define RECEPTION_NEW_TEX 2
static int new_tex_id[RECEPTION_NEW_TEX];
static const struct { const char *file; int slot; } new_tex[RECEPTION_NEW_TEX] = {
    { "\\TEX\\STRS.TIM;1",   0 },
    { "\\TEX\\FRNTDR.TIM;1", 5 },
};

/* Read a whole TIM into a freshly malloc'd buffer (caller owns it). NULL on fail. */
static uint8_t *read_tim(const char *filename) {
    CdlFILE file;
    if (!CdSearchFile(&file, filename)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    uint8_t *buf = malloc(sectors * 2048);
    if (!buf) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);
    return buf;
}

/* Capture tpage/clut for a texture that is ALREADY resident in VRAM (shared with
   another room): read its header only, no LoadImage. */
static void capture_tpage(const char *filename, int slot) {
    uint8_t *buf = read_tim(filename);
    if (!buf) return;
    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    if (tim.mode & 0x8) tex_clut[slot] = getClut(tim.crect->x, tim.crect->y);
    tex_tpage[slot] = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);
    free(buf);
}

/* Load geometry AND preload textures at STARTUP. Geometry and the six shared
   textures need only their headers/data read here (no mid-game CD); the three
   unique textures are kept resident in RAM for entry-time upload. All CD access
   happens here, at startup, where it is safe. */
void reception_load_assets(void) {
    reception_buff = load_file_from_cd("\\RECEPT.SMD;1");
    if (reception_buff)
        reception_smd = smdInitData(reception_buff);

    /* Register the 3 reception-only textures with the texture manager (RAM-
       resident, uploaded to VRAM on each reception entry) and capture their
       tpage/clut into the renderer's slot table. */
    for (int i = 0; i < RECEPTION_NEW_TEX; i++) {
        int slot = new_tex[i].slot;
        new_tex_id[i]   = texmgr_register(new_tex[i].file);
        tex_tpage[slot] = texmgr_tpage(new_tex_id[i]);
        tex_clut[slot]  = texmgr_clut(new_tex_id[i]);
    }

    /* The other 6 are resident from startup (kitchen + fatdoor); just capture
       their tpage/clut for reception's renderer. */
    capture_tpage("\\REDWLPPR.TIM;1", 1);
    capture_tpage("\\WDFLR.TIM;1",    2);
    capture_tpage("\\DINCL.TIM;1",    4);
    capture_tpage("\\WDDR.TIM;1",     6);
    capture_tpage("\\INRDBLDR.TIM;1", 7);
    capture_tpage("\\STNGLS.TIM;1",   8);
}

/* Upload reception's 3 unique textures into VRAM from their resident RAM copies.
   Pure LoadImage — no CD access — so it is safe during the room transition (the
   caller idles the GPU with DrawSync first, and kicks no draw until the next
   flip_buffers). */
void reception_upload_textures(void) {
    for (int i = 0; i < RECEPTION_NEW_TEX; i++)
        texmgr_upload(new_tex_id[i]);
}

/* ---- Door back to the kitchen ---------------------------------------------
   The double door on the bottom floor (where the player spawns). Mirrors the
   kitchen's "to reception" door: a floating "Press " BTN_CIRCLE " to enter" sign, and a fresh
   Circle press within range starts the transition back. */
#define RDOOR_X                  1450
#define RDOOR_Z                 (-414)
#define RDOOR_TEXT_Y            (-186)
#define RDOOR_TEXT_RADIUS        1500
#define RDOOR_FADE_NEAR          1000   /* fully opaque within this distance */
#define RDOOR_TRIGGER_RADIUS      500   /* distance at which Circle activates */

/* Circle edge-detect, seeded by reception_door_arm(). Starts "held" so a press
   carried in from the kitchen-side transition doesn't immediately bounce back. */
static int rdoor_circle_prev = 1;

void reception_door_arm(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    rdoor_circle_prev = held;
}

int reception_door_triggered(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    int just = held && !rdoor_circle_prev;
    rdoor_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - RDOOR_X;
    int32_t dz = cam_z - RDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < RDOOR_TRIGGER_RADIUS;
}

/* Floating "Press " BTN_CIRCLE " to enter" sign on the double door, in the YZ plane (faces
   along X). The player approaches from the -X (room) side, so mirror=1. */
static void reception_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - RDOOR_X;
    int32_t dz = cam_z - RDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= RDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > RDOOR_FADE_NEAR) {
        int range = RDOOR_TEXT_RADIUS - RDOOR_FADE_NEAR;
        int prog  = xz - RDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        RDOOR_X, RDOOR_TEXT_Y, RDOOR_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* ---- Door to the new west room --------------------------------------------
   On the west wall; the player approaches from the +X (room) side, so the
   sign is in the YZ plane with mirror=0 (same facing as the kitchen's
   "to reception" sign). Shares the fade radii and text height with RDOOR. */
#define WDOOR_X                (-1435)
#define WDOOR_Z                 (-756)

/* Circle edge-detect for the west door, seeded by wdoor_arm() (same pattern
   as rdoor_circle_prev above). */
static int wdoor_circle_prev = 1;

void wdoor_arm(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    wdoor_circle_prev = held;
}

int wdoor_triggered(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    int just = held && !wdoor_circle_prev;
    wdoor_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - WDOOR_X;
    int32_t dz = cam_z - WDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < RDOOR_TRIGGER_RADIUS;
}

static void wdoor_text(RenderContext *ctx) {
    int32_t dx = cam_x - WDOOR_X;
    int32_t dz = cam_z - WDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= RDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > RDOOR_FADE_NEAR) {
        int range = RDOOR_TEXT_RADIUS - RDOOR_FADE_NEAR;
        int prog  = xz - RDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        WDOOR_X, RDOOR_TEXT_Y, WDOOR_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

void reception_init(void) {
    reception_collision_init(&current_collision_room);
    reception_floor_zones_init();

    /* Spawn by the double door on the bottom floor, facing west (-X). */
    cam_x   = 1306;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = -414;
    cam_rot = 3072;   /* facing west (-X) */

    reception_door_arm();   /* don't re-trigger on a held Circle from the entry */
    wdoor_arm();            /* same, for the west single door */
    save_point_arm();       /* same, for the Circle-to-save interaction */

    /* Place reception's props. */
    save_points_clear();
    /* rot 512 = 45 deg (4096 = 360); scale 2048 = half size (4096 = full). */
    save_point_add(78, -300, -67, 512, 2048);

    dressers_clear();
    /* Dresser in the ground-floor room tucked under the upper floor (bottom-floor
       standing reference -189). rot_y is 0..4096 = a full turn. */
    dresser_add(580, -189, 958, 1024);
}

static void draw_reception_smd(RenderContext *ctx) {
    if (!reception_smd) return;

    uint8_t *p = (uint8_t *)reception_smd->p_prims;
    int i;

    for (i = 0; i < reception_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &reception_smd->p_verts[vi[0]];
        SVECTOR *v1 = &reception_smd->p_verts[vi[1]];
        SVECTOR *v2 = &reception_smd->p_verts[vi[2]];

        {
            int32_t dx = (int32_t)v0->vx - cam_x;
            int32_t dz = (int32_t)v0->vz - cam_z;
            /* Distance cull (Manhattan) at the fog-out distance so culled polys
               are already invisible. 1500 keeps the room GPU-fill within a 60fps
               frame (was 2300, which pushed the fill to VB2/30fps). */
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

        /* Backface cull exactly like the baseline, EXCEPT the handful of
           triangle-shaped (degenerate) quads flagged at build time in
           reception_nocull: their first triangle is collinear so gte_nclip is
           ~0 and the plain <=0 test flickered them in and out. Everything else
           (normal quads AND real triangles) uses the original test, so there is
           no extra back-face over-draw — perf matches the baseline. */
        int nocull = (i < RECEPTION_PRIM_COUNT) && reception_nocull[i];
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
            v3 = &reception_smd->p_verts[vi[3]];
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
        int32_t fog_start = 350, fog_end = 1500;   /* fog fully saturates at the cull distance */
        int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
        int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). Reception
           textures every face (no 0xFF), but keep the guard for safety. UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in reception_draw, reproducing the Blender UVs. */
        uint8_t tex_idx = (i < RECEPTION_PRIM_COUNT) ? reception_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < RECEPTION_TEX_COUNT);
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

void reception_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = 350; g_fog_far = 1500;

    /* Dark interior background, same as the kitchen. */
    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setXY0(bg, 0, 0);
    setWH(bg, SCREEN_XRES, SCREEN_YRES);
    setRGB0(bg, 20, 15, 10);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
    ctx->next_packet += sizeof(TILE);

    /* 128x128 texture window so per-poly UVs wrap (tile) within each texture's
       page. Sorted at OT_LENGTH-1 so the GPU applies it before any textured poly
       (those are clamped to OT_LENGTH-2). Mask = texels>>3 (128>>3 = 16). All
       reception textures sit at page-top (V 0-127), so one window serves them. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    /* View matrix from the camera (same construction as kitchen_dining_draw). */
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

    draw_reception_smd(ctx);
    /* Floating collectibles (Grave-olver + rounds) in the room behind the fat
       door. Billboards drawn in the active view matrix; their 64x64 sprites sit
       at VRAM Voff 0 so the room's 128 texture window leaves their UVs intact. */
    item_pickups_draw(ctx);
    /* Dresser props — reuse reception's resident wd_flr (slot 2) for their
       non-drawer faces and the room's 128 texture window set above; the module
       owns the drawer texture. Restores the view matrix before returning. */
    dressers_draw(ctx, tex_tpage[2], tex_clut[2]);
    /* Breakable door in the small-room doorway. Draws with reception's active
       128 texture window (its UVs are 0-127, so wrapping is a no-op) and restores
       the view matrix before returning. */
    fatdoors_draw(ctx);
    save_points_draw(ctx);
    reception_door_text(ctx);
    wdoor_text(ctx);
}
