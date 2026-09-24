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
#include "player.h"         /* player_items/_keys/_ammo/_hatch_keys */
#include "key.h"            /* KEY_FRONT_DOOR — the one key that is a bit  */
#include "menu.h"           /* MENU_SLOT_*, menu_item_held */
#include "sound.h"          /* SFX_MCHNE, SFX_GAS, SND_BANK_CATACOMBS */
#include "particles.h"      /* spawn_soot_spurt: the vent at cycle end   */
#include "incinerator.h"

/* The Incinerator — see incinerator.h for what it is, for why its collision
   comes out of its own mesh, and for why the mesh is in ROOM WORLD COORDINATES
   and is therefore placed at the origin. */

typedef struct {
    GameState area;                        /* only draws/collides in this room */
    int32_t   x, y, z, rot_y;              /* world y = y + GROUND_FLOOR_Y     */
    int32_t   min_x, max_x, min_z, max_z;  /* world AABB, baked at place time  */
    int       active;
} Incinerator;

/* A SINGLETON, not an array — incinerator.h's hopper note says why a stateful
   prop that is re-placed on every room entry cannot carry its state per
   instance, and there is exactly one of these in the game. */
static Incinerator inc;

static SMD  *inc_smd = NULL;
static void *inc_buf = NULL;

/* The collision mesh, and it is the SAME mesh the draw uses: these six are read
   off inc_smd's vertex array at load and are the only description of the
   machine's solid volume anywhere in the game. MODEL SPACE, and signed. Because
   this model is authored in room coordinates, model space IS room space and
   these are the machine's real extents in the hall. */
static int32_t in_min_x = 0, in_max_x = 0;   /* -2000 .. -1   as authored */
static int32_t in_min_z = 0, in_max_z = 0;   /* -3150 .. -2550           */
static int32_t in_min_y = 0, in_max_y = 0;   /*  -400 .. 0, -Y is up     */

static int inc_tex = -1;

/* ---- THE FURNACE GLOW ------------------------------------------------------
   The two openings in the machine's middle - one facing each way across the
   central gap, at x=-1800 and x=-200 - are the only faces that show the orange
   furnace art, and they get an additive halo laid over them so the fire spills
   onto the plating around the hole.

   >>> WHICH FACES THEY ARE IS DERIVED FROM THE UVs AT LOAD, NOT WRITTEN DOWN AS
   {87, 89}. <<< src/asag.c's glow mask makes this argument at length and it
   holds here for the same reason: a re-export renumbers primitives freely, and
   a stale index pair would light two polygons somewhere else on the body - the
   kind of wrong that reads as a corrupt mesh. It has already paid off once, in
   the re-export that shortened the machine from 400 tall to 300.

   THE TEST IS "V WRAPS PAST THE BOTTOM OF THE SHEET, ON A BIG FACE". The orange
   is the top-left quadrant of incinerator.png, and these two faces reach it by
   running their V off the bottom edge and round - v[126,188] and v[124,188],
   which the 128 texture window wraps to a two-row sliver of the bottom followed
   by the top 60 rows. Nothing else on the model does that AND is big: the only
   other face whose V crosses 128 is a conveyor edge 24 texels wide, so the span
   floor of 48 separates them with room to spare (checked against the export,
   not assumed).

   IF THE ART IS EVER REPAINTED so the furnace is somewhere else on the sheet,
   this is the one rule to move - the same single-rectangle promise asag.c
   makes. */
#define IN_GLOW_V_WRAP   128   /* the V the two openings straddle          */
#define IN_GLOW_MIN_SPAN  48   /* texels, both axes: "a big face"          */

/* One bit per primitive. 107 today; four words is 128 and the loop that fills
   it stops at the array's own size, so a bigger mesh loses the glow rather than
   scribbling past the end. */
#define IN_GLOW_WORDS  4
static uint32_t glow_mask[IN_GLOW_WORDS];

/* ---- THE VENT ---------------------------------------------------------------
   Where the soot comes out at the end of a cycle: the mouth of the furnace
   opening at the EAST end of the body, which is the one on the same end of the
   machine as the button (550 away in plan, against 1650 for the west one).

   MEASURED, NOT WRITTEN DOWN, for the glow mask's reason one comment up. Both
   openings' centres are averaged out of their own vertices in the same load
   pass, and the vent is whichever of them has the greater X. That survives the
   machine being resized or moved, which the numbers in this file have already
   had to survive once.

   THE DIRECTION IS +X BY CONSTRUCTION, because the east opening's normal is
   (+1,0,0) - it faces out along the body, over the east conveyor tray. It is
   not read off the mesh because a normal that came back the other way would
   mean the model had been MIRRORED, and the glow, the trays and both signs
   would all need looking at rather than just this line. */
static int32_t vent_x, vent_y, vent_z;
static int     vent_found;

/* The halo's colour and size. Additive, so these are what gets ADDED to the
   plating around the hole rather than what it is painted: a saturated orange
   with the blue crushed to nothing, because additive blue on a dark grey wall
   reads as haze rather than as fire.

   The light is drawn as FOUR BANDS projecting outward from the opening's four
   edges and fading to black - see inc_glow_frame, which also records why the
   first version (one quad laid over the face, the Helluminator's) was wrong for
   a hole in a wall. Nothing is drawn over the opening itself; these three
   channels are what the band carries at the edge it leaves. */
#define IN_GLOW_R      255
#define IN_GLOW_G       96
#define IN_GLOW_B       16
/* HOW FAR THE LIGHT REACHES past the opening's edge, in 256ths of the face's
   own half-size. The opening is 200 x 150, so half-sizes are 100 and 75: at 360
   the band is 39 units wide across and 29 tall, at 512 it is 100 and 75. Those
   are the numbers to move to make the spill bigger, and they mean something
   here that they did not when this was an overlay - then they set how much of
   the face was covered, now they set how far the light throws. */
