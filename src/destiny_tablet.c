#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "collision.h"      /* GROUND_FLOOR_Y */
#include "texmgr.h"
#include "title.h"          /* current_area gate */
#include "player.h"         /* game flags, show_pickup_msg_raw */
#include "sound.h"
#include "item_pickup.h"    /* the Fake Tablet of Destinies */
#include "door.h"           /* the sign, and the board's screen font */
#include "btn_glyph.h"
#include "underworld.h"     /* the chapter's fog */
#include "underworld_sky.h" /* UW_SKY_OT         */
#include "room_arena.h"     /* the mesh rides in the room's arena */
#include "destiny_tablet.h"

/* The Destiny Tablet - see destiny_tablet.h. The Lightning Tablet's module
   (src/lightning_tablet.c: the in-place mesh measured into a box, the sign,
   the shot, the shards), with a word board in screen space for its puzzle. */

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- The mesh ------------------------------------------------------------
   60 prims as exported; the cap bounds what the draw will walk. */
#define DT_MAX_PRIMS   96

/* >>> THE MESH IS ROOM-SCOPED, IN THE ROOM ARENA'S TAIL, NOT ON THE HEAP. <<<
   The Lightning Tablet's scheme: read on every entry to Destinys Cabin into
   the space behind the room's own file (room_arena_load_extra), gone the
   moment the next room loads over it. dt_gen is the arena generation it was
   read in: dt_loaded() trusts the pointer only while that is still the
   arena's. */
static SMD  *dt_smd = NULL;
static void *dt_buf = NULL;
static int   dt_gen = -1;

/* The box, measured off the mesh at load. WORLD space. -Y is up, so dt_min_y
   is the top of the slab and dt_max_y its base. */
static int32_t dt_min_x = 0, dt_max_x = 0;   /*  2100 .. 3300 as authored */
static int32_t dt_min_z = 0, dt_max_z = 0;   /*  4750 .. 4850             */
static int32_t dt_min_y = 0, dt_max_y = 0;   /*  -700 ..    0             */
static int32_t dt_cx = 0, dt_cz = 0;

static int dt_tex = -1;   /* \TEXUNDW\DSTTBLT.TIM */

#define DT_PLAYER_HEAD 30

/* ---- THE FOG TARGET: the Lightning Tablet's reasoning, on this art's own
   95th-percentile texel, 179 (median 129). */
#define DT_TEX_BRIGHT 179
#define DT_FOG_R (UW_FOG_R * 128 / DT_TEX_BRIGHT)
#define DT_FOG_G (UW_FOG_G * 128 / DT_TEX_BRIGHT)
#define DT_FOG_B (UW_FOG_B * 128 / DT_TEX_BRIGHT)

/* ---- The sign and the reach: the Lightning Tablet's numbers (a slab of the
   same size, held the same 195 off it). */
#define DT_REACH          450
#define DT_TEXT_RADIUS   1500
#define DT_FADE_NEAR     1000
#define DT_SIGN_UP        200
#define DT_SIGN_OUT       100
#define DT_YAW              0

/* ---- The shot: the Lightning Tablet's - straight out from the middle of
   the face at 1050, level with its middle. The camera stands at z=3700,
   inside the cabin (its inside runs z[2000,5000]). */
#define DT_SHOT_DIST      1050
#define DT_INTRO_FRAMES     30

/* ---- The words ------------------------------------------------------------
   0..7 down the left side, 8..15 down the right, top to bottom, as the user
   listed them. */
#define DT_WORDS        16
#define DT_SIDE          8
#define DT_BLANKS        4
static const char *const DT_WORD[DT_WORDS] = {
    "ADAM", "FATE", "LAMASHTU", "SHARUR", "NINURTA", "FIRE", "SATURN", "EARTH",
    "LIGHTNING", "WIND", "JUPITER", "ENLIL", "NEPTUNE", "WATER", "APPLE", "ANZU",
};
/* The answer, blank by blank: ENLIL, FATE, ANZU, NINURTA. */
static const uint8_t DT_ANSWER[DT_BLANKS] = { 11, 1, 15, 4 };

/* The four lines: the text left of each blank (right-aligned against it) and
   the text after it. */
static const char *const DT_LINE_L[DT_BLANKS] = { "",   "MASTER OF", "",               "RETURNED BY" };
static const char *const DT_LINE_R[DT_BLANKS] = { "IS", "",          "DEFIED & STOLE", ""            };

/* ---- The scene, in frames from the solve -----------------------------------
     0  .. 30    the side columns fade; the last word settles in the eye
     30 .. 330   the four words glow, one after another, 75 frames apart, each
                 rising over DT_SC_GLOW_UP and staying lit
     330 .. 390  all four lit, held
     390 .. 420  the lines fade off the stone
     420         the slab shatters and leaves the Fake Tablet of Destinies
     490         control returns */
#define DT_SC_SIDES_OUT     30
#define DT_SC_GLOW_FROM     30
#define DT_SC_GLOW_STEP     75
#define DT_SC_GLOW_UP       60
#define DT_SC_FADE_FROM    390
#define DT_SC_SHATTER      420
#define DT_SC_END          490

/* ---- The item: the slab's centre, 100 over the floor (the Lightning
   Tablet's). Destinys Cabin seeds no pickups (world.c), so a run-time spawn
   may take any slot. */
#define DT_ITEM_UP         100
#define DT_SEEDED_PICKUPS    0

/* ---- The shards: the Lightning Tablet's cubes, in this art's greys (16
   quantiles of its stone, luminance). */
#define DT_SHARDS           10
#define DT_SHARD_SHRINK     20

static const uint8_t DT_GREYS[16] = {
     57,  67,  91, 100, 100, 110, 118, 129,
    129, 140, 140, 149, 156, 167, 179, 194
};
static const uint16_t DT_FACE_SHADE[6] = { 150, 200, 256, 110, 175, 225 };

