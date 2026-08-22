#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "room_arena.h"
#include "tim_slots.h"
#include "camera.h"
#include "rear_gate.h"
#include "collision.h"
#include "rear_gate_mesh_collision.h"
#include "rear_gate_tex_map.h"
#include "btn_glyph.h"
#include "door.h"
#include "texmgr.h"
#include "dresser.h"
#include "garden_courtyard.h"    /* garden_courtyard_upload_textures      */
#include "fountain_square.h"     /* fountain_square_upload_drain          */
#include "save_point.h"
#include "zombie.h"
#include "spider.h"
#include "rafflesia.h"
#include "mushroom.h"
#include "living_statue.h"
#include "web.h"
#include "item_pickup.h"
#include "sml_med.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* Rear Gate: the walled rear lawn west of Fountain Square. See rear_gate.h for
   the layout. */

static SMD  *rear_gate_smd  = NULL;
static void *rear_gate_buff = NULL;

/* ---- View distance ---------------------------------------------------------
   MAZE ONE'S EXACTLY, which is Fountain Square's exactly: 575/2500, the same
   purple SKY_FOG_* colour, and the cull equal to the fog-far so nothing is
   dropped until the fog has already faded it into the background. The whole
   garden from the courtyard's gate onwards is one continuous outdoors, and the
   step through a gate changes the plan of the place but not the weather. The
   user asked for these distances explicitly.

   It suits the room on its own account too. The footprint is 4400 x 7306, so
   from the lawn the perimeter is deep in the fog while the plinth and the mouth
   of the southern path resolve as you walk; and standing at the bottom of the
   ramp the double door 1100 units up it is still legible, which is the point of
   putting a door there at all. */
#define RG_CULL_DIST      2500
#define RG_FOG_NEAR        575
#define RG_FOG_FAR        2500

/* ---- Floor zones -----------------------------------------------------------
   TWO zones, and this is the first garden room to need more than one.

   The collision generator reported thirteen floor planes and listed twelve of
   them at y=0. The thirteenth, "FLOOR 9: y=-250 x(-300 to 300) z(-3200 to
   -2100)", IS NOT A PLANE — it is the generator averaging a RAMP, and taking it
   at face value would put a 250-tall invisible step at z=-2100 and leave the
   double door's sill 250 units above a floor that stops dead. The visual mesh
   has the real shape: six gravel bands climbing from y=0 at z=-2100 to y=-500 at
   z=-3200, and they are exactly linear (y=-83 at z=-2283, -167 at -2467, -250 at
   -2650, -333 at -2833, -417 at -3017), so a single-axis FLOOR_RAMP anchored at
   the two ends is an EXACT fit here rather than the fit-with-error the delivery
   area's ramp settles for.

   ORDER IS LOAD-BEARING. apply_height walks the list and takes the first zone
   whose XZ box contains the player, so the ramp must come before the flat
   catch-all that also covers its footprint. Put the other way round the player
   would walk up the gravel at y=0 and pass through it.

   The flat zone is then one rect over the whole collision bounds, as in Maze One
   and Fountain Square. It covers more than the walkable area — the insides of
   the hedge blocks and the plinth's footprint fall inside it — but the player is
   kept out of those by the wall runs, and giving them zones of their own would
   only help someone who had already clipped a corner. */
static void rear_gate_floor_zones_init(void) {
    floor_zones[0].type            = FLOOR_RAMP;
    floor_zones[0].min_x           =  -300; floor_zones[0].max_x =   300;
    floor_zones[0].min_z           = -3200; floor_zones[0].max_z = -2100;
    floor_zones[0].ramp_y_start    =     0;   /* Y at z=-2100 (north, ground) */
    floor_zones[0].ramp_y_end      =  -500;   /* Y at z=-3200 (south, at the door) */
    floor_zones[0].ramp_axis_start = -2100;
    floor_zones[0].ramp_axis_end   = -3200;
    floor_zones[0].ramp_along_x    =     0;   /* the slope runs along Z */

    floor_zones[1].type  = FLOOR_FLAT;
    floor_zones[1].min_x = -2200; floor_zones[1].max_x = 2200;
    floor_zones[1].min_z = -3200; floor_zones[1].max_z = 4106;
    floor_zones[1].y     = 0;

    floor_zone_count = 2;
}

