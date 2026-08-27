#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"          /* game_flag, player_items, show_pickup_msg_raw */
#include "menu.h"            /* MENU_SLOT_*, the shared item icons and names  */
#include "sound.h"
#include "btn_glyph.h"       /* BTN_CIRCLE, btn_prompt_draw */
#include "door.h"            /* door_draw_string_3d for the plinth prompts */
#include "item_pickup.h"     /* the hatch key the payoff leaves behind */
#include "keystone_plinths.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* The Keystone Maze's four alcove plinths and the keystone in the middle. See
   keystone_plinths.h for the plinth/colour/face table and the shape of the
   interaction. */

/* ---- Colours ---------------------------------------------------------------
   One per plinth, in the array order below, plus the white the keystone's top
   burns once all four faces are lit. */
#define KP_COUNT 4

static const uint8_t COLOUR_RGB[KP_COUNT + 1][3] = {
    {  40, 255,  60 },   /* 0 NW  green   */
    { 255,  40, 220 },   /* 1 W   magenta */
    { 255, 230,  40 },   /* 2 NE  yellow  */
    {  40,  80, 255 },   /* 3 SE  blue    */
    { 255, 255, 255 },   /* 4     white: the top face */
};
#define KP_WHITE KP_COUNT

/* ---- The fixtures, read straight off "Keystone Maze.smx" --------------------
   Each plinth is a 200x200 block whose plinth_diamond cap is a SLOPED quad:
   y=-175 along the back edge, y=-124 along the edge facing the corridor. The
   approach vector (ax,az) points from the plinth toward the player, i.e. out of
   the alcove, and it is what decides which edge is which, which plane the sign
   lies in and which way it is mirrored:

     ax=+1  the player stands EAST of it   sign in YZ, mirror 0   (as the west
                                                                   gate's)
     ax=-1  the player stands WEST of it   sign in YZ, mirror 1   (as Maze One's
                                                                   east gate)
     az=-1  the player stands SOUTH of it  sign in XY, mirror 0

   >>> READ THE VECTOR OFF THE CAP'S SLOPE, NOT OFF THE COLLISION WALLS. <<< The
   north-east plinth is cornered by hedge on two sides and has a wall on each, so
   the wall list alone would let you read it either way — and reading it the
   wrong way puts the sign round the side and builds the glow quad twisted across
   the real cap. The cap is unambiguous: its LOW edge (y=-124) always faces the
   corridor. Only the NW plinth here is read along X; the other three are read
   from the south, and no plinth is read from +Z, so the az=+1 / mirror 1 case
   does not arise. The table still drives all four off the vector rather than
   hard-coding, so a fifth plinth would need no new rule.

   The keystone's own faces are the quads at x=2300/2500 and z=2300/2500, each
   200 wide and 150 tall over x/z(2300,2500); `fx` is 0 for a face in the YZ
   plane (fixed X) and 1 for one in the XY plane (fixed Z), `fc` is that fixed
   coordinate and `fd` the outward normal's sign along the same axis. */
static const struct {
    int32_t px, pz;      /* the plinth's centre in XZ                        */
    int8_t  ax, az;      /* approach vector: plinth -> player, one unit axis */
    int     item;        /* MENU_SLOT_* of the stone it takes                */
    int     bit;         /* ITEM_* bit that stone occupies in player_items   */
    int     flag;        /* GameFlag recording that it has been placed       */
    int8_t  fx;          /* the keystone face: 0 = YZ (fixed X), 1 = XY      */
    int32_t fc;          /* ...its fixed coordinate                          */
    int8_t  fd;          /* ...and the sign of its outward normal            */
    /* The shot: up and out to one side of the alcove, looking down on the cap.
       Every one of these stands on the corridor floor the player walked in
       along, so none of them is buried in a hedge. Yaw is atan2(dx,dz) onto the
       cap and pitch is atan(dy/horizontal) — move a camera and both have to be
       re-derived. */
    int32_t cx, cy, cz, crot, cpitch;
    const char *msg;
} PLINTH[KP_COUNT] = {
    /* NW — green   -> the keystone's NORTH face. Alcove off the top corridor,
       read from the east; wall 17 stops the player at x=99. */
    {    0, 5400,  1,  0, MENU_SLOT_GREEN_KEY_STONE,   ITEM_GREEN_KEY_STONE,
         FLAG_KEYSTONE_NW,   1, 2500,  1,
       400, -460, 5150, 3436, 354,
       "The green stone settles into the plinth" },
    /* W — magenta -> the WEST face. Dead end off the long west corridor, read
       from the south; wall 58 stops the player at z=3500. */
    {    0, 3600,  0, -1, MENU_SLOT_MAGENTA_KEY_STONE, ITEM_MAGENTA_KEY_STONE,
         FLAG_KEYSTONE_W,    0, 2300, -1,
      -250, -460, 3200,  364, 354,
       "The magenta stone settles into the plinth" },
    /* NE — yellow -> the EAST face. North-east corner. It has hedge on TWO
       sides — wall 59 along x=5300 and wall 5 along z=5300 — so the walls alone
       do not say which way it is meant to be read. THE CAP DOES: its low edge
       (y=-124) is the one at z=5300, so it tilts south and this plinth is
       approached from the south like the other two, NOT from the west. */
    { 5400, 5400,  0, -1, MENU_SLOT_YELLOW_KEY_STONE,  ITEM_YELLOW_KEY_STONE,
         FLAG_KEYSTONE_NE,   0, 2500,  1,
      5150, -460, 5000,  364, 354,
       "The yellow stone settles into the plinth" },
    /* SE — blue   -> the SOUTH face. Off the east corridor, read from the
       south; wall 63 stops the player at z=1100. */
    { 4800, 1200,  0, -1, MENU_SLOT_BLUE_KEY_STONE,    ITEM_BLUE_KEY_STONE,
         FLAG_KEYSTONE_SE,   1, 2300, -1,
      4550, -460,  800,  364, 354,
       "The blue stone settles into the plinth" },
};

