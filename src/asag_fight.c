#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "asag.h"
#include "asag_arena.h"
#include "asag_fight.h"
#include "camera.h"         /* player_x/y/z, cam_* for the draws only          */
#include "collision.h"      /* DEBUG_EXPERIMENT — the pools' A/B slot          */
#include "damage.h"       /* the boils' weakness to holy fire               */
#include "particles.h"    /* spawn_rock_burst - the boulders smashing    */
#include "cdaudio.h"        /* cdaudio_stop — the music dies with him          */
#include "player.h"         /* player_hurt, player_health, game_over, flash_timer */
#include "rabisu.h"         /* rbs_glow_quad — see THE ONE BORROWING below     */
#include "render.h"
#include "sound.h"          /* the attack cues, and SFX_HURT/SFX_DIE           */
#include "title.h"          /* current_area, STATE_ASAG_ARENA                  */

/* =========================================================================
   ASAG'S FIGHT
   =========================================================================
   src/asag_fight.h has the brief, the split between the three files, and the
   table of measurements every placement below comes from. This file is the
   machine.

   ---- THE ONE THING TO UNDERSTAND BEFORE READING ANY OF IT ------------------
   >>> ASAG CANNOT TURN. <<< His vertices are baked in the arena's own
   coordinate space and there is no model matrix (src/asag.h says so at length),
   so every clip puts his head in exactly one place in the room every time it
   plays. An attack therefore lands where the ANIMATOR put it, and the only
   parts code can aim are the parts that LEAVE him: the laser's ground point,
   the boulders' cells, the puss balls' headings.

   That single fact shapes all three attacks:

     THE LASER SWEEPS because its head settles at z=1923, in the back half of
     the room. A beam fired straight out of it would hit one fixed patch of
     floor and the fight would be "stand somewhere else".

     THE SLAM IS FIXED AND THE BOULDERS ARE NOT. The head comes down at
     (x -15, z 1487) and that is the whole of the slam's own threat; the four
     boulders after it are what makes the attack follow the player around.

     THE VOMIT IS FIXED AND WIDE. It pours straight down out of his mouth at
     (x -20, z 1657), so the only honest way to make it a threat is the "wide
     radius circle" the brief asks for — the dodge is to be far from his mouth,
     not to be behind him.

   ---- RE-MEASURING, IF THE CLIPS ARE EVER RE-BAKED --------------------------
   Every tick constant below was read off the .pva files, not chosen. The dump
   that produced them walks each clip frame by frame with the position track
   applied, which is the only way to see where the head actually is:

     for each frame f: t = f*60/ASAG_ANIM_FPS, dz = update_pos(clip, t),
                       face = centroid of verts within 90 of the front-most z

   A re-bake at a different step changes every one of them. The clip LENGTHS
   look after themselves — asag_clip_ticks() is asked rather than restated — but
   the moments WITHIN a clip do not, and a stale one presents as an attack whose
   effect fires before or after the animation that is supposed to cause it.
   ========================================================================= */

/* =========================================================================
   THE ONE BORROWING: rbs_glow_quad
   =========================================================================
   >>> THIS FILE DRAWS ITS ADDITIVE QUADS THROUGH src/rabisu.c's PRIMITIVE, AND
   THAT IS A DELIBERATE EXCEPTION TO THE RUNBOOK. <<<
   tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 7 says to put the glow primitives in
   the enemy's own file, because two of the Rabisu's four users are attacks and
   an attack has to work for a second one of these dropped in a room with no
   cutscene. That argument is about ownership of the EFFECT, and it is right.

   rbs_glow_quad is not the effect. It is sixty lines of GTE: project four
   vertices, reject anything past the +/-1023 screen clamp, take the average Z,
   bias it by the room mesh's own 40, emit a POLY_F4 with setSemiTrans and push
   the ABR=1 DR_TPAGE into the same OT bucket AFTER it because the OT is LIFO.
   Every one of those is a property of this engine and none of them is a
   property of the Rabisu — and all of them are easy to get subtly wrong. A
   second copy would not stay a copy.

   What it does NOT bring with it is the Rabisu's colour ramp. rbs_glow_pulse's
   white -> orange -> red is the look of THAT boss's reveal, and the brief here
   asks for a light-blue marker, a green spray and a yellow ball. The colours
   are this file's, passed in per quad; only the projection is shared. The glow
   POINT (af_glow_point, below) is written out in full for exactly that reason —
   rbs_glow_point hard-codes the ramp and so could not be borrowed.

   The +40 bias inside it is correct here: the arena mesh uses the same 40 (see
   src/asag_arena.c, and src/asag.c's note about biasing the body two nearer).
   If the Rabisu is ever deleted, this is the one line that has to move. */

/* =========================================================================
   THE ARENA FLOOR AS A GRID
   =========================================================================
   The laser's burning trail and the slam's boulder markers both light up FLOOR
   POLYGONS, so both want the arena's floor as cells. It is a clean grid and the
   mesh says so: 221 flat quads at y=0, in 15 columns 200 wide and 15 rows
   186.67 deep, spanning the room exactly.

   >>> THE EDGES ARE STORED, NOT A CELL SIZE. <<< STEP 2C of the runbook, and it
   is the same trap the Rabisu's lawn hit: the rows are 186.666... and
   multiplying a rounded 187 out fifteen times lands 5 units off the back wall,
   which is a visible seam on a lit cell. Storing the edges makes the lights sit
   on the polygons rather than near them.

   ROWS 0 AND 14 ARE THE RECESSES. The collision proxy narrows to x[-1300,1300]
   at each end (the landing at the front, Asag's alcove at the back), so those
   two rows are not full width. Nothing below lights a cell in them: the laser's
   sweep is clamped clear of both and the boulders draw from rows 1..13. It
   costs the player nothing — standing in the mouth of the shaft is not a
   hiding place, because the laser's arc reaches z=423 at its centre. */
#define AF_COLS   15
#define AF_ROWS   15
#define AF_CELLS  (AF_COLS * AF_ROWS)

static const int16_t AF_COL_X[AF_COLS + 1] = {
    -1500, -1300, -1100, -900, -700, -500, -300, -100,
      100,   300,   500,  700,  900, 1100, 1300, 1500,
};
static const int16_t AF_ROW_Z[AF_ROWS + 1] = {
       0,  187,  373,  560,  747,  933, 1120, 1307,
    1493, 1680, 1867, 2053, 2240, 2427, 2613, 2800,
};

/* =========================================================================
   THE THREE ZONES, WHICH ARE WHAT THE ATTACKS ACTUALLY HIT
   =========================================================================
   >>> EVERY ATTACK'S DAMAGE IS A THIRD OF THE ARENA, NOT A RADIUS. <<< That is
   the shape the design asks for and it is worth saying plainly, because the
   first version of this file measured everything as circles and they read as
   arbitrary: a blast radius is a number the player has to be hit by twice to
   learn, whereas a third of the room is a place, and the floor lights up to say
   which place.

   The grid is 15 x 15, so a third is exactly five rows or five columns and no
   rounding is involved:

       ROWS  0.. 4   z    0.. 933   THE LANDING END. The laser burns this.
       ROWS  5.. 9   z  933..1867   the middle. The vomit crosses it and, as of
                                    the flanks block below, the slam reaches
                                    into both ends of it.
       ROWS 10..14   z 1867..2800   ASAG'S END. The boulders fall here.
       COLS  5.. 9   x -500.. 500   the centre lane. The vomit covers it, all
                                    the way from the back wall to the landing.

   >>> AND THE THREE COVER DIFFERENT GROUND, WHICH IS THE WHOLE LOOP. <<< The
   laser drives the player FORWARD, off the landing and up the arena. The
   boulders drive them BACK, off Asag's end. The vomit splits the room
   LEFT/RIGHT down the middle. No single spot in the arena survives all three,
   so standing still is never the answer and the two-second idles are when the
   player picks the next place to be.

   >>> AND CHECKING A ZONE AGAINST THE OTHER TWO IS NOT ENOUGH, WHICH IS WHAT
   THE NEXT BLOCK IS ABOUT. <<< That instruction was here from the start and it
   was still obeyed when two squares of the arena ended up hit by nothing at
   all: three zones can differ from each other and STILL leave a gap, because
   what matters is not how they overlap but what none of them covers. Check the
   LEFTOVER. */
#define AF_ROWS_PER_THIRD  (AF_ROWS / 3)
#define AF_COLS_PER_THIRD  (AF_COLS / 3)

#define AF_ROW_LASER_LO    0
#define AF_ROW_LASER_HI    (AF_ROWS_PER_THIRD - 1)          /*  0.. 4 */
#define AF_ROW_SLAM_LO     (AF_ROWS - AF_ROWS_PER_THIRD)    /* 10..14 */
#define AF_ROW_SLAM_HI     (AF_ROWS - 1)
#define AF_COL_VOM_LO      AF_COLS_PER_THIRD                /*  5.. 9 */
#define AF_COL_VOM_HI      (AF_COLS - AF_COLS_PER_THIRD - 1)
#define AF_ROW_MID_LO      AF_ROWS_PER_THIRD                /*  5.. 9 */
#define AF_ROW_MID_HI      (AF_ROWS - AF_ROWS_PER_THIRD - 1)

/* =========================================================================
   ...AND THE TWO FLANKS OF THE MIDDLE ROW, WHICH NOTHING USED TO REACH
   =========================================================================
   >>> THE THREE ZONES ABOVE LEFT TWO SQUARES SAFE FROM EVERYTHING. <<< Read the
   arena as the 3x3 the thirds actually make it, numbered in reading order from
   Asag's end:

       1 2 3     rows 10..14, HIS end        the slam
       4 5 6     rows  5.. 9, the middle
       7 8 9     rows  0.. 4, the landing    the laser

   the vomit taking the centre COLUMN, 2/5/8. Lay the three over each other and
   the laser has 7-8-9, the slam has 1-2-3 and the vomit has 2-5-8 — so
   SQUARES 4 AND 6, the middle row's two flanks, were hit by nothing at all. A
   player who found either one could stand in it for the whole fight and pick
   the boss off between attacks, which is the exact failure the three-zones note
   above says to check for and which it missed because it only ever compared the
   zones with each other, never with what was left over.

   >>> THE FIX IS IN THE SLAM ALONE, AND IT USED TO BE IN TWO. <<< The vomit was
   widened to the whole middle row at the same time, making a PLUS of it, and
   that has since been taken back out: two attacks each covering five of the
   nine squares left too little floor to stand on and blurred what either one
   was saying. The slam carries the flanks by itself now:

       SLAM    his third PLUS 4 and 6 — a C, open toward the landing, with
               two more boulders falling in the flanks it just grew.
       VOMIT   the centre lane, 2-5-8, and nothing else.

   WHAT IS LEFT SAFE, WHICH IS THE THING TO RE-CHECK IF EITHER MOVES AGAIN:

       laser   safe in 1 2 3 4 5 6
       slam    safe in 5 7 8 9
       vomit   safe in 1 3 4 6 7 9

   No square is in all three, so there is still no seat to sit in: 4 and 6 are
   now safe from two attacks out of three and are held by the slam alone, and 5,
   the dead centre, is safe from the slam alone. Both are deliberate — a square
   should be a place you can be for some of the fight, not a place you cannot be
   at all — but 4 and 6 are the thin ones, so if the SLAM'S shape is ever what
   moves, this table is what has to be read first.

   >>> THE SHAPES ARE ASKED AS ONE PREDICATE PER ATTACK, AND THAT IS LOAD
   BEARING. <<< The zones used to be a row range and a column range, so the
   damage test and the floor lighting could each be written as a pair of nested
   loops and could not drift apart. A C is not a range. Each is a function of
   (row, col) instead, called BOTH by the damage test (on the cell the player
   occupies) and by the draw (on every cell), so what is lit and what hurts
   cannot disagree — with the ONE deliberate exception the slam's draw now
   documents, where the lit cells are a subset of the zone that hurts. */

/* 1 if the cell is inside the slam's zone: Asag's third, plus the middle row's
   two flanks (squares 4 and 6). */
static int af_cell_in_slam(int r, int c) {
    if (r >= AF_ROW_SLAM_LO && r <= AF_ROW_SLAM_HI) return 1;
    return r >= AF_ROW_MID_LO && r <= AF_ROW_MID_HI &&
           (c < AF_COL_VOM_LO || c > AF_COL_VOM_HI);
}

/* 1 if the cell is inside the vomit's zone: the centre lane, end to end, and
   nothing else — squares 2, 5 and 8.

   >>> THE CROSSBAR IS GONE AND THE PLUS IS A LANE AGAIN. <<< The zone gained
   the whole middle row when squares 4 and 6 were found to be safe from
   everything (see the flanks block above). It made one attack cover five of the
   nine squares, which is too much of the room for something the player is meant
   to dodge sideways out of, and it blurred the one thing this attack says: pick
   a side. >>> 4 AND 6 DO NOT GO BACK TO BEING SAFE. <<< The SLAM still reaches
   both of them and drops a boulder in each, so that half of the flanks fix is
   what carries it now, and no square survives all three attacks. Re-check it
   against the safe-square table above, not against this function alone, if the
   slam's shape is the next thing to move. */
static int af_cell_in_vomit(int r, int c) {
    (void)r;
    return c >= AF_COL_VOM_LO && c <= AF_COL_VOM_HI;
}


/* The floor. Every plane in this room is at y=0 (the generated collision table
   says so, and src/asag_arena.c mirrors it as three flat zones), so a floor
   lookup per corner — which the Rabisu's terraced lawn needs — would be four
   scans of a three-entry list to be told 0. Re-visit if the arena ever grows a
   step; tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 7C is the warning. */
#define AF_FLOOR_Y  0

