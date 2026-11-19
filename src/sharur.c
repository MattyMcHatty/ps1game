#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "title.h"
#include "collision.h"
#include "crucifaxe.h"
#include "weapon.h"
#include "sound.h"
#include "particles.h"
#include "demondog.h"
#include "zombie.h"
#include "spider.h"
#include "crawler.h"
#include "tentacle.h"
#include "rafflesia.h"
#include "mushroom.h"
#include "lumberer.h"
#include "ahriman.h"
#include "golem.h"
#include "nessie.h"
#include "maggot.h"
#include "creep.h"
#include "lms_creep.h"
#include "rabisu.h"
#include "living_statue.h"
#include "hadad.h"
#include "asag.h"
#include "asag_fight.h"
#include "asag_arena.h"
#include "gula.h"
#include "lamashtu.h"
#include "adapa.h"
#include "vampire.h"
#include "crate.h"
#include "fatdoor.h"
#include "vines.h"
#include "sharur.h"

/* Sharur - see sharur.h for the design. */

const uint8_t SHR_GLOW_RGB[3] = { 170, 235, 255 };

static SMD  *shr_smd  = NULL;
static void *shr_buff = NULL;

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

void sharur_init(void) {
    CdlFILE file;
    if (!CdSearchFile(&file, "\\TEXUNDW\\SHARURWP.SMD;1")) return;
    int sectors = (file.size + 2047) / 2048;
    shr_buff = malloc(sectors * 2048);
    if (!shr_buff) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)shr_buff, CdlModeSpeed);
    CdReadSync(0, NULL);
    shr_smd = smdInitData(shr_buff);
}

SMD *sharur_model(void) { return shr_smd; }

static int32_t shr_abs(int32_t v) { return v < 0 ? -v : v; }

/* src/arm_switch.c's as_set_view: the SDK's gte_SetRotMatrix/TransMatrix
   clobber $t0/$t1 without saying so (memory: gte-setmatrix-clobber-bug), and
   this file computes a lot across its matrix loads. */
