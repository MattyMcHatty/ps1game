#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "particles.h"
#include "crate.h"
#include "key.h"
#include "sml_med.h"
#include "sound.h"

Crate crates[MAX_CRATES];
int   crate_count = 0;

static Crate crate_defaults[MAX_CRATES];
static SMD  *crate_smd    = NULL;
static void *crate_buffer = NULL;

static void load_file(const char *name, void **buf_out, int *size_out) {
    CdlFILE file;
    if (!CdSearchFile(&file, name)) { *buf_out = NULL; return; }
    int sectors  = (file.size + 2047) / 2048;
    *buf_out = malloc(sectors * 2048);
    if (!*buf_out) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)*buf_out, CdlModeSpeed);
    CdReadSync(0, NULL);
    if (size_out) *size_out = file.size;
}

void crates_init(void) {
    /* Load crate SMD */
    load_file("\\CRATE.SMD;1", &crate_buffer, NULL);
    if (crate_buffer)
        crate_smd = smdInitData(crate_buffer);

    /* Load crate texture into VRAM (16bpp, no CLUT needed) */
    void *tim_buf = NULL;
    load_file("\\WCRATE.TIM;1", &tim_buf, NULL);
    if (tim_buf) {
        TIM_IMAGE tim;
        GetTimInfo((uint32_t *)tim_buf, &tim);
        LoadImage(tim.prect, tim.paddr);
        DrawSync(0);
        free(tim_buf);
    }

    /* Define crate positions.
       y=93 puts the bottom face (model +56) at GROUND_FLOOR_Y=149.
       All Z values are positive (ahead of player spawn, which faces +Z). */
    int i = 0;

    /* Three crates under the upper floor at the far north of the big room.
       Upper floor footprint: x(-5483 to -4143), big room z(2557 to 5426).
       Spaced 500 units apart in X, centred on the overhang at z=5000.
       y=37: bottom face (model +112) sits at GROUND_FLOOR_Y=149. */
    crates[i].x = -5300; crates[i].y = 37; crates[i].z = 5000;
    crates[i].rot_y = 128; crates[i].state = CRATE_INTACT;
    crates[i].item = ITEM_MEDIPAC; crates[i].active = 1;
    crates[i].half_w = 160; crates[i].half_d = 160; i++;

    crates[i].x = -4800; crates[i].y = 37; crates[i].z = 5000;
    crates[i].rot_y = 0; crates[i].state = CRATE_INTACT;
    crates[i].item = ITEM_MEDIPAC; crates[i].active = 1;
    crates[i].half_w = 160; crates[i].half_d = 160; i++;

    crates[i].x = -4300; crates[i].y = 37; crates[i].z = 5000;
    crates[i].rot_y = 896; crates[i].state = CRATE_INTACT;
    crates[i].item = ITEM_KEY; crates[i].active = 1;
    crates[i].half_w = 160; crates[i].half_d = 160; i++;

    crate_count = i;

    int j;
    for (j = 0; j < crate_count; j++)
        crate_defaults[j] = crates[j];
}

void crates_reset(void) {
    int i;
    for (i = 0; i < crate_count; i++)
        crates[i] = crate_defaults[i];
    keys_reset();      /* also clears player_keys */
    sml_meds_reset();
}

void crates_update(void) {
    /* Reserved for smash animation timer, item pickup range, etc. */
}