/* The cap, from the mesh: 199 across (so 99 either side of centre), its far edge
   at y=-175 and its near edge at -124. The glow is built on these exact
   coordinates and then pushed off them — see the GLOW table below, whose first
   layer's `push` is what keeps it from z-fighting the plinth_diamond under it. */
#define KP_HALF          99
#define KP_CAP_FAR    (-175)
#define KP_CAP_NEAR   (-124)

/* The keystone: x(2300,2500) z(2300,2500), drawn 150 tall on this room's y=0
   floor. Its five plinth_diamond faces are the glowing polys, so these are the
   coordinates the face and top glows are built on. */
#define KP_KEY_X0      2300
#define KP_KEY_X1      2500
#define KP_KEY_Z0      2300
#define KP_KEY_Z1      2500
#define KP_KEY_TOP    (-150)

/* ---- What a light IS here ---------------------------------------------------
   NOT a cone and NOT a pool on the floor. Every light in this room is a poly
   that GLOWS: the plinth_diamond face itself, made to burn its colour, with the
   glow standing a few units proud of it and bleeding a little past its edges.
   Nothing is cast onto anything else, and nothing reaches the ground.

   That is drawn as KP_LAYERS copies of the poly stacked along its own normal —
   each pushed a little further out, swelled a little wider, and dimmed — so what
   the player sees is a bright face with a soft halo around and above it rather
   than a beam. `push` is in world units straight out along the normal and
   `scale` swells the quad about its own centre in 1/256ths, so a 200-wide face
   at 268 gains about 5 units on every side.

   Keep the pushes SMALL. The brief was a few centimetres, and at this project's
   scale (100 units per Blender unit) that is what these numbers are — the whole
   stack stands 14 units off the poly, against a plinth 200 units wide. */
#define KP_LAYERS 3

static const struct { int16_t push; int16_t scale; uint8_t level; } GLOW[KP_LAYERS] = {
    {  2, 256, 235 },   /* the face itself, burning        */
    {  7, 268, 120 },   /* the halo just off it            */
    { 14, 284,  55 },   /* the last of it, fading into air */
};

/* The top face of the keystone burns a touch harder than a side does: it is the
   one the whole puzzle is pointed at, and it is seen edge-on from most of the
   court. */
#define KP_TOP_BOOST    276   /* 1/256ths applied on top of the layer's level */

/* ---- Timing ---------------------------------------------------------------- */
#define KP_FADE_FRAMES    45   /* 0.75 s each way: the plinth down, then the face up */
#define KP_CAM_FRAMES     24   /* the glide out to the plinth shot, as the stove's   */
#define KP_TOP_FRAMES     60   /* the keystone's top coming up white                 */
#define KP_HOLD_FRAMES    60   /* a beat on it before the camera goes back           */

/* ---- Interaction ------------------------------------------------------------
   Manhattan, measured to the plinth's centre. The player is stopped by the
   alcove's end wall about 295 short of it, so the trigger has to reach past
   that; the four plinths are thousands of units apart and each sits at the end
   of its own dead end, so there is no ambiguity about which one a press means. */
#define KP_TRIGGER_RADIUS   600
#define KP_TEXT_RADIUS     1200
#define KP_FADE_NEAR        800
#define KP_TEXT_PIXEL         3   /* "Press O to interact" is 19 chars: 456 wide */
#define KP_TEXT_Y         (-300)  /* glyph TOP; the line floats clear of the cap */
#define KP_SIGN_STANDOFF    111   /* 100 to the front face, 11 clear of it       */

/* ---- The payoff shot --------------------------------------------------------
   A hard cut, as the Attic Exit's lightswitch payoff is: from wherever the
   player was standing when the fourth face finished lighting, straight to a spot
   south-west of the keystone on the court's gravel. Yaw is atan2(dx,dz) onto
   the block's middle at (2400,-75,2400): dx=+500, dz=+650, i.e. 37.6deg of
   4096. Pitch is atan(445/820) = 28.5deg. Move the camera and re-derive both. */
#define KP_PAY_X       1900
#define KP_PAY_Y     (-520)
#define KP_PAY_Z       1750
#define KP_PAY_ROT      428
#define KP_PAY_PITCH    324

