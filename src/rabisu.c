#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "collision.h"      /* GROUND_FLOOR_Y */
#include "crucifaxe.h"      /* SWING_RANGE */
#include "particles.h"
#include "sound.h"
#include "title.h"          /* game_state */
#include "rabisu.h"

/* Rabisu — the first boss, and the first 3D-model enemy. See rabisu.h for the
   tuning sheet and tools/ADDING_A_3D_ENEMY.txt for the procedure. */

Rabisu rabisus[MAX_RABISUS];
int    rabisu_count = 0;

static SMD  *rabisu_smd  = NULL;
static void *rabisu_buff = NULL;

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

/* Startup: load the model and its idle clip. CD access is only safe before the
   render loop begins (tools/TEXTURING_NOTES.txt) — the same rule the sprite
   enemies' LoadImage obeys, and it binds here too even though there is no VRAM
   upload: these are CD reads. There is no *_upload_textures() counterpart
   because the mesh is untextured and owns no VRAM at all. */
void rabisus_load_assets(void) {
    /* In TEX, not the disc root: the root directory records must all fit
       the first 2048-byte sector or the boot ROM cannot find SYSTEM.CNF
       and the console hangs at the logo. See the comment in disc.xml. */
    rabisu_buff = read_file("\\TEX\\RABISU.SMD;1");
    if (rabisu_buff) rabisu_smd = smdInitData(rabisu_buff);
    rabisus_load_anim();
}

/* The vertex block this boss is posed on for this frame. Falls back to the
   .smd's own bind pose whenever the animation is missing or was rejected. */
static SVECTOR *rbs_verts(const Rabisu *r) {
    if (!rabisu_anim_frames) return rabisu_smd->p_verts;
    int f = r->anim_frame;
    if (f < 0 || f >= rabisu_anim_count) f = 0;
    return rabisu_anim_frames + (f * rabisu_smd->n_verts);
}

void rabisus_init(void) {
    /* Placements are seeded on a room's first entry by the world system (see
       world_enter in world.c). The array starts empty. */
    rabisu_count = 0;
}

void rabisus_reset(void) {
    rabisu_count = 0;
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
    int32_t y = ground_y - RBS_HOVER;
    *r = (Rabisu){0};
    r->x = x; r->y = y; r->z = z;
    r->spawn_x = x; r->spawn_y = y; r->spawn_z = z;
    r->health = RBS_MAX_HEALTH;
    r->active = 1;
    r->area   = area;
    /* Facing +Z until the first update turns it toward the player. */
    r->face_s = 0;
    r->face_c = ONE;
    return i;
}

void rabisus_rest(void) {
    int i;
    for (i = 0; i < rabisu_count; i++) {
        Rabisu *r = &rabisus[i];
        if (!r->active || r->dead) continue;
        /* Rebuild exactly as rabisu_add left it. Preserve the area tag across
           the wipe — losing it would strand the boss in room 0. */
        int32_t sx = r->spawn_x, sy = r->spawn_y, sz = r->spawn_z;
        GameState area = r->area;
        *r = (Rabisu){0};
        r->x = sx; r->y = sy; r->z = sz;
        r->spawn_x = sx; r->spawn_y = sy; r->spawn_z = sz;
        r->health = RBS_MAX_HEALTH;
        r->active = 1;
        r->area   = area;
        r->face_s = 0;
        r->face_c = ONE;
    }
}

/* Flame Rounds do double damage; a standard round and a crucifaxe swing both do
   1, so 20 HP is 20 swings, 20 standard rounds or 10 flame rounds. Append
   another { DMG_*, percent } line to give it a second weakness (damage.h). */
static const Weakness rabisu_weakness[] = {
    { DMG_FLAME, 200 },
};

int32_t rabisu_scale_damage(int32_t base, DamageType type) {
    return damage_scale(base, type, rabisu_weakness,
                        WEAKNESS_COUNT(rabisu_weakness));
}

