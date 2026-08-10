#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"          /* player_items, game_flags, show_pickup_msg_raw */
#include "menu.h"            /* MENU_SLOT_*, shared item icons/names */
#include "sound.h"           /* SFX_PICKUP */
#include "btn_glyph.h"       /* BTN_*, btn_prompt_draw */
#include "piano_props.h"     /* the piano's position, keys swap, bookcase sink */
#include "piano_puzzle.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- Trigger --------------------------------------------------------------
   Same Manhattan radius the old examine prompt used, measured to the piano prop
   itself (piano_prop_x/z) so moving the prop moves the trigger with it. */
#define PP_INTERACT_RADIUS    400

/* ---- The two fixed shots ---------------------------------------------------
   Shot 1, the keyboard: south-west of the piano, raised to the same height above
   the floor the stove puzzle uses (-442 against a y=0 floor). Yaw is atan2(dx,dz)
   onto the keyboard front at (-420,-130,800) — dx=+373, dz=+300, so 51.2deg of
   4096 — and the pitch tilts down onto it: 312 below over 479 of ground, i.e.
   33.1deg. Move PP_CAM_X/Z and BOTH of those have to be re-derived.

   Shot 2, the bookcase: the camera turns AND dollies back east while it turns.
   The bookcase is a 1800-long, 520-tall wall of shelves along x=-1150; from the
   keyboard spot it is only 357 away, far too close to read as an object. Pulling
   back to x=-400 puts it 750 out, where the vertical field (2*750*tan(20.6deg),
   about 563) covers the full height with the floor line still inside the frame —
   and seeing the shelves go INTO the boards is the whole point of the shot.
   The yaw is a flat 3072 (due -X) at any x on this line; the pitch aims at the
   shelves' midpoint, 260 above the floor. */
#define PP_CAM_X            (-793)
#define PP_CAM_Y            (-442)
#define PP_CAM_Z              500
#define PP_CAM_ROT            582
#define PP_CAM_PITCH          376

#define PP_BOOK_X           (-400)   /* dollied back east; Y/Z are unchanged */
#define PP_BOOK_ROT          3072
#define PP_BOOK_PITCH         155

#define PP_CAM_ANIM_FRAMES     24   /* glide in from wherever the player stood */
#define PP_PLACED_FRAMES       45   /* beat on the repaired keys before turning */
#define PP_TURN_FRAMES         72   /* the pan+dolly onto the bookcase         */
#define PP_HOLD_FRAMES         45   /* beat on the empty floor before ejecting */

/* ---- Board layout (320x240) -----------------------------------------------
   One item box with the half-height PLACE box under it, both on the right,
   vertically centred on the same column the stove board uses. */
#define BOX_X                 244
#define BOX_W                  56
#define BOX_H                  56
#define BOX_Y                  78
#define PLACE_Y               150
#define PLACE_H                28   /* half height, as specified */
#define BOX_ICON               40

/* Item picker panel + grid — identical metrics to the stove's, so the two
   puzzles present the inventory the same way (see the sizing note in
   stove_puzzle.c: 3 columns, not 2, or MENU_ITEM_SLOTS spills off the panel). */
#define PICK_X                 24
#define PICK_Y                 30
#define PICK_W                168
#define PICK_H                168
#define PICK_CELL              42
#define PICK_ICON              30
#define PICK_PAD                6
#define PICK_COLS               3
#define PICK_ROWS  ((MENU_ITEM_SLOTS + PICK_COLS - 1) / PICK_COLS)
#define PICK_GRID_X   (PICK_X + (PICK_W - PICK_COLS * PICK_CELL) / 2)
#define PICK_GRID_Y   (PICK_Y + 22)
#define PICK_NAME_Y   (PICK_Y + PICK_H - 16)

/* OT layers — inside the menu-reserved range (0..SCENE_OT_MIN-1) so the board
   sits on top of the room. Higher index = drawn first = further back. */
