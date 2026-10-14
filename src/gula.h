#ifndef GULA_H
#define GULA_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
#include "title.h"   /* GameState */

/* -----------------------------------------------------------------------
 * GULA — the Kennels' one-off, the next part of its puzzle.
 *
 * A giant dog-headed matron, almost as tall as the room's four pillars, who
 * appears on the Gula Tablet inlay in the middle of the Kennels once all eight
 * caged demon dogs are dead, and fights from there without moving.
 *
 * ---- The fight ---------------------------------------------------------
 *   WATCH    Every frame in the Kennels until kennels_puzzle_dogs_dead() reads
 *            all eight (src/kennels_puzzle.h), then GULA_APPEAR_DELAY frames.
 *   FADE IN  GULA_FADEIN_FRAMES (3 s), additively from nothing, standing on
 *            the inlay. The HOWL LOOP starts with it — howl, one second of
 *            silence, howl — and runs until she dies. She cannot be hurt
 *            while she fades in, and the door is SEALED from here on.
 *   FIGHT    Rooted. One attack: a DARK BLUE SHOCKWAVE that sweeps from her
 *            feet to the far corners of the room in GULA_WAVE_FRAMES (2 s),
 *            then GULA_WAVE_GAP (2 s) before the next. The first goes out
 *            GULA_WAVE_GAP after she is fully there. It takes
 *            GULA_WAVE_DAMAGE (35% of the bar) from a player it reaches and
 *            SHOVES them away from her until a wall (or a pillar) stops them, and
 *            the ONLY shelter is the shadow of one of the four pillars: the
 *            wave is blocked where the line from her to the player crosses a
 *            pillar's footprint (gula_sheltered). The ring is drawn with the
 *            same gaps, so the shelter can be seen. A health bar rides over
 *            her crown for the whole of this phase, and she and the tablet
 *            tiles under her show through the fog at any range.
 *   DIE      on the hit that takes the last of GULA_MAX_HEALTH. The killing
 *            blow plays her howl once more as a death cry, and
 *            src/gula_death.c plays Adapa's death: the camera turns onto her,
 *            she fades over the howl, and her spirit shoots up out of sight.
 *
 * WEAPONS: all three. The axe and the gun at their plain 1; the Helluminator
 * at 2x (DMG_HOLY 200), i.e. 2 a second of burn. 20 HP is twenty swings or
 * rounds, or ten seconds of lantern.
 *
 * ---- Persistence --------------------------------------------------------
 * ONE SAVED BIT, FLAG_GULA_DEAD, set on the killing blow. Nothing else: no
 * WorldState array and no WorldDelta field. The Kennels' door is sealed from
 * her fade-in to the end of her death, and the Kennels has no save point, so
 * the only ways out of a live fight are winning and dying - and a death reloads
 * a save from before it, where the dogs-dead byte brings her back the next time
 * the player walks in.
 *
 * ---- Textures ------------------------------------------------------------
 * textures/catacombs/gula.png, four frames, one per quarter, read left to
 * right and top row first; she cycles through them for as long as she is
 * drawn. FULL RESOLUTION AND 8bpp, as the brief asked, which is the shape of
 * the CRAWLER's art - so it takes the crawler's two blocks
 * (tools/split_gula_sheet.py):
 *
 *     GULAA  x[320,448) y128   frames 0, 1     CLUT (672,506)
 *     GULAB  x[704,832) y128   frames 2, 3     the same CLUT
 *
 * One palette, 256 colours, STP on every opaque entry (the fades are additive).
 * No crawler is placed in the Kennels; main.c skips crawlers_upload_textures()
 * for that room alone, and every other Chapter 3 room's entry puts the crawler
 * back. STREAMED off the disc on entry by gula_upload_textures() - a 34 KB
 * scratch, freed - because the heap at rest (118 KB, tools/HEAP_BUDGET.txt)
 * had no 64 KB registration to give. Voff 128, so every draw brackets a full
 * texture window (tools/ADDING_AN_ENEMY.txt STEP 4).
 * ----------------------------------------------------------------------- */

#define GULA_MAX_HEALTH        20

/* ---- Where she stands ------------------------------------------------------
   On the 4x4 gula-tablet inlay in the Kennels' floor, x[-400,400]
   z[1400,2200] (src/kennels.h), at its centre. The floor is y=0 throughout. */
#define GULA_X                  0
#define GULA_Z               1800
#define GULA_FLOOR_Y            0

