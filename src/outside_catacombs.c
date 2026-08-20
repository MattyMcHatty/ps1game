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
#include "outside_catacombs.h"
#include "collision.h"
#include "outside_catacombs_mesh_collision.h"
#include "outside_catacombs_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "garden_courtyard.h"   /* garden_courtyard_upload_textures */
#include "conservatory.h"       /* conservatory_upload_con_tile     */
#include "save_point.h"
#include "zombie.h"
#include "spider.h"
#include "rafflesia.h"
#include "mushroom.h"
#include "web.h"
#include "item_pickup.h"
#include "sml_med.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Outside Catacombs: the approach to the catacomb mouth, north of Fountain
   Square. See outside_catacombs.h for the layout. */

static SMD  *outside_catacombs_smd  = NULL;
static void *outside_catacombs_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   Cull and fog-far are equal, the invariant that makes culling invisible —
   nothing is dropped until the fog has already faded it into the background.

   These are Fountain Square's numbers exactly, and deliberately so: the gate
   between the two rooms is a step from one walled garden into the next, not a
   change of weather, and the purple SKY_FOG_* colour is already shared by the
   whole garden chain. Only note what it means HERE: this footprint is about
   8800 x 6600, twice the square's each way, so from the gate the catacomb
   facade 6600 units north is far beyond the fog and resolves as the player
   walks up the avenue. That is the intent — the room reveals itself — but it is
   worth knowing that a fog-far matched to the neighbour, not to the mesh, is
   why nothing distant is legible from the entrance.

   It is also a win on the packet buffer: 924 prims is the largest mesh in the
   garden, and 2500 keeps a good deal more of it culled than 3500 would. */
#define OC_CULL_DIST      2500
#define OC_FOG_NEAR        575
#define OC_FOG_FAR        2500

/* Wall standoff, wider than the 195 the interior rooms use, for the same reason
   the Garden Courtyard and the Garden Stairs need it: the perimeter hedge is
   drawn 1100 tall (the catacomb facade 1141), so standing close to it puts its
   poly corners far enough off-axis to hit the GTE's near-plane clamp and clip.
   Fountain Square keeps the default because its hedge is only 500 tall.
   It costs nothing here — the narrowest walkable run in this room is the 1500-
   wide avenue, which 260 still leaves 980 units of. */
#define OC_WALL_RADIUS     260

/* ---- Floor zones -----------------------------------------------------------
   ONE zone, and it really is one: the collision generator found sixteen floor
   planes and every single one is at y=0. They are the avenue, the side lawns
   and the pockets between the hedge masses, tiled edge to edge, so a rect over
   the full footprint covers them exactly with nothing left to fall through.

   Note the Z: the collision bounds stop at 4353 but floor plane 13 (and the
   drawn ground) reach 4637, so the zone is taken to 4637 rather than to the
   collision bound. A floor-zone rect that misses part of the room is the way
   players fall through the world (tools/ADDING_A_ROOM.txt STEP 8). */
