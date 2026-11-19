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
#include "player.h"         /* game flags, the log line   */
#include "sound.h"
#include "item_pickup.h"    /* Ninurta's Wings            */
#include "door.h"           /* the "Press O to interact" sign */
#include "btn_glyph.h"
#include "golem.h"          /* the guard a wrong answer summons */
#include "underworld.h"     /* the chapter's fog */
#include "underworld_sky.h" /* UW_SKY_OT         */
#include "wings_tablet.h"

/* The Wings Tablet - see wings_tablet.h. The Gula Tablet's module (an in-place
   export measured into an axis-aligned box) with Sharur's Tablet's Chapter 4
   terms and its puzzle machinery: the sign, the shot, the shatter. */

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

static SMD  *wt_smd = NULL;
static void *wt_buf = NULL;

/* The collision box, measured off the SAME mesh the draw uses, at load. WORLD
   space, since the export is. -Y is up, so wt_min_y is the top of the slab and
   wt_max_y its base. */
static int32_t wt_min_x = 0, wt_max_x = 0;   /*  -168 .. 1032 as authored */
static int32_t wt_min_z = 0, wt_max_z = 0;   /*  2880 .. 2980             */
static int32_t wt_min_y = 0, wt_max_y = 0;   /* -1100 ..  -500            */
static int32_t wt_cx = 0, wt_cz = 0;         /* the box's centre in plan  */

static int wt_tex    = -1;   /* the slab                       */
static int wt_qa_tex = -1;   /* the question and the answers   */

/* The player's head, relative to cam_y - the figure apply_collision_* uses
   for its own body span. */
#define WT_PLAYER_HEAD 30

/* ---- THE FOG TARGET --------------------------------------------------------
   Fog here is a vertex colour lerped toward UW_FOG_*, and the GPU MULTIPLIES
   the texel by it (128 = x1). That lands a texel of 128 exactly on the fog
   colour, and the room's textures (volcanic, rocks) are almost all under 128,
   so they sink into it. "wing tablet" is bright stone: median 112, 95th
   percentile 195, max 250 - against the plain fog colour its light texels
   stayed up to 2x the fog's red when "fully fogged", so the slab never faded
   and simply appeared at its cull edge. It fades instead toward the fog
   colour scaled by 128/WT_TEX_BRIGHT, which puts the 95th-percentile texel on
   the fog colour and the median a little under it, as the room's rock is. */
#define WT_TEX_BRIGHT 195
#define WT_FOG_R (UW_FOG_R * 128 / WT_TEX_BRIGHT)
#define WT_FOG_G (UW_FOG_G * 128 / WT_TEX_BRIGHT)
#define WT_FOG_B (UW_FOG_B * 128 / WT_TEX_BRIGHT)

/* ---- The sign and the reach -------------------------------------------------
   Sharur's numbers, measured to the NEAREST POINT ON THE FRONT FACE: the slab
   is 1200 long and the player held 195 off it. The sign hovers WT_SIGN_OUT
   off the face (tools/ADDING_A_PROP.txt, "THE SIGN"), 200 above the terrace
   - the door signs' height over their floor. */
#define WT_REACH          450
#define WT_TEXT_RADIUS   1500
#define WT_FADE_NEAR     1000
#define WT_SIGN_UP        200
#define WT_SIGN_OUT       100
/* The reader faces +Z: cam_rot's forward is (sin, cos), so yaw 0. Both the
   shot's yaw and the sign's. */
#define WT_YAW              0

/* ---- The shot ---------------------------------------------------------------
   Straight out from the middle of the face, LEVEL WITH THE SLAB'S MIDDLE, at
   WT_SHOT_DIST: with gte_SetGeomScreen(256) the 1200-long slab is 293 of the
   screen's 320 at 1050 (Sharur's figure), and the 600-tall face spans screen
   y 47..193 round the centre line - so the question at the very centre of the
   screen sits on the middle of the stone, and the answers' row (y ~212) under
   its foot. The camera stands at (432, 1830): open terrace, 530 north of the
   cliff edge, nothing between it and the face. */
#define WT_SHOT_DIST      1050
#define WT_INTRO_FRAMES     30

/* ---- The board, in frames from the shot's arrival -------------------------
   The question fades in, a beat, then the four answers together, a beat, then
   the reticule and the controls. */
#define WT_Q_START           0
#define WT_A_START          60
#define WT_FADE             40
#define WT_PICK_AT         110

/* ---- The cuneiform: five lines of "wings tablet question and answers.png",
   top to bottom has wing, bird, dog, cat, snake - their texel boxes measured
   off the PNG (non-zero rows/columns). Re-measure if the art changes. */
