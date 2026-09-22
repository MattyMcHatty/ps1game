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
#include "player.h"         /* player_oil, HELL_OIL_MAX, the weapon bitmask */
#include "sound.h"          /* SFX_GLUG — the pour, SND_BANK_CATACOMBS */
#include "oil_dispenser.h"

/* Oil Dispenser — see oil_dispenser.h for what it is, and for why its collision
   comes out of its own mesh and is measured as min/max rather than as the
   sconce's half-extents. */

typedef struct {
    GameState area;                        /* only draws/collides in this room */
    int32_t   x, y, z, rot_y;              /* floor corner; world y = y+GROUND_FLOOR_Y */
    int32_t   min_x, max_x, min_z, max_z;  /* world AABB, baked at place time  */
    int       active;
} OilDispenser;

static OilDispenser dispensers[MAX_OIL_DISPENSERS];
static int          dispenser_count = 0;

static SMD  *oil_smd = NULL;
static void *oil_buf = NULL;

/* The collision mesh, and it is the SAME mesh the draw uses: these six are read
   off oil_smd's vertex array at load and are the only description of the prop's
   solid volume anywhere in the game. MODEL SPACE, and signed — the model is
   offset from its origin on every axis (oil_dispenser.h says why), so there is
   no half-extent that describes it. */
static int32_t od_min_x = 0, od_max_x = 0;   /* -40 .. 0   as authored */
static int32_t od_min_z = 0, od_max_z = 0;   /*   0 .. 40             */
static int32_t od_min_y = 0, od_max_y = 0;   /* -280 .. -170, -Y is up */

/* The prop's own texture. Deferred like the rest of the chapter's art: the
   header is read at startup, the pixels only when TEXBANK_CATACOMBS is selected
   at the catacomb mouth. */
static int oil_tex = -1;

/* The player's head, relative to cam_y — the same figure apply_collision_*
   uses for its own body span, so the vertical test below agrees with the walls'.
   Feet are cam_y + GROUND_FLOOR_Y. */
#define OD_PLAYER_HEAD 30

/* ---- THE RESERVOIR ---------------------------------------------------------
   ONE scalar for the whole game, not a field of OilDispenser — oil_dispenser.h
   sets out at length why that is forced by clear()/place() running on every
   room entry, and what has to change the day a second refill point ships. Full
   at module scope so a run that never touches reset_game still starts sane. */
static int32_t reservoir = OD_OIL_MAX;

int oil_dispenser_oil(void) { return (int)reservoir; }

void oil_dispenser_set_oil(int units) {
    /* Clamped on the way in exactly as savegame.c clamps player_oil, and for
       the same reason: the bar prints it and the refill spends it, so a corrupt
       value would show as a bar off the end of its trough and a lantern filled
       from nothing. */
    if (units < 0)           units = 0;
    if (units > OD_OIL_MAX)  units = OD_OIL_MAX;
    reservoir = units;
}

void oil_dispensers_reset(void) { reservoir = OD_OIL_MAX; }

/* Pour. The arithmetic is the whole of it and it lives HERE rather than in the
   room, because the room's business is which button was pressed and what the
   log says about it — this is the only code that may move the number.

   THE FOUR CASES ARE DISTINGUISHED BEFORE ANYTHING MOVES, so a press can never
   both report a failure and spend a unit. Order matters once: a player without
   the lantern is told about the oil whether the tank is full or dry, because
   what they are being told is that there is oil here at all, and "the dispenser
   is empty" to someone who cannot use it either way is a fact about nothing. */
