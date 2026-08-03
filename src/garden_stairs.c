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
#include "garden_stairs.h"
#include "collision.h"
#include "garden_stairs_mesh_collision.h"
#include "garden_stairs_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "delivery_area.h"    /* delivery_upload_brick_wall */
#include "east_stairwell.h"   /* east_stairwell_upload_chnlnk */
#include "save_point.h"
#include "zombie.h"
#include "spider.h"
#include "web.h"
#include "item_pickup.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Garden Stairs: the caged stairway behind the Attic Exit's exit door. See
   garden_stairs.h for the layout. */

static SMD  *garden_stairs_smd  = NULL;
static void *garden_stairs_buff = NULL;

/* ---- Floor zones -----------------------------------------------------------
   A genuine multi-storey shaft: three landings stacked over the same west XZ
   footprint, three over the same east one, and five flights over the same
   middle strip. The floor-zone model has one Y per XZ box, so the levels are
   told apart by apply_height's rule that a surface ABOVE the player is not the
   one they are standing on — which means the zones must be listed HIGHEST
   FIRST (most negative Y first). Reorder them and the player drops through to
   the bottom of the shaft.

   Eleven zones against MAX_FLOOR_ZONES=16. Values come straight from the FLOOR
   list in garden_stairs_mesh_collision.c; each flight's two ends were read off
   the collision mesh's sloped quads (west end -> east end). */
static void gs_flat(int i, int32_t min_x, int32_t max_x,
                    int32_t min_z, int32_t max_z, int32_t y) {
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = min_x; floor_zones[i].max_x = max_x;
    floor_zones[i].min_z = min_z; floor_zones[i].max_z = max_z;
    floor_zones[i].y     = y;
}

/* A flight, always sloped along X between the flight strip's two edges. */
static void gs_flight(int i, int32_t min_z, int32_t max_z,
                      int32_t y_west, int32_t y_east) {
    floor_zones[i].type            = FLOOR_RAMP;
    floor_zones[i].min_x           = -1242; floor_zones[i].max_x = -267;
    floor_zones[i].min_z           = min_z; floor_zones[i].max_z = max_z;
    floor_zones[i].ramp_y_start    = y_west;
    floor_zones[i].ramp_y_end      = y_east;
    floor_zones[i].ramp_axis_start = -1242;
    floor_zones[i].ramp_axis_end   =  -267;
    floor_zones[i].ramp_along_x    = 1;
}

#define GS_WEST_X0   (-2140)
#define GS_WEST_X1   (-1242)
#define GS_EAST_X0     (-267)
#define GS_EAST_X1       713
#define GS_Z0           1117
#define GS_Z1           2317
#define GS_ZMID         1717   /* south flights below it, north flights above */

static void garden_stairs_floor_zones_init(void) {
    int i = 0;
    /* --- top of the shaft --- */
    gs_flat  (i++, GS_EAST_X0, GS_EAST_X1, GS_Z0,   GS_Z1,   -1500);
    gs_flight(i++, GS_Z0,   GS_ZMID, -1200, -1500);   /* south, climbing east */
    gs_flat  (i++, GS_WEST_X0, GS_WEST_X1, GS_Z0,   GS_Z1,   -1200);
    gs_flight(i++, GS_ZMID, GS_Z1,   -1200,  -900);   /* north, climbing west */
    gs_flat  (i++, GS_EAST_X0, GS_EAST_X1, GS_Z0,   GS_Z1,    -900);
    gs_flight(i++, GS_Z0,   GS_ZMID,  -600,  -900);   /* south, climbing east */
    gs_flat  (i++, GS_WEST_X0, GS_WEST_X1, GS_Z0,   GS_Z1,    -600);
    gs_flight(i++, GS_ZMID, GS_Z1,    -600,  -300);   /* north, climbing west */
    gs_flat  (i++, GS_EAST_X0, GS_EAST_X1, GS_Z0,   GS_Z1,    -300);
    gs_flight(i++, GS_Z0,   GS_ZMID,     0,  -300);   /* south, climbing east */
    gs_flat  (i++, GS_WEST_X0, GS_WEST_X1, GS_Z0,   GS_Z1,       0);
    /* --- bottom of the shaft --- */
    floor_zone_count = i;
}