typedef struct { uint8_t u, v, w, h; } WtLine;
static const WtLine WT_LINES[5] = {
    {  1,   5, 125, 17 },   /* 0 HAS WING - the question */
    { 35,  31,  61, 18 },   /* 1 BIRD     - the answer   */
    { 41,  56,  45, 18 },   /* 2 DOG                     */
    { 41,  83,  47, 17 },   /* 3 CAT                     */
    { 23, 107,  80, 17 },   /* 4 SNAKE                   */
};
#define WT_QUESTION     0
#define WT_RIGHT        1     /* BIRD */
#define WT_ANSWERS      4

/* Layout. Scales are PERCENT of the art's own size. The question at 160%
   (200 x 27) at the very centre; the answers at 80%, evenly spaced
   WT_SPACING apart about the screen's middle - centres 46 / 122 / 198 / 274.
   Both were 20% bigger (200% / 100%) until the user asked for them smaller.
   The spacing keeps the widest word, SNAKE (64 at 80%), inside the screen
   with its reticule round it in either end cell. The reticule hugs the word
   under it (WT_RET_PAD each side). */
#define WT_Q_SCALE        160
#define WT_A_SCALE         80
#define WT_Q_CX           (SCREEN_XRES / 2)
#define WT_Q_CY           (SCREEN_YRES / 2)
#define WT_A_CY           212
#define WT_SPACING         76
#define WT_CELL_CX(i)     (SCREEN_XRES / 2 + ((2 * (i) - (WT_ANSWERS - 1)) * WT_SPACING) / 2)
#define WT_RET_PAD          2
#define WT_RET_H           20
#define WT_FULL           128    /* vertex colour for x1 of an additive texel */

#define WT_OT_HINT           1
#define WT_OT_CURSOR         2
#define WT_OT_TEXT           4
#define WT_OT_BAND           6

/* ---- The wings ---------------------------------------------------------------
   Where the carving waits: the slab's centre, 100 over the terrace (Sharur's
   SH_ARM_Y, on this floor). Wingscape seeds no pickups (world.c), so a run-time
   spawn may take any slot. */
#define WT_ITEM_UP         100
#define WT_SEEDED_PICKUPS    0

/* ---- The wrong answer ----------------------------------------------------------
   The cliff edge behind the reader: the terrace's south edge is z=1300 across
   x[-1500,1900] (the rock face, wall 1 of the proxy, one-sided toward the
   ground, so it does not push a player standing on top). 40 in from it, on
   the slab's centre line - right on the edge, facing the tablet. */
#define WT_KICK_Z         1340

/* ---- The shards: Sharur's cubes and physics, in "wing tablet.png"'s greys. */
#define WT_SHARDS           10
#define WT_SHARD_SHRINK     20

static const uint8_t WT_GREYS[16] = {
     38,  53,  69,  83,  94, 103, 112, 120,
    131, 143, 155, 168, 181, 195, 212, 250
};
/* Per local face: -x +x -y(top) +y -z +z, out of 256 (Sharur's). */
static const uint16_t WT_FACE_SHADE[6] = { 150, 200, 256, 110, 175, 225 };

typedef struct {
    int32_t x, y, z, vx, vy, vz;
    int16_t rx, ry, rz, sx, sy, sz;
    int16_t half, life, max_life;
    uint8_t grey;
} WtShard;

static WtShard wt_shards[WT_SHARDS];
static int     wt_shard_count = 0;

/* ---- State ------------------------------------------------------------------ */
typedef enum { WT_IDLE = 0, WT_INTRO, WT_BOARD } WtState;

static WtState  wt_state = WT_IDLE;
static int32_t  wt_t     = 0;
static int      wt_cur   = 0;              /* the reticule's cell, 0..3        */
static uint8_t  wt_order[WT_ANSWERS] = { 1, 2, 3, 4 };   /* cell -> line       */
static int      wt_circle_prev = 1;
static uint16_t wt_btn_prev    = 0xFFFF;

static int32_t wt_save_x, wt_save_y, wt_save_z, wt_save_rot, wt_save_pitch, wt_save_vy;
static int32_t wt_src_x, wt_src_y, wt_src_z, wt_src_rot, wt_src_pitch, wt_rot_delta;
static int32_t wt_shot_x, wt_shot_y, wt_shot_z;

static uint32_t wt_rng = 0x3C6EF35u;
static int32_t wt_rand(int32_t n) {      /* 0..n-1 */
    wt_rng = wt_rng * 1103515245u + 12345u;
    return n > 0 ? (int32_t)((wt_rng >> 16) % (uint32_t)n) : 0;
}

