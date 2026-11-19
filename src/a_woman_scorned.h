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
   south-west and the south-east - that burn like every Chapter 4 floor's
   (its ROOMS line in tools/gen_lava_floors.py). It hangs in the chapter's
   sky (src/underworld_sky.h), centred on it. One flat floor zone. No props,
   no enemies.

   TWO DOORS, in the south corners:
     - SOUTH-WEST: a catacomb inner door, distorted - narrow at the floor and
       flaring out as it leans in (see a_woman_scorned.c). It leads back to
       HELL HATH NO FURY's north-east door, whose side is a catacomb inner
       door too: DOOR_PANEL_CATACOMB both ways (the user's choice, Nov 2026).
     - SOUTH-EAST: an inner double door (the mansion's inr_dbl_dr, resident).
       Leads nowhere yet - no prompt and no trigger until its room exists.

   THE ARRIVAL. One: through the south-west door, just inside it facing
   north-east (a_woman_scorned_spawn_sw, also the init's default and where a
   debug jump lands).

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

#endif
