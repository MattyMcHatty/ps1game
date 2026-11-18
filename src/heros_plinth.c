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
#include "heros_plinth.h"
#include "heros_plinth_tex_map.h"
#include "underworld.h"
#include "underworld_sky.h"
#include "hell_hath_no_fury.h"  /* the volcanic floor, a narrow uploader      */
#include "descent_to_hell.h"    /* the lava, a narrow uploader                */
#include "catacombs_entry.h"    /* the catacomb inner door, a narrow uploader */
#include "save_point.h"
#include "dresser.h"
#include "sconce.h"
#include "oil_dispenser.h"
#include "bars.h"
#include "particles.h"
#include "btn_glyph.h"
#include "door.h"
#include "player.h"       /* game_flag, show_pickup_msg_raw                */
#include "hadad.h"        /* hadads[] - Hadad's death on a pre-flag save   */
#include "spirit_glow.h"  /* the mini-bosses' spirits and their colours    */
#include "sound.h"        /* the ceremony's cues, SND_BANK_PLINTH          */

/* Hero's Plinth - see heros_plinth.h. The room half is src/wingscape.c's
   (itself Hell Hath No Fury's): cull keys, radius side cull and all. */

static SMD  *hp_smd  = NULL;
static void *hp_buff = NULL;

/* ---- The room --------------------------------------------------------------
   x[-1900,2100] z[-2100,2100] (the collision proxy's bounds; the drawn floor
   runs to x=-2100 behind the frame). The sky is centred on the middle of the
   drawn geometry, half way up Destiny's Door's frame (floor 0, top -1120). */
#define HP_CX                0
#define HP_CZ                0
#define HP_SKY_Y          (-560)

#define HP_FLOOR_Y           0
#define HP_EYE_Y          (HP_FLOOR_Y - GROUND_FLOOR_Y - 40)
/* The tallest drawn thing is the frame's arch, whose top is at y=-1120. */
#define HP_CEILING_Y      (-1125)

/* The ramp up to the frame: x[-1700,-1500] z[-700,700], y=0 at x=-1500 to
   y=-70 at the frame's foot, x=-1700 (collision wall 21, the frame, stands
   there from -70 up). Its two 200-square corners beside the frame,
   x[-1900,-1700], fall from -70 at z=+-700 back to 0 at z=+-900. */
#define HP_RAMP_TOP_Y     (-70)

/* ---- THE NORTH-EAST DOOR, to Wingscape's west door -------------------------
   The catacomb inner door polys stand on the cut corner x+z=3600 (collision
   wall 10, inward normal (-0.707,-0.707)), foot (1700,1900)-(1900,1700),
   leaning out to y=-600: centre (1800,1800), approached from the south-west.

   THE SIGN is on a free yaw facing the way the reader faces - 512, north-
   east - 11 off the door toward the player (8 on each axis). THE SPAWN, for an
   arrival through it, is 250 in along the normal (177 on each axis), clear of
   the 195 push radius, facing south-west into the room. */
#define HP_NE_X            1800
#define HP_NE_Z            1800
#define HP_NE_TEXT_X       1792
#define HP_NE_TEXT_Z       1792
#define HP_NE_TEXT_YAW      512
#define HP_NE_SPAWN_X      1623
#define HP_NE_SPAWN_Z      1623
#define HP_NE_SPAWN_ROT    2560

/* ---- THE SOUTH-EAST DOOR, to Sharur's Trial's north-west door --------------
   NOT a flat door: a 200 x 200 x 400 block in the cut corner, x[1700,1900]
   z[-1900,-1700], whose two faces into the room - west (collision wall 22,
   x=1700) and north (wall 16, z=-1700) - are both wd_dr. So the door point is
   the block's centre (1800,-1800): a player held 195 off either face is 295
   from it (Manhattan), and 476 on the diagonal at the block's outer corner,
   all inside the 500 reach.

   THE SIGN stands off that outer corner (1700,-1700), 11 toward the player
   (8 on each axis), facing the way the reader faces - 1536, south-east. THE
   SPAWN is 250 off both faces, (1450,-1450), facing north-west into the
   room. */
#define HP_SE_X            1800
#define HP_SE_Z          (-1800)
#define HP_SE_TEXT_X       1692
#define HP_SE_TEXT_Z     (-1692)
#define HP_SE_TEXT_YAW     1536
#define HP_SE_SPAWN_X      1450
#define HP_SE_SPAWN_Z    (-1450)
#define HP_SE_SPAWN_ROT    3584

#define HP_DOOR_TEXT_Y    (HP_FLOOR_Y - 200)
#define HP_TEXT_RADIUS     1500
#define HP_FADE_NEAR       1000
#define HP_TRIGGER_RADIUS   500

/* Circle edge-detect, one per door. Seeded "held" by the arm so a press
   carried in through the transition cannot fire on the arrival frame. */
static int ne_circle_prev = 1;
static int se_circle_prev = 1;
static int frame_circle_prev = 1;   /* ...and Destiny's Door's frame's */

/* ---- THE FLOOR ZONES -------------------------------------------------------
   One storey, so order matters only where zones overlap: the three ramps
   first, the y=0 catch-all LAST (apply_height takes the first match). */
static void hp_floor_zones_init(void) {
    int i = 0;

    /* The ramp up to the frame, rising WEST. */
    floor_zones[i].type            = FLOOR_RAMP;
    floor_zones[i].min_x = -1700;  floor_zones[i].max_x = -1500;
    floor_zones[i].min_z =  -700;  floor_zones[i].max_z =   700;
    floor_zones[i].ramp_y_start    = HP_FLOOR_Y;      /* at x=-1500 */
    floor_zones[i].ramp_y_end      = HP_RAMP_TOP_Y;   /* at x=-1700 */
    floor_zones[i].ramp_axis_start = -1500;
    floor_zones[i].ramp_axis_end   = -1700;
    floor_zones[i].ramp_along_x    = 1;
    i++;
    /* The two corners beside the frame, falling away from it. */
    floor_zones[i].type            = FLOOR_RAMP;      /* north */
    floor_zones[i].min_x = -1900;  floor_zones[i].max_x = -1700;
    floor_zones[i].min_z =   700;  floor_zones[i].max_z =   900;
    floor_zones[i].ramp_y_start    = HP_RAMP_TOP_Y;   /* at z=700 */
    floor_zones[i].ramp_y_end      = HP_FLOOR_Y;      /* at z=900 */
    floor_zones[i].ramp_axis_start =   700;
    floor_zones[i].ramp_axis_end   =   900;
    floor_zones[i].ramp_along_x    = 0;
    i++;
    floor_zones[i].type            = FLOOR_RAMP;      /* south */
    floor_zones[i].min_x = -1900;  floor_zones[i].max_x = -1700;
    floor_zones[i].min_z =  -900;  floor_zones[i].max_z =  -700;
    floor_zones[i].ramp_y_start    = HP_RAMP_TOP_Y;   /* at z=-700 */
    floor_zones[i].ramp_y_end      = HP_FLOOR_Y;      /* at z=-900 */
    floor_zones[i].ramp_axis_start =  -700;
    floor_zones[i].ramp_axis_end   =  -900;
    floor_zones[i].ramp_along_x    = 0;
    i++;

    /* THE FLOOR, y=0: the catch-all, LAST. */
    floor_zones[i].type  = FLOOR_FLAT;
    floor_zones[i].min_x = -1900;  floor_zones[i].max_x = 2100;
    floor_zones[i].min_z = -2100;  floor_zones[i].max_z = 2100;
    floor_zones[i].y     = HP_FLOOR_Y;
    i++;

    floor_zone_count = i;
}

