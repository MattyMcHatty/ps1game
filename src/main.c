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

int32_t enemy_x = 1200;
int32_t enemy_z = 1200;

#define ENEMY_SPEED   4
#define ENEMY_HALF_W  100
#define ENEMY_HALF_H  200
#define ENEMY_Y       100
#define CATCH_DIST      200
#define MAX_HEALTH      100
#define DAMAGE_INTERVAL 2

#define SWING_DURATION   7
#define SWING_RETURN    14
#define SWING_TOTAL     21
#define SWING_RANGE     350
#define KNOCKBACK_SPEED  80

static int     game_over     = 0;
static int     flash_timer   = 0;
static int     damage_timer  = 0;
static int32_t player_health = MAX_HEALTH;

static int swing_timer    = 0;
static int hit_this_swing = 0;

#define ENEMY_MAX_HEALTH 5
static int enemy_health = ENEMY_MAX_HEALTH;

static int32_t enemy_kb_vx = 0;
static int32_t enemy_kb_vz = 0;




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

void update_enemy(void) {
    if (enemy_health <= 0) return;

    if (enemy_kb_vx != 0 || enemy_kb_vz != 0) {
        enemy_x += enemy_kb_vx;
        enemy_z += enemy_kb_vz;
        if (enemy_x < -1600) { enemy_x = -1600; enemy_kb_vx = 0; }
        if (enemy_x >  1600) { enemy_x =  1600; enemy_kb_vx = 0; }
        if (enemy_z < -1600) { enemy_z = -1600; enemy_kb_vz = 0; }
        if (enemy_z >  1600) { enemy_z =  1600; enemy_kb_vz = 0; }
        enemy_kb_vx = (enemy_kb_vx * 7) >> 3;
        enemy_kb_vz = (enemy_kb_vz * 7) >> 3;
        damage_timer = 0;
        return;
    }

    int32_t dx   = cam_x - enemy_x;
    int32_t dz   = cam_z - enemy_z;
    int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (!game_over && dist < CATCH_DIST) {
        if (++damage_timer >= DAMAGE_INTERVAL) {
            damage_timer = 0;
            player_health -= 1;
            if (player_health <= 0) {
                player_health = 0;
                game_over   = 1;
                flash_timer = 90;
            }
        }
    } else {
        damage_timer = 0;
    }
    if (dist < ENEMY_SPEED) return;
    enemy_x += (dx * ENEMY_SPEED) / dist;
    enemy_z += (dz * ENEMY_SPEED) / dist;
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

void draw_enemy(RenderContext *ctx) {
    if (enemy_health <= 0) return;
    int32_t rx  = icos(cam_rot);
    int32_t rz  = -isin(cam_rot);
    int16_t dwx = (int16_t)((ENEMY_HALF_W * rx) >> 12);
    int16_t dwz = (int16_t)((ENEMY_HALF_W * rz) >> 12);

    SVECTOR v[4];
    v[0].vx = enemy_x - dwx; v[0].vy = ENEMY_Y - ENEMY_HALF_H; v[0].vz = enemy_z - dwz; v[0].pad = 0;
    v[1].vx = enemy_x + dwx; v[1].vy = ENEMY_Y - ENEMY_HALF_H; v[1].vz = enemy_z + dwz; v[1].pad = 0;
    v[2].vx = enemy_x + dwx; v[2].vy = ENEMY_Y + ENEMY_HALF_H; v[2].vz = enemy_z + dwz; v[2].pad = 0;
    v[3].vx = enemy_x - dwx; v[3].vy = ENEMY_Y + ENEMY_HALF_H; v[3].vz = enemy_z - dwz; v[3].pad = 0;

    DVECTOR sv[4];
    int32_t sz[4];
    int32_t otz, nclip;

    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);

    gte_nclip();
    gte_stopz(&nclip);
    if (nclip <= 0) return;

    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);

    gte_stsz4c(sz);
    if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) return;

    gte_avsz4();
    gte_stotz(&otz);

    if (otz <= 0 || otz >= OT_LENGTH) return;

    int32_t dx   = enemy_x - cam_x;
    int32_t dz   = enemy_z - cam_z;
    int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int32_t fog_start  = 500;
    int32_t fog_end    = 3000;
    int32_t fog        = dist < fog_start ? fog_start : dist > fog_end ? fog_end : dist;
    int32_t fog_factor = ((fog_end - fog) << 8) / (fog_end - fog_start);

    uint8_t r = (uint8_t)((60 * fog_factor) >> 8);
    uint8_t g = (uint8_t)(( 5 * fog_factor) >> 8);
    uint8_t b = (uint8_t)(( 5 * fog_factor) >> 8);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_F4) > buf_end) return;

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

