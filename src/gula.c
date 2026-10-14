#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "collision.h"       /* GROUND_FLOOR_Y */
#include "crucifaxe.h"       /* SWING_RANGE */
#include "sound.h"           /* SFX_HOWL (KENNELS bank), HURT / DIE / AXEHIT / SLAM */
#include "cdaudio.h"         /* suspend/resume around the entry-time reads */
#include "kennels_puzzle.h"  /* kennels_puzzle_dogs_dead: what calls her */
#include "gula.h"

/* Gula - see gula.h for the fight, the persistence and the VRAM. */

_Static_assert(GULA_WAVE_DAMAGE * 20 == MAX_HEALTH * 7,
               "GULA_WAVE_DAMAGE is no longer 35% of the player's maximum");

Gula gula;

#define GULA_ALL_DOGS   0xFF   /* the Kennels' eight, one bit each */

/* ---- Textures ---------------------------------------------------------------
   Two TIMs, streamed (gula.h). Frame f is half f>>1, and within the half its
   own 128-texel tpage: the halves are 128 words wide at 8bpp, i.e. two pages
   each, so frame 0 is page x320, frame 1 x384, frame 2 x704, frame 3 x768, and
   every frame's U runs 0..127 in its own page. */
#define GULA_FILE_A     "\\TEXCTCMB\\GULAA.TIM;1"
#define GULA_FILE_B     "\\TEXCTCMB\\GULAB.TIM;1"
#define GULA_SCRATCH    (17 * 2048)   /* each half is 33,312 bytes: 17 sectors */
#define GULA_STACK_MARGIN 2048        /* rabisu.c's RBS_STACK_MARGIN           */
#define GULA_CLUT_X     672
#define GULA_CLUT_Y     506

static const int16_t gula_page_x[4] = { 320, 384, 704, 768 };
static int gula_tex_ok = 0;   /* both halves landed this entry */

static RECT gula_tw_restore;
static int  gula_tw_active = 0;

void gula_set_texwindow(const RECT *tw) {
    if (tw) { gula_tw_restore = *tw; gula_tw_active = 1; }
    else    { gula_tw_active = 0; }
}

/* kennels.c's kn_stream_tim: one TIM off the disc into `buf` and up into VRAM,
   pixels and CLUT. 1 on success. */
static int gula_stream_tim(const char *filename, uint8_t *buf, int bufcap) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)filename)) return 0;
    int sectors = (file.size + 2047) / 2048;
    if (sectors * 2048 > bufcap) return 0;
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
    }
    return 1;
}

void gula_upload_textures(void) {
    gula_tex_ok = 0;
    /* Dead is dead: nothing in the room will draw her again, so the crawler's
       art can stay where it is. (On a title load the flags are not restored
       yet and this reads 0 - which only costs the read.) */
    if (game_flag(FLAG_GULA_DEAD)) return;

    uint8_t *scratch = malloc(GULA_SCRATCH);
    if (!scratch) return;   /* she is not drawn rather than drawn as garbage */

    /* >>> AND NOT IF IT RUNS INTO THE STACK. <<< src/rabisu.c's read_file guard:
       the heap top IS the stack, so a full heap hands out memory the stack is
       using and the CdRead below would DMA through a return address. Measured
       on a force-boot into the Kennels (the kindest case) this buffer ended
       about 36 KB under $sp. Should a future build eat that, she is simply not
       drawn - the fight still runs - instead of the console going black. */
    {
        uint32_t sp;
        __asm__ volatile("move %0, $sp" : "=r"(sp));
        if ((uint32_t)scratch + GULA_SCRATCH + GULA_STACK_MARGIN > sp) {
            free(scratch);
            return;
        }
    }
    cdaudio_suspend();
    int ok = gula_stream_tim(GULA_FILE_A, scratch, GULA_SCRATCH);
    ok    &= gula_stream_tim(GULA_FILE_B, scratch, GULA_SCRATCH);
    cdaudio_resume();
    free(scratch);
    gula_tex_ok = ok;
}

