#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "collision.h"
#include "camera.h"
#include "vampire.h"
#include "delivery_area_mesh_collision.h"
#include "kitchen_dining_mesh_collision.h"
#include "crate.h"
#include "dining_table.h"
#include "dresser.h"
#include "grinder.h"
#include "grinder_puzzle.h"  /* the corridor gate's backstop, collided as a prop */
#include "fatdoor.h"
#include "save_point.h"
#include "rafflesia.h"
#include "living_statue.h"
#include "hadad.h"
#include "piano_props.h"
#include "concrete_props.h"
#include "trick_drawers.h"
#include "attic_stairwell.h"   /* the attic altar, collided as a prop */
#include "chainlink_door.h"    /* placeable solid fence-gate prop */
#include "lever.h"             /* placeable wall-lever prop */
#include "rabisu.h"            /* the Rabisu boss is solid (area-tagged) */

CollisionRoom current_collision_room;

/* Player wall standoff for the current room; see the note in collision.h.
   Defined up here because apply_collision_reception() reads it well before
   collision_set_wall_radius() appears further down. */
static int32_t wall_radius = COLLISION_WALL_RADIUS;

void collision_init(void) {
    delivery_area_collision_init(&current_collision_room);
    collision_set_ceiling_y(0);   /* proxy wall tops reach the drawn ceiling */
}

int collide_wall(Wall *w, int32_t *px, int32_t *pz, int32_t radius) {
    int32_t dx = *px - w->x1;
    int32_t dz = *pz - w->z1;

    /* Signed distance from wall plane. Shift operands to avoid overflow on
       large levels before the >>12 denormalisation. */
    int32_t dot = ((dx >> 4) * (w->nx >> 4) + (dz >> 4) * (w->nz >> 4)) >> 4;
    if (dot >= radius) return 0;
    if (dot < -radius * 4) return 0;

    /* Reject if player is past either end of the segment. Shift wall vector
       and player offset down to keep products within 32-bit range. */
    int32_t wx      = (w->x2 - w->x1) >> 4;
    int32_t wz      = (w->z2 - w->z1) >> 4;
    int32_t dx_s    = dx >> 4;
    int32_t dz_s    = dz >> 4;
    int32_t along   = dx_s * wx + dz_s * wz;
    int32_t wlen_sq = wx * wx + wz * wz;
    if (along < 0 || along > wlen_sq) return 0;

    /* Push player out to radius distance from wall. */
    int32_t push = radius - dot;
    *px += (push * w->nx) >> 12;
    *pz += (push * w->nz) >> 12;
    return 1;
}

/* Hitscan-only prop test: 1 if (x,y,z) lies inside any prop's REAL solid volume
   (true footprint + real height, no player push margin). Every prop is height-
   aware, so the gun respects verticality — a shot passing over a low table or
   under an overhang isn't blocked, but a shot into the body is. This is separate
   from the player push-collide (which keeps its comfort standoff and its own
   height handling); a bullet wants the geometry, not the walking clearance. */
#define SHOT_PROP_SLACK  12   /* bullet half-width added to each prop footprint */
#define SHOT_PROP_STEP   30   /* segment sample spacing; < thinnest prop depth   */
static int props_block_point(int32_t x, int32_t y, int32_t z) {
    if (crates_point_solid(x, y, z, SHOT_PROP_SLACK))        return 1;
    if (fatdoors_point_solid(x, y, z, SHOT_PROP_SLACK))      return 1;
    if (dressers_point_solid(x, y, z, SHOT_PROP_SLACK))      return 1;
    if (grinders_point_solid(x, y, z, SHOT_PROP_SLACK))      return 1;
    if (dining_tables_point_solid(x, y, z, SHOT_PROP_SLACK)) return 1;
    if (piano_props_point_solid(x, y, z, SHOT_PROP_SLACK))   return 1;
    if (concrete_props_point_solid(x, y, z, SHOT_PROP_SLACK)) return 1;
    if (attic_stairwell_altar_point_solid(x, y, z, SHOT_PROP_SLACK)) return 1;
    if (chainlink_doors_point_solid(x, y, z, SHOT_PROP_SLACK)) return 1;
    if (levers_point_solid(x, y, z, SHOT_PROP_SLACK))          return 1;
    return 0;
}

/* Does the current area hold ANY solid prop at all? The same nine families, but
   asking only their active/state/area gates — no coordinates, so it is one pass
   over nine short arrays rather than one per sample point.

   >>> THIS IS WHAT KEEPS THE SAMPLER OFF THE PER-FRAME PATH. <<< The sampling
   loop below was sized for GUNFIRE, which happens a few times a second; its own
   comment says the extra samples are free for that reason. They are not free for
   the AI, which calls collision_segment_blocked up to four times a frame per
   enemy (src/mushroom.c has five call sites), and the sample COUNT scales with
   the segment's length — a mushroom's pacing sightline runs the full length of
   its patrol leg, so Maze One's 6097-unit leg alone hit 203 samples x 9 prop
   calls x 60 fps for a room that contains no props whatsoever.

   The gardens, the maze and Fountain Square hold nothing this test can find, so
   the whole pass is skipped there and the function collapses to its exact
   wall-crossing loop. Rooms that DO hold props are unaffected, sample for sample
   — this changes cost, never answers. */
static int props_any_solid(void) {
    return crates_any_solid()        || fatdoors_any_solid()      ||
           dressers_any_solid()      || dining_tables_any_solid() ||
           piano_props_any_solid()   || concrete_props_any_solid()||
           attic_stairwell_altar_any_solid() ||
           chainlink_doors_any_solid() || levers_any_solid() ||
           grinders_any_solid();
}

