#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "tim_slots.h"
#include "player.h"      /* game_flag / game_flag_set */
#include "btn_glyph.h"   /* BTN_CIRCLE */
#include "door.h"        /* door_draw_string_3d_yaw, DOOR_PIXEL_SIZE */
#include "cdaudio.h"     /* suspend/resume around the entry-time reads */
#include "title.h"       /* current_area: the leaves are solid in ONE room */
#include "hatch_doors.h"

/* The two leaves over the pit in The Hatch's yard. See hatch_doors.h for the
   placement arithmetic, the clip layout and why none of this is resident at
   startup. */

#define PVA_HEADER_SIZE  12
#define HD_LEFT   0
#define HD_RIGHT  1
#define HD_LEAVES 2

static const char *HD_MESH_FILE[HD_LEAVES] = {
    "\\TEX\\HATCHDL.SMD;1", "\\TEX\\HATCHDR.SMD;1",
};
static const char *HD_ANIM_FILE[HD_LEAVES] = {
    "\\TEX\\HATCHDL.PVA;1", "\\TEX\\HATCHDR.PVA;1",
};

static void     *hd_mesh_buff[HD_LEAVES];
static SMD      *hd_smd[HD_LEAVES];
static void     *hd_anim_buff[HD_LEAVES];
static SVECTOR  *hd_anim_frames[HD_LEAVES];   /* n_frames blocks of n_verts */
static int       hd_anim_count[HD_LEAVES];    /* 0 = pose on the bind pose  */

/* THE CLIP LENGTH THE CLOCK ACTUALLY RUNS ON: the SHORTER of the two, so the
   pair always stops on a frame both leaves have. They are authored 10 and 10, so
   this is 10 today and the min only matters if one is ever re-baked alone. 1
   when neither loaded, which pins the swing to a single held pose rather than
   letting it run off the end of a missing array. */
static int hd_frames = 1;

/* ---- Playback state --------------------------------------------------------
   ONE clock for the pair, not one each. The two leaves are halves of a single
   authored movement and there is nothing that could ever want them out of step;
   a per-leaf clock would only be a way for them to drift. */
static int hd_frame    = 0;   /* 0-based: 0 is authored frame 1, shut     */
static int hd_tick     = 0;
static int hd_swinging = 0;

/* Circle edge-detect, seeded by hatch_doors_init(). Starts "held" so a press
   carried in through the gate transition does not throw the doors open on the
   arrival frame -- the same contract the birdcage and every gate here keep. */
static int hd_circle_prev = 1;

/* ---- Reach and the sign ----------------------------------------------------
   BOTH ARE ON THE SOUTH SIDE, and the two have to agree: a prompt that appears
   where the press does not work is worse than no prompt. So the reach, the fade
   and the sign are all measured from ONE point, the middle of the pit's SOUTH
   lip.

   +Z IS NORTH in this room -- the passage into the chamber leaves the yard's
   north hedge at z(1500,2100) -- so south is -Z and the south lip is z=-300.
   The pit spans x(3000,4200), so its middle is x=3600, which is also the doors'
   own centre line.

   MEASURED FROM THE LIP, not from the doors' centre, even though on this side
   the two share an x. The player cannot get onto the pit -- walls 20..23 fence
   it -- so the nearest they ever stand is the default 195 wall radius back from
   z=-300, and the lip is the edge they actually walk up to. */
#define HD_LIP_X          3600
#define HD_LIP_Z         (-300)
#define HD_TRIGGER_RADIUS  600   /* 195 of that is the wall push itself       */
#define HD_TEXT_RADIUS    1200
#define HD_FADE_NEAR       800

/* = TH_TEXT_Y, the height every sign in The Hatch stands at over its y=0 lawn:
   the glyph TOP, so the line hangs at eye level rather than over the player's
   head. Not shared through a header because it is one number that has been the
   same in every garden room since Maze One. */
#define HD_TEXT_Y        (-186)

