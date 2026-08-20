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
#include "fountain_square.h"
#include "collision.h"
#include "fountain_square_mesh_collision.h"
#include "fountain_square_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "garden_courtyard.h"   /* garden_courtyard_upload_textures */
#include "save_point.h"
#include "zombie.h"
#include "spider.h"
#include "mushroom.h"
#include "web.h"
#include "item_pickup.h"
#include "sml_med.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Fountain Square: the hedge-walled parterre north of the Garden Courtyard. See
   fountain_square.h for the layout. */

static SMD  *fountain_square_smd  = NULL;
static void *fountain_square_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   Cull and fog-far are equal, the invariant that makes culling invisible —
   nothing is dropped until the fog has already faded it into the background.

   Halfway between RECEPTION's 350/1500 and the rest of the garden's 800/3500,
   and that is deliberate: this square should close in more than the courtyard
   next door without shutting down like a room indoors. The courtyard and the
   Garden Stairs keep their 800/3500 — walking through the gate is a step into
   a tighter space, and the two rooms are supposed to differ.

   The COLOUR is still the garden's purple SKY_FOG_*, not Reception's near-black
   interior murk. Only the distances were moved. So the square reads as a hedge
   maze in heavy night air: from the fountain in the middle the perimeter at
   ~1900 Manhattan is deep in the fog but still legible as a boundary, and the
   parterre blocks resolve as the player walks the paths.

   It is also a win on the packet buffer — this mesh is 802 prims, half again
   the courtyard's, and 2500 keeps a good deal more of it culled than 3500. */
#define FS_CULL_DIST      2500
#define FS_FOG_NEAR        575
#define FS_FOG_FAR        2500

/* ---- Floor zones -----------------------------------------------------------
   ONE zone, and deliberately one. The collision generator found twenty floor
   planes, but seventeen of those are the TOPS of things the player cannot stand
   on: thirteen 200-tall parterre blocks and the four faces of the 149-tall
   fountain rim. Their side walls are in the wall list and block at exactly
   those heights, so the player is kept off them by collision rather than by
   floor zones — giving them zones of their own would let anyone who clipped a
   corner climb onto the hedge.

   What is left is the paving, which is flat at y=0 across the whole footprint
   including all four gate alcoves. One rect over the collision bounds covers
   it exactly. */
static void fountain_square_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -1994; floor_zones[0].max_x = 2005;
    floor_zones[0].min_z = -1822; floor_zones[0].max_z = 2177;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   Four of this mesh's six textures are drawn by the Garden Courtyard too, from
   the very same VRAM slots, so for those the room owns NO texture RAM: it
   captures the tpage/clut headers at startup and delegates the entry-time
   LoadImage to garden_courtyard_upload_textures(). Registering a second RAM
   copy of each would have been the obvious thing and is the wrong thing —
   texmgr has a hard TEXMGR_MAX and a registration past it fails SILENTLY,
   breaking that texture everywhere (see tools/TEXTURING_NOTES.txt).

   That call is itself a delegation: it runs garden_stairs_upload_textures()
   first (brick_wall, chnlnk, xt_dr_cg, xt_dr_outr, grss_gs, gravel_gs) and then
   puts hedge and grdn_gte up. Two of the slots it fills are the two this room
   then overwrites — see fountain_square_upload_textures.

     0 hedge      the perimeter and the parterre  (clsd_drwr page, x384 y0)
     1 grdn_gte   the four gates                  (kchn_wl page,   x512 y0)
     2 gravel_gs  the paving                      (rusty_fence page, x704 y0)
     3 grss_gs    the lawn inside the parterre    (stn_stl page,   x320 y0)
     4 fountain   the basin in the middle         (brick_wall page, x768 y0) OWNED
     5 drain      the channel across the paving   (opn_drwr page,  x832 y0) OWNED

   grss_gs / gravel_gs are the byte-for-byte clones the Garden Stairs introduced
   (tools/retarget_tim.py); this room inherits them from the courtyard rather
   than resolving grss/gravel_texture to their own pages, so that the whole
   garden chain uses one set of slots. fountain and drain then took the only two
   full 8bpp pages this room draws nothing from — brick_wall's and opn_drwr's.
   Both were already time-shared, so neither adds a restore obligation: the
   Garden Stairs uploader puts brick_wall and xt_dr_cg back on the way out
   through the courtyard, delivery/conservatory/kitchen re-upload their own, and
   the trick drawers re-upload opn_drwr on 2F hall entry.

   All six sit at Voff 0, so the one 128 texture window set in
   fountain_square_draw serves them all. */
