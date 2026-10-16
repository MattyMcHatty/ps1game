#ifndef LAMASHTU_H
#define LAMASHTU_H

#include <stdint.h>
#include <psxgte.h>
#include "render.h"

/* =========================================================================
   LAMASHTU — the body. Chapter 3's boss, in the Lamashtu Arena.
   =========================================================================
   >>> THIS FILE IS THE MODEL, HER ANIMATION AND HER TWO ATTACKS. THE
   ENCOUNTER IS src/lamashtu_boss.c. <<< tools/ADDING_A_BOSS_ENCOUNTER.txt
   STEP 1's split: this file is everything that would work unchanged if a
   second Lamashtu stood in another room - where she stands, which way she
   looks, how she fades, her clips, the one spot that can be hurt, the Creeps
   she pours and the shockwave she throws. The director decides WHEN: it places
   her for each phase, fades her, turns her attacks on and off, and watches for
   the hit that moves the fight on.

   ---- WHERE SHE STANDS ------------------------------------------------------
   Wherever lamashtu_place() puts her: the centre of her 600-unit block on the
   arena's y=0 floor, and one of four facings. The seven phase spots are the
   director's (misc/Lamashtu fight plan; src/lamashtu_boss.c). Her body is a
   600 block (+-300 in the bind pose, +-324 at the widest frame of the shriek)
   with her head and arms reaching ~290 further forward, so she fills one aisle
   crossing of the arena's 600 grid.

   ---- WHICH WAY SHE FACES ---------------------------------------------------
   The mesh faces -Z: the polys UV-mapped onto the face in textures/lamashtu.png
   (u~64 v~10) sit at z=-404, on the head; her anus - the one small square low
   on her BACK - is at z=+313. A facing turns the model in quarter turns only,
   so it is applied as a swap-and-negate while the clip is unpacked
   (lamashtu.c's body_verts): every consumer of the pose - the draw, the
   collision, the target, the shockwave's reach - reads one block of WORLD
   vertices and no GTE matrix is ever loaded or restored. Asag's shape
   (src/asag.h), reached by a different route. With f the facing's unit vector:

       world x = x - local_x * f.z - local_z * f.x
       world z = z + local_x * f.x - local_z * f.z
       world y = LMS_FLOOR_Y + local_y

   which is the identity for SOUTH (-Z, the model's own) and puts local -Z on f
   for the other three.

   ---- THE FOG ---------------------------------------------------------------
   Gula's rule (src/gula.h): she is drawn UNFOGGED and UNCULLED at any range;
   the room's own fog and view distance are untouched.

   ---- THE FADE ---------------------------------------------------------------
   0..256, and under 256 she is drawn ADDITIVE (ABR=1) with her skin
   modulated toward black - so at 0 she adds nothing over anything. Unlike
   Asag's and the Rabisu's, which drop to flat polys to fade, she KEEPS her
   skin: textures/lamashtu.tim's palette carries STP on every entry, which is
   what lets a textured poly be semi-transparent at all (Gula's sheet does the
   same). At 0 the draw is skipped.

   ---- THE ONLY WAY TO HURT HER ----------------------------------------------
   The single square low on her back (prim 30 of the .smd, found by geometry at
   load). It glows Asag's crown-pad blue while lamashtu_set_glow() says so, and
   a hit on it from the Crucifaxe, the Grave-olver or the Helluminator, from
   BEHIND her, while she is VULNERABLE, takes 1 of LMS_MAX_HEALTH - every
   weapon at 1x. The hit itself: the glow goes out, she becomes invulnerable,
   her attacks stop (the crying with them), EVERY LAMASHTU CREEP IN THE ROOM
   DIES, and she screams and shrieks once. Her Creeps start again from her new
   spot once she has faded back in.
   lamashtu_hit_taken() reports it to the director exactly once.

   ---- HER ATTACKS (lamashtu_set_aggressive) ----------------------------------
   CREEPS: a LAMASHTU CREEP (src/lms_creep.h) out of her body every
     LMS_SPAWN_FRAMES (4 s) while fewer than LMS_MAX_CREEPS (5) of them are
     alive in the room.
   THE CRY: SFX_LMS_CRY re-keyed from C for as long as she is aggressive.
   THE SHOCKWAVE: a player within LMS_WAVE_TRIGGER (300) of her body, IN
     FRONT OF HER OR TO HER SIDE, sets her shrieking and screaming, and a red
     ring races out of her. From BEHIND she never throws it - behind meaning
     past the plane of her back (LMS_BACK_DEPTH), which is where the player has
     to stand to reach the spot on it. It takes
     LMS_WAVE_DAMAGE (25) and shoves the player LMS_SHOVE_FRAMES x
     LMS_SHOVE_STEP (600) straight away from her, stopped early by a wall -
     Gula's shove (src/gula.c). Once the shriek has played out it can fire
     again if they are still in reach.

   ---- MEMORY: ROOM-SCOPED ---------------------------------------------------
   lamashtus_load_model() from main.c's STATE_LOADING when the destination is
   the arena, AFTER its texture upload; lamashtus_free_model() on every OTHER
   transition, with the Rabisu's and Asag's frees.

       LAMASHTU.SMD   156 verts, 139 prims             8,084 bytes
       LMSIDLE.PVA    24 frames, PVA2                 22,476
       LMSSHRK.PVA    36 frames, PVA2                 33,708
       pose scratch   156 SVECTORs                     1,248

   The clips are baked by tools/export_lamashtu.py at step 2 (24 fps authored,
   12 played): re-bake through that script, never the Blender UI.

   ---- THE SKIN --------------------------------------------------------------
   textures/lamashtu.tim, 128x128 4bpp at x576 y0 (the crib's page), CLUT
   (640,502) with STP set. Registered through texmgr, uploaded by the arena.
   ========================================================================= */

