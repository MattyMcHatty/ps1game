#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "item_pickup.h"
#include "menu.h"
#include "sound.h"
#include "door.h"
#include "btn_glyph.h"
#include "crucifaxe.h"
#include "graveolver.h"
#include "helluminator.h"
#include "sharur.h"
#include "destiny_plinths.h"

/* The Chamber of Destiny's west plinths - see destiny_plinths.h. */

#define DP_FACE_X        (-5400)   /* both fronts, facing east                    */
#define DP_DAIS_EYE_MAX  (-280)    /* cam_y above this = standing on the dais      */

/* The prompt: on the plinth's front face, 11 off it toward the reader, on the
   reader's yaw (west, 3072 - Chamber of Destiny's door sign's rule). */
#define DP_SIGN_X        (DP_FACE_X + 11)
#define DP_SIGN_Y        (-290)
#define DP_SIGN_YAW      3072
#define DP_TEXT_RADIUS   1100
#define DP_FADE_NEAR      700
#define DP_REACH          450      /* Manhattan, player to the face's centre       */

/* THE SHOT: square in front of the plinth, level, looking west at the item. */
#define DP_SHOT_X        (-5130)
#define DP_SHOT_Y        (-440)
#define DP_SHOT_YAW      3072

/* The beats, in frames (60 a second). */
#define DP_GLIDE_FRAMES   45      /* camera into the shot                         */
#define DP_RISE_AT        60      /* "after a second"                             */
#define DP_RISE_FRAMES    40
#define DP_GLOW_FRAMES   120      /* "glow for 2 seconds"                         */
#define DP_FLY_FRAMES     40
#define DP_GLOW_IN        20      /* the rings fade up over this                  */
#define DP_RISE_HEIGHT    80
#define DP_FLY_STOP      190      /* ends this far in front of the eye            */

#define SHARUR_SCALE      6554    /* 1.6x the held mace: 165 tall                  */
#define SHARUR_HALF        82     /* grip to the middle of it, at that scale       */
#define TABLET_HALF        70     /* the sprite's world half-size                  */

typedef struct {
    int32_t     z;         /* centre line                                       */
    int32_t     top_y;     /* the plinth's top                                  */
    int32_t     rest_cy;   /* the item's CENTRE at rest                         */
    GameFlag    taken;
    const char *name;
} Plinth;

static const Plinth plinths[2] = {
    { -300, -350, -350 - SHARUR_HALF, FLAG_SHARUR_TAKEN,              "Sharur" },
    {  300, -400, -400 - TABLET_HALF, FLAG_TABLET_OF_DESTINIES_TAKEN, "Tablet of Destinies" },
};

static int     dp_circle_prev = 1;
static int     dp_which = -1;       /* the plinth in ceremony, -1 = none          */
static int     dp_t = 0;
static int32_t dp_save_x, dp_save_y, dp_save_z, dp_save_rot, dp_save_pitch, dp_save_vy;
static int32_t dp_rot_delta;
/* Where the item is now (its centre), and the spin it has picked up. */
static int32_t item_x, item_y, item_z, item_spin;

static int32_t dp_abs(int32_t v) { return v < 0 ? -v : v; }

/* Ease out (fast then settling), 0..256 - the ends returned BEFORE the divide
   (memory: clamp-before-divide). */
static int32_t dp_ease_out(int32_t t, int32_t len) {
    int32_t inv;
    if (t <= 0)   return 0;
    if (t >= len) return 256;
    inv = 256 - ((t << 8) / len);
    return 256 - ((inv * inv) >> 8);
}
/* Ease in (slow then rushing) - the flight. */
static int32_t dp_ease_in(int32_t t, int32_t len) {
    int32_t lin;
    if (t <= 0)   return 0;
    if (t >= len) return 256;
    lin = (t << 8) / len;
    return (lin * lin) >> 8;
}

void destiny_plinths_arm(void) {
    if (dp_which >= 0) camera_release_player();
    dp_which       = -1;
    dp_t           = 0;
    dp_circle_prev = 1;      /* a Circle carried in through the door is not one */
}

int destiny_plinths_active(void) { return dp_which >= 0; }

static int dp_on_dais(void) { return cam_y < DP_DAIS_EYE_MAX; }

