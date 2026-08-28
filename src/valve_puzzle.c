#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"          /* player_items, game_flag, show_pickup_msg_raw   */
#include "menu.h"            /* MENU_SLOT_*, the shared item icons and names   */
#include "sound.h"
#include "btn_glyph.h"       /* BTN_*, btn_prompt_draw                          */
#include "door.h"            /* door_draw_string_3d, TEXT_PLANE_*               */
#include "title.h"
#include "item_pickup.h"
#include "birdcage.h"
#include "valve_handle.h"
#include "valve_puzzle.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* The room the player is in. NOT game_state — the same distinction
   src/valve_handle.c and src/fatdoor.c draw. */
extern GameState current_area;

/* ---- The three pipes -------------------------------------------------------
   EVERYTHING GEOMETRIC IS TAKEN FROM THE MOUNT, not authored here. A pipe's
   position, its facing, which way its wheel stands and which pipe texture it
   wears are all already stated once, in src/valve_handle.c's mount list, and
   were derived there from the room's own .smx. Duplicating any of it would mean
   a re-export could move the wheel and leave the camera, the sign and the reach
   test pointing at where it used to be.

   So this table is only the two things a MOUNT cannot know: which room the pipe
   is in and which flag records that it has been turned. */
typedef struct {
    GameState area;
    GameFlag  flag;
} VpPipe;

#define VP_PIPE_COUNT 3
static const VpPipe VP_PIPES[VP_PIPE_COUNT] = {
    { STATE_MAZE_ONE,   FLAG_VALVE_MAZE_ONE   },
    { STATE_MAZE_TWO,   FLAG_VALVE_MAZE_TWO   },
    { STATE_CHAIN_ROOM, FLAG_VALVE_CHAIN_ROOM },
};

/* ---- Reach -----------------------------------------------------------------
   THE GREENHOUSE'S 450, and for the same reason it is 450 there: the drawn pipe
   is a 50-unit column but what stops the player is the hedge run or the wall
   BEHIND it, so the closest they can physically stand is a good 250 out. 450
   leaves slack on that and still cannot reach anything else — the nearest gate
   to any of the three pipes is thousands of units away. */
#define VP_TRIGGER_RADIUS   450
#define VP_TEXT_RADIUS     1300
#define VP_FADE_NEAR        900
/* The sign is smaller than a door sign: "Press O to interact" is 20 cells, which
   at DOOR_PIXEL_SIZE (4) would be 480 units wide — most of the width of a maze
   corridor. At 3 it is 360 and clears the hedges either side. */
#define VP_TEXT_PIXEL          3
#define VP_TEXT_ROWS_H        21   /* 7 font rows at VP_TEXT_PIXEL              */
#define VP_SIGN_CLEAR         60   /* air between the glyph bottom and the wheel */
#define VP_SIGN_PROUD         20   /* how far the sign stands off the wheel      */

/* ---- The shot --------------------------------------------------------------
   DERIVED FROM THE MOUNT, one geometry for all three pipes. The camera stands
   VP_CAM_BACK out along the mount's own facing — straight out of the pipe, the
   way the player came at it — and VP_CAM_UP above it, and looks back down.

   THE PITCH IS THOSE TWO NUMBERS AND NOTHING ELSE: atan(260/300) is 40.9deg,
   which is 465 of 4096. Change either constant and this has to be re-derived;
   it is not a taste value. (The stove's shot is the same arithmetic — see
   SP_CAM_PITCH — and lands on 40deg from 337 below and 402 away.)

   THE YAW is one of the four cardinals, because every pipe in the game is square
   to the world grid: the camera looks along -face, and yaw is measured from +Z
   toward +X, so a facing of -Z is a yaw of 0 and a facing of -X is 1024.

   >>> ALL THREE CAMERA SPOTS AND ALL THREE SIGHT LINES WERE CHECKED AGAINST THE
   MESHES, AND THEY HAVE TO BE. <<< Nothing about this geometry guarantees the
   camera lands in open air -- it is 300 units back from a pipe that stands
   against a hedge or a wall, and hedge is exactly what tends to be behind a
   pipe. Do NOT reason about it from the hedge heights: Maze One's interior runs
   are 333 tall but MAZE TWO'S ARE 500, so a camera at y = -430 is above one
   maze's hedges and inside the other's. What makes it safe is that the three
   spots happen to be over floor:

     Maze One    camera (5358, -431,  653), the grass north of the drain. The
                 only mesh within 60 units is floor at y=0.
     Maze Two    camera (4347, -431, 2301), likewise -- floor only.
     Chain Room  camera (1363, -429,  597), the gravel yard. The only thing over
                 it is a chain at y=-700, 271 above the lens.

   And the line from each camera to its wheel is clear of anything above the
   floor, sampled every 40 units. Re-export any of the three rooms and this
   paragraph has to be re-checked, not assumed. */
