#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>
#include <psxcd.h>
#include "cdaudio.h"
#include "texmgr.h"

/* Registration cap. Overrunning it USED to be a nasty failure: texmgr_register
   returns -1, the caller stores tpage/clut 0, and the texture is then WRONG IN
   EVERY ROOM that draws it — with no crash and no message. It has happened once
   (the East Stairwell's duplicate upstairs/strs copies pushed cncrte and dresser
   off the end, because both register AFTER it in main()). Each unused slot costs
   ~40 bytes of BSS (sizeof(TexEntry)); the TIM buffers are malloc'd per bank
   load, so raising this alone costs almost nothing at runtime.

   >>> IT IS NO LONGER SILENT, TWICE OVER. <<<
     BUILD TIME  tools/check_texmgr_cap.py runs before every compile (CMake's
                 texmgr_cap target), counts the registrations in src/ and FAILS
                 THE BUILD if they exceed the TEXMGR_MAX below. It reads this
                 #define, so raising the cap here is the whole fix.
     RUN TIME    a refused registration is counted (texmgr_refused()), and
                 main() stops on a red screen naming the count the moment the
                 startup block finishes, before the title. The backstop for a
                 registration the build-time scan cannot see.

   Raised 48 -> 56 when the Rafflesia arrived, 56 -> 64 for Maze Two's plinth,
   64 -> 72 for the Greenhouse, and 72 -> 80 for THE PIT's rusty ironwork, whose
   registration was the 72nd of 72 — i.e. the array was exactly full and the next
   one would have been the silent failure described above. The live count is 72;
   py tools/heap_budget.py prints it, and it is the number to check rather than
   this sentence.

   59 -> 63 for the CRAWLER, and note that only two of those four are the new
   enemy. Its sprite sheet is two entries because its frames are 128x128 and an
   8bpp texture 256 rows tall cannot be placed in this VRAM at all (V is eight
   bits: y%256 + height <= 256). The other two are the ZOMBIE's sleep and alert
   sprites, which were a plain startup LoadImage until the crawler needed to
   borrow their slots — registering them is what gives them a way back, and it
   is the cost of making x[704,832) y128 a time-share instead of dead space.
   Nine slots spare.

   80 -> 128 with the ROOM OF TORSOS, whose registration was the 79th of 80. Not
   because anything was refused — because a cap one away from a silent failure
   was costing a paragraph of care per room, and 48 more slots cost ~2 KB of BSS
   against ~280 KB free at rest. The checks above are what make the number safe
   to leave alone; py tools/heap_budget.py still prints the live count.

   >>> THE REAL CEILING USED TO BE MAIN RAM AND IS NOT ANY MORE. <<< Until
   September 2026 every registration held its whole TIM for the life of the run
   — 59 of them, 820 KB of a 937 KB heap — and this file's old comment said at
   length that the array size was not the thing to watch. That is fixed at the
   root: a registration now keeps its HEADER and nothing else, and its pixels
   live only while a bank containing it is loaded. See the bank note in
   texmgr.h, and tools/check_tex_banks.py, which is what stops a mis-tagged
   texture becoming a silent rendering bug. */

#define TEXMGR_MAX 128

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
    /* The rooms that hold it (tools/gen_tex_rooms.py's table), or none if the
       table has no row for it - then `banks` decides, the old way. */
    uint32_t  rooms[TEX_ROOM_WORDS];
    int       has_rooms;
} TexEntry;

/* The generated table, build/tex_rooms_gen.c. */
extern const TexRoomRow tex_rooms[];
extern const int        tex_rooms_count;

static TexEntry entries[TEXMGR_MAX];
static int      entry_count   = 0;
static TexBank  current_bank  = TEXBANK_MANSION;   /* what registrations get tagged */
static int      loaded_room   = -1;                /* nothing is loaded at boot     */
static int      loading_room  = -1;                /* >= 0: late loads allowed      */
static uint32_t missed        = 0;
static uint32_t late          = 0;
static int      refused       = 0;   /* registrations past TEXMGR_MAX */

void texmgr_set_bank(TexBank banks) {
    current_bank = banks;
}

uint32_t texmgr_missed_uploads(void) {
    return missed;
}

uint32_t texmgr_late_loads(void) {
    return late;
}

