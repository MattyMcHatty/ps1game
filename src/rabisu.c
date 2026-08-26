#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"         /* player_x/y/z, player_knockback */
#include "collision.h"      /* GROUND_FLOOR_Y, collision_segment_blocked */
#include "crucifaxe.h"      /* swing_timer — the deflect windows read it */
#include "player.h"         /* player_hurt, player_health, game_over */
#include "particles.h"
#include "sound.h"
#include "title.h"          /* game_state */
#include "texmgr.h"
#include "rabisu.h"

/* Rabisu — the first boss, and the first 3D-model enemy. See rabisu.h for the
   tuning sheet and tools/ADDING_A_3D_ENEMY.txt for the procedure.

   >>> EVERYTHING IN THIS FILE TARGETS player_x/y/z(), NEVER cam_*. <<< The
   encounter's cutscenes fly the camera to a fixed vantage while the player
   stands where they stood, so a boss that chased cam_* would spend the reveal
   staring at an empty corner of the garden and the fight's first fireball
   would be aimed at a camera. camera.h's anchor is exactly for this. */

Rabisu rabisus[MAX_RABISUS];
int    rabisu_count = 0;

static SMD  *rabisu_smd  = NULL;
static void *rabisu_buff = NULL;

/* The boss's one skin, via texmgr rather than a private RAM copy (the rule in
   tools/ADDING_A_3D_ENEMY.txt STEP 3 for a textured model). Its VRAM slot at
   (704,256) is the boss's alone — nothing else streams over it — so it is
   uploaded ONCE at startup and stays resident, and no room needs a
   rabisus_upload_textures() on its transition.

   -1 until registered, which is also what a failed registration leaves behind
   (texmgr's cap fails SILENTLY); the draw falls back to the flat-colour path
   on that value rather than drawing the model in whatever art happens to sit
   at tpage 0. */
static int rabisu_tex = -1;

/* Distance beyond which the whole model is skipped. Generous compared with the
   concrete props' 1500: this thing is 4.6 m tall and is the thing the player is
   in the room to look at, so it stays drawn to the edge of the Garden
   Courtyard's 3500 fog rather than popping in. */
#define RBS_DRAW_DIST      3600

/* Extra gap between the player and the body cylinder, matching the margin the
   crates and concrete props use. */
#define RBS_PUSH_MARGIN     30

/* Integer square root (Newton, seeded by halving). Same routine web.c uses for
   its projectile aim: the Manhattan divide the steering code gets away with
   would skew the facing direction by up to ~40% off-axis, which on a model that
   visibly turns reads as the boss looking past the player. */
static int32_t isqrt32(int32_t v) {
    if (v <= 0) return 0;
    int32_t x = v, last;
    if (x > 1 << 16) x = 1 << 16;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
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

/* ---- Baked vertex animation (.pva) ----------------------------------------
   The rig smooth-skins up to THIRTEEN bones per vertex, so neither PS1-era
   option was open: the mesh cannot be split into rigid per-bone pieces, and
   evaluating that many weights over 496 vertices every frame is far outside the
   GTE's budget. Blender therefore does the skinning at EXPORT time and ships
   finished vertex positions, one set per frame (tools/io_export_pva.py).

   Playback is then free: pick the frame's SVECTOR block and hand it to the same
   prim loop. The .smd still owns the topology — the polygon vertex INDICES and
   the baked colours — and only the positions come from here, which is why the
   two files must be exported from the same mesh and why the vertex count is
   checked below before a single frame is trusted.

   Cost: 19 frames x 496 verts x 8 bytes = 73.6 KB resident. That is the price
   of the approach and it scales linearly with every clip added — see
   tools/ANIMATING_A_3D_MODEL.txt before adding the fourth or fifth. */
#define PVA_HEADER_SIZE   12

static void    *rabisu_anim_buff   = NULL;
static SVECTOR *rabisu_anim_frames = NULL;   /* n_frames blocks of n_verts */
static int      rabisu_anim_count  = 0;      /* 0 = play the bind pose */

static void rabisus_load_anim(void) {
    if (!rabisu_smd) return;
    uint8_t *p = (uint8_t *)read_file("\\TEX\\RBSIDLE.PVA;1");
    if (!p) return;

    /* Fields read byte-wise rather than through a struct: the file is
       little-endian and packed, and a struct would invite the compiler to pad
       it. */
    if (p[0] != 'P' || p[1] != 'V' || p[2] != 'A' || p[3] != '1') { free(p); return; }
    int n_verts  = p[4] | (p[5] << 8);
    int n_frames = p[6] | (p[7] << 8);

    /* >>> The check that keeps a stale .pva from drawing garbage. <<< Positions
       are indexed by the .smd's polygon indices, so a file baked from a mesh
       with a different vertex count would read off the end of every frame.
       Refuse it and fall back to the bind pose, which always renders. */
    if (n_verts != rabisu_smd->n_verts || n_frames <= 0) { free(p); return; }

    rabisu_anim_buff   = p;
    rabisu_anim_frames = (SVECTOR *)(p + PVA_HEADER_SIZE);
    rabisu_anim_count  = n_frames;
}

/* Startup: load the model, its skin and its idle clip. Both the CD reads and
   the LoadImage are only safe before the render loop begins
   (tools/TEXTURING_NOTES.txt), which is why all three happen here and nothing
   the boss owns is touched on a room transition. */
void rabisus_load_assets(void) {
    /* In TEX, not the disc root: the root directory records must all fit
       the first 2048-byte sector or the boot ROM cannot find SYSTEM.CNF
       and the console hangs at the logo. See the comment in disc.xml. */
    rabisu_buff = read_file("\\TEX\\RABISU.SMD;1");
    if (rabisu_buff) rabisu_smd = smdInitData(rabisu_buff);

    /* Uploaded here AND, since the Greenhouse's vines took this slot, again on
       entry to the room the boss actually fights in — see
       rabisus_restore_texture below. */
    rabisu_tex = texmgr_register("\\TEX\\RABISU.TIM;1");
    if (rabisu_tex >= 0) texmgr_upload(rabisu_tex);

    rabisus_load_anim();
}

/* PUT THE SKIN BACK. x704 y256 stopped being the boss's alone when the
   Greenhouse's vine curtain took the page (it was the cheapest full 8bpp page
   left in the garden-west bank — every other one carried the Anzu tiles, the
   kitchen's resident set, the global door-panel art or stn_gls). The boss fights
   in the Garden Courtyard, which the player reaches by walking back out of the
   Greenhouse, so that room's uploader calls this on the way in.

   It is one LoadImage off the RAM copy texmgr_register already keeps — no CD
   access, so it is safe on a transition — and a no-op if the registration failed
   (past texmgr's cap), which is the same condition the draw already falls back
   on. See tools/VRAM_MAP_GARDEN_WEST.txt. */
void rabisus_restore_texture(void) {
    if (rabisu_tex >= 0) texmgr_upload(rabisu_tex);
}

/* The vertex block this boss is posed on for this frame. Falls back to the
   .smd's own bind pose whenever the animation is missing or was rejected. */
static SVECTOR *rbs_verts(const Rabisu *r) {
    if (!rabisu_anim_frames) return rabisu_smd->p_verts;
    int f = r->anim_frame;
    if (f < 0 || f >= rabisu_anim_count) f = 0;
    return rabisu_anim_frames + (f * rabisu_smd->n_verts);
}

/* Self-contained LCG, as the Anzu and lightswitch puzzles use — there is no
   global PRNG in this project. It picks the number of traversals between
   attacks and which attack follows, and it jitters the death shake. */
static uint32_t rbs_rng = 0x9E3779B9u;
static uint32_t rbs_rand(void) {
    rbs_rng = rbs_rng * 1664525u + 1013904223u;
    return rbs_rng >> 16;
}

void rabisus_init(void) {
    /* Placements are seeded on a room's first entry by the world system (see
       world_enter in world.c). The array starts empty. */
    rabisu_count = 0;
    rbs_fireballs_reset();
}

void rabisus_reset(void) {
    rabisu_count = 0;
    rbs_fireballs_reset();
}

/* Put a slot into its pristine, full-health, spawn-point state. The spawn and
   the area tag are read out of the slot first and put back, so this is the one
   routine both the initial placement and every later rest go through — there
   is no second list of fields to keep in step. */
static void rbs_reset_state(Rabisu *r) {
    int32_t sx = r->spawn_x, sy = r->spawn_y, sz = r->spawn_z;
    GameState area = r->area;

    *r = (Rabisu){0};
    r->x = sx; r->y = sy; r->z = sz;
    r->spawn_x = sx; r->spawn_y = sy; r->spawn_z = sz;
    r->health = RBS_MAX_HEALTH;
    r->active = 1;
    r->area   = area;
    /* Looking down world +Z until the first update turns it toward the player.
       The negation matches the one the steering applies, so this seed agrees
       with every value that follows it — set it to +ONE and the boss spends
       its first frame, and the whole of its dormant wait, pointing the way it
       used to before the yaw was corrected. */
    r->face_s = 0;
    r->face_c = RBS_FACE_BACKWARD ? -ONE : ONE;
    /* Solid and unclipped. The zero-fill above would otherwise leave it fully
       burnt out AND cut off at world y=0 — invisible twice over from the moment
       it was placed. */
    r->fade   = 256;
    r->clip_y = RBS_NO_CLIP;
    /* Dormant until the encounter director says otherwise: a boss must not
       start throwing fireballs while its own reveal cutscene is playing. */
    r->ai_state = RBS_AI_DORMANT;
}

int rabisu_add(int32_t x, int32_t ground_y, int32_t z, GameState area) {
    if (rabisu_count >= MAX_RABISUS) return -1;
    int i = rabisu_count++;
    Rabisu *r = &rabisus[i];
    /* The anchor is the model's UNDERSIDE — the lowest point it reaches across
       the WHOLE animation, not in one pose — so the hover height is just a
       subtraction (-Y is up) and the boss never dips below it mid-flap.
       RBS_FOOT_OFF, which cancels the model's authored origin offset, belongs
       to the DRAW: folding it in here would make every other user of r->y (the
       hit box, the melee reach, the collision cylinder) silently wrong by that
       amount. */
    r->spawn_x = x;
    r->spawn_y = ground_y - RBS_HOVER;
    r->spawn_z = z;
    r->area    = area;
    rbs_reset_state(r);
    return i;
}

void rabisus_rest(void) {
    int i;
    for (i = 0; i < rabisu_count; i++) {
        Rabisu *r = &rabisus[i];
        if (!r->active || r->dead) continue;
        rbs_reset_state(r);
    }
    /* Fireballs are transient and area-scoped; a boss put back at its spawn
       must not still have one of its shots in the air. */
    rbs_fireballs_reset();
}

/* Kill every living Rabisu tagged to `area`, SILENTLY: no blood burst, no death
   sequence, no explosion. It is for a room being emptied by the story rather
   than by the player — the East Hall once FLAG_HADAD_TWO is set (east_hall.h) —
   and it runs during a transition, with nobody there to see or hear it.

   >>> BOTH FLAGS, NOT JUST `dying`. <<< A killing BLOW sets `dying` alone and
   hands the body to the encounter director for nine seconds of coming apart
   (rbs_damage_sfx). There is no director here and no one watching, so this skips
   straight to `dead` — the state that means "gone", which every loop skips,
   rabisus_rest() refuses to stand back up, and world.c's save delta records
   (rabisus_dead). */
void rabisus_kill_area(GameState area) {
    int i;
    for (i = 0; i < rabisu_count; i++) {
        Rabisu *r = &rabisus[i];
        if (!r->active || r->area != area || r->dead) continue;
        r->health = 0;
        r->dying  = 1;
        r->dead   = 1;
        /* Anything it had in the air dies with it — a shockwave mid-expansion
           would otherwise still be hitting the player on the far side of the
           transition. */
        rabisu_go_dormant(r);
    }
}

Rabisu *rabisu_boss_instance(void) {
    int i;
    for (i = 0; i < rabisu_count; i++) {
        Rabisu *r = &rabisus[i];
        if (r->active && !r->dead && r->area == current_area) return r;
    }
    return NULL;
}

void rabisu_go_dormant(Rabisu *r) {
    r->ai_state   = RBS_AI_DORMANT;
    r->ai_timer   = 0;
    r->lean       = 0;
    /* Every attack in flight is cancelled, not just the projectiles: a
       shockwave still expanding through a death cutscene would carry on
       damaging the player while the camera was somewhere else entirely. */
    r->wave_t     = 0;
    r->beam_cells = 0;
    r->beam_step  = 0;
    rbs_fireballs_reset();
    /* Including the beam's charge tell, which is only ever cut by the first
       poly lighting — a boss killed mid-charge would otherwise carry eleven
       seconds of it into the death cutscene. Safe on the reveal's path too:
       begin_reveal calls this BEFORE the reveal plays its own EMERGE. */
    sound_stop(SFX_EMERGE);
}

void rabisu_face_override(Rabisu *r, int on, int32_t x, int32_t z) {
    r->face_ovr   = on ? 1 : 0;
    r->face_ovr_x = x;
    r->face_ovr_z = z;
}

void rabisu_fight_begin(Rabisu *r) {
    if (r->dying || r->dead) return;
    r->face_ovr     = 0;   /* eyes back on the player */
    r->ai_state     = RBS_AI_MOVE;
    r->ai_timer     = 0;
    r->sweep        = 0;
    r->lean         = 0;
    r->moves_done   = 0;
    r->moves_target = RBS_MOVES_MIN +
                      (int32_t)(rbs_rand() % (RBS_MOVES_MAX - RBS_MOVES_MIN + 1));
    /* First leg goes to one lip or the other — never "to the spawn", which is
       where it already is. */
    r->sweep_from   = 0;   /* == r->sweep, set just above */
    r->sweep_target = (rbs_rand() & 1) ? 4096 : -4096;
}

/* Flame Rounds do double damage; a standard round and a deflected fireball both
   do 1, so 20 HP is 20 standard rounds or 10 flame rounds. Append
   another { DMG_*, percent } line to give it a second weakness (damage.h). */
static const Weakness rabisu_weakness[] = {
    { DMG_FLAME, 200 },
};

int32_t rabisu_scale_damage(int32_t base, DamageType type) {
    return damage_scale(base, type, rabisu_weakness,
                        WEAKNESS_COUNT(rabisu_weakness));
}

/* The real entry point. `hit_sfx` is what a NON-FATAL hit sounds like, and it
   is a parameter for one reason: a hit is announced by the thing that landed
   it, not by the thing it landed on. A bullet is the axe-hit thud; a fireball
   turned back into its owner's chest is the fireball's own report again. The
   killing blow makes no sound of its own either way — SFX_EXPLODE belongs to
   the burn, six seconds later (rabisu_boss.c). */
static void rbs_damage_sfx(Rabisu *r, int dmg, SfxID hit_sfx) {
    if (!r->active || r->dead || r->dying) return;
    r->health   -= dmg;
    r->hit_timer = RBS_BAR_TIMER_MAX;
    if (r->health <= 0) {
        r->health = 0;
        /* `dying`, NOT `dead`. The body has to stay drawable: the encounter
           director now takes it over for a nine-second death sequence and
           only sets `dead` once it has finished burning away. Setting `dead`
           here would blink the boss out of existence on the killing shot. */
        r->dying    = 1;
        /* Everything it had in the air dies with it — including a shockwave
           mid-expansion, which would otherwise carry on hitting the player
           through the first seconds of the death cutscene. */
        rabisu_go_dormant(r);
        /* Burst at mid-body, not at the anchor: the anchor is the underside, so
           a burst there would spray from beneath its feet. */
        spawn_blood_burst(r->x, r->y - RBS_HALF_H, r->z);
        /* SFX_EXPLODE is NOT played here. It belongs to the burn — the phase
           where the death lights come up — and rabisu_boss.c fires it on the
           entry to RBE_D_BURN. Played on the killing blow instead, the 5.4 s
           clip ran out under the settle and the freeze and the body burned in
           silence. */
    } else {
        sound_play(hit_sfx);
    }
}

void rabisu_damage(Rabisu *r, int dmg) {
    rbs_damage_sfx(r, dmg, SFX_AXEHIT);
}

void rabisu_body(const Rabisu *r, int32_t *cyc, int32_t *hh, int32_t *hw) {
    /* Head, wings and torso only: the box stops at the hips, so the legs and
       the tail cannot be hit at all. All four numbers come off the mesh in
       rabisu.h — see RBS_HIT_TOP_VY for the derivation and for which two to
       turn if the fight needs to be harder or easier. */
    *cyc = r->y + RBS_HIT_CY_OFF;
    *hh  = RBS_HIT_HALF_H;
    *hw  = RBS_HIT_HALF_W;
}

/* ===========================================================================
 * SHARED ADDITIVE-GLOW HELPERS
 * ===========================================================================
 * See the block comment in rabisu.h. Everything the encounter and the boss's
 * attacks light up goes through these, so the reveal, the beam and the death
 * cannot drift into looking like three different effects.
 *
 * The blend is ABR=1, additive, and the DR_TPAGE that selects it is added to
 * the SAME OT bucket immediately AFTER its poly — the OT is LIFO, so "after"
 * is what puts it in front. Same pattern as lightswitch_puzzle.c's ls_quad,
 * which this is descended from. */

int32_t rbs_glow_clock = 0;

/* White -> orange -> red -> white, one leg per RBS_PULSE_STEP frames. */
static const uint8_t RBS_PULSE_RGB[3][3] = {
    { 255, 250, 235 },   /* white  */
    { 255, 145,  30 },   /* orange */
    { 255,  35,  10 },   /* red    */
};
#define RBS_PULSE_STEP        26

void rbs_glow_pulse(int32_t clock, int32_t level,
                    uint8_t *r, uint8_t *g, uint8_t *b) {
    if (clock < 0) clock = -clock;
    if (level < 0) level = 0;
    if (level > 256) level = 256;
    int32_t leg = (clock / RBS_PULSE_STEP) % 3;
    int32_t f   = ((clock % RBS_PULSE_STEP) * 256) / RBS_PULSE_STEP;
    const uint8_t *a = RBS_PULSE_RGB[leg];
    const uint8_t *c = RBS_PULSE_RGB[(leg + 1) % 3];
    int32_t rr = (a[0] * (256 - f) + c[0] * f) >> 8;
    int32_t gg = (a[1] * (256 - f) + c[1] * f) >> 8;
    int32_t bb = (a[2] * (256 - f) + c[2] * f) >> 8;
    *r = (uint8_t)((rr * level) >> 8);
    *g = (uint8_t)((gg * level) >> 8);
    *b = (uint8_t)((bb * level) >> 8);
}

void rbs_glow_quad(RenderContext *ctx, const SVECTOR v[4],
                   uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) return;

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
    otz += 40;   /* the room mesh's own bias, so a wall in front still occludes */
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

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

    DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
    addPrim(&ot[otz], tp);
    ctx->next_packet += sizeof(DR_TPAGE);
}

