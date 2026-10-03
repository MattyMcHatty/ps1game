#ifndef TITLE_H
#define TITLE_H

#include "render.h"

typedef enum {
    STATE_TITLE,
    STATE_DELIVERY_AREA,
    STATE_MENU,
    STATE_LOADING,
    STATE_KITCHEN_DINING,
    STATE_DOOR_ANIM,   /* RE-style door-opening transition; runs before STATE_LOADING */
    STATE_RECEPTION,
    STATE_SAVE_MENU,   /* memory-card save flow, opened at a save point */
    STATE_PIANO_ROOM,  /* keep new states at the end: saves store GameState values */
    STATE_CONSERVATORY,
    STATE_2F_HALL,     /* second-floor hall, up the conservatory stairs */
    STATE_STAIR_ANIM,  /* stair-climb transition between conservatory and 2F hall */
    STATE_MASTER_BEDROOM, /* master bedroom, off the 2F hall corridor */
    STATE_EAST_HALL,   /* east hall, through the double door on reception's 2F east wall */
    STATE_LIBRARY,     /* library, through the double door at the east hall's east end */
    STATE_EAST_STAIRWELL, /* caged stairwell passage: east hall's single door (west
                             landing) and the library's single door (east landing) */
    STATE_ATTIC_STAIRWELL, /* attic, up the stairs at the east landing's east wall */
    STATE_ATTIC_EXIT,  /* caged attic room north of the attic stairwell's west room */
    STATE_GARDEN_STAIRS, /* caged switchback stairway behind the attic exit's door */
    STATE_GARDEN_COURTYARD, /* walled garden at the foot of the garden stairs */
    STATE_INTRO,       /* opening sequence, between New Game and the delivery area */
    STATE_FOUNTAIN_SQUARE, /* hedge parterre north of the garden courtyard, through
                              the gate in its north hedge */
    STATE_OUTSIDE_CATACOMBS, /* the approach to the catacomb mouth, north of
                                Fountain Square through the gate in ITS north
                                hedge. Appended, not inserted: saves store raw
                                enum values. */
    STATE_MAZE_ONE,          /* the hedge maze east of Fountain Square, through
                                the gate in its EAST hedge. Appended too, for
                                the same reason. */
    STATE_MAZE_TWO,          /* the second hedge maze, north of Maze One through
                                the gate in ITS north hedge. Appended, not
                                inserted: saves store raw enum values. */
    STATE_REAR_GATE,         /* the walled rear lawn WEST of Fountain Square,
                                through the gate in its west hedge — the last of
                                that room's four to be connected. Appended, not
                                inserted, for the same reason. */
    STATE_WEST_CORRIDOR,     /* the upper-floor passage joining Reception's
                                north-west door to the door at the top of the
                                Rear Gate's ramp — the first link between the
                                house and the garden chain that does not go
                                through the front of the mansion. Appended, not
                                inserted, for the same reason. */
    STATE_STABLES,           /* the walled stable yard WEST of the Rear Gate,
                                through the gate in its west hedge — the first
                                room of the garden-west VRAM bank (the Greenhouse
                                behind it is the second). Appended, not
                                inserted: saves store raw enum values. */
    STATE_LIBRARY_DESTROYED, /* the Library after the keystones come back out of
                                the Attic Exit's door. Not a new place on the
                                map: from the moment FLAG_HADAD_TWO is set this
                                REPLACES STATE_LIBRARY behind the same two doors
                                (see src/library_destroyed.h). A separate room
                                rather than a mesh swap so each version keeps its
                                own record of what has been killed and taken.
                                Appended, not inserted, for the same reason. */
    STATE_KEYSTONE_MAZE,     /* the third hedge maze, EAST of Maze One through
                                the gate in that room's east hedge. Appended,
                                not inserted: saves store raw enum values. */
    STATE_GREENHOUSE,        /* the glasshouse WEST of the Stables, through the
                                greenhouse door in that room's west wall — the
                                second and last room of the garden-west VRAM
                                bank, and a dead end. Appended, not inserted:
                                saves store raw enum values. */
    STATE_CHAIN_ROOM,        /* the walled yard joining Maze Two's east gate to
                                the Keystone Maze's north gate — the first room
                                in the garden that is not itself a maze, and the
                                first that opened with BOTH its gates connected.
                                Appended, not inserted: saves store raw enum
                                values. */
    STATE_THE_HATCH,         /* the walled lawn EAST of the Keystone Maze,
                                through the gate in that room's east hedge — the
                                end of the garden's eastern line, and a dead end
                                like the Greenhouse. Appended, not inserted:
                                saves store raw enum values. */
    STATE_ASAG_ARENA,        /* ASAG'S ARENA, at the bottom of The Hatch's
                                1200-unit shaft. The game's second boss room and
                                the first that is reached by a one-way drop: the
                                only ways out are the exit in its south wall and
                                dying. That shape is what lets it take a VRAM
                                bank and an SPU bank of its own — see
                                src/asag_arena.h and
                                tools/ADDING_THE_ASAG_FIGHT.txt. Appended, not
                                inserted: saves store raw enum values. */
    STATE_CATACOMBS_ENTRY,   /* CHAPTER 3 BEGINS HERE. The first room of the
                                Catacombs, through the open doors in the Outside
                                Catacombs' facade — and the first room in the
                                game the player cannot walk back out of. That is
                                what lets the whole chapter take a VRAM bank, an
                                SPU bank AND the mansion's and the garden's
                                texture RAM: see src/area_bank.h, which is the new
                                thing here and is worth reading before adding a
                                second Catacombs room. Appended, not inserted:
                                saves store raw enum values. */
    STATE_UP_DOWN_MAZE,      /* The UP DOWN MAZE, through the inner door at the
                                east end of the Catacombs Entry's burial hall.
                                Chapter 3's second room and the first room in the
                                game that is a maze in TWO STOREYS over one
                                footprint — the gaps between the blocks are one
                                maze, the tops of the same blocks are another,
                                and the only way down is off an edge. See
                                src/up_down_maze.h. Appended, not inserted: saves
                                store raw enum values. */
    STATE_INCINERATOR_ROOM,  /* The INCINERATOR ROOM, through the south door on
                                the LOWER floor of the Up Down Maze. Chapter 3's
                                third room and, after the maze, a deliberately
                                plain one: an L-shaped hall, flat, single storey,
                                under a vault half the maze's height. See
                                src/incinerator_room.h. Appended, not inserted:
                                saves store raw enum values. */
    STATE_TOMB,              /* The TOMB, through the WEST door of the
                                Incinerator Room. Chapter 3's fourth room: one
                                square chamber 4200 on a side, flat and single
                                storey under the Incinerator Room's low vault,
                                holding nine free-standing blocks of burial
                                loculi on a 3x3 grid with 600-wide aisles between
                                them. See src/tomb.h. Appended, not inserted:
                                saves store raw enum values. */
    STATE_CATACOMB_WALK,     /* the transition INTO that room, and a transition
                                state rather than a place — the company
                                STATE_DOOR_ANIM and STATE_STAIR_ANIM keep. The
                                camera walks between the two open door leaves
                                and fades; src/catacomb_walk.h. Appended for the
                                same reason, though nothing ever saves in it. */
    STATE_ROOM_OF_ARMS,      /* The ROOM OF ARMS, through the WEST door of the
                                Tomb. Chapter 3's fifth room: one octagonal
                                chamber 2586 across, flat and single storey
                                under the same low vault, with a cobblestone
                                screen across its north-west third and a field
                                of grasping arms in the sealed pocket behind it
                                — seen through a gap in that screen and never
                                walked into. See src/room_of_arms.h. Appended,
                                not inserted: saves store raw enum values. */
    STATE_THE_PIT,           /* THE PIT, through the NORTH door of the Tomb — the
                                last of that room's three drawn doors. Chapter 3's
                                sixth room and the first two-height room in the
                                chapter that is not a maze: a shaft 4800 by 3900
                                with a gallery running round three sides at
                                y=-1000 and the pit floor 1000 below it at y=0.
                                The two do not connect yet; the player walks the
                                gallery and looks down. See src/the_pit.h.
                                Appended, not inserted: saves store raw enum
                                values. */
    STATE_NORTH_CHAMBER,     /* THE NORTH CHAMBER, through the NORTH door of The
                                Pit, behind the bars the ambush lifts. Chapter 3's
                                seventh room: a ground floor at y=0 with a gallery
                                at y=-1000 standing over it round three sides, a
                                platform on a bars cage in the middle, and a ramp
                                up the north wall joining the two. See
                                src/north_chamber.h. Appended, not inserted: saves
                                store raw enum values. */
    STATE_ROOM_OF_HEADS,     /* THE ROOM OF HEADS, through the WEST door of the
                                North Chamber, on its gallery. Chapter 3's eighth
                                room: the Room of Arms' octagon, flat under a
                                y=-800 vault, with four piles of heads standing
                                in it and one door. See src/room_of_heads.h.
                                Appended, not inserted: saves store raw enum
                                values. */
    STATE_CLEAVER_CORRIDOR,  /* THE CLEAVER CORRIDOR, at the top of the LADDER in
                                the North Chamber's east alcove. Chapter 3's ninth
                                room: one straight corridor, flat under a y=-800
                                vault, with the ladder's shaft at its west end.
                                See src/cleaver_corridor.h. Appended, not
                                inserted: saves store raw enum values. */
    STATE_LADDER_ANIM,       /* the climb between those two rooms, and a
                                transition state rather than a place — the company
                                STATE_STAIR_ANIM and STATE_CATACOMB_WALK keep. A
                                column of ladder tiles lurches past a fixed camera;
                                src/ladder_anim.h. Appended for the same reason,
                                though nothing ever saves in it. */
    STATE_CRUCIFIX_CORRIDOR, /* THE CRUCIFIX CORRIDOR, through the single door in
                                the north-east corner of the Up Down Maze's LOWER
                                storey. Chapter 3's tenth room: a cross laid flat
                                under a y=-800 vault, with a lit sconce in the
                                alcove at its head. See src/crucifix_corridor.h.
                                Appended, not inserted: saves store raw enum
                                values. */
    STATE_SLIDING_BARS_ROOM, /* THE SLIDING BARS ROOM, through the north door of
                                the Crucifix Corridor's cross arm. Chapter 3's
                                eleventh room: a square grid of stone blocks
                                under a y=-800 vault, some of its corridors
                                closed by bars. See src/sliding_bars_room.h.
                                Appended, not inserted: saves store raw enum
                                values. */
    STATE_ROOM_OF_LEGS,      /* THE ROOM OF LEGS, through the NORTH of the two
                                doors in the Sliding Bars Room's east wall.
                                Chapter 3's twelfth room: the Room of Arms'
                                octagon, flat under a y=-800 vault, with a
                                C-shaped pile of legs, a crib and one door. See
                                src/room_of_legs.h. Appended, not inserted:
                                saves store raw enum values. */
    STATE_MEAT_PLANT,        /* THE MEAT PLANT, through the SOUTH of the two
                                doors in the Sliding Bars Room's east wall.
                                Chapter 3's thirteenth room: a hall with four
                                alcoves, flat under a y=-800 vault, a C-shaped
                                mass of legs standing in the middle. See
                                src/meat_plant.h. Appended, not inserted: saves
                                store raw enum values. */
    STATE_ROOM_OF_BONES,     /* THE ROOM OF BONES, through the door at the back
                                of the Meat Plant's west alcove. Chapter 3's
                                fourteenth room: the Room of Arms' octagon,
                                flat under a y=-800 vault, a mound of bones in
                                the middle, a crib and one door. See
                                src/room_of_bones.h. Appended, not inserted:
                                saves store raw enum values. */
    STATE_CLEAVER_L,         /* CLEAVER L, through the door at the back of the
                                Meat Plant's south alcove. Chapter 3's fifteenth
                                room: an L of two 600-wide shafts, flat, four
                                slamming cleavers and one door wired. See
                                src/cleaver_l.h. Appended, not inserted: saves
                                store raw enum values. */
    STATE_ZIG_ZAG_TOMB,      /* THE ZIG ZAG TOMB, through the Crucifix
                                Corridor's south door. Chapter 3's sixteenth
                                room: a 4200 square of nine blocks whose gaps
                                are barred so the way through zig-zags; its east
                                door to Cleaver L is locked from this side. See
                                src/zig_zag_tomb.h. Appended, not inserted: saves
                                store raw enum values. */
    STATE_H_CORRIDOR,        /* THE H CORRIDOR, through the Zig Zag Tomb's
                                south door. Chapter 3's seventeenth room: three
                                600-wide corridors in an "h", flat, one door
                                wired; a second door and a ladder are drawn and
                                sealed. See src/h_corridor.h. Appended, not
                                inserted: saves store raw enum values. */
    STATE_ROOM_OF_TORSOS,    /* THE ROOM OF TORSOS, through the H Corridor's
                                south door. Chapter 3's eighteenth room: the
                                Room of Arms' octagon with three pointed piles
                                of torsos, a crib in the north-west corner and
                                one door. See src/room_of_torsos.h. Appended,
                                not inserted: saves store raw enum values. */
    STATE_THE_SHELF,         /* THE SHELF, up the H Corridor's ladder and
                                through the Up Down Maze's south-upper door.
                                Chapter 3's nineteenth room: a long hall split
                                into three lanes by two rows of bars, with two
                                Lumberers and two Crawlers. See
                                src/the_shelf.h. Appended, not inserted: saves
                                store raw enum values. */
    STATE_GAOL_ENTRY,        /* THE GAOL ENTRY, through the Up Down Maze's
                                east-upper door. Chapter 3's twentieth room: an
                                empty box with a sealed gaol door in its east
                                wall, cells visible through its bars. See
                                src/gaol_entry.h. Appended, not inserted: saves
                                store raw enum values. */
} GameState;

