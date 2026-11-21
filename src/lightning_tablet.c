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
#include "player.h"         /* game flags        */
#include "sound.h"
#include "item_pickup.h"    /* the Lightning Bolt */
#include "door.h"           /* the "Press O to interact" sign */
#include "btn_glyph.h"
#include "underworld.h"     /* the chapter's fog */
#include "underworld_sky.h" /* UW_SKY_OT         */
#include "room_arena.h"     /* the mesh rides in the room's arena */
#include "lightning_tablet.h"

/* The Lightning Tablet - see lightning_tablet.h. The Wings Tablet's module
   (src/wings_tablet.c: the in-place mesh measured into a box, the sign, the
   shot, the shards) with the Anzu Tablet's quarter turns (src/anzu_puzzle.c)
   done in the mesh's own UVs, and the Seal's lit lines (src/seal.c). */

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- The mesh ------------------------------------------------------------
   92 prims as exported; the caps bound the per-prim and per-UV tables. */
#define LT_MAX_PRIMS   128
#define LT_MAX_UV      192

/* >>> THE MESH IS ROOM-SCOPED, IN THE ROOM ARENA'S TAIL, NOT ON THE HEAP. <<<
   The Wings Tablet holds its mesh for the whole run; this one is read on
   every entry to Lightnings Edge into the space behind the room's own file
   (room_arena_load_extra) and is gone the moment the next room loads over
   it. 6 KB of heap handed back at every door in the game, Asag's included.
   lt_gen is the arena generation it was read in: lt_live() trusts the
   pointer only while that is still the arena's. */
static SMD  *lt_smd = NULL;
static void *lt_buf = NULL;
static int   lt_gen = -1;
static uint8_t lt_ninurta[LT_MAX_PRIMS];   /* 1 = a front face of Ninurta's
                                              panel: brightened by the glow */

/* The box, measured off the mesh at load. WORLD space. -Y is up, so lt_min_y
   is the top of the slab and lt_max_y its base. */
static int32_t lt_min_x = 0, lt_max_x = 0;   /*  -600 ..  600 as authored */
static int32_t lt_min_z = 0, lt_max_z = 0;   /*  2450 .. 2550             */
static int32_t lt_min_y = 0, lt_max_y = 0;   /*  -700 ..    0             */
static int32_t lt_cx = 0, lt_cz = 0;

/* TWO TEXTURES, split down the face at x=200 by the export: "lightning tablet
   puzzle" (the cloud frieze over the grid, the grid, the ends and the back
   over them) and "lightning tablet ninurta" (his panel, its frieze and the
   back behind it). smxlink bakes each face's own palette into the prim, so
   lt_index_prims sorts the faces by that (lt_ntex), not by position - the
   right-hand END face, x=600, is puzzle art. */
static int lt_tex_p = -1;   /* puzzle  - \TEXUNDW\LTNGPZL.TIM  */
static int lt_tex_n = -1;   /* ninurta - \TEXUNDW\LTNGNNRT.TIM */
static uint8_t lt_ntex[LT_MAX_PRIMS];   /* 1 = drawn with Ninurta's texture */

#define LT_PLAYER_HEAD 30

/* ---- THE FOG TARGET: the Wings Tablet's reasoning (wings_tablet.c), on this
   art's own 95th-percentile stone texel, 162 (median 109). */
#define LT_TEX_BRIGHT 162
#define LT_FOG_R (UW_FOG_R * 128 / LT_TEX_BRIGHT)
#define LT_FOG_G (UW_FOG_G * 128 / LT_TEX_BRIGHT)
#define LT_FOG_B (UW_FOG_B * 128 / LT_TEX_BRIGHT)

/* ---- The squares -----------------------------------------------------------
   THE GRID IS MEASURED OFF THE BOX. The face is six squares wide: four in the
   grid, Ninurta's panel the other two. A square is (max_x - min_x) / 6 (200
   as authored); the grid sits on the floor of the face, its left edge on the
   slab's, three squares tall - the frieze is what is left above it.

   Cells are numbered row * 4 + col, row 0 at the TOP, col 0 at the LEFT as the
   reader sees it (the cloud's corner: the reader looks +Z, so +X is right). */
#define LT_COLS            4
#define LT_ROWS            3
#define LT_CELLS          12
#define LT_FACE_SQUARES    6

/* Which squares carry YELLOW lightning - counted off "lightning tablet
   puzzle.png" (yellow texels per square; the blue-only ones have none):
       row 0: cols 0, 1, 3     row 1: cols 1, 2, 3     row 2: cols 1, 2   */
#define LT_YELLOW_MASK  ((1 << 0) | (1 << 1) | (1 << 3) | \
                         (1 << 5) | (1 << 6) | (1 << 7) | \
                         (1 << 9) | (1 << 10))
/* FLIP (the header): row 1 col 1 also looks joined at a half turn. */
#define LT_FLIP_CELL        5

static int32_t lt_cell = 200;          /* a square's side          */
static int32_t lt_gx0  = -600;         /* the grid's left edge     */
static int32_t lt_gy0  = -600;         /* the grid's top edge      */

/* Every UV pair of every square's corner vertex, front fans and back quads:
   its byte offset into the prim buffer, its square, and which corner of the
   square the VERTEX stands on (0 TL, 1 TR, 2 BR, 3 BL - clockwise as the
   reader sees it). The centre vertices never change and are not listed. */
typedef struct { uint16_t off; uint8_t cell; uint8_t corner; } LtUv;
static LtUv    lt_uv[LT_MAX_UV];
static int     lt_uv_count = 0;
/* The art's own corners per square, read off the front fans at load. */
static uint8_t lt_base[LT_CELLS][4][2];
static uint8_t lt_rot[LT_CELLS];       /* quarter turns clockwise, 0 = true */

/* ---- The sign and the reach: the Wings Tablet's numbers (a slab of the same
   length, held the same 195 off it). */
