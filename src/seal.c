#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "room_arena.h"
#include "cull_arena.h"
#include "tim_slots.h"
#include "texmgr.h"
#include "camera.h"
#include "seal.h"
#include "collision.h"
#include "room_data.h"
#include "seal_tex_map.h"
#include "btn_glyph.h"
#include "door.h"               /* door_draw_string_billboard - the prompt     */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "sound.h"              /* SFX_EMERGE, SFX_EXPLODE: SND_BANK_SEAL        */
#include "particles.h"          /* spawn_burst - the explosions' debris        */
#include "rabisu.h"             /* rbs_glow_quad/point/pillar - every light    */

/* The Seal - see seal.h. The room half is copied from src/lamashtu_arena.c
   (itself the_head.c's), which has been through DIAGNOSING_FRAME_RATE.txt;
   the scene half borrows lamashtu_boss.c's camera glide. */

static SMD  *sl_smd  = NULL;
static void *sl_buff = NULL;

/* ---- No fog, no distance cull ------------------------------------------
   The visual mesh is ONLY the seal's own polys now - 107 of them; the
   transparent floor around it was deleted from the export - so there is
   nothing to hide, and the seal is meant to be seen from right across the
   room. Every poly is drawn at full colour from anywhere, and the Helluminator
   changes nothing (there is no view distance for it to lengthen).

   g_fog_* are set past the room's far corner (12000 in Manhattan XZ) so the
   sprites drawn here - the blasts' debris - are neither fogged nor culled. */
#define SL_NO_FOG_NEAR   12000
#define SL_NO_FOG_FAR    16000

/* Wall standoff. The chapter's 195. */
#define SL_WALL_RADIUS     195

/* The proxy's walls stop at y=-400; nothing is drawn above the floor. */
#define SL_CEILING_Y      (-400)

/* Standing eye on the one floor (y=0): less GROUND_FLOOR_Y and the 40-unit
   standoff apply_height applies. */
#define SL_FLOOR_Y           0
#define SL_EYE_Y          (SL_FLOOR_Y - GROUND_FLOOR_Y - 40)

/* THE SOUTH-EAST CORNER (east +X, south -Z), 400 in from both walls - twice
   the push radius, so the spawn is clear of both. */
#define SL_SPAWN_X        2600
#define SL_SPAWN_Z      (-2600)
#define SL_SPAWN_ROT      3584   /* north-west, straight at the seal (init says why) */

/* ---- Floor zones -----------------------------------------------------------
   ONE, FLAT, AT y=0, over the proxy's one floor face. */
