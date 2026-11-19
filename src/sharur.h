#ifndef SHARUR_H
#define SHARUR_H

#include <stdint.h>
#include <psxgte.h>
#include <smd/smd.h>
#include "render.h"

/* -----------------------------------------------------------------------
 * SHARUR - Ninurta's mace, Chapter 4's weapon (Nov 2026)
 *
 * Found on the Chamber of Destiny's lower west plinth (src/destiny_plinths.h)
 * and held only in the Underworld. WEAPON_SHARUR; the bottom box of the
 * weapon wheel; the fourth cell of the menu's WEAPONS column.
 *
 * THE SWING IS THE CRUCIFAXE'S (src/crucifaxe.c melee_start_swing /
 * melee_swing_tick), with two changes:
 *   - TWICE AS FAST: swing_timer advances SHR_SWING_STEP (2) a frame, so the
 *     hit window and the return/cooldown both take half as long (41 -> 21
 *     frames). Every reader of swing_timer (the parry windows) stays right
 *     because the timer still counts the same numbers.
 *   - DOUBLE DAMAGE: melee_damage is SHR_DAMAGE (2) for the length of the
 *     tick, and every enemy's hit path reads it.
 *
 * >>> SHARUR AFFECTS EVERY ENEMY IN THE GAME <<< (the user's rule, Nov 2026 -
 * it is meant to come round again in a New Game +), bosses and mini-bosses
 * included - even ADAPA, whom nothing else but the Helluminator can touch, the
 * RABISU, whom the axe can only parry, and a LIVING STATUE that is not
 * striking. Its swing reaches those three in sharur_swing_extras() on top of
 * the axe's tests; the blade has a block for every enemy family. Fight WINDOWS
 * stay (Asag's head while exposed, Lamashtu's back, Hadad off his plinth).
 * A NEW ENEMY needs a block in sharur_blade_hits() (src/sharur.c), and in
 * sharur_swing_extras() if the axe cannot hurt it.
 *
 * TAP TO SWING, HOLD TO CHARGE. The swing waits for the release: let go
 * within SHR_TAP_FRAMES and it swings (queued if the last swing is still out).
 * HOLD and the head's pale blue-white faces start to glow, peaking after
 * SHR_CHARGE_FRAMES (3 s). Released at full charge it swings and throws THE
 * BLADE: a large "()" of light - two crescents back to back - shot forward at
 * SHR_BLADE_SPEED until it meets a wall. Released between the two: nothing.
 *
 * THE BLADE deals SHR_BLADE_DAMAGE (4, in sword-swing units) once to every
 * enemy it passes through, and shoves (at twice the axe's shove) everything
 * the axe shoves. It ALSO breaks what the axe breaks - crates, breakable doors,
 * vine curtains - up to SHR_BLADE_DAMAGE blows each, one a frame, through the
 * axe's own smash tests run from the blade's position.
 * ----------------------------------------------------------------------- */

#define SHR_SWING_STEP        2     /* swing_timer per frame: twice the axe's    */
#define SHR_DAMAGE            2     /* melee_damage while Sharur swings          */
#define SHR_CHARGE_FRAMES   180     /* 3 s of holding to a full glow             */
#define SHR_TAP_FRAMES       15     /* released sooner than this = a tap: swing  */
#define SHR_HUM_FRAMES      105     /* SFX_SHARUR re-key period: the clip is
                                       1.763 s = 105.8 frames; re-keyed a hair
                                       early, inside its faded tail, so the
                                       loop never drops to silence           */

#define SHR_BLADE_DAMAGE      4     /* per enemy passed through, swing units     */
#define SHR_BLADE_SPEED      70     /* world units a frame (4200 a second)       */
#define SHR_BLADE_LIFE      150     /* frames before it gives out in open air    */
#define SHR_BLADE_HALF_W    170     /* the "()" across, either side of its line  */
#define SHR_BLADE_HALF_H    200     /* ...and up/down                            */

/* The light's colour: the pale blue-white of the mace's own head. */
extern const uint8_t SHR_GLOW_RGB[3];

void sharur_init(void);               /* boot: \TEXUNDW\SHARURWP.SMD (the room
                                         Sharur's Trial is SHARUR.SMD)          */
SMD *sharur_model(void);              /* the same model, for the plinth          */

void sharur_update(void);             /* weapons_update, while Sharur is held    */
void sharur_draw(RenderContext *ctx); /* weapons_draw: the mace + its charge glow */
void sharur_cancel(void);             /* put down (switch, wheel): charge drops  */
void sharur_reset(void);              /* reset_game: no charge, no blade         */

/* THE BLADE runs whatever is in hand - it is in the air, not in the hand -
   from weapons_update / weapons_draw. It dies on a room change. Draw with the
   CAMERA view matrix up. */
void sharur_blade_update(void);
void sharur_blade_draw(RenderContext *ctx);

/* Three additive rings round a world point (the camera's view matrix up), sized
   off the GTE's own depth - the spirits' glow, readable close up. bright
   0..256. Used by the plinth ceremony. */
void sharur_glow_world(RenderContext *ctx, int32_t x, int32_t y, int32_t z,
                       int32_t world_half, int32_t bright, const uint8_t rgb[3]);

/* The mace standing in the world: grip at (x, y, z), pointing up, turned
   `yaw` (4096 = a full turn), `scale` 4096 = the held size. Flat-shaded like
   the held one, sorted at true depth (+bias). Restores the camera view. */
void sharur_draw_world(RenderContext *ctx, int32_t x, int32_t y, int32_t z,
                       int32_t yaw, int32_t scale, int32_t gain);

/* Put the camera's view matrix back on the GTE (the clobber-safe load). */
void sharur_camera_view(void);

#endif
