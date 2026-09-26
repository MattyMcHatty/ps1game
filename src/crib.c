#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "collision.h"      /* GROUND_FLOOR_Y */
#include "texmgr.h"
#include "title.h"          /* current_area gate */
#include "world.h"          /* world_room_index() — the solved set's key       */
#include "creep.h"          /* what the encounter pours out                    */
#include "crucifaxe.h"      /* SWING_RANGE, for cribs_try_hit                   */
#include "sound.h"          /* SFX_CREEP — the encounter's loop                 */
#include "crib.h"

/* Crib — see crib.h for what it is, for the whole encounter timeline, for why
   its collision comes out of its own mesh, and for why the solved set is a
   WorldDelta bitmask keyed by room rather than a GameFlag. */

typedef struct {
    GameState area;                        /* only draws/collides in this room  */
    int32_t   x, y, z, rot_y;              /* centre in plan; world y = y+GROUND_FLOOR_Y */
    int32_t   min_x, max_x, min_z, max_z;  /* world AABB, baked at place time   */
    int       active;

    /* ---- the encounter (crib.h) ---- */
    int       encounter;   /* 1 = this is its room's live cot; 0 = scenery      */
    CribState state;
    int32_t   tick;        /* frames in the current state                       */
    int       spawned;     /* Creeps released so far, 0..CRIB_CREEP_TOTAL       */
    int32_t   tilt;        /* current rock angle, PS1 units; 0 = level          */
} Crib;

static Crib cribs[MAX_CRIBS];
static int  crib_count = 0;

/* ---- The solved set -------------------------------------------------------
   One bit per room, keyed by world_room_index(). It lives here rather than in
   world.c's WorldState because it is the crib's own fact and because keeping it
   module-scope means world.c touches it through two accessors and not through a
   struct field it would then have to keep in step.

   >>> IT IS NOT PART OF ANY INSTANCE. <<< cribs_clear() empties the array on
   every room entry and crib_place() rebuilds it, so an instance is the wrong
   place for anything that has to outlive a door. It is also why crib_place()
   does not cache the answer: it reads the mask to pick a starting state, and
   cribs_update() re-reads it every frame, so the order of room init,
   world_enter() and savegame_apply_pending() cannot produce a cot that thinks
   it is unsolved because the save had not landed yet. */
static uint64_t crib_solved_rooms = 0;

uint64_t crib_solved_mask(void)            { return crib_solved_rooms; }
void     crib_solved_mask_set(uint64_t m)  { crib_solved_rooms = m; }

int crib_room_solved(GameState area) {
    /* world_room_bit(), not a shift of the mask: a 64-bit shift by a variable
       needs libgcc and there is none — see the note in src/world.h. */
    return (crib_solved_rooms & world_room_bit(world_room_index(area))) != 0;
}

static SMD  *crib_smd = NULL;
static void *crib_buf = NULL;

/* The collision mesh, and it is the SAME mesh the draw uses: these six are read
   off crib_smd's vertex array at load and are the only description of the prop's
   solid volume anywhere in the game. MODEL SPACE, and signed — see crib.h for
   why they are measured rather than assumed symmetric even though this model
   happens to be. */
static int32_t cr_min_x = 0, cr_max_x = 0;   /* -175 .. 175  as authored */
static int32_t cr_min_z = 0, cr_max_z = 0;   /* -100 .. 100             */
static int32_t cr_min_y = 0, cr_max_y = 0;   /* -195 ..   0, -Y is up   */

/* >>> THERE IS NO cr_deck_y ANY MORE, AND THAT IS A SIMPLIFICATION WORTH
   KNOWING ABOUT. <<< The creeps used to come out of the EIGHT corners of the
   cot's big box portion, which needed the box's underside as well as its rim —
   and the only way to get it was "the second distinct y in the vertex array
   counting from the top", a derivation that min/max cannot be wrong about but
   that one is: an export putting detail geometry between the rim and the deck
   would have moved it silently. The three spawn points are all at the rim now
   (see crib_release), so the measurement and its sharp edge are both gone. Do
   not reintroduce it for a spawn point that could be expressed as a fraction of
   the box instead.

   The prop's own texture. Deferred like the rest of the chapter's art: the
   header is read at startup, the pixels only when TEXBANK_CATACOMBS is selected
   at the catacomb mouth. */
static int crib_tex = -1;

/* The player's head, relative to cam_y — the same figure apply_collision_*
   uses for its own body span, so the vertical test below agrees with the walls'.
   Feet are cam_y + GROUND_FLOOR_Y. */
#define CR_PLAYER_HEAD 30

static void *read_file(const char *name) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)name)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    void *buf = malloc(sectors * 2048);
    if (!buf) return NULL;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);
    return buf;
}

/* Startup. The CD read is the geometry; the texture is a registration only —
   one sector of TIM header, no pixels — so Chapters 1 and 2 pay nothing for it
   (src/texmgr.h). */
