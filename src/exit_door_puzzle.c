#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"          /* game_flag, show_pickup_msg_raw */
#include "menu.h"            /* MENU_SLOT_*, shared item icons/names */
#include "sound.h"           /* SFX_UNLOCK */
#include "btn_glyph.h"       /* BTN_*, btn_prompt_draw */
#include "door.h"            /* door_draw_string_3d for the world-space sign */
#include "attic_exit.h"      /* attic_exit_unlock_door */
#include "exit_door_puzzle.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- The door --------------------------------------------------------------
   Read straight off "Attic Exit.smx": the xt_dr_lckd panel spans x[-225,225],
   y[-467,0] at z=1000 (the room's north wall), inside the chainlink cage. The
   player can only reach it once the lightswitch puzzle has winched the gate
   away, so no extra gating is needed here — with the gate down the nearest a
   player can stand is ~985 away, well outside both radii below. */
#define XD_DOOR_X                 0
#define XD_DOOR_Z              1000
#define XD_INTERACT_RADIUS      500
#define XD_TEXT_RADIUS          900
#define XD_FADE_NEAR            600
/* Sign ON the door, at the same height as the room's entry-door sign
   (AEXIT_TEXT_Y in attic_exit.c) so the two read as the same piece of signage —
   near eye level rather than up by the ceiling, where it was lost against the
   roof. Read from the -Z (room) side, so mirror=0 — the same orientation as the
   lightswitch puzzle's north-wall pair. Pixel size 3, not the entry door's 4:
   "Press O to interact" is 19 chars, which at 18 world units per char is 342
   wide and fits inside the 450-wide panel (at 4 it would overhang). */
#define XD_TEXT_Y             (-186)
#define XD_TEXT_PIXEL             3

/* ---- The fixed shot --------------------------------------------------------
   Straight down the room's spine at the door's own centre height, 700 back from
   its face. At gte_SetGeomScreen(256) that puts the 450x467 panel across
   x[78,242] y[35,205] of the 320x240 screen — the whole door in frame, with the
   socket diamond beside it in the right of the screen. Yaw 0 (facing +Z) and
   pitch 0, so moving XD_CAM_X/Z means re-deriving both. */
#define XD_CAM_X                  0
#define XD_CAM_Y              (-233)
#define XD_CAM_Z                300
#define XD_CAM_ROT                0
#define XD_CAM_ANIM_FRAMES       24

/* ---- Board layout (320x240) ------------------------------------------------
   Four sockets in a diamond: one top, two flanking, one bottom (the fixed
   magenta stone). The diamond sits in the RIGHT of the screen, clear of the
   item picker panel below — the picker occupies x[24,192], and the diamond's
   leftmost edge is 202, so the two never overlap and the board stays readable
   with the picker open.

   The flanking pair are spaced by XD_BOX_GAP rather than the diamond's height,
   so they sit close without touching. Column centre is 250: the pair spans
   2*44 + 8 = 96 centred on it, and the top/bottom sockets are centred on it
   too. */
#define BOX_W                    44
#define BOX_H                    44
#define BOX_ICON                 32
#define BOX_PAD    ((BOX_W - BOX_ICON) / 2)
#define XD_BOX_GAP                8   /* between the flanking pair */
#define XD_BOX_CX               250   /* the diamond's centre column */

#define XD_BOX_TOP                0
#define XD_BOX_LEFT               1
#define XD_BOX_RIGHT              2
#define XD_BOX_COUNT              3   /* the PLACEABLE sockets */

#define XD_MID_L   (XD_BOX_CX - XD_BOX_GAP / 2 - BOX_W)   /* 202 */
#define XD_MID_R   (XD_BOX_CX + XD_BOX_GAP / 2)           /* 254 */
#define XD_COL_X   (XD_BOX_CX - BOX_W / 2)                /* 228 */

static const struct { int x, y; } BOX_XY[XD_BOX_COUNT] = {
    /* TOP   */ { XD_COL_X,  46 },
    /* LEFT  */ { XD_MID_L, 100 },
    /* RIGHT */ { XD_MID_R, 100 },
};
#define XD_FIXED_X         XD_COL_X   /* the magenta stone's socket */
#define XD_FIXED_Y              154

/* Item picker panel + its grid — the same metrics as the stove puzzle's, so the
   two pickers read as one piece of UI. */
#define PICK_X                   24
#define PICK_Y                   30
#define PICK_W                  168
#define PICK_H                  168
#define PICK_CELL                42
#define PICK_ICON                30
#define PICK_PAD                  6
#define PICK_COLS                 3
#define PICK_ROWS  ((MENU_ITEM_SLOTS + PICK_COLS - 1) / PICK_COLS)
#define PICK_GRID_X   (PICK_X + (PICK_W - PICK_COLS * PICK_CELL) / 2)
#define PICK_GRID_Y   (PICK_Y + 22)
#define PICK_NAME_Y   (PICK_Y + PICK_H - 16)

/* OT layers — all inside the menu-reserved range (0..SCENE_OT_MIN-1), so the
   board always sits on top of the room. Higher index = drawn first = further
   back (see menu.c). */
#define XD_OT_PANEL              12
#define XD_OT_LINE               10
#define XD_OT_TEXWIN              8   /* reset the 128 window before the icons */
#define XD_OT_ICON                7
#define XD_OT_CURSOR              3
#define XD_OT_TEXT                1

typedef enum {
    XD_IDLE = 0,   /* not in the puzzle: proximity prompt + Circle trigger */
    XD_INTRO,      /* camera gliding back to the fixed shot                */
    XD_BOARD,      /* board live: move between the three open sockets      */
    XD_PICKER      /* item picker open for socket `pick_target`            */
} XdState;

static XdState state = XD_IDLE;

/* Each open socket holds a MENU_SLOT_* or -1. The fixed bottom socket is not in
   here — it is always the magenta stone and never changes. */
static int box_item[XD_BOX_COUNT] = { -1, -1, -1 };
static int board_cur   = XD_BOX_TOP;
static int pick_target = XD_BOX_TOP;
static int pick_cur    = 0;

/* Pre-puzzle camera, restored on the way out. */
static int32_t save_cx, save_cy, save_cz, save_crot, save_cvy;
static int32_t cam_anim_t = 0;
static int32_t src_x, src_y, src_z, src_rot, rot_delta;

static uint16_t pz_btn_prev   = 0xFFFF;  /* board input edge-detect (btn = ~pad) */
static int      interact_prev = 1;       /* interact-Circle edge-detect          */
static int      exit_latch    = 0;       /* TEMPORARY: the "Press O to exit" press */

/* ---- The magenta stone icon ------------------------------------------------
   Not an inventory item, so it has no menu slot: it is its own 32x32 TIM at a
   permanently resident VRAM slot (x672 y256, Voff 0 — window-safe), uploaded
   once at startup like the Anzu tiles. */
static uint16_t mgn_tpage = 0, mgn_clut = 0;
static uint8_t  mgn_u0 = 0, mgn_v0 = 0, mgn_u1 = 0, mgn_v1 = 0;

void exit_door_puzzle_load_assets(void) {
    CdlFILE file;
    if (!CdSearchFile(&file, "\\TEX\\MGNKYSTN.TIM;1")) return;
    int   sectors = (file.size + 2047) / 2048;
    void *buf     = malloc(sectors * 2048);
    if (!buf) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);

    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);
    if (tim.mode & 0x8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
        mgn_clut = getClut(tim.crect->x, tim.crect->y);
    }
    mgn_tpage = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);

    /* UV within the tpage — same derivation as menu.c's load_icon_tim: U0 is the
       texture's x WITHIN its 64-word page, scaled by pixels-per-word. */
    int bpp_mode = tim.mode & 3;
    int px_mult  = (bpp_mode == 0) ? 4 : (bpp_mode == 1) ? 2 : 1;
    int u_off    = (tim.prect->x & 63) * px_mult;
    mgn_u0 = (uint8_t)u_off;
    mgn_v0 = (uint8_t)(tim.prect->y % 256);
    mgn_u1 = (uint8_t)(u_off + tim.prect->w * px_mult - 1);
    mgn_v1 = (uint8_t)(mgn_v0 + tim.prect->h - 1);

    free(buf);
}

