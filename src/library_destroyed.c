#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "room_arena.h"
#include "tim_slots.h"
#include "camera.h"
#include "library_destroyed.h"
#include "collision.h"
#include "library_destroyed_mesh_collision.h"
#include "library_destroyed_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "concrete_props.h"
#include "piano_room.h"
#include "piano_props.h"
#include "save_point.h"
#include "player.h"          /* game_flag, FLAG_HADAD_TWO */
#include "zombie.h"
#include "spider.h"
#include "rabisu.h"
#include "hadad.h"
#include "hadad_library.h"
#include "item_pickup.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Library Destroyed: the Library's replacement once the keystones come out of
   the Attic Exit's door. Drawn exactly like the Library (per-poly tex map + 128
   texture window + fog) because it IS the Library's mesh with the reading room
   collapsed — same 466 prims, same seven textures, 91 of its 468 vertices moved.
   See library_destroyed.h for the room's shape and why it is a separate room. */

static SMD  *ld_smd  = NULL;
static void *ld_buff = NULL;

/* Once the player has pulled the four stones back out of the exit door, every
   route that used to reach the Library reaches this room instead. One reader
   for both neighbours and the save menu so the rule cannot drift apart. */
int library_destroyed_active(void) {
    return game_flag(FLAG_HADAD_TWO);
}

/* Single flat floor at y=0. Every FLOOR plane the collision generator found is
   at y=0 (the other 72 entries it reported are the -730 ceiling), so this is the
   Library's zone unchanged. The rect over-covers the rubble block and the
   L-shape's empty north-west quarter, which is harmless — the walls keep the
   player out of both. */
static void ld_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -1780; floor_zones[0].max_x = 350;
    floor_zones[0].min_z = -2080; floor_zones[0].max_z = 349;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   The same seven the Library draws, in the same order — the destroyed mesh was
   modelled from it and its material set is untouched. So this room adds NO new
   art, claims NO new VRAM and spends NO texmgr registration; it borrows the
   same three narrow uploaders the Library does:
     - cncrte     occupies the kchn_tile slot (x384 y0)   -> concrete_props
     - prpl_wlppr occupies the stove slot     (x384 y256) -> piano room
     - bookshelf  occupies the stn_stl slot   (x320 y0)   -> bookcase prop
   and takes the other four from the startup-resident set (wd_flr, din_cl,
   inr_dbl_dr with the kitchen/reception, wd_dr with the fat door).

   NOTE the narrow entry points, for the reason library.c states: the full
   piano_room_upload_textures() would also drag in piano_keys, which shares the
   kchn_tile slot with the cncrte this room needs.

   All three streamed slots are already on kitchen_restore_textures()' list, so
   stomping them here creates no NEW restore obligation — this room stomps
   exactly what the Library already stomped. All seven sit at Voff 0, so the one
   128 texture window set in library_destroyed_draw serves them all. */
#define LD_TEX_COUNT 7

static uint16_t tex_tpage[LD_TEX_COUNT];
static uint16_t tex_clut[LD_TEX_COUNT];

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. See src/room_arena.h. */
void library_destroyed_load_geometry(void) {
    ld_buff = room_arena_load("\\TEX\\LIBDEST.SMD;1");
    ld_smd  = ld_buff ? smdInitData(ld_buff) : NULL;
}

/* Nothing to read at STARTUP: every texture this room draws is registered and
   uploaded by another module, so the slots below are compile-time constants and
   this costs no CD access at all. */
void library_destroyed_load_assets(void) {
    /* Uploaded by concrete_props (cncrte), the piano room (prpl_wlppr) and the
       bookcase prop (bookshelf), all of which the upload fn below calls. */
    TIM_SLOT(0, CNCRTE);
    TIM_SLOT(1, PRPLWLP);
    TIM_SLOT(4, BOOKSHLF);

    /* Resident from startup (kitchen/reception + fatdoor). */
    TIM_SLOT(2, WDFLR);
    TIM_SLOT(3, DINCL);
    TIM_SLOT(5, INRDBLDR);
    TIM_SLOT(6, WDDR);
}

/* Upload the three streamed textures from their owners' resident RAM copies.
   Pure LoadImage — no CD access — safe during the room transition (the caller
   DrawSyncs first, as main's STATE_LOADING does). */
void library_destroyed_upload_textures(void) {
    concrete_props_upload_textures();       /* cncrte     -> kchn_tile slot */
    piano_room_upload_wallpaper();          /* prpl_wlppr -> stove slot     */
    piano_props_upload_bookcase_texture();  /* bookshelf  -> stn_stl slot   */
}

