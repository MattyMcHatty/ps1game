#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include "door_anim.h"
#include "sound.h"
#include "world.h"   /* world_silence_monsters */

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

/* DOOR_PANEL_GATE runs on its own clock. Its sound is not SFX_DOOR but SFX_GATE
 * — 2.90 s of iron hinge, where DROPEN is 5.07 s — and the brief says the leaf
 * should open over the course of it. So the swing STARTS on the frame the sound
 * does (rather than after the usual half-second hold) and lasts exactly as long
 * as the clip, and everything after it slides out to match. If SFX_GATE is ever
 * retrimmed, GATE_SWING_FRAMES is the number to change: 60 * seconds. */
#define GATE_SWING_START     FADE_IN_FRAMES              /* 60: the sfx frame   */
#define GATE_SWING_FRAMES    174                         /* 2.90 s @ 60fps      */
#define GATE_SWING_END       (GATE_SWING_START + GATE_SWING_FRAMES)
#define GATE_FADE_START      (GATE_SWING_END + HOLD_OPEN_FRAMES)
#define GATE_TOTAL_FRAMES    (GATE_FADE_START + FADE_FRAMES)

/* Slow camera dolly toward the door over the last ZOOM_FRAMES: the door image
 * scales from 1.0x up to ZOOM_MAX/256 about screen centre. */
#define ZOOM_FRAMES         120   /* final 2.0s */
#define ZOOM_MAX            358   /* ~1.4x (256 = 1.0x) */

/* ------------------------------------------------------ on-screen geometry */
/* 320x240 screen. The double door is centred; each leaf is PANEL_W wide. The
 * left leaf is static, the right leaf swings about its outer (right) hinge. */
#define DOOR_CENTER_X   160
#define DOOR_CENTER_Y   120
#define PANEL_W          80     /* width of one leaf on screen */
#define DOOR_HALF_H     100     /* half the door height on screen */
#define PERSP_D         220     /* fake-perspective focal distance (bigger = flatter) */

/* Both door TIMs are 64x128 and were placed so that within their page the texels
 * span U[0,63] V[128,255] (dbl_dr_hlf at VRAM x=512 y=128, inr_dbl_dr_half at
 * x=576 y=128). U=0 is the inner edge (door handle / centre seam). Because the
 * in-page texel offsets match, the same UV constants serve every variant. */
#define DOOR_U_INNER    0
#define DOOR_U_OUTER   63
#define DOOR_V_TOP    128
#define DOOR_V_BOT    255

/* The wood variant (DOOR_PANEL_WOOD) is a SINGLE door: the full 128x128
 * wd_dr.tim (16bpp, no CLUT, Voff 0) on one leaf that swings about its right
 * hinge — same timing, sound, zoom and fades as the double doors. */
#define WOOD_U_FREE     0
#define WOOD_U_HINGE  127
#define WOOD_V_TOP      0
#define WOOD_V_BOT    127

/* DOOR_PANEL_EXIT is the odd one out. The Attic Exit's unlocked door onto the
 * Garden Stairs is TWO separate 64x128 TIMs — xt_dr_lft_hlf and xt_dr_rt_hlf —
 * rather than one leaf image drawn twice mirrored, and BOTH leaves swing away
 * from the camera about their outer hinges. Each is page-aligned at Voff 128
 * (x832 and x896, y128), so U spans 0..63 and V 128..255 exactly like the other
 * double doors; only the U ENDS differ per leaf, because the two images are not
 * mirrors of each other:
 *   left  leaf: U 0 = outer edge,  U 63 = centre seam
 *   right leaf: U 0 = centre seam, U 63 = outer edge  */
#define EXIT_L_U_OUTER   0
#define EXIT_L_U_SEAM   63
#define EXIT_R_U_SEAM    0
#define EXIT_R_U_OUTER  63

/* The two leaves are drawn closer together than their own width, so the closed
 * door has no hairline of background showing down the centre seam. EACH leaf
 * crosses the centre line by 10% of its own width, making the overlapping strip
 * 20% of a leaf. Each is translated inward by that amount, hinge and all, so the
 * swing still pivots about its own outer edge and neither image is stretched. */
