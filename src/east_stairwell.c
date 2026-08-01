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
#include "east_stairwell.h"
#include "collision.h"
#include "east_stairwell_mesh_collision.h"
#include "east_stairwell_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "concrete_props.h"
#include "hall_2f.h"
#include "save_point.h"
#include "zombie.h"
#include "spider.h"
#include "web.h"
#include "item_pickup.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* East Stairwell: the caged service passage between the East Hall and the
   Library, rendered the same way as the Library / master bedroom (per-poly tex
   map + 128 texture window + fog). See east_stairwell.h for the layout — the
   two chain-link fences at x=+/-44 are solid, so the room is two landings that
   only see each other through the wire. */

static SMD  *east_stairwell_smd  = NULL;
static void *east_stairwell_buff = NULL;

/* One flat zone per landing, both at y=0 (the two floors detected in
   east_stairwell_mesh_collision.c). The shaft strip x[-44,44] is deliberately
   left uncovered: it has no floor in the mesh and the fence walls (2 and 7)
   keep the player 195 units clear of it. */
static void east_stairwell_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -1750; floor_zones[0].max_x = -44;
    floor_zones[0].min_z = -349;  floor_zones[0].max_z = 349;
    floor_zones[0].y     = 0;

    floor_zones[1].type  = FLOOR_FLAT;
    floor_zones[1].min_x = 44;    floor_zones[1].max_x = 1750;
    floor_zones[1].min_z = -349;  floor_zones[1].max_z = 349;
    floor_zones[1].y     = 0;

    floor_zone_count = 2;
}

/* ---- Per-room textures -----------------------------------------------------
   Six mesh textures. Two are resident from startup (wd_flr with the kitchen,
   wd_dr with the fatdoor) and just need their headers captured. The other four
   live in time-shared slots and are (re)uploaded on every stairwell entry:

     - cncrte   occupies the kchn_tile slot (x384 y0) — the same slot the
                conservatory / East Hall / Library stream it into, so we call
                concrete_props_upload_textures() rather than keeping a second
                RAM copy. Already on kitchen_restore_textures()' list.
     - upstairs occupies the DELIVERY-only rusty_fence slot (x704 y0), and
     - strs     (the stair treads in the east landing's alcove) occupies the
                stn_stl slot (x320 y0).
                Both are already RAM-resident in hall_2f, so we call its narrow
                hall_2f_upload_upstairs()/hall_2f_upload_strs() rather than
                registering a third copy of each. That is not just tidiness:
                texmgr has a hard TEXMGR_MAX and a registration past it fails
                SILENTLY, handing back tpage/clut 0 — three extra registrations
                here once pushed concrete_props' cncrte and the dresser prop off
                the end of the table and broke them in EVERY room.
                delivery_restore_textures() puts rusty_fence back, and stn_stl
                is already on kitchen_restore_textures()' list.
     - chnlnk   is this room's only NEW art. It time-shares the DELIVERY-only
                gravel slot (x640 y0), which the conservatory already streams
                trees over — so it adds no new restore obligation:
                delivery_restore_textures() puts gravel back on delivery entry
                and conservatory_upload_textures() puts trees back on
                conservatory entry.

   chnlnk is 4bpp with its transparent wire gaps mapped to CLUT entry 0x0000,
   which the GPU skips entirely — so the geometry behind the fence shows
   through with no blend state and no extra sorting. All six textures sit at
   Voff 0, so the one 128 texture window set in east_stairwell_draw serves them
   all (see tools/VRAM_MAP.txt). */
#define EAST_STAIRWELL_TEX_COUNT 6

/* Streamed slots we own a RAM copy of: engine slot -> texmgr id. Only chnlnk —
   every other texture here is already registered by the module that owns it. */
#define EAST_STAIRWELL_NEW_TEX 1
static int new_tex_id[EAST_STAIRWELL_NEW_TEX];
static const struct { const char *file; int slot; } new_tex[EAST_STAIRWELL_NEW_TEX] = {
    { "\\TEX\\CHNLNK.TIM;1",   4 },
};