static void outside_catacombs_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -4396; floor_zones[0].max_x = 4399;
    floor_zones[0].min_z = -2000; floor_zones[0].max_z = 4637;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   Seven mesh textures, and five of them are already put in VRAM by rooms this
   one is reached through, so for those the room owns NO texture RAM: it takes
   the tpage/clut headers as compile-time constants and delegates the entry-time
   LoadImage to the module that owns the RAM copy. Registering a second copy of
   each would have been the obvious thing and is the wrong thing — texmgr has a
   hard TEXMGR_MAX of 48, 46 of which were spent before this room, and a
   registration past it fails SILENTLY, breaking that texture everywhere (see
   tools/TEXTURING_NOTES.txt). The two left are exactly the two this room owns.

     0 hedge          the perimeter and the hedge masses (clsd_drwr page, x384 y0)
     1 grdn_gte       the south gate                     (kchn_wl page,   x512 y0)
     2 gravel_gs      the paths                          (rusty_fence page, x704 y0)
     3 grss_gs        the lawns                          (stn_stl page,   x320 y0)
     4 con_tile       the catacomb facade                (opn_drwr page,  x832 y0)
     5 lamashtu tablet the tablet in the facade          (brick_wall page, x768 y0) OWNED
     6 poison_flower_base the flower bed on the ground   (trck_clue page, x640 y0) OWNED

   Slots 0-3 are the Garden Courtyard's set exactly, inherited by way of that
   room's uploader (which itself runs garden_stairs_upload_textures first). As
   in Fountain Square, the mesh's 'grss' and 'gravel_texture' materials resolve
   to the grss_gs / gravel_gs CLONES rather than to their own pages, so the whole
   garden chain draws from one set of slots — see gen_outside_catacombs_tex_map.py.

   Slot 4 is the conservatory's con_tile, taken through its narrow
   conservatory_upload_con_tile() rather than the whole conservatory set (which
   would stamp over three slots this room needs).

   The two OWNED textures then took the only two full 8bpp pages this room draws
   nothing from, both already time-shared, so neither adds a restore obligation:
     lamashtu tablet    -> the brick_wall page (x768 y0), shared with grss /
                           bed / fountain / xt_dr_lckd. The Garden Stairs
                           uploader puts brick_wall here on the way in, so the
                           tablet must go up AFTER it — see the upload below.
     poison_flower_base -> the trck_clue page (x640 y0), shared with
                           gravel_texture / trees / chnlnk. Same ordering rule:
                           the Garden Stairs uploader puts chnlnk there.
   Everyone who needs the originals re-uploads on their own entry (delivery
   restores gravel_texture and trees, the east stairwell and garden stairs
   chnlnk, the attic stairwell trck_clue, master bedroom bed, attic exit
   xt_dr_lckd, Fountain Square fountain).

   All seven sit at Voff 0, so the one 128 texture window set in
   outside_catacombs_draw serves them all. */
#define OUTSIDE_CATACOMBS_TEX_COUNT 7

static uint16_t tex_tpage[OUTSIDE_CATACOMBS_TEX_COUNT];
static uint16_t tex_clut[OUTSIDE_CATACOMBS_TEX_COUNT];

/* The two textures this room OWNS: RAM-resident from startup so the entry-time
   upload is a pure LoadImage with no CD read. Keep this table in step with the
   slot numbering above and with NAME_TO_SLOT in
   gen_outside_catacombs_tex_map.py. */
#define OUTSIDE_CATACOMBS_NEW_TEX 2
static int new_tex_id[OUTSIDE_CATACOMBS_NEW_TEX];
static const char *new_tex_file[OUTSIDE_CATACOMBS_NEW_TEX] = {
    "\\TEX\\LMSHTBLT.TIM;1",   /* slot 5 */
    "\\TEX\\PSNFLWR.TIM;1",    /* slot 6 */
};

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale. */
void outside_catacombs_load_geometry(void) {
    outside_catacombs_buff = room_arena_load("\\TEX\\OUTCTCMB.SMD;1");
    outside_catacombs_smd  = outside_catacombs_buff
                             ? smdInitData(outside_catacombs_buff) : NULL;
}

/* Read at STARTUP — the only safe time for CD access — the two textures this
   room owns. Geometry moved to outside_catacombs_load_geometry above, and the
   other five slots are compile-time constants that cost nothing here. */
void outside_catacombs_load_assets(void) {
    /* Every one of these is uploaded by another module; header only — no
       LoadImage, no second RAM copy. */
    TIM_SLOT(0, HEDGE);
    TIM_SLOT(1, GRDNGTE);
    TIM_SLOT(2, GRAVELGS);
    TIM_SLOT(3, GRSSGS);
    TIM_SLOT(4, CONTILE);

    /* This room's own pair: RAM-resident copies for a CD-free re-upload. */
    for (int i = 0; i < OUTSIDE_CATACOMBS_NEW_TEX; i++)
        new_tex_id[i] = texmgr_register(new_tex_file[i]);
    TIM_SLOT(5, LMSHTBLT);
    TIM_SLOT(6, PSNFLWR);
}

/* Upload the streamed textures. Pure LoadImage — no CD access — safe during the
   room transition (the caller DrawSyncs first, as main's STATE_LOADING does).

   ORDER MATTERS, and on three counts. The courtyard's uploader (via the Garden
   Stairs') puts brick_wall on x768 y0, chnlnk on x640 y0 and xt_dr_cg on
   x832 y0 — which are where the tablet, the flower bed and con_tile live — so
   everything this room actually draws from those three pages must go up AFTER
   it, not before. */
