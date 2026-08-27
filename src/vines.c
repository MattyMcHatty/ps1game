#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include <smd/smd.h>
#include "render.h"
#include "camera.h"
#include "particles.h"
#include "sound.h"
#include "damage.h"
#include "title.h"
#include "vines.h"

/* The room the player is in; curtains of other areas are skipped. NOT
   game_state — with the inventory menu open that is STATE_MENU and no curtain
   would match, so they would blink out of the room behind the menu. The fat
   door's note on this is the long version. */
extern GameState current_area;

Vine vines[MAX_VINES];
int  vine_count = 0;

static Vine  vine_defaults[MAX_VINES];
static SMD  *vine_smd    = NULL;
static void *vine_buffer = NULL;

/* NO TEXTURE LOAD HERE, which is the one asset difference from the fat door.
   vines.tim is not resident: it is streamed into the garden-west bank on entry
   to the Greenhouse, alongside that room's own five, because this prop appears
   in that room and nowhere else (see greenhouse_upload_textures). smxlink baked
   its tpage/clut into the mesh from the same .tim, so the draw loop below reads
   them straight out of the primitive the way the fat door reads wd_dr's. */
static void load_file(const char *name, void **buf_out) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)name)) { *buf_out = NULL; return; }
    int sectors = (file.size + 2047) / 2048;
    *buf_out = malloc(sectors * 2048);
    if (!*buf_out) return;
    CdControl(CdlSetloc, &file.pos, NULL);
    CdRead(sectors, (uint32_t *)*buf_out, CdlModeSpeed);
    CdReadSync(0, NULL);
}

void vines_load_assets(void) {
    load_file("\\TEX\\VINES.SMD;1", &vine_buffer);
    if (vine_buffer)
        vine_smd = smdInitData(vine_buffer);
}

/* ---- The raise ------------------------------------------------------------
   One in flight at a time; see the block on vines_raise_start in vines.h for why
   `lift` is draw-only. raise_len is kept per-raise rather than as a constant so
   the puzzle that starts one owns its pacing. */
static int     raise_index = -1;
static int32_t raise_off   = 0;
static int32_t raise_t     = 0;
static int32_t raise_len   = 0;

void vines_raise_start(int i, int32_t frames) {
    if (i < 0 || i >= vine_count) return;
    if (!vines[i].active || vines[i].state != VINE_INTACT) return;
    if (frames < 1) frames = 1;
    raise_index = i;
    raise_len   = frames;
    raise_t     = 0;
    raise_off   = 0;
    vines[i].lift = 0;
}

int vines_raise_update(void) {
    if (raise_index < 0) return 1;

    Vine *v = &vines[raise_index];
    raise_t++;
    raise_off = (VINE_HEIGHT * raise_t) / raise_len;
    if (raise_t >= raise_len) {
        raise_off = VINE_HEIGHT;
        /* Ends where a burnt curtain ends, so world.c's delta records it with
           the bit it already had: health 0 reads back as VINE_CLEARED. */
        v->lift   = VINE_HEIGHT;
        v->health = 0;
        v->state  = VINE_CLEARED;
        raise_index = -1;
        return 1;
    }
    v->lift = raise_off;
    return 0;
}

int vine_in_area(GameState area) {
    int i;
    for (i = 0; i < vine_count; i++)
        if (vines[i].active && vines[i].state == VINE_INTACT && vines[i].area == area)
            return i;
    return -1;
}