static void sl_floor_zones_init(void) {
    int i = 0;

    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -3000;  floor_zones[i].max_x = 3000;
    floor_zones[i].min_z = -3000;  floor_zones[i].max_z = 3000;
    floor_zones[i].y     = SL_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   ONE, OWNED: seal.tim, 4bpp 128x128 at x576 y0 - the crib's page - with
   Lamashtu's own palette line at (640,502). Her skin is the page's other
   arena-only tenant and the arena re-uploads it (pixels and CLUT) on entry;
   every crib room puts the crib back on its way in. Transparent everywhere
   but the gold (CLUT 0x0000, which the GPU skips), so between the seal's
   lines the black shows through. */
#define SEAL_TEX_COUNT 1

static uint16_t tex_tpage[SEAL_TEX_COUNT];
static uint16_t tex_clut[SEAL_TEX_COUNT];
static int      seal_tex = -1;

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/lamashtu_arena.c. Built on the same call that reloads the
   mesh it describes, and nowhere else. */
static int sl_key_count = 0;

static void sl_build_cull_keys(void) {
    sl_key_count = 0;
    if (!sl_smd) return;
    uint8_t *p = (uint8_t *)sl_smd->p_prims;
    int i, n = sl_smd->n_prims;
    if (n > SEAL_PRIM_COUNT) n = SEAL_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &sl_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    sl_key_count = n;
}

void seal_load_geometry(void) {
    sl_buff = room_arena_load("\\TEXCTCMB\\SEAL.SMD;1");
    sl_smd  = sl_buff ? smdInitData(sl_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (sl_smd && !room_data_bind(sl_smd->n_prims)) sl_smd = NULL;
    sl_build_cull_keys();
}

void seal_load_assets(void) {
    texmgr_set_bank(TEXBANK_CATACOMBS);
    seal_tex = texmgr_register("\\TEXCTCMB\\SEAL.TIM;1");
    TIM_SLOT(0, SEAL);
}

/* Pure LoadImage from the RAM copy texmgr holds - no CD access, safe during the
   transition (main's STATE_LOADING DrawSyncs first). */
void seal_upload_textures(void) {
    if (seal_tex >= 0) texmgr_upload(seal_tex);
}

/* ---- The state ------------------------------------------------------------- */
typedef enum {
    SL_OFF = 0,
    SL_BLACK,      /* rooted, nothing drawn                                     */
    SL_FADE_IN,    /* free play, the room coming up                             */
    SL_READY,      /* free play, the prompt live                                */
    SL_RISE,       /* the scene: up off the player to the orbit                  */
    SL_ORBIT,      /* the scene: round and round, the lines, the blasts, white   */
    SL_WHITE,      /* held white, then main.c loads Descent to Hell under it    */
} SlState;

static SlState state    = SL_OFF;
static int32_t phase_t  = 0;
static int32_t scene_t  = 0;   /* frames since the scene began (RISE + ORBIT)  */
static int32_t sl_clock = 0;   /* free-running, for the flicker                */

static void seal_begin_scene(void);   /* THE SCENE, below */

#define SL_T_BLACK     60    /* 1 s: the second half of the brief's 2 s of dark */
#define SL_T_FADE_IN  360    /* 6 s up from black                               */

static void enter(SlState s) { state = s; phase_t = 0; }

void seal_reset(void) {
    sound_stop(SFX_EMERGE);
    sound_stop(SFX_EXPLODE);
    state   = SL_OFF;
    phase_t = 0;
    scene_t = 0;
}

int seal_roots_player(void) { return state == SL_BLACK; }

int seal_cutscene(void) {
    return state == SL_RISE || state == SL_ORBIT || state == SL_WHITE;
}

/* How long the white is held before the next room is loaded under it: long
   enough to read as the screen STAYING white, not as a cut. The load itself
   then holds it a while longer (main.c paints its one frame white too). */
#define SL_T_WHITE_HOLD  30

int seal_leaving(void) {
    return state == SL_WHITE && phase_t >= SL_T_WHITE_HOLD;
}

/* The room's brightness, 0..256. */
static int32_t sl_light(void) {
    if (state == SL_OFF || state == SL_BLACK) return 0;
    if (state == SL_FADE_IN) return (phase_t * 256) / SL_T_FADE_IN;
    return 256;
}

/* ---- THE PROMPT ------------------------------------------------------------
   The middle of the seal - the circle's centre is the room's (0,0). A
   billboard rather than a door's wall-plane sign, because it is approached
   from every side. The doors' green and their fade; the doors' Circle test. */
#define SL_CENTRE_X            0
#define SL_CENTRE_Z            0
#define SL_PROMPT_Y        (-186)   /* eye level on the y=0 floor */
#define SL_TEXT_RADIUS      1000
#define SL_FADE_NEAR         700
#define SL_TRIGGER_RADIUS    450

static int prompt_prev = 1;

static void sl_arm(void) { prompt_prev = interact_tapped(); }

static int32_t centre_dist(void) {
    int32_t dx = cam_x - SL_CENTRE_X;
    int32_t dz = cam_z - SL_CENTRE_Z;
    return (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
}

/* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED OR NOT YET READY, so a
   Circle held across either does not read as a fresh press when it lifts. */
static int sl_prompt_triggered(int lock) {
    int held = interact_tapped();
    int just = held && !prompt_prev;
    prompt_prev = held;
    if (lock || !just || state != SL_READY) return 0;
    if (centre_dist() >= SL_TRIGGER_RADIUS) return 0;
    if (!interact_facing(SL_CENTRE_X, SL_CENTRE_Z)) return 0;
    return 1;
}

static void sl_prompt_text(RenderContext *ctx) {
    int32_t xz = centre_dist();
    int fade = 256;
    if (xz >= SL_TEXT_RADIUS) return;
    if (xz > SL_FADE_NEAR) {
        int range = SL_TEXT_RADIUS - SL_FADE_NEAR;
        int prog  = xz - SL_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_billboard(ctx, "Press " BTN_CIRCLE,
                               SL_CENTRE_X, SL_PROMPT_Y, SL_CENTRE_Z,
                               50, 255, 50, fade, DOOR_PIXEL_SIZE);
}

/* ---- Spawn and init ------------------------------------------------------- */
void seal_init(void) {
    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(SL_CEILING_Y);
    collision_set_wall_radius(SL_WALL_RADIUS);

    sl_floor_zones_init();
    cam_pitch = 0;

    /* THE SOUTH-EAST CORNER, wherever in the arena they stood: the room is
       "placed around" the player, which in a void under a black screen is the
       same thing as placing the player in it.

       >>> AND TURNED TO FACE THE SEAL, NOT LEFT FACING WHERE THEY WERE. <<<
       The seal is the only thing in the room, ~3700 off, so a player still
       facing the arena's east wall would fade in looking at nothing at all.
       The turn happens under the black, so it is never seen. */
    cam_x   = SL_SPAWN_X;
    cam_y   = SL_EYE_Y;
    cam_vy  = 0;
    cam_z   = SL_SPAWN_Z;
    cam_rot = SL_SPAWN_ROT;

    /* The global prop arrays, as the arena clears them: an instance left from
       another room would block invisibly inside x[-3000,3000] z[-3000,3000]. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    bars_clear();
    reset_particles();

    scene_t     = 0;
    sl_arm();
    enter(SL_BLACK);
}

/* ---- Free play ------------------------------------------------------------- */
void seal_update(int lock) {
    sl_clock++;
    phase_t++;
    switch (state) {
    case SL_BLACK:
        if (phase_t >= SL_T_BLACK) enter(SL_FADE_IN);
        break;
    case SL_FADE_IN:
        if (phase_t >= SL_T_FADE_IN) enter(SL_READY);
        break;
    default:
        break;
    }
    if (sl_prompt_triggered(lock)) seal_begin_scene();
}

/* ===========================================================================
   THE SCENE
   ===========================================================================
   Seventeen and a half seconds, one clock (scene_t, frames), TIMED TO THE TWO
   CLIPS: the lines light over the whole of SFX_EMERGE, and the blasts and the
   white fill the whole of SFX_EXPLODE, which starts the moment the last line
   is lit. Retrim either clip and SL_T_EMERGE / SL_T_EXPLODE move with it.

       0 ..   90   RISE    off the player and up to the orbit, eased in and out
      90 .. 1050   ORBIT   round the seal's middle, speeding up all the way
      60 ..  728   the lines light over the emerge, bird to every end
     728 .. 1050   the explode: five blasts across it, the light curtains
                   climbing over its first second
     960 .. 1050   to white, landing as the explode ends
           1050   the room holds full white for SL_T_WHITE_HOLD, and then
                  seal_leaving() has main.c load Descent to Hell under it

   THE SHOT. The orbit's centre is SL_FOCUS - the circle's centre nudged north
   toward the bird so the whole seal, x[-677,677] z[-643,1071], sits in frame.
   The camera circles it at SL_ORBIT_R, SL_ORBIT_H above the floor, always
   looking at it: at those two the pitch is ~49 degrees down. Because the yaw
   always points AT the seal, no part of it is ever behind the draw loop's
   700-unit behind-the-yaw cull (the overhead-camera trap). */
#define SL_FOCUS_X           0
#define SL_FOCUS_Z         200
#define SL_ORBIT_R        1300
#define SL_ORBIT_H        1500

#define SL_T_EMERGE        668   /* SFX_EMERGE, 11.14 s (sound.h)              */
#define SL_T_EXPLODE       322   /* SFX_EXPLODE, 5.374 s (sound.h)             */

#define SL_T_RISE           90
#define SL_T_LINES_FROM     60   /* the emerge starts here...                   */
#define SL_T_LINES_TO      (SL_T_LINES_FROM + SL_T_EMERGE)  /* ...and ends here */
#define SL_T_SCENE         (SL_T_LINES_TO + SL_T_EXPLODE)   /* the explode ends */
#define SL_T_WHITE_FROM    (SL_T_SCENE - 90)                /* 1.5 s to white   */
#define SL_T_CURTAIN_CLIMB  60   /* curtains to full height after the last line */

/* Orbit speed, in 1/16ths of a 4096 turn a frame: from 3/frame (~16 degrees a
   second) to 34/frame (~180 a second) over the orbit's 960 frames - about four
   laps in all, the last of them a blur. */
#define SL_W0      (3  * 16)
#define SL_W1      (34 * 16)

static int32_t orbit_a16;      /* the orbit angle, 1/16ths of a 4096 turn */
static int32_t orbit_pitch;
static int32_t shake_amp;      /* camera shake, world units, decays        */
static uint32_t rng = 0x5EA1u;

static int32_t rnd(int32_t n) {
    rng = rng * 1103515245u + 12345u;
    return (int32_t)((rng >> 16) % (uint32_t)n);
}

/* ---- Camera glide: src/lamashtu_boss.c's ---------------------------------- */
typedef struct { int32_t x, y, z, rot, pitch; } CamShot;
static CamShot  glide_src, glide_dst;
static int32_t  glide_rot_delta;

static int32_t turn_delta(int32_t from, int32_t to) {
    int32_t d = ((to - from) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    return d;
}

static void glide_step(int32_t t, int32_t len) {
    int32_t p = (t * 256) / len; if (p > 256) p = 256;
    int32_t e = (p * p * (3 * 256 - 2 * p)) / (256 * 256);   /* ease in-out */
    cam_x     = glide_src.x     + ((glide_dst.x     - glide_src.x)     * e) / 256;
    cam_y     = glide_src.y     + ((glide_dst.y     - glide_src.y)     * e) / 256;
    cam_z     = glide_src.z     + ((glide_dst.z     - glide_src.z)     * e) / 256;
    cam_rot   = (glide_src.rot  + (glide_rot_delta * e) / 256) & 4095;
    cam_pitch = glide_src.pitch + ((glide_dst.pitch - glide_src.pitch) * e) / 256;
    cam_vy    = 0;
}

/* Pitch aiming at a point dy below and dz in front (rabisu_boss.c's). */
static int32_t aim_pitch(int32_t dy, int32_t dz) {
    int32_t lo = -1024, hi = 1024;
    while (hi - lo > 1) {
        int32_t mid = (lo + hi) >> 1, a = mid & 4095;
        if (isin(a) * dz < icos(a) * dy) lo = mid;
        else                             hi = mid;
    }
    return lo;
}

/* The yaw whose (sin, cos) points along (dx, dz): a coarse sweep and a fine
   one. Runs once, on the scene's first frame. */
static int32_t yaw_to(int32_t dx, int32_t dz) {
    int32_t a, best = 0, best_dot = -0x7FFFFFFF;
    for (a = 0; a < 4096; a += 32) {
        int32_t d = isin(a) * dx + icos(a) * dz;
        if (d > best_dot) { best_dot = d; best = a; }
    }
    int32_t lo = best - 32;
    for (a = lo; a <= best + 32; a++) {
        int32_t d = isin(a & 4095) * dx + icos(a & 4095) * dz;
        if (d > best_dot) { best_dot = d; best = a & 4095; }
    }
    return best;
}

/* The orbit position at angle a (4096 units): out from the focus along
   (sin a, cos a), looking back at it. */
static void orbit_shot(int32_t a, CamShot *s) {
    s->x     = SL_FOCUS_X + ((isin(a) * SL_ORBIT_R) >> 12);
    s->z     = SL_FOCUS_Z + ((icos(a) * SL_ORBIT_R) >> 12);
    s->y     = SL_FLOOR_Y - SL_ORBIT_H;
    s->rot   = (a + 2048) & 4095;
    s->pitch = orbit_pitch;
}

static void seal_begin_scene(void) {
    camera_look_cancel();
    camera_anchor_player(cam_x, cam_y, cam_z);

    /* Rise BEHIND the player: the orbit starts on the side of the seal they
       are standing on, so the first move is up and back, not across. Stood
       dead on the focus, behind is wherever their back is. */
    int32_t dx = cam_x - SL_FOCUS_X, dz = cam_z - SL_FOCUS_Z;
    int32_t a  = ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) < 64)
                     ? ((cam_rot + 2048) & 4095) : yaw_to(dx, dz);
    orbit_a16   = a << 4;
    orbit_pitch = aim_pitch(SL_ORBIT_H, SL_ORBIT_R);

    glide_src.x = cam_x; glide_src.y = cam_y; glide_src.z = cam_z;
    glide_src.rot = cam_rot; glide_src.pitch = cam_pitch;
    orbit_shot(a, &glide_dst);
    glide_rot_delta = turn_delta(glide_src.rot, glide_dst.rot);

    shake_amp = 0;
    scene_t   = 0;
    reset_particles();
    enter(SL_RISE);
}

/* ---- The explosions ---------------------------------------------------------
   "A few explosions at different spots on the seal": the bird, both sides of
   the circle, its foot and its middle, each at a node of the light map so the
   blast sits on a lit line. Spread across the explode: the first on its first
   frame, the last into the white, each one closer to the one before. */
#define SL_BLASTS       5
#define SL_BLAST_LIFE  70
static const struct { int16_t x, z, t; } BLAST[SL_BLASTS] = {
    { -677,   214, SL_T_LINES_TO       },   /* the circle's west side   */
    {  677,  -214, SL_T_LINES_TO +  70 },   /* its east side            */
    {    0,   950, SL_T_LINES_TO + 130 },   /* the bird                 */
    {    0,  -643, SL_T_LINES_TO + 180 },   /* the foot                 */
    {   97,     0, SL_T_LINES_TO + 222 },   /* the middle               */
};

static void blasts_tick(void) {
    int i;
    for (i = 0; i < SL_BLASTS; i++) {
        if (scene_t != BLAST[i].t) continue;
        spawn_burst(BLAST[i].x, SL_FLOOR_Y - 60, BLAST[i].z, 255, 150, 40);
        /* SFX_EXPLODE is one 5.4 s rumble on one voice: replaying it would cut
           it off, so it is the first blast's and it carries the rest. */
        if (i == 0) sound_play(SFX_EXPLODE);
        shake_amp = 40;
    }
}

/* 0..256 for blast i at this frame: a hard flash that decays. */
static int32_t blast_level(int i) {
    int32_t a = scene_t - BLAST[i].t;
    if (a < 0 || a >= SL_BLAST_LIFE) return 0;
    return 256 - (a * 256) / SL_BLAST_LIFE;
}

void seal_cutscene_update(void) {
    sl_clock++;
    phase_t++;

    if (state == SL_WHITE) return;   /* held; seal_leaving() counts it out */

    scene_t++;

    /* THE EMERGE as the lines start to light: light hauled up out of the
       ground (SND_BANK_SEAL). The fuse is timed to it: the last line lights
       as it ends, and the explode takes over (SL_T_EMERGE). */
    if (scene_t == SL_T_LINES_FROM) sound_play(SFX_EMERGE);

    if (state == SL_RISE) {
        glide_step(scene_t, SL_T_RISE);
        if (scene_t >= SL_T_RISE) enter(SL_ORBIT);
    } else if (state == SL_ORBIT) {
        int32_t span = SL_T_SCENE - SL_T_RISE;
        int32_t t    = scene_t - SL_T_RISE; if (t > span) t = span;
        orbit_a16 += SL_W0 + ((SL_W1 - SL_W0) * t) / span;
        CamShot s;
        orbit_shot((orbit_a16 >> 4) & 4095, &s);
        cam_x = s.x; cam_y = s.y; cam_z = s.z;
        cam_rot = s.rot; cam_pitch = s.pitch; cam_vy = 0;
    }

    blasts_tick();
    if (shake_amp > 0) {
        cam_x += rnd(2 * shake_amp + 1) - shake_amp;
        cam_y += rnd(2 * shake_amp + 1) - shake_amp;
        cam_z += rnd(2 * shake_amp + 1) - shake_amp;
        shake_amp = (shake_amp * 7) / 8;
    }

    if (scene_t >= SL_T_SCENE) {
        /* FULL WHITE, held - and then Chapter 4 (seal_leaving, below). */
        enter(SL_WHITE);
    }
}

/* ---- The lines --------------------------------------------------------------
   Read off misc/seal light map: every yellow line there is an edge of the
   visual mesh (Seal.smx), on its 31 x 28 grid, cut into one stroke per grid
   step. Generated offline: d0/d1 are how far along the network each end is
   from the bird's head, so the light runs out of the bird like a fuse and
   branches down. The V in the circle's top-left touches no other line; the
   light jumps one grid step to it at both ends.

   Each stroke lights from whichever end the fuse reaches first (both, where
   two runs meet in the middle). */
typedef struct { int16_t x0, z0, x1, z1; uint16_t d0, d1, len; } SealLine;

/* 38 strokes; the fuse reaches its last end at 3888. */
#define SEAL_LINE_COUNT 38
#define SEAL_LINE_DMAX  3888
static const SealLine LINES[SEAL_LINE_COUNT] = {
    {   -97,  1071,    97,  1071,     0,     0,  194 },
    {    97,  1071,   290,   857,     0,   288,  288 },
    {   290,   857,   290,   643,   288,   502,  214 },
    {   290,   643,   484,   643,   502,   696,  194 },
    {   484,   643,   677,   429,   696,   984,  288 },
    {   677,   429,   677,   214,   984,  1199,  215 },
    {   677,   214,   677,     0,  1199,  1413,  214 },
    {   677,     0,   677,  -214,  1413,  1627,  214 },
    {   677,  -214,   677,  -429,  1627,  1842,  215 },
    {   677,  -429,   484,  -643,  1842,  2131,  288 },
    {   484,  -643,   290,  -643,  2131,  2325,  194 },
    {   290,  -643,    97,  -643,  2325,  2518,  193 },
    {    97,  -643,   -97,  -643,  2518,  2518,  194 },
    {   -97,  -643,  -290,  -643,  2518,  2325,  193 },
    {  -290,  -643,  -484,  -643,  2325,  2131,  194 },
    {  -484,  -643,  -677,  -429,  2131,  1842,  288 },
    {  -677,  -429,  -677,  -214,  1842,  1627,  215 },
    {  -677,  -214,  -677,     0,  1627,  1413,  214 },
    {  -677,     0,  -677,   214,  1413,  1199,  214 },
    {  -677,   214,  -677,   429,  1199,   984,  215 },
    {  -677,   429,  -484,   643,   984,   696,  288 },
    {  -484,   643,  -290,   643,   696,   502,  194 },
    {  -290,   643,  -290,   857,   502,   288,  214 },
    {  -290,   857,   -97,  1071,   288,     0,  288 },
    {  -484,   429,  -290,   214,  1177,  1273,  290 },
    {  -290,   214,   -97,   429,  1273,   984,  289 },
    {   -97,   429,    97,   643,   984,   695,  289 },
    {  -484,  -214,  -290,     0,  2560,  2848,  289 },
    {  -290,     0,   -97,   214,  2848,  3137,  288 },
    {  -484,  -214,  -484,  -429,  2560,  2345,  215 },
    {  -484,  -429,  -484,  -643,  2345,  2131,  214 },
    {   484,   429,   290,   214,  3888,  3598,  290 },
    {   290,   214,    97,     0,  3598,  3310,  288 },
    {    97,     0,   -97,  -214,  3310,  3021,  289 },
    {   -97,  -214,    97,  -429,  3021,  2732,  290 },
    {    97,  -429,    97,  -643,  2732,  2518,  214 },
    {   677,   214,   484,     0,  1199,  1488,  288 },
    {   484,     0,   290,  -214,  1488,  1776,  289 },
};

#define SL_LINE_Y      (SL_FLOOR_Y - 3)   /* just proud of the floor          */
#define SL_CORE_HALF     26               /* half-widths, world units          */
#define SL_HALO_HALF     90
#define SL_MAX_SPARKS    10

/* THE CURTAIN: light standing UP off every lit run, so the lines read as light
   pouring out of the seal rather than paint on it. An additive Gouraud quad:
   the run's colour at the floor, black - which adds nothing - at the top. It
   stands SL_CURTAIN_H tall while the fuse burns and climbs to SL_CURTAIN_H_MAX
   once everything is lit, by the first blast. Vertical, and never culled: from
   either side it is the same light. */
#define SL_CURTAIN_H      300
#define SL_CURTAIN_H_MAX  600

static void curtain(RenderContext *ctx, int32_t ax, int32_t az,
                    int32_t bx, int32_t bz, int32_t h,
                    uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_G4) + sizeof(DR_TPAGE) > buf_end) return;

    SVECTOR v[4];
    v[0].vx = (int16_t)ax; v[0].vy = (int16_t)SL_LINE_Y;       v[0].vz = (int16_t)az;
    v[1].vx = (int16_t)bx; v[1].vy = (int16_t)SL_LINE_Y;       v[1].vz = (int16_t)bz;
    v[2].vx = (int16_t)ax; v[2].vy = (int16_t)(SL_LINE_Y - h); v[2].vz = (int16_t)az;
    v[3].vx = (int16_t)bx; v[3].vy = (int16_t)(SL_LINE_Y - h); v[3].vz = (int16_t)bz;
    v[0].pad = v[1].pad = v[2].pad = v[3].pad = 0;

    /* rbs_glow_quad_ex's projection, step for step (src/rabisu.c). */
    DVECTOR sv[4];
    int32_t sz[4], otz;
    int k;
    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);
    gte_stsz4c(sz);
    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);
    gte_stsz(&sz[3]);
    if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) return;
    for (k = 0; k < 4; k++)
        if (sv[k].vx <= -1023 || sv[k].vx >= 1023 ||
            sv[k].vy <= -1023 || sv[k].vy >= 1023) return;
    gte_avsz4();
    gte_stotz(&otz);
    if (otz <= SCENE_OT_MIN) return;
    otz += 40;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    POLY_G4 *p = (POLY_G4 *)ctx->next_packet;
    setPolyG4(p);
    setSemiTrans(p, 1);
    setRGB0(p, r, g, b);
    setRGB1(p, r, g, b);
    setRGB2(p, 0, 0, 0);
    setRGB3(p, 0, 0, 0);
    p->x0 = sv[0].vx; p->y0 = sv[0].vy;
    p->x1 = sv[1].vx; p->y1 = sv[1].vy;
    p->x2 = sv[2].vx; p->y2 = sv[2].vy;
    p->x3 = sv[3].vx; p->y3 = sv[3].vy;
    addPrim(&ot[otz], p);
    ctx->next_packet += sizeof(POLY_G4);

    /* AFTER the poly: an OT node is LIFO, so the GPU sees the blend first. */
    DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
    addPrim(&ot[otz], tp);
    ctx->next_packet += sizeof(DR_TPAGE);
}

