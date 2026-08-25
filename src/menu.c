#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <stdlib.h>
#include <inline_c.h>
#include "menu.h"
#include "render.h"
#include "player.h"
#include "key.h"
#include "copper_pot.h"
#include "crucifaxe.h"
#include "camera.h"
#include "title.h"
#include "btn_glyph.h"
#include "sound.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Layout constants — all in screen pixels (320x240) */
#define MENU_BG_R           0
#define MENU_BG_G           0
#define MENU_BG_B           0

/* Two columns of icon cells. ITEMS is 3 wide x 4 tall (12 cells), WEAPONS 2
   wide x 4 tall. The ITEMS column grew from 2 wide when the Yellow Key Stone
   made a 9th item; the cell and icon both shrank to pay for the extra column,
   which is what keeps the description box its original width. Widen the cells
   again and DESC_W is what gives. */
#define ITEM_COLS            3
#define WEAPON_COLS          2
#define GRID_ROWS            4
_Static_assert(ITEM_COLS * GRID_ROWS == MENU_ITEM_CELLS,
               "MENU_ITEM_CELLS must match the drawn ITEMS grid");
#define COL_ITEMS_X          5
#define COL_WEAPONS_X      115      /* 5 + ITEM_COLS*CELL_W + 8 gap */
#define HEADER_Y            10
#define COL_Y_START         26
#define CELL_W              34      /* ICON_SIZE + 2*ICON_PADDING */
#define CELL_H              34
#define ICON_SIZE           24
#define ICON_PADDING         5

/* Description box (right side) — starts after both columns */
#define DESC_X             191      /* COL_WEAPONS_X + WEAPON_COLS*CELL_W + 8 */
#define DESC_Y              10
#define DESC_W             125      /* 320 - DESC_X - 4 */
#define DESC_H             195      /* down to the bottom control-prompt strip */

static int col_cols(int col) { return col == 0 ? ITEM_COLS : WEAPON_COLS; }

/* Control prompt, along the bottom strip the health bar used to occupy. The
   HUD panel carries health now, so the menu does not repeat it. */
#define PROMPT_X            20
#define PROMPT_Y           226

/* Cursor position — col=0/1 (items/weapons), subcol=0..col_cols(col)-1, row=0-3 */
static int cursor_col    = 0;
static int cursor_subcol = 0;
static int cursor_row    = 0;

/* ---- The ITEMS grid's contents ---------------------------------------------
   item_cell[c] is the item ID (MENU_SLOT_*) shown in grid cell c, or -1 for an
   empty cell. Cells fill in collection order (menu_inventory_sync) and the
   player rearranges them with Circle; held_cell is the cell whose item is
   currently lifted off the grid, or -1. Same take/place idiom as the Anzu
   tablet's tiles, except placing onto an occupied cell SWAPS rather than being
   refused — with a fixed 12 cells there is no need to keep one free to shuffle
   through. */
static int8_t item_cell[MENU_ITEM_CELLS] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};
static int    held_cell = -1;

/* Button state for navigation */
static int dpad_prev   = 0;

/* VRAM handles for menu icons */
static uint16_t key_tpage    = 0;
static uint16_t key_clut     = 0;
static uint8_t  key_u0, key_v0, key_u1, key_v1;
static uint16_t crfx_tpage   = 0;
static uint16_t crfx_clut    = 0;
static uint8_t  crfx_u0, crfx_v0, crfx_u1, crfx_v1;
static uint16_t grav_tpage   = 0;
static uint16_t grav_clut    = 0;
static uint8_t  grav_u0, grav_v0, grav_u1, grav_v1;
static uint16_t rnds_tpage   = 0;
static uint16_t rnds_clut    = 0;
static uint8_t  rnds_u0, rnds_v0, rnds_u1, rnds_v1;
static uint16_t flmr_tpage   = 0;
static uint16_t flmr_clut    = 0;
static uint8_t  flmr_u0, flmr_v0, flmr_u1, flmr_v1;
static uint16_t waxcb_tpage  = 0;
static uint16_t waxcb_clut   = 0;
static uint8_t  waxcb_u0, waxcb_v0, waxcb_u1, waxcb_v1;
static uint16_t gkst_tpage   = 0;
static uint16_t gkst_clut    = 0;
static uint8_t  gkst_u0, gkst_v0, gkst_u1, gkst_v1;
static uint16_t pnok_tpage   = 0;
static uint16_t pnok_clut    = 0;
static uint8_t  pnok_u0, pnok_v0, pnok_u1, pnok_v1;
static uint16_t bkst_tpage   = 0;
static uint16_t bkst_clut    = 0;
static uint8_t  bkst_u0, bkst_v0, bkst_u1, bkst_v1;
static uint16_t ykst_tpage   = 0;
static uint16_t ykst_clut    = 0;
static uint8_t  ykst_u0, ykst_v0, ykst_u1, ykst_v1;
/* The magenta stone. exit_door_puzzle.c keeps its own handle on the SAME TIM for
   the fixed socket on its board; both LoadImage the one permanently resident
   slot (x672 y256, Voff 0 — window-safe), so the second read is redundant
   pixels, not a second slot. */
