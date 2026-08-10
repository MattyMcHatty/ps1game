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

/* Sign, centred on the DRAWN door and sitting just proud of its wall.
 *
 * These do NOT derive from DOOR_X/DOOR_Z above: those are the interaction point,
 * pushed out into the room so the trigger's Manhattan test feels right, and they
 * are not where the door is drawn. Taking the sign's position from them left it
 * 59 units off centre and 187 units out from the wall, floating in mid-air.
 *
 * The real door, from the double_door polys in "Delivery Area v2.smx":
 *     wall  x = -5451        leaves  z[3924,4320]   y[-899,-524]
 * so its midpoint is z=4122, y=-711. */
#define SIGN_X    (-5451 + 11)    /* 11 units proud of the wall, toward the
                                     player, who stands at +X — the same standoff
                                     the other rooms' door signs use */
#define SIGN_Y    (-725)          /* text TOP. The glyphs are 7 rows of
                                     DOOR_PIXEL_SIZE = 28 units tall, so a top of
                                     -725 centres them on the door's y midpoint */
#define SIGN_Z    (4122 - 200)    /* door_draw_string_3d adds 200 back to the
                                     reading axis before centring, hence the -200 */

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

/* Screen-space pixel-font text: 5x7 glyphs on a 6px cell, drawn as 1x1 tiles
   sorted at ot[ot_idx]. Smaller than btn_prompt_draw's 8px debug font, and it
   covers lowercase — for captions that have to fit in a corner. */
#define DOOR_SMALL_CELL_W 6   /* 5 glyph columns + 1 gap */
int  door_small_text_width(const char *str);
void door_draw_string_2d(RenderContext *ctx, const char *str,
                         int32_t sx, int32_t sy,
                         uint8_t r, uint8_t g, uint8_t b, int ot_idx);

/* Camera-facing (billboard) variant of the pixel-font text, centred on
   (wx,wy,wz). Caller must have the camera view matrix loaded in the GTE. */
void door_draw_string_billboard(RenderContext *ctx, const char *str,
                                int32_t wx, int32_t wy, int32_t wz,
                                uint8_t r, uint8_t g, uint8_t b,
                                int fade_factor, int pixel);

#endif