int exit_door_puzzle_active(void) { return state != XD_IDLE; }

static int solved(void) { return game_flag(FLAG_EXIT_DOOR_UNLOCKED); }

void exit_door_puzzle_arm(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    interact_prev = held;
    state         = XD_IDLE;
    exit_latch    = 0;
    board_cur     = XD_BOX_TOP;
    box_item[0] = box_item[1] = box_item[2] = -1;
    camera_release_player();
}

int exit_door_exit_triggered(void) {
    int t = exit_latch;
    exit_latch = 0;
    return t;
}

/* ---- State transitions --------------------------------------------------- */

static void start_puzzle(void) {
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    /* The player stays at the door while the camera pulls back — pin their
       position so anything hunting them keeps hunting the real spot. */
    camera_anchor_player(save_cx, save_cy, save_cz);

    src_x = cam_x; src_y = cam_y; src_z = cam_z; src_rot = cam_rot;
    /* Shortest signed turn from the current heading to the fixed shot. */
    int32_t d = ((XD_CAM_ROT - src_rot) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    rot_delta = d;

    cam_anim_t = 0;
    state      = XD_INTRO;
    board_cur  = XD_BOX_TOP;
}

/* Drop the fixed camera and put the player back exactly where they stood.
   Backing out empties the three open sockets, so the board is always approached
   fresh rather than resuming a half-finished attempt. Nothing is given back:
   placing an item only ever copied it into the socket, so the player has held it
   the whole time (the door only SPENDS the stones on a correct combination, in
   unlock_door). The magenta stone is not in box_item at all — it is part of the
   lock, not a placement — so it stays put. */
static void exit_puzzle(void) {
    box_item[0] = box_item[1] = box_item[2] = -1;
    board_cur   = XD_BOX_TOP;
    state       = XD_IDLE;
    cam_x   = save_cx; cam_y = save_cy; cam_z = save_cz;
    cam_rot = save_crot; cam_vy = save_cvy;
    cam_pitch = 0;
    camera_release_player();
    interact_prev = 1;   /* swallow the held Circle so we don't re-open instantly */
}

/* An item sitting in ANOTHER socket is spoken for: the player holds one of
   each, so the same item can never fill two sockets. */
static int slot_taken(int slot) {
    int i;
    for (i = 0; i < XD_BOX_COUNT; i++)
        if (i != pick_target && box_item[i] == slot) return 1;
    return 0;
}

/* The one combination. Anything else — including the right three stones in the
   wrong sockets — simply sits there. */
static int combination_ok(void) {
    return box_item[XD_BOX_TOP]   == MENU_SLOT_GREEN_KEY_STONE &&
           box_item[XD_BOX_LEFT]  == MENU_SLOT_YELLOW_KEY_STONE &&
           box_item[XD_BOX_RIGHT] == MENU_SLOT_BLUE_KEY_STONE;
}

static void unlock_door(void) {
    /* The stones stay in the door: spend all three. Nothing respawns them —
       the blue one is a first-visit pickup snapshotted into the Attic
       Stairwell's room state, and the yellow is spawned once by the Anzu
       puzzle — and the green no longer doubles as the stove puzzle's
       completion flag (FLAG_STOVE_SOLVED does that now), so losing it cannot
       re-open the burner for a second one. */
    player_items &= ~((1 << ITEM_GREEN_KEY_STONE)  |
                      (1 << ITEM_YELLOW_KEY_STONE) |
                      (1 << ITEM_BLUE_KEY_STONE));

    game_flag_set(FLAG_EXIT_DOOR_UNLOCKED);
    attic_exit_unlock_door();          /* swap xt_dr_lckd -> xt_dr_cmplt */
    sound_play(SFX_UNLOCK);
    show_pickup_msg_raw("The door is unlocked");
    exit_puzzle();
}

/* ---- Per-frame update ---------------------------------------------------- */

void exit_door_puzzle_update(void) {
    uint16_t btn = 0;
    if (pad_buff_len[0]) { PadResponse *pad = (PadResponse *)pad_buff[0]; btn = ~pad->btn; }

    if (state == XD_IDLE) {
        int held = (btn & PAD_CIRCLE) ? 1 : 0;
        int just = held && !interact_prev;
        interact_prev = held;
        if (!just) return;

        int32_t dx = cam_x - XD_DOOR_X, dz = cam_z - XD_DOOR_Z;
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (xz >= XD_INTERACT_RADIUS) return;
        /* Once the door is open the same press is the way OUT, not back into a
           finished board. */
        if (solved()) exit_latch = 1;
        else          start_puzzle();
        return;
    }

    /* Camera glides back to the fixed shot; the board opens once it settles. */
    if (state == XD_INTRO) {
        cam_anim_t++;
        int32_t t = cam_anim_t * 256 / XD_CAM_ANIM_FRAMES; if (t > 256) t = 256;
        int32_t inv = 256 - t;
        int32_t e = 256 - (inv * inv / 256);            /* ease-out 0..256 */
        cam_x     = src_x   + ((XD_CAM_X - src_x) * e) / 256;
        cam_y     = src_y   + ((XD_CAM_Y - src_y) * e) / 256;
        cam_z     = src_z   + ((XD_CAM_Z - src_z) * e) / 256;
        cam_rot   = src_rot + (rot_delta * e) / 256;
        cam_pitch = 0;
        cam_vy    = 0;
        if (cam_anim_t >= XD_CAM_ANIM_FRAMES) {
            cam_x = XD_CAM_X; cam_y = XD_CAM_Y; cam_z = XD_CAM_Z;
            cam_rot = XD_CAM_ROT; cam_pitch = 0; cam_vy = 0;
            state = XD_BOARD;
            pz_btn_prev = btn;   /* arm: don't treat the held Circle as a select */
        }
        return;
    }

    uint16_t pressed = btn & ~pz_btn_prev;
    pz_btn_prev = btn;

    /* The board's three open sockets sit in a diamond, so navigation is spelled
       out rather than derived from a grid: the bottom socket is fixed and is
       deliberately unreachable. */
    if (state == XD_BOARD) {
        if (pressed & PAD_UP)    { board_cur = XD_BOX_TOP; }
        if (pressed & PAD_DOWN)  { if (board_cur == XD_BOX_TOP) board_cur = XD_BOX_LEFT; }
        if (pressed & PAD_LEFT)  { if (board_cur == XD_BOX_RIGHT) board_cur = XD_BOX_LEFT; }
        if (pressed & PAD_RIGHT) { if (board_cur == XD_BOX_LEFT)  board_cur = XD_BOX_RIGHT; }
        if (pressed & PAD_CROSS) { exit_puzzle(); return; }
        if (pressed & PAD_CIRCLE) {
            pick_target = board_cur;
            /* Open on the socket's current item, else the first slot. */
            pick_cur = box_item[board_cur] >= 0 ? box_item[board_cur] : 0;
            state    = XD_PICKER;
        }
        return;
    }

    /* XD_PICKER: PICK_COLS-wide grid over every inventory slot. Choosing an item
       copies it into the socket — the player keeps it either way. The trailing
       cells of the last row may be past MENU_ITEM_SLOTS; the guard below keeps
       the cursor out of them and the draw loop never paints them. */
    {
        int row = pick_cur / PICK_COLS, col = pick_cur % PICK_COLS;
        if (pressed & PAD_UP)    { if (row > 0) row--; }
        if (pressed & PAD_DOWN)  { if (row < PICK_ROWS - 1) row++; }
        if (pressed & PAD_LEFT)  { if (col > 0) col--; }
        if (pressed & PAD_RIGHT) { if (col < PICK_COLS - 1) col++; }
        int next = row * PICK_COLS + col;
        if (next < MENU_ITEM_SLOTS) pick_cur = next;

        if (pressed & PAD_CROSS) { state = XD_BOARD; return; }
        if ((pressed & PAD_CIRCLE) && menu_item_held(pick_cur) && !slot_taken(pick_cur)) {
            box_item[pick_target] = pick_cur;
            state = XD_BOARD;
            /* There is no "confirm" control: the lock reads itself the moment
               all three sockets are filled, and stays silent unless the
               combination is right. */
            if (combination_ok()) unlock_door();
        }
    }
}

/* ---- Drawing ------------------------------------------------------------- */

static void xd_rect(RenderContext *ctx, int x, int y, int w, int h,
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

static void xd_outline(RenderContext *ctx, int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    xd_rect(ctx, x,       y,       w, 2, r, g, b, ot_idx);
    xd_rect(ctx, x,       y+h-2,   w, 2, r, g, b, ot_idx);
    xd_rect(ctx, x,       y,       2, h, r, g, b, ot_idx);
    xd_rect(ctx, x+w-2,   y,       2, h, r, g, b, ot_idx);
}

/* Same double outline the stove board and the trick-drawers reticule use. */
static void xd_cursor(RenderContext *ctx, int x, int y, int w, int h) {
    xd_outline(ctx, x - 4, y - 4, w + 8, h + 8, 80, 80, 200, XD_OT_CURSOR);
    xd_outline(ctx, x - 2, y - 2, w + 4, h + 4, 180, 180, 255, XD_OT_CURSOR);
}

/* The magenta stone, drawn from its own resident slot rather than a menu one. */
static void draw_mgn_icon(RenderContext *ctx, int x, int y, int size) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;
    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, 255, 255, 255);   /* dark source art, same lift the menu icons take */
    poly->x0 = x;        poly->y0 = y;
    poly->x1 = x + size; poly->y1 = y;
    poly->x2 = x;        poly->y2 = y + size;
    poly->x3 = x + size; poly->y3 = y + size;
    poly->u0 = mgn_u0; poly->v0 = mgn_v0;
    poly->u1 = mgn_u1; poly->v1 = mgn_v0;
    poly->u2 = mgn_u0; poly->v2 = mgn_v1;
    poly->u3 = mgn_u1; poly->v3 = mgn_v1;
    poly->tpage = mgn_tpage;
    poly->clut  = mgn_clut;
    addPrim(&ctx->buffers[ctx->active_buffer].ot[XD_OT_ICON], poly);
    ctx->next_packet += sizeof(POLY_FT4);
}