#define IN_GLOW_SCALE  360

/* ---- THE BURN -------------------------------------------------------------
   While the machine is running the furnace swells and goes RED, and the shape
   of it is src/helluminator.c's burn glow: a ramp counter that climbs while the
   thing is lit and falls when it is not, with the scale and the colour lerped
   across it, so the light SWELLS instead of snapping between two states.

   >>> REDDER IS DONE BY TAKING GREEN AND BLUE AWAY, NOT BY ADDING RED. <<<
   This is the Helluminator's rule and it is forced by the same arithmetic: the
   halo is ADDITIVE, red is already at 255 in the idle colour above, and a
   channel scaled up would clip against whatever is behind it while a channel
   scaled down cannot. So the tint drains G and B and the fire reddens without
   the halo losing a single unit of the brightness it already had.

   INTENSITY IS THE OTHER TWO KNOBS: a bigger quad (332 -> 470, i.e. 1.3x the
   opening to 1.84x) and a flicker that rides higher and swings less. A furnace
   under load does not gutter the way an idle one does, so the amplitude coming
   DOWN as the base goes UP is part of the read rather than a side effect.

   14 FRAMES, a little slower than the lantern's 10: that one answers a button
   the player is holding and has to feel instant, this one is a machine
   spinning up and is allowed to take a quarter of a second. */
#define IN_BURN_RAMP          14
#define IN_GLOW_SCALE_BURN   512
#define IN_BURN_TINT_G       110   /* of 256, at full burn */
#define IN_BURN_TINT_B        48
/* Blue and green come down; red has no constant, because nothing may scale it. */

/* One channel's 256ths tint, lerped from untinted (256) to TINT across the
   ramp. Reads burn_ramp from the caller's scope, exactly as HELL_TINT does. */
#define IN_BURN_TINT(TINT) \
    (256 + (((TINT) - 256) * burn_ramp) / IN_BURN_RAMP)

/* The flicker's base and half-amplitude at each end of the ramp. Idle wanders
   103..255; full burn wanders 208..255. */
#define IN_FLICK_BASE_IDLE   179
#define IN_FLICK_AMP_IDLE     76
#define IN_FLICK_BASE_BURN   232
#define IN_FLICK_AMP_BURN     24

static int32_t burn_ramp = 0;   /* 0..IN_BURN_RAMP */

/* THE FLICKER. A furnace that glowed at one level would read as a painted-on
   texture, which is asag.c's argument for its own pulse. This is a fire, not a
   lamp, so it is not a clean sine: two counters of different (coprime) periods
   summed give a level that wanders without ever repeating on a beat the eye can
   catch. Advanced by the DRAW, so it keeps moving while the machine is idle and
   there is no update to skip. */
static uint8_t glow_ph1, glow_ph2;

/* The player's head, relative to cam_y — the same figure apply_collision_* uses
   for its own body span, so the vertical test below agrees with the walls'.
   Feet are cam_y + GROUND_FLOOR_Y. */
#define IN_PLAYER_HEAD 30

/* ---- THE HOPPER ----------------------------------------------------------- */

static int hopper_slot  = -1;   /* MENU_SLOT_* or -1 */
static int hopper_count =  0;   /* rounds, or 1 for a plain item */

int incinerator_slot(void)  { return hopper_slot; }
int incinerator_count(void) { return hopper_count; }

void incinerator_reset(void) { hopper_slot = -1; hopper_count = 0; }

void incinerator_set_stored(int slot, int count) {
    /* Clamped on the way in exactly as savegame.c clamps the oil, and for the
       same reason: a corrupt slot would index menu_item_name() off the end and
       a corrupt count would hand the player rounds out of nothing. */
    if (slot < 0 || slot >= MENU_ITEM_SLOTS) { incinerator_reset(); return; }
    if (count < 1) { incinerator_reset(); return; }
    if (count > INC_AMMO_MAX) count = INC_AMMO_MAX;
    hopper_slot  = slot;
    hopper_count = count;
}

/* The three counted slots, and everything else is a bit. Kept as one switch in
   each direction rather than a table, because menu_item_held() next door is the
   same switch and the two have to agree about what "holding one" means — a
   table would let them drift. */
static int slot_take(int slot) {
    switch (slot) {
    case MENU_SLOT_ROUNDS: {
        int n = player_ammo[AMMO_STANDARD];
        if (n <= 0) return 0;
        if (n > INC_AMMO_MAX) n = INC_AMMO_MAX;
        player_ammo[AMMO_STANDARD] -= n;
        return n;
    }
    case MENU_SLOT_FLAME_ROUNDS: {
        int n = player_ammo[AMMO_FLAME];
        if (n <= 0) return 0;
        if (n > INC_AMMO_MAX) n = INC_AMMO_MAX;
        player_ammo[AMMO_FLAME] -= n;
        return n;
    }
    case MENU_SLOT_HATCH_KEY:
        /* Counted like ammo, but ONE — a hatch key is a thing, not a quantity,
           and the door it opens wants a specific number of them. */
        if (player_hatch_keys <= 0) return 0;
        player_hatch_keys--;
        return 1;
    case MENU_SLOT_FRONT_DOOR_KEY:
        if (!(player_keys & (1 << KEY_FRONT_DOOR))) return 0;
        player_keys &= ~(1 << KEY_FRONT_DOOR);
        return 1;
    case MENU_SLOT_COPPER_POT:        if (!(player_items & (1 << ITEM_COPPER_POT)))        return 0; player_items &= ~(1 << ITEM_COPPER_POT);        return 1;
    case MENU_SLOT_WAX_CUBE:          if (!(player_items & (1 << ITEM_WAX_CUBE)))          return 0; player_items &= ~(1 << ITEM_WAX_CUBE);          return 1;
    case MENU_SLOT_GREEN_KEY_STONE:   if (!(player_items & (1 << ITEM_GREEN_KEY_STONE)))   return 0; player_items &= ~(1 << ITEM_GREEN_KEY_STONE);   return 1;
    case MENU_SLOT_PIANO_KEY:         if (!(player_items & (1 << ITEM_PIANO_KEY)))         return 0; player_items &= ~(1 << ITEM_PIANO_KEY);         return 1;
    case MENU_SLOT_BLUE_KEY_STONE:    if (!(player_items & (1 << ITEM_BLUE_KEY_STONE)))    return 0; player_items &= ~(1 << ITEM_BLUE_KEY_STONE);    return 1;
    case MENU_SLOT_YELLOW_KEY_STONE:  if (!(player_items & (1 << ITEM_YELLOW_KEY_STONE)))  return 0; player_items &= ~(1 << ITEM_YELLOW_KEY_STONE);  return 1;
    case MENU_SLOT_MAGENTA_KEY_STONE: if (!(player_items & (1 << ITEM_MAGENTA_KEY_STONE))) return 0; player_items &= ~(1 << ITEM_MAGENTA_KEY_STONE); return 1;
    case MENU_SLOT_VALVE_HANDLE:      if (!(player_items & (1 << ITEM_VALVE_HANDLE)))      return 0; player_items &= ~(1 << ITEM_VALVE_HANDLE);      return 1;
    default: return 0;
    }
}