/* ---- Textures --------------------------------------------------------------
   EIGHT mesh textures, FOUR owned, and a NINTH owned for later:
     0  volcanic             Hell Hath No Fury's, its narrow uploader (x384 y256)
     1  lava                 the Descent's, its narrow uploader (x320 y256)
     2  catacomb inner door  the Catacombs Entry's, its narrow uploader (x832
                             y0) - which is also DOOR_PANEL_CATACOMB's panel,
                             the transition out to Wingscape, so it MUST be
                             uploaded here (the panel draws before
                             STATE_LOADING).
     3  wd_dr                RESIDENT: the fat door's startup upload at x768
                             y256 - DOOR_PANEL_WOOD's panel too. Nothing here
                             may take that page.
     4  destiny door frame top left      OWNED, 8bpp x384 y0,   CLUT (672,499)
     5  destinys door frame top right    OWNED, 8bpp x512 y0,   CLUT (672,500)
     6  destinys door frame bottom left  OWNED, 8bpp x896 y256, CLUT (672,490)
     7  destinys door frame bottom right OWNED, 8bpp x704 y0,   CLUT (672,484)
     8  destinys door                    OWNED, 8bpp x768 y0,   CLUT (672,483) -
                             DESTINY'S DOOR itself. No poly in the export maps
                             to it: the draw loop lays it over the frame's 30
                             RECESSED quads (x=-1750) itself, with UVs from
                             their vertices, once the jewel ceremony has
                             opened the door (see DESTINY'S DOOR below). Its
                             CLUT carries STP (png_to_tim --stp) so the
                             ceremony can fade it in ADDITIVELY over the
                             frame's own texture; drawn opaque, STP is
                             ignored.
   >>> ALL FIVE ARE 8bpp, FULL 256-COLOUR ART, BY THE USER'S ORDER. <<< They
   were shipped at 4bpp first and the user had them put back: never drop a
   texture to 16 colours without asking (tools/ADDING_A_ROOM.txt STEP 3d).
   Each takes a Voff-0 page whose tenant this room never draws - the
   cobblestones, Sharur's rocks, the two tablets (Sharur's / the Wings
   Tablet), the cracked tablet / the puzzle's cuneiform and Hell Hath No
   Fury's oil tank - every one re-uploaded on entry by the room that draws it
   (tools/VRAM_MAP_UNDERWORLD.txt: the chapter re-lays VRAM room by room). The
   Seal's page (x576 y0) is NOT one of them at 8bpp: its right half is
   hadad_idle_64, uploaded once at boot. Each 256-word CLUT is a Chapter 1-3
   room texture's (mud, Boss Wall, cuneiform _symbols, xt_dr_outr,
   xt_dr_cmplt/lckd), re-uploaded by its own room and never drawn down here.
   Registered in tools/vram_map.py KNOWN_STREAM_PAIRS. The sky's texture is
   src/underworld_sky.c's business. */
#define HP_TEX_COUNT 9
#define HP_OWNED     5

static uint16_t tex_tpage[HP_TEX_COUNT];
static uint16_t tex_clut[HP_TEX_COUNT];
static int      owned_tex[HP_OWNED] = { -1, -1, -1, -1, -1 };

void heros_plinth_load_assets(void) {
    /* BANK: Chapter 4. Derived, not guessed - py tools/check_tex_banks.py walks
       the uploader call graph and fails the build if this is short. */
    texmgr_set_bank(TEXBANK_UNDERWORLD);
    owned_tex[0] = texmgr_register("\\TEXUNDW\\DDFRTL.TIM;1");
    owned_tex[1] = texmgr_register("\\TEXUNDW\\DDFRTR.TIM;1");
    owned_tex[2] = texmgr_register("\\TEXUNDW\\DDFRBL.TIM;1");
    owned_tex[3] = texmgr_register("\\TEXUNDW\\DDFRBR.TIM;1");
    owned_tex[4] = texmgr_register("\\TEXUNDW\\DSTNYDR.TIM;1");
    TIM_SLOT(0, VOLCANIC);
    TIM_SLOT(1, LAVA);
    TIM_SLOT(2, CTCMBDR);
    TIM_SLOT(3, WDDR);
    TIM_SLOT(4, DDFRTL);
    TIM_SLOT(5, DDFRTR);
    TIM_SLOT(6, DDFRBL);
    TIM_SLOT(7, DDFRBR);
    TIM_SLOT(8, DSTNYDR);
}

/* Pure LoadImage from the RAM copies texmgr holds - no CD access, safe during
   the transition (main's STATE_LOADING DrawSyncs first). */
void heros_plinth_upload_textures(void) {
    int i;
    hell_hath_no_fury_upload_volcanic();
    descent_to_hell_upload_lava();
    catacombs_entry_upload_inner_door();
    for (i = 0; i < HP_OWNED; i++)
        if (owned_tex[i] >= 0) texmgr_upload(owned_tex[i]);
    underworld_sky_upload_textures();
}

/* DESTINY'S DOOR alone, for the Chamber of Destiny: the door is on its east
   wall too, and DOOR_PANEL_DESTINY draws it on the way out. Narrow, so the
   frame's four (whose pages the Chamber re-lays) are not stamped with it. */
void heros_plinth_upload_destiny_door(void) {
    if (owned_tex[4] >= 0) texmgr_upload(owned_tex[4]);
}

/* ---- The cull key, and its RADIUS ------------------------------------------
   ws_build_cull_keys() verbatim (src/wingscape.c, itself Hell Hath No Fury's;
   tools/DIAGNOSING_FRAME_RATE.txt STEP 3I for why the radius). Built on the
   same call that reloads the mesh it describes, and nowhere else. */
static int     hp_key_count = 0;
static int32_t hp_y_lo = 0, hp_y_hi = 0;

