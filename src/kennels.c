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
#include "kennels.h"
#include "lumberer.h"
#include "crawler.h"
#include "collision.h"
#include "room_data.h"
#include "kennels_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "cdaudio.h"            /* suspend/resume around the entry-time read */
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "gula_tablet.h"        /* ...the floor inlay's                       */
#include "bars.h"               /* ...and the kennel fronts'                  */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons             */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */
#include "kennels_puzzle.h"     /* the four levers and the four cell gates    */
#include "demondog.h"           /* the eight caged dogs: sheet, draw          */
#include "gula.h"               /* ...and what their deaths call up            */
#include "gula_death.h"

/* The Kennels — see kennels.h. Copied from src/neck.c. */

static SMD  *kn_smd  = NULL;
static void *kn_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   366/1300 and the ~+46%/+46% lantern bonus - the Up Down Maze's numbers
   (src/up_down_maze.c says why): 1300 / 1900 / 2500 at rest, raised and lit.
   366/1300 keeps the 450/1600 ramp's shape, only shorter. Anything that
   lengthens these has to be measured BURNING, not at rest. */
#define KN_BASE_FOG_NEAR   366
#define KN_BASE_FOG_FAR   1300

#define KN_VIEW_UNIT        256
#define KN_VIEW_HELL_BONUS  118   /* ~+46% while the lantern is in hand   */
#define KN_VIEW_BURN_BONUS  118   /* ~+46% more while it is actually lit  */
#define KN_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t kn_view     = KN_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t kn_fog_near = KN_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t kn_fog_far  = KN_BASE_FOG_FAR;

