#ifndef CRATE_H
#define CRATE_H

#include <stdint.h>
#include "render.h"

#define MAX_CRATES       16
#define CRATE_SMASH_RADIUS 250

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
} Crate;

extern Crate crates[MAX_CRATES];
extern int   crate_count;

void crates_init(void);
void crates_reset(void);
void crates_update(void);
void crates_draw(RenderContext *ctx);
int  crate_try_smash(void);

#endif
