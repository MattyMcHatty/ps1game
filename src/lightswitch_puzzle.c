#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "collision.h"        /* GROUND_FLOOR_Y */
#include "player.h"           /* game_flag, show_pickup_msg_raw */
#include "sound.h"            /* SFX_UNLOCK: the clunk of a lever being thrown */
#include "btn_glyph.h"        /* BTN_CIRCLE */
#include "door.h"             /* door_draw_string_3d for the lever prompts */
#include "title.h"            /* STATE_ATTIC_EXIT */
#include "lever.h"
#include "chainlink_door.h"
#include "lightswitch_puzzle.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- The fixtures, read straight off "Attic Exit.smx" -----------------------
   Four levers, one per corner, in the room's four brown wall boxes; four white
   20x20x20 cubes floating just under the ceiling, one per corner. Both are
   indexed by QUADRANT, and the pairing is index-for-index — north-west lever to
   north-west light, and so on. +Z is north, +X is east.

   Lever y is the standing reference (world y = y + GROUND_FLOOR_Y). Z is NOT the
   wall box's centre: the lever is 150 long against a box only 40 deep, so it is
   pushed along Z until its blue cap meets the wall (see attic_exit.c). Local +Z
   is the blue end, so the north pair sit unrotated and the south pair are turned
   180deg.

   Cube values are world coordinates: the mesh puts all four at y[-550,-530],
   so -540 is the centre and -530 the bottom face the beam leaves from. */
#define LS_COUNT 4

static const struct {
    int32_t lx, ly, lz, lrot;   /* the lever          */
    int32_t cx, cz;             /* the cube's centre in XZ (y is LS_CUBE_Y) */
    int32_t text_z;             /* prompt plane: 80 off the wall, over the lever */
    int     mirror;             /* 1 = the wall is read from its +Z side     */
} FIXTURE[LS_COUNT] = {
    /* NW */ { -1052, -354,  925,    0,  -525,  -33,  920, 0 },
    /* NE */ {  1129, -354,  923,    0,   525,  -33,  920, 0 },
    /* SW */ { -1051, -355, -925, 2048,  -525, -984, -920, 1 },
    /* SE */ {  1111, -355, -925, 2048,   525, -984, -920, 1 },
};

#define LS_CUBE_Y      (-540)   /* cube centre, world */
#define LS_CUBE_BOT    (-530)   /* cube bottom face: where the beam starts */
#define LS_CUBE_HALF     10

/* The purple star on the floor in front of the cage gate (four quads meeting at
   (0,0,-245); the group's bounds are what every cone widens to cover, so the
   four colours overlap and add over the whole figure). Held 3 above the boards
   so the pool never z-fights the painted polys under it. */
#define LS_POOL_X0     (-300)
#define LS_POOL_X1       300
#define LS_POOL_Z0     (-500)
#define LS_POOL_Z1         0
#define LS_POOL_Y        (-3)

/* ---- Timing ---------------------------------------------------------------- */
#define LS_THROW_FRAMES   30    /* half a second, as specified */
#define LS_THROW_PITCH   512    /* 45deg of 4096: the tip swings down that far */
#define LS_SOLVE_HOLD     30    /* beat on the opened cage before cutting back  */

/* ---- Interaction ------------------------------------------------------------
   Manhattan, measured to the lever. The 195 wall standoff parks the player about
   120 from the nearest lever, and the two levers sharing a wall are ~2100 apart,
   so there is no ambiguity about which one a press means. */
#define LS_TRIGGER_RADIUS     500
#define LS_TEXT_RADIUS        900
#define LS_FADE_NEAR          600
#define LS_TEXT_PIXEL           3    /* "Press O to deactivate" is 21 chars: 378 wide */
#define LS_TEXT_Y            (-300)  /* glyph TOP; 7 rows of 3 end 66 above the lever,
                                        so the line hovers clear of the shaft */

