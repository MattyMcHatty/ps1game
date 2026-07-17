#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "key.h"
#include "door.h"
#include "door_anim.h"
#include "sound.h"
#include "title.h"

DoorState door_state = DOOR_LOCKED;

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];
extern PickupEntry pickup_log[PICKUP_MSG_COUNT];

/* Circle edge-detect state, file-scope so door_arm() can seed it. Starts
   "held" so a press carried in from another screen isn't seen as fresh. */
static int door_circle_prev = 1;

/* -----------------------------------------------------------------------
 * 5x7 pixel font — same bitmask format as the old HUD font.
 * Index 0-9: digits, 10-35: A-Z, 36: space, 37: !, 38-63: a-z,
 * 64: -, 65: :, 66: ., 67: '
 * ----------------------------------------------------------------------- */
static const uint8_t door_glyphs[][7] = {
    {0x1F,0x11,0x11,0x11,0x11,0x11,0x1F}, /* 0 */
    {0x04,0x06,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x1F,0x10,0x10,0x1F,0x01,0x01,0x1F}, /* 2 */
    {0x1F,0x10,0x10,0x1F,0x10,0x10,0x1F}, /* 3 */
    {0x11,0x11,0x11,0x1F,0x10,0x10,0x10}, /* 4 */
    {0x1F,0x01,0x01,0x1F,0x10,0x10,0x1F}, /* 5 */
    {0x1F,0x01,0x01,0x1F,0x11,0x11,0x1F}, /* 6 */
    {0x1F,0x10,0x10,0x10,0x10,0x10,0x10}, /* 7 */
    {0x1F,0x11,0x11,0x1F,0x11,0x11,0x1F}, /* 8 */
    {0x1F,0x11,0x11,0x1F,0x10,0x10,0x1F}, /* 9 */
    {0x1F,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
    {0x1F,0x11,0x11,0x1F,0x11,0x11,0x1F}, /* B */
    {0x0F,0x11,0x10,0x10,0x10,0x11,0x0F}, /* C */
    {0x1F,0x11,0x11,0x11,0x11,0x11,0x1F}, /* D */
    {0x1F,0x01,0x01,0x1F,0x01,0x01,0x1F}, /* E */
    {0x1F,0x01,0x01,0x0F,0x01,0x01,0x01}, /* F */
    {0x0F,0x10,0x10,0x17,0x11,0x11,0x0F}, /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
    {0x1C,0x08,0x08,0x08,0x08,0x09,0x06}, /* J */
    {0x11,0x09,0x05,0x03,0x05,0x09,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
    {0x11,0x1B,0x15,0x11,0x11,0x11,0x11}, /* M */
    {0x11,0x13,0x15,0x19,0x11,0x11,0x11}, /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
    {0x1F,0x11,0x11,0x1F,0x01,0x01,0x01}, /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x09,0x16}, /* Q */
    {0x1F,0x11,0x11,0x1F,0x05,0x09,0x11}, /* R */
    {0x1E,0x01,0x01,0x0E,0x10,0x10,0x0F}, /* S (LSB-first, matches the rest) */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
    {0x1F,0x10,0x08,0x04,0x02,0x01,0x1F}, /* Z */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x04,0x04,0x04,0x00,0x04,0x00}, /* ! */
    {0x00,0x00,0x0E,0x10,0x1E,0x11,0x1E}, /* a (LSB-first, matches the rest) */
    {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}, /* b */
    {0x00,0x00,0x0E,0x10,0x10,0x11,0x0E}, /* c */
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}, /* d */
    {0x00,0x00,0x0E,0x11,0x1F,0x01,0x0E}, /* e */
    {0x0C,0x12,0x02,0x07,0x02,0x02,0x02}, /* f */
    {0x00,0x00,0x0E,0x11,0x11,0x1E,0x10}, /* g */
    {0x01,0x01,0x0F,0x11,0x11,0x11,0x11}, /* h */
    {0x04,0x00,0x06,0x04,0x04,0x04,0x0E}, /* i */
    {0x08,0x00,0x0C,0x08,0x08,0x09,0x06}, /* j */
    {0x01,0x01,0x09,0x05,0x03,0x05,0x09}, /* k */
    {0x06,0x04,0x04,0x04,0x04,0x04,0x0E}, /* l */
    {0x00,0x00,0x0B,0x15,0x15,0x11,0x11}, /* m */
    {0x00,0x00,0x0F,0x11,0x11,0x11,0x11}, /* n */
    {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, /* o */
    {0x00,0x00,0x0F,0x11,0x11,0x0F,0x01}, /* p */
    {0x00,0x00,0x1E,0x11,0x11,0x1E,0x10}, /* q */
    {0x00,0x00,0x0D,0x13,0x01,0x01,0x01}, /* r */
    {0x00,0x00,0x0E,0x01,0x0E,0x10,0x0F}, /* s */
    {0x02,0x02,0x0F,0x02,0x02,0x12,0x0C}, /* t */
    {0x00,0x00,0x11,0x11,0x11,0x19,0x16}, /* u */
    {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, /* v */
    {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, /* w */
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, /* x */
    {0x00,0x00,0x11,0x11,0x1E,0x10,0x0E}, /* y */
    {0x00,0x00,0x1F,0x08,0x04,0x02,0x1F}, /* z */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* - */
    {0x00,0x04,0x04,0x00,0x04,0x04,0x00}, /* : */
    {0x00,0x00,0x00,0x00,0x00,0x01,0x00}, /* . */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x04}, /* ' */
};

static int char_to_glyph(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c == ' ')  return 36;
    if (c == '!')  return 37;
    if (c >= 'a' && c <= 'z') return c - 'a' + 38;
    if (c == '-')  return 64;
    if (c == ':')  return 65;
    if (c == '.')  return 66;
    if (c == '\'') return 67;
    return 36;
}

