#ifndef LIGHTNINGS_EDGE_H
#define LIGHTNINGS_EDGE_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   LIGHTNINGS EDGE - Chapter 4's tenth room
   =========================================================================
   Behind Ribcage's EAST door.

   THE SHAPE. One storey of volcanic rock at y=0, x[-1200,1200]
   z[-1200,2599]: an octagon with cut corners inside 600-tall proxy walls (the
   drawn walls stand 400). It hangs in the chapter's sky
   (src/underworld_sky.h), centred on it. One flat floor zone. No lava, no
   props, no enemies.

   ONE DOOR:
     - SOUTH: a garden gate (the Garden Courtyard's grdn_gte), x[-200,400]
       in the south wall. It leads back to RIBCAGE's east door, a double
       door. Per direction (the user's rule, Nov 2026: the transition is the
       door on the side being left): out of here DOOR_PANEL_GATE, out of
       Ribcage DOOR_PANEL_OUTER. Ribcage's side is blocked by its Golem until
       that Golem dies.

   THE ARRIVAL. One: through the south gate from Ribcage, just inside it
   facing north (lightnings_edge_spawn_s, the init's spawn and where a debug
   jump lands).

   THE MUSIC is the chapter's (CDAUDIO_UNDERWORLD_TRACK, main.c's Chapter 4
   block).

   Textures: two, none owned - see lightnings_edge.c. Chapter rules (the
   fog, the sky): src/underworld.h. */

void lightnings_edge_load_assets(void);     /* startup: TIM_SLOTs only        */
void lightnings_edge_load_geometry(void);   /* ROOM ENTRY: mesh + the sky     */
void lightnings_edge_upload_textures(void); /* room entry: pure LoadImage     */
void lightnings_edge_init(void);            /* collision, floor, spawn        */
void lightnings_edge_draw(RenderContext *ctx);

/* Seed the gate's Circle edge state "held", so a press carried in through
   the transition cannot fire on the arrival frame. */
void lightnings_edge_doors_arm(void);

/* The SOUTH gate, to Ribcage: a fresh Circle within reach and facing it.
   The edge state is kept current while locked. */
int  lightnings_edge_s_gate_triggered(int lock);

/* Arrival through the south gate: just inside it, facing north. */
void lightnings_edge_spawn_s(void);

#endif