void crib_load_assets(void) {
    /* BANK: Chapter 3 and nothing else. Derived, not guessed — py
       tools/check_tex_banks.py walks the uploader call graph (this module is
       reached from room_of_arms_upload_textures) and fails the build if this
       mask is short. */
    texmgr_set_bank(TEXBANK_CATACOMBS);

    crib_buf = read_file("\\TEXCTCMB\\CRIB.SMD;1");
    if (crib_buf) crib_smd = smdInitData(crib_buf);

    /* MEASURE THE MESH. This is the whole of the prop's collision authoring:
       the box below is the model's real bounding volume, so it can never drift
       from what is drawn. No symmetry is assumed — see crib.h. */
    if (crib_smd && crib_smd->n_verts > 0) {
        int i;
        cr_min_x = cr_max_x = crib_smd->p_verts[0].vx;
        cr_min_y = cr_max_y = crib_smd->p_verts[0].vy;
        cr_min_z = cr_max_z = crib_smd->p_verts[0].vz;
        for (i = 1; i < crib_smd->n_verts; i++) {
            int32_t vx = crib_smd->p_verts[i].vx;
            int32_t vy = crib_smd->p_verts[i].vy;
            int32_t vz = crib_smd->p_verts[i].vz;
            if (vx < cr_min_x) cr_min_x = vx;
            if (vx > cr_max_x) cr_max_x = vx;
            if (vy < cr_min_y) cr_min_y = vy;
            if (vy > cr_max_y) cr_max_y = vy;
            if (vz < cr_min_z) cr_min_z = vz;
            if (vz > cr_max_z) cr_max_z = vz;
        }
    }

    crib_tex = texmgr_register("\\TEXCTCMB\\CRIB.TIM;1");
}

/* Room entry: pure LoadImage out of the RAM copy area_bank_sync() has already
   read. No CD access, so it is safe inside main's STATE_LOADING. Called from
   room_of_arms_upload_textures(). */
void crib_upload_texture(void) {
    texmgr_upload(crib_tex);
}

void cribs_clear(void) { crib_count = 0; }

void crib_place(GameState area, int32_t x, int32_t y, int32_t z, int32_t rot_y) {
    if (crib_count >= MAX_CRIBS) return;

    /* THE FIRST CRIB PLACED IN AN AREA IS THAT ROOM'S ENCOUNTER; any later one
       in the same area is scenery and never wakes. crib.h argues this: the
       solved set is one bit per ROOM, and one bit cannot describe two
       independent encounters. The sweep is over the instances placed so far,
       which is the whole array — cribs_clear() runs on every room entry, so
       "already placed in this area" and "already placed" are the same question
       in practice, and writing it as the area test keeps it correct if that
       ever stops being true. */
    int i, first_here = 1;
    for (i = 0; i < crib_count; i++)
        if (cribs[i].active && cribs[i].area == area) { first_here = 0; break; }

    Crib *c = &cribs[crib_count++];
    c->area  = area;
    c->x = x;  c->y = y;  c->z = z;
    c->rot_y = rot_y;
    c->active = 1;

    c->encounter = first_here;
    /* Start posed from the saved set. A scenery cot is parked in SOLVED so that
       every "is this thing inert" test is one state comparison rather than a
       state comparison and an `encounter` test — cribs_try_hit() is the one
       place that has to tell the two apart, and it checks `encounter` there. */
    c->state   = (!first_here || crib_room_solved(area)) ? CRIB_SOLVED : CRIB_IDLE;
    c->tick    = 0;
    c->spawned = 0;
    c->tilt    = 0;

    /* World AABB = the axis-aligned bound of the rotated mesh footprint, corner
       by corner, exactly as the lever, the sconce and the oil dispenser bake
       theirs. Computed once here rather than per frame, and from the MEASURED
       min/max above rather than from a constant somebody has to remember to
       update. The four corners are written out in full instead of as ±hw/±hd so
       the arithmetic still holds for a re-export whose footprint is NOT centred
       on the origin it rotates about.

       THIS IS THE FUNCTION THE MOVEMENT WILL CALL AGAIN. A crib that rocks
       moves rot_y (or x/z) and re-derives its box from here; nothing else in
       the module holds a world coordinate. */
    int32_t cs = icos(rot_y), sn = isin(rot_y);
    const int32_t lx[4] = { cr_min_x, cr_max_x, cr_max_x, cr_min_x };
    const int32_t lz[4] = { cr_min_z, cr_min_z, cr_max_z, cr_max_z };
    int k;
    for (k = 0; k < 4; k++) {
        /* Same handedness as the RotMatrix Y rotation the draw uses. */
        int32_t wx = x + ((lx[k] * cs + lz[k] * sn) >> 12);
        int32_t wz = z + ((lz[k] * cs - lx[k] * sn) >> 12);
        if (k == 0) {
            c->min_x = c->max_x = wx;
            c->min_z = c->max_z = wz;
        } else {
            if (wx < c->min_x) c->min_x = wx;
            if (wx > c->max_x) c->max_x = wx;
            if (wz < c->min_z) c->min_z = wz;
            if (wz > c->max_z) c->max_z = wz;
        }
    }
}

/* ==========================================================================
   THE ENCOUNTER
   Read the timeline at the top of crib.h before changing any number here.
   ========================================================================== */

/* Where the rock is in its cycle, as a tilt about the model's X axis in PS1
   angle units. `amp` is the current peak, which the states below damp; the
   phase is the state tick, so a rock always STARTS level and a damped one
   always ENDS level (CRIB_ECHO_FRAMES is exactly one CRIB_ROCK_PERIOD, and the
   closing outro is a fraction of one). */
static int32_t crib_rock(int32_t tick, int32_t amp) {
    int32_t phase = ((tick % CRIB_ROCK_PERIOD) * 4096) / CRIB_ROCK_PERIOD;
    return (amp * isin(phase & 4095)) >> 12;
}

