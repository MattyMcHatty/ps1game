#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "room_arena.h"
#include "cull_arena.h"
#include "tim_slots.h"
#include "texmgr.h"
#include "camera.h"
#include "collision.h"
#include "room_data.h"
#include "the_tube.h"
#include "the_tube_tex_map.h"
#include "underworld.h"
#include "underworld_sky.h"
#include "sharurs_trial.h"      /* the rocks, a narrow uploader       */
#include "catacombs_entry.h"    /* the north door's art, likewise     */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "item_pickup.h"
#include "particles.h"
#include "btn_glyph.h"
#include "door.h"
#include "golem.h"              /* the north door's Golem             */
#include "player.h"             /* game_flag: the south door's Golem  */

/* The Tube - see the_tube.h. The room half is src/heavens_gate.c's (cull
   keys, the chapter's fog, the sky), with the mesh drawn in TWO PASSES: the
   cylinder under a spin matrix, then the doors under the plain view. */

static SMD  *tt_smd  = NULL;
static void *tt_buff = NULL;

/* ---- The room --------------------------------------------------------------
   x[-300,300] z[0,2400] (the collision proxy's bounds), one floor at y=0.
   The cylinder's axis is the line x=0, y=-300; its crown is y=-600. The sky
   is centred on the middle of the tube, on that axis. */
#define TT_CX                0
#define TT_CZ             1200
#define TT_AXIS_Y         (-300)
#define TT_RADIUS          300

#define TT_FLOOR_Y           0
#define TT_EYE_Y          (TT_FLOOR_Y - GROUND_FLOOR_Y - 40)
/* Above both the drawn mesh (the crown, -600) and the proxy's walls (-600). */
#define TT_CEILING_Y      (-601)

/* ---- THE SPIN --------------------------------------------------------------
   The cylinder turns about its own axis (x=0, y=-300, along Z) by
   TT_SPIN_STEP angle units a frame: 4096 / 8 = 512 frames a revolution,
   about 8.5 s at 60fps - slow to medium, the user's brief. Advanced in
   the_tube_update(), so it pauses with the game. Nothing else moves: not
   the doors, not the walls, not the floor. */
#define TT_SPIN_STEP         8
static int32_t tt_spin = 0;

/* ---- THE SOUTH DOOR, to Heavens Gate ---------------------------------------
   The wd_dr polys stand in the plane z=0 (collision wall 1, inward normal
   +Z), 400 tall and DISTORTED: the foot runs x[-174,100], the head
   x[-100,224], so the door leans east as it rises. Its middle vertex is
   (0,-200), so:
     - the PRESS point is (0,0), under it;
     - THE SIGN is 11 off it along the normal, at (0,-200,11), on a free yaw
       facing the way the reader faces - 2048, south;
     - THE SPAWN, for an arrival through it, is 250 in along the normal
       ((0,250)), clear of the 195 push radius, facing north (0), down the
       tube.
   The walls at x=+-300 leave the player x[-105,105], so the whole of that
   strip is inside the trigger radius at the door.
   Out of here plays DOOR_PANEL_WOOD: wd_dr is resident (x768 y256) and
   SFX_DOOR is resident. The far side is a catacomb inner door, so the way in
   plays DOOR_PANEL_CATACOMB (per direction, the chapter's rule 4). */
#define TT_S_X               0
#define TT_S_Z               0
#define TT_S_TEXT_X          0
#define TT_S_TEXT_Z         11
#define TT_S_TEXT_Y       (TT_FLOOR_Y - 200)
#define TT_S_TEXT_YAW     2048
#define TT_S_SPAWN_X         0
#define TT_S_SPAWN_Z       250
#define TT_S_SPAWN_ROT       0

/* ---- THE NORTH DOOR --------------------------------------------------------
   The catacomb inner door polys stand in the plane z=2400 (collision wall 3,
   inward normal -Z): a 56-wide post x[-28,28], 400 tall, with an arm out to
   x=-151 at the top. It leads to DESTINYS CABIN's west gate. So, the south
   door's mirror image:
     - the PRESS point is (0,2400), under it;
     - THE SIGN is 11 off it along the normal, at (0,-200,2389), on a free
       yaw facing the way the reader faces - 0, north;
     - THE SPAWN, for an arrival through it, is 250 in along the normal
       ((0,2150)), clear of the 195 push radius, facing south (2048), down
       the tube.
   Out of here plays DOOR_PANEL_CATACOMB: its art (CTCMBDR, x832 y0) is
   already up - this room draws that door - and SFX_CTCMBDR is in
   SND_BANK_UNDERWORLD. The far side is a garden gate, so the way in plays
   DOOR_PANEL_GATE (per direction, the chapter's rule 4).
   >>> A GOLEM STANDS IN FRONT OF IT, 500 in at (0,1900) <<< (world.c's
   seed), ALERT from the moment the player arrives, and tagged
   GLM_BLOCKS_TUBE_N_DOOR: the trigger refuses and the sign does not draw
   while golem_blocks(STATE_THE_TUBE, GLM_BLOCKS_TUBE_N_DOOR) - Ribcage's
   east door is the pattern. Only a DEBUG JUMP into the Cabin can bring the
   player back through this door with the Golem still standing; the arrival
   spawn is then 250 off its centre, inside its 425 hold, and its push
   settles them against the door wall. */
