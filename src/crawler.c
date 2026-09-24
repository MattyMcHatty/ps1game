#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "collision.h"
#include "crate.h"
#include "particles.h"
#include "crawler.h"
#include "crucifaxe.h"   /* SWING_RANGE — crawlers_try_hit owns its own reach */
#include "fatdoor.h"
#include "texmgr.h"
#include "sound.h"

/* The brief is "20% of the player's max HP". Keep the two from drifting apart:
   raising MAX_HEALTH without revisiting this would quietly make the crawler
   weaker, and the number is meant to be a FRACTION of the bar. */
_Static_assert(CRW_DAMAGE_AMOUNT * 5 == MAX_HEALTH,
               "CRW_DAMAGE_AMOUNT is no longer 20% of the player's maximum");

Crawler crawlers[MAX_CRAWLERS];
int     crawler_count = 0;

/* ONE sheet per PAIR of animation frames. The art is a single 256x256 image
   quartered into four 128x128 frames and it cannot be uploaded whole: V is
   eight bits, so an 8bpp texture has to satisfy y%256 + height <= 256 — a
   256-row texture must start at VRAM y 0 or 256, and both bands are full. So
   it ships as two 256x128 halves, each holding two frames side by side:

     sheet 0  x[320,448) y128   frames 0 (left) and 1 (right)   <- spider/flower
     sheet 1  x[704,832) y128   frames 2 (left) and 3 (right)   <- zombie pair

   Both are quantised against ONE master palette (see disc.xml), so crossing
   from frame 1 to frame 2 does not shift the creature's colour even though the
   two halves carry separate CLUTs. */
#define CRW_SHEETS 2
static uint16_t crw_tpage[CRW_SHEETS] = { 0, 0 };
static uint16_t crw_clut [CRW_SHEETS] = { 0, 0 };
static int      crw_tex_id[CRW_SHEETS] = { -1, -1 };
static uint16_t shadow_tpage = 0, shadow_clut = 0;

/* Texture window the current area expects, restored after each sprite. The
   sheet sits at VRAM y=128, i.e. Voff 128, so a room's 128-tall window would
   wrap its V and sample the wrong half of the page. Identical to the spider's
   bracket and needed for the identical reason. */
static RECT crw_tw_restore;
static int  crw_tw_active = 0;

void crawlers_set_texwindow(const RECT *tw) {
    if (tw) { crw_tw_restore = *tw; crw_tw_active = 1; }
    else    { crw_tw_active = 0; }
}

static void load_tim(const char *filename, uint16_t *tpage_out, uint16_t *clut_out) {
    CdlFILE file;
    if (!CdSearchFile(&file, filename)) return;
    int sectors = (file.size + 2047) / 2048;
    void *buf = malloc(sectors * 2048);
    if (!buf) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
    CdReadSync(0, NULL);
    TIM_IMAGE tim;
    GetTimInfo((uint32_t *)buf, &tim);
    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);
    if (tim.mode & 0x8) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
    }
    *tpage_out = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);
    *clut_out  = getClut(tim.crect->x, tim.crect->y);
    free(buf);
}

/* TWO registrations for the whole enemy — one per sheet, four frames in all.
   The shadow is the shared SHADOW.TIM every other enemy uses and stays a plain
   startup load: it is resident, belongs to no bank and nothing overwrites it. */
void crawlers_load_textures(void) {
    /* BANK: Chapter 3 only. The crawler is placed in the Up Down Maze and can
       never be drawn outside TEXBANK_CATACOMBS — derived, not guessed:
       py tools/check_tex_banks.py walks the uploader call graph and fails the
       build if this mask is short. */
    texmgr_set_bank(TEXBANK_CATACOMBS);
    crw_tex_id[0] = texmgr_register("\\TEXCTCMB\\CRAWLRA.TIM;1");
    crw_tex_id[1] = texmgr_register("\\TEXCTCMB\\CRAWLRB.TIM;1");
    {
        int i;
        for (i = 0; i < CRW_SHEETS; i++) {
            crw_tpage[i] = texmgr_tpage(crw_tex_id[i]);
            crw_clut[i]  = texmgr_clut(crw_tex_id[i]);
        }
    }
    load_tim("\\SHADOW.TIM;1", &shadow_tpage, &shadow_clut);
}

void crawlers_upload_textures(void) {
    int i;
    for (i = 0; i < CRW_SHEETS; i++) texmgr_upload(crw_tex_id[i]);
}

/* ---- Placement ------------------------------------------------------------ */

static int crawler_add(int32_t x, int32_t y, int32_t z,
                       CrawlerSurf surf, GameState area) {
    if (crawler_count >= MAX_CRAWLERS) return -1;
    int i = crawler_count++;
    Crawler *s = &crawlers[i];
    *s = (Crawler){0};
    s->x = x; s->y = y; s->z = z;
    s->spawn_x = x; s->spawn_y = y; s->spawn_z = z;
    s->spawn_surface = surf;
    s->surface       = surf;
    s->wall   = -1;
    s->health = CRW_MAX_HEALTH;
    s->state  = CRW_IDLE;
    s->active = 1;
    s->area   = area;
    return i;
}

int crawler_add_floor(int32_t x, int32_t z, int32_t floor_y, GameState area) {
    /* floor_y is a floor SURFACE; GROUND_FLOOR_Y turns it into the standing
       anchor, exactly as apply_ddog_height does every frame afterwards. This is
       the one place that conversion is correct — see mistake 2 in
       tools/ADDING_AN_ENEMY.txt, and note the ceiling call below does NOT do
       it. */
    return crawler_add(x, floor_y - GROUND_FLOOR_Y, z, CRW_SURF_FLOOR, area);
}

int crawler_add_ceiling(int32_t x, int32_t z, int32_t ceiling_y, GameState area) {
    /* Hang the sprite's TOP edge on the ceiling: the quad is centred on
       y + CRW_Y_OFFSET and reaches CRW_HALF_H either side, so the half-height
       comes back off here. Anchoring the centre buries half the body in the
       roof (mistake 5). Ceiling heights are MESH coordinates and share one
       space with entity coordinates, so GROUND_FLOOR_Y has no business here. */
    return crawler_add(x, ceiling_y - CRW_Y_OFFSET + CRW_HALF_H, z,
                       CRW_SURF_CEILING, area);
}

void crawlers_init(void) {
    /* Placements are seeded on a room's first entry by world_enter(). */
    crawler_count = 0;
}

void crawlers_reset(void) {
    crawler_count = 0;
}

void crawlers_rest(void) {
    int i;
    for (i = 0; i < crawler_count; i++) {
        Crawler *s = &crawlers[i];
        if (!s->active || s->state == CRW_DEAD) continue;
        int32_t     sx = s->spawn_x, sy = s->spawn_y, sz = s->spawn_z;
        CrawlerSurf sf = s->spawn_surface;
        GameState   ar = s->area;
        int         ro = s->roused;
        *s = (Crawler){0};
        s->x = sx; s->y = sy; s->z = sz;
        s->spawn_x = sx; s->spawn_y = sy; s->spawn_z = sz;
        s->spawn_surface = sf;
        s->surface       = sf;
        s->wall   = -1;
        s->health = CRW_MAX_HEALTH;
        s->active = 1;
        s->area   = ar;
        s->roused = ro;
        /* >>> AN ALREADY-ROUSED CRAWLER COMES BACK ROUSED. <<< The brief: leave
           the room while one is active and on re-entry it is back at its
           starting position but STILL ACTIVE, so it rushes the moment the
           player walks in. Every other enemy in this game goes back to sleep
           here; this one does not, and that is the whole difference.

           A roused crawler that spawned on a CEILING starts its drop again
           rather than coming back mid-air — CRW_DROPPING from the perch is the
           same thing waking it would have done. */
        if (ro) s->state = (sf == CRW_SURF_CEILING) ? CRW_DROPPING : CRW_RUSH;
        else    s->state = CRW_IDLE;
        if (s->state == CRW_DROPPING) s->vy = CRW_DROP_VEL;
    }
}

/* ---- Sound ---------------------------------------------------------------
   Three shared latches, one voice each, exactly as the tentacle writhe and the
   spider scuttle are driven: ONE sound for every crawler in the room rather
   than one per instance, which would be five copies of the same sample fighting
   over the SPU.

   walk_on is the hardware-looped scuttle (SFX_SPDR_WLK, borrowed from the
   spider — see sfx_bank[] in sound.c, which gained SND_BANK_CATACOMBS for
   exactly this). The other two are one-shot cooldowns: five crawlers waking on
   the same frame must produce ONE scream, not five. */
static int walk_on          = 0;
static int scream_cooldown  = 0;
static int whisper_cooldown = 0;

void crawlers_silence(void) {
    sound_stop(SFX_SPDR_WLK);
    walk_on          = 0;
    scream_cooldown  = 0;
    whisper_cooldown = 0;
}

static void crawler_scream(void) {
    if (scream_cooldown > 0) return;
    sound_play(SFX_CRWL_SCRM);
    scream_cooldown = 30;
}

/* ---- Damage --------------------------------------------------------------- */

/* 2x to holy fire, i.e. the Helluminator. Nothing else on the table, so every
   Grave-olver round does its flat 1 whatever is chambered. Compare the zombie's
   3x: the crawler is the second thing in the game the lantern is a real weapon
   against, and half as much so. */
static const Weakness crawler_weakness[] = {
    { DMG_HOLY, 200 },
};

int32_t crawler_scale_damage(int32_t base, DamageType type) {
    return damage_scale(base, type, crawler_weakness,
                        WEAKNESS_COUNT(crawler_weakness));
}

/* Send it into its retreat. Shared by "it just bit the player" and "the player
   just hit it", which are the brief's two triggers and the only two.

   >>> THE DIRECTION IS LATCHED HERE AND NEVER ASKED AGAIN. <<< "It retreats in
   a straight line" is a statement about the PATH, not about the bearing: a
   retreat that recomputed -(player - self) every frame is a chase run
   backwards, and it curves as the player moves, which is what the old one did.
   One vector, fixed the frame the blow lands, Manhattan-normalised to 4096 so
   the steering arithmetic downstream sees the same magnitude it would have got
   from a goal delta. */
static void crawler_begin_retreat(Crawler *s) {
    int32_t ax = s->x - player_x();
    int32_t az = s->z - player_z();
    int32_t m  = (ax < 0 ? -ax : ax) + (az < 0 ? -az : az);
    if (m <= 0) { ax = 4096; az = 0; m = 4096; }   /* dead on top of us */
    s->ret_x = (int16_t)((ax * 4096) / m);
    s->ret_z = (int16_t)((az * 4096) / m);
    s->state         = CRW_RETREAT;
    s->retreat_left  = CRW_RETREAT_DIST;
    s->steer_timer   = 0;
    s->stall_timer   = 0;
    /* Hit halfway round a face: it stops following that wall and starts going
       over it, on the spot. Leaving the mode alone would have it slide on along
       the face toward a goal that is now the retreat's, which is neither the
       corner-turn it was doing nor the straight line it is supposed to be
       doing. */
    if (s->surface == CRW_SURF_WALL) s->wall_mode = CRW_WALL_CLIMB;
}

