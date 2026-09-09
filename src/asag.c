#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "collision.h"   /* DEBUG_CULL_DIST */
#include "cdaudio.h"       /* the bracket around the entry-time reads */
#include "asag_arena.h"    /* asag_arena_tex_slot — the room streams the skins */
#include "asag.h"

/* ASAG — the body. See asag.h for the whole argument; this file is the
   mechanism. Nothing here decides anything about the fight. */

/* ---- The parts -------------------------------------------------------------
   One table, and everything else in the file indexes it by AsagPart. Adding a
   part is a row here, a row in the enum, a <file> in disc.xml and a texture
   slot — no other code changes. */
typedef struct {
    const char *file;      /* \TEX\ 8.3, uppercase, ;1 — never the disc root  */
    int         tex_slot;  /* index into the ARENA's streamed texture table    */
} AsagPartDef;

/* >>> THE SKINS ARE STREAMED BY THE ROOM, NOT REGISTERED BY THE BOSS. <<<
   tools/ADDING_A_3D_ENEMY.txt STEP 3 says a textured enemy registers through
   texmgr. That is right for the Rabisu, whose room is walked into and out of
   from three directions. It is wrong here: texmgr keeps the whole TIM in main
   RAM for the life of the run, four skins is ~66 KB permanent, and this boss is
   reachable only through one one-way drop into a room that already streams its
   own art on that transition. So the four skins are rows 3..6 of
   asag_arena.c's stream_tex_file[] and this module reads their tpage/clut back
   through asag_arena_tex_slot(). Zero permanent bytes, and one CD read that
   was already happening. */
static const AsagPartDef part_def[ASAG_PART_COUNT] = {
    /* ASAG_PART_BOILS       */ { "\\TEXASAG\\ASGBOILS.SMD;1", ASAG_TEX_BOIL },
    /* ASAG_PART_BODY        */ { "\\TEXASAG\\ASGBODY.SMD;1",  ASAG_TEX_SKIN },
    /* ASAG_PART_LEAF_TOP    */ { "\\TEXASAG\\ASGLEAFT.SMD;1", ASAG_TEX_LEAF },
    /* ASAG_PART_LEAF_BOTTOM */ { "\\TEXASAG\\ASGLEAFB.SMD;1", ASAG_TEX_LEAF },
    /* ASAG_PART_LEAF_LEFT   */ { "\\TEXASAG\\ASGLEAFL.SMD;1", ASAG_TEX_LEAF },
    /* ASAG_PART_LEAF_RIGHT  */ { "\\TEXASAG\\ASGLEAFR.SMD;1", ASAG_TEX_LEAF },
    /* ASAG_PART_TENT_L      */ { "\\TEXASAG\\ASGTENTL.SMD;1", ASAG_TEX_TENT },
    /* ASAG_PART_TENT_R      */ { "\\TEXASAG\\ASGTENTR.SMD;1", ASAG_TEX_TENT },
};

/* ---- The clips -------------------------------------------------------------
   Which part each clip belongs to, and where it lives. The .pva files are baked
   by tools/export_asag.py at STEP 4; their headers carry the played rate (6)
   and the loop flag the exporter detected, but the loop the GAME uses is the
   `loop` argument to asag_play() — the header flag only records whether the
   exporter trimmed a duplicate end frame, which is a property of the art, not a
   decision about playback. */
typedef struct {
    AsagPart    part;
    const char *file;
} AsagClipDef;

