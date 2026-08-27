#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <psxpad.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "title.h"
#include "player.h"
#include "collision.h"
#include "demondog.h"
#include "zombie.h"
#include "spider.h"
#include "tentacle.h"
#include "rafflesia.h"
#include "mushroom.h"
#include "rabisu.h"
#include "living_statue.h"
#include "vampire.h"
#include "particles.h"   /* spawn_blood_burst, for the vampire */
#include "damage.h"
#include "weapon.h"
#include "helluminator.h"

extern volatile uint8_t pad_buff[2][34];
extern volatile size_t  pad_buff_len[2];

/* ---- The burn ---------------------------------------------------------------
   ONE counter pays for both halves. burn_frames climbs by one for every frame
   Square is held and everything comes off the same 60: at 60 it spends a unit of
   oil AND does a point of damage to everything in the cone, then drops 60 and
   keeps whatever is left over. That leftover is what makes a stuttered trigger
   honest — three bursts of twenty frames cost exactly what one of sixty does,
   and the burn cannot be tapped for free.

   It is also why oil and damage can never disagree: they are not two timers
   agreeing, they are one event with two effects. */
#define HELL_TICK_FRAMES   60   /* 1 s at 60 fps: 1 oil, 1 hp */
#define HELL_TICK_DAMAGE    1

/* ---- Reach ------------------------------------------------------------------
   THREE TIMES THE GUN'S CROSSHAIR CIRCLE (14 -> 42 px), matching the reticule
   drawn at three times the size — what the player sees is what burns.

   The RANGE is deliberately shorter than the gun's 4000. A revolver reaches
   across the room; a lantern lights what is near it, and 1800 is about as far as
   any of the garden rooms' fog lets you see a target clearly anyway. Making the
   circle wider without shortening the reach would have turned it into a
   room-clearing weapon rather than a close one. */
#define HELL_AIM_RADIUS    42
#define HELL_RANGE       1800

/* ---- Hold pose (view space) -------------------------------------------------
   The Grave-olver's, and on purpose: this model has the same shape in the same
   axis (a long handle down +X with the business end at -X), so the same yaw puts
   the lamp forward and to the left with the grip near the camera. See the block
   in graveolver.c for what the numbers mean. */
#define HELL_VS_X    65
#define HELL_VS_Y    70
#define HELL_VS_Z   170
#define HELL_ROT_X    0
#define HELL_ROT_Y  741
#define HELL_ROT_Z    0

/* The base colours here are mid-dark rather than the Grave-olver's near-black,
   so 2.0x is enough. At 5.0x (the gun's) the grey handle clips to white. */
#define HELL_BRIGHTNESS 8192

/* THE PIVOT: the centre of the dark-red marker quad at the far end of the
   handle, straight out of assets/props/Helluminator.smx —
   ((25.85 + 32.17)/2, (5.70 - 3.10)/2, 0). The model swings about this point and
   this point is placed at the hold position above, so it is where the player's
   fist is. See helluminator.h. */
#define HELL_PIVOT_X   29
#define HELL_PIVOT_Y    1
#define HELL_PIVOT_Z    0

/* ---- Aim follow ------------------------------------------------------------
   The gun's numbers, halved. The lantern is held further from the body and
   swings about a point 29 units further back, so the same gains threw the lamp
   right off the side of the screen at full aim deflection. */
#define HELL_AIM_YAW      55
#define HELL_AIM_YAW_R   210
#define HELL_AIM_PITCH    55

/* ---- The glow ---------------------------------------------------------------
   The three lamp faces — two orange panes and the yellow end cap — get an
   additive white quad laid over them, grown out from the face's own screen
   centre so the light spills past the glass. Two levels: a small permanent one
   (the wick is always lit) and a larger, brighter one while Square is held.

   >>> IT LIGHTS THE MODEL, NOT THE ROOM. <<< Every room's mesh loop shades from
   its own fog curve and nothing else; making the lantern brighten nearby world
   polys would mean a per-poly distance-to-player term in eleven draw loops, and
   tools/DIAGNOSING_FRAME_RATE.txt is unambiguous about what an extra per-poly
   term costs in the rooms that are already tight. The glow is on the lamp and
   the lamp alone.

   SCALE is in 256ths of the face's own half-size about its screen centre: 320 is
   1.25x, 448 is 1.75x. LEVEL is the additive white, 0..255 — additive, so it
   only ever brightens what is behind it and needs no sorting against itself. */
