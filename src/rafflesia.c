#include <stdint.h>
#include <stdlib.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include "render.h"
#include "camera.h"
#include "player.h"
#include "collision.h"      /* GROUND_FLOOR_Y */
#include "crucifaxe.h"      /* SWING_RANGE */
#include "particles.h"
#include "texmgr.h"
#include "web.h"            /* WEB_DAMAGE — the mist carries the spider's poison */
#include "rafflesia.h"
#include "sound.h"

Rafflesia rafflesias[MAX_RAFFLESIAS];
int       rafflesia_count = 0;

static Rafflesia raf_defaults[MAX_RAFFLESIAS];

/* ---- Tuning -----------------------------------------------------------------
   Distances are horizontal Manhattan to the player, matching every other enemy
   in the game (the tentacle's ranges, the crucifaxe's reach). */

/* Sprite half-extents, world units. TALLER THAN WIDE, unlike every other sprite
   enemy here: 210 x 380 for a flower that stands a little over the player's eye
   line. The bottom edge rests on the floor (draw_billboard centres the quad one
   half-height above it), so HALF_H is also the drawn height above the bed. */
#define RAF_HALF_W           105
#define RAF_HALF_H           190

#define RAF_OSC_RATE          24   /* frames per sprite swap while awake (~0.4s) */

/* The MIST. 5 s between clouds, 2 s in the air, 300 units of radius. The damage
   is the web's, once per cloud — the poison status is refreshed for as long as
   the player stands in it, but a cloud that has already bitten does not bite
   again, or two seconds inside one would be lethal on its own. */
#define RAF_MIST_INTERVAL    300   /* 5 s at 60 fps                              */
#define RAF_MIST_LIFE        120   /* 2 s at 60 fps                              */
#define RAF_MIST_RADIUS      300
#define RAF_MIST_DAMAGE      WEB_DAMAGE
#define RAF_MIST_HALF_H      130   /* drawn height of the cloud above the ground  */

/* The GRIP. The player is hauled in from RAF_PULL_RADIUS and held at
   RAF_HOLD_DIST; the bite lands only inside RAF_BITE_DIST.

   >>> SIZE THESE AGAINST THE ROOM'S WALL STANDOFF, NOT AGAINST THE SPRITE. <<<
   These were first written as 150/70/110 — reading "within 150" straight off
   the design — and the enemy was inert, because a flower is PLANTED IN A BED
   AND EVERY BED IS TUCKED AGAINST A HEDGE. The Outside Catacombs holds the
   player 260 off every wall (OC_WALL_RADIUS, wider than the usual 195 because
   its hedges are drawn 1100 tall), so the closest any of the three flowers can
   actually be approached is 110, 120 and 110 Manhattan respectively. A bite
   gated on `dist < 110` can therefore never fire on any frame, and a 150 pull
   radius leaves a 30-unit sliver to stand in.

   The rule this cost: a rooted enemy's reach is bounded below by
   (room wall radius) - (its distance from the nearest wall), and its HOLD
   distance must sit ABOVE that floor or the pull just grinds the player into
   the geometry instead of settling. 400/200/260 clears it on all three beds.
   Re-derive these if the flowers are ever replanted or the standoff changes;
   the check is the reachability sweep in the commit that fixed this.

   THE PULL HAS TWO STAGES, and which one is running is the whole feel of the
   enemy:

     HAUL    From RAF_PULL_RADIUS in to RAF_BITE_DIST, at RAF_HAUL_SPEED — well
             ABOVE the 12/frame walk (camera.c), so the player cannot slow it by
             running. 700 down to 340 is ~13 frames standing still and ~22 while
             walking away: a yank, not a drift. This is the capture.
     TETHER  Once it HAS them (r->hauled), it drops to RAF_TETHER_SPEED for as
             long as the grip lasts. This one is deliberately below walk speed,
             so backing off gains ground and the bite stops; let go of the stick
             and the same tether reels them back in at that gentler pace.

   The stage is latched per flower rather than recomputed from distance, and it
   has to be: recomputing would flip back to HAUL the instant the player backed
   past RAF_BITE_DIST and yank them in again, making the grip inescapable — the
   opposite of the "you can back off" rule.

   >>> RAF_TETHER_SPEED MUST BEAT THE *POISONED* WALK SPEED, NOT THE NORMAL ONE.
   <<< The flower's own mist halves the player's walk to 6 (camera.c: `if
   (player_poisoned()) speed /= 2`), and a player being bitten has almost
   certainly just been gassed. A tether of 6 would exactly cancel a poisoned
   retreat and pin them forever. At 4 they still gain 2 a frame poisoned and 8
   clean — and since the bite's escape test only asks whether distance is
   RISING, 2 a frame is enough to stop the damage on the very next frame.

   >>> SIZE THESE AGAINST THE ROOM'S WALL STANDOFF, NOT AGAINST THE SPRITE. <<<
   These were first written as 150/70/110 — reading "within 150" straight off
   the design — and the enemy was inert, because a flower is PLANTED IN A BED
   AND EVERY BED IS TUCKED AGAINST A HEDGE. The Outside Catacombs holds the
   player 260 off every wall (OC_WALL_RADIUS, wider than the usual 195 because
   its hedges are drawn 1100 tall), so the closest any of the three flowers
   could be approached was 110, 120 and 110. A bite gated on `dist < 110` could
   never fire on any frame. The flower's own solid body (RAF_BODY_RADIUS, 240
   including the player's standoff) now dominates that floor, but the lesson
   stands: re-derive these whenever the flowers are replanted.

   DISTANCES HERE ARE TRUE RADIAL, not the Manhattan sum used elsewhere in this
   codebase. That is what lets a stated range mean the same in every direction —
   Manhattan overstates by up to 41% on the diagonal, so the old 400 "radius"
   was really anywhere from 283 to 400 depending on the approach angle. 100
   units is 1 metre (RBS_HOVER), so 700 is about 23 feet. */
#define RAF_PULL_RADIUS      700   /* ~7 m: reaches past the avenue centreline,
                                      so the flowers grab at a player walking
                                      up the middle rather than only at one who
                                      walks into them                            */
#define RAF_HOLD_DIST        260   /* tether stops here — just outside the 240
                                      solid body, so it does not grind           */
#define RAF_BITE_DIST        340   /* "in close": bites, and the HAUL->TETHER
                                      handover point                             */
#define RAF_HAUL_SPEED        28   /* units/frame reeling them in (walk is 12)   */
#define RAF_TETHER_SPEED       4   /* units/frame once held; under POISONED walk */
#define RAF_BITE_DELAY        60   /* 1 s held before the first bite lands        */
#define RAF_BITE_TICK          6   /* frames per point of bite damage...          */
#define RAF_BITE_DAMAGE        1   /* ...so 10 HP/s = 10% of MAX_HEALTH per second */

