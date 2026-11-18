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
#include "heros_plinth.h"
#include "heros_plinth_tex_map.h"
#include "underworld.h"
#include "underworld_sky.h"
#include "hell_hath_no_fury.h"  /* the volcanic floor, a narrow uploader      */
#include "descent_to_hell.h"    /* the lava, a narrow uploader                */
#include "catacombs_entry.h"    /* the catacomb inner door, a narrow uploader */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "particles.h"
#include "btn_glyph.h"
#include "door.h"

/* Hero's Plinth - see heros_plinth.h. The room half is src/wingscape.c's
   (itself Hell Hath No Fury's): cull keys, radius side cull and all. */

static SMD  *hp_smd  = NULL;
static void *hp_buff = NULL;

/* ---- The room --------------------------------------------------------------
   x[-1900,2100] z[-2100,2100] (the collision proxy's bounds; the drawn floor
   runs to x=-2100 behind the frame). The sky is centred on the middle of the
   drawn geometry, half way up Destiny's Door's frame (floor 0, top -1120). */
#define HP_CX                0
#define HP_CZ                0
#define HP_SKY_Y          (-560)

#define HP_FLOOR_Y           0
#define HP_EYE_Y          (HP_FLOOR_Y - GROUND_FLOOR_Y - 40)
/* The tallest drawn thing is the frame's arch, whose top is at y=-1120. */
#define HP_CEILING_Y      (-1125)

/* The ramp up to the frame: x[-1700,-1500] z[-700,700], y=0 at x=-1500 to
   y=-70 at the frame's foot, x=-1700 (collision wall 21, the frame, stands
   there from -70 up). Its two 200-square corners beside the frame,
   x[-1900,-1700], fall from -70 at z=+-700 back to 0 at z=+-900. */
#define HP_RAMP_TOP_Y     (-70)

/* ---- THE NORTH-EAST DOOR, to Wingscape's west door -------------------------
   The catacomb inner door polys stand on the cut corner x+z=3600 (collision
   wall 10, inward normal (-0.707,-0.707)), foot (1700,1900)-(1900,1700),
   leaning out to y=-600: centre (1800,1800), approached from the south-west.

   THE SIGN is on a free yaw facing the way the reader faces - 512, north-
   east - 11 off the door toward the player (8 on each axis). THE SPAWN, for an
   arrival through it, is 250 in along the normal (177 on each axis), clear of
   the 195 push radius, facing south-west into the room. */
#define HP_NE_X            1800
#define HP_NE_Z            1800
#define HP_NE_TEXT_X       1792
#define HP_NE_TEXT_Z       1792
#define HP_NE_TEXT_YAW      512
#define HP_NE_SPAWN_X      1623
#define HP_NE_SPAWN_Z      1623
#define HP_NE_SPAWN_ROT    2560

/* ---- THE SOUTH-EAST DOOR, to Sharur's Trial's north-west door --------------
   NOT a flat door: a 200 x 200 x 400 block in the cut corner, x[1700,1900]
   z[-1900,-1700], whose two faces into the room - west (collision wall 22,
   x=1700) and north (wall 16, z=-1700) - are both wd_dr. So the door point is
   the block's centre (1800,-1800): a player held 195 off either face is 295
   from it (Manhattan), and 476 on the diagonal at the block's outer corner,
   all inside the 500 reach.

   THE SIGN stands off that outer corner (1700,-1700), 11 toward the player
   (8 on each axis), facing the way the reader faces - 1536, south-east. THE
   SPAWN is 250 off both faces, (1450,-1450), facing north-west into the
   room. */
#define HP_SE_X            1800
#define HP_SE_Z          (-1800)
#define HP_SE_TEXT_X       1692
#define HP_SE_TEXT_Z     (-1692)
#define HP_SE_TEXT_YAW     1536
#define HP_SE_SPAWN_X      1450
#define HP_SE_SPAWN_Z    (-1450)
#define HP_SE_SPAWN_ROT    3584

#define HP_DOOR_TEXT_Y    (HP_FLOOR_Y - 200)
#define HP_TEXT_RADIUS     1500
#define HP_FADE_NEAR       1000
#define HP_TRIGGER_RADIUS   500

/* Circle edge-detect, one per door. Seeded "held" by the arm so a press
   carried in through the transition cannot fire on the arrival frame. */
