#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"          /* game_flag, player_hatch_keys, show_pickup_msg_raw */
#include "menu.h"            /* MENU_SLOT_*, the shared item icons and names      */
#include "sound.h"
#include "btn_glyph.h"       /* BTN_*, btn_prompt_draw                            */
#include "door.h"            /* door_draw_string_3d_yaw, DOOR_PIXEL_SIZE          */
#include "title.h"
#include "hatch_doors.h"
#include "hatch_puzzle.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* The room the player is in. NOT game_state — the same distinction
   src/valve_puzzle.c and src/hatch_doors.c draw. */
extern GameState current_area;

/* ---- Reach and the sign ----------------------------------------------------
   MEASURED FROM THE PIT'S SOUTH LIP, and from nothing else. hatch_doors.h owns
   the three numbers (HATCH_LIP_X/Z and HATCH_TRIGGER_RADIUS) because the leaves
   and this module have to agree about where the interaction is: a prompt that
   appears where the press does not work is worse than no prompt.

   +Z is north in this room, so the lip is the pit's NEAR edge as the player
   comes down the corridor from the west gate. They cannot get onto the pit —
   collision walls 20..23 fence it — so the nearest they ever stand is the
   default 195 wall radius back from z=-300, and 600 of reach leaves plenty of
   slack on that. Nothing else in the room is within reach of it: the west gate
   is at x=-200, 3800 away. */
#define HP_TEXT_RADIUS    1200
#define HP_FADE_NEAR       800

/* = TH_TEXT_Y, the height every sign in The Hatch stands at over its y=0 lawn:
   the glyph TOP, so the line hangs at eye level rather than over the player's
   head. Not shared through a header because it is one number that has been the
   same in every garden room since Maze One. */
#define HP_TEXT_Y        (-186)

/* The line floats just south of the leaves' own south edge (z=-310), between it
   and the closest the fence lets the player stand (z=-495). It reads over the
   lip rather than over the grass in front of it. x is the doors' centre line, so
   the line is centred on the hatch.

   FACING SOUTH, back at a player on the lawn below the pit: the yaw handed to
   door_draw_string_3d_yaw is the direction the VIEWER is looking, and a player
   who has walked up to the hole and turned to face it is looking north, i.e.
   +Z, i.e. cam_rot 0. FIXED rather than cam_rot itself, so the sign belongs to
   the pit's south edge and is read edge-on from anywhere else. */
#define HP_TEXT_X         3600
#define HP_TEXT_Z        (-350)
#define HP_TEXT_YAW          0

/* ---- The shot --------------------------------------------------------------
   THE VALVE PUZZLE'S GEOMETRY, SOLVED FOR A MUCH BIGGER OBJECT AND THEN SWUNG
   ROUND. The camera stands above the yard south-EAST of the pit and looks back
   down and across it, so the hole is read at a three-quarter angle rather than
   square on. It is the only shot in the room that is not axis-aligned, and it is
   deliberate: a 1200 x 600 rectangle photographed dead-on from its short side
   reads as a flat band, and the doors' two leaves are only told apart by the
   line between them.

   THE NUMBERS ARE ONE PIECE OF ARITHMETIC AND NOT TASTE VALUES:

     FRAMING     the pit is 1200 wide in x. At gte_SetGeomScreen(256) on a
                 320-wide screen a span S at distance D covers S*256/D pixels, so
                 to sit the hole across ~200 of the 320 the lens wants to be
                 1200*256/200 = 1536 out.
     THE TILT    1536 broken at 40deg (455 of 4096) gives up = 1536*sin40 = 987
                 and a GROUND RUN of 1536*cos40 = 1177. Rounded to 990 and 1180.
                 Those two are what the pitch is, and swinging the camera round
                 the pit does not touch either: only the compass bearing of the
                 1180 changes, so the framing and HP_CAM_PITCH survive the swing.
     THE SWING   that 1180 of ground is laid out on a bearing 35deg east of due
                 south — east = 1180*sin35 = 677, south = 1180*cos35 = 967 —
                 which puts the lens at (4277, -990, -967).
     THE AIM     the camera looks back along that bearing at the pit's centre
                 (3600, 0, 0), i.e. along (-677, +967). Yaw runs from +Z toward
                 +X, so that is atan2(-677, 967) = -35deg = 4096 - 398 = 3698.
                 >>> THE SWING AND THE YAW ARE THE SAME 35deg. <<< Change one
                 and the shot stops pointing at the hole.

   >>> THE SPOT WAS CHECKED AGAINST THE ROOM, AND IT HAS TO BE. <<< (4277, -990,
   -967) is over the YARD's open lawn — the yard is x(1800,4800) z(-1499,1500),
   and the nearest of the four corner blocks it is bitten by is the SE one at
   x(4400,4800) z(-1499,-1099), which x=4277 is 123 clear of on one axis and
   z=-967 is 132 clear of on the other. The lens sits 990 above a y=0 lawn under
   a hedge line drawn to -500, so it is ABOVE the hedges looking down into the
   yard: at 40deg of pitch against a half-field of atan(120/256) = 25deg, the TOP
   of frame is still 15deg below the horizon, so the shot is all ground and no
   sky. Re-export the room and this paragraph has to be re-checked, not assumed. */