static const AsagClipDef clip_def[ASAG_CLIP_COUNT] = {
    /* HEAD_IDLE    */ { ASAG_PART_BODY,        "\\TEXASAG\\ASGHIDLE.PVA;1" },
    /* HEAD_SPEAK   */ { ASAG_PART_BODY,        "\\TEXASAG\\ASGHSPK.PVA;1"  },
    /* HEAD_EMERGE  */ { ASAG_PART_BODY,        "\\TEXASAG\\ASGHEMRG.PVA;1" },
    /* HEAD_RETRACT */ { ASAG_PART_BODY,        "\\TEXASAG\\ASGHRETR.PVA;1" },
    /* HEAD_VOMIT   */ { ASAG_PART_BODY,        "\\TEXASAG\\ASGHVOM.PVA;1"  },
    /* HEAD_LASER   */ { ASAG_PART_BODY,        "\\TEXASAG\\ASGHLAS.PVA;1"  },
    /* LEAF_TOP     */ { ASAG_PART_LEAF_TOP,    "\\TEXASAG\\ASGLFTOP.PVA;1" },
    /* LEAF_BOTTOM  */ { ASAG_PART_LEAF_BOTTOM, "\\TEXASAG\\ASGLFBOT.PVA;1" },
    /* LEAF_LEFT    */ { ASAG_PART_LEAF_LEFT,   "\\TEXASAG\\ASGLFLFT.PVA;1" },
    /* LEAF_RIGHT   */ { ASAG_PART_LEAF_RIGHT,  "\\TEXASAG\\ASGLFRGT.PVA;1" },
    /* TENT_L_IDLE  */ { ASAG_PART_TENT_L,      "\\TEXASAG\\ASGTLIDL.PVA;1" },
    /* TENT_L_ATTACK*/ { ASAG_PART_TENT_L,      "\\TEXASAG\\ASGTLATK.PVA;1" },
    /* TENT_R_IDLE  */ { ASAG_PART_TENT_R,      "\\TEXASAG\\ASGTRIDL.PVA;1" },
    /* TENT_R_ATTACK*/ { ASAG_PART_TENT_R,      "\\TEXASAG\\ASGTRATK.PVA;1" },
};

#define PVA_HEADER_SIZE  12

/* ---- Loaded state — ALL OF IT FREED BY asags_free_model() ---------------- */
static void *part_buff[ASAG_PART_COUNT];
static SMD  *part_smd [ASAG_PART_COUNT];
static int16_t part_cx[ASAG_PART_COUNT];   /* mesh centre, for the per-part fog */
static int16_t part_cz[ASAG_PART_COUNT];

static void    *clip_buff  [ASAG_CLIP_COUNT];
static SVECTOR *clip_frames[ASAG_CLIP_COUNT];
static int      clip_count [ASAG_CLIP_COUNT];   /* 0 = rejected or absent */

/* ---- Playback state — survives a free/load, because it is a statement about
   the FIGHT and not about what is in memory. A director that started the
   emerge, then had the model dropped and reloaded, should find the emerge still
   running rather than silently back on the bind pose. ---------------------- */
static int8_t  cur_clip[ASAG_PART_COUNT];   /* AsagClip, or ASAG_CLIP_NONE   */
static int16_t anim_frame[ASAG_PART_COUNT];
static int8_t  anim_tick[ASAG_PART_COUNT];
static int8_t  clip_loop[ASAG_PART_COUNT];
static int8_t  clip_held[ASAG_PART_COUNT];  /* 1 = one-shot on its last frame */
static int8_t  part_vis[ASAG_PART_COUNT];

static int model_loaded = 0;

/* ---- read_file, WITH A STACK GUARD ----------------------------------------
   rabisu.c's, plus the check that would have turned this boss's first build
   from a crash into a missing model.

   >>> THE HEAP TOP IS THE STACK, AND malloc DOES NOT KNOW THAT. <<<
   PSn00bSDK's InitHeap hands the heap every byte from _end to 0x801FFFF8, and
   the stack grows DOWN into the same region with nothing between them. So when
   the heap fills, malloc does not fail - it SUCCEEDS and returns memory the
   stack is already using, and the next CdRead DMAs straight through a saved
   return address. tools/HEAP_BUDGET.txt describes the mechanism; this boss
   demonstrated it. Its first build asked for 229 KB of meshes and clips against
   what tools/heap_budget.py reported as 371 KB free, and the console died every
   single time inside CdReadSync:

       buffer 0x801D46D0..0x801DE6D0   sp = 0x801DBD40   <- sp is INSIDE it
       "Attempted unaligned JR to 0xfe51ffbd from 0x8008D9A0"

   The report was wrong by 145 KB: main() declares its RenderContext as a LOCAL
   (two 64 KB packet buffers and two 8 KB ordering tables), so the top of RAM is
   permanently stack and was being counted as heap. Real free at the arena door
   is ~171 KB. The script is fixed and the clips are baked to fit.

   THIS CHECK IS THE BACKSTOP FOR THE NEXT TIME. Reading $sp gives the true
   ceiling at this exact call site, which is deeper in the call chain than any
   budget script can model. A refused read leaves the part or clip absent, which
   draws a bind pose or nothing at all - visible, harmless and obvious - instead
   of corrupting the return address of whatever called us.

   The margin is for the frames BELOW us: CdRead, CdReadSync and the interrupt
   handler all push after this point. 8 KB is far more than they use and costs
   nothing, since the whole point is to fail before touching the stack at all. */