/* Where the reward lands: the MIDDLE of the keystone's top face, at the
   centre of x(2300,2500) z(2300,2500). It used to sit on the west lip instead,
   because the keystone has collision walls of its own — the 195 push radius
   parks the player 295 (Manhattan) from the centre, and the old flat
   ITEM_PICKUP_RADIUS of 200 could not reach that far, so a box on the middle
   would have been scenery. The pickup now carries its OWN reach instead, so it
   can sit where the shot points and still be taken by walking level with any
   face of the plinth.

   KP_REWARD_RANGE has to clear that 295 with a little slack for the diagonal
   approach into a corner; 340 does, and is still far short of the 600 the
   alcove plinths use for their own prompts, so nothing is collected from
   across the court. */
#define KP_REWARD_X    2400
#define KP_REWARD_Z    2400
#define KP_REWARD_RANGE 340
#define KP_REWARD_AMT     1   /* one Hatch Key; the second is elsewhere */

/* ---- Board layout (320x240) -------------------------------------------------
   The stove's item picker, centred: there is only one thing to fill here, so
   there is no box column beside it to make room for. The grid metrics are the
   stove's exactly, INCLUDING the warning that comes with them — the panel fits
   three PICK_CELL rows and no more, so a new item widens the grid rather than
   adding a row (see stove_puzzle.c). */
#define PICK_W        168
#define PICK_H        168
#define PICK_X    ((320 - PICK_W) / 2)
#define PICK_Y         30
#define PICK_CELL      42
#define PICK_ICON      30
#define PICK_PAD        6
#define PICK_COLS       4
#define PICK_ROWS  ((MENU_ITEM_SLOTS + PICK_COLS - 1) / PICK_COLS)
#define PICK_GRID_X   (PICK_X + (PICK_W - PICK_COLS * PICK_CELL) / 2)
#define PICK_GRID_Y   (PICK_Y + 22)
#define PICK_NAME_Y   (PICK_Y + PICK_H - 16)

/* OT layers — all inside the menu-reserved range (0..SCENE_OT_MIN-1), so the
   panel always sits on top of the room. Higher index = further back. */
#define KP_OT_PANEL    12
#define KP_OT_LINE     10
#define KP_OT_TEXWIN    8   /* reset the room's 128 window before the icons */
#define KP_OT_ICON      7
#define KP_OT_CURSOR    3
#define KP_OT_TEXT      1

/* ---- State ----------------------------------------------------------------- */
typedef enum {
    KP_IDLE = 0,   /* free play: the prompts are live               */
    KP_INTRO,      /* camera gliding out to the plinth shot         */
    KP_PICKER,     /* the item picker, over that shot               */
    KP_CUT,        /* all four in: the keystone's top coming up     */
    KP_HOLD        /* a beat on it, then the camera goes back       */
} KpState;

static KpState state = KP_IDLE;

/* Per plinth: which half of the cross-fade is running, and how far into it.
   Phase 0 is settled — the levels then come straight off the flag. */
static uint8_t phase[KP_COUNT];
static uint8_t ptime[KP_COUNT];

static int16_t top_level;     /* 0..256: the keystone's white top face */
static int     cur      = 0;  /* the plinth the picker is filling      */
static int     pick_cur = 0;  /* picker cursor: MENU_SLOT_*            */
static int     hold_timer = 0;
static int     top_timer  = 0;

static int32_t save_cx, save_cy, save_cz, save_crot, save_cvy;
static int32_t cam_anim_t, src_x, src_y, src_z, src_rot, rot_delta;

static uint16_t kp_btn_prev   = 0xFFFF;  /* picker input edge-detect (btn = ~pad) */
static int      interact_prev = 1;       /* interact-Circle edge-detect           */

int keystone_plinths_active(void) { return state != KP_IDLE; }

static int placed(int i) { return game_flag((GameFlag)PLINTH[i].flag); }

static int all_placed(void) {
    int i;
    for (i = 0; i < KP_COUNT; i++) if (!placed(i)) return 0;
    return 1;
}

/* 0..256. A plinth burns until its stone goes in, then fades out over the first
   half of the cross-fade; its keystone face is dark until that half finishes and
   comes up over the second. One number each, derived rather than stored, so a
   room entry installs the finished state by doing nothing at all. */
static int32_t plinth_level(int i) {
    if (!placed(i)) return 256;
    if (phase[i] == 1) return 256 - ((int32_t)ptime[i] * 256) / KP_FADE_FRAMES;
    return 0;
}

static int32_t face_level(int i) {
    if (!placed(i)) return 0;
    if (phase[i] == 1) return 0;
    if (phase[i] == 2) return ((int32_t)ptime[i] * 256) / KP_FADE_FRAMES;
    return 256;
}

void keystone_plinths_place(void) {
    int i;
    for (i = 0; i < KP_COUNT; i++) { phase[i] = 0; ptime[i] = 0; }
    top_level  = game_flag(FLAG_KEYSTONE_REWARD) ? 256 : 0;
    state      = KP_IDLE;
    hold_timer = 0;
    top_timer  = 0;
    cur        = 0;
    pick_cur   = 0;
    camera_release_player();

    /* Don't treat a Circle held through the gate transition as an interact. */
    interact_prev = interact_tapped();
}

