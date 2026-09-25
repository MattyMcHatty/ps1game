#ifndef LUMBERER_H
#define LUMBERER_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
#include "title.h"   /* GameState — each lumberer is tagged with its area */

/* -----------------------------------------------------------------------
 * The Lumberer — Chapter 3's patrolling bruiser.
 *
 * Built on the Mushroom Head (src/mushroom.c), which is the game's other
 * two-point patroller and the enemy whose walk cycle already switches between a
 * front sprite and a back one. What it takes from there: the AUTHORED patrol
 * (mushroom_add's two points, so world.c can rebuild a placement from a save
 * without the room's geometry resident), the feeler steering generalised over a
 * goal, the alert test, separation, knockback, the floor shadow and the health
 * bar. What it takes from the Crawler instead is the DRAWING: one 256x256 sheet
 * split across two VRAM pages, and a subdivided body quad (see LMB_SUBDIV).
 *
 * TWO STATES, as specified — patrolling and alert — with the attack as a
 * two-phase interlude inside the alert one rather than a third state the enemy
 * can be met in:
 *
 *   PATROL  It walks between its two authored points and back again, forever,
 *           at LMB_WALK_SPEED. Nothing else happens here.
 *   ALERT   The player came within LMB_ALERT_RADIUS, or something hit it. It
 *           walks at the player at the SAME speed it patrols at — this enemy
 *           has one gait and the alert changes only where it is going.
 *   WINDUP  } The attack. Inside LMB_ATTACK_RADIUS it plants itself, raises the
 *   STRIKE  } arm for a second (frame 5), then swings it down and lets out a
 *           shockwave (frame 6) which it holds for another second. Then it
 *           either attacks again, if the player is still in reach, or goes back
 *           to walking. It is rooted for the whole two seconds.
 *
 * >>> THE TWO SECONDS ARE THE DODGE. <<< The wave is centred on the body and
 * reaches LMB_SHOCK_RADIUS, which is larger than the radius that triggered the
 * attack — so standing still is a hit. Walking (12 a frame) covers 1440 units
 * in the wind-up against a 600 radius, so leaving is always possible and always
 * a decision made BEFORE the arm comes down, which is the same bargain the
 * Rabisu's shockwave offers (src/rabisu.h, RBS_AI_SHOCK).
 *
 * 10 HP and NO weaknesses: ten crucifaxe swings or ten rounds of any kind.
 *
 * TEXTURES. ONE 256x256 sheet, 3x2, read left to right and top row first:
 *
 *     frame 0 (image 1)  }  approaching the camera: the two alternate
 *     frame 1 (image 2)  }
 *     frame 2 (image 3)  }  walking away from it: likewise
 *     frame 3 (image 4)  }
 *     frame 4 (image 5)     the arm raised — the wind-up
 *     frame 5 (image 6)     the arm down — the shockwave
 *
 * Which PAIR is showing is decided per frame by the dot of the body's travel
 * direction with the direction to the camera, exactly as the Mushroom Head
 * picks between mushy_run and mushy_behind. The cadence is LMB_ANIM_RATE and it
 * is deliberately slow, to match the gait.
 *
 * >>> IT SHIPS AS TWO 256x128 HALVES, AND THEY ARE ON LOAN FROM THE FRONT END.
 * <<< A 256-row 8bpp texture cannot be placed anywhere in this game's VRAM (V is
 * eight bits: y%256 + height <= 256), so the sheet is cut along the row
 * boundary — which is also the frame boundary — and the two halves take:
 *
 *   x[448,576) y128   mansion.tim (the opening still) + dbl_dr_hlf + grdngtl
 *   x[832,960) y128   xt_dr_lft_hlf + xt_dr_rt_hlf
 *
 * ...and the CLUT lines of mansion.tim (256,495) and xt_dr_lft_hlf (672,486)
 * with them. Nothing in that list is drawn anywhere in Chapter 3: one is the
 * New Game opening, the other four are door panels belonging to the Attic Exit,
 * the Garden Stairs and the Garden Courtyard.
 *
 * >>> THE FIRST VERSION OF THIS ENEMY TOOK THE CRAWLER'S PAGES INSTEAD, AND
 * THAT WAS A PLACEMENT RULE DRESSED UP AS A VRAM SAVING. <<< It meant a
 * lumberer and a crawler could never stand in the same room — a restriction on
 * the DESIGN paid for in the wrong currency, because the pages it "saved" were
 * not scarce, they were merely unexamined. tools/VRAM_MAP_CATACOMBS.txt is the
 * sweep that examined them: Chapter 3 draws seven textures across four rooms,
 * and both of the blocks above were sitting there holding art the chapter
 * cannot reach. The two enemies are independent now and may share any room.
 *
 * NEITHER LOAN IS FREE, AND BOTH ARE PAID BACK EXPLICITLY. All five textures
 * were startup-only LoadImages with no way to put them back — the same trap the
 * zombie pair was in before the Crawler — so both owners were given one:
 *   door_anim_restore_panels()  re-reads the four leaves; main.c calls it on
 *                               entry to any room outside TEXBANK_CATACOMBS
 *   intro_start()               re-reads the still on the frame New Game is
 *                               confirmed, instead of main() doing it at boot
 * lumberers_upload_textures() sets the flag that arms the first of those. Both
 * are no-ops until it does, so no other room in the game pays anything.
 *
 * PERSISTENCE is the spiders'/mushrooms' model: ONE global array tagged by
 * area, with every update/draw/weapon loop skipping any instance whose area is
 * not current_area — current_area and NEVER game_state, or the enemy freezes
 * for as long as the inventory menu is open (tools/ADDING_AN_ENEMY.txt STEP 6).
 * MAX_LUMBERERS is the budget for the WHOLE GAME, not per room, and
 * lumberer_add drops SILENTLY once it is full. Raise it (and WD_MAX_LUMBERERS
 * in src/world.h with it) before placing a seventh.
 *
 * SOUND. TWO CLIPS OF ITS OWN, both SND_BANK_CATACOMBS and both on borrowed
 * voices (16 and 21 — the reasoning is in sound.c's sfx_channel, and it is the
 * chapter's fifth and sixth borrowing):
 *
 *   SFX_LMBR_MOAN  the walk. Re-triggered from C on LMB_MOAN_INTERVAL for as
 *                  long as the body is TRAVELLING, so a rooted or pinned
 *                  lumberer falls silent — the same `moved` flag the animation
 *                  clock runs on, and the same reasoning.
 *   SFX_LMBR_YELL  the shockwave, on the first frame of the strike.
 *
 * >>> THE DEATH IS STILL SFX_CRWL_SCRM, THE CRAWLER'S CRY, AND THAT IS THE ONE
 * BORROWING LEFT. <<< It was both of these before the two clips above existed.
 * Give it a third of its own by adding one SfxID and changing the one remaining
 * sound_play(SFX_CRWL_SCRM) in src/lumberer.c; the bank has the room.
 * ----------------------------------------------------------------------- */

