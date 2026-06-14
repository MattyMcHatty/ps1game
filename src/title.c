#include <stdint.h>
#include <psxgpu.h>
#include <psxpad.h>
#include "render.h"
#include "title.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- Debug level-select menu (opened with Select on the title screen) ---- */

static int debug_menu_open   = 0;
static int debug_menu_cursor = 0;
static int level_select_fnt  = -1;

static const char *const level_names[] = {
    "DELIVERY AREA",
    "KITCHEN DINING",
};
#define LEVEL_SELECT_COUNT ((int)(sizeof(level_names) / sizeof(level_names[0])))

/* Target game state for each entry (index matches level_names). The kitchen
   loads via STATE_LOADING, which runs kitchen_dining_init then switches to
   STATE_KITCHEN_DINING. */
static const GameState level_states[LEVEL_SELECT_COUNT] = {
    STATE_DELIVERY_AREA,
    STATE_LOADING,
};

/* ---- Letter bitmasks: 7 rows x 5 cols, row 0 = top ---- */

static const uint8_t LETTER_H[7][5] = {
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,1,1,1,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
};

static const uint8_t LETTER_O[7][5] = {
    {0,1,1,1,0},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {0,1,1,1,0},
};

static const uint8_t LETTER_R[7][5] = {
    {1,1,1,1,0},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,1,1,1,0},
    {1,0,1,0,0},
    {1,0,0,1,0},
    {1,0,0,0,1},
};

static const uint8_t LETTER_P[7][5] = {
    {1,1,1,1,0},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,1,1,1,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
};

static const uint8_t LETTER_E[7][5] = {
    {1,1,1,1,1},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,1,1,1,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,1,1,1,1},
};

static const uint8_t LETTER_S[7][5] = {
    {0,1,1,1,1},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {0,1,1,1,0},
    {0,0,0,0,1},
    {0,0,0,0,1},
    {1,1,1,1,0},
};

static const uint8_t LETTER_T[7][5] = {
    {1,1,1,1,1},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,0,1,0,0},
};

static const uint8_t LETTER_A[7][5] = {
    {0,1,1,1,0},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,1,1,1,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
};

static const uint8_t LETTER_SPACE[7][5] = {
    {0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},
    {0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},
};

static const uint8_t LETTER_L[7][5] = {
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,1,1,1,1},
};

static const uint8_t LETTER_D[7][5] = {
    {1,1,1,1,0},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,1,1,1,0},
};

static const uint8_t LETTER_I[7][5] = {
    {1,1,1,1,1},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {0,0,1,0,0},
    {1,1,1,1,1},
};

static const uint8_t LETTER_N[7][5] = {
    {1,0,0,0,1},
    {1,1,0,0,1},
    {1,0,1,0,1},
    {1,0,0,1,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
};

static const uint8_t LETTER_G[7][5] = {
    {0,1,1,1,1},
    {1,0,0,0,0},
    {1,0,0,0,0},
    {1,0,1,1,1},
    {1,0,0,0,1},
    {1,0,0,0,1},
    {0,1,1,1,1},
};

static const uint8_t LETTER_DOT[7][5] = {
    {0,0,0,0,0},
    {0,0,0,0,0},
    {0,0,0,0,0},
    {0,0,0,0,0},
    {0,0,0,0,0},
    {0,1,1,0,0},
    {0,1,1,0,0},
};

/* ---- Helpers ---- */

typedef const uint8_t (*LetterPtr)[5];

static void draw_letter(
    RenderContext *ctx,
    LetterPtr letter,
    int32_t sx, int32_t sy,
    int32_t tile_size,
    uint8_t r, uint8_t g, uint8_t b
) {
    int row, col;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (row = 0; row < 7; row++) {
        for (col = 0; col < 5; col++) {
            if (!letter[row][col]) continue;
            if (ctx->next_packet + sizeof(TILE) > buf_end) return;

            TILE *tile = (TILE *)ctx->next_packet;
            setTile(tile);
            setXY0(tile, sx + col * tile_size, sy + row * tile_size);
            setWH(tile, tile_size - 1, tile_size - 1);
            setRGB0(tile, r, g, b);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[1], tile);
            ctx->next_packet += sizeof(TILE);
        }
    }
}

static LetterPtr char_to_letter(char c) {
    switch (c) {
        case 'H': return LETTER_H;
        case 'O': return LETTER_O;
        case 'R': return LETTER_R;
        case 'P': return LETTER_P;
        case 'E': return LETTER_E;
        case 'S': return LETTER_S;
        case 'T': return LETTER_T;
        case 'A': return LETTER_A;
        case 'L': return LETTER_L;
        case 'D': return LETTER_D;
        case 'I': return LETTER_I;
        case 'N': return LETTER_N;
        case 'G': return LETTER_G;
        case '.': return LETTER_DOT;
        default:  return LETTER_SPACE;
    }
}

static void draw_title_string(
    RenderContext *ctx,
    const char    *str,
    int32_t        sx, int32_t sy,
    int32_t        tile_size,
    uint8_t r, uint8_t g, uint8_t b
) {
    int32_t letter_width = 5 * tile_size + tile_size;
    int i = 0;
    while (str[i]) {
        draw_letter(ctx, char_to_letter(str[i]),
                    sx + i * letter_width, sy,
                    tile_size, r, g, b);
        i++;
    }
}

