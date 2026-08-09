#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxpad.h>
#include "render.h"
#include "title.h"      /* TITLE_BG_* — the purple this fades to */
#include "cdaudio.h"
#include "intro.h"      /* the 5x7 fade-able font */
#include "trial_end.h"

/* The trial-end screen. See trial_end.h for what it is and when it arms. */

/* ------------------------------------------------------------------- timing */
#define TE_T_FADE        240   /* 4.0 s, as briefed: garden -> purple          */
#define TE_LINE_FADE      45   /* 0.75 s for one line to reach full brightness */
#define TE_HOLD_AFTER     45   /* everything is up this long before Start arms */

/* ------------------------------------------------------------------- layout */
#define TE_LINE_SPACING   14
#define TE_ROWS            9   /* seven lines and the two blank rows between   */
#define TE_TOP     (120 - TE_ROWS * TE_LINE_SPACING / 2)

/* OT buckets. Lower draws later, so the text sits over the wash. The HUD and
   the menus own 0..15 and both are suppressed while this screen runs, so
   nothing contends for either. */
#define TE_OT_TEXT         0
#define TE_OT_WASH         1

/* ---- The sign-off -----------------------------------------------------------
   Blank rows are NULL: they cost a row of vertical space and no time, which is
   what puts air around THANK YOU without stalling the fade-on for two beats.

   The start times are stated rather than derived from a stagger, because the
   last two lines deliberately do not keep the others' rhythm — the thank-you and
   the prompt each want a moment of their own.

   >>> CHECK ANY REWORDED LINE AGAINST intro_text_width. <<< The screen is 320px
   and the longest line here ("TO SEE MORE PROGRESS...") is 283 at the font's own
   advances, so there is about 18px of slack a side. */
typedef struct {
    const char *text;    /* NULL = a blank row */
    int16_t     start;   /* frames after the fade ends */
} TeLine;

static const TeLine SCRIPT[TE_ROWS] = {
    { "CONGRATULATIONS ON COMPLETING THIS TRIAL VERSION",    0 },
    { "OF ORDER OF NINURTA",                                75 },
    { "FOLLOW ALONG ON TIKTOK OR YOUTUBE",                 150 },
    { "@ELECTRICRELOAD",                                   225 },
    { "TO SEE MORE PROGRESS ON THE GAME AND WHAT'S TO COME",300 },
    { 0,                                                     0 },
    { "THANK YOU!",                                        405 },
    { 0,                                                     0 },
    { "PRESS START TO RETURN",                             510 },
};

#define TE_T_TEXT_END (510 + TE_LINE_FADE + TE_HOLD_AFTER)

/* -------------------------------------------------------------------- state */
static int      active   = 0;
static int      done     = 0;   /* latched for one trial_end_finished() call */
static int32_t  t        = 0;
static uint16_t prev_held = 0xFFFF;

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

void trial_end_reset(void) {
    active = 0;
    done   = 0;
    t      = 0;
}

void trial_end_start(void) {
    active    = 1;
    done      = 0;
    t         = 0;
    prev_held = 0xFFFF;   /* whatever is held on the first frame is not an edge */
    /* THE MUSIC IS THE PIANO ROOM'S POST-ANZU TRACK, as briefed. It comes in on
       the first frame of the fade rather than when the text arrives: the garden
       has been silent since the killing blow (rabisu_boss.c stops the courtyard
       track at begin_death), and four seconds of silence under a fade reads as
       the game having crashed. cdaudio_play re-asserts full volume, so nothing
       here has to undo the drive-mixer fades the opening sequence leaves. */
    cdaudio_play(CDAUDIO_ANZU_TRACK, 1);
}

int trial_end_active(void)        { return active; }
int trial_end_world_visible(void) { return active && t < TE_T_FADE; }

void trial_end_update(void) {
    if (!active) return;
    t++;

    /* Start is dead until the whole screen is up. There is no way back from
       here, so an accidental press — the player still had the pad in hand for
       the boss fight — must not be able to throw the sign-off away. */
    if (t >= TE_T_FADE + TE_T_TEXT_END && pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        uint16_t held    = ~pad->btn;
        uint16_t pressed = held & ~prev_held;
        prev_held        = held;
        if (pressed & PAD_START) {
            active = 0;
            done   = 1;
        }
    }
}

