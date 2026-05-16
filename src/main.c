#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "spi.h"

#define OT_LENGTH     2048
#define BUFFER_LENGTH 32768
#define SCREEN_XRES   320
#define SCREEN_YRES   240

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

// Change starting position
int32_t cam_x   = 0;
int32_t cam_y   = 0;
int32_t cam_z   = -800;  // further back
int32_t cam_rot = 0;

// Cube
MATRIX cube_matrix;

SVECTOR cube_verts[8] = {
    {-100, -100, -100, 0},
    { 100, -100, -100, 0},
    { 100,  100, -100, 0},
    {-100,  100, -100, 0},
    {-100, -100,  100, 0},
    { 100, -100,  100, 0},
    { 100,  100,  100, 0},
    {-100,  100,  100, 0},
};

int cube_faces[6][4] = {
    {0,1,2,3},
    {5,4,7,6},
    {4,0,3,7},
    {1,5,6,2},
    {4,5,1,0},
    {3,2,6,7},
};

uint8_t face_colors[6][3] = {
    {255,   0,   0},
    {  0, 255,   0},
    {  0,   0, 255},
    {255, 255,   0},
    {255,   0, 255},
    {  0, 255, 255},
};

// Room (a big box around the cube - we're inside it)
SVECTOR room_verts[8] = {
    {-600, -300, -600, 0},
    { 600, -300, -600, 0},
    { 600,  300, -600, 0},
    {-600,  300, -600, 0},
    {-600, -300,  600, 0},
    { 600, -300,  600, 0},
    { 600,  300,  600, 0},
    {-600,  300,  600, 0},
};

int room_faces[6][4] = {
    {1,0,3,2},
    {4,5,6,7},
    {5,1,2,6},
    {0,4,7,3},
    {0,1,5,4},
    {3,7,6,2},
};

uint8_t room_colors[6][3] = {
    { 80,  80,  80},
    { 80,  80,  80},
    { 60,  60,  60},
    { 60,  60,  60},
    { 40,  40,  40},
    { 50,  50,  70},
};

// Pad state
static volatile uint8_t pad_buff[2][34];
static volatile size_t  pad_buff_len[2];

void poll_cb(uint32_t port, const volatile uint8_t *buff, size_t rx_len) {
    pad_buff_len[port] = rx_len;
    if (rx_len)
        memcpy((void *)pad_buff[port], (void *)buff, rx_len);
}

void setup_context(RenderContext *ctx, int w, int h, int r, int g, int b) {
    SetDefDrawEnv(&ctx->buffers[0].draw_env, 0, 0, w, h);
    SetDefDispEnv(&ctx->buffers[0].disp_env, 0, 0, w, h);
    SetDefDrawEnv(&ctx->buffers[1].draw_env, 0, h, w, h);
    SetDefDispEnv(&ctx->buffers[1].disp_env, 0, h, w, h);

    setRGB0(&ctx->buffers[0].draw_env, r, g, b);
    setRGB0(&ctx->buffers[1].draw_env, r, g, b);
    ctx->buffers[0].draw_env.isbg = 1;
    ctx->buffers[1].draw_env.isbg = 1;

    ctx->active_buffer = 0;
    ctx->next_packet   = ctx->buffers[0].buffer;
    ClearOTagR(ctx->buffers[0].ot, OT_LENGTH);
    ClearOTagR(ctx->buffers[1].ot, OT_LENGTH);

    SetDispMask(1);
}

void flip_buffers(RenderContext *ctx) {
    DrawSync(0);
    VSync(0);

    RenderBuffer *draw_buffer = &ctx->buffers[ctx->active_buffer];
    RenderBuffer *disp_buffer = &ctx->buffers[ctx->active_buffer ^ 1];

    PutDispEnv(&disp_buffer->disp_env);
    DrawOTagEnv(&draw_buffer->ot[OT_LENGTH - 1], &draw_buffer->draw_env);

    ctx->active_buffer ^= 1;
    ctx->next_packet    = disp_buffer->buffer;
    ClearOTagR(disp_buffer->ot, OT_LENGTH);
}

