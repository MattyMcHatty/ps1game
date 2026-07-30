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
} GameState;

extern GameState game_state;
extern GameState current_area;   /* last playable area entered; the menu returns here */
extern GameState pending_area;   /* area STATE_LOADING will switch to once set up */

void title_init(void);
void update_title(void);
void draw_title(RenderContext *ctx);
void draw_loading_screen(RenderContext *ctx);

#endif
