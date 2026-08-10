#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"          /* player_items, show_pickup_msg_raw */
#include "menu.h"            /* MENU_SLOT_*, shared item icons/names */
#include "sound.h"           /* SFX_PICKUP */
#include "btn_glyph.h"       /* BTN_*, btn_prompt_draw */
#include "kitchen_dining.h"  /* kitchen_stove_set_lit */
#include "stove_puzzle.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- The stove ------------------------------------------------------------
   Interact point = the burner the flame rises from (STOVE_FIRE_* in
   kitchen_dining.c), with the same Manhattan trigger radius the old ignite
   prompt used. */
#define SP_STOVE_X          (-107)
#define SP_STOVE_Z           1080
#define SP_INTERACT_RADIUS    500

/* Fixed camera: stands south-east of the hob at the author-specified spot and
   tilts down onto it. Yaw faces the burner (forward = isin/icos of cam_rot, so
   heading atan2(dx,dz) for dx=-343, dz=+209), and the pitch centres a target
   337 below and 402 away — tan(40deg) — i.e. 40deg of 4096.
   The camera was later moved 100 east; yaw/pitch were left as authored, so the
   whole shot pans with it rather than re-aiming (yaw 3360 would recentre the
   burner if that's ever wanted). */
#define SP_CAM_X              336
#define SP_CAM_Y            (-442)
#define SP_CAM_Z              871
#define SP_CAM_ROT           3428
#define SP_CAM_PITCH          455

#define SP_CAM_ANIM_FRAMES     24
#define SP_COOK_FRAMES        180   /* 3 s of flame at 60 fps */

/* ---- Board layout (320x240) ---------------------------------------------- */
#define BOX_X                 244
#define BOX_W                  56
#define BOX_H                  56
/* The column is lifted 20px from its authored position: at COOK_Y 174 the cook
   button's bottom edge (174+28=202) ran under the HUD log box, which starts at
   y=191 and spans x 183..315 — the same columns this board uses. The lift also
   has to clear the 4px the selection cursor draws outside the box. */
#define BOX_A_Y                26
#define BOX_B_Y                90
#define COOK_Y                154
#define COOK_H                 28   /* half-height, as specified */
#define BOX_ICON               40   /* ingredient icon inside a box */

/* Item picker panel + its 2-wide grid (same cell metrics as the pause menu). */
#define PICK_X                 24
#define PICK_Y                 30
#define PICK_W                168
#define PICK_H                168   /* 30..198: clears the controls line at 206 */
#define PICK_CELL              42
#define PICK_ICON              30
#define PICK_PAD                6
/* 3 wide, NOT 2: the panel is only tall enough for three PICK_CELL rows (the
   grid starts at PICK_GRID_Y=+22 and the item name sits at PICK_H-16), so at
   MENU_ITEM_SLOTS=8 a 2-wide grid would need a 4th row and spill past the panel
   onto the controls line. Widening keeps every slot on screen without resizing
   the panel. The picker's grid is independent of the inventory menu's own 2x4
   layout — only the slot NUMBERS are shared. */
#define PICK_COLS               3
#define PICK_ROWS  ((MENU_ITEM_SLOTS + PICK_COLS - 1) / PICK_COLS)
/* Grid centred in the panel, under the header, with room for the item name
   along the bottom edge. */
#define PICK_GRID_X   (PICK_X + (PICK_W - PICK_COLS * PICK_CELL) / 2)
#define PICK_GRID_Y   (PICK_Y + 22)
#define PICK_NAME_Y   (PICK_Y + PICK_H - 16)

/* OT layers — all inside the menu-reserved range (0..SCENE_OT_MIN-1), so the
   board always sits on top of the room. Higher index = drawn first = further
   back (see menu.c). */
#define SP_OT_PANEL            12
#define SP_OT_LINE             10
#define SP_OT_TEXWIN            8   /* reset the 128 window before the icons */
#define SP_OT_ICON              7
#define SP_OT_CURSOR            3
#define SP_OT_TEXT              1

typedef enum {
    SP_IDLE = 0,   /* not in the puzzle: proximity prompt + Circle trigger */
    SP_INTRO,      /* camera gliding to the fixed shot                     */
    SP_BOARD,      /* board live: pick boxes / cook                        */
    SP_PICKER,     /* item picker open for box `pick_target`               */
    SP_COOK        /* burner lit, counting down to the reward             */
} SpState;

static SpState state = SP_IDLE;

