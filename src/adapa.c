#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "texmgr.h"
#include "sound.h"       /* SFX_ADAPA (HOUSE bank), and the player's HURT / DIE */
#include "door_anim.h"   /* door_anim_panels_taken — see adapas_upload_textures */
#include "zombie.h"      /* the Library's zombie, for its arming condition */
#include "spider.h"      /* ...and its three spiders */
#include "adapa.h"

/* Adapa — see adapa.h for the fight, the arming rule and the VRAM. */

Adapa adapa;

/* ---- Spawn points -----------------------------------------------------------
   Three in the Piano Room, four in the Library — adapa.h has the mesh numbers
   and why each sits where it does. Stored as the point BEHIND the wall, i.e.
   with ADP_SPAWN_BEHIND already applied outward. */
typedef struct { int32_t x, y, z; } AdpPoint;

#define ADP_PIANO_Y  (ADP_PIANO_CEILING + ADP_HALF_H / 2)
#define ADP_LIB_Y    (ADP_LIB_CEILING   + ADP_HALF_H / 2)

static const AdpPoint piano_spawns[] = {
    /* the Anzu frame's centre, straight out through the west wall */
    { ADP_PIANO_WALL_X - ADP_SPAWN_BEHIND, ADP_PIANO_Y, ADP_PIANO_FRAME_Z },
    /* the west wall's north corner, out of both walls */
    { ADP_PIANO_WALL_X - ADP_SPAWN_BEHIND, ADP_PIANO_Y,
      ADP_PIANO_NORTH_Z + ADP_SPAWN_BEHIND },
    /* ...and its south corner */
    { ADP_PIANO_WALL_X - ADP_SPAWN_BEHIND, ADP_PIANO_Y,
      ADP_PIANO_SOUTH_Z - ADP_SPAWN_BEHIND },
};

static const AdpPoint library_spawns[] = {
    { ADP_LIB_WEST_X - ADP_SPAWN_BEHIND, ADP_LIB_Y,
      ADP_LIB_NORTH_Z + ADP_SPAWN_BEHIND },                      /* north-west */
    { ADP_LIB_WEST_X - ADP_SPAWN_BEHIND, ADP_LIB_Y,
      ADP_LIB_SOUTH_Z - ADP_SPAWN_BEHIND },                      /* south-west */
    { ADP_LIB_EAST_X + ADP_SPAWN_BEHIND, ADP_LIB_Y,
      ADP_LIB_SOUTH_Z - ADP_SPAWN_BEHIND },                      /* south-east */
    { ADP_LIB_EAST_X + ADP_SPAWN_BEHIND, ADP_LIB_Y,
      ADP_LIB_NORTH_Z + ADP_SPAWN_BEHIND },                      /* north-east */
};

#define ADP_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- Textures ---------------------------------------------------------------
   Two registrations, one per half of the sheet (adapa.h). TEXBANK_MANSION: both
   his rooms are in it, and py tools/check_tex_banks.py derives that from
   main.c's upload line and fails the build if the mask is short. */
#define ADP_SHEETS 2
static int adp_tex[ADP_SHEETS] = { -1, -1 };

static RECT adp_tw_restore;
static int  adp_tw_active = 0;

void adapas_set_texwindow(const RECT *tw) {
    if (tw) { adp_tw_restore = *tw; adp_tw_active = 1; }
    else    { adp_tw_active = 0; }
}

void adapas_load_textures(void) {
    texmgr_set_bank(TEXBANK_MANSION);
    adp_tex[0] = texmgr_register("\\TEX\\ADAPAA.TIM;1");
    adp_tex[1] = texmgr_register("\\TEX\\ADAPAB.TIM;1");
}

void adapas_upload_textures(void) {
    int i;
    for (i = 0; i < ADP_SHEETS; i++) texmgr_upload(adp_tex[i]);
    /* >>> AND SAY WHAT WAS JUST OVERWRITTEN. <<< x[832,960) y128 holds the exit
       door's two leaves everywhere else in the game (adapa.h). This is how
       door_anim learns they are gone; main.c's STATE_LOADING puts them back on
       entry to the next room, exactly as it does after the lumberer. */
    door_anim_panels_taken();
}

/* ---- The watch: "the conditions were met while the player stood here" ------
   Set by update_adapa(), cashed in as a saved ARMED flag by adapa_room_enter()
   when the player turns up somewhere else. Not saved (adapa.h). */
