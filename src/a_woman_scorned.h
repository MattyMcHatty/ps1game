#ifndef A_WOMAN_SCORNED_H
#define A_WOMAN_SCORNED_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   A WOMAN SCORNED - Chapter 4's seventh room
   =========================================================================
   Behind Hell Hath No Fury's NORTH-EAST door.

   THE SHAPE. One storey of volcanic ground at y=0, x[0,6600] z[-600,4800],
   with an irregular rim (the collision proxy's outer walls, 774 tall), square
   rock pillars standing about the floor (600 tall) and two lava pools - the
   north, x[3000,3800] z[3200,4000], and the south-east, x[4200,5600]
   z[-600,1000] - that burn like every Chapter 4 floor's (its ROOMS line in
   tools/gen_lava_floors.py). It hangs in the chapter's sky
   (src/underworld_sky.h), centred on it. One flat floor zone. No props.

   ENEMIES (world.c's world_seed_room, the user's placements): two GOLEMS
   standing free at (6260,672) and (310,3122), guarding nothing; a NESSIE in
   the middle of each lava pool, confined to it; two AHRIMANS, (4485,3082)
   at height B and (1559,4510) at height C.

   TWO DOORS, in the south corners:
     - SOUTH-WEST: a catacomb inner door, distorted - narrow at the floor and
       flaring out as it leans in (see a_woman_scorned.c). It leads back to
       HELL HATH NO FURY's north-east door, whose side is a catacomb inner
       door too: DOOR_PANEL_CATACOMB both ways (the user's choice, Nov 2026).
     - SOUTH-EAST: an inner double door (the mansion's inr_dbl_dr, resident).
       It leads to the LAVA MAZE's north door, a catacomb inner door: out of
       here DOOR_PANEL_INNER (its panel is the same resident inr_dbl_dr),
       back DOOR_PANEL_CATACOMB (the user's choice, Nov 2026, per direction).

   THE ARRIVALS. Two: through the south-west door, just inside it facing
   north-east (a_woman_scorned_spawn_sw, also the init's default and where a
   debug jump lands), and through the south-east door from the Lava Maze,
   facing north-west (a_woman_scorned_spawn_se, main.c's override).

   THE MUSIC is the chapter's (CDAUDIO_UNDERWORLD_TRACK, main.c's Chapter 4
   block).

   Textures: five, none owned - see a_woman_scorned.c. Chapter rules (the fog,
   the sky): src/underworld.h. */

void a_woman_scorned_load_assets(void);     /* startup: TIM_SLOTs only        */
void a_woman_scorned_load_geometry(void);   /* ROOM ENTRY: mesh + the sky     */
void a_woman_scorned_upload_textures(void); /* room entry: pure LoadImage     */
void a_woman_scorned_init(void);            /* collision, floor, spawn        */
void a_woman_scorned_draw(RenderContext *ctx);

/* Seed every door's Circle edge state "held", so a press carried in through
   the transition cannot fire on the arrival frame. */
void a_woman_scorned_doors_arm(void);

/* The SOUTH-WEST door, to Hell Hath No Fury: a fresh Circle within reach and
   facing it. The edge state is kept current while locked. */
int  a_woman_scorned_sw_door_triggered(int lock);

/* Arrival through the south-west door: just inside it, facing north-east. */
void a_woman_scorned_spawn_sw(void);

/* The SOUTH-EAST door, to the Lava Maze, on the south-west door's terms, and
   the arrival through it: just inside it, facing north-west. */
int  a_woman_scorned_se_door_triggered(int lock);
void a_woman_scorned_spawn_se(void);

#endif
