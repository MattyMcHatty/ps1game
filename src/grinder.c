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
#include "tim_slots.h"
#include "grinder.h"
#include "grinder_tex_map.h"
#include "title.h"          /* GameState / current_area — see grinder_add */

Grinder grinders[MAX_GRINDERS];
int     grinder_count = 0;

static SMD  *grinder_smd    = NULL;
static void *grinder_buffer = NULL;

/* ---- The grinder's own texture ---------------------------------------------
   64x64 8bpp at VRAM x[992,1024) y[256,384) — the upper half of the page whose
   lower half holds the delivery area's TREES_DL clone. Nothing else in the game
   lives there, so this is the TREES_DL arrangement exactly: uploaded once at
   startup and never restored, which means NO texmgr registration (the cap is
   hard and an overrun fails silently — see texmgr.c), no entry-time LoadImage,
   and no restore obligation on any room.

   THE ART IS 64 TEXELS WIDE AND THE MODEL'S UVs SPAN 128, WHICH IS WHY
   grinders_draw HALVES THEM. The SMX exporter normalises every UV to a fixed
   128-texel tile (tools/TEXTURING_NOTES.txt PART 1 B2), so the grinder plate is
   mapped 0-128 across regardless of how big grinder.png actually is; smxlink
   then adds the texture's page-relative column, which for x992 at 8bpp is 64.
   The SMD therefore arrives carrying U 64-192 — correctly based, twice too wide.
   Halving about that base lands it on U 64-128, the 64 columns the art occupies.

   V needs no base (y256 is page-aligned, so the art starts at V 0) but does need
   the same halving, for the same reason.

   THE HALVING COSTS NO DETAIL: the source really is 64x64, so the whole plate
   still maps the whole image exactly once. What it replaces is upscaling
   grinder.png to 128x128, which would have needed 64 free VRAM columns at
   Voff 0 — and there are none left anywhere (see the slot table in
   src/rear_gate.c), so the art would have had to stream over another room's
   page instead of costing nothing.

   IT LIVES HERE, NOT IN THE .SMX. The exporter rewrites every UV on every
   export, so a fix baked into the asset would be silently undone the next time
   the model comes out of Blender. This survives a re-export untouched. */
#define GRINDER_U_BASE   64   /* first texel column of the art within its tpage */
#define GRINDER_U_MAX   127   /* last texel column that is still our art        */
#define GRINDER_V_MAX    63   /* last texel row  that is still our art          */

/* Halve one of the model's 128-wide tile UVs about the art's own base column.
   The clamp bites only on the 128 endpoint, which lands one past the last
   column and would otherwise wrap to U 0 under the room's 128 texture window —
   sampling TREES_DL, the art in the same page's lower half, for a single texel
   column at the seam. */
static inline uint8_t gr_u(uint8_t u) {
    int t = GRINDER_U_BASE + (((int)u - GRINDER_U_BASE) >> 1);
    if (t < GRINDER_U_BASE) t = GRINDER_U_BASE;
    return (uint8_t)(t > GRINDER_U_MAX ? GRINDER_U_MAX : t);
}
static inline uint8_t gr_v(uint8_t v) {
    int t = v >> 1;
    return (uint8_t)(t > GRINDER_V_MAX ? GRINDER_V_MAX : t);
}

/* ---- The wall cut ----------------------------------------------------------
   See the long note on cut_x in grinder.h for WHY this exists: the skirt quads
   run the model's full depth, so half-burying one leaves every skirt straddling
   the wall plane, and a painter's sort can only put such a quad wholly in front
   of the wall or wholly behind it.

   The clip is exact and cheap because of what the model is. Every skirt quad is
   axis-aligned and takes exactly TWO distinct model X values, so clipping never
   changes a prim's vertex count — each vertex past the plane simply slides down
   its own edge onto it. The partner to slide towards is whichever of a vertex's
   two edge-neighbours is on the near side, which for a two-X quad is always
   exactly one of them.

   PS1 quad topology, for the neighbour table below: v0,v1,v2 and v1,v3,v2 are
   the two triangles, so the edges are v0-v1, v0-v2, v1-v3, v2-v3 and the v1-v2
   diagonal is not an edge. A triangle is the ordinary three.

   UVs are interpolated by the same fraction, so the shortened skirt shows the
   near end of its texture at its true scale rather than a squashed copy of the
   whole thing. */
