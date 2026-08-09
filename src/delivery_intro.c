#include <stdint.h>
#include <psxgpu.h>
#include "render.h"
#include "camera.h"
#include "collision.h"      /* GROUND_FLOOR_Y */
#include "sound.h"          /* SFX_STEP1 / SFX_STEP2 / SFX_HURT */
#include "delivery_intro.h"

/* The Delivery Area's arrival sequence. See delivery_intro.h for the beat sheet
   and for why this is a camera move rather than an animation. */

/* ---- The ground this happens on --------------------------------------------
   MINED FROM THE MESH, NOT EYEBALLED. Every number below was read out of
   assets/Delivery Area v2.smx (the 1150-primitive mesh delivery_area.smd is
   built from) and out of src/collision.c's floor zones. If the yard is
   remodelled, re-derive them with:

     py -c "import xml.etree.ElementTree as ET; \
       r=ET.parse('assets/Delivery Area v2.smx').getroot(); \
       t=[x.get('file') for x in r.find('textures').findall('texture')]; \
       v=[(float(a.get('x')),float(a.get('y')),float(a.get('z'))) \
          for a in r.find('vertices').findall('v')]; \
       [print(t[int(p.get('texture'))], \
              [round(c) for c in (min(q[0] for q in qq),max(q[0] for q in qq), \
                                  min(q[2] for q in qq),max(q[2] for q in qq))]) \
        for p,qq in ((p,[v[int(p.get('v%d'%k))] for k in range(4) \
                         if p.get('v%d'%k) is not None]) \
                     for p in r.find('primitives').findall('poly') \
                     if p.get('texture') is not None) \
        if max(q[0] for q in qq) > 1810]"

   THE GRASS OUTSIDE THE YARD is the only patch of ground in the whole room that
   sits outside the collision perimeter (x -1800..1800, z -1800..1800): twelve
   grss quads at x 1862..2674, z -406..406, surface y = 150 — the same height as
   the yard floor. It is due EAST of the fence the player starts against, which
   is what "the opposite side of the fence to where we normally start" picks
   out; there is no second candidate anywhere in the mesh.

   THE FENCE between the two is two rusty_fence faces, at x = 1800 (the yard
   side, and collision wall 3) and x = 1862 (the outer side), capped across the
   top. Both run y -150..150, so with the ground at 150 it stands 300 tall and
   its top edge is at y = -150 (-Y is up).

   THE LANDING SPOT is reset_game()'s spawn, verbatim: x 1600, z 0, facing west
   across the yard. Handing control back anywhere else would mean the game
   starts somewhere the rest of the code does not think it starts.            */

#define DIA_Z                 0     /* the whole move is on the z=0 centre line */
#define DIA_ROT            3072     /* facing west (-X): reset_game()'s facing   */

/* Eye height for the yard's y=150 floor: the same derivation apply_height uses
   (floor surface - GROUND_FLOOR_Y, less the 40-unit float standoff). The grass
   outside is at the same surface height, so the whole walk is level and the
   camera arrives at exactly the height free play will hold it at. */
#define DIA_EYE_Y     (150 - GROUND_FLOOR_Y - 40)

#define DIA_START_X        2500     /* on the outer grass, 174 in from its east
                                       edge at 2674                             */
/* TAKE-OFF. >>> THIS IS A NEAR-CLIP CONSTANT, NOT A TASTE ONE. <<< It was 1990
   first, 128 clear of the fence's outer face at 1862, and the walk ended with
   the fence close enough that its polys clipped out against the near plane —
   the wall the whole sequence is about tearing open right before the jump. 288
   of clearance holds it whole, and it puts the arc's apex (the midpoint of the
   leap, x 1875) directly OVER the fence rather than past it, which is where the
   vault reads best anyway. Do not walk this in without checking the fence from
   a standstill at the take-off point. */
#define DIA_LEAP_X         2150
#define DIA_LAND_X         1600     /* reset_game()'s spawn                     */

/* The eye has to clear the fence, not merely reach it: the top edge is at
   y = -150 and the walking eye is at DIA_EYE_Y (-39), so the fence stands 111
   above eye level. 330 of rise puts the apex 219 clear of the top — enough that
   the fence passes visibly UNDER the camera at the top of the arc instead of
   grazing it. */
#define DIA_JUMP_RISE       330

/* ---- Timing, in frames at 60 fps ------------------------------------------- */
#define DIA_T_FADE           60     /* 1.0 s up out of the intro's black, stood
                                       still: the establishing beat             */
#define DIA_T_WALK           62     /* 1.03 s to cross the 350 units to the
                                       take-off — 5.6 units a frame, about half
                                       walking pace, which is the difference
                                       between approaching and marching. Sized
                                       FROM the pace: shortening the walk (the
                                       start moved forward, the take-off back)
                                       has to shorten the time with it or the
                                       camera creeps.                            */
#define DIA_T_CROUCH         14     /* 0.23 s knee-bend before the leap         */
#define DIA_T_JUMP           64     /* 1.07 s in the air. The leap is 550 units
                                       now, so this holds the same 8.6-a-frame
                                       horizontal speed the shorter jump had —
                                       the arc is longer, not slower.            */