#define MAX_LUMBERERS         6    /* WHOLE-GAME budget — see above            */
#define LMB_MAX_HEALTH       10    /* ten crucifaxe swings or ten rounds        */

/* ---- Speed, in units per frame. ONE number, because the brief is one gait:
   it lumbers toward its next patrol point and it lumbers toward the player at
   exactly the same rate. The player walks at 12 and sprints at 20 (camera.c),
   so 5 is well under a walk — an alerted lumberer can always be outpaced, and
   its threat is the reach and the damage rather than the closing speed. */
#define LMB_WALK_SPEED        5

/* Alert radius, TRUE RADIAL (not the Manhattan sum the coarse zombie wake
   uses). It started at 1430 — "about 30% greater than the Crawler's wake
   radius", CRW_WAKE_RADIUS being 1100 — and has since been CUT BY A FIFTH, to
   1144, so the player can cross an aisle mouth one block away without waking it.
   That leaves it a hair above the Crawler's, which is the shape it should have
   had: this one is slower than the Crawler and hits far harder, so the range at
   which it notices you is not where its threat is meant to come from.

   Unlike the Mushroom Head's, this is the WHOLE test — no facing dot and no
   sightline. The Mushroom's two extra conditions exist so it can be crept up on
   from behind during a patrol; the Lumberer is specified to wake on proximity
   alone, so a player who walks into 1144 has woken it whichever way either of
   them is pointing — and, since the cut, through a loculus block as readily as
   down an open aisle. It closes the distance by walking the aisles either way
   (LMB_NAV below), so a wake through a wall costs the player the detour rather
   than a monster that grinds on the far side of it. Note 1144 is well inside the
   Tomb's fog (tomb.c), so it never wakes to a player it could not have seen. */