static uint16_t mkst_tpage   = 0;
static uint16_t mkst_clut    = 0;
static uint8_t  mkst_u0, mkst_v0, mkst_u1, mkst_v1;
/* The hatch key. Its art sits at VRAM y=320, i.e. Voff 64 — the pickup band at
   Voff 0 had no 64x64 gap left. That is still window-safe: every room's texture
   window is RECT{0,0,128,128}, so a V range of 64..127 maps to itself and needs
   no reset, unlike the Voff>=128 icons the note in menu_draw is about. */
static uint16_t htky_tpage   = 0;
static uint16_t htky_clut    = 0;
static uint8_t  htky_u0, htky_v0, htky_u1, htky_v1;

/* Font handles */
static int menu_fnt    = -1;   /* description box */
static int items_fnt   = -1;   /* "ITEMS" header */
static int weapons_fnt = -1;   /* "WEAPONS" header */

/* Item descriptions — indexed by the item's grid slot */
static const char *item_descriptions[] = {
    "Front Door Key\n\nUnlocks the\nmansion's\nfront entrance",
    "Rounds\n\nAmmunition\nfor the\nGrave-olver",
    "Copper Pot\n\nAn old copper\npot found in\nthe conservatory",
    "Wax Cube\n\nI think there's\nsomething inside...",
    "Green Key Stone\n\nA green jewel\nwith a key\nprotruding from\nthe back",
    "Flame Rounds\n\nIncendiary shot.\nBurns zombies\nand tentacles",
    "Piano Key\n\nA white key\nfor a piano",
    "Blue Key Stone\n\nA blue jewel\nwith a key\nprotruding from\nthe back",
    "Yellow Key Stone\n\nA yellow jewel\nwith a key\nprotruding from\nthe back",
    "Magenta Key Stone\n\nA magenta jewel\nwith a key\nprotruding from\nthe back",
    "Hatch Key\n\nA key for the\nhatch at the back\nof the garden",
};

static const char *weapon_descriptions[] = {
    "Crucifaxe\n\nA crucifix\nforged into\nan axe head.\nThe quintessential\nweapon of a\nDemon Hunter",
    "Grave-olver\n\nA revolver\nforged to lay\nthe risen dead\nback in their\ngraves",
};

/* Load TIM from disc, computing tpage/clut and the UV rect within the tpage. */
static void load_icon_tim(const char *filename,
                           uint16_t *tpage_out,
                           uint16_t *clut_out,
                           uint8_t *u0, uint8_t *v0,
                           uint8_t *u1, uint8_t *v1) {
    CdlFILE file;
    if (!CdSearchFile(&file, filename)) return;
    int   sectors = (file.size + 2047) / 2048;
    void *buf     = malloc(sectors * 2048);
    if (!buf) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);
    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);
    if (tim.mode & 0x8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
        *clut_out = getClut(tim.crect->x, tim.crect->y);
    }
    *tpage_out = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);

    /* UV within the tpage — V offset by the VRAM y within its 256-line page,
       U width scaled by pixels-per-16bit-word for the bit depth. */
    int bpp_mode = tim.mode & 3;
    int px_mult  = (bpp_mode == 0) ? 4 : (bpp_mode == 1) ? 2 : 1;
    int tex_w    = tim.prect->w * px_mult;
    int tex_h    = tim.prect->h;
    /* U0 = the texture's x offset WITHIN its 64-word tpage, so an icon that sits
       in the right half of a page (VRAM x not a multiple of 64) still maps: the
       tpage snaps to the page's left edge and U0 indexes into it. Page-aligned
       icons (key, crucifaxe) yield U0=0 as before. */
    int u_off = (tim.prect->x & 63) * px_mult;
    *u0 = (uint8_t)u_off;
    *v0 = (uint8_t)(tim.prect->y % 256);
    *u1 = (uint8_t)(u_off + tex_w - 1);
    *v1 = (uint8_t)(*v0 + tex_h - 1);

    free(buf);
}

/* Draw a filled rectangle */
static void draw_rect(RenderContext *ctx, int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;

    TILE *t = (TILE *)ctx->next_packet;
    setTile(t);
    setXY0(t, x, y);
    setWH(t, w, h);
    setRGB0(t, r, g, b);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[ot_idx], t);
    ctx->next_packet += sizeof(TILE);
}

/* Draw an outline rectangle (4 thin tiles) */
static void draw_outline(RenderContext *ctx, int x, int y, int w, int h,
                          uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    draw_rect(ctx, x,         y,         w, 1,     r, g, b, ot_idx);
    draw_rect(ctx, x,         y+h-1,     w, 1,     r, g, b, ot_idx);
    draw_rect(ctx, x,         y,         1, h,     r, g, b, ot_idx);
    draw_rect(ctx, x+w-1,     y,         1, h,     r, g, b, ot_idx);
}

/* Draw a textured icon using POLY_FT4. `bright` is the texture-modulation level
   (128 = neutral / authored brightness, 255 ≈ 2x). Most icons are dark source
   art drawn at 255; full-brightness art (e.g. the copper pot) uses 128. */