/* ---- The payoff shot --------------------------------------------------------
   A hard cut (not a glide) to the spot just inside the room's entry door — the
   same x the player arrives on, 200 north of the south wall — looking up the
   room at the cage gate. From the south-east lever the gate was almost edge-on
   and mostly behind the cage's own side panel; from here the shot is straight
   down the room's spine, with the lit star in the foreground.

   Yaw is atan2(dx,dz) onto the gate's centre at (0,-253,15): dx=+400, dz=+815,
   i.e. 26.1deg of 4096. Pitch stays 0 — the gate's centre is only 13 off the
   camera's own height, and at 908 away the ~50deg vertical field spans
   y[-669,189], so the floor, the star and the ceiling the gate vanishes into are
   all in frame without tilting. Move LS_CAM_X/Z and the yaw has to be
   re-derived. */
#define LS_CAM_X        (-400)
#define LS_CAM_Y        (-240)
#define LS_CAM_Z        (-800)
#define LS_CAM_ROT        297

/* ---- Colours ----------------------------------------------------------------
   Dealt to the four lights at random on entry. The order here is the order the
   solution is stated in: the first three must be lit and GREEN must not. */
typedef enum {
    LC_YELLOW = 0,
    LC_BLUE,
    LC_MAGENTA,
    LC_GREEN,
    LC_COUNT
} LightColour;

#define LS_SOLUTION_MASK ((1 << LC_YELLOW) | (1 << LC_BLUE) | (1 << LC_MAGENTA))

static const uint8_t COLOUR_RGB[LC_COUNT][3] = {
    { 255, 230,  40 },   /* yellow  */
    {  40,  80, 255 },   /* blue    */
    { 255,  40, 220 },   /* magenta */
    {  40, 255,  60 },   /* green   */
};

/* Additive brightness of each part of a cone, as a 0..256 scale of the colour
   above. The shaft is faint so four overlapping beams do not white out the
   middle of the room; the pool on the floor is where the colour has to read. */
#define LS_SHAFT_LEVEL    70
#define LS_POOL_LEVEL    150
#define LS_CAP_LEVEL     220

/* ---- State ----------------------------------------------------------------- */
typedef enum {
    LS_IDLE = 0,   /* free play: the levers are live                */
    LS_CUT,        /* solved: fixed shot, the gate winching upwards */
    LS_HOLD        /* gate gone: a beat, then the camera goes back  */
} LsState;

static LsState state = LS_IDLE;

static uint8_t colour_of[LS_COUNT];   /* light index -> LightColour       */
static uint8_t lit[LS_COUNT];         /* 1 = the player has thrown it     */
static uint8_t anim[LS_COUNT];        /* frames into the throw, 0..LS_THROW_FRAMES */

static int32_t save_cx, save_cy, save_cz, save_crot, save_cvy;
static int     hold_timer  = 0;
static int     interact_prev = 1;     /* Circle edge-detect */

/* Self-contained LCG, as the Anzu puzzle uses — there is no global PRNG in this
   project. Stirred every frame the player spends in the room, so the deal really
   does differ from entry to entry instead of repeating a fixed hand. */
static uint32_t rng = 0x1F123BB5u;
static uint32_t ls_rand(void) {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 16;
}

int lightswitch_puzzle_active(void) { return state != LS_IDLE; }

static int solved(void) { return game_flag(FLAG_LIGHTS_SOLVED); }

/* 0..256 across the throw: drives the lever's pitch AND the light's brightness
   from one number, so the beam fades up exactly as the lever swings down. */
static int32_t level(int i) {
    return ((int32_t)anim[i] * 256) / LS_THROW_FRAMES;
}

static void push_pitch(int i) {
    lever_set_pitch(i, (LS_THROW_PITCH * level(i)) / 256);
}

/* Which colours are currently on. */
static int lit_mask(void) {
    int i, m = 0;
    for (i = 0; i < LS_COUNT; i++) if (lit[i]) m |= 1 << colour_of[i];
    return m;
}

