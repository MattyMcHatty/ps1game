#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxcd.h>
#include "anzu_tex.h"

static AnzuTex tiles[ANZU_TILE_COUNT];

static const char * const tile_tim[ANZU_TILE_COUNT] = {
    "\\TEX\\ANZU1.TIM;1",
    "\\TEX\\ANZU2.TIM;1",
    "\\TEX\\ANZU3.TIM;1",
    "\\TEX\\ANZU4.TIM;1",
    "\\TEX\\ANZU5.TIM;1",
    "\\TEX\\ANZU6.TIM;1",
};

/* Read a TIM, LoadImage it into the slot baked into its own header, and record
   the handles needed to draw it. Same derivation as item_pickup.c's
   load_sprite: U0 is the texture's x WITHIN its 64-word tpage, so a tile in the
   right half of a page still maps correctly. The RAM buffer is freed — these
   slots are never re-uploaded. */
static void load_tile(const char *filename, AnzuTex *t) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)filename)) return;
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
        t->clut = getClut(tim.crect->x, tim.crect->y);
    }
    t->tpage = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);

    int bpp_mode = tim.mode & 3;
    int px_mult  = (bpp_mode == 0) ? 4 : (bpp_mode == 1) ? 2 : 1;  /* px per word */
    t->u0 = (uint8_t)((tim.prect->x & 63) * px_mult);
    t->v0 = (uint8_t)(tim.prect->y % 256);

    free(buf);
}

void anzu_tex_load(void) {
    int i;
    for (i = 0; i < ANZU_TILE_COUNT; i++)
        load_tile(tile_tim[i], &tiles[i]);
}

const AnzuTex *anzu_tex(int tile) {
    if (tile < 0 || tile >= ANZU_TILE_COUNT) tile = 0;
    return &tiles[tile];
}
