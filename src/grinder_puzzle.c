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
#include "hadad_grinder.h" /* ...and the scene that killing has become        */

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
_Static_assert(GP_Z == GRINDER_PUZZLE_MEET_Z,
               "the death scene frames the wrong point");

#define GP_CRUSH_X_HALF  400
#define GP_CRUSH_Z_MIN  (-285)
#define GP_CRUSH_Z_MAX    115

/* ---- THE KILL BAND ----------------------------------------------------------
   Where Hadad has to be STANDING AT THE MOMENT THE LEVER GOES OVER for the
   plates to have him. It is the crush strip above plus a little leeway at each
   end, and it is a different question from the strip itself: the strip is the
   ground the plates sweep, this is a window in TIME dressed as a window in
   space, and it wants to be a shade more forgiving than the machinery is.

   150 either way is 37 frames at HAD_SPEED at each end — a little over half a
   second of grace on a crossing that takes about a hundred frames. Enough that a
   throw aimed at the moment he is level with the plates still lands if it is
   early or late by a beat, and nowhere near enough to turn "time it" into "throw
   it whenever".

   >>> THE SOUTH EDGE IS ALSO WHERE THE LEAP BEGINS TO APPLY. <<< South of it is
   the leap's case, so the two are the same number read from opposite sides, and
   it is passed to hadads_grinder_vault() as exactly that. It must stay NORTH of
   HAD_VAULT_Z (-585), which it is by 150: a Hadad in the gap between the two is
   told to leap and takes off on his very next frame, which is still south of the
   plates and is fine. Move the leeway past 300 and that stops being true. */
#define GP_KILL_LEEWAY   150
#define GP_KILL_Z_MIN   (GP_CRUSH_Z_MIN - GP_KILL_LEEWAY)   /* -435 */
#define GP_KILL_Z_MAX   (GP_CRUSH_Z_MAX + GP_KILL_LEEWAY)   /*  265 */

#define GP_W_OPEN_X    (-420)
#define GP_W_SHUT_X    (-200)
#define GP_E_OPEN_X      420
#define GP_E_SHUT_X      200

/* ---- The lever -------------------------------------------------------------
   In the west hedge near the corridor's NORTH MOUTH (+Z is north; the lawn is
   the room's north half). The corridor runs z[-1500,1500], so z=1400 puts it
   100 inside the mouth — the first thing the player passes coming down from the
   lawn, and 1485 up the corridor from the grinders it drives.

   >>> IT IS DELIBERATELY A LONG WAY FROM THE MACHINERY, AND THAT IS THE
   ENCOUNTER. <<< Under FLAG_HADAD_THREE, Hadad marches the length of the room
   and straight through the plates on his way to the lawn (hadad.h). The player's
   window is to stand up here, watch him come, and throw the lever as he crosses
   the strip — 1485 away, inside the room's 2500 fog cull, so he is visible for
   the whole approach. Moving this back down beside the grinders would put the
   player inside the crush zone at the moment they need to be watching it.

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
#define GP_LEVER_Z    1400
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
/* >>> THE SIGN IS NOT CENTRED ON THE LEVER, BECAUSE IT WOULD NOT FIT. <<< It
   reads along Z, and door_draw_string_3d advances 6 * pixel per character: 19
   characters at GP_TEXT_PIXEL 3 is 342 world units, so it needs 171 either side
   of its centre. Centred on the lever at z=1400 it would run to z=1571 and bury
   its last four characters in the lawn's south hedge, which begins at the
   corridor mouth at z=1500 — a prompt whose tail is eaten by a hedge corner
   reads as a bug, not as a sign.

   1300 gives it z[1129,1471], 29 clear of the mouth. The lever still stands
   inside that span (it is at 1400, right of centre), which is all the sign has
   to do: it is a wide banner on the hedge and the shaft is in front of it. */
#define GP_TEXT_Z          1300

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

    /* Open, still, silent — UNLESS THE MECHANISM IS BROKEN, in which case the
       pair are seated SHUT and stay that way for the rest of the game. That is
       the GameFlag the header used to say this reset would one day need
       (FLAG_GRINDER_BROKEN, player.h): the corridor is the only way from the
       lawn to the ramp, so a re-entry that put the plates back to open would
       hand the player a route the story has closed.

       `shut` matches `travel`, so nothing is moving on the first frame either
       way and the lever's own arm is already down (push_pitch below reads
       throw_anim, which is set from the same fact). */
    int broken = game_flag(FLAG_GRINDER_BROKEN);
    travel = broken ? GP_TRAVEL_FRAMES : 0;
    shut = broken;
    throw_anim = broken ? GP_THROW_FRAMES : 0;
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