static int32_t wt_abs(int32_t v) { return v < 0 ? -v : v; }
static int32_t wt_clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void *read_file(const char *name) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)name)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    void *buf = malloc(sectors * 2048);
    if (!buf) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);
    return buf;
}

/* The view, loaded without the SDK's gte_SetRotMatrix/TransMatrix, which
   clobber $t0/$t1 undeclared (sharur_tablet.c's sh_set_view, verbatim). */
static inline void wt_set_view(const MATRIX *m) {
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

/* The slab is there to be drawn, read and walked into. */
static int wt_standing(void) {
    return wt_smd && current_area == STATE_WINGSCAPE &&
           !game_flag(FLAG_WINGS_TABLET_BROKEN);
}

/* The guard is standing: the tablet is silent (the header). */
static int wt_guarded(void) {
    return golem_blocks(STATE_WINGSCAPE, GLM_BLOCKS_WINGS_TABLET);
}

/* ---- Load ---------------------------------------------------------------------- */

/* Startup. A 2.6 KB mesh held for the run, and two texture registrations
   that read nothing but a TIM header until the Underworld's bank is
   selected. */
void wings_tablet_load_assets(void) {
    /* BANK: Chapter 4. Derived, not guessed - py tools/check_tex_banks.py
       walks the uploader call graph (wingscape_upload_textures reaches this
       module) and fails if UNDERWORLD is not in the mask. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);

    wt_buf = read_file("\\TEXUNDW\\WINGTBLT.SMD;1");
    if (wt_buf) wt_smd = smdInitData(wt_buf);

    /* MEASURE THE MESH. No re-centre: the export's own coordinates are where
       it stands. */
    if (wt_smd && wt_smd->n_verts > 0) {
        int i;
        wt_min_x = wt_max_x = wt_smd->p_verts[0].vx;
        wt_min_y = wt_max_y = wt_smd->p_verts[0].vy;
        wt_min_z = wt_max_z = wt_smd->p_verts[0].vz;
        for (i = 1; i < wt_smd->n_verts; i++) {
            int32_t vx = wt_smd->p_verts[i].vx;
            int32_t vy = wt_smd->p_verts[i].vy;
            int32_t vz = wt_smd->p_verts[i].vz;
            if (vx < wt_min_x) wt_min_x = vx;
            if (vx > wt_max_x) wt_max_x = vx;
            if (vy < wt_min_y) wt_min_y = vy;
            if (vy > wt_max_y) wt_max_y = vy;
            if (vz < wt_min_z) wt_min_z = vz;
            if (vz > wt_max_z) wt_max_z = vz;
        }
    } else {
        wt_smd = NULL;
    }
    wt_cx = (wt_min_x + wt_max_x) / 2;
    wt_cz = (wt_min_z + wt_max_z) / 2;

    /* The shot: WT_SHOT_DIST straight out from the middle of the face, level
       with the middle of the slab. */
    wt_shot_x = wt_cx;
    wt_shot_y = (wt_min_y + wt_max_y) / 2;
    wt_shot_z = wt_min_z - WT_SHOT_DIST;

    wt_tex    = texmgr_register("\\TEXUNDW\\WINGTBLT.TIM;1");
    wt_qa_tex = texmgr_register("\\TEXUNDW\\WINGTBQA.TIM;1");
}

/* Room entry: pure LoadImage out of the RAM copies area_bank_sync() has
   already read. Called from wingscape_upload_textures(). BOTH, every time:
   the cuneiform's page is Sharur's cracked art's, which Sharur's Trial puts
   back on its own entry. */
void wings_tablet_upload_texture(void) {
    if (wt_tex    >= 0) texmgr_upload(wt_tex);
    if (wt_qa_tex >= 0) texmgr_upload(wt_qa_tex);
}

/* ---- Room entry ---------------------------------------------------------------- */

void wings_tablet_arm(void) {
    int i;
    if (wt_state != WT_IDLE) camera_release_player();
    wt_state       = WT_IDLE;
    wt_t           = 0;
    wt_cur         = 0;
    wt_shard_count = 0;
    wt_circle_prev = interact_tapped();

    /* A NEW ORDER ON EVERY ENTRY (the brief): Fisher-Yates over the four
       answers, the generator stirred with the vblank counter so two entries
       do not deal the same hand (the light-switch puzzle's seed). */
    wt_rng ^= (uint32_t)VSync(-1) * 2654435761u;
    for (i = 0; i < WT_ANSWERS; i++) wt_order[i] = (uint8_t)(i + 1);
    for (i = WT_ANSWERS - 1; i > 0; i--) {
        int j = wt_rand(i + 1);
        uint8_t t = wt_order[i]; wt_order[i] = wt_order[j]; wt_order[j] = t;
    }
}

static int wt_wings_live(void) {
    int i;
    for (i = 0; i < item_pickup_count; i++)
        if (item_pickups[i].active && item_pickups[i].kind == PICKUP_NINURTAS_WINGS)
            return 1;
    return 0;
}

static void wt_spawn_wings(void) {
    if (wt_wings_live()) return;
    item_pickup_spawn_from(WT_SEEDED_PICKUPS, wt_cx, wt_max_y - WT_ITEM_UP, wt_cz,
                           PICKUP_NINURTAS_WINGS, 1, 0);
}

void wings_tablet_apply_flags(void) {
    if (!wt_smd) return;
    if (game_flag(FLAG_WINGS_TABLET_BROKEN) && !game_flag(FLAG_NINURTAS_WINGS_TAKEN))
        wt_spawn_wings();
    /* The guard: planted absent by every seed, so a load needs this to stand
       it back up. Idle, as one left behind comes back (golems_rest). */
    if (game_flag(FLAG_WINGS_GUARD_UP)) {
        Golem *g = golem_guard(STATE_WINGSCAPE);
        if (g && g->state == GLM_DEAD) golem_revive(g);
    }
}

int wings_tablet_examining(void) { return wt_state != WT_IDLE; }

/* ---- The shards ------------------------------------------------------------------ */

static void wt_shatter(void) {
    int32_t hl = (wt_max_x - wt_min_x) / 2;
    int32_t ht = (wt_max_z - wt_min_z) / 2;
    int i;
    for (i = 0; i < WT_SHARDS; i++) {
        WtShard *s = &wt_shards[i];
        /* Spread evenly along the slab, jittered, at any height in it, inside
           its thickness - so it comes apart where it stood (sh_shatter). */
        int32_t a = -hl + ((2 * hl) * (2 * i + 1)) / (2 * WT_SHARDS) + wt_rand(81) - 40;
        int32_t b = wt_rand(2 * ht + 1) - ht;
        /* Thrown out of both faces in turn, and a little along the slab. */
        int32_t out   = (i & 1) ? (8 + wt_rand(12)) : -(8 + wt_rand(12));
        int32_t along = wt_rand(17) - 8;
        s->x  = wt_cx + a;
        s->z  = wt_cz + b;
        s->y  = wt_min_y + 80 + wt_rand((wt_max_y - wt_min_y) - 160 + 1);
        s->vx = along;
        s->vz = out;
        s->vy = -(10 + wt_rand(14));
        s->rx = (int16_t)wt_rand(4096);
        s->ry = (int16_t)wt_rand(4096);
        s->rz = (int16_t)wt_rand(4096);
        s->sx = (int16_t)(wt_rand(81) - 40);
        s->sy = (int16_t)(wt_rand(81) - 40);
        s->sz = (int16_t)(wt_rand(81) - 40);
        s->half     = (int16_t)(40 + wt_rand(21));
        s->life     = (int16_t)(80 + wt_rand(31));
        s->max_life = s->life;
        s->grey     = WT_GREYS[wt_rand(16)];
    }
    wt_shard_count = WT_SHARDS;
}

static void wt_update_shards(void) {
    int i;
    for (i = 0; i < wt_shard_count; i++) {
        WtShard *s = &wt_shards[i];
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
        /* The terrace under the slab: a dull bounce that bleeds off the spin. */
        if (s->y > wt_max_y - s->half) {
            s->y = wt_max_y - s->half;
            if (s->vy > 0) s->vy = -(s->vy * 3) / 10;
            if (s->vy > -3) s->vy = 0;
            s->sx = (int16_t)((s->sx * 3) / 4);
            s->sy = (int16_t)((s->sy * 3) / 4);
            s->sz = (int16_t)((s->sz * 3) / 4);
        }
        s->life--;
    }
}

/* ---- Collision ---------------------------------------------------------------- */

/* Player push-out against the measured box, Minkowski-expanded by the caller's
   radius and resolved along the shallowest axis - the Gula Tablet's scheme. */
void wings_tablet_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    if (!wt_standing()) return;

    int32_t feet = py + GROUND_FLOOR_Y, head = py - WT_PLAYER_HEAD;
    if (head >= wt_max_y || feet <= wt_min_y) return;

    int32_t min_x = wt_min_x - radius, max_x = wt_max_x + radius;
    int32_t min_z = wt_min_z - radius, max_z = wt_max_z + radius;
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

/* The point on the FRONT FACE nearest the player, or 0 if they are not in
   front of it. */
static int wt_front_point(int32_t *qx, int32_t *qz) {
    if (cam_z >= wt_min_z) return 0;
    *qx = wt_clamp(cam_x, wt_min_x, wt_max_x);
    *qz = wt_min_z;
    return 1;
}

static int32_t wt_ease(int32_t t, int32_t len) {
    int32_t lin, inv;
    /* THE ENDS BEFORE THE DIVIDE (src/descent_to_hell.c; memory: clamp before
       divide). */
    if (t <= 0)   return 0;
    if (t >= len) return 256;
    lin = (t << 8) / len;
    inv = 256 - lin;
    return 256 - ((inv * inv) >> 8);
}

static void wt_start(void) {
    int32_t d;
    wt_save_x = cam_x; wt_save_y = cam_y; wt_save_z = cam_z;
    wt_save_rot = cam_rot; wt_save_pitch = cam_pitch; wt_save_vy = cam_vy;
    camera_anchor_player(cam_x, cam_y, cam_z);

    wt_src_x = cam_x; wt_src_y = cam_y; wt_src_z = cam_z;
    wt_src_rot = cam_rot; wt_src_pitch = cam_pitch;
    d = ((WT_YAW - wt_src_rot) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    wt_rot_delta = d;

    wt_t     = 0;
    wt_cur   = 0;
    wt_state = WT_INTRO;
}

/* Out of the puzzle: control back, and the next Circle must be fresh. */
static void wt_release(void) {
    camera_release_player();
    wt_state       = WT_IDLE;
    wt_t           = 0;
    wt_circle_prev = 1;
}

/* Cross, or the right answer: back where the player stood. */
static void wt_exit(void) {
    cam_x = wt_save_x; cam_y = wt_save_y; cam_z = wt_save_z;
    cam_rot = wt_save_rot; cam_pitch = wt_save_pitch; cam_vy = wt_save_vy;
    wt_release();
}

static void wt_answer_right(void) {
    wt_exit();
    /* SET ON THE ANSWER, before anything that could fail after it. */
    game_flag_set(FLAG_WINGS_TABLET_BROKEN);
    sound_play(SFX_RUMBLE_4);
    wt_shatter();
    wt_spawn_wings();
}

static void wt_answer_wrong(void) {
    Golem *g;
    show_pickup_msg_raw("YOU ARE NOT WORTHY");
    /* NOT where they stood: on the cliff edge behind them, facing the slab. */
    cam_x = wt_cx; cam_y = wt_save_y; cam_z = WT_KICK_Z;
    cam_rot = WT_YAW; cam_pitch = 0; cam_vy = 0;
    camera_look_cancel();
    wt_release();
    /* ...and the guard, in front of the tablet, already attacking. */
    g = golem_guard(STATE_WINGSCAPE);
    if (g) {
        golem_summon(g);
        game_flag_set(FLAG_WINGS_GUARD_UP);
        sound_play(SFX_RUMBLE);
    }
}

void wings_tablet_update(int lock) {
    uint16_t btn = 0, pressed;
    if (current_area != STATE_WINGSCAPE) return;
    if (pad_buff_len[0]) { PadResponse *pad = (PadResponse *)pad_buff[0]; btn = ~pad->btn; }

    wt_update_shards();

    /* The guard's flag follows it down the frame it dies (player.h). */
    if (game_flag(FLAG_WINGS_GUARD_UP) && !wt_guarded())
        game_flag_clear(FLAG_WINGS_GUARD_UP);

    if (wt_state == WT_IDLE) {
        int held = interact_tapped();
        int just = held && !wt_circle_prev;
        int32_t qx, qz;
        wt_circle_prev = held;          /* kept current even while locked */
        if (lock || !just || !wt_standing() || wt_guarded()) return;
        if (!wt_front_point(&qx, &qz)) return;
        if (wt_abs(cam_x - qx) + wt_abs(cam_z - qz) >= WT_REACH) return;
        if (!interact_facing(qx, qz)) return;
        wt_start();
        wt_btn_prev = btn;              /* the Circle that opened it is not a press */
        return;
    }

    /* CROSS, AT ANY TIME - the camera's flight included. */
    pressed     = btn & ~wt_btn_prev;
    wt_btn_prev = btn;
    if (pressed & PAD_CROSS) {
        sound_play(SFX_BACK);
        wt_exit();
        return;
    }

    if (wt_state == WT_INTRO) {
        int32_t e;
        wt_t++;
        e = wt_ease(wt_t, WT_INTRO_FRAMES);
        cam_x     = wt_src_x     + (((wt_shot_x - wt_src_x) * e) >> 8);
        cam_y     = wt_src_y     + (((wt_shot_y - wt_src_y) * e) >> 8);
        cam_z     = wt_src_z     + (((wt_shot_z - wt_src_z) * e) >> 8);
        cam_rot   = wt_src_rot   + ((wt_rot_delta * e) >> 8);
        cam_pitch = wt_src_pitch - ((wt_src_pitch * e) >> 8);
        cam_vy    = 0;
        if (wt_t >= WT_INTRO_FRAMES) {
            cam_x = wt_shot_x; cam_y = wt_shot_y; cam_z = wt_shot_z;
            cam_rot = WT_YAW; cam_pitch = 0; cam_vy = 0;
            wt_t     = 0;
            wt_state = WT_BOARD;
        }
        return;
    }

    /* WT_BOARD: the fades run on wt_t; the reticule only once they are up. */
    if (wt_t < 0x7FFF) wt_t++;
    if (wt_t < WT_PICK_AT) return;

    if (pressed & PAD_LEFT)  { if (wt_cur > 0)              { wt_cur--; sound_play(SFX_CURSOR); } }
    if (pressed & PAD_RIGHT) { if (wt_cur < WT_ANSWERS - 1) { wt_cur++; sound_play(SFX_CURSOR); } }
    if (pressed & PAD_CIRCLE) {
        sound_play(SFX_SELECT);
        if (wt_order[wt_cur] == WT_RIGHT) wt_answer_right();
        else                              wt_answer_wrong();
    }
}

/* ---- Drawing: the slab ------------------------------------------------------- */

/* The Gula Tablet's primitive loop with no model matrix: the vertices are
   already world space, so the plain view transform is the whole of it. */
static void wt_draw_slab(RenderContext *ctx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint16_t tp = texmgr_tpage(wt_tex);
    uint16_t cl = texmgr_clut(wt_tex);

    uint8_t *p = (uint8_t *)wt_smd->p_prims;
    int pi;
    for (pi = 0; pi < wt_smd->n_prims; pi++) {
        SMD_PRI_TYPE *pt       = (SMD_PRI_TYPE *)p;
        uint8_t       stride   = pt->len;
        int           is_quad  = (pt->type >= 2);
        int           textured = pt->texture;

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &wt_smd->p_verts[vi[0]];
        SVECTOR *v1 = &wt_smd->p_verts[vi[1]];
        SVECTOR *v2 = &wt_smd->p_verts[vi[2]];

        /* PER FACE, AS THE ROOM IS (draw_ws_smd): each face is fogged and
           culled at its own centre's Manhattan distance, so the slab fades
           and drops out with the terrace and rim around it rather than as
           one block judged from its middle. */
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
            SVECTOR *v3 = &wt_smd->p_verts[vi[3]];
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
        /* TRUE DEPTH, no +40: the room's terrace sorts at its far corner plus
           40, so the slab standing on it wins the tie. In front of the sky. */
        if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
        if (otz >= UW_SKY_OT)   otz = UW_SKY_OT - 1;

        uint8_t *col = p + 16;
        /* Untextured faces show the vertex colour as-is, so they take the
           true fog colour; textured ones the dimmed target above. */
        int32_t fr = textured ? WT_FOG_R : UW_FOG_R;
        int32_t fg = textured ? WT_FOG_G : UW_FOG_G;
        int32_t fb = textured ? WT_FOG_B : UW_FOG_B;
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + fr * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + fg * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + fb * (256 - fog_factor)) >> 8);

        if (is_quad && textured) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            uint8_t *uv = p + 20;
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
        } else if (is_quad) {
            if (ctx->next_packet + sizeof(POLY_F4) > buf_end) { p += stride; continue; }
            POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
            setPolyF4(poly);
            setRGB0(poly, r, g, b);
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F4);
        } else if (textured) {
            if (ctx->next_packet + sizeof(POLY_FT3) > buf_end) { p += stride; continue; }
            uint8_t *uv = p + 20;
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
        } else {
            if (ctx->next_packet + sizeof(POLY_F3) > buf_end) { p += stride; continue; }
            POLY_F3 *poly = (POLY_F3 *)ctx->next_packet;
            setPolyF3(poly);
            setRGB0(poly, r, g, b);
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F3);
        }

        p += stride;
    }
}

