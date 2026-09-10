#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxetc.h>
#include <psxcd.h>
#include <inline_c.h>
#include "render.h"
#include "splash.h"
#include "title.h"
#include "sound.h"

/* ---- Layout ----------------------------------------------------------------
   The logo is a 128x128 disc drawn 1:1 (no scaling), centred both ways on the
   320x240 screen. Its own art is 256x256; textures/menus/er_logo_128.png is the halved
   copy that goes on the disc, because at 4bpp 128x128 is 32 VRAM words by 128
   rows and that is the ONLY hole of that shape left in VRAM (x=672, y=128 —
   tools/VRAM_MAP.txt). The full-size original would have wanted four times the
   space and there is nowhere to put it.

   ROTATION: the quad's corners are swung about its centre, which means the
   drawn square is a rotating BOUNDING box — its corners reach out to the
   half-DIAGONAL, 90px rather than 64. That is why the roll starts at x = -112
   and not at -64: anything nearer and a corner of the (transparent) square is
   still on screen at the moment the animation starts, and while the corners are
   transparent the packet is not free. */
#define LOGO_PX        128
#define LOGO_HALF      (LOGO_PX / 2)
#define LOGO_CX        160          /* landing position: dead centre           */
#define LOGO_CY        120          /* ...of the screen, both ways             */
#define LOGO_START_CX (-112)        /* clear of the left edge, corners included */

/* The background yellow is lifted straight out of the logo's own 16-colour
   palette (entry 0), so the disc dissolves into the background at the flash and
   only the ring and the pistol are left standing on it. That effect is the
   whole point of the yellow. */
#define BG_R 248
#define BG_G 248
#define BG_B   0

/* ---- Phase lengths, in 60Hz frames ---------------------------------------
   Real frames, not pump calls: see the header. The two prelude phases are the
   only ones with movement in them and they are also the only ones that get the
   CPU to themselves, which is not a coincidence. */
#define ROLL_FRAMES      90   /* 1.5s  */
#define FLASH_FRAMES     24   /* 0.4s  */
#define FLASH_HOLD        4   /* frames held at the burst colour before the ramp */
#define FLASH_RAMP       16   /* frames of burst -> background yellow after that */
#define HOLD_MIN        210   /* 3.5s of stillness, minimum                    */
#define HOLD_MAX        420   /* 7s ceiling: past this the loading screen wins  */
#define FADE_FRAMES      40

/* The burst is a blown-out YELLOW, not white. White reads as a camera flash and
   the yellow that follows it as a separate event; blowing the logo's own hue out
   to near-white instead makes the two one movement — the screen floods with the
   colour the background is about to become. The gunshot fires on the same frame
   (see advance_phases), which is the other half of why it reads as one beat. */
#define FLASH_R 255
#define FLASH_G 255
#define FLASH_B 190

enum { PH_ROLL, PH_FLASH, PH_HOLD, PH_FADE, PH_DONE };

static int      phase        = PH_DONE;  /* no logo loaded -> nothing to play */
static int      phase_start  = 0;        /* vblank count the phase began on   */
static int      end_request  = 0;        /* startup block reported finished   */

static uint16_t logo_tpage = 0;          /* 0 = not loaded, splash disabled */
static uint16_t logo_clut  = 0;
static uint8_t  logo_u0 = 0, logo_v0 = 0, logo_u1 = 0, logo_v1 = 0;

/* ---- Asset ---------------------------------------------------------------- */

void splash_load_assets(void) {
    CdlFILE file;
    if (!CdSearchFile(&file, "\\TEX\\ERLOGO.TIM;1")) return;
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
        logo_clut = getClut(tim.crect->x, tim.crect->y);
    }

    /* UV within the tpage — same derivation as the loading screen's axe. At
       4bpp a VRAM word holds four texels, so the U offset is four times the
       texture's x within its 64-word page: (672 & 63) * 4 = 128, and the 128px
       image runs to u=255. V is the row within the 256-tall page: 128..255. */
    int bpp_mode = tim.mode & 3;
    int px_mult  = (bpp_mode == 0) ? 4 : (bpp_mode == 1) ? 2 : 1;
    int u_off    = (tim.prect->x & 63) * px_mult;
    logo_u0 = (uint8_t)u_off;
    logo_v0 = (uint8_t)(tim.prect->y % 256);
    logo_u1 = (uint8_t)(u_off + tim.prect->w * px_mult - 1);
    logo_v1 = (uint8_t)(logo_v0 + tim.prect->h - 1);

    logo_tpage = getTPage(bpp_mode, 0, tim.prect->x, tim.prect->y);
    free(buf);

    phase       = PH_ROLL;
    phase_start = VSync(-1);
}

