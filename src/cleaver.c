#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"         /* cam_x/y/z, player_knockback */
#include "player.h"         /* player_hurt, player_health, game_over */
#include "collision.h"      /* GROUND_FLOOR_Y */
#include "title.h"          /* current_area gate */
#include "tim_slots.h"      /* TIM_TPAGE_RUSTY / TIM_CLUT_RUSTY */
#include "sound.h"          /* SFX_SLAM, SFX_HURT */
#include "cleaver.h"

/* Cleaver — see cleaver.h for the cycle, the hit, and why the draw skips
   whatever is above the ceiling. */

typedef struct {
    GameState    area;                        /* only runs/draws/collides here */
    int32_t      x, y, z;                     /* centre in plan; world y = y+GROUND_FLOOR_Y */
    int32_t      min_x, max_x, min_z, max_z;  /* world AABB, baked at place time */
    int32_t      ceiling_y;                   /* world y of the vault it hangs from */
    int          active;

    CleaverState state;
    int32_t      lift;      /* the lift it hangs at when raised, whole units   */
    int32_t      lift_fp;   /* edge above the floor now, 1/256 units; + is up  */
    int32_t      vel_fp;    /* 1/256 units a frame; + is DOWN (falling)        */
    int          landed;    /* this drop's impact has happened (sound played) */
    int          hit_done;  /* this drop has already hit the player           */
    int32_t      timer;     /* frames left in DOWN / RAISED, or into RISING   */
} Cleaver;

static Cleaver cleavers[MAX_CLEAVERS];
static int     cleaver_count = 0;

static SMD  *cl_smd = NULL;
static void *cl_buf = NULL;

/* The collision mesh — the draw's own, measured at load AFTER the re-centre, so
   these are model-space extents about the plan centre and up from the edge.
   -Y is up, so cl_min_y is the blade's top and cl_max_y is 0, the edge. */
static int32_t cl_min_x = 0, cl_max_x = 0;   /*  -15 ..  15 as authored */
static int32_t cl_min_z = 0, cl_max_z = 0;   /* -275 .. 275             */
static int32_t cl_min_y = 0, cl_max_y = 0;   /* -500 ..   0             */

/* What the re-centre subtracted: the authored placement (see cleaver.h). */
static int32_t cl_auth_x = 0, cl_auth_z = 0, cl_auth_lift = 0;

/* The player's head, relative to cam_y — the bars' BR_PLAYER_HEAD. */
#define CL_PLAYER_HEAD 30

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

/* Startup. One sector of geometry held for the run, and nothing else: the
   texture is The Pit's, uploaded by the room (the_pit_upload_rusty). */
void cleavers_load_assets(void) {
    cl_buf = read_file("\\TEXCTCMB\\CLEAVER.SMD;1");
    if (cl_buf) cl_smd = smdInitData(cl_buf);

    /* MEASURE, then RE-CENTRE — the bars' pass. The plan centre goes to the
       origin and the cutting edge (the LARGEST y) to y=0, and what was taken
       off is kept as the authored placement. The export's floor is the
       corridor's y=0, so the edge's height above it is simply -max_y. */
    if (cl_smd && cl_smd->n_verts > 0) {
        int i;
        int32_t mnx, mxx, mny, mxy, mnz, mxz;
        mnx = mxx = cl_smd->p_verts[0].vx;
        mny = mxy = cl_smd->p_verts[0].vy;
        mnz = mxz = cl_smd->p_verts[0].vz;
        for (i = 1; i < cl_smd->n_verts; i++) {
            int32_t vx = cl_smd->p_verts[i].vx;
            int32_t vy = cl_smd->p_verts[i].vy;
            int32_t vz = cl_smd->p_verts[i].vz;
            if (vx < mnx) mnx = vx;
            if (vx > mxx) mxx = vx;
            if (vy < mny) mny = vy;
            if (vy > mxy) mxy = vy;
            if (vz < mnz) mnz = vz;
            if (vz > mxz) mxz = vz;
        }
        int32_t cx = (mnx + mxx) / 2, cz = (mnz + mxz) / 2;
        for (i = 0; i < cl_smd->n_verts; i++) {
            cl_smd->p_verts[i].vx = (int16_t)(cl_smd->p_verts[i].vx - cx);
            cl_smd->p_verts[i].vy = (int16_t)(cl_smd->p_verts[i].vy - mxy);
            cl_smd->p_verts[i].vz = (int16_t)(cl_smd->p_verts[i].vz - cz);
        }
        cl_min_x = mnx - cx;  cl_max_x = mxx - cx;
        cl_min_y = mny - mxy; cl_max_y = 0;
        cl_min_z = mnz - cz;  cl_max_z = mxz - cz;

        cl_auth_x    = cx;
        cl_auth_z    = cz;
        cl_auth_lift = -mxy;
    }
}

