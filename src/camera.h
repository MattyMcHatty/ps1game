#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>

extern int32_t cam_x;
extern int32_t cam_y;
extern int32_t cam_vy;
extern int32_t cam_z;
extern int32_t cam_rot;

#define SPRINT_STAMINA_MAX  120   /* 2 s at 60 fps */
#define SPRINT_COOLDOWN_MAX 240   /* 4 s at 60 fps */
extern int sprint_stamina;        /* remaining sprint frames; 0 = exhausted      */
extern int sprint_cooldown;       /* 1 = sprint locked until bar fully refills   */

extern int     aiming;            /* 1 while L2 aiming (player movement frozen)   */
extern int32_t aim_x, aim_y;      /* crosshair screen position (moves while aiming) */

void update_camera(void);
void camera_set_view_matrix(void);   /* load GTE with the camera view (for debug overlays) */
void apply_collision(void);
void apply_vampire_collision(void);

#endif
