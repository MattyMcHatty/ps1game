#ifndef KITCHEN_DINING_H
#define KITCHEN_DINING_H

#include "render.h"

void kitchen_load_assets(void);     /* startup: register streamed textures */
void kitchen_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void kitchen_stream_textures(void); /* re-upload kitchen textures on re-entry (GPU idle) */
void kitchen_restore_textures(void);/* re-upload reception-shared textures from RAM (no CD) */
/* NARROW single-texture uploads out of the same resident RAM copies, for rooms
   that need one of these two slots but not the rest of the kitchen's set. */
void kitchen_upload_red_crpt(void);    /* red_crpt    -> x320 y256 (frnt_dr slot) */
void kitchen_upload_double_door(void); /* double_door -> x832 y0  (con_tile slot) */
void kitchen_dining_init(void);
void kitchen_dining_draw(RenderContext *ctx);
void kitchen_door_arm(void);        /* seed Circle edge state on entering the kitchen */
int  kitchen_door_triggered(void);  /* 1 when Circle pressed within range of the door */
int  to_reception_door_triggered(void); /* 1 when Circle pressed within range of the reception door */
void kitchen_stove_update(void);    /* feed/advance the stove flame each frame */
void kitchen_stove_set_lit(int lit);/* stove puzzle: burner on/off */
void kitchen_stove_reset(void);     /* extinguish the stove (new game) */

#endif