#define VP_CAM_BACK          300
#define VP_CAM_UP            260
#define VP_CAM_PITCH         465
/* >>> AND VALVE_FIT_DIST IS SOLVED AGAINST THESE THREE. <<< The wheel comes in
   along the same facing the camera stands on, so how far out it appears decides
   how low in frame it starts, and at the first value tried it started with its
   bottom third off the screen. The arithmetic is written out in valve_handle.h;
   change any of the three above and it has to be redone. */
#define VP_CAM_ANIM_FRAMES    24

/* How long each log line is held with the shot still up. Long enough to read
   four wrapped rows at the HUD's pitch without being a wait. */
#define VP_MSG_FRAMES        110

/* ---- Board layout (320x240) ------------------------------------------------
   The stove's board with one ingredient box instead of two, so the column is the
   same width in the same place and the pair is centred on the same axis. The
   whole column sits above y=150: the HUD log box starts at y=191 and spans
   x 183..315, which are the very columns this uses, and the selection cursor
   draws 4px outside whatever it is on. */
#define VP_BOX_X             244
#define VP_BOX_W              56
#define VP_BOX_H              56
#define VP_BOX_Y              58
#define VP_USE_Y             122
#define VP_USE_H              28   /* half height, as specified */
#define VP_BOX_ICON           40

/* Item picker panel + grid: the stove's exactly, because it is the same picker
   over the same MENU_SLOT_* set and two puzzles that look different while doing
   the same thing would be a lie. See the note on PICK_COLS in stove_puzzle.c —
   the panel fits exactly three rows and widens rather than growing a fourth. */
#define VP_PICK_X             24
#define VP_PICK_Y             30
#define VP_PICK_W            168
#define VP_PICK_H            168
#define VP_PICK_CELL          42
#define VP_PICK_ICON          30
#define VP_PICK_PAD            6
#define VP_PICK_COLS           4
#define VP_PICK_ROWS  ((MENU_ITEM_SLOTS + VP_PICK_COLS - 1) / VP_PICK_COLS)
#define VP_PICK_GRID_X  (VP_PICK_X + (VP_PICK_W - VP_PICK_COLS * VP_PICK_CELL) / 2)
#define VP_PICK_GRID_Y  (VP_PICK_Y + 22)
#define VP_PICK_NAME_Y  (VP_PICK_Y + VP_PICK_H - 16)

/* OT layers — all inside the menu-reserved range (0..SCENE_OT_MIN-1), so the
   board always sits on top of the room. Higher index = drawn first = further
   back (see menu.c). The stove's numbers, for the same reasons. */
#define VP_OT_PANEL           12
#define VP_OT_LINE            10
#define VP_OT_TEXWIN           8
#define VP_OT_ICON             7
#define VP_OT_CURSOR           3
#define VP_OT_TEXT             1

typedef enum {
    VP_IDLE = 0,   /* not in the puzzle: the sign, and the Circle trigger      */
    VP_INTRO,      /* camera gliding to the fixed shot                          */
    VP_BOARD,      /* board live: the item box and USE                          */
    VP_PICKER,     /* item picker open                                          */
    VP_FIT,        /* board gone, the wheel sliding in and taking its three     */
    VP_MSG1,       /* this pipe's own log line, held with the shot up           */
    VP_MSG2        /* ...and the handle being spent, if this was the third      */
} VpState;

static VpState state    = VP_IDLE;
static int     pipe_idx = -1;   /* index into VP_PIPES while a puzzle runs */
static int     mount    = -1;   /* the ValveMount it drives                */
static int     box_item = -1;   /* MENU_SLOT_* in the item box, or -1      */
static int     board_cur = 0;   /* 0 = the item box, 1 = USE               */
static int     pick_cur  = 0;
static int     timer     = 0;

/* Pre-puzzle camera, restored on the way out. */
static int32_t save_cx, save_cy, save_cz, save_crot, save_cvy;
static int32_t cam_anim_t = 0;
static int32_t src_x, src_y, src_z, src_rot, rot_delta;

static uint16_t btn_prev      = 0xFFFF;  /* board input edge-detect (btn = ~pad) */
static int      interact_prev = 1;       /* interact-Circle edge-detect          */

