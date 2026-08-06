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
#define RBE_SHAFT_H          900   /* how far up the beam reaches from the turf */
/* >>> THE CONE FLARES OUTWARD AS IT RISES. <<< It is light coming OUT OF the
   ground, not a spotlight shining down onto it, and a cone is read almost
   entirely by which end is wide: narrowing it upward — which is what the
   lightswitch puzzle's ceiling cones do, and what this was copied from — reads
   unmistakably as a lamp in the sky picking out the lawn. Each corner is
   pushed this far AWAY from its cell's centre by the top. */
#define RBE_SHAFT_FLARE      150
#define RBE_SHAFT_SEGS         3

/* Pulse: white -> orange -> red -> white, one leg per RBE_PULSE_STEP frames, so
   a 78-frame (1.3 s) round trip. Used by the lawn lights AND by the death
   lights, which is the point — the brief asks for the death to read as the same
   phenomenon as the reveal, and sharing the colour ramp is what does that. */
static const uint8_t PULSE[3][3] = {
    { 255, 250, 235 },   /* white  */
    { 255, 145,  30 },   /* orange */
    { 255,  35,  10 },   /* red    */
};
#define RBE_PULSE_STEP        26

/* Additive brightness of each part of a beam, as a 0..256 scale of the pulse
   colour. Faint in the shaft so sixteen of them do not white out the middle of
   the garden; the pool on the turf is where the colour has to read. Same
   split, and the same reasoning, as the lightswitch puzzle's cones. */
#define RBE_POOL_LEVEL       210
#define RBE_SHAFT_LEVEL       60

/* ---- Death lights ----------------------------------------------------------
   Four of them, hung on the model's own anchors (RBS_A_* in rabisu.h). Each is
   drawn as three concentric additive squares — wide and dim, then tighter and
   brighter — which is a cheap, entirely convincing glow and needs no texture,
   no VRAM and no sorting among themselves. */
#define RBE_GLOW_WORLD       230   /* world half-size of the outermost square */
#define RBE_GLOW_RINGS         3
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
static int32_t  pulse_t   = 0;     /* free-running, so the colour never jumps */
static int32_t  light_master = 0;  /* 0..256 over the whole lamp bank         */

/* Where the player was standing when the director took over, and what the
   camera was doing. Restored on the way out. */
static int32_t save_cx, save_cy, save_cz, save_crot, save_cvy;

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

static int32_t rbe_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x = v, last;
    if (x > 1 << 16) x = 1 << 16;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
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
    pulse_t      = 0;
    light_master = 0;
    camera_release_player();
    cam_pitch = 0;
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
    Rabisu *r = rabisu_boss_instance();

    pulse_t++;

    if (state == RBE_IDLE) {
        /* Arm on the first frame a living boss is in the room with us. */
        if (r) begin_reveal(r);
        return;
    }
    if (state == RBE_DONE) return;

    /* The boss vanished out from under the script (a debug jump, a load). Bail
       out cleanly rather than driving a NULL through the rest of this. */
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
            enter_phase(RBE_LINE1);
        }
        break;

    case RBE_LINE1:
        if (phase_t >= RBE_T_LINE) enter_phase(RBE_LINE2);
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

/* ---- Drawing: the beams ----------------------------------------------------
   One additive flat quad, lifted wholesale from the lightswitch puzzle's
   ls_quad and for the same reasons: additive (ABR=1) so overlapping beams mix
   the way real light does and need no sorting among themselves, no backface
   cull because a beam is a volume rather than a surface, and the DR_TPAGE added
   to the same OT bucket immediately AFTER the poly — the OT is LIFO, so "after"
   is what puts it in front. */
static void rbe_quad(RenderContext *ctx, const SVECTOR v[4],
                     uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) return;

    DVECTOR sv[4];
    int32_t sz[4], otz;
    int k;

    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);
    gte_stsz4c(sz);
    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);
    gte_stsz(&sz[3]);
    if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) return;

    for (k = 0; k < 4; k++)
        if (sv[k].vx <= -1023 || sv[k].vx >= 1023 ||
            sv[k].vy <= -1023 || sv[k].vy >= 1023) return;

    gte_avsz4();
    gte_stotz(&otz);
    if (otz <= SCENE_OT_MIN) return;
    otz += 40;   /* the room mesh's own bias, so a wall in front still occludes */
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;

    POLY_F4 *p = (POLY_F4 *)ctx->next_packet;
    setPolyF4(p);
    setSemiTrans(p, 1);
    setRGB0(p, r, g, b);
    p->x0 = sv[0].vx; p->y0 = sv[0].vy;
    p->x1 = sv[1].vx; p->y1 = sv[1].vy;
    p->x2 = sv[2].vx; p->y2 = sv[2].vy;
    p->x3 = sv[3].vx; p->y3 = sv[3].vy;
    addPrim(&ot[otz], p);
    ctx->next_packet += sizeof(POLY_F4);

    DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
    addPrim(&ot[otz], tp);
    ctx->next_packet += sizeof(DR_TPAGE);
}