/* ---- Timing ----------------------------------------------------------------
   Every phase asks the vblank counter how long it has been up rather than
   counting its own draws, because during the startup block a "frame" can be a
   whole CD read long. The animation therefore keeps real time and drops frames
   under load instead of playing back in slow motion. */
static int phase_frames(void) {
    int f = VSync(-1) - phase_start;
    return f < 0 ? 0 : f;   /* the counter is free-running; never go negative */
}

static void enter_phase(int p) {
    phase       = p;
    phase_start = VSync(-1);
}

static void advance_phases(void) {
    int f = phase_frames();
    switch (phase) {
        case PH_ROLL:
            if (f >= ROLL_FRAMES) {
                enter_phase(PH_FLASH);
                /* The gunshot goes off ON the transition, not in the PH_FLASH
                   draw — that runs every frame of the flash and would retrigger
                   the voice two dozen times. Silent if sound_splash_init() did
                   not run or could not find the clip: sound_play takes its
                   `!loaded` exit, which is the right answer for a flourish. */
                sound_play(SFX_GR_SHOT);
            }
            break;
        case PH_FLASH: if (f >= FLASH_FRAMES) enter_phase(PH_HOLD);  break;
        case PH_HOLD:
            /* Out on either the startup block finishing (once the minimum
               stillness has been served) or the ceiling — whichever is first. */
            if ((end_request && f >= HOLD_MIN) || f >= HOLD_MAX)
                enter_phase(PH_FADE);
            break;
        case PH_FADE:  if (f >= FADE_FRAMES)  phase = PH_DONE; break;
        default: break;
    }
}

/* ---- Drawing --------------------------------------------------------------- */

/* Quadratic ease-out over ONE units: t and the result both run 0..ONE. The logo
   comes in fast and settles onto the centre rather than arriving at full speed
   and stopping dead, which is what "lands" has to look like.
   >>> QUADRATIC, NOT CUBIC. <<< A cubic ease-out covers 42% of the travel in
   the first sixth of the roll and then spends the last half of it crawling
   through 34 pixels — it reads as a whip followed by a drift, not as something
   rolling. Squared it is: two thirds of the way across at the half-way frame,
   and the last fifteen frames are an eight-pixel settle. */
static int32_t ease_out(int32_t t) {
    int32_t inv = ONE - t;
    return ONE - ((inv * inv) >> 12);
}

/* Scale a colour channel by a 0..256 fade level. */
static uint8_t fade8(int v, int level) {
    return (uint8_t)((v * level) >> 8);
}

static void draw_bg(RenderContext *ctx, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;

    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setXY0(bg, 0, 0);
    setWH(bg, SCREEN_XRES, SCREEN_YRES);
    setRGB0(bg, r, g, b);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
    ctx->next_packet += sizeof(TILE);
}

/* The logo as a rotated POLY_FT4, centred on (cx, LOGO_CY) and turned `ang`
   ONE-units clockwise. `level` is the 0..256 fade, applied against the 128
   neutral so the art draws at its own brightness at full level. */
static void draw_logo(RenderContext *ctx, int32_t cx, int32_t ang, int level) {
    if (!logo_tpage) return;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) + sizeof(DR_TWIN) > buf_end) return;

    int32_t sn = isin(ang), cs = icos(ang);
    int32_t h  = LOGO_HALF;
    int32_t cos_h = (cs * h) >> 12;
    int32_t sin_h = (sn * h) >> 12;

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    {
        uint8_t l = fade8(128, level);   /* 128 = unmodulated on this hardware */
        setRGB0(poly, l, l, l);
    }
    setXY4(poly,
           cx - cos_h + sin_h, LOGO_CY - sin_h - cos_h,   /* top-left     */
           cx + cos_h + sin_h, LOGO_CY + sin_h - cos_h,   /* top-right    */
           cx - cos_h - sin_h, LOGO_CY - sin_h + cos_h,   /* bottom-left  */
           cx + cos_h - sin_h, LOGO_CY + sin_h + cos_h);  /* bottom-right */

    poly->u0 = logo_u0;  poly->v0 = logo_v0;
    poly->u1 = logo_u1;  poly->v1 = logo_v0;
    poly->u2 = logo_u0;  poly->v2 = logo_v1;
    poly->u3 = logo_u1;  poly->v3 = logo_v1;

    poly->tpage = logo_tpage;
    poly->clut  = logo_clut;
    addPrim(&ctx->buffers[ctx->active_buffer].ot[2], poly);
    ctx->next_packet += sizeof(POLY_FT4);

    /* Full-page texture window, for the reason the loading screen's axe resets
       one: this texture sits at Voff 128 and a 128x128 window left active by
       anything else would wrap its V and sample the page above it. Nothing has
       set a window this early in the boot, but the cost is one packet and the
       failure mode is a screen of garbage. Sorted BEHIND the logo (higher OT
       index) so the GPU applies it first. */
    {
        RECT full = {0, 0, 0, 0};
        DR_TWIN *tw = (DR_TWIN *)ctx->next_packet;
        setTexWindow(tw, &full);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[3], tw);
        ctx->next_packet += sizeof(DR_TWIN);
    }
}