void lightswitch_place(void) {
    int i;

    /* Stir in the free-running vblank counter. Walking around the room stirs the
       LCG for RE-entries, but the FIRST entry would otherwise always deal from
       the fixed seed; the vblank count depends on how long the player took to get
       here, so the opening hand varies too. */
    rng ^= (uint32_t)VSync(-1) * 2654435761u;
    (void)ls_rand();

    /* Fisher-Yates over the four colours: every light gets a different one. */
    for (i = 0; i < LS_COUNT; i++) colour_of[i] = (uint8_t)i;
    for (i = LS_COUNT - 1; i > 0; i--) {
        int j = (int)(ls_rand() % (uint32_t)(i + 1));
        uint8_t t = colour_of[i]; colour_of[i] = colour_of[j]; colour_of[j] = t;
    }

    for (i = 0; i < LS_COUNT; i++) {
        lever_place(STATE_ATTIC_EXIT, FIXTURE[i].lx, FIXTURE[i].ly,
                    FIXTURE[i].lz, FIXTURE[i].lrot);
        lit[i]  = 0;
        anim[i] = 0;
    }

    /* Already solved: install the winning set so a returning player finds the
       room as they left it — three lights burning, the green one dark, three
       levers thrown. The random deal above still ran, so which CORNER holds
       which colour varies; only the pattern of lit colours is fixed. */
    if (solved()) {
        for (i = 0; i < LS_COUNT; i++) {
            lit[i]  = (LS_SOLUTION_MASK & (1 << colour_of[i])) ? 1 : 0;
            anim[i] = lit[i] ? LS_THROW_FRAMES : 0;
        }
    }
    for (i = 0; i < LS_COUNT; i++) push_pitch(i);

    state      = LS_IDLE;
    hold_timer = 0;

    /* Don't treat a Circle held through the door transition as an interact. */
    {
        int held = interact_tapped();
        interact_prev = held;
    }
}

/* ---- Update ---------------------------------------------------------------- */

/* Index of the lever the player is standing at, or -1. */
static int lever_in_reach(void) {
    int i, best = -1;
    int32_t best_d = LS_TRIGGER_RADIUS;
    for (i = 0; i < LS_COUNT; i++) {
        int32_t dx = cam_x - FIXTURE[i].lx;
        int32_t dz = cam_z - FIXTURE[i].lz;
        int32_t d  = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (d >= best_d) continue;
        /* Has to be in view as well as in range — a lever behind you is not
           one you can throw. */
        if (!interact_facing(FIXTURE[i].lx, FIXTURE[i].lz)) continue;
        best_d = d; best = i;
    }
    return best;
}

static void start_cutscene(void) {
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    /* The player stays at the lever they just threw while the camera cuts away,
       so anything hunting them keeps hunting the real spot. */
    camera_anchor_player(save_cx, save_cy, save_cz);

    cam_x = LS_CAM_X; cam_y = LS_CAM_Y; cam_z = LS_CAM_Z;
    cam_rot = LS_CAM_ROT; cam_pitch = 0; cam_vy = 0;

    game_flag_set(FLAG_LIGHTS_SOLVED);
    chainlink_door_raise_start();
    show_pickup_msg_raw("Something heavy begins to lift");
    state = LS_CUT;
}

static void end_cutscene(void) {
    state   = LS_IDLE;
    cam_x   = save_cx; cam_y = save_cy; cam_z = save_cz;
    cam_rot = save_crot; cam_vy = save_cvy;
    cam_pitch = 0;
    camera_release_player();
    interact_prev = 1;   /* swallow the Circle that is probably still held */
}

