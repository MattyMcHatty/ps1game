#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "sml_med.h"
#include "sound.h"

SmlMed sml_meds[MAX_SML_MEDS];
int    sml_med_count = 0;

#define SML_MED_FLOAT_Y    50   /* units below crate origin so it sits above the floor */
#define SML_MED_BOB_RATE   16
#define SML_MED_BOB_AMP    18
#define SML_MED_WORLD_HALF 80   /* half-size in world units for depth scaling */
/* Forward OT bias (subtracted from true depth). Crates sort at true depth
   (zoff=0), so an unbiased pickup spawned at a crate's centre z-fights with
   and loses to neighbouring crates. A small forward nudge pulls the pickup in
   front of co-located crates. Kept well under the doors' +40 bias so a
   fat_door genuinely in front of the pickup still occludes it. */
#define SML_MED_OT_BIAS    24

static uint16_t sml_med_tpage = 0;
static uint16_t sml_med_clut  = 0;
static uint8_t  sml_med_u0, sml_med_v0, sml_med_u1, sml_med_v1;

void sml_meds_init(void) {
    sml_meds_reset();

    CdlFILE file;
    if (!CdSearchFile(&file, "\\SML_MED.TIM;1")) return;

    int   sectors = (file.size + 2047) / 2048;
    void *buf     = malloc(sectors * 2048);
    if (!buf) return;

    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);

    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);

    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);

    if (tim.mode & 8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
        sml_med_clut = getClut(tim.crect->x, tim.crect->y);
    }

    sml_med_tpage = getTPage(tim.mode & 3, 0, tim.prect->x, tim.prect->y);

    int bpp_mode = tim.mode & 3;
    int px_mult  = (bpp_mode == 0) ? 4 : (bpp_mode == 1) ? 2 : 1;
    int tex_w    = tim.prect->w * px_mult;
    int tex_h    = tim.prect->h;
    sml_med_u0 = 0;
    sml_med_v0 = (uint8_t)(tim.prect->y % 256);
    sml_med_u1 = (uint8_t)(tex_w - 1);
    sml_med_v1 = (uint8_t)(sml_med_v0 + tex_h - 1);

    free(buf);
}

void sml_meds_reset(void) {
    int i;
    for (i = 0; i < MAX_SML_MEDS; i++)
        sml_meds[i].active = 0;
    sml_med_count = 0;
}

void sml_med_spawn(int32_t x, int32_t y, int32_t z) {
    int i;
    for (i = 0; i < MAX_SML_MEDS; i++) {
        if (!sml_meds[i].active) {
            sml_meds[i].x         = x;
            sml_meds[i].y         = y + SML_MED_FLOAT_Y;
            sml_meds[i].z         = z;
            sml_meds[i].bob_angle = 0;
            sml_meds[i].active    = 1;
            if (i >= sml_med_count) sml_med_count = i + 1;
            return;
        }
    }
}

void sml_meds_update(void) {
    int i;
    for (i = 0; i < sml_med_count; i++) {
        SmlMed *m = &sml_meds[i];
        if (!m->active) continue;

        m->bob_angle = (m->bob_angle + SML_MED_BOB_RATE) & 4095;

        int32_t dx   = m->x - cam_x;
        int32_t dz   = m->z - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (dist < SML_MED_PICKUP_RADIUS) {
            m->active      = 0;
            player_health += MAX_HEALTH / 4;
            if (player_health > MAX_HEALTH)
                player_health = MAX_HEALTH;
            sound_play(SFX_PICKUP);
            show_pickup_msg("Small Medipac");
        }
    }
}

void sml_meds_draw(RenderContext *ctx) {
    if (!sml_med_tpage) return;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    int i;
    for (i = 0; i < sml_med_count; i++) {
        SmlMed *m = &sml_meds[i];
        if (!m->active) continue;

        int32_t dx = m->x - cam_x;
        int32_t dz = m->z - cam_z;
        if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= g_fog_far) continue;

        int32_t bob = (isin(m->bob_angle) * SML_MED_BOB_AMP) >> 12;
        SVECTOR sv  = {(int16_t)m->x, (int16_t)(m->y + bob), (int16_t)m->z, 0};

        DVECTOR screen;
        int32_t otz;

        gte_ldv0(&sv); gte_rtps();
        gte_ldv0(&sv); gte_rtps();
        gte_ldv0(&sv); gte_rtps();
        gte_stsxy(&screen);
        gte_avsz3();
        gte_stotz(&otz);
        /* Sort at true scene depth so the pickup is occluded by walls/doors in
           front of it (e.g. the kitchen fat_doors). World geometry is biased
           +40 into the OT, so an unbiased pickup still draws over the floor /
           surface it floats above without z-fighting. */
        /* otz == 0 means the point projected at/behind the camera's near
           plane. Without this reject the clamp below would force a sprite
           that's behind the player up to the front of the OT, drawing it
           through the floor in front of the camera. Mirrors the behind-camera
           cull in the demondog/crucifaxe billboards. */
        if (otz <= 0) continue;
        otz -= SML_MED_OT_BIAS;   /* draw in front of co-located crates */
        if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;
        if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) continue;

        int32_t wdist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (wdist < 1) wdist = 1;
        int32_t half = (SML_MED_WORLD_HALF * 256) / wdist;
        if (half < 4)  half = 4;
        if (half > 80) half = 80;

        POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
        setPolyFT4(poly);
        {   /* distance fog to match the room mesh */
            uint8_t fc = (uint8_t)((128 * render_fog_scale(wdist)) >> 8);
            setRGB0(poly, fc, fc, fc);
        }

        poly->x0 = (int16_t)(screen.vx - half); poly->y0 = (int16_t)(screen.vy - half);
        poly->x1 = (int16_t)(screen.vx + half); poly->y1 = (int16_t)(screen.vy - half);
        poly->x2 = (int16_t)(screen.vx - half); poly->y2 = (int16_t)(screen.vy + half);
        poly->x3 = (int16_t)(screen.vx + half); poly->y3 = (int16_t)(screen.vy + half);

        poly->u0 = sml_med_u0; poly->v0 = sml_med_v0;
        poly->u1 = sml_med_u1; poly->v1 = sml_med_v0;
        poly->u2 = sml_med_u0; poly->v2 = sml_med_v1;
        poly->u3 = sml_med_u1; poly->v3 = sml_med_v1;
        poly->clut  = sml_med_clut;
        poly->tpage = sml_med_tpage;

        ctx->next_packet += sizeof(POLY_FT4);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
    }
}
