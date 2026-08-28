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
void draw_loading_screen(RenderContext *ctx);

/* The game's title alone, at the title screen's own size and position, in an
   arbitrary colour. The opening sequence (src/intro.c) takes the title over at
   the moment New Game is confirmed and fades this out; the letter bitmaps and
   the layout constants live here, so it draws through this rather than
   duplicating them. */
void title_draw_logo(RenderContext *ctx, uint8_t r, uint8_t g, uint8_t b);

#endif
