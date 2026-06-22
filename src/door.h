#ifndef DOOR_H
#define DOOR_H

#include <stdint.h>
#include "render.h"

typedef enum {
    DOOR_LOCKED,
    DOOR_UNLOCKED,
    DOOR_OPEN,
} DoorState;

extern DoorState door_state;

/*
 * World-space coordinates of the door.
 * Enable debug mode (Select) and walk to the door to read cam_x/cam_z
 * from the on-screen display, then update these to match exactly.
 * Upper floor is at world Y = -544 (floor surface).
 */
#define DOOR_X             (-5264)
#define DOOR_Y             (-693)   /* eye level where door is accessible */
#define DOOR_Z              4063
#define DOOR_TEXT_RADIUS    1500    /* distance at which text becomes visible */
#define DOOR_TRIGGER_RADIUS 500     /* distance at which player can interact */
#define DOOR_Y_TOLERANCE    250     /* tight vertical tolerance */

/* Sign floats just south of the door, centred on it */
#define SIGN_X    DOOR_X
#define SIGN_Y    (-730)    /* text top — slightly above eye level (cam_y=-693) */
#define SIGN_Z    (DOOR_Z - 200)

/* World units per font pixel — reduce if text is too wide on screen */
#define PIXEL_SIZE  8

/* Default world units per font pixel for door_draw_string_3d signs. */
#define DOOR_PIXEL_SIZE 4

void door_init(void);
void door_update(void);
void door_draw(RenderContext *ctx);
void door_arm(void);   /* seed Circle edge state on (re)entering the delivery area */

/* Plane the text lies in (its reading direction runs along the first axis,
   the fixed wall coordinate is the second):
     TEXT_PLANE_YZ — reading along Z, fixed X (door signs facing +/-X).
     TEXT_PLANE_XY — reading along X, fixed Z (90deg-rotated signs facing +/-Z). */
typedef enum {
    TEXT_PLANE_YZ = 0,
    TEXT_PLANE_XY = 1,
} TextPlane;

/* Reusable world-space pixel-font text on a wall.
   mirror=1 flips it horizontally (reverses reading order) for viewing from the
   opposite side; combined with the plane this gives 90/180deg orientations.
   pixel = world units per font pixel (use DOOR_PIXEL_SIZE for the default size). */
void door_draw_string_3d(RenderContext *ctx, const char *str,
                         int32_t world_x, int32_t world_y, int32_t world_z,
                         uint8_t r, uint8_t g, uint8_t b,
                         int fade_factor, int mirror, TextPlane plane, int pixel);

#endif