/* The pulse colour at a given clock, already scaled to a 0..256 brightness.
   Fading an additive poly means scaling it toward BLACK — there is nothing to
   blend toward, so the usual fog-colour lerp does not apply here. */
static void rbe_pulse(int32_t t, int32_t level, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (t < 0) t = 0;
    int32_t leg = (t / RBE_PULSE_STEP) % 3;
    int32_t f   = ((t % RBE_PULSE_STEP) * 256) / RBE_PULSE_STEP;
    const uint8_t *a = PULSE[leg];
    const uint8_t *c = PULSE[(leg + 1) % 3];
    int32_t rr = (a[0] * (256 - f) + c[0] * f) >> 8;
    int32_t gg = (a[1] * (256 - f) + c[1] * f) >> 8;
    int32_t bb = (a[2] * (256 - f) + c[2] * f) >> 8;
    *r = (uint8_t)((rr * level) >> 8);
    *g = (uint8_t)((gg * level) >> 8);
    *b = (uint8_t)((bb * level) >> 8);
}

/* One lawn light: a pool covering its grass quad, and a shaft tapering upward
   out of it. The shaft is banded so no single quad is large enough to blow past
   the GTE's +/-1023 screen clamp if the player ever ends up standing in one. */
static void draw_lawn_light(RenderContext *ctx, int cell, int32_t bright) {
    int col = cell & 3, row = cell >> 2;
    int32_t x0 = LAWN_X[col], x1 = LAWN_X[col + 1];
    int32_t z0 = LAWN_Z[row], z1 = LAWN_Z[row + 1];
    /* Each light runs its own clock, offset by cell, so the sixteen shimmer out
       of step instead of strobing the whole patch as one lamp. */
    int32_t clock = pulse_t + cell * 9;

    uint8_t r, g, b;
    SVECTOR v[4];
    int k;
    for (k = 0; k < 4; k++) v[k].pad = 0;

    /* --- The pool, 4 above the turf so it never z-fights the grass poly --- */
    rbe_pulse(clock, (bright * RBE_POOL_LEVEL) >> 8, &r, &g, &b);
    v[0].vx = (int16_t)x0; v[0].vy = RBE_LAWN_Y - 4; v[0].vz = (int16_t)z0;
    v[1].vx = (int16_t)x1; v[1].vy = RBE_LAWN_Y - 4; v[1].vz = (int16_t)z0;
    v[2].vx = (int16_t)x0; v[2].vy = RBE_LAWN_Y - 4; v[2].vz = (int16_t)z1;
    v[3].vx = (int16_t)x1; v[3].vy = RBE_LAWN_Y - 4; v[3].vz = (int16_t)z1;
    rbe_quad(ctx, v, r, g, b);

    /* --- The shaft: four walls, drawn from both sides, FLARING as it rises so
           the beam widens out of the poly it is pouring from. Banded along its
           length both so no single quad can blow past the GTE's +/-1023 screen
           clamp and so the brightness can fall off with height — a beam that
           is uniformly lit top to bottom looks like a solid box, and the whole
           point is that it is brightest where it leaves the ground. --- */
    {
        /* Corners in a ring, so consecutive pairs share an edge. */
        const int32_t cx[4] = { x0, x1, x1, x0 };
        const int32_t cz[4] = { z0, z0, z1, z1 };
        /* The cell's centre: corners are pushed directly away from it. */
        const int32_t mx = (x0 + x1) / 2, mz = (z0 + z1) / 2;
        int s, t;
        for (t = 0; t < RBE_SHAFT_SEGS; t++) {
            int32_t e0 = t, e1 = t + 1;
            int32_t y0 = RBE_LAWN_Y - (RBE_SHAFT_H * e0) / RBE_SHAFT_SEGS;
            int32_t y1 = RBE_LAWN_Y - (RBE_SHAFT_H * e1) / RBE_SHAFT_SEGS;
            int32_t o0 = (RBE_SHAFT_FLARE * e0) / RBE_SHAFT_SEGS;
            int32_t o1 = (RBE_SHAFT_FLARE * e1) / RBE_SHAFT_SEGS;

            /* Thins out toward the top: full level at the turf, a third of it
               at the last band. */
            int32_t drop = 256 - (170 * e0) / RBE_SHAFT_SEGS;
            rbe_pulse(clock, (((bright * RBE_SHAFT_LEVEL) >> 8) * drop) >> 8,
                      &r, &g, &b);

            for (s = 0; s < 4; s++) {
                int n = (s + 1) & 3;
                /* Away from the centre, not toward it — this sign IS the fix. */
                #define PUSHX(c, o) ((c) + ((mx - (c)) > 0 ? -(o) : (o)))
                #define PUSHZ(c, o) ((c) + ((mz - (c)) > 0 ? -(o) : (o)))
                v[0].vx = (int16_t)PUSHX(cx[s], o0); v[0].vz = (int16_t)PUSHZ(cz[s], o0); v[0].vy = (int16_t)y0;
                v[1].vx = (int16_t)PUSHX(cx[n], o0); v[1].vz = (int16_t)PUSHZ(cz[n], o0); v[1].vy = (int16_t)y0;
                v[2].vx = (int16_t)PUSHX(cx[s], o1); v[2].vz = (int16_t)PUSHZ(cz[s], o1); v[2].vy = (int16_t)y1;
                v[3].vx = (int16_t)PUSHX(cx[n], o1); v[3].vz = (int16_t)PUSHZ(cz[n], o1); v[3].vy = (int16_t)y1;
                #undef PUSHX
                #undef PUSHZ
                rbe_quad(ctx, v, r, g, b);
            }
        }
    }
}