/* Beam brightness, 0..256, from the instance's state and tick. 256 is "full",
   which the draw then scales CRIB_BEAM_PEAK by — keeping the ramp in a fixed
   256 scale rather than in colour units means the peak can be retuned without
   touching the timing.

   >>> THE RAMP IS CUBIC, AND FIXING THAT DID NOT INVOLVE CHANGING THE DURATION.
   <<< It was LINEAR, and it looked like it hit full brightness in about a second
   against the three CRIB_BEAM_RAMP has always specified. The duration was never
   wrong; the CURVE was, because ADDITIVE BLENDING IS NOT LINEAR IN PERCEIVED
   BRIGHTNESS and this shaft draws two walls that sum. At a third of the way
   through, a linear ramp is at level 85, which the two layers turn into 170 of
   255 — two thirds of the way to white already, with two seconds still to run.

   Cubing the fraction moves the growth into the back half where it belongs:

      t (of the 3s)   linear level   summed   cubic level   summed
      1/3             85             67%      9             7%
      2/3             171            100%     76            60%
      1               256            sat      256           sat

   So raising CRIB_BEAM_RAMP would have been the wrong fix — it would have made
   the beam take six seconds to LOOK like three. If the build still reads as too
   fast, raise the exponent here before touching the timing.

   The closing fade takes the same curve, mirrored, for the same reason: a linear
   fade out of an additive volume holds near-white for most of its length and then
   appears to snap off at the end. */
static int32_t crib_beam_level(const Crib *c) {
    int32_t num, den;

    if (c->state == CRIB_ACTIVE) {
        if (c->tick >= CRIB_BEAM_RAMP) return 256;
        num = c->tick;            den = CRIB_BEAM_RAMP;
    } else if (c->state == CRIB_CLOSING) {
        num = CRIB_CLOSING_FRAMES - c->tick;  den = CRIB_CLOSING_FRAMES;
    } else {
        return 0;
    }

    /* 256 * (num/den)^3, staged so nothing overflows an int32 and nothing is
       truncated to zero early: num is at most 180 and den at most 180, so
       num*num*256 peaks around 8.3 million and the two divides that follow keep
       the intermediate in range. Multiplying by 256 FIRST is what stops
       (num/den) collapsing to 0 in integer arithmetic. */
    return (((num * num * 256) / (den * den)) * num) / den;
}

/* Release one Creep from one of THREE spawn points, chosen at random. In MODEL
   space, so all three follow the instance's own yaw and a cot standing at any
   rotation spills from the right places:

     0  the CENTRE of the cot, in plan
     1  a little outside it on one long side
     2  a little outside it on the other

   >>> "LEFT" AND "RIGHT" ARE THE LONG SIDES, i.e. +/- Z. <<< The footprint is
   350 along X and 200 along Z, so X is the head-and-foot axis and Z is the pair
   of long sides a cot reads as having a left and a right. If the brief meant the
   ENDS instead, this is a one-line change: swap the lz table below for an lx one
   and take CRIB_SPAWN_OUT off cr_min_x / cr_max_x. Nothing else in the file
   depends on which axis it is.

   ALL THREE ARE AT THE RIM (cr_min_y), the cot's top face — the same face the
   beam comes out of, and the height the eight corners this replaced already used
   for half their draws. It is ONE height now rather than two, which is what lets
   creep_spawn()'s anchor argument stay safely above the floor (src/creep.h).

   THE ROCK IS DELIBERATELY NOT IN THIS TRANSFORM. Folding the tilt in would move
   each spawn by at most the amplitude times the box half-height — under 60 units
   — and would tie a gameplay position to an animation phase for no gain. The
   DRAW applies the tilt, because that is what has to look right. */
#define CRIB_SPAWN_OUT 60   /* how far outside the long side points 1 and 2 sit */

static void crib_release(Crib *c) {
    int32_t cx = (cr_min_x + cr_max_x) / 2;
    int32_t cz = (cr_min_z + cr_max_z) / 2;
    int     k  = (int)((uint32_t)rand() % 3u);

    /* Centre in plan for all three; only the Z offset differs. Measured off the
       mesh rather than written as literals, so a re-export that changes the
       cot's depth moves the two outside points with it. */
    int32_t lx = cx;
    int32_t lz = (k == 0) ? cz
               : (k == 1) ? cr_min_z - CRIB_SPAWN_OUT
                          : cr_max_z + CRIB_SPAWN_OUT;
    int32_t ly = cr_min_y;

    int32_t cs = icos(c->rot_y), sn = isin(c->rot_y);
    int32_t wx = c->x + ((lx * cs + lz * sn) >> 12);
    int32_t wz = c->z + ((lz * cs - lx * sn) >> 12);
    /* THE CORNER'S WORLD Y — where the creep's body emerges. Model y is an
       offset from the floor the prop stands on, and entity y is the same space
       as mesh y (tools/ADDING_AN_ENEMY.txt STEP 2), so this is an addition and
       NOT a GROUND_FLOOR_Y conversion.

       >>> NOTHING IS SUBTRACTED FROM IT. <<< It used to take off
       (CRP_Y_OFFSET + CRP_HALF_H) to stand a creep's FEET on the corner. A creep
       FLOATS now and has no feet: creep_spawn() interpolates the body from here
       up or down to the hover height (src/creep.h). */
    int32_t wy = c->y + GROUND_FLOOR_Y + ly;

    /* ...and the ANCHOR, which is a different height and is this prop's own `y`.
       The field is a floor reference defined by world y = y + GROUND_FLOOR_Y, so
       the standing anchor for that floor — (surface) - GROUND_FLOOR_Y — is the
       same number back again. Passing the CORNER here instead is the bug
       src/creep.h's creep_spawn() note is about: the deck corners sit below the
       anchor and apply_ddog_height will not lift a body that starts below a
       floor, so half the spawn points ended up under the world.

       A full pool is counted as a spawn anyway. creep_spawn() returns -1 and
       places nothing when MAX_CREEPS is reached, and if that did not count the
       crib would sit at nine forever waiting for a tenth it can never make. The
       encounter's end condition is "ten released and none alive", so a dropped
       release resolves it rather than hanging it. */
    creep_spawn(wx, wz, c->y, wy, c->area);
    c->spawned++;
}

