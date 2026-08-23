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
#include "west_corridor.h"
#include "collision.h"
#include "west_corridor_mesh_collision.h"
#include "west_corridor_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "save_point.h"
#include "zombie.h"
#include "spider.h"
#include "rabisu.h"
#include "hadad.h"           /* he can come into the mansion through here */
#include "web.h"
#include "item_pickup.h"
#include "fatdoor.h"         /* the breakable door in the inner-room opening */
#include "kitchen_dining.h"   /* kitchen_upload_double_door / _red_crpt */
#include "piano_room.h"       /* piano_room_upload_wallpaper           */
#include "sound.h"            /* SFX_UNLOCK, for the east door's first press */
#include "player.h"           /* FLAG_WEST_CORR_DOOR — the east door's saved lock */

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* West Corridor — see west_corridor.h for the layout, the door pairing and why
   the room is single-level. Rendered the same way as the Master Bedroom /
   conservatory (per-poly tex map + 128 texture window + fog). */

static SMD  *west_corridor_smd  = NULL;
static void *west_corridor_buff = NULL;

/* Single flat floor at y=0 across the walkable bounds. All FOUR floor planes
   detected in west_corridor_mesh_collision.c sit at y=0, and the 14 walls hem
   the player into the parts of the rectangle that actually have floor, so one
   zone over the collision bounds is both correct and the safest thing to
   write — a zone list that misses a corner is how a player falls through. */
static void west_corridor_floor_zones_init(void) {
    floor_zones[0].type  = FLOOR_FLAT;
    floor_zones[0].min_x = -2700; floor_zones[0].max_x = 0;
    floor_zones[0].min_z = -325;  floor_zones[0].max_z = 2000;
    floor_zones[0].y     = 0;
    floor_zone_count = 1;
}

/* ---- Per-room textures -----------------------------------------------------
   Seven mesh textures and this room OWNS NONE of them, so it registers nothing
   with texmgr (which has a hard TEXMGR_MAX and overruns fail SILENTLY) and does
   no CD access at any point — the garden_courtyard.c arrangement.

   FOUR are resident from startup and cost nothing at all: red_wlppr, wd_flr and
   din_cl come up with the kitchen, wd_dr with the fat door. TIM_SLOT gives the
   tpage/clut as compile-time constants.

   The other THREE live in time-shared slots and have to be put back into VRAM
   on every entry. Each comes in through a NARROW single-texture upload on the
   module that already keeps a RAM copy, never a second registration and never
   that module's FULL uploader (which would drop slots this room needs):

     double_door  x832 y0   -> kitchen_upload_double_door()
     prpl_wlppr   x384 y256 -> piano_room_upload_wallpaper()
     red_crpt     x320 y256 -> kitchen_upload_red_crpt()

   This adds NO new restore obligation. Every one of those three slots is
   already stomped by the kitchen, the piano room, the 2F hall or the Master
   Bedroom, and the rooms that need the underlying texture back already restore
   it on their own entry: conservatory_upload_textures() puts con_tile back over
   double_door, reception_upload_textures() puts frnt_dr back over red_crpt,
   rear_gate_upload_textures() puts its retargeted double door back over the
   same page, and kitchen_restore_textures() puts stove back over prpl_wlppr. */
#define WEST_CORRIDOR_TEX_COUNT 7

static uint16_t tex_tpage[WEST_CORRIDOR_TEX_COUNT];
static uint16_t tex_clut[WEST_CORRIDOR_TEX_COUNT];

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale. */
void west_corridor_load_geometry(void) {
    west_corridor_buff = room_arena_load("\\TEX\\WSTCORR.SMD;1");
    west_corridor_smd  = west_corridor_buff ? smdInitData(west_corridor_buff) : NULL;
}

/* STARTUP. Nothing but TIM_SLOT lines: the pixels of all seven textures are
   already owned by other modules, so there is no texmgr_register and no CD read
   here. The tpage/clut are compile-time constants either way — the streamed
   three go back into the SAME VRAM slots their owners use, so the constants
   from tim_slots.h are the right ones. */
void west_corridor_load_assets(void) {
    TIM_SLOT(0, REDWLPPR);
    TIM_SLOT(1, WDFLR);
    TIM_SLOT(2, DINCL);
    TIM_SLOT(3, WDDR);
    TIM_SLOT(4, DBLDOOR);
    TIM_SLOT(5, PRPLWLP);
    TIM_SLOT(6, REDCRPT);
}

/* Put the three time-shared slots back. Pure LoadImage from RAM copies other
   modules already hold — no CD access — so it is safe during the room
   transition (the caller DrawSyncs first, as main's STATE_LOADING does). */
void west_corridor_upload_textures(void) {
    kitchen_upload_double_door();   /* double_door -> con_tile slot, x832 y0    */
    kitchen_upload_red_crpt();      /* red_crpt    -> frnt_dr  slot, x320 y256  */
    piano_room_upload_wallpaper();  /* prpl_wlppr  -> stove    slot, x384 y256  */
}