typedef struct {
    int32_t x, y, z, vx, vy, vz;
    int16_t rx, ry, rz, sx, sy, sz;
    int16_t half, life, max_life;
    uint8_t grey;
} DtShard;

static DtShard dt_shards[DT_SHARDS];
static int     dt_shard_count = 0;

/* ---- State ------------------------------------------------------------------ */
typedef enum { DT_IDLE = 0, DT_INTRO, DT_BOARD, DT_SCENE } DtState;

static DtState  dt_state = DT_IDLE;
static int32_t  dt_t     = 0;
static int      dt_col   = 0;              /* the cursor: 0 left, 1 right      */
static int      dt_row   = 0;              /*             0..7, top first      */
static uint8_t  dt_placed[DT_BLANKS];      /* word index in each blank         */
static int      dt_n     = 0;              /* blanks filled, top first         */
static uint16_t dt_used  = 0;              /* bit w = word w is in a blank     */
static int      dt_circle_prev = 1;
static uint16_t dt_btn_prev    = 0xFFFF;
static int32_t  dt_clock = 0;              /* the next blank's pulse           */

static int32_t dt_save_x, dt_save_y, dt_save_z, dt_save_rot, dt_save_pitch, dt_save_vy;
static int32_t dt_src_x, dt_src_y, dt_src_z, dt_src_rot, dt_src_pitch, dt_rot_delta;
static int32_t dt_shot_x, dt_shot_y, dt_shot_z;

static uint32_t dt_rng = 0x5D3A71u;
static int32_t dt_rand(int32_t n) {      /* 0..n-1 */
    dt_rng = dt_rng * 1103515245u + 12345u;
    return n > 0 ? (int32_t)((dt_rng >> 16) % (uint32_t)n) : 0;
}

