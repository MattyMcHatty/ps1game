#ifndef HALL_2F_H
#define HALL_2F_H

#include "render.h"

/* Second-floor Hall, reached by ascending the conservatory stairs. An L-shaped
   corridor (a long east-west run along the north) opening west into a larger
   room, with an open stairwell descending back down to the conservatory.
   A single flat y=0 floor (the descending stairwell is scenery only, like the
   conservatory's upstairs). Modelled on conservatory.c. */
void hall_2f_load_assets(void);      /* startup: geometry + texture registration */
void hall_2f_upload_textures(void);  /* room entry: pure LoadImage from RAM (no CD) */

/* Narrow uploads of this module's strs / upstairs copies, for rooms that want
   one without the rest of the hall's set (see the note in hall_2f.c). */
void hall_2f_upload_strs(void);
void hall_2f_upload_upstairs(void);
void hall_2f_init(void);             /* set collision/floor zones + spawn */
void hall_2f_draw(RenderContext *ctx);
void hall_2f_stairs_arm(void);       /* seed Circle edge state on entry */
int  hall_2f_stairs_triggered(void); /* 1 when Circle pressed near the down-stairs */
void hall_2f_edoor_arm(void);        /* same pair for the far-east door out to reception */
int  hall_2f_edoor_triggered(void);  /* 1 when Circle pressed near the east door */

/* The two doors in the corridor's south wall (z=-656) leading into the Master
   Bedroom: "east" at x=-400 and "west" at x=-1889. They map to the bedroom's
   east-wing and west-wing north-wall doors respectively. */
void hall_2f_bdoors_arm(void);         /* seed both doors' Circle edge state */
int  hall_2f_bdoor_e_triggered(void);  /* 1 when Circle pressed near the east one */
int  hall_2f_bdoor_w_triggered(void);  /* 1 when Circle pressed near the west one */

/* Spawn the player back in the corridor, just outside one of those doors. */
void hall_2f_spawn_bdoor_e(void);
void hall_2f_spawn_bdoor_w(void);

/* Shared lock state for the Hall 2F <-> Reception door. Starts locked; the
   Hall 2F side unlocks it (Circle) and both sides read it: the Reception side
   stays "Locked from the other side" until this flips to 1. Persists across
   room transitions (a plain global); reset to 0 on a new game. */
extern int hall_2f_door_unlocked;

#endif