#define LMB_ALERT_RADIUS   1144

/* ---- The reach. TRUE RADIAL again, and it is the enemy's whole character:
   "it does not have to be as close as other monsters, because it has one long
   arm". For scale, the Crawler catches at 180 and the Mushroom Head at 200,
   both Manhattan sums measured against their own footprint. 450 is roughly
   two and a half times that, and it clears this enemy's own half-width (173)
   by a margin, so the arm really does reach past the body.

   It is checked against the TRUE radius so that "450" means 450 whichever
   bearing the player approaches from — the Manhattan tests the contact-damage
   code elsewhere uses are sized to a sprite footprint, which is not what this
   is. */
#define LMB_ATTACK_RADIUS   450

/* ---- The attack, in frames at 60 fps. One second on the raised arm, one
   second on the swing. The wave goes out on the FIRST frame of the strike: the
   raised arm is the warning and the swing is the event, so putting the hitbox
   anywhere else in the two seconds would make one of the two frames a lie. */
#define LMB_WINDUP_FRAMES    60
#define LMB_STRIKE_FRAMES    60

/* ---- The shockwave. A ring racing outward across the floor with a short
   vertical skirt on its leading edge, copied in shape from the Rabisu's
   (src/rabisu.c, draw_rbs_shockwave) and smaller: that one reaches
   RBS_SWEEP_RADIUS, 1146, and this one reaches 600, a little over half.

   It is centred on the BODY and it is larger than LMB_ATTACK_RADIUS, so a
   player who was in reach when the arm went up and did not move is in the wave
   when it comes down. It expands over LMB_SHOCK_EXPAND and then lingers, and
   the whole thing is over inside the strike's own second.

   Damage is "roughly 35% of the player's maximum HP" — MAX_HEALTH is 100, so
   35. The _Static_assert in the .c keeps the two from drifting apart. There is
   no knockback: the brief asks for damage and nothing else, and the Rabisu's
   270-unit shove is a boss's answer to an attack that cannot be dodged, which
   this one can. */
#define LMB_SHOCK_RADIUS    600
#define LMB_SHOCK_EXPAND     24    /* frames to reach full radius              */
#define LMB_SHOCK_LINGER     12    /* frames it stays up after reaching it     */
#define LMB_SHOCK_BAND      140    /* radial width of the bright ring          */
#define LMB_SHOCK_WALL_H    150    /* the vertical skirt on the leading edge   */
#define LMB_SHOCK_SEGS       16    /* ring resolution: segments around the arc */

/* ---- Sprite. "Taller than they are wide. Taller than the Crawler, but not as
   wide." The Crawler is 220 x 220 in half-extents; this is 173 x 260, so it is
   40 units taller and 47 narrower, and the ratio 173:260 is the source frame's
   own 85:128 — the art is not stretched.

   LMB_Y_OFFSET + LMB_HALF_H == 150 is the invariant every sprite enemy in this
   game keeps: that sum is the drop from the entity anchor to the feet, so a
   taller body has to come back out of the offset or the thing floats. 260 makes
   the offset NEGATIVE, which is new here and is fine — it simply means the
   sprite's centre is above the anchor rather than below it.

   HEADROOM: standing on the Tomb's floor the anchor is -149, so the centre is
   at -259 and the top edge at -519, against a vault the room draws at -800
   (src/tomb.h — the visual mesh and the collision proxy agree there, so -800 is
   the height the player really sees). 281 units of clearance. */