/* Integer square root - lumberer.c's, with its `t > 0` seed (mistake 11). */
static int32_t gula_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x, last, s = 1, t = v;
    while (t > 0) { t >>= 2; s <<= 1; }
    x = s;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

/* ---- The pillars ---------------------------------------------------------------
   Is the straight line from her feet to (px, pz) cut by one of the four posts?
   A Liang-Barsky slab clip of the segment against each footprint, in 4096ths of
   the segment: coordinates stay within ~5000, so q * 4096 fits an int32. */
typedef struct { int16_t x0, x1, z0, z1; } GulaBox;

static const GulaBox gula_posts[4] = {
    { -GULA_POST_HI, -GULA_POST_LO, GULA_POST_Z0_SOUTH, GULA_POST_Z1_SOUTH },
    {  GULA_POST_LO,  GULA_POST_HI, GULA_POST_Z0_SOUTH, GULA_POST_Z1_SOUTH },
    { -GULA_POST_HI, -GULA_POST_LO, GULA_POST_Z0_NORTH, GULA_POST_Z1_NORTH },
    {  GULA_POST_LO,  GULA_POST_HI, GULA_POST_Z0_NORTH, GULA_POST_Z1_NORTH },
};

static int gula_clip(int32_t p, int32_t q, int32_t *t0, int32_t *t1) {
    if (p == 0) return q >= 0;              /* parallel: inside the slab or not */
    int32_t r = (q * 4096) / p;
    if (p < 0) { if (r > *t1) return 0; if (r > *t0) *t0 = r; }
    else       { if (r < *t0) return 0; if (r < *t1) *t1 = r; }
    return 1;
}

static int gula_sheltered(int32_t px, int32_t pz) {
    int32_t dx = px - GULA_X, dz = pz - GULA_Z;
    int k;
    for (k = 0; k < 4; k++) {
        const GulaBox *b = &gula_posts[k];
        int32_t t0 = 0, t1 = 4096;
        if (gula_clip(-dx, GULA_X - b->x0, &t0, &t1) &&
            gula_clip( dx, b->x1 - GULA_X, &t0, &t1) &&
            gula_clip(-dz, GULA_Z - b->z0, &t0, &t1) &&
            gula_clip( dz, b->z1 - GULA_Z, &t0, &t1) &&
            t0 <= t1)
            return 1;
    }
    return 0;
}

/* ---- Life cycle ---------------------------------------------------------------- */

static void gula_howl_stop(void) { sound_stop(SFX_HOWL); }

void gula_reset(void) {
    gula       = (Gula){0};
    gula.state = GULA_GONE;
    gula_howl_stop();   /* 3.4 s on a voice of its own: a new game must not
                           carry it out of the Kennels */
}

void gula_room_enter(GameState area) {
    /* Whatever she was doing is over. The door is sealed while she lives, so
       this only ever ends a fight by a death or a debug jump - and either way
       the next entry starts her from the beginning. */
    gula_howl_stop();
    gula        = (Gula){0};
    gula.area   = area;
    gula.health = GULA_MAX_HEALTH;
    gula.state  = (area == STATE_KENNELS) ? GULA_WATCH : GULA_GONE;
}

int gula_seals_door(void) {
    return gula.area == current_area &&
           (gula.state == GULA_FADEIN || gula.state == GULA_FIGHT ||
            gula.state == GULA_DYING);
}

int32_t gula_reveal(void) {
    if (gula.area != current_area) return 0;
    if (gula.state != GULA_FADEIN && gula.state != GULA_FIGHT &&
        gula.state != GULA_DYING) return 0;
    return gula.fade;
}

/* ---- Weapons ------------------------------------------------------------------- */

static const Weakness gula_weakness[] = {
    { DMG_HOLY, 200 },   /* the Helluminator, 2x: 2 a second of burn */
};

int32_t gula_scale_damage(int32_t base, DamageType type) {
    return damage_scale(base, type, gula_weakness, WEAKNESS_COUNT(gula_weakness));
}

int gula_hittable(void) {
    return gula.state == GULA_FIGHT && gula.area == current_area;
}

