#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "particles.h"
#include "sound.h"
#include "title.h"
#include "fatdoor.h"

extern GameState game_state;   /* current area; doors of other areas are skipped */

FatDoor fatdoors[MAX_FATDOORS];
int     fatdoor_count = 0;

static FatDoor fatdoor_defaults[MAX_FATDOORS];
static SMD  *fatdoor_smd    = NULL;
static void *fatdoor_buffer = NULL;

/* Cracked-door texture (wd_dr_crk.tim), swapped in for a door on its last hit.
   It is 8bpp+CLUT (the intact wd_dr.tim baked into the SMD is 16bpp, no CLUT),
   but both are 128x128 so the texel-based UVs and the shared 128 texture window
   map either one correctly — we only override the per-poly tpage/clut. 0 = not
   loaded, in which case we fall back to the baked intact texture. */
static uint16_t fatdoor_crk_tpage = 0;
static uint16_t fatdoor_crk_clut  = 0;

static void load_file(const char *name, void **buf_out) {
    CdlFILE file;
    if (!CdSearchFile(&file, name)) { *buf_out = NULL; return; }
    int sectors = (file.size + 2047) / 2048;
    *buf_out = malloc(sectors * 2048);
    if (!*buf_out) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)*buf_out, CdlModeSpeed);
    CdReadSync(0, NULL);
}

void fatdoors_load_assets(void) {
    /* Door geometry (textured FT4 SMD from smxlink). */
    load_file("\\FATDOOR.SMD;1", &fatdoor_buffer);
    if (fatdoor_buffer)
        fatdoor_smd = smdInitData(fatdoor_buffer);

    /* Door texture (16bpp, no CLUT). LoadImage is only safe at startup, before
       the per-frame draw loop begins (see TEXTURING_NOTES). */
    void *tim_buf = NULL;
    load_file("\\WDDR.TIM;1", &tim_buf);
    if (tim_buf) {
        TIM_IMAGE tim;
        GetTimInfo((uint32_t *)tim_buf, &tim);
        LoadImage(tim.prect, tim.paddr);
        DrawSync(0);
        free(tim_buf);
    }

    /* Cracked variant (8bpp + CLUT) — loaded to its own VRAM slot so both
       textures stay resident. Capture its tpage/clut to override per-poly when
       a door reaches its last hit point. */
    void *crk_buf = NULL;
    load_file("\\WDDRCRK.TIM;1", &crk_buf);
    if (crk_buf) {
        TIM_IMAGE tim;
        GetTimInfo((uint32_t *)crk_buf, &tim);
        LoadImage(tim.prect, tim.paddr);
        DrawSync(0);
        if (tim.mode & 0x8) {
            LoadImage(tim.crect, tim.caddr);
            DrawSync(0);
            fatdoor_crk_clut = getClut(tim.crect->x, tim.crect->y);
        }
        fatdoor_crk_tpage = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);
        free(crk_buf);
    }
}

void fatdoors_init(void) {
    int i = 0;

    /* The three kitchen entryways. Positions are each gap's centre point,
       Blender metres -> PS1 (x*100, -z*100, y*100). The door model is centred
       on its origin (±150 wide, ±188 tall, ±13 deep), so y=-188 stands it
       exactly floor (world y=0) to top (world y=-375). */

    /* South wall, east opening — thin axis along Z, no rotation. The depth
       half-width (30) is wider than the model's actual 13 so the camera is
       stopped well clear of the door face (avoids near-plane clipping). */
    fatdoors[i].x = 300;  fatdoors[i].y = -188; fatdoors[i].z = -1013;
    fatdoors[i].rot_y = 0;    fatdoors[i].half_x = 150; fatdoors[i].half_z = 30;
    fatdoors[i].state = FATDOOR_INTACT; fatdoors[i].active = 1;
    fatdoors[i].area = STATE_KITCHEN_DINING; i++;

    /* South wall, west opening — thin axis along Z, no rotation. */
    fatdoors[i].x = -300; fatdoors[i].y = -188; fatdoors[i].z = -1013;
    fatdoors[i].rot_y = 0;    fatdoors[i].half_x = 150; fatdoors[i].half_z = 30;
    fatdoors[i].state = FATDOOR_INTACT; fatdoors[i].active = 1;
    fatdoors[i].area = STATE_KITCHEN_DINING; i++;

    /* Big-room/dining partition doorway — rotated 90°, thin axis along X. */
    fatdoors[i].x = -615; fatdoors[i].y = -188; fatdoors[i].z = -375;
    fatdoors[i].rot_y = 1024; fatdoors[i].half_x = 30;  fatdoors[i].half_z = 150;
    fatdoors[i].state = FATDOOR_INTACT; fatdoors[i].active = 1;
    fatdoors[i].area = STATE_KITCHEN_DINING; i++;

    /* Reception: the small room behind the stairs. The doorway is the opening in
       the z~=512 wall between the x=900 and x=1200 jambs, so the door fills it
       along X (thin in Z), like the kitchen's south doors. */
    fatdoors[i].x = 1045; fatdoors[i].y = -189; fatdoors[i].z = 500;
    fatdoors[i].rot_y = 0;    fatdoors[i].half_x = 150; fatdoors[i].half_z = 30;
    fatdoors[i].state = FATDOOR_INTACT; fatdoors[i].active = 1;
    fatdoors[i].area = STATE_RECEPTION; i++;

    fatdoor_count = i;

    int j;
    for (j = 0; j < fatdoor_count; j++) {
        fatdoors[j].health = FATDOOR_MAX_HEALTH;
        fatdoor_defaults[j] = fatdoors[j];
    }
}

