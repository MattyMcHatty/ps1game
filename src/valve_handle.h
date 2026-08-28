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

     THE FIT            valve_handle_begin_fit() is what the Valve Puzzle plays
                        when the player uses the handle on a pipe, and it is a
                        SCRIPT rather than a spin: the wheel appears
                        VALVE_FIT_DIST out along the mount's own facing, slides
                        straight in over VALVE_FIT_IN_FRAMES, holds for
                        VALVE_FIT_HOLD, then takes VALVE_FIT_STEPS sharp
                        CLOCKWISE turns of VALVE_FIT_STEP_DEG apiece, each over
                        VALVE_FIT_STEP_FRAMES with VALVE_FIT_STEP_PAUSE of
                        stillness after it. valve_handle_fitting() is 1 for the
                        whole of it and VALVE_FIT_FRAMES is its total length, so
                        the puzzle can hold its fixed camera for exactly that
                        long and fire the payoff on the frame it ends.

                        It sets `present` ITSELF — the handle is not on the pipe
                        until the script puts it there — so the caller does not
                        also call valve_handle_set_present().

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

#define MAX_VALVE_MOUNTS     4   /* the present bits are part of the save blob.
                                    ALL FOUR ARE NOW USED: the Greenhouse's pipe
                                    and the Valve Puzzle's three. world.c
                                    asserts this fits its 8-bit valve_present
                                    mask, so a fifth costs nothing but a wider
                                    field -- check that assert before adding
                                    one. */
#define VALVE_TURN_FRAMES   90   /* 1.5 s at 60 fps                            */
#define VALVE_TURN_REVS      2   /* full revolutions per turn                  */

/* ---- The fit script (valve_handle_begin_fit) -------------------------------
   >>> CLOCKWISE IS NEGATIVE HERE, AND THAT IS NOT A CHOICE. <<< The spin is
   applied about the model's own +Y, and every mount points that axis INTO the
   pipe -- so the player is always looking down it from the NEGATIVE end. A
   positive (right-handed) rotation about +Y therefore reads as ANTI-clockwise
   on screen, and the sign has to be flipped to get the turn the puzzle asks
   for. See the step in valve_handles_update().

   30 degrees is 4096/12 = 341.33, stored as the ROUNDED 341: three steps land
   the wheel on 1023 of 4096, a fifth of a degree short of 90, which nothing can
   see on a 33-poly ring. Deriving each step from the step INDEX instead would
   be exact -- it is how the plain turn derives its angle from the remaining
   frames -- but it would make the RESTING angle between steps a division, and
   the pauses are the whole point of this animation. */
/* HOW FAR OUT THE WHEEL APPEARS, AND IT IS SOLVED AGAINST THE PUZZLE'S CAMERA
   RATHER THAN PICKED. valve_puzzle.c stands that camera VP_CAM_BACK (300) out
   along this same facing and VP_CAM_UP (260) above the mount, pitched
   atan(260/300) = 40.9deg down — so the wheel's RESTING position is dead centre
   of frame, and the further out it starts, the lower in frame it starts.

   The engine is gte_SetGeomScreen(256) on a 320x240 screen (main.c), so the
   VERTICAL half-field is atan(120/256) = 25.1deg. A wheel D out along the facing
   sits atan(260/(300-D)) below the horizontal, i.e. that minus 40.9deg below the
   centre line, and its 45-unit radius projects to 45*256/range pixels. At
   D = 160 that put the centre 20.8deg down — y = 217 of 240 — with a 39-pixel
   radius, so the bottom third of the wheel was OFF THE SCREEN for the first half
   of the slide.

   100 keeps it inside: 11.5deg below centre, y = 172, radius 35, so the whole
   ring is in frame from the first frame and travels up to centre as it goes in.
   >>> CHANGE VP_CAM_BACK, VP_CAM_UP OR VP_CAM_PITCH AND THIS HAS TO BE RESOLVED.
   <<< The three of them and this one are a single piece of arithmetic. */
#define VALVE_FIT_DIST         100  /* how far out along the facing it appears */
#define VALVE_FIT_IN_FRAMES     36  /* 0.6 s of slide                          */
#define VALVE_FIT_HOLD          30  /* "a brief pause" before the first turn   */
#define VALVE_FIT_STEPS          3
#define VALVE_FIT_STEP_DEG     341  /* 30 degrees of 4096                      */
/* HOW LONG ONE 30-DEGREE TURN TAKES. 16 frames is a bit over a quarter of a
   second, and it is the SECOND value: the first was 8, which at 60 fps was
   quick enough that the three steps read as one blurred movement rather than as
   three separate wrenches. The pause between them (VALVE_FIT_STEP_PAUSE) is
   deliberately left where it was, so halving the speed lengthened the turns
   without softening the stop-start rhythm that makes them read as "sharp". */
#define VALVE_FIT_STEP_FRAMES   16  /* a turn: a bit over a quarter second     */
#define VALVE_FIT_STEP_PAUSE    22  /* stillness after each turn, the last one
                                       included, so the script does not end on
                                       the frame the wheel stops moving        */
#define VALVE_FIT_FRAMES  (VALVE_FIT_IN_FRAMES + VALVE_FIT_HOLD +              \
                           VALVE_FIT_STEPS * (VALVE_FIT_STEP_FRAMES +          \
                                              VALVE_FIT_STEP_PAUSE))

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
    /* MOUNT ORIENTATION, AND WHY THERE ARE THREE ANGLES RATHER THAN TWO. The
       draw loop builds the mount matrix from {rot_x, rot_y, rot_z}, and
       PSn00bSDK's RotMatrix composes Rx * Ry * Rz -- so Ry is applied to the
       model FIRST and leaves the stem (the model's +Y) exactly where it was.
       rot_y is therefore a ROLL about the stem and can never aim it, and rot_x
       alone can only swing the stem within the YZ plane:

         rot_x = -1024   stem -> world -Z   (the Greenhouse's pipe, a +Z face)
         rot_x = +1024   stem -> world +Z   (Maze One's pipe, a -Z face)
         rot_z = -1024   stem -> world +X   (Maze Two's and the Chain Room's,
                                             both -X faces)

       >>> rot_z EXISTS BECAUSE TWO OF THE FOUR PIPES ARE APPROACHED ALONG X.
       <<< Without it those two are unmountable, not merely mis-posed. */
    int32_t   rot_x, rot_y, rot_z;
    /* THE OUTWARD FACING, as unit components on the two ground axes: the way the
       wheel looks, i.e. AWAY from the pipe and back at the player. It is the
       opposite of the stem, and the fit slide runs along it. Stored rather than
       pulled out of the mount matrix every frame because every pipe in the game
       is square to the world grid; exactly one of the two is non-zero. */
    int32_t   face_x, face_z;
    int32_t   present;        /* 1 = a handle is fitted here                   */
    int32_t   spin;           /* current wheel angle, 0..4095                  */
    int32_t   turn_timer;     /* frames left in an active turn; 0 = at rest    */
    int32_t   fit_timer;      /* frames left in a fit script; 0 = not fitting  */
    int32_t   offset;         /* how far out along the facing the wheel is
                                 drawn. Non-zero only during a fit's slide-in. */
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

/* Play the fit script on an EMPTY mount: the handle appears out along the
   facing, slides in, and takes its three sharp clockwise steps. Sets `present`
   itself. No-op on a mount that already holds a handle or is already fitting.
   valve_handle_fitting() reads 1 for VALVE_FIT_FRAMES from the call. */
void valve_handle_begin_fit(int mount);
int  valve_handle_fitting(int mount);

#endif
