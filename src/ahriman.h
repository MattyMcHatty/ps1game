#ifndef AHRIMAN_H
#define AHRIMAN_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
#include "title.h"         /* GameState - each Ahriman is tagged with its area */
#include "crucifaxe.h"     /* SWING_RANGE - the axe test is measured from it   */

/* -----------------------------------------------------------------------
 * AHRIMAN - a winged eyeball that floats at the player's eye line, and the
 * Underworld's second enemy.
 *
 * >>> EVERY PLACEMENT NAMES A HEIGHT: A, B OR C. <<< That is the design, and
 * the user asks for each Ahriman with one. Never default it - ask.
 *   AHR_HEIGHT_A  just BELOW the player's eye line  (body centre 60 under it)
 *   AHR_HEIGHT_B  AT the player's eye line
 *   AHR_HEIGHT_C  just ABOVE the player's eye line  (body centre 60 over it)
 *
 * THE HEIGHT IS A DRAWN LIFT, THE CREEP'S MODEL (src/creep.h). The anchor stays
 * on the floor every frame (apply_ddog_height), so ramps and storeys keep
 * working, and the body is drawn and aimed at anchor + the height's offset -
 * ahriman_body_y() is the one place that resolves it. The arithmetic, from two
 * different height functions (tools/ADDING_AN_ENEMY.txt mistake 14):
 *   an enemy's anchor   apply_ddog_height -> floor - GROUND_FLOOR_Y
 *   the player's eye    apply_height      -> floor - GROUND_FLOOR_Y - 40
 * so the eye is AHR_EYE_RISE (40) above an enemy anchor on the same floor, and
 * height B's offset is -40.
 *
 * THE FIGHT, in order:
 *
 *   HUNT    From the moment it spawns, or the moment the player walks in - no
 *           dormant state. It floats at the player until it is within
 *           AHR_HOLD_DIST (1500, true radial) and then hangs there; the moment
 *           the player is beyond 1500 again it follows. Routed round the room's
 *           solid blocks (Lava U's rock island) through a few authored
 *           waypoints - see the .c.
 *
 *           In range and IN VIEW - the body's centre projects inside the
 *           screen AND nothing solid stands between it and the camera - a
 *           two-second countdown runs (AHR_GAZE_FRAMES). Out of view resets
 *           it to zero. Out of range only PAUSES it: nothing happens out of
 *           range, but backing off a step is not a way to wipe it.
 *
 *   PRIMED  The countdown expires: the eye flashes, the screen goes white
 *           (ahrimans_draw_flash), and THE PLAYER IS FROZEN - turn, look, aim
 *           and attack, but no walking or strafing (camera.h's player_rooted,
 *           which main.c ORs ahrimans_hold_player() into, and the stamina bar
 *           turns grey: src/hud.c). It hangs for AHR_PRIME_FRAMES (1 s).
 *
 *   CHARGE  Then it comes in at 1.5x its pursuit speed and bites for
 *           AHR_BITE_DAMAGE (20) every AHR_BITE_COOLDOWN (1 s) while within
 *           AHR_BITE_REACH. Looking away no longer matters, and nothing ends it
 *           but its death or the player's. A crucifaxe hit knocks it back;
 *           it comes straight back in.
 *
 * The freeze lasts while ANY Ahriman in the room is PRIMED or CHARGING, so it
 * ends when the one that froze you dies (and two that both caught you both
 * have to die). Leaving the room - possible, if the freeze caught the player
 * at a door - rests them all (ahrimans_rest) and the freeze with them.
 *
 * DAMAGE. 3 HP, held as HALF-POINTS (AHR_HP_SCALE) because FLAME ROUNDS DO
 * HALF: damage_scale() never rounds a hit below 1, so a halved 1 would still be
 * 1 (damage.h). In half-points a swing, a Standard or Scattershot round and a
 * second of the Helluminator are 2 each, a Flame Round 1 - three hits, or six
 * Flame Rounds. ahriman_scale_damage() is the conversion; every weapon passes
 * its plain 1 through it and nothing else knows about half-points.
 *
 * TEXTURES. ONE 256x256 4bpp sheet (textures/underworld/ahriman.png), 2x2,
 * four frames of wing-beat that loop for as long as it lives, whatever it is
 * doing. Shipped as two 256x128 halves on the CRAWLER's blocks (x320 / x704,
 * y128), CLUTs (144,480)/(160,480): tools/split_ahriman_sheet.py, disc.xml.
 * Voff 128, so every sprite brackets its own texture window.
 *
 * >>> ONLY LAVA U UPLOADS THE SHEET TODAY. <<< Placing one in another room
 * means: the placement in world.c (with its height), the room in
 * ahriman_room_has_sheet() (the .c) - which main.c and tools/gen_tex_rooms.py
 * key the upload on - update_ahrimans() / draw_ahrimans() /
 * ahrimans_set_texwindow() in that room, and that room's waypoints in
 * ahr_routes[] if it has a solid block to go round. And the room must not draw
 * anything else at x320 or x704 y128.
 *
 * PERSISTENCE: the nessies' model. ONE global array tagged by area, every loop
 * gated on current_area (NEVER game_state - the inventory menu must not pause
 * the freeze), deaths keyed by canonical_index() in WorldDelta.ahrimans_dead.
 * MAX_AHRIMANS is the budget for the WHOLE GAME, and ahriman_add drops SILENTLY
 * once it is full.
 *
 * SOUND. One of its own, UNDERWORLD-banked (sound.h):
 *   SFX_AHR_FLASH  the eye going off, once, with the white flash (voice 20).
 * No wing loop for now: a wingflap clip was tried and dropped, and a replacement
 * is still to be chosen. sound_play_vol / sound_set_volume (sound.h) are the
 * pieces a distance-levelled loop would be built from.
 * The rest are borrowed stand-ins: SFX_AXEHIT for a hit, SFX_HURT for its bite,
 * and its death is the tentacle's cry (SFX_TNTCL_DIE, which sound.c gives an
 * UNDERWORLD copy for this).
 * ----------------------------------------------------------------------- */