/* ---- Solid body -------------------------------------------------------------
   A rafflesia is SOLID, unlike the tentacle, which the player walks straight
   through. It has to be: the grip hauls the player in, and with nothing to stop
   them they ended up inside the billboard, where the quad's corners cross the
   GTE near plane and the sprite clips away to nothing — so the thing eating you
   was invisible, and being invisible it could not be aimed at either.

   The stop distance is RAF_BODY_RADIUS plus whatever the caller passes as the
   player's own radius; apply_collision_reception passes 75, the same value the
   dressers and dining tables use, for 240 in total. That number is picked from
   three constraints, all of which it clears:
     - it is outside the sprite's 105 half-width, so the player never stands
       inside the silhouette;
     - it is inside the crucifaxe's SWING_RANGE (350, and the axe's dy against
       this enemy is ~1), so the flower stays meleeable while it holds you;
     - it is far enough back that a 380-tall billboard still projects whole
       rather than clipping, which was the original complaint.
   RAF_HOLD_DIST sits above it so the pull eases off at the surface instead of
   grinding the player into it every frame.

   >>> IT IS A SOFT CONSTRAINT, AND THE HEDGES OUTRANK IT. <<< Two of the three
   beds are tucked against a hedge, so this circle overlaps solid geometry, and
   apply_collision_reception therefore runs this push BEFORE its wall passes
   rather than after (the only prop there that does — see the note beside the
   call). The two AVENUE flowers have room and hold the full 240. The east-lawn
   one sits in a hedge pocket that does not, so on some bearings the walls win
   and the player ends up nearer — down to ~120 in the worst case, still outside
   the sprite's 105 half-width but a good deal more in-your-face. Nothing about
   that wedges or clips; if it ever reads badly, the fix is to move that flower
   off the pocket, not to shrink this radius, which 160 would force on all three. */
#define RAF_BODY_RADIUS      165

/* Light-green blood, as the tentacle's — the garden bleeds sap. */
#define RAF_BLOOD_R          150
#define RAF_BLOOD_G          230
#define RAF_BLOOD_B          130

/* Mist colour, drawn ADDITIVELY (see draw_mist). */
#define RAF_MIST_R            30
#define RAF_MIST_G           140
#define RAF_MIST_B            50

/* >>> THE GRIP CLAIM. <<< Index into rafflesias[] of the one flower currently
   holding the player, or -1. Without it the three Outside Catacombs beds — the
   two at z=1571 are 1200 apart, but a player walking the avenue passes inside
   both pull radii of a future denser planting — would each pull on the same
   frame and the player would be batted between them. File-scope and not
   persisted: it is re-derived within a frame of entering any radius. */
static int raf_grip = -1;

/* Integer square root (Newton, seeded by halving the bit length). Every gameplay
   radius in this file is TRUE RADIAL rather than the Manhattan sum used for the
   steering elsewhere in the codebase, and that needs a real sqrt: Manhattan runs
   up to 41% long on the diagonal, so a "700 radius" built on it would reach 700
   straight on and only 495 cornerwise. Both the ranges and the solid body's
   push-out would then vary with the approach angle — the body visibly so, since
   the sprite would clip through on the diagonals only.

   Newton here converges DOWNWARD, so the seed must land at or above the root
   and the loop stops the moment it would rise again. The seed condition is
   `t > 0`, not `t > 1`: with `t > 1` the shift stops one step early and lands
   BELOW the root for every v that is not a perfect power of four, at which
   point the first iteration already fails `x < last` and the function returns
   its own seed — isqrt(120) came back 8 instead of 10. Verified exhaustively
   against floor(sqrt) over 0..200000 plus the large values.

   Range: d2 is dx*dx + dz*dz and the Outside Catacombs is the biggest room in
   the game at roughly 8800 x 6600, so the worst case is 1.21e8 — comfortably
   inside int32. */
static int32_t raf_isqrt(int32_t v) {
    if (v <= 0) return 0;
    int32_t x, last, s = 1, t = v;
    while (t > 0) { t >>= 2; s <<= 1; }
    x = s;
    do { last = x; x = (x + v / x) >> 1; } while (x < last);
    return last;
}

/* Spread one area's flowers evenly around the mist cycle.

   The timer is armed on the WAKING edge, and flowers that come into fog range on
   the same frame therefore arm on the same frame — and then stay phase-locked
   for good, because every one of them runs the same RAF_MIST_INTERVAL period
   afterwards. Maze One's two southern beds are 1210 apart and wake together, so
   they exhaled on exactly the same frames, and a cloud is the most expensive
   thing this enemy draws (see the occlusion note in draw_mist).

   The phase is (this flower's ordinal WITHIN ITS AREA) / (flowers in that area),
   spread across one interval. Per-area rather than per-array, because a global
   ordinal would waste most of the cycle on flowers in a room the player is not
   standing in: Maze One's five want the whole 300 frames between them, not the
   five eighths of it that eight global slots would leave.

   With 5 flowers, a 300-frame period and a 120-frame cloud life the spacing is
   60 frames and at most TWO clouds are ever in the air at once — 5x120 > 300, so
   zero overlap is arithmetically impossible and two is the floor.

   Always ADDED to a full interval, never subtracted from one: nothing here may
   make a flower gas the player sooner after waking than it did before (mistake 8
   again). Worst case the fifth flower's first cloud is four seconds late, and
   after that every flower is on the ordinary five-second beat. */
static int raf_mist_phase(int idx) {
    int j, ordinal = 0, in_area = 0;
    GameState a = rafflesias[idx].area;
    for (j = 0; j < rafflesia_count; j++) {
        if (!rafflesias[j].active || rafflesias[j].area != a) continue;
        if (j < idx) ordinal++;
        in_area++;
    }
    if (in_area <= 1) return 0;
    return (ordinal * RAF_MIST_INTERVAL) / in_area;
}

/* ---- The two looped ambiences ----------------------------------------------
   Driven exactly like the tentacle's writhe latch: ONE voice for every flower in
   the room, flipped by a per-frame "did anything do this" flag at the END of the
   update. Per-instance voices would be three copies of one sample fighting over
   the SPU.

     writhe  SFX_TNTCL_WRTH, while any flower is awake and NOT biting ("idle")
     chew    SFX_SPDR_WLK,   while any flower is actually landing bites

   Both samples are HARDWARE-looped in the VAG (the 0x06/0x03 block flags), so
   the repeat costs no code here and the loop period is just the clip length.

   >>> BOTH ARE BORROWED FROM HOUSE MONSTERS AND SHARE THEIR VOICES <<< (17 and
   18, see sfx_channel). That is safe only because a rafflesia can never be in a
   room with a tentacle or a spider — flowers are garden, those two are house —
   and it would break the moment one is. If a garden room ever gets a tentacle,
   these need voices of their own. */
static int writhe_on = 0;
static int chew_on   = 0;

/* Per-sprite VRAM handles. These are texmgr ids, not a private RAM copy: the
   two sprites live in the SPIDERS' VRAM slots (x320/x384, y128 — see
   tools/VRAM_MAP.txt) and are streamed in on entry to a room that has
   rafflesias, exactly as a room's mesh textures are. There is no 128-row hole
   left in VRAM for a private slot, and the two enemies never share a room:
   spiders are house-interior, rafflesias are garden. */
static int raf_tex_id[2] = { -1, -1 };
static const char *raf_tex_file[2] = {
    "\\TEX\\RAFLSIA1.TIM;1",   /* in TEX, not the root: see disc.xml */
    "\\TEX\\RAFLSIA2.TIM;1",
};

