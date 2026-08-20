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
/* bookshelf alone — for rooms (the library) that draw bookcase art but must
   keep piano_keys OUT of the kchn_tile slot they need for cncrte. */
void piano_props_upload_bookcase_texture(void);
void piano_props_place(void);           /* piano_room_init: position both props */
void piano_props_draw(RenderContext *ctx);
void piano_props_text(RenderContext *ctx); /* floating "examine" sign (view matrix active) */
void piano_props_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);
/* Hitscan solid test for the gun's line of sight (dresser-style, height-aware). */
int  piano_props_point_solid(int32_t x, int32_t y, int32_t z, int32_t slack);

/* Is ANY instance of this family solid in the CURRENT AREA right now? Mirrors
   the non-coordinate gates of piano_props_point_solid above/below, and nothing
   else. collision_segment_blocked uses it to skip its whole segment-sampling
   pass in rooms that hold no props at all — see the note there. */
int  piano_props_any_solid(void);

/* ---- Piano puzzle hooks (piano_puzzle.c) ----------------------------------
   Where the piano stands, so the puzzle can range-check the Circle press
   against the same spot the examine sign floats over. */
int32_t piano_prop_x(void);
int32_t piano_prop_z(void);

/* Fit the missing key: re-uploads the repaired keyboard over the piano_keys
   VRAM slot. Idempotent, and a no-op if the repaired TIM never registered. */
void piano_props_repair_keys(void);

/* Drop the bookcase through the floor. sink_start begins it, _update advances
   one frame and returns 1 once it is fully gone (deactivating the prop, which
   also drops its collision box), _sinking reports whether it is still moving. */
void piano_props_bookcase_sink_start(void);
int  piano_props_bookcase_update(void);
int  piano_props_bookcase_sinking(void);

/* ---- Anzu Tablet puzzle hook (anzu_puzzle.c) -------------------------------
   Retire the Tablets prop the instant the puzzle is solved. piano_props_place
   already keeps it hidden on later visits (it reads FLAG_ANZU_SOLVED), so this
   only covers the frames between solving and leaving the room. */
void piano_props_tablets_hide(void);

#endif
