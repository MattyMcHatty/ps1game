#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "sound.h"          /* SFX_HAD_DIE, SFX_WOOSH — both SND_BANK_GARDEN */
#include "player.h"         /* show_pickup_msg_raw */
#include "particles.h"      /* the grey burst, which a cutscene must draw itself */
#include "door.h"           /* interact_spend_press */
#include "rear_gate.h"      /* re-arming the room's Circle triggers */
#include "grinder_puzzle.h"
#include "hadad.h"
#include "hadad_grinder.h"

/* The Rear Gate's grinder death. Read hadad_grinder.h first: the beat sheet and
   the reason this is not in hadad.c are both there. */

/* ---- The shot ---------------------------------------------------------------
   ONE camera position for the whole scene; only the aim moves. That is what
   "step north a little and up a little, then turn to look down on the grinder"
   asks for, and it is also what makes the pan after the spirit a pan rather than
   a second crane.

   It stands on the corridor's OWN centre line (x = 0), which is where the plates
   meet, so the shot is square to the machine, the yaw is exactly due south
   (2048) and it never changes. The player threw the lever from beside it, at
   x = -105 or so behind the 195 wall standoff, so the sideways component of the
   move is under a body's width.

   >>> THE STEP IS NOW UP AND SLIGHTLY FORWARD, NOT UP AND BACK. <<< It was
   written as a step back from a lever at z = 300; the lever has since moved up
   to z = 1400, near the corridor's mouth, so the same shot at z = 1100 is 300
   in FRONT of where the player is standing. That is deliberate and the framing
   below is why: the shot's distance is set by what has to fit in it, and
   retreating another 300 to stay behind the player would push the subject 25%
   smaller for nothing. What the beat reads as is unchanged — the camera lifts
   over the hedge and looks away down the corridor.

   THE HEIGHT IS THE HEDGE'S. The corridor's side hedges stand 500 tall
   (rear_gate.h), so 750 above the floor is 250 clear of their tops — high enough
   to look DOWN into the corridor at all, which a shot at standing height cannot
   do, and not so high that the machine is seen in plan.

   THE DISTANCE IS SET BY WHAT HAS TO FIT, and the tallest thing in the frame is
   not the grinders:
     - the plates are 450 tall on a floor at y=0, and the pair spans x[-400,400]
       when shut;
     - HE is 600 tall on the same floor, i.e. y[0,-600] — but he is being
       SQUASHED, and a fully squashed Hadad is half again as tall with his feet
       pinned (the squash note in hadad.h, and draw_had_sprite), so his crown
       reaches y = 1 - 900 = -899. THAT is the number the frame has to hold.
   At gte_SetGeomScreen(256) on a 320x240 screen the half-field at distance D is
   D*120/256 vertically and D*160/256 horizontally. From (0,-750,1100) onto an
   aim point at (0,-420,-85) the forward distance is 1185 and the drop is 330, so
   D = 1230, and the frame runs y[-997,157] and x[-769,769]: the crown clears the
   top by ~100, the gravel is in shot, and the shut pair has 369 either side.

   Move the camera and this has to be reworked AS A WHOLE. The pitch is solved
   from the aim point every time it is used, so nudging the position re-aims the
   shot automatically — and tells you nothing about what has just fallen out of
   the top of the frame. */
#define HG_CAM_X          GRINDER_PUZZLE_MEET_X
#define HG_CAM_Y            (-750)
#define HG_CAM_Z             1100
#define HG_CAM_ROT           2048   /* due south: the machine is at -Z of here */

/* What the shot is centred on: the corridor's middle at the grinders' Z, a
   little above the machine's mid-height so the body has the room above it. */
#define HG_AIM_Y            (-420)

/* Where the spirit is left: THE MIDDLE OF HIS CHEST, on the body the player has
   just been watching rather than on the one in the sprite sheet. By the time he
   goes he is squashed, and a squashed Hadad is half again as tall with his feet
   pinned (the squash note in hadad.h), so he spans y[1,-899] and not y[1,-599].
   The chest is the upper third of that: 1 - 900*0.69 = -620.

   It sits 170 clear of the 450-tall plates on top of everything else, which is
   worth having but is NOT what makes it visible — hg_glow sorts at sz>>2, four
   times nearer than the room mesh, so the ball draws over the machine whatever
   height it is given. That is deliberate and it is the Rabisu's death lights'
   own trick: a spirit is not an object and being occluded by a lump of iron
   would read as a z-fighting bug rather than as depth. */