/* FACING SOUTH, back at a player standing on the lawn below the pit. The yaw is
   measured the way cam_rot is (0 = facing +Z, increasing toward +X), and
   door_draw_string_3d_yaw treats the value handed to it as the direction the
   VIEWER is looking -- passing cam_rot itself is the camera-facing billboard. A
   player who has walked round to the south side and turned to face the hole is
   looking north, i.e. +Z, i.e. cam_rot 0. So the yaw is 0, and the line reads
   left-to-right along +X, which is that player's right hand.

   FIXED rather than cam_rot itself, so the sign belongs to the pit's south edge:
   a player circling round to the north or east sees it edge-on and reads it from
   the side it is meant to be read from. */
#define HD_TEXT_YAW          0

/* The line floats just south of the leaves' own south edge (z=-310), between it
   and the closest the fence lets the player stand (z=-495). It reads over the
   lip rather than over the grass in front of it, and at 145 units from that
   standing point it sits at the same reading distance the room's gate sign does
   (184 from its own wall). x is the doors' centre line, so the line is centred
   on the hatch. */
#define HD_TEXT_X         3600
#define HD_TEXT_Z        (-350)

static void *hd_read_file(const char *name) {
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

/* One leaf's clip. Rejected -- and the leaf left on its bind pose, which is the
   shut frame -- unless the vertex count matches the .smd it will be indexed
   through. Positions are looked up by the .smd's polygon indices, so a clip
   baked from a different topology reads off the end of every frame; this is the
   check tools/ANIMATING_A_3D_MODEL.txt's mistake #5 is about. */
static void hd_load_anim(int leaf) {
    if (!hd_smd[leaf]) return;
    uint8_t *p = (uint8_t *)hd_read_file(HD_ANIM_FILE[leaf]);
    if (!p) return;

    /* Byte-wise: the file is little-endian and packed, and a struct would invite
       the compiler to pad it. */
    if (p[0] != 'P' || p[1] != 'V' || p[2] != 'A' || p[3] != '1') { free(p); return; }
    int n_verts  = p[4] | (p[5] << 8);
    int n_frames = p[6] | (p[7] << 8);
    if (n_verts != hd_smd[leaf]->n_verts || n_frames <= 0) { free(p); return; }

    hd_anim_buff[leaf]   = p;
    hd_anim_frames[leaf] = (SVECTOR *)(p + PVA_HEADER_SIZE);
    hd_anim_count[leaf]  = n_frames;
}

void hatch_doors_load(void) {
    hatch_doors_unload();          /* idempotent: re-entering the room twice
                                      must not leak the first load             */

    /* >>> THE CD-DA BRACKET IS MANDATORY, NOT DEFENSIVE. <<< A data read issued
       while CD-DA is streaming hangs the drive (tools/TEXTURE_STREAMING_DEBUG.txt),
       and these FOUR reads are not covered by anyone else's: room_arena_load
       brackets its own and RESUMES on the way out, so by the time
       the_hatch_load_geometry gets here the music is playing again. The gate
       trigger into this room stops the track first, but a title-screen load or a
       debug level-select jump can arrive with one running -- which is the case
       this is for. suspend/resume are no-ops when nothing is playing. */
    cdaudio_suspend();
    int i;
    for (i = 0; i < HD_LEAVES; i++) {
        hd_mesh_buff[i] = hd_read_file(HD_MESH_FILE[i]);
        hd_smd[i] = hd_mesh_buff[i] ? smdInitData(hd_mesh_buff[i]) : NULL;
        hd_load_anim(i);
    }
    cdaudio_resume();

    hd_frames = 1;
    for (i = 0; i < HD_LEAVES; i++) {
        if (hd_anim_count[i] <= 0) continue;
        if (hd_frames == 1 || hd_anim_count[i] < hd_frames)
            hd_frames = hd_anim_count[i];
    }
}

void hatch_doors_unload(void) {
    int i;
    for (i = 0; i < HD_LEAVES; i++) {
        if (hd_mesh_buff[i]) free(hd_mesh_buff[i]);
        if (hd_anim_buff[i]) free(hd_anim_buff[i]);
        hd_mesh_buff[i]   = NULL;
        hd_smd[i]         = NULL;
        hd_anim_buff[i]   = NULL;
        hd_anim_frames[i] = NULL;
        hd_anim_count[i]  = 0;
    }
    hd_frames = 1;
}

/* The vertex block a leaf is posed on this frame. Falls back to the .smd's own
   bind pose -- authored frame 1, the shut pose -- whenever the clip is missing
   or was rejected, so a bad .pva shows as doors that never open rather than as
   exploded geometry. */
static SVECTOR *hd_verts(int leaf) {
    if (!hd_anim_frames[leaf]) return hd_smd[leaf]->p_verts;
    int f = hd_frame;
    if (f < 0 || f >= hd_anim_count[leaf]) f = 0;
    return hd_anim_frames[leaf] + (f * hd_smd[leaf]->n_verts);
}

int hatch_doors_open(void)     { return !hd_swinging && hd_frame >= hd_frames - 1; }
int hatch_doors_swinging(void) { return hd_swinging; }

/* ---- Collision -------------------------------------------------------------
   THE FOOTPRINT IS READ OUT OF THE CLIP, ONE FRAME AT A TIME, rather than being
   two hand-written boxes for "shut" and "open". That is what makes the leaves
   solid THROUGHOUT the swing and not just at the ends of it: a leaf on its way
   over sweeps out onto the lawn, and a player standing where it lands is pushed
   clear by the door itself instead of being left standing inside it.

   It is also the only version that cannot go stale. Re-bake the clip with a
   longer swing or a different resting angle and the collision follows it; a pair
   of literal boxes would silently keep describing the old animation.

   >>> WHAT ACTUALLY MOVES IS X, AND ONLY X. <<< Each leaf hinges about an axis
   parallel to Z, so its z extent is [-310,310] on every frame of both clips and
   only the x span travels: the left leaf runs x[2980,3600] shut and x[2360,2981]
   open, the right x[3600,4220] shut and x[4214,4800] open. The scan below does
   not assume that -- it takes both axes off the vertices — but it is why the box
   is a fair description of a leaf at any angle rather than a gross overestimate:
   the only pose where an axis-aligned box is much bigger than the leaf is one
   standing on edge, and that is a single frame in the middle of the travel.

   NO VERTICAL GATE, deliberately. The Hatch is one flat floor at y=0 throughout
   (all eight collision planes agree), and the open leaves lie only 62 and 261
   above it — heights the player would step over in life and cannot here. Being
   stopped by them is the ask; py is ignored the way the concrete props ignore it
   for the same "single flat floor" reason.

   IT DOES NOT REPLACE THE PIT'S FENCE. Walls 20..23 still keep the player off
   the hole whether the doors are shut, moving or open, and this only ever adds
   to them. A shut leaf's box overlaps that fence almost exactly — 20 units of
   lip wider on each side — so on the shut frame this is very nearly a no-op, and
   the 10 units it takes off how close the player can stand to the south lip are
   well inside the 600-unit trigger reach.

   NOT IN THE GUN'S LINE OF SIGHT. There is no hatch_doors_point_solid and no
   family bit in collision.c's props_block_point: a shot fired over a pair of
   doors lying flat on the grass should not stop on them, and every bit added to
   that mask is paid for at every sample of every enemy sightline in the game
   (tools/DIAGNOSING_FRAME_RATE.txt STEP 3A). The valve handle sits out of that
   test for the same kind of reason. */

/* This frame's world-space x/z bounds for one leaf. 0 when the leaf is not
   loaded, which is every room but this one. */
static int hd_leaf_box(int leaf, int32_t *min_x, int32_t *max_x,
                       int32_t *min_z, int32_t *max_z)
{
    if (!hd_smd[leaf]) return 0;
    int n = hd_smd[leaf]->n_verts;
    if (n <= 0) return 0;

    SVECTOR *vp = hd_verts(leaf);
    int32_t x0 = vp[0].vx, x1 = vp[0].vx;
    int32_t z0 = vp[0].vz, z1 = vp[0].vz;
    int i;
    for (i = 1; i < n; i++) {
        if (vp[i].vx < x0) x0 = vp[i].vx;
        if (vp[i].vx > x1) x1 = vp[i].vx;
        if (vp[i].vz < z0) z0 = vp[i].vz;
        if (vp[i].vz > z1) z1 = vp[i].vz;
    }
    /* The clip is in the pair's own space; the same translation the draw applies
       through the GTE is applied here by hand. 40 vertices at most, once a
       frame — the loop is cheaper than keeping a table in step with the clip. */
    *min_x = x0 + HATCH_DOORS_X; *max_x = x1 + HATCH_DOORS_X;
    *min_z = z0 + HATCH_DOORS_Z; *max_z = z1 + HATCH_DOORS_Z;
    return 1;
}

/* Player push-out, the concrete props' Minkowski AABB with the smallest
   penetration chosen. Gated to The Hatch so the shared reception collision
   routine can call it unconditionally, exactly as every other prop family in
   that routine is.

   >>> A LEAF ONLY EVER PUSHES OUTWARD ALONG X, AND WITHOUT THAT THIS DEADLOCKS
   AGAINST THE PIT'S OWN FENCE. <<< The plain smallest-penetration rule the
   concrete props use is right for a block standing in open floor and wrong here,
   because the far side of each leaf is not open floor — it is the hole. Take the
   left leaf open, x[2360,2981] against the pit wall at x=3000: a player at the
   west lip sits inside its box, the shortest way out is EAST, and east is a
   19-unit slot against a wall that pushes straight back. The walls run before
   the props in apply_collision_reception, so the door gets the last word and the
   player is left standing over the hole, shoved back and forth every frame.

   The fix is a real invariant of the art rather than a patch: each leaf is
   hinged AT its own lip and everything solid about it lies away from the pit, on
   every frame of the clip. Shut, the left leaf reaches from x=2980 to the middle
   of the hole; open, it reaches from the same lip out onto the west lawn. So
   +X is never an escape from the LEFT leaf and -X is never one from the RIGHT,
   at any pose, and dropping those two candidates leaves three that all lead
   somewhere the player can stand. Going round an open leaf is then what the two
   Z pushes are for. */
void hatch_doors_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    (void)py;   /* single flat floor — see the note above */
    if (current_area != STATE_THE_HATCH) return;

    int leaf;
    for (leaf = 0; leaf < HD_LEAVES; leaf++) {
        int32_t min_x, max_x, min_z, max_z;
        if (!hd_leaf_box(leaf, &min_x, &max_x, &min_z, &max_z)) continue;

        min_x -= radius; max_x += radius;
        min_z -= radius; max_z += radius;

        if (*px <= min_x || *px >= max_x) continue;
        if (*pz <= min_z || *pz >= max_z) continue;

        int32_t push_l = *px - min_x;    /* west, out of the LEFT leaf   */
        int32_t push_r = max_x - *px;    /* east, out of the RIGHT leaf  */
        int32_t push_f = *pz - min_z;    /* south, round either          */
        int32_t push_b = max_z - *pz;    /* north, round either          */

        /* The two Z pushes are always candidates; the X one is only the leaf's
           own outward side. Seeded with it so there is always a candidate. */
        int32_t min_push, px_delta, pz_delta;
        if (leaf == HD_LEFT) { min_push = push_l; px_delta = -push_l; }
        else                 { min_push = push_r; px_delta =  push_r; }
        pz_delta = 0;

        if (push_f < min_push) { min_push = push_f; px_delta = 0; pz_delta = -push_f; }
        if (push_b < min_push) {                    px_delta = 0; pz_delta =  push_b; }

        *px += px_delta;
        *pz += pz_delta;
    }
}