static int ne_circle_prev = 1;
static int se_circle_prev = 1;

/* ---- THE FLOOR ZONES -------------------------------------------------------
   One storey, so order matters only where zones overlap: the three ramps
   first, the y=0 catch-all LAST (apply_height takes the first match). */
static void hp_floor_zones_init(void) {
    int i = 0;

    /* The ramp up to the frame, rising WEST. */
    floor_zones[i].type            = FLOOR_RAMP;
    floor_zones[i].min_x = -1700;  floor_zones[i].max_x = -1500;
    floor_zones[i].min_z =  -700;  floor_zones[i].max_z =   700;
    floor_zones[i].ramp_y_start    = HP_FLOOR_Y;      /* at x=-1500 */
    floor_zones[i].ramp_y_end      = HP_RAMP_TOP_Y;   /* at x=-1700 */
    floor_zones[i].ramp_axis_start = -1500;
    floor_zones[i].ramp_axis_end   = -1700;
    floor_zones[i].ramp_along_x    = 1;
    i++;
    /* The two corners beside the frame, falling away from it. */
    floor_zones[i].type            = FLOOR_RAMP;      /* north */
    floor_zones[i].min_x = -1900;  floor_zones[i].max_x = -1700;
    floor_zones[i].min_z =   700;  floor_zones[i].max_z =   900;
    floor_zones[i].ramp_y_start    = HP_RAMP_TOP_Y;   /* at z=700 */
    floor_zones[i].ramp_y_end      = HP_FLOOR_Y;      /* at z=900 */
    floor_zones[i].ramp_axis_start =   700;
    floor_zones[i].ramp_axis_end   =   900;
    floor_zones[i].ramp_along_x    = 0;
    i++;
    floor_zones[i].type            = FLOOR_RAMP;      /* south */
    floor_zones[i].min_x = -1900;  floor_zones[i].max_x = -1700;
    floor_zones[i].min_z =  -900;  floor_zones[i].max_z =  -700;
    floor_zones[i].ramp_y_start    = HP_RAMP_TOP_Y;   /* at z=-700 */
    floor_zones[i].ramp_y_end      = HP_FLOOR_Y;      /* at z=-900 */
    floor_zones[i].ramp_axis_start =  -700;
    floor_zones[i].ramp_axis_end   =  -900;
    floor_zones[i].ramp_along_x    = 0;
    i++;

    /* THE FLOOR, y=0: the catch-all, LAST. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -1900;  floor_zones[i].max_x = 2100;
    floor_zones[i].min_z = -2100;  floor_zones[i].max_z = 2100;
    floor_zones[i].y     = HP_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   EIGHT mesh textures, FOUR owned, and a NINTH owned for later:
     0  volcanic             Hell Hath No Fury's, its narrow uploader (x384 y256)
     1  lava                 the Descent's, its narrow uploader (x320 y256)
     2  catacomb inner door  the Catacombs Entry's, its narrow uploader (x832
                             y0) - which is also DOOR_PANEL_CATACOMB's panel,
                             the transition out to Wingscape, so it MUST be
                             uploaded here (the panel draws before
                             STATE_LOADING).
     3  wd_dr                RESIDENT: the fat door's startup upload at x768
                             y256 - DOOR_PANEL_WOOD's panel too. Nothing here
                             may take that page.
     4  destiny door frame top left      OWNED, 8bpp x384 y0,   CLUT (672,499)
     5  destinys door frame top right    OWNED, 8bpp x512 y0,   CLUT (672,500)
     6  destinys door frame bottom left  OWNED, 8bpp x896 y256, CLUT (672,490)
     7  destinys door frame bottom right OWNED, 8bpp x704 y0,   CLUT (672,484)
     8  destinys door                    OWNED, 8bpp x768 y0,   CLUT (672,483) -
                             DESTINY'S DOOR itself, which fills the frame. NOT
                             MODELLED YET, so no poly maps to this slot; it is
                             registered and uploaded with the room so the art
                             is in VRAM the day the door is.
   >>> ALL FIVE ARE 8bpp, FULL 256-COLOUR ART, BY THE USER'S ORDER. <<< They
   were shipped at 4bpp first and the user had them put back: never drop a
   texture to 16 colours without asking (tools/ADDING_A_ROOM.txt STEP 3d).
   Each takes a Voff-0 page whose tenant this room never draws - the
   cobblestones, Sharur's rocks, the two tablets (Sharur's / the Wings
   Tablet), the cracked tablet / the puzzle's cuneiform and Hell Hath No
   Fury's oil tank - every one re-uploaded on entry by the room that draws it
   (tools/VRAM_MAP_UNDERWORLD.txt: the chapter re-lays VRAM room by room). The
   Seal's page (x576 y0) is NOT one of them at 8bpp: its right half is
   hadad_idle_64, uploaded once at boot. Each 256-word CLUT is a Chapter 1-3
   room texture's (mud, Boss Wall, cuneiform _symbols, xt_dr_outr,
   xt_dr_cmplt/lckd), re-uploaded by its own room and never drawn down here.
   Registered in tools/vram_map.py KNOWN_STREAM_PAIRS. The sky's texture is
   src/underworld_sky.c's business. */
#define HP_TEX_COUNT 9
#define HP_OWNED     5

static uint16_t tex_tpage[HP_TEX_COUNT];
static uint16_t tex_clut[HP_TEX_COUNT];
static int      owned_tex[HP_OWNED] = { -1, -1, -1, -1, -1 };

void heros_plinth_load_assets(void) {
    /* BANK: Chapter 4. Derived, not guessed - py tools/check_tex_banks.py walks
       the uploader call graph and fails the build if this is short. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    owned_tex[0] = texmgr_register("\\TEXUNDW\\DDFRTL.TIM;1");
    owned_tex[1] = texmgr_register("\\TEXUNDW\\DDFRTR.TIM;1");
    owned_tex[2] = texmgr_register("\\TEXUNDW\\DDFRBL.TIM;1");
    owned_tex[3] = texmgr_register("\\TEXUNDW\\DDFRBR.TIM;1");
    owned_tex[4] = texmgr_register("\\TEXUNDW\\DSTNYDR.TIM;1");
    TIM_SLOT(0, VOLCANIC);
    TIM_SLOT(1, LAVA);
    TIM_SLOT(2, CTCMBDR);
    TIM_SLOT(3, WDDR);
    TIM_SLOT(4, DDFRTL);
    TIM_SLOT(5, DDFRTR);
    TIM_SLOT(6, DDFRBL);
    TIM_SLOT(7, DDFRBR);
    TIM_SLOT(8, DSTNYDR);
}

/* Pure LoadImage from the RAM copies texmgr holds - no CD access, safe during
   the transition (main's STATE_LOADING DrawSyncs first). */
void heros_plinth_upload_textures(void) {
    int i;
    hell_hath_no_fury_upload_volcanic();
    descent_to_hell_upload_lava();
    catacombs_entry_upload_inner_door();
    for (i = 0; i < HP_OWNED; i++)
        if (owned_tex[i] >= 0) texmgr_upload(owned_tex[i]);
    underworld_sky_upload_textures();
}

/* ---- The cull key, and its RADIUS ------------------------------------------
   ws_build_cull_keys() verbatim (src/wingscape.c, itself Hell Hath No Fury's;
   tools/DIAGNOSING_FRAME_RATE.txt STEP 3I for why the radius). Built on the
   same call that reloads the mesh it describes, and nowhere else. */
static int     hp_key_count = 0;
static int32_t hp_y_lo = 0, hp_y_hi = 0;

static void hp_build_cull_keys(void) {
    hp_key_count = 0;
    if (!hp_smd) return;
    uint8_t *p = (uint8_t *)hp_smd->p_prims;
    int i, n = hp_smd->n_prims;
    if (n > HEROS_PLINTH_PRIM_COUNT) n = HEROS_PLINTH_PRIM_COUNT;
    hp_y_lo = hp_y_hi = 0;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &hp_smd->p_verts[vi[0]];
        int nv = (pt->type >= 2) ? 4 : 3;
        int16_t lo_x = v0->vx, hi_x = v0->vx;
        int16_t lo_z = v0->vz, hi_z = v0->vz;
        int k;
        for (k = 0; k < nv; k++) {
            SVECTOR *vk = &hp_smd->p_verts[vi[k]];
            if (vk->vx < lo_x) lo_x = vk->vx;
            if (vk->vx > hi_x) hi_x = vk->vx;
            if (vk->vz < lo_z) lo_z = vk->vz;
            if (vk->vz > hi_z) hi_z = vk->vz;
            if (vk->vy < hp_y_lo) hp_y_lo = vk->vy;
            if (vk->vy > hp_y_hi) hp_y_hi = vk->vy;
        }
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        {
            int32_t rad = (hi_x - lo_x) + (hi_z - lo_z);   /* >= any |v - v0| */
            rad = (rad + 3) >> 2;
            cull_keys[i].pad = (uint8_t)(rad >= 255 ? 255 : rad);
        }
        p += pt->len;
    }
    hp_key_count = n;
}

void heros_plinth_load_geometry(void) {
    hp_buff = room_arena_load("\\TEXUNDW\\HEROPLN.SMD;1");
    hp_smd  = hp_buff ? smdInitData(hp_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (hp_smd && !room_data_bind(hp_smd->n_prims)) hp_smd = NULL;
    hp_build_cull_keys();
    /* The sky: kept across Chapter 4's rooms (a no-op when already held). */
    underworld_sky_load();
}

/* ---- The two doors ---------------------------------------------------------*/
void heros_plinth_doors_arm(void) {
    int held = interact_tapped();
    ne_circle_prev = held;
    se_circle_prev = held;
}

/* The shared body: a fresh Circle within reach of (door_x, door_z) and facing
   it. The edge state is kept up to date even while locked. */
static int hp_door_triggered(int32_t door_x, int32_t door_z,
                             int *circle_prev, int lock) {
    int held = interact_tapped();
    int just = held && !*circle_prev;
    int32_t dx, dz;
    *circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - door_x;
    dz = cam_z - door_z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= HP_TRIGGER_RADIUS)
        return 0;
    return interact_facing(door_x, door_z);
}

int heros_plinth_ne_door_triggered(int lock) {
    return hp_door_triggered(HP_NE_X, HP_NE_Z, &ne_circle_prev, lock);
}
int heros_plinth_se_door_triggered(int lock) {
    return hp_door_triggered(HP_SE_X, HP_SE_Z, &se_circle_prev, lock);
}

static void hp_door_text(RenderContext *ctx, int32_t door_x, int32_t door_z,
                         int32_t text_x, int32_t text_z, int yaw) {
    int32_t dx = cam_x - door_x;
    int32_t dz = cam_z - door_z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= HP_TEXT_RADIUS) return;
    if (xz > HP_FADE_NEAR) {
        int range = HP_TEXT_RADIUS - HP_FADE_NEAR;
        int prog  = xz - HP_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            text_x, HP_DOOR_TEXT_Y, text_z,
                            50, 255, 50, fade, yaw,
                            DOOR_PIXEL_SIZE);
}

