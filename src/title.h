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
} GameState;

extern GameState game_state;
extern GameState current_area;   /* last playable area entered; the menu returns here */
extern GameState pending_area;   /* area STATE_LOADING will switch to once set up */

void title_init(void);
void update_title(void);
void draw_title(RenderContext *ctx);
void draw_loading_screen(RenderContext *ctx);

#endif