static int32_t dt_abs(int32_t v) { return v < 0 ? -v : v; }
static int32_t dt_clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The view, loaded without the SDK's gte_SetRotMatrix/TransMatrix, which
   clobber $t0/$t1 undeclared (lightning_tablet.c's lt_set_view, verbatim). */
static inline void dt_set_view(const MATRIX *m) {
    uint32_t r0 = (uint16_t)m->m[0][0] | ((uint32_t)(uint16_t)m->m[0][1] << 16);
    uint32_t r1 = (uint16_t)m->m[0][2] | ((uint32_t)(uint16_t)m->m[1][0] << 16);
    uint32_t r2 = (uint16_t)m->m[1][1] | ((uint32_t)(uint16_t)m->m[1][2] << 16);
    uint32_t r3 = (uint16_t)m->m[2][0] | ((uint32_t)(uint16_t)m->m[2][1] << 16);
    uint32_t r4 = (uint16_t)m->m[2][2];
    __asm__ volatile (
        "ctc2 %0, $0;"
        "ctc2 %1, $1;"
        "ctc2 %2, $2;"
        "ctc2 %3, $3;"
        "ctc2 %4, $4"
        : : "r"(r0), "r"(r1), "r"(r2), "r"(r3), "r"(r4));
    __asm__ volatile (
        "ctc2 %0, $5;"
        "ctc2 %1, $6;"
        "ctc2 %2, $7"
        : : "r"(m->t[0]), "r"(m->t[1]), "r"(m->t[2]));
}

/* The mesh was read for this visit and is still in the arena. NO AREA TEST
   (lightning_tablet.c's lt_loaded: the room's init runs inside STATE_LOADING
   before current_area is the room). */
static int dt_loaded(void) {
    return dt_smd && dt_gen == room_arena_generation();
}

/* ...and the player is in the room: for the per-frame paths. */
static int dt_live(void) {
    return dt_loaded() && current_area == STATE_DESTINYS_CABIN;
}

static int dt_standing(void) {
    return dt_live() && !game_flag(FLAG_DESTINY_TABLET_BROKEN);
}

/* ---- Load ---------------------------------------------------------------------- */

void destiny_tablet_load_assets(void) {
    /* BANK: Chapter 4 (py tools/check_tex_banks.py walks the uploader call
       graph: destinys_cabin_upload_textures reaches this module). */
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    dt_tex = texmgr_register("\\TEXUNDW\\DSTTBLT.TIM;1");
}

/* ROOM ENTRY, from destinys_cabin_load_geometry() after the room's own
   room_data_bind(): read the mesh into the arena's tail, rebase it, measure
   it, and pull the exporter's 128-edge UVs in to 127. */
void destiny_tablet_load_geometry(void) {
    int i;
    dt_smd = NULL;
    dt_buf = room_arena_load_extra("\\TEXUNDW\\DSTTBLT.SMD;1");
    dt_gen = room_arena_generation();
    if (dt_buf) dt_smd = smdInitData(dt_buf);

    if (dt_smd && dt_smd->n_verts > 0 && dt_smd->n_prims <= DT_MAX_PRIMS) {
        dt_min_x = dt_max_x = dt_smd->p_verts[0].vx;
        dt_min_y = dt_max_y = dt_smd->p_verts[0].vy;
        dt_min_z = dt_max_z = dt_smd->p_verts[0].vz;
        for (i = 1; i < dt_smd->n_verts; i++) {
            int32_t vx = dt_smd->p_verts[i].vx;
            int32_t vy = dt_smd->p_verts[i].vy;
            int32_t vz = dt_smd->p_verts[i].vz;
            if (vx < dt_min_x) dt_min_x = vx;
            if (vx > dt_max_x) dt_max_x = vx;
            if (vy < dt_min_y) dt_min_y = vy;
            if (vy > dt_max_y) dt_max_y = vy;
            if (vz < dt_min_z) dt_min_z = vz;
            if (vz > dt_max_z) dt_max_z = vz;
        }
        /* THE 128 EDGE (lightning_tablet.c's lt_index_prims): the exporter
           writes UVs on a 128 tile, so the art's far edge is u or v = 128 -
           one texel past a 128 texture, which the room's 128 window wraps to
           0. Pulled in to 127. A fresh read every entry, so once a visit. */
        {
            uint8_t *p = (uint8_t *)dt_smd->p_prims;
            int pi, k;
            for (pi = 0; pi < dt_smd->n_prims; pi++) {
                SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
                int n = (pt->type >= 2) ? 4 : 3;
                uint8_t *uv = p + 20;
                if (pt->texture)
                    for (k = 0; k < 2 * n; k++) if (uv[k] > 127) uv[k] = 127;
                p += pt->len;
            }
        }
    } else {
        dt_smd = NULL;
    }
    dt_cx = (dt_min_x + dt_max_x) / 2;
    dt_cz = (dt_min_z + dt_max_z) / 2;

    dt_shot_x = dt_cx;
    dt_shot_y = (dt_min_y + dt_max_y) / 2;
    dt_shot_z = dt_min_z - DT_SHOT_DIST;
}

/* Room entry, inside STATE_LOADING: the texture streams in. Every time: the
   tablets' page is Sharur's, the Wings Tablet's and the Lightning Tablet's
   too. */
void destiny_tablet_upload_texture(void) {
    if (dt_tex >= 0) texmgr_upload(dt_tex);
}

/* ---- The board's state ---------------------------------------------------- */

static int dt_word_at(int col, int row) { return col * DT_SIDE + row; }

static int dt_is_used(int w) { return (dt_used >> w) & 1; }

/* The nearest free row to `row` in column `col`, searching outward (down
   first on a tie); -1 if the column has none (it cannot: at most four of its
   eight are ever placed). */
static int dt_nearest_free(int col, int row) {
    int d;
    for (d = 0; d < DT_SIDE; d++) {
        if (row + d < DT_SIDE && !dt_is_used(dt_word_at(col, row + d))) return row + d;
        if (row - d >= 0      && !dt_is_used(dt_word_at(col, row - d))) return row - d;
    }
    return -1;
}

static void dt_board_clear(void) {
    dt_n    = 0;
    dt_used = 0;
    dt_col  = 0;
    dt_row  = 0;
}

static int dt_solved(void) {
    int i;
    if (dt_n != DT_BLANKS) return 0;
    for (i = 0; i < DT_BLANKS; i++) if (dt_placed[i] != DT_ANSWER[i]) return 0;
    return 1;
}

/* ---- Room entry ---------------------------------------------------------------- */

void destiny_tablet_arm(void) {
    if (dt_state != DT_IDLE) camera_release_player();
    dt_state       = DT_IDLE;
    dt_t           = 0;
    dt_shard_count = 0;
    dt_circle_prev = interact_tapped();
    dt_board_clear();
}

static int dt_item_live(void) {
    int i;
    for (i = 0; i < item_pickup_count; i++)
        if (item_pickups[i].active &&
            item_pickups[i].kind == PICKUP_FAKE_TABLET_OF_DESTINIES)
            return 1;
    return 0;
}

static void dt_spawn_item(void) {
    if (dt_item_live()) return;
    item_pickup_spawn_from(DT_SEEDED_PICKUPS, dt_cx, dt_max_y - DT_ITEM_UP, dt_cz,
                           PICKUP_FAKE_TABLET_OF_DESTINIES, 1, 0);
}

void destiny_tablet_apply_flags(void) {
    if (!dt_loaded()) return;
    if (game_flag(FLAG_DESTINY_TABLET_BROKEN) && !game_flag(FLAG_FAKE_TABLET_TAKEN))
        dt_spawn_item();
}

int destiny_tablet_examining(void) { return dt_state != DT_IDLE; }
int destiny_tablet_scene(void)     { return dt_state == DT_SCENE; }

/* ---- The shards (lightning_tablet.c's, verbatim but for the greys) --------- */

static void dt_shatter(void) {
    int32_t hl = (dt_max_x - dt_min_x) / 2;
    int32_t ht = (dt_max_z - dt_min_z) / 2;
    int i;
    for (i = 0; i < DT_SHARDS; i++) {
        DtShard *s = &dt_shards[i];
        int32_t a = -hl + ((2 * hl) * (2 * i + 1)) / (2 * DT_SHARDS) + dt_rand(81) - 40;
        int32_t b = dt_rand(2 * ht + 1) - ht;
        int32_t out   = (i & 1) ? (8 + dt_rand(12)) : -(8 + dt_rand(12));
        int32_t along = dt_rand(17) - 8;
        s->x  = dt_cx + a;
        s->z  = dt_cz + b;
        s->y  = dt_min_y + 80 + dt_rand((dt_max_y - dt_min_y) - 160 + 1);
        s->vx = along;
        s->vz = out;
        s->vy = -(10 + dt_rand(14));
        s->rx = (int16_t)dt_rand(4096);
        s->ry = (int16_t)dt_rand(4096);
        s->rz = (int16_t)dt_rand(4096);
        s->sx = (int16_t)(dt_rand(81) - 40);
        s->sy = (int16_t)(dt_rand(81) - 40);
        s->sz = (int16_t)(dt_rand(81) - 40);
        s->half     = (int16_t)(40 + dt_rand(21));
        s->life     = (int16_t)(80 + dt_rand(31));
        s->max_life = s->life;
        s->grey     = DT_GREYS[dt_rand(16)];
    }
    dt_shard_count = DT_SHARDS;
}

static void dt_update_shards(void) {
    int i;
    for (i = 0; i < dt_shard_count; i++) {
        DtShard *s = &dt_shards[i];
        if (s->life <= 0) continue;
        s->x += s->vx;
        s->y += s->vy;
        s->z += s->vz;
        s->vy += 3;
        s->vx = (s->vx * 28) >> 5;
        s->vz = (s->vz * 28) >> 5;
        s->rx = (int16_t)((s->rx + s->sx) & 4095);
        s->ry = (int16_t)((s->ry + s->sy) & 4095);
        s->rz = (int16_t)((s->rz + s->sz) & 4095);
        if (s->y > dt_max_y - s->half) {
            s->y = dt_max_y - s->half;
            if (s->vy > 0) s->vy = -(s->vy * 3) / 10;
            if (s->vy > -3) s->vy = 0;
            s->sx = (int16_t)((s->sx * 3) / 4);
            s->sy = (int16_t)((s->sy * 3) / 4);
            s->sz = (int16_t)((s->sz * 3) / 4);
        }
        s->life--;
    }
}

/* ---- Collision (the Lightning Tablet's scheme) ------------------------------ */

void destiny_tablet_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    if (!dt_standing()) return;

    int32_t feet = py + GROUND_FLOOR_Y, head = py - DT_PLAYER_HEAD;
    if (head >= dt_max_y || feet <= dt_min_y) return;

    int32_t min_x = dt_min_x - radius, max_x = dt_max_x + radius;
    int32_t min_z = dt_min_z - radius, max_z = dt_max_z + radius;
    if (*px <= min_x || *px >= max_x) return;
    if (*pz <= min_z || *pz >= max_z) return;

    int32_t pl = *px - min_x, pr = max_x - *px;
    int32_t pf = *pz - min_z, pb = max_z - *pz;
    int32_t m = pl, ddx = -pl, ddz = 0;
    if (pr < m) { m = pr; ddx =  pr; ddz = 0; }
    if (pf < m) { m = pf; ddx = 0; ddz = -pf; }
    if (pb < m) {         ddx = 0; ddz =  pb; }
    *px += ddx; *pz += ddz;
}