#define DIA_T_LAND           26     /* 0.43 s of knee-bend on the far side      */

/* Footstep cadence. update_camera plays a step every STEP_STRIDE (280) units
   walked, which at the normal pace of 12 a frame is one every ~23 frames — so
   22 here is the game's own gait, and the sequence sounds like the player walks
   rather than like a metronome. Five steps fall inside DIA_T_WALK. */
#define DIA_STEP_EVERY       22

#define DIA_WALK_BOB          7     /* head bob, peak-to-floor, synced to the
                                       stride so the steps land on the low point */
#define DIA_CROUCH_DIP       26     /* how far the knee-bend drops the eye      */
#define DIA_LAND_DIP         66     /* ...and the landing, which is deeper      */
#define DIA_LAND_DOWN         8     /* frames of the landing spent going DOWN;
                                       the rest is the ease back up             */

/* ---- State ----------------------------------------------------------------- */
typedef enum {
    DIA_IDLE = 0,   /* never armed, or long since finished */
    DIA_FADE,
    DIA_WALK,
    DIA_CROUCH,
    DIA_JUMP,
    DIA_LAND,
    DIA_DONE
} DiaState;

static DiaState state   = DIA_IDLE;
static int32_t  phase_t = 0;   /* frames elapsed IN THE CURRENT PHASE */
static int32_t  fade_t  = 0;   /* frames elapsed since the sequence armed; the
                                  fade has its own counter because it is a
                                  property of the whole sequence, not of a phase */
static int      step_foot = 0; /* alternates STEP1/STEP2, as update_camera does */

static void enter_phase(DiaState s) {
    state   = s;
    phase_t = 0;
}

/* A symmetric 0 -> peak -> 0 hump over a phase, with `u` the phase position in
   256ths. 4u(256-u) peaks at exactly 256*256, so the divide gives back `peak`
   at the midpoint and nothing at either end. Used for the walk bob's stride,
   the crouch and the jump arc — one curve, so they cannot disagree about where
   "the middle" is. Worst case here is 330 * 65536, well inside an int32. */
static int32_t hump(int32_t u, int32_t peak) {
    return (peak * (4 * u * (256 - u))) / (256 * 256);
}

/* ---- Public ---------------------------------------------------------------- */

int delivery_intro_active(void) {
    return state != DIA_IDLE && state != DIA_DONE;
}

void delivery_intro_reset(void) {
    state   = DIA_IDLE;
    phase_t = 0;
    fade_t  = 0;
    camera_release_player();
    cam_pitch = 0;
}

void delivery_intro_start(void) {
    state     = DIA_FADE;
    phase_t   = 0;
    fade_t    = 0;
    step_foot = 0;

    cam_x     = DIA_START_X;
    cam_y     = DIA_EYE_Y;
    cam_z     = DIA_Z;
    cam_vy    = 0;
    cam_rot   = DIA_ROT;
    cam_pitch = 0;          /* the whole move is level; nothing tilts the view */

    /* ANCHOR THE PLAYER AT THE DESTINATION, not at the camera. The camera spends
       the next four seconds outside the level, and anything that means "where
       the player is standing" (camera.h) must read the spot the game actually
       starts at rather than a patch of grass over the fence. Releasing the
       anchor at the end is what hands control over — by then the camera is
       standing on that same spot. */
    camera_anchor_player(DIA_LAND_X, DIA_EYE_Y, DIA_Z);
}