/* ---- The backstop ----------------------------------------------------------
   The invisible wall across the machine's SOUTH mouth while it runs; see the
   block on grinder_puzzle_collide in the header for what it is for.

   THE PLANE IS THE GRINDER BODIES' OWN SOUTH FACE, which is GP_CRUSH_Z_MIN —
   the same edge that bounds the strip the plates sweep, stated once and read
   here rather than re-derived from GP_Z. That places the stop line INSIDE the
   crush strip on purpose: held against it, the player is standing between the
   two plates with a running machine either side, which is the death the corridor
   gate is offering. A plane at the north face would have been a bumper that
   made the grinders safe.

   The standoff is the 75 apply_collision_reception gives the grinders
   themselves, so the invisible line stops the player where the machine's own
   surfaces would; anything wider would read as being held up by nothing.

   >>> IT GRABS FROM BEHIND AS WELL, FOR ONE KNOCKBACK'S WORTH. <<< A plain
   front-only wall is a 75-unit band, and 75 is less than a single frame of
   HAD_KNOCKBACK: Hadad swinging at a player who is standing in the mouth would
   punt them 200 clean through the plane in one step, and out the far side is
   the one place this whole wall exists to keep them out of. So the catch
   reaches HAD_KNOCKBACK past the plane too, which is far enough that nothing
   can cross it in a frame and short enough that it is still only the machine's
   own throat. Nobody legitimately stands in that strip while the pair are
   running: the lever that starts them is 1485 north, so the player is always up
   there when the wall goes up.

   THE X SPAN IS THE MACHINE, not the corridor. The pair span x[-400,400] when
   shut (GP_CRUSH_X_HALF), which already overhangs both hedges — bounding the
   plane by the corridor walls instead would have left it stopping at the hedge
   faces, and the hedges are collision walls anyway. */
#define GP_BAR_Z        GP_CRUSH_Z_MIN
#define GP_BAR_RADIUS   75
#define GP_BAR_GRAB     HAD_KNOCKBACK

/* 1 while the pair are travelling in either direction — the one statement of
   "the machine is running", shared by the update below and the backstop. Note it
   is NOT grinder_puzzle_blocking(): a pair sitting fully SHUT is not running,
   and needs no invisible wall because the bodies themselves span x[-400,400]
   across a 600-wide corridor. That is also what keeps the backstop clear of
   grinders_collide's escape pass, which only ever fires once nothing is
   moving. */
static int running(void) {
    return travel != (shut ? GP_TRAVEL_FRAMES : 0);
}

void grinder_puzzle_collide(int32_t *px, int32_t py, int32_t *pz) {
    if (current_area != STATE_REAR_GATE) return;
    if (!running()) return;

    /* Vertical gate, the grinders' own: only while the player is on the
       corridor's floor, so the plane cannot reach down the ramp. */
    int32_t dy = py - GP_FLOOR_Y;
    if ((dy < 0 ? -dy : dy) > GRINDER_HALF_H) return;

    if (*px <= -GP_CRUSH_X_HALF || *px >= GP_CRUSH_X_HALF) return;

    /* Facing north (+Z), and one-sided beyond the grab strip: push what is
       inside the standoff on the north side of the plane, and anything up to
       GP_BAR_GRAB south of it back out through the mouth it came in by. A player
       further south than that is behind the wall and is left exactly where they
       are, the way collide_wall_frontonly_y leaves a point behind a room wall —
       so someone down by the ramp when the lever is thrown is never dragged
       back up into the plates. */
    int32_t depth = *pz - GP_BAR_Z;
    if (depth <= -GP_BAR_GRAB || depth >= GP_BAR_RADIUS) return;
    *pz = GP_BAR_Z + GP_BAR_RADIUS;
}

int grinder_puzzle_blocking(void) {
    return travel > 0;
}

/* The plate stands GP_PLATE_OFF in from the body's centre, on the face that
   looks up the corridor: the model is 400 deep and its grinder-textured plate is
   its -X face, so the east grinder's plate is 200 west of its centre and the
   west one's (turned a half-turn) is 200 east of its. That is the same 200 that
   makes GP_E_SHUT_X 200 rather than 0 — the plates MEET at x=0 — so it is stated
   once here and read off the live prop below rather than assumed twice. */
#define GP_PLATE_OFF   200

int32_t grinder_puzzle_plate_gap(void) {
    /* Off the EAST grinder's live x, not off `travel`: the pair are mirror
       images and apply_travel is the one thing that decides where they are, so
       reading one of them back cannot drift out of step with the other. */
    int32_t gap = (east_index >= 0)
                ? grinders[east_index].x - GP_PLATE_OFF
                : GP_E_OPEN_X - GP_PLATE_OFF;
    return gap < 0 ? 0 : gap;
}

int grinder_puzzle_travel_left(void) {
    return shut ? (GP_TRAVEL_FRAMES - travel) : travel;
}

void grinder_puzzle_arm(void) {
    /* Seeded from the pad's CURRENT state, not held at 1: a Circle released
       during the scene must leave a clean edge for the next press, and one still
       held must not fire on the frame control comes back. Same contract as
       rear_gate_gate_arm(). */
    interact_prev = interact_tapped();
}