/* ---- Per frame ---------------------------------------------------------------- */

static int dt_front_point(int32_t *qx, int32_t *qz) {
    if (cam_z >= dt_min_z) return 0;
    *qx = dt_clamp(cam_x, dt_min_x, dt_max_x);
    *qz = dt_min_z;
    return 1;
}

static int32_t dt_ease(int32_t t, int32_t len) {
    int32_t lin, inv;
    /* THE ENDS BEFORE THE DIVIDE (memory: clamp before divide). */
    if (t <= 0)   return 0;
    if (t >= len) return 256;
    lin = (t << 8) / len;
    inv = 256 - lin;
    return 256 - ((inv * inv) >> 8);
}

static void dt_start(void) {
    int32_t d;
    dt_save_x = cam_x; dt_save_y = cam_y; dt_save_z = cam_z;
    dt_save_rot = cam_rot; dt_save_pitch = cam_pitch; dt_save_vy = cam_vy;
    camera_anchor_player(cam_x, cam_y, cam_z);

    dt_src_x = cam_x; dt_src_y = cam_y; dt_src_z = cam_z;
    dt_src_rot = cam_rot; dt_src_pitch = cam_pitch;
    d = ((DT_YAW - dt_src_rot) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    dt_rot_delta = d;

    dt_board_clear();
    dt_t     = 0;
    dt_state = DT_INTRO;
}

/* Back where the player stood, control returned; the next Circle must be
   fresh. The board is always empty here (Cross only leaves an empty one). */
static void dt_exit(void) {
    cam_x = dt_save_x; cam_y = dt_save_y; cam_z = dt_save_z;
    cam_rot = dt_save_rot; cam_pitch = dt_save_pitch; cam_vy = dt_save_vy;
    camera_release_player();
    dt_state       = DT_IDLE;
    dt_t           = 0;
    dt_circle_prev = 1;
    dt_board_clear();
}

static void dt_scene_tick(void) {
    dt_t++;
    /* The words light in SILENCE (the user's call): no chime per word. The
       only sound in the scene is the shatter's rumble. */
    if (dt_t == DT_SC_SHATTER) {
        /* SET AS IT GOES, before anything that could fail after it. The scene
           cannot be left before this frame: no Cross, no enemies, and the
           cabin's lava is nowhere near the shot. */
        game_flag_set(FLAG_DESTINY_TABLET_BROKEN);
        sound_play(SFX_RUMBLE_4);
        dt_shatter();
        dt_spawn_item();
    }
    if (dt_t >= DT_SC_END) dt_exit();
}

/* The board: the cursor, Circle to place, Cross to take back or leave. */
static void dt_board_input(uint16_t pressed) {
    /* CROSS: the last word back off the stone, or out if there is none. */
    if (pressed & PAD_CROSS) {
        sound_play(SFX_BACK);
        if (dt_n > 0) {
            dt_n--;
            dt_used &= (uint16_t)~(1u << dt_placed[dt_n]);
        } else {
            dt_exit();
        }
        return;
    }

    /* UP / DOWN: the next free word that way in this column (the used ones
       are skipped); nothing if there is none. */
    if (pressed & (PAD_UP | PAD_DOWN)) {
        int step = (pressed & PAD_UP) ? -1 : 1, r = dt_row + step;
        while (r >= 0 && r < DT_SIDE && dt_is_used(dt_word_at(dt_col, r))) r += step;
        if (r >= 0 && r < DT_SIDE) { dt_row = r; sound_play(SFX_CURSOR); }
    }
    /* LEFT / RIGHT: across to the other column, onto the nearest free word. */
    if (((pressed & PAD_LEFT) && dt_col == 1) || ((pressed & PAD_RIGHT) && dt_col == 0)) {
        int r = dt_nearest_free(1 - dt_col, dt_row);
        if (r >= 0) { dt_col = 1 - dt_col; dt_row = r; sound_play(SFX_CURSOR); }
    }

    /* CIRCLE: the word under the cursor into the next blank. */
    if (pressed & PAD_CIRCLE) {
        int w = dt_word_at(dt_col, dt_row);
        if (dt_n >= DT_BLANKS || dt_is_used(w)) return;
        dt_placed[dt_n++] = (uint8_t)w;
        dt_used |= (uint16_t)(1u << w);
        sound_play(SFX_SELECT);
        /* The cursor off the word it just placed, onto the nearest free one
           in the same column. */
        {
            int r = dt_nearest_free(dt_col, dt_row);
            if (r >= 0) dt_row = r;
        }
        if (dt_n == DT_BLANKS) {
            if (dt_solved()) {
                dt_state = DT_SCENE;
                dt_t     = 0;
            } else {
                /* Wrong: nothing happens but the log line, and the board
                   stays up for Cross to take words back. */
                show_pickup_msg_raw("That doesn't make sense");
            }
        }
    }
}

void destiny_tablet_update(int lock) {
    uint16_t btn = 0, pressed;
    if (current_area != STATE_DESTINYS_CABIN) return;
    if (pad_buff_len[0]) { PadResponse *pad = (PadResponse *)pad_buff[0]; btn = ~pad->btn; }

    dt_clock++;
    dt_update_shards();

    if (dt_state == DT_IDLE) {
        int held = interact_tapped();
        int just = held && !dt_circle_prev;
        int32_t qx, qz;
        dt_circle_prev = held;
        if (lock || !just || !dt_standing()) return;
        if (!dt_front_point(&qx, &qz)) return;
        if (dt_abs(cam_x - qx) + dt_abs(cam_z - qz) >= DT_REACH) return;
        if (!interact_facing(qx, qz)) return;
        dt_start();
        dt_btn_prev = btn;              /* the Circle that opened it is not a press */
        return;
    }

    if (dt_state == DT_SCENE) {         /* a cutscene: nothing reads the pad */
        dt_btn_prev = btn;
        dt_scene_tick();
        return;
    }

    pressed     = btn & ~dt_btn_prev;
    dt_btn_prev = btn;

    if (dt_state == DT_INTRO) {
        int32_t e;
        /* CROSS during the flight leaves, as on the Lightning Tablet. */
        if (pressed & PAD_CROSS) {
            sound_play(SFX_BACK);
            dt_exit();
            return;
        }
        dt_t++;
        e = dt_ease(dt_t, DT_INTRO_FRAMES);
        cam_x     = dt_src_x     + (((dt_shot_x - dt_src_x) * e) >> 8);
        cam_y     = dt_src_y     + (((dt_shot_y - dt_src_y) * e) >> 8);
        cam_z     = dt_src_z     + (((dt_shot_z - dt_src_z) * e) >> 8);
        cam_rot   = dt_src_rot   + ((dt_rot_delta * e) >> 8);
        cam_pitch = dt_src_pitch - ((dt_src_pitch * e) >> 8);
        cam_vy    = 0;
        if (dt_t >= DT_INTRO_FRAMES) {
            cam_x = dt_shot_x; cam_y = dt_shot_y; cam_z = dt_shot_z;
            cam_rot = DT_YAW; cam_pitch = 0; cam_vy = 0;
            dt_t     = 0;
            dt_state = DT_BOARD;
        }
        return;
    }

    dt_board_input(pressed);
}

/* ---- Drawing: the slab ------------------------------------------------------- */

/* The Lightning Tablet's primitive loop, textured prims only - every face of
   this export carries the one texture. */
static void dt_draw_slab(RenderContext *ctx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint16_t tp = texmgr_tpage(dt_tex), cl = texmgr_clut(dt_tex);

    uint8_t *p = (uint8_t *)dt_smd->p_prims;
    int pi;
    for (pi = 0; pi < dt_smd->n_prims; pi++) {
        SMD_PRI_TYPE *pt       = (SMD_PRI_TYPE *)p;
        uint8_t       stride   = pt->len;
        int           is_quad  = (pt->type >= 2);

        if (!pt->texture) { p += stride; continue; }

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &dt_smd->p_verts[vi[0]];
        SVECTOR *v1 = &dt_smd->p_verts[vi[1]];
        SVECTOR *v2 = &dt_smd->p_verts[vi[2]];

        int32_t fdx = ((int32_t)v0->vx + v2->vx) / 2 - cam_x;
        int32_t fdz = ((int32_t)v0->vz + v2->vz) / 2 - cam_z;
        int32_t fdist = (fdx < 0 ? -fdx : fdx) + (fdz < 0 ? -fdz : fdz);
        if (fdist > g_fog_far) { p += stride; continue; }
        int32_t fog_factor = render_fog_scale(fdist);

        DVECTOR sv[4];
        int32_t sz[4], otz, nclip;

        gte_ldv3(v0, v1, v2);
        gte_rtpt();
        gte_stsxy3c(sv);

        if (sv[0].vx <= -1023 || sv[0].vx >= 1023 || sv[0].vy <= -1023 || sv[0].vy >= 1023 ||
            sv[1].vx <= -1023 || sv[1].vx >= 1023 || sv[1].vy <= -1023 || sv[1].vy >= 1023 ||
            sv[2].vx <= -1023 || sv[2].vx >= 1023 || sv[2].vy <= -1023 || sv[2].vy >= 1023) {
            p += stride; continue;
        }

        if (!pt->nocull) {
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }

        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        if (is_quad) {
            SVECTOR *v3 = &dt_smd->p_verts[vi[3]];
            gte_ldv0(v3);
            gte_rtps();
            gte_stsxy(&sv[3]);
            gte_stsz(&sz[3]);
            if (sv[3].vx <= -1023 || sv[3].vx >= 1023 || sv[3].vy <= -1023 || sv[3].vy >= 1023) { p += stride; continue; }
            if (sz[3] == 0) { p += stride; continue; }
            gte_avsz4();
        } else {
            gte_avsz3();
        }

        gte_stotz(&otz);
        if (otz <= 0) { p += stride; continue; }
        if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
        if (otz >= UW_SKY_OT)   otz = UW_SKY_OT - 1;

        uint8_t *col = p + 16;
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + DT_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + DT_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + DT_FOG_B * (256 - fog_factor)) >> 8);
        uint8_t *uv = p + 20;

        if (is_quad) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, r, g, b);
            poly->tpage = tp;
            poly->clut  = cl;
            poly->u0=uv[0]; poly->v0=uv[1];
            poly->u1=uv[2]; poly->v1=uv[3];
            poly->u2=uv[4]; poly->v2=uv[5];
            poly->u3=uv[6]; poly->v3=uv[7];
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_FT4);
        } else {
            if (ctx->next_packet + sizeof(POLY_FT3) > buf_end) { p += stride; continue; }
            POLY_FT3 *poly = (POLY_FT3 *)ctx->next_packet;
            setPolyFT3(poly);
            setRGB0(poly, r, g, b);
            poly->tpage = tp;
            poly->clut  = cl;
            poly->u0=uv[0]; poly->v0=uv[1];
            poly->u1=uv[2]; poly->v1=uv[3];
            poly->u2=uv[4]; poly->v2=uv[5];
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_FT3);
        }

        p += stride;
    }
}

