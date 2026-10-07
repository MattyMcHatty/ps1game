#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "title.h"          /* current_area */
#include "sound.h"          /* SFX_WOOSH — HOUSE|GARDEN for this file */
#include "player.h"         /* show_pickup_msg_raw */
#include "particles.h"      /* a cutscene draws its own (hadad_grinder.h) */
#include "door.h"           /* interact_spend_press */
#include "piano_room.h"     /* pdoor_arm */
#include "library.h"        /* library_doors_arm */
#include "adapa.h"
#include "adapa_death.h"

/* Adapa's death. Read adapa_death.h first: the beat sheet is there. */

/* ---- Timing, in frames at 60 fps ------------------------------------------- */
#define AD_T_TURN              45   /* 0.75 s onto him — hadad_grinder's turn   */
#define AD_T_FADE  ADP_FADE_FRAMES  /* the clip's length: he goes as it ends    */
#define AD_T_ORB               60   /* 1 s on the ball — Hadad's                */
#define AD_T_RISE             180   /* 3 s climbing out of sight — Hadad's      */
#define AD_T_BACK              60   /* 1 s home — Hadad's                       */

/* ---- The spirit -------------------------------------------------------------
   hadad_grinder.c's, number for number: constant upward acceleration, so it is
   still gaining speed as it leaves ("rises into the air gaining speed until it
   disappears"), faded over the last second so it goes out rather than being
   cut off, and faded IN as he finishes going. */
#define AD_RISE_ACCEL          48
#define AD_ORB_FADE            60
#define AD_ORB_IN              20

typedef enum {
    AD_IDLE = 0,
    AD_TURN,
    AD_FADE,
    AD_ORB,
    AD_RISE,
    AD_BACK,
    AD_DONE,
} AdState;

static AdState  state = AD_IDLE;
static int32_t  phase_t;
static int32_t  orb_x, orb_y, orb_z;   /* where his body was, then the climb  */
static int32_t  orb_y0;
static int32_t  orb_bright;            /* 0..256                              */
static int32_t  glow_clock;

/* Where the player was standing and what the camera was doing, restored on the
   way out; and the yaw that faces him, solved once on the arm (he does not
   move while he dies). */
static int32_t  save_cx, save_cy, save_cz, save_crot, save_cvy;
static int32_t  from_rot, from_pitch;
static int32_t  face_rot;

/* ---- Small maths, borrowed (hadad_grinder.c, which borrowed them in turn) -- */