int32_t cleaver_authored_x(void)    { return cl_auth_x; }
int32_t cleaver_authored_z(void)    { return cl_auth_z; }
int32_t cleaver_authored_lift(void) { return cl_auth_lift; }

void cleavers_clear(void) { cleaver_count = 0; }

int cleaver_place(GameState area, int32_t x, int32_t y, int32_t z,
                  int32_t lift, int32_t ceiling_y) {
    if (cleaver_count >= MAX_CLEAVERS) return -1;
    int idx = cleaver_count++;
    Cleaver *c = &cleavers[idx];
    c->area      = area;
    c->x = x;  c->y = y;  c->z = z;
    c->ceiling_y = ceiling_y;
    c->active    = 1;
    c->state     = CL_ARMED;
    c->lift      = lift;
    c->lift_fp   = lift << 8;
    c->vel_fp    = 0;
    c->landed    = 0;
    c->hit_done  = 0;
    c->timer     = 0;
    /* No yaw: every blade hangs the way it was modelled, across the corridor. */
    c->min_x = x + cl_min_x;  c->max_x = x + cl_max_x;
    c->min_z = z + cl_min_z;  c->max_z = z + cl_max_z;
    return idx;
}

static void cl_start_drop(Cleaver *c) {
    c->state    = CL_FALLING;
    c->vel_fp   = 0;
    c->landed   = 0;
    c->hit_done = 0;
}

/* THE HIT: the edge at or below the player's head, and the player's centre
   inside the footprint widened by CL_HIT_REACH. Once per drop. */
static void cl_check_hit(Cleaver *c) {
    if (c->hit_done || game_over) return;
    int32_t edge_y = c->y + GROUND_FLOOR_Y - (c->lift_fp >> 8);
    int32_t head   = cam_y - CL_PLAYER_HEAD;
    if (edge_y < head) return;                      /* still above them */
    if (cam_x <= c->min_x - CL_HIT_REACH || cam_x >= c->max_x + CL_HIT_REACH) return;
    if (cam_z <= c->min_z - CL_HIT_REACH || cam_z >= c->max_z + CL_HIT_REACH) return;

    c->hit_done = 1;
    player_hurt(CL_DAMAGE);
    sound_play(SFX_HURT);
    /* Straight along X, away from the blade: `from` is level with the player in
       Z, so player_knockback's direction has no Z in it. Dead centre goes -X. */
    player_knockback(cam_x > c->x ? c->x : c->x + 1, cam_z, CL_KNOCKBACK);
    if (player_health <= 0) {
        player_health = 0;
        game_over     = 1;
        flash_timer   = 90;
    }
}

void cleavers_update(void) {
    int i;
    for (i = 0; i < cleaver_count; i++) {
        Cleaver *c = &cleavers[i];
        if (!c->active || c->area != current_area) continue;

        switch (c->state) {
        case CL_ARMED: {
            /* "Almost underneath": in X only — the blade spans the corridor. */
            int32_t dx = cam_x - c->x;
            if (dx < 0) dx = -dx;
            if (dx < CL_TRIGGER_REACH) cl_start_drop(c);
            break;
        }
        case CL_RAISED:
            if (--c->timer <= 0) cl_start_drop(c);
            break;
        case CL_DOWN:
            if (--c->timer <= 0) {
                c->state = CL_RISING;
                c->timer = 0;
            }
            break;
        case CL_RISING:
            /* Recomputed from the frame counter, not accumulated, so it ends
               exactly on CL_RISE_FRAMES (the bars' reasoning). */
            c->timer++;
            c->lift_fp = ((c->lift << 8) * c->timer) / CL_RISE_FRAMES;
            if (c->timer >= CL_RISE_FRAMES) {
                c->lift_fp = c->lift << 8;
                c->state   = CL_RAISED;
                c->timer   = CL_UP_FRAMES;
            }
            break;
        default:
            break;
        }

        if (c->state != CL_FALLING) continue;

        /* Semi-implicit Euler, the bars' integrator: speed first, then
           position, so the first frame of the drop already moves. */
        c->vel_fp  += CL_GRAVITY;
        c->lift_fp -= c->vel_fp;
        if (c->lift_fp < 0) c->lift_fp = 0;
        if (c->vel_fp > 0) cl_check_hit(c);     /* only on the way DOWN */
        if (c->lift_fp > 0) continue;

        /* THE FLOOR. The bars' bounce, with less spring in it. */
        if (!c->landed) {
            c->landed = 1;
            sound_play(SFX_SLAM);
        }
        {
            int32_t rebound = (c->vel_fp * CL_RESTITUTION) >> 8;
            if (rebound < CL_SETTLE_SPEED) {
                c->vel_fp = 0;
                c->state  = CL_DOWN;
                c->timer  = CL_DOWN_FRAMES;
            } else {
                c->vel_fp = -rebound;          /* going back up */
            }
        }
    }
}

