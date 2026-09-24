#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"          /* show_pickup_msg_raw */
#include "menu.h"            /* MENU_SLOT_*, shared item icons/names */
#include "sound.h"           /* SFX_CURSOR / SFX_SELECT / SFX_BACK */
#include "btn_glyph.h"       /* BTN_*, btn_prompt_draw */
#include "incinerator.h"
#include "incinerator_panel.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- The conveyor ----------------------------------------------------------
   The tray that sticks out of the machine's WEST end: x[-2000,-1800],
   z[-2950,-2750], top surface around y=-100. Read off assets/props/
   Incinerator.smx, whose vertices are already room coordinates
   (src/incinerator.h), so these are measurements and not a guess.

   The interact point is the tray's outer face at x=-2000, centred on its depth
   — the player walks to the west end of the machine and turns to face it. The
   prop's collision box stops at x=-2000, so a player pushed off it stands at
   -2195 and is 195 away, well inside the trigger. */
#define IP_CONVEYOR_X      (-2000)
#define IP_CONVEYOR_Z      (-2850)
#define IP_INTERACT_RADIUS    500

/* Fixed camera: 583 back and 400 above the tray, looking 34.4 degrees down at
   it, and standing WEST of the machine's box so it never sits inside the model.
   Yaw is atan2(dx,dz) the way cam_rot is measured (the stove's note derives the
   convention and its own numbers check out against it); pitch is atan(drop /
   horizontal) in the same 4096 units.

   >>> THE WHOLE SHOT CAME DOWN 100 WITH THE MACHINE. <<< The re-export that
   shortened it from 400 tall to 300 dropped the conveyor tray's surface from
   y=-200 to y=-100, and the camera dropped the same 100 rather than being
   re-aimed. That is what keeps the framing: the offset from camera to tray is
   unchanged, so yaw and pitch are still the numbers derived above and the tray
   still sits where it did in frame. Re-aiming instead - leaving the camera at
   -600 and tilting further down - would have worked too, but it would have put
   the machine's top edge through the middle of the shot.

   y=-500 is 300 clear of this room's -800 vaulting, so the lift still fits
   under a ceiling that is HALF the Up Down Maze's. */
#define IP_CAM_X           (-2400)
#define IP_CAM_Y            (-500)
#define IP_CAM_Z           (-2550)
#define IP_CAM_ROT           1376
#define IP_CAM_PITCH          392

#define IP_CAM_ANIM_FRAMES     24

/* ---- Board layout (320x240) -------------------------------------------------
   One box, on the right, at the stove board's own metrics and vertically
   centred where its column used to run — so the two boards read as the same
   piece of furniture. The stove's note about the HUD log box (which starts at
   y=191 and spans the same columns) still applies: 90+56 = 146 clears it. */
#define BOX_X                 244
#define BOX_Y                  90
#define BOX_W                  56
#define BOX_H                  56
#define BOX_ICON               40

/* Item picker panel + its grid. Identical to the stove's, deliberately: it is
   the same list of the same items and the player has met it before. */
#define PICK_X                 24
#define PICK_Y                 30
#define PICK_W                168
#define PICK_H                168
#define PICK_CELL              42
#define PICK_ICON              30
#define PICK_PAD                6
/* >>> WIDEN, NEVER ADD A ROW. <<< The panel fits exactly three rows (the grid
   starts at PICK_Y+22 = 52 and the item name sits at 182, so a 4th row would
   run to 220, past the panel's bottom edge at 198 and over the controls line at
   206). 4 x PICK_CELL is 168 = PICK_W exactly. src/stove_puzzle.c carries the
   full derivation and the two must move together — they share MENU_ITEM_SLOTS. */
#define PICK_COLS               4
#define PICK_ROWS  ((MENU_ITEM_SLOTS + PICK_COLS - 1) / PICK_COLS)
#define PICK_GRID_X   (PICK_X + (PICK_W - PICK_COLS * PICK_CELL) / 2)
#define PICK_GRID_Y   (PICK_Y + 22)
#define PICK_NAME_Y   (PICK_Y + PICK_H - 16)

/* OT layers — all inside the menu-reserved range (0..SCENE_OT_MIN-1), so the
   board always sits on top of the room. Higher index = drawn first = further
   back (see menu.c). */
