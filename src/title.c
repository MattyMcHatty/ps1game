#include <stdint.h>
#include <psxgpu.h>
#include <psxpad.h>
#include "render.h"
#include "title.h"
#include "btn_glyph.h"
#include "memcard.h"
#include "savegame.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- Debug level-select menu (opened with Select on the title screen) ---- */

static int debug_menu_open   = 0;
static int debug_menu_cursor = 0;
static int level_select_fnt  = -1;

/* ---- Start menu (opened with Start): New Game / Load Game ------------------
   Load Game walks card slot -> save file, reads the chosen SaveData, stages it
   (savegame_stage_load) and routes into the saved area exactly like the level
   select does; main.c applies the staged state once the area is initialised. */
enum { TM_CLOSED, TM_MAIN, TM_CARD, TM_FILE };
static int tmenu        = TM_CLOSED;
static int tmenu_cursor = 0;
static int tmenu_port   = 0;                 /* chosen card slot (0/1) */
static const char *tmenu_msg = 0;            /* inline error line, if any */
static SaveSlotInfo tmenu_slots[SAVE_MAX_SLOTS];
static int tmenu_slot_count = 0;
static int load_list_fnt    = -1;            /* wider stream for save titles */

static const char *const level_names[] = {
    "DELIVERY AREA",
    "KITCHEN DINING",
    "RECEPTION",
};
#define LEVEL_SELECT_COUNT ((int)(sizeof(level_names) / sizeof(level_names[0])))

/* Target game state for each entry (index matches level_names). Rooms set up by
   STATE_LOADING use level_pending[] below to say which area to switch to. */
static const GameState level_states[LEVEL_SELECT_COUNT] = {
    STATE_DELIVERY_AREA,
    STATE_LOADING,
    STATE_LOADING,
};

/* For STATE_LOADING entries, the area STATE_LOADING should switch to. */
static const GameState level_pending[LEVEL_SELECT_COUNT] = {
    STATE_DELIVERY_AREA,    /* unused (not a STATE_LOADING entry) */
    STATE_KITCHEN_DINING,
    STATE_RECEPTION,
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
        FntFlush(level_select_fnt);
        /* Footer with the coloured Cross button glyph (the Fnt streams can't
           hold coloured glyphs, so footers are drawn separately at a fixed y). */
        btn_prompt_draw(ctx, 96, 176, BTN_CROSS ":LOAD  SEL:BACK", 1);
    } else if (tmenu == TM_MAIN) {
        FntPrint(level_select_fnt, "%s NEW GAME\n%s LOAD GAME\n",
                 tmenu_cursor == 0 ? "*" : " ",
                 tmenu_cursor == 1 ? "*" : " ");
        FntFlush(level_select_fnt);
        btn_prompt_draw(ctx, 96, 176, BTN_CIRCLE ":SELECT " BTN_CROSS ":BACK", 1);
    } else if (tmenu == TM_CARD) {
        FntPrint(level_select_fnt, "LOAD GAME\n\n");
        FntPrint(level_select_fnt, "%s MEMORY CARD 1\n%s MEMORY CARD 2\n",
                 tmenu_cursor == 0 ? "*" : " ",
                 tmenu_cursor == 1 ? "*" : " ");
        if (tmenu_msg)
            FntPrint(level_select_fnt, "\n%s\n", tmenu_msg);
        FntFlush(level_select_fnt);
        btn_prompt_draw(ctx, 96, 176, BTN_CIRCLE ":SELECT " BTN_CROSS ":BACK", 1);
    } else if (tmenu == TM_FILE) {
        /* Wider window: save titles run up to 32 characters. Scroll a 10-entry
           window so a full card (15 saves) stays reachable. */
        int fnt = load_list_fnt >= 0 ? load_list_fnt : level_select_fnt;
        int first = (tmenu_cursor > 9) ? tmenu_cursor - 9 : 0;
        int k;
        FntPrint(fnt, "SELECT SAVE\n\n");
        for (k = first; k < tmenu_slot_count && k < first + 10; k++)
            FntPrint(fnt, "%s %s\n",
                     k == tmenu_cursor ? "*" : " ", tmenu_slots[k].title);
        if (tmenu_msg)
            FntPrint(fnt, "%s\n", tmenu_msg);
        FntFlush(fnt);
        btn_prompt_draw(ctx, 28, 222, BTN_CIRCLE ":LOAD  " BTN_CROSS ":BACK", 1);
    } else {
        title_flash++;
        if ((title_flash & 63) < 40)
            draw_press_start(ctx);
    }
}

void title_init(void) {
    /* Font window for the level-select list (8x16 glyphs), below the title. */
    level_select_fnt = FntOpen(96, 120, 160, 96, 0, 256);
    /* Wider window for the save-file list (32-char titles + cursor). */
    load_list_fnt = FntOpen(28, 118, 264, 112, 0, 512);
}