typedef struct { uint16_t tpage, clut; uint8_t u0, v0, u1, v1; } Sprite;
static Sprite spr[2];
static int    tex_loaded = 0;

/* The mist cloud's texture. Unlike the two body sprites this one owns a VRAM
   slot outright — 32x32 at 4bpp is 16 words by 32 rows, and that fits the free
   band at x[832,848) y[448,480) with nothing else wanting it. So no texmgr, no
   re-upload on a transition: a plain CD read and one LoadImage at startup, and
   it stays put for the rest of the run (the same arrangement the tentacle's
   sprites have).

   >>> ITS CLUT IS BUILT WITH png_to_tim's --stp FLAG. <<< The PS1 decides
   semi-transparency PER TEXEL, from bit 15 of the palette entry — a primitive
   with setSemiTrans() set still draws OPAQUE wherever that bit is clear, which
   is everything the converter emits by default. Without --stp this texture
   would have turned the mist from a translucent green haze into a solid slab.
   Fully transparent entries stay 0x0000 so the cloud's edges keep being
   skipped rather than becoming blended black. */
static Sprite spr_mist;
static int    mist_loaded = 0;
/* The slot the TIM was built at (png_to_tim --tx/--ty). Kept as constants
   because the draw rebuilds the tpage word itself to fold in the additive blend
   bit; keep in step with tools/VRAM_MAP.txt. */
#define RAF_MIST_VRAM_X      832
#define RAF_MIST_VRAM_Y      448

/* Texture window the area expects, restored around each sprite — the sprites
   sit at Voff >= 128, so a room's 128 window would wrap their V onto the wrong
   texels. Same bracket as the tentacle's. */
static RECT raf_tw_restore;
static int  raf_tw_active = 0;

void rafflesias_set_texwindow(const RECT *tw) {
    if (tw) { raf_tw_restore = *tw; raf_tw_active = 1; }
    else    { raf_tw_active = 0; }
}

/* Add an already-filled POLY_FT4 (packet space reserved) to ot[otz], bracketing
   it with a full/unmasked window then the area's restore window when active. */
static void add_ft4_windowed(RenderContext *ctx, int32_t otz, POLY_FT4 *poly) {
    uint8_t  *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    uint32_t *ot      = ctx->buffers[ctx->active_buffer].ot;
    if (raf_tw_active && ctx->next_packet + 2 * sizeof(DR_TWIN) <= buf_end) {
        DR_TWIN *restore = (DR_TWIN *)ctx->next_packet;
        setTexWindow(restore, &raf_tw_restore);
        addPrim(&ot[otz], restore);
        ctx->next_packet += sizeof(DR_TWIN);
        addPrim(&ot[otz], poly);
        RECT full = { 0, 0, 0, 0 };
        DR_TWIN *disable = (DR_TWIN *)ctx->next_packet;
        setTexWindow(disable, &full);
        addPrim(&ot[otz], disable);
        ctx->next_packet += sizeof(DR_TWIN);
    } else {
        addPrim(&ot[otz], poly);
    }
}

/* UV rect of one sprite. texmgr keeps a texture's tpage/clut but not its rect,
   so this repeats the arithmetic load_sprite() does in tentacle.c, specialised
   to the known geometry of both sprites: 128x128 at 8bpp, i.e. 64 VRAM words
   wide, so U runs 0..127 within the page and V starts at the VRAM y mod 256.
   vram_x/vram_y are the --tx/--ty the TIMs were built at; keep them in step with
   tools/VRAM_MAP.txt. */
#define RAF_TEX_SIZE 128            /* both sprites, pixels, square             */
#define RAF_PX_PER_WORD 2           /* 8bpp                                     */
static void sprite_from_texmgr(int id, Sprite *s, int vram_x, int vram_y) {
    s->tpage = texmgr_tpage(id);
    s->clut  = texmgr_clut(id);
    int u_off = (vram_x & 63) * RAF_PX_PER_WORD;   /* 0 for both — each sits on */
    s->u0 = (uint8_t)u_off;                        /* a 64-word page boundary   */
    s->v0 = (uint8_t)(vram_y % 256);
    s->u1 = (uint8_t)(u_off + RAF_TEX_SIZE - 1);
    s->v1 = (uint8_t)(s->v0 + RAF_TEX_SIZE - 1);
}

/* Plain startup loader for a texture that owns its VRAM slot: CD read, one
   LoadImage, keep the tpage/clut/UVs. Modelled on tentacle.c's load_sprite.
   STARTUP ONLY — a CdRead once the render loop is running is unsafe (see
   tools/TEXTURING_NOTES.txt). */
static void load_owned_sprite(const char *filename, Sprite *s) {
    CdlFILE file;
    if (!CdSearchFile(&file, (char *)filename)) return;
    int   sectors = (file.size + 2047) / 2048;
    void *buf     = malloc(sectors * 2048);
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
        s->clut = getClut(tim.crect->x, tim.crect->y);
    }
    s->tpage = getTPage(tim.mode & 0x3, 0, tim.prect->x, tim.prect->y);

    int bpp_mode = tim.mode & 3;
    int px_mult  = (bpp_mode == 0) ? 4 : (bpp_mode == 1) ? 2 : 1;
    int tex_w    = tim.prect->w * px_mult;
    int u_off    = (tim.prect->x & 63) * px_mult;
    s->u0 = (uint8_t)u_off;
    s->v0 = (uint8_t)(tim.prect->y % 256);
    s->u1 = (uint8_t)(u_off + tex_w - 1);
    s->v1 = (uint8_t)(s->v0 + tim.prect->h - 1);
    free(buf);
}

void rafflesias_load_assets(void) {
    load_owned_sprite("\\TEX\\MIST.TIM;1", &spr_mist);
    if (spr_mist.tpage) mist_loaded = 1;

    raf_tex_id[0] = texmgr_register(raf_tex_file[0]);
    raf_tex_id[1] = texmgr_register(raf_tex_file[1]);
    if (raf_tex_id[0] < 0 || raf_tex_id[1] < 0) return;   /* see TEXMGR_MAX */
    /* The VRAM coordinates the TIMs were built at (textures/png_to_tim.py
       --tx/--ty): the spiders' two slots. Keep in step with tools/VRAM_MAP.txt. */
    sprite_from_texmgr(raf_tex_id[0], &spr[0], 320, 128);
    sprite_from_texmgr(raf_tex_id[1], &spr[1], 384, 128);
    tex_loaded = 1;
}

void rafflesias_upload_textures(void) {
    texmgr_upload(raf_tex_id[0]);
    texmgr_upload(raf_tex_id[1]);
}

static void add_rafflesia_state(int32_t x, int32_t y, int32_t z, GameState area,
                                int active) {
    if (rafflesia_count >= MAX_RAFFLESIAS) return;
    Rafflesia *r = &rafflesias[rafflesia_count++];
    *r = (Rafflesia){0};
    r->x = x; r->y = y; r->z = z;
    r->health = RAFFLESIA_MAX_HEALTH;
    r->active = active;
    r->area   = area;
}

static void add_rafflesia(int32_t x, int32_t y, int32_t z, GameState area) {
    add_rafflesia_state(x, y, z, area, 1);
}

