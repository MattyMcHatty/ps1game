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

DoorState door_state = DOOR_LOCKED;

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];
extern PickupEntry pickup_log[PICKUP_MSG_COUNT];

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
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
    {0x1F,0x10,0x08,0x04,0x02,0x01,0x1F}, /* Z */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x04,0x04,0x04,0x00,0x04,0x00}, /* ! */
    {0x00,0x00,0x0E,0x11,0x1F,0x01,0x0E}, /* a */
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
#define DOOR_PIXEL_SIZE 4           /* text size — half the normal PIXEL_SIZE */
#define CHAR_W  (6 * DOOR_PIXEL_SIZE)   /* 5 pixel cols + 1 gap */

static void door_draw_string_3d(
    RenderContext *ctx,
    const char    *str,
    int32_t        world_x,
    int32_t        world_y,
    int32_t        world_z,
    uint8_t        r, uint8_t g, uint8_t b,
    int             fade_factor  /* 0-256, applied as (color * fade) >> 8 */
) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    /* Apply fade to color */
    r = (uint8_t)((r * fade_factor) >> 8);
    g = (uint8_t)((g * fade_factor) >> 8);
    b = (uint8_t)((b * fade_factor) >> 8);

    int len = 0;
    while (str[len]) len++;

    int32_t start_z = world_z + 200 - (len * CHAR_W) / 2;

    int ci;
    for (ci = 0; ci < len; ci++) {
        const uint8_t *glyph = door_glyphs[char_to_glyph(str[ci])];
        int32_t char_z = start_z + ci * CHAR_W;

        int row, col;
        for (row = 0; row < 7; row++) {
            for (col = 0; col < 5; col++) {
                if (!(glyph[row] & (0x01 << col))) continue;
                if (ctx->next_packet + sizeof(POLY_F4) > buf_end) return;

                int32_t pz = char_z + col * DOOR_PIXEL_SIZE;
                int32_t py = world_y + row * DOOR_PIXEL_SIZE;

                /* Quad on wall: Z varies per char (left-right), Y varies per row (top-bottom), X fixed
                   Flipped winding: v0=TL, v1=BL, v2=TR, v3=BR (CCW from south)          */
                SVECTOR verts[4];
                verts[0].vx = (int16_t)world_x; verts[0].vy = (int16_t)py;                    verts[0].vz = (int16_t)pz;               verts[0].pad = 0;
                verts[1].vx = (int16_t)world_x; verts[1].vy = (int16_t)(py + DOOR_PIXEL_SIZE); verts[1].vz = (int16_t)pz;               verts[1].pad = 0;
                verts[2].vx = (int16_t)world_x; verts[2].vy = (int16_t)py;                    verts[2].vz = (int16_t)(pz + DOOR_PIXEL_SIZE); verts[2].pad = 0;
                verts[3].vx = (int16_t)world_x; verts[3].vy = (int16_t)(py + DOOR_PIXEL_SIZE); verts[3].vz = (int16_t)(pz + DOOR_PIXEL_SIZE); verts[3].pad = 0;

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

void door_update(void) {
    static int x_prev = 0;

    int o_held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        o_held = (~pad->btn & PAD_CIRCLE) ? 1 : 0;
    }
    int o_just_pressed = o_held && !x_prev;
    x_prev = o_held;

    if (!player_near_door()) return;

    if (door_state == DOOR_LOCKED) {
        if (o_just_pressed && (player_keys & (1 << KEY_FRONT_DOOR))) {
            player_keys &= ~(1 << KEY_FRONT_DOOR);
            door_state = DOOR_UNLOCKED;
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
            door_state = DOOR_OPEN;
            game_over  = 2;  /* win */
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
                50, 255, 50, fade_factor);
        } else {
            door_draw_string_3d(ctx,
                "Front Door Key required",
                SIGN_X, SIGN_Y, SIGN_Z,
                255, 50, 50, fade_factor);
        }
    } else if (door_state == DOOR_UNLOCKED) {
        door_draw_string_3d(ctx,
            "Press O to enter",
            SIGN_X, SIGN_Y, SIGN_Z,
            50, 255, 50, fade_factor);
    }
}