void lightswitch_update(void) {
    int i;
    uint16_t btn = 0;
    if (pad_buff_len[0]) { PadResponse *pad = (PadResponse *)pad_buff[0]; btn = ~pad->btn; }

    if (state == LS_CUT) {
        if (chainlink_doors_raise_update()) {
            state      = LS_HOLD;
            hold_timer = LS_SOLVE_HOLD;
        }
        return;
    }
    if (state == LS_HOLD) {
        if (--hold_timer <= 0) end_cutscene();
        return;
    }

    rng += 0x6D2B79F5u;   /* stir while the player walks around the room */

    /* Advance every lever that is mid-throw. A switch completing its travel is
       also when the wiring is re-checked: that way the player watches the last
       lever land and its light come up before the camera cuts. */
    int settled = -1;
    for (i = 0; i < LS_COUNT; i++) {
        uint8_t want = lit[i] ? LS_THROW_FRAMES : 0;
        if (anim[i] == want) continue;
        anim[i] = lit[i] ? (uint8_t)(anim[i] + 1) : (uint8_t)(anim[i] - 1);
        push_pitch(i);
        if (anim[i] == want) settled = i;
    }

    /* No unlock cue here: every throw already clunks below, and start_cutscene
       fires the machinery the same frame. */
    if (settled >= 0 && !solved() && lit_mask() == LS_SOLUTION_MASK) {
        start_cutscene();
        return;
    }

    /* Throw the lever the player is standing at. A lever mid-travel is ignored
       so a mashed Circle cannot leave one stuck between states. */
    {
        int held = interact_tapped();
        int just = held && !interact_prev;
        interact_prev = held;
        if (!just || solved()) return;

        i = lever_in_reach();
        if (i < 0) return;
        if (anim[i] != (lit[i] ? LS_THROW_FRAMES : 0)) return;   /* still moving */
        lit[i] = (uint8_t)!lit[i];
        sound_play(SFX_UNLOCK);   /* the clunk of the switch, either direction */
    }
}

/* ---- Drawing ---------------------------------------------------------------
   A cone is an untextured pyramid frustum: a small cap over the cube's bottom
   face, four shaft walls widening down to the floor, and a pool covering the
   purple star. Everything is drawn with ADDITIVE semi-transparency (ABR=1), the
   same blend the stove's gas flame uses, so overlapping beams mix their colours
   the way real light does and need no sorting among themselves.

   Additive means a DR_TPAGE has to be in front of each semi-transparent poly in
   OT order. Adding the TPAGE to the SAME bucket immediately AFTER its quad puts
   it there — the OT is LIFO, so the last thing added to a bucket is the first
   the GPU processes.

   Nothing here is backface culled: a light shaft is a volume, not a surface, and
   drawing both walls of it is what makes the beam read as solid from any angle.
   The shafts are cut into LS_CONE_SEGS bands along their length so no single
   quad is large enough to blow past the GTE's +/-1023 screen clamp when the
   player walks under one. */
#define LS_CONE_SEGS  4
#define LS_POOL_CELLS 2

/* One additive flat quad. Returns nothing; silently drops anything off-screen,
   behind the camera, or past the packet budget. */
static void ls_quad(RenderContext *ctx, const SVECTOR v[4],
                    uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) return;

    DVECTOR sv[4];
    int32_t sz[4], otz;

    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);
    gte_stsz4c(sz);
    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);
    gte_stsz(&sz[3]);
    if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) return;

    int k;
    for (k = 0; k < 4; k++)
        if (sv[k].vx <= -1023 || sv[k].vx >= 1023 ||
            sv[k].vy <= -1023 || sv[k].vy >= 1023) return;

    gte_avsz4();
    gte_stotz(&otz);
    if (otz <= SCENE_OT_MIN) return;
    /* The room mesh and every prop in it sort at otz+40; the beams share that
       bias so a wall in front of a shaft still hides it. */
    otz += 40;
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

/* Scale a colour to an additive brightness. "Fogging out" an additive poly means
   fading it toward black, so distance scales the level rather than blending
   toward the room's fog colour. */
static void ls_shade(int colour, int32_t level_256, int32_t scale_256,
                     uint8_t *r, uint8_t *g, uint8_t *b) {
    int32_t s = (level_256 * scale_256) >> 8;
    *r = (uint8_t)((COLOUR_RGB[colour][0] * s) >> 8);
    *g = (uint8_t)((COLOUR_RGB[colour][1] * s) >> 8);
    *b = (uint8_t)((COLOUR_RGB[colour][2] * s) >> 8);
}