static void draw_icon(RenderContext *ctx, int x, int y, int size,
                       uint16_t tpage, uint16_t clut,
                       uint8_t u0, uint8_t v0, uint8_t u1, uint8_t v1,
                       uint8_t bright, int ot_idx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, bright, bright, bright);

    /* Quad corners in screen space: TL, TR, BL, BR */
    poly->x0 = x;           poly->y0 = y;
    poly->x1 = x + size;    poly->y1 = y;
    poly->x2 = x;           poly->y2 = y + size;
    poly->x3 = x + size;    poly->y3 = y + size;

    /* Texture coordinates from the loaded TIM's UV rect */
    poly->u0 = u0;  poly->v0 = v0;
    poly->u1 = u1;  poly->v1 = v0;
    poly->u2 = u0;  poly->v2 = v1;
    poly->u3 = u1;  poly->v3 = v1;

    poly->tpage = tpage;
    poly->clut = clut;

    addPrim(&ctx->buffers[ctx->active_buffer].ot[ot_idx], poly);
    ctx->next_packet += sizeof(POLY_FT4);
}

/* 3x5 bitmap font for digits 0-9. Each row's low 3 bits are the pixels,
   bit2 = left column. Used for the little ammo count over the rounds icon. */
static const uint8_t digit_font[10][5] = {
    {7,5,5,5,7}, /*0*/
    {2,6,2,2,7}, /*1*/
    {7,1,7,4,7}, /*2*/
    {7,1,7,1,7}, /*3*/
    {5,5,7,1,1}, /*4*/
    {7,4,7,1,7}, /*5*/
    {7,4,7,5,7}, /*6*/
    {7,1,2,4,4}, /*7*/
    {7,5,7,5,7}, /*8*/
    {7,5,7,1,7}, /*9*/
};

/* Draw an unsigned number as scaled bitmap pixels, left-aligned so its
   left/bottom edge sits at (left_x, bottom_y). Rendered in yellow over a 1px
   black shadow for legibility against the sprite behind it. */
static void draw_number(RenderContext *ctx, int left_x, int bottom_y,
                         int value, int scale, int ot_idx) {
    char buf[8];
    int  n = 0;
    if (value < 0) value = 0;
    /* itoa into buf (no stdio dependency) */
    if (value == 0) {
        buf[n++] = 0;
    } else {
        int v = value;
        while (v > 0 && n < 7) { buf[n++] = (char)(v % 10); v /= 10; }
    }
    /* buf holds digits least-significant first. */
    int digit_w = 3 * scale;
    int gap     = scale;
    int y0      = bottom_y - 5 * scale;

    int i;
    for (i = 0; i < n; i++) {
        int digit = buf[i];                       /* i=0 is the rightmost digit */
        int x0    = left_x + (n - 1 - i) * (digit_w + gap);
        int row, col;
        for (row = 0; row < 5; row++) {
            uint8_t bits = digit_font[digit][row];
            for (col = 0; col < 3; col++) {
                if (bits & (4 >> col)) {
                    int px = x0 + col * scale;
                    int py = y0 + row * scale;
                    /* Shadow one OT step behind (higher index = drawn first), the
                       yellow glyph in front (lower index = drawn last, on top). */
                    draw_rect(ctx, px + 1, py + 1, scale, scale, 0, 0, 0, ot_idx + 1);
                    draw_rect(ctx, px,     py,     scale, scale, 255, 255, 0, ot_idx);
                }
            }
        }
    }
}

/* ---- Shared inventory-slot accessors -------------------------------------
   The stove puzzle's item picker shows the SAME items as this column, so the
   slot table below is the single description of what lives in each grid cell
   (order matches item_descriptions[] and the draw loop's i == n cases). */
int menu_item_held(int slot) {
    switch (slot) {
        case MENU_SLOT_FRONT_DOOR_KEY: return (player_keys & (1 << KEY_FRONT_DOOR)) != 0;
        case MENU_SLOT_ROUNDS:         return player_ammo[AMMO_STANDARD] > 0;
        case MENU_SLOT_COPPER_POT:     return (player_items & (1 << ITEM_COPPER_POT)) != 0;
        case MENU_SLOT_WAX_CUBE:       return (player_items & (1 << ITEM_WAX_CUBE)) != 0;
        case MENU_SLOT_GREEN_KEY_STONE:return (player_items & (1 << ITEM_GREEN_KEY_STONE)) != 0;
        case MENU_SLOT_FLAME_ROUNDS:   return player_ammo[AMMO_FLAME] > 0;
        case MENU_SLOT_PIANO_KEY:      return (player_items & (1 << ITEM_PIANO_KEY)) != 0;
        case MENU_SLOT_BLUE_KEY_STONE: return (player_items & (1 << ITEM_BLUE_KEY_STONE)) != 0;
        case MENU_SLOT_YELLOW_KEY_STONE:
                                       return (player_items & (1 << ITEM_YELLOW_KEY_STONE)) != 0;
        case MENU_SLOT_MAGENTA_KEY_STONE:
                                       return (player_items & (1 << ITEM_MAGENTA_KEY_STONE)) != 0;
        case MENU_SLOT_HATCH_KEY:      return player_hatch_keys > 0;
        default: return 0;
    }
}

const char *menu_item_name(int slot) {
    switch (slot) {
        case MENU_SLOT_FRONT_DOOR_KEY: return "Front Door Key";
        case MENU_SLOT_ROUNDS:         return "Rounds";
        case MENU_SLOT_COPPER_POT:     return "Copper Pot";
        case MENU_SLOT_WAX_CUBE:       return "Wax Cube";
        case MENU_SLOT_GREEN_KEY_STONE:return "Green Key Stone";
        case MENU_SLOT_FLAME_ROUNDS:   return "Flame Rounds";
        case MENU_SLOT_PIANO_KEY:      return "Piano Key";
        case MENU_SLOT_BLUE_KEY_STONE: return "Blue Key Stone";
        case MENU_SLOT_YELLOW_KEY_STONE:return "Yellow Key Stone";
        case MENU_SLOT_MAGENTA_KEY_STONE:return "Magenta Key Stone";
        case MENU_SLOT_HATCH_KEY:      return "Hatch Key";
        default: return "";
    }
}

