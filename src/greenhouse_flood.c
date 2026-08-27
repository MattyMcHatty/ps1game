#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"          /* game_flag, player_items, show_pickup_msg_raw */
#include "menu.h"            /* menu_inventory_sync — the grid catches up now */
#include "sound.h"
#include "particles.h"
#include "btn_glyph.h"       /* BTN_CIRCLE */
#include "door.h"            /* door_draw_string_3d */
#include "title.h"           /* STATE_GREENHOUSE */
#include "valve_handle.h"
#include "vines.h"
#include "rafflesia.h"
#include "mushroom.h"
#include "greenhouse_flood.h"

/* ---- The wheel, and the sign over it ---------------------------------------
   src/valve_handle.c owns the placement; these mirror it rather than duplicate
   it, because the sign has to hang off the ART and the art's position is stated
   there in full. The mount stands on the +Z face of the standing pipe in the
   south bay: the black hole in the pipe texture is at (-1435, -197, -3775), and
   the model ORIGIN — the wheel's centre — is one VALVE_STEM_LEN in front of it,
   at z = -3755. The wheel is a ring of radius VALVE_RADIUS lying in the XY
   plane there, so its top edge is at y = -242.

   THE SIGN IS AN XY-PLANE ONE (fixed Z, reading along X), which is the quarter
   turn from the door signs' YZ. The player reads it from the +Z side — they
   walk down the bay toward the pipe — and that side takes mirror = 1, the same
   hand as the Attic Exit's own +Z sign and the opposite of the Attic
   Stairwell's. Get it backwards and it reads "evomer ot O sserP".

   door_draw_string_3d centres the reading axis on world_x AFTER adding 200,
   hence the -200 in the call. */
#define GHF_SIGN_X       (-1435)
#define GHF_SIGN_Z       (-3735)   /* 20 proud of the wheel's face, so the
                                      glyphs stand off the metal rather than
                                      z-fighting with it */
#define GHF_SIGN_Y        (-330)   /* glyph TOP. Seven rows of GHF_TEXT_PIXEL
                                      end at -302, leaving 60 of clear air
                                      between the text and the top of the wheel
                                      at -242 — "floating above it" */
#define GHF_TEXT_PIXEL       4     /* DOOR_PIXEL_SIZE. "Press O to remove" is 17
                                      cells wide, 408 units at this size, which
                                      fits the 1900-wide bay several times over */

/* ---- Reach ------------------------------------------------------------------
   Manhattan to the wheel, as every other prompt in this room. 450 rather than
   the buttons' 400 because the PIPE'S COLLISION IS BIGGER THAN THE PIPE: the
   drawn column is 50x50 but walls 13, 20 and 37 box x[-1544,-1322]
   z[-3900,-3700], so the 195-unit wall radius holds the player at z = -3505 and
   the closest they can physically stand is 250 from the wheel. 450 leaves 200
   of slack on that and still cannot reach anything else — the nearest button is
   1100 away up the room. */
#define GHF_TRIGGER_RADIUS  450
#define GHF_TEXT_RADIUS    1300
#define GHF_FADE_NEAR       900

/* ---- The shot ---------------------------------------------------------------
   A HARD CUT, like the button puzzle's payoff and the Attic Exit lightswitches'
   before it. What it looks at is the difference: those cut to the ONE thing
   that changed, and here everything changes at once, so this is a corner
   vantage that holds the whole nave.

   THE SPOT is the nave's SOUTH-EAST corner — +X is east, +Z is north
   (greenhouse_puzzle.c states the same convention), and the nave is
   x[-3100,100] z[-2600,1400], so that corner is (100,-2600). The camera sits
   just inside it at (0,-2550) and at y = -850, which is 50 BELOW the top of the
   walls (the shell's eaves are the y = -900 band in Greenhouse.smx). Inside
   matters: at y = -900 or above it would be outside the glass, looking in
   through backfaces.

   THE YAW is atan2(dx,dz) onto the middle of the nave at (-1400,-700):
   dx = -1400, dz = 1850, i.e. 322.9deg, which is 3671 of 4096 — 3668 here,
   nudged a hair west so the two far jets sit inside the frame rather than on
   its edge. The same derivation the other two cuts use and the same trap: it is
   measured from +Z toward +X, so it is NOT atan2(dz,dx).

   THE PITCH is solved the same way. The camera looks at mid-height, y = -450,
   at a range of about 2460; that is 400 BELOW it, and world +Y is down, so it
   is a downward pitch of atan(400/2460) = 9.2deg = 105 of 4096. At that range
   the vertical field spans roughly 1150 either way, so the frame runs from
   about -1600 to +700 — the roof line at -900 and the floor at 0 are both
   comfortably inside it, which is what a shot of falling water needs. */