static int af_col_of(int32_t x) {
    int c;
    for (c = 0; c < AF_COLS; c++)
        if (x >= AF_COL_X[c] && x < AF_COL_X[c + 1]) return c;
    return -1;
}

static int af_row_of(int32_t z) {
    int r;
    for (r = 0; r < AF_ROWS; r++)
        if (z >= AF_ROW_Z[r] && z < AF_ROW_Z[r + 1]) return r;
    return -1;
}

/* The cell containing (x,z), or -1 outside the grid. */
static int af_cell_of(int32_t x, int32_t z) {
    int c = af_col_of(x), r = af_row_of(z);
    if (c < 0 || r < 0) return -1;
    return r * AF_COLS + c;
}

/* Is the player standing in a zone? Asked of the CELL they occupy rather than
   of their raw coordinates, so the hit zone is exactly the set of polygons that
   lit up and there is no half-cell disagreement between what the floor says and
   what the damage does. Both go through the cell predicates above, so the
   answer is by construction the one the draw is painting. Outside the grid is
   outside every zone, which is right: the only way out of it is out of the
   room.

   >>> THERE WAS AN af_in_rows()/af_in_cols() PAIR HERE AND BOTH ARE GONE. <<<
   They were right while every zone was a band of rows or a band of columns.
   Two of the three are shapes now and the third — the laser — never used
   them: it damages off the cells its trail actually set alight, which is the
   same principle arrived at first. */
static int af_in_slam_zone(void) {
    int r = af_row_of(player_z()), c = af_col_of(player_x());
    return r >= 0 && c >= 0 && af_cell_in_slam(r, c);
}

static int af_in_vomit_zone(void) {
    int r = af_row_of(player_z()), c = af_col_of(player_x());
    return r >= 0 && c >= 0 && af_cell_in_vomit(r, c);
}

/* =========================================================================
   TUNING — every duration in GAME FRAMES with the brief's seconds beside it
   ========================================================================= */

/* ---- The loop ------------------------------------------------------------ */
/* >>> TWO SECONDS, HALVED FROM THE BRIEFED FOUR. <<< Four seconds was what the
   original brief asked for and it played too slow: the idles are meant to be
   when the player picks where to be next (see the three-zones block above), and
   that decision takes about a second. The other two were dead air.

   IT IS THE ONE NUMBER THAT SETS THE FIGHT'S PACE, so note what halving it also
   halves: the time to reposition between the laser's fire, the boulders' zone
   and the vomit's lane, and the window in which the player shoots at the boils
   without an attack in the air. If the fight ever reads as too frantic, this is
   the first thing to put back. */
#define AF_T_IDLE            120   /* 2 s between attacks                      */

/* >>> HOW LONG BEFORE A CLIP ENDS THE HEAD STOPS BEING EXPOSED. <<< The brief
   gives 0.5 s for the slam and 1.0 s for the faint. It says only "as soon as
   this attack starts Asag becomes exposed" for the vomit, so the slam's rule is
   reused there — see asag_fight.h, this is the assumption. Both are subtracted
   from asag_clip_ticks(), never from a written-down length. */
#define AF_EXPOSE_LEAD        30   /* 0.5 s — slam and vomit                   */
#define AF_EXPOSE_LEAD_FAINT  60   /* 1.0 s — the faint                        */

/* ---- The laser ------------------------------------------------------------
   The head settles at t 75 and holds to t 280 (43 frames = 322 ticks all told),
   so the charge fills the first fifteen frames of that and the sweep takes the
   rest with a little to spare either side. */
#define AF_T_LAS_CHARGE       75   /* head in place: the glow starts           */
#define AF_T_LAS_FIRE         90   /* the beam leaves him                      */
#define AF_T_LAS_END         270   /* 3 s of sweep                             */

/* >>> THE SWEEP RAKES STRAIGHT ACROSS THE LANDING END, AND WHAT IT LEAVES
   BEHIND IS THE WHOLE THIRD. <<< The beam's ground point travels in a straight
   line in X at a fixed Z, and as it crosses each of the fifteen columns it sets
   the ENTIRE column of the landing third alight - five rows at once, not just
   the cell it is standing on. So the sweep is a WIPE: by the time it reaches
   the far wall the whole third is burning, and it burns for three seconds.

   THAT IS A REWRITE, AND THE FIRST VERSION WAS WRONG ABOUT WHAT THE ATTACK IS.
   It swept an ARC at a fixed range from his head and lit only the single cell
   under the point, which made the hazard a moving line the player could step
   over and left a thin scribble behind it. The attack is A THIRD OF THE ROOM
   CATCHING FIRE; the sweep is how it gets there, not what it hits.

   THE Z IS THE MIDDLE OF THAT THIRD (row 2 of 0..4), which is where a beam
   raking the far floor should touch down. It is not a hit box - the five rows
   light regardless of where the beam's own footprint falls - so it is free to
   be chosen for the look. */
#define AF_LAS_SWEEP_Z       466   /* centre of row 2: (373 + 560) / 2        */
/* >>> 126 AND NOT THE BRIEFED 180. <<< Three seconds of burning floor was what
   was asked for and it out-stayed the attack: the laser's own clip is over long
   before the fire is, so the trail was still fading through the idle that
   follows and into the next attack's telegraph, and two hazards lit at once is
   one hazard too many to read. 126 is 180 less 30% — 2.1 s — which still
   outlasts the sweep that lights it but is gone by the time the loop moves on.
   AF_T_TRAIL_COOLDOWN (60) is unchanged and still well inside this, so a player
   who stands in the fire can be burned twice by one trail. */
#define AF_T_LAS_TRAIL       126   /* 2.1 s a cell stays lit                   */

/* ---- The slam ------------------------------------------------------------
   The head is on the floor from t 112 to t 337 out of 375. */
#define AF_T_SLAM_IMPACT     112   /* the head lands: damage, and the markers  */
#define AF_SLAM_RADIUS       420   /* "caught under the slamming head"         */

/* >>> THE CUE LEADS THE IMPACT, IT DOES NOT SIT ON IT. <<< slam_asag was fired
   on AF_T_SLAM_IMPACT itself, which is correct about the frame and late in the
   ear: the sample opens on a short rush of the head coming down before its hit
   arrives, so aligning its FIRST frame with the landing puts its loudest part a
   fifth of a second past the thing it is supposed to be the sound of. Backing
   it off by that much lands the hit on the hit and puts the rush under the
   descent, where the animation already is.

   It is a SEPARATE constant and not a smaller AF_T_SLAM_IMPACT because the
   impact frame is measured off the .pva and owns the damage and the boulders;
   moving it to fix a sound would move those with it. */
#define AF_T_SLAM_SFX_LEAD    14   /* 0.23 s before the head lands             */

/* ---- THE BOULDERS --------------------------------------------------------
   >>> TWO, FIXED, ONE EITHER SIDE OF HIM - NOT FOUR AT RANDOM. <<< The first
   version dropped four on cells drawn from a block around the player, which
   made the attack a shotgun that followed them anywhere. It is a PLACE now:
   two boulders fall either side of Asag, and standing in his third of the arena
   when they land is what hurts.

   THE COLUMNS ARE 4 AND 10, whose centres are x=-600 and x=+600. Symmetric
   about the room's centre line, which is also symmetric about ASAG - his head
   sits at x=-15 through the whole slam, so "one to each side of him" and "one
   to each side of the room" are the same two cells to within fifteen units.
   Row 12 is the middle of his third.

   THEY ARE STAGGERED BY A FEW FRAMES and that is not decoration: spawn_burst
   and its family overwrite the WHOLE particle pool, so two smashes on the same
   frame show as one. A short offset gives two impacts, two bursts, and a better
   sound to the moment than a single thud would have. */
#define AF_BOULDERS            4
#define AF_BLD_COL_L           4    /* x[-700,-500], centre -600               */
#define AF_BLD_COL_R          10    /* x[ 500, 700], centre +600               */
#define AF_BLD_ROW            12    /* the middle of Asag's third              */

/* >>> AND TWO MORE, OUT IN THE FLANKS THE ZONE JUST GREW. <<< The slam now
   reaches squares 4 and 6 (see the flanks block up in the grid section), and a
   zone that hurts with nothing falling into it is the half of this attack the
   player cannot read. These are the boulders for those two squares.

   THE COLUMNS ARE 2 AND 12, centres x=-1000 and x=+1000: the middles of the
   outer thirds, the same way 4 and 10 are placed inside the room's middle. ROW
   7, z[1307,1493], is the middle of the middle third — and it is worth
   noting that this is where his HEAD lands too (x=-15, z=1487, i.e. row 7
   column 7). The three are on the same line across the room with the head
   between them, which reads as one movement rather than as rocks arriving from
   somewhere unrelated.

   FOUR ROCKS AND FOUR STAGGER SLOTS. The stagger exists because spawn_rock_burst
   overwrites the WHOLE particle pool, so impacts on the same frame show as one;
   at 9 frames apart the last of four starts 27 frames after the first and lands
   at t 184 of a 375-tick clip, which is comfortable. Adding a fifth would want
   that arithmetic re-checked, not just the array grown. */
#define AF_BLD_COL_FL          2    /* x[-1100,-900], centre -1000: square 4   */
#define AF_BLD_COL_FR         12    /* x[ 900,1100], centre +1000: square 6    */
#define AF_BLD_ROW_FLANK       7    /* the middle of the middle third          */
#define AF_T_BLD_STAGGER       9    /* 0.15 s between one impact and the next  */
#define AF_T_BLD_FALL         45   /* 0.75 s from the sky to the floor         */
#define AF_T_BLD_LINGER       25   /* it sits there, then it is gone           */
#define AF_BLD_DROP         1800   /* how far up "the sky" is                  */
#define AF_BLD_HALF          150   /* "a large brown cube"                     */

/* ---- The vomit ------------------------------------------------------------
   The head settles at t 112 and TWITCHES from t 150 to t 265, which is the part
   the brief hangs the spray on: "once it moves its head into position it starts
   doing a twitching motion, as it twitches a stream of green particles...". */
#define AF_T_VOM_START       150
#define AF_T_VOM_END         265

/* >>> IT IS A LANE, NOT A CIRCLE, AND IT RUNS THE WHOLE LENGTH OF THE ROOM.
   <<< The middle third in X - columns 5..9, x[-500,500] - from the back wall to
   the landing. The first version was a 1100-unit circle under his mouth, which
   was the right instinct (it has to be wide, because he cannot aim it) and the
   wrong shape: a circle centred at z=1657 cannot reach the front of the arena
   at all, so the whole attack was dodged by never walking forward.

   A LANE FIXES THAT AND SPLITS THE ROOM, which is the job this attack does in
   the loop. There is no "far enough away" from it - the dodge is sideways, to
   either flank, and it is the only attack whose answer is left or right rather
   than forward or back.

   THE PARTICLES FAN ALONG IT rather than raining out of nowhere: they leave his
   mouth with a large spread in Z and a small one in X, so the spray visibly
   travels down the lane it is about to poison. See af_vom_spit().

   >>> THIS IS THE NUMBER ON SCREEN, NOT A CEILING THE SPRAY RARELY REACHES.
   <<< af_vom_spit() is called on EVERY frame of the window and asks for four,
   while a drop lives about twenty-five before it lands, so the pool is asked
   for roughly a hundred and saturates within the first frames. It then stays
   full until the spray stops. That makes this constant the count the player
   actually sees — change it and the density changes with it, which is not
   true of the `made < 4` refill cap next to it.

   23 AND NOT THE ORIGINAL 34: a third fewer, because the stream read as a
   solid green wall rather than as a spray and the lit lane underneath — the
   half of this attack that says where the damage is — was being covered by
   it. Each drop is an af_glow_point, so this is eleven additive primitives a
   frame off the spray as well. */
#define AF_VOM_PARTICLES      23

/* ---- The boils ------------------------------------------------------------ */
#define AF_BOIL_HEALTH         3   /* as briefed                               */

/* >>> AND THE ONE EXCEPTION TO "EVERY WEAPON DEALS 1x DAMAGE TO ASAG". <<<
   That brief is why neither the head nor the boils had a weakness table at all
   — see the long note at graveolver_fire's best_kind == 9. It still holds for
   the HEAD, which takes a flat 1 from everything. The BOILS are now 3x weak to
   holy fire and to nothing else.

   WHAT THAT BUYS: the Helluminator ticks once a SECOND for 1 (HELL_TICK_FRAMES
   / HELL_TICK_DAMAGE), so a 3 HP boil used to cost three full seconds of
   holding the trigger. Three seconds is a long time to stand still at the back
   of this arena — HELL_RANGE is 1800 against the boils at z=2734, so the player
   has to walk most of the way up it to reach one at all, into the ground the
   slam lands on — and at that price nobody chose the lantern over the gun,
   which kills a boil in three shots from anywhere. At 300 the single tick takes
   all 3 and ONE SECOND OF BURN BURSTS A BOIL, which is what makes the walk
   worth making.

   THE GUN IS UNCHANGED BY THIS. It fires DMG_KINETIC and DMG_FLAME, neither of
   which is in the table, so a boil still takes three rounds of either. Both
   call sites go through asag_boil_scale_damage() all the same, the way every
   other enemy's do, so a second entry added here reaches the gun without
   anyone having to remember it exists.

   300 IS THE ZOMBIES' NUMBER and deliberately so: zombie.c argues it as the
   biggest modifier in the game because a walking corpse is what the lantern was
   built for. A boil is the other thing it was built for — holy fire against an
   organ of the boss — and reusing the figure keeps "the lantern kills what it
   is meant to kill in one tick" one rule rather than two. */
static const Weakness asag_boil_weakness[] = {
    { DMG_HOLY, 300 },
};