int collision_segment_blocked(int32_t ax, int32_t ay, int32_t az,
                              int32_t bx, int32_t by, int32_t bz) {
    CollisionRoom *r = &current_collision_room;
    int i;

    /* Shot direction in the XZ plane. */
    int32_t rx = bx - ax;
    int32_t rz = bz - az;

    /* Exact ray/segment crossing against every wall. A wall blocks the shot if
       the segment A->B crosses the wall segment within both extents. Because
       this is a true crossing test (not point-sampling) a wall can never be
       stepped over, at any distance, near or far. Coordinates stay within a few
       thousand units, so the 2D cross products fit comfortably in 32 bits. */
    for (i = 0; i < r->wall_count; i++) {
        Wall *w = &r->walls[i];

        /* Low walls the gun shoots over (e.g. the kitchen counter): the player
           still collides with them, but a shot passes so enemies on the far
           side are hittable. */
        if (i < 32 && ((r->shoot_over_mask >> i) & 1u)) continue;

        int32_t sx = w->x2 - w->x1;
        int32_t sz = w->z2 - w->z1;

        int32_t denom = rx * sz - rz * sx;   /* r x s; 0 => parallel */
        if (denom == 0) continue;

        int32_t qx = w->x1 - ax;
        int32_t qz = w->z1 - az;
        int32_t tn = qx * sz - qz * sx;      /* t*denom: pos along the shot   */
        int32_t un = qx * rz - qz * rx;      /* u*denom: pos along the wall    */

        /* Normalise sign so both parameters can be range-checked in [0,denom]
           without dividing. */
        int32_t d = denom;
        if (d < 0) { d = -d; tn = -tn; un = -un; }
        if (tn < 0 || tn > d) continue;      /* crossing behind A or past B    */
        if (un < 0 || un > d) continue;      /* crossing off the wall's ends   */

        /* Multi-level rooms: gate by height so a shot isn't blocked by a wall on
           another floor. The shot's world Y at the crossing is
             world_y = base + (by-ay)*t,   t = tn/d,  base = ay + GROUND_FLOOR_Y
           (camera-offset Y -> world Y to match the wall's world-space y_min/
           y_max). Test y_min <= world_y <= y_max without dividing by multiplying
           through by d (>0 after the sign-normalise above): 64-bit products keep
           the (Y-range * d) terms exact, and there's no 64-bit divide (which the
           -nostdlib toolchain can't link). Flat rooms carry only debug Y values,
           so they never gate. */
        if (r->multi_level && w->y_min != w->y_max) {
            int32_t base = ay + GROUND_FLOOR_Y;
            int64_t proj = (int64_t)(by - ay) * tn;          /* (world_y-base)*d */
            int64_t lo   = (int64_t)(w->y_min - base) * d;
            int64_t hi   = (int64_t)(w->y_max - base) * d;
            if (proj < lo || proj > hi) continue;
        }

        return 1;   /* a wall lies between the shot origin and the target */
    }

    /* Volumetric props (crates, doors, dressers, tables): sample the segment.
       Step at a FIXED distance, not a fixed count — a long shot with few samples
       would space them hundreds of units apart and skip a thin prop (e.g. a
       ~60-unit-deep door). SHOT_PROP_STEP < the thinnest prop's depth guarantees
       at least one sample lands inside.

       Skipped entirely in an area with no solid props — see props_any_solid.
       That guard is load-bearing now that the AI calls this every frame; without
       it the cost of the loop is paid by every room, in proportion to how long
       the segment is. */
    if (props_any_solid()) {
        int32_t adx = rx < 0 ? -rx : rx;
        int32_t adz = rz < 0 ? -rz : rz;
        int32_t span = adx > adz ? adx : adz;   /* dominant horizontal extent */
        int steps = span / SHOT_PROP_STEP;
        int k;
        if (steps < 2)   steps = 2;
        if (steps > 256) steps = 256;
        for (k = 1; k < steps; k++) {
            int32_t px = ax + (rx * k) / steps;
            int32_t py = ay + ((by - ay) * k) / steps;
            int32_t pz = az + (rz * k) / steps;
            if (props_block_point(px, py, pz)) return 1;
        }
    }

    return 0;
}

/* Draw-side occlusion test — see the long note in collision.h for the rules
   every caller has to keep. Cheap in the rooms that need it most: a prop-less
   room skips the sampling pass entirely (props_any_solid), so this is one pass
   over the room's walls per sprite. */
int collision_hidden_from_camera(int32_t x, int32_t y, int32_t z) {
    return collision_segment_blocked(cam_x, cam_y, cam_z, x, y, z);
}

/* ---- The DELIVERY AREA's wall set, per floor -------------------------------
 *
 * collide_wall() above is purely 2D: it knows nothing about y_min/y_max, and it
 * pushes an entity out to the wall's front face from EITHER side (up to 175 in
 * front, up to 700 behind). That is fine for a flat room, where every wall is a
 * room boundary at one height. It is not fine here, because this room's mesh
 * contributes interior faces that only exist for one of its three floors:
 *
 *   THE RAMP'S NORTH BANK  the z 3321..3339 strip, walls 6/8/9/15/28/30-34.
 *       These are the OUTSIDE of the embankment the ramp is cut into: their
 *       normals point north, into the yard, and they exist to stop someone down
 *       in the yard walking into the bank. Someone up on the ramp deck is behind
 *       them, ~330 units back, well inside collide_wall's 700-unit reach — so
 *       leaving these on flung the player ~500 units north, off the deck and
 *       into the yard, where the bank then blocked them from climbing back.
 *       That was the juddering. Gated on POSITION (DELIVERY_BANK_Z), which is
 *       set just past the ramp zone's north edge so nothing holds the player on
 *       the deck — the edge is unfenced and falling off it is intended.
 *
 *   THE UNDER-RAMP FACE  x=-4194, z(2556..3339). The retaining face under the
 *       TOP of the ramp, walkable side -X. Ground floor only: it is what stops a
 *       player under the platform walking into the ramp's underside, but on the
 *       ramp or the platform it would seal the two off from each other.
 *
 * Both sets are matched on their coordinates rather than by index, so
 * regenerating the collision from a fresh export cannot silently shift the set
 * out from under this function.
 *
 * Shared by the player, the vampire and the demon dogs so all three read the
 * same wall list — they only differ in radius and which floor flags they pass. */

/* The ramp's north bank: the whole wall lies in the z 3300..3345 strip. */
static int delivery_wall_is_ramp_bank(const Wall *w) {
    return w->z1 >= 3300 && w->z1 <= 3345 &&
           w->z2 >= 3300 && w->z2 <= 3345;
}

/* North of this line you are in the yard, looking at the bank; south of it you
   are on the ramp deck, behind it.
   It is one unit past the ramp floor zone's own north edge (3339) ON PURPOSE.
   The ramp's north edge is unfenced — walking off it and dropping into the yard
   is intended — so the bank must not engage while the player is still anywhere
   the ramp zone still claims, or it would shove them off instead of letting
   them walk off. The bank's own geometry stops at z=3339 too, so nothing is
   left unguarded: it only has to hold back someone already down in the yard. */
#define DELIVERY_BANK_Z 3340

/* The retaining face under the top of the ramp: the whole wall lies on x=-4194
   (the generator splits it as -4204..-4193, hence the band). */
static int delivery_wall_is_under_ramp(const Wall *w) {
    return w->x1 >= -4210 && w->x1 <= -4185 &&
           w->x2 >= -4210 && w->x2 <= -4185;
}

/* Walls too short for collide_wall's along-the-wall test to mean anything.
 *
 * That test shifts both the wall vector and the player offset down by 4 before
 * multiplying, so a wall's DIRECTION is only known to 1/16 of a unit. On a wall
 * a few hundred units long that is a rounding error. On a short diagonal it is
 * not: this mesh's 55-unit north-east chamfer, (-2023,3339)-(-2075,3321),
 * quantises to (-4,-2) — 8 degrees off its true (-52,-18) — and over a long
 * lever arm that error swamps the endpoint check entirely. The wall then behaves
 * as an almost unbounded LINE: it was displacing the player ~200 units
 * diagonally from as far away as the first yard and the corridor mouth, which is
 * the "jutting, then can't get through" this was reported as.
 *
 * 100 units is comfortably below every wall that carries real room boundary
 * (the next shortest here is 156 and axis-aligned, where the direction is exact
 * however short it is) and above the chamfer. The chamfer itself is redundant:
 * walls 28 and 32 already overlap across the corner it cuts.
 *
 * The proper fix is to stop quantising in collide_wall — the products fit in
 * int32 unshifted at this room's coordinate scale — but that function is shared
 * by all fourteen rooms and every doorway in them is tuned around its current
 * tolerance, so this stays local to delivery. */