/* A flower that is PLACED but not yet in the world. Its slot is inert
   everywhere — update, draw, collide, the axe, the gun — because every one of
   those loops already tests `active`, so there is no second bit to remember.
   See the note on the Greenhouse's six in rafflesia.h. */
static void add_rafflesia_dormant(int32_t x, int32_t y, int32_t z, GameState area) {
    add_rafflesia_state(x, y, z, area, 0);
}

void rafflesias_wake_area(GameState area) {
    int i;
    for (i = 0; i < rafflesia_count; i++)
        if (rafflesias[i].area == area) rafflesias[i].active = 1;
}

void rafflesias_init(void) {
    rafflesia_count = 0;

    /* Outside Catacombs: one on each of the three poison_flower_base beds in the
       drawn mesh (assets/garden/Outside Catacombs.smx, texture index 3). These
       are the bed CENTROIDS, so each flower stands in the middle of its own
       patch:
           x[ 450, 750] z[1429,1714]  ->  ( 600, 1571)   east of the avenue
           x[2200,2504] z[   0, 286]  ->  (2352,  143)   the east lawn pocket
           x[-750,-450] z[1429,1714]  ->  (-600, 1571)   west of the avenue

       y is the STANDING ANCHOR, not the floor surface: draw_billboard rests the
       sprite's bottom on y + GROUND_FLOOR_Y, and this room's ground is one flat
       plane at world y=0 (outside_catacombs_floor_zones_init), so the anchor is
       -149 — the same value the tentacles use over their own y=0 floors.

       The two avenue beds are 1200 apart, comfortably outside each other's
       300-unit mist and 150-unit pull, so today no player is ever in two
       radii at once. The grip claim (raf_grip) is what makes a denser planting
       safe, and is worth keeping whatever this list grows into. */
    add_rafflesia(  600, -149, 1571, STATE_OUTSIDE_CATACOMBS);
    add_rafflesia( 2352, -149,  143, STATE_OUTSIDE_CATACOMBS);
    add_rafflesia( -600, -149, 1571, STATE_OUTSIDE_CATACOMBS);

    /* Maze One: one on each of the FIVE poison_flower_base beds in its drawn
       mesh (assets/garden/Maze One.smx, texture index 3 — the same texture the
       Catacombs' beds use). Bed centroids, as above:
           x[ 498, 699] z[3098,3298]  ->  ( 599, 3198)   north-west corridor
           x[2898,3097] z[2701,2901]  ->  (2998, 2801)   the middle of the maze
           x[4500,4802] z[3300,3500]  ->  (4651, 3400)   north-east pocket
           x[4299,4500] z[-1893,-1696] -> (4400,-1794)   } the pair on the
           x[5508,5711] z[-1893,-1696] -> (5610,-1794)   } southern dead end

       Same -149 standing anchor: this room's ground is one flat plane at y=0
       too (maze_one_floor_zones_init), so the sprite's feet land on it.

       >>> REACH CHECKED AGAINST THE MAZE, per mistake 9 in the enemy runbook.
       <<< The Catacombs hold the player 260 off every wall; the maze uses the
       default 195 (maze_one.c says so explicitly), and none of these five beds
       is tucked into a corner the way the Catacombs' are. Sweeping every
       standable cell of the room, the closest a player can physically get to
       each centroid is 7, 4, 51, 10 and 1 units — so all five are inside
       RAF_BITE_DIST and RAF_PULL_RADIUS by a mile, and RAF_HOLD_DIST (260) is
       comfortably above the floor rather than under it. The same sweep says the
       five 240-unit push circles sever no corridor: the reachable area from the
       gate is identical with and without them.

       Spacing: the two southern beds are 1210 apart and the rest are thousands
       apart, so no two mist clouds (300) or pull radii (700) overlap. */
    add_rafflesia(  599, -149, 3198, STATE_MAZE_ONE);
    add_rafflesia( 2998, -149, 2801, STATE_MAZE_ONE);
    add_rafflesia( 4651, -149, 3400, STATE_MAZE_ONE);
    add_rafflesia( 4400, -149, -1794, STATE_MAZE_ONE);
    add_rafflesia( 5610, -149, -1794, STATE_MAZE_ONE);

    /* Maze Two: one on each of the THREE poison_flower_base beds in its drawn
       mesh (assets/garden/Maze Two.smx, texture index 4 there — the index moves
       between rooms, the texture does not). Bed centroids, as above:
           x[-3800,-3600] z[4200,4400]  ->  (-3700, 4300)   north-west corner
           x[ -800, -600] z[ 803,1002]  ->  ( -700,  902)   the west lane pocket
           x[ 3600, 3800] z[ 803,1000]  ->  ( 3700,  901)   the central doorway

       Same -149 standing anchor: this room's ground is one flat plane at y=0
       (maze_two_floor_zones_init), so the sprite's feet land on it.

       >>> REACH CHECKED AGAINST THE MAZE, per mistake 9 in the enemy runbook.
       <<< Maze Two uses the default 195 wall radius (maze_two.c says so). A grid
       sweep of every standable cell reachable from the south gate says the
       closest a player can get to these three centroids is 0, 2 and 1 units, so
       all three are inside RAF_BITE_DIST (340) and RAF_PULL_RADIUS (700) by a
       mile, and RAF_HOLD_DIST (260) sits well above the floor rather than under
       it. Each bed stands 299-300 off the nearest hedge, so none of them is in
       the Catacombs' tucked-into-a-corner situation.

       Spacing: the three are thousands apart, so no two mist clouds (300) or
       pull radii (700) overlap, and none is within MSH_SEP_RADIUS of the
       mushroom's patrol line (the nearest approach is 378, to the doorway
       flower — see world_seed_room).

       >>> THE DOORWAY FLOWER AT (3700, 901) IS A GATE, NOT A DECORATION. <<<
       Its bed sits dead centre in the 600-wide gap (collision FLOOR 0,
       x[3400,4000] z[802,1000]) that is the ONLY way into the central chamber,
       FLOOR 10. The 240 push circle (RAF_BODY_RADIUS 165 + the player's 75 in
       apply_collision_reception) spans that gap end to end: the same sweep says
       7433 standable cells — the whole chamber — go unreachable while the
       flower is alive. The x walls cannot squeeze the player past it either,
       because the push that stops them points along z, which no wall opposes.
       So the chamber is behind FOUR crucifaxe swings (RAF_BODY_RADIUS is inside
       SWING_RANGE by design) or four rounds, and rafflesias_rest() regrows it on
       every transition, so the toll is paid again on every visit. That is a real
       design decision and is deliberate here; move the flower off the bed
       centroid if it should ever become scenery instead. */
    add_rafflesia(-3700, -149, 4300, STATE_MAZE_TWO);
    add_rafflesia( -700, -149,  902, STATE_MAZE_TWO);
    add_rafflesia( 3700, -149,  901, STATE_MAZE_TWO);

    /* Greenhouse: one on each of the FOUR poison_flower_base polys in its drawn
       mesh (assets/garden/Greenhouse.smx, texture index 6 in the Aug 2026
       decimated export — the index moves between exports, the NAME does not,
       which is why gen_greenhouse_tex_map.py resolves by name). Poly centroids:
           x[-4400,-4200] z[-1000, -800] -> (-4300, -900)   west annexe, north
           x[-4400,-4200] z[-2600,-2400] -> (-4300,-2500)   west annexe, south
           x[-1322,-1099] z[ -300, -100] -> (-1211, -200)   east of bed B
           x[-2211,-1988] z[  850, 1100] -> (-2100,  975)   south of bed C

       >>> THERE WERE SIX, AND THE FIFTH BED LOST ITS TEXTURE IN THE DECIMATED
       EXPORT. <<< The old mesh carried a bed at x[-2878,-2656] z[-1200,-1000],
       cut into two coplanar halves at z = -1068, and it held two of these
       flowers. In the new export that footprint is a SINGLE quad — which is the
       merge working as intended — but it now carries `grss` instead of
       poison_flower_base, so there is no bed there any more, only lawn. The two
       flowers went with it rather than being left standing on grass.

       IT LOOKS LIKE AN ACCIDENT OF THE MERGE RATHER THAN A DESIGN CHANGE: the
       quad is still exactly the bed's old footprint and only its material
       differs. Re-assign poison_flower_base to it in Blender, re-export, and add
       a fifth line below at (-2767, -1100) — ONE flower, not two, because the
       two halves are now a single poly.

       y is the STANDING ANCHOR: this room's floor is one flat plane at world
       y = 0 (greenhouse_floor_zones_init says all five collision planes are), so
       the anchor is -149, the same as every other garden room's.

       REACH, per mistake 9 in the enemy runbook. This room uses the default 195
       wall radius, so the two ANNEXE flowers — the only two tucked into a corner
       — are held 95 off in each axis, i.e. 134 away, well inside RAF_BITE_DIST
       (340). The other four stand in open nave floor with the player able to
       walk right onto them. No push circle severs a route: the annexe is
       1200x1800 with its only doorway at the far side, and the four in the nave
       stand in aisles 600 or wider.

       ALL SIX ARE DORMANT until the room floods — see rafflesias_wake_area and
       src/greenhouse_flood.c. Sound-legal either way: the Greenhouse is on
       SND_BANK_GARDEN, which is where both of the flowers' looped ambiences
       live. */
    add_rafflesia_dormant(-4300, -149,  -900, STATE_GREENHOUSE);
    add_rafflesia_dormant(-4300, -149, -2500, STATE_GREENHOUSE);
    add_rafflesia_dormant(-1211, -149,  -200, STATE_GREENHOUSE);
    add_rafflesia_dormant(-2100, -149,   975, STATE_GREENHOUSE);

    int i;
    for (i = 0; i < rafflesia_count; i++)
        raf_defaults[i] = rafflesias[i];
}