#define MAX_AHRIMANS           8    /* WHOLE-GAME budget. Three placed: Lava U */

/* ---- Health, in half-points - see above. */
#define AHR_MAX_HEALTH         3
#define AHR_HP_SCALE           2

/* ---- Heights. The body centre is anchor + offset (negative is up). */
typedef enum {
    AHR_HEIGHT_A = 0,   /* just below the eye line */
    AHR_HEIGHT_B,       /* the eye line            */
    AHR_HEIGHT_C,       /* just above the eye line */
} AhrimanHeight;
#define AHR_EYE_RISE          40    /* eye above an enemy anchor, same floor   */
#define AHR_HEIGHT_STEP       60    /* A and C either side of the eye line     */

/* ---- Movement. 6 a frame is half the player's walk (12); the charge is 1.5x
   that. Positions are kept at 1/256 of a unit (Nessie's NSS_SPEED_Q8 reason:
   an integer step of 6 truncates its smaller component away on most
   bearings, tools/ADDING_AN_ENEMY.txt mistake 16d). */
#define AHR_SPEED_Q8        (6 * 256)
#define AHR_CHARGE_Q8       (AHR_SPEED_Q8 * 3 / 2)        /* 9 a frame        */

/* It hangs at this true radial distance and pursues past it. */
#define AHR_HOLD_DIST       1500

/* ---- The attack, in frames at 60 fps. */
#define AHR_GAZE_FRAMES      120    /* 2 s in view before the flash            */
#define AHR_PRIME_FRAMES      60    /* 1 s frozen before it moves in           */
#define AHR_BITE_DAMAGE       20
#define AHR_BITE_COOLDOWN     60    /* 1 s between bites                       */

/* The white flash: full white for AHR_FLASH_HOLD frames, then a fade out. */
#define AHR_FLASH_HOLD         6
#define AHR_FLASH_FADE        36

/* ---- Sprite. A 240x240 billboard: the art fills about 120 of each 128-texel
   frame across and 100 down, so the body reads ~225 wide wing to wing and
   ~190 tall. One quad: a 240 span is dropped by the GPU only inside a view
   depth of 240*256/1023 = 60, far inside the push-out. */
#define AHR_HALF_W           120
#define AHR_HALF_H           120
/* The hit shape for the gun and the lantern: the body and the inner wings,
   not the transparent corners of the quad. */
#define AHR_HIT_HALF_W       105
#define AHR_HIT_HALF_H        90
#define AHR_ANIM_RATE          8    /* frames per wing-beat frame: 0.53 s loop */

/* ---- Body. AHR_BODY_RADIUS is how far the WALLS hold it (and two of them hold
   each other); AHR_PUSH_RADIUS is how far it holds the PLAYER, in
   apply_collision_reception before the walls (nessies_collide's terms), so a
   player walking into a hanging one cannot pass through the billboard. */
#define AHR_BODY_RADIUS      100
#define AHR_PUSH_RADIUS       40
#define AHR_WALL_LIKE_PUSH   195    /* what apply_collision_reception passes in */
#define AHR_HOLD_PLAYER     (AHR_PUSH_RADIUS + AHR_WALL_LIKE_PUSH)   /* 235      */
#define AHR_PUSH_STEP         48

/* ---- The charge stops this far off the player's centre and bites inside
   AHR_BITE_REACH. >>> THE STOP MUST BE OUTSIDE THE PUSH-OUT, <<< or the push
   would shove a FROZEN player backwards every frame of the charge. */