void crates_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < crate_count; i++) {
        Crate *c = &crates[i];
        if (!c->active || c->state != CRATE_INTACT) continue;

        /* Skip if the mover is on a different floor level to the crate. */
        if (py < c->y - CRATE_HALF_H || py > c->y + CRATE_HALF_H) continue;

        /* Minkowski-expanded AABB plus push margin so the player's camera
           stops well clear of the visible model faces. */
        int32_t min_x = c->x - c->half_w - radius - CRATE_PUSH_MARGIN;
        int32_t max_x = c->x + c->half_w + radius + CRATE_PUSH_MARGIN;
        int32_t min_z = c->z - c->half_d - radius - CRATE_PUSH_MARGIN;
        int32_t max_z = c->z + c->half_d + radius + CRATE_PUSH_MARGIN;

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

void crates_draw(RenderContext *ctx) {
    if (!crate_smd) return;

    /* Build the camera view matrix (same computation as delivery_area_draw).
       CompMatrixLV needs this to combine view + per-crate world transforms. */
    MATRIX view;
    SVECTOR neg_rot = {0, -cam_rot, 0, 0};
    RotMatrix(&neg_rot, &view);
    VECTOR vt = {-cam_x, -cam_y, -cam_z};
    ApplyMatrixLV(&view, &vt, &vt);
    view.t[0] = vt.vx;
    view.t[1] = vt.vy;
    view.t[2] = vt.vz;

    /* smdSortModel runs GTE NCDS (normal colour cueing) for l_type=1 prims.
       Back colour = 128,128,128 gives neutral ambient (texture at full
       brightness). Zero light matrix = ambient-only, no directional light. */
    MATRIX zero_light = {{{0}}};
    gte_SetLightMatrix(&zero_light);
    gte_SetBackColor(128, 128, 128);

    /* SC_OT matching our OT length and the level renderer's depth bias. */
    SC_OT sc_ot;
    sc_ot.ot    = ctx->buffers[ctx->active_buffer].ot;
    sc_ot.otlen = OT_LENGTH;
    sc_ot.zdiv  = 1;
    sc_ot.zoff  = 0;

    int i;
    for (i = 0; i < crate_count; i++) {
        Crate *c = &crates[i];
        if (!c->active || c->state == CRATE_SMASHED) continue;

        int32_t dx = c->x - cam_x, dz = c->z - cam_z;
        if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 3000) continue;

        /* Combine view matrix with this crate's world transform.
           CompMatrixLV: combined.R = view.R × crate.R
                         combined.t = view.R × crate.t + view.t
           As a side-effect it also loads view into the GTE, so we
           immediately override with the combined result. */
        MATRIX crate_m, combined;
        SVECTOR cr = {0, c->rot_y, 0, 0};
        RotMatrix(&cr, &crate_m);
        VECTOR pos = {c->x, c->y, c->z};
        TransMatrix(&crate_m, &pos);
        CompMatrixLV(&view, &crate_m, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        ctx->next_packet = smdSortModel(&sc_ot, ctx->next_packet, crate_smd);
    }
    /* NOTE: GTE matrix is NOT restored here.
       delivery_area_draw re-sets it after this call returns. */
}

int crate_try_smash(void) {
    int i, smashed_any = 0;

    for (i = 0; i < crate_count; i++) {
        Crate *c = &crates[i];
        if (!c->active || c->state != CRATE_INTACT) continue;

        if (cam_y < c->y - CRATE_HALF_H || cam_y > c->y + CRATE_HALF_H) continue;

        int32_t min_x = c->x - c->half_w - SMASH_RANGE;
        int32_t max_x = c->x + c->half_w + SMASH_RANGE;
        int32_t min_z = c->z - c->half_d - SMASH_RANGE;
        int32_t max_z = c->z + c->half_d + SMASH_RANGE;

        if (cam_x < min_x || cam_x > max_x) continue;
        if (cam_z < min_z || cam_z > max_z) continue;

        /* Reject if crate is behind the player */
        int32_t cdx = (c->x - cam_x) >> 4;
        int32_t cdz = (c->z - cam_z) >> 4;
        int32_t dot = cdx * (isin(cam_rot) >> 4) + cdz * (icos(cam_rot) >> 4);
        if (dot <= 0) continue;

        c->state = CRATE_SMASHED;
        spawn_wood_burst(c->x, c->y - 30, c->z);
        sound_play(SFX_SMASH);

        switch (c->item) {
            case ITEM_MEDIPAC:
                sml_med_spawn(c->x, c->y, c->z);
                break;
            case ITEM_KEY:
                key_spawn(c->x, c->y, c->z, KEY_FRONT_DOOR);
                break;
            default:
                break;
        }

        smashed_any = 1;
    }
    return smashed_any;
}
