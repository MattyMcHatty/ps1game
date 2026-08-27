#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"          /* game_flag, show_pickup_msg_raw */
#include "sound.h"
#include "btn_glyph.h"       /* BTN_CIRCLE */
#include "door.h"            /* door_draw_string_3d */
#include "title.h"           /* STATE_GREENHOUSE */
#include "vines.h"
#include "greenhouse_puzzle.h"

/* ---- The ten buttons, read straight out of "Greenhouse.smx" -----------------
   `prim` is the primitive's index in the mesh, which is what greenhouse.c's draw
   loop counts in and what greenhouse_tex_map[] is indexed by. The indices below
   were taken by listing every poly whose texture resolves to pipe_button_off;
   re-export the room and they can move, so re-derive them rather than nudging
   them (the check is: exactly ten prims carry slot 9 in src/greenhouse_tex_map.h,
   and the same ten appear here).

   x/z are the quad's centre. Every button is a flat YZ-plane quad spanning
   y[-243,-158] on its wall, so the centre height is -200 and the only thing that
   varies is z.

   `mirror` is which side of the wall the reader stands on, in
   door_draw_string_3d's sense. The east wall (x=100) is the greenhouse door's
   own wall, approached from -X, so it takes mirror=1 exactly as that door's sign
   does; the west wall (x=-3100) is approached from +X and takes mirror=0. Get it
   backwards and the prompt reads "O sserP". `sign_x` carries GHB_TEXT_STANDOFF
   in the right direction for that side. */
/* HOW FAR THE SIGN FLOATS OFF THE WALL. The room's door signs use 11, which is
   the standoff for text painted ON a surface; these read better standing proud
   of it, so they take 40. It is far enough to separate the glyphs from the
   brickwork at a glance and near enough that the sign still plainly belongs to
   the button above it rather than hanging in the aisle. */
#define GHB_TEXT_STANDOFF     40

static const struct {
    uint16_t prim;
    int32_t  x, z;
    int32_t  sign_x;
    int      mirror;
} BUTTON[GH_BUTTON_COUNT] = {
    /*  1 */ {  874, -3100,   813, -3100 + GHB_TEXT_STANDOFF, 0 },
    /*  2 */ {  875, -3100,   308, -3100 + GHB_TEXT_STANDOFF, 0 },
    /*  3 */ {  804, -3100,   -50, -3100 + GHB_TEXT_STANDOFF, 0 },
    /*  4 */ {  780, -3100, -1034, -3100 + GHB_TEXT_STANDOFF, 0 },
    /*  5 */ {  487, -3100, -2370, -3100 + GHB_TEXT_STANDOFF, 0 },
    /*  6 */ {  519,   100,   813,   100 - GHB_TEXT_STANDOFF, 1 },
    /*  7 */ {  510,   100,   308,   100 - GHB_TEXT_STANDOFF, 1 },
    /*  8 */ {  801,   100, -1034,   100 - GHB_TEXT_STANDOFF, 1 },
    /*  9 */ {  884,   100, -1765,   100 - GHB_TEXT_STANDOFF, 1 },
    /* 10 */ {  888,   100, -2370,   100 - GHB_TEXT_STANDOFF, 1 },
};

/* >>> THE PRIM COLUMN WAS RE-DERIVED FOR THE Aug 2026 DECIMATED EXPORT. <<<
   The room went from 1230 primitives to 1025 and every index moved; the x/z
   column did NOT change, because the buttons themselves were not touched. The
   check the block above prescribes was run and passes: exactly ten prims carry
   slot 9 in src/greenhouse_tex_map.h, and they are the ten listed here, at the
   ten coordinates that were already here. Re-derive again on the next
   re-export — do not nudge these. */

/* Buttons 3, 4, 5 and 6 — bits 2, 3, 4 and 5. The whole solve test is
   `lit_mask == GH_SOLUTION_MASK`, so a sixth button left on, or one of these
   four switched back off, is unsolved by construction. */
#define GH_SOLUTION_MASK  ((1u << 2) | (1u << 3) | (1u << 4) | (1u << 5))

