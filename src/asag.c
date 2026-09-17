#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "collision.h"     /* DEBUG_CULL_DIST */
#include "cdaudio.h"       /* the bracket around the entry-time reads */
#include "asag_arena.h"    /* the ROOM streams the skin; this reads it back */
#include "title.h"         /* STATE_ASAG_ARENA, for the area gate on collide */
#include "asag.h"

/* ASAG — the body. See asag.h for the whole argument; this file is the
   mechanism. Nothing here decides anything about the fight. */

/* ---- The mesh --------------------------------------------------------------
   ONE part where the first Asag had eight, so there is no part table and no
   per-part array: the whole of the loaded state is the handful of statics
   below. See asag.h for why the eight went away.

   >>> THE SKIN IS STREAMED BY THE ROOM, NOT REGISTERED BY THE BOSS. <<<
   tools/ADDING_A_3D_ENEMY.txt STEP 3 says a textured enemy registers through
   texmgr. That is right for the Rabisu, whose room is walked into and out of
   from three directions. It is wrong here: texmgr keeps the whole TIM in main
   RAM for the life of the run, and this boss is reachable only through one
   one-way drop into a room that already streams its own art on that
   transition. So `asag` is row 2 of asag_arena.c's stream_tex_file[] and this
   module reads its tpage/clut back through asag_arena_tex_page()/_clut().
   Zero permanent bytes, and one CD read that was already happening. */
#define ASAG_MESH_FILE  "\\TEXASAG\\ASGBODY.SMD;1"

/* ---- The clips -------------------------------------------------------------
   Indexed by AsagClip. The .pva files are baked by tools/export_asag.py at
   STEP 3; their headers carry the played rate (8) and the loop flag the
   exporter detected, but the loop the GAME uses is the `loop` argument to
   asag_play() — the header flag only records whether the exporter trimmed a
   duplicate end frame, which is a property of the art and not a decision about
   playback. */
static const char *const clip_file[ASAG_CLIP_COUNT] = {
    /* IDLE   */ "\\TEXASAG\\ASGHIDLE.PVA;1",
    /* LASER  */ "\\TEXASAG\\ASGHLAS.PVA;1",
    /* SLAM   */ "\\TEXASAG\\ASGHSLAM.PVA;1",
    /* VOMIT  */ "\\TEXASAG\\ASGHVOM.PVA;1",
    /* FAINT  */ "\\TEXASAG\\ASGHFNT.PVA;1",
};

/* ---- The position track ----------------------------------------------------
   How far each clip travels from HOME toward EMERGED, and when. See asag.h for
   where EMERGED comes from and why the travel is not baked into the clips.

   THE SHAPE IS ALWAYS THE SAME: ramp out, hold, ramp back. Only the two ramp
   lengths differ, and they are in GAME FRAMES because that is what the brief
   was written in (60 = one second).

       out   Home -> Emerged, linear, from the first frame of the clip
       hold  whatever is left in the middle
       back  Emerged -> Home, linear, ending exactly on the clip's last frame

   >>> IDLE IS out = -1, WHICH MEANS "DO NOT TOUCH THE POSITION". <<< It is not
   zero and it is not a ramp: the idle inherits whatever offset was in effect
   when it started and holds it. In practice that is always Home, because every
   other clip ends its back-ramp at Home — but a director that wants Asag to sit
   breathing while emerged can start an idle mid-lunge and get exactly that, for
   free and with no extra state. */
typedef struct {
    int16_t out_ticks;    /* <0 = hold the inherited offset (idle)            */
    int16_t back_ticks;
} AsagMove;

static const AsagMove clip_move[ASAG_CLIP_COUNT] = {
    /* IDLE  */ { -1, -1 },   /* inherits; see above                          */
    /* LASER */ { 60, 60 },   /* out over 1.0s, back over 1.0s                */
    /* SLAM  */ { 30, 60 },   /* out over 0.5s, back over 1.0s                */
    /* VOMIT */ { 90, 60 },   /* out over 1.5s, back over 1.0s                */
    /* FAINT */ { 60, 30 },   /* out over 1.0s, back over 0.5s                */
};

#define PVA_HEADER_SIZE  12

/* ---- Loaded state — ALL OF IT FREED BY asags_free_model() ---------------- */
static void   *mesh_buff;
static SMD    *mesh_smd;
static int16_t mesh_cx, mesh_cz;   /* mesh centre, for the whole-body fog */

static void    *clip_buff  [ASAG_CLIP_COUNT];
static int16_t *clip_data  [ASAG_CLIP_COUNT];   /* first vertex of frame 0    */
static int8_t   clip_stride[ASAG_CLIP_COUNT];   /* int16s per vertex: 4 or 3  */
static int      clip_count [ASAG_CLIP_COUNT];   /* 0 = rejected or absent     */

/* Scratch for ONE unpacked frame, n_verts SVECTORs, allocated with the mesh and
   freed with it. Only PVA2 needs it; see body_verts(). */
static SVECTOR *pose_buf;

/* ---- Playback state — survives a free/load, because it is a statement about
   the FIGHT and not about what is in memory. A director that started the emerge
   and then had the model dropped and reloaded should find the emerge still
   running rather than silently back on the bind pose. --------------------- */
static int8_t  cur_clip = ASAG_CLIP_NONE;
static int16_t anim_frame;
static int16_t anim_acc;      /* 60ths; see ASAG_ANIM_FPS in the header */
static int8_t  clip_loop;
static int8_t  clip_held;     /* 1 = one-shot sitting on its last frame */
static int8_t  clip_frozen;   /* 1 = asag_update() does nothing; see the header */
static int8_t  body_vis;

/* ---- THE TWO FIELDS THE DEATH SEQUENCE WRITES -----------------------------
   tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 5 lists four; this body needs only
   these two on top of `frozen`. There is no clip_y because nothing about Asag
   rises through a floor - he comes out of a wall, and the wall's own polygons
   occlude him correctly already.

   >>> shake IS APPLIED TO THE DRAW'S VERTICES, NOT TO A POSITION. <<< The
   runbook's rule is "the draw's translation only, never to the entity's x/z",
   for the reason that a jittered entity jitters its collision box and its
   health bar with it. This body has no x/z to jitter at all - the vertices are
   baked in world space - so the same rule lands as "offset the posed vertices
   as they are loaded into the GTE, and leave pos_dz alone". asag_collide(),
   asag_body_centre() and asag_head_box() all read the UNSHAKEN pose, which is
   what keeps the camera from vibrating along with him.

   fade is 256 solid / 0 gone, and at anything under 256 the body switches to
   the ADDITIVE blend the runbook's TRICK 1 describes: a flat colour scaled
   toward black, which genuinely disappears over any background. A textured
   poly cannot fade, so the burn drops the texture and draws the silhouette in
   one colour - the same move src/rabisu.c makes for the same reason, and why
   RBS_BODY_R/G/B still exists over there. */
static int16_t body_shake;
static int16_t body_fade = 256;

/* >>> THE DAMAGE FLASH, AND IT IS THE RABISU'S EXACTLY. <<< 1 = tint the whole
   model red this frame. src/rabisu.c does the same thing with the same
   reasoning: a one-colour model has no sprite to flash, so the silhouette
   itself is the only thing that can carry "that hit". It is a MODULATION, not a
   replacement - R goes to full and G and B are cut to a quarter of whatever the
   fog left, so the texture is still there underneath and he reads as lit from
   inside rather than as a red cut-out.

   WHO SETS IT: src/asag_fight.c, every frame, from (hit_timer > 0 || dying). It
   is a plain field and not a timer here because the fight already owns that
   timer for the health bar, and two countdowns for one flash is one too many.
   Held for the WHOLE death rather than the two seconds the killing blow buys -
   the Rabisu's note on that is worth reading, and the short version is that
   dropping back to normal colour halfway through a death and re-igniting with
   the burn looks like a bug. */
static int8_t  body_hit;

/* =========================================================================
   THE EXPOSURE GLOW — the yellow spot on the crown of his head, lit BLUE
   =========================================================================
   >>> WHY THERE IS A SECOND GLOW AT ALL. <<< body_hit says "that hit landed".
   Nothing said "he can be hurt RIGHT NOW", and asag_fight.h is explicit that
   asag_exposed() is the whole of the fight's difficulty — the player was
   supposed to read it off which attack he was playing, and the windows inside
   those attacks are invisible. This is the tell.

   BLUE BECAUSE NOTHING ELSE IN THE ARENA IS. The boils are yellow-green, the
   damage flash is red, the laser and the vomit are his; holy blue is the
   PLAYER's colour in this game (the Helluminator's burn tint, helluminator.c),
   so a blue light coming on means "now".

   ---- WHICH POLYGONS, AND WHY IT IS NOT A LIST OF INDICES -------------------
   The spot is a raised pad on the crown of the head — five primitives, a cap
   and the four short walls that hold it proud of the skull — and every one of
   them is UV-mapped into the one bright yellow disc in textures/asag.png at
   u[85,100] v[113,123]. There is nothing else in that corner of the sheet.

   So the mask is built from the UVs at load time rather than written down as
   {34, 75, 76, 77, 78}. A re-export renumbers primitives freely (the vertex
   ORDER is what tools/export_asag.py pins, not the polygon order), and a stale
   index list would light five polygons somewhere else on the body — the kind of
   wrong that looks like a corrupt mesh. The UV box survives anything short of
   repainting the texture, and if the spot is ever moved on the sheet this is
   one rectangle to move with it.

   The box is deliberately looser than the disc: the four walls sample the skin
   AROUND it, so a tight box would light the cap alone and the pad would read as
   a flat sticker rather than a lamp set into him. */
