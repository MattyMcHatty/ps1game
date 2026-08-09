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
   Driven by look mode (below) and by scripted fixed-camera shots like the stove
   puzzle's top-down framing; 0 the rest of the time, where the pitch matrix is
   the identity. Only view matrices built via camera_build_view() honour it, so
   every room/prop draw builds its view through that call. */
extern int32_t cam_pitch;

/* ---- Look mode ------------------------------------------------------------
   HOLD Circle to look around from a standstill: the d-pad swings the view up to
   LOOK_MAX_ANG off the player's facing in each axis instead of walking/turning.
   A TAP of the same button interacts instead — see interact_tapped() below,
   which every door, prompt, save point and puzzle entry uses in place of
   reading Circle off the pad.
   Only the camera is affected — the world keeps ticking exactly as it does in
   roaming mode. Releasing Circle eases the view back to the player's facing.
   The offset is folded into cam_rot/cam_pitch, so every draw (rooms, props,
   billboards) follows it without knowing look mode exists; update_camera strips
   it again at the top of the next frame, so cam_rot outside this window is
   still the true facing. */
#define LOOK_MAX_ANG 228   /* 20 deg, in 4096-per-turn units */
extern int look_mode;      /* 1 while Circle is held and the view is unlocked */

/* Drop any look offset immediately, putting cam_rot back to the player's real
   facing. update_camera does this for itself every frame; call it from anything
   that takes the camera or freezes the game on a frame where Circle may still
   be down (puzzles, the save flow, room transitions), so nothing snapshots or
   saves a swung-off-centre cam_rot. */
void camera_look_cancel(void);

/* 1 for the single frame a Circle TAP resolves — i.e. on release, if the press
   was too short to have become a look. World interactions (doors, stairs, save
   points, puzzle entries, prompts) read this instead of the pad, so they fire
   on release and a hold-to-look never triggers them. Modules that already
   edge-detect their own `held` flag can keep that logic: this reads as a
   one-frame press. In-puzzle menus still read Circle straight off the pad —
   look mode is off while a puzzle owns the camera, so nothing is shared. */
int interact_tapped(void);

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
