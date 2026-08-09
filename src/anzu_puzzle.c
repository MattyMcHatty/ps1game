#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"          /* game_flag, show_pickup_msg_raw */
#include "sound.h"           /* SFX_UNLOCK, SFX_PICKUP */
#include "btn_glyph.h"       /* BTN_*, btn_prompt_draw */
#include "door.h"            /* door_draw_string_3d for the proximity prompt */
#include "cdaudio.h"         /* the room's music changes on the solve */
#include "item_pickup.h"     /* the Yellow Key Stone appears by the piano */
#include "piano_props.h"     /* retire the Tablets prop */
#include "anzu_tex.h"
#include "anzu_puzzle.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- The frame in world space ---------------------------------------------
   Read straight off assets/Piano_Room.smx. The frame is built into the back
   (west) wall: its outer border stands at x[-2301,-2261] and the six central
   panels are recessed to x=-2288, laid out 3 across by 2 down.

   Rows are listed TOP first — the engine's Y axis points DOWN, so the top row
   is the more negative pair. Columns are listed LEFT first as the player sees
   them: the puzzle camera looks due -X, and at that heading screen-right is +Z
   (the view matrix maps camera-space x to +wz), so ascending Z reads
   left-to-right. Get that backwards and the solution mirrors. */
#define AZ_PANEL_X   (-2288)   /* the recessed panel plane                  */
#define AZ_TILE_X    (-2285)   /* placed tiles sit just proud of it         */

static const int16_t ROW_Y0[2] = { -401, -241 };   /* top edge of each row    */
static const int16_t ROW_Y1[2] = { -241,  -82 };   /* bottom edge             */
static const int16_t COL_Z0[3] = { -216,  -66,  83 };  /* left edge of each col  */
static const int16_t COL_Z1[3] = {  -66,   83, 233 };  /* right edge             */

/* ---- The fixed shot --------------------------------------------------------
   Dead-on the frame, looking due -X with no pitch: camera Y/Z sit on the
   frame's centre (y -240, z 7) so the frame lands centred on the screen.

   The distance is chosen from the projection, not by eye. With
   gte_SetGeomScreen(256) and a 320-wide screen, a span S at distance D covers
   S*256/D pixels. The frame's outer border is 520 wide; putting it in the
   middle 188 px leaves 66 px of clear screen on each side, which is where the
   three boxes per side live. 520*256/188 = 708, so the camera stands 708 in
   front of the panel plane. Move AZ_CAM_X and the box columns have to move with
   it or they will overlap the frame. */
#define AZ_CAM_X     (-1580)   /* AZ_PANEL_X + 708 */
#define AZ_CAM_Y      (-240)
#define AZ_CAM_Z          7
#define AZ_CAM_ROT     3072    /* face -X, straight at the frame */

#define AZ_PROJ_H       256    /* matches gte_SetGeomScreen */
#define AZ_TILE_DIST   (AZ_CAM_X - AZ_TILE_X)   /* 705: tile plane to camera */

#define AZ_CAM_ANIM_FRAMES  24
#define AZ_WIN_HOLD_FRAMES  96   /* beat on the finished relief before ejecting */

/* Proximity prompt / trigger, measured to the middle of the frame. */
#define AZ_INTERACT_X          (-2270)
#define AZ_INTERACT_Z                7
#define AZ_INTERACT_RADIUS         600
#define AZ_INTERACT_TEXT_RADIUS   1100
#define AZ_INTERACT_FADE_NEAR      800
#define AZ_TEXT_Y              (-252)   /* 7 glyph rows tall, so ~centred on -240 */
#define AZ_TEXT_PIXEL                3

/* ---- Slots ----------------------------------------------------------------
   0..5   the side boxes: 0,1,2 down the left, 3,4,5 down the right
   6..11  the frame panels, row-major: 6,7,8 across the top, 9,10,11 the bottom
   The solution is panel 6+i holding tile i, upright. */
#define AZ_BOX_SLOTS    6
#define AZ_SLOT_COUNT  12
#define AZ_PANEL0       AZ_BOX_SLOTS

/* The reticule grid. Column 0 and 4 are the box stacks; columns 1..3 rows 0..1
   are the frame. Row 2 of the frame columns is the one hole, which the movement
   rules below step over. */