void vines_init(void) {
    int i = 0;

    /* GREENHOUSE, the gap between the nave and the empty west annexe. That gap
       is not a hole in a plane, it is a SHORT TUNNEL: the mesh gives it a floor
       at y=0 and a ceiling at y=-900 over x[-3200,-3100] z[-2000,-1400], with
       jamb faces on both z ends running the full 900, and the collision walls 16
       and 19 are its two sides. So it is 100 x 600 x 900 — which is the vine
       model's bounding box to the unit (x[-50,50], z[-300,300], y[-900,0]). It
       goes in unrotated at the tunnel's centre, x = (-3200 + -3100)/2, and fills
       it exactly.

       Note the ORIGIN IS AT THE FOOT, so y is the floor (0) and the curtain
       hangs up to -900 — see VINE_HEIGHT. Centring it the way a fat door is
       centred would bury the bottom half.

       INDESTRUCTIBLE, AND THAT IS THE PUZZLE'S DOING RATHER THAN A DEAD END.
       Neither the axe nor a Flame Round touches it; what opens it is the ten
       pipe buttons on the nave's two walls, which wind it up into the roof
       (src/greenhouse_puzzle.c calls vines_raise_start on this instance). So it
       reads as a route the whole time and is one only once the board is
       answered — which is the reason it is `destructible = 0` and not simply
       tough. */
    vines[i].x = -3150; vines[i].y = 0; vines[i].z = -1700;
    vines[i].lift  = 0;
    vines[i].rot_y = 0;
    vines[i].half_x = VINE_HALF_X; vines[i].half_z = VINE_HALF_Z;
    vines[i].destructible = 0;
    vines[i].state = VINE_INTACT; vines[i].active = 1;
    vines[i].area = STATE_GREENHOUSE; i++;

    vine_count = i;

    int j;
    for (j = 0; j < vine_count; j++) {
        vines[j].health = VINE_MAX_HEALTH;
        vine_defaults[j] = vines[j];
    }
}

void vines_reset(void) {
    int i;
    /* Also drops a raise in flight. Nothing can legitimately be mid-travel here
       — a reset is a new game or a save rebuild — but leaving raise_index
       pointing into a re-placed array would wind the wrong curtain up. */
    raise_index = -1;
    raise_off   = 0;
    raise_t     = 0;
    raise_len   = 0;
    for (i = 0; i < vine_count; i++)
        vines[i] = vine_defaults[i];
}

/* The curtain's solid Y span. The origin is at the FOOT, so the box runs from
   the floor UP (which is -Y here) to y - VINE_HEIGHT. */
static int vine_spans_y(const Vine *v, int32_t y) {
    return y <= v->y && y >= v->y - VINE_HEIGHT;
}

void vines_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < vine_count; i++) {
        Vine *v = &vines[i];
        if (!v->active || v->state != VINE_INTACT) continue;
        if (v->area != current_area) continue;
        if (!vine_spans_y(v, py)) continue;

        int32_t min_x = v->x - v->half_x - radius - VINE_PUSH_MARGIN;
        int32_t max_x = v->x + v->half_x + radius + VINE_PUSH_MARGIN;
        int32_t min_z = v->z - v->half_z - radius - VINE_PUSH_MARGIN;
        int32_t max_z = v->z + v->half_z + radius + VINE_PUSH_MARGIN;

        if (*px <= min_x || *px >= max_x) continue;
        if (*pz <= min_z || *pz >= max_z) continue;

        /* Push out along the axis with the smallest penetration. */
        int32_t push_l = *px - min_x;
        int32_t push_r = max_x - *px;
        int32_t push_f = *pz - min_z;
        int32_t push_b = max_z - *pz;

        int32_t min_push = push_l, px_delta = -push_l, pz_delta = 0;
        if (push_r < min_push) { min_push = push_r; px_delta =  push_r; pz_delta = 0; }
        if (push_f < min_push) { min_push = push_f; px_delta = 0; pz_delta = -push_f; }
        if (push_b < min_push) {                    px_delta = 0; pz_delta =  push_b; }

        *px += px_delta;
        *pz += pz_delta;
    }
}

int vines_point_solid(int32_t x, int32_t y, int32_t z, int32_t slack) {
    int i;
    for (i = 0; i < vine_count; i++) {
        Vine *v = &vines[i];
        if (!v->active || v->state != VINE_INTACT) continue;
        if (v->area != current_area) continue;
        /* Real solid box: true footprint + real height, no push margin. */
        if (!vine_spans_y(v, y)) continue;
        if (x < v->x - v->half_x - slack || x > v->x + v->half_x + slack) continue;
        if (z < v->z - v->half_z - slack || z > v->z + v->half_z + slack) continue;
        return 1;
    }
    return 0;
}

int vines_any_solid(void) {
    int i;
    for (i = 0; i < vine_count; i++) {
        Vine *v = &vines[i];
        if (v->active && v->state == VINE_INTACT && v->area == current_area)
            return 1;
    }
    return 0;
}