#define HG_ORB_Y            (-620)

/* ---- Timing, in frames at 60 fps ------------------------------------------- */
#define HG_T_STEP              45   /* 0.75 s: north and up, no turn           */
#define HG_T_TURN              45   /* 0.75 s: turn and pitch onto the machine */
#define HG_T_POSE  (HG_T_STEP + HG_T_TURN)  /* the two together — see hg_pose  */
#define HG_T_ORB               60   /* 1 s on the ball, as specified           */
#define HG_T_ORB_TILT          30   /* ...the first half of which is the tilt  */
#define HG_T_RISE             180   /* 3 s climbing out of sight               */
#define HG_T_BACK              60   /* 1 s home                                */

/* >>> THE ROAR'S LENGTH IS LOAD-BEARING. <<< hadad_die.vag is 38016 samples at
   11025 Hz = 3.45 s = 207 frames, and the kill is timed to land on the END of
   it: he roars all the way down and the grey burst is the last frame of the
   sound. Retrim the clip and this must move with it — the same contract
   SFX_GRIND has with GP_TRAVEL_FRAMES, and SFX_EXPLODE with the Rabisu's
   burn. */
#define HG_T_ROAR             200

/* How much travel is still to run when he goes. 20 frames of 318 leaves the
   plates 14 apart out of a 220 opening — "after the grinders are almost closed",
   with him squashed to 6% of his width and nothing left to see. */
#define HG_DEATH_LEAD          20

/* A floor under the CLOSE phase, for the one case where the plates have hardly
   any travel left when the scene arms. It cannot happen off the lever (the
   trigger runs on the frame after the throw, with the whole 318 ahead of it) but
   it can in principle off a Hadad who walks into plates that are nearly shut,
   and a death sequence that fires its roar and its burst on the same frame is
   worse than one that waits. */
#define HG_T_CLOSE_MIN        120

/* ---- The spirit -------------------------------------------------------------
   It ACCELERATES: y = orb - A*t*t/256 is a constant upward acceleration, so the
   speed is still growing when it leaves the frame — "rises into the air gaining
   speed until it disappears out of sight". A = 48 puts it 6075 up after
   HG_T_RISE, by which point it is 6.3k from the camera and the glow is nine
   pixels across; the last second is faded out on top of that, so it goes out
   rather than being cut off. */
#define HG_RISE_ACCEL          48
#define HG_ORB_FADE            60   /* frames of fade at the end of the rise   */
#define HG_ORB_IN              20   /* ...and of fade-IN as the dust settles   */

/* ---- State ---------------------------------------------------------------- */
typedef enum {
    HG_IDLE = 0,
    HG_STEP,
    HG_TURN,
    HG_CLOSE,
    HG_ORB,
    HG_RISE,
    HG_BACK,
    HG_DONE,
} HgState;

static HgState  state = HG_IDLE;
static int32_t  phase_t;
static int32_t  pose_t;        /* runs UNDER step and turn — see hg_pose      */
static int32_t  death_at;      /* frame of HG_CLOSE the kill lands on         */
static int32_t  roar_at;       /* ...and the frame the roar starts            */
static int32_t  close_len;     /* how long HG_CLOSE runs                      */
static int32_t  orb_y;         /* the spirit's live height, once it exists    */
static int32_t  orb_bright;    /* 0..256                                      */
static int32_t  glow_clock;    /* free-running, so the ball shimmers          */

/* Where the player was standing and what the camera was doing, restored on the
   way out; and where HE was standing, so the slide onto the centre line can be
   interpolated rather than snapped. */
static int32_t  save_cx, save_cy, save_cz, save_crot, save_cvy;
static int32_t  from_x, from_z, from_rot, from_pitch;

/* >>> LATCHED, NOT RE-DERIVED. <<< ADDING_A_BOSS_ENCOUNTER.txt STEP 4. Nothing
   can empty the slot while this runs — update_hadads is not even called — but
   the kill at the end of HG_CLOSE is the one call that must not miss, so the
   body is taken once, here, at the start. It is dropped again ON the kill, and
   the phases after that deliberately do not want it. */
static Hadad   *victim = NULL;