static uint16_t tex_tpage[EAST_STAIRWELL_TEX_COUNT];
static uint16_t tex_clut[EAST_STAIRWELL_TEX_COUNT];

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

/* Load geometry AND register/capture textures at STARTUP (the only time CD
   access is safe — see tools/TEXTURING_NOTES.txt). */
void east_stairwell_load_assets(void) {
    east_stairwell_buff = load_file_from_cd("\\TEX\\EASTSTRW.SMD;1");
    if (east_stairwell_buff)
        east_stairwell_smd = smdInitData(east_stairwell_buff);

    /* Streamed slots we own: RAM-resident via the texture manager, uploaded on
       entry. */
    for (int i = 0; i < EAST_STAIRWELL_NEW_TEX; i++) {
        int slot = new_tex[i].slot;
        new_tex_id[i]   = texmgr_register(new_tex[i].file);
        tex_tpage[slot] = texmgr_tpage(new_tex_id[i]);
        tex_clut[slot]  = texmgr_clut(new_tex_id[i]);
    }

    /* Owned by other modules, all of whose uploads east_stairwell_upload_textures
       calls: concrete_props (cncrte) and the 2F hall (upstairs, strs). Header
       only — no LoadImage, no second RAM copy. */
    capture_tpage("\\TEX\\CNCRTE.TIM;1",   0);
    capture_tpage("\\TEX\\UPSTAIRS.TIM;1", 3);
    capture_tpage("\\TEX\\STRS.TIM;1",     5);

    /* Resident from startup (kitchen + fatdoor). */
    capture_tpage("\\WDFLR.TIM;1", 1);
    capture_tpage("\\WDDR.TIM;1",  2);
}

/* Upload the streamed textures from their resident RAM copies. Pure LoadImage
   — no CD access — safe during the room transition (the caller DrawSyncs first,
   as main's STATE_LOADING does). */
void east_stairwell_upload_textures(void) {
    for (int i = 0; i < EAST_STAIRWELL_NEW_TEX; i++)
        texmgr_upload(new_tex_id[i]);       /* chnlnk   -> gravel slot        */
    concrete_props_upload_textures();       /* cncrte   -> kchn_tile slot     */
    hall_2f_upload_upstairs();              /* upstairs -> rusty_fence slot   */
    hall_2f_upload_strs();                  /* strs     -> stn_stl slot       */
}

/* ---- The two north-wall doors, one per landing -----------------------------
   Both come from the wd_dr polys in "East Stairwell.smx", both in the z=349
   wall (the collision wall behind the drawn z=350 face):

     WEST (x=-1380) -> the single wooden door in the East Hall's south wall
                       (east hall x=220, z=-992).
     EAST (x=503)   -> the single wooden door in the Library's south wall
                       (library x=-1400, z=-2080).

   The player approaches both from the -Z (room) side, so the signs lie in the
   XY plane with mirror=0 — the same orientation as the master bedroom's north-
   wall signs. The two landings are not connected, so only one of these is ever
   reachable from a given arrival. */
#define ESDOOR_Z                   349
#define ESDOOR_W_X             (-1380)
#define ESDOOR_E_X                 503
#define ESDOOR_TEXT_Y            (-186)
#define ESDOOR_TEXT_RADIUS        1500
#define ESDOOR_FADE_NEAR          1000
#define ESDOOR_TRIGGER_RADIUS      500

/* Circle edge-detect per door, seeded by east_stairwell_doors_arm(). Both start
   "held" so a press carried in through the transition doesn't bounce the player
   straight back out. */
static int wdoor_circle_prev = 1;
static int edoor_circle_prev = 1;
/* Same for the east landing's stairs up to the attic (see further down). */
static int esstairs_circle_prev = 1;