#define AZ_GRID_ROWS 3
#define AZ_GRID_COLS 5
static const int8_t GRID[AZ_GRID_ROWS][AZ_GRID_COLS] = {
    { 0,  6,  7,  8,  3 },
    { 1,  9, 10, 11,  4 },
    { 2, -1, -1, -1,  5 },
};

/* ---- Board layout (320x240) -----------------------------------------------
   Three boxes down each side, clear of the frame: the frame's panels project to
   screen x 80..242, so the left column ends at 60 and the right starts at 260.
   The bottom row stops at y=200, above the control prompt at 206/216. */
#define BOX_W        52
#define BOX_H        52
#define BOX_L_X       8
#define BOX_R_X     260
#define BOX_ROW_Y0   24
#define BOX_ROW_DY   62
#define BOX_PAD       4   /* inset of the tile art within its box */

/* OT layers — inside the menu-reserved range (< SCENE_OT_MIN) so the board
   sits on top of the room. Higher index = drawn first = further back. */
#define AZ_OT_PANEL  12
#define AZ_OT_LINE   10
#define AZ_OT_TILE    8
#define AZ_OT_CURSOR  3
#define AZ_OT_TEXT    1

typedef enum {
    AZ_IDLE = 0,
    AZ_INTRO,    /* camera gliding to the fixed shot     */
    AZ_BOARD,    /* live: reticule + pick up / drop      */
    AZ_WIN       /* solved: holding on the finished relief */
} AzState;

static AzState state = AZ_IDLE;

/* Contents. -1 = empty. Tiles are 0..5 == anzu1..anzu6; rot is 0..3 quarter
   turns clockwise. slot_tile/slot_rot cover boxes AND panels, so a tile can be
   picked back out of the frame — without that a single misplacement would make
   the puzzle unwinnable without leaving and starting over. */
static int8_t slot_tile[AZ_SLOT_COUNT];
static int8_t slot_rot[AZ_SLOT_COUNT];

static int8_t held_tile = -1;   /* the tile riding the reticule, or -1 */
static int8_t held_rot  =  0;
static int    ret_row = 0, ret_col = 0;
static int    win_timer = 0;

static int32_t save_cx, save_cy, save_cz, save_crot, save_cvy;
static int32_t cam_anim_t = 0;
static int32_t src_x, src_y, src_z, src_rot, src_pitch, rot_delta;

static uint16_t pz_btn_prev  = 0xFFFF;  /* board input edge-detect (btn = ~pad) */
static int      interact_prev = 1;      /* interact-Circle edge-detect          */

/* ---- Shuffle ---------------------------------------------------------------
   No global PRNG exists in this project, so this is a self-contained LCG. It is
   stirred every frame the player spends in the room before opening the puzzle,
   which is what makes the layout differ from entry to entry — a fixed seed
   would deal the same hand every time. */
static uint32_t rng = 0x9E3779B9u;
static uint32_t az_rand(void) {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 16;
}

int anzu_puzzle_active(void) { return state != AZ_IDLE; }

static int solved(void) { return game_flag(FLAG_ANZU_SOLVED); }

/* Deal the six tiles into the six boxes in a random order, each turned a random
   QUARTER turn — never 0, so every tile always has to be rotated at least once
   and the player can never win by luck alone. The frame starts empty. */
static void shuffle(void) {
    int i;
    int8_t bag[AZ_BOX_SLOTS];
    for (i = 0; i < AZ_BOX_SLOTS; i++) bag[i] = (int8_t)i;
    for (i = AZ_BOX_SLOTS - 1; i > 0; i--) {
        int j = (int)(az_rand() % (uint32_t)(i + 1));
        int8_t t = bag[i]; bag[i] = bag[j]; bag[j] = t;
    }
    for (i = 0; i < AZ_BOX_SLOTS; i++) {
        slot_tile[i] = bag[i];
        slot_rot[i]  = (int8_t)(1 + az_rand() % 3);   /* 90, 180 or 270 */
    }
    for (i = AZ_PANEL0; i < AZ_SLOT_COUNT; i++) {
        slot_tile[i] = -1;
        slot_rot[i]  = 0;
    }
    held_tile = -1;
    held_rot  = 0;
}

