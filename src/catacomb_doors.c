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
#include "cdaudio.h"     /* suspend/resume around the entry-time reads */
#include "title.h"       /* current_area: the leaves are solid in ONE room */
#include "player.h"      /* game_flag: FLAG_ASAG_DEAD poses them open on entry */
#include "catacomb_doors.h"

/* The two leaves in the catacomb mouth at the north end of Outside Catacombs.
   See catacomb_doors.h for why they carry no translation, what they replaced in
   the room mesh, and which texture slot they borrow. */

#define CD_LEFT   0
#define CD_RIGHT  1
#define CD_LEAVES 2

/* ---- WHY THE LEAVES OUTRANK THE WALL THEY STAND IN -------------------------
   The general case, the triage order and the art-side rule that prevents this
   from being needed at all: tools/DIAGNOSING_DEPTH_SORTING.txt. This room is
   that document's worked example, so the numbers below are repeated there.

   >>> THE PAIR SORTS 256 WORLD UNITS IN FRONT OF WHERE IT ACTUALLY IS. <<<
   Without this the black backing grid behind the mouth cut through them in
   streaks whenever the player looked at the doorway off-axis, and that is not
   a near-tie the OT could resolve with more buckets — the sort genuinely
   INVERTS.

   The PS1 sorts a poly by the average depth of its corners (gte_avsz4), which
   is one number for the whole poly, so what actually decides a pair is

       dviewz = dx * sin(theta)  +  dz * cos(theta)
                 ^ lateral            ^ the separation you built

   and the lateral term has nothing to do with depth. The leaves' front faces
   are at z=3787; the fifteen untextured backing quads are at z=3850, so
   dz = 63 — while dx between a leaf poly and a backing cell that overlaps it
   on screen runs to 275 and past it. The sort therefore flips at
   atan(dz/dx): 40 degrees for the pair at dx=75, and as little as 20 degrees
   for the leaf poly at x=-125 against the backing cell at x=-300. Different
   pairs flip at different angles, which is why it read as streaking rather
   than the leaf simply disappearing.

   Ties go to the WALL, not to chance: outside_catacombs_draw queues the room
   mesh before it calls us, and addPrim pushes to the head of the bucket, so
   within one OT slot the later-added primitive draws FIRST and gets painted
   over. The zero-crossing was a loss.

   64 buckets is 256 world units, and it is sized off the worst overlapping
   pair rather than picked round: the error to beat is
   (275 + 63*tan(t))*sin(t) - 63*cos(t), which is 195 units at 45 degrees and
   301 at 60. Past that the leaf is nearly edge-on and covers no screen area
   worth arguing about.

   IT IS SAFE BECAUSE NOTHING STANDS IN THAT SLAB. The 256 units south of the
   facade are inside the mouth's approach, which the room spawns nothing into —
   anything that did stand there would now be drawn behind a door it is in
   front of. The same bias is what sml_med.c buys with SML_MED_OT_BIAS 24
   against co-located crates; this one is larger because it is fighting a wall
   the leaves are set flush into rather than a crate they sit on.

   >>> THE CLAMP AT THE USE SITE IS NOT DECORATION. <<< Every other draw in the
   game only ever ADDS to otz, so +40 alone kept it clear of the menu's
   reserved OT range for free. Subtracting 64 does not, and a leaf seen from
   close up lands in single digits without it. */
#define CD_OT_BIAS 64

static const char *CD_MESH_FILE[CD_LEAVES] = {
    "\\TEX\\CTCMBDL.SMD;1", "\\TEX\\CTCMBDR.SMD;1",
};

static void *cd_mesh_buff[CD_LEAVES];
static SMD  *cd_smd[CD_LEAVES];

/* How far the pair has slid apart, in world units. See CD_SLIDE_FULL in the
   header. Applied OUTWARD, so the sign is per leaf and cd_leaf_off() is the one
   place that knows which way each one goes — the draw and the collision both go
   through it rather than each carrying their own -/+ . */
static int32_t cd_slide;

static int32_t cd_leaf_off(int leaf) {
    return leaf == CD_LEFT ? -cd_slide : cd_slide;
}

void catacomb_doors_set_slide(int32_t slide) {
    if (slide < 0)              slide = 0;
    if (slide > CD_SLIDE_FULL)  slide = CD_SLIDE_FULL;
    cd_slide = slide;
}

int32_t catacomb_doors_slide(void) { return cd_slide; }

