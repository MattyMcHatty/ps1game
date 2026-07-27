#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include "stair_anim.h"
#include "sound.h"

/* ------------------------------------------------------------------ timing
   Fade in from black, hold ~1s, then three step-ups (~1s each), then a 1s fade
   to black. Footsteps land on the step boundaries: step1 as the first step
   begins, then step2/step1/step2 at the end of steps 1/2/3 (alternating A/B
   footfalls). All frame counts @ 60fps. */
#define FADE_IN_FRAMES 30    /* 0.5s image fades up from black                  */
#define WAIT_FRAMES    60    /* 1.0s hold before the first footstep + movement  */
#define INTRO_FRAMES   (FADE_IN_FRAMES + WAIT_FRAMES)   /* 90 */
#define STEP_FRAMES    60
#define STEP_COUNT      3
#define FADE_FRAMES    60
#define STEPS_START    INTRO_FRAMES                      /* 90  */
#define STEPS_END      (STEPS_START + STEP_FRAMES * STEP_COUNT)  /* 270 */
#define TOTAL_FRAMES   (STEPS_END + FADE_FRAMES)         /* 330 */

/* ------------------------------------------------------ on-screen geometry
   The upstairs image is a centred quad; each step nudges the "camera" a little
   forward (the quad zooms in about screen centre) and up/down (it slides on Y).
   Motion is eased per step so each one reads as a distinct lurch. */
#define SCR_CX        160
#define SCR_CY        120
#define QUAD_HW       130     /* half width  of the base quad */
#define QUAD_HH       110     /* half height of the base quad */
#define STEP_DY        18     /* screen pixels the image slides per step        */
#define ZOOM_TOTAL    144     /* extra scale (256 = 1.0x) at full 3-step climb  */

/* Upstairs texture: 128x128 4bpp at page-top (Voff 0), so U/V span 0..127. */
#define UPS_U0   0
#define UPS_U1 127
#define UPS_V0   0
#define UPS_V1 127

static int32_t  anim_timer  = 0;
static int      anim_active = 0;
static int      anim_dir    = STAIR_UP;

static uint16_t ups_tpage = 0;
static uint16_t ups_clut  = 0;
static int      tex_ready  = 0;

/* Read the upstairs TIM header at startup to capture its tpage/clut. No
   LoadImage — the pixels are already resident (uploaded by the conservatory /
   hall on entry) whenever this transition actually runs. */
void stair_anim_load_assets(void) {
    CdlFILE file;
    if (!CdSearchFile(&file, "\\TEX\\UPSTAIRS.TIM;1")) return;
    int   sectors = (file.size + 2047) / 2048;
    void *buf     = malloc(sectors * 2048);
    if (!buf) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);

    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    if (tim.mode & 0x8) ups_clut = getClut(tim.crect->x, tim.crect->y);
    ups_tpage = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);
    tex_ready = 1;
    free(buf);
}

/* ------------------------------------------------------------- state machine */
void stair_anim_start(int direction) {
    anim_timer  = 0;
    anim_active = 1;
    anim_dir    = (direction == STAIR_DOWN) ? STAIR_DOWN : STAIR_UP;
    /* No sound yet: the image fades in and holds for INTRO_FRAMES first. */
}

void stair_anim_update(void) {
    if (!anim_active) return;
    anim_timer++;
    /* Footfalls: step1 as movement begins (after the fade-in + 1s hold), then
       step2/step1/step2 at the end of steps 1/2/3. */
    if (anim_timer == STEPS_START)                   sound_play(SFX_STEP1);
    if (anim_timer == STEPS_START + STEP_FRAMES)     sound_play(SFX_STEP2);
    if (anim_timer == STEPS_START + STEP_FRAMES * 2) sound_play(SFX_STEP1);
    if (anim_timer == STEPS_START + STEP_FRAMES * 3) sound_play(SFX_STEP2);
}

int stair_anim_finished(void) {
    if (anim_active && anim_timer >= TOTAL_FRAMES) {
        anim_active = 0;
        return 1;
    }
    return 0;
}

/* Cumulative eased climb in 1/256-of-a-step units (0 .. STEP_COUNT*256). Each
   step eases out so the rise lurches then settles. Frozen at the top once the
   steps finish and the fade takes over. */