/* The far end of the retreat, however it was reached: the distance spent, or
   the stall watch on a body that cannot spend it. One place, because the
   scream and the pause have to fire on both. */
static void crawler_end_retreat(Crawler *s) {
    s->state       = CRW_PAUSE;
    s->pause_timer = CRW_PAUSE_FRAMES;
    s->steer_timer = 0;
    s->stall_timer = 0;
    crawler_scream();          /* "and when it pauses after a retreat" */
}

void crawler_damage(Crawler *s, int dmg) {
    if (!s->active || s->state == CRW_DEAD) return;

    /* >>> WAS IT ALREADY ADVANCING? ASK BEFORE THE WAKE, NOT AFTER. <<< The
       brief retreats a crawler that is damaged "while it is ADVANCING", and a
       sleeping one is not advancing — but the wake below turns it into a
       CRW_RUSH, so a test made afterwards would see one and bounce every
       ambushed crawler straight back into the dark without ever coming at the
       player. Which means the reward for getting the first hit in would be to
       be denied the fight. */
    int was_advancing = (s->state == CRW_RUSH);

    /* Shot in its sleep: wake it on the spot, the way every other enemy here
       does. A ceiling crawler drops first; anything else turns and rushes. */
    if (s->state == CRW_IDLE) {
        s->roused = 1;
        s->state  = (s->surface == CRW_SURF_CEILING) ? CRW_DROPPING : CRW_RUSH;
        if (s->state == CRW_DROPPING) s->vy = CRW_DROP_VEL;
        crawler_scream();
    }

    s->health   -= dmg;
    s->hit_timer = CRW_BAR_TIMER_MAX;

    if (s->health <= 0) {
        s->health = 0;
        s->state  = CRW_DEAD;
        spawn_blood_burst(s->x, s->y, s->z);
        sound_play(SFX_CRWL_SCRM);   /* the death cry is the same clip, and it
                                        ignores the cooldown: a kill must always
                                        be heard */
        scream_cooldown = 30;
        return;
    }

    sound_play(SFX_AXEHIT);

    /* >>> HURTING AN ADVANCING CRAWLER TURNS IT ROUND. <<< Only one that was
       ALREADY advancing when the blow landed: a hit taken during the retreat or
       the pause must not restart the retreat, or a player with the gun could pin
       one in the dark indefinitely and the fight would never come back to them
       — and a hit that WOKE it must not either (see was_advancing above). */
    if (was_advancing) crawler_begin_retreat(s);
}

/* One crucifaxe swing. Its own routine rather than a block in crucifaxe.c,
   because a crawler can be halfway up a wall and the inline blocks there all
   assume a body standing on the floor. Reach, facing and one-hit-per-swing are
   the same three tests the zombie's block makes.

   NO KNOCKBACK, and that is deliberate: the hit already sends it into a full
   retreat, so a shove on top would only fight the steering for the first few
   frames of a move it is making anyway. */
int crawlers_try_hit(void) {
    int i;
    for (i = 0; i < crawler_count; i++) {
        Crawler *s = &crawlers[i];
        if (!s->active || s->state == CRW_DEAD || s->area != current_area) continue;

        int32_t cy     = s->y + CRW_Y_OFFSET;
        int32_t dx     = s->x - cam_x;
        int32_t dy     = cy   - cam_y;
        int32_t dz     = s->z - cam_z;
        int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t dist3d = dist2d + (dy < 0 ? -dy : dy);
        if (dist3d >= SWING_RANGE) continue;

        int32_t dot = ((int32_t)dx * isin(cam_rot) +
                       (int32_t)dz * icos(cam_rot)) >> 12;
        if (dot <= 0) continue;

        crawler_damage(s, 1);
        return 1;
    }
    return 0;
}

/* ---- Wall attachment ------------------------------------------------------
   The crawler's answer to "the player is round a corner": rather than scraping
   along the base of the wall the way the zombie does, it MOUNTS the face and
   slides along it. The XZ path either way is the same wall-follow; what changes
   is where the body is drawn, which is the whole point of the enemy.

   A mounted crawler is parameterised by (wall index, distance along the
   segment, Y). Its XZ position is recomputed from those every frame, so nothing
   downstream may push it — apply_flat_entity_collision would shove it straight
   back off the face it is clinging to, which is why the move code below skips
   the wall passes entirely while CRW_SURF_WALL.

   THE FRONT-ONLY TEST IS THE GAME'S, NOT AN APPROXIMATION OF IT: the same
   signed dot against the inward normal and the same along-segment reject that
   collide_wall_frontonly_y makes, in that order. A crawler behind a wall face
   is not touching it and must not mount it. See mistake 12 in
   tools/ADDING_AN_ENEMY.txt for what happens when a re-implementation of this
   test drifts from the original. */

/* Integer square root, needed for a real wall-segment LENGTH: the Manhattan
   divide used all over the steering is fine for a direction that only has to be
   roughly right, but a length is a denominator here and a 40% error in it would
   put the body off the face.

   web.c's isqrt32 VERBATIM — a restoring bitwise root rather than a second
   descending-Newton one. Mistake 11 in tools/ADDING_AN_ENEMY.txt is a Newton
   seed that lands under the root and makes the refinement loop return its own
   seed (isqrt(120) came back 8); copying the version that is already correct
   and already exercised is cheaper than writing a third one and checking it. */
static int32_t crw_isqrt(int32_t v) {
    if (v <= 0) return 0;
    uint32_t n = (uint32_t)v, rem = 0, root = 0;
    int i;
    for (i = 0; i < 16; i++) {
        root <<= 1;
        rem = (rem << 2) | (n >> 30);
        n <<= 2;
        if (root < rem) { rem -= root + 1; root += 2; }
    }
    return (int32_t)(root >> 1);
}

/* ---- The body's vertical span, and Y-AWARE wall collision -----------------
   >>> apply_flat_entity_collision IS THE WRONG PUSH FOR A CRAWLER. <<< It is
   collide_wall_frontonly, which reads no Y at all: every wall in the room is a
   full-height barrier to it. That is right for a zombie, which never leaves the
   one floor a flat room has, and wrong the moment a crawler stands a storey up
   — in the Up Down Maze the ten block tops sit DIRECTLY OVER the lower maze's
   corridor walls (see the multi_level note in up_down_maze_mesh_collision.c), so
   a crawler on a walkway would be shoved about by faces a thousand units under
   its feet and could not cross its own floor. Same story on the ceiling, where
   only the outer walls reach.

   So the crawler carries its own copy of collide_wall_frontonly_y's gate: the
   same interval-overlap test on the same convention (y_min is the wall's TOP,
   y_max its bottom) and the same y_min == y_max escape meaning "no Y data,
   treat as full height", which every flat room in the game relies on.

   THE FEET ARE LIFTED A LITTLE. A wall's top IS the surface of the floor it
   carries, so a body standing on that floor overlaps the wall beneath it by a
   hair and the gate would fire on every face of the very block it is standing
   on. CRW_FOOT_LIFT is smaller than anything the geometry distinguishes and
   larger than the one-unit slop between GROUND_FLOOR_Y and the sprite's own
   foot line (Y_OFFSET + HALF_H is 150, GROUND_FLOOR_Y is 149). */
#define CRW_FOOT_LIFT 8

static void crw_body_span(const Crawler *s, int32_t *top, int32_t *bot) {
    int32_t cy = s->y + CRW_Y_OFFSET;
    *top = cy - CRW_HALF_H;
    *bot = cy + CRW_HALF_H - CRW_FOOT_LIFT;
}

static int crw_wall_in_span(const Wall *w, int32_t top, int32_t bot) {
    if (w->y_min == w->y_max) return 1;          /* no Y data: full height */
    return !(top > w->y_max || w->y_min > bot);
}

/* collide_wall_frontonly with that gate in front of it, and the two passes
   apply_flat_entity_collision makes so an inside corner resolves in one frame. */
static void crw_walls_collide(const Crawler *s, int32_t *x, int32_t *z,
                              int32_t radius) {
    CollisionRoom *room = &current_collision_room;
    int32_t top, bot;
    int i, pass;
    crw_body_span(s, &top, &bot);
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < room->wall_count; i++) {
            Wall *w = &room->walls[i];
            int32_t dx, dz, dot, tx, tz, along, len2, push;
            if (!crw_wall_in_span(w, top, bot)) continue;
            dx  = *x - w->x1;
            dz  = *z - w->z1;
            dot = ((dx >> 4) * (w->nx >> 4) + (dz >> 4) * (w->nz >> 4)) >> 4;
            if (dot >= radius || dot < 0) continue;   /* behind it: never push */
            tx    = (w->x2 - w->x1) >> 4;
            tz    = (w->z2 - w->z1) >> 4;
            along = (dx >> 4) * tx + (dz >> 4) * tz;
            len2  = tx * tx + tz * tz;
            if (along < 0 || along > len2) continue;
            push = radius - dot;
            *x += (push * w->nx) >> 12;
            *z += (push * w->nz) >> 12;
        }
    }
}

/* The way OFF the raised floor the crawler is standing on: toward whichever of
   its four sides is nearest. Returns 0 if it is not on one.

   A rushing crawler that has ended up directly above the player has a goal it
   already satisfies and no reason to move ever again (see the note on
   CRW_ABOVE_DIST in crawler.h). The nearest lip is the shortest way to stop
   being above them and start falling on them, and the drop itself is what the
   room does to anything that walks off a walkway — nothing here has to model
   it. */
static int crw_upper_edge_dir(int32_t x, int32_t z, int32_t *ex, int32_t *ez) {
    int i;
    for (i = 0; i < floor_zone_count; i++) {
        FloorZone *fz = &floor_zones[i];
        int32_t dw, de, dn, ds, dmin;
        if (fz->type != FLOOR_UPPER) continue;
        if (x < fz->min_x || x > fz->max_x) continue;
        if (z < fz->min_z || z > fz->max_z) continue;
        dw = x - fz->min_x; de = fz->max_x - x;
        dn = z - fz->min_z; ds = fz->max_z - z;
        dmin = dw; *ex = -4096; *ez =     0;
        if (de < dmin) { dmin = de; *ex =  4096; *ez =     0; }
        if (dn < dmin) { dmin = dn; *ex =     0; *ez = -4096; }
        if (ds < dmin) {            *ex =     0; *ez =  4096; }
        return 1;
    }
    return 0;
}

/* Is there a floor at (x,z) whose surface sits within `tol` of `want_y`?
   apply_ddog_height's zone walk asked as a QUESTION instead of applied as a
   move, because the step over the top of a wall has to know there is something
   up there before it commits — afterwards there is no way back. A crawler that
   stepped over a lip with nothing behind it lands outside every zone, and
   apply_ddog_height's `target` then defaults to 0: GROUND_FLOOR_Y *below* the
   floor surface, i.e. buried in it, and behind every face that could ever have
   pushed it out again (collide_wall_frontonly returns early on a negative dot).
   That is the "it was inside the floor" failure, and it is unrecoverable. */
