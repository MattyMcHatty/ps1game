#include <string.h>
#include "world.h"
#include "demondog.h"
#include "zombie.h"
#include "spider.h"
#include "crate.h"
#include "key.h"
#include "sml_med.h"
#include "item_pickup.h"
#include "player.h"      /* GRAVEOLVER_CAPACITY */
#include "door.h"
#include "fatdoor.h"
#include "tentacle.h"
#include "rafflesia.h"
#include "mushroom.h"
#include "rabisu.h"
#include "savegame.h"
#include "sound.h"

/* A saved snapshot of one room's entities. Mirrors the live arrays below.
   This lives in RAM only — the memory card gets the far smaller WorldDelta
   (see world.h), which is rebuilt into these by world_load_delta(). */
typedef struct {
    int       visited;

    DemonDog  dogs[MAX_DEMON_DOGS];   int dog_count;
    Zombie    zombs[MAX_ZOMBIES];     int zomb_count;
    Crate      crates[MAX_CRATES];         int crate_count;
    KeyPickup  keys[MAX_KEYS];             int key_count;
    SmlMed     meds[MAX_SML_MEDS];         int med_count;
    ItemPickup items[MAX_ITEM_PICKUPS];    int item_count;
    DoorState  door_state;
} RoomState;

/* The live world, in RAM: the per-room snapshots plus global (non-room-swapped)
   state. The fatdoors are one global array tagged by area — they never pass
   through world_leave/enter's swapping, so they get their own section, mirrored
   on every leave. */
typedef struct {
    RoomState rooms[WORLD_NUM_ROOMS];
    FatDoor   fatdoors[MAX_FATDOORS];
    int       fatdoor_count;
    Tentacle  tentacles[MAX_TENTACLES];   /* also global + area-tagged, like fatdoors */
    int       tentacle_count;
    Spider    spiders[MAX_SPIDERS];       /* likewise: one global area-tagged array */
    int       spider_count;
    Rabisu    rabisus[MAX_RABISUS];       /* the boss: same global area-tagged model */
    int       rabisu_count;
    Mushroom  mushrooms[MAX_MUSHROOMS];   /* ditto: the garden's pacing ambusher */
    int       mushroom_count;
} WorldState;

/* The delta's bit widths must cover the live arrays. Raise the WD_MAX_* in
   world.h (and widen the matching field) if one of these fires. */
_Static_assert(MAX_ZOMBIES       <= WD_MAX_PER_ROOM, "zombies_dead too narrow");
_Static_assert(MAX_DEMON_DOGS    <= WD_MAX_PER_ROOM, "dogs_dead too narrow");
_Static_assert(MAX_SML_MEDS      <= WD_MAX_PER_ROOM, "meds_gone too narrow");
_Static_assert(MAX_ITEM_PICKUPS  <= WD_MAX_PER_ROOM, "items_gone too narrow");
_Static_assert(MAX_CRATES        <= WD_MAX_CRATES,   "crates_smashed too narrow");
_Static_assert(MAX_KEYS          <= WD_MAX_KEYS,     "keys_gone too narrow");
_Static_assert(MAX_FATDOORS      <= WD_MAX_FATDOORS, "fatdoor_health too short");
_Static_assert(MAX_TENTACLES     <= WD_MAX_TENTACLES,"tentacle_health too short");
_Static_assert(MAX_SPIDERS       <= WD_MAX_SPIDERS,  "spiders_dead too narrow");
_Static_assert(MAX_RABISUS       <= WD_MAX_RABISUS,  "rabisus_dead too narrow");
_Static_assert(MAX_MUSHROOMS     <= WD_MAX_MUSHROOMS,"mushrooms_dead too narrow");
/* The `visited` bitmap is what caps the room count — one bit per room. It was a
   uint16_t and the sixteenth room filled it exactly; Maze One is the seventeenth
   and widened it to 32 bits. This assert is what makes the NEXT overflow a
   compile error instead of rooms silently coming back unvisited from a save. */
_Static_assert(WORLD_NUM_ROOMS <= 8 * sizeof(((WorldDelta *)0)->visited),
               "WorldDelta.visited too narrow for WORLD_NUM_ROOMS");

static WorldState world;

/* Live entity arrays (owned by their modules) — the "working set" for the
   room the player is currently in. */
extern DemonDog  demon_dogs[MAX_DEMON_DOGS];   extern int demon_dog_count;
extern Zombie    zombies[MAX_ZOMBIES];         extern int zombie_count;
extern Spider    spiders[MAX_SPIDERS];         extern int spider_count;
extern Rabisu    rabisus[MAX_RABISUS];         extern int rabisu_count;
extern Mushroom  mushrooms[MAX_MUSHROOMS];     extern int mushroom_count;
extern Crate     crates[MAX_CRATES];           extern int crate_count;
extern KeyPickup keys[MAX_KEYS];               extern int key_count;
extern SmlMed    sml_meds[MAX_SML_MEDS];       extern int sml_med_count;
extern ItemPickup item_pickups[MAX_ITEM_PICKUPS]; extern int item_pickup_count;
extern DoorState door_state;

