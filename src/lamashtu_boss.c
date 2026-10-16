#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "collision.h"     /* GROUND_FLOOR_Y */
#include "cdaudio.h"
#include "sound.h"
#include "btn_glyph.h"     /* btn_prompt_draw - screen-space text */
#include "player.h"        /* game_flag / FLAG_LAMASHTU_DEAD */
#include "title.h"         /* STATE_LAMASHTU_ARENA, current_area */
#include "bars.h"
#include "lms_creep.h"
#include "rabisu.h"        /* rbs_glow_point: the death's lights, the Rabisu's */
#include "lamashtu.h"
#include "lamashtu_arena.h"
#include "lamashtu_boss.h"

/* The Lamashtu encounter. See lamashtu_boss.h for the beat sheet; the camera
   machinery is src/rabisu_boss.c's (glide_begin/glide_step/aim_pitch). */

/* ---- The seven phases --------------------------------------------------------
   Read off misc/Lamashtu fight plan, one grid per phase: the green circle is
   her and its arrow her facing, the red lines are gates. The plan's grid is
   the arena's - six pillar columns, centres x 900..6900, four pillar rows,
   centres z -300..3300 - with the image's UP as +Z (Blender's top view; the
   west door is on its left). Every red line fills ONE 600 gap, between two
   pillars or between a pillar and the wall, and a gate is named by the gap's
   centre:

     'X'  runs along X (rot 0), across a pillar ROW:    x = gap, z = row
     'Z'  runs along Z (rot 1024), down a pillar COLUMN: x = column, z = gap

   Gaps along X are centred at 300 (the west wall to column 1), 1500, 2700,
   3900, 5100, 6300 and 7500 (column 6 to the east wall); along Z at -900 (the
   south wall to row 1), 300, 1500, 2700 and 3900 (row 4 to the north wall).

   Her spots are aisle crossings (or, in phase 3, the gap in a pillar row),
   each one block of the 600 grid, which is her size. */
typedef struct { char axis; int16_t x, z; } LbeGate;
typedef struct {
    int16_t x, z;
    LamashtuFacing facing;
    uint8_t n;
    LbeGate g[11];
} LbePhase;

#define LBE_PHASES  7

static const LbePhase PHASE[LBE_PHASES] = {
    /* 1: the opening spot, five squares down the door aisle. A box: the
          north and south pillar rows walled from the west wall to column 4,
          and column 4 walled between them. */
    { 2700, 1500, LMS_FACE_WEST, 11, {
        {'X',  300, 3300}, {'X', 1500, 3300}, {'X', 2700, 3300}, {'X', 3900, 3300},
        {'X',  300, -300}, {'X', 1500, -300}, {'X', 2700, -300}, {'X', 3900, -300},
        {'Z', 4500,  300}, {'Z', 4500, 1500}, {'Z', 4500, 2700} } },
    /* 2: the east end of the door aisle. */
    { 6300, 1500, LMS_FACE_WEST, 5, {
        {'X', 3900, 2100}, {'X', 3900,  900},
        {'Z', 3300,  300}, {'Z', 3300, 2700}, {'Z', 4500, 1500} } },
    /* 3: in the gap of the north pillar row, looking south. Column 3 walled
          from wall to wall. */
    { 3900, 3300, LMS_FACE_SOUTH, 11, {
        {'X', 7500, 3300}, {'X', 6300, 2100}, {'X', 6300,  900}, {'X', 7500, -300},
        {'Z', 3300, -900}, {'Z', 3300,  300}, {'Z', 3300, 1500}, {'Z', 3300, 2700},
        {'Z', 3300, 3900}, {'Z', 6900,  300}, {'Z', 6900, 2700} } },
    /* 4: the south-west crossing, looking east. */
    { 1500,  300, LMS_FACE_EAST, 9, {
        {'X', 1500, 2100}, {'X', 2700, 2100}, {'X', 3900, 2100},
        {'Z',  900, 2700}, {'Z',  900, 3900}, {'Z', 2100, -900},
        {'Z', 3300,  300}, {'Z', 3300, 1500}, {'Z', 4500, 2700} } },
    /* 5: the south-east crossing, looking west. */
    { 6300,  300, LMS_FACE_WEST, 10, {
        {'X', 1500, 2100}, {'X', 3900, 2100}, {'X', 6300, 2100},
        {'Z',  900, 2700}, {'Z',  900, 3900}, {'Z', 2100, -900},
        {'Z', 3300,  300}, {'Z', 6900, -900}, {'Z', 6900, 1500}, {'Z', 6900, 2700} } },
    /* 6: the door aisle's west crossing, looking north. */
    { 1500, 1500, LMS_FACE_NORTH, 11, {
        {'X', 1500, 2100}, {'X', 3900, 2100}, {'X', 5100, 2100}, {'X', 6300, 2100},
        {'Z',  900,  300}, {'Z',  900, 2700}, {'Z', 2100, -900}, {'Z', 2100,  300},
        {'Z', 3300,  300}, {'Z', 6900, 2700}, {'Z', 6900, 3900} } },
    /* 7: ADDED SO PHASE 6 CANNOT TRAP THE PLAYER. Shot from a distance in
          phase 6, a player can be left east of phase 1's column-4 wall with no
          way back to her; this phase sits between the two, with her in the
          west wall's gap right in front of the door (x[0,600], z 1500),
          looking south, and column 3 walled from wall to wall. Its gates hand
          over to phase 1's without a gap a player could be caught behind. */
    {  300, 1500, LMS_FACE_SOUTH, 10, {
        {'X',  300, 3300}, {'X', 1500, 3300}, {'X', 1500, 2100},
        {'Z',  900, 1500}, {'Z',  900, 3900},
        {'Z', 3300, -900}, {'Z', 3300,  300}, {'Z', 3300, 1500},
        {'Z', 3300, 2700}, {'Z', 3300, 3900} } },
};

