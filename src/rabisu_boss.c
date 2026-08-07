#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "collision.h"     /* GROUND_FLOOR_Y */
#include "cdaudio.h"
#include "sound.h"         /* SFX_EMERGE / SFX_DMNSPEAK — the BOSS sound bank */
#include "btn_glyph.h"     /* btn_prompt_draw — screen-space text */
#include "garden_courtyard.h"
#include "rabisu.h"
#include "rabisu_boss.h"

/* The Rabisu encounter. See rabisu_boss.h for what it is and why it is a
   separate file from the boss itself. */

/* ---- The two camera shots ---------------------------------------------------
   SHOT 1, the crane. Due south of the boss's spawn on the same X, 1200 back and
   400 up (-Y is up), pitched down onto the middle of where the model will be
   standing once it is out of the ground.

   The numbers are worked, not eyeballed, and they have to be reworked TOGETHER:
   moving the camera without redoing the pitch just re-aims the shot at the far
   wall.
     - the spawn is (-290, 800, 0) — world.c places it, and rabisu_add turns the
       lawn surface y=900 into that underside anchor by hovering RBS_HOVER;
     - the model is RBS_HEIGHT (559) tall, so it occupies y 800 down to 241; the
       shot centres on y~600, a little below its middle, which buys headroom
       over the crown without letting the turf fall out of frame;
     - pitch = atan((600 - RBE_CAM_Y) / 1200) in 4096ths = atan(400/1200) =
       18.4deg = 210;
     - at gte_SetGeomScreen(256) on a 320x240 screen the half-field at 1200 is
       1200*120/256 = 562 vertically and 1200*160/256 = 750 horizontally, so the
       frame runs y[38,1162] and x[-1040,460] — which holds the lawn surface
       (900), the crown (241) with ~200 to spare, and all but the near corners
       of the 1149-wide light patch. Cropping those is deliberate: the near row
       is foreground, and standing far enough back to contain it was what made
       the boss look small in the first place.
   Yaw is 0, due north (+Z): forward is (sin,cos) of cam_rot, see update_camera.

   SHOT 2, the fight. On the south terrace, one wall standoff clear of the z =
   -2000 wall, on the boss's X, at ordinary standing eye height for a y=800
   floor — the same derivation garden_courtyard.h states for the east terrace.
   Pitch 0: free-look gameplay never sets pitch, and control is handed back
   here, so it must be flat. */
#define RBE_CAM_X          (-290)
#define RBE_CAM_Y            200
#define RBE_CAM_Z          (-1200)
#define RBE_CAM_ROT            0
#define RBE_CAM_PITCH        210

#define RBE_FIGHT_X        (-290)
#define RBE_FIGHT_Y   (800 - GROUND_FLOOR_Y - 40)
/* Wall 7 is the z = -2000 south wall and the room's standoff is
   GC_WALL_RADIUS (260), so -1740 is the exact limit. Sit 20 short of it: the
   player is handed control here, and being handed control ON a push-out
   boundary means the first frame of free play shoves them. */
#define RBE_FIGHT_Z        (-1720)
#define RBE_FIGHT_ROT          0

/* ---- Timing, in frames at 60 fps -------------------------------------------
   Everything the brief states in seconds, stated here once. */
#define RBE_T_CAM_IN          90   /* 1.5 s glide out to the crane shot     */
#define RBE_T_WATCH          120   /* 2 s of nothing, as specified          */
#define RBE_T_LIGHTS_UP      180   /* 3 s, sixteen lights two at a time     */
#define RBE_T_RISE           300   /* 5 s coming up through the lawn        */
#define RBE_T_LIGHTS_DOWN    120   /* 2 s fade                              */
#define RBE_T_LINE           360   /* 6 s per line of scripture             */
#define RBE_T_CAM_OUT         60   /* 1 s drop to the fight position        */