int valve_puzzle_active(void) { return state != VP_IDLE; }

int valve_puzzle_gates_unlocked(void) { return game_flag(FLAG_VALVE_MAZE_TWO); }

/* ---- Which pipe, if any, is in this room ---------------------------------- */
static int pipe_in_area(GameState area) {
    int i;
    for (i = 0; i < VP_PIPE_COUNT; i++)
        if (VP_PIPES[i].area == area) return i;
    return -1;
}

/* The pipe in this room that is still WORKABLE: it exists, it has a mount, and
   its flag is clear. Returns -1 otherwise, which is what retires a spent pipe —
   no sign, no prompt, no board. */
static int live_pipe(void) {
    int p = pipe_in_area(current_area);
    if (p < 0) return -1;
    if (game_flag(VP_PIPES[p].flag)) return -1;
    if (valve_mount_in_area(current_area) < 0) return -1;
    return p;
}

/* ---- The shot, derived ---------------------------------------------------- */
static int32_t mount_yaw(const ValveMount *m) {
    /* The camera looks along -face; yaw runs from +Z toward +X. */
    if (m->face_z < 0) return 0;      /* facing -Z: look +Z */
    if (m->face_x < 0) return 1024;   /* facing -X: look +X */
    if (m->face_z > 0) return 2048;   /* facing +Z: look -Z */
    return 3072;                      /* facing +X: look -X */
}

/* ---- State transitions ---------------------------------------------------- */

static void start_puzzle(int p, int m_idx) {
    const ValveMount *m = &valve_mounts[m_idx];

    pipe_idx = p;
    mount    = m_idx;

    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    /* The player STAYS at the pipe while the camera goes overhead — pinned, so
       anything hunting them hunts the real spot and the camera has somewhere
       honest to come back to. The stove's anchor and the Greenhouse flood's. */
    camera_anchor_player(save_cx, save_cy, save_cz);

    src_x = cam_x; src_y = cam_y; src_z = cam_z; src_rot = cam_rot;
    {
        int32_t target = mount_yaw(m);
        int32_t d = ((target - src_rot) % 4096 + 4096) % 4096;
        if (d > 2048) d -= 4096;      /* shortest signed turn */
        rot_delta = d;
    }

    cam_anim_t = 0;
    state      = VP_INTRO;
    board_cur  = 0;
    box_item   = -1;
}

/* Drop the fixed camera and put the player back exactly where they stood. */
static void exit_puzzle(void) {
    state    = VP_IDLE;
    pipe_idx = -1;
    mount    = -1;
    cam_x    = save_cx; cam_y = save_cy; cam_z = save_cz;
    cam_rot  = save_crot; cam_vy = save_cvy;
    cam_pitch = 0;
    camera_release_player();
    interact_prev = 1;   /* swallow the held Circle so it cannot re-open */
}

/* ---- The water ------------------------------------------------------------
   ONE CALL DOES BOTH ENDS. SFX_WATER is hardware-looped, so starting it is a
   single sound_play() and it then runs until something keys the voice off —
   there is no per-frame upkeep and no "still playing" state to track. What
   there IS is an obligation to stop it, which is why this is written as a
   start/stop pair over the area rather than as a start called from three rooms:
   every transition passes through it, so the loop cannot survive into a room it
   does not belong in.

   The three rooms are the ones the drain runs through: Maze One, where the valve
   is; Fountain Square, west of it; and the Rear Gate, at the far end of the same
   channel. All three are on SND_BANK_GARDEN — check main.c's sound_bank_select
   before adding a fourth, because a banked effect in the wrong bank is SILENT
   rather than broken. */
static void water_for_area(GameState area) {
    if (game_flag(FLAG_VALVE_MAZE_ONE) &&
        (area == STATE_MAZE_ONE || area == STATE_FOUNTAIN_SQUARE ||
         area == STATE_REAR_GATE)) {
        sound_play(SFX_WATER);
    } else {
        sound_stop(SFX_WATER);
    }
}

/* ---- The three payoffs ---------------------------------------------------- */

/* MAZE ONE: the drain runs. It also WASHES a key that is already lying in it,
   which is the second of the cage's two routes to its last state — see the
   block on ordering in valve_puzzle.h. */
static const char *payoff_maze_one(void) {
    game_flag_set(FLAG_VALVE_MAZE_ONE);
    water_for_area(current_area);       /* the player is standing in Maze One */
    if (birdcage_state() == BIRDCAGE_DROPPED) birdcage_wash();
    return "The drain below is flowing with water";
}

