#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>

#define OT_LENGTH     512
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

MATRIX cube_matrix;
int rotation = 0;

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
    {0,1,2,3}, // front
    {5,4,7,6}, // back
    {4,0,3,7}, // left
    {1,5,6,2}, // right
    {4,5,1,0}, // top
    {3,2,6,7}, // bottom
};

uint8_t face_colors[6][3] = {
    {255,   0,   0},
    {  0, 255,   0},
    {  0,   0, 255},
    {255, 255,   0},
    {255,   0, 255},
    {  0, 255, 255},
};

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

void draw_cube(RenderContext *ctx) {
    SVECTOR rot_vec = {rotation, rotation, 0, 0};
    VECTOR  pos = {0, 0, 400};

    RotMatrix(&rot_vec, &cube_matrix);
    TransMatrix(&cube_matrix, &pos);
    gte_SetRotMatrix(&cube_matrix);
    gte_SetTransMatrix(&cube_matrix);

    int i;
    for (i = 0; i < 6; i++) {
        POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
        setPolyF4(poly);
        setRGB0(poly, face_colors[i][0], face_colors[i][1], face_colors[i][2]);

        DVECTOR sv0, sv1, sv2, sv3;

        gte_ldv0(&cube_verts[cube_faces[i][0]]);
        gte_rtps();
        gte_stsxy(&sv0);

        gte_ldv0(&cube_verts[cube_faces[i][1]]);
        gte_rtps();
        gte_stsxy(&sv1);

        gte_ldv0(&cube_verts[cube_faces[i][2]]);
        gte_rtps();
        gte_stsxy(&sv2);

        gte_ldv0(&cube_verts[cube_faces[i][3]]);
        gte_rtps();
        gte_stsxy(&sv3);

        // Backface culling using cross product of first 3 projected verts
        int32_t nclip =
            (sv1.vx - sv0.vx) * (sv2.vy - sv0.vy) -
            (sv1.vy - sv0.vy) * (sv2.vx - sv0.vx);

        if (nclip <= 0) continue; // back-facing, skip

        poly->x0 = sv0.vx; poly->y0 = sv0.vy;
        poly->x1 = sv1.vx; poly->y1 = sv1.vy;
        poly->x2 = sv3.vx; poly->y2 = sv3.vy;
        poly->x3 = sv2.vx; poly->y3 = sv2.vy;
        

        addPrim(&ctx->buffers[ctx->active_buffer].ot[8], poly);
        ctx->next_packet += sizeof(POLY_F4);
    }
}

int main(int argc, const char **argv) {
    ResetGraph(0);

    RenderContext ctx;
    setup_context(&ctx, SCREEN_XRES, SCREEN_YRES, 0, 0, 0);

    InitGeom();
    gte_SetGeomScreen(160);
    gte_SetGeomOffset(160, 120);

    for (;;) {
        draw_cube(&ctx);
        rotation = (rotation + 8) & 4095;
        flip_buffers(&ctx);
    }

    return 0;
}