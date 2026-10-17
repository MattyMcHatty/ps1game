#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "room_arena.h"
#include "cull_arena.h"
#include "tim_slots.h"
#include "texmgr.h"
#include "camera.h"
#include "collision.h"
#include "room_data.h"
#include "cdaudio.h"
#include "intro.h"              /* the fade-able 5x7 font - the lines          */
#include "trial_end.h"          /* where the white goes: the sign-off          */
#include "descent_to_hell.h"
#include "descent_to_hell_tex_map.h"

/* Descent to Hell - see descent_to_hell.h. The load/upload/cull-key half is
   src/seal.c's; the draw is NOT the shared room loop, because nothing about
   this room is a room the player stands in. See "The box" below. */

static SMD  *dh_smd  = NULL;
static void *dh_buff = NULL;

/* ---- Textures --------------------------------------------------------------
   TWO, BOTH OWNED, BOTH 4bpp 128x128:
     0  lava     x576 y0, Lamashtu's CLUT line (640,502) - the Seal's slot and
                 palette exactly. The only way in is the Seal, the Seal puts its
                 own back on entry, and so does every other owner of that page.
     1  hell bg  x640 y0, the gaol door's CLUT line (576,502). Every room that
                 draws x640 y0 uploads its own pixels on entry; the gaol rooms
                 upload the gaol door's palette with them.
   The tube is drawn with slot 0 as well. */
#define DH_TEX_COUNT 2

static uint16_t tex_tpage[DH_TEX_COUNT];
static uint16_t tex_clut[DH_TEX_COUNT];
static int      lava_tex   = -1;
static int      hellbg_tex = -1;

void descent_to_hell_load_assets(void) {
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    lava_tex   = texmgr_register("\\TEXUNDW\\LAVA.TIM;1");
    hellbg_tex = texmgr_register("\\TEXUNDW\\HELLBG.TIM;1");
    TIM_SLOT(0, LAVA);
    TIM_SLOT(1, HELLBG);
}

/* Pure LoadImage from the RAM copies texmgr holds - no CD access, safe during
   the transition (main's STATE_LOADING DrawSyncs first). */
void descent_to_hell_upload_textures(void) {
    if (lava_tex   >= 0) texmgr_upload(lava_tex);
    if (hellbg_tex >= 0) texmgr_upload(hellbg_tex);
}

/* ---- The cull key (STEP 3B, tools/DIAGNOSING_FRAME_RATE.txt) ---------------
   Copied from src/seal.c. Built on the same call that reloads the mesh it
   describes, and nowhere else. The box's draw reads only the strides from it
   (see "The box"), but the table is the arena's contract with every room. */
static int dh_key_count = 0;

static void dh_build_cull_keys(void) {
    dh_key_count = 0;
    if (!dh_smd) return;
    uint8_t *p = (uint8_t *)dh_smd->p_prims;
    int i, n = dh_smd->n_prims;
    if (n > DESCENT_TO_HELL_PRIM_COUNT) n = DESCENT_TO_HELL_PRIM_COUNT;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &dh_smd->p_verts[vi[0]];
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        cull_keys[i].pad    = 0;
        p += pt->len;
    }
    dh_key_count = n;
}

/* ---- The tube --------------------------------------------------------------
   Malloc'd on room entry, freed by the next transition (main.c's
   load_area_geometry calls descent_to_hell_unload_tube on every one, as it
   does catacomb_doors_unload) and by descent_to_hell_reset, because the way
   out of this room is the title, and New Game enters the Delivery Area
   without passing load_area_geometry. */
static void *tube_buff = NULL;
static SMD  *tube_smd  = NULL;

/* Each tube quad's DEEPEST point (largest y), built on load: the draw's cull
   (see draw_tube). 360 quads as modelled; a re-export past the cap draws its
   extra quads uncull'd rather than not at all. */
#define DH_TUBE_KEYS 400
static int16_t tube_ymax[DH_TUBE_KEYS];
static int     tube_key_count = 0;

static void dh_build_tube_keys(void) {
    tube_key_count = 0;
    if (!tube_smd) return;
    uint8_t *p = (uint8_t *)tube_smd->p_prims;
    int i, k, n = tube_smd->n_prims;
    if (n > DH_TUBE_KEYS) n = DH_TUBE_KEYS;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        int16_t       ym = -32768;
        for (k = 0; k < (pt->type >= 2 ? 4 : 3); k++)
            if (tube_smd->p_verts[vi[k]].vy > ym) ym = tube_smd->p_verts[vi[k]].vy;
        tube_ymax[i] = ym;
        p += pt->len;
    }
    tube_key_count = n;
}