int32_t asag_boil_scale_damage(int32_t base, DamageType type) {
    return damage_scale(base, type, asag_boil_weakness,
                        WEAKNESS_COUNT(asag_boil_weakness));
}
#define AF_T_BOIL_RESTORE   1800   /* 30 s, as briefed                         */
#define AF_T_BOIL_RELIGHT     45   /* 0.75 s of coming back up, not a snap     */

/* How long a boil's health bar stays up after a hit. ASAG'S OWN hit_timer, to
   the frame — the two bars are the same piece of UI hung on two different
   things, and a boil bar that outlived the boss's would read as a second
   system. src/rabisu.c's RBS_BAR_TIMER is the original. */
#define AF_T_BOIL_BAR        120   /* 2 s, Asag's hit_timer exactly            */

/* ---- The puss balls ------------------------------------------------------- */
#define AF_PUSS_PER_BOIL       3   /* as briefed                               */
#define AF_PUSS_MAX           (AF_PUSS_PER_BOIL * ASAG_BOIL_COUNT)
#define AF_PUSS_SPREAD       341   /* 30 degrees in 4096ths, as briefed        */

/* >>> THE SPEED AND THE ARC ARE ONE NUMBER BETWEEN THEM, AND WHAT THEY HAVE TO
   BUY IS THE LENGTH OF THE ROOM. <<< These were 34 and a rise of 10, which put
   a ball down after about 40 frames and therefore about 1360 units out — under
   half the arena's 2800. From boils at z=2734 that is the back half of the room
   and nothing else, so a player standing on the landing could watch both boils
   burst and never be threatened by what came out of them, which is the whole
   point of the puss balls.

   The flight time is set by the fall, not by the speed: a ball leaves the LEFT
   boil at y=-400 with AF_PUSS_RISE_V of upward speed and AF_PUSS_GRAV a frame
   pulling it back, so it is in the air for roughly

       t = rise_v + sqrt(rise_v^2 + 2 * grav * 400)     ~= 46 frames at 14/1

   and the reach is that times the speed. 60 x 46 = 2760, i.e. the arena's
   length from the wall they are fired out of, which is what was asked for. The
   RIGHT boil is 200 higher and so throws a little further still, which is free.

   THE RISE WENT UP WITH IT ON PURPOSE. Holding it at 10 and raising only the
   speed would have bought the range with a flatter, faster line — a dart rather
   than the lobbed arc the whole "shoot up and then arc towards the floor" note
   below is about. Raising both keeps the shape and stretches it.

   AF_PUSS_LIFE is untouched: 180 frames is still four times the longest flight,
   so it stays what it was meant to be — a backstop, not a range limit. */
#define AF_PUSS_SPEED         60   /* units/frame in XZ: 46 frames x 60 = 2760 */
#define AF_PUSS_HALF          44   /* "yellow cubes"                           */
#define AF_PUSS_LIFE         180   /* backstop: 3 s and it is gone             */

/* >>> "NEVER HIGH ENOUGH TO GO OVER THE PLAYER'S HEAD" IS MEASURED FROM THE
   LAUNCH, AND IT HAS TO BE. <<< The brief asks for a low arc that the player can
   see coming and that never sails overhead. Taken as an ABSOLUTE ceiling it is
   unsatisfiable: the boils are part of the back wall at y=-400 and y=-600, i.e.
   400 and 600 above the floor, and the player's eye is at y=-189. A ball
   leaving a boil is already well over their head and the only arc that obeys
   the letter of it is a pure descent with no rise at all — which is not the
   "shoot up in an arc and then arc towards the floor" the same sentence asks
   for.

   So the rise is relative: it leaves the boil climbing, tops out this far above
   where it started, and falls to the floor. The SPIRIT of the constraint — a
   flat, readable, basketball-but-lower arc — is what survives, and it is the
   half that affects play. */
#define AF_PUSS_RISE_V        14   /* initial upward speed, units/frame        */
#define AF_PUSS_GRAV           1   /* added to the downward speed each frame   */

/* ---- What everything does to the player -----------------------------------
   Percentages of MAX_HEALTH, so these are flat amounts — which is how every
   other attack in this game works (player_hurt takes a number). */
#define AF_DMG_LASER    ((MAX_HEALTH * 30) / 100)   /* 30 */
#define AF_DMG_SLAM     ((MAX_HEALTH * 20) / 100)   /* 20 */
#define AF_DMG_BOULDER  ((MAX_HEALTH * 20) / 100)   /* 20 */
#define AF_DMG_VOMIT    ((MAX_HEALTH * 20) / 100)   /* 20 */
#define AF_DMG_PUSS     ((MAX_HEALTH * 10) / 100)   /* 10 */

/* =========================================================================
   HURTING THE PLAYER — ONE ENTRY POINT, AND IT HAD TO BECOME ONE
   =========================================================================
   >>> THIS FIGHT USED TO CALL player_hurt() RAW, AND THAT IS WHY THE PLAYER
   COULD NOT DIE IN IT. <<< player_hurt() subtracts from player_health and does
   nothing else: it does not raise `game_over`, it does not set flash_timer and
   it does not make a noise. Every other enemy in the game — spider.c,
   rabisu.c, tentacle.c, mushroom.c, hadad.c, rafflesia.c, demondog.c — follows
   it with the SAME four lines at every call site, and this file had none of
   them. The result was a boss arena in which the health bar emptied, went
   negative and the player walked around on a negative total forever, with
   main.c's game-over branch (which already lists STATE_ASAG_ARENA) waiting on a
   flag nobody ever set.

   So the five call sites go through here instead of each remembering the
   ritual. FIVE was exactly the problem: the laser's burning floor, the slam's
   head, the boulders, the vomit's lane and the puss balls, and a fix applied
   four times is a fix.

   >>> THE HURT CUE IS COOLED DOWN, THE DEATH CUE IS NOT. <<< src/spider.c's
   arrangement, and this fight needs it more than the spider does: the trail and
   the lane are CONTINUOUS hazards with their own latches, but a player crossing
   the burning third as the boulders land can take two hits inside a few frames,
   and two HURTs on one voice is a click rather than a cry. SFX_DIE is left
   uncooled because it can only ever fire once.

   SFX_HURT and SFX_DIE are both RESIDENT (src/sound.h), so they are audible
   down here whatever bank is loaded — which is the whole reason the arena could
   be given an empty bank of its own. */
#define AF_T_HURT_SFX   30   /* 0.5 s, src/spider.c's SPD hurt cooldown */

static int32_t hurt_sfx_cooldown;

static void af_hurt(int32_t amount) {
    /* ALREADY DEAD IS NOT HURT AGAIN. Every other call site in the game opens
       with `if (!game_over)`; one test here covers all five. */
    if (game_over) return;

    player_hurt(amount);

    if (hurt_sfx_cooldown == 0) {
        sound_play(SFX_HURT);
        hurt_sfx_cooldown = AF_T_HURT_SFX;
    }

    if (player_health <= 0) {
        player_health = 0;
        game_over     = 1;
        flash_timer   = 90;     /* the white flash, everyone else's 90 */
        sound_play(SFX_DIE);
    }
}

/* >>> THE BURNING FLOOR NEEDS A COOLDOWN AND THE ONE-SHOT ATTACKS NEED A
   LATCH. <<< A cell that burns for three seconds would otherwise deal its 30 on
   every one of 180 frames. A cooldown rather than a per-cell "already used"
   flag because the trail is a CONTINUOUS strip: marking cells spent would let a
   player walk along the trail collecting a fresh 30 per cell, which is worse.
   One second is long enough to be an unmistakable "get off the fire". */
#define AF_T_TRAIL_COOLDOWN   60

/* The player's collision radius, the same one apply_collision_reception() hands
   asag_collide(). A blast radius is measured to the player's SURFACE, not to
   the point the camera happens to be. */
#define AF_PLAYER_RADIUS      75

/* =========================================================================
   STATE
   ========================================================================= */

/* The loop. IDLE_A/B/C are three distinct phases rather than one with a
   counter, because the faint has to resume "whichever phase he was in" and a
   counter would have to be saved alongside it anyway. */
typedef enum {
    AF_OFF = 0,     /* before the handover, and from the moment health hits 0 */
    AF_LASER,
    AF_IDLE_A,
    AF_SLAM,
    AF_IDLE_B,
    AF_VOMIT,
    AF_IDLE_C,
    AF_FAINT,       /* the interrupt: both boils burst                        */
} AfPhase;

static AfPhase phase;
static int32_t phase_t;

/* >>> WHERE THE LOOP GOES BACK TO AFTER A FAINT. <<< The faint now CUTS INTO
   whatever was running (see af_begin_faint), so the phase it interrupted never
   finishes — and going back to it would replay an attack the player has already
   dodged half of. This is therefore the phase AFTER the interrupted one: "once
   the faint resolves he should return to his loop and move onto the next move
   in the loop". It is solved with af_next() at the moment of the interrupt
   rather than at the end of the faint, because by then `phase` is AF_FAINT and
   the loop position would have been lost. */
static AfPhase resume_phase;

static int32_t health;
static int32_t hit_timer;      /* health-bar flash countdown, the Rabisu's     */
static int     dying_flag;     /* health 0: AI stopped, body still drawn       */
static int     dead_flag;      /* the director, at the end of the fade         */

/* Each attack fires its damage ONCE. Without these a slam would deal 20 on
   every frame the player stood under the head. */
static int     slam_hit_done;
static int     vom_hit_done;
static int32_t trail_cooldown;

/* ---- The boils ---------------------------------------------------------- */
typedef struct {
    int8_t  hp;
    int8_t  burst;
    int32_t restore_t;     /* counts up to AF_T_BOIL_RESTORE while burst       */
    int32_t hit_timer;     /* bar flash countdown, Asag's own and the Rabisu's */
} AfBoil;
static AfBoil boil[ASAG_BOIL_COUNT];

/* ---- The laser ---------------------------------------------------------- */
static int32_t las_gx, las_gz;      /* this frame's ground point               */
static int32_t las_ox, las_oy, las_oz;  /* ...and where the beam leaves him    */
static int     las_firing;
static int     las_dir;             /* 0 = sweeps left to right, 1 = the other */
static uint8_t trail[AF_CELLS];     /* frames of burn left in each cell        */

/* ---- The slam's boulders ------------------------------------------------- */
typedef struct {
    int16_t cell;
    int16_t t;             /* counts up: fall, then linger, then gone          */
    int8_t  live;
    int8_t  hit_done;
} AfBoulder;
static AfBoulder boulder[AF_BOULDERS];
static int       boulders_armed;
static int       bld_hit_done;   /* the ZONE's hit, once a slam, not per cube */
static int       bld_sfx_done;   /* ...and ONE rumble for the pair; see below */

/* ---- The vomit's spray --------------------------------------------------- */
typedef struct {
    int16_t x, y, z;
    int16_t vx, vy, vz;
    int16_t life;
} AfDrop;
static AfDrop vom[AF_VOM_PARTICLES];
static int32_t vom_mx, vom_my, vom_mz;   /* his live mouth, for the spray       */
/* How brightly the lane is lit, 0..256. Ramped up as the twitch begins and down
   as it ends, rather than switched: the zone appearing between two frames reads
   as a draw error, and the ramp up is also the attack's tell. */
static int32_t vom_zone_lit;

/* ---- The puss balls ------------------------------------------------------ */
typedef struct {
    int32_t x, y, z;
    int32_t vx, vy, vz;
    int16_t life;
    int8_t  live;
} AfPuss;
static AfPuss puss[AF_PUSS_MAX];

/* =========================================================================
   SMALL MATHS
   ========================================================================= */

/* The same Newton-by-halving isqrt src/rabisu.c, src/web.c and src/asag_boss.c
   all carry. Copied for the fourth time rather than shared for the reason the
   third copy gives: nine self-contained lines that depend on nothing beat a
   header nobody can find. */
static int32_t af_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x = v, last;
    if (x > 1 << 16) x = 1 << 16;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

/* Distance in the XZ plane. Real, not Manhattan: these are BLAST RADII and a
   Manhattan circle is a diamond, which would make the vomit's 1100 reach 1100
   straight ahead and 778 on the diagonal. That is a 30% difference in a number
   the player learns by being hit by it. */
static int32_t af_dist_xz(int32_t ax, int32_t az, int32_t bx, int32_t bz) {
    int32_t dx = ax - bx, dz = az - bz;
    return af_isqrt(dx * dx + dz * dz);
}

/* =========================================================================
   ADDITIVE DRAWING
   =========================================================================
   The quads go through rbs_glow_quad (see THE ONE BORROWING at the head of this
   file). What is here is the two shapes that one does not cover. */

/* A pool of light lying on the floor, an additive quad four units clear of the
   surface so it cannot z-fight the poly it is lighting — the same four
   rbs_glow_pillar uses, and the same reason.

   NO SHAFT ABOVE IT, unlike the Rabisu's lawn lights. That effect is light
   POURING OUT of the ground, which is what the reveal of something rising
   through a lawn wants. Neither of this file's two users is that: the laser's
   trail is ground that has been BURNED and the slam's markers are a target
   painted from above. A flaring shaft on either would say the floor was the
   source, which is exactly backwards for both. */