#define ASAG_GLOW_U0   80
#define ASAG_GLOW_U1  108
#define ASAG_GLOW_V0  105
#define ASAG_GLOW_V1  127

/* One bit per primitive. 79 today; three words is 96 and the loop that fills it
   stops at the array's own size, so a bigger mesh loses the glow rather than
   scribbling past the end. */
#define ASAG_GLOW_WORDS  3
static uint32_t glow_mask[ASAG_GLOW_WORDS];

/* Frames the light takes to come up and to die away. The exposure windows open
   and close mid-animation, and a lamp that snapped on between two frames would
   read as a draw error — the same argument asag_fight.c's boils make for their
   own re-light ramp. 12 is a fifth of a second: fast enough to be information
   the player can act on, slow enough to be a light. */
#define ASAG_GLOW_RAMP  12

/* THE PULSE, once the light is up: the level breathes between about 65% and
   100% over ASAG_GLOW_PULSE frames. A steady blue lamp on a body that is
   otherwise moving reads as a texture; a breathing one reads as a state. */
#define ASAG_GLOW_PULSE  40

/* >>> THERE WAS A HALO HERE AND IT IS GONE ON PURPOSE. <<< The light used to
   be two things: the skin modulation below, plus an additive quad laid over
   each pad polygon and grown from its own screen centre (the Helluminator's
   hell_glow_quad shape), so the blue spilled past the pad's edge. Looked at on
   screen, the spill read as a bloom sitting in FRONT of his head rather than as
   a lamp set into it, and it washed the crown out at the range the fight is
   actually fought at. The pad now simply glows blue: modulation only, no
   additive primitive, no extra OT bucket. ASAG_GLOW_SCALE and the halo's own
   R/G/B went with it — the tint constants below are what colours the light
   now, and they are the ones to move if it wants to be a different blue. */

/* What the SKIN of the pad is modulated to at full glow. Modulation is
   texel * colour / 128 and the disc is a saturated yellow, so R and G have to
   be crushed hard or the pad comes out green rather than blue. */
#define ASAG_GLOW_TINT_R  28
#define ASAG_GLOW_TINT_G  44
#define ASAG_GLOW_TINT_B 255

/* Set by src/asag_fight.c every frame from asag_exposed(), exactly the way
   body_hit is driven — the fight decides, this file only draws. `glow_lvl` is
   the ramp's own state in 256ths and `glow_ph` the pulse's phase; both are
   advanced by the DRAW rather than by asag_update(), because asag_update() is
   skipped while `frozen` and a light that stalled with a held pose would be a
   second thing to reason about. Both cleared by asag_reset(). */
static int8_t  body_glow;
static int16_t glow_lvl;
static int16_t glow_ph;

/* The position track's own clock and output. clip_ticks runs in GAME frames
   (60/s), NOT animation frames, because the brief's ramps are in seconds and
   because sliding the body at 60 Hz under a pose that steps at 8 reads as a
   smooth glide rather than a stutter. pos_dz is what the draw adds to every
   vertex's Z, and it SURVIVES a clip change on purpose - that is what lets the
   idle inherit a position. */
static int32_t clip_ticks;
static int32_t pos_dz;

/* ---- THE DEATH'S RAMP HOME, which OVERRIDES the clip's own position track --
   >>> IT EXISTS BECAUSE THE IDLE INHERITS AND THE DEATH CANNOT WAIT. <<<
   clip_move[]'s idle row is {-1,-1}, i.e. "hold whatever offset is in effect",
   which is right for an idle between attacks (they all end their back-ramp at
   Home anyway) and wrong for a death: health can hit 0 in the middle of a slam,
   with the body 969 units out over the arena, and the brief has him "return to
   his starting Idle position and then freeze". Playing an idle there would
   freeze him mid-lunge.

   The alternatives were worse. Waiting for the killing clip to run its own
   back-ramp costs up to six seconds of a boss with no health standing over the
   player; re-baking the idle to travel would make it travel exactly once, from
   exactly one place.

   So this is a second, independent ramp the DIRECTOR asks for, it takes
   priority over update_pos() while it runs, and any asag_play() cancels it —
   a director that starts a clip has said where it wants the body.

   >>> IT IS A RAMP TO AN ARBITRARY OFFSET, NOT ONLY TO HOME, SINCE THE OPENING
   SCENE GAINED ITS LEAN. <<< The brief for that scene asks Asag to come out of
   the wall about halfway while the boils light and before he speaks, and
   update_pos() cannot express that: a clip's position track is an ABSOLUTE
   0 -> EMERGED -> 0 shape read off the clip's own clock, and the idle's row
   inherits rather than travels. So the same override serves both, with
   asag_ramp_home() now one line on top of asag_ramp_dz().

   >>> AND THAT MADE WHERE THE BODY IS WHEN A CLIP STARTS INTO A REAL QUESTION.
   <<< update_pos() writes pos_dz absolutely, so an attack begun with the body
   parked halfway out SNAPS it back to Home on the ramp's first frame. The
   opening therefore ramps him Home again across the handover, and the fight
   starts from 0 exactly as it always did. Anything else that parks him out of
   position owes the same debt. */
static int32_t home_ramp;     /* frames left, 0 = not ramping */
static int32_t home_total;
static int32_t home_from;
static int32_t home_to;       /* where the ramp is going; 0 is Home */

static int model_loaded = 0;

/* ---- read_file, WITH A STACK GUARD ----------------------------------------
   rabisu.c's, plus the check that would have turned this boss's first build
   from a crash into a missing model.

   >>> THE HEAP TOP IS THE STACK, AND malloc DOES NOT KNOW THAT. <<<
   PSn00bSDK's InitHeap hands the heap every byte from _end to 0x801FFFF8, and
   the stack grows DOWN into the same region with nothing between them. So when
   the heap fills, malloc does not fail - it SUCCEEDS and returns memory the
   stack is already using, and the next CdRead DMAs straight through a saved
   return address. tools/HEAP_BUDGET.txt describes the mechanism; the first
   Asag demonstrated it. It asked for 229 KB of meshes and clips against what
   tools/heap_budget.py reported as 371 KB free, and the console died every
   single time inside CdReadSync:

       buffer 0x801D46D0..0x801DE6D0   sp = 0x801DBD40   <- sp is INSIDE it
       "Attempted unaligned JR to 0xfe51ffbd from 0x8008D9A0"

   The report was wrong by 145 KB: main() declares its RenderContext as a LOCAL
   (two 64 KB packet buffers and two 8 KB ordering tables), so the top of RAM is
   permanently stack and was being counted as heap. Real free at the arena door
   is ~171 KB. The script is fixed and the clips are baked to fit.

   THIS CHECK IS THE BACKSTOP FOR THE NEXT TIME. Reading $sp gives the true
   ceiling at this exact call site, which is deeper in the call chain than any
   budget script can model. A refused read leaves the mesh or a clip absent,
   which draws a bind pose or nothing at all - visible, harmless and obvious -
   instead of corrupting the return address of whatever called us.

   The margin is for the frames BELOW us: CdRead, CdReadSync and the interrupt
   handler all push after this point.

   >>> IT WAS 8192, AND "FAR MORE THAN THEY USE AND COSTS NOTHING" WAS HALF
   RIGHT. <<< It was far more than they use. It did not cost nothing: it cost
   the FAINT, for the third time, in September 2026. The ending scenes
   (src/catacomb_open.c, src/hatch_arrival.c) moved _end up by 2,528 bytes, the
   last clip in the read order missed the guard by 2,012, and the fight's long
   exposure became one second of a motionless boss again — af_clip_watchdog()
   firing on a clip reporting zero ticks, exactly the symptom
   tools/ADDING_THE_ASAG_FIGHT.txt PART 6 says to read that way.

   SO THE CONSTANT WAS MEASURED INSTEAD OF ASSUMED. The words below $sp were
   painted with 0xA5A5A5A5 immediately before the CdRead and scanned afterwards
   for the lowest one disturbed, on every one of the six reads this module
   makes:

       below_sp = 96 bytes   (sp 0x801dbd40, lowest touched 0x801dbce0)

   The same 96 on all six. CdRead, CdReadSync and the interrupt handler between
   them touch twenty-four words below this frame. 2048 is a WHOLE SECTOR and
   twenty-one times the measured depth, and it hands 6,144 bytes back to the top
   of the heap — three times what the faint was short by, so the door has real
   headroom again instead of the 500 bytes it had before.

   >>> RE-MEASURE, DO NOT RE-GUESS, IF THIS EVER LOOKS TIGHT. <<< The method is
   a dozen lines and it is written up in tools/ADDING_THE_ASAG_FIGHT.txt PART 6B
   along with the numbers above. Raising this constant back to 8192 "to be safe"
   would put the faint straight back on the floor; lowering it below a sector is
   not worth the bytes. */
