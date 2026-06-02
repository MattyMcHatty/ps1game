#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>

#define OT_LENGTH     2048
#define BUFFER_LENGTH 65536
#define SCREEN_XRES   320
#define SCREEN_YRES   240

#define SKY_FOG_R 25
#define SKY_FOG_G  0
#define SKY_FOG_B 29

typedef struct {
    DISPENV  disp_env;
    DRAWENV  draw_env;
    uint32_t ot[OT_LENGTH];
    uint8_t  buffer[BUFFER_LENGTH];
} RenderBuffer;

typedef struct {
    RenderBuffer buffers[2];
    uint8_t     *next_packet;
    int          active_buffer;
} RenderContext;

void setup_context(RenderContext *ctx, int w, int h, int r, int g, int b);
void flip_buffers(RenderContext *ctx);
void draw_sky_gradient(RenderContext *ctx);
void draw_faces(RenderContext *ctx, SVECTOR *verts, int faces[][4],
                uint8_t colors[][3], int face_count, int depth_bias);

#endif
