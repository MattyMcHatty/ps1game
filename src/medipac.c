#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "medipac.h"

#define MEDIPAC_X            (-1400)
#define MEDIPAC_Z            (-1400)
#define MEDIPAC_SW           60      /* half-width  (long axis) */
#define MEDIPAC_SD           20      /* half-depth  (short axis) */
#define MEDIPAC_HEIGHT       40      /* full box height — half the original */
#define MEDIPAC_BASE_TOP_Y   20      /* hover: ~90 units above floor (floor at y=150) */
#define MEDIPAC_COLLECT_DIST 200

#define SPIN_RATE  6
#define FLOAT_RATE 3
#define FLOAT_AMP  12

static int medipac_taken       = 0;
static int medipac_spin_angle  = 0;
static int medipac_float_angle = 0;

void reset_medipac(void) {
    medipac_taken       = 0;
    medipac_spin_angle  = 0;
    medipac_float_angle = 0;
}

void update_medipac(void) {
    medipac_spin_angle  = (medipac_spin_angle  + SPIN_RATE)  & 4095;
    medipac_float_angle = (medipac_float_angle + FLOAT_RATE) & 4095;

    if (medipac_taken) return;

    int32_t dx   = cam_x - MEDIPAC_X;
    int32_t dz   = cam_z - MEDIPAC_Z;
    int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (dist < MEDIPAC_COLLECT_DIST) {
        player_health = MAX_HEALTH;
        medipac_taken = 1;
    }
}

void draw_medipac(RenderContext *ctx) {
    if (medipac_taken) return;

    int32_t float_y = (isin(medipac_float_angle) * FLOAT_AMP) >> 12;
    int16_t top_y   = (int16_t)(MEDIPAC_BASE_TOP_Y + float_y);
    int16_t bot_y   = (int16_t)(top_y + MEDIPAC_HEIGHT);

    /* CCW Y-axis rotation with separate half-width (SW) and half-depth (SD).
       Local corners (±SW, ±SD) in XZ; rotation formula:
         wx = cx + (lx*cos + lz*sin) / 4096
         wz = cz + (-lx*sin + lz*cos) / 4096            */
    int32_t cos_a  = icos(medipac_spin_angle);
    int32_t sin_a  = isin(medipac_spin_angle);
    int16_t cos_sw = (int16_t)((cos_a * MEDIPAC_SW) >> 12);
    int16_t sin_sw = (int16_t)((sin_a * MEDIPAC_SW) >> 12);
    int16_t cos_sd = (int16_t)((cos_a * MEDIPAC_SD) >> 12);
    int16_t sin_sd = (int16_t)((sin_a * MEDIPAC_SD) >> 12);

    SVECTOR v[8] = {
        {MEDIPAC_X - cos_sw - sin_sd, top_y, MEDIPAC_Z + sin_sw - cos_sd, 0}, /* 0 top */
        {MEDIPAC_X + cos_sw - sin_sd, top_y, MEDIPAC_Z - sin_sw - cos_sd, 0}, /* 1 top */
        {MEDIPAC_X + cos_sw + sin_sd, top_y, MEDIPAC_Z - sin_sw + cos_sd, 0}, /* 2 top */
        {MEDIPAC_X - cos_sw + sin_sd, top_y, MEDIPAC_Z + sin_sw + cos_sd, 0}, /* 3 top */
        {MEDIPAC_X - cos_sw - sin_sd, bot_y, MEDIPAC_Z + sin_sw - cos_sd, 0}, /* 4 bot */
        {MEDIPAC_X + cos_sw - sin_sd, bot_y, MEDIPAC_Z - sin_sw - cos_sd, 0}, /* 5 bot */
        {MEDIPAC_X + cos_sw + sin_sd, bot_y, MEDIPAC_Z - sin_sw + cos_sd, 0}, /* 6 bot */
        {MEDIPAC_X - cos_sw + sin_sd, bot_y, MEDIPAC_Z + sin_sw + cos_sd, 0}, /* 7 bot */
    };

    int faces[5][4] = {
        {0, 3, 2, 1},  /* top              */
        {0, 1, 5, 4},  /* north  (z-)      */
        {1, 2, 6, 5},  /* east   (x+)      */
        {2, 3, 7, 6},  /* south  (z+)      */
        {3, 0, 4, 7},  /* west   (x-)      */
    };

    uint8_t colors[5][3] = {
        {  0, 200,   0},  /* top   - bright green */
        {  0, 130,   0},  /* north - mid green    */
        {  0, 130,   0},  /* east                 */
        {  0, 130,   0},  /* south                */
        {  0, 130,   0},  /* west                 */
    };

    draw_faces(ctx, v, faces, colors, 5, 0);
}