static void hp_build_cull_keys(void) {
    hp_key_count = 0;
    if (!hp_smd) return;
    uint8_t *p = (uint8_t *)hp_smd->p_prims;
    int i, n = hp_smd->n_prims;
    if (n > HEROS_PLINTH_PRIM_COUNT) n = HEROS_PLINTH_PRIM_COUNT;
    hp_y_lo = hp_y_hi = 0;
    for (i = 0; i < n; i++) {
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        uint16_t     *vi = (uint16_t *)(p + 4);
        SVECTOR      *v0 = &hp_smd->p_verts[vi[0]];
        int nv = (pt->type >= 2) ? 4 : 3;
        int16_t lo_x = v0->vx, hi_x = v0->vx;
        int16_t lo_z = v0->vz, hi_z = v0->vz;
        int k;
        for (k = 0; k < nv; k++) {
            SVECTOR *vk = &hp_smd->p_verts[vi[k]];
            if (vk->vx < lo_x) lo_x = vk->vx;
            if (vk->vx > hi_x) hi_x = vk->vx;
            if (vk->vz < lo_z) lo_z = vk->vz;
            if (vk->vz > hi_z) hi_z = vk->vz;
            if (vk->vy < hp_y_lo) hp_y_lo = vk->vy;
            if (vk->vy > hp_y_hi) hp_y_hi = vk->vy;
        }
        cull_keys[i].x      = v0->vx;
        cull_keys[i].z      = v0->vz;
        cull_keys[i].stride = pt->len;
        {
            int32_t rad = (hi_x - lo_x) + (hi_z - lo_z);   /* >= any |v - v0| */
            rad = (rad + 3) >> 2;
            cull_keys[i].pad = (uint8_t)(rad >= 255 ? 255 : rad);
        }
        p += pt->len;
    }
    hp_key_count = n;
}

void heros_plinth_load_geometry(void) {
    hp_buff = room_arena_load("\\TEXUNDW\\HEROPLN.SMD;1");
    hp_smd  = hp_buff ? smdInitData(hp_buff) : NULL;
    /* ...and the tex map, no-cull bits and walls packed onto the end of the
       same file (src/room_data.h). No block, no room. */
    if (hp_smd && !room_data_bind(hp_smd->n_prims)) hp_smd = NULL;
    hp_build_cull_keys();
    /* The sky: kept across Chapter 4's rooms (a no-op when already held). */
    underworld_sky_load();
}

/* ---- The two doors ---------------------------------------------------------*/
void heros_plinth_doors_arm(void) {
    int held = interact_tapped();
    ne_circle_prev = held;
    se_circle_prev = held;
    frame_circle_prev = held;
}

/* The shared body: a fresh Circle within reach of (door_x, door_z) and facing
   it. The edge state is kept up to date even while locked. */
static int hp_door_triggered(int32_t door_x, int32_t door_z,
                             int *circle_prev, int lock) {
    int held = interact_tapped();
    int just = held && !*circle_prev;
    int32_t dx, dz;
    *circle_prev = held;
    if (lock || !just) return 0;
    dx = cam_x - door_x;
    dz = cam_z - door_z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= HP_TRIGGER_RADIUS)
        return 0;
    return interact_facing(door_x, door_z);
}

int heros_plinth_ne_door_triggered(int lock) {
    return hp_door_triggered(HP_NE_X, HP_NE_Z, &ne_circle_prev, lock);
}
int heros_plinth_se_door_triggered(int lock) {
    return hp_door_triggered(HP_SE_X, HP_SE_Z, &se_circle_prev, lock);
}

static void hp_door_text(RenderContext *ctx, int32_t door_x, int32_t door_z,
                         int32_t text_x, int32_t text_z, int yaw) {
    int32_t dx = cam_x - door_x;
    int32_t dz = cam_z - door_z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (xz >= HP_TEXT_RADIUS) return;
    if (xz > HP_FADE_NEAR) {
        int range = HP_TEXT_RADIUS - HP_FADE_NEAR;
        int prog  = xz - HP_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, "Press " BTN_CIRCLE " to enter",
                            text_x, HP_DOOR_TEXT_Y, text_z,
                            50, 255, 50, fade, yaw,
                            DOOR_PIXEL_SIZE);
}

static void hp_spawn_at(int32_t x, int32_t z, int rot) {
    cam_x     = x;
    cam_y     = HP_EYE_Y;
    cam_vy    = 0;
    cam_z     = z;
    cam_rot   = rot;
    cam_pitch = 0;
    camera_look_cancel();
    heros_plinth_doors_arm();
}

void heros_plinth_spawn_ne(void) {
    hp_spawn_at(HP_NE_SPAWN_X, HP_NE_SPAWN_Z, HP_NE_SPAWN_ROT);
}
void heros_plinth_spawn_se(void) {
    hp_spawn_at(HP_SE_SPAWN_X, HP_SE_SPAWN_Z, HP_SE_SPAWN_ROT);
}
/* Back out of DESTINY'S DOOR from the Chamber of Destiny: 250 off the frame's
   face (x=-1700) on its centre line, past the ramp's foot (x=-1500) so on the
   y=0 floor, facing east into the room - the way through the door. */
void heros_plinth_spawn_destiny(void) {
    hp_spawn_at(-1450, 0, 1024);
}

/* ===========================================================================
   DESTINY'S DOOR - the jewels and the ceremony
   ===========================================================================
   tools/DESTINYS_DOOR_CHECKLIST.txt has the brief. The frame's FACE is x=-1700
   (the player looks at it facing WEST, so their LEFT is -z - the sense its
   "bottom left"/"bottom right" textures are named in). Three jewels sit on it,
   each where four triangles meet in the export:

       LEFT  (y -539, z -600)  YELLOW  Gula's
       TOP   (y -970, z    0)  GREEN   Hadad's
       RIGHT (y -539, z +600)  BLUE    Adapa's

   and its centre is RECESSED 50, to x=-1750: thirty quads over y[-870,-70]
   z[-500,500]. That recess is where Destiny's Door's texture goes.

   THE PROMPT, at the frame's centre:
     - the ceremony has not run: "Press O to interact". Circle with none of the
       three dead reads the marble line and nothing else happens (the prompt
       stays - there is nothing to use up). With ANY of them dead it plays the
       ceremony.
     - it has run with all three: "Press O to enter" -> through Destiny's
       Door to the Chamber of Destiny (src/chamber_of_destiny.h).
     - it has run with fewer: no prompt at all, ever (FLAG_DESTINY_CEREMONY is
       once only; Chapter 4 cannot go back for the rest).

   THE CEREMONY is a director in the death scenes' manner (adapa_death.c): the
   player anchored, the camera ours, main.c's cutscene branch running nothing
   else. The camera glides to the room's centre and looks up and away from the
   door into the eastern sky; the spirits appear far off and fly in on curves
   that swing round the NORTH side of the room - never overhead, where the
   tracking yaw would flip - and into the jewels. THE CAMERA'S PATH IS THE SAME
   WHICHEVER BOSSES ARE DEAD: it follows the centroid of all three paths, and
   a living boss's spirit is simply not drawn and its jewel never lights. */

#define HP_FRAME_X        (-1700)   /* the face                                 */
#define HP_FRAME_Z            0
#define HP_FRAME_CY        (-560)   /* half way up: the settled shot's aim      */
#define HP_FRAME_TEXT_X   (-1689)   /* 11 off the face, as the doors' signs     */
#define HP_FRAME_TEXT_Y   (HP_RAMP_TOP_Y - 200)
#define HP_FRAME_TEXT_Z       0
#define HP_FRAME_TEXT_YAW   3072    /* the reader faces west                    */