static int crw_floor_at(int32_t x, int32_t z, int32_t want_y, int32_t tol,
                        int32_t *out_y) {
    int i;
    for (i = 0; i < floor_zone_count; i++) {
        FloorZone *fz = &floor_zones[i];
        int32_t fy, d;
        if (x < fz->min_x || x > fz->max_x) continue;
        if (z < fz->min_z || z > fz->max_z) continue;
        if (fz->type == FLOOR_RAMP) {
            int32_t len = fz->ramp_axis_end - fz->ramp_axis_start;
            int32_t pos = fz->ramp_along_x ? x : z;
            if (len == 0) {
                fy = fz->ramp_y_start;
            } else {
                int32_t t = ((pos - fz->ramp_axis_start) << 12) / len;
                if (t <    0) t =    0;
                if (t > 4096) t = 4096;
                fy = fz->ramp_y_start +
                     (((fz->ramp_y_end - fz->ramp_y_start) * t) >> 12);
            }
        } else {
            fy = fz->y;
        }
        d = fy - want_y;
        if (d < 0) d = -d;
        if (d > tol) continue;
        *out_y = fy;
        return 1;
    }
    return 0;
}

/* Recompute the body's XZ from (wall, wall_t) and hold it SURF_OFFSET off the
   face. Called after any change to either. */
static void crw_place_on_wall(Crawler *s) {
    Wall *w = &current_collision_room.walls[s->wall];
    int32_t tx = w->x2 - w->x1, tz = w->z2 - w->z1;
    int32_t off = s->wall_off;
    if (s->wall_len <= 0) return;
    s->x = w->x1 + (tx * s->wall_t) / s->wall_len + ((s->wall_nx * off) >> 12);
    s->z = w->z1 + (tz * s->wall_t) / s->wall_len + ((s->wall_nz * off) >> 12);
}

/* The body settles onto the face it has taken hold of. It mounts from wherever
   it happened to be when it saw the wall (up to CRW_MOUNT_REACH), so the first
   few frames close that gap rather than teleporting it — the decision is
   instant, the approach is still drawn. See the note on CRW_MOUNT_REACH. */
static void crw_settle_offset(Crawler *s) {
    if (s->wall_off > CRW_SURF_OFFSET) {
        s->wall_off -= CRW_OFFSET_EASE;
        if (s->wall_off < CRW_SURF_OFFSET) s->wall_off = CRW_SURF_OFFSET;
    } else if (s->wall_off < CRW_SURF_OFFSET) {
        s->wall_off = CRW_SURF_OFFSET;
    }
}

/* Find the face the crawler is running into, if any. `gx,gz` is the direction
   it WANTS to travel; a wall it is not heading into is not an obstacle and must
   not be mounted, or a crawler running alongside a corridor would climb it for
   no reason. Returns the wall index or -1, and fills the distance out from the
   face, the distance along it and its length.

   `reach` is how far out to look, and it is NOT CRW_MOUNT_DIST — see the long
   note on CRW_MOUNT_REACH in crawler.h for why conflating the two is what kept
   these things on the ground.

   THE FRONT-ONLY TEST IS THE GAME'S, NOT AN APPROXIMATION OF IT: the same
   signed dot against the inward normal and the same along-segment reject that
   collide_wall_frontonly_y makes, in that order, now with that function's Y
   gate in front of them as well. A crawler behind a wall face is not touching
   it and must not mount it, and neither is one a whole storey above it. See
   mistake 12 in tools/ADDING_AN_ENEMY.txt for what happens when a
   re-implementation of this test drifts from the original. */
static int crw_find_wall(const Crawler *s, int32_t gx, int32_t gz, int32_t reach,
                         int32_t *out_dot, int32_t *out_t, int32_t *out_len) {
    CollisionRoom *room = &current_collision_room;
    /* A TRUE length, not the Manhattan one the steering uses: it is the
       denominator of the head-on test and a 41% error in it is the difference
       between mounting in a corner and standing in one. See CRW_MOUNT_HEADON. */
    int32_t gmag = crw_isqrt(gx * gx + gz * gz);
    int32_t top, bot;
    /* NEAREST WINS, ties to the lower index — "the first face it comes into
       contact with", and stable frame to frame, which is the property a corner
       actually needs. Anything cleverer re-scores two equal answers every frame
       and spends the fight alternating between them. */
    int     best = -1;
    int32_t best_dot = reach, best_t = 0, best_len = 0;
    int i;

    if (gmag <= 0) return -1;
    crw_body_span(s, &top, &bot);

    for (i = 0; i < room->wall_count; i++) {
        Wall *w = &room->walls[i];
        int32_t ex, ez, dot, into, tx, tz, len, along;
        /* 0. Not the face it just let go of, and not one on another storey. */
        if (i == s->last_wall && s->wall_cool > 0) continue;
        if (!crw_wall_in_span(w, top, bot)) continue;
        ex = s->x - w->x1; ez = s->z - w->z1;
        /* 1. In front of the face, and nearer than the best so far. */
        dot = ((ex * w->nx) + (ez * w->nz)) >> 12;
        if (dot < 0 || dot >= best_dot) continue;
        /* 2. Heading SQUARELY into it: the goal's component along the inward
              normal has to be a real fraction of the goal, not merely negative.
              "Into it at all" is true of almost every diagonal down a corridor,
              and a crawler that mounted on those would spend the fight going up
              and down the walls it was running past. */
        into = -(((gx * w->nx) + (gz * w->nz)) >> 12);
        if (into <= 0 || into * CRW_MOUNT_HEADON < gmag) continue;
        /* 3. The foot of the perpendicular has to land on the segment. */
        tx = w->x2 - w->x1; tz = w->z2 - w->z1;
        len = crw_isqrt(tx * tx + tz * tz);
        if (len <= 0) continue;
        along = ((ex * tx) + (ez * tz)) / len;
        if (along < 0 || along > len) continue;
        /* 4. And the face has to be tall enough to be worth climbing — a
              knee-high retaining step is not a wall to a crawler. y_min is the
              TOP and y_max the bottom, so the span is y_max - y_min. */
        if (w->y_max - w->y_min < CRW_HALF_H * 2) continue;

        best_dot = dot; best = i; best_t = along; best_len = len;
    }

    *out_dot = best_dot; *out_t = best_t; *out_len = best_len;
    return best;
}

/* Take hold of that face. `mode` is what the crawler intends to do with it —
   CRW_WALL_FOLLOW to get round it, CRW_WALL_CLIMB to get over it. */
static void crw_mount(Crawler *s, int wall, int32_t t, int32_t len,
                      int32_t dot, int mode) {
    Wall *w = &current_collision_room.walls[wall];
    int32_t climb, lo, hi;
    s->surface   = CRW_SURF_WALL;
    s->wall      = wall;
    s->wall_t    = t;
    s->wall_len  = len;
    s->wall_nx   = w->nx;
    s->wall_nz   = w->nz;
    s->wall_mode = (int16_t)mode;
    s->wall_hold = CRW_WALL_HOLD;   /* it is on this one for a moment now */
    s->wall_off  = (int16_t)(dot > CRW_SURF_OFFSET ? dot : CRW_SURF_OFFSET);
    s->vy        = 0;
    /* Settle CRW_CLIMB_RISE above the face's own base, clamped so the whole
       sprite stays on the face. -Y is up, so `lo` (the highest it may go) is
       the more negative bound. Only CRW_WALL_FOLLOW eases toward this; a climb
       is going past it and off the top. */
    climb = w->y_max - CRW_CLIMB_RISE;
    lo    = w->y_min - CRW_Y_OFFSET + CRW_HALF_H;
    hi    = w->y_max - CRW_Y_OFFSET - CRW_HALF_H;
    if (hi < lo) hi = lo;
    if (climb < lo) climb = lo;
    if (climb > hi) climb = hi;
    s->climb_y = climb;
    crw_place_on_wall(s);
}

/* Where does the top of this face lead? Fills the landing for a step over the
   lip: an upper floor if a zone really carries the wall's top, the ceiling if
   the face runs all the way up to one, and 0 for a lip with nothing behind it,
   which nothing may step over. See crw_floor_at for what happens if it does.

   A CEILING landing is taken at the point where the whole sprite still fits on
   the face, because that height and the ceiling-hanging anchor are the same
   number when the face reaches the roof: both are y_min - CRW_Y_OFFSET +
   CRW_HALF_H. A FLOOR landing is a real step over the brow and is taken a
   body's clearance beyond the face, on the other side. */
static int crw_cross_target(const Crawler *s, const Wall *w,
                            int32_t *lx, int32_t *lz, int32_t *ly, int *surf) {
    int32_t step  = CRW_BODY_RADIUS + CRW_SURF_OFFSET + 16;
    int32_t clear = CRW_BODY_RADIUS + 8 - CRW_SURF_OFFSET;
    int32_t ax    = s->x - ((s->wall_nx * step) >> 12);
    int32_t az    = s->z - ((s->wall_nz * step) >> 12);
    int32_t fy;
    if (crw_floor_at(ax, az, w->y_min, CRW_CROSS_TOL, &fy)) {
        *lx   = ax;
        *lz   = az;
        *ly   = fy - GROUND_FLOOR_Y;   /* a floor SURFACE into a standing anchor */
        *surf = CRW_SURF_FLOOR;
        return 1;
    }
    {
        int32_t cy = collision_ceiling_y(s->x, s->z);
        if (w->y_min - cy <= CRW_CEIL_TOL) {
            /* Step back off the face as it lets go, or it hangs inside the very
               wall it just climbed and every push that could free it is one it
               is already behind. */
            *lx   = s->x + ((s->wall_nx * clear) >> 12);
            *lz   = s->z + ((s->wall_nz * clear) >> 12);
            *ly   = cy - CRW_Y_OFFSET + CRW_HALF_H;
            *surf = CRW_SURF_CEILING;
            return 1;
        }
    }
    return 0;
}

/* Let go of the current face and remember which one it was. EVERY exit from a
   wall goes through here, the crossings at the top included, because the whole
   value of the record is that it is complete: a crawler that came off face A
   must not be handed face A again on the next frame, whichever way it left. */
static void crw_release_wall(Crawler *s) {
    if (s->wall >= 0) {
        s->last_wall = (int16_t)s->wall;
        s->wall_cool = CRW_WALL_COOL;
    }
    s->wall      = -1;
    s->wall_mode = CRW_WALL_FOLLOW;
    s->wall_hold = 0;
}

static void crw_dismount(Crawler *s) {
    /* >>> STEP CLEAR OF THE FACE BEFORE LETTING GO. <<< CRW_SURF_OFFSET is only
       the 24 that kept the sprite off the wall polys, and a body left that
       close is inside the wall's own push radius. At the END of a segment —
       which is where the follow-round-a-corner dismount always happens — that
       puts it within a hair of the neighbouring face, on whichever side of it
       the rounding falls. Land behind that face and nothing ever pushes it out
       again: collide_wall_frontonly returns early on a negative dot, so the
       crawler is inside the block for good, standing at lower-floor height in
       the middle of solid geometry. */
    if (s->wall >= 0 && s->wall < current_collision_room.wall_count) {
        int32_t clear = CRW_BODY_RADIUS + 8 - CRW_SURF_OFFSET;
        s->x += (s->wall_nx * clear) >> 12;
        s->z += (s->wall_nz * clear) >> 12;
    }
    crw_release_wall(s);
    s->surface     = CRW_SURF_FLOOR;
    s->vy          = 0;      /* gravity takes it from here */
    s->steer_timer = 0;
}