/* One cube (lightning_tablet.c's lt_draw_shard). */
static void dt_draw_shard(RenderContext *ctx, const DtShard *s, const MATRIX *view) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    SVECTOR  rot = { s->rx, s->ry, s->rz, 0 };
    MATRIX   m;
    SVECTOR  wv[8];
    DVECTOR  scr[8];
    int32_t  sz[8], h, dist, fog, k, f;

    h = s->half;
    if (s->life < DT_SHARD_SHRINK) h = (h * s->life) / DT_SHARD_SHRINK;
    if (h <= 0) return;

    dist = dt_abs(s->x - cam_x) + dt_abs(s->z - cam_z);
    if (dist > g_fog_far) return;
    fog = render_fog_scale(dist);

    RotMatrix(&rot, &m);
    for (k = 0; k < 8; k++) {
        int32_t lx = (k & 1) ? h : -h;
        int32_t ly = (k & 2) ? h : -h;
        int32_t lz = (k & 4) ? h : -h;
        wv[k].vx = (int16_t)(s->x + ((m.m[0][0] * lx + m.m[0][1] * ly + m.m[0][2] * lz) >> 12));
        wv[k].vy = (int16_t)(s->y + ((m.m[1][0] * lx + m.m[1][1] * ly + m.m[1][2] * lz) >> 12));
        wv[k].vz = (int16_t)(s->z + ((m.m[2][0] * lx + m.m[2][1] * ly + m.m[2][2] * lz) >> 12));
        wv[k].pad = 0;
    }
    dt_set_view(view);
    for (k = 0; k < 8; k++) {
        gte_ldv0(&wv[k]);
        gte_rtps();
        gte_stsxy(&scr[k]);
        gte_stsz(&sz[k]);
        if (sz[k] <= 0) return;
        if (scr[k].vx <= -1023 || scr[k].vx >= 1023 ||
            scr[k].vy <= -1023 || scr[k].vy >= 1023) return;
    }

    for (f = 0; f < 6; f++) {
        int axis = f >> 1;
        int bit  = 1 << axis;
        int ba   = 1 << ((axis + 1) % 3);
        int bb   = 1 << ((axis + 2) % 3);
        int c0   = (f & 1) ? bit : 0;
        int c1 = c0 | ba, c2 = c0 | bb, c3 = c0 | ba | bb;
        int32_t otz = (sz[c0] + sz[c1] + sz[c2] + sz[c3]) >> 4;
        int32_t lit = ((int32_t)s->grey * DT_FACE_SHADE[f]) >> 8;
        uint8_t r, g, b;
        POLY_F4 *poly;

        if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
        if (otz >= UW_SKY_OT)   otz = UW_SKY_OT - 1;
        if (ctx->next_packet + sizeof(POLY_F4) > buf_end) return;

        r = (uint8_t)((lit * fog + UW_FOG_R * (256 - fog)) >> 8);
        g = (uint8_t)((lit * fog + UW_FOG_G * (256 - fog)) >> 8);
        b = (uint8_t)((lit * fog + UW_FOG_B * (256 - fog)) >> 8);

        poly = (POLY_F4 *)ctx->next_packet;
        setPolyF4(poly);
        setRGB0(poly, r, g, b);
        poly->x0 = scr[c0].vx; poly->y0 = scr[c0].vy;
        poly->x1 = scr[c1].vx; poly->y1 = scr[c1].vy;
        poly->x2 = scr[c2].vx; poly->y2 = scr[c2].vy;
        poly->x3 = scr[c3].vx; poly->y3 = scr[c3].vy;
        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
        ctx->next_packet += sizeof(POLY_F4);
    }
}

