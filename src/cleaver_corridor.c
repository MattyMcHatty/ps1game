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
#include "cleaver_corridor.h"
#include "lumberer.h"
#include "crawler.h"
#include "collision.h"
#include "cleaver_corridor_mesh_collision.h"
#include "cleaver_corridor_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "catacombs_entry.h"    /* the two narrow uploaders this room borrows */
#include "north_chamber.h"      /* ...and the ladder's, the third            */
#include "the_pit.h"            /* ...and the rusty ironwork, for the blades */
#include "cleaver.h"            /* the three slamming blades                 */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "player.h"             /* current_weapon, player_weapons */
#include "helluminator.h"       /* helluminator_burning — a view-distance factor */

/* The Cleaver Corridor — see cleaver_corridor.h for the layout, the shaft and
   the ways out. */

static SMD  *cc_smd  = NULL;
static void *cc_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   THE CHAPTER'S MACHINERY: cull and fog-far are equal, the base is scaled by
   what the player is carrying, and the scale eases rather than jumping.

   450/1600 with +50%/+50% for the lantern — the chapter's usual numbers. 321
   primitives in a 600-wide corridor is a small mesh, and the ring cull throws
   away everything more than 1600 down the corridor, so there is no lag argument
   for pulling them in. From the shaft the far door is in the dark; the corridor
   is walked into rather than seen down, which is the point of it. */
#define CC_BASE_FOG_NEAR   450
#define CC_BASE_FOG_FAR   1600

#define CC_VIEW_UNIT        256
#define CC_VIEW_HELL_BONUS  128   /* +50% while the lantern is in hand   */
#define CC_VIEW_BURN_BONUS  128   /* +50% more while it is actually lit  */
#define CC_VIEW_RATE          8   /* 1/256ths a frame; one bonus in 16 frames */

static int32_t cc_view     = CC_VIEW_UNIT;       /* eased scale, 1/256ths */
static int32_t cc_fog_near = CC_BASE_FOG_NEAR;   /* resolved, this frame  */
static int32_t cc_fog_far  = CC_BASE_FOG_FAR;

static int32_t cc_view_target(void) {
    int32_t s = CC_VIEW_UNIT;
    if (current_weapon == WEAPON_HELLUMINATOR &&
        (player_weapons & (1 << WEAPON_HELLUMINATOR))) {
        s += CC_VIEW_HELL_BONUS;
        if (helluminator_burning()) s += CC_VIEW_BURN_BONUS;
    }
    return s;
}

/* Ease one frame toward the target, then resolve both distances from it. `snap`
   jumps straight there, for room entry. */
static void cc_view_resolve(int snap) {
    int32_t target = cc_view_target();
    if (snap) {
        cc_view = target;
    } else if (cc_view < target) {
        cc_view += CC_VIEW_RATE;
        if (cc_view > target) cc_view = target;
    } else if (cc_view > target) {
        cc_view -= CC_VIEW_RATE;
        if (cc_view < target) cc_view = target;
    }
    cc_fog_near = (CC_BASE_FOG_NEAR * cc_view) >> 8;
    cc_fog_far  = (CC_BASE_FOG_FAR  * cc_view) >> 8;
}

/* The chapter's near-black with a cold lift (src/catacombs_entry.c says why it
   is not a true black). The nine untextured polys at the bottom of the shaft
   are authored dark and fog toward this, so the shaft reads as a drop into
   darkness rather than a floor. */
#define CC_FOG_R             7
#define CC_FOG_G             6
#define CC_FOG_B             9

/* Wall standoff. The chapter's 195. The corridor is 600 wide, which leaves a
   210 band down its middle — the same band as the North Chamber's gallery arms,
   and plenty to walk. */
#define CC_WALL_RADIUS     195

/* Standing eye on this room's one floor (y=0): less GROUND_FLOOR_Y and the
   40-unit standoff apply_height applies. */