OdRefill oil_dispenser_refill(void) {
    if (!(player_weapons & (1 << WEAPON_HELLUMINATOR))) return OD_REFILL_NO_LANTERN;
    if (reservoir <= 0)                                 return OD_REFILL_EMPTY;

    int32_t need = HELL_OIL_MAX - player_oil;
    if (need <= 0) return OD_REFILL_FULL;

    /* "Up to a maximum of 100, and the balance out of the dispenser": the
       transfer is the SHORTFALL, capped by what is in the tank, so a tank with
       30 left in it puts 30 into a lantern that is 60 short and is then empty
       — rather than refusing because it cannot fill the lantern outright. */
    int32_t moved = (need < reservoir) ? need : reservoir;
    player_oil += (int)moved;
    reservoir  -= moved;

    /* THE GLUG, AND IT IS HERE RATHER THAN BESIDE THE LOG LINE IN THE ROOM.
       This is the only branch in the game in which oil actually moves, so it is
       the only place the sound can be fired without also firing it for a press
       that poured nothing — an empty tank, a full lantern or no lantern at all
       each get their line and no noise. STEP 8's rule, which is that the cue
       belongs at the EVENT.

       SND_BANK_CATACOMBS, so it is silent everywhere the bank is not loaded —
       which is every room but this chapter's, and this prop is only ever placed
       in one of them. */
    sound_play(SFX_GLUG);
    return OD_REFILL_DONE;
}

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
void oil_dispenser_load_assets(void) {
    /* BANK: Chapter 3 and nothing else. Derived, not guessed — py
       tools/check_tex_banks.py walks the uploader call graph (this module is
       reached from catacombs_entry_upload_textures) and fails the build if this
       mask is short. */
    texmgr_set_bank(TEXBANK_CATACOMBS);

    oil_buf = read_file("\\TEXCTCMB\\OILDISP.SMD;1");
    if (oil_buf) oil_smd = smdInitData(oil_buf);

    /* MEASURE THE MESH. This is the whole of the prop's collision authoring:
       the box below is the model's real bounding volume, so it can never drift
       from what is drawn. No symmetry is assumed — see the header. */
    if (oil_smd && oil_smd->n_verts > 0) {
        int i;
        od_min_x = od_max_x = oil_smd->p_verts[0].vx;
        od_min_y = od_max_y = oil_smd->p_verts[0].vy;
        od_min_z = od_max_z = oil_smd->p_verts[0].vz;
        for (i = 1; i < oil_smd->n_verts; i++) {
            int32_t vx = oil_smd->p_verts[i].vx;
            int32_t vy = oil_smd->p_verts[i].vy;
            int32_t vz = oil_smd->p_verts[i].vz;
            if (vx < od_min_x) od_min_x = vx;
            if (vx > od_max_x) od_max_x = vx;
            if (vy < od_min_y) od_min_y = vy;
            if (vy > od_max_y) od_max_y = vy;
            if (vz < od_min_z) od_min_z = vz;
            if (vz > od_max_z) od_max_z = vz;
        }
    }

    oil_tex = texmgr_register("\\TEXCTCMB\\OILCNTNR.TIM;1");
}

/* Room entry: pure LoadImage out of the RAM copy area_bank_sync() has already
   read. No CD access, so it is safe inside main's STATE_LOADING. Called from
   catacombs_entry_upload_textures(). */
void oil_dispenser_upload_texture(void) {
    texmgr_upload(oil_tex);
}

void oil_dispensers_clear(void) { dispenser_count = 0; }

void oil_dispenser_place(GameState area, int32_t x, int32_t y, int32_t z,
                         int32_t rot_y) {
    if (dispenser_count >= MAX_OIL_DISPENSERS) return;
    OilDispenser *d = &dispensers[dispenser_count++];
    d->area  = area;
    d->x = x;  d->y = y;  d->z = z;
    d->rot_y = rot_y;
    d->active = 1;

    /* World AABB = the axis-aligned bound of the rotated mesh footprint, corner
       by corner, exactly as the lever and the sconce bake theirs. Computed once
       here rather than per frame, and from the MEASURED min/max above rather
       than from a constant somebody has to remember to update — which is also
       why the four corners are written out in full instead of as +-hw/+-hd:
       this footprint is not centred on the origin it rotates about. */
    int32_t c = icos(rot_y), sn = isin(rot_y);
    const int32_t lx[4] = { od_min_x, od_max_x, od_max_x, od_min_x };
    const int32_t lz[4] = { od_min_z, od_min_z, od_max_z, od_max_z };
    int k;
    for (k = 0; k < 4; k++) {
        /* Same handedness as the RotMatrix Y rotation the draw uses. */
        int32_t wx = x + ((lx[k] * c + lz[k] * sn) >> 12);
        int32_t wz = z + ((lz[k] * c - lx[k] * sn) >> 12);
        if (k == 0) {
            d->min_x = d->max_x = wx;
            d->min_z = d->max_z = wz;
        } else {
            if (wx < d->min_x) d->min_x = wx;
            if (wx > d->max_x) d->max_x = wx;
            if (wz < d->min_z) d->min_z = wz;
            if (wz > d->max_z) d->max_z = wz;
        }
    }
}

/* Player push-out against the baked box, Minkowski-expanded by the caller's
   radius and resolved along the shallowest axis — the dresser's scheme. Area-
   gated, so the shared collision routine calls it unconditionally. */
