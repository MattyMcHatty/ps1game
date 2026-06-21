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
#include "delivery_area.h"
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
#include "dining_table.h"
#include "key.h"
#include "door.h"
#include "demondog.h"
#include "zombie.h"
#include "menu.h"
#include "kitchen_dining.h"
#include "world.h"
#include "fatdoor.h"
#include "door_anim.h"

GameState game_state   = STATE_TITLE;
GameState current_area = STATE_DELIVERY_AREA;  /* last playable area; menu returns here */
GameState pending_area = STATE_KITCHEN_DINING; /* area STATE_LOADING will switch to */
int       debug_mode   = 0;

/* HUD/debug font streams (opened in main() after FntLoad). */
static int gameover_fnt, hud_fnt, notify_fnt, debug_fnt, compass_fnt;

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
    zombies_reset();
    fatdoors_reset();
    setRGB0(&ctx->buffers[0].draw_env, 0, 0, 0);
    setRGB0(&ctx->buffers[1].draw_env, 0, 0, 0);
}

/* ---- Shared per-frame helpers, used by every playable area ---- */

/* Open the inventory menu on a fresh Start press, remembering the area. */
static void handle_menu_open(void) {
    if (!pad_buff_len[0]) return;
    PadResponse *pad = (PadResponse *)pad_buff[0];
    static int start_prev = 1; /* held on the first frame swallows the title-screen press */
    int start_held = (~pad->btn & PAD_START) ? 1 : 0;
    if (start_held && !start_prev) {
        current_area = game_state;
        menu_open();
        game_state = STATE_MENU;
    }
    start_prev = start_held;
}

/* Advance one area: player movement + the area's own geometry/entities,
   then the shared weapon and particle systems. */
static void update_current_area(GameState area) {
    update_camera();
    if (area == STATE_KITCHEN_DINING) {
        apply_collision_kitchen_dining();
        apply_height();
        update_zombies();
        sml_meds_update();
        if (kitchen_door_triggered()) {
            pending_area = STATE_DELIVERY_AREA;
            door_anim_start();
            game_state   = STATE_DOOR_ANIM;
        }
    } else {
        apply_collision();
        apply_height();
        update_demon_dogs();
        update_zombies();
        crates_update();
        keys_update();
        sml_meds_update();
        door_update();
    }
    update_crucifaxe();
    update_particles();
}

/* Draw an area's world + entities only (player overlays come separately). */
static void draw_current_area(RenderContext *ctx, GameState area) {
    if (area == STATE_KITCHEN_DINING)
        kitchen_dining_draw(ctx);
    else
        delivery_area_draw(ctx);
}

/* Player overlays shared by every area: blood particles, the swingable
   weapon, the health/stamina HUD, and the debug collision view. */
static void draw_player_systems(RenderContext *ctx) {
    draw_particles(ctx);
    draw_crucifaxe(ctx);
    draw_hud(ctx);
#ifdef DEBUG_COLLISION
    debug_draw_walls(ctx);
    debug_draw_coords(ctx);
#endif
}