static void slot_give(int slot, int count) {
    switch (slot) {
    case MENU_SLOT_ROUNDS:            player_ammo[AMMO_STANDARD] += count; break;
    case MENU_SLOT_FLAME_ROUNDS:      player_ammo[AMMO_FLAME]    += count; break;
    case MENU_SLOT_HATCH_KEY:         player_hatch_keys          += count; break;
    case MENU_SLOT_FRONT_DOOR_KEY:    player_keys  |= (1 << KEY_FRONT_DOOR);        break;
    case MENU_SLOT_COPPER_POT:        player_items |= (1 << ITEM_COPPER_POT);       break;
    case MENU_SLOT_WAX_CUBE:          player_items |= (1 << ITEM_WAX_CUBE);         break;
    case MENU_SLOT_GREEN_KEY_STONE:   player_items |= (1 << ITEM_GREEN_KEY_STONE);  break;
    case MENU_SLOT_PIANO_KEY:         player_items |= (1 << ITEM_PIANO_KEY);        break;
    case MENU_SLOT_BLUE_KEY_STONE:    player_items |= (1 << ITEM_BLUE_KEY_STONE);   break;
    case MENU_SLOT_YELLOW_KEY_STONE:  player_items |= (1 << ITEM_YELLOW_KEY_STONE); break;
    case MENU_SLOT_MAGENTA_KEY_STONE: player_items |= (1 << ITEM_MAGENTA_KEY_STONE);break;
    case MENU_SLOT_VALVE_HANDLE:      player_items |= (1 << ITEM_VALVE_HANDLE);     break;
    default: break;
    }
}

int incinerator_store(int slot) {
    int n;
    if (hopper_slot >= 0) return 0;              /* one thing at a time */
    if (slot < 0 || slot >= MENU_ITEM_SLOTS) return 0;
    if (!menu_item_held(slot)) return 0;
    n = slot_take(slot);
    if (n <= 0) return 0;
    hopper_slot  = slot;
    hopper_count = n;
    return n;
}

int incinerator_retrieve(void) {
    int slot = hopper_slot;
    if (slot < 0) return -1;
    slot_give(slot, hopper_count);
    incinerator_reset();
    return slot;
}

/* ---- THE BUTTON -----------------------------------------------------------
   TWO plays of the grind, then the gas as the log line lands. The counter runs
   DOWN from IN_CYCLE_FRAMES so the fire points are a plain equality test on the
   way through rather than a modulo, which is how src/chainlink_door.c fires its
   two.

   5.6 SECONDS, DOWN FROM 8.4. It was three grinds; a third play added length
   without adding information, and the wait was the one thing about this button
   that read as the game having stopped rather than the machine having started.

   >>> THE GAS IS THE FULL STOP AND IT IS ON THE SAME FRAME AS THE MESSAGE. <<<
   It fires at cycle_t == 0, the frame incinerator_cycle_finished() latches, so
   the hiss and the line in the log are one event rather than two things that
   happen to be close. That is the whole reason the sound lives here and not
   beside the wording in the room: only this function knows which frame is the
   last one.

   SFX_GAS IS THE RAFFLESIA'S SPORE PUFF, BORROWED. It is 0.80 s of pressurised
   hiss, which is what the machine venting sounds like, and reusing it cost one
   bank bit instead of a new clip in an SPU that has 6.5 KB spare
   (tools/ADDING_A_SOUND.txt). See the note in src/sound.c about the voice it
   shares. */
#define IN_CLIP_FRAMES   168   /* mchne.vag: 2.80 s at 60 fps */
#define IN_CYCLE_FRAMES  (IN_CLIP_FRAMES * 2)

static int32_t cycle_t   = 0;   /* frames left; 0 = idle */
static int     cycle_end = 0;   /* latched for one frame when it finishes */

int incinerator_cycle_active(void) { return cycle_t > 0; }

int incinerator_cycle_finished(void) {
    int f = cycle_end;
    cycle_end = 0;
    return f;
}

IncPress incinerator_button_press(void) {
    IncPress r;
    if (cycle_t > 0) return INC_PRESS_IGNORED;   /* already running */

    /* >>> THE ITEM IS CONSUMED HERE, ON THE PRESS. <<< incinerator.h says why:
       the press is the decision, the cycle is the machine carrying it out. The
       branch is taken BEFORE the hopper is cleared, so the caller is told which
       line to print eight seconds later without having to remember what was in
       it. */
    r = (hopper_slot >= 0) ? INC_PRESS_BURNED : INC_PRESS_EMPTY;
    if (r == INC_PRESS_BURNED) incinerator_reset();

    cycle_t   = IN_CYCLE_FRAMES;
    cycle_end = 0;
    sound_play(SFX_MCHNE);   /* first of two; the second and the gas are below */
    return r;
}