void keystone_plinths_apply_flags(void) {
    keystone_plinths_place();

    /* The reward catch-up. Four stones in and no reward recorded means the
       payoff never got to run — a quit, a death or a debug jump in the seconds
       between the fourth placement and the spawn. Hand the key over here
       instead of replaying the scene, and record it so this fires once. */
    if (all_placed() && !game_flag(FLAG_KEYSTONE_REWARD)) {
        item_pickup_set_display(
            item_pickup_spawn_range(KP_REWARD_X, KP_KEY_TOP, KP_REWARD_Z,
                                    PICKUP_HATCH_KEY, KP_REWARD_AMT,
                                    KP_REWARD_RANGE),
            0, ITEM_PICKUP_ROOM_BIAS);   /* see finish_payoff */
        game_flag_set(FLAG_KEYSTONE_REWARD);
        top_level = 256;
    }
}

/* ---- State transitions ------------------------------------------------------ */

static void start_puzzle(int i) {
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    /* The player stays at the plinth while the camera swings out, so anything
       hunting them keeps hunting the real spot. */
    camera_anchor_player(save_cx, save_cy, save_cz);

    src_x = cam_x; src_y = cam_y; src_z = cam_z; src_rot = cam_rot;
    {   /* shortest signed turn from the current heading to the fixed shot */
        int32_t d = ((PLINTH[i].crot - src_rot) % 4096 + 4096) % 4096;
        if (d > 2048) d -= 4096;
        rot_delta = d;
    }

    cur        = i;
    pick_cur   = PLINTH[i].item;   /* open on the stone this plinth wants */
    cam_anim_t = 0;
    state      = KP_INTRO;
}

/* Drop the fixed camera and put the player back exactly where they stood. */
static void exit_puzzle(void) {
    state     = KP_IDLE;
    cam_x     = save_cx; cam_y = save_cy; cam_z = save_cz;
    cam_rot   = save_crot; cam_vy = save_cvy;
    cam_pitch = 0;
    camera_release_player();
    interact_prev = 1;   /* swallow the held Circle so we don't re-open instantly */
}

/* The right stone, in the right plinth. The stone is SPENT: nothing gives these
   four back a second time, and the placement flag is what carries the fact
   afterwards — see FLAG_KEYSTONE_NW in player.h.

   Play is handed back on the same frame, as specified, and the cross-fade runs
   in free play behind the player. */
static void place_stone(int i) {
    player_items &= ~(1 << PLINTH[i].bit);
    game_flag_set((GameFlag)PLINTH[i].flag);
    phase[i] = 1;
    ptime[i] = 0;
    sound_play(SFX_UNLOCK);
    show_pickup_msg_raw(PLINTH[i].msg);
    exit_puzzle();
}

/* All four faces are burning. Hard cut to the keystone, bring the top up white,
   leave the hatch key on it, hold a beat, cut back. */
static void start_payoff(void) {
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    camera_anchor_player(save_cx, save_cy, save_cz);

    cam_x = KP_PAY_X; cam_y = KP_PAY_Y; cam_z = KP_PAY_Z;
    cam_rot = KP_PAY_ROT; cam_pitch = KP_PAY_PITCH; cam_vy = 0;

    top_level = 0;
    top_timer = 0;
    state     = KP_CUT;
}

static void finish_payoff(void) {
    top_level = 256;
    /* The sprite is centred 50 above KP_KEY_TOP and reaches 70 world units
       down from there, so its bottom 20 units overlap the keystone it stands
       on. With no depth bias a pickup deliberately sorts in FRONT of room
       geometry at the same depth, so that overlap showed the key straight
       through the stone. ITEM_PICKUP_ROOM_BIAS puts it on the room mesh's own
       footing and the top of the plinth clips it, which is what standing on
       something looks like. Size is left alone — this one is a reward meant to
       be seen from across the court. */
    item_pickup_set_display(
        item_pickup_spawn_range(KP_REWARD_X, KP_KEY_TOP, KP_REWARD_Z,
                                PICKUP_HATCH_KEY, KP_REWARD_AMT,
                                KP_REWARD_RANGE),
        0, ITEM_PICKUP_ROOM_BIAS);
    game_flag_set(FLAG_KEYSTONE_REWARD);
    sound_play(SFX_PICKUP);
    show_pickup_msg_raw("The keystones light the way");
    state      = KP_HOLD;
    hold_timer = KP_HOLD_FRAMES;
}

/* ---- Update ----------------------------------------------------------------- */

/* Index of the plinth the player is standing at, or -1. */
static int plinth_in_reach(void) {
    int i, best = -1;
    int32_t best_d = KP_TRIGGER_RADIUS;
    for (i = 0; i < KP_COUNT; i++) {
        if (placed(i)) continue;               /* finished with */
        {
            int32_t dx = cam_x - PLINTH[i].px;
            int32_t dz = cam_z - PLINTH[i].pz;
            int32_t d  = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            if (d >= best_d) continue;
            /* Has to be in view as well as in range — a plinth behind you is not
               one you can reach into. */
            if (!interact_facing(PLINTH[i].px, PLINTH[i].pz)) continue;
            best_d = d;
            best   = i;
        }
    }
    return best;
}

