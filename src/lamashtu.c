#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "collision.h"     /* GROUND_FLOOR_Y */
#include "cdaudio.h"       /* the bracket around the entry-time reads */
#include "texmgr.h"
#include "crucifaxe.h"     /* SWING_RANGE, for lamashtu_try_hit */
#include "title.h"         /* STATE_LAMASHTU_ARENA, current_area */
#include "player.h"        /* player_hurt, game_over - the shockwave */
#include "sound.h"
#include "lms_creep.h"     /* what she pours out of herself */
#include "lamashtu.h"

/* LAMASHTU — the body. See lamashtu.h for the whole argument; this file is the
   mechanism. The model half is src/asag.c's, the attacks are Gula's wave and
   shove (src/gula.c) and the crib's Creeps (src/crib.c). */

#define LMS_MESH_FILE  "\\TEXCTCMB\\LAMASHTU.SMD;1"

static const char *const clip_file[LMS_CLIP_COUNT] = {
    /* IDLE   */ "\\TEXCTCMB\\LMSIDLE.PVA;1",
    /* SHRIEK */ "\\TEXCTCMB\\LMSSHRK.PVA;1",
};

#define PVA_HEADER_SIZE  12

/* ---- The skin: texmgr, the crib's terms. -1 until registered, and the draw
   falls back to flat polys rather than sampling tpage 0. ------------------- */
static int lms_tex = -1;

/* ---- Loaded state - ALL OF IT FREED BY lamashtus_free_model() ------------- */
static void    *mesh_buff;
static SMD     *mesh_smd;
static void    *clip_buff  [LMS_CLIP_COUNT];
static int16_t *clip_data  [LMS_CLIP_COUNT];   /* first vertex of frame 0     */
static int8_t   clip_stride[LMS_CLIP_COUNT];   /* int16s per vertex: 4 or 3   */
static int      clip_count [LMS_CLIP_COUNT];   /* 0 = rejected or absent      */
static SVECTOR *pose_buf;                      /* this frame, in WORLD space  */
static int      model_loaded = 0;

/* ---- THE ANUS: which primitive, found ONCE off the bind pose ----------------
   Of the quads whose four corners are all on her back (z > 250) and low
   (y > -300), the NARROWEST: 56 across where every other quad on that strip is
   200. Prim 30 today; the test survives a re-export renumbering the prims. -1
   means no glow and no target. */
#define LMS_ANUS_BACK_Z   250
#define LMS_ANUS_LOW_Y  (-300)
static int anus_prim = -1;

/* ---- Where she stands ------------------------------------------------------ */
static int32_t lms_x = 2700, lms_z = 1500;
static int32_t lms_fx = -4096, lms_fz = 0;        /* facing, 4096ths        */

/* ---- Playback ------------------------------------------------------------- */
static int8_t  cur_clip = LMS_CLIP_IDLE;
static int16_t anim_frame;
static int16_t anim_tick;
static int8_t  frozen;

/* ---- What the director writes ---------------------------------------------- */
static int8_t  visible;
static int16_t fade = 256;
static int8_t  glow_on;
static int8_t  vulnerable;
static int8_t  aggressive;
static int8_t  burning;
static int16_t shake;

/* ---- The fight's state ------------------------------------------------------ */
static int16_t lms_health = LMS_MAX_HEALTH;
static int16_t lms_flash;
static int8_t  hit_latch;
static int16_t cry_timer;
static int16_t spawn_timer;

/* The wave (0 = none, else frames since it went out) and the shove. */
static int16_t wave_t;
static int8_t  wave_hit;
static int32_t wave_x, wave_z;          /* where it went out from: fixed       */
static int16_t shove_t;
static int32_t shove_dx, shove_dz, shove_x, shove_z;

/* ---- THE GLOW, Asag's crown pad exactly (src/asag.c, ASAG_GLOW_*): the skin
   of the spot modulated toward this blue, breathing between ~65% and 100%. ---- */
#define LMS_GLOW_TINT_R    28
#define LMS_GLOW_TINT_G    44
#define LMS_GLOW_TINT_B   255
#define LMS_GLOW_PULSE     40
static int16_t glow_ph;

/* The weapons' target has a floor on its size, as Asag's head box does: the
   spot is 56 across, two pixels at the length of the hall. */
#define LMS_ANUS_MIN_HALF  40

/* The collider's vertical slack, Asag's. */
#define LMS_SOLID_Y_SLACK  60

static int32_t lms_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x, last, s = 1, t = v;
    while (t > 0) { t >>= 2; s <<= 1; }
    x = s;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

/* ---- read_file, src/asag.c's: exact-size buffer, tail sector through one
   shared scratch, and the $sp guard. Read the notes there. */