/* ---- Interaction ------------------------------------------------------------
   Manhattan to the button's centre. 400 rather than the doors' 500, and the
   reason is the SPACING: the tightest pair on one wall is 2 and 3, 358 apart at
   z=308 and z=-50. The player is held 195 off the wall by the collision radius,
   so at the wall a 400 budget leaves 205 of reach along z — comfortably less
   than half that gap, which means no standing spot is in range of two buttons at
   once and "nearest wins" never has to arbitrate something the player would read
   as a coin toss. It still leaves 205 units of slack at the wall itself. */
#define GHB_TRIGGER_RADIUS   400
#define GHB_TEXT_RADIUS     1100
#define GHB_FADE_NEAR        750
#define GHB_TEXT_PIXEL         4   /* DOOR_PIXEL_SIZE. "Press O" is 7 chars on a
                                      6-column cell, so 168 units wide — which
                                      still clears the 358-unit gap between the
                                      closest pair of buttons at this size and
                                      would not at 5 */
#define GHB_TEXT_Y         (-145)  /* glyph TOP. 7 rows of 4 end at -117, so the
                                      prompt hangs in the 145 units of clear wall
                                      between the button's bottom edge and the
                                      floor, which is what "under it" means here */

/* ---- The payoff -------------------------------------------------------------
   The curtain is 900 tall and the room's drawn ceiling is at -900, so the raise
   distance is the model's own height: at the end of it the whole thing is inside
   the roof. vines.c owns the animation itself (see vines_raise_start).

   >>> 108 FRAMES BECAUSE THAT IS HOW LONG THE SOUND IS. <<< SFX_MCHNE_GH runs
   1.8 s and the travel is cut to it, so the grind starts with the curtain and
   stops with it — the same contract SFX_GRIND has with GP_TRAVEL_FRAMES. Retrim
   sounds/mchne_gh.vag and this constant has to move with it. */
#define GHB_RAISE_FRAMES   108

/* ---- The payoff shot --------------------------------------------------------
   A HARD CUT, like the Attic Exit lightswitches' — not a glide — to a spot
   halfway up the nave, looking back and down at the annexe doorway the curtain
   is coming out of.

   THE SPOT IS IN THE AISLE BETWEEN THE TWO BEDS. Collision walls 0-3 box a bed
   at x[-2433,-500] z[-2000,-1400] and walls 4-6 another at x[-3100,-1766]
   z[-800,-300]; (-2013,-1104) sits in the clear strip between them, and the
   sight line to the doorway crosses the first bed's north-west corner at
   z = -1324, which is 76 clear of its z = -1400 face. Move GHB_CAM_X/Z and that
   has to be re-checked as well as the yaw re-derived.

   THE YAW is atan2(dx,dz) onto the curtain at (-3150,-1700): dx = -1137,
   dz = -596, i.e. 242.34deg, which is 2757 of 4096. The same derivation the
   lightswitch's shot uses, and the same trap — it is measured from +Z toward
   +X, so it is NOT atan2(dz,dx).

   THE PITCH is solved the same way: the camera sits at y = -700, well above the
   waist-high beds so it looks over them, and the doorway's mid-height is -450,
   which is 250 BELOW it. World +Y is down, so that is a positive (downward)
   pitch of atan(250/1284) = 11.02deg = 125 of 4096. At that range the vertical
   field spans about 602 either way, so the whole 900-tall opening is in frame
   with the floor under it. */
#define GHB_CAM_X      (-2013)
#define GHB_CAM_Y        (-700)
#define GHB_CAM_Z      (-1104)
#define GHB_CAM_ROT       2757
#define GHB_CAM_PITCH      125
#define GHB_SOLVE_HOLD      30   /* half a second on the open doorway before the
                                    camera goes back to the player */

/* ---- State -----------------------------------------------------------------
   PARTIAL PROGRESS IS NOT SAVED, and that is deliberate rather than an omission:
   the lightswitch puzzle re-deals its colours on every entry for the same
   reason. What persists is the ANSWER (FLAG_GREENHOUSE_BUTTONS) and the world
   change it caused (the curtain's cleared state, which rides in the WorldDelta
   like any other). A half-pressed board is not a world change. */