/* One additive strip along stroke L from `from` to `to` (distances along it,
   from its x0/z0 end). */
static void stroke(RenderContext *ctx, const SealLine *L, int32_t from,
                   int32_t to, int32_t half, uint8_t r, uint8_t g, uint8_t b) {
    int32_t dx = L->x1 - L->x0, dz = L->z1 - L->z0, len = L->len;
    int32_t ax = L->x0 + (dx * from) / len, az = L->z0 + (dz * from) / len;
    int32_t bx = L->x0 + (dx * to)   / len, bz = L->z0 + (dz * to)   / len;
    int32_t px = (-dz * half) / len, pz = (dx * half) / len;
    SVECTOR v[4];
    v[0].vx = (int16_t)(ax + px); v[0].vz = (int16_t)(az + pz);
    v[1].vx = (int16_t)(ax - px); v[1].vz = (int16_t)(az - pz);
    v[2].vx = (int16_t)(bx + px); v[2].vz = (int16_t)(bz + pz);
    v[3].vx = (int16_t)(bx - px); v[3].vz = (int16_t)(bz - pz);
    int k;
    for (k = 0; k < 4; k++) { v[k].vy = SL_LINE_Y; v[k].pad = 0; }
    rbs_glow_quad(ctx, v, r, g, b);
}