/* room_index()'s inverse. The save rebuild walks the rooms in this order, which
   is what makes the global spider/rabisu arrays come back in a canonical layout
   no matter what route the player actually took through the house — see
   world_load_delta(). Keep the two tables in step. */
static const GameState room_areas[WORLD_NUM_ROOMS] = {
    STATE_DELIVERY_AREA,  STATE_KITCHEN_DINING, STATE_RECEPTION,
    STATE_PIANO_ROOM,     STATE_CONSERVATORY,   STATE_2F_HALL,
    STATE_MASTER_BEDROOM, STATE_EAST_HALL,      STATE_LIBRARY,
    STATE_EAST_STAIRWELL, STATE_ATTIC_STAIRWELL,STATE_ATTIC_EXIT,
    STATE_GARDEN_STAIRS,  STATE_GARDEN_COURTYARD, STATE_FOUNTAIN_SQUARE,
    STATE_OUTSIDE_CATACOMBS, STATE_MAZE_ONE,      STATE_MAZE_TWO,
};

static int room_index(GameState area) {
    switch (area) {
        case STATE_DELIVERY_AREA:  return 0;
        case STATE_KITCHEN_DINING: return 1;
        case STATE_RECEPTION:      return 2;
        case STATE_PIANO_ROOM:     return 3;
        case STATE_CONSERVATORY:   return 4;
        case STATE_2F_HALL:        return 5;
        case STATE_MASTER_BEDROOM: return 6;
        case STATE_EAST_HALL:      return 7;
        case STATE_LIBRARY:        return 8;
        case STATE_EAST_STAIRWELL: return 9;
        case STATE_ATTIC_STAIRWELL:return 10;
        case STATE_ATTIC_EXIT:     return 11;
        case STATE_GARDEN_STAIRS:  return 12;
        case STATE_GARDEN_COURTYARD: return 13;
        case STATE_FOUNTAIN_SQUARE: return 14;
        case STATE_OUTSIDE_CATACOMBS: return 15;
        case STATE_MAZE_ONE:          return 16;
        case STATE_MAZE_TWO:          return 17;
        default:                   return 0;
    }
}

/* live arrays -> room slot */
static void snapshot(RoomState *r) {
    memcpy(r->dogs,   demon_dogs, sizeof demon_dogs); r->dog_count   = demon_dog_count;
    memcpy(r->zombs,  zombies,    sizeof zombies);    r->zomb_count  = zombie_count;
    memcpy(r->crates, crates,     sizeof crates);     r->crate_count = crate_count;
    memcpy(r->keys,   keys,       sizeof keys);       r->key_count   = key_count;
    memcpy(r->meds,   sml_meds,   sizeof sml_meds);   r->med_count   = sml_med_count;
    memcpy(r->items,  item_pickups, sizeof item_pickups); r->item_count = item_pickup_count;
    r->door_state = door_state;
}

/* room slot -> live arrays */
static void restore(const RoomState *r) {
    memcpy(demon_dogs, r->dogs,   sizeof demon_dogs); demon_dog_count = r->dog_count;
    memcpy(zombies,    r->zombs,  sizeof zombies);    zombie_count    = r->zomb_count;
    memcpy(crates,     r->crates, sizeof crates);     crate_count     = r->crate_count;
    memcpy(keys,       r->keys,   sizeof keys);       key_count       = r->key_count;
    memcpy(sml_meds,   r->meds,   sizeof sml_meds);   sml_med_count   = r->med_count;
    memcpy(item_pickups, r->items, sizeof item_pickups); item_pickup_count = r->item_count;
    door_state = r->door_state;
}

/* Mirror the live global (non-room-swapped) arrays — fat doors, tentacles,
   spiders, the boss — into the blob's sections. */
static void snapshot_fatdoors(void) {
    memcpy(world.fatdoors, fatdoors, sizeof fatdoors);
    world.fatdoor_count = fatdoor_count;
    memcpy(world.tentacles, tentacles, sizeof tentacles);
    world.tentacle_count = tentacle_count;
    memcpy(world.spiders, spiders, sizeof spiders);
    world.spider_count = spider_count;
    memcpy(world.rabisus, rabisus, sizeof rabisus);
    world.rabisu_count = rabisu_count;
    memcpy(world.mushrooms, mushrooms, sizeof mushrooms);
    world.mushroom_count = mushroom_count;
}

void world_new_game(void) {
    memset(&world, 0, sizeof world);
    /* The starting room (delivery) already has its entities set up by the
       startup inits + reset_game, so capture that as its initial state. */
    int d = room_index(STATE_DELIVERY_AREA);
    snapshot(&world.rooms[d]);
    world.rooms[d].visited = 1;
    snapshot_fatdoors();
}

