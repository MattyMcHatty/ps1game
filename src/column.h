#ifndef COLUMN_H
#define COLUMN_H

#include <stdint.h>
#include "render.h"
#include "damage.h"
#include "title.h"         /* GameState - each Column is tagged with its area */

/* -----------------------------------------------------------------------
 * COLUMN - a stone column with a purple star orb set in its middle, and the
 * Underworld's fourth enemy. Most of them are exactly what they look like.
 *
 * >>> IT IS NOT A BILLBOARD. <<< It does not turn to face the camera the way
 * every other enemy does: it stands in the world like a piece of level wall,
 * in a fixed vertical plane, and it is DOUBLE-SIDED (no backface cull), so it
 * reads the same from either side. Each placement says which plane:
 *   COL_AXIS_X  the plane runs along X - it FACES north/south and steps
 *               east/west;
 *   COL_AXIS_Z  the plane runs along Z - it faces east/west, steps north/south.
 *
 * SCALE: 100 units to the metre (the user's answer, Nov 2026 - the Rabisu's
 * figure; the game has never fixed one). Every piece is a COL_SQ (1 m) square.
 *
 * THE TEXTURE (textures/underworld/column.png, 128x128, 8bpp) is two frames:
 *   TOP HALF     the IDLE frame: a squashed column. Drawn as FIVE 1.2 m x 0.8 m
 *                pieces stacked 4 m tall (COL_IDLE_*) - each a fifth of
 *                the half's rows - so the orb, painted in the middle band, is
 *                the middle piece.
 *   BOTTOM HALF  the MOVING frame: a 3x2 grid of equal cells - left drum, orb,
 *                right drum over left base, blank, right base. Each piece is
 *                its cell (COL_ARCH_SQ in column.c) drawn as its OWN 1 m
 *                square, laid out like an arch (COL_ARCH_* below). The foot stays where
 *                the column stood; the head lands COL_STEP (3 m) along the
 *                plane. Stepping the other way mirrors the whole arch.
 *
 * TWO KINDS, chosen per placement (the user says which, every time):
 *
 *   STATIC  A wall. It collides, it draws its idle frame, it does nothing, and
 *           it cannot be hurt - not even through the orb.
 *
 *   ALIVE   Looks exactly like a static one, until it is caught moving:
 *     IDLE  COL_IDLE_FRAMES (2 s), then - >>> ONLY WHILE IT IS HIDDEN <<< -
 *           it starts a step. HIDDEN means no part of it (nor of the arch it
 *           is about to become) is inside the camera's view, or all of it is
 *           past the fog (g_fog_far, the distance the draw stops at). The test
 *           is the camera's, not the player's, and walls do NOT hide it - the
 *           brief says "off camera or outside the fog range" and nothing else.
 *           A visible column simply keeps waiting.
 *     ARCH  The arch frame for COL_ARCH_FRAMES (2 s), the foot planted and the
 *           head COL_STEP along. A BLUE ORB leaves the orb square for the
 *           player as the arch appears. Then it stands up again as a column
 *           where the head landed. The arch finishes whether or not it is
 *           seen - catching it mid-step is how the player finds out.
 *
 *     It steps left or right at random, whichever way is open: never into a
 *     wall (collision_segment_blocked along the arch), never onto the player,
 *     never into another Column. It is NOT kept to a pool or a patrol - it
 *     walks wherever the walls let it (the user's answer).
 *
 *   ENRAGED  The first hit. From then on it moves 4x faster (COL_RAGE_*:
 *           0.5 s a phase), visible or not, always stepping TOWARD the player
 *           along its plane, and fires an orb every COL_RAGE_FIRE (2 s)
 *           instead of on each step - until it dies. Leaving the room puts it
 *           back at its spawn, calm and whole (columns_rest).
 *
 * THE ORB (the projectile): straight-line, aimed once at the player as it is
 * fired, at HALF a Nessie fireball's speed (COL_ORB_SPEED). COL_ORB_DAMAGE (25)
 * on contact, and it is gone - into the player or into the first wall.
 *
 * DAMAGE. 2 HP, a plain hit point a hit from any weapon (no weakness table
 * worth the name). >>> ONLY THE ORB SQUARE CAN BE HURT. <<< column_orb_box()
 * is the only thing the weapons are shown, so a round into one of the other
 * four squares passes through as if they were not there (Lamashtu's spot is
 * the precedent) - "no effect", as briefed.
 *
 * DEATH. The Golem's: SFX_RUMBLE and its burst of tumbling stone cubes, in
 * greys and purples, spread along the column's own plane.
 *
 * TEXTURE SLOT. x[448,512) y[128,256), 8bpp, CLUT (256,487) - the intro
 * still's (intro_start() reads it itself) and lumberer_a's (re-uploaded on
 * entry to every Catacombs room). Voff 128, so every quad brackets its own
 * texture window. >>> ONLY THE ROOMS column_room_has_sheet() NAMES UPLOAD
 * IT. <<< Placing one in another room: the placement in world.c, the room in
 * column_room_has_sheet() (and COLUMN_ROOMS in tools/gen_tex_rooms.py), and
 * update_columns() / draw_columns() / columns_set_texwindow() in that room.
 *
 * PERSISTENCE: the Golem's model - ONE global array tagged by area, every loop
 * gated on current_area (NEVER game_state). Deaths are saved in
 * WorldDelta.columns_dead, keyed by canonical index AMONG THE ALIVE ONES
 * (static Columns cannot die, so they spend no bit): MAX_ALIVE_COLUMNS is that
 * budget, MAX_COLUMNS the whole-game one.
 * ----------------------------------------------------------------------- */