/* ---- Update --------------------------------------------------------------- */

/* Move a mounted crawler one frame on its wall, and decide whether it should
   still be on it. What "move" means depends on why it got on:

     CRW_WALL_FOLLOW    the rush's corner-turn. Slide along the face toward the
                        player and drop off the far end, which is the original
                        behaviour and the reason this enemy exists.
     CRW_WALL_CLIMB     the retreat's vertical leg. Straight UP, wall_t frozen,
                        because the retreat is one straight line in three
                        dimensions and the wall is only where that line turns
                        vertical. It does not steer, and it does not care where
                        the player is.
     CRW_WALL_OVER      the same climb, past the point where the sprite still
                        fits on the face, with a landing already probed.
     CRW_WALL_DESCEND   back down, then step off. */
static void crw_move_on_wall(Crawler *s, int32_t gx, int32_t gz,
                             int32_t px, int32_t py, int32_t pz, int pursuing) {
    Wall *w = &current_collision_room.walls[s->wall];
    int32_t tx = w->x2 - w->x1, tz = w->z2 - w->z1;
    int32_t before_t = s->wall_t, before_y = s->y;

    if (s->wall_hold > 0) s->wall_hold--;

    /* ---- Going OVER it ------------------------------------------------- */
    if (s->wall_mode == CRW_WALL_CLIMB || s->wall_mode == CRW_WALL_OVER) {
        /* `lo` is as high as the whole sprite fits on the face; `brow` is the
           anchor of something STANDING on the wall's top, which is a further
           CRW_HALF_H - CRW_Y_OFFSET - GROUND_FLOOR_Y up and is where the body
           has to reach before it can put its weight on the other side. */
        int32_t lo   = w->y_min - CRW_Y_OFFSET + CRW_HALF_H;
        int32_t brow = w->y_min - GROUND_FLOOR_Y;
        int32_t lx, lz, ly;
        int     surf;

        crw_settle_offset(s);
        s->y -= CRW_SCALE_SPEED;

        if (s->wall_mode == CRW_WALL_CLIMB && s->y <= lo) {
            s->y = lo;
            if (crw_cross_target(s, w, &lx, &lz, &ly, &surf)) {
                if (surf == CRW_SURF_CEILING) {
                    /* Already level with the roof: this IS the hand-over. */
                    s->x = lx; s->z = lz; s->y = ly;
                    crw_release_wall(s);
                    s->surface = CRW_SURF_CEILING;
                    s->vy      = 0;
                    s->moved   = 1;
                    return;
                }
                s->wall_mode = CRW_WALL_OVER;   /* there is a floor up there */
            } else {
                /* >>> NOTHING ON TOP OF THIS ONE, AND HOLDING HERE IS A HANG.
                   <<< This used to stop under the lip and wait for the stall
                   watch, which only runs during a RETREAT — so a rushing
                   crawler that climbed a face with no walkway and no roof over
                   it stayed there for the rest of the game, and a retreating
                   one spent three quarters of a second doing nothing before
                   anything noticed. There is always a way off a face sideways,
                   so take it: follow the wall from here, which ends either at a
                   corner it can turn or at the end of the face, where it drops.
                   Every path out of CRW_WALL_CLIMB now terminates. */
                s->wall_mode = CRW_WALL_FOLLOW;
                s->climb_y   = s->y;   /* stay at the height it got to */
                s->wall_hold = CRW_WALL_HOLD;
            }
        } else if (s->wall_mode == CRW_WALL_OVER && s->y <= brow) {
            s->y = brow;
            if (crw_cross_target(s, w, &lx, &lz, &ly, &surf)) {
                s->x = lx; s->z = lz; s->y = ly;
                crw_release_wall(s);
                s->surface = (CrawlerSurf)surf;
                s->vy      = 0;
                s->moved   = 1;
                return;
            }
            /* The landing went away between the probe and the brow, which
               takes a moving floor to do and nothing here has one — but if it
               ever happens, go round rather than up. */
            s->wall_mode = CRW_WALL_FOLLOW;
            s->y         = lo;
            s->climb_y   = lo;
        }

        crw_place_on_wall(s);
        if (s->y != before_y) s->moved = 1;
        return;
    }

    /* ---- Coming back DOWN it ------------------------------------------- */
    if (s->wall_mode == CRW_WALL_DESCEND) {
        int32_t base = w->y_max - CRW_Y_OFFSET - CRW_HALF_H;
        crw_settle_offset(s);
        s->y += CRW_DESCEND_SPEED;
        if (s->y >= base) {
            s->y = base;
            crw_place_on_wall(s);
            crw_dismount(s);
            /* It came down because it could see the player FROM UP THERE, and
               the line from down here may well be blocked again — which would
               send it back up the same face on the very next frame. Commit a
               sidestep first; the mount is gated on this timer. */
            s->steer_timer = CRW_STEER_COMMIT;
            s->moved = 1;
            return;
        }
        crw_place_on_wall(s);
        if (s->y != before_y) s->moved = 1;
        return;
    }

    /* ---- FOLLOW: round the corner, the rush's version -------------------- */

    crw_settle_offset(s);

    /* Slide toward whichever end of the face makes progress toward the goal.
       The sign of the goal's projection on the tangent IS the answer, and it is
       what carries the crawler round a corner: the face runs out, it drops off
       the end, and the ordinary floor steering walks it into the next one. */
    int32_t proj = ((gx * tx) + (gz * tz)) / (s->wall_len > 0 ? s->wall_len : 1);
    if (proj > 0)      s->wall_t += CRW_SPEED;
    else if (proj < 0) s->wall_t -= CRW_SPEED;

    /* Settle toward the climb height rather than snapping to it, so the mount
       reads as a climb. */
    if (s->y > s->climb_y) {
        s->y -= CRW_CLIMB_SPEED;
        if (s->y < s->climb_y) s->y = s->climb_y;
    } else if (s->y < s->climb_y) {
        s->y += CRW_CLIMB_SPEED;
        if (s->y > s->climb_y) s->y = s->climb_y;
    }

    /* Off the end of the face: back onto the floor, at the corner. */
    if (s->wall_t < 0 || s->wall_t > s->wall_len) {
        s->wall_t = s->wall_t < 0 ? 0 : s->wall_len;
        crw_place_on_wall(s);
        crw_dismount(s);
        s->moved = 1;
        return;
    }

    crw_place_on_wall(s);

    /* A CLEAR LINE TO THE PLAYER ENDS THE CLIMB — but only while PURSUING. The
       sightline runs toward the player and says nothing whatever about what is
       behind a retreating crawler, so trusting it during a retreat would drop
       one off the wall exactly when it is reversing blind. Mistake 7 in
       tools/ADDING_AN_ENEMY.txt, one surface along.

       >>> AND IT CLIMBS DOWN, IT DOES NOT LET GO. <<< This used to dismount on
       the spot, which drops a body from CRW_CLIMB_RISE in mid-air — read from
       the floor as the crawler going a little way up the wall and then falling
       through it, because that is exactly what it looks like. The same event,
       played as a descent, reads as the creature coming back down for you.

       >>> AND NOT BEFORE IT HAS BEEN UP THERE A MOMENT. <<< The sightline is
       often clear on the very frame it takes hold — the mount is decided by a
       FEELER, which trips on a wall the crawler can still see perfectly well
       past. Acting on that instantly gives mount, descend, sidestep, mount
       again: the creature pinned at a corner working hard and arriving nowhere.
       CRW_WALL_HOLD is the commitment that turns it into a move. */
    if (pursuing && s->wall_hold <= 0 &&
        !collision_segment_blocked(s->x, s->y, s->z, px, py, pz))
        s->wall_mode = CRW_WALL_DESCEND;

    if (s->wall_t != before_t || s->y != before_y) s->moved = 1;
}