/* The finished relief, as it must look on every visit after the solve. */
static void install_solution(void) {
    int i;
    for (i = 0; i < AZ_BOX_SLOTS; i++) { slot_tile[i] = -1; slot_rot[i] = 0; }
    for (i = 0; i < AZ_BOX_SLOTS; i++) {
        slot_tile[AZ_PANEL0 + i] = (int8_t)i;
        slot_rot[AZ_PANEL0 + i]  = 0;
    }
    held_tile = -1;
}

void anzu_puzzle_place(void) {
    state     = AZ_IDLE;
    ret_row   = 0;
    ret_col   = 0;
    win_timer = 0;

    if (solved()) install_solution();
    else          shuffle();

    /* Don't treat a Circle held through the door transition as an interact. */
    int held = interact_tapped();
    interact_prev = held;
}

/* ---- State transitions --------------------------------------------------- */

static void start_puzzle(void) {
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    /* The player stays at the frame while the camera flies over. */
    camera_anchor_player(save_cx, save_cy, save_cz);

    src_x = cam_x; src_y = cam_y; src_z = cam_z;
    src_rot = cam_rot; src_pitch = cam_pitch;
    {
        int32_t d = ((AZ_CAM_ROT - src_rot) % 4096 + 4096) % 4096;
        if (d > 2048) d -= 4096;
        rot_delta = d;
    }

    shuffle();          /* a fresh deal on EVERY entry, as specified */
    cam_anim_t = 0;
    ret_row = 0; ret_col = 0;
    state = AZ_INTRO;
}

/* Drop the fixed camera and put the player back where they stood. `reset`
   throws the tiles back into the boxes — every exit but the winning one. */
static void exit_puzzle(int reset) {
    state     = AZ_IDLE;
    cam_x     = save_cx; cam_y = save_cy; cam_z = save_cz;
    cam_rot   = save_crot; cam_vy = save_cvy;
    cam_pitch = 0;
    camera_release_player();
    if (reset) shuffle();
    interact_prev = 1;   /* swallow the held Circle so we don't re-open instantly */
}

static int is_solution(void) {
    int i;
    for (i = 0; i < AZ_BOX_SLOTS; i++) {
        if (slot_tile[AZ_PANEL0 + i] != (int8_t)i) return 0;
        if (slot_rot[AZ_PANEL0 + i]  != 0)         return 0;
    }
    return 1;
}

/* The payoff. Everything here is permanent — the flag is what makes it survive
   a save/load, and piano_props_place reads it on every later entry. */
static void win(void) {
    game_flag_set(FLAG_ANZU_SOLVED);
    piano_props_tablets_hide();
    /* The reward, on the floor just south of (in front of) the piano. The room
       keeps its own pickup array, so this persists through world_leave. */
    item_pickup_spawn(-420, -50, 640, PICKUP_YELLOW_KEY_STONE);
    cdaudio_play(CDAUDIO_ANZU_TRACK, 1);   /* the room's music, from now on */
    sound_play(SFX_UNLOCK);
    show_pickup_msg_raw("The carving is whole. Something clicks");
    state     = AZ_WIN;
    win_timer = AZ_WIN_HOLD_FRAMES;
}

/* Circle: pick a tile up off the reticule's slot, or drop the held one into it.
   Dropping is only allowed into an EMPTY slot (either a box or a panel), which
   keeps the rule simple and means a tile can always be parked somewhere. */
static void press_select(void) {
    int slot = GRID[ret_row][ret_col];
    if (slot < 0) return;

    if (held_tile < 0) {
        if (slot_tile[slot] < 0) return;
        held_tile = slot_tile[slot];
        held_rot  = slot_rot[slot];
        slot_tile[slot] = -1;
        slot_rot[slot]  = 0;
        return;
    }

    if (slot_tile[slot] >= 0) return;   /* occupied: nothing to do */
    slot_tile[slot] = held_tile;
    slot_rot[slot]  = held_rot;
    held_tile = -1;
    held_rot  = 0;
    if (is_solution()) win();
}