void cribs_update(void) {
    int i;
    for (i = 0; i < crib_count; i++) {
        Crib *c = &cribs[i];
        /* current_area, NEVER game_state — the two differ the moment the
           inventory menu opens, and gating on game_state would let the player
           pause a running encounter with Start (tools/ADDING_AN_ENEMY.txt
           STEP 6). The creeps keep moving under the menu, so the crib that is
           counting them has to as well. */
        if (!c->active || c->area != current_area) continue;

        switch (c->state) {
        case CRIB_IDLE:
        case CRIB_SOLVED:
            c->tilt = 0;
            break;

        case CRIB_ECHO:
            /* One rock, damped linearly to nothing over exactly one period. */
            c->tilt = crib_rock(c->tick,
                                (CRIB_ROCK_AMP * (CRIB_ECHO_FRAMES - c->tick))
                                / CRIB_ECHO_FRAMES);
            if (++c->tick >= CRIB_ECHO_FRAMES) {
                c->state = CRIB_SOLVED;
                c->tick  = 0;
                c->tilt  = 0;
            }
            break;

        case CRIB_ACTIVE:
            c->tilt = crib_rock(c->tick, CRIB_ROCK_AMP);

            /* THE LOOP, RE-KEYED IN C AND NOT IN HARDWARE. tick is 0 on the
               first ACTIVE frame, so this keys the clip on with the rock and
               again every CRIB_LOOP_FRAMES — which is the sample's own length,
               so there is no seam. It keeps going through the whole pour AND
               through the wait for the last body, because tick keeps counting
               in both. crib.h says why a hardware loop is not an option and
               sound.c's note on voice 20 says it at length.

               ONE SITE, ON PURPOSE: cribs_try_hit() deliberately does NOT play
               it when it wakes the cot. Doing both would double-key the voice on
               whichever frame ordering puts the swing before this update, and a
               single frame of delay is inaudible. */
            if (c->tick % CRIB_LOOP_FRAMES == 0) sound_play(SFX_CREEP);

            /* The cadence is FIXED: the timer runs whatever the player is doing
               and whatever is still alive, so all ten are out in SEVENTEEN AND A
               HALF seconds — 180 of beam ramp, 60 of hold, then nine intervals
               of CRIB_SPAWN_INTERVAL — and falling behind is punished hard. The
               interval was 180 frames and is now 90, which halves the room the
               player has to clear bodies in and is the whole difficulty knob on
               this encounter; the ten-body total and the beam's opening 4
               seconds are untouched. Written as an exact frame match
               rather than a countdown so the schedule cannot drift, and guarded
               on `spawned` so the last interval does not release an eleventh. */
            if (c->spawned < CRIB_CREEP_TOTAL) {
                int32_t due = (int32_t)CRIB_BEAM_RAMP + CRIB_BEAM_HOLD
                            + (int32_t)CRIB_SPAWN_INTERVAL * c->spawned;
                if (c->tick == due) crib_release(c);
            } else if (creeps_alive_in(c->area) == 0) {
                /* Ten released and the room is clear: the encounter is over.
                   >>> THE BIT GOES ON HERE, AT THE START OF THE OUTRO. <<< See
                   crib.h — a player who leaves during the one second of the
                   light going out has still beaten it. */
                crib_solved_rooms |= world_room_bit(world_room_index(c->area));
                /* And the voice goes with the beam. sound_stop() keys 20 off
                   rather than letting the clip play out, so the room falls quiet
                   on the frame the tenth body drops instead of carrying up to
                   six seconds of it into the outro and past it. */
                sound_stop(SFX_CREEP);
                c->state = CRIB_CLOSING;
                c->tick  = 0;
            }
            c->tick++;
            break;

        case CRIB_CLOSING:
            c->tilt = crib_rock(c->tick,
                                (CRIB_ROCK_AMP * (CRIB_CLOSING_FRAMES - c->tick))
                                / CRIB_CLOSING_FRAMES);
            if (++c->tick >= CRIB_CLOSING_FRAMES) {
                c->state = CRIB_SOLVED;
                c->tick  = 0;
                c->tilt  = 0;
            }
            break;
        }
    }
}

void cribs_rest(void) {
    int i;
    for (i = 0; i < crib_count; i++) {
        Crib *c = &cribs[i];
        if (!c->active) continue;
        /* >>> SILENCE THE LOOP HERE TOO, AND IT IS NOT BELT-AND-BRACES. <<< This
           is the abandonment path — a room change, a death, a save — and it is
           the ONLY one that covers a player who walks out mid-pour. Without it
           the voice is left keyed on with nothing updating it, and it would play
           out its remaining seconds over the door animation and into the next
           room, where nothing would ever re-key or stop it. Unconditional
           because a crib not in CRIB_ACTIVE has no voice keyed on and keying off
           an idle one costs a register write. */
        sound_stop(SFX_CREEP);
        /* Re-read the set rather than trusting the state: a crib that reached
           CRIB_CLOSING has already banked its bit, so it settles into SOLVED
           here, while one abandoned mid-pour goes back to IDLE with the whole
           thing to do again. Scenery has `encounter` clear and is parked in
           SOLVED already, and this lands it back there. */
        c->state   = (c->encounter && !crib_room_solved(c->area))
                   ? CRIB_IDLE : CRIB_SOLVED;
        c->tick    = 0;
        c->spawned = 0;
        c->tilt    = 0;
    }
}