#define LMS_STACK_MARGIN 2048
static uint8_t *tail_scratch;

static void *read_file(const char *name) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)name)) return NULL;
    int size  = (int)file.size;
    if (size <= 0) return NULL;
    int whole = size / 2048;
    int tail  = size - whole * 2048;
    int alloc = (size + 3) & ~3;

    if (tail > 0 && !tail_scratch) return NULL;

    void *buf = malloc(alloc);
    if (!buf) return NULL;

    uint32_t sp;
    __asm__ volatile("move %0, $sp" : "=r"(sp));
    if ((uint32_t)buf + (uint32_t)alloc + LMS_STACK_MARGIN > sp) {
        free(buf);
        return NULL;
    }

    if (whole > 0) {
        CdControl(CdlSetloc, &file.pos, NULL);
        CdRead(whole, (uint32_t *)buf, CdlModeSpeed);
        CdReadSync(0, NULL);
    }
    if (tail > 0) {
        CdlLOC pos;
        CdIntToPos(CdPosToInt(&file.pos) + whole, &pos);
        CdControl(CdlSetloc, &pos, NULL);
        CdRead(1, (uint32_t *)tail_scratch, CdlModeSpeed);
        CdReadSync(0, NULL);
        memcpy((uint8_t *)buf + whole * 2048, tail_scratch, tail);
    }
    return buf;
}

static void load_clip(int c) {
    if (!mesh_smd) return;
    uint8_t *p = (uint8_t *)read_file(clip_file[c]);
    if (!p) return;
    int stride;
    if (p[0] == 'P' && p[1] == 'V' && p[2] == 'A' && p[3] == '1')      stride = 4;
    else if (p[0] == 'P' && p[1] == 'V' && p[2] == 'A' && p[3] == '2') stride = 3;
    else { free(p); return; }
    int n_verts  = p[4] | (p[5] << 8);
    int n_frames = p[6] | (p[7] << 8);
    if (n_verts != mesh_smd->n_verts || n_frames <= 0) { free(p); return; }
    clip_buff[c]   = p;
    clip_data[c]   = (int16_t *)(p + PVA_HEADER_SIZE);
    clip_stride[c] = (int8_t)stride;
    clip_count[c]  = n_frames;
}

static void find_anus(void) {
    anus_prim = -1;
    if (!mesh_smd) return;
    uint8_t *p = (uint8_t *)mesh_smd->p_prims;
    int32_t best = 0x7FFFFFFF;
    for (int k = 0; k < mesh_smd->n_prims; k++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        if (pt->type >= 2) {
            uint16_t *vi = (uint16_t *)(p + 4);
            int32_t mnx = 32767, mxx = -32768, ok = 1;
            for (int c = 0; c < 4; c++) {
                SVECTOR *v = &mesh_smd->p_verts[vi[c]];
                if (v->vz <= LMS_ANUS_BACK_Z || v->vy <= LMS_ANUS_LOW_Y) { ok = 0; break; }
                if (v->vx < mnx) mnx = v->vx;
                if (v->vx > mxx) mxx = v->vx;
            }
            if (ok && mxx - mnx < best) { best = mxx - mnx; anus_prim = k; }
        }
        p += pt->len;
    }
}

static void load_mesh(void) {
    mesh_buff = read_file(LMS_MESH_FILE);
    mesh_smd  = mesh_buff ? smdInitData(mesh_buff) : NULL;
    if (!mesh_smd) return;
    pose_buf = (SVECTOR *)malloc(mesh_smd->n_verts * sizeof(SVECTOR));
    find_anus();
}

void lamashtus_load_model(void) {
    if (model_loaded) return;
    cdaudio_suspend();
    tail_scratch = (uint8_t *)malloc(2048);
    load_mesh();
    for (int c = 0; c < LMS_CLIP_COUNT; c++) load_clip(c);
    if (tail_scratch) { free(tail_scratch); tail_scratch = NULL; }
    cdaudio_resume();
    model_loaded = 1;
}

void lamashtus_free_model(void) {
    if (mesh_buff) { free(mesh_buff); mesh_buff = NULL; }
    mesh_smd = NULL;
    if (pose_buf) { free(pose_buf); pose_buf = NULL; }
    for (int c = 0; c < LMS_CLIP_COUNT; c++) {
        if (clip_buff[c]) { free(clip_buff[c]); clip_buff[c] = NULL; }
        clip_data[c]   = NULL;
        clip_stride[c] = 0;
        clip_count[c]  = 0;
    }
    anus_prim    = -1;
    model_loaded = 0;
}

void lamashtu_load_assets(void) {
    texmgr_set_bank(TEXBANK_CATACOMBS);
    lms_tex = texmgr_register("\\TEXCTCMB\\LAMASHTU.TIM;1");
}