/* ---- Reticule movement -----------------------------------------------------
   Vertical steps over the hole in its own column. Horizontal jumps to the next
   column that has ANY slot and then lands on the row nearest the one we left,
   so stepping right from the bottom-left box arrives at the frame's bottom row
   rather than skipping the frame entirely. */
static int col_row_valid(int c, int r) {
    return r >= 0 && r < AZ_GRID_ROWS && GRID[r][c] >= 0;
}

static void move_vert(int d) {
    int r = ret_row + d;
    while (r >= 0 && r < AZ_GRID_ROWS && GRID[r][ret_col] < 0) r += d;
    if (r >= 0 && r < AZ_GRID_ROWS) ret_row = r;
}

static void move_horz(int d) {
    int c = ret_col + d;
    while (c >= 0 && c < AZ_GRID_COLS) {
        int r;
        for (r = 0; r < AZ_GRID_ROWS; r++) if (GRID[r][c] >= 0) break;
        if (r < AZ_GRID_ROWS) break;   /* this column has something in it */
        c += d;
    }
    if (c < 0 || c >= AZ_GRID_COLS) return;

    ret_col = c;
    if (col_row_valid(c, ret_row)) return;
    /* Nearest valid row to the one we came from. */
    int off;
    for (off = 1; off < AZ_GRID_ROWS; off++) {
        if (col_row_valid(c, ret_row - off)) { ret_row -= off; return; }
        if (col_row_valid(c, ret_row + off)) { ret_row += off; return; }
    }
}

/* ---- Per-frame update ---------------------------------------------------- */

void anzu_puzzle_update(void) {
    uint16_t btn = 0;
    if (pad_buff_len[0]) { PadResponse *pad = (PadResponse *)pad_buff[0]; btn = ~pad->btn; }

    if (state == AZ_IDLE) {
        rng += 0x6D2B79F5u;   /* stir while the player walks around the room */
        if (solved()) return; /* retired: the relief is finished */
        int held = interact_tapped();
        int just = held && !interact_prev;
        interact_prev = held;
        int32_t dx = cam_x - AZ_INTERACT_X, dz = cam_z - AZ_INTERACT_Z;
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (just && xz < AZ_INTERACT_RADIUS &&
            interact_facing(AZ_INTERACT_X, AZ_INTERACT_Z)) start_puzzle();
        return;
    }

    if (state == AZ_INTRO) {
        cam_anim_t++;
        int32_t t = cam_anim_t * 256 / AZ_CAM_ANIM_FRAMES; if (t > 256) t = 256;
        int32_t inv = 256 - t;
        int32_t e = 256 - (inv * inv / 256);            /* ease-out 0..256 */
        cam_x     = src_x     + ((AZ_CAM_X - src_x) * e) / 256;
        cam_y     = src_y     + ((AZ_CAM_Y - src_y) * e) / 256;
        cam_z     = src_z     + ((AZ_CAM_Z - src_z) * e) / 256;
        cam_rot   = src_rot   + (rot_delta * e) / 256;
        cam_pitch = src_pitch - (src_pitch * e) / 256;
        cam_vy    = 0;
        if (cam_anim_t >= AZ_CAM_ANIM_FRAMES) {
            cam_x = AZ_CAM_X; cam_y = AZ_CAM_Y; cam_z = AZ_CAM_Z;
            cam_rot = AZ_CAM_ROT; cam_pitch = 0; cam_vy = 0;
            state = AZ_BOARD;
            pz_btn_prev = btn;   /* arm: don't treat the held Circle as a select */
        }
        return;
    }

    if (state == AZ_WIN) {
        /* No input: hold on the finished carving, then eject. The tiles stay
           where they are, so `reset` is 0. */
        if (--win_timer <= 0) exit_puzzle(0);
        return;
    }

    uint16_t pressed = btn & ~pz_btn_prev;
    pz_btn_prev = btn;

    if (pressed & PAD_UP)    move_vert(-1);
    if (pressed & PAD_DOWN)  move_vert( 1);
    if (pressed & PAD_LEFT)  move_horz(-1);
    if (pressed & PAD_RIGHT) move_horz( 1);

    /* Square turns the held tile a quarter turn clockwise. */
    if ((pressed & PAD_SQUARE) && held_tile >= 0) held_rot = (int8_t)((held_rot + 1) & 3);

    if (pressed & PAD_CROSS)  { exit_puzzle(1); return; }
    if (pressed & PAD_CIRCLE) press_select();
}