/* On-screen item pickup notifications. */
static void draw_pickup_messages(void) {
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

/* Debug overlay: held items, coordinates, and the scrolling compass. */
static void draw_debug_overlay(void) {
    if (!debug_mode) return;
    int k;

    for (k = 0; k < MAX_KEY_TYPES; k++)
        if (player_keys & (1 << k))
            FntPrint(hud_fnt, "%s\n", key_type_names[k]);
    FntFlush(hud_fnt);

    FntPrint(debug_fnt, "X:%d\nY:%d\nZ:%d", cam_x, cam_y, cam_z);
    FntFlush(debug_fnt);

    /* Scrolling compass tape: 80 chars = 360deg, 10 chars per 45deg,
       order N->NE->E->SE->S->SW->W->NW matches increasing cam_rot (CW). */
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

/* Game-over / restart screen (area-agnostic). */
static void draw_lose_screen(RenderContext *ctx) {
    if (flash_timer > 0) {
        flash_timer--;
        uint8_t r = (flash_timer / 6) % 2 == 0 ? 200 : 0;
        setRGB0(&ctx->buffers[ctx->active_buffer].draw_env, r, 0, 0);
    } else if (pad_buff_len[0] &&
               (~((PadResponse *)pad_buff[0])->btn & PAD_START)) {
        reset_game(ctx);
        game_state = STATE_TITLE;
    } else {
        setRGB0(&ctx->buffers[ctx->active_buffer].draw_env, 80, 0, 0);
        FntPrint(gameover_fnt, "          GAME OVER\n    PRESS START TO RESTART");
        FntFlush(gameover_fnt);
    }
}

int main(int argc, const char **argv) {
    ResetGraph(0);

    RenderContext ctx;
    setup_context(&ctx, SCREEN_XRES, SCREEN_YRES, 25, 0, 29);

    InitGeom();
    gte_SetGeomScreen(256);
    gte_SetGeomOffset(160, 120);

    SPI_Init(&poll_cb);

    /* Show loading screen while CD assets initialise.
       Two flips fill both double-buffer framebuffers before blocking reads begin. */
    draw_loading_screen(&ctx);
    flip_buffers(&ctx);
    draw_loading_screen(&ctx);
    flip_buffers(&ctx);

    delivery_area_init();
    kitchen_load_assets();     /* load kitchen textures + geometry at startup —
                                  LoadImage is only safe before the main render
                                  loop, and this keeps gameplay CD-read-free */
    fatdoors_load_assets();    /* kitchen entryway doors (texture + geometry) */
    fatdoors_init();
    door_anim_load_assets();   /* level-transition door panel (texture) */
    collision_init();
    floor_zones_init();
    crates_init();
    dining_tables_init();      /* static kitchen props (loads DINTABLE.SMD) */
    keys_init();
    sml_meds_init();
    door_init();
    demon_dogs_init();
    zombies_load_textures();   /* LoadImage at startup only (see TEXTURING_NOTES) */
    zombies_init();            /* capture spawn defaults (none placed yet) */
    crucifaxe_init();
    sound_init();
    cdaudio_init();

    FntLoad(960, 0);
    gameover_fnt = FntOpen(40,  104, 240, 32, 0, 128);
    hud_fnt      = FntOpen(4,   16,  120, 16, 0, 64);
    notify_fnt   = FntOpen(116, 210, 200, 28, 0, 192);
    debug_fnt    = FntOpen(4,   210, 180, 28, 0, 128);
    compass_fnt  = FntOpen(0,   0,   320, 16, 0, 48);

    /* menu_init and title_init open their own font streams, so call them
       after FntLoad above. */
    menu_init();
    title_init();

    GameState prev_state = STATE_TITLE;

    for (;;) {
        if (game_state == STATE_TITLE) {
            update_title();
            draw_title(&ctx);
        } else if (game_state == STATE_MENU) {
            /* The area keeps running in the background (enemies move, gravity
               applies); update_camera and weapon input self-disable while in
               STATE_MENU, so the player is effectively frozen. */
            if (game_over) {
                game_state = current_area; /* death closes menu, shows game over */
            } else {
                update_current_area(current_area);
                draw_current_area(&ctx, current_area);
                draw_player_systems(&ctx);
                draw_pickup_messages();
                menu_update();
                menu_draw(&ctx);
            }
        } else if (game_state == STATE_LOADING) {
            /* Switch areas. All assets are resident (loaded at startup), so this
               does no CD reads — the music keeps playing and there's no real load
               delay. The door animation already faded to black, so we just hold a
               black screen for this one-frame switch (no LOADING screen). */
            {
                TILE *bg = (TILE *)ctx.next_packet;
                setTile(bg);
                setXY0(bg, 0, 0);
                setWH(bg, SCREEN_XRES, SCREEN_YRES);
                setRGB0(bg, 0, 0, 0);
                addPrim(&ctx.buffers[ctx.active_buffer].ot[OT_LENGTH - 1], bg);
                ctx.next_packet += sizeof(TILE);
            }
            /* Snapshot the room we're leaving so its progress (defeated enemies,
               smashed crates, collected pickups, door state) persists. */
            world_leave(current_area);
            if (pending_area == STATE_KITCHEN_DINING) {
                kitchen_dining_init();
                kitchen_door_arm();
            } else {
                /* Return to the delivery area: restore its collision/floor and
                   place the player just inside the front door, facing in, armed
                   so a held Circle won't bounce them straight back. */
                collision_init();
                floor_zones_init();
                cam_x   = DOOR_X + 150;   /* delivery side of the door */
                cam_y   = DOOR_Y;         /* upper-floor eye level */
                cam_vy  = 0;
                cam_z   = DOOR_Z;
                cam_rot = 1024;           /* face +X, into the delivery area */
                door_arm();
            }
            /* Restore the entered room's entities into the live arrays. */
            world_enter(pending_area);
            current_area = pending_area;
            game_state   = pending_area;
        } else if (game_state == STATE_DOOR_ANIM) {
            /* RE-style door transition: a black screen with the door swinging
               open, then a fade to black. Draws nothing of the live room — when
               it finishes we hand off to STATE_LOADING, which switches areas. */
            door_anim_update();
            door_anim_draw(&ctx);
            if (door_anim_finished())
                game_state = STATE_LOADING;
        } else if (game_state == STATE_DELIVERY_AREA ||
                   game_state == STATE_KITCHEN_DINING) {
            if (game_over) {
                draw_lose_screen(&ctx);
            } else {
                /* Capture the area first: handle_menu_open may switch game_state
                   to STATE_MENU mid-frame, but the rest of this frame must keep
                   using the real area so the correct collision runs (otherwise
                   the kitchen would fall through to delivery-area collision and
                   its back-face push would catapult the player). */
                GameState area = game_state;
                handle_menu_open();
                update_current_area(area);
                draw_current_area(&ctx, area);
                draw_player_systems(&ctx);
                draw_pickup_messages();
                draw_debug_overlay();
            }
        }

        /* CD-DA music: start once when leaving the title for gameplay, stop
           when returning to the title. In-game area transitions never touch it
           (all assets are resident, so no CD read competes with playback). */
        if (prev_state == STATE_TITLE && game_state != STATE_TITLE) {
            if (game_state == STATE_DELIVERY_AREA)
                reset_game(&ctx);   /* fresh-start spawn/state */
            world_new_game();       /* reset rooms; capture the fresh starting room */
            cdaudio_play(CDAUDIO_MUSIC_TRACK, 1);
        }
        if (prev_state != STATE_TITLE && game_state == STATE_TITLE) {
            cdaudio_stop();
        }
        cdaudio_update();
        prev_state = game_state;

        flip_buffers(&ctx);
    }

    return 0;
}