#define RBS_GLOW_WORLD       230   /* world half-size of the outermost square */
#define RBS_GLOW_RINGS         3

void rbs_glow_point(RenderContext *ctx, const VECTOR *at, int32_t bright,
                    int32_t clock) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    SVECTOR pt;
    pt.vx = (int16_t)at->vx; pt.vy = (int16_t)at->vy; pt.vz = (int16_t)at->vz;
    pt.pad = 0;

    DVECTOR sv;
    int32_t sz;
    gte_ldv0(&pt);
    gte_rtps();
    gte_stsxy(&sv);
    gte_stsz(&sz);
    if (sz == 0) return;
    if (sv.vx <= -1023 || sv.vx >= 1023 || sv.vy <= -1023 || sv.vy >= 1023) return;

    int32_t dx = at->vx - cam_x, dy = at->vy - cam_y, dz = at->vz - cam_z;
    int32_t dist = isqrt32(dx * dx + dy * dy + dz * dz);
    if (dist < 64) dist = 64;

    int32_t otz = sz >> 2;
    if (otz <= SCENE_OT_MIN) otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;

    int ring;
    for (ring = 0; ring < RBS_GLOW_RINGS; ring++) {
        if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) return;

        /* Outermost ring is full width and dimmest; each step in halves the
           square and raises the level, so the three add up to a soft falloff
           with a hot core. */
        int32_t world_half = RBS_GLOW_WORLD >> ring;
        int32_t half = (world_half * 256) / dist;   /* gte_SetGeomScreen(256) */
        if (half < 1)   half = 1;
        if (half > 400) half = 400;

        uint8_t r, g, b;
        rbs_glow_pulse(clock + ring * 5, (bright * (60 + ring * 70)) >> 8,
                       &r, &g, &b);

        POLY_F4 *p = (POLY_F4 *)ctx->next_packet;
        setPolyF4(p);
        setSemiTrans(p, 1);
        setRGB0(p, r, g, b);
        p->x0 = (int16_t)(sv.vx - half); p->y0 = (int16_t)(sv.vy - half);
        p->x1 = (int16_t)(sv.vx + half); p->y1 = (int16_t)(sv.vy - half);
        p->x2 = (int16_t)(sv.vx - half); p->y2 = (int16_t)(sv.vy + half);
        p->x3 = (int16_t)(sv.vx + half); p->y3 = (int16_t)(sv.vy + half);
        addPrim(&ot[otz], p);
        ctx->next_packet += sizeof(POLY_F4);

        DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
        addPrim(&ot[otz], tp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
}

/* Brightness split between the pool on the ground and the shaft above it. The
   shaft is faint because sixteen of them overlap during the reveal and would
   otherwise white out the middle of the garden. */
#define RBS_POOL_LEVEL       210
#define RBS_SHAFT_LEVEL       60
#define RBS_SHAFT_H          900   /* how far up the beam reaches               */
#define RBS_SHAFT_FLARE      150   /* how far out each corner is pushed at the top */
#define RBS_SHAFT_SEGS         3