/* MAZE TWO: both of the Chain Room's gates. Nothing to do but set the bit — the
   gates have no state of their own and read the flag directly. */
static const char *payoff_maze_two(void) {
    game_flag_set(FLAG_VALVE_MAZE_TWO);
    sound_play(SFX_UNLOCK);
    return "You hear something unlock";
}

/* CHAIN ROOM: the chain winds in over Maze One's cage. The key inside it is a
   real ItemPickup in ANOTHER room, so nothing here can touch it — the cage state
   is the record, and valve_puzzle_apply_flags() removes the pickup on the next
   entry to Maze One. */
static const char *payoff_chain_room(void) {
    game_flag_set(FLAG_VALVE_CHAIN_ROOM);
    /* _wash if the drain is already running, _open if it is not: whichever pipe
       is turned second is the one that reaches WASHED. */
    if (game_flag(FLAG_VALVE_MAZE_ONE)) birdcage_wash();
    else                                birdcage_open();
    return "The chain has been wound in";
}

static int all_pipes_done(void) {
    int i;
    for (i = 0; i < VP_PIPE_COUNT; i++)
        if (!game_flag(VP_PIPES[i].flag)) return 0;
    return 1;
}

/* The frame the fit animation ends. */
static void fire_payoff(void) {
    const char *line = NULL;

    /* >>> THE WHEEL COMES STRAIGHT BACK OFF. <<< The player carries ONE handle
       and fits it to three pipes, so leaving it on each would have three of them
       hanging in the garden at once — and the third pipe would then be turned by
       a handle the player could see still bolted to the first. Clearing
       `present` here, on the frame the turning stops, is what makes the prop
       agree with the inventory: the valve has been worked, the handle is back in
       the player's hands, and the pipe is left with the hole in its texture that
       valve_handles_draw shows for an empty mount.

       It also settles the save: `present` is what rides in
       WorldDelta.valve_present, so a mount cleared here comes back cleared and
       nothing has to remember that this pipe was already spent. The pipe's own
       FLAG_VALVE_* is what retires the interaction (live_pipe reads it); this
       bit is only the art.

       valve_handle_set_present(0) also zeroes the spin, the offset and the fit
       timer, which is exactly right — the script has just ended and the mount
       must be left at rest. */
    valve_handle_set_present(mount, 0);

    switch (VP_PIPES[pipe_idx].area) {
        case STATE_MAZE_ONE:   line = payoff_maze_one();   break;
        case STATE_MAZE_TWO:   line = payoff_maze_two();   break;
        case STATE_CHAIN_ROOM: line = payoff_chain_room(); break;
        default: break;
    }
    if (line) show_pickup_msg_raw(line);

    state = VP_MSG1;
    timer = VP_MSG_FRAMES;
}

/* The handle is spent. Its own beat, AFTER the pipe's line rather than with it,
   because the two together are five wrapped rows and the HUD log shows four —
   posted in one frame, the pipe's line would scroll off before it was read. */
static void retire_handle(void) {
    player_items &= ~(1 << ITEM_VALVE_HANDLE);
    /* The pause menu's grid catches up now rather than waiting for the next
       menu_open, so a player who pauses on the way out does not find a handle
       they no longer have. The stove's cook and the piano key take the same
       route. */
    menu_inventory_sync();
    show_pickup_msg_raw("I no longer need this Valve Handle");
    state = VP_MSG2;
    timer = VP_MSG_FRAMES;
}

/* ---- Room entry ----------------------------------------------------------- */

void valve_puzzle_arm(void) {
    interact_prev = interact_tapped();
    state         = VP_IDLE;
    pipe_idx      = -1;
    mount         = -1;
    box_item      = -1;
    board_cur     = 0;
    timer         = 0;
    camera_release_player();
}

void valve_puzzle_reset(void) {
    valve_puzzle_arm();
    interact_prev = 1;
    sound_stop(SFX_WATER);
}

/* ---- Post-load / post-transition reconcile --------------------------------
   Called from main.c AFTER savegame_apply_pending(), for the reason
   keystone_plinths_apply_flags() is: it writes to item_pickups, which
   world_enter() has only just restored, and it reads flags that a title-screen
   Load Game installs later still. */