#define ASAG_STACK_MARGIN 2048

/* ---- ...AND THE BUFFER IS THE FILE'S SIZE, NOT THE SECTORS' -----------------
   >>> THIS IS WHERE THE FAINT WENT, THE SECOND TIME. <<< A CD read moves whole
   2048-byte sectors, so the obvious buffer is `sectors * 2048` — and the six
   files this module reads waste 6,308 bytes in that rounding, all of it at the
   top of the heap where the ceiling is. Measured on a headless run, the faint
   was the last read and it missed by 936 BYTES:

       ASGHVOM.PVA  buf=801d0190 top=801d5190           ok
       ASGHFNT.PVA  buf=801d5240 top=801da240  sp=801dbd50  REFUSED

   so it fell back to the bind pose, reported no frames, and the fight's faint
   became one second of a motionless boss — the watchdog in asag_fight.c firing
   on a clip that was never there. Exactly the failure PVA2 was introduced to
   fix (see the note below), re-opened by 16 KB of new code moving _end up under
   a heap that had no margin left.

   THE TAIL IS WHAT MAKES AN EXACT BUFFER POSSIBLE. Every sector but the last
   lands entirely inside the file, so it is DMA'd straight into the buffer. The
   last one is read into a single-sector scratch and the used part of it copied
   in, which is the whole trick: the buffer never has to be bigger than what the
   file says. `tail_scratch` is taken once per load and shared by all six reads
   (asags_load_model owns it), so it is one 2048-byte transient that lands in
   the fragmented low heap rather than six roundings that land at the ceiling.

   THE TAIL READ IS SEEKED EXPLICITLY, not left to the drive's own advance.
   Consecutive CdReads do continue where the last one stopped, but a read that
   depends on that is a read that breaks the day something else touches the
   drive between them. CdPosToInt/CdIntToPos cost nothing and say what is meant.

   The allocation is rounded up to a WORD because CdRead's destination is a
   uint32_t* and the copy below writes bytes; malloc's own alignment is bigger
   than that anyway, so this only ever pads by up to three bytes. */
static uint8_t *tail_scratch;    /* one sector, owned by asags_load_model() */

static void *read_file(const char *name) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)name)) return NULL;
    int size  = (int)file.size;
    if (size <= 0) return NULL;
    int whole = size / 2048;           /* sectors landing entirely inside     */
    int tail  = size - whole * 2048;   /* 0..2047 bytes of one more           */
    int alloc = (size + 3) & ~3;

    /* A tail with no scratch to land in would have to overrun the buffer, so
       the read is refused instead — same silent-but-safe contract as the guard
       below, and asags_load_model() takes the scratch first for that reason. */
    if (tail > 0 && !tail_scratch) return NULL;

    void *buf = malloc(alloc);
    if (!buf) return NULL;

    uint32_t sp;
    __asm__ volatile("move %0, $sp" : "=r"(sp));
    if ((uint32_t)buf + (uint32_t)alloc + ASAG_STACK_MARGIN > sp) {
        free(buf);
        return NULL;          /* out of heap: this mesh/clip simply will not exist */
    }

    if (whole > 0) {
        CdControl(CdlSetloc, &file.pos, NULL);
        CdRead(whole, (uint32_t *)buf, CdlModeSpeed);
        CdReadSync(0, NULL);
    }

    if (tail > 0) {
        CdlLOC pos;
        CdIntToPos(CdPosToInt(&file.pos) + whole, &pos);
        CdControl(CdlSetloc, &pos, NULL);
        CdRead(1, (uint32_t *)tail_scratch, CdlModeSpeed);
        CdReadSync(0, NULL);
        memcpy((uint8_t *)buf + whole * 2048, tail_scratch, tail);
    }
    return buf;
}

/* ---- Loading a clip, and the TWO formats -----------------------------------
   PVA1 stores four int16 per vertex (x, y, z and a pad that is ALWAYS ZERO),
   which is exactly an SVECTOR, so the draw can point straight into the file.
   PVA2 drops the pad and stores three. Same coordinates, same frame order, same
   header - a quarter smaller.

   >>> THE PAD COST THIS BOSS A CLIP. <<< Unpacked, Asag's six clips came to
   133,120 bytes sector-rounded on top of a 6,144-byte mesh, and on a real
   console the LAST read - the faint - was refused by read_file()'s stack guard
   below. Nothing crashed and nothing was logged: the clip was simply absent,
   the body fell back to its bind pose for the five seconds the faint should
   have played, and it looked for all the world like an animation with no motion
   in it. Packed, the same six are 102,400 and all of them fit.

   BOTH ARE ACCEPTED HERE, and in src/rabisu.c too - IT IS PACKED NOW AS WELL.
   It pointed straight into its PVA1 buffer for months, and in September 2026
   that cost the Garden Courtyard the same way the pad cost this boss a clip:
   the room stopped loading at all. Its clip was converted in place with
   tools/pack_pva.py and it unpacks a frame exactly as this does. THE BLENDER
   ADD-ON STILL WRITES PVA1 BY DEFAULT - only tools/export_asag.py asks for
   packing - so a hand re-export of ANY clip in the game comes back a quarter
   bigger and has to be run through that script. A PVA1 clip dropped into this
   table keeps working, it is just bigger.

   >>> AND THE VERTEX-COUNT CHECK IS WHAT KEEPS A STALE CLIP FROM DRAWING
   GARBAGE. <<< Positions are indexed by the .smd's polygon indices, so a clip
   baked from a mesh with a different vertex count reads off the end of every
   frame. Refuse it and the body falls back to its bind pose, which always
   renders - a motionless boss instead of an exploded one. Mistake 5 in
   tools/ANIMATING_A_3D_MODEL.txt. */
static void load_clip(int c) {
    if (!mesh_smd) return;
    uint8_t *p = (uint8_t *)read_file(clip_file[c]);
    if (!p) return;
    int stride;
    if (p[0] == 'P' && p[1] == 'V' && p[2] == 'A' && p[3] == '1')      stride = 4;
    else if (p[0] == 'P' && p[1] == 'V' && p[2] == 'A' && p[3] == '2') stride = 3;
    else { free(p); return; }
    int n_verts  = p[4] | (p[5] << 8);
    int n_frames = p[6] | (p[7] << 8);
    if (n_verts != mesh_smd->n_verts || n_frames <= 0) { free(p); return; }
    clip_buff[c]   = p;
    clip_data[c]   = (int16_t *)(p + PVA_HEADER_SIZE);
    clip_stride[c] = (int8_t)stride;
    clip_count[c]  = n_frames;
}

static void load_mesh(void) {
    mesh_buff = read_file(ASAG_MESH_FILE);
    mesh_smd  = mesh_buff ? smdInitData(mesh_buff) : NULL;
    if (!mesh_smd) return;

    /* The mesh's XZ centre, taken ONCE. The fog is computed per BODY rather
       than per polygon (tools/ADDING_A_3D_ENEMY.txt STEP 5): the gradient
       across one model is invisible and the divides are not free. The bind pose
       is close enough for something rooted in one wall, and re-deriving it per
       frame from the clip would spend the saving it exists to make. */
    int32_t mnx = 32767, mxx = -32768, mnz = 32767, mxz = -32768;
    for (int v = 0; v < mesh_smd->n_verts; v++) {
        int32_t x = mesh_smd->p_verts[v].vx;
        int32_t z = mesh_smd->p_verts[v].vz;
        if (x < mnx) mnx = x;
        if (x > mxx) mxx = x;
        if (z < mnz) mnz = z;
        if (z > mxz) mxz = z;
    }
    mesh_cx = (int16_t)((mnx + mxx) / 2);
    mesh_cz = (int16_t)((mnz + mxz) / 2);

    /* One frame's worth of unpacked vertices, for PVA2 (see body_verts). 640
       bytes for this model, and it is taken BEFORE the clips so that if the
       heap is tight it is a CLIP that goes missing and not this - without it
       every packed clip would draw the bind pose. */
    pose_buf = (SVECTOR *)malloc(mesh_smd->n_verts * sizeof(SVECTOR));

    /* ---- WHICH PRIMITIVES ARE THE YELLOW SPOT, taken ONCE off their UVs ----
       See the ASAG_GLOW_* block for why this is a UV box and not five indices.
       The test is on the primitive's UV CENTROID rather than on "any corner
       inside", because the four walls of the pad each reach a long way out of
       the disc and an any-corner test would sweep in whatever they touch. On
       this sheet the two answers happen to agree, and the centroid is the one
       that keeps agreeing after a re-export.

       It costs one pass over 79 primitives at load and nothing per frame. */
    {
        uint8_t *p = (uint8_t *)mesh_smd->p_prims;
        int k;
        for (k = 0; k < ASAG_GLOW_WORDS; k++) glow_mask[k] = 0;
        for (k = 0; k < mesh_smd->n_prims; k++) {
            SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
            int      corners = (pt->type >= 2) ? 4 : 3;
            uint8_t *uv      = p + 20;
            int32_t  su = 0, sv = 0;
            int      c;
            if (k >= ASAG_GLOW_WORDS * 32) break;
            for (c = 0; c < corners; c++) { su += uv[c * 2]; sv += uv[c * 2 + 1]; }
            su /= corners;
            sv /= corners;
            if (su >= ASAG_GLOW_U0 && su <= ASAG_GLOW_U1 &&
                sv >= ASAG_GLOW_V0 && sv <= ASAG_GLOW_V1)
                glow_mask[k >> 5] |= 1u << (k & 31);
            p += pt->len;
        }
    }
}

