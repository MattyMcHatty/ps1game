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
#include "crib.h"

/* Crib — see crib.h for what it is, for why its collision comes out of its own
   mesh, and for which parts of the shape below are here for the movement that is
   coming rather than for the static prop that shipped. */

typedef struct {
    GameState area;                        /* only draws/collides in this room  */
    int32_t   x, y, z, rot_y;              /* centre in plan; world y = y+GROUND_FLOOR_Y */
    int32_t   min_x, max_x, min_z, max_z;  /* world AABB, baked at place time   */
    int       active;
} Crib;

static Crib cribs[MAX_CRIBS];
static int  crib_count = 0;

static SMD  *crib_smd = NULL;
static void *crib_buf = NULL;

/* The collision mesh, and it is the SAME mesh the draw uses: these six are read
   off crib_smd's vertex array at load and are the only description of the prop's
   solid volume anywhere in the game. MODEL SPACE, and signed — see crib.h for
   why they are measured rather than assumed symmetric even though this model
   happens to be. */
static int32_t cr_min_x = 0, cr_max_x = 0;   /* -175 .. 175  as authored */
static int32_t cr_min_z = 0, cr_max_z = 0;   /* -100 .. 100             */
static int32_t cr_min_y = 0, cr_max_y = 0;   /* -195 ..   0, -Y is up   */

/* The prop's own texture. Deferred like the rest of the chapter's art: the
   header is read at startup, the pixels only when TEXBANK_CATACOMBS is selected
   at the catacomb mouth. */
static int crib_tex = -1;

/* The player's head, relative to cam_y — the same figure apply_collision_*
   uses for its own body span, so the vertical test below agrees with the walls'.
   Feet are cam_y + GROUND_FLOOR_Y. */
#define CR_PLAYER_HEAD 30

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

/* Startup. The CD read is the geometry; the texture is a registration only —
   one sector of TIM header, no pixels — so Chapters 1 and 2 pay nothing for it
   (src/texmgr.h). */
void crib_load_assets(void) {
    /* BANK: Chapter 3 and nothing else. Derived, not guessed — py
       tools/check_tex_banks.py walks the uploader call graph (this module is
       reached from room_of_arms_upload_textures) and fails the build if this
       mask is short. */
    texmgr_set_bank(TEXBANK_CATACOMBS);

    crib_buf = read_file("\\TEXCTCMB\\CRIB.SMD;1");
    if (crib_buf) crib_smd = smdInitData(crib_buf);

    /* MEASURE THE MESH. This is the whole of the prop's collision authoring:
       the box below is the model's real bounding volume, so it can never drift
       from what is drawn. No symmetry is assumed — see crib.h. */
    if (crib_smd && crib_smd->n_verts > 0) {
        int i;
        cr_min_x = cr_max_x = crib_smd->p_verts[0].vx;
        cr_min_y = cr_max_y = crib_smd->p_verts[0].vy;
        cr_min_z = cr_max_z = crib_smd->p_verts[0].vz;
        for (i = 1; i < crib_smd->n_verts; i++) {
            int32_t vx = crib_smd->p_verts[i].vx;
            int32_t vy = crib_smd->p_verts[i].vy;
            int32_t vz = crib_smd->p_verts[i].vz;
            if (vx < cr_min_x) cr_min_x = vx;
            if (vx > cr_max_x) cr_max_x = vx;
            if (vy < cr_min_y) cr_min_y = vy;
            if (vy > cr_max_y) cr_max_y = vy;
            if (vz < cr_min_z) cr_min_z = vz;
            if (vz > cr_max_z) cr_max_z = vz;
        }
    }

    crib_tex = texmgr_register("\\TEXCTCMB\\CRIB.TIM;1");
}

/* Room entry: pure LoadImage out of the RAM copy area_bank_sync() has already
   read. No CD access, so it is safe inside main's STATE_LOADING. Called from
   room_of_arms_upload_textures(). */
void crib_upload_texture(void) {
    texmgr_upload(crib_tex);
}

void cribs_clear(void) { crib_count = 0; }