void valve_puzzle_apply_flags(GameState area) {
    water_for_area(area);

    /* MAZE ONE: the caged Hatch Key goes the moment the cage is opened. It has
       to be removed HERE rather than gated out of world_seed_room(), because a
       player who has already been to Maze One gets restore()d from that room's
       RoomState snapshot and never touches the seed again. Deactivating is also
       what the pickup was built for — world.c's note on it says the answer when
       the puzzle resolves is to widen its reach or move it, and this is the
       third door: take it away. */
    if (area == STATE_MAZE_ONE && birdcage_state() != BIRDCAGE_LOCKED) {
        int i;
        for (i = 0; i < item_pickup_count; i++)
            if (item_pickups[i].active &&
                item_pickups[i].kind == PICKUP_HATCH_KEY)
                item_pickups[i].active = 0;
    }

    /* THE REAR GATE'S HATCH KEY, placed ONCE and then owned by the ordinary
       per-room persistence. FLAG_DRAIN_KEY_PLACED is what makes it once: after
       this fires, the pickup lives in the Rear Gate's RoomState like any other,
       is snapshotted on the way out, and stays taken if the player takes it.
       Without the flag this would re-place it on every entry and the player
       could farm hatch keys off it.
       world_seed_room() seeds the same key under the same flag, which is what
       rebuilds it on a save load — and it is the room's ONLY pickup either way,
       so both routes put it in slot 0 and WorldDelta.items_gone stays keyed to
       the same thing.

       WHERE: (230, 300), and the SAME THREE NUMBERS ARE IN world.c's seed —
       keep them in step or a save rebuild will put the key somewhere else.
       x=230 is the centre line of the drain channel that crosses this room (its
       north-south run is x[204,257]), so the key lies on the line the water
       would have carried it down; the path is x[-300,300] there, walled both
       sides, so it lands squarely in the corridor.

       z=300 is on the GRINDER SIDE of the drain: the channel's south end is at
       z=429 and the two grinder plates are set into the path walls at z=-85, so
       this is 129 short of where the drain stops and 385 north of the grinders —
       the stretch of corridor between the two. It is also 185 clear of
       GP_CRUSH_Z_MAX (115), the north face of the box the grinders close on, so
       the key can never be somewhere the player has to stand inside that box to
       reach. (GP_KILL_Z_MAX, 265, is a HADAD-position test and not a hazard to
       anything standing here — but the spot clears that too.)
       The y is -50, which is what every floor-level pickup in a y=0 room passes:
       item_pickup_spawn subtracts IP_FLOAT_Y (50), so the sprite hovers at -100,
       just off the gravel. */
    if (area == STATE_REAR_GATE &&
        game_flag(FLAG_BIRDCAGE_WASHED) && !game_flag(FLAG_DRAIN_KEY_PLACED)) {
        /* The flag records the SPAWN, so it is only set if the spawn happened.
           MAX_ITEM_PICKUPS is 8 and this room seeds none, so a full array is not
           a live case -- but setting the bit on a failed spawn would retire the
           key permanently, which is the one outcome nothing could recover from. */
        if (item_pickup_spawn_amount(VP_DRAIN_KEY_X, VP_DRAIN_KEY_Y,
                                     VP_DRAIN_KEY_Z, PICKUP_HATCH_KEY, 1) >= 0)
            game_flag_set(FLAG_DRAIN_KEY_PLACED);
    }
}

/* ---- Interaction ---------------------------------------------------------- */

static int pipe_in_reach(int m_idx) {
    const ValveMount *m = &valve_mounts[m_idx];
    int32_t dx = cam_x - m->x, dz = cam_z - m->z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= VP_TRIGGER_RADIUS) return 0;
    /* In view as well as in range, as every other Circle prompt in the garden:
       a pipe behind you is not one you are working on. */
    return interact_facing(m->x, m->z);
}

