#include <stdint.h>
#include <string.h>
#include <psxgte.h>
#include "asag.h"
#include "asag_arena.h"
#include "asag_boss.h"
#include "asag_fight.h"   /* the combat AI: the loop, the boils, the attacks */
#include "camera.h"
#include "rabisu.h"        /* rbs_glow_point - the death lights; see the draw */
#include "cdaudio.h"        /* CDAUDIO_ASAG_TRACK — borrowed; see cdaudio.h  */
#include "sound.h"          /* SFX_DMNSPEAK — SND_BANK_ASAG as of this scene  */
#include "btn_glyph.h"      /* btn_prompt_draw — screen-space text            */
#include "render.h"
#include "title.h"          /* STATE_ASAG_ARENA, for the arm gate             */

/* =========================================================================
   ASAG'S DIRECTOR — THE OPENING SCENE.
   =========================================================================
   This replaces the demo that cycled the six clips end to end so they could be
   looked at. What it runs now is the scene the brief asks for, and then it hands
   the player their camera back and lets the OLD demo loop run as the "fight"
   until the fight is written.

   >>> SO THIS FILE IS HALF FINISHED ON PURPOSE, AND THE HALVES ARE MARKED. <<<
   Everything down to ABE_HANDOVER is the real thing. ABE_FIGHT is still the
   placeholder: no health, no attacks, no damage, no death sequence, no seal.
   tools/ADDING_A_BOSS_ENCOUNTER.txt is the runbook for the rest, and the shape
   this file already has — one enum, one phase_t, one switch, one enter_phase —
   is that runbook's STEP 4 and is what the fight should grow inside.

   ---- THE BEAT SHEET, AS BRIEFED -------------------------------------------
     1. The level loads and the camera is ABOVE the landing, looking straight
        down at the floor — carrying straight on from the transition, which has
        just finished with mud filling the screen.
     2. It FALLS to the landing, accelerating.
     3. It BOUNCES, twice and decaying, and the hurt sound plays as the player
        lands. Asag is SITTING two seconds into his faint for all of the above:
        the brief says "sitting", so the clip is seeded and then HELD.
     4. It PANS UP off the floor to look at him in that position, and the faint
        is released.
     5. As the faint resolves and he returns to Home, the camera moves UP AND TO
        THE RIGHT, ending looking slightly DOWN at his face.
     6. He drops into the idle, looping.
     7. The boils light up over two seconds.
     8. Still idling, he speaks: the demon-speech clip and the boss music start
        together, and two lines of subtitle follow, one per utterance.
     9. The camera returns to the landing, levels off, and control goes back —
        the player standing exactly where the shaft dropped them.

   ---- THE SHOT LIST, AND WHERE EVERY NUMBER IN IT COMES FROM ---------------
   Three camera positions, all expressed as OFFSETS FROM THE SPAWN rather than
   as world literals, so they follow AA_SHAFT_X/Z and AA_EYE_Y if the landing
   ever moves. The spawn itself is captured on the arm — the director does not
   own it and should not restate it.

     SPAWN     (0, -189, 300) as asag_arena_spawn_shaft() leaves it. Both the
               start and the end of the scene: the drop lands here and the
               handover comes back here, which is what "control returns at the
               original starting position" means.
     ABOVE     the spawn plus ABE_DROP_RISE in the air, pitch straight down.
     VANTAGE   up and to the right: +ABE_VANTAGE_DX in X, +ABE_VANTAGE_RISE up.

   THE VANTAGE IS SOLVED FOR THE BRIEF'S "LOOK DOWN SLIGHTLY AT HIS FACE", and
   the arithmetic is worth keeping because the roofline nearly forbids it:

     At Home his face sits at (-14, -785, 2290) — measured off the idle .pva,
     see asag_face_point(). From the spawn that is 1990 away in Z.
     A camera that stays at the spawn's Z needs to be dy = 1990*tan(angle)
     above the face to look down by `angle`:
         3 deg -> 104 above ->  cam_y -889
         4 deg -> 139 above ->  cam_y -924
         6 deg -> 209 above ->  cam_y -994    <- AT THE ROOFLINE. Too far.
     The arena's perimeter walls top out at y=-1000, so anything past about
     five degrees puts the camera in the ceiling. ABE_VANTAGE_RISE is 730,
     giving cam_y -919 and a measured +43 (3.8 deg) at Home — "slightly" is
     not a stylistic choice here, it is the room's height.

     THE SIDEWAYS MOVE IS WHAT ACTUALLY IMPROVES THE SHOT. Head-on from the
     spawn, Asag is 314 units wide at 1990 range inside a 2488-wide frame —
     about an eighth of the screen. From +650 in X the yaw is -210 (18.5 deg
     off axis), which turns his 1669-unit LENGTH into about 530 units of
     lateral spread, and he reads as a long animal across roughly a third of
     the frame instead of a blob in the middle. x=650 is well inside the
     arena's x[-1500,1500] and the camera stays on the same floor zone.

   ---- WHAT IS DIFFERENT FROM THE RABISU'S REVEAL, AND WHY ------------------
   Read src/rabisu_boss.c first; this is the second encounter in the game and it
   deliberately keeps that one's shape. Two things genuinely differ:

   A. THE AIM POINT IS ALIVE. The Rabisu's crane holds a FIXED point — its
      spawn — so its pitch is a constant with one binary search behind it. Asag
      is animating through the whole shot, and what the camera is asked to
      follow is that animation. So the aim is re-solved every frame from
      asag_face_point(), which is why that accessor exists.

      >>> AND THAT NEEDS SMOOTHING, WHICH A FIXED AIM POINT NEVER DID. <<< The
      pose steps at ASAG_ANIM_FPS (8) while the camera runs at 60, so an aim
      taken raw off the body JUMPS eight times a second — measured, the worst
      single step through the faint is 54 units of pitch, which reads as a
      camera being kicked. The aim therefore chases the solved target
      exponentially (ABE_PAN_LAG), which turns the stepping into a glide. This
      is the one piece of machinery here the Rabisu has no equivalent of.

   B. THERE IS NOTHING TO SEAL. The arena has no exit at all — see
      src/asag_arena.h, where that is written down as a deliberate hole — so
      there is no door trigger to gate, no prompt to suppress and no
      asag_boss_seals_door() to write. When the way out is decided, that
      predicate belongs in asag_arena_exit_sealed() and this file supplies it;
      tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 9 has the shape, including why the
      re-arm has to happen on the frame the seal LIFTS.

   >>> AND ONE THING THAT *USED* TO BE DIFFERENT AND NO LONGER IS. <<< An
   earlier version of this scene never moved the camera at all — it opened from
   the player's own eyes and gave them back the same spot — so this file argued,
   at length, that it needed no glide machinery and that THE YAW NEVER CHANGES,
   on the measurement that Asag's centre sits at x = -11 on every frame of both
   clips. The first half of that is simply gone: the camera now falls in, bounces
   and moves to a vantage, so it glides and it restores.

   The SECOND half was the more interesting claim and it is now FALSE, for a
   reason worth spelling out because it is easy to re-introduce: the yaw was
   invariant because the CAMERA sat on x = 0 and so did the boss. Move the camera
   to x = 650 and the yaw to him is -210, which is 18 degrees and not a rounding
   error. The invariance was a property of one shot, never of the geometry — and
   this is why the aim below solves BOTH angles through one routine instead of
   hard-coding a zero.

   ---- AND ONE THING THAT IS THE SAME AND MATTERS ---------------------------
   THE MUSIC IS THE BOSS'S, NOT THE ROOM'S. main.c's STATE_LOADING calls
   cdaudio_stop() for this area, so the arena is silent from the drop until the
   speech, and this file starts the track. Do not move it to the loading branch:
   that stop is also what keeps a debug level-select jump from arriving with The
   Hatch's music still playing.
   ========================================================================= */