void rabisu_damage(Rabisu *r, int dmg) {
    if (!r->active || r->dead) return;
    r->health   -= dmg;
    r->hit_timer = RBS_BAR_TIMER_MAX;
    if (r->health <= 0) {
        r->health = 0;
        r->dead   = 1;
        /* Burst at mid-body, not at the anchor: the anchor is the underside, so
           a burst there would spray from beneath its feet. */
        spawn_blood_burst(r->x, r->y - RBS_HALF_H, r->z);
        sound_play(SFX_TNTCL_DIE);   /* no boss clip yet; borrows the tentacle's
                                        death cry as a deliberate stand-in */
    } else {
        sound_play(SFX_AXEHIT);
    }
}

void rabisu_body(const Rabisu *r, int32_t *cyc, int32_t *hh, int32_t *hw) {
    *cyc = r->y - RBS_HALF_H;   /* anchor is the underside; -Y is up */
    *hh  = RBS_HALF_H;
    *hw  = RBS_HALF_W;
}

void update_rabisus(void) {
    int i;
    for (i = 0; i < rabisu_count; i++) {
        Rabisu *r = &rabisus[i];
        if (!r->active || r->dead) continue;
        if (r->area != game_state) continue;

        if (r->hit_timer > 0) r->hit_timer--;

        /* Turn to face the player. The facing is kept as the normalised
           direction vector rather than an angle: that IS the sin/cos pair the
           draw's rotation matrix needs, so no atan2 (which this SDK does not
           provide) and no trig lookup are involved. */
        int32_t dx = cam_x - r->x;
        int32_t dz = cam_z - r->z;
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
           boss's own area because of the game_state gate above — which is what
           we want: an off-screen boss in another room should not be burning
           frames of animation. */
        if (rabisu_anim_count > 1 && ++r->anim_tick >= RBS_ANIM_TICKS) {
            r->anim_tick = 0;
            if (++r->anim_frame >= rabisu_anim_count) r->anim_frame = 0;
        }

        /* No movement, wake or attack yet — the abilities come later. */
    }
}

/* Horizontal distance from (px,pz) to the body cylinder's SURFACE, and the
   vertical distance from py to the body's span. Both are 0 when the point is
   inside. Shared by the melee reach test and the collision push-out so the two
   can never disagree about where the boss's edges are. */
static void rabisu_gap(const Rabisu *r, int32_t px, int32_t py, int32_t pz,
                       int32_t *out_h, int32_t *out_v) {
    int32_t dx = px - r->x;
    int32_t dz = pz - r->z;
    int32_t d  = isqrt32(dx * dx + dz * dz);
    *out_h = d > RBS_BODY_RADIUS ? d - RBS_BODY_RADIUS : 0;

    int32_t top = r->y - RBS_HEIGHT;   /* -Y is up */
    int32_t bot = r->y;
    if      (py < top) *out_v = top - py;
    else if (py > bot) *out_v = py - bot;
    else               *out_v = 0;
}