static int       watch_on   = 0;
static GameState watch_area = STATE_PIANO_ROOM;

/* Integer square root — lumberer.c's, with its `t > 0` seed (mistake 11). */
static int32_t adp_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x, last, s = 1, t = v;
    while (t > 0) { t >>= 2; s <<= 1; }
    x = s;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

void adapas_init(void) { adapa_reset(); }

void adapa_reset(void) {
    adapa       = (Adapa){0};
    adapa.state = ADP_GONE;
    watch_on    = 0;
    /* A 4.2 s clip on a voice of its own, so nothing else would cut it: a new
       game started under it would carry it into the delivery area. */
    sound_stop(SFX_ADAPA);
}

static int adp_is_his_room(GameState a) {
    return a == STATE_PIANO_ROOM || a == STATE_LIBRARY;
}

void adapa_room_enter(GameState area) {
    /* Arriving ANYWHERE other than the watched room is "the player left it". */
    if (watch_on && area != watch_area) {
        game_flag_set(watch_area == STATE_PIANO_ROOM ? FLAG_ADAPA_PIANO_ARMED
                                                     : FLAG_ADAPA_LIBRARY_ARMED);
        watch_on = 0;
    }

    /* Whatever he was doing in the last room is over — and LEAVING HEALS HIM,
       as specified: every entry starts him at full health. A fade in progress
       is simply dropped. */
    sound_stop(SFX_ADAPA);
    adapa        = (Adapa){0};
    adapa.area   = area;
    adapa.health = ADP_MAX_HEALTH;
    adapa.fade   = 256;
    adapa.spawn_idx = -1;
    if (adp_is_his_room(area)) {
        adapa.state = ADP_WAITING;
        adapa.timer = ADP_ENTRY_FRAMES;
    } else {
        adapa.state = ADP_GONE;
    }
}

/* Is he due in this room? Asked when the wait runs out, never at entry: on a
   title-screen load the flags arrive after adapa_room_enter (adapa.h). */
static int adp_armed_here(void) {
    if (game_flag(FLAG_ADAPA_DEAD)) return 0;
    if (adapa.area == STATE_PIANO_ROOM) return game_flag(FLAG_ADAPA_PIANO_ARMED);
    if (adapa.area == STATE_LIBRARY)    return game_flag(FLAG_ADAPA_LIBRARY_ARMED);
    return 0;
}

/* The Library's condition: every enemy world.c seeds there is dead. Read off
   the LIVE arrays, which hold this room's enemies while the player is in it —
   the zombie from its RoomHostiles, the spiders by their area tag. */
static int adp_library_clear(void) {
    int i;
    for (i = 0; i < zombie_count; i++) {
        Zombie *z = &zombies[i];
        if (z->active && z->state != ZMB_DEAD) return 0;
    }
    for (i = 0; i < spider_count; i++) {
        Spider *s = &spiders[i];
        if (s->active && s->state != SPD_DEAD && s->area == STATE_LIBRARY) return 0;
    }
    return 1;
}

/* Watch the room the player is standing in. Once both are true the watch is
   on, and it stays on until they arrive somewhere else (adapa_room_enter). */
static void adp_update_watch(void) {
    if (game_flag(FLAG_ADAPA_DEAD)) return;
    if (current_area == STATE_PIANO_ROOM &&
        !game_flag(FLAG_ADAPA_PIANO_ARMED) &&
        game_flag(FLAG_PIANO_SOLVED) && game_flag(FLAG_ANZU_SOLVED)) {
        watch_on = 1; watch_area = STATE_PIANO_ROOM;
    } else if (current_area == STATE_LIBRARY &&
               !game_flag(FLAG_ADAPA_LIBRARY_ARMED) && adp_library_clear()) {
        watch_on = 1; watch_area = STATE_LIBRARY;
    }
}

/* Put him at one of this room's spawn points, chosen at random, and announce
   him. */
static void adp_arrive(void) {
    const AdpPoint *pts = (adapa.area == STATE_PIANO_ROOM) ? piano_spawns
                                                           : library_spawns;
    int n = (adapa.area == STATE_PIANO_ROOM) ? ADP_COUNT(piano_spawns)
                                             : ADP_COUNT(library_spawns);
    int k = (int)((uint32_t)rand() % (uint32_t)n);
    adapa.spawn_idx    = k;
    adapa.x            = pts[k].x;
    adapa.y            = pts[k].y;
    adapa.z            = pts[k].z;
    adapa.fade         = 256;
    adapa.damage_timer = 0;
    adapa.state        = ADP_FLOAT;
    sound_play(SFX_ADAPA);
}