/* ---- THE ZONE POOLS' REJECT PATH ------------------------------------------
   >>> A LIT ZONE IS 125 OF THESE, AND ON AVERAGE 78% OF THEM ARE NOT ON
   SCREEN. <<< PART 9A of tools/ADDING_THE_ASAG_FIGHT.txt sized this effect at
   75 quads; the sixth pass grew both heavy zones to 125 when it closed the
   arena's two safe squares, and that note was never revisited. Without a cheap
   reject, every one of them pays a full gte_rtpt + gte_rtps + gte_avsz4 inside
   rbs_glow_quad and is then thrown away by its +/-1023 screen test — the exact
   complaint STEP 3B of tools/DIAGNOSING_FRAME_RATE.txt makes about a reject
   path that reads the thing it is about to reject.

   THIS IS src/asag_arena.c's SIDE-PLANE TEST, unchanged, run on a cell's four
   corners instead of a primitive's vertices. With gte_SetGeomScreen(256) on a
   320-wide screen the half-field is 160/256, so a point is outside the right
   plane when 8*side > 5*fwd. >>> A CELL IS ONLY DROPPED WHEN ALL FOUR CORNERS
   ARE OUTSIDE THE SAME PLANE <<< — that is what makes it behaviour-neutral,
   since such a cell covers no pixel of the screen and nothing that was drawn
   stops being drawn.

   Y IS NOT TESTED and does not need to be: these quads lie on the y=0 floor,
   which is the case the room's own test already argues is safe. Pitch does not
   matter either — it rotates about the camera's X axis and cannot carry a point
   across the left or right plane.

   COUNTED, NOT GUESSED, which is the runbook's own rule. Over the walkable
   arena at 200-unit spacing and 16 headings, of 125 cells a mean of 27.8 (slam)
   and 28.3 (vomit) survive: 78% and 77% culled.

   >>> THE WORST POSE IS 107 OF 125 AND THIS DOES ALMOST NOTHING FOR IT. <<<
   Standing back and looking straight down a lit zone is still 107 quads of
   additive floor. If a meter says the lag lives there rather than in the mean,
   the remaining lever is FILL and it is a look decision — PART 9A's paragraph
   on why per-cell beats per-row still stands, and merging rows would make a
   whole zone vanish from a camera standing in it.

   Level 5 (DBG_EXP_NO_FRUSTUM) switches it off, so it can be A/B'd in place the
   way STEP 0 of the frame-rate runbook intends. */
static int32_t af_cam_sn, af_cam_cs;   /* hoisted per frame; STEP 3C fix 1     */
static int     af_no_cull;

static int af_cell_on_screen(int c, int w) {
    if (af_no_cull) return 1;

    const int32_t cx[4] = { AF_COL_X[c], AF_COL_X[c + 1], AF_COL_X[c], AF_COL_X[c + 1] };
    const int32_t cz[4] = { AF_ROW_Z[w], AF_ROW_Z[w], AF_ROW_Z[w + 1], AF_ROW_Z[w + 1] };
    int32_t f[4], sd[4];
    int k, behind = 1, right = 1, left = 1;

    for (k = 0; k < 4; k++) {
        int32_t dx = cx[k] - cam_x, dz = cz[k] - cam_z;
        f[k]  = (dx * af_cam_sn + dz * af_cam_cs) >> 12;
        sd[k] = (dx * af_cam_cs - dz * af_cam_sn) >> 12;
        if (f[k] >= -1200) behind = 0;
    }
    if (behind) return 0;

    for (k = 0; k < 4; k++) {
        if (!( sd[k] * 8 > f[k] * 5)) right = 0;
        if (!(-sd[k] * 8 > f[k] * 5)) left  = 0;
    }
    return !(right || left);
}

static void af_pool(RenderContext *ctx, int cell, uint8_t r, uint8_t g, uint8_t b) {
    int c = cell % AF_COLS, w = cell / AF_COLS;
    if (!af_cell_on_screen(c, w)) return;
    SVECTOR v[4];
    int k;
    for (k = 0; k < 4; k++) { v[k].pad = 0; v[k].vy = (int16_t)(AF_FLOOR_Y - 4); }
    v[0].vx = AF_COL_X[c];     v[0].vz = AF_ROW_Z[w];
    v[1].vx = AF_COL_X[c + 1]; v[1].vz = AF_ROW_Z[w];
    v[2].vx = AF_COL_X[c];     v[2].vz = AF_ROW_Z[w + 1];
    v[3].vx = AF_COL_X[c + 1]; v[3].vz = AF_ROW_Z[w + 1];
    rbs_glow_quad(ctx, v, r, g, b);
}

/* A glow hung at a world point, as concentric additive screen-space squares
   around its projection. rbs_glow_point's shape exactly — and written out again
   rather than called, because that one takes a `clock` and runs it through the
   Rabisu's white -> orange -> red ramp. Everything this file glows is a colour
   that ramp cannot make. The projection trick is the health bar's: a point has
   no billboard quad to hang a sprite off, so it is projected explicitly and the
   size comes from the world distance, matching gte_SetGeomScreen(256). */
static void af_glow_point(RenderContext *ctx, int32_t px, int32_t py, int32_t pz,
                          int32_t world_half, int rings,
                          uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    SVECTOR pt;
    pt.vx = (int16_t)px; pt.vy = (int16_t)py; pt.vz = (int16_t)pz; pt.pad = 0;

    DVECTOR sv;
    int32_t sz;
    gte_ldv0(&pt);
    gte_rtps();
    gte_stsxy(&sv);
    gte_stsz(&sz);
    if (sz == 0) return;
    if (sv.vx <= -1023 || sv.vx >= 1023 || sv.vy <= -1023 || sv.vy >= 1023) return;

    int32_t dx = px - cam_x, dy = py - cam_y, dz = pz - cam_z;
    int32_t dist = af_isqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 64) dist = 64;

    int32_t otz = sz >> 2;
    if (otz <= SCENE_OT_MIN)   otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1)  otz = OT_LENGTH - 2;

    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;

    int ring;
    for (ring = 0; ring < rings; ring++) {
        if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) return;

        /* Outermost is full width and dimmest; each step in halves the square
           and raises the level, so the rings sum to a soft falloff with a hot
           core for no texture and nothing to sort. */
        int32_t wh   = world_half >> ring;
        int32_t half = (wh * 256) / dist;
        if (half < 1)   half = 1;
        if (half > 400) half = 400;
        int32_t lev = 70 + ring * 62;
        if (lev > 256) lev = 256;

        POLY_F4 *p = (POLY_F4 *)ctx->next_packet;
        setPolyF4(p);
        setSemiTrans(p, 1);
        setRGB0(p, (uint8_t)((r * lev) >> 8),
                   (uint8_t)((g * lev) >> 8),
                   (uint8_t)((b * lev) >> 8));
        p->x0 = (int16_t)(sv.vx - half); p->y0 = (int16_t)(sv.vy - half);
        p->x1 = (int16_t)(sv.vx + half); p->y1 = (int16_t)(sv.vy - half);
        p->x2 = (int16_t)(sv.vx - half); p->y2 = (int16_t)(sv.vy + half);
        p->x3 = (int16_t)(sv.vx + half); p->y3 = (int16_t)(sv.vy + half);
        addPrim(&ot[otz], p);
        ctx->next_packet += sizeof(POLY_F4);

        DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
        addPrim(&ot[otz], tp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
}

/* A solid cube: the puss balls and the boulders. src/rabisu.c's fireball, and
   for the same reasons it gives — all six faces at one flat colour in one OT
   bucket means the result is the silhouette filled, with no dependence on
   winding for gte_nclip and nothing to sort inside it. Fogged with the room's
   own near/far so a boulder falling at the back of the arena belongs to the
   back of the arena. */
static void af_cube(RenderContext *ctx, int32_t x, int32_t y, int32_t z,
                    int32_t half, uint8_t cr, uint8_t cg, uint8_t cb) {
    static const int8_t corner[8][3] = {
        { -1, -1, -1 }, {  1, -1, -1 }, {  1, -1,  1 }, { -1, -1,  1 },
        { -1,  1, -1 }, {  1,  1, -1 }, {  1,  1,  1 }, { -1,  1,  1 },
    };
    static const uint8_t face[6][4] = {
        { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
        { 3, 2, 6, 7 }, { 0, 3, 7, 4 }, { 1, 2, 6, 5 },
    };

    int32_t ddx = x - cam_x, ddz = z - cam_z;
    int32_t dist = (ddx < 0 ? -ddx : ddx) + (ddz < 0 ? -ddz : ddz);
    if (dist >= g_fog_far) return;
    int32_t fs = render_fog_scale(dist);
    if (fs > 255) fs = 255;
    cr = (uint8_t)((cr * fs) >> 8);
    cg = (uint8_t)((cg * fs) >> 8);
    cb = (uint8_t)((cb * fs) >> 8);

    SVECTOR v[8];
    int c;
    for (c = 0; c < 8; c++) {
        v[c].vx  = (int16_t)(x + corner[c][0] * half);
        v[c].vy  = (int16_t)(y + corner[c][1] * half);
        v[c].vz  = (int16_t)(z + corner[c][2] * half);
        v[c].pad = 0;
    }

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int fi;
    for (fi = 0; fi < 6; fi++) {
        if (ctx->next_packet + sizeof(POLY_F4) > buf_end) return;

        DVECTOR sv[4];
        int32_t sz[4], otz;

        gte_ldv3(&v[face[fi][0]], &v[face[fi][1]], &v[face[fi][2]]);
        gte_rtpt();
        gte_stsxy3c(sv);
        gte_ldv0(&v[face[fi][3]]);
        gte_rtps();
        gte_stsxy(&sv[3]);
        gte_stsz4c(sz);
        if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) continue;

        int k, off = 0;
        for (k = 0; k < 4; k++)
            if (sv[k].vx <= -1023 || sv[k].vx >= 1023 ||
                sv[k].vy <= -1023 || sv[k].vy >= 1023) { off = 1; break; }
        if (off) continue;

        gte_avsz4();
        gte_stotz(&otz);
        if (otz <= 0) continue;
        /* >>> CLAMPED, NOT DROPPED, AND NO +40. <<< Two differences from the
           additive quads above and both matter for a SOLID object.

           A quad that comes out nearer than SCENE_OT_MIN is dropped up there
           because losing one band of a light is free. Dropping a CUBE is not:
           the one moment a puss ball has to be visible is the half second
           before it reaches the player, which is exactly when its otz is
           smallest. Clamping keeps it on screen.

           And the +40 is the room mesh's own bias, applied so an additive light
           lying on the floor does not fight the floor. A boulder STANDS on the
           floor, so biasing it back by the same 40 would sort it behind the
           polygon it has just landed on. src/rabisu.c's fireball does neither
           of these things either, for the same two reasons. */
        if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        POLY_F4 *p = (POLY_F4 *)ctx->next_packet;
        setPolyF4(p);
        setRGB0(p, cr, cg, cb);
        p->x0 = sv[0].vx; p->y0 = sv[0].vy;
        p->x1 = sv[1].vx; p->y1 = sv[1].vy;
        p->x2 = sv[3].vx; p->y2 = sv[3].vy;
        p->x3 = sv[2].vx; p->y3 = sv[2].vy;
        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], p);
        ctx->next_packet += sizeof(POLY_F4);
    }
}

/* =========================================================================
   THE BOILS
   ========================================================================= */

/* Where a boil is. The literals live in asag_arena.h beside the polygons they
   describe; this is the lookup. */
static void af_boil_at(int which, int32_t *x, int32_t *y, int32_t *z) {
    *z = ASAG_BOIL_Z;
    if (which == ASAG_BOIL_LEFT) { *x = ASAG_BOIL_LEFT_X;  *y = ASAG_BOIL_LEFT_Y;  }
    else                         { *x = ASAG_BOIL_RIGHT_X; *y = ASAG_BOIL_RIGHT_Y; }
}

static void af_puss_launch(int which);
static void af_begin_faint(void);

/* Both of them down. "At the moment that both boils are destroyed Asag will
   play its Faint animation", and "if both boils are destroyed the restore timer
   for both starts again from zero". */
static int af_both_burst(void) {
    int i;
    for (i = 0; i < ASAG_BOIL_COUNT; i++)
        if (!boil[i].burst) return 0;
    return 1;
}

static void af_boil_burst(int which) {
    boil[which].burst     = 1;
    boil[which].hp        = 0;
    boil[which].restore_t = 0;
    boil[which].hit_timer = 0;   /* the bar goes out with the light */
    asag_arena_set_boil_one(which, 0);      /* "the lights will dim and go off" */
    af_puss_launch(which);

    /* >>> THE POP. <<< A boil is an organ, and the game already owns the wet
       burst an organ makes when it dies — SFX_TNTCL_DIE, which is the
       tentacle's death and the Rafflesia's after it (src/rafflesia.c). It is
       the ONE event the player has to hear over the attack that is probably
       still playing, so it rides the burst itself rather than the hit: three
       damage lands in silence and the third one pops. It is in Asag's bank as
       of this change (src/sound.c); without that tag sound_play() would return
       silently in the only room it can ever be heard in. */
    sound_play(SFX_TNTCL_DIE);

    if (af_both_burst()) {
        /* BOTH TIMERS RESTART HERE, including the one belonging to the boil
           that has been down for twenty-nine seconds. That is what the brief
           asks for and it is also what makes the faint worth setting up: burst
           one, then burst the other late, and the pair come back together
           rather than one of them re-lighting during the faint you just
           bought. */
        int i;
        for (i = 0; i < ASAG_BOIL_COUNT; i++) boil[i].restore_t = 0;
        af_begin_faint();
    }
}

void asag_boil_damage(int which, int32_t amount) {
    if (which < 0 || which >= ASAG_BOIL_COUNT) return;
    if (phase == AF_OFF || dying_flag) return;
    if (boil[which].burst || amount <= 0) return;

    boil[which].hp = (int8_t)(boil[which].hp - amount);
    if (boil[which].hp <= 0) { af_boil_burst(which); return; }
    /* THE BAR COMES UP ON THE HIT, which is Asag's own rule and the Rabisu's
       before it — see asag_fight_draw_boil_bars(). Only on a hit the boil
       SURVIVES: the burst above returns first, because a bar over a dark organ
       is the same mistake as a bar over a corpse (STEP 11), and what says "that
       one is gone" is the light going out and the puss balls coming at you. */
    boil[which].hit_timer = AF_T_BOIL_BAR;
}