#define LT_REACH          450
#define LT_TEXT_RADIUS   1500
#define LT_FADE_NEAR     1000
#define LT_SIGN_UP        200
#define LT_SIGN_OUT       100
#define LT_YAW              0

/* ---- The shot ----------------------------------------------------------------
   Straight out from the middle of the face, level with its middle, at 1050:
   the 1200-long face is 293 of the screen's 320 wide and the 700-tall one 171
   high, so the whole of it - cloud, squares and Ninurta - is in frame. The
   camera stands at z=1400, open floor in the middle of the room. */
#define LT_SHOT_DIST      1050
#define LT_INTRO_FRAMES     30

/* ---- The scene, in frames from the solve -------------------------------------
     0  .. 30    a beat: the cursor gone, the last square settling in the eye
     30 .. 330   the fuse burns from the cloud to the tip of the bolt (5 s -
                 the user's call: half the first cut's 2.5 s)
     330 .. 420  Ninurta glows: up, held, down (1.5 s), the lines still lit
     420         the slab shatters and leaves the Lightning Bolt
     490         control returns */
#define LT_SC_FUSE_FROM     30
#define LT_SC_FUSE_TO      330
#define LT_SC_GLOW_UP       25
#define LT_SC_GLOW_HOLD     35
#define LT_SC_GLOW_DOWN     30
#define LT_SC_SHATTER      420
#define LT_SC_END          490

/* ---- The lines -------------------------------------------------------------
   The yellow run's centre line, traced OFF THE ART (the Seal's lines were made
   the same way): "lightning tablet puzzle.png" and "lightning tablet
   ninurta.png" laid side by side at the face's own proportions (3.125 world
   units a texel both ways), a shortest path through every yellow texel from
   inside the cloud to the topmost texel of the bolt, weighted to keep to the
   MIDDLE of the stroke - then STRAIGHTENED (Douglas-Peucker, 3 texels): a
   lightning bolt's straight runs and hard corners, not the stroke's curves
   (the user's call). d is the distance along it; mx, my the corner's MITRE
   (x256) - the offset that joins the two runs meeting there edge to edge, so
   a corner is sharp with no gap and no doubled-up overlap. Clamped at 2.5x
   so a hairpin cannot spike. Re-trace it if the art changes. */
typedef struct { int16_t x, y; uint16_t d; int16_t mx, my; } LtPt;

/* 21 points; the fuse reaches the tip at 1611. */
#define LT_PATH_N    21
#define LT_PATH_DMAX 1611
static const LtPt LT_PATH[LT_PATH_N] = {
    {  -538,  -666,     0,  -252,    47 },
    {  -518,  -559,   109,  -239,   114 },
    {  -425,  -450,   252,  -130,   242 },
    {  -375,  -444,   302,     9,   259 },
    {  -297,  -459,   381,   -76,   275 },
    {  -252,  -419,   441,  -256,   115 },
    {  -252,  -366,   494,  -256,   -90 },
    {  -350,  -244,   650,  -259,   -86 },
    {  -348,  -188,   706,  -252,   114 },
    {  -288,  -131,   789,   -97,   261 },
    {  -197,  -135,   880,   128,   251 },
    {  -100,  -258,  1037,   131,   247 },
    {     6,  -266,  1143,   116,   248 },
    {    48,  -309,  1203,   253,   108 },
    {    51,  -403,  1297,   253,   102 },
    {    88,  -444,  1352,   127,   242 },
    {   134,  -450,  1398,   -17,   260 },
    {   194,  -434,  1460,    62,   282 },
    {   224,  -459,  1499,   250,   125 },
    {   228,  -538,  1578,   261,   -94 },
    {   206,  -562,  1611,   189,  -173 },
};

/* The lines stand LT_LINE_OUT proud of the face so they never z-fight it, and
   sort LT_LINE_BIAS slots in front of it. Half-widths in world units: the
   yellow stroke is ~12 texels of the 384-wide face, ~37 on it. */
#define LT_LINE_OUT          4
#define LT_LINE_BIAS         2
#define LT_HALO_HALF        30
#define LT_CORE_HALF         9

/* ---- The item: the slab's centre, 100 over the floor (the Wings Tablet's).
   Lightnings Edge seeds no pickups (world.c), so a run-time spawn may take
   any slot. */
#define LT_ITEM_UP         100
#define LT_SEEDED_PICKUPS    0

/* ---- The shards: the Wings Tablet's cubes, in this art's greys (16
   quantiles of its stone, luminance). */
#define LT_SHARDS           10
#define LT_SHARD_SHRINK     20

static const uint8_t LT_GREYS[16] = {
     49,  70,  82,  90,  96, 100, 104, 108,
    111, 115, 118, 123, 128, 136, 148, 171
};
static const uint16_t LT_FACE_SHADE[6] = { 150, 200, 256, 110, 175, 225 };

typedef struct {
    int32_t x, y, z, vx, vy, vz;
    int16_t rx, ry, rz, sx, sy, sz;
    int16_t half, life, max_life;
    uint8_t grey;
} LtShard;

static LtShard lt_shards[LT_SHARDS];
static int     lt_shard_count = 0;

/* ---- State ------------------------------------------------------------------ */
typedef enum { LT_IDLE = 0, LT_INTRO, LT_BOARD, LT_SCENE } LtState;

static LtState  lt_state = LT_IDLE;
static int32_t  lt_t     = 0;
static int      lt_cur   = 0;              /* the cursor's square, 0..11      */
static int      lt_circle_prev = 1;
static uint16_t lt_btn_prev    = 0xFFFF;
static int32_t  lt_clock = 0;              /* the lines' flicker              */

static int32_t lt_save_x, lt_save_y, lt_save_z, lt_save_rot, lt_save_pitch, lt_save_vy;
static int32_t lt_src_x, lt_src_y, lt_src_z, lt_src_rot, lt_src_pitch, lt_rot_delta;
static int32_t lt_shot_x, lt_shot_y, lt_shot_z;

