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
#include "keystone_maze.h"
#include "collision.h"
#include "keystone_maze_mesh_collision.h"
#include "keystone_maze_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "garden_courtyard.h"    /* garden_courtyard_upload_textures      */
#include "maze_two.h"            /* maze_two_upload_plinth                */
#include "save_point.h"
#include "zombie.h"
#include "spider.h"
#include "rafflesia.h"
#include "mushroom.h"
#include "living_statue.h"
#include "web.h"
#include "item_pickup.h"
#include "sml_med.h"
#include "keystone_plinths.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Keystone Maze: the third hedge maze, east of Maze One. See keystone_maze.h
   for the layout. */

static SMD  *keystone_maze_smd  = NULL;
static void *keystone_maze_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   MAZE ONE'S EXACTLY, which is Fountain Square's exactly: 575/2500, the same
   purple SKY_FOG_* colour, and the cull equal to the fog-far so nothing is
   dropped until the fog has already faded it into the background. The whole
   garden is one continuous walled place at one time of night, and stepping
   through a gate changes the plan of it but not the weather.

   It suits this room on its own account too. At 1439 prims over 6200 x 6200
   units it is a little smaller than Maze One both ways, and its corridors are
   600 wide as they are there — so at 2500 Manhattan the cull holds most of the
   room out of the packet buffer, and the hedge runs occlude far more than the
   fog does anyway. What the player sees is a couple of junctions ahead fading
   into the murk, which is the point of putting them in a maze at night. */
#define KM_CULL_DIST      2500
#define KM_FOG_NEAR        575
#define KM_FOG_FAR        2500

/* ---- Floor zones -----------------------------------------------------------
   ONE zone, exactly as both mazes. The collision generator found thirteen floor
   planes and every one is at y=0 — this room has no step, ramp or storey
   anywhere in it. They tile the maze's corridors and its central court rather
   than the whole bounding rectangle, but the gaps between them are the insides
   of hedge blocks, which the player is kept out of by the 500-tall wall runs in
   the wall list. One rect over the collision bounds therefore covers every
   square unit the player can legally stand on. */
static void keystone_maze_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -300;  floor_zones[0].max_x = 5900;
    floor_zones[0].min_z = -300;  floor_zones[0].max_z = 5900;
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

     0 hedge          the perimeter and every maze run  (clsd_drwr page, x384 y0)
     1 grdn_gte       the three gates                   (kchn_wl page,   x512 y0)
     2 grss_gs        the ground throughout             (stn_stl page,   x320 y0)
     3 gravel_gs      the central court's paving        (rusty_fence pg, x704 y0)
     4 plinth         the four corner blocks, and the
                      shafts of the five set into the
                      hedges                            (opn_drwr page,  x832 y0)
     5 plinth_diamond the keystone block in the middle,
                      and the caps on those five        (brick_wall pg,  x768 y0) OWNED

   Slots 0-3 come from the Garden Courtyard's uploader (which itself runs
   garden_stairs_upload_textures first) and cost this room NOTHING — and unlike
   either maze, this one draws gravel, so all four of that uploader's outdoor
   slots are actually in use here. As everywhere in the garden chain, the mesh's
   'grss' and 'gravel_texture' materials resolve to the grss_gs / gravel_gs
   CLONES rather than to their own pages, so the whole chain draws from one set
   of slots — see gen_keystone_maze_tex_map.py.

   Slot 4 is taken through a NARROW accessor on the module that owns it,
   maze_two_upload_plinth(), rather than through Maze Two's full uploader. That
   is not tidiness: maze_two_upload_textures() would also stamp its borrowed pipe
   on x768 y0, which is precisely where this room's plinth_diamond lives. The
   accessor was added for this room, on the conservatory_upload_con_tile pattern.

   Slot 5 is the one thing this room owns, and it went to an 8bpp full page this
   room draws nothing from: no fountain, no drain, no flower bed, no pipe, no
   bed. It adds no restore obligation — that page is already time-shared seven
   ways (grss, brick_wall, bed, fountain, the lamashtu tablet, the pipe,
   xt_dr_lckd/xt_dr_cmplt) and everyone who needs the original re-uploads on
   their own entry. Note what was NOT available this time: the gravel_gs page at
   x704, which Maze One's notes called free to a maze because no maze draws
   gravel, is in use HERE — and its right half is anzu3/anzu6 anyway, the trap
   Maze Two nearly fell into.

   All six sit at Voff 0, so the one 128 texture window set in keystone_maze_draw
   serves them all. */