static int32_t climb256(void) {
    int32_t t = anim_timer - STEPS_START;          /* movement starts after the intro */
    if (t < 0) t = 0;
    if (t > STEP_FRAMES * STEP_COUNT) t = STEP_FRAMES * STEP_COUNT;
    int32_t s     = t / STEP_FRAMES;               /* completed steps 0..3   */
    int32_t local = t - s * STEP_FRAMES;           /* frames into this step  */
    int32_t lt    = local * 256 / STEP_FRAMES;     /* 0..255                 */
    int32_t inv   = 256 - lt;
    int32_t eased = 256 - (inv * inv / 256);       /* ease-out 0..256        */
    return s * 256 + eased;                          /* 0 .. STEP_COUNT*256   */
}

/* ----------------------------------------------------------------- rendering */
void stair_anim_draw(RenderContext *ctx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    /* Full-screen black background. */
    if (ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *bg = (TILE *)ctx->next_packet;
        setTile(bg);
        setXY0(bg, 0, 0);
        setWH(bg, SCREEN_XRES, SCREEN_YRES);
        setRGB0(bg, 0, 0, 0);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
        ctx->next_packet += sizeof(TILE);
    }

    if (!tex_ready) return;

    /* Full-page texture window so the upstairs UVs (0..127) don't wrap. */
    if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
        RECT     tw   = { 0, 0, 0, 0 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[300], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    int32_t c    = climb256();                       /* 0 .. STEP_COUNT*256 */
    int32_t cmax = STEP_COUNT * 256;

    /* Camera forward -> zoom in about screen centre. */
    int32_t scale   = 256 + (ZOOM_TOTAL * c) / cmax;  /* 256 = 1.0x */
    /* Camera up (climb) slides the image DOWN; descending slides it UP. */
    int32_t dir_sig = (anim_dir == STAIR_DOWN) ? -1 : 1;
    int32_t shift_y = dir_sig * (STEP_DY * STEP_COUNT * c) / cmax;

    /* Brightness: fade up from black over FADE_IN_FRAMES, hold at texture-neutral
       128 through the wait + climb, then fade to black over the final FADE_FRAMES
       (128 is PS1's pass-through for a textured poly, so the image shows at its
       natural brightness). */
    int32_t intensity = 128;
    if (anim_timer < FADE_IN_FRAMES) {
        intensity = 128 * anim_timer / FADE_IN_FRAMES;      /* 0 -> 128 */
    } else if (anim_timer > STEPS_END) {
        int32_t fade = (anim_timer - STEPS_END) * 256 / FADE_FRAMES;
        if (fade > 256) fade = 256;
        intensity = 128 * (256 - fade) / 256;               /* 128 -> 0 */
    }

    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;

    /* Base corners, scaled about (SCR_CX,SCR_CY) then shifted on Y. */
#define SX(x) (SCR_CX + ((x) - SCR_CX) * scale / 256)
#define SY(y) (SCR_CY + ((y) - SCR_CY) * scale / 256 + shift_y)
    int32_t xl = SX(SCR_CX - QUAD_HW), xr = SX(SCR_CX + QUAD_HW);
    int32_t yt = SY(SCR_CY - QUAD_HH), yb = SY(SCR_CY + QUAD_HH);
#undef SX
#undef SY

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, (uint8_t)intensity, (uint8_t)intensity, (uint8_t)intensity);
    poly->x0 = (int16_t)xl; poly->y0 = (int16_t)yt;
    poly->x1 = (int16_t)xr; poly->y1 = (int16_t)yt;
    poly->x2 = (int16_t)xl; poly->y2 = (int16_t)yb;
    poly->x3 = (int16_t)xr; poly->y3 = (int16_t)yb;
    poly->u0 = UPS_U0; poly->v0 = UPS_V0;
    poly->u1 = UPS_U1; poly->v1 = UPS_V0;
    poly->u2 = UPS_U0; poly->v2 = UPS_V1;
    poly->u3 = UPS_U1; poly->v3 = UPS_V1;
    poly->tpage = ups_tpage;
    poly->clut  = ups_clut;
    addPrim(&ctx->buffers[ctx->active_buffer].ot[200], poly);
    ctx->next_packet += sizeof(POLY_FT4);
}