#define GHF_CAM_X            0
#define GHF_CAM_Y        (-850)
#define GHF_CAM_Z      (-2550)
#define GHF_CAM_ROT      3668
#define GHF_CAM_PITCH     105

/* ---- The six jets -----------------------------------------------------------
   >>> THERE IS NO CEILING GEOMETRY TO HANG THEM FROM. <<< The room is open at
   the top: the walls and glazing simply stop, and the highest thing anywhere in
   "Greenhouse.smx" is that stopping line. So the jets are placed ON IT, which is
   the only "ceiling" this room has.

   >>> AND IT MOVED WITH THE Aug 2026 DECIMATED EXPORT. <<< The old mesh carried
   a four-vertex apex ridge at y = -1205 over x[-2900,-100] z[-2400,1100], and
   the jets hung from that. The re-export dropped the ridge: nothing in the mesh
   is above y = -900 now, so a jet left at -1205 would have sprayed from a point
   OUTSIDE the building, visibly detached from it. GHF_JET_Y is the wall-top band
   and moves with it — re-check it against the mesh on any re-export, along with
   the fall timing in particles.c and the mushrooms' drop height, both of which
   are cut to this number.

   EQUIDISTANT over the NAVE (x[-3100,100] z[-2600,1400]) rather than over the
   ridge that no longer exists, as a 2x3 grid on the thirds of X and the quarters
   of Z: x = -3100 + 3200/3 and + 2*3200/3 -> -2033 and -967;
   z = 1400 - 4000/4, -2*4000/4, -3*4000/4 -> 400, -600 and -1600. That is 1067
   apart across the room and 1000 apart down it, and every one is in frame from
   the shot above (the widest bearing off the camera's yaw is 27deg against a
   horizontal half-field of about 32deg).

   particles.c owns the water itself; see the note on the spray pool there for
   why the fall is timed against this exact height. */
#define GHF_JET_Y       (-900)
#define GHF_JET_COUNT       6
static const struct { int32_t x, z; } GHF_JET[GHF_JET_COUNT] = {
    { -2033,   400 }, { -967,   400 },
    { -2033,  -600 }, { -967,  -600 },
    { -2033, -1600 }, { -967, -1600 },
};

/* ---- Timing -----------------------------------------------------------------
   GHF_SPRAY_FRAMES is the three seconds asked for, flat.

   GHF_ARRIVE_HOLD is what the shot stays up for AFTER everything lands, and it
   is sized by the SLOWEST of the three arrivals rather than picked: the vine
   curtains take GHF_VINE_DROP frames to come out of the roof, and the mushrooms
   fall 751 units at MSH_FALL_GRAVITY, which is 20. So 60 covers the curtains
   and 40 more is the beat that lets the player see what is now standing in the
   room before the camera goes back. */
#define GHF_SPRAY_FRAMES   180
#define GHF_VINE_DROP       60
#define GHF_ARRIVE_HOLD    100

/* ---- State -----------------------------------------------------------------
   Nothing here rides in the save. What persists is FLAG_GREENHOUSE_FLOOD and
   the world it produced; a half-run scene is not a world change, which is the
   same call greenhouse_puzzle.c makes about a half-pressed board. */
typedef enum {
    GHF_IDLE = 0,   /* the wheel is on the pipe and the prompt is live   */
    GHF_SPRAY,      /* fixed shot, six jets running                      */
    GHF_ARRIVE,     /* everything has landed; a beat before control back */
    GHF_DONE        /* the room has flooded; this module is inert        */
} GhfState;

static GhfState state       = GHF_IDLE;
static int      timer       = 0;
static int      circle_prev = 1;   /* starts "held": swallow a press carried in
                                      through the door transition */
static int32_t  save_cx, save_cy, save_cz, save_crot, save_cvy;

static int flooded(void) { return game_flag(FLAG_GREENHOUSE_FLOOD); }

int greenhouse_flood_active(void) {
    return state == GHF_SPRAY || state == GHF_ARRIVE;
}

/* ---- Installing the flooded room -------------------------------------------
   The flowers and the curtains, put where the flood leaves them. Called twice
   over: once from the scene, on the frame the water stops, with a travel time
   so the curtains visibly come down; and once from room entry with zero travel,
   for a player who has already seen it.

   The MUSHROOMS are deliberately not here — see the note on the three routes in
   greenhouse_flood.h, and the two call sites below. */
