#ifndef DEMONDOG_H
#define DEMONDOG_H

#include <stdint.h>
#include "render.h"

#define MAX_DEMON_DOGS        8
#define DDOG_MAX_HEALTH       3
#define DDOG_SPEED           10
#define DDOG_WAKE_RADIUS      2400
#define DDOG_CATCH_DIST       180
#define DDOG_DAMAGE_AMOUNT    10
#define DDOG_DAMAGE_COOLDOWN  60
#define DDOG_HALF_W           60
#define DDOG_HALF_H           50
#define DDOG_Y_OFFSET        100
#define DDOG_KNOCKBACK        40
/* Bite "lunge": on a damage hit the sprite snaps to a fixed spot in front of the
   camera and STAYS there (a bite in the face) until the dog is knocked back or the
   player retreats out of catch range. Purely visual — position/collision are
   unaffected. Size is controlled by LUNGE_HALF_W/H (not by getting closer, which
   would clip), so make them BIGGER here rather than reducing DIST. Framing is
   controlled relative to the camera: SIDE shifts along the view's right axis
   (+ = right), DROP shifts down in world Y (+ = down) so the body falls below the
   frame and only the head/upper body shows. Keep DROP+HALF_H modest so the sprite
   doesn't project past the ~1023 vertex-coord limit. */
#define DDOG_LUNGE_DIST      100    /* depth in front of camera (safe, no clip)   */
#define DDOG_LUNGE_HALF_W     85    /* on-screen half width  (normal is 60)       */
#define DDOG_LUNGE_HALF_H     70    /* on-screen half height (normal is 50)       */
#define DDOG_LUNGE_SIDE       50    /* + = to the right of the view centre         */
#define DDOG_LUNGE_DROP       45    /* + = downward (drop the body below frame)    */
#define DDOG_LUNGE_FLIP        0    /* fixed facing during a bite (0/1); 0 = face  */
#define DDOG_BAR_TIMER_MAX    120
#define DDOG_BARK_INTERVAL    100   /* frames between repeated barks while alert (~1.7s) */

/* --- Steering / flocking tuning --- */
#define DDOG_SEP_RADIUS      180    /* dogs try to stay this far apart (soft push) */
#define DDOG_SEP_WEIGHT        2    /* separation strength vs. pursuit */
#define DDOG_BODY_RADIUS     120    /* hard collision radius (~half sprite width) */
#define DDOG_FEELER_LEN      150    /* obstacle look-ahead distance */
#define DDOG_TURN_RATE         3    /* turn smoothing: 1=sluggish .. 8=instant snap */
#define DDOG_STEER_COMMIT     30    /* frames to commit to a wall-follow side once blocked */

#define DDOG_SHADOW_W          70
#define DDOG_SHADOW_D          30
#define DDOG_SHADOW_R           0
#define DDOG_SHADOW_G           0
#define DDOG_SHADOW_B           0

typedef enum {
    DDOG_DORMANT,
    DDOG_ALERT,
    DDOG_DEAD,
} DDogState;

typedef struct {
    int32_t   x, y, z;
    int32_t   vy;
    int32_t   kb_vx, kb_vz;
    int       health;
    int       damage_timer;
    int       hit_timer;
    int       lunging;        /* 1 = latched into the player's face after a bite */
    DDogState state;
    int32_t   active;
    int       on_upper_floor;
    int       on_ramp;
    int       anim_tick;
    int       bark_timer;
    int32_t   facing;         /* last move dir, packed: hi16 = X, lo16 = Z */
    int       steer_timer;    /* frames left committed to a wall-follow side */
    int       steer_dir;      /* committed side: -1 = left, +1 = right */
} DemonDog;

extern DemonDog demon_dogs[MAX_DEMON_DOGS];
extern int      demon_dog_count;

void demon_dogs_init(void);
void demon_dogs_reset(void);
/* Put every still-living dog back at its spawn point, asleep, at full health
   (deaths stick). Called when leaving a room and when saving. */
void demon_dogs_rest(void);
void update_demon_dogs(void);
void draw_demon_dogs(RenderContext *ctx);

#endif