#define HELL_GLOW_SCALE_IDLE   320
#define HELL_GLOW_SCALE_BURN   448
#define HELL_GLOW_LEVEL_IDLE    56
#define HELL_GLOW_LEVEL_BURN   168
/* Frames the burn glow takes to come up and to die away, so the light swells
   rather than snapping. */
#define HELL_GLOW_RAMP          10

/* The model's own marker colours, matched exactly against the SMD's per-prim
   base RGB (offset 16). They come through smxlink unchanged — that is checked,
   not assumed — so this is a reliable way to name three faces out of thirty
   without hard-coding primitive indices that a re-export would move. */
#define IS_RGB_(p, R, G, B) ((p)[16] == (R) && (p)[17] == (G) && (p)[18] == (B))
/* The extra hop is what lets a three-part colour macro be passed as ONE
   argument: the preprocessor counts a macro's arguments BEFORE expanding them,
   so IS_RGB_(p, HELL_LAMP_OR) is "two arguments" and only becomes four once it
   has been handed on. */
#define IS_RGB(p, RGB)  IS_RGB_(p, RGB)
#define HELL_MARKER_R  204, 0, 0      /* the pivot marker: NEVER drawn      */
#define HELL_LAMP_OR   204, 53, 0     /* the two lantern panes             */
#define HELL_LAMP_YL   204, 201, 21   /* the lantern's end cap             */

/* Front OT layer for the 2D reticule. The gun uses 3 for the same thing; HUD
   owns 0/1 and the scene starts at SCENE_OT_MIN, so 3 is free either way and
   only one weapon is ever drawing. */
#define OT_HELL_RETICULE  3

/* The band the model's faces are remapped into — the Grave-olver's, for the
   reason weapon_render_model documents: the weapon has no depth buffer, so
   correct self-occlusion depends entirely on spreading its faces across OT
   buckets, and the band has to stay in front of world geometry (>= ~41). */
#define HELL_OT_SPAN  24

static SMD  *hell_smd  = NULL;
static void *hell_buff = NULL;

static int burning     = 0;   /* Square is down AND there is oil to burn */
static int burn_frames = 0;   /* see HELL_TICK_FRAMES */
static int glow_ramp   = 0;   /* 0..HELL_GLOW_RAMP, the swell */

int helluminator_burning(void) { return burning; }

void helluminator_cancel_burn(void) { burning = 0; }

void helluminator_init(void) {
    CdlFILE file;
    /* \TEX\, not the disc root: check_disc_root.py reports 36 free bytes in the
       root's first sector and one more directory record is 60. The Grave-olver
       is at the root only because it got there first. */
    if (!CdSearchFile(&file, "\\TEX\\HELLUM.SMD;1")) return;
    int sectors = (file.size + 2047) / 2048;
    hell_buff   = malloc(sectors * 2048);
    if (!hell_buff) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)hell_buff, CdlModeSpeed);
    CdReadSync(0, NULL);
    hell_smd = smdInitData(hell_buff);
}

/* ---- One second of burning -------------------------------------------------
   EVERY enemy whose body falls inside the crosshair circle takes the hit, not
   just the nearest — that is the one way this uses the shared aim test
   differently from the gun, and the reason is in helluminator.h. The blocked
   test is still applied per target, so a zombie behind a wall does not burn
   because a zombie in the doorway does.

   The list below is the same list graveolver_fire sweeps, in the same order,
   and it has to stay that way: an enemy added to one and not the other is
   immune to one weapon and nothing says so. THE ONE DELIBERATE EXTRA is the
   Living Statue, which the gun has no loop for at all and never will — see the
   block at the end. The VINE CURTAINS are deliberately
   absent — they answer to DMG_FLAME and holy fire is not that (see vines.h),
   and burning down scenery is not what this weapon is for. */