/* ---- Per-room textures -----------------------------------------------------
   Four mesh textures, none resident from startup, so all four are uploaded on
   every entry. TWO are owned by other modules and just need their headers
   captured here; the other two are this room's new art:

     - brick_wall (the south wall the exit door sits in) lives in the
       DELIVERY-only slot at x768 y0. Delivery owns the RAM copy, so we call its
       narrow delivery_upload_brick_wall() rather than registering a second copy
       — texmgr has a hard TEXMGR_MAX and a registration past it fails SILENTLY,
       breaking that texture in every room. delivery_restore_textures() would
       have done it, but it also stamps gravel back over the chnlnk this room
       needs, hence the narrow call.
     - chnlnk (the cage, and most of this room) lives in the DELIVERY-only
       gravel slot (x640 y0), whose RAM copy the East Stairwell owns.

     - xt_dr_outr (the exit door's outer face) time-shares the clsd_drwr page
       (x384 y0). NOT the obvious delivery pages: this room draws brick_wall
       (x768) and chnlnk (x640) itself, and the rusty_fence page's right half is
       anzu3/anzu6, which are resident for the whole run and never re-uploaded.
       Every consumer of that page re-uploads on its own entry —
       trick_drawers_upload_texture (clsd_drwr, via the 2F hall),
       kitchen_restore_textures (kchn_tile), the conservatory / east hall /
       library / stairwells (cncrte) and the piano room (piano_keys) — so it
       adds no new restore obligation.
     - xt_dr_cg (the door at the bottom of the stairs) time-shares the opn_drwr
       page (x832 y0), already shared with double_door (delivery/kitchen) and
       con_tile (conservatory/attic stairwell). Same story: everyone restores.

   All four sit at Voff 0, so the one 128 texture window set in
   garden_stairs_draw serves them all (see tools/VRAM_MAP.txt). */
#define GARDEN_STAIRS_TEX_COUNT 4

/* Streamed slots we own a RAM copy of: engine slot -> texmgr id. */
#define GARDEN_STAIRS_NEW_TEX 2
static int new_tex_id[GARDEN_STAIRS_NEW_TEX];
static const struct { const char *file; int slot; } new_tex[GARDEN_STAIRS_NEW_TEX] = {
    { "\\TEX\\XTDRCG.TIM;1",   2 },
    { "\\TEX\\XTDROUTR.TIM;1", 3 },
};

static uint16_t tex_tpage[GARDEN_STAIRS_TEX_COUNT];
static uint16_t tex_clut[GARDEN_STAIRS_TEX_COUNT];

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

/* Capture tpage/clut for a texture whose upload another module owns: read its
   header only, no LoadImage. */
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
void garden_stairs_load_assets(void) {
    garden_stairs_buff = load_file_from_cd("\\TEX\\GRDNSTRS.SMD;1");
    if (garden_stairs_buff)
        garden_stairs_smd = smdInitData(garden_stairs_buff);

    /* Streamed slots we own: RAM-resident via the texture manager, uploaded on
       entry. */
    for (int i = 0; i < GARDEN_STAIRS_NEW_TEX; i++) {
        int slot = new_tex[i].slot;
        new_tex_id[i]   = texmgr_register(new_tex[i].file);
        tex_tpage[slot] = texmgr_tpage(new_tex_id[i]);
        tex_clut[slot]  = texmgr_clut(new_tex_id[i]);
    }

    /* Owned by other modules, whose narrow uploads garden_stairs_upload_textures
       calls. Header only — no LoadImage, no second RAM copy. */
    capture_tpage("\\BRIKWLL.TIM;1",  0);
    capture_tpage("\\TEX\\CHNLNK.TIM;1", 1);
}

/* Upload the streamed textures from their resident RAM copies. Pure LoadImage
   — no CD access — safe during the room transition (the caller DrawSyncs first,
   as main's STATE_LOADING does). */
void garden_stairs_upload_textures(void) {
    for (int i = 0; i < GARDEN_STAIRS_NEW_TEX; i++)
        texmgr_upload(new_tex_id[i]);       /* xt_dr_cg, xt_dr_outr */
    delivery_upload_brick_wall();           /* brick_wall -> its own slot   */
    east_stairwell_upload_chnlnk();         /* chnlnk     -> gravel slot    */
}

/* ---- The south-wall door back to the Attic Exit ----------------------------
   The xt_dr_outr poly spans x[-104,386] at z=1117, on the TOP landing
   (floor y=-1500). It is the OUTER face of the same door the exit-door puzzle
   unlocked, so it maps to the Attic Exit's north-wall door at x=0, z=1000.

   The player approaches from the +Z (room) side, so the sign lies in the XY
   plane with mirror=1 — the mirror image of the signs read from -Z. */
#define GS_DOOR_X                141    /* (-104 + 386) / 2 */
#define GS_DOOR_Z               1117
#define GS_TEXT_Y             (-1686)   /* eye level on the top landing */
#define GS_TEXT_RADIUS          1500
#define GS_FADE_NEAR            1000
#define GS_TRIGGER_RADIUS        500

/* Top-landing standing eye: floor y=-1500, less GROUND_FLOOR_Y and the 40-unit
   floor standoff apply_height applies. Spawning at the shaft's default cam_y
   would put the player under every landing and drop them to the bottom, so this
   is set explicitly. */
#define GS_TOP_EYE_Y   (-1500 - GROUND_FLOOR_Y - 40)