#define MAX_COLUMNS           16   /* WHOLE-GAME budget, static + alive. One
                                      seeded: the Lava Maze's middle pool    */
#define MAX_ALIVE_COLUMNS      8   /* ...of which alive (one save bit each)  */

#define COL_MAX_HEALTH         2

/* ---- Size, 100 units to the metre. */
#define COL_SQ               100   /* one square: 1 m                        */
#define COL_HALF            (COL_SQ / 2)
/* THE IDLE COLUMN IS 20% WIDER AND 20% SHORTER than five squares (the user's
   two tweaks, Nov 2026 - 1 x 5 m read too thin, then too tall): 120 x 400, as
   five pieces of 120 x 80. Only the idle frame; the arch's squares stay 1 m.
   Its body, its hidden test and its orb's hit box all use these sizes. */
#define COL_IDLE_HALF_W     ((COL_SQ * 6) / 10)    /* 60 */
#define COL_IDLE_PIECE_H    ((COL_SQ * 8) / 10)    /* 80: one piece's height */
#define COL_IDLE_HALF_H     (COL_IDLE_PIECE_H / 2) /* 40                     */
#define COL_STACK              5   /* the idle frame: five pieces            */
#define COL_IDLE_H          (COL_IDLE_PIECE_H * COL_STACK)   /* 400          */
#define COL_STEP             300   /* foot to head: 3 m, the painted arch's  */
/* How far along the plane a step reaches: the far edge of the column it
   stands up as. */
#define COL_STEP_REACH      (COL_STEP + COL_IDLE_HALF_W)

/* ---- The arch: each square's centre, as an offset ALONG the plane from the
   foot (in the stepping direction) and a height above the floor. From the
   painted pieces' centroids, scaled so the two bases sit COL_STEP apart
   (~3.6 units a texel), then made symmetric. */
#define COL_ARCH_PIECES        5
#define COL_ARCH_H           235   /* the orb square's top                   */
#define COL_ORB_ARCH_ALONG   150
#define COL_ORB_ARCH_H       185
#define COL_ORB_IDLE_H      (COL_IDLE_H / 2)   /* 200: the middle piece   */

/* ---- Timing, in frames at 60 fps. */
#define COL_IDLE_FRAMES      120   /* 2 s standing before a step             */
#define COL_ARCH_FRAMES      120   /* 2 s as an arch                         */
#define COL_RAGE_IDLE         30   /* enraged: 4x faster (the user's answer) */
#define COL_RAGE_ARCH         30
#define COL_RAGE_FIRE        120   /* enraged: an orb every 2 s              */
#define COL_RAGE_FIRST        60   /* ...the first one a second after the hit
                                      (mistake 8: never on frame one)        */

