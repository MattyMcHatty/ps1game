#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "room_arena.h"
#include "camera.h"
#include "delivery_area.h"
#include "collision.h"
#include "texmgr.h"

#include "vampire.h"
#include "player.h"
#include "crucifaxe.h"
#include "sml_med.h"
#include "particles.h"
#include "crate.h"
#include "key.h"
#include "door.h"
#include "demondog.h"
#include "garden_stairs.h"    /* garden_stairs_upload_grss_gs */
#include "delivery_intro.h"   /* the New Game arrival sequence's fade overlay */
#include "tim_slots.h"
#include "delivery_area_tex_map.h"

static SMD  *room_smd  = NULL;
static void *room_buff = NULL;

/* ---- Per-room textures -----------------------------------------------------
   The remodelled yard draws SEVEN textures where the old one drew four. Three of
   the new set (grss, chnlnk, trees) already exist on the disc for other rooms —
   but at VRAM pages this room now fills with something else it draws at the same
   time. grss lives at x768, which delivery fills with brick_wall; chnlnk and
   trees both live at x640, which delivery fills with gravel. So all three are
   drawn from CLONES at pages this room does not otherwise touch, exactly as the
   Garden Stairs does for the same two textures (tools/retarget_tim.py):

     grss   -> grss_gs   (x320 y0)   already on the disc for the Garden Stairs;
                                     shares the stn_stl / strs / bookshelf page,
                                     which every consumer re-uploads on its own
                                     entry, so this adds no restore obligation.
                                     The Garden Stairs owns the RAM copy — we
                                     call its narrow upload rather than
                                     registering a second one, because texmgr has
                                     a hard TEXMGR_MAX and overruns fail SILENTLY.
     trees  -> trees_dl  (x960 y256) NEW, and the last free page-aligned Voff-0
                                     page in VRAM. Nothing else lives there, so
                                     it is uploaded once at startup, never
                                     restored.
     chnlnk -> chnlnk_dl (x384 y0)   NEW, parked on the clsd_drwr / cncrte /
                                     kchn_tile / piano_keys / xt_dr_outr page.
                                     The room draws none of those and each of
                                     them is re-uploaded by its own room on
                                     entry, so this adds no restore obligation
                                     beyond our own.

   EVERY ONE OF THE SEVEN MUST BE PAGE-ALIGNED AND AT Voff 0. The room's 128
   texture window masks U and V to 0-127 of the tpage, so there is no way to
   reach the upper half of a page or the lower half of a 256-row one. chnlnk_dl
   originally sat at x992 — the upper half of trees_dl's page — which worked
   only while this room hand-computed its UVs, and had to move when it started
   using the window like everywhere else.

   Slots (must match NAME_TO_SLOT in gen_delivery_area_tex_map.py):
     [0]=gravel [1]=rusty_fence [2]=brick_wall [3]=double_door
     [4]=grss   [5]=chnlnk      [6]=trees                                     */
#define DELIVERY_TEX_COUNT 7
static uint16_t tex_tpage[DELIVERY_TEX_COUNT];
static uint16_t tex_clut[DELIVERY_TEX_COUNT];

static void *load_file_from_cd(const char *filename, int *size_out) {
    CdlFILE file;
    if (!CdSearchFile(&file, filename)) {
        if (size_out) *size_out = 0;
        return NULL;
    }
    int sectors = (file.size + 2047) / 2048;
    void *buff = malloc(sectors * 2048);
    if (!buff) {
        if (size_out) *size_out = 0;
        return NULL;
    }
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buff, CdlModeSpeed);
    CdReadSync(0, NULL);
    if (size_out) *size_out = file.size;
    return buff;
}

static void load_tim_to_vram(const char *filename, int slot) {
    void *buf = load_file_from_cd(filename, NULL);
    if (!buf) return;

    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);

    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);

    if (tim.mode & 0x8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
    }

    tex_tpage[slot] = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);
    tex_clut[slot]  = getClut(tim.crect->x, tim.crect->y);

    free(buf);
}