int asag_boil_target(int which, int32_t *cx, int32_t *cy, int32_t *cz,
                     int32_t *half_w, int32_t *half_h) {
    if (which < 0 || which >= ASAG_BOIL_COUNT) return 0;
    if (phase == AF_OFF || dying_flag) return 0;
    /* A BURST BOIL IS NOT A TARGET, which is the difference between "dark" and
       "gone". Returning it would let the player keep the crosshair on a dead
       organ and wonder why nothing happens — and, worse, would let the nearest-
       thing-in-the-circle test in graveolver_fire prefer it over the boil that
       is actually lit beside it. */
    if (boil[which].burst) return 0;

    int32_t x, y, z;
    af_boil_at(which, &x, &y, &z);
    if (cx) *cx = x;
    if (cy) *cy = y;
    if (cz) *cz = z;
    if (half_w) *half_w = ASAG_BOIL_HALF;
    if (half_h) *half_h = ASAG_BOIL_HALF;
    return 1;
}

/* How far short of the boil to test the line. See asag_fight.h for why this is
   a FRACTION of the shot and not a fixed number of units off the end of it.

   The whole of it: along the crosshair, Z accrues at a constant rate, so the
   ray covers the (boil_z - cam_z) between the camera and the target over
   exactly `depth`. The last ASAG_BOIL_CLEAR_BACKOFF of that Z is therefore the
   last (BACKOFF * depth) / (boil_z - cam_z) of the depth, at ANY heading —
   which is what a flat subtraction could not say.

   THE TWO DEGENERATE CASES BOTH END UP CLEAR, and that is right rather than
   merely safe. Standing inside the backoff of the wall (cam_z past 2654) leaves
   nothing between the muzzle and the organ, and a camera somehow behind the
   boil plane has no line to test at all; the floor of 1 is a segment of one
   unit, which nothing in this room blocks. */
int32_t asag_boil_clear_depth(int32_t boil_z, int32_t depth) {
    int32_t zspan = boil_z - cam_z;
    int32_t clr;
    if (zspan <= ASAG_BOIL_CLEAR_BACKOFF) return 1;
    clr = depth - (depth * ASAG_BOIL_CLEAR_BACKOFF) / zspan;
    return clr < 1 ? 1 : clr;
}

/* The reach to test a BOIL with. See the long note in asag_fight.h: the boils
   are in the corners of a room 3000 wide and the lantern's 1800 is a radius, so
   the sideways component alone put the far boil out of reach from a third of
   the arena. Never SHORTENS a weapon's own range — the gun's 4000 comes back
   unchanged — so both weapons can call it unconditionally. */
int32_t asag_boil_reach(int32_t weapon_range) {
    return weapon_range > ASAG_BOIL_REACH ? weapon_range : ASAG_BOIL_REACH;
}

/* Tick the restore. "The boils will be restored to full health and light up
   again after 30 seconds." The re-light is a short ramp rather than a snap for
   the same reason the opening scene ramps them up over two seconds: a light
   that appears between two frames reads as a draw error. */
static void af_boils_update(void) {
    int i;
    for (i = 0; i < ASAG_BOIL_COUNT; i++) {
        if (boil[i].hit_timer > 0) boil[i].hit_timer--;
        if (!boil[i].burst) continue;
        boil[i].restore_t++;
        if (boil[i].restore_t >= AF_T_BOIL_RESTORE) {
            boil[i].burst = 0;
            boil[i].hp    = AF_BOIL_HEALTH;
            asag_arena_set_boil_one(i, ASAG_BOIL_LEVEL_MAX);
        } else if (boil[i].restore_t > AF_T_BOIL_RESTORE - AF_T_BOIL_RELIGHT) {
            int32_t k = boil[i].restore_t - (AF_T_BOIL_RESTORE - AF_T_BOIL_RELIGHT);
            asag_arena_set_boil_one(i, (k * ASAG_BOIL_LEVEL_MAX) / AF_T_BOIL_RELIGHT);
        }
    }
}

/* =========================================================================
   THE PUSS BALLS
   ========================================================================= */

/* Three out of a bursting boil: one at the player, one 30 degrees either side.
   The heading is taken at the moment of the burst and never revised — they are
   thrown, not steered, and a player who moves after the burst should be able to
   walk out from between them. */
static void af_puss_launch(int which) {
    int32_t bx, by, bz;
    af_boil_at(which, &bx, &by, &bz);

    int32_t px = player_x(), pz = player_z();
    int32_t dx = px - bx, dz = pz - bz;

    /* The heading toward the player, as an angle in 4096ths. cam_rot 0 faces
       +Z and the forward vector is (sin, cos), so the same convention gives
       the angle whose (sin, cos) points from the boil at the player. Solved by
       the same binary search on isin/icos the director uses — this SDK has no
       arctangent, and answering in its own trig table is what keeps a heading
       from disagreeing with the picture it is drawn in. */
    int32_t base;
    {
        /* >>> THE SEARCH MUST BE OVER A HALF TURN, NOT A WHOLE ONE. <<< This is
           src/asag_boss.c's aim_angle: eleven halvings that solve
           tan(a) = num/den on the SDK's own isin/icos, with no division and
           nothing that overflows an int32. It is monotonic over [-1024, 1024]
           and only over that, so it needs den > 0 — and here den is
           (player z - boil z), which is ALWAYS NEGATIVE, because the boils are
           part of the back wall at z=2734 and the arena's floor stops at 2700.

           So the sign is taken out first and put back afterwards: search with
           |dz|, which answers in the half-plane facing the player, then reflect
           through 2048. Reflecting is exact rather than approximate — sin is
           even about 1024 and cos is odd — so the two branches meet cleanly if
           the geometry ever changes and dz comes out positive. */
        int flip    = (dz < 0);
        int32_t adz = flip ? -dz : dz;
        int32_t lo  = -1024, hi = 1024;
        if (adz < 1) adz = 1;
        while (hi - lo > 1) {
            int32_t mid = (lo + hi) >> 1, a = mid & 4095;
            if (isin(a) * adz < icos(a) * dx) lo = mid;
            else                              hi = mid;
        }
        base = (flip ? (2048 - lo) : lo) & 4095;
    }

    int n, made = 0;
    for (n = -1; n <= 1 && made < AF_PUSS_PER_BOIL; n++) {
        int slot;
        for (slot = 0; slot < AF_PUSS_MAX; slot++) if (!puss[slot].live) break;
        /* >>> A FULL POOL MUST NOT HANG ANYTHING. <<< It cannot happen with six
           slots and three per burst, but the runbook's STEP 12 asks for the
           guarantee rather than the arithmetic: nothing waits on a puss ball, so
           dropping one costs a projectile and never a state machine. */
        if (slot >= AF_PUSS_MAX) break;

        int32_t a = (base + n * AF_PUSS_SPREAD) & 4095;
        AfPuss *p = &puss[slot];
        p->x = bx; p->y = by; p->z = bz;
        p->vx = (AF_PUSS_SPEED * isin(a)) >> 12;
        p->vz = (AF_PUSS_SPEED * icos(a)) >> 12;
        p->vy = -AF_PUSS_RISE_V;     /* world +Y is DOWN, so up is negative */
        p->life = AF_PUSS_LIFE;
        p->live = 1;
        made++;
    }
}

static void af_puss_update(void) {
    int i;
    for (i = 0; i < AF_PUSS_MAX; i++) {
        AfPuss *p = &puss[i];
        if (!p->live) continue;

        p->x += p->vx;
        p->z += p->vz;
        p->y += p->vy;
        p->vy += AF_PUSS_GRAV;

        /* >>> NO collision_segment_blocked() ON THESE, AND THAT IS NOT AN
           OVERSIGHT. <<< src/web.c tests the segment just travelled so a fast
           shot cannot tunnel a thin wall, which is right in a mansion. Here the
           launch point is a boil at z=2734 and the collision proxy's back wall
           is at z=2700 — 34 units IN FRONT of it, because the proxy is a
           rectangle and the lumps stand proud of the wall it approximates. Every
           ball would be destroyed on its first frame by a wall standing inside
           the thing that fired it. The arena is an empty box, so the bounds test
           below is the whole of what a segment test would have bought. */
        if (p->y >= AF_FLOOR_Y ||
            p->x < -1500 || p->x > 1500 || p->z < 0 || p->z > 2800 ||
            --p->life <= 0) {
            p->live = 0;
            continue;
        }

        /* A RADIAL 3-AXIS CONTACT TEST, not a flat XZ one: these arc, and a
           plan-view test would count a ball that sailed past at head height.
           Same rule src/web.c follows. */
        int32_t dx = p->x - player_x();
        int32_t dy = p->y - player_y();
        int32_t dz = p->z - player_z();
        int32_t reach = AF_PUSS_HALF + AF_PLAYER_RADIUS;
        if (dx * dx + dy * dy + dz * dz <= reach * reach) {
            af_hurt(AF_DMG_PUSS);
            p->live = 0;
        }
    }
}

/* =========================================================================
   THE LASER
   =========================================================================
   The beam leaves his mouth and its GROUND POINT sweeps an arc at a fixed reach
   in front of him. Every cell the point crosses catches fire and burns for
   three seconds. */

/* Set one whole COLUMN of the landing third alight - all five rows of it.
   Re-lighting a column the sweep lingers on simply refreshes it, which is
   right: the fire does not get hotter, it gets newer.

   >>> A COLUMN AND NOT A CELL, AND THAT IS THE ATTACK. <<< See AF_LAS_SWEEP_Z
   above for why. */
static void af_trail_light_col(int32_t x) {
    int c = af_col_of(x), r;
    if (c < 0) return;
    for (r = AF_ROW_LASER_LO; r <= AF_ROW_LASER_HI; r++)
        trail[r * AF_COLS + c] = AF_T_LAS_TRAIL;
}

static void af_trail_update(void) {
    int i;
    if (trail_cooldown > 0) trail_cooldown--;

    int hit = 0;
    int here = af_cell_of(player_x(), player_z());
    for (i = 0; i < AF_CELLS; i++) {
        if (!trail[i]) continue;
        trail[i]--;
        if (i == here && trail[i]) hit = 1;
    }

    if (hit && trail_cooldown <= 0) {
        af_hurt(AF_DMG_LASER);
        trail_cooldown = AF_T_TRAIL_COOLDOWN;
    }
}

/* One frame of the beam. `p` is 0..256 across the sweep.

   >>> THE ORIGIN IS RE-SOLVED EVERY FRAME AND THE GROUND POINT IS NOT DERIVED
   FROM IT. <<< The runbook's telegraphed-path rule says to FREEZE the origin
   so a path does not swing around as the boss moves, which is the right answer
   for a Rabisu that re-solves its arc position while the tell plays. Asag is
   the opposite case: his head is animating through the whole beam and the beam
   has to stay in his mouth, so the origin follows it. What is fixed is the
   sweep — the arc is measured from the head's settled XZ, captured once when
   the beam starts, so the ground point traces a clean arc even while his head
   bobs inside it. */
static void af_laser_step(int32_t p) {
    VECTOR f;
    if (asag_face_point(&f)) {
        las_ox = f.vx; las_oy = f.vy; las_oz = f.vz;
    }

    /* Straight across, wall to wall, in whichever direction this attack is
       going. The ends run a little inside the perimeter so the beam's own
       footprint never straddles it. */
    int32_t from = -1450, to = 1450;
    if (las_dir) { from = 1450; to = -1450; }
    las_gx = from + ((to - from) * p) / 256;
    las_gz = AF_LAS_SWEEP_Z;

    af_trail_light_col(las_gx);

    /* >>> NO SEPARATE BEAM-CONTACT TEST, AND THE REASON IS THE COLUMN. <<< The
       first version had one, because the sweep lit only the cell it was
       standing on and being swept over was therefore a different event from
       standing in the fire. It is not any more: the beam sets the player's
       whole COLUMN alight the instant it reaches them, so af_trail_update()
       below IS the contact test, on the same 30 damage and the same cooldown.
       A second test here would double-charge the one frame the beam arrives. */
}

/* =========================================================================
   THE SLAM'S BOULDERS
   ========================================================================= */

/* Arm the two boulders. Fixed cells, so there is nothing to choose and nothing
   to fail - see the constants above for why they are fixed and where.

   `hit_done` is shared through bld_hit_done rather than held per boulder: the
   zone is the zone, and being caught in it should cost twenty whether one
   boulder lands in it or both. */
static void af_boulders_arm(void) {
    /* HIS PAIR FIRST, THE FLANK PAIR SECOND, because the stagger runs down this
       array in order and the attack should land nearest him and travel out. */
    static const int8_t COL[AF_BOULDERS] = {
        AF_BLD_COL_L, AF_BLD_COL_R, AF_BLD_COL_FL, AF_BLD_COL_FR,
    };
    static const int8_t ROW[AF_BOULDERS] = {
        AF_BLD_ROW,   AF_BLD_ROW,   AF_BLD_ROW_FLANK, AF_BLD_ROW_FLANK,
    };
    int n;
    for (n = 0; n < AF_BOULDERS; n++) {
        boulder[n].cell = (int16_t)(ROW[n] * AF_COLS + COL[n]);
        /* The stagger is a NEGATIVE start, so both fall for AF_T_BLD_FALL and
           the second simply begins later. Shortening the second one's fall
           instead would have made it visibly faster than the first. */
        boulder[n].t    = (int16_t)(-n * AF_T_BLD_STAGGER);
        boulder[n].live = 1;
        boulder[n].hit_done = 0;
    }
    bld_hit_done   = 0;
    bld_sfx_done   = 0;
    boulders_armed = 1;
}