#define LMB_HALF_W          173
#define LMB_HALF_H          260
#define LMB_Y_OFFSET        (150 - LMB_HALF_H)   /* -110; keeps the feet down */

/* >>> THE BODY IS DRAWN AS A GRID, NOT AS ONE QUAD. <<< The GPU DROPS a
   primitive whose screen extent passes 1023 pixels in either axis — it does not
   clip it — and at gte_SetGeomScreen(256) a 520-unit span hits that at a view
   depth of 520*256/1023 = 130, which is inside this enemy's own reach. A 2x2
   grid makes each piece 173 x 260 and moves the threshold to 65, closer than
   the body radius lets anything stand. Same reasoning and same shape as
   CRW_SUBDIV; tools/ADDING_AN_ENEMY.txt mistake 13 is the long version. */
#define LMB_SUBDIV            2

/* Frames per animation step. "Equally as slow" as the movement: at 30 a full
   two-frame cycle takes a second, against the Mushroom Head's 25-frame flip at
   a speed of 6 and the Crawler's 6-frame step at 14. Like the crawler's, the
   clock only advances on frames the body actually TRAVELLED, so a lumberer
   pinned against geometry stops animating instead of marching on the spot. */
#define LMB_ANIM_RATE        30

#define LMB_BAR_TIMER_MAX   120

/* ---- The walking moan, in frames at 60 fps. The brief is "play it, wait two
   seconds, play it again", so the INTERVAL is the clip plus the gap and not the
   gap alone: lmbrmoan.vag runs 2.96 s, which is 178 frames, and 120 more is the
   two seconds of silence after it. Firing every 120 would instead re-key the
   voice 58 frames before the moan had finished and the player would never hear
   the end of it.

   >>> SO THIS CONSTANT IS CUT TO THE CLIP'S LENGTH AND RE-CUTTING THE CLIP
   BREAKS IT SILENTLY. <<< Same trap as SFX_EXPLODE's length in rabisu_boss.c
   (tools/ADDING_A_SOUND.txt STEP 1B). If lmbrmoan.vag is ever re-recorded or
   trimmed, work its length out in frames again and reset LMB_MOAN_CLIP.

   The clock only advances on frames the body actually TRAVELLED, so a lumberer
   rooted in its wind-up or pinned against a loculus goes quiet and picks the
   cycle up where it left off — the rule LMB_ANIM_RATE already follows. */
#define LMB_MOAN_CLIP       178    /* lmbrmoan.vag, 2.96 s at 60 fps           */
#define LMB_MOAN_GAP        120    /* the two seconds of silence after it      */
#define LMB_MOAN_INTERVAL   (LMB_MOAN_CLIP + LMB_MOAN_GAP)

/* Steering and knockback, mirroring the Mushroom Head's. The body radius and
   the feeler are sized up from the mushroom's to match the wider silhouette.
   This is the LOCAL layer only — one feeler, one wall-follow — and it is what
   the LMB_NAV block below exists to sit on top of. */
#define LMB_SEP_RADIUS      260
#define LMB_SEP_WEIGHT        2
#define LMB_BODY_RADIUS     100
#define LMB_DOOR_CLEARANCE  100
#define LMB_FEELER_LEN      200
#define LMB_TURN_RATE         2    /* slower than the mushroom's 3: it lumbers */
/* The HEADING is kept as a direction vector of this Manhattan magnitude, and
   the step is scaled down out of it (lmb_steer, lmb_face). It is not a tuning
   number and there is nothing to taste in it — it is RESOLUTION, and the whole
   reason the heading is not simply the last step:

   A GAIT IS FIVE UNITS, AND A FIVE-UNIT VECTOR CANNOT REPRESENT A TURN. Round a
   turning direction into integer components that small and the smaller
   component truncates to zero, which snaps the heading back onto the axis it
   was leaving — so it never leaves. At 256 the same blend has eight bits to
   turn in and the rounding is worth a fraction of a degree. This is the reason
   the whole of this game's steering family is written against the step and gets
   away with it only while the goal is axis-aligned. */