/* ---- The two doors ---------------------------------------------------------
   NORTH: the double_door polys in "West Corridor.smx" span x[-2550,-2250] at
   z=2000, so the centre is x=-2400. It lies in the XY plane and the player
   approaches from -Z, so its sign is TEXT_PLANE_XY with mirror=0 and sits just
   south of the wall (z-11).

   EAST: the wd_dr polys span z[-108,108] at x=0, so the centre is z=0. It lies
   in the YZ plane and the player approaches from -X, so its sign is
   TEXT_PLANE_YZ with mirror=1 and sits just west of the wall (x-11).

   Both leaves are drawn y[-400,0], mid -200, and door_draw_string_3d takes the
   glyph TOP — 7 rows of DOOR_PIXEL_SIZE = 28 units — so -215 centres the line
   on the leaf, the same offset reception.c uses for its ground-floor doors. */
#define WCDOOR_TEXT_Y           (-215)
#define WCDOOR_TEXT_RADIUS       1500
#define WCDOOR_FADE_NEAR         1000
#define WCDOOR_TRIGGER_RADIUS     500

#define WCDOOR_N_X              (-2400)
#define WCDOOR_N_Z                2000

#define WCDOOR_E_X                   0
#define WCDOOR_E_Z                   0

/* Standing eye height on this room's y=0 floor, as the other interior rooms. */
#define WC_EYE_Y                 (-189)

/* The EAST door's lock lives in FLAG_WEST_CORR_DOOR (src/player.h). It is the
   2F Hall's east door mechanic exactly: the door is locked from the Reception
   side until the player unlocks it from THIS side, the first Circle press here
   unlocks rather than enters, and Reception's ndoor reads the same bit. Being a
   GameFlag rather than a plain global is what makes it survive a save/load —
   game_flags rides in SaveData.flags, so no save-format change was needed. */

/* Circle edge-detect per door, seeded by west_corridor_doors_arm(). Start
   "held" so a press carried in through either transition doesn't fire the
   nearest trigger and bounce the player straight back out. */
static int ndoor_circle_prev = 1;
static int edoor_circle_prev = 1;

void west_corridor_doors_arm(void) {
    int held = interact_tapped();
    ndoor_circle_prev = held;
    edoor_circle_prev = held;
}

static int door_triggered(int32_t door_x, int32_t door_z, int *circle_prev) {
    int held = interact_tapped();
    int just = held && !*circle_prev;
    *circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - door_x;
    int32_t dz = cam_z - door_z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < WCDOOR_TRIGGER_RADIUS && interact_facing(door_x, door_z);
}

int west_corridor_ndoor_triggered(void) {
    return door_triggered(WCDOOR_N_X, WCDOOR_N_Z, &ndoor_circle_prev);
}

int west_corridor_edoor_triggered(void) {
    if (!door_triggered(WCDOOR_E_X, WCDOOR_E_Z, &edoor_circle_prev))
        return 0;

    /* First Circle press unlocks the door (no transition yet); once unlocked,
       further presses enter Reception, which reads the same flag. Straight out
       of hall_2f_edoor_triggered — same mechanic, same sound. */
    if (!game_flag(FLAG_WEST_CORR_DOOR)) {
        game_flag_set(FLAG_WEST_CORR_DOOR);
        sound_play(SFX_UNLOCK);
        return 0;
    }
    return 1;
}

/* Shared fade curve for both signs. */
static int door_sign_fade(int32_t xz) {
    if (xz <= WCDOOR_FADE_NEAR) return 256;
    int range = WCDOOR_TEXT_RADIUS - WCDOOR_FADE_NEAR;
    int prog  = xz - WCDOOR_FADE_NEAR;
    if (prog > range) prog = range;
    return 256 - ((prog * 256) / range);
}

/* North door's sign. XY plane: door_draw_string_3d centres the reading axis (X)
   on world_x after adding 200, so pass door_x - 200. */
static void ndoor_text(RenderContext *ctx) {
    int32_t dx = cam_x - WCDOOR_N_X;
    int32_t dz = cam_z - WCDOOR_N_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= WCDOOR_TEXT_RADIUS) return;

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        WCDOOR_N_X - 200, WCDOOR_TEXT_Y, WCDOOR_N_Z - 11,
                        50, 255, 50, door_sign_fade(xz), 0,
                        TEXT_PLANE_XY, DOOR_PIXEL_SIZE);
}