static inline void shr_set_matrix(const MATRIX *m) {
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

void sharur_camera_view(void) {
    MATRIX view;
    camera_build_view(&view);
    shr_set_matrix(&view);
}

/* ---- Screen-space glow -------------------------------------------------------
   spirit_glow's three additive squares (src/spirit_glow.c), centred on a
   screen point at a given outer half-size in pixels. One DR_TPAGE (ABR 1,
   additive) heads the bucket - LIFO, so it is added LAST. */
static void shr_glow_screen(RenderContext *ctx, int sx, int sy, int32_t half,
                            int32_t bright, const uint8_t rgb[3], int otz) {
    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;
    int ring, drew = 0;
    if (bright <= 0) return;
    if (bright > 256) bright = 256;
    if (otz < SCENE_OT_MIN)  otz = SCENE_OT_MIN;
    if (otz > OT_LENGTH - 2) otz = OT_LENGTH - 2;
    for (ring = 0; ring < 3; ring++) {
        int32_t h = half >> ring;
        int32_t level = (bright * (60 + ring * 70)) >> 8;
        POLY_F4 *p;
        if (h < 1) h = 1;
        if (h > 400) h = 400;
        if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) break;
        p = (POLY_F4 *)ctx->next_packet;
        setPolyF4(p);
        setSemiTrans(p, 1);
        setRGB0(p, (uint8_t)((rgb[0] * level) >> 8),
                   (uint8_t)((rgb[1] * level) >> 8),
                   (uint8_t)((rgb[2] * level) >> 8));
        p->x0 = (int16_t)(sx - h); p->y0 = (int16_t)(sy - h);
        p->x1 = (int16_t)(sx + h); p->y1 = (int16_t)(sy - h);
        p->x2 = (int16_t)(sx - h); p->y2 = (int16_t)(sy + h);
        p->x3 = (int16_t)(sx + h); p->y3 = (int16_t)(sy + h);
        addPrim(&ot[otz], p);
        ctx->next_packet += sizeof(POLY_F4);
        drew = 1;
    }
    if (drew && ctx->next_packet + sizeof(DR_TPAGE) <= buf_end) {
        DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
        addPrim(&ot[otz], tp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
}

void sharur_glow_world(RenderContext *ctx, int32_t x, int32_t y, int32_t z,
                       int32_t world_half, int32_t bright, const uint8_t rgb[3]) {
    SVECTOR pt;
    DVECTOR sv;
    int32_t sz;
    pt.vx = (int16_t)x; pt.vy = (int16_t)y; pt.vz = (int16_t)z; pt.pad = 0;
    gte_ldv0(&pt);
    gte_rtps();
    gte_stsxy(&sv);
    gte_stsz(&sz);
    if (sz <= 16) return;
    if (sv.vx <= -1023 || sv.vx >= 1023 || sv.vy <= -1023 || sv.vy >= 1023) return;
    /* In front of the thing it surrounds (that sorts at about sz >> 2). */
    shr_glow_screen(ctx, sv.vx, sv.vy, (world_half * WEAPON_PROJ_H) / sz,
                    bright, rgb, (sz >> 2) - 2);
}

/* ---- The held weapon -------------------------------------------------------- */
static int sq_prev       = 1;   /* seeded "held": a press carried in is not one */
static int charge_armed  = 0;   /* Square went down and is still down           */
static int charge        = 0;   /* frames held, 0..SHR_CHARGE_FRAMES            */
static int tap_queued    = 0;   /* a tap released mid-swing: swing at rest      */
static int glow_clock    = 0;

/* THE FULL-CHARGE FLASH (the user's ask, Nov 2026 - the head's glow alone did
   not say "ready" clearly enough). The frame the charge tops out, the whole
   screen washes in SHR_GLOW_RGB: an ADDITIVE full-screen tile, as Ahriman's
   white flash (ahrimans_draw_flash) but tinted and weaker, held
   SHR_FLASH_HOLD frames then faded over SHR_FLASH_FADE. Same OT slot (3):
   over the scene, under the HUD. */
#define SHR_FLASH_HOLD    3
#define SHR_FLASH_FADE   24
#define SHR_FLASH_PEAK  176          /* of 256: a wash, not a white-out          */
#define OT_SHR_FLASH      3
static int flash_left    = 0;

/* THE CHARGE HUM (SFX_SHARUR, voice 0). A one-shot re-keyed from C every
   SHR_HUM_FRAMES while Square is held past the tap threshold - never a
   hardware loop, which would poison voice 0 (tools/ADDING_A_SOUND.txt STEP
   6). It starts at SHR_TAP_FRAMES, not on the press, so a tap's swing does
   not carry a blip of hum, and it runs on past full charge for as long as
   Square stays down. Keyed off on release, in the menu, and by
   sharur_cancel() - which every put-down, pickup and reset goes through. */
static int hum_on        = 0;
static int hum_clock     = 0;

static void shr_hum_stop(void) {
    if (!hum_on) return;    /* voice 0 is the grind's too: never key it off blind */
    sound_stop(SFX_SHARUR);
    hum_on    = 0;
    hum_clock = 0;
}

static void shr_hum_tick(void) {
    if (!hum_on || ++hum_clock >= SHR_HUM_FRAMES) {
        sound_play(SFX_SHARUR);
        hum_on    = 1;
        hum_clock = 0;
    }
}

/* Sharur's own swing latches - the three enemies the AXE cannot touch at all
   (see sharur_swing_extras). The axe's own latches live in crucifaxe.c and
   reset with melee_start_swing(); these reset with shr_start_swing(). */
static int ex_adapa_hit = 0, ex_rabisu_hit = 0, ex_statue_hit = 0;

static void shr_start_swing(void) {
    melee_start_swing();
    ex_adapa_hit = ex_rabisu_hit = ex_statue_hit = 0;
}

/* The blade (one at a time - a charge takes 3 s). Every enemy it passes
   through is hit ONCE: one bit per slot of every family. */
typedef struct {
    int      active;
    int      dying;            /* frames left of the fade at a wall          */
    int      life;
    int32_t  x, y, z;          /* the "()"'s centre                          */
    int32_t  fx, fz;           /* travel, 4096 = 1                            */
    int32_t  rot;              /* cam_rot it was thrown along                 */
    GameState area;
    uint32_t hit_ddog, hit_zmb, hit_spd, hit_crw, hit_tnt, hit_raf, hit_msh,
             hit_lmb, hit_nss, hit_ahr, hit_glm, hit_mgt, hit_rbs, hit_lst,
             hit_had, hit_crp, hit_lcr;
    uint32_t hit_boss;         /* BOSS_* bits, the one-of-a-kind targets      */
    uint8_t  crate_blows, door_blows, vine_blows;
} Blade;
static Blade blade;

#define BOSS_VAMPIRE   (1u << 0)
#define BOSS_ASAG      (1u << 1)
#define BOSS_GULA      (1u << 2)
#define BOSS_LAMASHTU  (1u << 3)
#define BOSS_ADAPA     (1u << 4)
#define BOSS_BOIL0     5            /* + boil index                              */
_Static_assert(BOSS_BOIL0 + ASAG_BOIL_COUNT <= 32, "boss bits");
_Static_assert(MAX_DEMON_DOGS <= 32 && MAX_ZOMBIES <= 32 && MAX_SPIDERS <= 32 &&
               MAX_CRAWLERS <= 32 && MAX_TENTACLES <= 32 && MAX_RAFFLESIAS <= 32 &&
               MAX_MUSHROOMS <= 32 && MAX_LUMBERERS <= 32 && MAX_NESSIES <= 32 &&
               MAX_AHRIMANS <= 32 && MAX_GOLEMS <= 32 && MAX_MAGGOTS <= 32 &&
               MAX_RABISUS <= 32 && MAX_LIVING_STATUES <= 32 && MAX_HADADS <= 32 &&
               MAX_CREEPS <= 32 && MAX_LMS_CREEPS <= 32,
               "Blade's hit masks are one 32-bit word per family");

static void shr_blade_throw(void) {
    blade.active  = 1;
    blade.dying   = 0;
    blade.life    = SHR_BLADE_LIFE;
    blade.rot     = cam_rot;
    blade.fx      = isin(cam_rot);
    blade.fz      = icos(cam_rot);
    blade.x       = cam_x + ((blade.fx * 60) >> 12);
    blade.y       = cam_y;
    blade.z       = cam_z + ((blade.fz * 60) >> 12);
    blade.area    = current_area;
    blade.hit_ddog = blade.hit_zmb = blade.hit_spd = blade.hit_crw = 0;
    blade.hit_tnt  = blade.hit_raf = blade.hit_msh = blade.hit_lmb = 0;
    blade.hit_nss  = blade.hit_ahr = blade.hit_glm = blade.hit_mgt = 0;
    blade.hit_rbs  = blade.hit_lst = blade.hit_had = blade.hit_crp = 0;
    blade.hit_lcr  = 0;
    blade.hit_boss = 0;
    blade.crate_blows = blade.door_blows = blade.vine_blows = 0;
    sound_play(SFX_FIREBALL);   /* resident: the throw can happen in any room */
}

void sharur_cancel(void) {
    shr_hum_stop();
    charge_armed = 0;
    charge       = 0;
    tap_queued   = 0;
    flash_left   = 0;
    sq_prev      = 1;   /* the next swing needs a fresh press */
}

void sharur_reset(void) {
    sharur_cancel();
    blade.active = 0;
    blade.dying  = 0;
}

/* ---- Sharur's reach past the axe's rules ------------------------------------
   Sharur affects EVERY enemy in the game (the user's rule, Nov 2026: it is
   meant to come round again in a New Game +). The axe's hit tests in
   crucifaxe.c already cover everything the axe can hurt, and Sharur runs them
   at double damage. Three enemies the axe cannot hurt AT ALL, by their design,
   and Sharur reaches each of them here, at the same swing reach (to the body's
   surface, facing):
     ADAPA          immune to everything but the Helluminator - not to Sharur.
     THE RABISU     the axe is only its parry (rabisu.c reads swing_timer, and
                    still does - a Sharur swing parries as well as wounds).
     LIVING STATUES the axe lands only on one that is striking; Sharur lands
                    in any state and wakes it (living_statue_sharur). One that
                    IS striking is the axe's test's, at melee_damage.
   Bosses that are only open at a weak point or in a window (Asag's head, the
   spot on Lamashtu's back, Hadad off his plinth) keep those rules: the axe's
   tests already reach them there, at Sharur's damage. */
static int shr_swing_reaches(int32_t ex, int32_t ey, int32_t ez,
                             int32_t hw, int32_t hh) {
    int32_t dx = ex - cam_x, dz = ez - cam_z;
    int32_t gh = shr_abs(dx) + shr_abs(dz) - hw;
    int32_t gv = shr_abs(ey - cam_y) - hh;
    if (gh < 0) gh = 0;
    if (gv < 0) gv = 0;
    if (gh + gv >= SWING_RANGE) return 0;
    return ((dx * isin(cam_rot) + dz * icos(cam_rot)) >> 12) > 0;
}

static void sharur_swing_extras(void) {
    int i;
    int32_t cy, hh, hw;
    if (swing_timer <= 0 || swing_timer > SWING_DURATION) return;

    if (!ex_adapa_hit && adapa_burnable()) {
        int32_t ax, ay, az;
        adapa_body(&ax, &ay, &az);
        if (shr_swing_reaches(ax, ay, az, ADP_HALF_W, ADP_HALF_H)) {
            adapa_burn(SHR_DAMAGE);
            ex_adapa_hit = 1;
        }
    }
    for (i = 0; !ex_rabisu_hit && i < rabisu_count; i++) {
        Rabisu *rb = &rabisus[i];
        if (!rb->active || rb->dead || rb->dying || rb->area != current_area) continue;
        rabisu_body(rb, &cy, &hh, &hw);
        if (!shr_swing_reaches(rb->x, cy, rb->z, hw, hh)) continue;
        rabisu_damage(rb, SHR_DAMAGE);
        ex_rabisu_hit = 1;
    }
    for (i = 0; !ex_statue_hit && i < living_statue_count; i++) {
        LivingStatue *s = &living_statues[i];
        if (!s->active || s->state == LST_DEAD || s->state == LST_ATTACK ||
            s->area != current_area) continue;
        living_statue_body(s, &cy, &hh, &hw);
        if (!shr_swing_reaches(s->x, cy, s->z, hw, hh)) continue;
        living_statue_sharur(s, SHR_DAMAGE);
        ex_statue_hit = 1;
    }
}

/* PRESS AND HOLD CHARGES; ONLY A TAP SWINGS (the user's correction, Nov 2026 -
   the first cut swung on every press and charged after). So the swing waits
   for the RELEASE:
     released before SHR_TAP_FRAMES   a tap: the swing (queued if the last
                                      one is still out, so quick taps chain)
     released at a full charge        the swing AND the blade
     released anywhere between        the swing alone, no blade (the user's
                                      change, Nov 2026 - it used to do nothing) */
void sharur_update(void) {
    int sq_held = 0, sq_just;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        sq_held = (~pad->btn & PAD_SQUARE) ? 1 : 0;
    }
    sq_just = sq_held && !sq_prev;
    sq_prev = sq_held;
    glow_clock++;
    if (flash_left > 0) flash_left--;

    if (game_state == STATE_MENU) {
        shr_hum_stop();
        charge_armed = 0;
        charge       = 0;
        tap_queued   = 0;
        flash_left   = 0;
    } else if (sq_just) {
        shr_hum_stop();
        charge_armed = 1;
        charge       = 0;
    } else if (charge_armed && sq_held) {
        if (charge < SHR_CHARGE_FRAMES && ++charge == SHR_CHARGE_FRAMES)
            flash_left = SHR_FLASH_HOLD + SHR_FLASH_FADE;   /* charged: flash once */
        if (charge >= SHR_TAP_FRAMES) shr_hum_tick();
    } else if (charge_armed) {
        shr_hum_stop();         /* released: the hum ends whatever happens next */
        if (charge >= SHR_CHARGE_FRAMES) {
            shr_start_swing();
            shr_blade_throw();
            tap_queued = 0;
        } else {
            tap_queued = 1;     /* a tap or a part charge: the swing, no blade */
        }
        charge_armed = 0;
        charge       = 0;
    }
    if (tap_queued && swing_timer == 0) {
        shr_start_swing();
        tap_queued = 0;
    }

    melee_damage = SHR_DAMAGE;
    sharur_swing_extras();
    melee_swing_tick(SHR_SWING_STEP);
    melee_damage = 1;
}

/* THE CHARGE GLOW IS THE HEAD ITSELF (the user's correction, Nov 2026 - the
   first cut hung one ring of light on the head's centre). Every face of the
   mace's pale blue-white head is redrawn twice over the flat-shaded mace,
   ADDITIVELY: once where it is, so the face itself burns toward white, and once
   a quarter bigger round the head's centre, dimmer, as its halo. Only the head
   - the gold band, the red tip and the haft stay as they are. About a dozen
   faces, two passes: nothing against the room. */
#define SHR_HEAD_CY    (-82)        /* the head's centre in the model            */
static int shr_is_head_face(const uint8_t *col) {
    return col[0] >= 150 && col[1] >= 200 && col[2] >= 200;   /* (186,255,241) */
}

static void shr_draw_head_glow(RenderContext *ctx, const MATRIX *vs, int32_t level) {
    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;
    int pass, pi, drew = 0;
    if (level <= 0 || !shr_smd) return;
    if (level > 256) level = 256;
    shr_set_matrix(vs);

    for (pass = 0; pass < 2; pass++) {
        /* pass 0: the face, strong. pass 1: the halo, 1.25x, faint. */
        int32_t lv = pass == 0 ? (level * 200) >> 8 : (level * 90) >> 8;
        uint8_t r = (uint8_t)((SHR_GLOW_RGB[0] * lv) >> 8);
        uint8_t g = (uint8_t)((SHR_GLOW_RGB[1] * lv) >> 8);
        uint8_t b = (uint8_t)((SHR_GLOW_RGB[2] * lv) >> 8);
        uint8_t *p = (uint8_t *)shr_smd->p_prims;
        for (pi = 0; pi < shr_smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt      = (SMD_PRI_TYPE *)p;
            uint8_t       stride  = pt->len;
            int           is_quad = (pt->type >= 2);
            uint16_t     *vi      = (uint16_t *)(p + 4);
            SVECTOR v[4];
            DVECTOR sv[4];
            int32_t sz[4], nclip;
            int k, nv = is_quad ? 4 : 3;
            if (!shr_is_head_face(p + 16)) { p += stride; continue; }
            for (k = 0; k < nv; k++) {
                v[k] = shr_smd->p_verts[vi[k]];
                if (pass == 1) {
                    v[k].vx = (int16_t)((v[k].vx * 5) / 4);
                    v[k].vy = (int16_t)(SHR_HEAD_CY + ((v[k].vy - SHR_HEAD_CY) * 5) / 4);
                    v[k].vz = (int16_t)((v[k].vz * 5) / 4);
                }
            }
            gte_ldv3(&v[0], &v[1], &v[2]);
            gte_rtpt();
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }   /* front faces only */
            gte_stsxy3c(sv);
            gte_stsz4c(sz);
            if (sz[1] <= 0 || sz[2] <= 0 || sz[3] <= 0) { p += stride; continue; }
            if (is_quad) {
                POLY_F4 *q;
                gte_ldv0(&v[3]);
                gte_rtps();
                gte_stsxy(&sv[3]);
                if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) break;
                q = (POLY_F4 *)ctx->next_packet;
                setPolyF4(q);
                setSemiTrans(q, 1);
                setRGB0(q, r, g, b);
                q->x0 = sv[0].vx; q->y0 = sv[0].vy;
                q->x1 = sv[1].vx; q->y1 = sv[1].vy;
                q->x2 = sv[2].vx; q->y2 = sv[2].vy;
                q->x3 = sv[3].vx; q->y3 = sv[3].vy;
                addPrim(&ot[SCENE_OT_MIN], q);
                ctx->next_packet += sizeof(POLY_F4);
            } else {
                POLY_F3 *q;
                if (ctx->next_packet + sizeof(POLY_F3) + sizeof(DR_TPAGE) > buf_end) break;
                q = (POLY_F3 *)ctx->next_packet;
                setPolyF3(q);
                setSemiTrans(q, 1);
                setRGB0(q, r, g, b);
                q->x0 = sv[0].vx; q->y0 = sv[0].vy;
                q->x1 = sv[1].vx; q->y1 = sv[1].vy;
                q->x2 = sv[2].vx; q->y2 = sv[2].vy;
                addPrim(&ot[SCENE_OT_MIN], q);
                ctx->next_packet += sizeof(POLY_F3);
            }
            drew = 1;
            p += stride;
        }
    }
    if (drew && ctx->next_packet + sizeof(DR_TPAGE) <= buf_end) {
        /* Added LAST so it heads the bucket (LIFO): additive for all of them. */
        DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
        addPrim(&ot[SCENE_OT_MIN], tp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
    sharur_camera_view();
}

static void shr_draw_flash(RenderContext *ctx) {
    int32_t k;
    uint8_t *buf_end;
    TILE *t;
    DR_TPAGE *dp;
    if (flash_left <= 0) return;
    k = flash_left >= SHR_FLASH_FADE ? SHR_FLASH_PEAK
                                     : (flash_left * SHR_FLASH_PEAK) / SHR_FLASH_FADE;
    buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) + sizeof(DR_TPAGE) > buf_end) return;

    t = (TILE *)ctx->next_packet;
    setTile(t);
    setSemiTrans(t, 1);
    setRGB0(t, (uint8_t)((SHR_GLOW_RGB[0] * k) >> 8),
               (uint8_t)((SHR_GLOW_RGB[1] * k) >> 8),
               (uint8_t)((SHR_GLOW_RGB[2] * k) >> 8));
    setXY0(t, 0, 0);
    setWH(t, SCREEN_XRES, SCREEN_YRES);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_SHR_FLASH], t);
    ctx->next_packet += sizeof(TILE);

    dp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(dp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_SHR_FLASH], dp);
    ctx->next_packet += sizeof(DR_TPAGE);
}