static uint32_t lt_rng = 0x6B43A9u;
static int32_t lt_rand(int32_t n) {      /* 0..n-1 */
    lt_rng = lt_rng * 1103515245u + 12345u;
    return n > 0 ? (int32_t)((lt_rng >> 16) % (uint32_t)n) : 0;
}

static int32_t lt_abs(int32_t v) { return v < 0 ? -v : v; }
static int32_t lt_clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The view, loaded without the SDK's gte_SetRotMatrix/TransMatrix, which
   clobber $t0/$t1 undeclared (wings_tablet.c's wt_set_view, verbatim). */
static inline void lt_set_view(const MATRIX *m) {
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

/* The mesh was read for this visit and is still in the arena. NO AREA
   TEST, on purpose: the room's init (which deals) runs inside STATE_LOADING
   BEFORE main.c sets current_area to the room (memory: current_area is not
   a route). Gating the deal on current_area dealt the turns into lt_rot but
   never into the mesh - the face showed the solved art while the puzzle
   believed every square was turned, so it could never be solved. */
static int lt_loaded(void) {
    return lt_smd && lt_gen == room_arena_generation();
}

/* ...and the player is in the room: for the per-frame paths. */
static int lt_live(void) {
    return lt_loaded() && current_area == STATE_LIGHTNINGS_EDGE;
}

static int lt_standing(void) {
    return lt_live() && !game_flag(FLAG_LIGHTNING_TABLET_BROKEN);
}

/* ---- The squares' UVs ---------------------------------------------------- */

/* Square `c` drawn at its current turn: every corner vertex takes the art's
   corner that a clockwise turn of lt_rot[c] brings onto it - at a quarter
   turn the art's top-left lands on the top-right corner (anzu_puzzle.c's
   ROT_CORNER, as an index sum). Written straight into the prim buffer, so the
   draw needs no idea that squares exist. */
static void lt_apply(int c) {
    uint8_t *prims = (uint8_t *)lt_smd->p_prims;
    int i;
    for (i = 0; i < lt_uv_count; i++) {
        const LtUv *e = &lt_uv[i];
        int k;
        if (e->cell != c) continue;
        k = (e->corner - lt_rot[c]) & 3;
        prims[e->off]     = lt_base[c][k][0];
        prims[e->off + 1] = lt_base[c][k][1];
    }
}

/* Which corner of square `cell` the point (x, y) stands on, or -1 (the centre).
   A few units of slack: the export's coordinates are floats rounded to ints. */
static int lt_corner_of(int cell, int32_t x, int32_t y) {
    int32_t x0 = lt_gx0 + (cell % LT_COLS) * lt_cell;
    int32_t y0 = lt_gy0 + (cell / LT_COLS) * lt_cell;
    int left  = lt_abs(x - x0) <= 4,       right  = lt_abs(x - (x0 + lt_cell)) <= 4;
    int top   = lt_abs(y - y0) <= 4,       bottom = lt_abs(y - (y0 + lt_cell)) <= 4;
    if (top    && left)  return 0;
    if (top    && right) return 1;
    if (bottom && right) return 2;
    if (bottom && left)  return 3;
    return -1;
}

/* Walk the prims once: the 128-edge UVs pulled in, Ninurta's faces marked,
   and every square's corner UVs listed and their art read. */
static void lt_index_prims(void) {
    uint8_t *base = (uint8_t *)lt_smd->p_prims;
    uint8_t *p    = base;
    int pi;
    lt_uv_count = 0;
    for (pi = 0; pi < lt_smd->n_prims; pi++) {
        SMD_PRI_TYPE *pt  = (SMD_PRI_TYPE *)p;
        int       n       = (pt->type >= 2) ? 4 : 3;
        uint16_t *vi      = (uint16_t *)(p + 4);
        uint8_t  *uv      = p + 20;
        int32_t   sx = 0, sy = 0, cx, cy;
        int       front = 1, back = 1, k;

        /* THE 128 EDGE. The exporter writes UVs on a 128 tile, so the art's
           far edge is u or v = 128 - one texel past a 128 texture, which the
           room's 128 window wraps to 0 (the frieze's top row on the bottom
           row's edge). Pulled in to 127. */
        if (pt->texture)
            for (k = 0; k < 2 * n; k++) if (uv[k] > 127) uv[k] = 127;

        for (k = 0; k < n; k++) {
            SVECTOR *v = &lt_smd->p_verts[vi[k]];
            sx += v->vx; sy += v->vy;
            if (v->vz != lt_min_z) front = 0;
            if (v->vz != lt_max_z) back  = 0;
        }
        cx = sx / n; cy = sy / n;

        if (pi < LT_MAX_PRIMS)
            lt_ntex[pi] = (uint8_t)(pt->texture &&
                                    *(uint16_t *)(p + 30) == texmgr_clut(lt_tex_n));
        if (pi < LT_MAX_PRIMS)
            lt_ninurta[pi] = (uint8_t)(front &&
                                       cx > lt_gx0 + LT_COLS * lt_cell &&
                                       cy > lt_gy0);

        if ((front || back) && pt->texture &&
            cx > lt_gx0 && cx < lt_gx0 + LT_COLS * lt_cell &&
            cy > lt_gy0 && cy < lt_gy0 + LT_ROWS * lt_cell) {
            int cell = ((cy - lt_gy0) / lt_cell) * LT_COLS + (cx - lt_gx0) / lt_cell;
            for (k = 0; k < n; k++) {
                SVECTOR *v = &lt_smd->p_verts[vi[k]];
                int corner = lt_corner_of(cell, v->vx, v->vy);
                if (corner < 0) continue;
                if (front) {
                    lt_base[cell][corner][0] = uv[2 * k];
                    lt_base[cell][corner][1] = uv[2 * k + 1];
                }
                if (lt_uv_count < LT_MAX_UV) {
                    lt_uv[lt_uv_count].off    = (uint16_t)(uv + 2 * k - base);
                    lt_uv[lt_uv_count].cell   = (uint8_t)cell;
                    lt_uv[lt_uv_count].corner = (uint8_t)corner;
                    lt_uv_count++;
                }
            }
        }
        p += pt->len;
    }
}

/* ---- Load ---------------------------------------------------------------------- */

void lightning_tablet_load_assets(void) {
    /* BANK: Chapter 4 (py tools/check_tex_banks.py walks the uploader call
       graph: lightnings_edge_upload_textures reaches this module). */
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    lt_tex_p = texmgr_register("\\TEXUNDW\\LTNGPZL.TIM;1");
    lt_tex_n = texmgr_register("\\TEXUNDW\\LTNGNNRT.TIM;1");
}

/* ROOM ENTRY, from lightnings_edge_load_geometry() after the room's own
   room_data_bind(): read the mesh into the arena's tail, rebase it (a fresh
   read - the smdInitData trap, src/room_arena.h), and measure it. A fresh
   copy each time, so the squares' turned UVs never survive a visit. */
void lightning_tablet_load_geometry(void) {
    int i;
    lt_smd = NULL;
    lt_buf = room_arena_load_extra("\\TEXUNDW\\LTNGTBLT.SMD;1");
    lt_gen = room_arena_generation();
    if (lt_buf) lt_smd = smdInitData(lt_buf);

    if (lt_smd && lt_smd->n_verts > 0 && lt_smd->n_prims <= LT_MAX_PRIMS) {
        lt_min_x = lt_max_x = lt_smd->p_verts[0].vx;
        lt_min_y = lt_max_y = lt_smd->p_verts[0].vy;
        lt_min_z = lt_max_z = lt_smd->p_verts[0].vz;
        for (i = 1; i < lt_smd->n_verts; i++) {
            int32_t vx = lt_smd->p_verts[i].vx;
            int32_t vy = lt_smd->p_verts[i].vy;
            int32_t vz = lt_smd->p_verts[i].vz;
            if (vx < lt_min_x) lt_min_x = vx;
            if (vx > lt_max_x) lt_max_x = vx;
            if (vy < lt_min_y) lt_min_y = vy;
            if (vy > lt_max_y) lt_max_y = vy;
            if (vz < lt_min_z) lt_min_z = vz;
            if (vz > lt_max_z) lt_max_z = vz;
        }
    } else {
        lt_smd = NULL;
    }
    lt_cx = (lt_min_x + lt_max_x) / 2;
    lt_cz = (lt_min_z + lt_max_z) / 2;

    /* The grid, off the box (above). */
    lt_cell = (lt_max_x - lt_min_x) / LT_FACE_SQUARES;
    lt_gx0  = lt_min_x;
    lt_gy0  = lt_max_y - LT_ROWS * lt_cell;

    if (lt_smd) lt_index_prims();

    lt_shot_x = lt_cx;
    lt_shot_y = (lt_min_y + lt_max_y) / 2;
    lt_shot_z = lt_min_z - LT_SHOT_DIST;
}

/* Room entry: pure LoadImage out of the RAM copy. Every time: the tablets'
   page is Sharur's Tablet's and the Wings Tablet's too. */
void lightning_tablet_upload_texture(void) {
    if (lt_tex_p >= 0) texmgr_upload(lt_tex_p);
    if (lt_tex_n >= 0) texmgr_upload(lt_tex_n);
}

/* ---- Room entry ---------------------------------------------------------------- */

/* THE DEAL: every square a quarter turn off true, 1..3 - and the FLIP square
   only 1 or 3, never the half turn that would already look joined. Stirred
   with the vblank counter so two entries do not deal the same hand. */
static void lt_deal(void) {
    int c;
    lt_rng ^= (uint32_t)VSync(-1) * 2654435761u;
    for (c = 0; c < LT_CELLS; c++) {
        if (c == LT_FLIP_CELL) lt_rot[c] = (uint8_t)(lt_rand(2) ? 3 : 1);
        else                   lt_rot[c] = (uint8_t)(1 + lt_rand(3));
        if (lt_loaded()) lt_apply(c);
    }
}

void lightning_tablet_arm(void) {
    if (lt_state != LT_IDLE) camera_release_player();
    /* A scene cut off by a load (title, debug jump) must not carry its
       fuse loop into the next visit: its one caller keys it off. */
    if (lt_state == LT_SCENE) sound_stop(SFX_FUSE);
    lt_state       = LT_IDLE;
    lt_t           = 0;
    lt_cur         = 0;
    lt_shard_count = 0;
    lt_circle_prev = interact_tapped();
    lt_deal();
}

static int lt_bolt_live(void) {
    int i;
    for (i = 0; i < item_pickup_count; i++)
        if (item_pickups[i].active && item_pickups[i].kind == PICKUP_LIGHTNING_BOLT)
            return 1;
    return 0;
}

static void lt_spawn_bolt(void) {
    if (lt_bolt_live()) return;
    item_pickup_spawn_from(LT_SEEDED_PICKUPS, lt_cx, lt_max_y - LT_ITEM_UP, lt_cz,
                           PICKUP_LIGHTNING_BOLT, 1, 0);
}

void lightning_tablet_apply_flags(void) {
    if (!lt_loaded()) return;
    if (game_flag(FLAG_LIGHTNING_TABLET_BROKEN) && !game_flag(FLAG_LIGHTNING_BOLT_TAKEN))
        lt_spawn_bolt();
}

int lightning_tablet_examining(void) { return lt_state != LT_IDLE; }

/* ---- The shards (wings_tablet.c's, verbatim but for the greys) ------------- */

static void lt_shatter(void) {
    int32_t hl = (lt_max_x - lt_min_x) / 2;
    int32_t ht = (lt_max_z - lt_min_z) / 2;
    int i;
    for (i = 0; i < LT_SHARDS; i++) {
        LtShard *s = &lt_shards[i];
        int32_t a = -hl + ((2 * hl) * (2 * i + 1)) / (2 * LT_SHARDS) + lt_rand(81) - 40;
        int32_t b = lt_rand(2 * ht + 1) - ht;
        int32_t out   = (i & 1) ? (8 + lt_rand(12)) : -(8 + lt_rand(12));
        int32_t along = lt_rand(17) - 8;
        s->x  = lt_cx + a;
        s->z  = lt_cz + b;
        s->y  = lt_min_y + 80 + lt_rand((lt_max_y - lt_min_y) - 160 + 1);
        s->vx = along;
        s->vz = out;
        s->vy = -(10 + lt_rand(14));
        s->rx = (int16_t)lt_rand(4096);
        s->ry = (int16_t)lt_rand(4096);
        s->rz = (int16_t)lt_rand(4096);
        s->sx = (int16_t)(lt_rand(81) - 40);
        s->sy = (int16_t)(lt_rand(81) - 40);
        s->sz = (int16_t)(lt_rand(81) - 40);
        s->half     = (int16_t)(40 + lt_rand(21));
        s->life     = (int16_t)(80 + lt_rand(31));
        s->max_life = s->life;
        s->grey     = LT_GREYS[lt_rand(16)];
    }
    lt_shard_count = LT_SHARDS;
}

static void lt_update_shards(void) {
    int i;
    for (i = 0; i < lt_shard_count; i++) {
        LtShard *s = &lt_shards[i];
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
        if (s->y > lt_max_y - s->half) {
            s->y = lt_max_y - s->half;
            if (s->vy > 0) s->vy = -(s->vy * 3) / 10;
            if (s->vy > -3) s->vy = 0;
            s->sx = (int16_t)((s->sx * 3) / 4);
            s->sy = (int16_t)((s->sy * 3) / 4);
            s->sz = (int16_t)((s->sz * 3) / 4);
        }
        s->life--;
    }
}

/* ---- Collision (the Wings Tablet's scheme) ---------------------------------- */

void lightning_tablet_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    if (!lt_standing()) return;

    int32_t feet = py + GROUND_FLOOR_Y, head = py - LT_PLAYER_HEAD;
    if (head >= lt_max_y || feet <= lt_min_y) return;

    int32_t min_x = lt_min_x - radius, max_x = lt_max_x + radius;
    int32_t min_z = lt_min_z - radius, max_z = lt_max_z + radius;
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

static int lt_front_point(int32_t *qx, int32_t *qz) {
    if (cam_z >= lt_min_z) return 0;
    *qx = lt_clamp(cam_x, lt_min_x, lt_max_x);
    *qz = lt_min_z;
    return 1;
}

static int32_t lt_ease(int32_t t, int32_t len) {
    int32_t lin, inv;
    /* THE ENDS BEFORE THE DIVIDE (memory: clamp before divide). */
    if (t <= 0)   return 0;
    if (t >= len) return 256;
    lin = (t << 8) / len;
    inv = 256 - lin;
    return 256 - ((inv * inv) >> 8);
}

static void lt_start(void) {
    int32_t d;
    lt_save_x = cam_x; lt_save_y = cam_y; lt_save_z = cam_z;
    lt_save_rot = cam_rot; lt_save_pitch = cam_pitch; lt_save_vy = cam_vy;
    camera_anchor_player(cam_x, cam_y, cam_z);

    lt_src_x = cam_x; lt_src_y = cam_y; lt_src_z = cam_z;
    lt_src_rot = cam_rot; lt_src_pitch = cam_pitch;
    d = ((LT_YAW - lt_src_rot) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    lt_rot_delta = d;

    lt_t     = 0;
    lt_state = LT_INTRO;
}

/* Back where the player stood, control returned; the next Circle must be
   fresh. */
static void lt_exit(void) {
    cam_x = lt_save_x; cam_y = lt_save_y; cam_z = lt_save_z;
    cam_rot = lt_save_rot; cam_pitch = lt_save_pitch; cam_vy = lt_save_vy;
    camera_release_player();
    lt_state       = LT_IDLE;
    lt_t           = 0;
    lt_circle_prev = 1;
}

/* Every yellow square true - the FLIP square at either of its two. */
static int lt_solved(void) {
    int c;
    for (c = 0; c < LT_CELLS; c++) {
        if (!((LT_YELLOW_MASK >> c) & 1)) continue;
        if (lt_rot[c] == 0) continue;
        if (c == LT_FLIP_CELL && lt_rot[c] == 2) continue;
        return 0;
    }
    return 1;
}

static void lt_begin_scene(void) {
    /* The FLIP square upright, so the art under the lit line is the line's. */
    if (lt_rot[LT_FLIP_CELL] == 2) {
        lt_rot[LT_FLIP_CELL] = 0;
        lt_apply(LT_FLIP_CELL);
    }
    lt_state = LT_SCENE;
    lt_t     = 0;
    sound_play(SFX_SELECT);
}

/* THE FUSE'S LOOP: SFX_FUSE (1.18 s) re-keyed from C every LT_FUSE_FRAMES
   while the line burns - Sharur's hum's scheme (src/sharur.c), never a
   hardware loop (tools/ADDING_A_SOUND.txt STEP 6). 70 frames is the clip to
   within a frame at 59.94 Hz, and the clip has no silent lead or tail, so
   the seam does not gap. */
#define LT_FUSE_FRAMES      70

static void lt_scene_tick(void) {
    lt_t++;
    /* The FUSE from the first frame the line burns, on a loop, until it
       reaches the bolt; then LIGHTNING. The RUMBLE as it shatters, below. */
    if (lt_t >= LT_SC_FUSE_FROM && lt_t < LT_SC_FUSE_TO &&
        (lt_t - LT_SC_FUSE_FROM) % LT_FUSE_FRAMES == 0)
        sound_play(SFX_FUSE);
    if (lt_t == LT_SC_FUSE_TO) {
        sound_stop(SFX_FUSE);
        sound_play(SFX_LIGHTNING);
    }
    if (lt_t == LT_SC_SHATTER) {
        /* SET AS IT GOES, before anything that could fail after it. The
           scene cannot be left before this frame: no Cross, no enemies, no
           lava in this room. */
        game_flag_set(FLAG_LIGHTNING_TABLET_BROKEN);
        sound_play(SFX_RUMBLE_4);
        lt_shatter();
        lt_spawn_bolt();
    }
    if (lt_t >= LT_SC_END) lt_exit();
}

void lightning_tablet_update(int lock) {
    uint16_t btn = 0, pressed;
    if (current_area != STATE_LIGHTNINGS_EDGE) return;
    if (pad_buff_len[0]) { PadResponse *pad = (PadResponse *)pad_buff[0]; btn = ~pad->btn; }

    lt_clock++;
    lt_update_shards();

    if (lt_state == LT_IDLE) {
        int held = interact_tapped();
        int just = held && !lt_circle_prev;
        int32_t qx, qz;
        lt_circle_prev = held;
        if (lock || !just || !lt_standing()) return;
        if (!lt_front_point(&qx, &qz)) return;
        if (lt_abs(cam_x - qx) + lt_abs(cam_z - qz) >= LT_REACH) return;
        if (!interact_facing(qx, qz)) return;
        lt_start();
        lt_btn_prev = btn;              /* the Circle that opened it is not a press */
        return;
    }

    if (lt_state == LT_SCENE) {         /* a cutscene: nothing reads the pad */
        lt_btn_prev = btn;
        lt_scene_tick();
        return;
    }

    pressed     = btn & ~lt_btn_prev;
    lt_btn_prev = btn;
    /* CROSS, AT ANY TIME before the solve - the camera's flight included. */
    if (pressed & PAD_CROSS) {
        sound_play(SFX_BACK);
        lt_exit();
        return;
    }

    if (lt_state == LT_INTRO) {
        int32_t e;
        lt_t++;
        e = lt_ease(lt_t, LT_INTRO_FRAMES);
        cam_x     = lt_src_x     + (((lt_shot_x - lt_src_x) * e) >> 8);
        cam_y     = lt_src_y     + (((lt_shot_y - lt_src_y) * e) >> 8);
        cam_z     = lt_src_z     + (((lt_shot_z - lt_src_z) * e) >> 8);
        cam_rot   = lt_src_rot   + ((lt_rot_delta * e) >> 8);
        cam_pitch = lt_src_pitch - ((lt_src_pitch * e) >> 8);
        cam_vy    = 0;
        if (lt_t >= LT_INTRO_FRAMES) {
            cam_x = lt_shot_x; cam_y = lt_shot_y; cam_z = lt_shot_z;
            cam_rot = LT_YAW; cam_pitch = 0; cam_vy = 0;
            lt_t     = 0;
            lt_state = LT_BOARD;
        }
        return;
    }

    /* LT_BOARD: the cursor over the grid, Circle turns. */
    {
        int col = lt_cur % LT_COLS, row = lt_cur / LT_COLS, moved = 0;
        if ((pressed & PAD_LEFT)  && col > 0)           { col--; moved = 1; }
        if ((pressed & PAD_RIGHT) && col < LT_COLS - 1) { col++; moved = 1; }
        if ((pressed & PAD_UP)    && row > 0)           { row--; moved = 1; }
        if ((pressed & PAD_DOWN)  && row < LT_ROWS - 1) { row++; moved = 1; }
        if (moved) { lt_cur = row * LT_COLS + col; sound_play(SFX_CURSOR); }
    }
    if (pressed & PAD_SQUARE) {
        /* SQUARE turns, as on the Anzu Tablet (the user's call). A turn is a
           change to the square, not a choice: the cursor blip. */
        lt_rot[lt_cur] = (uint8_t)((lt_rot[lt_cur] + 1) & 3);
        lt_apply(lt_cur);
        sound_play(SFX_CURSOR);
        if (lt_solved()) lt_begin_scene();
    }
}

/* ---- Drawing: the slab ------------------------------------------------------- */

/* How bright Ninurta is this frame, 0..256 (the scene's glow beat). */
static int32_t lt_glow_level(void) {
    int32_t t;
    if (lt_state != LT_SCENE) return 0;
    t = lt_t - LT_SC_FUSE_TO;
    if (t <= 0) return 0;
    if (t < LT_SC_GLOW_UP) return (t * 256) / LT_SC_GLOW_UP;
    t -= LT_SC_GLOW_UP;
    if (t < LT_SC_GLOW_HOLD) return 256;
    t -= LT_SC_GLOW_HOLD;
    if (t < LT_SC_GLOW_DOWN) return 256 - (t * 256) / LT_SC_GLOW_DOWN;
    return 0;
}

/* The Wings Tablet's primitive loop, textured prims only - every face of
   this export carries one of the two tablet textures, so the untextured branches are not
   carried (code is heap here: tools/HEAP_BUDGET.txt). `glow` brightens
   Ninurta's faces toward twice their texels, warm: the carving's pale lines
   flare and its shadows stay put, which is the glow. */
static void lt_draw_slab(RenderContext *ctx, int32_t glow) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint16_t tpp = texmgr_tpage(lt_tex_p), clp = texmgr_clut(lt_tex_p);
    uint16_t tpn = texmgr_tpage(lt_tex_n), cln = texmgr_clut(lt_tex_n);

    uint8_t *p = (uint8_t *)lt_smd->p_prims;
    int pi;
    for (pi = 0; pi < lt_smd->n_prims; pi++) {
        SMD_PRI_TYPE *pt       = (SMD_PRI_TYPE *)p;
        uint8_t       stride   = pt->len;
        int           is_quad  = (pt->type >= 2);
        int           tn       = pi < LT_MAX_PRIMS && lt_ntex[pi];

        if (!pt->texture) { p += stride; continue; }

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &lt_smd->p_verts[vi[0]];
        SVECTOR *v1 = &lt_smd->p_verts[vi[1]];
        SVECTOR *v2 = &lt_smd->p_verts[vi[2]];

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
            SVECTOR *v3 = &lt_smd->p_verts[vi[3]];
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
        int32_t cr = col[0], cg = col[1], cb = col[2];
        if (glow > 0 && pi < LT_MAX_PRIMS && lt_ninurta[pi]) {
            cr += (cr * glow) >> 8;               /* up to x2   */
            cg += (cg * glow * 7) >> 11;          /* up to x1.9 */
            cb += (cb * glow * 3) >> 11;          /* up to x1.4 */
            if (cr > 255) cr = 255;
            if (cg > 255) cg = 255;
            if (cb > 255) cb = 255;
        }
        uint8_t r = (uint8_t)((cr * fog_factor + LT_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)((cg * fog_factor + LT_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)((cb * fog_factor + LT_FOG_B * (256 - fog_factor)) >> 8);

        if (is_quad) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            uint8_t *uv = p + 20;
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, r, g, b);
            poly->tpage = tn ? tpn : tpp;
            poly->clut  = tn ? cln : clp;
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
            uint8_t *uv = p + 20;
            POLY_FT3 *poly = (POLY_FT3 *)ctx->next_packet;
            setPolyFT3(poly);
            setRGB0(poly, r, g, b);
            poly->tpage = tn ? tpn : tpp;
            poly->clut  = tn ? cln : clp;
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

/* ---- Drawing: the lit lines ---------------------------------------------------
   ADDITIVE (ABR=1), as the Seal's are - but NOT through rbs_glow_quad, which
   sorts 40 slots BEHIND its own depth so a wall in front still hides it: on
   the slab's face that put every line under the stone. These sort
   LT_LINE_BIAS slots IN FRONT of the face they lie on. */

/* One additive quad; returns its slot (0 if dropped) for a spark to share. */
static int32_t lt_glow_quad(RenderContext *ctx, const SVECTOR v[4],
                            uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    DVECTOR sv[4];
    int32_t sz[4], otz;
    int k;
    if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) return 0;

    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);
    gte_stsz4c(sz);
    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);
    gte_stsz(&sz[3]);
    if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) return 0;
    for (k = 0; k < 4; k++)
        if (sv[k].vx <= -1023 || sv[k].vx >= 1023 ||
            sv[k].vy <= -1023 || sv[k].vy >= 1023) return 0;
    gte_avsz4();
    gte_stotz(&otz);
    if (otz <= 0) return 0;
    otz -= LT_LINE_BIAS;
    if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
    if (otz >= UW_SKY_OT)   otz = UW_SKY_OT - 1;

    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    POLY_F4 *p = (POLY_F4 *)ctx->next_packet;
    setPolyF4(p);
    setSemiTrans(p, 1);
    setRGB0(p, r, g, b);
    p->x0 = sv[0].vx; p->y0 = sv[0].vy;
    p->x1 = sv[1].vx; p->y1 = sv[1].vy;
    p->x2 = sv[2].vx; p->y2 = sv[2].vy;
    p->x3 = sv[3].vx; p->y3 = sv[3].vy;
    addPrim(&ot[otz], p);
    ctx->next_packet += sizeof(POLY_F4);

    /* AFTER the poly: an OT node is LIFO, so the GPU sees the blend first. */
    DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
    addPrim(&ot[otz], tp);
    ctx->next_packet += sizeof(DR_TPAGE);
    return otz;
}

/* Run i of the bolt, lit from its start to `to` (distance along it). Each
   end sits on its corner's MITRE, so neighbouring runs share an edge: sharp
   corners, no gaps, no overlap. The burning front, part way along a run, is
   cut square across it instead. */
static void lt_strip(RenderContext *ctx, int i, int32_t to, int32_t half,
                     uint8_t r, uint8_t g, uint8_t b) {
    const LtPt *A = &LT_PATH[i], *B = &LT_PATH[i + 1];
    int32_t dx = B->x - A->x, dy = B->y - A->y;
    int32_t len = B->d - A->d;
    int32_t bx, by, ax0, ay0, bx0, by0;
    SVECTOR v[4];
    int k;
    if (len <= 0) return;
    ax0 = (A->mx * half) >> 8;  ay0 = (A->my * half) >> 8;
    if (to >= len) {
        bx = B->x; by = B->y;
        bx0 = (B->mx * half) >> 8;  by0 = (B->my * half) >> 8;
    } else {
        bx = A->x + (dx * to) / len; by = A->y + (dy * to) / len;
        bx0 = (-dy * half) / len;   by0 = (dx * half) / len;
    }
    v[0].vx = (int16_t)(A->x + ax0); v[0].vy = (int16_t)(A->y + ay0);
    v[1].vx = (int16_t)(A->x - ax0); v[1].vy = (int16_t)(A->y - ay0);
    v[2].vx = (int16_t)(bx + bx0);   v[2].vy = (int16_t)(by + by0);
    v[3].vx = (int16_t)(bx - bx0);   v[3].vy = (int16_t)(by - by0);
    for (k = 0; k < 4; k++) { v[k].vz = (int16_t)(lt_min_z - LT_LINE_OUT); v[k].pad = 0; }
    lt_glow_quad(ctx, v, r, g, b);
}

static void lt_draw_lines(RenderContext *ctx) {
    int32_t s, lvl, fl, dip;
    int i;
    if (lt_state != LT_SCENE || lt_t >= LT_SC_SHATTER) return;
    s = lt_t - LT_SC_FUSE_FROM;
    if (s <= 0) return;
    s = (s >= LT_SC_FUSE_TO - LT_SC_FUSE_FROM)
        ? LT_PATH_DMAX + 1
        : (s * LT_PATH_DMAX) / (LT_SC_FUSE_TO - LT_SC_FUSE_FROM);

    for (i = 0; i + 1 < LT_PATH_N; i++) {
        const LtPt *A = &LT_PATH[i];
        int32_t len = LT_PATH[i + 1].d - A->d, to;
        uint8_t cg;
        if (s <= A->d) break;
        to = s - A->d;
        if (to > len) to = len;

        /* Per-segment flicker (the Seal's): the light dips a little, never
           the same on two neighbours. */
        fl  = (lt_clock * 7 + i * 41) & 63;
        dip = (fl < 32 ? fl : 63 - fl);
        lvl = 256 - dip;
        cg  = (uint8_t)(((200 + ((i * 13 + lt_clock) & 31)) * lvl) >> 8);

        /* A trail and nothing ahead of it: no leading glow (the user's
           call - the first cut's spark is gone). */
        lt_strip(ctx, i, to, LT_HALO_HALF,
                 (uint8_t)((150 * lvl) >> 8), (uint8_t)((100 * lvl) >> 8), 0);
        lt_strip(ctx, i, to, LT_CORE_HALF,
                 255, cg, (uint8_t)((90 * lvl) >> 8));
    }
}

/* One cube (wings_tablet.c's wt_draw_shard). */
static void lt_draw_shard(RenderContext *ctx, const LtShard *s, const MATRIX *view) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    SVECTOR  rot = { s->rx, s->ry, s->rz, 0 };
    MATRIX   m;
    SVECTOR  wv[8];
    DVECTOR  scr[8];
    int32_t  sz[8], h, dist, fog, k, f;

    h = s->half;
    if (s->life < LT_SHARD_SHRINK) h = (h * s->life) / LT_SHARD_SHRINK;
    if (h <= 0) return;

    dist = lt_abs(s->x - cam_x) + lt_abs(s->z - cam_z);
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
    lt_set_view(view);
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
        int32_t lit = ((int32_t)s->grey * LT_FACE_SHADE[f]) >> 8;
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

void lightning_tablet_draw(RenderContext *ctx) {
    MATRIX view;
    int    i;
    if (!lt_live()) return;

    camera_build_view(&view);
    lt_set_view(&view);

    if (lt_standing()) {
        int32_t dist = lt_abs(lt_cx - cam_x) + lt_abs(lt_cz - cam_z);
        if (dist - (lt_max_x - lt_min_x) / 2 <= g_fog_far) {
            lt_draw_slab(ctx, lt_glow_level());
            lt_draw_lines(ctx);
        }
    }

    for (i = 0; i < lt_shard_count; i++)
        if (lt_shards[i].life > 0) lt_draw_shard(ctx, &lt_shards[i], &view);

    /* The view back for whatever draws next (the shards leave it set, but a
       frame with none must not depend on that). */
    lt_set_view(&view);

    /* THE SIGN, in front of the middle of the face, only while it can be acted
       on: standing, not being read, the player in front. */
    if (lt_standing() && lt_state == LT_IDLE) {
        int32_t qx, qz, xz;
        int fade = 256;
        if (lt_front_point(&qx, &qz)) {
            xz = lt_abs(cam_x - qx) + lt_abs(cam_z - qz);
            if (xz < LT_TEXT_RADIUS) {
                if (xz > LT_FADE_NEAR) {
                    int range = LT_TEXT_RADIUS - LT_FADE_NEAR;
                    int prog  = xz - LT_FADE_NEAR;
                    if (prog > range) prog = range;
                    fade = 256 - ((prog * 256) / range);
                }
                door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to interact",
                                        lt_cx, lt_max_y - LT_SIGN_UP,
                                        lt_min_z - LT_SIGN_OUT,
                                        50, 255, 50, fade, LT_YAW,
                                        DOOR_PIXEL_SIZE);
            }
        }
    }
}