void update_crawlers(void) {
    static int hurt_sfx_cooldown = 0;
    int any_walking = 0;
    int want_whisper = 0;
    int i;

    if (hurt_sfx_cooldown  > 0) hurt_sfx_cooldown--;
    if (scream_cooldown    > 0) scream_cooldown--;
    if (whisper_cooldown   > 0) whisper_cooldown--;

    for (i = 0; i < crawler_count; i++) {
        Crawler *s = &crawlers[i];
        int32_t  pre_x, pre_y, pre_z;
        if (!s->active || s->state == CRW_DEAD || s->area != current_area) continue;

        if (s->hit_timer    > 0) s->hit_timer--;
        if (s->damage_timer > 0) s->damage_timer--;
        if (s->wall_cool    > 0) s->wall_cool--;
        s->moved = 0;
        pre_x = s->x; pre_y = s->y; pre_z = s->z;

        /* >>> EVERY `continue` IN HERE BREAKS OUT OF THIS do{}while(0), NOT OUT
           OF THE FOR. <<< The frame has a tail now — the stall watch below,
           which is the only thing that tells a crawler wedged in a corner from
           one still making its way into the dark — and it has to run however
           this crawler's frame ended. A do-while(0) is the cheapest way to give
           a dozen early exits one common tail without turning the whole body
           into a function or seeding a dozen gotos. `continue` inside the
           separation loop further down still belongs to that loop, which is
           what it always meant. */
        do {

        int32_t px = player_x(), py = player_y(), pz = player_z();
        int32_t dx = px - s->x;
        int32_t dy = py - s->y;
        int32_t dz = pz - s->z;
        int32_t rad2 = dx * dx + dz * dz;

        /* ---- Asleep --------------------------------------------------------
           The wake test is a CYLINDER that extends downward: inside the radius
           in plan view, and the player at or below this crawler's own level.
           See the long note on CRW_WAKE_RADIUS in crawler.h for why a sphere
           cannot express what the brief asks for.

           dy = player_y - crawler_y, and -Y is up, so dy grows as the player
           gets LOWER and goes negative as they get higher. The bar is therefore
           dy >= -CRW_WAKE_ABOVE: below, level, or at most a body-height over.

           >>> NOT dy >= 0. <<< That is the same test with the tolerance left
           out, and it never fires: the player's eye rests 40 units above the
           floor it shares with the crawler (apply_height's player-only
           standoff, which apply_ddog_height does not apply), so a strict test
           reads every normal frame as "the player is overhead". See the note on
           CRW_WAKE_ABOVE in crawler.h. */
        if (s->state == CRW_IDLE) {
            if (dy >= -CRW_WAKE_ABOVE &&
                rad2 <= (int32_t)CRW_WAKE_RADIUS * CRW_WAKE_RADIUS) {
                s->roused = 1;
                s->state  = (s->surface == CRW_SURF_CEILING)
                          ? CRW_DROPPING : CRW_RUSH;
                if (s->state == CRW_DROPPING) s->vy = CRW_DROP_VEL;
                crawler_scream();
            } else {
                /* Still asleep. The listening radius is 50% wider and, unlike
                   the wake test, has no vertical gate at all — an idle crawler
                   is heard from the walkway above it. Flag only: the whisper is
                   fired ONCE at the end of the loop however many crawlers have
                   the player in earshot. */
                if (rad2 <= (int32_t)CRW_WHISPER_RADIUS * CRW_WHISPER_RADIUS)
                    want_whisper = 1;
                continue;
            }
        }

        /* ---- Dropping off a ceiling ---------------------------------------
           The one thing the brief says a surface changes about the attack: a
           ceiling crawler comes down first, a wall or floor one does not.
           apply_ddog_height zeroes vy on the frame it clamps to the floor,
           which is the landing. */
        if (s->state == CRW_DROPPING) {
            s->surface = CRW_SURF_FLOOR;
            apply_ddog_height(&s->x, &s->y, &s->z, &s->vy,
                              &s->on_upper_floor, &s->on_ramp);
            if (s->vy == 0) s->state = CRW_RUSH;
            continue;                  /* no rushing or biting mid-air */
        }

        /* ---- Paused at the far end of the retreat --------------------------
           The pause is also where a retreat that ended somewhere other than the
           floor is cashed in. One that finished on the CEILING drops on the
           player from up there — the same entrance a ceiling spawn makes, and
           it wants no special case of its own; one that finished part way up a
           face climbs back down it. */
        if (s->state == CRW_PAUSE) {
            if (--s->pause_timer <= 0) {
                s->state       = CRW_RUSH;
                s->steer_timer = 0;
                if (s->surface == CRW_SURF_CEILING) {
                    s->state = CRW_DROPPING;
                    s->vy    = CRW_DROP_VEL;
                } else if (s->surface == CRW_SURF_WALL) {
                    s->wall_mode = CRW_WALL_DESCEND;
                }
            }
            continue;                  /* stands still, and is drawn on frame 0 */
        }

        /* ---- Where the body is held up ------------------------------------
           Gravity and the floor for anything walking on one — and that now
           includes the block tops, which a retreat can put a crawler on. One
           under the roof hangs from whatever the roof is doing over its current
           XZ instead; nothing in this room's ceiling steps, but reading it
           every frame is what lets a crawler travel along one that does. A wall
           crawler is held by (wall, wall_t) and is touched by neither. */
        if (s->surface == CRW_SURF_FLOOR)
            apply_ddog_height(&s->x, &s->y, &s->z, &s->vy,
                              &s->on_upper_floor, &s->on_ramp);
        else if (s->surface == CRW_SURF_CEILING)
            s->y = collision_ceiling_y(s->x, s->z) - CRW_Y_OFFSET + CRW_HALF_H;

        /* ---- Contact ------------------------------------------------------
           Horizontal and vertical reach tested SEPARATELY, because the player's
           eye sits above the crawler's body and a combined budget is eaten by
           the vertical term before the horizontal one is asked. This bug has
           been fixed twice in this codebase; see STEP 4 of
           tools/ADDING_AN_ENEMY.txt. Only a RUSHING crawler bites — one that is
           bolting away past you is running, not attacking. */
        int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (s->state == CRW_RUSH && !game_over &&
            dist2d < CRW_CATCH_DIST &&
            (dy < 0 ? -dy : dy) < CRW_CATCH_DIST && s->damage_timer == 0) {
            s->damage_timer = 30;
            player_hurt(CRW_DAMAGE_AMOUNT);
            if (hurt_sfx_cooldown == 0) {
                sound_play(SFX_HURT);
                hurt_sfx_cooldown = 30;
            }
            if (player_health <= 0) {
                player_health = 0;
                game_over     = 1;
                flash_timer   = 90;
                sound_play(SFX_DIE);
            }
            /* It got what it came for: break off and go back into the dark. */
            crawler_begin_retreat(s);
            continue;
        }

        int pursuing = (s->state == CRW_RUSH);
        /* >>> THE RETREAT AIMS AT A LATCHED VECTOR, NOT AT THE PLAYER. <<< See
           crawler_begin_retreat. Everything below reads goal_dx/goal_dz without
           caring which of the two it got, which is what keeps one steering path
           for both states. */
        int32_t goal_dx = pursuing ? dx : s->ret_x;
        int32_t goal_dz = pursuing ? dz : s->ret_z;

        /* >>> A RUSH DOES NOT STAY UPSTAIRS. <<< Once it is overhead, closing
           on the player in plan view is a goal it already meets standing still,
           and standing still a storey up is where these things went quiet and
           never came back. Being above them becomes the instruction instead:
           let go of the roof, or walk off the nearest lip. See the note on
           CRW_ABOVE_DIST in crawler.h. */
        if (pursuing && dy > CRW_UPPER_DROP_DY && dist2d < CRW_ABOVE_DIST) {
            if (s->surface == CRW_SURF_CEILING) {
                s->state = CRW_DROPPING;
                s->vy    = CRW_DROP_VEL;
                continue;
            }
            if (s->on_upper_floor) {
                int32_t ex, ez;
                if (crw_upper_edge_dir(s->x, s->z, &ex, &ez)) {
                    goal_dx = ex;
                    goal_dz = ez;
                }
            }
        }

        /* ---- On a wall ---------------------------------------------------- */
        if (s->surface == CRW_SURF_WALL) {
            /* The room's collision was swapped out from under it (a debug jump,
               a load): there is no face to cling to, so fall off. */
            if (s->wall < 0 || s->wall >= current_collision_room.wall_count) {
                crw_dismount(s);
            } else {
                crw_move_on_wall(s, goal_dx, goal_dz, px, py, pz, pursuing);
                if (s->moved) any_walking = 1;
                if (s->moved) s->anim_tick++;
                continue;
            }
        }

        /* ---- Separation: soft push away from nearby crawlers ----------------
           PURSUIT ONLY. Separation bends the path it is applied to, which is
           exactly what a straight-line retreat may not have; two crawlers
           retreating along crossing lines simply pass each other, and the hard
           push at the bottom of this function is what stops them ending the
           frame in the same place. */
        int32_t sep_x = 0, sep_z = 0;
        if (pursuing) {
            int j;
            for (j = 0; j < crawler_count; j++) {
                if (j == i) continue;
                Crawler *o = &crawlers[j];
                if (!o->active || o->state == CRW_DEAD ||
                    o->area != current_area) continue;
                int32_t odx   = s->x - o->x;
                int32_t odz   = s->z - o->z;
                int32_t odist = (odx < 0 ? -odx : odx) + (odz < 0 ? -odz : odz);
                if (odist < CRW_SEP_RADIUS && odist > 0) {
                    int32_t push = CRW_SEP_RADIUS - odist;
                    sep_x += (odx * push) / odist;
                    sep_z += (odz * push) / odist;
                }
            }
        }

        int32_t desired_x = goal_dx + sep_x * CRW_SEP_WEIGHT;
        int32_t desired_z = goal_dz + sep_z * CRW_SEP_WEIGHT;
        int32_t desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                               (desired_z < 0 ? -desired_z : desired_z);
        if (desired_dist == 0) desired_dist = 1;

        /* ---- Obstacle feeler ----------------------------------------------- */
        int32_t feeler_x = s->x + (desired_x * CRW_FEELER_LEN) / desired_dist;
        int32_t feeler_z = s->z + (desired_z * CRW_FEELER_LEN) / desired_dist;
        int32_t fx = feeler_x, fz = feeler_z;
        /* LEVEL GEOMETRY FIRST, AND ASKED SEPARATELY. Only a wall can be
           climbed, so the mount below has to know whether the thing in the way
           was one; a crawler that charged a crate because something beyond it
           happened to be a wall would push at the crate until the fight timed
           out. */
        crw_walls_collide(s, &fx, &fz, CRW_BODY_RADIUS);
        int wall_blocked = (fx != feeler_x || fz != feeler_z);
        if (s->surface == CRW_SURF_FLOOR) crates_collide(&fx, s->y, &fz, 80);
        int blocked = (fx != feeler_x || fz != feeler_z);

        /* A clear line to the player means charge straight, and it is gated on
           `pursuing` for the reason spelled out in crw_move_on_wall. */
        if (pursuing &&
            !collision_segment_blocked(s->x, s->y, s->z, px, py, pz)) {
            blocked        = 0;
            s->steer_timer = 0;
        }

        /* >>> BLOCKED BY LEVEL GEOMETRY? CLIMB IT. <<< This is the enemy's
           whole movement idea: where the zombie slides along the base of the
           wall it has run into, the crawler goes UP it — round the face while
           it is hunting, straight over the top while it is running away.

           THE SEARCH REACHES AS FAR AS THE FEELER DOES; THE MOUNT DOES NOT. A
           face found beyond CRW_MOUNT_DIST is not something to steer round, it
           is something to walk INTO until it is close enough to take hold of,
           so the sidestep is suppressed and the goal is replaced by the face's
           own normal. Without that the crawler turns away at ~280 every time
           and closes on the wall exactly never — which is the whole history of
           this enemy not climbing anything. See CRW_MOUNT_REACH. */
        /* >>> BLOCKED BY LEVEL GEOMETRY? GO OVER IT. <<< Rules 1 to 4: a wall
           is something a crawler climbs, whichever way it is going, and the
           only question is whether this one is worth going over or round.

           RULE 5 IS THE EXCEPTION AND IT IS THE ONLY ONE. A face met near its
           END has a way round, and an advancing crawler takes it — that is
           "follows the face nearer the player to get round it", with the way
           along the face chosen by the goal's projection on it, which is the
           player's side by construction. A face met in the MIDDLE has no way
           round worth taking, so it goes over.

           A RETREAT NEVER FOLLOWS. Rules 1 to 3 are all "up", edge or no edge:
           following a face is a detour and a retreat is a straight line.

           No sightline test here. Rule 4 does not care whether the player is
           visible — a wall the crawler is squarely walking into is a wall it
           climbs — and gating this on the sightline was what let it grab a face
           it could see straight past and then be put back on the floor by the
           sightline test in crw_move_on_wall, on and on. ON THE FLOOR ONLY,
           though: a body under the roof has nowhere further up to go, and a
           face it mounted there would hand it back to the ceiling on the same
           frame. */
        if (wall_blocked && s->surface == CRW_SURF_FLOOR) {
            int32_t mdot = 0, mt = 0, mlen = 0;
            int     mw   = crw_find_wall(s, desired_x, desired_z,
                                         CRW_MOUNT_REACH, &mdot, &mt, &mlen);
            if (mw >= 0) {
                int at_edge = (mt < CRW_WALL_EDGE || mt > mlen - CRW_WALL_EDGE);
                crw_mount(s, mw, mt, mlen, mdot,
                          (pursuing && at_edge) ? CRW_WALL_FOLLOW
                                                : CRW_WALL_CLIMB);
                s->anim_tick++;
                any_walking = 1;
                s->moved    = 1;
                continue;
            }
        }

        int32_t pl_x = -goal_dz, pl_z =  goal_dx;   /* left  */
        int32_t pr_x =  goal_dz, pr_z = -goal_dx;   /* right */
        int32_t goal_px = s->x + goal_dx;
        int32_t goal_pz = s->z + goal_dz;

        /* >>> THE SIDESTEP IS A PURSUIT BEHAVIOUR NOW. <<< A retreat that
           steers round obstacles is not a straight line, and a maze is nothing
           but obstacles: the old one turned off at right angles to its own
           heading at the first wall and then oscillated between the two
           choices, which is most of why one could sit in a corner making no
           progress at all. A retreating crawler either climbs what is in its
           way or grinds along it and stalls out; it does not pick a side. */
        if (blocked && pursuing && s->steer_timer <= 0) {
            int32_t pl_dist = (pl_x < 0 ? -pl_x : pl_x) + (pl_z < 0 ? -pl_z : pl_z);
            int32_t pr_dist = (pr_x < 0 ? -pr_x : pr_x) + (pr_z < 0 ? -pr_z : pr_z);
            if (pl_dist == 0) pl_dist = 1;
            if (pr_dist == 0) pr_dist = 1;

            int32_t lx = s->x + (pl_x * CRW_FEELER_LEN) / pl_dist;
            int32_t lz = s->z + (pl_z * CRW_FEELER_LEN) / pl_dist;
            int32_t rx = s->x + (pr_x * CRW_FEELER_LEN) / pr_dist;
            int32_t rz = s->z + (pr_z * CRW_FEELER_LEN) / pr_dist;

            int32_t tlx = lx, tlz = lz;
            crates_collide(&tlx, s->y, &tlz, 80);
            crw_walls_collide(s, &tlx, &tlz, CRW_BODY_RADIUS);
            int left_blocked = (tlx != lx || tlz != lz);

            int32_t trx = rx, trz = rz;
            crates_collide(&trx, s->y, &trz, 80);
            crw_walls_collide(s, &trx, &trz, CRW_BODY_RADIUS);
            int right_blocked = (trx != rx || trz != rz);

            if (left_blocked && !right_blocked) {
                s->steer_dir = +1;
            } else if (right_blocked && !left_blocked) {
                s->steer_dir = -1;
            } else {
                int32_t ld = (goal_px - lx < 0 ? lx - goal_px : goal_px - lx) +
                             (goal_pz - lz < 0 ? lz - goal_pz : goal_pz - lz);
                int32_t rd = (goal_px - rx < 0 ? rx - goal_px : goal_px - rx) +
                             (goal_pz - rz < 0 ? rz - goal_pz : goal_pz - rz);
                s->steer_dir = (ld <= rd) ? -1 : +1;
            }
            s->steer_timer = CRW_STEER_COMMIT;
        }

        if (s->steer_timer > 0 && pursuing) {
            if (s->steer_dir < 0) { desired_x = pl_x; desired_z = pl_z; }
            else                  { desired_x = pr_x; desired_z = pr_z; }
            desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                           (desired_z < 0 ? -desired_z : desired_z);
            if (desired_dist == 0) desired_dist = 1;
            s->steer_timer--;
        }

        int32_t speed   = pursuing ? CRW_SPEED : CRW_RETREAT_SPEED;
        int32_t move_x  = (desired_x * speed) / desired_dist;
        int32_t move_z  = (desired_z * speed) / desired_dist;
        int32_t prev_mx = (int16_t)(s->facing >> 16);
        int32_t prev_mz = (int16_t)(s->facing & 0xFFFF);
        int32_t blend_x = (prev_mx * (8 - CRW_TURN_RATE) + move_x * CRW_TURN_RATE) >> 3;
        int32_t blend_z = (prev_mz * (8 - CRW_TURN_RATE) + move_z * CRW_TURN_RATE) >> 3;
        s->facing = ((int32_t)(int16_t)blend_x << 16) | (uint16_t)(int16_t)blend_z;

        if (blend_x != 0 || blend_z != 0) {
            any_walking = 1;
            s->moved    = 1;
            s->anim_tick++;
        }

        s->x += blend_x;
        s->z += blend_z;
        crw_walls_collide(s, &s->x, &s->z, CRW_BODY_RADIUS);
        /* Props stand on the floor, so only a body on the floor meets them. A
           crawler on a walkway a storey up or under the roof is nowhere near
           the crates and the doors, and pushing it off them would be pushing it
           off something that is not there. */
        if (s->surface == CRW_SURF_FLOOR) {
            crates_collide(&s->x, s->y, &s->z, 80);
            fatdoors_collide(&s->x, s->y, &s->z, CRW_DOOR_CLEARANCE);
        }

        } while (0);

        /* ---- The odometer, and the stall watch ------------------------------
           The frame's tail, and the one place the body's REAL travel is known:
           everything that could move it — the steering, the wall collide, the
           props, gravity — has already run. Measure it across all three axes,
           so a climb counts as progress just as a run does.

           That measurement is the retreat's clock. A retreat owes
           CRW_RETREAT_DIST of path and pays it off here, frame by frame, and
           ends when the debt is clear — wherever that leaves it, and whatever
           it had to go over to get there. Nothing asks where the player is: a
           crawler that cannot open the gap still finishes, in the same time an
           unobstructed one would, which is what the old distance-to-player test
           and its eight-second timeout could not do.

           The stall watch is what is left for a body that is not travelling at
           all, and so would never pay the debt down: less than CRW_STALL_MIN in
           a frame, CRW_STALL_FRAMES running. */
        {
            int32_t mvx = s->x - pre_x, mvy = s->y - pre_y, mvz = s->z - pre_z;
            int32_t mv  = (mvx < 0 ? -mvx : mvx) + (mvy < 0 ? -mvy : mvy) +
                          (mvz < 0 ? -mvz : mvz);
            if (s->state != CRW_RETREAT && s->state != CRW_RUSH) {
                s->stall_timer = 0;
            } else if (s->state == CRW_RETREAT) {
                s->retreat_left -= mv;
                if (s->retreat_left <= 0) {
                    crawler_end_retreat(s);
                } else if (mv >= CRW_STALL_MIN) {
                    s->stall_timer = 0;
                } else if (++s->stall_timer >= CRW_STALL_FRAMES) {
                    crawler_end_retreat(s);
                }
            } else if (mv >= CRW_STALL_MIN) {
                s->stall_timer = 0;
            } else if (++s->stall_timer >= CRW_RUSH_STALL) {
                /* >>> AND THE RUSH NEEDS ONE TOO. <<< A retreat has its
                   odometer behind it; a rush has nothing, so anything that
                   wedges one is permanent by default. Whatever it is stuck
                   on, stop doing it: let go of the face, or commit a sidestep
                   the other way from the last one. */
                s->stall_timer = 0;
                if (s->surface == CRW_SURF_WALL) {
                    crw_dismount(s);
                } else if (s->surface == CRW_SURF_CEILING) {
                    s->state = CRW_DROPPING;   /* wherever it is, come down */
                    s->vy    = CRW_DROP_VEL;
                } else {
                    s->steer_dir   = (s->steer_dir > 0) ? -1 : +1;
                    s->steer_timer = CRW_STEER_COMMIT;
                }
            }
        }
    }

    /* The shared scuttle loop, forced off on game-over — the area update stops
       running then and the voice would stay keyed on forever, the same guard
       the tentacle writhe and the spider scuttle need. */
    if (game_over) any_walking = 0;
    if (any_walking && !walk_on)      { sound_play(SFX_SPDR_WLK); walk_on = 1; }
    else if (!any_walking && walk_on) { sound_stop(SFX_SPDR_WLK); walk_on = 0; }

    /* ONE whisper however many idle crawlers can hear the player, re-triggered
       on an interval for as long as they stay in earshot. Suppressed on
       game-over for the loop's reason. */
    if (game_over) want_whisper = 0;
    if (want_whisper && whisper_cooldown == 0) {
        sound_play(SFX_CRWL_WHSP);
        whisper_cooldown = CRW_WHISPER_INTERVAL;
    }

    /* ---- Crawler vs crawler hard collision (after every one has moved) ----
       Floor bodies only: a mounted one is held on its face by (wall, wall_t)
       and a shove would put it somewhere that parameterisation cannot
       describe. */
    int a, b;
    for (a = 0; a < crawler_count; a++) {
        Crawler *sa = &crawlers[a];
        if (!sa->active || sa->state == CRW_DEAD || sa->state == CRW_IDLE ||
            sa->surface != CRW_SURF_FLOOR || sa->area != current_area) continue;
        for (b = a + 1; b < crawler_count; b++) {
            Crawler *sb = &crawlers[b];
            if (!sb->active || sb->state == CRW_DEAD || sb->state == CRW_IDLE ||
                sb->surface != CRW_SURF_FLOOR || sb->area != current_area) continue;
            int32_t cdx  = sa->x - sb->x;
            int32_t cdz  = sa->z - sb->z;
            int32_t dist = (cdx < 0 ? -cdx : cdx) + (cdz < 0 ? -cdz : cdz);
            int32_t min_dist = CRW_BODY_RADIUS * 2;
            if (dist < min_dist && dist > 0) {
                int32_t push    = (min_dist - dist) / 2;
                int32_t push_ax = (cdx * push) / dist;
                int32_t push_az = (cdz * push) / dist;
                sa->x += push_ax; sa->z += push_az;
                sb->x -= push_ax; sb->z -= push_az;
            }
        }
    }
}