/* Cut every monster sound dead. Called the instant a room transition begins
   (door and stair alike), because the area update stops running from that frame
   on: a hardware-looped scuttle or writhe would otherwise keep sounding right
   through the transition and into the next room, and an in-flight groan or bark
   would ring out over it. The two looped sounds are latched inside their own
   modules, so they get dedicated helpers that clear the latch as well — see
   spiders_silence(). Player sounds (hurt, death, the gun) are deliberately left
   alone; only the monsters are silenced. */
void world_silence_monsters(void) {
    spiders_silence();          /* SFX_SPDR_WLK   — looped, latched */
    tentacles_silence();        /* SFX_TNTCL_WRTH — looped, latched */
    rafflesias_silence();       /* borrows BOTH of the above loops — see rafflesia.c */
    sound_stop(SFX_ZOMBIE);     /* groan, retriggered on an interval while alert */
    sound_stop(SFX_ZOMBIEDIE);
    sound_stop(SFX_DOGBARK);
    sound_stop(SFX_DOGDIE);
    sound_stop(SFX_SPIT);
    sound_stop(SFX_TNTCL_DIE);  /* also the spider's death cry (see spider.c) */
    sound_stop(SFX_HISS);       /* the Mushroom Head's scream — which is also
                                   its death cry (mushroom.c) */
}

void world_leave(GameState area) {
    /* Enemy wake/chase state never persists: leaving a room — and saving, which
       also snapshots via this function — puts every still-living enemy back at
       its spawn point, asleep, at full health. Deaths stick. */
    zombies_rest();
    spiders_rest();
    rabisus_rest();
    mushrooms_rest();
    demon_dogs_rest();
    /* The rafflesias go further than "rest": they come back at full health even
       if they were killed. They are the one enemy with no permanent death (see
       rafflesia.h), which is also why they have no WorldState section and no
       WorldDelta field — there is nothing about them worth saving. */
    rafflesias_rest();
    snapshot(&world.rooms[room_index(area)]);
    snapshot_fatdoors();
}

/* Build a room's entities exactly as they stand on a FIRST VISIT, into the live
   arrays. Called from world_enter() when the room has never been seen, and from
   world_load_delta() for every visited room while rebuilding a save.
 *
 * This runs for rooms that are NOT loaded — the save rebuild walks all fourteen
 * with only one room's geometry resident — so nothing in here may read the
 * current room's mesh or collision data. Every coordinate, floor height and
 * ceiling height must be AUTHORED here. That is why the spiders below use
 * spider_add_at() with an explicit ceiling rather than spider_add(), which
 * probes collision_ceiling_y().
 */