static void *dh_read_file(const char *name) {
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

void descent_to_hell_unload_tube(void) {
    if (tube_buff) free(tube_buff);
    tube_buff      = NULL;
    tube_smd       = NULL;
    tube_key_count = 0;
}

void descent_to_hell_load_geometry(void) {
    dh_buff = room_arena_load("\\TEXUNDW\\DESCENT.SMD;1");
    dh_smd  = dh_buff ? smdInitData(dh_buff) : NULL;
    /* ...and the tex map, no-cull bits and (zero) walls packed onto the end of
       the same file (src/room_data.h). No block, no room. */
    if (dh_smd && !room_data_bind(dh_smd->n_prims)) dh_smd = NULL;
    dh_build_cull_keys();

    /* THE TUBE. room_arena_load brackets its own read and resumes CD-DA on the
       way out, so this one needs its own bracket (catacomb_doors_load's note).
       Nothing is playing on the way in from the Seal, but a debug jump may
       arrive with a track running. */
    descent_to_hell_unload_tube();
    cdaudio_suspend();
    tube_buff = dh_read_file("\\TEXUNDW\\DSCTUBE.SMD;1");
    tube_smd  = tube_buff ? smdInitData(tube_buff) : NULL;
    cdaudio_resume();
    dh_build_tube_keys();
}

/* ---- The clock -------------------------------------------------------------
   Frames since the room was built. Every beat is a fixed time on it, and the
   lines are 130 frames apart, the last held 4 s in full white. */
#define DH_T_FADE_IN      120   /* 2 s up out of the Seal's white              */
#define DH_T_MOUTH        740   /* the camera reaches the tube's mouth, as
                                   DEATH AWAITS YOU... comes up               */
#define DH_T_WHITE_FROM   870   /* the fade to white, the last line up...      */
#define DH_T_WHITE_TO     970   /* ...full white; the fall stops here          */
#define DH_T_END         1210   /* the last line 4 s in the white, then the
                                   sign-off                                    */

typedef enum {
    DH_OFF = 0,
    DH_FALL,      /* the fade in, the fall, the lines, the fade to white        */
    DH_WHITE,     /* held white; trial_end has the screen                       */
} DhState;

static DhState state = DH_OFF;
static int32_t dh_t  = 0;
static int32_t spin  = 0;      /* the tube's turn, 4096 units                  */

/* ---- The fall --------------------------------------------------------------
   Down the room's centre line, which is the tube's axis, at ONE STEADY SPEED
   from the first frame to full white: from just under the tube's top rim
   (y=-6000) to its mouth (y=0) at DH_T_MOUTH, and on at the same speed through
   the last line and the fade to white. At full white it is ~1850 down, about
   150 above the lava, and it stops there for the held white.

   LOOKING STRAIGHT DOWN: pitch 1024 is a quarter turn below level
   (camera_build_view rotates about X by cam_pitch, and +Y is down). The yaw is
   irrelevant to a camera looking along the axis; it is held at 0. */
#define DH_CAM_Y0      (-5950)
#define DH_MOUTH_Y_CAM    0
#define DH_PITCH_DOWN  1024

/* The tube's spin, 4096 units a frame: ~1.4 turns a second. Twelve sides means
   the pattern repeats every 341 units, and 48 is well clear of any multiple of
   it, so it reads as turning rather than strobing. */
#define DH_SPIN          48

/* >>> THE HOLD IS AN EARLY RETURN, NOT A CLAMP. <<< A clamp on the clock
   ahead of the multiply is the shape that compiled to a MULT in a branch
   delay slot followed straight away by a second MULT, which PCSX-Redux reads
   the wrong LO back for: the eased version of this function put the camera a
   kilometre under the lava from the first frame its clamp applied (memory:
   clamp-before-divide). Past DH_T_WHITE_TO the camera is simply left where the
   last moving frame put it. */
static void dh_place_camera(void) {
    if (dh_t > DH_T_WHITE_TO) return;
    cam_x     = 0;
    cam_z     = 0;
    cam_y     = DH_CAM_Y0 + ((DH_MOUTH_Y_CAM - DH_CAM_Y0) * dh_t) / DH_T_MOUTH;
    cam_vy    = 0;
    cam_rot   = 0;
    cam_pitch = DH_PITCH_DOWN;
}

void descent_to_hell_reset(void) {
    state = DH_OFF;
    dh_t  = 0;
    spin  = 0;
    descent_to_hell_unload_tube();
}

void descent_to_hell_init(void) {
    /* Zero walls; the call still sets the room's bounds and empties whatever
       wall list the Seal left. */
    room_data_collision(&current_collision_room);
    floor_zone_count = 0;

    camera_look_cancel();
    dh_t  = 0;
    spin  = 0;
    state = DH_FALL;
    dh_place_camera();
}

void descent_to_hell_update(void) {
    if (state != DH_FALL) return;   /* DH_WHITE: trial_end has it from here */

    dh_t++;
    spin = (spin + DH_SPIN) & 4095;
    dh_place_camera();

    if (dh_t >= DH_T_END) {
        /* THE END OF THE BUILD: the thank-you screen, out of full white. It
           dissolves this room - still drawn, frozen - to its purple, and owns
           the frame from here (main.c's trial_end branch). */
        trial_end_start();
        state = DH_WHITE;
    }
}

/* ---- The lines -------------------------------------------------------------
   One at a time, each at its own place: a quick fade in as the one before
   fades out over the same frames, so there is never a gap and never two at
   full. The last arrives as the white starts to come up, and stays.

   The bosses' lines are the SDK font, which draws at one brightness and cannot
   fade, so these are the opening sequence's font (intro.h) - the same white,
   drawn SEMI-TRANSPARENT over an additive DR_TPAGE, which on the PS1 is what a
   fade over a moving picture is: the text's colour, scaled by the fade, ADDED
   to whatever is under it. At full it saturates to white over anything.

   Positions keep 20px in from every edge, out of the overscan. The longest
   line ("THE CURSE OF SHARUR...") is ~210px at the font's advances. */
typedef enum { POS_TL, POS_TR, POS_L, POS_C, POS_R, POS_BL, POS_BR } DhPos;

typedef struct {
    const char *text;
    DhPos       pos;
    int16_t     start;   /* frame it begins to fade in */
} DhLine;

#define DH_LINE_COUNT  7
#define DH_LINE_FADE  12    /* a quick fade: a fifth of a second each way */

static const DhLine LINES[DH_LINE_COUNT] = {
    { "I REMEMBER THAT FEELING...",                POS_TL,  90 },
    { "MY CLIPPED WINGS HAVE BEEN RESTORED...",    POS_BR, 220 },
    { "YOUR ORDER HAS FAILED...",                  POS_L,  350 },
    { "THE STORM IS COMING...",                    POS_TR, 480 },
    { "THE CURSE OF SHARUR HAS BEEN EXPELLED...",  POS_R,  610 },
    { "DEATH AWAITS YOU...",                       POS_BL, 740 },
    { "DEATH AWAITS YOU ALL!",                     POS_C,  870 },
};

#define DH_MARGIN_X   20
#define DH_TEXT_TOP   20
#define DH_TEXT_MID  (SCREEN_YRES / 2 - 4)
#define DH_TEXT_BOT  (SCREEN_YRES - 20 - 7)

/* The opening's bone white, and the deep red the last line turns. */
#define DH_TEXT_R  235
#define DH_TEXT_G  230
#define DH_TEXT_B  215
#define DH_RED_R   150
#define DH_RED_G     0
#define DH_RED_B     0

/* OT slot 2: in front of the scene, behind trial_end's wash at ot[1], so the
   sign-off dissolves the lines away with the white. Sorted BEFORE the white's
   tile in the same slot: LIFO, so the text is drawn after the white, never
   under it (src/seal.c's line, the same note). */
#define DH_OT_TEXT   2
#define DH_OT_WHITE  2

static int32_t line_level(int i) {
    int32_t a = dh_t - LINES[i].start;
    if (a <= 0) return 0;
    int32_t v = (a * 255) / DH_LINE_FADE;
    if (v > 255) v = 255;
    if (i + 1 < DH_LINE_COUNT) {
        int32_t b = dh_t - LINES[i + 1].start;
        if (b > 0) {
            int32_t f = 255 - (b * 255) / DH_LINE_FADE;
            if (f < 0) f = 0;
            if (f < v) v = f;
        }
    }
    return v;
}

static void line_xy(const DhLine *L, int *x, int *y) {
    int w = intro_text_width(L->text);
    switch (L->pos) {
    case POS_TL: *x = DH_MARGIN_X;                     *y = DH_TEXT_TOP; break;
    case POS_TR: *x = SCREEN_XRES - DH_MARGIN_X - w;   *y = DH_TEXT_TOP; break;
    case POS_L:  *x = DH_MARGIN_X;                     *y = DH_TEXT_MID; break;
    case POS_R:  *x = SCREEN_XRES - DH_MARGIN_X - w;   *y = DH_TEXT_MID; break;
    case POS_BL: *x = DH_MARGIN_X;                     *y = DH_TEXT_BOT; break;
    case POS_BR: *x = SCREEN_XRES - DH_MARGIN_X - w;   *y = DH_TEXT_BOT; break;
    default:     *x = (SCREEN_XRES - w) / 2;           *y = DH_TEXT_MID; break;
    }
}

/* An additive DR_TPAGE into `slot`, AFTER the semi-trans prims it governs: an
   OT node is LIFO, so the GPU sees the blend mode first. */
static void add_additive_tpage(RenderContext *ctx, int slot) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(DR_TPAGE) > buf_end) return;
    DR_TPAGE *dp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(dp, 0, 0, getTPage(0, 1 /* ABR=1: B + F */, 0, 0));
    addPrim(&ctx->buffers[ctx->active_buffer].ot[slot], dp);
    ctx->next_packet += sizeof(DR_TPAGE);
}