#define HP_CAM_BACK         967   /* south of the pit's centre */
#define HP_CAM_EAST         677   /* ...and east of it         */
#define HP_CAM_UP           990
#define HP_CAM_PITCH        455
#define HP_CAM_YAW         3698   /* 35deg west of north: back along the bearing */
#define HP_CAM_ANIM_FRAMES   30   /* slower than the valve's 24: it travels
                                     ten times as far to get there            */

/* How long each log beat is held with the shot still up, and how long the pair
   of leaves is watched after the second key turns. The swing itself is 9 x
   HATCH_DOORS_ANIM_TICKS = 54 frames; the extra is a breath on the open hole
   before the camera hands back. */
#define HP_MSG_FRAMES       110
#define HP_DOORS_FRAMES     100

/* >>> THE SECOND KEY'S UNLOCK IS ALLOWED TO FINISH BEFORE THE LEAVES MOVE. <<<
   The two used to start on the same frame and the swing walked all over the
   tail of the lock turning. This is the length of that tail, MEASURED off the
   clip rather than guessed: sounds/unlock.vag is a 26640-byte VAG body, and SPU
   ADPCM packs 28 samples into each 16-byte block, so it is 26640/16 x 28 =
   46620 samples at the 22050 Hz its header declares — 2.11 seconds, or 127
   frames at 60. 130 clears it with a few frames of air.

   >>> IT IS NOT THE HOUSE 11025. <<< Halve the rate on a re-cut of that clip
   (tools/ADDING_A_SOUND.txt notes twice that a bank has been paid for exactly
   that way) and the sound becomes 4.2 seconds and this number doubles. */
#define HP_UNLOCK_FRAMES    130

/* ---- The descent -----------------------------------------------------------
   A SCRIPTED HEAD, then a jump. Nothing here is interactive — it is the payoff
   for a press made in front of an open hole, and there is nothing to skip to.

   THE WALK ends at z = -320, which is 20 short of the pit's south lip and a good
   175 past the closest the collision fence lets the player WALK. That is the
   point of it being a cutscene: the camera goes where the player's own feet
   cannot, right to the edge. It is spent entirely inside the FIRST beat below,
   so everything after that is the head moving over a body standing still.

   THE FALL stops at y = 1050. The shaft's black floor is at y=1200 and the
   camera is an eye rather than a body, so 1050 is "near the bottom" with the
   floor still below frame — which is where the boss room is meant to take over.
   See the placeholder note in hatch_puzzle.h. */
#define HP_FALL_FRAMES       60

#define HP_EDGE_Z         (-320)
#define HP_FALL_Y          1050

/* THE TWO PITCHES THE WHOLE LOOK IS BUILT OUT OF. "Forward" is wherever the
   player's own head was on the frame they pressed — captured, not assumed zero,
   so the scene starts from the pose it inherits. "Down" is the look INTO the
   hole: standing at z=-320 with the shaft floor 1200 below and 600 of pit
   running away north, a line onto the middle of that floor is atan(1200/300) =
   76deg, which is steeper than reads as looking rather than falling. 750 (66deg)
   puts the far wall and the floor in frame together, which is what "there is a
   long way down there" looks like. */
#define HP_LOOK_DOWN        750
#define HP_FALL_PITCH       900   /* very nearly straight down (1024 = down)  */

/* ---- Board layout (320x240) ------------------------------------------------
   THE VALVE PUZZLE'S BOARD, number for number. It is the same question asked of
   the same inventory — put an item in the box, press USE — and two puzzles that
   look different while doing the same thing would be a lie. See the note on the
   column in valve_puzzle.c: the whole of it sits above y=150 so the HUD log box
   at y=191 is clear of the selection cursor. */
#define HP_BOX_X             244
#define HP_BOX_W              56
#define HP_BOX_H              56
#define HP_BOX_Y              58
#define HP_USE_Y             122
#define HP_USE_H              28   /* half height */
#define HP_BOX_ICON           40