void world_seed_room(GameState area) {
    memset(demon_dogs, 0, sizeof demon_dogs); demon_dog_count = 0;
    memset(zombies,    0, sizeof zombies);    zombie_count    = 0;
    memset(crates,     0, sizeof crates);     crate_count     = 0;
    memset(keys,       0, sizeof keys);       key_count       = 0;
    memset(sml_meds,   0, sizeof sml_meds);   sml_med_count   = 0;
    memset(item_pickups, 0, sizeof item_pickups); item_pickup_count = 0;
    door_state = DOOR_LOCKED;

    /* The delivery area is the one room whose contents are placed by module
       inits (crates_init's table + demon_dogs_init's) rather than here, and
       the one room the player is already standing in when a new game starts.
       Restate that starting layout so a save rebuild can reproduce it — the
       crate-only variant, because crates_reset() would wipe the inventory
       the load has already restored. */
    if (area == STATE_DELIVERY_AREA) {
        crates_place_defaults();
        demon_dogs_reset();
    }

    /* Seed each room's resident enemies on first entry; they are then
       snapshotted on the next world_leave() and persist (deaths stick). */
    if (area == STATE_KITCHEN_DINING) {
        zombie_add(-309, -149, -1275);
        zombie_add(-892, -149,  -384);
        zombie_add(-2064, -149, -663);
        zombie_add(-2969, -149,  -13);
        zombie_add(-1997, -149,  830);

        /* Floating small medipac. sml_med_spawn adds SML_MED_FLOAT_Y, so
           this floats just above the floor at the requested spot. */
        sml_med_spawn(293, -149, -1399);
    }

    /* Reception: the Grave-olver gun and a box of Standard Rounds sit on top
       of the dresser (at x=580, z=958, rotated 90° so its long axis runs
       along Z), spaced 120 apart along that length and centred on it. */
    if (area == STATE_RECEPTION) {
        item_pickup_spawn(580, -149, 898,  PICKUP_GRAVEOLVER);
        item_pickup_spawn(580, -149, 1018, PICKUP_ROUNDS);
    }

    if (area == STATE_CONSERVATORY) {
        /* A zombie lurking in the small room behind the east fatdoor
           (room x[-838,-333], z[385,997]), with a small medipac deeper in
           the room past it (sml_med_spawn floats it above the floor). */
        zombie_add(-585, -149, 750);
        sml_med_spawn(-585, -149, 900);

        /* Three demon dogs spread across the west end of the hall: one near
           the south wall (z=-311), one just south of the west double door
           (z=356), and one between them set further back (west). Fields set
           directly (no add helper, same as demon_dogs_init); apply_ddog_height
           settles their y onto the floor. */
        static const int32_t dpos[3][2] = {
            { -1600, -250 },   /* near the south wall            */
            { -1750,  315 },   /* near the west double door      */
            { -1950,   40 },   /* between them, a bit further back */
        };
        int di;
        for (di = 0; di < 3; di++) {
            demon_dogs[di].x      = dpos[di][0];
            demon_dogs[di].y      = -149;
            demon_dogs[di].z      = dpos[di][1];
            demon_dogs[di].spawn_x = dpos[di][0];  /* rest back here on exit, */
            demon_dogs[di].spawn_y = -149;         /* not the delivery-dog     */
            demon_dogs[di].spawn_z = dpos[di][1];  /* defaults from init       */
            demon_dogs[di].health = DDOG_MAX_HEALTH;
            demon_dogs[di].state  = DDOG_DORMANT;
            demon_dogs[di].active = 1;
        }
        demon_dog_count = 3;
    }

    /* Master Bedroom: a box of Standard Rounds in the middle of the bed
       chamber (x[-500,500]) in front of the bed — the bed's front edge sits
       behind the z=-379 wall, so this sits clear of it in open floor. One
       cylinder's worth (GRAVEOLVER_CAPACITY), stated explicitly rather than
       left to the ROUNDS_PER_PICKUP default.
       Y hovers it just off the floor rather than at chest height like the
       reception pair: the room's floor is world y=0, item_pickup_spawn
       raises the anchor 50, and the sprite is drawn centred with a ~70-unit
       half-height plus an 18-unit bob — so a centre at -100 keeps its bottom
       edge between 12 and 48 above the floorboards through the whole bob. */
    if (area == STATE_MASTER_BEDROOM) {
        item_pickup_spawn_amount(0, -50, -200, PICKUP_ROUNDS,
                                 GRAVEOLVER_CAPACITY);
    }

    /* East Hall: three ceiling spiders — two spread along the main
       east-west hall and one in the south offshoot room, behind the
       connector's fat door.

       -520 is the ceiling east_hall_init hands to collision_set_ceiling_y,
       which is a FLAT room-wide value, so spider_add_at with the number
       stated here places them exactly where the collision probe used to
       (the offshoot's drawn ceiling is -517, close enough not to warrant a
       per-point value). Authoring it is what lets a save rebuild this room
       without its geometry loaded — keep the two in step if the room's
       ceiling ever moves. */
    if (area == STATE_EAST_HALL) {
        spider_add_at(2132,  354, -520, STATE_EAST_HALL); /* east end of hall */
        spider_add_at(1127,  252, -520, STATE_EAST_HALL); /* above connector  */
        spider_add_at( 598, -540, -520, STATE_EAST_HALL); /* south offshoot   */
    }

    /* Library: three ceiling spiders spread across the reading room — one
       mid-floor between the two bookcase dividers, one in the far south-west
       corner past them, and one at the south-east end. All three sit in the
       room proper (z < -349), so they take its -730 ceiling rather than the
       entrance vestibule's -500 — and -730 is exactly what library_init
       hands to collision_set_ceiling_y, so stating it here reproduces the
       old probe. See the East Hall note above on why it is authored. */
    if (area == STATE_LIBRARY) {
        spider_add_at( -909, -1249, -730, STATE_LIBRARY); /* between bookcases */
        spider_add_at(-1531, -1784, -730, STATE_LIBRARY); /* south-west corner */
        spider_add_at(   -9, -1832, -730, STATE_LIBRARY); /* south-east end    */

        /* One zombie in the northern strip, between the entrance and the
           first bookcase divider — the y=-149 body reference every other
           y=0-floor room uses; apply_zombie_height settles it onto the
           floorboards. */
        zombie_add(-839, -149, -621);

        /* Flame Rounds, six of them (one full cylinder), on the floor
           between the bookcases — directly under the first spider. Stated
           explicitly rather than left to the ROUNDS_PER_PICKUP default so
           the count survives any retune of that constant. y=-50 matches the
           master bedroom's floor-level pickup: item_pickup_spawn raises the
           anchor 50, putting the sprite just above this room's y=0 boards. */
        item_pickup_spawn_amount(-909, -50, -1249, PICKUP_FLAME_ROUNDS,
                                 GRAVEOLVER_CAPACITY);
    }

    /* East Stairwell: one full cylinder of Standard Rounds on the WEST
       landing's floor (the one you arrive on from the East Hall), out in
       open boards near the chain-link fence. Stated explicitly rather than
       left to the ROUNDS_PER_PICKUP default so the count survives any
       retune of that constant. y=-50 is the floor-level convention for a
       y=0 room: item_pickup_spawn adds IP_FLOAT_Y=50, putting the sprite
       just above the boards. The fence wall pushes the player back to
       x=-239, only 53 from this spot — well inside ITEM_PICKUP_RADIUS. */
    if (area == STATE_EAST_STAIRWELL) {
        item_pickup_spawn_amount(-186, -50, -7, PICKUP_ROUNDS,
                                 GRAVEOLVER_CAPACITY);

        /* Small medipac on the EAST landing (x>44), out in open boards.
           y=-149 is the same body reference the kitchen/conservatory
           medipacs use; sml_med_spawn adds SML_MED_FLOAT_Y=50, floating it
           just above this room's y=0 floor. */
        sml_med_spawn(289, -149, -139);
    }

    /* Attic Stairwell: the Piano Key and the Blue Key Stone, side by side at
       the altar in the west room. Both are flag items — puzzle inputs with
       nothing consuming them yet — so the amount is irrelevant; spawn_amount
       is used anyway so the ROUNDS_PER_PICKUP default never applies.

       x=-2280 sits ON the altar top: the altar is the con_tile block with
       footprint x[-2420,-2229] z[-543,-7], top surface y=-117, so both
       spawns are inside its footprint, a little east of its centre.
       item_pickup_spawn subtracts IP_FLOAT_Y=50, so each sprite hovers just
       above the altar's top face, 22 below the standing eye — well inside
       ITEM_PICKUP_HEIGHT. The 160 Z gap clears the sprites' IP_WORLD_HALF=70
       so they read as two objects side by side.

       Reaching them depends on the altar being collided as a PROP (radius
       75) rather than as room walls — see attic_stairwell_altar_collide.
       With the wall standoff of 195 the nearest the player could stand was
       x=-2033, 247 Manhattan away and so past ITEM_PICKUP_RADIUS (200); the
       prop radius brings that to x=-2153 and 127. Move these east or shrink
       that radius and they go out of reach again. */
    if (area == STATE_ATTIC_STAIRWELL) {
        item_pickup_spawn_amount(-2280, -117, -355, PICKUP_PIANO_KEY,      1);
        item_pickup_spawn_amount(-2280, -117, -195, PICKUP_BLUE_KEY_STONE, 1);
    }

    /* Garden Stairs: two boxes of rounds on the MIDDLE WEST landing (floor
       y=-600), the one you reach after two flights down. Both hug the
       shaft's west wall (x=-2140) at x=-1867, one toward each end of the
       landing, so they read as a pair without overlapping.

       y=-650 is the floor-level convention: item_pickup_spawn subtracts
       IP_FLOAT_Y=50, centring each sprite at -700 — 89 above the standing
       eye of -789 on this landing, well inside ITEM_PICKUP_HEIGHT (150) and
       far outside it from the landings 600 above and below.

       Reach: the room's wide GS_WALL_RADIUS (260) stops the player at
       x=-1880, only 13 from these, so both stay inside ITEM_PICKUP_RADIUS.
       Widen that standoff and they go out of reach. One full cylinder each,
       stated explicitly so a retune of ROUNDS_PER_PICKUP cannot change it. */
    if (area == STATE_GARDEN_STAIRS) {
        item_pickup_spawn_amount(-1867, -650, 1430, PICKUP_ROUNDS,
                                 GRAVEOLVER_CAPACITY);
        item_pickup_spawn_amount(-1867, -650, 1991, PICKUP_FLAME_ROUNDS,
                                 GRAVEOLVER_CAPACITY);

        /* Small medipac at the BOTTOM of the shaft — same west wall and z
           as the Standard Rounds above, but on the y=0 landing two flights
           further down, so the two never compete for a pickup. y=-149 is
           the body reference every y=0-floor room uses; sml_med_spawn
           lowers it 50, floating it just above the boards. */
        sml_med_spawn(-1867, -149, 1430);
    }

    if (area == STATE_2F_HALL) {
        /* One guarding the reception exit door (east wall), one standing in
           front of the trick drawers in the west room. */
        zombie_add(-50,   -149, -320);
        zombie_add(-3299, -149, -1120);
    }

    /* Garden Courtyard: the Rabisu, the game's first boss, hovering in the
       dead centre of the garden.

       (-290, 0) is the mesh's own centre — the courtyard spans x[-2580,2000]
       and z[-2000,2000], so the midpoint is west of the origin, not on it.
       That point falls inside the sunken lawn zone (x[-1722,1142],
       z[-857,2000]), whose surface is y=900; rabisu_add hovers the model's
       underside RBS_HOVER (1 m) above that, at y=800.

       900 is the floor SURFACE, exactly as garden_courtyard_floor_zones_init
       states it. Do NOT pre-subtract GROUND_FLOOR_Y — that constant turns a
       surface into a STANDING anchor and has no business in a hover height
       (see mistake 2 in tools/ADDING_AN_ENEMY.txt). */
    if (area == STATE_GARDEN_COURTYARD) {
        rabisu_add(-290, 900, 0, STATE_GARDEN_COURTYARD);

        /* Two small medipacs for the boss fight, one on the west perimeter
           walk and one on the east, so whichever side the sweep drives the
           player to there is a heal within reach.

           Both sit on the RAISED WALK, not the lawn: x=-2229 is west of the
           -1722 lip and x=1654 is east of the +1142 one, which puts them in
           the y=800 zones (garden_courtyard_floor_zones_init). Hence 651,
           not the 751 a lawn placement would take — y=800 floor minus the
           149 body reference every other sml_med_spawn call passes, which
           spawn then lowers 50 to float them just above the paving. */
        sml_med_spawn(-2229, 651, 1442);
        sml_med_spawn( 1654, 651, 1475);
    }

    /* Fountain Square seeds NOTHING — no enemies, no pickups. That is not an
       oversight to be quietly corrected: it is what pays for the gate sound.
       SFX_GATE lives in the BOSS bank (there is no resident or house-bank room
       for it), so this room runs with the nine SND_BANK_HOUSE monster effects
       evicted, exactly as the Garden Courtyard does. Anything with a voice
       placed here would be silent. See src/sound.h before adding to this. */

    /* Outside Catacombs: ONE Mushroom Head, pacing the WESTERLY ROOM —
       x[-4396,-2498] z[-1449,1424], the big walled space off the west side of
       the map. It is reached through the one gap in its east wall, the
       z[-285,285] doorway at x=-2498 (collision walls 7 and 13 stop either side
       of it), by way of the corridor that runs west out of the central chamber.

       The patrol is a straight north-south run along the room's west wall:
           A  (-4039, -1055)      B  (-4039,  871)
       x=-4039 stands 357 clear of the west wall at -4396, and the two ends are
       394 and 553 clear of the south and north walls, so the whole leg is open
       floor and nothing on it is inside MSH_BODY_RADIUS of anything.

       That line runs BROADSIDE to the doorway, which is what makes the new
       three-part wake worth having: a player coming through at z~0 arrives
       square in the middle of the run, and whether they are seen depends on
       which way the mushroom happens to be walking when they do.

       y is the STANDING ANCHOR over this room's flat y=0 ground, -149 — the
       same value the rafflesias and every other y=0-floor placement here use.
       AUTHORED rather than probed, because world_seed_room runs for rooms whose
       geometry is not loaded.

       >>> THIS ROOM USED TO HAVE TO STAY EMPTY, AND NO LONGER DOES. <<< The
       note that stood here said so because the room was then on SND_BANK_BOSS
       purely to reach SFX_GATE, with every monster effect evicted. It is on
       SND_BANK_GARDEN now (main.c's STATE_LOADING), which is where SFX_HISS
       lives — so the scream sounds, and so do the effects the mushroom borrows:
       SFX_TNTCL_DIE for its death (garden bank) and SFX_AXEHIT / SFX_HURT
       (resident). Check any FURTHER placement against src/sound.h the same way;
       the nine house-bank effects are still evicted here. */
    if (area == STATE_OUTSIDE_CATACOMBS) {
        mushroom_add(-4039, -1055,   /* A: south end of the west wall run */
                     -4039,   871,   /* B: north end of it                */
                     -149, STATE_OUTSIDE_CATACOMBS);
    }

    /* Maze One: TWO Mushroom Heads, on the two patrol lines the design asked
       for. Both are legal walks, checked the way the Catacombs' one was —
       sampling each line every 20 units against maze_one_mesh_collision.c:

         A (  668, 3942) -> B (6765, 3942)   the full-width northern run, 6097
             long, outside the maze's north hedge. Tightest clearance anywhere
             on it is 242 (against the x[4800,6900] z=3700 run), so the whole
             leg clears MSH_BODY_RADIUS (90) nearly threefold and even the
             player's own 195 wall radius. This one is the long patrol: at
             MSH_WALK_SPEED it takes about 17 s a leg, and it runs BROADSIDE to
             most of the maze's northern exits, so which way it happens to be
             facing decides whether a player stepping out is noticed (see the
             three-part wake in mushroom.h).
         A ( 3825,  418) -> B (5279,  418)   a short 1454-long east-west leg
             inside the south-east of the maze, in a pocket 325 to 682 clear of
             the hedges either side.

       Neither line comes within MSH_SEP_RADIUS of the other or of any flower,
       and both are inside the region reachable from the gate.

       y is the STANDING ANCHOR over this room's flat y=0 ground, -149, the same
       value the rafflesias here take. AUTHORED, not probed — world_seed_room
       runs for rooms whose geometry is not resident.

       Sound: this room is on SND_BANK_GARDEN (main.c's STATE_LOADING), so
       SFX_HISS is loaded and the scream sounds, as do SFX_TNTCL_DIE for the
       death and the resident SFX_AXEHIT / SFX_HURT. */
    if (area == STATE_MAZE_ONE) {
        mushroom_add( 668, 3942, 6765, 3942, -149, STATE_MAZE_ONE);
        mushroom_add(3825,  418, 5279,  418, -149, STATE_MAZE_ONE);
    }

    /* Maze Two seeds NOTHING, deliberately. The room is in as geometry only, the
       way Maze One went in before it was populated a commit later. Its three
       poison_flower_base beds are art — rafflesia.c places the Catacombs' three
       and Maze One's five by name and this room is in neither list — and no
       mushroom walks it. It IS on SND_BANK_GARDEN (main.c's STATE_LOADING), so
       whatever goes here later can reach SFX_HISS and the flowers' loops; check
       any placement against src/sound.h the way Maze One's was. */
}

