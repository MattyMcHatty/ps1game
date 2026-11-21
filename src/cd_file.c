#include <stdint.h>
#include <stdlib.h>
#include <psxcd.h>
#include "cd_file.h"

/* See cd_file.h: whole sectors for the read, the file's own size after it. */
void *cd_read_file(const char *path) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)path)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    void *buf = malloc(sectors * 2048);
    if (!buf) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);

    /* In place (cd_file.h) - but should a future allocator ever move or refuse
       a shrink, the full-size buffer is still a valid answer. */
    void *fit = realloc(buf, (file.size + 3) & ~3u);
    return fit ? fit : buf;
}