#define TT_N_X               0
#define TT_N_Z            2400
#define TT_N_TEXT_X          0
#define TT_N_TEXT_Z       2389
#define TT_N_TEXT_Y       (TT_FLOOR_Y - 200)
#define TT_N_TEXT_YAW        0
#define TT_N_SPAWN_X         0
#define TT_N_SPAWN_Z      2150
#define TT_N_SPAWN_ROT    2048

#define TT_TEXT_RADIUS    1500
#define TT_FADE_NEAR      1000
#define TT_TRIGGER_RADIUS  500

/* Circle edge-detect. Seeded "held" by the arm so a press carried in through
   the transition cannot fire on the arrival frame. */
static int s_circle_prev = 1;
static int n_circle_prev = 1;

/* ---- THE FLOOR -------------------------------------------------------------
   One storey: the proxy has one floor plane, y=0, so one FLOOR_FLAT zone over
   the collision bounds. The floor is flat although the tube is round - the
   player walks the bottom of it, and the walls keep them within 105 of the
   axis, where the curve is under 20 below the plane. No lava: the mesh has
   no lava texture, so the chapter's rule 5 has nothing to mask and the room
   has no ROOMS line in tools/gen_lava_floors.py (as Heavens Gate). */
static void tt_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -300;   floor_zones[0].max_x = 300;
    floor_zones[0].min_z = 0;      floor_zones[0].max_z = 2400;
    floor_zones[0].y     = TT_FLOOR_Y;
    floor_zone_count = 1;
}

/* ---- Textures --------------------------------------------------------------
   THREE mesh textures, NONE owned (the user's brief: no new textures). All
   4bpp at Voff 0:
     0  rocks_128            Sharur's Trial's ROCKS.TIM (x512 y0, CLUT
                             960,502), through sharurs_trial_upload_rocks() -
                             the cylinder. Heavens Gate's stairs share that
                             page and it re-uploads them on entry.
     1  wd_dr                the fat door's, RESIDENT (x768 y256) - the south
                             door, and DOOR_PANEL_WOOD's art for the way out.
     2  catacomb inner door  the Catacombs Entry's (x832 y0), its narrow
                             uploader - the north door.
   The sky's texture is src/underworld_sky.c's business. */
#define TT_TEX_COUNT 3

static uint16_t tex_tpage[TT_TEX_COUNT];
static uint16_t tex_clut[TT_TEX_COUNT];

void the_tube_load_assets(void) {
    /* No registrations: every texture is another module's, so no
       texmgr_set_bank() either (as A Woman Scorned and Lightnings Edge). */
    TIM_SLOT(0, ROCKS);
    TIM_SLOT(1, WDDR);
    TIM_SLOT(2, CTCMBDR);
}

/* Inside main's STATE_LOADING (which DrawSyncs first). */
void the_tube_upload_textures(void) {
    sharurs_trial_upload_rocks();
    catacombs_entry_upload_inner_door();
    underworld_sky_upload_textures();
}

/* ---- The cull key ------------------------------------------------------------
   Built on the same call that reloads the mesh it describes, and nowhere
   else (src/cull_arena.h). A DOOR prim's key is its first vertex, as in every
   room. A SPINNING prim's key is (0, v0.z): Z is the one coordinate the spin
   leaves alone, and x=0 is the axis, so the true point is never more than
   TT_RADIUS from the key in X - the draw widens both culls by that much for
   the spinning pass. No side cull, no box (the runbook's default; 152
   prims). */
static int tt_key_count = 0;

static void tt_build_cull_keys(void) {
    tt_key_count = 0;
    if (!tt_smd) return;
    uint8_t *p = (uint8_t *)tt_smd->p_prims;
    int i, n = tt_smd->n_prims;
    if (n > THE_TUBE_PRIM_COUNT) n = THE_TUBE_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &tt_smd->p_verts[vi[0]];
        cull_keys[i].x      = (i < THE_TUBE_SPIN_PRIMS) ? TT_CX : v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    tt_key_count = n;
}

