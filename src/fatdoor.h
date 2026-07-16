#ifndef FATDOOR_H
#define FATDOOR_H

#include <stdint.h>
#include "render.h"
#include "title.h"   /* GameState — each door is tagged with the area it lives in */

/* Breakable doors that fill the kitchen entryways. They render like crates
   (instanced SMD via smdSortModel) and smash like crates (hit with the
   crucifaxe). While intact they block the doorway; once smashed the gap opens. */

#define MAX_FATDOORS         8
#define FATDOOR_MAX_HEALTH   2     /* hits to destroy a door (no health bar) */
#define FATDOOR_SMASH_RANGE  300
#define FATDOOR_PUSH_MARGIN   55   /* keep the camera well clear of the thin door
                                      face so it never near-plane clips */
#define FATDOOR_HALF_H       188   /* half-height of the door model (375/2) */

typedef enum {
    FATDOOR_INTACT,
    FATDOOR_SMASHED,
} FatDoorState;

typedef struct {
    int32_t      x, y, z;
    int32_t      rot_y;
    int32_t      half_x, half_z;  /* world-axis collision half-widths */
    int32_t      health;          /* hits remaining before it smashes */
    FatDoorState state;
    int32_t      active;
    GameState    area;            /* which room this door belongs to; the doors are
                                     a single global array, so draw/collide/smash all
                                     skip doors whose area != the current game_state */
} FatDoor;

extern FatDoor fatdoors[MAX_FATDOORS];
extern int     fatdoor_count;

void fatdoors_load_assets(void);   /* LoadImage TIM + load SMD — STARTUP only */
void fatdoors_init(void);          /* place the kitchen doors */
void fatdoors_reset(void);
void fatdoors_draw(RenderContext *ctx);
void fatdoors_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);
/* Hitscan solid test: 1 if (x,y,z) is inside an intact door's real solid volume
   (true footprint + height, no push margin). For gun line-of-sight. */
int  fatdoors_point_solid(int32_t x, int32_t y, int32_t z, int32_t slack);
int  fatdoors_try_smash(void);

/* Damage the first intact door whose collision box (expanded by `reach`)
   contains (x,z). Used by enemies that batter the doors. Returns 0 = none in
   range, 1 = damaged, 2 = that hit smashed it. */
int  fatdoors_damage_at(int32_t x, int32_t z, int32_t reach, int amount);

#endif