/* Ingredient boxes: MENU_SLOT_* of the item shown, or -1 for empty. */
static int box_item[2] = { -1, -1 };
static int board_cur   = 0;    /* 0 = box A, 1 = box B, 2 = COOK */
static int pick_target = 0;    /* box the open picker fills       */
static int pick_cur    = 0;    /* picker cursor: MENU_SLOT_*      */
static int cook_timer  = 0;

/* Pre-puzzle camera, restored on the way out. */
static int32_t save_cx, save_cy, save_cz, save_crot, save_cvy;
static int32_t cam_anim_t = 0;
static int32_t src_x, src_y, src_z, src_rot, rot_delta;

static uint16_t pz_btn_prev  = 0xFFFF;  /* board input edge-detect (btn = ~pad) */
static int      interact_prev = 1;      /* interact-Circle edge-detect          */

int stove_puzzle_active(void) { return state != SP_IDLE; }

/* Owning the stone USED to be the completion flag on its own, but the Attic
   Exit's door puzzle consumes it — so a cook that has happened is now recorded
   in FLAG_STOVE_SOLVED, and spending the stone no longer re-opens the burner.
   The stone is still accepted as proof: saves written before that flag existed
   carry the stone but not the bit, and the debug menu's HAS KEY STONES grant
   hands one out without ever lighting the stove. */
int stove_puzzle_solved(void) {
    return game_flag(FLAG_STOVE_SOLVED) ||
           (player_items & (1 << ITEM_GREEN_KEY_STONE)) != 0;
}

void stove_puzzle_arm(void) {
    int held = interact_tapped();
    interact_prev = held;
    state         = SP_IDLE;
    camera_release_player();
    box_item[0] = box_item[1] = -1;
    board_cur   = 0;
    cook_timer  = 0;
    kitchen_stove_set_lit(0);
}

/* ---- State transitions --------------------------------------------------- */

