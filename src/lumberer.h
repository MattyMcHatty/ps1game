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
 * SOUND. No new clips: SPU RAM is nearly full (tools/ADDING_A_SOUND.txt) and
 * this enemy did not earn any. SFX_CRWL_SCRM — the crawler's cry, and already
 * in SND_BANK_CATACOMBS — stands in for both the strike and the death. That is
 * deliberate, not an oversight; give it its own clips by adding two SfxIDs and
 * changing the two sound_play calls in src/lumberer.c.
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
   uses). "About 30% greater than the Crawler's wake radius": CRW_WAKE_RADIUS is
   1100, so 1430.

   Unlike the Mushroom Head's, this is the WHOLE test — no facing dot and no
   sightline. The Mushroom's two extra conditions exist so it can be crept up on
   from behind during a patrol; the Lumberer is specified to wake on proximity
   alone, so a player who walks into 1430 has woken it whichever way either of
   them is pointing. Note 1430 is inside the Tomb's fog (tomb.c), so it never
   wakes to a player it could not have seen. */
#define LMB_ALERT_RADIUS   1430

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

/* Steering and knockback, mirroring the Mushroom Head's. No nav graph: a
   lumberer fights in the room it patrols. The body radius and the feeler are
   sized up from the mushroom's to match the wider silhouette. */
#define LMB_SEP_RADIUS      260
#define LMB_SEP_WEIGHT        2
#define LMB_BODY_RADIUS     100
#define LMB_DOOR_CLEARANCE  100
#define LMB_FEELER_LEN      200
#define LMB_TURN_RATE         2    /* slower than the mushroom's 3: it lumbers */
#define LMB_STEER_COMMIT     30
#define LMB_KNOCKBACK        35    /* decays 7/8 a frame -> ~280 units total   */

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
    int           atk_tick;      /* frames into the current attack phase      */

    /* The wave. `wave_t` counts from 1 while one is expanding and is 0
       otherwise; `wave_hit` makes it damage the player at most once, the same
       shape the Rabisu's uses. */
    int           wave_t;
    int           wave_hit;

    int32_t       facing;        /* last travel dir, packed: hi16 X, lo16 Z   */
    int           steer_timer;
    int           steer_dir;
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
