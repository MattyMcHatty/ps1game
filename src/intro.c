#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxcd.h>
#include <psxpad.h>
#include "render.h"
#include "intro.h"
#include "title.h"
#include "cdaudio.h"
#include "sound.h"

/* ============================================================================
   OPENING SEQUENCE
   ============================================================================
   Runs between "NEW GAME" on the title screen and the first frame of the
   Delivery Area (main.c's STATE_INTRO branch). The whole thing is one frame
   counter, `t`, with the phase boundaries below computed from it.

     t = 0                 the title's menu text has already vanished (main.c
                           simply stops calling draw_title), leaving the title
                           over the purple background. The screen flashes white
                           and the Order of Ninurta line plays
     .. F_FLASH_HOLD       the title sits under the burning-off flash
     .. F_VOICE            the line plays out; then the music starts
     .. F_TITLE_FADE       the title and background fade together to black
     .. + F_TITLE_HOLD     black
     T_B1 ..               block 1: three lines fade in one at a time, hold,
                           fade out together
     T_B2 ..               block 2: the same, three more lines
     T_B3 ..               block 3: the mansion fades in top-left, then two
                           lines; then everything AND the music fade out
     T_END                 done -> Delivery Area

   THE BACKGROUND IS THE FRAMEBUFFER CLEAR COLOUR, not a drawn tile (same as the
   title screen — see title.h). intro_draw writes it into both draw_envs every
   frame, which is what makes the 3-second title fade possible at all.

   TEXT is drawn as 1x1..5x1 TILEs from the 5x7 bitmaps below, one primitive per
   horizontal run of lit pixels. That is the only way to get a smooth fade: the
   SDK's font streams (FntPrint/FntSort) draw at a fixed brightness with no
   per-primitive colour, so they cannot be faded at all. The longest line here
   ("WHEN DARKNESS FALLS, ...") is 277px wide at the advances below, so there is
   about 20px of slack each side — check any reworded line against ADV_*.

   MUSIC is the Reception track, started here and faded out on the drive's mixer
   under the last block (cdaudio_set_mix; see the CD_MIX_* note in cdaudio.c for
   why that stage and not the SPU one). It is STOPPED before the sequence hands
   over, because the Delivery Area is entered directly and reads its mesh off
   the disc — a data read issued while CD-DA streams hangs the drive.          */

/* ------------------------------------------------------------------- timing */
#define F_FLASH_OPAQUE     4   /* frames of PURE white — the pop                */
#define F_FLASH_DECAY     26   /* frames the wash burns off over                */
#define F_VOICE          300   /* 5.0s: the Ninurta line (4.82s) plays out over
                                  the held title, then the music comes in       */
#define F_TITLE_FADE     180   /* 3.0s title + background -> black              */
#define F_TITLE_HOLD      36   /* 0.6s of black before the first line           */

/* The two WAITS — how long a line sits before the next one starts, and how long
   the finished block sits before it goes — are deliberately long: this is text
   the player has to read, and reading pace, not fade pace, sets it. The FADE
   durations below are separate and stay short. */
#define LINE_FADE         45   /* 0.75s for one line to reach full brightness   */
#define LINE_STAGGER     110   /* 1.83s between successive lines starting       */
#define BLOCK_HOLD       240   /* 4.0s with the whole block up                  */
#define BLOCK_FADE        60   /* 1.0s for a block to fade out                  */
#define BLOCK_GAP         36   /* 0.6s of black between blocks                  */

#define IMG_FADE          60   /* 1.0s for the mansion to fade in (block 3)     */
#define FINAL_FADE        90   /* 1.5s: last text + mansion + music fade out    */
#define END_HOLD          30   /* 0.5s of black before the game starts          */

#define SKIP_ARM         120   /* 2.0s before Start is allowed to skip          */

#define B1_LINES           3
#define B2_LINES           4   /* the Order's pledge is split across two lines */
#define B3_LINES           2