#define KEYSTONE_MAZE_TEX_COUNT 6

static uint16_t tex_tpage[KEYSTONE_MAZE_TEX_COUNT];
static uint16_t tex_clut[KEYSTONE_MAZE_TEX_COUNT];

/* The one texture this room OWNS: RAM-resident from startup so the entry-time
   upload is a pure LoadImage with no CD read. Keep this in step with the slot
   numbering above and with NAME_TO_SLOT in gen_keystone_maze_tex_map.py. */
static int plinth_diamond_tex_id;

/* ---- Cull keys -------------------------------------------------------------
   >>> THE REJECT PATH, NOT THE DRAW PATH, IS WHAT THIS ROOM SPENDS ITS FRAME ON.
   <<< Measured on Maze One with the section counters, and the same shape of
   room gives the same answer here: only a small fraction of the 1439 primitives
   survive the distance cull, so nearly all of them are pure overhead — and each
   one would otherwise cost a read of the primitive header for its stride, a
   read of its first vertex INDEX, and then a chase into the 83 KB vertex array
   for the coordinates. The R3000 has no data cache behind any of
   that; every one of those is a main-memory stall, and the mesh is far too big
   for the scratchpad.

   So the cull key is lifted out into its own array, built once at load: the
   first vertex's X and Z, plus the primitive's stride so the walk can advance
   without reading the header at all. A rejected primitive now costs ONE
   sequential 6-byte read and never touches the mesh. Identical output — this
   changes what the reject path READS, not what it decides.

   6 bytes x 1439 = 8.6 KB of BSS, alongside the 1.4 KB keystone_maze_nocull
   table that is already indexed the same way. Indices match the draw loop's
   `i`. */
typedef struct { int16_t x, z; uint8_t stride, pad; } KmCullKey;
static KmCullKey km_keys[KEYSTONE_MAZE_PRIM_COUNT];
static int       km_key_count = 0;

static void km_build_cull_keys(void) {
    km_key_count = 0;
    if (!keystone_maze_smd) return;
    uint8_t *p = (uint8_t *)keystone_maze_smd->p_prims;
    int i, n = keystone_maze_smd->n_prims;
    if (n > KEYSTONE_MAZE_PRIM_COUNT) n = KEYSTONE_MAZE_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &keystone_maze_smd->p_verts[vi[0]];
        km_keys[i].x      = v0->vx;
        km_keys[i].z      = v0->vz;
        km_keys[i].stride = pt->len;
        km_keys[i].pad    = 0;
        p += pt->len;
    }
    km_key_count = n;
}

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale. At 83 KB this mesh sits well
   inside the arena that Maze One's 117 KB sizes, so nothing had to grow. */
void keystone_maze_load_geometry(void) {
    keystone_maze_buff = room_arena_load("\\TEX\\KEYSTONE.SMD;1");
    keystone_maze_smd  = keystone_maze_buff ? smdInitData(keystone_maze_buff) : NULL;
    km_build_cull_keys();
}

/* Read at STARTUP — the only safe time for CD access — the one texture this room
   owns. Geometry moved to keystone_maze_load_geometry above, and the other five slots
   are compile-time constants that cost nothing here. */