static const Weakness adapa_weakness[] = {
    { DMG_HOLY, 100 },   /* the Helluminator, at its plain 1 a second */
};

int32_t adapa_scale_damage(int32_t base, DamageType type) {
    return damage_scale(base, type, adapa_weakness, WEAKNESS_COUNT(adapa_weakness));
}

int adapa_burnable(void) {
    return adapa.state == ADP_FLOAT && adapa.area == current_area;
}

void adapa_body(int32_t *x, int32_t *y, int32_t *z) {
    *x = adapa.x; *y = adapa.y; *z = adapa.z;
}

void adapa_burn(int dmg) {
    if (!adapa_burnable() || dmg <= 0) return;
    adapa.health -= dmg;
    sound_play(SFX_ADAPA);
    if (adapa.health <= 0) {
        adapa.health = 0;
        adapa.state  = ADP_DYING;
        /* ON THE BLOW, not at the end of the scene: a reset or a death during
           the next ten seconds must not leave a world where he died and the
           flag was never set (hadad_grinder.c's FLAG_GRINDER_BROKEN, same
           reasoning). src/adapa_death.c sees ADP_DYING on its next update and
           takes the camera. */
        game_flag_set(FLAG_ADAPA_DEAD);
        return;
    }
    adapa.state = ADP_FADE;
    adapa.timer = ADP_FADE_FRAMES;
}

int  adapa_dying(void)          { return adapa.state == ADP_DYING &&
                                         adapa.area == current_area; }
void adapa_set_fade(int32_t f)  { adapa.fade = f < 0 ? 0 : (f > 256 ? 256 : f); }
void adapa_gone(void)           { adapa.state = ADP_GONE; adapa.fade = 0; }

void update_adapa(void) {
    static int hurt_sfx_cooldown = 0;
    if (hurt_sfx_cooldown > 0) hurt_sfx_cooldown--;

    adp_update_watch();

    /* current_area, NEVER game_state — the inventory menu is not a pause
       (tools/ADDING_AN_ENEMY.txt STEP 6). */
    if (adapa.area != current_area) return;

    switch (adapa.state) {

    case ADP_WAITING:
        if (--adapa.timer > 0) break;
        if (adp_armed_here()) adp_arrive();
        else                  adapa.state = ADP_GONE;
        break;

    case ADP_FADE:
        adapa.fade = (adapa.timer * 256) / ADP_FADE_FRAMES;
        if (--adapa.timer <= 0) {
            adapa.fade  = 0;
            adapa.state = ADP_WAITING;
            adapa.timer = ADP_RESPAWN_FRAMES;
        }
        break;

    case ADP_FLOAT: {
        if (adapa.damage_timer > 0) adapa.damage_timer--;
        adapa.anim_tick++;

        /* EVERYTHING HERE TARGETS player_x/y/z(), never cam_* — the player is
           anchored while a cutscene flies the camera (camera.h). */
        int32_t dx = player_x() - adapa.x;
        int32_t dy = player_y() + ADP_AIM_BELOW_EYE - adapa.y;
        int32_t dz = player_z() - adapa.z;

        /* --- Contact: horizontal and vertical reach tested SEPARATELY. --- */
        int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t ady    = dy < 0 ? -dy : dy;
        if (!game_over && dist2d < ADP_CATCH_DIST && ady < ADP_CATCH_DY &&
            adapa.damage_timer == 0) {
            adapa.damage_timer = ADP_DAMAGE_TICK;
            player_hurt(ADP_DAMAGE_AMOUNT);
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
        }

        /* --- The step: straight at the aim point in 3D, one gait a frame,
           normalised with a real square root so the speed does not vary with
           the angle he comes down at. Room coordinates stay within ~3000 of
           each other, so the squares are inside int32. --- */
        int32_t d3 = adp_isqrt(dx * dx + dy * dy + dz * dz);
        if (d3 > ADP_STOP_DIST) {
            int32_t step = d3 - ADP_STOP_DIST;
            if (step > ADP_SPEED) step = ADP_SPEED;
            adapa.x += (dx * step) / d3;
            adapa.y += (dy * step) / d3;
            adapa.z += (dz * step) / d3;
        }
        break;
    }

    default:   /* GONE, and DYING — the director owns him */
        break;
    }
}