void keystone_plinths_update(void) {
    int i;
    uint16_t btn = 0;
    if (pad_buff_len[0]) { PadResponse *pad = (PadResponse *)pad_buff[0]; btn = ~pad->btn; }

    if (state == KP_CUT) {
        if (++top_timer >= KP_TOP_FRAMES) finish_payoff();
        else top_level = (int16_t)(((int32_t)top_timer * 256) / KP_TOP_FRAMES);
        return;
    }
    if (state == KP_HOLD) {
        if (--hold_timer <= 0) exit_puzzle();
        return;
    }

    /* Advance every cross-fade that is running. This ticks in the picker and in
       the camera glide too: a player who walks straight from one plinth to the
       next must not freeze the light they just set going. */
    {
        int settled = -1;
        for (i = 0; i < KP_COUNT; i++) {
            if (phase[i] == 0) continue;
            if (++ptime[i] < KP_FADE_FRAMES) continue;
            ptime[i] = 0;
            /* First half done: the plinth is dark. Second half done: the face is
               fully lit, and THAT is the moment the puzzle is finished — the
               player watches the last face come up before the camera cuts. */
            if (phase[i] == 1) phase[i] = 2;
            else             { phase[i] = 0; settled = i; }
        }
        if (settled >= 0 && state == KP_IDLE && all_placed() &&
            !game_flag(FLAG_KEYSTONE_REWARD)) {
            start_payoff();
            return;
        }
    }

    if (state == KP_IDLE) {
        int held = interact_tapped();
        int just = held && !interact_prev;
        interact_prev = held;
        if (!just) return;
        i = plinth_in_reach();
        if (i >= 0) start_puzzle(i);
        return;
    }

    /* Camera glides out to the fixed shot; the picker opens once it settles. */
    if (state == KP_INTRO) {
        cam_anim_t++;
        {
            int32_t t = cam_anim_t * 256 / KP_CAM_FRAMES; if (t > 256) t = 256;
            {
                int32_t inv = 256 - t;
                int32_t e = 256 - (inv * inv / 256);        /* ease-out 0..256 */
                cam_x     = src_x   + ((PLINTH[cur].cx - src_x) * e) / 256;
                cam_y     = src_y   + ((PLINTH[cur].cy - src_y) * e) / 256;
                cam_z     = src_z   + ((PLINTH[cur].cz - src_z) * e) / 256;
                cam_rot   = src_rot + (rot_delta * e) / 256;
                cam_pitch = (PLINTH[cur].cpitch * e) / 256;
                cam_vy    = 0;
            }
        }
        if (cam_anim_t >= KP_CAM_FRAMES) {
            cam_x = PLINTH[cur].cx; cam_y = PLINTH[cur].cy; cam_z = PLINTH[cur].cz;
            cam_rot = PLINTH[cur].crot; cam_pitch = PLINTH[cur].cpitch; cam_vy = 0;
            state = KP_PICKER;
            kp_btn_prev = btn;   /* arm: the Circle that opened this is still held */
        }
        return;
    }

    /* KP_PICKER: the whole inventory in a PICK_COLS-wide grid. The trailing cells
       of the last row may be past MENU_ITEM_SLOTS; the guard below keeps the
       cursor out of them and the draw loop never paints them. */
    {
        uint16_t pressed = btn & ~kp_btn_prev;
        kp_btn_prev = btn;

        {
            int row = pick_cur / PICK_COLS, col = pick_cur % PICK_COLS;
            if (pressed & PAD_UP)    { if (row > 0) row--; }
            if (pressed & PAD_DOWN)  { if (row < PICK_ROWS - 1) row++; }
            if (pressed & PAD_LEFT)  { if (col > 0) col--; }
            if (pressed & PAD_RIGHT) { if (col < PICK_COLS - 1) col++; }
            {
                int next = row * PICK_COLS + col;
                if (next < MENU_ITEM_SLOTS && next != pick_cur) {
                    pick_cur = next;
                    sound_play(SFX_CURSOR);
                }
            }
        }

        if (pressed & PAD_CROSS) { sound_play(SFX_BACK); exit_puzzle(); return; }
        if ((pressed & PAD_CIRCLE) && menu_item_held(pick_cur)) {
            if (pick_cur == PLINTH[cur].item) {
                place_stone(cur);
            } else {
                /* Refused rather than swallowed: the player has to be told the
                   press landed and that this is the wrong stone, and the picker
                   stays up so the next guess costs nothing. */
                sound_play(SFX_BACK);
                show_pickup_msg_raw("It will not sit in the socket");
            }
        }
    }
}

/* ---- Drawing ----------------------------------------------------------------
   Every light here is ADDITIVE semi-transparency (ABR=1), the same blend the
   Attic Exit's light cones and the stove's gas flame use, so overlapping colours
   mix the way real light does and need no sorting among themselves. Additive
   means a DR_TPAGE has to be in front of each semi-transparent poly in OT order;
   adding the TPAGE to the SAME bucket immediately AFTER its quad puts it there,
   because the OT is LIFO.

   Nothing is backface culled: a glow is a volume, not a surface, and the layer
   stack is meant to read from underneath as well as from in front. Every quad is
   small — a swelled copy of a 200-unit face — so none of them comes anywhere
   near the GTE's +/-1023 screen clamp, which is why there is no banding here
   like the Attic Exit's shafts have. */

