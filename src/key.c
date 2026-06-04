#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "key.h"

KeyPickup keys[MAX_KEYS];
int       key_count = 0;

#define KEY_FLOAT_Y    50   /* units below crate origin — floats above ground */
#define KEY_BOB_RATE   16   /* vertical bob speed */
#define KEY_BOB_AMP    18   /* bob amplitude in world units */
#define KEY_WORLD_HALF 60   /* half-size in world units for depth scaling */

static uint16_t key_tpage = 0;
static uint16_t key_clut  = 0;
static uint8_t  key_u0, key_v0, key_u1, key_v1;

void keys_init(void) {
    keys_reset();

    CdlFILE file;
    if (!CdSearchFile(&file, "\\KEY.TIM;1")) return;

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
        key_clut = getClut(tim.crect->x, tim.crect->y);
    }

    key_tpage = getTPage(tim.mode & 3, 0, tim.prect->x, tim.prect->y);

    /* UV within the tpage — V must be offset by the VRAM y of the texture
       since the tpage origin is at y=0 of that 256-line page block. */
    int bpp_mode = tim.mode & 3;
    int px_mult  = (bpp_mode == 0) ? 4 : (bpp_mode == 1) ? 2 : 1;
    int tex_w    = tim.prect->w * px_mult;
    int tex_h    = tim.prect->h;
    key_u0 = 0;
    key_v0 = (uint8_t)(tim.prect->y % 256);
    key_u1 = (uint8_t)(tex_w - 1);
    key_v1 = (uint8_t)(key_v0 + tex_h - 1);

    free(buf);
}

void keys_reset(void) {
    int i;
    for (i = 0; i < MAX_KEYS; i++)
        keys[i].active = 0;
    key_count = 0;
}

void key_spawn(int32_t x, int32_t y, int32_t z) {
    int i;
    for (i = 0; i < MAX_KEYS; i++) {
        if (!keys[i].active) {
            keys[i].x           = x;
            keys[i].y           = y + KEY_FLOAT_Y;
            keys[i].z           = z;
            keys[i].spin_angle  = 0;
            keys[i].active      = 1;
            if (i >= key_count) key_count = i + 1;
            return;
        }
    }
}

void keys_update(void) {
    int i;
    for (i = 0; i < key_count; i++) {
        KeyPickup *k = &keys[i];
        if (!k->active) continue;

        k->spin_angle = (k->spin_angle + KEY_BOB_RATE) & 4095;

        int32_t dx   = k->x - cam_x;
        int32_t dz   = k->z - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (dist < KEY_PICKUP_RADIUS) {
            k->active = 0;
            player_has_key++;
        }
    }
}

void keys_draw(RenderContext *ctx) {
    if (!key_tpage) return;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    int i;
    for (i = 0; i < key_count; i++) {
        KeyPickup *k = &keys[i];
        if (!k->active) continue;

        int32_t dx = k->x - cam_x;
        int32_t dz = k->z - cam_z;
        if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 3000) continue;

        /* Vertical bob using spin_angle */
        int32_t bob = (isin(k->spin_angle) * KEY_BOB_AMP) >> 12;
        SVECTOR sv = {(int16_t)k->x, (int16_t)(k->y + bob), (int16_t)k->z, 0};

        DVECTOR screen;
        int32_t otz;

        gte_ldv0(&sv);
        gte_rtps();
        gte_stsxy(&screen);
        gte_avsz3();
        gte_stotz(&otz);

        if (otz <= 0 || otz >= OT_LENGTH) continue;
        if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) continue;

        /* Size from world-space distance — avoids stale GTE SZ registers
           that made the OTZ-based formula pin the sprite to the camera. */
        int32_t wdist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (wdist < 1) wdist = 1;
        int32_t half = (KEY_WORLD_HALF * 256) / wdist;
        if (half < 4)  half = 4;
        if (half > 60) half = 60;

        /* POLY_FT4 screen-space billboard — maps the full texture correctly */
        POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
        setPolyFT4(poly);
        setRGB0(poly, 128, 128, 128);  /* neutral: texture at full brightness */

        poly->x0 = (int16_t)(screen.vx - half); poly->y0 = (int16_t)(screen.vy - half);
        poly->x1 = (int16_t)(screen.vx + half); poly->y1 = (int16_t)(screen.vy - half);
        poly->x2 = (int16_t)(screen.vx - half); poly->y2 = (int16_t)(screen.vy + half);
        poly->x3 = (int16_t)(screen.vx + half); poly->y3 = (int16_t)(screen.vy + half);

        poly->u0 = key_u0; poly->v0 = key_v0;
        poly->u1 = key_u1; poly->v1 = key_v0;
        poly->u2 = key_u0; poly->v2 = key_v1;
        poly->u3 = key_u1; poly->v3 = key_v1;
        poly->clut  = key_clut;
        poly->tpage = key_tpage;  /* POLY_FT4 embeds tpage in the primitive itself */

        ctx->next_packet += sizeof(POLY_FT4);

        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
    }
}