void rafflesias_reset(void) {
    int i;
    for (i = 0; i < rafflesia_count; i++)
        rafflesias[i] = raf_defaults[i];
    raf_grip = -1;
    rafflesias_silence();   /* no loop may survive a new game / load */
}

/* Room change (and save — world_leave does both): every flower comes back at
   full health, asleep, with no cloud in the air and no grip on the player.
   Unlike zombies_rest/spiders_rest this does NOT preserve deaths: a rafflesia
   cannot be killed for good, by design, so a dead one regrows with the rest. */
void rafflesias_rest(void) {
    int i;
    for (i = 0; i < rafflesia_count; i++) {
        Rafflesia *r = &rafflesias[i];
        if (!r->active) continue;
        int32_t   x = r->x, y = r->y, z = r->z;
        GameState a = r->area;
        *r = (Rafflesia){0};
        r->x = x; r->y = y; r->z = z;
        r->health = RAFFLESIA_MAX_HEALTH;
        r->active = 1;
        r->area   = a;
    }
    raf_grip = -1;
}

/* Cut both loops dead and clear their latches. Clearing the latches matters as
   much as the sound_stops: the update only issues sound_play on the 0->1 edge,
   so a stopped voice with its latch still set would stay silent forever once the
   player came back. Called on every room transition (world.h). */
void rafflesias_silence(void) {
    sound_stop(SFX_TNTCL_WRTH);
    sound_stop(SFX_SPDR_WLK);
    writhe_on = 0;
    chew_on   = 0;
}