/* ---- Drawing: the board ------------------------------------------------------- */

#define LT_OT_HINT     1
#define LT_OT_CURSOR   2

static void lt_tile(RenderContext *ctx, int x, int y, int w, int h,
                    uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    TILE *t;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    t = (TILE *)ctx->next_packet;
    setTile(t);
    setXY0(t, x, y);
    setWH(t, w, h);
    setRGB0(t, r, g, b);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[LT_OT_CURSOR], t);
    ctx->next_packet += sizeof(TILE);
}

/* The double outline every board's reticule uses (wings_tablet.c's). */
static void lt_outline(RenderContext *ctx, int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b) {
    lt_tile(ctx, x,         y,         w, 2, r, g, b);
    lt_tile(ctx, x,         y + h - 2, w, 2, r, g, b);
    lt_tile(ctx, x,         y,         2, h, r, g, b);
    lt_tile(ctx, x + w - 2, y,         2, h, r, g, b);
}

void lightning_tablet_draw_overlay(RenderContext *ctx) {
    MATRIX  view;
    SVECTOR c[2];
    DVECTOR s[2];
    int32_t x0, y0, x1, y1;
    if (lt_state != LT_BOARD || !lt_live()) return;

    /* The square under the cursor, projected through the shot: its top-left
       and bottom-right corners on the face. */
    camera_build_view(&view);
    lt_set_view(&view);
    c[0].vx = (int16_t)(lt_gx0 + (lt_cur % LT_COLS) * lt_cell);
    c[0].vy = (int16_t)(lt_gy0 + (lt_cur / LT_COLS) * lt_cell);
    c[1].vx = (int16_t)(c[0].vx + lt_cell);
    c[1].vy = (int16_t)(c[0].vy + lt_cell);
    c[0].vz = c[1].vz = (int16_t)lt_min_z;
    c[0].pad = c[1].pad = 0;
    gte_ldv0(&c[0]); gte_rtps(); gte_stsxy(&s[0]);
    gte_ldv0(&c[1]); gte_rtps(); gte_stsxy(&s[1]);
    x0 = s[0].vx; y0 = s[0].vy; x1 = s[1].vx; y1 = s[1].vy;
    if (x1 < x0) { int32_t t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int32_t t = y0; y0 = y1; y1 = t; }

    lt_outline(ctx, x0 - 2, y0 - 2, (x1 - x0) + 4, (y1 - y0) + 4,  80,  80, 200);
    lt_outline(ctx, x0,     y0,     (x1 - x0),     (y1 - y0),     180, 180, 255);

    btn_prompt_draw(ctx, 8, 8, BTN_SQUARE " - Turn  " BTN_CROSS " - Exit",
                    LT_OT_HINT);
}