void destiny_tablet_draw(RenderContext *ctx) {
    MATRIX view;
    int    i;
    if (!dt_live()) return;

    camera_build_view(&view);
    dt_set_view(&view);

    if (dt_standing()) {
        int32_t dist = dt_abs(dt_cx - cam_x) + dt_abs(dt_cz - cam_z);
        if (dist - (dt_max_x - dt_min_x) / 2 <= g_fog_far)
            dt_draw_slab(ctx);
    }

    for (i = 0; i < dt_shard_count; i++)
        if (dt_shards[i].life > 0) dt_draw_shard(ctx, &dt_shards[i], &view);

    /* The view back for whatever draws next. */
    dt_set_view(&view);

    /* THE SIGN, in front of the middle of the face, only while it can be acted
       on: standing, not being read, the player in front. */
    if (dt_standing() && dt_state == DT_IDLE) {
        int32_t qx, qz, xz;
        int fade = 256;
        if (dt_front_point(&qx, &qz)) {
            xz = dt_abs(cam_x - qx) + dt_abs(cam_z - qz);
            if (xz < DT_TEXT_RADIUS) {
                if (xz > DT_FADE_NEAR) {
                    int range = DT_TEXT_RADIUS - DT_FADE_NEAR;
                    int prog  = xz - DT_FADE_NEAR;
                    if (prog > range) prog = range;
                    fade = 256 - ((prog * 256) / range);
                }
                door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to interact",
                                        dt_cx, dt_max_y - DT_SIGN_UP,
                                        dt_min_z - DT_SIGN_OUT,
                                        50, 255, 50, fade, DT_YAW,
                                        DOOR_PIXEL_SIZE);
            }
        }
    }
}