#define EXIT_INSET       (PANEL_W / 10)

/* DOOR_PANEL_GATE, the garden gate. Two 64x128 TIMs like the exit door, but
 * they could NOT be page-aligned — there is no free 64-word column left at a
 * page boundary — so each sits in the RIGHT half of a page it shares with
 * something else (grdngtl at VRAM x544 on the dbl_dr_hlf page, grdngtr at x608
 * on the inr_dbl_dr_half page). Both are therefore at U 64..127 rather than
 * 0..63; V is 128..255 as usual. Left leaf reads outer-edge-first, right leaf
 * seam-first, exactly as the exit door's pair do.
 *
 * No EXIT_INSET here: the art is see-through, so overlapping the leaves would
 * double-draw the bars down the centre seam instead of hiding a hairline.
 *
 * And it is WIDER than every other variant: a garden gate is a wide, low thing
 * next to a house door, and at the shared PANEL_W it read as a narrow slot. 96
 * is PANEL_W + 20%, giving a 192-wide gate on a 320-wide screen. Height is left
 * at DOOR_HALF_H — widening only is the point. The final dolly takes it to
 * 96 * ZOOM_MAX/256 = 134 either side of centre, which still lands inside the
 * screen; anything much past this would start clipping the hinge off-screen. */
#define GATE_PANEL_W    ((PANEL_W * 12) / 10)   /* 96 */
#define GATE_L_U_OUTER   64
#define GATE_L_U_SEAM   127
#define GATE_R_U_SEAM    64
#define GATE_R_U_OUTER  127

#define DOOR_PANEL_COUNT  5

static int32_t  anim_timer  = 0;
static int      anim_active = 0;
static int      anim_variant = DOOR_PANEL_OUTER;   /* which door texture to draw */

/* One tpage/clut pair per variant (indexed by DOOR_PANEL_*). For the single- and
 * mirrored-double-door variants this is the whole story — one image serves both
 * leaves. */
static uint16_t panel_tpage[DOOR_PANEL_COUNT];
static uint16_t panel_clut [DOOR_PANEL_COUNT];
/* The RIGHT leaf of the two variants whose leaves are different images rather
 * than one image drawn twice mirrored (DOOR_PANEL_EXIT, DOOR_PANEL_GATE). Their
 * left leaf uses the panel_* entry above; the others leave this unset. */
static uint16_t panel_r_tpage[DOOR_PANEL_COUNT];
static uint16_t panel_r_clut [DOOR_PANEL_COUNT];
static int      tex_loaded  = 0;

/* ------------------------------------------------------------ asset loading */
/* Mirrors fatdoors_load_assets: a CD read + LoadImage, done ONCE at startup.
 * LoadImage is only safe before the per-frame draw loop begins (see
 * tools/TEXTURING_NOTES.txt) — never call this mid-game. */
/* Load one door-panel TIM into VRAM and record its tpage/clut through the given
 * pointers. Returns 1 on success. */
static int load_panel_to(const char *path, uint16_t *tpage_out, uint16_t *clut_out) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)path)) return 0;

    int   sectors = (file.size + 2047) / 2048;
    void *buf     = malloc(sectors * 2048);
    if (!buf) return 0;

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

    free(buf);
    return 1;
}

/* Load into a variant's own slot — what every variant's single/left leaf wants. */
static int load_panel(const char *path, int variant) {
    return load_panel_to(path, &panel_tpage[variant], &panel_clut[variant]);
}

/* ...and into the right-leaf slot, for the two variants that have one. */
static int load_panel_r(const char *path, int variant) {
    return load_panel_to(path, &panel_r_tpage[variant], &panel_r_clut[variant]);
}