void incinerator_cycle_update(void) {
    /* THE BURN RAMP RUNS WHETHER OR NOT A CYCLE IS GOING, which is why it is
       above the early return: it has to be able to fall back as well as swell,
       and the frames it falls over are by definition frames with no cycle. The
       lantern's runs in its own update for the same reason. */
    if (cycle_t > 0) { if (burn_ramp < IN_BURN_RAMP) burn_ramp++; }
    else             { if (burn_ramp > 0)            burn_ramp--; }

    if (cycle_t <= 0) return;
    cycle_t--;
    /* The second grind, as the first clip runs out. SND_BANK_CATACOMBS carries
       SFX_MCHNE now (src/sound.c) - before this prop it was HOUSE-only and
       would have been silent down here, and the same is true of SFX_GAS. */
    if (cycle_t == IN_CLIP_FRAMES) sound_play(SFX_MCHNE);
    if (cycle_t == 0) {
        sound_play(SFX_GAS);   /* on the latch frame, with the log line */
        /* ...and the soot, out of the east opening, on that same frame. The
           vent, the hiss and the message are one event; that is the whole
           argument for the sound living here rather than beside the wording in
           the room, and it applies to this exactly as much.

           MODEL SPACE -> WORLD. rot_y is 0 for this prop (incinerator.h says
           why), so the plan needs no rotation and only the placement offset
           applies; Y picks up GROUND_FLOOR_Y the way the draw's does. Guarded
           on vent_found so a mesh with no opening in it vents nothing rather
           than puffing soot out of the room's origin. */
        if (vent_found && inc.active)
            spawn_soot_spurt(inc.x + vent_x,
                             inc.y + GROUND_FLOOR_Y + vent_y,
                             inc.z + vent_z,
                             1, 0);
        cycle_end = 1;
    }
}

/* ---- Assets ---------------------------------------------------------------- */

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
void incinerator_load_assets(void) {
    /* BANK: Chapter 3 and nothing else. Derived, not guessed — py
       tools/check_tex_banks.py walks the uploader call graph (this module is
       reached from incinerator_room_upload_textures) and fails the build if this
       mask is short. */
    texmgr_set_bank(TEXBANK_CATACOMBS);

    inc_buf = read_file("\\TEXCTCMB\\INCINPRP.SMD;1");
    if (inc_buf) inc_smd = smdInitData(inc_buf);

    /* MEASURE THE MESH. This is the whole of the prop's collision authoring:
       the box below is the model's real bounding volume, so it can never drift
       from what is drawn. No symmetry is assumed — see the header. */
    if (inc_smd && inc_smd->n_verts > 0) {
        int i;
        in_min_x = in_max_x = inc_smd->p_verts[0].vx;
        in_min_y = in_max_y = inc_smd->p_verts[0].vy;
        in_min_z = in_max_z = inc_smd->p_verts[0].vz;
        for (i = 1; i < inc_smd->n_verts; i++) {
            int32_t vx = inc_smd->p_verts[i].vx;
            int32_t vy = inc_smd->p_verts[i].vy;
            int32_t vz = inc_smd->p_verts[i].vz;
            if (vx < in_min_x) in_min_x = vx;
            if (vx > in_max_x) in_max_x = vx;
            if (vy < in_min_y) in_min_y = vy;
            if (vy > in_max_y) in_max_y = vy;
            if (vz < in_min_z) in_min_z = vz;
            if (vz > in_max_z) in_max_z = vz;
        }
    }

    /* MARK THE FURNACE OPENINGS, off the UVs, on the rule set out above. One
       pass over 107 primitives at load and nothing per frame - asag.c's
       arrangement exactly. */
    {
        int k;
        for (k = 0; k < IN_GLOW_WORDS; k++) glow_mask[k] = 0;
        if (inc_smd) {
            uint8_t *gp = (uint8_t *)inc_smd->p_prims;
            for (k = 0; k < inc_smd->n_prims; k++) {
                SMD_PRI_TYPE *pt      = (SMD_PRI_TYPE *)gp;
                int           corners = (pt->type >= 2) ? 4 : 3;
                uint8_t      *uv      = gp + 20;
                int           c, ulo, uhi, vlo, vhi;
                if (k >= IN_GLOW_WORDS * 32) break;
                /* Untextured faces have no UVs to read and can never be the
                   opening; skipping them also keeps whatever sits at +20 on a
                   flat primitive out of the test. */
                if (!pt->texture) { gp += pt->len; continue; }
                ulo = uhi = uv[0];
                vlo = vhi = uv[1];
                for (c = 1; c < corners; c++) {
                    int u = uv[c * 2], v = uv[c * 2 + 1];
                    if (u < ulo) ulo = u;
                    if (u > uhi) uhi = u;
                    if (v < vlo) vlo = v;
                    if (v > vhi) vhi = v;
                }
                if (vlo < IN_GLOW_V_WRAP && vhi > IN_GLOW_V_WRAP &&
                    (uhi - ulo) >= IN_GLOW_MIN_SPAN &&
                    (vhi - vlo) >= IN_GLOW_MIN_SPAN) {
                    uint16_t *vi = (uint16_t *)(gp + 4);
                    int32_t   ax = 0, ay = 0, az = 0;
                    glow_mask[k >> 5] |= 1u << (k & 31);
                    /* ...and average this opening's own vertices for its mouth.
                       The vent is the EASTERNMOST of them - see THE VENT. */
                    for (c = 0; c < corners; c++) {
                        ax += inc_smd->p_verts[vi[c]].vx;
                        ay += inc_smd->p_verts[vi[c]].vy;
                        az += inc_smd->p_verts[vi[c]].vz;
                    }
                    ax /= corners; ay /= corners; az /= corners;
                    if (!vent_found || ax > vent_x) {
                        vent_x = ax; vent_y = ay; vent_z = az;
                        vent_found = 1;
                    }
                }
                gp += pt->len;
            }
        }
    }

    inc_tex = texmgr_register("\\TEXCTCMB\\INCINPRP.TIM;1");
}