/* =========================================================================
   TIMING. Every duration in FRAMES at 60 Hz, with the brief's seconds beside
   it, all in one block so the beat sheet above can be read off the numbers.
   ========================================================================= */

/* >>> WHERE THE FAINT STARTS, AND IT IS A TICK COUNT FOR A REASON. <<< The
   brief says "two seconds into his Faint animation", and a clip in this engine
   is two tracks at two rates — the baked pose at 8 fps and the position ramp at
   60. asag_play_at() seeks both off one tick count, so 120 is the whole of it.
   See the note on that function in src/asag.h. */
#define ABE_FAINT_ENTRY_TICKS  120   /* 2.0 s in, as briefed                  */

/* The faint is 40 baked frames at 8 fps = 300 ticks, so entering at 120 leaves
   180. This is the BACKSTOP, not the length: the phase normally ends on
   asag_clip_done(). It exists because a clip that failed to load has no frames,
   never advances and therefore never reports done — the exact silent failure
   src/asag.c's read_file() produces when the heap runs short, and the one
   tools/ADDING_A_BOSS_ENCOUNTER.txt's checklist means by "every non-terminal
   phase has an exit that fires on a frame count". 240 is a third longer than
   the 180 it should take, which is slack enough to never fire by accident and
   short enough that a missing faint costs four seconds rather than a hang.

   >>> IT IS ALSO THE LENGTH OF THE CAMERA'S DRIFT TO THE VANTAGE, SO IT IS NOT
   PURELY SLACK ANY MORE. <<< The drift is scaled to this rather than to the
   clip, which means the camera is 84% of the way there when the faint ends and
   ABE_BOILS carries the rest. Shorten this and the drift gets faster; lengthen
   it and more of the arrival spills into the boils. Neither is wrong, but
   neither is free either — this stopped being a number that could be changed
   without looking at the shot. */
#define ABE_T_FAINT_MAX        240   /* 4.0 s: the backstop AND the drift     */

#define ABE_T_BOILS            120   /* 2.0 s of boils coming up, as briefed  */
#define ABE_T_LINE             360   /* 6.0 s a line, the Rabisu's pacing     */

/* ---- The arrival ----------------------------------------------------------
   >>> THE DROP HEIGHT IS CAPPED BY THE CEILING, NOT CHOSEN FOR DRAMA. <<< The
   arena's perimeter walls top out at y=-1000 and the landing's eye height is
   -189, so there are 811 units of air above the player and the camera has to
   stay inside them: above the wall tops it would see over the perimeter and out
   of the room. 730 leaves 81 units of headroom and is still nearly four times
   the player's own height, which is plenty of fall to read as one.

   It is the SAME number as ABE_VANTAGE_RISE by coincidence rather than by
   design — both are "as high as this room allows" — so they are two constants
   and not one.

   THE FALL RUNS ON t^2, like the drop in the yard it continues (HP_FALL in
   src/hatch_puzzle.c) and like the mud plane in the transition between them
   (DOOR_PANEL_FALL in src/door_anim.c). All three are the same fall and all
   three accelerate; a linear one reads as a descent on a wire. */
#define ABE_DROP_RISE          730   /* units above the landing, eye at -189   */
#define ABE_T_DROP              36   /* 0.6 s of fall                          */

/* THE BOUNCE. Two lobes, decaying, ending exactly back on the eye height —
   |sin| over two half-cycles scaled by (1 - u), so the first dip is about
   three quarters of ABE_BOUNCE_AMP and the second about a quarter of it. It is
   a landing compression, so it goes DOWN, which in this engine's coordinates
   means cam_y INCREASES toward the floor at y=0.

   54 units on the first dip against a 189-unit eye height is a visible knee-bend
   and not a camera fault. Much more and it reads as falling through the floor. */
#define ABE_BOUNCE_AMP          72   /* peak of the envelope; first dip ~54    */
#define ABE_T_BOUNCE            30   /* 0.5 s for both lobes                   */

/* 1.2 s to swing the aim from straight down onto his face. That is a 107-degree
   move (pitch +1024 to a measured -194), and it gets SMOOTHSTEP rather than the
   usual ease-out for exactly that reason — see the phase. */
#define ABE_T_PAN_UP            72

/* THE MOVE TO THE VANTAGE runs for the WHOLE of ABE_FAINT rather than for a
   phase of its own. The brief puts it "as Asag returns to the idle position",
   and in the clip that return is only the last half second — the faint from two
   seconds in plunges him to the floor, holds him there for two seconds, and
   only then lifts him back to Home. A camera move confined to that half second
   would be a jerk. Spread across the faint's remaining three seconds, eased in
   AND out, it is a drift that happens to arrive as he does.

   ABE_VANTAGE_* are solved in the shot list at the head of this file. */
#define ABE_VANTAGE_DX         650   /* to the right, in X                     */
#define ABE_VANTAGE_RISE       730   /* up; cam_y -919, about 3.8 deg of look  */

#define ABE_T_HANDOVER          60   /* 1.0 s back to the landing and level    */

/* How hard cam_pitch chases the solved aim: it closes 1/ABE_PAN_LAG of the
   remaining gap each frame. 8 is a ~0.13 s time constant, which swallows the
   8 fps pose stepping whole while trailing the body by about 4 units of pitch
   across the faint's 97-unit pan — under half a degree, and invisible.

   A DIVISOR RATHER THAN A LERP CONSTANT because the whole thing is integers:
   at gap 1 the step rounds to 0 and the chase would stall a unit short, which
   is why the snap below exists. */
#define ABE_PAN_LAG              8
#define ABE_PAN_SNAP             2   /* gap at which to just take the target  */

/* =========================================================================
   THE DEATH, WHICH IS THE RABISU'S ENDING PLAYED ON A BOSS THAT CANNOT MOVE
   =========================================================================
   The brief: "the game will take control of the camera. He will return to his
   starting Idle position and then freeze. He will then vibrate and explode
   exactly the same way Rabisu does when he is defeated. Control is then
   returned to the player."

   So it is rabisu_boss.c's RBE_D_* sequence — settle, freeze, burn, fade, camera
   back — with three differences, and all three come from the same root fact
   that Asag has no position of his own.

   >>> DIFFERENCE 1: THE SETTLE IS A CLIP AND A RAMP, NOT A LERP. <<< The Rabisu
   walks its body back to its spawn by writing r->x and r->z. Asag's vertices
   are baked in world space and the only thing that moves him is the position
   track, so "return to his starting Idle position" is asag_play(IDLE) plus
   asag_ramp_home() — and the ramp had to be added to src/asag.c for this, since
   the idle's own clip_move row INHERITS the offset it starts on. Kill him
   mid-slam and without the ramp he freezes 969 units out over the arena.

   >>> DIFFERENCE 2: THE CAMERA GOES TO THE VANTAGE IT ALREADY OWNS. <<< The
   opening scene solved a shot of his face that fits under this room's roofline
   (see ABE_VANTAGE_*, and the four-degree argument at the head of this file).
   The death wants exactly that shot and reuses it rather than solving a second
   one — which also means the death is framed the same way the reveal was, which
   is the read the brief is after.

   >>> DIFFERENCE 3: NO FACING OVERRIDE, BECAUSE HE HAS NO FACING. <<< Mistake 8
   in the runbook is a boss that spends its death looking at the door the player
   came in by, because the enemy layer targets the anchored player. Asag cannot
   turn at all, so the whole class of bug is absent here — and that is worth
   writing down, because its absence looks like an omission.

   THE BURN'S LENGTH IS THE RABISU'S, and deliberately: the two deaths are meant
   to read as the same phenomenon. The Rabisu's 232 frames were the length of
   SFX_EXPLODE; >>> NO SOUND PLAYS HERE YET <<< — the brief for this pass says
   sound comes later — so the number is currently the Rabisu's shape without its
   reason. When the clip lands, re-check it against the clip's own length the
   way RBE_T_D_BURN was. */