static void hell_burn_tick(void) {
    int32_t fx = isin(cam_rot), fz = icos(cam_rot);
    int32_t depth;
    int i;

    #define HIT(EX, CY, EZ, HW, HH) \
        (weapon_aim_in_circle((EX), (CY), (EZ), (HW), (HH), fx, fz, \
                              HELL_AIM_RADIUS, HELL_RANGE, &depth) && \
         weapon_aim_clear(fx, fz, depth))

    for (i = 0; i < demon_dog_count; i++) {
        DemonDog *d = &demon_dogs[i];
        if (!d->active || d->state == DDOG_DEAD) continue;
        if (HIT(d->x, d->y + DDOG_Y_OFFSET, d->z, DDOG_HALF_W, DDOG_HALF_H))
            demon_dog_damage(d, demon_dog_scale_damage(HELL_TICK_DAMAGE, DMG_HOLY));
    }
    for (i = 0; i < zombie_count; i++) {
        Zombie *z = &zombies[i];
        if (!z->active || z->state == ZMB_DEAD) continue;
        if (HIT(z->x, z->y + ZMB_Y_OFFSET, z->z, ZMB_HALF_W, ZMB_HALF_H))
            zombie_damage(z, zombie_scale_damage(HELL_TICK_DAMAGE, DMG_HOLY));
    }
    for (i = 0; i < spider_count; i++) {
        Spider *s = &spiders[i];
        if (!s->active || s->state == SPD_DEAD || s->area != current_area) continue;
        if (HIT(s->x, s->y + SPD_Y_OFFSET, s->z, SPD_HALF_W, SPD_HALF_H))
            spider_damage(s, spider_scale_damage(HELL_TICK_DAMAGE, DMG_HOLY));
    }
    for (i = 0; i < tentacle_count; i++) {
        Tentacle *t = &tentacles[i];
        if (!t->active || t->health <= 0 || t->area != current_area) continue;
        int32_t cyc, hh, hw;
        tentacle_body(t, &cyc, &hh, &hw);
        if (HIT(t->x, cyc, t->z, hw, hh))
            tentacle_shoot(t, tentacle_scale_damage(HELL_TICK_DAMAGE, DMG_HOLY));
    }
    for (i = 0; i < rafflesia_count; i++) {
        Rafflesia *rf = &rafflesias[i];
        if (!rf->active || rf->health <= 0 || rf->area != current_area) continue;
        int32_t cyc, hh, hw;
        rafflesia_body(rf, &cyc, &hh, &hw);
        if (HIT(rf->x, cyc, rf->z, hw, hh))
            rafflesia_shoot(rf, rafflesia_scale_damage(HELL_TICK_DAMAGE, DMG_HOLY));
    }
    for (i = 0; i < mushroom_count; i++) {
        Mushroom *m = &mushrooms[i];
        if (!m->active || m->state == MSH_DEAD || m->area != current_area) continue;
        int32_t cyc, hh, hw;
        mushroom_body(m, &cyc, &hh, &hw);
        if (HIT(m->x, cyc, m->z, hw, hh))
            mushroom_damage(m, mushroom_scale_damage(HELL_TICK_DAMAGE, DMG_HOLY));
    }
    for (i = 0; i < rabisu_count; i++) {
        Rabisu *rb = &rabisus[i];
        /* `dying` as well as `dead`: the boss stays on screen through its whole
           death sequence, and a second of burning into a corpse should miss. */
        if (!rb->active || rb->dead || rb->dying || rb->area != current_area) continue;
        int32_t cyc, hh, hw;
        rabisu_body(rb, &cyc, &hh, &hw);
        if (HIT(rb->x, cyc, rb->z, hw, hh))
            rabisu_damage(rb, rabisu_scale_damage(HELL_TICK_DAMAGE, DMG_HOLY));
    }
    /* >>> LIVING STATUES, AND THIS WEAPON ONLY. <<< The one entry in this sweep
       with no counterpart in graveolver_fire, and the exception to the rule two
       comments up that the two lists must match: there is no Grave-olver block
       for this enemy anywhere, by its own design (living_statue.h). The lantern
       is the answer to the dead end that left — the only thing that can hurt a
       statue which is not already striking.

       living_statue_burn, NOT living_statue_damage: the state gate that lets a
       hit through in STALK, and the wake-up that follows one, live entirely
       behind that call, so nothing about when a statue may be burnt is decided
       here. A burn on one that has not left its plinth is refused there, and
       silently — the same silence the crucifaxe answers with. */
    for (i = 0; i < living_statue_count; i++) {
        LivingStatue *s = &living_statues[i];
        if (!s->active || s->state == LST_DEAD || s->area != current_area)
            continue;
        int32_t cyc, hh, hw;
        living_statue_body(s, &cyc, &hh, &hw);
        if (HIT(s->x, cyc, s->z, hw, hh))
            living_statue_burn(s, living_statue_scale_damage(HELL_TICK_DAMAGE,
                                                             DMG_HOLY));
    }
    if (vampire_health > 0 &&
        HIT(vampire_x, vampire_y + VAMPIRE_Y, vampire_z,
            VAMPIRE_HALF_W, VAMPIRE_HALF_H)) {
        vampire_health   -= vampire_scale_damage(HELL_TICK_DAMAGE, DMG_HOLY);
        vampire_hit_timer = VAMPIRE_BAR_TIMER_MAX;
        if (vampire_health <= 0)
            spawn_blood_burst(vampire_x, vampire_y, vampire_z);
    }

    #undef HIT
}