/* ROOM ENTRY, after the flag restore. FLAG_ASAG_DEAD is set on the last frame
   of the scene that slides them (src/catacomb_open.h), so it is also the bit
   that says "these are open" — there is no half-way state to remember, because
   the whole scene runs inside one visit with no menu and no save in it. */
void catacomb_doors_init(void) {
    cd_slide = game_flag(FLAG_ASAG_DEAD) ? CD_SLIDE_FULL : 0;
}

static void *cd_read_file(const char *name) {
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

void catacomb_doors_load(void) {
    catacomb_doors_unload();       /* idempotent: re-entering the room twice
                                      must not leak the first load             */

    /* >>> THE CD-DA BRACKET IS MANDATORY, NOT DEFENSIVE. <<< A data read issued
       while CD-DA is streaming hangs the drive (tools/TEXTURE_STREAMING_DEBUG.txt),
       and these two reads are not covered by anyone else's: room_arena_load
       brackets its own and RESUMES on the way out, so by the time
       outside_catacombs_load_geometry gets here the music is playing again. The
       gate trigger into this room stops the track first, but a title-screen load
       or a debug level-select jump can arrive with one running — which is the
       case this is for. suspend/resume are no-ops when nothing is playing. */
    cdaudio_suspend();
    int i;
    for (i = 0; i < CD_LEAVES; i++) {
        cd_mesh_buff[i] = cd_read_file(CD_MESH_FILE[i]);
        cd_smd[i] = cd_mesh_buff[i] ? smdInitData(cd_mesh_buff[i]) : NULL;
    }
    cdaudio_resume();
}

void catacomb_doors_unload(void) {
    int i;
    for (i = 0; i < CD_LEAVES; i++) {
        if (cd_mesh_buff[i]) free(cd_mesh_buff[i]);
        cd_mesh_buff[i] = NULL;
        cd_smd[i]       = NULL;
    }
}

/* The vertex block a leaf is posed on this frame. Today that is always the
   .smd's own bind pose — the shut pose — because there is no clip. It is a
   function rather than a bare `cd_smd[leaf]->p_verts` at every use site so that
   adding one is a change to THIS function and nothing else; hd_verts() in
   src/hatch_doors.c is the shape it takes once a .pva is wired in. */
static SVECTOR *cd_verts(int leaf) {
    return cd_smd[leaf]->p_verts;
}

/* ---- Collision -------------------------------------------------------------
   THE FOOTPRINT IS SCANNED OFF THE LEAF'S VERTICES, not written out as a
   literal box. With no clip that is the same 26 vertices every frame and a
   constant would do — but it is what makes the pair follow a swing for free the
   day one is baked, and it cannot go stale if the art is re-exported a few
   units wider. 26 vertices a leaf, twice a frame; cheaper than a table anyone
   has to remember to update.

   NO VERTICAL GATE. Outside Catacombs is one flat floor at y=0 (all sixteen of
   the collision generator's planes agree) and the leaves run the full y[-910,0]
   from the ground to well over head height, so there is no pose in which the
   player's height decides anything. py is ignored the way the concrete props
   ignore it, for the same single-flat-floor reason.

   NOT IN THE GUN'S LINE OF SIGHT. No catacomb_doors_point_solid and no family
   bit in collision.c's props_block_point, for the reason hatch_doors.c gives:
   every bit added to that mask is paid for at every sample of every enemy
   sightline in the game (tools/DIAGNOSING_FRAME_RATE.txt STEP 3A), and nothing
   in this room shoots at anything through this doorway. */

/* This frame's world-space x/z bounds for one leaf. 0 when the leaf is not
   loaded, which is every room but this one. The vertices ARE world coordinates
   — see the "nowhere" note in catacomb_doors.h — so the ONLY thing added here
   is this frame's slide, and it goes on the X pair alone: the leaves move due
   west and due east and nothing about the slide touches Z. */
static int cd_leaf_box(int leaf, int32_t *min_x, int32_t *max_x,
                       int32_t *min_z, int32_t *max_z)
{
    if (!cd_smd[leaf]) return 0;
    int n = cd_smd[leaf]->n_verts;
    if (n <= 0) return 0;

    SVECTOR *vp = cd_verts(leaf);
    int32_t x0 = vp[0].vx, x1 = vp[0].vx;
    int32_t z0 = vp[0].vz, z1 = vp[0].vz;
    int i;
    for (i = 1; i < n; i++) {
        if (vp[i].vx < x0) x0 = vp[i].vx;
        if (vp[i].vx > x1) x1 = vp[i].vx;
        if (vp[i].vz < z0) z0 = vp[i].vz;
        if (vp[i].vz > z1) z1 = vp[i].vz;
    }
    int32_t off = cd_leaf_off(leaf);
    *min_x = x0 + off; *max_x = x1 + off;
    *min_z = z0;       *max_z = z1;
    return 1;
}

/* Player push-out. Area-gated to Outside Catacombs so the shared reception
   collision routine can call it unconditionally, exactly as every other prop
   family in that routine is.

   >>> THE PUSH IS SOUTH AND ONLY SOUTH, AND THAT IS A STATEMENT ABOUT WHERE THE
   DOORS ARE — NOT A SHORTCUT. <<< The concrete props' "smallest penetration of
   the four sides" rule is right for a block standing in open floor. These are
   set into the catacomb facade, and three of their four sides are stone:

     NORTH  the untextured backing plane at z=3850, which collision wall 51 runs
            along (x[-750,750]). A north push would shove the player at that
            wall, the wall would push them back south on the same frame — the
            walls run before the props in apply_collision_reception, so the door
            would get the last word — and the two would trade the player back
            and forth forever. This is the deadlock hatch_doors.c hit against
            the pit's own fence, in the one geometry where it is unavoidable.
     EAST   past x=500 is the facade mass either side of the doorway, fenced by
     WEST   walls 1 and 2 running out to (±1364,4150). Not floor.

   South is the doorway, the avenue, and the only direction the player can have
   come from. Seeding the push with it is therefore not a simplification — it is
   the complete set of candidates.

   >>> THE LEAVES DO MOVE NOW, AND IT DID NOT COST THIS RULE A THING. <<< That
   is the second reason they SLIDE rather than swing (catacomb_doors.h has the
   first). A swung leaf stands out into the room with walkable floor on more
   than one side of it, which is what the note that used to live here was
   warning about: going ROUND it becomes a real move, the way it is for The
   Hatch's pair, and the south-only push stops being the complete set.

   A slid one never leaves the facade. At full slide the left leaf occupies
   x[-1000,-500] and the right x[500,1000], both of them flat against stone that
   walls 1 and 2 already fence the player out of — so the box below is sitting
   inside a volume that is unreachable anyway and every side of it that is not
   south is still stone. The push is unchanged and it is still complete. */
void catacomb_doors_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    (void)py;   /* single flat floor, full-height leaves — see the note above */
    if (current_area != STATE_OUTSIDE_CATACOMBS) return;

    int leaf;
    for (leaf = 0; leaf < CD_LEAVES; leaf++) {
        int32_t min_x, max_x, min_z, max_z;
        if (!cd_leaf_box(leaf, &min_x, &max_x, &min_z, &max_z)) continue;

        min_x -= radius; max_x += radius;
        min_z -= radius; max_z += radius;

        if (*px <= min_x || *px >= max_x) continue;
        if (*pz <= min_z || *pz >= max_z) continue;

        *pz = min_z;    /* out the south face, the only walkable side */
    }
}