/* ---- Inventory order (see menu.h) ---------------------------------------- */

static int cell_of_item(int item) {
    int c;
    for (c = 0; c < MENU_ITEM_CELLS; c++) if (item_cell[c] == (int8_t)item) return c;
    return -1;
}

void menu_inventory_reset(void) {
    int c;
    for (c = 0; c < MENU_ITEM_CELLS; c++) item_cell[c] = -1;
    held_cell = -1;
}

void menu_inventory_sync(void) {
    int c, id;

    /* Spent or consumed: the ammo counters hit zero and the puzzles clear bits
       in player_items, and neither tells us — menu_item_held is the only truth. */
    for (c = 0; c < MENU_ITEM_CELLS; c++)
        if (item_cell[c] >= 0 && !menu_item_held(item_cell[c])) item_cell[c] = -1;

    /* Newly held: first free cell. Called from the pickup path, so items landing
       in one sync are normally a single item and the order is collection order;
       a batch grant (a debug jump, a load) falls back to ID order within itself. */
    for (id = 0; id < MENU_ITEM_SLOTS; id++) {
        if (!menu_item_held(id) || cell_of_item(id) >= 0) continue;
        for (c = 0; c < MENU_ITEM_CELLS; c++)
            if (item_cell[c] < 0) { item_cell[c] = (int8_t)id; break; }
    }

    /* A pickup can land while the menu is open and an item is lifted. The lifted
       item stays in its cell until it is placed, so it cannot be overwritten —
       but if THAT item was the one consumed, stop carrying a hole. */
    if (held_cell >= 0 && item_cell[held_cell] < 0) held_cell = -1;
}

void menu_inventory_save(uint8_t *out) {
    int c;
    for (c = 0; c < MENU_ITEM_CELLS; c++)
        out[c] = (item_cell[c] >= 0) ? (uint8_t)(item_cell[c] + 1) : 0;
}

void menu_inventory_load(const uint8_t *in) {
    int c, d;
    menu_inventory_reset();
    for (c = 0; c < MENU_ITEM_CELLS; c++) {
        int id = (int)in[c] - 1;
        if (id < 0 || id >= MENU_ITEM_SLOTS) continue;   /* empty, or out of range */
        for (d = 0; d < c; d++) if (item_cell[d] == (int8_t)id) break;
        if (d < c) continue;                             /* already placed: drop the dup */
        item_cell[c] = (int8_t)id;
    }
    /* The blob is only an ARRANGEMENT. What is actually held comes from the
       inventory fields the caller has already restored, so sync has the last
       word: it adds anything the blob missed and clears anything it invented. */
    menu_inventory_sync();
}

/* Draw the icon for an inventory slot anywhere on screen (the stove puzzle's
   picker and its ingredient boxes). No-op for a slot the player doesn't hold.
   The caller is responsible for resetting the texture window first — see the
   note in menu_draw. */
void menu_draw_item_icon(RenderContext *ctx, int slot, int x, int y, int size,
                         int ot_idx) {
    switch (slot) {
        case MENU_SLOT_FRONT_DOOR_KEY:
            if (!menu_item_held(slot)) return;
            draw_icon(ctx, x, y, size, key_tpage, key_clut,
                      key_u0, key_v0, key_u1, key_v1, 255, ot_idx);
            break;
        case MENU_SLOT_ROUNDS:
            if (!menu_item_held(slot)) return;
            draw_icon(ctx, x, y, size, rnds_tpage, rnds_clut,
                      rnds_u0, rnds_v0, rnds_u1, rnds_v1, 255, ot_idx);
            break;
        case MENU_SLOT_FLAME_ROUNDS:
            if (!menu_item_held(slot)) return;
            draw_icon(ctx, x, y, size, flmr_tpage, flmr_clut,
                      flmr_u0, flmr_v0, flmr_u1, flmr_v1, 255, ot_idx);
            break;
        case MENU_SLOT_COPPER_POT: {
            if (!menu_item_held(slot)) return;
            uint16_t tp, cl; uint8_t u0, v0, u1, v1;
            copper_pot_icon(&tp, &cl, &u0, &v0, &u1, &v1);
            /* Full-brightness art — neutral 128 modulation (see draw_icon). */
            draw_icon(ctx, x, y, size, tp, cl, u0, v0, u1, v1, 128, ot_idx);
            break;
        }
        case MENU_SLOT_WAX_CUBE:
            if (!menu_item_held(slot)) return;
            draw_icon(ctx, x, y, size, waxcb_tpage, waxcb_clut,
                      waxcb_u0, waxcb_v0, waxcb_u1, waxcb_v1, 128, ot_idx);
            break;
        case MENU_SLOT_GREEN_KEY_STONE:
            if (!menu_item_held(slot)) return;
            draw_icon(ctx, x, y, size, gkst_tpage, gkst_clut,
                      gkst_u0, gkst_v0, gkst_u1, gkst_v1, 128, ot_idx);
            break;
        case MENU_SLOT_PIANO_KEY:
            if (!menu_item_held(slot)) return;
            draw_icon(ctx, x, y, size, pnok_tpage, pnok_clut,
                      pnok_u0, pnok_v0, pnok_u1, pnok_v1, 128, ot_idx);
            break;
        case MENU_SLOT_BLUE_KEY_STONE:
            if (!menu_item_held(slot)) return;
            draw_icon(ctx, x, y, size, bkst_tpage, bkst_clut,
                      bkst_u0, bkst_v0, bkst_u1, bkst_v1, 128, ot_idx);
            break;
        case MENU_SLOT_YELLOW_KEY_STONE:
            if (!menu_item_held(slot)) return;
            draw_icon(ctx, x, y, size, ykst_tpage, ykst_clut,
                      ykst_u0, ykst_v0, ykst_u1, ykst_v1, 128, ot_idx);
            break;
        case MENU_SLOT_MAGENTA_KEY_STONE:
            if (!menu_item_held(slot)) return;
            draw_icon(ctx, x, y, size, mkst_tpage, mkst_clut,
                      mkst_u0, mkst_v0, mkst_u1, mkst_v1, 128, ot_idx);
            break;
        case MENU_SLOT_HATCH_KEY:
            if (!menu_item_held(slot)) return;
            draw_icon(ctx, x, y, size, htky_tpage, htky_clut,
                      htky_u0, htky_v0, htky_u1, htky_v1, 128, ot_idx);
            break;
        default: break;
    }
}