void update_rafflesias(void) {
    int i, any_idle = 0, any_chewing = 0;

    /* Drop a stale claim BEFORE anything else can take it: the holder may have
       been shot, or the player may have walked out of the room, since last
       frame. */
    if (raf_grip >= 0) {
        Rafflesia *g = &rafflesias[raf_grip];
        if (!g->active || g->health <= 0 || g->area != current_area) raf_grip = -1;
    }

    for (i = 0; i < rafflesia_count; i++) {
        Rafflesia *r = &rafflesias[i];
        if (!r->active || r->health <= 0) continue;
        /* current_area, never game_state — the flowers keep acting while the
           inventory menu is up (see tools/ADDING_AN_ENEMY.txt STEP 6). */
        if (r->area != current_area) { if (raf_grip == i) raf_grip = -1; continue; }

        if (r->hit_timer > 0) r->hit_timer--;

        int32_t dx = cam_x - r->x;
        int32_t dz = cam_z - r->z;

        /* TWO metrics, each matched to what it is compared against.

           mdist is the Manhattan sum, and it exists only for the wake test
           below, because draw_billboard culls on Manhattan against the same
           g_fog_far. Waking on radial while drawing on Manhattan would let a
           flower be awake but culled — an invisible thing gassing the player —
           since radial is always the smaller of the two.

           dist is TRUE RADIAL and everything else uses it: the mist circle, the
           pull, the bite, the receding test. Those are all stated as radii in
           the design, and only radial makes a stated radius mean the same in
           every direction. */
        int32_t mdist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t dist  = raf_isqrt(dx * dx + dz * dz);

        /* WAKE at the room's own draw distance, so the range is whatever this
           room can show. g_fog_far is set by the room's draw each frame; on the
           very first frame in a room it still holds the previous room's value,
           which at worst costs one frame of a wrong wake radius. */
        int was_awake = r->awake;
        r->awake = (mdist < g_fog_far);
        if (r->awake) {
            r->anim++;
            /* Arm the cooldown on the WAKING edge, not at zero: a fresh timer
               would exhale a cloud on the first frame the player came into view
               (tools/ADDING_AN_ENEMY.txt, mistake 8).
               PLUS A PER-FLOWER PHASE, which is what keeps a bed of them from
               breathing in unison — see raf_mist_phase. */
            if (!was_awake) r->mist_timer = RAF_MIST_INTERVAL + raf_mist_phase(i);
        } else {
            /* Asleep: no cloud, no grip, and the distance history is stale. */
            r->mist_life  = 0;
            r->grip_timer = 0;
            r->hauled     = 0;
            if (raf_grip == i) raf_grip = -1;
            r->last_dist = dist;
            continue;
        }

        /* ---- Mist -----------------------------------------------------------
           Exhale on the interval; the cloud then hangs for RAF_MIST_LIFE and
           damages once, but refreshes the poison for every frame the player
           spends inside it. */
        if (r->mist_life > 0) {
            r->mist_life--;
            if (!game_over && !r->mist_hit && dist < RAF_MIST_RADIUS) {
                r->mist_hit = 1;
                player_hurt(RAF_MIST_DAMAGE);
                sound_play(SFX_HURT);
                if (player_health <= 0) {
                    player_health = 0;
                    game_over     = 1;
                    flash_timer   = 90;
                }
            }
            if (!game_over && dist < RAF_MIST_RADIUS) player_poison();
        }
        /* The interval is a PERIOD, not a gap: it ticks through the cloud's own
           two seconds as well, so a flower exhales on a steady five-second beat
           rather than every seven. */
        if (--r->mist_timer <= 0) {
            r->mist_timer = RAF_MIST_INTERVAL;
            r->mist_life  = RAF_MIST_LIFE;
            r->mist_hit   = 0;
            sound_play(SFX_GAS);   /* the exhale itself, one-shot on its own voice */
        }

        /* ---- Grip and bite --------------------------------------------------
           One claim at a time (see raf_grip). A flower only takes the claim when
           it is free — if another already holds the player, this one does
           nothing at all, however close the player is. */
        int chewing_now = 0;
        if (dist < RAF_PULL_RADIUS) {
            /* The grab cue fires on the CLAIM edge only — one bark per capture,
               not one per frame of being held, and not a second one from the
               flower next to it (the claim is exclusive, which is what makes
               this safe to put here). */
            if (raf_grip < 0) { raf_grip = i; sound_play(SFX_PULL); }
        } else if (raf_grip == i) {
            raf_grip  = -1;      /* player broke away */
            r->hauled = 0;       /* next capture starts with a fresh fast haul  */
        }

        if (raf_grip == i && !game_over) {
            r->grip_timer++;

            /* Once it has reeled them into bite range the capture is over and
               the gentle tether takes it from here — LATCHED, so backing out
               past RAF_BITE_DIST does not re-arm the yank (see the note on the
               two stages above). It only clears when the grip itself is lost. */
            if (dist <= RAF_BITE_DIST) r->hauled = 1;

            /* Written into the knockback velocity the camera already integrates
               and resolves against walls (camera.h): pointing it TOWARD the
               flower is the whole of the pull. Re-set every frame, so the 0.75
               decay update_camera applies never gets a chance to bite.

               The stop at RAF_HOLD_DIST only matters for the tether; the haul
               hands over at RAF_BITE_DIST, which is further out. The solid body
               (rafflesias_collide) is what actually stops the player, one push
               later in the same frame. */
            int32_t speed = r->hauled ? RAF_TETHER_SPEED : RAF_HAUL_SPEED;
            if (dist > RAF_HOLD_DIST && dist > 0) {
                cam_kb_vx = (-dx * speed) / dist;
                cam_kb_vz = (-dz * speed) / dist;
            }

            /* The bite. Three separate gates, and all three are the brief:
                 - in close                      (dist < RAF_BITE_DIST)
                 - held for a second first       (grip_timer >= RAF_BITE_DELAY)
                 - NOT backing away              (dist <= last_dist)
               The last is why RAF_TETHER_SPEED is under the walk speed — and
               under the POISONED walk speed, which is the one that matters
               here. A player walking backwards gains ground every frame, so
               dist rises and the bite stops on the next frame; stop walking and
               the tether closes it again and the biting resumes. Standing still
               holds dist level against the solid body and is bitten. */
            int receding = (dist > r->last_dist);
            if (dist < RAF_BITE_DIST && r->grip_timer >= RAF_BITE_DELAY &&
                !receding) {
                chewing_now = 1;   /* drives the chew loop below */
                if (--r->bite_timer <= 0) {
                    r->bite_timer = RAF_BITE_TICK;
                    player_hurt(RAF_BITE_DAMAGE);
                    if (player_health <= 0) {
                        player_health = 0;
                        game_over     = 1;
                        flash_timer   = 90;
                        sound_play(SFX_DIE);
                    }
                }
            } else {
                r->bite_timer = RAF_BITE_TICK;
            }
        } else {
            r->grip_timer = 0;
            r->bite_timer = RAF_BITE_TICK;
            r->hauled     = 0;
        }

        /* Every awake flower is doing exactly one of the two: chewing, or idle.
           The two ambiences are therefore mutually exclusive PER FLOWER but not
           across them — with three in a bed, one can be chewing while the others
           writhe, and both loops run. */
        if (chewing_now) any_chewing = 1;
        else             any_idle    = 1;

        r->last_dist = dist;
    }

    /* Start/stop the two shared loops. Forced off on game-over: the area update
       stops running from that frame, so a keyed-on voice would otherwise sound
       right through the death screen (the same reason update_tentacles does it). */
    if (game_over) { any_idle = 0; any_chewing = 0; }
    if (any_idle && !writhe_on)      { sound_play(SFX_TNTCL_WRTH); writhe_on = 1; }
    else if (!any_idle && writhe_on) { sound_stop(SFX_TNTCL_WRTH); writhe_on = 0; }
    if (any_chewing && !chew_on)      { sound_play(SFX_SPDR_WLK); chew_on = 1; }
    else if (!any_chewing && chew_on) { sound_stop(SFX_SPDR_WLK); chew_on = 0; }
}

/* Push the player (or anything else with a radius) out of every live flower in
   the current area. A CYLINDER, not the crates' AABB: a billboard has no facing,
   so the stop distance must be the same whichever way it is approached.

   No vertical test. The flower stands on the floor and is 380 tall against a
   player body of about 190, so it spans the mover's whole height everywhere it
   exists; a y check here would only ever be true. */
void rafflesias_collide(int32_t *px, int32_t py, int32_t *pz, int32_t radius) {
    (void)py;
    int i;
    for (i = 0; i < rafflesia_count; i++) {
        Rafflesia *r = &rafflesias[i];
        if (!r->active || r->health <= 0) continue;
        if (r->area != current_area) continue;

        int32_t want = RAF_BODY_RADIUS + radius;
        int32_t dx   = *px - r->x;
        int32_t dz   = *pz - r->z;
        int32_t d2   = dx * dx + dz * dz;
        if (d2 >= want * want) continue;

        int32_t d = raf_isqrt(d2);
        if (d <= 0) {
            /* Dead centre: no direction to push along, so pick one rather than
               divide by zero. Only reachable by a spawn placed on the player. */
            *px = r->x + want;
            continue;
        }
        *px = r->x + (dx * want) / d;
        *pz = r->z + (dz * want) / d;
    }
}

/* Apply `amount` damage. Never knocked back — a rafflesia is rooted. */
static void rafflesia_take_damage(Rafflesia *r, int amount) {
    r->health   -= amount;
    r->hit_timer = 30;
    if (r->health <= 0) {
        spawn_burst(r->x, r->y + GROUND_FLOOR_Y - RAF_HALF_H, r->z,
                    RAF_BLOOD_R, RAF_BLOOD_G, RAF_BLOOD_B);
        sound_play(SFX_TNTCL_DIE);
        r->mist_life = 0;
        /* Its cloud goes with it, and so may the loops — if this was the last
           awake flower they stop on the next update's latch test. */
    } else {
        sound_play(SFX_AXEHIT);
    }
}

