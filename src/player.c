#include <stdint.h>
#include <psxgpu.h>
#include "render.h"
#include "player.h"

int32_t player_health = MAX_HEALTH;
int     game_over     = 0;
int     flash_timer   = 0;
int     damage_timer  = 0;
int     player_keys   = 0;

PickupEntry pickup_log[PICKUP_MSG_COUNT] = {{{""},0},{{""},0},{{""},0}};

void show_pickup_msg(const char *item_name) {
    const char *prefix = "Picked up ";
    int i, j;

    /* Shift entries up — oldest (slot 0) is discarded */
    pickup_log[0] = pickup_log[1];
    pickup_log[1] = pickup_log[2];

    /* Build new message in bottom slot */
    i = 0; j = 0;
    while (prefix[i] && i < 63)     { pickup_log[2].msg[i] = prefix[i]; i++; }
    while (item_name[j] && i < 63)  { pickup_log[2].msg[i++] = item_name[j++]; }
    pickup_log[2].msg[i] = '\0';
    pickup_log[2].timer  = PICKUP_MSG_DURATION;
}

void draw_hud(RenderContext *ctx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    /* Health bar background */
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setRGB0(bg, 40, 40, 40);
    setXY0(bg, 4, 4);
    setWH(bg, 102, 10);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[1], bg);
    ctx->next_packet += sizeof(TILE);

    /* Health bar fill */
    if (player_health > 0) {
        if (ctx->next_packet + sizeof(TILE) > buf_end) return;
        TILE *bar = (TILE *)ctx->next_packet;
        setTile(bar);
        setRGB0(bar, 0, 0, 200);
        setXY0(bar, 5, 5);
        setWH(bar, (uint16_t)player_health, 8);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[0], bar);
        ctx->next_packet += sizeof(TILE);
    }
}