/* ---- Per-room textures -----------------------------------------------------
   NINE mesh textures, the most of any room in the game, and the room owns
   exactly TWO of them. The other seven are already put in VRAM by rooms this one
   is reached through, so for those it owns no texture RAM at all: it takes the
   tpage/clut headers as compile-time constants and delegates the entry-time
   LoadImage to whichever module holds the RAM copy. Registering a second copy of
   each would have been the obvious thing and is the wrong thing — texmgr has a
   hard TEXMGR_MAX and a registration past it fails SILENTLY, breaking that
   texture everywhere (see tools/TEXTURING_NOTES.txt).

     0 hedge      the perimeter and the path runs   (clsd_drwr page, x384 y0)
     1 grdn_gte   the three gates                   (kchn_wl page,   x512 y0)
     2 grss_gs    the lawn                          (stn_stl page,   x320 y0)
     3 gravel_gs  the path, the courts and the ramp (rusty_fence page, x704 y0)
     4 brick_wall the 1500-tall wall at the south end (x768 y0)
     5 drain      the channel across the lawn       (opn_drwr page,  x832 y0)
     6 trees_dl   the trees behind the north gate   (x960 y256)
     7 plinth_rg  the block in the middle of the lawn (trck_clue page, x640 y0) OWNED
     8 dbl_dr_rg  the door at the top of the ramp   (frnt_dr page, x320 y256) OWNED

   Slots 0-4 come from the Garden Courtyard's uploader (which itself runs
   garden_stairs_upload_textures first, and it is that call which puts brick_wall
   up). As in every garden room since the Garden Stairs, the mesh's 'grss' and
   'gravel_texture' materials resolve to the grss_gs and gravel_gs CLONES rather
   than to their own pages, so the whole garden chain draws from one set of
   slots — and here it is forced rather than merely tidy, since grss.tim's own
   home is x768 (this room's brick wall) and gravel_texture.tim's is x640 (this
   room's plinth). See gen_rear_gate_tex_map.py.

   Slot 5 is taken through a NARROW accessor on the module that owns it —
   fountain_square_upload_drain() — rather than through that room's full
   uploader, which would also stamp the fountain on x768 y0 straight over the
   brick wall.

   >>> SLOT 6 COSTS ABSOLUTELY NOTHING. <<< 'trees' resolves to the delivery
   area's TREES_DL clone, which owns its VRAM page outright: delivery_area_init
   uploads it once at startup and nothing in the game ever overwrites it, so
   there is no registration, no entry-time LoadImage and no restore obligation.
   trees.tim's own page is x640, which this room fills with the plinth, so the
   clone was needed anyway — it just happened to already exist.

   Slots 7 and 8 are the two this room owns, and BOTH ARE RETARGETS, not new art
   (tools/retarget_tim.py). This is the first room to draw drain, plinth AND
   double_door at once, and all three of those live on the opn_drwr page at
   x832 y0. drain keeps that page, being the one of the three another garden room
   already uploads, so the other two moved:
     plinth_rg -> x640 y0, the trck_clue/poison_flower page. An 8bpp FULL page,
                  which plinth needs and which this room draws nothing from — it
                  has no flower beds and no chnlnk, and its gravel and trees both
                  resolve elsewhere. Already time-shared five ways.
     dbl_dr_rg -> x320 y256, the frnt_dr/red_crpt page. double_door is 4bpp, so
                  it occupies only 32 VRAM columns and stays inside that page's
                  left half, clear of graveolver at x352. frnt_dr and red_crpt are
                  already an intentional streaming pair and both rooms re-upload
                  on entry, so a third sharer adds no restore obligation.
   Neither page was free — there are no free page-aligned Voff-0 slots left at
   all — but neither addition obliges anybody new to restore anything.

   All nine sit at Voff 0, so the one 128 texture window set in rear_gate_draw
   serves them all. */
#define REAR_GATE_TEX_COUNT 9

static uint16_t tex_tpage[REAR_GATE_TEX_COUNT];
static uint16_t tex_clut[REAR_GATE_TEX_COUNT];

/* The two textures this room OWNS: RAM-resident from startup so the entry-time
   upload is a pure LoadImage with no CD read. Keep this table in step with the
   slot numbering above and with NAME_TO_SLOT in gen_rear_gate_tex_map.py. */