#define HP_PICK_X             24
#define HP_PICK_Y             30
#define HP_PICK_W            168
#define HP_PICK_H            168
#define HP_PICK_CELL          42
#define HP_PICK_ICON          30
#define HP_PICK_PAD            6
#define HP_PICK_COLS           4
#define HP_PICK_ROWS  ((MENU_ITEM_SLOTS + HP_PICK_COLS - 1) / HP_PICK_COLS)
#define HP_PICK_GRID_X  (HP_PICK_X + (HP_PICK_W - HP_PICK_COLS * HP_PICK_CELL) / 2)
#define HP_PICK_GRID_Y  (HP_PICK_Y + 22)
#define HP_PICK_NAME_Y  (HP_PICK_Y + HP_PICK_H - 16)

/* OT layers — all inside the menu-reserved range (0..SCENE_OT_MIN-1), so the
   board always sits on top of the room. Higher index = drawn first = further
   back (see menu.c). The valve's numbers, for the same reasons. */
#define HP_OT_PANEL           12
#define HP_OT_LINE            10
#define HP_OT_TEXWIN           8
#define HP_OT_ICON             7
#define HP_OT_CURSOR           3
#define HP_OT_TEXT             1

typedef enum {
    HP_IDLE = 0,   /* not in the puzzle: the sign, and the Circle trigger     */
    HP_INTRO,      /* camera panning up and back to the fixed shot            */
    HP_BOARD,      /* board live: the item box and USE                        */
    HP_PICKER,     /* item picker open                                        */
    HP_MSG,        /* a key has turned: its line, held with the board gone    */
    HP_UNLOCK,     /* ...the SECOND key: its lock turning, before the leaves  */
    HP_DOORS,      /* the leaves swinging under the same shot                 */
    HP_LOOK,       /* the descent: up to the lip, then the scripted head      */
    HP_FALL        /* ...and the jump itself                                  */
} HpState;

/* ---- The descent's script --------------------------------------------------
   ONE TABLE, READ TOP TO BOTTOM, and it is the whole of the look. Each row is a
   beat: how long it runs, whether it ends looking DOWN the hole or FORWARD
   again, and how it gets there. The pitch a beat starts from is whatever the row
   above left, so the column reads as a sequence of poses rather than a list of
   deltas and a beat can be re-timed without touching its neighbours.

   >>> ONLY THE FIRST BEAT MOVES THE CAMERA. <<< The walk up to the lip and the
   turn onto north are both spent inside it; every row after it is a head turning
   over a body that has stopped. That is why the first row is the only one whose
   duration changes where the camera ends up rather than merely how long it takes
   to look.

   The last row is the pause before the jump: it ends FORWARD and starts there
   too, so its curve never comes into it — the head is simply held, facing out
   over the hole, for two seconds. */
typedef enum { HP_LIN = 0, HP_SMOOTH } HpCurve;

typedef struct {
    int16_t frames;    /* at 60 fps                                          */
    uint8_t down;      /* 1 = this beat ends looking down the hole           */
    uint8_t curve;     /* HP_LIN is abrupt at both ends; HP_SMOOTH eases both */
} HpBeat;

static const HpBeat HP_SCRIPT[] = {
    { 120, 0, HP_SMOOTH },  /* 2.0s  look forward, closing on the edge      */
    { 180, 1, HP_SMOOTH },  /* 3.0s  pan down into the hole                 */
    {  30, 0, HP_LIN    },  /* 0.5s  SHARPLY back to forward — linear, so it
                                     starts at full speed instead of creeping
                                     out of the pose it is leaving           */
    { 120, 1, HP_SMOOTH },  /* 2.0s  pan down again                         */
    {  90, 0, HP_SMOOTH },  /* 1.5s  slowly back to forward                 */
    { 120, 0, HP_LIN    },  /* 2.0s  hold, and then jump                    */
};
#define HP_SCRIPT_BEATS ((int)(sizeof HP_SCRIPT / sizeof HP_SCRIPT[0]))

static HpState state     = HP_IDLE;
static int     box_item  = -1;   /* MENU_SLOT_* in the item box, or -1 */
static int     board_cur = 0;    /* 0 = the item box, 1 = USE          */
static int     pick_cur  = 0;
static int     timer     = 0;
static int     drop_done = 0;    /* one frame, at the bottom of the shaft */

/* Where the descent has got to: which row of HP_SCRIPT is running, and the
   pitch the row above left behind (which is what this one eases FROM). */
static int     beat      = 0;
static int32_t beat_from = 0;
static int32_t fwd_pitch = 0;    /* "forward": the head the player pressed with */

/* Pre-puzzle camera, restored on the way out of anything that is not the drop. */
static int32_t save_cx, save_cy, save_cz, save_crot, save_cvy;
static int32_t cam_anim_t = 0;
static int32_t src_x, src_y, src_z, src_rot, src_pitch, rot_delta;

static uint16_t btn_prev      = 0xFFFF;  /* board input edge-detect (btn = ~pad) */
static int      interact_prev = 1;       /* interact-Circle edge-detect          */

