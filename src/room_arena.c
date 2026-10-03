#include <stdint.h>
#include <stddef.h>
#include <psxcd.h>
#include "cdaudio.h"
#include "room_arena.h"
#include "room_arena_size.h"
#include "room_data.h"

/* The arena. 16-byte aligned because CdRead DMAs into it as uint32_t* and the
   SMD structures smdInitData builds on top expect at least word alignment;
   16 costs nothing in BSS and is the safe side of every DMA rule on this
   machine. Sized by tools/gen_room_arena.py from the largest mesh on the disc. */
static uint8_t arena[ROOM_ARENA_BYTES] __attribute__((aligned(16)));

static int arena_used = 0;
static int arena_file_size = 0;   /* exact bytes of the file last loaded */

void *room_arena_load(const char *filename) {
    CdlFILE file;

    arena_used = 0;
    arena_file_size = 0;
    room_data_unbind();   /* the bound block is in the bytes about to go */
    if (!CdSearchFile(&file, (char *)filename)) return NULL;

    int sectors = (file.size + 2047) / 2048;

    /* Refuse rather than overrun. This fires only if a mesh grew past the arena
       and tools/gen_room_arena.py was not re-run; the room draws empty, which
       is loud, instead of the read walking off the end into BSS, which is not
       and would corrupt whatever the linker happened to place next. */
    if (sectors > ROOM_ARENA_SECTORS) return NULL;

    /* The drive cannot serve a data read and CD-DA at the same time — it hangs
       (tools/TEXTURE_STREAMING_DEBUG.txt). Suspend/resume are no-ops when
       nothing is playing, which is the usual case here: every transition except
       the delivery <-> kitchen pair stops the music at the door trigger. That
       pair is why cdaudio_resume restores the playback POSITION rather than
       restarting the track. */
    cdaudio_suspend();
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)arena, CdlModeSpeed);
    CdReadSync(0, NULL);
    cdaudio_resume();

    arena_used = sectors * 2048;
    arena_file_size = file.size;
    return arena;
}

int room_arena_used(void) { return arena_used; }
int room_arena_file_size(void) { return arena_file_size; }
void *room_arena_base(void) { return arena; }
