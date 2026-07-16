#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "bullet_hit.h"

static BulletHit hits[MAX_BULLET_HITS];

#define BULLET_HIT_LIFE   10   /* frames the sprite shows (brief) */
#define BULLET_HIT_HALF   22   /* world-space half size for depth scaling */

/* VRAM sprite handle (ghit.tim). */
static uint16_t ghit_tpage = 0;
static uint16_t ghit_clut  = 0;
static uint8_t  ghit_u0, ghit_v0, ghit_u1, ghit_v1;

void bullet_hits_load_texture(void) {
    CdlFILE file;
    if (!CdSearchFile(&file, "\\TEX\\GHIT.TIM;1")) return;
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
    if (tim.mode & 0x8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
        ghit_clut = getClut(tim.crect->x, tim.crect->y);
    }
    ghit_tpage = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);

    int bpp_mode = tim.mode & 3;
    int px_mult  = (bpp_mode == 0) ? 4 : (bpp_mode == 1) ? 2 : 1;
    int tex_w    = tim.prect->w * px_mult;
    int tex_h    = tim.prect->h;
    int u_off    = (tim.prect->x & 63) * px_mult;   /* x offset within the tpage */
    ghit_u0 = (uint8_t)u_off;
    ghit_v0 = (uint8_t)(tim.prect->y % 256);
    ghit_u1 = (uint8_t)(u_off + tex_w - 1);
    ghit_v1 = (uint8_t)(ghit_v0 + tex_h - 1);

    free(buf);
}

void bullet_hits_reset(void) {
    int i;
    for (i = 0; i < MAX_BULLET_HITS; i++) hits[i].life = 0;
}

void bullet_hit_spawn(int32_t x, int32_t y, int32_t z) {
    int i;
    for (i = 0; i < MAX_BULLET_HITS; i++) {
        if (hits[i].life <= 0) {
            hits[i].x = x; hits[i].y = y; hits[i].z = z;
            hits[i].life = hits[i].max_life = BULLET_HIT_LIFE;
            return;
        }
    }
}

void bullet_hits_update(void) {
    int i;
    for (i = 0; i < MAX_BULLET_HITS; i++)
        if (hits[i].life > 0) hits[i].life--;
}

void bullet_hits_draw(RenderContext *ctx) {
    if (!ghit_tpage) return;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    int i;
    for (i = 0; i < MAX_BULLET_HITS; i++) {
        BulletHit *h = &hits[i];
        if (h->life <= 0) continue;

        SVECTOR sv = {(int16_t)h->x, (int16_t)h->y, (int16_t)h->z, 0};
        DVECTOR screen;
        int32_t otz;
        gte_ldv0(&sv);
        gte_rtps();
        gte_stsxy(&screen);
        gte_avsz3();
        gte_stotz(&otz);
        if (otz <= 0 || otz >= OT_LENGTH) continue;
        if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) continue;

        int32_t dx = h->x - cam_x, dz = h->z - cam_z;
        int32_t wdist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (wdist < 1) wdist = 1;
        int32_t half = (BULLET_HIT_HALF * 256) / wdist;
        if (half < 1)  half = 1;
        if (half > 24) half = 24;

        POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
        setPolyFT4(poly);
        setRGB0(poly, 128, 128, 128);   /* neutral: texture at full brightness */
        poly->x0 = (int16_t)(screen.vx - half); poly->y0 = (int16_t)(screen.vy - half);
        poly->x1 = (int16_t)(screen.vx + half); poly->y1 = (int16_t)(screen.vy - half);
        poly->x2 = (int16_t)(screen.vx - half); poly->y2 = (int16_t)(screen.vy + half);
        poly->x3 = (int16_t)(screen.vx + half); poly->y3 = (int16_t)(screen.vy + half);
        poly->u0 = ghit_u0; poly->v0 = ghit_v0;
        poly->u1 = ghit_u1; poly->v1 = ghit_v0;
        poly->u2 = ghit_u0; poly->v2 = ghit_v1;
        poly->u3 = ghit_u1; poly->v3 = ghit_v1;
        poly->clut  = ghit_clut;
        poly->tpage = ghit_tpage;
        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
        ctx->next_packet += sizeof(POLY_FT4);
    }
}