#define IP_OT_PANEL            12
#define IP_OT_LINE             10
#define IP_OT_TEXWIN            8   /* reset the 128 window before the icons */
#define IP_OT_ICON              7
#define IP_OT_CURSOR            3
#define IP_OT_TEXT              1

typedef enum {
    IP_IDLE = 0,   /* not on the board: proximity prompt + Circle trigger */
    IP_INTRO,      /* camera gliding to the fixed shot                    */
    IP_BOARD,      /* the box is live                                     */
    IP_PICKER      /* item picker open                                    */
} IpState;

static IpState state = IP_IDLE;

static int pick_cur = 0;       /* picker cursor: MENU_SLOT_* */

/* Pre-board camera, restored on the way out. */
static int32_t save_cx, save_cy, save_cz, save_crot, save_cvy;
static int32_t cam_anim_t = 0;
static int32_t src_x, src_y, src_z, src_rot, rot_delta;

static uint16_t ip_btn_prev   = 0xFFFF;  /* board input edge-detect (btn = ~pad) */
static int      interact_prev = 1;       /* interact-Circle edge-detect          */

int incinerator_panel_active(void) { return state != IP_IDLE; }

void incinerator_panel_arm(void) {
    int held = interact_tapped();
    interact_prev = held;
    state         = IP_IDLE;
    camera_release_player();
    pick_cur      = 0;
    /* NOTHING IS CLEARED HERE, and that is the difference from
       stove_puzzle_arm(). The stove's boxes are scratch and are emptied on
       every kitchen entry; this machine's hopper is the player's property until
       they burn it, and it survives a room change and a save. */
}

/* ---- State transitions --------------------------------------------------- */