void asags_load_model(void) {
    if (model_loaded) return;              /* already in */

    /* THE CD BRACKET IS MANDATORY. This is a mid-game read, and a data read
       issued while CD-DA streams HANGS THE DRIVE (tools/TEXTURE_STREAMING_DEBUG
       .txt). suspend/resume are no-ops when nothing is playing; the case they
       exist for is a debug level-select jump that arrives with a track running.
       Seven reads under ONE bracket, not seven brackets. */
    cdaudio_suspend();
    /* THE TAIL SCRATCH FIRST AND FREED LAST. One sector, shared by all six
       reads below; see read_file(). It is asked for before anything else so it
       lands in the low heap rather than on top of the clips, and a load that
       cannot get even this falls back to sector-rounded refusals rather than
       overrunning anything — read_file() checks. */
    tail_scratch = (uint8_t *)malloc(2048);
    load_mesh();
    for (int c = 0; c < ASAG_CLIP_COUNT; c++) load_clip(c);
    if (tail_scratch) { free(tail_scratch); tail_scratch = NULL; }
    cdaudio_resume();

    model_loaded = 1;
}

void asags_free_model(void) {
    if (mesh_buff) { free(mesh_buff); mesh_buff = NULL; }
    mesh_smd = NULL;                      /* the pointer the draw tests */
    if (pose_buf) { free(pose_buf); pose_buf = NULL; }
    for (int c = 0; c < ASAG_CLIP_COUNT; c++) {
        if (clip_buff[c]) { free(clip_buff[c]); clip_buff[c] = NULL; }
        clip_data[c]   = NULL;
        clip_stride[c] = 0;
        clip_count[c]  = 0;
    }
    model_loaded = 0;
}

int asag_model_loaded(void) { return model_loaded; }

/* ---- The clip API ------------------------------------------------------- */

/* Total length of a clip in GAME frames. n animation frames at ASAG_ANIM_FPS
   is n*60/FPS, which for 8 fps is n*7.5 - so this is deliberately integer
   division of the product and not (n/FPS)*60, which would throw away the half
   frame and drift the back-ramp off the end of the clip. */
static int32_t clip_total_ticks(int c) {
    return ((int32_t)clip_count[c] * 60) / ASAG_ANIM_FPS;
}

/* Ramp out, hold, ramp back. Linear, because that is what the brief specified;
   an ease would go here and nowhere else. */
static void update_pos(int c) {
    int32_t out = clip_move[c].out_ticks;
    if (out < 0) return;                  /* idle: hold the inherited offset */

    int32_t back  = clip_move[c].back_ticks;
    int32_t total = clip_total_ticks(c);
    int32_t t     = clip_ticks;

    /* A clip too short to hold both ramps would otherwise ramp back before it
       has finished ramping out and never reach Emerged at all. Give each ramp
       its share of what there is instead, so the shape degrades to
       "out then straight back" rather than to nonsense. Nothing in the current
       table hits this - the tightest is the vomit at 150 of 300 ticks - but a
       re-bake at a different rate could. */
    if (out + back > total && out + back > 0) {
        out  = (out  * total) / (out + back);
        back = total - out;
    }

    if (out > 0 && t < out) {
        pos_dz = ((int32_t)ASAG_EMERGE_DZ * t) / out;
    } else if (t < total - back) {
        pos_dz = ASAG_EMERGE_DZ;
    } else if (back > 0) {
        int32_t left = total - t;         /* counts down to 0 on the last tick */
        if (left < 0) left = 0;
        pos_dz = ((int32_t)ASAG_EMERGE_DZ * left) / back;
    } else {
        pos_dz = 0;
    }
}

void asag_play(AsagClip clip, int loop) {
    asag_play_at(clip, loop, 0);
}

/* >>> SEEKING IS DONE IN TICKS, NOT IN ANIMATION FRAMES, AND THAT IS THE WHOLE
   REASON THIS FUNCTION EXISTS RATHER THAN A `start_frame` ARGUMENT. <<<
   A clip is TWO tracks running at two different rates: the baked pose at
   ASAG_ANIM_FPS, and the position ramp at 60 (see the header). Seeking the pose
   alone would drop the body in a mid-faint posture at Home, when the whole
   point of starting two seconds in is that the lunge is two seconds in as well.
   Ticks are the unit both tracks share, they are game frames, and the brief is
   written in seconds — so "2 s in" is 120 and needs no conversion at the call
   site.

   The pose is then DERIVED from the tick count by the same arithmetic
   asag_update's accumulator would have reached, remainder included, so a seek to
   t and t frames of play land on the identical frame and the identical phase. A
   seek past the end clamps and comes up HELD, which is the state a finished
   one-shot is in — so a director that over-seeks gets the last frame rather than
   a clip that never reports done. */
void asag_play_at(AsagClip clip, int loop, int32_t start_ticks) {
    if (clip < 0 || clip >= ASAG_CLIP_COUNT) return;
    home_ramp = 0;      /* a director starting a clip has said where the body goes */
    cur_clip   = (int8_t)clip;
    clip_loop  = loop ? 1 : 0;
    clip_held  = 0;
    if (start_ticks < 0) start_ticks = 0;

    int32_t total = clip_total_ticks(clip);
    if (start_ticks > total) start_ticks = total;
    clip_ticks = start_ticks;

    /* The accumulator's state at this tick: whole frames and the leftover
       60ths. Exactly what asag_update() would be holding. */
    int32_t units = start_ticks * ASAG_ANIM_FPS;
    int32_t f     = units / 60;
    anim_acc      = (int16_t)(units % 60);

    int n = clip_count[clip];
    if (n <= 0) {
        anim_frame = 0;               /* absent clip: the bind pose, as always */
    } else if (f >= n) {
        if (clip_loop) {
            anim_frame = (int16_t)(f % n);
        } else {
            anim_frame = (int16_t)(n - 1);
            clip_held  = 1;
        }
    } else {
        anim_frame = (int16_t)f;
    }

    /* And the travel, so the FIRST FRAME DRAWN is already at the right offset.
       Without this the body would be posed mid-clip at Home for one frame and
       then snap, which on a reveal is the only frame anybody is looking at.
       pos_dz is otherwise never reset here — a clip with a ramp overwrites it,
       and the idle is supposed to inherit it (see asag_play's old note). */
    update_pos(clip);
}

void asag_stop(void) {
    cur_clip   = ASAG_CLIP_NONE;
    anim_frame = 0;
    anim_acc   = 0;
    clip_held  = 0;
    clip_ticks = 0;
    pos_dz     = 0;           /* the bind pose IS Home, by definition */
}

AsagClip asag_playing(void) { return (AsagClip)cur_clip; }

int asag_clip_loaded(AsagClip clip) {
    if (clip < 0 || clip >= ASAG_CLIP_COUNT) return 0;
    return clip_count[clip] > 0;
}

int asag_clip_done(void) { return clip_held; }

void asag_set_visible(int visible) { body_vis = visible ? 1 : 0; }

int asag_visible(void) { return body_vis; }

void asag_set_frozen(int frozen) { clip_frozen = frozen ? 1 : 0; }

int asag_frozen(void) { return clip_frozen; }

void asag_set_hit_glow(int on) { body_hit = on ? 1 : 0; }

/* Driven every frame by src/asag_fight.c from asag_exposed(), the same way the
   damage flash is: the fight owns the windows, this file owns the light. A flag
   and not a timer for the same reason — the exposure already has a definition
   and a second countdown could only disagree with it. */
void asag_set_head_glow(int on) { body_glow = on ? 1 : 0; }

void asag_set_shake(int32_t units) {
    if (units < 0) units = 0;
    if (units > 64) units = 64;      /* past this it stops reading as a body */
    body_shake = (int16_t)units;
}

void asag_set_fade(int32_t fade) {
    if (fade < 0)   fade = 0;
    if (fade > 256) fade = 256;
    body_fade = (int16_t)fade;
}

int32_t asag_fade(void) { return body_fade; }

/* >>> AN ACCUMULATOR, NOT A TICK COUNTDOWN, AND THE HEADER SAYS WHY. <<< The
   clips play at 8 fps and 60/8 is 7.5, so there is no whole number of game
   frames per animation frame. Adding ASAG_ANIM_FPS per game frame and stepping
   whenever the total passes 60 is exact on average where a rounded 7 or 8
   would drift most of a second across the 50-frame slam.

   The `while` rather than `if` is not defensive padding: it is what keeps this
   correct if ASAG_ANIM_FPS ever goes above 60 (a clip baked at step 1 plays at
   24, which is still under, but step-free art would not be).

   Snapping between baked frames with no interpolation, exactly as the Rabisu
   does — it costs nothing and the stepping is period-correct. */