/* ---- Small maths, all of it borrowed ---------------------------------------
   turn_delta and the ease are hadad_library.c's; aim_pitch and the square root
   are rabisu_boss.c's and rabisu.c's. They are copied rather than shared for the
   reason those two files copy each other's: three lines of trig in a director is
   not a subsystem, and the day this corridor's shot changes it has to be free to
   change without moving anything under the Library or the boss. */

/* Shortest signed turn from `from` to `to`, in 4096ths — never lerp the raw
   angle, or a turn from 4000 to 100 goes the long way round. */
static int32_t turn_delta(int32_t from, int32_t to) {
    int32_t d = ((to - from) % 4096 + 4096) % 4096;
    if (d > 2048) d -= 4096;
    return d;
}

/* 0..256 ease-out: leaves fast and settles. */
static int32_t hg_ease(int32_t t, int32_t total) {
    int32_t p = t * 256 / total; if (p > 256) p = 256;
    int32_t inv = 256 - p;
    return 256 - (inv * inv / 256);
}

/* Pitch, in 4096ths, that aims a camera at a point `dy` below it (world +Y is
   down, so dy > 0 is a downward, positive pitch — camera.h) and `dz` in front.
   This SDK has no arctangent, so it is eleven halvings on the SDK's OWN
   isin/icos: answering in the same trig camera_build_view projects through means
   the aim cannot disagree with the picture. Cross-multiplied, so there is no
   division and 4096 * 6000 stays well inside an int32. */
static int32_t aim_pitch(int32_t dy, int32_t dz) {
    int32_t lo = -1024, hi = 1024;
    while (hi - lo > 1) {
        /* & 4095 because the answer is genuinely negative once the spirit is
           above the camera, and isin/icos are documented over 0..4095. The mask
           is exact rather than a clamp: both are periodic in 4096 and two's
           complement wraps the right way. */
        int32_t mid = (lo + hi) >> 1, a = mid & 4095;
        if (isin(a) * dz < icos(a) * dy) lo = mid;
        else                             hi = mid;
    }
    return lo;
}

/* Integer square root (Newton, seeded by halving) for the glow's falloff. */
static int32_t hg_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x = v, last;
    if (x > 1 << 16) x = 1 << 16;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

/* The pitch that holds a point on the corridor's centre line from the fixed
   camera. >>> NOTE THE SUBTRACTION. <<< The forward distance is cam_z - z and
   NOT z - cam_z, because this shot faces SOUTH, unlike the Rabisu's crane it is
   copied from. Getting it the wrong way round gives a camera that aims itself
   very carefully at the lawn behind it. */
static int32_t hg_look_at(int32_t y, int32_t z) {
    return aim_pitch(y - HG_CAM_Y, HG_CAM_Z - z);
}

/* ---- Public ---------------------------------------------------------------- */

int hadad_grinder_cutscene(void) {
    return state != HG_IDLE && state != HG_DONE;
}

static void hg_park(void) {
    state      = HG_IDLE;
    phase_t    = 0;
    pose_t     = 0;
    orb_bright = 0;
    victim     = NULL;
}

void hadad_grinder_enter(void) { hg_park(); }

void hadad_grinder_reset(void) {
    hg_park();
    camera_release_player();
    cam_pitch = 0;
    /* Both are long clips on voices of their own, so nothing else would ever cut
       them: a new game started under the roar would otherwise carry the roar
       into the delivery area, and one started under the spirit would carry
       that. */
    sound_stop(SFX_HAD_DIE);
    sound_stop(SFX_WOOSH);
}

