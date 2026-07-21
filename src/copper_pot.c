#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "copper_pot.h"
#include "sound.h"

/* World placement: north-east corner of the conservatory's northern room area
   (east bound is the x=-901 divider wall, north wall at z=1702). Floats above
   the floor (world y=0) and bobs, like the key / item pickups. */
#define CP_X            (-1000)
#define CP_Y             (-110)   /* sprite centre, world Y (-Y is up)          */
#define CP_Z             1600
#define CP_PICKUP_RADIUS  220     /* Manhattan XZ reach                         */
#define CP_PICKUP_HEIGHT  200     /* vertical reach                             */
#define CP_BOB_RATE        16
#define CP_BOB_AMP         18
#define CP_WORLD_HALF      70     /* half-size in world units for depth scaling */

/* The conservatory's texture window (128x128), restored around the sprite so
   the pot — which sits at VRAM Voff 128 (the key slot) — samples its true
   region instead of wrapping mod-128 (same bracket zombie sprites use). */
#define CP_TW_MASK (128 >> 3)

static uint8_t  *cp_buf   = NULL;   /* resident RAM copy of the TIM (for re-upload) */
static TIM_IMAGE cp_tim;
static uint16_t  cp_tpage = 0, cp_clut = 0;
static uint8_t   cp_u0, cp_v0, cp_u1, cp_v1;
static int       cp_bob   = 0;

static int owned(void) { return player_items & (1 << ITEM_COPPER_POT); }

void copper_pot_load_assets(void) {
    CdlFILE file;
    if (!CdSearchFile(&file, "\\TEX\\CPPRPOT.TIM;1")) return;
    int   sectors = (file.size + 2047) / 2048;
    cp_buf        = malloc(sectors * 2048);
    if (!cp_buf) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)cp_buf, CdlModeSpeed);
    CdReadSync(0, NULL);

    GetTimInfo((uint32_t *)cp_buf, &cp_tim);
    /* Capture handle/UV WITHOUT LoadImage — uploading now would overwrite the
       key texture that shares this VRAM slot and is still needed early game. */
    if (cp_tim.mode & 0x8)
        cp_clut = getClut(cp_tim.crect->x, cp_tim.crect->y);
    cp_tpage = getTPage(cp_tim.mode & 0x3, 0, cp_tim.prect->x, cp_tim.prect->y);

    int bpp_mode = cp_tim.mode & 3;
    int px_mult  = (bpp_mode == 0) ? 4 : (bpp_mode == 1) ? 2 : 1;
    int tex_w    = cp_tim.prect->w * px_mult;
    int tex_h    = cp_tim.prect->h;
    int u_off    = (cp_tim.prect->x & 63) * px_mult;
    cp_u0 = (uint8_t)u_off;
    cp_v0 = (uint8_t)(cp_tim.prect->y % 256);
    cp_u1 = (uint8_t)(u_off + tex_w - 1);
    cp_v1 = (uint8_t)(cp_v0 + tex_h - 1);
}

void copper_pot_upload_texture(void) {
    if (!cp_buf) return;
    LoadImage(cp_tim.prect, cp_tim.paddr);
    DrawSync(0);
    if (cp_tim.mode & 0x8) {
        LoadImage(cp_tim.crect, cp_tim.caddr);
        DrawSync(0);
    }
}

void copper_pot_reset(void) {
    player_items &= ~(1 << ITEM_COPPER_POT);
}

int copper_pot_owned(void) { return owned() ? 1 : 0; }

void copper_pot_icon(uint16_t *tpage, uint16_t *clut,
                     uint8_t *u0, uint8_t *v0, uint8_t *u1, uint8_t *v1) {
    *tpage = cp_tpage; *clut = cp_clut;
    *u0 = cp_u0; *v0 = cp_v0; *u1 = cp_u1; *v1 = cp_v1;
}

void copper_pot_update(void) {
    if (owned()) return;
    cp_bob = (cp_bob + CP_BOB_RATE) & 4095;

    int32_t dx = cam_x - CP_X;
    int32_t dz = cam_z - CP_Z;
    int32_t dy = cam_y - CP_Y;
    int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (dist < CP_PICKUP_RADIUS && (dy < 0 ? -dy : dy) < CP_PICKUP_HEIGHT) {
        player_items |= (1 << ITEM_COPPER_POT);
        sound_play(SFX_PICKUP);
        show_pickup_msg("Copper Pot");
    }
}

void copper_pot_draw(RenderContext *ctx) {
    if (owned() || !cp_tpage) return;

    int32_t dx = CP_X - cam_x;
    int32_t dz = CP_Z - cam_z;
    int32_t wdist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (wdist >= g_fog_far) return;
    /* Reject behind the camera (else the GTE mirrors it to the front). */
    if (((dx * isin(cam_rot) + dz * icos(cam_rot)) >> 12) <= 0) return;

    int32_t bob = (isin(cp_bob) * CP_BOB_AMP) >> 12;
    SVECTOR sv = {(int16_t)CP_X, (int16_t)(CP_Y + bob), (int16_t)CP_Z, 0};

    DVECTOR screen;
    int32_t otz;
    gte_ldv0(&sv); gte_rtps();
    gte_ldv0(&sv); gte_rtps();
    gte_ldv0(&sv); gte_rtps();
    gte_stsxy(&screen);
    gte_avsz3();
    gte_stotz(&otz);
    if (otz < SCENE_OT_MIN)  otz = SCENE_OT_MIN;
    if (otz > OT_LENGTH - 2)  otz = OT_LENGTH - 2;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    /* Need room for the sprite plus its two window-bracket DR_TWINs. */
    if (ctx->next_packet + sizeof(POLY_FT4) + 2 * sizeof(DR_TWIN) > buf_end) return;

    if (wdist < 1) wdist = 1;
    int32_t half = (CP_WORLD_HALF * 256) / wdist;
    if (half < 4)  half = 4;
    if (half > 70) half = 70;

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
    poly->u0 = cp_u0; poly->v0 = cp_v0;
    poly->u1 = cp_u1; poly->v1 = cp_v0;
    poly->u2 = cp_u0; poly->v2 = cp_v1;
    poly->u3 = cp_u1; poly->v3 = cp_v1;
    poly->clut  = cp_clut;
    poly->tpage = cp_tpage;
    ctx->next_packet += sizeof(POLY_FT4);

    /* Window bracket (addPrim prepends, so this yields draw order:
       disable-window -> sprite -> restore-conservatory-window). */
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    RECT restore = { 0, 0, CP_TW_MASK, CP_TW_MASK };
    DR_TWIN *tw_restore = (DR_TWIN *)ctx->next_packet;
    setTexWindow(tw_restore, &restore);
    addPrim(&ot[otz], tw_restore);
    ctx->next_packet += sizeof(DR_TWIN);

    addPrim(&ot[otz], poly);

    RECT full = { 0, 0, 0, 0 };
    DR_TWIN *tw_disable = (DR_TWIN *)ctx->next_packet;
    setTexWindow(tw_disable, &full);
    addPrim(&ot[otz], tw_disable);
    ctx->next_packet += sizeof(DR_TWIN);
}
