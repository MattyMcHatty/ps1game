#ifndef THE_PIT_H
#define THE_PIT_H

#include <stdint.h>
#include "render.h"

/* The Pit: Chapter 3's SIXTH room, through the NORTH door of the Tomb — the last
   of the three doors that room's header has been listing as "drawn, not built".

   THE SHAPE, AND IT IS THE FIRST ROOM IN THE CHAPTER WITH TWO WALKABLE HEIGHTS
   THAT ARE NOT A MAZE. A rectangular shaft 4800 by 3900, open from top to bottom,
   with a GALLERY running round three of its sides at y=-1000 and the PIT FLOOR
   itself 1000 units below at y=0. The collision proxy found nine floor faces and
   they split cleanly in two:

     THE GALLERY   y=-1000, the upper level, where the player arrives. A U: a
                   south ledge x[-1200,1800] z[0,600] with the arrival door in
                   it, chamfered corners at both ends, and two ARMS running
                   north up the outside of the shaft —
                   east x[2099,2699] and west x[-2104,-1504], both z[910,3300].
                   Both arms are DEAD ENDS at z=3300.

     THE SLOPE     the shaft's south face, drawn in the VISUAL mesh and absent
                   from the collision proxy: a straight 63-degree chute from the
                   ledge's rim (z=600, y=-1000) to the pit's south edge (z=1099,
                   y=0), right across in front of the south door.

     THE PIT       y=0, x[-692,1292] z[1099,3300], with a north alcove
                   x[-125,725] z[3300,3900] holding a second drawn door.

   >>> THE WAY DOWN IS THE SLOPE, AND IT IS A SCENE AND NOT A WALK. <<<
   The proxy still does not connect the two levels — the gallery's rim (walls
   0-4) seals the pit off in plan and the pit's own walls (5, 14-18, 22, 23) stop
   1000 units below the gallery's floor — and it does not need to: a Circle at
   the rim in front of the south door ("Press Circle to descend") takes the
   camera, tilts it down the chute, hops the player onto it and slides them to
   the bottom, and hands control back on the pit floor. The player never stands
   on the slope, so it has no floor zone and no wall. See THE DESCENT in the .c.

   >>> IT IS ONE-WAY. <<< Nothing takes the player back up. Once down they are
   in the pit until something down there is built to let them out.

   >>> AND IT ENDS IN AN AMBUSH. <<< At the bottom the camera holds a second
   looking north, the BARS (src/bars.h) hanging over the north alcove drop
   across its mouth and bounce, and two Lumberers appear either side of them,
   already alert. Only then is control handed back. Kill both and two Crawlers
   scuttle down the pit's east and west banks; kill those and the bars winch
   back up to the height they started at, over two plays of the machinery
   sound, and the alcove is open. All four enemies are seeded by world.c like
   any other and stowed on every entry until their wave. See THE AMBUSH and
   THE WAVES, AFTER THE SCENE in the .c.

   THE VAULT is at y=-1800 — 800 of headroom over the GALLERY, the chapter's
   usual, and 1800 over the pit floor, which is the deepest drawn space in the
   game. The collision proxy's gallery walls stop at -1800 too, so the two agree
   and collision_set_ceiling_y(-1800) is the honest number. >>> ANYTHING HUNG
   FROM IT IS 1800 ABOVE THE PIT AND 800 ABOVE THE LEDGE THE PLAYER IS ACTUALLY
   STANDING ON. <<< Author against whichever of the two the thing is over, not
   against the single value this room reports.

   THE DOORS. Two are drawn, and ONE of them is wired up:

     SOUTH  z=0     x[200,400]    y[-1400,-1000]  -> Tomb, north door.
                    On the GALLERY, in the south ledge.
     north  z=3900  x[158,442]    y[-500,0]       not built.
                    On the PIT FLOOR, in the north alcove, at the bottom of the
                    descent. It reads as a sealed door, which is what it is —
                    and the bars dropped across the alcove's mouth seal the way
                    to it until both waves of the ambush are dead.

   Both are XY-plane doors (fixed Z) and both are approached from +Z, so the live
   one takes TEXT_PLANE_XY with mirror=1 and its sign 11 units proud of the wall
   along +Z. Wall 19 runs z=0 with nz=+4096, which is where that comes from. The
   Tomb's north door, the far side of this wall, takes the opposite pair for the
   same reason in reverse (its wall 19 has nz=-4096).

   NO STOREY TEST ON THE SOUTH DOOR, and this is a room where that has to be
   argued rather than assumed. The Up Down Maze's two doors are the only triggers
   in the game that test Y, because there the walkable surface is not a function
   of XZ — a corridor runs directly under a door. Here it is a function of XZ:
   the gallery and the pit have DISJOINT footprints (compare the two lists above
   — the pit starts at z=1099 and the gallery has nothing past z=910 inside
   x[-692,1292]), so no point in the room has two walkable heights and a plain
   Manhattan test is correct. The nearest pit floor is 1099 from the door in
   plan against a 500 trigger radius, so it is not even close.
   >>> THE DESCENT DID NOT CHANGE THAT, AND IT WAS CHECKED. <<< The slope is
   never walkable, and the closest the player can stand on the pit floor is
   z=1294 (wall 17's push), 1294 from the door. A WALKABLE ramp or stair under
   the south ledge would break it, silently: the player would open the Tomb's
   door from the bottom of the shaft. The descend prompt DOES test the storey,
   because the pit floor is inside its radius; see the .c.

   THREE TEXTURES, AND THE ROOM OWNS ONE — the Room of Arms' arrangement.
   Cobblestone and the catacomb inner door are registered by
   src/catacombs_entry.c in TEXBANK_CATACOMBS and reached through that module's
   two NARROW uploaders, as every Chapter 3 room since the Up Down Maze reaches
   theirs. `rusty` is the corroded ironwork lining the shaft, new art nothing else
   in the game draws, so this room registers it — deferred, like the rest of the
   chapter. Its VRAM page is x704 y0, the LAST whole mesh-art page in the
   Catacombs bank, and it costs seven other modules' uploaders a promise it did
   not cost the crib. The whole argument is in the_pit_load_assets() and in
   tools/vram_map.py beside the stream pairs.

   Its exports live in assets/catacombs/, beside the other five Chapter 3
   rooms'. */