int hatch_puzzle_active(void)    { return state != HP_IDLE; }
int hatch_puzzle_drop_done(void) { return drop_done; }

/* ---- State transitions ---------------------------------------------------- */

/* Shared by the board and the drop: both take the camera, and both leave the
   player standing exactly where they pressed. The player is PINNED rather than
   moved, so anything hunting them hunts the real spot and the camera has an
   honest place to come back to — the valve's anchor and the stove's. */
static void take_camera(int32_t target_yaw) {
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    camera_anchor_player(save_cx, save_cy, save_cz);

    src_x = cam_x; src_y = cam_y; src_z = cam_z;
    src_rot = cam_rot; src_pitch = cam_pitch;
    {
        /* Shortest signed turn onto the target. The two scenes want different
           ones: the BOARD swings round to the three-quarter bearing
           (HP_CAM_YAW) and the DROP faces straight north at the hole (0). */
        int32_t d = ((target_yaw - src_rot) % 4096 + 4096) % 4096;
        if (d > 2048) d -= 4096;
        rot_delta = d;
    }
    cam_anim_t = 0;
}

/* Drop the scripted camera and put the player back exactly where they stood. */
static void exit_puzzle(void) {
    state    = HP_IDLE;
    box_item = -1;
    cam_x    = save_cx; cam_y = save_cy; cam_z = save_cz;
    cam_rot  = save_crot; cam_vy = save_cvy;
    cam_pitch = 0;
    camera_release_player();
    interact_prev = 1;   /* swallow the held Circle so it cannot re-open */
}

/* ---- Room entry ----------------------------------------------------------- */

void hatch_puzzle_arm(void) {
    interact_prev = interact_tapped();
    state         = HP_IDLE;
    box_item      = -1;
    board_cur     = 0;
    timer         = 0;
    drop_done     = 0;
    beat          = 0;
    beat_from     = 0;
    camera_release_player();
}

void hatch_puzzle_reset(void) {
    hatch_puzzle_arm();
    interact_prev = 1;
}

/* ---- Interaction ---------------------------------------------------------- */

/* Manhattan reach plus a facing test, as every other Circle prompt in the
   garden uses: a hole behind you is not one you are working at. */
static int lip_in_reach(void) {
    int32_t dx = cam_x - HATCH_LIP_X;
    int32_t dz = cam_z - HATCH_LIP_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < HATCH_TRIGGER_RADIUS && interact_facing(HATCH_LIP_X, HATCH_LIP_Z);
}

/* What the board says as it arrives. Both keyholes are described every time —
   the player is being told what is in front of them, not given a running score —
   and the second sentence is only added once one has been turned. */
static const char *keyhole_line(void) {
    if (game_flag(FLAG_HATCH_LOCK_ONE))
        return "A hatch. There are 2 keyholes. One is unlocked.";
    return "A hatch. There are 2 keyholes";
}

/* A Hatch Key turns. BOTH KEYS BREAK, so this always spends one, and the board
   is cleared either way: after the first there may be no second key in hand to
   put back in the box, and leaving a spent icon sitting there would read as a
   key the player still has. */
static void turn_key(void) {
    sound_play(SFX_UNLOCK);
    if (player_hatch_keys > 0) player_hatch_keys--;
    /* The pause menu's grid catches up now rather than waiting for the next
       menu_open, so a player who pauses on the way out does not find a key they
       no longer have. The valve's retire and the stove's cook take this route. */
    menu_inventory_sync();
    box_item = -1;

    if (!game_flag(FLAG_HATCH_LOCK_ONE)) {
        game_flag_set(FLAG_HATCH_LOCK_ONE);
        show_pickup_msg_raw("You unlocked one side. The key broke in the keyhole!");
        /* THE PUZZLE STAYS OPEN. A player carrying both keys works the second
           keyhole without leaving the shot; one carrying only the first backs
           out with Cross and comes back when they find the other. */
        state = HP_MSG;
        timer = HP_MSG_FRAMES;
        return;
    }

    show_pickup_msg_raw("You unlocked the other side. This key broke too!");
    /* >>> THE LEAVES DO NOT MOVE YET. <<< The lock is allowed to finish turning
       first — HP_UNLOCK holds the shot for the length of the clip and the swing
       starts when it ends, so the two are heard one after the other rather than
       the doors trampling the tail of the unlock. The line above is posted NOW,
       with the sound, because it is what the sound means. */
    state = HP_UNLOCK;
    timer = HP_UNLOCK_FRAMES;
}

static int board_update(uint16_t btn);   /* the two board states, below */