/* A lit run of stroke L, `from`..`to`: the halo and the hot core on the
   floor, and the curtain standing up off it. */
static void lit_run(RenderContext *ctx, const SealLine *L, int32_t from,
                    int32_t to, int i, int32_t h) {
    /* Per-stroke flicker, never the same on two strokes; it dips the light a
       little and shakes the curtain's top. */
    int32_t fl  = (sl_clock * 7 + i * 41) & 63;
    int32_t dip = (fl < 32 ? fl : 63 - fl);              /* 0..31 */
    int32_t lvl = 256 - dip;
    uint8_t cr = 255;
    uint8_t cg = (uint8_t)(((150 + ((i * 13 + sl_clock) & 31)) * lvl) >> 8);
    uint8_t cb = (uint8_t)((50 * lvl) >> 8);

    stroke(ctx, L, from, to, SL_HALO_HALF,
           (uint8_t)((210 * lvl) >> 8), (uint8_t)((40 * lvl) >> 8), 0);
    stroke(ctx, L, from, to, SL_CORE_HALF, cr, cg, cb);

    int32_t dx = L->x1 - L->x0, dz = L->z1 - L->z0, len = L->len;
    curtain(ctx, L->x0 + (dx * from) / len, L->z0 + (dz * from) / len,
                 L->x0 + (dx * to)   / len, L->z0 + (dz * to)   / len,
            h - dip * 2,
            (uint8_t)((230 * lvl) >> 8), (uint8_t)((70 * lvl) >> 8),
            (uint8_t)((8 * lvl) >> 8));
}