int cribs_try_hit(void) {
    int i;
    for (i = 0; i < crib_count; i++) {
        Crib *c = &cribs[i];
        if (!c->active || c->area != current_area) continue;

        /* SCENERY IS SKIPPED RATHER THAN REPORTED AS HIT, so the same swing
           stays live for anything else in the room — the rule every module-side
           hit test in this game keeps (the Living Statue's note in
           src/crucifaxe.c is the long version). Same for a cot that is already
           moving: an encounter in progress absorbs nothing. */
        if (!c->encounter) continue;
        if (c->state != CRIB_IDLE && c->state != CRIB_SOLVED) continue;

        /* Reach to the box's SURFACE, by clamping the camera into the baked
           AABB and measuring to the point that comes back. Measuring to the
           CENTRE cannot work here and crib.h has the arithmetic: the player is
           held 75 clear of a box with a 194 plan half-extent, so a corner
           approach is a Manhattan 538 from the centre against SWING_RANGE's
           350, and the cot would be unhittable from exactly the place the
           alcove makes it easiest to stand. */
        int32_t nx = cam_x < c->min_x ? c->min_x : (cam_x > c->max_x ? c->max_x : cam_x);
        int32_t nz = cam_z < c->min_z ? c->min_z : (cam_z > c->max_z ? c->max_z : cam_z);

        /* The vertical span comes off the mesh exactly as cribs_collide's does:
           -Y is up, so the underside is cr_max_y and the top rail cr_min_y,
           both offsets from the floor the prop was placed on. Clamping cam_y
           into that span and measuring the remainder means a player on a
           walkway above a cot cannot swing down through the floor at it. */
        int32_t floor_y = c->y + GROUND_FLOOR_Y;
        int32_t top     = floor_y + cr_min_y;   /* the top rail */
        int32_t bot     = floor_y + cr_max_y;   /* the feet     */
        int32_t ny      = cam_y < top ? top : (cam_y > bot ? bot : cam_y);

        int32_t dx = nx - cam_x, dy = ny - cam_y, dz = nz - cam_z;
        int32_t dist3d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy)
                       + (dz < 0 ? -dz : dz);
        if (dist3d >= CRIB_HIT_REACH) continue;

        /* Facing, measured to the box CENTRE and not to the clamped point: a
           player standing right against the side has a clamped delta of almost
           zero and its dot says nothing about which way they are looking. */
        int32_t fx  = ((c->min_x + c->max_x) >> 1) - cam_x;
        int32_t fz  = ((c->min_z + c->max_z) >> 1) - cam_z;
        int32_t dot = ((int32_t)fx * isin(cam_rot) +
                       (int32_t)fz * icos(cam_rot)) >> 12;
        if (dot <= 0) continue;

        if (c->state == CRIB_IDLE) {
            c->state   = CRIB_ACTIVE;
            c->tick    = 0;
            c->spawned = 0;
        } else {
            c->state = CRIB_ECHO;       /* solved: one rock and nothing else */
            c->tick  = 0;
        }
        return 1;
    }
    return 0;
}

/* Player push-out against the baked box, Minkowski-expanded by the caller's
   radius and resolved along the shallowest axis — the dresser's scheme. Area-
   gated, so the shared collision routine calls it unconditionally. */
void cribs_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < crib_count; i++) {
        Crib *c = &cribs[i];
        if (!c->active || c->area != current_area) continue;

        /* Vertical gate, and it too comes out of the mesh. -Y is up, so the
           cot's UNDERSIDE is cr_max_y (the floor it stands on) and its top rail
           is cr_min_y, both offsets from the floor the prop was placed on; the
           player's body spans feet to head, and only an overlap blocks. This
           prop sits ON its own floor, so unlike the oil dispenser's the test is
           very nearly a formality here — it earns its keep only in a multi-storey
           room, where it is what stops a cot on the lower floor from blocking
           the walkway above it. */
        int32_t floor_y   = c->y + GROUND_FLOOR_Y;
        int32_t solid_bot = floor_y + cr_max_y;   /* the feet of the cot   */
        int32_t solid_top = floor_y + cr_min_y;   /* its top rail          */
        int32_t feet = py + GROUND_FLOOR_Y, head = py - CR_PLAYER_HEAD;
        if (head >= solid_bot || feet <= solid_top) continue;

        int32_t min_x = c->min_x - radius, max_x = c->max_x + radius;
        int32_t min_z = c->min_z - radius, max_z = c->max_z + radius;
        if (*px <= min_x || *px >= max_x) continue;
        if (*pz <= min_z || *pz >= max_z) continue;

        int32_t pl = *px - min_x, pr = max_x - *px;
        int32_t pf = *pz - min_z, pb = max_z - *pz;
        int32_t m = pl, ddx = -pl, ddz = 0;
        if (pr < m) { m = pr; ddx =  pr; ddz = 0; }
        if (pf < m) { m = pf; ddx = 0; ddz = -pf; }
        if (pb < m) {         ddx = 0; ddz =  pb; }
        *px += ddx; *pz += ddz;
    }
}

/* ---- The beam ------------------------------------------------------------
   A rectangular shaft standing on the cot's TOP FACE — poly 8 and poly 9 of
   Crib.smx, which together are exactly the measured box's lid, so the shaft's
   footprint is cr_min/cr_max and is DERIVED rather than authored.

   Called from inside cribs_draw()'s per-instance loop with that instance's
   model matrix still loaded, so every vertex below is in MODEL SPACE and the
   whole thing rocks with the cot for free.

   Read the long note in crib.h before touching the subdivision, the peak
   brightness, or the decision to draw both walls: each of those three is an
   answer to something (the GPU's 1023-pixel primitive drop, additive
   saturation, and what an additive volume looks like from inside) rather than a
   preference. */
