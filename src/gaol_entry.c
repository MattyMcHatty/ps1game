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
#include "gaol_entry.h"
#include "lumberer.h"
#include "maggot.h"
#include "crawler.h"
#include "collision.h"
#include "room_data.h"
#include "gaol_entry_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "bars.h"               /* ...and the bars'                           */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* The Gaol Entry — see gaol_entry.h for the layout and the doors. */

static SMD  *gae_smd  = NULL;
static void *gae_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   366/1300 with ~+46%/~+46% for the lantern — THE UP DOWN MAZE'S NUMBERS,
   all four, so the fog does not change across the door between the two
   rooms. Keep them in step with src/up_down_maze.c. The room itself is 1200 x
   2200, so 1300 reaches most of it from the west door but not the back of the
   cells behind the gaol door (x=2700, 3000 from that door): the lantern is
   what shows how deep they go. 492 primitives, 125 of them bars. */
#define GAE_BASE_FOG_NEAR   366
#define GAE_BASE_FOG_FAR   1300

#define GAE_VIEW_UNIT        256
#define GAE_VIEW_HELL_BONUS  118   /* ~+46% while the lantern is in hand   */
#define GAE_VIEW_BURN_BONUS  118   /* ~+46% more while it is actually lit  */
#define GAE_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t gae_view     = GAE_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t gae_fog_near = GAE_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t gae_fog_far  = GAE_BASE_FOG_FAR;

