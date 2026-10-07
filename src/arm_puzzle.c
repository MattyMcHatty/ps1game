#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"          /* player_blood_pearls, game flags, the log      */
#include "menu.h"            /* MENU_SLOT_*, shared item icons/names          */
#include "sound.h"           /* SFX_CURSOR / SFX_SELECT / SFX_BACK / SFX_UNLOCK */
#include "btn_glyph.h"       /* BTN_*, btn_prompt_draw                        */
#include "door.h"            /* door_draw_string_3d: the "Press O" sign       */
#include "arm_switch.h"
#include "arm_puzzle.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- The answer ------------------------------------------------------------
   Arms 2, 3 and 6 counted from the NORTH; the switch array counts from the
   south (arm_puzzle.h), so arm n is index ARM_SWITCH_MAX - n. */
#define AP_ARM(n)        (1 << (ARM_SWITCH_MAX - (n)))
#define AP_SOLUTION      (AP_ARM(2) | AP_ARM(3) | AP_ARM(6))
#define AP_ALL_ARMS      ((1 << ARM_SWITCH_MAX) - 1)

/* ---- The turn --------------------------------------------------------------
   "Down a little": 12 degrees (4096 = 360). The hand is ~200 from the
   shoulder, so it drops ~40 — plainly visible from the floor, and nowhere
   near the bottom of the relief. Eased over AP_TURN_FRAMES, and the unlock
   lands as it ends.

   45 FRAMES IS THE GRIND'S LENGTH: SFX_STONE_SLIDE runs 0.75 s, so the arm
   stops moving as the stone stops sounding, and the unlock sound comes after
   it rather than over it. */
#define AP_DOWN_ANGLE     136
#define AP_TURN_FRAMES     45
#define AP_TURN_STEP     ((AP_DOWN_ANGLE + AP_TURN_FRAMES - 1) / AP_TURN_FRAMES)

/* ---- Reach -----------------------------------------------------------------
   The arms stand east of the proxy's east wall (x=2210), which holds the
   player at x=2015 at the closest. The interact point is that wall face at
   the arm's Z: 195 away at best, so AP_REACH leaves 225 along the wall either
   side — less than half the 272 between the two closest arms, so at most two
   are ever in reach and the NEARER one wins. */
#define AP_WALL_X        2210
#define AP_REACH          420
#define AP_TEXT_RADIUS    800
#define AP_FADE_NEAR      500

/* The sign FLOATS IN THE HAND: 6 west of the fingertips (x=2221), so it
   stands just in front of them, with its bottom edge on the palm of an EMPTY
   hand (the palm's top is y~-176 at rest). The glyphs are 7 font pixels tall,
   so the top is 7 * DOOR_PIXEL_SIZE above that. FIXED, whatever the pose: an
   arm that drops with a pearl leaves the sign where it was, just above the
   pearl. YZ plane read from -X — mirror=1, and the -200 door_draw_string_3d
   wants on the reading axis, which for YZ is Z. */
#define AP_SIGN_X        2215
#define AP_SIGN_Y        (-180 - 7 * DOOR_PIXEL_SIZE)

/* ---- The shot --------------------------------------------------------------
   Above and west of the arm, on its own Z, facing +X (cam_rot 1024) and
   looking down at the middle of the arm — (2320, -185), between the hand and
   the shoulder. From (1960, -470) that is 360 across and 285 down: atan(285 /
   360) = 38.4 degrees = 437 in 4096ths. Inside the room (west of the 2210
   wall, 330 under the -800 ceiling), so nothing has to be culled for it, and
   facing AWAY from everything behind it, so the draw loops' behind-the-yaw
   cull never bites. */
#define AP_CAM_X         1960
#define AP_CAM_Y         (-470)
#define AP_CAM_ROT       1024
#define AP_CAM_PITCH      437
#define AP_CAM_ANIM_FRAMES 24

/* ---- Board layout (320x240) -------------------------------------------------
   The Incinerator board's metrics exactly (src/incinerator_panel.c says why:
   the stove's board, the HUD log box, and the three-row picker). */
#define BOX_X                 244
#define BOX_Y                  90
#define BOX_W                  56
#define BOX_H                  56
#define BOX_ICON               40