static void crib_draw_beam(RenderContext *ctx, const Crib *c) {
    int32_t level = crib_beam_level(c);
    if (level <= 0) return;

    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;

    /* The lid's four corners in plan, walked as a PERIMETER. The four faces are
       (0,1), (1,2), (2,3), (3,0) — a ring, not index order. Index order would
       cut across the middle of the lid and the "shaft" would be two crossed
       sheets. src/incinerator.c's glow frame documents the same trap. */
    const int32_t px[4] = { cr_min_x, cr_max_x, cr_max_x, cr_min_x };
    const int32_t pz[4] = { cr_min_z, cr_min_z, cr_max_z, cr_max_z };

    int32_t base_y = cr_min_y;                      /* the lid; -Y is up  */
    /* The cot's plan centre, which the flare pushes the corners AWAY from. Taken
       off the measured box rather than assumed to be the model origin: this mesh
       happens to be centred there, and an export that was not would otherwise
       have the shaft splay sideways as it rose instead of symmetrically. */
    int32_t fcx = (cr_min_x + cr_max_x) / 2;
    int32_t fcz = (cr_min_z + cr_max_z) / 2;
    int     face, col, row;

    for (face = 0; face < 4; face++) {
        int a = face, b = (face + 1) & 3;
        for (col = 0; col < CRIB_BEAM_COLS; col++) {
            /* Interpolate along this face's edge, so the columns share their
               boundary exactly and the shaft has no seam down it. */
            int32_t t0 = col, t1 = col + 1;
            int32_t x0 = px[a] + (px[b] - px[a]) * t0 / CRIB_BEAM_COLS;
            int32_t z0 = pz[a] + (pz[b] - pz[a]) * t0 / CRIB_BEAM_COLS;
            int32_t x1 = px[a] + (px[b] - px[a]) * t1 / CRIB_BEAM_COLS;
            int32_t z1 = pz[a] + (pz[b] - pz[a]) * t1 / CRIB_BEAM_COLS;

            for (row = 0; row < CRIB_BEAM_ROWS; row++) {
                /* -Y is up, so a row further along the shaft is MORE negative. */
                int32_t y_lo = base_y - (int32_t)CRIB_BEAM_HEIGHT * row       / CRIB_BEAM_ROWS;
                int32_t y_hi = base_y - (int32_t)CRIB_BEAM_HEIGHT * (row + 1) / CRIB_BEAM_ROWS;

                /* The fall-off, linear from the lid to the far end, times the
                   ramp. Computed per ROW rather than per quad so the rows agree
                   at the boundary they share and the gradient is continuous. */
                int32_t f_lo = (CRIB_BEAM_ROWS - row)       * 256 / CRIB_BEAM_ROWS;
                int32_t f_hi = (CRIB_BEAM_ROWS - row - 1)   * 256 / CRIB_BEAM_ROWS;
                int32_t c_lo = (((int32_t)CRIB_BEAM_PEAK * f_lo) >> 8) * level >> 8;
                int32_t c_hi = (((int32_t)CRIB_BEAM_PEAK * f_hi) >> 8) * level >> 8;

                /* THE FLARE. Each row's two edges get their own plan scale,
                   interpolated from 256 (1x, at the rim) to CRIB_BEAM_TOP_SCALE
                   at the far end, and applied to each corner's offset from the
                   cot's plan centre. Computed per ROW — like the colours above —
                   so the row below and the row above agree exactly on the edge
                   they share and the cone has no step in its silhouette. */
                int32_t s_lo = 256 + ((int32_t)(CRIB_BEAM_TOP_SCALE - 256) * row)       / CRIB_BEAM_ROWS;
                int32_t s_hi = 256 + ((int32_t)(CRIB_BEAM_TOP_SCALE - 256) * (row + 1)) / CRIB_BEAM_ROWS;

                int32_t x0lo = fcx + ((x0 - fcx) * s_lo >> 8);
                int32_t z0lo = fcz + ((z0 - fcz) * s_lo >> 8);
                int32_t x1lo = fcx + ((x1 - fcx) * s_lo >> 8);
                int32_t z1lo = fcz + ((z1 - fcz) * s_lo >> 8);
                int32_t x0hi = fcx + ((x0 - fcx) * s_hi >> 8);
                int32_t z0hi = fcz + ((z0 - fcz) * s_hi >> 8);
                int32_t x1hi = fcx + ((x1 - fcx) * s_hi >> 8);
                int32_t z1hi = fcz + ((z1 - fcz) * s_hi >> 8);

                SVECTOR v[4];
                v[0].vx = (int16_t)x0lo; v[0].vy = (int16_t)y_lo; v[0].vz = (int16_t)z0lo; v[0].pad = 0;
                v[1].vx = (int16_t)x1lo; v[1].vy = (int16_t)y_lo; v[1].vz = (int16_t)z1lo; v[1].pad = 0;
                v[2].vx = (int16_t)x0hi; v[2].vy = (int16_t)y_hi; v[2].vz = (int16_t)z0hi; v[2].pad = 0;
                v[3].vx = (int16_t)x1hi; v[3].vy = (int16_t)y_hi; v[3].vz = (int16_t)z1hi; v[3].pad = 0;

                DVECTOR sv[4];
                int32_t sz[4], otz;

                gte_ldv3(&v[0], &v[1], &v[2]);
                gte_rtpt();
                gte_stsxy3c(sv);
                gte_ldv0(&v[3]);
                gte_rtps();
                gte_stsxy(&sv[3]);
                gte_stsz4c(sz);
                if (!sz[1] || !sz[2] || !sz[3]) continue;

                /* The room loops' coordinate clamp. A vertex past the GPU's
                   +/-1023 would WRAP rather than clip, and the far end of a
                   700-unit shaft is exactly the kind of vertex that gets there.
                   This is a different guard from the 1023-pixel EXTENT limit
                   the subdivision answers; both are needed. */
                int k, off = 0;
                for (k = 0; k < 4; k++) {
                    if (sv[k].vx <= -1023 || sv[k].vx >= 1023 ||
                        sv[k].vy <= -1023 || sv[k].vy >= 1023) { off = 1; break; }
                }
                if (off) continue;

                gte_avsz4();
                gte_stotz(&otz);
                if (otz <= 0) continue;
                /* Sorted at true scene depth with the room's clamps — no +40
                   bias, for the reason the cot's own polys carry none (see the
                   long note in the primitive loop below). */
                if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
                if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

                if (ctx->next_packet + sizeof(POLY_G4) > buf_end) return;

                POLY_G4 *poly = (POLY_G4 *)ctx->next_packet;
                setPolyG4(poly);
                setSemiTrans(poly, 1);
                /* Bottom pair lit, top pair dimmer — and black at the very top,
                   which additive blending turns into nothing at all, so the
                   shaft needs no mask to end on. */
                setRGB0(poly, (uint8_t)c_lo, (uint8_t)c_lo, (uint8_t)c_lo);
                setRGB1(poly, (uint8_t)c_lo, (uint8_t)c_lo, (uint8_t)c_lo);
                setRGB2(poly, (uint8_t)c_hi, (uint8_t)c_hi, (uint8_t)c_hi);
                setRGB3(poly, (uint8_t)c_hi, (uint8_t)c_hi, (uint8_t)c_hi);
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
                addPrim(&ot[otz], poly);
                ctx->next_packet += sizeof(POLY_G4);

                /* ADDITIVE MEANS A DR_TPAGE PER QUAD HERE, not one for the
                   group. The incinerator's glow can queue one page select for
                   its whole frame because every band lands in ONE OT bucket; a
                   700-unit shaft spans many, and a page select only governs the
                   primitives processed after it within its own bucket. So each
                   quad carries its own, added AFTER it — the OT is LIFO within
                   a bucket, so last in is processed first, and the page select
                   has to be processed before the quad it applies to.

                   >>> AND NOTHING RESTORES THE BLEND MODE AFTERWARDS. <<< That
                   is the house pattern (src/lightswitch_puzzle.c's light cones,
                   src/helluminator.c's flame, src/incinerator.c's glow): every
                   textured primitive in this game sets its own tpage anyway, so
                   there is nothing downstream to corrupt. */
                if (ctx->next_packet + sizeof(DR_TPAGE) > buf_end) return;
                DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
                setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
                addPrim(&ot[otz], tp);
                ctx->next_packet += sizeof(DR_TPAGE);
            }
        }
    }
}