#define REAR_GATE_NEW_TEX 2
static int new_tex_id[REAR_GATE_NEW_TEX];
static const char *new_tex_file[REAR_GATE_NEW_TEX] = {
    "\\TEX\\PLNTHRG.TIM;1",   /* slot 7 */
    "\\TEX\\DBLDRRG.TIM;1",   /* slot 8 */
};

/* ---- Cull keys -------------------------------------------------------------
   Maze One's scheme, and for the same reason: in a room this size the reject
   path, not the draw path, is what the frame is spent on. The footprint is
   4400 x 7306 against a 2500 Manhattan cull, so most of the 1198 primitives are
   out of range from anywhere the player can stand, and each rejected one would
   otherwise cost a read of the primitive header for its stride, a read of its
   first vertex INDEX, and a chase into the vertex array — three main-memory
   stalls with no data cache behind them.

   So the cull key is lifted out into its own array, built once at load: the
   first vertex's X and Z, plus the primitive's stride so the walk can advance
   without reading the header at all. A rejected primitive costs ONE sequential
   6-byte read and never touches the mesh. Identical output — this changes what
   the reject path READS, not what it decides.

   6 bytes x 1198 = 7 KB of BSS, alongside the 1.2 KB rear_gate_nocull table that
   is already indexed the same way. Indices match the draw loop's `i`. */
typedef struct { int16_t x, z; uint8_t stride, pad; } RgCullKey;
static RgCullKey rg_keys[REAR_GATE_PRIM_COUNT];
static int       rg_key_count = 0;

static void rg_build_cull_keys(void) {
    rg_key_count = 0;
    if (!rear_gate_smd) return;
    uint8_t *p = (uint8_t *)rear_gate_smd->p_prims;
    int i, n = rear_gate_smd->n_prims;
    if (n > REAR_GATE_PRIM_COUNT) n = REAR_GATE_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &rear_gate_smd->p_verts[vi[0]];
        rg_keys[i].x      = v0->vx;
        rg_keys[i].z      = v0->vz;
        rg_keys[i].stride = pt->len;
        rg_keys[i].pad    = 0;
        p += pt->len;
    }
    rg_key_count = n;
}

/* Load this room's geometry into the shared arena. Called on ENTRY, from main's
   STATE_LOADING branch — NOT at startup. The arena holds exactly one room, so
   this overwrites whatever the player just walked out of; that is safe because
   collision and floor heights come from compile-time tables, not from the mesh.
   See src/room_arena.h for the whole rationale. At 68 KB this mesh is well
   inside the arena Maze One's 117 KB sizes, so nothing there had to change. */
void rear_gate_load_geometry(void) {
    rear_gate_buff = room_arena_load("\\TEX\\REARGATE.SMD;1");
    rear_gate_smd  = rear_gate_buff ? smdInitData(rear_gate_buff) : NULL;
    rg_build_cull_keys();
}

/* Read at STARTUP — the only safe time for CD access — the two textures this
   room owns. Geometry moved to rear_gate_load_geometry above, and the other
   seven slots are compile-time constants that cost nothing here. */
void rear_gate_load_assets(void) {
    /* Every one of these is uploaded by another module; header only — no
       LoadImage, no second RAM copy. TREESDL is the extreme case: its pixels go
       up once at startup in delivery_area_init and are never touched again, so
       this line is the entire cost of drawing the trees. */
    TIM_SLOT(0, HEDGE);
    TIM_SLOT(1, GRDNGTE);
    TIM_SLOT(2, GRSSGS);
    TIM_SLOT(3, GRAVELGS);
    TIM_SLOT(4, BRIKWLL);
    TIM_SLOT(5, DRAIN);
    TIM_SLOT(6, TREESDL);

    /* This room's own pair: RAM-resident copies for a CD-free re-upload. */
    for (int i = 0; i < REAR_GATE_NEW_TEX; i++)
        new_tex_id[i] = texmgr_register(new_tex_file[i]);
    TIM_SLOT(7, PLNTHRG);
    TIM_SLOT(8, DBLDRRG);
}

