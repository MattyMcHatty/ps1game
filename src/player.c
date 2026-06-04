#include <stdint.h>
#include <psxgpu.h>
#include "render.h"
#include "player.h"

int32_t player_health = MAX_HEALTH;
int     game_over     = 0;
int     flash_timer   = 0;
int     damage_timer  = 0;
int     player_has_key = 0;

/* 5×7 pixel font, one byte per row, bit4 = leftmost column */
static const uint8_t hud_glyphs[][7] = {
    {0x1F,0x11,0x11,0x11,0x11,0x11,0x1F}, /* 0 */
    {0x04,0x06,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x1F,0x10,0x10,0x1F,0x01,0x01,0x1F}, /* 2 */
    {0x1F,0x10,0x10,0x1F,0x10,0x10,0x1F}, /* 3 */
    {0x11,0x11,0x11,0x1F,0x10,0x10,0x10}, /* 4 */
    {0x1F,0x01,0x01,0x1F,0x10,0x10,0x1F}, /* 5 */
    {0x1F,0x01,0x01,0x1F,0x11,0x11,0x1F}, /* 6 */
    {0x1F,0x10,0x10,0x10,0x10,0x10,0x10}, /* 7 */
    {0x1F,0x11,0x11,0x1F,0x11,0x11,0x1F}, /* 8 */
    {0x1F,0x11,0x11,0x1F,0x10,0x10,0x1F}, /* 9 */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x00,0x00,0x0E,0x11,0x1F,0x01,0x0E}, /* e */
    {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}, /* y */
    {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}, /* s */
    {0x00,0x04,0x04,0x00,0x04,0x04,0x00}, /* : */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
};

static int char_to_glyph(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == 'K') return 10;
    if (c == 'e') return 11;
    if (c == 'y') return 12;
    if (c == 's') return 13;
    if (c == ':') return 14;
    return 15; /* space / unknown */
}

static void hud_draw_string(RenderContext *ctx, const char *str,
                             int sx, int sy, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int cx = sx;
    int row, col;

    for (; *str; str++, cx += 12) {
        const uint8_t *glyph = hud_glyphs[char_to_glyph(*str)];
        for (row = 0; row < 7; row++) {
            for (col = 0; col < 5; col++) {
                if (!(glyph[row] & (0x10 >> col))) continue;
                if (ctx->next_packet + sizeof(TILE) > buf_end) return;
                TILE *t = (TILE *)ctx->next_packet;
                setTile(t);
                setXY0(t, cx + col * 2, sy + row * 2);
                setWH(t, 2, 2);
                setRGB0(t, r, g, b);
                addPrim(&ctx->buffers[ctx->active_buffer].ot[0], t);
                ctx->next_packet += sizeof(TILE);
            }
        }
    }
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

    /* Key counter — build "Keys: N" string */
    char key_str[10];
    int  pos = 0;
    int  k   = player_has_key;
    key_str[pos++] = 'K';
    key_str[pos++] = 'e';
    key_str[pos++] = 'y';
    key_str[pos++] = 's';
    key_str[pos++] = ':';
    key_str[pos++] = ' ';
    if (k >= 10) key_str[pos++] = '0' + (k / 10);
    key_str[pos++] = '0' + (k % 10);
    key_str[pos]   = '\0';

    /* Dark background behind key text (7 chars max × 12px wide, 14px tall) */
    int text_w = pos * 12;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    TILE *kbg = (TILE *)ctx->next_packet;
    setTile(kbg);
    setRGB0(kbg, 40, 40, 40);
    setXY0(kbg, 3, 19);
    setWH(kbg, text_w + 2, 16);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[1], kbg);
    ctx->next_packet += sizeof(TILE);

    hud_draw_string(ctx, key_str, 4, 20, 255, 220, 50);
}
