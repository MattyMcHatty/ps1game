#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include "door_anim.h"
#include "sound.h"

/* ------------------------------------------------------------------ timing */
/* All in frames @ 60fps. The door fades in from black, holds closed briefly,
 * swings open, holds open, then the whole screen fades to black before the level
 * loads. Total = 300 frames = 5.0s, matching the door sound (which starts at the
 * fade-in). Over the final ZOOM_FRAMES the door scales up, as if the camera is
 * dollying toward it / through the opening. */
#define FADE_IN_FRAMES       60   /* 1.0s door fades up from black (closed)  */
#define HOLD_CLOSED_FRAMES   30   /* 0.5s closed while the sound builds       */
#define SWING_FRAMES        120   /* 2.0s to swing the leaf fully open        */
#define HOLD_OPEN_FRAMES     30   /* 0.5s held wide open                      */
#define FADE_FRAMES          60   /* 1.0s fade to black                       */
#define SWING_START          (FADE_IN_FRAMES + HOLD_CLOSED_FRAMES)
#define SWING_END            (SWING_START + SWING_FRAMES)
#define FADE_START           (SWING_END + HOLD_OPEN_FRAMES)
#define TOTAL_FRAMES         (FADE_START + FADE_FRAMES)

/* Slow camera dolly toward the door over the last ZOOM_FRAMES: the door image
 * scales from 1.0x up to ZOOM_MAX/256 about screen centre. */
#define ZOOM_FRAMES         120   /* final 2.0s */
#define ZOOM_START          (TOTAL_FRAMES - ZOOM_FRAMES)
#define ZOOM_MAX            358   /* ~1.4x (256 = 1.0x) */

/* ------------------------------------------------------ on-screen geometry */
/* 320x240 screen. The double door is centred; each leaf is PANEL_W wide. The
 * left leaf is static, the right leaf swings about its outer (right) hinge. */
#define DOOR_CENTER_X   160
#define DOOR_CENTER_Y   120
#define PANEL_W          80     /* width of one leaf on screen */
#define DOOR_HALF_H     100     /* half the door height on screen */
#define PERSP_D         220     /* fake-perspective focal distance (bigger = flatter) */

/* dbl_dr_hlf.tim was placed at VRAM x=512 y=128, so within its page the texels
 * span U[0,63] V[128,255]. U=0 is the inner edge (door handle / centre seam). */
#define DOOR_U_INNER    0
#define DOOR_U_OUTER   63
#define DOOR_V_TOP    128
#define DOOR_V_BOT    255

static int32_t  anim_timer  = 0;
static int      anim_active = 0;

static uint16_t panel_tpage = 0;
static uint16_t panel_clut  = 0;
static int      tex_loaded  = 0;

/* ------------------------------------------------------------ asset loading */
/* Mirrors fatdoors_load_assets: a CD read + LoadImage, done ONCE at startup.
 * LoadImage is only safe before the per-frame draw loop begins (see
 * tools/TEXTURING_NOTES.txt) — never call this mid-game. */
void door_anim_load_assets(void) {
    CdlFILE file;
    if (!CdSearchFile(&file, "\\DBLDRHLF.TIM;1")) return;

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
        panel_clut = getClut(tim.crect->x, tim.crect->y);
    }
    panel_tpage = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);

    free(buf);
    tex_loaded = 1;
}

/* ------------------------------------------------------------- state machine */
void door_anim_start(void) {
    anim_timer  = 0;
    anim_active = 1;
    /* The door sound starts when the fade-in finishes (see door_anim_update),
       so it lines up with the door becoming fully visible, not with the black. */
}

void door_anim_update(void) {
    if (!anim_active) return;
    anim_timer++;
    if (anim_timer == FADE_IN_FRAMES) sound_play(SFX_DOOR);
}

int door_anim_finished(void) {
    if (anim_active && anim_timer >= TOTAL_FRAMES) {
        anim_active = 0;
        return 1;
    }
    return 0;
}

/* Right-leaf swing angle in GTE units (0 = closed, 1024 = 90deg open), with an
 * ease-out curve so the door flings open then settles. */
static int32_t swing_angle(void) {
    if (anim_timer <= SWING_START) return 0;
    if (anim_timer >= SWING_END)   return 1024;
    int32_t t   = (anim_timer - SWING_START) * 256 / SWING_FRAMES; /* 0..256 */
    int32_t inv = 256 - t;
    int32_t eased = 256 - (inv * inv / 256);   /* ease-out, 0..256 */
    return eased * 1024 / 256;                 /* 0..1024 */
}

/* Camera dolly: door scale about screen centre, 256 = 1.0x. Ramps from 1.0x up
 * to ZOOM_MAX over the final ZOOM_FRAMES. */
static int32_t zoom_factor(void) {
    if (anim_timer <= ZOOM_START) return 256;
    int32_t zt = anim_timer - ZOOM_START;
    if (zt > ZOOM_FRAMES) zt = ZOOM_FRAMES;
    return 256 + (ZOOM_MAX - 256) * zt / ZOOM_FRAMES;
}

/* ----------------------------------------------------------------- rendering */
/* Emit one textured quad (TL, TR, BL, BR order) at OT index 200. All corners are
 * scaled by `zoom` (256 = 1.0x) about screen centre for the camera dolly. */