/* Upload the streamed textures. Pure LoadImage — no CD access — safe during the
   room transition (the caller DrawSyncs first, as main's STATE_LOADING does).

   ORDER MATTERS. The courtyard's uploader (via the Garden Stairs') puts chnlnk
   on x640 y0 and xt_dr_cg on x832 y0 — which are where the plinth and the drain
   live — so both of those must go up AFTER it, never before. The delegated call
   is the NARROW one by design: fountain_square_upload_textures would also drop
   the fountain on x768 y0, straight over this room's brick wall.

   dbl_dr_rg's page (x320 y256) is touched by nothing in this chain, so its order
   is free; it is uploaded with its partner to keep the pair together. */
void rear_gate_upload_textures(void) {
    garden_courtyard_upload_textures();   /* hedge, grdn_gte, grss_gs, gravel_gs,
                                             and brick_wall via the stairs' */
    fountain_square_upload_drain();       /* drain -> the xt_dr_cg slot        */
    for (int i = 0; i < REAR_GATE_NEW_TEX; i++)
        texmgr_upload(new_tex_id[i]);     /* plinth -> chnlnk, door -> frnt_dr */
}

/* ---- The east-wall gate back to Fountain Square -----------------------------
   The grdn_gte polys on this side span z[1500,2300] at x=2200, y[-500,0]. It is
   the same gate leaf as the one in Fountain Square's west hedge, which is 72
   units narrower there — the two rooms' meshes are independent, and only this
   pairing links them. The gate's own alcove is collision FLOOR 6,
   x(2000,2200) z(1500,2300), which is what fixes the centre at z=1900.

   Collision wall 17 runs across the opening at x=2200 with nx = -4096, so the
   walkable side is -X and the player approaches from inside this room. For a
   sign in the YZ plane that is mirror=1, and it stands 11 units WEST of the wall
   (x - 11). Both are the opposite hand from Fountain Square's side of the same
   gate, whose wall faces +X — getting either backwards comes out as mirrored
   text or a sign buried in the hedge.

   The wall STAYS in the collision list: the leaf is shut as far as collision is
   concerned and it is the trigger, not a hole, that lets the player through. */
#define RG_GATE_X             2200
#define RG_GATE_Z             1900    /* (1500 + 2300) / 2, and FLOOR 6's centre */
#define RG_TEXT_Y            (-186)   /* eye level on the y=0 lawn               */
#define RG_TEXT_RADIUS        1500
#define RG_FADE_NEAR          1000
#define RG_TRIGGER_RADIUS      500

/* Standing eye on the lawn: floor y=0, less GROUND_FLOOR_Y and the 40-unit floor
   standoff apply_height applies. The same expression Fountain Square, the
   Outside Catacombs and both mazes use, and for the same reason — this whole run
   of garden rooms has its floor at y=0 while the courtyard below is at positive
   y. It is the LAWN's height, not the ramp's: the only spawn in this room is at
   the east gate, and nothing arrives at the top of the ramp. */
#define RG_EYE_Y  (0 - GROUND_FLOOR_Y - 40)

/* Circle edge-detect, seeded by rear_gate_gate_arm(). Starts "held" so a press
   carried in through the transition doesn't bounce the player straight back.
   ONE gate is connected here, so unlike Fountain Square and Maze One there is
   only one edge state to keep. */
static int gate_circle_prev = 1;

static int circle_held(void) {
    return interact_tapped();
}

void rear_gate_gate_arm(void) {
    gate_circle_prev = circle_held();
}

int rear_gate_gate_triggered(void) {
    int held = circle_held();
    int just = held && !gate_circle_prev;
    gate_circle_prev = held;
    if (!just) return 0;

    int32_t dx = cam_x - RG_GATE_X;
    int32_t dz = cam_z - RG_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    return xz < RG_TRIGGER_RADIUS && interact_facing(RG_GATE_X, RG_GATE_Z);
}

/* Floating "Press O to enter" sign on the east gate. YZ plane:
   door_draw_string_3d centres the reading axis (Z) on world_z after adding 200,
   so pass door_z - 200. Sits just west (x-11) of the wall so it floats in front
   of the gate on the side the player is actually on. */
static void gate_text(RenderContext *ctx) {
    int32_t dx = cam_x - RG_GATE_X;
    int32_t dz = cam_z - RG_GATE_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= RG_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > RG_FADE_NEAR) {
        int range = RG_TEXT_RADIUS - RG_FADE_NEAR;
        int prog  = xz - RG_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to enter",
                        RG_GATE_X - 11, RG_TEXT_Y, RG_GATE_Z - 200,
                        50, 255, 50, fade, 1, TEXT_PLANE_YZ, DOOR_PIXEL_SIZE);
}