static int circle_held(void) {
    if (!pad_buff_len[0]) return 0;
    PadResponse *pad = (PadResponse *)pad_buff[0];
    return (~pad->btn & PAD_CIRCLE) ? 1 : 0;
}

void east_stairwell_doors_arm(void) {
    int held = circle_held();
    wdoor_circle_prev    = held;
    edoor_circle_prev    = held;
    esstairs_circle_prev = held;   /* the east landing's stairs, defined below */
}

/* Shared body: fresh Circle press within Manhattan range of (door_x, ESDOOR_Z).
   Each door owns its own edge-detect state, passed in by pointer. */
static int esdoor_triggered(int32_t door_x, int *circle_prev) {
    int held = circle_held();
    int just = held && !*circle_prev;
    *circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - door_x;
    int32_t dz = cam_z - ESDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < ESDOOR_TRIGGER_RADIUS;
}

int east_stairwell_wdoor_triggered(void) {
    return esdoor_triggered(ESDOOR_W_X, &wdoor_circle_prev);
}

int east_stairwell_edoor_triggered(void) {
    return esdoor_triggered(ESDOOR_E_X, &edoor_circle_prev);
}

/* Floating "Press O to enter" sign on one of the north-wall doors. XY plane:
   door_draw_string_3d centres the reading axis (X) on world_x after adding 200,
   so pass door_x - 200. Sits just south (z-11) of the wall so it floats in
   front of the door. */
static void esdoor_text(RenderContext *ctx, int32_t door_x) {
    int32_t dx = cam_x - door_x;
    int32_t dz = cam_z - ESDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= ESDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > ESDOOR_FADE_NEAR) {
        int range = ESDOOR_TEXT_RADIUS - ESDOOR_FADE_NEAR;
        int prog  = xz - ESDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        door_x - 200, ESDOOR_TEXT_Y, ESDOOR_Z - 11,
                        50, 255, 50, fade, 0, TEXT_PLANE_XY, DOOR_PIXEL_SIZE);
}

/* ---- The stairs up to the Attic Stairwell ----------------------------------
   The east landing's east wall (x=1750) backs onto the modelled stair alcove,
   whose three treads climb east across z[-175,175] to the "upstairs" image on
   its back wall. The player stands in the landing, on the -X side, so the
   floating sign lies in the YZ plane with mirror=1 (the mirror image of the
   attic's, which is read from +X). A fresh Circle press in range hands off to
   the same stair-climb transition the conservatory and the 2F hall use. */
#define ESSTAIRS_X                1750   /* the alcove wall (foot of the stairs) */
#define ESSTAIRS_Z                   0   /* centre of the stair width            */
#define ESSTAIRS_TEXT_Y          (-186)
#define ESSTAIRS_TEXT_RADIUS      1500
#define ESSTAIRS_FADE_NEAR        1000
#define ESSTAIRS_TRIGGER_RADIUS    350
#define ESSTAIRS_TEXT_PIXEL          2   /* smaller so the line fits the stairs */

/* Its Circle edge-detect (esstairs_circle_prev) is declared with the two doors'
   above and seeded by east_stairwell_doors_arm() along with them. */
int east_stairwell_stairs_triggered(void) {
    int held = circle_held();
    int just = held && !esstairs_circle_prev;
    esstairs_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - ESSTAIRS_X;
    int32_t dz = cam_z - ESSTAIRS_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < ESSTAIRS_TRIGGER_RADIUS;
}

static void esstairs_text(RenderContext *ctx) {
    int32_t dx = cam_x - ESSTAIRS_X;
    int32_t dz = cam_z - ESSTAIRS_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= ESSTAIRS_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > ESSTAIRS_FADE_NEAR) {
        int range = ESSTAIRS_TEXT_RADIUS - ESSTAIRS_FADE_NEAR;
        int prog  = xz - ESSTAIRS_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* YZ plane: door_draw_string_3d centres the reading axis (Z) on world_z
       after adding 200, so pass ESSTAIRS_Z - 200. Sits just west (x-11) of the
       alcove wall so it floats in front of the climb. The player views it from
       the -X (landing) side, so mirror=1. */
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to ascend",
                        ESSTAIRS_X - 11, ESSTAIRS_TEXT_Y, ESSTAIRS_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ, ESSTAIRS_TEXT_PIXEL);
}