/* The recess: Destiny's Door's UV rectangle. The vertices sit at x=-1749.99,
   so the test is a window rather than an equality. */
#define HP_RECESS_X_LO    (-1755)
#define HP_RECESS_X_HI    (-1745)
#define HP_RECESS_Z0       (-500)
#define HP_RECESS_W         1000
#define HP_RECESS_Y0       (-870)
#define HP_RECESS_H          800
#define HP_DOOR_SLOT           8

/* The jewels' glow: the spirit's three rings at this outer half-size, a hair
   proud of the face so it never sorts behind it. */
#define HP_JEWEL_X        (HP_FRAME_X + 10)
#define HP_JEWEL_GLOW        110

/* The scene's camera point: the room's centre, at the player's eye. */
#define HP_SCENE_X   HP_CX
#define HP_SCENE_Z   HP_CZ

/* ---- Timing, frames at 60 fps ---------------------------------------------- */
#define HP_T_GLIDE        90   /* 1.5 s to the centre, turning up and away       */
#define HP_T_APPEAR       60   /* 1 s: the spirits fade up in the distance       */
#define HP_T_APPEAR_IN    40
#define HP_T_SETTLE       45   /* the shot eases onto the whole frame            */
#define HP_T_WAIT         60   /* "It will wait 1 second"                        */
#define HP_T_DOOR        150   /* Destiny's Door fades up over 2.5 s             */
#define HP_T_HOLD         45   /* ...and is looked at                            */
#define HP_T_RETURN       45   /* the pitch back to level                        */
#define HP_T_JEWEL_IN     60   /* a jewel's glow, up from nothing                */
#define HP_FLASH_START   200   /* the full-screen flash, additive, 0..256 ...    */
#define HP_FLASH_FRAMES   24   /* ...gone in 0.4 s                               */

enum { SP_ADAPA = 0, SP_HADAD, SP_GULA, SP_COUNT };

/* Each spirit's flight: a quadratic curve from far out in the eastern sky (S)
   round a control point off the room's north side (C) into its jewel (J),
   arriving `arrive` frames into the flight - Adapa first, Hadad 1 s later,
   Gula 0.5 s after that.

   THEY START CLOSE, IN A TRIANGLE - the jewels' own: Hadad at the apex, 300
   above the other two, which sit 300 either side of him on the same x, so it
   faces the camera square-on. The order holds all the way in (Adapa's z is
   always the greatest, Hadad's y always the highest), so the formation opens
   out round the north side and closes onto the jewels without the three ever
   crossing. */
typedef struct {
    int16_t sx, sy, sz;
    int16_t cx, cy, cz;
    int16_t jx, jy, jz;
    int16_t arrive;
} HpPath;

static const HpPath hp_paths[SP_COUNT] = {
    /* ADAPA, blue, into the RIGHT jewel */
    { 3500, -2650,  300,   400, -1900, 3300,   HP_JEWEL_X, -539,  600,  480 },
    /* HADAD, green, into the TOP jewel */
    { 3500, -2950,    0,   400, -2000, 2800,   HP_JEWEL_X, -970,    0,  540 },
    /* GULA, yellow, into the LEFT jewel */
    { 3500, -2650, -300,   400, -1900, 2300,   HP_JEWEL_X, -539, -600,  570 },
};
#define HP_T_FLY  570   /* the last arrival */

static const uint8_t *hp_spirit_rgb(int i) {
    return i == SP_ADAPA ? SPIRIT_RGB_ADAPA
         : i == SP_HADAD ? SPIRIT_RGB_HADAD
         :                 SPIRIT_RGB_GULA;
}

typedef enum {
    HPS_IDLE = 0,
    HPS_GLIDE,
    HPS_APPEAR,
    HPS_FLY,
    HPS_SETTLE,
    HPS_WAIT,
    HPS_DOOR,
    HPS_HOLD,
    HPS_RETURN,
} HpScene;

/* ---- The ceremony's sound ---------------------------------------------------
   spirit_decend, then spirit_woosh straight after it, timed BACKWARDS from the
   first hit so the woosh ENDS as the first spirit reaches its jewel (Adapa's
   at 480 if he is dead, else Hadad's, else Gula's). Then a twinkle on each
   hit. The clocks are the clips' own lengths at 60 fps: woosh 4.50 s and
   descent 4.40 s (sound.h). They run on FLIGHT time - frames from the end of
   APPEAR - so the descent may start inside APPEAR: with Adapa first it keys
   on 6 frames into it, while the spirits are still fading up. */
#define HP_SND_WOOSH_FRAMES  270
#define HP_SND_DESC_FRAMES   264

static const SfxID hp_twinkle[SP_COUNT] = {
    SFX_TWINKLE1, SFX_TWINKLE2, SFX_TWINKLE3,
};

static HpScene hp_scene = HPS_IDLE;
static int32_t hp_t;                 /* frames into the current phase          */
static int32_t hp_clock;             /* the glows' pulse                       */
static int     hp_dead;              /* bit SP_*: that boss was dead at start  */
static int32_t hp_spirit_bright;     /* 0..256, the flight's fade-in           */
static int32_t hp_jewel_t[SP_COUNT]; /* frames since that jewel was hit, -1 not */
static int32_t hp_door_lv;           /* 0..256 while the scene runs            */
static int32_t hp_flash;             /* 0..256                                 */
static const uint8_t *hp_flash_rgb = 0;
static int32_t hp_from_x, hp_from_y, hp_from_z, hp_from_rot, hp_from_pitch;
static int32_t hp_to_rot, hp_to_pitch;

/* ---- Who is dead ------------------------------------------------------------
   Adapa and Gula have their bits. Hadad has FLAG_HADAD_DEAD now; a save from
   before it still knows from his placement's saved state, so that answers
   too. */
static int hp_dead_mask(void) {
    int m = 0, i;
    if (game_flag(FLAG_ADAPA_DEAD)) m |= 1 << SP_ADAPA;
    if (game_flag(FLAG_HADAD_DEAD)) m |= 1 << SP_HADAD;
    else
        for (i = 0; i < hadad_count; i++)
            if (hadads[i].state == HAD_DEAD) { m |= 1 << SP_HADAD; break; }
    if (game_flag(FLAG_GULA_DEAD))  m |= 1 << SP_GULA;
    return m;
}
#define HP_ALL_DEAD ((1 << SP_ADAPA) | (1 << SP_HADAD) | (1 << SP_GULA))