/* Every gate is the 600 gap's width and 600 tall, as the user set the first
   one (200 short of the 800 vault). */
#define LBE_GATE_W   600
#define LBE_GATE_H   600

/* ---- The camera ---------------------------------------------------------------
   "Hovering just in front of Lamashtu, angled down at her face." The face is
   LMS_FACE_* in her own frame (y -513, 404 in front of her centre). The shot
   stands LBE_FRONT_DIST further out along her facing, at LBE_CAM_Y: 100 under
   the -800 vault, which is as high as the room allows (ADDING_A_BOSS_
   ENCOUNTER.txt, "mind the ceiling"), and 187 above the face - so it looks
   down at about 20 degrees, solved by aim_pitch rather than written down.

   At 500 the half-field is 500*120/256 = 234 vertically: crown to chest. Every
   phase's spot puts that camera in an open aisle (her facing points down one
   by construction), which is what lets the DEATH use the same shot wherever
   she happens to be standing. */
#define LBE_FRONT_DIST  500
#define LBE_CAM_Y     (-700)

/* ---- Timing, in frames at 60 fps --------------------------------------------- */
#define LBE_T_INTRO_IN   150   /* 2.5 s: the door to her face                 */
#define LBE_T_LINE       360   /* 6 s a line: the Rabisu's and Asag's cadence */
#define LBE_T_INTRO_OUT   90   /* 1.5 s back to the door                     */
#define LBE_T_FADE_OUT   180   /* the shriek: lamashtu_shriek_ticks() if it
                                  loaded (36 frames x 5 = 180), this if not   */
#define LBE_T_FADE_IN     90   /* 1.5 s at her next spot                     */

/* The death: the Rabisu's, with the settle a pan. BURN + FADE is the length of
   SFX_EXPLODE (5.374 s = 322 frames), as there. */
#define LBE_T_D_PAN       90   /* 1.5 s round to her front                    */
#define LBE_T_D_FREEZE   120   /* 2 s frozen                                  */
#define LBE_T_D_BURN     232   /* 3.87 s shaking and pouring light            */
#define LBE_T_D_FADE      90   /* 1.5 s burning away                          */
#define LBE_T_D_CAM_BACK  60   /* 1 s back to the player                      */
#define LBE_SHAKE_MAX     11