#define RBE_T_D_SETTLE        60   /* 1 s back to the crane + boss to spawn */
#define RBE_T_D_FREEZE       120   /* 2 s frozen, as specified              */
#define RBE_T_D_BURN         360   /* 6 s of shaking and light              */
#define RBE_T_D_FADE          90   /* 1.5 s burning away                    */
#define RBE_T_D_CAM_BACK      60   /* 1 s back to the player                */

/* How far under the lawn it waits. The model is RBS_HEIGHT tall and the lawn
   is at y=900, so the underside anchor has to sit at least 900 + 559 - 800 =
   659 below its spawn for the crown to be buried. 750 gives it a hand's
   breadth of soil. */
#define RBE_RISE_DEPTH       750

/* ---- The sixteen lights -----------------------------------------------------
   THE central 16 polys of the lawn: the 4x4 block of grass quads centred
   exactly on the boss's spawn at (-290, 0).

   These edges are read off "Garden Courtyard.smx", not invented. The sunken
   lawn is an 8x8 grid of flat y=900 quads spanning x[-1437,857] z[-571,1714];
   its columns are ~286 wide and its rows ~286 deep. The four columns and four
   rows below are the ones straddling the spawn, and their centre works out at
   (-291, 0) — the spawn, to within a unit of the mesh's own rounding.

   Re-export the courtyard mesh and these have to be re-read. */
#define RBE_LIGHTS            16
#define RBE_LAWN_Y           900
static const int16_t LAWN_X[5] = { -866, -580, -294, -6, 283 };
static const int16_t LAWN_Z[5] = { -571, -286,    0, 286, 571 };

/* Which pair lights when. Cells are row*4+col, and the order pairs each cell
   with its diagonal opposite so the patch opens outward from the middle rather
   than filling in from a corner — the middle is where the thing is coming up.
   Eight pairs across RBE_T_LIGHTS_UP is one pair every 22 frames. */
static const uint8_t LIGHT_ORDER[RBE_LIGHTS] = {
    5, 10,  6,  9,   1, 14,  2, 13,
    4, 11,  7,  8,   0, 15,  3, 12,
};
static uint8_t light_slot[RBE_LIGHTS];   /* cell -> its position in LIGHT_ORDER */

#define RBE_LIGHT_FADE        30   /* frames a single light takes to come up  */

/* >>> THE LIGHT ITSELF LIVES IN rabisu.c. <<< rbs_glow_pillar, rbs_glow_point,
   rbs_glow_pulse and rbs_glow_quad are shared with the boss's own light-beam
   and shockwave attacks, and they are shared deliberately: the brief asks for
   the beam and the death to read as the same phenomenon as this reveal, and
   one colour ramp and one blend is what actually delivers that rather than
   three effects that merely resemble each other. See the block comment on them
   in rabisu.h. All this file adds is WHICH sixteen polys, and WHEN. */

/* ---- Death lights ----------------------------------------------------------
   Four of them, hung on the model's own anchors (RBS_A_* in rabisu.h) via
   rbs_glow_point. */
#define RBE_SHAKE_MAX         11   /* world units of jitter at full intensity */

/* ---- The lines --------------------------------------------------------------
   Wrapped by hand rather than by a word-wrapper. The debug font advances 8px a
   character on a 320-wide screen, so 40 characters is the hard limit and both
   lines are past it; splitting them at their own commas and full stops reads
   better than any automatic break would. */
static const char *LINE_1A = "Repent sinner, for your deeds";
static const char *LINE_1B = "are numerous and ill";
static const char *LINE_2A = "Thou art but a servant.";
static const char *LINE_2B = "The cursed long for vengeance!";
#define RBE_TEXT_Y0          194
#define RBE_TEXT_Y1          208
#define RBE_TEXT_OT            2   /* menu-reserved range: on top of everything */

