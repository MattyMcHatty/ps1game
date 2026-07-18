#ifndef ZOMBIE_H
#define ZOMBIE_H

#include <stdint.h>
#include "render.h"

#define MAX_ZOMBIES           8
#define ZMB_MAX_HEALTH        5
#define ZMB_SPEED             8    /* 5 + ~half (1.5x) */
#define ZMB_WAKE_RADIUS     900    /* Manhattan; reaches the player at the
                                      adjacent fat door, but no further */
#define ZMB_CATCH_DIST      180
#define ZMB_DAMAGE_AMOUNT    15
#define ZMB_DAMAGE_COOLDOWN  60
#define ZMB_DOOR_COOLDOWN    90    /* frames between hits on a fat door (~1.5s) */
#define ZMB_DOOR_CLEARANCE  100    /* movement-collision radius vs fat doors; bigger
                                      than the body radius so the sprite is held far
                                      enough back not to clip through the thin door */
#define ZMB_DOOR_CLEAR_DIST  80    /* how close to a doorway clearance point counts as
                                      "stepped clear of the opening" */
#define ZMB_HALF_W           62    /* sprite half width (world units), ~1/4 wider */
#define ZMB_HALF_H          125    /* half height (half the earlier height bump) */
#define ZMB_Y_OFFSET         25    /* feet stay planted: y_offset + half_h = 150 */
#define ZMB_KNOCKBACK        20
/* Bite "lunge": on a damage hit the sprite snaps to a fixed spot in front of the
   camera and STAYS there (a bite in the face) until knocked back or the player
   retreats out of catch range. Purely visual. Rather than scaling the whole
   full-body sprite up and dropping it (a giant poly the GPU won't draw), the UVs
   are CROPPED to the top FACE_ROWS texels of the sprite (the head + shoulders),
   drawn at LUNGE_HALF_W/H. SIDE shifts along the view's right axis (+ = right);
   DROP nudges it down in world Y (+ = down). Make FACE_ROWS smaller to show less
   of the body, HALF_W/H bigger to fill more of the screen. (Mirrors the demon
   dog, which uses the position approach since its sprite is short enough.) */
#define ZMB_LUNGE_DIST      100
#define ZMB_LUNGE_HALF_W    120    /* on-screen half width of the cropped face  */
#define ZMB_LUNGE_HALF_H     95    /* on-screen half height of the cropped face */
#define ZMB_LUNGE_FACE_ROWS  30    /* texels from the sprite top to show (of 128) */
#define ZMB_LUNGE_SIDE        0    /* + = to the right of the view centre        */
#define ZMB_LUNGE_DROP        0    /* + = downward nudge                          */
#define ZMB_LUNGE_FLIP        0    /* fixed facing during a bite (0/1)            */
#define ZMB_BAR_TIMER_MAX   120
#define ZMB_GROAN_INTERVAL  400    /* frames between repeated groans while alert; set
                                      just under the ~7s clip length so it re-triggers
                                      before the end with no silent gap (continuous) */

/* --- Steering / flocking tuning (mirrors the demon dog) --- */
#define ZMB_SEP_RADIUS      150    /* zombies try to stay this far apart (soft push) */
#define ZMB_SEP_WEIGHT        2    /* separation strength vs. pursuit */
#define ZMB_BODY_RADIUS      60    /* hard collision radius (~half sprite width) */
#define ZMB_FEELER_LEN      150    /* obstacle look-ahead distance */
#define ZMB_TURN_RATE         3    /* turn smoothing: 1=sluggish .. 8=instant snap */
#define ZMB_STEER_COMMIT     30    /* frames to commit to a wall-follow side once blocked */

#define ZMB_SHADOW_W         70
#define ZMB_SHADOW_D         30
#define ZMB_SHADOW_R          0
#define ZMB_SHADOW_G          0
#define ZMB_SHADOW_B          0

typedef enum {
    ZMB_DORMANT,
    ZMB_ALERT,
    ZMB_DEAD,
} ZombieState;

typedef struct {
    int32_t     x, y, z;
    int32_t     spawn_x, spawn_y, spawn_z;   /* where zombie_add placed it; used
                                                by zombies_rest() */
    int32_t     vy;
    int32_t     kb_vx, kb_vz;
    int         health;
    int         damage_timer;
    int         door_timer;     /* cooldown between battering a fat door */
    int         hit_timer;
    int         lunging;        /* 1 = latched into the player's face after a bite */
    ZombieState state;
    int32_t     active;
    int         on_upper_floor;
    int         on_ramp;
    int         anim_tick;
    int         groan_timer;
    int32_t     facing;         /* last move dir, packed: hi16 = X, lo16 = Z */
    int         steer_timer;    /* frames left committed to a wall-follow side */
    int         steer_dir;      /* committed side: -1 = left, +1 = right */
    int         nav_clear;      /* doorway node whose far side we must step clear
                                   of before chasing again (-1 = none) */
} Zombie;

extern Zombie zombies[MAX_ZOMBIES];
extern int    zombie_count;

/* Load the zombie sprite TIMs into VRAM. Call ONCE at startup (LoadImage is
   only safe before the main render loop begins). */
void zombies_load_textures(void);

/* Place a zombie at a world position into the live array. Returns its index,
   or -1 if full. The world system (world.c) seeds each room's zombies and
   persists them across area transitions. */
int  zombie_add(int32_t x, int32_t y, int32_t z);

void zombies_init(void);
void zombies_reset(void);
/* Put every still-living zombie back at its spawn point, asleep, at full
   health (deaths stick). Called when leaving a room and when saving. */
void zombies_rest(void);
void update_zombies(void);
void draw_zombies(RenderContext *ctx);

/* Tell the zombie renderer which texture window the current area has active, so
   each sprite can be drawn unmasked and then the area's window restored. Pass
   NULL for areas that use no texture window. */
void zombies_set_texwindow(const RECT *tw);

#endif