static void install_growth(int32_t frames) {
    int i;

    rafflesias_wake_area(STATE_GREENHOUSE);

    /* Every curtain tagged with this room EXCEPT the annexe one, which is the
       button puzzle's and is placed active from the start. Testing
       `destructible` is how they are told apart, for the reason
       vine_locked_in_area exists — see vines.h. */
    for (i = 0; i < vine_count; i++)
        if (vines[i].area == STATE_GREENHOUSE && vines[i].destructible)
            vines_drop_start(i, frames);
}

void greenhouse_flood_reset(void) {
    player_items &= ~(1 << ITEM_VALVE_HANDLE);
    state       = GHF_IDLE;
    timer       = 0;
    circle_prev = 1;
    reset_spray();
}

/* world.c's hook. See greenhouse_flood.h for why the seed path needs one of
   its own rather than leaning on greenhouse_flood_init. */
void greenhouse_flood_seed(void) { install_growth(0); }

void greenhouse_flood_init(void) {
    state       = flooded() ? GHF_DONE : GHF_IDLE;
    timer       = 0;
    circle_prev = interact_tapped();

    if (!flooded()) return;

    /* >>> THE FLAG IS SET, BUT THE ROOM MAY NOT LOOK LIKE IT. <<< That pairing
       is reachable and it is the same trap greenhouse_puzzle_init catches for
       its curtain: take() sets the flag and starts a scene, and anything that
       ends the session inside it — a death, a reset, a save reloaded, a debug
       grant of the flag — comes back with the bit set and neither the flowers
       nor the curtains placed. update() never runs again once flooded(), so
       nothing else would ever put them there.

       Installing them outright rather than replaying the scene is right for the
       reason the curtain's catch-up is: the scene is the reward for the press,
       and this player has either already had it or arrived by a route that
       never earned one. What they need is the room as it now stands.

       The MUSHROOMS are absent from this on purpose: world_seed_room() places
       them under the same flag and has already run by the time a room entry
       reaches here, so adding them again would double them. */
    install_growth(0);

    /* And the wheel is gone, whatever the mount says. valve_present rides in
       the save so it is normally already clear, but a debug grant of the flag
       would otherwise leave a handle the player could take a second time. */
    {
        int m = valve_mount_in_area(STATE_GREENHOUSE);
        if (m >= 0) valve_handle_set_present(m, 0);
    }
}

/* ---- Interaction ----------------------------------------------------------- */
static int wheel_in_reach(void) {
    int m = valve_mount_in_area(STATE_GREENHOUSE);
    if (m < 0 || !valve_handle_present(m)) return -1;

    int32_t dx = cam_x - valve_mounts[m].x;
    int32_t dz = cam_z - valve_mounts[m].z;
    if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) >= GHF_TRIGGER_RADIUS) return -1;
    /* In view as well as in range, as the buttons are: a wheel behind you is
       not one you can get a hand to. */
    if (!interact_facing(valve_mounts[m].x, valve_mounts[m].z)) return -1;
    return m;
}

static void end_cutscene(void) {
    state     = GHF_DONE;
    cam_x     = save_cx; cam_y = save_cy; cam_z = save_cz;
    cam_rot   = save_crot; cam_vy = save_cvy;
    cam_pitch = 0;
    camera_release_player();
    circle_prev = 1;   /* swallow the Circle that is probably still held */
    reset_spray();
}

static void take(int mount) {
    game_flag_set(FLAG_GREENHOUSE_FLOOD);

    valve_handle_set_present(mount, 0);
    player_items |= (1 << ITEM_VALVE_HANDLE);
    /* The grid catches up now rather than waiting for the menu to open, so the
       item is already in its cell if the player pauses during the scene. */
    menu_inventory_sync();
    show_pickup_msg_raw("Obtained the Valve Handle");
    sound_play(SFX_PICKUP);

    /* The player STAYS at the pipe while the camera cuts away — so the camera
       has somewhere honest to come back to, and so anything that starts hunting
       them hunts the real spot. The same anchor the button puzzle takes. */
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;
    camera_anchor_player(save_cx, save_cy, save_cz);

    cam_x   = GHF_CAM_X; cam_y = GHF_CAM_Y; cam_z = GHF_CAM_Z;
    cam_rot = GHF_CAM_ROT; cam_pitch = GHF_CAM_PITCH; cam_vy = 0;

    reset_spray();
    sound_play(SFX_MCHNE_GH);   /* the garden bank's machinery: the same grind
                                   the vine curtain winds up to. SFX_MCHNE
                                   itself is HOUSE-only and would be SILENT
                                   here; see sound.h. */
    state = GHF_SPRAY;
    timer = GHF_SPRAY_FRAMES;
}

/* The frame the water stops. Everything below happens together, which is the
   point of the beat: the room does not fill up gradually, it is simply full. */