void lamashtu_upload_texture(void) {
    if (lms_tex >= 0) texmgr_upload(lms_tex);
}

static void stop_cry(void) {
    sound_stop(SFX_LMS_CRY);
    cry_timer = 0;
}

void lamashtu_cancel_wave(void) {
    wave_t  = 0;
    shove_t = 0;
}

void lamashtu_reset(void) {
    cur_clip   = LMS_CLIP_IDLE;
    anim_frame = 0;
    anim_tick  = 0;
    frozen     = 0;
    visible    = 0;
    fade       = 256;
    glow_on    = 0;
    vulnerable = 0;
    aggressive = 0;
    burning    = 0;
    shake      = 0;
    lms_health = LMS_MAX_HEALTH;
    lms_flash  = 0;
    hit_latch  = 0;
    spawn_timer = 0;
    glow_ph    = 0;
    lamashtu_cancel_wave();
    stop_cry();
    sound_stop(SFX_LMS_SCREAM);
}

int lamashtu_health(void)       { return lms_health; }
int lamashtu_model_loaded(void) { return model_loaded && mesh_smd; }

static int lms_here(void) {
    return current_area == STATE_LAMASHTU_ARENA && model_loaded && mesh_smd;
}

/* ---- The director's switches ----------------------------------------------- */

void lamashtu_place(int32_t x, int32_t z, LamashtuFacing facing) {
    lms_x = x; lms_z = z;
    switch (facing) {
    case LMS_FACE_WEST:  lms_fx = -4096; lms_fz =     0; break;
    case LMS_FACE_EAST:  lms_fx =  4096; lms_fz =     0; break;
    case LMS_FACE_NORTH: lms_fx =     0; lms_fz =  4096; break;
    default:             lms_fx =     0; lms_fz = -4096; break;
    }
}

void lamashtu_set_visible(int on) { visible = on ? 1 : 0; }

void lamashtu_set_fade(int32_t f) {
    if (f < 0) f = 0;
    if (f > 256) f = 256;
    fade = (int16_t)f;
}
int32_t lamashtu_fade(void) { return fade; }

void lamashtu_set_glow(int on)       { glow_on = on ? 1 : 0; }
void lamashtu_set_vulnerable(int on) { vulnerable = on ? 1 : 0; }
void lamashtu_set_frozen(int on)     { frozen = on ? 1 : 0; }
void lamashtu_set_burning(int on)    { burning = on ? 1 : 0; }
void lamashtu_set_shake(int32_t u) {
    if (u < 0) u = 0;
    if (u > 64) u = 64;
    shake = (int16_t)u;
}

/* On: the cry starts NOW (it is the sound of her being back), the first Creep
   comes LMS_SPAWN_FRAMES later. Off: the cry stops; a wave already out finishes
   its sweep (the Lumberer's rule - launching it was the commitment). */
void lamashtu_set_aggressive(int on) {
    if (on && !aggressive) {
        cry_timer   = 1;
        spawn_timer = LMS_SPAWN_FRAMES;
    }
    if (!on && aggressive) stop_cry();
    aggressive = on ? 1 : 0;
}

void lamashtu_play_shriek(void) {
    if (clip_count[LMS_CLIP_SHRIEK] <= 0) return;   /* absent: stay on Idle */
    cur_clip   = LMS_CLIP_SHRIEK;
    anim_frame = 0;
    anim_tick  = 0;
}

int32_t lamashtu_shriek_ticks(void) {
    return (int32_t)clip_count[LMS_CLIP_SHRIEK] * LMS_ANIM_TICKS;
}

int lamashtu_hit_taken(void) {
    int h = hit_latch;
    hit_latch = 0;
    return h;
}

void lamashtu_facing_vec(int32_t *fx, int32_t *fz) { *fx = lms_fx; *fz = lms_fz; }

void lamashtu_local_to_world(int32_t lx, int32_t ly, int32_t lz, VECTOR *out) {
    out->vx = lms_x - ((lx * lms_fz + lz * lms_fx) >> 12);
    out->vy = LMS_FLOOR_Y + ly;
    out->vz = lms_z + ((lx * lms_fx - lz * lms_fz) >> 12);
}

/* ---- The pose ------------------------------------------------------------------
   This frame's clip frame, unpacked AND turned AND placed (lamashtu.h's
   transform) into pose_buf - the loop has to exist to expand PVA2's triples
   anyway. Falls back to the .smd's bind pose when the clip is absent. */
