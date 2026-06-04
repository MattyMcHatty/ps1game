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

    crates[i].x = 0;    crates[i].y = 37; crates[i].z = 500;
    crates[i].rot_y = 256; crates[i].state = CRATE_INTACT;
    crates[i].item = ITEM_MEDIPAC; crates[i].active = 1; i++;

    crates[i].x = -300; crates[i].y = 93; crates[i].z = 600;
    crates[i].rot_y = 0; crates[i].state = CRATE_INTACT;
    crates[i].item = ITEM_NONE; crates[i].active = 1; i++;

    crates[i].x = 300;  crates[i].y = 93; crates[i].z = 700;
    crates[i].rot_y = 512; crates[i].state = CRATE_INTACT;
    crates[i].item = ITEM_KEY; crates[i].active = 1; i++;

    crate_count = i;

    int j;
    for (j = 0; j < crate_count; j++)
        crate_defaults[j] = crates[j];
}

void crates_reset(void) {
    int i;
    for (i = 0; i < crate_count; i++)
        crates[i] = crate_defaults[i];
    player_has_key = 0;
}

void crates_update(void) {
    /* Reserved for smash animation timer, item pickup range, etc. */
}

void crates_draw(RenderContext *ctx) {
    if (!crate_smd) return;

    /* Build the camera view matrix (same computation as draw_scene).
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
       draw_scene re-sets it after this call returns. */
}

int crate_try_smash(void) {
    int i;
    for (i = 0; i < crate_count; i++) {
        Crate *c = &crates[i];
        if (!c->active || c->state != CRATE_INTACT) continue;

        int32_t dx   = c->x - cam_x;
        int32_t dz   = c->z - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (dist > CRATE_SMASH_RADIUS) continue;

        /* Must be roughly in front of the player */
        int32_t dot = ((isin(cam_rot) >> 6) * (dx >> 6) +
                       (icos(cam_rot) >> 6) * (dz >> 6));
        if (dot <= 0) continue;

        c->state = CRATE_SMASHED;
        spawn_burst(c->x, c->y - 30, c->z, 120, 80, 20);

        switch (c->item) {
            case ITEM_MEDIPAC:
                player_health += 30;
                if (player_health > MAX_HEALTH)
                    player_health = MAX_HEALTH;
                break;
            case ITEM_KEY:
                player_has_key = 1;
                break;
            default:
                break;
        }

        return 1;
    }
    return 0;
}