/* ---- Small maths (adapa_death.c's, which borrowed them in turn) ------------ */
static int32_t hp_turn_delta(int32_t from, int32_t to) {
    int32_t d = ((to - from) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    return d;
}

/* 0..4096 smoothstep. THE ENDS RETURN BEFORE THE DIVIDE (a clamped quotient
   once compiled to back-to-back MULTs that Redux misread). */
static int32_t hp_smooth(int32_t t, int32_t total) {
    int32_t p;
    if (t <= 0)     return 0;
    if (t >= total) return 4096;
    p = (t * 4096) / total;
    return (((p * p) >> 12) * (3 * 4096 - 2 * p)) >> 12;
}

static int32_t hp_lerp(int32_t a, int32_t b, int32_t s) {   /* s 0..4096 */
    return a + (((b - a) * s) >> 12);
}

static int32_t hp_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x, last, s = 1, t = v;
    while (t > 0) { t >>= 2; s <<= 1; }
    x = s;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

/* hadad_grinder.c's aim_pitch: the pitch that looks at a point dy below and
   dz in front, on the SDK's own trig. */
static int32_t hp_aim_pitch(int32_t dy, int32_t dz) {
    int32_t lo = -1024, hi = 1024;
    while (hi - lo > 1) {
        int32_t mid = (lo + hi) >> 1, a = mid & 4095;
        if (isin(a) * dz < icos(a) * dy) lo = mid;
        else                             hi = mid;
    }
    return lo;
}

/* The yaw that faces (dx, dz): adapa_death.c's sweep, coarse then fine, since
   this one runs every frame of the flight. */
static int32_t hp_face(int32_t dx, int32_t dz) {
    int32_t best = 0, best_dot = -0x7FFFFFFF, a, base;
    for (a = 0; a < 4096; a += 32) {
        int32_t dot = dx * isin(a) + dz * icos(a);
        if (dot > best_dot) { best_dot = dot; best = a; }
    }
    base = best;
    for (a = base - 32; a <= base + 32; a++) {
        int32_t dot = dx * isin(a & 4095) + dz * icos(a & 4095);
        if (dot > best_dot) { best_dot = dot; best = a & 4095; }
    }
    return best;
}

/* Yaw and pitch from the scene's camera point onto (x, y, z). */
static void hp_aim(int32_t x, int32_t y, int32_t z,
                   int32_t *rot, int32_t *pitch) {
    int32_t dx = x - HP_SCENE_X, dz = z - HP_SCENE_Z;
    int32_t fwd = hp_isqrt(dx * dx + dz * dz);
    if (fwd < 1) fwd = 1;
    *rot   = hp_face(dx, dz);
    *pitch = hp_aim_pitch(y - HP_EYE_Y, fwd);
}

/* Where spirit i is, `ft` frames into the flight: de Casteljau on the curve,
   eased in and out over its own flight time. */
static void hp_spirit_pos(int i, int32_t ft,
                          int32_t *x, int32_t *y, int32_t *z) {
    const HpPath *pp = &hp_paths[i];
    int32_t s = hp_smooth(ft, pp->arrive);
    int32_t ax = hp_lerp(pp->sx, pp->cx, s), bx = hp_lerp(pp->cx, pp->jx, s);
    int32_t ay = hp_lerp(pp->sy, pp->cy, s), by = hp_lerp(pp->cy, pp->jy, s);
    int32_t az = hp_lerp(pp->sz, pp->cz, s), bz = hp_lerp(pp->cz, pp->jz, s);
    *x = hp_lerp(ax, bx, s);
    *y = hp_lerp(ay, by, s);
    *z = hp_lerp(az, bz, s);
}

/* The camera's target: the centroid of ALL THREE, dead or not. */
static void hp_centroid(int32_t ft, int32_t *x, int32_t *y, int32_t *z) {
    int32_t sx = 0, sy = 0, sz = 0, px, py, pz;
    int i;
    for (i = 0; i < SP_COUNT; i++) {
        hp_spirit_pos(i, ft, &px, &py, &pz);
        sx += px; sy += py; sz += pz;
    }
    *x = sx / SP_COUNT; *y = sy / SP_COUNT; *z = sz / SP_COUNT;
}

/* ---- What is lit -------------------------------------------------------------
   While the scene runs, its own clocks; otherwise the saved answer. */
static int32_t hp_jewel_level(int i) {
    if (hp_scene != HPS_IDLE) {
        int32_t t = hp_jewel_t[i];
        if (t < 0) return 0;
        if (t >= HP_T_JEWEL_IN) return 256;
        return (t * 256) / HP_T_JEWEL_IN;
    }
    return (game_flag(FLAG_DESTINY_CEREMONY) && (hp_dead_mask() & (1 << i)))
           ? 256 : 0;
}

static int32_t hp_door_level(void) {
    if (hp_scene != HPS_IDLE) return hp_door_lv;
    return (game_flag(FLAG_DESTINY_CEREMONY) && hp_dead_mask() == HP_ALL_DEAD)
           ? 256 : 0;
}

static int hp_is_recess(const SVECTOR *a, const SVECTOR *b,
                        const SVECTOR *c, const SVECTOR *d) {
    return a->vx >= HP_RECESS_X_LO && a->vx <= HP_RECESS_X_HI &&
           b->vx >= HP_RECESS_X_LO && b->vx <= HP_RECESS_X_HI &&
           c->vx >= HP_RECESS_X_LO && c->vx <= HP_RECESS_X_HI &&
           d->vx >= HP_RECESS_X_LO && d->vx <= HP_RECESS_X_HI;
}

/* The door's 128x128 art over the whole recess: u across it left to right as
   the player sees it (+z), v down it (+y). 127, not 128, at the far edges -
   the 128 texture window would wrap 128 back to 0. */
static void hp_door_uv(const SVECTOR *v, uint8_t *u, uint8_t *vv) {
    int32_t uu = (((int32_t)v->vz - HP_RECESS_Z0) * 127) / HP_RECESS_W;
    int32_t tv = (((int32_t)v->vy - HP_RECESS_Y0) * 127) / HP_RECESS_H;
    if (uu < 0) uu = 0;
    if (uu > 127) uu = 127;
    if (tv < 0) tv = 0;
    if (tv > 127) tv = 127;
    *u  = (uint8_t)uu;
    *vv = (uint8_t)tv;
}

/* ---- The prompt ------------------------------------------------------------- */
enum { HP_FRAME_NONE = 0, HP_FRAME_INTERACT, HP_FRAME_ENTER };

static int hp_frame_mode(void) {
    if (!game_flag(FLAG_DESTINY_CEREMONY)) return HP_FRAME_INTERACT;
    return hp_dead_mask() == HP_ALL_DEAD ? HP_FRAME_ENTER : HP_FRAME_NONE;
}

int heros_plinth_scene_active(void) { return hp_scene != HPS_IDLE; }

static void hp_scene_park(void) {
    int i;
    hp_scene         = HPS_IDLE;
    hp_t             = 0;
    hp_spirit_bright = 0;
    hp_door_lv       = 0;
    hp_flash         = 0;
    for (i = 0; i < SP_COUNT; i++) hp_jewel_t[i] = -1;
}

/* The two long cues on flight time `ft` (see HP_SND_*). Called once a frame
   from APPEAR and FLY; ft is negative in APPEAR, so each fires exactly once. */
static void hp_scene_sound(int32_t ft) {
    int32_t first = HP_T_FLY;
    int i;
    for (i = 0; i < SP_COUNT; i++)
        if ((hp_dead & (1 << i)) && hp_paths[i].arrive < first)
            first = hp_paths[i].arrive;
    if (ft == first - HP_SND_WOOSH_FRAMES - HP_SND_DESC_FRAMES)
        sound_play(SFX_SPIRIT_DESC);
    if (ft == first - HP_SND_WOOSH_FRAMES)
        sound_play(SFX_WOOSH);
}

void heros_plinth_scene_reset(void) {
    if (hp_scene != HPS_IDLE) {
        camera_release_player();
        cam_pitch = 0;
        sound_stop(SFX_SPIRIT_DESC);   /* a new game started mid-ceremony */
        sound_stop(SFX_WOOSH);
    }
    hp_scene_park();
}

static void hp_scene_begin(int dead) {
    int32_t x, y, z;
    hp_scene_park();
    hp_dead       = dead;
    hp_clock      = 0;
    camera_look_cancel();
    camera_anchor_player(cam_x, cam_y, cam_z);
    hp_from_x     = cam_x;
    hp_from_y     = cam_y;
    hp_from_z     = cam_z;
    hp_from_rot   = cam_rot;
    hp_from_pitch = cam_pitch;
    hp_centroid(0, &x, &y, &z);              /* where they will appear */
    hp_aim(x, y, z, &hp_to_rot, &hp_to_pitch);
    hp_scene      = HPS_GLIDE;
}

static void hp_scene_finish(void) {
    game_flag_set(FLAG_DESTINY_CEREMONY);
    cam_x     = HP_SCENE_X;
    cam_y     = HP_EYE_Y;
    cam_z     = HP_SCENE_Z;
    cam_vy    = 0;
    cam_pitch = 0;
    camera_release_player();
    camera_look_cancel();
    heros_plinth_doors_arm();   /* nothing carried in the pad fires a door */
    frame_circle_prev = 1;
    hp_scene_park();
}

/* Circle at the frame. Edge-detected like the doors, the state kept up to date
   while locked. Returns 1 when the player goes THROUGH Destiny's Door (open:
   all three dead and the ceremony run) - main.c takes it to the Chamber of
   Destiny on DOOR_PANEL_DESTINY. */
int heros_plinth_frame_update(int lock) {
    int held = interact_tapped();
    int just = held && !frame_circle_prev;
    int mode, dead;
    int32_t dx, dz;
    frame_circle_prev = held;
    hp_clock++;                       /* the lit jewels keep pulsing */
    if (lock || !just) return 0;
    mode = hp_frame_mode();
    if (mode == HP_FRAME_NONE) return 0;
    dx = cam_x - HP_FRAME_X;
    dz = cam_z - HP_FRAME_Z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= HP_TRIGGER_RADIUS) return 0;
    if (!interact_facing(HP_FRAME_X, HP_FRAME_Z)) return 0;

    if (mode == HP_FRAME_ENTER)
        return 1;                     /* through, to the Chamber of Destiny */
    dead = hp_dead_mask();
    if (!dead) {
        show_pickup_msg_raw("A marble structure. It looks important.");
        return 0;
    }
    hp_scene_begin(dead);
    return 0;
}