static SVECTOR *body_verts(void) {
    int nv = mesh_smd->n_verts;
    int c  = cur_clip;
    int16_t *src;
    int      stride;

    if (clip_data[c] && clip_count[c] > 0) {
        int f = anim_frame;
        if (f < 0 || f >= clip_count[c]) f = 0;
        stride = clip_stride[c];
        src    = clip_data[c] + (int32_t)f * nv * stride;
    } else {
        src    = (int16_t *)mesh_smd->p_verts;
        stride = 4;
    }
    if (!pose_buf) return NULL;

    /* The facing is a quarter turn, so every product below is 0 or +-lx/lz:
       exact, and the >> 12 is a no-op in effect. */
    SVECTOR *d = pose_buf;
    for (int v = 0; v < nv; v++) {
        int32_t lx = *src++;
        int32_t ly = *src++;
        int32_t lz = *src++;
        if (stride == 4) src++;
        d[v].vx  = (int16_t)(lms_x - ((lx * lms_fz + lz * lms_fx) >> 12));
        d[v].vy  = (int16_t)(LMS_FLOOR_Y + ly);
        d[v].vz  = (int16_t)(lms_z + ((lx * lms_fx - lz * lms_fz) >> 12));
        d[v].pad = 0;
    }
    return pose_buf;
}

/* The box of her vertices at the player's height - the collider's, shared with
   the shockwave's trigger so the two cannot disagree about where she is. */
static int body_box(int32_t py, int32_t *mnx, int32_t *mxx,
                    int32_t *mnz, int32_t *mxz) {
    SVECTOR *vp = body_verts();
    if (!vp) return 0;
    int32_t band_top = (py - 30) - LMS_SOLID_Y_SLACK;
    int32_t band_bot = (py + GROUND_FLOOR_Y) + LMS_SOLID_Y_SLACK;
    int32_t a = 32767, b = -32768, c = 32767, d = -32768;
    int found = 0;
    for (int v = 0; v < mesh_smd->n_verts; v++) {
        int32_t y = vp[v].vy;
        if (y < band_top || y > band_bot) continue;
        int32_t x = vp[v].vx, z = vp[v].vz;
        if (x < a) a = x;
        if (x > b) b = x;
        if (z < c) c = z;
        if (z > d) d = z;
        found = 1;
    }
    if (!found) return 0;
    *mnx = a; *mxx = b; *mnz = c; *mxz = d;
    return 1;
}

/* ---- The shockwave ------------------------------------------------------------- */
static int32_t wave_reach(void) {
    return LMS_WAVE_START +
           ((LMS_WAVE_RADIUS - LMS_WAVE_START) * wave_t) / LMS_WAVE_FRAMES;
}

/* EVERYTHING HERE TARGETS player_x/z(), never cam_* (camera.h). */
static void wave_tick(void) {
    if (wave_t <= 0) return;
    if (++wave_t > LMS_WAVE_FRAMES) { wave_t = 0; return; }
    if (wave_hit || game_over) return;

    int32_t px = player_x(), pz = player_z();
    int32_t dx = px - wave_x, dz = pz - wave_z;
    int32_t d  = lms_isqrt(dx * dx + dz * dz);
    int32_t reach = wave_reach();
    if (d > reach || d < reach - LMS_WAVE_HIT_BAND) return;

    wave_hit = 1;
    player_hurt(LMS_WAVE_DAMAGE);
    sound_play(SFX_HURT);
    if (d > 0) {
        shove_dx = (dx * LMS_SHOVE_STEP) / d;
        shove_dz = (dz * LMS_SHOVE_STEP) / d;
    } else {
        shove_dx = LMS_SHOVE_STEP;
        shove_dz = 0;
    }
    shove_t = LMS_SHOVE_FRAMES;
    shove_x = cam_x;
    shove_z = cam_z;
    if (player_health <= 0) {
        player_health = 0;
        game_over     = 1;
        flash_timer   = 90;
        sound_play(SFX_DIE);
    }
}

/* Gula's shove: runs AFTER apply_collision_reception has resolved the last
   step, so a step a wall ate shows up as the player standing short of it - and
   that ends the shove. */
static void shove_tick(void) {
    if (shove_t <= 0) return;
    if (game_over) { shove_t = 0; return; }
    if (shove_t < LMS_SHOVE_FRAMES) {
        int32_t lx = shove_x - cam_x, lz = shove_z - cam_z;
        if ((lx < 0 ? -lx : lx) + (lz < 0 ? -lz : lz) > 30) { shove_t = 0; return; }
    }
    cam_x  += shove_dx;
    cam_z  += shove_dz;
    shove_x = cam_x;
    shove_z = cam_z;
    shove_t--;
}

/* ---- Update ---------------------------------------------------------------------
   The clip: Idle loops; a Shriek plays once and hands back to Idle. */
