#ifndef SPIDER_H
#define SPIDER_H

#include <stdint.h>
#include "render.h"
#include "title.h"   /* GameState — each spider is tagged with its area */

/* -----------------------------------------------------------------------
 * Spider — a reusable ceiling ambusher.
 *
 * Built the same way as the zombie (see zombie.h/.c, the reference enemy):
 * a camera-facing textured quad, a DORMANT -> ALERT wake, feeler-based steering
 * while chasing, and contact damage on a cooldown. What it adds on top:
 *
 *   - It starts stuck to the CEILING, upside down, on the spdr_rst texture.
 *     The ceiling height is not authored per placement: spider_add reads it out
 *     of the room's mesh-generated collision data (collision_ceiling_y).
 *   - It wakes when the player comes within SPD_WAKE_RADIUS of its XZ position
 *     (a true radial distance, not the Manhattan approximation the zombie's
 *     much larger wake radius uses) or when anything damages it.
 *   - Waking drops it fast, rotating from upside down to upright on the way,
 *     and it chases on the floor once it lands.
 *   - Once landed it is a KITER, not a straight chaser. Three concentric bands
 *     around the player decide what it does (see SPD_ATTACK_RADIUS below).
 *
 * The sprite is a 3-wide by 2-tall quad (SPD_HALF_W/SPD_HALF_H), roughly the
 * same scale as the zombie's 1x2 body.
 *
 * Unlike the zombie's per-room array, spiders are ONE global array tagged by
 * area (update/draw/hit skip any spider whose area != game_state), the same way
 * the fat doors and tentacles work. That keeps them out of world.c's per-room
 * RoomState: a copy of the array in all eight rooms would have eaten almost all
 * the memory card blob's remaining headroom (see the _Static_assert in world.c)
 * to store seven empty copies.
 * ----------------------------------------------------------------------- */

#define MAX_SPIDERS           4
#define SPD_MAX_HEALTH        6    /* six crucifaxe swings or six rounds       */
#define SPD_SPEED             8    /* same ground speed as a zombie            */
#define SPD_WAKE_RADIUS     600    /* true radial XZ distance to the player    */
#define SPD_CATCH_DIST      180
#define SPD_DAMAGE_AMOUNT    15    /* same bite as ZMB_DAMAGE_AMOUNT           */
#define SPD_DAMAGE_COOLDOWN  60
#define SPD_BAR_TIMER_MAX   120

/* --- Ranged behaviour: three concentric bands around the player ---
   A landed spider measures its TRUE radial XZ distance to the player (not the
   Manhattan approximation used for the contact bite, which is a reach test and
   wants to match the sprite's own footprint) and picks one of three modes:

     d <= SPD_ATTACK_RADIUS   close in and bite, exactly as it always has
     d <= SPD_SPIT_RADIUS     BACK OFF at half speed
     d >  SPD_SPIT_RADIUS     close the gap at full speed until inside it

   MOVEMENT and FIRING are separate decisions. The bands above choose only where
   the spider walks; it spits at ANY range beyond SPD_ATTACK_RADIUS, so the
   outermost band closes the gap and webs the player on the way in rather than
   running at them silently. Only the melee band holds fire.

   The effect is that it settles at arm's length around SPD_SPIT_RADIUS and
   pelts the player from there, and only commits to melee once the player has
   closed to SPD_ATTACK_RADIUS. Backing off is deliberately HALF speed so the
   player can always win the chase and force the melee band. */
#define SPD_ATTACK_RADIUS   500    /* inside this it closes in and bites, and
                                      holds fire; outside it, it always spits  */
#define SPD_SPIT_RADIUS     800    /* stand-off distance it retreats to        */
#define SPD_RETREAT_SPEED     4    /* half of SPD_SPEED, backing away          */
#define SPD_SPIT_INTERVAL   180    /* frames between webs                      */

/* Sprite: 3 wide by 2 tall. Half-height is set so the feet land where a
   zombie's do — SPD_Y_OFFSET + SPD_HALF_H == 150, the zombie's 25 + 125. */
#define SPD_HALF_W           93
#define SPD_HALF_H           62
#define SPD_Y_OFFSET         88