#define DELIVERY_MIN_WALL_LEN 100
static int delivery_wall_too_short(const Wall *w) {
    int32_t dx = w->x2 - w->x1;
    int32_t dz = w->z2 - w->z1;
    return dx * dx + dz * dz <
           (int32_t)DELIVERY_MIN_WALL_LEN * DELIVERY_MIN_WALL_LEN;
}

static void delivery_collide_walls(int32_t *x, int32_t *z, int32_t radius,
                                   int on_upper_floor, int on_ramp) {
    CollisionRoom *r = &current_collision_room;
    int on_ground = !on_ramp && !on_upper_floor;
    int i, pass;

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < r->wall_count; i++) {
            Wall *w = &r->walls[i];

            if (delivery_wall_too_short(w)) continue;

            if (delivery_wall_is_ramp_bank(w)) {
                /* Gated on the entity's POSITION, not on on_ramp — and that
                   distinction is the whole reason the ramp was unclimbable.
                   The floor flags are one frame stale here: main runs collision
                   BEFORE apply_height, so on the frame you first step into the
                   ramp's x range you are still flagged 'ground'. With the bank
                   live for that one frame it caught you from behind and threw
                   you ~700 units north, out of the ramp zone entirely — so
                   apply_height never got to flag you as on the ramp, and you
                   ended up walking the yard instead. Position has no such lag:
                   south of the bank's own face you are on the deck, full stop. */
                if (*z < DELIVERY_BANK_Z || on_upper_floor) continue;
            } else if (delivery_wall_is_under_ramp(w)) {
                if (!on_ground) continue;        /* seen from the yard only */
            }
            collide_wall(w, x, z, radius);
        }
    }
}

void apply_collision(void) {
    int32_t radius = 175;

    delivery_collide_walls(&cam_x, &cam_z, radius,
                           player_on_upper_floor, player_on_ramp);
    crates_collide(&cam_x, cam_y, &cam_z, radius);
}

#ifdef DEBUG_COLLISION

extern GameState current_area;  /* the room the player is in — matches a fat
                                   door's tag. NOT game_state: that reads
                                   STATE_MENU with the inventory open and no
                                   door would match. See title.h. */

#define DBG_BLOCK_R 190   /* purple: everything that stops a shot */
#define DBG_BLOCK_G  40
#define DBG_BLOCK_B 220

/* Draw a solid box (4 sides + top) given its four footprint corners (world XZ,
   in ring order) and world top/bottom Y. Used to show the exact volumes the gun
   treats as solid. Projects like debug_draw_walls and shares its near-plane
   guard (a wild POLY_F4 locks the GPU). */
static void debug_fill_box(RenderContext *ctx,
                           const int32_t *cx, const int32_t *cz,
                           int32_t y_top, int32_t y_bot,
                           uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    SVECTOR v[8];
    DVECTOR sv[8];
    int32_t sz[8];
    int i, f;
    /* 0-3 bottom ring, 4-7 top ring. */
    for (i = 0; i < 4; i++) {
        v[i].vx   = (int16_t)cx[i]; v[i].vy   = (int16_t)y_bot; v[i].vz   = (int16_t)cz[i]; v[i].pad   = 0;
        v[i+4].vx = (int16_t)cx[i]; v[i+4].vy = (int16_t)y_top; v[i+4].vz = (int16_t)cz[i]; v[i+4].pad = 0;
    }
    for (i = 0; i < 8; i++) {
        gte_ldv0(&v[i]); gte_rtps(); gte_stsxy(&sv[i]); gte_stsz(&sz[i]);
        if (sz[i] == 0 ||
            sv[i].vx <= -1023 || sv[i].vx >= 1023 ||
            sv[i].vy <= -1023 || sv[i].vy >= 1023) return;   /* near/off-screen: skip box */
    }
    {
        static const uint8_t faces[5][4] = {
            {0,1,4,5}, {1,2,5,6}, {2,3,6,7}, {3,0,7,4},   /* four sides */
            {4,5,7,6},                                    /* top */
        };
        for (f = 0; f < 5; f++) {
            const uint8_t *q = faces[f];
            int32_t otz;
            gte_ldv0(&v[q[0]]); gte_rtps();
            gte_ldv0(&v[q[1]]); gte_rtps();
            gte_ldv0(&v[q[2]]); gte_rtps();
            gte_ldv0(&v[q[3]]); gte_rtps();
            gte_avsz4(); gte_stotz(&otz);
            if (otz <= 0 || otz >= OT_LENGTH) continue;
            if (ctx->next_packet + sizeof(POLY_F4) > buf_end) return;
            POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
            setPolyF4(poly);
            setRGB0(poly, r, g, b);
            poly->x0 = sv[q[0]].vx; poly->y0 = sv[q[0]].vy;
            poly->x1 = sv[q[1]].vx; poly->y1 = sv[q[1]].vy;
            poly->x2 = sv[q[2]].vx; poly->y2 = sv[q[2]].vy;
            poly->x3 = sv[q[3]].vx; poly->y3 = sv[q[3]].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F4);
        }
    }
}

/* Axis-aligned box corners in ring order (helper for the AABB props). */
static void dbg_aabb_corners(int32_t x, int32_t z, int32_t hx, int32_t hz,
                             int32_t *cx, int32_t *cz) {
    cx[0] = x - hx; cz[0] = z - hz;
    cx[1] = x + hx; cz[1] = z - hz;
    cx[2] = x + hx; cz[2] = z + hz;
    cx[3] = x - hx; cz[3] = z + hz;
}

/* Draw every shot-blocking prop as a purple box, matching each prop's
   *_point_solid volume exactly (real footprint + SHOT_PROP_SLACK, real height,
   world-space Y). Keep in lock-step with the point-solid tests in the prop
   modules — if one changes, change the other. */