/* ---- Drawing: the board ---------------------------------------------------------
   SCREEN SPACE, in the 5x7 screen font (door_draw_string_2d, 6 px a letter).

   THE LAYOUT (the user's): each side column's eight words in order, split
   four and four round the four lines, which run across the middle. The
   blanks line up in ONE column at DT_BLANK_X: the text before a blank ends
   one letter short of it, the text after starts one letter past the blank
   (or past the word in it, which may be longer).

   Vertically it keeps clear of the log box, which stays up over the board
   (main.c's `puzzle` list, so "That doesn't make sense" can be read) and sits
   bottom right, y 191..232: the board runs y 14..180, the controls at the
   foot on the left.

   Horizontally: "RETURNED BY" is 11 letters (65 px) and "______ DEFIED &
   STOLE" 21 (125 px); laid out round a 6 px gap each side of the blank
   column, that block is 196 wide, centred on 320 when DT_BLANK_X is 134. The
   longest word in the third blank (LIGHTNING) takes its right end to 276.

   OT SLOTS: the puzzles' 12/10/8/7 (hud.c's list), clear of the log box's. */
#define DT_OT_HINT      7
#define DT_OT_TEXT      8
#define DT_OT_GLOW     10
#define DT_OT_BACK     12

#define DT_CELL         DOOR_SMALL_CELL_W
#define DT_GLYPH_H      7
#define DT_PITCH       13
#define DT_GAP          8         /* extra between a side block and the lines */
#define DT_TOP_Y       14
#define DT_LEFT_X      16         /* left column, left-aligned             */
#define DT_RIGHT_X    304         /* right column, right-aligned to this   */
#define DT_BLANK_X    134
#define DT_BLANK_LEN    6         /* "______", as the user drew it         */
#define DT_HINT_Y     214

/* Row y of side row r (0..7) and line l (0..3). */
static int dt_side_y(int r) {
    return DT_TOP_Y + r * DT_PITCH + (r >= 4 ? 4 * DT_PITCH + 2 * DT_GAP : 0);
}
static int dt_line_y(int l) {
    return DT_TOP_Y + 4 * DT_PITCH + DT_GAP + l * DT_PITCH;
}

static int dt_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int dt_text_w(const char *s) { int n = dt_strlen(s); return n ? n * DT_CELL - 1 : 0; }

static uint8_t dt_scale(int32_t c, int32_t f) { return (uint8_t)((c * f) >> 8); }

static void dt_tile(RenderContext *ctx, int ot, int x, int y, int w, int h,
                    uint8_t r, uint8_t g, uint8_t b, int semi) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    TILE *t;
    if (w <= 0 || h <= 0) return;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    t = (TILE *)ctx->next_packet;
    setTile(t);
    setSemiTrans(t, semi);
    setXY0(t, x, y);
    setWH(t, w, h);
    setRGB0(t, r, g, b);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[ot], t);
    ctx->next_packet += sizeof(TILE);
}

/* The blend for every semi-transparent prim in slot `ot`. Added AFTER them:
   an OT node is LIFO, so the GPU sees the mode first. */
static void dt_blend(RenderContext *ctx, int ot, int abr) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    DR_TPAGE *tp;
    if (ctx->next_packet + sizeof(DR_TPAGE) > buf_end) return;
    tp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(tp, 0, 0, getTPage(0, abr, 320, 0));
    addPrim(&ctx->buffers[ctx->active_buffer].ot[ot], tp);
    ctx->next_packet += sizeof(DR_TPAGE);
}

/* A dark band behind a run of text, so the letters read over the carving.
   SUBTRACTIVE (ABR=2), so it fades with `f` - a black 50% tile could not. */
static void dt_backing(RenderContext *ctx, int x, int y, int w, int32_t f) {
    uint8_t k = dt_scale(110, f);
    if (w <= 0 || !k) return;
    dt_tile(ctx, DT_OT_BACK, x - 3, y - 3, w + 6, DT_GLYPH_H + 6, k, k, k, 1);
}

static void dt_text(RenderContext *ctx, const char *s, int x, int y,
                    uint8_t r, uint8_t g, uint8_t b, int32_t f) {
    if (!s[0] || f <= 0) return;
    dt_backing(ctx, x, y, dt_text_w(s), f);
    door_draw_string_2d(ctx, s, x, y, dt_scale(r, f), dt_scale(g, f), dt_scale(b, f),
                        DT_OT_TEXT);
}

/* The double outline the boards' reticules use. */
static void dt_outline(RenderContext *ctx, int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b) {
    dt_tile(ctx, DT_OT_TEXT, x,         y,         w, 1, r, g, b, 0);
    dt_tile(ctx, DT_OT_TEXT, x,         y + h - 1, w, 1, r, g, b, 0);
    dt_tile(ctx, DT_OT_TEXT, x,         y,         1, h, r, g, b, 0);
    dt_tile(ctx, DT_OT_TEXT, x + w - 1, y,         1, h, r, g, b, 0);
}

/* How lit placed word `i` is this frame in the scene, 0..256. */
static int32_t dt_glow_level(int i) {
    int32_t t;
    if (dt_state != DT_SCENE) return 0;
    t = dt_t - (DT_SC_GLOW_FROM + i * DT_SC_GLOW_STEP);
    if (t <= 0) return 0;
    if (t >= DT_SC_GLOW_UP) return 256;
    return (t * 256) / DT_SC_GLOW_UP;
}

/* The side columns' fade (out over the scene's first beat) and the lines'
   (out before the shatter). */