void world_enter(GameState area) {
    RoomState *r = &world.rooms[room_index(area)];
    if (r->visited) {
        restore(r);
    } else {
        /* First visit: build the room's residents from the seed above. They are
           snapshotted on the next world_leave() and persist (deaths stick). */
        world_seed_room(area);
        r->visited = 1;
    }
}

/* ---- Save delta ------------------------------------------------------------
   See world.h for what this stores and why it can be so much smaller than the
   world it describes.

   ORDERING INVARIANT (both halves depend on it): a room's pickup slots are
   filled by world_seed_room() FIRST and by crate drops SECOND. The *_gone bit
   masks are keyed by slot and describe only the seeded pickups; crate drops are
   keyed by CRATE index instead (crate_drops_gone), because a crate drop lands in
   whichever slot happens to be free and so has no stable slot of its own. No
   room today both seeds pickups and holds crates — Delivery is the only room
   with crates and it seeds none — and if one ever does, the two schemes would
   have to be reconciled rather than simply layered.
   ------------------------------------------------------------------------- */

/* Is a crate's dropped pickup still lying on the floor? Matched on XZ, which is
   the crate's own position: both key_spawn and sml_med_spawn copy x and z
   through untouched and only shift Y. */
static int crate_drop_present(const RoomState *r, const Crate *c) {
    int i;
    switch (c->item) {
        case ITEM_MEDIPAC:
            for (i = 0; i < MAX_SML_MEDS; i++)
                if (r->meds[i].active && r->meds[i].x == c->x && r->meds[i].z == c->z)
                    return 1;
            return 0;
        case ITEM_KEY:
            for (i = 0; i < MAX_KEYS; i++)
                if (r->keys[i].active && r->keys[i].x == c->x && r->keys[i].z == c->z)
                    return 1;
            return 0;
        default:
            return 0;   /* nothing to drop: never "still present" */
    }
}