void the_tube_load_geometry(void) {
    tt_buff = room_arena_load("\\TEXUNDW\\THETUBE.SMD;1");
    tt_smd  = tt_buff ? smdInitData(tt_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (tt_smd && !room_data_bind(tt_smd->n_prims)) tt_smd = NULL;
    tt_build_cull_keys();
    /* The sky: kept across Chapter 4's rooms (a no-op when already held). */
    underworld_sky_load();
}

/* ---- The doors ---------------------------------------------------------------*/
void the_tube_doors_arm(void) {
    s_circle_prev = interact_tapped();
    n_circle_prev = s_circle_prev;
}

/* A fresh Circle within reach of a door and facing it. The edge state is
   kept up to date even while locked. */
static int tt_door_triggered(int lock, int32_t x, int32_t z, int *prev) {
    int held = interact_tapped();
    int just = held && !*prev;
    int32_t dx, dz;
    *prev = held;
    if (lock || !just) return 0;
    dx = cam_x - x;
    dz = cam_z - z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= TT_TRIGGER_RADIUS)
        return 0;
    return interact_facing(x, z);
}

/* The south door, REFUSED while the Destiny Tablet's Golem stands in front
   of it (the north door's rule). */
int the_tube_s_door_triggered(int lock) {
    if (golem_blocks(STATE_THE_TUBE, GLM_BLOCKS_TUBE_S_DOOR)) {
        s_circle_prev = interact_tapped();
        return 0;
    }
    return tt_door_triggered(lock, TT_S_X, TT_S_Z, &s_circle_prev);
}

/* The north door, REFUSED while the Golem in front of it stands. The edge
   state is still kept current while it is. */
int the_tube_n_door_triggered(int lock) {
    if (golem_blocks(STATE_THE_TUBE, GLM_BLOCKS_TUBE_N_DOOR)) {
        n_circle_prev = interact_tapped();
        return 0;
    }
    return tt_door_triggered(lock, TT_N_X, TT_N_Z, &n_circle_prev);
}

/* A door's floating sign, faded in from TT_TEXT_RADIUS to TT_FADE_NEAR. */
static void tt_door_text(RenderContext *ctx, int32_t x, int32_t z,
                         int32_t tx, int32_t ty, int32_t tz, int yaw) {
    int32_t dx = cam_x - x;
    int32_t dz = cam_z - z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= TT_TEXT_RADIUS) return;
    if (xz > TT_FADE_NEAR) {
        int range = TT_TEXT_RADIUS - TT_FADE_NEAR;
        int prog  = xz - TT_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            tx, ty, tz, 50, 255, 50, fade, yaw,
                            DOOR_PIXEL_SIZE);
}

void the_tube_spawn_s(void) {
    cam_x     = TT_S_SPAWN_X;
    cam_y     = TT_EYE_Y;
    cam_vy    = 0;
    cam_z     = TT_S_SPAWN_Z;
    cam_rot   = TT_S_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();
    the_tube_doors_arm();
}

void the_tube_spawn_n(void) {
    cam_x     = TT_N_SPAWN_X;
    cam_y     = TT_EYE_Y;
    cam_vy    = 0;
    cam_z     = TT_N_SPAWN_Z;
    cam_rot   = TT_N_SPAWN_ROT;
    cam_pitch = 0;
    camera_look_cancel();
    the_tube_doors_arm();
}

