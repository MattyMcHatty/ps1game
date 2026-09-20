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
#include "sconce.h"

/* Sconce — see sconce.h for what it is and why its collision comes out of its
   own mesh. */

typedef struct {
    GameState area;                        /* only draws/collides in this room */
    int32_t   x, y, z, rot_y;              /* base centre; world y = y+GROUND_FLOOR_Y */
    int32_t   min_x, max_x, min_z, max_z;  /* world AABB, baked at place time  */
    int32_t   flame_phase;                 /* per-instance offset into the flip */
    int32_t   light;                       /* 0..256 eased glow, see below     */
    int       active;
} Sconce;

static Sconce sconces[MAX_SCONCES];
static int    sconce_count = 0;

static SMD  *sconce_smd = NULL;
static void *sconce_buf = NULL;

/* The collision mesh, and it is the SAME mesh the draw uses: these three are
   read off sconce_smd's vertex array at load and are the only description of
   the prop's solid volume anywhere in the game. */
static int32_t sc_half_w = 0;   /* |vx| max — 60 for the model as authored */
static int32_t sc_half_d = 0;   /* |vz| max — 60                           */
static int32_t sc_height = 0;   /* -min(vy) — 180, base at model y=0       */

/* The prop's own texture. Deferred like the rest of the chapter's art: the
   header is read at startup, the pixels only when TEXBANK_CATACOMBS is
   selected at the catacomb mouth. */
static int sconce_tex = -1;

/* ---- THE FLAME -------------------------------------------------------------
   ONE billboarded quad standing on the bowl, flipping between the two frames
   in the BOTTOM half of sconce.tim. The mesh does not contain it: the model's
   top face is a black quad (the one untextured F4 in Sconce.smx, verts 28-31 at
   model y=-175 — the coal bed), and the fire is drawn over it as a sprite, the
   way every other thing in this game that has to look the same from all sides
   is drawn.

   THE TEXTURE IS ALREADY PAID FOR. The gold the mesh samples lives in the top
   half, v[0,63]; the two flame cells are v[64,127], u[0,63] and u[64,127], and
   nothing samples them until now. Their background is ALPHA in the PNG, which
   png_to_tim turns into a CLUT entry of 0x0000 — a texel the GPU skips — so the
   fire cuts out against the room with no blend state, no semi-transparency bit
   and no extra sorting. (Verified on the shipped .tim: 4751 transparent texels
   in the bottom half, exactly the PNG's count, and none in the top.) The cells'
   own borders are transparent too, so unlike the zombie's sprite these UVs need
   no one-texel inset to stop them bleeding.

   AND THE CELLS ARE AT Voff 64-127, NOT >=128, which is the only reason this
   needs no texture-window bracket: sconce.tim sits at VRAM y=0, so the room's
   128 window wraps mod-128 and v=127 is still v=127. Move the texture to a page
   bottom and the flame would sample the gold instead (tools/TEXTURING_NOTES.txt
   PART 5) — that is what the Voff column in tools/VRAM_MAP.txt is for. */
#define SCONCE_FLAME_HALF_W  44   /* quad is 88 wide; the art fills ~89% of it  */
#define SCONCE_FLAME_H      104   /* and ~82% of this, the rest being headroom  */
#define SCONCE_FLAME_RATE     5   /* frames per cell — a flicker, not a cycle   */
#define SCONCE_FLAME_OT_BIAS  8   /* 32 world units, clear of the bowl's own rim */

/* Free-running frame counter, advanced by sconces_update(). The flip is read
   off it rather than stored per instance so nothing has to be reset on a room
   re-entry or a save load; each instance only carries a PHASE, so two sconces
   side by side do not flicker in lockstep, which is what would make them read
   as one animation playing twice rather than as two fires. */
static int32_t sc_tick = 0;

void sconces_update(void) { sc_tick++; }

