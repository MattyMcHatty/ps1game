#ifndef TEXMGR_H
#define TEXMGR_H

#include <stdint.h>

/* Central registry for textures that must be RE-UPLOADABLE to VRAM without a
   mid-game CD read — i.e. textures a room/prop streams into a shared VRAM slot
   on a transition (see tools/TEXTURING_NOTES.txt: LoadImage is safe, CdRead is
   NOT, once the render loop is running).

   Each registered texture has its TIM HEADER read once at startup, so its
   tpage/clut are known for the life of the run. Its PIXELS are held in RAM only
   while a bank that contains it is loaded; from that RAM copy the entry-time
   re-upload is a pure LoadImage with no CD access.

   Adding a new streamed prop/room texture is: texmgr_set_bank() + texmgr_register()
   at startup, texmgr_upload() on the transition, texmgr_tpage()/texmgr_clut()
   when drawing.

   ===========================================================================
   BANKS — WHY THE WHOLE MAP IS NO LONGER IN RAM AT ONCE
   ===========================================================================
   Until September 2026 a registration read its whole TIM at startup and NEVER
   FREED IT. That is a fine trade with five rooms and a bad one with twenty-
   eight: the game held 748 KB of a 937 KB heap — the kitchen wallpaper, the
   piano, the Rear Gate's grinders — permanently, including while the player was
   at the bottom of Asag's shaft where none of it can be drawn. Free-at-rest was
   145 KB, every new room made it worse, and the arena's boss model had about
   2 KB of headroom left.

   NOTHING ABOUT THE CONSOLE REQUIRED THAT. A CD read is perfectly legal during
   main's STATE_LOADING — that is where every room's 60-118 KB mesh comes from,
   behind the red loading screen. The resident copies were buying about a second
   of loading per transition, not correctness.

   So textures now belong to BANKS, and the game holds one bank at a time. The
   design is sound.c's `sfx_bank` table, deliberately — read the bank note in
   src/sound.h first, because every rule there applies here:

     - `banks` is a MASK, not a single value. A texture drawn in more than one
       area is tagged with all of them and is resident in each. ONE RAM copy,
       one id, one entry; it is simply not freed while any bank containing it
       is loaded.
     - TEXBANK_RESIDENT is 0, i.e. "no bank": never loaded by a bank select and
       never freed. Nothing uses it today and nothing should without a reason
       written down — a resident texture is charged to every area at once,
       which is the cost this whole mechanism exists to stop paying.
     - THE FAILURE MODE IS SILENCE. texmgr_upload() on an entry whose bank is
       not loaded does nothing, so the room draws with whatever the previous
       room left in that VRAM page: no crash, no message, and it looks like a
       texturing bug rather than a banking one. That is exactly sound_play()'s
       behaviour on an evicted clip and it has the same remedy — do not reason
       about the masks, CHECK them:

           py tools/check_tex_banks.py

       walks every *_upload_textures() call chain from every room, works out
       which modules each area actually reaches, and fails if a module's
       declared mask does not cover them. Run it after touching any uploader.

   THE PER-AREA SETS, as that tool reports them (September 2026):

       MANSION      456 KB      GARDEN       424 KB
       RABISU       206 KB      WEST GARDEN  256 KB
       ASAG           0 KB      CATACOMBS     72 KB
       ---------------------------------------------
       all at once  820 KB      <- what the old design held, always

   GRANULARITY IS PER MODULE, NOT PER TEXTURE, and that is a deliberate stop.
   The cost is visible in the table above: delivery_area is in four banks
   because the Garden Stairs borrow three of its five textures, and Garden
   Stairs is in four because the Delivery Area borrows one of its four. Going
   per-texture would shave perhaps 80-100 KB off the peak and would mean a mask
   on every row of every new_tex[] table in the game. The peak is 456 KB against
   937 KB of heap; spend that complexity when something needs it, not before.
   =========================================================================== */
typedef uint32_t TexBank;

#define TEXBANK_RESIDENT     0u         /* no bank: never loaded, never freed */
#define TEXBANK_MANSION      (1u << 0)
#define TEXBANK_GARDEN       (1u << 1)
#define TEXBANK_RABISU       (1u << 2)  /* the Garden Courtyard, i.e. the fight */
#define TEXBANK_WEST_GARDEN  (1u << 3)  /* the Stables and the Greenhouse       */
#define TEXBANK_ASAG         (1u << 4)  /* empty: that room streams its own art */
#define TEXBANK_CATACOMBS    (1u << 5)

/* Tag every registration made AFTER this call. A module sets it at the top of
   its *_load_assets() and is not obliged to put it back — main() sets it again
   before each module — but the convention is to leave it alone, because
   forgetting is a silent mis-tag. Startup begins at TEXBANK_MANSION. */
void texmgr_set_bank(TexBank banks);

/* Register a texture at STARTUP. Reads and parses its TIM HEADER ONLY — one
   sector — so tpage/clut are correct from this moment for the rest of the run,
   and KEEPS NO PIXELS. The pixels arrive when a bank containing it is selected.
   Returns a texture id (>= 0), or -1 on failure. */
int texmgr_register(const char *filename);

/* Upload a registered texture's pixels (+CLUT if any) from its RAM copy. Pure
   LoadImage, no CD access — safe during a room transition once the GPU is idle
   (the caller DrawSyncs first, as main's STATE_LOADING does).
   A no-op, silently, if this texture's bank is not loaded. See the note above. */
void texmgr_upload(int id);

/* Make `bank` the loaded one: free the pixels of everything not in it, then
   read the pixels of everything in it that is not already held. Frees before it
   reads, always, because the heap top IS the stack and holding two banks at
   once is how that ends (tools/DIAGNOSING_A_BOOT_CRASH.txt section 2).

   >>> THIS TOUCHES THE DRIVE, so it is legal only where a CD read is legal —
   inside main's STATE_LOADING, and on the one title-exit path that skips it.
   <<< It brackets its own cdaudio_suspend/resume, as sound_bank_select() does.
   A no-op when the right bank is already in, which is every transition inside
   an area, so the caller runs it unconditionally. */
void texmgr_bank_select(TexBank bank);

TexBank texmgr_bank_loaded(void);

/* How many times texmgr_upload() has been called on an entry whose bank is not
   loaded — i.e. how many times the silent failure above has actually happened.
   Zero in a correct build. Not wired to anything; it is here so that "the room
   is drawing the wrong texture" can be answered in one line from a debug
   overlay instead of by reading masks. */
uint32_t texmgr_missed_uploads(void);

uint16_t texmgr_tpage(int id);
uint16_t texmgr_clut(int id);

#endif
