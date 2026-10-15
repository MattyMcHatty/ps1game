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
#include "lamashtu.h"

/* LAMASHTU — the body. See lamashtu.h for the whole argument; this file is the
   mechanism. Most of it is src/asag.c with the position track, the fade and
   the shake taken out (nothing drives them yet) and a placement put in. */

#define LMS_MESH_FILE  "\\TEXCTCMB\\LAMASHTU.SMD;1"

static const char *const clip_file[LMS_CLIP_COUNT] = {
    /* IDLE   */ "\\TEXCTCMB\\LMSIDLE.PVA;1",
    /* SHRIEK */ "\\TEXCTCMB\\LMSSHRK.PVA;1",
};

#define PVA_HEADER_SIZE  12

/* ---- The skin: texmgr, the crib's terms (src/crib.c). -1 until registered,
   and the draw falls back to flat polys rather than sampling tpage 0. ------ */
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
   "On her back side there is a single square poly near the bottom." In the
   model's own frame that is the only quad whose four corners are all on the
   back (z > 250) and low (y > -300) and that is SMALL: 56 across, where every
   other quad on that strip of her back is 200 (the four that frame it,
   prims 127-130, and the two flanking panels). So the test is "of the low back
   quads, the narrowest", which survives a re-export renumbering the prims - it
   is prim 30 today - and is a statement about the geometry rather than an
   index. A re-model that gives her back a second small quad would want this
   looking at again; -1 (none found) just means no glow and no target. */
#define LMS_ANUS_BACK_Z   250
#define LMS_ANUS_LOW_Y  (-300)
static int anus_prim = -1;

/* ---- Playback and fight state - survives a free/load, like Asag's --------- */
static int8_t  cur_clip = LMS_CLIP_IDLE;
static int16_t anim_frame;
static int16_t anim_tick;
static int16_t lms_health = LMS_MAX_HEALTH;
static int16_t lms_flash;

/* ---- THE GLOW, Asag's crown pad exactly (src/asag.c, ASAG_GLOW_*) ----------
   The skin of the spot is MODULATED toward this blue and that is the whole of
   it: no additive halo (Asag's was taken off because it read as a bloom in
   front of him rather than a lamp set into him). Modulation is
   texel * colour / 128, so R and G are crushed hard or a warm texel comes out
   green rather than blue.

   THE DIFFERENCE IS THAT IT IS ALWAYS ON. Asag's ramps up and down with his
   exposure windows; this is held at full level and only the pulse moves -
   the level breathes between about 65% and 100% over LMS_GLOW_PULSE frames,
   which is what makes it read as a state rather than a texture. */
#define LMS_GLOW_TINT_R    28
#define LMS_GLOW_TINT_G    44
#define LMS_GLOW_TINT_B   255
#define LMS_GLOW_PULSE     40
static int16_t glow_ph;

/* The target the weapons are given has a floor on its size, as Asag's head box
   does: the spot is 56 across, which at the length of the hall is two pixels
   and would be correct and unhittable. */
#define LMS_ANUS_MIN_HALF  40

/* Below this the collider ignores a vertex: the player's body span plus the
   slack Asag's collider uses for the same reason (src/asag.c). */
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
   shared scratch, and the $sp guard that turns "the heap reached the stack"
   into a missing clip instead of a CdRead through a return address. Read the
   notes there; the margin is the measured one. */
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

/* PVA1 or PVA2, and the vertex-count check that turns a stale clip into a
   motionless boss instead of an exploded one (tools/ANIMATING_A_3D_MODEL.txt
   mistake 5). */
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
    /* Taken BEFORE the clips, as Asag's is, so a tight heap loses a clip and
       not this - without it every PVA2 clip would draw the bind pose. */
    pose_buf = (SVECTOR *)malloc(mesh_smd->n_verts * sizeof(SVECTOR));
    find_anus();
}