void gula_body(int32_t *x, int32_t *cy, int32_t *z, int32_t *hh, int32_t *hw) {
    *x = GULA_X; *cy = GULA_CY; *z = GULA_Z;
    *hh = GULA_HALF_H; *hw = GULA_HALF_W;
}

void gula_damage(int dmg) {
    if (!gula_hittable() || dmg <= 0) return;
    gula.health -= dmg;
    gula.flash   = 8;
    if (gula.health > 0) {
        sound_play(SFX_AXEHIT);
        return;
    }
    gula.health = 0;
    gula.state  = GULA_DYING;
    /* A wave in the air dies with her (ADDING_A_BOSS_ENCOUNTER.txt STEP 12),
       and so does a shove still carrying the player: the death scene anchors
       them where they are. */
    gula.wave_t  = 0;
    gula.shove_t = 0;
    /* ON THE BLOW, not at the end of the scene (Adapa's reasoning). The
       director sees GULA_DYING on its next update and takes the camera. */
    game_flag_set(FLAG_GULA_DEAD);
    /* The loop stops, and the howl plays once more as her death cry - the fade
       in src/gula_death.c runs exactly its length. */
    gula_howl_stop();
    sound_play(SFX_HOWL);
}

/* The axe: measured to her push SURFACE, not her centre - the cylinder holds
   the player GULA_WALL_LIKE_PUSH off it - and vertically to the nearest point
   of her body, which runs floor to y=-700 and so contains the player's eye. */
int gula_try_hit(void) {
    if (!gula_hittable()) return 0;
    int32_t dx = GULA_X - cam_x;
    int32_t dz = GULA_Z - cam_z;
    int32_t d  = gula_isqrt(dx * dx + dz * dz);
    int32_t gh = d > GULA_PUSH_RADIUS ? d - GULA_PUSH_RADIUS : 0;
    int32_t top = GULA_FLOOR_Y - 2 * GULA_HALF_H, dy = 0;
    if (cam_y < top)          dy = top - cam_y;
    if (cam_y > GULA_FLOOR_Y) dy = cam_y - GULA_FLOOR_Y;
    if (gh + dy >= SWING_RANGE) return 0;
    int32_t dot = ((int32_t)dx * isin(cam_rot) + (int32_t)dz * icos(cam_rot)) >> 12;
    if (dot <= 0) return 0;
    gula_damage(1);
    return 1;
}

/* The plug: lumberers_collide's radial push, capped a frame. Solid from the
   moment she starts to appear until the killing blow. */
void gula_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    (void)py;
    if (gula.area != current_area) return;
    if (gula.state != GULA_FADEIN && gula.state != GULA_FIGHT) return;
    int32_t dx   = *px - GULA_X;
    int32_t dz   = *pz - GULA_Z;
    int32_t need = GULA_PUSH_RADIUS + radius;
    int32_t d    = gula_isqrt(dx * dx + dz * dz);
    if (d >= need) return;
    int32_t step = need - d;
    if (step > GULA_PUSH_STEP) step = GULA_PUSH_STEP;
    if (d == 0) { *px += step; return; }
    *px += (dx * step) / d;
    *pz += (dz * step) / d;
}

int  gula_dying(void)         { return gula.state == GULA_DYING &&
                                       gula.area == current_area; }
void gula_set_fade(int32_t f) { gula.fade = f < 0 ? 0 : (f > 256 ? 256 : f); }
void gula_gone(void)          { gula.state = GULA_GONE; gula.fade = 0; }

/* ---- The wave ------------------------------------------------------------------
   Ticked every frame one is up, outside the state switch, so a wave already
   out finishes its sweep whatever she does (the Lumberer's rule). */
static int32_t gula_wave_reach(void) {
    return (GULA_WAVE_RADIUS * gula.wave_t) / GULA_WAVE_FRAMES;
}