/* Draw a WEAPON's icon at an arbitrary screen rect — the HUD's weapon box uses
   this for whatever is currently equipped, from the same VRAM copies the menu's
   WEAPONS column draws. Unlike menu_draw_item_icon this does NOT check
   ownership: the caller already knows which weapon is in hand.
   The caller must have reset the texture window — see the note in menu_draw. */
void menu_draw_weapon_icon(RenderContext *ctx, int weapon, int x, int y, int size,
                           int ot_idx) {
    switch (weapon) {
        case WEAPON_CRUCIFAXE:
            draw_icon(ctx, x, y, size, crfx_tpage, crfx_clut,
                      crfx_u0, crfx_v0, crfx_u1, crfx_v1, 255, ot_idx);
            break;
        case WEAPON_GRAVEOLVER:
            draw_icon(ctx, x, y, size, grav_tpage, grav_clut,
                      grav_u0, grav_v0, grav_u1, grav_v1, 255, ot_idx);
            break;
        default: break;
    }
}

/* Public API */

void menu_init(void) {
    load_icon_tim("\\KEY.TIM;1",      &key_tpage,  &key_clut,
                  &key_u0, &key_v0, &key_u1, &key_v1);
    load_icon_tim("\\CRFXICON.TIM;1", &crfx_tpage, &crfx_clut,
                  &crfx_u0, &crfx_v0, &crfx_u1, &crfx_v1);
    load_icon_tim("\\TEX\\GRAVOLVR.TIM;1", &grav_tpage, &grav_clut,
                  &grav_u0, &grav_v0, &grav_u1, &grav_v1);
    load_icon_tim("\\TEX\\STNDRNDS.TIM;1", &rnds_tpage, &rnds_clut,
                  &rnds_u0, &rnds_v0, &rnds_u1, &rnds_v1);
    load_icon_tim("\\TEX\\FLMRNDS.TIM;1", &flmr_tpage, &flmr_clut,
                  &flmr_u0, &flmr_v0, &flmr_u1, &flmr_v1);
    load_icon_tim("\\TEX\\WXCB.TIM;1", &waxcb_tpage, &waxcb_clut,
                  &waxcb_u0, &waxcb_v0, &waxcb_u1, &waxcb_v1);
    load_icon_tim("\\TEX\\GRNKYSTN.TIM;1", &gkst_tpage, &gkst_clut,
                  &gkst_u0, &gkst_v0, &gkst_u1, &gkst_v1);
    load_icon_tim("\\TEX\\PNOKEY.TIM;1", &pnok_tpage, &pnok_clut,
                  &pnok_u0, &pnok_v0, &pnok_u1, &pnok_v1);
    load_icon_tim("\\TEX\\YLKYSTN.TIM;1", &ykst_tpage, &ykst_clut,
                  &ykst_u0, &ykst_v0, &ykst_u1, &ykst_v1);
    load_icon_tim("\\TEX\\BLKYSTN.TIM;1", &bkst_tpage, &bkst_clut,
                  &bkst_u0, &bkst_v0, &bkst_u1, &bkst_v1);
    load_icon_tim("\\TEX\\MGNKYSTN.TIM;1", &mkst_tpage, &mkst_clut,
                  &mkst_u0, &mkst_v0, &mkst_u1, &mkst_v1);
    load_icon_tim("\\TEX\\HATCHKEY.TIM;1", &htky_tpage, &htky_clut,
                  &htky_u0, &htky_v0, &htky_u1, &htky_v1);

    /* Font streams — opened after main's FntLoad so they aren't clobbered. */
    items_fnt   = FntOpen(COL_ITEMS_X,   HEADER_Y, CELL_W * ITEM_COLS,   14, 0, 64);
    weapons_fnt = FntOpen(COL_WEAPONS_X, HEADER_Y, CELL_W * WEAPON_COLS, 14, 0, 64);
    menu_fnt    = FntOpen(DESC_X + 4, DESC_Y + 4, DESC_W - 8, DESC_H - 8, 0, 512);
}