void door_anim_load_assets(void) {
    int ok = load_panel("\\DBLDRHLF.TIM;1", DOOR_PANEL_OUTER);
    load_panel("\\INRDRHLF.TIM;1", DOOR_PANEL_INNER);
    /* wd_dr is also loaded by fatdoors_load_assets (same VRAM rect, 768,256);
       re-reading it here just records its tpage the same way as the others. */
    load_panel("\\WDDR.TIM;1", DOOR_PANEL_WOOD);
    /* The exit door's two leaves: the left in the variant's own slot, the right
       in its own pair — this is the one variant whose leaves are different
       images rather than one image drawn twice. */
    load_panel("\\TEX\\XTDRLHLF.TIM;1", DOOR_PANEL_EXIT);
    load_panel_r("\\TEX\\XTDRRHLF.TIM;1", DOOR_PANEL_EXIT);
    /* The garden gate's two leaves, same arrangement. */
    load_panel("\\TEX\\GRDNGTL.TIM;1", DOOR_PANEL_GATE);
    load_panel_r("\\TEX\\GRDNGTR.TIM;1", DOOR_PANEL_GATE);
    /* The outer door is the default/fallback; require at least it to draw. */
    if (ok) tex_loaded = 1;
}

/* ------------------------------------------------------------- state machine */
void door_anim_start(int variant) {
    anim_timer   = 0;
    anim_active  = 1;
    anim_variant = (variant >= 0 && variant < DOOR_PANEL_COUNT) ? variant
                                                                : DOOR_PANEL_OUTER;
    /* Kill every monster sound the moment the transition begins: from here the
       area update stops running, so a looped scuttle/writhe would otherwise
       follow the player into the next room. */
    world_silence_monsters();
    /* The door sound starts when the fade-in finishes (see door_anim_update),
       so it lines up with the door becoming fully visible, not with the black. */
}

/* The clock. Every variant but the gate runs on the shared timing at the top of
 * the file; the gate's swing is cut to the length of its own sound and pushes
 * the fade and the total out with it. Kept as four accessors rather than four
 * `if`s scattered through the file so there is one place the two clocks are
 * stated, and so a third variant with its own timing is a one-line change. */
static int is_gate(void)             { return anim_variant == DOOR_PANEL_GATE; }
static int32_t swing_start(void)     { return is_gate() ? GATE_SWING_START  : SWING_START;  }
static int32_t swing_frames(void)    { return is_gate() ? GATE_SWING_FRAMES : SWING_FRAMES; }
static int32_t fade_start(void)      { return is_gate() ? GATE_FADE_START   : FADE_START;   }
static int32_t total_frames(void)    { return is_gate() ? GATE_TOTAL_FRAMES : TOTAL_FRAMES; }

void door_anim_update(void) {
    if (!anim_active) return;
    anim_timer++;
    /* The gate has a sound of its own — see DOOR_PANEL_GATE in door_anim.h. */
    if (anim_timer == FADE_IN_FRAMES)
        sound_play(is_gate() ? SFX_GATE : SFX_DOOR);
}

int door_anim_finished(void) {
    if (anim_active && anim_timer >= total_frames()) {
        anim_active = 0;
        return 1;
    }
    return 0;
}

int door_anim_active(void) {
    return anim_active;
}

/* Swinging-leaf angle in GTE units (0 = closed, 1024 = 90deg open), with an
 * ease-out curve so the door flings open then settles. */
static int32_t swing_angle(void) {
    int32_t start = swing_start(), frames = swing_frames();
    if (anim_timer <= start)          return 0;
    if (anim_timer >= start + frames) return 1024;
    int32_t t   = (anim_timer - start) * 256 / frames;   /* 0..256 */
    int32_t inv = 256 - t;
    int32_t eased = 256 - (inv * inv / 256);   /* ease-out, 0..256 */
    return eased * 1024 / 256;                 /* 0..1024 */
}

/* Camera dolly: door scale about screen centre, 256 = 1.0x. Ramps from 1.0x up
 * to ZOOM_MAX over the final ZOOM_FRAMES — measured back from whichever total
 * this variant runs to, so the dolly always lands on the fade-out. */
static int32_t zoom_factor(void) {
    int32_t zoom_start = total_frames() - ZOOM_FRAMES;
    if (anim_timer <= zoom_start) return 256;
    int32_t zt = anim_timer - zoom_start;
    if (zt > ZOOM_FRAMES) zt = ZOOM_FRAMES;
    return 256 + (ZOOM_MAX - 256) * zt / ZOOM_FRAMES;
}