void lamashtus_load_model(void) {
    if (model_loaded) return;
    /* A mid-game read: bracketed, because a data read issued while CD-DA
       streams hangs the drive. Three reads under one bracket. */
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

/* STARTUP: the header only (texmgr reads the pixels when the arena's room set
   is selected). Chapter 3's bank - py tools/check_tex_banks.py derives it from
   the uploader graph (the arena's uploader calls lamashtu_upload_texture). */
void lamashtu_load_assets(void) {
    texmgr_set_bank(TEXBANK_CATACOMBS);
    lms_tex = texmgr_register("\\TEXCTCMB\\LAMASHTU.TIM;1");
}

void lamashtu_upload_texture(void) {
    if (lms_tex >= 0) texmgr_upload(lms_tex);
}

void lamashtu_reset(void) {
    cur_clip   = LMS_CLIP_IDLE;
    anim_frame = 0;
    anim_tick  = 0;
    lms_health = LMS_MAX_HEALTH;
    lms_flash  = 0;
    glow_ph    = 0;
}

int lamashtu_health(void) { return lms_health; }

static int lms_here(void) {
    return current_area == STATE_LAMASHTU_ARENA && model_loaded && mesh_smd;
}

/* IDLE, then SHRIEK, then IDLE: each plays once through and hands to the
   other. A clip that never loaded has no frames, so it is stepped over rather
   than waited on (asag.h's asag_clip_loaded note) - with neither loaded she
   stands on the bind pose. */
void lamashtu_update(void) {
    if (!lms_here()) return;
    if (lms_flash > 0) lms_flash--;

    int n = clip_count[cur_clip];
    if (n <= 0) {
        int other = cur_clip ^ 1;
        if (clip_count[other] > 0) { cur_clip = (int8_t)other; anim_frame = 0; anim_tick = 0; }
        return;
    }
    if (++anim_tick < LMS_ANIM_TICKS) return;
    anim_tick = 0;
    if (++anim_frame >= n) {
        anim_frame = 0;
        int other = cur_clip ^ 1;
        if (clip_count[other] > 0) cur_clip = (int8_t)other;
    }
}

/* This frame's pose, unpacked AND PLACED: the quarter turn and the translation
   from lamashtu.h, applied as the PVA2 triples are expanded into SVECTORs - the
   loop has to exist anyway, so the placement costs a swap and two adds a
   vertex and no model matrix. Falls back to the .smd's own bind pose (placed
   the same way) when the clip is absent. Rebuilt every call, not cached: 156
   vertices. */
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

    SVECTOR *d = pose_buf;
    for (int v = 0; v < nv; v++) {
        int32_t lx = *src++;
        int32_t ly = *src++;
        int32_t lz = *src++;
        if (stride == 4) src++;
        d[v].vx  = (int16_t)(LMS_X + lz);
        d[v].vy  = (int16_t)(LMS_FLOOR_Y + ly);
        d[v].vz  = (int16_t)(LMS_Z - lx);
        d[v].pad = 0;
    }
    return pose_buf;
}

/* ---- The draw ----------------------------------------------------------------
   src/asag.c's asag_draw() less the fade, the shake and - the point - THE FOG:
   no distance reject and no fog blend, so she is drawn at the .smd's neutral
   128 modulation at any range (lamashtu.h). The room has already loaded the
   plain view (camera_build_view) and the vertices are world space, so no matrix
   is loaded and none is restored.

   The one whole-body cull kept is "well behind the camera", the room's own
   test widened by her size, so turning your back on her costs 139 transforms
   less. Everything else is per prim, as the room's loop does it. */
void lamashtu_draw(RenderContext *ctx) {
    if (!lms_here()) return;
    SMD *smd = mesh_smd;

    {
        int32_t dx = LMS_X - cam_x, dz = LMS_Z - cam_z;
        if (dx * isin(cam_rot) + dz * icos(cam_rot) < -(1300 << 12)) return;
    }

    SVECTOR *vp = body_verts();
    if (!vp) return;

    glow_ph = (int16_t)((glow_ph + 1) % LMS_GLOW_PULSE);
    int32_t glow_amt;
    {
        /* Asag's breath at a held full level: 168..256. */
        int32_t s = isin(((int32_t)glow_ph * 4096) / LMS_GLOW_PULSE);
        glow_amt  = 212 + (s * 44 >> 12);
        if (glow_amt > 256) glow_amt = 256;
    }

    int      textured = lms_tex >= 0;
    uint16_t tpage    = textured ? texmgr_tpage(lms_tex) : 0;
    uint16_t clut     = textured ? texmgr_clut(lms_tex)  : 0;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot     = ctx->buffers[ctx->active_buffer].ot;
    uint8_t  *p      = (uint8_t *)smd->p_prims;
    int n = smd->n_prims;

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

        /* KEEP THE BACKFACE CULL: tools/check_model_winding.py reports this
           mesh OK, one shell. */
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
        /* +38, two nearer than the room's +40, Asag's tie-break: she stands on
           the floor and among the pillars, and at the same bias the two meshes
           would trade the frames where they meet. */
        otz += 38;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        /* NO FOG: the baked colour straight through. */
        uint8_t *col = p + 16;
        uint8_t r = col[0], g = col[1], b = col[2];

        if (k == anus_prim) {
            r = (uint8_t)(r + (((int32_t)LMS_GLOW_TINT_R - r) * glow_amt >> 8));
            g = (uint8_t)(g + (((int32_t)LMS_GLOW_TINT_G - g) * glow_amt >> 8));
            b = (uint8_t)(b + (((int32_t)LMS_GLOW_TINT_B - b) * glow_amt >> 8));
        }
        /* The damage flash, after the glow so a hit still reads as red - Asag's
           order and his reason. */
        if (lms_flash > 0) { r = 255; g = (uint8_t)(g >> 2); b = (uint8_t)(b >> 2); }

        uint8_t *uv = p + 20;
        if (is_quad && textured) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, r, g, b);
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
        } else if (is_quad) {
            if (ctx->next_packet + sizeof(POLY_F4) > buf_end) { p += stride; continue; }
            POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
            setPolyF4(poly);
            setRGB0(poly, r, g, b);
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
            addPrim(&ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F4);
        } else if (textured) {
            if (ctx->next_packet + sizeof(POLY_FT3) > buf_end) { p += stride; continue; }
            POLY_FT3 *poly = (POLY_FT3 *)ctx->next_packet;
            setPolyFT3(poly);
            setRGB0(poly, r, g, b);
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
        } else {
            if (ctx->next_packet + sizeof(POLY_F3) > buf_end) { p += stride; continue; }
            POLY_F3 *poly = (POLY_F3 *)ctx->next_packet;
            setPolyF3(poly);
            setRGB0(poly, r, g, b);
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            addPrim(&ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F3);
        }
        p += stride;
    }
}