void hatch_doors_init(void) {
    /* POSED OFF THE FLAG, which savegame_apply_pending has already restored by
       the time a room init runs. A save taken mid-swing comes back fully open
       rather than part way: the flag is set on the PRESS, and there is no second
       bit for "halfway", because a hatch caught between two frames is not a
       state worth a bit of the save blob. */
    hd_swinging = 0;
    hd_tick     = 0;
    hd_frame    = game_flag(FLAG_HATCH_DOORS_OPEN) ? hd_frames - 1 : 0;
    hd_circle_prev = interact_tapped();
}

/* Manhattan reach plus a facing test, as every other Circle prompt in the garden
   uses: doors behind you are not doors you are opening. */
static int lip_in_reach(void) {
    int32_t dx = cam_x - HD_LIP_X;
    int32_t dz = cam_z - HD_LIP_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < HD_TRIGGER_RADIUS && interact_facing(HD_LIP_X, HD_LIP_Z);
}

int hatch_doors_update(int lock) {
    /* THE CLOCK RUNS BEFORE THE PRESS IS READ, so the frame the swing starts on
       is the frame the press was taken and not the one after it. */
    if (hd_swinging) {
        if (++hd_tick >= HATCH_DOORS_ANIM_TICKS) {
            hd_tick = 0;
            /* ONE-SHOT: it stops on the last frame and holds it. A looping clip
               would wrap to 0 here, which on this one would slam the doors shut
               again every second. */
            if (++hd_frame >= hd_frames - 1) {
                hd_frame    = hd_frames - 1;
                hd_swinging = 0;
            }
        }
    }

    /* THE EDGE STATE IS KEPT UP TO DATE EVEN WHILE LOCKED, and that is the
       point of doing it before the lock test rather than after: a Circle held
       down across the menu closing must not read as a fresh press on the frame
       the lock lifts. */
    int held = interact_tapped();
    int just = held && !hd_circle_prev;
    hd_circle_prev = held;
    if (lock) return 0;

    /* The press is consumed either way -- the edge state is updated above -- but
       it only does anything on doors that are still shut and standing still. */
    if (!just || hd_swinging || hd_frame > 0) return 0;
    if (!lip_in_reach()) return 0;

    hd_swinging = 1;
    hd_tick     = 0;
    /* SET ON THE PRESS, not on the last frame of the swing. See the note in
       hatch_doors_init() on what a save taken mid-swing comes back as. */
    game_flag_set(FLAG_HATCH_DOORS_OPEN);
    return 1;
}