/* -----------------------------------------------------------------------
 * Draw a string as world-space pixel quads on the wall (YZ plane at fixed X).
 * Text extends left-to-right in Z, and top-to-bottom in Y.
 * ----------------------------------------------------------------------- */
void door_draw_string_3d(
    RenderContext *ctx,
    const char    *str,
    int32_t        world_x,
    int32_t        world_y,
    int32_t        world_z,
    uint8_t        r, uint8_t g, uint8_t b,
    int             fade_factor, /* 0-256, applied as (color * fade) >> 8 */
    int             mirror,      /* 1 = flip horizontally for viewing from the far side */
    TextPlane       plane,       /* TEXT_PLANE_YZ (fixed X) or TEXT_PLANE_XY (fixed Z) */
    int             pixel        /* world units per font pixel (DOOR_PIXEL_SIZE = default) */
) {
    int char_w = 6 * pixel;   /* 5 pixel cols + 1 gap */
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    /* Apply fade to color */
    r = (uint8_t)((r * fade_factor) >> 8);
    g = (uint8_t)((g * fade_factor) >> 8);
    b = (uint8_t)((b * fade_factor) >> 8);

    int len = 0;
    while (str[len]) len++;

    /* The reading axis is Z (YZ plane) or X (XY plane); centre on that world
       coord. The +200 nudge matches the door-sign callers (which pass coord-200). */
    int32_t read_coord = (plane == TEXT_PLANE_XY) ? world_x : world_z;
    int32_t read_start = read_coord + 200 - (len * char_w) / 2;

    int ci;
    for (ci = 0; ci < len; ci++) {
        /* When mirrored, reverse character order along the reading axis so the
           text reads correctly from the opposite side of the plane. */
        int eff_ci = mirror ? (len - 1 - ci) : ci;
        const uint8_t *glyph = door_glyphs[char_to_glyph(str[ci])];
        int32_t char_read = read_start + eff_ci * char_w;

        int row, col;
        for (row = 0; row < 7; row++) {
            for (col = 0; col < 5; col++) {
                if (!(glyph[row] & (0x01 << col))) continue;
                if (ctx->next_packet + sizeof(POLY_F4) > buf_end) return;

                /* Mirror flips columns within each glyph too. */
                int eff_col = mirror ? (4 - col) : col;
                int32_t pr = char_read + eff_col * pixel; /* reading-axis pos */
                int32_t py = world_y + row * pixel;

                /* Quad on the wall: the reading axis varies per char/col
                   (left-right), Y varies per row (top-bottom), the third axis is
                   fixed. v0=TL, v1=BL, v2=TR, v3=BR. */
                SVECTOR verts[4];
                if (plane == TEXT_PLANE_XY) {
                    /* Fixed Z; reading axis is X (90deg from the YZ door signs). */
                    verts[0].vx = (int16_t)pr;             verts[0].vy = (int16_t)py;             verts[0].vz = (int16_t)world_z; verts[0].pad = 0;
                    verts[1].vx = (int16_t)pr;             verts[1].vy = (int16_t)(py + pixel); verts[1].vz = (int16_t)world_z; verts[1].pad = 0;
                    verts[2].vx = (int16_t)(pr + pixel); verts[2].vy = (int16_t)py;             verts[2].vz = (int16_t)world_z; verts[2].pad = 0;
                    verts[3].vx = (int16_t)(pr + pixel); verts[3].vy = (int16_t)(py + pixel); verts[3].vz = (int16_t)world_z; verts[3].pad = 0;
                } else {
                    /* Fixed X; reading axis is Z (door signs). */
                    verts[0].vx = (int16_t)world_x; verts[0].vy = (int16_t)py;             verts[0].vz = (int16_t)pr;             verts[0].pad = 0;
                    verts[1].vx = (int16_t)world_x; verts[1].vy = (int16_t)(py + pixel); verts[1].vz = (int16_t)pr;             verts[1].pad = 0;
                    verts[2].vx = (int16_t)world_x; verts[2].vy = (int16_t)py;             verts[2].vz = (int16_t)(pr + pixel); verts[2].pad = 0;
                    verts[3].vx = (int16_t)world_x; verts[3].vy = (int16_t)(py + pixel); verts[3].vz = (int16_t)(pr + pixel); verts[3].pad = 0;
                }

                DVECTOR sv[4];
                int32_t otz;

                gte_ldv3(&verts[0], &verts[1], &verts[2]);
                gte_rtpt();
                gte_stsxy3c(sv);

                gte_ldv0(&verts[3]);
                gte_rtps();
                gte_stsxy(&sv[3]);
                gte_avsz4();
                gte_stotz(&otz);

                if (otz <= 0 || otz >= OT_LENGTH) continue;

                POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
                setPolyF4(poly);
                setRGB0(poly, r, g, b);
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_F4);
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * Camera-facing (billboard) variant: draws the string as pixel quads centred on
 * (wx,wy,wz), always facing the player. Text extends along the camera's world
 * right axis and downward in Y. Used for free-standing labels (e.g. the save
 * point) where a fixed wall plane would sit edge-on from some angles. The caller
 * must have the camera view matrix loaded in the GTE.
 * ----------------------------------------------------------------------- */
void door_draw_string_billboard(
    RenderContext *ctx, const char *str,
    int32_t wx, int32_t wy, int32_t wz,
    uint8_t r, uint8_t g, uint8_t b,
    int fade_factor, int pixel
) {
    int32_t rx = icos(cam_rot), rz = -isin(cam_rot);   /* world screen-right */
    int      char_w = 6 * pixel;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    r = (uint8_t)((r * fade_factor) >> 8);
    g = (uint8_t)((g * fade_factor) >> 8);
    b = (uint8_t)((b * fade_factor) >> 8);

    int len = 0;
    while (str[len]) len++;

    int32_t start_h = -(len * char_w) / 2;          /* centre the line horizontally */
    int32_t hx = (rx * pixel) >> 12, hz = (rz * pixel) >> 12;  /* one pixel right */

    int ci;
    for (ci = 0; ci < len; ci++) {
        const uint8_t *glyph = door_glyphs[char_to_glyph(str[ci])];
        int32_t char_h = start_h + ci * char_w;
        int row, col;
        for (row = 0; row < 7; row++) {
            for (col = 0; col < 5; col++) {
                if (!(glyph[row] & (0x01 << col))) continue;   /* same bit order as door_draw_string_3d */
                if (ctx->next_packet + sizeof(POLY_F4) > buf_end) return;

                int32_t h  = char_h + col * pixel;
                int32_t bx = wx + ((rx * h) >> 12);
                int32_t bz = wz + ((rz * h) >> 12);
                int32_t by = wy + row * pixel;

                SVECTOR verts[4];
                verts[0].vx = (int16_t)bx;      verts[0].vy = (int16_t)by;           verts[0].vz = (int16_t)bz;      verts[0].pad = 0;
                verts[1].vx = (int16_t)bx;      verts[1].vy = (int16_t)(by + pixel);  verts[1].vz = (int16_t)bz;      verts[1].pad = 0;
                verts[2].vx = (int16_t)(bx+hx); verts[2].vy = (int16_t)by;           verts[2].vz = (int16_t)(bz+hz); verts[2].pad = 0;
                verts[3].vx = (int16_t)(bx+hx); verts[3].vy = (int16_t)(by + pixel);  verts[3].vz = (int16_t)(bz+hz); verts[3].pad = 0;

                DVECTOR sv[4];
                int32_t otz;
                gte_ldv3(&verts[0], &verts[1], &verts[2]);
                gte_rtpt();
                gte_stsxy3c(sv);
                gte_ldv0(&verts[3]);
                gte_rtps();
                gte_stsxy(&sv[3]);
                gte_avsz4();
                gte_stotz(&otz);
                if (otz <= 0 || otz >= OT_LENGTH) continue;

                POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
                setPolyF4(poly);
                setRGB0(poly, r, g, b);
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_F4);
            }
        }
    }
}

/* ----------------------------------------------------------------------- */

static int player_sees_door_text(void) {
    int32_t dx   = cam_x - DOOR_X;
    int32_t dz   = cam_z - DOOR_Z;
    int32_t xz_dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz_dist < DOOR_TEXT_RADIUS;
}

static int player_near_door(void) {
    int32_t dx   = cam_x - DOOR_X;
    int32_t dz   = cam_z - DOOR_Z;
    int32_t dy   = cam_y - DOOR_Y;
    int32_t xz_dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int32_t y_dist  = (dy < 0 ? -dy : dy);
    return xz_dist < DOOR_TRIGGER_RADIUS && y_dist < DOOR_Y_TOLERANCE;
}

void door_init(void) {
    door_state = DOOR_LOCKED;
}

/* Seed the Circle edge state to the current button so a press held from the
   previous screen/door doesn't immediately re-trigger this door on arrival. */
void door_arm(void) {
    int held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    door_circle_prev = held;
}

void door_update(void) {
    int o_held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        o_held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    int o_just_pressed = o_held && !door_circle_prev;
    door_circle_prev = o_held;

    if (!player_near_door()) return;

    if (door_state == DOOR_LOCKED) {
        if (o_just_pressed && (player_keys & (1 << KEY_FRONT_DOOR))) {
            player_keys &= ~(1 << KEY_FRONT_DOOR);
            door_state = DOOR_UNLOCKED;
            sound_play(SFX_UNLOCK);
            /* Add message to log without "Picked up " prefix */
            pickup_log[0] = pickup_log[1];
            pickup_log[1] = pickup_log[2];
            int i = 0;
            const char *msg = "Used Front Door Key";
            while (msg[i] && i < 63) { pickup_log[2].msg[i] = msg[i]; i++; }
            pickup_log[2].msg[i] = '\0';
            pickup_log[2].timer = PICKUP_MSG_DURATION;
        }
    } else if (door_state == DOOR_UNLOCKED) {
        if (o_just_pressed) {
            /* Stay UNLOCKED so the door is reusable for repeated trips. Play the
               door-opening transition; it hands off to STATE_LOADING when done. */
            pending_area = STATE_KITCHEN_DINING;
            door_anim_start(DOOR_PANEL_OUTER);
            game_state   = STATE_DOOR_ANIM;
        }
    }
}

void door_draw(RenderContext *ctx) {
    if (!player_sees_door_text()) return;

    /* Rebuild view matrix — same pattern as crates_draw */
    MATRIX view;
    SVECTOR neg_rot = {0, -cam_rot, 0, 0};
    RotMatrix(&neg_rot, &view);
    VECTOR vt = {-cam_x, -cam_y, -cam_z};
    ApplyMatrixLV(&view, &vt, &vt);
    view.t[0] = vt.vx;
    view.t[1] = vt.vy;
    view.t[2] = vt.vz;
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);

    /* Calculate fade factor based on XZ distance: fully visible at 1000, fades from 1000-1500 */
    int32_t dx   = cam_x - DOOR_X;
    int32_t dz   = cam_z - DOOR_Z;
    int32_t xz_dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade_factor = 256;  /* fully opaque by default */
    if (xz_dist > 1000) {
        int fade_range = 1500 - 1000;
        int fade_progress = xz_dist - 1000;
        if (fade_progress > fade_range) fade_progress = fade_range;
        fade_factor = 256 - ((fade_progress * 256) / fade_range);
    }

    if (door_state == DOOR_LOCKED) {
        if (player_keys & (1 << KEY_FRONT_DOOR)) {
            door_draw_string_3d(ctx,
                "Press O to unlock",
                SIGN_X, SIGN_Y, SIGN_Z,
                50, 255, 50, fade_factor, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
        } else {
            door_draw_string_3d(ctx,
                "Front Door Key required",
                SIGN_X, SIGN_Y, SIGN_Z,
                255, 50, 50, fade_factor, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
        }
    } else if (door_state == DOOR_UNLOCKED) {
        door_draw_string_3d(ctx,
            "Press O to enter",
            SIGN_X, SIGN_Y, SIGN_Z,
            50, 255, 50, fade_factor, 0, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
    }
}