static void splash_draw(RenderContext *ctx) {
    int f = phase_frames();

    switch (phase) {
        case PH_ROLL: {
            /* Black screen, logo rolling right. The turn is three full circles
               tied to the same eased progress as the travel, so it lands on
               exactly ONE * 3 — i.e. back at zero, upright — at the instant the
               logo reaches the centre. Increasing the angle swings the top-left
               corner right, which is clockwise on a y-down screen, which is the
               way a thing rolling to the right turns. */
            int32_t t     = (f * ONE) / ROLL_FRAMES;
            int32_t eased = ease_out(t > ONE ? ONE : t);
            int32_t cx    = LOGO_START_CX
                          + (((LOGO_CX - LOGO_START_CX) * eased) >> 12);
            int32_t ang   = (3 * eased) & (ONE - 1);
            draw_bg(ctx, 0, 0, 0);
            draw_logo(ctx, cx, ang, 256);
            break;
        }
        case PH_FLASH: {
            /* The burst holds for a few frames, then saturates down into the
               logo's own yellow and stays there as the new background. */
            uint8_t r = FLASH_R, g = FLASH_G, b = FLASH_B;
            if (f >= FLASH_HOLD) {
                int t = ((f - FLASH_HOLD) * 256) / FLASH_RAMP;
                if (t > 256) t = 256;
                r = (uint8_t)(FLASH_R - (((FLASH_R - BG_R) * t) >> 8));
                g = (uint8_t)(FLASH_G - (((FLASH_G - BG_G) * t) >> 8));
                b = (uint8_t)(FLASH_B - (((FLASH_B - BG_B) * t) >> 8));
            }
            draw_bg(ctx, r, g, b);
            draw_logo(ctx, LOGO_CX, 0, 256);
            break;
        }
        case PH_HOLD:
            draw_bg(ctx, BG_R, BG_G, BG_B);
            draw_logo(ctx, LOGO_CX, 0, 256);
            break;
        case PH_FADE: {
            int level = 256 - ((f * 256) / FADE_FRAMES);
            if (level < 0) level = 0;
            draw_bg(ctx, fade8(BG_R, level), fade8(BG_G, level),
                         fade8(BG_B, level));
            draw_logo(ctx, LOGO_CX, 0, level);
            break;
        }
        default:
            break;
    }
}

/* ---- Public --------------------------------------------------------------- */

int splash_active(void) {
    return phase != PH_DONE;
}

void splash_pump(RenderContext *ctx) {
    if (phase == PH_DONE) return;
    advance_phases();
    if (phase == PH_DONE) return;
    splash_draw(ctx);
    flip_buffers(ctx);
}

void splash_prelude(RenderContext *ctx) {
    if (phase == PH_DONE) {
        /* No logo on the disc. Fall back to what main() did before this
           existed: prime BOTH framebuffers with the loading screen, so the
           startup block's first blocking read has something on the television
           whichever buffer it lands on. */
        draw_loading_screen(ctx);
        flip_buffers(ctx);
        draw_loading_screen(ctx);
        flip_buffers(ctx);
        DrawSync(0);
        return;
    }

    /* Start the clock HERE, not in splash_load_assets(): the axe icon is read
       off the disc between the two and the roll would otherwise open part-way
       through itself. */
    enter_phase(PH_ROLL);

    /* ROLL and FLASH at a true 60fps, with the disc idle behind them. Both
       phases have something moving in them and neither can share the CPU with
       a blocking read; everything after the flash can, and does. */
    while (phase == PH_ROLL || phase == PH_FLASH) splash_pump(ctx);

    /* Not optional, for the reason loading_screen_pump ends with one: the last
       flip kicked an ASYNCHRONOUS DrawOTagEnv, and the next thing to run is a
       load step whose first act is usually a LoadImage. */
    DrawSync(0);
}

void splash_finish(RenderContext *ctx) {
    end_request = 1;
    while (phase != PH_DONE) splash_pump(ctx);
    DrawSync(0);   /* FntLoad and the HUD's LoadImage are next — see above */
}