/* ---- Drawing ------------------------------------------------------------- */

/* The four corners of a tile's art within its VRAM rect, in the order
   top-left, top-right, bottom-left, bottom-right. */
static const uint8_t CORNER_DU[4] = { 0, ANZU_TILE_PX - 1, 0,              ANZU_TILE_PX - 1 };
static const uint8_t CORNER_DV[4] = { 0, 0,                ANZU_TILE_PX - 1, ANZU_TILE_PX - 1 };

/* Which tile corner lands on each poly corner, per quarter turn CLOCKWISE.
   Row r is rotation r*90deg; the four entries are the poly's TL,TR,BL,BR.
   At 90deg CW the tile's bottom-left corner is the one that ends up top-left,
   hence {2,0,3,1}. */
static const uint8_t ROT_CORNER[4][4] = {
    { 0, 1, 2, 3 },   /*   0 deg */
    { 2, 0, 3, 1 },   /*  90 deg */
    { 3, 2, 1, 0 },   /* 180 deg */
    { 1, 3, 0, 2 },   /* 270 deg */
};

static void tile_uv(const AnzuTex *a, int rot, int corner, uint8_t *u, uint8_t *v) {
    int c = ROT_CORNER[rot & 3][corner];
    *u = (uint8_t)(a->u0 + CORNER_DU[c]);
    *v = (uint8_t)(a->v0 + CORNER_DV[c]);
}

/* Filled 2D rect at an OT index (same helper every board in this game uses). */
static void az_rect(RenderContext *ctx, int x, int y, int w, int h,
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

static void az_outline(RenderContext *ctx, int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b, int ot_idx) {
    az_rect(ctx, x,       y,       w, 2, r, g, b, ot_idx);
    az_rect(ctx, x,       y+h-2,   w, 2, r, g, b, ot_idx);
    az_rect(ctx, x,       y,       2, h, r, g, b, ot_idx);
    az_rect(ctx, x+w-2,   y,       2, h, r, g, b, ot_idx);
}

static void az_cursor(RenderContext *ctx, int x, int y, int w, int h) {
    az_outline(ctx, x - 4, y - 4, w + 8, h + 8, 80, 80, 200, AZ_OT_CURSOR);
    az_outline(ctx, x - 2, y - 2, w + 4, h + 4, 180, 180, 255, AZ_OT_CURSOR);
}

/* A tile drawn flat on the screen, rotated. No texture-window reset is needed:
   every anzu tile lives at u 64..127 / v 0..127 of its page, which is inside
   the 128x128 window the piano room sorts at OT_LENGTH-1. */
static void az_tile_2d(RenderContext *ctx, int tile, int rot,
                       int x, int y, int w, int h, int ot_idx) {
    if (tile < 0) return;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;
    const AnzuTex *a = anzu_tex(tile);

    POLY_FT4 *p = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(p);
    setRGB0(p, 128, 128, 128);
    p->tpage = a->tpage;
    p->clut  = a->clut;
    tile_uv(a, rot, 0, &p->u0, &p->v0);
    tile_uv(a, rot, 1, &p->u1, &p->v1);
    tile_uv(a, rot, 2, &p->u2, &p->v2);
    tile_uv(a, rot, 3, &p->u3, &p->v3);
    p->x0 = (int16_t)x;       p->y0 = (int16_t)y;
    p->x1 = (int16_t)(x + w); p->y1 = (int16_t)y;
    p->x2 = (int16_t)x;       p->y2 = (int16_t)(y + h);
    p->x3 = (int16_t)(x + w); p->y3 = (int16_t)(y + h);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[ot_idx], p);
    ctx->next_packet += sizeof(POLY_FT4);
}

/* Screen rect of a box slot (0..5). */
static void box_rect(int slot, int *x, int *y) {
    *x = (slot < 3) ? BOX_L_X : BOX_R_X;
    *y = BOX_ROW_Y0 + (slot % 3) * BOX_ROW_DY;
}