void update_bat(void) {
    if (swing_timer == 0 && pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        if (~pad->btn & PAD_SQUARE) {
            swing_timer    = 1;
            hit_this_swing = 0;
        }
    }

    if (swing_timer > 0) {
        /* Hit window: forward stroke only, one hit per swing */
        if (swing_timer <= SWING_DURATION && !hit_this_swing) {
            int32_t dx   = enemy_x - cam_x;
            int32_t dz   = enemy_z - cam_z;
            int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            if (dist < SWING_RANGE) {
                int32_t dot = ((int32_t)dx * isin(cam_rot) +
                               (int32_t)dz * icos(cam_rot)) >> 12;
                if (dot > 0) {
                    enemy_kb_vx    = (dx * KNOCKBACK_SPEED) / dist;
                    enemy_kb_vz    = (dz * KNOCKBACK_SPEED) / dist;
                    enemy_health--;
                    hit_this_swing = 1;
                }
            }
        }
        if (++swing_timer > SWING_TOTAL)
            swing_timer = 0;
    }
}

void draw_bat(RenderContext *ctx) {
    /* t in [0,256]: 0 = rest, 256 = full swing */
    int32_t t;
    if (swing_timer == 0) {
        t = 0;
    } else if (swing_timer <= SWING_DURATION) {
        t = (swing_timer * 256) / SWING_DURATION;
    } else {
        int32_t ret = swing_timer - SWING_DURATION;
        t = 256 - (ret * 256) / SWING_RETURN;
    }

    /* Camera basis (Q12) */
    int32_t fwd_x   =  isin(cam_rot);
    int32_t fwd_z   =  icos(cam_rot);
    int32_t right_x =  icos(cam_rot);
    int32_t right_z = -isin(cam_rot);

    /* Bat spine endpoints in camera-local space (lerped).
       +x = camera right, -y = up, +z = forward.
       Rest:  handle(25,40,40)   tip(12,-10,100)  barrel raised, ~40 deg from vertical
       Swing: handle(22,47,40)   tip(-80,35,160)  barrel swept to the left           */
    int32_t h_lx = 25 - (( 3 * t) >> 8);
    int32_t h_ly = 40 + (( 7 * t) >> 8);
    int32_t h_lz = 40;

    int32_t p_lx = 12  - (( 92 * t) >> 8);
    int32_t p_ly = -10 + (( 45 * t) >> 8);
    int32_t p_lz = 100 + (( 60 * t) >> 8);

    /* Half-dimensions of the bat cross-section */
    const int32_t W = 4;   /* half-width  (local x) */
    const int32_t H = 5;   /* half-height (world y)  */

    /* Build 8 world-space corners.
       c[0..3] = handle end (TL,TR,BR,BL), c[4..7] = tip end (TL,TR,BR,BL) */
#define CORNER(sv, lx, ly, lz) \
    (sv).vx  = (int16_t)(cam_x + (((lx)*right_x + (lz)*fwd_x) >> 12)); \
    (sv).vy  = (int16_t)(ly); \
    (sv).vz  = (int16_t)(cam_z + (((lx)*right_z + (lz)*fwd_z) >> 12)); \
    (sv).pad = 0

    SVECTOR c[8];
    CORNER(c[0], h_lx-W, h_ly-H, h_lz);
    CORNER(c[1], h_lx+W, h_ly-H, h_lz);
    CORNER(c[2], h_lx+W, h_ly+H, h_lz);
    CORNER(c[3], h_lx-W, h_ly+H, h_lz);
    CORNER(c[4], p_lx-W, p_ly-H, p_lz);
    CORNER(c[5], p_lx+W, p_ly-H, p_lz);
    CORNER(c[6], p_lx+W, p_ly+H, p_lz);
    CORNER(c[7], p_lx-W, p_ly+H, p_lz);
#undef CORNER

    /* Face table: indices into c[], then RGB.
       Vertex order (v0,v1,v2,v3) → poly (x0,x1, x2=v3,x3=v2).
       Winding is CCW from outside; nclip discards backfaces automatically. */
    static const struct { int8_t i[4]; uint8_t r, g, b; } faces[5] = {
        { {0, 4, 5, 1}, 130,  75, 22 },  /* top    - lighter  */
        { {0, 3, 7, 4},  80,  46, 14 },  /* left   - darker   */
        { {1, 2, 6, 5}, 105,  60, 18 },  /* right  - medium   */
        { {4, 5, 6, 7}, 150,  88, 26 },  /* tip    - brightest */
        { {2, 3, 7, 6},  55,  32, 10 },  /* bottom - darkest  */
    };

    int fi;
    for (fi = 0; fi < 5; fi++) {
        DVECTOR sv[4];
        int32_t otz, nclip;

        gte_ldv3(&c[faces[fi].i[0]], &c[faces[fi].i[1]], &c[faces[fi].i[2]]);
        gte_rtpt();
        gte_stsxy3c(sv);

        gte_nclip();
        gte_stopz(&nclip);
        if (nclip <= 0) continue;

        gte_ldv0(&c[faces[fi].i[3]]);
        gte_rtps();
        gte_stsxy(&sv[3]);

        gte_avsz4();
        gte_stotz(&otz);
        if (otz <= 0 || otz >= OT_LENGTH) continue;

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(POLY_F4) > buf_end) return;

        POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
        setPolyF4(poly);
        setRGB0(poly, faces[fi].r, faces[fi].g, faces[fi].b);

        poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
        poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
        poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
        poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;

        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
        ctx->next_packet += sizeof(POLY_F4);
    }
}

