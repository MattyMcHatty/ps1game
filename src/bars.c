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
#include "texmgr.h"
#include "title.h"          /* current_area gate */
#include "sound.h"          /* SFX_SLAM — the first impact */
#include "bars.h"

/* Bars — see bars.h for the drop, why the mesh is re-centred at load, and why
   the caller's texture window matters to this prop. */

typedef struct {
    GameState area;                        /* only draws/collides in this room  */
    int32_t   x, y, z, rot_y;              /* centre in plan; world y = y+GROUND_FLOOR_Y */
    int32_t   min_x, max_x, min_z, max_z;  /* world AABB, baked at place time   */
    int       active;

    BarsState state;
    int32_t   lift_fp;    /* base above the floor, 1/256 units; + is up        */
    int32_t   vel_fp;     /* 1/256 units a frame; + is DOWN (falling)          */
    int       landed;     /* the first impact has happened (its sound played)  */
    int32_t   rise_t;     /* frames into a bars_raise()                        */
    int32_t   rise_to;    /* the lift it is winching up to, whole units        */
    int32_t   sx, sy;     /* width / height scale, 4096ths (bars_set_size)     */
    int32_t   sl_fx, sl_fz, sl_tx, sl_tz;   /* a bars_slide(): from, to       */
    int32_t   sl_t;       /* frames into it                                    */
} Bars;

static Bars bars[MAX_BARS];
static int  bars_count = 0;

static SMD  *bars_smd = NULL;
static void *bars_buf = NULL;

/* The collision mesh — the draw's own, measured at load AFTER the re-centre,
   so these are model-space extents about the plan centre and up from the base.
   -Y is up, so br_min_y is the top rail and br_max_y is 0, the base. */
static int32_t br_min_x = 0, br_max_x = 0;   /* -430 .. 430 as authored */
static int32_t br_min_z = 0, br_max_z = 0;   /*  -25 ..  25             */
static int32_t br_min_y = 0, br_max_y = 0;   /* -860 ..   0             */

static int bars_tex = -1;

/* The player's head, relative to cam_y — the crib's CR_PLAYER_HEAD, and the
   same figure apply_collision_* uses for its own body span. */
#define BR_PLAYER_HEAD 30

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

/* Startup. One sector of geometry held for the run, and a texture registration
   that reads nothing but a TIM header until the Catacombs bank is selected.

   >>> THE TEXTURE'S PAGE IS BORROWED, AND IT HAS A WAY BACK ALREADY. <<<
   x512 y256's left half is wd_dr_crk — the kitchen fat door's cracked face —
   and the Greenhouse's two pipe-button textures in its top-left corner. All
   three are put back by calls that already run on the entry of every room that
   draws them: kitchen_restore_textures() re-stamps wd_dr_crk (and its CLUT, at
   (0,497), which this texture borrows on the crib's argument: a palette whose
   owner's PIXELS it is already displacing, so the two go back together), and
   the Greenhouse streams its own buttons on its own entry. So nothing is owed.
   That is the pair of entries in tools/vram_map.py's KNOWN_STREAM_PAIRS.

   It is a HALF page because it is 4bpp — iron bars against transparency need
   fifteen colours and a hole — and every whole Voff-0 page in the Catacombs
   bank was spent when the Pit took x704 y0.

   >>> THE 73rd texmgr REGISTRATION OF 80. <<< Past TEXMGR_MAX a register
   returns -1 silently and that texture breaks in every room; count with
   py tools/heap_budget.py before adding more. */