static int32_t dt_sides_fade(void) {
    if (dt_state != DT_SCENE) return 256;
    if (dt_t >= DT_SC_SIDES_OUT) return 0;
    return 256 - (dt_t * 256) / DT_SC_SIDES_OUT;
}
static int32_t dt_lines_fade(void) {
    int32_t t;
    if (dt_state != DT_SCENE) return 256;
    t = dt_t - DT_SC_FADE_FROM;
    if (t <= 0) return 256;
    if (t >= DT_SC_SHATTER - DT_SC_FADE_FROM) return 0;
    return 256 - (t * 256) / (DT_SC_SHATTER - DT_SC_FADE_FROM);
}

static void dt_draw_sides(RenderContext *ctx) {
    int32_t f = dt_sides_fade();
    int col, row;
    if (f <= 0) return;
    for (col = 0; col < 2; col++) {
        for (row = 0; row < DT_SIDE; row++) {
            int w  = dt_word_at(col, row);
            const char *s = DT_WORD[w];
            int tw = dt_text_w(s);
            int x  = col ? DT_RIGHT_X - tw : DT_LEFT_X;
            int y  = dt_side_y(row);
            int on = (dt_state == DT_BOARD && col == dt_col && row == dt_row);
            if (dt_is_used(w))
                dt_text(ctx, s, x, y,  70,  66,  60, f);   /* in a blank: greyed */
            else if (on)
                dt_text(ctx, s, x, y, 255, 230, 120, f);   /* under the cursor   */
            else
                dt_text(ctx, s, x, y, 205, 195, 170, f);
            if (on) {
                dt_outline(ctx, x - 5, y - 5, tw + 10, DT_GLYPH_H + 10,  80,  80, 200);
                dt_outline(ctx, x - 4, y - 4, tw + 8,  DT_GLYPH_H + 8,  180, 180, 255);
            }
        }
    }
}

static void dt_draw_lines(RenderContext *ctx) {
    int32_t f = dt_lines_fade();
    int l;
    if (f <= 0) return;
    for (l = 0; l < DT_BLANKS; l++) {
        int y  = dt_line_y(l);
        int bw = DT_BLANK_LEN * DT_CELL - 1;     /* the blank's own width */
        int ww = bw;

        /* Before the blank, right-aligned one letter short of it. */
        dt_text(ctx, DT_LINE_L[l], DT_BLANK_X - DT_CELL - dt_text_w(DT_LINE_L[l]), y,
                205, 195, 170, f);

        if (l < dt_n) {
            /* A placed word, white - and in the scene, warming to gold and
               haloed (ADDITIVE, behind the letters) as its turn comes. */
            const char *s = DT_WORD[dt_placed[l]];
            int32_t gl = dt_glow_level(l);
            int tw = dt_text_w(s);
            if (tw > ww) ww = tw;
            if (gl > 0) {
                /* A slow breath on the halo once it is up. */
                int32_t ph = (dt_clock * 3 + l * 64) & 255;
                int32_t br = 200 + (ph < 128 ? ph : 255 - ph) / 2;   /* 200..263 */
                int32_t hk = (gl * br) >> 8;
                hk = (hk * f) >> 8;
                dt_tile(ctx, DT_OT_GLOW, DT_BLANK_X - 6, y - 6, tw + 12, DT_GLYPH_H + 12,
                        dt_scale(70, hk), dt_scale(50, hk), dt_scale(10, hk), 1);
                dt_tile(ctx, DT_OT_GLOW, DT_BLANK_X - 3, y - 3, tw + 6, DT_GLYPH_H + 6,
                        dt_scale(110, hk), dt_scale(80, hk), dt_scale(20, hk), 1);
            }
            {
                uint8_t r = 255;
                uint8_t g = (uint8_t)(255 - ((40 * gl) >> 8));
                uint8_t b = (uint8_t)(255 - ((165 * gl) >> 8));
                /* No backing under a lit word: the halo is its own. */
                if (gl > 0)
                    door_draw_string_2d(ctx, s, DT_BLANK_X, y, dt_scale(r, f),
                                        dt_scale(g, f), dt_scale(b, f), DT_OT_TEXT);
                else
                    dt_text(ctx, s, DT_BLANK_X, y, r, g, b, f);
            }
        } else {
            /* An empty blank: a rule on the baseline. The NEXT one to fill
               pulses, so the player can see where Circle will put a word. */
            uint8_t k = 120;
            if (l == dt_n && dt_state == DT_BOARD) {
                int32_t ph = (dt_clock * 6) & 255;
                k = (uint8_t)(150 + (ph < 128 ? ph : 255 - ph) * 105 / 128);
            }
            dt_backing(ctx, DT_BLANK_X, y, bw, f);
            dt_tile(ctx, DT_OT_TEXT, DT_BLANK_X, y + DT_GLYPH_H - 1, bw, 1,
                    dt_scale(k, f), dt_scale(k, f), dt_scale(k * 9 / 10, f), 0);
        }

        /* After the blank, one letter past it or past its word. */
        dt_text(ctx, DT_LINE_R[l], DT_BLANK_X + ww + DT_CELL, y, 205, 195, 170, f);
    }
}

void destiny_tablet_draw_overlay(RenderContext *ctx) {
    if ((dt_state != DT_BOARD && dt_state != DT_SCENE) || !dt_live()) return;
    /* Nothing once the stone has gone. */
    if (dt_state == DT_SCENE && dt_t >= DT_SC_SHATTER) return;

    dt_draw_sides(ctx);
    dt_draw_lines(ctx);
    dt_blend(ctx, DT_OT_BACK, 2);   /* the backings: subtract */
    dt_blend(ctx, DT_OT_GLOW, 1);   /* the halos: add         */

    if (dt_state == DT_BOARD)
        btn_prompt_draw(ctx, 8, DT_HINT_Y,
                        BTN_CIRCLE " Place  " BTN_CROSS " Back", DT_OT_HINT);
}