/* ---- Draw ----------------------------------------------------------------- */

/* ---- The texture-window bracket -------------------------------------------
   Both sheets sit at Voff 128, so a room's 128-tall window would wrap their V
   mod-128 and sample the wrong half of the page. Each body therefore goes down
   under a full/unmasked window with the area's own window restored after it.

   >>> ONE BRACKET FOR THE WHOLE BODY, NOT ONE PER QUAD. <<< The sprite is a
   CRW_SUBDIV x CRW_SUBDIV grid now (crawler.h says why), and bracketing each
   piece would put eight DR_TWINs in the packet buffer per crawler and switch
   the window sixteen times for one creature. addPrim() PREPENDS within a
   bucket, so the sequence is: add the RESTORE first, then every piece, then the
   DISABLE last — which the GPU then draws as disable, pieces, restore. Both
   twins and all the pieces share one OT bucket, so nothing can be sorted
   between them.

   The caller reserves the whole body's packet space up front and only then
   calls these, so a buffer that runs out mid-body is impossible — a half-
   emitted bracket would leave the unmasked window switched on for the rest of
   the frame and mis-sample every textured poly after it. */
static void crw_bracket_begin(RenderContext *ctx, int32_t otz) {
    if (!crw_tw_active) return;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
    setTexWindow(restore, &crw_tw_restore);
    addPrim(&ot[otz], restore);
    ctx->next_packet += sizeof(DR_TWIN);
}

