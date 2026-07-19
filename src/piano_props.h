#ifndef PIANO_PROPS_H
#define PIANO_PROPS_H

#include <stdint.h>
#include "render.h"

/* The piano room's static props: the piano (north wall, door side) and the
   bookcase (a wall-to-wall divider at the room's halfway point). Both are
   indestructible SMD props modelled on the dresser, each with its own streamed
   texture (bookshelf / piano_keys) time-sharing the stn_stl / kchn_tile VRAM
   slots — the same slots reception streams strs / bnnstr into, so every room
   entry re-uploads its own set and the kitchen restores the originals. */

void piano_props_load_assets(void);     /* startup: geometry + texture registration */
void piano_props_upload_textures(void); /* piano-room entry: pure LoadImage from RAM */
void piano_props_place(void);           /* piano_room_init: position both props */
void piano_props_update(void);          /* Circle-to-examine the piano */
void piano_props_draw(RenderContext *ctx);
void piano_props_text(RenderContext *ctx); /* floating "examine" sign (view matrix active) */
void piano_props_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);
/* Hitscan solid test for the gun's line of sight (dresser-style, height-aware). */
int  piano_props_point_solid(int32_t x, int32_t y, int32_t z, int32_t slack);

#endif