void keystone_maze_load_assets(void) {
    /* Every one of these is uploaded by another module; header only — no
       LoadImage, no second RAM copy. */
    TIM_SLOT(0, HEDGE);
    TIM_SLOT(1, GRDNGTE);
    TIM_SLOT(2, GRSSGS);
    TIM_SLOT(3, GRAVELGS);
    TIM_SLOT(4, PLINTH);

    /* This room's own: a RAM-resident copy for a CD-free re-upload. */
    plinth_diamond_tex_id = texmgr_register("\\TEX\\PLNTHDMD.TIM;1");
    TIM_SLOT(5, PLNTHDMD);
}

/* Upload the streamed textures. Pure LoadImage — no CD access — safe during the
   room transition (the caller DrawSyncs first, as main's STATE_LOADING does).

   ORDER MATTERS on both lines below. The courtyard's uploader (via the Garden
   Stairs') puts xt_dr_cg on x832 y0 and brick_wall on x768 y0 — which are where
   the plinth and its diamond cap live — so both of the following must go up
   AFTER it, never before. The delegated call is the NARROW one by design:
   maze_two_upload_textures() would also drop its borrowed pipe on x768 y0,
   straight over plinth_diamond. */
void keystone_maze_upload_textures(void) {
    garden_courtyard_upload_textures();   /* hedge, grdn_gte, grss_gs, gravel_gs */
    maze_two_upload_plinth();             /* plinth  -> the xt_dr_cg slot         */
    texmgr_upload(plinth_diamond_tex_id); /* diamond -> the brick_wall slot       */
}

/* ---- The west-wall gate back to Maze One ------------------------------------
   This room's ONLY connected gate, and the only way in or out of it. The
   grdn_gte polys on this side span z[-300,300] at x=-300, y[-600,0]. It is the
   same gate leaf as the one in Maze One's east hedge, which is twelve units
   narrower there (z[-900,-312], 588 against 600) — the two rooms' meshes are
   independent and only this pairing links them.

   Collision wall 44 runs across the opening at x=-300 with nx = +4096, so the
   walkable side is +X and the player approaches from inside this maze. For a
   sign in the YZ plane that is mirror=0, and it stands 11 units EAST of the wall
   (x + 11). Both are the opposite hand from Maze One's side of the same gate,
   whose wall faces -X — getting either backwards comes out as mirrored text or a
   sign buried in the hedge.

   The wall STAYS in the collision list: as at every other gate in the garden,
   the leaf is shut as far as collision is concerned and it is the trigger, not a
   hole, that lets the player through. */
#define KM_GATE_X            (-300)
#define KM_GATE_Z                0    /* (-300 + 300) / 2, the leaf's centre     */
#define KM_TEXT_Y             (-186)  /* eye level on the y=0 ground             */
#define KM_TEXT_RADIUS         1500
#define KM_FADE_NEAR           1000
#define KM_TRIGGER_RADIUS       500

/* Standing eye on the ground: floor y=0, less GROUND_FLOOR_Y and the 40-unit
   floor standoff apply_height applies. The same expression the rest of the
   garden uses, and for the same reason — this whole run of rooms has its floor
   at y=0 while the courtyard below is at positive y. */
#define KM_EYE_Y  (0 - GROUND_FLOOR_Y - 40)

/* Circle edge-detect, seeded by keystone_maze_gate_arm(). Starts "held" so a
   press carried in through the transition doesn't bounce the player straight
   back. */
static int gate_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void keystone_maze_gate_arm(void) {
    gate_circle_prev = circle_held();
}

int keystone_maze_gate_triggered(void) {
    int held = circle_held();
    int just = held && !gate_circle_prev;
    gate_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - KM_GATE_X;
    int32_t dz = cam_z - KM_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < KM_TRIGGER_RADIUS && interact_facing(KM_GATE_X, KM_GATE_Z);
}

/* Floating "Press O to enter" sign on the west gate. YZ plane:
   door_draw_string_3d centres the reading axis (Z) on world_z after adding 200,
   so pass door_z - 200. Sits just east (x+11) of the wall so it floats in front
   of the gate on the side the player is actually on. */
