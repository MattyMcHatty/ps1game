#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "collision.h"      /* GROUND_FLOOR_Y */
#include "texmgr.h"
#include "title.h"          /* current_area gate: props only exist in the piano room */
#include "player.h"         /* game_flag: FLAG_PIANO_SOLVED latches both props */
#include "door.h"           /* door_draw_string_3d for the floating sign */
#include "btn_glyph.h"
#include "sound.h"          /* SFX_MCHNE: the bookcase's descent */
#include "piano_puzzle.h"   /* piano_puzzle_active: hide the sign during the shot */
#include "piano_props.h"

#define PPROP_COUNT        2
#define PPROP_PUSH_MARGIN 30   /* extra gap between player and prop edge (as tables) */

/* One static prop. Placed axis-aligned (rot_y is draw-only decoration; the
   collision box is a world AABB computed at place time), y is the standing
   reference: world translate = y + GROUND_FLOOR_Y -> model base on the floor
   (same convention as the dining tables / dresser). */
typedef struct {
    SMD     *smd;
    int      tex;                            /* texmgr id of the prop's texture */
    int32_t  x, y, z, rot_y;
    int32_t  min_x, max_x, min_z, max_z;     /* world-space collision footprint */
    int32_t  solid_h;                        /* solid height above the floor    */
    int      active;
} PianoProp;

static PianoProp props[PPROP_COUNT];   /* [0] piano, [1] bookcase */

static SMD  *piano_smd = NULL,    *bookcase_smd = NULL;
static void *piano_buf = NULL,    *bookcase_buf = NULL;
static int   piano_tex = -1,       bookcase_tex = -1;

/* The repaired keyboard. piano_keys_full.tim is built at the SAME VRAM rect and
   the SAME CLUT rect as piano_keys.tim, so swapping it in is a plain
   texmgr_upload over the top — every baked UV and the prop's tpage/clut stay
   valid and the draw loop needs no branch (cf. the fat door's cracked texture,
   which does have to override per-poly because it lives elsewhere in VRAM). */
static int   piano_full_tex = -1;

/* Which of the two the next upload (and the current VRAM contents) should be.
   Latched from FLAG_PIANO_SOLVED on room entry so a loaded save comes back with
   the repaired keys already fitted. */
static int piano_keys_repaired = 0;

/* ---- The sinking bookcase -------------------------------------------------
   Solving the puzzle drops the divider through the floor to open up the west
   half of the room. sink_off is added to the bookcase's y (down is +Y here), so
   it slides straight down; past PPROP_SINK_DIST the prop is retired outright —
   deactivated, so it stops drawing AND stops colliding.

   The descent is timed to the machinery sound rather than the other way round:
   mchne.vag is 2.80 s, and it plays TWICE back to back, so the travel lasts
   exactly two clip lengths and the second trigger lands on the frame the first
   one runs out. Re-encode that VAG at a different length and this constant has
   to move with it or the audio and the movement come apart.

   sink_off is recomputed from the frame counter each tick (not accumulated by a
   rounded per-frame step) so the travel really does finish on frame
   PPROP_SINK_FRAMES instead of whenever integer rounding gets there. */
#define PPROP_SINK_DIST      620  /* solid_h 520 + clearance under the boards */
#define PPROP_SINK_SFX      168   /* mchne.vag: 2.80 s at 60 fps              */
#define PPROP_SINK_FRAMES   (PPROP_SINK_SFX * 2)   /* the clip, twice         */

static int32_t sink_off   = 0;
static int     sinking    = 0;
static int32_t sink_t     = 0;   /* frames into the descent */

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

/* Startup: load geometry and register both textures with the texture manager
   (RAM-resident for a pure-LoadImage upload on each piano-room entry). */
void piano_props_load_assets(void) {
    piano_buf = read_file("\\TEX\\PIANO.SMD;1");
    if (piano_buf) piano_smd = smdInitData(piano_buf);
    bookcase_buf = read_file("\\TEX\\BOOKCSE.SMD;1");
    if (bookcase_buf) bookcase_smd = smdInitData(bookcase_buf);

    piano_tex      = texmgr_register("\\TEX\\PIANOKEY.TIM;1");
    piano_full_tex = texmgr_register("\\TEX\\PIANOFUL.TIM;1");
    bookcase_tex   = texmgr_register("\\TEX\\BOOKSHLF.TIM;1");
}