/* Centre and half-extents for the gun's circle test. The box is the model's, so
   the centre sits half a curtain's height above the floor. `half_w` is the wider
   of the two footprint halves — the crosshair test is screen-space and does not
   know which way the curtain faces. */
void vines_body(int i, int32_t *out_cy, int32_t *out_half_h, int32_t *out_half_w) {
    Vine *v = &vines[i];
    *out_cy     = v->y - (VINE_HEIGHT / 2);
    *out_half_h = VINE_HEIGHT / 2;
    *out_half_w = (v->half_x > v->half_z) ? v->half_x : v->half_z;
}

/* Leaf litter rather than wood chips: the same burst, in the greenery's own
   colour, so a hit reads as plant matter. */
static void vine_burst(const Vine *v) {
    spawn_burst(v->x, v->y - (VINE_HEIGHT / 2), v->z, 60, 120, 40);
}

void vines_shoot(int i, int damage_type) {
    Vine *v = &vines[i];
    if (!v->active || v->state != VINE_INTACT) return;

    vine_burst(v);

    /* ONLY FIRE CLEARS THEM, and it clears them outright. A Standard Round tears
       leaves off and changes nothing — which is the point of the pairing: the
       Flame Rounds are the answer to overgrowth, and the player is meant to work
       that out by trying the other one first. An indestructible curtain burns
       exactly as badly as it chops: not at all. */
    if (damage_type != DMG_FLAME || !v->destructible) {
        sound_play(SFX_AXEHIT);
        return;
    }

    v->health -= VINE_FLAME_DAMAGE;
    if (v->health <= 0) {
        v->health = 0;
        v->state  = VINE_CLEARED;
        sound_play(SFX_SMASH);
    } else {
        sound_play(SFX_AXEHIT);
    }
}

int vines_try_smash(void) {
    int i, hit_any = 0;

    for (i = 0; i < vine_count; i++) {
        Vine *v = &vines[i];
        if (!v->active || v->state != VINE_INTACT) continue;
        if (v->area != current_area) continue;
        if (!vine_spans_y(v, cam_y)) continue;

        int32_t min_x = v->x - v->half_x - VINE_SMASH_RANGE;
        int32_t max_x = v->x + v->half_x + VINE_SMASH_RANGE;
        int32_t min_z = v->z - v->half_z - VINE_SMASH_RANGE;
        int32_t max_z = v->z + v->half_z + VINE_SMASH_RANGE;

        if (cam_x < min_x || cam_x > max_x) continue;
        if (cam_z < min_z || cam_z > max_z) continue;

        /* Reject if the curtain is behind the player. */
        int32_t cdx = (v->x - cam_x) >> 4;
        int32_t cdz = (v->z - cam_z) >> 4;
        int32_t dot = cdx * (isin(cam_rot) >> 4) + cdz * (icos(cam_rot) >> 4);
        if (dot <= 0) continue;

        /* One hit per swing (the caller guards re-entry). An INDESTRUCTIBLE
           curtain takes the hit and the sound and loses nothing: the swing has
           to connect, or the player reads it as out of range and keeps trying
           from further in. */
        vine_burst(v);
        hit_any = 1;

        if (!v->destructible) { sound_play(SFX_AXEHIT); continue; }

        v->health--;
        if (v->health <= 0) {
            v->health = 0;
            v->state  = VINE_CLEARED;
            sound_play(SFX_SMASH);
        } else {
            sound_play(SFX_AXEHIT);
        }
    }
    return hit_any;
}