static void draw_socket(RenderContext *ctx, int x, int y, int fixed) {
    /* The fixed socket is warmer and outlined brighter so it reads as part of
       the lock rather than an empty slot the player has missed. */
    xd_rect(ctx, x, y, BOX_W, BOX_H,
            fixed ? 45 : 35, fixed ? 25 : 30, fixed ? 45 : 45, XD_OT_PANEL);
    xd_outline(ctx, x, y, BOX_W, BOX_H,
               fixed ? 160 : 80, fixed ? 70 : 70, fixed ? 160 : 100, XD_OT_LINE);
}

void exit_door_puzzle_draw(RenderContext *ctx) {
    if (state == XD_IDLE || state == XD_INTRO) return;

    /* Reset the texture window before any icon: the room sorts a 128x128 window
       at OT_LENGTH-1 which is still active down here and would wrap the icons'
       UVs (see the same note in menu_draw). */
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            RECT full = {0, 0, 0, 0};
            DR_TWIN *tw = (DR_TWIN *)ctx->next_packet;
            setTexWindow(tw, &full);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[XD_OT_TEXWIN], tw);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    /* --- The diamond of sockets, right of the door --- */
    {
        int i;
        for (i = 0; i < XD_BOX_COUNT; i++) {
            draw_socket(ctx, BOX_XY[i].x, BOX_XY[i].y, 0);
            if (box_item[i] >= 0)
                menu_draw_item_icon(ctx, box_item[i],
                                    BOX_XY[i].x + BOX_PAD, BOX_XY[i].y + BOX_PAD,
                                    BOX_ICON, XD_OT_ICON);
        }
        draw_socket(ctx, XD_FIXED_X, XD_FIXED_Y, 1);
        draw_mgn_icon(ctx, XD_FIXED_X + BOX_PAD, XD_FIXED_Y + BOX_PAD, BOX_ICON);
    }

    /* --- Board cursor (hidden while the picker is up) --- */
    if (state == XD_BOARD)
        xd_cursor(ctx, BOX_XY[board_cur].x, BOX_XY[board_cur].y, BOX_W, BOX_H);

    /* --- Item picker --- */
    if (state == XD_PICKER) {
        xd_rect(ctx, PICK_X, PICK_Y, PICK_W, PICK_H, 15, 12, 20, XD_OT_PANEL);
        xd_outline(ctx, PICK_X, PICK_Y, PICK_W, PICK_H, 80, 80, 80, XD_OT_LINE);
        btn_prompt_draw(ctx, PICK_X + 8, PICK_Y + 6, "ITEMS", XD_OT_TEXT);

        int s;
        for (s = 0; s < MENU_ITEM_SLOTS; s++) {
            int cx = PICK_GRID_X + (s % PICK_COLS) * PICK_CELL;
            int cy = PICK_GRID_Y + (s / PICK_COLS) * PICK_CELL;
            /* Items already in another socket get a red cell so it's clear why
               Circle won't take them. */
            int taken = slot_taken(s);
            xd_rect(ctx, cx, cy, PICK_CELL, PICK_CELL,
                    taken ? 60 : 35, taken ? 25 : 30, taken ? 30 : 45, XD_OT_PANEL);
            xd_outline(ctx, cx, cy, PICK_CELL, PICK_CELL,
                       taken ? 140 : 80, taken ? 70 : 70, taken ? 70 : 100, XD_OT_LINE);
            menu_draw_item_icon(ctx, s, cx + PICK_PAD, cy + PICK_PAD,
                                PICK_ICON, XD_OT_ICON);
        }

        {
            int cx = PICK_GRID_X + (pick_cur % PICK_COLS) * PICK_CELL;
            int cy = PICK_GRID_Y + (pick_cur / PICK_COLS) * PICK_CELL;
            xd_cursor(ctx, cx, cy, PICK_CELL, PICK_CELL);
        }

        /* Name of the highlighted item, along the panel's bottom edge. */
        {
            const char *label = "Empty";
            if (slot_taken(pick_cur))          label = "In another socket";
            else if (menu_item_held(pick_cur)) label = menu_item_name(pick_cur);
            btn_prompt_draw(ctx, PICK_X + 8, PICK_NAME_Y, label, XD_OT_TEXT);
        }
    }

    /* --- Controls, same line as the other puzzles --- */
    if (state == XD_BOARD)
        btn_prompt_draw(ctx, 8, 206, BTN_CIRCLE " - Place  " BTN_CROSS " - Exit",
                        XD_OT_TEXT);
    else if (state == XD_PICKER)
        btn_prompt_draw(ctx, 8, 206, BTN_CIRCLE " - Select  " BTN_CROSS " - Back",
                        XD_OT_TEXT);
}

/* The floating sign on the door itself. XY plane (reading along X, fixed Z):
   door_draw_string_3d centres the reading axis on world_x AFTER adding 200, so
   pass door_x - 200. Sits 11 south of the panel so it floats in front of it. */
void exit_door_prompt_draw(RenderContext *ctx) {
    if (state != XD_IDLE) return;

    int32_t dx = cam_x - XD_DOOR_X;
    int32_t dz = cam_z - XD_DOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= XD_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > XD_FADE_NEAR) {
        int range = XD_TEXT_RADIUS - XD_FADE_NEAR;
        int prog  = xz - XD_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx,
                        solved() ? "Press " BTN_CIRCLE " to exit"
                                 : "Press " BTN_CIRCLE " to interact",
                        XD_DOOR_X - 200, XD_TEXT_Y, XD_DOOR_Z - 11,
                        50, 255, 50, fade, 0, TEXT_PLANE_XY, XD_TEXT_PIXEL);
}
