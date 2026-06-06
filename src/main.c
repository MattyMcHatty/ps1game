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
#include "sml_med.h"
#include "particles.h"
#include "title.h"
#include "collision.h"
#include "crate.h"
#include "key.h"
#include "door.h"
#include "demondog.h"

GameState game_state = STATE_TITLE;
int       debug_mode = 0;

volatile uint8_t pad_buff[2][34];
volatile size_t  pad_buff_len[2];

void poll_cb(uint32_t port, const volatile uint8_t *buff, size_t rx_len) {
    pad_buff_len[port] = rx_len;
    if (rx_len)
        memcpy((void *)pad_buff[port], (void *)buff, rx_len);
}

void reset_game(RenderContext *ctx) {
    cam_x   = 0;
    cam_y   = 0;
    cam_vy  = 0;
    cam_z   = 0;
    cam_rot = 0;
    vampire_x  = 1200;
    vampire_y  = 0;
    vampire_vy = 0;
    vampire_z  = 1200;
    game_over     = 0;
    flash_timer   = 0;
    damage_timer  = 0;
    player_health = MAX_HEALTH;
    swing_timer    = 0;
    vampire_kb_vx    = 0;
    vampire_kb_vz    = 0;
    vampire_health   = VAMPIRE_MAX_HEALTH;
    vampire_hit_timer = 0;
    reset_particles();
    {
        int k;
        for (k = 0; k < PICKUP_MSG_COUNT; k++) pickup_log[k].timer = 0;
    }
    sprint_stamina  = SPRINT_STAMINA_MAX;
    sprint_cooldown = 0;
    door_init();
    crates_reset();
    demon_dogs_reset();
    setRGB0(&ctx->buffers[0].draw_env, 0, 0, 0);
    setRGB0(&ctx->buffers[1].draw_env, 0, 0, 0);
}

int main(int argc, const char **argv) {
    ResetGraph(0);

    RenderContext ctx;
    setup_context(&ctx, SCREEN_XRES, SCREEN_YRES, 25, 0, 29);

    InitGeom();
    gte_SetGeomScreen(256);
    gte_SetGeomOffset(160, 120);
    level_init();
    collision_init();
    floor_zones_init();
    crates_init();
    keys_init();
    sml_meds_init();
    door_init();
    demon_dogs_init();

    SPI_Init(&poll_cb);

    FntLoad(960, 0);
    int gameover_fnt = FntOpen(40,  104, 240, 32, 0, 128);
    int hud_fnt      = FntOpen(4,   16,  120, 16, 0, 64);
    int notify_fnt   = FntOpen(116, 210, 200, 28, 0, 192);
    int debug_fnt    = FntOpen(4,   210, 180, 28, 0, 128);

    for (;;) {
        if (game_state == STATE_TITLE) {
            update_title();
            draw_title(&ctx);
        } else {
            if (!game_over) {
                update_camera();
                apply_collision();
                apply_height();
                /* update_vampire(); */       /* disabled — kept for later */
                /* apply_vampire_height(); */ /* disabled — kept for later */
                update_demon_dogs();
                update_bat();
                update_particles();
                crates_update();
                keys_update();
                sml_meds_update();
                door_update();
                draw_scene(&ctx);
                {
                    int k, any = 0;
                    for (k = 0; k < PICKUP_MSG_COUNT; k++) {
                        if (pickup_log[k].timer > 0) {
                            FntPrint(notify_fnt, "%s\n", pickup_log[k].msg);
                            pickup_log[k].timer--;
                            any = 1;
                        }
                    }
                    if (any) FntFlush(notify_fnt);
                }
                if (debug_mode) {
                    int k;
                    for (k = 0; k < MAX_KEY_TYPES; k++) {
                        if (player_keys & (1 << k))
                            FntPrint(hud_fnt, "%s\n", key_type_names[k]);
                    }
                    FntFlush(hud_fnt);
                    FntPrint(debug_fnt, "X:%d\nY:%d\nZ:%d",
                             cam_x, cam_y, cam_z);
                    FntFlush(debug_fnt);
                }
            } else if (game_over == 2) {
                /* Win screen */
                setRGB0(&ctx.buffers[ctx.active_buffer].draw_env, 0, 30, 0);
                FntPrint(gameover_fnt, "\n\n       YOU ESCAPED!\n\n    PRESS START");
                FntFlush(gameover_fnt);
                if (pad_buff_len[0] &&
                    (~((PadResponse *)pad_buff[0])->btn & PAD_START)) {
                    reset_game(&ctx);
                    game_state = STATE_TITLE;
                }
            } else {
                /* Lose screen */
                uint8_t r;
                if (flash_timer > 0) {
                    flash_timer--;
                    r = (flash_timer / 6) % 2 == 0 ? 200 : 0;
                    setRGB0(&ctx.buffers[ctx.active_buffer].draw_env, r, 0, 0);
                } else {
                    if (pad_buff_len[0] &&
                        (~((PadResponse *)pad_buff[0])->btn & PAD_START)) {
                        reset_game(&ctx);
                        game_state = STATE_TITLE;
                    } else {
                        setRGB0(&ctx.buffers[ctx.active_buffer].draw_env, 80, 0, 0);
                        FntPrint(gameover_fnt, "          GAME OVER\n    PRESS START TO RESTART");
                        FntFlush(gameover_fnt);
                    }
                }
            }
        }
        flip_buffers(&ctx);
    }

    return 0;
}