static void draw_cone(RenderContext *ctx, int i, int32_t bright) {
    /* The frustum's two ends, corner for corner: (-x,-z), (+x,-z), (+x,+z),
       (-x,+z) so consecutive corners share an edge. */
    const int32_t ax[4] = { FIXTURE[i].cx - LS_CUBE_HALF, FIXTURE[i].cx + LS_CUBE_HALF,
                            FIXTURE[i].cx + LS_CUBE_HALF, FIXTURE[i].cx - LS_CUBE_HALF };
    const int32_t az[4] = { FIXTURE[i].cz - LS_CUBE_HALF, FIXTURE[i].cz - LS_CUBE_HALF,
                            FIXTURE[i].cz + LS_CUBE_HALF, FIXTURE[i].cz + LS_CUBE_HALF };
    const int32_t bx[4] = { LS_POOL_X0, LS_POOL_X1, LS_POOL_X1, LS_POOL_X0 };
    const int32_t bz[4] = { LS_POOL_Z0, LS_POOL_Z0, LS_POOL_Z1, LS_POOL_Z1 };

    int colour = colour_of[i];
    uint8_t r, g, b;
    SVECTOR v[4];
    int s, t, k;

    /* --- The cap over the cube's underside: the lamp itself, lit --- */
    ls_shade(colour, bright, LS_CAP_LEVEL, &r, &g, &b);
    for (k = 0; k < 4; k++) v[k].pad = 0;
    v[0].vx = (int16_t)ax[0]; v[0].vy = LS_CUBE_BOT; v[0].vz = (int16_t)az[0];
    v[1].vx = (int16_t)ax[1]; v[1].vy = LS_CUBE_BOT; v[1].vz = (int16_t)az[1];
    v[2].vx = (int16_t)ax[3]; v[2].vy = LS_CUBE_BOT; v[2].vz = (int16_t)az[3];
    v[3].vx = (int16_t)ax[2]; v[3].vy = LS_CUBE_BOT; v[3].vz = (int16_t)az[2];
    ls_quad(ctx, v, r, g, b);

    /* --- The shaft, banded along its length --- */
    ls_shade(colour, bright, LS_SHAFT_LEVEL, &r, &g, &b);
    for (s = 0; s < 4; s++) {
        int n = (s + 1) & 3;
        for (t = 0; t < LS_CONE_SEGS; t++) {
            /* Corner c at band edge e: a straight lerp from cap to pool. */
            int32_t e0 = t, e1 = t + 1;
            int32_t y0 = LS_CUBE_BOT + ((LS_POOL_Y - LS_CUBE_BOT) * e0) / LS_CONE_SEGS;
            int32_t y1 = LS_CUBE_BOT + ((LS_POOL_Y - LS_CUBE_BOT) * e1) / LS_CONE_SEGS;

            v[0].vx = (int16_t)(ax[s] + ((bx[s] - ax[s]) * e0) / LS_CONE_SEGS);
            v[0].vz = (int16_t)(az[s] + ((bz[s] - az[s]) * e0) / LS_CONE_SEGS);
            v[0].vy = (int16_t)y0;
            v[1].vx = (int16_t)(ax[n] + ((bx[n] - ax[n]) * e0) / LS_CONE_SEGS);
            v[1].vz = (int16_t)(az[n] + ((bz[n] - az[n]) * e0) / LS_CONE_SEGS);
            v[1].vy = (int16_t)y0;
            v[2].vx = (int16_t)(ax[s] + ((bx[s] - ax[s]) * e1) / LS_CONE_SEGS);
            v[2].vz = (int16_t)(az[s] + ((bz[s] - az[s]) * e1) / LS_CONE_SEGS);
            v[2].vy = (int16_t)y1;
            v[3].vx = (int16_t)(ax[n] + ((bx[n] - ax[n]) * e1) / LS_CONE_SEGS);
            v[3].vz = (int16_t)(az[n] + ((bz[n] - az[n]) * e1) / LS_CONE_SEGS);
            v[3].vy = (int16_t)y1;
            ls_quad(ctx, v, r, g, b);
        }
    }

    /* --- The pool on the star, as a grid so no one quad covers the screen --- */
    ls_shade(colour, bright, LS_POOL_LEVEL, &r, &g, &b);
    {
        int cx_i, cz_i;
        for (cx_i = 0; cx_i < LS_POOL_CELLS; cx_i++) {
            int32_t x0 = LS_POOL_X0 + ((LS_POOL_X1 - LS_POOL_X0) * cx_i) / LS_POOL_CELLS;
            int32_t x1 = LS_POOL_X0 + ((LS_POOL_X1 - LS_POOL_X0) * (cx_i + 1)) / LS_POOL_CELLS;
            for (cz_i = 0; cz_i < LS_POOL_CELLS; cz_i++) {
                int32_t z0 = LS_POOL_Z0 + ((LS_POOL_Z1 - LS_POOL_Z0) * cz_i) / LS_POOL_CELLS;
                int32_t z1 = LS_POOL_Z0 + ((LS_POOL_Z1 - LS_POOL_Z0) * (cz_i + 1)) / LS_POOL_CELLS;
                v[0].vx = (int16_t)x0; v[0].vy = LS_POOL_Y; v[0].vz = (int16_t)z0;
                v[1].vx = (int16_t)x1; v[1].vy = LS_POOL_Y; v[1].vz = (int16_t)z0;
                v[2].vx = (int16_t)x0; v[2].vy = LS_POOL_Y; v[2].vz = (int16_t)z1;
                v[3].vx = (int16_t)x1; v[3].vy = LS_POOL_Y; v[3].vz = (int16_t)z1;
                ls_quad(ctx, v, r, g, b);
            }
        }
    }
}