static void gula_wave_tick(void) {
    if (gula.wave_t <= 0) return;
    gula.wave_t++;
    if (gula.wave_t > GULA_WAVE_FRAMES) {
        gula.wave_t = 0;
        gula.timer  = GULA_WAVE_GAP;   /* the 2 s before the next */
        return;
    }
    if (gula.wave_hit || game_over) return;

    /* EVERYTHING HERE TARGETS player_x/z(), never cam_* (camera.h). */
    int32_t px = player_x(), pz = player_z();
    int32_t dx = px - GULA_X, dz = pz - GULA_Z;
    int32_t d  = gula_isqrt(dx * dx + dz * dz);
    int32_t reach = gula_wave_reach();
    if (d > reach || d < reach - GULA_WAVE_HIT_BAND) return;
    if (gula_sheltered(px, pz)) return;   /* behind a pillar: it breaks on it */

    gula.wave_hit = 1;
    player_hurt(GULA_WAVE_DAMAGE);
    sound_play(SFX_HURT);

    /* THE SHOVE, straight away from her (gula.h). The step is fixed here, so
       the player is carried along one line however they turn. */
    if (d > 0) {
        gula.shove_dx = (dx * GULA_SHOVE_STEP) / d;
        gula.shove_dz = (dz * GULA_SHOVE_STEP) / d;
    } else {
        gula.shove_dx = 0;
        gula.shove_dz = GULA_SHOVE_STEP;
    }
    gula.shove_t = GULA_SHOVE_FRAMES;
    gula.shove_x = cam_x;
    gula.shove_z = cam_z;
    if (player_health <= 0) {
        player_health = 0;
        game_over     = 1;
        flash_timer   = 90;
        sound_play(SFX_DIE);
    }
}

/* One step of the shove. Runs AFTER apply_collision_reception has resolved
   the previous step against the walls (main.c's Kennels branch), so a step
   that a wall ate shows up here as the player standing short of where it put
   them - and that is the wall it was meant to carry them to. 30 rather than
   half a step, so a player merely walking against it is not read as a wall. */
static void gula_shove_tick(void) {
    if (gula.shove_t <= 0) return;
    if (game_over) { gula.shove_t = 0; return; }
    if (gula.shove_t < GULA_SHOVE_FRAMES) {
        int32_t lx = gula.shove_x - cam_x, lz = gula.shove_z - cam_z;
        if ((lx < 0 ? -lx : lx) + (lz < 0 ? -lz : lz) > 30) {
            gula.shove_t = 0;
            return;
        }
    }
    cam_x += gula.shove_dx;
    cam_z += gula.shove_dz;
    gula.shove_x = cam_x;
    gula.shove_z = cam_z;
    gula.shove_t--;
}

static void gula_howl_tick(void) {
    if (--gula.howl_timer > 0) return;
    sound_play(SFX_HOWL);
    gula.howl_timer = GULA_HOWL_FRAMES + GULA_HOWL_GAP;
}

void update_gula(void) {
    if (gula.flash > 0) gula.flash--;

    /* current_area, NEVER game_state - the inventory menu is not a pause. */
    if (gula.area != current_area) return;

    gula_wave_tick();
    gula_shove_tick();

    switch (gula.state) {

    case GULA_WATCH:
        if (game_flag(FLAG_GULA_DEAD)) { gula.state = GULA_GONE; break; }
        if ((kennels_puzzle_dogs_dead() & GULA_ALL_DOGS) == GULA_ALL_DOGS) {
            gula.state = GULA_WAIT;
            gula.timer = GULA_APPEAR_DELAY;
        }
        break;

    case GULA_WAIT:
        if (--gula.timer > 0) break;
        gula.state      = GULA_FADEIN;
        gula.timer      = 0;
        gula.fade       = 0;
        gula.howl_timer = 1;   /* the howl starts WITH the fade */
        break;

    case GULA_FADEIN:
        gula.anim_tick++;
        gula_howl_tick();
        gula.timer++;
        gula.fade = (gula.timer * 256) / GULA_FADEIN_FRAMES;
        if (gula.timer >= GULA_FADEIN_FRAMES) {
            gula.fade  = 256;
            gula.state = GULA_FIGHT;
            gula.timer = GULA_WAVE_GAP;   /* armed here, not zero (mistake 8) */
        }
        break;

    case GULA_FIGHT:
        gula.anim_tick++;
        gula_howl_tick();
        if (gula.wave_t == 0 && --gula.timer <= 0) {
            gula.wave_t   = 1;
            gula.wave_hit = 0;
            sound_play(SFX_SLAM);   /* resident: the wave leaving her feet */
        }
        break;

    default:   /* GONE, and DYING - the director owns her */
        break;
    }
}