static void crw_bracket_end(RenderContext *ctx, int32_t otz) {
    if (!crw_tw_active) return;
    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;
    RECT full = { 0, 0, 0, 0 };   /* mask 0 = no wrapping, full page */
    DR_TWIN *disable = (DR_TWIN *)ctx->next_packet;
    setTexWindow(disable, &full);
    addPrim(&ot[otz], disable);
    ctx->next_packet += sizeof(DR_TWIN);
}

/* Which of the four frames to draw. An idle, paused or otherwise stationary
   crawler holds the FIRST one; a moving one cycles all four. anim_tick is only
   incremented on frames it actually travelled, so a crawler pinned against
   geometry correctly stops animating as well as falling silent. */
static int crw_frame(const Crawler *s) {
    if (!s->moved) return 0;
    return (s->anim_tick / CRW_ANIM_RATE) & (CRW_ANIM_FRAMES - 1);
}

_Static_assert(CRW_SUBDIV == 2, "the UV table below is written for a 2x2 grid");

/* Texel bounds of each PIECE of `frame`, in the order the grid is walked.
   Each sheet is 256x128 8bpp: U runs 0..255 across its two frames and V, being
   the row within a 256-tall page, runs 128..255. Frame `frame` lives in sheet
   frame>>1, in that sheet's left half when frame is even and its right half
   when odd.

   INSET BY ONE TEXEL ON THE FRAME'S OUTER EDGES so a magnified quad's edge
   pixels cannot sample the neighbouring frame — the zombie hit exactly this as
   a strip of the wrong texture bleeding in, and here the neighbour is another
   crawler's legs. The INTERNAL seam between the two pieces is not inset: those
   texels are genuinely adjacent in the source image and pulling them apart
   would draw a hairline of nothing down the middle of the body. */
static void crw_frame_uv(int frame, uint8_t ulo[CRW_SUBDIV], uint8_t uhi[CRW_SUBDIV],
                                    uint8_t vlo[CRW_SUBDIV], uint8_t vhi[CRW_SUBDIV]) {
    int ub = (frame & 1) * 128;   /* which half of this sheet the frame is in */
    ulo[0] = (uint8_t)(ub +   1); uhi[0] = (uint8_t)(ub +  63);
    ulo[1] = (uint8_t)(ub +  64); uhi[1] = (uint8_t)(ub + 126);
    vlo[0] = 129;                 vhi[0] = 191;
    vlo[1] = 192;                 vhi[1] = 254;
}

static void draw_crw_shadow(RenderContext *ctx, Crawler *s) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int32_t  shade;
    if (ctx->next_packet + sizeof(DR_TPAGE) + sizeof(POLY_FT4) > buf_end) return;

    int32_t rx  = icos(cam_rot);
    int32_t rz  = -isin(cam_rot);
    int16_t dwx = (int16_t)((CRW_SHADOW_W * rx) >> 12);
    int16_t dwz = (int16_t)((CRW_SHADOW_W * rz) >> 12);

    int32_t fx  = isin(cam_rot);
    int32_t fz  = icos(cam_rot);
    int16_t ddx = (int16_t)((CRW_SHADOW_D * fx) >> 12);
    int16_t ddz = (int16_t)((CRW_SHADOW_D * fz) >> 12);

    int32_t shadow_y = s->y + CRW_Y_OFFSET + CRW_HALF_H - 2;

    SVECTOR sv[4];
    sv[0].vx = (int16_t)(s->x - dwx - ddx); sv[0].vy = (int16_t)shadow_y; sv[0].vz = (int16_t)(s->z - dwz - ddz); sv[0].pad = 0;
    sv[1].vx = (int16_t)(s->x + dwx - ddx); sv[1].vy = (int16_t)shadow_y; sv[1].vz = (int16_t)(s->z + dwz - ddz); sv[1].pad = 0;
    sv[2].vx = (int16_t)(s->x - dwx + ddx); sv[2].vy = (int16_t)shadow_y; sv[2].vz = (int16_t)(s->z - dwz + ddz); sv[2].pad = 0;
    sv[3].vx = (int16_t)(s->x + dwx + ddx); sv[3].vy = (int16_t)shadow_y; sv[3].vz = (int16_t)(s->z + dwz + ddz); sv[3].pad = 0;

    DVECTOR ssv[4];
    int32_t otz;

    gte_ldv0(&sv[0]); gte_rtps(); gte_stsxy(&ssv[0]);
    gte_ldv0(&sv[1]); gte_rtps(); gte_stsxy(&ssv[1]);
    gte_ldv0(&sv[2]); gte_rtps(); gte_stsxy(&ssv[2]);
    gte_ldv0(&sv[3]); gte_rtps(); gte_stsxy(&ssv[3]);

    gte_avsz4();
    gte_stotz(&otz);
    if (otz <= 0) return;
    otz += 2;
    if (otz >= OT_LENGTH - 2) otz = OT_LENGTH - 3;

    /* >>> THE SHADOW FOGS ON THE BODY'S OWN CURVE. <<< It used to be drawn at a
       flat 128 with no distance cull at all, and in a room this dark that was
       the tell that gave a retreated crawler away: the creature faded properly
       into the fog and left a crisp black patch on the floor where it was
       standing. Same g_fog_far cull and the same render_fog_scale as
       draw_crw_quad, so the two disappear together.

       The scale is applied to 128 rather than used as the colour directly.
       128 is the PS1's 1.0x modulation and is what this quad has always been
       drawn at; render_fog_scale returns 256 at the near plane, which used as a
       colour would be a 2x overbright shadow. Multiplying keeps the near look
       byte-identical to before and takes it to black at the far end. */
    {
        int32_t sdx = s->x - cam_x, sdz = s->z - cam_z;
        int32_t sd  = (sdx < 0 ? -sdx : sdx) + (sdz < 0 ? -sdz : sdz);
        if (sd >= g_fog_far) return;
        shade = (128 * render_fog_scale(sd)) >> 8;
        if (shade > 255) shade = 255;
        if (shade < 0)   shade = 0;
    }

    DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(tp, 0, 1, shadow_tpage);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[otz + 1], tp);
    ctx->next_packet += sizeof(DR_TPAGE);

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setRGB0(poly, (uint8_t)shade, (uint8_t)shade, (uint8_t)shade);

    poly->x0 = ssv[0].vx; poly->y0 = ssv[0].vy;
    poly->x1 = ssv[1].vx; poly->y1 = ssv[1].vy;
    poly->x2 = ssv[2].vx; poly->y2 = ssv[2].vy;
    poly->x3 = ssv[3].vx; poly->y3 = ssv[3].vy;

    /* Shadow texture at VRAM (640,160): tpage base y=0, so V offset = 160 */
    poly->u0 =  0; poly->v0 = 160;
    poly->u1 = 63; poly->v1 = 160;
    poly->u2 =  0; poly->v2 = 191;
    poly->u3 = 63; poly->v3 = 191;

    poly->clut  = shadow_clut;
    poly->tpage = shadow_tpage;

    ctx->next_packet += sizeof(POLY_FT4);
    /* shadow.tim is at VRAM y=160, i.e. Voff 160, so it needs the same bracket
       the body does. One quad, so the begin/end pair wraps just this poly. */
    crw_bracket_begin(ctx, otz);
    addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
    crw_bracket_end(ctx, otz);
}

/* Emit the body from four already-built world-space corners, in the order
   top-left, top-right, bottom-right, bottom-left of the TEXTURE. Everything
   surface-specific is decided by the caller, which is the whole reason this is
   split out: a floor crawler's quad faces the camera, a wall crawler's lies in
   the wall, a ceiling one's lies flat under the roof, and none of that changes
   the sorting, the fog, the packet accounting or the health bar.

   NO gte_nclip BACKFACE CULL. Two of the three quads are built from a surface's
   own axes rather than the camera's, so their winding reverses as the player
   walks round them and the cull would throw the whole sprite away — mistake 4
   in tools/ADDING_AN_ENEMY.txt, which the spider hit with its roll. The wall
   itself occludes a face seen from behind, which is what the cull would have
   been for. */
