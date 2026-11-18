#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "spirit_glow.h"

/* See spirit_glow.h. The green is the one Hadad's spirit always had; the blue
   and the yellow are pitched to the same strength so no ball reads brighter
   than another when they fly side by side into Destiny's Door. */
const uint8_t SPIRIT_RGB_ADAPA[3] = {  60, 130, 255 };   /* blue   */
const uint8_t SPIRIT_RGB_HADAD[3] = {  40, 255, 120 };   /* green  */
const uint8_t SPIRIT_RGB_GULA[3]  = { 255, 215,  40 };   /* yellow */

#define SG_RINGS 3

/* lumberer.c's integer square root, with the `t > 0` seed. */
static int32_t sg_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x, last, s = 1, t = v;
    while (t > 0) { t >>= 2; s <<= 1; }
    x = s;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

int32_t spirit_glow_pulse(int32_t clock) {
    return 224 + (isin((clock * 40) & 4095) >> 6);
}

void spirit_glow_draw_sized(RenderContext *ctx, int32_t x, int32_t y,
                            int32_t z, int32_t bright, const uint8_t rgb[3],
                            int32_t world_outer) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (bright <= 0) return;

    SVECTOR pt;
    pt.vx = (int16_t)x; pt.vy = (int16_t)y; pt.vz = (int16_t)z; pt.pad = 0;

    DVECTOR sv;
    int32_t sz;
    gte_ldv0(&pt);
    gte_rtps();
    gte_stsxy(&sv);
    gte_stsz(&sz);
    if (sz == 0) return;
    if (sv.vx <= -1023 || sv.vx >= 1023 || sv.vy <= -1023 || sv.vy >= 1023) return;

    int32_t dx = x - cam_x, dy = y - cam_y, dz = z - cam_z;
    int32_t dist = sg_isqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 64) dist = 64;

    int32_t otz = sz >> 2;
    if (otz <= SCENE_OT_MIN)  otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;

    int ring;
    for (ring = 0; ring < SG_RINGS; ring++) {
        if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) return;

        int32_t world_half = world_outer >> ring;
        int32_t half = (world_half * 256) / dist;   /* gte_SetGeomScreen(256) */
        if (half < 1)   half = 1;
        if (half > 400) half = 400;

        int32_t level = (bright * (60 + ring * 70)) >> 8;
        if (level > 256) level = 256;
        if (level <= 0)  continue;

        POLY_F4 *p = (POLY_F4 *)ctx->next_packet;
        setPolyF4(p);
        setSemiTrans(p, 1);
        setRGB0(p, (uint8_t)((rgb[0] * level) >> 8),
                   (uint8_t)((rgb[1] * level) >> 8),
                   (uint8_t)((rgb[2] * level) >> 8));
        p->x0 = (int16_t)(sv.vx - half); p->y0 = (int16_t)(sv.vy - half);
        p->x1 = (int16_t)(sv.vx + half); p->y1 = (int16_t)(sv.vy - half);
        p->x2 = (int16_t)(sv.vx - half); p->y2 = (int16_t)(sv.vy + half);
        p->x3 = (int16_t)(sv.vx + half); p->y3 = (int16_t)(sv.vy + half);
        addPrim(&ot[otz], p);
        ctx->next_packet += sizeof(POLY_F4);

        DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
        addPrim(&ot[otz], tp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
}

void spirit_glow_draw(RenderContext *ctx, int32_t x, int32_t y, int32_t z,
                      int32_t bright, const uint8_t rgb[3]) {
    spirit_glow_draw_sized(ctx, x, y, z, bright, rgb, SPIRIT_GLOW_WORLD);
}