void delivery_intro_update(void) {
    int32_t t, u;

    if (!delivery_intro_active()) return;

    fade_t++;
    t = phase_t;

    switch (state) {

    case DIA_FADE:
        /* Stood still on the grass while the screen comes up. */
        if (++phase_t >= DIA_T_FADE) {
            enter_phase(DIA_WALK);
            sound_play(SFX_STEP1);      /* the first stride starts the walk */
            step_foot = 1;
        }
        break;

    case DIA_WALK: {
        /* Linear: a walk holds its pace, and the footsteps are only honest if
           the ground moves under them at a constant rate. */
        int32_t stride = t % DIA_STEP_EVERY;
        cam_x = DIA_START_X + ((DIA_LEAP_X - DIA_START_X) * t) / DIA_T_WALK;
        /* Bob: one hump per stride. The eye is at its LOWEST — level, the height
           it will be handed over at — on each footfall, and rises between them,
           so the gait is tied to the sound rather than free-running against it. */
        cam_y = DIA_EYE_Y - hump((stride * 256) / DIA_STEP_EVERY, DIA_WALK_BOB);

        if (++phase_t >= DIA_T_WALK) {
            cam_x = DIA_LEAP_X;         /* land the phase exactly on its end
                                           point rather than one frame short */
            enter_phase(DIA_CROUCH);
        } else if (phase_t % DIA_STEP_EVERY == 0) {
            sound_play(step_foot ? SFX_STEP2 : SFX_STEP1);
            step_foot ^= 1;
        }
        break;
    }

    case DIA_CROUCH:
        /* Anticipation. Down and back up, ending level, so the jump arc below
           starts from the same height the walk finished at. */
        u     = (t * 256) / DIA_T_CROUCH;
        cam_x = DIA_LEAP_X;
        cam_y = DIA_EYE_Y + hump(u, DIA_CROUCH_DIP);
        if (++phase_t >= DIA_T_CROUCH) {
            cam_y = DIA_EYE_Y;
            enter_phase(DIA_JUMP);
        }
        break;

    case DIA_JUMP:
        /* Ballistic: constant horizontal speed, symmetric arc. NOT eased — an
           ease-out on the X would read as the camera being lowered onto the
           yard on a wire rather than thrown over the fence. */
        u     = (t * 256) / DIA_T_JUMP;
        cam_x = DIA_LEAP_X + ((DIA_LAND_X - DIA_LEAP_X) * u) / 256;
        cam_y = DIA_EYE_Y - hump(u, DIA_JUMP_RISE);
        if (++phase_t >= DIA_T_JUMP) {
            cam_x = DIA_LAND_X;
            cam_y = DIA_EYE_Y;
            enter_phase(DIA_LAND);
            sound_play(SFX_HURT);       /* the impact, as briefed. No damage is
                                           dealt: it is the sound only */
        }
        break;

    case DIA_LAND:
        /* Absorb it: eight frames down, then ease back up. The dip is deeper
           and slower coming back than the crouch, which is what separates
           "landed" from "hopped". */
        cam_x = DIA_LAND_X;
        if (t <= DIA_LAND_DOWN) {
            cam_y = DIA_EYE_Y + (DIA_LAND_DIP * t) / DIA_LAND_DOWN;
        } else {
            int32_t p   = ((t - DIA_LAND_DOWN) * 256) / (DIA_T_LAND - DIA_LAND_DOWN);
            int32_t inv = 256 - p;
            int32_t e   = 256 - (inv * inv) / 256;      /* ease-out */
            cam_y = DIA_EYE_Y + (DIA_LAND_DIP * (256 - e)) / 256;
        }
        if (++phase_t >= DIA_T_LAND) {
            /* HAND OVER. The camera is standing on reset_game()'s spawn at its
               proper eye height, so releasing the anchor makes player_x/y/z()
               read it again and the player is simply THERE — the same handover
               the Rabisu's fight spot uses (ADDING_A_BOSS_ENCOUNTER.txt STEP 3).
               Restore everything free play assumes: level eye, no pitch, no
               inherited fall velocity. */
            cam_x     = DIA_LAND_X;
            cam_y     = DIA_EYE_Y;
            cam_z     = DIA_Z;
            cam_rot   = DIA_ROT;
            cam_vy    = 0;
            cam_pitch = 0;
            camera_release_player();
            state   = DIA_DONE;
            phase_t = 0;
        }
        break;

    default:
        break;
    }

    cam_z  = DIA_Z;
    cam_vy = 0;   /* nothing here integrates gravity; do not leave it a velocity
                     to apply on the first frame of free play */
}

/* ---- The fade --------------------------------------------------------------
   A fade FROM BLACK over a live 3D scene, which is not the same problem as the
   intro's white flash over a flat background. There is no alpha on the PS1: the
   only way to darken what is already in the frame buffer is SUBTRACTIVE
   blending (ABR = 2, "background minus foreground"), so this is a full-screen
   TILE whose grey level counts down from 255 and is subtracted off the scene.
   At 255 the result is black over any picture at all; at 0 nothing is drawn.

   The DR_TPAGE selecting ABR=2 is added AFTER the tile because an OT node is
   LIFO and the GPU has to see the blend mode first — the same ordering, and the
   same reason, as the muzzle flash in graveolver.c and the flash in intro.c.

   OT bucket 0 is the front-most bucket (higher indices draw first), which puts
   this over the room, the props and the sprites. Nothing contends for it: the
   HUD and menus own 0..15 and both are suppressed while this runs.           */
#define DIA_OT_FADE 0

void delivery_intro_draw(RenderContext *ctx) {
    uint8_t *buf_end;
    int32_t  v;

    if (!delivery_intro_active()) return;
    if (fade_t >= DIA_T_FADE)     return;

    v = 255 - (fade_t * 255) / DIA_T_FADE;
    if (v <= 0) return;

    buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) + sizeof(DR_TPAGE) > buf_end) return;

    {
        TILE *tl = (TILE *)ctx->next_packet;
        setTile(tl);
        setSemiTrans(tl, 1);
        setRGB0(tl, (uint8_t)v, (uint8_t)v, (uint8_t)v);
        setXY0(tl, 0, 0);
        setWH(tl, SCREEN_XRES, SCREEN_YRES);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[DIA_OT_FADE], tl);
        ctx->next_packet += sizeof(TILE);
    }
    {
        DR_TPAGE *dp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(dp, 0, 0, getTPage(0, 2 /* ABR=2: B - F */, 0, 0));
        addPrim(&ctx->buffers[ctx->active_buffer].ot[DIA_OT_FADE], dp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
}