static void clip_tick(void) {
    int n = clip_count[cur_clip];
    if (n <= 0) { cur_clip = LMS_CLIP_IDLE; return; }
    if (++anim_tick < LMS_ANIM_TICKS) return;
    anim_tick = 0;
    if (++anim_frame >= n) {
        anim_frame = 0;
        cur_clip   = LMS_CLIP_IDLE;
    }
}

static void attacks_tick(void) {
    /* THE CRY, re-keyed from C. */
    if (cry_timer > 0 && --cry_timer == 0) {
        sound_play(SFX_LMS_CRY);
        cry_timer = LMS_CRY_FRAMES;
    }

    /* THE CREEPS, out of the middle of her. The anchor is the floor's standing
       anchor (creep.h); the body floats down out of her chest to its own
       height. */
    if (--spawn_timer <= 0) {
        spawn_timer = LMS_SPAWN_FRAMES;
        if (lms_creeps_alive_in(STATE_LAMASHTU_ARENA) < LMS_MAX_CREEPS)
            lms_creep_spawn(lms_x, lms_z, LMS_FLOOR_Y - GROUND_FLOOR_Y,
                            LMS_FLOOR_Y + LMS_SPAWN_Y, STATE_LAMASHTU_ARENA);
    }

    /* THE SHOCKWAVE: one at a time, and not again until the shriek it came
       with has played out - and NEVER at a player behind her (lamashtu.h). */
    if (wave_t == 0 && cur_clip == LMS_CLIP_IDLE && !game_over) {
        int32_t mnx, mxx, mnz, mxz;
        int32_t px = player_x(), pz = player_z();
        int32_t along = ((px - lms_x) * lms_fx + (pz - lms_z) * lms_fz) >> 12;
        if (along >= -LMS_BACK_DEPTH &&
            body_box(player_y(), &mnx, &mxx, &mnz, &mxz)) {
            int32_t gx = px < mnx ? mnx - px : (px > mxx ? px - mxx : 0);
            int32_t gz = pz < mnz ? mnz - pz : (pz > mxz ? pz - mxz : 0);
            if (gx < LMS_WAVE_TRIGGER && gz < LMS_WAVE_TRIGGER &&
                gx * gx + gz * gz < LMS_WAVE_TRIGGER * LMS_WAVE_TRIGGER) {
                wave_t   = 1;
                wave_hit = 0;
                wave_x   = lms_x;
                wave_z   = lms_z;
                lamashtu_play_shriek();
                sound_play(SFX_LMS_SCREAM);
            }
        }
    }
}

void lamashtu_update(void) {
    if (!lms_here()) return;
    if (lms_flash > 0) lms_flash--;

    wave_tick();
    shove_tick();

    if (!visible) return;
    if (!frozen) clip_tick();
    if (aggressive) attacks_tick();
}

/* ---- Drawing --------------------------------------------------------------------
   src/asag.c's draw less the position track, plus: no fog at any range (Gula's
   rule), the anus lamp, and an ADDITIVE TEXTURED fade - the palette carries
   STP (lamashtu.h), so the poly keeps its skin and goes semi-transparent with
   ABR=1 and its colour scaled toward black. */