/* ---- The descent's easing --------------------------------------------------
   Two curves over a 0..256 progress, and the choice between them is what makes
   a beat read as a deliberate look or as a flinch.

   HP_SMOOTH is ease-in-out — it leaves the pose it is holding gently and
   arrives at the next one gently, which is a head being turned on purpose. It
   is built from the ease-out the rest of this file uses, mirrored about the
   halfway point rather than being a second formula.

   HP_LIN is a straight ramp: full speed on the first frame and a dead stop on
   the last. On the 30-frame beat that snaps the head back up from the hole that
   abruptness IS the beat — an eased version of the same half second reads as a
   slow start rather than a jerk. */
static int32_t hp_ease(int32_t t, int curve) {   /* t 0..256 -> 0..256 */
    if (curve == HP_LIN) return t;
    if (t < 128) {                     /* ease IN: the mirrored half   */
        int32_t u = t * 2;             /* 0..256 over the first half   */
        return (u * u / 256) / 2;
    }
    {                                  /* ease OUT: the plain half     */
        int32_t u = (256 - t) * 2;
        return 256 - ((u * u / 256) / 2);
    }
}

/* The pitch a beat is easing TOWARDS. Two poses only — see HP_LOOK_DOWN. */
static int32_t beat_target(int b) {
    return HP_SCRIPT[b].down ? HP_LOOK_DOWN : fwd_pitch;
}

