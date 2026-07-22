#ifndef CONSERVATORY_H
#define CONSERVATORY_H

#include "render.h"

/* Conservatory, entered through reception's second west-wall door (z=757).
   A long flat-floored glasshouse room (the visible upstairs/garden is scenery
   only — the collision has a single y=0 floor). Modelled on piano_room.c. */
void conservatory_load_assets(void);     /* startup: geometry + texture registration */
void conservatory_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void conservatory_init(void);            /* set collision/floor zones + spawn */
void conservatory_draw(RenderContext *ctx);
void condoor_arm(void);                  /* seed Circle edge state on entry */
int  condoor_triggered(void);            /* 1 when Circle pressed within range of the
                                            east door (back to reception) */
void stairs_arm(void);                   /* same pair for the stairs-ascend prompt */
int  stairs_triggered(void);             /* 1 when Circle pressed near the stairs */

#endif
