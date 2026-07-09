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
#include "crucifaxe.h"
#include "camera.h"
#include "title.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Layout constants — all in screen pixels (320x240) */
#define MENU_BG_R           0
#define MENU_BG_G           0
#define MENU_BG_B           0

/* Two columns — each holds a 2-wide x 4-tall icon grid */
#define COL_ITEMS_X         5
#define COL_WEAPONS_X       97      /* 5 + 2*CELL_W + 8 gap */
#define HEADER_Y            10
#define COL_Y_START         26
#define CELL_W              42      /* ICON_SIZE + 12 */
#define CELL_H              42
#define ICON_SIZE           30
#define ICON_PADDING        6

/* Description box (right side) — starts after both columns */
#define DESC_X              189     /* COL_WEAPONS_X + 2*CELL_W + 8 */
#define DESC_Y              10
#define DESC_W              127     /* 320 - DESC_X - 4 */
#define DESC_H              195     /* HBAR_Y - DESC_Y - 5 */

/* Health bar */
#define HBAR_X              20
#define HBAR_Y              210
#define HBAR_W              180
#define HBAR_H              12

/* Cursor position — col=0/1 (items/weapons), subcol=0/1 (left/right within grid), row=0-3 */
static int cursor_col    = 0;
static int cursor_subcol = 0;
static int cursor_row    = 0;

/* Button state for navigation */
static int dpad_prev   = 0;

/* VRAM handles for menu icons */
static uint16_t key_tpage    = 0;
static uint16_t key_clut     = 0;
static uint8_t  key_u0, key_v0, key_u1, key_v1;
static uint16_t crfx_tpage   = 0;
static uint16_t crfx_clut    = 0;
static uint8_t  crfx_u0, crfx_v0, crfx_u1, crfx_v1;

/* Font handles */
static int menu_fnt    = -1;   /* description box */
static int items_fnt   = -1;   /* "ITEMS" header */
static int weapons_fnt = -1;   /* "WEAPONS" header */

/* Item descriptions */
static const char *item_descriptions[] = {
    "Front Door Key\n\nUnlocks the\nmansion's\nfront entrance",
};

static const char *weapon_descriptions[] = {
    "Crucifaxe\n\nA crucifix\nforged into\nan axe head.\nThe quintessential\nweapon of a\nDemon Hunter",
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
    *u0 = 0;
    *v0 = (uint8_t)(tim.prect->y % 256);
    *u1 = (uint8_t)(tex_w - 1);
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

/* Draw a textured icon using POLY_FT4 */
static void draw_icon(RenderContext *ctx, int x, int y, int size,
                       uint16_t tpage, uint16_t clut,
                       uint8_t u0, uint8_t v0, uint8_t u1, uint8_t v1,
                       int ot_idx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, 255, 255, 255);

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

/* Helper to get item count */
static int items_count(void) {
    int count = 0;
    if (player_keys & (1 << KEY_FRONT_DOOR)) count++;
    return count;
}

/* Helper to get weapon count */
static int weapons_count(void) {
    return 1;  /* crucifaxe always present */
}

static int col_count(int col) {
    return col == 0 ? items_count() : weapons_count();
}

/* Public API */

void menu_init(void) {
    load_icon_tim("\\KEY.TIM;1",      &key_tpage,  &key_clut,
                  &key_u0, &key_v0, &key_u1, &key_v1);
    load_icon_tim("\\CRFXICON.TIM;1", &crfx_tpage, &crfx_clut,
                  &crfx_u0, &crfx_v0, &crfx_u1, &crfx_v1);

    /* Font streams — opened after main's FntLoad so they aren't clobbered. */
    items_fnt   = FntOpen(COL_ITEMS_X,   HEADER_Y, CELL_W * 2, 14, 0, 64);
    weapons_fnt = FntOpen(COL_WEAPONS_X, HEADER_Y, CELL_W * 2, 14, 0, 64);
    menu_fnt    = FntOpen(DESC_X + 4, DESC_Y + 4, DESC_W - 8, DESC_H - 8, 0, 512);
}

void menu_open(void) {
    cursor_col    = 0;
    cursor_subcol = 0;
    cursor_row    = 0;
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
        game_state = current_area;
        return;
    }

    /* Navigate within the 2-wide x 4-tall grid; crossing column edges switches column */
    if (pressed & PAD_LEFT) {
        if (cursor_subcol > 0) {
            cursor_subcol--;
        } else if (cursor_col > 0) {
            cursor_col--;
            cursor_subcol = 1;
        }
    }
    if (pressed & PAD_RIGHT) {
        if (cursor_subcol < 1) {
            cursor_subcol++;
        } else if (cursor_col < 1) {
            cursor_col++;
            cursor_subcol = 0;
        }
    }
    if (pressed & PAD_UP) {
        cursor_row--;
        if (cursor_row < 0) cursor_row = 3;
    }
    if (pressed & PAD_DOWN) {
        cursor_row++;
        if (cursor_row > 3) cursor_row = 0;
    }
}

/* OT layers — all within the menu-reserved range 0..(SCENE_OT_MIN-1) so the menu
   always renders on top of every scene/entity primitive. Lower index = on top. */
