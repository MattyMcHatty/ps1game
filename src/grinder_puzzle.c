#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "collision.h"        /* GROUND_FLOOR_Y */
#include "sound.h"            /* SFX_GRIND, SFX_HURT */
#include "player.h"           /* MAX_HEALTH, player_hurt, game_over, flash_timer */
#include "btn_glyph.h"        /* BTN_CIRCLE */
#include "door.h"             /* door_draw_string_3d, interact_tapped */
#include "title.h"            /* STATE_REAR_GATE */
#include "lever.h"
#include "grinder.h"
#include "grinder_puzzle.h"
#include "hadad.h"          /* the lever lockout, and the thing the plates kill */

/* ---- The corridor ----------------------------------------------------------
   The PATH is the hedged corridor running south out of the Rear Gate's lawn:
   x[-300,300] between collision walls 0 and 1, z[1500,-1500]. Everything below
   is measured off those two walls. */
#define GP_WALL_W      (-300)   /* west hedge face */
#define GP_WALL_E        300    /* east hedge face */
#define GP_Z            (-85)   /* both grinders, a little south of the midpoint */
#define GP_FLOOR_Y      (-149)  /* standing reference for a floor at world y=0 */

/* ---- The grinders ----------------------------------------------------------
   The model is 400 x 400 in plan and 450 tall, authored with its base at y=0 and
   the grinder-textured plate on its -X face. The east one faces west unrotated;
   the west one is turned a half-turn so its plate faces east.

   OPEN: 80 of the 400 depth stands proud of the hedge and 320 is buried, so the
   plates sit at +/-220 against hedge faces at +/-300, and the centres 200
   further out again.

   SHUT: the two plates MEET AT x=0, the corridor's centre line, so each centre
   ends 200 short of it. That is a travel of 220 for each grinder, and it leaves
   the pair spanning x[-400,400] across a corridor only 600 wide — the path is
   properly closed, not merely narrowed.

   >>> THE CUT PLANE STAYS ON THE HEDGE. <<< cut_x is a world plane, so as a
   grinder emerges more of its body simply comes into view; the part still
   inside the hedge goes on being clipped. See the cut_x note in grinder.h for
   why the clip exists at all. */
/* The strip the closing plates sweep, for anything the grinders can catch that
   is not the player. The pair span x[-400,400] when shut and each body occupies
   z[-285,115] in either position, so that rectangle IS the danger zone and it
   never moves — only the plates inside it do. Hadad is caught by his CENTRE
   being in it, not by an overlap test: he is 600 wide and the corridor is 600
   wide, so "his centre is between the plates" and "he is between the plates"
   are the same statement here. */
#define GP_CRUSH_X_HALF  400
#define GP_CRUSH_Z_MIN  (-285)
#define GP_CRUSH_Z_MAX    115

#define GP_W_OPEN_X    (-420)
#define GP_W_SHUT_X    (-200)
#define GP_E_OPEN_X      420
#define GP_E_SHUT_X      200

/* ---- The lever -------------------------------------------------------------
   In the west hedge just north of the west grinder (+Z is north; the lawn is the
   room's north half). The grinder occupies z[-285,115] in either position, so
   z=300 clears its north face by 185 and the two never share a coordinate.

   The model is a 150-long shaft with its origin at its CENTROID and its BLUE cap
   at local +Z, which is the end that mounts against the wall. rot_y 3072 aims
   local +Z at world -X (RotMatrix's Y rotation gives wx = x + ((lz * sin) >> 12),
   and sin(3072) = -4096), which is the hand a west wall wants: cap into the
   hedge, red tip out into the corridor. The cap centre must land on the hedge
   face, so the centroid sits 75 east of it and the tip reaches x=-150.

   y is the standing reference (world y = y + GROUND_FLOOR_Y), and -354 is the
   Attic Exit's lever height verbatim — that room's floor is world y=0 as this
   corridor's is, so the same number puts it at the same height off the ground,
   about level with the player's eye. */
