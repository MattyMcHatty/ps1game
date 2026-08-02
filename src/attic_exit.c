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
#include "attic_exit.h"
#include "collision.h"
#include "attic_exit_mesh_collision.h"
#include "attic_exit_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "east_stairwell.h"
#include "chainlink_door.h"
#include "lever.h"
#include "save_point.h"
#include "zombie.h"
#include "spider.h"
#include "web.h"
#include "item_pickup.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Attic Exit: the room north of the Attic Stairwell's west room. See
   attic_exit.h for the layout — a plain rectangle at y=0 with a chainlink cage
   against its north wall and the locked exit door inside it. */

static SMD  *attic_exit_smd  = NULL;
static void *attic_exit_buff = NULL;

/* One flat zone at y=0 over the whole footprint: the collision mesh detected a
   single floor plane spanning the full bounds (attic_exit_mesh_collision.c),
   and the cage is fenced off by walls, not by a floor gap. */
static void attic_exit_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -1500; floor_zones[0].max_x = 1500;
    floor_zones[0].min_z =  -999; floor_zones[0].max_z =  999;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   Six mesh textures. FOUR are resident from startup (wd_flr/red_wlppr/din_cl
   with the kitchen, wd_dr with the fatdoor) and just need their headers
   captured. The other two are uploaded on every entry:

     - chnlnk (the cage) lives in the DELIVERY-only gravel slot (x640 y0), whose
       RAM copy the East Stairwell owns. We call its narrow
       east_stairwell_upload_chnlnk() rather than registering a second copy:
       texmgr has a hard TEXMGR_MAX and a registration past it fails SILENTLY,
       handing back tpage/clut 0 and breaking that texture in every room.
       delivery_restore_textures() puts gravel back on delivery entry.
     - xt_dr_lckd (the locked exit door) is this room's only NEW art. It
       time-shares the DELIVERY-only brick_wall slot (x768 y0) that the
       conservatory already streams grss over and the master bedroom bed over,
       so it adds no new restore obligation: delivery_restore_textures() puts
       brick_wall back, conservatory_upload_textures() puts grss back and
       master_bedroom_upload_textures() puts bed back. It is 8bpp, so it covers
       the whole x[768,832) tpage rather than the 4bpp pair's x[768,800) half —
       the extra columns are bed's, which is 8bpp too.

   All six sit at Voff 0, so the one 128 texture window set in attic_exit_draw
   serves them all (see tools/VRAM_MAP.txt). */
#define ATTIC_EXIT_TEX_COUNT 6

/* Streamed slots we own a RAM copy of: engine slot -> texmgr id. Only
   xt_dr_lckd — chnlnk is registered by the module that owns it. */
#define ATTIC_EXIT_NEW_TEX 1
static int new_tex_id[ATTIC_EXIT_NEW_TEX];
static const struct { const char *file; int slot; } new_tex[ATTIC_EXIT_NEW_TEX] = {
    { "\\TEX\\XTDRLCKD.TIM;1", 5 },
};

static uint16_t tex_tpage[ATTIC_EXIT_TEX_COUNT];
static uint16_t tex_clut[ATTIC_EXIT_TEX_COUNT];

/* Read a whole TIM into a freshly malloc'd buffer (caller owns it). NULL on fail. */
static uint8_t *read_tim(const char *filename) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)filename)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    uint8_t *buf = malloc(sectors * 2048);
    if (!buf) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);
    return buf;
}

static void *load_file_from_cd(const char *filename) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)filename)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    void *buff = malloc(sectors * 2048);
    if (!buff) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buff, CdlModeSpeed);
    CdReadSync(0, NULL);
    return buff;
}

/* Capture tpage/clut for a texture that is ALREADY resident in VRAM (shared
   with another room), or whose upload another module owns: read its header
   only, no LoadImage. */
static void capture_tpage(const char *filename, int slot) {
    uint8_t *buf = read_tim(filename);
    if (!buf) return;
    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    if (tim.mode & 0x8) tex_clut[slot] = getClut(tim.crect->x, tim.crect->y);
    tex_tpage[slot] = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);
    free(buf);
}

/* Load geometry AND register/capture textures at STARTUP (the only time CD
   access is safe — see tools/TEXTURING_NOTES.txt). */
void attic_exit_load_assets(void) {
    attic_exit_buff = load_file_from_cd("\\TEX\\ATTCEXIT.SMD;1");
    if (attic_exit_buff)
        attic_exit_smd = smdInitData(attic_exit_buff);

    /* Streamed slots we own: RAM-resident via the texture manager, uploaded on
       entry. */
    for (int i = 0; i < ATTIC_EXIT_NEW_TEX; i++) {
        int slot = new_tex[i].slot;
        new_tex_id[i]   = texmgr_register(new_tex[i].file);
        tex_tpage[slot] = texmgr_tpage(new_tex_id[i]);
        tex_clut[slot]  = texmgr_clut(new_tex_id[i]);
    }

    /* Owned by the East Stairwell, whose narrow upload attic_exit_upload_textures
       calls. Header only — no LoadImage, no second RAM copy. */
    capture_tpage("\\TEX\\CHNLNK.TIM;1", 4);

    /* Resident from startup (kitchen + fatdoor). */
    capture_tpage("\\WDFLR.TIM;1",    0);
    capture_tpage("\\REDWLPPR.TIM;1", 1);
    capture_tpage("\\DINCL.TIM;1",    2);
    capture_tpage("\\WDDR.TIM;1",     3);
}