void cleavers_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < cleaver_count; i++) {
        Cleaver *c = &cleavers[i];
        if (!c->active || c->area != current_area) continue;

        /* Vertical gate on the CURRENT edge. Raised, the blade is entirely over
           the player's head and this skips. */
        int32_t floor_y   = c->y + GROUND_FLOOR_Y;
        int32_t edge_y    = floor_y - (c->lift_fp >> 8);
        int32_t solid_bot = edge_y + cl_max_y;
        int32_t solid_top = edge_y + cl_min_y;
        int32_t feet = py + GROUND_FLOOR_Y, head = py - CL_PLAYER_HEAD;
        if (head >= solid_bot || feet <= solid_top) continue;

        int32_t min_x = c->min_x - radius, max_x = c->max_x + radius;
        int32_t min_z = c->min_z - radius, max_z = c->max_z + radius;
        if (*px <= min_x || *px >= max_x) continue;
        if (*pz <= min_z || *pz >= max_z) continue;

        int32_t pl = *px - min_x, pr = max_x - *px;
        int32_t pf = *pz - min_z, pb = max_z - *pz;
        int32_t m = pl, ddx = -pl, ddz = 0;
        if (pr < m) { m = pr; ddx =  pr; ddz = 0; }
        if (pf < m) { m = pf; ddx = 0; ddz = -pf; }
        if (pb < m) {         ddx = 0; ddz =  pb; }
        *px += ddx; *pz += ddz;
    }
}

/* The bars' draw without the yaw, plus THE SLOT: a primitive whose every vertex
   is above the instance's ceiling is not drawn (see cleaver.h). Sorted at TRUE
   scene depth with no +40, for the crib's and the bars' reason. */
void cleavers_draw(RenderContext *ctx) {
    if (!cl_smd) return;

    MATRIX view;
    camera_build_view(&view);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    int i;
    for (i = 0; i < cleaver_count; i++) {
        Cleaver *c = &cleavers[i];
        if (!c->active || c->area != current_area) continue;

        int32_t dcx = c->x - cam_x, dcz = c->z - cam_z;
        int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
        dist = render_light_dist(c->x, c->z, dist);
        if (dist > g_fog_far) continue;

        int32_t pos_y = c->y + GROUND_FLOOR_Y - (c->lift_fp >> 8);
        /* The slot line in MODEL space: a vertex with vy below this is above the
           vault. -Y is up, so "above" is numerically smaller. */
        int32_t slot_vy = c->ceiling_y - pos_y;

        MATRIX m, combined;
        SVECTOR no_rot = {0, 0, 0, 0};
        RotMatrix(&no_rot, &m);
        VECTOR pos = {c->x, pos_y, c->z};
        TransMatrix(&m, &pos);
        CompMatrixLV(&view, &m, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        int32_t fog_factor = render_fog_scale(dist);

        uint8_t *p = (uint8_t *)cl_smd->p_prims;
        int pi;
        for (pi = 0; pi < cl_smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt       = (SMD_PRI_TYPE *)p;
            uint8_t       stride   = pt->len;
            int           is_quad  = (pt->type >= 2);
            int           textured = pt->texture;

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &cl_smd->p_verts[vi[0]];
            SVECTOR *v1 = &cl_smd->p_verts[vi[1]];
            SVECTOR *v2 = &cl_smd->p_verts[vi[2]];
            SVECTOR *v3 = is_quad ? &cl_smd->p_verts[vi[3]] : v2;

            /* THE SLOT: wholly above the vault, so wholly inside the slot. */
            {
                int32_t low = v0->vy;
                if (v1->vy > low) low = v1->vy;
                if (v2->vy > low) low = v2->vy;
                if (v3->vy > low) low = v3->vy;
                if (low < slot_vy) { p += stride; continue; }
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

            if (!pt->nocull) {
                gte_nclip();
                gte_stopz(&nclip);
                if (nclip <= 0) { p += stride; continue; }
            }

            gte_stsz4c(sz);
            if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

            if (is_quad) {
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

            /* The chapter's fog colour, 7,6,9, hard-coded as the bars' is. */
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
                poly->tpage = TIM_TPAGE_RUSTY;
                poly->clut  = TIM_CLUT_RUSTY;
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
                poly->tpage = TIM_TPAGE_RUSTY;
                poly->clut  = TIM_CLUT_RUSTY;
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