#define CC_FLOOR_Y            0
#define CC_EYE_Y           (CC_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* ---- Floor zones -----------------------------------------------------------
   ONE, FLAT, AT y=0, over the proxy's one floor face exactly: x[0,4800]
   z[-300,300]. The shaft is NOT in it — nothing stands over the drop, and wall 3
   keeps the player 195 short of where the zone ends. */
static void cc_floor_zones_init(void) {
    int i = 0;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x =    0; floor_zones[i].max_x = 4800;
    floor_zones[i].min_z = -300; floor_zones[i].max_z =  300;
    floor_zones[i].y     = CC_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   THREE, AND THE ROOM OWNS NONE.

     0 cobblestones        the corridor, the vault and the shaft — 305 of the
                           321 polys                      (x384 y0)
     1 catacomb inner door the south door at the far end  (x832 y0)
     2 ladder              down the shaft's east wall, 5 polys
                           (x704 y256, the North Chamber's, on the
                           incinerator's page)

   Slots 0 and 1 come through src/catacombs_entry.c's narrow uploaders, as every
   Chapter 3 room takes them, and slot 2 through north_chamber_upload_ladder(),
   the narrow accessor that exists for this room. The ladder is drawn in exactly
   two rooms, the two ends of it, and each puts it up on entry — which is also
   what the ladder transition relies on (src/ladder_anim.h).

   All three sit at Voff 0, so the one 128 texture window in the draw serves
   them. */
#define CLEAVER_CORRIDOR_TEX_COUNT 3

static uint16_t tex_tpage[CLEAVER_CORRIDOR_TEX_COUNT];
static uint16_t tex_clut[CLEAVER_CORRIDOR_TEX_COUNT];

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/room_of_heads.c. The keys go in the SHARED arena
   (src/cull_arena.h) and are built on the same call that reloads the mesh they
   describe. No box key and no side-plane cull: nothing here would feed one. */
static int cc_key_count = 0;

static void cc_build_cull_keys(void) {
    cc_key_count = 0;
    if (!cc_smd) return;
    uint8_t *p = (uint8_t *)cc_smd->p_prims;
    int i, n = cc_smd->n_prims;
    if (n > CLEAVER_CORRIDOR_PRIM_COUNT) n = CLEAVER_CORRIDOR_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &cc_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    cc_key_count = n;
}

void cleaver_corridor_load_geometry(void) {
    cc_buff = room_arena_load("\\TEXCTCMB\\CLVRCRDR.SMD;1");
    cc_smd  = cc_buff ? smdInitData(cc_buff) : NULL;
    /* The one rule from src/cull_arena.h: build the keys HERE, on the same call
       that reloads the mesh they describe, and nowhere else. */
    cc_build_cull_keys();
}

/* STARTUP, and it does not touch the drive: three compile-time headers and no
   registration at all, so no texmgr_set_bank() either (the Garden Courtyard's
   case). The bank this room's art is in is decided by the three owners'. */
void cleaver_corridor_load_assets(void) {
    TIM_SLOT(0, COBBLE);
    TIM_SLOT(1, CTCMBDR);
    TIM_SLOT(2, LADDER);
}

/* Pure LoadImage from the RAM copies area_bank_sync() has already read — no CD
   access, safe during the transition (main's STATE_LOADING DrawSyncs first).

   >>> THE LADDER LINE IS WHAT OVERWRITES THE INCINERATOR, as it is in the North
   Chamber. <<< Nothing else in this room draws x704 y256, and the Incinerator
   Room's own uploader puts the machine back on the way in there, so there is no
   ordering rule between the three calls. */
void cleaver_corridor_upload_textures(void) {
    catacombs_entry_upload_cobble();
    catacombs_entry_upload_inner_door();
    north_chamber_upload_ladder();
    /* ...and The Pit's rusty ironwork, which the CLEAVERS are modelled in
       (src/cleaver.h) — x704 y0, a page nothing else in this room draws. */
    the_pit_upload_rusty();
}

/* ---- THE LADDER ------------------------------------------------------------
   x=0, z[-100,100], running DOWN the shaft's east wall from the corridor floor.
   Back down to THE NORTH CHAMBER, onto its gallery at the foot of its ladder.

   The prompt stands at the shaft's edge, over the top rung, at eye level. The
   edge is x=0 — wall 3, nx=+4096, approached from +X — so it is a YZ-plane
   sign with mirror=0, 11 proud of the edge along +X, and the -200
   door_draw_string_3d wants on the Z argument (the reading axis for a YZ sign),
   the North Chamber west door's pair.

   The trigger is the usual Manhattan radius about the top of the ladder with a
   facing test: the player stands at the edge looking into the shaft. Wall 3
   holds them at x>=195, well inside the 500. */
#define CC_LADDER_X             0
#define CC_LADDER_Z             0     /* the ladder spans z[-100,100] */
#define CC_LADDER_TEXT_Y     (-186)   /* eye level on the y=0 floor */
#define CC_TEXT_RADIUS       1200
#define CC_FADE_NEAR          800
#define CC_TRIGGER_RADIUS     500

/* ---- THE SOUTH DOOR --------------------------------------------------------
   z=-300, x[3800,4000], y[-400,0] — in the corridor's south wall at its east
   end. Into THE UP DOWN MAZE, through the north door on its UPPER storey.

   A door in the XY plane at fixed Z, approached from +Z (wall 1 runs z=-300
   with nz=+4096), so TEXT_PLANE_XY with mirror=1, the sign 11 proud of the wall
   along +Z, and the -200 on the X argument (the reading axis for an XY sign).
   The maze's north door, the far side, takes the opposite pair. */
#define CC_SOUTH_X           3900     /* the art spans x[3800,4000] */
#define CC_SOUTH_Z          (-300)
#define CC_SOUTH_TEXT_Y      (-186)   /* eye level on the y=0 floor */

/* ---- THE CLEAVERS (src/cleaver.h) ------------------------------------------
   THREE, AND THE LAYOUT IS THE BRIEF'S: cleaver, gap, cleaver, gap, cleaver,
   gap, door — every gap the same. The FIRST blade stands where Cleaver.smx put
   it (cleaver_authored_x/z/lift — x 1200, z 0, its edge 650 over the floor),
   and the other two share the distance from it to the south door's centre in
   equal thirds: 900 apart today, so at x 1200, 2100 and 3000 with the door at
   3900. Nothing here is a hard-coded blade position; re-export the blade
   somewhere else and the row moves with it. The door's centre is the measure
   rather than its west jamb (3800), because a gap is read by eye between the
   blade and the door as a whole.

   Each hangs from the vault at y=-800 — the ceiling line that hides the part
   of the blade still up in its slot.

   THEY HANG CC_CLEAVER_RAISE HIGHER THAN MODELLED: the edge at 750 over the
   floor rather than the export's 650, so only 50 of blade shows below the vault
   at rest. A placement tweak, kept here rather than in the prop, so the
   authored figure still reads back unchanged from cleaver_authored_lift(). */
#define CC_CLEAVER_COUNT       3
#define CC_CLEAVER_RAISE     100
#define CC_CEILING_Y        (-800)

static void cc_place_cleavers(void) {
    int32_t x0  = cleaver_authored_x();
    int32_t gap = (CC_SOUTH_X - x0) / CC_CLEAVER_COUNT;
    int k;
    cleavers_clear();
    for (k = 0; k < CC_CLEAVER_COUNT; k++)
        cleaver_place(STATE_CLEAVER_CORRIDOR, x0 + k * gap, -GROUND_FLOOR_Y,
                      cleaver_authored_z(),
                      cleaver_authored_lift() + CC_CLEAVER_RAISE,
                      CC_CEILING_Y);
}

/* Circle edge-detect, one per way out. Seeded "held" by the arm below so a press
   carried in through the transition cannot fire on the arrival frame. */
static int ladder_circle_prev = 1;
static int south_circle_prev  = 1;

void cleaver_corridor_arm(void) {
    ladder_circle_prev = interact_tapped();
    south_circle_prev  = ladder_circle_prev;
}

/* The south door's Circle test. The ladder's shape; the two are 3900 apart, so
   they can never both be in reach. */
int cleaver_corridor_south_door_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !south_circle_prev;
    int32_t dx, dz, xz;
    south_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - CC_SOUTH_X;
    dz = cam_z - CC_SOUTH_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= CC_TRIGGER_RADIUS) return 0;
    if (!interact_facing(CC_SOUTH_X, CC_SOUTH_Z)) return 0;
    return 1;
}

