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
#include "title.h"          /* current_area, STATE_SHARURS_TRIAL */
#include "player.h"         /* game flags                        */
#include "sound.h"
#include "item_pickup.h"    /* Ninurta's Arm                     */
#include "door.h"           /* the "Press O to examine" sign     */
#include "btn_glyph.h"
#include "intro.h"          /* the 5x7 fade-able font            */
#include "underworld.h"     /* the chapter's fog                 */
#include "underworld_sky.h" /* UW_SKY_OT                         */
#include "sharur_tablet.h"

/* Sharur's Tablet - see sharur_tablet.h for the puzzle and why the box is
   measured off the mesh. */

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- The mesh, and what is measured off it ---------------------------------
   48 quads as exported; the cap only bounds the front-face table. */
#define SH_MAX_PRIMS   64

static SMD  *sh_smd = NULL;
static void *sh_buf = NULL;
static uint8_t sh_front[SH_MAX_PRIMS];   /* 1 = an inscription face: takes the
                                            cracked art from the 4th blow     */

/* The slab's own frame, all WORLD space. n is the front face's outward unit
   normal (4096 = 1.0), t runs along the slab. As exported: n = (-0.739,
   -0.673), centre (-1676,-1232), half-length 600, half-thickness 50, y
   [-600, 0]. */
static int32_t sh_nx = -3028, sh_nz = -2758;
static int32_t sh_tx =  2758, sh_tz = -3028;
static int32_t sh_cx = 0, sh_cz = 0;
static int32_t sh_hl = 0, sh_ht = 0;
static int32_t sh_ymin = 0, sh_ymax = 0;   /* -Y is up: ymin is the top */

static int sh_tex_plain   = -1;
static int sh_tex_cracked = -1;

/* ---- Tuning -------------------------------------------------------------- */
/* THE FACE'S DIRECTION, as a hint the load refines. Any face whose outward
   normal is within 45 degrees of (-1, 0, -1) is a front face; the end faces
   are ~88 degrees off it and the top ~90, so the margin is wide both ways. */

/* The Circle reach and the sign's fade, measured from the player to the
   NEAREST POINT ON THE FRONT FACE (not its centre: the slab is 1200 long and
   the inscription runs the whole of it). The player is held SH_COLLIDE_RADIUS
   (195) off the stone, so the reach leaves room to stand back from it.

   THE SIGN HOVERS SH_SIGN_OUT IN FRONT OF THE FACE. At 20 it sat all but on
   the carving and read as part of it: green letters lying flat on a busy
   texture, at the stone's own depth. 100 is clear of the slab (2x its
   half-thickness) and still well inside the 195 the player is held at, so
   they can never walk into it. tools/ADDING_A_PROP.txt, "THE SIGN". */
#define SH_REACH          450
#define SH_TEXT_RADIUS   1500
#define SH_FADE_NEAR     1000
#define SH_SIGN_Y        (-200)    /* the door signs' height in this room       */
#define SH_SIGN_OUT       100      /* off the face, toward the reader           */

/* The yaw that LOOKS AT the face: cam_rot's convention is forward = (sin,
   cos) of it, and the face's normal is (-0.739, -0.673), so the reader looks
   along (+0.739, +0.673): atan2(0.739, 0.673) = 47.7 deg = 542 in 4096ths.
   It is both the shot's yaw and the sign's (door_draw_string_3d_yaw takes
   the direction the reader faces). A constant, unlike the rest of the frame,
   because the SDK's atan is not one this codebase has trusted yet: re-derive
   it if the slab is ever turned. */
#define SH_YAW             542

/* ---- The shot ---------------------------------------------------------------
   Straight out from the middle of the face, level, at SH_SHOT_DIST. With
   gte_SetGeomScreen(256) a span S at distance D is S * 256 / D pixels, so the
   1200-long slab is 293 of the screen's 320 at 1050 - all of it, with a
   margin. The eye sits at SH_SHOT_EYE_Y: screen y = 120 + (wy - eye) * 256 /
   1050, which puts the slab's top (-600) at y 30 and its foot (0) at y 176,
   and leaves y 186..221 under it for the three lines of the inscription.
   Measured (as exported) the camera stands at (-2489, -1973): volcanic floor
   all the way to the slab, nothing standing between, inside the rim.

   Fog: the chapter's, which nothing may change (src/underworld.h). The slab is
   ~1550 Manhattan from the eye, so it reads about 70% through the fog - the
   same as it does from that spot on foot. */
