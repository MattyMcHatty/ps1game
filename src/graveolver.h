#ifndef GRAVEOLVER_H
#define GRAVEOLVER_H

#include "render.h"

/* The Grave-olver: a gun held in the player's view, rendered like the crucifaxe
   (flat-shaded SMD in view space). Firing/recoil is not implemented yet — for
   now it just holds the model in a rest pose so weapon switching can be tested. */
void graveolver_init(void);              /* load the model (startup) */
void draw_graveolver(RenderContext *ctx);

#endif