/* Screen rect of a frame panel (0..5), from the FIXED camera. The projection is
   deterministic here — the camera cannot move while the board is up — so this
   is plain arithmetic rather than a GTE round trip. */
static void panel_rect(int panel, int *x0, int *y0, int *x1, int *y1) {
    int r = panel / 3, c = panel % 3;
    *x0 = 160 + ((COL_Z0[c] - AZ_CAM_Z) * AZ_PROJ_H) / AZ_TILE_DIST;
    *x1 = 160 + ((COL_Z1[c] - AZ_CAM_Z) * AZ_PROJ_H) / AZ_TILE_DIST;
    *y0 = 120 + ((ROW_Y0[r] - AZ_CAM_Y) * AZ_PROJ_H) / AZ_TILE_DIST;
    *y1 = 120 + ((ROW_Y1[r] - AZ_CAM_Y) * AZ_PROJ_H) / AZ_TILE_DIST;
}

/* ---- The tiles sitting in the frame (3D) -----------------------------------
   Real geometry, not an overlay on the board: once solved they have to stay
   visible from anywhere in the room. Drawn as quads 3 units proud of the panel
   plane and sorted one OT bucket in front of the room mesh (which adds 40), so
   they can never lose the painter's sort to the panel behind them.

   Backface culling is skipped: these six quads only exist inside a wall recess
   and are only ever seen from the room side. */
static void draw_frame_tiles(RenderContext *ctx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int i;
    for (i = 0; i < AZ_BOX_SLOTS; i++) {
        int tile = slot_tile[AZ_PANEL0 + i];
        if (tile < 0) continue;
        int r = i / 3, c = i % 3;

        SVECTOR v[4];
        v[0].vx = AZ_TILE_X; v[0].vy = ROW_Y0[r]; v[0].vz = COL_Z0[c]; v[0].pad = 0;
        v[1].vx = AZ_TILE_X; v[1].vy = ROW_Y0[r]; v[1].vz = COL_Z1[c]; v[1].pad = 0;
        v[2].vx = AZ_TILE_X; v[2].vy = ROW_Y1[r]; v[2].vz = COL_Z0[c]; v[2].pad = 0;
        v[3].vx = AZ_TILE_X; v[3].vy = ROW_Y1[r]; v[3].vz = COL_Z1[c]; v[3].pad = 0;

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
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) continue;

        int k, off = 0;
        for (k = 0; k < 4; k++)
            if (sv[k].vx <= -1023 || sv[k].vx >= 1023 ||
                sv[k].vy <= -1023 || sv[k].vy >= 1023) { off = 1; break; }
        if (off) continue;

        gte_avsz4();
        gte_stotz(&otz);
        if (otz <= 0) continue;
        otz += 39;                       /* the room mesh uses +40 */
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        /* Same fog curve as the room, so a placed tile sits in the same light
           as the frame around it. */
        int32_t dx = AZ_PANEL_X - cam_x;
        int32_t dz = ((int32_t)COL_Z0[c] + COL_Z1[c]) / 2 - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog_start = 350, fog_end = 1500;
        int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
        int32_t ff = ((fog_end - fog) << 8) / (fog_end - fog_start);
        uint8_t cr = (uint8_t)((128 * ff + 20 * (256 - ff)) >> 8);
        uint8_t cg = (uint8_t)((128 * ff + 15 * (256 - ff)) >> 8);
        uint8_t cb = (uint8_t)((128 * ff + 10 * (256 - ff)) >> 8);

        if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;
        const AnzuTex *a = anzu_tex(tile);
        POLY_FT4 *p = (POLY_FT4 *)ctx->next_packet;
        setPolyFT4(p);
        setRGB0(p, cr, cg, cb);
        p->tpage = a->tpage;
        p->clut  = a->clut;
        tile_uv(a, slot_rot[AZ_PANEL0 + i], 0, &p->u0, &p->v0);
        tile_uv(a, slot_rot[AZ_PANEL0 + i], 1, &p->u1, &p->v1);
        tile_uv(a, slot_rot[AZ_PANEL0 + i], 2, &p->u2, &p->v2);
        tile_uv(a, slot_rot[AZ_PANEL0 + i], 3, &p->u3, &p->v3);
        p->x0 = sv[0].vx; p->y0 = sv[0].vy;
        p->x1 = sv[1].vx; p->y1 = sv[1].vy;
        p->x2 = sv[2].vx; p->y2 = sv[2].vy;
        p->x3 = sv[3].vx; p->y3 = sv[3].vy;
        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], p);
        ctx->next_packet += sizeof(POLY_FT4);
    }
}