void asag_update(void) {
    int c = cur_clip;
    if (c == ASAG_CLIP_NONE) return;
    int n = clip_count[c];
    if (n <= 0) return;             /* clip absent or rejected */

    /* >>> FROZEN STOPS BOTH TRACKS, NOT JUST THE POSE. <<< The position clock
       below is deliberately allowed to run on past the end of a held clip, so
       a back-ramp always reaches Home — but a DIRECTOR that has asked for the
       body to hold still wants it to hold still in space as well, or Asag goes
       on sliding out of the wall underneath a motionless pose. One early
       return covers both. See the header. */
    if (clip_frozen) return;

    if (!clip_held) {
        anim_acc += ASAG_ANIM_FPS;
        while (anim_acc >= 60) {
            anim_acc -= 60;
            if (++anim_frame >= n) {
                if (clip_loop) {
                    anim_frame = 0;
                } else {
                    /* HOLD the last frame rather than snapping back to the bind
                       pose. That is what lets a one-shot leave the body where
                       the animator left it, and it is why asag_clip_done() is a
                       separate question from "is a clip set". */
                    anim_frame = (int16_t)(n - 1);
                    clip_held  = 1;
                    break;
                }
            }
        }
    }

    /* >>> THE POSITION CLOCK RUNS EVEN ON A HELD FRAME, AND THAT IS THE POINT.
       <<< It is what guarantees the back-ramp reaches Home: the pose stops
       advancing the moment clip_held goes up, and if the travel stopped with it
       the body would be left stranded wherever the ramp had got to. Clamped at
       the total so a clip held for a minute does not run pos_dz past Home. */
    {
        int32_t total = clip_total_ticks(c);
        if (clip_ticks < total) clip_ticks++;
        /* THE DEATH'S RAMP WINS while it runs — see home_ramp above. The clip's
           own clock still advances underneath it so that releasing the ramp
           (which nothing does today) would not leave the body stranded. */
        if (home_ramp > 0) {
            home_ramp--;
            pos_dz = home_total > 0
                   ? home_to + ((home_from - home_to) * home_ramp) / home_total
                   : home_to;
        } else {
            update_pos(c);
        }
    }
}

/* Slide the body to `dz` over `ticks`, whatever the playing clip's own position
   track would have done. See home_ramp above for why this exists. A `ticks` of
   0 or less puts it there this instant. */
void asag_ramp_dz(int32_t dz, int32_t ticks) {
    home_from  = pos_dz;
    home_to    = dz;
    home_total = ticks;
    home_ramp  = ticks;
    if (ticks <= 0) { pos_dz = dz; home_ramp = 0; home_total = 0; }
}

void asag_ramp_home(int32_t ticks) { asag_ramp_dz(0, ticks); }

/* How long a clip runs, in GAME FRAMES. Exposed so a director can say "0.5 s
   before this animation ends" without restating a number that is really
   (frames * 60 / ASAG_ANIM_FPS) and would go silently stale the next time the
   clips are re-baked at a different step. 0 for a clip that is not loaded,
   which is also the honest answer — an absent clip takes no time at all. */
int32_t asag_clip_ticks(AsagClip clip) {
    if (clip < 0 || clip >= ASAG_CLIP_COUNT) return 0;
    if (clip_count[clip] <= 0) return 0;
    return clip_total_ticks(clip);
}

/* How far into the playing clip we are, in game frames. Pairs with the above:
   the two together are what an exposure window is expressed in. */
int32_t asag_clip_elapsed(void) { return clip_ticks; }

/* The vertex block the body is posed on this frame. Falls back to the .smd's
   own bind pose whenever the clip is missing or was rejected - which is also
   the state the body is in until a director starts something.

   PVA1 IS RETURNED IN PLACE: four int16 per vertex is an SVECTOR, the header is
   12 bytes so every frame is 4-byte aligned, and the draw walks the file.

   PVA2 IS UNPACKED into pose_buf first, because three int16 per vertex is not
   an SVECTOR and the GTE load wants the real thing. >>> AND IT IS UNPACKED
   UNCONDITIONALLY, EVERY FRAME, RATHER THAN CACHED. <<< Caching it would mean
   tracking which clip and which frame the buffer currently holds and
   invalidating that on play, stop, free and load - four places to get wrong for
   a saving of 80 iterations on a model this size. If a later Asag is thousands
   of vertices, cache it then and key the cache on (cur_clip, anim_frame). */
static SVECTOR *body_verts(void) {
    int nv = mesh_smd->n_verts;
    int c  = cur_clip;

    int16_t *src    = NULL;
    int      stride = 0;

    if (c != ASAG_CLIP_NONE && clip_data[c] && clip_count[c] > 0) {
        int f = anim_frame;
        if (f < 0 || f >= clip_count[c]) f = 0;
        stride = clip_stride[c];
        src    = clip_data[c] + (int32_t)f * nv * stride;
    } else {
        src    = (int16_t *)mesh_smd->p_verts;   /* the bind pose = Home */
        stride = 4;
    }

    /* THE ONE CASE THAT COSTS NOTHING: an unpacked PVA1 frame (or the .smd's own
       vertices) sitting at Home is already an SVECTOR array in the right place,
       so hand it straight to the draw. Everything else has to be built. */
    if (stride == 4 && pos_dz == 0) return (SVECTOR *)src;

    if (!pose_buf) return mesh_smd->p_verts;   /* no scratch: hold the bind pose */

    /* Unpack and/or translate into the scratch. The Z add is FREE here - this
       loop already exists to turn PVA2's three int16 into an SVECTOR, so
       sliding the body between Home and Emerged costs one addition per vertex
       and no matrix at all. That is why asag.h can still say this boss loads no
       model matrix and restores no view. */
    SVECTOR *d = pose_buf;
    for (int v = 0; v < nv; v++) {
        d[v].vx  = *src++;
        d[v].vy  = *src++;
        d[v].vz  = (int16_t)(*src++ + pos_dz);
        if (stride == 4) src++;            /* PVA1/SMD pad */
        d[v].pad = 0;
    }
    return pose_buf;
}

/* ---- The draw --------------------------------------------------------------
   asag_arena.c's draw_asag_arena_smd() with three differences, and NOTHING
   else:

     - the vertices come from body_verts(), not from the .smd, so a clip poses
       the same prim stream the bind pose does;
     - the fog is computed ONCE for the whole body rather than per polygon;
     - the side-plane frustum cull is GONE. It exists in the room's loop to
       reject most of a 522-primitive mesh; this one is 79, so the test would
       cost more than it saves.

   >>> NO MATRIX IS LOADED AND NONE IS RESTORED. <<< Every vertex is already in
   the arena's coordinate space (asag.h), so this draws under the view
   asag_arena_draw() set with camera_build_view() and leaves the GTE exactly as
   it found it. That is why there is no early-out path here that has to remember
   to put a matrix back — the trap tools/ADDING_A_3D_ENEMY.txt STEP 5 is about
   does not exist in this shape. */