#define PP_OT_PANEL            12
#define PP_OT_LINE             10
#define PP_OT_TEXWIN            8   /* reset the 128 window before the icons */
#define PP_OT_ICON              7
#define PP_OT_CURSOR            3
#define PP_OT_TEXT              1

typedef enum {
    PZ_IDLE = 0,   /* not in the puzzle: proximity prompt + Circle trigger */
    PZ_INTRO,      /* camera gliding to the keyboard shot                  */
    PZ_BOARD,      /* board live: pick the box / PLACE                     */
    PZ_PICKER,     /* item picker open                                     */
    PZ_PLACED,     /* keys repaired, holding on them before the pan        */
    PZ_TURN,       /* panning across onto the bookcase                     */
    PZ_SINK,       /* bookcase descending                                  */
    PZ_HOLD        /* beat on the revealed floor, then eject               */
} PzState;

static PzState state = PZ_IDLE;

/* The box's contents: MENU_SLOT_* of the item shown, or -1 for empty. */
static int box_item  = -1;
static int board_cur =  0;    /* 0 = the item box, 1 = PLACE */
static int pick_cur  =  0;    /* picker cursor: MENU_SLOT_*  */
static int seq_timer =  0;    /* counts down the scripted PZ_* beats */

/* Pre-puzzle camera, restored on the way out. */
static int32_t save_cx, save_cy, save_cz, save_crot, save_cvy;
static int32_t cam_anim_t = 0;
static int32_t src_x, src_y, src_z, src_rot, src_pitch, rot_delta;

static uint16_t pz_btn_prev   = 0xFFFF;  /* board input edge-detect (btn = ~pad) */
static int      interact_prev = 1;       /* interact-Circle edge-detect          */

int piano_puzzle_active(void) { return state != PZ_IDLE; }

/* The saved flag IS the completion state — the Piano Key is consumed, so there
   is no inventory bit left to infer it from. */
int piano_puzzle_solved(void) { return game_flag(FLAG_PIANO_SOLVED); }

void piano_puzzle_arm(void) {
    int held = interact_tapped();
    interact_prev = held;
    state         = PZ_IDLE;
    camera_release_player();
    box_item  = -1;
    board_cur = 0;
    seq_timer = 0;
}

/* ---- State transitions --------------------------------------------------- */