#define SH_SHOT_DIST      1050
#define SH_SHOT_EYE_Y     (-231)
#define SH_INTRO_FRAMES     30

/* ---- The inscription ------------------------------------------------------- */
#define SH_LINE_COUNT        3
#define SH_LINE_Y0         186
#define SH_LINE_SPACING     14
#define SH_LETTER_GAP        3      /* frames between one letter and the next */
#define SH_LETTER_FADE      14      /* each letter's own fade, 0 -> full      */
#define SH_BAND_FADE        16      /* the dark band under the text           */
#define SH_TEXT_R          235      /* the intro's bone white                 */
#define SH_TEXT_G          230
#define SH_TEXT_B          215

#define SH_OT_HINT           1
#define SH_OT_TEXT           4
#define SH_OT_BAND           6

static const char *const SH_LINES[SH_LINE_COUNT] = {
    "AND WITH ALL THE MIGHT OF THE GODS HE STRUCK",
    "ONE TIME ON THE WING, ONE TIME ON THE REAR,",
    "AND THREE BLOWS TO THE HEAD",
};

/* ---- The blows --------------------------------------------------------------
   Reach to the box's SURFACE in the slab's frame, the crib's rule
   (src/crib.c, cribs_try_hit): the player is held 195 off a 1200-long face, so
   a test to the centre would miss from anywhere near the ends. */
#define SH_HIT_REACH       300
#define SH_CRACK_AT          4
#define SH_BREAK_AT          5

/* Where the carving waits: the slab's centre, sprite ~150 off the floor.
   item_pickup_spawn takes IP_FLOAT_Y (50) back off. Sharur's Trial seeds no
   pickups (world.c), so a run-time spawn may take any slot; were it ever to
   seed some, this must become that count (item_pickup_spawn_from). */
#define SH_ARM_Y          (-100)
#define SH_SEEDED_PICKUPS    0

/* ---- The shards --------------------------------------------------------------
   A FEW LARGE CUBES, real 3D ones, tumbling: the burst pool's TILEs are flat
   squares, and the brief was cubes. Their physics is the burst pool's exactly
   (update_particles in src/particles.c: +3 gravity a frame, sideways speed
   damped by 28/32), so they move like every other debris in the game; the one
   thing added is the floor, which a cube that sits up off the ground needs and
   a 2-pixel tile does not. Each is one grey out of the tablet texture's own
   palette, shaded per face so the tumble reads. */
#define SH_SHARDS           10
#define SH_SHARD_SHRINK     20      /* last frames of a life: it shrinks away */

static const uint8_t SH_GREYS[16] = {   /* "sharur tablet.png"'s palette */
     52,  65,  77,  87,  94, 103, 112, 120,
    129, 141, 150, 160, 168, 176, 184, 197
};
/* Per local face: -x +x -y(top) +y -z +z, out of 256. */
static const uint16_t SH_FACE_SHADE[6] = { 150, 200, 256, 110, 175, 225 };

typedef struct {
    int32_t x, y, z, vx, vy, vz;
    int16_t rx, ry, rz, sx, sy, sz;    /* angle and spin, 4096 = a turn       */
    int16_t half, life, max_life;
    uint8_t grey;
} Shard;

static Shard sh_shards[SH_SHARDS];
static int   sh_shard_count = 0;

/* ---- State ------------------------------------------------------------------ */
typedef enum { SH_IDLE = 0, SH_INTRO, SH_READ } ShState;

static ShState  sh_state  = SH_IDLE;
static int      sh_hits   = 0;        /* this visit's blows                   */
static int32_t  sh_t      = 0;        /* frames into the intro / the reading  */
static int      sh_circle_prev = 1;
static uint16_t sh_btn_prev    = 0xFFFF;

static int32_t sh_save_x, sh_save_y, sh_save_z, sh_save_rot, sh_save_pitch, sh_save_vy;
static int32_t sh_src_x, sh_src_y, sh_src_z, sh_src_rot, sh_src_pitch, sh_rot_delta;
static int32_t sh_shot_x, sh_shot_z;   /* derived from the frame at load */

static uint32_t sh_rng = 0x5A4B2u;
static int32_t sh_rand(int32_t n) {      /* 0..n-1 */
    sh_rng = sh_rng * 1103515245u + 12345u;
    return n > 0 ? (int32_t)((sh_rng >> 16) % (uint32_t)n) : 0;
}