void asag_draw(RenderContext *ctx) {
    SMD *smd = mesh_smd;
    if (!model_loaded || !smd || !body_vis) return;

    /* ---- THE EXPOSURE LIGHT'S CLOCK, ticked here and not in asag_update() ---
       asag_update() is skipped entirely while `frozen`, and a lamp that stalled
       under a held pose would be a second piece of state to reason about in
       every scene that holds him. The draw runs every frame he is on screen,
       which is exactly when the light means anything.

       glow_amt is the ramp times the pulse, 0..256, and it is 0 for the whole
       of the game that is not an exposure window — so everything the glow costs
       below is behind one compare. */
    if (body_glow) { if (glow_lvl < 256) { glow_lvl += 256 / ASAG_GLOW_RAMP;
                                           if (glow_lvl > 256) glow_lvl = 256; } }
    else           { if (glow_lvl > 0)   { glow_lvl -= 256 / ASAG_GLOW_RAMP;
                                           if (glow_lvl < 0)   glow_lvl = 0; } }
    glow_ph = (int16_t)((glow_ph + 1) % ASAG_GLOW_PULSE);
    int32_t glow_amt = 0;
    if (glow_lvl > 0) {
        /* isin over one whole turn per ASAG_GLOW_PULSE frames, mapped from
           +/-4096 onto 168..256 — a breath, not a flicker. */
        int32_t s = isin(((int32_t)glow_ph * 4096) / ASAG_GLOW_PULSE);
        glow_amt  = (glow_lvl * (212 + (s * 44 >> 12))) >> 8;
        if (glow_amt > 256) glow_amt = 256;
    }

    SVECTOR *vp = body_verts();
    uint8_t *p  = (uint8_t *)smd->p_prims;
    int n = smd->n_prims;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = ASAG_CULL_DIST;

    /* Whole-body distance reject and whole-body fog, both from the mesh centre
       taken at load. */
    /* The fog and the distance reject follow the body OUT. mesh_cz is the bind
       pose's centre, i.e. Home; without pos_dz a lunging Asag would keep Home's
       fog and stay dimmer than the ground it is standing over. */
    int32_t pdx  = (int32_t)mesh_cx - cam_x;
    int32_t pdz  = (int32_t)mesh_cz + pos_dz - cam_z;
    int32_t pdst = (pdx < 0 ? -pdx : pdx) + (pdz < 0 ? -pdz : pdz);
    if (pdst > cull) return;

    int32_t fog = pdst < g_fog_near ? g_fog_near
                                    : (pdst > g_fog_far ? g_fog_far : pdst);
    int32_t ff  = ((g_fog_far - fog) << 8) / (g_fog_far - g_fog_near);

    uint16_t tpage = asag_arena_tex_page(ASAG_TEX_SKIN);
    uint16_t clut  = asag_arena_tex_clut(ASAG_TEX_SKIN);

    /* ---- THE DEATH'S TWO FIELDS -------------------------------------------
       Both are no-ops at rest — fade 256 and shake 0 — so everything below
       costs one compare a frame for the 99% of the game that is not a death.

       ONE JITTER FOR THE WHOLE BODY, SOLVED HERE AND NOT PER VERTEX. A
       per-vertex offset would not read as a body vibrating, it would read as
       the mesh boiling. */
    int32_t shk_x = 0, shk_y = 0, shk_z = 0;
    if (body_shake) {
        int32_t a = body_shake * 2 + 1;
        shk_x = (rand() % a) - body_shake;
        shk_y = (rand() % a) - body_shake;
        shk_z = (rand() % a) - body_shake;
    }

    /* >>> A TEXTURED POLY CANNOT FADE, SO THE BURN DROPS THE TEXTURE. <<<
       tools/ADDING_A_BOSS_ENCOUNTER.txt TRICK 1: there is no alpha on a flat
       poly, darkening gives a black silhouette and fogging only works against
       the sky. What works is ADDITIVE — a colour scaled toward black, which
       contributes nothing at all at 0 and is therefore genuinely gone over the
       geometry as well as over the clear colour. So under 256 the body draws as
       POLY_F4/F3 in one colour rather than POLY_FT4/FT3 in its skin, exactly
       the swap src/rabisu.c makes and the reason RBS_BODY_R/G/B still exists
       over there. This colour is Asag's: the hot orange-white of something
       coming apart, not his skin. */
    int fading = (body_fade < 256);
    uint8_t burn_r = 0, burn_g = 0, burn_b = 0;
    if (fading) {
        if (body_fade == 0) return;   /* gone: 79 prims a frame saved as well */
        /* RED WHILE HE IS DYING, which he always is when this runs: the fight
           holds body_hit up for the whole death, so the glow he has been
           burning with since the killing blow carries through the fade instead
           of switching to a different colour halfway out. The hot orange is
           what is left if anything ever fades him for another reason. */
        if (body_hit) { burn_r = 255; burn_g =  40; burn_b =  30; }
        else          { burn_r = 255; burn_g = 170; burn_b =  60; }
        burn_r = (uint8_t)((burn_r * body_fade) >> 8);
        burn_g = (uint8_t)((burn_g * body_fade) >> 8);
        burn_b = (uint8_t)((burn_b * body_fade) >> 8);
    }

    for (int k = 0; k < n; k++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);
        int stride  = pt->len;

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &vp[vi[0]];
        SVECTOR *v1 = &vp[vi[1]];
        SVECTOR *v2 = &vp[vi[2]];

        /* The shake displaces COPIES. vp may point straight into a PVA1 file
           (see body_verts) and writing through it would corrupt the clip for
           every later frame — and even for PVA2, whose pose_buf is rebuilt each
           frame, offsetting in place would put the jitter into asag_collide()
           and asag_head_box(), which both read the same block. The pose the
           fight reasons about must not vibrate; only the picture does. */
        SVECTOR sk[4];
        if (body_shake) {
            sk[0] = *v0; sk[1] = *v1; sk[2] = *v2;
            for (int j = 0; j < 3; j++) {
                sk[j].vx = (int16_t)(sk[j].vx + shk_x);
                sk[j].vy = (int16_t)(sk[j].vy + shk_y);
                sk[j].vz = (int16_t)(sk[j].vz + shk_z);
            }
            v0 = &sk[0]; v1 = &sk[1]; v2 = &sk[2];
        }

        DVECTOR sv[4];
        int32_t sz[4];
        int32_t otz, nclip;

        gte_ldv3(v0, v1, v2);
        gte_rtpt();
        gte_stsxy3c(sv);

        /* The GTE clamps screen coordinates to +/-1023 and a primitive with a
           corner past it comes out folded. Drop the whole thing. */
        if (sv[0].vx <= -1023 || sv[0].vx >= 1023 || sv[0].vy <= -1023 || sv[0].vy >= 1023 ||
            sv[1].vx <= -1023 || sv[1].vx >= 1023 || sv[1].vy <= -1023 || sv[1].vy >= 1023 ||
            sv[2].vx <= -1023 || sv[2].vx >= 1023 || sv[2].vy <= -1023 || sv[2].vy >= 1023) {
            p += stride; continue;
        }

        /* KEEP THE BACKFACE CULL. A solid model genuinely has back faces and
           culling them halves what reaches the GPU; the rolling-sprite argument
           in ADDING_AN_ENEMY.txt's mistake 4 does not transfer. This mesh was
           verified with tools/check_model_winding.py (OK, 1 shell, 4 open
           edges) — the first Asag shipped six boils and a tentacle inside-out,
           so it is worth re-running after any re-export. The SMD's own nocull
           bit still exempts whatever asks to be exempt. */
        if (!pt->nocull) {
            gte_nclip();
            gte_stopz(&nclip);
            if (nclip <= 0) { p += stride; continue; }
        }

        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        SVECTOR *v3    = 0;
        int32_t  v2_sz = sz[3];
        if (is_quad) {
            v3 = &vp[vi[3]];
            if (body_shake) {
                sk[3] = *v3;
                sk[3].vx = (int16_t)(sk[3].vx + shk_x);
                sk[3].vy = (int16_t)(sk[3].vy + shk_y);
                sk[3].vz = (int16_t)(sk[3].vz + shk_z);
                v3 = &sk[3];
            }
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
        /* Horizontal polys sort by their farthest corner so a flat face stays
           behind whatever stands on it. */
        if (poly_is_flat_y(v0, v1, v2, v3))
            otz = is_quad ? otz_far4(sz[1], sz[2], v2_sz, sz[3])
                          : otz_far3(sz[1], sz[2], sz[3]);
        if (otz <= 0) { p += stride; continue; }

        /* +38, TWO NEARER THAN THE ROOM'S +40. The arena's own mesh biases its
           primitives back by 40 so its floor does not fight what stands on it;
           Asag is rooted IN the rock behind the back wall and its silhouette
           meets that wall, so at the same bias the two meshes tie and the wall
           wins on some frames and not others. Biasing the boss two steps nearer
           breaks the tie the same way every frame. */
        otz += 38;
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        /* The .smd's baked RGB is a neutral 128,128,128 once a model is
           textured, so fogging it is what makes the boss fade INTO the room
           rather than alongside it (ADDING_A_3D_ENEMY.txt STEP 3). */
        uint8_t *col = p + 16;
        uint8_t r = (uint8_t)(((int32_t)col[0] * ff + ASAG_FOG_R * (256 - ff)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * ff + ASAG_FOG_G * (256 - ff)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * ff + ASAG_FOG_B * (256 - ff)) >> 8);

        /* ---- THE EXPOSURE LIGHT, on the five polygons of the crown pad ----
           The skin is MODULATED toward blue and that is the whole of it: the
           additive halo that used to be laid over the finished poly is gone
           (see the ASAG_GLOW_* block for why). The pad glows blue; it does not
           spill.

           THE TINT IS HEAVY ON PURPOSE. Modulation is texel * colour / 128, and
           the disc is a saturated yellow (about 230, 230, 60), so anything less
           than crushing R and G comes out green rather than blue. See the
           ASAG_GLOW_* block.

           APPLIED BEFORE THE DAMAGE FLASH, so a hit that lands during a window
           still reads as red. The two say different things and the more urgent
           one wins the frame. */
        if (glow_amt > 0 && k < ASAG_GLOW_WORDS * 32 &&
            ((glow_mask[k >> 5] >> (k & 31)) & 1u)) {
            r = (uint8_t)(r + (((int32_t)ASAG_GLOW_TINT_R - r) * glow_amt >> 8));
            g = (uint8_t)(g + (((int32_t)ASAG_GLOW_TINT_G - g) * glow_amt >> 8));
            b = (uint8_t)(b + (((int32_t)ASAG_GLOW_TINT_B - b) * glow_amt >> 8));
        }

        /* THE DAMAGE FLASH, applied AFTER the fog for the same reason the
           arena's boils are: it is light coming out of him, and light does not
           dim with the distance to the thing it is coming out of. R to full and
           G/B cut to a quarter, which is src/rabisu.c's exact modulation - the
           skin is still sampled underneath, so he reads as lit from inside
           rather than as a red silhouette. */
        if (body_hit) { r = 255; g = (uint8_t)(g >> 2); b = (uint8_t)(b >> 2); }

        /* ---- THE BURN'S OWN PRIMITIVE. Untextured, additive, one colour, and
               it takes the same OT bucket the textured one would have. The
               DR_TPAGE that selects ABR=1 goes in AFTER the poly because the OT
               is LIFO, so "after" is what puts it in front — the same pattern
               rbs_glow_quad and lightswitch_puzzle.c's ls_quad use. ---- */
        if (fading) {
            size_t need = (is_quad ? sizeof(POLY_F4) : sizeof(POLY_F3))
                          + sizeof(DR_TPAGE);
            if (ctx->next_packet + need > buf_end) { p += stride; continue; }
            uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
            if (is_quad) {
                POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
                setPolyF4(poly);
                setSemiTrans(poly, 1);
                setRGB0(poly, burn_r, burn_g, burn_b);
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
                addPrim(&ot[otz], poly);
                ctx->next_packet += sizeof(POLY_F4);
            } else {
                POLY_F3 *poly = (POLY_F3 *)ctx->next_packet;
                setPolyF3(poly);
                setSemiTrans(poly, 1);
                setRGB0(poly, burn_r, burn_g, burn_b);
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                addPrim(&ot[otz], poly);
                ctx->next_packet += sizeof(POLY_F3);
            }
            DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
            setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
            addPrim(&ot[otz], tp);
            ctx->next_packet += sizeof(DR_TPAGE);
            p += stride;
            continue;
        }

        uint8_t *uv = p + 20;
        if (is_quad) {
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, r, g, b);
            poly->tpage = tpage;
            poly->clut  = clut;
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
        } else {
            if (ctx->next_packet + sizeof(POLY_FT3) > buf_end) { p += stride; continue; }
            POLY_FT3 *poly = (POLY_FT3 *)ctx->next_packet;
            setPolyFT3(poly);
            setRGB0(poly, r, g, b);
            poly->tpage = tpage;
            poly->clut  = clut;
            poly->u0=uv[0]; poly->v0=uv[1];
            poly->u1=uv[2]; poly->v1=uv[3];
            poly->u2=uv[4]; poly->v2=uv[5];
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_FT3);
        }

        p += stride;
    }
}

