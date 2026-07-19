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
#include "fatdoor.h"
#include "save_point.h"
#include "piano_props.h"

CollisionRoom current_collision_room;

void collision_init(void) {
    delivery_area_collision_init(&current_collision_room);
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
    if (dining_tables_point_solid(x, y, z, SHOT_PROP_SLACK)) return 1;
    if (piano_props_point_solid(x, y, z, SHOT_PROP_SLACK))   return 1;
    return 0;
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
       at least one sample lands inside. Firing is infrequent, so the extra
       samples are free. */
    {
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

void apply_collision(void) {
    CollisionRoom *r = &current_collision_room;
    int32_t radius = 175;
    int i, pass;

    /* Wall layout (see delivery_area_mesh_collision.c):
     *   0-12  regular room walls — always active
     *   13    upper-floor bannister — only when player_on_upper_floor
     *   14    under-ramp barrier along Z (at ramp midpoint X=-3208)
     *   15    under-ramp barrier along X (north edge Z=3054 to ramp start X=-2269)
     *         walls 14 & 15 active only when !player_on_ramp && !player_on_upper_floor */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < 13; i++)
            collide_wall(&r->walls[i], &cam_x, &cam_z, radius);
        if (player_on_upper_floor)
            collide_wall(&r->walls[13], &cam_x, &cam_z, radius);
        if (!player_on_ramp && !player_on_upper_floor) {
            collide_wall(&r->walls[14], &cam_x, &cam_z, radius);
            collide_wall(&r->walls[15], &cam_x, &cam_z, radius);
        }
    }
    crates_collide(&cam_x, cam_y, &cam_z, radius);
}

#ifdef DEBUG_COLLISION

extern GameState game_state;   /* current area — matches a fat door's tag */

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
        if (!d->active || d->state != FATDOOR_INTACT || d->area != game_state) continue;
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
    if (debug_mode < 2) return;   /* heavy overdraw — only in full-debug (level 2) */

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
    if (!debug_mode) return;

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
    /* radius 195: holds the player well off the walls (stopped from farther
     * away) to avoid near-plane poly clipping. Doorways stay passable via the
     * along-segment reject in collide_wall_frontonly_y. */
    int32_t radius = 195;
    /* Body vertical span: feet at the floor (cam_y + GROUND_FLOOR_Y), head a
     * little above the eye (cam_y). */
    int32_t body_bot = cam_y + GROUND_FLOOR_Y;
    int32_t body_top = cam_y - 30;
    int i, pass;
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < r->wall_count; i++)
            collide_wall_frontonly_y(&r->walls[i], &cam_x, &cam_z,
                                     body_top, body_bot, radius);
    /* Props use a tighter radius than the walls (walk right up to them), same as
       the kitchen's dining tables. */
    dressers_collide(&cam_x, cam_y, &cam_z, 75);
    /* Breakable door in the small-room doorway. Radius 125 (like the kitchen's
       doors) rather than the wide wall radius, so the player can walk up close
       enough to smash it. fatdoors_collide skips doors of other areas. */
    fatdoors_collide(&cam_x, cam_y, &cam_z, 125);
    /* Save point: solid, using its own mesh footprint (radius = player standoff). */
    save_points_collide(&cam_x, cam_y, &cam_z, 55);
    /* Piano-room props (this routine is shared with the piano room); the module
       gates itself to that area, so this is a no-op in reception. */
    piano_props_collide(&cam_x, cam_y, &cam_z, 75);
}

void apply_flat_entity_collision(int32_t *x, int32_t *z, int32_t radius) {
    CollisionRoom *r = &current_collision_room;
    int i, pass;
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < r->wall_count; i++)
            collide_wall_frontonly(&r->walls[i], x, z, radius);
}

