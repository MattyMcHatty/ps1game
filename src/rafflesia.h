#ifndef RAFFLESIA_H
#define RAFFLESIA_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
#include "title.h"   /* GameState — each rafflesia is tagged with its area */

/* -----------------------------------------------------------------------
 * Rafflesia — the garden's stationary carnivorous flower.
 *
 * Rooted like the tentacle (src/tentacle.c is the archetype this copies: one
 * global area-tagged array, no movement, no nav) but with a far longer reach
 * and two attacks instead of one:
 *
 *   MIST   Every RAF_MIST_INTERVAL it exhales a cloud of green spores that
 *          hangs for RAF_MIST_LIFE over a RAF_MIST_RADIUS circle. Standing in
 *          it costs RAF_MIST_DAMAGE once per cloud and applies the player's
 *          poison status — the SAME effect the spider's web carries (half walk
 *          speed, no sprint; see player_poison in player.h).
 *   GRIP   Inside RAF_PULL_RADIUS the flower hauls the player in and, once
 *          RAF_BITE_DELAY has passed, bites for a tenth of MAX_HEALTH per
 *          second. Backing away stops the bite (see the receding test in
 *          update_rafflesias) — the pull is deliberately SLOWER than the
 *          player's walk so retreating really does gain ground.
 *
 * >>> ONLY ONE FLOWER MAY HOLD THE PLAYER AT A TIME. <<< The Outside
 * Catacombs' three beds are close enough that two overlapping pull radii would
 * drag the player back and forth between them forever, so the grip is a single
 * global CLAIM (raf_grip): the first flower to take hold keeps it until the
 * player is clear of its radius, and every other flower leaves them alone.
 *
 * They WAKE at the room's fog distance — g_fog_far, so the range is whatever
 * the room the player is standing in can actually draw — which is far wider
 * than the tentacle's 520 and is the point of them: an avenue of these is
 * hostile from the moment it comes out of the fog.
 *
 * 4 HP, weak to Flame Rounds (double damage). They are NOT permanently
 * killable: nothing about them is persisted, and rafflesias_rest() (called from
 * world_leave, i.e. on every transition AND on every save) puts every one of
 * them back at full health. Leave the bed and come back and it has regrown.
 * ----------------------------------------------------------------------- */

#define MAX_RAFFLESIAS         8
#define RAFFLESIA_MAX_HEALTH   4

typedef struct {
    int32_t   x, y, z;        /* y = floor "standing" reference (as the tentacle) */
    int32_t   health;
    int32_t   active;         /* slot in use                                      */
    int       anim;           /* sprite-swap counter, ticks while awake           */
    int       awake;          /* 1 = player inside the room's fog distance        */
    int       mist_timer;     /* frames until the next cloud is exhaled           */
    int       mist_life;      /* frames of cloud left; 0 = no cloud in the air    */
    int       mist_hit;       /* 1 = this cloud has already landed its damage     */
    int       grip_timer;     /* frames the player has been held (bite wind-up)   */
    int       hauled;         /* 1 = the fast capture is done and the gentle
                                 tether has taken over; latched, see rafflesia.c */
    int       bite_timer;     /* frames until the next point of bite damage       */
    int32_t   last_dist;      /* previous frame's distance — "is the player
                                 backing off?" is the whole of the bite gate      */
    int       hit_timer;      /* health-bar flash after a hit                     */
    GameState area;
} Rafflesia;

extern Rafflesia rafflesias[MAX_RAFFLESIAS];
extern int       rafflesia_count;

/* STARTUP: register both sprite TIMs with texmgr (RAM-resident, NOT uploaded —
   they share the spiders' two VRAM slots, so the upload is a room-entry job).
   See rafflesias_upload_textures. */
void rafflesias_load_assets(void);
void rafflesias_init(void);       /* place every room's rafflesias            */
void rafflesias_reset(void);      /* new game / load: restore the layout      */
void rafflesias_rest(void);       /* room change: full health, asleep, no mist */
void rafflesias_silence(void);    /* stop both looped ambiences + clear latches */
void update_rafflesias(void);
void draw_rafflesias(RenderContext *ctx);

/* Push a mover out of every live flower in the current area — a solid cylinder
   of RAF_BODY_RADIUS plus the `radius` passed in. Called for the PLAYER from
   apply_collision_reception. Without it the grip drags the player inside the
   billboard, where it near-plane clips and becomes both invisible and
   un-aimable; see the note on RAF_BODY_RADIUS in rafflesia.c. */
void rafflesias_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* Stream the two sprites into the spiders' VRAM slots. Pure LoadImage, so it is
   safe on a room transition and ONLY there; main.c pairs it with
   spiders_upload_textures() so whichever of the two enemies the destination
   room actually has is the one in VRAM. */
void rafflesias_upload_textures(void);

/* Set the area's texture window so the sprites can bracket it (they sit at
   Voff >= 128). Pass NULL for areas with no window. */
void rafflesias_set_texwindow(const RECT *tw);

/* Crucifaxe strike: damage the first in-range flower in front of the player.
   Returns 1 if one was hit (no knockback — rafflesias are rooted). */
int  rafflesias_try_hit(void);

/* Grave-olver hitscan support: aim target (sprite-centre Y, half-height and
   half-width) and a shot that deals `amount` HP. */
void rafflesia_body(const Rafflesia *r, int32_t *cyc, int32_t *hh, int32_t *hw);
void rafflesia_shoot(Rafflesia *r, int amount);

/* Scale a hit by this enemy's weaknesses (see damage.h). */
int32_t rafflesia_scale_damage(int32_t base, DamageType type);

#endif