void rbs_glow_pillar(RenderContext *ctx, int32_t x0, int32_t x1,
                     int32_t z0, int32_t z1, int32_t floor_y,
                     int32_t bright, int32_t clock) {
    uint8_t r, g, b;
    SVECTOR v[4];
    int k;
    for (k = 0; k < 4; k++) v[k].pad = 0;

    /* --- The pool, 4 above the surface so it never z-fights the floor poly --- */
    rbs_glow_pulse(clock, (bright * RBS_POOL_LEVEL) >> 8, &r, &g, &b);
    v[0].vx = (int16_t)x0; v[0].vy = (int16_t)(floor_y - 4); v[0].vz = (int16_t)z0;
    v[1].vx = (int16_t)x1; v[1].vy = (int16_t)(floor_y - 4); v[1].vz = (int16_t)z0;
    v[2].vx = (int16_t)x0; v[2].vy = (int16_t)(floor_y - 4); v[2].vz = (int16_t)z1;
    v[3].vx = (int16_t)x1; v[3].vy = (int16_t)(floor_y - 4); v[3].vz = (int16_t)z1;
    rbs_glow_quad(ctx, v, r, g, b);

    /* --- The shaft: four walls, FLARING as it rises so the beam widens out of
           the poly it is pouring from. Banded along its length both so no
           single quad can blow past the GTE's +/-1023 screen clamp and so the
           brightness can fall off with height — a beam lit evenly top to
           bottom looks like a solid box, and the point is that its source is
           the ground. --- */
    {
        const int32_t cx[4] = { x0, x1, x1, x0 };
        const int32_t cz[4] = { z0, z0, z1, z1 };
        const int32_t mx = (x0 + x1) / 2, mz = (z0 + z1) / 2;
        int s, t;
        for (t = 0; t < RBS_SHAFT_SEGS; t++) {
            int32_t e0 = t, e1 = t + 1;
            int32_t y0 = floor_y - (RBS_SHAFT_H * e0) / RBS_SHAFT_SEGS;
            int32_t y1 = floor_y - (RBS_SHAFT_H * e1) / RBS_SHAFT_SEGS;
            int32_t o0 = (RBS_SHAFT_FLARE * e0) / RBS_SHAFT_SEGS;
            int32_t o1 = (RBS_SHAFT_FLARE * e1) / RBS_SHAFT_SEGS;

            int32_t drop = 256 - (170 * e0) / RBS_SHAFT_SEGS;
            rbs_glow_pulse(clock, (((bright * RBS_SHAFT_LEVEL) >> 8) * drop) >> 8,
                           &r, &g, &b);

            for (s = 0; s < 4; s++) {
                int n = (s + 1) & 3;
                /* Away from the centre, not toward it: this is light coming
                   OUT of the ground, and which end of a cone is wide is the
                   whole read. */
                #define PUSHX(c, o) ((c) + ((mx - (c)) > 0 ? -(o) : (o)))
                #define PUSHZ(c, o) ((c) + ((mz - (c)) > 0 ? -(o) : (o)))
                v[0].vx = (int16_t)PUSHX(cx[s], o0); v[0].vz = (int16_t)PUSHZ(cz[s], o0); v[0].vy = (int16_t)y0;
                v[1].vx = (int16_t)PUSHX(cx[n], o0); v[1].vz = (int16_t)PUSHZ(cz[n], o0); v[1].vy = (int16_t)y0;
                v[2].vx = (int16_t)PUSHX(cx[s], o1); v[2].vz = (int16_t)PUSHZ(cz[s], o1); v[2].vy = (int16_t)y1;
                v[3].vx = (int16_t)PUSHX(cx[n], o1); v[3].vz = (int16_t)PUSHZ(cz[n], o1); v[3].vy = (int16_t)y1;
                #undef PUSHX
                #undef PUSHZ
                rbs_glow_quad(ctx, v, r, g, b);
            }
        }
    }
}

int32_t rbs_floor_y_at(int32_t x, int32_t z, int32_t fallback) {
    int i;
    for (i = 0; i < floor_zone_count; i++) {
        FloorZone *f = &floor_zones[i];
        if (x < f->min_x || x > f->max_x) continue;
        if (z < f->min_z || z > f->max_z) continue;
        return f->y;
    }
    return fallback;
}

/* ===========================================================================
 * FIREBALLS
 * ===========================================================================
 * The boss's projectile. Modelled on the spider's web (src/web.c) — same
 * straight-line flight, same segment-based wall test so a fast shot cannot
 * tunnel through a thin wall between frames — with the one behaviour a web
 * never has: it can be turned around.
 *
 * Transient, so unlike the bosses themselves these are NOT in the save blob.
 * A save taken mid-fight reloads with the air clear, which is the forgiving
 * reading and the same one webs already get. */
typedef struct {
    int32_t   x, y, z;
    int32_t   vx, vy, vz;
    int32_t   life;        /* frames left before it fizzles; 0 = free slot     */
    int32_t   age;         /* frames flown, so the parry window can be timed   */
    int32_t   deflected;   /* 1 = turned around and hunting its owner          */
    int32_t   owner;       /* index into rabisus[] — who eats it if deflected  */
    GameState area;
} RbsFireball;

static RbsFireball rbs_fireballs[MAX_RBS_FIREBALLS];

void rbs_fireballs_reset(void) {
    int i;
    for (i = 0; i < MAX_RBS_FIREBALLS; i++) rbs_fireballs[i].life = 0;
}

/* Is any of this boss's fireballs still in the air? RBS_AI_FIRE waits on it,
   which is what makes the attack "fully resolve" before the sweep resumes —
   including the extra second a deflected ball spends flying home. */
static int rbs_fireball_in_flight(int owner) {
    int i;
    for (i = 0; i < MAX_RBS_FIREBALLS; i++)
        if (rbs_fireballs[i].life > 0 && rbs_fireballs[i].owner == owner) return 1;
    return 0;
}

static void rbs_fireball_spawn(int owner, int32_t x, int32_t y, int32_t z,
                               int32_t tx, int32_t ty, int32_t tz,
                               GameState area) {
    int i;
    for (i = 0; i < MAX_RBS_FIREBALLS; i++)
        if (rbs_fireballs[i].life <= 0) break;
    if (i >= MAX_RBS_FIREBALLS) return;

    int32_t dx = tx - x, dy = ty - y, dz = tz - z;
    int32_t len = isqrt32(dx * dx + dy * dy + dz * dz);
    if (len <= 0) return;

    RbsFireball *f = &rbs_fireballs[i];
    f->x = x; f->y = y; f->z = z;
    /* Speed is set by the FLIGHT TIME, not the other way round: the attack is
       specified as half a second regardless of how far away the player is
       standing, and the parry window is only fair if the arrival is
       predictable. Dividing the whole distance by RBS_FB_FLIGHT gives that. */
    f->vx = (dx / RBS_FB_FLIGHT);
    f->vy = (dy / RBS_FB_FLIGHT);
    f->vz = (dz / RBS_FB_FLIGHT);
    /* Outlives the nominal arrival so a DODGED ball keeps sailing past and
       fizzles in the open rather than vanishing at the player's shoulder. */
    f->life      = RBS_FB_FLIGHT * 2;
    f->age       = 0;
    f->deflected = 0;
    f->owner     = owner;
    f->area      = area;
    sound_play(SFX_FIREBALL);   /* the ball leaving the chest */
}

/* Turn a ball around: double speed, straight back at the chest of the thing
   that threw it. It re-aims once, here, and then flies straight — a homing
   return would make the deflect a guaranteed hit rather than a good one. */
static void rbs_fireball_deflect(RbsFireball *f) {
    Rabisu *r = &rabisus[f->owner];
    VECTOR chest;
    rabisu_anchor_world(r, RBS_A_CHEST_X, RBS_A_CHEST_Y, RBS_A_CHEST_Z, &chest);

    int32_t dx = chest.vx - f->x, dy = chest.vy - f->y, dz = chest.vz - f->z;
    int32_t len = isqrt32(dx * dx + dy * dy + dz * dz);
    if (len <= 0) { f->life = 0; return; }

    int32_t speed = (RBS_FB_FLIGHT / 2);   /* halve the frames = double the pace */
    if (speed < 1) speed = 1;
    f->vx = (dx / speed);
    f->vy = (dy / speed);
    f->vz = (dz / speed);
    f->deflected = 1;
    f->life      = speed * 3;   /* generous: it only has to cross once */
    sound_play(SFX_AXEHIT);
}

void rbs_fireballs_update(void) {
    int i;
    for (i = 0; i < MAX_RBS_FIREBALLS; i++) {
        RbsFireball *f = &rbs_fireballs[i];
        if (f->life <= 0) continue;
        if (f->area != current_area) { f->life = 0; continue; }
        if (--f->life <= 0) continue;
        f->age++;

        int32_t ox = f->x, oy = f->y, oz = f->z;
        f->x += f->vx;
        f->y += f->vy;
        f->z += f->vz;

        if (collision_segment_blocked(ox, oy, oz, f->x, f->y, f->z)) {
            f->life = 0;
            continue;
        }

        if (f->deflected) {
            /* Homeward leg: the only thing it can hit is its owner. */
            Rabisu *r = &rabisus[f->owner];
            if (r->active && !r->dead && !r->dying) {
                int32_t hx = r->x - f->x;
                int32_t hy = (r->y - RBS_HALF_H) - f->y;
                int32_t hz = r->z - f->z;
                int32_t d  = isqrt32(hx * hx + hy * hy + hz * hz);
                if (d <= RBS_FB_RBS_RADIUS) {
                    f->life = 0;
                    spawn_blood_burst(f->x, f->y, f->z);
                    /* Its own shot going back in: the fireball's report, not
                       the gun's thud. The ball is spent on this frame, so the
                       clip that opened its flight also closes it. */
                    rbs_damage_sfx(r, 1, SFX_FIREBALL);
                }
            } else {
                f->life = 0;   /* it died to something else mid-return */
            }
            continue;
        }

        /* --- Outbound leg -----------------------------------------------
           THE DEFLECT. Live only in the last RBS_FB_PARRY frames before the
           nominal arrival, which is the window the player is being asked to
           read: swing as it closes, not when it is halfway across the garden.
           swing_timer counts 1..SWING_TOTAL from the frame Square was pressed
           (crucifaxe.c), so "swing_timer is inside the window" is exactly
           "a swing was started within 0.2 s of contact". It is only ever
           non-zero with the axe equipped, so no weapon check is needed. */
        if (f->age >= RBS_FB_FLIGHT - RBS_FB_PARRY &&
            swing_timer > 0 && swing_timer <= RBS_FB_PARRY) {
            rbs_fireball_deflect(f);
            continue;
        }

        /* Contact. Radial in all three axes, as the web's is and for the same
           reason: the shot was aimed at the player's anchor, so dy closes to
           nothing on arrival and a flat XZ test would count a ball that sailed
           over their head. Missing this sphere IS the dodge. */
        if (!game_over) {
            /* Tested against the SAME dropped point it was aimed at, so the
               ball's flight and its hit sphere agree. Testing the eye while
               aiming below it would quietly shrink the effective radius. */
            int32_t hx = player_x() - f->x;
            int32_t hy = (player_y() + RBS_FB_AIM_DROP) - f->y;
            int32_t hz = player_z() - f->z;
            if (hx * hx + hy * hy + hz * hz <=
                (int32_t)RBS_FB_HIT_RADIUS * RBS_FB_HIT_RADIUS) {
                f->life = 0;
                player_hurt(RBS_FB_DAMAGE);
                sound_play(SFX_HURT);
                if (player_health <= 0) {
                    player_health = 0;
                    game_over     = 1;
                    flash_timer   = 90;
                    sound_play(SFX_DIE);
                }
            }
        }
    }
}