/* Canonical index of the global array's entry `i`, i.e. where the rebuild in
   world_load_delta() will put it. The global spider/rabisu arrays are appended
   to as rooms are first entered, so a raw index means different things in two
   playthroughs that took different routes; ordering by (room, then position
   within that room) makes it stable. `areas` is the per-entry area tag. */
static int canonical_index(const GameState *areas, int count, int i) {
    int rank = 0, j;
    int my_room = room_index(areas[i]);
    for (j = 0; j < count; j++) {
        int room = room_index(areas[j]);
        if (room < my_room)  rank++;          /* whole earlier rooms first   */
        else if (room == my_room && j < i) rank++;  /* then order within it  */
    }
    return rank;
}

void world_save_delta(WorldDelta *d) {
    int r, i;

    memset(d, 0, sizeof *d);

    for (r = 0; r < WORLD_NUM_ROOMS; r++) {
        const RoomState *rs = &world.rooms[r];
        RoomDelta       *rd = &d->rooms[r];
        if (!rs->visited) continue;
        d->visited |= (uint32_t)(1u << r);

        for (i = 0; i < MAX_ZOMBIES; i++)
            if (rs->zombs[i].state == ZMB_DEAD)
                rd->zombies_dead |= (uint8_t)(1u << i);
        for (i = 0; i < MAX_DEMON_DOGS; i++)
            if (rs->dogs[i].state == DDOG_DEAD)
                rd->dogs_dead |= (uint8_t)(1u << i);

        for (i = 0; i < MAX_CRATES; i++) {
            if (!rs->crates[i].active || rs->crates[i].state != CRATE_SMASHED)
                continue;
            rd->crates_smashed |= (uint16_t)(1u << i);
            if (!crate_drop_present(rs, &rs->crates[i]))
                rd->crate_drops_gone |= (uint16_t)(1u << i);
        }

        /* An inactive slot is a pickup that was collected (or was never seeded,
           which the rebuild treats the same way: deactivating an empty slot is
           a no-op, and it happens before any crate drop can claim it). */
        for (i = 0; i < MAX_KEYS; i++)
            if (!rs->keys[i].active)  rd->keys_gone  |= (uint8_t)(1u << i);
        for (i = 0; i < MAX_SML_MEDS; i++)
            if (!rs->meds[i].active)  rd->meds_gone  |= (uint8_t)(1u << i);
        for (i = 0; i < MAX_ITEM_PICKUPS; i++)
            if (!rs->items[i].active) rd->items_gone |= (uint8_t)(1u << i);

        rd->door_state = (uint8_t)rs->door_state;
    }

    /* Globals. Fat doors and tentacles are placed once at startup in a fixed
       order, so their array index IS stable and needs no canonical remap. */
    for (i = 0; i < MAX_FATDOORS; i++)
        d->fatdoor_health[i] = (world.fatdoors[i].state == FATDOOR_SMASHED)
                             ? 0 : (uint8_t)world.fatdoors[i].health;
    for (i = 0; i < MAX_TENTACLES; i++)
        d->tentacle_health[i] = (world.tentacles[i].health > 0)
                              ? (uint8_t)world.tentacles[i].health : 0;

    {
        GameState areas[MAX_SPIDERS];
        for (i = 0; i < world.spider_count; i++) areas[i] = world.spiders[i].area;
        for (i = 0; i < world.spider_count; i++)
            if (world.spiders[i].state == SPD_DEAD)
                d->spiders_dead |= (uint8_t)
                    (1u << canonical_index(areas, world.spider_count, i));
    }
    {
        GameState areas[MAX_RABISUS];
        for (i = 0; i < world.rabisu_count; i++) areas[i] = world.rabisus[i].area;
        for (i = 0; i < world.rabisu_count; i++)
            if (world.rabisus[i].dead)
                d->rabisus_dead |= (uint8_t)
                    (1u << canonical_index(areas, world.rabisu_count, i));
    }
    {
        GameState areas[MAX_MUSHROOMS];
        for (i = 0; i < world.mushroom_count; i++) areas[i] = world.mushrooms[i].area;
        for (i = 0; i < world.mushroom_count; i++)
            if (world.mushrooms[i].state == MSH_DEAD)
                d->mushrooms_dead |= (uint8_t)
                    (1u << canonical_index(areas, world.mushroom_count, i));
    }
}