/* ---- The west double door back to the East Hall ----------------------------
   Unmoved from the Library: the entrance vestibule's west wall (x=-350, from the
   inr_dbl_dr polys in "Library Destroyed.smx"), centred on z=0, mapping to the
   double door at the EAST end of the East Hall (east hall x=2672, z=372). The
   player approaches from the +X (vestibule) side, so the sign lies in the YZ
   plane with mirror=0. Interaction point 65 units east of the wall. */
#define LDDOOR_W_X               (-285)
#define LDDOOR_W_Z                   0
#define LDDOOR_TEXT_Y           (-186)
#define LDDOOR_TEXT_RADIUS        1500
#define LDDOOR_FADE_NEAR          1000
#define LDDOOR_TRIGGER_RADIUS      500

/* ---- The south single door onto the East Stairwell -------------------------
   Also unmoved: the south wall (z=-2080, from the wd_dr polys), centred on
   x=-1400, mapping to the East Stairwell's EAST landing door (stairwell x=503,
   z=349). The player approaches from +Z, so the sign lies in the XY plane with
   mirror=1. Interaction point 65 units north of the wall.

   >>> THE PLAYER CANNOT STAND SQUARE ON THIS ONE ANY MORE. <<< The rubble block's
   west face (collision wall 4, x=-1303, normal -X) holds them to x <= -1498, so
   they reach the door from about 100 units west of its centre rather than head
   on. The 500 Manhattan trigger covers that with room to spare (the closest
   standable cell measures 245), and the spawn below is placed in the aisle
   rather than on the door for the same reason. */
#define LDSDOOR_X              (-1400)
#define LDSDOOR_Z              (-2015)

/* Circle edge-detect, one per door, seeded by library_destroyed_doors_arm().
   Both start "held" so a press carried in from a transition doesn't bounce the
   player straight back out. */
static int wdoor_circle_prev = 1;
static int sdoor_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void library_destroyed_doors_arm(void) {
    int held = circle_held();
    wdoor_circle_prev = held;
    sdoor_circle_prev = held;
}

int library_destroyed_wdoor_triggered(void) {
    int held = circle_held();
    int just = held && !wdoor_circle_prev;
    wdoor_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - LDDOOR_W_X;
    int32_t dz = cam_z - LDDOOR_W_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < LDDOOR_TRIGGER_RADIUS && interact_facing(LDDOOR_W_X, LDDOOR_W_Z);
}

int library_destroyed_sdoor_triggered(void) {
    int held = circle_held();
    int just = held && !sdoor_circle_prev;
    sdoor_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - LDSDOOR_X;
    int32_t dz = cam_z - LDSDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < LDDOOR_TRIGGER_RADIUS && interact_facing(LDSDOOR_X, LDSDOOR_Z);
}

/* Floating "Press O to enter" sign on the west door. YZ plane:
   door_draw_string_3d centres the reading axis (Z) on world_z after adding 200,
   so pass door_z - 200. mirror=0 because the player reads it from +X. */
static void lddoor_text(RenderContext *ctx) {
    int32_t dx = cam_x - LDDOOR_W_X;
    int32_t dz = cam_z - LDDOOR_W_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= LDDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > LDDOOR_FADE_NEAR) {
        int range = LDDOOR_TEXT_RADIUS - LDDOOR_FADE_NEAR;
        int prog  = xz - LDDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        LDDOOR_W_X, LDDOOR_TEXT_Y, LDDOOR_W_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* Floating sign on the south door. XY plane (fixed Z): door_draw_string_3d
   centres the reading axis (X) on world_x after adding 200, so pass door_x-200.
   mirror=1 because the player reads it from +Z. */
static void ldsdoor_text(RenderContext *ctx) {
    /* Dead from the moment Hadad appears, and it is the SIGN going out that
       tells the player so — the crawl gap's own prompt comes up in its place the
       same frame (hadad_library.h). main.c gates the trigger on the same
       predicate, so the door never answers either. */
    if (hadad_library_seals_sdoor()) return;

    int32_t dx = cam_x - LDSDOOR_X;
    int32_t dz = cam_z - LDSDOOR_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= LDDOOR_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > LDDOOR_FADE_NEAR) {
        int range = LDDOOR_TEXT_RADIUS - LDDOOR_FADE_NEAR;
        int prog  = xz - LDDOOR_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        LDSDOOR_X - 200, LDDOOR_TEXT_Y, LDSDOOR_Z,
                        50, 255, 50, fade, 1, TEXT_PLANE_XY, DOOR_PIXEL_SIZE);
}

/* Spawn just inside the west door, far enough east of the x=-350 wall to clear
   the 195 push radius, facing +X (east) along the direction of travel through
   the door. The vestibule is untouched by the collapse, so this is the
   Library's spawn unchanged. */