/* ---- Solidity --------------------------------------------------------------
   See asag.h for why this exists at all and why it replaced 26 baked walls.

   HOW MUCH SLACK THE VERTICAL BAND GETS, AND WHY IT IS NOT ZERO. The selection
   takes vertices whose Y lies within the player's span WIDENED BY
   ASAG_SOLID_Y_SLACK. Without the slack a part of the body could pass THROUGH
   the band steeply enough that no vertex landed inside it — the edge crosses,
   both its endpoints miss, and the player walks through a leg. Vertex spacing
   along this model is 100-200 units against a 179-unit band, so that is a real
   case and not a hypothetical. The slack is deliberately modest: every unit of
   it also fattens the box when the body is merely PASSING overhead, and an
   invisible barrier under a boss that is clearly above you is worse than a
   rare clip through a thin part. */
#define ASAG_SOLID_Y_SLACK  60

void asag_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    /* Area-gated inside, like every prop family in apply_collision_reception,
       so the call site stays unconditional. */
    if (current_area != STATE_ASAG_ARENA) return;
    if (!model_loaded || !mesh_smd || !body_vis) return;

    /* The player's body span, the same expression rabisus_collide uses. */
    int32_t band_top = (py - 30)  - ASAG_SOLID_Y_SLACK;
    int32_t band_bot = (py + GROUND_FLOOR_Y) + ASAG_SOLID_Y_SLACK;

    /* THIS FRAME'S POSE AND POSITION, straight out of the draw's own accessor -
       so the thing that pushes the player is by construction the thing on the
       screen. body_verts() has already applied pos_dz. */
    SVECTOR *vp = body_verts();
    int nv = mesh_smd->n_verts;

    int32_t min_x = 32767, max_x = -32768, min_z = 32767, max_z = -32768;
    int found = 0;
    for (int v = 0; v < nv; v++) {
        int32_t y = vp[v].vy;
        if (y < band_top || y > band_bot) continue;
        int32_t x = vp[v].vx, z = vp[v].vz;
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (z < min_z) min_z = z;
        if (z > max_z) max_z = z;
        found = 1;
    }
    if (!found) return;        /* nothing of Asag is at body height this frame */

    min_x -= radius; max_x += radius;
    min_z -= radius; max_z += radius;

    if (*px <= min_x || *px >= max_x) return;
    if (*pz <= min_z || *pz >= max_z) return;

    /* SMALLEST PENETRATION, all four candidates. src/hatch_doors.c had to drop
       two of its four because the far side of a pit door is the hole itself and
       the shortest way out led somewhere the walls immediately pushed back
       from. Nothing like that applies here: Asag comes down in the middle of an
       open floor - the slam's lowest frames sit around z[1380,2006] with the
       side walls 1500 out and the back wall 600 further on - so every direction
       out of this box leads somewhere the player can stand. */
    int32_t push_w = *px - min_x;      /* out to -X */
    int32_t push_e = max_x - *px;      /* out to +X */
    int32_t push_n = *pz - min_z;      /* out to -Z, toward the shaft */
    int32_t push_s = max_z - *pz;      /* out to +Z, toward the back wall */

    int32_t best = push_w, dx = -push_w, dz = 0;
    if (push_e < best) { best = push_e; dx =  push_e; dz = 0; }
    if (push_n < best) { best = push_n; dx = 0; dz = -push_n; }
    if (push_s < best) {                dx = 0; dz =  push_s; }

    *px += dx;
    *pz += dz;
}

/* ---- Where the body IS, for a camera to aim at -----------------------------
   The centre of an AABB over EVERY posed vertex — no Y band, unlike
   asag_collide's. The two want opposite things: a collider wants only the part
   at the player's height, and a camera wants the whole silhouette, because what
   it is framing is the animal and not its footprint.

   >>> IT IS THE SAME VERTEX BLOCK THE DRAW USES, WHICH IS WHAT MAKES IT WORTH
   HAVING. <<< A director could instead aim at Home plus ASAG_EMERGE_DZ times
   some guess at the ramp's progress, and it would be wrong twice over: it would
   miss the pose entirely (the faint drops the body to floor level and the idle
   bobs) and it would have to re-derive a ramp that asag.h is explicit about
   owning. Asking the body where it is costs one pass over 80 vertices.

   Returns 0 and leaves `out` untouched when there is nothing posed — no model,
   or hidden. A camera that gets 0 should hold its last aim rather than swing to
   the origin. */
int asag_body_centre(VECTOR *out) {
    if (!out) return 0;
    if (!model_loaded || !mesh_smd) return 0;

    SVECTOR *vp = body_verts();
    int nv = mesh_smd->n_verts;
    if (nv <= 0) return 0;

    int32_t min_x = vp[0].vx, max_x = min_x;
    int32_t min_y = vp[0].vy, max_y = min_y;
    int32_t min_z = vp[0].vz, max_z = min_z;
    for (int v = 1; v < nv; v++) {
        int32_t x = vp[v].vx, y = vp[v].vy, z = vp[v].vz;
        if (x < min_x) min_x = x; else if (x > max_x) max_x = x;
        if (y < min_y) min_y = y; else if (y > max_y) max_y = y;
        if (z < min_z) min_z = z; else if (z > max_z) max_z = z;
    }

    out->vx = (min_x + max_x) / 2;
    out->vy = (min_y + max_y) / 2;
    out->vz = (min_z + max_z) / 2;
    return 1;
}

/* ---- The FACE, which is what a camera actually wants to look at -----------
   The centroid of every posed vertex within ASAG_FACE_WINDOW of the front-most
   one. Asag lies along Z with his head at the low end and his tail at the high
   end (the bind pose spans z[2262,3931], and the tail cluster at 3607..3931 is
   what ASAG_EMERGE_DZ is derived from), so "front-most in Z" IS the head.

   >>> A WINDOW AND NOT A COUNT, AND THAT IS THE WHOLE DESIGN OF IT. <<< The
   obvious version is "average the eight front-most vertices", and eight is
   right for the rest pose — the head's front cluster is exactly eight, spanning
   52 units. It is wrong the moment the body deforms: measured across the faint,
   the front-eight-by-Z is NINE DIFFERENT INDEX SETS, because a count has to
   keep taking eight vertices whether or not eight belong to the face. The
   window takes however many are actually up there — 8 at rest, 21 when the
   faint has him flattened and much of him is near the front — which is a
   statement about the geometry rather than about the topology, and it needs no
   sort.

   The two agree to within 50 units of Y and 32 of Z across the whole faint,
   i.e. well under a degree of aim at this room's ranges, so the cheap robust
   one is simply better.

   >>> AND IT IS NOT asag_body_centre(). <<< That is the middle of a
   1669-unit-long animal; aiming a close shot at it points the camera at his
   flank. Three accessors, three jobs: the collider wants the part at the
   player's height, a wide shot wants the whole silhouette, and a shot of his
   face wants his face.

   Returns 0 with `out` untouched when there is nothing posed. */