/* ---- Helpers ---------------------------------------------------------------- */

static int32_t sh_abs(int32_t v) { return v < 0 ? -v : v; }

static int32_t isqrt32(uint32_t v) {
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else            { r >>= 1; }
        b >>= 2;
    }
    return (int32_t)r;
}

/* World plan point -> the slab's (along, out-of-face) and back. */
static void sh_local(int32_t x, int32_t z, int32_t *a, int32_t *b) {
    int32_t dx = x - sh_cx, dz = z - sh_cz;
    *a = (dx * sh_tx + dz * sh_tz) >> 12;
    *b = (dx * sh_nx + dz * sh_nz) >> 12;
}
static void sh_world(int32_t a, int32_t b, int32_t *x, int32_t *z) {
    *x = sh_cx + ((a * sh_tx + b * sh_nx) >> 12);
    *z = sh_cz + ((a * sh_tz + b * sh_nz) >> 12);
}

static int32_t sh_clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The slab is there to be drawn, hit, read and walked into. */
static int sh_standing(void) {
    return sh_smd && current_area == STATE_SHARURS_TRIAL &&
           !game_flag(FLAG_SHARUR_TABLET_BROKEN);
}

/* The arm-switch's GTE load (src/arm_switch.c, as_set_view): the SDK's
   gte_SetRotMatrix / gte_SetTransMatrix clobber $t0/$t1 without saying so. */
