#ifndef ADAPA_H
#define ADAPA_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
#include "title.h"   /* GameState */

/* -----------------------------------------------------------------------
 * ADAPA — the mini-boss of the Library and the Piano Room.
 *
 * A robed ghost that comes out of the walls up near the ceiling and floats
 * straight down at the player. He is a GHOST in the maggot's sense (src/maggot.h
 * and src/creep.h): no feeler, no wall-follow, no collision — the walls are
 * what he comes THROUGH, so they cannot be what stops him.
 *
 * ---- The fight ---------------------------------------------------------
 *   ARRIVE   at one of his room's spawn points (below), each a point BEHIND a
 *            wall at ceiling height, and SFX_ADAPA plays.
 *   FLOAT    straight at the player in 3D, descending as he comes, until he
 *            is ADP_STOP_DIST from them. In reach he takes ADP_DAMAGE_AMOUNT
 *            every ADP_DAMAGE_TICK frames: 25 hit points a second. He does not
 *            stop for anything except being hurt — walk away and he follows.
 *   HIT      by the Helluminator, and ONLY the Helluminator (one point a
 *            second of burn, at 1x). SFX_ADAPA again, and he fades out where
 *            he is over exactly that clip's length, invulnerable throughout.
 *   WAIT     ADP_RESPAWN_FRAMES (1 s) unseen, then ARRIVE again at a randomly
 *            chosen spawn point.
 *   DIE      on the hit that takes his last point. The camera is taken and
 *            src/adapa_death.c plays the death; see that file.
 *
 * >>> THE AXE AND THE GUN DO NOTHING TO HIM, BY DESIGN. <<< There is no
 * adapas_try_hit() and no Grave-olver loop for him, and both weapon files carry
 * a comment saying so where the block would be. The lantern is the answer.
 *
 * ---- When he appears ----------------------------------------------------
 *   PIANO ROOM  FLAG_PIANO_SOLVED and FLAG_ANZU_SOLVED, and the player has
 *               since LEFT the room once. Not necessarily straight back.
 *   LIBRARY     every enemy in it dead (three spiders and the zombie, world.c),
 *               and the player has since LEFT the room once. STATE_LIBRARY
 *               only — the destroyed Library is a different area and he never
 *               appears there.
 *
 * "Has since left" is a pair of saved flags (player.h, FLAG_ADAPA_*_ARMED) set
 * by adapa_room_enter() when the player arrives somewhere ELSE after
 * update_adapa() saw the room's conditions met while they were standing in it.
 * That watch is deliberately NOT saved: a save made in the room after the last
 * kill and loaded later has not "left the room" in any sense the player would
 * recognise, so they have to walk out and back once more.
 *
 * ---- Persistence --------------------------------------------------------
 * NONE beyond those flags and FLAG_ADAPA_DEAD. There is no WorldState array and
 * no WorldDelta field: leaving the room mid-fight HEALS HIM back to
 * ADP_MAX_HEALTH, as specified, so there is nothing about a live Adapa worth
 * keeping. He is ONE creature, though — one FLAG_ADAPA_DEAD for both rooms, so
 * killing him in either ends him in both.
 *
 * ---- Textures ------------------------------------------------------------
 * textures/adapa.png is four frames, one per quarter, read left to right and
 * top row first; he cycles through them as he floats. It ships as two 4bpp
 * 256x128 halves on ONE master palette (tools/split_adapa_sheet.py):
 *
 *     ADAPAA  x[832,896) y128   frames 0, 1     CLUT (80, 480)
 *     ADAPAB  x[896,960) y128   frames 2, 3     CLUT (96, 480)
 *
 * That is the LUMBERER's second block, which the lumberer itself borrowed from
 * the exit door's two leaves. Neither Chapter 3 nor the Attic Exit / Garden
 * Stairs / Garden Courtyard (the only rooms that open the exit door) is either
 * of his rooms, so nothing he can meet needs those pages. adapas_upload_textures()
 * tells door_anim it has taken them, and main.c's existing restore puts the
 * leaves back on entry to the next room — the lumberer's arrangement exactly.
 * Voff 128, so every draw brackets a full texture window (STEP 4).
 * ----------------------------------------------------------------------- */

