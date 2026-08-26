#ifndef VALVE_HANDLE_H
#define VALVE_HANDLE_H

#include <stdint.h>
#include "render.h"
#include "title.h"   /* GameState — each mount is tagged with the area it lives in */

/* The Valve Handle: a wheel the player takes off one pipe and fits to another.
   Structurally the LIGHTEST prop in the game — it is the fat door with almost
   everything removed:

     NO COLLISION AT ALL. It is not in props_block_point, not in
     collision_props_collide, and has no *_point_solid. The player walks through
     it and a shot passes through it. That is deliberate: it is a 83-unit wheel
     bolted flat to a wall, and giving it a box would only push the camera off
     the pipe the player is trying to look at.

     NO HEALTH, no smash, no particles.

   What it does have is a MOUNT LIST and an ANIMATION, and they are what the rest
   of the game will drive:

     PRESENT / ABSENT   Each mount is a place a handle CAN be, plus one bit for
                        whether one is in it. The Greenhouse's mount starts
                        present — that is where the player finds the thing — and
                        valve_handle_set_present(area, 0) clears it when they
                        take it. Fitting it elsewhere sets that mount's bit.
                        This is the "spawn it into and out of a room" part, and
                        it is a bit rather than an array edit so the mounts
                        themselves stay compile-time constants.

     THE TURN           valve_handle_begin_turn() spins the fitted wheel
                        VALVE_TURN_REVS full revolutions about its own stem over
                        VALVE_TURN_FRAMES, then stops. valve_handle_turning()
                        reports whether one is still going, so a puzzle can wait
                        for it before opening whatever the valve opens.

   >>> IT REUSES THE PIPE TEXTURE, AND WHICH ONE IS PER-ROOM. <<< The mesh was
   linked against pipe.tim (x768 y0), but the Greenhouse cannot draw that page —
   brick_wall lives there and the room draws brick_wall — so it draws the 4bpp
   clone pipe_gh.tim at x384 y256 instead. The draw loop therefore OVERRIDES the
   baked tpage/clut per mount, from a slot chosen by area. Adding a room means
   adding a line to valve_pipe_slot(); getting it wrong shows as the wheel
   wearing another room's wallpaper. See src/greenhouse.c's texture table.

   >>> THE RED POLY IN THE MODEL IS NOT DRAWN. <<< assets/props/Valve Handle.smx
   carries one untextured F3 at the tip of the stem, dark red, and it is a
   REGISTRATION MARKER rather than art: it marks the face that has to end up flat
   against the pipe, on the black hole in the pipe texture. Every mount below is
   authored so that marker lands there, which is why the positions look like they
   have arbitrary 20-unit offsets in them — that is the stem length. Drawing it
   would put a red triangle coplanar with the pipe wall and z-fight. The loop
   skips every untextured primitive, which is exactly that one poly. */

#define MAX_VALVE_MOUNTS     4   /* the present bits are part of the save blob */
#define VALVE_TURN_FRAMES   90   /* 1.5 s at 60 fps                            */
#define VALVE_TURN_REVS      2   /* full revolutions per turn                  */

/* Model bounds, from assets/props/Valve Handle.smx: the wheel is a ring of
   radius 33..45 lying in the model's XZ plane, 10 deep in Y, with a 20-unit stem
   up +Y ending in the marker poly. STEM_LEN is what every mount position is
   offset by along its own facing. */
#define VALVE_STEM_LEN      20
#define VALVE_RADIUS        45

typedef struct {
    int32_t   x, y, z;        /* the model ORIGIN: the wheel's centre, one stem
                                 length out from the pipe face (see the note on
                                 the marker poly above)                        */
    int32_t   rot_x, rot_y;   /* mount orientation; rot_x = -1024 stands the
                                 wheel up with its stem pointing -Z            */
    int32_t   present;        /* 1 = a handle is fitted here                   */
    int32_t   spin;           /* current wheel angle, 0..4095                  */
    int32_t   turn_timer;     /* frames left in an active turn; 0 = at rest    */
    int32_t   active;
    GameState area;
} ValveMount;

extern ValveMount valve_mounts[MAX_VALVE_MOUNTS];
extern int        valve_mount_count;

void valve_handles_load_assets(void);  /* load VALVEH.SMD — STARTUP only */
void valve_handles_init(void);         /* place every mount in the game   */
void valve_handles_reset(void);
void valve_handles_update(void);       /* advance the turn animation      */
void valve_handles_draw(RenderContext *ctx);

/* The mount in `area` (there is at most one per room today). Returns -1 if that
   room has none. Every call below takes a mount index from this. */
int  valve_mount_in_area(GameState area);

void valve_handle_set_present(int mount, int present);
int  valve_handle_present(int mount);

/* Start the fitted wheel turning. No-op on an empty mount or one already
   turning. valve_handle_turning() stays 1 until the animation finishes. */
void valve_handle_begin_turn(int mount);
int  valve_handle_turning(int mount);

#endif