int hatch_puzzle_update(int lock) {
    uint16_t btn = 0;
    if (pad_buff_len[0]) { PadResponse *pad = (PadResponse *)pad_buff[0]; btn = ~pad->btn; }

    drop_done = 0;

    if (state == HP_IDLE) {
        /* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, and that is the
           point of reading it before the lock test rather than after: a Circle
           held down across the menu closing must not read as a fresh press on
           the frame the lock lifts. */
        int held = interact_tapped();
        int just = held && !interact_prev;
        interact_prev = held;
        if (lock) return 0;
        if (!just || !lip_in_reach()) return 0;

        if (hatch_doors_open()) {
            /* The hole is open: the press is the drop, not the board, and the
               descent walks straight at the lip — yaw 0, due north. */
            take_camera(0);
            /* The script's poses are measured off the head the press was made
               with, so "forward" is captured here and not assumed to be level. */
            fwd_pitch = cam_pitch;
            beat      = 0;
            beat_from = fwd_pitch;
            state     = HP_LOOK;
        } else {
            take_camera(HP_CAM_YAW);
            state     = HP_INTRO;
            board_cur = 0;
            box_item  = -1;
        }
        return 1;              /* the tap is ours: the gate must not see it */
    }

    /* Camera pans up and back to the fixed shot; the board opens once it
       settles, and the log states what the player is looking at. */
    if (state == HP_INTRO) {
        cam_anim_t++;
        int32_t t = cam_anim_t * 256 / HP_CAM_ANIM_FRAMES; if (t > 256) t = 256;
        int32_t inv = 256 - t;
        int32_t e = 256 - (inv * inv / 256);            /* ease-out 0..256 */

        int32_t tx = HATCH_LIP_X + HP_CAM_EAST;         /* east of the centre  */
        int32_t ty = -HP_CAM_UP;
        int32_t tz = -HP_CAM_BACK;                      /* south of the centre */

        cam_x     = src_x   + ((tx - src_x) * e) / 256;
        cam_y     = src_y   + ((ty - src_y) * e) / 256;
        cam_z     = src_z   + ((tz - src_z) * e) / 256;
        cam_rot   = src_rot + (rot_delta * e) / 256;
        cam_pitch = src_pitch + ((HP_CAM_PITCH - src_pitch) * e) / 256;
        cam_vy    = 0;
        if (cam_anim_t >= HP_CAM_ANIM_FRAMES) {
            cam_x = tx; cam_y = ty; cam_z = tz;
            cam_rot = HP_CAM_YAW; cam_pitch = HP_CAM_PITCH; cam_vy = 0;
            state = HP_BOARD;
            btn_prev = btn;   /* arm: the held Circle is not a select */
            show_pickup_msg_raw(keyhole_line());
        }
        return 1;
    }

    /* The first key's line, held with the shot up and the board hidden. Then the
       board comes back for the second keyhole. */
    if (state == HP_MSG) {
        if (--timer <= 0) {
            state     = HP_BOARD;
            board_cur = 0;
            btn_prev  = btn;   /* a Circle held through the beat is not a select */
        }
        return 1;
    }

    /* The second key's lock, turning. Nothing moves — the shot is held while the
       clip plays out, and the leaves are thrown on the frame it ends. See
       HP_UNLOCK_FRAMES for where that length comes from. */
    if (state == HP_UNLOCK) {
        if (--timer <= 0) {
            /* hatch_doors_begin_open() sets FLAG_HATCH_DOORS_OPEN itself, on the
               frame the swing starts — see hatch_doors.h. That bit is also what
               stands for "both keyholes are turned", which is why there is no
               third flag for it. */
            hatch_doors_begin_open();
            state = HP_DOORS;
            timer = HP_DOORS_FRAMES;
        }
        return 1;
    }

    /* The leaves swinging, under the same fixed shot. No input: this is the
       payoff for the second key and there is nothing to skip to. */
    if (state == HP_DOORS) {
        if (--timer <= 0) exit_puzzle();
        return 1;
    }

    /* The board and the picker, which are the only two states that read the
       pad. Everything below this line is the descent and is scripted. */
    if (state == HP_BOARD || state == HP_PICKER) return board_update(btn);

    /* ---- The descent's script. Scripted throughout; the pad is not read. ----
       One row of HP_SCRIPT at a time. The BODY is carried entirely by the first
       row — the walk up to the lip and the turn onto north both finish with it —
       and every row after that only turns the head, which is why the position
       lerp is guarded on `beat == 0` rather than run against a target it has
       already reached. */
    if (state == HP_LOOK) {
        const HpBeat *b = &HP_SCRIPT[beat];
        cam_anim_t++;
        int32_t t = cam_anim_t * 256 / b->frames; if (t > 256) t = 256;
        int32_t e = hp_ease(t, b->curve);

        if (beat == 0) {
            /* Up to the edge, and round onto north. Eased on the SAME curve as
               the head, so the body arrives as the first look begins rather
               than still drifting under it. */
            cam_x   = src_x + ((HATCH_LIP_X - src_x) * e) / 256;
            cam_z   = src_z + ((HP_EDGE_Z   - src_z) * e) / 256;
            cam_y   = src_y;
            cam_rot = src_rot + (rot_delta * e) / 256;
        }
        cam_pitch = beat_from + ((beat_target(beat) - beat_from) * e) / 256;
        cam_vy    = 0;

        if (cam_anim_t >= b->frames) {
            cam_pitch  = beat_target(beat);
            beat_from  = cam_pitch;        /* the next row eases FROM here */
            cam_anim_t = 0;
            if (beat == 0) {
                /* Snap the body onto its marks once, so 240 frames of integer
                   easing cannot leave it a unit or two short of the lip. */
                cam_x = HATCH_LIP_X; cam_z = HP_EDGE_Z; cam_rot = 0;
            }
            if (++beat >= HP_SCRIPT_BEATS) {
                src_y     = cam_y;         /* the height the fall starts from */
                src_z     = cam_z;
                src_pitch = cam_pitch;
                state     = HP_FALL;
            }
        }
        return 1;
    }

    /* The jump. The step OUT over the hole is spent early and the DROP late:
       x/z ease out onto the pit's centre in the first stride while y runs t^2,
       which is what an accelerating fall looks like and what keeps the ground
       from appearing to rise at a constant rate. */
    {
        cam_anim_t++;
        int32_t t = cam_anim_t * 256 / HP_FALL_FRAMES; if (t > 256) t = 256;
        int32_t inv = 256 - t;
        int32_t out = 256 - (inv * inv / 256);          /* ease-out: the step  */
        int32_t acc = (t * t) / 256;                    /* t^2: the fall       */

        cam_x     = HATCH_LIP_X;
        cam_z     = src_z + ((0 - src_z) * out) / 256;  /* onto the pit centre */
        cam_y     = src_y + ((HP_FALL_Y - src_y) * acc) / 256;
        cam_rot   = 0;
        cam_pitch = src_pitch + ((HP_FALL_PITCH - src_pitch) * acc) / 256;
        cam_vy    = 0;
        if (cam_anim_t >= HP_FALL_FRAMES) {
            /* NEAR THE BOTTOM. The camera is NOT put back — the transition
               main.c runs off this covers the screen with the door animation and
               rebuilds the room from its own spawn. Only the player anchor is
               given back, because nothing else would. */
            camera_release_player();
            state         = HP_IDLE;
            interact_prev = 1;
            drop_done     = 1;
        }
        return 1;
    }
}

/* ---- The board's input ----------------------------------------------------
   Split out of the update above only because that one had grown two scenes; the
   two board states are the valve's, key for key. */