#define ASAG_STACK_MARGIN 8192

static void *read_file(const char *name) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)name)) return NULL;
    int sectors = (file.size + 2047) / 2048;
    void *buf = malloc(sectors * 2048);
    if (!buf) return NULL;

    uint32_t sp;
    __asm__ volatile("move %0, $sp" : "=r"(sp));
    if ((uint32_t)buf + (uint32_t)(sectors * 2048) + ASAG_STACK_MARGIN > sp) {
        free(buf);
        return NULL;          /* out of heap: this part/clip simply will not exist */
    }

    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);
    return buf;
}

/* >>> THE CHECK THAT KEEPS A STALE CLIP FROM DRAWING GARBAGE. <<< Positions are
   indexed by the .smd's polygon indices, so a clip baked from a mesh with a
   different vertex count reads off the end of every frame. Refuse it and the
   part falls back to its bind pose, which always renders — a motionless boss
   instead of an exploded one. Mistake 5 in tools/ANIMATING_A_3D_MODEL.txt. */
static void load_clip(int c) {
    SMD *smd = part_smd[clip_def[c].part];
    if (!smd) return;
    uint8_t *p = (uint8_t *)read_file(clip_def[c].file);
    if (!p) return;
    if (p[0] != 'P' || p[1] != 'V' || p[2] != 'A' || p[3] != '1') { free(p); return; }
    int n_verts  = p[4] | (p[5] << 8);
    int n_frames = p[6] | (p[7] << 8);
    if (n_verts != smd->n_verts || n_frames <= 0) { free(p); return; }
    clip_buff[c]   = p;
    clip_frames[c] = (SVECTOR *)(p + PVA_HEADER_SIZE);
    clip_count[c]  = n_frames;
}

static void load_part(int i) {
    part_buff[i] = read_file(part_def[i].file);
    part_smd[i]  = part_buff[i] ? smdInitData(part_buff[i]) : NULL;
    if (!part_smd[i]) return;

    /* The mesh's XZ centre, taken ONCE. The fog is computed per part rather
       than per polygon (tools/ADDING_A_3D_ENEMY.txt STEP 5): the gradient
       across a body this size is invisible and the divides are not free. The
       bind pose is close enough for a part that never leaves its own corner of
       the arena, and re-deriving it per frame from the clip would spend the
       saving it exists to make. */
    int32_t mnx = 32767, mxx = -32768, mnz = 32767, mxz = -32768;
    for (int v = 0; v < part_smd[i]->n_verts; v++) {
        int32_t x = part_smd[i]->p_verts[v].vx;
        int32_t z = part_smd[i]->p_verts[v].vz;
        if (x < mnx) mnx = x;
        if (x > mxx) mxx = x;
        if (z < mnz) mnz = z;
        if (z > mxz) mxz = z;
    }
    part_cx[i] = (int16_t)((mnx + mxx) / 2);
    part_cz[i] = (int16_t)((mnz + mxz) / 2);
}

void asags_load_model(void) {
    if (model_loaded) return;              /* already in */

    /* THE CD BRACKET IS MANDATORY. This is a mid-game read, and a data read
       issued while CD-DA streams HANGS THE DRIVE (tools/TEXTURE_STREAMING_DEBUG
       .txt). suspend/resume are no-ops when nothing is playing; the case they
       exist for is a debug level-select jump that arrives with a track running.
       Twenty-two reads under ONE bracket, not twenty-two brackets. */
    cdaudio_suspend();
    for (int i = 0; i < ASAG_PART_COUNT; i++) load_part(i);
    for (int c = 0; c < ASAG_CLIP_COUNT; c++) load_clip(c);
    cdaudio_resume();

    model_loaded = 1;
}

