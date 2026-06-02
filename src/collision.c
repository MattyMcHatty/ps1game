#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "collision.h"
#include "camera.h"
#include "vampire.h"
#include "level1_mesh_collision.h"

CollisionRoom current_collision_room;

void collision_init(void) {
    room_collision_init(&current_collision_room);
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

void apply_collision(void) {
    CollisionRoom *r = &current_collision_room;
    int32_t radius = 175;
    int i, pass;

    /* Wall layout (see level1_mesh_collision.c):
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
}

#ifdef DEBUG_COLLISION

void debug_draw_walls(RenderContext *ctx) {
    if (!debug_mode) return;

    CollisionRoom *r = &current_collision_room;
    int i;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < r->wall_count; i++) {
        Wall *w = &r->walls[i];

        int32_t floor_y   = w->y_floor;
        int32_t ceiling_y = w->y_ceil;

        SVECTOR verts[4];
        verts[0].vx = (int16_t)w->x1; verts[0].vy = (int16_t)floor_y;   verts[0].vz = (int16_t)w->z1; verts[0].pad = 0;
        verts[1].vx = (int16_t)w->x2; verts[1].vy = (int16_t)floor_y;   verts[1].vz = (int16_t)w->z2; verts[1].pad = 0;
        verts[2].vx = (int16_t)w->x1; verts[2].vy = (int16_t)ceiling_y; verts[2].vz = (int16_t)w->z1; verts[2].pad = 0;
        verts[3].vx = (int16_t)w->x2; verts[3].vy = (int16_t)ceiling_y; verts[3].vz = (int16_t)w->z2; verts[3].pad = 0;

        DVECTOR sv[4];
        int32_t otz;

        gte_ldv0(&verts[0]); gte_rtps(); gte_stsxy(&sv[0]);
        gte_ldv0(&verts[1]); gte_rtps(); gte_stsxy(&sv[1]);
        gte_ldv0(&verts[2]); gte_rtps(); gte_stsxy(&sv[2]);
        gte_ldv0(&verts[3]); gte_rtps(); gte_stsxy(&sv[3]);

        gte_avsz4();
        gte_stotz(&otz);

        if (otz <= 0 || otz >= OT_LENGTH) continue;
        if (ctx->next_packet + sizeof(POLY_F4) > buf_end) continue;

        POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
        setPolyF4(poly);

        if      (w->nx >  2048) setRGB0(poly, 255,  50,  50);
        else if (w->nx < -2048) setRGB0(poly,  50,  50, 255);
        else if (w->nz >  2048) setRGB0(poly,  50, 255,  50);
        else if (w->nz < -2048) setRGB0(poly, 255, 255,   0);
        else                    setRGB0(poly, 200, 200, 200);

        poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
        poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
        poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
        poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;

        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
        ctx->next_packet += sizeof(POLY_F4);
    }
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
 * level1_mesh_collision.c comments:
 *   FLOOR 0: y=149  x(-5451 to -238)   z(2557 to 5426)  big room
 *   FLOOR 1: y=149  x(-1400 to -1000)  z(1800 to 2557)  corridor
 *   FLOOR 2: y=149  x(-1800 to 1800)   z(-1799 to 1800) first room
 *   FLOOR 3: y=-200 x(-4148 to -2269)  z(2554 to 3054)  ramp surface
 *   FLOOR 4: y=-544 x(-5483 to -4143)  z(2527 to 5447)  upper floor
 *
 * cam_y = floor_surface_y - GROUND_FLOOR_Y  (0 = default camera height)
 * trans.vy in draw_scene = -cam_y
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