void sharur_draw(RenderContext *ctx) {
    MATRIX  vs;
    int32_t level;
    shr_draw_flash(ctx);
    melee_draw_model(ctx, shr_smd, 4096, &vs);
    if (charge <= 0) return;
    level = (charge * 256) / SHR_CHARGE_FRAMES;
    if (charge >= SHR_CHARGE_FRAMES)            /* full: it pulses */
        level = 224 + (isin((glow_clock * 60) & 4095) >> 6);
    shr_draw_head_glow(ctx, &vs, level);
}

/* ---- The blade ---------------------------------------------------------------- */

/* Is a body at (ex, ey, ez), `r` across and `hh` tall, inside the stretch of
   air the blade swept this frame - from SHR_BLADE_SPEED behind its centre to
   its centre - and within its "()" across and up? */
static int blade_covers(int32_t ex, int32_t ey, int32_t ez, int32_t r, int32_t hh) {
    int32_t dx = ex - blade.x, dz = ez - blade.z;
    int32_t along = (dx * blade.fx + dz * blade.fz) >> 12;
    int32_t side  = (dx * blade.fz - dz * blade.fx) >> 12;
    if (along < -SHR_BLADE_SPEED - r || along > r) return 0;
    if (shr_abs(side) > SHR_BLADE_HALF_W + r) return 0;
    if (shr_abs(ey - blade.y) > SHR_BLADE_HALF_H + hh) return 0;
    return 1;
}