void outside_catacombs_upload_textures(void) {
    garden_courtyard_upload_textures();   /* hedge, grdn_gte, grss_gs, gravel_gs */
    conservatory_upload_con_tile();       /* con_tile -> the xt_dr_cg slot       */
    for (int i = 0; i < OUTSIDE_CATACOMBS_NEW_TEX; i++)
        texmgr_upload(new_tex_id[i]);     /* tablet -> brick_wall, flowers -> chnlnk */
}

/* Just the flower bed, for a room that draws poison_flower_base but nothing else
   of this one's. Maze One is the case: it has flower beds of its own but no
   facade and no tablet, and the tablet's page (brick_wall, x768 y0) is where its
   pipe goes — so running the whole uploader above would put the tablet straight
   over it. Narrow accessor rather than a second registration of PSNFLWR.TIM,
   for the usual TEXMGR_MAX reason. Callers must still run the courtyard's
   uploader FIRST: that is what puts chnlnk on this slot. */
void outside_catacombs_upload_flowers(void) {
    texmgr_upload(new_tex_id[1]);   /* slot 6, \TEX\PSNFLWR.TIM */
}

/* ---- The south-wall gate back to Fountain Square ---------------------------
   The grdn_gte polys on this side span x[-450,450] at z=-2000, y[-600,0]. It is
   the same gate leaf as the one in Fountain Square's north hedge, which is 172
   units narrower there — the two rooms' meshes are independent, and only this
   pairing links them.

   Collision wall 17 runs across the opening at z=-2000 with nz = +4096, so the
   walkable side is +Z and the player approaches from inside the room. The sign
   therefore lies in the XY plane with mirror=1, exactly as Fountain Square's
   own south gate does. */
#define OC_GATE_X                  0    /* (-450 + 450) / 2 */
#define OC_GATE_Z            (-2000)
#define OC_TEXT_Y             (-186)    /* eye level on the y=0 ground          */
#define OC_TEXT_RADIUS         1500
#define OC_FADE_NEAR           1000
#define OC_TRIGGER_RADIUS       500

/* Standing eye on the ground: floor y=0, less GROUND_FLOOR_Y and the 40-unit
   floor standoff apply_height applies. The same expression Fountain Square uses,
   and for the same reason — both rooms' floors are at y=0 while the courtyard
   below has its lawn at positive y. */
#define OC_EYE_Y  (0 - GROUND_FLOOR_Y - 40)

/* Circle edge-detect, seeded by outside_catacombs_gate_arm(). Starts "held" so a
   press carried in through the transition doesn't bounce the player straight
   back. */
static int gate_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void outside_catacombs_gate_arm(void) {
    gate_circle_prev = circle_held();
}

int outside_catacombs_gate_triggered(void) {
    int held = circle_held();
    int just = held && !gate_circle_prev;
    gate_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - OC_GATE_X;
    int32_t dz = cam_z - OC_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < OC_TRIGGER_RADIUS && interact_facing(OC_GATE_X, OC_GATE_Z);
}

/* Floating "Press O to enter" sign on the south gate. XY plane:
   door_draw_string_3d centres the reading axis (X) on world_x after adding 200,
   so pass door_x - 200. Sits just north (z+11) of the wall so it floats in front
   of the gate. */
static void gate_text(RenderContext *ctx) {
    int32_t dx = cam_x - OC_GATE_X;
    int32_t dz = cam_z - OC_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= OC_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > OC_FADE_NEAR) {
        int range = OC_TEXT_RADIUS - OC_FADE_NEAR;
        int prog  = xz - OC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        OC_GATE_X - 200, OC_TEXT_Y, OC_GATE_Z + 11,
                        50, 255, 50, fade, 1, TEXT_PLANE_XY, DOOR_PIXEL_SIZE);
}

/* Arriving from Fountain Square: stand on the ground north of the z=-2000
   hedge, clear of the wall push radius (so the player isn't shoved on their
   first frame), facing +Z — the direction of travel through the gate, looking
   straight up the avenue at the catacomb facade. */
void outside_catacombs_spawn_south(void) {
    cam_x   = OC_GATE_X;
    cam_y   = OC_EYE_Y;
    cam_vy  = 0;
    cam_z   = OC_GATE_Z + OC_WALL_RADIUS + 25;
    cam_rot = 0;    /* facing +Z, into the room */
    outside_catacombs_gate_arm();
}

