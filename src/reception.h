#ifndef RECEPTION_H
#define RECEPTION_H

#include "render.h"

/* Reception is a placeholder room (untextured, flat-shaded) entered through the
   kitchen's "to reception" door. It will be replaced once the art is done. */
void reception_load_assets(void);     /* startup: register streamed textures */
void reception_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void reception_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void reception_init(void);          /* set collision/floor zones + spawn */
void reception_draw(RenderContext *ctx);
void reception_door_arm(void);      /* seed Circle edge state on entering reception */
int  reception_door_triggered(void);/* 1 when Circle pressed within range of the door */
void wdoor_arm(void);               /* same pair for the west single door */
int  wdoor_triggered(void);         /* 1 when Circle pressed within its range */
void cdoor_arm(void);               /* same pair for the conservatory door */
int  cdoor_triggered(void);         /* 1 when Circle pressed within its range */
void hdoor_arm(void);               /* same pair for the 2nd-floor door to the 2F hall */
int  hdoor_triggered(void);         /* 1 when Circle pressed near the upper-floor south door */
void edoor_arm(void);               /* same pair for the 2nd-floor east double door */
int  edoor_triggered(void);         /* 1 when Circle pressed near the upper-floor east door */
void ndoor_arm(void);               /* same pair for the 2nd-floor NORTH-WEST door */
int  ndoor_triggered(void);         /* 1 when Circle pressed near it (-> West Corridor) */

/* Arrive back on the upper floor at the north-west door, coming out of the
   West Corridor's east door. */
void reception_spawn_northwest(void);

#endif