#define FOUNTAIN_SQUARE_TEX_COUNT 6

static uint16_t tex_tpage[FOUNTAIN_SQUARE_TEX_COUNT];
static uint16_t tex_clut[FOUNTAIN_SQUARE_TEX_COUNT];

/* The two textures this room OWNS: RAM-resident from startup so the entry-time
   upload is a pure LoadImage with no CD read. Keep this table in step with the
   slot numbering above and with NAME_TO_SLOT in gen_fountain_square_tex_map.py. */
#define FOUNTAIN_SQUARE_NEW_TEX 2
static int new_tex_id[FOUNTAIN_SQUARE_NEW_TEX];
static const char *new_tex_file[FOUNTAIN_SQUARE_NEW_TEX] = {
    "\\TEX\\FOUNTAIN.TIM;1",   /* slot 4 */
    "\\TEX\\DRAIN.TIM;1",      /* slot 5 */
};

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale. */
void fountain_square_load_geometry(void) {
    fountain_square_buff = room_arena_load("\\TEX\\FNTNSQ.SMD;1");
    fountain_square_smd  = fountain_square_buff ? smdInitData(fountain_square_buff) : NULL;
}

/* Read at STARTUP — the only safe time for CD access — the two textures this
   room owns. Geometry moved to fountain_square_load_geometry above, and the
   other four slots are compile-time constants that cost nothing here. */
void fountain_square_load_assets(void) {
    /* Every one of these is uploaded by another module; header only — no
       LoadImage, no second RAM copy. */
    TIM_SLOT(0, HEDGE);
    TIM_SLOT(1, GRDNGTE);
    TIM_SLOT(2, GRAVELGS);
    TIM_SLOT(3, GRSSGS);

    /* This room's own pair: RAM-resident copies for a CD-free re-upload. */
    for (int i = 0; i < FOUNTAIN_SQUARE_NEW_TEX; i++)
        new_tex_id[i] = texmgr_register(new_tex_file[i]);
    TIM_SLOT(4, FOUNTAIN);
    TIM_SLOT(5, DRAIN);
}

/* Upload the streamed textures. Pure LoadImage — no CD access — safe during the
   room transition (the caller DrawSyncs first, as main's STATE_LOADING does).
   Slots 0-3 are the Garden Courtyard's set exactly, so its uploader does that
   job. ORDER MATTERS, and doubly so here: that call puts brick_wall on x768 y0
   and xt_dr_cg on x832 y0, which are where fountain and drain live — so this
   room's own pair must go up AFTER it, not before. */
void fountain_square_upload_textures(void) {
    garden_courtyard_upload_textures();
    for (int i = 0; i < FOUNTAIN_SQUARE_NEW_TEX; i++)
        texmgr_upload(new_tex_id[i]);
}

/* ---- The south-wall gate back to the Garden Courtyard -----------------------
   The grdn_gte polys on this side span x[-364,364] at z=-1822, y[-600,0]. It is
   the same gate leaf as the one in the Garden Courtyard's north hedge, which is
   728 units narrower there — the two rooms' meshes are independent, and only
   this pairing links them.

   Collision wall 79 runs the full width of the south side at z=-1822 with
   nz = +4096, so the walkable side is +Z and the player approaches from inside
   the room. The sign therefore lies in the XY plane with mirror=1. */
#define FS_GATE_X                  0    /* (-364 + 364) / 2 */
#define FS_GATE_Z            (-1822)
#define FS_TEXT_Y             (-186)    /* eye level on the y=0 paving         */
#define FS_TEXT_RADIUS         1500
#define FS_FADE_NEAR           1000
#define FS_TRIGGER_RADIUS       500

/* Standing eye on the paving: floor y=0, less GROUND_FLOOR_Y and the 40-unit
   floor standoff apply_height applies. Stated explicitly because the room the
   player arrives from has its floors at POSITIVE y — carrying the courtyard's
   cam_y in would drop them well under the paving. */
#define FS_EYE_Y  (0 - GROUND_FLOOR_Y - 40)

/* Circle edge-detect, seeded by fountain_square_gate_arm(). Starts "held" so a
   press carried in through the transition doesn't bounce the player straight
   back. */
static int gate_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void fountain_square_gate_arm(void) {
    gate_circle_prev = circle_held();
}

