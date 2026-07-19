#ifndef PIANO_ROOM_H
#define PIANO_ROOM_H

#include "render.h"

/* Piano room, entered through reception's west single wooden door. Flat
   single-floor room, textured (prpl_wlppr / wd_flr / din_cl / wd_dr),
   modelled on reception.c. */
void piano_room_load_assets(void);     /* startup: geometry + texture registration */
void piano_room_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void piano_room_init(void);            /* set collision/floor zones + spawn */
void piano_room_draw(RenderContext *ctx);
void pdoor_arm(void);                  /* seed Circle edge state on entry */
int  pdoor_triggered(void);            /* 1 when Circle pressed within range of the
                                          east door (back to reception) */

#endif