/* The keyboard art the piano should be showing right now. Falls back to the
   broken one if the repaired TIM failed to register (same defensive fallback
   the fat door uses for its cracked texture). */
static int piano_keys_tex(void) {
    return (piano_keys_repaired && piano_full_tex >= 0) ? piano_full_tex
                                                        : piano_tex;
}

void piano_props_upload_textures(void) {
    texmgr_upload(piano_keys_tex());
    texmgr_upload(bookcase_tex);
}

/* Bookcase art ONLY (stn_stl slot, x320 y0). The library draws bookshelf but
   NOT piano_keys, and piano_keys shares the kchn_tile slot (x384 y0) with the
   cncrte the library does draw — so the library must not take the pair. */
void piano_props_upload_bookcase_texture(void) {
    texmgr_upload(bookcase_tex);
}

/* Position both props in the piano room (room x[-2301,0], z[-740,974], floor
   world y=0 -> standing reference -149). Called from piano_room_init. */
void piano_props_place(void) {
    /* Latch the keyboard state from the save flag BEFORE the upload that
       follows on room entry, so a loaded game walks in to repaired keys and a
       bookcase that is already gone. */
    piano_keys_repaired = game_flag(FLAG_PIANO_SOLVED);
    sinking  = 0;
    sink_t   = piano_keys_repaired ? PPROP_SINK_FRAMES : 0;
    sink_off = piano_keys_repaired ? PPROP_SINK_DIST   : 0;

    /* Piano against the north (+Z) wall on the door (east) side, keys facing
       south into the room (the model's keyboard faces -Z at rot 0). Model
       footprint x +/-195, z +/-50. */
    props[0].smd  = piano_smd;
    props[0].tex  = piano_keys_tex();
    props[0].x    = -420;  props[0].y = -149;  props[0].z = 850;
    props[0].rot_y = 0;
    props[0].min_x = -420 - 195;  props[0].max_x = -420 + 195;
    props[0].min_z =  850 -  50;  props[0].max_z =  850 +  50;
    props[0].solid_h = 152;
    props[0].active  = 1;

    /* Bookcase: a wall-to-wall divider across the room's halfway point. The
       model runs along Z (x +/-25, z -775..+1025, centre z +125), so origin
       z=-8 centres it on the room's z span; the ends bury into the walls. */
    props[1].smd  = bookcase_smd;
    props[1].tex  = bookcase_tex;
    props[1].x    = -1150;  props[1].y = -149;  props[1].z = -8;
    props[1].rot_y = 0;
    /* Collision box widened past the model's 25-unit half-width so the player
       stands off the shelves a little further. */
    props[1].min_x = -1150 - 90;  props[1].max_x = -1150 + 90;
    props[1].min_z =    -8 - 775; props[1].max_z =    -8 + 1025;
    props[1].solid_h = 520;
    props[1].active  = !piano_keys_repaired;   /* solved: already through the floor */
}

/* ---- Puzzle hooks (called by piano_puzzle.c) ------------------------------ */

int32_t piano_prop_x(void) { return props[0].x; }
int32_t piano_prop_z(void) { return props[0].z; }

/* Fit the missing key. The upload is a pure LoadImage out of the texmgr's RAM
   copy — no CD access — but it still touches VRAM the GPU may be reading, so
   idle it first exactly as main's room-transition path does. */
void piano_props_repair_keys(void) {
    if (piano_keys_repaired) return;
    piano_keys_repaired = 1;
    props[0].tex = piano_keys_tex();
    DrawSync(0);
    texmgr_upload(props[0].tex);
}

void piano_props_bookcase_sink_start(void) {
    if (!props[1].active) return;
    sinking = 1;
    sink_t  = 0;
    sound_play(SFX_MCHNE);   /* first of two; the second is fired below */
}

int piano_props_bookcase_sinking(void) { return sinking; }

/* One frame of the descent. Returns 1 on the frame it finishes (and every frame
   after), so the caller can hold on the empty floor and then eject. */