#define PICK_X                 24
#define PICK_Y                 30
#define PICK_W                168
#define PICK_H                168
#define PICK_CELL              42
#define PICK_ICON              30
#define PICK_PAD                6
#define PICK_COLS               4
#define PICK_ROWS  ((MENU_ITEM_CELLS + PICK_COLS - 1) / PICK_COLS)
_Static_assert(PICK_ROWS <= 3, "the picker panel fits three rows");
#define PICK_GRID_X   (PICK_X + (PICK_W - PICK_COLS * PICK_CELL) / 2)
#define PICK_GRID_Y   (PICK_Y + 22)
#define PICK_NAME_Y   (PICK_Y + PICK_H - 16)

#define AP_OT_PANEL            12
#define AP_OT_LINE             10
#define AP_OT_TEXWIN            8
#define AP_OT_ICON              7
#define AP_OT_COUNT             5   /* in front of the icon (incinerator_panel) */
#define AP_OT_CURSOR            3
#define AP_OT_TEXT              1

typedef enum {
    AP_IDLE = 0,   /* free roam: sign + Circle trigger           */
    AP_INTRO,      /* camera gliding to the arm                  */
    AP_BOARD,      /* the box is live                            */
    AP_PICKER      /* item picker open                           */
} ApState;

/* ---- Persistent: which hands hold a pearl (saved) ---- */
static uint8_t ap_pearls = 0;

/* ---- Per room visit ---- */
static ApState state     = AP_IDLE;
static int     ap_arm    = -1;    /* the switch the board is on              */
static int     ap_target = -1;    /* the switch in reach and faced, or -1    */
static int     ap_box_item = -1;  /* a NON-pearl shown in the box: display
                                     only, never taken off the player        */
static int     pick_cur  = 0;
static int32_t ap_angle[ARM_SWITCH_MAX];
static int     ap_unlock_timer = 0;   /* >0: frames until the unlock lands   */

static int32_t save_cx, save_cy, save_cz, save_crot, save_cvy;
static int32_t cam_anim_t = 0;
static int32_t src_x, src_y, src_z, src_rot, rot_delta;
static int32_t dst_z;

static uint16_t ap_btn_prev   = 0xFFFF;
static int      interact_prev = 1;

/* ---- Save / reset ---------------------------------------------------------- */

void arm_puzzle_reset(void)          { ap_pearls = 0; ap_unlock_timer = 0; }
int  arm_puzzle_pearls(void)         { return ap_pearls; }
void arm_puzzle_set_pearls(int bits) { ap_pearls = (uint8_t)(bits & AP_ALL_ARMS); }
int  arm_puzzle_solved(void)         { return game_flag(FLAG_HEAD_ARMS_SOLVED); }
int  arm_puzzle_active(void)         { return state != AP_IDLE; }
int  arm_puzzle_targeted(void)       { return state == AP_IDLE && ap_target >= 0; }

static int32_t ap_rest_angle(int i) {
    return (ap_pearls & (1 << i)) ? AP_DOWN_ANGLE : 0;
}

/* Hand every switch its pose. The arm under an open board keeps the angle it
   had when the board opened — it turns once the player has stepped back. */
static void ap_push_poses(void) {
    int i, n = arm_switch_count();
    for (i = 0; i < n; i++)
        arm_switch_set_pose(i, ap_angle[i], (ap_pearls >> i) & 1);
}

void arm_puzzle_arm(void) {
    int i;
    interact_prev = interact_tapped();
    state         = AP_IDLE;
    ap_arm        = -1;
    ap_target     = -1;
    ap_box_item   = -1;
    pick_cur      = 0;
    ap_unlock_timer = 0;
    camera_release_player();
    for (i = 0; i < ARM_SWITCH_MAX; i++) ap_angle[i] = ap_rest_angle(i);
    ap_push_poses();
}

/* ---- The log ---------------------------------------------------------------- */

static void ap_log(const char *pre, int slot, const char *post) {
    char buf[64];
    const char *parts[3];
    int i = 0, p;
    parts[0] = pre; parts[1] = menu_item_name(slot); parts[2] = post;
    for (p = 0; p < 3; p++) {
        const char *s = parts[p];
        while (*s && i < 63) buf[i++] = *s++;
    }
    buf[i] = '\0';
    show_pickup_msg_raw(buf);
}

/* What the box shows: the pearl if the hand really holds one, else whatever
   non-pearl the player is trying, else -1. */
static int ap_box_slot(void) {
    if (ap_arm >= 0 && (ap_pearls & (1 << ap_arm))) return MENU_SLOT_BLOOD_PEARL;
    return ap_box_item;
}

