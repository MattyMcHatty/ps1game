#ifndef TEXMGR_H
#define TEXMGR_H

#include <stdint.h>

/* Central registry for textures that must be RE-UPLOADABLE to VRAM without a
   mid-game CD read — i.e. textures a room/prop streams into a shared VRAM slot
   on a transition (see tools/TEXTURING_NOTES.txt: LoadImage is safe, CdRead is
   NOT, once the render loop is running).

   Each registered texture is CdRead into a resident RAM buffer ONCE at startup
   (the only safe time), its TIM header parsed and its tpage/clut captured. The
   pixels (+CLUT) can then be re-uploaded to VRAM with a pure LoadImage on any
   room transition. This replaces the per-module RAM-buffer / TIM-parse / upload
   boilerplate that kitchen_dining.c, reception.c and dresser.c each hand-rolled.

   Adding a new streamed prop/room texture is now: texmgr_register() at startup,
   texmgr_upload() on the transition, texmgr_tpage()/texmgr_clut() when drawing. */

/* Register a texture at STARTUP: reads the file into a kept RAM buffer, parses
   its TIM and captures tpage/clut. Does NOT upload to VRAM — the caller decides
   when via texmgr_upload() (e.g. at startup for a resident slot, or on room
   entry for a streamed one). Returns a texture id (>= 0), or -1 on failure. */
int texmgr_register(const char *filename);

/* Upload a registered texture's pixels (+CLUT if any) from its RAM copy. Pure
   LoadImage, no CD access — safe during a room transition once the GPU is idle
   (the caller DrawSyncs first, as main's STATE_LOADING does). */
void texmgr_upload(int id);

uint16_t texmgr_tpage(int id);
uint16_t texmgr_clut(int id);

#endif
