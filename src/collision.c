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
    int32_t radius = 120;
    int i;

    /* Two passes resolves simultaneous corner collisions. */
    for (i = 0; i < r->wall_count; i++)
        collide_wall(&r->walls[i], &cam_x, &cam_z, radius);
    for (i = 0; i < r->wall_count; i++)
        collide_wall(&r->walls[i], &cam_x, &cam_z, radius);
}

void apply_vampire_collision(void) {
    CollisionRoom *r = &current_collision_room;
    int32_t radius = 100;
    int i;

    for (i = 0; i < r->wall_count; i++)
        collide_wall(&r->walls[i], &vampire_x, &vampire_z, radius);
}