void asags_free_model(void) {
    for (int i = 0; i < ASAG_PART_COUNT; i++) {
        if (part_buff[i]) { free(part_buff[i]); part_buff[i] = NULL; }
        part_smd[i] = NULL;               /* the pointer the draw tests */
    }
    for (int c = 0; c < ASAG_CLIP_COUNT; c++) {
        if (clip_buff[c]) { free(clip_buff[c]); clip_buff[c] = NULL; }
        clip_frames[c] = NULL;
        clip_count[c]  = 0;
    }
    model_loaded = 0;
}

int asag_model_loaded(void) { return model_loaded; }

/* ---- The clip API ------------------------------------------------------- */

void asag_play(AsagClip clip, int loop) {
    if (clip < 0 || clip >= ASAG_CLIP_COUNT) return;
    AsagPart p = clip_def[clip].part;
    cur_clip[p]   = (int8_t)clip;
    anim_frame[p] = 0;
    anim_tick[p]  = 0;
    clip_loop[p]  = loop ? 1 : 0;
    clip_held[p]  = 0;
}

void asag_stop(AsagPart part) {
    if (part < 0 || part >= ASAG_PART_COUNT) return;
    cur_clip[part]   = ASAG_CLIP_NONE;
    anim_frame[part] = 0;
    anim_tick[part]  = 0;
    clip_held[part]  = 0;
}

void asag_stop_all(void) {
    for (int i = 0; i < ASAG_PART_COUNT; i++) asag_stop((AsagPart)i);
}

AsagClip asag_playing(AsagPart part) {
    if (part < 0 || part >= ASAG_PART_COUNT) return ASAG_CLIP_NONE;
    return (AsagClip)cur_clip[part];
}

int asag_clip_done(AsagPart part) {
    if (part < 0 || part >= ASAG_PART_COUNT) return 0;
    return clip_held[part];
}

void asag_set_visible(AsagPart part, int visible) {
    if (part < 0 || part >= ASAG_PART_COUNT) return;
    part_vis[part] = visible ? 1 : 0;
}

int asag_visible(AsagPart part) {
    if (part < 0 || part >= ASAG_PART_COUNT) return 0;
    return part_vis[part];
}

/* One clock per part, so two parts mid-different-clips do not step together and
   a part on its bind pose costs one compare. Snapping between baked frames with
   no interpolation, exactly as the Rabisu does — it costs nothing and the
   stepping is period-correct. */
void asag_update(void) {
    for (int i = 0; i < ASAG_PART_COUNT; i++) {
        int c = cur_clip[i];
        if (c == ASAG_CLIP_NONE) continue;
        int n = clip_count[c];
        if (n <= 0) continue;             /* clip absent or rejected */
        if (clip_held[i]) continue;       /* one-shot, holding its last frame */

        if (++anim_tick[i] < ASAG_ANIM_TICKS) continue;
        anim_tick[i] = 0;

        if (++anim_frame[i] >= n) {
            if (clip_loop[i]) {
                anim_frame[i] = 0;
            } else {
                /* HOLD the last frame rather than snapping back to the bind
                   pose. That is what lets HEAD_RETRACT leave the head down and
                   HEAD_EMERGE leave it up, and it is why asag_clip_done() is a
                   separate question from "is a clip set". */
                anim_frame[i] = (int16_t)(n - 1);
                clip_held[i]  = 1;
            }
        }
    }
}

/* The vertex block a part is posed on this frame. Falls back to the .smd's own
   bind pose whenever the clip is missing or was rejected — which is also the
   state every part is in until the director starts something. */
static SVECTOR *part_verts(int i) {
    int c = cur_clip[i];
    if (c == ASAG_CLIP_NONE || !clip_frames[c] || clip_count[c] <= 0)
        return part_smd[i]->p_verts;
    int f = anim_frame[i];
    if (f < 0 || f >= clip_count[c]) f = 0;
    return clip_frames[c] + (f * part_smd[i]->n_verts);
}