static int32_t kn_view_target(void) {
    int32_t s = KN_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += KN_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += KN_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void kn_view_resolve(int snap) {
    int32_t target = kn_view_target();
    if (snap) {
        kn_view = target;
    } else if (kn_view < target) {
        kn_view += KN_VIEW_RATE;
        if (kn_view > target) kn_view = target;
    } else if (kn_view > target) {
        kn_view -= KN_VIEW_RATE;
        if (kn_view < target) kn_view = target;
    }
    kn_fog_near = (KN_BASE_FOG_NEAR * kn_view) >> 8;
    kn_fog_far  = (KN_BASE_FOG_FAR  * kn_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). */
#define KN_FOG_R             7
#define KN_FOG_G             6
#define KN_FOG_B             9

/* The gula-tablet inlay, x[-400,400] z[1400,2200], with a margin for the
   cull key (a poly's first vertex, which may sit on the box's edge). The tiles
   Gula stands on, lit through the fog while she is up (src/gula.h). */
#define KN_INLAY_X0       (-410)
#define KN_INLAY_X1         410
#define KN_INLAY_Z0        1390
#define KN_INLAY_Z1        2210

/* Wall standoff. The chapter's 195. The posts stand 600 in from the side walls
   and 800 off the south wall, so nothing here is a squeeze. */
#define KN_WALL_RADIUS      195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define KN_FLOOR_Y            0
#define KN_EYE_Y           (KN_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   ONE, FLAT, AT y=0, over the whole proxy. Its six floor faces (the FLOOR list
   at the foot of src/kennels_mesh_collision.c) are slabs of one plane - the
   hall, its chamfers, the two diagonal bays and the kennels - so one rectangle
   over the bounds serves them all. The posts and the space outside the
   chamfers are walls only; nobody can stand there. */
static void kn_floor_zones_init(void) {
    int i = 0;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -2389;  floor_zones[i].max_x = 2387;
    floor_zones[i].min_z =     0;  floor_zones[i].max_z = 4299;
    floor_zones[i].y     = KN_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   FIVE, ALL AT Voff 0:

     0 cobblestones        walls, posts, floor: 742 of 834     (x384 y0)
     1 gula tablet         the 4x4 floor inlay, 4bpp           (x640 y0)
     2 gula codes          the 14 panels on the back walls,
                           4bpp, THIS ROOM'S OWN               (x768 y0)
     3 bars                the kennel fronts and partition,
                           4bpp ('bars_128' in the export)     (x512 y256)
     4 catacomb inner door the one doorway                     (x832 y0)

   Slots 0 and 4 come through src/catacombs_entry.c's narrow uploaders, as every
   Chapter 3 room takes them; slot 1 through src/gula_tablet.c's (the Nursery
   draws the same tiles); slot 3 through src/bars.c's.

   >>> SLOT 2 TIME-SHARES THE ARM SWITCH'S PAGE *AND* ITS PALETTE. <<< 4bpp, so
   it covers x[768,800) only, and its CLUT is the arm switch's own (720,480):
   one texture displaced, restored by one uploader. The arm switch is drawn in
   The Head and nowhere else, and the_head_upload_textures() puts it back on
   the way in there; the page's other tenants (the lamashtu tablet, and the
   mansion and garden art) are re-uploaded by their own rooms on entry. It
   could not go on the baby names' page, which is the obvious "never drawn
   together" candidate: that is x640 y0, and the gula tablet in this room's
   own floor is already on it.

   >>> AND IT IS STREAMED, NOT REGISTERED. <<< The Head and Adapa left FREE AT
   REST at 128.1 KB (tools/HEAP_BUDGET.txt), right on the line under which that
   report says not to add a registration without reclaiming one; registering
   this 8 KB TIM put it at 112 KB. So it is the Greenhouse's pattern 2 instead
   (tools/heap_budget.py's docstring): read on entry into a scratch buffer,
   LoadImage'd, freed. Nothing resident, nothing in the CATACOMBS bank. The
   read is one 5-sector file, and it happens inside the door transition, where
   the chapter's music has already been stopped. */
#define KENNELS_TEX_COUNT 5

static uint16_t tex_tpage[KENNELS_TEX_COUNT];
static uint16_t tex_clut[KENNELS_TEX_COUNT];

#define KN_CODES_FILE     "\\TEXCTCMB\\GULACODE.TIM;1"
#define KN_TEX_SCRATCH    (5 * 2048)   /* gula codes.tim is 8256 bytes */

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/neck.c. The keys go in the SHARED arena (src/cull_arena.h)
   and are built on the same call that reloads the mesh they describe. No box
   key and no side-plane cull: nothing here would feed one. */
static int kn_key_count = 0;

static void kn_build_cull_keys(void) {
    kn_key_count = 0;
    if (!kn_smd) return;
    uint8_t *p = (uint8_t *)kn_smd->p_prims;
    int i, n = kn_smd->n_prims;
    if (n > KENNELS_PRIM_COUNT) n = KENNELS_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &kn_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    kn_key_count = n;
}

void kennels_load_geometry(void) {
    kn_buff = room_arena_load("\\TEXCTCMB\\KENNELS.SMD;1");
    kn_smd  = kn_buff ? smdInitData(kn_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of
       the same file (src/room_data.h). No block, no room. */
    if (kn_smd && !room_data_bind(kn_smd->n_prims)) kn_smd = NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    kn_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: five compile-time headers and no
   registration at all (the gula codes are streamed, below). It still declares
   the bank, which py tools/check_tex_banks.py checks against the uploader
   graph. */
void kennels_load_assets(void) {
    texmgr_set_bank(TEXBANK_CATACOMBS);

    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, GULATBLT);
    TIM_SLOT(2, GULACODE);
    TIM_SLOT(3, BARS);
    TIM_SLOT(4, CTCMBDR);
}

/* One TIM off the disc into `buf` and up into VRAM, pixels and CLUT -
   src/greenhouse.c's gh_stream_tim(). The tpage/clut it would produce are the
   TIM_SLOT constants already in tex_tpage/tex_clut, so nothing is captured. */
static void kn_stream_tim(const char *filename, uint8_t *buf, int bufcap) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)filename)) return;
    int sectors = (file.size + 2047) / 2048;
    if (sectors * 2048 > bufcap) return;   /* too big for the scratch buffer */
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);

    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);
    if (tim.mode & 0x8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
    }
}

/* Four pure LoadImages from the RAM copies area_bank_sync() has already read,
   then the one streamed read. Safe during the transition (main's
   STATE_LOADING DrawSyncs first); the read is bracketed by cdaudio_suspend /
   resume as every mid-game CdRead must be.

   >>> THE CODES READ IS WHAT OVERWRITES THE ARM SWITCH (PIXELS AND PALETTE).
   <<< Nothing else in this room draws x768 y0, and The Head puts it back on
   entry. The gula tablet line is the Nursery's, the way in here: arriving from
   it the page already holds the tablet, but a title load or a debug jump does
   not come that way. */
