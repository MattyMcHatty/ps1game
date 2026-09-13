#ifndef ASAG_FIGHT_H
#define ASAG_FIGHT_H

#include <stdint.h>
#include "asag_arena.h"   /* ASAG_BOIL_COUNT/_LEFT/_RIGHT and the boils' geometry:
                             a caller that loops over the boils needs the count,
                             and every one of them already needs this header. */
#include "render.h"

/* =========================================================================
   ASAG'S FIGHT — the combat AI, the boils, and everything the attacks throw.
   =========================================================================
   >>> WHY THIS IS A THIRD FILE AND NOT PART OF EITHER EXISTING ONE. <<<
   tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 1 splits a boss in two: the BODY
   (model, animation, damage, collision and the combat AI) and the DIRECTOR
   (camera, lights, subtitles, music, the seal, the death sequence). That split
   is right and this file does not break it — it is the BODY half's combat AI,
   living in its own translation unit because src/asag.c's half was already 900
   lines of model and animation and its header opens by saying, in capitals,
   that it is not the fight and decides nothing.

   So the division is:

     src/asag.c        the mesh, the clips, the position track, the collider and
                       the four geometry accessors. Still decides nothing.
     src/asag_fight.c  THIS. Health, exposure, the two boils, the attack loop
                       and every attack's effect and damage. Would work
                       unchanged if the opening scene were deleted.
     src/asag_boss.c   the encounter script: the opening scene, the handover,
                       and the death sequence. Owns the camera.

   The traffic between them is small and one-way in each direction: the director
   calls asag_fight_begin() when it hands the player their camera back and
   watches asag_fight_dying() to know when to start the death; the fight calls
   into src/asag.c and src/asag_arena.c and knows nothing about a camera.

   ---- THE FIGHT, AS BRIEFED ------------------------------------------------
   ASAG has 20 HP and is INVULNERABLE except while his head is EXPOSED. Hitting
   the body never does anything; while exposed, any contact with the head
   counts. Every weapon does 1.

   THE ATTACK LOOP runs forever:

       LASER -> idle 4 s -> SLAM -> idle 4 s -> VOMIT -> idle 4 s -> (repeat)

   THE TWO BOILS are the way in. Each has 3 HP. Burst one and its lights go out
   and it throws three puss balls; it comes back to full 30 s later. Burst BOTH
   and Asag FAINTS — which is the long exposure — and both restore timers
   restart from zero at that moment.

   EXPOSURE, all four windows:
       LASER    never
       SLAM     from the first frame until 0.5 s before the clip ends
       VOMIT    from the first frame until 0.5 s before the clip ends
       FAINT    from the first frame until 1.0 s before the clip ends

   >>> THE VOMIT'S END IS THE ONE ASSUMPTION IN THAT TABLE. <<< The brief gives
   the half-second rule for the slam and says only "as soon as this attack
   starts Asag becomes exposed" for the vomit. The slam's rule is reused; it is
   AF_EXPOSE_LEAD in the .c and it is one constant to change.

   ---- WHAT THE ATTACKS DO, AND WHERE --------------------------------------
   >>> EVERY ATTACK HITS A THIRD OF THE ARENA, NOT A RADIUS. <<< The grid is
   15 x 15, so a third is exactly five rows or five columns:

       LASER   THE LANDING THIRD, rows 0..4 (z 0..933). The beam rakes across
               it and sets it ALL alight; it burns for 3 s and standing on lit
               floor is what hurts.                                   30 damage
       SLAM    ASAG'S THIRD, rows 10..14 (z 1867..2800), where two boulders
               fall either side of him.                               20 damage
               ...plus the head itself, which lands at z=1487 and hurts anyone
               caught under it.                                       20 damage
       VOMIT   THE CENTRE LANE, columns 5..9 (x -500..500), from the back wall
               to the landing.                            20 damage and POISON
       PUSS    a thrown ball, and the one thing here that is not a zone: three
               of them out of each bursting boil.                     10 damage

   THE THREE ZONES COVER DIFFERENT GROUND AND THAT IS THE WHOLE LOOP. The laser
   drives the player FORWARD off the landing; the boulders drive them BACK off
   Asag's end; the vomit splits the room LEFT/RIGHT. No single spot survives all
   three, so standing still is never the answer and the four-second idles are
   when the player picks where to be next. Change one zone and check it against
   the other two.

   All damage figures are PERCENTAGES OF MAX_HEALTH and therefore flat amounts,
   which is how every other attack in this game works.

   THE FLOOR SAYS WHERE. Each zone lights its own polygons while it is live -
   burning orange for the laser, light blue for the slam, green for the vomit -
   and the damage test is the same set of cells, so what is lit and what hurts
   cannot disagree.

   ---- WHERE THE NUMBERS CAME FROM ------------------------------------------
   >>> EVERY PLACEMENT IN THE .c WAS MEASURED OFF THE CLIPS, NOT CHOSEN. <<<
   Asag cannot turn — his vertices are baked in world space (src/asag.h) — so an
   attack lands where the animator put his head and nowhere else, and the only
   thing code can aim is the part that leaves him. Dumping the .pva files frame
   by frame with the position track applied gives, for each clip, the tick at
   which the head settles and where:

       LASER   settles t 75    at x   65  y -850  z 1923   (held to t 280)
       SLAM    settles t 112   at x  -15  y  -66  z 1487   ON THE FLOOR
       VOMIT   settles t 112   at x  -20  y -625  z 1657   twitches t 150-265
       FAINT   settles t 150   at x  -10  y -170  z 1929   near the floor

   Those are why the slam is the attack the axe can answer (the head comes to
   the floor in front of the player) and why the laser is the one that has to
   sweep (its head never leaves the back half of the room, so a fixed beam would
   be dodged by standing anywhere).

   Re-measure them if the clips are ever re-baked. The command is in the .c.
   ========================================================================= */