void update_title(void) {
    if (!pad_buff_len[0]) return;
    PadResponse *pad = (PadResponse *)pad_buff[0];

    /* Edge-detect newly pressed buttons (pad bits are active-low). */
    static uint16_t prev_held = 0;
    uint16_t held    = ~pad->btn;
    uint16_t pressed = held & ~prev_held;
    prev_held = held;

    if (!debug_menu_open && tmenu == TM_CLOSED) {
        if (pressed & PAD_SELECT) {
            debug_menu_open   = 1;
            debug_menu_cursor = 0;
        } else if (pressed & PAD_START) {
            tmenu        = TM_MAIN;   /* New Game / Load Game */
            tmenu_cursor = 0;
            tmenu_msg    = 0;
        }
        return;
    }

    if (debug_menu_open) {
        /* Level select: D-pad moves the cursor, X loads, Select backs out. */
        if (pressed & PAD_UP)
            debug_menu_cursor = (debug_menu_cursor + LEVEL_SELECT_COUNT - 1) % LEVEL_SELECT_COUNT;
        if (pressed & PAD_DOWN)
            debug_menu_cursor = (debug_menu_cursor + 1) % LEVEL_SELECT_COUNT;
        if (pressed & PAD_SELECT)
            debug_menu_open = 0;
        if (pressed & PAD_CROSS) {
            debug_menu_open = 0;
            GameState target = level_states[debug_menu_cursor];
            /* STATE_LOADING entries (kitchen, reception) need the area to switch to. */
            if (target == STATE_LOADING) pending_area = level_pending[debug_menu_cursor];
            game_state = target;
        }
        return;
    }

    /* ---- Start menu ----
       Circle = select, X = back: matches the in-game save menu's convention. */
    int confirm = (pressed & PAD_CIRCLE) ? 1 : 0;
    int back    = (pressed & PAD_CROSS)  ? 1 : 0;

    if (tmenu == TM_MAIN) {
        if (pressed & (PAD_UP | PAD_DOWN)) tmenu_cursor ^= 1;
        if (back) { tmenu = TM_CLOSED; return; }
        if (confirm) {
            if (tmenu_cursor == 0) {
                tmenu      = TM_CLOSED;             /* New Game — as before */
                game_state = STATE_DELIVERY_AREA;
            } else {
                tmenu        = TM_CARD;             /* Load Game */
                tmenu_cursor = 0;
                tmenu_msg    = 0;
            }
        }
    } else if (tmenu == TM_CARD) {
        if (pressed & (PAD_UP | PAD_DOWN)) { tmenu_cursor ^= 1; tmenu_msg = 0; }
        if (back) { tmenu = TM_MAIN; tmenu_cursor = 1; tmenu_msg = 0; return; }
        if (confirm) {
            tmenu_port = tmenu_cursor;
            memcard_begin();
            int present = memcard_present(tmenu_port);
            memcard_end();
            if (!present) { tmenu_msg = "NO MEMORY CARD"; return; }
            int n = savegame_list(tmenu_port, tmenu_slots, SAVE_MAX_SLOTS);
            if (n < 0)       { tmenu_msg = "CARD READ ERROR"; return; }
            if (n == 0)      { tmenu_msg = "NO SAVES ON CARD"; return; }
            tmenu_slot_count = n;
            tmenu        = TM_FILE;
            tmenu_cursor = 0;
            tmenu_msg    = 0;
        }
    } else if (tmenu == TM_FILE) {
        if (pressed & PAD_UP)
            tmenu_cursor = (tmenu_cursor + tmenu_slot_count - 1) % tmenu_slot_count;
        if (pressed & PAD_DOWN)
            tmenu_cursor = (tmenu_cursor + 1) % tmenu_slot_count;
        if (back) { tmenu = TM_CARD; tmenu_cursor = tmenu_port; tmenu_msg = 0; return; }
        if (confirm) {
            SaveData sd;
            if (savegame_read(tmenu_port, tmenu_slots[tmenu_cursor].block, &sd) != MC_OK) {
                tmenu_msg = "LOAD FAILED";
                return;
            }
            /* Stage the state for main.c to apply once the area is set up, then
               route into the saved area exactly like the level select. */
            savegame_stage_load(&sd);
            tmenu = TM_CLOSED;
            if (sd.area == (int32_t)STATE_KITCHEN_DINING ||
                sd.area == (int32_t)STATE_RECEPTION) {
                pending_area = (GameState)sd.area;
                game_state   = STATE_LOADING;
            } else {
                game_state = STATE_DELIVERY_AREA;
            }
        }
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