void rabisus_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < rabisu_count; i++) {
        Rabisu *r = &rabisus[i];
        if (!r->active || r->dead) continue;
        if (r->area != game_state) continue;

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

int rabisus_try_hit(void) {
    int i;
    for (i = 0; i < rabisu_count; i++) {
        Rabisu *r = &rabisus[i];
        if (!r->active || r->dead) continue;
        if (r->area != game_state) continue;

        /* Reach measured to the body's SURFACE. The other enemies test their
           centre because they are roughly axe-sized; this one is 3.4 m across
           and 4.6 m tall, so its centre sits ~500 from the player's eye at the
           closest the collision cylinder allows — past SWING_RANGE (350), and
           the axe would never connect. */
        int32_t gh, gv;
        rabisu_gap(r, cam_x, cam_y, cam_z, &gh, &gv);
        if (gh + gv >= SWING_RANGE) continue;

        /* Facing test, as every other melee hit does. */
        int32_t dx  = r->x - cam_x;
        int32_t dz  = r->z - cam_z;
        int32_t dot = ((int32_t)dx * isin(cam_rot) +
                       (int32_t)dz * icos(cam_rot)) >> 12;
        if (dot <= 0) continue;

        rabisu_damage(r, 1);   /* no knockback: it hovers, and nothing moves it */
        return 1;
    }
    return 0;
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

/* Walk one boss's SMD prim stream. The model is untextured — every prim is a
   flat quad, 20 bytes, with its baked RGB at offset 16 — so this is the plain
   POLY_F4 path only, no tex map, no tpage/clut and no texture window to
   bracket. The GTE matrix is already the composed model-view when this runs. */
static void draw_rbs_model(RenderContext *ctx, const Rabisu *r, int32_t dist) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint8_t *p = (uint8_t *)rabisu_smd->p_prims;

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
       silhouette can carry. */
    int hit = (r->hit_timer > 0);

    int i;
    for (i = 0; i < rabisu_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt      = (SMD_PRI_TYPE *)p;
        uint8_t       stride  = pt->len;
        int           is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &vp[vi[0]];
        SVECTOR *v1 = &vp[vi[1]];
        SVECTOR *v2 = &vp[vi[2]];

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

        uint8_t *col = p + 16;
        int32_t cr = col[0], cg = col[1], cb = col[2];
        if (hit) { cr = 255; cg = cg >> 2; cb = cb >> 2; }
        uint8_t rr = (uint8_t)((cr * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t gg = (uint8_t)((cg * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t bb = (uint8_t)((cb * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8);

        if (is_quad) {
            if (ctx->next_packet + sizeof(POLY_F4) > buf_end) { p += stride; continue; }
            POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
            setPolyF4(poly);
            setRGB0(poly, rr, gg, bb);
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F4);
        } else {
            if (ctx->next_packet + sizeof(POLY_F3) > buf_end) { p += stride; continue; }
            POLY_F3 *poly = (POLY_F3 *)ctx->next_packet;
            setPolyF3(poly);
            setRGB0(poly, rr, gg, bb);
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F3);
        }

        p += stride;
    }
}

void draw_rabisus(RenderContext *ctx) {
    if (!rabisu_smd || rabisu_count == 0) return;

    /* Rebuild the camera view matrix locally (the same six lines every room's
       draw uses) so this module is self-contained, then compose the boss's own
       rotation+translation onto it per instance. The caller's matrix is put
       back at the end — the sprite enemies, pickups and door text drawn after
       this all assume the plain view matrix is loaded. */
    MATRIX view;
    SVECTOR neg_rot = {0, -cam_rot, 0, 0};
    RotMatrix(&neg_rot, &view);
    VECTOR vt = {-cam_x, -cam_y, -cam_z};
    ApplyMatrixLV(&view, &vt, &vt);
    view.t[0] = vt.vx;
    view.t[1] = vt.vy;
    view.t[2] = vt.vz;

    int i, drew_any = 0;
    for (i = 0; i < rabisu_count; i++) {
        Rabisu *r = &rabisus[i];
        if (!r->active || r->dead) continue;
        if (r->area != game_state) continue;

        int32_t dcx = r->x - cam_x, dcz = r->z - cam_z;
        int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
        if (dist > RBS_DRAW_DIST) continue;

        /* Y-rotation built straight from the stored facing vector. This is
           exactly the matrix RotMatrix({0,yaw,0}) would produce for the yaw
           whose sine and cosine those are — same handedness as every other
           rotated prop in the game (see chainlink_door.c's footprint maths). */
        MATRIX pm, combined;
        pm.m[0][0] = (int16_t)r->face_c; pm.m[0][1] = 0;          pm.m[0][2] = (int16_t)r->face_s;
        pm.m[1][0] = 0;                  pm.m[1][1] = (int16_t)ONE; pm.m[1][2] = 0;
        pm.m[2][0] = (int16_t)-r->face_s; pm.m[2][1] = 0;         pm.m[2][2] = (int16_t)r->face_c;

        /* RBS_FOOT_OFF is applied HERE and nowhere else: it cancels the model's
           authored offset so vertex y=-168 (its lowest) lands on the anchor.
           Adding it moves the model DOWN, because -Y is up. */
        VECTOR pos = { r->x, r->y + RBS_FOOT_OFF, r->z };
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
        if (!r->active || r->dead || r->area != game_state) continue;
        draw_rbs_bar(ctx, r);
    }
}