void anzu_puzzle_draw(RenderContext *ctx) {
    /* Tiles already in the frame, in world space. Valid in every state — during
       the puzzle they show what has been placed, afterwards they are simply
       part of the room. The caller has the view matrix loaded. */
    draw_frame_tiles(ctx);

    if (state == AZ_IDLE) {
        if (solved()) return;
        int32_t dx = cam_x - AZ_INTERACT_X, dz = cam_z - AZ_INTERACT_Z;
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (xz >= AZ_INTERACT_TEXT_RADIUS) return;

        int fade = 256;
        if (xz > AZ_INTERACT_FADE_NEAR) {
            int range = AZ_INTERACT_TEXT_RADIUS - AZ_INTERACT_FADE_NEAR;
            int prog  = xz - AZ_INTERACT_FADE_NEAR;
            if (prog > range) prog = range;
            fade = 256 - ((prog * 256) / range);
        }
        /* Centred in the frame, in the YZ plane (reading along Z, fixed X). The
           player is on the +X side, so no mirror; door_draw_string_3d centres on
           the reading coord + 200, hence the -200. */
        door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to interact",
                            AZ_INTERACT_X, AZ_TEXT_Y, AZ_INTERACT_Z - 200,
                            50, 255, 50, fade, 0, TEXT_PLANE_YZ, AZ_TEXT_PIXEL);
        return;
    }

    if (state != AZ_BOARD) return;   /* the intro glide and the win beat play clean */

    /* --- The six boxes --- */
    int s;
    for (s = 0; s < AZ_BOX_SLOTS; s++) {
        int bx, by;
        box_rect(s, &bx, &by);
        az_rect(ctx, bx, by, BOX_W, BOX_H, 35, 30, 45, AZ_OT_PANEL);
        az_outline(ctx, bx, by, BOX_W, BOX_H, 80, 70, 100, AZ_OT_LINE);
        az_tile_2d(ctx, slot_tile[s], slot_rot[s],
                   bx + BOX_PAD, by + BOX_PAD,
                   BOX_W - 2 * BOX_PAD, BOX_H - 2 * BOX_PAD, AZ_OT_TILE);
    }

    /* --- Reticule, plus the tile it is carrying --- */
    {
        int slot = GRID[ret_row][ret_col];
        int x, y, w, h;
        if (slot >= AZ_PANEL0) {
            int x0, y0, x1, y1;
            panel_rect(slot - AZ_PANEL0, &x0, &y0, &x1, &y1);
            x = x0; y = y0; w = x1 - x0; h = y1 - y0;
        } else if (slot >= 0) {
            box_rect(slot, &x, &y);
            w = BOX_W; h = BOX_H;
        } else {
            x = y = w = h = 0;
        }
        if (w > 0) {
            az_cursor(ctx, x, y, w, h);
            /* The held tile rides the reticule so the player can see what they
               are carrying and how far they have turned it. */
            if (held_tile >= 0)
                az_tile_2d(ctx, held_tile, held_rot,
                           x + BOX_PAD, y + BOX_PAD,
                           w - 2 * BOX_PAD, h - 2 * BOX_PAD, AZ_OT_CURSOR + 1);
        }
    }

    /* Two lines, not three-on-one: the log box starts at x=183 and all three
       prompts on a single row would run under it. Two per line keeps the text
       inside the free strip to the left of the log. */
    btn_prompt_draw(ctx, 8, 206,
                    held_tile >= 0
                        ? BTN_CIRCLE " - Place  " BTN_SQUARE " - Turn"
                        : BTN_CIRCLE " - Take   " BTN_SQUARE " - Turn",
                    AZ_OT_TEXT);
    btn_prompt_draw(ctx, 8, 216, BTN_CROSS " - Exit", AZ_OT_TEXT);
}