void the_pit_load_assets(void);     /* startup: one deferred reg + three headers */
void the_pit_load_geometry(void);   /* ROOM ENTRY: read the mesh into the arena  */
void the_pit_upload_textures(void); /* room entry: pure LoadImage from RAM       */
void the_pit_init(void);            /* collision + floor zones + spawn          */
void the_pit_draw(RenderContext *ctx);

/* Arrival through the south door, from the Tomb: standing on the gallery's south
   ledge just inside it, facing north across the shaft. */
void the_pit_spawn_south(void);

/* One frame of the south door's Circle test. `lock` is main's usual suppression
   (a menu is up, a cutscene owns the camera). Returns 1 on a fresh press made in
   range and facing the door — the frame main.c starts the transition on. */
int  the_pit_south_door_triggered(int lock);

/* Arm every interaction in the room. Called by the spawn above; exported so a
   caller that places the player some other way (a debug jump) can still ensure a
   Circle held through the transition does not fire on the arrival frame. */
void the_pit_arm(void);

/* THE DESCENT — the slide from the gallery to the pit floor.

   Called UNCONDITIONALLY from main.c's Pit branch, `lock` passed in, like the
   door test: it keeps its Circle edge state current while locked. Returns 1 on
   any frame the descent owns the camera OR has just consumed the Circle press,
   and main.c's south-door test runs after it and must not act on a press this
   one took. While it is running, main.c routes the frame to it from the
   cutscene early-return at the top of update_current_area() instead, with lock
   0, so no update_camera, collision or apply_height fights the shot. */
int  the_pit_descent_update(int lock);

/* 1 from the Circle press until control is handed back on the pit floor —
   which is AFTER the ambush, not at the landing. main.c needs it in the
   early-return and in the `cutscene` list (no Start menu, no HUD). */
int  the_pit_descent_active(void);

/* Stow the ambush's Lumberers and Crawlers. Call after world_enter() on every
   entry to this room — world_enter seeds/restores them active, and they must
   not be until their wave. */
void the_pit_after_world_enter(void);

#endif