static void debug_draw_shot_props(RenderContext *ctx) {
    int i;
    int32_t cx[4], cz[4];

    for (i = 0; i < crate_count; i++) {
        Crate *c = &crates[i];
        if (!c->active || c->state != CRATE_INTACT) continue;
        dbg_aabb_corners(c->x, c->z, c->half_w + SHOT_PROP_SLACK, c->half_d + SHOT_PROP_SLACK, cx, cz);
        debug_fill_box(ctx, cx, cz, c->y - CRATE_HALF_H, c->y + CRATE_HALF_H,
                       DBG_BLOCK_R, DBG_BLOCK_G, DBG_BLOCK_B);
    }
    for (i = 0; i < fatdoor_count; i++) {
        FatDoor *d = &fatdoors[i];
        if (!d->active || d->state != FATDOOR_INTACT || d->area != current_area) continue;
        dbg_aabb_corners(d->x, d->z, d->half_x + SHOT_PROP_SLACK, d->half_z + SHOT_PROP_SLACK, cx, cz);
        debug_fill_box(ctx, cx, cz, d->y - FATDOOR_HALF_H, d->y + FATDOOR_HALF_H,
                       DBG_BLOCK_R, DBG_BLOCK_G, DBG_BLOCK_B);
    }
    for (i = 0; i < dresser_count; i++) {
        Dresser *d = &dressers[i];
        if (!d->active) continue;
        int32_t c = icos(d->rot_y), s = isin(d->rot_y);
        if (c < 0) c = -c;
        if (s < 0) s = -s;
        int32_t hw = (d->half_w * c + d->half_d * s) >> 12;
        int32_t hd = (d->half_w * s + d->half_d * c) >> 12;
        int32_t base = d->y + GROUND_FLOOR_Y;
        dbg_aabb_corners(d->x, d->z, hw + SHOT_PROP_SLACK, hd + SHOT_PROP_SLACK, cx, cz);
        debug_fill_box(ctx, cx, cz, base - DRESSER_SOLID_H, base,
                       DBG_BLOCK_R, DBG_BLOCK_G, DBG_BLOCK_B);
    }
    for (i = 0; i < dining_table_count; i++) {
        DiningTable *t = &dining_tables[i];
        if (!t->active) continue;
        int32_t base = t->y + GROUND_FLOOR_Y;
        dbg_aabb_corners(t->x, t->z, t->half_w + SHOT_PROP_SLACK, t->half_d + SHOT_PROP_SLACK, cx, cz);
        debug_fill_box(ctx, cx, cz, base - DTABLE_TOP_REACH, base,
                       DBG_BLOCK_R, DBG_BLOCK_G, DBG_BLOCK_B);
    }
}

void debug_draw_walls(RenderContext *ctx) {
    if (debug_mode != 3) return;  /* heavy overdraw — only in full-debug (level 3) */

    /* Called from draw_player_systems right after the bullet-hit sprites, so the
       GTE still holds the scene's camera view matrix — project world coords
       directly, exactly as bullet_hits_draw does. */
    CollisionRoom *r = &current_collision_room;
    int i;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < r->wall_count; i++) {
        Wall *w = &r->walls[i];

        int32_t floor_y   = w->y_max;   /* bottom (least negative) */
        int32_t ceiling_y = w->y_min;   /* top (most negative)     */

        SVECTOR verts[4];
        verts[0].vx = (int16_t)w->x1; verts[0].vy = (int16_t)floor_y;   verts[0].vz = (int16_t)w->z1; verts[0].pad = 0;
        verts[1].vx = (int16_t)w->x2; verts[1].vy = (int16_t)floor_y;   verts[1].vz = (int16_t)w->z2; verts[1].pad = 0;
        verts[2].vx = (int16_t)w->x1; verts[2].vy = (int16_t)ceiling_y; verts[2].vz = (int16_t)w->z1; verts[2].pad = 0;
        verts[3].vx = (int16_t)w->x2; verts[3].vy = (int16_t)ceiling_y; verts[3].vz = (int16_t)w->z2; verts[3].pad = 0;

        DVECTOR sv[4];
        int32_t sz[4];
        int32_t otz;

        gte_ldv0(&verts[0]); gte_rtps(); gte_stsxy(&sv[0]); gte_stsz(&sz[0]);
        gte_ldv0(&verts[1]); gte_rtps(); gte_stsxy(&sv[1]); gte_stsz(&sz[1]);
        gte_ldv0(&verts[2]); gte_rtps(); gte_stsxy(&sv[2]); gte_stsz(&sz[2]);
        gte_ldv0(&verts[3]); gte_rtps(); gte_stsxy(&sv[3]); gte_stsz(&sz[3]);

        /* Skip walls that cross the near plane or project off-screen. An
           unclamped POLY_F4 with wild / out-of-range screen coords LOCKS the GPU
           (DrawSync never returns and the whole game freezes). The level renderer
           guards its geometry the same way; debug walls need it too because
           Reception's tall multi-level walls are the first to get close enough to
           trigger it — delivery/kitchen walls never do. */
        {
            int bad = 0, k;
            for (k = 0; k < 4; k++) {
                if (sz[k] == 0 ||
                    sv[k].vx <= -1023 || sv[k].vx >= 1023 ||
                    sv[k].vy <= -1023 || sv[k].vy >= 1023) { bad = 1; break; }
            }
            if (bad) continue;
        }

        gte_avsz4();
        gte_stotz(&otz);

        if (otz <= 0 || otz >= OT_LENGTH) continue;
        if (ctx->next_packet + sizeof(POLY_F4) > buf_end) continue;

        POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
        setPolyF4(poly);

        /* Purple: every collision wall is a shot blocker. */
        setRGB0(poly, DBG_BLOCK_R, DBG_BLOCK_G, DBG_BLOCK_B);

        poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
        poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
        poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
        poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;

        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
        ctx->next_packet += sizeof(POLY_F4);
    }

    /* Props that stop a shot, drawn as purple boxes matching the gun's solid
       tests. */
    debug_draw_shot_props(ctx);
}

static void debug_draw_digit(RenderContext *ctx, int digit, int sx, int sy) {
    static const uint8_t digit_bitmask[10][7] = {
        {0x1F,0x11,0x11,0x11,0x11,0x11,0x1F}, /* 0 */
        {0x04,0x06,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
        {0x1F,0x10,0x10,0x1F,0x01,0x01,0x1F}, /* 2 */
        {0x1F,0x10,0x10,0x1F,0x10,0x10,0x1F}, /* 3 */
        {0x11,0x11,0x11,0x1F,0x10,0x10,0x10}, /* 4 */
        {0x1F,0x01,0x01,0x1F,0x10,0x10,0x1F}, /* 5 */
        {0x1F,0x01,0x01,0x1F,0x11,0x11,0x1F}, /* 6 */
        {0x1F,0x10,0x10,0x10,0x10,0x10,0x10}, /* 7 */
        {0x1F,0x11,0x11,0x1F,0x11,0x11,0x1F}, /* 8 */
        {0x1F,0x11,0x11,0x1F,0x10,0x10,0x1F}, /* 9 */
    };
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int row, col;
    for (row = 0; row < 7; row++) {
        for (col = 0; col < 5; col++) {
            if (!(digit_bitmask[digit][row] & (0x10 >> col))) continue;
            if (ctx->next_packet + sizeof(TILE) > buf_end) return;
            TILE *t = (TILE *)ctx->next_packet;
            setTile(t);
            setXY0(t, sx + col*3, sy + row*3);
            setWH(t, 2, 2);
            setRGB0(t, 255, 255, 255);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[0], t);
            ctx->next_packet += sizeof(TILE);
        }
    }
}

