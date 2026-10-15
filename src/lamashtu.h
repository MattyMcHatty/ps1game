#ifndef LAMASHTU_H
#define LAMASHTU_H

#include <stdint.h>
#include "render.h"

/* =========================================================================
   LAMASHTU — the body. Chapter 3's boss, in the Lamashtu Arena.
   =========================================================================
   >>> THIS FILE IS THE MODEL, THE ANIMATION AND THE ONE WAY TO HURT HER. IT IS
   NOT THE FIGHT. <<< She stands where she is placed and plays her two clips back
   to back for ever - IDLE, then SHRIEK, then IDLE again - and nothing else
   happens. The fight, when it is written, drives this file the way
   src/asag_fight.c drives src/asag.c: by asking the body questions, not by
   moving it in here.

   ---- WHERE SHE STANDS ------------------------------------------------------
   "Five squares in front of where the player starts", on the arena's own grid:
   the hall is laid out in 600-unit squares (src/lamashtu_arena.h - pillars 600
   across, aisles 600 wide), the player arrives in the first one (x[0,600],
   spawn x=220, z=1500, facing +X) and the fifth is x[2400,3000] on the same
   z[1200,1800] door aisle - the crossing of the door aisle with the second
   north-south aisle, the green marker's spot on the first sheet of
   misc/Lamashtu fight plan. Its centre is (2700, 1500).

   >>> AND THE GRID IS HER SIZE, WHICH IS THE CHECK THAT IT IS THE RIGHT GRID.
   <<< Her body is a 600-unit block (+-300 in the bind pose, +-324 at the widest
   frame of the shriek) with her head and arms reaching another ~290 forward,
   so she fills that one crossing exactly and leans out along the door aisle
   toward the player. The floor's 200-unit tiles would have put her a third of
   the way down the same aisle, half inside the first column of pillars.

   ---- WHICH WAY SHE FACES ---------------------------------------------------
   The mesh faces -Z: the polys UV-mapped onto the face in textures/lamashtu.png
   (u~64 v~10) sit at z=-404, on the head, on the -Z side. Her anus - the one
   small square low on her BACK - sits at z=+313 on the other side, which is the
   same answer from the other end. Both are corroborated, as
   tools/ADDING_A_3D_ENEMY.txt STEP 4 asks.

   She is turned a quarter so -Z looks down the hall at the door (-X), which
   makes her back +X. That turn and the placement are applied ONCE PER FRAME
   while the clip is unpacked (lamashtu.c's body_verts), not as a GTE model
   matrix: the turn is an exact quarter, so it is a swap and a negate, and every
   consumer of the pose - the draw, the collision, the anus target - then reads
   one block of WORLD-space vertices and no view matrix is ever replaced or has
   to be restored. Asag's shape (src/asag.h), reached by a different route.

       world x = LMS_X + local z
       world y = LMS_FLOOR_Y + local y
       world z = LMS_Z - local x

   ---- THE FOG ---------------------------------------------------------------
   Gula's rule (src/gula.h): she is drawn UNFOGGED and UNCULLED at any range, so
   from the doorway, 2000 units off with the room's fog-far at 1300, she stands
   in the dark at the end of the hall at full strength. Nothing else changes -
   the room's own fog and view distance are untouched, so the hall around her
   stays exactly as dark as it was.

   ---- THE ONLY WAY TO HURT HER ----------------------------------------------
   The single square low on her back, prim 30 of the .smd (found by geometry at
   load, not by that index - see lamashtu.c). It glows Asag's crown-pad blue at
   all times, with the same pulse (src/asag.c's ASAG_GLOW_* block), and a hit on
   it from the Crucifaxe, the Grave-olver or the Helluminator takes 1 of her
   LMS_MAX_HEALTH. Nowhere else on her takes damage, and the spot is only a
   target from BEHIND her: a round fired at her face does not pass through her
   to find it. Every weapon is at 1x - 10 swings, 10 rounds, or 10 seconds of
   lantern.

   AT 0 SHE DOES NOT DIE YET. There is no death sequence until the fight is
   built; she stops taking damage and goes on playing her clips. Every arrival
   puts her back at full health (lamashtu_reset from the arena's init), and
   nothing about her is saved.

   ---- MEMORY: ROOM-SCOPED ---------------------------------------------------
   lamashtus_load_model() is called from main.c's STATE_LOADING when the
   destination is the arena, AFTER its texture upload, and lamashtus_free_model()
   on every OTHER transition, up with the Rabisu's and Asag's frees - the shape
   tools/ADDING_THE_ASAG_FIGHT.txt PART 6 prescribes.

       LAMASHTU.SMD   156 verts, 139 prims             8,084 bytes
       LMSIDLE.PVA    24 frames, PVA2                 22,476
       LMSSHRK.PVA    36 frames, PVA2                 33,708
       pose scratch   156 SVECTORs                     1,248
                                                      -------
                                                      65,516 at the door

   The clips are baked by tools/export_lamashtu.py at step 2 (24 fps authored,
   12 played): re-bake through that script, never the Blender UI, which still
   writes the unpacked PVA1.

   ---- THE SKIN --------------------------------------------------------------
   textures/lamashtu.tim, 128x128 4bpp at x576 y0 (the crib's page - no crib
   stands in the arena), CLUT (640,502). The export was made at texture size
   128 (its UVs run 0..128) and the arena draws under a 128 texture window, so
   the 256-square art in textures/lamashtu/ is shipped halved
   (textures/lamashtu/lamashtu_128.png, on its own 16-colour palette). Registered
   through texmgr like the crib, uploaded by the arena's own uploader.
   ========================================================================= */

