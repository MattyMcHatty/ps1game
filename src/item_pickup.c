#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "item_pickup.h"
#include "sound.h"

ItemPickup item_pickups[MAX_ITEM_PICKUPS];
int        item_pickup_count = 0;

#define IP_FLOAT_Y     50   /* units above the spawn point — floats above ground */
#define IP_BOB_RATE    16   /* vertical bob speed */
#define IP_BOB_AMP     18   /* bob amplitude in world units */
#define IP_WORLD_HALF  70   /* half-size in world units for depth scaling */

/* Per-kind VRAM sprite handle, filled at startup. */
typedef struct {
    uint16_t tpage, clut;
    uint8_t  u0, v0, u1, v1;
} Sprite;

static Sprite sprites[PICKUP_KIND_COUNT];

/* Disc paths + display names, indexed by PickupKind. */
static const char * const kind_tim[PICKUP_KIND_COUNT] = {
    "\\TEX\\GRAVOLVR.TIM;1",   /* PICKUP_GRAVEOLVER */
    "\\TEX\\STNDRNDS.TIM;1",   /* PICKUP_ROUNDS     */
};
static const char * const kind_name[PICKUP_KIND_COUNT] = {
    "Grave-olver",             /* PICKUP_GRAVEOLVER */
    "Rounds",                  /* PICKUP_ROUNDS     */
};

/* Load one TIM into VRAM and record its tpage/clut and UV rect. The UV's U0 is
   derived from the texture's x WITHIN its 64-word tpage, so a sprite may sit in
   the right half of a page (VRAM x not a multiple of 64) and still map correctly
   — the tpage snaps to the page's left edge and U0 offsets into it. */
static void load_sprite(const char *filename, Sprite *s) {
    CdlFILE file;
    if (!CdSearchFile(&file, filename)) return;
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
        s->clut = getClut(tim.crect->x, tim.crect->y);
    }
    s->tpage = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);

    int bpp_mode = tim.mode & 3;
    int px_mult  = (bpp_mode == 0) ? 4 : (bpp_mode == 1) ? 2 : 1;  /* px per 16-bit word */
    int tex_w    = tim.prect->w * px_mult;
    int tex_h    = tim.prect->h;
    int u_off    = (tim.prect->x & 63) * px_mult;  /* x offset within the 64-word page */
    s->u0 = (uint8_t)u_off;
    s->v0 = (uint8_t)(tim.prect->y % 256);
    s->u1 = (uint8_t)(u_off + tex_w - 1);
    s->v1 = (uint8_t)(s->v0 + tex_h - 1);

    free(buf);
}

void item_pickups_load_textures(void) {
    int k;
    for (k = 0; k < PICKUP_KIND_COUNT; k++)
        load_sprite(kind_tim[k], &sprites[k]);
}

void item_pickups_reset(void) {
    int i;
    for (i = 0; i < MAX_ITEM_PICKUPS; i++)
        item_pickups[i].active = 0;
    item_pickup_count = 0;
    player_weapons = (1 << WEAPON_CRUCIFAXE);
    player_rounds  = 0;
}

int item_pickup_spawn(int32_t x, int32_t y, int32_t z, PickupKind kind) {
    int i;
    for (i = 0; i < MAX_ITEM_PICKUPS; i++) {
        if (!item_pickups[i].active) {
            item_pickups[i].x         = x;
            item_pickups[i].y         = y - IP_FLOAT_Y;   /* -Y is up */
            item_pickups[i].z         = z;
            item_pickups[i].bob_angle = 0;
            item_pickups[i].kind      = kind;
            item_pickups[i].active    = 1;
            if (i >= item_pickup_count) item_pickup_count = i + 1;
            return i;
        }
    }
    return -1;
}

static void collect(ItemPickup *p) {
    switch (p->kind) {
        case PICKUP_GRAVEOLVER: player_weapons |= (1 << WEAPON_GRAVEOLVER); break;
        case PICKUP_ROUNDS:     player_rounds  += ROUNDS_PER_PICKUP;        break;
        default: break;
    }
    sound_play(SFX_PICKUP);
    show_pickup_msg(kind_name[p->kind]);
}

void item_pickups_update(void) {
    int i;
    for (i = 0; i < item_pickup_count; i++) {
        ItemPickup *p = &item_pickups[i];
        if (!p->active) continue;

        p->bob_angle = (p->bob_angle + IP_BOB_RATE) & 4095;

        int32_t dx   = p->x - cam_x;
        int32_t dz   = p->z - cam_z;
        int32_t dy   = cam_y - p->y;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (dist < ITEM_PICKUP_RADIUS &&
            (dy < 0 ? -dy : dy) < ITEM_PICKUP_HEIGHT) {
            p->active = 0;
            collect(p);
        }
    }
}

void item_pickups_draw(RenderContext *ctx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    int i;
    for (i = 0; i < item_pickup_count; i++) {
        ItemPickup *p = &item_pickups[i];
        if (!p->active) continue;

        Sprite *s = &sprites[p->kind];
        if (!s->tpage) continue;

        int32_t dx = p->x - cam_x;
        int32_t dz = p->z - cam_z;
        int32_t wdist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (wdist > 3000) continue;

        int32_t bob = (isin(p->bob_angle) * IP_BOB_AMP) >> 12;
        SVECTOR sv = {(int16_t)p->x, (int16_t)(p->y + bob), (int16_t)p->z, 0};

        /* Project the sprite centre three times so SZ0..2 are all this point's
           depth, then average to its true OTZ. Sorting at the real depth (same
           scale the room SMD and fat door use) lets nearer geometry — a wall or
           the fat door — occlude the pickup instead of it drawing on top. The
           room geometry biases itself +40 back, so a pickup at the same depth as
           the floor beneath it still shows above the floor. */
        DVECTOR screen;
        int32_t otz;
        gte_ldv0(&sv); gte_rtps();
        gte_ldv0(&sv); gte_rtps();
        gte_ldv0(&sv); gte_rtps();
        gte_stsxy(&screen);
        gte_avsz3();
        gte_stotz(&otz);
        if (otz < SCENE_OT_MIN)     otz = SCENE_OT_MIN;
        if (otz > OT_LENGTH - 2)    otz = OT_LENGTH - 2;
        if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) continue;

        if (wdist < 1) wdist = 1;
        int32_t half = (IP_WORLD_HALF * 256) / wdist;
        if (half < 4)  half = 4;
        if (half > 70) half = 70;

        POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
        setPolyFT4(poly);
        setRGB0(poly, 128, 128, 128);   /* neutral: texture at full brightness */

        poly->x0 = (int16_t)(screen.vx - half); poly->y0 = (int16_t)(screen.vy - half);
        poly->x1 = (int16_t)(screen.vx + half); poly->y1 = (int16_t)(screen.vy - half);
        poly->x2 = (int16_t)(screen.vx - half); poly->y2 = (int16_t)(screen.vy + half);
        poly->x3 = (int16_t)(screen.vx + half); poly->y3 = (int16_t)(screen.vy + half);

        poly->u0 = s->u0; poly->v0 = s->v0;
        poly->u1 = s->u1; poly->v1 = s->v0;
        poly->u2 = s->u0; poly->v2 = s->v1;
        poly->u3 = s->u1; poly->v3 = s->v1;
        poly->clut  = s->clut;
        poly->tpage = s->tpage;

        ctx->next_packet += sizeof(POLY_FT4);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
    }
}