void grinder_puzzle_update(int lock) {
    int moving = running();

    /* The crush, from the contact count the collision pass already worked out
       this frame — apply_collision_reception runs before this in main.c's Rear
       Gate branch, so the count is current. Done FIRST so a killing blow lands
       on the frame the wall reached the player rather than the frame after.

       NOT while the death scene owns the camera. The collision pass does not run
       during a cutscene (main.c returns before it), so the count is whatever the
       last frame of free play left behind — and paying it out for six seconds
       would grind down a player who is anchored somewhere they cannot be
       touched. Dropped rather than banked, which is what crush_update does with
       a zero count anyway. */
    crush_update(hadad_grinder_cutscene() ? 0 : grinders_crush_contacts());

    /* >>> THE PLATES NO LONGER ANSWER FOR HADAD EVERY FRAME. <<< This is where
       the per-frame crush test on him used to be, and it was wrong in a way
       that only showed once he was made to march past the machine rather than
       stand in it: `shut` stays true after the travel has finished, so plates
       that were closed MINUTES ago still caught him, and he died of walking
       into a wall that had been sitting there all along. Nothing about that
       read as the player killing him.

       The verdict is now taken ONCE, on the frame the lever is thrown, and it
       lives at the throw itself at the foot of this function. See THE THREE
       ANSWERS TO THE LEVER there. */

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
        apply_travel(running());

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
       re-open it either — and it stays dead until FLAG_HADAD_THREE replaces flag
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

    /* >>> AND THIS IS WHERE THE LEVER DIES FOR GOOD. <<< Refused AFTER the edge
       detector and the range test, unlike hadad_lever_locked above, because
       this refusal has something to SAY: the player has to be told that the
       thing they are standing in front of is finished, or a corridor they can
       no longer open reads as a bug. The prompt is deliberately left up for the
       same reason — it is what gets them to press.

       The two refusals can never both apply: hadad_lever_locked is
       flag one AND NOT flag three, and nothing sets FLAG_GRINDER_BROKEN
       without flag three. See the flag's own block in player.h for the two
       endings that set it. */
    if (game_flag(FLAG_GRINDER_BROKEN)) {
        show_pickup_msg_raw("The mechanism is broken");
        return;
    }

    /* Throw it. The grind starts on this frame, not when the lever finishes its
       swing: the player is watching the machinery, and a sound that arrives half
       a second after the press reads as lag. */
    shut = !shut;
    plays_left = GP_PLAYS;
    play_timer = 0;

    /* ---- THE THREE ANSWERS TO THE LEVER -----------------------------------
       Everything the machine has to say about Hadad is said HERE, on the one
       frame the switch goes over, and never again. THE THROW HAS TO CATCH HIM;
       the plates standing shut across his road do not. That is the whole rule,
       and the three cases are the three places he can be standing when it
       happens:

         IN THE BAND    the plates have him. He is handed to the death scene
                        (src/hadad_grinder.c), which takes the camera and closes
                        them on him over the next six seconds.

         SOUTH OF IT    he is still on his way up. He carries on marching and
                        LEAPS the machine when he gets to it — hadads_grinder_vault
                        latches it, hadad.c flies it, and he lands on the north
                        side and finishes the climb as if nothing had happened.

         NORTH OF IT    he is already past. Nothing to do at all: he walks on to
                        HAD_CLIMB and takes up the pursuit, with the corridor now
                        shut behind him.

       Unreachable outside FLAG_HADAD_THREE, and not by a test here: under flag
       one hadad_lever_locked() refuses the press above, and with neither flag he
       is a statue on the plinth 2900 north of the band. */
    {
        Hadad *caught = hadads_grinder_caught(GP_CRUSH_X_HALF,
                                              GP_KILL_Z_MIN, GP_KILL_Z_MAX);
        if (caught) hadad_grinder_begin(caught);
        else        hadads_grinder_vault(GP_KILL_Z_MIN);
    }

    /* >>> AND UNDER FLAG THREE THAT WAS THE ONE THROW HE GETS. <<< The lever
       breaks on the spot, whichever of the three answers came back: the third
       encounter is a single decision made at a single moment, and a lever that
       could be worked again would let the player close the plates, watch him
       walk up, re-open, and try the timing over until it landed. The corridor
       stays shut from here — which is where main.c's two doors were already
       taking it, and which is why FLAG_GRINDER_BROKEN's own block in player.h
       calls this the end of the route.

       Set AFTER the throw has been committed above, so this press still runs its
       travel; it is the NEXT one that gets "The mechanism is broken". */
    if (game_flag(FLAG_HADAD_THREE)) game_flag_set(FLAG_GRINDER_BROKEN);
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
                        GP_TEXT_X, GP_TEXT_Y, GP_TEXT_Z - 200,
                        50, 255, 50, fade, 0, TEXT_PLANE_YZ, GP_TEXT_PIXEL);
}