/* How red the last line is, 0..256: it follows the white up. */
static int32_t last_line_red(void) {
    if (state == DH_WHITE) return 256;
    int32_t a = dh_t - DH_T_WHITE_FROM;
    if (a <= 0) return 0;
    if (a >= DH_T_WHITE_TO - DH_T_WHITE_FROM) return 256;
    return (a * 256) / (DH_T_WHITE_TO - DH_T_WHITE_FROM);
}

static void draw_lines(RenderContext *ctx) {
    int i, any_semi = 0;
    for (i = 0; i < DH_LINE_COUNT; i++) {
        int32_t lv = (state == DH_WHITE && i == DH_LINE_COUNT - 1) ? 255
                                                                  : line_level(i);
        if (lv <= 0) continue;
        int x, y;
        line_xy(&LINES[i], &x, &y);

        int32_t red = (i == DH_LINE_COUNT - 1) ? last_line_red() : 0;
        if (red > 0) {
            /* OPAQUE once the white starts to rise: additive red on white
               would be white. At red 0 the two are the same picture (full
               additive white saturates to white), so the switch is seamless. */
            int32_t k = 256 - red;
            intro_text_draw_at(ctx, LINES[i].text, x, y,
                (uint8_t)((DH_TEXT_R * k + DH_RED_R * red) >> 8),
                (uint8_t)((DH_TEXT_G * k + DH_RED_G * red) >> 8),
                (uint8_t)((DH_TEXT_B * k + DH_RED_B * red) >> 8),
                DH_OT_TEXT, 0);
        } else {
            intro_text_draw_at(ctx, LINES[i].text, x, y,
                               (uint8_t)((DH_TEXT_R * lv) / 255),
                               (uint8_t)((DH_TEXT_G * lv) / 255),
                               (uint8_t)((DH_TEXT_B * lv) / 255),
                               DH_OT_TEXT, 1);
            any_semi = 1;
        }
    }
    if (any_semi) add_additive_tpage(ctx, DH_OT_TEXT);
}