/* The floating "Press O to activate/deactivate" sign in front of a lever. XY
   plane (reading along X, fixed Z), exactly like the room's own door sign:
   door_draw_string_3d centres the reading axis on world_x AFTER adding 200, so
   pass lever_x - 200. The north wall pair are read from -Z (mirror 0), the south
   wall pair from +Z (mirror 1), which is what FIXTURE.mirror carries. */
static void draw_prompt(RenderContext *ctx, int i) {
    int32_t dx = cam_x - FIXTURE[i].lx;
    int32_t dz = cam_z - FIXTURE[i].lz;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= LS_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > LS_FADE_NEAR) {
        int range = LS_TEXT_RADIUS - LS_FADE_NEAR;
        int prog  = xz - LS_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* The label states what the press WILL do, so it tracks the switch's
       committed state rather than the lever's mid-throw position. */
    door_draw_string_3d(ctx,
                        lit[i] ? "Press " BTN_CIRCLE " to deactivate"
                               : "Press " BTN_CIRCLE " to activate",
                        FIXTURE[i].lx - 200, LS_TEXT_Y, FIXTURE[i].text_z,
                        50, 255, 50, fade, FIXTURE[i].mirror,
                        TEXT_PLANE_XY, LS_TEXT_PIXEL);
}

void lightswitch_draw(RenderContext *ctx) {
    int i;

    for (i = 0; i < LS_COUNT; i++) {
        int32_t bright = level(i);
        if (bright <= 0) continue;

        /* Distance fade, matched to the room's fog budget (350..1500) but scaled
           toward black rather than toward the fog colour — see ls_shade. */
        int32_t dx = FIXTURE[i].cx - cam_x, dz = FIXTURE[i].cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (dist < 1500) {
            int32_t fog = dist < 350 ? 350 : dist;
            int32_t ff  = ((1500 - fog) << 8) / (1500 - 350);
            draw_cone(ctx, i, (bright * ff) >> 8);
        }
    }

    /* Prompts are free play only — during the payoff the camera is elsewhere and
       nothing is interactive, and once solved the levers are finished with. */
    if (state != LS_IDLE || solved()) return;
    for (i = 0; i < LS_COUNT; i++) draw_prompt(ctx, i);
}