#define ADP_MAX_HEALTH          5    /* five seconds of lantern, one per arrival */

/* Sprite: 280 x 280 world units — a robed figure a head taller than the
   zombie's 250 and as broad as he is tall, because the art is square and
   the robes fill the frame. Drawn as a 2 x 2 GRID (hadad.c's construction,
   mistake 13 in tools/ADDING_AN_ENEMY.txt): at ADP_STOP_DIST a single 280-unit
   quad is ~420 px, but a player who walks INTO him (he has no body to stop
   them) takes the view depth toward zero, and the GPU drops — not clips — any
   primitive past 1023 px. Halving each piece halves the depth at which that
   happens to ~35, which is inside the near plane anyway. */
#define ADP_HALF_W            140
#define ADP_HALF_H            140

/* Movement: a straight 3D line at the player, at a constant speed and with no
   heading blend (mistake 16 does not apply — there is no turning to blend).
   7 a frame is 420 a second: the Library's long diagonal (~2600 from the far
   corner) takes about six seconds, which is a fight and not an ambush. */
#define ADP_SPEED               7

/* What he aims his body CENTRE at: a little below the player's eye, so the
   face of the sprite rather than its crown ends up in front of the camera.
   player_y() is the eye (camera.h), and -Y is up, so +30 is lower. */
#define ADP_AIM_BELOW_EYE      30

/* Where he stops: a TRUE 3D radius from that aim point. Outside the 1023 px
   limit with room to spare (see the sprite note) and well inside the reach. */
#define ADP_STOP_DIST         170

/* Contact. "25 hit points of damage per second": 5 every 12 frames. Reach is
   tested horizontally and vertically SEPARATELY (STEP 4's bug, fixed three
   times): Manhattan XZ 260 covers a body stopped on the diagonal of
   ADP_STOP_DIST (170 * sqrt 2 = 240), and 220 vertically covers the whole
   sprite's half-height plus the aim offset. */
#define ADP_CATCH_DIST        260
#define ADP_CATCH_DY          220
#define ADP_DAMAGE_AMOUNT       5
#define ADP_DAMAGE_TICK        12

/* The float cycle: game frames per image. 8 is 7.5 images a second — a slow
   drift of the robes, not a flap. Ticked only on frames he is in FLOAT, so a
   fading or dying body holds its last image. */
#define ADP_ANIM_FRAMES         8

/* >>> THE FADE'S LENGTH IS SFX_ADAPA'S. <<< adapa.vag is 1645 ADPCM blocks of
   28 samples at 11025 Hz = 4.18 s = 251 frames. "Fade away over the course of
   it" means the last frame of him is the last of the sound. Re-cut the clip
   and this must move with it. The death fade in src/adapa_death.c uses the
   same number for the same reason. */
#define ADP_FADE_FRAMES       251

/* "Wait 1 second and appear." */
#define ADP_RESPAWN_FRAMES     60

/* How long after the player walks in before he first arrives. Long enough for
   the door to have closed behind them and the room to be read; short enough
   that walking in IS the trigger. Not in the brief — tune freely. */
#define ADP_ENTRY_FRAMES       90

/* ---- Spawn points -----------------------------------------------------------
   Every one BEHIND A WALL AT CEILING HEIGHT, so he comes out of the room's
   shell above the player's head and descends. All mined from the VISUAL .smx,
   not the collision proxy (mistake 1).

   "Behind" is ADP_SPAWN_BEHIND outside the wall plane — and diagonally out of
   both walls for a corner. 200 is enough that the whole 280-wide sprite starts
   outside the room, so he is first seen passing THROUGH the wall rather than
   standing in front of it.

   Height: the body CENTRE sits ADP_HALF_H / 2 below the drawn ceiling, so the
   top quarter of him starts inside the roof and he reads as coming out of the
   corner of the room rather than hanging below it. */
#define ADP_SPAWN_BEHIND      200