static const unsigned char GR_NB_QUAD[4][2] = { {1,2}, {0,3}, {0,3}, {1,2} };
static const unsigned char GR_NB_TRI [3][2] = { {1,2}, {0,2}, {0,1} };

/* Model-space cut for one instance: how far along the model's +X (the buried
   direction) the wall plane falls. icos is +4096 at rot 0 and -4096 at 2048,
   which is exactly the sign flip between a grinder in the east wall and one in
   the west — see the rot_y note in grinder.h. */
static inline int32_t gr_model_cut(const Grinder *g) {
    return ((g->cut_x - g->x) * icos(g->rot_y)) >> 12;
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

/* Startup: load geometry and put the grinder texture into VRAM for good. ALL CD
   access and the one LoadImage happen here — LoadImage is only safe before the
   main render loop begins (see tools/TEXTURING_NOTES.txt PROBLEM A), and because
   this slot is never overwritten there is no second upload to schedule.
   tpage/clut are compile-time constants from tim_slots.h, so nothing is read
   back off the disc to recover them. */
void grinder_load_assets(void) {
    grinder_buffer = read_file("\\TEX\\GRINDER.SMD;1");
    if (grinder_buffer)
        grinder_smd = smdInitData(grinder_buffer);

    void *tbuf = read_file("\\TEX\\GRINDER.TIM;1");
    if (tbuf) {
        TIM_IMAGE tim;
        GetTimInfo((uint32_t *)tbuf, &tim);
        LoadImage(tim.prect, tim.paddr);
        DrawSync(0);
        if (tim.mode & 0x8) {
            LoadImage(tim.crect, tim.caddr);
            DrawSync(0);
        }
        free(tbuf);
    }
}

void grinders_clear(void) {
    grinder_count = 0;
}

int grinder_add(int32_t x, int32_t y, int32_t z, int32_t rot_y, int32_t area,
                int32_t cut_x) {
    if (grinder_count >= MAX_GRINDERS) return -1;
    int i = grinder_count++;
    grinders[i].x      = x;
    grinders[i].y      = y;
    grinders[i].z      = z;
    grinders[i].rot_y  = rot_y;
    grinders[i].area   = area;
    grinders[i].cut_x  = cut_x;
    grinders[i].active = 1;
    grinders[i].half_w = 200;   /* model footprint: X +/-200, Z +/-200 */
    grinders[i].half_d = 200;
    return i;
}

/* One instance's push box, or 0 if the player's height misses it entirely. */
static int gr_push_box(const Grinder *g, int32_t py, int32_t radius,
                       int32_t *min_x, int32_t *max_x,
                       int32_t *min_z, int32_t *max_z) {
    if (!g->active || g->area != (int32_t)current_area) return 0;

    /* Vertical gate: only collide when the player is on the grinder's floor, so
       its footprint doesn't reach up or down a level (see the dresser). */
    int32_t dy = py - g->y;
    if ((dy < 0 ? -dy : dy) > GRINDER_HALF_H) return 0;

    /* The footprint is square, so the rotated bound is the same for every
       multiple of a quarter turn — but do the general maths anyway, since a
       puzzle that slides one of these may well turn it too. isin/icos are
       fixed-point (4096 = 1.0). */
    int32_t c = icos(g->rot_y), s = isin(g->rot_y);
    if (c < 0) c = -c;
    if (s < 0) s = -s;
    int32_t hw = (g->half_w * c + g->half_d * s) >> 12;
    int32_t hd = (g->half_w * s + g->half_d * c) >> 12;

    /* Minkowski-expanded AABB plus a push margin (same scheme as the dresser and
       the dining table) so the camera stops clear of the visible edges. */
    *min_x = g->x - hw - radius - GRINDER_PUSH_MARGIN;
    *max_x = g->x + hw + radius + GRINDER_PUSH_MARGIN;
    *min_z = g->z - hd - radius - GRINDER_PUSH_MARGIN;
    *max_z = g->z + hd + radius + GRINDER_PUSH_MARGIN;
    return 1;
}

static int crush_contacts = 0;

int grinders_crush_contacts(void) { return crush_contacts; }

void grinders_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    int32_t min_x, max_x, min_z, max_z;
    int     held_by_mover = 0;

    crush_contacts = 0;

    for (i = 0; i < grinder_count; i++) {
        if (!gr_push_box(&grinders[i], py, radius, &min_x, &max_x, &min_z, &max_z))
            continue;
        if (*px <= min_x || *px >= max_x) continue;
        if (*pz <= min_z || *pz >= max_z) continue;

        /* Contact, counted before the push resolves it — see the note on
           grinders_crush_contacts in the header. Only a grinder under power
           counts: walking into a parked one is furniture, not machinery. */
        if (grinders[i].moving) { crush_contacts++; held_by_mover = 1; }

        /* Push out along the axis with the smallest penetration. */
        int32_t push_l = *px - min_x;
        int32_t push_r = max_x - *px;
        int32_t push_f = *pz - min_z;
        int32_t push_b = max_z - *pz;

        int32_t min_push = push_l, px_delta = -push_l, pz_delta = 0;
        if (push_r < min_push) { min_push = push_r; px_delta =  push_r; pz_delta = 0; }
        if (push_f < min_push) { min_push = push_f; px_delta = 0; pz_delta = -push_f; }
        if (push_b < min_push) {                    px_delta = 0; pz_delta =  push_b; }

        *px += px_delta;
        *pz += pz_delta;
    }

    /* ---- Second pass: caught in the jaws --------------------------------------
       >>> TWO GRINDERS CLOSING ON THE SAME LANE OVERLAP COMPLETELY BY THE END,
       AND THE PASS ABOVE CANNOT CLEAR A PLAYER OUT OF BOTH. <<< Standing between
       them, the smallest penetration for each is sideways and the two point at
       each other: one shoves the player east into the other's box, which shoves
       them back west, and they sit on the centre line juddering a frame at a
       time instead of being put somewhere legal.

       So whichever box still holds them after the first pass pushes along Z
       instead — out of the mouth of the machine rather than across it. Z is the
       free axis by construction: a grinder travels along its own X, which is
       what cut_x already requires of rot_y, so no amount of travel ever closes
       it. Nothing else reaches this pass; a player beside a single grinder was
       already cleared above and this loop finds nothing to do.

       >>> BUT NOT WHILE THE MACHINE IS RUNNING. <<< Being spat out of the jaws
       is an ESCAPE, and a player who stands in a closing pair is supposed to be
       killed by it — ejecting them would end the contact a few frames in and
       take the crush with it. So this runs only once nothing that holds them is
       moving, which is the case it was written for: a player somehow left
       inside a pair that has already stopped, who would otherwise be stuck
       there for good. While the grinders run, they judder on the centre line
       and die there, which is the point of standing in front of them. */
    if (held_by_mover) return;

    for (i = 0; i < grinder_count; i++) {
        if (!gr_push_box(&grinders[i], py, radius, &min_x, &max_x, &min_z, &max_z))
            continue;
        if (*px <= min_x || *px >= max_x) continue;
        if (*pz <= min_z || *pz >= max_z) continue;

        int32_t push_f = *pz - min_z;      /* out through the south mouth */
        int32_t push_b = max_z - *pz;      /* out through the north mouth */
        *pz += (push_f < push_b) ? -push_f : push_b;
    }
}