#define LMB_FACE_SCALE      256
#define LMB_STEER_COMMIT     30
#define LMB_KNOCKBACK        35    /* decays 7/8 a frame -> ~280 units total   */

/* ---- LMB_NAV: ROUTING, which is a different problem from STEERING.
   >>> THE FEELER ABOVE FOLLOWS A WALL; IT DOES NOT GO ROUND A BLOCK. <<< An
   alerted lumberer used to aim the raw player delta at lmb_steer and nothing
   else, and in the Tomb that is a straight line into the side of a loculus.
   The wall-follow then slid it along that face for LMB_STEER_COMMIT frames,
   expired, re-aimed at the player, and drove it back into the same face: a
   monster grinding on the corner it should have walked around.

   The fix is the ZOMBIE's, taken whole (src/zombie.c, "Navigation graph"):
   carve the room into ZONES, bridge them with NODES, breadth-first the zone
   graph toward the player's zone, and walk to the next node instead of at the
   player — with a sightline that CANCELS all of it, so an enemy that can see
   you still charges you in a straight line. The parts kept verbatim, and why:

     - THE TWO-STAGE CROSSING. Walk to the near-side clearance point first to
       line up square with the gap, then aim at the FAR-side one. Never at the
       node centre: the centre sits ON the zone boundary, so a body standing
       there is still in the zone it came from, still routed to the node it is
       standing on, and its goal is the spot it already occupies. That is the
       zombie's "parks in the gap" bug and it would be this one's too.
     - THE `md <= nd` LATCH that keeps the near stage from re-arming once the
       body is deeper into the crossing than the staging point was.
     - nav_clear, the one-shot "step clear of the opening before you turn",
       without which an off-axis player drags the body back into the corner it
       has just come round.

   WHAT IS DIFFERENT HERE is the shape of the graph, and only that. A zombie's
   zones are ROOMS and its nodes are DOORWAYS, authored one or two at a time.
   The Tomb is one chamber with nine free-standing blocks in it, so its zones
   are the sixteen cells of the aisle grid and its nodes are the twenty-four
   aisle segments joining them (lmb_tomb_nav_* in the .c). Same tables, same
   BFS, same staging — an open-plan room described in the doorway vocabulary.

   LMB_LOS_WIDTH is the half-width of the body sightline. A zero-width line
   threads a gap a body cannot walk, which is what wedged zombies in the kitchen
   doorway; 150 is 1.5x LMB_BODY_RADIUS, the same margin ZMB_LOS_WIDTH 90 gives
   the zombie's 60, and Manhattan-normalised so it realises 106..150 — erring
   toward over-cautious sight, never over-confident.

   LMB_LOS_COMMIT is hysteresis: keep charging for half a second after the last
   clear sightline, so a line that flickers across a block corner does not flip
   the body between "charge" and "route" on alternate frames.

   LMB_NODE_CLEAR_DIST is "close enough to a clearance point to count as having
   reached it", sized off the 5-unit gait rather than the zombie's 8. */
#define LMB_NAV_MAX_ZONES    16    /* stack-array sizing in lmb_nav_next_node  */
#define LMB_LOS_WIDTH       150
#define LMB_LOS_COMMIT       30
#define LMB_NODE_CLEAR_DIST 120

#define LMB_WAYPOINT_REACH  140    /* Manhattan; flip the patrol leg here      */

#define LMB_SHADOW_W        190
#define LMB_SHADOW_D         85