#define BLOCK_LEN(n)     ((n - 1) * LINE_STAGGER + LINE_FADE + BLOCK_HOLD + BLOCK_FADE)

#define T_FADE (F_VOICE)                       /* when the title starts to go   */
#define T_B1   (T_FADE + F_TITLE_FADE + F_TITLE_HOLD)
#define T_B2   (T_B1 + BLOCK_LEN(B1_LINES) + BLOCK_GAP)
#define T_B3   (T_B2 + BLOCK_LEN(B2_LINES) + BLOCK_GAP)
#define B3_LEN (IMG_FADE + (B3_LINES - 1) * LINE_STAGGER + LINE_FADE \
                + BLOCK_HOLD + FINAL_FADE)
#define T_END  (T_B3 + B3_LEN + END_HOLD)

/* ------------------------------------------------------------------- layout */
#define TEXT_R           235   /* full-brightness text colour (bone white)      */
#define TEXT_G           230
#define TEXT_B           215

#define GLYPH_W            5
#define GLYPH_H            7
#define ADV_LETTER         6   /* 5px cell + 1px gap                            */
#define ADV_SPACE          4   /* narrower than a letter: the longest line only
                                  fits inside 320px because of this             */
#define ADV_DOT            3   /* so "..." reads as an ellipsis, not three stops */
#define LINE_SPACING      14

/* The two full-screen blocks are centred on this line, so a three-line and a
   four-line block both sit in the middle of the screen rather than the
   four-line one hanging low. */
#define BLOCK12_MID      120
#define BLOCK12_TOP(n)   (BLOCK12_MID - (n) * LINE_SPACING / 2)

#define MANSION_X         16   /* the still, top-left                            */
#define MANSION_Y         16
#define MANSION_SIZE      96   /* drawn size; the source TIM is 128x128          */
#define B3_TEXT_TOP      150   /* block 3's text sits below the picture          */

/* MANSION.TIM lives at VRAM x[448,512) y[128,256), 8bpp — see tools/VRAM_MAP.txt.
   y=128 within a 256-tall page puts it at Voff 128, so its V range is 128..255
   and a stale 128x128 texture window left behind by a room would wrap it back
   onto the wrong art. intro_draw sorts a full window reset above it. */
#define MANSION_U0         0
#define MANSION_U1       127
#define MANSION_V0       128
#define MANSION_V1       255

/* OT buckets. Higher index draws first, so: window reset, then the picture,
   then the text over it — and the opening flash last of all, over everything
   including the title letters (which title.c draws at index 1). */
#define OT_FLASH           0
#define OT_TEXT          100
#define OT_IMAGE         200
#define OT_TWIN          300

/* ------------------------------------------------------------ the 5x7 font */
/* One byte per row, top row first, bit 0 = leftmost column (same convention as
   src/btn_glyph.c's button glyphs). Uppercase only, plus the two punctuation
   marks the script uses; anything else renders as a blank cell. */
static const uint8_t FONT[26][GLYPH_H] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
    {0x0F,0x11,0x11,0x0F,0x11,0x11,0x0F}, /* B */
    {0x0E,0x11,0x01,0x01,0x01,0x11,0x0E}, /* C */
    {0x0F,0x11,0x11,0x11,0x11,0x11,0x0F}, /* D */
    {0x1F,0x01,0x01,0x0F,0x01,0x01,0x1F}, /* E */
    {0x1F,0x01,0x01,0x0F,0x01,0x01,0x01}, /* F */
    {0x0E,0x11,0x01,0x1D,0x11,0x11,0x0E}, /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}, /* I */
    {0x1C,0x08,0x08,0x08,0x08,0x09,0x06}, /* J */
    {0x11,0x09,0x05,0x03,0x05,0x09,0x11}, /* K */
    {0x01,0x01,0x01,0x01,0x01,0x01,0x1F}, /* L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x13,0x15,0x19,0x11,0x11,0x11}, /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
    {0x0F,0x11,0x11,0x0F,0x01,0x01,0x01}, /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x09,0x16}, /* Q */
    {0x0F,0x11,0x11,0x0F,0x05,0x09,0x11}, /* R */
    {0x1E,0x01,0x01,0x0E,0x10,0x10,0x0F}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
    {0x1F,0x10,0x08,0x04,0x02,0x01,0x1F}, /* Z */
};