void hadad_grinder_begin(Hadad *h) {
    if (!h) return;

    /* Take the camera and PIN THE PLAYER where they stood. They keep that spot
       for the whole scene and are put back on it at the end; camera.h explains
       why the anchor is what makes "the player" and "the camera" two different
       things for the duration. */
    camera_look_cancel();
    camera_anchor_player(cam_x, cam_y, cam_z);
    save_cx = cam_x; save_cy = cam_y; save_cz = cam_z;
    save_crot = cam_rot; save_cvy = cam_vy;

    /* >>> THE BODY IS OURS NOW — AND THIS IS ALSO WHAT STOPS THE MUSIC. <<<
       `frozen` takes him out of update_hadads entirely (hadad.h), which does
       two things at once: the AI cannot walk him back out from between the
       plates, and he stops asking for the stalker track. The reconciliation at
       the foot of update_hadads then sees `music_wanted` clear on this very
       frame and stops CD-DA track 8 itself.

       >>> DO NOT STOP IT FROM HERE INSTEAD. <<< That was the first attempt and
       it did not work: update_hadads still runs ONCE MORE on the frame the
       trigger fires (grinder_puzzle_update is in main.c's Rear Gate branch, and
       the shared entity updates below that branch are not skipped until the
       NEXT frame takes the cutscene path). Its reconciliation saw music_wanted
       still set and music_on cleared by hand, and put the track straight back
       on — after which nothing ever called update_hadads again to stop it, so
       it looped for the rest of the scene while the CD commands issued into a
       streaming drive dragged the whole game down. It is the identical bug the
       door_anim_active() guard at the top of update_hadads was written for; the
       fix is the identical one, which is to stop ASKING rather than to stop the
       track.

       Nothing replaces it. The scene plays over the grinders' three-times grind
       (grinder_puzzle.c, still running underneath), the roar and the spirit, and
       the corridor is silent after. */
    h->frozen = 1;

    victim     = h;
    from_x     = h->x;
    from_z     = h->z;
    from_rot   = cam_rot;
    from_pitch = cam_pitch;

    /* The two frame numbers of the CLOSE phase, worked out ONCE and up front
       rather than tested against the live travel every frame. Doing it this way
       is what makes the degenerate case (plates with almost no travel left) come
       out as a short wait instead of as a roar and a burst on the same frame:
       the floor is applied to the PHASE, and the roar is then placed relative to
       the kill rather than to the machinery. */
    close_len = grinder_puzzle_travel_left();
    if (close_len < HG_T_CLOSE_MIN) close_len = HG_T_CLOSE_MIN;
    death_at = close_len - HG_DEATH_LEAD;
    if (death_at < HG_T_CLOSE_MIN) death_at = HG_T_CLOSE_MIN;
    if (death_at > close_len)      death_at = close_len;
    roar_at  = death_at - HG_T_ROAR;
    if (roar_at < 0) roar_at = 0;

    orb_y      = HG_ORB_Y;
    orb_bright = 0;
    glow_clock = 0;
    phase_t    = 0;
    pose_t     = 0;
    state      = HG_STEP;
}

/* ---- The pose ---------------------------------------------------------------
   Runs UNDER both camera phases on its own counter, for the reason the Rabisu's
   pull-back does: it is one continuous move, and splitting it in two would be
   two sets of endpoints to keep in agreement at the seam.

   Two things happen to him across the 1.5 seconds the camera is moving. He
   SLIDES onto the point the plates meet — both axes, because "exactly in the
   middle of it" is what the shot is of and being caught only ever meant his
   centre was somewhere in an 800 x 400 strip. And he starts being SQUEEZED — eased in from
   his true width rather than snapped to the plates, because the plates are
   already only 220 apart against a 300 half-width when the lever is thrown, so
   the honest value on frame one is a visible 27% pop. */
static int32_t hg_squash_want(void) {
    int32_t gap  = grinder_puzzle_plate_gap();
    int32_t want = 256 - (gap * 256) / HAD_HALF_W;
    if (want < 0)   want = 0;
    if (want > 256) want = 256;
    return want;
}

static void hg_pose(void) {
    if (!victim) return;
    int32_t e = hg_ease(pose_t, HG_T_POSE);
    victim->x = from_x + ((GRINDER_PUZZLE_MEET_X - from_x) * e) / 256;
    victim->z = from_z + ((GRINDER_PUZZLE_MEET_Z - from_z) * e) / 256;
    /* Scaled to the LIVE gap, so the body and the plates cannot disagree — the
       plates are still travelling underneath this, driven by grinder_puzzle.c
       exactly as they would be with nobody between them. */
    victim->squash = (hg_squash_want() * e) / 256;
}

/* Once the pose is over, he simply IS the gap. */
static void hg_crush(void) {
    if (!victim) return;
    victim->x      = GRINDER_PUZZLE_MEET_X;
    victim->z      = GRINDER_PUZZLE_MEET_Z;
    victim->squash = hg_squash_want();
}