/* One cube - sh_draw_shard verbatim: corners rotated on the CPU and projected
   through the plain view, no backface cull. */
static void wt_draw_shard(RenderContext *ctx, const WtShard *s, const MATRIX *view) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    SVECTOR  rot = { s->rx, s->ry, s->rz, 0 };
    MATRIX   m;
    SVECTOR  wv[8];
    DVECTOR  scr[8];
    int32_t  sz[8], h, dist, fog, k, f;

    h = s->half;
    if (s->life < WT_SHARD_SHRINK) h = (h * s->life) / WT_SHARD_SHRINK;
    if (h <= 0) return;

    dist = wt_abs(s->x - cam_x) + wt_abs(s->z - cam_z);
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
    wt_set_view(view);
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
        int32_t lit = ((int32_t)s->grey * WT_FACE_SHADE[f]) >> 8;
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

void wings_tablet_draw(RenderContext *ctx) {
    MATRIX view;
    int    i;
    if (current_area != STATE_WINGSCAPE || !wt_smd) return;

    camera_build_view(&view);
    wt_set_view(&view);

    if (wt_standing()) {
        /* Culled at fog-far from the box's CENTRE, less its half-length so an
           end still in view is not lost with the middle (Sharur's rule); each
           face is then fogged and culled on its own. */
        int32_t dist = wt_abs(wt_cx - cam_x) + wt_abs(wt_cz - cam_z);
        if (dist - (wt_max_x - wt_min_x) / 2 <= g_fog_far)
            wt_draw_slab(ctx);
    }

    for (i = 0; i < wt_shard_count; i++)
        if (wt_shards[i].life > 0) wt_draw_shard(ctx, &wt_shards[i], &view);

    /* THE SIGN, in front of the middle of the face - only while it can be
       acted on: standing, unguarded, not being read, the player in front. */
    if (wt_standing() && wt_state == WT_IDLE && !wt_guarded()) {
        int32_t qx, qz, xz;
        int fade = 256;
        if (wt_front_point(&qx, &qz)) {
            xz = wt_abs(cam_x - qx) + wt_abs(cam_z - qz);
            if (xz < WT_TEXT_RADIUS) {
                if (xz > WT_FADE_NEAR) {
                    int range = WT_TEXT_RADIUS - WT_FADE_NEAR;
                    int prog  = xz - WT_FADE_NEAR;
                    if (prog > range) prog = range;
                    fade = 256 - ((prog * 256) / range);
                }
                door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to interact",
                                        wt_cx, wt_max_y - WT_SIGN_UP,
                                        wt_min_z - WT_SIGN_OUT,
                                        50, 255, 50, fade, WT_YAW,
                                        DOOR_PIXEL_SIZE);
            }
        }
    }
}