/* Aim target for the gun's hitscan: sprite centre height, half-height and
   half-width. Both sprites are the same size, so unlike the tentacle there is
   no pulsing hitbox to worry about. */
void rafflesia_body(const Rafflesia *r, int32_t *cyc, int32_t *hh, int32_t *hw) {
    *cyc = (r->y + GROUND_FLOOR_Y) - RAF_HALF_H;
    *hh  = RAF_HALF_H;
    *hw  = RAF_HALF_W;
}

/* Green and full of sap: Flame Rounds do double damage, so two flame shots kill
   where four standard ones are needed. */
static const Weakness rafflesia_weakness[] = {
    { DMG_FLAME, 200 },
};

int32_t rafflesia_scale_damage(int32_t base, DamageType type) {
    return damage_scale(base, type, rafflesia_weakness,
                        WEAKNESS_COUNT(rafflesia_weakness));
}

void rafflesia_shoot(Rafflesia *r, int amount) {
    rafflesia_take_damage(r, amount);
}

int rafflesias_try_hit(void) {
    int i;
    for (i = 0; i < rafflesia_count; i++) {
        Rafflesia *r = &rafflesias[i];
        if (!r->active || r->health <= 0) continue;
        if (r->area != current_area) continue;

        int32_t hit_y  = (r->y + GROUND_FLOOR_Y) - RAF_HALF_H;
        int32_t dx     = r->x - cam_x;
        int32_t dy     = hit_y - cam_y;
        int32_t dz     = r->z - cam_z;
        int32_t dist2d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        int32_t dist3d = dist2d + (dy < 0 ? -dy : dy);
        if (dist3d >= SWING_RANGE) continue;

        int32_t dot = ((int32_t)dx * isin(cam_rot) +
                       (int32_t)dz * icos(cam_rot)) >> 12;
        if (dot <= 0) continue;

        rafflesia_take_damage(r, 1);
        return 1;
    }
    return 0;
}

/* Emit one camera-facing billboard, bottom edge resting on the floor. Copied
   from the tentacle's, which is the reference for a rooted sprite: world-space
   quad along the camera's right axis so the GTE gives true perspective scaling,
   fog to match the room mesh, red flash on a fresh hit. */
static void draw_billboard(RenderContext *ctx, Rafflesia *r, const Sprite *s) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (ctx->next_packet + sizeof(POLY_FT4) + 2 * sizeof(DR_TWIN) > buf_end) return;

    int32_t dx = r->x - cam_x;
    int32_t dz = r->z - cam_z;
    int32_t wdist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (wdist >= g_fog_far) return;
    if (((dx * isin(cam_rot) + dz * icos(cam_rot)) >> 12) <= 0) return;  /* behind camera */

    /* Behind a hedge: skip the fill (collision.h explains why the OT sort does
       not already save it). 210 x 380 world units is a big billboard, and Maze
       One plants five of them around a maze where most are hidden from any given
       spot.

       EXEMPT RAF_PULL_RADIUS. Inside its own reach a flower must be drawn
       whatever stands in the way: that is the range at which it grabs the
       player, and an invisible thing hauling you in is unaimable and unreadable
       — mistake 10 in the runbook, and the reason this cull went on the mist
       first and the body only now. Nothing outside 700 can touch the player, so
       nothing outside 700 needs to be on screen through a wall.

       The 3/2 is the MANHATTAN-to-radial conversion, and it is not decoration:
       wdist is the Manhattan sum while RAF_PULL_RADIUS is a true radius, and
       Manhattan overstates by up to sqrt(2) on the diagonals. Compared raw, a
       flower gripping the player from 700 away on a diagonal reads as 990 and
       would be culled mid-grab — exactly the case the exemption exists for.
       3/2 > sqrt(2) covers every bearing. */
    if (wdist > (RAF_PULL_RADIUS * 3) / 2 &&
        collision_hidden_from_camera(r->x, r->y, r->z))
        return;

    int32_t floor_y = r->y + GROUND_FLOOR_Y;
    int32_t cy      = floor_y - RAF_HALF_H;

    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);
    int16_t dwx = (int16_t)((RAF_HALF_W * rx) >> 12);
    int16_t dwz = (int16_t)((RAF_HALF_W * rz) >> 12);
    int16_t vy_top = (int16_t)(cy - RAF_HALF_H);
    int16_t vy_bot = (int16_t)(cy + RAF_HALF_H);

    SVECTOR v[4];
    v[0].vx = (int16_t)(r->x - dwx); v[0].vy = vy_top; v[0].vz = (int16_t)(r->z - dwz); v[0].pad = 0;
    v[1].vx = (int16_t)(r->x + dwx); v[1].vy = vy_top; v[1].vz = (int16_t)(r->z + dwz); v[1].pad = 0;
    v[2].vx = (int16_t)(r->x + dwx); v[2].vy = vy_bot; v[2].vz = (int16_t)(r->z + dwz); v[2].pad = 0;
    v[3].vx = (int16_t)(r->x - dwx); v[3].vy = vy_bot; v[3].vz = (int16_t)(r->z - dwz); v[3].pad = 0;

    DVECTOR sv[4];
    int32_t sz[4], otz;
    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);
    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);
    gte_stsz4c(sz);
    if (!sz[1] || !sz[2] || !sz[3]) return;
    gte_avsz4();
    gte_stotz(&otz);
    if (otz <= 0) return;
    if (otz < SCENE_OT_MIN)  otz = SCENE_OT_MIN;
    if (otz > OT_LENGTH - 2) otz = OT_LENGTH - 2;

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    {
        uint8_t fc = (uint8_t)((128 * render_fog_scale(wdist)) >> 8);
        if (r->hit_timer > 0) setRGB0(poly, fc, fc >> 2, fc >> 2);
        else                  setRGB0(poly, fc, fc, fc);
    }
    poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
    poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
    poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
    poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;
    poly->u0 = s->u0; poly->v0 = s->v0;
    poly->u1 = s->u1; poly->v1 = s->v0;
    poly->u2 = s->u0; poly->v2 = s->v1;
    poly->u3 = s->u1; poly->v3 = s->v1;
    poly->clut  = s->clut;
    poly->tpage = s->tpage;
    ctx->next_packet += sizeof(POLY_FT4);
    add_ft4_windowed(ctx, otz, poly);
}

/* The spore cloud: ONE camera-facing untextured quad, RAF_MIST_RADIUS wide,
   drawn with ADDITIVE semi-transparency (ABR=1, exactly as the stove flame in
   particles.c does it).
 *
 * Additive rather than the 50/50 blend for the same reason the poison wash is:
 * over two seconds a half-strength blend reads as the lights going out, while
 * adding green tints without darkening what is behind it. It is also
 * order-independent, so one quad needs no sorting against the flower's own
 * sprite — which is why this is a quad and not a puff of particles.
 *
 * The brightness follows a triangle over the cloud's life so it blooms and
 * disperses instead of popping on and off. */
