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
#include "crucifaxe.h"
#include "sound.h"
#include "cdaudio.h"
#include "sml_med.h"
#include "particles.h"
#include "title.h"
#include "collision.h"
#include "crate.h"
#include "key.h"
#include "door.h"
#include "demondog.h"
#include "menu.h"

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
    cam_x   = 1600;     /* back against the east wall (x=1800, radius 175) */
    cam_y   = 0;
    cam_vy  = 0;
    cam_z   = 0;        /* centred north-south in the first room */
    cam_rot = 3072;     /* facing west (-X), across the room */
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
    crucifaxe_init();
    sound_init();
    cdaudio_init();

    SPI_Init(&poll_cb);

    FntLoad(960, 0);
    int gameover_fnt = FntOpen(40,  104, 240, 32, 0, 128);
    int hud_fnt      = FntOpen(4,   16,  120, 16, 0, 64);
    int notify_fnt   = FntOpen(116, 210, 200, 28, 0, 192);
    int debug_fnt    = FntOpen(4,   210, 180, 28, 0, 128);
    int compass_fnt  = FntOpen(0,   0,   320, 16, 0, 48);

    /* menu_init opens its own font streams, so call it after FntLoad above. */
    menu_init();

    GameState prev_state = STATE_TITLE;

    for (;;) {
        if (game_state == STATE_TITLE) {
            update_title();
            draw_title(&ctx);
        } else if (game_state == STATE_MENU) {
            /* Game runs fully in background — enemies move, damage applies.
               Player controls are locked by hiding pad input from game systems. */
            if (game_over) {
                game_state = STATE_GAME; /* death closes menu, shows game over */
            } else {
                update_camera();
                apply_collision();
                apply_height();
                update_demon_dogs();
                update_crucifaxe();
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

                menu_update();
                menu_draw(&ctx);
            }
        } else {
            if (!game_over) {
                /* Check for Start to open menu */
                if (pad_buff_len[0]) {
                    PadResponse *pad = (PadResponse *)pad_buff[0];
                    static int start_prev = 1; /* treat Start as already held on first frame, swallows title-screen press */
                    int start_held = (~pad->btn & PAD_START) ? 1 : 0;
                    if (start_held && !start_prev) {
                        game_state = STATE_MENU;
                        menu_open();
                    }
                    start_prev = start_held;
                }

                update_camera();
                apply_collision();
                apply_height();
                /* update_vampire(); */       /* disabled — kept for later */
                /* apply_vampire_height(); */ /* disabled — kept for later */
                update_demon_dogs();
                update_crucifaxe();
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
                    /* scrolling compass tape: 80 chars = 360deg, 10 chars per 45deg
                       tape order N->NE->E->SE->S->SW->W->NW matches CW / increasing cam_rot */
                    {
                        static const char tape[] =
                            "N         NE        E         SE        S         SW        W         NW        ";
                        int rot   = ((cam_rot % 4096) + 4096) % 4096;
                        int pos   = rot * 80 / 4096;
                        int start = ((pos - 20) % 80 + 80) % 80;
                        char cbuf[41];
                        for (k = 0; k < 40; k++)
                            cbuf[k] = tape[(start + k) % 80];
                        cbuf[40] = '\0';
                        FntPrint(compass_fnt, cbuf);
                        FntFlush(compass_fnt);
                    }
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

        /* Handle CD audio state transitions */
        if (prev_state == STATE_TITLE && game_state == STATE_GAME) {
            reset_game(&ctx);   /* apply spawn position/state on a fresh start */
            cdaudio_play(CDAUDIO_MUSIC_TRACK, 1);
        }
        if (prev_state != STATE_TITLE && game_state == STATE_TITLE) {
            cdaudio_stop();
        }
        prev_state = game_state;

        flip_buffers(&ctx);
    }

    return 0;
}