/* ---- State ---------------------------------------------------------------- */
typedef enum {
    RBE_IDLE = 0,      /* not running; the reveal arms itself from here */
    RBE_CAM_IN,
    RBE_WATCH,
    RBE_LIGHTS_UP,
    RBE_RISE,
    RBE_LIGHTS_DOWN,
    RBE_LINE1,
    RBE_LINE2,
    RBE_CAM_OUT,
    RBE_FIGHT,
    RBE_D_SETTLE,
    RBE_D_FREEZE,
    RBE_D_BURN,
    RBE_D_FADE,
    RBE_D_CAM_BACK,
    RBE_DONE,
} RbeState;

static RbeState state = RBE_IDLE;
static int32_t  phase_t   = 0;     /* frames elapsed in the current phase */
static int32_t  light_master = 0;  /* 0..256 over the whole lamp bank         */

/* Where the player was standing when the director took over, and what the
   camera was doing. Restored on the way out. */
static int32_t save_cx, save_cy, save_cz, save_crot, save_cvy;

/* ---- Which boss this encounter owns ----------------------------------------
   >>> LATCHED AS AN INDEX AT ARM TIME, AND NOT LOOKED UP AGAIN. <<<
   rabisu_boss_instance() answers "is there a LIVE boss in the CURRENT area",
   which is the right question for arming and exactly the wrong one for driving
   a running script. It goes NULL in two situations that are both perfectly
   normal, and in both the encounter used to tear itself down:

     - the frame after RBE_D_FADE sets `dead`. The bail then fired BEFORE
       RBE_D_CAM_BACK could run, releasing the camera while it still sat at the
       crane shot — so the player was dumped in the middle of the garden facing
       north instead of being carried back to where they were standing. That
       looked exactly like "it put me back at the start of the fight".
     - any frame the inventory menu is open, because game_state is STATE_MENU
       and the area tag no longer matches. Opening the menu mid-fight ended the
       whole encounter and unsealed the door.

   The index does not care about either. The only thing that can invalidate it
   is the slot itself going away, which is what boss() checks. */
static int boss_idx = -1;

static Rabisu *boss(void) {
    if (boss_idx < 0 || boss_idx >= rabisu_count) return NULL;
    Rabisu *r = &rabisus[boss_idx];
    return r->active ? r : NULL;
}

/* Camera glide (the piano puzzle's, factored out — this file needs four of
   them and inlining the lerp four times would be four chances to differ). */
typedef struct { int32_t x, y, z, rot, pitch; } CamShot;
static CamShot  glide_src, glide_dst;
static int32_t  glide_rot_delta, glide_len;

/* Shortest signed turn from `from` to `to`, in 4096ths. */
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

/* Ease-out, matching every other scripted camera move in the game: it leaves
   fast and settles, which is what stops a 1.5 s crane from reading as a slide. */
static void glide_step(int32_t t) {
    int32_t p = (t * 256) / glide_len; if (p > 256) p = 256;
    int32_t inv = 256 - p;
    int32_t e = 256 - (inv * inv / 256);
    cam_x     = glide_src.x     + ((glide_dst.x     - glide_src.x)     * e) / 256;
    cam_y     = glide_src.y     + ((glide_dst.y     - glide_src.y)     * e) / 256;
    cam_z     = glide_src.z     + ((glide_dst.z     - glide_src.z)     * e) / 256;
    cam_rot   = (glide_src.rot  + (glide_rot_delta * e) / 256) & 4095;
    cam_pitch = glide_src.pitch + ((glide_dst.pitch - glide_src.pitch) * e) / 256;
    cam_vy    = 0;
}

/* ---- Public predicates ----------------------------------------------------- */

int rabisu_boss_cutscene(void) {
    return state != RBE_IDLE && state != RBE_FIGHT && state != RBE_DONE;
}

int rabisu_boss_seals_door(void) {
    return state != RBE_IDLE && state != RBE_DONE;
}