void oil_dispensers_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    int i;
    for (i = 0; i < dispenser_count; i++) {
        OilDispenser *d = &dispensers[i];
        if (!d->active || d->area != current_area) continue;

        /* Vertical gate, and it too comes out of the mesh. -Y is up, so the
           tank's UNDERSIDE is od_max_y and its top is od_min_y, both offsets
           from the floor the prop was placed on; the player's body spans feet to
           head, and only an overlap blocks. This prop hangs clear of its own
           floor (170 units of air under the tap as authored), so the test is
           doing real work rather than being a formality: it is what would let
           the player walk under a dispenser mounted higher, and what keeps this
           one from blocking the Catacombs Entry's other walkable levels. */
        int32_t floor_y   = d->y + GROUND_FLOOR_Y;
        int32_t solid_bot = floor_y + od_max_y;   /* the underside of the tank */
        int32_t solid_top = floor_y + od_min_y;   /* its top                   */
        int32_t feet = py + GROUND_FLOOR_Y, head = py - OD_PLAYER_HEAD;
        if (head >= solid_bot || feet <= solid_top) continue;

        int32_t min_x = d->min_x - radius, max_x = d->max_x + radius;
        int32_t min_z = d->min_z - radius, max_z = d->max_z + radius;
        if (*px <= min_x || *px >= max_x) continue;
        if (*pz <= min_z || *pz >= max_z) continue;

        int32_t pl = *px - min_x, pr = max_x - *px;
        int32_t pf = *pz - min_z, pb = max_z - *pz;
        int32_t m = pl, ddx = -pl, ddz = 0;
        if (pr < m) { m = pr; ddx =  pr; ddz = 0; }
        if (pf < m) { m = pf; ddx = 0; ddz = -pf; }
        if (pb < m) {         ddx = 0; ddz =  pb; }
        *px += ddx; *pz += ddz;
    }
}

/* Render every instance in the current area. Textured-prim path with per-poly
   UVs from the SMD (the model is one texture, so the tpage/clut are the same for
   every face and there is no tex map to keep in step). The untextured branches
   are kept because the loop branches on the primitive's own texture bit, which
   costs nothing and survives a re-export that leaves a flat face behind.

   THE CALLER OWNS THE 128 TEXTURE WINDOW and this prop NEEDS it, which is the
   one thing here that is not the sconce's arrangement. oil_container.tim sits at
   Voff 0 (x896 y256, tools/VRAM_MAP.txt) so there is nothing to bracket — but
   the model's own UVs run past one tile, u to 195 and v to 128, because the tank
   tiles its plating across its big faces. Those only land back on the art
   because the window wraps them mod-128. Draw this prop in a room that sets no
   window and the tank samples whatever else is in the page. */