/* "\\TEXCTCMB\\GULAA.TIM;1" -> does it name `base` ("GULAA.TIM")? Case-blind,
   ignoring the directory and the ;1. */
static int tim_is(const char *path, const char *base) {
    const char *p = path, *q;
    for (q = path; *q; q++) if (*q == '\\') p = q + 1;
    while (*p && *p != ';' && *base) {
        char a = *p, b = *base;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (a != b) return 0;
        p++; base++;
    }
    return *base == 0 && (*p == 0 || *p == ';');
}

int texmgr_registered(void) { return entry_count; }
int texmgr_capacity(void)   { return TEXMGR_MAX; }
int texmgr_refused(void)    { return refused; }

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
    if (entry_count >= TEXMGR_MAX) {
        /* Counted, and printed for an emulator log; main() turns a non-zero
           count into a red boot screen (see the cap note above). */
        refused++;
        printf("TEXMGR: refused %s - TEXMGR_MAX (%d) is full\n",
               filename, TEXMGR_MAX);
        return -1;
    }

    uint8_t *hdr = read_sectors(filename, 1, NULL);
    if (!hdr) return -1;

    int id      = entry_count++;
    TexEntry *e = &entries[id];
    e->file     = filename;
    e->banks    = current_bank;
    e->buf      = NULL;
    e->valid    = 0;
    e->has_rooms = 0;
    {
        int r, w;
        for (r = 0; r < tex_rooms_count; r++) {
            if (!tim_is(filename, tex_rooms[r].tim)) continue;
            for (w = 0; w < TEX_ROOM_WORDS; w++) e->rooms[w] = tex_rooms[r].rooms[w];
            e->has_rooms = 1;
            break;
        }
        if (!e->has_rooms)
            printf("TEXMGR: %s has no room row - held by its area\n", filename);
    }

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

static int entry_wanted(const TexEntry *e, int room, TexBank area_bank) {
    if (e->has_rooms)
        return room >= 0 && room < 32 * TEX_ROOM_WORDS &&
               ((e->rooms[room >> 5] >> (room & 31)) & 1);
    return e->banks != TEXBANK_RESIDENT && (e->banks & area_bank);
}

void texmgr_room_select(int room, TexBank area_bank) {
    int i;
    loading_room = room;   /* the late-load window opens (texmgr.h) */
    if (room == loaded_room) return;

    /* >>> FREE EVERYTHING GOING OUT BEFORE READING ANYTHING COMING IN. <<<
       Two sets held at once is how the heap runs into the stack: malloc would
       succeed on memory the stack is using and the next CdRead would DMA
       through a return address (tools/DIAGNOSING_A_BOOT_CRASH.txt section 2).
       Two passes for exactly that reason; do not merge them.

       An entry both rooms want is skipped by both passes - never given up and
       re-read - which is what makes most doors free: the rooms of one chunk of
       the map share most of what they draw. */
    for (i = 0; i < entry_count; i++)
        if (entries[i].banks != TEXBANK_RESIDENT &&
            !entry_wanted(&entries[i], room, area_bank))
            entry_free(&entries[i]);

    /* The drive cannot do a data read while CD-DA streams — it hangs
       (tools/TEXTURE_STREAMING_DEBUG.txt). Suspend/resume are no-ops when
       nothing is playing, and resume restores the playback position. */
    cdaudio_suspend();
    for (i = 0; i < entry_count; i++)
        if (entry_wanted(&entries[i], room, area_bank))
            entry_load(&entries[i]);
    cdaudio_resume();

    loaded_room = room;
}

void texmgr_loading_done(void) {
    loading_room = -1;
}

void texmgr_upload(int id) {
    if (id < 0 || id >= entry_count) return;
    TexEntry *e = &entries[id];
    if (!e->valid && loading_room >= 0 && e->file) {
        /* THE SAFETY NET (texmgr.h): a room's entry asked for a texture its
           generated set does not hold. Read it now - this is STATE_LOADING -
           and say so, so the table gets fixed. It stays held until the next
           room select decides it is not wanted. */
        cdaudio_suspend();
        int ok = entry_load(e);
        cdaudio_resume();
        if (ok) {
            late++;
            printf("TEXMGR: late load %s for room %d\n", e->file, loading_room);
        }
    }
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
