#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxcd.h>
#include "cdaudio.h"
#include "texmgr.h"

/* Registration cap. Overrunning it is a nasty failure: texmgr_register returns
   -1, the caller stores tpage/clut 0, and the texture is then WRONG IN EVERY
   ROOM that draws it — with no crash and no message. It has happened once (the
   East Stairwell's duplicate upstairs/strs copies pushed cncrte and dresser off
   the end, because both register AFTER it in main()). Each unused slot costs
   ~48 bytes of BSS; the TIM buffers are malloc'd per bank load, so raising this
   alone costs almost nothing at runtime.

   Raised 48 -> 56 when the Rafflesia arrived, 56 -> 64 for Maze Two's plinth,
   64 -> 72 for the Greenhouse. The live count is 59.

   >>> THE REAL CEILING USED TO BE MAIN RAM AND IS NOT ANY MORE. <<< Until
   September 2026 every registration held its whole TIM for the life of the run
   — 59 of them, 820 KB of a 937 KB heap — and this file's old comment said at
   length that the array size was not the thing to watch. That is fixed at the
   root: a registration now keeps its HEADER and nothing else, and its pixels
   live only while a bank containing it is loaded. See the bank note in
   texmgr.h, and tools/check_tex_banks.py, which is what stops a mis-tagged
   texture becoming a silent rendering bug. */

#define TEXMGR_MAX 72

typedef struct {
    uint8_t  *buf;    /* pixels: held only while a bank containing this is in  */
    TIM_IMAGE tim;    /* parsed header; prect/paddr/crect/caddr point into buf  */
    uint16_t  tpage;  /* captured from the header at startup; never changes     */
    uint16_t  clut;
    int       valid;  /* 1 when buf holds this texture's pixels                 */
    /* The disc path, kept so the pixels can be read whenever a bank asks for
       them. Every caller passes a string literal or an entry in a file-scope
       const table, so the pointer outlives the entry; nothing here copies the
       bytes. */
    const char *file;
    TexBank   banks;
} TexEntry;

static TexEntry entries[TEXMGR_MAX];
static int      entry_count   = 0;
static TexBank  current_bank  = TEXBANK_MANSION;   /* what registrations get tagged */
static TexBank  loaded_bank   = 0;                 /* nothing is loaded at boot     */
static uint32_t missed        = 0;

void texmgr_set_bank(TexBank banks) {
    current_bank = banks;
}

TexBank texmgr_bank_loaded(void) {
    return loaded_bank;
}

uint32_t texmgr_missed_uploads(void) {
    return missed;
}

/* Read `sectors` sectors of a file into a fresh buffer. Returns NULL on any
   failure, which every caller treats as "this texture is absent" rather than as
   an error — the same tolerance the original had, and what keeps a missing file
   a blank wall instead of a crash. */
static uint8_t *read_sectors(const char *path, int max_sectors, int *got) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)path)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    if (max_sectors > 0 && sectors > max_sectors) sectors = max_sectors;
    uint8_t *buf = malloc(sectors * 2048);
    if (!buf) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);
    if (got) *got = sectors;
    return buf;
}

/* ---- Registration: THE HEADER ONLY ----------------------------------------
   ONE SECTOR is always enough and it is worth writing down why. A TIM is an
   8-byte file header, then (for a 4/8bpp image) a CLUT block whose own header
   is 12 bytes and whose body is at most 512 bytes, then the pixel block's
   12-byte header. So the last thing GetTimInfo needs to see lives at offset 532
   at the very worst, a quarter of the way into the first sector. Nothing below
   reads tim.paddr or tim.caddr — those point into a buffer that is freed three
   lines later — and tpage/clut are functions of the VRAM rectangles in those
   headers, which are baked at build time and never move. */
int texmgr_register(const char *filename) {
    if (entry_count >= TEXMGR_MAX) return -1;

    uint8_t *hdr = read_sectors(filename, 1, NULL);
    if (!hdr) return -1;

    int id      = entry_count++;
    TexEntry *e = &entries[id];
    e->file     = filename;
    e->banks    = current_bank;
    e->buf      = NULL;
    e->valid    = 0;

    TIM_IMAGE t;
    GetTimInfo((uint32_t *)hdr, &t);
    e->clut  = (t.mode & 0x8) ? getClut(t.crect->x, t.crect->y) : 0;
    e->tpage = getTPage(t.mode & 0x3, 0, t.prect->x, t.prect->y);
    free(hdr);
    return id;
}

/* The pixel half. Re-parses the header out of the full buffer, because tim's
   paddr/caddr must point into THIS buffer and the registration's copy is long
   gone. tpage/clut are left alone — they were captured at registration and
   cannot have changed. */
static int entry_load(TexEntry *e) {
    if (e->buf || !e->file) return 0;
    uint8_t *buf = read_sectors(e->file, 0, NULL);
    if (!buf) return 0;
    e->buf = buf;
    GetTimInfo((uint32_t *)buf, &e->tim);
    e->valid = 1;
    return 1;
}

static void entry_free(TexEntry *e) {
    if (!e->buf) return;
    free(e->buf);
    e->buf   = NULL;
    e->valid = 0;
}

void texmgr_bank_select(TexBank bank) {
    if (bank == loaded_bank) return;

    /* >>> FREE EVERYTHING GOING OUT BEFORE READING ANYTHING COMING IN. <<<
       Two banks held at once is up to 900 KB against a 937 KB heap whose top IS
       the stack: malloc would succeed on memory the stack is using and the next
       CdRead would DMA through a return address
       (tools/DIAGNOSING_A_BOOT_CRASH.txt section 2). The loop is split in two
       passes for exactly that reason and must not be merged into one.

       Note this runs over ALL entries both times, not over the difference:
       an entry in both the outgoing and the incoming bank is skipped by the
       free pass (it is in `bank`) and skipped by the load pass (it already has
       its buf), so it is never given up and re-read. That is what makes
       Mansion -> Garden cost 424 KB of reads rather than 424 KB on top of
       everything the two share. */
    int i;
    for (i = 0; i < entry_count; i++)
        if (entries[i].banks != TEXBANK_RESIDENT && !(entries[i].banks & bank))
            entry_free(&entries[i]);

    /* The drive cannot do a data read while CD-DA streams — it hangs
       (tools/TEXTURE_STREAMING_DEBUG.txt). Suspend/resume are no-ops when
       nothing is playing, and resume restores the playback position. */
    cdaudio_suspend();
    for (i = 0; i < entry_count; i++)
        if (entries[i].banks & bank)
            entry_load(&entries[i]);
    cdaudio_resume();

    loaded_bank = bank;
}

void texmgr_upload(int id) {
    if (id < 0 || id >= entry_count) return;
    TexEntry *e = &entries[id];
    if (!e->valid) { missed++; return; }
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
