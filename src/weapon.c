#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "title.h"
#include "collision.h"   /* collision_segment_blocked, for the shared aim test */
#include "crucifaxe.h"
#include "graveolver.h"
#include "helluminator.h"
#include "menu.h"        /* the weapon wheel's boxes show the menu's weapon icons */
#include "sound.h"       /* SFX_CURSOR as a wheel's cursor moves */
#include "weapon.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- Shared ranged aim -----------------------------------------------------
   Moved wholesale out of graveolver.c when the Helluminator needed the same
   geometry with a three-times-wider circle. The only changes are that the radius
   and the range are now parameters instead of that file's GUN_AIM_RADIUS and
   GUN_RANGE. See weapon.h for the model. */

/* Project a world point to screen pixels BY HAND (no GTE state needed, so this
   is safe in the update phase, where firing runs). Rotation is Y-only, so view X
   is the point's perpendicular offset from the aim axis and view Z its forward
   depth; the perspective divide uses the renderer's H and centre. */
static int aim_project(int32_t x, int32_t y, int32_t z,
                       int32_t fx, int32_t fz, int *sx, int *sy) {
    int32_t dx = x - cam_x, dy = y - cam_y, dz = z - cam_z;
    int32_t depth = (dx * fx + dz * fz) >> 12;        /* view Z (forward) */
    if (depth <= 0) return 0;
    int32_t vx = (dx * fz - dz * fx) >> 12;           /* view X (perp)    */
    *sx = SCREEN_XRES / 2 + (vx * WEAPON_PROJ_H) / depth;
    *sy = SCREEN_YRES / 2 + (dy * WEAPON_PROJ_H) / depth;
    return 1;
}

void weapon_aim_ray_point(int32_t fx, int32_t fz, int32_t depth,
                          int32_t *px, int32_t *py, int32_t *pz) {
    int32_t view_x = ((aim_x - SCREEN_XRES / 2) * depth) / WEAPON_PROJ_H;
    int32_t view_y = ((aim_y - SCREEN_YRES / 2) * depth) / WEAPON_PROJ_H;
    *px = cam_x + ((fx * depth) >> 12) + ((fz * view_x) >> 12);   /* right = (fz,-fx) */
    *pz = cam_z + ((fz * depth) >> 12) - ((fx * view_x) >> 12);
    *py = cam_y + view_y;
}

int weapon_aim_in_circle(int32_t ex, int32_t cyc, int32_t ez,
                         int32_t hw, int32_t hh,
                         int32_t fx, int32_t fz,
                         int32_t radius, int32_t range, int32_t *out_depth) {
    return weapon_aim_in_box(ex, cyc, ez, hw, hh, fx, fz,
                             radius, radius, range, out_depth);
}

int weapon_aim_in_box(int32_t ex, int32_t cyc, int32_t ez,
                      int32_t hw, int32_t hh,
                      int32_t fx, int32_t fz,
                      int32_t rx, int32_t ry, int32_t range, int32_t *out_depth) {
    int32_t depth = ((ex - cam_x) * fx + (ez - cam_z) * fz) >> 12;
    if (depth <= 0 || depth > range) return 0;

    int cx, cy;
    if (!aim_project(ex, cyc, ez, fx, fz, &cx, &cy)) return 0;

    /* World half-extents -> screen pixels at the body's depth, plus the slop.
       Testing only the centre LINE (as this once did) left most of a wide sprite
       unhittable: a tentacle is 262 units across, ~84px at close range against a
       flat 14px of aim slop, so only the middle strip of the visible body
       scored. */
    int32_t phw = (hw * WEAPON_PROJ_H) / depth + rx;
    int32_t phh = (hh * WEAPON_PROJ_H) / depth + ry;

    int32_t ax = aim_x - cx; if (ax < 0) ax = -ax;
    int32_t ay = aim_y - cy; if (ay < 0) ay = -ay;
    if (ax > phw || ay > phh) return 0;

    *out_depth = depth;
    return 1;
}

