#ifndef RECEPTION_H
#define RECEPTION_H

#include "render.h"

/* Reception is a placeholder room (untextured, flat-shaded) entered through the
   kitchen's "to reception" door. It will be replaced once the art is done. */
void reception_load_assets(void);     /* startup: geometry + preload textures to RAM */
void reception_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void reception_init(void);          /* set collision/floor zones + spawn */
void reception_draw(RenderContext *ctx);
void reception_door_arm(void);      /* seed Circle edge state on entering reception */
int  reception_door_triggered(void);/* 1 when Circle pressed within range of the door */

#endif