/* ---- Drawing --------------------------------------------------------------------
   adapa.c's add_ft4_run_windowed: the sheet is at Voff 128, so the run of
   pieces is bracketed by a full/unmasked window and a restore of the room's,
   all in one OT bucket. addPrim prepends, so restore, the polys back to front,
   then disable yields disable -> poly[0..n-1] -> restore. */
static void add_ft4_run_windowed(RenderContext *ctx, int32_t otz,
                                 POLY_FT4 **polys, int n) {
    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;
    int       i;

    if (gula_tw_active && ctx->next_packet + 2 * sizeof(DR_TWIN) <= buf_end) {
        DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
        setTexWindow(restore, &gula_tw_restore);
        addPrim(&ot[otz], restore);
        ctx->next_packet += sizeof(DR_TWIN);

        for (i = n - 1; i >= 0; i--) addPrim(&ot[otz], polys[i]);

        RECT full = { 0, 0, 0, 0 };
        DR_TWIN *disable = (DR_TWIN *)ctx->next_packet;
        setTexWindow(disable, &full);
        addPrim(&ot[otz], disable);
        ctx->next_packet += sizeof(DR_TWIN);
    } else {
        for (i = n - 1; i >= 0; i--) addPrim(&ot[otz], polys[i]);
    }
}

/* The body: a camera-facing billboard cut into a 3 x 3 grid of SHARED
   vertices (adapa.c's draw_adp_sprite one row and column further - gula.h on
   why). No backface cull: a camera-facing quad has no back face.

   THE FADES ARE ADDITIVE (ADDING_A_BOSS_ENCOUNTER.txt STEP 5, TRICK 1): below
   fade 256 every piece is semi-transparent with ABR=1 and its colour scaled
   toward black, so at fade 0 it adds nothing over any background. The CLUT
   carries STP on every opaque entry (tools/split_gula_sheet.py). */
#define GULA_GRID       3
#define GULA_GRID_VX    ((GULA_GRID + 1) * (GULA_GRID + 1))   /* 16 */
#define GULA_GRID_QUADS (GULA_GRID * GULA_GRID)              /* 9  */
#define GULA_BAR_W      80     /* the lumberer's 40, doubled for a boss */
#define GULA_BAR_H       5