/* ---- The white -------------------------------------------------------------
   src/seal.c's draw_white: an ADDITIVE full-screen tile at ot[2], which
   trial_end's wash at ot[1] subtracts back down and dissolves to its purple.
   Up out of the Seal's white at the start, back to white at the end. */
static int32_t white_level(void) {
    if (state == DH_WHITE) return 255;
    if (state != DH_FALL) return 0;
    if (dh_t < DH_T_FADE_IN)
        return 255 - (dh_t * 255) / DH_T_FADE_IN;
    if (dh_t >= DH_T_WHITE_FROM) {
        int32_t k = ((dh_t - DH_T_WHITE_FROM) * 255) /
                    (DH_T_WHITE_TO - DH_T_WHITE_FROM);
        return k > 255 ? 255 : k;
    }
    return 0;
}

static void draw_white(RenderContext *ctx, int32_t k) {
    if (k <= 0) return;
    if (k > 255) k = 255;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(TILE) > buf_end) return;

    TILE *tl = (TILE *)ctx->next_packet;
    setTile(tl);
    setSemiTrans(tl, 1);
    setRGB0(tl, (uint8_t)k, (uint8_t)k, (uint8_t)k);
    setXY0(tl, 0, 0);
    setWH(tl, SCREEN_XRES, SCREEN_YRES);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[DH_OT_WHITE], tl);
    ctx->next_packet += sizeof(TILE);

    add_additive_tpage(ctx, DH_OT_WHITE);
}

