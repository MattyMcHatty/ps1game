#include <stdint.h>
#include <psxgpu.h>
#include "render.h"
#include "player.h"
#include "camera.h"
#include "title.h"
#include "debug_opts.h"

int32_t player_health = MAX_HEALTH;
int     game_over     = 0;
int     flash_timer   = 0;
int     damage_timer  = 0;
int     player_keys   = 0;
int     player_items  = 0;
int     game_flags    = 0;
int     player_weapons = (1 << WEAPON_CRUCIFAXE);  /* crucifaxe always owned */
int      player_ammo[MAX_AMMO_TYPES] = {0};
AmmoType graveolver_ammo = AMMO_STANDARD;          /* cylinder starts on lead  */
int      graveolver_loaded = GRAVEOLVER_CAPACITY;  /* cylinder starts loaded   */

/* One row per AmmoType (see player.h). The muzzle-flash colour is the whole
   visual tell for which rounds are chambered, so keep them clearly distinct.
   NOTE the load_msg for AMMO_FLAME says "Fire Rounds" while the item itself is
   called "Flame Rounds" — that is the wording the log line was specified with,
   not a typo. */
const AmmoInfo ammo_info[MAX_AMMO_TYPES] = {
    { "Standard Rounds", "Loaded Standard Rounds", DMG_KINETIC, 255, 255, 255 },
    { "Flame Rounds",    "Loaded Fire Rounds",     DMG_FLAME,   255, 140,  20 },
};

int player_ammo_total(void) {
    int i, total = 0;
    for (i = 0; i < MAX_AMMO_TYPES; i++) total += player_ammo[i];
    return total;
}
int     player_save_count = 0;   /* bumps on every successful save, any slot/card */
int     player_poison_timer = 0; /* frames of spider-web poison left */
WeaponType current_weapon = WEAPON_CRUCIFAXE;

PickupEntry pickup_log[PICKUP_MSG_COUNT] = {{{""},0},{{""},0},{{""},0}};

void player_hurt(int32_t amount) {
    if (debug_opts[DBG_INFINITE_LIFE]) return;
    player_health -= amount;
}

/* Apply (or refresh) the web's poison. Deliberately NOT gated on
   DBG_INFINITE_LIFE: that toggle suppresses health loss, not status effects,
   the same way a zombie still lunges and knocks the player back with it on. */
void player_poison(void) {
    player_poison_timer = POISON_DURATION;
}

void player_status_update(void) {
    if (player_poison_timer > 0) player_poison_timer--;
}

/* Slight green wash over the scene while poisoned — the ambient half of the
   feedback, with the red sprint bar in draw_hud carrying the "no sprint" half.
   Built like the Grave-olver's muzzle flash — semi-transparent full-screen TILE
   added first, DR_TPAGE last so the GPU sets the blend mode before the tile
   (LIFO within one OT node) — but ADDITIVE (abr=1, as the particles use) rather
   than the flash's 50/50 blend. A 50/50 blend halves the scene underneath, which
   over five seconds reads as "the lights went out" rather than "you are
   poisoned"; adding a little green instead tints without darkening, so the room
   stays as navigable as it was. Sits at OT_POISON_TINT, behind the gun flash and
   the HUD but in front of all scene geometry (SCENE_OT_MIN). */
#define OT_POISON_TINT 3
void player_draw_status_overlay(RenderContext *ctx) {
    if (player_poison_timer <= 0) return;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *t = (TILE *)ctx->next_packet;
        setTile(t);
        setSemiTrans(t, 1);
        setRGB0(t, 0, 48, 0);   /* dim: added to every pixel, not blended */
        setXY0(t, 0, 0);
        setWH(t, SCREEN_XRES, SCREEN_YRES);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_POISON_TINT], t);
        ctx->next_packet += sizeof(TILE);
    }
    if (ctx->next_packet + sizeof(DR_TPAGE) <= buf_end) {
        DR_TPAGE *dp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(dp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_POISON_TINT], dp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
}

void show_pickup_msg(const char *item_name) {
    const char *prefix = "Picked up ";
    int i, j;

    /* Shift entries up — oldest (slot 0) is discarded */
    pickup_log[0] = pickup_log[1];
    pickup_log[1] = pickup_log[2];

    /* Build new message in bottom slot */
    i = 0; j = 0;
    while (prefix[i] && i < 63)     { pickup_log[2].msg[i] = prefix[i]; i++; }
    while (item_name[j] && i < 63)  { pickup_log[2].msg[i++] = item_name[j++]; }
    pickup_log[2].msg[i] = '\0';
    pickup_log[2].live   = 1;
}

/* Post a verbatim log line (no "Picked up " prefix) — used for puzzle/status
   messages like "The drawer is locked". Same scrolling slot behaviour. */
void show_pickup_msg_raw(const char *text) {
    int i = 0;
    pickup_log[0] = pickup_log[1];
    pickup_log[1] = pickup_log[2];
    while (text[i] && i < 63) { pickup_log[2].msg[i] = text[i]; i++; }
    pickup_log[2].msg[i] = '\0';
    pickup_log[2].live   = 1;
}