void the_tube_init(void) {
    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(TT_CEILING_Y);
    tt_floor_zones_init();

    /* The default arrival, and where a debug jump lands: the south door,
       from Heavens Gate's east door. main.c overrides it with
       the_tube_spawn_n() for an arrival from Destinys Cabin. */
    the_tube_spawn_s();

    /* The global prop arrays, as every Chapter 4 room clears them: Hell Hath
       No Fury's save point and tank would otherwise block invisibly inside
       this room's bounds. The clear drops the placement, never OD_TANK_HHNF's
       oil. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    bars_clear();
    reset_particles();
}

/* ---- THE SOUTH DOOR'S GOLEM (world.c's second Tube seed) ---------------------
   The Destiny Tablet's second consequence. Seeded ABSENT; stood up here on
   every entry once the tablet is broken, until it has been killed.
   tt_s_golem_up is "it was stood up this visit", so the update only reads a
   GLM_DEAD as a kill after a revive - never the absent state it starts in. */
static int tt_s_golem_up = 0;

void the_tube_apply_flags(void) {
    Golem *g = golem_guard(STATE_THE_TUBE);
    tt_s_golem_up = 0;
    if (!g) return;
    if (game_flag(FLAG_DESTINY_TABLET_BROKEN) &&
        !game_flag(FLAG_TUBE_S_GOLEM_KILLED)) {
        if (g->state == GLM_DEAD) golem_revive(g);   /* alert: attacking */
        tt_s_golem_up = 1;
    }
}

void the_tube_update(void) {
    tt_spin = (tt_spin + TT_SPIN_STEP) & 4095;

    if (tt_s_golem_up) {
        Golem *g = golem_guard(STATE_THE_TUBE);
        if (!g || g->state == GLM_DEAD) {
            game_flag_set(FLAG_TUBE_S_GOLEM_KILLED);
            tt_s_golem_up = 0;
        }
    }
}

/* >>> EVERY MATRIX LOAD IN THIS FILE GOES THROUGH HERE, NOT THE SDK MACROS. <<<
   src/arm_switch.c's as_set_view(), verbatim: PSn00bSDK's gte_SetRotMatrix /
   gte_SetTransMatrix load through $t0..$t2 but declare only $t2 clobbered,
   and this file loads TWO matrices mid-draw with live values either side.
   Compiler-chosen operands, nothing hidden from it. */
static inline void tt_set_matrix(const MATRIX *m) {
    uint32_t r0 = (uint16_t)m->m[0][0] | ((uint32_t)(uint16_t)m->m[0][1] << 16);
    uint32_t r1 = (uint16_t)m->m[0][2] | ((uint32_t)(uint16_t)m->m[1][0] << 16);
    uint32_t r2 = (uint16_t)m->m[1][1] | ((uint32_t)(uint16_t)m->m[1][2] << 16);
    uint32_t r3 = (uint16_t)m->m[2][0] | ((uint32_t)(uint16_t)m->m[2][1] << 16);
    uint32_t r4 = (uint16_t)m->m[2][2];
    __asm__ volatile (
        "ctc2 %0, $0;"
        "ctc2 %1, $1;"
        "ctc2 %2, $2;"
        "ctc2 %3, $3;"
        "ctc2 %4, $4"
        : : "r"(r0), "r"(r1), "r"(r2), "r"(r3), "r"(r4));
    __asm__ volatile (
        "ctc2 %0, $5;"
        "ctc2 %1, $6;"
        "ctc2 %2, $7"
        : : "r"(m->t[0]), "r"(m->t[1]), "r"(m->t[2]));
}

/* ---- The mesh ----------------------------------------------------------------
   draw_hg_smd() (src/heavens_gate.c) for prims [first, last), less its side
   cull, with the caller's matrix already in the GTE. For the SPINNING pass
   (spin_s/spin_c = the turn's sin/cos, `spinning` set):
     - both culls are widened by TT_RADIUS, the most the key can be off in X
       (tt_build_cull_keys);
     - the fog distance is taken at the face centre AS TURNED: the turn moves
       X by x*cos - (y - axis)*sin, one pair of multiplies a survivor;
     - the flat-poly sort is skipped - a face that is level in the model is
       not level once turned.
   The backface cull needs nothing: it works on the projected corners. */
static void draw_tt_range(RenderContext *ctx, int first, int last,
                          int spinning, int32_t spin_s, int32_t spin_c) {
    if (!tt_smd) return;

    uint8_t *p = (uint8_t *)tt_smd->p_prims;
    int i;

    /* HOISTED OUT OF THE LOOP (STEP 3C fix 1): none of these can change while
       a frame is being queued. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = UW_FOG_FAR;
    int32_t fog_start = UW_FOG_NEAR < cull ? UW_FOG_NEAR : cull / 2;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int32_t slack     = spinning ? TT_RADIUS : 0;
    int32_t cull_far  = cull + slack;
    int32_t cull_back = -((700 + slack) << 12);

    if (last > tt_key_count) last = tt_key_count;
    for (i = 0; i < first; i++) p += cull_keys[i].stride;

    for (i = first; i < last; i++) {
        /* >>> THE REJECT PATH READS cull_keys, NOT THE MESH. <<< */
        uint8_t stride = cull_keys[i].stride;
        {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > cull_far)
                { p += stride; continue; }
            if (dx * sn + dz * cs < cull_back)
                { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &tt_smd->p_verts[vi[0]];
        SVECTOR *v1 = &tt_smd->p_verts[vi[1]];
        SVECTOR *v2 = &tt_smd->p_verts[vi[2]];

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

        int nocull = (i < THE_TUBE_PRIM_COUNT) && room_nocull(i);
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
            v3 = &tt_smd->p_verts[vi[3]];
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
           so floors stay behind whatever stands on them (see render.h). Not
           for the turning cylinder: see the note above. */
        if (!spinning && poly_is_flat_y(v0, v1, v2, v3))
            otz = is_quad ? otz_far4(sz[1], sz[2], v2_sz, sz[3])
                          : otz_far3(sz[1], sz[2], sz[3]);
        if (otz <= 0) { p += stride; continue; }
        otz += 40;
        if (otz >= UW_SKY_OT) otz = UW_SKY_OT - 1;   /* in front of the sky */

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        if (spinning) {
            int32_t face_cy = ((int32_t)v0->vy + v2->vy) / 2 - TT_AXIS_Y;
            face_cx = TT_CX + ((face_cx * spin_c - face_cy * spin_s) >> 12);
        }
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog = dist < fog_start ? fog_start : (dist > cull ? cull : dist);
        int32_t fog_factor = ((cull - fog) << 8) / (cull - fog_start);

        uint8_t tex_idx = (i < THE_TUBE_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < TT_TEX_COUNT);
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + UW_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + UW_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + UW_FOG_B * (256 - fog_factor)) >> 8);

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

/* The spinning pass: view * (turn about the axis). The turn M rotates about
   Z through the axis point A = (0, TT_AXIS_Y, 0): p' = A + R(p - A), i.e.
   rotation R and translation A - R*A. CompMatrixLV makes the view's
   V*R and V*(A - R*A) + T. */
static void draw_tt_tube(RenderContext *ctx, const MATRIX *view) {
    int32_t s = isin(tt_spin), c = icos(tt_spin);
    MATRIX turn, combined;
    turn.m[0][0] = (int16_t)c; turn.m[0][1] = (int16_t)-s; turn.m[0][2] = 0;
    turn.m[1][0] = (int16_t)s; turn.m[1][1] = (int16_t)c;  turn.m[1][2] = 0;
    turn.m[2][0] = 0;          turn.m[2][1] = 0;           turn.m[2][2] = ONE;
    /* R*A = (-s*Ay, c*Ay, 0) >> 12, so A - R*A: */
    turn.t[0] = TT_CX + ((s * TT_AXIS_Y) >> 12);
    turn.t[1] = TT_AXIS_Y - ((c * TT_AXIS_Y) >> 12);
    turn.t[2] = 0;
    CompMatrixLV((MATRIX *)view, &turn, &combined);
    tt_set_matrix(&combined);
    draw_tt_range(ctx, 0, THE_TUBE_SPIN_PRIMS, 1, s, c);
}

void the_tube_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THE CHAPTER'S FOG, and nothing else's (src/underworld.h). */
    g_fog_near = UW_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : UW_FOG_FAR;

    /* The clear colour, not a full-screen TILE (wrong turn #3). */
    render_set_clear_colour(ctx, UW_FOG_R, UW_FOG_G, UW_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All three room textures and the sky's sit at Voff 0. The Golem's sheet
       does NOT (Voff 128), so it brackets itself and is told what to
       restore. */
    RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
    golems_set_texwindow(&tw);
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
            setTexWindow(twin, &tw);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    MATRIX view;
    camera_build_view(&view);
    tt_set_matrix(&view);

    if (exp != DBG_EXP_NO_MESH) {
        underworld_sky_draw(ctx, TT_CX, TT_AXIS_Y, TT_CZ);
        /* The cylinder under the turn, then the view back for the doors -
           and for everything after them, which expects it in the GTE. */
        draw_tt_tube(ctx, &view);
        tt_set_matrix(&view);
        draw_tt_range(ctx, THE_TUBE_SPIN_PRIMS, THE_TUBE_PRIM_COUNT, 0, 0, ONE);
    }

    /* >>> LEVEL 8 IS THE GOLEM <<< (and its beam), the south door's sign
       and any pickup a later seed puts down. No props. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        item_pickups_draw(ctx);
        draw_golems(ctx);
        if (!golem_blocks(STATE_THE_TUBE, GLM_BLOCKS_TUBE_S_DOOR))
            tt_door_text(ctx, TT_S_X, TT_S_Z, TT_S_TEXT_X, TT_S_TEXT_Y,
                         TT_S_TEXT_Z, TT_S_TEXT_YAW);
        if (!golem_blocks(STATE_THE_TUBE, GLM_BLOCKS_TUBE_N_DOOR))
            tt_door_text(ctx, TT_N_X, TT_N_Z, TT_N_TEXT_X, TT_N_TEXT_Y,
                         TT_N_TEXT_Z, TT_N_TEXT_YAW);
    }
}