static void start_puzzle(void) {
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    /* The player stays at the stove while the camera flies overhead — pin their
       position so zombies keep chasing and biting the real spot. */
    camera_anchor_player(save_cx, save_cy, save_cz);

    src_x = cam_x; src_y = cam_y; src_z = cam_z; src_rot = cam_rot;
    /* Shortest signed turn from the current heading to the fixed shot. */
    int32_t d = ((SP_CAM_ROT - src_rot) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    rot_delta = d;

    cam_anim_t = 0;
    state      = SP_INTRO;
    board_cur  = 0;
    box_item[0] = box_item[1] = -1;
}

/* Drop the fixed camera and put the player back exactly where they stood. */
static void exit_puzzle(void) {
    state   = SP_IDLE;
    cam_x   = save_cx; cam_y = save_cy; cam_z = save_cz;
    cam_rot = save_crot; cam_vy = save_cvy;
    cam_pitch = 0;
    camera_release_player();
    interact_prev = 1;   /* swallow the held Circle so we don't re-open instantly */
}

/* An item sitting in the OTHER box is spoken for: the player has one of each,
   so the same item can never fill both slots. */
static int slot_taken(int slot) {
    return box_item[pick_target ^ 1] == slot;
}

/* The one recipe: Copper Pot + Wax Cube, in either box. */
static int recipe_ok(void) {
    int a = box_item[0], b = box_item[1];
    return (a == MENU_SLOT_COPPER_POT && b == MENU_SLOT_WAX_CUBE) ||
           (a == MENU_SLOT_WAX_CUBE   && b == MENU_SLOT_COPPER_POT);
}

static void try_cook(void) {
    if (!recipe_ok()) {
        show_pickup_msg_raw("That doesn't make sense...");
        return;
    }
    state      = SP_COOK;
    cook_timer = SP_COOK_FRAMES;
    kitchen_stove_set_lit(1);
}

static void finish_cook(void) {
    kitchen_stove_set_lit(0);
    game_flag_set(FLAG_STOVE_SOLVED);
    player_items |= (1 << ITEM_GREEN_KEY_STONE);
    sound_play(SFX_PICKUP);
    show_pickup_msg_raw("Received Green Key Stone");
    exit_puzzle();
}

/* ---- Per-frame update ---------------------------------------------------- */

void stove_puzzle_update(void) {
    uint16_t btn = 0;
    if (pad_buff_len[0]) { PadResponse *pad = (PadResponse *)pad_buff[0]; btn = ~pad->btn; }

    if (state == SP_IDLE) {
        if (stove_puzzle_solved()) return;   /* retired: nothing left to cook */
        int held = interact_tapped();
        int just = held && !interact_prev;
        interact_prev = held;
        int32_t dx = cam_x - SP_STOVE_X, dz = cam_z - SP_STOVE_Z;
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (just && xz < SP_INTERACT_RADIUS &&
            interact_facing(SP_STOVE_X, SP_STOVE_Z)) start_puzzle();
        return;
    }

    /* Camera glides to the fixed shot; the board opens once it settles. */
    if (state == SP_INTRO) {
        cam_anim_t++;
        int32_t t = cam_anim_t * 256 / SP_CAM_ANIM_FRAMES; if (t > 256) t = 256;
        int32_t inv = 256 - t;
        int32_t e = 256 - (inv * inv / 256);            /* ease-out 0..256 */
        cam_x     = src_x   + ((SP_CAM_X - src_x) * e) / 256;
        cam_y     = src_y   + ((SP_CAM_Y - src_y) * e) / 256;
        cam_z     = src_z   + ((SP_CAM_Z - src_z) * e) / 256;
        cam_rot   = src_rot + (rot_delta * e) / 256;
        cam_pitch = (SP_CAM_PITCH * e) / 256;
        cam_vy    = 0;
        if (cam_anim_t >= SP_CAM_ANIM_FRAMES) {
            cam_x = SP_CAM_X; cam_y = SP_CAM_Y; cam_z = SP_CAM_Z;
            cam_rot = SP_CAM_ROT; cam_pitch = SP_CAM_PITCH; cam_vy = 0;
            state = SP_BOARD;
            pz_btn_prev = btn;   /* arm: don't treat the held Circle as a select */
        }
        return;
    }

    if (state == SP_COOK) {
        if (--cook_timer <= 0) finish_cook();
        return;               /* no input while the burner is going */
    }

    uint16_t pressed = btn & ~pz_btn_prev;
    pz_btn_prev = btn;

    if (state == SP_BOARD) {
        /* Guarded so a press against either end of the three rows is silent. */
        if ((pressed & PAD_UP)   && board_cur > 0) { board_cur--; sound_play(SFX_CURSOR); }
        if ((pressed & PAD_DOWN) && board_cur < 2) { board_cur++; sound_play(SFX_CURSOR); }
        if (pressed & PAD_CROSS)  { sound_play(SFX_BACK); exit_puzzle(); return; }
        if (pressed & PAD_CIRCLE) {
            /* COOK is confirmed here whether or not the recipe is right: the
               burner's own cue is seconds away on a success (finish_cook) and
               there is none at all on a failure, so this blip is the only
               acknowledgement the press gets either way. */
            sound_play(SFX_SELECT);
            if (board_cur == 2) {
                try_cook();
            } else {
                pick_target = board_cur;
                /* Open on the box's current item, else the first slot. */
                pick_cur = box_item[board_cur] >= 0 ? box_item[board_cur] : 0;
                state    = SP_PICKER;
            }
        }
        return;
    }

    /* SP_PICKER: PICK_COLS-wide grid over every inventory slot. Choosing an item
       copies it into the box — the player keeps it either way. The trailing
       cells of the last row may be past MENU_ITEM_SLOTS; the guard below keeps
       the cursor out of them and the draw loop never paints them. */
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

        if (pressed & PAD_CROSS) { sound_play(SFX_BACK); state = SP_BOARD; return; }
        if ((pressed & PAD_CIRCLE) && menu_item_held(pick_cur) && !slot_taken(pick_cur)) {
            box_item[pick_target] = pick_cur;
            state = SP_BOARD;
            sound_play(SFX_SELECT);
        }
    }
}

/* ---- Drawing ------------------------------------------------------------- */