void rabisu_boss_reset(void) {
    state        = RBE_IDLE;
    phase_t      = 0;
    light_master = 0;
    boss_idx     = -1;
    camera_release_player();
    cam_pitch = 0;
    /* Both are long clips on dedicated voices, so nothing else would ever cut
       them: a new game started while the reveal was mid-sentence would carry
       the speech into the delivery area. */
    sound_stop(SFX_EMERGE);
    sound_stop(SFX_DMNSPEAK);
}

void rabisu_boss_enter(void) {
    /* Park it and let the update arm itself. The boss is not PLACED until
       world_enter runs, which is after the room's init calls this — so any
       decision made here about whether there is a boss to reveal would be made
       a frame too early. */
    int i;
    state        = RBE_IDLE;
    phase_t      = 0;
    light_master = 0;
    boss_idx     = -1;
    cam_pitch    = 0;
    camera_release_player();
    for (i = 0; i < RBE_LIGHTS; i++) light_slot[LIGHT_ORDER[i]] = (uint8_t)i;
}

/* ---- Phase transitions ------------------------------------------------------ */

static void enter_phase(RbeState s) {
    state   = s;
    phase_t = 0;
}

static void begin_reveal(Rabisu *r) {
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    /* The player stays just inside the door while the camera cranes out. */
    camera_anchor_player(save_cx, save_cy, save_cz);

    /* Bury it. fade 0 keeps it out of the draw entirely rather than trusting
       the lawn to hide it (see the note on Rabisu.fade). */
    rabisu_go_dormant(r);
    /* Look at the CAMERA, not at the player. The player is anchored just
       inside the east cage door for the whole reveal, so the default
       player-facing would have it claw its way out of the ground with its back
       three-quarters turned to the shot. rabisu_fight_begin drops this again
       when control returns. Pointed at the fixed crane position rather than at
       the live cam_*, so it does not swivel while the camera glides. */
    rabisu_face_override(r, 1, RBE_CAM_X, RBE_CAM_Z);
    r->x = r->spawn_x;
    r->z = r->spawn_z;
    r->y = r->spawn_y + RBE_RISE_DEPTH;
    r->fade   = 0;
    r->clip_y = RBS_NO_CLIP;

    CamShot shot = { RBE_CAM_X, RBE_CAM_Y, RBE_CAM_Z, RBE_CAM_ROT, RBE_CAM_PITCH };
    glide_begin(&shot, RBE_T_CAM_IN);
    enter_phase(RBE_CAM_IN);
}

/* Hand the fight over: put the player at the south-wall spot and let go.
   The re-anchor happens at the START of the drop, not at the end, so the boss
   is already tracking where the player is about to be standing rather than
   snapping its gaze across the garden the instant control returns. */
static void begin_handover(void) {
    CamShot shot = { RBE_FIGHT_X, RBE_FIGHT_Y, RBE_FIGHT_Z, RBE_FIGHT_ROT, 0 };
    camera_anchor_player(RBE_FIGHT_X, RBE_FIGHT_Y, RBE_FIGHT_Z);
    glide_begin(&shot, RBE_T_CAM_OUT);
    enter_phase(RBE_CAM_OUT);
}

static void begin_death(Rabisu *r) {
    /* "Return to the players last position" — which is exactly where they are
       standing now, since they had control right up to the killing shot. */
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    camera_anchor_player(save_cx, save_cy, save_cz);

    /* Same reason as the reveal: the death plays to the crane shot, so it dies
       facing the camera rather than facing wherever the player happened to be
       standing when the last round went in. */
    rabisu_face_override(r, 1, RBE_CAM_X, RBE_CAM_Z);

    cdaudio_stop();

    CamShot shot = { RBE_CAM_X, RBE_CAM_Y, RBE_CAM_Z, RBE_CAM_ROT, RBE_CAM_PITCH };
    glide_begin(&shot, RBE_T_D_SETTLE);
    /* Where the body is being walked back FROM, so the settle can lerp it. */
    r->slash_from_x = r->x;
    r->slash_from_z = r->z;
    enter_phase(RBE_D_SETTLE);
}