static void gate_text(RenderContext *ctx) {
    int32_t dx = cam_x - KM_GATE_X;
    int32_t dz = cam_z - KM_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= KM_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > KM_FADE_NEAR) {
        int range = KM_TEXT_RADIUS - KM_FADE_NEAR;
        int prog  = xz - KM_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        KM_GATE_X + 11, KM_TEXT_Y, KM_GATE_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* Arriving from Maze One: stand EAST of the x=-300 hedge, clear of the wall push
   radius (so the player isn't shoved on their first frame), facing +X — the
   direction of travel through the gate, looking down the corridor that runs
   along the south wall. x lands at -80, z at 0, which is inside collision
   FLOOR 11's x(-300,4499) z(-300,300) run, so the floor is under them
   immediately, and 300 clear of the corridor's own walls at z=-300 and z=300. */
void keystone_maze_spawn_west(void) {
    cam_x   = KM_GATE_X + COLLISION_WALL_RADIUS + 25;
    cam_y   = KM_EYE_Y;
    cam_vy  = 0;
    cam_z   = KM_GATE_Z;
    cam_rot = 1024;   /* facing +X, into the room */
    keystone_maze_gate_arm();
}

void keystone_maze_init(void) {
    keystone_maze_collision_init(&current_collision_room);
    /* The perimeter hedge is DRAWN to y=-500 (the gates reach -600, but they are
       the openings, not the roofline); most of the collision runs agree at 500,
       but the four corner plinths are only 120 and the keystone block 304 —
       those are proxies for solid objects, not for the roof. State the DRAWN
       value so anything ceiling-mounted hangs at the height the room actually
       has — see tools/ADDING_A_ROOM.txt on visual-vs-collision heights. */
    collision_set_ceiling_y(-500);
    /* No collision_set_wall_radius: the default 195 is right here, as in both
       mazes. Every corridor in this one is 600 wide, which at 195 leaves 210
       units of walkable lane — the same margin Maze One's give. The courtyard's
       260 would cut it to 80 and make the maze impassable. main.c resets to the
       default before every room init, so saying nothing is enough. */

    keystone_maze_floor_zones_init();

    /* Only one gate is connected, so there is one spawn and no main.c override
       to go with it. */
    keystone_maze_spawn_west();

    /* Save points and dresser props are global (not room-swapped) and neither is
       area-gated in its collide routine, so reception's instances would block
       the player invisibly if they fell inside this room's bounds — and this
       room's bounds contain the origin, so they certainly do. Clearing is safe:
       reception_init() re-places both on every reception entry. No save point of
       this room's own; the nearest is on the Garden Stairs' top landing. */
    save_points_clear();
    dressers_clear();

    /* The four alcove plinths and the keystone in the middle, installed from the
       CURRENT flags. main.c runs keystone_plinths_apply_flags() again once the
       real flags are in and the room's entities have been restored — this call
       is only so the room is never drawn in an undefined state during the load
       frame. See keystone_plinths.h. */
    keystone_plinths_place();
}

static void draw_keystone_maze_smd(RenderContext *ctx) {
    if (!keystone_maze_smd) return;


    uint8_t *p = (uint8_t *)keystone_maze_smd->p_prims;
    int i, n = keystone_maze_smd->n_prims;

    /* Hoisted: all constant for the whole frame, and every one of them was being
       recomputed per primitive inside the hottest loop in the room — the two
       trig lookups once for each poly that passed the distance cull, the cull
       distance and the debug test all 2037 times. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = KM_CULL_DIST;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    if (km_key_count < n) n = km_key_count;   /* keys not built: draw nothing */

    for (i = 0; i < n; i++) {
        /* ---- The reject path. ONE sequential read, no mesh access ----------
           km_keys carries this primitive's first vertex and its stride, so a
           primitive outside the view distance is skipped without touching the
           header or the vertex array at all. See km_build_cull_keys. */
        uint8_t stride = km_keys[i].stride;
        int32_t kdx = (int32_t)km_keys[i].x - cam_x;
        int32_t kdz = (int32_t)km_keys[i].z - cam_z;
        if ((kdx < 0 ? -kdx : kdx) + (kdz < 0 ? -kdz : kdz) > cull)
            { p += stride; continue; }

        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &keystone_maze_smd->p_verts[vi[0]];
        SVECTOR *v1 = &keystone_maze_smd->p_verts[vi[1]];
        SVECTOR *v2 = &keystone_maze_smd->p_verts[vi[2]];

        {

            /* ---- SIDE-PLANE FRUSTUM CULL -------------------------------------
               The behind-the-camera test below rejects almost nothing on its
               own: measured over the whole mesh at the entry pose it threw away
               ZERO polys, because in a maze the geometry within the cull radius
               is all around you rather than behind you. Everything else was
               being handed to the GTE, transformed, and then — if it projected
               inside the GPU's +/-1023 coordinate limit, which is over three
               screens wide — queued as a primitive the GPU had to take in and
               throw away. Measured at the entry pose: 297 primitives submitted,
               133 of them inside the horizontal field of view. Sweeping the
               heading, 72% of what was submitted was off to the sides, rising to
               93% when facing into a hedge.

               The test is the two side planes of the frustum. With
               gte_SetGeomScreen(256) on a 320-wide screen the half-field is
               160/256, so a point is outside the right plane when
                   side > (5/8) * fwd,  i.e.  8*side > 5*fwd
               and outside the left when the same holds for -side. Both sides of
               that comparison are world units (the >>12 undoes isin/icos), and
               at this room's scale the products stay far inside an int32.

               >>> A POLY IS ONLY CULLED WHEN EVERY VERTEX IS OUTSIDE THE SAME
               PLANE. <<< Testing v0 alone would slice through polys that
               straddle the screen edge. The cheap alternative — v0 plus a
               bounding sphere of the mesh's largest poly radius — was written
               first and measured against this: over a full heading sweep the
               sphere version removed 46% of submitted primitives and the exact
               one removes 72%, because a 400-unit sphere around v0 is far bigger
               than the poly it stands for. The exact test costs up to three more
               vertex evaluations, but ONLY for polys whose v0 is already outside
               a plane — a poly in view exits on the first test — and each one it
               rejects saves a full GTE transform and a queued primitive.

               Verified against the mesh offline before it was written: over 16
               headings at the entry pose it removes 72% of the 3604 primitives
               that would have been submitted, and introduces ZERO holes — where
               a hole means culling a poly with any vertex inside the true
               frustum. Re-run that check if the mesh is re-exported.

               Y is not tested. The camera is at standing height in a room whose
               hedges are drawn to y=-500 and whose floor is flat, so nothing is
               ever outside the top or bottom planes while inside the side ones;
               adding them would cost multiplies to reject nothing.
               ------------------------------------------------------------- */
            int32_t fwd = kdx * sn + kdz * cs;
            if (fwd < -(700 << 12))
                { p += stride; continue; }
            if (!no_frustum) {
                int32_t f0 = fwd >> 12;                      /* world units */
                int32_t s0 = (kdx * cs - kdz * sn) >> 12;
                int     sign = 0;
                if      ( s0 * 8 > f0 * 5) sign =  1;        /* v0 off right */
                else if (-s0 * 8 > f0 * 5) sign = -1;        /* v0 off left  */
                if (sign) {
                    int n = is_quad ? 4 : 3, k, all_out = 1;
                    for (k = 1; k < n; k++) {
                        SVECTOR *vk = &keystone_maze_smd->p_verts[vi[k]];
                        int32_t ex = (int32_t)vk->vx - cam_x;
                        int32_t ez = (int32_t)vk->vz - cam_z;
                        int32_t f  = (ex * sn + ez * cs) >> 12;
                        int32_t sd = (ex * cs - ez * sn) >> 12;
                        if (!(sign * sd * 8 > f * 5)) { all_out = 0; break; }
                    }
                    if (all_out) { p += stride; continue; }
                }
            }

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
           build time in keystone_maze_nocull — same scheme as the other rooms. */
        int nocull = (i < KEYSTONE_MAZE_PRIM_COUNT) && keystone_maze_nocull[i];
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
            v3 = &keystone_maze_smd->p_verts[vi[3]];
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
        int32_t fog = dist < KM_FOG_NEAR ? KM_FOG_NEAR : (dist > KM_FOG_FAR ? KM_FOG_FAR : dist);
        int32_t fog_factor = ((KM_FOG_FAR - fog) << 8) / (KM_FOG_FAR - KM_FOG_NEAR);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in keystone_maze_draw. */
        uint8_t tex_idx = (i < KEYSTONE_MAZE_PRIM_COUNT) ? keystone_maze_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < KEYSTONE_MAZE_TEX_COUNT);
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

void keystone_maze_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = KM_FOG_NEAR; g_fog_far = KM_FOG_FAR;

    /* Background in the SAME colour the fog saturates to, so a poly that has
       faded out is indistinguishable from the void behind it and the cull never
       shows a seam. Purple here, matching the rest of the garden.

       Painted by the HARDWARE CLEAR, not by a full-screen TILE like the other
       eighteen rooms — the draw environments already clear the framebuffer
       (isbg=1) before anything is drawn, so a tile on top of that was a second
       full-screen fill of the same 77k pixels every frame. See the note on
       render_set_clear_colour, and Maze One, which is where this was first
       tried. */
    render_set_clear_colour(ctx, SKY_FOG_R, SKY_FOG_G, SKY_FOG_B);

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

    /* --- Isolation switches (debug levels 4-7; see camera.c). Normal play and
       every other debug level take exp == 0 and none of this applies. --- */
    int exp = DEBUG_EXPERIMENT();

    /* The ladder moves the FOG with the cull, so each rung shows what the room
       would actually look like at that view distance — geometry fading out into
       the background colour rather than popping off at the cull line. Without
       this the rungs would all look wrong and the choice would be made on a
       picture the shipped game never draws. */
    if (DEBUG_CULL_DIST()) g_fog_far = DEBUG_CULL_DIST();

    if (exp != DBG_EXP_NO_MESH) draw_keystone_maze_smd(ctx);

    /* Every sprite enemy renderer is handed this room's texture window, because
       all of their sprites live at Voff >= 128 and must bracket it rather than
       sample the hedge (see tools/TEXTURING_NOTES.txt PART 5). Every one of
       these arrays is seeded EMPTY here — nothing lives in this maze yet — and
       the calls cost nothing on an empty array, so they stay wired up for
       whatever gets placed later.

       This room is on SND_BANK_GARDEN (main.c's STATE_LOADING), so anything
       placed here reaches SFX_HISS and the flowers' own loops; check any
       placement against src/sound.h the way Maze One's was. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
        spiders_set_texwindow(&tw);
        rafflesias_set_texwindow(&tw);
        mushrooms_set_texwindow(&tw);
        living_statues_set_texwindow(&tw);
    }
    {
        draw_zombies(ctx);
        draw_spiders(ctx);
        draw_rafflesias(ctx);
        draw_mushrooms(ctx);
        draw_living_statues(ctx);
        webs_draw(ctx);
        item_pickups_draw(ctx);
        sml_meds_draw(ctx);
    }

    /* The plinth puzzle: the coloured glows, the keystone's lit faces and — when
       it owns the camera — its item picker. After the mesh and the entities, so
       the additive lights land over what they fall on; the 2D half sorts into
       the menu-reserved OT range and is unaffected by the order. */
    keystone_plinths_draw(ctx);

    /* Last: the gate sign. */
    gate_text(ctx);
}