static void debug_draw_number(RenderContext *ctx, int32_t num, int sx, int sy) {
    uint8_t buf[12];
    int len = 0, neg = 0;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    if (num < 0) { neg = 1; num = -num; }
    if (num == 0) { buf[len++] = 0; }
    while (num > 0) { buf[len++] = (uint8_t)(num % 10); num /= 10; }

    if (neg) {
        if (ctx->next_packet + sizeof(TILE) <= buf_end) {
            TILE *t = (TILE *)ctx->next_packet;
            setTile(t);
            setXY0(t, sx, sy + 9);
            setWH(t, 6, 2);
            setRGB0(t, 255, 255, 255);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[0], t);
            ctx->next_packet += sizeof(TILE);
        }
        sx += 8;
    }

    int i;
    for (i = len - 1; i >= 0; i--) {
        debug_draw_digit(ctx, buf[i], sx, sy);
        sx += 18;
    }
}

static void debug_draw_label(RenderContext *ctx, int sx, int sy,
                              uint8_t r, uint8_t g, uint8_t b, int w) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    TILE *t = (TILE *)ctx->next_packet;
    setTile(t);
    setXY0(t, sx, sy);
    setWH(t, w, 22);
    setRGB0(t, r, g, b);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[0], t);
    ctx->next_packet += sizeof(TILE);
}

void debug_draw_coords(RenderContext *ctx) {
    /* Level 2+, not level 1. This panel is the expensive half of the old
       overlay: a 320x30 backing tile and a seven-segment TILE per digit segment
       across four numbers. Level 1 is the perf meter alone, so that VB can be
       read without this panel changing the answer (see camera.c's Select note). */
    if (debug_mode != 2 && debug_mode != 3) return;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int sx = 8, sy = 8;

    if (ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *bg = (TILE *)ctx->next_packet;
        setTile(bg);
        setXY0(bg, 0, 0);
        setWH(bg, 320, 30);
        setRGB0(bg, 0, 0, 0);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[0], bg);
        ctx->next_packet += sizeof(TILE);
    }

    debug_draw_label(ctx, sx, sy+1, 200, 50, 50, 10); sx += 14;
    debug_draw_number(ctx, cam_x, sx, sy);             sx += 90;

    debug_draw_label(ctx, sx, sy+1, 50, 200, 50, 10); sx += 14;
    debug_draw_number(ctx, cam_y, sx, sy);             sx += 90;

    debug_draw_label(ctx, sx, sy+1, 50, 50, 200, 10); sx += 14;
    debug_draw_number(ctx, cam_z, sx, sy);             sx += 90;

    debug_draw_label(ctx, sx, sy+1, 150, 150, 50, 10); sx += 14;
    debug_draw_number(ctx, current_collision_room.wall_count, sx, sy);
}

#endif /* DEBUG_COLLISION */

/* Front-side-only wall collision for level 2.
 * Identical to collide_wall but with NO back-face rescue: a player who is
 * behind the wall (negative dot) is never pushed. Level 2's interior partitions
 * have coincident two-sided faces; the back-face rescue in collide_wall would
 * catapult the player across the partition and trap them oscillating between
 * the two faces. Each face here only blocks from its own (normal) side. */
static int collide_wall_frontonly(Wall *w, int32_t *px, int32_t *pz, int32_t radius) {
    int32_t dx = *px - w->x1;
    int32_t dz = *pz - w->z1;

    int32_t dot = ((dx >> 4) * (w->nx >> 4) + (dz >> 4) * (w->nz >> 4)) >> 4;
    if (dot >= radius) return 0;
    if (dot < 0)       return 0;  /* behind the wall — never push */

    int32_t wx      = (w->x2 - w->x1) >> 4;
    int32_t wz      = (w->z2 - w->z1) >> 4;
    int32_t dx_s    = dx >> 4;
    int32_t dz_s    = dz >> 4;
    int32_t along   = dx_s * wx + dz_s * wz;
    int32_t wlen_sq = wx * wx + wz * wz;
    if (along < 0 || along > wlen_sq) return 0;

    int32_t push = radius - dot;
    *px += (push * w->nx) >> 12;
    *pz += (push * w->nz) >> 12;
    return 1;
}

void apply_collision_kitchen_dining(void) {
    CollisionRoom *r = &current_collision_room;
    /* radius 125 (vs level 1's 175): holds the player well off the walls to
     * avoid near-plane poly clipping. Doorways stay passable because the wall
     * push only applies while the player projects onto a jamb segment (see the
     * along-segment reject in collide_wall_frontonly), not when crossing the
     * gap between jambs. */
    int32_t radius = 125;
    /* Props (tables) use a tighter radius than the walls: the big wall standoff
     * exists to keep the camera off large flat surfaces (near-plane clipping),
     * but tables are things you walk right up to. A smaller radius lets the
     * player get close and squeeze between tables placed near each other. */
    int32_t table_radius = 75;
    int i, pass;
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < r->wall_count; i++)
            collide_wall_frontonly(&r->walls[i], &cam_x, &cam_z, radius);
    fatdoors_collide(&cam_x, cam_y, &cam_z, radius);
    dining_tables_collide(&cam_x, cam_y, &cam_z, table_radius);
}

/* Front-side, Y-aware wall collision: like collide_wall_frontonly but only
 * pushes when the body's vertical span overlaps the wall's. body_top is the
 * head (most negative), body_bot the feet. Used by the multi-level reception so
 * a wall only blocks on its own floor (no full-Y-plane collision). */
static int collide_wall_frontonly_y(Wall *w, int32_t *px, int32_t *pz,
                                     int32_t body_top, int32_t body_bot,
                                     int32_t radius) {
    /* Vertical gate. y_min = wall top (most -ve), y_max = bottom. Intervals
     * overlap iff body_top <= y_max && y_min <= body_bot. A wall with no Y data
     * (y_min == y_max) is treated as full height (always overlaps). */
    if (w->y_min != w->y_max && (body_top > w->y_max || w->y_min > body_bot))
        return 0;

    int32_t dx = *px - w->x1;
    int32_t dz = *pz - w->z1;

    int32_t dot = ((dx >> 4) * (w->nx >> 4) + (dz >> 4) * (w->nz >> 4)) >> 4;
    if (dot >= radius) return 0;
    if (dot < 0)       return 0;  /* behind the wall — never push */

    int32_t wx      = (w->x2 - w->x1) >> 4;
    int32_t wz      = (w->z2 - w->z1) >> 4;
    int32_t dx_s    = dx >> 4;
    int32_t dz_s    = dz >> 4;
    int32_t along   = dx_s * wx + dz_s * wz;
    int32_t wlen_sq = wx * wx + wz * wz;
    if (along < 0 || along > wlen_sq) return 0;

    int32_t push = radius - dot;
    *px += (push * w->nx) >> 12;
    *pz += (push * w->nz) >> 12;
    return 1;
}

/* Reception: walls only, Y-aware (multi-level room). Does NOT collide the
 * kitchen's dining-table/fat-door props (those entities are global, not
 * room-scoped, so they'd otherwise act as invisible colliders here). */
