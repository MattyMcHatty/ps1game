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
   just hit it", which are the brief's two triggers and the only two. */
static void crawler_begin_retreat(Crawler *s) {
    s->state         = CRW_RETREAT;
    s->retreat_timer = CRW_RETREAT_TIMEOUT;
    s->steer_timer   = 0;
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

/* Recompute the body's XZ from (wall, wall_t) and hold it SURF_OFFSET off the
   face. Called after any change to either. */
static void crw_place_on_wall(Crawler *s) {
    Wall *w = &current_collision_room.walls[s->wall];
    int32_t tx = w->x2 - w->x1, tz = w->z2 - w->z1;
    if (s->wall_len <= 0) return;
    s->x = w->x1 + (tx * s->wall_t) / s->wall_len
                 + ((s->wall_nx * CRW_SURF_OFFSET) >> 12);
    s->z = w->z1 + (tz * s->wall_t) / s->wall_len
                 + ((s->wall_nz * CRW_SURF_OFFSET) >> 12);
}

/* Try to climb whatever the crawler has just run into. `gx,gz` is the direction
   it WANTS to travel; a wall it is not heading into is not an obstacle and must
   not be mounted, or a crawler running alongside a corridor would climb it for
   no reason. Returns 1 if it mounted. */
static int crw_try_mount(Crawler *s, int32_t gx, int32_t gz) {
    CollisionRoom *room = &current_collision_room;
    int  best = -1;
    int32_t best_dot = CRW_MOUNT_DIST;
    int32_t best_t = 0, best_len = 0;
    int i;

    for (i = 0; i < room->wall_count; i++) {
        Wall *w = &room->walls[i];
        int32_t ex = s->x - w->x1, ez = s->z - w->z1;
        /* 1. In front of the face, and close enough to reach it. */
        int32_t dot = ((ex * w->nx) + (ez * w->nz)) >> 12;
        if (dot < 0 || dot >= best_dot) continue;
        /* 2. Heading INTO it: the goal must oppose the inward normal. */
        if (((gx * w->nx) + (gz * w->nz)) >> 12 >= 0) continue;
        /* 3. The foot of the perpendicular has to land on the segment. */
        int32_t tx = w->x2 - w->x1, tz = w->z2 - w->z1;
        int32_t len = crw_isqrt(tx * tx + tz * tz);
        if (len <= 0) continue;
        int32_t along = ((ex * tx) + (ez * tz)) / len;
        if (along < 0 || along > len) continue;
        /* 4. And the face has to be tall enough to be worth climbing — a
              knee-high retaining step is not a wall to a crawler. y_min is the
              TOP and y_max the bottom, so the span is y_max - y_min. */
        if (w->y_max - w->y_min < CRW_HALF_H * 2) continue;

        best_dot = dot; best = i; best_t = along; best_len = len;
    }

    if (best < 0) return 0;

    {
        Wall *w = &room->walls[best];
        int32_t climb, lo, hi;
        s->surface = CRW_SURF_WALL;
        s->wall    = best;
        s->wall_t  = best_t;
        s->wall_len = best_len;
        s->wall_nx = w->nx;
        s->wall_nz = w->nz;
        s->vy      = 0;
        /* Settle CRW_CLIMB_RISE above the face's own base, clamped so the whole
           sprite stays on the face. -Y is up, so `lo` (the highest it may go) is
           the more negative bound. */
        climb = w->y_max - CRW_CLIMB_RISE;
        lo    = w->y_min - CRW_Y_OFFSET + CRW_HALF_H;
        hi    = w->y_max - CRW_Y_OFFSET - CRW_HALF_H;
        if (hi < lo) hi = lo;
        if (climb < lo) climb = lo;
        if (climb > hi) climb = hi;
        s->climb_y = climb;
        crw_place_on_wall(s);
    }
    return 1;
}

static void crw_dismount(Crawler *s) {
    s->surface     = CRW_SURF_FLOOR;
    s->wall        = -1;
    s->vy          = 0;      /* gravity takes it from here */
    s->steer_timer = 0;
}

/* ---- Update --------------------------------------------------------------- */

/* Move a mounted crawler one frame along its wall, and decide whether it should
   still be on it. */
static void crw_move_on_wall(Crawler *s, int32_t gx, int32_t gz,
                             int32_t px, int32_t py, int32_t pz, int pursuing) {
    Wall *w = &current_collision_room.walls[s->wall];
    int32_t tx = w->x2 - w->x1, tz = w->z2 - w->z1;
    int32_t before_t = s->wall_t, before_y = s->y;

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
       tools/ADDING_AN_ENEMY.txt, one surface along. */
    if (pursuing &&
        !collision_segment_blocked(s->x, s->y, s->z, px, py, pz))
        crw_dismount(s);

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
        if (!s->active || s->state == CRW_DEAD || s->area != current_area) continue;

        if (s->hit_timer    > 0) s->hit_timer--;
        if (s->damage_timer > 0) s->damage_timer--;
        s->moved = 0;

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

        /* ---- Paused at the far end of the retreat -------------------------- */
        if (s->state == CRW_PAUSE) {
            if (--s->pause_timer <= 0) {
                s->state       = CRW_RUSH;
                s->steer_timer = 0;
            }
            continue;                  /* stands still, and is drawn on frame 0 */
        }

        /* Gravity and the floor, for anything not clinging to something. */
        if (s->surface == CRW_SURF_FLOOR)
            apply_ddog_height(&s->x, &s->y, &s->z, &s->vy,
                              &s->on_upper_floor, &s->on_ramp);

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

        /* ---- Has the retreat finished? ------------------------------------
           A fixed distance and not the room's live fog_far — raising the
           lantern must reveal a retreated crawler, not push it further out.
           The timeout is the maze insurance: a crawler that has backed into a
           dead end turns and comes at you anyway rather than grinding there. */
        if (s->state == CRW_RETREAT) {
            if (rad2 >= (int32_t)CRW_RETREAT_DIST * CRW_RETREAT_DIST ||
                --s->retreat_timer <= 0) {
                s->state       = CRW_PAUSE;
                s->pause_timer = CRW_PAUSE_FRAMES;
                s->steer_timer = 0;
                crawler_scream();      /* "and when it pauses after a retreat" */
                continue;
            }
        }

        int pursuing = (s->state == CRW_RUSH);
        int32_t goal_dx = pursuing ?  dx : -dx;
        int32_t goal_dz = pursuing ?  dz : -dz;

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

        /* ---- Separation: soft push away from nearby crawlers ---------------- */
        int32_t sep_x = 0, sep_z = 0;
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

        int32_t desired_x = goal_dx + sep_x * CRW_SEP_WEIGHT;
        int32_t desired_z = goal_dz + sep_z * CRW_SEP_WEIGHT;
        int32_t desired_dist = (desired_x < 0 ? -desired_x : desired_x) +
                               (desired_z < 0 ? -desired_z : desired_z);
        if (desired_dist == 0) desired_dist = 1;

        /* ---- Obstacle feeler ----------------------------------------------- */
        int32_t feeler_x = s->x + (desired_x * CRW_FEELER_LEN) / desired_dist;
        int32_t feeler_z = s->z + (desired_z * CRW_FEELER_LEN) / desired_dist;
        int32_t fx = feeler_x, fz = feeler_z;
        crates_collide(&fx, s->y, &fz, 80);
        apply_flat_entity_collision(&fx, &fz, CRW_BODY_RADIUS);
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
           wall it has run into, the crawler goes UP it and follows the face
           round. Only tried when the feeler has actually reported an obstacle,
           and crw_try_mount then insists the crawler is in FRONT of a wall it
           is heading INTO — a prop or another crawler blocking the feeler finds
           no face and falls through to the ordinary wall-follow below. */
        if (blocked && crw_try_mount(s, desired_x, desired_z)) {
            s->anim_tick++;
            any_walking = 1;
            s->moved    = 1;
            continue;
        }

        int32_t pl_x = -goal_dz, pl_z =  goal_dx;   /* left  */
        int32_t pr_x =  goal_dz, pr_z = -goal_dx;   /* right */
        int32_t goal_px = s->x + goal_dx;
        int32_t goal_pz = s->z + goal_dz;

        if (blocked && s->steer_timer <= 0) {
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
            apply_flat_entity_collision(&tlx, &tlz, CRW_BODY_RADIUS);
            int left_blocked = (tlx != lx || tlz != lz);

            int32_t trx = rx, trz = rz;
            crates_collide(&trx, s->y, &trz, 80);
            apply_flat_entity_collision(&trx, &trz, CRW_BODY_RADIUS);
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

        if (s->steer_timer > 0) {
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
        apply_flat_entity_collision(&s->x, &s->z, CRW_BODY_RADIUS);
        crates_collide(&s->x, s->y, &s->z, 80);
        fatdoors_collide(&s->x, s->y, &s->z, CRW_DOOR_CLEARANCE);
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
       THIS IS ALSO THE RETREAT PAYING OFF: CRW_RETREAT_DIST is set just past
       the room's unlit fog_far, so a fully retreated crawler drops out here and
       really is gone into the dark rather than merely dim. */
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
               Oriented by the camera's right axis, which keeps it readable from
               wherever it is looked at without needing a facing of its own —
               a ceiling crawler in this game never moves (it drops the moment
               it wakes), so there is no travel direction to orient to. */
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