/* ---- Solidity: src/asag.c's asag_collide, on this frame's placed pose ------
   An AABB over only the vertices at the player's height, pushed out along the
   shallowest of the four axes. At body height she is her 600 block plus
   whatever the clip swings down into that band, so the box is the block on
   most frames; it stands in the crossing of two aisles, so every way out of it
   is open floor. */
void lamashtu_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    if (!lms_here()) return;

    int32_t band_top = (py - 30) - LMS_SOLID_Y_SLACK;
    int32_t band_bot = (py + GROUND_FLOOR_Y) + LMS_SOLID_Y_SLACK;

    SVECTOR *vp = body_verts();
    if (!vp) return;
    int nv = mesh_smd->n_verts;

    int32_t min_x = 32767, max_x = -32768, min_z = 32767, max_z = -32768;
    int found = 0;
    for (int v = 0; v < nv; v++) {
        int32_t y = vp[v].vy;
        if (y < band_top || y > band_bot) continue;
        int32_t x = vp[v].vx, z = vp[v].vz;
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (z < min_z) min_z = z;
        if (z > max_z) max_z = z;
        found = 1;
    }
    if (!found) return;

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

/* ---- The anus as a target ---------------------------------------------------
   Where the spot is THIS FRAME (the centroid of its four placed corners, so a
   clip that moves her hips moves the target with them) and its size, floored.

   >>> ONLY FROM BEHIND. <<< Her back faces +X (lamashtu.h), so the player must
   be east of the spot. Without this a round aimed at her face would find the
   spot through her: weapon_aim_clear() tests walls and props, and she is
   neither. */
static int anus_point(int32_t *x, int32_t *y, int32_t *z,
                      int32_t *half_w, int32_t *half_h) {
    if (!lms_here() || anus_prim < 0) return 0;
    SVECTOR *vp = body_verts();
    if (!vp) return 0;

    uint8_t *p = (uint8_t *)mesh_smd->p_prims;
    for (int k = 0; k < anus_prim; k++) p += ((SMD_PRI_TYPE *)p)->len;
    uint16_t *vi = (uint16_t *)(p + 4);

    int32_t sx = 0, sy = 0, sz = 0;
    int32_t mny = 32767, mxy = -32768, mnz = 32767, mxz = -32768;
    for (int c = 0; c < 4; c++) {
        SVECTOR *v = &vp[vi[c]];
        sx += v->vx; sy += v->vy; sz += v->vz;
        if (v->vy < mny) mny = v->vy;
        if (v->vy > mxy) mxy = v->vy;
        if (v->vz < mnz) mnz = v->vz;
        if (v->vz > mxz) mxz = v->vz;
    }
    *x = sx / 4; *y = sy / 4; *z = sz / 4;
    *half_w = (mxz - mnz) / 2;
    *half_h = (mxy - mny) / 2;
    if (*half_w < LMS_ANUS_MIN_HALF) *half_w = LMS_ANUS_MIN_HALF;
    if (*half_h < LMS_ANUS_MIN_HALF) *half_h = LMS_ANUS_MIN_HALF;
    return 1;
}

int lamashtu_anus_target(int32_t *x, int32_t *cy, int32_t *z,
                         int32_t *half_w, int32_t *half_h) {
    if (lms_health <= 0) return 0;
    int32_t ax, ay, az, hw, hh;
    if (!anus_point(&ax, &ay, &az, &hw, &hh)) return 0;
    if (cam_x <= ax) return 0;                 /* not behind her */
    *x = ax; *cy = ay; *z = az; *half_w = hw; *half_h = hh;
    return 1;
}

void lamashtu_damage(int dmg) {
    if (dmg <= 0 || lms_health <= 0) return;
    lms_health -= dmg;
    if (lms_health < 0) lms_health = 0;
    lms_flash = LMS_FLASH_FRAMES;
}

/* The crucifaxe: gula_try_hit's surface reach, measured to the SPOT rather than
   to a body - horizontal distance to the spot plus the vertical gap to its
   span - and only from behind and facing it. The collider holds the player
   about 100 off her back and the spot is 100 off the floor, so a player
   standing behind her is ~200 from it against SWING_RANGE 350. */
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