/* Spawn just inside a door, far enough south of the z=349 wall to clear the 195
   push radius apply_collision_reception uses, facing -Z into the landing. */
void east_stairwell_spawn_west(void) {
    cam_x   = ESDOOR_W_X;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = 110;
    cam_rot = 2048;   /* facing -Z, into the west landing */
    east_stairwell_doors_arm();
}

void east_stairwell_spawn_east(void) {
    cam_x   = ESDOOR_E_X;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = 110;
    cam_rot = 2048;   /* facing -Z, into the east landing */
    east_stairwell_doors_arm();
}

/* Coming back DOWN out of the attic: stand west of the alcove wall, clear of
   the 195 push radius, facing -X — the direction of travel out of the descent,
   with the stairs behind. */
void east_stairwell_spawn_stairs(void) {
    cam_x   = 1500;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = ESSTAIRS_Z;
    cam_rot = 3072;   /* facing -X, into the east landing */
    east_stairwell_doors_arm();
}

void east_stairwell_init(void) {
    east_stairwell_collision_init(&current_collision_room);
    /* Proxy walls top out at y=-519, one unit shy of the drawn ceiling; state
       the DRAWN value so ceiling-mounted enemies hang flush with the roof the
       player can see. */
    collision_set_ceiling_y(-520);
    east_stairwell_floor_zones_init();

    /* Default spawn: the west landing (main.c overrides it for an arrival from
       the Library, which lands on the east one). */
    east_stairwell_spawn_west();

    /* Reception's save point and dresser prop are global (not room-swapped) and
       neither is area-gated in its collide routine, so reception's instances
       would block the player invisibly inside this room's bounds — the save
       point sits at (78,-67), inside the stairwell shaft's east fence. Clearing
       them is safe: reception_init() re-places both on every reception entry. */
    save_points_clear();
    dressers_clear();
}

static void draw_east_stairwell_smd(RenderContext *ctx) {
    if (!east_stairwell_smd) return;

    uint8_t *p = (uint8_t *)east_stairwell_smd->p_prims;
    int i;

    for (i = 0; i < east_stairwell_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &east_stairwell_smd->p_verts[vi[0]];
        SVECTOR *v1 = &east_stairwell_smd->p_verts[vi[1]];
        SVECTOR *v2 = &east_stairwell_smd->p_verts[vi[2]];

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
           build time in east_stairwell_nocull — same scheme as the other rooms. */
        int nocull = (i < EAST_STAIRWELL_PRIM_COUNT) && east_stairwell_nocull[i];
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
            v3 = &east_stairwell_smd->p_verts[vi[3]];
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
           texture window set in east_stairwell_draw. */
        uint8_t tex_idx = (i < EAST_STAIRWELL_PRIM_COUNT) ? east_stairwell_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < EAST_STAIRWELL_TEX_COUNT);
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

void east_stairwell_draw(RenderContext *ctx) {
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
       page. All six stairwell textures sit at page-top (Voff 0), so one window
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

    draw_east_stairwell_smd(ctx);

    /* No enemies placed here yet (just the west landing's box of rounds, seeded
       in world.c); the room's 128 texture window is still handed to the sprite
       renderers so a future spawn brackets its Voff>=128 sprite correctly
       (see tools/TEXTURING_NOTES.txt PART 5). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
        spiders_set_texwindow(&tw);
    }
    draw_zombies(ctx);
    draw_spiders(ctx);
    webs_draw(ctx);
    item_pickups_draw(ctx);

    esdoor_text(ctx, ESDOOR_W_X);
    esdoor_text(ctx, ESDOOR_E_X);
    esstairs_text(ctx);
}
