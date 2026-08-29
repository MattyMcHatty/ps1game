#ifndef ASAG_ARENA_H
#define ASAG_ARENA_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   ASAG'S ARENA — the room at the bottom of The Hatch's shaft.
   =========================================================================
   This is a ROOM WITH NO ART IN IT YET, and that is deliberate. Every system a
   boss encounter needs — the area, its geometry slot, its VRAM bank, its SPU
   bank, its music track, its save slot — is wired and budgeted here so that
   dropping in the mesh, the model, the textures and the clips later is a table
   edit and not an architecture change. It boots, it is enterable, it is
   walkable, and it draws a grey box until the mesh exists.

   >>> READ tools/ADDING_THE_ASAG_FIGHT.txt BEFORE PUTTING ANYTHING IN IT. <<<
   That file is the handover: what is reserved, what it cost, and the exact
   order the pieces go in.

   ---- HOW IT IS REACHED, AND WHY THAT MATTERS MORE THAN IT LOOKS ------------
   ONE WAY IN: the 1200-unit drop down the pit in The Hatch's yard, after both
   keyholes are turned (src/hatch_puzzle.c). ONE WAY OUT: the exit below, which
   the encounter is expected to seal for its whole length.

   That shape is not decoration — it is what makes every budget in this file
   legal. Nothing else in the game is ever drawn, heard or updated while the
   player is down here, and the player cannot arrive from anywhere else, so:

     - VRAM: everything except the player's own kit may be overwritten, because
       nothing that is clobbered can be seen again until the player leaves
       through a transition that puts it back. See tools/VRAM_MAP_ASAG.txt.
     - SPU: the room takes a bank of its own (SND_BANK_ASAG) holding NOTHING
       shared, because every monster in the game is somewhere else. That hands
       the fight the entire 232 KB bank region — the largest sound budget any
       room in this game has ever had. See src/sound.h.
     - HEAP: the room registers nothing with texmgr and keeps no RAM copy of
       anything, so it costs ZERO permanent bytes. See the texture note below.

   A one-way drop also means there is no "walk back out mid-fight" case to
   handle, and no re-entry replay of the reveal: the only exits are winning and
   dying. There is deliberately NO SAVE POINT here, which is what makes that
   true rather than wishful (tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 9).

   ---- THE PLACEHOLDER GEOMETRY ---------------------------------------------
   Own coordinate space, like every garden room. Floor y=0 throughout; a
   4000 x 4000 square, x[-2000,2000] z[-2000,2000], under a 900-tall wall. The
   reasoning for both numbers is in src/asag_arena_mesh_collision.c and it is
   sized off the Rabisu's fight, not invented.

   The player lands under the shaft mouth at the NORTH edge and faces +Z, across
   the room. The exit is a door in the middle of the SOUTH wall, as far from the
   arrival as the room allows — so the fight happens between the two and the
   player crosses the arena to leave it.

   ---- THE TEXTURES: STREAMED, NOT RESIDENT. THIS IS NOT OPTIONAL. -----------
   texmgr keeps the WHOLE TIM in main RAM for the life of the run, and main RAM
   is the budget this project is closest to breaking — the MEASURED boot cliff is
   between 233 and 235 KB of free-at-rest, and the failure there is not a tidy
   malloc error, it is the stack being handed out and the next CdRead DMAing over
   a return address in some unrelated function. When this room was built
   free-at-rest was 245 KB, and six 8bpp 128x128 registrations would have been
   ~110 KB: it would not have booted.

   >>> IT IS 363 KB NOW, AND THAT DOES NOT CHANGE THE ANSWER. <<< The headroom
   came from making the BOSS MODEL room-scoped rather than resident (PART 6 of
   tools/ADDING_THE_ASAG_FIGHT.txt), and it is already spoken for: it is what
   pays for Asag's own model and animation clips, which are far larger than its
   textures — one 19-frame .pva is 75 KB. Streaming the art costs a few hundred
   milliseconds on ONE loading screen and nothing at all at rest, so there is no
   reason to spend permanent bytes on it whatever the figure says.

   So this room reads its own art off the disc on the transition into it, into
   ONE scratch buffer that is freed again: the Greenhouse's shape
   (src/greenhouse.c), by way of the Chain Room's. Zero permanent bytes, against
   a few hundred milliseconds of drive time behind a door fade that already
   makes two other bracketed reads.

   >>> AND UNLIKE THOSE TWO, THE tpage/clut ARE CAPTURED AT READ TIME. <<< The
   Greenhouse and the Chain Room take theirs as compile-time constants out of
   src/tim_slots.h, which is generated from the TIMs listed in disc.xml — so a
   texture that does not exist yet cannot have a constant. Capturing from
   GetTimInfo instead costs two stores per texture on one loading screen and
   means a new TIM can be added to ASAG_TEX below and to disc.xml with NO
   generator run and no header regeneration. Swap to TIM_SLOT() if that ever
   matters; it will not.

   ---- WHAT IS STILL MISSING (all of it is content, none of it is plumbing) --
     - the mesh                 \TEX\ASAGARNA.SMD, and the collision to match
     - the textures             ASAG_TEX[] below is empty
     - the boss                 no body module, no director module
     - the music                CDAUDIO_ASAG_TRACK is defined; track 9 is not
                                yet on the disc
     - the sounds               SND_BANK_ASAG exists and is EMPTY
   ========================================================================= */

/* How many texture slots the room's draw can address. Sized, not measured: the
   biggest textured room in the game (the Greenhouse) uses eleven, and a boss
   arena should be simpler than a greenhouse, not richer. Raising it costs one
   pair of uint16_t in BSS per slot and nothing else — but every slot filled
   costs real VRAM, so check tools/VRAM_MAP_ASAG.txt before spending one. */
#define ASAG_ARENA_TEX_COUNT 12

void asag_arena_load_assets(void);     /* startup: nothing. See the .c.        */
void asag_arena_load_geometry(void);   /* ROOM ENTRY: mesh -> the shared arena */
void asag_arena_upload_textures(void); /* ROOM ENTRY: stream this room's TIMs  */
void asag_arena_init(void);            /* collision, floor zones, spawn        */
void asag_arena_draw(RenderContext *ctx);

/* The one arrival: dropped down the shaft from The Hatch, facing +Z. */
void asag_arena_spawn_shaft(void);

/* The one exit, in the middle of the south wall.
   >>> THE SEAL GOES ON THE CALLER, NOT IN HERE. <<< main.c tests this AFTER the
   encounter's seal predicate, exactly as the Garden Courtyard does — see
   tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 9. asag_arena_exit_arm() must be called
   at the moment the seal LIFTS as well as on arrival: a trigger that has not
   been polled for four minutes holds a stale Circle edge, and without the
   re-arm the player's first press after the fight is either swallowed or fires
   instantly off a button held through the death sequence. */
void asag_arena_exit_arm(void);
int  asag_arena_exit_triggered(void);

/* 1 while the room's own prompt should be suppressed — currently always 0.
   The encounter's seal predicate goes HERE when it exists, so that main.c and
   the sign both read one function and cannot disagree about whether the door
   is offering something it will not do. */
int  asag_arena_exit_sealed(void);

#endif /* ASAG_ARENA_H */