void bars_load_assets(void) {
    /* BANK: Chapter 3 only. Derived, not guessed — py tools/check_tex_banks.py
       walks the uploader call graph (the_pit_upload_textures reaches this
       module) and fails if CATACOMBS is not in the mask. */
    texmgr_set_bank(TEXBANK_CATACOMBS);

    bars_buf = read_file("\\TEXCTCMB\\BARS.SMD;1");
    if (bars_buf) bars_smd = smdInitData(bars_buf);

    /* MEASURE, then RE-CENTRE, then measure again. See bars.h: the export is in
       The Pit's world coordinates, and this is the one place that turns it
       into a model. The plan centre goes to the origin and the base (the
       LARGEST y, since -Y is up) to y=0. Done in place on the SVECTORs, once,
       on a buffer nothing else reads. */
    if (bars_smd && bars_smd->n_verts > 0) {
        int i;
        int32_t mnx, mxx, mny, mxy, mnz, mxz;
        mnx = mxx = bars_smd->p_verts[0].vx;
        mny = mxy = bars_smd->p_verts[0].vy;
        mnz = mxz = bars_smd->p_verts[0].vz;
        for (i = 1; i < bars_smd->n_verts; i++) {
            int32_t vx = bars_smd->p_verts[i].vx;
            int32_t vy = bars_smd->p_verts[i].vy;
            int32_t vz = bars_smd->p_verts[i].vz;
            if (vx < mnx) mnx = vx;
            if (vx > mxx) mxx = vx;
            if (vy < mny) mny = vy;
            if (vy > mxy) mxy = vy;
            if (vz < mnz) mnz = vz;
            if (vz > mxz) mxz = vz;
        }
        int32_t cx = (mnx + mxx) / 2, cz = (mnz + mxz) / 2;
        for (i = 0; i < bars_smd->n_verts; i++) {
            bars_smd->p_verts[i].vx = (int16_t)(bars_smd->p_verts[i].vx - cx);
            bars_smd->p_verts[i].vy = (int16_t)(bars_smd->p_verts[i].vy - mxy);
            bars_smd->p_verts[i].vz = (int16_t)(bars_smd->p_verts[i].vz - cz);
        }
        br_min_x = mnx - cx;  br_max_x = mxx - cx;
        br_min_y = mny - mxy; br_max_y = 0;
        br_min_z = mnz - cz;  br_max_z = mxz - cz;
    }

    bars_tex = texmgr_register("\\TEXCTCMB\\BARS.TIM;1");
}

void bars_upload_texture(void) {
    texmgr_upload(bars_tex);
}

void bars_clear(void) { bars_count = 0; }

/* World AABB of the rotated, scaled footprint, corner by corner — the crib's
   bake, for the crib's reason. The drop moves only y, so only a scale or a
   slide redoes it. */
static void bars_bake(Bars *b) {
    int32_t cs = icos(b->rot_y), sn = isin(b->rot_y);
    int32_t mnx = (br_min_x * b->sx) >> 12, mxx = (br_max_x * b->sx) >> 12;
    const int32_t lx[4] = { mnx, mxx, mxx, mnx };
    const int32_t lz[4] = { br_min_z, br_min_z, br_max_z, br_max_z };
    int k;
    for (k = 0; k < 4; k++) {
        int32_t wx = b->x + ((lx[k] * cs + lz[k] * sn) >> 12);
        int32_t wz = b->z + ((lz[k] * cs - lx[k] * sn) >> 12);
        if (k == 0) {
            b->min_x = b->max_x = wx;
            b->min_z = b->max_z = wz;
        } else {
            if (wx < b->min_x) b->min_x = wx;
            if (wx > b->max_x) b->max_x = wx;
            if (wz < b->min_z) b->min_z = wz;
            if (wz > b->max_z) b->max_z = wz;
        }
    }
}

int bars_place(GameState area, int32_t x, int32_t y, int32_t z,
               int32_t rot_y, int32_t lift) {
    if (bars_count >= MAX_BARS) return -1;
    int idx = bars_count++;
    Bars *b = &bars[idx];
    b->area  = area;
    b->x = x;  b->y = y;  b->z = z;
    b->rot_y = rot_y;
    b->active  = 1;
    b->state   = lift > 0 ? BARS_RAISED : BARS_DOWN;
    b->lift_fp = lift << 8;
    b->vel_fp  = 0;
    b->landed  = (lift <= 0);
    b->sx = b->sy = 4096;
    bars_bake(b);
    return idx;
}

void bars_set_size(int idx, int32_t width, int32_t height) {
    if (idx < 0 || idx >= bars_count) return;
    int32_t w = br_max_x - br_min_x, h = br_max_y - br_min_y;
    if (w <= 0 || h <= 0) return;   /* no mesh: nothing to fit */
    bars[idx].sx = (width  << 12) / w;
    bars[idx].sy = (height << 12) / h;
    bars_bake(&bars[idx]);
}

void bars_slide(int idx, int32_t x, int32_t z) {
    if (idx < 0 || idx >= bars_count) return;
    Bars *b = &bars[idx];
    if (b->state != BARS_DOWN) return;
    b->state = BARS_SLIDING;
    b->sl_fx = b->x;  b->sl_fz = b->z;
    b->sl_tx = x;     b->sl_tz = z;
    b->sl_t  = 0;
    sound_play(SFX_MCHNE);
}

int bars_sliding(int idx) {
    if (idx < 0 || idx >= bars_count) return 0;
    return bars[idx].state == BARS_SLIDING;
}

void bars_drop(int idx) {
    if (idx < 0 || idx >= bars_count) return;
    Bars *b = &bars[idx];
    if (b->state != BARS_RAISED) return;
    b->state  = BARS_FALLING;
    b->vel_fp = 0;
}