/* How far down the network the light has got this frame. */
static int32_t fuse_dist(void) {
    int32_t t = scene_t - SL_T_LINES_FROM;
    if (t <= 0) return 0;
    return (t * SEAL_LINE_DMAX) / (SL_T_LINES_TO - SL_T_LINES_FROM);
}

static void draw_lines(RenderContext *ctx) {
    int32_t s = fuse_dist();
    if (s <= 0) return;

    /* The curtain climbs once everything is lit, to its full height over the
       explode's first second. */
    int32_t h    = SL_CURTAIN_H;
    int32_t full = scene_t - SL_T_LINES_TO;
    if (full > 0) {
        if (full > SL_T_CURTAIN_CLIMB) full = SL_T_CURTAIN_CLIMB;
        h += ((SL_CURTAIN_H_MAX - SL_CURTAIN_H) * full) / SL_T_CURTAIN_CLIMB;
    }

    int sparks = 0, i;
    for (i = 0; i < SEAL_LINE_COUNT; i++) {
        const SealLine *L = &LINES[i];
        int32_t la = s - L->d0, lb = s - L->d1;
        if (la <= 0 && lb <= 0) continue;
        if (la < 0)      la = 0;
        if (la > L->len) la = L->len;
        if (lb < 0)      lb = 0;
        if (lb > L->len) lb = L->len;

        if (la + lb >= L->len) {
            lit_run(ctx, L, 0, L->len, i, h);
            continue;
        }
        /* Still burning along: one run in from each end that has been reached,
           and a hot spark riding the front of each. */
        if (la > 0) {
            lit_run(ctx, L, 0, la, i, h);
            if (sparks < SL_MAX_SPARKS) {
                VECTOR w = { L->x0 + ((L->x1 - L->x0) * la) / L->len,
                             SL_LINE_Y - 40,
                             L->z0 + ((L->z1 - L->z0) * la) / L->len };
                rbs_glow_point(ctx, &w, 256, sl_clock + i * 3);
                sparks++;
            }
        }
        if (lb > 0) {
            lit_run(ctx, L, L->len - lb, L->len, i, h);
            if (sparks < SL_MAX_SPARKS) {
                VECTOR w = { L->x1 - ((L->x1 - L->x0) * lb) / L->len,
                             SL_LINE_Y - 40,
                             L->z1 - ((L->z1 - L->z0) * lb) / L->len };
                rbs_glow_point(ctx, &w, 256, sl_clock + i * 3);
                sparks++;
            }
        }
    }
}