static const uint8_t GLYPH_COMMA[GLYPH_H] = {0,0,0,0,0x04,0x04,0x02};
static const uint8_t GLYPH_DOT[GLYPH_H]   = {0,0,0,0,0,0x01,0x01};
/* The three the trial-end screen adds (src/trial_end.c): a handle, a
   contraction and an exclamation. Same convention — bit 0 is the left column. */
static const uint8_t GLYPH_AT[GLYPH_H]    = {0x0E,0x11,0x1D,0x15,0x1D,0x01,0x0E};
static const uint8_t GLYPH_APOS[GLYPH_H]  = {0x04,0x04,0x02,0,0,0,0};
static const uint8_t GLYPH_BANG[GLYPH_H]  = {0x04,0x04,0x04,0x04,0x04,0,0x04};

static const uint8_t *glyph_for(char c) {
    if (c >= 'A' && c <= 'Z') return FONT[c - 'A'];
    if (c == ',')             return GLYPH_COMMA;
    if (c == '.')             return GLYPH_DOT;
    if (c == '@')             return GLYPH_AT;
    if (c == '\'')            return GLYPH_APOS;
    if (c == '!')             return GLYPH_BANG;
    return 0;                  /* space, and anything unmapped */
}

static int advance_for(char c) {
    if (c == ' ')  return ADV_SPACE;
    if (c == '.')  return ADV_DOT;
    if (c == '\'') return ADV_DOT + 1;   /* narrow, but not as tight as a stop */
    if (c == '!')  return ADV_SPACE;
    return ADV_LETTER;
}

/* ------------------------------------------------------------- the script */
/* Written in the caps the font supports; "..." stands in for the ellipsis. */
static const char *const BLOCK1[3] = {
    "WHEN DARKNESS FALLS, THE DEMONS COME OUT TO DANCE",
    "EVIL BEYOND COMPREHENSION",
    "THAT FEEDS ON THE ILLS OF MEN",
};
static const char *const BLOCK2[4] = {
    "CENTURIES AGO THE ORDER OF NINURTA",
    "PLEDGED TO FIGHT THE DEMONS",
    "AND PROTECT THE HUMAN WORLD",
    "ONLY ONE MEMBER OF THEIR ORDER REMAINS...",
};
static const char *const BLOCK3[2] = {
    "ALL ROADS LEAD HERE",
    "FOR THE EVIL CANNOT BE ALLOWED TO BE SATIATED...",
};

/* ------------------------------------------------------------------- state */
static int32_t  t          = 0;
static int      active     = 0;
static uint16_t tex_tpage  = 0;
static uint16_t tex_clut   = 0;
static int      tex_loaded = 0;
static uint16_t prev_held  = 0;   /* pad edge state for the Start skip */

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ------------------------------------------------------------ asset loading */
/* STARTUP ONLY. LoadImage is unsafe once the per-frame draw loop is running —
   see tools/TEXTURING_NOTES.txt PROBLEM A. Modelled on door_anim's loader. */
void intro_load_assets(void) {
    CdlFILE file;
    if (!CdSearchFile(&file, "\\TEX\\MANSION.TIM;1")) return;

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
        tex_clut = getClut(tim.crect->x, tim.crect->y);
    }
    tex_tpage = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);

    free(buf);
    tex_loaded = 1;
}