static void af_boulder_pos(const AfBoulder *b, int32_t *x, int32_t *y, int32_t *z) {
    int c = b->cell % AF_COLS, w = b->cell / AF_COLS;
    *x = (AF_COL_X[c] + AF_COL_X[c + 1]) / 2;
    *z = (AF_ROW_Z[w] + AF_ROW_Z[w + 1]) / 2;

    int32_t t = b->t;
    if (t < 0) t = 0;
    if (t >= AF_T_BLD_FALL) { *y = AF_FLOOR_Y - AF_BLD_HALF; return; }
    /* ACCELERATING, on t^2. A boulder on a linear descent reads as a boulder
       being lowered, which is the same note the opening scene's fall carries. */
    int32_t p   = (t * 256) / AF_T_BLD_FALL;
    int32_t acc = (p * p) / 256;
    *y = (AF_FLOOR_Y - AF_BLD_HALF) - AF_BLD_DROP + (AF_BLD_DROP * acc) / 256;
}

static void af_boulders_update(void) {
    if (!boulders_armed) return;

    int any = 0, n;
    for (n = 0; n < AF_BOULDERS; n++) {
        AfBoulder *b = &boulder[n];
        if (!b->live) continue;
        any = 1;
        b->t++;

        if (b->t >= AF_T_BLD_FALL + AF_T_BLD_LINGER) { b->live = 0; continue; }

        if (!b->hit_done && b->t >= AF_T_BLD_FALL) {
            b->hit_done = 1;

            int32_t x, y, z;
            af_boulder_pos(b, &x, &y, &z);

            /* THE SMASH. src/particles.c's spawn_rock_burst - spawn_wood_burst
               with stone colours and square chunks, which is what "similar to
               when we smash our Crate asset but coloured to suit the boulders"
               asks for. It is spawned at the boulder's BASE rather than its
               centre, so the chips come off the floor it just hit rather than
               out of the middle of a cube that is no longer there.

               >>> IT OVERWRITES THE WHOLE PARTICLE POOL. <<< That is why the
               two boulders are staggered; see AF_T_BLD_STAGGER. */
            spawn_rock_burst(x, AF_FLOOR_Y - 20, z);

            /* >>> ONE RUMBLE FOR THE PAIR, ON THE FIRST ONE DOWN. <<< The two
               impacts are AF_T_BLD_STAGGER apart, which is nine frames — a
               second play would land 0.15 s into a 2.12 s clip on the same
               voice and CUT it, so two rocks would sound quieter than one. The
               stagger exists to separate the two particle bursts, not the
               audio; a rumble that starts on the first impact covers both.

               SFX_RUMBLE is the Living Statue's stone grind, borrowed because
               it is already the sound of rock meeting floor and a fourth clip
               saying the same thing would cost the bank 13 KB for nothing. It
               had to be tagged SND_BANK_ASAG to be audible here — see the note
               on it in src/sound.c, and note it plays on VOICE 9, which nothing
               else in this arena touches. */
            if (!bld_sfx_done) {
                bld_sfx_done = 1;
                sound_play(SFX_RUMBLE);
            }

            /* >>> THE DAMAGE IS THE ZONE, NOT THE CUBE. <<< Four boulders
               cannot cover the ground between them, and the design is that
               being caught in the SHAPE when they land is what hurts: Asag's
               third plus the middle row's two flanks, which is what the four
               rocks are spread across. THE FOUR ROCKS ARE ALSO ALL THAT LIGHTS
               UP NOW — the faint wash over the rest of the shape was taken
               out of af_draw_boulders, so this zone is wider than what the
               floor shows and that note is the one to read before moving
               either. Once per slam, however many boulders land in it. */
            if (!bld_hit_done && af_in_slam_zone()) {
                bld_hit_done = 1;
                af_hurt(AF_DMG_BOULDER);
            }
        }
    }

    /* "The light turns off after the cubes have landed and disappeared." */
    if (!any) boulders_armed = 0;
}

/* =========================================================================
   THE VOMIT
   ========================================================================= */

/* Spawned from the LIVE mouth - the twitch is supposed to be visible in the
   spray - and thrown down the LANE rather than straight down.

   >>> THE SPREAD DRAWS THE ZONE, AND THE ZONE IS A LANE. <<< A particle
   system's job here is to make the player believe in a hazard the floor glow
   has already drawn, so the throw has to have the same shape as the thing that
   hurts. One throw: a huge Z spread and a small X one, fanning the stream down
   the 1000-wide, 2800-long lane and deliberately keeping it out of the flanks,
   which are not this attack's business.

   >>> THERE WAS A SECOND THROW HERE AND IT IS GONE WITH THE CROSSBAR. <<< For
   as long as the zone was a plus, half the particles went out across the middle
   row on the particle slot's parity. The zone is the lane and nothing else now
   (see af_cell_in_vomit), so that half would be spraying green over floor that
   cannot hurt anyone — the same mismatch this note warns about, reached from
   the other direction. If the zone ever grows a crossbar again, the spread has
   to grow one back on the same commit. */
static void af_vom_spit(int32_t mx, int32_t my, int32_t mz) {
    int i, made = 0;
    for (i = 0; i < AF_VOM_PARTICLES && made < 4; i++) {
        if (vom[i].life > 0) continue;
        vom[i].x = (int16_t)mx;
        vom[i].y = (int16_t)my;
        vom[i].z = (int16_t)mz;
        /* DOWN THE LANE: fans along its length, stays inside its width. */
        vom[i].vx = (int16_t)((rand() % 25) - 12);
        vom[i].vz = (int16_t)((rand() % 121) - 60);
        vom[i].vy = (int16_t)(2 + (rand() % 7));
        vom[i].life = (int16_t)(40 + (rand() % 26));
        made++;
    }
}

static void af_vom_update(void) {
    int i;
    for (i = 0; i < AF_VOM_PARTICLES; i++) {
        if (vom[i].life <= 0) continue;
        vom[i].life--;
        vom[i].x = (int16_t)(vom[i].x + vom[i].vx);
        vom[i].y = (int16_t)(vom[i].y + vom[i].vy);
        vom[i].z = (int16_t)(vom[i].z + vom[i].vz);
        vom[i].vy = (int16_t)(vom[i].vy + 2);
        if (vom[i].y >= AF_FLOOR_Y) vom[i].life = 0;
    }
}

/* =========================================================================
   THE ATTACK LOOP
   =========================================================================
   One enum, one phase_t, one switch, one enter_phase — the runbook's STEP 4,
   and the same shape src/asag_boss.c's opening already has. */

static void af_enter(AfPhase p) {
    phase   = p;
    phase_t = 0;
    slam_hit_done = 0;
    vom_hit_done  = 0;
    las_firing    = 0;

    switch (p) {
    case AF_LASER: asag_play(ASAG_CLIP_LASER, 0); las_dir = !las_dir; break;
    case AF_SLAM:  asag_play(ASAG_CLIP_SLAM,  0); break;
    case AF_VOMIT: asag_play(ASAG_CLIP_VOMIT, 0); break;
    case AF_FAINT: asag_play(ASAG_CLIP_FAINT, 0); break;
    /* THE IDLES LOOP. A one-shot idle would report done after 1.25 s and the
       phase would then be standing on a held frame for the other 2.75 — which
       looks like a boss that froze. The phase's own counter ends it. */
    case AF_IDLE_A:
    case AF_IDLE_B:
    case AF_IDLE_C: asag_play(ASAG_CLIP_IDLE, 1); break;
    default: break;
    }
}

/* What comes after `p` in the loop: LASER -> idle -> SLAM -> idle -> VOMIT ->
   idle -> LASER. */
static AfPhase af_next(AfPhase p) {
    switch (p) {
    case AF_LASER:  return AF_IDLE_A;
    case AF_IDLE_A: return AF_SLAM;
    case AF_SLAM:   return AF_IDLE_B;
    case AF_IDLE_B: return AF_VOMIT;
    case AF_VOMIT:  return AF_IDLE_C;
    case AF_IDLE_C: return AF_LASER;
    default:        return AF_LASER;
    }
}

static int af_is_idle(AfPhase p) {
    return p == AF_IDLE_A || p == AF_IDLE_B || p == AF_IDLE_C;
}

/* >>> THE SECOND BOIL BURSTING CUTS STRAIGHT INTO WHATEVER HE IS DOING. <<<
   This used to be a `faint_pending` flag consumed at the END of the running
   phase, on the reading that an attack should play out first — and it is the
   one thing about the boils that did not read on screen. Bursting the second
   boil is the hardest thing in this fight to do and the reward for it arrived
   anywhere between instantly and four seconds later, depending on which frame
   of which attack the shot landed on; at the far end of that range the player
   has stopped connecting the two events at all, which is why it looked like it
   "doesn't do it every time". So: the faint is an INTERRUPT now. Mid-laser,
   mid-slam, mid-vomit, mid-idle, it takes over on the frame the boil pops.

   af_enter() is what makes that safe rather than a special case: it restarts
   the clip, zeroes phase_t and clears every per-attack latch, so the abandoned
   attack leaves nothing behind. What it does NOT clear is what is already in
   the air — a boulder still falling, a burning cell, the puss balls this very
   burst just threw — and that is the same rule the death uses. The commitment
   was made when the thing launched.

   Called from af_boil_burst(), i.e. from the WEAPON's call into
   asag_boil_damage() rather than from this file's own update. The guard is
   therefore not decoration: a shot can only reach a boil during the fight, but
   this is the one entry point into the phase machine that does not come from
   asag_fight_update(), and a faint started over a corpse would restart a clip
   the death sequence is in the middle of posing. */
static void af_begin_faint(void) {
    if (phase == AF_OFF || dying_flag || dead_flag) return;
    /* THE LOOP MOVES ON. See resume_phase's note: the interrupted phase never
       finished, so the loop picks up after it rather than replaying it. */
    resume_phase = af_next(phase);
    af_enter(AF_FAINT);
}

/* The end of a phase. The faint no longer passes through here on its way IN —
   it cuts in from af_begin_faint() above — so all this does is walk the loop,
   with the one exception that the phase after a faint is the one the interrupt
   already chose. */
static void af_phase_over(void) {
    if (phase == AF_FAINT) { af_enter(resume_phase); return; }
    af_enter(af_next(phase));
}

/* >>> EVERY NON-TERMINAL PHASE ENDS ON A FRAME COUNT, NOT ONLY ON A CLIP. <<<
   The runbook's STEP 12 asks for exactly this, and this boss has a specific
   reason to need it: src/asag.c's read_file() refuses a clip that would reach
   the stack, SILENTLY, and an absent clip never advances and never reports
   asag_clip_done(). A phase waiting only on the clip would sit there forever
   and look like a boss that froze. The watchdog is the clip's own length plus a
   second, which is never reached by a clip that loaded. */
static int32_t af_clip_watchdog(AsagClip c) {
    int32_t t = asag_clip_ticks(c);
    return t > 0 ? t + 60 : 60;     /* absent: give up after a second */
}

static int af_attack_over(AsagClip c) {
    return asag_clip_done() || phase_t >= af_clip_watchdog(c);
}

/* =========================================================================
   EXPOSURE
   =========================================================================
   The whole of the fight's difficulty. See asag_fight.h for the table. */
int asag_exposed(void) {
    if (phase == AF_OFF || dying_flag || dead_flag) return 0;

    AsagClip c;
    int32_t lead;
    switch (phase) {
    case AF_SLAM:  c = ASAG_CLIP_SLAM;  lead = AF_EXPOSE_LEAD;       break;
    case AF_VOMIT: c = ASAG_CLIP_VOMIT; lead = AF_EXPOSE_LEAD;       break;
    case AF_FAINT: c = ASAG_CLIP_FAINT; lead = AF_EXPOSE_LEAD_FAINT; break;
    /* THE LASER NEVER EXPOSES HIM, and neither does an idle. */
    default: return 0;
    }

    /* The window closes `lead` before the ANIMATION ends, and the animation's
       length is asked for rather than written down — see asag_clip_ticks(). A
       clip that never loaded reports 0, which closes the window immediately;
       that is the right failure, because the attack it belongs to is not
       playing either. */
    int32_t total = asag_clip_ticks(c);
    if (total <= 0) return 0;
    return phase_t < total - lead;
}

/* =========================================================================
   HEALTH
   ========================================================================= */

int32_t asag_health(void)     { return health; }
int     asag_fight_dying(void) { return dying_flag; }
int     asag_fight_dead(void)  { return dead_flag; }
void    asag_fight_set_dead(void) { dead_flag = 1; }

void asag_damage(int32_t amount) {
    /* ONE GATE FOR EVERY WEAPON. The alternative is three call sites each
       remembering to test asag_exposed(), and the one that forgets makes the
       boss killable through the whole fight with nothing on screen to explain
       it. */
    if (amount <= 0) return;
    if (phase == AF_OFF || dying_flag || dead_flag) return;
    if (!asag_exposed()) return;

    health -= amount;
    hit_timer = 120;                 /* 2 s of bar, the Rabisu's RBS_BAR_TIMER */
    if (health <= 0) {
        health = 0;
        /* `dying`, NOT `dead`: the body has to stay drawn for the sequence that
           is about to play over it. src/asag_boss.c watches this. STEP 11. */
        dying_flag = 1;
        asag_fight_stop();
    }
}

/* =========================================================================
   LIFETIME
   ========================================================================= */

