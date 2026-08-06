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
#include "garden_courtyard.h"
#include "collision.h"
#include "garden_courtyard_mesh_collision.h"
#include "garden_courtyard_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "garden_stairs.h"    /* garden_stairs_upload_textures */
#include "save_point.h"
#include "zombie.h"
#include "spider.h"
#include "rabisu.h"
#include "rabisu_boss.h"
#include "web.h"
#include "item_pickup.h"
#include "sml_med.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Garden Courtyard: the walled garden at the foot of the Garden Stairs. See
   garden_courtyard.h for the layout. */

static SMD  *garden_courtyard_smd  = NULL;
static void *garden_courtyard_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   One budget, unlike the Garden Stairs' split: there is no cage here to see out
   of, just the one open space. Cull and fog-far are equal, the invariant that
   makes culling invisible — nothing is dropped until the fog has already faded
   it into the background.

   3500 is set from the mesh: the courtyard is 4580 x 4000, so from the middle
   every wall is within ~2300 Manhattan and from a corner the two near walls are
   in range while the far ones fade out. The whole mesh is only 550 prims, so
   drawing all of it would fit the 64KB packet buffer — the budget is chosen for
   how far the garden should READ, not to stay inside it. */
#define GC_CULL_DIST      3500
#define GC_FOG_NEAR        800
#define GC_FOG_FAR        3500

/* Wall standoff, wider than the 195 the interior rooms use, for the same reason
   the Garden Stairs needs it: the perimeter is drawn 1700 tall, so standing
   close to it puts its poly corners far enough off-axis to hit the GTE's
   near-plane clamp and clip. */
#define GC_WALL_RADIUS     260

/* ---- Floor zones -----------------------------------------------------------
   Five zones, one per terrace rectangle, taken straight from the FLOOR list in
   garden_courtyard_mesh_collision.c (its three y=850 entries are one contiguous
   band, so they collapse into a single zone). They do NOT overlap in XZ, so
   apply_height's "a surface above the player is not the one they stand on" rule
   never has to arbitrate and the listing order is free; they are still written
   highest-first (most negative Y first) to match every other room. */
static void gc_flat(int i, int32_t min_x, int32_t max_x,
                    int32_t min_z, int32_t max_z, int32_t y) {
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = min_x; floor_zones[i].max_x = max_x;
    floor_zones[i].min_z = min_z; floor_zones[i].max_z = max_z;
    floor_zones[i].y     = y;
}

static void garden_courtyard_floor_zones_init(void) {
    int i = 0;
    /* --- the raised perimeter walk, y=800 --- */
    gc_flat(i++, -2579,  2000, -2000, -1142, 800);   /* south strip, full width */
    gc_flat(i++, -2579, -1722, -1142,  2000, 800);   /* west strip              */
    gc_flat(i++,  1142,  2000, -1142,  2000, 800);   /* east strip (the door)   */
    /* --- the tread down to the lawn, y=850 --- */
    gc_flat(i++, -1722,  1142, -1142,  -857, 850);
    /* --- the sunken lawn, y=900 --- */
    gc_flat(i++, -1722,  1142,  -857,  2000, 900);
    floor_zone_count = i;
}

/* ---- Per-room textures -----------------------------------------------------
   This mesh's five textures are all drawn by the Garden Stairs too, from the
   very same VRAM slots, so this room owns NO texture RAM of its own: it
   captures the tpage/clut headers at startup and delegates the entry-time
   LoadImage to garden_stairs_upload_textures(). Registering a second RAM copy
   of each would have been the obvious thing and is the wrong thing — texmgr has
   a hard TEXMGR_MAX and a registration past it fails SILENTLY, breaking that
   texture everywhere (see tools/TEXTURING_NOTES.txt).

   That call also uploads xt_dr_outr, which this mesh does not use. Harmless:
   its page (clsd_drwr, x384 y0) is one this room never draws from.

   Slot numbering is the Garden Stairs' verbatim, gap and all, so the two rooms'
   tex maps stay directly comparable:

     0 brick_wall      the perimeter wall (delivery's slot, x768 y0)
     1 chnlnk          the fence panels   (delivery's gravel slot, x640 y0)
     2 xt_dr_cg        the east door      (opn_drwr page, x832 y0)
     3 xt_dr_outr      UNUSED here
     4 gravel_gs       the paved walk     (rusty_fence page, x704 y0)
     5 grss_gs         the lawn           (stn_stl page, x320 y0)

   grss_gs / gravel_gs are the byte-for-byte clones the Garden Stairs introduced
   (tools/retarget_tim.py), needed because this room fills grss's and gravel's
   own pages with brick_wall and chnlnk. All six sit at Voff 0, so the one 128
   texture window set in garden_courtyard_draw serves them all. */
