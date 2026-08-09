#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxcd.h>
#include "hud.h"
#include "render.h"
#include "player.h"
#include "camera.h"   /* sprint_stamina, SPRINT_STAMINA_MAX, sprint_cooldown */
#include "menu.h"     /* menu_draw_weapon_icon */

/* ---- Where the panel sits ------------------------------------------------
   The art is authored at screen resolution, so a source-texture coordinate and
   a screen coordinate differ only by HUD_Y. Every rect below is in SOURCE
   space (0..319, 0..55) and is read straight off textures/hud.png. */
/* HUD_PANEL_H, not HUD_H: hud.h's include guard already owns that name. */
#define HUD_PANEL_W      320
#define HUD_PANEL_H       56
#define HUD_Y            (SCREEN_YRES - HUD_PANEL_H)   /* 184 */

/* The purple wash behind the frame's transparent boxes. Matches the log box's
   own fill in the art, so the panel reads as one slab. */
#define HUD_BACK_R        36
#define HUD_BACK_G         6
#define HUD_BACK_B        61

/* Box interiors — inside the 2px yellow borders. */
#define BAR_X              6
#define BAR_W            128
#define BAR_H             11
#define HEALTH_Y          14
#define STAMINA_Y         31

#define WPN_X            141
#define WPN_Y              9
#define WPN_SIZE          38
#define WPN_ICON          34      /* centred in the box, 2px of margin */

/* The log box, outer edge (what hud_draw_log_only redraws on its own) and the
   text area inside it. */
#define LOGBOX_X0        183
#define LOGBOX_Y0          7
#define LOGBOX_X1        315
#define LOGBOX_Y1         48
#define LOG_TEXT_X       187
#define LOG_TEXT_Y        11
#define LOG_COLS          15      /* 125px of interior / 8px font advance */
#define LOG_ROWS           4      /*  34px of interior / 8px line pitch   */

/* ---- OT slots ------------------------------------------------------------
   All inside the 0-15 block main.c reserves for UI, and all chosen to miss the
   indices the puzzles and menus use (12/10/8/7/3/1) and the poison wash (3),
   because the log box stays up over a camera-locked puzzle. Lower index = drawn
   later = nearer the front. Each bucket that draws a TEXTURE also carries its
   own window disable — see hud_disable_texwindow. */
#define HUD_OT_BACK        6      /* purple slab                                 */
#define HUD_OT_FILL        5      /* bar fills + weapon icon (textured)          */
#define HUD_OT_PANEL       4      /* the frame texture itself                    */
#define HUD_OT_COUNT_SHADE 2      /* ammo count drop shadow                      */
#define HUD_OT_COUNT       1      /* ammo count                                  */
#define HUD_OT_TEXT        0      /* log text (textured font), front-most        */

/* VRAM handles for HUD.TIM, captured at load. */
static int      hud_loaded    = 0;
static uint16_t hud_clut      = 0;
static int      hud_page_x    = 0;   /* page-aligned VRAM word x of the texture */
static int      hud_page_y    = 0;
static int      hud_u_base    = 0;   /* the art's u within that first page      */
static uint8_t  hud_v_base    = 0;   /* the art's v within its 256-line page    */

void hud_load_texture(void) {
    CdlFILE file;
    if (!CdSearchFile(&file, "\\TEX\\HUD.TIM;1")) return;
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
        hud_clut = getClut(tim.crect->x, tim.crect->y);
    }
    /* 4bpp: 4 pixels per VRAM word, 64 words (256 px) per texture page. The
       panel is 80 words wide, so it spills into the page after this one and
       hud_panel_span walks the pages rather than assuming one. */
    hud_page_x = tim.prect->x & ~63;
    hud_page_y = tim.prect->y;
    hud_u_base = (tim.prect->x & 63) * 4;
    hud_v_base = (uint8_t)(tim.prect->y % 256);
    hud_loaded = 1;

    free(buf);
}