/* ---- The director ------------------------------------------------------------
   main.c's cutscene branch, every frame while heros_plinth_scene_active(). */
void heros_plinth_scene_update(void) {
    int32_t x, y, z, rot, pitch;
    int i;
    if (hp_scene == HPS_IDLE) return;

    hp_t++;
    hp_clock++;
    cam_vy = 0;

    /* The flash and the lit jewels run on their own clocks through every
       phase after the hits. */
    if (hp_flash > 0) {
        hp_flash -= HP_FLASH_START / HP_FLASH_FRAMES;
        if (hp_flash < 0) hp_flash = 0;
    }
    for (i = 0; i < SP_COUNT; i++)
        if (hp_jewel_t[i] >= 0 && hp_jewel_t[i] < HP_T_JEWEL_IN) hp_jewel_t[i]++;

    switch (hp_scene) {

    /* GLIDE - to the room's centre, turning up and away from the door onto
       the patch of sky the spirits will appear in. */
    case HPS_GLIDE: {
        int32_t s = hp_smooth(hp_t, HP_T_GLIDE);
        cam_x     = hp_lerp(hp_from_x, HP_SCENE_X, s);
        cam_y     = hp_lerp(hp_from_y, HP_EYE_Y,   s);
        cam_z     = hp_lerp(hp_from_z, HP_SCENE_Z, s);
        cam_rot   = (hp_from_rot + ((hp_turn_delta(hp_from_rot, hp_to_rot) * s) >> 12)) & 4095;
        cam_pitch = hp_lerp(hp_from_pitch, hp_to_pitch, s);
        if (hp_t >= HP_T_GLIDE) { hp_t = 0; hp_scene = HPS_APPEAR; }
        break;
    }

    /* APPEAR - they fade up, far off, before they move. */
    case HPS_APPEAR:
        cam_x = HP_SCENE_X; cam_y = HP_EYE_Y; cam_z = HP_SCENE_Z;
        cam_rot = hp_to_rot; cam_pitch = hp_to_pitch;
        hp_spirit_bright = hp_t >= HP_T_APPEAR_IN ? 256
                         : (hp_t * 256) / HP_T_APPEAR_IN;
        hp_scene_sound(hp_t - HP_T_APPEAR);   /* flight time: -59..0 */
        if (hp_t >= HP_T_APPEAR) { hp_t = 0; hp_scene = HPS_FLY; }
        break;

    /* FLY - the camera on the three's centroid; each arrival flashes, puts
       the spirit out and starts its jewel. */
    case HPS_FLY:
        hp_spirit_bright = 256;
        hp_centroid(hp_t, &x, &y, &z);
        hp_aim(x, y, z, &rot, &pitch);
        cam_rot = rot; cam_pitch = pitch;
        hp_scene_sound(hp_t);
        for (i = 0; i < SP_COUNT; i++) {
            if (hp_t != hp_paths[i].arrive) continue;
            if (!(hp_dead & (1 << i))) continue;   /* no spirit, no flash */
            sound_play(hp_twinkle[i]);
            hp_jewel_t[i] = 0;
            hp_flash      = HP_FLASH_START;
            hp_flash_rgb  = hp_spirit_rgb(i);
        }
        if (hp_t >= HP_T_FLY) {
            hp_from_rot = cam_rot; hp_from_pitch = cam_pitch;
            hp_t = 0; hp_scene = HPS_SETTLE;
        }
        break;

    /* SETTLE - off the jewels' centroid onto the middle of the frame, so the
       whole structure is in shot. */
    case HPS_SETTLE: {
        int32_t s = hp_smooth(hp_t, HP_T_SETTLE);
        hp_aim(HP_FRAME_X, HP_FRAME_CY, HP_FRAME_Z, &rot, &pitch);
        cam_rot   = (hp_from_rot + ((hp_turn_delta(hp_from_rot, rot) * s) >> 12)) & 4095;
        cam_pitch = hp_lerp(hp_from_pitch, pitch, s);
        if (hp_t >= HP_T_SETTLE) {
            cam_rot = rot; cam_pitch = pitch;
            hp_t = 0; hp_scene = HPS_WAIT;
        }
        break;
    }

    /* WAIT - one second on it. Then the door, only if all three came. */
    case HPS_WAIT:
        if (hp_t >= HP_T_WAIT) {
            hp_t = 0;
            hp_scene = (hp_dead == HP_ALL_DEAD) ? HPS_DOOR : HPS_RETURN;
            hp_from_pitch = cam_pitch;
        }
        break;

    case HPS_DOOR:
        hp_door_lv = hp_t >= HP_T_DOOR ? 256 : (hp_t * 256) / HP_T_DOOR;
        if (hp_t >= HP_T_DOOR) { hp_t = 0; hp_scene = HPS_HOLD; }
        break;

    case HPS_HOLD:
        if (hp_t >= HP_T_HOLD) {
            hp_t = 0; hp_scene = HPS_RETURN;
            hp_from_pitch = cam_pitch;
        }
        break;

    /* RETURN - level the view, still facing the door, and hand it back. */
    case HPS_RETURN: {
        int32_t s = hp_smooth(hp_t, HP_T_RETURN);
        cam_pitch = hp_lerp(hp_from_pitch, 0, s);
        if (hp_t >= HP_T_RETURN) hp_scene_finish();
        break;
    }

    default:
        break;
    }
}