/* East door's sign. YZ plane: the reading axis is Z, so pass door_z - 200. */
static void edoor_text(RenderContext *ctx) {
    int32_t dx = cam_x - WCDOOR_E_X;
    int32_t dz = cam_z - WCDOOR_E_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= WCDOOR_TEXT_RADIUS) return;

    door_draw_string_3d(ctx,
                        game_flag(FLAG_WEST_CORR_DOOR) ? "Press " BTN_CIRCLE " to enter"
                                                       : "Press " BTN_CIRCLE " to unlock",
                        WCDOOR_E_X - 11, WCDOOR_TEXT_Y, WCDOOR_E_Z - 200,
                        50, 255, 50, door_sign_fade(xz), 1,
                        TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* Spawns. Both stand at least 215 off their door's wall — the push radius in
   apply_collision_reception() is 195 — and face along the direction of travel
   through the door. Each arms BOTH doors, so whichever one the player came
   through, a Circle still held from the transition fires neither. */
void west_corridor_spawn_north(void) {
    cam_x   = WCDOOR_N_X;
    cam_y   = WC_EYE_Y;
    cam_vy  = 0;
    cam_z   = 1750;   /* 250 south of the z=2000 wall */
    cam_rot = 2048;   /* facing -Z, down the west arm */
    west_corridor_doors_arm();
}

void west_corridor_spawn_east(void) {
    cam_x   = -250;   /* 250 west of the x=0 wall */
    cam_y   = WC_EYE_Y;
    cam_vy  = 0;
    cam_z   = WCDOOR_E_Z;
    cam_rot = 3072;   /* facing -X, along the south arm */
    west_corridor_doors_arm();
}

void west_corridor_init(void) {
    west_corridor_collision_init(&current_collision_room);
    collision_set_ceiling_y(0);   /* proxy wall tops reach the drawn ceiling */
    west_corridor_floor_zones_init();

    /* Default spawn: the east door back to Reception (main.c overrides it for
       an arrival through the north one). */
    west_corridor_spawn_east();

    /* Reception's save point and dresser prop are global (neither is in
       world.c's RoomState swap, neither is area-gated in its collide routine),
       so their instances would collide invisibly anywhere inside this room's
       bounds. Both of reception's happen to sit at x>0 and so fall outside
       x[-2700,0] — but that is a coincidence of this mesh, not a rule, and the
       clears are what every other room does. Safe: reception_init() re-places
       both on every reception entry. */
    save_points_clear();
    dressers_clear();
}

static void draw_west_corridor_smd(RenderContext *ctx) {
    if (!west_corridor_smd) return;

    uint8_t *p = (uint8_t *)west_corridor_smd->p_prims;
    int i;

    for (i = 0; i < west_corridor_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &west_corridor_smd->p_verts[vi[0]];
        SVECTOR *v1 = &west_corridor_smd->p_verts[vi[1]];
        SVECTOR *v2 = &west_corridor_smd->p_verts[vi[2]];

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
           build time in west_corridor_nocull — same scheme as the conservatory.
           (This mesh has none, but the table and the test stay: the flag is
           regenerated from the SMX and a re-export can introduce one.) */
        int nocull = (i < WEST_CORRIDOR_PRIM_COUNT) && west_corridor_nocull[i];
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
            v3 = &west_corridor_smd->p_verts[vi[3]];
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
        int32_t fog_start = 350, fog_end = 1500;   /* the conservatory's, and the
                                                      cull distance above */
        int32_t fog = dist < fog_start ? fog_start : (dist > fog_end ? fog_end : dist);
        int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in west_corridor_draw. */
        uint8_t tex_idx = (i < WEST_CORRIDOR_PRIM_COUNT) ? west_corridor_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < WEST_CORRIDOR_TEX_COUNT);
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

void west_corridor_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below —
       the conservatory's numbers, as the user asked. */
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
       page. All seven corridor textures sit at page-top (Voff 0), so one window
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
    camera_build_view(&rot_matrix);

    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    draw_west_corridor_smd(ctx);

    /* Breakable door filling the west-arm/inner-room opening. Draws with the
       room's active 128 texture window (its wd_dr UVs are 0-127, so the wrap is
       a no-op) and restores the view matrix before returning, which the sprite
       renderers below rely on. */
    fatdoors_draw(ctx);

    /* No wandering enemies placed here; the room's 128 texture window is still
       handed to every sprite renderer so a future spawn brackets its Voff>=128
       sprite correctly (see tools/TEXTURING_NOTES.txt PART 5).

       HADAD IS PLACED HERE — world_seed_room drops the second instance in front
       of the north double door, waiting in HAD_ABSENT for the player to walk
       into the corner where the corridor turns (see THE WEST CORRIDOR AMBUSH in
       src/hadad.h). He is not drawn until he arrives. His art
       needs NO texture work: hadad_stp1_64 (x608 y384) and hadad_stp2_64
       (x992 y384) own their VRAM outright and are LoadImaged once at startup by
       hadads_load_textures(), so nothing streams over them and this room owes
       no upload. What the room DOES owe is the bracket below — all three of his
       frames sit at Voff 128, so under this room's 128 window an unbracketed
       quad would wrap his V and draw the corridor's own wallpaper on him. */
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
    webs_draw(ctx);
    item_pickups_draw(ctx);

    ndoor_text(ctx);
    edoor_text(ctx);
}