static void draw_crw_quad(RenderContext *ctx, Crawler *s, SVECTOR v[4],
                          int frame, int flip) {
    DVECTOR sv[4];
    int32_t sz[4];
    int32_t otz;
    int     gi, gj;

    /* The four CORNERS first, for the sort, the near-plane reject and the
       health bar. The grid below re-transforms them as part of its own sweep;
       four extra rtps a crawler is the price of keeping the sorting identical
       to every other sprite in the game rather than re-deriving it by hand. */
    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);

    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);

    gte_stsz4c(sz);
    if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) return;

    gte_avsz4();
    gte_stotz(&otz);
    /* Sort on the room-geometry scale (raw average Z) so walls between the
       camera and the crawler occlude it, while the ~40-unit gap to the floor
       polys (which the mesh sorts at avgZ+40) keeps it off the floor. ONE
       bucket for the whole body: the pieces are coplanar and cannot need
       sorting against each other, and sharing a bucket is what lets them share
       one texture-window bracket. */
    if (otz <= 0) return;
    if (otz < SCENE_OT_MIN) otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    int32_t fdx  = s->x - cam_x;
    int32_t fdz  = s->z - cam_z;
    int32_t dist = (fdx < 0 ? -fdx : fdx) + (fdz < 0 ? -fdz : fdz);
    /* Fog to match the room mesh; beyond fog_far it is fully fogged, so cull.
       THIS IS ALSO THE RETREAT PAYING OFF: CRW_RETREAT_DIST is the room's
       unlit fog_far, so a crawler that spent that much path running straight
       away from the player drops out here and really is gone into the dark
       rather than merely dim. */
    if (dist >= g_fog_far) return;
    int32_t fs   = render_fog_scale(dist);
    uint8_t fog8 = fs > 255 ? 255 : (uint8_t)fs;

    /* >>> RESERVE THE WHOLE BODY UP FRONT. <<< The bracket below switches the
       texture window off and back on around every piece at once, so running
       out of packet buffer halfway through would leave the unmasked window in
       force for the rest of the frame and mis-sample every textured poly
       after it. All or nothing. */
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    size_t   need    = (size_t)(CRW_SUBDIV * CRW_SUBDIV) * sizeof(POLY_FT4)
                     + (crw_tw_active ? 2 * sizeof(DR_TWIN) : 0);
    if (ctx->next_packet + need > buf_end) return;

    /* ---- The subdivision grid ------------------------------------------
       (CRW_SUBDIV+1)^2 points, bilinearly interpolated between the four
       corners. v[] is the ring TL, TR, BR, BL, so the top edge runs v0->v1 and
       the bottom edge v3->v2, and each column is interpolated between them.
       Works unchanged for all three surface orientations, because the caller
       has already put the corners wherever the surface wants them. */
    SVECTOR  g[(CRW_SUBDIV + 1) * (CRW_SUBDIV + 1)];
    DVECTOR  gs[(CRW_SUBDIV + 1) * (CRW_SUBDIV + 1)];
    for (gi = 0; gi <= CRW_SUBDIV; gi++) {
        int32_t tx = v[0].vx + ((int32_t)(v[1].vx - v[0].vx) * gi) / CRW_SUBDIV;
        int32_t ty = v[0].vy + ((int32_t)(v[1].vy - v[0].vy) * gi) / CRW_SUBDIV;
        int32_t tz = v[0].vz + ((int32_t)(v[1].vz - v[0].vz) * gi) / CRW_SUBDIV;
        int32_t bx = v[3].vx + ((int32_t)(v[2].vx - v[3].vx) * gi) / CRW_SUBDIV;
        int32_t by = v[3].vy + ((int32_t)(v[2].vy - v[3].vy) * gi) / CRW_SUBDIV;
        int32_t bz = v[3].vz + ((int32_t)(v[2].vz - v[3].vz) * gi) / CRW_SUBDIV;
        for (gj = 0; gj <= CRW_SUBDIV; gj++) {
            SVECTOR *p = &g[gj * (CRW_SUBDIV + 1) + gi];
            p->vx  = (int16_t)(tx + ((bx - tx) * gj) / CRW_SUBDIV);
            p->vy  = (int16_t)(ty + ((by - ty) * gj) / CRW_SUBDIV);
            p->vz  = (int16_t)(tz + ((bz - tz) * gj) / CRW_SUBDIV);
            p->pad = 0;
        }
    }
    {
        int n = (CRW_SUBDIV + 1) * (CRW_SUBDIV + 1), k;
        for (k = 0; k < n; k++) {
            gte_ldv0(&g[k]);
            gte_rtps();
            gte_stsxy(&gs[k]);
        }
    }

    uint8_t ulo[CRW_SUBDIV], uhi[CRW_SUBDIV], vlo[CRW_SUBDIV], vhi[CRW_SUBDIV];
    crw_frame_uv(frame, ulo, uhi, vlo, vhi);

    uint16_t tpage = crw_tpage[frame >> 1];
    uint16_t clut  = crw_clut [frame >> 1];

    crw_bracket_begin(ctx, otz);
    for (gj = 0; gj < CRW_SUBDIV; gj++) {
        for (gi = 0; gi < CRW_SUBDIV; gi++) {
            DVECTOR *tl = &gs[ gj      * (CRW_SUBDIV + 1) + gi    ];
            DVECTOR *tr = &gs[ gj      * (CRW_SUBDIV + 1) + gi + 1];
            DVECTOR *bl = &gs[(gj + 1) * (CRW_SUBDIV + 1) + gi    ];
            DVECTOR *br = &gs[(gj + 1) * (CRW_SUBDIV + 1) + gi + 1];

            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, fog8, fog8, fog8);

            /* POLY_FT4 wants the quad in Z order: TL, TR, BL, BR. */
            poly->x0 = tl->vx; poly->y0 = tl->vy;
            poly->x1 = tr->vx; poly->y1 = tr->vy;
            poly->x2 = bl->vx; poly->y2 = bl->vy;
            poly->x3 = br->vx; poly->y3 = br->vy;

            /* Flipping mirrors the whole body, so it swaps which COLUMN of the
               texture a piece samples as well as which way round that column
               runs. Getting only the second half right mirrors each piece in
               place and leaves the creature inside out. */
            int     sc  = flip ? (CRW_SUBDIV - 1 - gi) : gi;
            uint8_t uL  = flip ? uhi[sc] : ulo[sc];
            uint8_t uR  = flip ? ulo[sc] : uhi[sc];

            poly->u0 = uL; poly->v0 = vlo[gj];
            poly->u1 = uR; poly->v1 = vlo[gj];
            poly->u2 = uL; poly->v2 = vhi[gj];
            poly->u3 = uR; poly->v3 = vhi[gj];

            poly->tpage = tpage;
            poly->clut  = clut;

            ctx->next_packet += sizeof(POLY_FT4);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
        }
    }
    crw_bracket_end(ctx, otz);

    if (s->hit_timer <= 0) return;

    int16_t bar_cx  = (sv[0].vx + sv[1].vx) / 2;
    int16_t bar_top = (sv[0].vy < sv[1].vy ? sv[0].vy : sv[1].vy) - 8;
    int16_t bar_x   = bar_cx - 20;
    int32_t bar_otz = otz > 0 ? otz - 1 : 0;

    if (ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *bg = (TILE *)ctx->next_packet;
        setTile(bg);
        setRGB0(bg, 40, 40, 40);
        setXY0(bg, bar_x, bar_top);
        setWH(bg, 40, 5);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[bar_otz + 1], bg);
        ctx->next_packet += sizeof(TILE);
    }

    int16_t fill_w = (int16_t)((s->health * 40) / CRW_MAX_HEALTH);
    if (fill_w > 0 && ctx->next_packet + sizeof(TILE) <= buf_end) {
        TILE *fill = (TILE *)ctx->next_packet;
        setTile(fill);
        setRGB0(fill, 200, 20, 20);
        setXY0(fill, bar_x, bar_top);
        setWH(fill, fill_w, 5);
        addPrim(&ctx->buffers[ctx->active_buffer].ot[bar_otz], fill);
        ctx->next_packet += sizeof(TILE);
    }
}

void draw_crawlers(RenderContext *ctx) {
    int i;
    for (i = 0; i < crawler_count; i++) {
        Crawler *s = &crawlers[i];
        if (!s->active || s->state == CRW_DEAD || s->area != current_area) continue;

        int32_t dx = s->x - cam_x;
        int32_t dz = s->z - cam_z;
        if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) > 4000) continue;

        int32_t cx = s->x, cy = s->y + CRW_Y_OFFSET, cz = s->z;
        int     frame = crw_frame(s);
        int     flip  = 0;
        SVECTOR v[4];

        /* >>> THE QUAD IS BUILT IN THE SURFACE'S OWN PLANE. <<< That is what
           "its legs are touching whatever it is standing on" means and it is
           the reason this enemy does not just roll a camera-facing billboard
           the way the hanging spider does. A rolled billboard can only ever
           put the sprite's own up-vector along screen-right or screen-left,
           because a wall normal has no Y component to project; it flips sign as
           the player orbits past the face, and it never reads as a body lying
           ON something. Three real planes cost three corner set-ups and get it
           right from every bearing. */
        if (s->surface == CRW_SURF_WALL &&
            s->wall >= 0 && s->wall < current_collision_room.wall_count) {
            /* IN THE WALL: right = the face's tangent, down = world +Y. */
            Wall *w = &current_collision_room.walls[s->wall];
            int32_t tx = w->x2 - w->x1, tz = w->z2 - w->z1;
            int32_t len = s->wall_len > 0 ? s->wall_len : 1;
            int32_t hx  = (tx * CRW_HALF_W) / len;
            int32_t hz  = (tz * CRW_HALF_W) / len;
            v[0].vx = (int16_t)(cx - hx); v[0].vy = (int16_t)(cy - CRW_HALF_H); v[0].vz = (int16_t)(cz - hz);
            v[1].vx = (int16_t)(cx + hx); v[1].vy = (int16_t)(cy - CRW_HALF_H); v[1].vz = (int16_t)(cz + hz);
            v[2].vx = (int16_t)(cx + hx); v[2].vy = (int16_t)(cy + CRW_HALF_H); v[2].vz = (int16_t)(cz + hz);
            v[3].vx = (int16_t)(cx - hx); v[3].vy = (int16_t)(cy + CRW_HALF_H); v[3].vz = (int16_t)(cz - hz);
            v[0].pad = v[1].pad = v[2].pad = v[3].pad = 0;
            /* Face it along the way it is travelling on the face. */
            flip = (s->facing >> 16) != 0
                 ? (((int16_t)(s->facing >> 16) * tx +
                     (int16_t)(s->facing & 0xFFFF) * tz) < 0)
                 : 0;
        } else if (s->surface == CRW_SURF_CEILING) {
            /* UNDER THE ROOF: the quad lies FLAT in the XZ plane, so the player
               looking up sees the crawler's underside spread on the ceiling.
               Oriented by the camera's right axis, which keeps it readable
               from wherever it is looked at without needing a facing of its
               own. A ceiling crawler DOES travel now — the retreat can climb a
               wall that reaches the roof and carry on across it — but it is
               seen from directly underneath, where a body lying flat has no
               legible front, and the camera axis reads better from every
               bearing than a travel direction foreshortened to nothing. */
            int32_t rx = icos(cam_rot), rz = -isin(cam_rot);
            int32_t fwx = isin(cam_rot), fwz = icos(cam_rot);
            int32_t hx = (rx  * CRW_HALF_W) >> 12, hz = (rz  * CRW_HALF_W) >> 12;
            int32_t dxf= (fwx * CRW_HALF_H) >> 12, dzf= (fwz * CRW_HALF_H) >> 12;
            int32_t top = cy - CRW_HALF_H;   /* flush under the ceiling plane */
            v[0].vx = (int16_t)(cx - hx + dxf); v[0].vy = (int16_t)top; v[0].vz = (int16_t)(cz - hz + dzf);
            v[1].vx = (int16_t)(cx + hx + dxf); v[1].vy = (int16_t)top; v[1].vz = (int16_t)(cz + hz + dzf);
            v[2].vx = (int16_t)(cx + hx - dxf); v[2].vy = (int16_t)top; v[2].vz = (int16_t)(cz + hz - dzf);
            v[3].vx = (int16_t)(cx - hx - dxf); v[3].vy = (int16_t)top; v[3].vz = (int16_t)(cz - hz - dzf);
            v[0].pad = v[1].pad = v[2].pad = v[3].pad = 0;
        } else {
            /* ON THE FLOOR: the ordinary camera-facing billboard every other
               enemy uses. right = (icos(cam_rot), -isin(cam_rot)), up = world Y
               (isin/icos are 4096 = 360 degrees in this SDK). */
            int32_t rx = icos(cam_rot), rz = -isin(cam_rot);
            int32_t hx = (rx * CRW_HALF_W) >> 12;
            int32_t hz = (rz * CRW_HALF_W) >> 12;
            v[0].vx = (int16_t)(cx - hx); v[0].vy = (int16_t)(cy - CRW_HALF_H); v[0].vz = (int16_t)(cz - hz);
            v[1].vx = (int16_t)(cx + hx); v[1].vy = (int16_t)(cy - CRW_HALF_H); v[1].vz = (int16_t)(cz + hz);
            v[2].vx = (int16_t)(cx + hx); v[2].vy = (int16_t)(cy + CRW_HALF_H); v[2].vz = (int16_t)(cz + hz);
            v[3].vx = (int16_t)(cx - hx); v[3].vy = (int16_t)(cy + CRW_HALF_H); v[3].vz = (int16_t)(cz - hz);
            v[0].pad = v[1].pad = v[2].pad = v[3].pad = 0;
            /* Flip so it always faces toward the player. */
            {
                int32_t dot = ((dx >> 4) * (icos(cam_rot) >> 4))
                            - ((dz >> 4) * (isin(cam_rot) >> 4));
                flip = dot <= 0;
            }
            /* Only a grounded crawler casts a floor shadow — one drawn under a
               wall or ceiling body would sit on the floor beneath it, detached
               from anything. */
            draw_crw_shadow(ctx, s);
        }

        draw_crw_quad(ctx, s, v, frame, flip);
    }
}