static void hp_spawn_at(int32_t x, int32_t z, int rot) {
    cam_x     = x;
    cam_y     = HP_EYE_Y;
    cam_vy    = 0;
    cam_z     = z;
    cam_rot   = rot;
    cam_pitch = 0;
    camera_look_cancel();
    heros_plinth_doors_arm();
}

void heros_plinth_spawn_ne(void) {
    hp_spawn_at(HP_NE_SPAWN_X, HP_NE_SPAWN_Z, HP_NE_SPAWN_ROT);
}
void heros_plinth_spawn_se(void) {
    hp_spawn_at(HP_SE_SPAWN_X, HP_SE_SPAWN_Z, HP_SE_SPAWN_ROT);
}

void heros_plinth_init(void) {
    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(HP_CEILING_Y);
    hp_floor_zones_init();

    /* The default arrival (main.c overrides it per door), and where a debug
       jump lands: the south-east door, from Sharur's Trial. */
    heros_plinth_spawn_se();

    /* The global prop arrays, as Wingscape clears them: Hell Hath No Fury's
       save point and tank would otherwise block invisibly inside this room's
       bounds. The clear drops the placement, never OD_TANK_HHNF's oil. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    bars_clear();
    reset_particles();
}

/* ---- The mesh --------------------------------------------------------------
   draw_ws_smd() (src/wingscape.c) verbatim: the chapter's fog, set in
   src/underworld.h and changed by nothing, the key-point-and-radius side cull,
   and far primitives clamped one OT slot in front of the sky. */
static void draw_hp_smd(RenderContext *ctx) {
    if (!hp_smd) return;

    uint8_t *p = (uint8_t *)hp_smd->p_prims;
    int i, n = hp_key_count;

    /* HOISTED OUT OF THE LOOP (STEP 3C fix 1): none of these can change while
       a frame is being queued. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = UW_FOG_FAR;
    int32_t fog_start = UW_FOG_NEAR < cull ? UW_FOG_NEAR : cull / 2;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    /* ---- THE SIDE-PLANE CULL'S PER-FRAME TERMS ------------------------------
       Hell Hath No Fury's, unchanged: see draw_hhnf_smd for the derivation
       (the pitch bound, the 10*R radius term and the arithmetic). */
#define HP_SIDE_MARGIN  (8 << 12)
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    int     pitched    = (cam_pitch != 0);
    int32_t pc = icos(cam_pitch);
    int32_t pslack = 0;
    if (pc < 0) no_frustum = 1;
    if (pitched) {
        int32_t ps   = isin(cam_pitch);
        int32_t dylo = hp_y_lo - cam_y, dyhi = hp_y_hi - cam_y;
        if (ps   < 0) ps   = -ps;
        if (dylo < 0) dylo = -dylo;
        if (dyhi < 0) dyhi = -dyhi;
        pslack = ps * (dylo > dyhi ? dylo : dyhi) + 512;
    }

    for (i = 0; i < n; i++) {
        /* >>> THE REJECT PATH READS cull_keys, NOT THE MESH. <<< */
        uint8_t stride = cull_keys[i].stride;
        {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > cull)
                { p += stride; continue; }
            if (dx * sn + dz * cs < -(700 << 12))
                { p += stride; continue; }
        }
        /* The side planes, off the key point and its radius. */
        if (!no_frustum && cull_keys[i].pad != 255) {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            int32_t f  = dx * sn + dz * cs;
            int32_t sd = dx * cs - dz * sn;
            int32_t r  = ((int32_t)cull_keys[i].pad * 40) << 12;   /* >= 10R */
            if (pitched) f = (((f >> 8) * pc) >> 4) + pslack;
            f = f * 5 + HP_SIDE_MARGIN + r;
            if (sd * 8 > f || -sd * 8 > f) { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &hp_smd->p_verts[vi[0]];
        SVECTOR *v1 = &hp_smd->p_verts[vi[1]];
        SVECTOR *v2 = &hp_smd->p_verts[vi[2]];

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

        int nocull = (i < HEROS_PLINTH_PRIM_COUNT) && room_nocull(i);
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
            v3 = &hp_smd->p_verts[vi[3]];
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
        if (otz >= UW_SKY_OT) otz = UW_SKY_OT - 1;   /* in front of the sky */

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog = dist < fog_start ? fog_start : (dist > cull ? cull : dist);
        int32_t fog_factor = ((cull - fog) << 8) / (cull - fog_start);

        uint8_t tex_idx = (i < HEROS_PLINTH_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < HP_TEX_COUNT);
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

void heros_plinth_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THE CHAPTER'S FOG, and nothing else's (src/underworld.h). */
    g_fog_near = UW_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : UW_FOG_FAR;

    /* The clear colour, not a full-screen TILE (wrong turn #3). The sky is an
       open ring, so this is what shows above and below it. */
    render_set_clear_colour(ctx, UW_FOG_R, UW_FOG_G, UW_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All nine room textures and the sky's sit at Voff 0; no sprite is drawn
       here. */
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
            setTexWindow(twin, &tw);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    MATRIX view;
    camera_build_view(&view);
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);

    if (exp != DBG_EXP_NO_MESH) {
        underworld_sky_draw(ctx, HP_CX, HP_SKY_Y, HP_CZ);
        draw_hp_smd(ctx);
    }

    /* >>> LEVEL 8 IS THE TWO DOOR SIGNS: nothing else stands in this room. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        hp_door_text(ctx, HP_NE_X, HP_NE_Z, HP_NE_TEXT_X, HP_NE_TEXT_Z,
                     HP_NE_TEXT_YAW);
        hp_door_text(ctx, HP_SE_X, HP_SE_Z, HP_SE_TEXT_X, HP_SE_TEXT_Z,
                     HP_SE_TEXT_YAW);
    }
}