/* ---- The glows and the flash ------------------------------------------------
   World space, the view matrix up: from heros_plinth_draw, after the mesh and
   BEFORE the signs (door_draw_string_3d_yaw loads matrices of its own). */
static void hp_draw_glows(RenderContext *ctx) {
    int i;
    int32_t pulse = spirit_glow_pulse(hp_clock);

    for (i = 0; i < SP_COUNT; i++) {
        int32_t lv = hp_jewel_level(i);
        if (lv > 0)
            spirit_glow_draw_sized(ctx, HP_JEWEL_X, hp_paths[i].jy,
                                   hp_paths[i].jz, (lv * pulse) >> 8,
                                   hp_spirit_rgb(i), HP_JEWEL_GLOW);
    }

    /* The spirits in flight: the death scenes' ball, pulse and all. */
    if (hp_scene == HPS_APPEAR || hp_scene == HPS_FLY) {
        int32_t ft = hp_scene == HPS_FLY ? hp_t : 0;
        int32_t lv = (hp_spirit_bright * pulse) >> 8;
        if (lv > 256) lv = 256;
        for (i = 0; i < SP_COUNT; i++) {
            int32_t x, y, z;
            if (!(hp_dead & (1 << i))) continue;
            if (ft >= hp_paths[i].arrive) continue;      /* in its jewel */
            hp_spirit_pos(i, ft, &x, &y, &z);
            spirit_glow_draw(ctx, x, y, z, lv, hp_spirit_rgb(i));
        }
    }
}

static void hp_draw_flash(RenderContext *ctx) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    TILE *t;
    DR_TPAGE *tp;
    if (hp_flash <= 0 || !hp_flash_rgb) return;
    if (ctx->next_packet + sizeof(TILE) + sizeof(DR_TPAGE) > buf_end) return;
    t = (TILE *)ctx->next_packet;
    setTile(t);
    setSemiTrans(t, 1);
    setRGB0(t, (uint8_t)((hp_flash_rgb[0] * hp_flash) >> 8),
               (uint8_t)((hp_flash_rgb[1] * hp_flash) >> 8),
               (uint8_t)((hp_flash_rgb[2] * hp_flash) >> 8));
    setXY0(t, 0, 0);
    setWH(t, SCREEN_XRES, SCREEN_YRES);
    addPrim(&ot[SCENE_OT_MIN], t);
    ctx->next_packet += sizeof(TILE);
    tp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
    addPrim(&ot[SCENE_OT_MIN], tp);
    ctx->next_packet += sizeof(DR_TPAGE);
}

/* The frame's sign, the doors' fade. Nothing while the scene runs. */
static void hp_frame_text(RenderContext *ctx) {
    int mode = hp_frame_mode();
    int32_t dx = cam_x - HP_FRAME_X;
    int32_t dz = cam_z - HP_FRAME_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    int fade = 256;

    if (hp_scene != HPS_IDLE || mode == HP_FRAME_NONE) return;
    if (xz >= HP_TEXT_RADIUS) return;
    if (xz > HP_FADE_NEAR) {
        int range = HP_TEXT_RADIUS - HP_FADE_NEAR;
        int prog  = xz - HP_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }
    door_draw_string_3d_yaw(ctx, mode == HP_FRAME_ENTER
                                 ? "Press " BTN_CIRCLE " to enter"
                                 : "Press " BTN_CIRCLE " to interact",
                            HP_FRAME_TEXT_X, HP_FRAME_TEXT_Y, HP_FRAME_TEXT_Z,
                            50, 255, 50, fade, HP_FRAME_TEXT_YAW,
                            DOOR_PIXEL_SIZE);
}

void heros_plinth_init(void) {
    /* A scene a quit or a death left half-played does not survive the room. */
    heros_plinth_scene_reset();

    room_data_collision(&current_collision_room);
    collision_set_ceiling_y(HP_CEILING_Y);
    hp_floor_zones_init();

    /* The default arrival (main.c overrides it per door), and where a debug
       jump lands: the south-east door, from Sharur's Trial. */
    heros_plinth_spawn_se();

    /* The global prop arrays, as Wingscape clears them: Hell Hath No Fury's
       save point and tank would otherwise block invisibly inside this room's
       bounds. The clear drops the placement, never OD_TANK_HHNF's oil. */
    save_points_clear();
    dressers_clear();
    sconces_clear();
    oil_dispensers_clear();
    bars_clear();
    reset_particles();
}

/* ---- The mesh --------------------------------------------------------------
   draw_ws_smd() (src/wingscape.c) verbatim: the chapter's fog, set in
   src/underworld.h and changed by nothing, the key-point-and-radius side cull,
   and far primitives clamped one OT slot in front of the sky. */