static void draw_press_start(RenderContext *ctx) {
    static const LetterPtr letters[11] = {
        LETTER_P, LETTER_R, LETTER_E, LETTER_S, LETTER_S,
        LETTER_SPACE,
        LETTER_S, LETTER_T, LETTER_A, LETTER_R, LETTER_T,
    };

    int32_t tile_size    = 4;
    int32_t letter_width = 5 * tile_size + tile_size;   /* 24 */
    int32_t total_width  = letter_width * 11;            /* 264 */
    int32_t start_x      = (SCREEN_XRES - total_width) / 2;
    int32_t start_y      = 165;
    int i;

    for (i = 0; i < 11; i++) {
        draw_letter(ctx, letters[i],
                    start_x + i * letter_width, start_y,
                    tile_size, 200, 200, 200);
    }
}

/* ---- Public functions ---- */

void draw_title(RenderContext *ctx) {
    static const LetterPtr horror[6] = {
        LETTER_H, LETTER_O, LETTER_R, LETTER_R, LETTER_O, LETTER_R,
    };

    static int32_t pulse      = 0;
    static int32_t title_flash = 0;

    int32_t tile_size    = 8;
    int32_t letter_width = 5 * tile_size + tile_size;   /* 48 */
    int32_t total_width  = letter_width * 6;             /* 288 */
    int32_t start_x      = (SCREEN_XRES - total_width) / 2;
    int32_t start_y      = 60;
    int i;

    pulse = (pulse + 2) & 255;
    uint8_t red = (uint8_t)(180 + ((pulse < 128) ? pulse / 2 : (255 - pulse) / 2));

    for (i = 0; i < 6; i++) {
        draw_letter(ctx, horror[i],
                    start_x + i * letter_width, start_y,
                    tile_size, red, 0, 0);
    }

    if (debug_menu_open) {
        int k;
        FntPrint(level_select_fnt, "LEVEL SELECT\n\n");
        for (k = 0; k < LEVEL_SELECT_COUNT; k++) {
            if (k == debug_menu_cursor)
                FntPrint(level_select_fnt, "* %s\n", level_names[k]);
            else
                FntPrint(level_select_fnt, "  %s\n", level_names[k]);
        }
        FntPrint(level_select_fnt, "\nX:LOAD  SEL:BACK");
        FntFlush(level_select_fnt);
    } else {
        title_flash++;
        if ((title_flash & 63) < 40)
            draw_press_start(ctx);
    }
}

void title_init(void) {
    /* Font window for the level-select list (8x16 glyphs), below the title. */
    level_select_fnt = FntOpen(96, 120, 160, 96, 0, 256);
}

void update_title(void) {
    if (!pad_buff_len[0]) return;
    PadResponse *pad = (PadResponse *)pad_buff[0];

    /* Edge-detect newly pressed buttons (pad bits are active-low). */
    static uint16_t prev_held = 0;
    uint16_t held    = ~pad->btn;
    uint16_t pressed = held & ~prev_held;
    prev_held = held;

    if (!debug_menu_open) {
        if (pressed & PAD_SELECT) {
            debug_menu_open   = 1;
            debug_menu_cursor = 0;
        } else if (pressed & PAD_START) {
            game_state = STATE_DELIVERY_AREA;
        }
        return;
    }

    /* Menu open: D-pad moves the cursor, X loads, Select backs out. */
    if (pressed & PAD_UP)
        debug_menu_cursor = (debug_menu_cursor + LEVEL_SELECT_COUNT - 1) % LEVEL_SELECT_COUNT;
    if (pressed & PAD_DOWN)
        debug_menu_cursor = (debug_menu_cursor + 1) % LEVEL_SELECT_COUNT;
    if (pressed & PAD_SELECT)
        debug_menu_open = 0;
    if (pressed & PAD_CROSS) {
        debug_menu_open = 0;
        game_state = level_states[debug_menu_cursor];
    }
}

void draw_loading_screen(RenderContext *ctx) {
    /* Full-screen red background */
    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setXY0(bg, 0, 0);
    setWH(bg, 320, 240);
    setRGB0(bg, 120, 0, 0);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
    ctx->next_packet += sizeof(TILE);

    int32_t tile_size    = 4;
    int32_t letter_width = 5 * tile_size + tile_size;  /* 24 */
    int32_t start_x      = (320 - (7 * letter_width)) / 2;
    int32_t start_y      = 100;
    draw_title_string(ctx, "LOADING", start_x, start_y, tile_size, 255, 255, 255);

    /* Animated dots centred below the word, cycling 0->1->2->3 every 30 frames */
    static int32_t dot_timer = 0;
    dot_timer++;
    int32_t dot_count = (dot_timer / 30) % 4;
    int32_t dot_y     = start_y + 7 * tile_size + tile_size * 2;
    int32_t dot_x     = (320 - (3 * letter_width)) / 2;
    if (dot_count >= 1) draw_title_string(ctx, ".", dot_x,                    dot_y, tile_size, 255, 255, 255);
    if (dot_count >= 2) draw_title_string(ctx, ".", dot_x + letter_width,     dot_y, tile_size, 255, 255, 255);
    if (dot_count >= 3) draw_title_string(ctx, ".", dot_x + letter_width * 2, dot_y, tile_size, 255, 255, 255);
}