void menu_open(void) {
    cursor_col    = 0;
    cursor_subcol = 0;
    cursor_row    = 0;
    held_cell     = -1;   /* never re-enter the menu mid-carry */
    /* Opening is a confirmation like any other — main.c only calls this on a
       fresh Start press, never on a state restore. */
    sound_play(SFX_SELECT);
    /* Anything granted or consumed away from the pickup path — a puzzle reward,
       a crate, a debug grant, ammo spent since the last look — lands in the grid
       here. */
    menu_inventory_sync();
    /* Capture current button state so opening press doesn't close menu */
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        dpad_prev = ~pad->btn;
    } else {
        dpad_prev = 0;
    }
}

void menu_update(void) {
    if (!pad_buff_len[0]) return;
    PadResponse *pad = (PadResponse *)pad_buff[0];
    uint16_t btn     = ~pad->btn;
    uint16_t pressed = btn & ~dpad_prev;
    dpad_prev        = btn;

    /* Close menu on Start — return to whichever area opened it */
    if (pressed & PAD_START) {
        sound_play(SFX_BACK);
        game_state = current_area;
        return;
    }

    /* Navigate the current column's grid; crossing its edge switches column.
       The two columns are different widths (ITEMS 3, WEAPONS 2), so the edge is
       col_cols(cursor_col) rather than a hard-coded 1.

       While carrying an item the cursor is penned into the ITEMS column: the
       WEAPONS column has nowhere to put it, so letting the reticule wander there
       would only offer a Circle that does nothing. */
    /* Where the reticule was, so the blip below can be conditioned on it having
       actually MOVED. Several of the steps here legitimately do nothing — Left
       at the ITEMS column's left edge, or any column change while carrying —
       and a blip on a press that moved nothing reads as the menu accepting
       something it did not. */
    int prev_col = cursor_col, prev_subcol = cursor_subcol, prev_row = cursor_row;

    if (pressed & PAD_LEFT) {
        if (cursor_subcol > 0) {
            cursor_subcol--;
        } else if (cursor_col > 0 && held_cell < 0) {
            cursor_col--;
            cursor_subcol = col_cols(cursor_col) - 1;
        }
    }
    if (pressed & PAD_RIGHT) {
        if (cursor_subcol < col_cols(cursor_col) - 1) {
            cursor_subcol++;
        } else if (cursor_col < 1 && held_cell < 0) {
            cursor_col++;
            cursor_subcol = 0;
        }
    }
    if (pressed & PAD_UP) {
        cursor_row--;
        if (cursor_row < 0) cursor_row = GRID_ROWS - 1;
    }
    if (pressed & PAD_DOWN) {
        cursor_row++;
        if (cursor_row > GRID_ROWS - 1) cursor_row = 0;
    }

    if (cursor_col != prev_col || cursor_subcol != prev_subcol ||
        cursor_row != prev_row)
        sound_play(SFX_CURSOR);

    /* Circle lifts the item under the reticule off the grid; Circle again drops
       it into the cell the reticule is now over — empty (a move) or occupied (a
       swap). Placing back onto the source cell is the swap with itself, so it
       cancels for free.

       The blip goes on the branches that DO something: lifting an empty cell,
       and Circle over the WEAPONS column, are both no-ops the player should
       hear nothing from. */
    if ((pressed & PAD_CIRCLE) && cursor_col == 0) {
        int cell = cursor_row * ITEM_COLS + cursor_subcol;
        if (held_cell < 0) {
            if (item_cell[cell] >= 0) { held_cell = cell; sound_play(SFX_SELECT); }
        } else {
            sound_play(SFX_SELECT);
            int8_t t             = item_cell[cell];
            item_cell[cell]      = item_cell[held_cell];
            item_cell[held_cell] = t;
            held_cell            = -1;
        }
    }
    /* Cross puts a carried item straight back where it came from. */
    if ((pressed & PAD_CROSS) && held_cell >= 0) {
        held_cell = -1;
        sound_play(SFX_BACK);
    }
}

/* OT layers — all within the menu-reserved range 0..(SCENE_OT_MIN-1) so the menu
   always renders on top of every scene/entity primitive. Lower index = on top. */
#define OT_BG        15
#define OT_BOX       12
#define OT_FILL      10
#define OT_TEXWIN     8   /* above OT_ICON: reset the texture window before icons */
#define OT_ICON       7
#define OT_COUNT      5   /* ammo count over an icon (yellow at 5, shadow at 6) */
#define OT_RETICULE   3
#define OT_TEXT       1   /* the control prompt along the bottom */

/* ---- The carry box ---------------------------------------------------------
   Where a lifted item is shown while the reticule moves: one cell's worth of
   space in the bottom of the description panel. The Anzu board hangs the held
   tile off the reticule itself, which cannot work here — the reticule passes
   over OCCUPIED cells (that is what a swap is), and two 24px icons in one 34px
   cell is unreadable. Parking it beside the description keeps both the thing
   being carried and the thing about to be displaced legible. */
#define CARRY_X      (DESC_X + 6)
#define CARRY_Y             168      /* label at 158; the panel ends at 205 */

/* One item's contents at (x,y): the icon plus, for the two counted items, the
   reserve count over it. Drawn wherever the item happens to be — a cell of the
   ITEMS grid or the carry box — so the count follows the icon instead of being
   pinned to a fixed cell. -1 (an empty cell) draws nothing. */