int piano_props_bookcase_update(void) {
    if (!sinking) return sink_off >= PPROP_SINK_DIST;

    sink_t++;
    /* Second play, on the frame the first clip runs out — back to back, so the
       grind reads as one continuous machine rather than two hits. */
    if (sink_t == PPROP_SINK_SFX) sound_play(SFX_MCHNE);

    sink_off = (PPROP_SINK_DIST * sink_t) / PPROP_SINK_FRAMES;
    if (sink_t >= PPROP_SINK_FRAMES) {
        sink_off        = PPROP_SINK_DIST;
        sinking         = 0;
        props[1].active = 0;   /* gone for good: stops drawing AND colliding */
        return 1;
    }
    return 0;
}

/* ---- The "Press O to examine" sign -----------------------------------------
   A floating sign slightly above the piano (XY plane, facing -Z like the stove
   sign). The Circle press it advertises is handled by piano_puzzle.c, which
   owns the interaction from here on — this module only draws the invitation. */
#define PIANO_TEXT_Y         (-190)  /* just above the piano top (-152)        */
#define PIANO_TEXT_Z_OFF      (-45)  /* sign plane just in front of the piano  */
#define PIANO_TEXT_RADIUS     1500
#define PIANO_TEXT_FADE_NEAR  1000
#define PIANO_TEXT_PIXEL         2   /* small prop sign, as the stove's        */

/* Floating examine sign, drawn with the room's view matrix active. The XY
   plane reads along X and faces along Z; the player approaches from -Z, same
   side as the stove sign, so mirror=0. door_draw_string_3d centres the reading
   axis on world_x after adding 200, so pass x - 200 (as the stove does). */
void piano_props_text(RenderContext *ctx) {
    PianoProp *p = &props[0];
    if (!p->active) return;
    /* Nothing left to examine once the key is in, and the puzzle's own fixed
       camera should not be looking at a floating prompt. */
    if (piano_keys_repaired || piano_puzzle_active()) return;

    int32_t dx = cam_x - p->x;
    int32_t dz = cam_z - p->z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= PIANO_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > PIANO_TEXT_FADE_NEAR) {
        int range = PIANO_TEXT_RADIUS - PIANO_TEXT_FADE_NEAR;
        int prog  = xz - PIANO_TEXT_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to examine",
                        p->x - 200, PIANO_TEXT_Y, p->z + PIANO_TEXT_Z_OFF,
                        50, 255, 50, fade, 0, TEXT_PLANE_XY, PIANO_TEXT_PIXEL);
}

/* Player push-out (dresser-style Minkowski AABB). Gated to the piano room so
   the shared reception collision routine can call this unconditionally. */
void piano_props_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    (void)py;   /* single flat floor — no vertical gating needed */
    if (current_area != STATE_PIANO_ROOM) return;
    int i;
    for (i = 0; i < PPROP_COUNT; i++) {
        PianoProp *p = &props[i];
        if (!p->active) continue;

        int32_t min_x = p->min_x - radius - PPROP_PUSH_MARGIN;
        int32_t max_x = p->max_x + radius + PPROP_PUSH_MARGIN;
        int32_t min_z = p->min_z - radius - PPROP_PUSH_MARGIN;
        int32_t max_z = p->max_z + radius + PPROP_PUSH_MARGIN;

        if (*px <= min_x || *px >= max_x) continue;
        if (*pz <= min_z || *pz >= max_z) continue;

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
}

int piano_props_point_solid(int32_t x, int32_t y, int32_t z, int32_t slack) {
    if (current_area != STATE_PIANO_ROOM) return 0;
    int i;
    for (i = 0; i < PPROP_COUNT; i++) {
        PianoProp *p = &props[i];
        if (!p->active) continue;
        /* Vertical span in world Y: base rests on the floor at
           (p->y + GROUND_FLOOR_Y), body reaches solid_h above it. */
        int32_t base = p->y + GROUND_FLOOR_Y;
        if (y < base - p->solid_h || y > base) continue;
        if (x < p->min_x - slack || x > p->max_x + slack) continue;
        if (z < p->min_z - slack || z > p->max_z + slack) continue;
        return 1;
    }
    return 0;
}