static void draw_mist(RenderContext *ctx, Rafflesia *r) {
    uint8_t *buf_end = ctx->buffers[ctx->active_buffer].buffer + BUFFER_LENGTH;
    if (!mist_loaded) return;
    if (ctx->next_packet + sizeof(POLY_FT4) + 2 * sizeof(DR_TWIN) > buf_end) return;

    int32_t dx = r->x - cam_x;
    int32_t dz = r->z - cam_z;
    int32_t wdist = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
    if (wdist >= g_fog_far) return;
    if (((dx * isin(cam_rot) + dz * icos(cam_rot)) >> 12) <= 0) return;

    /* 0..256..0 triangle across the cloud's life. */
    int32_t half = RAF_MIST_LIFE / 2;
    int32_t age  = RAF_MIST_LIFE - r->mist_life;          /* 0 at release */
    int32_t k    = (age < half) ? (age * 256) / half
                                : ((RAF_MIST_LIFE - age) * 256) / half;
    /* Fade with distance too, so a cloud does not glow out of the fog. */
    k = (k * render_fog_scale(wdist)) >> 8;
    if (k <= 0) return;

    /* >>> OCCLUSION. THIS IS A FILL-RATE CULL, NOT A CORRECTNESS ONE. <<<
       The cloud is 600 x 260 world units, SEMI-TRANSPARENT and ADDITIVE, which
       is several times the per-pixel cost of the opaque hedges around it. At
       gte_SetGeomScreen(256) a 300-unit half-width covers the full 320-pixel
       width from 480 units away, so a near cloud is a full-screen blended fill —
       and there are five flowers in Maze One, most of them behind a hedge from
       any given spot. Without this test the GPU fills every one of those and the
       maze then paints over them: the whole cost, none of the picture.

       Tested to the flower's own centre, which is where the cloud is anchored.
       The failure mode is a small pop as a bed comes round a corner, and that is
       the right trade against a full-screen additive quad per hidden flower.
       Cheap now that a prop-less room skips the sampler (see props_any_solid in
       collision.c): this is one pass over the room's walls, and only for a
       flower that actually has a cloud in the air.

       The MIST ONLY. The body sprite deliberately keeps drawing through a hedge:
       a flower can grip the player from 700 away and an ungrabbable, unaimable
       invisible one is exactly the bug RAF_BODY_RADIUS exists to prevent (see
       mistake 10 in tools/ADDING_AN_ENEMY.txt). Note the cloud still DAMAGES
       through geometry, as it always has — that is an update-side question and
       this changes nothing about it.

       No close-radius exemption here, unlike the body below: a cloud has nothing
       to aim at and nothing to read off, so there is no distance at which one
       hidden behind a hedge is worth its fill. */
    if (collision_hidden_from_camera(r->x, r->y, r->z)) return;

    int32_t floor_y = r->y + GROUND_FLOOR_Y;
    int32_t cy      = floor_y - RAF_MIST_HALF_H;

    int32_t rx = icos(cam_rot);
    int32_t rz = -isin(cam_rot);
    int16_t dwx = (int16_t)((RAF_MIST_RADIUS * rx) >> 12);
    int16_t dwz = (int16_t)((RAF_MIST_RADIUS * rz) >> 12);
    int16_t vy_top = (int16_t)(cy - RAF_MIST_HALF_H);
    int16_t vy_bot = (int16_t)(cy + RAF_MIST_HALF_H);

    SVECTOR v[4];
    v[0].vx = (int16_t)(r->x - dwx); v[0].vy = vy_top; v[0].vz = (int16_t)(r->z - dwz); v[0].pad = 0;
    v[1].vx = (int16_t)(r->x + dwx); v[1].vy = vy_top; v[1].vz = (int16_t)(r->z + dwz); v[1].pad = 0;
    v[2].vx = (int16_t)(r->x + dwx); v[2].vy = vy_bot; v[2].vz = (int16_t)(r->z + dwz); v[2].pad = 0;
    v[3].vx = (int16_t)(r->x - dwx); v[3].vy = vy_bot; v[3].vz = (int16_t)(r->z - dwz); v[3].pad = 0;

    DVECTOR sv[4];
    int32_t sz[4], otz;
    gte_ldv3(&v[0], &v[1], &v[2]);
    gte_rtpt();
    gte_stsxy3c(sv);
    gte_ldv0(&v[3]);
    gte_rtps();
    gte_stsxy(&sv[3]);
    gte_stsz4c(sz);
    if (!sz[1] || !sz[2] || !sz[3]) return;
    gte_avsz4();
    gte_stotz(&otz);
    if (otz <= 0) return;
    if (otz < SCENE_OT_MIN)  otz = SCENE_OT_MIN;
    if (otz > OT_LENGTH - 2) otz = OT_LENGTH - 2;

    POLY_FT4 *poly = (POLY_FT4 *)ctx->next_packet;
    setPolyFT4(poly);
    setSemiTrans(poly, 1);
    /* The tint MODULATES the texture now rather than being the colour outright:
       128 is neutral on a textured poly, so these values darken red and blue and
       leave green a shade over neutral, turning a grey puff green. `k` then
       fades the whole thing in and out over the cloud's life. */
    setRGB0(poly, (uint8_t)((RAF_MIST_R * k) >> 8),
                  (uint8_t)((RAF_MIST_G * k) >> 8),
                  (uint8_t)((RAF_MIST_B * k) >> 8));
    poly->x0 = sv[0].vx; poly->y0 = sv[0].vy;
    poly->x1 = sv[1].vx; poly->y1 = sv[1].vy;
    poly->x2 = sv[3].vx; poly->y2 = sv[3].vy;
    poly->x3 = sv[2].vx; poly->y3 = sv[2].vy;
    poly->u0 = spr_mist.u0; poly->v0 = spr_mist.v0;
    poly->u1 = spr_mist.u1; poly->v1 = spr_mist.v0;
    poly->u2 = spr_mist.u0; poly->v2 = spr_mist.v1;
    poly->u3 = spr_mist.u1; poly->v3 = spr_mist.v1;
    poly->clut = spr_mist.clut;
    /* ABR=1 (additive) is carried in the primitive's OWN tpage word, so this no
       longer needs the trailing DR_TPAGE the untextured version used — and it is
       tidier, because that one set the blend mode for the whole OT band rather
       than for this quad alone. The page origin is the mist texture's own. */
    poly->tpage = getTPage(0 /* 4bpp */, 1 /* ABR=1: additive */,
                           RAF_MIST_VRAM_X, RAF_MIST_VRAM_Y);
    ctx->next_packet += sizeof(POLY_FT4);
    /* Voff 192, so it needs the same window bracket the body sprites do. */
    add_ft4_windowed(ctx, otz, poly);
}

void draw_rafflesias(RenderContext *ctx) {
    if (!tex_loaded) return;
    int i;
    for (i = 0; i < rafflesia_count; i++) {
        Rafflesia *r = &rafflesias[i];
        if (!r->active || r->health <= 0) continue;
        if (r->area != current_area) continue;

        /* Alternate the two sprites while awake; sprite 0 alone at rest. */
        int frame = (r->awake && ((r->anim / RAF_OSC_RATE) & 1)) ? 1 : 0;
        draw_billboard(ctx, r, &spr[frame]);
        if (r->mist_life > 0) draw_mist(ctx, r);
    }
}