/* ---- The lines --------------------------------------------------------------
   As given, wrapped by hand at their own punctuation: the debug font is 8 px a
   character on a 320 screen, so 40 is the limit (the Rabisu's rule). */
static const char *LINE_1A = "COME MY CHILD, COME TO MUMMY.";
static const char *LINE_1B = "SUCKLE AT MY ANXIOUS TEET";
static const char *LINE_2A = "ALL MY CHILDREN SHALL CRY AND SUFFER,";
static const char *LINE_2B = "FOR THAT IS THEIR PURPOSE.";
#define LBE_TEXT_Y0      194
#define LBE_TEXT_Y1      208
#define LBE_TEXT_OT        2

/* ---- State ------------------------------------------------------------------- */
typedef enum {
    LBE_IDLE = 0,     /* not running: arms itself, or she is dead             */
    LBE_INTRO_IN,
    LBE_LINE1,
    LBE_LINE2,
    LBE_INTRO_OUT,
    LBE_FIGHT,        /* free play: she is in, crying, hittable               */
    LBE_FADE_OUT,     /* free play: hit, shrieking, going                     */
    LBE_FADE_IN,      /* free play: arriving at the next spot                 */
    LBE_D_PAN,
    LBE_D_FREEZE,
    LBE_D_BURN,
    LBE_D_FADE,
    LBE_D_CAM_BACK,
    LBE_DONE,
} LbeState;

static LbeState state = LBE_IDLE;
static int32_t  phase_t;
static int      fight_phase;          /* 0..6                                  */
static int32_t  fade_len;
static int32_t  glow_clock;
static int32_t  save_cx, save_cy, save_cz, save_crot, save_cvy;

/* The live gates: which gap, and bars.c's index for it. */
#define LBE_MAX_LIVE  24
typedef struct { LbeGate g; int idx; } LbeLive;
static LbeLive live[LBE_MAX_LIVE];
static int     live_n;

/* ---- Camera glides: src/rabisu_boss.c's, verbatim ----------------------------- */
typedef struct { int32_t x, y, z, rot, pitch; } CamShot;
static CamShot  glide_src, glide_dst;
static int32_t  glide_rot_delta, glide_len;