/* Room entry: pure LoadImage out of the RAM copy area_bank_sync() has already
   read. No CD access, so it is safe inside main's STATE_LOADING. Called from
   incinerator_room_upload_textures(). */
void incinerator_upload_texture(void) {
    texmgr_upload(inc_tex);
}

/* Drop the placed instance — AND STOP THE MACHINE. The cycle is a room-local
   animation, not a piece of world state: it is fired by a button the player can
   only reach in this room, and leaving mid-cycle has to stop it or the finish
   would latch in whatever room they walked into and put "the item was
   destroyed!" in the log there. The HOPPER is untouched, because that is the
   player's property and survives everything (incinerator.h). */
void incinerator_clear(void) {
    inc.active = 0;
    cycle_t    = 0;
    cycle_end  = 0;
    /* The swell goes with the cycle that caused it. Left standing, a player who
       walked out mid-cycle would walk back into a furnace glowing at full burn
       with nothing running, and it would stay that way until the ramp happened
       to be ticked down - which incinerator_cycle_update() only does while the
       prop is in the room. */
    burn_ramp  = 0;
}

void incinerator_place(GameState area, int32_t x, int32_t y, int32_t z,
                       int32_t rot_y) {
    int32_t c, sn;
    int k;
    const int32_t lx[4] = { in_min_x, in_max_x, in_max_x, in_min_x };
    const int32_t lz[4] = { in_min_z, in_min_z, in_max_z, in_max_z };

    inc.area  = area;
    inc.x = x;  inc.y = y;  inc.z = z;
    inc.rot_y = rot_y;
    inc.active = 1;

    /* World AABB = the axis-aligned bound of the rotated mesh footprint, corner
       by corner, exactly as the oil dispenser and the lever bake theirs. For
       THIS prop rot_y is always 0 and the four corners collapse to the measured
       box unchanged (incinerator.h says why rotating it is meaningless) — the
       arithmetic is kept because it is the shared idiom, and dropping it would
       make this the one placement in the game that silently ignores its angle. */
    c = icos(rot_y); sn = isin(rot_y);
    for (k = 0; k < 4; k++) {
        /* Same handedness as the RotMatrix Y rotation the draw uses. */
        int32_t wx = x + ((lx[k] * c + lz[k] * sn) >> 12);
        int32_t wz = z + ((lz[k] * c - lx[k] * sn) >> 12);
        if (k == 0) {
            inc.min_x = inc.max_x = wx;
            inc.min_z = inc.max_z = wz;
        } else {
            if (wx < inc.min_x) inc.min_x = wx;
            if (wx > inc.max_x) inc.max_x = wx;
            if (wz < inc.min_z) inc.min_z = wz;
            if (wz > inc.max_z) inc.max_z = wz;
        }
    }
}

/* Player push-out against the baked box, Minkowski-expanded by the caller's
   radius and resolved along the shallowest axis — the dresser's scheme. Area-
   gated, so the shared collision routine calls it unconditionally. */
void incinerator_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int32_t floor_y, solid_bot, solid_top, feet, head;
    int32_t min_x, max_x, min_z, max_z, pl, pr, pf, pb, m, ddx, ddz;

    if (!inc.active || inc.area != current_area) return;

    /* Vertical gate, and it too comes out of the mesh. -Y is up, so the
       machine's TOP is in_min_y and its base in_max_y, both offsets from the
       floor it was placed on. Unlike the oil dispenser's tank this prop sits ON
       its floor, so the test never opens — but it is what stops a 400-tall
       machine being solid up into the y=-800 vaulting, and it is what would let
       a later re-export raise the body onto legs without a code change. */
    floor_y   = inc.y + GROUND_FLOOR_Y;
    solid_bot = floor_y + in_max_y;   /* its base */
    solid_top = floor_y + in_min_y;   /* its top  */
    feet = py + GROUND_FLOOR_Y; head = py - IN_PLAYER_HEAD;
    if (head >= solid_bot || feet <= solid_top) return;

    min_x = inc.min_x - radius; max_x = inc.max_x + radius;
    min_z = inc.min_z - radius; max_z = inc.max_z + radius;
    if (*px <= min_x || *px >= max_x) return;
    if (*pz <= min_z || *pz >= max_z) return;

    pl = *px - min_x; pr = max_x - *px;
    pf = *pz - min_z; pb = max_z - *pz;
    m = pl; ddx = -pl; ddz = 0;
    if (pr < m) { m = pr; ddx =  pr; ddz = 0; }
    if (pf < m) { m = pf; ddx = 0; ddz = -pf; }
    if (pb < m) {         ddx = 0; ddz =  pb; }
    *px += ddx; *pz += ddz;
}