#define ABE_T_D_SETTLE        90   /* 1.5 s: camera to the vantage, body Home */
#define ABE_T_D_FREEZE       120   /* 2 s frozen, the Rabisu's                */
#define ABE_T_D_BURN         232   /* 3.87 s of shaking and light             */
#define ABE_T_D_FADE          90   /* 1.5 s burning away                      */
#define ABE_T_D_CAM_BACK      60   /* 1 s back to the player                  */

/* World units of jitter at the peak of the burn. The Rabisu's RBS_SHAKE_MAX is
   11 on a body 559 tall; Asag is 1669 long and much further from the camera, so
   the same 11 would be invisible. 26 subtends about the same angle at this
   room's ranges, which is the property that actually matters. */
#define ABE_D_SHAKE_MAX       26

/* =========================================================================
   THE LINES
   =========================================================================
   >>> WRAPPED BY HAND AT THEIR OWN PUNCTUATION. <<< btn_prompt_draw's font
   advances 8 px a character on a 320-wide screen, so FORTY CHARACTERS is the
   hard limit and both of the briefed lines are well past it. An automatic
   wrapper would break "a fever on the planets" wherever the count happened to
   run out; these break at the comma and at clause ends, which is where a voice
   would.

   THREE ROWS EACH, BOTTOM-ALIGNED, rather than the Rabisu's two: these lines
   are longer, and three rows of ~28 characters sit more comfortably than two of
   38. The row table is shared, so both lines stack up from the same baseline.

   The spelling is the brief's, "planets face" included. */
static const char *LINE_1[3] = {
    "Fear me mortal, for I bring",
    "pestilence and angst to your",
    "horrid little realm.",
};
static const char *LINE_2[3] = {
    "You are the true disease, a fever",
    "on the planets face and must",
    "be eradicated.",
};
#define ABE_TEXT_ROWS  3
static const int ABE_TEXT_Y[ABE_TEXT_ROWS] = { 180, 194, 208 };
#define ABE_TEXT_OT      2   /* menu-reserved range: on top of everything     */

/* =========================================================================
   STATE
   ========================================================================= */
typedef enum {
    ABE_IDLE = 0,    /* not running; the scene arms itself out of here        */
    ABE_DROP,        /* above the landing, looking down, falling              */
    ABE_BOUNCE,      /* landed: two decaying lobes, and the hurt sound        */
    ABE_PAN_UP,      /* pitch swings off the floor onto the held faint        */
    ABE_FAINT,       /* the faint resolving; camera drifts to the vantage     */
    ABE_BOILS,       /* idle looping; the boils come up over 2 s              */
    ABE_LINE1,       /* speech + music + the first subtitle                   */
    ABE_LINE2,       /* the second                                           */
    ABE_HANDOVER,    /* back to the landing, level off, let go                */
    ABE_FIGHT,       /* free play: src/asag_fight.c runs the attack loop       */

    /* ---- THE DEATH. Free play ends here and the camera comes back. ---- */
    ABE_D_SETTLE,    /* camera to the vantage; he plays an idle back to Home  */
    ABE_D_FREEZE,    /* held still, as briefed                                */
    ABE_D_BURN,      /* vibrating                                             */
    ABE_D_FADE,      /* burning away to nothing                               */
    ABE_D_CAM_BACK,  /* back to wherever the player was standing              */
    ABE_DONE,        /* over. Free play, and nothing left to drive.           */
} AbeState;

static AbeState state = ABE_IDLE;
static int32_t  phase_t;

/* THE LANDING, captured on the arm. Every position in this scene is an offset
   from it and the handover comes back to it, which is how "control returns at
   the original starting position" is guaranteed rather than asserted: the
   number the scene ends on is the same one it started from, so the two cannot
   drift apart. The director does NOT restate AA_SHAFT_X/Z — the room owns
   those. */
static int32_t save_cx, save_cy, save_cz, save_crot, save_pitch;

/* Where the aim is easing toward. BOTH angles now: the camera leaves the
   centre line for the vantage, so the yaw is live (see the note at the head of
   this file about why it used to be a constant and no longer is). */
static int32_t pan_yaw, pan_pitch;

/* The handover's start point, so its ease has something to leave from. */
static int32_t ho_x, ho_y, ho_z, ho_rot, ho_pitch;


/* =========================================================================
   AIMING
   ========================================================================= */

/* Shortest signed turn from `from` to `to`, in 4096ths. rabisu_boss.c's, and
   the reason any yaw interpolation has to go through it: lerping raw angles
   would take a camera at 4000 chasing 100 the long way round the circle. */