/* Upload the streamed textures from their resident RAM copies. Pure LoadImage
   — no CD access — safe during the room transition (the caller DrawSyncs first,
   as main's STATE_LOADING does). */
void attic_exit_upload_textures(void) {
    for (int i = 0; i < ATTIC_EXIT_NEW_TEX; i++)
        texmgr_upload(new_tex_id[i]);       /* xt_dr_lckd -> brick_wall slot */
    east_stairwell_upload_chnlnk();         /* chnlnk     -> gravel slot     */
}

/* ---- The south-wall door back to the Attic Stairwell -----------------------
   The wd_dr poly in "Attic Exit.smx" sits at x[-500,-300], z=-1000 (the drawn
   wall; the collision wall behind it is at z=-999). It maps to the Attic
   Stairwell's west-room north-wall door at x=-1942, z=350 — the meshes are
   modelled in a shared world offset by attic_exit_x = stairwell_x + 1542.

   The player approaches from the +Z (room) side, so the sign lies in the XY
   plane with mirror=1 — the mirror image of the Master Bedroom's north-wall
   signs, which are read from -Z. */
#define AEXIT_DOOR_X            (-400)
#define AEXIT_DOOR_Z           (-1000)
#define AEXIT_TEXT_Y            (-186)
#define AEXIT_TEXT_RADIUS        1500
#define AEXIT_FADE_NEAR          1000
#define AEXIT_TRIGGER_RADIUS      500

/* Circle edge-detect, seeded by attic_exit_door_arm(). Starts "held" so a press
   carried in through the transition doesn't bounce the player straight back. */
static int door_circle_prev = 1;

static int circle_held(void) {
    if (!pad_buff_len[0]) return 0;
    PadResponse *pad = (PadResponse *)pad_buff[0];
    return (~pad->btn & PAD_CIRCLE) ? 1 : 0;
}

void attic_exit_door_arm(void) {
    door_circle_prev = circle_held();
}

int attic_exit_door_triggered(void) {
    int held = circle_held();
    int just = held && !door_circle_prev;
    door_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - AEXIT_DOOR_X;
    int32_t dz = cam_z - AEXIT_DOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < AEXIT_TRIGGER_RADIUS;
}

/* Floating "Press O to enter" sign on the south-wall door. XY plane:
   door_draw_string_3d centres the reading axis (X) on world_x after adding 200,
   so pass door_x - 200. Sits just north (z+11) of the wall so it floats in
   front of the door. */
static void door_text(RenderContext *ctx) {
    int32_t dx = cam_x - AEXIT_DOOR_X;
    int32_t dz = cam_z - AEXIT_DOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= AEXIT_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > AEXIT_FADE_NEAR) {
        int range = AEXIT_TEXT_RADIUS - AEXIT_FADE_NEAR;
        int prog  = xz - AEXIT_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        AEXIT_DOOR_X - 200, AEXIT_TEXT_Y, AEXIT_DOOR_Z + 11,
                        50, 255, 50, fade, 1, TEXT_PLANE_XY, DOOR_PIXEL_SIZE);
}

/* Arriving from the Attic Stairwell: stand north of the z=-999 wall, far enough
   clear of the 195 push radius apply_collision_reception uses, facing +Z — the
   direction of travel through the door, with the cage ahead. */
void attic_exit_spawn_south(void) {
    cam_x   = AEXIT_DOOR_X;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = -770;
    cam_rot = 0;      /* facing +Z, into the room */
    attic_exit_door_arm();
}