/* ----------------------------------------------------------------- rendering */
/* Emit one textured quad (TL, TR, BL, BR order) at OT index 200. All corners are
 * scaled by `zoom` (256 = 1.0x) about screen centre for the camera dolly.
 * `tpage`/`clut` are passed rather than read from anim_variant because the exit
 * door's two leaves come from different TIMs. */
static void emit_panel_tex(RenderContext *ctx, uint8_t *buf_end,
                       uint16_t tpage, uint16_t clut,
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
    poly->tpage = tpage;
    poly->clut  = clut;
    addPrim(&ctx->buffers[ctx->active_buffer].ot[200], poly);
    ctx->next_packet += sizeof(POLY_FT4);
}

/* The common case: draw with the current variant's own texture. */
#define emit_panel(ctx, buf_end, ...) \
    emit_panel_tex((ctx), (buf_end), panel_tpage[anim_variant], \
                   panel_clut[anim_variant], __VA_ARGS__)

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
    } else if (anim_timer > fade_start()) {
        int32_t fade = (anim_timer - fade_start()) * 256 / FADE_FRAMES;
        if (fade > 256) fade = 256;
        intensity = 128 * (256 - fade) / 256;            /* 128 -> 0 */
    } else {
        intensity = 128;
    }

    int32_t zoom = zoom_factor();
    int32_t top = DOOR_CENTER_Y - DOOR_HALF_H;
    int32_t bot = DOOR_CENTER_Y + DOOR_HALF_H;

    /* Single wooden door: one quad, centred, hinged on its right edge. Same
     * swing math as the double door's right leaf, but the leaf is the whole
     * door and the hinge sits at centre + W/2 so the door stays centred. */
    if (anim_variant == DOOR_PANEL_WOOD) {
        int32_t swing  = swing_angle();
        int32_t cos_t  = icos(swing);
        int32_t sin_t  = isin(swing);
        int32_t z      = PANEL_W * sin_t / 4096;
        int32_t persp  = PERSP_D * 256 / (PERSP_D + z);
        int32_t hinge_x = DOOR_CENTER_X + PANEL_W / 2;
        int32_t xoff    = (-PANEL_W * cos_t / 4096) * persp / 256;
        int32_t free_x  = hinge_x + xoff;
        int32_t free_hh = DOOR_HALF_H * persp / 256;

        emit_panel(ctx, buf_end,
                   free_x,  DOOR_CENTER_Y - free_hh,  hinge_x, top,
                   free_x,  DOOR_CENTER_Y + free_hh,  hinge_x, bot,
                   WOOD_U_FREE, WOOD_V_TOP,  WOOD_U_HINGE, WOOD_V_TOP,
                   WOOD_U_FREE, WOOD_V_BOT,  WOOD_U_HINGE, WOOD_V_BOT,
                   intensity, zoom);
        return;
    }

    /* Exit door: BOTH leaves swing away from the camera, each about its own
     * outer hinge, so the doorway opens from the centre seam outwards. Same
     * swing curve, perspective and foreshortening as the single-leaf cases —
     * the left leaf is simply the right leaf's mirror, with its hinge at
     * center-W and its free edge travelling to the RIGHT as it opens. */
    if (anim_variant == DOOR_PANEL_EXIT) {
        int32_t swing  = swing_angle();
        int32_t cos_t  = icos(swing);
        int32_t sin_t  = isin(swing);
        int32_t z      = PANEL_W * sin_t / 4096;
        int32_t persp  = PERSP_D * 256 / (PERSP_D + z);
        int32_t free_hh = DOOR_HALF_H * persp / 256;
        int32_t xoff    = (PANEL_W * cos_t / 4096) * persp / 256;

        /* Left leaf: hinge on the left, free (seam) edge swinging right. */
        {
            int32_t hinge_x = DOOR_CENTER_X - PANEL_W + EXIT_INSET;
            int32_t free_x  = hinge_x + xoff;
            emit_panel(ctx, buf_end,
                       hinge_x, top,  free_x, DOOR_CENTER_Y - free_hh,
                       hinge_x, bot,  free_x, DOOR_CENTER_Y + free_hh,
                       EXIT_L_U_OUTER, DOOR_V_TOP,  EXIT_L_U_SEAM, DOOR_V_TOP,
                       EXIT_L_U_OUTER, DOOR_V_BOT,  EXIT_L_U_SEAM, DOOR_V_BOT,
                       intensity, zoom);
        }

        /* Right leaf: hinge on the right, free (seam) edge swinging left. Its
           own texture, hence emit_panel_tex rather than the macro. */
        {
            int32_t hinge_x = DOOR_CENTER_X + PANEL_W - EXIT_INSET;
            int32_t free_x  = hinge_x - xoff;
            emit_panel_tex(ctx, buf_end,
                       panel_r_tpage[anim_variant], panel_r_clut[anim_variant],
                       free_x, DOOR_CENTER_Y - free_hh,  hinge_x, top,
                       free_x, DOOR_CENTER_Y + free_hh,  hinge_x, bot,
                       EXIT_R_U_SEAM, DOOR_V_TOP,  EXIT_R_U_OUTER, DOOR_V_TOP,
                       EXIT_R_U_SEAM, DOOR_V_BOT,  EXIT_R_U_OUTER, DOOR_V_BOT,
                       intensity, zoom);
        }
        return;
    }

    /* Garden gate: the LEFT leaf alone swings, about its own outer (left) hinge
     * and away from the camera, exactly as the exit door's left leaf does; the
     * right leaf is drawn flat and shut for the whole transition. Both leaves
     * meet edge to edge at screen centre — no inset, because the art is
     * see-through and an overlap would double the bars down the seam.
     *
     * The swing itself is slower than every other variant's: swing_angle()
     * spreads it over GATE_SWING_FRAMES, the length of SFX_GATE. */
    if (anim_variant == DOOR_PANEL_GATE) {
        int32_t swing  = swing_angle();
        int32_t cos_t  = icos(swing);
        int32_t sin_t  = isin(swing);
        int32_t z      = GATE_PANEL_W * sin_t / 4096;
        int32_t persp  = PERSP_D * 256 / (PERSP_D + z);
        int32_t free_hh = DOOR_HALF_H * persp / 256;
        int32_t xoff    = (GATE_PANEL_W * cos_t / 4096) * persp / 256;

        /* Right leaf: shut. Drawn first so the swinging leaf overlaps it rather
           than the other way about, which matters for the frame or two either
           side of fully closed. */
        {
            int32_t xl = DOOR_CENTER_X;                  /* seam (inner) edge */
            int32_t xr = DOOR_CENTER_X + GATE_PANEL_W;   /* outer edge        */
            emit_panel_tex(ctx, buf_end,
                       panel_r_tpage[anim_variant], panel_r_clut[anim_variant],
                       xl, top,  xr, top,
                       xl, bot,  xr, bot,
                       GATE_R_U_SEAM, DOOR_V_TOP,  GATE_R_U_OUTER, DOOR_V_TOP,
                       GATE_R_U_SEAM, DOOR_V_BOT,  GATE_R_U_OUTER, DOOR_V_BOT,
                       intensity, zoom);
        }

        /* Left leaf: hinge on the left, free (seam) edge swinging right and
           into the screen. */
        {
            int32_t hinge_x = DOOR_CENTER_X - GATE_PANEL_W;
            int32_t free_x  = hinge_x + xoff;
            emit_panel(ctx, buf_end,
                       hinge_x, top,  free_x, DOOR_CENTER_Y - free_hh,
                       hinge_x, bot,  free_x, DOOR_CENTER_Y + free_hh,
                       GATE_L_U_OUTER, DOOR_V_TOP,  GATE_L_U_SEAM, DOOR_V_TOP,
                       GATE_L_U_OUTER, DOOR_V_BOT,  GATE_L_U_SEAM, DOOR_V_BOT,
                       intensity, zoom);
        }
        return;
    }

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
