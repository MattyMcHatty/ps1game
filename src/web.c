#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "collision.h"
#include "sound.h"
#include "web.h"

Web webs[MAX_WEBS];

void webs_reset(void) {
    int i;
    for (i = 0; i < MAX_WEBS; i++) webs[i].life = 0;
}

/* Integer square root (bit-by-bit). Needed to aim a web properly: dividing by a
   Manhattan distance — the shortcut used all over the steering code, where a
   direction only has to be roughly right — would make the web's speed vary by
   up to 40% with the firing angle, so diagonal shots would crawl. Runs once per
   spawn, a few times a second at most. */
static int32_t isqrt32(int32_t v) {
    if (v <= 0) return 0;
    uint32_t n = (uint32_t)v, rem = 0, root = 0;
    int i;
    for (i = 0; i < 16; i++) {
        root <<= 1;
        rem = (rem << 2) | (n >> 30);
        n <<= 2;
        if (root < rem) { rem -= root + 1; root += 2; }
    }
    return (int32_t)(root >> 1);
}

void web_spawn(int32_t x, int32_t y, int32_t z,
               int32_t tx, int32_t ty, int32_t tz, GameState area) {
    int i;
    for (i = 0; i < MAX_WEBS; i++)
        if (webs[i].life <= 0) break;
    if (i >= MAX_WEBS) return;      /* pool full: drop the shot */

    int32_t dx = tx - x, dy = ty - y, dz = tz - z;
    int32_t len = isqrt32(dx * dx + dy * dy + dz * dz);
    if (len <= 0) return;           /* fired at its own position */

    Web *w = &webs[i];
    w->x  = x;  w->y  = y;  w->z  = z;
    w->vx = (dx * WEB_SPEED) / len;
    w->vy = (dy * WEB_SPEED) / len;
    w->vz = (dz * WEB_SPEED) / len;
    w->life = WEB_LIFE;
    w->area = area;
}

void webs_update(void) {
    int i;
    for (i = 0; i < MAX_WEBS; i++) {
        Web *w = &webs[i];
        if (w->life <= 0) continue;
        if (w->area != current_area) { w->life = 0; continue; }   /* left the room */

        if (--w->life <= 0) continue;

        int32_t ox = w->x, oy = w->y, oz = w->z;
        w->x += w->vx;
        w->y += w->vy;
        w->z += w->vz;

        /* Walls and solid props stop a web. Testing the SEGMENT just travelled
           rather than the new point means a fast web can never tunnel through a
           thin wall between two frames — the same reason the gun's hitscan uses
           an exact crossing test. */
        if (collision_segment_blocked(ox, oy, oz, w->x, w->y, w->z)) {
            w->life = 0;
            continue;
        }

        /* Contact with the player. Radial in all three axes: the web was aimed
           at the player's anchor, so dy closes to nothing as it arrives, and a
           flat XZ test would let a web pass through their feet or over their
           head and still count. */
        if (!game_over) {
            int32_t hx = player_x() - w->x;
            int32_t hy = player_y() - w->y;
            int32_t hz = player_z() - w->z;
            if (hx * hx + hy * hy + hz * hz <=
                (int32_t)WEB_HIT_RADIUS * WEB_HIT_RADIUS) {
                w->life = 0;
                player_hurt(WEB_DAMAGE);
                player_poison();
                sound_play(SFX_HURT);
                if (player_health <= 0) {
                    player_health = 0;
                    game_over     = 1;
                    flash_timer   = 90;
                    sound_play(SFX_DIE);
                }
            }
        }
    }
}

/* Draw one web as a solid white cube: eight corners, six quads.
 *
 * All six faces are drawn — no backface cull — and all at the SAME flat colour
 * and the same OT depth. That is deliberate: a convex solid whose faces are all
 * one colour looks identical whichever of them the GPU happens to lay down
 * first, so the result is exactly the cube's silhouette filled white, with no
 * dependence on getting the winding order right for gte_nclip. At the size a
 * web appears on screen there is nothing to gain from shaded faces.
 */
static void draw_web(RenderContext *ctx, Web *w) {
    static const int8_t corner[8][3] = {
        { -1, -1, -1 }, {  1, -1, -1 }, {  1, -1,  1 }, { -1, -1,  1 },  /* top    */
        { -1,  1, -1 }, {  1,  1, -1 }, {  1,  1,  1 }, { -1,  1,  1 },  /* bottom */
    };
    /* Each face as four corner indices, in quad order (the POLY_F4 swap from
       0,1,2,3 to 0,1,3,2 happens where the primitive is filled). */
    static const uint8_t face[6][4] = {
        { 0, 1, 2, 3 },   /* top    (-Y) */
        { 4, 5, 6, 7 },   /* bottom (+Y) */
        { 0, 1, 5, 4 },   /* -Z          */
        { 3, 2, 6, 7 },   /* +Z          */
        { 0, 3, 7, 4 },   /* -X          */
        { 1, 2, 6, 5 },   /* +X          */
    };

    /* Fog to match the room mesh, so a web does not stay bright against walls
       that have faded out. Beyond fog_far there is nothing to see. */
    int32_t fdx  = w->x - cam_x;
    int32_t fdz  = w->z - cam_z;
    int32_t dist = (fdx < 0 ? -fdx : fdx) + (fdz < 0 ? -fdz : fdz);
    if (dist >= g_fog_far) return;
    int32_t fs   = render_fog_scale(dist);
    uint8_t fog8 = fs > 255 ? 255 : (uint8_t)fs;

    SVECTOR v[8];
    int c;
    for (c = 0; c < 8; c++) {
        v[c].vx  = (int16_t)(w->x + corner[c][0] * WEB_HALF);
        v[c].vy  = (int16_t)(w->y + corner[c][1] * WEB_HALF);
        v[c].vz  = (int16_t)(w->z + corner[c][2] * WEB_HALF);
        v[c].pad = 0;
    }

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int f;
    for (f = 0; f < 6; f++) {
        if (ctx->next_packet + sizeof(POLY_F4) > buf_end) return;

        DVECTOR sv[4];
        int32_t sz[4], otz;

        gte_ldv3(&v[face[f][0]], &v[face[f][1]], &v[face[f][2]]);
        gte_rtpt();
        gte_stsxy3c(sv);

        gte_ldv0(&v[face[f][3]]);
        gte_rtps();
        gte_stsxy(&sv[3]);

        gte_stsz4c(sz);
        if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) continue;

        gte_avsz4();
        gte_stotz(&otz);
        /* Same scale the room mesh sorts on, so walls in front of a web occlude
           it, matching how the spider sprites sort. */
        if (otz <= 0) continue;
        if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
        setPolyF4(poly);
        setRGB0(poly, fog8, fog8, fog8);
        poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
        poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
        poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
        poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;
        addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
        ctx->next_packet += sizeof(POLY_F4);
    }
}

void webs_draw(RenderContext *ctx) {
    int i;
    for (i = 0; i < MAX_WEBS; i++) {
        Web *w = &webs[i];
        if (w->life <= 0 || w->area != current_area) continue;
        draw_web(ctx, w);
    }
}
