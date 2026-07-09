#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxcd.h>
#include "texmgr.h"

#define TEXMGR_MAX 32

typedef struct {
    uint8_t  *buf;    /* resident RAM copy of the whole TIM (kept for re-upload) */
    TIM_IMAGE tim;    /* parsed header; prect/paddr/crect/caddr point into buf   */
    uint16_t  tpage;
    uint16_t  clut;
    int       valid;
} TexEntry;

static TexEntry entries[TEXMGR_MAX];
static int      entry_count = 0;

int texmgr_register(const char *filename) {
    if (entry_count >= TEXMGR_MAX) return -1;

    CdlFILE file;
    if (!CdSearchFile(&file, (char *)filename)) return -1;
    int sectors = (file.size + 2047) / 2048;
    uint8_t *buf = malloc(sectors * 2048);
    if (!buf) return -1;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);

    int id      = entry_count++;
    TexEntry *e = &entries[id];
    e->buf      = buf;
    GetTimInfo((uint32_t *)buf, &e->tim);
    if (e->tim.mode & 0x8)
        e->clut = getClut(e->tim.crect->x, e->tim.crect->y);
    e->tpage = getTPage(e->tim.mode & 0x3, 0, e->tim.prect->x, e->tim.prect->y);
    e->valid = 1;
    return id;
}

void texmgr_upload(int id) {
    if (id < 0 || id >= entry_count || !entries[id].valid) return;
    TexEntry *e = &entries[id];
    LoadImage(e->tim.prect, e->tim.paddr);
    DrawSync(0);
    if (e->tim.mode & 0x8) {
        LoadImage(e->tim.crect, e->tim.caddr);
        DrawSync(0);
    }
}

uint16_t texmgr_tpage(int id) {
    return (id >= 0 && id < entry_count) ? entries[id].tpage : 0;
}

uint16_t texmgr_clut(int id) {
    return (id >= 0 && id < entry_count) ? entries[id].clut : 0;
}