/* ---- Sprite: 466 x 700 world units. "Almost as tall as the pillars": the
   four posts are drawn FLOOR TO CEILING, y 0 to -800, in
   assets/catacombs/Kennels.smx (the collision proxy stops them at -600; that
   is the number kennels.h quotes, and it is the wrong one for anything seen -
   ADDING_AN_ENEMY.txt mistake 1). 700 leaves her crown 100 under the roof. The
   art is square and the figure fills it, feet on the bottom row, so the quad's
   bottom edge sits on the floor. She is drawn a THIRD NARROWER than the art
   (466 against 700): the brief's second pass, height kept, width cut.

   DRAWN AS A 3 x 3 GRID (mistake 13): a 700-unit quad is dropped by the GPU
   at a view depth of 175, which a player held at GULA_HOLD_DIST and turning
   would reach; each 233-unit piece holds out to 58, inside the near plane. */
#define GULA_HALF_W           233
#define GULA_HALF_H           350
#define GULA_CY              (GULA_FLOOR_Y - GULA_HALF_H)   /* body centre, -350 */

/* ---- Her body: a cylinder the player cannot walk into, pushed before the
   walls in apply_collision_reception (lumberers_collide's terms). 180 is the
   apron's half-width in the art (about half the frame), so the push surface is
   where she visibly is. With the wall-like 195 the player is held 375 off her
   centre - outside the GPU's limit above, and close enough for the axe, which
   is measured to this surface (gula_try_hit) rather than to her centre. Cut
   by the same third as the sprite's width: 120, a 315 hold. */
#define GULA_PUSH_RADIUS      120
#define GULA_WALL_LIKE_PUSH   195   /* what apply_collision_reception passes */
#define GULA_PUSH_STEP         60   /* per-frame cap: never through a wall   */

/* ---- Timing, in frames at 60 fps -------------------------------------------- */
#define GULA_APPEAR_DELAY      60   /* 1 s after the last dog falls           */
#define GULA_FADEIN_FRAMES    180   /* "fade in over 3 seconds"               */
#define GULA_ANIM_FRAMES        8   /* game frames per image: 7.5 a second    */

/* >>> THE HOWL'S LENGTH IS howl.vag's. <<< 1330 ADPCM blocks of 28 samples at
   11025 Hz = 3.38 s = 203 frames. The loop re-keys it every
   GULA_HOWL_FRAMES + GULA_HOWL_GAP, which is "howl > 1 second > howl". The
   death fade in src/gula_death.c is the same length, for Adapa's reason. Re-cut
   the clip and these must move with it. */
#define GULA_HOWL_FRAMES      203
#define GULA_HOWL_GAP          60

/* ---- The shockwave -----------------------------------------------------------
   The Lumberer's ring (src/lumberer.c, draw_lmb_shockwave), room-sized and
   dark blue. GULA_WAVE_RADIUS reaches every corner of the Kennels from her
   spot: the farthest floor is the kennels' back corners (+-2389, 4299), 3457
   away. "Sweeps across the entire room" in GULA_WAVE_FRAMES; the next wave
   goes out GULA_WAVE_GAP after this one has finished. */
#define GULA_WAVE_RADIUS     3500
#define GULA_WAVE_FRAMES      120   /* 2 s, ~29 units a frame                 */
#define GULA_WAVE_GAP         120   /* 2 s between waves                      */
#define GULA_WAVE_BAND        200   /* radial width of the band on the floor  */
#define GULA_WAVE_WALL_H      220   /* the skirt on the leading edge          */
#define GULA_WAVE_SEGS         64   /* 3500 * 2pi / 64 = 344 a segment        */

/* The hit window: the player is caught while the leading edge is within this
   much behind them - about seven frames of travel, long enough that the edge
   cannot step OVER a player in one frame, short enough that stepping into a
   shadow just after it passed counts as having been hit. */
#define GULA_WAVE_HIT_BAND    200

/* 35% of the bar - the Lumberer's wave, as asked. */
#define GULA_WAVE_DAMAGE     (MAX_HEALTH * 7 / 20)

/* ---- The shove: a wave that lands carries the player away from her "a good
   distance, all the way to the wall". NOT player_knockback(): its velocity
   decays by a quarter a frame, so a shove that travels ~2500 would have to
   start at ~600 a frame, which steps clean through a wall's 195 push-out and
   out of the room (the walls are front-only - ADDING_AN_ENEMY.txt mistake 12).
   So a constant GULA_SHOVE_STEP a frame along the line from her, resolved by
   the walls between every step, until a wall (or a pillar) stops it - read as
   the player having moved less than half a step - or GULA_SHOVE_FRAMES runs
   out. 40 a frame is the far end of the hall in about a second. */