void kennels_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    gula_tablet_upload_texture();
    bars_upload_texture();

    uint8_t *scratch = malloc(KN_TEX_SCRATCH);
    if (!scratch) return;   /* draw with the arm switch's art rather than crash */
    cdaudio_suspend();
    kn_stream_tim(KN_CODES_FILE, scratch, KN_TEX_SCRATCH);
    cdaudio_resume();
    free(scratch);

    /* ...and the eight caged dogs' sheet, AFTER the scratch above is freed so
       the two reads never hold their buffers at once. Its own read and its own
       bracket; the same VRAM slot the mansion uses (src/demondog.c). */
    demon_dogs_upload_texture();

    /* ...and GULA's two halves, last and on the same terms: their own 34 KB
       scratch, freed before this returns. They land on the CRAWLER's two
       blocks, which is why main.c skips the crawler upload for this room
       (src/gula.h). A no-op once she is dead. */
    gula_upload_textures();
}

/* ---- THE SOUTH DOOR --------------------------------------------------------
   z=0, x[-200,0], y[-400,0] — in the south wall, just west of the centre line,
   and the only door. Back into the Nursery, through its west door (the one
   the Gula Tablet lifts off).

   A door in the XY plane at fixed Z, approached from +Z (wall 3 runs z=0 with
   nz=+4096), so TEXT_PLANE_XY with mirror=1 and the sign 11 proud of the wall
   along +Z - the Throat's south door's terms. The reading axis for an XY sign
   is X, so the -200 door_draw_string_3d wants goes on the X argument. */
#define KN_SOUTH_X          (-100)    /* the art spans x[-200,0] */
#define KN_SOUTH_Z              0
#define KN_DOOR_TEXT_Y       (-186)   /* eye level on the y=0 floor */
#define KN_TEXT_RADIUS       1200
#define KN_FADE_NEAR          800
#define KN_TRIGGER_RADIUS     500

/* Circle edge-detect. Seeded "held" by the arm below so a press carried in
   through the transition cannot fire on the arrival frame. */
static int south_circle_prev = 1;

void kennels_arm(void) {
    south_circle_prev = interact_tapped();
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int kennels_south_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !south_circle_prev;
    int32_t dx, dz, xz;
    south_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - KN_SOUTH_X;
    dz = cam_z - KN_SOUTH_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= KN_TRIGGER_RADIUS) return 0;
    if (!interact_facing(KN_SOUTH_X, KN_SOUTH_Z)) return 0;
    return 1;
}

/* The door's floating sign. Same shape as every other sign in the game: opaque
   within KN_FADE_NEAR, gone by KN_TEXT_RADIUS. */
static void kn_south_door_text(RenderContext *ctx) {
    /* No offer on a door that will not answer: sealed while Gula lives. */
    if (gula_seals_door()) return;
    int32_t dx = cam_x - KN_SOUTH_X;
    int32_t dz = cam_z - KN_SOUTH_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= KN_TEXT_RADIUS) return;
    if (xz > KN_FADE_NEAR) {
        int range = KN_TEXT_RADIUS - KN_FADE_NEAR;
        int prog  = xz - KN_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        KN_SOUTH_X - 200, KN_DOOR_TEXT_Y, KN_SOUTH_Z + 11,
                        50, 255, 50, fade, 1, TEXT_PLANE_XY,
                        DOOR_PIXEL_SIZE);
}

void kennels_spawn_south(void) {
    /* 220 off wall 3 on its walkable +Z side, on the door's centre line,
       facing +Z up the hall. (-100,220) is ~580 clear of the nearest post,
       the south-west one at x[-800,-599] z[799,999]. */
    cam_x   = KN_SOUTH_X;
    cam_y   = KN_EYE_Y;
    cam_vy  = 0;
    cam_z   = KN_SOUTH_Z + (KN_WALL_RADIUS + 25);
    cam_rot = 0;                       /* facing +Z, north up the hall */
    kennels_arm();
}

void kennels_init(void) {
    room_data_collision(&current_collision_room);
    /* THE WALLS, read off the VISUAL mesh: the stone rises to y=-800, where
       the proxy's outer walls stop too. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(KN_WALL_RADIUS);

    kn_floor_zones_init();
    cam_pitch = 0;

    kennels_spawn_south();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every collide routine, so an
       instance left from another room would block invisibly anywhere it falls
       inside x[-2389,2387] z[0,4299]. Cleared as the Neck clears them; safe,
       because catacombs_entry_init() re-places all four. The gula tablet is
       area-tagged and needs nothing. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    /* THE LEVER PUZZLE (src/kennels_puzzle.h): four levers on the east wall
       and a Bars gate in each of the four cell mouths, placed from the saved
       levers. Both prop arrays are cleared first; The Pit and the Sliding
       Bars Room re-place their own bars, and the Attic Exit its levers, on
       their own entries. */
    kennels_puzzle_place();

    kn_view_resolve(1);
}