/* The conservatory streams its four textures over these VRAM slots
   (gravel/fence/brick <- trees/upstairs/grss, double_door <- con_tile), so
   they must be re-uploadable on return here with no mid-game CD read. The
   texture manager keeps them RAM-resident; ids captured at startup. */
#define DELIVERY_SHARED_TEX 5
static int shared_id[DELIVERY_SHARED_TEX];
static const char *shared_tex_file[DELIVERY_SHARED_TEX] = {
    "\\GRAVEL.TIM;1",
    "\\FENCE.TIM;1",
    "\\BRIKWLL.TIM;1",
    "\\DBLDOOR.TIM;1",
    "\\TEX\\CHNLNKDL.TIM;1",   /* our chnlnk clone, on the clsd_drwr page */
};

/* Re-upload the shared textures from their resident RAM copies. Pure
   LoadImage, no CD access — safe mid-game once the caller has idled the GPU
   (DrawSync), as main's loading/title paths do.

   trees_dl is deliberately absent: it owns its VRAM page outright, so the
   startup upload is the only one it ever needs. */
void delivery_restore_textures(void) {
    for (int i = 0; i < DELIVERY_SHARED_TEX; i++)
        texmgr_upload(shared_id[i]);
    /* grss, from the Garden Stairs' clone. That page is shared with stn_stl /
       strs / bookshelf, all of whose rooms re-upload on their own entry. */
    garden_stairs_upload_grss_gs();
    /* key.tim, which the copper pot streams over — this is the only room that
       draws a key, and a new game after a session that reached the conservatory
       would otherwise show the pot's texels in the key's palette. Callers that
       own the pot re-upload it after this, so the pot still wins its slot. */
    keys_upload_texture();
}

/* brick_wall ONLY (index 2 above). The Garden Stairs draws brick_wall but keeps
   chnlnk in the gravel slot, so it cannot take the whole set — see the note in
   delivery_area.h. */
void delivery_upload_brick_wall(void) { texmgr_upload(shared_id[2]); }

void delivery_area_init(void) {
    CdInit();
    scSetClipRect(0, 0, SCREEN_XRES, SCREEN_YRES);

    load_tim_to_vram("\\GRAVEL.TIM;1",  0);
    load_tim_to_vram("\\FENCE.TIM;1",   1);
    load_tim_to_vram("\\BRIKWLL.TIM;1", 2);
    load_tim_to_vram("\\DBLDOOR.TIM;1", 3);

    /* trees_dl owns its VRAM page, so this single startup upload is the only one
       it needs — no entry-time restore. chnlnk_dl shares the clsd_drwr page, so
       it is uploaded here AND kept RAM-resident below for the restores. */
    load_tim_to_vram("\\TEX\\TREESDL.TIM;1",  6);
    load_tim_to_vram("\\TEX\\CHNLNKDL.TIM;1", 5);

    /* grss comes from the Garden Stairs' grss_gs clone. Its pixels are uploaded
       on every delivery entry by delivery_restore_textures(); the tpage/clut are
       compile-time constants, so nothing is read off the CD for it here.
       (garden_stairs_load_assets() has not run yet at this point in main's
       startup — the first upload happens on the title-exit path.) */
    TIM_SLOT(4, GRSSGS);

    /* Startup-only (CD reads): keep RAM copies for the mid-game restores. */
    for (int i = 0; i < DELIVERY_SHARED_TEX; i++)
        shared_id[i] = texmgr_register(shared_tex_file[i]);

}

/* Load this room's geometry into the shared arena. Called on ENTRY, not at
   startup, like every other room — but delivery is the one room reached WITHOUT
   a STATE_LOADING pass (the title screen drops straight into it), so main.c
   calls this from the title-exit path as well as from STATE_LOADING. See
   src/room_arena.h. */
