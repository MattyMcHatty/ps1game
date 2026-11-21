#ifndef CD_FILE_H
#define CD_FILE_H

/* Read a whole file off the disc into a heap buffer the size of the FILE, not
   of the whole sectors the drive moved. Returns NULL if the file is missing or
   the heap is out (every caller already treats NULL as "this model is absent").
   free() it as usual.

   >>> WHY NOT JUST malloc(sectors * 2048). <<< CdRead transfers whole 2048-byte
   sectors, so the buffer has to be that big FOR THE READ - but not after it.
   The 25 resident prop models were 79,872 bytes allocated for 54,548 bytes of
   file: a quarter of every buffer was the tail of its last sector, held for the
   whole game. This reads the sectors, then realloc()s the block down to the
   file. PSn00bSDK's allocator keeps free space as GAPS between blocks (each
   header is prev/next/ptr/size; malloc searches the gaps), and realloc's shrink
   path only rewrites the size - so the shrink is in place, the pointer does not
   move, and the tail is immediately reusable. Disassembled from
   libc_exe_nogprel.a, 21 Nov 2026 (tools/RAM_AB_CHECKLIST.txt B6).

   No CD-DA bracketing here, the same as the per-module readers it replaces: a
   caller that can run while music plays brackets it itself (area_bank.c's
   props_sync does). */
void *cd_read_file(const char *path);

#endif
