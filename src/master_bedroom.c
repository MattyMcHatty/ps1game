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
#include "cull_arena.h"
#include "tim_slots.h"
#include "camera.h"
#include "master_bedroom.h"
#include "collision.h"
#include "master_bedroom_mesh_collision.h"
#include "master_bedroom_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "kitchen_dining.h"
#include "dresser.h"
#include "save_point.h"
#include "zombie.h"
#include "spider.h"
#include "rabisu.h"
#include "web.h"
#include "item_pickup.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Master Bedroom: an H-shaped suite off the 2F hall corridor, rendered the same
   way as the conservatory / 2F hall (per-poly tex map + 128 texture window +
   fog). Two doors in its north wall lead back up to the corridor. */

static SMD  *master_bedroom_smd  = NULL;
static void *master_bedroom_buff = NULL;

/* Single flat floor at y=0 across the walkable bounds (all seven floors detected
   in master_bedroom_mesh_collision.c sit at y=0). */
static void master_bedroom_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -1594; floor_zones[0].max_x = 1245;
    floor_zones[0].min_z = -441;  floor_zones[0].max_z = 319;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   Eight mesh textures. FIVE are resident from startup (wd_flr/red_wlppr/din_cl/
   stn_gls with the kitchen, wd_dr with the fat door). THREE are uploaded on
   every bedroom entry:
     - red_crpt time-shares the frnt_dr slot (reception streams frnt_dr over it;
       the 2F hall streams red_crpt back, but a title-screen load straight into
       this room guarantees nothing, so we upload our own copy);
     - dresser time-shares the kchn_wl slot — the same upload reception does for
       the dresser PROP, reused here for the dressers modelled into the mesh
       (restored by kitchen_restore_textures on kitchen entry);
     - bed is this room's only new texture. It time-shares the DELIVERY-only
       brick_wall slot (x768 y0), which the conservatory already streams grss
       over — so it adds no new restore obligation: delivery_restore_textures()
       puts brick_wall back on delivery entry and conservatory_upload_textures()
       puts grss back on conservatory entry. */
#define MASTER_BEDROOM_TEX_COUNT 8

/* Streamed slots: engine slot -> texmgr id. */
#define MASTER_BEDROOM_NEW_TEX 1
static int new_tex_id[MASTER_BEDROOM_NEW_TEX];
/* ONE registration, down from two: red_crpt is borrowed from the kitchen, which
   owns the only RAM copy and already re-uploads it for its own restore, rather
   than kept a second time here. See tools/HEAP_BUDGET.txt. */
static const struct { const char *file; int slot; } new_tex[MASTER_BEDROOM_NEW_TEX] = {
    { "\\TEX\\BED.TIM;1",     7 },
};

static uint16_t tex_tpage[MASTER_BEDROOM_TEX_COUNT];
static uint16_t tex_clut[MASTER_BEDROOM_TEX_COUNT];

/* ---- The reject path's cull keys -------------------------------------------
   See tools/DIAGNOSING_FRAME_RATE.txt STEP 3B and src/cull_arena.h for the arena
   itself. This room was written in Aug 2026 and inherited NONE of that work: the
   draw loop below walked all 622 primitives every frame and, for every one of
   them, read the primitive header for its stride, read three vertex INDICES and
   chased v0/v1/v2 into the vertex array -- all BEFORE either of the two cheap
   culls had a chance to throw the primitive away.

   Counted offline against assets/master_bedroom.smd over the walkable footprint
   (X -1594..1245, Z -441..319) at 16 headings, 960 poses -- sweep the room, do
   not sample the doorway, which is STEP 5's own lesson:

        622  primitives walked, every frame, from anywhere in the room
        384  mean reaching the second cull (i.e. the trig below)
        482  mean surviving BOTH culls -> the GTE
        542  worst, standing at (6,-41) facing -Z -- WHICH IS THE BED

   >>> THE LAST LINE IS THE PLAYER'S REPORT. <<< (6,-41) is the middle of the
   suite and rot 2048 is the heading that looks down the bed chamber. The
   distance cull is at its most useless from that stance (542 of 622 primitives
   pass it, because the room is 2840 x 1293 and the cull is 1500 Manhattan) and
   the "behind me" cull is at its most useless on that heading (542 survive
   against 437 with the bed at the player's back). The worst stance and the worst
   heading in this room are the same pose, and it is the one he named.

   A rejected primitive now costs ONE sequential 6-byte read out of cull_keys and
   never addresses the mesh at all.

   NO BOX KEY, for reception's reason (src/reception.c): cull_boxes pays for
   itself only where a SIDE-PLANE frustum test would otherwise chase v1..v3 for
   every primitive that survives the distance cull, and this room has no such
   test to feed -- nor should it get one. The Greenhouse measured a side-plane
   cull four hblanks on the WRONG side of neutral and Maze One measured it
   exactly neutral. A box here would be a table nothing reads. */