/* Control comes back. The release is the teleport: player_x/y/z() go back to
   reading cam_*, and cam_* is standing where the player left off.

   `played` distinguishes the two ways out. 1 is the scene ending as written and
   is the only one that posts the log line; 0 is the bail-out below, where the
   body was pulled out from under the script and nothing has actually happened
   to anybody's spirit. Everything else — the camera, the pitch, the anchor, the
   four stale Circle triggers — is owed either way. */
static void hg_finish(int played) {
    cam_x = save_cx; cam_y = save_cy; cam_z = save_cz;
    cam_rot = save_crot; cam_vy = save_cvy;
    cam_pitch = 0;              /* free-look never clears this for you */
    camera_release_player();

    /* A Circle still down would otherwise resolve as an interact on release and
       throw the lever straight back the other way. Spend the press, then re-seed
       the four Circle triggers in this room: none of them has been polled for
       eleven seconds and all four are stale. */
    interact_spend_press();
    grinder_puzzle_arm();
    rear_gate_gate_arm();
    rear_gate_sdoor_arm();
    rear_gate_plinth_arm();

    /* The line the whole thing was for. Posted HERE and not on the kill: the log
       box is one of the things a cutscene hides (main.c's HUD rules), so a line
       posted six seconds earlier would simply have gone unread. */
    if (played) show_pickup_msg_raw("Hadad's spirit was set free.");

    victim = NULL;
    state  = HG_DONE;
}