/* ---- Update ---------------------------------------------------------------- */

void rabisu_boss_update(void) {
    Rabisu *r = boss();

    if (state == RBE_IDLE) {
        /* Arm on the first frame a living boss is in the room with us. */
        Rabisu *found = rabisu_boss_instance();
        if (found) { boss_idx = (int)(found - rabisus); begin_reveal(found); }
        return;
    }
    if (state == RBE_DONE) return;

    /* The boss vanished out from under the script (a debug jump, a load).
       Bail out cleanly rather than driving a NULL through the rest of this.
       See boss() for why this is the SLOT being empty and nothing else. */
    if (!r) {
        camera_release_player();
        cam_pitch = 0;
        state = RBE_DONE;
        return;
    }

    phase_t++;

    switch (state) {
    case RBE_CAM_IN:
        glide_step(phase_t);
        if (phase_t >= RBE_T_CAM_IN) enter_phase(RBE_WATCH);
        break;

    case RBE_WATCH:
        /* Two seconds of an empty lawn. Nothing happens on purpose. */
        if (phase_t >= RBE_T_WATCH) {
            light_master = 256;
            /* On the LIGHTS, not on the rise: the sound is what the two silent
               seconds have been setting up, and it has to arrive with the first
               pair of polys rather than three seconds after them. The clip runs
               11.1 s, which carries it across RBE_T_LIGHTS_UP (3 s) and
               RBE_T_RISE (5 s) and dies away under the fade — so the lights
               coming up and the thing coming out of the ground are one event.
               It is a BANKED effect (see sound.h); it is only ever audible
               because main.c swapped the boss bank in on the way through the
               cage door. */
            sound_play(SFX_EMERGE);
            enter_phase(RBE_LIGHTS_UP);
        }
        break;

    case RBE_LIGHTS_UP:
        if (phase_t >= RBE_T_LIGHTS_UP) {
            /* All sixteen burning. Now it comes up, and only now: the brief
               puts the rise AFTER the last pair lights, not overlapping it. */
            r->fade   = 256;
            r->clip_y = RBE_LAWN_Y;
            enter_phase(RBE_RISE);
        }
        break;

    case RBE_RISE: {
        int32_t p = (phase_t * 256) / RBE_T_RISE; if (p > 256) p = 256;
        /* Linear, not eased. Five seconds of something hauling itself out of
           the ground at a constant, inevitable rate is the read; an ease-out
           would make it look like it was being lifted on a wire. */
        r->y = (r->spawn_y + RBE_RISE_DEPTH) - (RBE_RISE_DEPTH * p) / 256;
        if (phase_t >= RBE_T_RISE) {
            r->y      = r->spawn_y;
            r->clip_y = RBS_NO_CLIP;
            enter_phase(RBE_LIGHTS_DOWN);
        }
        break;
    }

    case RBE_LIGHTS_DOWN:
        light_master = 256 - (phase_t * 256) / RBE_T_LIGHTS_DOWN;
        if (light_master < 0) light_master = 0;
        if (phase_t >= RBE_T_LIGHTS_DOWN) {
            light_master = 0;
            /* THE MUSIC STARTS HERE — not on entering the room, which is where
               it used to be (see the note in main.c's loading branch). The
               garden is silent from the cage door until the lights die. */
            cdaudio_play(CDAUDIO_COURTYARD_TRACK, 1);
            /* One utterance per line, fired as the line appears. The clip is
               8.6 s against a 6 s line, so the second play cuts the first
               short — which is why both go on the same voice (sound.c) and why
               this is two sound_play calls at the two phase entries rather than
               one long one: the speech has to break where the text does. */
            sound_play(SFX_DMNSPEAK);
            enter_phase(RBE_LINE1);
        }
        break;

    case RBE_LINE1:
        if (phase_t >= RBE_T_LINE) {
            sound_play(SFX_DMNSPEAK);   /* the second line */
            enter_phase(RBE_LINE2);
        }
        break;

    case RBE_LINE2:
        if (phase_t >= RBE_T_LINE) begin_handover();
        break;

    case RBE_CAM_OUT:
        glide_step(phase_t);
        if (phase_t >= RBE_T_CAM_OUT) {
            cam_x = RBE_FIGHT_X; cam_y = RBE_FIGHT_Y; cam_z = RBE_FIGHT_Z;
            cam_rot = RBE_FIGHT_ROT; cam_pitch = 0; cam_vy = 0;
            /* Releasing the anchor makes the camera the player again — and the
               camera is standing at the fight spot, which is the whole trick
               behind "the camera moves there and control is given back". */
            camera_release_player();
            garden_courtyard_door_arm();
            rabisu_fight_begin(r);
            enter_phase(RBE_FIGHT);
        }
        break;

    case RBE_FIGHT:
        if (r->dying) begin_death(r);
        break;

    case RBE_D_SETTLE: {
        glide_step(phase_t);
        int32_t p = (phase_t * 256) / RBE_T_D_SETTLE; if (p > 256) p = 256;
        int32_t inv = 256 - p;
        int32_t e = 256 - (inv * inv / 256);
        r->x = r->slash_from_x + ((r->spawn_x - r->slash_from_x) * e) / 256;
        r->z = r->slash_from_z + ((r->spawn_z - r->slash_from_z) * e) / 256;
        r->y = r->spawn_y;
        if (phase_t >= RBE_T_D_SETTLE) {
            r->x = r->spawn_x; r->z = r->spawn_z;
            r->frozen = 1;   /* the idle flap stops dead */
            enter_phase(RBE_D_FREEZE);
        }
        break;
    }

    case RBE_D_FREEZE:
        if (phase_t >= RBE_T_D_FREEZE) enter_phase(RBE_D_BURN);
        break;

    case RBE_D_BURN:
        /* The shake builds rather than switching on: it is coming apart, and
           the six seconds should escalate. The lights (drawn below) ramp on the
           same curve. */
        r->shake = (RBE_SHAKE_MAX * phase_t) / RBE_T_D_BURN;
        if (phase_t >= RBE_T_D_BURN) enter_phase(RBE_D_FADE);
        break;

    case RBE_D_FADE:
        r->fade = 256 - (phase_t * 256) / RBE_T_D_FADE;
        if (r->fade < 0) r->fade = 0;
        if (phase_t >= RBE_T_D_FADE) {
            r->fade  = 0;
            r->shake = 0;
            r->dead  = 1;   /* NOW it is gone: skipped by update, draw and save */
            /* Home is where the player has been standing all along. */
            CamShot back = { save_cx, save_cy, save_cz, save_crot, 0 };
            glide_begin(&back, RBE_T_D_CAM_BACK);
            enter_phase(RBE_D_CAM_BACK);
        }
        break;

    case RBE_D_CAM_BACK:
        glide_step(phase_t);
        if (phase_t >= RBE_T_D_CAM_BACK) {
            cam_x = save_cx; cam_y = save_cy; cam_z = save_cz;
            cam_rot = save_crot; cam_vy = save_cvy; cam_pitch = 0;
            camera_release_player();
            garden_courtyard_door_arm();
            state = RBE_DONE;
        }
        break;

    default:
        break;
    }
}