/* ---- Lifetime -------------------------------------------------------------
   asag_fight_reset() is called from asag_boss_reset(), i.e. on every arrival,
   and puts the whole fight back to its start — full health, both boils lit and
   whole, nothing in flight, no attack running. It does NOT start the fight:
   the opening scene runs first and hands over.

   asag_fight_begin() is that handover, called from the last frame of
   ABE_HANDOVER. The loop starts on the laser, as briefed.

   asag_fight_stop() parks everything: the attack loop stops, every projectile
   and every burning cell is cleared, and the boils go dark. The death sequence
   calls it so that nothing the boss threw can still be in the air while it
   comes apart — tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 12 asks for exactly one
   call that does this rather than a field cleared at each call site. */
void asag_fight_reset(void);
void asag_fight_begin(void);
void asag_fight_stop(void);

/* 1 while the attack loop is running. False before the handover and from the
   moment health hits 0. */
int  asag_fight_active(void);

/* One game frame of the fight. Call from main.c's free-play branch for this
   room, after asag_boss_update() and asag_update() — it reads the pose those
   two leave, and an attack's effects must line up with the frame the player is
   looking at rather than the one before it. Safe with no model loaded, safe
   before the handover, and safe during the death. */
void asag_fight_update(void);

/* ---- Health and vulnerability ---------------------------------------------
   >>> asag_exposed() IS THE WHOLE OF THE FIGHT'S DIFFICULTY AND EVERY WEAPON
   MUST ASK IT. <<< src/asag.h's asag_head_box() says where the head is; this
   says whether hitting it does anything. A weapon that tests only the box
   damages a boss the brief makes invulnerable for most of the fight, and
   nothing on screen would say why it was working.

   asag_damage() is the one entry point. It ignores everything while not
   exposed, while not fighting, and once dying — so a call site does not have to
   re-check any of that. */
#define ASAG_MAX_HEALTH  20

int     asag_exposed(void);
int32_t asag_health(void);
void    asag_damage(int32_t amount);

/* ---- `dying` vs `dead`, which are two flags and not one --------------------
   tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 11. `dying` goes up the instant health
   reaches 0: the attack loop stops, nothing he threw can still hurt anyone, the
   health bar goes away and every weapon skips him — but THE BODY IS STILL
   DRAWN, because that is the whole point of a death sequence. `dead` is set by
   the DIRECTOR at the end of the fade, and means gone.

   The director owns the transition between them; this module only reports. */
int  asag_fight_dying(void);
int  asag_fight_dead(void);
void asag_fight_set_dead(void);

/* ---- The boils, as targets ------------------------------------------------
   The eight polygons the arena already draws, in two clusters of four, with
   3 HP each. See asag_arena.h for where they are and gen_asag_arena_tex_map.py
   for how the two clusters are told apart.

   asag_boil_target() fills the (centre, half-width, half-height) cylinder the
   weapon layer tests, and returns 0 for a boil that is currently BURST — which
   is what makes a burst one un-shootable rather than merely dark.

   >>> AND A SHOT AT A BOIL MUST BE CLEARED SHORT OF IT. <<< The boil faces sit
   at z=2734 and the collision proxy's back wall is at z=2700, i.e. 34 units in
   FRONT of them: the proxy is a rectangle and the lumps stand proud of the
   visual wall it approximates. So weapon_aim_clear() out to the boil's own
   depth reports every shot blocked, by a wall that is standing inside the thing
   being aimed at. This is the runbook's mistake 2 wearing a different hat — the
   arena's own geometry eating the shots that define the fight — and the fix is
   to clear the line to just short of that wall instead of past it.

   ASAG_BOIL_CLEAR_BACKOFF is that "just short", in world units along the
   crosshair. 80 clears z=2700 comfortably from anywhere in the room and is far
   less than the distance to any other wall, so nothing else can hide behind it.
   Both the gun and the lantern must use it. */
#define ASAG_BOIL_CLEAR_BACKOFF  80

int  asag_boil_target(int which, int32_t *cx, int32_t *cy, int32_t *cz,
                      int32_t *half_w, int32_t *half_h);
void asag_boil_damage(int which, int32_t amount);

/* ---- Drawing --------------------------------------------------------------
   asag_fight_draw() is everything the fight puts in the WORLD: the laser beam
   and its burning trail, the slam's marker pools and the boulders falling into
   them, the vomit's spray, and the puss balls. All of it additive, all of it in
   world space, so it wants the PLAIN camera view matrix — which is what
   asag_arena_draw() has loaded and what src/asag.c's body draw leaves alone.

   asag_fight_draw_bar() is Asag's health bar, which projects a world point and
   so wants the same matrix, but is drawn LAST of the two so it sits in front of
   whatever the attacks lit up. Both are no-ops outside the fight.

   THE BAR IS THE RABISU'S, EXACTLY: two seconds of a RED bar when something
   connects, and nothing the rest of the time. It is deliberately NOT an
   exposure read-out - that was tried, in green, and dropped. What tells the
   player a hit landed is the BODY, which flashes red the same way the Rabisu's
   does (asag_set_hit_glow in src/asag.h), and what tells them he is vulnerable
   is the attack he is playing. */
void asag_fight_draw(RenderContext *ctx);
void asag_fight_draw_bar(RenderContext *ctx);

#endif /* ASAG_FIGHT_H */