static void start_board(void) {
    int32_t d;

    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    /* The player stays at the conveyor while the camera flies overhead — pin
       their position so anything chasing them keeps chasing the real spot. */
    camera_anchor_player(save_cx, save_cy, save_cz);

    src_x = cam_x; src_y = cam_y; src_z = cam_z; src_rot = cam_rot;
    /* Shortest signed turn from the current heading to the fixed shot. */
    d = ((IP_CAM_ROT - src_rot) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    rot_delta = d;

    cam_anim_t = 0;
    state      = IP_INTRO;
}

/* Drop the fixed camera and put the player back exactly where they stood. */
static void exit_board(void) {
    state   = IP_IDLE;
    cam_x   = save_cx; cam_y = save_cy; cam_z = save_cz;
    cam_rot = save_crot; cam_vy = save_cvy;
    cam_pitch = 0;
    camera_release_player();
    interact_prev = 1;   /* swallow the held Circle so we don't re-open instantly */
}

/* ---- Per-frame update ---------------------------------------------------- */

void incinerator_panel_update(void) {
    uint16_t btn = 0, pressed;
    if (pad_buff_len[0]) { PadResponse *pad = (PadResponse *)pad_buff[0]; btn = ~pad->btn; }

    if (state == IP_IDLE) {
        int held = interact_tapped();
        int just = held && !interact_prev;
        int32_t dx, dz, xz;
        /* THE EDGE STATE IS KEPT CURRENT EVEN WHILE THE MACHINE IS RUNNING, and
           only the trigger is withheld - the same rule the room's interactions
           keep against `lock`. Returning before this line would leave
           interact_prev stale for 336 frames, and the first tap after the cycle
           ended would be swallowed or, worse, a Circle held across the whole
           cycle would fire the board the instant it finished. */
        interact_prev = held;
        /* >>> NO OFFER WHILE IT IS WORKING. <<< The conveyor is closed for the
           length of the cycle and its sign is hidden for exactly the same span
           (src/incinerator_room.c), so the prompt and the press go away and come
           back together. Deliberately NOT a message - there is nothing to tell
           the player that the machine grinding away in front of them does not
           already say. */
        if (incinerator_cycle_active()) return;
        dx = cam_x - IP_CONVEYOR_X; dz = cam_z - IP_CONVEYOR_Z;
        xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (just && xz < IP_INTERACT_RADIUS &&
            interact_facing(IP_CONVEYOR_X, IP_CONVEYOR_Z)) start_board();
        return;
    }

    /* Camera glides to the fixed shot; the board opens once it settles. */
    if (state == IP_INTRO) {
        int32_t t, inv, e;
        cam_anim_t++;
        t = cam_anim_t * 256 / IP_CAM_ANIM_FRAMES; if (t > 256) t = 256;
        inv = 256 - t;
        e = 256 - (inv * inv / 256);            /* ease-out 0..256 */
        cam_x     = src_x   + ((IP_CAM_X - src_x) * e) / 256;
        cam_y     = src_y   + ((IP_CAM_Y - src_y) * e) / 256;
        cam_z     = src_z   + ((IP_CAM_Z - src_z) * e) / 256;
        cam_rot   = src_rot + (rot_delta * e) / 256;
        cam_pitch = (IP_CAM_PITCH * e) / 256;
        cam_vy    = 0;
        if (cam_anim_t >= IP_CAM_ANIM_FRAMES) {
            cam_x = IP_CAM_X; cam_y = IP_CAM_Y; cam_z = IP_CAM_Z;
            cam_rot = IP_CAM_ROT; cam_pitch = IP_CAM_PITCH; cam_vy = 0;
            state = IP_BOARD;
            ip_btn_prev = btn;   /* arm: don't treat the held Circle as a select */
        }
        return;
    }

    pressed = btn & ~ip_btn_prev;
    ip_btn_prev = btn;

    if (state == IP_BOARD) {
        if (pressed & PAD_CROSS) { sound_play(SFX_BACK); exit_board(); return; }
        if (pressed & PAD_CIRCLE) {
            sound_play(SFX_SELECT);
            if (incinerator_slot() >= 0) {
                /* FULL: take it back. The line is worth printing even though the
                   box empties in front of the player, because what changed is
                   their INVENTORY and that is off-screen while this board is
                   up. */
                int slot = incinerator_retrieve();
                if (slot >= 0) show_pickup_msg_raw(menu_item_name(slot));
            } else {
                /* EMPTY: open the picker on the first slot. */
                pick_cur = 0;
                state    = IP_PICKER;
            }
        }
        return;
    }

    /* IP_PICKER: PICK_COLS-wide grid over every inventory slot. Choosing an item
       MOVES it into the machine. The trailing cells of the last row may be past
       MENU_ITEM_SLOTS; the guard below keeps the cursor out of them and the draw
       loop never paints them. */
    {
        int row = pick_cur / PICK_COLS, col = pick_cur % PICK_COLS, next;
        if (pressed & PAD_UP)    { if (row > 0) row--; }
        if (pressed & PAD_DOWN)  { if (row < PICK_ROWS - 1) row++; }
        if (pressed & PAD_LEFT)  { if (col > 0) col--; }
        if (pressed & PAD_RIGHT) { if (col < PICK_COLS - 1) col++; }
        next = row * PICK_COLS + col;
        /* Blipped off the accepted cell, not the press: the guard here rejects a
           step into the last row's trailing cells, and the grid edges reject the
           rest. */
        if (next < MENU_ITEM_SLOTS && next != pick_cur) {
            pick_cur = next;
            sound_play(SFX_CURSOR);
        }

        if (pressed & PAD_CROSS) { sound_play(SFX_BACK); state = IP_BOARD; return; }
        if ((pressed & PAD_CIRCLE) && menu_item_held(pick_cur)) {
            /* >>> incinerator_store() IS ASKED ONCE AND ITS ANSWER IS BELIEVED.
               <<< It is the only code that may move the item off the player, and
               it re-checks the hold itself, so a press that it refuses must not
               fall through to a state change — the board would then show an
               empty box over a lost item. */
            if (incinerator_store(pick_cur) > 0) {
                state = IP_BOARD;
                sound_play(SFX_SELECT);
            }
        }
    }
}

/* ---- Drawing ------------------------------------------------------------- */

static void ip_rect(RenderContext *ctx, int x, int y, int w, int h,
                    uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    TILE *t;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    t = (TILE *)ctx->next_packet;
    setTile(t);
    setXY0(t, x, y);
    setWH(t, w, h);
    setRGB0(t, r, g, b);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[ot_idx], t);
    ctx->next_packet += sizeof(TILE);
}

static void ip_outline(RenderContext *ctx, int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    ip_rect(ctx, x,       y,       w, 2, r, g, b, ot_idx);
    ip_rect(ctx, x,       y+h-2,   w, 2, r, g, b, ot_idx);
    ip_rect(ctx, x,       y,       2, h, r, g, b, ot_idx);
    ip_rect(ctx, x+w-2,   y,       2, h, r, g, b, ot_idx);
}