void apply_collision_reception(void) {
    CollisionRoom *r = &current_collision_room;
    /* Holds the player well off the walls (stopped from farther away) to avoid
     * near-plane poly clipping. Doorways stay passable via the along-segment
     * reject in collide_wall_frontonly_y. Per room — see collision.h. */
    int32_t radius = wall_radius;
    /* Body vertical span: feet at the floor (cam_y + GROUND_FLOOR_Y), head a
     * little above the eye (cam_y). */
    int32_t body_bot = cam_y + GROUND_FLOOR_Y;
    int32_t body_top = cam_y - 30;
    int i, pass;
    /* Rafflesias go BEFORE the walls, and they are the only prop that does.
       Everything else here is placed in open floor, so pushing it after the
       walls is free; a rafflesia is planted in a flower bed and two of the three
       beds are tucked against a hedge, so its push circle overlaps solid
       geometry. Pushed last it would win, and shove the player INTO the hedge —
       measured: 949 of the standable cells around the east-lawn flower do
       exactly that at this radius. Pushed first, the wall passes below get the
       final say and the player is never left inside a hedge; the cost is that in
       that one tight pocket they may end up nearer the flower than its body
       radius asks. Hard world geometry beats a soft prop, every time.
       See RAF_BODY_RADIUS in rafflesia.c. */
    rafflesias_collide(&cam_x, cam_y, &cam_z, 75);
    /* Living statues go before the walls too, and for exactly the same reason:
       Maze Two's is planted on a plinth standing hard against a hedge run, so
       its push circle overlaps solid geometry. Pushed last it would shove the
       player into the hedge; pushed first, the wall passes below get the final
       say. Area-gated inside the module, so this is a no-op everywhere else.

       >>> AND IT GETS THE WALL RADIUS, NOT THE 75 EVERY OTHER PROP HERE GETS.
       <<< A statue must never be walkable-through, so it is collided like level
       geometry rather than like furniture: 195 + its own LST_BODY_RADIUS is a
       265 stop. That is past what a centre-based melee test could reach, which
       is why the crucifaxe measures to its surface — see LST_BODY_RADIUS. */
    living_statues_collide(&cam_x, cam_y, &cam_z, LST_WALL_LIKE_PUSH);
    /* Hadad, on the same terms and before the walls for the same reason: on the
       plinth his push circle sits inside that block's own collision box, and
       pushed last he would shove the player into the stone. He gets the WALL
       radius too — "a collision zone around him so the player can't walk
       through him" was asked for explicitly, and it is also what makes him a
       plug in a 600-wide corridor rather than something to squeeze past. His
       300 + 195 is a 495 stop, well past what a centre-based melee test could
       reach, which is why the crucifaxe measures to his surface. */
    hadads_collide(&cam_x, cam_y, &cam_z, HAD_WALL_LIKE_PUSH);
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < r->wall_count; i++)
            collide_wall_frontonly_y(&r->walls[i], &cam_x, &cam_z,
                                     body_top, body_bot, radius);
    /* Props use a tighter radius than the walls (walk right up to them), same as
       the kitchen's dining tables. */
    dressers_collide(&cam_x, cam_y, &cam_z, 75);
    /* Grinders; the module gates each instance to the area it was placed in, so
       this is a no-op everywhere but the Rear Gate. */
    grinders_collide(&cam_x, cam_y, &cam_z, 75);
    /* ...and the corridor gate's invisible backstop, immediately after them: it
       closes the machine's south mouth for as long as the pair are travelling,
       so a player cannot throw the lever at the top of the corridor and then
       outrun their own grinders through the closing gap. Gated to the Rear Gate
       inside the module, like every other prop here. See grinder_puzzle.h.

       AFTER grinders_collide, not before, because it is the tighter of the two
       and must have the last word: the grinders' own escape pass can spit a
       player out through that same south mouth, and it must not be able to spit
       them past this. (It cannot in practice — that pass only runs once nothing
       is moving, which is precisely when the backstop is down — but the ordering
       is what makes that a belt-and-braces argument rather than the only one.) */
    grinder_puzzle_collide(&cam_x, cam_y, &cam_z);
    /* Breakable door in the small-room doorway. Radius 125 (like the kitchen's
       doors) rather than the wide wall radius, so the player can walk up close
       enough to smash it. fatdoors_collide skips doors of other areas. */
    fatdoors_collide(&cam_x, cam_y, &cam_z, 125);
    /* Save point: solid, using its own mesh footprint (radius = player standoff). */
    save_points_collide(&cam_x, cam_y, &cam_z, 55);
    /* Piano-room props (this routine is shared with the piano room); the module
       gates itself to that area, so this is a no-op in reception. */
    piano_props_collide(&cam_x, cam_y, &cam_z, 75);
    /* Conservatory concrete props; likewise gated to that area. */
    concrete_props_collide(&cam_x, cam_y, &cam_z, 75);
    /* 2F hall chest of drawers; likewise gated to that area. */
    trick_drawers_collide(&cam_x, cam_y, &cam_z, 75);
    /* Attic Stairwell altar; likewise gated. Prop radius rather than the wall
       standoff so the player can reach the pickups on its top face. */
    attic_stairwell_altar_collide(&cam_x, cam_y, &cam_z, 75);
    /* Chainlink gate props; area-tagged, so this is a no-op elsewhere. Full wall
       standoff rather than a prop radius: it seals a gap in a fence run whose own
       panels are collision walls, and a tighter radius here would let the player
       squeeze past its edge into the cage. */
    chainlink_doors_collide(&cam_x, cam_y, &cam_z, 195);
    /* Wall levers; likewise area-tagged. Prop radius, not the wall standoff, as
       they are small chest-height fixtures; the module also gates them by the
       player's vertical span. */
    levers_collide(&cam_x, cam_y, &cam_z, 75);
    /* The Rabisu boss: solid, area-tagged like the props above so this is a
       no-op everywhere it is not placed. Prop radius rather than the wall
       standoff — the crucifaxe has to be able to reach it, and its own
       RBS_BODY_RADIUS already holds the player 170 off its centre. */
    rabisus_collide(&cam_x, cam_y, &cam_z, 75);
}

void apply_flat_entity_collision(int32_t *x, int32_t *z, int32_t radius) {
    CollisionRoom *r = &current_collision_room;
    int i, pass;
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < r->wall_count; i++)
            collide_wall_frontonly(&r->walls[i], x, z, radius);
}

/* Squared XZ distance from (px,pz) to the wall segment w. Coordinates stay
   within a few thousand units, so the products fit an int32 comfortably. */
static int32_t wall_dist2(const Wall *w, int32_t px, int32_t pz) {
    int32_t ex = w->x2 - w->x1, ez = w->z2 - w->z1;
    int32_t qx = w->x1,         qz = w->z1;
    int32_t len2 = ex * ex + ez * ez;
    if (len2 > 0) {
        int32_t t = (((px - w->x1) * ex + (pz - w->z1) * ez) << 12) / len2;
        if (t < 0)    t = 0;
        if (t > 4096) t = 4096;
        qx += (ex * t) >> 12;
        qz += (ez * t) >> 12;
    }
    int32_t dx = px - qx, dz = pz - qz;
    return dx * dx + dz * dz;
}