/* ---- Drawing: the death lights ---------------------------------------------
   A glow hung on one of the model's anchors. Projected as a single point and
   drawn as concentric SCREEN-space squares, the way the health bar is anchored
   (draw_rbs_bar): a model has no billboard quad to hang anything off, so the
   point is projected explicitly and the sprite built around it in 2D. Sizing it
   off the world distance rather than the projected SZ keeps the maths obvious
   and keeps the glow the same physical size wherever the shot is set. */
static void draw_death_glow(RenderContext *ctx, const VECTOR *at, int32_t bright,
                            int32_t clock) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    SVECTOR pt;
    pt.vx = (int16_t)at->vx; pt.vy = (int16_t)at->vy; pt.vz = (int16_t)at->vz;
    pt.pad = 0;

    DVECTOR sv;
    int32_t sz;
    gte_ldv0(&pt);
    gte_rtps();
    gte_stsxy(&sv);
    gte_stsz(&sz);
    if (sz == 0) return;
    if (sv.vx <= -1023 || sv.vx >= 1023 || sv.vy <= -1023 || sv.vy >= 1023) return;

    int32_t dx = at->vx - cam_x, dy = at->vy - cam_y, dz = at->vz - cam_z;
    int32_t dist = rbe_isqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 64) dist = 64;

    int32_t otz = sz >> 2;
    if (otz <= SCENE_OT_MIN) otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;

    int ring;
    for (ring = 0; ring < RBE_GLOW_RINGS; ring++) {
        if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) return;

        /* Outermost ring is full width and dimmest; each step in halves the
           square and doubles the level, so the three add up to a soft falloff
           with a hot core. */
        int32_t world_half = RBE_GLOW_WORLD >> ring;
        int32_t half = (world_half * 256) / dist;   /* gte_SetGeomScreen(256) */
        if (half < 1) half = 1;
        if (half > 400) half = 400;
        int32_t level = (bright * (60 + ring * 70)) >> 8;

        uint8_t r, g, b;
        rbe_pulse(clock + ring * 5, level, &r, &g, &b);

        POLY_F4 *p = (POLY_F4 *)ctx->next_packet;
        setPolyF4(p);
        setSemiTrans(p, 1);
        setRGB0(p, r, g, b);
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
        /* Still findable: RBE_D_FADE does not set `dead` until the frame it
           ends on, and by then the state has already moved past this block. */
        Rabisu *r = rabisu_boss_instance();
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
                    draw_death_glow(ctx, &w, bright, pulse_t + a * 13);
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