static void draw_gula_sprite(RenderContext *ctx, int frame) {
    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);

    SVECTOR v[GULA_GRID_VX];
    int r, c;
    for (r = 0; r <= GULA_GRID; r++) {
        int32_t y = GULA_CY - GULA_HALF_H + (2 * GULA_HALF_H * r) / GULA_GRID;
        for (c = 0; c <= GULA_GRID; c++) {
            int32_t  u = -GULA_HALF_W + (2 * GULA_HALF_W * c) / GULA_GRID;
            SVECTOR *p = &v[r * (GULA_GRID + 1) + c];
            p->vx  = (int16_t)(GULA_X + ((u * rx) >> 12));
            p->vy  = (int16_t)y;
            p->vz  = (int16_t)(GULA_Z + ((u * rz) >> 12));
            p->pad = 0;
        }
    }

    /* >>> NO DISTANCE CULL AND NO FOG. <<< She is drawn only while she is up,
       and while she is up she shows through the fog at any range (gula.h) -
       the room's view distance does not move, only she and her inlay do. */

    /* Every vertex through the GTE, three at a time (16 = 5 * 3 + 1). One
       behind the camera (sz 0) rejects the whole body, as the single quad's
       corner test would. */
    DVECTOR sv[GULA_GRID_VX];
    int32_t sz[GULA_GRID_VX];
    int k;
    for (k = 0; k + 2 < GULA_GRID_VX; k += 3) {
        DVECTOR out[3];
        int32_t zz[4];
        gte_ldv3(&v[k], &v[k + 1], &v[k + 2]);
        gte_rtpt();
        gte_stsxy3c(out);
        gte_stsz4c(zz);
        sv[k] = out[0]; sv[k + 1] = out[1]; sv[k + 2] = out[2];
        sz[k] = zz[1];  sz[k + 1] = zz[2];  sz[k + 2] = zz[3];
    }
    for (; k < GULA_GRID_VX; k++) {
        gte_ldv0(&v[k]);
        gte_rtps();
        gte_stsxy(&sv[k]);
        gte_stsz(&sz[k]);
    }
    for (k = 0; k < GULA_GRID_VX; k++) if (!sz[k]) return;

    /* One OT bucket for the whole body: the single quad's, i.e. the GTE's own
       average over the four CORNERS (adapa.c does the same), so she sorts on
       the scale every room's polys sort on. */
    int32_t otz;
    {
        DVECTOR scr[3];
        gte_ldv3(&v[0], &v[GULA_GRID], &v[GULA_GRID_VX - 1 - GULA_GRID]);
        gte_rtpt();
        gte_stsxy3c(scr);
        gte_ldv0(&v[GULA_GRID_VX - 1]);
        gte_rtps();
        gte_avsz4();
        gte_stotz(&otz);
    }
    if (otz <= 0) return;
    if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    int32_t lvl = gula.fade > 255 ? 255 : gula.fade;
    uint8_t cr = (uint8_t)lvl, cg = (uint8_t)lvl, cb = (uint8_t)lvl;
    if (gula.flash > 0) {   /* hit: a red flare for a few frames */
        int32_t hr = lvl * 2; if (hr > 255) hr = 255;
        cr = (uint8_t)hr; cg = (uint8_t)(lvl / 2); cb = (uint8_t)(lvl / 2);
    }

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + GULA_GRID_QUADS * sizeof(POLY_FT4) > buf_end) return;

    /* Inset one texel on every edge so a magnified edge does not sample the
       next frame along. U is 0..127 within the frame's own page; V 128..255. */
    uint8_t uc[GULA_GRID + 1], vr[GULA_GRID + 1];
    for (c = 0; c <= GULA_GRID; c++) uc[c] = (uint8_t)(1 + (125 * c) / GULA_GRID);
    for (r = 0; r <= GULA_GRID; r++) vr[r] = (uint8_t)(129 + (125 * r) / GULA_GRID);

    int      fading = gula.fade < 256;
    uint16_t tpage  = getTPage(1, fading ? 1 : 0, gula_page_x[frame & 3], 128);
    uint16_t clut   = getClut(GULA_CLUT_X, GULA_CLUT_Y);

    POLY_FT4 *quads[GULA_GRID_QUADS];
    int       n = 0;
    for (r = 0; r < GULA_GRID; r++) {
        for (c = 0; c < GULA_GRID; c++) {
            int tl = r * (GULA_GRID + 1) + c;
            int tr = tl + 1;
            int bl = tl + (GULA_GRID + 1);
            int br = bl + 1;

            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, cr, cg, cb);
            if (fading) setSemiTrans(poly, 1);

            poly->x0 = sv[tl].vx; poly->y0 = sv[tl].vy;
            poly->x1 = sv[tr].vx; poly->y1 = sv[tr].vy;
            poly->x2 = sv[bl].vx; poly->y2 = sv[bl].vy;
            poly->x3 = sv[br].vx; poly->y3 = sv[br].vy;

            poly->u0 = uc[c];     poly->v0 = vr[r];
            poly->u1 = uc[c + 1]; poly->v1 = vr[r];
            poly->u2 = uc[c];     poly->v2 = vr[r + 1];
            poly->u3 = uc[c + 1]; poly->v3 = vr[r + 1];

            poly->tpage = tpage;
            poly->clut  = clut;

            ctx->next_packet += sizeof(POLY_FT4);
            quads[n++] = poly;
        }
    }
    add_ft4_run_windowed(ctx, otz, quads, n);

    /* THE HEALTH BAR, while she fights: lumberer.c's two TILEs, wider for a
       boss, centred over her crown and held on screen when the crown is not.
       Not while she fades in (nothing to hurt yet) and not while she dies (an
       empty bar over the death scene - ADDING_A_BOSS_ENCOUNTER.txt STEP 11). */
    if (gula.state != GULA_FIGHT) return;
    {
        int32_t bx = (sv[0].vx + sv[GULA_GRID].vx) / 2 - GULA_BAR_W / 2;
        int32_t by = (sv[0].vy < sv[GULA_GRID].vy ? sv[0].vy : sv[GULA_GRID].vy) - 12;
        if (by < 8) by = 8;
        if (bx < 4) bx = 4;
        if (bx > SCREEN_XRES - GULA_BAR_W - 4) bx = SCREEN_XRES - GULA_BAR_W - 4;
        int32_t bar_otz = otz > 1 ? otz - 1 : 1;

        if (ctx->next_packet + 2 * sizeof(TILE) > buf_end) return;
        TILE *bg = (TILE *)ctx->next_packet;
        setTile(bg);
        setRGB0(bg, 40, 40, 40);
        setXY0(bg, bx, by);
        setWH(bg, GULA_BAR_W, GULA_BAR_H);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[bar_otz + 1], bg);
        ctx->next_packet += sizeof(TILE);

        int32_t fill_w = (gula.health * GULA_BAR_W) / GULA_MAX_HEALTH;
        if (fill_w > 0) {
            TILE *fill = (TILE *)ctx->next_packet;
            setTile(fill);
            setRGB0(fill, 200, 20, 20);
            setXY0(fill, bx, by);
            setWH(fill, fill_w, GULA_BAR_H);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[bar_otz], fill);
            ctx->next_packet += sizeof(TILE);
        }
    }
}

