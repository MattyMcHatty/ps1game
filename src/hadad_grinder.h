#ifndef HADAD_GRINDER_H
#define HADAD_GRINDER_H

#include <stdint.h>
#include "render.h"
#include "hadad.h"

/*
 * THE HADAD DEATH SCENE — the Rear Gate corridor's grinders closing on him.
 *
 * This is the DIRECTOR half of the flag-three encounter's payoff, split from
 * the body for the reason tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 1 gives: the
 * camera, the cues and the beats are a one-off script bolted to one corridor,
 * while hadad.c is the enemy that would still work dropped in another room.
 * It is a LIGHT director in the src/hadad_library.c sense — no arena, no music
 * cue, no seal — and that file, not src/rabisu_boss.c, is the template it
 * follows.
 *
 * THE TRIGGER IS THE THROW, AND ONLY THE THROW. src/grinder_puzzle.c takes one
 * verdict on the frame the lever goes over: a Hadad standing in the band across
 * the plates (hadads_grinder_caught) is handed to this file, one still south of
 * it is told to LEAP the machine instead, and one already north of it is left
 * alone. It used to test every frame of the closing travel, which meant plates
 * that had been shut for a minute went on collecting him when he finally walked
 * into them — see the note on hadads_grinder_caught in hadad.h. Either way it is
 * only ever reachable under FLAG_HADAD_THREE, because that is the only state in
 * which the lever accepts a press at all.
 *
 * >>> AND THAT PRESS IS THE ONLY ONE. <<< Under the same flag the lever breaks
 * on the throw, whichever answer came back, so the encounter cannot be re-rolled
 * by closing and re-opening the corridor until the timing lands.
 *
 * What being CAUGHT does is this file: it used to empty his health on the spot,
 * and it now hands him over for six seconds.
 *
 * >>> IT IS ALSO THE END OF THE ROUTE. <<< The machinery that kills him is
 * broken by having been USED: FLAG_GRINDER_BROKEN goes on at the throw, up in
 * grinder_puzzle.c, and this file sets it again with the killing blow — the same
 * flag, idempotent, kept here because the kill must seal the corridor even if
 * the throw that started it ever stops doing so. The plates stay shut and the
 * lever answers "The mechanism is broken" from then on. The
 * hedged corridor is the only way from the Rear Gate's lawn to the ramp and the
 * door at the top of it, so winning here seals the house off from the garden.
 * That is deliberate, and the same flag is set — by src/main.c, not by this
 * file — if the player walks away without killing him. See the flag's own block
 * in src/player.h for both endings.
 *
 * THE BEAT SHEET, in the order it plays:
 *
 *   STEP     0.75 s  the camera takes over, HIS MUSIC STOPS (the stalker track
 *                    has been up under him since the ramp, and the hunt is
 *                    decided the moment the plates have him — though not by a
 *                    stop call from in here: see Hadad.frozen, and the note at
 *                    the head of hadad_grinder_begin), and the camera steps
 *                    NORTH and UP — a translation only, holding whatever way
 *                    the player was looking when they threw the lever.
 *   TURN     0.75 s  ...and only then turns and pitches down onto the grinders,
 *                    which are by now under power and closing. Across both of
 *                    these he is slid onto the corridor's centre line, exactly
 *                    between the plates, and starts to be squeezed.
 *   CLOSE    ~3.8 s  the plates finish their travel. He is scaled to the gap
 *                    between them the whole way, so he narrows and rises as
 *                    they come in. He ROARS (SFX_HAD_DIE) with the last three
 *                    and a half seconds of it to run, and the plates are ALL
 *                    BUT TOUCHING when he goes: the grey burst and the cue are
 *                    hadad_damage's, unchanged. THE SAME FRAME SETS
 *                    FLAG_GRINDER_BROKEN (above).
 *   ORB      1 s     a glowing green ball is left in the middle of his chest.
 *                    The camera tilts off the machine and holds on it.
 *   RISE     3 s     it climbs, gaining speed, until it is a dot. The camera
 *                    pans up after it and SFX_WOOSH plays once.
 *   BACK     1 s     back to where the player was standing, and the log says
 *                    "Hadad's spirit was set free."
 *
 * The CLOSE phase is the only one whose length is not a constant: it is however
 * much travel the grinders had left when the scene armed. Now that the trigger
 * is the throw itself that is always the WHOLE travel — the lever went over on
 * this very frame — but it is still read from the machinery rather than assumed,
 * because the scene should not have to be re-timed if the pair are ever armed
 * from some other state. See hg_begin() for the two frame numbers derived from
 * it, and for what the floor under them is protecting against.
 */

/* Room entry: park the scene. Called from the END of rear_gate_init(), which
   runs BEFORE world_enter() places anybody — so this arms nothing and decides
   nothing (ADDING_A_BOSS_ENCOUNTER.txt STEP 4, arm lazily). */
void hadad_grinder_enter(void);

/* New game / load: park it and cut its two clips, so a scene interrupted mid
   roar does not carry the roar into the delivery area. */
void hadad_grinder_reset(void);

/* 1 while the scene owns the camera and the input. Read by main.c for the
   cutscene branch and the HUD/menu suppression, and by grinder_puzzle.c so its
   own per-frame crush tests do not fire underneath it. */
int  hadad_grinder_cutscene(void);

/* The plates have him. Takes the camera and starts the sequence above. Safe to
   call with NULL. */
void hadad_grinder_begin(Hadad *h);

/* Per-frame, from main.c's cutscene branch and from nowhere else — the scene is
   either running or it is not, and while it is not there is nothing to tick. */
void hadad_grinder_update(void);

/* The green ball, and the death burst. Called from rear_gate_draw() with the
   room's plain view matrix loaded, right after draw_hadads(ctx).

   >>> IT DRAWS THE PARTICLES ITSELF, AND THAT IS NOT AN OVERSIGHT. <<< The
   grey burst is a particle burst, and particles are drawn by main.c's
   draw_player_systems — which is exactly what a cutscene suppresses, along with
   the weapon and the HUD. A death sequence whose one visible payload is a burst
   therefore has to draw it, and only while it is running, or main.c would draw
   it a second time. */
void hadad_grinder_draw(RenderContext *ctx);

#endif