int fountain_square_gate_triggered(void) {
    int held = circle_held();
    int just = held && !gate_circle_prev;
    gate_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - FS_GATE_X;
    int32_t dz = cam_z - FS_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < FS_TRIGGER_RADIUS && interact_facing(FS_GATE_X, FS_GATE_Z);
}

/* Floating "Press O to enter" sign on the south gate. XY plane:
   door_draw_string_3d centres the reading axis (X) on world_x after adding 200,
   so pass door_x - 200. Sits just north (z+11) of the wall so it floats in front
   of the gate. */
static void gate_text(RenderContext *ctx) {
    int32_t dx = cam_x - FS_GATE_X;
    int32_t dz = cam_z - FS_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= FS_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > FS_FADE_NEAR) {
        int range = FS_TEXT_RADIUS - FS_FADE_NEAR;
        int prog  = xz - FS_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        FS_GATE_X - 200, FS_TEXT_Y, FS_GATE_Z + 11,
                        50, 255, 50, fade, 1, TEXT_PLANE_XY, DOOR_PIXEL_SIZE);
}

/* Arriving from the Garden Courtyard: stand on the paving north of the z=-1822
   hedge, clear of the wall push radius (so the player isn't shoved on their
   first frame), facing +Z — the direction of travel through the gate, looking
   up the central path at the fountain. */
void fountain_square_spawn_south(void) {
    cam_x   = FS_GATE_X;
    cam_y   = FS_EYE_Y;
    cam_vy  = 0;
    cam_z   = FS_GATE_Z + COLLISION_WALL_RADIUS + 25;
    cam_rot = 0;    /* facing +Z, into the room */
    fountain_square_gate_arm();
}

/* ---- The north-wall gate on to the Outside Catacombs -----------------------
   The second of this room's four modelled gates to be connected. Its grdn_gte
   polys span x[-364,364] at z=2177, y[-600,0] — the mirror of the south gate
   above, and it opens on the Outside Catacombs' south gate at that room's
   z=-2000.

   Collision wall 66 runs the full width of the north side at z=2177 with
   nz = -4096, so the walkable side is -Z: the player approaches from inside the
   room, which is the OPPOSITE face from the south gate. The sign is therefore
   still in the XY plane but with mirror=0, and it stands 11 units SOUTH of the
   wall (z - 11) rather than north of it. Getting either of those backwards
   comes out as mirrored text or a sign inside the hedge. */
#define FS_NGATE_X                 0    /* (-364 + 364) / 2 */
#define FS_NGATE_Z            2177

/* Its own Circle edge-detect. Two gates in one room means two independent edge
   states — sharing one would let a press consumed by the near gate re-arm the
   far one — and both are seeded on every entry by fountain_square_init. */
static int ngate_circle_prev = 1;

void fountain_square_ngate_arm(void) {
    ngate_circle_prev = circle_held();
}

int fountain_square_ngate_triggered(void) {
    int held = circle_held();
    int just = held && !ngate_circle_prev;
    ngate_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - FS_NGATE_X;
    int32_t dz = cam_z - FS_NGATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < FS_TRIGGER_RADIUS && interact_facing(FS_NGATE_X, FS_NGATE_Z);
}

/* Floating sign on the north gate. Same radii and fade as the south one; only
   the mirror flag and the sign's side of the wall differ (see above). */
static void ngate_text(RenderContext *ctx) {
    int32_t dx = cam_x - FS_NGATE_X;
    int32_t dz = cam_z - FS_NGATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= FS_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > FS_FADE_NEAR) {
        int range = FS_TEXT_RADIUS - FS_FADE_NEAR;
        int prog  = xz - FS_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        FS_NGATE_X - 200, FS_TEXT_Y, FS_NGATE_Z - 11,
                        50, 255, 50, fade, 0, TEXT_PLANE_XY, DOOR_PIXEL_SIZE);
}

/* Arriving back from the Outside Catacombs: stand on the paving SOUTH of the
   z=2177 hedge, clear of the wall push radius, facing -Z — the direction of
   travel through the gate, looking back down the central path at the fountain. */
void fountain_square_spawn_north(void) {
    cam_x   = FS_NGATE_X;
    cam_y   = FS_EYE_Y;
    cam_vy  = 0;
    cam_z   = FS_NGATE_Z - COLLISION_WALL_RADIUS - 25;
    cam_rot = 2048;   /* facing -Z, into the room */
    /* Arm BOTH gates, not just this one: a Circle held through the transition
       would otherwise fire whichever interaction was left unarmed. */
    fountain_square_gate_arm();
    fountain_square_ngate_arm();
}