static void draw_blasts(RenderContext *ctx) {
    int i;
    for (i = 0; i < SL_BLASTS; i++) {
        int32_t lv = blast_level(i);
        if (lv <= 0) continue;
        int32_t r = 160 + (256 - lv);          /* the pool spreads as it fades */
        rbs_glow_pillar(ctx, BLAST[i].x - r, BLAST[i].x + r,
                        BLAST[i].z - r, BLAST[i].z + r, SL_FLOOR_Y,
                        lv, sl_clock + i * 11);
        VECTOR w = { BLAST[i].x, SL_FLOOR_Y - 160, BLAST[i].z };
        rbs_glow_point(ctx, &w, lv, sl_clock + i * 11);
    }
}

/* ---- The white --------------------------------------------------------------
   An ADDITIVE full-screen tile (ABR=1) at ot[2], in front of the scene. At
   full it is the white the room hands Descent to Hell (src/descent_to_hell.h),
   which fades up out of the same white. The DR_TPAGE goes in AFTER the tile:
   an OT node is LIFO, and the GPU must see the blend mode first (trial_end.c's
   wash, the same note). */
#define SL_OT_WHITE  2

static void draw_white(RenderContext *ctx, int32_t k) {
    if (k <= 0) return;
    if (k > 255) k = 255;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot     = &ctx->buffers[ctx->active_buffer].ot[SL_OT_WHITE];
    if (ctx->next_packet + sizeof(TILE) + sizeof(DR_TPAGE) > buf_end) return;

    TILE *tl = (TILE *)ctx->next_packet;
    setTile(tl);
    setSemiTrans(tl, 1);
    setRGB0(tl, (uint8_t)k, (uint8_t)k, (uint8_t)k);
    setXY0(tl, 0, 0);
    setWH(tl, SCREEN_XRES, SCREEN_YRES);
    addPrim(ot, tl);
    ctx->next_packet += sizeof(TILE);

    DR_TPAGE *dp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(dp, 0, 0, getTPage(0, 1 /* ABR=1: B + F */, 0, 0));
    addPrim(ot, dp);
    ctx->next_packet += sizeof(DR_TPAGE);
}