#define ASAG_FACE_WINDOW  90

int asag_face_point(VECTOR *out) {
    if (!out) return 0;
    if (!model_loaded || !mesh_smd) return 0;

    SVECTOR *vp = body_verts();
    int nv = mesh_smd->n_verts;
    if (nv <= 0) return 0;

    int32_t front = vp[0].vz;
    for (int v = 1; v < nv; v++)
        if (vp[v].vz < front) front = vp[v].vz;

    int32_t lim = front + ASAG_FACE_WINDOW;
    int32_t sx = 0, sy = 0, sz = 0, n = 0;
    for (int v = 0; v < nv; v++) {
        if (vp[v].vz > lim) continue;
        sx += vp[v].vx; sy += vp[v].vy; sz += vp[v].vz;
        n++;
    }
    if (n <= 0) return 0;       /* impossible — the front vertex is its own */

    out->vx = sx / n;
    out->vy = sy / n;
    out->vz = sz / n;
    return 1;
}

/* ---- ...and the HEAD AS A TARGET, which is what a WEAPON wants --------------
   The same Z window asag_face_point() uses, reported as a cylinder instead of a
   point: centre, half-width and half-height over exactly those vertices.

   >>> IT IS A FOURTH ACCESSOR AND IT IS NOT A DUPLICATE OF THE THIRD. <<< A
   camera wants ONE POINT to aim at and gets the centroid; a gun wants an
   EXTENT to hit and gets the bounding box, and the two differ by a lot on a
   deformed head — the centroid of the faint's 22 front vertices sits well
   inside a box those 22 vertices span. Averaging and then assuming a radius
   would make the head hittable at rest and miss it flat on the floor.

   THE WINDOW IS SHARED ON PURPOSE. "The head portion of Asag as a whole" is the
   brief's phrase for what counts as a hit, and if the camera's idea of his face
   and the gun's idea of his head came off two different definitions, the shot
   the player lines up would not be the shot that lands. One constant, two
   readers.

   >>> AND IT HAS A FLOOR ON BOTH HALF-SIZES. <<< At rest the front cluster is
   eight vertices spanning about 100 units across and 60 tall, which at the
   1600-2000 ranges this arena is fought at is a few pixels — technically
   correct and unhittable. ASAG_HEAD_MIN_HALF makes the target the size of the
   thing the player can see rather than the size of the vertex cluster, which is
   the same correction weapon_aim_in_circle's callers make by quoting HALF_W and
   HALF_H rather than measuring a sprite.

   It is deliberately NOT the collision box: asag_collide() selects on Y first
   because a collider wants the part of him at the player's height, and the head
   is usually nowhere near it. Reads the UNSHAKEN pose, so a boss vibrating
   through his death is still exactly where the geometry says.

   Returns 0 with nothing written when there is no posed body. */
#define ASAG_HEAD_MIN_HALF  170

int asag_head_box(int32_t *cx, int32_t *cy, int32_t *cz,
                  int32_t *half_w, int32_t *half_h) {
    if (!model_loaded || !mesh_smd) return 0;

    SVECTOR *vp = body_verts();
    int nv = mesh_smd->n_verts;
    if (nv <= 0) return 0;

    int32_t front = vp[0].vz;
    for (int v = 1; v < nv; v++)
        if (vp[v].vz < front) front = vp[v].vz;

    int32_t lim = front + ASAG_FACE_WINDOW;
    int32_t mnx = 32767, mxx = -32768;
    int32_t mny = 32767, mxy = -32768;
    int32_t mnz = 32767, mxz = -32768;
    int n = 0;
    for (int v = 0; v < nv; v++) {
        if (vp[v].vz > lim) continue;
        int32_t x = vp[v].vx, y = vp[v].vy, z = vp[v].vz;
        if (x < mnx) mnx = x;
        if (x > mxx) mxx = x;
        if (y < mny) mny = y;
        if (y > mxy) mxy = y;
        if (z < mnz) mnz = z;
        if (z > mxz) mxz = z;
        n++;
    }
    if (n <= 0) return 0;

    /* The width is taken over X AND Z, because the cylinder the weapons test
       against is round in plan and the head is as deep as it is wide. Taking X
       alone would make him easy to hit from the front and hard from the side,
       which in this arena — where the player circles a boss that cannot turn —
       would read as the hit box drifting. */
    int32_t hw_x = (mxx - mnx) / 2;
    int32_t hw_z = (mxz - mnz) / 2;
    int32_t hw   = hw_x > hw_z ? hw_x : hw_z;
    int32_t hh   = (mxy - mny) / 2;
    if (hw < ASAG_HEAD_MIN_HALF) hw = ASAG_HEAD_MIN_HALF;
    if (hh < ASAG_HEAD_MIN_HALF) hh = ASAG_HEAD_MIN_HALF;

    if (cx)     *cx = (mnx + mxx) / 2;
    if (cy)     *cy = (mny + mxy) / 2;
    if (cz)     *cz = (mnz + mxz) / 2;
    if (half_w) *half_w = hw;
    if (half_h) *half_h = hh;
    return 1;
}

/* ---- POINTS SPREAD ALONG HIS LENGTH, FOR THE DEATH'S LIGHTS ----------------
   The centroid of the posed vertices in the i-th of `n` equal slices of his
   CURRENT Z extent. Fills `out` and returns 1; returns 0 for a slice that holds
   no vertices, which a caller should simply skip.

   >>> IT IS THE ANSWER TO A QUESTION THE RABISU SOLVED WITH LITERALS, AND IT
   HAD TO BE. <<< That boss hangs its four death lights on RBS_A_HEAD/WING/CHEST
   - mesh-local anchors read out of the .pva once, by hand, at the frame its
   death freezes on, and turned into world points by rabisu_anchor_world()
   through the same yaw and lean the draw uses.

   Neither half of that transfers. Asag's vertices are already in world space,
   so there is no anchor transform to reproduce; and he has no head/wings/chest
   to name - he is a 1669-unit animal lying along the view axis, and what the
   brief wants is light coming out of him ALONG HIS LENGTH. Slices say that
   directly, they need no hand measurement, and they follow the pose: the same
   call gives points spread along him whether he is stretched out at Home or
   collapsed on the floor.

   It reads the UNSHAKEN pose, like every other accessor here, so the lights sit
   on the body rather than vibrating independently of it. */
int asag_span_point(int i, int n, VECTOR *out) {
    if (!out || n <= 0 || i < 0 || i >= n) return 0;
    if (!model_loaded || !mesh_smd) return 0;

    SVECTOR *vp = body_verts();
    int nv = mesh_smd->n_verts;
    if (nv <= 0) return 0;

    int32_t lo = vp[0].vz, hi = lo;
    for (int v = 1; v < nv; v++) {
        if (vp[v].vz < lo) lo = vp[v].vz;
        if (vp[v].vz > hi) hi = vp[v].vz;
    }
    /* The slice is half-open [z0, z1) so no vertex is counted twice, except the
       last, which takes the far edge so the tail-most vertex belongs to
       something. */
    int32_t span = hi - lo;
    int32_t z0 = lo + (span * i)       / n;
    int32_t z1 = lo + (span * (i + 1)) / n;

    int32_t sx = 0, sy = 0, sz = 0, cnt = 0;
    for (int v = 0; v < nv; v++) {
        int32_t z = vp[v].vz;
        if (z < z0) continue;
        if (z > z1 || (z == z1 && i != n - 1)) continue;
        sx += vp[v].vx; sy += vp[v].vy; sz += z;
        cnt++;
    }
    if (cnt <= 0) return 0;

    out->vx = sx / cnt;
    out->vy = sy / cnt;
    out->vz = sz / cnt;
    return 1;
}

/* Visible and on the bind pose. This runs from asag_arena_init(), i.e. on every
   arrival, so a debug jump into the room finds the same state a real drop does.
   It is not the load: asags_load_model() runs in main.c's STATE_LOADING beside
   the Rabisu's, and this only resets the playback state, so it is safe on a
   debug jump that arrives before the read has happened. */
void asag_reset(void) {
    body_vis   = 1;
    cur_clip   = ASAG_CLIP_NONE;
    anim_frame = 0;
    anim_acc   = 0;
    clip_loop  = 0;
    clip_held  = 0;
    clip_ticks = 0;
    clip_frozen = 0;          /* a director's hold does not survive an arrival */
    pos_dz     = 0;           /* Home */
    /* ...nor does a half-finished death. A fade left at 0 would make the boss
       invisible for the whole of the next visit, which presents as a model that
       failed to load; a shake left on would vibrate an idle. */
    body_shake = 0;
    body_fade  = 256;
    body_hit   = 0;
    /* ...and the exposure light, which is a statement about a fight that is
       over. Left up, it would greet the next visit with a boss who looks
       vulnerable through his whole opening scene. */
    body_glow  = 0;
    glow_lvl   = 0;
    glow_ph    = 0;
    home_ramp  = 0;
    home_total = 0;
    home_from  = 0;
    home_to    = 0;
}