void apply_vampire_collision(void) {
    CollisionRoom *r = &current_collision_room;
    int32_t radius = 100;
    int i, pass;

    /* Mirror the player's floor-conditional wall logic using the vampire's
     * own floor state (set each frame by apply_vampire_height). */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < 13; i++)
            collide_wall(&r->walls[i], &vampire_x, &vampire_z, radius);
        if (vampire_on_upper_floor)
            collide_wall(&r->walls[13], &vampire_x, &vampire_z, radius);
        if (!vampire_on_ramp && !vampire_on_upper_floor) {
            collide_wall(&r->walls[14], &vampire_x, &vampire_z, radius);
            collide_wall(&r->walls[15], &vampire_x, &vampire_z, radius);
        }
    }
}

/* -----------------------------------------------------------------------
 * Floor zones
 *
 * Coordinates taken from the script-detected floor data in
 * delivery_area_mesh_collision.c comments:
 *   FLOOR 0: y=149  x(-5451 to -238)   z(2557 to 5426)  big room
 *   FLOOR 1: y=149  x(-1400 to -1000)  z(1800 to 2557)  corridor
 *   FLOOR 2: y=149  x(-1800 to 1800)   z(-1799 to 1800) first room
 *   FLOOR 3: y=-200 x(-4148 to -2269)  z(2554 to 3054)  ramp surface
 *   FLOOR 4: y=-544 x(-5483 to -4143)  z(2527 to 5447)  upper floor
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

    /* First room */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -1800; floor_zones[i].max_x = 1800;
    floor_zones[i].min_z = -1800; floor_zones[i].max_z = 1800;
    floor_zones[i].y     = 149;
    i++;

    /* Corridor */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -1400; floor_zones[i].max_x = -1000;
    floor_zones[i].min_z =  1800; floor_zones[i].max_z =  2557;
    floor_zones[i].y     = 149;
    i++;

    /* Big room — right section east of ramp, ground level */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -2269; floor_zones[i].max_x = -238;
    floor_zones[i].min_z =  2557; floor_zones[i].max_z = 5426;
    floor_zones[i].y     = 149;
    i++;

    /* Ramp — runs along the X axis (east=ground, west=upper) */
    floor_zones[i].type             = FLOOR_RAMP;
    floor_zones[i].min_x            = -4148; floor_zones[i].max_x = -2269;
    floor_zones[i].min_z            =  2554; floor_zones[i].max_z =  3054;
    floor_zones[i].ramp_y_start     =   149; /* Y at x=-2269 (east, ground) */
    floor_zones[i].ramp_y_end       =  -544; /* Y at x=-4148 (west, upper)  */
    floor_zones[i].ramp_axis_start  = -2269;
    floor_zones[i].ramp_axis_end    = -4148;
    floor_zones[i].ramp_along_x     = 1;
    i++;

    /* Upper floor — max_x is -4149 (one unit west of the ramp end at -4148)
     * so the ramp top itself is not inside this zone. Stepping sideways off
     * the ramp before crossing that boundary causes a fall, not a snap. */
    floor_zones[i].type  = FLOOR_UPPER;
    floor_zones[i].min_x = -5483; floor_zones[i].max_x = -4149;
    floor_zones[i].min_z =  2527; floor_zones[i].max_z =  5447;
    floor_zones[i].y     = -544;
    i++;

    /* Big room catch-all — ground level fallback for areas not covered above */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -5451; floor_zones[i].max_x = -238;
    floor_zones[i].min_z =  2557; floor_zones[i].max_z =  5426;
    floor_zones[i].y     = 149;
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

            /* If the player is at ground level and the ramp surface is more
             * than 150 cam_y units above them, they are walking underneath
             * the ramp — skip it and let the catch-all ground zone apply. */
            if (cam_y > -50 && ramp_target < cam_y - 150) continue;

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

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < 13; i++)
            collide_wall(&r->walls[i], x, z, radius);
        if (on_upper_floor)
            collide_wall(&r->walls[13], x, z, radius);
        if (!on_ramp && !on_upper_floor) {
            collide_wall(&r->walls[14], x, z, radius);
            collide_wall(&r->walls[15], x, z, radius);
        }
    }
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