/* ---- The draw --------------------------------------------------------------
   asag_arena.c's draw_asag_arena_smd() with three differences, and NOTHING
   else:

     - the vertices come from part_verts(), not from the .smd, so a clip poses
       the same prim stream the bind pose does;
     - the fog is computed ONCE PER PART rather than per polygon;
     - the side-plane frustum cull is GONE. It exists in the room's loop to
       reject most of a 674-primitive mesh; the largest part here is 75 prims,
       so the test would cost more than it saves.

   >>> NO MATRIX IS LOADED AND NONE IS RESTORED. <<< Every part's vertices are
   already in the arena's coordinate space (asag.h), so this draws under the
   view asag_arena_draw() set with camera_build_view() and leaves the GTE
   exactly as it found it. That is why there is no early-out path here that has
   to remember to put a matrix back — the trap
   tools/ADDING_A_3D_ENEMY.txt STEP 5 is about does not exist in this shape. */
static void draw_part(RenderContext *ctx, int i) {
    SMD *smd = part_smd[i];
    if (!smd || !part_vis[i]) return;

    SVECTOR *vp = part_verts(i);
    uint8_t *p  = (uint8_t *)smd->p_prims;
    int n = smd->n_prims;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = ASAG_CULL_DIST;

    /* Whole-part distance reject and whole-part fog, both from the mesh centre
       taken at load. */
    int32_t pdx  = (int32_t)part_cx[i] - cam_x;
    int32_t pdz  = (int32_t)part_cz[i] - cam_z;
    int32_t pdst = (pdx < 0 ? -pdx : pdx) + (pdz < 0 ? -pdz : pdz);
    if (pdst > cull) return;

    int32_t fog = pdst < g_fog_near ? g_fog_near
                                    : (pdst > g_fog_far ? g_fog_far : pdst);
    int32_t ff  = ((g_fog_far - fog) << 8) / (g_fog_far - g_fog_near);

    uint16_t tpage = asag_arena_tex_page(part_def[i].tex_slot);
    uint16_t clut  = asag_arena_tex_clut(part_def[i].tex_slot);

    for (int k = 0; k < n; k++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);
        int stride  = pt->len;

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

        /* The GTE clamps screen coordinates to +/-1023 and a primitive with a
           corner past it comes out folded. Drop the whole thing. */
        if (sv[0].vx <= -1023 || sv[0].vx >= 1023 || sv[0].vy <= -1023 || sv[0].vy >= 1023 ||
            sv[1].vx <= -1023 || sv[1].vx >= 1023 || sv[1].vy <= -1023 || sv[1].vy >= 1023 ||
            sv[2].vx <= -1023 || sv[2].vx >= 1023 || sv[2].vy <= -1023 || sv[2].vy >= 1023) {
            p += stride; continue;
        }

        /* KEEP THE BACKFACE CULL. A solid model genuinely has back faces and
           culling them halves what reaches the GPU; the rolling-sprite argument
           in ADDING_AN_ENEMY.txt's mistake 4 does not transfer. Every part was
           verified with tools/check_model_winding.py — six boils and the right
           tentacle shipped inside-out and were flipped at source. The SMD's own
           nocull bit still exempts whatever asks to be exempt. */
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
           behind whatever stands on it — the leaves have plenty. */
        if (poly_is_flat_y(v0, v1, v2, v3))
            otz = is_quad ? otz_far4(sz[1], sz[2], v2_sz, sz[3])
                          : otz_far3(sz[1], sz[2], sz[3]);
        if (otz <= 0) { p += stride; continue; }

        /* +38, TWO NEARER THAN THE ROOM'S +40. The arena's own mesh biases its
           primitives back by 40 so its floor does not fight what stands on it;
           Asag is rooted IN the back wall and its leaves sit millimetres off the
           alcove's surfaces, so at the same bias the two meshes tie and the wall
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

void asag_draw(RenderContext *ctx) {
    if (!model_loaded) return;
    for (int i = 0; i < ASAG_PART_COUNT; i++) draw_part(ctx, i);
}

/* Every part starts VISIBLE and on its bind pose. This runs from
   asag_arena_init(), i.e. on every arrival, so a debug jump into the room finds
   the same state a real drop does. */
void asag_reset(void) {
    for (int i = 0; i < ASAG_PART_COUNT; i++) {
        part_vis[i] = 1;
        cur_clip[i] = ASAG_CLIP_NONE;
        anim_frame[i] = 0;
        anim_tick[i]  = 0;
        clip_loop[i]  = 0;
        clip_held[i]  = 0;
    }
}