/* Same double outline the trick-drawers reticule and the stove board use. */
static void ip_cursor(RenderContext *ctx, int x, int y, int w, int h) {
    ip_outline(ctx, x - 4, y - 4, w + 8, h + 8, 80, 80, 200, IP_OT_CURSOR);
    ip_outline(ctx, x - 2, y - 2, w + 4, h + 4, 180, 180, 255, IP_OT_CURSOR);
}

void incinerator_panel_draw(RenderContext *ctx) {
    int slot;

    if (state == IP_IDLE || state == IP_INTRO) return;

    /* Reset the texture window before any icon: the room sorts a 128x128 window
       at OT_LENGTH-1 which is still active down here and would wrap the icons'
       UVs (the same note is in menu_draw and stove_puzzle_draw). */
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            RECT full = {0, 0, 0, 0};
            DR_TWIN *tw = (DR_TWIN *)ctx->next_packet;
            setTexWindow(tw, &full);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[IP_OT_TEXWIN], tw);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    /* --- The one box, always on the right --- */
    slot = incinerator_slot();
    ip_rect(ctx, BOX_X, BOX_Y, BOX_W, BOX_H, 35, 30, 45, IP_OT_PANEL);
    ip_outline(ctx, BOX_X, BOX_Y, BOX_W, BOX_H, 80, 70, 100, IP_OT_LINE);
    if (slot >= 0)
        menu_draw_item_icon(ctx, slot,
                            BOX_X + (BOX_W - BOX_ICON) / 2,
                            BOX_Y + (BOX_H - BOX_ICON) / 2,
                            BOX_ICON, IP_OT_ICON);

    /* --- Board cursor (hidden while the picker is up) --- */
    if (state == IP_BOARD) ip_cursor(ctx, BOX_X, BOX_Y, BOX_W, BOX_H);

    /* --- Item picker --- */
    if (state == IP_PICKER) {
        int s, cx, cy;
        ip_rect(ctx, PICK_X, PICK_Y, PICK_W, PICK_H, 15, 12, 20, IP_OT_PANEL);
        ip_outline(ctx, PICK_X, PICK_Y, PICK_W, PICK_H, 80, 80, 80, IP_OT_LINE);
        btn_prompt_draw(ctx, PICK_X + 8, PICK_Y + 6, "ITEMS", IP_OT_TEXT);

        for (s = 0; s < MENU_ITEM_SLOTS; s++) {
            cx = PICK_GRID_X + (s % PICK_COLS) * PICK_CELL;
            cy = PICK_GRID_Y + (s / PICK_COLS) * PICK_CELL;
            ip_rect(ctx, cx, cy, PICK_CELL, PICK_CELL, 35, 30, 45, IP_OT_PANEL);
            ip_outline(ctx, cx, cy, PICK_CELL, PICK_CELL, 80, 70, 100, IP_OT_LINE);
            menu_draw_item_icon(ctx, s, cx + PICK_PAD, cy + PICK_PAD,
                                PICK_ICON, IP_OT_ICON);
        }

        cx = PICK_GRID_X + (pick_cur % PICK_COLS) * PICK_CELL;
        cy = PICK_GRID_Y + (pick_cur / PICK_COLS) * PICK_CELL;
        ip_cursor(ctx, cx, cy, PICK_CELL, PICK_CELL);

        /* Name of the highlighted item, along the panel's bottom edge. */
        btn_prompt_draw(ctx, PICK_X + 8, PICK_NAME_Y,
                        menu_item_held(pick_cur) ? menu_item_name(pick_cur) : "Empty",
                        IP_OT_TEXT);
    }

    /* --- Controls, same line as the stove board --- */
    if (state == IP_BOARD)
        btn_prompt_draw(ctx, 8, 206,
                        incinerator_slot() >= 0
                            ? BTN_CIRCLE " - Take  " BTN_CROSS " - Exit"
                            : BTN_CIRCLE " - Place  " BTN_CROSS " - Exit",
                        IP_OT_TEXT);
    else if (state == IP_PICKER)
        btn_prompt_draw(ctx, 8, 206, BTN_CIRCLE " - Select  " BTN_CROSS " - Back",
                        IP_OT_TEXT);
}