static int32_t turn_delta(int32_t from, int32_t to) {
    int32_t d = ((to - from) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    return d;
}

static void glide_begin(const CamShot *dst, int32_t frames) {
    glide_src.x = cam_x; glide_src.y = cam_y; glide_src.z = cam_z;
    glide_src.rot = cam_rot; glide_src.pitch = cam_pitch;
    glide_dst = *dst;
    glide_rot_delta = turn_delta(glide_src.rot, glide_dst.rot);
    glide_len = frames;
}

/* EASE IN AND OUT for the two long moves (the opening's 2.5 s and the death's
   pan), ease-out for the short returns - the runbook's two curves, and which
   is which. */
static void glide_step(int32_t t, int smooth) {
    int32_t p = (t * 256) / glide_len; if (p > 256) p = 256;
    int32_t e;
    if (smooth) {
        e = (p * p * (3 * 256 - 2 * p)) / (256 * 256);
    } else {
        int32_t inv = 256 - p;
        e = 256 - (inv * inv / 256);
    }
    cam_x     = glide_src.x     + ((glide_dst.x     - glide_src.x)     * e) / 256;
    cam_y     = glide_src.y     + ((glide_dst.y     - glide_src.y)     * e) / 256;
    cam_z     = glide_src.z     + ((glide_dst.z     - glide_src.z)     * e) / 256;
    cam_rot   = (glide_src.rot  + (glide_rot_delta * e) / 256) & 4095;
    cam_pitch = glide_src.pitch + ((glide_dst.pitch - glide_src.pitch) * e) / 256;
    cam_vy    = 0;
}

/* Pitch aiming at a point dy below and dz in front (rabisu_boss.c's). */
static int32_t aim_pitch(int32_t dy, int32_t dz) {
    int32_t lo = -1024, hi = 1024;
    while (hi - lo > 1) {
        int32_t mid = (lo + hi) >> 1, a = mid & 4095;
        if (isin(a) * dz < icos(a) * dy) lo = mid;
        else                             hi = mid;
    }
    return lo;
}

/* The shot in front of her face, wherever she stands now. The yaw looks back
   along her facing: forward is (sin, cos) of cam_rot, so facing west (-X) is
   looked at from the west with yaw 1024, and so on. */
static void front_shot(CamShot *s) {
    VECTOR face;
    int32_t fx, fz;
    lamashtu_local_to_world(0, LMS_FACE_LY, LMS_FACE_LZ, &face);
    lamashtu_facing_vec(&fx, &fz);
    s->x = face.vx + ((fx * LBE_FRONT_DIST) >> 12);
    s->z = face.vz + ((fz * LBE_FRONT_DIST) >> 12);
    s->y = LBE_CAM_Y;
    if      (fx < 0) s->rot = 1024;
    else if (fx > 0) s->rot = 3072;
    else if (fz < 0) s->rot = 0;
    else             s->rot = 2048;
    s->pitch = aim_pitch(face.vy - LBE_CAM_Y, LBE_FRONT_DIST);
}

/* ---- The gates ---------------------------------------------------------------- */
static int gate_eq(const LbeGate *a, const LbeGate *b) {
    return a->axis == b->axis && a->x == b->x && a->z == b->z;
}

static int phase_has(int p, const LbeGate *g) {
    int i;
    if (p < 0) return 0;
    for (i = 0; i < PHASE[p].n; i++)
        if (gate_eq(&PHASE[p].g[i], g)) return 1;
    return 0;
}

/* Bring the gates to phase `p` (-1: none). A live gate the phase does not
   have goes down into the floor (and frees itself there - bars_sink); a gate
   the phase has that is not live comes up; one in both stays put. */
static void gates_to(int p) {
    int i, j;
    for (i = 0; i < live_n; ) {
        if (!phase_has(p, &live[i].g)) {
            bars_sink(live[i].idx);
            live[i] = live[--live_n];
        } else {
            i++;
        }
    }
    if (p < 0) return;
    for (i = 0; i < PHASE[p].n; i++) {
        const LbeGate *g = &PHASE[p].g[i];
        int have = 0;
        for (j = 0; j < live_n; j++) if (gate_eq(&live[j].g, g)) { have = 1; break; }
        if (have || live_n >= LBE_MAX_LIVE) continue;
        int idx = bars_pop_up(STATE_LAMASHTU_ARENA, g->x,
                              LMS_FLOOR_Y - GROUND_FLOOR_Y, g->z,
                              g->axis == 'X' ? 0 : 1024,
                              LBE_GATE_W, LBE_GATE_H);
        if (idx < 0) continue;
        live[live_n].g   = *g;
        live[live_n].idx = idx;
        live_n++;
    }
}

/* ---- Public ---------------------------------------------------------------- */
int lamashtu_boss_cutscene(void) {
    return state == LBE_INTRO_IN || state == LBE_LINE1 || state == LBE_LINE2 ||
           state == LBE_INTRO_OUT ||
           state == LBE_D_PAN || state == LBE_D_FREEZE || state == LBE_D_BURN ||
           state == LBE_D_FADE || state == LBE_D_CAM_BACK;
}

int lamashtu_boss_seals_door(void) {
    return state != LBE_IDLE && state != LBE_DONE;
}

static void park(void) {
    state       = LBE_IDLE;
    phase_t     = 0;
    fight_phase = 0;
    live_n      = 0;
    cam_pitch   = 0;
    camera_release_player();
}

void lamashtu_boss_reset(void) {
    park();
    sound_stop(SFX_DMNSPEAK);
    sound_stop(SFX_EXPLODE);
}

/* The arena's init, every arrival. bars_clear() has run there, so there are no
   live gates whatever this held before. */
void lamashtu_boss_enter(void) { park(); }

static void enter(LbeState s) { state = s; phase_t = 0; }

static void begin_opening(void) {
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    camera_anchor_player(save_cx, save_cy, save_cz);

    fight_phase = 0;
    lamashtu_place(PHASE[0].x, PHASE[0].z, PHASE[0].facing);
    lamashtu_set_visible(1);
    lamashtu_set_fade(256);
    lamashtu_set_glow(1);
    gates_to(0);                 /* "the bars pop up as the player enters" */

    CamShot s;
    front_shot(&s);
    glide_begin(&s, LBE_T_INTRO_IN);
    enter(LBE_INTRO_IN);
}

static void begin_fight_phase(void) {
    lamashtu_set_fade(256);
    lamashtu_set_glow(1);
    lamashtu_set_vulnerable(1);
    lamashtu_set_aggressive(1);   /* the cry starts "once she's fully faded in" */
    enter(LBE_FIGHT);
}

static void begin_death(void) {
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    camera_anchor_player(save_cx, save_cy, save_cz);

    game_flag_set(FLAG_LAMASHTU_DEAD);
    gates_to(-1);
    lms_creeps_reset();           /* what she poured goes with her */
    lamashtu_cancel_wave();
    lamashtu_set_aggressive(0);
    cdaudio_stop();

    CamShot s;
    front_shot(&s);
    glide_begin(&s, LBE_T_D_PAN);
    enter(LBE_D_PAN);
}

void lamashtu_boss_update(void) {
    if (current_area != STATE_LAMASHTU_ARENA) return;

    if (state == LBE_IDLE) {
        if (!game_flag(FLAG_LAMASHTU_DEAD) && lamashtu_model_loaded())
            begin_opening();
        return;
    }
    if (state == LBE_DONE) return;

    /* The model went out from under the script (a debug jump): bail cleanly. */
    if (!lamashtu_model_loaded()) {
        camera_release_player();
        cam_pitch = 0;
        state = LBE_DONE;
        return;
    }

    phase_t++;
    glow_clock++;

    switch (state) {
    case LBE_INTRO_IN:
        glide_step(phase_t, 1);
        if (phase_t >= LBE_T_INTRO_IN) {
            /* The bosses' music and the first line together, as both of theirs
               do: the voice breaks where the text does, so it is keyed per
               line on its own voice (sound.c). */
            cdaudio_play(CDAUDIO_COURTYARD_TRACK, 1);
            sound_play(SFX_DMNSPEAK);
            enter(LBE_LINE1);
        }
        break;

    case LBE_LINE1:
        if (phase_t >= LBE_T_LINE) {
            sound_play(SFX_DMNSPEAK);
            enter(LBE_LINE2);
        }
        break;

    case LBE_LINE2:
        if (phase_t >= LBE_T_LINE) {
            CamShot back = { save_cx, save_cy, save_cz, save_crot, 0 };
            glide_begin(&back, LBE_T_INTRO_OUT);
            enter(LBE_INTRO_OUT);
        }
        break;

    case LBE_INTRO_OUT:
        glide_step(phase_t, 0);
        if (phase_t >= LBE_T_INTRO_OUT) {
            cam_x = save_cx; cam_y = save_cy; cam_z = save_cz;
            cam_rot = save_crot; cam_vy = save_cvy; cam_pitch = 0;
            camera_release_player();
            begin_fight_phase();
        }
        break;

    case LBE_FIGHT:
        if (lamashtu_hit_taken()) {
            if (lamashtu_health() <= 0) { begin_death(); break; }
            /* "As soon as she takes the hit the bars for the current phase go
               down and the ones for the next phase pop up." */
            gates_to((fight_phase + 1) % LBE_PHASES);
            fade_len = lamashtu_shriek_ticks();
            if (fade_len <= 0) fade_len = LBE_T_FADE_OUT;
            enter(LBE_FADE_OUT);
        }
        break;

    case LBE_FADE_OUT:
        lamashtu_set_fade(256 - (phase_t * 256) / fade_len);
        if (phase_t >= fade_len) {
            fight_phase = (fight_phase + 1) % LBE_PHASES;
            lamashtu_place(PHASE[fight_phase].x, PHASE[fight_phase].z,
                           PHASE[fight_phase].facing);
            lamashtu_set_fade(0);
            lamashtu_set_glow(1);
            enter(LBE_FADE_IN);
        }
        break;

    case LBE_FADE_IN:
        lamashtu_set_fade((phase_t * 256) / LBE_T_FADE_IN);
        if (phase_t >= LBE_T_FADE_IN) begin_fight_phase();
        break;

    case LBE_D_PAN:
        glide_step(phase_t, 1);
        if (phase_t >= LBE_T_D_PAN) {
            lamashtu_set_frozen(1);
            enter(LBE_D_FREEZE);
        }
        break;

    case LBE_D_FREEZE:
        if (phase_t >= LBE_T_D_FREEZE) {
            lamashtu_set_burning(1);
            sound_play(SFX_EXPLODE);
            enter(LBE_D_BURN);
        }
        break;

    case LBE_D_BURN:
        lamashtu_set_shake((LBE_SHAKE_MAX * phase_t) / LBE_T_D_BURN);
        if (phase_t >= LBE_T_D_BURN) enter(LBE_D_FADE);
        break;

    case LBE_D_FADE:
        lamashtu_set_fade(256 - (phase_t * 256) / LBE_T_D_FADE);
        if (phase_t >= LBE_T_D_FADE) {
            lamashtu_set_visible(0);
            lamashtu_set_shake(0);
            lamashtu_set_burning(0);
            CamShot back = { save_cx, save_cy, save_cz, save_crot, 0 };
            glide_begin(&back, LBE_T_D_CAM_BACK);
            enter(LBE_D_CAM_BACK);
        }
        break;

    case LBE_D_CAM_BACK:
        glide_step(phase_t, 0);
        if (phase_t >= LBE_T_D_CAM_BACK) {
            cam_x = save_cx; cam_y = save_cy; cam_z = save_cz;
            cam_rot = save_crot; cam_vy = save_cvy; cam_pitch = 0;
            camera_release_player();
            lamashtu_arena_arm();      /* a Circle held through it must not fire */
            cdaudio_play(CDAUDIO_CATACOMBS_TRACK, 1);
            state = LBE_DONE;
        }
        break;

    default:
        break;
    }
}

/* ---- The death's lights: the Rabisu's glow points, hung on her own frame -----
   Her crown, both shoulders and her chest, ramped in over the burn and riding
   her fade out, so the light never outlives the body it pours out of. */
void lamashtu_boss_draw(RenderContext *ctx) {
    if (state != LBE_D_BURN && state != LBE_D_FADE) return;
    int32_t bright = (state == LBE_D_BURN) ? (phase_t * 256) / LBE_T_D_BURN
                                           : lamashtu_fade();
    if (bright > 256) bright = 256;
    if (bright <= 0) return;
    static const int16_t A[4][3] = {
        {    0, -560, -380 },   /* crown */
        {  260, -560, -290 },   /* shoulders */
        { -260, -560, -290 },
        {    0, -330, -320 },   /* chest */
    };
    int a;
    for (a = 0; a < 4; a++) {
        VECTOR w;
        lamashtu_local_to_world(A[a][0], A[a][1], A[a][2], &w);
        rbs_glow_point(ctx, &w, bright, glow_clock + a * 13);
    }
}

static void lbe_line(RenderContext *ctx, const char *s, int y) {
    int w = (int)strlen(s) * 8;
    int x = (SCREEN_XRES - w) / 2;
    if (x < 0) x = 0;
    btn_prompt_draw(ctx, x, y, s, LBE_TEXT_OT);
}

void lamashtu_boss_draw_overlay(RenderContext *ctx) {
    if (state == LBE_LINE1) {
        lbe_line(ctx, LINE_1A, LBE_TEXT_Y0);
        lbe_line(ctx, LINE_1B, LBE_TEXT_Y1);
    } else if (state == LBE_LINE2) {
        lbe_line(ctx, LINE_2A, LBE_TEXT_Y0);
        lbe_line(ctx, LINE_2B, LBE_TEXT_Y1);
    }
}