#define GP_LEVER_X   (GP_WALL_W + 75)   /* -225: blue cap meets the hedge face */
#define GP_LEVER_Y   (-354)
#define GP_LEVER_Z     300
#define GP_LEVER_ROT  3072

/* ---- Timing ----------------------------------------------------------------
   >>> THE TRAVEL IS THE SOUND. <<< grind.vag is 19438 samples at 11025 Hz =
   1.7631 s = 105.8 frames at 60 fps, and the user asked for three plays with the
   grinders arriving as the third ends. GP_CLIP_FRAMES is therefore the clip's
   own length and the travel is exactly three of them; retrim the clip and both
   have to move (the same contract SFX_GATE has with door_anim.c's
   GATE_SWING_FRAMES).

   220 units over 318 frames is 0.69 units a frame, which is the "slowly" that
   was asked for — a shade over five seconds to shut. */
#define GP_CLIP_FRAMES   106
#define GP_PLAYS           3
#define GP_TRAVEL_FRAMES (GP_CLIP_FRAMES * GP_PLAYS)   /* 318 */

/* The lever's own throw, the Attic Exit's numbers: half a second, tip swinging
   45deg of 4096 down about the blue cap. It is far shorter than the travel on
   purpose — the switch lands and the machinery grinds on after it. */
#define GP_THROW_FRAMES   30
#define GP_THROW_PITCH   512

/* ---- The crush --------------------------------------------------------------
   ONE SECOND OF CONTACT WITH A RUNNING GRINDER KILLS FROM FULL HEALTH, and two
   at once kill in half of that. Both numbers are exact rather than approximate,
   which MAX_HEALTH = 100 against 60 frames does not divide into: 100/60 is
   1.667 health a frame and player_hurt only takes whole numbers.

   So the damage is carried in sixtieths and paid out whole. Each frame of
   contact adds MAX_HEALTH per grinder touching, and every GP_CRUSH_FRAMES in
   the pot buys one point of health — which is 100 points across exactly 60
   frames on one grinder, and across exactly 30 on two, with no drift and no
   rounding loss. Change MAX_HEALTH and this tracks it. */
#define GP_CRUSH_FRAMES   60   /* frames of one-sided contact that kill outright */

/* ---- Interaction -----------------------------------------------------------
   Manhattan to the lever's centroid. The 195 wall standoff parks the player at
   x=-105, so the closest they can stand is 120 away and there is nothing else
   in the corridor to press. The text numbers are the Attic Exit's. */
#define GP_TRIGGER_RADIUS   500
#define GP_TEXT_RADIUS      900
#define GP_FADE_NEAR        600
#define GP_TEXT_PIXEL         3
#define GP_TEXT_Y         (-300)  /* glyph TOP; 7 rows of 3 end clear of the shaft */
/* FLAT ON THE HEDGE, 11 units proud of its face — the same standoff every door
   and gate sign in the game uses. It hangs in the hedge's own plane rather than
   out at the lever's tip, and the lever stands in front of it: the shaft is at
   world y=-205 and the glyphs run y[-300,-279], so the text clears it overhead
   and nothing intersects. */
#define GP_TEXT_X    (GP_WALL_W + 11)   /* -289 */

/* ---- State -----------------------------------------------------------------
   travel counts frames from fully open (0) to fully shut (GP_TRAVEL_FRAMES);
   `shut` is the committed target it is walking towards. The two are equal when
   nothing is moving, which is also the only time a press is accepted, so a
   mashed Circle cannot leave the pair stranded half way. */
static int lever_index;          /* our lever's slot; 0 after levers_clear()    */
static int west_index, east_index;
static int travel;
static int shut;
static int throw_anim;           /* frames into the lever's swing, 0..GP_THROW_FRAMES */
static int plays_left;           /* grind plays still owed this trip            */
static int play_timer;           /* frames until the next one starts            */
static int interact_prev;
static int crush_pot;            /* sixtieths of health owed by the crush       */
static int crush_prev;           /* were we in contact last frame? (hurt cue)   */