void delivery_load_geometry(void) {
    room_buff = room_arena_load("\\DELIVERY.SMD");
    room_smd  = room_buff ? smdInitData(room_buff) : NULL;
}

/* SMD FT4 layout, stride=32:
   [0-3] SMD_PRI_TYPE  [4-11] v0..v3 uint16  [12-13] n0  [14-15] pad
   [16-18] r,g,b  [19] code  [20-27] UVs  [28-31] tpage/clut (ignored)
   Texture per primitive from delivery_area_tex_map.h (0xFF = untextured).

   UVs come straight from the SMD (offset 20+) and wrap via the 128 texture
   window set in delivery_area_draw — the same scheme every other room uses.

   This room used to project its UVs from world position instead and mask them
   to 7 bits in software, which is how it was written before the rest of the
   game settled on baked UVs. Two things were wrong with that, and they are
   worth recording because both were reported as art bugs:

     - it threw away the UV map from Blender, so anything that is ONE image
       rather than a tiling material came out wrong. The double door is the
       clear case: eight polys that should carry one picture of a door were
       each handed gravel-style tiling coordinates and came out stretched;

     - masking in software only tiles POLYGON BY POLYGON, because the GPU
       interpolates linearly between the two masked corner values and cannot
       wrap in between. That put a hard ceiling on polygon size (256 world
       units on floors, 128 vertically on walls) which no other room has, and
       the mesh had to be subdivided against it — which is what pushed the
       primitive count, and the frame rate near the upper floor, over budget.

   The texture window does the wrapping in hardware, so both problems go away
   and polygons can be any size. Do not reintroduce compute_uv(). */