/* ---- Drawing: the board ------------------------------------------------------- */

/* A DR_TPAGE carrying blend mode `abr` into `slot`, AFTER the semi-trans prims
   it governs: an OT node is LIFO, so the GPU meets the mode first. */
static void wt_blend_tpage(RenderContext *ctx, int slot, int abr) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    DR_TPAGE *dp;
    if (ctx->next_packet + sizeof(DR_TPAGE) > buf_end) return;
    dp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(dp, 0, 0, getTPage(0, abr, 0, 0));
    addPrim(&ctx->buffers[ctx->active_buffer].ot[slot], dp);
    ctx->next_packet += sizeof(DR_TPAGE);
}

static void wt_tile(RenderContext *ctx, int x, int y, int w, int h,
                    uint8_t r, uint8_t g, uint8_t b, int semi, int slot) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    TILE *t;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    t = (TILE *)ctx->next_packet;
    setTile(t);
    if (semi) setSemiTrans(t, 1);
    setXY0(t, x, y);
    setWH(t, w, h);
    setRGB0(t, r, g, b);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[slot], t);
    ctx->next_packet += sizeof(TILE);
}

/* The double outline every board's reticule uses (xd_cursor). */
static void wt_outline(RenderContext *ctx, int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b) {
    wt_tile(ctx, x,         y,         w, 2, r, g, b, 0, WT_OT_CURSOR);
    wt_tile(ctx, x,         y + h - 2, w, 2, r, g, b, 0, WT_OT_CURSOR);
    wt_tile(ctx, x,         y,         2, h, r, g, b, 0, WT_OT_CURSOR);
    wt_tile(ctx, x + w - 2, y,         2, h, r, g, b, 0, WT_OT_CURSOR);
}
static void wt_cursor(RenderContext *ctx, int x, int y, int w, int h) {
    wt_outline(ctx, x - 4, y - 4, w + 8, h + 8,  80,  80, 200);
    wt_outline(ctx, x - 2, y - 2, w + 4, h + 4, 180, 180, 255);
}