int valve_puzzle_update(void) {
    uint16_t btn = 0;
    if (pad_buff_len[0]) { PadResponse *pad = (PadResponse *)pad_buff[0]; btn = ~pad->btn; }

    if (state == VP_IDLE) {
        int held = interact_tapped();
        int just = held && !interact_prev;
        interact_prev = held;

        int p = live_pipe();
        if (p < 0 || !just) return 0;

        int m_idx = valve_mount_in_area(current_area);
        if (!pipe_in_reach(m_idx)) return 0;

        start_puzzle(p, m_idx);
        return 1;              /* the tap is ours: the gates must not see it */
    }

    /* Camera glides to the fixed shot; the board opens once it settles. */
    if (state == VP_INTRO) {
        cam_anim_t++;
        int32_t t = cam_anim_t * 256 / VP_CAM_ANIM_FRAMES; if (t > 256) t = 256;
        int32_t inv = 256 - t;
        int32_t e = 256 - (inv * inv / 256);            /* ease-out 0..256 */

        const ValveMount *m = &valve_mounts[mount];
        int32_t tx = m->x + m->face_x * VP_CAM_BACK;
        int32_t ty = m->y - VP_CAM_UP;
        int32_t tz = m->z + m->face_z * VP_CAM_BACK;

        cam_x     = src_x   + ((tx - src_x) * e) / 256;
        cam_y     = src_y   + ((ty - src_y) * e) / 256;
        cam_z     = src_z   + ((tz - src_z) * e) / 256;
        cam_rot   = src_rot + (rot_delta * e) / 256;
        cam_pitch = (VP_CAM_PITCH * e) / 256;
        cam_vy    = 0;
        if (cam_anim_t >= VP_CAM_ANIM_FRAMES) {
            cam_x = tx; cam_y = ty; cam_z = tz;
            cam_rot = mount_yaw(m); cam_pitch = VP_CAM_PITCH; cam_vy = 0;
            state = VP_BOARD;
            btn_prev = btn;   /* arm: the held Circle is not a select */
        }
        return 1;
    }

    /* The fit. No input at all while it runs — this is the payoff for the press
       and the camera is already where it needs to be, so there is nothing to
       skip to. The three SHARP TURNS get a click each: SFX_UNLOCK is the
       game's metal-on-metal one-shot and it is RESIDENT, so it sounds in all
       three of these rooms without touching the garden bank.

       The step boundaries are recomputed here from the same constants
       valve_handles_update() uses rather than being reported back by it, so the
       wheel and the click cannot drift apart: both are functions of the same
       elapsed frame count. */
    if (state == VP_FIT) {
        int32_t elapsed = VALVE_FIT_FRAMES - timer;
        int32_t k = elapsed - VALVE_FIT_IN_FRAMES - VALVE_FIT_HOLD;
        if (k >= 0) {
            int32_t span = VALVE_FIT_STEP_FRAMES + VALVE_FIT_STEP_PAUSE;
            if ((k % span) == 0 && (k / span) < VALVE_FIT_STEPS)
                sound_play(SFX_UNLOCK);
        }
        if (--timer <= 0) fire_payoff();
        return 1;
    }

    if (state == VP_MSG1) {
        if (--timer <= 0) {
            if (all_pipes_done()) retire_handle();
            else                  exit_puzzle();
        }
        return 1;
    }

    if (state == VP_MSG2) {
        if (--timer <= 0) exit_puzzle();
        return 1;
    }

    uint16_t pressed = btn & ~btn_prev;
    btn_prev = btn;

    if (state == VP_BOARD) {
        /* Guarded at both ends so a press against either is silent. */
        if ((pressed & PAD_UP)   && board_cur > 0) { board_cur--; sound_play(SFX_CURSOR); }
        if ((pressed & PAD_DOWN) && board_cur < 1) { board_cur++; sound_play(SFX_CURSOR); }
        if (pressed & PAD_CROSS)  { sound_play(SFX_BACK); exit_puzzle(); return 1; }
        if (pressed & PAD_CIRCLE) {
            /* USE is confirmed here whether or not the item is right: the fit is
               most of a second away on a success and there is no cue at all on a
               failure beyond the log line, so this blip is the acknowledgement
               the press gets either way. The stove's COOK does the same. */
            sound_play(SFX_SELECT);
            if (board_cur == 1) {
                if (box_item != MENU_SLOT_VALVE_HANDLE) {
                    show_pickup_msg_raw("That doesn't make sense...");
                } else {
                    /* The board goes and the wheel comes on. valve_handle_begin_fit
                       sets `present` itself — see valve_handle.h — so the wheel is
                       drawn from this frame and the slide is visible. */
                    valve_handle_begin_fit(mount);
                    state = VP_FIT;
                    timer = VALVE_FIT_FRAMES;
                }
            } else {
                pick_cur = box_item >= 0 ? box_item : 0;
                state    = VP_PICKER;
            }
        }
        return 1;
    }

    /* VP_PICKER: the stove's grid over every inventory slot. Choosing an item
       COPIES it into the box — nothing is consumed until the valve is turned,
       and a player who backs out keeps everything. The trailing cells of the
       last row may be past MENU_ITEM_SLOTS; the guard below keeps the cursor out
       of them and the draw loop never paints them. */
    {
        int row = pick_cur / VP_PICK_COLS, col = pick_cur % VP_PICK_COLS;
        if (pressed & PAD_UP)    { if (row > 0) row--; }
        if (pressed & PAD_DOWN)  { if (row < VP_PICK_ROWS - 1) row++; }
        if (pressed & PAD_LEFT)  { if (col > 0) col--; }
        if (pressed & PAD_RIGHT) { if (col < VP_PICK_COLS - 1) col++; }
        int next = row * VP_PICK_COLS + col;
        if (next < MENU_ITEM_SLOTS && next != pick_cur) {
            pick_cur = next;
            sound_play(SFX_CURSOR);
        }

        if (pressed & PAD_CROSS) { sound_play(SFX_BACK); state = VP_BOARD; return 1; }
        if ((pressed & PAD_CIRCLE) && menu_item_held(pick_cur)) {
            box_item = pick_cur;
            state    = VP_BOARD;
            sound_play(SFX_SELECT);
        }
    }
    return 1;
}