static int mb_key_count = 0;

static void mb_build_cull_keys(void) {
    mb_key_count = 0;
    if (!master_bedroom_smd) return;
    uint8_t *p = (uint8_t *)master_bedroom_smd->p_prims;
    int i, n = master_bedroom_smd->n_prims;
    if (n > MASTER_BEDROOM_PRIM_COUNT) n = MASTER_BEDROOM_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &master_bedroom_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    mb_key_count = n;
}

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale. */
void master_bedroom_load_geometry(void) {
    master_bedroom_buff = room_arena_load("\\TEX\\MSTRBED.SMD;1");
    master_bedroom_smd  = master_bedroom_buff ? smdInitData(master_bedroom_buff) : NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    mb_build_cull_keys();
}

/* Register this room's streamed textures at STARTUP. Geometry is NOT loaded
   here any more — see master_bedroom_load_geometry above — but the texmgr registrations
   still are: they keep a RAM copy so the entry-time upload is a pure LoadImage
   (tools/TEXTURING_NOTES.txt). */
void master_bedroom_load_assets(void) {
    /* Streamed slots: RAM-resident via the texture manager, uploaded on entry. */
    for (int i = 0; i < MASTER_BEDROOM_NEW_TEX; i++) {
        int slot = new_tex[i].slot;
        new_tex_id[i]   = texmgr_register(new_tex[i].file);
        tex_tpage[slot] = texmgr_tpage(new_tex_id[i]);
        tex_clut[slot]  = texmgr_clut(new_tex_id[i]);
    }

    /* Resident from startup (kitchen + fatdoor); just capture tpage/clut. */
    TIM_SLOT(0, WDFLR);
    TIM_SLOT(1, REDWLPPR);
    TIM_SLOT(5, REDCRPT);    /* the kitchen owns the copy */
    TIM_SLOT(2, DINCL);
    TIM_SLOT(3, STNGLS);
    TIM_SLOT(4, WDDR);
    /* Uploaded by the dresser prop module (dresser_upload_texture), which this
       room calls on entry; we only need its tpage/clut here. */
    TIM_SLOT(6, DRESSER);
}

/* Upload the streamed textures from their resident RAM copies. Pure LoadImage
   — no CD access — safe during the room transition (the caller DrawSyncs first,
   as main's STATE_LOADING does). */
void master_bedroom_upload_textures(void) {
    for (int i = 0; i < MASTER_BEDROOM_NEW_TEX; i++)
        texmgr_upload(new_tex_id[i]);
    kitchen_upload_red_crpt();  /* red_crpt -> frnt_dr slot, x320 y256. Different
                                   page from bed, so the order is free. */
    dresser_upload_texture();   /* dresser -> kchn_wl slot (mesh dressers here) */
}

/* ---- The two doors back up to the 2F hall corridor -------------------------
   Both sit in the room's north wall (z=315, from the wd_dr polys in
   "Master Bedroom.smx"): the west wing's at x=-808 and the east wing's at
   x=769. They map to the corridor doors at hall x=-1889 and x=-400
   respectively. The player approaches both from the -Z (room) side, so the
   signs lie in the XY plane with mirror=0 — the same orientation as the hall's
   own "descend" sign, and the opposite of the conservatory's ascend sign. */
/* The room's view distance, and the near end of its fog. The mesh culls at the
   far one so a culled poly has already fogged out to the background colour. */
#define MB_CULL_DIST             1500
#define MB_FOG_NEAR               350

#define MBDOOR_Z                  315
#define MBDOOR_W_X              (-808)
#define MBDOOR_E_X                769
#define MBDOOR_TEXT_Y           (-186)
#define MBDOOR_TEXT_RADIUS       1500
#define MBDOOR_FADE_NEAR         1000
#define MBDOOR_TRIGGER_RADIUS     500