/* ---- Shared poly emit -------------------------------------------------------
   One textured quad from four projected corners. */
static int otz_of(int32_t s0, int32_t s1, int32_t s2, int32_t s3) {
    int32_t otz = (s0 + s1 + s2 + s3) >> 4;   /* AVSZ4 with ZSF4=256 */
    if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;
    return (int)otz;
}

static int on_screen(const DVECTOR *v) {
    return v->vx > -1023 && v->vx < 1023 && v->vy > -1023 && v->vy < 1023;
}

/* ---- The box ---------------------------------------------------------------
   >>> NOT THE SHARED ROOM DRAW LOOP, AND IT CANNOT BE. <<< The box is five
   quads: a 3600-square floor and four walls 3600 x 9000. Drawn as five
   primitives they vanish - with the camera inside the box, every wall has
   corners BEHIND the camera (sz 0, rejected) and the floor, from 400 above
   it, projects its corners thousands of pixels off screen (past the GTE's
   +-1023 and the GPU's 1023 x 511 primitive limit, rejected). The room loops'
   own culls would not help either: the behind-the-yaw test throws away
   anything "behind" a camera that is looking straight DOWN
   (memory: overhead-camera behind-cull).

   So each quad is cut into a grid of sub-quads no more than DH_SUB_LEN on a
   side, its UVs interpolated across it, and each corner of the grid projected
   once. A sub-quad is drawn only if all four of its corners are in front of
   the camera and inside +-1023; the ones that fail are, by construction, off
   screen. The lava floor's UVs run 15..255 and rely on the 128 texture window
   to tile, which interpolates fine.

   >>> AND ONLY THE BAND THAT CAN BE SEEN IS CUT AT ALL. <<< Tessellating the
   whole box cost 272 hblanks a frame - more than a frame on its own - and
   nearly all of it was wall nobody could see. visible_from_y() works out, per
   quad, the shallowest y any of it can be on screen at, and the walls are
   clipped to the band below that before they are cut:
     - nothing above the camera, which looks straight down;
     - nothing above the tube's mouth while the camera is still in the tube:
       the shaft is a closed cylinder round the camera, so it hides all of it;
     - for an axis-aligned wall, nothing nearer the camera than the depth at
       which that wall comes inside the screen's corner (DH_SCREEN_REACH px
       out at the projection's 256): 1800 from the axis, that is 2304 deep.
   Together those leave the walls on screen for about half a second, at the
   mouth. The grid is stepped incrementally per row, not divided per corner.

   NO FOG, NO DISTANCE CULL: the lava is the light at the end of the tube. */
#define DH_SUB_LEN       400
#define DH_SUB_MAX        16    /* 289 grid corners: 3.2 KB of BSS */
#define DH_NEAR           32    /* corners nearer than this are not drawn */
#define DH_MOUTH_Y         0    /* the tube's open bottom (descent_tube.smd) */
#define DH_PROJ_H        256    /* gte_SetGeomScreen */
#define DH_SCREEN_REACH  200    /* px from the screen's centre to its corner */

static DVECTOR g_sv[(DH_SUB_MAX + 1) * (DH_SUB_MAX + 1)];
static int32_t g_sz[(DH_SUB_MAX + 1) * (DH_SUB_MAX + 1)];
static uint8_t g_u [(DH_SUB_MAX + 1) * (DH_SUB_MAX + 1)];
static uint8_t g_v [(DH_SUB_MAX + 1) * (DH_SUB_MAX + 1)];
static uint8_t g_ok[(DH_SUB_MAX + 1) * (DH_SUB_MAX + 1)];

static int32_t span(const SVECTOR *a, const SVECTOR *b) {
    int32_t dx = b->vx - a->vx, dy = b->vy - a->vy, dz = b->vz - a->vz;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dz < 0) dz = -dz;
    int32_t m = dx > dy ? dx : dy;
    return m > dz ? m : dz;
}

