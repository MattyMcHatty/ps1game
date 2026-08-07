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
#include "rabisu.h"
#include "savegame.h"
#include "sound.h"

#define WORLD_NUM_ROOMS 14  /* delivery_area, kitchen_dining, reception, piano_room,
                               conservatory, hall_2f, master_bedroom, east_hall,
                               library, east_stairwell, attic_stairwell,
                               attic_exit, garden_stairs, garden_courtyard */

/* A saved snapshot of one room's entities. Mirrors the live arrays below; this
   is the per-room unit a save file would store. */
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

/* Everything the save system persists in one contiguous blob: the per-room
   snapshots plus global (non-room-swapped) state. The fatdoors are one global
   array tagged by area — they never pass through world_leave/enter's swapping,
   so they get their own section, mirrored on every leave and restored wholesale
   on install. */
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
} WorldState;

/* The whole blob is written to the memory card frame by frame, so it must fit
   the block chain savegame.c lays out. Adding a room grows it by one RoomState
   (~2.5 KB) — if this fires, bump SAVE_WORLD_BLOCKS in savegame.h (each extra
   block costs one more memory-card block per save). */
_Static_assert(sizeof(WorldState) <= SAVE_WORLD_MAX_BYTES,
               "world blob outgrew the memory-card block chain: raise SAVE_WORLD_BLOCKS");

static WorldState world;

/* Live entity arrays (owned by their modules) — the "working set" for the
   room the player is currently in. */
extern DemonDog  demon_dogs[MAX_DEMON_DOGS];   extern int demon_dog_count;
extern Zombie    zombies[MAX_ZOMBIES];         extern int zombie_count;
extern Spider    spiders[MAX_SPIDERS];         extern int spider_count;
extern Rabisu    rabisus[MAX_RABISUS];         extern int rabisu_count;
extern Crate     crates[MAX_CRATES];           extern int crate_count;
extern KeyPickup keys[MAX_KEYS];               extern int key_count;
extern SmlMed    sml_meds[MAX_SML_MEDS];       extern int sml_med_count;
extern ItemPickup item_pickups[MAX_ITEM_PICKUPS]; extern int item_pickup_count;
extern DoorState door_state;

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

void *world_blob(void)      { return &world; }
int   world_blob_size(void) { return (int)sizeof world; }

void world_install(const void *blob) {
    memcpy(&world, blob, sizeof world);
    /* The fatdoor/tentacle/spider/rabisu sections are global (not room-swapped),
       so restore the live arrays immediately rather than waiting for a
       world_enter. */
    memcpy(fatdoors, world.fatdoors, sizeof fatdoors);
    fatdoor_count = world.fatdoor_count;
    memcpy(tentacles, world.tentacles, sizeof tentacles);
    tentacle_count = world.tentacle_count;
    memcpy(spiders, world.spiders, sizeof spiders);
    spider_count = world.spider_count;
    memcpy(rabisus, world.rabisus, sizeof rabisus);
    rabisu_count = world.rabisu_count;
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
    sound_stop(SFX_ZOMBIE);     /* groan, retriggered on an interval while alert */
    sound_stop(SFX_ZOMBIEDIE);
    sound_stop(SFX_DOGBARK);
    sound_stop(SFX_DOGDIE);
    sound_stop(SFX_SPIT);
    sound_stop(SFX_TNTCL_DIE);  /* also the spider's death cry (see spider.c) */
}

void world_leave(GameState area) {
    /* Enemy wake/chase state never persists: leaving a room — and saving, which
       also snapshots via this function — puts every still-living enemy back at
       its spawn point, asleep, at full health. Deaths stick. */
    zombies_rest();
    spiders_rest();
    rabisus_rest();
    demon_dogs_rest();
    snapshot(&world.rooms[room_index(area)]);
    snapshot_fatdoors();
}

void world_enter(GameState area) {
    RoomState *r = &world.rooms[room_index(area)];
    if (r->visited) {
        restore(r);
    } else {
        /* First visit: start empty. A room with its own enemies/crates/pickups
           would populate the live arrays here (e.g. call its *_init), then they
           get snapshotted on the next world_leave(). */
        memset(demon_dogs, 0, sizeof demon_dogs); demon_dog_count = 0;
        memset(zombies,    0, sizeof zombies);    zombie_count    = 0;
        memset(crates,     0, sizeof crates);     crate_count     = 0;
        memset(keys,       0, sizeof keys);       key_count       = 0;
        memset(sml_meds,   0, sizeof sml_meds);   sml_med_count   = 0;
        memset(item_pickups, 0, sizeof item_pickups); item_pickup_count = 0;
        door_state = DOOR_LOCKED;

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
           connector's fat door. spider_add reads the ceiling height from the
           room, which east_hall_init has already installed by the time
           world_enter runs (it states y=-520, the DRAWN ceiling; the offshoot's
           is -517, close enough not to warrant a per-point value). */
        if (area == STATE_EAST_HALL) {
            spider_add(2132,  354, STATE_EAST_HALL);   /* east end of the hall  */
            spider_add(1127,  252, STATE_EAST_HALL);   /* above the connector   */
            spider_add( 598, -540, STATE_EAST_HALL);   /* south offshoot room   */
        }

        /* Library: three ceiling spiders spread across the reading room — one
           mid-floor between the two bookcase dividers, one in the far south-west
           corner past them, and one at the south-east end. All three sit in the
           room proper (z < -349), so they take its -730 ceiling rather than the
           entrance vestibule's -500; library_init has already installed it by
           the time world_enter runs. */
        if (area == STATE_LIBRARY) {
            spider_add( -909, -1249, STATE_LIBRARY);   /* between the bookcases */
            spider_add(-1531, -1784, STATE_LIBRARY);   /* south-west corner     */
            spider_add(   -9, -1832, STATE_LIBRARY);   /* south-east end        */

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

        r->visited = 1;
    }
}
