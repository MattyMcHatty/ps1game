#ifndef TRICK_DRAWERS_H
#define TRICK_DRAWERS_H

#include <stdint.h>
#include "render.h"

/* Trick Drawers: a static, solid chest of drawers standing in the 2F hall's
   west room. Single textured SMD prop (closed-drawer texture only), rendered
   with the room's fog + 128 texture window, modelled on concrete_props.c. */
void trick_drawers_load_assets(void);      /* startup: SMD + register texture */
void trick_drawers_upload_texture(void);   /* room entry: pure LoadImage from RAM */
void trick_drawers_place(void);            /* place the hall's drawers */
void trick_drawers_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);
void trick_drawers_draw(RenderContext *ctx);

#endif