/* Render both props with the piano room's fog and texture window. Textured
   prims (pt->texture, fatdoor-style flag) use the prop's own texture; the rest
   draw flat-shaded. Fog is computed per prim (the bookcase is 1800 long, so a
   single per-prop distance would fog its far end wrongly). Restores the camera
   view matrix before returning. */
void piano_props_draw(RenderContext *ctx) {
    MATRIX view;
    camera_build_view(&view);   /* pitch-aware: the puzzle's fixed shots tilt */

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    int i;
    for (i = 0; i < PPROP_COUNT; i++) {
        PianoProp *pr = &props[i];
        if (!pr->active || !pr->smd) continue;

        /* Combine the view matrix with this prop's world transform. */
        MATRIX pm, combined;
        SVECTOR prot = {0, pr->rot_y, 0, 0};
        RotMatrix(&prot, &pm);
        /* Down is +Y, so the bookcase's sink offset just adds to its base. */
        int32_t base_y = pr->y + GROUND_FLOOR_Y + (i == 1 ? sink_off : 0);
        VECTOR pos = {pr->x, base_y, pr->z};
        TransMatrix(&pm, &pos);
        CompMatrixLV(&view, &pm, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        int32_t rc = icos(pr->rot_y), rs = isin(pr->rot_y);

        uint8_t *p = (uint8_t *)pr->smd->p_prims;
        int pi;
        for (pi = 0; pi < pr->smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt     = (SMD_PRI_TYPE *)p;
            uint8_t       stride = pt->len;
            int           is_quad = (pt->type >= 2);

            uint16_t *vi = (uint16_t *)(p + 4);
            /* Below-the-floor cull for the sinking bookcase. The floor is world
               y=0 and the props are sorted a little further back than the room
               (otz += 40), so a submerged prim would normally be hidden by the
               boards anyway — but the OT is a painter's sort with no depth
               buffer, and at this camera the prim and the board over it land
               within a bucket or two of each other. Dropping prims whose HIGHEST
               vertex has already passed the floor line takes that coin flip out
               of it. Only the sinking bookcase can trip this (sink_off is 0
               otherwise), so nothing standing is ever affected. */
            SVECTOR *v0 = &pr->smd->p_verts[vi[0]];
            SVECTOR *v1 = &pr->smd->p_verts[vi[1]];
            SVECTOR *v2 = &pr->smd->p_verts[vi[2]];

            if (sink_off && i == 1) {
                int32_t top = v0->vy;                    /* most negative = highest */
                if (v1->vy < top) top = v1->vy;
                if (v2->vy < top) top = v2->vy;
                if (is_quad) {
                    int32_t y3 = pr->smd->p_verts[vi[3]].vy;
                    if (y3 < top) top = y3;
                }
                if (base_y + top >= 0) { p += stride; continue; }
            }

            /* Per-prim distance cull + fog at the room's budget, from the prim
               centre rotated/translated into world space. */
            int32_t mcx = ((int32_t)v0->vx + v2->vx) / 2;
            int32_t mcz = ((int32_t)v0->vz + v2->vz) / 2;
            int32_t wcx = pr->x + ((mcx * rc + mcz * rs) >> 12);
            int32_t wcz = pr->z + ((mcz * rc - mcx * rs) >> 12);
            int32_t dist = (wcx > cam_x ? wcx - cam_x : cam_x - wcx) +
                           (wcz > cam_z ? wcz - cam_z : cam_z - wcz);
            if (dist > 1500) { p += stride; continue; }

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
                v3 = &pr->smd->p_verts[vi[3]];
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
            /* Stay below the room's texture-window primitive at OT_LENGTH-1. */
            if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

            int32_t fog_start = 350, fog_end = 1500;   /* matches the room */
            int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
            int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

            uint8_t *col = p + 16;
            uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8);
            uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8);
            uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8);

            int textured = pt->texture != 0;
            uint16_t tp = 0, cl = 0;
            if (textured) {
                tp = texmgr_tpage(pr->tex);
                cl = texmgr_clut(pr->tex);
            }

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

    /* Restore the camera view matrix for whatever the caller draws next. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
