#ifndef RECEPTION_H
#define RECEPTION_H

#include "render.h"

/* Reception is a placeholder room (untextured, flat-shaded) entered through the
   kitchen's "to reception" door. It will be replaced once the art is done. */
void reception_load_assets(void);   /* load geometry at startup (no LoadImage) */
void reception_stream_textures(void); /* SPIKE: LoadImage during the transition,
                                         GPU idle — proves mid-game upload is safe */
void reception_init(void);          /* set collision/floor zones + spawn */
void reception_draw(RenderContext *ctx);

#endif