int weapon_aim_clear_to(int32_t px, int32_t py, int32_t pz) {
    return !collision_segment_blocked(cam_x, cam_y, cam_z, px, py, pz);
}

int weapon_aim_clear(int32_t fx, int32_t fz, int32_t depth) {
    int32_t px, py, pz;
    weapon_aim_ray_point(fx, fz, depth, &px, &py, &pz);
    return !collision_segment_blocked(cam_x, cam_y, cam_z, px, py, pz);
}

void weapons_init(void) {
    crucifaxe_init();
    graveolver_init();
    helluminator_init();
}

/* Weapon-switch animation. Phase 1 lowers the outgoing weapon off the bottom;
   at the bottom current_weapon swaps to the target; phase 2 raises the new one
   back up. Input (fire/aim) is suppressed for the duration. */
#define WEAPON_SWITCH_FRAMES 14   /* frames per phase (down, then up)          */
#define WEAPON_SWITCH_DROP  260   /* view-space Y the model travels off screen  */
static int switch_phase  = 0;     /* 0 none, 1 lowering, 2 raising              */
static int switch_timer  = 0;
static int switch_target = 0;

int weapon_switch_offset(void) {
    if (switch_phase == 1)
        return (WEAPON_SWITCH_DROP * switch_timer) / WEAPON_SWITCH_FRAMES;
    if (switch_phase == 2)
        return (WEAPON_SWITCH_DROP * (WEAPON_SWITCH_FRAMES - switch_timer)) /
               WEAPON_SWITCH_FRAMES;
    return 0;
}

int weapon_switching(void) { return switch_phase != 0; }

/* ---- The selection wheel's look (shared — see weapon.h) --------------------- */
#define WHEEL_CX         160    /* screen centre of the cross                    */
#define WHEEL_CY          92    /* midway between the screen top and the HUD's
                                   top edge (HUD_Y, 184): the middle of what the
                                   player can actually see of the room          */
#define WHEEL_SPACING     42    /* centre-to-box-centre distance                 */
#define WHEEL_CELL        34    /* box size — the inventory menu's CELL_W        */
#define OT_WHEEL_BOX     12
#define OT_WHEEL_LINE    11
#define OT_WHEEL_CURSOR   7

static const int8_t wheel_dx[WHEEL_SLOTS] = { [WHEEL_UP] =  0, [WHEEL_LEFT] = -1,
                                              [WHEEL_RIGHT] = 1, [WHEEL_DOWN] = 0 };
static const int8_t wheel_dy[WHEEL_SLOTS] = { [WHEEL_UP] = -1, [WHEEL_LEFT] =  0,
                                              [WHEEL_RIGHT] = 0, [WHEEL_DOWN] = 1 };

/* Screen-space filled rect helper (2D, no GTE). */
static void screen_tile(RenderContext *ctx, int x, int y, int w, int h,
                        uint8_t r, uint8_t g, uint8_t b, int ot) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    TILE *t = (TILE *)ctx->next_packet;
    setTile(t);
    setRGB0(t, r, g, b);
    setXY0(t, x, y);
    setWH(t, w, h);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[ot], t);
    ctx->next_packet += sizeof(TILE);
}

static void screen_outline(RenderContext *ctx, int x, int y, int w, int h,
                           uint8_t r, uint8_t g, uint8_t b, int ot) {
    screen_tile(ctx, x,         y,         w, 1, r, g, b, ot);
    screen_tile(ctx, x,         y + h - 1, w, 1, r, g, b, ot);
    screen_tile(ctx, x,         y,         1, h, r, g, b, ot);
    screen_tile(ctx, x + w - 1, y,         1, h, r, g, b, ot);
}

int wheel_dpad_dir(int dpad_just) {
    if (dpad_just & PAD_UP)    return WHEEL_UP;
    if (dpad_just & PAD_LEFT)  return WHEEL_LEFT;
    if (dpad_just & PAD_RIGHT) return WHEEL_RIGHT;
    if (dpad_just & PAD_DOWN)  return WHEEL_DOWN;
    return -1;
}