/* The title screen's background. It is the framebuffer CLEAR colour, not a drawn
   tile — draw_title paints only the letters over it. Lives here rather than in
   main.c because the opening sequence fades this same colour down to black on
   its way out of the title (src/intro.c). */
#define TITLE_BG_R 25
#define TITLE_BG_G  0
#define TITLE_BG_B 29

extern GameState game_state;

/* The area the player is IN. Identical to game_state during ordinary play; the
   two differ while the inventory menu is up, when game_state is STATE_MENU and
   this still names the room behind it (main.c's STATE_MENU branch passes it to
   update_current_area and draw_current_area, because the room keeps running:
   enemies move, gravity applies, and anything in flight still lands).
   >>> EVERY "IS THIS THING IN THE PLAYER'S ROOM" TEST MUST USE THIS, NEVER
       game_state. <<< An entity gated on `area != game_state` freezes and
       vanishes the instant the menu opens, and a per-room table selected on
       game_state silently falls through to its default — which is how the boss
       fight used to pause and how zombies used to navigate the conservatory
       with the kitchen's zone tables. The rule holds one frame earlier than
       you would think, too: handle_menu_open sets this BEFORE switching
       game_state, so it is already correct on the frame Start is pressed.
   Gates on STATE_MENU itself (no camera, no firing, no HUD) are the separate,
   deliberate thing — those really do mean "is the menu up". */
