#ifndef HADAD_LIBRARY_H
#define HADAD_LIBRARY_H

#include "render.h"

/* -----------------------------------------------------------------------
 * THE LIBRARY DESTROYED ENCOUNTER — THE DIRECTOR
 *
 * src/hadad.c is the BODY: which flag puts a Hadad in this room, where he
 * appears, the two walks he makes round the U and the fact that he is solid and
 * lethal while he does. This file is the ENCOUNTER: the floating prompt on the
 * crawl gap, the camera going under the collapsed bookcase, and the handover
 * that starts his second walk when it comes out the other side. The split is
 * STEP 1 of tools/ADDING_A_BOSS_ENCOUNTER.txt and it is not cosmetic — the
 * walking would work unchanged in another room; none of what is below would.
 *
 * They talk through the three functions in the marked block at the foot of
 * hadad.h (hadad_library_instance / _present / _begin_return) and nothing else.
 *
 * ======================================================================
 * THE BEAT SHEET
 * ======================================================================
 *   1. The player reaches the room's north-west corner. Hadad appears at the
 *      east end of the west spur — hadad.c's trigger, not this file's — and
 *      starts walking west and then south toward the single door.
 *
 *   2. THE SINGLE DOOR DIES ON THE APPEARANCE. Its floating "Press O to enter"
 *      goes out and its trigger stops being polled (main.c gates both on
 *      hadad_library_seals_sdoor()). In its place a prompt comes up on the GAP
 *      under the fallen shelving, a little north and east of the door:
 *      "Press O to escape".
 *
 *   3. Circle takes the camera. It ducks to crawl height as it slides across to
 *      the mouth of the gap, travels the length of the gap due east under the
 *      block at a constant crawl, and stands back up at the far side as it turns
 *      to face north up the east aisle. Six seconds, and no input the whole way.
 *
 *   4. Control comes back with the player standing in the room's south-east
 *      corner. The director puts Hadad in front of the single door — where the
 *      first walk was aimed, whether or not he had got there — and he walks back
 *      north, east along the spur, and then down whichever arm of the east aisle
 *      the player is in. The double door is the only way out.
 *
 * ======================================================================
 * THE CAMERA, AND THE ONE TRICK IN IT
 * ======================================================================
 * >>> THE CAMERA IS THE PLAYER, SO THE CRAWL MOVES THE PLAYER. <<< There is no
 * avatar to render in this engine (camera.h). The takeover ANCHORS the player
 * where they stood — so anything hunting them keeps hunting the real spot for
 * the six seconds the camera is elsewhere — and the release at the far end
 * simply lets player_x/y/z() read cam_* again, with the camera by then standing
 * in the south-east corner. That release IS the teleport; there is no other.
 *
 * The three moves are deliberately not the same shape. The duck and the stand
 * are EASED (the glide every other camera in this game uses); the crawl itself
 * is LINEAR, because a constant rate under there reads as squeezing along and an
 * ease-out reads as being pulled out on a rope. Same distinction the Rabisu's
 * rise makes for the same reason.
 *
 * Nothing here pitches the camera, so the yaw-only view matrix trap
 * (ADDING_A_BOSS_ENCOUNTER.txt mistake 1) cannot bite — but the room already
 * builds its view through camera_build_view(), so it would not anyway. cam_pitch
 * is still zeroed on the way out, because free-look sets it and never clears it.
 * ----------------------------------------------------------------------- */

/* Room entry. Parks the director in its idle state — it ARMS LAZILY off
   hadad_library_present(), because library_destroyed_init() runs before
   world_enter() has placed anybody. */
void hadad_library_enter(void);
void hadad_library_reset(void);   /* new game / load: forget a half-played run */

void hadad_library_update(void);
/* The gap's floating prompt. World space, so it is drawn with the room. */
void hadad_library_draw(RenderContext *ctx);

/* 1 while the camera and all input belong to the crawl. main.c's cutscene
   branch and its overlay suppression both read this; it is FALSE for the rest
   of the encounter, which is ordinary free play with something walking at you. */
int  hadad_library_cutscene(void);

/* 1 while the south single door must refuse to be used and must not offer its
   sign. True from the moment Hadad appears and for the rest of the visit —
   there is no "lifts", so no re-arm is owed (library_destroyed_doors_arm() runs
   on every entry anyway). */
int  hadad_library_seals_sdoor(void);

#endif