/* ---- Drawing ----------------------------------------------------------------
   Bracket a run of POLY_FT4s with a full/unmasked texture window and a restore
   of the room's, all in one OT bucket — hadad.c's add_ft4_run_windowed: the
   sheet is at Voff 128 and the rooms' 128 windows would wrap its V. addPrim
   prepends, so restore, the polys back to front, then disable yields the draw
   order disable -> poly[0..n-1] -> restore. */
static void add_ft4_run_windowed(RenderContext *ctx, int32_t otz,
                                 POLY_FT4 **polys, int n) {
    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;
    int       i;

    if (adp_tw_active && ctx->next_packet + 2 * sizeof(DR_TWIN) <= buf_end) {
        DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
        setTexWindow(restore, &adp_tw_restore);
        addPrim(&ot[otz], restore);
        ctx->next_packet += sizeof(DR_TWIN);

        for (i = n - 1; i >= 0; i--) addPrim(&ot[otz], polys[i]);

        RECT full = { 0, 0, 0, 0 };   /* mask 0 = no wrapping, full page */
        DR_TWIN *disable = (DR_TWIN *)ctx->next_packet;
        setTexWindow(disable, &full);
        addPrim(&ot[otz], disable);
        ctx->next_packet += sizeof(DR_TWIN);
    } else {
        for (i = n - 1; i >= 0; i--) addPrim(&ot[otz], polys[i]);
    }
}

/* The body: a camera-facing billboard cut into a 2 x 2 grid of SHARED vertices
   (hadad.c's draw_had_sprite, at 2 x 2 because he is square — adapa.h on why
   he is cut at all). No backface cull: a camera-facing quad has no back face
   (mistake 4).

   THE FADE IS ADDITIVE (tools/ADDING_A_BOSS_ENCOUNTER.txt STEP 5, TRICK 1).
   Below fade 256 every piece is semi-transparent with ABR=1 in its own tpage
   and its colour scaled toward black, so at fade 0 it adds nothing over ANY
   background. The CLUT carries STP on every opaque entry (split_adapa_sheet.py
   --stp) — without it the PS1 would draw the semi-transparent pieces solid.

   UVs: each half is 256x128 4bpp at x832 / x896 y128. Both x values are tpage
   boundaries (13 * 64, 14 * 64), so U runs 0..255 across the half — frame 0 or
   2 at U 0..127, frame 1 or 3 at U 128..255 — and V is 128..255. Inset one
   texel on every edge so a magnified edge does not sample its neighbour. */
#define ADP_GRID      2
#define ADP_GRID_VX   ((ADP_GRID + 1) * (ADP_GRID + 1))   /* 9 */
#define ADP_GRID_QUADS (ADP_GRID * ADP_GRID)              /* 4 */