typedef enum {
    LMB_PATROL,   /* unalerted: walking the two-point patrol                  */
    LMB_ALERT,    /* closing on the player at the same speed                  */
    LMB_WINDUP,   /* rooted, arm raised (frame 4)                             */
    LMB_STRIKE,   /* rooted, arm down (frame 5); the wave is out              */
    LMB_DEAD,
} LumbererState;

typedef struct {
    int32_t       x, y, z;
    /* The patrol. `pa`/`pb` are the two authored points and never change; `y`
       above is the standing anchor, authored too (lumberer_add takes it) so a
       save rebuild needs no floor probe. */
    int32_t       pa_x, pa_z, pb_x, pb_z;
    int32_t       spawn_y;
    int           to_b;          /* 1 = heading for pb, 0 = heading for pa    */

    int32_t       vy;
    int32_t       kb_vx, kb_vz;

    int           health;
    int           hit_timer;
    int           anim_tick;
    int           moan_tick;     /* frames left before the next walking moan  */
    int           atk_tick;      /* frames into the current attack phase      */

    /* The wave. `wave_t` counts from 1 while one is expanding and is 0
       otherwise; `wave_hit` makes it damage the player at most once, the same
       shape the Rabisu's uses. */
    int           wave_t;
    int           wave_hit;

    int32_t       facing;        /* last travel dir, packed: hi16 X, lo16 Z   */
    int           steer_timer;
    int           steer_dir;
    /* Routing (LMB_NAV). `los_timer` counts down from LMB_LOS_COMMIT while the
       player is out of body-sight and pins the body to a straight charge while
       it is positive; `nav_clear` is the node whose far side still has to be
       stepped clear of, or -1. Neither is saved: lumberers_rest() zeroes the
       whole struct on the way out of the room. */
    int           los_timer;
    int           nav_clear;
    int           moved;         /* travelled this frame — drives anim_tick   */

    LumbererState state;
    int32_t       active;
    int           on_upper_floor;
    int           on_ramp;
    GameState     area;
} Lumberer;

extern Lumberer lumberers[MAX_LUMBERERS];
extern int      lumberer_count;

/* Register the two sheet halves with texmgr (TEXBANK_CATACOMBS) and load the
   shared floor shadow. Call ONCE at startup, beside the other enemies'. */
void lumberers_load_textures(void);

/* Stream both halves into VRAM. Called from main.c's STATE_LOADING block on
   entry to STATE_TOMB, in place of the crawler's — see the note on the sheet
   above, and the branch itself in main.c. */
void lumberers_upload_textures(void);

/* Place one, patrolling between (ax,az) and (bx,bz) and standing at anchor `y`.
   Everything is AUTHORED — nothing here reads the current room's mesh — so this
   is safe to call for a room that is not loaded, which is what lets world.c
   rebuild every visited room from a save delta. Returns its index, or -1 if the
   global pool is full. */
int  lumberer_add(int32_t ax, int32_t az, int32_t bx, int32_t bz,
                  int32_t y, GameState area);

void lumberers_init(void);
void lumberers_reset(void);
/* Put every still-living lumberer back on patrol point A, unalerted, at full
   health (deaths stick). Called when leaving a room and when saving. */
void lumberers_rest(void);
void update_lumberers(void);
void draw_lumberers(RenderContext *ctx);

/* Deal damage from any source. Wakes a patrolling lumberer, flashes the health
   bar, and handles death. Both the crucifaxe and a Grave-olver round pass 1
   (scaled by lumberer_scale_damage). */
void lumberer_damage(Lumberer *s, int dmg);

/* Scale a hit by this enemy's weaknesses — there are none, by design. */
int32_t lumberer_scale_damage(int32_t base, DamageType type);

/* Grave-olver hitscan support: sprite-centre Y, half-height and half-width. */
void lumberer_body(const Lumberer *s, int32_t *cyc, int32_t *hh, int32_t *hw);

/* Tell the renderer which texture window the current area has active, so each
   sprite can be drawn unmasked and the area's window then restored. Pass NULL
   for areas that use no texture window. */
void lumberers_set_texwindow(const RECT *tw);

#endif