static void draw_item_cell(RenderContext *ctx, int item, int x, int y) {
    if (item < 0) return;
    menu_draw_item_icon(ctx, item, x, y, ICON_SIZE, OT_ICON);
    /* Ammo count, yellow, tucked into the icon's bottom-left. */
    if (item == MENU_SLOT_ROUNDS && player_ammo[AMMO_STANDARD] > 0)
        draw_number(ctx, x, y + ICON_SIZE, player_ammo[AMMO_STANDARD], 2, OT_COUNT);
    if (item == MENU_SLOT_FLAME_ROUNDS && player_ammo[AMMO_FLAME] > 0)
        draw_number(ctx, x, y + ICON_SIZE, player_ammo[AMMO_FLAME], 2, OT_COUNT);
    /* Hatch keys stack two to a cell. Only worth a number once there are two —
       a "1" over the icon of a thing you obviously hold once is noise, which is
       not true of ammo, where the exact reserve is the point. */
    if (item == MENU_SLOT_HATCH_KEY && player_hatch_keys > 1)
        draw_number(ctx, x, y + ICON_SIZE, player_hatch_keys, 2, OT_COUNT);
}

void menu_draw(RenderContext *ctx) {
    /* Semi-transparent background: DR_TPAGE sets abr=0 (B/2+F/2, 50% blend),
       then a semi-trans TILE darkens/tints the scene behind the menu. */
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Add TILE first so DR_TPAGE ends up at the head (executed first by GPU). */
        if (ctx->next_packet + sizeof(TILE) <= buf_end) {
            TILE *t = (TILE *)ctx->next_packet;
            setTile(t);
            setSemiTrans(t, 1);
            setXY0(t, 0, 0);
            setWH(t, 320, 240);
            setRGB0(t, 20, 16, 28);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_BG], t);
            ctx->next_packet += sizeof(TILE);
        }

        if (ctx->next_packet + sizeof(DR_TPAGE) <= buf_end) {
            DR_TPAGE *dp = (DR_TPAGE *)ctx->next_packet;
            setDrawTPage(dp, 0, 0, getTPage(0, 0, 0, 0)); /* abr=0 = 50% blend */
            addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_BG], dp);
            ctx->next_packet += sizeof(DR_TPAGE);
        }
    }

    /* Reset the texture window before the icons. The menu draws over the live
       area, and kitchen/reception sort a 128x128 texture window at OT_LENGTH-1
       that (being at the top of the OT) stays active down into the menu. That
       window wraps the icons' UVs mod-128, so the crucifaxe/key icons — whose
       texture sits at VRAM y>=384 (V offset 128 within its page) — sample the
       texture ABOVE them instead (red_crpt in the kitchen, frnt_dr in reception,
       hence a different corruption per room; delivery sets no window so it's
       fine). RECT{0,0,0,0} = full page, no masking. Sorted at OT_TEXWIN (>OT_ICON)
       so the GPU applies it before the icons. */
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            RECT full = {0, 0, 0, 0};
            DR_TWIN *tw = (DR_TWIN *)ctx->next_packet;
            setTexWindow(tw, &full);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_TEXWIN], tw);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    /* Column header text */
    if (items_fnt >= 0)   { FntPrint(items_fnt, "ITEMS");     FntFlush(items_fnt); }
    if (weapons_fnt >= 0) { FntPrint(weapons_fnt, "WEAPONS"); FntFlush(weapons_fnt); }

    /* Column header dividers */
    draw_rect(ctx, COL_ITEMS_X,   HEADER_Y + 12, CELL_W * ITEM_COLS,   1, 80, 80, 80, OT_FILL);
    draw_rect(ctx, COL_WEAPONS_X, HEADER_Y + 12, CELL_W * WEAPON_COLS, 1, 80, 80, 80, OT_FILL);

    /* Items column — MENU_ITEM_CELLS cells, each showing whatever item_cell says
       it holds. The icons come from menu_draw_item_icon, the same accessor the
       stove and Anzu pickers use, so a new item only ever has to be described in
       ONE place. The cell a carried item came from draws EMPTY: the item itself
       is shown in the carry box beside the description. */
    {
        int i;
        for (i = 0; i < MENU_ITEM_CELLS; i++) {
            int row = i / ITEM_COLS;
            int sc  = i % ITEM_COLS;
            int ix = COL_ITEMS_X + sc * CELL_W + ICON_PADDING;
            int iy = COL_Y_START + row * CELL_H + ICON_PADDING;
            draw_rect(ctx, ix - ICON_PADDING, iy - ICON_PADDING,
                      CELL_W, CELL_H, 35, 30, 45, OT_BOX);
            draw_outline(ctx, ix - ICON_PADDING, iy - ICON_PADDING,
                         CELL_W, CELL_H, 80, 70, 100, OT_FILL);
            if (i == held_cell) continue;
            draw_item_cell(ctx, item_cell[i], ix, iy);
        }
    }

    /* Weapons column — WEAPON_COLS x GRID_ROWS grid */
    {
        int i;
        for (i = 0; i < WEAPON_COLS * GRID_ROWS; i++) {
            int row = i / WEAPON_COLS;
            int sc  = i % WEAPON_COLS;
            int wx = COL_WEAPONS_X + sc * CELL_W + ICON_PADDING;
            int wy = COL_Y_START + row * CELL_H + ICON_PADDING;
            draw_rect(ctx, wx - ICON_PADDING, wy - ICON_PADDING,
                      CELL_W, CELL_H, 35, 30, 45, OT_BOX);
            draw_outline(ctx, wx - ICON_PADDING, wy - ICON_PADDING,
                         CELL_W, CELL_H, 80, 70, 100, OT_FILL);
            if (i == 0) {
                draw_icon(ctx, wx, wy, ICON_SIZE, crfx_tpage, crfx_clut,
                          crfx_u0, crfx_v0, crfx_u1, crfx_v1, 255, OT_ICON);
            }
            if (i == 1 && (player_weapons & (1 << WEAPON_GRAVEOLVER))) {
                draw_icon(ctx, wx, wy, ICON_SIZE, grav_tpage, grav_clut,
                          grav_u0, grav_v0, grav_u1, grav_v1, 255, OT_ICON);
                /* Loaded-round count, yellow, tucked into the icon's bottom-right
                   (single digit: cylinder holds at most GRAVEOLVER_CAPACITY). */
                draw_number(ctx, wx + ICON_SIZE - 3 * 2, wy + ICON_SIZE,
                            graveolver_loaded, 2, OT_COUNT);
            }
        }
    }

    /* Cursor reticule (frontmost). It turns amber while an item is being carried
       — the one tell that Circle will now PLACE rather than take. */
    {
        int col_x = cursor_col == 0 ? COL_ITEMS_X : COL_WEAPONS_X;
        int cx = col_x + cursor_subcol * CELL_W + ICON_PADDING - 2;
        int cy = COL_Y_START + cursor_row * CELL_H + ICON_PADDING - 2;
        int cs = ICON_SIZE + 4;
        int carry = held_cell >= 0;
        uint8_t or_ = carry ? 200 : 80,  og = carry ? 120 : 80,  ob = carry ?  20 : 200;
        uint8_t ir  = carry ? 255 : 180, ig = carry ? 210 : 180, ib = carry ?  90 : 255;
        uint8_t kr  = 255,               kg = carry ? 230 : 255, kb = carry ? 120 : 255;

        draw_outline(ctx, cx - 2, cy - 2, cs + 4, cs + 4, or_, og, ob, OT_RETICULE);
        draw_outline(ctx, cx, cy, cs, cs, ir, ig, ib, OT_RETICULE);

        draw_rect(ctx, cx,        cy,        3, 3, kr, kg, kb, OT_RETICULE);
        draw_rect(ctx, cx+cs-3,   cy,        3, 3, kr, kg, kb, OT_RETICULE);
        draw_rect(ctx, cx,        cy+cs-3,   3, 3, kr, kg, kb, OT_RETICULE);
        draw_rect(ctx, cx+cs-3,   cy+cs-3,   3, 3, kr, kg, kb, OT_RETICULE);
    }

    /* Description box */
    draw_outline(ctx, DESC_X, DESC_Y, DESC_W, DESC_H, 80, 80, 80, OT_FILL);
    draw_rect(ctx, DESC_X + 1, DESC_Y + 1, DESC_W - 2, DESC_H - 2, 15, 12, 20, OT_BOX);

    if (menu_fnt >= 0) {
        int slot = cursor_row * col_cols(cursor_col) + cursor_subcol;
        const char *desc = "Empty";
        if (cursor_col == 0) {
            /* item_descriptions[] is indexed by item ID, so the cell's CONTENTS
               are the index — not the cell number, which no longer says anything
               about which item is in it. The cell the carried item came from
               reads "Empty", which is exactly what it is right now. */
            int item = (slot == held_cell) ? -1 : item_cell[slot];
            if (item >= 0) desc = item_descriptions[item];
        } else {
            if (slot == 0)
                desc = weapon_descriptions[0];
            else if (slot == 1 && (player_weapons & (1 << WEAPON_GRAVEOLVER)))
                desc = weapon_descriptions[1];
        }
        FntPrint(menu_fnt, desc);
        FntFlush(menu_fnt);
    }

    /* Carry box — only while something is lifted, so the panel is unchanged in
       the ordinary case. Its own cell, in the description panel's bottom. */
    if (held_cell >= 0) {
        btn_prompt_draw(ctx, CARRY_X, CARRY_Y - 10, "CARRYING", OT_TEXT);
        draw_rect(ctx, CARRY_X, CARRY_Y, CELL_W, CELL_H, 60, 45, 20, OT_BOX);
        draw_outline(ctx, CARRY_X, CARRY_Y, CELL_W, CELL_H, 200, 160, 60, OT_FILL);
        draw_item_cell(ctx, item_cell[held_cell],
                       CARRY_X + ICON_PADDING, CARRY_Y + ICON_PADDING);
    }

    /* Control prompt along the bottom. Only the ITEMS column can be
       rearranged, so nothing is advertised over an empty cell or the WEAPONS
       column — the line appears exactly when there is a button to press. */
    if (cursor_col == 0) {
        int cell = cursor_row * ITEM_COLS + cursor_subcol;
        const char *prompt = 0;
        if (held_cell >= 0)            prompt = BTN_CIRCLE " Place/Swap  " BTN_CROSS " Cancel";
        else if (item_cell[cell] >= 0) prompt = BTN_CIRCLE " Move item";
        if (prompt) btn_prompt_draw(ctx, PROMPT_X, PROMPT_Y, prompt, OT_TEXT);
    }
}