void draw_hud(RenderContext *ctx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    if (ctx->next_packet + sizeof(TILE) > buf_end) return;
    TILE *bg = (TILE *)ctx->next_packet;
    setTile(bg);
    setRGB0(bg, 40, 40, 40);
    setXY0(bg, 4, 4);
    setWH(bg, 102, 10);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[1], bg);
    ctx->next_packet += sizeof(TILE);

    if (player_health > 0) {
        if (ctx->next_packet + sizeof(TILE) > buf_end) return;
        TILE *bar = (TILE *)ctx->next_packet;
        setTile(bar);
        uint8_t r = (uint8_t)((200 * (MAX_HEALTH - player_health)) / MAX_HEALTH);
        uint8_t g = (uint8_t)((200 * player_health) / MAX_HEALTH);
        setRGB0(bar, r, g, 0);
        setXY0(bar, 5, 5);
        setWH(bar, (uint16_t)player_health, 8);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[0], bar);
        ctx->next_packet += sizeof(TILE);
    }
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
    draw_enemy(ctx);
    draw_bat(ctx);
    draw_hud(ctx);
}

void reset_game(RenderContext *ctx) {
    cam_x   = 0;
    cam_z   = -500;
    cam_rot = 0;
    enemy_x = 1200;
    enemy_z = 1200;
    game_over     = 0;
    flash_timer   = 0;
    damage_timer  = 0;
    player_health = MAX_HEALTH;
    swing_timer    = 0;
    hit_this_swing = 0;
    enemy_kb_vx    = 0;
    enemy_kb_vz    = 0;
    enemy_health   = ENEMY_MAX_HEALTH;
    setRGB0(&ctx->buffers[0].draw_env, 0, 0, 0);
    setRGB0(&ctx->buffers[1].draw_env, 0, 0, 0);
}

int main(int argc, const char **argv) {
    ResetGraph(0);

    RenderContext ctx;
    setup_context(&ctx, SCREEN_XRES, SCREEN_YRES, 0, 0, 0);

    InitGeom();
    gte_SetGeomScreen(160);
    gte_SetGeomOffset(160, 120);

    SPI_Init(&poll_cb);

    FntLoad(960, 0);
    FntOpen(40, 104, 240, 32, 0, 128);

    for (;;) {
        if (!game_over) {
            update_camera();
            apply_collision();
            update_enemy();
            update_bat();
            draw_scene(&ctx);
        } else {
            uint8_t r;
            if (flash_timer > 0) {
                flash_timer--;
                r = (flash_timer / 6) % 2 == 0 ? 200 : 0;
                setRGB0(&ctx.buffers[ctx.active_buffer].draw_env, r, 0, 0);
            } else {
                if (pad_buff_len[0] &&
                    (~((PadResponse *)pad_buff[0])->btn & PAD_START)) {
                    reset_game(&ctx);
                } else {
                    setRGB0(&ctx.buffers[ctx.active_buffer].draw_env, 80, 0, 0);
                    FntPrint(-1, "          GAME OVER\n    PRESS START TO RESTART");
                    FntFlush(-1);
                }
            }
        }
        flip_buffers(&ctx);
    }

    return 0;
}