static void push_pitch(void) {
    lever_set_pitch(lever_index, (GP_THROW_PITCH * throw_anim) / GP_THROW_FRAMES);
}

/* Slide both grinders to match `travel`. Called every frame they move, and once
   at placement to seat them. The two are mirror images, so one fraction drives
   both: 0 = fully apart, GP_TRAVEL_FRAMES = plates touching at x=0. */
static void apply_travel(int moving) {
    int32_t w = GP_W_OPEN_X + (((GP_W_SHUT_X - GP_W_OPEN_X) * travel) / GP_TRAVEL_FRAMES);
    int32_t e = GP_E_OPEN_X + (((GP_E_SHUT_X - GP_E_OPEN_X) * travel) / GP_TRAVEL_FRAMES);
    if (west_index >= 0) {
        grinders[west_index].x      = w;
        grinders[west_index].moving = moving;
    }
    if (east_index >= 0) {
        grinders[east_index].x      = e;
        grinders[east_index].moving = moving;
    }
}

/* Pay out the crush for one frame. `contacts` is how many running grinders had
   to shove the player (see grinders_crush_contacts) — 0, 1 or 2. */
static void crush_update(int contacts) {
    if (contacts <= 0) {
        crush_pot  = 0;   /* the part-point owed is dropped, not banked */
        crush_prev = 0;
        return;
    }

    /* One cue on the way in. The grind itself is already running and a hurt
       sample every frame for a whole second would be a buzz, not a warning. */
    if (!crush_prev) sound_play(SFX_HURT);
    crush_prev = 1;

    crush_pot += MAX_HEALTH * contacts;
    while (crush_pot >= GP_CRUSH_FRAMES) {
        crush_pot -= GP_CRUSH_FRAMES;
        player_hurt(1);
    }

    /* Death is the caller's job everywhere else in the game, so it is here too
       — same three lines as the flower, the mushroom and the statue. */
    if (player_health <= 0) {
        player_health = 0;
        game_over     = 1;
        flash_timer   = 90;
    }
}

void grinder_puzzle_place(void) {
    /* Both arrays are global and neither is part of world.c's per-room swap, so
       clear before placing — and the lever array in particular MUST be cleared
       here, because lever_set_pitch indexes it by placement order and the Attic
       Exit's four would otherwise sit in front of ours. Both rooms re-place on
       every entry, so clearing costs nothing either way. */
    grinders_clear();
    levers_clear();

    west_index = grinder_add(GP_W_OPEN_X, GP_FLOOR_Y, GP_Z, 2048,
                             STATE_REAR_GATE, GP_WALL_W);
    east_index = grinder_add(GP_E_OPEN_X, GP_FLOOR_Y, GP_Z,    0,
                             STATE_REAR_GATE, GP_WALL_E);

    lever_place(STATE_REAR_GATE, GP_LEVER_X, GP_LEVER_Y, GP_LEVER_Z, GP_LEVER_ROT);
    lever_index = 0;

    /* Open, still, silent. See the header on why this is not saved. */
    travel = 0;
    shut = 0;
    throw_anim = 0;
    plays_left = 0;
    play_timer = 0;
    crush_pot = 0;
    crush_prev = 0;
    push_pitch();
    apply_travel(0);

    /* Seed the edge detector held, so a Circle carried in through the gate
       transition does not throw the lever on the player's first frame — the
       same reason rear_gate_gate_arm() exists. */
    interact_prev = 1;
}

int grinder_puzzle_blocking(void) {
    return travel > 0;
}