/* ------------------------------------------------------------ state machine */
void intro_start(void) {
    t         = 0;
    active    = 1;
    prev_held = 0xFFFF;   /* whatever is held on the first frame is not an edge */

    /* The voice lives in a bank of its own. This is normally a no-op — the
       title screen already holds SND_BANK_INTRO, loaded at startup by
       sound_init and put back by main.c whenever the game returns to the title
       — so the flash lands on the frame the button is pressed rather than
       behind a quarter-second of drive time. It is called anyway so the voice
       cannot be silently missing if some future path reaches the title with
       another bank in.

       No music yet: it comes in at T_FADE, when the title starts to go. */
    sound_bank_select(SND_BANK_INTRO);
    sound_play(SFX_NINURTA);
}

/* End the sequence: silence the drive so the Delivery Area's mesh read (issued
   the moment main.c leaves this state) is not competing with CD-DA, and cut the
   voice, which on a skip is still mid-sentence. */
static void intro_end(void) {
    sound_stop(SFX_NINURTA);
    cdaudio_stop();
    active = 0;
    t      = T_END;
}

void intro_update(void) {
    if (!active) return;
    t++;

    /* Start skips, but only once the title fade has had its two seconds — long
       enough that a player still holding the pad from the menu cannot lose the
       opening by accident. */
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        uint16_t held    = ~pad->btn;
        uint16_t pressed = held & ~prev_held;
        prev_held        = held;
        if (t >= SKIP_ARM && (pressed & PAD_START)) {
            intro_end();
            return;
        }
    }

    /* The music starts as the title begins to fade, once the voice has had its
       five seconds. Reception's track, at full level — the fade-out at the end
       runs on the drive mixer, and cdaudio_play re-asserts the level every time
       it is called, so the Delivery Area's music comes up loud again by itself. */
    if (t == T_FADE) cdaudio_play(CDAUDIO_RECEPTION_TRACK, 1);

    /* The music rides the last fade down with the picture and the text. */
    if (t >= T_B3 + B3_LEN - FINAL_FADE) {
        int32_t f = t - (T_B3 + B3_LEN - FINAL_FADE);
        if (f > FINAL_FADE) f = FINAL_FADE;
        cdaudio_set_mix(cdaudio_mix_full() * (FINAL_FADE - f) / FINAL_FADE);
    }

    if (t >= T_END) intro_end();
}

int intro_finished(void) {
    if (!active && t >= T_END) {
        t = T_END + 1;   /* report finished exactly once */
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ drawing */
static void tile_run(RenderContext *ctx, int x, int y, int w,
                     uint8_t r, uint8_t g, uint8_t b, int ot) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    TILE *tl = (TILE *)ctx->next_packet;
    setTile(tl);
    setXY0(tl, x, y);
    setWH(tl, w, 1);
    setRGB0(tl, r, g, b);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[ot], tl);
    ctx->next_packet += sizeof(TILE);
}

int intro_text_width(const char *s) {
    int w = 0;
    while (*s) w += advance_for(*s++);
    return w ? w - 1 : 0;   /* the trailing 1px inter-letter gap is not width */
}

/* One line, horizontally centred, at brightness `level` (0-255), into OT bucket
   `ot`. Each glyph row is emitted as horizontal RUNS of lit pixels rather than
   one TILE per pixel, which roughly halves the primitive count for a screenful
   of text.

   PUBLIC because the trial-end screen (src/trial_end.c) fades text on in exactly
   the same way and for the same reason the opening sequence does: the SDK's font
   streams draw at a fixed brightness, so they cannot be faded at all. */
void intro_text_draw(RenderContext *ctx, const char *s, int y, int level, int ot) {
    if (level <= 0) return;
    uint8_t r = (uint8_t)(TEXT_R * level / 255);
    uint8_t g = (uint8_t)(TEXT_G * level / 255);
    uint8_t b = (uint8_t)(TEXT_B * level / 255);

    int x = (SCREEN_XRES - intro_text_width(s)) / 2;
    for (; *s; s++) {
        const uint8_t *gl = glyph_for(*s);
        if (gl) {
            int row;
            for (row = 0; row < GLYPH_H; row++) {
                uint8_t bits = gl[row];
                int col = 0;
                while (col < GLYPH_W) {
                    if (!(bits & (1 << col))) { col++; continue; }
                    int start = col;
                    while (col < GLYPH_W && (bits & (1 << col))) col++;
                    tile_run(ctx, x + start, y + row, col - start, r, g, b, ot);
                }
            }
        }
        x += advance_for(*s);
    }
}

