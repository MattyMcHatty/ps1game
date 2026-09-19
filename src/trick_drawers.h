#ifndef TRICK_DRAWERS_H
#define TRICK_DRAWERS_H

#include <stdint.h>
#include "render.h"

/* Trick Drawers: a static, solid chest of drawers standing in the 2F hall's
   west room. Single textured SMD prop (closed-drawer texture only), rendered
   with the room's fog + 128 texture window, modelled on concrete_props.c. */
void trick_drawers_load_assets(void);      /* startup: SMD + register texture */
void trick_drawers_upload_texture(void);   /* room entry: pure LoadImage from RAM */
void trick_drawers_place(void);            /* place the hall's drawers + reset puzzle */
void trick_drawers_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);
void trick_drawers_draw(RenderContext *ctx);

/* Drawer puzzle: proximity "Press O to interact" prompt, then a camera-locked
   reticule mini-game (open drawers in a fixed order to win the Wax Cube). */
void trick_drawers_update(void);           /* prompt/trigger when idle; input when active */
int  trick_drawers_puzzle_active(void);    /* 1 while the puzzle owns the camera + input */


/* Free the prop's geometry on the one-way walk into the Catacombs.
   src/area_bank.h owns the decision; nothing else may call it. */
void trick_drawers_free_assets(void);

/* Re-read that geometry when a save load brings the player back. Geometry
   only - see the note in the .c. */
void trick_drawers_reload_assets(void);

#endif