int grinders_point_solid(int32_t x, int32_t y, int32_t z, int32_t slack) {
    int i;
    for (i = 0; i < grinder_count; i++) {
        Grinder *g = &grinders[i];
        if (!g->active || g->area != (int32_t)current_area) continue;
        /* Vertical span in WORLD Y to match the shot's y. The grinder stores y in
           offset space (drawn at g->y + GROUND_FLOOR_Y), so its base rests on the
           floor at world (g->y + GROUND_FLOOR_Y) and its body reaches
           GRINDER_SOLID_H above that. */
        int32_t base = g->y + GROUND_FLOOR_Y;
        if (y < base - GRINDER_SOLID_H || y > base) continue;
        /* Axis-aligned bound of the rotated footprint (same maths as the push
           collide), real size + a little bullet slack, no player push margin. */
        int32_t c = icos(g->rot_y), s = isin(g->rot_y);
        if (c < 0) c = -c;
        if (s < 0) s = -s;
        int32_t hw = (g->half_w * c + g->half_d * s) >> 12;
        int32_t hd = (g->half_w * s + g->half_d * c) >> 12;
        if (x < g->x - hw - slack || x > g->x + hw + slack) continue;
        if (z < g->z - hd - slack || z > g->z + hd + slack) continue;
        return 1;
    }
    return 0;
}