/* ---- Drawing ---------------------------------------------------------------
   The room mesh's own prim loop with two changes:

     THE WORLD MATRIX IS THE SLIDE AND NOTHING ELSE. The leaves are already in
     room coordinates, so with the pair shut there is genuinely nothing to
     compose and the plain view matrix projects them where they belong — which
     is what this file used to say in place of this paragraph. Sliding them
     needs one translation along X, and it is applied through the GTE rather
     than by rewriting 26 vertices a leaf: the vertex block is the .smd's own
     and cd_leaf_box() reads it too, so mutating it would make the collision's
     offset double up on the draw's.

     THE TEXTURE SLOT IS THE ROOM'S, not the one smxlink baked in — slot 5,
     `lamashtu tablet`, on the brick_wall page. Their UVs run past 128, so they
     depend on the 128 texture window outside_catacombs_draw sets for the whole
     frame. */
static void cd_draw_leaf(RenderContext *ctx, int leaf, MATRIX *view) {
    SMD *smd = cd_smd[leaf];
    if (!smd) return;

    /* Identity rotation, this leaf's slide for the translation. Composed even
       at slide 0, where it is the view matrix again exactly — one branch fewer
       than testing for it, and the shut pose is not the hot case anyway: this
       runs twice a frame in one room. */
    MATRIX world_m;
    world_m.m[0][0] = ONE; world_m.m[0][1] = 0;   world_m.m[0][2] = 0;
    world_m.m[1][0] = 0;   world_m.m[1][1] = ONE; world_m.m[1][2] = 0;
    world_m.m[2][0] = 0;   world_m.m[2][1] = 0;   world_m.m[2][2] = ONE;

    VECTOR pos = { cd_leaf_off(leaf), 0, 0 };
    TransMatrix(&world_m, &pos);

    MATRIX combined;
    CompMatrixLV(view, &world_m, &combined);
    gte_SetRotMatrix(&combined);
    gte_SetTransMatrix(&combined);

    int32_t min_x, max_x, min_z, max_z;
    if (!cd_leaf_box(leaf, &min_x, &max_x, &min_z, &max_z)) return;

    /* ONE distance for the leaf, taken at its centre and used for the fog on
       every poly of it. The room mesh pays per-poly because its polys are
       thousands of units apart; a leaf is 500 x 60, a fraction of a fog step
       over this room's 575/2500. Doubles as the distance cull: the facade is
       6600 units from the gate and the room deliberately fogs it out long
       before then (see the view-distance note in outside_catacombs.c), so the
       leaves must disappear with it rather than hanging in the purple. */
    int32_t cx = (min_x + max_x) / 2;
    int32_t cz = (min_z + max_z) / 2;
    int32_t dx = cx - cam_x;
    int32_t dz = cz - cam_z;
    int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (dist > g_fog_far + 400) return;      /* 400 = the leaf's own reach */
    int32_t fog_factor = render_fog_scale(dist);

    SVECTOR *vp = cd_verts(leaf);
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

        /* Backface cull. >>> check_model_winding SAYS THESE ARE INSIDE-OUT AND
           IT IS WRONG. <<< Its verdict is a signed-volume test and these leaves
           are OPEN shells — no rear face at z=3847, 14 open edges — so the
           volume it integrates is meaningless. Checked by hand against the room
           mesh instead, which renders correctly and is the only authority
           available: in this exporter's convention a visible face's winding
           normal points AWAY from the viewer (the room's ground gives (0,+1,0),
           its west perimeter (-1,0,0), its north wall (0,0,+1)). Every face of
           both leaves agrees — the doorway plane at z=3787 gives (0,0,+1) for a
           player standing to its south, the flanks point outward, the top face
           points down. They are wound correctly and culling them is safe. */
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
        /* THE ROOM MESH'S SORTING RULE, and it has to be the same one: the
           leaves' own top faces are horizontal slabs sitting over the room's
           horizontal ground, and the room sorts horizontal polys by their
           FARTHEST corner rather than by their average (see render.h). The +40
           bias is the room's too. */
        if (poly_is_flat_y(v0, v1, v2, v3))
            otz = is_quad ? otz_far4(sz[1], sz[2], v2_sz, sz[3])
                          : otz_far3(sz[1], sz[2], sz[3]);
        if (otz <= 0) { p += stride; continue; }
        otz += 40 - CD_OT_BIAS;
        if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        uint8_t *col = p + 16;
        /* Purple fog, the same night sky the rest of the garden looks out on. */
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8);

        uint8_t *uv = p + 20;   /* UVs sit at +20 for FT3 and FT4 alike */

        if (is_quad && pt->texture) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, r, g, b);
            poly->tpage = TIM_TPAGE_LMSHTBLT;
            poly->clut  = TIM_CLUT_LMSHTBLT;
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
            poly->tpage = TIM_TPAGE_LMSHTBLT;
            poly->clut  = TIM_CLUT_LMSHTBLT;
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

/* Called from outside_catacombs_draw with the room's 128 texture window already
   in the OT.

   >>> IT NOW LEAVES THE GTE'S MATRIX CHANGED AND PUTS IT BACK ITSELF. <<< This
   used to compose nothing and so needed no restore, and the entity draws and
   the gate sign after it relied on that. Each leaf carries its own slide now,
   so the plain view matrix is rebuilt here and reinstated on the way out —
   hatch_doors_draw()'s shape exactly, for the same reason and with the same
   tail. Building the view rather than borrowing the caller's also means this is
   correct on its own terms rather than by agreement with the call site. */
void catacomb_doors_draw(RenderContext *ctx) {
    MATRIX view;
    camera_build_view(&view);

    cd_draw_leaf(ctx, CD_LEFT,  &view);
    cd_draw_leaf(ctx, CD_RIGHT, &view);

    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