static void draw_line(RenderContext *ctx, const char *s, int y, int level) {
    intro_text_draw(ctx, s, y, level, OT_TEXT);
}

/* Brightness of line `line` of a block, given the block-local time `bt`.
   `delay` shifts the whole block's fade-in (block 3 waits for the picture), and
   the block-wide fade-out clamps every line at once. */
static int line_level(int32_t bt, int line, int delay,
                      int32_t fade_out_start, int32_t fade_out_len) {
    int32_t in_start = delay + line * LINE_STAGGER;
    if (bt <= in_start) return 0;

    int32_t v = (bt - in_start) * 255 / LINE_FADE;
    if (v > 255) v = 255;

    if (bt >= fade_out_start) {
        int32_t f = 255 - (bt - fade_out_start) * 255 / fade_out_len;
        if (f < 0) f = 0;
        if (f < v) v = f;
    }
    return (int)v;
}

static void draw_block(RenderContext *ctx, const char *const *lines, int count,
                       int32_t bt, int delay, int top,
                       int32_t fade_out_start, int32_t fade_out_len) {
    int i;
    for (i = 0; i < count; i++)
        draw_line(ctx, lines[i], top + i * LINE_SPACING,
                  line_level(bt, i, delay, fade_out_start, fade_out_len));
}

/* The mansion still, top-left, at `level` (0-255). Textured primitives take 128
   as "unmodulated", so full brightness is 128 — same convention door_anim uses
   for its panels. */
static void draw_mansion(RenderContext *ctx, int level) {
    if (!tex_loaded || level <= 0) return;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;

    uint8_t i = (uint8_t)(128 * level / 255);
    int x0 = MANSION_X, y0 = MANSION_Y;
    int x1 = MANSION_X + MANSION_SIZE, y1 = MANSION_Y + MANSION_SIZE;

    POLY_FT4 *p = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(p);
    setRGB0(p, i, i, i);
    p->x0 = (int16_t)x0; p->y0 = (int16_t)y0;
    p->x1 = (int16_t)x1; p->y1 = (int16_t)y0;
    p->x2 = (int16_t)x0; p->y2 = (int16_t)y1;
    p->x3 = (int16_t)x1; p->y3 = (int16_t)y1;
    p->u0 = MANSION_U0; p->v0 = MANSION_V0;
    p->u1 = MANSION_U1; p->v1 = MANSION_V0;
    p->u2 = MANSION_U0; p->v2 = MANSION_V1;
    p->u3 = MANSION_U1; p->v3 = MANSION_V1;
    p->tpage = tex_tpage;
    p->clut  = tex_clut;
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_IMAGE], p);
    ctx->next_packet += sizeof(POLY_FT4);
}