/* ---- The prompt -------------------------------------------------------------
   GONE ONCE THEY ARE MOVING, not once they are open: offering "Press O to open"
   over doors that are already swinging reads as a press that did not take. */
void hatch_doors_text(RenderContext *ctx) {
    if (hd_swinging || hd_frame > 0) return;

    int32_t dx = cam_x - HD_LIP_X;
    int32_t dz = cam_z - HD_LIP_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= HD_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > HD_FADE_NEAR) {
        int range = HD_TEXT_RADIUS - HD_FADE_NEAR;
        int prog  = xz - HD_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* The garden's sign green. The Circle control code inside the string
       supplies its own red and the fade still applies to both. */
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to open",
                            HD_TEXT_X, HD_TEXT_Y, HD_TEXT_Z,
                            50, 255, 50, fade, HD_TEXT_YAW, DOOR_PIXEL_SIZE);
}

/* ---- Drawing ---------------------------------------------------------------
   The room mesh's own prim loop with three changes:

     THE POSITIONS COME FROM THE CLIP, not from the .smd. The .smd still owns the
     polygon vertex INDICES, the baked colours and the UVs; only where those
     vertices are this frame comes from the .pva (hd_verts above).

     ONE TRANSLATION, NO ROTATION. The swing is baked into the clip, so the
     matrix is the pair's placement and nothing else -- there is no per-frame
     angle to compose. The leaves are handed to the GTE in the pit's own space
     and the matrix carries them to it.

     THE TEXTURE SLOT IS THE ROOM'S, not the one smxlink baked in. Both leaves
     are UV'd against `hatch`, which The Hatch streams to the trck_clue page at
     x640 y0 on entry -- the valve handle's trick, for the same reason. Their UVs
     run past 128, so they depend on the 128 texture window the_hatch_draw
     already sets for the whole frame; drawing them anywhere else needs that
     window set too. */