/* Circle edge-detect, seeded by garden_stairs_door_arm(). Starts "held" so a
   press carried in through the transition doesn't bounce the player straight
   back. */
static int door_circle_prev = 1;

static int circle_held(void) {
    if (!pad_buff_len[0]) return 0;
    PadResponse *pad = (PadResponse *)pad_buff[0];
    return (~pad->btn & PAD_CIRCLE) ? 1 : 0;
}

void garden_stairs_door_arm(void) {
    door_circle_prev = circle_held();
}

int garden_stairs_door_triggered(void) {
    int held = circle_held();
    int just = held && !door_circle_prev;
    door_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - GS_DOOR_X;
    int32_t dz = cam_z - GS_DOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < GS_TRIGGER_RADIUS;
}

/* Floating "Press O to enter" sign on the south-wall door. XY plane:
   door_draw_string_3d centres the reading axis (X) on world_x after adding 200,
   so pass door_x - 200. Sits just north (z+11) of the wall so it floats in
   front of the door. */
static void door_text(RenderContext *ctx) {
    int32_t dx = cam_x - GS_DOOR_X;
    int32_t dz = cam_z - GS_DOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= GS_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > GS_FADE_NEAR) {
        int range = GS_TEXT_RADIUS - GS_FADE_NEAR;
        int prog  = xz - GS_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        GS_DOOR_X - 200, GS_TEXT_Y, GS_DOOR_Z + 11,
                        50, 255, 50, fade, 1, TEXT_PLANE_XY, DOOR_PIXEL_SIZE);
}

/* Arriving from the Attic Exit: stand on the top landing north of the z=1117
   wall, far enough clear of the 195 push radius apply_collision_reception uses,
   facing +Z — the direction of travel through the door, with the stairs
   dropping away ahead. */
void garden_stairs_spawn_top(void) {
    cam_x   = GS_DOOR_X;
    cam_y   = GS_TOP_EYE_Y;
    cam_vy  = 0;
    cam_z   = GS_DOOR_Z + 215;
    cam_rot = 0;      /* facing +Z, into the room */
    garden_stairs_door_arm();
}

void garden_stairs_init(void) {
    garden_stairs_collision_init(&current_collision_room);
    /* The drawn mesh's roof is y=-2100, well above the proxy wall tops; state
       the DRAWN value so ceiling-mounted enemies hang flush with the roof the
       player can see (see tools/ADDING_A_ROOM.txt). */
    collision_set_ceiling_y(-2100);
    garden_stairs_floor_zones_init();

    /* Only one way in, so no per-door override is needed in main.c. */
    garden_stairs_spawn_top();

    /* Reception's save point and dresser prop are global (not room-swapped) and
       neither is area-gated in its collide routine, so reception's instances
       would block the player invisibly if they fell inside this room's bounds.
       Clearing them is safe: reception_init() re-places both on every reception
       entry. */
    save_points_clear();
    dressers_clear();
}

static void draw_garden_stairs_smd(RenderContext *ctx) {
    if (!garden_stairs_smd) return;

    uint8_t *p = (uint8_t *)garden_stairs_smd->p_prims;
    int i;

    for (i = 0; i < garden_stairs_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &garden_stairs_smd->p_verts[vi[0]];
        SVECTOR *v1 = &garden_stairs_smd->p_verts[vi[1]];
        SVECTOR *v2 = &garden_stairs_smd->p_verts[vi[2]];

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
           build time in garden_stairs_nocull — same scheme as the other rooms. */
        int nocull = (i < GARDEN_STAIRS_PRIM_COUNT) && garden_stairs_nocull[i];
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
            v3 = &garden_stairs_smd->p_verts[vi[3]];
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
           texture window set in garden_stairs_draw. */
        uint8_t tex_idx = (i < GARDEN_STAIRS_PRIM_COUNT) ? garden_stairs_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < GARDEN_STAIRS_TEX_COUNT);
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

void garden_stairs_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = 350; g_fog_far = 1500;

    /* Dark background, same as the other rooms. */
    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setXY0(bg, 0, 0);
    setWH(bg, SCREEN_XRES, SCREEN_YRES);
    setRGB0(bg, 20, 15, 10);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
    ctx->next_packet += sizeof(TILE);

    /* 128x128 texture window so per-poly UVs wrap (tile) within each texture's
       page. All four garden-stairs textures sit at page-top (Voff 0), so one
       window serves them (see tools/VRAM_MAP.txt). */
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

    draw_garden_stairs_smd(ctx);

    /* No enemies placed here yet; the room's 128 texture window is still handed
       to the sprite renderers so a future spawn brackets its Voff>=128 sprite
       correctly (see tools/TEXTURING_NOTES.txt PART 5). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
        spiders_set_texwindow(&tw);
    }
    draw_zombies(ctx);
    draw_spiders(ctx);
    webs_draw(ctx);
    item_pickups_draw(ctx);

    door_text(ctx);
}