/* One line of cuneiform, centred on (cx, cy) at `scale` PERCENT, ADDITIVE at `level`
   (0..WT_FULL): the texture carries STP, and a textured prim takes its blend
   mode from its OWN tpage, so ABR 1 (B + F) goes in that. */
static void wt_glyph(RenderContext *ctx, int line, int cx, int cy, int scale,
                     int level) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    const WtLine *L = &WT_LINES[line];
    int w = (L->w * scale) / 100, h = (L->h * scale) / 100;   /* percent */
    int x0 = cx - w / 2, y0 = cy - h / 2;
    POLY_FT4 *poly;
    if (level <= 0 || wt_qa_tex < 0) return;
    if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;
    poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setSemiTrans(poly, 1);
    setRGB0(poly, (uint8_t)level, (uint8_t)level, (uint8_t)level);
    poly->tpage = (uint16_t)((texmgr_tpage(wt_qa_tex) & ~(3 << 5)) | (1 << 5));
    poly->clut  = texmgr_clut(wt_qa_tex);
    poly->u0 = L->u;        poly->v0 = L->v;
    poly->u1 = L->u + L->w; poly->v1 = L->v;
    poly->u2 = L->u;        poly->v2 = L->v + L->h;
    poly->u3 = L->u + L->w; poly->v3 = L->v + L->h;
    poly->x0 = x0;     poly->y0 = y0;
    poly->x1 = x0 + w; poly->y1 = y0;
    poly->x2 = x0;     poly->y2 = y0 + h;
    poly->x3 = x0 + w; poly->y3 = y0 + h;
    addPrim(&ctx->buffers[ctx->active_buffer].ot[WT_OT_TEXT], poly);
    ctx->next_packet += sizeof(POLY_FT4);
}