/* A shove along the blade's line, at `speed`. */
#define BLADE_KB(VX, VZ, SPEED) do { \
        (VX) = (blade.fx * (SPEED)) >> 12; \
        (VZ) = (blade.fz * (SPEED)) >> 12; } while (0)

/* EVERY ENEMY IN THE GAME (the user's rule - see sharur_swing_extras). The
   list is the Helluminator's burn sweep (hell_burn_tick), which is the
   longest in the game, plus the two it leaves out on purpose - Hadad and the
   crib's Creeps - and with Adapa, the Rabisu and the Living Statues let in on
   Sharur's terms. Each slot is hit ONCE per blade. Damage is SHR_BLADE_DAMAGE
   swings' worth in each enemy's own swing units (x HP_SCALE for the two that
   count half points). Shoved, at twice the axe's own shove, wherever the axe
   shoves; never where the axe does not (rooted, teleporting, or one hit point). */
#define BLADE_DMG SHR_BLADE_DAMAGE
#define SEEN(MASK, I) ((MASK) & (1u << (I)))
#define MARK(MASK, I) ((MASK) |= (1u << (I)))
static void sharur_blade_hits(void) {
    int i;
    int32_t cy, hh, hw;

    for (i = 0; i < demon_dog_count; i++) {
        DemonDog *d = &demon_dogs[i];
        if (SEEN(blade.hit_ddog, i) || !d->active || d->state == DDOG_DEAD) continue;
        if (!blade_covers(d->x, d->y + DDOG_Y_OFFSET, d->z, DDOG_HALF_W, DDOG_HALF_H)) continue;
        MARK(blade.hit_ddog, i);
        BLADE_KB(d->kb_vx, d->kb_vz, DDOG_KNOCKBACK * 2);
        demon_dog_damage(d, BLADE_DMG);
    }
    for (i = 0; i < zombie_count; i++) {
        Zombie *z = &zombies[i];
        if (SEEN(blade.hit_zmb, i) || !z->active || z->state == ZMB_DEAD) continue;
        if (!blade_covers(z->x, z->y + ZMB_Y_OFFSET, z->z, ZMB_HALF_W, ZMB_HALF_H)) continue;
        MARK(blade.hit_zmb, i);
        BLADE_KB(z->kb_vx, z->kb_vz, ZMB_KNOCKBACK * 2);
        zombie_damage(z, BLADE_DMG);
    }
    for (i = 0; i < spider_count; i++) {
        Spider *s = &spiders[i];
        if (SEEN(blade.hit_spd, i) || !s->active || s->state == SPD_DEAD ||
            s->area != current_area) continue;
        if (!blade_covers(s->x, s->y + SPD_Y_OFFSET, s->z, SPD_HALF_W, SPD_HALF_H)) continue;
        MARK(blade.hit_spd, i);
        if (s->state == SPD_ALERT) BLADE_KB(s->kb_vx, s->kb_vz, SPD_KNOCKBACK * 2);
        spider_damage(s, BLADE_DMG);
    }
    for (i = 0; i < crawler_count; i++) {
        Crawler *s = &crawlers[i];
        if (SEEN(blade.hit_crw, i) || !s->active || s->state == CRW_DEAD ||
            s->area != current_area) continue;
        if (!blade_covers(s->x, s->y + CRW_Y_OFFSET, s->z, CRW_HALF_W, CRW_HALF_H)) continue;
        MARK(blade.hit_crw, i);
        crawler_damage(s, BLADE_DMG);
    }
    for (i = 0; i < tentacle_count; i++) {
        Tentacle *t = &tentacles[i];
        if (SEEN(blade.hit_tnt, i) || !t->active || t->health <= 0 ||
            t->area != current_area) continue;
        tentacle_body(t, &cy, &hh, &hw);
        if (!blade_covers(t->x, cy, t->z, hw, hh)) continue;
        MARK(blade.hit_tnt, i);
        tentacle_shoot(t, BLADE_DMG);
    }
    for (i = 0; i < rafflesia_count; i++) {
        Rafflesia *rf = &rafflesias[i];
        if (SEEN(blade.hit_raf, i) || !rf->active || rf->health <= 0 ||
            rf->area != current_area) continue;
        rafflesia_body(rf, &cy, &hh, &hw);
        if (!blade_covers(rf->x, cy, rf->z, hw, hh)) continue;
        MARK(blade.hit_raf, i);
        rafflesia_shoot(rf, BLADE_DMG);
    }
    for (i = 0; i < mushroom_count; i++) {
        Mushroom *m = &mushrooms[i];
        if (SEEN(blade.hit_msh, i) || !m->active || m->state == MSH_DEAD ||
            m->area != current_area) continue;
        mushroom_body(m, &cy, &hh, &hw);
        if (!blade_covers(m->x, cy, m->z, hw, hh)) continue;
        MARK(blade.hit_msh, i);
        if (!mushroom_airborne(m)) BLADE_KB(m->kb_vx, m->kb_vz, MSH_KNOCKBACK * 2);
        mushroom_damage(m, BLADE_DMG);
    }
    for (i = 0; i < lumberer_count; i++) {
        Lumberer *l = &lumberers[i];
        if (SEEN(blade.hit_lmb, i) || !l->active || l->state == LMB_DEAD ||
            l->area != current_area) continue;
        lumberer_body(l, &cy, &hh, &hw);
        if (!blade_covers(l->x, cy, l->z, hw, hh)) continue;
        MARK(blade.hit_lmb, i);
        BLADE_KB(l->kb_vx, l->kb_vz, LMB_KNOCKBACK * 2);
        lumberer_damage(l, BLADE_DMG);
    }
    /* NESSIE: no shove - it would only push her head out of her lava. */
    for (i = 0; i < nessie_count; i++) {
        Nessie *s = &nessies[i];
        if (SEEN(blade.hit_nss, i) || !s->active || s->state == NSS_DEAD ||
            s->area != current_area) continue;
        nessie_body(s, &cy, &hh, &hw);
        if (!blade_covers(s->x, cy, s->z, hw, hh)) continue;
        MARK(blade.hit_nss, i);
        nessie_damage(s, BLADE_DMG);
    }
    for (i = 0; i < ahriman_count; i++) {
        Ahriman *s = &ahrimans[i];
        if (SEEN(blade.hit_ahr, i) || !s->active || s->state == AHR_DEAD ||
            s->area != current_area) continue;
        ahriman_body(s, &cy, &hh, &hw);
        if (!blade_covers(s->x, cy, s->z, hw, hh)) continue;
        MARK(blade.hit_ahr, i);
        BLADE_KB(s->kb_vx, s->kb_vz, AHR_KNOCKBACK * 2);
        ahriman_damage(s, AHR_HP_SCALE * BLADE_DMG);
    }
    /* THE GOLEM: no shove - it never moves. */
    for (i = 0; i < golem_count; i++) {
        Golem *s = &golems[i];
        if (SEEN(blade.hit_glm, i) || !s->active || s->state == GLM_DEAD ||
            s->area != current_area) continue;
        golem_body(s, &cy, &hh, &hw);
        if (!blade_covers(s->x, cy, s->z, hw, hh)) continue;
        MARK(blade.hit_glm, i);
        golem_damage(s, GLM_HP_SCALE * BLADE_DMG);
    }
    for (i = 0; i < maggot_count; i++) {
        Maggot *mg = &maggots[i];
        if (SEEN(blade.hit_mgt, i) || !mg->active || mg->state == MGT_DEAD ||
            mg->area != current_area) continue;
        if (!blade_covers(mg->x, maggot_body_y(mg), mg->z, MGT_HALF_W, MGT_HALF_H)) continue;
        MARK(blade.hit_mgt, i);
        maggot_damage(mg, BLADE_DMG);
    }
    for (i = 0; i < creep_count; i++) {
        Creep *c = &creeps[i];
        if (SEEN(blade.hit_crp, i) || !c->active || c->state == CRP_DEAD ||
            c->area != current_area) continue;
        if (!blade_covers(c->x, creep_body_y(c), c->z, CRP_HALF_W, CRP_HALF_H)) continue;
        MARK(blade.hit_crp, i);
        creep_damage(c, BLADE_DMG);
    }
    for (i = 0; i < lms_creep_count; i++) {
        LmsCreep *lc = &lms_creeps[i];
        if (SEEN(blade.hit_lcr, i) || !lc->active || lc->state == LCR_DEAD ||
            lc->area != current_area) continue;
        if (!blade_covers(lc->x, lms_creep_body_y(lc), lc->z, LCR_HALF_W, LCR_HALF_H)) continue;
        MARK(blade.hit_lcr, i);
        lms_creep_damage(lc, BLADE_DMG);
    }
    /* THE RABISU: wounded by Sharur, where the axe only parries. */
    for (i = 0; i < rabisu_count; i++) {
        Rabisu *rb = &rabisus[i];
        if (SEEN(blade.hit_rbs, i) || !rb->active || rb->dead || rb->dying ||
            rb->area != current_area) continue;
        rabisu_body(rb, &cy, &hh, &hw);
        if (!blade_covers(rb->x, cy, rb->z, hw, hh)) continue;
        MARK(blade.hit_rbs, i);
        rabisu_damage(rb, BLADE_DMG);
    }
    /* LIVING STATUES: in any state, waking them (living_statue_sharur). */
    for (i = 0; i < living_statue_count; i++) {
        LivingStatue *s = &living_statues[i];
        if (SEEN(blade.hit_lst, i) || !s->active || s->state == LST_DEAD ||
            s->area != current_area) continue;
        living_statue_body(s, &cy, &hh, &hw);
        if (!blade_covers(s->x, cy, s->z, hw, hh)) continue;
        MARK(blade.hit_lst, i);
        living_statue_sharur(s, BLADE_DMG);
    }
    /* HADAD: hadad_damage is a no-op outside his fight (on the plinth, gone),
       as it is for the axe. */
    for (i = 0; i < hadad_count; i++) {
        Hadad *h = &hadads[i];
        if (SEEN(blade.hit_had, i) || !h->active || h->state == HAD_DEAD ||
            h->area != current_area) continue;
        hadad_body(h, &cy, &hh, &hw);
        if (!blade_covers(h->x, cy, h->z, hw, hh)) continue;
        MARK(blade.hit_had, i);
        hadad_damage(h, BLADE_DMG);
    }
    /* ASAG: his head while exposed, and each boil while it stands - the
       fight's own windows, as every weapon keeps them. */
    if (!(blade.hit_boss & BOSS_ASAG) && asag_exposed()) {
        int32_t hx, hy, hz;
        if (asag_head_box(&hx, &hy, &hz, &hw, &hh) && blade_covers(hx, hy, hz, hw, hh)) {
            blade.hit_boss |= BOSS_ASAG;
            asag_damage(BLADE_DMG);
        }
    }
    for (i = 0; i < ASAG_BOIL_COUNT; i++) {
        int32_t bx, by, bz;
        if (blade.hit_boss & (1u << (BOSS_BOIL0 + i))) continue;
        if (!asag_boil_target(i, &bx, &by, &bz, &hw, &hh)) continue;
        if (!blade_covers(bx, by, bz, hw, hh)) continue;
        blade.hit_boss |= (1u << (BOSS_BOIL0 + i));
        asag_boil_damage(i, BLADE_DMG);
    }
    if (!(blade.hit_boss & BOSS_GULA) && gula_hittable()) {
        int32_t gx, gz;
        gula_body(&gx, &cy, &gz, &hh, &hw);
        if (blade_covers(gx, cy, gz, hw, hh)) {
            blade.hit_boss |= BOSS_GULA;
            gula_damage(BLADE_DMG);
        }
    }
    /* LAMASHTU: the spot on her back, as for every weapon. */
    if (!(blade.hit_boss & BOSS_LAMASHTU)) {
        int32_t lx, lz;
        if (lamashtu_anus_target(&lx, &cy, &lz, &hw, &hh) &&
            blade_covers(lx, cy, lz, hw, hh)) {
            blade.hit_boss |= BOSS_LAMASHTU;
            lamashtu_damage(BLADE_DMG);
        }
    }
    /* ADAPA: immune to everything but the lantern - and Sharur. */
    if (!(blade.hit_boss & BOSS_ADAPA) && adapa_burnable()) {
        int32_t ax, az;
        adapa_body(&ax, &cy, &az);
        if (blade_covers(ax, cy, az, ADP_HALF_W, ADP_HALF_H)) {
            blade.hit_boss |= BOSS_ADAPA;
            adapa_burn(BLADE_DMG);
        }
    }
    if (!(blade.hit_boss & BOSS_VAMPIRE) && vampire_health > 0 &&
        blade_covers(vampire_x, vampire_y + VAMPIRE_Y, vampire_z,
                     VAMPIRE_HALF_W, VAMPIRE_HALF_H)) {
        blade.hit_boss |= BOSS_VAMPIRE;
        BLADE_KB(vampire_kb_vx, vampire_kb_vz, KNOCKBACK_SPEED * 2);
        vampire_health   -= BLADE_DMG;
        vampire_hit_timer = VAMPIRE_BAR_TIMER_MAX;
        if (vampire_health <= 0)
            spawn_blood_burst(vampire_x, vampire_y, vampire_z);
    }

    /* What the axe breaks, through the axe's own tests run FROM THE BLADE: they
       all read cam_x/y/z/rot, so those are lent to the blade for the calls and
       put straight back. One blow a frame per family, SHR_BLADE_DAMAGE at most. */
    {
        int32_t sx = cam_x, sy = cam_y, sz = cam_z, sr = cam_rot;
        cam_x = blade.x; cam_y = blade.y; cam_z = blade.z; cam_rot = blade.rot;
        if (blade.crate_blows < SHR_BLADE_DAMAGE && crate_try_smash())    blade.crate_blows++;
        if (blade.door_blows  < SHR_BLADE_DAMAGE && fatdoors_try_smash()) blade.door_blows++;
        if (blade.vine_blows  < SHR_BLADE_DAMAGE && vines_try_smash())    blade.vine_blows++;
        cam_x = sx; cam_y = sy; cam_z = sz; cam_rot = sr;
    }
}
#undef SEEN
#undef MARK
#undef BLADE_DMG
#undef BLADE_KB