static void lms_wave_quad(RenderContext *ctx, SVECTOR v[4],
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
    otz += 40;   /* the room mesh's own bias: a pillar in front still hides it */
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

/* The red ring: Gula's band on the floor plus a skirt on its leading edge,
   unfogged like her, segments well behind the camera skipped. */
static void draw_wave(RenderContext *ctx) {
    int32_t reach = wave_reach();
    int32_t inner = reach - LMS_WAVE_BAND;
    if (inner < 0) inner = 0;
    int32_t fy = LMS_FLOOR_Y - 4;
    int32_t fx = isin(cam_rot), fz = icos(cam_rot);

    /* Bright as it leaves her, dying away as it reaches its limit. */
    int32_t lvl = 256 - (wave_t * 160) / LMS_WAVE_FRAMES;
    uint8_t gr = (uint8_t)((150 * lvl) >> 8), gg = (uint8_t)((10 * lvl) >> 8),
            gb = (uint8_t)((10 * lvl) >> 8);
    uint8_t wr = (uint8_t)((230 * lvl) >> 8), wg = (uint8_t)((30 * lvl) >> 8),
            wb = (uint8_t)((20 * lvl) >> 8);

    SVECTOR v[4];
    int k, seg;
    for (k = 0; k < 4; k++) v[k].pad = 0;
    for (seg = 0; seg < LMS_WAVE_SEGS; seg++) {
        int32_t a0 = (seg * 4096) / LMS_WAVE_SEGS;
        int32_t a1 = ((seg + 1) * 4096) / LMS_WAVE_SEGS;
        int32_t am = (a0 + a1) >> 1;
        int32_t mx = wave_x + ((isin(am) * reach) >> 12);
        int32_t mz = wave_z + ((icos(am) * reach) >> 12);
        int32_t cdx = mx - cam_x, cdz = mz - cam_z;
        if (((cdx * fx + cdz * fz) >> 12) < -400) continue;   /* behind */

        int32_t s0 = isin(a0), c0 = icos(a0), s1 = isin(a1), c1 = icos(a1);
        int32_t ix0 = wave_x + (s0 * inner >> 12), iz0 = wave_z + (c0 * inner >> 12);
        int32_t ix1 = wave_x + (s1 * inner >> 12), iz1 = wave_z + (c1 * inner >> 12);
        int32_t ox0 = wave_x + (s0 * reach >> 12), oz0 = wave_z + (c0 * reach >> 12);
        int32_t ox1 = wave_x + (s1 * reach >> 12), oz1 = wave_z + (c1 * reach >> 12);

        v[0].vx = (int16_t)ix0; v[0].vy = (int16_t)fy; v[0].vz = (int16_t)iz0;
        v[1].vx = (int16_t)ox0; v[1].vy = (int16_t)fy; v[1].vz = (int16_t)oz0;
        v[2].vx = (int16_t)ix1; v[2].vy = (int16_t)fy; v[2].vz = (int16_t)iz1;
        v[3].vx = (int16_t)ox1; v[3].vy = (int16_t)fy; v[3].vz = (int16_t)oz1;
        lms_wave_quad(ctx, v, gr, gg, gb);

        v[0].vx = (int16_t)ox0; v[0].vy = (int16_t)(fy - LMS_WAVE_WALL_H); v[0].vz = (int16_t)oz0;
        v[1].vx = (int16_t)ox1; v[1].vy = (int16_t)(fy - LMS_WAVE_WALL_H); v[1].vz = (int16_t)oz1;
        v[2].vx = (int16_t)ox0; v[2].vy = (int16_t)fy;                     v[2].vz = (int16_t)oz0;
        v[3].vx = (int16_t)ox1; v[3].vy = (int16_t)fy;                     v[3].vz = (int16_t)oz1;
        lms_wave_quad(ctx, v, wr, wg, wb);
    }
}

void lamashtu_draw(RenderContext *ctx) {
    if (!lms_here()) return;
    if (wave_t > 0) draw_wave(ctx);
    if (!visible || fade <= 0) return;      /* fade 0 is a SKIP (TRICK 1) */
    SMD *smd = mesh_smd;

    {
        int32_t dx = lms_x - cam_x, dz = lms_z - cam_z;
        if (dx * isin(cam_rot) + dz * icos(cam_rot) < -(1300 << 12)) return;
    }

    SVECTOR *vp = body_verts();
    if (!vp) return;
    /* The death's jitter: ONE offset for the whole body, into the copy the draw
       is about to read - pose_buf is rebuilt by every caller, so the collider
       and the target never see it. */
    if (shake) {
        int32_t a = shake * 2 + 1;
        int32_t sx = (rand() % a) - shake, sy = (rand() % a) - shake,
                sz = (rand() % a) - shake;
        for (int v = 0; v < smd->n_verts; v++) {
            vp[v].vx = (int16_t)(vp[v].vx + sx);
            vp[v].vy = (int16_t)(vp[v].vy + sy);
            vp[v].vz = (int16_t)(vp[v].vz + sz);
        }
    }

    glow_ph = (int16_t)((glow_ph + 1) % LMS_GLOW_PULSE);
    int32_t glow_amt = 0;
    if (glow_on) {
        int32_t s = isin(((int32_t)glow_ph * 4096) / LMS_GLOW_PULSE);
        glow_amt  = 212 + (s * 44 >> 12);
        if (glow_amt > 256) glow_amt = 256;
    }

    int      fading   = fade < 256;
    int      textured = lms_tex >= 0;
    uint16_t tpage    = textured ? texmgr_tpage(lms_tex) : 0;
    uint16_t clut     = textured ? texmgr_clut(lms_tex)  : 0;
    /* ABR lives in bits 5-6 of a tpage; 1 is additive. */
    if (fading) tpage = (uint16_t)((tpage & ~0x60u) | 0x20u);

    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;
    uint8_t  *p       = (uint8_t *)smd->p_prims;
    int n = smd->n_prims;
    int any_flat_fade = 0;
    int32_t last_otz = 0;

    for (int k = 0; k < n; k++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);
        int stride  = pt->len;

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &vp[vi[0]];
        SVECTOR *v1 = &vp[vi[1]];
        SVECTOR *v2 = &vp[vi[2]];

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

        if (!pt->nocull) {
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }

        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        SVECTOR *v3    = 0;
        int32_t  v2_sz = sz[3];
        if (is_quad) {
            v3 = &vp[vi[3]];
            gte_ldv0(v3);
            gte_rtps();
            gte_stsxy(&sv[3]);
            gte_stsz(&sz[3]);
            if (sv[3].vx <= -1023 || sv[3].vx >= 1023 ||
                sv[3].vy <= -1023 || sv[3].vy >= 1023) { p += stride; continue; }
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
        otz += 38;   /* two nearer than the room's +40: Asag's tie-break */
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        /* NO FOG: the baked colour straight through. */
        uint8_t *col = p + 16;
        int32_t r = col[0], g = col[1], b = col[2];

        if (k == anus_prim && glow_amt > 0) {
            r = r + (((int32_t)LMS_GLOW_TINT_R - r) * glow_amt >> 8);
            g = g + (((int32_t)LMS_GLOW_TINT_G - g) * glow_amt >> 8);
            b = b + (((int32_t)LMS_GLOW_TINT_B - b) * glow_amt >> 8);
        }
        /* The damage flash, and the death's held red: after the glow, so a hit
           reads as red - Asag's order. */
        if (lms_flash > 0 || burning) { r = 255; g >>= 2; b >>= 2; }
        if (fading) { r = (r * fade) >> 8; g = (g * fade) >> 8; b = (b * fade) >> 8; }

        uint8_t *uv = p + 20;
        if (textured) {
            if (is_quad) {
                if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
                POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
                setPolyFT4(poly);
                setRGB0(poly, r, g, b);
                if (fading) setSemiTrans(poly, 1);
                poly->tpage = tpage;
                poly->clut  = clut;
                poly->u0 = uv[0]; poly->v0 = uv[1];
                poly->u1 = uv[2]; poly->v1 = uv[3];
                poly->u2 = uv[4]; poly->v2 = uv[5];
                poly->u3 = uv[6]; poly->v3 = uv[7];
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
                addPrim(&ot[otz], poly);
                ctx->next_packet += sizeof(POLY_FT4);
            } else {
                if (ctx->next_packet + sizeof(POLY_FT3) > buf_end) { p += stride; continue; }
                POLY_FT3 *poly = (POLY_FT3 *)ctx->next_packet;
                setPolyFT3(poly);
                setRGB0(poly, r, g, b);
                if (fading) setSemiTrans(poly, 1);
                poly->tpage = tpage;
                poly->clut  = clut;
                poly->u0 = uv[0]; poly->v0 = uv[1];
                poly->u1 = uv[2]; poly->v1 = uv[3];
                poly->u2 = uv[4]; poly->v2 = uv[5];
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                addPrim(&ot[otz], poly);
                ctx->next_packet += sizeof(POLY_FT3);
            }
        } else {
            /* No skin registered: flat, and a flat poly takes its blend mode
               from the CURRENT draw tpage - so one additive DR_TPAGE goes in
               after the last of them (see below). */
            if (fading) any_flat_fade = 1, last_otz = otz;
            if (is_quad) {
                if (ctx->next_packet + sizeof(POLY_F4) > buf_end) { p += stride; continue; }
                POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
                setPolyF4(poly);
                setRGB0(poly, r, g, b);
                if (fading) setSemiTrans(poly, 1);
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
                addPrim(&ot[otz], poly);
                ctx->next_packet += sizeof(POLY_F4);
            } else {
                if (ctx->next_packet + sizeof(POLY_F3) > buf_end) { p += stride; continue; }
                POLY_F3 *poly = (POLY_F3 *)ctx->next_packet;
                setPolyF3(poly);
                setRGB0(poly, r, g, b);
                if (fading) setSemiTrans(poly, 1);
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                addPrim(&ot[otz], poly);
                ctx->next_packet += sizeof(POLY_F3);
            }
        }
        p += stride;
    }

    /* The fallback path only: textured polys carry their own ABR. */
    if (any_flat_fade && ctx->next_packet + sizeof(DR_TPAGE) <= buf_end) {
        DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(tp, 0, 0, getTPage(0, 1, 320, 0));
        addPrim(&ot[last_otz], tp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
}

/* ---- Solidity: src/asag.c's collider on the placed pose. Solid only while she
   is in the room and more than half there - a body fading in does not shove
   the player out of where it is about to be until it reads as solid. */
void lamashtu_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    if (!lms_here() || !visible || fade < 128) return;

    int32_t min_x, max_x, min_z, max_z;
    if (!body_box(py, &min_x, &max_x, &min_z, &max_z)) return;

    min_x -= radius; max_x += radius;
    min_z -= radius; max_z += radius;
    if (*px <= min_x || *px >= max_x) return;
    if (*pz <= min_z || *pz >= max_z) return;

    int32_t push_w = *px - min_x;
    int32_t push_e = max_x - *px;
    int32_t push_n = *pz - min_z;
    int32_t push_s = max_z - *pz;

    int32_t best = push_w, dx = -push_w, dz = 0;
    if (push_e < best) { best = push_e; dx =  push_e; dz = 0; }
    if (push_n < best) { best = push_n; dx = 0; dz = -push_n; }
    if (push_s < best) {                dx = 0; dz =  push_s; }
    *px += dx;
    *pz += dz;
}

/* ---- The anus as a target: this frame's centroid of its four corners, floored
   in size; only while she is vulnerable, and only from BEHIND (the player on
   the far side of the spot from her facing). */
static int anus_point(int32_t *x, int32_t *y, int32_t *z,
                      int32_t *half_w, int32_t *half_h) {
    if (!lms_here() || anus_prim < 0) return 0;
    SVECTOR *vp = body_verts();
    if (!vp) return 0;

    uint8_t *p = (uint8_t *)mesh_smd->p_prims;
    for (int k = 0; k < anus_prim; k++) p += ((SMD_PRI_TYPE *)p)->len;
    uint16_t *vi = (uint16_t *)(p + 4);

    int32_t sx = 0, sy = 0, sz = 0;
    int32_t mny = 32767, mxy = -32768, mnh = 32767, mxh = -32768;
    for (int c = 0; c < 4; c++) {
        SVECTOR *v = &vp[vi[c]];
        /* Her width runs across the facing: x for N/S, z for E/W. */
        int32_t h = lms_fx ? v->vz : v->vx;
        sx += v->vx; sy += v->vy; sz += v->vz;
        if (v->vy < mny) mny = v->vy;
        if (v->vy > mxy) mxy = v->vy;
        if (h < mnh) mnh = h;
        if (h > mxh) mxh = h;
    }
    *x = sx / 4; *y = sy / 4; *z = sz / 4;
    *half_w = (mxh - mnh) / 2;
    *half_h = (mxy - mny) / 2;
    if (*half_w < LMS_ANUS_MIN_HALF) *half_w = LMS_ANUS_MIN_HALF;
    if (*half_h < LMS_ANUS_MIN_HALF) *half_h = LMS_ANUS_MIN_HALF;
    return 1;
}

int lamashtu_anus_target(int32_t *x, int32_t *cy, int32_t *z,
                         int32_t *half_w, int32_t *half_h) {
    if (!visible || !vulnerable || lms_health <= 0) return 0;
    int32_t ax, ay, az, hw, hh;
    if (!anus_point(&ax, &ay, &az, &hw, &hh)) return 0;
    /* Behind: (camera - spot) . (-facing) > 0. */
    if ((cam_x - ax) * lms_fx + (cam_z - az) * lms_fz >= 0) return 0;
    *x = ax; *cy = ay; *z = az; *half_w = hw; *half_h = hh;
    return 1;
}

/* THE HIT. Everything that is the BODY's business happens here and now - the
   lamp out, invulnerable, the attacks off (the cry with them), the scream and
   one shriek - and the director is told once through lamashtu_hit_taken(). */
void lamashtu_damage(int dmg) {
    if (dmg <= 0 || !vulnerable || lms_health <= 0) return;
    lms_health -= dmg;
    if (lms_health < 0) lms_health = 0;
    lms_flash  = LMS_FLASH_FRAMES;
    vulnerable = 0;
    glow_on    = 0;
    lamashtu_set_aggressive(0);
    /* HER CREEPS DIE WITH THE BLOW. The next ones come out of her at the next
       phase's spot, LMS_SPAWN_FRAMES after she has faded back in - the spawn
       point is always her own body, wherever it now stands. */
    lms_creeps_kill_all(STATE_LAMASHTU_ARENA);
    sound_play(SFX_LMS_SCREAM);
    lamashtu_play_shriek();
    hit_latch  = 1;
}

/* The crucifaxe: measured to the SPOT - horizontal distance plus the vertical
   gap to its span - only from behind (lamashtu_anus_target) and facing it. */
int lamashtu_try_hit(void) {
    int32_t ax, ay, az, hw, hh;
    if (!lamashtu_anus_target(&ax, &ay, &az, &hw, &hh)) return 0;
    int32_t dx = ax - cam_x;
    int32_t dz = az - cam_z;
    int32_t gh = lms_isqrt(dx * dx + dz * dz);
    int32_t dy = 0;
    if (cam_y < ay - hh) dy = (ay - hh) - cam_y;
    if (cam_y > ay + hh) dy = cam_y - (ay + hh);
    if (gh + dy >= SWING_RANGE) return 0;
    int32_t dot = ((int32_t)dx * isin(cam_rot) + (int32_t)dz * icos(cam_rot)) >> 12;
    if (dot <= 0) return 0;
    lamashtu_damage(1);
    return 1;
}