extern GameState current_area;
extern GameState pending_area;   /* area STATE_LOADING will switch to once set up */

void title_init(void);
void update_title(void);
void draw_title(RenderContext *ctx);

/* Discard whatever is held on the pad when the title next runs, so buttons
   still down on the screen we came FROM cannot act here. main.c calls it on
   every route back to the title; see the definition in title.c. */
void title_input_arm(void);

void draw_loading_screen(RenderContext *ctx);

/* Read the crucifaxe icon the loading screen's animated axe is drawn with.
   >>> CALL THIS BEFORE THE FIRST draw_loading_screen. <<< It is the very first
   thing main() loads, ahead of the whole startup asset block, because that
   block is the longest freeze in the game and the axe is what tells the player
   it is a freeze with something happening behind it. Without it the loading
   screen simply draws no axe (it is not an error — the icon is a nice-to-have,
   not a dependency). */
void loading_screen_load_axe(void);

/* The game's title alone, at the title screen's own size and position, in an
   arbitrary colour. The opening sequence (src/intro.c) takes the title over at
   the moment New Game is confirmed and fades this out; the letter bitmaps and
   the layout constants live here, so it draws through this rather than
   duplicating them. */
void title_draw_logo(RenderContext *ctx, uint8_t r, uint8_t g, uint8_t b);

#endif