/* Shortest signed turn from `from` to `to`, in 4096ths. */
static int32_t turn_delta(int32_t from, int32_t to) {
    int32_t d = ((to - from) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    return d;
}

static void start_puzzle(void) {
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    /* The player stays at the piano while the camera flies over — pin their
       position so anything that hunts them keeps hunting the real spot. */
    camera_anchor_player(save_cx, save_cy, save_cz);

    src_x = cam_x; src_y = cam_y; src_z = cam_z;
    src_rot = cam_rot; src_pitch = cam_pitch;
    rot_delta = turn_delta(src_rot, PP_CAM_ROT);

    cam_anim_t = 0;
    state      = PZ_INTRO;
    board_cur  = 0;
    box_item   = -1;
}

/* Drop the fixed camera and put the player back exactly where they stood. */
static void exit_puzzle(void) {
    state   = PZ_IDLE;
    cam_x   = save_cx; cam_y = save_cy; cam_z = save_cz;
    cam_rot = save_crot; cam_vy = save_cvy;
    cam_pitch = 0;
    camera_release_player();
    interact_prev = 1;   /* swallow the held Circle so we don't re-open instantly */
}

static void try_place(void) {
    if (box_item != MENU_SLOT_PIANO_KEY) {
        show_pickup_msg_raw("That makes no sense...");
        return;
    }
    /* The key becomes part of the instrument: consumed, unlike the stove's
       ingredients. FLAG_PIANO_SOLVED is what survives instead. */
    player_items &= ~(1 << ITEM_PIANO_KEY);
    game_flag_set(FLAG_PIANO_SOLVED);
    piano_props_repair_keys();
    sound_play(SFX_PICKUP);
    show_pickup_msg_raw("The key slots into place");

    state     = PZ_PLACED;
    seq_timer = PP_PLACED_FRAMES;
}

/* ---- Per-frame update ---------------------------------------------------- */

void piano_puzzle_update(void) {
    uint16_t btn = 0;
    if (pad_buff_len[0]) { PadResponse *pad = (PadResponse *)pad_buff[0]; btn = ~pad->btn; }

    if (state == PZ_IDLE) {
        if (piano_puzzle_solved()) return;   /* retired: the key is already in */
        int held = interact_tapped();
        int just = held && !interact_prev;
        interact_prev = held;
        int32_t dx = cam_x - piano_prop_x(), dz = cam_z - piano_prop_z();
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (just && xz < PP_INTERACT_RADIUS &&
            interact_facing(piano_prop_x(), piano_prop_z())) start_puzzle();
        return;
    }

    /* Camera glides to the keyboard shot; the board opens once it settles, and
       the flavour line the old examine gave is posted then. */
    if (state == PZ_INTRO) {
        cam_anim_t++;
        int32_t t = cam_anim_t * 256 / PP_CAM_ANIM_FRAMES; if (t > 256) t = 256;
        int32_t inv = 256 - t;
        int32_t e = 256 - (inv * inv / 256);            /* ease-out 0..256 */
        cam_x     = src_x   + ((PP_CAM_X - src_x) * e) / 256;
        cam_y     = src_y   + ((PP_CAM_Y - src_y) * e) / 256;
        cam_z     = src_z   + ((PP_CAM_Z - src_z) * e) / 256;
        cam_rot   = src_rot + (rot_delta * e) / 256;
        cam_pitch = src_pitch + ((PP_CAM_PITCH - src_pitch) * e) / 256;
        cam_vy    = 0;
        if (cam_anim_t >= PP_CAM_ANIM_FRAMES) {
            cam_x = PP_CAM_X; cam_y = PP_CAM_Y; cam_z = PP_CAM_Z;
            cam_rot = PP_CAM_ROT; cam_pitch = PP_CAM_PITCH; cam_vy = 0;
            state = PZ_BOARD;
            pz_btn_prev = btn;   /* arm: don't treat the held Circle as a select */
            show_pickup_msg_raw("A dusty old piano. There is a white key missing");
        }
        return;
    }

    /* ---- The scripted payoff: no input from here to the eject ---- */

    if (state == PZ_PLACED) {
        if (--seq_timer <= 0) {
            src_x     = cam_x;
            src_rot   = cam_rot;
            src_pitch = cam_pitch;
            rot_delta = turn_delta(src_rot, PP_BOOK_ROT);
            cam_anim_t = 0;
            state      = PZ_TURN;
        }
        return;
    }

    /* Turn and dolly together, on the same ease-out, so the pull-back reads as
       one move rather than a pan followed by a shove. Only X moves — the
       bookcase runs along Z, so backing off along X is a straight retreat from
       it and the yaw target never changes. */
    if (state == PZ_TURN) {
        cam_anim_t++;
        int32_t t = cam_anim_t * 256 / PP_TURN_FRAMES; if (t > 256) t = 256;
        int32_t inv = 256 - t;
        int32_t e = 256 - (inv * inv / 256);            /* ease-out 0..256 */
        cam_x     = src_x     + ((PP_BOOK_X - src_x) * e) / 256;
        cam_rot   = src_rot   + (rot_delta * e) / 256;
        cam_pitch = src_pitch + ((PP_BOOK_PITCH - src_pitch) * e) / 256;
        if (cam_anim_t >= PP_TURN_FRAMES) {
            cam_x     = PP_BOOK_X;
            cam_rot   = PP_BOOK_ROT;
            cam_pitch = PP_BOOK_PITCH;
            piano_props_bookcase_sink_start();
            state = PZ_SINK;
        }
        return;
    }

    if (state == PZ_SINK) {
        if (piano_props_bookcase_update()) {
            state     = PZ_HOLD;
            seq_timer = PP_HOLD_FRAMES;
        }
        return;
    }

    if (state == PZ_HOLD) {
        if (--seq_timer <= 0) exit_puzzle();
        return;
    }

    uint16_t pressed = btn & ~pz_btn_prev;
    pz_btn_prev = btn;

    if (state == PZ_BOARD) {
        /* Guarded so a press against either end of the two rows is silent. */
        if ((pressed & PAD_UP)   && board_cur > 0) { board_cur--; sound_play(SFX_CURSOR); }
        if ((pressed & PAD_DOWN) && board_cur < 1) { board_cur++; sound_play(SFX_CURSOR); }
        if (pressed & PAD_CROSS)  { sound_play(SFX_BACK); exit_puzzle(); return; }
        if (pressed & PAD_CIRCLE) {
            if (board_cur == 1) {
                /* PLACE answers a success itself — try_place fires SFX_PICKUP
                   in this very frame — so the confirm blip is only for the
                   wrong-item branch, which otherwise gets a message and no
                   sound at all. */
                if (box_item != MENU_SLOT_PIANO_KEY) sound_play(SFX_SELECT);
                try_place();
            } else {
                sound_play(SFX_SELECT);
                /* Open on the box's current item, else the first slot. */
                pick_cur = box_item >= 0 ? box_item : 0;
                state    = PZ_PICKER;
            }
        }
        return;
    }

    /* PZ_PICKER: PICK_COLS-wide grid over every inventory slot. Choosing an item
       copies it into the box; nothing is consumed until PLACE succeeds. The
       trailing cells of the last row may be past MENU_ITEM_SLOTS; the guard
       below keeps the cursor out of them and the draw loop never paints them. */
    {
        int row = pick_cur / PICK_COLS, col = pick_cur % PICK_COLS;
        if (pressed & PAD_UP)    { if (row > 0) row--; }
        if (pressed & PAD_DOWN)  { if (row < PICK_ROWS - 1) row++; }
        if (pressed & PAD_LEFT)  { if (col > 0) col--; }
        if (pressed & PAD_RIGHT) { if (col < PICK_COLS - 1) col++; }
        int next = row * PICK_COLS + col;
        /* Blipped off the accepted cell, not the press: the guard below rejects
           a step into the last row's trailing cells, and the grid edges reject
           the rest. */
        if (next < MENU_ITEM_SLOTS && next != pick_cur) {
            pick_cur = next;
            sound_play(SFX_CURSOR);
        }

        if (pressed & PAD_CROSS) { sound_play(SFX_BACK); state = PZ_BOARD; return; }
        if ((pressed & PAD_CIRCLE) && menu_item_held(pick_cur)) {
            box_item = pick_cur;
            state    = PZ_BOARD;
            sound_play(SFX_SELECT);
        }
    }
}

/* ---- Drawing ------------------------------------------------------------- */

static void pp_rect(RenderContext *ctx, int x, int y, int w, int h,
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

static void pp_outline(RenderContext *ctx, int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    pp_rect(ctx, x,       y,       w, 2, r, g, b, ot_idx);
    pp_rect(ctx, x,       y+h-2,   w, 2, r, g, b, ot_idx);
    pp_rect(ctx, x,       y,       2, h, r, g, b, ot_idx);
    pp_rect(ctx, x+w-2,   y,       2, h, r, g, b, ot_idx);
}

/* Same double outline the stove board and the trick drawers use. */
static void pp_cursor(RenderContext *ctx, int x, int y, int w, int h) {
    pp_outline(ctx, x - 4, y - 4, w + 8, h + 8, 80, 80, 200, PP_OT_CURSOR);
    pp_outline(ctx, x - 2, y - 2, w + 4, h + 4, 180, 180, 255, PP_OT_CURSOR);
}

void piano_puzzle_draw(RenderContext *ctx) {
    /* The board only exists while the player can act on it: the intro glide and
       the whole scripted payoff (repair -> pan -> sink) play clean. */
    if (state != PZ_BOARD && state != PZ_PICKER) return;

    /* Reset the texture window before any icon: the room sorts a 128x128 window
       at OT_LENGTH-1 which is still active down here and would wrap the icons'
       UVs (see the same note in menu_draw). */
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            RECT full = {0, 0, 0, 0};
            DR_TWIN *tw = (DR_TWIN *)ctx->next_packet;
            setTexWindow(tw, &full);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[PP_OT_TEXWIN], tw);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    /* --- The item box + PLACE, always on the right --- */
    pp_rect(ctx, BOX_X, BOX_Y, BOX_W, BOX_H, 35, 30, 45, PP_OT_PANEL);
    pp_outline(ctx, BOX_X, BOX_Y, BOX_W, BOX_H, 80, 70, 100, PP_OT_LINE);
    if (box_item >= 0)
        menu_draw_item_icon(ctx, box_item,
                            BOX_X + (BOX_W - BOX_ICON) / 2,
                            BOX_Y + (BOX_H - BOX_ICON) / 2,
                            BOX_ICON, PP_OT_ICON);

    pp_rect(ctx, BOX_X, PLACE_Y, BOX_W, PLACE_H, 25, 35, 45, PP_OT_PANEL);
    pp_outline(ctx, BOX_X, PLACE_Y, BOX_W, PLACE_H, 70, 100, 120, PP_OT_LINE);
    /* "PLACE" is 5 chars at the font's 8px advance. */
    btn_prompt_draw(ctx, BOX_X + (BOX_W - 40) / 2, PLACE_Y + (PLACE_H - 8) / 2,
                    "PLACE", PP_OT_TEXT);

    /* --- Board cursor (hidden while the picker is up) --- */
    if (state == PZ_BOARD) {
        if (board_cur == 1) pp_cursor(ctx, BOX_X, PLACE_Y, BOX_W, PLACE_H);
        else                pp_cursor(ctx, BOX_X, BOX_Y,   BOX_W, BOX_H);
    }

    /* --- Item picker --- */
    if (state == PZ_PICKER) {
        pp_rect(ctx, PICK_X, PICK_Y, PICK_W, PICK_H, 15, 12, 20, PP_OT_PANEL);
        pp_outline(ctx, PICK_X, PICK_Y, PICK_W, PICK_H, 80, 80, 80, PP_OT_LINE);
        btn_prompt_draw(ctx, PICK_X + 8, PICK_Y + 6, "ITEMS", PP_OT_TEXT);

        int s;
        for (s = 0; s < MENU_ITEM_SLOTS; s++) {
            int cx = PICK_GRID_X + (s % PICK_COLS) * PICK_CELL;
            int cy = PICK_GRID_Y + (s / PICK_COLS) * PICK_CELL;
            pp_rect(ctx, cx, cy, PICK_CELL, PICK_CELL, 35, 30, 45, PP_OT_PANEL);
            pp_outline(ctx, cx, cy, PICK_CELL, PICK_CELL, 80, 70, 100, PP_OT_LINE);
            menu_draw_item_icon(ctx, s, cx + PICK_PAD, cy + PICK_PAD,
                                PICK_ICON, PP_OT_ICON);
        }

        {
            int cx = PICK_GRID_X + (pick_cur % PICK_COLS) * PICK_CELL;
            int cy = PICK_GRID_Y + (pick_cur / PICK_COLS) * PICK_CELL;
            pp_cursor(ctx, cx, cy, PICK_CELL, PICK_CELL);
        }

        /* Name of the highlighted item, along the panel's bottom edge. */
        btn_prompt_draw(ctx, PICK_X + 8, PICK_NAME_Y,
                        menu_item_held(pick_cur) ? menu_item_name(pick_cur) : "Empty",
                        PP_OT_TEXT);
    }

    /* --- Controls, same line as the stove board --- */
    if (state == PZ_BOARD)
        btn_prompt_draw(ctx, 8, 206, BTN_CIRCLE " - Select  " BTN_CROSS " - Exit",
                        PP_OT_TEXT);
    else
        btn_prompt_draw(ctx, 8, 206, BTN_CIRCLE " - Select  " BTN_CROSS " - Back",
                        PP_OT_TEXT);
}
