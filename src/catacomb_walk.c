#include <stdint.h>
#include <psxgpu.h>
#include <inline_c.h>
#include "catacomb_walk.h"
#include "tim_slots.h"
#include "sound.h"
#include "world.h"   /* world_silence_monsters */

/* ------------------------------------------------------------------ timing
   All frame counts @ 60fps. Fade up, hold, six paces, fade out — and the fade
   out ENDS on the last frame rather than starting on it, so the screen is black
   at the cut and not merely dim. */
#define CW_FADE_IN_FRAMES   24    /* 0.40 s up from black                     */
#define CW_WAIT_FRAMES      42    /* 0.70 s held on the open doors            */
#define CW_INTRO_FRAMES     (CW_FADE_IN_FRAMES + CW_WAIT_FRAMES)   /* 66  */
#define CW_STEP_FRAMES      48    /* 0.80 s a pace — slow, as the brief asks   */
#define CW_STEP_COUNT        6
#define CW_STEPS_START      CW_INTRO_FRAMES                        /* 66  */
#define CW_STEPS_END        (CW_STEPS_START + CW_STEP_FRAMES * CW_STEP_COUNT)
                                                                   /* 354 */
#define CW_FADE_OUT_FRAMES  54    /* 0.90 s down, ending on the last frame    */
#define CW_TOTAL_FRAMES     (CW_STEPS_END + CW_FADE_OUT_FRAMES)    /* 408 */

/* ------------------------------------------------------ the shot's geometry
   THE LEAVES, in the fully-open pose src/catacomb_doors.h defines. Keep these
   in step with CD_SLIDE_FULL and the leaf bounds quoted there.

   z is the distance from the camera to the plane the leaves stand in. It runs
   CW_Z_FAR -> CW_Z_NEAR over the walk.

   CW_Z_FAR is chosen so both leaves are wholly on screen with the mouth open
   between them: the outer edge at |x| = 1000 projects to 1000*256/2200 = 116
   px from centre against a 160-px half-screen, and the inner edges at |x| = 500
   land 58 px either side of centre — an unmistakable gap to walk into.

   CW_Z_NEAR STOPS SHORT OF THE DOORS ON PURPOSE, at 150 rather than at 0. By
   then the inner edges project 853 px from centre and the leaves have long
   since swept off both sides of the screen, so the last stride is spent looking
   at nothing but the black they faded into — which is the shot. Running z to 0
   would divide by zero to get a picture nobody sees. */
#define CW_LEAF_OUTER     1000    /* |x| of each leaf's outer edge, open      */
#define CW_LEAF_INNER      500    /* |x| of each leaf's inner edge, open      */
#define CW_LEAF_TOP      (-910)   /* -Y is up: the lintel                     */
#define CW_LEAF_BOTTOM       0    /* the ground                               */
#define CW_EYE_Y         (-189)   /* standing eye height above that ground    */
#define CW_Z_FAR          2200
#define CW_Z_NEAR          150
#define CW_FOCAL           256    /* matches gte_SetGeomScreen(256)           */

#define CW_SCR_CX          160
#define CW_SCR_CY          120

/* One full tile of the tablet across each leaf. The real leaves in the room UV
   past 128 because the art tiles across a 500-wide slab; here each leaf gets
   one clean wrap, so the window can be the full page and no wrapping is
   involved at all. */
#define CW_U_LO   0
#define CW_U_HI 127
#define CW_V_LO   0
#define CW_V_HI 127

static int32_t walk_timer  = 0;
static int     walk_active = 0;

void catacomb_walk_start(void) {
    walk_timer  = 0;
    walk_active = 1;
    /* Same as the door and stair transitions: silence the monsters being left
       behind before the area update stops running. Nothing in the Outside
       Catacombs makes a noise after this frame. */
    world_silence_monsters();
}

int catacomb_walk_active(void) {
    return walk_active;
}

void catacomb_walk_update(void) {
    if (!walk_active) return;
    walk_timer++;
    /* A footfall at the START of each pace, alternating left/right. The first
       lands on the frame the camera begins to move, so the sound and the first
       inch of travel are the same event. Both clips are SND_RESIDENT
       (src/sound.h), so this plays whatever bank the outgoing room was on —
       which matters here more than anywhere, because the bank is about to be
       swapped out from under it. */
    for (int s = 0; s < CW_STEP_COUNT; s++)
        if (walk_timer == CW_STEPS_START + CW_STEP_FRAMES * s)
            sound_play((s & 1) ? SFX_STEP2 : SFX_STEP1);
}

int catacomb_walk_finished(void) {
    if (walk_active && walk_timer >= CW_TOTAL_FRAMES) {
        walk_active = 0;
        return 1;
    }
    return 0;
}

/* Cumulative eased travel in 1/256ths of a pace, 0 .. CW_STEP_COUNT*256. Each
   pace eases OUT so it lands and settles rather than gliding — stair_anim's
   climb256(), and it is what makes six paces read as six footsteps instead of
   one slow dolly. Frozen at the end once the paces are done and the fade has
   the screen. */