int bars_settled(int idx) {
    if (idx < 0 || idx >= bars_count) return 1;   /* nothing to wait for */
    return bars[idx].state == BARS_DOWN;
}

void bars_raise(int idx, int32_t lift) {
    if (idx < 0 || idx >= bars_count) return;
    Bars *b = &bars[idx];
    if (b->state != BARS_DOWN) return;
    b->state   = BARS_RISING;
    b->rise_t  = 0;
    b->rise_to = lift;
    b->vel_fp  = 0;
    sound_play(SFX_MCHNE);          /* first of two; the second is in bars_update */
}

int bars_raised(int idx) {
    if (idx < 0 || idx >= bars_count) return 1;
    return bars[idx].state == BARS_RAISED;
}

void bars_update(void) {
    int i;
    for (i = 0; i < bars_count; i++) {
        Bars *b = &bars[i];
        if (!b->active || b->area != current_area) continue;

        /* THE RISE. Recomputed from the frame counter, not accumulated, so it
           ends exactly on BARS_RISE_FRAMES (the piano bookcase's reasoning). The
           second clip goes on the frame the first runs out. */
        if (b->state == BARS_RISING) {
            b->rise_t++;
            if (b->rise_t == BARS_MCHNE_FRAMES) sound_play(SFX_MCHNE);
            b->lift_fp = ((b->rise_to << 8) * b->rise_t) / BARS_RISE_FRAMES;
            if (b->rise_t >= BARS_RISE_FRAMES) {
                b->lift_fp = b->rise_to << 8;
                b->state   = BARS_RAISED;
                /* Armed to drop again, bounce and slam included. */
                b->landed  = 0;
            }
            continue;
        }

        /* THE SLIDE, recomputed from the frame counter for the same reason,
           and the AABB rebaked every frame so collision moves with the art. */
        if (b->state == BARS_SLIDING) {
            b->sl_t++;
            if (b->sl_t >= BARS_SLIDE_FRAMES) {
                b->x = b->sl_tx;
                b->z = b->sl_tz;
                b->state = BARS_DOWN;
            } else {
                b->x = b->sl_fx + ((b->sl_tx - b->sl_fx) * b->sl_t) / BARS_SLIDE_FRAMES;
                b->z = b->sl_fz + ((b->sl_tz - b->sl_fz) * b->sl_t) / BARS_SLIDE_FRAMES;
            }
            bars_bake(b);
            continue;
        }

        if (b->state != BARS_FALLING) continue;

        /* Semi-implicit Euler: speed first, then position, so the first frame
           of the drop already moves. */
        b->vel_fp  += BARS_GRAVITY;
        b->lift_fp -= b->vel_fp;
        if (b->lift_fp > 0) continue;

        /* THE FLOOR. Clamp to it, then bounce at a fraction of the impact
           speed — or stop, if that fraction would be too slow to see. The
           settle test is on the REBOUND speed, not the impact speed, so a drop
           from any height ends in the same small rattle. */
        b->lift_fp = 0;
        if (!b->landed) {
            b->landed = 1;
            sound_play(SFX_SLAM);
        }
        {
            int32_t rebound = (b->vel_fp * BARS_RESTITUTION) >> 8;
            if (rebound < BARS_SETTLE_SPEED) {
                b->vel_fp = 0;
                b->state  = BARS_DOWN;
            } else {
                b->vel_fp = -rebound;          /* going back up */
            }
        }
    }
}

void bars_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < bars_count; i++) {
        Bars *b = &bars[i];
        if (!b->active || b->area != current_area) continue;

        /* Vertical gate on the CURRENT base, which is the floor less the lift.
           Raised, the solid span is entirely above the player's head and this
           skips; down, it is the full 860 from the floor up. */
        int32_t floor_y   = b->y + GROUND_FLOOR_Y;
        int32_t base_y    = floor_y - (b->lift_fp >> 8);
        int32_t solid_bot = base_y + ((br_max_y * b->sy) >> 12);
        int32_t solid_top = base_y + ((br_min_y * b->sy) >> 12);
        int32_t feet = py + GROUND_FLOOR_Y, head = py - BR_PLAYER_HEAD;
        if (head >= solid_bot || feet <= solid_top) continue;

        int32_t min_x = b->min_x - radius, max_x = b->max_x + radius;
        int32_t min_z = b->min_z - radius, max_z = b->max_z + radius;
        if (*px <= min_x || *px >= max_x) continue;
        if (*pz <= min_z || *pz >= max_z) continue;

        int32_t pl = *px - min_x, pr = max_x - *px;
        int32_t pf = *pz - min_z, pb = max_z - *pz;
        int32_t m = pl, ddx = -pl, ddz = 0;
        if (b->state == BARS_SLIDING) {
            /* MOVING: out along the thin axis only, to whichever side of the
               line the body is on. The shallowest axis would be the direction
               of travel, and that shoves the player ahead of the gate into the
               next wall. */
            if (b->max_x - b->min_x < b->max_z - b->min_z) {
                ddz = 0;
                ddx = (pl < pr) ? -pl : pr;
            } else {
                ddx = 0;
                ddz = (pf < pb) ? -pf : pb;
            }
            *px += ddx; *pz += ddz;
            continue;
        }
        if (pr < m) { m = pr; ddx =  pr; ddz = 0; }
        if (pf < m) { m = pf; ddx = 0; ddz = -pf; }
        if (pb < m) {         ddx = 0; ddz =  pb; }
        *px += ddx; *pz += ddz;
    }
}