/* ---- THE GLOW --------------------------------------------------------------
   A sconce pushes the dark back around itself, and it does it through the
   room's OWN fog rather than through any shading of its own: it registers a
   render.h point light at its base, and every surface within SCONCE_LIGHT_
   RADIUS is then fogged and culled as if the camera were that much closer to
   it. Read render.h's "Point lights" note for why that is the whole trick -
   the short version is that "lit" here means "the shade this wall has when the
   player is standing next to it", which is a shade the room already defines.

   WHEN IT SWITCHES ON is the player's own view distance, which is what makes
   this feel like the room reacting rather than like a second lighting system:
   the target is full exactly while the sconce is inside g_fog_far, i.e. from the
   moment the player is close enough for any part of it to be drawn at all. The
   ramp then carries the glow in over SCONCE_LIGHT_RATE, and because the sconce
   sits at distance 0 from its own light it is the first thing the glow reaches
   - so walking into range lifts the whole prop out of the fog together, rather
   than fading it up edge-first the way an unlit prop at that distance fades.

   RADIUS 750, Manhattan like every distance in this fog. In the Catacombs
   Entry the pair at x=+-595, z=200 stand at the two ends of the lamashtu
   tablet, in a chamber the player is held inside x[-555,555] of, so 750
   reaches the chamber's centre line from either one with 155 to spare and the
   two together hold the tablet end of the room open at the base 1600 view
   distance, with the far end still dark. It is one number and it is meant to
   be moved.

   THE LIGHT IS FLAT IN Y, as all this game's fog is. That is free here because
   the Catacombs Entry's sconces stand on the chamber floor and the chamber is
   the only level within 750 of them; a sconce placed over one of the room's
   stacked walkable levels (719, 1240) would glow through the floor between. */
#define SCONCE_LIGHT_RADIUS  750
#define SCONCE_LIGHT_RATE     16   /* 0 -> 256 in 16 frames, ~a quarter second */

/* Advance every instance's glow one frame and hand the lights to the renderer.

   >>> CALL IT FROM THE ROOM'S DRAW, AFTER g_fog_near/g_fog_far ARE SET FOR THE
   FRAME AND BEFORE THE ROOM MESH IS QUEUED. <<< Both halves matter:
   render_light_add resolves its ramp against the fog band, and the room mesh
   is the main thing meant to be lit, so a call after it lights the room one
   frame late. It does NOT need to clear the list — draw_current_area() in
   src/main.c clears it ahead of every area's draw, which is what keeps these
   lights out of every other room. */
void sconces_publish_lights(void) {
    int i;
    for (i = 0; i < sconce_count; i++) {
        Sconce *s = &sconces[i];
        if (!s->active || s->area != current_area) continue;

        /* The gate is the RAW camera distance, never the lit one: asking the
           light whether the thing casting it is visible is a feedback loop
           that latches on and never lets go. */
        int32_t dcx = s->x - cam_x, dcz = s->z - cam_z;
        int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
        int32_t target = (dist <= g_fog_far) ? 256 : 0;

        if (s->light < target) {
            s->light += SCONCE_LIGHT_RATE;
            if (s->light > target) s->light = target;
        } else if (s->light > target) {
            s->light -= SCONCE_LIGHT_RATE;
            if (s->light < target) s->light = target;
        }

        render_light_add(s->x, s->z, SCONCE_LIGHT_RADIUS, s->light);
    }
}

/* The player's head, relative to cam_y — the same figure apply_collision_*
   uses for its own body span, so the vertical test below agrees with the walls'.
   Feet are cam_y + GROUND_FLOOR_Y. */
#define SCONCE_PLAYER_HEAD 30

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
void sconce_load_assets(void) {
    /* BANK: Chapter 3 and nothing else. Derived, not guessed — py
       tools/check_tex_banks.py walks the uploader call graph (this module is
       reached from catacombs_entry_upload_textures) and fails the build if this
       mask is short. */
    texmgr_set_bank(TEXBANK_CATACOMBS);

    sconce_buf = read_file("\\TEXCTCMB\\SCONCE.SMD;1");
    if (sconce_buf) sconce_smd = smdInitData(sconce_buf);

    /* MEASURE THE MESH. This is the whole of the prop's collision authoring:
       the box below is the model's real bounding volume, so it can never drift
       from what is drawn. Assumes the model is centred on its origin in plan
       and stands UP from it (-Y is up), which is how Sconce.smx is authored. */
    if (sconce_smd) {
        int i;
        for (i = 0; i < sconce_smd->n_verts; i++) {
            int32_t ax = sconce_smd->p_verts[i].vx; if (ax < 0) ax = -ax;
            int32_t az = sconce_smd->p_verts[i].vz; if (az < 0) az = -az;
            int32_t vy = sconce_smd->p_verts[i].vy;
            if (ax  > sc_half_w) sc_half_w = ax;
            if (az  > sc_half_d) sc_half_d = az;
            if (-vy > sc_height) sc_height = -vy;
        }
    }

    sconce_tex = texmgr_register("\\TEXCTCMB\\SCONCE.TIM;1");
}

/* Room entry: pure LoadImage out of the RAM copy area_bank_sync() has already
   read. No CD access, so it is safe inside main's STATE_LOADING. Called from
   catacombs_entry_upload_textures(). */