/* A box in the inventory menu's colours; the picked one also wears the menu's
   highlight (blue/white double outline with white corner ticks). */
void wheel_draw_box(RenderContext *ctx, int slot, int picked, int *ix, int *iy) {
    const int pad = (WHEEL_CELL - WHEEL_ICON) / 2;
    int bx = WHEEL_CX + wheel_dx[slot] * WHEEL_SPACING - WHEEL_CELL / 2;
    int by = WHEEL_CY + wheel_dy[slot] * WHEEL_SPACING - WHEEL_CELL / 2;
    screen_tile(ctx, bx, by, WHEEL_CELL, WHEEL_CELL, 35, 30, 45, OT_WHEEL_BOX);
    screen_outline(ctx, bx, by, WHEEL_CELL, WHEEL_CELL, 80, 70, 100, OT_WHEEL_LINE);
    *ix = bx + pad;
    *iy = by + pad;

    if (picked) {
        int cx = bx + pad - 2, cy = by + pad - 2, cs = WHEEL_ICON + 4;
        screen_outline(ctx, cx - 2, cy - 2, cs + 4, cs + 4, 80, 80, 200, OT_WHEEL_CURSOR);
        screen_outline(ctx, cx, cy, cs, cs, 180, 180, 255, OT_WHEEL_CURSOR);
        screen_tile(ctx, cx,          cy,          3, 3, 255, 255, 255, OT_WHEEL_CURSOR);
        screen_tile(ctx, cx + cs - 3, cy,          3, 3, 255, 255, 255, OT_WHEEL_CURSOR);
        screen_tile(ctx, cx,          cy + cs - 3, 3, 3, 255, 255, 255, OT_WHEEL_CURSOR);
        screen_tile(ctx, cx + cs - 3, cy + cs - 3, 3, 3, 255, 255, 255, OT_WHEEL_CURSOR);
    }
}

/* Texture window OFF in the icons' own bucket, added after them so it heads
   the list — a room's 128 window would otherwise wrap the icons' UVs (the
   HUD's hud_disable_texwindow has the whole story). */
void wheel_draw_done(RenderContext *ctx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
        RECT full = {0, 0, 0, 0};
        DR_TWIN *tw = (DR_TWIN *)ctx->next_packet;
        setTexWindow(tw, &full);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_WHEEL_ICON], tw);
        ctx->next_packet += sizeof(DR_TWIN);
    }
}

/* ---- The weapon wheel (hold Triangle) ---------------------------------------
   Triangle has two jobs, told apart by how long it is held, exactly as R2 is
   for the Grave-olver:
     TAP  (released before WHEEL_HOLD_FRAMES)  switches to the next owned
          weapon, as Triangle always did.
     HOLD (WHEEL_HOLD_FRAMES or longer)        opens the weapon wheel; the d-pad
          picks a box and releasing Triangle switches to that weapon.
   Both are decided on RELEASE except the wheel opening itself. Until it opens
   the equipped weapon keeps working, so a hold that becomes a tap costs nothing.

   weapon_wheel_slots[] is the ONLY place that says which weapon sits where; -1
   is an empty box. A box can be picked only if its weapon is owned.

   Opening the wheel puts the weapon in hand down the way a switch does: a
   reload is abandoned, a burning lantern goes out and a swing is cut short.
   While the wheel is up the equipped weapon's update does not run at all, so
   nothing can be fired or reloaded mid-choice. */
static const int8_t weapon_wheel_slots[WHEEL_SLOTS] = {
    [WHEEL_UP]    = WEAPON_CRUCIFAXE,
    [WHEEL_LEFT]  = WEAPON_GRAVEOLVER,
    [WHEEL_RIGHT] = WEAPON_HELLUMINATOR,
    [WHEEL_DOWN]  = -1,
};

static int tri_frames   = -1;  /* frames Triangle has been down; -1 = not armed */
static int wwheel_open  = 0;
static int wwheel_pick  = WHEEL_UP;
/* Set by weapons_update on every frame the wheel is open and consumed by the
   draw, as the ammo wheel's wheel_show is. */