static void cc_south_door_text(RenderContext *ctx) {
    int32_t dx = cam_x - CC_SOUTH_X;
    int32_t dz = cam_z - CC_SOUTH_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= CC_TEXT_RADIUS) return;

    if (xz > CC_FADE_NEAR) {
        int range = CC_TEXT_RADIUS - CC_FADE_NEAR;
        int prog  = xz - CC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        CC_SOUTH_X - 200, CC_SOUTH_TEXT_Y, CC_SOUTH_Z + 11,
                        50, 255, 50, fade, 1, TEXT_PLANE_XY,
                        DOOR_PIXEL_SIZE);
}

void cleaver_corridor_spawn_south(void) {
    /* Back from the Up Down Maze. 220 off wall 1 on its walkable +Z side,
       facing +Z — the direction of travel through the door, across the
       corridor. The corridor is 600 deep, so that leaves the player 380 short of
       the north wall: inside it, clear of both. The nearest blade is 900 west,
       far outside its trigger. */
    cam_x   = CC_SOUTH_X;
    cam_y   = CC_EYE_Y;
    cam_vy  = 0;
    cam_z   = CC_SOUTH_Z + (CC_WALL_RADIUS + 25);
    cam_rot = 0;                       /* facing +Z, north across the corridor */
    cleaver_corridor_arm();
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, so a Circle held across a
   menu closing does not read as a fresh press on the frame the lock lifts. */
int cleaver_corridor_ladder_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !ladder_circle_prev;
    int32_t dx, dz, xz;
    ladder_circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - CC_LADDER_X;
    dz = cam_z - CC_LADDER_Z;
    xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= CC_TRIGGER_RADIUS) return 0;
    if (!interact_facing(CC_LADDER_X, CC_LADDER_Z)) return 0;
    return 1;
}