void sharur_blade_update(void) {
    int32_t nx, nz;
    if (!blade.active) return;
    if (blade.area != current_area) { blade.active = 0; return; }
    if (blade.dying) {
        if (--blade.dying <= 0) blade.active = 0;
        return;
    }
    if (--blade.life <= 0) { blade.dying = 10; return; }

    nx = blade.x + ((blade.fx * SHR_BLADE_SPEED) >> 12);
    nz = blade.z + ((blade.fz * SHR_BLADE_SPEED) >> 12);
    /* Hits first, at the new position, so a crate in the way is broken before
       its box can stop the blade below. */
    {
        int32_t ox = blade.x, oz = blade.z;
        blade.x = nx; blade.z = nz;
        sharur_blade_hits();
        blade.x = ox; blade.z = oz;
    }
    /* A wall across its middle OR its upper half stops it. One test at the
       centre let a blade thrown from the floor fly on through the Chamber's
       plinths: its centre stays at the thrower's eye, under the dais the
       plinths stand on, and the plinths' walls span only their own height
       above it (headless self-test, Nov 2026). */
    /* >>> collision_segment_blocked TAKES CAMERA-OFFSET Y <<< and adds
       GROUND_FLOOR_Y to gate walls by WORLD height (a shot is measured from
       the shooter's feet). blade.y is where the light is DRAWN, a world height,
       so it comes off first or both tests sit 149 too low. */
    if (collision_segment_blocked(blade.x, blade.y - GROUND_FLOOR_Y, blade.z,
                                  nx, blade.y - GROUND_FLOOR_Y, nz) ||
        collision_segment_blocked(blade.x, blade.y - SHR_BLADE_HALF_H / 2 - GROUND_FLOOR_Y,
                                  blade.z, nx,
                                  blade.y - SHR_BLADE_HALF_H / 2 - GROUND_FLOOR_Y, nz)) {
        spawn_burst(blade.x, blade.y, blade.z,
                    SHR_GLOW_RGB[0], SHR_GLOW_RGB[1], SHR_GLOW_RGB[2]);
        blade.dying = 10;
        return;
    }
    blade.x = nx;
    blade.z = nz;
}