/* ===========================================================================
 * COMBAT AI
 * =========================================================================== */

/* The world point the arc puts the boss at for a given sweep value.
 *
 * The sweep is a LATERAL offset, measured perpendicular to the line from the
 * player to the boss's spawn, and +/-4096 maps to +/-RBS_SWEEP_RADIUS — the
 * distance from the spawn to each retaining lip. What makes the path an ARC
 * rather than a straight slide is the other component: the boss holds its
 * DISTANCE from the player throughout, so the along-axis component shortens as
 * the lateral one grows (a^2 + off^2 = base^2) and the path bows toward the
 * player at its ends.
 *
 * Doing it this way, rather than bending a straight-line target back onto the
 * circle, is what keeps the ends of the sweep exactly on the lips: the lateral
 * offset is an input here, not something left over after a normalisation.
 */
static void rbs_arc_point(const Rabisu *r, int32_t sweep,
                          int32_t *out_x, int32_t *out_z) {
    int32_t px = player_x(), pz = player_z();
    int32_t sx = r->spawn_x, sz = r->spawn_z;

    int32_t bx = sx - px, bz = sz - pz;
    int32_t base = isqrt32(bx * bx + bz * bz);
    if (base <= 0) { *out_x = sx; *out_z = sz; return; }

    int32_t off = (sweep * RBS_SWEEP_RADIUS) >> 12;
    /* Stand close enough and the arc has nowhere to go: the lateral reach can
       never exceed the radius of the circle it is measured on. Clamping here
       degenerates the sweep into a tight orbit around the player, which is the
       sensible thing for it to do when they have walked right up to it. */
    if (off >  base) off =  base;
    if (off < -base) off = -base;

    int32_t along = isqrt32(base * base - off * off);

    /* Unit basis: `b` toward the spawn, `perp` 90deg from it in XZ. */
    int32_t tx = px + (bx * along) / base + ( bz * off) / base;
    int32_t tz = pz + (bz * along) / base + (-bx * off) / base;

    /* Never over a lip, and never off the lawn. The arc is sized so the centre
       just touches each lip, but the player is free to stand somewhere that
       swings it wide, and a boss parked over the one-way step would be a boss
       the player has to jump down to shoot at. */
    if (tx < RBS_ARENA_MIN_X) tx = RBS_ARENA_MIN_X;
    if (tx > RBS_ARENA_MAX_X) tx = RBS_ARENA_MAX_X;
    if (tz < RBS_ARENA_MIN_Z) tz = RBS_ARENA_MIN_Z;
    if (tz > RBS_ARENA_MAX_Z) tz = RBS_ARENA_MAX_Z;

    *out_x = tx;
    *out_z = tz;
}

/* Pick the next stopping point. The three are the two lips and the spawn, and
   it must differ from where it is standing — "moves from 1 spot to the other"
   has no reading in which staying put counts as a move. */
/* How far the body is currently dropped below its rest height, 0 at either end
   of the traversal and RBS_SWOOP_DIP at the midpoint. See RBS_SWOOP_DIP in
   rabisu.h for why it is a half sine and not a triangle.

   isin's period is 4096, so mapping the traversal's 0..4096 progress onto
   0..2048 — a plain >> 1 — walks exactly half a period: up from zero, over the
   peak at the midpoint, back to zero on arrival. */
static int32_t rbs_swoop_dip(const Rabisu *r) {
    int32_t span = r->sweep_target - r->sweep_from;
    if (span < 0) span = -span;
    if (span == 0) return 0;          /* holding a stop, or centred */

    int32_t done = r->sweep - r->sweep_from;
    if (done < 0) done = -done;
    if (done > span) done = span;     /* the last step lands ON the target */

    int32_t t = (done << 12) / span;  /* 0..4096 through the traversal */
    return (RBS_SWOOP_DIP * isin(t >> 1)) >> 12;
}

static void rbs_pick_target(Rabisu *r) {
    static const int32_t STOP[3] = { -4096, 0, 4096 };
    int32_t opts[2];
    int i, n = 0;
    for (i = 0; i < 3; i++)
        if (STOP[i] != r->sweep_target && n < 2) opts[n++] = STOP[i];
    /* Always exactly two: it is standing on one of the three. The n < 2 guard
       is only there so a restored save that somehow held a fourth value could
       not walk off the end of opts[]. */
    r->sweep_from   = r->sweep;   /* where this traversal starts — the swoop's
                                     other end. Set BEFORE the new target, and
                                     from the live sweep rather than the old
                                     target, so a leg interrupted part-way still
                                     dips about its own true midpoint. */
    r->sweep_target = opts[rbs_rand() & 1];
}

/* How far the player is from the middle of the arena. The shockwave's trigger
   and the wave's own reach are both measured from the SPAWN, not from the
   boss: the boss is the thing that moves to the middle, so using its live
   position would make the zone follow it around. */
static int32_t rbs_player_from_centre(const Rabisu *r) {
    int32_t dx = player_x() - r->spawn_x;
    int32_t dz = player_z() - r->spawn_z;
    return isqrt32(dx * dx + dz * dz);
}

/* Give up sweeping and go and sit in the middle: the player came inside. */
static void rbs_begin_centre(Rabisu *r) {
    r->ai_state     = RBS_AI_CENTRE;
    r->ai_timer     = 0;
    r->lean         = 0;
    r->slash_from_x = r->x;
    r->slash_from_z = r->z;
    /* Y as well, and it is NOT redundant now the sweep swoops: this can fire
       on any frame of a traversal, including the deepest one, so the boss may
       be up to RBS_SWOOP_DIP below its rest height when it is told to centre.
       Without this the CENTRE ease would slide it home at whatever height the
       dive had reached and leave it there for the whole shockwave. */
    r->slash_from_y = r->y;
}

/* The stop is over: throw something. Fireball 2, foot slash 1, light beam 1. */
static void rbs_begin_attack(Rabisu *r, int self) {
    uint32_t roll = rbs_rand() % RBS_ATTACK_ROLL;

    if (roll == RBS_SLASH_FACE) {
        /* At the START of the RBS_SLASH_IN charge — "about to swing". That is
           the 1 s wind-up, so the 1.2 s clip runs right through it and lands
           with rbs_slash_land: the sound is the tell the parry is timed off,
           and putting it on the landing instead would be a sound that arrives
           after the window it warns about has closed. It is the player's own
           crucifaxe clip — the same kind of event and the same arc of air — but
           played through SFX_RBS_SWING, which is that sample on a voice of its
           own so the player swinging cannot silence the tell. */
        sound_play(SFX_RBS_SWING);
        r->ai_state    = RBS_AI_SLASH_IN;
        r->ai_timer    = 0;
        r->slash_from_x = r->x;
        r->slash_from_z = r->z;
        return;
    }

    if (roll == RBS_BEAM_FACE) {
        /* The charge's tell, on the frame the chest starts burning. It is the
           reveal's own clip — the same thing dragging the same light up out of
           the lawn — and at 11.1 s it is far longer than the 1.5 s charge, so
           it is CUT deliberately by the first poly igniting (rbs_beam_ignite):
           the player hears the rising half and the detonation takes over. It is
           BANKED (SND_BANK_BOSS, see sound.h) and audible here only because the
           whole fight happens inside the boss bank. Its dedicated voice 19 means
           nothing else can silence it early, and rabisu_go_dormant stops it for
           the case where the boss is killed mid-charge. */
        sound_play(SFX_EMERGE);
        /* Charge only. The path is not drawn until the charge ENDS — a path
           locked in now would be aimed at where the player stood 1.5 s before
           the first poly lit, and the tell would be pointless. */
        r->ai_state = RBS_AI_BEAM_CHARGE;
        r->ai_timer = 0;
        return;
    }

    VECTOR chest;
    rabisu_anchor_world(r, RBS_A_CHEST_X, RBS_A_CHEST_Y, RBS_A_CHEST_Z, &chest);
    rbs_fireball_spawn(self, chest.vx, chest.vy, chest.vz,
                       player_x(), player_y() + RBS_FB_AIM_DROP, player_z(),
                       r->area);
    r->ai_state = RBS_AI_FIRE;
    /* A ceiling, not the schedule: the state really ends when the ball is gone
       (see below). This only covers the case where the pool was full and no
       ball was ever spawned, which would otherwise hang the boss forever. */
    r->ai_timer = RBS_FB_FLIGHT * 4;
}

/* ---- The light beam's path --------------------------------------------------
   Laid out BACKWARD from the player at the lawn's own ~286 poly pitch, so the
   LAST cell is centred exactly on where they stood and the rest march back
   toward the boss. Doing it forward from the boss instead would leave the
   final cell wherever the division happened to land, and "the last one lands
   on the poly the player is standing on" is the whole point of the attack. */
static void rbs_beam_draw_path(Rabisu *r) {
    r->beam_px = player_x();
    r->beam_pz = player_z();
    r->beam_bx = r->x;
    r->beam_bz = r->z;

    int32_t dx = r->beam_px - r->beam_bx;
    int32_t dz = r->beam_pz - r->beam_bz;
    int32_t d  = isqrt32(dx * dx + dz * dz);

    int32_t n = d / RBS_BEAM_CELL + 1;
    if (n < 2)                  n = 2;
    if (n > RBS_BEAM_MAX_CELLS) n = RBS_BEAM_MAX_CELLS;
    r->beam_cells = n;
    r->beam_step  = 0;
    r->ai_state   = RBS_AI_BEAM_WALK;
    r->ai_timer   = 0;
}

/* Centre of path cell k, in world XZ. k = beam_cells-1 is the player's own
   cell; k counts back toward the boss from there. */
static void rbs_beam_cell_centre(const Rabisu *r, int k,
                                 int32_t *out_x, int32_t *out_z) {
    int32_t dx = r->beam_bx - r->beam_px;
    int32_t dz = r->beam_bz - r->beam_pz;
    int32_t d  = isqrt32(dx * dx + dz * dz);
    int32_t back = (int32_t)(r->beam_cells - 1 - k) * RBS_BEAM_CELL;
    if (d <= 0) { *out_x = r->beam_px; *out_z = r->beam_pz; return; }
    *out_x = r->beam_px + (dx * back) / d;
    *out_z = r->beam_pz + (dz * back) / d;
}

/* One poly of the path ignites. It burns whoever is standing on it — which is
   checked NOW, against the player's live position, not against where the path
   was drawn. Stepping off the line beats it; running along it does not. */
static void rbs_beam_ignite(Rabisu *r, int k) {
    int32_t cx, cz;
    rbs_beam_cell_centre(r, k, &cx, &cz);

    /* The charge tell is over the moment the first shaft comes up: EMERGE runs
       11.1 s against a 1.5 s charge, so left alone it would still be swelling
       under the whole walk. Cut on the FIRST cell only — k>0 is the same attack
       continuing, and re-stopping it there would be a no-op anyway. */
    if (k == 0) sound_stop(SFX_EMERGE);

    /* Per POLY, not per attack, and before the hit test: every shaft that comes
       up out of the lawn makes the noise whether or not the player was standing
       on it. Retriggered every RBS_BEAM_STEP (0.3 s) on one dedicated voice, so
       each detonation is cut by the next and only the last plays its tail —
       see the note on SFX_BOOM in sound.h. */
    sound_play(SFX_BOOM);

    int32_t px = player_x() - cx;
    int32_t pz = player_z() - cz;
    if (px < 0) px = -px;
    if (pz < 0) pz = -pz;

    if (!game_over && px <= RBS_BEAM_CELL / 2 && pz <= RBS_BEAM_CELL / 2) {
        player_hurt(RBS_BEAM_DAMAGE);
        sound_play(SFX_HURT);
        if (player_health <= 0) {
            player_health = 0;
            game_over     = 1;
            flash_timer   = 90;
            sound_play(SFX_DIE);
        }
    }
}