/* One additive flat quad, in POLY_F4's Z-order (v0,v1 one edge; v2,v3 the
   opposite one). Silently drops anything off-screen, behind the camera or past
   the packet budget. */
static void kp_quad(RenderContext *ctx, const SVECTOR v[4],
                    uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) return;

    {
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
        /* The room mesh and every prop in it sort at otz+40; the glows share
           that bias so a hedge in front of one still hides it. */
        otz += 40;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        {
            uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
            POLY_F4  *p  = (POLY_F4 *)ctx->next_packet;
            setPolyF4(p);
            setSemiTrans(p, 1);
            setRGB0(p, r, g, b);
            p->x0 = sv[0].vx; p->y0 = sv[0].vy;
            p->x1 = sv[1].vx; p->y1 = sv[1].vy;
            p->x2 = sv[2].vx; p->y2 = sv[2].vy;
            p->x3 = sv[3].vx; p->y3 = sv[3].vy;
            addPrim(&ot[otz], p);
            ctx->next_packet += sizeof(POLY_F4);

            {
                DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
                setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
                addPrim(&ot[otz], tp);
                ctx->next_packet += sizeof(DR_TPAGE);
            }
        }
    }
}

/* Scale a colour to an additive brightness. "Fogging out" an additive poly means
   fading it toward black, so distance scales the level rather than blending
   toward the room's purple fog. */
static void kp_shade(int colour, int32_t level_256, int32_t scale_256,
                     uint8_t *r, uint8_t *g, uint8_t *b) {
    int32_t s = (level_256 * scale_256) >> 8;
    if (s > 256) s = 256;
    *r = (uint8_t)((COLOUR_RGB[colour][0] * s) >> 8);
    *g = (uint8_t)((COLOUR_RGB[colour][1] * s) >> 8);
    *b = (uint8_t)((COLOUR_RGB[colour][2] * s) >> 8);
}

/* ---- The one way anything in here glows -------------------------------------
   Take a poly, stack KP_LAYERS swelled copies of it out along its own normal and
   dim them as they go (see the GLOW table). `base` is the poly in POLY_F4's
   Z-order; (nx,ny,nz) is its outward normal as a unit axis — this room has no
   glowing face that is not axis-aligned, and the alcove caps are close enough to
   flat (a 14deg tilt) that pushing them straight up is indistinguishable from
   pushing them along their true normal.

   The swell is about the poly's OWN centre, so a face grows evenly in both
   directions and the halo sits square on it whatever size the face is. */
static void kp_glow(RenderContext *ctx, const SVECTOR base[4],
                    int nx, int ny, int nz,
                    int colour, int32_t bright, int32_t boost) {
    int32_t cx = 0, cy = 0, cz = 0;
    int k, L;
    for (k = 0; k < 4; k++) { cx += base[k].vx; cy += base[k].vy; cz += base[k].vz; }
    cx /= 4; cy /= 4; cz /= 4;

    for (L = 0; L < KP_LAYERS; L++) {
        SVECTOR v[4];
        uint8_t r, g, b;
        int32_t sc = GLOW[L].scale, ph = GLOW[L].push;
        kp_shade(colour, bright, ((int32_t)GLOW[L].level * boost) >> 8, &r, &g, &b);
        for (k = 0; k < 4; k++) {
            v[k].pad = 0;
            v[k].vx = (int16_t)(cx + (((int32_t)base[k].vx - cx) * sc) / 256 + nx * ph);
            v[k].vy = (int16_t)(cy + (((int32_t)base[k].vy - cy) * sc) / 256 + ny * ph);
            v[k].vz = (int16_t)(cz + (((int32_t)base[k].vz - cz) * sc) / 256 + nz * ph);
        }
        kp_quad(ctx, v, r, g, b);
    }
}

/* ---- One alcove plinth's cap, burning --------------------------------------
   The cap is the sloped plinth_diamond quad itself: 199 across, its LOW edge
   (y=-124) facing the corridor and its high edge (y=-175) at the back. The glow
   is the same quad, so the slope comes along for free, pushed UP out of it. */
static void draw_plinth_glow(RenderContext *ctx, int i, int32_t bright) {
    int32_t ax = PLINTH[i].ax, az = PLINTH[i].az;
    int32_t px = PLINTH[i].px, pz = PLINTH[i].pz;
    SVECTOR v[4];
    int k;

    if (ax != 0) {          /* read along X: the slope runs along X too */
        int32_t nearx = px + ax * KP_HALF, farx = px - ax * KP_HALF;
        v[0].vx = (int16_t)nearx; v[0].vz = (int16_t)(pz - KP_HALF);
        v[1].vx = (int16_t)nearx; v[1].vz = (int16_t)(pz + KP_HALF);
        v[2].vx = (int16_t)farx;  v[2].vz = (int16_t)(pz - KP_HALF);
        v[3].vx = (int16_t)farx;  v[3].vz = (int16_t)(pz + KP_HALF);
    } else {                /* read along Z: so does the slope */
        int32_t nearz = pz + az * KP_HALF, farz = pz - az * KP_HALF;
        v[0].vx = (int16_t)(px - KP_HALF); v[0].vz = (int16_t)nearz;
        v[1].vx = (int16_t)(px + KP_HALF); v[1].vz = (int16_t)nearz;
        v[2].vx = (int16_t)(px - KP_HALF); v[2].vz = (int16_t)farz;
        v[3].vx = (int16_t)(px + KP_HALF); v[3].vz = (int16_t)farz;
    }
    v[0].vy = v[1].vy = KP_CAP_NEAR;
    v[2].vy = v[3].vy = KP_CAP_FAR;
    for (k = 0; k < 4; k++) v[k].pad = 0;

    kp_glow(ctx, v, 0, -1, 0, i, bright, 256);   /* -Y is up */
}

