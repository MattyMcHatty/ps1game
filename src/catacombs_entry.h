#ifndef CATACOMBS_ENTRY_H
#define CATACOMBS_ENTRY_H

#include <stdint.h>
#include "render.h"

/* Catacombs Entry: the first room of Chapter 3, behind the doors in the
   Outside Catacombs' facade. THE FIRST ROOM THE PLAYER CANNOT LEAVE — the way
   in is one-way by design, and src/area_bank.h is what that buys.

   Modelled in its own coordinate space like every other room. Its ground is NOT
   flat: the player comes in at y=0 and ends up 1240 units further down, through
   two ramps. (-Y is up, so a LARGER y is deeper.)

   Bounds x[-750,4800] y[-1100,1240] z[0,4792], laid out as a descending L:

     ENTRY      x[-750,750] z[0,1400], floor y=0, ceiling y=-1100. A 1500-square
     CHAMBER    stone room. The LAMASHTU TABLET is carved across the whole of
                its south wall at z=0, x[-536,536] — the far side of the doors
                the player just walked between. It is the only thing in the room
                that can be interacted with, and all it does is say so (see
                the examine below). The way on is the 642-wide opening in the
                north wall at x[-321,321].
     RAMP ONE   x[-321,321] z[1400,4192], a corridor falling y=0 -> y=719 over
                2792 units of run. Walls run the full drop.
     NORTH      x[-321,1621] z[4192,4792], flat at y=719. The corner: the
     LANDING    corridor arrives from the south and turns east.
     RAMP TWO   x[971,1621] z[2102,4192], falling BACK SOUTH, y=722 -> y=1240.
     LOWER      x[971,1621] z[1102,2102] and the connector x[1621,1800]
     APPROACH   z[1102,1702], both flat at y=1240.
     THE HALL   x[1800,4800] z[502,2302], floor y=1240, ceiling y=440 — 800 of
                headroom against the entry chamber's 1100, which is the whole
                point of the descent. The LOCULUS art (the burial niches) runs
                down both long sides, x[2000,4600], and two stone masses stand
                in the middle of it at x[2400,3000] and x[3600,4200]. At the
                east end, x=4800 z[1302,1502], is the CATACOMB INNER DOOR — the
                way deeper in, and not built yet (see the placeholder below).

   FOUR TEXTURES, ALL THE CHAPTER'S OWN. Nothing is borrowed from a mansion or a
   garden module, and nothing could be: by the time this room draws, every one
   of those modules has had its RAM copies freed (src/area_bank.h), so a borrowed
   *_upload_textures() would be uploading from a pointer that is now NULL. This
   is the first room in the game that owns every slot it draws.

   Its exports live in assets/catacombs/, the third such subdirectory after
   assets/garden/ and assets/bosses/. */

void catacombs_entry_load_assets(void);     /* startup: DEFERRED registrations only */
void catacombs_entry_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena */
void catacombs_entry_upload_textures(void); /* room entry: pure LoadImage from RAM (no CD) */
void catacombs_entry_init(void);            /* collision + floor zones + spawn */
void catacombs_entry_draw(RenderContext *ctx);

/* Arrival from the Outside Catacombs, and the only arrival there is: standing
   just inside the tablet wall, facing north up the room. */
void catacombs_entry_spawn_south(void);

/* One frame of the two things in here that answer Circle. `lock` is main's
   usual suppression (a menu is up, a cutscene owns the camera). Returns 1 if
   the frame's Circle tap was consumed, so the caller can stop looking — the
   two are 4800 apart against a 500 reach and can never both be in range, but
   the room has one unambiguous order anyway, which is the arrangement The
   Hatch's lip and gate have. */
int  catacombs_entry_interact_update(int lock);

#endif