static void draw_kennels_smd(RenderContext *ctx) {
    if (!kn_smd) return;

    uint8_t *p = (uint8_t *)kn_smd->p_prims;
    int i, n = kn_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = kn_fog_far;   /* resolved by kn_view_resolve() this frame */
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    /* GULA LIGHTS HER OWN SPOT (src/gula.h): while she is up, the gula-tablet
       inlay she stands on is exempt from the distance cull and drawn at her
       fade instead of the fog's. Only those sixteen tiles - the room's view
       distance does not move. 0 before the eighth dog and after she is gone. */
    int32_t reveal = gula_reveal();

    for (i = 0; i < n; i++) {
        /* THE REJECT PATH READS cull_keys, NOT THE MESH. */
        uint8_t stride = cull_keys[i].stride;
        int     inlay  = 0;
        {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            int32_t cd = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            if (reveal > 0 &&
                cull_keys[i].x >= KN_INLAY_X0 && cull_keys[i].x <= KN_INLAY_X1 &&
                cull_keys[i].z >= KN_INLAY_Z0 && cull_keys[i].z <= KN_INLAY_Z1)
                inlay = 1;
            if (cd > cull && !inlay)              { p += stride; continue; }
            if (dx * sn + dz * cs < -(700 << 12)) { p += stride; continue; }
        }

        /* SURVIVED BOTH CULLS: only now is the header read and the vertex array
           addressed. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &kn_smd->p_verts[vi[0]];
        SVECTOR *v1 = &kn_smd->p_verts[vi[1]];
        SVECTOR *v2 = &kn_smd->p_verts[vi[2]];

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

        int nocull = (i < KENNELS_PRIM_COUNT) && room_nocull(i);
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
            v3 = &kn_smd->p_verts[vi[3]];
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
           must not be sorted over the kennel posts standing on it. */
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
        int32_t fog = dist < kn_fog_near ? kn_fog_near : (dist > kn_fog_far ? kn_fog_far : dist);
        int32_t fog_factor = ((kn_fog_far - fog) << 8) / (kn_fog_far - kn_fog_near);

        uint8_t tex_idx = (i < KENNELS_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < KENNELS_TEX_COUNT);

        /* ...and only the TABLET tiles in that box (texture slot 1): her fade
           as a floor under the fog's, so it never darkens a near tile. */
        if (inlay && tex_idx == 1 && reveal > fog_factor) fog_factor = reveal;

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + KN_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + KN_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + KN_FOG_B * (256 - fog_factor)) >> 8);

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

void kennels_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    kn_view_resolve(0);

    g_fog_near = kn_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : kn_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, KN_FOG_R, KN_FOG_G, KN_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All five textures sit at Voff 0 (the bars at y256, which is Voff 0). */
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

    if (exp != DBG_EXP_NO_MESH) draw_kennels_smd(ctx);

    /* >>> LEVEL 8 IS THE SIGN AND THE PUZZLE. <<< The kennel front and the
       posts are mesh; the four cell gates and the levers are props. The two
       Chapter 3 enemy draws are area-tagged and
       cost nothing here; they are called on the Tomb's argument, so a room that
       is later given an occupant in world.c draws it without an edit. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        kn_south_door_text(ctx);   /* south: XY plane, approached from +Z */
        /* THE CHAPTER 3 ENEMIES. Their sheets sit at Voff 128, so each is
           handed the window to restore after drawing unmasked. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
            lumberers_set_texwindow(&tw);
            demon_dogs_set_texwindow(&tw);   /* the strip is at Voff 128 too */
            gula_set_texwindow(&tw);         /* ...and Gula's halves         */
        }
        draw_crawlers(ctx);
        draw_lumberers(ctx);
        draw_demon_dogs(ctx);   /* the eight in the cells (src/kennels_puzzle.h) */
        /* GULA and her wave, then her spirit (src/gula.h). Both want the plain
           view matrix, which nothing above has replaced. */
        draw_gula(ctx);
        gula_death_draw(ctx);
        /* AFTER the enemies: the bars need the 128 window they restore. */
        kennels_puzzle_draw(ctx);
    }
}