static int board_update(uint16_t btn) {
    uint16_t pressed = btn & ~btn_prev;
    btn_prev = btn;

    if (state == HP_BOARD) {
        /* Guarded at both ends so a press against either is silent. */
        if ((pressed & PAD_UP)   && board_cur > 0) { board_cur--; sound_play(SFX_CURSOR); }
        if ((pressed & PAD_DOWN) && board_cur < 1) { board_cur++; sound_play(SFX_CURSOR); }
        if (pressed & PAD_CROSS)  { sound_play(SFX_BACK); exit_puzzle(); return 1; }
        if (pressed & PAD_CIRCLE) {
            /* USE is confirmed here whether or not the item is right: on a
               success the unlock is the next thing heard anyway, and on a
               failure there is no cue at all beyond the log line, so this blip
               is the acknowledgement the press gets either way. */
            sound_play(SFX_SELECT);
            if (board_cur == 1) {
                if (box_item != MENU_SLOT_HATCH_KEY || player_hatch_keys <= 0)
                    show_pickup_msg_raw("That doesn't make sense...");
                else
                    turn_key();
            } else {
                pick_cur = box_item >= 0 ? box_item : 0;
                state    = HP_PICKER;
            }
        }
        return 1;
    }

    /* HP_PICKER: the stove's grid over every inventory slot. Choosing an item
       COPIES it into the box — nothing is spent until USE is pressed, and a
       player who backs out keeps everything. The trailing cells of the last row
       may be past MENU_ITEM_SLOTS; the guard below keeps the cursor out of them
       and the draw loop never paints them. */
    {
        int row = pick_cur / HP_PICK_COLS, col = pick_cur % HP_PICK_COLS;
        if (pressed & PAD_UP)    { if (row > 0) row--; }
        if (pressed & PAD_DOWN)  { if (row < HP_PICK_ROWS - 1) row++; }
        if (pressed & PAD_LEFT)  { if (col > 0) col--; }
        if (pressed & PAD_RIGHT) { if (col < HP_PICK_COLS - 1) col++; }
        int next = row * HP_PICK_COLS + col;
        if (next < MENU_ITEM_SLOTS && next != pick_cur) {
            pick_cur = next;
            sound_play(SFX_CURSOR);
        }

        if (pressed & PAD_CROSS) { sound_play(SFX_BACK); state = HP_BOARD; return 1; }
        if ((pressed & PAD_CIRCLE) && menu_item_held(pick_cur)) {
            box_item = pick_cur;
            state    = HP_BOARD;
            sound_play(SFX_SELECT);
        }
    }
    return 1;
}

/* ---- The floating sign ----------------------------------------------------
   ONE sign, two lines, and which one it reads is the whole state of the puzzle
   as far as the player standing on the lawn is concerned: the leaves are either
   shut, in which case Circle opens the keyholes' board, or open, in which case
   Circle is the drop. Taken off entirely while they are MOVING — offering
   anything over doors that are already swinging reads as a press that did not
   take. */
void hatch_puzzle_text(RenderContext *ctx) {
    if (state != HP_IDLE) return;          /* the board owns the screen */
    if (hatch_doors_swinging()) return;

    int32_t dx = cam_x - HATCH_LIP_X;
    int32_t dz = cam_z - HATCH_LIP_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= HP_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > HP_FADE_NEAR) {
        int range = HP_TEXT_RADIUS - HP_FADE_NEAR;
        int prog  = xz - HP_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* The garden's sign green. The Circle control code inside the string
       supplies its own red and the fade still applies to both. */
    const char *line = hatch_doors_open() ? "Press " BTN_CIRCLE " to drop"
                                          : "Press " BTN_CIRCLE " to interact";
    door_draw_string_3d_yaw(ctx, line, HP_TEXT_X, HP_TEXT_Y, HP_TEXT_Z,
                            50, 255, 50, fade, HP_TEXT_YAW, DOOR_PIXEL_SIZE);
}

/* ---- The board ------------------------------------------------------------ */

static void hp_rect(RenderContext *ctx, int x, int y, int w, int h,
                    uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    TILE *t = (TILE *)ctx->next_packet;
    setTile(t);
    setXY0(t, x, y);
    setWH(t, w, h);
    setRGB0(t, r, g, b);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[ot_idx], t);
    ctx->next_packet += sizeof(TILE);
}

static void hp_outline(RenderContext *ctx, int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    hp_rect(ctx, x,       y,       w, 2, r, g, b, ot_idx);
    hp_rect(ctx, x,       y+h-2,   w, 2, r, g, b, ot_idx);
    hp_rect(ctx, x,       y,       2, h, r, g, b, ot_idx);
    hp_rect(ctx, x+w-2,   y,       2, h, r, g, b, ot_idx);
}

static void hp_cursor(RenderContext *ctx, int x, int y, int w, int h) {
    hp_outline(ctx, x - 4, y - 4, w + 8, h + 8, 80, 80, 200, HP_OT_CURSOR);
    hp_outline(ctx, x - 2, y - 2, w + 4, h + 4, 180, 180, 255, HP_OT_CURSOR);
}