static int wwheel_show  = 0;

int weapon_wheel_open(void) { return wwheel_open; }

static int weapon_owned(int w) {
    return w >= 0 && (player_weapons & (1 << w)) != 0;
}

static void draw_weapon_wheel(RenderContext *ctx) {
    int s;
    for (s = 0; s < WHEEL_SLOTS; s++) {
        int ix, iy;
        wheel_draw_box(ctx, s, s == wwheel_pick, &ix, &iy);
        if (weapon_owned(weapon_wheel_slots[s]))
            menu_draw_weapon_icon(ctx, weapon_wheel_slots[s], ix, iy,
                                  WHEEL_ICON, OT_WHEEL_ICON);
    }
    wheel_draw_done(ctx);
}

/* The next OWNED weapon after current_weapon (wrapping), or current if it's the
   only one. */
static int next_owned_weapon(void) {
    int w = current_weapon, i;
    for (i = 0; i < MAX_WEAPON_TYPES; i++) {
        w = (w + 1) % MAX_WEAPON_TYPES;
        if (player_weapons & (1 << w)) return w;
    }
    return current_weapon;
}

/* Put the weapon in hand away without starting a switch — what both a switch
   and the wheel opening do to it. */
static void put_weapon_down(void) {
    /* Switching away mid-reload interrupts it — the cylinder stays empty
       and the reload must be restarted after switching back. */
    graveolver_cancel_reload();
    /* And a lantern being put away goes out. Nothing is lost by it —
       oil is spent per second of burning, so releasing Square early and
       switching are the same act. */
    helluminator_cancel_burn();
}

static void start_switch(int target) {
    if (target == current_weapon) return;
    put_weapon_down();
    switch_target = target;
    switch_phase  = 1;
    switch_timer  = 0;
}

void weapons_update(void) {
    /* Triangle and the d-pad are edge-detected every frame, a switch or not, so
       a press made during a switch is simply dropped rather than firing the
       moment it ends. */
    static int tri_prev  = 0;
    static int dpad_prev = 0;
    int tri_held = 0, dpad = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        tri_held = (~pad->btn & PAD_TRIANGLE) ? 1 : 0;
        dpad     = ~pad->btn & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
    }
    int tri_just  = tri_held && !tri_prev;
    int dpad_just = dpad & ~dpad_prev;
    tri_prev  = tri_held;
    dpad_prev = dpad;

    /* Drive an in-progress switch; block all weapon input until it finishes. */
    if (switch_phase) {
        switch_timer++;
        if (switch_timer >= WEAPON_SWITCH_FRAMES) {
            switch_timer = 0;
            if (switch_phase == 1) {
                current_weapon = (WeaponType)switch_target;  /* swap off screen */
                swing_timer    = 0;   /* abort any in-progress crucifaxe swing */
                switch_phase   = 2;
            } else {
                switch_phase = 0;
            }
        }
        return;
    }

    /* Triangle: tap for the next weapon, hold for the wheel (see the note at
       weapon_wheel_slots). Not armed in the menu, nor while the ammo wheel is
       up — R2 + d-pad is already choosing something. */
    if (game_state == STATE_MENU) {
        wwheel_open = 0;
        tri_frames  = -1;
    } else if (tri_just && !graveolver_wheel_open()) {
        tri_frames = 0;
    }
    if (tri_frames >= 0) {
        if (tri_held) {
            if (!wwheel_open && ++tri_frames >= WHEEL_HOLD_FRAMES) {
                int s;
                wwheel_open = 1;
                wwheel_pick = WHEEL_UP;
                for (s = 0; s < WHEEL_SLOTS; s++)
                    if (weapon_wheel_slots[s] == (int)current_weapon) wwheel_pick = s;
                put_weapon_down();
                swing_timer = 0;   /* abort any in-progress crucifaxe swing */
            }
            if (wwheel_open) {
                int dir = wheel_dpad_dir(dpad_just);
                if (dir >= 0 && dir != wwheel_pick &&
                    weapon_owned(weapon_wheel_slots[dir])) {
                    wwheel_pick = dir;
                    sound_play(SFX_CURSOR);
                }
                wwheel_show = 1;
                return;
            }
        } else {
            /* Released. Leaving the cursor on the weapon in hand is a change of
               mind, not a switch. */
            tri_frames = -1;
            if (wwheel_open) {
                wwheel_open = 0;
                start_switch(weapon_wheel_slots[wwheel_pick]);
            } else {
                start_switch(next_owned_weapon());
            }
            if (switch_phase) return;   /* a switch just started this frame */
        }
    }

    /* Route to the equipped weapon's update. The crucifaxe's melee swing only
       runs while it is equipped (a gun can't chop crates/doors); the grave-olver
       fires rounds. */
    if (current_weapon == WEAPON_CRUCIFAXE)
        update_crucifaxe();
    else if (current_weapon == WEAPON_GRAVEOLVER)
        graveolver_update();
    else if (current_weapon == WEAPON_HELLUMINATOR)
        helluminator_update();
}