/* THE PIANO ROOM: assets/Piano_Room.smx spans x[-2302,0] z[-740,974], ceiling
   y=-514 throughout. The Anzu frame is on the WEST wall (x=-2302): its panel
   is x[-2288,-2261] z[-253,267] y[-430,-50], centred on z=7 (anzu_puzzle.c's
   AZ_INTERACT_Z). So: the frame's centre, and the west wall's two corners. */
#define ADP_PIANO_WALL_X     (-2302)
#define ADP_PIANO_FRAME_Z        7
#define ADP_PIANO_NORTH_Z      974
#define ADP_PIANO_SOUTH_Z    (-740)
#define ADP_PIANO_CEILING    (-514)

/* THE LIBRARY: the reading room is x[-1780,350] z[-2080,-349], ceiling -730
   (assets/Library.smx's y=-730 plane, and library_init's
   collision_set_ceiling_y). The alcove the player enters by from the East
   Hall is x[-350,350] z[-349,349] under a -500 ceiling and is NOT used: the
   four corners are the reading room's own. The north-east one is where the
   alcove's mouth meets the east wall (x=350 runs the room's whole length), so
   its spawn is out beyond that wall rather than inside the alcove. */
#define ADP_LIB_WEST_X       (-1780)
#define ADP_LIB_EAST_X          350
#define ADP_LIB_NORTH_Z       (-349)
#define ADP_LIB_SOUTH_Z      (-2080)
#define ADP_LIB_CEILING       (-730)

typedef enum {
    ADP_GONE = 0,   /* not in this room at all: dead, unarmed, or elsewhere */
    ADP_WAITING,    /* armed, unseen, counting down to his next arrival     */
    ADP_FLOAT,      /* coming at the player / in reach and hurting them     */
    ADP_FADE,       /* hurt: fading where he is, invulnerable               */
    ADP_DYING,      /* killed: the director (adapa_death.c) owns him now    */
} AdapaState;

typedef struct {
    int32_t     x, y, z;      /* y is the BODY CENTRE, not a floor anchor —
                                 he floats, and nothing here touches a floor */
    int         health;
    int         timer;        /* WAITING / FADE countdown                     */
    int         damage_timer;
    int32_t     fade;         /* 256 solid .. 0 gone; < 256 draws additive    */
    uint32_t    anim_tick;
    int         spawn_idx;    /* the point he last arrived at                 */
    AdapaState  state;
    GameState   area;
} Adapa;

extern Adapa adapa;

/* Startup: register the two halves (TEXBANK_MANSION). */
void adapas_load_textures(void);
/* Room entry: pure LoadImage out of the RAM copy. main.c's STATE_LOADING calls
   it on entry to his two rooms, after door_anim_restore_panels(). */
void adapas_upload_textures(void);

void adapas_init(void);
void adapa_reset(void);   /* new game / load: no Adapa, no watch, no sound */

/* Every room entry, AFTER world_enter() (the Library's enemies are in the live
   arrays by then). Settles "has the player left the room since its conditions
   were met" and parks him for the room being entered. Does NOT decide whether
   he appears — on a title-screen load the flags are not restored until after
   this runs, so that is update_adapa()'s job, when the entry delay runs out. */
void adapa_room_enter(GameState area);

void update_adapa(void);
void draw_adapa(RenderContext *ctx);

/* Tell the renderer which texture window the room has active (Voff 128). */
void adapas_set_texwindow(const RECT *tw);

/* The Helluminator's door, and the only one. 1 if a burn this tick can land
   on him at all (he is FLOATING in the current area); then adapa_body() is
   where to aim. */
int     adapa_burnable(void);
void    adapa_body(int32_t *x, int32_t *y, int32_t *z);
void    adapa_burn(int dmg);

/* 1x from holy fire, explicitly. Nothing else reaches him. */
int32_t adapa_scale_damage(int32_t base, DamageType type);

/* ---- For the director (src/adapa_death.c) only --------------------------
   The killing blow sets ADP_DYING and FLAG_ADAPA_DEAD; the director then owns
   the body through two calls and one field: */
int  adapa_dying(void);          /* 1 from the killing blow until adapa_gone() */
void adapa_set_fade(int32_t f);  /* 256..0, the death fade                     */
void adapa_gone(void);           /* the end of the fade: stop drawing him      */

#endif