/* Circle edge-detect per door, seeded by master_bedroom_doors_arm(). Start
   "held" so a press carried in from the hall-side transition doesn't bounce the
   player straight back out. */
static int wdoor_circle_prev = 1;
static int edoor_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void master_bedroom_doors_arm(void) {
    int held = circle_held();
    wdoor_circle_prev = held;
    edoor_circle_prev = held;
}

static int door_triggered(int32_t door_x, int *circle_prev) {
    int held = circle_held();
    int just = held && !*circle_prev;
    *circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - door_x;
    int32_t dz = cam_z - MBDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < MBDOOR_TRIGGER_RADIUS && interact_facing(door_x, MBDOOR_Z);
}

int master_bedroom_wdoor_triggered(void) {
    return door_triggered(MBDOOR_W_X, &wdoor_circle_prev);
}

int master_bedroom_edoor_triggered(void) {
    return door_triggered(MBDOOR_E_X, &edoor_circle_prev);
}

/* Floating "Press O to enter" sign on one of the north-wall doors. XY plane:
   door_draw_string_3d centres the reading axis (X) on world_x after adding 200,
   so pass door_x - 200. Sits just south (z-11) of the wall so it floats in
   front of the door. */
static void mbdoor_text(RenderContext *ctx, int32_t door_x) {
    int32_t dx = cam_x - door_x;
    int32_t dz = cam_z - MBDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= MBDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > MBDOOR_FADE_NEAR) {
        int range = MBDOOR_TEXT_RADIUS - MBDOOR_FADE_NEAR;
        int prog  = xz - MBDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        door_x - 200, MBDOOR_TEXT_Y, MBDOOR_Z - 11,
                        50, 255, 50, fade, 0, TEXT_PLANE_XY, DOOR_PIXEL_SIZE);
}

/* Spawn just inside a door, far enough south of the z=315 wall to clear the
   195 push radius apply_collision_reception uses, facing -Z into the room. */
void master_bedroom_spawn_west(void) {
    cam_x   = MBDOOR_W_X;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = 100;
    cam_rot = 2048;   /* facing -Z, into the room */
    master_bedroom_doors_arm();
}

void master_bedroom_spawn_east(void) {
    cam_x   = MBDOOR_E_X;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = 100;
    cam_rot = 2048;   /* facing -Z, into the room */
    master_bedroom_doors_arm();
}

void master_bedroom_init(void) {
    master_bedroom_collision_init(&current_collision_room);
    collision_set_ceiling_y(0);   /* proxy wall tops reach the drawn ceiling */
    master_bedroom_floor_zones_init();

    /* Default spawn: the east wing door (main.c overrides it for an arrival
       through the west one). */
    master_bedroom_spawn_east();

    /* Reception's save point and dresser prop are global (not room-swapped) and
       neither is area-gated in its collide routine, so reception's instances
       would block the player invisibly inside this room's bounds — the save
       point sits at (78,-67), right in the middle of the bed chamber. Clearing
       them is safe: reception_init() re-places both on every reception entry. */
    save_points_clear();
    dressers_clear();
}