void collision_set_ceiling_y(int32_t y) {
    current_collision_room.ceiling_y = y;
}

void collision_set_wall_radius(int32_t r) {
    wall_radius = r > 0 ? r : COLLISION_WALL_RADIUS;
}

void collision_shoot_over_short_walls(int32_t max_height) {
    CollisionRoom *r = &current_collision_room;
    int i, n = r->wall_count > 32 ? 32 : r->wall_count;   /* the mask is 32 bits */
    r->shoot_over_mask = 0;
    for (i = 0; i < n; i++) {
        int32_t h = r->walls[i].y_max - r->walls[i].y_min;
        if (h < 0) h = -h;
        if (h <= max_height) r->shoot_over_mask |= 1u << i;
    }
}

int32_t collision_ceiling_y(int32_t x, int32_t z) {
    CollisionRoom *r = &current_collision_room;
    int32_t near_top = 0, room_top = 0;
    int     have_near = 0, have_room = 0;
    int     i;

    /* The room knows its own drawn ceiling: trust it over the proxy walls. */
    if (r->ceiling_y != 0) return r->ceiling_y;

    for (i = 0; i < r->wall_count; i++) {
        const Wall *w = &r->walls[i];
        /* y_min == y_max means the generator emitted no Y data for this face. */
        if (w->y_min == w->y_max) continue;
        if (!have_room || w->y_min < room_top) { room_top = w->y_min; have_room = 1; }
        if (wall_dist2(w, x, z) <= (int32_t)CEILING_PROBE_R * CEILING_PROBE_R) {
            if (!have_near || w->y_min < near_top) { near_top = w->y_min; have_near = 1; }
        }
    }

    if (have_near) return near_top;
    if (have_room) return room_top;
    return CEILING_DEFAULT_Y;
}

void apply_vampire_collision(void) {
    /* Mirror the player's floor-conditional wall logic using the vampire's
     * own floor state (set each frame by apply_vampire_height). */
    delivery_collide_walls(&vampire_x, &vampire_z, 100,
                           vampire_on_upper_floor, vampire_on_ramp);
}

/* -----------------------------------------------------------------------
 * Floor zones
 *
 * Coordinates taken from the condensed floor list in
 * delivery_area_mesh_collision.c:
 *   y=150   x(-1800..1800)   z(-1800..1800)   first yard
 *   y=150   x(-1400..-1000)  z(1800..2556)    corridor
 *   RAMP    x(-4194..-1711)  z(2556..3339)    150 (east) -> -524 (west)
 *   y=-524  x(-5451..-4194)  z(2556..5425)    upper platform
 *   y=150   x(-5451..-238)   z(2556..5425)    big yard, ground level
 *
 * cam_y = floor_surface_y - GROUND_FLOOR_Y  (0 = default camera height)
 * trans.vy in delivery_area_draw = -cam_y
 * ----------------------------------------------------------------------- */

FloorZone floor_zones[MAX_FLOOR_ZONES];
int       floor_zone_count      = 0;
int       player_on_upper_floor = 0;
int       player_on_ramp        = 0;
int       vampire_on_upper_floor = 0;
int       vampire_on_ramp        = 0;

void floor_zones_init(void) {
    int i = 0;

    /* First yard */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -1800; floor_zones[i].max_x = 1800;
    floor_zones[i].min_z = -1800; floor_zones[i].max_z = 1800;
    floor_zones[i].y     = 150;
    i++;

    /* Corridor */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -1400; floor_zones[i].max_x = -1000;
    floor_zones[i].min_z =  1800; floor_zones[i].max_z =  2556;
    floor_zones[i].y     = 150;
    i++;

    /* Ramp — one continuous slope along X (east = ground, west = upper floor).
     * The mesh subdivides it into 8 x-cells by 3 z-bands and skews the cells
     * very slightly in Z, so a single-axis ramp is a fit, not an exact match:
     * anchoring the ends at the two floors they join (150 at x=-1711, -524 at
     * x=-4194) keeps both joins seamless and leaves at most ~29 units of error
     * mid-slope, well inside the 40-unit float standoff apply_height applies. */
    floor_zones[i].type             = FLOOR_RAMP;
    floor_zones[i].min_x            = -4194; floor_zones[i].max_x = -1711;
    floor_zones[i].min_z            =  2556; floor_zones[i].max_z =  3339;
    floor_zones[i].ramp_y_start     =   150; /* Y at x=-1711 (east, ground) */
    floor_zones[i].ramp_y_end       =  -524; /* Y at x=-4194 (west, upper)  */
    floor_zones[i].ramp_axis_start  = -1711;
    floor_zones[i].ramp_axis_end    = -4194;
    floor_zones[i].ramp_along_x     = 1;
    i++;

    /* Upper platform. It sits directly over the west end of the ground floor,
     * so it MUST come before the ground catch-all below: apply_height walks the
     * list in order and skips any flat/upper zone that is above the player, so
     * a player underneath falls through to the ground zone while one up here
     * matches this one first (and gets player_on_upper_floor set).
     * max_x is -4194, the ramp's west end, so the two meet with no gap. */
    floor_zones[i].type  = FLOOR_UPPER;
    floor_zones[i].min_x = -5451; floor_zones[i].max_x = -4194;
    floor_zones[i].min_z =  2556; floor_zones[i].max_z =  5425;
    floor_zones[i].y     = -524;
    i++;

    /* Big yard, ground level — catch-all for everything z>2556 not covered
     * above, including the strip under the upper platform. The north edge of
     * the mesh tilts to y=165; 150 is within the float standoff. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -5451; floor_zones[i].max_x = -238;
    floor_zones[i].min_z =  2556; floor_zones[i].max_z =  5425;
    floor_zones[i].y     = 150;
    i++;

    floor_zone_count = i;
}

void apply_height(void) {
    int i;
    int32_t target = 0;

    player_on_upper_floor = 0;
    player_on_ramp        = 0;

    /* Find the floor Y (as a cam_y offset) directly below the player.
     * For flat/upper zones we skip any floor that is above the player —
     * that means the player is walking underneath it, not standing on it.
     * Ramp zones always apply so the player can ascend them. */
    for (i = 0; i < floor_zone_count; i++) {
        FloorZone *z = &floor_zones[i];
        if (cam_x < z->min_x || cam_x > z->max_x) continue;
        if (cam_z < z->min_z || cam_z > z->max_z) continue;

        if (z->type == FLOOR_FLAT || z->type == FLOOR_UPPER) {
            int32_t zone_target = z->y - GROUND_FLOOR_Y;
            /* Floor is above the player — keep searching for one below.
             * 10-unit tolerance covers the max ramp-edge discrepancy
             * (~4.4 units at slope 0.37 × max step 12) plus gravity tick. */
            if (zone_target < cam_y - 10) continue;
            target = zone_target;
            if (z->type == FLOOR_UPPER) player_on_upper_floor = 1;
            break;
        } else if (z->type == FLOOR_RAMP) {
            int32_t ramp_len = z->ramp_axis_end - z->ramp_axis_start;
            int32_t pos      = z->ramp_along_x ? cam_x : cam_z;
            int32_t t, dy, floor_y, ramp_target;
            if (ramp_len == 0) { target = z->ramp_y_start - GROUND_FLOOR_Y; player_on_ramp = 1; break; }
            t = ((pos - z->ramp_axis_start) << 12) / ramp_len;
            if (t <    0) t =    0;
            if (t > 4096) t = 4096;
            dy          = z->ramp_y_end - z->ramp_y_start;
            floor_y     = z->ramp_y_start + ((dy * t) >> 12);
            ramp_target = floor_y - GROUND_FLOOR_Y;

            /* A ramp surface well above the player is one they are walking
             * UNDERNEATH, not standing on — skip it and let a lower zone apply.
             * This used to be gated on cam_y > -50 ("only at ground level"),
             * which was enough when delivery's single ramp was the only one in
             * the game. The Garden Stairs stacks FIVE flights over the same XZ
             * strip, so the test has to work at any height; without it the first
             * matching flight in the list always won and the player fell through
             * to it. Delivery is unaffected: standing on a ramp puts its surface
             * ~40 units BELOW the eye, nowhere near the 150 threshold, and
             * walking under it still trips the skip exactly as before. */
            if (ramp_target < cam_y - 150) continue;

            target = ramp_target;
            player_on_ramp = 1;
            break;
        }
    }
    /* target == 0 when no zone found: fall to ground floor */

    /* Floor standoff: rest the eye 40 units ABOVE the floor surface (-Y is up)
     * so the player floats a touch off every floor. Applied to the final target
     * for all zone types; player only (NPCs use their own height functions). */
    target -= 40;

    /* Gravity — cam_y increases toward 0 (down in PS1 Y-down space). */
    cam_vy += GRAVITY;
    if (cam_vy > MAX_FALL_VEL) cam_vy = MAX_FALL_VEL;
    cam_y += cam_vy;

    /* Land: clamp when player reaches or passes through the floor. */
    if (cam_y >= target) {
        cam_y  = target;
        cam_vy = 0;
    }
}