#define LMS_FLOOR_Y           0

/* ---- Extents over EVERY frame of both clips (the bakes' union boxes, in the
   model's own frame; -Y is up). */
#define LMS_HEIGHT          686   /* y[-672, 14]: 128 under the -800 ceiling */
#define LMS_HALF_W          324

/* Her face, in the model's own frame: the face poly's centre (prim 57,
   (-8,-513,-404)). The director aims its shots at this. */
#define LMS_FACE_LY      (-513)
#define LMS_FACE_LZ      (-404)

#define LMS_MAX_HEALTH       10

/* 12 fps: the clips are baked at step 2 of 24, and 60/12 is whole. */
#define LMS_ANIM_TICKS        5

/* Frames of red on a landed hit - Asag's and the Rabisu's modulation. */
#define LMS_FLASH_FRAMES      8

/* ---- The Creeps -------------------------------------------------------------
   "Up to a maximum of 5 on screen at one time", one every 4 seconds - the
   brief's 2, slowed to half the rate on the second pass. They come out of the
   middle of her block at chest height and float down to their own hover
   height (lms_creep_spawn's emerge). */
#define LMS_SPAWN_FRAMES    240
#define LMS_MAX_CREEPS        5
#define LMS_SPAWN_Y        (-300)

/* ---- The cry: lamashtu crying.mp3 is 1767 ADPCM blocks of 28 samples at
   11025 Hz = 4.49 s = 269 frames, re-keyed on the frame it ends. Re-cut the
   clip and this moves with it. */
#define LMS_CRY_FRAMES      270

/* ---- The shockwave ----------------------------------------------------------
   The trigger is measured from her BODY, not her centre: the collider holds the
   player 75 off her block, so "within 300 of her" is within 300 of that block's
   surface at the player's height (the same box lamashtu_collide pushes out of).
   The ring starts at her block's edge and sweeps out to LMS_WAVE_RADIUS -
   past anyone the trigger could have caught - in LMS_WAVE_FRAMES. */
#define LMS_WAVE_TRIGGER    300
/* "Behind" for the shockwave: the player's offset from her centre, measured
   along her facing, is further back than her back face. The bind pose's back
   is at local z +300..325 (the anus quad's plane is 299..325), so 300 puts
   every spot from which the anus is a target - lamashtu_anus_target's own
   "behind" - on the safe side, and leaves both flanks of her block covered. */