int trial_end_finished(void) {
    if (done) { done = 0; return 1; }
    return 0;
}

/* ---- The dissolve -----------------------------------------------------------
   One counter, two full-screen tiles: grey subtracted off the scene (ABR=2,
   B - F) to take it to true black, and the title's purple added back (ABR=1,
   B + F) to bring the colour up. At k = 255 the first has flattened any picture
   at all to zero and the second has written exactly TITLE_BG_*.

   BOTH DR_TPAGEs ARE ADDED AFTER THE TILE THEY APPLY TO. An OT node is LIFO, so
   the GPU has to see the blend mode first — the same ordering, and the same
   reason, as delivery_intro.c's fade and graveolver.c's muzzle flash. The four
   primitives go in in reverse of the order they must be drawn. */
static void wash(RenderContext *ctx, int32_t k) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot     = &ctx->buffers[ctx->active_buffer].ot[TE_OT_WASH];

    if (ctx->next_packet + 2 * (sizeof(TILE) + sizeof(DR_TPAGE)) > buf_end) return;

    /* Drawn LAST: the purple, added on top of the blacked-out scene. */
    {
        TILE *tl = (TILE *)ctx->next_packet;
        setTile(tl);
        setSemiTrans(tl, 1);
        setRGB0(tl, (uint8_t)(TITLE_BG_R * k / 255),
                    (uint8_t)(TITLE_BG_G * k / 255),
                    (uint8_t)(TITLE_BG_B * k / 255));
        setXY0(tl, 0, 0);
        setWH(tl, SCREEN_XRES, SCREEN_YRES);
        addPrim(ot, tl);
        ctx->next_packet += sizeof(TILE);
    }
    {
        DR_TPAGE *dp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(dp, 0, 0, getTPage(0, 1 /* ABR=1: B + F */, 0, 0));
        addPrim(ot, dp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
    /* Drawn FIRST: the scene, subtracted away. */
    {
        TILE *tl = (TILE *)ctx->next_packet;
        setTile(tl);
        setSemiTrans(tl, 1);
        setRGB0(tl, (uint8_t)k, (uint8_t)k, (uint8_t)k);
        setXY0(tl, 0, 0);
        setWH(tl, SCREEN_XRES, SCREEN_YRES);
        addPrim(ot, tl);
        ctx->next_packet += sizeof(TILE);
    }
    {
        DR_TPAGE *dp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(dp, 0, 0, getTPage(0, 2 /* ABR=2: B - F */, 0, 0));
        addPrim(ot, dp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
}

void trial_end_draw(RenderContext *ctx) {
    if (!active) return;

    if (t < TE_T_FADE) {
        /* The room is still being drawn behind this (main.c asks
           trial_end_world_visible), so the fade is the wash and nothing else. */
        wash(ctx, (t * 255) / TE_T_FADE);
        return;
    }

    /* Past the fade the picture is gone and the purple is the framebuffer CLEAR
       colour instead of a pair of blended tiles — the same trick the title
       screen and the opening sequence use, and the reason the dissolve had to
       land on exactly TITLE_BG_*. Written into BOTH draw_envs because either may
       be the next one drawn into. */
    setRGB0(&ctx->buffers[0].draw_env, TITLE_BG_R, TITLE_BG_G, TITLE_BG_B);
    setRGB0(&ctx->buffers[1].draw_env, TITLE_BG_R, TITLE_BG_G, TITLE_BG_B);

    {
        int32_t bt = t - TE_T_FADE;
        int i;
        for (i = 0; i < TE_ROWS; i++) {
            if (!SCRIPT[i].text) continue;
            int32_t v = (bt - SCRIPT[i].start) * 255 / TE_LINE_FADE;
            if (v <= 0)  continue;
            if (v > 255) v = 255;   /* up, and it stays up: nothing fades out */
            intro_text_draw(ctx, SCRIPT[i].text, TE_TOP + i * TE_LINE_SPACING,
                            (int)v, TE_OT_TEXT);
        }
    }
}