/* ---- The floating sign ---------------------------------------------------- */

void valve_puzzle_text(RenderContext *ctx) {
    if (state != VP_IDLE) return;          /* the board owns the screen */
    if (live_pipe() < 0) return;           /* spent, or not a pipe room */

    int m_idx = valve_mount_in_area(current_area);
    const ValveMount *m = &valve_mounts[m_idx];

    int32_t dx = cam_x - m->x, dz = cam_z - m->z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= VP_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > VP_FADE_NEAR) {
        int range = VP_TEXT_RADIUS - VP_FADE_NEAR;
        int prog  = xz - VP_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* THE SIGN HANGS OVER THE WHEEL, not on it: VP_SIGN_CLEAR of air between the
       bottom of the glyphs and the top of the ring (the ring's top is
       VALVE_RADIUS above the mount origin, and -Y is up). The Greenhouse's sign
       is placed the same way and for the same reason — text across the wheel
       hides the thing the player is being told to work.

       THE PLANE AND THE HAND ARE BOTH THE FACING'S. A pipe whose face looks
       along Z is read along X (TEXT_PLANE_XY) and one whose face looks along X
       is read along Z (TEXT_PLANE_YZ); `mirror` is which SIDE of it the player
       stands on, and getting it backwards spells the line out reversed. The two
       rules below are the ones stated in chain_room.h and maze_one.h for gate
       signs, applied to a face instead of a leaf:
         XY, player on the -Z side (face -Z) -> mirror 0; +Z side -> 1
         YZ, player on the -X side (face -X) -> mirror 1; +X side -> 0
       door_draw_string_3d centres the READING axis on what it is handed after
       adding 200, hence the -200 on that axis and not on the other. */
    int32_t sign_y = m->y - VALVE_RADIUS - VP_SIGN_CLEAR - VP_TEXT_ROWS_H;

    if (m->face_z != 0) {
        door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to interact",
                            m->x - 200, sign_y, m->z + m->face_z * VP_SIGN_PROUD,
                            50, 255, 50, fade,
                            m->face_z > 0 ? 1 : 0, TEXT_PLANE_XY, VP_TEXT_PIXEL);
    } else {
        door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to interact",
                            m->x + m->face_x * VP_SIGN_PROUD, sign_y, m->z - 200,
                            50, 255, 50, fade,
                            m->face_x < 0 ? 1 : 0, TEXT_PLANE_YZ, VP_TEXT_PIXEL);
    }
}

/* ---- The board ------------------------------------------------------------ */