#define OT_BG        15
#define OT_BOX       12
#define OT_FILL      10
#define OT_TEXWIN     8   /* above OT_ICON: reset the texture window before icons */
#define OT_ICON       7
#define OT_RETICULE   3

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
    draw_rect(ctx, COL_ITEMS_X,   HEADER_Y + 12, CELL_W * 2, 1, 80, 80, 80, OT_FILL);
    draw_rect(ctx, COL_WEAPONS_X, HEADER_Y + 12, CELL_W * 2, 1, 80, 80, 80, OT_FILL);

    /* Items column — 2x4 grid (flat loop to avoid nested-loop variable issues) */
    {
        int i;
        for (i = 0; i < 8; i++) {
            int row = i >> 1;
            int sc  = i & 1;
            int ix = COL_ITEMS_X + sc * CELL_W + ICON_PADDING;
            int iy = COL_Y_START + row * CELL_H + ICON_PADDING;
            draw_rect(ctx, ix - ICON_PADDING, iy - ICON_PADDING,
                      CELL_W, CELL_H, 35, 30, 45, OT_BOX);
            draw_outline(ctx, ix - ICON_PADDING, iy - ICON_PADDING,
                         CELL_W, CELL_H, 80, 70, 100, OT_FILL);
            if (i == 0 && (player_keys & (1 << KEY_FRONT_DOOR))) {
                draw_icon(ctx, ix, iy, ICON_SIZE, key_tpage, key_clut,
                          key_u0, key_v0, key_u1, key_v1, OT_ICON);
            }
        }
    }

    /* Weapons column — 2x4 grid */
    {
        int i;
        for (i = 0; i < 8; i++) {
            int row = i >> 1;
            int sc  = i & 1;
            int wx = COL_WEAPONS_X + sc * CELL_W + ICON_PADDING;
            int wy = COL_Y_START + row * CELL_H + ICON_PADDING;
            draw_rect(ctx, wx - ICON_PADDING, wy - ICON_PADDING,
                      CELL_W, CELL_H, 35, 30, 45, OT_BOX);
            draw_outline(ctx, wx - ICON_PADDING, wy - ICON_PADDING,
                         CELL_W, CELL_H, 80, 70, 100, OT_FILL);
            if (i == 0) {
                draw_icon(ctx, wx, wy, ICON_SIZE, crfx_tpage, crfx_clut,
                          crfx_u0, crfx_v0, crfx_u1, crfx_v1, OT_ICON);
            }
        }
    }

    /* Cursor reticule (frontmost) */
    {
        int col_x = cursor_col == 0 ? COL_ITEMS_X : COL_WEAPONS_X;
        int cx = col_x + cursor_subcol * CELL_W + ICON_PADDING - 2;
        int cy = COL_Y_START + cursor_row * CELL_H + ICON_PADDING - 2;
        int cs = ICON_SIZE + 4;

        draw_outline(ctx, cx - 2, cy - 2, cs + 4, cs + 4, 80, 80, 200, OT_RETICULE);
        draw_outline(ctx, cx, cy, cs, cs, 180, 180, 255, OT_RETICULE);

        draw_rect(ctx, cx,        cy,        3, 3, 255, 255, 255, OT_RETICULE);
        draw_rect(ctx, cx+cs-3,   cy,        3, 3, 255, 255, 255, OT_RETICULE);
        draw_rect(ctx, cx,        cy+cs-3,   3, 3, 255, 255, 255, OT_RETICULE);
        draw_rect(ctx, cx+cs-3,   cy+cs-3,   3, 3, 255, 255, 255, OT_RETICULE);
    }

    /* Description box */
    draw_outline(ctx, DESC_X, DESC_Y, DESC_W, DESC_H, 80, 80, 80, OT_FILL);
    draw_rect(ctx, DESC_X + 1, DESC_Y + 1, DESC_W - 2, DESC_H - 2, 15, 12, 20, OT_BOX);

    if (menu_fnt >= 0) {
        int slot = cursor_row * 2 + cursor_subcol;
        const char *desc = "Empty";
        if (cursor_col == 0) {
            if (slot == 0 && (player_keys & (1 << KEY_FRONT_DOOR)))
                desc = item_descriptions[0];
        } else {
            if (slot == 0)
                desc = weapon_descriptions[0];
        }
        FntPrint(menu_fnt, desc);
        FntFlush(menu_fnt);
    }

    /* Health bar */
    draw_rect(ctx, HBAR_X, HBAR_Y, HBAR_W, HBAR_H, 40, 0, 0, OT_BOX);
    int fill = (HBAR_W * player_health) / MAX_HEALTH;
    if (fill > 0) {
        uint8_t hr, hg;
        if (player_health > 60) {
            hr = 0;   hg = 200;
        } else if (player_health > 30) {
            hr = 200; hg = 200;
        } else {
            hr = 220; hg = 0;
        }
        draw_rect(ctx, HBAR_X, HBAR_Y, fill, HBAR_H, hr, hg, 0, OT_FILL);
    }
    draw_outline(ctx, HBAR_X, HBAR_Y, HBAR_W, HBAR_H, 100, 100, 100, OT_FILL);
}