#define GARDEN_COURTYARD_TEX_COUNT 6

static uint16_t tex_tpage[GARDEN_COURTYARD_TEX_COUNT];
static uint16_t tex_clut[GARDEN_COURTYARD_TEX_COUNT];

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

/* Load geometry AND capture the texture headers at STARTUP (the only time CD
   access is safe — see tools/TEXTURING_NOTES.txt). */
void garden_courtyard_load_assets(void) {
    garden_courtyard_buff = load_file_from_cd("\\TEX\\GRDNCRTY.SMD;1");
    if (garden_courtyard_buff)
        garden_courtyard_smd = smdInitData(garden_courtyard_buff);

    /* Every one of these is uploaded by another module; header only — no
       LoadImage, no second RAM copy. */
    capture_tpage("\\BRIKWLL.TIM;1",     0);
    capture_tpage("\\TEX\\CHNLNK.TIM;1", 1);
    capture_tpage("\\TEX\\XTDRCG.TIM;1", 2);
    capture_tpage("\\TEX\\GRAVELGS.TIM;1", 4);
    capture_tpage("\\TEX\\GRSSGS.TIM;1",   5);
}

/* Upload the streamed textures. Pure LoadImage — no CD access — safe during the
   room transition (the caller DrawSyncs first, as main's STATE_LOADING does).
   Identical slot set to the Garden Stairs, so its uploader does the whole job. */
void garden_courtyard_upload_textures(void) {
    garden_stairs_upload_textures();
}

/* ---- The east-wall door back to the Garden Stairs --------------------------
   The xt_dr_cg poly spans z[-1429,-857] at x=2000, on the east terrace
   (floor y=800). It is the same door leaf as the one on the Garden Stairs'
   bottom landing at x=-2140, z=1942.

   The player approaches from the -X (room) side, so the sign lies in the YZ
   plane with mirror=1. */
#define GC_DOOR_X               2000
#define GC_DOOR_Z             (-1143)   /* (-1429 + -857) / 2 */
#define GC_TEXT_Y                614    /* eye level on the east terrace */
#define GC_TEXT_RADIUS          1500
#define GC_FADE_NEAR            1000
#define GC_TRIGGER_RADIUS        500

/* East-terrace standing eye: floor y=800, less GROUND_FLOOR_Y and the 40-unit
   floor standoff apply_height applies. Stated explicitly because this room's
   floors are at POSITIVE y — arriving at the shaft's cam_y would drop the
   player in from well overhead. */
#define GC_EAST_EYE_Y  (800 - GROUND_FLOOR_Y - 40)

/* Circle edge-detect, seeded by garden_courtyard_door_arm(). Starts "held" so a
   press carried in through the transition doesn't bounce the player straight
   back. */
static int door_circle_prev = 1;

static int circle_held(void) {
    if (!pad_buff_len[0]) return 0;
    PadResponse *pad = (PadResponse *)pad_buff[0];
    return (~pad->btn & PAD_CIRCLE) ? 1 : 0;
}

void garden_courtyard_door_arm(void) {
    door_circle_prev = circle_held();
}