typedef enum {
    GHP_IDLE = 0,   /* free play: the buttons are live                     */
    GHP_CUT,        /* solved: fixed shot, the curtain winding up          */
    GHP_HOLD        /* curtain gone: a beat, then the camera goes back     */
} GhpState;

static uint16_t lit_mask     = 0;
static int      circle_prev  = 1;   /* starts "held": swallow a press carried in
                                       through the door transition */
static GhpState state        = GHP_IDLE;
static int      hold_timer   = 0;
static int32_t  save_cx, save_cy, save_cz, save_crot, save_cvy;

static int solved(void) { return game_flag(FLAG_GREENHOUSE_BUTTONS); }

int greenhouse_puzzle_active(void) { return state != GHP_IDLE; }

void greenhouse_puzzle_init(void) {
    /* A returning player finds the board as they left it: the four right buttons
       lit and nothing else. Unsolved, everything starts dark — see the note on
       partial progress above. */
    lit_mask    = solved() ? GH_SOLUTION_MASK : 0;
    state       = GHP_IDLE;
    hold_timer  = 0;
    circle_prev = interact_tapped();

    /* >>> THE FLAG IS SET, BUT THE CURTAIN IS STILL THERE. <<< That pairing is
       reachable and it is a DEAD ROOM if it is not caught here: solve() sets the
       flag and starts a GHB_RAISE_FRAMES animation, and anything that ends the
       session inside those two seconds — a death, a reset, a debug jump — comes
       back with the flag saved and the curtain's cleared state not. update() returns early
       once solved(), so nothing would ever start the raise again and the annexe
       would be shut for good with the board answered.

       Clearing it outright rather than replaying the animation is the right
       answer: the animation is a reward for the press, and this player has
       already had it or has arrived by a route that never earned one (a debug
       grant of the flag). Either way what they need is the doorway open. */
    if (solved()) {
        int v = vine_locked_in_area(STATE_GREENHOUSE);
        if (v >= 0) {
            vines[v].lift   = VINE_HEIGHT;
            vines[v].health = 0;
            vines[v].state  = VINE_CLEARED;
        }
    }
}

int greenhouse_button_prim_lit(int prim) {
    int i;
    for (i = 0; i < GH_BUTTON_COUNT; i++)
        if (BUTTON[i].prim == (uint16_t)prim)
            return (lit_mask >> i) & 1;
    return 0;
}

/* Index of the button the player is standing at, or -1. Nearest wins, and it has
   to be in view as well as in range — a button behind you is not one you can
   press, and without that test walking backwards into the wall between two of
   them would toggle whichever happened to be marginally closer. */
static int button_in_reach(void) {
    int i, best = -1;
    int32_t best_d = GHB_TRIGGER_RADIUS;
    for (i = 0; i < GH_BUTTON_COUNT; i++) {
        int32_t dx = cam_x - BUTTON[i].x;
        int32_t dz = cam_z - BUTTON[i].z;
        int32_t d  = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (d >= best_d) continue;
        if (!interact_facing(BUTTON[i].x, BUTTON[i].z)) continue;
        best_d = d; best = i;
    }
    return best;
}

static void end_cutscene(void) {
    state     = GHP_IDLE;
    cam_x     = save_cx; cam_y = save_cy; cam_z = save_cz;
    cam_rot   = save_crot; cam_vy = save_cvy;
    cam_pitch = 0;
    camera_release_player();
    circle_prev = 1;   /* swallow the Circle that is probably still held */
}