void fatdoors_reset(void) {
    int i;
    for (i = 0; i < fatdoor_count; i++)
        fatdoors[i] = fatdoor_defaults[i];
}

void fatdoors_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < fatdoor_count; i++) {
        FatDoor *d = &fatdoors[i];
        if (!d->active || d->state != FATDOOR_INTACT) continue;
        if (d->area != game_state) continue;

        /* Skip if the player is on a different vertical level to the door. */
        if (py < d->y - FATDOOR_HALF_H || py > d->y + FATDOOR_HALF_H) continue;

        int32_t min_x = d->x - d->half_x - radius - FATDOOR_PUSH_MARGIN;
        int32_t max_x = d->x + d->half_x + radius + FATDOOR_PUSH_MARGIN;
        int32_t min_z = d->z - d->half_z - radius - FATDOOR_PUSH_MARGIN;
        int32_t max_z = d->z + d->half_z + radius + FATDOOR_PUSH_MARGIN;

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

void fatdoors_draw(RenderContext *ctx) {
    if (!fatdoor_smd) return;

    /* View matrix (same computation as kitchen_dining_draw). */
    MATRIX view;
    SVECTOR neg_rot = {0, -cam_rot, 0, 0};
    RotMatrix(&neg_rot, &view);
    VECTOR vt = {-cam_x, -cam_y, -cam_z};
    ApplyMatrixLV(&view, &vt, &vt);
    view.t[0] = vt.vx;
    view.t[1] = vt.vy;
    view.t[2] = vt.vz;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    int d;
    for (d = 0; d < fatdoor_count; d++) {
        FatDoor *door = &fatdoors[d];
        if (!door->active || door->state == FATDOOR_SMASHED) continue;
        if (door->area != game_state) continue;

        int32_t ddx = door->x - cam_x, ddz = door->z - cam_z;
        int32_t ddist = (ddx < 0 ? -ddx : ddx) + (ddz < 0 ? -ddz : ddz);
        if (ddist > 2600) continue;

        /* Per-door fog matched to the kitchen renderer so doors blend in. */
        int32_t fog_start = 500, fog_end = 2200;
        int32_t fog = ddist < fog_start ? fog_start : (ddist > fog_end ? fog_end : ddist);
        int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

        /* Combine the view matrix with this door's world transform so the
           door-local vertices project straight to screen via the GTE. */
        MATRIX door_m, combined;
        SVECTOR dr = {0, door->rot_y, 0, 0};
        RotMatrix(&dr, &door_m);
        VECTOR pos = {door->x, door->y, door->z};
        TransMatrix(&door_m, &pos);
        CompMatrixLV(&view, &door_m, &combined);
        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        /* Manual draw loop — identical projection + OT bias to draw_kitchen_smd
           so the doors depth-sort correctly against the room geometry. */
        uint8_t *p = (uint8_t *)fatdoor_smd->p_prims;
        int i;
        for (i = 0; i < fatdoor_smd->n_prims; i++) {
            SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
            uint8_t stride = pt->len;
            int is_quad = (pt->type >= 2);

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &fatdoor_smd->p_verts[vi[0]];
            SVECTOR *v1 = &fatdoor_smd->p_verts[vi[1]];
            SVECTOR *v2 = &fatdoor_smd->p_verts[vi[2]];

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

            if (!pt->nocull) {
                gte_nclip();
                gte_stopz(&nclip);
                if (nclip <= 0) { p += stride; continue; }
            }

            gte_stsz4c(sz);
            if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

            if (is_quad) {
                SVECTOR *v3 = &fatdoor_smd->p_verts[vi[3]];
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
            otz += 40;
            if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

            uint8_t *col = p + 16;
            uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + 20 * (256 - fog_factor)) >> 8);
            uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + 15 * (256 - fog_factor)) >> 8);
            uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + 10 * (256 - fog_factor)) >> 8);

            if (is_quad && pt->texture) {
                if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
                /* FT4 stride 32: UVs at 20-27, tpage at 28, clut at 30 (baked
                   by smxlink from wd_dr.tim, matching the startup LoadImage). */
                uint8_t *uv = p + 20;
                POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
                setPolyFT4(poly);
                setRGB0(poly, r, g, b);
                /* On its final hit point, swap the baked intact texture for the
                   cracked one (if it loaded). UVs/texture window are unchanged. */
                if (door->health <= 1 && fatdoor_crk_tpage) {
                    poly->tpage = fatdoor_crk_tpage;
                    poly->clut  = fatdoor_crk_clut;
                } else {
                    poly->tpage = *(uint16_t *)(p + 28);
                    poly->clut  = *(uint16_t *)(p + 30);
                }
                poly->u0=uv[0]; poly->v0=uv[1];
                poly->u1=uv[2]; poly->v1=uv[3];
                poly->u2=uv[4]; poly->v2=uv[5];
                poly->u3=uv[6]; poly->v3=uv[7];
                poly->x0=sv[0].vx; poly->y0=sv[0].vy;
                poly->x1=sv[1].vx; poly->y1=sv[1].vy;
                poly->x2=sv[2].vx; poly->y2=sv[2].vy;
                poly->x3=sv[3].vx; poly->y3=sv[3].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_FT4);
            } else if (is_quad) {
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
    }

    /* Restore the plain view matrix so later world-space draws (particles,
       crucifaxe restore, etc.) project correctly. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}

int fatdoors_try_smash(void) {
    int i, smashed_any = 0;

    for (i = 0; i < fatdoor_count; i++) {
        FatDoor *d = &fatdoors[i];
        if (!d->active || d->state != FATDOOR_INTACT) continue;
        if (d->area != game_state) continue;

        if (cam_y < d->y - FATDOOR_HALF_H || cam_y > d->y + FATDOOR_HALF_H) continue;

        int32_t min_x = d->x - d->half_x - FATDOOR_SMASH_RANGE;
        int32_t max_x = d->x + d->half_x + FATDOOR_SMASH_RANGE;
        int32_t min_z = d->z - d->half_z - FATDOOR_SMASH_RANGE;
        int32_t max_z = d->z + d->half_z + FATDOOR_SMASH_RANGE;

        if (cam_x < min_x || cam_x > max_x) continue;
        if (cam_z < min_z || cam_z > max_z) continue;

        /* Reject if the door is behind the player. */
        int32_t cdx = (d->x - cam_x) >> 4;
        int32_t cdz = (d->z - cam_z) >> 4;
        int32_t dot = cdx * (isin(cam_rot) >> 4) + cdz * (icos(cam_rot) >> 4);
        if (dot <= 0) continue;

        /* One hit per swing (caller guards re-entry). Wood chips every hit;
           the hit/hurt sound on a non-destroying hit, smash only on the last. */
        d->health--;
        spawn_wood_burst(d->x, d->y, d->z);
        if (d->health <= 0) {
            d->state = FATDOOR_SMASHED;
            sound_play(SFX_SMASH);
        } else {
            sound_play(SFX_AXEHIT);
        }
        smashed_any = 1;
    }
    return smashed_any;
}

int fatdoors_damage_at(int32_t x, int32_t z, int32_t reach, int amount) {
    int i;
    for (i = 0; i < fatdoor_count; i++) {
        FatDoor *d = &fatdoors[i];
        if (!d->active || d->state != FATDOOR_INTACT) continue;
        if (d->area != game_state) continue;

        int32_t min_x = d->x - d->half_x - reach;
        int32_t max_x = d->x + d->half_x + reach;
        int32_t min_z = d->z - d->half_z - reach;
        int32_t max_z = d->z + d->half_z + reach;

        if (x < min_x || x > max_x) continue;
        if (z < min_z || z > max_z) continue;

        d->health -= amount;
        spawn_wood_burst(d->x, d->y, d->z);
        if (d->health <= 0) {
            d->state = FATDOOR_SMASHED;
            sound_play(SFX_SMASH);
            return 2;
        }
        sound_play(SFX_AXEHIT);
        return 1;
    }
    return 0;
}
