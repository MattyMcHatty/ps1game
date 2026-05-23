#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <inline_c.h>
#include "spi.h"
#include "camera.h"
#include "render.h"
#include "level.h"
#include "player.h"
#include "vampire.h"
#include "bat.h"
#include "medipac.h"

volatile uint8_t pad_buff[2][34];
volatile size_t  pad_buff_len[2];

void poll_cb(uint32_t port, const volatile uint8_t *buff, size_t rx_len) {
    pad_buff_len[port] = rx_len;
    if (rx_len)
        memcpy((void *)pad_buff[port], (void *)buff, rx_len);
}

void reset_game(RenderContext *ctx) {
    cam_x   = 0;
    cam_z   = -500;
    cam_rot = 0;
    vampire_x = 1200;
    vampire_z = 1200;
    game_over     = 0;
    flash_timer   = 0;
    damage_timer  = 0;
    player_health = MAX_HEALTH;
    swing_timer    = 0;
    vampire_kb_vx    = 0;
    vampire_kb_vz    = 0;
    vampire_health   = VAMPIRE_MAX_HEALTH;
    reset_medipac();
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
            update_vampire();
            update_bat();
            update_medipac();
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