int garden_courtyard_door_triggered(void) {
    int held = circle_held();
    int just = held && !door_circle_prev;
    door_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - GC_DOOR_X;
    int32_t dz = cam_z - GC_DOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < GC_TRIGGER_RADIUS;
}

/* Floating "Press O to enter" sign on the east-wall door. YZ plane:
   door_draw_string_3d centres the reading axis (Z) on world_z after adding 200,
   so pass door_z - 200. Sits just west (x-11) of the wall so it floats in front
   of the door. */
static void door_text(RenderContext *ctx) {
    int32_t dx = cam_x - GC_DOOR_X;
    int32_t dz = cam_z - GC_DOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= GC_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > GC_FADE_NEAR) {
        int range = GC_TEXT_RADIUS - GC_FADE_NEAR;
        int prog  = xz - GC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        GC_DOOR_X - 11, GC_TEXT_Y, GC_DOOR_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* Arriving from the Garden Stairs: stand on the east terrace west of the x=2000
   wall, clear of this room's GC_WALL_RADIUS push (so the player isn't shoved on
   their first frame), facing -X — the direction of travel through the door,
   looking out across the garden. */
void garden_courtyard_spawn_east(void) {
    cam_x   = GC_DOOR_X - GC_WALL_RADIUS - 25;
    cam_y   = GC_EAST_EYE_Y;
    cam_vy  = 0;
    cam_z   = GC_DOOR_Z;
    cam_rot = 3072;   /* facing -X, into the room */
    garden_courtyard_door_arm();
}

void garden_courtyard_init(void) {
    garden_courtyard_collision_init(&current_collision_room);
    /* The drawn perimeter tops out at y=-800, far above the proxy walls' y=114;
       state the DRAWN value so ceiling-mounted enemies hang at the height the
       player actually sees (see tools/ADDING_A_ROOM.txt). */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(GC_WALL_RADIUS);

    /* >>> THE RETAINING LIPS MUST NOT BLOCK SHOTS. <<<
       Four of this room's ten walls are the 100-unit terrace steps down to the
       sunken lawn (the y 800..900 entries). They are real obstacles to the
       PLAYER — that one-way lip is the boss arena's boundary — but they are
       ankle-high, and this room's collision is flagged multi_level = 0, so the
       hitscan's Y gate never runs and they were blocking everything fired
       across them at any height. Standing on either terrace, the player could
       not shoot the boss and the boss's fireballs died on the step: the fight
       only worked from the middle of the lawn, and nothing said why.

       Stated as a HEIGHT RULE rather than a list of wall indices, because the
       wall list is generated and the indices move whenever the proxy mesh is
       re-exported. 150 cleanly separates the four 100-tall steps from the six
       perimeter walls, which are 686 and 786. */
    collision_shoot_over_short_walls(150);

    garden_courtyard_floor_zones_init();

    /* Only one way in, so no per-door override is needed in main.c. */
    garden_courtyard_spawn_east();

    /* Save points and dresser props are global (not room-swapped) and neither is
       area-gated in its collide routine, so reception's instances would block
       the player invisibly if they fell inside this room's bounds — and this
       room's bounds are wide enough to contain both. Clearing is safe:
       reception_init() re-places them on every reception entry. No save point of
       this room's own; the nearest is on the Garden Stairs' top landing. */
    save_points_clear();
    dressers_clear();

    /* Arm the boss encounter. It only PARKS the director — the reveal starts
       itself on the first update that finds a living Rabisu, because the boss
       is not placed until world_enter runs, which is after this. */
    rabisu_boss_enter();
}