void asag_fight_reset(void) {
    int i;
    phase         = AF_OFF;
    phase_t       = 0;
    resume_phase  = AF_LASER;
    health        = ASAG_MAX_HEALTH;
    hit_timer     = 0;
    dying_flag    = 0;
    dead_flag     = 0;
    slam_hit_done = 0;
    vom_hit_done  = 0;
    trail_cooldown = 0;
    las_firing    = 0;
    las_dir       = 0;
    boulders_armed = 0;
    bld_hit_done  = 0;
    bld_sfx_done  = 0;
    hurt_sfx_cooldown = 0;

    for (i = 0; i < ASAG_BOIL_COUNT; i++) {
        boil[i].hp        = AF_BOIL_HEALTH;
        boil[i].burst     = 0;
        boil[i].restore_t = 0;
        boil[i].hit_timer = 0;
    }
    for (i = 0; i < AF_CELLS; i++)     trail[i] = 0;
    for (i = 0; i < AF_PUSS_MAX; i++)  puss[i].live = 0;
    for (i = 0; i < AF_BOULDERS; i++)  boulder[i].live = 0;
    for (i = 0; i < AF_VOM_PARTICLES; i++) vom[i].life = 0;
    vom_zone_lit = 0;

    /* NOT asag_arena_set_boil_glow(0) HERE. The arena's own init already puts
       them out on every arrival, and the OPENING SCENE is what lights them —
       writing a level from the fight's reset would fight the scene's two-second
       ramp for the frame the two happen to overlap. */
}

void asag_fight_begin(void) {
    if (dying_flag || dead_flag) return;
    /* THE LOOP STARTS ON THE LASER, as briefed. The opening scene hands over on
       a LOOPING idle, so starting with an idle would play one twice. */
    af_enter(AF_LASER);
}

void asag_fight_stop(void) {
    int i;
    phase   = AF_OFF;
    phase_t = 0;
    las_firing    = 0;
    boulders_armed = 0;
    for (i = 0; i < AF_CELLS; i++)     trail[i] = 0;
    for (i = 0; i < AF_PUSS_MAX; i++)  puss[i].live = 0;
    for (i = 0; i < AF_BOULDERS; i++)  boulder[i].live = 0;
    for (i = 0; i < AF_VOM_PARTICLES; i++) vom[i].life = 0;
    vom_zone_lit = 0;
    /* THE LIGHTS GO OUT WITH HIM. Two organs still breathing on the wall over a
       corpse would be the one thing on screen insisting the fight is still on —
       the same judgement STEP 11 makes about a health bar. */
    for (i = 0; i < ASAG_BOIL_COUNT; i++) {
        asag_arena_set_boil_one(i, 0);
        boil[i].hit_timer = 0;      /* ...and their bars with them */
    }
}

int asag_fight_active(void) { return phase != AF_OFF; }

/* =========================================================================
   UPDATE
   ========================================================================= */

void asag_fight_update(void) {
    if (current_area != STATE_ASAG_ARENA) return;
    if (hit_timer > 0) hit_timer--;
    /* OUTSIDE THE `phase == AF_OFF` GATE BELOW, with the projectiles: a puss
       ball or a burning cell can still hurt the player after the loop has
       stopped, so the cue that rate-limits those hits has to keep cooling. */
    if (hurt_sfx_cooldown > 0) hurt_sfx_cooldown--;

    /* >>> WHAT IS IN THE AIR KEEPS FLYING EVEN WHEN THE LOOP IS OFF, AND THAT
       IS DELIBERATE — up to a point. <<< The runbook's rule is that launching a
       projectile was the commitment and escaping it should not be free just
       because the boss changed state. So these tick outside the phase switch.
       The one state that DOES clear them is death, and it clears them through
       asag_fight_stop() rather than by zeroing fields here. */
    af_trail_update();
    af_puss_update();
    af_boulders_update();
    af_vom_update();

    /* THE LANE'S LIGHT RAMPS, and it is ticked out here with the projectiles
       rather than inside the vomit's case so that it still fades DOWN after the
       attack has ended or been cut short by a death. A zone light left on over
       a corpse is the same mistake as a health bar left on over one. */
    {
        int spraying = (phase == AF_VOMIT &&
                        phase_t >= AF_T_VOM_START && phase_t <= AF_T_VOM_END);
        vom_zone_lit += spraying ? 16 : -12;
        if (vom_zone_lit > 256) vom_zone_lit = 256;
        if (vom_zone_lit < 0)   vom_zone_lit = 0;
    }

    /* >>> THE BODY'S RED FLASH IS DRIVEN FROM HERE, EVERY FRAME. <<< It is
       (a fresh hit) OR (dying), which is src/rabisu.c's exact rule: held for the
       WHOLE death rather than the two seconds the killing blow's timer buys, so
       the glow runs unbroken under the settle, the freeze and the burn instead
       of dropping back to normal halfway through and re-igniting. Driving it
       from the fight rather than letting the body own a timer keeps src/asag.c
       free of anything that decides. */
    asag_set_hit_glow(hit_timer > 0 || dying_flag);

    /* >>> AND THE EXPOSURE LIGHT, FROM THE SAME PLACE AND ON THE SAME RULE.
       <<< asag_exposed() is this file's own predicate and it is read here, once
       a frame, rather than being re-derived by the body: src/asag.c decides
       nothing about the fight and this keeps it that way.

       It is deliberately the WHOLE of asag_exposed() and not "the phase is a
       slam" — the window closes AF_EXPOSE_LEAD before the clip does, and the
       half second in which he is still slamming and can no longer be hurt is
       exactly the half second the player most needs the light to be off in. */
    asag_set_head_glow(asag_exposed());

    if (phase == AF_OFF) return;
    if (!asag_model_loaded()) return;

    af_boils_update();
    phase_t++;

    switch (phase) {

    /* ---- LASER ------------------------------------------------------------
       Head in place at t 75, beam from t 90 to t 270, clip ends at t 322. */
    case AF_LASER:
        las_firing = (phase_t >= AF_T_LAS_FIRE && phase_t <= AF_T_LAS_END);
        if (las_firing) {
            int32_t p = ((phase_t - AF_T_LAS_FIRE) * 256)
                        / (AF_T_LAS_END - AF_T_LAS_FIRE);
            af_laser_step(p);
            /* >>> THE CUE IS ON THE FRAME THE BEAM LEAVES HIM, not on the frame
               the charge starts. <<< STEP 8 of tools/ADDING_A_SOUND.txt asks for
               the sound of the EVENT, and the event the player is reading here
               is the beam appearing — the charge already has its own tell (the
               head's glow, af_draw_charge). == and not >=: phase_t is reset to 0
               by af_enter and steps by exactly one a frame, so this fires once
               and needs no latch of its own. The clip is 4.44 s against a 3 s
               sweep, so it runs a little past the beam on purpose. */
            if (phase_t == AF_T_LAS_FIRE) sound_play(SFX_LASER);
        } else if (phase_t >= AF_T_LAS_CHARGE && phase_t < AF_T_LAS_FIRE) {
            /* The charge: the head glows and nothing else happens. Its only
               state is the phase counter; the draw reads that. */
            VECTOR f;
            if (asag_face_point(&f)) { las_ox = f.vx; las_oy = f.vy; las_oz = f.vz; }
        }
        if (af_attack_over(ASAG_CLIP_LASER)) af_phase_over();
        break;

    /* ---- SLAM -------------------------------------------------------------
       The head reaches the floor at t 112 and the four markers light with it. */
    case AF_SLAM:
        /* THE HEAD COMING DOWN. Fired AF_T_SLAM_SFX_LEAD frames BEFORE the
           landing so the sample's own impact lands on the animation's — see
           that constant. == and not >=, which is the laser's and the vomit's
           cue exactly: phase_t is zeroed by af_enter and steps by one, so this
           fires once and needs no latch of its own. SFX_RUMBLE still follows a
           beat later, when the boulders land (af_boulders_update). */
        if (phase_t == AF_T_SLAM_IMPACT - AF_T_SLAM_SFX_LEAD)
            sound_play(SFX_SLAM_ASAG);

        if (!slam_hit_done && phase_t >= AF_T_SLAM_IMPACT) {
            slam_hit_done = 1;

            /* "Caught under the slamming head" — measured to where the head
               ACTUALLY IS on the impact frame, not to a constant. asag.c owns
               the pose; re-deriving the landing spot here would be a second
               copy of a number the art can move. */
            VECTOR f;
            if (asag_face_point(&f) &&
                af_dist_xz(f.vx, f.vz, player_x(), player_z())
                    <= AF_SLAM_RADIUS + AF_PLAYER_RADIUS)
                af_hurt(AF_DMG_SLAM);

            af_boulders_arm();
        }
        if (af_attack_over(ASAG_CLIP_SLAM)) af_phase_over();
        break;

    /* ---- VOMIT ------------------------------------------------------------
       Head in place at t 112, twitching from t 150 to t 265. */
    case AF_VOMIT:
        /* HE STARTS SPEWING AT AF_T_VOM_START, which is the tick the head
           begins to twitch — the same moment the first particle leaves him and
           the lane starts lighting. == for the reason the laser's cue uses it:
           phase_t is reset by af_enter and steps by one, so no latch is needed.
           Outside the spray window so it reads as the cue for the whole block
           below rather than as part of the per-frame spit. */
        if (phase_t == AF_T_VOM_START) sound_play(SFX_VOMIT);
        if (phase_t >= AF_T_VOM_START && phase_t <= AF_T_VOM_END) {
            /* The particles leave his LIVE mouth, twitch and all. The damage
               zone is not a point at all any more - it is the lane, columns
               5..9, fixed by the room rather than by where his head happens to
               be this eighth of a second. That removes the whole problem the
               first version needed a frozen mouth for: a hit box that jittered
               60 units eight times a second is a hit box nobody can learn. */
            {
                VECTOR f;
                if (asag_face_point(&f)) {
                    vom_mx = f.vx; vom_my = f.vy; vom_mz = f.vz;
                }
                af_vom_spit(vom_mx, vom_my, vom_mz);
            }

            /* >>> CHECKED EVERY FRAME OF THE SPRAY, NOT ONLY ON ITS FIRST. <<<
               The brief says "if the player is standing there when the vomit
               starts", which read literally means walking into a visible sheet
               of falling vomit is free. It is checked continuously and fires
               ONCE, which is a superset of that reading and is the one that
               matches what is on screen - the zone is lit and pouring for two
               seconds, and anything in it should be hit. A player who leaves
               before the spray reaches them is still safe, which is the half of
               the literal rule that mattered. */
            if (!vom_hit_done && af_in_vomit_zone()) {
                vom_hit_done = 1;
                af_hurt(AF_DMG_VOMIT);
                /* THE SAME STATUS THE SPIDER'S WEB CARRIES, by the same call:
                   half walk speed and no sprint for five seconds. Refreshing
                   rather than stacking is player_poison()'s own rule. */
                player_poison();
            }
        }
        if (af_attack_over(ASAG_CLIP_VOMIT)) af_phase_over();
        break;

    /* ---- THE FAINT --------------------------------------------------------
       No attack in it. It is the long exposure, and the only thing that ends it
       is the clip. */
    case AF_FAINT:
        if (af_attack_over(ASAG_CLIP_FAINT)) af_phase_over();
        break;

    /* ---- THE IDLES --------------------------------------------------------
       Two seconds of nothing. Nothing cuts them short any more: the faint that
       used to is now an interrupt and has already taken the phase over by the
       time this runs. See af_begin_faint(). */
    default:
        if (phase_t >= AF_T_IDLE) af_phase_over();
        break;
    }
}

/* =========================================================================
   DRAWING
   ========================================================================= */

/* The laser: a beam from his mouth to its ground point, plus the burn.

   THE BEAM IS TWO CROSSED QUADS, which is the cheapest thing that looks like a
   beam from every angle. One quad vanishes edge-on; two at right angles about
   the beam's own axis never both do. They are additive, so where they cross
   simply reads as the brightest part, which is where the core of a beam is
   anyway. */
static void af_draw_laser(RenderContext *ctx) {
    if (!las_firing) return;

    SVECTOR v[4];
    int k;
    for (k = 0; k < 4; k++) v[k].pad = 0;

    const int32_t HALF_TOP = 26, HALF_BOT = 64;

    /* Quad 1: spread along X. Quad 2: spread along Z. */
    v[0].vx = (int16_t)(las_ox - HALF_TOP); v[0].vy = (int16_t)las_oy; v[0].vz = (int16_t)las_oz;
    v[1].vx = (int16_t)(las_ox + HALF_TOP); v[1].vy = (int16_t)las_oy; v[1].vz = (int16_t)las_oz;
    v[2].vx = (int16_t)(las_gx - HALF_BOT); v[2].vy = (int16_t)(AF_FLOOR_Y - 6); v[2].vz = (int16_t)las_gz;
    v[3].vx = (int16_t)(las_gx + HALF_BOT); v[3].vy = (int16_t)(AF_FLOOR_Y - 6); v[3].vz = (int16_t)las_gz;
    rbs_glow_quad(ctx, v, 255, 190, 80);

    v[0].vx = (int16_t)las_ox; v[0].vz = (int16_t)(las_oz - HALF_TOP);
    v[1].vx = (int16_t)las_ox; v[1].vz = (int16_t)(las_oz + HALF_TOP);
    v[2].vx = (int16_t)las_gx; v[2].vz = (int16_t)(las_gz - HALF_BOT);
    v[3].vx = (int16_t)las_gx; v[3].vz = (int16_t)(las_gz + HALF_BOT);
    rbs_glow_quad(ctx, v, 255, 190, 80);

    /* The point where it meets the floor, which is the part the player watches
       coming. */
    af_glow_point(ctx, las_gx, AF_FLOOR_Y - 20, las_gz, 210, 3, 255, 215, 120);
}

