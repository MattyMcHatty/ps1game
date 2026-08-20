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
#include "maze_one.h"
#include "collision.h"
#include "maze_one_mesh_collision.h"
#include "maze_one_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "garden_courtyard.h"    /* garden_courtyard_upload_textures      */
#include "fountain_square.h"     /* fountain_square_upload_drain          */
#include "outside_catacombs.h"   /* outside_catacombs_upload_flowers      */
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

/* Maze One: the hedge maze east of Fountain Square. See maze_one.h for the
   layout. */

static SMD  *maze_one_smd  = NULL;
static void *maze_one_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   FOUNTAIN SQUARE'S EXACTLY, and deliberately so: 575/2500, the same purple
   SKY_FOG_* colour, and the cull equal to the fog-far so nothing is dropped
   until the fog has already faded it into the background. The two rooms are one
   continuous walled garden either side of a gate, and matching them means the
   step through it changes the plan of the place but not the weather.

   It suits this room on its own account too. The mesh is 2037 prims, two and a
   half times the square's 802, and it covers 7600 x 6600 units — at 2500
   Manhattan the cull is holding roughly three-quarters of it out of the packet
   buffer at any moment. The maze corridors are only ~600 wide, so the hedge
   runs occlude far more than the fog does anyway; what the player actually sees
   is a couple of junctions ahead fading into the murk, which is the point of
   putting them in a maze at night. */
#define MO_CULL_DIST      2500
#define MO_FOG_NEAR        575
#define MO_FOG_FAR        2500

/* ---- Floor zones -----------------------------------------------------------
   ONE zone, exactly as Fountain Square. The collision generator found twenty-
   seven floor planes and every one is at y=0 — this room has no step, ramp or
   storey anywhere in it. They tile the maze's corridors rather than the whole
   bounding rectangle, but the gaps between them are the insides of hedge
   blocks, which the player is kept out of by the 333-tall wall runs in the wall
   list. One rect over the collision bounds therefore covers every square unit
   the player can legally stand on, and giving the hedge interiors zones of their
   own would only help someone who clipped a corner. */
static void maze_one_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -100;  floor_zones[0].max_x = 7500;
    floor_zones[0].min_z = -2095; floor_zones[0].max_z = 4496;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   Six mesh textures and the room owns exactly ONE of them. The other five are
   already put in VRAM by rooms this one is reached through, so for those it owns
   no texture RAM at all: it takes the tpage/clut headers as compile-time
   constants and delegates the entry-time LoadImage to whichever module holds the
   RAM copy. Registering a second copy of each would have been the obvious thing
   and is the wrong thing — texmgr has a hard TEXMGR_MAX and a registration past
   it fails SILENTLY, breaking that texture everywhere (see
   tools/TEXTURING_NOTES.txt).

     0 hedge              the perimeter and every maze run (clsd_drwr page, x384 y0)
     1 grdn_gte           the three gates                  (kchn_wl page,   x512 y0)
     2 grss_gs            the ground throughout            (stn_stl page,   x320 y0)
     3 drain              the channel across the paths     (opn_drwr page,  x832 y0)
     4 poison_flower_base the five flower beds             (trck_clue page, x640 y0)
     5 pipe               the standpipe                    (brick_wall page, x768 y0) OWNED

   Slots 0-2 come from the Garden Courtyard's uploader (which itself runs
   garden_stairs_upload_textures first). As in Fountain Square and the Outside
   Catacombs, the mesh's 'grss' material resolves to the grss_gs CLONE rather
   than to grss's own page, so the whole garden chain draws from one set of
   slots — see gen_maze_one_tex_map.py.

   Slots 3 and 4 are taken through NARROW accessors on the modules that own them
   — fountain_square_upload_drain() and outside_catacombs_upload_flowers() —
   rather than through either room's full uploader. That is not tidiness: both
   full uploaders would stamp their own second texture (fountain, and the
   lamashtu tablet) onto the brick_wall page at x768 y0, which is precisely where
   this room's pipe lives. Calling either one wholesale would put the fountain
   basin on the standpipe.

   Slot 5 is the one thing this room owns, and it went to the only full 8bpp page
   this room draws nothing from. Note that unlike every other garden room, Maze
   One draws NO gravel at all, so the gravel_gs page (x704 y0) was free too;
   brick_wall was picked to keep the pipe among the already-shared 8bpp set
   rather than to start sharing a page that currently is not. It adds no restore
   obligation either way: that page is already time-shared six ways (grss, bed,
   fountain, the tablet, xt_dr_lckd, xt_dr_cmplt) and everyone who needs the
   original re-uploads on their own entry.

   All six sit at Voff 0, so the one 128 texture window set in maze_one_draw
   serves them all. */
#define MAZE_ONE_TEX_COUNT 6

static uint16_t tex_tpage[MAZE_ONE_TEX_COUNT];
static uint16_t tex_clut[MAZE_ONE_TEX_COUNT];