void crib_place(GameState area, int32_t x, int32_t y, int32_t z, int32_t rot_y) {
    if (crib_count >= MAX_CRIBS) return;
    Crib *c = &cribs[crib_count++];
    c->area  = area;
    c->x = x;  c->y = y;  c->z = z;
    c->rot_y = rot_y;
    c->active = 1;

    /* World AABB = the axis-aligned bound of the rotated mesh footprint, corner
       by corner, exactly as the lever, the sconce and the oil dispenser bake
       theirs. Computed once here rather than per frame, and from the MEASURED
       min/max above rather than from a constant somebody has to remember to
       update. The four corners are written out in full instead of as ±hw/±hd so
       the arithmetic still holds for a re-export whose footprint is NOT centred
       on the origin it rotates about.

       THIS IS THE FUNCTION THE MOVEMENT WILL CALL AGAIN. A crib that rocks
       moves rot_y (or x/z) and re-derives its box from here; nothing else in
       the module holds a world coordinate. */
    int32_t cs = icos(rot_y), sn = isin(rot_y);
    const int32_t lx[4] = { cr_min_x, cr_max_x, cr_max_x, cr_min_x };
    const int32_t lz[4] = { cr_min_z, cr_min_z, cr_max_z, cr_max_z };
    int k;
    for (k = 0; k < 4; k++) {
        /* Same handedness as the RotMatrix Y rotation the draw uses. */
        int32_t wx = x + ((lx[k] * cs + lz[k] * sn) >> 12);
        int32_t wz = z + ((lz[k] * cs - lx[k] * sn) >> 12);
        if (k == 0) {
            c->min_x = c->max_x = wx;
            c->min_z = c->max_z = wz;
        } else {
            if (wx < c->min_x) c->min_x = wx;
            if (wx > c->max_x) c->max_x = wx;
            if (wz < c->min_z) c->min_z = wz;
            if (wz > c->max_z) c->max_z = wz;
        }
    }
}

/* Player push-out against the baked box, Minkowski-expanded by the caller's
   radius and resolved along the shallowest axis — the dresser's scheme. Area-
   gated, so the shared collision routine calls it unconditionally. */
