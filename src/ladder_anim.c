#include <stdint.h>
#include <psxgpu.h>
#include "ladder_anim.h"
#include "tim_slots.h"
#include "sound.h"
#include "world.h"   /* world_silence_monsters */

/* ------------------------------------------------------------------ timing
   All frame counts @ 60fps. Fade up, hold, four pulls, fade out — and the fade
   out ENDS on the last frame rather than starting on it, so the screen is black
   at the cut and not merely dim (the catacomb walk's rule). */
#define LA_FADE_IN_FRAMES   30    /* 0.50 s up from black                     */
#define LA_WAIT_FRAMES      45    /* 0.75 s held: hands on the rungs          */
#define LA_INTRO_FRAMES     (LA_FADE_IN_FRAMES + LA_WAIT_FRAMES)   /* 75  */
#define LA_STEP_FRAMES      45    /* 0.75 s a pull — brisker than a stair    */
#define LA_STEP_COUNT        4
#define LA_STEPS_START      LA_INTRO_FRAMES                        /* 75  */
#define LA_STEPS_END        (LA_STEPS_START + LA_STEP_FRAMES * LA_STEP_COUNT)
                                                                   /* 255 */
#define LA_FADE_OUT_FRAMES  60    /* 1.00 s down, ending on the last frame    */
#define LA_TOTAL_FRAMES     (LA_STEPS_END + LA_FADE_OUT_FRAMES)    /* 315 */

/* ------------------------------------------------------ on-screen geometry
   One tile is one whole copy of LADDER.TIM, drawn square so the rungs keep the
   proportions the artist gave them. 192 across leaves 64 of black either side:
   the shaft beyond the reach of the light.

   Each tile is cut into LA_STRIPS horizontal strips, one per rung, because a
   single Gouraud quad can only interpolate LINEARLY between its top and bottom
   edges — a tile straddling the centre of the screen would come out evenly dim
   with no bright band in the middle. Four strips make the falloff a four-piece
   curve with its peak where the eye is.

   A pull climbs two rungs: half a tile. Four pulls climb two whole tiles, which
   the column's wrap below hides entirely. */
#define LA_SCR_CX          160
#define LA_SCR_CY          120
#define LA_TILE_W          192
#define LA_TILE_H          192
#define LA_STRIPS            4                        /* one per rung      */
#define LA_STRIP_H         (LA_TILE_H / LA_STRIPS)    /* 48 px             */
#define LA_STRIP_V         (128 / LA_STRIPS)          /* 32 texels         */
#define LA_PULL_PX         (LA_STRIP_H * 2)           /* two rungs: 96 px  */
#define LA_FALLOFF         200    /* px from the centre line to full black */

/* The ladder texture: 128x128 8bpp at page-top (Voff 0), so U/V span 0..127. */
#define LA_U0   0
#define LA_U1 127

static int32_t anim_timer  = 0;
static int     anim_active = 0;
static int     anim_dir    = LADDER_UP;

void ladder_anim_start(int direction) {
    anim_timer  = 0;
    anim_active = 1;
    anim_dir    = (direction == LADDER_DOWN) ? LADDER_DOWN : LADDER_UP;
    /* Same as the door and stair transitions: silence the monsters being left
       behind before the area update stops running. */
    world_silence_monsters();
}

int ladder_anim_active(void) {
    return anim_active;
}

void ladder_anim_update(void) {
    if (!anim_active) return;
    anim_timer++;
    /* A footfall at the START of each pull, alternating — a boot finding the
       next rung. Both clips are SND_RESIDENT (src/sound.h), so they play
       whatever bank the room being left was on. */
    for (int s = 0; s < LA_STEP_COUNT; s++)
        if (anim_timer == LA_STEPS_START + LA_STEP_FRAMES * s)
            sound_play((s & 1) ? SFX_STEP2 : SFX_STEP1);
}

int ladder_anim_finished(void) {
    if (anim_active && anim_timer >= LA_TOTAL_FRAMES) {
        anim_active = 0;
        return 1;
    }
    return 0;
}

/* Cumulative eased climb in 1/256ths of a pull, 0 .. LA_STEP_COUNT*256 —
   stair_anim's climb256(). Each pull eases OUT so it lurches and settles, which
   is what makes four pulls read as four and not one slow crane shot. Frozen at
   the top once the pulls are done and the fade has the screen. */
static int32_t climb256(void) {
    int32_t t = anim_timer - LA_STEPS_START;
    if (t < 0) t = 0;
    if (t > LA_STEP_FRAMES * LA_STEP_COUNT) t = LA_STEP_FRAMES * LA_STEP_COUNT;
    int32_t s     = t / LA_STEP_FRAMES;              /* completed pulls */
    int32_t local = t - s * LA_STEP_FRAMES;
    int32_t lt    = local * 256 / LA_STEP_FRAMES;    /* 0..255          */
    int32_t inv   = 256 - lt;
    int32_t eased = 256 - (inv * inv / 256);         /* ease-out 0..256 */
    return s * 256 + eased;
}

/* Vertex brightness at screen row y: `intensity` on the centre line, falling
   linearly to black LA_FALLOFF px above and below it. */