static inline void sh_set_view(const MATRIX *m) {
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

/* ---- Load: read the slab and measure it ------------------------------------- */

static void sh_measure(void) {
    int64_t  sx = 0, sz = 0;
    uint8_t *p;
    int      pi, n, i;
    SVECTOR *o;
    int32_t  amin = 0, amax = 0, bmin = 0, bmax = 0, am, bm, len;

    if (!sh_smd || sh_smd->n_verts <= 0) { sh_smd = NULL; return; }

    /* PASS 1: THE FRONT FACES, and the front normal as their sum.
       >>> OUTWARD IS (v2 - v0) x (v1 - v0). <<< Checked against the export's
       own per-face normals: (v1 - v0) x (v2 - v0) points INTO the slab on all
       48 faces, and smxlink keeps the .smx's vertex order. Edges are under
       1300, so every product fits an int32; the squares do not, so those are
       64-bit. */
    n = sh_smd->n_prims;
    if (n > SH_MAX_PRIMS) n = SH_MAX_PRIMS;
    p = (uint8_t *)sh_smd->p_prims;
    for (pi = 0; pi < n; pi++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &sh_smd->p_verts[vi[0]];
        SVECTOR *v1 = &sh_smd->p_verts[vi[1]];
        SVECTOR *v2 = &sh_smd->p_verts[vi[2]];
        int32_t ux = v1->vx - v0->vx, uy = v1->vy - v0->vy, uz = v1->vz - v0->vz;
        int32_t wx = v2->vx - v0->vx, wy = v2->vy - v0->vy, wz = v2->vz - v0->vz;
        int32_t nx = wy * uz - wz * uy;
        int32_t ny = wz * ux - wx * uz;
        int32_t nz = wx * uy - wy * ux;
        /* Within 45 degrees of (-1,0,-1)/sqrt2:  h = -(nx + nz) = sqrt2 |n| cos,
           so cos > 1/sqrt2  <=>  h > |n|  <=>  h > 0 and h^2 > |n|^2. */
        int32_t h = -(nx + nz);
        sh_front[pi] = 0;
        if (h > 0) {
            int64_t hh = (int64_t)h * h;
            int64_t nn = (int64_t)nx * nx + (int64_t)ny * ny + (int64_t)nz * nz;
            if (hh > nn) { sh_front[pi] = 1; sx += nx; sz += nz; }
        }
        p += pt->len;
    }
    /* Down to 15 bits before anything divides: the SDK links no libgcc, so a
       64-bit divide is an undefined __divdi3. Shifts, and 32-bit from here. */
    while (sx > 32767 || sx < -32767 || sz > 32767 || sz < -32767) {
        sx >>= 1; sz >>= 1;
    }
    {
        int32_t ix = (int32_t)sx, iz = (int32_t)sz;
        len = isqrt32((uint32_t)(ix * ix + iz * iz));
        if (len > 0) {
            sh_nx = (ix * 4096) / len;
            sh_nz = (iz * 4096) / len;
        }
    }
    sh_tx = -sh_nz;
    sh_tz =  sh_nx;

    /* PASS 2: THE EXTENTS in that frame, about vertex 0, then the centre. */
    o = &sh_smd->p_verts[0];
    sh_ymin = sh_ymax = o->vy;
    for (i = 0; i < sh_smd->n_verts; i++) {
        SVECTOR *v = &sh_smd->p_verts[i];
        int32_t dx = v->vx - o->vx, dz = v->vz - o->vz;
        int32_t a  = (dx * sh_tx + dz * sh_tz) >> 12;
        int32_t b  = (dx * sh_nx + dz * sh_nz) >> 12;
        if (a < amin) amin = a;
        if (a > amax) amax = a;
        if (b < bmin) bmin = b;
        if (b > bmax) bmax = b;
        if (v->vy < sh_ymin) sh_ymin = v->vy;
        if (v->vy > sh_ymax) sh_ymax = v->vy;
    }
    sh_hl = (amax - amin) / 2;
    sh_ht = (bmax - bmin) / 2;
    am = (amin + amax) / 2;
    bm = (bmin + bmax) / 2;
    sh_cx = o->vx + ((am * sh_tx + bm * sh_nx) >> 12);
    sh_cz = o->vz + ((am * sh_tz + bm * sh_nz) >> 12);

    /* The shot: SH_SHOT_DIST straight out from the middle of the face. */
    sh_world(0, sh_ht + SH_SHOT_DIST, &sh_shot_x, &sh_shot_z);
}

void sharur_tablet_load_assets(void) {
    /* BANK: Chapter 4. Derived, not guessed - py tools/check_tex_banks.py walks
       the uploader call graph (sharurs_trial_upload_textures reaches this
       module) and fails if UNDERWORLD is not in the mask. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);

    sh_buf = read_file("\\TEXUNDW\\SHARTBLT.SMD;1");
    if (sh_buf) sh_smd = smdInitData(sh_buf);
    sh_measure();

    sh_tex_plain   = texmgr_register("\\TEXUNDW\\SHARTBLT.TIM;1");
    sh_tex_cracked = texmgr_register("\\TEXUNDW\\SHARTBCR.TIM;1");
}

/* Room entry: pure LoadImage out of the RAM copies the room's set holds. BOTH,
   every time, so the crack on the 4th blow is a tpage swap in the draw and
   never a LoadImage mid-room. */
void sharur_tablet_upload_textures(void) {
    if (sh_tex_plain   >= 0) texmgr_upload(sh_tex_plain);
    if (sh_tex_cracked >= 0) texmgr_upload(sh_tex_cracked);
}

/* ---- Room entry ---------------------------------------------------------------- */

void sharur_tablet_arm(void) {
    if (sh_state != SH_IDLE) camera_release_player();
    sh_state       = SH_IDLE;
    sh_hits        = 0;            /* per visit - see the header */
    sh_t           = 0;
    sh_shard_count = 0;
    sh_circle_prev = interact_tapped();
}

static int sh_arm_live(void) {
    int i;
    for (i = 0; i < item_pickup_count; i++)
        if (item_pickups[i].active && item_pickups[i].kind == PICKUP_NINURTAS_ARM)
            return 1;
    return 0;
}

static void sh_spawn_arm(void) {
    if (sh_arm_live()) return;
    item_pickup_spawn_from(SH_SEEDED_PICKUPS, sh_cx, SH_ARM_Y, sh_cz,
                           PICKUP_NINURTAS_ARM, 1, 0);
}

void sharur_tablet_apply_flags(void) {
    if (!sh_smd) return;
    if (!game_flag(FLAG_SHARUR_TABLET_BROKEN)) return;
    if (game_flag(FLAG_NINURTAS_ARM_TAKEN))    return;
    sh_spawn_arm();
}

int sharur_tablet_examining(void) { return sh_state != SH_IDLE; }

/* ---- The shards ------------------------------------------------------------------ */

static void sh_shatter(void) {
    int i;
    for (i = 0; i < SH_SHARDS; i++) {
        Shard  *s = &sh_shards[i];
        /* Spread evenly along the slab, jittered, at any height in it, inside
           its thickness - so it comes apart where it stood, not from a point. */
        int32_t a = -sh_hl + ((2 * sh_hl) * (2 * i + 1)) / (2 * SH_SHARDS)
                    + sh_rand(81) - 40;
        int32_t b = sh_rand(2 * sh_ht + 1) - sh_ht;
        /* Thrown out of both faces in turn, and a little along the slab. */
        int32_t out   = (i & 1) ? (8 + sh_rand(12)) : -(8 + sh_rand(12));
        int32_t along = sh_rand(17) - 8;
        sh_world(a, b, &s->x, &s->z);
        s->y  = sh_ymin + 80 + sh_rand((sh_ymax - sh_ymin) - 160 + 1);
        s->vx = (along * sh_tx + out * sh_nx) >> 12;
        s->vz = (along * sh_tz + out * sh_nz) >> 12;
        s->vy = -(10 + sh_rand(14));
        s->rx = (int16_t)sh_rand(4096);
        s->ry = (int16_t)sh_rand(4096);
        s->rz = (int16_t)sh_rand(4096);
        s->sx = (int16_t)(sh_rand(81) - 40);
        s->sy = (int16_t)(sh_rand(81) - 40);
        s->sz = (int16_t)(sh_rand(81) - 40);
        s->half     = (int16_t)(40 + sh_rand(21));   /* 80..120 across: large */
        s->life     = (int16_t)(80 + sh_rand(31));
        s->max_life = s->life;
        s->grey     = SH_GREYS[sh_rand(16)];
    }
    sh_shard_count = SH_SHARDS;
}

static void sh_update_shards(void) {
    int i;
    for (i = 0; i < sh_shard_count; i++) {
        Shard *s = &sh_shards[i];
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
        /* The floor (y 0 here): a dull bounce that bleeds off the spin. */
        if (s->y > -s->half) {
            s->y = -s->half;
            if (s->vy > 0) s->vy = -(s->vy * 3) / 10;
            if (s->vy > -3) s->vy = 0;
            s->sx = (int16_t)((s->sx * 3) / 4);
            s->sy = (int16_t)((s->sy * 3) / 4);
            s->sz = (int16_t)((s->sz * 3) / 4);
        }
        s->life--;
    }
}

/* ---- The blows ---------------------------------------------------------------------- */

static void sh_blow(void) {
    sh_hits++;
    if (sh_hits < SH_CRACK_AT) {
        sound_play(SFX_AXEHIT);
    } else if (sh_hits == SH_CRACK_AT) {
        sound_play(SFX_RUMBLE);            /* ...and the front goes cracked */
    } else if (sh_hits >= SH_BREAK_AT) {
        sound_play(SFX_RUMBLE_4);          /* its own voice: see sound.c    */
        /* SET ON THE BLOW, before anything that could fail after it. */
        game_flag_set(FLAG_SHARUR_TABLET_BROKEN);
        sh_shatter();
        sh_spawn_arm();
    }
}

int sharur_tablet_try_hit(void) {
    int32_t a, b, ca, cb, ny, dist, fx, fz, dot;
    if (!sh_standing() || sh_state != SH_IDLE) return 0;

    /* Reach to the SURFACE: clamp the player into the box in its own frame
       and measure what is left over, plan and height. */
    sh_local(cam_x, cam_z, &a, &b);
    ca = sh_clamp(a, -sh_hl, sh_hl);
    cb = sh_clamp(b, -sh_ht, sh_ht);
    ny = sh_clamp(cam_y, sh_ymin, sh_ymax);
    dist = sh_abs(a - ca) + sh_abs(b - cb) + sh_abs(cam_y - ny);
    if (dist >= SH_HIT_REACH) return 0;

    /* Facing, to the point on the slab's CENTRE LINE abreast of the player:
       never nearer than the half-thickness plus the 195 standoff, so the dot
       always means something, and it follows the player along a long face
       where the slab's middle would not. */
    sh_world(ca, 0, &fx, &fz);
    fx -= cam_x; fz -= cam_z;
    dot = (fx * isin(cam_rot) + fz * icos(cam_rot)) >> 12;
    if (dot <= 0) return 0;

    sh_blow();
    return 1;
}

/* ---- Collision ---------------------------------------------------------------------- */

#define SH_PLAYER_HEAD 30   /* the crib's and the Gula Tablet's figure */

void sharur_tablet_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int32_t a, b, ea, eb, best, da = 0, db = 0, x, z;
    int32_t feet = py + GROUND_FLOOR_Y, head = py - SH_PLAYER_HEAD;
    if (!sh_standing()) return;
    if (head >= sh_ymax || feet <= sh_ymin) return;

    sh_local(*px, *pz, &a, &b);
    ea = sh_hl + radius;
    eb = sh_ht + radius;
    if (a <= -ea || a >= ea || b <= -eb || b >= eb) return;

    /* Out along the shallowest of the four, in the slab's frame, +1 so the
       rounding back to world cannot leave the player a unit inside. */
    best = ea - a;            da =  best + 1; db = 0;
    if (a + ea < best) { best = a + ea; da = -(best + 1); db = 0; }
    if (eb - b < best) { best = eb - b; da = 0; db =  best + 1; }
    if (b + eb < best) { best = b + eb; da = 0; db = -(best + 1); }

    sh_world(a + da, b + db, &x, &z);
    *px = x;
    *pz = z;
}

/* ---- Per frame ---------------------------------------------------------------------- */

/* The point on the FRONT FACE nearest the player, or 0 if they are not in
   front of it (behind, or level with it at an end). */
static int sh_front_point(int32_t *qx, int32_t *qz) {
    int32_t a, b;
    sh_local(cam_x, cam_z, &a, &b);
    if (b <= sh_ht) return 0;
    sh_world(sh_clamp(a, -sh_hl, sh_hl), sh_ht, qx, qz);
    return 1;
}

static int32_t sh_ease(int32_t t, int32_t len) {
    int32_t lin, inv;
    /* THE ENDS BEFORE THE DIVIDE - a clamped quotient compiled to back-to-back
       MULTs once and Redux read garbage out of LO (src/descent_to_hell.c). */
    if (t <= 0)   return 0;
    if (t >= len) return 256;
    lin = (t << 8) / len;
    inv = 256 - lin;
    return 256 - ((inv * inv) >> 8);
}

static void sh_start(void) {
    int32_t d;
    sh_save_x = cam_x; sh_save_y = cam_y; sh_save_z = cam_z;
    sh_save_rot = cam_rot; sh_save_pitch = cam_pitch; sh_save_vy = cam_vy;
    camera_anchor_player(cam_x, cam_y, cam_z);

    sh_src_x = cam_x; sh_src_y = cam_y; sh_src_z = cam_z;
    sh_src_rot = cam_rot; sh_src_pitch = cam_pitch;
    d = ((SH_YAW - sh_src_rot) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    sh_rot_delta = d;

    sh_t     = 0;
    sh_state = SH_INTRO;
}

static void sh_exit(void) {
    cam_x = sh_save_x; cam_y = sh_save_y; cam_z = sh_save_z;
    cam_rot = sh_save_rot; cam_pitch = sh_save_pitch; cam_vy = sh_save_vy;
    camera_release_player();
    sh_state       = SH_IDLE;
    sh_t           = 0;
    sh_circle_prev = 1;     /* the next Circle must be a fresh press */
}

void sharur_tablet_update(int lock) {
    uint16_t btn = 0;
    if (pad_buff_len[0]) { PadResponse *pad = (PadResponse *)pad_buff[0]; btn = ~pad->btn; }

    sh_update_shards();

    if (sh_state == SH_IDLE) {
        int held = interact_tapped();
        int just = held && !sh_circle_prev;
        int32_t qx, qz;
        sh_circle_prev = held;          /* kept current even while locked */
        if (lock || !just || !sh_standing()) return;
        if (!sh_front_point(&qx, &qz)) return;
        if (sh_abs(cam_x - qx) + sh_abs(cam_z - qz) >= SH_REACH) return;
        if (!interact_facing(qx, qz)) return;
        sh_start();
        return;
    }

    if (sh_state == SH_INTRO) {
        int32_t e;
        sh_t++;
        e = sh_ease(sh_t, SH_INTRO_FRAMES);
        cam_x     = sh_src_x     + (((sh_shot_x     - sh_src_x)     * e) >> 8);
        cam_y     = sh_src_y     + (((SH_SHOT_EYE_Y - sh_src_y)     * e) >> 8);
        cam_z     = sh_src_z     + (((sh_shot_z     - sh_src_z)     * e) >> 8);
        cam_rot   = sh_src_rot   + ((sh_rot_delta * e) >> 8);
        cam_pitch = sh_src_pitch - ((sh_src_pitch * e) >> 8);
        cam_vy    = 0;
        if (sh_t >= SH_INTRO_FRAMES) {
            cam_x = sh_shot_x; cam_y = SH_SHOT_EYE_Y; cam_z = sh_shot_z;
            cam_rot = SH_YAW; cam_pitch = 0; cam_vy = 0;
            sh_t        = 0;
            sh_state    = SH_READ;
            sh_btn_prev = btn;          /* a Cross already down is not a press */
        }
        return;
    }

    /* SH_READ: Cross, and nothing else. */
    {
        uint16_t pressed = btn & ~sh_btn_prev;
        sh_btn_prev = btn;
        if (sh_t < 0x7FFF) sh_t++;
        if (pressed & PAD_CROSS) {
            sound_play(SFX_BACK);
            sh_exit();
        }
    }
}

/* ---- Drawing ------------------------------------------------------------------------- */

static void sh_draw_slab(RenderContext *ctx, int32_t fog_factor) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint16_t tp_plain = texmgr_tpage(sh_tex_plain),   cl_plain = texmgr_clut(sh_tex_plain);
    uint16_t tp_crack = texmgr_tpage(sh_tex_cracked), cl_crack = texmgr_clut(sh_tex_cracked);
    int      cracked  = (sh_hits >= SH_CRACK_AT);
    uint8_t *p = (uint8_t *)sh_smd->p_prims;
    int pi;

    for (pi = 0; pi < sh_smd->n_prims; pi++) {
        SMD_PRI_TYPE *pt       = (SMD_PRI_TYPE *)p;
        uint8_t       stride   = pt->len;
        int           is_quad  = (pt->type >= 2);
        int           textured = pt->texture;
        int           front    = (pi < SH_MAX_PRIMS) && sh_front[pi];

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &sh_smd->p_verts[vi[0]];
        SVECTOR *v1 = &sh_smd->p_verts[vi[1]];
        SVECTOR *v2 = &sh_smd->p_verts[vi[2]];

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
            SVECTOR *v3 = &sh_smd->p_verts[vi[3]];
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
        /* TRUE DEPTH, no +40: the room's floor sorts at its far corner plus
           40, so the slab standing on it wins the tie. In front of the sky. */
        if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
        if (otz >= UW_SKY_OT)   otz = UW_SKY_OT - 1;

        uint8_t *col = p + 16;
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + UW_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + UW_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + UW_FOG_B * (256 - fog_factor)) >> 8);
        uint16_t tp = (front && cracked) ? tp_crack : tp_plain;
        uint16_t cl = (front && cracked) ? cl_crack : cl_plain;

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

/* One cube. Its eight corners are rotated and placed on the CPU and projected
   through the plain view, so no per-cube matrix ever goes into the GTE. NO
   BACKFACE CULL: a convex body painted face by face at each face's own depth
   comes out right, and it cannot get a winding wrong. */
static void sh_draw_shard(RenderContext *ctx, const Shard *s, const MATRIX *view) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    SVECTOR  rot = { s->rx, s->ry, s->rz, 0 };
    MATRIX   m;
    SVECTOR  wv[8];
    DVECTOR  scr[8];
    int32_t  sz[8], h, dist, fog, k, f;

    h = s->half;
    if (s->life < SH_SHARD_SHRINK) h = (h * s->life) / SH_SHARD_SHRINK;
    if (h <= 0) return;

    dist = sh_abs(s->x - cam_x) + sh_abs(s->z - cam_z);
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
    /* The view again, AFTER RotMatrix: belt and braces, so nothing computed
       above can have left the GTE holding anything but the camera. */
    sh_set_view(view);
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
        int32_t otz = (sz[c0] + sz[c1] + sz[c2] + sz[c3]) >> 4;   /* AVSZ4's scale */
        int32_t lit = ((int32_t)s->grey * SH_FACE_SHADE[f]) >> 8;
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

void sharur_tablet_draw(RenderContext *ctx) {
    MATRIX view;
    int    i;
    if (!sh_smd || current_area != STATE_SHARURS_TRIAL) return;

    camera_build_view(&view);
    sh_set_view(&view);

    if (sh_standing()) {
        int32_t dist = sh_abs(sh_cx - cam_x) + sh_abs(sh_cz - cam_z);
        /* Culled at fog-far from the box's CENTRE, plus its half-length so an
           end still in view is not lost with the middle. */
        if (dist - sh_hl <= g_fog_far)
            sh_draw_slab(ctx, render_fog_scale(dist));
    }

    for (i = 0; i < sh_shard_count; i++)
        if (sh_shards[i].life > 0) sh_draw_shard(ctx, &sh_shards[i], &view);

    /* THE SIGN, in front of the middle of the face - only while it can be
       acted on: standing, not being read, and the player in front of it. */
    if (sh_standing() && sh_state == SH_IDLE) {
        int32_t qx, qz, sx, sz, xz;
        int fade = 256;
        if (sh_front_point(&qx, &qz)) {
            xz = sh_abs(cam_x - qx) + sh_abs(cam_z - qz);
            if (xz < SH_TEXT_RADIUS) {
                if (xz > SH_FADE_NEAR) {
                    int range = SH_TEXT_RADIUS - SH_FADE_NEAR;
                    int prog  = xz - SH_FADE_NEAR;
                    if (prog > range) prog = range;
                    fade = 256 - ((prog * 256) / range);
                }
                sh_world(0, sh_ht + SH_SIGN_OUT, &sx, &sz);
                door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to examine",
                                        sx, SH_SIGN_Y, sz,
                                        50, 255, 50, fade, SH_YAW,
                                        DOOR_PIXEL_SIZE);
            }
        }
    }
}