/* The crib's draw with the tilt and the beam taken out: one yaw, one
   translation, the textured-prim loop. Sorted at TRUE scene depth with no +40,
   for the crib's reason — the room biases its own mesh 40 buckets deeper and a
   prop that took the same bias would lose every tie with the wall behind it.
   That matters here more than it did for the cot: this prop stands 5 units in
   front of the alcove's jambs.

   See bars.h for why the caller's 128 texture window has to be in force. */
void bars_draw(RenderContext *ctx) {
    if (!bars_smd) return;

    MATRIX view;
    camera_build_view(&view);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint16_t tp = texmgr_tpage(bars_tex);
    uint16_t cl = texmgr_clut(bars_tex);

    int i;
    for (i = 0; i < bars_count; i++) {
        Bars *b = &bars[i];
        if (!b->active || b->area != current_area) continue;

        /* The room's fog-far and lights, as the crib's does. In The Pit that
           is a live number the Helluminator moves, AND the ambush opens it up
           for the length of the drop so the fall can be seen from the bottom
           of the slope (src/the_pit.c). */
        int32_t dcx = b->x - cam_x, dcz = b->z - cam_z;
        int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
        dist = render_light_dist(b->x, b->z, dist);
        if (dist > g_fog_far) continue;

        MATRIX m, combined;
        SVECTOR yaw_r = {0, (int16_t)b->rot_y, 0, 0};
        RotMatrix(&yaw_r, &m);
        /* bars_set_size: scale the model's x (width) and y (height) columns,
           so the scale is applied in model space before the yaw. */
        if (b->sx != 4096 || b->sy != 4096) {
            int r;
            for (r = 0; r < 3; r++) {
                m.m[r][0] = (int16_t)((m.m[r][0] * b->sx) >> 12);
                m.m[r][1] = (int16_t)((m.m[r][1] * b->sy) >> 12);
            }
        }
        VECTOR pos = {b->x, b->y + GROUND_FLOOR_Y - (b->lift_fp >> 8), b->z};
        TransMatrix(&m, &pos);
        CompMatrixLV(&view, &m, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        int32_t fog_factor = render_fog_scale(dist);

        uint8_t *p = (uint8_t *)bars_smd->p_prims;
        int pi;
        for (pi = 0; pi < bars_smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt       = (SMD_PRI_TYPE *)p;
            uint8_t       stride   = pt->len;
            int           is_quad  = (pt->type >= 2);
            int           textured = pt->texture;

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &bars_smd->p_verts[vi[0]];
            SVECTOR *v1 = &bars_smd->p_verts[vi[1]];
            SVECTOR *v2 = &bars_smd->p_verts[vi[2]];

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
                SVECTOR *v3 = &bars_smd->p_verts[vi[3]];
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
            if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
            if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

            /* The chapter's fog colour, 7,6,9, hard-coded as the crib's is. */
            uint8_t *col = p + 16;
            uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + 7 * (256 - fog_factor)) >> 8);
            uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + 6 * (256 - fog_factor)) >> 8);
            uint8_t bl = (uint8_t)(((int32_t)col[2] * fog_factor + 9 * (256 - fog_factor)) >> 8);

            if (is_quad && textured) {
                if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
                uint8_t *uv = p + 20;
                POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
                setPolyFT4(poly);
                setRGB0(poly, r, g, bl);
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
                setRGB0(poly, r, g, bl);
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
                setRGB0(poly, r, g, bl);
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
                setRGB0(poly, r, g, bl);
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_F3);
            }

            p += stride;
        }
    }

    /* Back to the plain view matrix for whatever the caller draws next. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