void update_camera(void) {
    if (!pad_buff_len[0]) return;

    PadResponse *pad = (PadResponse *)pad_buff[0];
    uint16_t btn = ~pad->btn;

    if (btn & PAD_LEFT)  cam_rot = (cam_rot - 32) & 4095;
    if (btn & PAD_RIGHT) cam_rot = (cam_rot + 32) & 4095;

    int32_t speed = 12;
    if (btn & PAD_UP) {
        cam_x += (isin(cam_rot) * speed) >> 12;
        cam_z += (icos(cam_rot) * speed) >> 12;
    }
    if (btn & PAD_DOWN) {
        cam_x -= (isin(cam_rot) * speed) >> 12;
        cam_z -= (icos(cam_rot) * speed) >> 12;
    }
}

void draw_faces(
    RenderContext *ctx,
    SVECTOR *verts,
    int faces[][4],
    uint8_t colors[][3],
    int face_count,
    int depth_bias
) {
    int i;
    for (i = 0; i < face_count; i++) {
        POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
        setPolyF4(poly);
        setRGB0(poly, colors[i][0], colors[i][1], colors[i][2]);

        DVECTOR sv0, sv1, sv2, sv3;
        int32_t otz0, otz1, otz2, otz3, otz;

        gte_ldv0(&verts[faces[i][0]]);
        gte_rtps();
        gte_stsxy(&sv0);
        gte_stotz(otz0);

        gte_ldv0(&verts[faces[i][1]]);
        gte_rtps();
        gte_stsxy(&sv1);
        gte_stotz(otz1);

        gte_ldv0(&verts[faces[i][2]]);
        gte_rtps();
        gte_stsxy(&sv2);
        gte_stotz(otz2);

        gte_ldv0(&verts[faces[i][3]]);
        gte_rtps();
        gte_stsxy(&sv3);
        gte_stotz(otz3);

        otz = (otz0 + otz1 + otz2 + otz3) / 4;
        otz += depth_bias;
        if (otz <= 0) otz = 1;
        if (otz >= OT_LENGTH) otz = OT_LENGTH - 1;

        int32_t nclip =
            (sv1.vx - sv0.vx) * (sv2.vy - sv0.vy) -
            (sv1.vy - sv0.vy) * (sv2.vx - sv0.vx);
        if (nclip <= 0) continue;

        poly->x0 = sv0.vx; poly->y0 = sv0.vy;
        poly->x1 = sv1.vx; poly->y1 = sv1.vy;
        poly->x2 = sv3.vx; poly->y2 = sv3.vy;
        poly->x3 = sv2.vx; poly->y3 = sv2.vy;

        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
        ctx->next_packet += sizeof(POLY_F4);
    }
}

void draw_scene(RenderContext *ctx) {
    MATRIX rot_matrix;
    SVECTOR neg_rot = {0, -cam_rot, 0, 0};

    // Step 1: build rotation matrix from camera angle
    RotMatrix(&neg_rot, &rot_matrix);

    // Step 2: translation is simply the negative camera position
    // rotated by the camera's own rotation matrix
    VECTOR trans;
    trans.vx = -cam_x;
    trans.vy = -cam_y;
    trans.vz = -cam_z;

    // Apply rotation to the translation vector
    ApplyMatrixLV(&rot_matrix, &trans, &trans);

    rot_matrix.t[0] = trans.vx;
    rot_matrix.t[1] = trans.vy;
    rot_matrix.t[2] = trans.vz;

    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    draw_faces(ctx, room_verts, room_faces, room_colors, 6, 400);
    draw_faces(ctx, cube_verts, cube_faces, face_colors, 6, 0);
}

int main(int argc, const char **argv) {
    ResetGraph(0);

    RenderContext ctx;
    setup_context(&ctx, SCREEN_XRES, SCREEN_YRES, 0, 0, 0);

    InitGeom();
    gte_SetGeomScreen(160);
    gte_SetGeomOffset(160, 120);

    SPI_Init(&poll_cb);

    for (;;) {
        update_camera();
        draw_scene(&ctx);
        flip_buffers(&ctx);
    }

    return 0;
}