/* Arriving from Fountain Square: stand WEST of the x=2200 hedge, clear of the
   wall push radius (so the player isn't shoved on their first frame), facing -X
   — the direction of travel through the gate, looking out across the lawn at the
   plinth. x lands at 1980, twenty units short of the alcove mouth at x=2000 and
   so out on FLOOR 4's open lawn rather than in the alcove; that is deliberate,
   and it is also past the ends of both alcove side walls (10 and 20 run only
   z[2300,2500] and z[1299,1500]), so nothing pushes the player anywhere on the
   frame they arrive. */
void rear_gate_spawn_east(void) {
    cam_x   = RG_GATE_X - COLLISION_WALL_RADIUS - 25;
    cam_y   = RG_EYE_Y;
    cam_vy  = 0;
    cam_z   = RG_GATE_Z;
    cam_rot = 3072;   /* facing -X, into the room */
    rear_gate_gate_arm();
}

void rear_gate_init(void) {
    rear_gate_collision_init(&current_collision_room);
    /* The perimeter hedge is DRAWN to y=-500 (the gates and the trees reach
       -600, but they are the openings and the skyline, not the roofline); the
       collision runs are only 500 tall in the wall list, which is the proxy, not
       what the player sees. State the DRAWN value so anything ceiling-mounted
       hangs at the height the room actually has — see tools/ADDING_A_ROOM.txt on
       visual-vs-collision heights.

       NOTE THAT THIS IS THE HEDGE, NOT THE TALLEST THING IN THE ROOM. The brick
       wall across the south end is DRAWN to y=-1500 and collides to -1237.
       Nothing hangs from it today, but anything ever placed against it wants the
       drawn -1500, not this and not the collision proxy either. */
    collision_set_ceiling_y(-500);
    /* No collision_set_wall_radius: the default 195 is right here. The tightest
       space is the southern path and the two courtyards off it, all 600 wide,
       which at 195 leaves 210 units of walkable lane — the same margin both
       mazes run on. main.c resets to the default before every room init, so
       saying nothing is enough. */

    rear_gate_floor_zones_init();

    /* The only spawn: the east gate, back into Fountain Square. There is nowhere
       else to arrive from, so main.c needs no override. */
    rear_gate_spawn_east();

    /* Save points and dresser props are global (not room-swapped) and neither is
       area-gated in its collide routine, so reception's instances would block
       the player invisibly if they fell inside this room's bounds — and this
       room spans x[-2200,2200] z[-3200,4106], so they certainly do. Clearing is
       safe: reception_init() re-places both on every reception entry. No save
       point of this room's own; the nearest is on the Garden Stairs' top
       landing. */
    save_points_clear();
    dressers_clear();
}