void apply_vampire_height(void) {
    int i;
    int32_t target = 0;

    vampire_on_upper_floor = 0;
    vampire_on_ramp        = 0;

    for (i = 0; i < floor_zone_count; i++) {
        FloorZone *z = &floor_zones[i];
        if (vampire_x < z->min_x || vampire_x > z->max_x) continue;
        if (vampire_z < z->min_z || vampire_z > z->max_z) continue;

        if (z->type == FLOOR_FLAT || z->type == FLOOR_UPPER) {
            int32_t zone_target = z->y - GROUND_FLOOR_Y;
            if (zone_target < vampire_y - 2) continue;
            target = zone_target;
            if (z->type == FLOOR_UPPER) vampire_on_upper_floor = 1;
            break;
        } else if (z->type == FLOOR_RAMP) {
            int32_t ramp_len = z->ramp_axis_end - z->ramp_axis_start;
            int32_t pos      = z->ramp_along_x ? vampire_x : vampire_z;
            int32_t t, dy, floor_y, ramp_target;
            if (ramp_len == 0) { target = z->ramp_y_start - GROUND_FLOOR_Y; vampire_on_ramp = 1; break; }
            t = ((pos - z->ramp_axis_start) << 12) / ramp_len;
            if (t <    0) t =    0;
            if (t > 4096) t = 4096;
            dy          = z->ramp_y_end - z->ramp_y_start;
            floor_y     = z->ramp_y_start + ((dy * t) >> 12);
            ramp_target = floor_y - GROUND_FLOOR_Y;
            if (vampire_y > -50 && ramp_target < vampire_y - 150) continue;
            target = ramp_target;
            vampire_on_ramp = 1;
            break;
        }
    }

    vampire_vy += GRAVITY;
    if (vampire_vy > MAX_FALL_VEL) vampire_vy = MAX_FALL_VEL;
    vampire_y += vampire_vy;

    if (vampire_y >= target) {
        vampire_y  = target;
        vampire_vy = 0;
    }
}

void apply_ddog_collision(int32_t *x, int32_t *z, int on_upper_floor, int on_ramp) {
    CollisionRoom *r = &current_collision_room;
    int32_t radius = 80;
    int i, pass;

    /* Flat single-floor room (e.g. the conservatory): collide EVERY wall, front
       faces only, exactly like the zombies (apply_flat_entity_collision). The
       multi-level branch below hand-picks delivery-area wall indices and would
       ignore this room's other walls — letting a dog walk out through them and
       drop below the floor. */
    if (!r->multi_level) {
        for (pass = 0; pass < 2; pass++)
            for (i = 0; i < r->wall_count; i++)
                collide_wall_frontonly(&r->walls[i], x, z, radius);
        return;
    }

    /* Delivery area (multi-level): every wall, bar the two floor-conditional
       ones — same list the player and the vampire collide. */
    delivery_collide_walls(x, z, radius, on_upper_floor, on_ramp);
}

void apply_ddog_height(int32_t *px, int32_t *py, int32_t *pz,
                       int32_t *vy, int *on_upper_floor, int *on_ramp) {
    int i;
    int32_t target = 0;

    *on_upper_floor = 0;
    *on_ramp        = 0;

    for (i = 0; i < floor_zone_count; i++) {
        FloorZone *z = &floor_zones[i];
        if (*px < z->min_x || *px > z->max_x) continue;
        if (*pz < z->min_z || *pz > z->max_z) continue;

        if (z->type == FLOOR_FLAT || z->type == FLOOR_UPPER) {
            int32_t zone_target = z->y - GROUND_FLOOR_Y;
            if (zone_target < *py - 2) continue;
            target = zone_target;
            if (z->type == FLOOR_UPPER) *on_upper_floor = 1;
            break;
        } else if (z->type == FLOOR_RAMP) {
            int32_t ramp_len = z->ramp_axis_end - z->ramp_axis_start;
            int32_t pos      = z->ramp_along_x ? *px : *pz;
            int32_t t, dy, floor_y, ramp_target;
            if (ramp_len == 0) { target = z->ramp_y_start - GROUND_FLOOR_Y; *on_ramp = 1; break; }
            t = ((pos - z->ramp_axis_start) << 12) / ramp_len;
            if (t <    0) t =    0;
            if (t > 4096) t = 4096;
            dy          = z->ramp_y_end - z->ramp_y_start;
            floor_y     = z->ramp_y_start + ((dy * t) >> 12);
            ramp_target = floor_y - GROUND_FLOOR_Y;
            if (*py > -50 && ramp_target < *py - 150) continue;
            target = ramp_target;
            *on_ramp = 1;
            break;
        }
    }

    *vy += GRAVITY;
    if (*vy > MAX_FALL_VEL) *vy = MAX_FALL_VEL;
    *py += *vy;

    if (*py >= target) {
        *py = target;
        *vy = 0;
    }
}