void attic_exit_init(void) {
    attic_exit_collision_init(&current_collision_room);
    /* Proxy wall tops stop at y=-466, well short of the drawn y=-560 ceiling;
       state the DRAWN value so ceiling-mounted enemies hang flush with the roof
       the player can see (see tools/ADDING_A_ROOM.txt). */
    collision_set_ceiling_y(-560);
    attic_exit_floor_zones_init();

    /* Only one way in, so no per-door override is needed in main.c. */
    attic_exit_spawn_south();

    /* The chainlink gate, filling the cage's entrance gap. The model is exactly
       the size of that gap — 600 wide, front face 31 deep — and its origin is
       centred in X on its front face, so it drops in at the gap's own
       coordinates: x=0, z=0, unrotated. y=-149 seats a base-origin model on this
       room's y=0 floor (as the concrete props do). With it in place the cage is
       sealed, so the locked exit door inside is scenery you can only look at. */
    chainlink_doors_clear();
    chainlink_door_place(STATE_ATTIC_EXIT, 0, -149, 0, 0);

    /* One lever in each of the four brown wall boxes modelled into the room
       (the rgb(129,49,3) clusters in "Attic Exit.smx"), each a 60x150x40 box
       standing off its wall. x and y are the box's centre; y goes through the
       standing-reference conversion, so world_y = y + GROUND_FLOOR_Y.

       Z is NOT the box centre. The lever is 150 long against a box only 40
       deep, so it has to be pushed along Z until its blue cap (local z=+75)
       meets the wall — leaving the brown shaft crossing the box and the red tip
       protruding into the room, which is the whole point of the fixture. Local
       +Z is the blue end, so the north-wall pair sit unrotated and the
       south-wall pair are turned 180deg (rot_y 2048). */
    levers_clear();
    /* North wall (z=+1000): blue cap at +Z, so rot_y 0. */
    lever_place(STATE_ATTIC_EXIT, -1052, -354,  925, 0);
    lever_place(STATE_ATTIC_EXIT,  1129, -354,  923, 0);
    /* South wall (z=-1000): blue cap must face -Z, so rot_y 2048. */
    lever_place(STATE_ATTIC_EXIT, -1051, -355, -925, 2048);
    lever_place(STATE_ATTIC_EXIT,  1111, -355, -925, 2048);

    /* Reception's save point and dresser prop are global (not room-swapped) and
       neither is area-gated in its collide routine, so reception's instances
       would block the player invisibly inside this room's bounds — the save
       point sits at (78,-67), right in the cage's entrance gap. Clearing them is
       safe: reception_init() re-places both on every reception entry. */
    save_points_clear();
    dressers_clear();
}

static void draw_attic_exit_smd(RenderContext *ctx) {
    if (!attic_exit_smd) return;

    uint8_t *p = (uint8_t *)attic_exit_smd->p_prims;
    int i;

    for (i = 0; i < attic_exit_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &attic_exit_smd->p_verts[vi[0]];
        SVECTOR *v1 = &attic_exit_smd->p_verts[vi[1]];
        SVECTOR *v2 = &attic_exit_smd->p_verts[vi[2]];

        {
            int32_t dx = (int32_t)v0->vx - cam_x;
            int32_t dz = (int32_t)v0->vz - cam_z;
            /* Distance cull (Manhattan) at the fog-out distance so culled polys
               are already invisible (same budget as the other rooms). */
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 1500)
                { p += stride; continue; }
            int32_t fwd = dx * isin(cam_rot) + dz * icos(cam_rot);
            if (fwd < -(700 << 12))
                { p += stride; continue; }
        }

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

        /* Backface cull, except degenerate (triangle-shaped) quads flagged at
           build time in attic_exit_nocull — same scheme as the other rooms. */
        int nocull = (i < ATTIC_EXIT_PRIM_COUNT) && attic_exit_nocull[i];
        if (!pt->nocull && !nocull) {
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }

        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        SVECTOR *v3    = 0;
        int32_t  v2_sz = sz[3];   /* v2's SZ, before the quad path reuses sz[3] */
        if (is_quad) {
            v3 = &attic_exit_smd->p_verts[vi[3]];
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
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog_start = 350, fog_end = 1500;   /* fog saturates at the cull distance */
        int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
        int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in attic_exit_draw. */
        uint8_t tex_idx = (i < ATTIC_EXIT_PRIM_COUNT) ? attic_exit_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < ATTIC_EXIT_TEX_COUNT);
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8);

        if (is_quad && textured) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            uint8_t *uv = p + 20;
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, r, g, b);
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

void attic_exit_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = 350; g_fog_far = 1500;

    /* Dark interior background, same as the other rooms. */
    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setXY0(bg, 0, 0);
    setWH(bg, SCREEN_XRES, SCREEN_YRES);
    setRGB0(bg, 20, 15, 10);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], bg);
    ctx->next_packet += sizeof(TILE);

    /* 128x128 texture window so per-poly UVs wrap (tile) within each texture's
       page. All six attic-exit textures sit at page-top (Voff 0), so one window
       serves them (see tools/VRAM_MAP.txt). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    /* View matrix from the camera (same construction as the other rooms). */
    MATRIX rot_matrix;
    SVECTOR neg_rot = {0, -cam_rot, 0, 0};
    RotMatrix(&neg_rot, &rot_matrix);

    VECTOR trans;
    trans.vx = -cam_x;
    trans.vy = -cam_y;
    trans.vz = -cam_z;
    ApplyMatrixLV(&rot_matrix, &trans, &trans);
    rot_matrix.t[0] = trans.vx;
    rot_matrix.t[1] = trans.vy;
    rot_matrix.t[2] = trans.vz;

    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    draw_attic_exit_smd(ctx);
    chainlink_doors_draw(ctx);   /* the cage gate; shares the room's texture window */
    levers_draw(ctx);            /* the four wall levers (flat-shaded, no texture) */

    /* No enemies placed here yet; the room's 128 texture window is still handed
       to the sprite renderers so a future spawn brackets its Voff>=128 sprite
       correctly (see tools/TEXTURING_NOTES.txt PART 5). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
        spiders_set_texwindow(&tw);
    }
    draw_zombies(ctx);
    draw_spiders(ctx);
    webs_draw(ctx);
    item_pickups_draw(ctx);

    door_text(ctx);
}