void outside_catacombs_init(void) {
    outside_catacombs_collision_init(&current_collision_room);
    /* The perimeter hedge is drawn to y=-1100 (the gate reaches only -600, but
       that is the opening, not the roofline); state the DRAWN value so anything
       ceiling-mounted hangs at the height the player actually sees. The
       collision proxies stop at -500, which is why this cannot be read off the
       collision data. */
    collision_set_ceiling_y(-1100);
    collision_set_wall_radius(OC_WALL_RADIUS);

    outside_catacombs_floor_zones_init();

    /* Only one gate is connected, so no per-door override is needed in main.c. */
    outside_catacombs_spawn_south();

    /* Save points and dresser props are global (not room-swapped) and neither is
       area-gated in its collide routine, so reception's instances would block
       the player invisibly if they fell inside this room's bounds — and this
       room's bounds, the largest in the game, certainly contain both. Clearing
       is safe: reception_init() re-places them on every reception entry. No save
       point of this room's own; the nearest is on the Garden Stairs' top
       landing. */
    save_points_clear();
    dressers_clear();
}

static void draw_outside_catacombs_smd(RenderContext *ctx) {
    if (!outside_catacombs_smd) return;

    uint8_t *p = (uint8_t *)outside_catacombs_smd->p_prims;
    int i;

    for (i = 0; i < outside_catacombs_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &outside_catacombs_smd->p_verts[vi[0]];
        SVECTOR *v1 = &outside_catacombs_smd->p_verts[vi[1]];
        SVECTOR *v2 = &outside_catacombs_smd->p_verts[vi[2]];

        {
            int32_t dx = (int32_t)v0->vx - cam_x;
            int32_t dz = (int32_t)v0->vz - cam_z;
            /* Distance cull (Manhattan) at the fog-out distance so culled polys
               are already invisible — see the view-distance note above. */
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > OC_CULL_DIST)
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
           build time in outside_catacombs_nocull — same scheme as the other
           rooms. */
        int nocull = (i < OUTSIDE_CATACOMBS_PRIM_COUNT) && outside_catacombs_nocull[i];
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
            v3 = &outside_catacombs_smd->p_verts[vi[3]];
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
        int32_t fog = dist < OC_FOG_NEAR ? OC_FOG_NEAR : (dist > OC_FOG_FAR ? OC_FOG_FAR : dist);
        int32_t fog_factor = ((OC_FOG_FAR - fog) << 8) / (OC_FOG_FAR - OC_FOG_NEAR);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in outside_catacombs_draw. */
        uint8_t tex_idx = (i < OUTSIDE_CATACOMBS_PRIM_COUNT) ? outside_catacombs_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < OUTSIDE_CATACOMBS_TEX_COUNT);
        /* Purple fog, the same night sky the rest of the garden looks out on —
           this is one continuous outdoors, and the near/far are Fountain
           Square's exactly (see the view-distance note above). */
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

void outside_catacombs_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = OC_FOG_NEAR; g_fog_far = OC_FOG_FAR;

    /* Background in the SAME colour the fog saturates to, so a poly that has
       faded out is indistinguishable from the void behind it and the cull never
       shows a seam. Purple here, matching the rest of the garden. */
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
       block the older rooms use — same matrix at pitch 0, and it does not drop
       cam_pitch on the floor should anything here ever tilt the camera. */
    MATRIX rot_matrix;
    camera_build_view(&rot_matrix);
    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    draw_outside_catacombs_smd(ctx);

    /* The zombies and spiders are still seeded empty here (world.c, for the same
       sound-bank reason as Fountain Square) but keep their renderers wired up so
       a future spawn brackets its Voff>=128 quad correctly rather than sampling
       this room's hedge (see tools/TEXTURING_NOTES.txt PART 5). The RAFFLESIAS
       are real: three of them, one on each poison_flower_base bed, and their
       sprites sit at Voff 128 too — so the same window goes to them. So does
       the MUSHROOM HEAD pacing the southern chamber: its four 96x96 sprites sit
       at Voff 128 as well (VRAM y=384, see mushroom.h). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
        spiders_set_texwindow(&tw);
        rafflesias_set_texwindow(&tw);
        mushrooms_set_texwindow(&tw);
    }
    draw_zombies(ctx);
    draw_spiders(ctx);
    draw_rafflesias(ctx);
    draw_mushrooms(ctx);
    webs_draw(ctx);
    item_pickups_draw(ctx);
    sml_meds_draw(ctx);

    /* Last: the gate sign. */
    gate_text(ctx);
}