/* The ladder's floating prompt. Same shape as every door sign in the game:
   opaque within CC_FADE_NEAR, gone by CC_TEXT_RADIUS. */
static void cc_ladder_text(RenderContext *ctx) {
    int32_t dx = cam_x - CC_LADDER_X;
    int32_t dz = cam_z - CC_LADDER_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= CC_TEXT_RADIUS) return;

    if (xz > CC_FADE_NEAR) {
        int range = CC_TEXT_RADIUS - CC_FADE_NEAR;
        int prog  = xz - CC_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to descend",
                        CC_LADDER_X + 11, CC_LADDER_TEXT_Y, CC_LADDER_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void cleaver_corridor_spawn_ladder(void) {
    /* Up the ladder from the North Chamber. 220 off wall 3 on its walkable +X
       side, on the corridor's centre line, facing +X — the direction of travel:
       the climb comes up out of the shaft and steps off east, down the
       corridor. */
    cam_x   = CC_LADDER_X + (CC_WALL_RADIUS + 25);
    cam_y   = CC_EYE_Y;
    cam_vy  = 0;
    cam_z   = CC_LADDER_Z;
    cam_rot = 1024;                    /* facing +X, east down the corridor */
    cleaver_corridor_arm();
}

void cleaver_corridor_init(void) {
    cleaver_corridor_collision_init(&current_collision_room);
    /* The vault, read off the VISUAL mesh: y=-800 over the whole corridor, where
       the proxy's walls stop too. */
    collision_set_ceiling_y(-800);
    collision_set_wall_radius(CC_WALL_RADIUS);

    cc_floor_zones_init();
    cam_pitch = 0;

    cleaver_corridor_spawn_ladder();

    /* Save points, dressers, sconces and oil dispensers are global arrays, and
       two of the four are not area-gated in every collide routine, so an
       instance left from another room would block invisibly anywhere it falls
       inside x[0,4800] z[-300,300]. The Catacombs Entry's sconce at (595,200)
       lands inside it; sconces gate on area, so it does not bite, and this is
       the cheap guarantee rather than a fix. Safe to clear:
       catacombs_entry_init() re-places all four. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();

    /* THE CLEAVERS, cleared and re-placed ARMED on every entry: leaving the
       room is what resets them (src/cleaver.h). */
    cc_place_cleavers();

    cc_view_resolve(1);
}

static void draw_cleaver_corridor_smd(RenderContext *ctx) {
    if (!cc_smd) return;

    uint8_t *p = (uint8_t *)cc_smd->p_prims;
    int i, n = cc_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = cc_fog_far;   /* resolved by cc_view_resolve() this frame */
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
        SVECTOR *v0 = &cc_smd->p_verts[vi[0]];
        SVECTOR *v1 = &cc_smd->p_verts[vi[1]];
        SVECTOR *v2 = &cc_smd->p_verts[vi[2]];

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

        int nocull = (i < CLEAVER_CORRIDOR_PRIM_COUNT) && cleaver_corridor_nocull[i];
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
            v3 = &cc_smd->p_verts[vi[3]];
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
        /* Horizontal polys sort by their farthest corner (render.h): the
           corridor floor must not be sorted over the shaft walls below it. */
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
        int32_t fog = dist < cc_fog_near ? cc_fog_near : (dist > cc_fog_far ? cc_fog_far : dist);
        int32_t fog_factor = ((cc_fog_far - fog) << 8) / (cc_fog_far - cc_fog_near);

        uint8_t tex_idx = (i < CLEAVER_CORRIDOR_PRIM_COUNT) ? cleaver_corridor_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < CLEAVER_CORRIDOR_TEX_COUNT);

        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + CC_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + CC_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + CC_FOG_B * (256 - fog_factor)) >> 8);

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