/* ---- Primitive helpers ---------------------------------------------------- */

static void hud_rect(RenderContext *ctx, int x, int y, int w, int h,
                     uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    TILE *t = (TILE *)ctx->next_packet;
    setTile(t);
    setRGB0(t, r, g, b);
    setXY0(t, x, y);
    setWH(t, w, h);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[ot_idx], t);
    ctx->next_packet += sizeof(TILE);
}

/* Draw the source rect (sx0,sy0)-(sx1,sy1) INCLUSIVE of the panel art, at its
   matching screen position. Splits the run at every 256-texel page boundary so
   the 320-wide art can straddle two texture pages (see hud.h). */
static void hud_panel_span(RenderContext *ctx, int sx0, int sy0, int sx1, int sy1,
                           int ot_idx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint8_t  v0 = (uint8_t)(hud_v_base + sy0);
    uint8_t  v1 = (uint8_t)(hud_v_base + sy1);
    int      y0 = HUD_Y + sy0;
    int      y1 = HUD_Y + sy1 + 1;
    int      x  = sx0;

    while (x <= sx1) {
        int u_abs   = hud_u_base + x;
        int page    = u_abs >> 8;
        int u_start = u_abs & 255;
        int run     = 256 - u_start;
        if (run > sx1 - x + 1) run = sx1 - x + 1;

        if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;
        POLY_FT4 *p = (POLY_FT4 *)ctx->next_packet;
        setPolyFT4(p);
        setRGB0(p, 128, 128, 128);   /* neutral modulation: authored brightness */

        p->x0 = x;         p->y0 = y0;
        p->x1 = x + run;   p->y1 = y0;
        p->x2 = x;         p->y2 = y1;
        p->x3 = x + run;   p->y3 = y1;

        p->u0 = (uint8_t)u_start;             p->v0 = v0;
        p->u1 = (uint8_t)(u_start + run - 1); p->v1 = v0;
        p->u2 = (uint8_t)u_start;             p->v2 = v1;
        p->u3 = (uint8_t)(u_start + run - 1); p->v3 = v1;

        p->tpage = getTPage(0, 0, hud_page_x + page * 64, hud_page_y);
        p->clut  = hud_clut;

        addPrim(&ctx->buffers[ctx->active_buffer].ot[ot_idx], p);
        ctx->next_packet += sizeof(POLY_FT4);
        x += run;
    }
}

/* ---- Texture-window hygiene ----------------------------------------------
   Every textured thing the HUD draws needs the window OFF:
     - the panel is at Voff 128, and 320 px wide, so a 128 window wraps BOTH its
       v (128->0) and its u (0..255 -> 0..127 twice);
     - the crucifaxe icon is at Voff 128 too (the grave-olver, at Voff 0, is
       immune, which is why only one of the two ever looked wrong);
     - FntSort's font sits at u 0..255 of its page, so a 128 window shreds the
       log text as well.

   A single reset sorted a few OT buckets ABOVE the HUD is NOT enough. That is
   what this originally did, and in some spots the panel still came out sampling
   stove.tim / din_cl.tim — the two textures 128 lines above it in VRAM. Anything
   that lands in an OT bucket between the reset and the HUD content re-enables
   the window, and the reserved UI block 0-15 is shared with the puzzles, the
   poison wash and the debug overlay, so "nothing writes there" is not a property
   the HUD can rely on.

   So the window is disabled IN THE SAME OT BUCKET as the content it protects,
   exactly as zombie.c's add_ft4_windowed brackets its sprites. addPrim prepends,
   so calling this AFTER a bucket's prims puts the disable at the head of that
   bucket's list — adjacent to them, with nothing able to interpose. */