/* ---- Placement: the centre of her block, on the arena's y=0 floor --------- */
#define LMS_X              2700
#define LMS_Z              1500
#define LMS_FLOOR_Y           0

/* ---- Extents over EVERY frame of both clips (the bakes' union boxes, in the
   model's own frame before the turn; -Y is up). Her feet sit 14 below the
   origin, so she sinks 14 into the floor - invisible, and cheaper than a foot
   offset nothing else needs. */
#define LMS_HEIGHT          686   /* y[-672, 14]: the shriek's crown at -672,
                                     128 under the arena's -800 ceiling       */
#define LMS_HALF_W          324   /* the shriek's widest frame, local x      */

#define LMS_MAX_HEALTH       10

/* 12 fps: the clips are baked at step 2 of 24, and 60/12 is whole, so this is
   a plain countdown (Asag's 8 fps needed an accumulator; this does not). */
#define LMS_ANIM_TICKS        5

/* Frames of red on a landed hit - Asag's and the Rabisu's modulation. */
#define LMS_FLASH_FRAMES      8

typedef enum {
    LMS_CLIP_IDLE = 0,
    LMS_CLIP_SHRIEK,
    LMS_CLIP_COUNT
} LamashtuClip;

/* ---- Lifetime ------------------------------------------------------------- */
void lamashtu_load_assets(void);     /* startup: register the skin (header only) */
void lamashtu_upload_texture(void);  /* room entry: one LoadImage from RAM        */
void lamashtus_load_model(void);     /* STATE_LOADING, destination is the arena   */
void lamashtus_free_model(void);     /* STATE_LOADING, every other destination    */
void lamashtu_reset(void);           /* full health, IDLE from frame 0            */

void lamashtu_update(void);          /* area-gated: the clips, the flash          */
void lamashtu_draw(RenderContext *ctx);   /* under the room's plain view matrix   */

/* The player's push-out, from apply_collision_reception: an AABB over this
   frame's posed vertices at the player's height, Asag's collider exactly - "her
   own mesh for collision". Area-gated inside. */
void lamashtu_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* ---- Weapons: the anus, and nothing else --------------------------------- */
/* 1 and the target cylinder (centre, half-width, half-height) in the shape
   weapon_aim_in_circle takes, when the spot can be hit from where the player is
   standing: she is here, alive, and the player is on her BACK side. 0 with
   nothing written otherwise. */
int  lamashtu_anus_target(int32_t *x, int32_t *cy, int32_t *z,
                          int32_t *half_w, int32_t *half_h);
void lamashtu_damage(int dmg);
int  lamashtu_try_hit(void);         /* the crucifaxe; 1 if it landed            */

int  lamashtu_health(void);

#endif /* LAMASHTU_H */