static void draw_smd_room(RenderContext *ctx) {
    uint8_t *p = (uint8_t *)room_smd->p_prims;
    int i;
    for (i = 0; i < room_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &room_smd->p_verts[vi[0]];
        SVECTOR *v1 = &room_smd->p_verts[vi[1]];
        SVECTOR *v2 = &room_smd->p_verts[vi[2]];

        {
            int32_t dx = (int32_t)v0->vx - cam_x;
            int32_t dz = (int32_t)v0->vz - cam_z;
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 1950)
                { p += stride; continue; }
        }

        DVECTOR sv[4];
        int32_t sz[4];
        int32_t otz, nclip;

        gte_ldv3(v0, v1, v2);
        gte_rtpt();
        gte_stsxy3c(sv);

        /* Reject only GTE-saturated vertices (overflow causes GPU raster glitches).
           GTE clamps SXY to 11-bit signed +-1023; legitimate off-screen vertices
           can project well outside the screen rect without saturating. */
        if (sv[0].vx <= -1023 || sv[0].vx >= 1023 || sv[0].vy <= -1023 || sv[0].vy >= 1023 ||
            sv[1].vx <= -1023 || sv[1].vx >= 1023 || sv[1].vy <= -1023 || sv[1].vy >= 1023 ||
            sv[2].vx <= -1023 || sv[2].vx >= 1023 || sv[2].vy <= -1023 || sv[2].vy >= 1023) {
            p += stride; continue;
        }

        /* Backface cull, except degenerate (triangle-shaped) quads flagged at
           build time in delivery_area_nocull — same scheme as the other rooms. */
        int nocull = (i < DELIVERY_AREA_PRIM_COUNT) && delivery_area_nocull[i];
        if (!pt->nocull && !nocull) {
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }

        /* After gte_rtpt(), SZ FIFO = [stale, v0, v1, v2] stored as sz[0..3].
           sz[0] is the discarded oldest value — only check sz[1]/sz[2]/sz[3]. */
        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        SVECTOR *v3    = 0;
        int32_t  v2_sz = sz[3];   /* v2's SZ, before the quad path reuses sz[3] */
        if (is_quad) {
            v3 = &room_smd->p_verts[vi[3]];
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
        /* Horizontal polys sort by their farthest corner, not their average,
           so floors stay behind whatever stands on them (see render.h). */
        if (poly_is_flat_y(v0, v1, v2, v3))
            otz = is_quad ? otz_far4(sz[1], sz[2], v2_sz, sz[3])
                          : otz_far3(sz[1], sz[2], sz[3]);
        if (otz <= 0) { p += stride; continue; }
        otz += 40;
        if (otz >= OT_LENGTH) otz = OT_LENGTH - 1;

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog_start = 455, fog_end = 1950;
        int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
        int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        uint8_t tex_idx = (i < DELIVERY_AREA_PRIM_COUNT) ? delivery_area_tex_map[i] : 0xFF;

        if (is_quad && tex_idx != 0xFF) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            uint8_t *uv = p + 20;
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly,
                (uint8_t)(((int32_t)col[0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8));
            poly->tpage = tex_tpage[tex_idx];
            poly->clut  = tex_clut[tex_idx];
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
            setRGB0(poly,
                (uint8_t)(((int32_t)col[0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8));
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F4);
        } else if (tex_idx != 0xFF) {
            /* Textured triangle — the wedges that close off the ramp's brick
               embankment and its rusty_fence side wall. They sit in plain view,
               so they cannot fall through to the flat-shaded path. */
            if (ctx->next_packet + sizeof(POLY_FT3) > buf_end) { p += stride; continue; }
            uint8_t *uv = p + 20;
            POLY_FT3 *poly = (POLY_FT3 *)ctx->next_packet;
            setPolyFT3(poly);
            setRGB0(poly,
                (uint8_t)(((int32_t)col[0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8));
            poly->tpage = tex_tpage[tex_idx];
            poly->clut  = tex_clut[tex_idx];
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
            setRGB0(poly,
                (uint8_t)(((int32_t)col[0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8),
                (uint8_t)(((int32_t)col[2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8));
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F3);
        }

        p += stride;
    }
}

static void draw_panel(
    RenderContext *ctx,
    int ox, int oy, int oz,
    int ux, int uy, int uz,
    int vx, int vy, int vz,
    int segs_u, int segs_v,
    uint8_t cr, uint8_t cg, uint8_t cb,
    int depth_bias
) {
    int i, j;
    for (j = 0; j < segs_v; j++) {
        for (i = 0; i < segs_u; i++) {
            int bx = ox + i*ux + j*vx;
            int by = oy + i*uy + j*vy;
            int bz = oz + i*uz + j*vz;

            SVECTOR v[4];
            v[0].vx = bx;       v[0].vy = by;       v[0].vz = bz;       v[0].pad = 0;
            v[1].vx = bx+ux;    v[1].vy = by+uy;    v[1].vz = bz+uz;    v[1].pad = 0;
            v[2].vx = bx+ux+vx; v[2].vy = by+uy+vy; v[2].vz = bz+uz+vz; v[2].pad = 0;
            v[3].vx = bx+vx;    v[3].vy = by+vy;    v[3].vz = bz+vz;    v[3].pad = 0;

            DVECTOR sv[4];
            int32_t sz[4];
            int32_t otz, nclip;

            gte_ldv3(&v[0], &v[1], &v[2]);
            gte_rtpt();
            gte_stsxy3c(sv);

            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) continue;

            gte_ldv0(&v[3]);
            gte_rtps();
            gte_stsxy(&sv[3]);

            gte_stsz4c(sz);
            if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) continue;

            gte_avsz4();
            gte_stotz(&otz);
            /* Horizontal quads (the ground grid) sort by their farthest corner
               so props/walls standing on them always win the seam (render.h). */
            if (poly_is_flat_y(&v[0], &v[1], &v[2], &v[3]))
                otz = otz_far4(sz[0], sz[1], sz[2], sz[3]);

            otz += depth_bias;
            if (otz <= 0) continue;
            if (otz >= OT_LENGTH) otz = OT_LENGTH - 1;

            int32_t face_cx = bx + (ux + vx) / 2;
            int32_t face_cz = bz + (uz + vz) / 2;
            int32_t dx = face_cx - cam_x;
            int32_t dz = face_cz - cam_z;
            int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);

            int32_t fog_start = 455;
            int32_t fog_end   = 1950;
            int32_t fog = dist;
            if (fog < fog_start) fog = fog_start;
            if (fog > fog_end)   fog = fog_end;
            int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

            uint8_t r = (uint8_t)(((int32_t)cr * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8);
            uint8_t g = (uint8_t)(((int32_t)cg * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8);
            uint8_t b = (uint8_t)(((int32_t)cb * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8);

            uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
            if (ctx->next_packet + sizeof(POLY_F4) > buf_end) continue;

            POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
            setPolyF4(poly);
            setRGB0(poly, r, g, b);

            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
            poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;

            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F4);
        }
    }
}

static void draw_room(RenderContext *ctx) {
    draw_panel(ctx,  1800,-300,-1800,  -300,0,0,  0,600,0,  12,1,  80,80,80,  400);
    draw_panel(ctx, -1800,-300, 1800,   300,0,0,  0,600,0,  12,1,  80,80,80,  400);
    draw_panel(ctx,  1800,-300, 1800,  0,0,-300,  0,600,0,  12,1,  60,60,60,  400);
    draw_panel(ctx, -1800,-300,-1800,  0,0, 300,  0,600,0,  12,1,  60,60,60,  400);
    draw_panel(ctx, -1800,-300,-1800,   300,0,0,  0,0,300,  12,12,  40,40,40,  400);
    draw_panel(ctx, -1800, 300,-1800,  0,0,300,  300,0,0,   12,12,  50,50,70,  400);
}

void delivery_area_draw(RenderContext *ctx) {
    /* Entities fog with the same near/far as the mesh (matches kitchen/reception). */
    g_fog_near = 455; g_fog_far = 1950;

    draw_sky_gradient(ctx);

    MATRIX rot_matrix;
    camera_build_view(&rot_matrix);

    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    if (room_smd) {
        draw_smd_room(ctx);
    } else {
        draw_room(ctx);
    }
    crates_draw(ctx);
    /* crates_draw sets a per-crate GTE matrix — restore the view matrix
       so all subsequent world-space draws project correctly. */
    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);
    /* draw_vampire(ctx); */ /* disabled — kept for later */
    /* This room now sets a 128 window like every other one, so the sprites whose
       VRAM sits at Voff >= 128 must bracket themselves around it: the dogs
       (V128/192) and their shadow (V160), and the key pickups (V128). Both
       modules restore this RECT after each sprite. Crates (wdcrate, Voff 0) and
       the medipacs (sml_med, Voff 0) need nothing. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        demon_dogs_set_texwindow(&tw);
        keys_set_texwindow(&tw);
    }
    draw_demon_dogs(ctx);
    sml_meds_draw(ctx);
    keys_draw(ctx);
    door_draw(ctx);
    /* Player overlays (particles, weapon, HUD) and the debug collision view
       are drawn by the shared draw_player_systems step in main, so they
       apply uniformly to every area. */

    /* 128x128 texture window so per-poly UVs wrap (tile) within each texture's
       page — this is what lets a polygon be any size at all, and it is why all
       seven of this room's textures have to sit at page-top (Voff 0) and on a
       page boundary. chnlnk_dl was moved off the half-page at x992 to x384 for
       exactly this reason; see the texture notes at the top of this file.

       Added LAST so LIFO puts it at the head of the OT_LENGTH-1 bucket, i.e.
       the first GPU command, ahead of every delivery primitive. */
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
            setTexWindow(twin, &tw);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    /* The New Game arrival sequence's fade up from the intro's black. Screen
       space, OT bucket 0, so it goes over the room, the props and the sprites;
       a no-op on every other entry into this room. */
    delivery_intro_draw(ctx);
}