static void hud_disable_texwindow(RenderContext *ctx, int ot_idx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(DR_TWIN) > buf_end) return;
    RECT full = {0, 0, 0, 0};        /* mask 0 = no wrapping, full page */
    DR_TWIN *tw = (DR_TWIN *)ctx->next_packet;
    setTexWindow(tw, &full);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[ot_idx], tw);
    ctx->next_packet += sizeof(DR_TWIN);
}

/* ---- Ammo count ----------------------------------------------------------
   3x5 glyphs drawn as scaled tiles — the same chunky readout the old top-left
   HUD used, moved into the weapon box's bottom-right corner. */
static const uint8_t hud_digit_font[10][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
    {7,4,7,1,7},{7,4,7,5,7},{7,1,2,4,4},{7,5,7,5,7},{7,5,7,1,7},
};

static int hud_digit_count(int v) {
    int n = 1;
    while (v >= 10) { v /= 10; n++; }
    return n;
}

/* Top-left at (x,y) in SCREEN space. */
static void hud_draw_number(RenderContext *ctx, int value, int x, int y, int scale,
                            uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
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
                if (hud_digit_font[digit][row] & (4 >> col))
                    hud_rect(ctx, x0 + col * scale, y + row * scale,
                             scale, scale, r, g, b, ot_idx);
    }
}

/* ---- Log -----------------------------------------------------------------
   The box is 125px of usable width, i.e. 15 characters of the 8px debug font,
   and 4 lines tall. Messages are word-wrapped into that grid; when more lines
   are produced than fit, the OLDEST are dropped, so the newest line the player
   just triggered is always on screen.

   Nothing expires: a line stays up until newer lines push it out of the
   three-entry pickup_log (or off the top of the 4-row grid). */

static char hud_log_lines[LOG_ROWS][LOG_COLS + 1];
static int  hud_log_count;

static void hud_log_emit(const char *s, int len) {
    int i;
    if (len > LOG_COLS) len = LOG_COLS;
    if (hud_log_count == LOG_ROWS) {          /* full: scroll the oldest off */
        int r;
        for (r = 0; r < LOG_ROWS - 1; r++)
            for (i = 0; i <= LOG_COLS; i++)
                hud_log_lines[r][i] = hud_log_lines[r + 1][i];
        hud_log_count--;
    }
    for (i = 0; i < len; i++) hud_log_lines[hud_log_count][i] = s[i];
    hud_log_lines[hud_log_count][len] = '\0';
    hud_log_count++;
}

static int hud_log_empty(void) {
    int k;
    for (k = 0; k < PICKUP_MSG_COUNT; k++)
        if (pickup_log[k].live) return 0;
    return 1;
}

/* Break one message into LOG_COLS-wide lines, at spaces where possible. */
static void hud_log_wrap(const char *s) {
    while (*s) {
        int len = 0, brk = 0, take;
        while (*s == ' ') s++;
        if (!*s) return;
        while (s[len] && len < LOG_COLS) {
            if (s[len] == ' ') brk = len;
            len++;
        }
        /* Only break at a space if the line actually overruns AND there is a
           space to break at — a single over-long word is hard-split instead. */
        take = (s[len] && s[len] != ' ' && brk > 0) ? brk : len;
        hud_log_emit(s, take);
        s += take;
    }
}

static void hud_draw_log_text(RenderContext *ctx) {
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    int k;

    hud_log_count = 0;
    for (k = 0; k < PICKUP_MSG_COUNT; k++)
        if (pickup_log[k].live) hud_log_wrap(pickup_log[k].msg);

    for (k = 0; k < hud_log_count; k++)
        ctx->next_packet = FntSort(&ot[HUD_OT_TEXT], ctx->next_packet,
                                   LOG_TEXT_X, HUD_Y + LOG_TEXT_Y + k * 8,
                                   hud_log_lines[k]);

    /* The font spans u 0..255 of its page, so it needs the window off too. */
    if (hud_log_count) hud_disable_texwindow(ctx, HUD_OT_TEXT);
}

/* ---- Public draws --------------------------------------------------------- */