static int32_t gae_view_target(void) {
    int32_t s = GAE_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += GAE_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += GAE_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void gae_view_resolve(int snap) {
    int32_t target = gae_view_target();
    if (snap) {
        gae_view = target;
    } else if (gae_view < target) {
        gae_view += GAE_VIEW_RATE;
        if (gae_view > target) gae_view = target;
    } else if (gae_view > target) {
        gae_view -= GAE_VIEW_RATE;
        if (gae_view < target) gae_view = target;
    }
    gae_fog_near = (GAE_BASE_FOG_NEAR * gae_view) >> 8;
    gae_fog_far  = (GAE_BASE_FOG_FAR  * gae_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). */
#define GAE_FOG_R             7
#define GAE_FOG_G             6
#define GAE_FOG_B             9

/* Wall standoff. The chapter's 195. The room is an empty box 1200 across, so
   nothing here argues for anything else. */
#define GAE_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define GAE_FLOOR_Y            0
#define GAE_EYE_Y           (GAE_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   ONE, FLAT, AT y=0, over the proxy's one floor face, x[-300,899]
   z[-500,1700] (see the FLOOR list at the foot of
   src/gaol_entry_mesh_collision.c). The cells beyond the gaol door have no
   floor zone because the east wall keeps the player out of them. */
static void gae_floor_zones_init(void) {
    int i = 0;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -300;   floor_zones[i].max_x = 899;
    floor_zones[i].min_z = -500;   floor_zones[i].max_z = 1700;
    floor_zones[i].y     = GAE_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   FOUR, AND THE ROOM OWNS ONE.

     0 cobblestones        the walls, the floor and the ceiling
                           — 361 of the 492 polys         (x384 y0)
     1 catacomb inner door the west doorway, 2 polys      (x832 y0)
     2 bars                the cells behind the gaol door,
                           125 polys, 4bpp                (x512 y256)
     3 gaol door           the barred door in the east
                           wall, 4 polys, 4bpp            (x640 y0)  OWNED HERE

   Slots 0 and 1 come through src/catacombs_entry.c's narrow uploaders, slot 2
   through src/bars.c's — The Shelf's set.

   >>> SLOT 3 TIME-SHARES THE ROOM OF ARMS' PAGE, BUT NOT ITS PALETTE. <<< x640
   y0 is the page arms, heads, legs, bones, torsos and The Shelf's incinerator
   panel already take turns on. None of those is drawn here, and each owner's
   uploader puts its own art back on entry, so nobody owes anybody a restore.
   Being 4bpp it covers only x[640,672) of the page, and its 16-word CLUT is
   NEW, at (576,502) in the free run after the chain's — so the arms' palette
   at (672,501) is untouched.

   Its PNG has see-through texels (the gaps between the bars): they become CLUT
   entry 0x0000, which the GPU skips, so the cells show through the door with
   no blend state here.

   All four sit at Voff 0, so the one 128 texture window in the draw serves
   them. */
#define GAOL_ENTRY_TEX_COUNT 4

static uint16_t tex_tpage[GAOL_ENTRY_TEX_COUNT];
static uint16_t tex_clut[GAOL_ENTRY_TEX_COUNT];

/* The one texmgr entry this room owns (slot 3). -1 until registration — a
   refused registration stops the boot on texmgr_refused()'s red screen, so a
   -1 here past startup would be a bug. texmgr_upload on -1 is a no-op. */
static int gaol_door_tex = -1;

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/room_of_torsos.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int gae_key_count = 0;

static void gae_build_cull_keys(void) {
    gae_key_count = 0;
    if (!gae_smd) return;
    uint8_t *p = (uint8_t *)gae_smd->p_prims;
    int i, n = gae_smd->n_prims;
    if (n > GAOL_ENTRY_PRIM_COUNT) n = GAOL_ENTRY_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &gae_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    gae_key_count = n;
}

void gaol_entry_load_geometry(void) {
    gae_buff = room_arena_load("\\TEXCTCMB\\GAOLENTR.SMD;1");
    gae_smd  = gae_buff ? smdInitData(gae_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of
       the same file (src/room_data.h). No block, no room. */
    if (gae_smd && !room_data_bind(gae_smd->n_prims)) gae_smd = NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    gae_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: four compile-time headers and ONE
   deferred registration. The bank mask is derived, not guessed — py
   tools/check_tex_banks.py walks the uploader graph and fails if CATACOMBS is not
   in it. */
void gaol_entry_load_assets(void) {
    texmgr_set_bank(TEXBANK_CATACOMBS);
    gaol_door_tex = texmgr_register("\\TEXCTCMB\\GAOLDOOR.TIM;1");

    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, CTCMBDR);
    TIM_SLOT(2, BARS);
    TIM_SLOT(3, GAOLDOOR);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   >>> THE GAOL DOOR LINE IS WHAT OVERWRITES THE ARMS (OR WHICHEVER OF ITS
   TIME-SHARERS WAS LAST UP). <<< Nothing else in this room draws x640 y0, and
   each of those rooms' uploaders puts its own art back on the way in there, so
   there is no ordering rule between the calls. */
void gaol_entry_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    bars_upload_texture();
    texmgr_upload(gaol_door_tex);
}

/* ---- THE WEST DOOR ---------------------------------------------------------
   x=-300, z[-300,-100], y[-400,0] — the catacomb inner door in the west wall.
   Back into the Up Down Maze, through its east-upper door.

   A door in the YZ plane at fixed X, approached from +X (wall 2 runs x=-300
   with nx=+4096), so TEXT_PLANE_YZ with mirror=0 and the sign 11 proud of the
   wall along +X — the Up Down Maze's own west door. The reading axis for a YZ
   sign is Z, so the -200 door_draw_string_3d wants goes on the Z argument. No
   storey test: this room is flat, whatever the room behind the door is.

   THE GAOL DOOR, x=899 z[300,700] in the east wall, is drawn and nothing
   else: no sign, no trigger, no collision gap. It reads as a locked cell door,
   which is what it is until the gaol behind it is built. */
#define GAE_WEST_X           (-300)
#define GAE_WEST_Z           (-200)   /* the art spans z[-300,-100] */
#define GAE_WEST_TEXT_Y      (-186)   /* eye level on the y=0 floor */
#define GAE_TEXT_RADIUS      1200
#define GAE_FADE_NEAR         800
#define GAE_TRIGGER_RADIUS    500

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int west_circle_prev = 1;

void gaol_entry_arm(void) {
    west_circle_prev = interact_tapped();
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int gaol_entry_west_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !west_circle_prev;
    int32_t dx, dz, xz;
    west_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - GAE_WEST_X;
    dz = cam_z - GAE_WEST_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= GAE_TRIGGER_RADIUS) return 0;
    if (!interact_facing(GAE_WEST_X, GAE_WEST_Z)) return 0;
    return 1;
}

/* The door's floating sign. Same shape as every other sign in the game: opaque
   within GAE_FADE_NEAR, gone by GAE_TEXT_RADIUS. */
static void gae_west_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - GAE_WEST_X;
    int32_t dz = cam_z - GAE_WEST_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= GAE_TEXT_RADIUS) return;

    if (xz > GAE_FADE_NEAR) {
        int range = GAE_TEXT_RADIUS - GAE_FADE_NEAR;
        int prog  = xz - GAE_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        GAE_WEST_X + 11, GAE_WEST_TEXT_Y, GAE_WEST_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void gaol_entry_spawn_west(void) {
    /* 220 off wall 2 on its walkable +X side, on the door's centre line,
       facing +X — the direction of travel, across the room to the gaol door.
       (-80,-200) is 300 off the south wall (z=-500), clear of the 195 push. */
    cam_x   = GAE_WEST_X + (GAE_WALL_RADIUS + 25);
    cam_y   = GAE_EYE_Y;
    cam_vy  = 0;
    cam_z   = GAE_WEST_Z;
    cam_rot = 1024;                    /* facing +X, east into the room */
    gaol_entry_arm();
}

void gaol_entry_init(void) {
    room_data_collision(&current_collision_room);
    /* The ceiling, read off the VISUAL mesh: y=-800 over the whole room, where
       the proxy's walls stop too. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(GAE_WALL_RADIUS);

    gae_floor_zones_init();
    cam_pitch = 0;

    gaol_entry_spawn_west();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       not all of them are area-gated in every collide routine, so an instance
       left from another room would block invisibly anywhere it falls inside
       x[-300,899] z[-500,1700]. The Catacombs Entry's sconces at (+-595,200)
       land inside it. Safe to clear: catacombs_entry_init() re-places all four,
       and the Up Down Maze's init re-places its own sconce. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    gae_view_resolve(1);
}

static void draw_gaol_entry_smd(RenderContext *ctx) {
    if (!gae_smd) return;

    uint8_t *p = (uint8_t *)gae_smd->p_prims;
    int i, n = gae_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = gae_fog_far;   /* resolved by gae_view_resolve() this frame */
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < n; i++) {
        /* THE REJECT PATH READS cull_keys, NOT THE MESH. */
        uint8_t stride = cull_keys[i].stride;
        {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            int32_t cd = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            if (cd > cull)                        { p += stride; continue; }
            if (dx * sn + dz * cs < -(700 << 12)) { p += stride; continue; }
        }

        /* SURVIVED BOTH CULLS: only now is the header read and the vertex array
           addressed. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &gae_smd->p_verts[vi[0]];
        SVECTOR *v1 = &gae_smd->p_verts[vi[1]];
        SVECTOR *v2 = &gae_smd->p_verts[vi[2]];

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

        int nocull = (i < GAOL_ENTRY_PRIM_COUNT) && room_nocull(i);
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
            v3 = &gae_smd->p_verts[vi[3]];
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
        /* Horizontal polys sort by their farthest corner (render.h): the floor
           must not be sorted over the bars standing on it. */
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
        int32_t fog = dist < gae_fog_near ? gae_fog_near : (dist > gae_fog_far ? gae_fog_far : dist);
        int32_t fog_factor = ((gae_fog_far - fog) << 8) / (gae_fog_far - gae_fog_near);

        uint8_t tex_idx = (i < GAOL_ENTRY_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < GAOL_ENTRY_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + GAE_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + GAE_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + GAE_FOG_B * (256 - fog_factor)) >> 8);

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

void gaol_entry_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    gae_view_resolve(0);

    g_fog_near = gae_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : gae_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, GAE_FOG_R, GAE_FOG_G, GAE_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All four textures sit at Voff 0. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    MATRIX rot_matrix;
    camera_build_view(&rot_matrix);
    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    if (exp != DBG_EXP_NO_MESH) draw_gaol_entry_smd(ctx);

    /* >>> LEVEL 8 REMOVES THE SIGN, and the enemies if world.c ever places
       any here. <<< Today the room is empty, so level 8 is the sign alone. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        gae_west_door_text(ctx);   /* west: YZ plane, approached from +X */
        /* THE CHAPTER 3 ENEMIES, drawn in every room of the chapter whether or
           not world.c places one here: the area tag makes an absent enemy free.
           Their sheets sit at Voff 128, so each is handed the window to restore
           after drawing unmasked. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
            lumberers_set_texwindow(&tw);
            maggots_set_texwindow(&tw);
        }
        draw_crawlers(ctx);
        draw_lumberers(ctx);
        draw_maggots(ctx);   /* area-tagged: free where none is placed */
    }
}
