#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include "render.h"
#include "camera.h"        /* cam_x, cam_y, cam_z */
#include "collision.h"     /* GROUND_FLOOR_Y */
#include "sound.h"         /* SFX_HURT */
#include "player.h"        /* player_hurt, player_health, game_over, flash_timer */
#include "lava_floors.h"
#include "lava.h"

/* Chapter 4's burning lava - see lava.h. */

/* The feet: apply_height rests the eye GROUND_FLOOR_Y + 40 above the floor
   it stands on, so cam_y + LAVA_EYE_TO_FEET is the floor under the player.
   Within LAVA_FEET_TOL of the lava's plane (or below it) is "in" it; a floor
   any higher is a bridge. */
#define LAVA_EYE_TO_FEET  (GROUND_FLOOR_Y + 40)
#define LAVA_FEET_TOL      20

/* The pulse, in the 0..255 of an additive TILE: LAVA_FLASH_PEAK on the frame
   a point is lost, falling to LAVA_FLASH_BASE by the next one. Green is
   LAVA_FLASH_G_NUM/8 of red - orange, not red, so it never reads as the
   game-over flash. */
#define LAVA_FLASH_BASE    40
#define LAVA_FLASH_PEAK   144
#define LAVA_FLASH_G_NUM    3
#define OT_LAVA_TINT        3   /* src/player.c's OT_POISON_TINT: both additive */

static int lava_in    = 0;   /* standing in it this frame                  */
static int lava_ticks = 0;   /* frames since the last point, 0..TICK-1     */

void lava_reset(void) {
    lava_in    = 0;
    lava_ticks = 0;
}

int lava_burning(void) {
    return lava_in;
}

static const LavaFloor *lava_floor_for(GameState area) {
    int i;
    for (i = 0; i < LAVA_FLOOR_COUNT; i++)
        if (lava_floors[i].area == area)
            return lava_floors[i].bits ? &lava_floors[i] : NULL;
    return NULL;
}

/* Off the mask's own rectangle is simply "no". */
static int lava_cell(const LavaFloor *f, int32_t x, int32_t z) {
    int32_t u = x - f->x0, v = z - f->z0;
    if (u < 0 || v < 0) return 0;
    u /= LAVA_CELL;
    v /= LAVA_CELL;
    if (u >= f->w || v >= f->h) return 0;
    return (f->bits[v * f->stride + (u >> 3)] >> (u & 7)) & 1;
}

void lava_update(GameState area) {
    const LavaFloor *f = lava_floor_for(area);
    int in = f && !game_over &&
             cam_y + LAVA_EYE_TO_FEET >= f->y - LAVA_FEET_TOL &&
             lava_cell(f, cam_x, cam_z);

    if (!in) {
        lava_reset();
        return;
    }
    if (!lava_in) {
        sound_play(SFX_HURT);
        lava_ticks = 0;
    }
    lava_in = 1;

    if (lava_ticks == 0) player_hurt(1);
    if (++lava_ticks >= LAVA_TICK_FRAMES) lava_ticks = 0;

    /* Death is the caller's job everywhere else in the game, so it is here too
       - the grinder's three lines. */
    if (player_health <= 0) {
        player_health = 0;
        game_over     = 1;
        flash_timer   = 90;
    }
}

/* Built exactly as player_draw_status_overlay's poison wash: TILE first, then
   the DR_TPAGE that sets ABR=1 (LIFO within one OT node, so the GPU sees the
   blend mode before the tile). Additive, so it tints without darkening. */
void lava_draw_overlay(RenderContext *ctx) {
    if (!lava_in) return;

    /* lava_ticks was just advanced past the point it paid, so 1 is the
       freshest frame and 0 the oldest (the one before the next point). */
    int age = (lava_ticks + LAVA_TICK_FRAMES - 1) % LAVA_TICK_FRAMES;
    int r   = LAVA_FLASH_PEAK -
              ((LAVA_FLASH_PEAK - LAVA_FLASH_BASE) * age) / (LAVA_TICK_FRAMES - 1);
    int g   = (r * LAVA_FLASH_G_NUM) >> 3;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *t = (TILE *)ctx->next_packet;
        setTile(t);
        setSemiTrans(t, 1);
        setRGB0(t, r, g, 0);
        setXY0(t, 0, 0);
        setWH(t, SCREEN_XRES, SCREEN_YRES);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LAVA_TINT], t);
        ctx->next_packet += sizeof(TILE);
    }
    if (ctx->next_packet + sizeof(DR_TPAGE) <= buf_end) {
        DR_TPAGE *dp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(dp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LAVA_TINT], dp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
}