/* The head's charge glow, which is the laser's whole tell. */
static void af_draw_charge(RenderContext *ctx) {
    if (phase != AF_LASER) return;
    if (phase_t < AF_T_LAS_CHARGE) return;

    int32_t lev;
    if (phase_t < AF_T_LAS_FIRE)
        lev = ((phase_t - AF_T_LAS_CHARGE) * 256) / (AF_T_LAS_FIRE - AF_T_LAS_CHARGE);
    else if (phase_t <= AF_T_LAS_END)
        lev = 256;
    else
        return;

    af_glow_point(ctx, las_ox, las_oy, las_oz,
                  (150 * lev) / 256, 3,
                  255, (uint8_t)(150 + (lev * 60) / 256), 70);
}

/* The burning trail. Cells fade toward red as they cool, which is what makes
   "three seconds" legible without a number on screen. */
static void af_draw_trail(RenderContext *ctx) {
    int i;
    for (i = 0; i < AF_CELLS; i++) {
        int32_t age = trail[i];
        if (!age) continue;
        int32_t lev = (age * 256) / AF_T_LAS_TRAIL;
        af_pool(ctx,
                i,
                (uint8_t)((255 * lev) >> 8),
                (uint8_t)((110 * lev * lev) >> 16),
                (uint8_t)(( 30 * lev * lev) >> 16));
    }
}

/* The slam's markers and the boulders falling into them.

   >>> ONLY THE CELLS A BOULDER IS FALLING INTO LIGHT. <<< There used to be a
   faint wash over the whole damage shape underneath these markers — Asag's
   third plus the middle row's two flanks — on the argument that a hazard the
   player cannot see is an ambush. On screen it was too much blue: it covered
   the floor directly under Asag and both of the room's corners at his end,
   which is most of what the camera looks at during a slam, and the four bright
   markers had nothing to stand out against. The wash is gone; the four markers
   are the whole of the floor effect.

   >>> THE DAMAGE SHAPE IS UNCHANGED AND IS NOW WIDER THAN WHAT IS LIT. <<<
   af_in_slam_zone() still costs twenty anywhere in af_cell_in_slam(), so the
   promise the grid section makes — that what is lit is what hurts — no longer
   holds for this one attack. That is deliberate rather than an oversight: the
   four rocks are spread one per square across the shape, so they are the
   telegraph for it, and the slam stays the attack that clears his end of the
   room. If the damage is ever meant to shrink to the four cells as well, it is
   af_cell_in_slam() that moves, not this loop. */
static void af_draw_boulders(RenderContext *ctx) {
    if (!boulders_armed) return;

    int n;
    for (n = 0; n < AF_BOULDERS; n++) {
        AfBoulder *b = &boulder[n];
        if (!b->live) continue;

        /* The marker brightens as the thing above it gets closer, which turns a
           light into a countdown. Negative t is the staggered boulder waiting
           to start; it holds at the dimmest level until it does. */
        int32_t t = b->t < 0 ? 0 : b->t;
        int32_t p = t < AF_T_BLD_FALL ? (t * 256) / AF_T_BLD_FALL : 256;
        int32_t lev = 90 + (p * 166) / 256;
        af_pool(ctx, b->cell,
                (uint8_t)((110 * lev) >> 8),
                (uint8_t)((190 * lev) >> 8),
                (uint8_t)((255 * lev) >> 8));

        /* The cube exists for its FALL AND NOTHING ELSE: not before it has
           begun (b->t < 0 is the staggered one still waiting), and >>> NOT FOR
           ONE FRAME AFTER IT LANDS. <<<

           It used to sit on the floor for AF_T_BLD_LINGER, which put an intact
           boulder in the middle of its own smash — the rock burst spawns on the
           impact frame, so the chips flew out of a cube that was still standing
           there, and the whole thing read as a stutter rather than as an
           impact. A falling rock either shatters or it does not; this one
           shatters, so the cube goes the instant it touches down and the
           particles are what is left of it.

           AF_T_BLD_LINGER IS STILL DOING ITS JOB — it is what keeps the MARKER
           LIGHT above lit for a moment after the landing, which is what
           af_boulders_update's "the light turns off after the cubes have landed
           and disappeared" means. The cube's life and the light's are two
           different spans and this is the line that separates them. */
        if (b->t < 0 || b->t >= AF_T_BLD_FALL) continue;
        int32_t x, y, z;
        af_boulder_pos(b, &x, &y, &z);
        af_cube(ctx, x, y, z, AF_BLD_HALF, 96, 62, 38);
    }
}

/* The vomit: the LANE it is about to poison, then the spray falling into it.

   THE FLOOR GOES FIRST AND IT IS THE IMPORTANT HALF. The particles say what is
   happening; the lit lane says WHERE, exactly, to the polygon - and because the
   damage test calls af_cell_in_vomit() on the cell the player is standing in,
   the same function this loop paints with, what is lit and what hurts cannot
   disagree. A player who has been caught once knows to read the floor. */
static void af_draw_vomit(RenderContext *ctx) {
    if (vom_zone_lit > 0) {
        int32_t lev = vom_zone_lit;
        int r, c;
        /* ONE TEST PER CELL. The zone is a plain column range again, so a
           nested pair of loops over AF_COL_VOM_LO..HI would do — but the pools
           are additive, and asking the predicate is what keeps this loop and
           the damage test from ever painting different floor. */
        for (r = 0; r < AF_ROWS; r++)
            for (c = 0; c < AF_COLS; c++)
                if (af_cell_in_vomit(r, c))
                    af_pool(ctx, r * AF_COLS + c,
                            (uint8_t)((40 * lev) >> 8),
                            (uint8_t)((150 * lev) >> 8),
                            (uint8_t)((45 * lev) >> 8));
    }

    int i;
    for (i = 0; i < AF_VOM_PARTICLES; i++) {
        if (vom[i].life <= 0) continue;
        int32_t lev = vom[i].life > 30 ? 256 : (vom[i].life * 256) / 30;
        af_glow_point(ctx, vom[i].x, vom[i].y, vom[i].z, 80, 2,
                      (uint8_t)((90 * lev) >> 8),
                      (uint8_t)((255 * lev) >> 8),
                      (uint8_t)((70 * lev) >> 8));
    }
}

static void af_draw_puss(RenderContext *ctx) {
    int i;
    for (i = 0; i < AF_PUSS_MAX; i++)
        if (puss[i].live)
            af_cube(ctx, puss[i].x, puss[i].y, puss[i].z, AF_PUSS_HALF,
                    235, 225, 90);
}

void asag_fight_draw(RenderContext *ctx) {
    if (current_area != STATE_ASAG_ARENA) return;

    /* >>> HOISTED OUT OF THE POOL LOOPS. <<< isin/icos are SDK calls, and a lit
       zone would otherwise ask for them 125 times a frame to compute one pair
       of constants that cannot change while a frame is being queued. STEP 3C
       fix 1 of tools/DIAGNOSING_FRAME_RATE.txt — the same two lines
       src/asag_arena.c's mesh loop already hoists, for the same reason. */
    af_cam_sn  = isin(cam_rot);
    af_cam_cs  = icos(cam_rot);
    af_no_cull = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);

    /* THE TRAIL AND THE PROJECTILES DRAW EVEN WHEN THE LOOP IS OFF, for the
       same reason they still tick: what is in the air stays in the air. The
       beam and the charge belong to a phase and stop with it. */
    af_draw_trail(ctx);
    af_draw_boulders(ctx);
    af_draw_puss(ctx);
    af_draw_vomit(ctx);

    if (phase == AF_OFF) return;
    af_draw_charge(ctx);
    af_draw_laser(ctx);
}

/* =========================================================================
   THE HEALTH BARS
   =========================================================================
   >>> THERE ARE THREE OF THEM NOW AND THEY SHARE ONE ROUTINE. <<< Asag's hangs
   over his head; the two boils' hang over the boils. They are the same piece of
   UI — src/rabisu.c's draw_rbs_bar — pinned to three different world points,
   and the only things that differ are the point, the fraction and the width.

   Factoring it out was not tidiness. The projection has four separate traps in
   it (a zero z, the +/-1023 screen clamp, the packet-budget test, and the
   background having to sort one OT bucket BEHIND the fill) and three hand
   copies of that is three chances to get one wrong — the boil bars were written
   as a copy first and the copy dropped the sz test, which shows up as a bar
   smeared across the screen when the camera passes through the back wall. */

/* One bar, `width` px wide, centred over the world point (wx, wy, wz) and
   filled num/den. Silently draws nothing for a point that will not project. */
static void af_bar_at(RenderContext *ctx,
                      int32_t wx, int32_t wy, int32_t wz,
                      int32_t num, int32_t den, int16_t width) {
    if (den <= 0) return;
    if (num < 0) num = 0;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    SVECTOR top;
    top.vx  = (int16_t)wx;
    top.vy  = (int16_t)wy;
    top.vz  = (int16_t)wz;
    top.pad = 0;

    DVECTOR sv;
    int32_t sz;
    gte_ldv0(&top);
    gte_rtps();
    gte_stsxy(&sv);
    gte_stsz(&sz);
    if (sz == 0) return;
    if (sv.vx <= -1023 || sv.vx >= 1023 || sv.vy <= -1023 || sv.vy >= 1023) return;

    int32_t otz = SCENE_OT_MIN;
    int16_t bar_x = (int16_t)(sv.vx - width / 2);
    int16_t bar_y = sv.vy;

    if (ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *bg = (TILE *)ctx->next_packet;
        setTile(bg);
        setRGB0(bg, 40, 40, 40);
        setXY0(bg, bar_x, bar_y);
        setWH(bg, width, 6);
        /* otz + 1, i.e. one bucket FURTHER BACK than the fill: the OT is sorted
           back-to-front, so the trough has to go behind the thing standing in
           it. Both are in front of all scene geometry (SCENE_OT_MIN). */
        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz + 1], bg);
        ctx->next_packet += sizeof(TILE);
    }

    int16_t fill_w = (int16_t)((num * width) / den);
    if (fill_w > 0 && ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *fill = (TILE *)ctx->next_packet;
        setTile(fill);
        setRGB0(fill, 200, 20, 20);   /* the Rabisu's red, and only red */
        setXY0(fill, bar_x, bar_y);
        setWH(fill, fill_w, 6);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], fill);
        ctx->next_packet += sizeof(TILE);
    }
}

/* ---- THE TWO BOIL BARS ----------------------------------------------------
   Asag's bar, on the two organs that are the way in to him. They obey the same
   rule his does and for the same reasons: RED, two seconds after a hit that the
   boil SURVIVED, nothing the rest of the time.

   >>> WHY A BOIL NEEDED ONE AT ALL. <<< Asag has 20 HP and a boil has 3, and
   the boils are what the player actually spends the fight shooting — bursting
   both is the only thing that buys the long exposure. Without a bar there was
   no feedback at all on a hit that did not burst one: a boil at 3 HP and a boil
   at 1 looked identical, so "one more shot" was a guess. The boss's own bar is
   the less important of the two and it had one first.

   THEY ARE NARROWER THAN HIS, 40 against 60. Two of them are on screen at once,
   side by side on the same wall, at a range where his bar is already small; at
   his width they read as one strip. And the fraction they show is out of 3, so
   a bar 60 wide would step in 20-pixel jumps and look broken.

   NOT DRAWN FOR A BURST BOIL, which asag_boil_target() already refuses as a
   target — the hit_timer is cleared by the burst, so this falls out rather than
   being tested for twice. Not drawn once he is dying either: asag_fight_stop()
   clears both timers on the killing blow. */
#define AF_BOIL_BAR_W     40
#define AF_BOIL_BAR_RISE  60   /* px of world above the cluster's top edge */

static void af_draw_boil_bars(RenderContext *ctx) {
    int i;
    for (i = 0; i < ASAG_BOIL_COUNT; i++) {
        if (boil[i].hit_timer <= 0) continue;
        int32_t x, y, z;
        af_boil_at(i, &x, &y, &z);
        af_bar_at(ctx, x, y - ASAG_BOIL_HALF - AF_BOIL_BAR_RISE, z,
                  boil[i].hp, AF_BOIL_HEALTH, AF_BOIL_BAR_W);
    }
}

/* Asag's own health bar. src/rabisu.c's draw_rbs_bar, hung over the HEAD rather
   than over a model's top: he is 1669 units long and lies along the view axis,
   so a bar above his centre would float over his flank, a long way from the
   part the player is shooting at.

   NO BAR ONCE HE IS DYING, which is STEP 11's judgement: an empty bar over a
   death sequence is the one piece of UI still insisting there is a fight on. */
void asag_fight_draw_bar(RenderContext *ctx) {
    if (current_area != STATE_ASAG_ARENA) return;

    /* THE BOILS FIRST, so his own bar sorts in front of them if the camera ever
       lines the three up. They have their own timers and their own visibility
       rule, so this is not gated on his. */
    af_draw_boil_bars(ctx);

    /* >>> UP ONLY AFTER A HIT, AND THE RABISU'S RULE EXACTLY. <<< An earlier
       version also raised it for the whole of every exposure window and turned
       it green, on the argument that "can I hurt him now" is the question this
       fight asks every second. It was dropped: a bar that comes and goes on its
       own is a second UI element competing with the thing it hangs over, and
       the answer it gave was already on screen - the boils, the clip he is
       playing, and now the red glow the body itself carries when a hit lands.

       So it is what it is everywhere else in this game: two seconds of a RED
       bar when something connects, and nothing the rest of the time.

       NO BAR ONCE HE IS DYING, which is STEP 11's judgement: an empty bar over
       a death sequence is the one piece of UI still insisting there is a fight
       on. */
    if (hit_timer <= 0 || dying_flag || dead_flag) return;

    int32_t hx, hy, hz, hw, hh;
    if (!asag_head_box(&hx, &hy, &hz, &hw, &hh)) return;

    af_bar_at(ctx, hx, hy - hh - 90 /* clear of the head */, hz,
              health, ASAG_MAX_HEALTH, 60);
}
