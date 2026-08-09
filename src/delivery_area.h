#ifndef DELIVERY_AREA_H
#define DELIVERY_AREA_H

#include "render.h"

void delivery_area_init(void);
/* ROOM ENTRY: read the mesh into the arena. Unlike every other room, delivery is
   also reached WITHOUT a STATE_LOADING pass (straight off the title screen), so
   main.c calls this from both paths. */
void delivery_load_geometry(void);
void delivery_area_draw(RenderContext *ctx);
/* Re-upload every shared slot this room draws from: gravel / rusty_fence /
   brick_wall / double_door (which the conservatory streams over), plus grss_gs,
   the Garden Stairs' clone of grss that this room draws its grass from. The
   trees_dl and chnlnk_dl clones own their VRAM outright and are NOT in here —
   see the texture notes at the top of delivery_area.c. */
void delivery_restore_textures(void);

/* Narrow, single-texture re-upload of this module's brick_wall RAM copy, for a
   room that draws brick_wall but must NOT take the rest of the delivery set.
   The Garden Stairs needs exactly that: it draws brick_wall, but its own chnlnk
   lives in the gravel slot that delivery_restore_textures would stamp back.
   Same pattern as hall_2f_upload_strs(). */
void delivery_upload_brick_wall(void);

#endif