/* The furnace light, as FOUR quads projected OUT from the four edges of the
   opening - one per edge - each fading from the fire's colour at the edge it
   leaves to black at its outer end.

   >>> IT USED TO BE ONE QUAD LAID OVER THE OPENING AND THAT WAS WRONG. <<<
   The first version was src/helluminator.c's hell_glow_quad: the opening's own
   four screen points grown about their centre, drawn additive on top of the
   face. On a lamp held in the hand that reads as bloom, because the thing
   glowing is small, bright and moving. On a hole in a wall it read as exactly
   what it was - a semi-transparent panel hanging in front of the opening, in
   the same plane, dimming the art behind it instead of lighting the plating
   around it.

   SO NOTHING IS DRAWN OVER THE OPENING ANY MORE. The face keeps its own texture
   untouched and the light lives entirely OUTSIDE it, in the band between the
   opening's edge and `scale`. That is the difference between a filter over a
   hole and a hole with a fire in it.

   THE GRADIENT IS THE WHOLE EFFECT and it is why these are POLY_G4 rather than
   POLY_F4: the two vertices on the opening's edge carry the full colour, the
   two on the outer edge carry black, so each band falls off across its own
   width. src/outside_catacombs.c makes the same move for the same reason - its
   doorway backing is a gouraud rim because "the four corners of a backing quad
   do not share a colour and POLY_F4 cannot draw it". Additive, so black at the
   outer end contributes nothing at all and the band needs no mask to end on.

   THE PERIMETER ORDER IS 0-1-3-2, NOT 0-1-2-3. A PS1 quad is two triangles,
   (v0,v1,v2) and (v1,v2,v3), so v2 and v3 are the FAR pair and walking the
   corners in index order would cross the middle of the face - producing a
   bow-tie band across the opening rather than a frame around it. The edge list
   below is the real perimeter.

   Each band's own four points are laid out in the same two-triangle order:
   inner a, inner b, outer a, outer b. */
#define IN_GLOW_EDGES 4
static const uint8_t in_edge_a[IN_GLOW_EDGES] = { 0, 1, 3, 2 };
static const uint8_t in_edge_b[IN_GLOW_EDGES] = { 1, 3, 2, 0 };