/* ---- The shockwave ----------------------------------------------------------
   Ticked every frame regardless of what the AI is doing, so a wave already in
   the air still hunts the player down even if they have since left the zone
   and the boss has gone back to sweeping. Launching it is the decision; once
   it is out it is out. */
static void rbs_shock_tick(Rabisu *r) {
    if (r->wave_t <= 0) return;

    r->wave_t++;
    if (r->wave_t > RBS_SHOCK_EXPAND + RBS_SHOCK_LINGER) { r->wave_t = 0; return; }

    if (r->wave_hit) return;

    int32_t reach = r->wave_t >= RBS_SHOCK_EXPAND
                  ? RBS_SWEEP_RADIUS
                  : (RBS_SWEEP_RADIUS * r->wave_t) / RBS_SHOCK_EXPAND;
    if (rbs_player_from_centre(r) > reach) return;

    r->wave_hit = 1;
    if (game_over) return;
    player_hurt(RBS_SHOCK_DAMAGE);
    player_knockback(r->spawn_x, r->spawn_z, RBS_SHOCK_KNOCKBACK);
    sound_play(SFX_HURT);
    if (player_health <= 0) {
        player_health = 0;
        game_over     = 1;
        flash_timer   = 90;
        sound_play(SFX_DIE);
    }
}

/* Attack resolved (damage taken, blocked, or turned back) — sweep again. */
static void rbs_resume_sweep(Rabisu *r) {
    r->ai_state     = RBS_AI_MOVE;
    r->ai_timer     = 0;
    r->lean         = 0;
    r->beam_cells   = 0;   /* the path is spent; stop drawing it */
    r->beam_step    = 0;
    r->moves_done   = 0;
    r->moves_target = RBS_MOVES_MIN +
                      (int32_t)(rbs_rand() % (RBS_MOVES_MAX - RBS_MOVES_MIN + 1));
    rbs_pick_target(r);
}

static void rbs_slash_land(Rabisu *r) {
    /* The 0.4 s block. Twice the fireball's window because unlike the fireball
       this attack cannot be sidestepped — the charge re-aims every frame — so
       the parry is the ONLY answer to it and has to be catchable. */
    if (swing_timer > 0 && swing_timer <= RBS_SLASH_PARRY) {
        /* A block, not a riposte: no damage either way. It still SHOVES,
           though, and loudly — otherwise a blocked slash and a slash that
           somehow whiffed look and sound identical, and the player cannot tell
           that the parry is what saved them. */
        sound_play(SFX_AXEHIT);
        player_knockback(r->x, r->z, RBS_SLASH_KNOCKBACK / 2);
    } else if (!game_over) {
        player_hurt(RBS_SLASH_DAMAGE);
        player_knockback(r->x, r->z, RBS_SLASH_KNOCKBACK);
        sound_play(SFX_HURT);
        if (player_health <= 0) {
            player_health = 0;
            game_over     = 1;
            flash_timer   = 90;
            sound_play(SFX_DIE);
        }
    }
    r->ai_state = RBS_AI_SLASH_BACK;
    r->ai_timer = 0;
}

static void rbs_update_ai(Rabisu *r, int self) {
    switch (r->ai_state) {
    case RBS_AI_DORMANT:
        break;

    case RBS_AI_MOVE: {
        int32_t step = (4096 + RBS_SWEEP_FRAMES - 1) / RBS_SWEEP_FRAMES;
        if (r->sweep < r->sweep_target) {
            r->sweep += step;
            if (r->sweep > r->sweep_target) r->sweep = r->sweep_target;
        } else if (r->sweep > r->sweep_target) {
            r->sweep -= step;
            if (r->sweep < r->sweep_target) r->sweep = r->sweep_target;
        }
        rbs_arc_point(r, r->sweep, &r->x, &r->z);
        /* ...and the dive. Written from spawn_y every frame rather than
           accumulated, so the height cannot drift over a long fight, and
           whatever owned r->y before this state (the reveal's rise, the foot
           slash's return leg) is cleanly taken back over. It reaches 0 again on
           arrival, so every state that follows this one still starts at rest
           height without having to reset anything. */
        r->y = r->spawn_y + rbs_swoop_dip(r);

        /* The player walked into the middle: abandon the sweep. Checked here
           and in RBS_AI_PAUSE only, so a fireball or a charge already underway
           gets to finish rather than being cut off mid-lunge. */
        if (rbs_player_from_centre(r) < RBS_SWEEP_RADIUS) { rbs_begin_centre(r); break; }

        if (r->sweep == r->sweep_target) {
            /* Arrived at a stopping point. Either set off again, or — once the
               quota is met — plant and wind up. */
            if (++r->moves_done >= r->moves_target) {
                r->ai_state = RBS_AI_PAUSE;
                r->ai_timer = RBS_STOP_PAUSE;
            } else {
                rbs_pick_target(r);
            }
        }
        break;
    }

    case RBS_AI_PAUSE:
        /* Held dead still on the spot it arrived at. Keep re-solving the arc
           point so it still tracks a player who walks around it — it is
           stationary along its OWN arc, not pinned to a patch of grass. */
        rbs_arc_point(r, r->sweep, &r->x, &r->z);
        if (rbs_player_from_centre(r) < RBS_SWEEP_RADIUS) { rbs_begin_centre(r); break; }
        if (--r->ai_timer <= 0) rbs_begin_attack(r, self);
        break;

    case RBS_AI_CENTRE: {
        r->ai_timer++;
        int32_t t = (r->ai_timer * 256) / RBS_SHOCK_CENTRE_F; if (t > 256) t = 256;
        int32_t inv = 256 - t;
        int32_t e = 256 - (inv * inv / 256);          /* ease-out */
        r->x = r->slash_from_x + ((r->spawn_x - r->slash_from_x) * e) / 256;
        r->z = r->slash_from_z + ((r->spawn_z - r->slash_from_z) * e) / 256;
        /* Climbing out of the dive on the same ease, so it rises as it draws
           in rather than snapping level at the far end. */
        r->y = r->slash_from_y + ((r->spawn_y - r->slash_from_y) * e) / 256;
        if (r->ai_timer >= RBS_SHOCK_CENTRE_F) {
            r->x = r->spawn_x;
            r->z = r->spawn_z;
            r->y = r->spawn_y;
            r->sweep        = 0;   /* the middle IS sweep 0, so the sweep can
                                      pick up cleanly if the player backs off */
            r->sweep_target = 0;
            r->sweep_from   = 0;   /* zero-span leg: the swoop sits at rest
                                      height while it holds the middle */
            r->ai_state     = RBS_AI_SHOCK;
            r->shock_timer  = RBS_SHOCK_PERIOD;
        }
        break;
    }

    case RBS_AI_SHOCK:
        /* Parked. Nothing moves it while the player is inside. */
        r->x = r->spawn_x;
        r->z = r->spawn_z;
        /* Leaving needs the hysteresis or a player standing exactly on the
           radius flips it between centring and sweeping every frame. */
        if (rbs_player_from_centre(r) > RBS_SWEEP_RADIUS + RBS_SHOCK_HYST) {
            rbs_resume_sweep(r);
            break;
        }
        if (--r->shock_timer <= 0) {
            r->shock_timer = RBS_SHOCK_PERIOD;
            r->wave_t      = 1;
            r->wave_hit    = 0;
            sound_play(SFX_SLAM);
        }
        break;

    case RBS_AI_BEAM_CHARGE:
        /* Stopped, chest burning, telegraphing hard. Still tracks its arc so a
           player circling it does not leave it facing a hedge. */
        rbs_arc_point(r, r->sweep, &r->x, &r->z);
        if (++r->ai_timer >= RBS_BEAM_CHARGE) rbs_beam_draw_path(r);
        break;

    case RBS_AI_BEAM_WALK:
        rbs_arc_point(r, r->sweep, &r->x, &r->z);
        /* ai_timer counts DOWN through the current poly's 0.3 s, and 0 means
           "nothing is lit yet". That covers the entry frame and every step
           boundary with the same two lines — an up-counter needed a separate
           "have I already fired cell 0" flag, and without one the walk relit
           the first poly every time the counter wrapped. */
        if (r->ai_timer == 0) {
            rbs_beam_ignite(r, r->beam_step);
            r->ai_timer = RBS_BEAM_STEP;
        }
        if (--r->ai_timer == 0) {
            if (++r->beam_step >= r->beam_cells) {
                r->beam_step  = 0;
                r->beam_cells = 0;
                rbs_resume_sweep(r);
            }
            /* else: left at 0, so the next frame lights the new poly */
        }
        break;

    case RBS_AI_FIRE:
        rbs_arc_point(r, r->sweep, &r->x, &r->z);
        /* The real exit: the shot is spent — it hit, it was dodged and
           fizzled, or it came back and bit its owner. The timer is only the
           backstop described in rbs_begin_attack. */
        if (!rbs_fireball_in_flight(self) || --r->ai_timer <= 0)
            rbs_resume_sweep(r);
        break;

    case RBS_AI_SLASH_IN: {
        r->ai_timer++;
        int32_t t = (r->ai_timer * 256) / RBS_SLASH_IN;
        if (t > 256) t = 256;

        /* Re-aimed every frame at a point RBS_SLASH_STANDOFF short of the
           player, which is what "cannot be dodged" means mechanically: walking
           aside moves the destination rather than escaping it. */
        int32_t px = player_x(), pz = player_z();
        int32_t ax = r->slash_from_x - px, az = r->slash_from_z - pz;
        int32_t alen = isqrt32(ax * ax + az * az);
        int32_t tx = px, tz = pz;
        if (alen > 0) {
            tx = px + (ax * RBS_SLASH_STANDOFF) / alen;
            tz = pz + (az * RBS_SLASH_STANDOFF) / alen;
        }
        /* Ease-IN, the mirror of the camera glides' ease-out: it gathers pace
           as it comes, so the last few frames — the ones the parry is timed
           against — are the fastest and read as a strike, not a drift. */
        int32_t e = (t * t) / 256;
        r->x = r->slash_from_x + ((tx - r->slash_from_x) * e) / 256;
        r->z = r->slash_from_z + ((tz - r->slash_from_z) * e) / 256;
        /* Rise to meet them. Without this the charge holds its hover height —
           underside on the terrace floor — and the feet arrive below the
           bottom of the screen, which is the difference between an attack the
           player sees coming and one that just takes 20% off the bar. */
        {
            int32_t ty = player_y() + RBS_SLASH_RISE_TO;
            r->y = r->spawn_y + ((ty - r->spawn_y) * e) / 256;
        }
        r->lean = (RBS_SLASH_LEAN * e) / 256;

        if (r->ai_timer >= RBS_SLASH_IN) rbs_slash_land(r);
        break;
    }

    case RBS_AI_SLASH_BACK: {
        r->ai_timer++;
        int32_t t = (r->ai_timer * 256) / RBS_SLASH_BACK;
        if (t > 256) t = 256;
        int32_t inv = 256 - t;
        int32_t e = 256 - (inv * inv / 256);          /* ease-out */

        /* Home is the LIVE arc point for the sweep value it stopped on, not
           the world spot it launched from. Those are the same place if the
           player has not moved, and if they have, this is the one that leaves
           no jump when the sweep picks up again. */
        int32_t hx, hz;
        rbs_arc_point(r, r->sweep, &hx, &hz);
        if (r->ai_timer == 1) {
            r->slash_from_x = r->x;
            r->slash_from_y = r->y;   /* wherever the strike left it */
            r->slash_from_z = r->z;
        }
        r->x = r->slash_from_x + ((hx - r->slash_from_x) * e) / 256;
        r->z = r->slash_from_z + ((hz - r->slash_from_z) * e) / 256;
        r->y = r->slash_from_y + ((r->spawn_y - r->slash_from_y) * e) / 256;
        r->lean = (RBS_SLASH_LEAN * (256 - e)) / 256;

        if (r->ai_timer >= RBS_SLASH_BACK) rbs_resume_sweep(r);
        break;
    }
    }
}

