#ifndef QUAKE_H
#define QUAKE_H

/* -----------------------------------------------------------------------
 * THE QUAKE
 *
 * The house shaking, as a thing two places can ask for rather than a state of
 * the room that happened to want it first. It was written inside
 * exit_door_puzzle.c for the moment the four key stones come back out of the
 * Attic Exit's door; when the East Hall wanted THE SAME beat on the way out of
 * the wrecked Library it moved here whole, and both callers now drive this.
 *
 * WHAT IT IS: two seconds of nothing, then three seconds of the view shaking in
 * place with six overlapping rumbles under it and a log line on the frame it
 * starts moving, then control back. The camera never moves to a new shot — it is
 * jittered about the spot the player is standing on and put back exactly — so
 * there is no glide either way and no camera to compose.
 *
 * IT IS NOT A CUTSCENE MODULE. It owns cam_* and the player anchor for its
 * duration and nothing else: it reads no input, draws nothing, and has no
 * opinion about which room it is in. The CALLER owes it
 *   - a branch in main.c that routes the frame here and skips update_camera,
 *     apply_collision and apply_height (jittering the camera through
 *     apply_collision would have the walls push the shake around);
 *   - the HUD/menu suppression that goes with such a branch;
 *   - its own input re-arm on the finishing frame — see quake_update().
 *
 * >>> HOUSE ROOMS ONLY. <<< The six rumbles need five SPU voices and four of
 * them are BORROWED from garden and boss sounds that cannot sound in a house
 * room. Run this in a garden or courtyard room and it cuts a flower, a mushroom
 * or the boss's charge tell. Read the SFX_RUMBLE_2 block in sound.h before
 * moving it anywhere new.
 * ----------------------------------------------------------------------- */

/* New game, or entry to a room that could host one: park it and SILENCE the five
   rumble voices. The clip is 2.12 s and up to five are sounding when the shake
   ends, so a room change inside that tail would otherwise carry the earthquake
   out of the room with it (the same reason rabisu_boss_reset stops its three). */
void quake_reset(void);

/* Take the camera where it stands. Cancels any look offset, snapshots the base
   to shake around, anchors the player on the real spot so anything hunting them
   keeps hunting it, and spends any Circle still in flight. */
void quake_start(void);

/* 1 while the hold or the shake owns the camera. */
int  quake_running(void);

/* One frame. Returns 1 ON THE FRAME IT ENDS and 0 otherwise; the camera and the
   player have already been given back by then, so the caller owes only its own
   edge-detect re-arm. The rumbles still sounding are left to ring out. */
int  quake_update(void);

#endif
