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
#include "crucifaxe.h"
#include "graveolver.h"
#include "weapon.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

void weapons_init(void) {
    crucifaxe_init();
    graveolver_init();
}

/* Cycle the equipped weapon to the next OWNED one (wrapping). */
static void cycle_weapon(void) {
    int w = current_weapon;
    int i;
    for (i = 0; i < MAX_WEAPON_TYPES; i++) {
        w = (w + 1) % MAX_WEAPON_TYPES;
        if (player_weapons & (1 << w)) break;
    }
    if (w != current_weapon) {
        current_weapon = (WeaponType)w;
        swing_timer = 0;   /* abort any in-progress crucifaxe swing */
    }
}

void weapons_update(void) {
    /* Edge-detect L2 so one press cycles once. */
    static int l2_prev = 0;
    int l2_held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        l2_held = (~pad->btn & PAD_L2) ? 1 : 0;
    }
    if (l2_held && !l2_prev && game_state != STATE_MENU)
        cycle_weapon();
    l2_prev = l2_held;

    /* Route to the equipped weapon's update. The crucifaxe's melee swing only
       runs while it is equipped (a gun can't chop crates/doors). The grave-olver
       has no update yet — firing comes next. */
    if (current_weapon == WEAPON_CRUCIFAXE)
        update_crucifaxe();
}

void weapons_draw(RenderContext *ctx) {
    if (current_weapon == WEAPON_GRAVEOLVER)
        draw_graveolver(ctx);
    else
        draw_crucifaxe(ctx);
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
        /* Compress to a very low OT range so the weapon always renders in front
           of dogs, items and geometry (all at higher OT indices). Faces still
           sort relative to each other. */
        otz >>= 4;
        if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;

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
    SVECTOR neg_rot = {0, -cam_rot, 0, 0};
    RotMatrix(&neg_rot, &view);
    VECTOR vt = {-cam_x, -cam_y, -cam_z};
    ApplyMatrixLV(&view, &vt, &vt);
    view.t[0] = vt.vx;
    view.t[1] = vt.vy;
    view.t[2] = vt.vz;
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