static void sp_rect(RenderContext *ctx, int x, int y, int w, int h,
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

static void sp_outline(RenderContext *ctx, int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    sp_rect(ctx, x,       y,       w, 2, r, g, b, ot_idx);
    sp_rect(ctx, x,       y+h-2,   w, 2, r, g, b, ot_idx);
    sp_rect(ctx, x,       y,       2, h, r, g, b, ot_idx);
    sp_rect(ctx, x+w-2,   y,       2, h, r, g, b, ot_idx);
}

/* Same double outline the trick-drawers reticule uses. */
static void sp_cursor(RenderContext *ctx, int x, int y, int w, int h) {
    sp_outline(ctx, x - 4, y - 4, w + 8, h + 8, 80, 80, 200, SP_OT_CURSOR);
    sp_outline(ctx, x - 2, y - 2, w + 4, h + 4, 180, 180, 255, SP_OT_CURSOR);
}

void stove_puzzle_draw(RenderContext *ctx) {
    if (state == SP_IDLE || state == SP_INTRO) return;

    /* Reset the texture window before any icon: the kitchen sorts a 128x128
       window at OT_LENGTH-1 which is still active down here and would wrap the
       icons' UVs (see the same note in menu_draw). */
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            RECT full = {0, 0, 0, 0};
            DR_TWIN *tw = (DR_TWIN *)ctx->next_packet;
            setTexWindow(tw, &full);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[SP_OT_TEXWIN], tw);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    /* --- Ingredient boxes + COOK, always on the right --- */
    {
        int i;
        for (i = 0; i < 2; i++) {
            int by = i == 0 ? BOX_A_Y : BOX_B_Y;
            sp_rect(ctx, BOX_X, by, BOX_W, BOX_H, 35, 30, 45, SP_OT_PANEL);
            sp_outline(ctx, BOX_X, by, BOX_W, BOX_H, 80, 70, 100, SP_OT_LINE);
            if (box_item[i] >= 0)
                menu_draw_item_icon(ctx, box_item[i],
                                    BOX_X + (BOX_W - BOX_ICON) / 2,
                                    by + (BOX_H - BOX_ICON) / 2,
                                    BOX_ICON, SP_OT_ICON);
        }

        sp_rect(ctx, BOX_X, COOK_Y, BOX_W, COOK_H, 45, 25, 25, SP_OT_PANEL);
        sp_outline(ctx, BOX_X, COOK_Y, BOX_W, COOK_H, 120, 70, 70, SP_OT_LINE);
        /* "COOK" is 4 chars at the font's 8px advance. */
        btn_prompt_draw(ctx, BOX_X + (BOX_W - 32) / 2, COOK_Y + (COOK_H - 8) / 2,
                        "COOK", SP_OT_TEXT);
    }

    /* --- Board cursor (hidden while the picker is up or the burner is on) --- */
    if (state == SP_BOARD) {
        if (board_cur == 2) sp_cursor(ctx, BOX_X, COOK_Y, BOX_W, COOK_H);
        else                sp_cursor(ctx, BOX_X, board_cur == 0 ? BOX_A_Y : BOX_B_Y,
                                      BOX_W, BOX_H);
    }

    /* --- Item picker --- */
    if (state == SP_PICKER) {
        sp_rect(ctx, PICK_X, PICK_Y, PICK_W, PICK_H, 15, 12, 20, SP_OT_PANEL);
        sp_outline(ctx, PICK_X, PICK_Y, PICK_W, PICK_H, 80, 80, 80, SP_OT_LINE);
        btn_prompt_draw(ctx, PICK_X + 8, PICK_Y + 6, "ITEMS", SP_OT_TEXT);

        int s;
        for (s = 0; s < MENU_ITEM_SLOTS; s++) {
            int cx = PICK_GRID_X + (s % PICK_COLS) * PICK_CELL;
            int cy = PICK_GRID_Y + (s / PICK_COLS) * PICK_CELL;
            /* Items already in the other box get a red cell so it's clear why
               Circle won't take them. */
            int taken = slot_taken(s);
            sp_rect(ctx, cx, cy, PICK_CELL, PICK_CELL,
                    taken ? 60 : 35, taken ? 25 : 30, taken ? 30 : 45, SP_OT_PANEL);
            sp_outline(ctx, cx, cy, PICK_CELL, PICK_CELL,
                       taken ? 140 : 80, taken ? 70 : 70, taken ? 70 : 100, SP_OT_LINE);
            menu_draw_item_icon(ctx, s, cx + PICK_PAD, cy + PICK_PAD,
                                PICK_ICON, SP_OT_ICON);
        }

        {
            int cx = PICK_GRID_X + (pick_cur % PICK_COLS) * PICK_CELL;
            int cy = PICK_GRID_Y + (pick_cur / PICK_COLS) * PICK_CELL;
            sp_cursor(ctx, cx, cy, PICK_CELL, PICK_CELL);
        }

        /* Name of the highlighted item, along the panel's bottom edge. */
        {
            const char *label = "Empty";
            if (slot_taken(pick_cur))         label = "In the other slot";
            else if (menu_item_held(pick_cur)) label = menu_item_name(pick_cur);
            btn_prompt_draw(ctx, PICK_X + 8, PICK_NAME_Y, label, SP_OT_TEXT);
        }
    }

    /* --- Controls, same line as the trick drawers --- */
    if (state == SP_BOARD)
        btn_prompt_draw(ctx, 8, 206, BTN_CIRCLE " - Select  " BTN_CROSS " - Exit",
                        SP_OT_TEXT);
    else if (state == SP_PICKER)
        btn_prompt_draw(ctx, 8, 206, BTN_CIRCLE " - Select  " BTN_CROSS " - Back",
                        SP_OT_TEXT);
}