/* 0..WT_FULL: how far a fade that starts at `start` has come. */
static int wt_level(int32_t start) {
    int32_t since = wt_t - start;
    if (since <= 0)      return 0;
    if (since >= WT_FADE) return WT_FULL;
    return (since * WT_FULL) / WT_FADE;
}

void wings_tablet_draw_overlay(RenderContext *ctx) {
    int qlv, alv, i;
    if (wt_state != WT_BOARD || current_area != STATE_WINGSCAPE) return;

    qlv = wt_level(WT_Q_START);
    alv = wt_level(WT_A_START);

    /* THE BANDS: SUBTRACTIVE strips behind the question and the answers, so
       the pale cuneiform stays legible over the bright stone and the fog
       (Sharur's band). Each fades in with its own text. */
    if (qlv > 0) {
        int band = (qlv * 96) / WT_FULL;
        int h    = (WT_LINES[WT_QUESTION].h * WT_Q_SCALE) / 100 + 14;
        wt_tile(ctx, 0, WT_Q_CY - h / 2, SCREEN_XRES, h,
                (uint8_t)band, (uint8_t)band, (uint8_t)band, 1, WT_OT_BAND);
    }
    if (alv > 0) {
        int band = (alv * 96) / WT_FULL;
        wt_tile(ctx, 0, WT_A_CY - (WT_RET_H / 2) - 6, SCREEN_XRES, WT_RET_H + 12,
                (uint8_t)band, (uint8_t)band, (uint8_t)band, 1, WT_OT_BAND);
    }
    wt_blend_tpage(ctx, WT_OT_BAND, 2);     /* B - F */

    /* THE QUESTION at the very centre, then THE ANSWERS in their cells. */
    wt_glyph(ctx, WT_QUESTION, WT_Q_CX, WT_Q_CY, WT_Q_SCALE, qlv);
    for (i = 0; i < WT_ANSWERS; i++)
        wt_glyph(ctx, wt_order[i], WT_CELL_CX(i), WT_A_CY, WT_A_SCALE, alv);

    /* THE RETICULE and the controls, once everything is up. */
    if (wt_t >= WT_PICK_AT) {
        int cx = WT_CELL_CX(wt_cur);
        int w  = (WT_LINES[wt_order[wt_cur]].w * WT_A_SCALE) / 100 + 2 * WT_RET_PAD;
        wt_cursor(ctx, cx - w / 2, WT_A_CY - WT_RET_H / 2, w, WT_RET_H);
        btn_prompt_draw(ctx, 8, 8, BTN_CIRCLE " - Choose  " BTN_CROSS " - Exit",
                        WT_OT_HINT);
    }
}
