#ifndef AREA_BANK_H
#define AREA_BANK_H

#include "title.h"
#include "texmgr.h"

/* =========================================================================
   AREA BANKS — the game is six places, not one
   =========================================================================
   >>> READ THE BANK NOTE IN src/texmgr.h FIRST. <<< This file is the other
   half of it: texmgr.h says what a bank IS, and this says WHICH ONE each room
   belongs to and when the swap happens.

   THE PROBLEM IT SOLVES. Every texture registration used to read its whole TIM
   at startup and never free it, so the game held 820 KB of a 937 KB heap
   permanently — the kitchen wallpaper resident while the player is at the
   bottom of Asag's shaft, the Rear Gate's grinders resident in the mansion.
   Free-at-rest was 145 KB, it fell by 10-20 KB with every room added, and the
   arena's boss model had about 2 KB of headroom. That looked like the PS1
   running out of memory and it was not: it was a five-room policy still running
   at twenty-eight rooms. Nothing about the console requires it — a CD read is
   legal during STATE_LOADING, which is where every room's 60-118 KB mesh comes
   from already.

   THE SIX AREAS, and why the boundaries fall where they do:

     MANSION       the house, the delivery yard, the West Corridor.
     GARDEN        the stairs, Fountain Square, the three mazes, the Chain Room,
                   The Hatch, the Rear Gate, the Outside Catacombs.
     RABISU        the GARDEN COURTYARD alone — it is the boss's room, and it is
                   split off not because it is sealed (it is not: it sits in the
                   middle of the garden chain) but because it is where the
                   Rabisu's 104 KB of model and clip are read, and where
                   SND_BANK_BOSS's 70 KB EMERGE clip is read. It was the second
                   tightest door in the game. Splitting it takes what is resident
                   there from 424 KB to 206 KB.
     WEST GARDEN   the Stables and the Greenhouse, behind the Rear Gate's west
                   gate. Already a VRAM bank (tools/VRAM_MAP_GARDEN_WEST.txt);
                   now a heap one too.
     ASAG          the arena, and it is EMPTY — that room streams every texture
                   it draws and registers none. So the tightest door in the game
                   now has the whole heap.
     CATACOMBS     Chapter 3, behind the one-way mouth.

   WHAT EACH ONE COSTS, as tools/check_tex_banks.py reports it:

       MANSION      456 KB      GARDEN       424 KB
       RABISU       206 KB      WEST GARDEN  256 KB
       ASAG           0 KB      CATACOMBS     72 KB

   ---- THE BOUNDARIES ARE NOT THE GEOGRAPHY, AND THAT IS THE TRAP -----------
   >>> AN AREA'S BANK IS EVERY MODULE ITS ROOMS' UPLOADERS REACH, NOT EVERY
   MODULE IN ITS GEOGRAPHY. <<< Rooms borrow each other's textures through
   narrow uploaders, and those chains cross the map:

     garden_stairs_upload_textures()  calls delivery_upload_brick_wall() and
                                      east_stairwell_upload_chnlnk()
     delivery_restore_textures()      calls garden_stairs_upload_grss_gs()
     outside_catacombs_upload_...()   calls conservatory_upload_con_tile()

   ...so the DELIVERY AREA's registrations are in the garden's bank, the GARDEN
   STAIRS' are in the mansion's, and the CONSERVATORY's are in both. Three of
   the six banks contain modules from another area's half of the map. That is
   why the masks in each *_load_assets() are DERIVED by a tool and not written
   by hand:

       py tools/check_tex_banks.py

   It walks every uploader call chain from every room, works out which modules
   each area actually reaches, and fails if a declared mask does not cover them.
   RUN IT AFTER TOUCHING ANY *_upload_* FUNCTION, because the failure it catches
   is silent at runtime: texmgr_upload() on an unloaded entry does nothing and
   the room draws with whatever the last room left in that VRAM page.

   ---- THE WAY BACK ---------------------------------------------------------
   Nothing here is one-way in the PROGRAM, even where it is in the fiction. The
   player can quit to the title and load a save made anywhere, and that load
   does not restart the executable — it is an ordinary STATE_LOADING pass. So
   every free in here is a free of something that can be READ AGAIN off the
   disc, and the swap is symmetric. Nothing in this module may free a pointer
   whose contents cannot be reconstructed.

   And nothing is keyed off a door trigger: area_bank_sync() takes
   pending_area, so a debug level-select jump and a title-screen Load Game go
   through exactly the same code as walking through a door.
   ========================================================================= */

/* Which bank this area's textures live in. The single table; nothing else in
   the game should be naming rooms to work this out. */
TexBank area_bank_of(GameState area);

/* 1 if this area belongs to Chapter 3. Kept separate from the bank because the
   SOUND banks are not the same partition — the Garden Stairs are deliberately
   on SND_BANK_HOUSE while their textures are in the GARDEN bank, so that house
   monsters stay placeable on them (src/sound.h). Do not unify the two without
   re-reading that. */
int area_is_catacombs(GameState area);

/* Called from main's STATE_LOADING, and from the one title-exit path that does
   not pass through it — and NOWHERE ELSE, because it does CD reads.

   Selects the destination's texture bank and swaps the prop models to match.
   Idempotent, and a no-op on every transition inside an area, which is most of
   them, so the caller runs it unconditionally and lets it decide. */
void area_bank_sync(GameState area);

#endif