static void draw_master_bedroom_smd(RenderContext *ctx) {
    if (!master_bedroom_smd) return;

    uint8_t *p = (uint8_t *)master_bedroom_smd->p_prims;
    int i, n = mb_key_count;

    /* HOISTED OUT OF THE REJECT LOOP, all three of them. The two trig lookups
       and the cull distance cannot change while a frame is being queued, and
       isin/icos were being called once EACH for every primitive that passed the
       distance cull -- up to 542 pairs of SDK calls a frame from the middle of
       this room, to recompute one pair of constants. That count PEAKS at the
       stance the lag was reported from (see mb_build_cull_keys above). This is
       the Rabisu fight's first fix, STEP 3C in tools/DIAGNOSING_FRAME_RATE.txt;
       the bedroom predates that work and never got it. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = MB_CULL_DIST;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);

    for (i = 0; i < n; i++) {
        /* >>> THE REJECT PATH READS cull_keys, NOT THE MESH. <<< Six sequential
           bytes carry this primitive's first vertex X/Z and its stride, which is
           everything both cheap tests below need AND everything the walk needs to
           advance -- so a rejected primitive never touches the SMD header, the
           vertex index array or the vertex array. Only a primitive that SURVIVES
           pays to address the mesh. */
        uint8_t stride = cull_keys[i].stride;
        {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            /* Distance cull (Manhattan) at the fog-out distance so culled polys
               are already invisible (same budget as the other rooms). */
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > cull)
                { p += stride; continue; }
            if (dx * sn + dz * cs < -(700 << 12))
                { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &master_bedroom_smd->p_verts[vi[0]];
        SVECTOR *v1 = &master_bedroom_smd->p_verts[vi[1]];
        SVECTOR *v2 = &master_bedroom_smd->p_verts[vi[2]];

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
           build time in master_bedroom_nocull — same scheme as the conservatory. */
        int nocull = (i < MASTER_BEDROOM_PRIM_COUNT) && master_bedroom_nocull[i];
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
            v3 = &master_bedroom_smd->p_verts[vi[3]];
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
        int32_t fog_start = MB_FOG_NEAR, fog_end = cull;   /* fog saturates at the cull distance */
        int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
        int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in master_bedroom_draw. */
        uint8_t tex_idx = (i < MASTER_BEDROOM_PRIM_COUNT) ? master_bedroom_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < MASTER_BEDROOM_TEX_COUNT);
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

void master_bedroom_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* Entities in this room fog with the same near/far as the mesh below, and
       follow the debug view distance when one is selected so levels 6/7 change
       what the room LOOKS like consistently rather than just where it stops. */
    g_fog_near = MB_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : MB_CULL_DIST;

    /* Dark interior background -- but as the CLEAR COLOUR, not as a primitive.
       The draw environments carry isbg=1, so DrawOTagEnv has already filled the
       whole framebuffer before the first poly is drawn; the full-screen TILE
       this room used to queue on top was a SECOND 77,000-pixel fill every frame,
       for the colour alone. Wrong turn #3 in tools/DIAGNOSING_FRAME_RATE.txt,
       and the Rabisu fight's second fix. */
    render_set_clear_colour(ctx, 20, 15, 10);

    /* 128x128 texture window so per-poly UVs wrap (tile) within each texture's
       page. All eight bedroom textures sit at page-top (Voff 0), so one window
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

    if (exp != DBG_EXP_NO_MESH) draw_master_bedroom_smd(ctx);

    /* No enemies placed here yet; the room's 128 texture window is still handed
       to the zombie renderer so a future spawn brackets its Voff>=128 sprite
       correctly (see tools/TEXTURING_NOTES.txt PART 5). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
        spiders_set_texwindow(&tw);
    }
    /* >>> LEVEL 8 REMOVES THE ENTITIES *AND* THE TWO DOOR SIGNS. <<< In most
       rooms level 8 takes out the monsters and the props, because that is what
       stands in them. This room is empty of both today, and the thing standing
       in its mesh that costs real money is the SIGNAGE: both north-wall prompts
       are inside their 1500 Manhattan radius from 1.20 stances out of 2 on
       average across the walkable footprint, and door_draw_string_3d has no
       facing test, so they are queued in full with the player's back to them --
       which is exactly the pose he is standing in to look at the bed.

       A BEHIND-CAMERA CULL ON THE SIGNS WAS MEASURED AND REJECTED, not skipped.
       Over the footprint at 32 headings it removes only 9.3% of the live sign
       work (13.5% for the headings that face the bed), because the mesh's own
       -700 "behind me" threshold is slack and the doors sit on the long wall of
       a wide room. That is Reception's 11% again, and STEP 3D rejected it there
       for the same number. What the signs already have is the rectangle
       decomposition (STEP 3D, src/door.c), which took this prompt from 165 quads
       to 64.

       D read at level 1, at 4 (no mesh) and at 8 (no signs) now splits this
       room's frame three ways in one sitting, which is what STEP 1 asks for and
       what nobody could do in here before -- the room honoured no isolation
       level at all. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        draw_zombies(ctx);
        draw_spiders(ctx);
        draw_rabisus(ctx);
        webs_draw(ctx);
        item_pickups_draw(ctx);

        mbdoor_text(ctx, MBDOOR_W_X);
        mbdoor_text(ctx, MBDOOR_E_X);
    }
}