/* The drop. Seeding vy at MAX_FALL_VEL makes apply_ddog_height fall at its
   terminal rate from frame one (a standing start takes ~20 frames to get
   there), so a ~375-unit ceiling is cleared in well under a second.

   SPD_TURN_FRAMES must stay comfortably UNDER that fall time or the spider is
   still rolling when it lands. Hung under the East Hall's 520-high ceiling it
   falls about 395 units to the floor, i.e. ~20 frames at SPD_DROP_VEL, so 10
   leaves plenty of margin. A taller room simply finishes the roll earlier in
   the fall, which still looks right; a ceiling less than ~200 above the floor
   would land it mid-turn, but a spider needs that much headroom anyway. */
#define SPD_DROP_VEL         20
#define SPD_TURN_FRAMES      10
#define SPD_ANGLE_HUNG     2048    /* 180 degrees in PS1 angle units (4096 = 360) */

/* Frames per texture swap while active: the spdr_rst/spdr_wk oscillation that
   fakes a scuttle, the spider's version of the zombie's walk-cycle flip. */
#define SPD_ANIM_RATE        10

/* Steering / flocking, mirroring the zombie's. No doorway nav graph: a spider
   ambushes and fights within the room it drops into, so the feelers and the
   wall-follow commit are all it needs to get round furniture and corners. */
#define SPD_SEP_RADIUS      150
#define SPD_SEP_WEIGHT        2
#define SPD_BODY_RADIUS      60
#define SPD_DOOR_CLEARANCE  100    /* fat-door clearance, as ZMB_DOOR_CLEARANCE */
#define SPD_FEELER_LEN      150
#define SPD_TURN_RATE         3
#define SPD_STEER_COMMIT     30
#define SPD_KNOCKBACK        20

#define SPD_SHADOW_W        100
#define SPD_SHADOW_D         40
#define SPD_SHADOW_R          0
#define SPD_SHADOW_G          0
#define SPD_SHADOW_B          0

typedef enum {
    SPD_CEILING,    /* hung upside down, at rest, on the spdr_rst texture */
    SPD_DROPPING,   /* falling and turning upright                        */
    SPD_ALERT,      /* on the floor, chasing                              */
    SPD_DEAD,
} SpiderState;

typedef struct {
    int32_t     x, y, z;
    int32_t     spawn_x, spawn_y, spawn_z;   /* ceiling perch; spiders_rest()
                                                hangs them back up here */
    int32_t     vy;
    int32_t     kb_vx, kb_vz;
    int         health;
    int         damage_timer;
    int         hit_timer;
    SpiderState state;
    int32_t     active;
    int         on_upper_floor;
    int         on_ramp;
    int         anim_tick;
    int         drop_tick;      /* frames since the drop began (drives the turn) */
    int32_t     facing;         /* last move dir, packed: hi16 = X, lo16 = Z */
    int         steer_timer;
    int         steer_dir;
    int         spit_timer;     /* frames until the next web (spit band only) */
    GameState   area;
} Spider;

extern Spider spiders[MAX_SPIDERS];
extern int    spider_count;

/* Load the spider sprite TIMs into VRAM. Call ONCE at startup (LoadImage is
   only safe before the main render loop begins). */
void spiders_load_textures(void);

/* Hang a spider from the ceiling above (x,z) in `area`. The Y comes from the
   CURRENT room's collision mesh, so this must be called while that room is
   loaded: placements live in world_enter(), which runs after each area's
   *_init(). Returns its index, or -1 if full. */
int  spider_add(int32_t x, int32_t z, GameState area);

void spiders_init(void);
void spiders_reset(void);
/* Put every still-living spider back on its ceiling perch, at rest, at full
   health (deaths stick). Called when leaving a room and when saving. */
void spiders_rest(void);
/* Stop the shared scuttle loop and clear its latch (room transitions). */
void spiders_silence(void);
void update_spiders(void);
void draw_spiders(RenderContext *ctx);

/* Deal damage from any source. Wakes a resting spider — dropping it off the
   ceiling — flashes its health bar, and handles death. Both the crucifaxe and
   a Grave-olver round pass 1. */
void spider_damage(Spider *s, int dmg);

/* Tell the spider renderer which texture window the current area has active, so
   each sprite can be drawn unmasked and then the area's window restored. Pass
   NULL for areas that use no texture window. */
void spiders_set_texwindow(const RECT *tw);

#endif