/* ---- The orb. Half a Nessie fireball's speed (NSS_FB_SPEED, 32 - column.c
   asserts it): 16 a frame, 960 a second - from 1500 away about 1.6 s. */
#define COL_ORB_SPEED         16
#define COL_ORB_DAMAGE        25
#define COL_ORB_HIT_RADIUS   110   /* contact sphere round the aim point     */
#define COL_ORB_AIM_DROP      70   /* aim below the eye, as NSS_FB_AIM_DROP  */
#define COL_ORB_HALF          36   /* the glow's world half-size             */
#define MAX_COL_ORBS           6

/* ---- The body. The player is held COL_WALL_LIKE_PUSH off every square that
   stands on the floor - the walls' own standoff, because it IS a wall - at
   most COL_PUSH_STEP a frame, so an arch landing beside them nudges rather
   than throws. */
#define COL_WALL_LIKE_PUSH   195
#define COL_PUSH_STEP         48
/* A step is refused if its arch would come within this of the player, so it
   never lands on them. */
#define COL_PLAYER_CLEAR    (COL_WALL_LIKE_PUSH + 10)

#define COL_BAR_TIMER_MAX    120

typedef enum {
    COL_AXIS_X = 0,   /* plane along X: faces north/south, steps east/west */
    COL_AXIS_Z = 1,   /* plane along Z: faces east/west, steps north/south */
} ColumnAxis;

typedef enum {
    COL_IDLE,
    COL_ARCH,
    COL_DEAD,
} ColumnState;

typedef struct {
    int32_t     x, y, z;        /* x/z: where the column stands (the foot, in
                                   an arch); y: the standing anchor
                                   (floor - GROUND_FLOOR_Y)                   */
    int32_t     spawn_x, spawn_z, spawn_y;
    int32_t     vy;
    int16_t     health;
    int16_t     hit_timer;
    int16_t     timer;          /* frames into the current state              */
    int16_t     fire_timer;     /* enraged: frames to the next orb            */
    int8_t      dir;            /* the arch's direction along the plane: +/-1 */
    uint8_t     axis;           /* ColumnAxis                                 */
    uint8_t     alive;          /* 0 = STATIC (a wall), 1 = ALIVE             */
    uint8_t     enraged;
    ColumnState state;
    int32_t     active;
    int         on_upper_floor;
    int         on_ramp;
    GameState   area;
} Column;

extern Column columns[MAX_COLUMNS];
extern int    column_count;

/* Register the sheet with texmgr (TEXBANK_UNDERWORLD). Call ONCE at startup. */
void columns_load_textures(void);
/* Stream it into VRAM. main.c calls it from STATE_LOADING for every area
   column_room_has_sheet() names. */
void columns_upload_textures(void);
int  column_room_has_sheet(GameState area);

/* Plant one standing at (x,z) in `axis`'s plane. alive = 0 is a STATIC
   Column (a wall), 1 a living one. Authored - nothing here reads the mesh -
   so world.c can rebuild a placement from a save. Returns its index, or -1 if
   the pool (or the alive budget) is full. */
int  column_add(int32_t x, int32_t z, ColumnAxis axis, int alive, GameState area);

void columns_init(void);
void columns_reset(void);
/* Every living Column back at its spawn, calm, idle and whole (deaths stick),
   and every orb and shard cleared. Called when leaving a room. */
void columns_rest(void);

void update_columns(void);
void draw_columns(RenderContext *ctx);

/* Any damage to a living Column (through the orb - the weapons only see
   column_orb_box). Ignored by a static one. */
void column_damage(Column *s, int dmg);
int32_t column_scale_damage(int32_t base, DamageType type);
/* The orb square, the only thing a weapon can hit: centre and half-extents.
   0 if this Column cannot be hurt (static, dead, elsewhere). */
int  column_orb_box(const Column *s, int32_t *ox, int32_t *oy, int32_t *oz,
                    int32_t *hw, int32_t *hh);
/* The crucifaxe test, measured to the orb square. 1 if it landed on one. */
int  columns_try_hit(void);
/* The player's push-out; called from apply_collision_reception. */
void columns_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* The area's texture window, restored after each quad (the sheet is at Voff
   128). NULL for an area with none. */
void columns_set_texwindow(const RECT *tw);

#endif