void library_destroyed_spawn_west(void) {
    cam_x   = -80;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = LDDOOR_W_Z;
    cam_rot = 1024;   /* facing +X, into the vestibule */
    library_destroyed_doors_arm();
}

/* Spawn just inside the south door, facing +Z back up the west aisle.
   NOT on the door's centre line: x=-1400 is inside the rubble block's 195
   standoff (see LDSDOOR_X) and the player would be shoved west on the first
   frame. x=-1500 sits in the middle of the aisle's standable band
   (x[-1585,-1498]), and z=-1850 is 230 north of the z=-2080 wall. */
void library_destroyed_spawn_south(void) {
    cam_x   = -1500;
    cam_y   = -189;
    cam_vy  = 0;
    cam_z   = -1850;
    cam_rot = 0;      /* facing +Z, up the west aisle */
    library_destroyed_doors_arm();
}

void library_destroyed_init(void) {
    library_destroyed_collision_init(&current_collision_room);
    /* The proxy mesh tops its walls out well short of the drawn roof; the mesh
       actually DRAWN has the reading room's ceiling at y=-730 (the entrance
       vestibule's is the lower -500), same as the Library. State the drawn value
       so anything hung from the roof later sits flush with it. */
    collision_set_ceiling_y(-730);
    ld_floor_zones_init();

    /* Default arrival spawn; main.c's STATE_LOADING overrides it with
       library_destroyed_spawn_south() when the player came back from the East
       Stairwell. */
    library_destroyed_spawn_west();

    /* Reception's save point and dresser prop are global (not room-swapped) and
       neither is area-gated in its collide routine, so reception's instances
       would block the player invisibly inside this room's bounds — the save
       point sits at (78,-67), inside the entrance vestibule. Clearing them is
       safe: reception_init() re-places both on every reception entry. */
    save_points_clear();
    dressers_clear();

    /* The encounter director, LAST — it only parks itself here. It arms off
       hadad_library_present(), because world_enter() has not placed anybody yet
       on this line (ADDING_A_BOSS_ENCOUNTER.txt STEP 4). */
    hadad_library_enter();
}

static void draw_library_destroyed_smd(RenderContext *ctx) {
    if (!ld_smd) return;

    uint8_t *p = (uint8_t *)ld_smd->p_prims;
    int i;

    for (i = 0; i < ld_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &ld_smd->p_verts[vi[0]];
        SVECTOR *v1 = &ld_smd->p_verts[vi[1]];
        SVECTOR *v2 = &ld_smd->p_verts[vi[2]];

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
           build time in library_destroyed_nocull — same scheme as the other
           rooms. */
        int nocull = (i < LIBRARY_DESTROYED_PRIM_COUNT) && library_destroyed_nocull[i];
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
            v3 = &ld_smd->p_verts[vi[3]];
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
           texture window set in library_destroyed_draw. */
        uint8_t tex_idx = (i < LIBRARY_DESTROYED_PRIM_COUNT) ? library_destroyed_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < LD_TEX_COUNT);
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

void library_destroyed_draw(RenderContext *ctx) {
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
       page. All seven textures sit at page-top (Voff 0), so one window serves
       them (see tools/VRAM_MAP.txt). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    /* View matrix from the camera (same construction as the other rooms). */
    MATRIX rot_matrix;
    camera_build_view(&rot_matrix);

    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    draw_library_destroyed_smd(ctx);

    /* NOTHING is placed here — world.c seeds this room empty, deliberately: the
       Library's three ceiling spiders, its zombie and its pickups belong to
       STATE_LIBRARY and stay there. The renderers are still called, and still
       handed the room's 128 texture window, so a future spawn brackets its
       Voff>=128 sprite correctly (see tools/TEXTURING_NOTES.txt PART 5). This is
       the East Stairwell's arrangement, for the same reason.

       draw_hadads IS here, though: world.c seeds a THIRD Hadad into this room
       (HAD_ROLE_LIBRARY) and he is the one thing this room does contain. His
       three sprites sit at Voff 128, so he is handed the room's 128 window and
       brackets each quad with a full-page one himself. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
        spiders_set_texwindow(&tw);
        hadads_set_texwindow(&tw);
    }
    draw_zombies(ctx);
    draw_spiders(ctx);
    draw_rabisus(ctx);
    draw_hadads(ctx);
    item_pickups_draw(ctx);

    lddoor_text(ctx);
    ldsdoor_text(ctx);
    hadad_library_draw(ctx);   /* the crawl gap's prompt, in the door's place */
}