static uint8_t la_shade(int32_t y, int32_t intensity) {
    int32_t d = y - LA_SCR_CY;
    if (d < 0) d = -d;
    int32_t f = 256 - (d * 256) / LA_FALLOFF;
    if (f < 0) f = 0;
    return (uint8_t)((intensity * f) >> 8);
}

/* One rung-high strip of one tile: rows y0..y1 on screen, texels
   strip*LA_STRIP_V .. +LA_STRIP_V of the ladder. */
static void la_strip(RenderContext *ctx, uint8_t *buf_end,
                     int32_t y0, int32_t y1, int strip, int32_t intensity) {
    if (ctx->next_packet + sizeof(POLY_GT4) > buf_end) return;

    int32_t xl = LA_SCR_CX - LA_TILE_W / 2;
    int32_t xr = LA_SCR_CX + LA_TILE_W / 2;
    int32_t vt = strip * LA_STRIP_V;
    int32_t vb = vt + LA_STRIP_V;
    if (vb > 127) vb = 127;     /* row 128 is the next texture down the page */

    uint8_t ct = la_shade(y0, intensity);
    uint8_t cb = la_shade(y1, intensity);

    POLY_GT4 *poly = (POLY_GT4 *)ctx->next_packet;
    setPolyGT4(poly);
    setRGB0(poly, ct, ct, ct);
    setRGB1(poly, ct, ct, ct);
    setRGB2(poly, cb, cb, cb);
    setRGB3(poly, cb, cb, cb);
    poly->x0 = (int16_t)xl; poly->y0 = (int16_t)y0;
    poly->x1 = (int16_t)xr; poly->y1 = (int16_t)y0;
    poly->x2 = (int16_t)xl; poly->y2 = (int16_t)y1;
    poly->x3 = (int16_t)xr; poly->y3 = (int16_t)y1;
    poly->u0 = LA_U0; poly->v0 = (uint8_t)vt;
    poly->u1 = LA_U1; poly->v1 = (uint8_t)vt;
    poly->u2 = LA_U0; poly->v2 = (uint8_t)vb;
    poly->u3 = LA_U1; poly->v3 = (uint8_t)vb;
    poly->tpage = TIM_TPAGE_LADDER;
    poly->clut  = TIM_CLUT_LADDER;
    addPrim(&ctx->buffers[ctx->active_buffer].ot[200], poly);
    ctx->next_packet += sizeof(POLY_GT4);
}

/* ----------------------------------------------------------------- rendering */
void ladder_anim_draw(RenderContext *ctx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    /* Full-screen black behind everything. */
    if (ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *bg = (TILE *)ctx->next_packet;
        setTile(bg);
        setXY0(bg, 0, 0);
        setWH(bg, SCREEN_XRES, SCREEN_YRES);
        setRGB0(bg, 0, 0, 0);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
        ctx->next_packet += sizeof(TILE);
    }

    /* Full-page texture window: the UVs are 0..127 and must not be masked by a
       128 window the room being left happens to have set. */
    if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
        RECT     tw   = { 0, 0, 0, 0 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[300], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    /* Brightness: up from black, hold at 128 (the PS1's pass-through for a
       textured poly), then down to black over the last frames. */
    int32_t intensity;
    if (anim_timer < LA_FADE_IN_FRAMES) {
        intensity = 128 * anim_timer / LA_FADE_IN_FRAMES;
    } else if (anim_timer > LA_TOTAL_FRAMES - LA_FADE_OUT_FRAMES) {
        int32_t f = (anim_timer - (LA_TOTAL_FRAMES - LA_FADE_OUT_FRAMES)) * 256
                    / LA_FADE_OUT_FRAMES;
        if (f > 256) f = 256;
        intensity = 128 * (256 - f) / 256;
    } else {
        intensity = 128;
    }
    if (intensity <= 0) return;

    /* How far the column has scrolled. Climbing UP the camera rises, so the
       ladder slides DOWN the screen (+y); climbing down it slides up. */
    int32_t off = (climb256() * LA_PULL_PX) >> 8;
    if (anim_dir == LADDER_DOWN) off = -off;

    /* The column wraps by whole tiles, so only the phase within one tile
       matters. With the phase in [0, LA_TILE_H), tiles k = -2..1 around the
       centred one always cover rows 0..SCREEN_YRES: k=-2 is needed once the
       phase passes 168, k=1 while it is under 24. */
    int32_t phase = off % LA_TILE_H;
    if (phase < 0) phase += LA_TILE_H;
    int32_t top0 = LA_SCR_CY - LA_TILE_H / 2 + phase;

    for (int k = -2; k <= 1; k++) {
        int32_t ty = top0 + k * LA_TILE_H;
        for (int j = 0; j < LA_STRIPS; j++) {
            int32_t y0 = ty + j * LA_STRIP_H;
            int32_t y1 = y0 + LA_STRIP_H;
            if (y1 <= 0 || y0 >= SCREEN_YRES) continue;   /* off screen */
            la_strip(ctx, buf_end, y0, y1, j, intensity);
        }
    }
}