/* A DR_TPAGE carrying blend mode `abr` into `slot`, AFTER the semi-trans prims
   it governs: an OT node is LIFO, so the GPU meets the mode first. */
static void sh_blend_tpage(RenderContext *ctx, int slot, int abr) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    DR_TPAGE *dp;
    if (ctx->next_packet + sizeof(DR_TPAGE) > buf_end) return;
    dp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(dp, 0, 0, getTPage(0, abr, 0, 0));
    addPrim(&ctx->buffers[ctx->active_buffer].ot[slot], dp);
    ctx->next_packet += sizeof(DR_TPAGE);
}

/* 0..255: how far letter `k` (counting letters only, across the lines) has
   faded in, `t` frames into the reading. */
static int32_t sh_letter_level(int32_t t, int32_t k) {
    int32_t since = t - k * SH_LETTER_GAP;
    if (since <= 0)              return 0;
    if (since >= SH_LETTER_FADE) return 255;
    return (since * 255) / SH_LETTER_FADE;
}

void sharur_tablet_draw_overlay(RenderContext *ctx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int32_t  band, k = 0;
    int      line;

    if (sh_state != SH_READ || current_area != STATE_SHARURS_TRIAL) return;

    /* THE BAND: a SUBTRACTIVE strip under the lines, so bone-white text over
       the bright stone or the fog stays legible. Faded in with the text. */
    band = (sh_t >= SH_BAND_FADE) ? 96 : (sh_t * 96) / SH_BAND_FADE;
    if (band > 0 && ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *tl = (TILE *)ctx->next_packet;
        setTile(tl);
        setSemiTrans(tl, 1);
        setXY0(tl, 0, SH_LINE_Y0 - 6);
        setWH(tl, SCREEN_XRES, SH_LINE_COUNT * SH_LINE_SPACING + 8);
        setRGB0(tl, (uint8_t)band, (uint8_t)band, (uint8_t)band);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[SH_OT_BAND], tl);
        ctx->next_packet += sizeof(TILE);
        sh_blend_tpage(ctx, SH_OT_BAND, 2);     /* B - F */
    }

    /* THE LINES, a letter at a time: each glyph its own brightness, ADDITIVE
       so it fades up out of whatever is behind it (Descent to Hell's lines). */
    for (line = 0; line < SH_LINE_COUNT; line++) {
        const char *s = SH_LINES[line];
        int x = (SCREEN_XRES - intro_text_width(s)) / 2;
        int y = SH_LINE_Y0 + line * SH_LINE_SPACING;
        for (; *s; s++) {
            char one[2];
            one[0] = *s; one[1] = '\0';
            if (*s != ' ') {
                int32_t lv = sh_letter_level(sh_t, k++);
                if (lv > 0)
                    intro_text_draw_at(ctx, one, x, y,
                                       (uint8_t)((SH_TEXT_R * lv) / 255),
                                       (uint8_t)((SH_TEXT_G * lv) / 255),
                                       (uint8_t)((SH_TEXT_B * lv) / 255),
                                       SH_OT_TEXT, 1);
            }
            /* The font's own advance: a glyph's width plus its 1px gap. */
            x += intro_text_width(one) + 1;
        }
    }
    sh_blend_tpage(ctx, SH_OT_TEXT, 1);         /* B + F */

    /* Once the last letter is up, how to leave. */
    if (sh_letter_level(sh_t, k - 1) >= 255)
        btn_prompt_draw(ctx, 8, 8, BTN_CROSS " - Exit", SH_OT_HINT);
}
