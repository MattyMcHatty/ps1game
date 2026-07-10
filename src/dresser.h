#ifndef DRESSER_H
#define DRESSER_H

#include <stdint.h>
#include "render.h"

#define MAX_DRESSERS      8
#define DRESSER_PUSH_MARGIN 30   /* extra gap between player and dresser edge */
#define DRESSER_HALF_H     200   /* vertical collision reach around the dresser's
                                    floor reference (d->y). Less than a floor's
                                    height (>=150 step, ~600 to the upper floor) so
                                    the player can't collide with a dresser on the
                                    floor below/above them. */

/* A reusable, static, indestructible prop (like the dining table): the player
   collides with it but it has no state. It is textured with TWO textures:
     - wd_flr  : the room's already-resident wood-floor texture (reused, passed
                 into dressers_draw as tpage/clut, exactly like the dining table)
     - dresser : the prop's OWN texture, streamed into a spare VRAM slot at room
                 entry (owned by this module; see dresser_load_assets /
                 dresser_upload_texture).

   Load geometry + preload the dresser texture once at startup with
   dresser_load_assets(); upload the texture into VRAM on entering the room that
   uses it (dresser_upload_texture, pure LoadImage — call during the GPU-idle
   room transition). Place instances per area with dresser_add() from that area's
   init (after dressers_clear()), render from that area's draw with
   dressers_draw(), and collide from that area's apply_collision_* with
   dressers_collide().

   y is the room "standing on the floor" reference (e.g. -189 on reception's
   bottom floor); the draw adds GROUND_FLOOR_Y so the model's feet (authored at
   model y~0) rest on the floor, matching the other props. */
typedef struct {
    int32_t x, y, z;
    int32_t rot_y;       /* 0..4096 = full turn */
    int32_t active;
    int32_t half_w;      /* model-space XZ half-extents (footprint) */
    int32_t half_d;
} Dresser;

extern Dresser dressers[MAX_DRESSERS];
extern int     dresser_count;

void dresser_load_assets(void);      /* startup: load SMD + preload dresser TIM (CD reads) */
void dresser_upload_texture(void);   /* room entry: LoadImage the dresser TIM (no CD) */
void dressers_clear(void);           /* remove all instances (call in an area's init) */
int  dresser_add(int32_t x, int32_t y, int32_t z, int32_t rot_y);
/* wdflr_tpage/clut = the room's resident wood-floor texture (slot the dresser
   reuses for its non-drawer faces). The dresser's own texture is module-owned. */
void dressers_draw(RenderContext *ctx, uint16_t wdflr_tpage, uint16_t wdflr_clut);
void dressers_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

#endif