/* ---- One lit face of the keystone ------------------------------------------
   The 200x150 quad of the block's own side, glowing out along its normal. */
static void draw_face_glow(RenderContext *ctx, int i, int32_t bright) {
    int32_t fc = PLINTH[i].fc;
    SVECTOR v[4];
    int k;
    for (k = 0; k < 4; k++) v[k].pad = 0;

    if (PLINTH[i].fx == 0) {          /* YZ plane: fixed X, spanning Z */
        v[0].vx = (int16_t)fc; v[0].vy = KP_KEY_TOP; v[0].vz = KP_KEY_Z0;
        v[1].vx = (int16_t)fc; v[1].vy = KP_KEY_TOP; v[1].vz = KP_KEY_Z1;
        v[2].vx = (int16_t)fc; v[2].vy = 0;          v[2].vz = KP_KEY_Z0;
        v[3].vx = (int16_t)fc; v[3].vy = 0;          v[3].vz = KP_KEY_Z1;
        kp_glow(ctx, v, PLINTH[i].fd, 0, 0, i, bright, 256);
    } else {                          /* XY plane: fixed Z, spanning X */
        v[0].vx = KP_KEY_X0; v[0].vy = KP_KEY_TOP; v[0].vz = (int16_t)fc;
        v[1].vx = KP_KEY_X1; v[1].vy = KP_KEY_TOP; v[1].vz = (int16_t)fc;
        v[2].vx = KP_KEY_X0; v[2].vy = 0;          v[2].vz = (int16_t)fc;
        v[3].vx = KP_KEY_X1; v[3].vy = 0;          v[3].vz = (int16_t)fc;
        kp_glow(ctx, v, 0, 0, PLINTH[i].fd, i, bright, 256);
    }
}

/* The white top, once all four faces are burning. */
static void draw_top_glow(RenderContext *ctx, int32_t bright) {
    SVECTOR v[4];
    int k;
    for (k = 0; k < 4; k++) v[k].pad = 0;

    v[0].vx = KP_KEY_X0; v[0].vy = KP_KEY_TOP; v[0].vz = KP_KEY_Z0;
    v[1].vx = KP_KEY_X1; v[1].vy = KP_KEY_TOP; v[1].vz = KP_KEY_Z0;
    v[2].vx = KP_KEY_X0; v[2].vy = KP_KEY_TOP; v[2].vz = KP_KEY_Z1;
    v[3].vx = KP_KEY_X1; v[3].vy = KP_KEY_TOP; v[3].vz = KP_KEY_Z1;
    kp_glow(ctx, v, 0, -1, 0, KP_WHITE, bright, KP_TOP_BOOST);
}

/* Distance fade, matched to the room's own fog budget (KM_FOG_NEAR/FAR in
   keystone_maze.c) but scaled toward black rather than toward the fog colour —
   see kp_shade. Returns 0 when the light is past the room's cull. */
static int32_t kp_distance_fade(int32_t x, int32_t z) {
    int32_t dx = x - cam_x, dz = z - cam_z;
    int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (dist >= 2500) return 0;
    if (dist < 575) dist = 575;
    return ((2500 - dist) << 8) / (2500 - 575);
}

/* The floating "Press O to interact" sign in front of a plinth. The plane and
   the mirror both come off the approach vector — see the FIXTURE note above.
   door_draw_string_3d centres the reading axis on the coordinate passed AFTER
   adding 200, so the centre less 200 is what goes in. */
static void draw_prompt(RenderContext *ctx, int i) {
    int32_t dx = cam_x - PLINTH[i].px;
    int32_t dz = cam_z - PLINTH[i].pz;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= KP_TEXT_RADIUS) return;

    {
        int fade = 256;
        if (xz > KP_FADE_NEAR) {
            int range = KP_TEXT_RADIUS - KP_FADE_NEAR;
            int prog  = xz - KP_FADE_NEAR;
            if (prog > range) prog = range;
            fade = 256 - ((prog * 256) / range);
        }

        if (PLINTH[i].ax != 0) {
            int32_t sx = PLINTH[i].px + PLINTH[i].ax * KP_SIGN_STANDOFF;
            door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to interact",
                                sx, KP_TEXT_Y, PLINTH[i].pz - 200,
                                50, 255, 50, fade, PLINTH[i].ax > 0 ? 0 : 1,
                                TEXT_PLANE_YZ, KP_TEXT_PIXEL);
        } else {
            int32_t sz = PLINTH[i].pz + PLINTH[i].az * KP_SIGN_STANDOFF;
            door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to interact",
                                PLINTH[i].px - 200, KP_TEXT_Y, sz,
                                50, 255, 50, fade, PLINTH[i].az > 0 ? 1 : 0,
                                TEXT_PLANE_XY, KP_TEXT_PIXEL);
        }
    }
}