/* One lawn light. All of the work is rbs_glow_pillar's (rabisu.c) — the pool
   on the turf and the flaring shaft above it are the same effect the boss's
   light beam paints on the ground, and they are one routine so they cannot
   drift apart. What belongs here is only WHICH cell and HOW BRIGHT. */
static void draw_lawn_light(RenderContext *ctx, int cell, int32_t bright) {
    int col = cell & 3, row = cell >> 2;
    /* Each light runs its own clock, offset by cell, so the sixteen shimmer out
       of step instead of strobing the whole patch as one lamp. */
    rbs_glow_pillar(ctx, LAWN_X[col], LAWN_X[col + 1],
                    LAWN_Z[row], LAWN_Z[row + 1], RBE_LAWN_Y,
                    bright, rbs_glow_clock + cell * 9);
}

void rabisu_boss_draw(RenderContext *ctx) {
    if (state == RBE_IDLE || state == RBE_DONE) return;

    /* --- The lawn lights --- */
    if (light_master > 0) {
        int cell;
        for (cell = 0; cell < RBE_LIGHTS; cell++) {
            int32_t bright = 256;
            if (state == RBE_LIGHTS_UP) {
                /* Two at a time, eight pairs, three seconds. The eight start
                   times are spread over (3 s - one fade) rather than over the
                   full three seconds, so the LAST pair finishes coming up
                   exactly as the phase ends — otherwise it would still be at
                   three-quarter brightness when the rise begins and would pop
                   the rest of the way. */
                int32_t on = ((int32_t)(light_slot[cell] >> 1) *
                              (RBE_T_LIGHTS_UP - RBE_LIGHT_FADE)) / 7;
                bright = ((phase_t - on) * 256) / RBE_LIGHT_FADE;
                if (bright < 0)   bright = 0;
                if (bright > 256) bright = 256;
            }
            bright = (bright * light_master) >> 8;
            if (bright > 0) draw_lawn_light(ctx, cell, bright);
        }
    }

    /* --- The death lights: crown, both wing tips, chest --- */
    if (state == RBE_D_BURN || state == RBE_D_FADE) {
        Rabisu *r = boss();
        if (r) {
            /* Ramps in over the burn and rides the model's own fade out, so the
               light does not outlive the body it is pouring out of. */
            int32_t bright = (state == RBE_D_BURN)
                           ? (phase_t * 256) / RBE_T_D_BURN
                           : r->fade;
            if (bright > 256) bright = 256;
            if (bright > 0) {
                static const int32_t A[4][3] = {
                    { RBS_A_HEAD_X,   RBS_A_HEAD_Y,  RBS_A_HEAD_Z  },
                    { RBS_A_WING_X,   RBS_A_WING_Y,  RBS_A_WING_Z  },
                    { -RBS_A_WING_X,  RBS_A_WING_Y,  RBS_A_WING_Z  },
                    { RBS_A_CHEST_X,  RBS_A_CHEST_Y, RBS_A_CHEST_Z },
                };
                int a;
                for (a = 0; a < 4; a++) {
                    VECTOR w;
                    rabisu_anchor_world(r, A[a][0], A[a][1], A[a][2], &w);
                    rbs_glow_point(ctx, &w, bright, rbs_glow_clock + a * 13);
                }
            }
        }
    }
}

/* ---- Drawing: the subtitles ------------------------------------------------
   btn_prompt_draw sorts its text into the OT via FntSort (never FntFlush, which
   draws immediately and races a scene still being laid down — see the note in
   main.c's debug overlay). The font advances 8px a character, so centring is a
   character count and a subtraction. */
static void rbe_line(RenderContext *ctx, const char *s, int y) {
    int w = (int)strlen(s) * 8;
    int x = (SCREEN_XRES - w) / 2;
    if (x < 0) x = 0;
    btn_prompt_draw(ctx, x, y, s, RBE_TEXT_OT);
}

void rabisu_boss_draw_overlay(RenderContext *ctx) {
    if (state == RBE_LINE1) {
        rbe_line(ctx, LINE_1A, RBE_TEXT_Y0);
        rbe_line(ctx, LINE_1B, RBE_TEXT_Y1);
    } else if (state == RBE_LINE2) {
        rbe_line(ctx, LINE_2A, RBE_TEXT_Y0);
        rbe_line(ctx, LINE_2B, RBE_TEXT_Y1);
    }
}