void sconce_upload_texture(void) {
    texmgr_upload(sconce_tex);
}

void sconces_clear(void) { sconce_count = 0; }

void sconce_place(GameState area, int32_t x, int32_t y, int32_t z, int32_t rot_y) {
    if (sconce_count >= MAX_SCONCES) return;
    Sconce *s = &sconces[sconce_count++];
    s->area  = area;
    s->x = x;  s->y = y;  s->z = z;
    s->rot_y = rot_y;
    s->active = 1;
    /* Half a cell apart for consecutive instances, so a pair flanking something
       burns out of step. */
    s->flame_phase = (sconce_count - 1) * (SCONCE_FLAME_RATE / 2 + 1);
    s->light = 0;   /* dark until the player is near enough to see it at all */

    /* World AABB = the axis-aligned bound of the rotated mesh footprint, corner
       by corner, exactly as the lever bakes its own. Computed once here rather
       than per frame, and from the measured half-extents above rather than from
       a constant somebody has to remember to update. */
    int32_t c = icos(rot_y), sn = isin(rot_y);
    const int32_t hw = sc_half_w, hd = sc_half_d;
    const int32_t lx[4] = { -hw,  hw,  hw, -hw };
    const int32_t lz[4] = { -hd, -hd,  hd,  hd };
    int k;
    for (k = 0; k < 4; k++) {
        /* Same handedness as the RotMatrix Y rotation the draw uses. */
        int32_t wx = x + ((lx[k] * c + lz[k] * sn) >> 12);
        int32_t wz = z + ((lz[k] * c - lx[k] * sn) >> 12);
        if (k == 0) {
            s->min_x = s->max_x = wx;
            s->min_z = s->max_z = wz;
        } else {
            if (wx < s->min_x) s->min_x = wx;
            if (wx > s->max_x) s->max_x = wx;
            if (wz < s->min_z) s->min_z = wz;
            if (wz > s->max_z) s->max_z = wz;
        }
    }
}

/* Player push-out against the baked box, Minkowski-expanded by the caller's
   radius and resolved along the shallowest axis — the dresser's scheme. Area-
   gated, so the shared collision routine calls it unconditionally. */