void update_rabisus(void) {
    int i;
    for (i = 0; i < rabisu_count; i++) {
        Rabisu *r = &rabisus[i];
        if (!r->active || r->dead) continue;
        if (r->area != current_area) continue;

        if (r->hit_timer > 0) r->hit_timer--;

        /* Turn to face the player. The facing is kept as the normalised
           direction vector rather than an angle: that IS the sin/cos pair the
           draw's rotation matrix needs, so no atan2 (which this SDK does not
           provide) and no trig lookup are involved.

           This runs even while dying and while the director owns the body, and
           deliberately: "maintains a forward gaze on the player" holds through
           the reveal and through its own death — except that during a cutscene
           the player is anchored off wherever they walked in, so the director
           overrides the target with the camera (see rabisu_face_override). */
        int32_t tgt_x = r->face_ovr ? r->face_ovr_x : player_x();
        int32_t tgt_z = r->face_ovr ? r->face_ovr_z : player_z();
        int32_t dx = tgt_x - r->x;
        int32_t dz = tgt_z - r->z;
        int32_t len = isqrt32(dx * dx + dz * dz);
        if (len > 0) {
            r->face_s = (dx << 12) / len;
            r->face_c = (dz << 12) / len;
#if RBS_FACE_BACKWARD
            r->face_s = -r->face_s;
            r->face_c = -r->face_c;
#endif
        }

        /* Idle playback. One shared clip, looping, at RBS_ANIM_TICKS game
           frames each. The clock is per instance, and it only advances in the
           boss's own area because of the area gate above — which is what
           we want: an off-screen boss in another room should not be burning
           frames of animation. `frozen` is the death sequence holding the pose
           for its two-second beat before the light starts. */
        if (!r->frozen && rabisu_anim_count > 1 &&
            ++r->anim_tick >= RBS_ANIM_TICKS) {
            r->anim_tick = 0;
            if (++r->anim_frame >= rabisu_anim_count) r->anim_frame = 0;
        }

        if (!r->dying) rbs_update_ai(r, i);
        /* Outside the AI switch on purpose: a wave already in the air keeps
           expanding whatever the boss has moved on to, including its own
           death. Launching it was the commitment. */
        rbs_shock_tick(r);
    }

    rbs_glow_clock++;
    rbs_fireballs_update();
}

void rabisus_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < rabisu_count; i++) {
        Rabisu *r = &rabisus[i];
        /* A dying boss stops being solid. It is drawn for another nine
           seconds while it comes apart, but it is finished as an obstacle —
           and the death sequence walks it back to its spawn, which would shove
           the player across the garden if the cylinder were still live. */
        if (!r->active || r->dead || r->dying) continue;
        if (r->area != current_area) continue;

        /* Vertical gate: the player's body span (feet at py+GROUND_FLOOR_Y,
           head a little above the eye) against the boss's. It hovers, so a
           short enough enemy could be walked under — this one cannot, but the
           test costs nothing and keeps the rule honest. */
        int32_t body_top = py - 30;
        int32_t body_bot = py + GROUND_FLOOR_Y;
        int32_t rbs_top  = r->y - RBS_HEIGHT;
        int32_t rbs_bot  = r->y;
        if (body_bot < rbs_top || body_top > rbs_bot) continue;

        /* Radial push-out. A cylinder rather than the concrete props' rotated
           AABB: the boss turns to face the player every frame, and a box would
           breathe in and out under the player as it did. */
        int32_t dx   = *px - r->x;
        int32_t dz   = *pz - r->z;
        int32_t need = RBS_BODY_RADIUS + radius + RBS_PUSH_MARGIN;
        int32_t d    = isqrt32(dx * dx + dz * dz);
        if (d >= need) continue;

        if (d == 0) {          /* dead centre: pick an axis rather than divide */
            *px = r->x + need;
            continue;
        }
        *px = r->x + (dx * need) / d;
        *pz = r->z + (dz * need) / d;
    }
}

/* NOTE FOR ANYONE LOOKING FOR rabisus_try_hit(): there isn't one, on purpose.
   The crucifaxe cannot damage this enemy (see the note on RBS_MAX_HEALTH in
   rabisu.h) and crucifaxe.c has no rabisu block. The axe earns its place in
   this fight as a PARRY instead — both deflect windows above read swing_timer
   directly, so the swing matters without ever landing a hit. */

/* Turn a mesh-local point into the world point it currently occupies. This
   MUST stay in step with the transform draw_rabisus builds, because that is
   the whole contract: the death lights sit on the head and the wing tips only
   for as long as the two agree. Order is lean (local X) first, then the facing
   yaw, then the translate — identical to the CompMatrixLV chain below. */
void rabisu_anchor_world(const Rabisu *r, int32_t lx, int32_t ly, int32_t lz,
                         VECTOR *out) {
    /* Lean about local X: y and z rotate, x is untouched. */
    int32_t ry = ly, rz = lz;
    if (r->lean) {
        int32_t a = RBS_LEAN_BACKWARD ? r->lean : -r->lean;
        int32_t s = isin(a & 4095), c = icos(a & 4095);
        ry = (ly * c - lz * s) >> 12;
        rz = (ly * s + lz * c) >> 12;
    }

    /* Facing yaw, from the stored sin/cos pair. */
    out->vx = r->x + ((lx * r->face_c + rz * r->face_s) >> 12);
    out->vy = r->y + RBS_FOOT_OFF + ry;
    out->vz = r->z + ((-lx * r->face_s + rz * r->face_c) >> 12);
}

/* Health bar above the boss's head. Drawn with the plain camera view matrix
   (the caller restores it before this runs), by projecting a single point a
   little above the model's top. A model has no billboard quad to hang the bar
   off the way the sprite enemies do, so the anchor point is explicit. */
static void draw_rbs_bar(RenderContext *ctx, const Rabisu *r) {
    if (r->hit_timer <= 0) return;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    SVECTOR top;
    top.vx = (int16_t)r->x;
    top.vy = (int16_t)(r->y - RBS_HEIGHT - 60);   /* 60 clear of the head */
    top.vz = (int16_t)r->z;
    top.pad = 0;

    DVECTOR sv;
    int32_t sz;
    gte_ldv0(&top);
    gte_rtps();
    gte_stsxy(&sv);
    gte_stsz(&sz);
    if (sz == 0) return;
    if (sv.vx <= -1023 || sv.vx >= 1023 || sv.vy <= -1023 || sv.vy >= 1023) return;

    /* Above SCENE_OT_MIN so the bar lands in front of the model it belongs to
       but still behind the HUD. */
    int32_t otz = SCENE_OT_MIN;
    int16_t bar_x = sv.vx - 30;
    int16_t bar_y = sv.vy;

    if (ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *bg = (TILE *)ctx->next_packet;
        setTile(bg);
        setRGB0(bg, 40, 40, 40);
        setXY0(bg, bar_x, bar_y);
        setWH(bg, 60, 6);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz + 1], bg);
        ctx->next_packet += sizeof(TILE);
    }

    int16_t fill_w = (int16_t)((r->health * 60) / RBS_MAX_HEALTH);
    if (fill_w > 0 && ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *fill = (TILE *)ctx->next_packet;
        setTile(fill);
        setRGB0(fill, 200, 20, 20);
        setXY0(fill, bar_x, bar_y);
        setWH(fill, fill_w, 6);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], fill);
        ctx->next_packet += sizeof(TILE);
    }
}

/* The body colour the model was drawn in before it had a skin. Still load-
   bearing in two places: the death BURN (see below, where a textured poly
   cannot fade) and the fallback if the skin failed to register. Keep it in
   step with nothing — the .smd's own baked RGB is now a neutral 128,128,128
   modulation value and no longer carries the boss's colour. */
#define RBS_BODY_R   33
#define RBS_BODY_G   18
#define RBS_BODY_B  204

/* Walk one boss's SMD prim stream. Every prim is a TEXTURED quad: 32 bytes,
   with the neutral modulation RGB still at offset 16 and the four UV pairs at
   offset 20. One texture for the whole model, so there is no <slug>_tex_map.h
   — the slot is the same for every poly and comes from texmgr.

   NO TEXTURE WINDOW BRACKET. The skin sits at VRAM (704,256): page-aligned in
   X and Voff 0, so the 128x128 window the Garden Courtyard sorts for its own
   art wraps this model's UVs (all inside 0..127 already) onto itself. That is
   a property of where the TIM was placed, not luck — moving it off a 64-word
   boundary or onto Voff 128 would need the zombie-style bracket instead.

   The GTE matrix is already the composed model-view when this runs. */