void hud_draw(RenderContext *ctx) {
    if (!hud_loaded) return;

    /* 1. The purple slab, exactly the panel's footprint. */
    hud_rect(ctx, 0, HUD_Y, HUD_PANEL_W, HUD_PANEL_H,
             HUD_BACK_R, HUD_BACK_G, HUD_BACK_B, HUD_OT_BACK);

    /* 2. Health, then stamina, in the two left boxes. */
    {
        int32_t hp = player_health;
        if (hp < 0) hp = 0;
        if (hp > MAX_HEALTH) hp = MAX_HEALTH;
        int hw = (int)((hp * BAR_W) / MAX_HEALTH);
        if (hw > 0)
            hud_rect(ctx, BAR_X, HUD_Y + HEALTH_Y, hw, BAR_H,
                     0, 0, 200, HUD_OT_FILL);
    }
    {
        int sw = (sprint_stamina * BAR_W) / SPRINT_STAMINA_MAX;
        if (sw > 0) {
            /* Red while sprint is locked out — exhausted or poisoned; green
               when it is available. (Unchanged from the old bar.) */
            int locked = sprint_cooldown || player_poisoned();
            hud_rect(ctx, BAR_X, HUD_Y + STAMINA_Y, sw, BAR_H,
                     locked ? 200 : 0, locked ? 0 : 200, 0, HUD_OT_FILL);
        }
    }

    /* 3. The equipped weapon, centred in the middle box. */
    menu_draw_weapon_icon(ctx, current_weapon,
                          WPN_X + (WPN_SIZE - WPN_ICON) / 2,
                          HUD_Y + WPN_Y + (WPN_SIZE - WPN_ICON) / 2,
                          WPN_ICON, HUD_OT_FILL);
    /* The crucifaxe icon is Voff 128 — same trap as the panel. */
    hud_disable_texwindow(ctx, HUD_OT_FILL);

    /* 4. The frame itself, over the slab and the live content. */
    hud_panel_span(ctx, 0, 0, HUD_PANEL_W - 1, HUD_PANEL_H - 1, HUD_OT_PANEL);
    hud_disable_texwindow(ctx, HUD_OT_PANEL);

    /* 5. Rounds CHAMBERED (not the reserve — the inventory menu carries that),
          tucked into the weapon box's bottom-right corner and tinted by the
          ammo type so a glance tells standard from flame. */
    if (current_weapon == WEAPON_GRAVEOLVER) {
        const AmmoInfo *ai = &ammo_info[graveolver_ammo];
        int n  = hud_digit_count(graveolver_loaded);
        int w  = n * 8 - 2;                       /* 6px glyph + 2px gap, scale 2 */
        int x  = WPN_X + WPN_SIZE - 2 - w;
        int y  = HUD_Y + WPN_Y + WPN_SIZE - 2 - 10;
        hud_draw_number(ctx, graveolver_loaded, x + 1, y + 1, 2,
                        0, 0, 0, HUD_OT_COUNT_SHADE);
        hud_draw_number(ctx, graveolver_loaded, x, y, 2,
                        ai->flash_r, ai->flash_g, ai->flash_b, HUD_OT_COUNT);
    }

    /* 6. The log, inside the right box. */
    hud_draw_log_text(ctx);
}

void hud_draw_log_only(RenderContext *ctx) {
    /* Nothing to say = nothing to draw. This is what keeps an empty box off a
       boss cutscene while still letting a puzzle's own lines through. */
    if (!hud_loaded || hud_log_empty()) return;
    /* Just the log box's own slice of the frame — its fill is opaque in the art,
       so it needs no backing slab of its own. */
    hud_panel_span(ctx, LOGBOX_X0, LOGBOX_Y0, LOGBOX_X1, LOGBOX_Y1, HUD_OT_PANEL);
    hud_disable_texwindow(ctx, HUD_OT_PANEL);
    hud_draw_log_text(ctx);
}
