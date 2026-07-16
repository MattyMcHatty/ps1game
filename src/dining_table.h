#ifndef DINING_TABLE_H
#define DINING_TABLE_H

#include <stdint.h>
#include "render.h"

#define MAX_DINING_TABLES     8
#define DTABLE_PUSH_MARGIN    30   /* extra gap between player and table edge */
/* Height of the tabletop above the table's floor base, used by the gun's
   height-aware line-of-sight: a shot within this is blocked, one aimed above it
   passes over. Measured from the model (Dining Table.smx spans y=0 base to
   y=-115 top). */
#define DTABLE_TOP_REACH     115

/* A static, indestructible prop: the player collides with it but it has no
   state to change (no smashing, no items). Modelled on Crate, minus the
   gameplay behaviour. */
typedef struct {
    int32_t x, y, z;     /* y is the kitchen "standing" reference (e.g. -149);
                            world translate = y + GROUND_FLOOR_Y -> floor at 0 */
    int32_t rot_y;
    int32_t active;
    int32_t half_w;      /* XZ collision half-widths (tabletop footprint) */
    int32_t half_d;
} DiningTable;

extern DiningTable dining_tables[MAX_DINING_TABLES];
extern int         dining_table_count;

void dining_tables_init(void);
void dining_tables_draw(RenderContext *ctx, uint16_t tpage, uint16_t clut);
void dining_tables_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);
/* Hitscan solid test: 1 if (x,y,z) is inside a table's real solid volume
   (true footprint + height, no push margin). Height-aware, so a torso-height
   shot clears a low table but a low/angled shot strikes it. For gun LOS. */
int  dining_tables_point_solid(int32_t x, int32_t y, int32_t z, int32_t slack);

#endif