/* The one texture this room OWNS: RAM-resident from startup so the entry-time
   upload is a pure LoadImage with no CD read. Keep this in step with the slot
   numbering above and with NAME_TO_SLOT in gen_maze_one_tex_map.py. */
static int pipe_tex_id;

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale — and note that at 117 KB it is
   THIS mesh that now sets the arena's size. */
void maze_one_load_geometry(void) {
    maze_one_buff = room_arena_load("\\TEX\\MAZEONE.SMD;1");
    maze_one_smd  = maze_one_buff ? smdInitData(maze_one_buff) : NULL;
}

/* Read at STARTUP — the only safe time for CD access — the one texture this room
   owns. Geometry moved to maze_one_load_geometry above, and the other five slots
   are compile-time constants that cost nothing here. */
void maze_one_load_assets(void) {
    /* Every one of these is uploaded by another module; header only — no
       LoadImage, no second RAM copy. */
    TIM_SLOT(0, HEDGE);
    TIM_SLOT(1, GRDNGTE);
    TIM_SLOT(2, GRSSGS);
    TIM_SLOT(3, DRAIN);
    TIM_SLOT(4, PSNFLWR);

    /* This room's own: a RAM-resident copy for a CD-free re-upload. */
    pipe_tex_id = texmgr_register("\\TEX\\PIPE.TIM;1");
    TIM_SLOT(5, PIPE);
}

/* Upload the streamed textures. Pure LoadImage — no CD access — safe during the
   room transition (the caller DrawSyncs first, as main's STATE_LOADING does).

   ORDER MATTERS on every line here. The courtyard's uploader (via the Garden
   Stairs') puts xt_dr_cg on x832 y0, chnlnk on x640 y0 and brick_wall on
   x768 y0 — which are where the drain, the flower beds and the pipe live — so
   all three of the following must go up AFTER it, never before. The two
   delegated calls are the NARROW ones by design: fountain_square_upload_textures
   and outside_catacombs_upload_textures would each also drop their own second
   texture on x768 y0, straight over the pipe. */
void maze_one_upload_textures(void) {
    garden_courtyard_upload_textures();   /* hedge, grdn_gte, grss_gs, gravel_gs */
    fountain_square_upload_drain();       /* drain   -> the xt_dr_cg slot        */
    outside_catacombs_upload_flowers();   /* flowers -> the chnlnk slot          */
    texmgr_upload(pipe_tex_id);           /* pipe    -> the brick_wall slot      */
}

/* ---- The west-wall gate back to Fountain Square -----------------------------
   The grdn_gte polys on this side span z[-300,500] at x=-100, y[-600,0]. It is
   the same gate leaf as the one in Fountain Square's east hedge, which is 72
   units narrower there — the two rooms' meshes are independent, and only this
   pairing links them. The gate's own alcove is collision FLOOR 11,
   x(-100,100) z(-300,500), which is what fixes the centre at z=100.

   Collision wall 73 runs across the opening at x=-100 with nx = +4095, so the
   walkable side is +X and the player approaches from inside the maze. For a
   sign in the YZ plane that is mirror=0, and it stands 11 units EAST of the wall
   (x + 11). Both are the opposite hand from Fountain Square's side of the same
   gate, whose wall faces -X — getting either backwards comes out as mirrored
   text or a sign buried in the hedge. */
#define MO_GATE_X            (-100)
#define MO_GATE_Z              100    /* (-300 + 500) / 2, and FLOOR 11's centre */
#define MO_TEXT_Y             (-186)  /* eye level on the y=0 ground             */
#define MO_TEXT_RADIUS         1500
#define MO_FADE_NEAR           1000
#define MO_TRIGGER_RADIUS       500

/* Standing eye on the ground: floor y=0, less GROUND_FLOOR_Y and the 40-unit
   floor standoff apply_height applies. The same expression Fountain Square and
   the Outside Catacombs use, and for the same reason — this whole run of garden
   rooms has its floor at y=0 while the courtyard below is at positive y. */
#define MO_EYE_Y  (0 - GROUND_FLOOR_Y - 40)

/* Circle edge-detect, seeded by maze_one_gate_arm(). Starts "held" so a press
   carried in through the transition doesn't bounce the player straight back. */
static int gate_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void maze_one_gate_arm(void) {
    gate_circle_prev = circle_held();
}

int maze_one_gate_triggered(void) {
    int held = circle_held();
    int just = held && !gate_circle_prev;
    gate_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - MO_GATE_X;
    int32_t dz = cam_z - MO_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < MO_TRIGGER_RADIUS && interact_facing(MO_GATE_X, MO_GATE_Z);
}

/* Floating "Press O to enter" sign on the west gate. YZ plane:
   door_draw_string_3d centres the reading axis (Z) on world_z after adding 200,
   so pass door_z - 200. Sits just east (x+11) of the wall so it floats in front
   of the gate on the side the player is actually on. */