static void emit_panel(RenderContext *ctx, uint8_t *buf_end,
                       int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                       int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                       int u0, int v0, int u1, int v1,
                       int u2, int v2, int u3, int v3,
                       int32_t intensity, int32_t zoom) {
    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;

#define ZX(x) (DOOR_CENTER_X + ((x) - DOOR_CENTER_X) * zoom / 256)
#define ZY(y) (DOOR_CENTER_Y + ((y) - DOOR_CENTER_Y) * zoom / 256)
    x0 = ZX(x0); y0 = ZY(y0);  x1 = ZX(x1); y1 = ZY(y1);
    x2 = ZX(x2); y2 = ZY(y2);  x3 = ZX(x3); y3 = ZY(y3);
#undef ZX
#undef ZY

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, (uint8_t)intensity, (uint8_t)intensity, (uint8_t)intensity);
    poly->x0 = (int16_t)x0; poly->y0 = (int16_t)y0;
    poly->x1 = (int16_t)x1; poly->y1 = (int16_t)y1;
    poly->x2 = (int16_t)x2; poly->y2 = (int16_t)y2;
    poly->x3 = (int16_t)x3; poly->y3 = (int16_t)y3;
    poly->u0 = (uint8_t)u0; poly->v0 = (uint8_t)v0;
    poly->u1 = (uint8_t)u1; poly->v1 = (uint8_t)v1;
    poly->u2 = (uint8_t)u2; poly->v2 = (uint8_t)v2;
    poly->u3 = (uint8_t)u3; poly->v3 = (uint8_t)v3;
    poly->tpage = panel_tpage;
    poly->clut  = panel_clut;
    addPrim(&ctx->buffers[ctx->active_buffer].ot[200], poly);
    ctx->next_packet += sizeof(POLY_FT4);
}

void door_anim_draw(RenderContext *ctx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    /* Full-screen black background (covers the room/sky clear colour). */
    if (ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *bg = (TILE *)ctx->next_packet;
        setTile(bg);
        setXY0(bg, 0, 0);
        setWH(bg, SCREEN_XRES, SCREEN_YRES);
        setRGB0(bg, 0, 0, 0);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
        ctx->next_packet += sizeof(TILE);
    }

    if (!tex_loaded) return;

    /* Reset the GPU texture window to the full page. The kitchen renderer leaves
     * a 128x128 window active, which would otherwise wrap our V coords (128..255)
     * back to 0..127 and draw the wrong texels. RECT {0,0,0,0} = no wrapping. */
    if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
        RECT     tw   = { 0, 0, 0, 0 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[300], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    /* Door brightness: 0 -> 128 over the fade-in, full through the middle, then
     * 128 -> 0 over the fade-out. With the black background, a fully-dark door
     * means a fully-black screen. */
    int32_t intensity;
    if (anim_timer < FADE_IN_FRAMES) {
        intensity = 128 * anim_timer / FADE_IN_FRAMES;   /* 0 -> 128 */
    } else if (anim_timer > FADE_START) {
        int32_t fade = (anim_timer - FADE_START) * 256 / FADE_FRAMES;
        if (fade > 256) fade = 256;
        intensity = 128 * (256 - fade) / 256;            /* 128 -> 0 */
    } else {
        intensity = 128;
    }

    int32_t zoom = zoom_factor();
    int32_t top = DOOR_CENTER_Y - DOOR_HALF_H;
    int32_t bot = DOOR_CENTER_Y + DOOR_HALF_H;

    /* Left leaf: static, closed. Spans x[center-W, center]; mirrored so the
     * handle (U inner) sits at the centre seam (x = center). */
    {
        int32_t xl = DOOR_CENTER_X - PANEL_W;  /* outer edge */
        int32_t xr = DOOR_CENTER_X;            /* seam (inner) edge */
        emit_panel(ctx, buf_end,
                   xl, top,  xr, top,
                   xl, bot,  xr, bot,
                   DOOR_U_OUTER, DOOR_V_TOP,  DOOR_U_INNER, DOOR_V_TOP,
                   DOOR_U_OUTER, DOOR_V_BOT,  DOOR_U_INNER, DOOR_V_BOT,
                   intensity, zoom);
    }

    /* Right leaf: swings open about its outer hinge at x = center+W. The free
     * (inner/seam) edge rotates away into the screen — its X collapses toward
     * the hinge and it foreshortens vertically, revealing the black doorway. */
    {
        int32_t swing  = swing_angle();
        int32_t cos_t  = icos(swing);                  /* -4096..4096 */
        int32_t sin_t  = isin(swing);
        int32_t z      = PANEL_W * sin_t / 4096;       /* free-edge depth */
        int32_t persp  = PERSP_D * 256 / (PERSP_D + z);/* 0..256 */
        int32_t hinge_x = DOOR_CENTER_X + PANEL_W;
        int32_t xoff    = (-PANEL_W * cos_t / 4096) * persp / 256;
        int32_t free_x  = hinge_x + xoff;
        int32_t free_hh = DOOR_HALF_H * persp / 256;

        emit_panel(ctx, buf_end,
                   free_x,  DOOR_CENTER_Y - free_hh,  hinge_x, top,
                   free_x,  DOOR_CENTER_Y + free_hh,  hinge_x, bot,
                   DOOR_U_INNER, DOOR_V_TOP,  DOOR_U_OUTER, DOOR_V_TOP,
                   DOOR_U_INNER, DOOR_V_BOT,  DOOR_U_OUTER, DOOR_V_BOT,
                   intensity, zoom);
    }
}