/* This frame's white: each blast flashes, then the long fade over the end. */
static int32_t white_level(void) {
    if (state == SL_WHITE) return 255;
    if (state != SL_ORBIT && state != SL_RISE) return 0;
    int32_t k = 0, i;
    for (i = 0; i < SL_BLASTS; i++) {
        int32_t a = scene_t - BLAST[i].t;
        if (a >= 0 && a < 20) {
            int32_t f = 110 - a * 5;
            if (f > k) k = f;
        }
    }
    if (scene_t >= SL_T_WHITE_FROM) {
        int32_t w = ((scene_t - SL_T_WHITE_FROM) * 255) /
                    (SL_T_SCENE - SL_T_WHITE_FROM);
        if (w > k) k = w;
    }
    return k;
}

/* ---- The line ---------------------------------------------------------------
   One line as the seal starts to light, in the bosses' subtitle style
   (lamashtu_boss.c's lbe_line): centred, 8 px a character, at their first row
   and their OT. Up for 6 s from the emerge - the bosses' cadence a line.
   Sorted BEFORE draw_white puts its tile in the same OT slot: LIFO, so the
   text is drawn after the white, never under it. */
static const char *SL_LINE = "AT LAST, I AM FREE...";
#define SL_TEXT_Y       194
#define SL_TEXT_OT        2
#define SL_T_LINE_FROM  SL_T_LINES_FROM
#define SL_T_LINE_TO   (SL_T_LINE_FROM + 360)