static void gate_text(RenderContext *ctx) {
    int32_t dx = cam_x - MO_GATE_X;
    int32_t dz = cam_z - MO_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= MO_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > MO_FADE_NEAR) {
        int range = MO_TEXT_RADIUS - MO_FADE_NEAR;
        int prog  = xz - MO_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        MO_GATE_X + 11, MO_TEXT_Y, MO_GATE_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* Arriving from Fountain Square: stand in the gate alcove east of the x=-100
   hedge, clear of the wall push radius (so the player isn't shoved on their
   first frame), facing +X — the direction of travel through the gate, looking
   down the first corridor of the maze. x lands at 120, which is inside collision
   FLOOR 10's x(100,7500) run, so the floor is under them immediately. */
void maze_one_spawn_west(void) {
    cam_x   = MO_GATE_X + COLLISION_WALL_RADIUS + 25;
    cam_y   = MO_EYE_Y;
    cam_vy  = 0;
    cam_z   = MO_GATE_Z;
    cam_rot = 1024;   /* facing +X, into the room */
    maze_one_gate_arm();
}

void maze_one_init(void) {
    maze_one_collision_init(&current_collision_room);
    /* The perimeter hedge is DRAWN to y=-500 (the gates reach -600, but they are
       the openings, not the roofline); the collision runs are only 333 tall,
       which is the proxy, not what the player sees. State the DRAWN value so
       anything ceiling-mounted hangs at the height the room actually has — see
       tools/ADDING_A_ROOM.txt on visual-vs-collision heights. */
    collision_set_ceiling_y(-500);
    /* No collision_set_wall_radius: the default 195 is right here. The maze
       corridors are about 600 wide, which at 195 leaves 210 units of walkable
       lane — the same margin Fountain Square's 728-wide parterre paths give at
       338, and comfortably clear. The courtyard's 260 would cut it to 80 and
       make the maze impassable. main.c resets to the default before every room
       init, so saying nothing is enough. */

    maze_one_floor_zones_init();

    /* Only one gate is connected, so there is one spawn and no main.c override
       to go with it. */
    maze_one_spawn_west();

    /* Save points and dresser props are global (not room-swapped) and neither is
       area-gated in its collide routine, so reception's instances would block
       the player invisibly if they fell inside this room's bounds — and this
       room's bounds are the largest in the game, so they certainly do. Clearing
       is safe: reception_init() re-places both on every reception entry. No save
       point of this room's own; the nearest is on the Garden Stairs' top
       landing. */
    save_points_clear();
    dressers_clear();
}

static void draw_maze_one_smd(RenderContext *ctx) {
    if (!maze_one_smd) return;

    uint8_t *p = (uint8_t *)maze_one_smd->p_prims;
    int i;

    for (i = 0; i < maze_one_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &maze_one_smd->p_verts[vi[0]];
        SVECTOR *v1 = &maze_one_smd->p_verts[vi[1]];
        SVECTOR *v2 = &maze_one_smd->p_verts[vi[2]];

        {
            int32_t dx = (int32_t)v0->vx - cam_x;
            int32_t dz = (int32_t)v0->vz - cam_z;
            /* Distance cull (Manhattan) at the fog-out distance so culled polys
               are already invisible — see the view-distance note above. */
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > MO_CULL_DIST)
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
           build time in maze_one_nocull — same scheme as the other rooms. */
        int nocull = (i < MAZE_ONE_PRIM_COUNT) && maze_one_nocull[i];
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
            v3 = &maze_one_smd->p_verts[vi[3]];
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
        int32_t fog = dist < MO_FOG_NEAR ? MO_FOG_NEAR : (dist > MO_FOG_FAR ? MO_FOG_FAR : dist);
        int32_t fog_factor = ((MO_FOG_FAR - fog) << 8) / (MO_FOG_FAR - MO_FOG_NEAR);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in maze_one_draw. */
        uint8_t tex_idx = (i < MAZE_ONE_PRIM_COUNT) ? maze_one_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < MAZE_ONE_TEX_COUNT);
        /* Purple fog, the same night sky the rest of the garden looks out on,
           and at Fountain Square's exact near/far — this is one continuous
           outdoors and the gate between them is not a change in the weather. */
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

void maze_one_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = MO_FOG_NEAR; g_fog_far = MO_FOG_FAR;

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
       page. All six of this room's textures sit at page-top (Voff 0), so one
       window serves them (see tools/VRAM_MAP.txt). */
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

    draw_maze_one_smd(ctx);

    /* Every sprite enemy renderer is handed this room's texture window, because
       all of their sprites live at Voff >= 128 and must bracket it rather than
       sample the hedge (see tools/TEXTURING_NOTES.txt PART 5). The zombies and
       spiders are still seeded empty here; the FIVE RAFFLESIAS (one per
       poison_flower_base bed, rafflesias_init) and TWO MUSHROOM HEADS
       (world_seed_room) are real.

       Both of those are sound-legal here for the same reason Fountain Square's
       mushroom is: this room is on SND_BANK_GARDEN, which is where SFX_HISS and
       the flowers' own loops live (src/sound.h). */
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