static void draw_rear_gate_smd(RenderContext *ctx) {
    if (!rear_gate_smd) return;

    uint8_t *p = (uint8_t *)rear_gate_smd->p_prims;
    int i, n = rear_gate_smd->n_prims;

    /* Hoisted: all constant for the whole frame, and every one of them would
       otherwise be recomputed per primitive inside the hottest loop in the room —
       the two trig lookups once for each poly that passed the distance cull, the
       cull distance and the debug test all 1198 times. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = RG_CULL_DIST;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    if (rg_key_count < n) n = rg_key_count;   /* keys not built: draw nothing */

    for (i = 0; i < n; i++) {
        /* ---- The reject path. ONE sequential read, no mesh access ----------
           rg_keys carries this primitive's first vertex and its stride, so a
           primitive outside the view distance is skipped without touching the
           header or the vertex array at all. See rg_build_cull_keys. */
        uint8_t stride = rg_keys[i].stride;
        int32_t kdx = (int32_t)rg_keys[i].x - cam_x;
        int32_t kdz = (int32_t)rg_keys[i].z - cam_z;
        if ((kdx < 0 ? -kdx : kdx) + (kdz < 0 ? -kdz : kdz) > cull)
            { p += stride; continue; }

        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &rear_gate_smd->p_verts[vi[0]];
        SVECTOR *v1 = &rear_gate_smd->p_verts[vi[1]];
        SVECTOR *v2 = &rear_gate_smd->p_verts[vi[2]];

        {
            /* ---- SIDE-PLANE FRUSTUM CULL -------------------------------------
               Maze One's test verbatim; read the long note there for the whole
               argument. In short: the behind-the-camera test below rejects
               almost nothing on its own, because within the cull radius the
               geometry is all around you rather than behind you, and everything
               else was being handed to the GTE, transformed, and then — if it
               projected inside the GPU's +/-1023 coordinate limit, which is over
               three screens wide — queued as a primitive the GPU had to take in
               and throw away.

               The test is the two side planes of the frustum. With
               gte_SetGeomScreen(256) on a 320-wide screen the half-field is
               160/256, so a point is outside the right plane when
                   side > (5/8) * fwd,  i.e.  8*side > 5*fwd
               and outside the left when the same holds for -side. Both sides of
               that comparison are world units (the >>12 undoes isin/icos), and
               at this room's scale the products stay far inside an int32.

               >>> A POLY IS ONLY CULLED WHEN EVERY VERTEX IS OUTSIDE THE SAME
               PLANE. <<< That makes the test exact rather than conservative —
               it can never remove a poly with a vertex inside the true frustum,
               whatever the mesh — and it is why the loop is written this way
               rather than around a bounding sphere on v0, which measured barely
               half as effective on Maze One's mesh.

               Y is not tested, and that is still right here even though this
               room, unlike the mazes, has a ramp: adding top and bottom planes
               would cost multiplies to reject nothing, because the camera never
               leaves standing height over a surface that spans only 500 units.
               ------------------------------------------------------------- */
            int32_t fwd = kdx * sn + kdz * cs;
            if (fwd < -(700 << 12))
                { p += stride; continue; }
            if (!no_frustum) {
                int32_t f0 = fwd >> 12;                      /* world units */
                int32_t s0 = (kdx * cs - kdz * sn) >> 12;
                int     sign = 0;
                if      ( s0 * 8 > f0 * 5) sign =  1;        /* v0 off right */
                else if (-s0 * 8 > f0 * 5) sign = -1;        /* v0 off left  */
                if (sign) {
                    int nv = is_quad ? 4 : 3, k, all_out = 1;
                    for (k = 1; k < nv; k++) {
                        SVECTOR *vk = &rear_gate_smd->p_verts[vi[k]];
                        int32_t ex = (int32_t)vk->vx - cam_x;
                        int32_t ez = (int32_t)vk->vz - cam_z;
                        int32_t f  = (ex * sn + ez * cs) >> 12;
                        int32_t sd = (ex * cs - ez * sn) >> 12;
                        if (!(sign * sd * 8 > f * 5)) { all_out = 0; break; }
                    }
                    if (all_out) { p += stride; continue; }
                }
            }
        }

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

        /* Backface cull, except degenerate (triangle-shaped) quads flagged at
           build time in rear_gate_nocull — same scheme as the other rooms. This
           mesh has none, but the table is generated and checked all the same. */
        int nocull = (i < REAR_GATE_PRIM_COUNT) && rear_gate_nocull[i];
        if (!pt->nocull && !nocull) {
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }

        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        SVECTOR *v3    = 0;
        int32_t  v2_sz = sz[3];   /* v2's SZ, before the quad path reuses sz[3] */
        if (is_quad) {
            v3 = &rear_gate_smd->p_verts[vi[3]];
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
        /* Horizontal polys sort by their farthest corner, not their average,
           so floors stay behind whatever stands on them (see render.h). */
        if (poly_is_flat_y(v0, v1, v2, v3))
            otz = is_quad ? otz_far4(sz[1], sz[2], v2_sz, sz[3])
                          : otz_far3(sz[1], sz[2], sz[3]);
        if (otz <= 0) { p += stride; continue; }
        otz += 40;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog = dist < RG_FOG_NEAR ? RG_FOG_NEAR : (dist > RG_FOG_FAR ? RG_FOG_FAR : dist);
        int32_t fog_factor = ((RG_FOG_FAR - fog) << 8) / (RG_FOG_FAR - RG_FOG_NEAR);

        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

        /* Per-prim texture index (SMD prim order matches the tex map). UVs come
           straight from the SMD primitive (offset 20+) and wrap via the 128
           texture window set in rear_gate_draw. */
        uint8_t tex_idx = (i < REAR_GATE_PRIM_COUNT) ? rear_gate_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < REAR_GATE_TEX_COUNT);
        /* Purple fog, the same night sky the rest of the garden looks out on,
           and at Maze One's exact near/far — this is one continuous outdoors and
           the gate between them is not a change in the weather. */
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8);

        if (is_quad && textured) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            uint8_t *uv = p + 20;
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, r, g, b);
            poly->tpage = tex_tpage[tex_idx];
            poly->clut  = tex_clut[tex_idx];
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
            poly->tpage = tex_tpage[tex_idx];
            poly->clut  = tex_clut[tex_idx];
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
}