static void draw_line(RenderContext *ctx) {
    if (!seal_cutscene()) return;
    if (scene_t < SL_T_LINE_FROM || scene_t >= SL_T_LINE_TO) return;
    int x = (SCREEN_XRES - (int)strlen(SL_LINE) * 8) / 2;
    if (x < 0) x = 0;
    btn_prompt_draw(ctx, x, SL_TEXT_Y, SL_LINE, SL_TEXT_OT);
}

/* ---- The room ---------------------------------------------------------------
   draw_lamashtu_arena_smd() with the fog and the distance cull taken out and
   the fade-in's light scale on the colour instead. */
static void draw_seal_smd(RenderContext *ctx, int32_t lit) {
    if (!sl_smd || lit <= 0) return;

    uint8_t *p = (uint8_t *)sl_smd->p_prims;
    int i, n = sl_key_count;

    /* HOISTED OUT OF THE LOOP, all four (STEP 3C fix 1). */
    /* NO DISTANCE CULL (see "No fog"): only the debug isolation levels set
       one. The behind-the-yaw cull stays - it never drops anything in view. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = 0x7FFFFFFF;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    for (i = 0; i < n; i++) {
        /* THE REJECT PATH READS cull_keys, NOT THE MESH. */
        uint8_t stride = cull_keys[i].stride;
        {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            int32_t cd = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            if (cd > cull)                        { p += stride; continue; }
            if (dx * sn + dz * cs < -(700 << 12)) { p += stride; continue; }
        }

        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &sl_smd->p_verts[vi[0]];
        SVECTOR *v1 = &sl_smd->p_verts[vi[1]];
        SVECTOR *v2 = &sl_smd->p_verts[vi[2]];

        DVECTOR sv[4];
        int32_t sz[4];
        int32_t otz, nclip;

        gte_ldv3(v0, v1, v2);
        gte_rtpt();
        gte_stsxy3c(sv);

        if (sv[0].vx <= -1023 || sv[0].vx >= 1023 || sv[0].vy <= -1023 || sv[0].vy >= 1023 ||
            sv[1].vx <= -1023 || sv[1].vx >= 1023 || sv[1].vy <= -1023 || sv[1].vy >= 1023 ||
            sv[2].vx <= -1023 || sv[2].vx >= 1023 || sv[2].vy <= -1023 || sv[2].vy >= 1023) {
            p += stride; continue;
        }

        int nocull = (i < SEAL_PRIM_COUNT) && room_nocull(i);
        if (!pt->nocull && !nocull) {
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }

        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        SVECTOR *v3    = 0;
        int32_t  v2_sz = sz[3];
        if (is_quad) {
            v3 = &sl_smd->p_verts[vi[3]];
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
        if (poly_is_flat_y(v0, v1, v2, v3))
            otz = is_quad ? otz_far4(sz[1], sz[2], v2_sz, sz[3])
                          : otz_far3(sz[1], sz[2], sz[3]);
        if (otz <= 0) { p += stride; continue; }
        otz += 40;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        uint8_t *col = p + 16;

        uint8_t tex_idx = (i < SEAL_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < SEAL_TEX_COUNT);

        /* No fog: the fade-in's light scale is the only thing on the colour. */
        uint8_t r = (uint8_t)(((int32_t)col[0] * lit) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * lit) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * lit) >> 8);

        if (is_quad && textured) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            uint8_t *uv = p + 20;
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, r, g, b);
            poly->tpage = tex_tpage[tex_idx];
            poly->clut  = tex_clut[tex_idx];
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
            poly->tpage = tex_tpage[tex_idx];
            poly->clut  = tex_clut[tex_idx];
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

void seal_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    g_fog_near = SL_NO_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : SL_NO_FOG_FAR;

    /* True black as the CLEAR colour, not a full-screen TILE (wrong turn #3 in
       tools/DIAGNOSING_FRAME_RATE.txt): the void the arena faded into. */
    render_set_clear_colour(ctx, 0, 0, 0);

    /* 128x128 texture window; the seal sits at Voff 0. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    MATRIX rot_matrix;
    camera_build_view(&rot_matrix);
    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    if (exp != DBG_EXP_NO_MESH) draw_seal_smd(ctx, sl_light());

    /* >>> LEVEL 8 IS THE PROMPT AND THE SCENE'S LIGHT, the only things that
       stand in this room. <<< */
    if (exp != DBG_EXP_NO_ENTITIES) {
        if (state == SL_READY) sl_prompt_text(ctx);
        if (seal_cutscene()) {
            draw_lines(ctx);
            draw_blasts(ctx);
        }
    }

    draw_line(ctx);
    draw_white(ctx, white_level());
}