void cleaver_corridor_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THIS frame's view distance, before anything reads it. */
    cc_view_resolve(0);

    g_fog_near = cc_fog_near;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : cc_fog_far;

    /* The fog colour as the CLEAR colour, not a full-screen TILE (wrong turn #3
       in tools/DIAGNOSING_FRAME_RATE.txt). */
    render_set_clear_colour(ctx, CC_FOG_R, CC_FOG_G, CC_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All three textures sit at Voff 0. The ladder's UVs run past 127 down the
       shaft and rely on it. */
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

    if (exp != DBG_EXP_NO_MESH) draw_cleaver_corridor_smd(ctx);

    /* >>> LEVEL 8 REMOVES THE PROMPTS, THE CLEAVERS AND THE ENEMIES. <<< */
    if (exp != DBG_EXP_NO_ENTITIES) {
        cc_ladder_text(ctx);      /* the ladder: YZ plane, approached from +X */
        cc_south_door_text(ctx);  /* south door: XY plane, approached from +Z */
        /* BOTH CHAPTER 3 ENEMIES, drawn in every room of the chapter whether or
           not world.c places one here: the area tag makes an absent enemy free.
           Both sheets sit at Voff 128, so each is handed the window to restore
           after drawing unmasked. */
        {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            crawlers_set_texwindow(&tw);
            lumberers_set_texwindow(&tw);
        }
        draw_crawlers(ctx);
        draw_lumberers(ctx);
        /* The blades AFTER the two enemy calls, whose windows are restored for
           this Voff-0 art: their UVs reach 128 (the bars' rule). */
        cleavers_draw(ctx);
    }
}