/* ---- Transitions ------------------------------------------------------------ */

static void start_board(int arm) {
    int32_t d;
    ap_arm      = arm;
    ap_box_item = -1;

    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    camera_anchor_player(save_cx, save_cy, save_cz);

    src_x = cam_x; src_y = cam_y; src_z = cam_z; src_rot = cam_rot;
    dst_z = arm_switch_z(arm);
    d = ((AP_CAM_ROT - src_rot) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    rot_delta = d;

    cam_anim_t = 0;
    state      = AP_INTRO;
}

static void exit_board(void) {
    /* ANYTHING BUT A PEARL GOES BACK. It never left the inventory (see the
       header), so "returning" it is only clearing the box — and saying so. */
    if (ap_box_item >= 0) {
        ap_box_item = -1;
        show_pickup_msg_raw("That doesn't make sense...");
    }

    state   = AP_IDLE;
    cam_x   = save_cx; cam_y = save_cy; cam_z = save_cz;
    cam_rot = save_crot; cam_vy = save_cvy;
    cam_pitch = 0;
    camera_release_player();
    interact_prev = 1;
    /* THE GRIND, as the arm starts to move: only if the board changed what
       it holds, so opening and closing it without a change is silent. Up or
       down, it is the same stone. */
    if (ap_angle[ap_arm] != ap_rest_angle(ap_arm)) sound_play(SFX_STONE_SLIDE);
    ap_arm = -1;

    /* THE ANSWER, checked as the player steps back. The flag is set NOW, so a
       room change during the turn cannot lose it; only the sound and the line
       wait for the arm to finish moving. */
    if (!arm_puzzle_solved() && ap_pearls == AP_SOLUTION) {
        game_flag_set(FLAG_HEAD_ARMS_SOLVED);
        ap_unlock_timer = AP_TURN_FRAMES;
    }
}

/* ---- Per frame -------------------------------------------------------------- */

/* The nearest switch in reach that the player is facing, or -1. */
static int ap_find_target(void) {
    int i, n = arm_switch_count(), best = -1;
    int32_t best_d = AP_REACH;
    for (i = 0; i < n; i++) {
        int32_t z  = arm_switch_z(i);
        int32_t dx = cam_x - AP_WALL_X, dz = cam_z - z;
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (xz < best_d && interact_facing(AP_WALL_X, z)) { best_d = xz; best = i; }
    }
    return best;
}

static void ap_turn_arms(void) {
    int i;
    for (i = 0; i < ARM_SWITCH_MAX; i++) {
        int32_t target;
        if (state != AP_IDLE && i == ap_arm) continue;   /* frozen under the board */
        target = ap_rest_angle(i);
        if (ap_angle[i] < target) {
            ap_angle[i] += AP_TURN_STEP;
            if (ap_angle[i] > target) ap_angle[i] = target;
        } else if (ap_angle[i] > target) {
            ap_angle[i] -= AP_TURN_STEP;
            if (ap_angle[i] < target) ap_angle[i] = target;
        }
    }
    ap_push_poses();

    if (ap_unlock_timer > 0 && --ap_unlock_timer == 0) {
        sound_play(SFX_UNLOCK);
        show_pickup_msg_raw("You unlocked the door.");
    }
}

void arm_puzzle_update(void) {
    uint16_t btn = 0, pressed;
    if (pad_buff_len[0]) { PadResponse *pad = (PadResponse *)pad_buff[0]; btn = ~pad->btn; }

    ap_turn_arms();

    if (state == AP_IDLE) {
        int held = interact_tapped();
        int just = held && !interact_prev;
        interact_prev = held;
        /* LOCKED OUT once solved: the pearls are spent, and no arm offers its
           sign or its board again. */
        ap_target = arm_puzzle_solved() ? -1 : ap_find_target();
        if (just && ap_target >= 0) start_board(ap_target);
        return;
    }

    if (state == AP_INTRO) {
        int32_t t, inv, e;
        cam_anim_t++;
        t = cam_anim_t * 256 / AP_CAM_ANIM_FRAMES; if (t > 256) t = 256;
        inv = 256 - t;
        e = 256 - (inv * inv / 256);
        cam_x     = src_x   + ((AP_CAM_X - src_x) * e) / 256;
        cam_y     = src_y   + ((AP_CAM_Y - src_y) * e) / 256;
        cam_z     = src_z   + ((dst_z    - src_z) * e) / 256;
        cam_rot   = src_rot + (rot_delta * e) / 256;
        cam_pitch = (AP_CAM_PITCH * e) / 256;
        cam_vy    = 0;
        if (cam_anim_t >= AP_CAM_ANIM_FRAMES) {
            cam_x = AP_CAM_X; cam_y = AP_CAM_Y; cam_z = dst_z;
            cam_rot = AP_CAM_ROT; cam_pitch = AP_CAM_PITCH; cam_vy = 0;
            state = AP_BOARD;
            ap_btn_prev = btn;
        }
        return;
    }

    pressed = btn & ~ap_btn_prev;
    ap_btn_prev = btn;

    if (state == AP_BOARD) {
        if (pressed & PAD_CROSS) { sound_play(SFX_BACK); exit_board(); return; }
        if (pressed & PAD_CIRCLE) {
            sound_play(SFX_SELECT);
            if (ap_pearls & (1 << ap_arm)) {
                /* The pearl comes back out of the hand. */
                ap_pearls &= (uint8_t)~(1 << ap_arm);
                if (player_blood_pearls < BLOOD_PEARLS_MAX) player_blood_pearls++;
                menu_inventory_sync();
                ap_log("You retrieved the ", MENU_SLOT_BLOOD_PEARL, "");
            } else if (ap_box_item >= 0) {
                int slot = ap_box_item;
                ap_box_item = -1;
                ap_log("You retrieved the ", slot, "");
            } else {
                menu_inventory_sync();
                pick_cur = 0;
                state    = AP_PICKER;
            }
        }
        return;
    }

    /* AP_PICKER */
    {
        int row = pick_cur / PICK_COLS, col = pick_cur % PICK_COLS, next;
        if (pressed & PAD_UP)    { if (row > 0) row--; }
        if (pressed & PAD_DOWN)  { if (row < PICK_ROWS - 1) row++; }
        if (pressed & PAD_LEFT)  { if (col > 0) col--; }
        if (pressed & PAD_RIGHT) { if (col < PICK_COLS - 1) col++; }
        next = row * PICK_COLS + col;
        if (next < MENU_ITEM_CELLS && next != pick_cur) {
            pick_cur = next;
            sound_play(SFX_CURSOR);
        }

        if (pressed & PAD_CROSS) { sound_play(SFX_BACK); state = AP_BOARD; return; }
        if ((pressed & PAD_CIRCLE) && menu_item_held(menu_item_at_cell(pick_cur))) {
            int it = menu_item_at_cell(pick_cur);
            sound_play(SFX_SELECT);
            if (it == MENU_SLOT_BLOOD_PEARL) {
                /* The one thing that really moves. menu_item_held() said the
                   count is above zero, so this cannot go negative. */
                player_blood_pearls--;
                ap_pearls |= (uint8_t)(1 << ap_arm);
                menu_inventory_sync();
            } else {
                ap_box_item = it;   /* shown only — see the header */
            }
            ap_log("You placed the ", it, " in the hand");
            state = AP_BOARD;
        }
    }
}

/* ---- Drawing ---------------------------------------------------------------- */

static void ap_rect(RenderContext *ctx, int x, int y, int w, int h,
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

static void ap_outline(RenderContext *ctx, int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    ap_rect(ctx, x,       y,       w, 2, r, g, b, ot_idx);
    ap_rect(ctx, x,       y+h-2,   w, 2, r, g, b, ot_idx);
    ap_rect(ctx, x,       y,       2, h, r, g, b, ot_idx);
    ap_rect(ctx, x+w-2,   y,       2, h, r, g, b, ot_idx);
}

static void ap_cursor(RenderContext *ctx, int x, int y, int w, int h) {
    ap_outline(ctx, x - 4, y - 4, w + 8, h + 8, 80, 80, 200, AP_OT_CURSOR);
    ap_outline(ctx, x - 2, y - 2, w + 4, h + 4, 180, 180, 255, AP_OT_CURSOR);
}

/* The "Press O" sign over the arm in reach — ONE at a time, the nearest, so
   the eight never stack into an unreadable row. Faded by distance like every
   other sign. */
static void ap_sign(RenderContext *ctx) {
    int i, n = arm_switch_count(), best = -1, fade = 256;
    int32_t best_d = AP_TEXT_RADIUS, z;
    if (arm_puzzle_solved()) return;
    for (i = 0; i < n; i++) {
        int32_t dx = cam_x - AP_WALL_X, dz = cam_z - arm_switch_z(i);
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (xz < best_d) { best_d = xz; best = i; }
    }
    if (best < 0) return;
    if (best_d > AP_FADE_NEAR) {
        int range = AP_TEXT_RADIUS - AP_FADE_NEAR;
        int prog  = best_d - AP_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    z = arm_switch_z(best);
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE,
                        AP_SIGN_X, AP_SIGN_Y, z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ,
                        DOOR_PIXEL_SIZE);
}

void arm_puzzle_draw(RenderContext *ctx) {
    int slot;

    if (state == AP_IDLE)  { ap_sign(ctx); return; }
    if (state == AP_INTRO) return;

    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            RECT full = {0, 0, 0, 0};
            DR_TWIN *tw = (DR_TWIN *)ctx->next_packet;
            setTexWindow(tw, &full);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[AP_OT_TEXWIN], tw);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    /* --- The one box --- */
    slot = ap_box_slot();
    ap_rect(ctx, BOX_X, BOX_Y, BOX_W, BOX_H, 35, 30, 45, AP_OT_PANEL);
    ap_outline(ctx, BOX_X, BOX_Y, BOX_W, BOX_H, 80, 70, 100, AP_OT_LINE);
    /* _any: a pearl in the hand has left the inventory, and the gated form
       would draw nothing for an item the player no longer holds. */
    if (slot >= 0)
        menu_draw_item_icon_any(ctx, slot,
                                BOX_X + (BOX_W - BOX_ICON) / 2,
                                BOX_Y + (BOX_H - BOX_ICON) / 2,
                                BOX_ICON, AP_OT_ICON);

    if (state == AP_BOARD) ap_cursor(ctx, BOX_X, BOX_Y, BOX_W, BOX_H);

    if (state == AP_PICKER) {
        int s, cx, cy;
        ap_rect(ctx, PICK_X, PICK_Y, PICK_W, PICK_H, 15, 12, 20, AP_OT_PANEL);
        ap_outline(ctx, PICK_X, PICK_Y, PICK_W, PICK_H, 80, 80, 80, AP_OT_LINE);
        btn_prompt_draw(ctx, PICK_X + 8, PICK_Y + 6, "ITEMS", AP_OT_TEXT);

        for (s = 0; s < MENU_ITEM_CELLS; s++) {
            int it = menu_item_at_cell(s);
            cx = PICK_GRID_X + (s % PICK_COLS) * PICK_CELL;
            cy = PICK_GRID_Y + (s / PICK_COLS) * PICK_CELL;
            ap_rect(ctx, cx, cy, PICK_CELL, PICK_CELL, 35, 30, 45, AP_OT_PANEL);
            ap_outline(ctx, cx, cy, PICK_CELL, PICK_CELL, 80, 70, 100, AP_OT_LINE);
            menu_draw_item_icon(ctx, it, cx + PICK_PAD, cy + PICK_PAD,
                                PICK_ICON, AP_OT_ICON);
            /* What the player carries, as the inventory shows it. */
            if (it >= 0) {
                int count = menu_item_count(it);
                if (count > 0)
                    menu_draw_count(ctx,
                                    cx + PICK_PAD + PICK_ICON - menu_count_width(count, 2),
                                    cy + PICK_PAD + PICK_ICON, count, 2, AP_OT_COUNT);
            }
        }

        cx = PICK_GRID_X + (pick_cur % PICK_COLS) * PICK_CELL;
        cy = PICK_GRID_Y + (pick_cur / PICK_COLS) * PICK_CELL;
        ap_cursor(ctx, cx, cy, PICK_CELL, PICK_CELL);

        btn_prompt_draw(ctx, PICK_X + 8, PICK_NAME_Y,
                        menu_item_held(menu_item_at_cell(pick_cur))
                            ? menu_item_name(menu_item_at_cell(pick_cur)) : "Empty",
                        AP_OT_TEXT);
    }

    if (state == AP_BOARD)
        btn_prompt_draw(ctx, 8, 206,
                        slot >= 0
                            ? BTN_CIRCLE " - Take  " BTN_CROSS " - Exit"
                            : BTN_CIRCLE " - Place  " BTN_CROSS " - Exit",
                        AP_OT_TEXT);
    else if (state == AP_PICKER)
        btn_prompt_draw(ctx, 8, 206, BTN_CIRCLE " - Select  " BTN_CROSS " - Back",
                        AP_OT_TEXT);
}