static void dp_start(int which) {
    int32_t d;
    dp_which = which;
    dp_t     = 0;
    dp_save_x = cam_x; dp_save_y = cam_y; dp_save_z = cam_z;
    dp_save_rot = cam_rot; dp_save_pitch = cam_pitch; dp_save_vy = cam_vy;
    camera_anchor_player(cam_x, cam_y, cam_z);
    camera_look_cancel();
    d = ((DP_SHOT_YAW - cam_rot) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    dp_rot_delta = d;
    item_x    = -5600;
    item_y    = plinths[which].rest_cy;
    item_z    = plinths[which].z;
    item_spin = 0;
}

static void dp_finish(void) {
    const Plinth *pl = &plinths[dp_which];
    game_flag_set(pl->taken);
    if (dp_which == 0) {
        /* A weapon pickup auto-equips (the Grave-olver's and the lantern's
           rule). Whatever was in hand is put down first, as a switch would. */
        graveolver_cancel_reload();
        helluminator_cancel_burn();
        player_weapons |= (1 << WEAPON_SHARUR);
        current_weapon  = WEAPON_SHARUR;
        swing_timer     = 0;
        sharur_cancel();
    } else {
        player_items |= (1 << ITEM_TABLET_OF_DESTINIES);
    }
    sound_play(SFX_PICKUP);
    show_pickup_msg(pl->name);
    menu_inventory_sync();

    cam_x = dp_save_x; cam_y = dp_save_y; cam_z = dp_save_z;
    cam_rot = dp_save_rot; cam_pitch = dp_save_pitch; cam_vy = dp_save_vy;
    camera_release_player();
    dp_which       = -1;
    dp_t           = 0;
    dp_circle_prev = 1;
}

void destiny_plinths_update(int lock) {
    if (dp_which < 0) {
        int held = interact_tapped();
        int just = held && !dp_circle_prev;
        int best = -1, i;
        int32_t best_d = DP_REACH;
        dp_circle_prev = held;              /* kept current even while locked */
        if (lock || !just || !dp_on_dais()) return;
        for (i = 0; i < 2; i++) {
            int32_t d;
            if (game_flag(plinths[i].taken)) continue;
            d = dp_abs(cam_x - DP_FACE_X) + dp_abs(cam_z - plinths[i].z);
            if (d < best_d) { best_d = d; best = i; }
        }
        if (best < 0 || !interact_facing(DP_FACE_X, plinths[best].z)) return;
        dp_start(best);
        return;
    }

    dp_t++;
    {
        const Plinth *pl = &plinths[dp_which];
        int32_t e = dp_ease_out(dp_t, DP_GLIDE_FRAMES);
        int32_t t;

        /* The camera: into the shot, then held. */
        cam_x     = dp_save_x     + (((DP_SHOT_X - dp_save_x) * e) >> 8);
        cam_y     = dp_save_y     + (((DP_SHOT_Y - dp_save_y) * e) >> 8);
        cam_z     = dp_save_z     + (((pl->z     - dp_save_z) * e) >> 8);
        cam_rot   = (dp_save_rot  + ((dp_rot_delta * e) >> 8)) & 4095;
        cam_pitch = dp_save_pitch - ((dp_save_pitch * e) >> 8);
        cam_vy    = 0;

        /* The item: still, then up, then hanging (and turning), then at the
           camera. */
        t = dp_t - DP_RISE_AT;
        if (t > 0) item_spin += 12;
        if (t <= DP_RISE_FRAMES) {
            item_y = pl->rest_cy - ((DP_RISE_HEIGHT * dp_ease_out(t, DP_RISE_FRAMES)) >> 8);
        } else {
            t -= DP_RISE_FRAMES + DP_GLOW_FRAMES;
            if (t > 0) {
                int32_t f   = dp_ease_in(t, DP_FLY_FRAMES);
                int32_t sx  = -5600, sy = pl->rest_cy - DP_RISE_HEIGHT, sz = pl->z;
                int32_t ex  = DP_SHOT_X + ((isin(DP_SHOT_YAW) * DP_FLY_STOP) >> 12);
                int32_t ez  = pl->z     + ((icos(DP_SHOT_YAW) * DP_FLY_STOP) >> 12);
                item_x = sx + (((ex - sx) * f) >> 8);
                item_y = sy + (((DP_SHOT_Y - sy) * f) >> 8);
                item_z = sz + (((ez - sz) * f) >> 8);
                if (t >= DP_FLY_FRAMES) dp_finish();
            }
        }
    }
}

/* How bright the ring is at the current beat, 0..256: fading up as the glow
   begins, pulsing through it and the flight. */
static int32_t dp_glow(void) {
    int32_t t = dp_t - DP_RISE_AT - DP_RISE_FRAMES;
    int32_t b;
    if (t <= 0) return 0;
    b = 224 + (isin((dp_t * 40) & 4095) >> 6);
    if (t < DP_GLOW_IN) b = (b * t) / DP_GLOW_IN;
    return b;
}

static void dp_draw_item(RenderContext *ctx, int which, int32_t cx, int32_t cy,
                         int32_t cz, int32_t spin, int32_t glow) {
    if (which == 0) {
        int32_t gain = 4096 + ((glow * 4096) >> 8);
        sharur_draw_world(ctx, cx, cy + SHARUR_HALF, cz, spin, SHARUR_SCALE, gain);
    } else {
        item_pickup_draw_fixed(ctx, PICKUP_TABLET_OF_DESTINIES, cx, cy, cz,
                               TABLET_HALF, ITEM_PICKUP_ROOM_BIAS - 8);
    }
    if (glow > 0)
        sharur_glow_world(ctx, cx, cy, cz, which == 0 ? 80 : 70, glow,
                          SHR_GLOW_RGB);
}

void destiny_plinths_draw(RenderContext *ctx) {
    int i;
    sharur_camera_view();     /* whatever drew before, the view is up now */

    for (i = 0; i < 2; i++) {
        const Plinth *pl = &plinths[i];
        if (game_flag(pl->taken)) continue;

        if (i == dp_which) {
            dp_draw_item(ctx, i, item_x, item_y, item_z, item_spin, dp_glow());
            continue;
        }
        dp_draw_item(ctx, i, -5600, pl->rest_cy, pl->z, 0, 0);

        /* The sign, in free roam only, from the dais only (it is meaningless
           from the floor below the ramp). */
        if (dp_which < 0 && dp_on_dais()) {
            int32_t xz = dp_abs(cam_x - DP_FACE_X) + dp_abs(cam_z - pl->z);
            int fade = 256;
            if (xz >= DP_TEXT_RADIUS) continue;
            if (xz > DP_FADE_NEAR)
                fade = 256 - (((xz - DP_FADE_NEAR) * 256) / (DP_TEXT_RADIUS - DP_FADE_NEAR));
            door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE,
                                    DP_SIGN_X, DP_SIGN_Y, pl->z,
                                    50, 255, 50, fade, DP_SIGN_YAW,
                                    DOOR_PIXEL_SIZE);
        }
    }
}