static void draw_adp_sprite(RenderContext *ctx, int frame) {
    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);
    int32_t cx = adapa.x, cy = adapa.y, cz = adapa.z;

    int16_t dwx = (int16_t)((ADP_HALF_W * rx) >> 12);
    int16_t dwz = (int16_t)((ADP_HALF_W * rz) >> 12);

    /* Row-major from the top-left, index r * 3 + c; the corners are 0, 2, 6, 8. */
    SVECTOR v[ADP_GRID_VX];
    int r, c;
    for (r = 0; r <= ADP_GRID; r++) {
        int32_t y = cy - ADP_HALF_H + (2 * ADP_HALF_H * r) / ADP_GRID;
        for (c = 0; c <= ADP_GRID; c++) {
            int32_t  k = c - 1;            /* -1, 0, +1 */
            SVECTOR *p = &v[r * (ADP_GRID + 1) + c];
            p->vx  = (int16_t)(cx + dwx * k);
            p->vy  = (int16_t)y;
            p->vz  = (int16_t)(cz + dwz * k);
            p->pad = 0;
        }
    }

    DVECTOR sv[ADP_GRID_VX];
    DVECTOR out[3];
    int32_t sz[4], otz;

    /* The corners first, so the OT sort and the behind-the-camera reject are
       the single quad's. */
    gte_ldv3(&v[0], &v[2], &v[6]);
    gte_rtpt();
    gte_stsxy3c(out);
    sv[0] = out[0]; sv[2] = out[1]; sv[6] = out[2];
    gte_ldv0(&v[8]);
    gte_rtps();
    gte_stsxy(&sv[8]);
    gte_stsz4c(sz);
    if (!sz[0] || !sz[1] || !sz[2] || !sz[3]) return;

    gte_avsz4();
    gte_stotz(&otz);
    if (otz <= 0) return;
    if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
    if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

    int32_t fdx  = adapa.x - cam_x;
    int32_t fdz  = adapa.z - cam_z;
    int32_t dist = (fdx < 0 ? -fdx : fdx) + (fdz < 0 ? -fdz : fdz);
    if (dist >= g_fog_far) return;          /* fully fogged: cull */
    int32_t fs   = render_fog_scale(dist);
    int32_t lvl  = ((fs > 255 ? 255 : fs) * adapa.fade) >> 8;
    uint8_t col  = (uint8_t)lvl;

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + ADP_GRID_QUADS * sizeof(POLY_FT4) > buf_end) return;

    /* The other five vertices: 1, 3, 4, then 5, 7 (7 repeated). */
    static const uint8_t rest[6] = { 1, 3, 4,  5, 7, 7 };
    int b;
    for (b = 0; b < 2; b++) {
        const uint8_t *ix = &rest[b * 3];
        gte_ldv3(&v[ix[0]], &v[ix[1]], &v[ix[2]]);
        gte_rtpt();
        gte_stsxy3c(out);
        sv[ix[0]] = out[0]; sv[ix[1]] = out[1]; sv[ix[2]] = out[2];
    }

    int      sheet = frame >> 1;
    uint8_t  u0    = (uint8_t)((frame & 1) * 128 + 1);
    uint8_t  u1    = (uint8_t)((frame & 1) * 128 + 126);
    uint8_t  v0    = 129, v1 = 254;
    uint8_t  uc[ADP_GRID + 1], vr[ADP_GRID + 1];
    for (c = 0; c <= ADP_GRID; c++) uc[c] = (uint8_t)(u0 + ((u1 - u0) * c) / ADP_GRID);
    for (r = 0; r <= ADP_GRID; r++) vr[r] = (uint8_t)(v0 + ((v1 - v0) * r) / ADP_GRID);

    uint16_t tpage = texmgr_tpage(adp_tex[sheet]);
    int      fading = adapa.fade < 256;
    if (fading) tpage = (uint16_t)((tpage & ~(3 << 5)) | (1 << 5));   /* ABR=1 */

    POLY_FT4 *quads[ADP_GRID_QUADS];
    int       n = 0;
    for (r = 0; r < ADP_GRID; r++) {
        for (c = 0; c < ADP_GRID; c++) {
            int tl = r * (ADP_GRID + 1) + c;
            int tr = tl + 1;
            int bl = tl + (ADP_GRID + 1);
            int br = bl + 1;

            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, col, col, col);
            if (fading) setSemiTrans(poly, 1);

            poly->x0 = sv[tl].vx; poly->y0 = sv[tl].vy;
            poly->x1 = sv[tr].vx; poly->y1 = sv[tr].vy;
            poly->x2 = sv[bl].vx; poly->y2 = sv[bl].vy;
            poly->x3 = sv[br].vx; poly->y3 = sv[br].vy;

            poly->u0 = uc[c];     poly->v0 = vr[r];
            poly->u1 = uc[c + 1]; poly->v1 = vr[r];
            poly->u2 = uc[c];     poly->v2 = vr[r + 1];
            poly->u3 = uc[c + 1]; poly->v3 = vr[r + 1];

            poly->tpage = tpage;
            poly->clut  = texmgr_clut(adp_tex[sheet]);

            ctx->next_packet += sizeof(POLY_FT4);
            quads[n++] = poly;
        }
    }
    add_ft4_run_windowed(ctx, otz, quads, n);
}

void draw_adapa(RenderContext *ctx) {
    if (adapa.area != current_area) return;
    if (adapa.state != ADP_FLOAT && adapa.state != ADP_FADE &&
        adapa.state != ADP_DYING) return;
    /* fade 0 is a SKIP, not just invisible (TRICK 1). */
    if (adapa.fade <= 0) return;

    int frame = (int)((adapa.anim_tick / ADP_ANIM_FRAMES) & 3);
    draw_adp_sprite(ctx, frame);
}