/* Render every instance in the current area. Textured-prim path with per-poly
   UVs from the SMD (the model is one texture, so the tpage/clut are the same for
   every face and there is no tex map to keep in step). The untextured branches
   are kept because the loop branches on the primitive's own texture bit, which
   costs nothing and survives a re-export that leaves a flat face behind.

   THE CALLER OWNS THE TEXTURE WINDOW and this prop is happy either way, which is
   true of the 60-poly export and was not true of the 100-poly one it replaced:
   crib.tim sits at Voff 0 (x576 y0, tools/VRAM_MAP.txt) so there is nothing to
   bracket, and every UV is now inside u[2,127] v[2,124] so nothing needs wrapping
   either. The Room of Arms sets a 128 window for its own mesh art and this prop
   draws correctly under it. See crib.h — a re-UV past 127 makes that window
   load-bearing again.

   The whole matrix is rebuilt from the instance's fields every frame, which is
   what will make the movement a write to rot_y and nothing more (crib.h). */
void cribs_draw(RenderContext *ctx) {
    if (!crib_smd) return;

    MATRIX view;
    camera_build_view(&view);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint16_t tp = texmgr_tpage(crib_tex);
    uint16_t cl = texmgr_clut(crib_tex);

    int i;
    for (i = 0; i < crib_count; i++) {
        Crib *c = &cribs[i];
        if (!c->active || c->area != current_area) continue;

        /* Cull at the ROOM's fog-far, not at a constant of this module's own —
           the Room of Arms' view distance is a live number the Helluminator
           moves between 1600 and 3200, and g_fog_far is whatever the area draw
           set a few lines before calling us (render.h). Then through the room's
           lights, so a cot standing inside a sconce's reach appears and fades
           exactly where the wall behind it does. It registers no light of its
           own. */
        int32_t dcx = c->x - cam_x, dcz = c->z - cam_z;
        int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
        dist = render_light_dist(c->x, c->z, dist);
        if (dist > g_fog_far) continue;

        /* TWO ROTATIONS, COMPOSED EXPLICITLY, and the order is the whole point.
           The TILT is the rock, about the model's own X — its LONG axis, the
           350-unit one, so the cot tips side to side the way a cradle on curved
           runners does. The YAW then stands that already-rocking cot in the
           room.

           >>> ONE RotMatrix CALL WITH {tilt, rot_y, 0} WOULD BE WRONG HERE. <<<
           PSn00bSDK builds Rx * Ry * Rz (its header prints the product), so the
           yaw is applied FIRST and the tilt is then about the WORLD x axis. At
           this prop's rot_y of 512 — 45 degrees — that is 45 degrees off the
           cot's long axis, and it would rock on the diagonal. MulMatrix0(a,b,c)
           gives c = a * b, so yaw * tilt applies the tilt first, in model space.
           src/rabisu.c's foot-slash lean is the same composition for the same
           reason; src/valve_handle.c's note is the general statement.

           Rebuilt from the instance's fields every frame, which is what makes
           the rock a write to c->tilt and nothing more (crib.h). */
        MATRIX yaw_m, tilt_m, m, combined;
        SVECTOR yaw_r  = {0, (int16_t)c->rot_y, 0, 0};
        SVECTOR tilt_r = {(int16_t)c->tilt, 0, 0, 0};
        RotMatrix(&yaw_r,  &yaw_m);
        RotMatrix(&tilt_r, &tilt_m);
        MulMatrix0(&yaw_m, &tilt_m, &m);
        VECTOR pos = {c->x, c->y + GROUND_FLOOR_Y, c->z};
        TransMatrix(&m, &pos);
        CompMatrixLV(&view, &m, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        int32_t fog_factor = render_fog_scale(dist);

        uint8_t *p = (uint8_t *)crib_smd->p_prims;
        int pi;
        for (pi = 0; pi < crib_smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt       = (SMD_PRI_TYPE *)p;
            uint8_t       stride   = pt->len;
            int           is_quad  = (pt->type >= 2);
            int           textured = pt->texture;

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &crib_smd->p_verts[vi[0]];
            SVECTOR *v1 = &crib_smd->p_verts[vi[1]];
            SVECTOR *v2 = &crib_smd->p_verts[vi[2]];

            DVECTOR sv[4];
            int32_t sz[4], otz, nclip;

            gte_ldv3(v0, v1, v2);
            gte_rtpt();
            gte_stsxy3c(sv);

            if (sv[0].vx <= -1023 || sv[0].vx >= 1023 || sv[0].vy <= -1023 || sv[0].vy >= 1023 ||
                sv[1].vx <= -1023 || sv[1].vx >= 1023 || sv[1].vy <= -1023 || sv[1].vy >= 1023 ||
                sv[2].vx <= -1023 || sv[2].vx >= 1023 || sv[2].vy <= -1023 || sv[2].vy >= 1023) {
                p += stride; continue;
            }

            if (!pt->nocull) {
                gte_nclip();
                gte_stopz(&nclip);
                if (nclip <= 0) { p += stride; continue; }
            }

            gte_stsz4c(sz);
            if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

            if (is_quad) {
                SVECTOR *v3 = &crib_smd->p_verts[vi[3]];
                gte_ldv0(v3);
                gte_rtps();
                gte_stsxy(&sv[3]);
                gte_stsz(&sz[3]);
                if (sv[3].vx <= -1023 || sv[3].vx >= 1023 || sv[3].vy <= -1023 || sv[3].vy >= 1023) { p += stride; continue; }
                if (sz[3] == 0) { p += stride; continue; }
                gte_avsz4();
            } else {
                gte_avsz3();
            }

            gte_stotz(&otz);
            if (otz <= 0) { p += stride; continue; }
            /* >>> NO +40. <<< The room mesh biases every one of its polys 40
               buckets deeper into the OT; a prop that took the same bias sorted
               in the same slot as the wall it is set against, and a tie is a
               LOSS for the prop — the room queues its mesh before it calls us
               and addPrim pushes to the head of the bucket, so within one slot
               the later-added primitive is drawn FIRST and painted over. Sorting
               at true scene depth lifts the prop 40 buckets, and a bucket is 4
               world units (the arithmetic is in src/catacomb_doors.c), so this is
               a 160-unit lift — comfortably more than this prop's own 200-unit
               depth needs at the 30-unit standoff it is placed at, which is what
               it has to beat for no part of it to fall behind the wall it is set
               against. The sconce's and the oil dispenser's notes spell the same
               rule out at length.

               It costs no correctness: a wall GENUINELY in front of the cot is
               hundreds of buckets nearer and still occludes it. The clamp is not
               decoration — a prop seen from close up lands in single digits
               without SCENE_OT_MIN. */
            if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
            /* Stay below the room's texture-window primitive at OT_LENGTH-1 so
               it is processed first, the same rule the room geometry keeps. */
            if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

            /* Fog on the room's ramp, saturating to the chapter's clear colour
               (7,6,9 — ROA_FOG_* in src/room_of_arms.c, and the same three
               numbers in all five Catacombs rooms). Hard-coded as every prop's
               is: there is no global for the colour, only for the distance, and
               this prop is a Chapter 3 fixture. */
            uint8_t *col = p + 16;
            uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + 7 * (256 - fog_factor)) >> 8);
            uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + 6 * (256 - fog_factor)) >> 8);
            uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + 9 * (256 - fog_factor)) >> 8);

            if (is_quad && textured) {
                if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
                uint8_t *uv = p + 20;
                POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
                setPolyFT4(poly);
                setRGB0(poly, r, g, b);
                poly->tpage = tp;
                poly->clut  = cl;
                poly->u0=uv[0]; poly->v0=uv[1];
                poly->u1=uv[2]; poly->v1=uv[3];
                poly->u2=uv[4]; poly->v2=uv[5];
                poly->u3=uv[6]; poly->v3=uv[7];
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_FT4);
            } else if (is_quad) {
                if (ctx->next_packet + sizeof(POLY_F4) > buf_end) { p += stride; continue; }
                POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
                setPolyF4(poly);
                setRGB0(poly, r, g, b);
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_F4);
            } else if (textured) {
                if (ctx->next_packet + sizeof(POLY_FT3) > buf_end) { p += stride; continue; }
                uint8_t *uv = p + 20;
                POLY_FT3 *poly = (POLY_FT3 *)ctx->next_packet;
                setPolyFT3(poly);
                setRGB0(poly, r, g, b);
                poly->tpage = tp;
                poly->clut  = cl;
                poly->u0=uv[0]; poly->v0=uv[1];
                poly->u1=uv[2]; poly->v1=uv[3];
                poly->u2=uv[4]; poly->v2=uv[5];
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_FT3);
            } else {
                if (ctx->next_packet + sizeof(POLY_F3) > buf_end) { p += stride; continue; }
                POLY_F3 *poly = (POLY_F3 *)ctx->next_packet;
                setPolyF3(poly);
                setRGB0(poly, r, g, b);
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_F3);
            }

            p += stride;
        }

        /* The beam, LAST for this instance and inside its matrix — which is
           what makes it rock with the cot rather than stand still while the cot
           tips out from under it. */
        crib_draw_beam(ctx, c);
    }

    /* Back to the plain view matrix — whatever the caller draws next is in world
       space and must not inherit the last instance's model transform. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