static int32_t turn_delta(int32_t from, int32_t to) {
    int32_t d = ((to - from) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    return d;
}

/* >>> ONE ROUTINE SOLVES BOTH ANGLES, BECAUSE THEY ARE THE SAME PROBLEM. <<<
   An aim angle here is always "the angle whose tangent is num/den":

     PITCH   num = dy (world +Y is DOWN, so a target ABOVE the camera gives a
             NEGATIVE answer), den = the HORIZONTAL distance to it
     YAW     num = dx, den = dz, because cam_rot 0 faces +Z and the forward
             vector is (sin, cos) of it - see update_camera

   So this is rabisu_boss.c's aim_pitch with the pitch-specific naming taken
   out. That file computes only a pitch, because its camera never leaves the
   boss's own X and its yaw is therefore a constant; this one needs both.

   >>> COPIED RATHER THAN SHARED, AND THAT IS A JUDGEMENT WORTH RECORDING. <<<
   It is eleven lines and it depends on nothing; the alternative is a
   camera_aim_angle() in camera.c, which is probably where it ends up on the
   THIRD encounter. Two copies of eleven self-contained lines is cheaper than
   the wrong abstraction, and the comment in both says the other exists.

   The SDK has no arctangent, so it is a binary search on the SDK's own
   isin/icos: eleven halvings resolve a quarter turn to one unit, and answering
   in the same trig table camera_build_view() projects through means the aim
   cannot disagree with the picture. The compare is cross-multiplied, so there
   is no division and nothing overflows - 4096 * 2300 is well inside an int32.

   BOTH USES KEEP den > 0: Asag is always in front of the camera (his face runs
   z 1450..2300 against a camera at z 300) and a horizontal distance is positive
   by construction. A caller that could put him BEHIND the camera would need the
   full atan2 range, and this is not it. */
static int32_t aim_angle(int32_t num, int32_t den) {
    int32_t lo = -1024, hi = 1024;          /* +/- 90 deg */
    if (den < 1) den = 1;
    while (hi - lo > 1) {
        /* & 4095 because the answer is genuinely negative for much of this
           scene and isin/icos are documented over 0..4095. The mask is exact,
           not a clamp: both are periodic in 4096 and two's complement wraps
           the right way. */
        int32_t mid = (lo + hi) >> 1, a = mid & 4095;
        if (isin(a) * den < icos(a) * num) lo = mid;
        else                               hi = mid;
    }
    return lo;
}

/* Integer square root, the same Newton-by-halving routine src/rabisu.c and
   src/web.c use. It is here for the PITCH's denominator, which is a genuine
   horizontal distance now that the camera leaves the centre line: using dz
   alone would overstate the pitch by dz/sqrt(dx^2+dz^2), which at the vantage's
   18 degrees off axis is 5%. Small - and the sort of small that is
   indistinguishable from a framing mistake when a shot looks slightly wrong and
   nothing explains why. */
static int32_t isqrt32(int32_t v) {
    if (v <= 0) return 0;
    int32_t x = v, last;
    if (x > 1 << 16) x = 1 << 16;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

/* Re-solve the aim at HIS FACE from wherever the camera is this frame, into
   pan_yaw/pan_pitch.

   >>> THE FACE, NOT THE BODY CENTRE. <<< He is 1669 units long and lies along
   the view axis, so his centre is his flank and sits some 800 units further off
   than his head. asag_face_point() exists for this and asag.h has the argument.

   >>> BOTH ENDS OF THIS MOVE, WHICH IS WHY ORDER MATTERS AT THE CALL SITES.
   <<< The camera glides to the vantage while the body animates, so neither the
   eye nor the target is fixed. Every phase below writes the camera's POSITION
   first and calls this second; reversed, the aim would trail the position by a
   frame for the whole three-second drift.

   Holds the previous target when the body cannot be located - a camera handed a
   zero there would swing to the origin, which is the floor at the player's
   feet. */
static void solve_aim(void) {
    VECTOR f;
    if (!asag_face_point(&f)) return;
    int32_t dx = (int32_t)f.vx - cam_x;
    int32_t dy = (int32_t)f.vy - cam_y;
    int32_t dz = (int32_t)f.vz - cam_z;
    pan_yaw   = aim_angle(dx, dz) & 4095;
    pan_pitch = aim_angle(dy, isqrt32(dx * dx + dz * dz));
}

/* Close one angle a fraction of the way onto its target.

   >>> THE SNAP IS NOT DECORATION. <<< gap/LAG is integer division, so at a gap
   of 1 the step rounds to 0 and the chase would park a unit short of the target
   forever. */
static int32_t chase(int32_t cur, int32_t gap) {
    if (gap > -ABE_PAN_SNAP && gap < ABE_PAN_SNAP) return cur + gap;
    return cur + gap / ABE_PAN_LAG;
}

/* Aim at his face, smoothed. See the head of this file for why the smoothing
   exists at all: the pose steps at 8 fps under a 60 Hz camera, so a raw aim
   jumps eight times a second and the worst single step through the faint is 54
   units of pitch.

   THE YAW GOES THROUGH turn_delta, not through a plain subtraction. Both angles
   are masked 0..4095, so a camera at 4090 chasing a target at 10 has a raw gap
   of -4080 and would spin the long way round to cover 20 units. It cannot
   happen on this scene's numbers - the yaw stays within about 300 of zero
   throughout - and it is one call to make it impossible rather than merely
   unlikely. */
static void pan_step(void) {
    solve_aim();
    cam_rot   = chase(cam_rot, turn_delta(cam_rot, pan_yaw)) & 4095;
    cam_pitch = chase(cam_pitch, pan_pitch - cam_pitch);
    cam_vy    = 0;
}

/* =========================================================================
   PREDICATES
   ========================================================================= */

/* 1 while the script owns the camera. main.c's early-return branch and its
   menu/HUD suppression both hang off this. ABE_FIGHT is free play and must
   fall through, exactly as the Rabisu's RBE_FIGHT does. */
int asag_boss_cutscene(void) {
    /* >>> ABE_DONE IS FREE PLAY AND MUST FALL THROUGH, exactly as ABE_FIGHT
       does. <<< It is the state the player spends the rest of the session in
       once the boss is dead; leaving it inside the predicate would suppress
       their camera, their menu and their HUD forever. rabisu_boss_cutscene()
       excludes its RBE_DONE for the same reason. */
    return state != ABE_IDLE && state != ABE_FIGHT && state != ABE_DONE;
}

/* =========================================================================
   LIFETIME
   ========================================================================= */

/* Called from asag_arena_init(), i.e. on EVERY arrival — the drop, a debug
   level-select jump, a load — so all three see the same scene. It PARKS the
   director and lets the first update arm it; see the note there.

   Everything the scene touched is put back here, because this also runs when
   the player arrives having already seen it once. */
void asag_boss_reset(void) {
    state      = ABE_IDLE;
    phase_t    = 0;
    pan_yaw    = 0;
    pan_pitch  = 0;
    save_pitch = 0;
    camera_release_player();
    cam_pitch = 0;
    /* >>> AND THE BODY'S HOLD, which this scene is the only thing that sets.
       <<< Leaving it on would freeze Asag for the whole of the next visit: the
       clips would be chosen and never advance, which presents as five clips
       that all failed to load. asag_reset() also clears it on every arrival,
       and that is the belt to this braces - this path is the one that runs when
       the player quits to the title mid-drop and never reaches an arrival. */
    asag_set_frozen(0);
    asag_arena_set_boil_glow(0);
    /* ...and the body's death pose, for the same reason: a fade left at 0 would
       make the boss invisible for the whole of the next visit, which presents
       as a model that failed to load rather than as a scene that was cut off. */
    asag_set_shake(0);
    asag_set_fade(256);
    /* THE FIGHT GOES BACK TO ITS START HERE and nowhere else. It has no reset
       of its own on the arrival path — this runs from asag_arena_init(), i.e.
       on every arrival, which is exactly the guarantee it needs. */
    asag_fight_reset();
    /* A long clip on a dedicated voice, so nothing else would ever cut it: a
       new game or a level jump made while Asag was mid-sentence would otherwise
       carry the speech into the next room. The Rabisu's reset stops its three
       for the same reason. */
    sound_stop(SFX_DMNSPEAK);
    /* ...and the explosion, for the same reason: a 5.4 s clip on a dedicated
       voice would otherwise carry a half-finished death into the next room if
       the player quit to the title or jumped out mid-sequence. The Rabisu's
       reset stops the same clip. */
    sound_stop(SFX_EXPLODE);
}

/* =========================================================================
   PHASES
   ========================================================================= */

static void enter_phase(AbeState s) {
    state   = s;
    phase_t = 0;
}

/* ---- The eases -------------------------------------------------------------
   Two curves, and which one a move gets is not a preference:

   EASE-OUT (leaves fast, settles) is right for a move the player did not ask
   for and should not notice starting - the pan up off the floor, and the
   handover. It is the curve every other scripted camera in this game uses.

   SMOOTHSTEP (eases IN and out) is for the long drift to the vantage. Three
   seconds of ease-out reads as a camera being yanked and then coasting; this
   one has to leave without announcing itself and arrive without stopping dead.
   Same reasoning as the Rabisu's ten-second pull-back, which uses the same
   curve for the same reason.

   Both in 256ths. `p` is 0..256. */
static int32_t ease_out(int32_t p) {
    int32_t inv = 256 - p;
    return 256 - (inv * inv / 256);
}

static int32_t smoothstep(int32_t p) {
    /* 3p^2 - 2p^3, peaking at 50M before the shift - inside an int32. */
    return (p * p * (3 * 256 - 2 * p)) / (256 * 256);
}

/* phase_t as a 0..256 fraction of `len`, clamped. */
static int32_t phase_p(int32_t len) {
    int32_t p = (phase_t * 256) / len;
    return p > 256 ? 256 : p;
}

/* Straight into the idle, looping, with the boils about to come up. Reached
   from the end of the faint and also from the arm if the faint never loaded. */
static void begin_boils(void) {
    /* LOOPING, unlike every clip the demo plays. The brief asks for the idle to
       hold under the boils and the whole of the speech, which is 14 seconds
       against the clip's 1.25 - so this is the one place a `loop` argument of 1
       is right. It also means asag_clip_done() is false from here on, which is
       why the phases after it are all on frame counts. */
    asag_play(ASAG_CLIP_IDLE, 1);
    asag_set_frozen(0);
    asag_arena_set_boil_glow(0);
    enter_phase(ABE_BOILS);
}

/* Release the held faint and start the drift to the vantage. */
static void begin_faint(void) {
    asag_set_frozen(0);
    enter_phase(ABE_FAINT);
}

/* Take the camera, put it in the air over the landing, and hold Asag where he
   is while it falls. */
static void begin_scene(void) {
    /* THE LANDING, and every position in the scene is measured off it. Captured
       rather than restated: asag_arena_spawn_shaft() owns where the player
       arrives, and a second copy of those numbers here would be a second thing
       to keep in step. */
    save_cx    = cam_x;
    save_cy    = cam_y;
    save_cz    = cam_z;
    save_crot  = cam_rot;
    save_pitch = cam_pitch;

    /* >>> ANCHOR AT THE LANDING, NOT AT THE CAMERA'S START. <<< The camera is
       about to jump into the air and travel; the PLAYER is standing on the
       floor at the landing for the whole scene. camera.h explains why this
       matters: anything reading cam_* to find the player would otherwise spend
       the scene chasing a camera around the room. It is also what makes the
       handover exact - releasing the anchor with the camera back at the landing
       puts the player precisely where they started. */
    camera_anchor_player(save_cx, save_cy, save_cz);

    asag_set_visible(1);

    /* HIM FIRST, because the pan-up's target is solved off his pose and the
       drop needs him already sitting there. Two seconds in, pose and travel
       together, then HELD - the brief says he is "sitting" in the faint, which
       is a pose held rather than a clip playing, and without the freeze the
       position track would slide him out of the wall while the camera falls.
       See ABE_FAINT_ENTRY_TICKS and asag_play_at()/asag_set_frozen() in
       src/asag.h. */
    int have_faint = asag_clip_loaded(ASAG_CLIP_FAINT);
    if (have_faint) {
        asag_play_at(ASAG_CLIP_FAINT, 0, ABE_FAINT_ENTRY_TICKS);
        asag_set_frozen(1);
    }

    /* UP IN THE AIR, LOOKING STRAIGHT DOWN. +1024 is a quarter turn of pitch
       and, world +Y being down, it is the floor. What is under the camera is
       the arena's mud - which is exactly what the transition that just ended
       was showing, so the cut into the room lands on the same picture it left
       on. */
    cam_x     = save_cx;
    cam_y     = save_cy - ABE_DROP_RISE;
    cam_z     = save_cz;
    cam_rot   = save_crot;
    cam_pitch = 1024;
    cam_vy    = 0;

    /* Seed the aim so the first pan_step has somewhere to come from rather than
       a zero. Nothing reads these until ABE_PAN_UP. */
    pan_yaw   = cam_rot;
    pan_pitch = cam_pitch;

    enter_phase(ABE_DROP);

    if (!have_faint) {
        /* THE FAINT DID NOT LOAD, which src/asag.c makes a silent condition -
           read_file() refuses an allocation that would reach the stack, and the
           faint is LAST in the read order, so it is the clip that goes missing
           when the heap runs short (tools/ADDING_THE_ASAG_FIGHT.txt PART 6).

           The ARRIVAL still plays: the drop, the bounce and the pan up are the
           player's own, they are the best part of the scene and they do not
           depend on him. Only the faint's own beat is skipped, by ABE_FAINT
           below, which finds no clip and falls through on its backstop. That
           way a missing clip costs one beat and stays visible as one, rather
           than looking like bad animation - the same trade the demo's
           next_loaded() makes. */
    }
}

/* Hand it back: glide to the landing, level off, let go. */
static void begin_handover(void) {
    ho_x     = cam_x;
    ho_y     = cam_y;
    ho_z     = cam_z;
    ho_rot   = cam_rot;
    ho_pitch = cam_pitch;
    enter_phase(ABE_HANDOVER);
}

/* =========================================================================
   ABE_FIGHT AND THE DEATH
   =========================================================================
   >>> THE DEMO IS GONE. <<< For three passes this phase cycled the four attack
   clips end to end on a loop so the animation could be looked at, and every
   comment here said so in capitals. src/asag_fight.c is the real thing now: the
   attack loop, the two boils, the exposure windows, the damage and everything
   the attacks throw. This file's job during ABE_FIGHT is one line — watch for
   the kill — and that is the split the runbook's STEP 1 asks for.

   WHAT SURVIVED THE DEMO, because both are still true of the real fight:

     AN ABSENT CLIP MUST BE STEPPED OVER, NOT WAITED ON. src/asag.c's read_file()
     refuses an allocation that would reach the stack, SILENTLY, and a clip with
     no frames never advances and never reports asag_clip_done(). The demo used
     next_loaded() for this; the fight uses a per-phase watchdog built on
     asag_clip_ticks(), which reports 0 for exactly that clip. Same failure, same
     answer: the beat is missing and obviously missing, rather than a hang that
     looks like a frozen boss.

     WHICH CLIP GOES MISSING IS NOT RANDOM. asags_load_model() reads them in
     AsagClip order and the heap is what runs out, so it is always the LAST ones
     in that order — the faint first. If the faint stops happening, the fix is
     the budget (tools/ADDING_THE_ASAG_FIGHT.txt PART 6), not the art.
   ========================================================================= */

/* Take the camera back and start the death. Called from ABE_FIGHT the frame
   asag_fight_dying() goes up.

   >>> THE FIGHT IS STOPPED THROUGH ONE CALL, NOT BY CLEARING FIELDS HERE. <<<
   asag_fight_stop() kills the attack loop, every boulder, every puss ball, the
   burning floor and the boils' lights together. STEP 12 of the runbook asks for
   exactly that shape — "route it all through the go-dormant call rather than
   clearing fields at each call site" — and the reason is that a projectile in
   the air outliving the thing that threw it is the bug nobody tests for.
   asag_damage() already made the call on the killing blow; this is the belt to
   those braces, for the path where the director notices `dying` set by
   something else. */
static void begin_death(void) {
    asag_fight_stop();

    /* THE CAMERA IS THE PLAYER'S UNTIL THIS FRAME. Anchor them where they are
       standing — which, unlike the opening, is wherever they happened to be
       when they landed the kill, not a spawn constant. That is what makes
       "control is then returned to the player" put them back where they were
       rather than back at the start of the fight; it is mistake 4 in the
       runbook and it is the whole reason save_cx/cy/cz are re-captured here. */
    save_cx    = cam_x;
    save_cy    = cam_y;
    save_cz    = cam_z;
    save_crot  = cam_rot;
    save_pitch = cam_pitch;
    camera_anchor_player(save_cx, save_cy, save_cz);

    ho_x = cam_x; ho_y = cam_y; ho_z = cam_z;
    ho_rot = cam_rot; ho_pitch = cam_pitch;

    /* "He will return to his starting Idle position and then freeze."
       Both halves, and they are two different mechanisms: the POSE is the idle
       clip, and the POSITION is a ramp, because the idle's clip_move row
       INHERITS whatever offset it starts on (src/asag.h). Kill him mid-slam and
       an idle alone would freeze him 969 units out over the arena. */
    asag_play(ASAG_CLIP_IDLE, 1);
    asag_ramp_home(ABE_T_D_SETTLE);

    enter_phase(ABE_D_SETTLE);
}

/* The camera's move to the vantage during the settle. THE SAME VANTAGE THE
   OPENING SOLVED — see the death block in the timing section for why it is
   reused rather than re-solved, and the four-degree roofline argument at the
   head of this file for why that shot is the one this room allows.

   Position first, aim second, for the reason every other moving shot in this
   file gives: solve_aim() reads cam_*, so the other order leaves the aim
   trailing the position by a frame. */
static void death_settle_step(int32_t t) {
    int32_t p = (t * 256) / ABE_T_D_SETTLE;
    if (p > 256) p = 256;
    int32_t e = smoothstep(p);
    cam_x = ho_x + ((save_cx + ABE_VANTAGE_DX   - ho_x) * e) / 256;
    cam_y = ho_y + ((save_cy - ABE_VANTAGE_RISE - ho_y) * e) / 256;
    cam_z = ho_z + ((save_cz                    - ho_z) * e) / 256;
    pan_step();
}

/* =========================================================================
   UPDATE
   =========================================================================
   Called from main.c TWICE: from the cutscene early-return at the top of
   update_current_area(), and from the arena's free-play branch. Both call it
   BEFORE asag_update(), and that order is worth exactly one frame — see the
   note at the free-play call site and the one in the demo section below. */
void asag_boss_update(void) {
    /* Nothing to drive until the model is in. Arming against a body that has
       not been read yet would burn the faint's two-second entry on a bind pose,
       and ARMING IS WHY THIS IS LAZY: asag_arena_init() runs before the model
       load and before world_enter, so any decision taken in the reset about
       whether there is a boss to reveal would be a frame early. The Rabisu's
       director parks itself for the same reason. */
    if (state == ABE_IDLE) {
        if (current_area == STATE_ASAG_ARENA && asag_model_loaded())
            begin_scene();
        return;
    }

    /* A debug level-select jump or a load can pull the model out from under a
       running script. Put the camera back and go to free play rather than
       driving a bodiless scene — the same bail-out the runbook's STEP 4 asks
       for, testing the thing that can actually vanish. */
    if (!asag_model_loaded()) {
        /* >>> PUT THE CAMERA BACK ON THE FLOOR, NOT JUST BACK TO LEVEL. <<<
           This used to restore the pitch alone, which was correct while the
           scene never moved the camera. It does now: bail out during the drop
           or at the vantage and releasing the anchor would leave the player
           standing 730 units in the air, or off to one side of the arena, for
           the rest of the session. Every field the scene writes is restored
           here, in the same order the handover restores them. */
        cam_x     = save_cx;
        cam_y     = save_cy;
        cam_z     = save_cz;
        cam_rot   = save_crot;
        cam_pitch = save_pitch;
        cam_vy    = 0;
        camera_release_player();
        asag_set_frozen(0);
        asag_set_shake(0);
        asag_set_fade(256);
        sound_stop(SFX_DMNSPEAK);
        /* >>> AND GO TO DONE, NOT TO FIGHT. <<< This used to drop into the
           placeholder fight, which was harmless when the fight decided nothing.
           It is not now: there is no body to attack with, to expose or to kill,
           so an attack loop here would run its clips against a missing model and
           the player would stand in an empty arena being hurt by a boss that is
           not drawn. asag_fight_stop() takes down anything already in the air on
           the way past. */
        asag_fight_stop();
        enter_phase(ABE_DONE);
        return;
    }

    phase_t++;

    switch (state) {

    /* ---- THE ARRIVAL ------------------------------------------------------
       The fall. x and z are fixed, the pitch is fixed straight down, and only
       cam_y moves - on t^2, because that is what an accelerating fall looks
       like and it is the same curve the two halves of this fall before it use
       (hatch_puzzle.c's HP_FALL and door_anim.c's mud plane).

       NO pan_step HERE. The camera is looking at the floor on purpose; solving
       an aim at Asag would drag it off the ground it is falling toward. */
    case ABE_DROP: {
        int32_t p   = phase_p(ABE_T_DROP);
        int32_t acc = (p * p) / 256;
        cam_y  = (save_cy - ABE_DROP_RISE) + (ABE_DROP_RISE * acc) / 256;
        cam_vy = 0;
        if (phase_t >= ABE_T_DROP) {
            cam_y = save_cy;
            /* THE LANDING. The hurt sound, as briefed - and it is a SOUND and
               not damage: nothing in this scene can touch the player, and
               taking health off them for an arrival they did not choose would
               be a different design decision than the one asked for. SFX_HURT
               is RESIDENT (sound.h), so it plays in this arena whichever bank
               is loaded. */
            sound_play(SFX_HURT);
            enter_phase(ABE_BOUNCE);
        }
        break;
    }

    /* Two decaying lobes of |sin|, ending exactly back on the landing height.
       See ABE_BOUNCE_AMP. Still looking down - the knee-bend is part of the
       arrival, not part of the reveal. */
    case ABE_BOUNCE: {
        int32_t p = phase_p(ABE_T_BOUNCE);
        /* TWO lobes, which means sweeping ONE full turn and taking |sin| —
           |sin| has two humps per revolution. Sweeping two turns, which is the
           easy mistake and the one this had first, gives FOUR: measured, dips
           of 60, 33, 26 and 7, where the first two are close enough together to
           read as a rattle rather than a bounce. One turn gives 54 and 20.

           The (256 - p) factor is the decay, and it is also what lands the
           phase exactly back on the eye height — |sin| is 0 at p=256 anyway,
           so the two agree at the end rather than fighting. */
        int32_t sn = isin(((p * 4096) / 256) & 4095);
        if (sn < 0) sn = -sn;
        cam_y  = save_cy + (((ABE_BOUNCE_AMP * sn) / 4096) * (256 - p)) / 256;
        cam_vy = 0;
        if (phase_t >= ABE_T_BOUNCE) {
            cam_y = save_cy;
            enter_phase(ABE_PAN_UP);
        }
        break;
    }

    /* ---- PAN UP ONTO HIM --------------------------------------------------
       The camera is on the floor at the landing and stays there; only the aim
       moves, from straight down (+1024) to wherever his held pose puts his face
       - measured at -194, so this is a swing of about 106 degrees.

       >>> IT IS A ONE-OFF EASE, NOT pan_step's CHASE. <<< The chase closes a
       fixed FRACTION of the gap per frame, which across 1200 units would spend
       most of a second creeping through the last few degrees and never quite
       arrive. An explicit ease over a known duration lands on time. pan_step
       takes over in ABE_FAINT, where the gaps are small and the target moves.

       >>> AND IT IS SMOOTHSTEP, NOT ease_out, WHICH IS THE OPPOSITE OF WHAT THE
       HANDOVER WANTS. <<< This is a 107-degree swing and the runbook's rule
       applies — ease IN and out for a long move. With ease_out it is a WHIP:
       measured, 70% of the travel is spent in the first twelve frames and the
       remaining 36 creep through the last thirty degrees. Smoothstep over 72
       frames reads as somebody deliberately raising their head.

       The target is RE-SOLVED every frame even though the body is frozen: it
       costs one search, and it keeps the beat correct if a later version lets
       him move during it. */
    case ABE_PAN_UP: {
        solve_aim();
        int32_t e = smoothstep(phase_p(ABE_T_PAN_UP));
        cam_pitch = 1024 + ((pan_pitch - 1024) * e) / 256;
        cam_rot   = (save_crot + (turn_delta(save_crot, pan_yaw) * e) / 256) & 4095;
        cam_vy    = 0;
        if (phase_t >= ABE_T_PAN_UP) {
            cam_pitch = pan_pitch;
            cam_rot   = pan_yaw;
            begin_faint();
        }
        break;
    }

    /* ---- THE FAINT RESOLVES, AND THE CAMERA DRIFTS TO THE VANTAGE ---------
       Both ends of the shot move at once: he plunges to the floor, holds there,
       and rises back to Home, while the camera eases up and to the right.

       >>> POSITION FIRST, AIM SECOND. <<< solve_aim() reads cam_*, so the other
       order would leave the aim trailing the position by a frame for the whole
       three seconds of drift.

       SMOOTHSTEP, not ease-out - see the note by the curves. And the drift runs
       for the WHOLE phase rather than only for the half second in which he
       actually returns to Home; ABE_VANTAGE_DX has that argument. */
    case ABE_FAINT: {
        int32_t e = smoothstep(phase_p(ABE_T_FAINT_MAX));
        cam_x = save_cx + (ABE_VANTAGE_DX   * e) / 256;
        cam_y = save_cy - (ABE_VANTAGE_RISE * e) / 256;
        cam_z = save_cz;
        pan_step();
        /* >>> asag_clip_done() IS READ ONE FRAME LATE, ON PURPOSE. <<< This
           runs BEFORE asag_update(), which is what raises the flag, so by the
           time it is seen here the clip's final pose has already been drawn
           once. Reverse the two calls in main.c and the faint's last frame is
           skipped - which is most of the body arriving back at Home.

           THE DRIFT IS SCALED TO THE BACKSTOP, NOT TO THE CLIP, which is worth
           knowing before changing either number: the clip takes 180 frames and
           ABE_T_FAINT_MAX is 240, so smoothstep has the camera 84% of the way
           to the vantage when the faint ends. ABE_BOILS carries the last sixth
           rather than snapping - see there. */
        if (asag_clip_done() || phase_t >= ABE_T_FAINT_MAX)
            begin_boils();
        break;
    }

    case ABE_BOILS:
        /* FINISH THE DRIFT. The faint ends before the camera has arrived (see
           the note above), so this carries the same move to its end instead of
           snapping - which is what "as Asag returns to the idle position the
           camera moves up and to the right" actually asks for, since his return
           and the camera's arrival should coincide rather than one waiting for
           the other. The chase is the same fraction-of-the-gap closer the aim
           uses, so it cannot overshoot the vantage. */
        cam_x += chase(0, (save_cx + ABE_VANTAGE_DX)   - cam_x);
        cam_y += chase(0, (save_cy - ABE_VANTAGE_RISE) - cam_y);
        pan_step();
        /* Linear, not eased. Two seconds of something swelling at a constant
           rate reads as a thing filling up; an ease-out reads as a dimmer being
           turned. The PULSE afterwards is asag_arena.c's and starts the moment
           this reaches full - see the note by boil_lit() there. */
        asag_arena_set_boil_glow((phase_t * ASAG_BOIL_LEVEL_MAX) / ABE_T_BOILS);
        if (phase_t >= ABE_T_BOILS) {
            asag_arena_set_boil_glow(ASAG_BOIL_LEVEL_MAX);
            /* THE MUSIC AND THE FIRST UTTERANCE START TOGETHER, on the frame
               the first subtitle appears. The arena has been silent since the
               drop (main.c's loading branch stops the drive for this room), so
               apart from the landing this is the first sound in it.

               >>> THE TRACK IS BORROWED. <<< CDAUDIO_ASAG_TRACK currently points
               at the Garden Courtyard's, because Asag has no master of his own
               on the disc yet; src/cdaudio.h says what the one-line change is.
               Nothing here needs to know. */
            cdaudio_play(CDAUDIO_ASAG_TRACK, 1);
            sound_play(SFX_DMNSPEAK);
            enter_phase(ABE_LINE1);
        }
        break;

    case ABE_LINE1:
        pan_step();
        if (phase_t >= ABE_T_LINE) {
            /* One utterance per line, fired as the line appears. The clip is
               8.6 s against a 6 s line, so this second play cuts the first
               short - which is the point, and is why it is two calls at two
               phase entries rather than one long one: the speech has to break
               where the text does. Both are on the same voice (sound.c), so the
               cut is the hardware's. Same arrangement as the Rabisu's. */
            sound_play(SFX_DMNSPEAK);
            enter_phase(ABE_LINE2);
        }
        break;

    case ABE_LINE2:
        pan_step();
        if (phase_t >= ABE_T_LINE) begin_handover();
        break;

    /* ---- BACK TO THE LANDING ----------------------------------------------
       Position, yaw and pitch together, eased out, from wherever the vantage
       left them to exactly the numbers the scene started from.

       >>> THE PITCH HAS TO REACH ZERO AND NOTHING ELSE WILL TAKE IT THERE. <<<
       Free-look gameplay never sets cam_pitch and so never clears it; a scene
       that let go at +43 would hand the player a camera tilted permanently at
       the floor, with no input that could level it. camera.h says so and it is
       mistake 1 in tools/ADDING_A_BOSS_ENCOUNTER.txt.

       THE POSITION MATTERS FOR THE SAME REASON AND MORE VISIBLY: release the
       anchor with the camera still at the vantage and the player is left
       standing in mid-air 650 units to the right of where they landed. That is
       the trick behind "control returns and they are back at the original
       starting position" - it is not a teleport, it is the camera arriving. */
    case ABE_HANDOVER: {
        int32_t e = ease_out(phase_p(ABE_T_HANDOVER));
        cam_x     = ho_x    + ((save_cx - ho_x) * e) / 256;
        cam_y     = ho_y    + ((save_cy - ho_y) * e) / 256;
        cam_z     = ho_z    + ((save_cz - ho_z) * e) / 256;
        cam_rot   = (ho_rot + (turn_delta(ho_rot, save_crot) * e) / 256) & 4095;
        cam_pitch = ho_pitch - (ho_pitch * e) / 256;
        cam_vy    = 0;
        if (phase_t >= ABE_T_HANDOVER) {
            /* Snap to the saved numbers rather than trusting the last
               interpolation: an ease in 256ths can land a unit or two short,
               and a unit or two short of the landing is where the player then
               spends the whole fight. */
            cam_x     = save_cx;
            cam_y     = save_cy;
            cam_z     = save_cz;
            cam_rot   = save_crot;
            cam_pitch = 0;
            cam_vy    = 0;
            /* Releasing the anchor makes the camera the player again - and the
               camera is back at the landing, so they are standing exactly where
               the shaft dropped them, facing +Z down the arena at Asag. The
               spawn already obeys the room's rules and sits clear of the
               push-out boundary (z=300 against a 195 wall radius - see
               AA_SHAFT_Z in asag_arena.c), which is why this scene needs no
               fight position of its own.

               NO DOOR RE-ARM, because there is no door. See difference B at the
               head of this file. */
            camera_release_player();
            /* AND THE FIGHT STARTS ON THIS FRAME. src/asag_fight.c takes it
               from here: the loop opens on the laser, as briefed. */
            asag_fight_begin();
            enter_phase(ABE_FIGHT);
        }
        break;
    }

    /* ---- FREE PLAY --------------------------------------------------------
       src/asag_fight.c is driving. The director's only business here is to
       notice the kill — which is `dying` and not `dead`, two flags and not one
       (STEP 11): the body has to stay drawn for the sequence that follows. */
    case ABE_FIGHT:
        if (asag_fight_dying()) begin_death();
        break;

    /* ---- THE DEATH --------------------------------------------------------
       The camera drifts to the vantage the opening already solved while he
       plays an idle and the position ramp carries him back to Home. */
    case ABE_D_SETTLE:
        death_settle_step(phase_t);
        if (phase_t >= ABE_T_D_SETTLE) {
            cam_x = save_cx + ABE_VANTAGE_DX;
            cam_y = save_cy - ABE_VANTAGE_RISE;
            cam_z = save_cz;
            /* "...and then freeze." Both tracks: asag_set_frozen() stops the
               position clock as well as the pose, which is the whole point of
               it — see src/asag.h. The ramp has reached Home by now anyway, but
               a freeze that only stopped the pose would let a rounding remainder
               keep sliding him for the next seven seconds. */
            asag_set_frozen(1);
            enter_phase(ABE_D_FREEZE);
        }
        break;

    case ABE_D_FREEZE:
        pan_step();
        if (phase_t >= ABE_T_D_FREEZE) {
            /* >>> WITH THE LIGHTS, NOT WITH THE KILLING BLOW. <<< The death
               lights start on this exact frame (see the draw below), so the clip
               and the thing it is the sound of begin together, and its 5.37
               seconds run under ABE_T_D_BURN + ABE_T_D_FADE - 232 + 90 = 322
               frames = 5.37 s. Those two constants ARE the clip's length,
               exactly as RBE_T_D_BURN and RBE_T_D_FADE are; retrim EXPLODE.VAG
               and both pairs have to move. src/sound.h says so too.

               >>> AND SFX_EXPLODE HAD TO BE TAGGED BOSS|ASAG TO BE AUDIBLE HERE.
               <<< It was BOSS-only, and SND_BANK_ASAG is the only bank loaded in
               this arena - so the whole death would have played in silence, with
               no error anywhere, which is the exact failure mode at the head of
               src/sound.c's bank table. The STEP 3 arithmetic for the extra copy
               is in tools/ADDING_A_SOUND.txt and it was free.

               Voice 21 is its own, so nothing in the burn can cut it short. */
            sound_play(SFX_EXPLODE);
            enter_phase(ABE_D_BURN);
        }
        break;

    case ABE_D_BURN:
        pan_step();
        /* The shake BUILDS rather than switching on: he is coming apart, and
           four seconds of constant rattle reads as a broken camera. Same curve
           the Rabisu's does. */
        asag_set_shake((ABE_D_SHAKE_MAX * phase_t) / ABE_T_D_BURN);
        if (phase_t >= ABE_T_D_BURN) enter_phase(ABE_D_FADE);
        break;

    case ABE_D_FADE: {
        pan_step();
        int32_t f = 256 - (phase_t * 256) / ABE_T_D_FADE;
        if (f < 0) f = 0;
        asag_set_fade(f);
        if (phase_t >= ABE_T_D_FADE) {
            asag_set_fade(0);
            asag_set_shake(0);
            /* NOW he is gone. `dead` is the director's to set and it is set
               here, at the end of the fade and not on the killing blow —
               STEP 11, and setting it earlier would have blinked the body out
               at the exact moment the player was meant to watch it come
               apart. */
            asag_fight_set_dead();
            asag_set_visible(0);
            /* THE MUSIC STOPS WITH HIM. The arena was silent before the
               encounter (main.c's loading branch) and is silent after it; the
               track was the boss's, not the room's. */
            cdaudio_stop();
            ho_x = cam_x; ho_y = cam_y; ho_z = cam_z;
            ho_rot = cam_rot; ho_pitch = cam_pitch;
            enter_phase(ABE_D_CAM_BACK);
        }
        break;
    }

    /* ---- BACK TO THE PLAYER -----------------------------------------------
       To where they were STANDING WHEN THEY LANDED THE KILL, which begin_death()
       captured — not to the landing the opening scene used. Killing him from a
       corner of the arena and being put back in the middle of it is mistake 4's
       symptom and the runbook's last emulator check. */
    case ABE_D_CAM_BACK: {
        int32_t e = ease_out(phase_p(ABE_T_D_CAM_BACK));
        cam_x     = ho_x    + ((save_cx - ho_x) * e) / 256;
        cam_y     = ho_y    + ((save_cy - ho_y) * e) / 256;
        cam_z     = ho_z    + ((save_cz - ho_z) * e) / 256;
        cam_rot   = (ho_rot + (turn_delta(ho_rot, save_crot) * e) / 256) & 4095;
        cam_pitch = ho_pitch - (ho_pitch * e) / 256;
        cam_vy    = 0;
        if (phase_t >= ABE_T_D_CAM_BACK) {
            cam_x     = save_cx;
            cam_y     = save_cy;
            cam_z     = save_cz;
            cam_rot   = save_crot;
            cam_pitch = 0;      /* nothing else will ever clear it */
            cam_vy    = 0;
            camera_release_player();
            state = ABE_DONE;
            /* ...and that is the end of it. Free play, in a silent arena, with
               a boss that is gone.

               >>> AND NO WAY OUT OF THE ROOM, WHICH IS THE OPEN HOLE. <<< The
               arena still has no exit — src/asag_arena.h says so in as many
               words, and it was already the most urgent item in
               tools/ADDING_THE_ASAG_FIGHT.txt PART 7 before the fight existed.
               It is more urgent now: winning used to be impossible and is not,
               so a player who does everything right is left standing in a
               finished room. The seal (STEP 9) hangs off the same decision:
               asag_boss_seals_door() would be `state != ABE_IDLE && state !=
               ABE_DONE`, and the re-arm goes on this frame. */
        }
        break;
    }

    default:
        break;
    }
}

/* =========================================================================
   DRAWING: THE SUBTITLES
   =========================================================================
   btn_prompt_draw sorts its text into the OT via FntSort — never FntFlush,
   which draws IMMEDIATELY and races a scene still being laid down (see the note
   on main.c's debug overlay). The font advances 8 px a character, so centring
   is a character count and a subtraction.

   Called from the foot of asag_arena_draw(), after the world and in screen
   space. */
static void abe_line(RenderContext *ctx, const char *s, int y) {
    int w = (int)strlen(s) * 8;
    int x = (SCREEN_XRES - w) / 2;
    if (x < 0) x = 0;
    btn_prompt_draw(ctx, x, y, s, ABE_TEXT_OT);
}

/* =========================================================================
   THE DEATH LIGHTS
   =========================================================================
   Light pouring out of the body while it comes apart, ramping on over the burn
   and riding the fade out so it never outlives the body it is pouring from.
   src/rabisu_boss.c's, and deliberately the same effect: the two deaths are
   meant to read as one phenomenon, so they share the primitive, the colour ramp
   and the clock.

   >>> WHERE THEY HANG IS THE ONE THING THAT COULD NOT BE COPIED. <<< The Rabisu
   hangs four on RBS_A_HEAD/WING/CHEST - mesh-local anchors measured out of a
   .pva by hand and pushed through the same yaw and lean its draw uses. Asag has
   no yaw, no lean and no head-and-wings: he is a 1669-unit animal lying along
   the view axis, and light coming out of him is light coming out ALONG HIS
   LENGTH. asag_span_point() slices the posed body and hands back a point in
   each slice, which needs no hand measurement and follows the pose - the same
   call works whether he is stretched out at Home or flat on the floor.

   FIVE OF THEM, against the Rabisu's four, because he is three times as long. A
   slice holding no vertices is skipped rather than drawn at the origin.

   THE PER-LIGHT CLOCK OFFSET is the runbook's STEP 7B rule: without it the five
   pulse in lockstep and read as one lamp rather than as a body full of them. */
#define ABE_D_LIGHTS  5

static void asag_boss_draw_death_lights(RenderContext *ctx) {
    if (state != ABE_D_BURN && state != ABE_D_FADE) return;

    int32_t bright = (state == ABE_D_BURN)
                   ? (phase_t * 256) / ABE_T_D_BURN
                   : asag_fade();
    if (bright > 256) bright = 256;
    if (bright <= 0) return;

    int i;
    for (i = 0; i < ABE_D_LIGHTS; i++) {
        VECTOR w;
        if (!asag_span_point(i, ABE_D_LIGHTS, &w)) continue;
        rbs_glow_point(ctx, &w, bright, rbs_glow_clock + i * 13);
    }
}

/* Called from asag_arena_draw() with the PLAIN view matrix loaded. A no-op
   outside the two burning phases. */
void asag_boss_draw(RenderContext *ctx) {
    asag_boss_draw_death_lights(ctx);
}

void asag_boss_draw_overlay(RenderContext *ctx) {
    const char **rows = (state == ABE_LINE1) ? LINE_1 :
                        (state == ABE_LINE2) ? LINE_2 : 0;
    if (!rows) return;
    for (int i = 0; i < ABE_TEXT_ROWS; i++)
        abe_line(ctx, rows[i], ABE_TEXT_Y[i]);
}