static void arrive(void) {
    install_growth(GHF_VINE_DROP);

    /* THE MUSHROOMS ARE ADDED HERE AND RE-ADDED BY world_seed_room, and the two
       lists must agree — same four patrols in the same order, or a save
       reloaded after this hands the deaths to the wrong instances
       (mushrooms_dead is keyed by seed order within the room). Keep this table
       and the one in world.c's STATE_GREENHOUSE block identical.

       They come down from the roof ridge the water falls from, so the drop and
       the last of the spray share a height. */
    {
        static const struct { int32_t ax, az, bx, bz; } PATROL[4] = {
            {  -221,  1018,   -221,  -467 },
            { -2748,   874,  -1729,   120 },
            {  -424, -1114,  -2116, -1114 },
            { -1424, -2232,  -1424, -3336 },
        };
        int p;
        for (p = 0; p < 4; p++) {
            int m = mushroom_add(PATROL[p].ax, PATROL[p].az,
                                 PATROL[p].bx, PATROL[p].bz,
                                 -149, STATE_GREENHOUSE);
            if (m >= 0) mushroom_drop_in(m, GHF_JET_Y);
        }
    }

    sound_play(SFX_RUMBLE);   /* BANKED house|garden, so it sounds here */
    show_pickup_msg_raw("The greenhouse comes alive");
    state = GHF_ARRIVE;
    timer = GHF_ARRIVE_HOLD;
}

int greenhouse_flood_update(void) {
    /* The cut owns the camera, so nothing else in the room may run while it
       does — main.c's area update returns straight after calling this, the way
       the pipe buttons' payoff does. The Circle edge state is kept fresh
       throughout so the tap that started the scene cannot land on a button, or
       the door, on the frame control comes back. */
    if (state == GHF_SPRAY) {
        int j;
        for (j = 0; j < GHF_JET_COUNT; j++)
            spray_emit(GHF_JET[j].x, GHF_JET_Y, GHF_JET[j].z);
        update_spray();
        if (--timer <= 0) arrive();
        circle_prev = interact_tapped();
        return 1;
    }
    if (state == GHF_ARRIVE) {
        /* The jets are shut off but the water already in the air keeps falling
           — three seconds of spray does not stop in mid-air — so the pool is
           updated without being fed until it drains itself. */
        update_spray();
        vines_drop_update();
        if (--timer <= 0) end_cutscene();
        else              circle_prev = interact_tapped();
        return 1;
    }

    /* SELF-HEAL THE STATE AFTER A LOAD. greenhouse_flood_init runs BEFORE
       savegame_apply_pending restores game_flags, so a title-screen load
       straight into this room leaves `state` at GHF_IDLE with the flag about to
       come back set. Nothing visible goes wrong — the prompt and the take are
       both gated on the wheel still being on its mount, and it is not — but
       leaving the module saying "idle" about a room that has already flooded is
       a lie the next reader would have to work out. */
    if (state == GHF_IDLE && flooded()) state = GHF_DONE;

    int held = interact_tapped();
    int just = held && !circle_prev;
    circle_prev = held;

    if (!just || flooded()) return 0;

    int m = wheel_in_reach();
    if (m < 0) return 0;

    take(m);
    return 1;
}

void greenhouse_flood_draw(RenderContext *ctx) {
    /* Once the wheel is off there is nothing to prompt for, and nothing is left
       behind either: valve_handles_draw skips a mount whose `present` is clear,
       so the pipe simply has a hole in it where the wheel was. */
    if (state != GHF_IDLE) return;
    {
        int m = valve_mount_in_area(STATE_GREENHOUSE);
        if (m < 0 || !valve_handle_present(m)) return;
    }

    int32_t dx = cam_x - GHF_SIGN_X;
    int32_t dz = cam_z - GHF_SIGN_Z;
    int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (xz >= GHF_TEXT_RADIUS) return;

    int fade = 256;
    if (xz > GHF_FADE_NEAR) {
        int range = GHF_TEXT_RADIUS - GHF_FADE_NEAR;
        int prog  = xz - GHF_FADE_NEAR;
        if (prog > range) prog = range;
        fade = 256 - ((prog * 256) / range);
    }

    /* The green the room's other signs use. The Circle glyph inside the string
       supplies its own red regardless (btn_glyph_lookup), with the fade still
       applied to both. */
    door_draw_string_3d(ctx, "Press " BTN_CIRCLE " to remove",
                        GHF_SIGN_X - 200, GHF_SIGN_Y, GHF_SIGN_Z,
                        50, 255, 50, fade, 1, TEXT_PLANE_XY, GHF_TEXT_PIXEL);
}