static void draw_garden_courtyard_smd(RenderContext *ctx) {
    if (!garden_courtyard_smd) return;

    uint8_t *p = (uint8_t *)garden_courtyard_smd->p_prims;
    int i;

    for (i = 0; i < garden_courtyard_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &garden_courtyard_smd->p_verts[vi[0]];
        SVECTOR *v1 = &garden_courtyard_smd->p_verts[vi[1]];
        SVECTOR *v2 = &garden_courtyard_smd->p_verts[vi[2]];

        {
            int32_t dx = (int32_t)v0->vx - cam_x;
            int32_t dz = (int32_t)v0->vz - cam_z;
            /* Distance cull (Manhattan) at the fog-out distance so culled polys
               are already invisible — see the view-distance note above. */
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > GC_CULL_DIST)
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
           build time in garden_courtyard_nocull — same scheme as the other rooms. */
        int nocull = (i < GARDEN_COURTYARD_PRIM_COUNT) && garden_courtyard_nocull[i];
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
            v3 = &garden_courtyard_smd->p_verts[vi[3]];
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
        int32_t fog = dist < GC_FOG_NEAR ? GC_FOG_NEAR : (dist > GC_FOG_FAR ? GC_FOG_FAR : dist);
        int32_t fog_factor = ((GC_FOG_FAR - fog) << 8) / (GC_FOG_FAR - GC_FOG_NEAR);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in garden_courtyard_draw. */
        uint8_t tex_idx = (i < GARDEN_COURTYARD_PRIM_COUNT) ? garden_courtyard_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < GARDEN_COURTYARD_TEX_COUNT);
        /* Purple fog, the delivery area's SKY_FOG_* rather than the brown murk
           the interior rooms use — this is the outdoors, and it is the same
           night sky the Garden Stairs looks out on. */
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8);

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

void garden_courtyard_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = GC_FOG_NEAR; g_fog_far = GC_FOG_FAR;

    /* Background in the SAME colour the fog saturates to, so a poly that has
       faded out is indistinguishable from the void behind it and the cull never
       shows a seam. Purple here, matching the Garden Stairs and delivery. */
    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setXY0(bg, 0, 0);
    setWH(bg, SCREEN_XRES, SCREEN_YRES);
    setRGB0(bg, SKY_FOG_R, SKY_FOG_G, SKY_FOG_B);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
    ctx->next_packet += sizeof(TILE);

    /* 128x128 texture window so per-poly UVs wrap (tile) within each texture's
       page. All of this room's textures sit at page-top (Voff 0), so one window
       serves them (see tools/VRAM_MAP.txt). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    /* View matrix via camera_build_view rather than the hand-rolled yaw-only
       block most rooms still use. This room NEEDS the pitch: the boss reveal
       cranes the camera 400 up and tilts it down onto the middle of the garden
       (src/rabisu_boss.c), and a yaw-only matrix drops cam_pitch on the floor
       without a word — the shot would just be a high camera staring level at
       the far wall. With pitch 0, which is every other frame the player spends
       in here, this builds exactly the same matrix the old block did. */
    MATRIX rot_matrix;
    camera_build_view(&rot_matrix);
    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    draw_garden_courtyard_smd(ctx);

    /* The Rabisu (this room's boss) is the only enemy placed here, and it is a
       MODEL — untextured, so it owns no VRAM and needs no window bracket. The
       room's 128 window is still handed to the sprite renderers so a future
       SPRITE spawn brackets its Voff>=128 quad correctly (see
       tools/TEXTURING_NOTES.txt PART 5). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
        spiders_set_texwindow(&tw);
    }
    draw_zombies(ctx);
    draw_spiders(ctx);
    draw_rabisus(ctx);
    /* Both of these want the PLAIN camera view matrix, which draw_rabisus puts
       back after composing the boss's own — so they must come after it, not
       before. The fireballs are the boss's projectiles; the encounter draw is
       the lawn lights and the death glow. */
    rbs_fireballs_draw(ctx);
    rabisu_boss_draw(ctx);
    webs_draw(ctx);
    item_pickups_draw(ctx);
    sml_meds_draw(ctx);

    /* No door prompt while the encounter has the gate sealed: offering "Press O
       to enter" on a door that will not answer is worse than offering nothing. */
    if (!rabisu_boss_seals_door()) door_text(ctx);

    /* Screen-space, so last: the two lines of scripture over the reveal. */
    rabisu_boss_draw_overlay(ctx);
}