void weapons_draw(RenderContext *ctx) {
    if (current_weapon == WEAPON_GRAVEOLVER)
        draw_graveolver(ctx);
    else if (current_weapon == WEAPON_HELLUMINATOR)
        draw_helluminator(ctx);
    else
        draw_crucifaxe(ctx);

    if (wwheel_show) {
        if (game_state != STATE_MENU) draw_weapon_wheel(ctx);
        wwheel_show = 0;
    }
}

void weapon_render_model(RenderContext *ctx, SMD *smd, MATRIX *weapon_vs,
                         int32_t gain) {
    if (!smd) return;

    gte_SetRotMatrix(weapon_vs);
    gte_SetTransMatrix(weapon_vs);

    /* Manual render — skips NCLIP so all faces draw regardless of winding.
       Applies diffuse shading via dot product of face normal with a fixed
       light direction, giving visible angle variation across faces.
       Light direction in model space: upper-right-front,
       (1,-1,1)/sqrt(3) * 4096 ~= (2365,-2365,2365). */
    const int32_t lx = 2365, ly = -2365, lz = 2365;

    uint8_t *p       = (uint8_t *)smd->p_prims;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int pi;

    /* Pass 1 — find this model's near/far OT range. The weapon has no depth
       buffer, so correct occlusion depends entirely on spreading its faces
       across OT buckets by depth. It must also stay in FRONT of world geometry
       (min OT ~41 = raw +40), which leaves too few buckets to use raw depth
       directly, and simply clamping to one bucket makes a solid model look
       see-through (back faces draw over front faces). So we measure the range
       here and remap it into a dedicated front band below. Same validity check
       as the emit pass. */
    int32_t min_otz = 0x7fffffff, max_otz = 0;
    for (pi = 0; pi < smd->n_prims; pi++) {
        SMD_PRI_TYPE *pt     = (SMD_PRI_TYPE *)p;
        uint8_t       stride = pt->len;
        uint16_t     *vi     = (uint16_t *)(p + 4);
        int32_t sz[4], otz;
        gte_ldv3(&smd->p_verts[vi[0]], &smd->p_verts[vi[1]], &smd->p_verts[vi[2]]);
        gte_rtpt();
        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }
        if (pt->type >= 2) { gte_ldv0(&smd->p_verts[vi[3]]); gte_rtps(); gte_avsz4(); }
        else               { gte_avsz3(); }
        gte_stotz(&otz);
        if (otz < min_otz) min_otz = otz;
        if (otz > max_otz) max_otz = otz;
        p += stride;
    }
    int32_t otz_span = max_otz - min_otz;
    if (otz_span < 1) otz_span = 1;

    /* Pass 2 — project again and emit, remapping each face's depth linearly into
       the front band [SCENE_OT_MIN, SCENE_OT_MIN + WEAPON_OT_SPAN]. Nearest face
       -> lowest index (drawn last, on top); band max stays below world geometry
       so the weapon is always in front. */
    #define WEAPON_OT_SPAN 24   /* band 16..40; world geometry starts at ~41 */
    p = (uint8_t *)smd->p_prims;
    for (pi = 0; pi < smd->n_prims; pi++) {
        SMD_PRI_TYPE *pt      = (SMD_PRI_TYPE *)p;
        uint8_t       stride  = pt->len;
        int           is_quad = (pt->type >= 2);
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &smd->p_verts[vi[0]];
        SVECTOR *v1 = &smd->p_verts[vi[1]];
        SVECTOR *v2 = &smd->p_verts[vi[2]];

        DVECTOR sv[4];
        int32_t sz[4], otz;

        gte_ldv3(v0, v1, v2);
        gte_rtpt();
        gte_stsxy3c(sv);
        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        if (is_quad) {
            SVECTOR *v3 = &smd->p_verts[vi[3]];
            gte_ldv0(v3); gte_rtps(); gte_stsxy(&sv[3]);
            gte_avsz4();
        } else {
            gte_avsz3();
        }
        gte_stotz(&otz);
        otz = SCENE_OT_MIN + ((otz - min_otz) * WEAPON_OT_SPAN) / otz_span;
        if (otz < SCENE_OT_MIN)                    otz = SCENE_OT_MIN;
        if (otz > SCENE_OT_MIN + WEAPON_OT_SPAN)   otz = SCENE_OT_MIN + WEAPON_OT_SPAN;

        /* Per-face normal shading: 40% ambient + primary + dim fill. */
        uint16_t n0_idx = *(uint16_t *)(p + 12);
        SVECTOR *norm   = &smd->p_norms[n0_idx];
        int32_t dot = ((int32_t)norm->vx * lx +
                       (int32_t)norm->vy * ly +
                       (int32_t)norm->vz * lz) >> 12;
        int32_t dot2 = -dot;  /* fill light from opposite direction */
        if (dot  < 0) dot  = 0;
        if (dot2 < 0) dot2 = 0;
        int32_t shade = 1638 + ((dot * 2458) >> 12) + ((dot2 * 820) >> 12);
        if (shade > 4096) shade = 4096;

        /* Brighten the base colour by `gain`, clamp, then apply the shade. */
        int32_t br = ((int32_t)p[16] * gain) >> 12;
        int32_t bg = ((int32_t)p[17] * gain) >> 12;
        int32_t bb = ((int32_t)p[18] * gain) >> 12;
        if (br > 255) br = 255;
        if (bg > 255) bg = 255;
        if (bb > 255) bb = 255;
        uint8_t r = (uint8_t)((br * shade) >> 12);
        uint8_t g = (uint8_t)((bg * shade) >> 12);
        uint8_t b = (uint8_t)((bb * shade) >> 12);

        if (is_quad) {
            if (ctx->next_packet + sizeof(POLY_F4) > buf_end) { p += stride; continue; }
            POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
            setPolyF4(poly);
            setRGB0(poly, r, g, b);
            poly->x0=sv[0].vx; poly->y0=sv[0].vy;
            poly->x1=sv[1].vx; poly->y1=sv[1].vy;
            poly->x2=sv[2].vx; poly->y2=sv[2].vy;
            poly->x3=sv[3].vx; poly->y3=sv[3].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F4);
        } else {
            if (ctx->next_packet + sizeof(POLY_F3) > buf_end) { p += stride; continue; }
            POLY_F3 *poly = (POLY_F3 *)ctx->next_packet;
            setPolyF3(poly);
            setRGB0(poly, r, g, b);
            poly->x0=sv[0].vx; poly->y0=sv[0].vy;
            poly->x1=sv[1].vx; poly->y1=sv[1].vy;
            poly->x2=sv[2].vx; poly->y2=sv[2].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F3);
        }
        p += stride;
    }

    /* Restore the camera view matrix. */
    MATRIX view;
    camera_build_view(&view);
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