/* Shortest signed turn from `from` to `to`, in 4096ths. */
static int32_t turn_delta(int32_t from, int32_t to) {
    int32_t d = ((to - from) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    return d;
}

/* 0..256 ease-out. */
static int32_t ad_ease(int32_t t, int32_t total) {
    int32_t p = t * 256 / total; if (p > 256) p = 256;
    int32_t inv = 256 - p;
    return 256 - (inv * inv / 256);
}

/* Pitch that aims the camera at a point `dy` below it (world +Y is down, so
   dy > 0 is a downward, positive pitch) and `dz` in front of it. Eleven
   halvings on the SDK's own isin/icos, so the aim cannot disagree with the
   projection — hadad_grinder.c's aim_pitch verbatim. */
static int32_t aim_pitch(int32_t dy, int32_t dz) {
    int32_t lo = -1024, hi = 1024;
    while (hi - lo > 1) {
        int32_t mid = (lo + hi) >> 1, a = mid & 4095;
        if (isin(a) * dz < icos(a) * dy) lo = mid;
        else                             hi = mid;
    }
    return lo;
}

/* Integer square root — lumberer.c's, with the `t > 0` seed (ADDING_AN_ENEMY.txt
   mistake 11). */
static int32_t ad_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x, last, s = 1, t = v;
    while (t > 0) { t >>= 2; s <<= 1; }
    x = s;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

/* The yaw that faces a point (dx, dz) away. The camera's forward is
   (isin(rot), icos(rot)) in (x, z) — update_camera's convention — so this is
   the rot that maximises that dot. There is no arctangent in this SDK; one
   sweep of 4096 multiply-adds on the frame the scene arms is cheaper than being
   clever, and it answers in the same trig the view is built from. */
static int32_t ad_face(int32_t dx, int32_t dz) {
    int32_t best = 0, best_dot = -0x7FFFFFFF, a;
    for (a = 0; a < 4096; a += 4) {
        int32_t dot = (dx * isin(a) + dz * icos(a)) >> 4;
        if (dot > best_dot) { best_dot = dot; best = a; }
    }
    return best;
}

/* Pitch onto a world point from where the camera is standing. */
static int32_t ad_look_at(int32_t x, int32_t y, int32_t z) {
    int32_t dx = x - save_cx, dz = z - save_cz;
    int32_t fwd = ad_isqrt(dx * dx + dz * dz);
    if (fwd < 1) fwd = 1;
    return aim_pitch(y - save_cy, fwd);
}

/* ---- Public ---------------------------------------------------------------- */

int adapa_death_cutscene(void) {
    return state != AD_IDLE && state != AD_DONE;
}

static void ad_park(void) {
    state      = AD_IDLE;
    phase_t    = 0;
    orb_bright = 0;
}

void adapa_death_enter(void) { ad_park(); }

void adapa_death_reset(void) {
    ad_park();
    camera_release_player();
    cam_pitch = 0;
    sound_stop(SFX_WOOSH);
}

static void ad_begin(void) {
    camera_look_cancel();
    camera_anchor_player(cam_x, cam_y, cam_z);
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    from_rot   = cam_rot;
    from_pitch = cam_pitch;

    adapa_body(&orb_x, &orb_y, &orb_z);
    orb_y0   = orb_y;
    face_rot = ad_face(orb_x - save_cx, orb_z - save_cz);

    orb_bright = 0;
    glow_clock = 0;
    phase_t    = 0;
    state      = AD_TURN;
}

/* Control comes back: the release is the teleport (camera.h), and the camera
   is standing where the player was. Re-arm the room's Circle triggers — none of
   them was polled for nine seconds — and spend a press still held. */
static void ad_finish(void) {
    cam_x = save_cx; cam_y = save_cy; cam_z = save_cz;
    cam_rot = save_crot; cam_vy = save_cvy;
    cam_pitch = 0;              /* free-look never clears this for you */
    camera_release_player();

    interact_spend_press();
    if (current_area == STATE_PIANO_ROOM) pdoor_arm();
    if (current_area == STATE_LIBRARY)    library_doors_arm();

    /* Posted HERE and not on the kill: the log box is one of the things a
       cutscene hides, so a line posted nine seconds earlier would go unread. */
    show_pickup_msg_raw("Adapa's spirit was set free.");
    state = AD_DONE;
}

void adapa_death_update(void) {
    if (state == AD_IDLE || state == AD_DONE) {
        /* ARM LAZILY, off the body: the killing burn lands in
           helluminator_update, wherever that runs in the frame, and this picks
           it up on the next one. */
        if (adapa_dying()) ad_begin();
        return;
    }

    phase_t++;
    glow_clock++;
    cam_vy = 0;
    cam_x = save_cx; cam_y = save_cy; cam_z = save_cz;   /* turns, never moves */

    switch (state) {

    /* TURN — yaw and pitch onto his body, eased. */
    case AD_TURN: {
        int32_t e     = ad_ease(phase_t, AD_T_TURN);
        int32_t dst_p = ad_look_at(orb_x, orb_y0, orb_z);
        cam_rot   = (from_rot + (turn_delta(from_rot, face_rot) * e) / 256) & 4095;
        cam_pitch = from_pitch + ((dst_p - from_pitch) * e) / 256;
        /* The fade runs from the blow, not from here: the clip started on the
           killing frame, and he goes as it ends. */
        adapa_set_fade(256 - (phase_t * 256) / AD_T_FADE);
        if (phase_t >= AD_T_TURN) {
            cam_rot   = face_rot;
            cam_pitch = dst_p;
            state     = AD_FADE;   /* phase_t carries on: one clock for the fade */
        }
        break;
    }

    /* FADE — the camera holds; he goes over the rest of the clip. */
    case AD_FADE:
        cam_rot   = face_rot;
        cam_pitch = ad_look_at(orb_x, orb_y0, orb_z);
        adapa_set_fade(256 - (phase_t * 256) / AD_T_FADE);
        if (phase_t >= AD_T_FADE) {
            adapa_gone();
            orb_y   = orb_y0;
            phase_t = 0;
            state   = AD_ORB;
        }
        break;

    /* ORB — the spirit fades up where he was. */
    case AD_ORB:
        orb_bright = (phase_t * 256) / AD_ORB_IN;
        if (orb_bright > 256) orb_bright = 256;
        if (phase_t >= AD_T_ORB) {
            /* ONCE, as it GOES — Hadad's beat exactly. */
            sound_play(SFX_WOOSH);
            phase_t = 0;
            state   = AD_RISE;
        }
        break;

    /* RISE — constant acceleration, pitch solved from where the ball is on each
       frame rather than lerped between two end angles. */
    case AD_RISE:
        orb_y     = orb_y0 - (AD_RISE_ACCEL * phase_t * phase_t) / 256;
        cam_pitch = ad_look_at(orb_x, orb_y, orb_z);
        if (phase_t > AD_T_RISE - AD_ORB_FADE) {
            orb_bright = ((AD_T_RISE - phase_t) * 256) / AD_ORB_FADE;
            if (orb_bright < 0) orb_bright = 0;
        }
        if (phase_t >= AD_T_RISE) {
            orb_bright = 0;
            from_rot   = cam_rot;
            from_pitch = cam_pitch;
            phase_t    = 0;
            state      = AD_BACK;
        }
        break;

    /* BACK — the player's own yaw, and the pitch back to 0. */
    case AD_BACK: {
        int32_t e = ad_ease(phase_t, AD_T_BACK);
        cam_rot   = (from_rot + (turn_delta(from_rot, save_crot) * e) / 256) & 4095;
        cam_pitch = from_pitch - (from_pitch * e) / 256;
        if (phase_t >= AD_T_BACK) ad_finish();
        break;
    }

    default:
        break;
    }
}

/* ---- The spirit's glow ------------------------------------------------------
   hadad_grinder.c's hg_glow verbatim: concentric additive squares around the
   ball's projection, in the spirit's green, sorted at sz>>2 so it draws over
   the room (a spirit is not an object, and being occluded by a ceiling would
   read as a bug rather than as depth). */
#define AD_GLOW_WORLD        150
#define AD_GLOW_RINGS          3
static const uint8_t AD_GLOW_RGB[3] = { 40, 255, 120 };   /* Hadad's green */

static void ad_glow(RenderContext *ctx, int32_t x, int32_t y, int32_t z,
                    int32_t bright) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    SVECTOR pt;
    pt.vx = (int16_t)x; pt.vy = (int16_t)y; pt.vz = (int16_t)z; pt.pad = 0;

    DVECTOR sv;
    int32_t sz;
    gte_ldv0(&pt);
    gte_rtps();
    gte_stsxy(&sv);
    gte_stsz(&sz);
    if (sz == 0) return;
    if (sv.vx <= -1023 || sv.vx >= 1023 || sv.vy <= -1023 || sv.vy >= 1023) return;

    int32_t dx = x - cam_x, dy = y - cam_y, dz = z - cam_z;
    int32_t dist = ad_isqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 64) dist = 64;

    int32_t otz = sz >> 2;
    if (otz <= SCENE_OT_MIN)  otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;

    int ring;
    for (ring = 0; ring < AD_GLOW_RINGS; ring++) {
        if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) return;

        int32_t world_half = AD_GLOW_WORLD >> ring;
        int32_t half = (world_half * 256) / dist;   /* gte_SetGeomScreen(256) */
        if (half < 1)   half = 1;
        if (half > 400) half = 400;

        int32_t level = (bright * (60 + ring * 70)) >> 8;
        if (level > 256) level = 256;
        if (level <= 0)  continue;

        POLY_F4 *p = (POLY_F4 *)ctx->next_packet;
        setPolyF4(p);
        setSemiTrans(p, 1);
        setRGB0(p, (uint8_t)((AD_GLOW_RGB[0] * level) >> 8),
                   (uint8_t)((AD_GLOW_RGB[1] * level) >> 8),
                   (uint8_t)((AD_GLOW_RGB[2] * level) >> 8));
        p->x0 = (int16_t)(sv.vx - half); p->y0 = (int16_t)(sv.vy - half);
        p->x1 = (int16_t)(sv.vx + half); p->y1 = (int16_t)(sv.vy - half);
        p->x2 = (int16_t)(sv.vx - half); p->y2 = (int16_t)(sv.vy + half);
        p->x3 = (int16_t)(sv.vx + half); p->y3 = (int16_t)(sv.vy + half);
        addPrim(&ot[otz], p);
        ctx->next_packet += sizeof(POLY_F4);

        DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
        addPrim(&ot[otz], tp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
}

void adapa_death_draw(RenderContext *ctx) {
    if (!adapa_death_cutscene()) return;

    draw_particles(ctx);

    if (orb_bright > 0) {
        int32_t pulse = 224 + (isin((glow_clock * 40) & 4095) >> 6);  /* ~224..288 */
        int32_t level = (orb_bright * pulse) >> 8;
        if (level > 256) level = 256;
        ad_glow(ctx, orb_x, orb_y, orb_z, level);
    }
}