void hatch_puzzle_draw(RenderContext *ctx) {
    /* Nothing while the camera is still flying, nothing over a key's log beat,
       and nothing over the swing or the descent: those are the room and the
       leaves, with no overlay on them at all. */
    if (state != HP_BOARD && state != HP_PICKER) return;

    /* Reset the texture window before any icon: this room sorts a 128x128 window
       at OT_LENGTH-1 which is still active down here and would wrap the icons'
       UVs (the same note as in menu_draw and valve_puzzle_draw). */
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            RECT full = {0, 0, 0, 0};
            DR_TWIN *tw = (DR_TWIN *)ctx->next_packet;
            setTexWindow(tw, &full);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[HP_OT_TEXWIN], tw);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    /* --- The item box and USE, on the right --- */
    hp_rect(ctx, HP_BOX_X, HP_BOX_Y, HP_BOX_W, HP_BOX_H, 35, 30, 45, HP_OT_PANEL);
    hp_outline(ctx, HP_BOX_X, HP_BOX_Y, HP_BOX_W, HP_BOX_H, 80, 70, 100, HP_OT_LINE);
    if (box_item >= 0)
        menu_draw_item_icon(ctx, box_item,
                            HP_BOX_X + (HP_BOX_W - HP_BOX_ICON) / 2,
                            HP_BOX_Y + (HP_BOX_H - HP_BOX_ICON) / 2,
                            HP_BOX_ICON, HP_OT_ICON);

    hp_rect(ctx, HP_BOX_X, HP_USE_Y, HP_BOX_W, HP_USE_H, 45, 25, 25, HP_OT_PANEL);
    hp_outline(ctx, HP_BOX_X, HP_USE_Y, HP_BOX_W, HP_USE_H, 120, 70, 70, HP_OT_LINE);
    /* "Use" is 3 chars at the font's 8px advance. */
    btn_prompt_draw(ctx, HP_BOX_X + (HP_BOX_W - 24) / 2,
                    HP_USE_Y + (HP_USE_H - 8) / 2, "Use", HP_OT_TEXT);

    /* --- Board cursor (hidden while the picker is up) --- */
    if (state == HP_BOARD) {
        if (board_cur == 1) hp_cursor(ctx, HP_BOX_X, HP_USE_Y, HP_BOX_W, HP_USE_H);
        else                hp_cursor(ctx, HP_BOX_X, HP_BOX_Y, HP_BOX_W, HP_BOX_H);
    }

    /* --- Item picker --- */
    if (state == HP_PICKER) {
        hp_rect(ctx, HP_PICK_X, HP_PICK_Y, HP_PICK_W, HP_PICK_H, 15, 12, 20, HP_OT_PANEL);
        hp_outline(ctx, HP_PICK_X, HP_PICK_Y, HP_PICK_W, HP_PICK_H, 80, 80, 80, HP_OT_LINE);
        btn_prompt_draw(ctx, HP_PICK_X + 8, HP_PICK_Y + 6, "ITEMS", HP_OT_TEXT);

        int s;
        for (s = 0; s < MENU_ITEM_SLOTS; s++) {
            int cx = HP_PICK_GRID_X + (s % HP_PICK_COLS) * HP_PICK_CELL;
            int cy = HP_PICK_GRID_Y + (s / HP_PICK_COLS) * HP_PICK_CELL;
            hp_rect(ctx, cx, cy, HP_PICK_CELL, HP_PICK_CELL, 35, 30, 45, HP_OT_PANEL);
            hp_outline(ctx, cx, cy, HP_PICK_CELL, HP_PICK_CELL, 80, 70, 100, HP_OT_LINE);
            menu_draw_item_icon(ctx, s, cx + HP_PICK_PAD, cy + HP_PICK_PAD,
                                HP_PICK_ICON, HP_OT_ICON);
        }

        {
            int cx = HP_PICK_GRID_X + (pick_cur % HP_PICK_COLS) * HP_PICK_CELL;
            int cy = HP_PICK_GRID_Y + (pick_cur / HP_PICK_COLS) * HP_PICK_CELL;
            hp_cursor(ctx, cx, cy, HP_PICK_CELL, HP_PICK_CELL);
        }

        {
            const char *label = menu_item_held(pick_cur) ? menu_item_name(pick_cur)
                                                         : "Empty";
            btn_prompt_draw(ctx, HP_PICK_X + 8, HP_PICK_NAME_Y, label, HP_OT_TEXT);
        }
    }

    /* --- Controls, on the trick drawers' line --- */
    if (state == HP_BOARD)
        btn_prompt_draw(ctx, 8, 206, BTN_CIRCLE " - Select  " BTN_CROSS " - Exit",
                        HP_OT_TEXT);
    else
        btn_prompt_draw(ctx, 8, 206, BTN_CIRCLE " - Select  " BTN_CROSS " - Back",
                        HP_OT_TEXT);
}