void helluminator_update(void) {
    int square_held = 0;
    if (pad_buff_len[0]) {
        PadResponse *pad = (PadResponse *)pad_buff[0];
        square_held = (~pad->btn & PAD_SQUARE) ? 1 : 0;
    }

    /* No edge detect: Square is a HOLD here, which is the whole difference
       between this and the gun. An empty lantern simply will not light. */
    burning = square_held && player_oil > 0 && game_state != STATE_MENU;

    if (burning) {
        if (glow_ramp < HELL_GLOW_RAMP) glow_ramp++;
        burn_frames++;
        if (burn_frames >= HELL_TICK_FRAMES) {
            burn_frames -= HELL_TICK_FRAMES;
            player_oil--;
            if (player_oil < 0) player_oil = 0;
            hell_burn_tick();
        }
    } else if (glow_ramp > 0) {
        glow_ramp--;
    }
}

/* ---- Drawing ---------------------------------------------------------------
   The Grave-olver's two-pass renderer (weapon_render_model), with two changes
   that are exactly why it is not simply called: the red pivot marker is SKIPPED,
   and the three lamp faces get their screen coordinates captured on the way past
   so the glow can be laid over them without projecting anything twice. */

/* One additive white quad, in 2D, over four already-projected screen points,
   grown out from their centre by `scale`/256. Additive means a DR_TPAGE has to
   be IN FRONT of the poly in OT order, and since the OT is LIFO within a bucket
   that means adding the TPAGE to the same node immediately AFTER the quad — the
   same shape the Attic Exit's light cones use. */
static void hell_glow_quad(RenderContext *ctx, const DVECTOR *sv,
                           int32_t scale, int32_t level, int otz) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_F4) + sizeof(DR_TPAGE) > buf_end) return;

    int32_t cx = ((int32_t)sv[0].vx + sv[1].vx + sv[2].vx + sv[3].vx) / 4;
    int32_t cy = ((int32_t)sv[0].vy + sv[1].vy + sv[2].vy + sv[3].vy) / 4;

    int16_t gx[4], gy[4];
    int k;
    for (k = 0; k < 4; k++) {
        int32_t x = cx + (((int32_t)sv[k].vx - cx) * scale >> 8);
        int32_t y = cy + (((int32_t)sv[k].vy - cy) * scale >> 8);
        /* The GPU's coordinate limit, the same clamp the room loops apply. A
           halo that grew past it would wrap rather than clip. */
        if (x < -1023) x = -1023; if (x > 1023) x = 1023;
        if (y < -1023) y = -1023; if (y > 1023) y = 1023;
        gx[k] = (int16_t)x; gy[k] = (int16_t)y;
    }

    uint32_t *ot = ctx->buffers[ctx->active_buffer].ot;

    POLY_F4 *p = (POLY_F4 *)ctx->next_packet;
    setPolyF4(p);
    setSemiTrans(p, 1);
    setRGB0(p, (uint8_t)level, (uint8_t)level, (uint8_t)level);
    p->x0 = gx[0]; p->y0 = gy[0];
    p->x1 = gx[1]; p->y1 = gy[1];
    p->x2 = gx[2]; p->y2 = gy[2];
    p->x3 = gx[3]; p->y3 = gy[3];
    addPrim(&ot[otz], p);
    ctx->next_packet += sizeof(POLY_F4);

    DR_TPAGE *tp = (DR_TPAGE *)ctx->next_packet;
    setDrawTPage(tp, 0, 0, getTPage(0, 1 /* ABR=1: additive */, 320, 0));
    addPrim(&ot[otz], tp);
    ctx->next_packet += sizeof(DR_TPAGE);
}