void cribs_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < crib_count; i++) {
        Crib *c = &cribs[i];
        if (!c->active || c->area != current_area) continue;

        /* Vertical gate, and it too comes out of the mesh. -Y is up, so the
           cot's UNDERSIDE is cr_max_y (the floor it stands on) and its top rail
           is cr_min_y, both offsets from the floor the prop was placed on; the
           player's body spans feet to head, and only an overlap blocks. This
           prop sits ON its own floor, so unlike the oil dispenser's the test is
           very nearly a formality here — it earns its keep only in a multi-storey
           room, where it is what stops a cot on the lower floor from blocking
           the walkway above it. */
        int32_t floor_y   = c->y + GROUND_FLOOR_Y;
        int32_t solid_bot = floor_y + cr_max_y;   /* the feet of the cot   */
        int32_t solid_top = floor_y + cr_min_y;   /* its top rail          */
        int32_t feet = py + GROUND_FLOOR_Y, head = py - CR_PLAYER_HEAD;
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

/* Render every instance in the current area. Textured-prim path with per-poly
   UVs from the SMD (the model is one texture, so the tpage/clut are the same for
   every face and there is no tex map to keep in step). The untextured branches
   are kept because the loop branches on the primitive's own texture bit, which
   costs nothing and survives a re-export that leaves a flat face behind.

   THE CALLER OWNS THE TEXTURE WINDOW and this prop is happy either way, which is
   true of the 60-poly export and was not true of the 100-poly one it replaced:
   crib.tim sits at Voff 0 (x576 y0, tools/VRAM_MAP.txt) so there is nothing to
   bracket, and every UV is now inside u[2,127] v[2,124] so nothing needs wrapping
   either. The Room of Arms sets a 128 window for its own mesh art and this prop
   draws correctly under it. See crib.h — a re-UV past 127 makes that window
   load-bearing again.

   The whole matrix is rebuilt from the instance's fields every frame, which is
   what will make the movement a write to rot_y and nothing more (crib.h). */
void cribs_draw(RenderContext *ctx) {
    if (!crib_smd) return;

    MATRIX view;
    camera_build_view(&view);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint16_t tp = texmgr_tpage(crib_tex);
    uint16_t cl = texmgr_clut(crib_tex);

    int i;
    for (i = 0; i < crib_count; i++) {
        Crib *c = &cribs[i];
        if (!c->active || c->area != current_area) continue;

        /* Cull at the ROOM's fog-far, not at a constant of this module's own —
           the Room of Arms' view distance is a live number the Helluminator
           moves between 1600 and 3200, and g_fog_far is whatever the area draw
           set a few lines before calling us (render.h). Then through the room's
           lights, so a cot standing inside a sconce's reach appears and fades
           exactly where the wall behind it does. It registers no light of its
           own. */
        int32_t dcx = c->x - cam_x, dcz = c->z - cam_z;
        int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
        dist = render_light_dist(c->x, c->z, dist);
        if (dist > g_fog_far) continue;

        MATRIX m, combined;
        SVECTOR rr = {0, (int16_t)c->rot_y, 0, 0};
        RotMatrix(&rr, &m);
        VECTOR pos = {c->x, c->y + GROUND_FLOOR_Y, c->z};
        TransMatrix(&m, &pos);
        CompMatrixLV(&view, &m, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        int32_t fog_factor = render_fog_scale(dist);

        uint8_t *p = (uint8_t *)crib_smd->p_prims;
        int pi;
        for (pi = 0; pi < crib_smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt       = (SMD_PRI_TYPE *)p;
            uint8_t       stride   = pt->len;
            int           is_quad  = (pt->type >= 2);
            int           textured = pt->texture;

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &crib_smd->p_verts[vi[0]];
            SVECTOR *v1 = &crib_smd->p_verts[vi[1]];
            SVECTOR *v2 = &crib_smd->p_verts[vi[2]];

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
                SVECTOR *v3 = &crib_smd->p_verts[vi[3]];
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
            /* >>> NO +40. <<< The room mesh biases every one of its polys 40
               buckets deeper into the OT; a prop that took the same bias sorted
               in the same slot as the wall it is set against, and a tie is a
               LOSS for the prop — the room queues its mesh before it calls us
               and addPrim pushes to the head of the bucket, so within one slot
               the later-added primitive is drawn FIRST and painted over. Sorting
               at true scene depth lifts the prop 40 buckets, and a bucket is 4
               world units (the arithmetic is in src/catacomb_doors.c), so this is
               a 160-unit lift — comfortably more than this prop's own 200-unit
               depth needs at the 30-unit standoff it is placed at, which is what
               it has to beat for no part of it to fall behind the wall it is set
               against. The sconce's and the oil dispenser's notes spell the same
               rule out at length.

               It costs no correctness: a wall GENUINELY in front of the cot is
               hundreds of buckets nearer and still occludes it. The clamp is not
               decoration — a prop seen from close up lands in single digits
               without SCENE_OT_MIN. */
            if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
            /* Stay below the room's texture-window primitive at OT_LENGTH-1 so
               it is processed first, the same rule the room geometry keeps. */
            if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

            /* Fog on the room's ramp, saturating to the chapter's clear colour
               (7,6,9 — ROA_FOG_* in src/room_of_arms.c, and the same three
               numbers in all five Catacombs rooms). Hard-coded as every prop's
               is: there is no global for the colour, only for the distance, and
               this prop is a Chapter 3 fixture. */
            uint8_t *col = p + 16;
            uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + 7 * (256 - fog_factor)) >> 8);
            uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + 6 * (256 - fog_factor)) >> 8);
            uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + 9 * (256 - fog_factor)) >> 8);

            if (is_quad && textured) {
                if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
                uint8_t *uv = p + 20;
                POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
                setPolyFT4(poly);
                setRGB0(poly, r, g, b);
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

    /* Back to the plain view matrix — whatever the caller draws next is in world
       space and must not inherit the last instance's model transform. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