/* One flat, additively blended quad of the wave - lumberer.c's lmb_wave_quad. */
static void gula_wave_quad(RenderContext *ctx, const SVECTOR v[4],
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

    /* The GPU DROPS a primitive past 1023 pixels; throw it away here instead. */
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

/* The wave: a dark blue band racing across the floor with a skirt on its
   leading edge - draw_lmb_shockwave, room-sized. Three things it does that the
   Lumberer's does not, all because this one is 3500 across:

     - it SKIPS segments that cannot be seen: outside the room's bounds (behind
       its walls), past the fog, or well behind the camera. A room-sized ring is
       otherwise 128 quads a frame, most of them inside stone;
     - it FOGS each segment by its own distance, so the far side of the sweep
       goes into the dark the way the room does;
     - it LEAVES A GAP behind each pillar - the same line test that spares the
       player (gula_sheltered), asked of the segment's middle - so the shelter
       the fight turns on is visible in the wave itself. */
#define GULA_ROOM_X0   (-2389)
#define GULA_ROOM_X1     2387
#define GULA_ROOM_Z0        0
#define GULA_ROOM_Z1     4299

static void draw_gula_wave(RenderContext *ctx) {
    int32_t reach = gula_wave_reach();
    if (reach <= 0) return;
    int32_t inner = reach - GULA_WAVE_BAND;
    if (inner < 0) inner = 0;

    int32_t fy = GULA_FLOOR_Y - 4;   /* clear of the floor poly it lies on */
    int32_t fx = isin(cam_rot), fz = icos(cam_rot);

    SVECTOR v[4];
    int k, seg;
    for (k = 0; k < 4; k++) v[k].pad = 0;

    for (seg = 0; seg < GULA_WAVE_SEGS; seg++) {
        int32_t a0 = (seg * 4096) / GULA_WAVE_SEGS;
        int32_t a1 = ((seg + 1) * 4096) / GULA_WAVE_SEGS;
        int32_t am = (a0 + a1) >> 1;

        /* The middle of the leading edge decides whether this segment is
           worth drawing at all. */
        int32_t mx = GULA_X + ((isin(am) * reach) >> 12);
        int32_t mz = GULA_Z + ((icos(am) * reach) >> 12);
        if (mx < GULA_ROOM_X0 || mx > GULA_ROOM_X1 ||
            mz < GULA_ROOM_Z0 || mz > GULA_ROOM_Z1) continue;
        int32_t cdx = mx - cam_x, cdz = mz - cam_z;
        int32_t dist = (cdx < 0 ? -cdx : cdx) + (cdz < 0 ? -cdz : cdz);
        if (dist >= g_fog_far) continue;
        if (((cdx * fx + cdz * fz) >> 12) < -400) continue;   /* behind */
        if (gula_sheltered(mx, mz)) continue;                 /* a pillar's shadow */

        int32_t fs = render_fog_scale(dist);
        if (fs > 256) fs = 256;
        if (fs <= 0)  continue;

        /* Dark blue, and additive: the floor band dim, the skirt brighter. */
        uint8_t gr = (uint8_t)(( 10 * fs) >> 8);
        uint8_t gg = (uint8_t)(( 22 * fs) >> 8);
        uint8_t gb = (uint8_t)((110 * fs) >> 8);
        uint8_t wr = (uint8_t)(( 24 * fs) >> 8);
        uint8_t wg = (uint8_t)(( 44 * fs) >> 8);
        uint8_t wb = (uint8_t)((180 * fs) >> 8);

        int32_t s0 = isin(a0), c0 = icos(a0);
        int32_t s1 = isin(a1), c1 = icos(a1);
        int32_t ix0 = GULA_X + (s0 * inner >> 12), iz0 = GULA_Z + (c0 * inner >> 12);
        int32_t ix1 = GULA_X + (s1 * inner >> 12), iz1 = GULA_Z + (c1 * inner >> 12);
        int32_t ox0 = GULA_X + (s0 * reach >> 12), oz0 = GULA_Z + (c0 * reach >> 12);
        int32_t ox1 = GULA_X + (s1 * reach >> 12), oz1 = GULA_Z + (c1 * reach >> 12);

        /* The band on the ground. */
        v[0].vx = (int16_t)ix0; v[0].vy = (int16_t)fy; v[0].vz = (int16_t)iz0;
        v[1].vx = (int16_t)ox0; v[1].vy = (int16_t)fy; v[1].vz = (int16_t)oz0;
        v[2].vx = (int16_t)ix1; v[2].vy = (int16_t)fy; v[2].vz = (int16_t)iz1;
        v[3].vx = (int16_t)ox1; v[3].vy = (int16_t)fy; v[3].vz = (int16_t)oz1;
        gula_wave_quad(ctx, v, gr, gg, gb);

        /* The skirt standing on its leading edge. -Y is up. */
        v[0].vx = (int16_t)ox0; v[0].vy = (int16_t)(fy - GULA_WAVE_WALL_H); v[0].vz = (int16_t)oz0;
        v[1].vx = (int16_t)ox1; v[1].vy = (int16_t)(fy - GULA_WAVE_WALL_H); v[1].vz = (int16_t)oz1;
        v[2].vx = (int16_t)ox0; v[2].vy = (int16_t)fy;                      v[2].vz = (int16_t)oz0;
        v[3].vx = (int16_t)ox1; v[3].vy = (int16_t)fy;                      v[3].vz = (int16_t)oz1;
        gula_wave_quad(ctx, v, wr, wg, wb);
    }
}

void draw_gula(RenderContext *ctx) {
    if (gula.area != current_area) return;
    if (gula.wave_t > 0) draw_gula_wave(ctx);

    if (gula.state != GULA_FADEIN && gula.state != GULA_FIGHT &&
        gula.state != GULA_DYING) return;
    if (!gula_tex_ok) return;   /* the read failed: nothing to draw her with */
    /* fade 0 is a SKIP, not just invisible (TRICK 1). */
    if (gula.fade <= 0) return;

    int frame = (int)((gula.anim_tick / GULA_ANIM_FRAMES) & 3);
    draw_gula_sprite(ctx, frame);
}