void grinder_puzzle_update(int lock) {
    int moving = (travel != (shut ? GP_TRAVEL_FRAMES : 0));

    /* The crush, from the contact count the collision pass already worked out
       this frame — apply_collision_reception runs before this in main.c's Rear
       Gate branch, so the count is current. Done FIRST so a killing blow lands
       on the frame the wall reached the player rather than the frame after. */
    crush_update(grinders_crush_contacts());

    /* ...and the same plates on HADAD. This is the intended answer to a
       hundred-swing health bar, and it is only ever reachable under
       FLAG_HADAD_TWO because that is the only state in which the lever below
       will accept a press at all (hadad_lever_locked). Tested on every frame of
       the closing travel rather than only on the throw, so walking into the
       plates while they are already moving catches him too — which is what the
       player has to engineer, since he is not going to stand there. Area-gated
       inside the module, so it costs nothing anywhere else. */
    if (shut)
        hadads_grinder_crush(GP_CRUSH_X_HALF, GP_CRUSH_Z_MIN, GP_CRUSH_Z_MAX);

    /* The lever's swing, independent of the machinery it started. */
    {
        int want = shut ? GP_THROW_FRAMES : 0;
        if (throw_anim != want) {
            throw_anim += (shut ? 1 : -1);
            push_pitch();
        }
    }

    if (moving) {
        travel += (shut ? 1 : -1);
        /* Recomputed AFTER the step, so the frame the pair arrive is also the
           frame they stop being machinery — leave it at 1 and they would go on
           crushing anything standing against them for ever. */
        apply_travel(travel != (shut ? GP_TRAVEL_FRAMES : 0));

        /* Three plays, back to back, spanning the whole travel. Counting plays
           rather than testing the frame number keeps the cue identical in both
           directions — opening walks `travel` down from 318, and a t==212 test
           would have had to be written twice and kept in step. */
        if (plays_left > 0) {
            if (play_timer <= 0) {
                sound_play(SFX_GRIND);
                plays_left--;
                play_timer = GP_CLIP_FRAMES;
            }
            play_timer--;
        }
        return;   /* no press is taken while the pair are in motion */
    }

    if (lock) return;

    /* >>> HADAD TAKES THE LEVER AWAY. <<< Once the first encounter has armed,
       the corridor gate is dead — the player cannot shut it on him and cannot
       re-open it either — and it stays dead until FLAG_HADAD_TWO replaces flag
       one, at which point the grinders become the way to kill him. Refused
       before the edge detector is read, which costs nothing: the lock can only
       change between visits, and grinder_puzzle_place() re-seeds interact_prev
       held on every entry. */
    if (hadad_lever_locked()) return;

    {
        int held = interact_tapped();
        int just = held && !interact_prev;
        interact_prev = held;
        if (!just) return;
    }

    {
        int32_t dx = cam_x - GP_LEVER_X;
        int32_t dz = cam_z - GP_LEVER_Z;
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (xz >= GP_TRIGGER_RADIUS) return;
    }

    /* Throw it. The grind starts on this frame, not when the lever finishes its
       swing: the player is watching the machinery, and a sound that arrives half
       a second after the press reads as lag. */
    shut = !shut;
    plays_left = GP_PLAYS;
    play_timer = 0;
}

/* The floating sign, in the YZ plane (reading along Z at a fixed X) because the
   wall it hangs on runs along Z. door_draw_string_3d centres the reading axis on
   world_z AFTER adding 200, so pass the lever's z - 200.

   mirror=0: collision wall 0 faces +X (nx = +4096), so the player reads this
   from the +X side. That is the OPPOSITE hand from the room's east gate sign,
   whose wall faces -X — getting it backwards comes out as mirrored text. */
void grinder_puzzle_draw(RenderContext *ctx) {
    int32_t dx = cam_x - GP_LEVER_X;
    int32_t dz = cam_z - GP_LEVER_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= GP_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > GP_FADE_NEAR) {
        int range = GP_TEXT_RADIUS - GP_FADE_NEAR;
        int prog  = xz - GP_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to activate",
                        GP_TEXT_X, GP_TEXT_Y, GP_LEVER_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, GP_TEXT_PIXEL);
}