void world_load_delta(const WorldDelta *d) {
    int r, i;

    memset(&world, 0, sizeof world);

    /* The global area-tagged arrays are rebuilt from scratch by the room walk
       below, so empty them first. These resets touch no player state (unlike
       crates_reset, which cascades into the inventory modules) — the load has
       already restored the inventory by the time this runs. */
    spiders_reset();
    rabisus_reset();
    mushrooms_reset();
    fatdoors_reset();
    tentacles_reset();
    /* Not rebuilt by the room walk (its placements are fixed at startup, like
       the tentacles'), and nothing about it is in the delta — a load simply puts
       every flower back. */
    rafflesias_reset();

    for (r = 0; r < WORLD_NUM_ROOMS; r++) {
        const RoomDelta *rd = &d->rooms[r];
        if (!(d->visited & (1u << r))) continue;

        /* Rebuild the room as its first visit left it, then subtract what the
           player did to it. Rooms are walked in room_index order so the global
           spider/rabisu arrays come back in canonical_index() order. */
        world_seed_room(room_areas[r]);

        for (i = 0; i < MAX_ZOMBIES; i++)
            if (rd->zombies_dead & (1u << i)) zombies[i].state = ZMB_DEAD;
        for (i = 0; i < MAX_DEMON_DOGS; i++)
            if (rd->dogs_dead & (1u << i)) demon_dogs[i].state = DDOG_DEAD;

        /* Collected seeded pickups, BEFORE the crate drops below — see the
           ordering invariant above. */
        for (i = 0; i < MAX_KEYS; i++)
            if (rd->keys_gone & (1u << i))  keys[i].active = 0;
        for (i = 0; i < MAX_SML_MEDS; i++)
            if (rd->meds_gone & (1u << i))  sml_meds[i].active = 0;
        for (i = 0; i < MAX_ITEM_PICKUPS; i++)
            if (rd->items_gone & (1u << i)) item_pickups[i].active = 0;

        for (i = 0; i < MAX_CRATES; i++) {
            if (!(rd->crates_smashed & (1u << i))) continue;
            crates[i].state = CRATE_SMASHED;
            if (!(rd->crate_drops_gone & (1u << i)))
                crate_drop_contents(&crates[i]);
        }

        door_state = (DoorState)rd->door_state;

        snapshot(&world.rooms[r]);
        world.rooms[r].visited = 1;
    }

    for (i = 0; i < fatdoor_count; i++) {
        fatdoors[i].health = (int32_t)d->fatdoor_health[i];
        fatdoors[i].state  = d->fatdoor_health[i] ? FATDOOR_INTACT : FATDOOR_SMASHED;
    }
    for (i = 0; i < tentacle_count; i++)
        tentacles[i].health = (int32_t)d->tentacle_health[i];

    /* The room walk placed these in canonical order, so the delta's bit i and
       array slot i now mean the same thing. */
    for (i = 0; i < spider_count; i++)
        if (d->spiders_dead & (1u << i)) spiders[i].state = SPD_DEAD;
    for (i = 0; i < rabisu_count; i++)
        if (d->rabisus_dead & (1u << i)) { rabisus[i].dead = 1; rabisus[i].dying = 1; }
    for (i = 0; i < mushroom_count; i++)
        if (d->mushrooms_dead & (1u << i)) mushrooms[i].state = MSH_DEAD;

    snapshot_fatdoors();
}
