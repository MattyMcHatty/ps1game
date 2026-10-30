#ifndef UNDERWORLD_H
#define UNDERWORLD_H

/* =========================================================================
   CHAPTER 4 - THE UNDERWORLD: the rules every room in it shares
   =========================================================================
   The chapter is reached only through Descent to Hell, out of the Seal's
   white, and none of it can be walked back out of. Its rooms are open -
   platforms and ruins under a burning sky rather than corridors - so two
   things that are per-room everywhere else in the game are CHAPTER-WIDE
   here, and they live in this file so that no room can quietly differ:

   1. THE FOG. One distance for every room in the chapter: the Garden
      Courtyard's, the furthest of the game's ordinary rooms (Asag's arena
      and the Seal go further, and 2500/6000 here read as too far). Nothing
      changes it - no light, no lantern, no scripted ease, no room override.
      A room's draw sets g_fog_near/g_fog_far from these two and nothing else
      (DEBUG_CULL_DIST's isolation levels excepted, as in every room). The
      SKYBOX is not fogged at all; see src/underworld_sky.h.

      The fog COLOUR is the sky's: the average of the skybox's burning horizon,
      darkened, so a floor tile fading out at the cull line goes the colour
      of what is behind it rather than to black. It is also each room's
      clear colour, which shows above and below the sky's open ring.

   2. THE SKY. Every room but the Descent draws src/underworld_sky.c's ring,
      centred on its own geometry.

   3. THE MUSIC. Every room but the Descent plays CDAUDIO_UNDERWORLD_TRACK
      (src/cdaudio.h), from main.c's Chapter 4 music block - no music line
      of a room's own.

   Which rooms are in the chapter is area_is_underworld() in src/area_bank.h;
   their textures are TEXBANK_UNDERWORLD and their sounds SND_BANK_UNDERWORLD
   (src/sound.h), which is empty today: nothing in the chapter makes a sound
   the player does not bring with them. */

#define UW_FOG_NEAR    800   /* full colour inside this (Manhattan)            */
#define UW_FOG_FAR    3500   /* fully fogged, and the mesh's cull distance      */

#define UW_FOG_R       110
#define UW_FOG_G        24
#define UW_FOG_B         4

#endif
