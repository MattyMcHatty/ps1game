#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>
#include <psxgte.h>

extern int32_t cam_x;
extern int32_t cam_y;
extern int32_t cam_vy;
extern int32_t cam_z;
extern int32_t cam_rot;

/* Camera pitch (X rotation, 4096 = 360deg; positive tilts the view DOWN).
   Free-look gameplay never sets it — it stays 0 and the pitch matrix is the
   identity — it exists for scripted fixed-camera shots like the stove puzzle's
   top-down framing. Only view matrices built via camera_build_view() honour it,
   which is every draw in the kitchen's path; rooms that still build their own
   yaw-only matrix are unaffected (their pitch is always 0 anyway). */
extern int32_t cam_pitch;

/* ---- Player anchor --------------------------------------------------------
   The player's world position is normally just the camera's. A camera-locked
   puzzle breaks that: the camera flies off to a fixed shot while the PLAYER
   stays standing where they were, so enemies that chased cam_* would lose
   track of them (and their attacks would land on the empty camera spot).
   A puzzle anchors the player on entry and releases it on exit; everything
   that means "where the player is standing" reads player_x/y/z() instead. */
void    camera_anchor_player(int32_t x, int32_t y, int32_t z);
void    camera_release_player(void);
int32_t player_x(void);
int32_t player_y(void);
int32_t player_z(void);

/* Horizontal knockback velocity applied to the player (e.g. a tentacle hit).
   update_camera displaces the camera by this each frame and decays it; the
   area's apply_collision then resolves the push against walls. Set via
   player_knockback(). */
extern int32_t cam_kb_vx, cam_kb_vz;
void player_knockback(int32_t from_x, int32_t from_z, int32_t speed);

#define SPRINT_STAMINA_MAX  120   /* 2 s at 60 fps */
#define SPRINT_COOLDOWN_MAX 240   /* 4 s at 60 fps */
extern int sprint_stamina;        /* remaining sprint frames; 0 = exhausted      */
extern int sprint_cooldown;       /* 1 = sprint locked until bar fully refills   */

extern int     aiming;            /* 1 while L2 aiming (player movement frozen)   */
extern int32_t aim_x, aim_y;      /* crosshair screen position (moves while aiming) */

void update_camera(void);
/* Build the camera view matrix (yaw + pitch + translation) without touching the
   GTE, for callers that need to compose it with a prop's own matrix. */
void camera_build_view(MATRIX *out);
void camera_set_view_matrix(void);   /* load GTE with the camera view (for debug overlays) */
void apply_collision(void);
void apply_vampire_collision(void);

#endif