void intro_draw(RenderContext *ctx) {
    /* Background: the title's purple for the first three seconds, easing to
       black, and black for the rest. Written into BOTH draw_envs because either
       may be the next one drawn into. */
    {
        /* Full purple until T_FADE (the title is simply held while the voice
           plays), then down to black over F_TITLE_FADE. */
        int32_t ft = t - T_FADE;
        int32_t k  = (ft < 0)             ? F_TITLE_FADE
                   : (ft < F_TITLE_FADE)  ? F_TITLE_FADE - ft
                                          : 0;
        uint8_t r = (uint8_t)(TITLE_BG_R * k / F_TITLE_FADE);
        uint8_t g = (uint8_t)(TITLE_BG_G * k / F_TITLE_FADE);
        uint8_t b = (uint8_t)(TITLE_BG_B * k / F_TITLE_FADE);
        setRGB0(&ctx->buffers[0].draw_env, r, g, b);
        setRGB0(&ctx->buffers[1].draw_env, r, g, b);

        /* The title fades with it. No pulse — it is on its way out. */
        if (ft < F_TITLE_FADE)
            title_draw_logo(ctx, (uint8_t)(220 * k / F_TITLE_FADE), 0, 0);
    }

    /* The opening white flash, over everything. Modelled on the Grave-olver's
       muzzle flash (src/graveolver.c): a semi-transparent wash whose TILE is
       added BEFORE the DR_TPAGE that selects 50% blending, because the OT node
       is LIFO and the GPU must see the blend mode first. The difference is the
       front end — four frames of OPAQUE white, so it reads as a camera flash
       rather than a haze, before the wash burns off underneath it. */
    if (t <= F_FLASH_OPAQUE + F_FLASH_DECAY) {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        int opaque = (t <= F_FLASH_OPAQUE);
        int32_t v  = 255;
        if (!opaque) {
            v = 255 - (t - F_FLASH_OPAQUE) * 255 / F_FLASH_DECAY;
            if (v < 0) v = 0;
        }
        if (v > 0 && ctx->next_packet + sizeof(TILE) <= buf_end) {
            TILE *tl = (TILE *)ctx->next_packet;
            setTile(tl);
            if (!opaque) setSemiTrans(tl, 1);
            setRGB0(tl, (uint8_t)v, (uint8_t)v, (uint8_t)v);
            setXY0(tl, 0, 0);
            setWH(tl, SCREEN_XRES, SCREEN_YRES);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_FLASH], tl);
            ctx->next_packet += sizeof(TILE);
        }
        if (!opaque && ctx->next_packet + sizeof(DR_TPAGE) <= buf_end) {
            DR_TPAGE *dp = (DR_TPAGE *)ctx->next_packet;
            setDrawTPage(dp, 0, 0, getTPage(0, 0, 0, 0));   /* abr 0 = 50% */
            addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_FLASH], dp);
            ctx->next_packet += sizeof(DR_TPAGE);
        }
    }

    /* Full texture-window reset, so the mansion's V range (128..255) is not
       wrapped by a 128x128 window a room left behind. Sorted above the picture,
       and nothing else in this state uses a window. */
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            RECT     tw   = { 0, 0, 0, 0 };
            DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
            setTexWindow(twin, &tw);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_TWIN], twin);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    if (t >= T_B1 && t < T_B1 + BLOCK_LEN(B1_LINES)) {
        draw_block(ctx, BLOCK1, B1_LINES, t - T_B1, 0, BLOCK12_TOP(B1_LINES),
                   BLOCK_LEN(B1_LINES) - BLOCK_FADE, BLOCK_FADE);
    } else if (t >= T_B2 && t < T_B2 + BLOCK_LEN(B2_LINES)) {
        draw_block(ctx, BLOCK2, B2_LINES, t - T_B2, 0, BLOCK12_TOP(B2_LINES),
                   BLOCK_LEN(B2_LINES) - BLOCK_FADE, BLOCK_FADE);
    } else if (t >= T_B3 && t < T_B3 + B3_LEN) {
        int32_t bt = t - T_B3;

        /* The picture leads: it fades in over IMG_FADE, holds while the two
           lines arrive, and goes out with them on the final fade. */
        int level = (int)(bt * 255 / IMG_FADE);
        if (level > 255) level = 255;
        if (bt >= B3_LEN - FINAL_FADE) {
            int32_t f = 255 - (bt - (B3_LEN - FINAL_FADE)) * 255 / FINAL_FADE;
            if (f < 0) f = 0;
            if (f < level) level = (int)f;
        }
        draw_mansion(ctx, level);

        draw_block(ctx, BLOCK3, B3_LINES, bt, IMG_FADE, B3_TEXT_TOP,
                   B3_LEN - FINAL_FADE, FINAL_FADE);
    }
}