static void solve(void) {
    int v = vine_locked_in_area(STATE_GREENHOUSE);

    game_flag_set(FLAG_GREENHOUSE_BUTTONS);

    /* vine_locked_in_area, NOT vine_in_area: since the flood this room holds
       six curtains and the five in the aisles are destructible. Once the annexe
       curtain is cleared vine_in_area would start answering with one of those,
       and the catch-up branch below would delete an aisle curtain on every
       later entry. The one this puzzle opens is by construction the one nothing
       else can remove — see vines.h. */
    /* The curtain may already be gone — a save made mid-raise reloads with it
       cleared, and a debug grant of the flag can arrive with no curtain at all.
       Setting the flag is the part that must happen either way; there is nothing
       to cut away and look at if there is nothing to animate. */
    if (v < 0) {
        show_pickup_msg_raw("Something winds back into the roof");
        return;
    }

    /* The player STAYS at the button they just pressed while the camera cuts
       away, so anything hunting them keeps hunting the real spot — and so the
       camera has somewhere honest to come back to. */
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    camera_anchor_player(save_cx, save_cy, save_cz);

    cam_x   = GHB_CAM_X; cam_y = GHB_CAM_Y; cam_z = GHB_CAM_Z;
    cam_rot = GHB_CAM_ROT; cam_pitch = GHB_CAM_PITCH; cam_vy = 0;

    vines_raise_start(v, GHB_RAISE_FRAMES);
    sound_play(SFX_MCHNE_GH);   /* the same grinding machinery the piano bookcase
                                   and the cage gate use, re-cut for the garden
                                   bank: SFX_MCHNE itself is HOUSE-only and too
                                   big to copy here, so it would be SILENT in
                                   this room. See sound.h. */
    show_pickup_msg_raw("Something winds back into the roof");
    state = GHP_CUT;
}

int greenhouse_puzzle_update(void) {
    /* The cut owns the camera, so nothing else in the room may run while it
       does — main.c's area update returns straight after calling this, the way
       the Attic Exit's does for its lightswitch payoff. The Circle edge state is
       kept fresh throughout so the tap that solved the board cannot land on a
       button, or the door, on the frame control comes back. */
    if (state == GHP_CUT) {
        if (vines_raise_update()) {
            state      = GHP_HOLD;
            hold_timer = GHB_SOLVE_HOLD;
        }
        circle_prev = interact_tapped();
        return 1;
    }
    if (state == GHP_HOLD) {
        if (--hold_timer <= 0) end_cutscene();
        else                   circle_prev = interact_tapped();
        return 1;
    }

    int held = interact_tapped();
    int just = held && !circle_prev;
    circle_prev = held;

    if (!just || solved()) return 0;

    int i = button_in_reach();
    if (i < 0) return 0;

    lit_mask ^= (uint16_t)(1u << i);
    sound_play(SFX_CURSOR);   /* resident, so it is audible in every bank, and
                                 off the one-shot pool on its own voice — a
                                 player running along the wall pressing buttons
                                 is the one place in this room that fires sounds
                                 back to back, which is exactly what the menu
                                 blips were given private voices for */

    if (lit_mask == GH_SOLUTION_MASK) solve();
    return 1;
}

/* One button's floating prompt: the Circle glyph alone, in its own red, hanging
   under the button. door_draw_string_3d centres the reading axis (Z, for a YZ
   sign) on world_z AFTER adding 200, hence the -200. */
static void draw_prompt(RenderContext *ctx, int i) {
    int32_t dx = cam_x - BUTTON[i].x;
    int32_t dz = cam_z - BUTTON[i].z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= GHB_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > GHB_FADE_NEAR) {
        int range = GHB_TEXT_RADIUS - GHB_FADE_NEAR;
        int prog  = xz - GHB_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* The r/g/b passed here is ignored for a button code — btn_glyph_lookup
       supplies Circle's own red and the fade still applies — but it has to be
       something, so it matches the room's other signs. */
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE,
                        BUTTON[i].sign_x, GHB_TEXT_Y, BUTTON[i].z - 200,
                        50, 255, 50, fade, BUTTON[i].mirror,
                        TEXT_PLANE_YZ, GHB_TEXT_PIXEL);
}

void greenhouse_puzzle_draw(RenderContext *ctx) {
    int i;
    /* Once the board is answered the buttons are finished with, so the prompts
       go — the same call the lightswitch levers make. The LIT ART STAYS: the
       four burning buttons are the record of what the answer was. */
    if (solved()) return;
    for (i = 0; i < GH_BUTTON_COUNT; i++) draw_prompt(ctx, i);
}