void fountain_square_init(void) {
    fountain_square_collision_init(&current_collision_room);
    /* The perimeter hedge is drawn to y=-500 (the gates reach -600, but they
       are the openings, not the roofline); state the DRAWN value so anything
       ceiling-mounted hangs at the height the player actually sees. */
    collision_set_ceiling_y(-500);
    /* No collision_set_wall_radius: the default 195 is right here, and main.c
       resets to it before every room init. The Garden Courtyard needs 260 for
       its 1700-tall brick perimeter, which would reach the GTE's near-plane
       clamp at any less; this room's tallest face is a 500 hedge, and 260 would
       cost far more than it bought — the parterre paths are 728 wide, so it
       would leave 208 units of walkable corridor instead of 338. */

    fountain_square_floor_zones_init();

    /* Default arrival is the south gate; main.c's STATE_LOADING branch
       overrides it with the north spawn when the player is coming back out of
       the Outside Catacombs. Either way both gates end up armed — the south
       spawn calls its own arm and the extra one is added here. */
    fountain_square_spawn_south();
    fountain_square_ngate_arm();

    /* Save points and dresser props are global (not room-swapped) and neither is
       area-gated in its collide routine, so reception's instances would block
       the player invisibly if they fell inside this room's bounds — and this
       room's bounds are wide enough to contain both. Clearing is safe:
       reception_init() re-places them on every reception entry. No save point of
       this room's own; the nearest is on the Garden Stairs' top landing. */
    save_points_clear();
    dressers_clear();
}

static void draw_fountain_square_smd(RenderContext *ctx) {
    if (!fountain_square_smd) return;

    uint8_t *p = (uint8_t *)fountain_square_smd->p_prims;
    int i;

    for (i = 0; i < fountain_square_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &fountain_square_smd->p_verts[vi[0]];
        SVECTOR *v1 = &fountain_square_smd->p_verts[vi[1]];
        SVECTOR *v2 = &fountain_square_smd->p_verts[vi[2]];

        {
            int32_t dx = (int32_t)v0->vx - cam_x;
            int32_t dz = (int32_t)v0->vz - cam_z;
            /* Distance cull (Manhattan) at the fog-out distance so culled polys
               are already invisible — see the view-distance note above. */
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > FS_CULL_DIST)
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
           build time in fountain_square_nocull — same scheme as the other rooms. */
        int nocull = (i < FOUNTAIN_SQUARE_PRIM_COUNT) && fountain_square_nocull[i];
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
            v3 = &fountain_square_smd->p_verts[vi[3]];
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
        int32_t fog = dist < FS_FOG_NEAR ? FS_FOG_NEAR : (dist > FS_FOG_FAR ? FS_FOG_FAR : dist);
        int32_t fog_factor = ((FS_FOG_FAR - fog) << 8) / (FS_FOG_FAR - FS_FOG_NEAR);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in fountain_square_draw. */
        uint8_t tex_idx = (i < FOUNTAIN_SQUARE_PRIM_COUNT) ? fountain_square_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < FOUNTAIN_SQUARE_TEX_COUNT);
        /* Purple fog, the same night sky the Garden Courtyard and the Garden
           Stairs look out on — this is one continuous outdoors. Only the
           DISTANCES are pulled in, halfway toward Reception's; the colour
           stays the garden's (see the view-distance note above). */
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

void fountain_square_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = FS_FOG_NEAR; g_fog_far = FS_FOG_FAR;

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

    draw_fountain_square_smd(ctx);

    /* Nothing is placed in this room yet (world.c seeds it empty), but the
       renderers are wired up and handed the window so that a future SPRITE
       spawn brackets its Voff>=128 quad correctly rather than sampling this
       room's hedge (see tools/TEXTURING_NOTES.txt PART 5).

       The MUSHROOM HEAD is wired in for the same reason and one more: this is
       the only room besides the Outside Catacombs on SND_BANK_GARDEN, so it is
       the only other room a mushroom could be dropped into and still scream
       (SFX_HISS does not fit the house bank — see src/sound.h). Its sprites sit
       at Voff 128 too, so the same window serves them. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
        spiders_set_texwindow(&tw);
        mushrooms_set_texwindow(&tw);
    }
    draw_zombies(ctx);
    draw_spiders(ctx);
    draw_mushrooms(ctx);
    webs_draw(ctx);
    item_pickups_draw(ctx);
    sml_meds_draw(ctx);

    /* Last: the gate signs. */
    gate_text(ctx);
    ngate_text(ctx);
}