/* THE "()" (the user's correction, Nov 2026 - it was one ')'). Seen from
   behind, from the thrower: two crescents standing up back to back, '(' on the
   left bulging left and ')' on the right bulging right - an upright oval of
   light split at top and bottom, its tips trailing back a little so it has
   depth as it flies. Each crescent is two Gouraud strips meeting on a bright
   spine (outer edge -> spine -> inner edge), additive, so it reads as light. */
#define SHR_ARC_SEGS   8
void sharur_blade_draw(RenderContext *ctx) {
    uint8_t  *buf_end;
    uint32_t *ot;
    SVECTOR   pts[3][SHR_ARC_SEGS + 1];
    DVECTOR   scr[2][3][SHR_ARC_SEGS + 1];
    int32_t   sz, fade, otz;
    int32_t   rx, rz;                       /* right, 4096 = 1 */
    int       i, k, side;

    if (!blade.active) return;
    buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    ot      = ctx->buffers[ctx->active_buffer].ot;

    {
        SVECTOR c0;
        c0.vx = (int16_t)blade.x; c0.vy = (int16_t)blade.y; c0.vz = (int16_t)blade.z; c0.pad = 0;
        gte_ldv0(&c0);
        gte_rtps();
        gte_stsz(&sz);
        if (sz <= 32) return;
    }

    fade = 256;
    if (blade.dying) fade = (blade.dying * 256) / 10;
    {
        /* Fogged with the room, like everything else in it. */
        int32_t dist = shr_abs(blade.x - cam_x) + shr_abs(blade.z - cam_z);
        fade = (fade * render_fog_scale(dist)) >> 8;
        if (fade <= 0) return;
    }
    rx = blade.fz;  rz = -blade.fx;         /* right = (fz, -fx) */

    /* Each crescent: theta across +-75 degrees (853/4096 of a turn), lateral
       = side * the half-ellipse's x, so the two meet nowhere and leave a gap of
       ~90 between their tips. Thickness tapers to nothing at the tips. Edge 0
       = outer, 1 = spine, 2 = inner. Projected, and the whole blade dropped if
       any point is behind the eye or far off screen (no clipping needed). */
    for (side = 0; side < 2; side++) {
        int32_t sgn = side == 0 ? -1 : 1;
        for (i = 0; i <= SHR_ARC_SEGS; i++) {
            int32_t th  = -853 + (1706 * i) / SHR_ARC_SEGS;
            int32_t c   = icos(th), s = isin(th);
            int32_t c75 = icos(853);
            int32_t thick = (c > c75) ? ((c - c75) * 70) / (4096 - c75) : 0;
            int32_t back  = ((4096 - c) * 90) >> 12;          /* tips trail back */
            int32_t up    = (s * SHR_BLADE_HALF_H) >> 12;
            for (k = 0; k < 3; k++) {
                int32_t rad = SHR_BLADE_HALF_W - (thick * k) / 2;
                int32_t lat = sgn * ((c * rad) >> 12);
                pts[k][i].vx = (int16_t)(blade.x + ((rx * lat) >> 12) - ((blade.fx * back) >> 12));
                pts[k][i].vy = (int16_t)(blade.y - up);
                pts[k][i].vz = (int16_t)(blade.z + ((rz * lat) >> 12) - ((blade.fz * back) >> 12));
                pts[k][i].pad = 0;
            }
        }
        for (k = 0; k < 3; k++)
            for (i = 0; i <= SHR_ARC_SEGS; i++) {
                int32_t vz;
                gte_ldv0(&pts[k][i]);
                gte_rtps();
                gte_stsxy(&scr[side][k][i]);
                gte_stsz(&vz);
                if (vz <= 16) return;
                if (scr[side][k][i].vx <= -1023 || scr[side][k][i].vx >= 1023 ||
                    scr[side][k][i].vy <= -1023 || scr[side][k][i].vy >= 1023) return;
            }
    }

    otz = (sz >> 2);
    if (otz < SCENE_OT_MIN)  otz = SCENE_OT_MIN;
    if (otz > OT_LENGTH - 2) otz = OT_LENGTH - 2;

    for (side = 0; side < 2; side++)
        for (k = 0; k < 2; k++)
            for (i = 0; i < SHR_ARC_SEGS; i++) {
                POLY_G4 *p;
                DVECTOR (*sc)[SHR_ARC_SEGS + 1] = scr[side];
                /* k=0 strip is outer->spine, k=1 strip is spine->inner. */
                int32_t lo = (fade * 50) >> 8, hi = fade;
                int32_t a = (k == 0) ? lo : hi, b = (k == 0) ? hi : lo;
                if (ctx->next_packet + sizeof(POLY_G4) + sizeof(DR_TPAGE) > buf_end) goto done;
                p = (POLY_G4 *)ctx->next_packet;
                setPolyG4(p);
                setSemiTrans(p, 1);
                setRGB0(p, (SHR_GLOW_RGB[0] * a) >> 8, (SHR_GLOW_RGB[1] * a) >> 8, (SHR_GLOW_RGB[2] * a) >> 8);
                setRGB1(p, (SHR_GLOW_RGB[0] * a) >> 8, (SHR_GLOW_RGB[1] * a) >> 8, (SHR_GLOW_RGB[2] * a) >> 8);
                setRGB2(p, (SHR_GLOW_RGB[0] * b) >> 8, (SHR_GLOW_RGB[1] * b) >> 8, (SHR_GLOW_RGB[2] * b) >> 8);
                setRGB3(p, (SHR_GLOW_RGB[0] * b) >> 8, (SHR_GLOW_RGB[1] * b) >> 8, (SHR_GLOW_RGB[2] * b) >> 8);
                p->x0 = sc[k][i].vx;         p->y0 = sc[k][i].vy;
                p->x1 = sc[k][i + 1].vx;     p->y1 = sc[k][i + 1].vy;
                p->x2 = sc[k + 1][i].vx;     p->y2 = sc[k + 1][i].vy;
                p->x3 = sc[k + 1][i + 1].vx; p->y3 = sc[k + 1][i + 1].vy;
                addPrim(&ot[otz], p);
                ctx->next_packet += sizeof(POLY_G4);
            }
done:
    if (ctx->next_packet + sizeof(DR_TPAGE) <= buf_end) {
        DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
        addPrim(&ot[otz], tp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
}

/* ---- The mace in the world (the plinth) ------------------------------------- */
void sharur_draw_world(RenderContext *ctx, int32_t x, int32_t y, int32_t z,
                       int32_t yaw, int32_t scale, int32_t gain) {
    const int32_t lx = 2365, ly = -2365, lz = 2365;   /* weapon_render_model's light */
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot     = ctx->buffers[ctx->active_buffer].ot;
    MATRIX   view, model, comb;
    SVECTOR  rot;
    VECTOR   sc;
    int32_t  fog, dist;
    uint8_t *p;
    int      pi;

    if (!shr_smd) return;
    dist = shr_abs(x - cam_x) + shr_abs(z - cam_z);
    fog  = render_fog_scale(dist);
    if (fog <= 0) return;

    rot.vx = 0; rot.vy = (int16_t)yaw; rot.vz = 0; rot.pad = 0;
    RotMatrix(&rot, &model);
    sc.vx = scale; sc.vy = scale; sc.vz = scale;
    ScaleMatrix(&model, &sc);
    model.t[0] = x; model.t[1] = y; model.t[2] = z;
    camera_build_view(&view);
    CompMatrixLV(&view, &model, &comb);
    shr_set_matrix(&comb);

    p = (uint8_t *)shr_smd->p_prims;
    for (pi = 0; pi < shr_smd->n_prims; pi++) {
        SMD_PRI_TYPE *pt      = (SMD_PRI_TYPE *)p;
        uint8_t       stride  = pt->len;
        int           is_quad = (pt->type >= 2);
        uint16_t     *vi      = (uint16_t *)(p + 4);
        DVECTOR sv[4];
        int32_t sz[4], otz;

        gte_ldv3(&shr_smd->p_verts[vi[0]], &shr_smd->p_verts[vi[1]], &shr_smd->p_verts[vi[2]]);
        gte_rtpt();
        gte_stsxy3c(sv);
        /* Back faces off: at true depth (not the held weapon's remapped band)
           a few OT buckets cover the whole mace, and an unculled back face
           would draw over the front. Sharur.smx marks none of its faces
           nocull. */
        if (!pt->nocull) {
            int32_t nclip;
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }
        gte_stsz4c(sz);
        if (sz[1] <= 0 || sz[2] <= 0 || sz[3] <= 0) { p += stride; continue; }
        if (is_quad) {
            gte_ldv0(&shr_smd->p_verts[vi[3]]);
            gte_rtps();
            gte_stsxy(&sv[3]);
            gte_avsz4();
        } else {
            gte_avsz3();
        }
        gte_stotz(&otz);
        otz += 8;   /* just in front of the room (+40) it stands in */
        if (otz < SCENE_OT_MIN)  otz = SCENE_OT_MIN;
        if (otz > OT_LENGTH - 2) otz = OT_LENGTH - 2;

        {
            /* weapon_render_model's shade: 40% ambient + key + fill. */
            uint16_t n0  = *(uint16_t *)(p + 12);
            SVECTOR *nm  = &shr_smd->p_norms[n0];
            int32_t dot  = ((int32_t)nm->vx * lx + (int32_t)nm->vy * ly +
                            (int32_t)nm->vz * lz) >> 12;
            int32_t dot2 = -dot, shade, br, bg, bb;
            uint8_t r, g, b;
            if (dot  < 0) dot  = 0;
            if (dot2 < 0) dot2 = 0;
            shade = 1638 + ((dot * 2458) >> 12) + ((dot2 * 820) >> 12);
            if (shade > 4096) shade = 4096;
            shade = (shade * fog) >> 8;
            br = ((int32_t)p[16] * gain) >> 12; if (br > 255) br = 255;
            bg = ((int32_t)p[17] * gain) >> 12; if (bg > 255) bg = 255;
            bb = ((int32_t)p[18] * gain) >> 12; if (bb > 255) bb = 255;
            r = (uint8_t)((br * shade) >> 12);
            g = (uint8_t)((bg * shade) >> 12);
            b = (uint8_t)((bb * shade) >> 12);

            if (is_quad) {
                POLY_F4 *q;
                if (ctx->next_packet + sizeof(POLY_F4) > buf_end) break;
                q = (POLY_F4 *)ctx->next_packet;
                setPolyF4(q);
                setRGB0(q, r, g, b);
                q->x0 = sv[0].vx; q->y0 = sv[0].vy;
                q->x1 = sv[1].vx; q->y1 = sv[1].vy;
                q->x2 = sv[2].vx; q->y2 = sv[2].vy;
                q->x3 = sv[3].vx; q->y3 = sv[3].vy;
                addPrim(&ot[otz], q);
                ctx->next_packet += sizeof(POLY_F4);
            } else {
                POLY_F3 *q;
                if (ctx->next_packet + sizeof(POLY_F3) > buf_end) break;
                q = (POLY_F3 *)ctx->next_packet;
                setPolyF3(q);
                setRGB0(q, r, g, b);
                q->x0 = sv[0].vx; q->y0 = sv[0].vy;
                q->x1 = sv[1].vx; q->y1 = sv[1].vy;
                q->x2 = sv[2].vx; q->y2 = sv[2].vy;
                addPrim(&ot[otz], q);
                ctx->next_packet += sizeof(POLY_F3);
            }
        }
        p += stride;
    }
    sharur_camera_view();
}