void hadad_grinder_update(void) {
    if (state == HG_IDLE || state == HG_DONE) return;

    /* He was pulled out from under the script — a debug jump, a load. Give the
       camera back rather than driving a NULL through the rest of this. Only the
       phases BEFORE the kill care: from HG_ORB on there is deliberately no body
       left, and testing for one there would tear the scene down one frame after
       its own kill (ADDING_A_BOSS_ENCOUNTER.txt STEP 4, the same trap the
       Rabisu's latch exists for). */
    if (!victim && (state == HG_STEP || state == HG_TURN || state == HG_CLOSE)) {
        hg_finish(0);
        return;
    }

    phase_t++;
    glow_clock++;
    cam_vy = 0;   /* nothing applies gravity in the cutscene branch, but the
                     value carried in from free play would be waiting on the
                     other side of the release */

    switch (state) {

    /* STEP — north and up, and NOT a turn. The player goes on looking wherever
       they were looking when they threw the lever, so the first beat reads as
       the camera backing off rather than as a cut. */
    case HG_STEP: {
        int32_t e = hg_ease(phase_t, HG_T_STEP);
        cam_x     = save_cx + ((HG_CAM_X - save_cx) * e) / 256;
        cam_y     = save_cy + ((HG_CAM_Y - save_cy) * e) / 256;
        cam_z     = save_cz + ((HG_CAM_Z - save_cz) * e) / 256;
        cam_rot   = from_rot;
        cam_pitch = from_pitch;
        pose_t++;
        hg_pose();
        if (phase_t >= HG_T_STEP) {
            cam_x = HG_CAM_X; cam_y = HG_CAM_Y; cam_z = HG_CAM_Z;
            phase_t = 0;
            state   = HG_TURN;
        }
        break;
    }

    /* TURN — and only now the machine. The destination pitch is SOLVED from the
       aim point rather than written down, so the shot still holds the grinders
       if the camera constants are ever nudged. */
    case HG_TURN: {
        int32_t e     = hg_ease(phase_t, HG_T_TURN);
        int32_t dst_p = hg_look_at(HG_AIM_Y, GRINDER_PUZZLE_MEET_Z);
        int32_t d_rot = turn_delta(from_rot, HG_CAM_ROT);
        cam_x     = HG_CAM_X; cam_y = HG_CAM_Y; cam_z = HG_CAM_Z;
        cam_rot   = (from_rot + (d_rot * e) / 256) & 4095;
        cam_pitch = from_pitch + ((dst_p - from_pitch) * e) / 256;
        pose_t++;
        hg_pose();
        if (phase_t >= HG_T_TURN) {
            cam_rot   = HG_CAM_ROT;
            cam_pitch = dst_p;
            phase_t   = 0;
            state     = HG_CLOSE;
        }
        break;
    }

    /* CLOSE — the shot does not move. The plates come in under
       grinder_puzzle_update (main.c's cutscene branch still calls it, locked),
       he is scaled to whatever gap they leave, and the two scheduled beats land
       on the frames hadad_grinder_begin worked out. */
    case HG_CLOSE:
        hg_crush();
        if (phase_t == roar_at) sound_play(SFX_HAD_DIE);
        if (phase_t >= death_at) {
            /* THE KILL, through the ordinary damage path — so the grey burst,
               the cue and the stalker music stopping are the hundredth axe
               swing's exactly, at the position he has been squashed to. He is
               HAD_DEAD from this line and stops being drawn, which is why the
               squash had to reach the plates first: the last frame of him is a
               sliver. */
            hadad_damage(victim, victim->health);

            /* >>> AND THE MACHINERY IS SPENT. <<< The plates do not come apart
               again: the lever is dead from this frame and the corridor stays
               shut for the rest of the game, which seals the house off from the
               garden. Set HERE, on the kill, rather than when the scene ends —
               a reset or a death inside the eleven seconds that follow must not
               leave a world where he is dead AND the corridor still opens. The
               same reason FLAG_RECEPTION_HADAD is set at its arm; see
               FLAG_GRINDER_BROKEN's own block in player.h. */
            game_flag_set(FLAG_GRINDER_BROKEN);

            victim  = NULL;
            orb_y   = HG_ORB_Y;
            phase_t = 0;
            state   = HG_ORB;
        }
        break;

    /* ORB — the ball fades up out of the dust and the camera tilts off the
       machine onto it. The tilt is the first half-second; the rest is the hold
       the brief asks for. */
    case HG_ORB: {
        int32_t e     = hg_ease(phase_t, HG_T_ORB_TILT);
        int32_t src_p = hg_look_at(HG_AIM_Y, GRINDER_PUZZLE_MEET_Z);
        int32_t dst_p = hg_look_at(HG_ORB_Y, GRINDER_PUZZLE_MEET_Z);
        cam_pitch  = src_p + ((dst_p - src_p) * e) / 256;
        orb_bright = (phase_t * 256) / HG_ORB_IN;
        if (orb_bright > 256) orb_bright = 256;
        if (phase_t >= HG_T_ORB) {
            cam_pitch = dst_p;
            /* ONCE, as it GOES — not on the ball appearing. The clip runs 4.5 s
               against a 3 s climb, so it is still sounding as the camera turns
               back to the player, which is the intended read: the thing is gone
               and you can still hear it going. */
            sound_play(SFX_WOOSH);
            phase_t = 0;
            state   = HG_RISE;
        }
        break;
    }

    /* RISE — constant acceleration, and the camera solves its pitch from where
       the ball actually is on each frame rather than lerping between two end
       angles. It finishes looking very nearly straight up, which is the point. */
    case HG_RISE:
        orb_y     = HG_ORB_Y - (HG_RISE_ACCEL * phase_t * phase_t) / 256;
        cam_pitch = hg_look_at(orb_y, GRINDER_PUZZLE_MEET_Z);
        if (phase_t > HG_T_RISE - HG_ORB_FADE) {
            orb_bright = ((HG_T_RISE - phase_t) * 256) / HG_ORB_FADE;
            if (orb_bright < 0) orb_bright = 0;
        }
        if (phase_t >= HG_T_RISE) {
            orb_bright = 0;
            from_rot   = cam_rot;
            from_pitch = cam_pitch;
            phase_t    = 0;
            state      = HG_BACK;
        }
        break;

    /* BACK — home, and the pitch back to 0 with it. */
    case HG_BACK: {
        int32_t e     = hg_ease(phase_t, HG_T_BACK);
        int32_t d_rot = turn_delta(from_rot, save_crot);
        cam_x     = HG_CAM_X + ((save_cx - HG_CAM_X) * e) / 256;
        cam_y     = HG_CAM_Y + ((save_cy - HG_CAM_Y) * e) / 256;
        cam_z     = HG_CAM_Z + ((save_cz - HG_CAM_Z) * e) / 256;
        cam_rot   = (from_rot + (d_rot * e) / 256) & 4095;
        cam_pitch = from_pitch - (from_pitch * e) / 256;
        if (phase_t >= HG_T_BACK) hg_finish(1);
        break;
    }

    default:
        break;
    }
}