/* ---- The 2D picker ---------------------------------------------------------- */

static void kp_rect(RenderContext *ctx, int x, int y, int w, int h,
                    uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    {
        TILE *t = (TILE *)ctx->next_packet;
        setTile(t);
        setXY0(t, x, y);
        setWH(t, w, h);
        setRGB0(t, r, g, b);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[ot_idx], t);
        ctx->next_packet += sizeof(TILE);
    }
}

static void kp_outline(RenderContext *ctx, int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    kp_rect(ctx, x,       y,       w, 2, r, g, b, ot_idx);
    kp_rect(ctx, x,       y+h-2,   w, 2, r, g, b, ot_idx);
    kp_rect(ctx, x,       y,       2, h, r, g, b, ot_idx);
    kp_rect(ctx, x+w-2,   y,       2, h, r, g, b, ot_idx);
}

/* The same double outline the stove board and the trick-drawers reticule use. */
static void kp_cursor(RenderContext *ctx, int x, int y, int w, int h) {
    kp_outline(ctx, x - 4, y - 4, w + 8, h + 8, 80, 80, 200, KP_OT_CURSOR);
    kp_outline(ctx, x - 2, y - 2, w + 4, h + 4, 180, 180, 255, KP_OT_CURSOR);
}

static void draw_picker(RenderContext *ctx) {
    int s;

    /* Reset the texture window before any icon: the room sorts a 128x128 window
       at OT_LENGTH-1 which is still active down here and would wrap the icons'
       UVs (see the same note in menu_draw). */
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            RECT full = {0, 0, 0, 0};
            DR_TWIN *tw = (DR_TWIN *)ctx->next_packet;
            setTexWindow(tw, &full);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[KP_OT_TEXWIN], tw);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    kp_rect(ctx, PICK_X, PICK_Y, PICK_W, PICK_H, 15, 12, 20, KP_OT_PANEL);
    kp_outline(ctx, PICK_X, PICK_Y, PICK_W, PICK_H, 80, 80, 80, KP_OT_LINE);
    btn_prompt_draw(ctx, PICK_X + 8, PICK_Y + 6, "PLACE AN ITEM", KP_OT_TEXT);

    for (s = 0; s < MENU_ITEM_SLOTS; s++) {
        int cx = PICK_GRID_X + (s % PICK_COLS) * PICK_CELL;
        int cy = PICK_GRID_Y + (s / PICK_COLS) * PICK_CELL;
        kp_rect(ctx, cx, cy, PICK_CELL, PICK_CELL, 35, 30, 45, KP_OT_PANEL);
        kp_outline(ctx, cx, cy, PICK_CELL, PICK_CELL, 80, 70, 100, KP_OT_LINE);
        menu_draw_item_icon(ctx, s, cx + PICK_PAD, cy + PICK_PAD,
                            PICK_ICON, KP_OT_ICON);
    }

    {
        int cx = PICK_GRID_X + (pick_cur % PICK_COLS) * PICK_CELL;
        int cy = PICK_GRID_Y + (pick_cur / PICK_COLS) * PICK_CELL;
        kp_cursor(ctx, cx, cy, PICK_CELL, PICK_CELL);
    }

    btn_prompt_draw(ctx, PICK_X + 8, PICK_NAME_Y,
                    menu_item_held(pick_cur) ? menu_item_name(pick_cur) : "Empty",
                    KP_OT_TEXT);
    btn_prompt_draw(ctx, 8, 206, BTN_CIRCLE " - Place  " BTN_CROSS " - Back",
                    KP_OT_TEXT);
}

void keystone_plinths_draw(RenderContext *ctx) {
    int i;

    /* The alcove plinths, and the keystone faces they hand their light to. Both
       are drawn from the same per-plinth level pair, so the cross-fade needs no
       state of its own down here. */
    for (i = 0; i < KP_COUNT; i++) {
        int32_t lvl = plinth_level(i);
        if (lvl > 0) {
            int32_t ff = kp_distance_fade(PLINTH[i].px, PLINTH[i].pz);
            if (ff > 0) draw_plinth_glow(ctx, i, (lvl * ff) >> 8);
        }
        lvl = face_level(i);
        if (lvl > 0) {
            int32_t ff = kp_distance_fade((KP_KEY_X0 + KP_KEY_X1) / 2,
                                          (KP_KEY_Z0 + KP_KEY_Z1) / 2);
            if (ff > 0) draw_face_glow(ctx, i, (lvl * ff) >> 8);
        }
    }

    if (top_level > 0) {
        int32_t ff = kp_distance_fade((KP_KEY_X0 + KP_KEY_X1) / 2,
                                      (KP_KEY_Z0 + KP_KEY_Z1) / 2);
        if (ff > 0) draw_top_glow(ctx, ((int32_t)top_level * ff) >> 8);
    }

    /* Prompts are free play only — during the picker the camera is on one plinth
       and the rest are not interactive, and a placed plinth is finished with. */
    if (state == KP_IDLE)
        for (i = 0; i < KP_COUNT; i++)
            if (!placed(i)) draw_prompt(ctx, i);

    if (state == KP_PICKER) draw_picker(ctx);
}
