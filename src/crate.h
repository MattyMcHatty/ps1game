#ifndef CRATE_H
#define CRATE_H

#include <stdint.h>
#include "render.h"

#define MAX_CRATES          16
#define BAT_SMASH_RANGE     260
#define CRATE_PUSH_MARGIN    80  /* extra gap between player and crate face */
#define CRATE_HALF_H        112  /* half-height of crate model in world units */

typedef enum {
    CRATE_INTACT,
    CRATE_SMASHED,
} CrateState;

typedef enum {
    ITEM_NONE,
    ITEM_MEDIPAC,
    ITEM_KEY,
} CrateItem;

typedef struct {
    int32_t    x, y, z;
    int32_t    rot_y;
    CrateState state;
    CrateItem  item;
    int32_t    active;
    int32_t    half_w;   /* XZ collision half-widths */
    int32_t    half_d;
} Crate;

extern Crate crates[MAX_CRATES];
extern int   crate_count;

void crates_init(void);
void crates_reset(void);
void crates_update(void);
void crates_draw(RenderContext *ctx);
void crates_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);
int  crate_try_smash(void);

#endif