/* ---- The spirit's glow ------------------------------------------------------
   Concentric additive squares around the ball's projection: rbs_glow_point's
   trick (rabisu.c), in green and without the boss's colour ramp. There is one
   light here and it is one colour, and borrowing the Rabisu's white -> orange ->
   red would make the spirit read as a piece of the boss fight.

   ADDITIVE (ABR=1) for the reason everything else in this game that glows is:
   overlapping rings mix the way light does, and "dimmer" means scaled toward
   BLACK, so the outermost ring genuinely disappears into the sky rather than
   greying it. The OT is LIFO, hence the tpage going in AFTER the poly it applies
   to — the same pattern lightswitch_puzzle.c's ls_quad uses. */
#define HG_GLOW_WORLD        150   /* world half-size of the outermost square */
#define HG_GLOW_RINGS          3
static const uint8_t HG_GLOW_RGB[3] = { 40, 255, 120 };   /* the spirit's green */

static void hg_glow(RenderContext *ctx, int32_t x, int32_t y, int32_t z,
                    int32_t bright) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    SVECTOR pt;
    pt.vx = (int16_t)x; pt.vy = (int16_t)y; pt.vz = (int16_t)z; pt.pad = 0;

    DVECTOR sv;
    int32_t sz;
    gte_ldv0(&pt);
    gte_rtps();
    gte_stsxy(&sv);
    gte_stsz(&sz);
    if (sz == 0) return;
    if (sv.vx <= -1023 || sv.vx >= 1023 || sv.vy <= -1023 || sv.vy >= 1023) return;

    int32_t dx = x - cam_x, dy = y - cam_y, dz = z - cam_z;
    int32_t dist = hg_isqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 64) dist = 64;

    int32_t otz = sz >> 2;
    if (otz <= SCENE_OT_MIN)  otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;

    int ring;
    for (ring = 0; ring < HG_GLOW_RINGS; ring++) {
        if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) return;

        /* Outermost ring is full width and dimmest; each step in halves the
           square and raises the level, so the three add up to a soft falloff
           with a hot core. */
        int32_t world_half = HG_GLOW_WORLD >> ring;
        int32_t half = (world_half * 256) / dist;   /* gte_SetGeomScreen(256) */
        if (half < 1)   half = 1;
        if (half > 400) half = 400;

        int32_t level = (bright * (60 + ring * 70)) >> 8;
        if (level > 256) level = 256;
        if (level <= 0)  continue;

        POLY_F4 *p = (POLY_F4 *)ctx->next_packet;
        setPolyF4(p);
        setSemiTrans(p, 1);
        setRGB0(p, (uint8_t)((HG_GLOW_RGB[0] * level) >> 8),
                   (uint8_t)((HG_GLOW_RGB[1] * level) >> 8),
                   (uint8_t)((HG_GLOW_RGB[2] * level) >> 8));
        p->x0 = (int16_t)(sv.vx - half); p->y0 = (int16_t)(sv.vy - half);
        p->x1 = (int16_t)(sv.vx + half); p->y1 = (int16_t)(sv.vy - half);
        p->x2 = (int16_t)(sv.vx - half); p->y2 = (int16_t)(sv.vy + half);
        p->x3 = (int16_t)(sv.vx + half); p->y3 = (int16_t)(sv.vy + half);
        addPrim(&ot[otz], p);
        ctx->next_packet += sizeof(POLY_F4);

        DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
        setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
        addPrim(&ot[otz], tp);
        ctx->next_packet += sizeof(DR_TPAGE);
    }
}

void hadad_grinder_draw(RenderContext *ctx) {
    if (!hadad_grinder_cutscene()) return;

    /* The grey burst. See the note in hadad_grinder.h: particles belong to
       main.c's draw_player_systems, which is exactly what a cutscene suppresses,
       so the one visible payload of the kill would otherwise never be drawn. */
    draw_particles(ctx);

    /* A shimmer on the ball itself, so it breathes rather than sitting there as
       a flat square. */
    if (orb_bright > 0) {
        int32_t pulse = 224 + (isin((glow_clock * 40) & 4095) >> 6);  /* ~224..288 */
        int32_t level = (orb_bright * pulse) >> 8;
        if (level > 256) level = 256;
        hg_glow(ctx, GRINDER_PUZZLE_MEET_X, orb_y, GRINDER_PUZZLE_MEET_Z, level);
    }
}