void vines_draw(RenderContext *ctx) {
    if (!vine_smd) return;

    MATRIX view;
    camera_build_view(&view);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    int d;
    for (d = 0; d < vine_count; d++) {
        Vine *vine = &vines[d];
        if (!vine->active || vine->state == VINE_CLEARED) continue;
        if (vine->area != current_area) continue;

        int32_t ddx = vine->x - cam_x, ddz = vine->z - cam_z;
        int32_t ddist = (ddx < 0 ? -ddx : ddx) + (ddz < 0 ? -ddz : ddz);
        if (ddist > g_fog_far) continue;

        /* The ROOM'S fog, not a private copy: g_fog_near/g_fog_far are set by
           whichever room drew this frame, so the curtain fades on exactly the
           same curve as the wall it fills a hole in. The colour is the garden's
           purple, which is where every curtain placed so far lives. */
        int32_t fog_factor = render_fog_scale(ddist);

        MATRIX vine_m, combined;
        SVECTOR dr = {0, (short)vine->rot_y, 0, 0};
        RotMatrix(&dr, &vine_m);
        /* -Y is up, so a raise SUBTRACTS from the foot's height and the curtain
           travels into the ceiling. `lift` is 0 for every curtain that is not
           being wound up, so this costs nothing in the ordinary case. */
        VECTOR pos = {vine->x, vine->y - vine->lift, vine->z};
        TransMatrix(&vine_m, &pos);
        CompMatrixLV(&view, &vine_m, &combined);
        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        /* The fat door's manual loop, primitive for primitive — same projection,
           same screen-coordinate rejects, same +40 OT bias — so the curtain
           depth-sorts against the room geometry the way that prop already does.
           The mesh is 38 primitives and every one is a textured quad, so the
           untextured branches the fat door carries are not repeated here; a
           non-FT4 primitive is skipped rather than drawn flat. */
        uint8_t *p = (uint8_t *)vine_smd->p_prims;
        int i;
        for (i = 0; i < vine_smd->n_prims; i++) {
            SMD_PRI_TYPE *pt = (SMD_PRI_TYPE *)p;
            uint8_t stride = pt->len;
            int is_quad = (pt->type >= 2);

            if (!is_quad || !pt->texture) { p += stride; continue; }

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &vine_smd->p_verts[vi[0]];
            SVECTOR *v1 = &vine_smd->p_verts[vi[1]];
            SVECTOR *v2 = &vine_smd->p_verts[vi[2]];
            SVECTOR *v3 = &vine_smd->p_verts[vi[3]];

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

            if (!pt->nocull) {
                gte_nclip();
                gte_stopz(&nclip);
                if (nclip <= 0) { p += stride; continue; }
            }

            gte_stsz4c(sz);
            if (sz[1] == 0 || sz[2] == 0 || sz[3] == 0) { p += stride; continue; }

            gte_ldv0(v3);
            gte_rtps();
            gte_stsxy(&sv[3]);
            gte_stsz(&sz[3]);
            if (sv[3].vx <= -1023 || sv[3].vx >= 1023 ||
                sv[3].vy <= -1023 || sv[3].vy >= 1023) { p += stride; continue; }
            if (sz[3] == 0) { p += stride; continue; }
            gte_avsz4();

            gte_stotz(&otz);
            if (otz <= 0) { p += stride; continue; }
            otz += 40;
            if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

            uint8_t *col = p + 16;
            uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + SKY_FOG_R * (256 - fog_factor)) >> 8);
            uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + SKY_FOG_G * (256 - fog_factor)) >> 8);
            uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + SKY_FOG_B * (256 - fog_factor)) >> 8);

            if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
            /* FT4 stride 32: UVs at 20-27, tpage at 28, clut at 30 — baked by
               smxlink from vines.tim, which is exactly where the Greenhouse
               streams it to. Nothing is overridden here. */
            uint8_t *uv = p + 20;
            POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
            setPolyFT4(poly);
            setRGB0(poly, r, g, b);
            poly->tpage = *(uint16_t *)(p + 28);
            poly->clut  = *(uint16_t *)(p + 30);
            poly->u0=uv[0]; poly->v0=uv[1];
            poly->u1=uv[2]; poly->v1=uv[3];
            poly->u2=uv[4]; poly->v2=uv[5];
            poly->u3=uv[6]; poly->v3=uv[7];
            poly->x0=sv[0].vx; poly->y0=sv[0].vy;
            poly->x1=sv[1].vx; poly->y1=sv[1].vy;
            poly->x2=sv[2].vx; poly->y2=sv[2].vy;
            poly->x3=sv[3].vx; poly->y3=sv[3].vy;
            addPrim(&ctx->buffers[ctx->active_buffer].ot[otz], poly);
            ctx->next_packet += sizeof(POLY_FT4);

            p += stride;
        }
    }

    /* Restore the plain view matrix so later world-space draws project right. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}