static void hd_draw_leaf(RenderContext *ctx, int leaf, MATRIX *view,
                         int32_t fog_factor)
{
    SMD *smd = hd_smd[leaf];
    if (!smd) return;

    MATRIX world_m;
    /* Identity rotation: the clip is already in the pair's shared space. */
    world_m.m[0][0] = ONE; world_m.m[0][1] = 0;   world_m.m[0][2] = 0;
    world_m.m[1][0] = 0;   world_m.m[1][1] = ONE; world_m.m[1][2] = 0;
    world_m.m[2][0] = 0;   world_m.m[2][1] = 0;   world_m.m[2][2] = ONE;

    VECTOR pos = { HATCH_DOORS_X, HATCH_DOORS_Y, HATCH_DOORS_Z };
    TransMatrix(&world_m, &pos);

    MATRIX combined;
    CompMatrixLV(view, &world_m, &combined);
    gte_SetRotMatrix(&combined);
    gte_SetTransMatrix(&combined);

    SVECTOR *vp = hd_verts(leaf);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint8_t *p = (uint8_t *)smd->p_prims;

    int i;
    for (i = 0; i < smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride  = pt->len;
        int     is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &vp[vi[0]];
        SVECTOR *v1 = &vp[vi[1]];
        SVECTOR *v2 = &vp[vi[2]];

        DVECTOR sv[4];
        int32_t sz[4];
        int32_t otz, nclip;

        gte_ldv3(v0, v1, v2);
        gte_rtpt();
        gte_stsxy3c(sv);

        if (sv[0].vx <= -1023 || sv[0].vx >= 1023 || sv[0].vy <= -1023 || sv[0].vy >= 1023 ||
            sv[1].vx <= -1023 || sv[1].vx >= 1023 || sv[1].vy <= -1023 || sv[1].vy >= 1023 ||
            sv[2].vx <= -1023 || sv[2].vx >= 1023 || sv[2].vy <= -1023 || sv[2].vy >= 1023) {
            p += stride; continue;
        }

        /* Backface cull. Safe at every frame of the swing: each leaf is one
           closed shell with no open edges (check_model_winding reports shells 1,
           open edges 0), so a leaf turned upside down at the end of its travel
           simply shows the face that is now on top. */
        if (!pt->nocull) {
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }

        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        SVECTOR *v3    = 0;
        int32_t  v2_sz = sz[3];   /* v2's SZ, before the quad path reuses sz[3] */
        if (is_quad) {
            v3 = &vp[vi[3]];
            gte_ldv0(v3);
            gte_rtps();
            gte_stsxy(&sv[3]);
            gte_stsz(&sz[3]);
            if (sv[3].vx <= -1023 || sv[3].vx >= 1023 ||
                sv[3].vy <= -1023 || sv[3].vy >= 1023) { p += stride; continue; }
            if (sz[3] == 0) { p += stride; continue; }
            gte_avsz4();
        } else {
            gte_avsz3();
        }

        gte_stotz(&otz);
        /* THE ROOM MESH'S SORTING RULE, and it has to be the same one: a shut
           leaf is a horizontal slab lying beside the horizontal lawn, and the
           lawn's own quads are sorted by their FARTHEST corner rather than by
           their average (see render.h). Sorting these by average would let a
           door and the grass beside it swap over as the camera moves. The +40
           bias is the room's too. */
        if (poly_is_flat_y(v0, v1, v2, v3))
            otz = is_quad ? otz_far4(sz[1], sz[2], v2_sz, sz[3])
                          : otz_far3(sz[1], sz[2], sz[3]);
        if (otz <= 0) { p += stride; continue; }
        otz += 40;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        uint8_t *col = p + 16;
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8);

        uint8_t *uv = p + 20;   /* UVs sit at +20 for FT3 and FT4 alike */

        if (is_quad && pt->texture) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, r, g, b);
            poly->tpage = TIM_TPAGE_HATCH;
            poly->clut  = TIM_CLUT_HATCH;
            poly->u0=uv[0]; poly->v0=uv[1];
            poly->u1=uv[2]; poly->v1=uv[3];
            poly->u2=uv[4]; poly->v2=uv[5];
            poly->u3=uv[6]; poly->v3=uv[7];
            poly->x0=sv[0].vx; poly->y0=sv[0].vy;
            poly->x1=sv[1].vx; poly->y1=sv[1].vy;
            poly->x2=sv[2].vx; poly->y2=sv[2].vy;
            poly->x3=sv[3].vx; poly->y3=sv[3].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_FT4);
        } else if (is_quad) {
            if (ctx->next_packet + sizeof(POLY_F4) > buf_end) { p += stride; continue; }
            POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
            setPolyF4(poly);
            setRGB0(poly, r, g, b);
            poly->x0=sv[0].vx; poly->y0=sv[0].vy;
            poly->x1=sv[1].vx; poly->y1=sv[1].vy;
            poly->x2=sv[2].vx; poly->y2=sv[2].vy;
            poly->x3=sv[3].vx; poly->y3=sv[3].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F4);
        } else if (pt->texture) {
            if (ctx->next_packet + sizeof(POLY_FT3) > buf_end) { p += stride; continue; }
            POLY_FT3 *poly = (POLY_FT3 *)ctx->next_packet;
            setPolyFT3(poly);
            setRGB0(poly, r, g, b);
            poly->tpage = TIM_TPAGE_HATCH;
            poly->clut  = TIM_CLUT_HATCH;
            poly->u0=uv[0]; poly->v0=uv[1];
            poly->u1=uv[2]; poly->v1=uv[3];
            poly->u2=uv[4]; poly->v2=uv[5];
            poly->x0=sv[0].vx; poly->y0=sv[0].vy;
            poly->x1=sv[1].vx; poly->y1=sv[1].vy;
            poly->x2=sv[2].vx; poly->y2=sv[2].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_FT3);
        } else {
            if (ctx->next_packet + sizeof(POLY_F3) > buf_end) { p += stride; continue; }
            POLY_F3 *poly = (POLY_F3 *)ctx->next_packet;
            setPolyF3(poly);
            setRGB0(poly, r, g, b);
            poly->x0=sv[0].vx; poly->y0=sv[0].vy;
            poly->x1=sv[1].vx; poly->y1=sv[1].vy;
            poly->x2=sv[2].vx; poly->y2=sv[2].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F3);
        }

        p += stride;
    }
}

void hatch_doors_draw(RenderContext *ctx) {
    if (!hd_smd[HD_LEFT] && !hd_smd[HD_RIGHT]) return;

    /* ONE distance for the pair, taken at the pit's centre and used for the fog
       on both leaves. The pair is 1240 wide, so a per-poly distance would be
       more honest -- but the room's fog is 575/2500 over a 5000-unit room and
       the whole prop sits inside a 620-unit half-width, which is a quarter of a
       fog step. The room mesh pays per-poly because its polys are 5000 units
       apart; these are not. */
    int32_t dx = HATCH_DOORS_X - cam_x;
    int32_t dz = HATCH_DOORS_Z - cam_z;
    int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (dist > g_fog_far + 700) return;      /* 700 = the pair's own reach     */
    int32_t fog_factor = render_fog_scale(dist);

    MATRIX view;
    camera_build_view(&view);

    hd_draw_leaf(ctx, HD_LEFT,  &view, fog_factor);
    hd_draw_leaf(ctx, HD_RIGHT, &view, fog_factor);

    /* Restore the plain view matrix so later world-space draws project right --
       the room's own gate sign is drawn after this. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