static int subdivs(int32_t len) {
    int n = (int)((len + DH_SUB_LEN - 1) / DH_SUB_LEN);
    if (n < 1) n = 1;
    if (n > DH_SUB_MAX) n = DH_SUB_MAX;
    return n;
}

/* The shallowest y at which any of quad q can be on screen (see "The box"). */
static int32_t visible_from_y(SVECTOR *const *q) {
    int32_t lim = cam_y + DH_NEAR;
    if (tube_smd && cam_y < DH_MOUTH_Y && lim < DH_MOUTH_Y) lim = DH_MOUTH_Y;

    int32_t hd = -1;
    if (q[0]->vx == q[1]->vx && q[0]->vx == q[2]->vx && q[0]->vx == q[3]->vx)
        hd = q[0]->vx - cam_x;
    else if (q[0]->vz == q[1]->vz && q[0]->vz == q[2]->vz && q[0]->vz == q[3]->vz)
        hd = q[0]->vz - cam_z;
    if (hd != -1) {
        if (hd < 0) hd = -hd;
        int32_t reach = cam_y + (hd * DH_PROJ_H) / DH_SCREEN_REACH;
        if (reach > lim) lim = reach;
    }
    return lim;
}

static void draw_box_quad(RenderContext *ctx, uint8_t *p, uint8_t *buf_end,
                          int tex) {
    uint16_t *vi = (uint16_t *)(p + 4);
    SVECTOR *q[4];
    int k;
    for (k = 0; k < 4; k++) q[k] = &dh_smd->p_verts[vi[k]];
    uint8_t *uv  = p + 20;
    uint8_t *col = p + 16;

    /* The visible stretch of the quad along its v0->v2 edge, as t0..t1 in
       4096ths. Only quads whose two rows are each level (v0/v1 and v2/v3 at
       one y apiece - every quad in this mesh) are clipped; anything else is
       cut whole. */
    int32_t lim = visible_from_y(q);
    int32_t t0 = 0, t1 = 4096;
    if (q[0]->vy == q[1]->vy && q[2]->vy == q[3]->vy) {
        int32_t ya = q[0]->vy, yb = q[2]->vy;
        if (ya < lim && yb < lim) return;                 /* none of it */
        if (ya != yb) {
            int32_t tc = ((lim - ya) * 4096) / (yb - ya);
            if (ya < lim) t0 = tc;                        /* top is hidden    */
            else if (yb < lim) t1 = tc;                   /* bottom is hidden */
        }
    }

    int nu  = subdivs(span(q[0], q[1]));
    int nv  = subdivs((span(q[0], q[2]) * (t1 - t0)) >> 12);
    int row = nu + 1;
    int s, t;

    /* Project the grid, a row at a time: each row's two ends are interpolated
       down the quad's sides, then the corners are stepped across in 16.16. */
    for (t = 0; t <= nv; t++) {
        int32_t f = t0 + ((t1 - t0) * t) / nv;            /* 0..4096 */
        int32_t lx = q[0]->vx + (((q[2]->vx - q[0]->vx) * f) >> 12);
        int32_t ly = q[0]->vy + (((q[2]->vy - q[0]->vy) * f) >> 12);
        int32_t lz = q[0]->vz + (((q[2]->vz - q[0]->vz) * f) >> 12);
        int32_t rx = q[1]->vx + (((q[3]->vx - q[1]->vx) * f) >> 12);
        int32_t ry = q[1]->vy + (((q[3]->vy - q[1]->vy) * f) >> 12);
        int32_t rz = q[1]->vz + (((q[3]->vz - q[1]->vz) * f) >> 12);
        int32_t lu = uv[0] + (((uv[4] - uv[0]) * f) >> 12);
        int32_t lv = uv[1] + (((uv[5] - uv[1]) * f) >> 12);
        int32_t ru = uv[2] + (((uv[6] - uv[2]) * f) >> 12);
        int32_t rv = uv[3] + (((uv[7] - uv[3]) * f) >> 12);

        int32_t x = lx << 16, dx = ((rx - lx) << 16) / nu;
        int32_t y = ly << 16, dy = ((ry - ly) << 16) / nu;
        int32_t z = lz << 16, dz = ((rz - lz) << 16) / nu;
        int32_t u = lu << 16, du = ((ru - lu) << 16) / nu;
        int32_t v = lv << 16, dv = ((rv - lv) << 16) / nu;

        int g = t * row;
        for (s = 0; s <= nu; s++, g++) {
            SVECTOR w;
            w.vx = (int16_t)(x >> 16);
            w.vy = (int16_t)(y >> 16);
            w.vz = (int16_t)(z >> 16);
            w.pad = 0;
            g_u[g] = (uint8_t)(u >> 16);
            g_v[g] = (uint8_t)(v >> 16);

            gte_ldv0(&w);
            gte_rtps();
            gte_stsxy(&g_sv[g]);
            gte_stsz(&g_sz[g]);
            g_ok[g] = (g_sz[g] > DH_NEAR) && on_screen(&g_sv[g]);

            x += dx; y += dy; z += dz; u += du; v += dv;
        }
    }

    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    for (t = 0; t < nv; t++) {
        for (s = 0; s < nu; s++) {
            int a = t * row + s, b = a + 1, c = a + row, d = c + 1;
            if (!g_ok[a] || !g_ok[b] || !g_ok[c] || !g_ok[d]) continue;
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) return;
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, col[0], col[1], col[2]);
            poly->tpage = tex_tpage[tex];
            poly->clut  = tex_clut[tex];
            poly->u0 = g_u[a]; poly->v0 = g_v[a];
            poly->u1 = g_u[b]; poly->v1 = g_v[b];
            poly->u2 = g_u[c]; poly->v2 = g_v[c];
            poly->u3 = g_u[d]; poly->v3 = g_v[d];
            poly->x0 = g_sv[a].vx; poly->y0 = g_sv[a].vy;
            poly->x1 = g_sv[b].vx; poly->y1 = g_sv[b].vy;
            poly->x2 = g_sv[c].vx; poly->y2 = g_sv[c].vy;
            poly->x3 = g_sv[d].vx; poly->y3 = g_sv[d].vy;
            addPrim(&ot[otz_of(g_sz[a], g_sz[b], g_sz[c], g_sz[d])], poly);
            ctx->next_packet += sizeof(POLY_FT4);
        }
    }
}

