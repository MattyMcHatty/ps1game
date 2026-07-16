#include <stdint.h>
#include <psxgpu.h>
#include "render.h"
#include "player.h"
#include "camera.h"
#include "title.h"

int32_t player_health = MAX_HEALTH;
int     game_over     = 0;
int     flash_timer   = 0;
int     damage_timer  = 0;
int     player_keys   = 0;
int     player_weapons = (1 << WEAPON_CRUCIFAXE);  /* crucifaxe always owned */
int     player_rounds  = 0;
int     graveolver_loaded = GRAVEOLVER_CAPACITY;   /* cylinder starts loaded */
WeaponType current_weapon = WEAPON_CRUCIFAXE;

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

/* 3x5 bitmap digits (bit2 = leftmost pixel of each row). */
static const uint8_t hud_digit_font[10][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
    {7,4,7,1,7},{7,4,7,5,7},{7,1,2,4,4},{7,5,7,5,7},{7,5,7,1,7},
};

/* Draw an unsigned number as scaled pixels, top-left at (x, y). */
static void hud_draw_number(RenderContext *ctx, int value, int x, int y, int scale,
                            uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    char buf[8];
    int  n = 0, i, digit_w = 3 * scale, gap = scale;
    if (value < 0) value = 0;
    if (value == 0) buf[n++] = 0;
    else { int v = value; while (v > 0 && n < 7) { buf[n++] = (char)(v % 10); v /= 10; } }
    for (i = 0; i < n; i++) {
        int digit = buf[i];
        int x0 = x + (n - 1 - i) * (digit_w + gap);
        int row, col;
        for (row = 0; row < 5; row++)
            for (col = 0; col < 3; col++)
                if (hud_digit_font[digit][row] & (4 >> col)) {
                    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
                    TILE *t = (TILE *)ctx->next_packet;
                    setTile(t);
                    setRGB0(t, r, g, b);
                    setXY0(t, x0 + col * scale, y + row * scale);
                    setWH(t, scale, scale);
                    addPrim(&ctx->buffers[ctx->active_buffer].ot[0], t);
                    ctx->next_packet += sizeof(TILE);
                }
    }
}

void draw_hud(RenderContext *ctx) {
    if (game_state == STATE_MENU) return;
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

    /* Stamina bar background */
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    TILE *sbg = (TILE *)ctx->next_packet;
    setTile(sbg);
    setRGB0(sbg, 40, 40, 40);
    setXY0(sbg, 4, 16);
    setWH(sbg, 102, 10);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[1], sbg);
    ctx->next_packet += sizeof(TILE);

    /* Stamina bar fill — width proportional to remaining sprint stamina */
    uint16_t stamina_w = (uint16_t)((sprint_stamina * 100) / SPRINT_STAMINA_MAX);
    if (stamina_w > 0) {
        if (ctx->next_packet + sizeof(TILE) > buf_end) return;
        TILE *sbar = (TILE *)ctx->next_packet;
        setTile(sbar);
        if (sprint_cooldown)
            setRGB0(sbar, 200, 0, 0);   /* red while locked and refilling */
        else
            setRGB0(sbar, 0, 200, 0);   /* green when sprint is available */
        setXY0(sbar, 5, 17);
        setWH(sbar, stamina_w, 8);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[0], sbar);
        ctx->next_packet += sizeof(TILE);
    }

    /* Grave-olver cylinder count, under the bars (only once the gun is owned). */
    if (player_weapons & (1 << WEAPON_GRAVEOLVER))
        hud_draw_number(ctx, graveolver_loaded, 5, 29, 2, 255, 220, 0);
}
