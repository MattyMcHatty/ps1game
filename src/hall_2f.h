#ifndef HALL_2F_H
#define HALL_2F_H

#include "render.h"

/* Second-floor Hall, reached by ascending the conservatory stairs. An L-shaped
   corridor (a long east-west run along the north) opening west into a larger
   room, with an open stairwell descending back down to the conservatory.
   A single flat y=0 floor (the descending stairwell is scenery only, like the
   conservatory's upstairs). Modelled on conservatory.c. */
void hall_2f_load_assets(void);      /* startup: geometry + texture registration */
void hall_2f_upload_textures(void);  /* room entry: pure LoadImage from RAM (no CD) */
void hall_2f_init(void);             /* set collision/floor zones + spawn */
void hall_2f_draw(RenderContext *ctx);
void hall_2f_stairs_arm(void);       /* seed Circle edge state on entry */
int  hall_2f_stairs_triggered(void); /* 1 when Circle pressed near the down-stairs */

#endif