void rear_gate_draw(RenderContext *ctx) {
    /* Entities in this room fog with the same near/far as the mesh below. */
    g_fog_near = RG_FOG_NEAR; g_fog_far = RG_FOG_FAR;

    /* Background in the SAME colour the fog saturates to, so a poly that has
       faded out is indistinguishable from the void behind it and the cull never
       shows a seam. Purple here, matching the rest of the garden.

       Painted by the HARDWARE CLEAR, not by a full-screen TILE — the draw
       environments already clear the framebuffer (isbg=1) before anything is
       drawn, so a tile on top of that is a second full-screen fill of the same
       77k pixels every frame. See the note on render_set_clear_colour, and Maze
       One, which is the room that trialled it. */
    render_set_clear_colour(ctx, SKY_FOG_R, SKY_FOG_G, SKY_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap (tile) within each texture's
       page. All NINE of this room's textures sit at page-top (Voff 0) — the two
       retargets were placed with that as a hard requirement — so one window
       serves them (see tools/VRAM_MAP.txt). */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
        setTexWindow(twin, &tw);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
        ctx->next_packet += sizeof(DR_TWIN);
    }

    /* View matrix via camera_build_view rather than the hand-rolled yaw-only
       block the older rooms use — same matrix at pitch 0, and it does not drop
       cam_pitch on the floor should anything here ever tilt the camera. */
    MATRIX rot_matrix;
    camera_build_view(&rot_matrix);
    gte_SetRotMatrix(&rot_matrix);
    gte_SetTransMatrix(&rot_matrix);

    /* --- Isolation switches (debug levels 4-7; see camera.c). Normal play and
       every other debug level take exp == 0 and none of this applies. --- */
    int exp = DEBUG_EXPERIMENT();

    /* The ladder moves the FOG with the cull, so each rung shows what the room
       would actually look like at that view distance — geometry fading out into
       the background colour rather than popping off at the cull line. */
    if (DEBUG_CULL_DIST()) g_fog_far = DEBUG_CULL_DIST();

    if (exp != DBG_EXP_NO_MESH) draw_rear_gate_smd(ctx);

    /* Every sprite enemy renderer is handed this room's texture window, because
       all of their sprites live at Voff >= 128 and must bracket it rather than
       sample the hedge (see tools/TEXTURING_NOTES.txt PART 5). NOTHING is placed
       here yet — world.c seeds the room empty — so all of these run over empty
       arrays and cost nothing; they are wired up so that a future spawn brackets
       its quad correctly instead of drawing this room's grass on a monster.

       When something is placed, it is sound-legal: this room is on
       SND_BANK_GARDEN (main.c's STATE_LOADING), which is where SFX_HISS and the
       flowers' own loops live, so the rafflesias and mushroom heads are as
       usable here as in Maze One. Check anything else against src/sound.h the
       way Maze One's placement was. */
    {
        RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
        zombies_set_texwindow(&tw);
        spiders_set_texwindow(&tw);
        rafflesias_set_texwindow(&tw);
        mushrooms_set_texwindow(&tw);
        living_statues_set_texwindow(&tw);
    }
    {
        draw_zombies(ctx);
        draw_spiders(ctx);
        draw_rafflesias(ctx);
        draw_mushrooms(ctx);
        draw_living_statues(ctx);
        webs_draw(ctx);
        item_pickups_draw(ctx);
        sml_meds_draw(ctx);
    }

    /* Last: the gate sign. */
    gate_text(ctx);
}
