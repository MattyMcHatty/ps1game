#include <stdint.h>
#include <psxgpu.h>
#include "render.h"
#include "btn_glyph.h"

/* 5x7 bitmaps, LSB-first rows (bit 0 = leftmost column), matching the door
   font so the world-space renderers can use them directly. All four shapes are
   horizontally symmetric, so mirrored door signs render them correctly. */
static const uint8_t glyph_circle[7]   = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
static const uint8_t glyph_cross[7]    = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11};
static const uint8_t glyph_square[7]   = {0x1F,0x11,0x11,0x11,0x11,0x11,0x1F};
static const uint8_t glyph_triangle[7] = {0x04,0x04,0x0A,0x0A,0x11,0x11,0x1F};

const uint8_t *btn_glyph_lookup(char c, uint8_t *r, uint8_t *g, uint8_t *b) {
    switch (c) {
        case BTN_CIRCLE_CH:   *r = 235; *g =  75; *b =  65; return glyph_circle;
        case BTN_CROSS_CH:    *r = 120; *g = 190; *b = 255; return glyph_cross;
        case BTN_SQUARE_CH:   *r = 255; *g = 120; *b = 190; return glyph_square;
        case BTN_TRIANGLE_CH: *r =  80; *g = 210; *b = 120; return glyph_triangle;
        default: return 0;
    }
}

/* One coloured font pixel as a 1x1 TILE. */
static void px(RenderContext *ctx, int x, int y,
               uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    TILE *t = (TILE *)ctx->next_packet;
    setTile(t);
    setXY0(t, x, y);
    setWH(t, 1, 1);
    setRGB0(t, r, g, b);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[ot_idx], t);
    ctx->next_packet += sizeof(TILE);
}

void btn_prompt_draw(RenderContext *ctx, int x, int y, const char *s, int ot_idx) {
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    char run[64];
    int  rl = 0;
    int  cx = x, run_x = x;

    const char *p = s;
    for (;;) {
        char c = *p++;
        uint8_t r, g, b;
        const uint8_t *glyph = c ? btn_glyph_lookup(c, &r, &g, &b) : 0;

        if (c == '\0' || glyph) {
            /* Flush the pending text run before the glyph / end of string. */
            if (rl) {
                run[rl] = '\0';
                ctx->next_packet = FntSort(&ot[ot_idx], ctx->next_packet,
                                           run_x, y, run);
                rl = 0;
            }
            if (c == '\0') break;

            /* Button glyph: 5x7 pixels, same cell advance as the 8px font. */
            int row, col;
            for (row = 0; row < 7; row++)
                for (col = 0; col < 5; col++)
                    if (glyph[row] & (0x01 << col))
                        px(ctx, cx + col, y + row, r, g, b, ot_idx);
            cx   += 8;
            run_x = cx;
        } else {
            if (rl < (int)sizeof(run) - 1) run[rl++] = c;
            cx += 8;
        }
    }
}