void draw_helluminator(RenderContext *ctx) {
    if (!hell_smd) return;

    /* The switch slide, shared with the other two weapons: +Y is down in view
       space, so the drop is added to the hold-pose Y. There is no reload dip
       here — nothing about a lantern reloads. */
    int32_t drop = weapon_switch_offset();

    /* Aim-follow: angle the lantern toward the crosshair while aiming, yaw with
       its horizontal offset and pitch with its vertical one. Same signs as the
       gun (crosshair down => lamp down). */
    int32_t aim_yaw = 0, aim_pitch = 0;
    if (aiming) {
        int32_t dx = aim_x - SCREEN_XRES / 2;
        int32_t yaw_gain = (dx > 0) ? HELL_AIM_YAW_R : HELL_AIM_YAW;
        aim_yaw   =  (dx * yaw_gain) / 100;
        aim_pitch = -((aim_y - SCREEN_YRES / 2) * HELL_AIM_PITCH) / 100;
    }

    /* ---- The pivot ---------------------------------------------------------
       A rotation about a point P rather than the origin is R with the
       translation P - R*P baked in; placing that pivot at the hold position H
       makes the whole transform  v' = R*v + (H - R*P).  R*P is worked out by
       hand rather than through the GTE because this runs before any of the
       model's own loads and there is no reason to disturb the register file for
       three multiply-adds. Same identity lever.c uses for its blue cap. */
    SVECTOR rot = {(int16_t)(HELL_ROT_X + aim_pitch),
                   (int16_t)(HELL_ROT_Y + aim_yaw),
                   HELL_ROT_Z, 0};
    MATRIX  weapon_vs;
    RotMatrix(&rot, &weapon_vs);
    {
        int32_t px = HELL_PIVOT_X, py = HELL_PIVOT_Y, pz = HELL_PIVOT_Z;
        int32_t rx = (weapon_vs.m[0][0] * px + weapon_vs.m[0][1] * py +
                      weapon_vs.m[0][2] * pz) >> 12;
        int32_t ry = (weapon_vs.m[1][0] * px + weapon_vs.m[1][1] * py +
                      weapon_vs.m[1][2] * pz) >> 12;
        int32_t rz = (weapon_vs.m[2][0] * px + weapon_vs.m[2][1] * py +
                      weapon_vs.m[2][2] * pz) >> 12;
        weapon_vs.t[0] = HELL_VS_X        - rx;
        weapon_vs.t[1] = HELL_VS_Y + drop - ry;
        weapon_vs.t[2] = HELL_VS_Z        - rz;
    }

    gte_SetRotMatrix(&weapon_vs);
    gte_SetTransMatrix(&weapon_vs);

    /* Light direction in model space: upper-right-front, (1,-1,1)/sqrt(3)*4096.
       weapon_render_model's, so the lantern is lit like the other two weapons. */
    const int32_t lx = 2365, ly = -2365, lz = 2365;

    uint8_t *p       = (uint8_t *)hell_smd->p_prims;
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    int pi;

    /* Pass 1 — this model's near/far OT range, so pass 2 can remap it into the
       front band. See weapon_render_model for the whole argument. The marker is
       included here: leaving it out would change the range and so shift every
       other face's bucket, for a poly that is not drawn. */
    int32_t min_otz = 0x7fffffff, max_otz = 0;
    for (pi = 0; pi < hell_smd->n_prims; pi++) {
        SMD_PRI_TYPE *pt     = (SMD_PRI_TYPE *)p;
        uint8_t       stride = pt->len;
        uint16_t     *vi     = (uint16_t *)(p + 4);
        int32_t sz[4], otz;
        gte_ldv3(&hell_smd->p_verts[vi[0]], &hell_smd->p_verts[vi[1]],
                 &hell_smd->p_verts[vi[2]]);
        gte_rtpt();
        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }
        if (pt->type >= 2) { gte_ldv0(&hell_smd->p_verts[vi[3]]); gte_rtps(); gte_avsz4(); }
        else               { gte_avsz3(); }
        gte_stotz(&otz);
        if (otz < min_otz) min_otz = otz;
        if (otz > max_otz) max_otz = otz;
        p += stride;
    }
    int32_t otz_span = max_otz - min_otz;
    if (otz_span < 1) otz_span = 1;

    int32_t glow_scale = HELL_GLOW_SCALE_IDLE +
                         ((HELL_GLOW_SCALE_BURN - HELL_GLOW_SCALE_IDLE) * glow_ramp) /
                         HELL_GLOW_RAMP;
    int32_t glow_level = HELL_GLOW_LEVEL_IDLE +
                         ((HELL_GLOW_LEVEL_BURN - HELL_GLOW_LEVEL_IDLE) * glow_ramp) /
                         HELL_GLOW_RAMP;

    /* Pass 2 — project again and emit. */
    p = (uint8_t *)hell_smd->p_prims;
    for (pi = 0; pi < hell_smd->n_prims; pi++) {
        SMD_PRI_TYPE *pt      = (SMD_PRI_TYPE *)p;
        uint8_t       stride  = pt->len;
        int           is_quad = (pt->type >= 2);

        /* THE MARKER IS NEVER DRAWN. It is the pivot, not art — a dark-red quad
           sitting inside the player's fist. Skipping it by COLOUR rather than by
           primitive index means a re-export that reorders the mesh cannot
           silently start drawing it. */
        if (IS_RGB(p, HELL_MARKER_R)) { p += stride; continue; }

        int is_lamp = IS_RGB(p, HELL_LAMP_OR) || IS_RGB(p, HELL_LAMP_YL);

        uint16_t *vi = (uint16_t *)(p + 4);
        SVECTOR *v0 = &hell_smd->p_verts[vi[0]];
        SVECTOR *v1 = &hell_smd->p_verts[vi[1]];
        SVECTOR *v2 = &hell_smd->p_verts[vi[2]];

        DVECTOR sv[4];
        int32_t sz[4], otz;

        gte_ldv3(v0, v1, v2);
        gte_rtpt();
        gte_stsxy3c(sv);
        gte_stsz4c(sz);
        if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

        if (is_quad) {
            SVECTOR *v3 = &hell_smd->p_verts[vi[3]];
            gte_ldv0(v3); gte_rtps(); gte_stsxy(&sv[3]);
            gte_avsz4();
        } else {
            gte_avsz3();
        }
        gte_stotz(&otz);
        otz = SCENE_OT_MIN + ((otz - min_otz) * HELL_OT_SPAN) / otz_span;
        if (otz < SCENE_OT_MIN)                  otz = SCENE_OT_MIN;
        if (otz > SCENE_OT_MIN + HELL_OT_SPAN)   otz = SCENE_OT_MIN + HELL_OT_SPAN;

        /* Per-face normal shading: 40% ambient + primary + dim fill. */
        uint16_t n0_idx = *(uint16_t *)(p + 12);
        SVECTOR *norm   = &hell_smd->p_norms[n0_idx];
        int32_t dot = ((int32_t)norm->vx * lx +
                       (int32_t)norm->vy * ly +
                       (int32_t)norm->vz * lz) >> 12;
        int32_t dot2 = -dot;
        if (dot  < 0) dot  = 0;
        if (dot2 < 0) dot2 = 0;
        int32_t shade = 1638 + ((dot * 2458) >> 12) + ((dot2 * 820) >> 12);
        if (shade > 4096) shade = 4096;
        /* The lamp faces are GLASS WITH A FLAME BEHIND THEM: they do not take
           the directional shading, or the side facing away from the key light
           would read as a dead pane while the glow in front of it burned. */
        if (is_lamp) shade = 4096;

        int32_t br = ((int32_t)p[16] * HELL_BRIGHTNESS) >> 12;
        int32_t bg = ((int32_t)p[17] * HELL_BRIGHTNESS) >> 12;
        int32_t bb = ((int32_t)p[18] * HELL_BRIGHTNESS) >> 12;
        if (br > 255) br = 255;
        if (bg > 255) bg = 255;
        if (bb > 255) bb = 255;
        uint8_t r = (uint8_t)((br * shade) >> 12);
        uint8_t g = (uint8_t)((bg * shade) >> 12);
        uint8_t b = (uint8_t)((bb * shade) >> 12);

        if (is_quad) {
            if (ctx->next_packet + sizeof(POLY_F4) > buf_end) { p += stride; continue; }
            POLY_F4 *poly = (POLY_F4 *)ctx->next_packet;
            setPolyF4(poly);
            setRGB0(poly, r, g, b);
            poly->x0=sv[0].vx; poly->y0=sv[0].vy;
            poly->x1=sv[1].vx; poly->y1=sv[1].vy;
            poly->x2=sv[2].vx; poly->y2=sv[2].vy;
            poly->x3=sv[3].vx; poly->y3=sv[3].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F4);

            /* The halo, at the FRONT of the weapon band so it blooms over the
               lamp's own frame rather than being hidden by it. */
            if (is_lamp)
                hell_glow_quad(ctx, sv, glow_scale, glow_level, SCENE_OT_MIN);
        } else {
            if (ctx->next_packet + sizeof(POLY_F3) > buf_end) { p += stride; continue; }
            POLY_F3 *poly = (POLY_F3 *)ctx->next_packet;
            setPolyF3(poly);
            setRGB0(poly, r, g, b);
            poly->x0=sv[0].vx; poly->y0=sv[0].vy;
            poly->x1=sv[1].vx; poly->y1=sv[1].vy;
            poly->x2=sv[2].vx; poly->y2=sv[2].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_F3);
        }
        p += stride;
    }

    /* Restore the camera view matrix — every later world-space draw depends on
       it, exactly as weapon_render_model does on the way out. */
    {
        MATRIX view;
        camera_build_view(&view);
        gte_SetRotMatrix(&view);
        gte_SetTransMatrix(&view);
    }

    if (game_state == STATE_MENU) return;

    /* ---- The reticule ------------------------------------------------------
       >>> ONLY WHILE AIMING. <<< The ring is 42 pixels across and would sit over
       the middle of the screen at all times otherwise — the lantern is a weapon
       the player walks around with lit, unlike the gun, so an always-on
       crosshair is a permanent hole in the view rather than the gun's small
       cross. `aiming` is the aim-button hold (see camera.c), which is also
       exactly when aim_x/aim_y mean anything: outside it they sit parked at the
       rest position and the ring would be pointing at nothing in particular.

       A RING, not the Grave-olver's cross, and drawn at exactly HELL_AIM_RADIUS
       — so the circle on screen IS the hit circle rather than a symbol for it.
       That is worth more here than on the gun: at 42 pixels the slop is wide
       enough that the player would otherwise have no idea how much of it there
       was.

       Two concentric rings a pixel apart, because a LINE_F2 is one pixel wide
       and a single 42-radius hairline reads as a smudge against a fogged garden
       room; the pair gives it the same 2px weight the gun's arms have.

       It brightens with the flame, so the reticule doubles as the "it is lit"
       tell alongside the HUD's oil count. */
    if (aiming) {
        const int SEGS = 24;
        int cx = aim_x, cy = aim_y;
        uint8_t v = (uint8_t)(200 + ((255 - 200) * glow_ramp) / HELL_GLOW_RAMP);
        uint8_t *buf_end2 = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
        int ring;
        for (ring = 0; ring < 2; ring++) {
            int32_t rad = HELL_AIM_RADIUS - ring;
            /* isin/icos: angle 0 is +X, so the ring starts at three o'clock and
               closes back onto it after SEGS steps. */
            int prev_x = cx + rad, prev_y = cy;
            int seg;
            for (seg = 1; seg <= SEGS; seg++) {
                int32_t ang = (seg * 4096) / SEGS;
                int nx = cx + ((icos(ang) * rad) >> 12);
                int ny = cy + ((isin(ang) * rad) >> 12);
                if (ctx->next_packet + sizeof(LINE_F2) > buf_end2) return;
                LINE_F2 *ln = (LINE_F2 *)ctx->next_packet;
                setLineF2(ln);
                setRGB0(ln, v, v, v);
                setXY2(ln, prev_x, prev_y, nx, ny);
                addPrim(&ctx->buffers[ctx->active_buffer].ot[OT_HELL_RETICULE], ln);
                ctx->next_packet += sizeof(LINE_F2);
                prev_x = nx; prev_y = ny;
            }
        }
    }
}