void sconces_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < sconce_count; i++) {
        Sconce *s = &sconces[i];
        if (!s->active || s->area != current_area) continue;

        /* Vertical gate, and it too comes out of the mesh: the prop is solid
           from its base up to sc_height (-Y is up), the player's body spans
           feet to head, and only an overlap blocks. That is what keeps a sconce
           from blocking a floor it does not stand on — the Catacombs Entry
           stacks its walkable levels 719 and 1240 below this one — and it needs
           no reach constant to tune, because the model states its own height. */
        int32_t solid_bot = s->y + GROUND_FLOOR_Y;      /* the base, on the floor */
        int32_t solid_top = solid_bot - sc_height;      /* the bowl's lip         */
        int32_t feet = py + GROUND_FLOOR_Y, head = py - SCONCE_PLAYER_HEAD;
        if (head >= solid_bot || feet <= solid_top) continue;

        int32_t min_x = s->min_x - radius, max_x = s->max_x + radius;
        int32_t min_z = s->min_z - radius, max_z = s->max_z + radius;
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
   UVs from the SMD (the model is one texture, so the tpage/clut are the same
   for every face and there is no tex map to keep in step) and ONE untextured
   quad, which is why this loop branches on the primitive's own texture bit
   rather than assuming FT like the dresser's does.

   The caller owns the 128 texture window; sconce.tim sits at Voff 0
   (tools/VRAM_MAP.txt), so the Catacombs Entry's existing window serves it and
   there is nothing to bracket. */
void sconces_draw(RenderContext *ctx) {
    if (!sconce_smd) return;

    MATRIX view;
    camera_build_view(&view);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint16_t tp = texmgr_tpage(sconce_tex);
    uint16_t cl = texmgr_clut(sconce_tex);

    int i;
    for (i = 0; i < sconce_count; i++) {
        Sconce *s = &sconces[i];
        if (!s->active || s->area != current_area) continue;

        /* Cull at the ROOM's fog-far, not at a constant of this module's own.
           The Catacombs Entry's view distance is a live number the Helluminator
           moves between 1600 and 3200, and g_fog_far is whatever the area draw
           set a few lines before calling us (render.h) — so the prop appears and
           fades exactly where the walls around it do, at every distance that
           number takes. This is the fix save_point.c needed for the same room. */
        int32_t dcx = s->x - cam_x, dcz = s->z - cam_z;
        int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
        /* ...AND THEN THROUGH ITS OWN LIGHT. A sconce is at distance 0 from
           the light it registered, so once sconces_publish_lights() has run
           its glow in, this collapses to g_fog_near and the prop is drawn in
           full colour at any range the room will draw it at all. That is the
           "walk close enough to see any of it and you see all of it" rule, and
           it falls out of the light rather than being a second rule: the glow
           ramps from the frame the RAW distance came inside g_fog_far, so the
           prop lifts out of the fog over the same quarter second the ground
           around it does. Walking away runs the ramp back down and the sconce
           fades WITH its own glow instead of popping at the cull line. */
        dist = render_light_dist(s->x, s->z, dist);
        if (dist > g_fog_far) continue;

        MATRIX m, combined;
        SVECTOR rr = {0, (int16_t)s->rot_y, 0, 0};
        RotMatrix(&rr, &m);
        VECTOR pos = {s->x, s->y + GROUND_FLOOR_Y, s->z};
        TransMatrix(&m, &pos);
        CompMatrixLV(&view, &m, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        int32_t fog_factor = render_fog_scale(dist);

        uint8_t *p = (uint8_t *)sconce_smd->p_prims;
        int pi;
        for (pi = 0; pi < sconce_smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt       = (SMD_PRI_TYPE *)p;
            uint8_t       stride   = pt->len;
            int           is_quad  = (pt->type >= 2);
            int           textured = pt->texture;

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &sconce_smd->p_verts[vi[0]];
            SVECTOR *v1 = &sconce_smd->p_verts[vi[1]];
            SVECTOR *v2 = &sconce_smd->p_verts[vi[2]];

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
                SVECTOR *v3 = &sconce_smd->p_verts[vi[3]];
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
               in the same slot as the floor and the wall it is set against, and
               a tie is a LOSS for the prop — the room queues its mesh before it
               calls us and addPrim pushes to the head of the bucket, so within
               one slot the later-added primitive is drawn FIRST and painted
               over. That is the clipping. Sorting at true scene depth lifts the
               prop 40 buckets, and a bucket is 4 world units (the arithmetic is
               worked out in src/catacomb_doors.c), so this is a 160-unit lift:
               more than the sconce's own 120 footprint, which is what it has to
               beat for no part of it to fall behind a surface it touches.

               It is the sprites' rule, for the sprites' reason — see the note
               in src/sml_med.c — and it costs no correctness, because a wall
               GENUINELY in front of a sconce is hundreds of buckets nearer and
               still occludes it. In this room it is stronger than that: the
               player is held inside x[-555,555] z>=195 by the 195 standoff, so
               neither the tablet wall at z=0 nor the side wall at x=750 can
               ever be between the camera and a sconce at all.

               The clamp is not decoration. Every other draw in the game only
               ADDS to otz, so +40 alone kept it clear of the menu's reserved
               range for free; dropping it does not, and a sconce seen from
               close up lands in single digits without SCENE_OT_MIN. */
            if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
            /* Stay below the room's texture-window primitive at OT_LENGTH-1 so
               it is processed first, the same rule the room geometry keeps. */
            if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

            /* Fog on the room's ramp, saturating to the Catacombs Entry's clear
               colour (CE_FOG_* in src/catacombs_entry.c). Hard-coded as every
               prop's is — there is no global for the colour, only for the
               distance — and this prop is a Chapter 3 fixture, so the room it
               would have to be told about is the room it is in. */
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

    /* Back to the plain view matrix — the flames below are built in WORLD space
       (they face the camera, so they cannot live in a model's local space), and
       so is whatever the caller draws next. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);

    /* ---- The flames, one pass, after every sconce body -------------------
       A camera-facing quad per instance, built the way the zombie's sprite is
       (src/zombie.c): the width axis is the camera's RIGHT vector, the height
       axis is world Y. That is a cylindrical billboard — it turns with the
       player but never tips — which is exactly "flat from every angle" for
       something standing on the floor, and it keeps the fire vertical when the
       camera pitches. It is projected through the same GTE as the room, so it
       is perspective-correct and sorts on real scene depth. */
    int32_t rx = icos(cam_rot), rz = -isin(cam_rot);

    for (i = 0; i < sconce_count; i++) {
        Sconce *s = &sconces[i];
        if (!s->active || s->area != current_area) continue;

        int32_t dcx = s->x - cam_x, dcz = s->z - cam_z;
        int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
        /* Lit distance, the SAME expression the body used above — the fire and
           the stand it sits in must reach the cull line together or a sconce
           at the edge of the glow is a floating flame. (The flame is still not
           fogged; only whether it is drawn at all comes from here.) */
        dist = render_light_dist(s->x, s->z, dist);
        if (dist > g_fog_far) continue;

        /* The foot of the flame is the TOP OF THE MESH, taken from the same
           measurement the collision box uses — so a re-exported sconce carries
           its fire up or down with it instead of leaving it floating. That is
           5 units above the black coal bed at model y=-175, which is nothing,
           and it puts the flame's base inside the lip rather than on its rim. */
        int32_t foot_y = s->y + GROUND_FLOOR_Y - sc_height;

        int16_t dwx = (int16_t)((SCONCE_FLAME_HALF_W * rx) >> 12);
        int16_t dwz = (int16_t)((SCONCE_FLAME_HALF_W * rz) >> 12);
        int16_t y_top = (int16_t)(foot_y - SCONCE_FLAME_H);
        int16_t y_bot = (int16_t)foot_y;

        SVECTOR v[4];
        v[0].vx = (int16_t)(s->x - dwx); v[0].vy = y_top; v[0].vz = (int16_t)(s->z - dwz); v[0].pad = 0;
        v[1].vx = (int16_t)(s->x + dwx); v[1].vy = y_top; v[1].vz = (int16_t)(s->z + dwz); v[1].pad = 0;
        v[2].vx = (int16_t)(s->x + dwx); v[2].vy = y_bot; v[2].vz = (int16_t)(s->z + dwz); v[2].pad = 0;
        v[3].vx = (int16_t)(s->x - dwx); v[3].vy = y_bot; v[3].vz = (int16_t)(s->z - dwz); v[3].pad = 0;

        DVECTOR fsv[4];
        int32_t fsz[4], fotz, fnclip;

        gte_ldv3(&v[0], &v[1], &v[2]);
        gte_rtpt();
        gte_stsxy3c(fsv);

        gte_nclip();
        gte_stopz(&fnclip);
        if (fnclip <= 0) continue;

        gte_ldv0(&v[3]);
        gte_rtps();
        gte_stsxy(&fsv[3]);

        gte_stsz4c(fsz);
        if (!fsz[0] || !fsz[1] || !fsz[2] || !fsz[3]) continue;

        gte_avsz4();
        gte_stotz(&fotz);
        /* Behind the camera — see the same reject in src/sml_med.c. */
        if (fotz <= 0) continue;
        /* Unbiased like the body above, then a little nearer still so the fire
           always wins against the bowl it is standing in: the quad's centre is
           the sconce's centre, so without this the rim in front of it could sort
           level and be drawn over it. */
        fotz -= SCONCE_FLAME_OT_BIAS;
        if (fotz < SCENE_OT_MIN)   fotz = SCENE_OT_MIN;
        if (fotz >= OT_LENGTH - 1) fotz = OT_LENGTH - 2;

        if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) continue;

        /* WHICH FRAME. The free-running tick plus this instance's phase, so the
           two sconces in the entry chamber alternate out of step. */
        int cell = (((sc_tick + s->flame_phase) / SCONCE_FLAME_RATE) & 1);
        uint8_t u_left  = cell ? 64 : 0;
        uint8_t u_right = cell ? 127 : 63;

        POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
        setPolyFT4(poly);
        /* NEUTRAL, AND DELIBERATELY NOT FOGGED. 128 is 1.0 on the PS1's colour
           modulation, so this is the texture as authored. Every other surface in
           this room fades toward the clear colour with distance; a flame is the
           one thing that must not, because it is the light doing the fading —
           a fire that dims with range reads as a painting of a fire. It is still
           culled with its sconce at g_fog_far, so it never hangs alone in the
           dark past where the room stops. */
        setRGB0(poly, 128, 128, 128);

        poly->x0 = fsv[0].vx; poly->y0 = fsv[0].vy;
        poly->x1 = fsv[1].vx; poly->y1 = fsv[1].vy;
        poly->x2 = fsv[3].vx; poly->y2 = fsv[3].vy;
        poly->x3 = fsv[2].vx; poly->y3 = fsv[2].vy;

        poly->u0 = u_left;  poly->v0 = 64;
        poly->u1 = u_right; poly->v1 = 64;
        poly->u2 = u_left;  poly->v2 = 127;
        poly->u3 = u_right; poly->v3 = 127;
        poly->tpage = tp;
        poly->clut  = cl;

        ctx->next_packet += sizeof(POLY_FT4);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[fotz], poly);
    }
}