static void draw_rbs_model(RenderContext *ctx, const Rabisu *r, int32_t dist) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint8_t *p = (uint8_t *)rabisu_smd->p_prims;

    /* THE FADE-OUT, at the end of the death sequence. A flat-shaded POLY_F4 has
       no alpha channel, so "fade away" has to be a blend mode: every poly goes
       ADDITIVE (ABR=1, the same blend the lightswitch beams and the stove flame
       use) and its colour is scaled toward black. Additive black contributes
       nothing, so at fade 0 the body is genuinely gone over ANY background —
       which merely darkening the colour would not achieve, and which fogging it
       toward the sky would only achieve against the sky.
       It also reads right: the thing has spent six seconds pouring out light,
       and it burns out rather than dissolving.
       Additive needs a DR_TPAGE in FRONT of each poly in OT order, and the OT
       is LIFO, so the tpage is added to the same bucket immediately AFTER its
       poly. Costs 8 extra bytes per poly, and only while fading.

       >>> AND IT IS WHY THE BURN DROPS BACK TO FLAT, UNTEXTURED POLYS. <<< On a
       TEXTURED poly the PS1 does not take semi-transparency from the primitive
       at all: it takes it per TEXEL, from each colour's STP bit, and a TIM
       converted from an opaque PNG has that bit clear everywhere. setSemiTrans
       on a POLY_FT4 would therefore do exactly nothing — the boss would stand
       there fully lit through the whole six-second burn and then vanish in one
       frame. So the fading body is drawn as the flat silhouette it used to be,
       in RBS_BODY_*: by that point it is a lit-from-within glow rather than a
       creature, and its own skin has nothing left to say. */
    int      burning = (r->fade < 256);
    int32_t  fade    = r->fade < 0 ? 0 : r->fade;

    /* Textured for every ordinary frame; flat while burning, and flat if the
       skin never registered (texmgr fails silently past its cap). */
    int       textured = (rabisu_tex >= 0) && !burning;
    uint16_t  tex_tpage = textured ? texmgr_tpage(rabisu_tex) : 0;
    uint16_t  tex_clut  = textured ? texmgr_clut(rabisu_tex)  : 0;

    /* The rise-through-the-lawn cut, expressed in MODEL space so the test is
       one comparison per vertex rather than a transform. The draw translates
       by (r->y + RBS_FOOT_OFF), and neither the yaw nor the lean is active
       during the rise, so a vertex's world Y is just that plus its own vy —
       which makes the world cut plane this model-space vy. */
    int      clipping = (r->clip_y != RBS_NO_CLIP);
    int32_t  clip_vy  = r->clip_y - (r->y + RBS_FOOT_OFF);

    /* THE animation hook. Topology (which vertices, what colour) still comes
       from the .smd's prim stream below; only the POSITIONS are swapped for
       this frame's baked block. That is the whole of playback. */
    SVECTOR *vp = rbs_verts(r);

    /* Same fog the room's own mesh uses (g_fog_near/far are set by the room's
       draw), saturating to the sky colour. Computed once per model rather than
       per poly: the boss is ~4.6 m across and the fog gradient over that span is
       invisible, and 476 divides a frame is not. */
    int32_t fog = dist < g_fog_near ? g_fog_near : (dist > g_fog_far ? g_fog_far : dist);
    int32_t fog_factor = ((g_fog_far - fog) << 8) / (g_fog_far - g_fog_near);
    /* Flash red the whole model on a fresh hit, the way the sprite enemies tint
       their quad — on a one-colour model it is the only damage feedback the
       silhouette can carry.
       HELD FOR THE WHOLE DEATH, not just the two seconds the killing blow's
       hit_timer would buy: the body is lit from the inside from the moment it
       starts dying until it has faded out, so the red runs under the settle,
       the freeze and the burn as one unbroken glow rather than dropping back
       to its normal colour halfway through and re-igniting with the lights. */
    int hit = (r->hit_timer > 0) || r->dying;

    int i;
    for (i = 0; i < rabisu_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt      = (SMD_PRI_TYPE *)p;
        uint8_t       stride  = pt->len;
        int           is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &vp[vi[0]];
        SVECTOR *v1 = &vp[vi[1]];
        SVECTOR *v2 = &vp[vi[2]];

        /* Drop the poly only when EVERY corner is under the cut. Dropping any
           poly that merely straddles it would eat a visible band off the
           silhouette; leaving the stragglers means a few small faces poke a
           little way through the turf as it surfaces, which is the cheaper
           error and, on a thing clawing its way out of the ground, the more
           flattering one. */
        if (clipping && v0->vy > clip_vy && v1->vy > clip_vy && v2->vy > clip_vy &&
            (!is_quad || vp[vi[3]].vy > clip_vy)) {
            p += stride; continue;
        }

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

        /* Backface cull. Unlike a rolling SPRITE (see mistake 4 in
           ADDING_AN_ENEMY.txt) a solid model genuinely has back faces, and
           culling them halves what reaches the GPU. */
        if (!pt->nocull) {
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }

        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        SVECTOR *v3    = 0;
        int32_t  v2_sz = sz[3];   /* v2's SZ, before the quad path reuses sz[3] */
        if (is_quad) {
            v3 = &vp[vi[3]];
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

        /* Textured: the .smd's baked 128,128,128 is the neutral modulation
           value, so the fog and hit maths below shade the SKIN by exactly the
           factors they used to apply to the flat colour — and by exactly the
           blend the room's own textured mesh uses, so the boss fogs into the
           garden rather than alongside it. Flat: the old body colour, which
           the .smd no longer carries. */
        uint8_t *col = p + 16;
        int32_t cr, cg, cb;
        if (textured) { cr = col[0]; cg = col[1]; cb = col[2]; }
        else          { cr = RBS_BODY_R; cg = RBS_BODY_G; cb = RBS_BODY_B; }
        if (hit) { cr = 255; cg = cg >> 2; cb = cb >> 2; }
        int32_t sr = (cr * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8;
        int32_t sg = (cg * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8;
        int32_t sb = (cb * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8;
        if (burning) { sr = (sr * fade) >> 8; sg = (sg * fade) >> 8; sb = (sb * fade) >> 8; }
        uint8_t rr = (uint8_t)sr, gg = (uint8_t)sg, bb = (uint8_t)sb;

        int32_t   need;
        if (textured) need = is_quad ? (int32_t)sizeof(POLY_FT4) : (int32_t)sizeof(POLY_FT3);
        else          need = (is_quad ? (int32_t)sizeof(POLY_F4) : (int32_t)sizeof(POLY_F3)) +
                             (burning ? (int32_t)sizeof(DR_TPAGE) : 0);
        uint32_t *ot   = ctx->buffers[ctx->active_buffer].ot;
        if (ctx->next_packet + need > buf_end) { p += stride; continue; }

        /* UVs live at offset 20 of a textured prim, two bytes per corner, in
           the same corner order as the vertex indices. No DR_TPAGE of its own:
           a POLY_FT* carries its page and CLUT in the primitive. */
        uint8_t *uv = p + 20;

        if (textured && is_quad) {
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, rr, gg, bb);
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
            poly->u0 = uv[0]; poly->v0 = uv[1];
            poly->u1 = uv[2]; poly->v1 = uv[3];
            poly->u2 = uv[4]; poly->v2 = uv[5];
            poly->u3 = uv[6]; poly->v3 = uv[7];
            poly->tpage = tex_tpage;
            poly->clut  = tex_clut;
            addPrim(&ot[otz], poly);
            ctx->next_packet += sizeof(POLY_FT4);
        } else if (textured) {
            POLY_FT3 *poly = (POLY_FT3 *)ctx->next_packet;
            setPolyFT3(poly);
            setRGB0(poly, rr, gg, bb);
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            poly->u0 = uv[0]; poly->v0 = uv[1];
            poly->u1 = uv[2]; poly->v1 = uv[3];
            poly->u2 = uv[4]; poly->v2 = uv[5];
            poly->tpage = tex_tpage;
            poly->clut  = tex_clut;
            addPrim(&ot[otz], poly);
            ctx->next_packet += sizeof(POLY_FT3);
        } else if (is_quad) {
            POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
            setPolyF4(poly);
            setRGB0(poly, rr, gg, bb);
            if (burning) setSemiTrans(poly, 1);
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
            addPrim(&ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F4);
        } else {
            POLY_F3 *poly = (POLY_F3 *)ctx->next_packet;
            setPolyF3(poly);
            setRGB0(poly, rr, gg, bb);
            if (burning) setSemiTrans(poly, 1);
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            addPrim(&ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F3);
        }

        if (burning) {
            DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
            setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
            addPrim(&ot[otz], tp);
            ctx->next_packet += sizeof(DR_TPAGE);
        }

        p += stride;
    }
}

void draw_rabisus(RenderContext *ctx) {
    if (!rabisu_smd || rabisu_count == 0) return;

    /* Rebuild the camera view matrix locally so this module is self-contained,
       then compose the boss's own rotation+translation onto it per instance.
       The caller's matrix is put back at the end — the sprite enemies, pickups
       and door text drawn after this all assume the plain view matrix.

       camera_build_view rather than the yaw-only block the sprite enemies use,
       and it MATTERS here: the Rabisu's own reveal and death cutscenes pitch
       the camera down over the garden, and a yaw-only matrix would silently
       ignore that and draw the boss through a different projection than the
       room it is standing in. Identical to the old block whenever pitch is 0. */
    MATRIX view;
    camera_build_view(&view);

    int i, drew_any = 0;
    for (i = 0; i < rabisu_count; i++) {
        Rabisu *r = &rabisus[i];
        if (!r->active || r->dead) continue;
        if (r->area != current_area) continue;
        /* Fully burnt out, or waiting under the lawn before its reveal: not
           merely invisible but skipped, so neither case costs 476 polys. */
        if (r->fade <= 0) continue;

        int32_t dcx = r->x - cam_x, dcz = r->z - cam_z;
        int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
        if (dist > RBS_DRAW_DIST) continue;

        /* Y-rotation built straight from the stored facing vector. This is
           exactly the matrix RotMatrix({0,yaw,0}) would produce for the yaw
           whose sine and cosine those are — same handedness as every other
           rotated prop in the game (see chainlink_door.c's footprint maths). */
        MATRIX yawm, pm, combined;
        yawm.m[0][0] = (int16_t)r->face_c; yawm.m[0][1] = 0;           yawm.m[0][2] = (int16_t)r->face_s;
        yawm.m[1][0] = 0;                  yawm.m[1][1] = (int16_t)ONE; yawm.m[1][2] = 0;
        yawm.m[2][0] = (int16_t)-r->face_s; yawm.m[2][1] = 0;          yawm.m[2][2] = (int16_t)r->face_c;

        /* The foot slash's backward lean, about the model's OWN X axis — so it
           tips back relative to the direction it is charging, whichever way
           that happens to be. Applied INSIDE the yaw (yaw * lean), and built by
           hand rather than through RotMatrix for one reason: rabisu_anchor_world
           has to reproduce this exact transform to keep the death lights stuck
           to the head, and two hand-written matrices cannot disagree about a
           sign convention the way a hand-written one and RotMatrix could. */
        if (r->lean) {
            int32_t a = RBS_LEAN_BACKWARD ? r->lean : -r->lean;
            int32_t s = isin(a & 4095), c = icos(a & 4095);
            MATRIX lean;
            lean.m[0][0] = (int16_t)ONE; lean.m[0][1] = 0;           lean.m[0][2] = 0;
            lean.m[1][0] = 0;            lean.m[1][1] = (int16_t)c;  lean.m[1][2] = (int16_t)-s;
            lean.m[2][0] = 0;            lean.m[2][1] = (int16_t)s;  lean.m[2][2] = (int16_t)c;
            MulMatrix0(&yawm, &lean, &pm);
        } else {
            pm = yawm;
        }

        /* RBS_FOOT_OFF is applied HERE and nowhere else: it cancels the model's
           authored offset so its lowest vertex lands on the anchor. Adding it
           moves the model DOWN, because -Y is up.

           The death shake rides on the TRANSLATION only, never on r->x/r->z.
           Jittering the entity itself would jitter its collision cylinder, its
           health-bar anchor and the death lights' anchors with it — the body is
           meant to vibrate on the spot, not to be somewhere different. */
        int32_t jx = 0, jz = 0, jy = 0;
        if (r->shake) {
            int32_t a = r->shake;
            jx = (int32_t)(rbs_rand() % (uint32_t)(2 * a + 1)) - a;
            jy = (int32_t)(rbs_rand() % (uint32_t)(2 * a + 1)) - a;
            jz = (int32_t)(rbs_rand() % (uint32_t)(2 * a + 1)) - a;
        }
        VECTOR pos = { r->x + jx, r->y + RBS_FOOT_OFF + jy, r->z + jz };
        TransMatrix(&pm, &pos);
        CompMatrixLV(&view, &pm, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        draw_rbs_model(ctx, r, dist);
        drew_any = 1;
    }

    if (!drew_any) return;

    /* Back to the plain view matrix, then the bars — which project world points
       and so need it. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);

    for (i = 0; i < rabisu_count; i++) {
        Rabisu *r = &rabisus[i];
        /* No bar over a corpse. The killing hit set hit_timer to its full two
           seconds like any other, and an empty bar hanging over the death
           sequence would be the one piece of HUD still insisting there is a
           fight on. */
        if (!r->active || r->dead || r->dying || r->area != current_area) continue;
        draw_rbs_bar(ctx, r);
    }
}

/* ---- Fireball drawing ------------------------------------------------------
   A solid cube, exactly as a web is drawn (src/web.c) and for the same reason:
   all six faces at one flat colour and one OT depth means the result is the
   silhouette filled, with no dependence on winding order for gte_nclip and no
   sorting to get wrong. Only the colour differs — hot orange-red going out,
   white-hot on the way back, so a deflected ball reads as the player's shot
   rather than the boss's. */
static void draw_rbs_fireball(RenderContext *ctx, const RbsFireball *f) {
    static const int8_t corner[8][3] = {
        { -1, -1, -1 }, {  1, -1, -1 }, {  1, -1,  1 }, { -1, -1,  1 },
        { -1,  1, -1 }, {  1,  1, -1 }, {  1,  1,  1 }, { -1,  1,  1 },
    };
    static const uint8_t face[6][4] = {
        { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
        { 3, 2, 6, 7 }, { 0, 3, 7, 4 }, { 1, 2, 6, 5 },
    };

    int32_t fdx  = f->x - cam_x;
    int32_t fdz  = f->z - cam_z;
    int32_t dist = (fdx < 0 ? -fdx : fdx) + (fdz < 0 ? -fdz : fdz);
    if (dist >= g_fog_far) return;
    int32_t fs = render_fog_scale(dist);
    if (fs > 255) fs = 255;

    uint8_t cr, cg, cb;
    if (f->deflected) { cr = 255; cg = 240; cb = 190; }
    else              { cr = 255; cg =  70; cb =  20; }
    cr = (uint8_t)((cr * fs) >> 8);
    cg = (uint8_t)((cg * fs) >> 8);
    cb = (uint8_t)((cb * fs) >> 8);

    SVECTOR v[8];
    int c;
    for (c = 0; c < 8; c++) {
        v[c].vx  = (int16_t)(f->x + corner[c][0] * RBS_FB_HALF);
        v[c].vy  = (int16_t)(f->y + corner[c][1] * RBS_FB_HALF);
        v[c].vz  = (int16_t)(f->z + corner[c][2] * RBS_FB_HALF);
        v[c].pad = 0;
    }

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int fi;
    for (fi = 0; fi < 6; fi++) {
        if (ctx->next_packet + sizeof(POLY_F4) > buf_end) return;

        DVECTOR sv[4];
        int32_t sz[4], otz;

        gte_ldv3(&v[face[fi][0]], &v[face[fi][1]], &v[face[fi][2]]);
        gte_rtpt();
        gte_stsxy3c(sv);
        gte_ldv0(&v[face[fi][3]]);
        gte_rtps();
        gte_stsxy(&sv[3]);
        gte_stsz4c(sz);
        if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) continue;

        int k;
        for (k = 0; k < 4; k++)
            if (sv[k].vx <= -1023 || sv[k].vx >= 1023 ||
                sv[k].vy <= -1023 || sv[k].vy >= 1023) break;
        if (k < 4) continue;

        gte_avsz4();
        gte_stotz(&otz);
        if (otz <= 0) continue;
        if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
        setPolyF4(poly);
        setRGB0(poly, cr, cg, cb);
        poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
        poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
        poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
        poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;
        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
        ctx->next_packet += sizeof(POLY_F4);
    }
}

/* ---- The shockwave ----------------------------------------------------------
   A bright annulus racing outward across the ground, with a short vertical
   skirt riding its leading edge. The ring says where the wave IS; the skirt is
   the part the player actually sees coming, because in first person a flat
   figure on the floor disappears the moment it is more than a few metres away.

   Both are built as RBS_SHOCK_SEGS wedges around the arc, and every corner
   looks up its own floor height — the wave crosses the sunken lawn, the tread
   and the raised terraces, and a ring drawn at one Y would sink into two of
   them. */
#define RBS_SHOCK_BAND       210   /* radial width of the bright ring */

static void draw_rbs_shockwave(RenderContext *ctx, const Rabisu *r) {
    int32_t reach = r->wave_t >= RBS_SHOCK_EXPAND
                  ? RBS_SWEEP_RADIUS
                  : (RBS_SWEEP_RADIUS * r->wave_t) / RBS_SHOCK_EXPAND;
    if (reach <= 0) return;

    /* Full brightness while it is still travelling, then it dies back over the
       linger rather than blinking out at full reach. */
    int32_t bright = 256;
    if (r->wave_t > RBS_SHOCK_EXPAND) {
        int32_t k = r->wave_t - RBS_SHOCK_EXPAND;
        bright = 256 - (k * 256) / RBS_SHOCK_LINGER;
        if (bright < 0) bright = 0;
    }
    if (bright <= 0) return;

    int32_t inner = reach - RBS_SHOCK_BAND;
    if (inner < 0) inner = 0;

    /* One clock for the whole ring: unlike the sixteen lawn lights, this is a
       single object and shimmering its segments out of phase would break it up
       into twenty separate flickers. */
    uint8_t gr, gg, gb, wr, wg, wb;
    rbs_glow_pulse(rbs_glow_clock, (bright * RBS_POOL_LEVEL) >> 8, &gr, &gg, &gb);
    rbs_glow_pulse(rbs_glow_clock, (bright * 150) >> 8, &wr, &wg, &wb);

    SVECTOR v[4];
    int k, s;
    for (k = 0; k < 4; k++) v[k].pad = 0;

    for (s = 0; s < RBS_SHOCK_SEGS; s++) {
        int32_t a0 = (s * 4096) / RBS_SHOCK_SEGS;
        int32_t a1 = ((s + 1) * 4096) / RBS_SHOCK_SEGS;
        int32_t s0 = isin(a0), c0 = icos(a0);
        int32_t s1 = isin(a1), c1 = icos(a1);

        int32_t ix0 = r->spawn_x + (s0 * inner >> 12), iz0 = r->spawn_z + (c0 * inner >> 12);
        int32_t ix1 = r->spawn_x + (s1 * inner >> 12), iz1 = r->spawn_z + (c1 * inner >> 12);
        int32_t ox0 = r->spawn_x + (s0 * reach >> 12), oz0 = r->spawn_z + (c0 * reach >> 12);
        int32_t ox1 = r->spawn_x + (s1 * reach >> 12), oz1 = r->spawn_z + (c1 * reach >> 12);

        int32_t iy0 = rbs_floor_y_at(ix0, iz0, r->spawn_y + RBS_HOVER) - 4;
        int32_t iy1 = rbs_floor_y_at(ix1, iz1, r->spawn_y + RBS_HOVER) - 4;
        int32_t oy0 = rbs_floor_y_at(ox0, oz0, r->spawn_y + RBS_HOVER) - 4;
        int32_t oy1 = rbs_floor_y_at(ox1, oz1, r->spawn_y + RBS_HOVER) - 4;

        /* The band on the ground. */
        v[0].vx = (int16_t)ix0; v[0].vy = (int16_t)iy0; v[0].vz = (int16_t)iz0;
        v[1].vx = (int16_t)ox0; v[1].vy = (int16_t)oy0; v[1].vz = (int16_t)oz0;
        v[2].vx = (int16_t)ix1; v[2].vy = (int16_t)iy1; v[2].vz = (int16_t)iz1;
        v[3].vx = (int16_t)ox1; v[3].vy = (int16_t)oy1; v[3].vz = (int16_t)oz1;
        rbs_glow_quad(ctx, v, gr, gg, gb);

        /* The skirt standing on its leading edge. -Y is up. */
        v[0].vx = (int16_t)ox0; v[0].vy = (int16_t)(oy0 - RBS_SHOCK_WALL_H); v[0].vz = (int16_t)oz0;
        v[1].vx = (int16_t)ox1; v[1].vy = (int16_t)(oy1 - RBS_SHOCK_WALL_H); v[1].vz = (int16_t)oz1;
        v[2].vx = (int16_t)ox0; v[2].vy = (int16_t)oy0;                      v[2].vz = (int16_t)oz0;
        v[3].vx = (int16_t)ox1; v[3].vy = (int16_t)oy1;                      v[3].vz = (int16_t)oz1;
        rbs_glow_quad(ctx, v, wr, wg, wb);
    }
}

void rbs_attacks_draw(RenderContext *ctx) {
    int i;

    for (i = 0; i < MAX_RBS_FIREBALLS; i++) {
        RbsFireball *f = &rbs_fireballs[i];
        if (f->life <= 0 || f->area != current_area) continue;
        draw_rbs_fireball(ctx, f);
    }

    for (i = 0; i < rabisu_count; i++) {
        Rabisu *r = &rabisus[i];
        if (!r->active || r->dead || r->area != current_area) continue;

        /* The light beam's tell: the chest coming up to full over 1.5 s. It is
           the only warning the attack gives, so it ramps rather than switching
           on — a light that is already at full when you notice it tells you
           nothing about how long you have left. */
        if (r->ai_state == RBS_AI_BEAM_CHARGE) {
            VECTOR chest;
            rabisu_anchor_world(r, RBS_A_CHEST_X, RBS_A_CHEST_Y, RBS_A_CHEST_Z,
                                &chest);
            int32_t b = (r->ai_timer * 256) / RBS_BEAM_CHARGE;
            if (b > 256) b = 256;
            rbs_glow_point(ctx, &chest, b, rbs_glow_clock);
        }

        /* The path itself: the poly burning now, plus the one behind it dying
           back. Two at a time rather than one is what makes it read as
           something TRAVELLING instead of a light being switched between
           unrelated squares. */
        if (r->ai_state == RBS_AI_BEAM_WALK && r->beam_cells > 0) {
            int step;
            for (step = r->beam_step; step >= r->beam_step - 1; step--) {
                if (step < 0) continue;
                int32_t cx, cz, half = RBS_BEAM_CELL / 2;
                rbs_beam_cell_centre(r, step, &cx, &cz);
                int32_t fy = rbs_floor_y_at(cx, cz, r->spawn_y + RBS_HOVER);
                /* The trailing cell fades out across the CURRENT one's 0.3 s.
                   ai_timer counts down, so it is already the fraction left. */
                int32_t b = (step == r->beam_step)
                          ? 256
                          : (r->ai_timer * 256) / RBS_BEAM_STEP;
                if (b <= 0) continue;
                rbs_glow_pillar(ctx, cx - half, cx + half, cz - half, cz + half,
                                fy, b, rbs_glow_clock + step * 7);
            }
        }

        if (r->wave_t > 0) draw_rbs_shockwave(ctx, r);
    }
}