static void vp_rect(RenderContext *ctx, int x, int y, int w, int h,
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

static void vp_outline(RenderContext *ctx, int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    vp_rect(ctx, x,       y,       w, 2, r, g, b, ot_idx);
    vp_rect(ctx, x,       y+h-2,   w, 2, r, g, b, ot_idx);
    vp_rect(ctx, x,       y,       2, h, r, g, b, ot_idx);
    vp_rect(ctx, x+w-2,   y,       2, h, r, g, b, ot_idx);
}

static void vp_cursor(RenderContext *ctx, int x, int y, int w, int h) {
    vp_outline(ctx, x - 4, y - 4, w + 8, h + 8, 80, 80, 200, VP_OT_CURSOR);
    vp_outline(ctx, x - 2, y - 2, w + 4, h + 4, 180, 180, 255, VP_OT_CURSOR);
}

void valve_puzzle_draw(RenderContext *ctx) {
    /* Nothing while the camera is still flying, and nothing once the board has
       gone: the fit and the two log beats are the room and the wheel, with no
       overlay on them at all. */
    if (state != VP_BOARD && state != VP_PICKER) return;

    /* Reset the texture window before any icon: every one of these rooms sorts a
       128x128 window at OT_LENGTH-1 which is still active down here and would
       wrap the icons' UVs (the same note as in menu_draw and stove_puzzle_draw). */
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            RECT full = {0, 0, 0, 0};
            DR_TWIN *tw = (DR_TWIN *)ctx->next_packet;
            setTexWindow(tw, &full);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[VP_OT_TEXWIN], tw);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    /* --- The item box and USE, on the right --- */
    vp_rect(ctx, VP_BOX_X, VP_BOX_Y, VP_BOX_W, VP_BOX_H, 35, 30, 45, VP_OT_PANEL);
    vp_outline(ctx, VP_BOX_X, VP_BOX_Y, VP_BOX_W, VP_BOX_H, 80, 70, 100, VP_OT_LINE);
    if (box_item >= 0)
        menu_draw_item_icon(ctx, box_item,
                            VP_BOX_X + (VP_BOX_W - VP_BOX_ICON) / 2,
                            VP_BOX_Y + (VP_BOX_H - VP_BOX_ICON) / 2,
                            VP_BOX_ICON, VP_OT_ICON);

    vp_rect(ctx, VP_BOX_X, VP_USE_Y, VP_BOX_W, VP_USE_H, 45, 25, 25, VP_OT_PANEL);
    vp_outline(ctx, VP_BOX_X, VP_USE_Y, VP_BOX_W, VP_USE_H, 120, 70, 70, VP_OT_LINE);
    /* "Use" is 3 chars at the font's 8px advance. */
    btn_prompt_draw(ctx, VP_BOX_X + (VP_BOX_W - 24) / 2,
                    VP_USE_Y + (VP_USE_H - 8) / 2, "Use", VP_OT_TEXT);

    /* --- Board cursor (hidden while the picker is up) --- */
    if (state == VP_BOARD) {
        if (board_cur == 1) vp_cursor(ctx, VP_BOX_X, VP_USE_Y, VP_BOX_W, VP_USE_H);
        else                vp_cursor(ctx, VP_BOX_X, VP_BOX_Y, VP_BOX_W, VP_BOX_H);
    }

    /* --- Item picker --- */
    if (state == VP_PICKER) {
        vp_rect(ctx, VP_PICK_X, VP_PICK_Y, VP_PICK_W, VP_PICK_H, 15, 12, 20, VP_OT_PANEL);
        vp_outline(ctx, VP_PICK_X, VP_PICK_Y, VP_PICK_W, VP_PICK_H, 80, 80, 80, VP_OT_LINE);
        btn_prompt_draw(ctx, VP_PICK_X + 8, VP_PICK_Y + 6, "ITEMS", VP_OT_TEXT);

        int s;
        for (s = 0; s < MENU_ITEM_SLOTS; s++) {
            int cx = VP_PICK_GRID_X + (s % VP_PICK_COLS) * VP_PICK_CELL;
            int cy = VP_PICK_GRID_Y + (s / VP_PICK_COLS) * VP_PICK_CELL;
            vp_rect(ctx, cx, cy, VP_PICK_CELL, VP_PICK_CELL, 35, 30, 45, VP_OT_PANEL);
            vp_outline(ctx, cx, cy, VP_PICK_CELL, VP_PICK_CELL, 80, 70, 100, VP_OT_LINE);
            menu_draw_item_icon(ctx, s, cx + VP_PICK_PAD, cy + VP_PICK_PAD,
                                VP_PICK_ICON, VP_OT_ICON);
        }

        {
            int cx = VP_PICK_GRID_X + (pick_cur % VP_PICK_COLS) * VP_PICK_CELL;
            int cy = VP_PICK_GRID_Y + (pick_cur / VP_PICK_COLS) * VP_PICK_CELL;
            vp_cursor(ctx, cx, cy, VP_PICK_CELL, VP_PICK_CELL);
        }

        {
            const char *label = menu_item_held(pick_cur) ? menu_item_name(pick_cur)
                                                         : "Empty";
            btn_prompt_draw(ctx, VP_PICK_X + 8, VP_PICK_NAME_Y, label, VP_OT_TEXT);
        }
    }

    /* --- Controls, on the trick drawers' line --- */
    if (state == VP_BOARD)
        btn_prompt_draw(ctx, 8, 206, BTN_CIRCLE " - Select  " BTN_CROSS " - Exit",
                        VP_OT_TEXT);
    else
        btn_prompt_draw(ctx, 8, 206, BTN_CIRCLE " - Select  " BTN_CROSS " - Back",
                        VP_OT_TEXT);
}