/* See the header. Mirrors the non-coordinate gates of grinders_point_solid —
   active, and belonging to the room the player is actually in — and nothing
   else. */
int grinders_any_solid(void) {
    int i;
    for (i = 0; i < grinder_count; i++)
        if (grinders[i].active && grinders[i].area == (int32_t)current_area) return 1;
    return 0;
}

/* Render all active grinders using the same textured-prim path as the room
   geometry (per-poly UVs from the SMD, the 128 texture window the room sets, and
   the room's own fog band via render_fog_scale). Each face is drawn with either
   the room's plinth texture (tex slot 0, passed in) or the grinder's own art
   (slot 1), per the grinder_tex_map. Restores the caller's view matrix before
   returning.

   FOG COLOUR IS THE GARDEN'S PURPLE (SKY_FOG_*), matching every room this prop
   is placed in today. An interior placement would want the dresser's darker
   20/15/10 instead; that is the one thing here that is not room-agnostic. */
void grinders_draw(RenderContext *ctx, uint16_t plinth_tpage, uint16_t plinth_clut) {
    if (!grinder_smd) return;

    /* Camera view matrix (same construction as the other prop renderers). */
    MATRIX view;
    camera_build_view(&view);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    int i;
    for (i = 0; i < grinder_count; i++) {
        Grinder *g = &grinders[i];
        if (!g->active) continue;

        int32_t dcx = g->x - cam_x, dcz = g->z - cam_z;
        int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
        if (dist > g_fog_far) continue;   /* the room's own view distance */

        /* Combine the view matrix with this grinder's world transform. */
        MATRIX gm, combined;
        SVECTOR gr = {0, (short)g->rot_y, 0, 0};
        RotMatrix(&gr, &gm);
        VECTOR pos = {g->x, g->y + GROUND_FLOOR_Y, g->z};
        TransMatrix(&gm, &pos);
        CompMatrixLV(&view, &gm, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        /* One fog factor per grinder — the model is 400 across against a fog band
           thousands of units wide, so a per-prim distance would buy nothing. */
        int32_t fog_factor = render_fog_scale(dist);

        /* Where the wall it is set into falls in model space (see gr_model_cut).
           A free-standing grinder cuts nothing. */
        int     cutting   = (g->cut_x != GRINDER_NO_CUT);
        int32_t model_cut = cutting ? gr_model_cut(g) : 0;

        uint8_t *p = (uint8_t *)grinder_smd->p_prims;
        int pi;
        for (pi = 0; pi < grinder_smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt     = (SMD_PRI_TYPE *)p;
            uint8_t       stride = pt->len;
            int           is_quad = (pt->type >= 2);

            uint16_t *vi = (uint16_t *)(p + 4);
            int       nv = is_quad ? 4 : 3;
            uint8_t  *uv = p + 20;

            /* ---- Clip to the wall plane (see the note above gr_model_cut) ----
               Vertices past model_cut slide down an edge onto it, taking their UV
               with them; a prim with nothing on the near side is dropped whole,
               which is the sunk bookcase's rule for a prim that has passed the
               floor. Untouched prims keep pointing straight at the SMD, so a
               free-standing grinder costs nothing here. */
            SVECTOR  vbuf[4];
            uint8_t  uvbuf[8];
            SVECTOR *vsrc[4];
            {
                int k;
                for (k = 0; k < nv; k++) vsrc[k] = &grinder_smd->p_verts[vi[k]];
                if (cutting) {
                    int far_mask = 0, near_count = 0;
                    for (k = 0; k < nv; k++) {
                        if (vsrc[k]->vx > model_cut) far_mask |= 1 << k;
                        else                         near_count++;
                    }
                    if (!near_count) { p += stride; continue; }
                    if (far_mask) {
                        for (k = 0; k < nv; k++) {
                            vbuf[k]        = *vsrc[k];
                            uvbuf[k*2]     = uv[k*2];
                            uvbuf[k*2 + 1] = uv[k*2 + 1];
                        }
                        for (k = 0; k < nv; k++) {
                            if (!(far_mask & (1 << k))) continue;
                            const unsigned char *nb = is_quad ? GR_NB_QUAD[k] : GR_NB_TRI[k];
                            int n = (far_mask & (1 << nb[0])) ? nb[1] : nb[0];
                            int32_t xf = vsrc[k]->vx, xn = vsrc[n]->vx;
                            int32_t den = xn - xf;
                            if (!den) continue;             /* parallel to the cut */
                            int32_t num = model_cut - xf;
                            vbuf[k].vx = (short)model_cut;
                            vbuf[k].vy = (short)(vsrc[k]->vy + ((vsrc[n]->vy - vsrc[k]->vy) * num) / den);
                            vbuf[k].vz = (short)(vsrc[k]->vz + ((vsrc[n]->vz - vsrc[k]->vz) * num) / den);
                            uvbuf[k*2]     = (uint8_t)((int32_t)uv[k*2]     + (((int32_t)uv[n*2]     - uv[k*2])     * num) / den);
                            uvbuf[k*2 + 1] = (uint8_t)((int32_t)uv[k*2 + 1] + (((int32_t)uv[n*2 + 1] - uv[k*2 + 1]) * num) / den);
                        }
                        for (k = 0; k < nv; k++) vsrc[k] = &vbuf[k];
                        uv = uvbuf;
                    }
                }
            }

            SVECTOR *v0 = vsrc[0];
            SVECTOR *v1 = vsrc[1];
            SVECTOR *v2 = vsrc[2];

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

            SVECTOR *v3    = 0;
            int32_t  v2_sz = sz[3];   /* v2's SZ, before the quad path reuses sz[3] */
            if (is_quad) {
                v3 = vsrc[3];
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
            /* Horizontal polys sort by their farthest corner (see render.h). */
            if (poly_is_flat_y(v0, v1, v2, v3))
                otz = is_quad ? otz_far4(sz[1], sz[2], v2_sz, sz[3])
                              : otz_far3(sz[1], sz[2], sz[3]);
            if (otz <= 0) { p += stride; continue; }
            otz += 40;
            /* Stay below the room's texture-window primitive at OT_LENGTH-1 so it
               is processed first (same rule as the room geometry). */
            if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

            uint8_t *col = p + 16;
            uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8);
            uint8_t g8= (uint8_t)(((int32_t)col[1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8);
            uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8);

            /* Pick the texture for this face: slot 0 = the room's plinth, slot 1
               = the grinder plate. Both sit at page-top (V 0-127), so the room's
               single 128 texture window serves them — but only the plinth is at
               U 0, hence the gr_u/gr_v remap on slot 1 (see the note above). */
            int      plate = (pi < GRINDER_PRIM_COUNT) && grinder_tex_map[pi];
            uint16_t tp    = plate ? TIM_TPAGE_GRINDER : plinth_tpage;
            uint16_t cl    = plate ? TIM_CLUT_GRINDER  : plinth_clut;

            if (is_quad) {
                if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
                POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
                setPolyFT4(poly);
                setRGB0(poly, r, g8, b);
                poly->tpage = tp;
                poly->clut  = cl;
                if (plate) {
                    poly->u0=gr_u(uv[0]); poly->v0=gr_v(uv[1]);
                    poly->u1=gr_u(uv[2]); poly->v1=gr_v(uv[3]);
                    poly->u2=gr_u(uv[4]); poly->v2=gr_v(uv[5]);
                    poly->u3=gr_u(uv[6]); poly->v3=gr_v(uv[7]);
                } else {
                    poly->u0=uv[0]; poly->v0=uv[1];
                    poly->u1=uv[2]; poly->v1=uv[3];
                    poly->u2=uv[4]; poly->v2=uv[5];
                    poly->u3=uv[6]; poly->v3=uv[7];
                }
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_FT4);
            } else {
                if (ctx->next_packet + sizeof(POLY_FT3) > buf_end) { p += stride; continue; }
                POLY_FT3 *poly = (POLY_FT3 *)ctx->next_packet;
                setPolyFT3(poly);
                setRGB0(poly, r, g8, b);
                poly->tpage = tp;
                poly->clut  = cl;
                if (plate) {
                    poly->u0=gr_u(uv[0]); poly->v0=gr_v(uv[1]);
                    poly->u1=gr_u(uv[2]); poly->v1=gr_v(uv[3]);
                    poly->u2=gr_u(uv[4]); poly->v2=gr_v(uv[5]);
                } else {
                    poly->u0=uv[0]; poly->v0=uv[1];
                    poly->u1=uv[2]; poly->v1=uv[3];
                    poly->u2=uv[4]; poly->v2=uv[5];
                }
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_FT3);
            }

            p += stride;
        }
    }

    /* Restore the camera view matrix for whatever the caller draws next. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