#define LMS_BACK_DEPTH      300
#define LMS_WAVE_START      300
#define LMS_WAVE_RADIUS    1400
#define LMS_WAVE_FRAMES      40   /* 0.67 s, ~28 units a frame              */
#define LMS_WAVE_BAND       180   /* radial width of the band on the floor  */
#define LMS_WAVE_HIT_BAND   200   /* the leading edge's catch, behind it    */
#define LMS_WAVE_WALL_H     220   /* the skirt on the leading edge          */
#define LMS_WAVE_SEGS        32
#define LMS_WAVE_DAMAGE      25   /* "deals 25 HP of damage" of MAX_HEALTH 100 */
/* "Knocks the player back 600": 15 frames of 40, a wall ends it early. */
#define LMS_SHOVE_STEP       40
#define LMS_SHOVE_FRAMES     15

typedef enum {
    LMS_CLIP_IDLE = 0,
    LMS_CLIP_SHRIEK,
    LMS_CLIP_COUNT
} LamashtuClip;

typedef enum {
    LMS_FACE_WEST = 0,   /* toward -X */
    LMS_FACE_EAST,       /* toward +X */
    LMS_FACE_SOUTH,      /* toward -Z: the model as authored */
    LMS_FACE_NORTH       /* toward +Z */
} LamashtuFacing;

/* ---- Lifetime ------------------------------------------------------------- */
void lamashtu_load_assets(void);     /* startup: register the skin (header only) */
void lamashtu_upload_texture(void);  /* room entry: one LoadImage from RAM        */
void lamashtus_load_model(void);     /* STATE_LOADING, destination is the arena   */
void lamashtus_free_model(void);     /* STATE_LOADING, every other destination    */
/* Full health, Idle, hidden, every switch off, any wave and shove cancelled.
   The director shows and places her. */
void lamashtu_reset(void);

void lamashtu_update(void);          /* area-gated: clips, attacks, wave, shove   */
void lamashtu_draw(RenderContext *ctx);   /* the body and the wave, plain view    */

/* ---- What the director writes ----------------------------------------------- */
void lamashtu_place(int32_t x, int32_t z, LamashtuFacing facing);
void lamashtu_set_visible(int on);   /* drawn AND solid; 0 = not in the room      */
void lamashtu_set_fade(int32_t fade);/* 256 solid .. 0 gone (additive below 256) */
int32_t lamashtu_fade(void);
void lamashtu_set_glow(int on);      /* the anus lamp                             */
void lamashtu_set_vulnerable(int on);/* the anus is a target                      */
void lamashtu_set_aggressive(int on);/* creeps, the cry and the shockwave         */
void lamashtu_set_frozen(int on);    /* hold the pose: the death's freeze         */
void lamashtu_set_shake(int32_t units);   /* DRAW only, never the pose            */
void lamashtu_set_burning(int on);   /* the death's red: the flash, held          */
void lamashtu_play_shriek(void);     /* once through, then back to Idle           */
int32_t lamashtu_shriek_ticks(void); /* its length in game frames (0 if absent)  */
/* 1, exactly once, after a hit has landed (and set the glow, the attacks and
   vulnerability off). The director decides what follows. */
int  lamashtu_hit_taken(void);
/* Stop the shockwave and the shove where they are - the death sequence. */
void lamashtu_cancel_wave(void);

/* ---- What the director reads ------------------------------------------------- */
int  lamashtu_model_loaded(void);
int  lamashtu_health(void);
/* A point in her own frame (as the .smx has it), turned and placed into the
   world by the same transform the draw uses - the face for the camera, any
   anchor for the death's lights. */
void lamashtu_local_to_world(int32_t lx, int32_t ly, int32_t lz, VECTOR *out);
/* Her facing as a unit vector in 4096ths, (fx, fz). */
void lamashtu_facing_vec(int32_t *fx, int32_t *fz);

/* The player's push-out, from apply_collision_reception: an AABB over this
   frame's posed vertices at the player's height - "her own mesh for
   collision". Area-gated inside; nothing while hidden. */
void lamashtu_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius);

/* ---- Weapons: the anus, and nothing else ----------------------------------- */
/* 1 and the target cylinder when the spot can be hit from where the player is
   standing: she is here, VULNERABLE, and the player is on her back side. */
int  lamashtu_anus_target(int32_t *x, int32_t *cy, int32_t *z,
                          int32_t *half_w, int32_t *half_h);
void lamashtu_damage(int dmg);
int  lamashtu_try_hit(void);         /* the crucifaxe; 1 if it landed            */

#endif /* LAMASHTU_H */