#define AHR_STOP_DIST        300
#define AHR_BITE_REACH       360
_Static_assert(AHR_STOP_DIST > AHR_HOLD_PLAYER,
               "the charge would push the frozen player along the floor");
_Static_assert(AHR_BITE_REACH > AHR_STOP_DIST,
               "a charging Ahriman would stop short of its own bite");

/* ---- Crucifaxe: reach measured RADIALLY to the body's surface, plus the
   vertical gap (heights A and C sit 60 off the eye). At the charge's stop
   distance: 300 - 120 + 60 = 240 < 350. */
#define AHR_AXE_SURFACE      AHR_HALF_W
_Static_assert(AHR_STOP_DIST - AHR_AXE_SURFACE + AHR_HEIGHT_STEP < SWING_RANGE,
               "a charging Ahriman would sit outside the crucifaxe's reach");
/* The shove: 30 a frame, decaying by 3 - about 165 units back. */
#define AHR_KNOCKBACK         30
#define AHR_KB_DECAY           3

#define AHR_BAR_TIMER_MAX    120

typedef enum {
    AHR_HUNT,      /* floating at the player / hanging at 1500; the gaze      */
    AHR_PRIMED,    /* the flash has gone off; frozen player, 1 s wait         */
    AHR_CHARGE,    /* coming in and biting - until one of them dies           */
    AHR_DEAD,
} AhrimanState;

typedef struct {
    int32_t       x, y, z;        /* y is the FLOOR anchor - see the head      */
    int32_t       qx, qz;         /* position at 1/256; x/z are these >> 8     */
    int32_t       spawn_x, spawn_z, spawn_y;
    int32_t       vy;
    int32_t       kb_vx, kb_vz;   /* crucifaxe shove, decays AHR_KB_DECAY      */
    int32_t       goal_x, goal_z; /* where the route says to go this stretch   */

    int           health;         /* half-points: AHR_MAX_HEALTH*AHR_HP_SCALE  */
    int           hit_timer;
    int           anim_tick;      /* always ticking while alive                */
    int           gaze;           /* HUNT: frames in view, toward the flash    */
    int           timer;          /* PRIMED: wait; CHARGE: bite cooldown       */
    int           route_tick;     /* frames until the route is re-solved       */

    AhrimanHeight height;
    AhrimanState  state;
    int32_t       active;
    int           on_upper_floor;
    int           on_ramp;
    GameState     area;
} Ahriman;

extern Ahriman ahrimans[MAX_AHRIMANS];
extern int     ahriman_count;

/* Register the two sheet halves with texmgr (TEXBANK_UNDERWORLD). Call ONCE at
   startup, beside the other enemies'. */
void ahrimans_load_textures(void);
/* Stream both halves into VRAM. main.c calls it from STATE_LOADING for every
   area ahriman_room_has_sheet() names. */
void ahrimans_upload_textures(void);
int  ahriman_room_has_sheet(GameState area);

/* Place one at (x,z) at height A, B or C. Authored - nothing here reads the
   room's mesh - so world.c can rebuild a placement from a save. Returns its
   index, or -1 if the global pool is full. */
int  ahriman_add(int32_t x, int32_t z, AhrimanHeight height, GameState area);

void ahrimans_init(void);
void ahrimans_reset(void);
/* Every living Ahriman back at its spawn, hunting, at full health (deaths
   stick), and the flash cleared. Called when leaving a room and when saving. */
void ahrimans_rest(void);

void update_ahrimans(void);
void draw_ahrimans(RenderContext *ctx);
/* The white flash, full-screen, over the scene. From draw_player_systems. */
void ahrimans_draw_flash(RenderContext *ctx);

/* 1 while an Ahriman in `area` has the player frozen (PRIMED or CHARGE). main.c
   ORs it into player_rooted; the HUD greys the stamina bar on it. */
int  ahrimans_hold_player(GameState area);

/* Damage in HALF-POINTS - pass every weapon's base through
   ahriman_scale_damage() first. */
void ahriman_damage(Ahriman *s, int dmg);
int32_t ahriman_scale_damage(int32_t base, DamageType type);
/* The crucifaxe test, with the knockback. 1 if it landed on one. */
int  ahrimans_try_hit(void);
/* The player's push-out; called from apply_collision_reception. */
void ahrimans_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* Where the body is: its centre Y (anchor + the height's lift). */
int32_t ahriman_body_y(const Ahriman *s);
/* Grave-olver / Helluminator support: body-centre Y, half-height, half-width. */
void ahriman_body(const Ahriman *s, int32_t *cyc, int32_t *hh, int32_t *hw);

/* The area's texture window, restored after each sprite (the sheet is at Voff
   128). NULL for an area with none. */
void ahrimans_set_texwindow(const RECT *tw);

#endif
