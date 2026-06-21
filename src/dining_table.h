#ifndef DINING_TABLE_H
#define DINING_TABLE_H

#include <stdint.h>
#include "render.h"

#define MAX_DINING_TABLES     8
#define DTABLE_PUSH_MARGIN    30   /* extra gap between player and table edge */

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

#endif
