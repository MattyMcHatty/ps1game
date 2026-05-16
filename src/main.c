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

int32_t cam_x   = 0;
int32_t cam_z   = -500;
int32_t cam_rot = 0;

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

void apply_collision(void) {
    int32_t margin = 500;

    if (cam_x < -1800 + margin) cam_x = -1800 + margin;
    if (cam_x >  1800 - margin) cam_x =  1800 - margin;
    if (cam_z < -1800 + margin) cam_z = -1800 + margin;
    if (cam_z >  1800 - margin) cam_z =  1800 - margin;
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
        DVECTOR sv[4];
        int32_t otz, nclip;

        gte_ldv3(&verts[faces[i][0]], &verts[faces[i][1]], &verts[faces[i][2]]);
        gte_rtpt();
        gte_stsxy3c(sv);

        gte_nclip();
        gte_stopz(&nclip);
        if (nclip <= 0) continue;

        gte_ldv0(&verts[faces[i][3]]);
        gte_rtps();
        gte_stsxy(&sv[3]);

        gte_avsz4();
        gte_stotz(&otz);

        otz += depth_bias;
        if (otz <= 0) continue;
        if (otz >= OT_LENGTH) otz = OT_LENGTH - 1;

        int32_t face_cx = ((int32_t)verts[faces[i][0]].vx + verts[faces[i][2]].vx) / 2;
        int32_t face_cz = ((int32_t)verts[faces[i][0]].vz + verts[faces[i][2]].vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);

        int32_t fog_start = 500;
        int32_t fog_end   = 3000;
        int32_t fog = dist;
        if (fog < fog_start) fog = fog_start;
        if (fog > fog_end)   fog = fog_end;
        int32_t range = fog_end - fog_start;
        int32_t fog_factor = ((fog_end - fog) << 8) / range;

        uint8_t r = (uint8_t)((colors[i][0] * fog_factor) >> 8);
        uint8_t g = (uint8_t)((colors[i][1] * fog_factor) >> 8);
        uint8_t b = (uint8_t)((colors[i][2] * fog_factor) >> 8);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(POLY_F4) > buf_end) continue;

        POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
        setPolyF4(poly);
        setRGB0(poly, r, g, b);

        poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
        poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
        poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
        poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;

        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
        ctx->next_packet += sizeof(POLY_F4);
    }
}

void draw_panel(
    RenderContext *ctx,
    int ox, int oy, int oz,
    int ux, int uy, int uz,
    int vx, int vy, int vz,
    int segs_u, int segs_v,
    uint8_t cr, uint8_t cg, uint8_t cb,
    int depth_bias
) {
    int i, j;
    for (j = 0; j < segs_v; j++) {
        for (i = 0; i < segs_u; i++) {
            int bx = ox + i*ux + j*vx;
            int by = oy + i*uy + j*vy;
            int bz = oz + i*uz + j*vz;

            SVECTOR v[4];
            v[0].vx = bx;       v[0].vy = by;       v[0].vz = bz;       v[0].pad = 0;
            v[1].vx = bx+ux;    v[1].vy = by+uy;    v[1].vz = bz+uz;    v[1].pad = 0;
            v[2].vx = bx+ux+vx; v[2].vy = by+uy+vy; v[2].vz = bz+uz+vz; v[2].pad = 0;
            v[3].vx = bx+vx;    v[3].vy = by+vy;    v[3].vz = bz+vz;    v[3].pad = 0;

            DVECTOR sv[4];
            int32_t sz[4];
            int32_t otz, nclip;

            gte_ldv3(&v[0], &v[1], &v[2]);
            gte_rtpt();
            gte_stsxy3c(sv);

            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) continue;

            gte_ldv0(&v[3]);
            gte_rtps();
            gte_stsxy(&sv[3]);

            gte_stsz4c(sz);
            if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) continue;

            gte_avsz4();
            gte_stotz(&otz);

            otz += depth_bias;
            if (otz <= 0) continue;
            if (otz >= OT_LENGTH) otz = OT_LENGTH - 1;

            int32_t face_cx = bx + (ux + vx) / 2;
            int32_t face_cz = bz + (uz + vz) / 2;
            int32_t dx = face_cx - cam_x;
            int32_t dz = face_cz - cam_z;
            int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);

            int32_t fog_start = 500;
            int32_t fog_end   = 3000;
            int32_t fog = dist;
            if (fog < fog_start) fog = fog_start;
            if (fog > fog_end)   fog = fog_end;
            int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

            uint8_t r = (uint8_t)((cr * fog_factor) >> 8);
            uint8_t g = (uint8_t)((cg * fog_factor) >> 8);
            uint8_t b = (uint8_t)((cb * fog_factor) >> 8);

            uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
            if (ctx->next_packet + sizeof(POLY_F4) > buf_end) continue;

            POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
            setPolyF4(poly);
            setRGB0(poly, r, g, b);

            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
            poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;

            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F4);
        }
    }
}

void draw_room(RenderContext *ctx) {
    /* back wall  z=-1800: TL=(+1800,-300,-1800), U=-X, V=+Y */
    draw_panel(ctx,  1800,-300,-1800,  -300,0,0,  0,600,0,  12,1,  80,80,80,  400);
    /* front wall z=+1800: TL=(-1800,-300,+1800), U=+X, V=+Y */
    draw_panel(ctx, -1800,-300, 1800,   300,0,0,  0,600,0,  12,1,  80,80,80,  400);
    /* right wall x=+1800: TL=(+1800,-300,+1800), U=-Z, V=+Y */
    draw_panel(ctx,  1800,-300, 1800,  0,0,-300,  0,600,0,  12,1,  60,60,60,  400);
    /* left wall  x=-1800: TL=(-1800,-300,-1800), U=+Z, V=+Y */
    draw_panel(ctx, -1800,-300,-1800,  0,0, 300,  0,600,0,  12,1,  60,60,60,  400);
    /* ceiling    y=-300:  TL=(-1800,-300,-1800), U=+X, V=+Z */
    draw_panel(ctx, -1800,-300,-1800,   300,0,0,  0,0,300,  12,12,  40,40,40,  400);
    /* floor      y=+300:  TL=(-1800,+300,-1800), U=+Z, V=+X */
    draw_panel(ctx, -1800, 300,-1800,  0,0,300,  300,0,0,   12,12,  50,50,70,  400);
}

void draw_scene(RenderContext *ctx) {
    MATRIX rot_matrix;
    SVECTOR neg_rot = {0, -cam_rot, 0, 0};

    RotMatrix(&neg_rot, &rot_matrix);

    VECTOR trans;
    trans.vx = -cam_x;
    trans.vy = 0;
    trans.vz = -cam_z;

    ApplyMatrixLV(&rot_matrix, &trans, &trans);

    rot_matrix.t[0] = trans.vx;
    rot_matrix.t[1] = trans.vy;
    rot_matrix.t[2] = trans.vz;

    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    draw_room(ctx);
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
        apply_collision();
        draw_scene(&ctx);
        flip_buffers(&ctx);
    }

    return 0;
}