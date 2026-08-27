#ifndef HELLUMINATOR_H
#define HELLUMINATOR_H

#include "render.h"

/* ---- The Helluminator -------------------------------------------------------
   "A lantern to light the way to hell. Burns holy anointing oil."

   The third weapon, found in the Greenhouse's west annexe once the pipe-button
   puzzle has wound the vines out of the doorway. Structurally it is the
   Grave-olver (src/graveolver.c): a flat-shaded SMD held in view space, aimed
   with the same crosshair, hit-testing through the same shared circle test in
   weapon.h. Read that file first. What is different is worth stating in full,
   because none of it is cosmetic.

   IT IS HELD, NOT FIRED. Square is a HOLD, not a tap: while it is down the
   lantern burns, and burning is the only thing that costs. One unit of oil per
   second and one point of damage per second, both at 60 frames to the second and
   both paid out of the SAME frame counter (see burn_frames) so they can never
   drift apart — a lantern that charged for a second it did not burn, or burnt a
   second it did not charge for, would be a bug nobody could see.

   THAT ONE POINT IS THE BASE, AND TWO ENEMIES ANSWER IT DIFFERENTLY. Zombies
   are 3x weak to DMG_HOLY, so a tick takes 3 of their 5 and two seconds of
   burning clears one — the lantern is a zombie weapon first, and the whole
   reason to stand still holding it. Living Statues take the plain 1, but are
   the only enemy it can damage while they are STALKING (once they have left
   their plinth), and burning one wakes it. Both are documented where they are
   implemented: zombie.c's weakness table, and living_statue_burn.

   THE PIVOT IS THE RED POLY. assets/props/Helluminator.smx carries one dark-red
   quad at the far end of the handle (model (29, 1, 0)), and like the Valve
   Handle's red triangle it is a REGISTRATION MARKER rather than art: it is the
   point the model swings about, i.e. where the player's fist is. The draw loop
   both rotates about it and SKIPS IT, so it is never seen. Rotating about the
   model origin instead — which is what the Grave-olver does, because its origin
   already sits at its grip — would swing the lantern about a point 29 units up
   the handle and make the aim-follow read as a wobble.

   THE THREE LAMP FACES ALWAYS GLOW. Two orange quads (the lantern's z = +/-20
   panes) and one yellow (its x = -108 end face) carry a small additive white
   wash at all times, and a larger, brighter one while burning. That is the whole
   of the "it casts light": the glow is on the MODEL, in view space, and the room
   around it is not relit. See the note on HELL_GLOW_* below for why.

   THE RETICULE IS THREE TIMES THE SIZE, and so is the hit circle — 42 pixels
   against the gun's 14. What you see is what you burn.

   IT HITS EVERYTHING IN THE CONE, not the nearest thing. That is the one place
   the shared aim test is used differently from the gun: graveolver_fire picks a
   single best_depth and spends its round on it; this sweeps the same test over
   every enemy and burns all of them. A wide, continuous, multi-target beam is
   what a lantern is; a lantern that could only scorch one zombie in a group
   would be a torch-shaped revolver.

   THE OIL IS NOT AMMO. player_oil is a plain scalar and deliberately not an
   AmmoType — see the block on it in player.h. */

void helluminator_init(void);              /* load the model (startup) */
void helluminator_update(void);            /* Square burns (while equipped) */
void draw_helluminator(RenderContext *ctx);

/* 1 while the lantern is lit. The HUD reads it, and so does anything that wants
   to know the player is holding a light source. */
int  helluminator_burning(void);

/* Put it out. Called on a weapon switch, the same way the gun's reload is
   cancelled: a lantern that stayed lit in your pocket would keep spending oil
   the player cannot see. */
void helluminator_cancel_burn(void);

#endif
