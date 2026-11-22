#ifndef DESTINYS_CABIN_H
#define DESTINYS_CABIN_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   DESTINYS CABIN - Chapter 4's thirteenth room
   =========================================================================
   Behind The Tube's NORTH door.

   THE SHAPE. One storey at y=0, x[0,5400] z[-199,7000]: volcanic rock and
   grass inside an octagon of 600-tall proxy walls that are not drawn - the
   field stands open in the chapter's sky (src/underworld_sky.h), centred on
   it. Lava runs along the south edge (z below ~800) and the north edge (z
   above 6400), and it BURNS (tools/gen_lava_floors.py). In the middle, a
   wooden CABIN (stables wood), x[800,4600] z[1800,5200], 1000 tall, that
   the player can walk into through the doorway in its south side. One flat
   floor zone. No props, no enemies.

   ONE GATE:
     - WEST (x=0, z[5400,6200]): a garden gate (grdn_gte), 500 tall. It
       leads to THE TUBE's north door, a catacomb inner door. Per direction
       (the transition is the door on the side being left): out of here
       DOOR_PANEL_GATE, out of The Tube DOOR_PANEL_CATACOMB. The Tube's
       Golem blocks that door until it dies.

   THE ARRIVAL. One: through the west gate from The Tube, just inside it
   facing east (destinys_cabin_spawn_w, the init's spawn and where a debug
   jump lands).

   THE MUSIC is the chapter's (CDAUDIO_UNDERWORLD_TRACK, main.c's Chapter 4
   block).

   Textures: five, all borrowed - see destinys_cabin.c. Chapter rules (the
   fog, the sky): src/underworld.h. */

void destinys_cabin_load_assets(void);     /* startup: TIM_SLOTs only            */
void destinys_cabin_load_geometry(void);   /* ROOM ENTRY: mesh + the sky         */
void destinys_cabin_upload_textures(void); /* room entry: narrow uploaders       */
void destinys_cabin_init(void);            /* collision, floor, spawn            */
void destinys_cabin_draw(RenderContext *ctx);

/* Seed the gate's Circle edge state "held", so a press carried in through
   the transition cannot fire on the arrival frame. */
void destinys_cabin_doors_arm(void);

/* The WEST gate, to The Tube: a fresh Circle within reach and facing it.
   The edge state is kept current while locked. */
int  destinys_cabin_w_gate_triggered(int lock);

/* Arrival through the west gate: just inside it, facing east. */
void destinys_cabin_spawn_w(void);

#endif