static void draw_hp_smd(RenderContext *ctx) {
    if (!hp_smd) return;

    uint8_t *p = (uint8_t *)hp_smd->p_prims;
    int i, n = hp_key_count;

    /* HOISTED OUT OF THE LOOP (STEP 3C fix 1): none of these can change while
       a frame is being queued. */
    int32_t cull = DEBUG_CULL_DIST();
    if (!cull) cull = UW_FOG_FAR;
    int32_t fog_start = UW_FOG_NEAR < cull ? UW_FOG_NEAR : cull / 2;
    int32_t sn = isin(cam_rot), cs = icos(cam_rot);
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int32_t door_lv = hp_door_level();   /* 0 frame .. 256 Destiny's Door */

    /* ---- THE SIDE-PLANE CULL'S PER-FRAME TERMS ------------------------------
       Hell Hath No Fury's, unchanged: see draw_hhnf_smd for the derivation
       (the pitch bound, the 10*R radius term and the arithmetic). */
#define HP_SIDE_MARGIN  (8 << 12)
    int     no_frustum = (DEBUG_EXPERIMENT() == DBG_EXP_NO_FRUSTUM);
    int     pitched    = (cam_pitch != 0);
    int32_t pc = icos(cam_pitch);
    int32_t pslack = 0;
    if (pc < 0) no_frustum = 1;
    if (pitched) {
        int32_t ps   = isin(cam_pitch);
        int32_t dylo = hp_y_lo - cam_y, dyhi = hp_y_hi - cam_y;
        if (ps   < 0) ps   = -ps;
        if (dylo < 0) dylo = -dylo;
        if (dyhi < 0) dyhi = -dyhi;
        pslack = ps * (dylo > dyhi ? dylo : dyhi) + 512;
    }

    for (i = 0; i < n; i++) {
        /* >>> THE REJECT PATH READS cull_keys, NOT THE MESH. <<< */
        uint8_t stride = cull_keys[i].stride;
        {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > cull)
                { p += stride; continue; }
            if (dx * sn + dz * cs < -(700 << 12))
                { p += stride; continue; }
        }
        /* The side planes, off the key point and its radius. */
        if (!no_frustum && cull_keys[i].pad != 255) {
            int32_t dx = (int32_t)cull_keys[i].x - cam_x;
            int32_t dz = (int32_t)cull_keys[i].z - cam_z;
            int32_t f  = dx * sn + dz * cs;
            int32_t sd = dx * cs - dz * sn;
            int32_t r  = ((int32_t)cull_keys[i].pad * 40) << 12;   /* >= 10R */
            if (pitched) f = (((f >> 8) * pc) >> 4) + pslack;
            f = f * 5 + HP_SIDE_MARGIN + r;
            if (sd * 8 > f || -sd * 8 > f) { p += stride; continue; }
        }

        /* Survived both. NOW address the mesh. */
        SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
        int is_quad = (pt->type >= 2);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &hp_smd->p_verts[vi[0]];
        SVECTOR *v1 = &hp_smd->p_verts[vi[1]];
        SVECTOR *v2 = &hp_smd->p_verts[vi[2]];

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

        int nocull = (i < HEROS_PLINTH_PRIM_COUNT) && room_nocull(i);
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
            v3 = &hp_smd->p_verts[vi[3]];
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
        if (otz >= UW_SKY_OT) otz = UW_SKY_OT - 1;   /* in front of the sky */

        uint8_t *col = p + 16;
        int32_t face_cx = ((int32_t)v0->vx + v2->vx) / 2;
        int32_t face_cz = ((int32_t)v0->vz + v2->vz) / 2;
        int32_t dx = face_cx - cam_x;
        int32_t dz = face_cz - cam_z;
        int32_t dist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t fog = dist < fog_start ? fog_start : (dist > cull ? cull : dist);
        int32_t fog_factor = ((cull - fog) << 8) / (cull - fog_start);

        uint8_t tex_idx = (i < HEROS_PLINTH_PRIM_COUNT) ? room_tex_map[i] : 0xFF;
        int     textured = (tex_idx != 0xFF && tex_idx < HP_TEX_COUNT);
        uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + UW_FOG_R * (256 - fog_factor)) >> 8);
        uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + UW_FOG_G * (256 - fog_factor)) >> 8);
        uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + UW_FOG_B * (256 - fog_factor)) >> 8);

        /* DESTINY'S DOOR, over the frame's recessed quads: the door alone once
           it is open, the frame dimmed and the door ADDED while it fades up
           (frame*(1-a) + door*a). The door goes in FIRST - the slot's last
           addPrim draws first, so the frame lands and the door blends onto
           it. */
        if (is_quad && textured && door_lv > 0 && hp_is_recess(v0, v1, v2, v3)) {
            int solid = (door_lv >= 256);
            if (ctx->next_packet + 2 * sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            {
                POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
                setPolyFT4(poly);
                if (solid) {
                    setRGB0(poly, r, g, b);
                    poly->tpage = tex_tpage[HP_DOOR_SLOT];
                } else {
                    setSemiTrans(poly, 1);
                    setRGB0(poly, (uint8_t)((r * door_lv) >> 8),
                                  (uint8_t)((g * door_lv) >> 8),
                                  (uint8_t)((b * door_lv) >> 8));
                    poly->tpage = (uint16_t)((tex_tpage[HP_DOOR_SLOT] & ~(3 << 5)) |
                                             (1 << 5));   /* ABR 1: B + F */
                }
                poly->clut = tex_clut[HP_DOOR_SLOT];
                hp_door_uv(v0, &poly->u0, &poly->v0);
                hp_door_uv(v1, &poly->u1, &poly->v1);
                hp_door_uv(v2, &poly->u2, &poly->v2);
                hp_door_uv(v3, &poly->u3, &poly->v3);
                poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
                poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
                poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
                poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
                addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
                ctx->next_packet += sizeof(POLY_FT4);
            }
            if (solid) { p += stride; continue; }
            /* ...and the frame under it, dimmed by what the door has taken. */
            r = (uint8_t)((r * (256 - door_lv)) >> 8);
            g = (uint8_t)((g * (256 - door_lv)) >> 8);
            b = (uint8_t)((b * (256 - door_lv)) >> 8);
        }

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

void heros_plinth_draw(RenderContext *ctx) {
    int exp = DEBUG_EXPERIMENT();

    /* THE CHAPTER'S FOG, and nothing else's (src/underworld.h). */
    g_fog_near = UW_FOG_NEAR;
    g_fog_far  = DEBUG_CULL_DIST() ? DEBUG_CULL_DIST() : UW_FOG_FAR;

    /* The clear colour, not a full-screen TILE (wrong turn #3). The sky is an
       open ring, so this is what shows above and below it. */
    render_set_clear_colour(ctx, UW_FOG_R, UW_FOG_G, UW_FOG_B);

    /* 128x128 texture window so per-poly UVs wrap within each texture's page.
       All nine room textures and the sky's sit at Voff 0; no sprite is drawn
       here. */
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
        underworld_sky_draw(ctx, HP_CX, HP_SKY_Y, HP_CZ);
        draw_hp_smd(ctx);
    }

    /* >>> LEVEL 8 IS THE JEWELS, THE SPIRITS AND THE THREE SIGNS. <<< The
       glows first, while the view matrix is still the room's: the signs load
       matrices of their own. The flash is screen space and goes anywhere. */
    if (exp != DBG_EXP_NO_ENTITIES) {
        hp_draw_glows(ctx);
        hp_draw_flash(ctx);
        hp_frame_text(ctx);
        hp_door_text(ctx, HP_NE_X, HP_NE_Z, HP_NE_TEXT_X, HP_NE_TEXT_Z,
                     HP_NE_TEXT_YAW);
        hp_door_text(ctx, HP_SE_X, HP_SE_Z, HP_SE_TEXT_X, HP_SE_TEXT_Z,
                     HP_SE_TEXT_YAW);
    }
}