static void inc_glow_frame(RenderContext *ctx, const DVECTOR *sv, int32_t scale,
                           int lr, int lg, int lb, int otz)
{
    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;
    DR_TPAGE *tp;
    int32_t   cx, cy;
    int16_t   ox[4], oy[4];
    int       k, drew = 0;

    /* The outer ring: every corner pushed out from the face's own screen centre.
       Computed once for all four bands, because neighbouring bands must share
       the corner between them exactly or the frame opens at its corners. */
    cx = ((int32_t)sv[0].vx + sv[1].vx + sv[2].vx + sv[3].vx) / 4;
    cy = ((int32_t)sv[0].vy + sv[1].vy + sv[2].vy + sv[3].vy) / 4;
    for (k = 0; k < 4; k++) {
        int32_t x = cx + (((int32_t)sv[k].vx - cx) * scale >> 8);
        int32_t y = cy + (((int32_t)sv[k].vy - cy) * scale >> 8);
        /* The GPU's coordinate limit, the same clamp the room loops apply. A
           band grown past it would wrap rather than clip. */
        if (x < -1023) x = -1023;
        if (x >  1023) x =  1023;
        if (y < -1023) y = -1023;
        if (y >  1023) y =  1023;
        ox[k] = (int16_t)x; oy[k] = (int16_t)y;
    }

    for (k = 0; k < IN_GLOW_EDGES; k++) {
        int      a = in_edge_a[k], b = in_edge_b[k];
        POLY_G4 *poly;

        if (ctx->next_packet + sizeof(POLY_G4) > buf_end) break;
        poly = (POLY_G4 *)ctx->next_packet;
        setPolyG4(poly);
        setSemiTrans(poly, 1);
        /* Inner pair lit, outer pair black - the fall-off. */
        setRGB0(poly, (uint8_t)lr, (uint8_t)lg, (uint8_t)lb);
        setRGB1(poly, (uint8_t)lr, (uint8_t)lg, (uint8_t)lb);
        setRGB2(poly, 0, 0, 0);
        setRGB3(poly, 0, 0, 0);
        poly->x0 = sv[a].vx; poly->y0 = sv[a].vy;   /* on the opening's edge */
        poly->x1 = sv[b].vx; poly->y1 = sv[b].vy;
        poly->x2 = ox[a];    poly->y2 = oy[a];      /* out on the plating    */
        poly->x3 = ox[b];    poly->y3 = oy[b];
        addPrim(&ot[otz], poly);
        ctx->next_packet += sizeof(POLY_G4);
        drew = 1;
    }

    /* ADDITIVE MEANS THE DR_TPAGE GOES IN AFTER THE BANDS. The OT is LIFO
       within a bucket, so the primitive added LAST is processed FIRST; the
       page-select has to be processed before the quads it applies to. One page
       select covers all four, which is the other reason they are queued as a
       group. Skipped entirely if the packet budget stopped us, so a TPAGE is
       never left switching blend modes for somebody else's primitives.

       >>> AND NOTHING RESTORES THE BLEND MODE AFTERWARDS. <<< That is the house
       pattern (src/lightswitch_puzzle.c's light cones, src/helluminator.c's
       lamp) and it is safe because a DR_TPAGE only persists until the next
       primitive that carries its own `tpage` field - which every TEXTURED prim
       does. Checked for this room rather than assumed: all 708 polys of the
       Incinerator Room's mesh and all 107 of this prop's are textured, so there
       is no untextured primitive anywhere in here for the additive mode to leak
       onto. Add a flat-shaded face to either mesh and this needs a second
       DR_TPAGE, queued BEFORE the bands, putting ABR back. */
    if (drew && ctx->next_packet + sizeof(DR_TPAGE) <= buf_end) {
        tp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
        addPrim(&ot[otz], tp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
}

/* Render it. Textured-prim path with per-poly UVs from the SMD (the model is one
   texture, so the tpage/clut are the same for every face and there is no tex map
   to keep in step). The untextured branches are kept because the loop branches
   on the primitive's own texture bit, which costs nothing and survives a
   re-export that leaves a flat face behind.

   THE CALLER OWNS THE 128 TEXTURE WINDOW and this prop NEEDS it. incinerator.tim
   sits at Voff 0 (x704 y256) so there is nothing to bracket — but the model's
   UVs run past one tile, u to 193 and v to 190, because the plating and the
   conveyor grating tile across its big faces. Those only land back on the art
   because the window wraps them mod-128. Draw this prop in a room that sets no
   window and the machine samples whatever else is in the page.

   NO render_light_dist() DISCOUNT, and that matches the room: the Incinerator
   Room places no light, and incinerator_room.c's own draw carries none either.
   The day a sconce goes in that hall, put it back in BOTH — the cull's discount
   and the shading's must agree, or a lit poly survives the cull and is then
   shaded as though it had not been, i.e. drawn in the clear colour, a hole. */
void incinerator_draw(RenderContext *ctx) {
    MATRIX view, m, combined;
    SVECTOR rr;
    VECTOR pos;
    uint8_t *buf_end, *p;
    uint16_t tp, cl;
    int32_t dcx, dcz, dist, fog_factor;
    int32_t glow_lvl;
    int pi;
    /* ---- THE GLOW IS DEFERRED TO AFTER THE MESH, AND HERE IS WHY -----------
       >>> THE BANDS DO NOT BELONG TO THE FACE THEY LEAVE. <<< They project
       OUTWARD onto the machine's OTHER faces, so sorting them one bucket in
       front of their own opening is not enough: the plating around that opening
       is coplanar with it but spans a different Z, and which of those polys
       sorts nearer changes as the camera orbits. The result was bands
       disappearing behind the machine from some angles and popping in front
       from others - the exact failure this note exists to stop coming back.

       So the loop below STASHES the opening's four screen points and records
       the NEAREST otz of any face of this prop, and the bands are queued after
       it at one bucket nearer than that. The light then lies over the whole
       machine, which is what light spilling out of a hole in it does.

       IT IS STILL SORTED AT THE PROP'S OWN DEPTH, not lifted to the front of
       the scene: a wall genuinely between the camera and the machine is
       hundreds of buckets nearer and still occludes it. That is the same
       distinction the no-+40 note above draws.

       TWO SLOTS, THOUGH ONLY ONE CAN FILL TODAY: the openings face opposite
       ways along X ((+1,0,0) and (-1,0,0)), so the backface test drops one of
       them from every camera position, and collision keeps the player out of
       the gap between them. The second slot costs 16 bytes of stack and means a
       re-export that adds a third opening degrades by losing a glow rather than
       by writing past the array. */
    DVECTOR glow_sv[2][4];
    int     glow_pending = 0;
    int32_t min_otz = OT_LENGTH;

    if (!inc_smd) return;
    if (!inc.active || inc.area != current_area) return;

    camera_build_view(&view);

    buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    tp = texmgr_tpage(inc_tex);
    cl = texmgr_clut(inc_tex);

    /* Cull at the ROOM's fog-far, not at a constant of this module's own — the
       Incinerator Room's view distance is a live number the Helluminator moves,
       and g_fog_far is whatever the area draw set a few lines before calling us
       (render.h).

       >>> AND THE DISTANCE IS MEASURED TO THE BOX, NOT TO THE ORIGIN. <<< This
       is the one thing here the oil dispenser does differently, and it has to
       be: that prop is 40 units across and its origin is inside it, so origin
       distance describes it. This one is 2000 long and its origin is at the
       far END of the hall from its body — the model is in room coordinates, so
       inc.x/inc.z are (0,0) — and culling on that would blink the whole machine
       out while the player stood next to it. Manhattan to the nearest point of
       the footprint is the honest figure. */
    dcx = (cam_x < inc.min_x) ? inc.min_x - cam_x : (cam_x > inc.max_x ? cam_x - inc.max_x : 0);
    dcz = (cam_z < inc.min_z) ? inc.min_z - cam_z : (cam_z > inc.max_z ? cam_z - inc.max_z : 0);
    dist = dcx + dcz;
    if (dist > g_fog_far) return;

    rr.vx = 0; rr.vy = (int16_t)inc.rot_y; rr.vz = 0; rr.pad = 0;
    RotMatrix(&rr, &m);
    pos.vx = inc.x; pos.vy = inc.y + GROUND_FLOOR_Y; pos.vz = inc.z;
    TransMatrix(&m, &pos);
    CompMatrixLV(&view, &m, &combined);

    gte_SetRotMatrix(&combined);
    gte_SetTransMatrix(&combined);

    fog_factor = render_fog_scale(dist);

    /* The flicker, advanced once per drawn frame. Two coprime periods summed:
       neither counter wraps on the other's beat, so the level wanders instead
       of pulsing. Kept in the DRAW for asag.c's reason - there is no update to
       hang it on that runs whether or not the machine is doing anything. */
    glow_ph1 += 7;
    glow_ph2 += 11;
    {
        /* isin() is 4096 units to the turn and returns +-4096; two of them
           averaged and folded to 0..255 gives the level. 70% floor, so the
           furnace never goes dark - it is a fire behind a grate, not a lamp
           being switched. */
        int32_t w = (isin((int32_t)glow_ph1 << 4) + isin((int32_t)glow_ph2 << 4)) >> 1;
        /* Base up and amplitude down as the burn comes on: a furnace under load
           rides high and steady where an idle one gutters. Both ends lerp on
           the one ramp, so they can never disagree about how lit it is. */
        int32_t base = IN_FLICK_BASE_IDLE +
                       ((IN_FLICK_BASE_BURN - IN_FLICK_BASE_IDLE) * burn_ramp) / IN_BURN_RAMP;
        int32_t amp  = IN_FLICK_AMP_IDLE +
                       ((IN_FLICK_AMP_BURN - IN_FLICK_AMP_IDLE) * burn_ramp) / IN_BURN_RAMP;
        glow_lvl = base + ((w * amp) >> 12);
        if (glow_lvl < 0)   glow_lvl = 0;
        if (glow_lvl > 255) glow_lvl = 255;
    }

    p = (uint8_t *)inc_smd->p_prims;
    for (pi = 0; pi < inc_smd->n_prims; pi++) {
        SMD_PRI_TYPE *pt       = (SMD_PRI_TYPE *)p;
        uint8_t       stride   = pt->len;
        int           is_quad  = (pt->type >= 2);
        int           textured = pt->texture;

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &inc_smd->p_verts[vi[0]];
        SVECTOR *v1 = &inc_smd->p_verts[vi[1]];
        SVECTOR *v2 = &inc_smd->p_verts[vi[2]];

        DVECTOR sv[4];
        int32_t sz[4], otz, nclip;
        uint8_t *col, r, g, b;

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
            SVECTOR *v3 = &inc_smd->p_verts[vi[3]];
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
        /* >>> NO +40. <<< The room mesh biases every one of its polys 40 buckets
           deeper into the OT; a prop that took the same bias sorted in the same
           slot as the wall it is set against, and a tie is a LOSS for the prop —
           the room queues its mesh before it calls us and addPrim pushes to the
           head of the bucket, so within one slot the later-added primitive is
           drawn FIRST and painted over. The note above oil_dispensers_draw()
           spells the arithmetic out. */
        if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
        /* Stay below the room's texture-window primitive at OT_LENGTH-1 so it is
           processed first, the same rule the room geometry keeps. */
        if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

        /* The nearest face of this prop, for the glow's sort - see the note by
           glow_sv above. Taken after the clamps, so it is the bucket actually
           used, and taken for EVERY face that survived culling whether or not
           the packet budget then had room for it: a conservative minimum costs
           a bucket and an optimistic one costs the bug. */
        if (otz < min_otz) min_otz = otz;

        /* Fog on the room's ramp, saturating to the Incinerator Room's clear
           colour (INC_FOG_* in src/incinerator_room.c). Hard-coded as every
           prop's is — there is no global for the colour, only for the distance
           — and this prop stands in exactly one room. */
        col = p + 16;
        r = (uint8_t)(((int32_t)col[0] * fog_factor + 7 * (256 - fog_factor)) >> 8);
        g = (uint8_t)(((int32_t)col[1] * fog_factor + 6 * (256 - fog_factor)) >> 8);
        b = (uint8_t)(((int32_t)col[2] * fog_factor + 9 * (256 - fog_factor)) >> 8);

        if (is_quad && textured) {
            uint8_t *uv = p + 20;
            POLY_FT4 *poly;
            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            poly = (POLY_FT4 *)ctx->next_packet;
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

            /* A FURNACE OPENING: stash its four screen points and draw the
               light after the mesh - see the note by glow_sv. Quad-only by
               construction, the mask being built from a four-corner UV box. */
            if (((glow_mask[pi >> 5] >> (pi & 31)) & 1u) && glow_pending < 2) {
                glow_sv[glow_pending][0] = sv[0];
                glow_sv[glow_pending][1] = sv[1];
                glow_sv[glow_pending][2] = sv[2];
                glow_sv[glow_pending][3] = sv[3];
                glow_pending++;
            }
        } else if (is_quad) {
            POLY_F4 *poly;
            if (ctx->next_packet + sizeof(POLY_F4) > buf_end) { p += stride; continue; }
            poly = (POLY_F4 *)ctx->next_packet;
            setPolyF4(poly);
            setRGB0(poly, r, g, b);
            poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
            poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
            poly->x2 = sv[2].vx; poly->y2 = sv[2].vy;
            poly->x3 = sv[3].vx; poly->y3 = sv[3].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F4);
        } else if (textured) {
            uint8_t *uv = p + 20;
            POLY_FT3 *poly;
            if (ctx->next_packet + sizeof(POLY_FT3) > buf_end) { p += stride; continue; }
            poly = (POLY_FT3 *)ctx->next_packet;
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
            POLY_F3 *poly;
            if (ctx->next_packet + sizeof(POLY_F3) > buf_end) { p += stride; continue; }
            poly = (POLY_F3 *)ctx->next_packet;
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

    /* ---- THE FURNACE LIGHT ------------------------------------------------
       Queued after every face, at one bucket nearer than the NEAREST of them,
       so no part of the machine can sort over it. The colour and the reach are
       per-instance rather than per-face, so they are resolved once here instead
       of inside the loop.

       The fog scale is folded into the level: light seen from across the hall
       fades on the same ramp the plating it falls on does, and additive bands
       that ignored the fog would be orange smears hanging in a machine that had
       already gone grey. */
    if (glow_pending > 0) {
        int32_t lv = (glow_lvl * fog_factor) >> 8;
        /* Swell and redden together on the one ramp - see THE BURN. The tint
           scales G and B DOWN only; red is untouched, because an additive
           channel pushed up would clip. */
        int32_t sc = IN_GLOW_SCALE +
                     ((IN_GLOW_SCALE_BURN - IN_GLOW_SCALE) * burn_ramp) / IN_BURN_RAMP;
        int32_t gg = (IN_GLOW_G * IN_BURN_TINT(IN_BURN_TINT_G)) >> 8;
        int32_t bb = (IN_GLOW_B * IN_BURN_TINT(IN_BURN_TINT_B)) >> 8;
        int32_t gotz = (min_otz > SCENE_OT_MIN) ? min_otz - 1 : SCENE_OT_MIN;
        int     k;
        for (k = 0; k < glow_pending; k++)
            inc_glow_frame(ctx, glow_sv[k], sc,
                           (IN_GLOW_R * lv) >> 8,
                           (gg * lv) >> 8,
                           (bb * lv) >> 8,
                           gotz);
    }

    /* Back to the plain view matrix — whatever the caller draws next is in world
       space and must not inherit the model transform. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