void oil_dispensers_draw(RenderContext *ctx) {
    if (!oil_smd) return;

    MATRIX view;
    camera_build_view(&view);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint16_t tp = texmgr_tpage(oil_tex);
    uint16_t cl = texmgr_clut(oil_tex);

    int i;
    for (i = 0; i < dispenser_count; i++) {
        OilDispenser *d = &dispensers[i];
        if (!d->active || d->area != current_area) continue;

        /* Cull at the ROOM's fog-far, not at a constant of this module's own —
           the Catacombs Entry's view distance is a live number the Helluminator
           moves between 1600 and 3200, and g_fog_far is whatever the area draw
           set a few lines before calling us (render.h). Then through the room's
           lights, so a dispenser standing inside a sconce's reach appears and
           fades exactly where the wall behind it does. It registers no light of
           its own: it is a tank, not a lamp. */
        int32_t dcx = d->x - cam_x, dcz = d->z - cam_z;
        int32_t dist = (dcx < 0 ? -dcx : dcx) + (dcz < 0 ? -dcz : dcz);
        dist = render_light_dist(d->x, d->z, dist);
        if (dist > g_fog_far) continue;

        MATRIX m, combined;
        SVECTOR rr = {0, (int16_t)d->rot_y, 0, 0};
        RotMatrix(&rr, &m);
        VECTOR pos = {d->x, d->y + GROUND_FLOOR_Y, d->z};
        TransMatrix(&m, &pos);
        CompMatrixLV(&view, &m, &combined);

        gte_SetRotMatrix(&combined);
        gte_SetTransMatrix(&combined);

        int32_t fog_factor = render_fog_scale(dist);

        uint8_t *p = (uint8_t *)oil_smd->p_prims;
        int pi;
        for (pi = 0; pi < oil_smd->n_prims; pi++) {
            SMD_PRI_TYPE *pt       = (SMD_PRI_TYPE *)p;
            uint8_t       stride   = pt->len;
            int           is_quad  = (pt->type >= 2);
            int           textured = pt->texture;

            uint16_t *vi = (uint16_t *)(p + 4);
            SVECTOR *v0 = &oil_smd->p_verts[vi[0]];
            SVECTOR *v1 = &oil_smd->p_verts[vi[1]];
            SVECTOR *v2 = &oil_smd->p_verts[vi[2]];

            DVECTOR sv[4];
            int32_t sz[4], otz, nclip;

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
                SVECTOR *v3 = &oil_smd->p_verts[vi[3]];
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
            /* >>> NO +40. <<< The room mesh biases every one of its polys 40
               buckets deeper into the OT; a prop that took the same bias sorted
               in the same slot as the wall it is set against, and a tie is a
               LOSS for the prop — the room queues its mesh before it calls us
               and addPrim pushes to the head of the bucket, so within one slot
               the later-added primitive is drawn FIRST and painted over. Sorting
               at true scene depth lifts the prop 40 buckets, and a bucket is 4
               world units (the arithmetic is in src/catacomb_doors.c), so this is
               a 160-unit lift — more than this prop's own 40-unit depth, which is
               what it has to beat for no part of it to fall behind a surface it
               touches. The sconce's note spells the same rule out at length.

               It costs no correctness: a wall GENUINELY in front of the tank is
               hundreds of buckets nearer and still occludes it. The clamp is not
               decoration — dropping the bias drops the free headroom the +40 used
               to give against the menu's reserved range, and a prop seen from
               close up lands in single digits without SCENE_OT_MIN. */
            if (otz < SCENE_OT_MIN)   otz = SCENE_OT_MIN;
            /* Stay below the room's texture-window primitive at OT_LENGTH-1 so
               it is processed first, the same rule the room geometry keeps. */
            if (otz >= OT_LENGTH - 1) otz = OT_LENGTH - 2;

            /* Fog on the room's ramp, saturating to the Catacombs Entry's clear
               colour (CE_FOG_* in src/catacombs_entry.c). Hard-coded as every
               prop's is — there is no global for the colour, only for the
               distance — and this prop is a Chapter 3 fixture, so the room it
               would have to be told about is the room it is in. */
            uint8_t *col = p + 16;
            uint8_t r = (uint8_t)(((int32_t)col[0] * fog_factor + 7 * (256 - fog_factor)) >> 8);
            uint8_t g = (uint8_t)(((int32_t)col[1] * fog_factor + 6 * (256 - fog_factor)) >> 8);
            uint8_t b = (uint8_t)(((int32_t)col[2] * fog_factor + 9 * (256 - fog_factor)) >> 8);

            if (is_quad && textured) {
                if (ctx->next_packet + sizeof(POLY_FT4) > buf_end) { p += stride; continue; }
                uint8_t *uv = p + 20;
                POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
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

    /* Back to the plain view matrix — whatever the caller draws next is in world
       space and must not inherit the last instance's model transform. */
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);
}

/* ---- THE LEVEL BAR ---------------------------------------------------------
   The tank's only readout, and the shape is src/asag_fight.c's af_bar_at(): a
   dark trough with a fill in front of it, both projected from one world point
   and both sorted at SCENE_OT_MIN so they are UI rather than geometry. Read the
   note above af_bar_at before touching the projection — the four traps it lists
   (a zero z, the +-1023 screen clamp, the packet budget, and the trough having
   to sort one bucket BEHIND the fill) are all still live here, and the boil bars
   are on record as having been written as a copy that dropped one of them.

   >>> BLUE TO RED, AND IT IS A LERP ON THE FRACTION, NOT A THRESHOLD. <<< A
   monster's bar is red at every length because what it means is "this is an
   enemy and it is not dead yet"; this one means "how much is left", which is a
   quantity, so the colour carries the same information as the length and a
   player reading either one gets the answer. Blue is the oil, red is the warning
   you are about to be without it.

   IT FADES UP ON THE PROMPT'S OWN CURVE. The caller hands in its radius and its
   fade_near — the very numbers its "Press O to refill" sign uses — so the tank
   says what is in it on the same frame it says it can be used, and the two can
   never be tuned apart. That is what "comes into view the same way the text
   does" has to mean for it to stay true after someone moves one of them.

   SORTED IN FRONT OF EVERYTHING, which is the one thing here that is not
   strictly correct: at SCENE_OT_MIN the bar draws through a wall that is
   genuinely between it and the camera. It is the price af_bar_at already pays
   and it is a smaller price here — the fade radius is the prompt's, so the bar
   is only ever up when the player is close enough to be offered the prop, and
   in this room that is inside the same open corner. */
#define OD_BAR_W     48   /* narrower than Asag's 60: a small prop, seen close */
#define OD_BAR_H      6   /* the boss bars' height, so it reads as the same UI */
#define OD_BAR_RISE  40   /* world units of air above the top of the tank      */

void oil_dispensers_draw_bar(RenderContext *ctx, int32_t radius, int32_t fade_near) {
    if (radius <= 0 || fade_near >= radius) return;

    MATRIX view;
    camera_build_view(&view);
    gte_SetRotMatrix(&view);
    gte_SetTransMatrix(&view);

    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;

    int i;
    for (i = 0; i < dispenser_count; i++) {
        OilDispenser *d = &dispensers[i];
        if (!d->active || d->area != current_area) continue;

        /* RANGE AND FADE OFF THE TANK'S OWN CENTRE, the same Manhattan distance
           every prompt in this game fades on. The bar hangs above that centre
           rather than above the placement origin, which is the corner of the
           footprint and not under the tank at all (oil_dispenser.h). */
        int32_t cx = (d->min_x + d->max_x) / 2;
        int32_t cz = (d->min_z + d->max_z) / 2;
        int32_t dx = cam_x - cx, dz = cam_z - cz;
        int32_t xz = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (xz >= radius) continue;

        int fade = 256;
        if (xz > fade_near) {
            int32_t range = radius - fade_near;
            int32_t prog  = xz - fade_near;
            if (prog > range) prog = range;
            fade = (int)(256 - ((prog * 256) / range));
        }

        /* od_min_y is the TOP of the mesh (-Y is up), measured at load, so the
           bar rides a re-exported tank up or down instead of being left inside
           a taller one. */
        SVECTOR top;
        top.vx  = (int16_t)cx;
        top.vy  = (int16_t)(d->y + GROUND_FLOOR_Y + od_min_y - OD_BAR_RISE);
        top.vz  = (int16_t)cz;
        top.pad = 0;

        DVECTOR sv;
        int32_t sz;
        gte_ldv0(&top);
        gte_rtps();
        gte_stsxy(&sv);
        gte_stsz(&sz);
        if (sz == 0) continue;
        if (sv.vx <= -1023 || sv.vx >= 1023 || sv.vy <= -1023 || sv.vy >= 1023) continue;

        int16_t bar_x = (int16_t)(sv.vx - OD_BAR_W / 2);
        int16_t bar_y = sv.vy;

        if (ctx->next_packet + sizeof(TILE) <= buf_end) {
            TILE *bg = (TILE *)ctx->next_packet;
            setTile(bg);
            /* The trough fades too, or an empty tank leaves a grey rectangle
               hanging in the dark after the fill it framed has gone. */
            setRGB0(bg, (uint8_t)((40 * fade) >> 8),
                        (uint8_t)((40 * fade) >> 8),
                        (uint8_t)((40 * fade) >> 8));
            setXY0(bg, bar_x, bar_y);
            setWH(bg, OD_BAR_W, OD_BAR_H);
            /* SCENE_OT_MIN + 1: one bucket FURTHER BACK than the fill, because
               the OT is sorted back-to-front and the trough has to go behind the
               thing standing in it. */
            addPrim(&ctx->buffers[ctx->active_buffer].ot[SCENE_OT_MIN + 1], bg);
            ctx->next_packet += sizeof(TILE);
        }

        int32_t frac   = (reservoir * 256) / OD_OIL_MAX;   /* 256 = a full tank */
        int16_t fill_w = (int16_t)((reservoir * OD_BAR_W) / OD_OIL_MAX);
        if (fill_w > 0 && ctx->next_packet + sizeof(TILE) <= buf_end) {
            /* Blue (40,90,255) at full to red (220,30,30) at empty. The fade is
               folded into the same shift, so this is two multiplies per channel
               and no branch on how full the tank is. */
            int32_t r = ( 40 * frac + 220 * (256 - frac)) >> 8;
            int32_t g = ( 90 * frac +  30 * (256 - frac)) >> 8;
            int32_t b = (255 * frac +  30 * (256 - frac)) >> 8;
            TILE *fill = (TILE *)ctx->next_packet;
            setTile(fill);
            setRGB0(fill, (uint8_t)((r * fade) >> 8),
                          (uint8_t)((g * fade) >> 8),
                          (uint8_t)((b * fade) >> 8));
            setXY0(fill, bar_x, bar_y);
            setWH(fill, fill_w, OD_BAR_H);
            addPrim(&ctx->buffers[ctx->active_buffer].ot[SCENE_OT_MIN], fill);
            ctx->next_packet += sizeof(TILE);
        }
    }
}