static int32_t walk256(void) {
    int32_t t = walk_timer - CW_STEPS_START;
    if (t < 0) t = 0;
    if (t > CW_STEP_FRAMES * CW_STEP_COUNT) t = CW_STEP_FRAMES * CW_STEP_COUNT;
    int32_t s     = t / CW_STEP_FRAMES;              /* completed paces */
    int32_t local = t - s * CW_STEP_FRAMES;
    int32_t lt    = local * 256 / CW_STEP_FRAMES;    /* 0..255          */
    int32_t inv   = 256 - lt;
    int32_t eased = 256 - (inv * inv / 256);         /* ease-out 0..256 */
    return s * 256 + eased;
}

/* One textured quad, corners already in screen space. Kept local rather than
   borrowed from door_anim.c, whose emit_panel_tex carries a dolly parameter
   that means nothing here. */
static void cw_leaf(RenderContext *ctx, uint8_t *buf_end,
                    int32_t xl, int32_t xr, int32_t yt, int32_t yb,
                    int32_t intensity) {
    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;
    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, (uint8_t)intensity, (uint8_t)intensity, (uint8_t)intensity);
    poly->x0 = (int16_t)xl; poly->y0 = (int16_t)yt;
    poly->x1 = (int16_t)xr; poly->y1 = (int16_t)yt;
    poly->x2 = (int16_t)xl; poly->y2 = (int16_t)yb;
    poly->x3 = (int16_t)xr; poly->y3 = (int16_t)yb;
    poly->u0 = CW_U_LO; poly->v0 = CW_V_LO;
    poly->u1 = CW_U_HI; poly->v1 = CW_V_LO;
    poly->u2 = CW_U_LO; poly->v2 = CW_V_HI;
    poly->u3 = CW_U_HI; poly->v3 = CW_V_HI;
    poly->tpage = TIM_TPAGE_LMSHTBLT;
    poly->clut  = TIM_CLUT_LMSHTBLT;
    addPrim(&ctx->buffers[ctx->active_buffer].ot[200], poly);
    ctx->next_packet += sizeof(POLY_FT4);
}

void catacomb_walk_draw(RenderContext *ctx) {
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

    /* Full-page texture window: the UVs are 0..127 and must not be masked to a
       128 window that the room this is leaving happens to have left set. */
    if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
        RECT     tw   = { 0, 0, 0, 0 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[300], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    /* Depth: FAR -> NEAR, linear in world distance across the eased paces. A
       linear walk through a perspective projection already accelerates on
       screen, which is what closing on something looks like; easing the DEPTH
       as well would read as a lunge. */
    int32_t w    = walk256();
    int32_t wmax = CW_STEP_COUNT * 256;
    int32_t z    = CW_Z_FAR + ((CW_Z_NEAR - CW_Z_FAR) * w) / wmax;
    if (z < 1) z = 1;                     /* the divide's guard */

    /* Brightness: up from black, hold at 128 (the PS1's pass-through for a
       textured poly), then down to black over the last frames. */
    int32_t intensity;
    if (walk_timer < CW_FADE_IN_FRAMES) {
        intensity = 128 * walk_timer / CW_FADE_IN_FRAMES;
    } else if (walk_timer > CW_TOTAL_FRAMES - CW_FADE_OUT_FRAMES) {
        int32_t f = (walk_timer - (CW_TOTAL_FRAMES - CW_FADE_OUT_FRAMES)) * 256
                    / CW_FADE_OUT_FRAMES;
        if (f > 256) f = 256;
        intensity = 128 * (256 - f) / 256;
    } else {
        intensity = 128;
    }
    if (intensity <= 0) return;

    /* Project. The leaves share a plane, so one z serves all eight corners —
       which is also why they stay rectangular on screen and need no per-corner
       work: the camera walks straight down the mouth's centre line and never
       turns. */
    int32_t outer = (CW_LEAF_OUTER * CW_FOCAL) / z;
    int32_t inner = (CW_LEAF_INNER * CW_FOCAL) / z;
    int32_t top   = ((CW_LEAF_TOP    - CW_EYE_Y) * CW_FOCAL) / z;
    int32_t bot   = ((CW_LEAF_BOTTOM - CW_EYE_Y) * CW_FOCAL) / z;

    /* Clamp to the GPU's drawing range. A PS1 primitive's coordinates are 11-bit
       signed and wrap past ±1024, so a leaf that has swept off the side of the
       screen would otherwise reappear on the other one — which is exactly what
       the last two paces of this shot would do. */
#define CW_CLAMP(v) ((v) < -1000 ? -1000 : ((v) > 1000 ? 1000 : (v)))
    int32_t yt = CW_CLAMP(CW_SCR_CY + top);
    int32_t yb = CW_CLAMP(CW_SCR_CY + bot);

    /* Left leaf: x from -outer to -inner. Right leaf mirrors it. */
    cw_leaf(ctx, buf_end,
            CW_CLAMP(CW_SCR_CX - outer), CW_CLAMP(CW_SCR_CX - inner),
            yt, yb, intensity);
    cw_leaf(ctx, buf_end,
            CW_CLAMP(CW_SCR_CX + inner), CW_CLAMP(CW_SCR_CX + outer),
            yt, yb, intensity);
#undef CW_CLAMP
}