#define GULA_SHOVE_STEP        40
#define GULA_SHOVE_FRAMES      90

/* ---- The fog. The Kennels' fog-far is 1600, so from the doorway she is past
   it. While she is up - fading in, fighting, dying - she is drawn unfogged and
   unculled at any range, and so are the tablet tiles under her feet (kennels.c
   asks gula_reveal()). Nothing else changes: the room's own view distance is
   untouched, so the poly count is the same plus sixteen floor tiles. */

/* ---- The pillars, the only shelter -------------------------------------------
   The four posts' footprints, read off src/kennels_mesh_collision.c (walls
   8-23): 200 square at (+-700, 900) and (+-700, 2500). */
#define GULA_POST_LO          599
#define GULA_POST_HI          800
#define GULA_POST_Z0_SOUTH    799
#define GULA_POST_Z1_SOUTH    999
#define GULA_POST_Z0_NORTH   2399
#define GULA_POST_Z1_NORTH   2599

typedef enum {
    GULA_GONE = 0,   /* not in this room, or dead                           */
    GULA_WATCH,      /* in the Kennels, waiting for the eighth dog to fall    */
    GULA_WAIT,       /* the beat between the last dog and her fade-in         */
    GULA_FADEIN,     /* materialising: howling, sealed, invulnerable          */
    GULA_FIGHT,      /* the shockwaves                                        */
    GULA_DYING,      /* killed: the director (gula_death.c) owns her now      */
} GulaState;

typedef struct {
    GulaState  state;
    GameState  area;
    int        health;
    int        timer;        /* WAIT / FADEIN countdown, FIGHT's wave gap      */
    int32_t    fade;         /* 256 solid .. 0 gone; < 256 draws additive      */
    uint32_t   anim_tick;
    int        howl_timer;   /* frames to the next howl                        */
    int        wave_t;       /* 0 = no wave; else frames since it went out     */
    int        wave_hit;     /* this wave has already landed                   */
    int        flash;        /* frames of hit tint left                        */
    int        shove_t;      /* frames of shove left; 0 = none                 */
    int32_t    shove_dx, shove_dz;   /* the step, fixed when the wave lands     */
    int32_t    shove_x, shove_z;     /* where the last step put the player      */
} Gula;

extern Gula gula;

/* Room entry: two CD reads (one per half) into the crawler's blocks, through
   one 34 KB scratch that is freed again (see the header note). Called from
   kennels_upload_textures; main.c skips the crawler upload for the Kennels.
   Brackets its own reads with cdaudio_suspend/resume. A no-op once she is
   dead. */
void gula_upload_textures(void);

void gula_reset(void);                 /* new game / load: gone, silent         */
void gula_room_enter(GameState area);  /* every room entry, after world_enter   */

void update_gula(void);
void draw_gula(RenderContext *ctx);    /* the body and the wave                 */
void gula_set_texwindow(const RECT *tw);

/* 1 from her fade-in until her death scene hands the room back: the Kennels'
   door is dead for that span. */
int  gula_seals_door(void);

/* 0..256: how much she lights her own spot through the fog. Her fade while she
   is up (fading in, fighting, dying), 0 before the eighth dog and after she is
   gone. kennels.c draws the inlay under her at this brightness at any range. */
int32_t gula_reveal(void);

/* The player's push-out, from apply_collision_reception, before the walls. */
void gula_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* ---- Weapons: one entry point for all three ------------------------------- */
int     gula_hittable(void);     /* FIGHT in the current area                 */
void    gula_body(int32_t *x, int32_t *cy, int32_t *z, int32_t *hh, int32_t *hw);
void    gula_damage(int dmg);
int32_t gula_scale_damage(int32_t base, DamageType type);   /* 2x DMG_HOLY */
int     gula_try_hit(void);      /* the crucifaxe, measured to her surface    */

/* ---- For the director (src/gula_death.c) only ----------------------------- */
int  gula_dying(void);           /* 1 from the killing blow until gula_gone() */
void gula_set_fade(int32_t f);   /* 256..0, the death fade                    */
void gula_gone(void);            /* the end of the fade: stop drawing her     */

#endif