static void draw_box(RenderContext *ctx) {
    if (!dh_smd) return;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint8_t *p = (uint8_t *)dh_smd->p_prims;
    int i;
    for (i = 0; i < dh_key_count; i++) {
        uint8_t stride = cull_keys[i].stride;
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t tex = room_tex_map[i];
        /* Every prim in this mesh is a textured quad; anything else (a
           re-export that adds one) is skipped rather than drawn wrong. */
        if (pt->type >= 2 && tex < DH_TEX_COUNT)
            draw_box_quad(ctx, p, buf_end, tex);
        p += stride;
    }
}

/* ---- The tube's draw ---------------------------------------------------------
   catacomb_doors.c's leaf draw with a spin for the leaf's slide: the view
   composed with a rotation about Y, every primitive projected (360 of them, no
   cull key - the room arena's table is the box's), the lava slot for every
   face. The faces point INWARD, so from inside the shaft they all pass the
   backface test; from below the mouth they are all behind the camera.

   THE ONE CULL: a quad whose deepest point is less than DH_TUBE_REACH below
   the camera is skipped before it is projected. Above the camera it is
   behind it; within DH_TUBE_REACH of it, a wall 386 from the axis projects
   past DH_SCREEN_REACH (386 * 256 / 400 = 247 px) - off every edge of the
   screen. That is a whole slab of rings for nothing, early in the fall, and
   every ring once the camera has left the tube.

   Each quad is darkened with its depth, toward a third at the far end of the
   shaft, so the walls rush out of the dark while the lava below - drawn at
   full by the box - is the light at the end of it. */
#define DH_TUBE_DARK_FAR  6000   /* sz at which the tube is at its darkest */
#define DH_TUBE_DARK_MIN    96   /* 0..256: how dark that is               */
#define DH_TUBE_REACH      400   /* see THE ONE CULL                        */

static void draw_tube(RenderContext *ctx, MATRIX *view) {
    if (!tube_smd) return;

    MATRIX  world_m, combined;
    SVECTOR rot = { 0, (int16_t)spin, 0, 0 };
    VECTOR  pos = { 0, 0, 0 };
    RotMatrix(&rot, &world_m);
    TransMatrix(&world_m, &pos);
    CompMatrixLV(view, &world_m, &combined);
    gte_SetRotMatrix(&combined);
    gte_SetTransMatrix(&combined);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    SVECTOR *vp = tube_smd->p_verts;
    uint8_t *p  = (uint8_t *)tube_smd->p_prims;
    int i;

    for (i = 0; i < tube_smd->n_prims; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint8_t stride = pt->len;
        if (pt->type < 2) { p += stride; continue; }   /* quads only, as modelled */
        if (i < tube_key_count && tube_ymax[i] < cam_y + DH_TUBE_REACH) {
            p += stride; continue;                       /* THE ONE CULL */
        }

        uint16_t *vi = (uint16_t *)(p + 4);
        DVECTOR sv[4];
        int32_t sz[4], nclip;

        gte_ldv3(&vp[vi[0]], &vp[vi[1]], &vp[vi[2]]);
        gte_rtpt();
        gte_stsxy3c(sv);
        if (!on_screen(&sv[0]) || !on_screen(&sv[1]) || !on_screen(&sv[2])) {
            p += stride; continue;
        }
        gte_nclip();
        gte_stopz(&nclip);
        if (nclip <= 0) { p += stride; continue; }
        gte_stsz4c(sz);
        if (sz[1] <= DH_NEAR || sz[2] <= DH_NEAR || sz[3] <= DH_NEAR) {
            p += stride; continue;
        }
        int32_t s0 = sz[1], s1 = sz[2], s2 = sz[3];

        gte_ldv0(&vp[vi[3]]);
        gte_rtps();
        gte_stsxy(&sv[3]);
        gte_stsz(&sz[3]);
        if (!on_screen(&sv[3]) || sz[3] <= DH_NEAR) { p += stride; continue; }

        if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) break;

        int32_t avg  = (s0 + s1 + s2 + sz[3]) >> 2;
        int32_t dark = (avg >= DH_TUBE_DARK_FAR) ? DH_TUBE_DARK_MIN
                     : 256 - ((256 - DH_TUBE_DARK_MIN) * avg) / DH_TUBE_DARK_FAR;
        uint8_t *col = p + 16;
        uint8_t *uv  = p + 20;

        POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
        setPolyFT4(poly);
        setRGB0(poly, (uint8_t)((col[0] * dark) >> 8),
                      (uint8_t)((col[1] * dark) >> 8),
                      (uint8_t)((col[2] * dark) >> 8));
        poly->tpage = tex_tpage[0];
        poly->clut  = tex_clut[0];
        poly->u0 = uv[0]; poly->v0 = uv[1];
        poly->u1 = uv[2]; poly->v1 = uv[3];
        poly->u2 = uv[4]; poly->v2 = uv[5];
        poly->u3 = uv[6]; poly->v3 = uv[7];
        poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
        poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
        poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
        poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
        addPrim(&ot[otz_of(s0, s1, s2, sz[3])], poly);
        ctx->next_packet += sizeof(POLY_FT4);

        p += stride;
    }

    /* Put the plain view back for anything drawn after (the doors' tail). */
    gte_SetRotMatrix(view);
    gte_SetTransMatrix(view);
}

void descent_to_hell_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* Nothing here fogs; set past the room's reach for anything that asks. */
    g_fog_near = 12000;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : 16000;

    /* The clear colour, not a full-screen TILE (wrong turn #3): a deep red
       that only ever shows in a gap. */
    render_set_clear_colour(ctx, 24, 0, 0);

    /* 128x128 texture window: both textures sit at Voff 0, and the lava
       floor's UVs run past 128 and wrap through it. */
    {
        uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        if (ctx->next_packet + sizeof(DR_TWIN) <= buf_end) {
            RECT tw = { 0, 0, 128 >> 3, 128 >> 3 };
            DR_TWIN *twin = (DR_TWIN *)ctx->next_packet;
            setTexWindow(twin, &tw);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_LENGTH - 1], twin);
            ctx->next_packet += sizeof(DR_TWIN);
        }
    }

    MATRIX view;
    camera_build_view(&view);
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);

    if (exp != DBG_EXP_NO_MESH) {
        draw_box(ctx);
        draw_tube(ctx, &view);
    }

    /* >>> LEVEL 8 IS THE LINES, the only thing that stands in this room. <<<
       Before the white: same slot, LIFO, so they draw over it. */
    if (exp != DBG_EXP_NO_ENTITIES) draw_lines(ctx);
    draw_white(ctx, white_level());
}
